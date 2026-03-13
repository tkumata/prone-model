#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "prone_inference_bridge.h"
#include "sdmmc_cmd.h"

#if !defined(CONFIG_WIFI_SSID) || !defined(CONFIG_WIFI_PASSWORD)
#error "WiFi credentials must be configured via menuconfig"
#endif

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD

#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_INTERVAL_MS 5000

#define STREAM_WIDTH 320
#define STREAM_HEIGHT 240
#define INFERENCE_INTERVAL_MS 400
#define STREAM_PACING_DELAY_MS 5
#define STATUS_POLL_INTERVAL_MS 1500
#define INFERENCE_JPEG_MAX_BYTES (96 * 1024)
#define RESET_CONFIRM_TOKEN "RESET"

#define CSV_MOUNT_POINT "/sdcard"
#define CSV_FILE_PATH CSV_MOUNT_POINT "/prone_samples.csv"

// Freenove ESP32-S3 WROOM CAM
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D7 16
#define CAM_PIN_D6 17
#define CAM_PIN_D5 18
#define CAM_PIN_D4 12
#define CAM_PIN_D3 10
#define CAM_PIN_D2 8
#define CAM_PIN_D1 9
#define CAM_PIN_D0 11
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

// Freenove ESP32-S3 WROOM CAM microSD
#define SD_PIN_CLK 39
#define SD_PIN_CMD 38
#define SD_PIN_D0 40

static const char *TAG = "prone_collector";

typedef struct {
    int64_t timestamp_ms;
    bool face_detected;
    float face_score;
    int face_count;
    prone_face_box_t box;
} latest_detection_t;

static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_http_server;
static httpd_handle_t s_stream_http_server;
static esp_timer_handle_t s_wifi_retry_timer;
static SemaphoreHandle_t s_detection_mutex;
static SemaphoreHandle_t s_inference_frame_mutex;
static SemaphoreHandle_t s_csv_mutex;
static TaskHandle_t s_inference_task_handle;
static uint8_t *s_inference_frame_buf;
static size_t s_inference_frame_len;
static bool s_inference_frame_pending;
static bool s_wifi_connected;
static bool s_camera_ready;
static bool s_sd_ready;
static int64_t s_last_wifi_retry_ms;
static int64_t s_last_inference_publish_ms;
static latest_detection_t s_latest_detection;
static sdmmc_card_t *s_sd_card;

static void wifi_retry_timer_cb(void *arg);
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t init_nvs(void);
static esp_err_t start_wifi_sta(void);
static esp_err_t init_camera(void);
static esp_err_t init_sd_card(void);
static esp_err_t ensure_csv_header(void);
static esp_err_t start_http_server(void);
static esp_err_t start_stream_http_server(void);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t stream_get_handler(httpd_req_t *req);
static esp_err_t status_get_handler(httpd_req_t *req);
static esp_err_t sample_post_handler(httpd_req_t *req);
static esp_err_t export_get_handler(httpd_req_t *req);
static esp_err_t reset_post_handler(httpd_req_t *req);
static bool publish_frame_for_inference(camera_fb_t *fb);
static void inference_task(void *arg);
static esp_err_t start_inference_task(void);
static latest_detection_t get_latest_detection_copy(void);
static esp_err_t read_request_body(httpd_req_t *req, char *body, size_t body_size);
static esp_err_t parse_sample_request(httpd_req_t *req, int *label, char *session_id, size_t session_id_size);
static esp_err_t parse_reset_request(httpd_req_t *req, bool *confirmed);
static esp_err_t append_sample_csv(const latest_detection_t *detection, int label, const char *session_id);
static const char *csv_error_to_message(esp_err_t err);
static bool normalize_landmark_pair(const prone_face_box_t *box, int landmark_index, float *out_x, float *out_y);
static float normalize_bbox_x(int value);
static float normalize_bbox_y(int value);

static latest_detection_t get_latest_detection_copy(void)
{
    latest_detection_t copy = {0};
    if (s_detection_mutex == NULL) {
        return copy;
    }

    if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = s_latest_detection;
        xSemaphoreGive(s_detection_mutex);
    }
    return copy;
}

static void set_latest_detection(bool face_detected, float face_score, int face_count, const prone_face_box_t *box)
{
    if (s_detection_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_detection_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    s_latest_detection.timestamp_ms = esp_timer_get_time() / 1000;
    s_latest_detection.face_detected = face_detected;
    s_latest_detection.face_score = face_score;
    s_latest_detection.face_count = face_count;
    if (box != NULL) {
        s_latest_detection.box = *box;
    } else {
        memset(&s_latest_detection.box, 0, sizeof(s_latest_detection.box));
        s_latest_detection.box.x0 = -1;
        s_latest_detection.box.y0 = -1;
        s_latest_detection.box.x1 = -1;
        s_latest_detection.box.y1 = -1;
        s_latest_detection.box.valid = false;
        s_latest_detection.box.landmarks_valid = false;
    }

    xSemaphoreGive(s_detection_mutex);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void wifi_retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_wifi_connected) {
        return;
    }

    s_last_wifi_retry_ms = esp_timer_get_time() / 1000;
    ESP_LOGW(TAG, "Wi-Fi 再接続試行");
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        s_wifi_connected = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        int64_t elapsed_ms = now_ms - s_last_wifi_retry_ms;
        if (elapsed_ms >= WIFI_RETRY_INTERVAL_MS) {
            wifi_retry_timer_cb(NULL);
            return;
        }

        int64_t wait_ms = WIFI_RETRY_INTERVAL_MS - elapsed_ms;
        esp_err_t stop_err = esp_timer_stop(s_wifi_retry_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(stop_err);
        }
        ESP_ERROR_CHECK(esp_timer_start_once(s_wifi_retry_timer, wait_ms * 1000));
        ESP_LOGW(TAG, "Wi-Fi 切断。%lld ms 後に再接続", (long long)wait_ms);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG,
                 "Wi-Fi 接続完了 ip=" IPSTR " root=http://" IPSTR "/ stream=http://" IPSTR ":81/stream",
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t start_wifi_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    const esp_timer_create_args_t timer_args = {
        .callback = wifi_retry_timer_cb,
        .name = "wifi_retry_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_wifi_retry_timer));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static esp_err_t init_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 10,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        s_camera_ready = false;
        ESP_LOGE(TAG, "カメラ初期化失敗: %s", esp_err_to_name(err));
        return err;
    }

    s_camera_ready = true;
    ESP_LOGI(TAG, "カメラ初期化完了");
    return ESP_OK;
}

static esp_err_t ensure_csv_header(void)
{
    if (!s_sd_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    struct stat st;
    if (stat(CSV_FILE_PATH, &st) == 0 && st.st_size > 0) {
        return ESP_OK;
    }

    FILE *fp = fopen(CSV_FILE_PATH, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "CSV ヘッダ用ファイルを開けません: path=%s errno=%d", CSV_FILE_PATH, errno);
        return ESP_FAIL;
    }

    static const char header[] =
        "timestamp,session_id,label,face_detected,face_score,face_count,bbox_x,bbox_y,bbox_w,bbox_h,"
        "left_eye_x,left_eye_y,left_eye_visible,right_eye_x,right_eye_y,right_eye_visible,"
        "nose_x,nose_y,nose_visible,left_mouth_x,left_mouth_y,left_mouth_visible,"
        "right_mouth_x,right_mouth_y,right_mouth_visible\n";
    size_t written = fwrite(header, 1, strlen(header), fp);
    int flush_result = fflush(fp);
    int close_result = fclose(fp);
    if (written != strlen(header) || flush_result != 0 || close_result != 0) {
        ESP_LOGE(TAG,
                 "CSV ヘッダ書き込み失敗: written=%u expected=%u flush=%d close=%d errno=%d",
                 (unsigned)written,
                 (unsigned)strlen(header),
                 flush_result,
                 close_result,
                 errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t init_sd_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(CSV_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        s_sd_ready = false;
        ESP_LOGE(TAG, "SD カード初期化失敗: %s", esp_err_to_name(err));
        return err;
    }

    s_sd_ready = true;
    ESP_LOGI(TAG, "SD カード初期化完了");
    return ensure_csv_header();
}

static float normalize_bbox_x(int value)
{
    return (float)value / (float)STREAM_WIDTH;
}

static float normalize_bbox_y(int value)
{
    return (float)value / (float)STREAM_HEIGHT;
}

static bool normalize_landmark_pair(const prone_face_box_t *box, int landmark_index, float *out_x, float *out_y)
{
    if (box == NULL || out_x == NULL || out_y == NULL) {
        return false;
    }

    int width = box->x1 - box->x0;
    int height = box->y1 - box->y0;
    if (!box->landmarks_valid || !box->valid || width <= 0 || height <= 0) {
        *out_x = -1.0f;
        *out_y = -1.0f;
        return false;
    }

    int x = box->landmarks[landmark_index * 2];
    int y = box->landmarks[landmark_index * 2 + 1];
    *out_x = (float)(x - box->x0) / (float)width;
    *out_y = (float)(y - box->y0) / (float)height;
    return true;
}

static esp_err_t append_sample_csv(const latest_detection_t *detection, int label, const char *session_id)
{
    if (!s_sd_ready || detection == NULL || session_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "CSV ロック取得失敗");
        return ESP_ERR_TIMEOUT;
    }

    FILE *fp = fopen(CSV_FILE_PATH, "a");
    if (fp == NULL) {
        ESP_LOGE(TAG, "CSV ファイルを追記で開けません: path=%s errno=%d", CSV_FILE_PATH, errno);
        xSemaphoreGive(s_csv_mutex);
        return ESP_FAIL;
    }

    float bbox_x = -1.0f;
    float bbox_y = -1.0f;
    float bbox_w = -1.0f;
    float bbox_h = -1.0f;
    float left_eye_x = -1.0f;
    float left_eye_y = -1.0f;
    float right_eye_x = -1.0f;
    float right_eye_y = -1.0f;
    float nose_x = -1.0f;
    float nose_y = -1.0f;
    float left_mouth_x = -1.0f;
    float left_mouth_y = -1.0f;
    float right_mouth_x = -1.0f;
    float right_mouth_y = -1.0f;

    int left_eye_visible = 0;
    int right_eye_visible = 0;
    int nose_visible = 0;
    int left_mouth_visible = 0;
    int right_mouth_visible = 0;

    if (detection->face_detected && detection->box.valid) {
        int width = detection->box.x1 - detection->box.x0;
        int height = detection->box.y1 - detection->box.y0;
        if (width > 0 && height > 0) {
            bbox_x = normalize_bbox_x(detection->box.x0);
            bbox_y = normalize_bbox_y(detection->box.y0);
            bbox_w = (float)width / (float)STREAM_WIDTH;
            bbox_h = (float)height / (float)STREAM_HEIGHT;
        }

        left_eye_visible = normalize_landmark_pair(&detection->box, PRONE_LM_LEFT_EYE, &left_eye_x, &left_eye_y) ? 1 : 0;
        right_eye_visible = normalize_landmark_pair(&detection->box, PRONE_LM_RIGHT_EYE, &right_eye_x, &right_eye_y) ? 1 : 0;
        nose_visible = normalize_landmark_pair(&detection->box, PRONE_LM_NOSE, &nose_x, &nose_y) ? 1 : 0;
        left_mouth_visible =
            normalize_landmark_pair(&detection->box, PRONE_LM_LEFT_MOUTH, &left_mouth_x, &left_mouth_y) ? 1 : 0;
        right_mouth_visible =
            normalize_landmark_pair(&detection->box, PRONE_LM_RIGHT_MOUTH, &right_mouth_x, &right_mouth_y) ? 1 : 0;
    }

    int written = fprintf(fp,
                          "%lld,%s,%d,%d,%.4f,%d,%.6f,%.6f,%.6f,%.6f,"
                          "%.6f,%.6f,%d,%.6f,%.6f,%d,%.6f,%.6f,%d,"
                          "%.6f,%.6f,%d,%.6f,%.6f,%d\n",
                          (long long)detection->timestamp_ms,
                          session_id,
                          label,
                          detection->face_detected ? 1 : 0,
                          detection->face_score,
                          detection->face_count,
                          bbox_x,
                          bbox_y,
                          bbox_w,
                          bbox_h,
                          left_eye_x,
                          left_eye_y,
                          left_eye_visible,
                          right_eye_x,
                          right_eye_y,
                          right_eye_visible,
                          nose_x,
                          nose_y,
                          nose_visible,
                          left_mouth_x,
                          left_mouth_y,
                          left_mouth_visible,
                          right_mouth_x,
                          right_mouth_y,
                          right_mouth_visible);
    int write_errno = errno;
    int flush_result = fflush(fp);
    int flush_errno = errno;
    int close_result = fclose(fp);
    int close_errno = errno;
    xSemaphoreGive(s_csv_mutex);
    if (written <= 0 || flush_result != 0 || close_result != 0) {
        ESP_LOGE(TAG,
                 "CSV 保存失敗: written=%d flush=%d close=%d write_errno=%d flush_errno=%d close_errno=%d label=%d session_id=%s face_detected=%d face_count=%d",
                 written,
                 flush_result,
                 close_result,
                 write_errno,
                 flush_errno,
                 close_errno,
                 label,
                 session_id,
                 detection->face_detected ? 1 : 0,
                 detection->face_count);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html>"
        "<html><head><meta charset=\"utf-8\"><title>prone データ収集</title>"
        "<style>"
        "body{font-family:sans-serif;margin:20px;}"
        "#wrap{position:relative;width:320px;height:240px;display:inline-block;border:1px solid #ddd;}"
        "#stream{width:320px;height:240px;display:block;background:#111;}"
        "#face-box{position:absolute;border:2px solid #ef4444;display:none;pointer-events:none;box-sizing:border-box;}"
        "#status{margin:12px 0;font-weight:700;}"
        ".row{margin:10px 0;}"
        "button{padding:10px 14px;margin-right:8px;cursor:pointer;}"
        "input{padding:8px;width:220px;}"
        "pre{background:#f7f7f7;padding:12px;white-space:pre-wrap;}"
        "</style>"
        "</head><body>"
        "<h1>prone データ収集</h1>"
        "<div id=\"wrap\"><img id=\"stream\" alt=\"stream\"><div id=\"face-box\"></div></div>"
        "<div id=\"status\">状態: 初期化中</div>"
        "<div class=\"row\"><label>session_id <input id=\"session\" value=\"session_001\"></label></div>"
        "<div class=\"row\">"
        "<button id=\"save-prone\">うつ伏せ保存</button>"
        "<button id=\"save-non-prone\">非うつ伏せ保存</button>"
        "<button id=\"export\">CSV エクスポート</button>"
        "<button id=\"reset\">CSV リセット</button>"
        "</div>"
        "<pre id=\"detail\">読み込み中...</pre>"
        "<script>"
        "const stream=document.getElementById('stream');"
        "const box=document.getElementById('face-box');"
        "const statusEl=document.getElementById('status');"
        "const detail=document.getElementById('detail');"
        "const sessionInput=document.getElementById('session');"
        "let statusLoading=false;"
        "stream.src='http://'+location.hostname+':81/stream';"
        "stream.onerror=()=>{statusEl.textContent='状態: ストリーム接続失敗';};"
        "function clamp(v,min,max){return Math.min(max,Math.max(min,v));}"
        "async function loadStatus(){"
        "if(statusLoading){return;}"
        "statusLoading=true;"
        "try{"
        "const resp=await fetch('/api/status',{cache:'no-store'});"
        "if(!resp.ok){statusEl.textContent='状態: API 失敗';return;}"
        "const data=await resp.json();"
        "statusEl.textContent='状態: wifi='+data.wifi+' camera='+data.camera+' sd='+data.sdcard+' face='+(data.face_detected?1:0)+' count='+data.face_count+' score='+data.face_score.toFixed(3);"
        "detail.textContent=JSON.stringify(data,null,2);"
        "if(data.bbox&&data.face_detected){"
        "const x0=clamp(data.bbox.x0,0,319),y0=clamp(data.bbox.y0,0,239),x1=clamp(data.bbox.x1,0,319),y1=clamp(data.bbox.y1,0,239);"
        "if(x1>x0&&y1>y0){box.style.left=x0+'px';box.style.top=y0+'px';box.style.width=(x1-x0)+'px';box.style.height=(y1-y0)+'px';box.style.display='block';}"
        "else{box.style.display='none';}"
        "}else{box.style.display='none';}"
        "}catch(e){statusEl.textContent='状態: 通信失敗';}"
        "finally{statusLoading=false;}"
        "}"
        "async function saveSample(label){"
        "const session_id=sessionInput.value.trim();"
        "if(!session_id){alert('session_id を入力してください');return;}"
        "const resp=await fetch('/api/sample',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({label,session_id})});"
        "const text=await resp.text();"
        "if(!resp.ok){alert('保存失敗: '+text);return;}"
        "alert('保存成功: '+text);"
        "loadStatus();"
        "}"
        "document.getElementById('save-prone').onclick=()=>saveSample(1);"
        "document.getElementById('save-non-prone').onclick=()=>saveSample(0);"
        "document.getElementById('export').onclick=()=>{window.location='/api/export';};"
        "document.getElementById('reset').onclick=async()=>{"
        "if(!confirm('CSV を空にします。続行しますか。')){return;}"
        "const resp=await fetch('/api/reset',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({confirm:'" RESET_CONFIRM_TOKEN "'})});"
        "const text=await resp.text();"
        "if(!resp.ok){alert('リセット失敗: '+text);return;}"
        "alert('リセット完了');"
        "loadStatus();"
        "};"
        "setInterval(loadStatus," STRINGIFY(STATUS_POLL_INTERVAL_MS) ");"
        "loadStatus();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[1024];
    latest_detection_t detection = get_latest_detection_copy();

    int written = snprintf(
        json,
        sizeof(json),
        "{\"wifi\":\"%s\",\"camera\":\"%s\",\"sdcard\":\"%s\",\"inference\":\"%s\","
        "\"timestamp_ms\":%lld,\"face_detected\":%s,\"face_score\":%.4f,\"face_count\":%d,"
        "\"bbox\":{\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d,\"valid\":%s},"
        "\"landmarks_valid\":%s}",
        s_wifi_connected ? "connected" : "disconnected",
        s_camera_ready ? "ok" : "fault",
        s_sd_ready ? "ok" : "fault",
        prone_inference_status_to_string(prone_inference_get_status()),
        (long long)detection.timestamp_ms,
        detection.face_detected ? "true" : "false",
        detection.face_score,
        detection.face_count,
        detection.box.x0,
        detection.box.y0,
        detection.box.x1,
        detection.box.y1,
        detection.box.valid ? "true" : "false",
        detection.box.landmarks_valid ? "true" : "false");
    if (written < 0 || written >= (int)sizeof(json)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_request_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req == NULL || body == NULL || body_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || (size_t)req->content_len >= body_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int total_received = 0;
    while (total_received < req->content_len) {
        int received = httpd_req_recv(req, body + total_received, req->content_len - total_received);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ESP_FAIL;
        }
        total_received += received;
    }

    body[total_received] = '\0';
    return ESP_OK;
}

static const char *csv_error_to_message(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "csv lock timeout";
    case ESP_ERR_INVALID_STATE:
        return "sd card not ready";
    default:
        return "failed to save csv";
    }
}

static esp_err_t parse_sample_request(httpd_req_t *req, int *label, char *session_id, size_t session_id_size)
{
    if (req == NULL || label == NULL || session_id == NULL || session_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[257];
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *label_item = cJSON_GetObjectItemCaseSensitive(root, "label");
    cJSON *session_item = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!cJSON_IsNumber(label_item) || !cJSON_IsString(session_item) || session_item->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    *label = label_item->valueint;
    strlcpy(session_id, session_item->valuestring, session_id_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t sample_post_handler(httpd_req_t *req)
{
    int label = -1;
    char session_id[64];
    esp_err_t err = parse_sample_request(req, &label, session_id, sizeof(session_id));
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid json");
    }

    if (label != 0 && label != 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "label must be 0 or 1");
    }
    if (session_id[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "session_id is required");
    }
    if (!s_sd_ready) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "sd card not ready");
    }

    latest_detection_t detection = get_latest_detection_copy();
    if (detection.face_count > 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "multiple faces detected");
    }

    err = append_sample_csv(&detection, label, session_id);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, csv_error_to_message(err));
    }

    char message[128];
    snprintf(message,
             sizeof(message),
             "{\"saved\":true,\"label\":%d,\"face_detected\":%s,\"face_count\":%d}",
             label,
             detection.face_detected ? "true" : "false",
             detection.face_count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t export_get_handler(httpd_req_t *req)
{
    if (!s_sd_ready) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "sd card not ready");
    }

    FILE *fp = fopen(CSV_FILE_PATH, "r");
    if (fp == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "csv open failed");
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"prone_samples.csv\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buffer[512];
    while (!feof(fp)) {
        size_t read_size = fread(buffer, 1, sizeof(buffer), fp);
        if (read_size > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, buffer, read_size);
            if (err != ESP_OK) {
                fclose(fp);
                return err;
            }
        }
    }

    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t parse_reset_request(httpd_req_t *req, bool *confirmed)
{
    if (req == NULL || confirmed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *confirmed = false;
    char body[129];
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *confirm_item = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    if (cJSON_IsString(confirm_item) && confirm_item->valuestring != NULL &&
        strcmp(confirm_item->valuestring, RESET_CONFIRM_TOKEN) == 0) {
        *confirmed = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    bool confirmed = false;
    esp_err_t err = parse_reset_request(req, &confirmed);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid json");
    }
    if (!confirmed) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "confirm token mismatch");
    }
    if (!s_sd_ready) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "sd card not ready");
    }

    if (xSemaphoreTake(s_csv_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "csv lock failed");
    }

    FILE *fp = fopen(CSV_FILE_PATH, "w");
    if (fp == NULL) {
        xSemaphoreGive(s_csv_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "csv reset failed");
    }
    fclose(fp);
    xSemaphoreGive(s_csv_mutex);

    err = ensure_csv_header();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "csv header write failed");
    }

    return httpd_resp_sendstr(req, "reset ok");
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    if (!s_camera_ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "camera not ready");
    }

    static const char *stream_content_type = "multipart/x-mixed-replace;boundary=frame";
    static const char *stream_boundary = "\r\n--frame\r\n";
    char part_header[64];
    esp_err_t stream_err = ESP_OK;

    httpd_resp_set_type(req, stream_content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb == NULL) {
            ESP_LOGW(TAG, "カメラフレーム取得失敗");
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        if ((now_ms - s_last_inference_publish_ms) >= INFERENCE_INTERVAL_MS) {
            if (publish_frame_for_inference(fb)) {
                s_last_inference_publish_ms = now_ms;
            }
        }

        int hlen = snprintf(part_header,
                            sizeof(part_header),
                            "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                            (unsigned)fb->len);
        if (hlen <= 0 || hlen >= (int)sizeof(part_header)) {
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }

        stream_err = httpd_resp_send_chunk(req, stream_boundary, strlen(stream_boundary));
        if (stream_err == ESP_OK) {
            stream_err = httpd_resp_send_chunk(req, part_header, hlen);
        }
        if (stream_err == ESP_OK) {
            stream_err = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (stream_err == ESP_OK) {
            stream_err = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        esp_camera_fb_return(fb);
        if (stream_err != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(STREAM_PACING_DELAY_MS));
    }

    ESP_LOGI(TAG, "ストリーム接続終了: %s", esp_err_to_name(stream_err));
    return stream_err;
}

static bool publish_frame_for_inference(camera_fb_t *fb)
{
    if (fb == NULL || fb->buf == NULL || fb->len == 0) {
        return false;
    }
    if (fb->len > INFERENCE_JPEG_MAX_BYTES) {
        ESP_LOGW(TAG, "推論用 JPEG が大きすぎるためスキップ: %u bytes", (unsigned)fb->len);
        return false;
    }
    if (s_inference_frame_mutex == NULL || s_inference_frame_buf == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_inference_frame_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    memcpy(s_inference_frame_buf, fb->buf, fb->len);
    s_inference_frame_len = fb->len;
    s_inference_frame_pending = true;
    xSemaphoreGive(s_inference_frame_mutex);
    return true;
}

static void inference_task(void *arg)
{
    (void)arg;

    uint8_t *work_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work_buf == NULL) {
        work_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_8BIT);
    }
    if (work_buf == NULL) {
        ESP_LOGE(TAG, "推論バッファ確保失敗");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t frame_len = 0;

        if (xSemaphoreTake(s_inference_frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_inference_frame_pending && s_inference_frame_len > 0) {
                frame_len = s_inference_frame_len;
                memcpy(work_buf, s_inference_frame_buf, frame_len);
                s_inference_frame_pending = false;
            }
            xSemaphoreGive(s_inference_frame_mutex);
        }

        if (frame_len == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool face_detected = false;
        float face_score = 0.0f;
        prone_face_box_t face_box;
        int face_count = 0;
        esp_err_t err = prone_inference_run_jpeg(work_buf, frame_len, &face_detected, &face_score, &face_count, &face_box);
        if (err == ESP_OK) {
            set_latest_detection(face_detected, face_score, face_count, &face_box);
        } else {
            ESP_LOGW(TAG, "顔検出失敗: %s", esp_err_to_name(err));
            set_latest_detection(false, 0.0f, 0, NULL);
        }
    }
}

static esp_err_t start_inference_task(void)
{
    if (s_inference_task_handle != NULL) {
        return ESP_OK;
    }

    s_inference_frame_mutex = xSemaphoreCreateMutex();
    if (s_inference_frame_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_inference_frame_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_inference_frame_buf == NULL) {
        s_inference_frame_buf = (uint8_t *)heap_caps_malloc(INFERENCE_JPEG_MAX_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_inference_frame_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(inference_task,
                                     "inference_task",
                                     8192,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_inference_task_handle);
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t sample_uri = {
        .uri = "/api/sample",
        .method = HTTP_POST,
        .handler = sample_post_handler,
    };
    const httpd_uri_t export_uri = {
        .uri = "/api/export",
        .method = HTTP_GET,
        .handler = export_get_handler,
    };
    const httpd_uri_t reset_uri = {
        .uri = "/api/reset",
        .method = HTTP_POST,
        .handler = reset_post_handler,
    };

    httpd_register_uri_handler(s_http_server, &root_uri);
    httpd_register_uri_handler(s_http_server, &status_uri);
    httpd_register_uri_handler(s_http_server, &sample_uri);
    httpd_register_uri_handler(s_http_server, &export_uri);
    httpd_register_uri_handler(s_http_server, &reset_uri);
    return ESP_OK;
}

static esp_err_t start_stream_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_open_sockets = 1;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;

    esp_err_t err = httpd_start(&s_stream_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
    };
    httpd_register_uri_handler(s_stream_http_server, &stream_uri);
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs());

    s_detection_mutex = xSemaphoreCreateMutex();
    s_csv_mutex = xSemaphoreCreateMutex();
    if (s_detection_mutex == NULL || s_csv_mutex == NULL) {
        ESP_LOGE(TAG, "ミューテックス確保失敗");
        return;
    }

    memset(&s_latest_detection, 0, sizeof(s_latest_detection));
    s_latest_detection.box.x0 = -1;
    s_latest_detection.box.y0 = -1;
    s_latest_detection.box.x1 = -1;
    s_latest_detection.box.y1 = -1;

    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) < 8) {
        ESP_LOGE(TAG, "Wi-Fi 設定不足: menuconfig で SSID とパスフレーズを設定してください");
        return;
    }

    ESP_ERROR_CHECK(start_wifi_sta());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Wi-Fi 接続待機失敗");
        return;
    }

    esp_err_t camera_err = init_camera();
    if (camera_err != ESP_OK) {
        ESP_LOGW(TAG, "カメラ未初期化");
    }

    esp_err_t sd_err = init_sd_card();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD カード未初期化");
    }

    esp_err_t infer_err = ESP_ERR_INVALID_STATE;
    if (s_camera_ready) {
        infer_err = prone_inference_init();
        if (infer_err != ESP_OK) {
            ESP_LOGW(TAG, "顔検出初期化失敗: %s", esp_err_to_name(infer_err));
        }
    } else {
        ESP_LOGW(TAG, "カメラ未初期化のため顔検出を開始しません");
    }

    if (s_camera_ready && infer_err == ESP_OK) {
        esp_err_t task_err = start_inference_task();
        if (task_err != ESP_OK) {
            ESP_LOGE(TAG, "推論タスク開始失敗: %s", esp_err_to_name(task_err));
        }
    }

    ESP_ERROR_CHECK(start_http_server());
    ESP_ERROR_CHECK(start_stream_http_server());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
