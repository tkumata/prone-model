#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "storage.h"
#include "web_service.h"

#define WIFI_CONNECTED_BIT BIT0

#define WIFI_WAIT_TIMEOUT_MS 30000

#define CAMERA_XCLK_FREQ_HZ 10000000
#define CAMERA_FRAME_SIZE FRAMESIZE_QVGA

#define SDMMC_CMD_GPIO 38
#define SDMMC_CLK_GPIO 39
#define SDMMC_D0_GPIO 40

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

static const char *TAG = "prone_collector";
static const storage_context_t STORAGE_CONTEXT = {
    .dataset_dir = DATASET_DIR,
    .images_dir = IMAGES_DIR,
    .metadata_path = METADATA_PATH,
    .board_name = BOARD_NAME,
};
static web_service_context_t WEB_SERVICE_CONTEXT = {0};

typedef struct {
    httpd_handle_t server;
    httpd_handle_t stream_server;
    EventGroupHandle_t wifi_event_group;
    SemaphoreHandle_t storage_mutex;
    SemaphoreHandle_t camera_mutex;
    SemaphoreHandle_t status_mutex;
    SemaphoreHandle_t runtime_mutex;
    collector_status_t status;
    app_runtime_t runtime;
    sdmmc_card_t *sdcard;
} app_state_t;

static app_state_t g_state = {
    .server = NULL,
    .stream_server = NULL,
    .wifi_event_group = NULL,
    .storage_mutex = NULL,
    .camera_mutex = NULL,
    .status_mutex = NULL,
    .runtime_mutex = NULL,
    .status = {
        .wifi = "disconnected",
        .camera = "error",
        .sdcard = "error",
        .sample_count = 0,
        .usable_count = 0,
        .excluded_count = 0,
        .last_capture_ms = 0,
    },
    .runtime = {
        .camera_ready = false,
        .sdcard_ready = false,
        .metadata_ready = false,
        .wifi_connected = false,
        .storage_resetting = false,
        .last_capture_id = 0,
    },
    .sdcard = NULL,
};

static esp_err_t start_web_server(void);
static esp_err_t start_stream_server(void);
static esp_err_t ensure_storage_ready(void);
static void refresh_dataset_counts_locked(void);
static void update_capture_status_locked(int is_usable_for_training, int64_t timestamp_ms);
static void copy_status_snapshot(collector_status_t *out_status);
static void copy_runtime_snapshot(app_runtime_t *out_runtime);
static void set_camera_ready(bool ready);
static void set_storage_ready(bool sdcard_ready, bool metadata_ready);
static void set_wifi_connected(bool connected);
static void set_storage_resetting(bool resetting);
static void set_last_capture_id(int64_t capture_id);
static void reset_status_counts(void);
static void generate_capture_id(char *capture_id, size_t capture_id_len, int64_t *timestamp_ms_out);
static void init_web_service_context(void);

static void set_status_text(char *target, size_t target_len, const char *value)
{
    snprintf(target, target_len, "%s", value);
}

static void lock_status(void)
{
    if (g_state.status_mutex != NULL) {
        xSemaphoreTake(g_state.status_mutex, portMAX_DELAY);
    }
}

static void unlock_status(void)
{
    if (g_state.status_mutex != NULL) {
        xSemaphoreGive(g_state.status_mutex);
    }
}

static void lock_runtime(void)
{
    if (g_state.runtime_mutex != NULL) {
        xSemaphoreTake(g_state.runtime_mutex, portMAX_DELAY);
    }
}

static void unlock_runtime(void)
{
    if (g_state.runtime_mutex != NULL) {
        xSemaphoreGive(g_state.runtime_mutex);
    }
}

static void update_capture_status_locked(int is_usable_for_training, int64_t timestamp_ms)
{
    lock_status();
    g_state.status.sample_count++;
    if (is_usable_for_training == 1) {
        g_state.status.usable_count++;
    } else {
        g_state.status.excluded_count++;
    }
    g_state.status.last_capture_ms = timestamp_ms;
    unlock_status();
}

static void copy_status_snapshot(collector_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    lock_status();
    *out_status = g_state.status;
    unlock_status();
}

static void copy_runtime_snapshot(app_runtime_t *out_runtime)
{
    if (out_runtime == NULL) {
        return;
    }

    lock_runtime();
    *out_runtime = g_state.runtime;
    unlock_runtime();
}

static void set_camera_ready(bool ready)
{
    lock_runtime();
    g_state.runtime.camera_ready = ready;
    unlock_runtime();
}

static void set_storage_ready(bool sdcard_ready, bool metadata_ready)
{
    lock_runtime();
    g_state.runtime.sdcard_ready = sdcard_ready;
    g_state.runtime.metadata_ready = metadata_ready;
    unlock_runtime();
}

static void set_wifi_connected(bool connected)
{
    lock_runtime();
    g_state.runtime.wifi_connected = connected;
    unlock_runtime();
}

static void set_storage_resetting(bool resetting)
{
    lock_runtime();
    g_state.runtime.storage_resetting = resetting;
    unlock_runtime();
}

static void set_last_capture_id(int64_t capture_id)
{
    lock_runtime();
    g_state.runtime.last_capture_id = capture_id;
    unlock_runtime();
}

static void reset_status_counts(void)
{
    lock_status();
    g_state.status.sample_count = 0;
    g_state.status.usable_count = 0;
    g_state.status.excluded_count = 0;
    g_state.status.last_capture_ms = 0;
    unlock_status();
}

static void generate_capture_id(char *capture_id, size_t capture_id_len, int64_t *timestamp_ms_out)
{
    int64_t capture_id_value = esp_timer_get_time();

    lock_runtime();
    if (capture_id_value <= g_state.runtime.last_capture_id) {
        capture_id_value = g_state.runtime.last_capture_id + 1;
    }
    g_state.runtime.last_capture_id = capture_id_value;
    unlock_runtime();

    snprintf(capture_id, capture_id_len, "%lld", (long long) capture_id_value);
    if (timestamp_ms_out != NULL) {
        *timestamp_ms_out = capture_id_value / 1000LL;
    }
}

static void init_web_service_context(void)
{
    WEB_SERVICE_CONTEXT.server = &g_state.server;
    WEB_SERVICE_CONTEXT.stream_server = &g_state.stream_server;
    WEB_SERVICE_CONTEXT.storage_mutex = g_state.storage_mutex;
    WEB_SERVICE_CONTEXT.camera_mutex = g_state.camera_mutex;
    WEB_SERVICE_CONTEXT.storage_context = &STORAGE_CONTEXT;
    WEB_SERVICE_CONTEXT.copy_status_snapshot = copy_status_snapshot;
    WEB_SERVICE_CONTEXT.copy_runtime_snapshot = copy_runtime_snapshot;
    WEB_SERVICE_CONTEXT.update_capture_status = update_capture_status_locked;
    WEB_SERVICE_CONTEXT.generate_capture_id = generate_capture_id;
    WEB_SERVICE_CONTEXT.set_storage_resetting = set_storage_resetting;
    WEB_SERVICE_CONTEXT.set_last_capture_id = set_last_capture_id;
    WEB_SERVICE_CONTEXT.set_storage_ready = set_storage_ready;
    WEB_SERVICE_CONTEXT.reset_status_counts = reset_status_counts;
}

static esp_err_t ensure_storage_ready(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_err_t err;

    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    slot_config.width = 1;
    slot_config.clk = SDMMC_CLK_GPIO;
    slot_config.cmd = SDMMC_CMD_GPIO;
    slot_config.d0 = SDMMC_D0_GPIO;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &g_state.sdcard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD カードのマウントに失敗しました: %s", esp_err_to_name(err));
        lock_status();
        set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");
        unlock_status();
        set_storage_ready(false, false);
        return err;
    }

    if (storage_ensure_ready(&STORAGE_CONTEXT) != ESP_OK) {
        ESP_LOGE(TAG, "データセットディレクトリの準備に失敗しました");
        lock_status();
        set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");
        unlock_status();
        set_storage_ready(false, false);
        return ESP_FAIL;
    }

    set_storage_ready(true, true);
    lock_status();
    set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "ok");
    unlock_status();

    refresh_dataset_counts_locked();

    return ESP_OK;
}

static void refresh_dataset_counts_locked(void)
{
    collector_status_t status_snapshot;
    int64_t latest_capture_id;

    copy_status_snapshot(&status_snapshot);
    storage_refresh_dataset_counts(&STORAGE_CONTEXT, &status_snapshot);
    latest_capture_id = storage_find_latest_capture_id(&STORAGE_CONTEXT);

    lock_status();
    g_state.status.sample_count = status_snapshot.sample_count;
    g_state.status.usable_count = status_snapshot.usable_count;
    g_state.status.excluded_count = status_snapshot.excluded_count;
    g_state.status.last_capture_ms = status_snapshot.last_capture_ms;
    unlock_status();

    set_last_capture_id(latest_capture_id);
}

static esp_err_t init_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .xclk_freq_hz = CAMERA_XCLK_FREQ_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAMERA_FRAME_SIZE,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        .fb_count = 1,
        .grab_mode = CAMERA_GRAB_LATEST,
        .fb_location = CAMERA_FB_IN_DRAM,
    };
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "カメラ初期化に失敗しました: %s", esp_err_to_name(err));
        set_camera_ready(false);
        lock_status();
        set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "error");
        unlock_status();
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_framesize(sensor, CAMERA_FRAME_SIZE);
        sensor->set_quality(sensor, CAMERA_JPEG_QUALITY);
    }

    set_camera_ready(true);
    lock_status();
    set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "ok");
    unlock_status();
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        set_wifi_connected(false);
        lock_status();
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
        unlock_status();
        xEventGroupClearBits(g_state.wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        set_wifi_connected(true);
        lock_status();
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "connected");
        unlock_status();
        xEventGroupSetBits(g_state.wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi 接続完了: " IPSTR, IP2STR(&event->ip_info.ip));
        if (g_state.server == NULL) {
            start_web_server();
        }
        if (g_state.stream_server == NULL) {
            start_stream_server();
        }
    }
}

static esp_err_t init_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};
    esp_err_t err;

    if (CONFIG_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "CONFIG_WIFI_SSID が未設定です");
        lock_status();
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
        unlock_status();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    snprintf((char *) wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_WIFI_SSID);
    snprintf((char *) wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONFIG_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    return ESP_OK;
}

static esp_err_t start_web_server(void)
{
    return web_service_start(&WEB_SERVICE_CONTEXT);
}

static esp_err_t start_stream_server(void)
{
    return web_service_start_stream(&WEB_SERVICE_CONTEXT);
}

static void collector_main_task(void *arg)
{
    esp_err_t err;

    g_state.wifi_event_group = xEventGroupCreate();
    g_state.storage_mutex = xSemaphoreCreateMutex();
    g_state.camera_mutex = xSemaphoreCreateMutex();
    g_state.status_mutex = xSemaphoreCreateMutex();
    g_state.runtime_mutex = xSemaphoreCreateMutex();
    if (g_state.wifi_event_group == NULL || g_state.storage_mutex == NULL ||
        g_state.camera_mutex == NULL || g_state.status_mutex == NULL ||
        g_state.runtime_mutex == NULL) {
        ESP_LOGE(TAG, "同期オブジェクトの生成に失敗しました");
        vTaskDelete(NULL);
        return;
    }
    init_web_service_context();
    lock_status();
    set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
    set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "error");
    set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");
    unlock_status();

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = init_camera();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "カメラ準備に失敗したため収集機能は制限されます");
    }

    err = ensure_storage_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ストレージ準備に失敗したため保存機能は制限されます");
    }

    err = init_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初期化に失敗しました: %s", esp_err_to_name(err));
        return;
    }

    xEventGroupWaitBits(g_state.wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdFALSE,
                        pdMS_TO_TICKS(WIFI_WAIT_TIMEOUT_MS));

    vTaskDelete(NULL);
}

void app_main(void)
{
    BaseType_t task_result = xTaskCreate(collector_main_task,
                                         "collector_main",
                                         COLLECTOR_MAIN_TASK_STACK_SIZE,
                                         NULL,
                                         tskIDLE_PRIORITY + 1,
                                         NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "collector_main_task の作成に失敗しました");
    }
}
