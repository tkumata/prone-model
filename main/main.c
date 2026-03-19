#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
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
#include "validation.h"
#include "web_ui.h"

#define WIFI_CONNECTED_BIT BIT0

#define WIFI_WAIT_TIMEOUT_MS 30000

#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY "\r\n--frame\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

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

typedef struct {
    bool camera_ready;
    bool sdcard_ready;
    bool metadata_ready;
    bool wifi_connected;
    bool storage_resetting;
    int64_t last_capture_id;
} app_runtime_t;

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
static void generate_capture_id(char *capture_id, size_t capture_id_len, int64_t *timestamp_ms_out);

static const char *HTTP_STATUS_400 = "400 Bad Request";
static const char *HTTP_STATUS_404 = "404 Not Found";
static const char *HTTP_STATUS_409 = "409 Conflict";
static const char *HTTP_STATUS_500 = "500 Internal Server Error";
static const char *HTTP_STATUS_503 = "503 Service Unavailable";

static const char *http_status_text(httpd_err_code_t status)
{
    switch (status) {
        case HTTPD_400_BAD_REQUEST:
            return HTTP_STATUS_400;
        case HTTPD_404_NOT_FOUND:
            return HTTP_STATUS_404;
        case HTTPD_500_INTERNAL_SERVER_ERROR:
            return HTTP_STATUS_500;
        default:
            return HTTP_STATUS_500;
    }
}

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

static esp_err_t send_json_error(httpd_req_t *req, httpd_err_code_t status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    char *body;

    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    cJSON_AddBoolToObject(root, "saved", false);
    cJSON_AddStringToObject(root, "error", message);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_status(req, http_status_text(status));
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_json_error_with_status(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    char *body;
    esp_err_t err;

    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    cJSON_AddBoolToObject(root, "saved", false);
    cJSON_AddStringToObject(root, "error", message);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_text_error_with_status(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t parse_request_json(httpd_req_t *req, cJSON **out_root)
{
    char body[MAX_HTTP_BODY_SIZE + 1];
    int received = 0;

    if (req->content_len <= 0 || req->content_len > MAX_HTTP_BODY_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    *out_root = cJSON_Parse(body);
    return *out_root == NULL ? ESP_FAIL : ESP_OK;
}

static esp_err_t json_get_string(cJSON *root, const char *key, char *out, size_t out_len, bool required)
{
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (node == NULL || cJSON_IsNull(node)) {
        if (required) {
            return ESP_ERR_NOT_FOUND;
        }
        out[0] = '\0';
        return ESP_OK;
    }
    if (!cJSON_IsString(node) || node->valuestring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, out_len, "%s", node->valuestring);
    return ESP_OK;
}

static esp_err_t json_get_int(cJSON *root, const char *key, int *out)
{
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (node == NULL || !cJSON_IsNumber(node)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = node->valueint;
    return ESP_OK;
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

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *content_type)
{
    FILE *file = fopen(path, "rb");
    char buffer[1024];
    size_t read_size;

    if (file == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    httpd_resp_set_type(req, content_type);
    while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_size) != ESP_OK) {
            fclose(file);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, web_ui_html(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    collector_status_t status_snapshot;
    cJSON *root = cJSON_CreateObject();
    char *body;
    esp_err_t err;

    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    copy_status_snapshot(&status_snapshot);
    cJSON_AddStringToObject(root, "wifi", status_snapshot.wifi);
    cJSON_AddStringToObject(root, "camera", status_snapshot.camera);
    cJSON_AddStringToObject(root, "sdcard", status_snapshot.sdcard);
    cJSON_AddNumberToObject(root, "sample_count", status_snapshot.sample_count);
    cJSON_AddNumberToObject(root, "usable_count", status_snapshot.usable_count);
    cJSON_AddNumberToObject(root, "excluded_count", status_snapshot.excluded_count);
    cJSON_AddNumberToObject(root, "last_capture_ms", (double) status_snapshot.last_capture_ms);

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;
    cJSON *root = NULL;
    capture_request_t request = {0};
    char reason[128];
    char capture_id[MAX_CAPTURE_ID_LEN];
    char image_path[MAX_IMAGE_PATH_LEN];
    cJSON *response = NULL;
    char *body = NULL;
    esp_err_t err;
    int64_t timestamp_ms;

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.camera_ready) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "カメラ異常のため撮影できません");
    }
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD カード異常のため保存できません");
    }
    if (runtime_snapshot.storage_resetting) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "リセット処理中のため保存できません");
    }

    err = parse_request_json(req, &root);
    if (err != ESP_OK) {
        return send_json_error(req, HTTPD_400_BAD_REQUEST, "JSON が不正です");
    }

    if (json_get_string(root, "subject_id", request.subject_id, sizeof(request.subject_id), true) != ESP_OK ||
        json_get_string(root, "session_id", request.session_id, sizeof(request.session_id), true) != ESP_OK ||
        json_get_string(root, "location_id", request.location_id, sizeof(request.location_id), true) != ESP_OK ||
        json_get_string(root, "lighting_id", request.lighting_id, sizeof(request.lighting_id), true) != ESP_OK ||
        json_get_string(root, "camera_position_id", request.camera_position_id, sizeof(request.camera_position_id), true) != ESP_OK ||
        json_get_string(root, "annotator_id", request.annotator_id, sizeof(request.annotator_id), true) != ESP_OK ||
        json_get_string(root, "exclude_reason", request.exclude_reason, sizeof(request.exclude_reason), false) != ESP_OK ||
        json_get_string(root, "notes", request.notes, sizeof(request.notes), false) != ESP_OK ||
        json_get_int(root, "label", &request.label) != ESP_OK ||
        json_get_int(root, "is_usable_for_training", &request.is_usable_for_training) != ESP_OK) {
        cJSON_Delete(root);
        return send_json_error(req, HTTPD_400_BAD_REQUEST, "必須メタデータが不足または不正です");
    }

    err = validate_capture_request(&request, reason, sizeof(reason));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_json_error(req, HTTPD_400_BAD_REQUEST, reason);
    }

    if (xSemaphoreTake(g_state.storage_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "保存処理が混雑しています");
    }

    generate_capture_id(capture_id, sizeof(capture_id), &timestamp_ms);
    err = storage_save_capture_locked(&STORAGE_CONTEXT,
                                      &request,
                                      runtime_snapshot.camera_ready,
                                      g_state.camera_mutex,
                                      update_capture_status_locked,
                                      timestamp_ms,
                                      capture_id,
                                      image_path,
                                      sizeof(image_path));
    xSemaphoreGive(g_state.storage_mutex);
    if (err != ESP_OK) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存に失敗しました");
    }

    response = cJSON_CreateObject();
    if (response == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }
    cJSON_AddBoolToObject(response, "saved", true);
    cJSON_AddStringToObject(response, "capture_id", capture_id);
    cJSON_AddStringToObject(response, "image_path", image_path);
    body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t metadata_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sdcard error");
    }
    if (runtime_snapshot.storage_resetting) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "busy");
    }
    return send_file(req, METADATA_PATH, "text/csv");
}

static esp_err_t image_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;
    char capture_id[MAX_CAPTURE_ID_LEN];
    char path[128];

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sdcard error");
    }
    if (runtime_snapshot.storage_resetting) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "busy");
    }

    if (httpd_req_get_url_query_len(req) <= 0 ||
        httpd_req_get_url_query_str(req, path, sizeof(path)) != ESP_OK ||
        httpd_query_key_value(path, "capture_id", capture_id, sizeof(capture_id)) != ESP_OK ||
        !is_digits_only(capture_id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "capture_id error");
    }

    snprintf(path, sizeof(path), IMAGES_DIR "/%s.jpg", capture_id);
    return send_file(req, path, "image/jpeg");
}

static esp_err_t manifest_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;
    char query[128];
    char page_buf[16];
    char page_size_buf[16];
    int page = 1;
    int page_size = DEFAULT_PAGE_SIZE;
    int start_index;
    int end_index;
    int current_index = 0;
    FILE *file;
    char line[MAX_CSV_LINE_LEN];
    collector_status_t status_snapshot;
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    char *body;
    esp_err_t err;

    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metadata error");
    }
    if (runtime_snapshot.storage_resetting) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return send_text_error_with_status(req, HTTP_STATUS_503, "busy");
    }

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "page", page_buf, sizeof(page_buf)) == ESP_OK) {
            page = atoi(page_buf);
        }
        if (httpd_query_key_value(query, "page_size", page_size_buf, sizeof(page_size_buf)) == ESP_OK) {
            page_size = atoi(page_size_buf);
        }
    }

    if (page < 1) {
        page = 1;
    }
    if (page_size < 1) {
        page_size = DEFAULT_PAGE_SIZE;
    }
    if (page_size > MAX_PAGE_SIZE) {
        page_size = MAX_PAGE_SIZE;
    }

    start_index = (page - 1) * page_size;
    end_index = start_index + page_size;

    file = fopen(METADATA_PATH, "r");
    if (file == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metadata error");
    }

    fgets(line, sizeof(line), file);
    while (fgets(line, sizeof(line), file) != NULL) {
        if (current_index >= start_index && current_index < end_index) {
            char *cursor = line;
            char capture_id[MAX_CAPTURE_ID_LEN] = {0};
            char timestamp_ms[32] = {0};
            char label[8] = {0};
            char image_path[MAX_IMAGE_PATH_LEN] = {0};
            char discard[MAX_NOTES_LEN];
            cJSON *item = cJSON_CreateObject();

            if (item == NULL) {
                fclose(file);
                cJSON_Delete(root);
                cJSON_Delete(items);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
            }

            storage_csv_read_field(&cursor, capture_id, sizeof(capture_id));
            storage_csv_read_field(&cursor, timestamp_ms, sizeof(timestamp_ms));
            for (int i = 0; i < 6; ++i) {
                storage_csv_read_field(&cursor, discard, sizeof(discard));
            }
            storage_csv_read_field(&cursor, label, sizeof(label));
            for (int i = 0; i < 4; ++i) {
                storage_csv_read_field(&cursor, discard, sizeof(discard));
            }
            storage_csv_read_field(&cursor, image_path, sizeof(image_path));

            cJSON_AddStringToObject(item, "capture_id", capture_id);
            cJSON_AddStringToObject(item, "image_path", image_path);
            cJSON_AddNumberToObject(item, "timestamp_ms", (double) strtoll(timestamp_ms, NULL, 10));
            cJSON_AddNumberToObject(item, "label", atoi(label));
            cJSON_AddItemToArray(items, item);
        }
        current_index++;
    }
    fclose(file);

    copy_status_snapshot(&status_snapshot);
    cJSON_AddNumberToObject(root, "total_samples", status_snapshot.sample_count);
    cJSON_AddNumberToObject(root, "page", page);
    cJSON_AddNumberToObject(root, "page_size", page_size);
    cJSON_AddBoolToObject(root, "has_next", current_index > end_index);
    cJSON_AddItemToObject(root, "items", items);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t reset_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;
    cJSON *root = NULL;
    char confirm[16];
    DIR *dir;
    struct dirent *entry;
    cJSON *response = NULL;
    char *body = NULL;
    esp_err_t err;

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD カード異常のためリセットできません");
    }
    if (runtime_snapshot.storage_resetting) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "リセット処理が混雑しています");
    }

    err = parse_request_json(req, &root);
    if (err != ESP_OK) {
        return send_json_error(req, HTTPD_400_BAD_REQUEST, "JSON が不正です");
    }
    if (json_get_string(root, "confirm", confirm, sizeof(confirm), true) != ESP_OK || strcmp(confirm, "RESET") != 0) {
        cJSON_Delete(root);
        return send_json_error_with_status(req, HTTP_STATUS_409, "confirm は RESET が必要です");
    }
    cJSON_Delete(root);

    set_storage_resetting(true);
    if (xSemaphoreTake(g_state.storage_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        set_storage_resetting(false);
        return send_json_error_with_status(req, HTTP_STATUS_503, "リセット処理が混雑しています");
    }

    dir = opendir(IMAGES_DIR);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            char file_path[MAX_FILE_PATH_LEN];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            snprintf(file_path, sizeof(file_path), IMAGES_DIR "/%s", entry->d_name);
            unlink(file_path);
        }
        closedir(dir);
    }

    unlink(METADATA_PATH);
    err = storage_ensure_ready(&STORAGE_CONTEXT);
    if (err == ESP_OK) {
        lock_status();
        g_state.status.sample_count = 0;
        g_state.status.usable_count = 0;
        g_state.status.excluded_count = 0;
        g_state.status.last_capture_ms = 0;
        unlock_status();
        set_last_capture_id(0);
        set_storage_ready(true, true);
    } else {
        set_storage_ready(false, false);
    }
    xSemaphoreGive(g_state.storage_mutex);
    set_storage_resetting(false);

    if (err != ESP_OK) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "CSV の再生成に失敗しました");
    }

    response = cJSON_CreateObject();
    if (response == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }
    cJSON_AddBoolToObject(response, "reset", true);
    body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    app_runtime_t runtime_snapshot;
    char part_buf[64];
    camera_fb_t *fb = NULL;
    esp_err_t err = ESP_OK;

    copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.camera_ready) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "camera error");
    }

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    while (true) {
        if (xSemaphoreTake(g_state.camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        fb = esp_camera_fb_get();
        xSemaphoreGive(g_state.camera_mutex);
        if (fb == NULL) {
            err = ESP_FAIL;
            break;
        }

        if ((err = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY))) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        int header_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        if ((err = httpd_resp_send_chunk(req, part_buf, header_len)) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        err = httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (err != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }

    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
    return err;
}

static esp_err_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL};
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    httpd_uri_t uri_capture = {.uri = "/api/capture", .method = HTTP_POST, .handler = capture_handler, .user_ctx = NULL};
    httpd_uri_t uri_manifest = {.uri = "/api/export/manifest", .method = HTTP_GET, .handler = manifest_handler, .user_ctx = NULL};
    httpd_uri_t uri_metadata = {.uri = "/api/export/metadata", .method = HTTP_GET, .handler = metadata_handler, .user_ctx = NULL};
    httpd_uri_t uri_image = {.uri = "/api/export/image", .method = HTTP_GET, .handler = image_handler, .user_ctx = NULL};
    httpd_uri_t uri_reset = {.uri = "/api/reset", .method = HTTP_POST, .handler = reset_handler, .user_ctx = NULL};
    esp_err_t err;

    config.max_uri_handlers = 12;
    config.server_port = 80;
    config.stack_size = 12288;
    config.max_open_sockets = 4;
    config.backlog_conn = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    err = httpd_start(&g_state.server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web サーバ起動に失敗しました: %s", esp_err_to_name(err));
        g_state.server = NULL;
        return err;
    }

    httpd_register_uri_handler(g_state.server, &uri_root);
    httpd_register_uri_handler(g_state.server, &uri_status);
    httpd_register_uri_handler(g_state.server, &uri_capture);
    httpd_register_uri_handler(g_state.server, &uri_manifest);
    httpd_register_uri_handler(g_state.server, &uri_metadata);
    httpd_register_uri_handler(g_state.server, &uri_image);
    httpd_register_uri_handler(g_state.server, &uri_reset);

    ESP_LOGI(TAG, "Web サーバを起動しました");
    return ESP_OK;
}

static esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uri_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
    esp_err_t err;

    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    config.max_open_sockets = 1;
    config.backlog_conn = 1;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    err = httpd_start(&g_state.stream_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ストリームサーバ起動に失敗しました: %s", esp_err_to_name(err));
        g_state.stream_server = NULL;
        return err;
    }

    httpd_register_uri_handler(g_state.stream_server, &uri_stream);
    ESP_LOGI(TAG, "ストリームサーバを起動しました");
    return ESP_OK;
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
