#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

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

#define WIFI_CONNECTED_BIT BIT0

#define WIFI_WAIT_TIMEOUT_MS 30000

#define BOARD_NAME "Freenove ESP32-S3 WROOM CAM"
#define MOUNT_POINT "/sdcard"
#define DATASET_DIR MOUNT_POINT "/dataset"
#define IMAGES_DIR DATASET_DIR "/images"
#define METADATA_PATH DATASET_DIR "/metadata.csv"

#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY "\r\n--frame\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

#define MAX_HTTP_BODY_SIZE 4096
#define MAX_ID_LEN 64
#define MAX_NOTES_LEN 256
#define MAX_CSV_LINE_LEN 1024
#define MAX_CAPTURE_ID_LEN 32
#define MAX_IMAGE_PATH_LEN 96
#define MAX_FILE_PATH_LEN 384

#define DEFAULT_PAGE_SIZE 50
#define MAX_PAGE_SIZE 100
#define COLLECTOR_MAIN_TASK_STACK_SIZE 12288

#define CAMERA_XCLK_FREQ_HZ 10000000
#define CAMERA_JPEG_QUALITY 12
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

static const char *METADATA_HEADER =
    "capture_id,timestamp_ms,subject_id,session_id,location_id,lighting_id,"
    "camera_position_id,annotator_id,label,label_name,is_usable_for_training,"
    "exclude_reason,notes,image_path,image_bytes,frame_width,frame_height,"
    "pixel_format,jpeg_quality,board_name\n";

typedef struct {
    char subject_id[MAX_ID_LEN];
    char session_id[MAX_ID_LEN];
    char location_id[MAX_ID_LEN];
    char lighting_id[MAX_ID_LEN];
    char camera_position_id[MAX_ID_LEN];
    char annotator_id[MAX_ID_LEN];
    char exclude_reason[32];
    char notes[MAX_NOTES_LEN];
    int label;
    int is_usable_for_training;
} capture_request_t;

typedef struct {
    char wifi[16];
    char camera[16];
    char sdcard[16];
    int sample_count;
    int usable_count;
    int excluded_count;
    int64_t last_capture_ms;
} collector_status_t;

typedef struct {
    httpd_handle_t server;
    httpd_handle_t stream_server;
    EventGroupHandle_t wifi_event_group;
    SemaphoreHandle_t storage_mutex;
    bool camera_ready;
    bool sdcard_ready;
    bool metadata_ready;
    bool wifi_connected;
    collector_status_t status;
    sdmmc_card_t *sdcard;
} app_state_t;

static app_state_t g_state = {
    .server = NULL,
    .stream_server = NULL,
    .wifi_event_group = NULL,
    .storage_mutex = NULL,
    .camera_ready = false,
    .sdcard_ready = false,
    .metadata_ready = false,
    .wifi_connected = false,
    .status = {
        .wifi = "disconnected",
        .camera = "error",
        .sdcard = "error",
        .sample_count = 0,
        .usable_count = 0,
        .excluded_count = 0,
        .last_capture_ms = 0,
    },
    .sdcard = NULL,
};

static const char *allowed_exclude_reasons[] = {
    "ambiguous_pose",
    "face_not_visible",
    "subject_missing",
    "multiple_subjects",
    "motion_blur",
    "poor_lighting",
    "annotation_error",
    "other",
};

static esp_err_t start_web_server(void);
static esp_err_t start_stream_server(void);
static esp_err_t ensure_storage_ready(void);
static void refresh_dataset_counts_locked(void);

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

static int64_t current_time_ms(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

static bool is_valid_generic_token(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    for (size_t i = 0; value[i] != '\0'; ++i) {
        char ch = value[i];
        if (!(islower((unsigned char) ch) || isdigit((unsigned char) ch) || ch == '_')) {
            return false;
        }
    }
    return true;
}

static bool is_digits_only(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }

    for (size_t i = 0; value[i] != '\0'; ++i) {
        if (!isdigit((unsigned char) value[i])) {
            return false;
        }
    }
    return true;
}

static bool is_valid_subject_id(const char *value)
{
    if (!is_valid_generic_token(value)) {
        return false;
    }
    if (strncmp(value, "baby_", 5) != 0) {
        return false;
    }
    if (strlen(value) < 8) {
        return false;
    }
    return is_digits_only(value + 5);
}

static bool is_valid_location_id(const char *value)
{
    return is_valid_generic_token(value) && strncmp(value, "loc_", 4) == 0 && strlen(value) >= 6;
}

static bool is_valid_lighting_id(const char *value)
{
    return is_valid_generic_token(value) && strncmp(value, "light_", 6) == 0;
}

static bool is_valid_camera_position_id(const char *value)
{
    return is_valid_generic_token(value) && strncmp(value, "campos_", 7) == 0;
}

static bool is_valid_annotator_id(const char *value)
{
    return is_valid_generic_token(value) && strncmp(value, "ann_", 4) == 0;
}

static bool is_valid_session_id(const char *subject_id, const char *session_id)
{
    size_t subject_len;
    const char *date_part;
    const char *time_part;
    const char *setup_part;

    if (!is_valid_generic_token(session_id) || !is_valid_subject_id(subject_id)) {
        return false;
    }

    subject_len = strlen(subject_id);
    if (strncmp(session_id, subject_id, subject_len) != 0 || session_id[subject_len] != '_') {
        return false;
    }

    date_part = session_id + subject_len + 1;
    if (strlen(date_part) < 17) {
        return false;
    }

    for (int i = 0; i < 8; ++i) {
        if (!isdigit((unsigned char) date_part[i])) {
            return false;
        }
    }
    if (date_part[8] != '_') {
        return false;
    }

    time_part = date_part + 9;
    if (!(strncmp(time_part, "am_", 3) == 0 || strncmp(time_part, "pm_", 3) == 0 ||
          strncmp(time_part, "night_", 6) == 0)) {
        return false;
    }

    setup_part = strchr(time_part, '_');
    if (setup_part == NULL || setup_part[1] == '\0') {
        return false;
    }
    return true;
}

static bool is_valid_exclude_reason(const char *value)
{
    size_t i;

    if (value == NULL || value[0] == '\0') {
        return true;
    }

    for (i = 0; i < sizeof(allowed_exclude_reasons) / sizeof(allowed_exclude_reasons[0]); ++i) {
        if (strcmp(value, allowed_exclude_reasons[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *label_name_from_value(int label)
{
    return label == 1 ? "prone" : "non_prone";
}

static const char *pixel_format_name(pixformat_t format)
{
    switch (format) {
        case PIXFORMAT_JPEG:
            return "jpeg";
        case PIXFORMAT_RGB565:
            return "rgb565";
        case PIXFORMAT_GRAYSCALE:
            return "grayscale";
        default:
            return "unknown";
    }
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_OK : ESP_FAIL;
}

static esp_err_t ensure_metadata_file(void)
{
    FILE *file = fopen(METADATA_PATH, "r");
    if (file != NULL) {
        fclose(file);
        return ESP_OK;
    }

    file = fopen(METADATA_PATH, "w");
    if (file == NULL) {
        return ESP_FAIL;
    }
    if (fputs(METADATA_HEADER, file) < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

static char *csv_escape_dup(const char *src)
{
    size_t extra = 2;
    size_t len = 0;
    char *out;
    size_t i;
    size_t j = 0;

    if (src == NULL) {
        src = "";
    }

    len = strlen(src);
    for (i = 0; i < len; ++i) {
        if (src[i] == '"') {
            extra++;
        }
    }

    out = calloc(len + extra + 1, 1);
    if (out == NULL) {
        return NULL;
    }

    out[j++] = '"';
    for (i = 0; i < len; ++i) {
        if (src[i] == '"') {
            out[j++] = '"';
        }
        out[j++] = src[i];
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static bool csv_read_field(char **cursor, char *out, size_t out_len)
{
    char *p = *cursor;
    size_t idx = 0;
    bool quoted = false;

    if (p == NULL || *p == '\0') {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return false;
    }

    if (*p == '"') {
        quoted = true;
        ++p;
    }

    while (*p != '\0') {
        if (quoted) {
            if (*p == '"' && p[1] == '"') {
                if (idx + 1 < out_len) {
                    out[idx++] = '"';
                }
                p += 2;
                continue;
            }
            if (*p == '"') {
                ++p;
                break;
            }
        } else if (*p == ',' || *p == '\n' || *p == '\r') {
            break;
        }

        if (idx + 1 < out_len) {
            out[idx++] = *p;
        }
        ++p;
    }

    out[idx] = '\0';

    while (*p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p == ',') {
        ++p;
    }

    *cursor = p;
    return true;
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

static esp_err_t validate_capture_request(const capture_request_t *request, char *reason, size_t reason_len)
{
    if (!is_valid_subject_id(request->subject_id)) {
        snprintf(reason, reason_len, "subject_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_session_id(request->subject_id, request->session_id)) {
        snprintf(reason, reason_len, "session_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_location_id(request->location_id)) {
        snprintf(reason, reason_len, "location_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_lighting_id(request->lighting_id)) {
        snprintf(reason, reason_len, "lighting_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_camera_position_id(request->camera_position_id)) {
        snprintf(reason, reason_len, "camera_position_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_annotator_id(request->annotator_id)) {
        snprintf(reason, reason_len, "annotator_id が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!(request->label == 0 || request->label == 1)) {
        snprintf(reason, reason_len, "label は 0 または 1 が必要です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!(request->is_usable_for_training == 0 || request->is_usable_for_training == 1)) {
        snprintf(reason, reason_len, "is_usable_for_training は 0 または 1 が必要です");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_valid_exclude_reason(request->exclude_reason)) {
        snprintf(reason, reason_len, "exclude_reason が不正です");
        return ESP_ERR_INVALID_ARG;
    }
    if (request->is_usable_for_training == 1 && request->exclude_reason[0] != '\0') {
        snprintf(reason, reason_len, "学習利用可のとき exclude_reason は空にしてください");
        return ESP_ERR_INVALID_ARG;
    }
    if (request->is_usable_for_training == 0 && request->exclude_reason[0] == '\0') {
        snprintf(reason, reason_len, "学習利用不可のとき exclude_reason は必須です");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t append_metadata_line_locked(const capture_request_t *request,
                                             const camera_fb_t *fb,
                                             int64_t timestamp_ms,
                                             const char *capture_id,
                                             const char *image_path)
{
    char *subject_id = csv_escape_dup(request->subject_id);
    char *session_id = csv_escape_dup(request->session_id);
    char *location_id = csv_escape_dup(request->location_id);
    char *lighting_id = csv_escape_dup(request->lighting_id);
    char *camera_position_id = csv_escape_dup(request->camera_position_id);
    char *annotator_id = csv_escape_dup(request->annotator_id);
    char *exclude_reason = csv_escape_dup(request->exclude_reason);
    char *notes = csv_escape_dup(request->notes);
    char *image_path_csv = csv_escape_dup(image_path);
    char *board_name = csv_escape_dup(BOARD_NAME);
    FILE *file = NULL;
    int written = 0;
    esp_err_t result = ESP_FAIL;

    if (subject_id == NULL || session_id == NULL || location_id == NULL || lighting_id == NULL ||
        camera_position_id == NULL || annotator_id == NULL || exclude_reason == NULL ||
        notes == NULL || image_path_csv == NULL || board_name == NULL) {
        goto cleanup;
    }

    file = fopen(METADATA_PATH, "a");
    if (file == NULL) {
        goto cleanup;
    }

    written = fprintf(file,
                      "\"%s\",%lld,%s,%s,%s,%s,%s,%s,%d,\"%s\",%d,%s,%s,%s,%u,%u,%u,\"%s\",%d,%s\n",
                      capture_id,
                      (long long) timestamp_ms,
                      subject_id,
                      session_id,
                      location_id,
                      lighting_id,
                      camera_position_id,
                      annotator_id,
                      request->label,
                      label_name_from_value(request->label),
                      request->is_usable_for_training,
                      exclude_reason,
                      notes,
                      image_path_csv,
                      (unsigned int) fb->len,
                      (unsigned int) fb->width,
                      (unsigned int) fb->height,
                      pixel_format_name(fb->format),
                      CAMERA_JPEG_QUALITY,
                      board_name);
    if (written <= 0 || fflush(file) != 0) {
        goto cleanup;
    }

    g_state.status.sample_count++;
    if (request->is_usable_for_training == 1) {
        g_state.status.usable_count++;
    } else {
        g_state.status.excluded_count++;
    }
    g_state.status.last_capture_ms = timestamp_ms;
    result = ESP_OK;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    free(subject_id);
    free(session_id);
    free(location_id);
    free(lighting_id);
    free(camera_position_id);
    free(annotator_id);
    free(exclude_reason);
    free(notes);
    free(image_path_csv);
    free(board_name);
    return result;
}

static esp_err_t save_capture_locked(const capture_request_t *request,
                                     char *capture_id,
                                     size_t capture_id_len,
                                     char *image_path,
                                     size_t image_path_len)
{
    int64_t timestamp_ms;
    char image_abs_path[128];
    camera_fb_t *fb = NULL;
    FILE *image_file = NULL;
    esp_err_t result = ESP_FAIL;

    if (!g_state.camera_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    timestamp_ms = current_time_ms();
    snprintf(capture_id, capture_id_len, "%lld", (long long) timestamp_ms);
    snprintf(image_path, image_path_len, "images/%s.jpg", capture_id);
    snprintf(image_abs_path, sizeof(image_abs_path), "%s/%s", DATASET_DIR, image_path);

    fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "カメラフレーム取得に失敗しました");
        return ESP_FAIL;
    }

    image_file = fopen(image_abs_path, "wb");
    if (image_file == NULL) {
        ESP_LOGE(TAG, "画像保存に失敗しました: %s", image_abs_path);
        goto cleanup;
    }
    if (fwrite(fb->buf, 1, fb->len, image_file) != fb->len || fflush(image_file) != 0) {
        ESP_LOGE(TAG, "JPEG 書き込みに失敗しました");
        goto cleanup;
    }
    fclose(image_file);
    image_file = NULL;

    if (append_metadata_line_locked(request, fb, timestamp_ms, capture_id, image_path) != ESP_OK) {
        unlink(image_abs_path);
        ESP_LOGE(TAG, "metadata.csv 追記に失敗しました");
        goto cleanup;
    }

    result = ESP_OK;

cleanup:
    if (image_file != NULL) {
        fclose(image_file);
        unlink(image_abs_path);
    }
    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }
    return result;
}

static void refresh_dataset_counts_locked(void)
{
    FILE *file = fopen(METADATA_PATH, "r");
    char line[MAX_CSV_LINE_LEN];

    g_state.status.sample_count = 0;
    g_state.status.usable_count = 0;
    g_state.status.excluded_count = 0;
    g_state.status.last_capture_ms = 0;

    if (file == NULL) {
        return;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor = line;
        char field[20][MAX_ID_LEN > MAX_NOTES_LEN ? MAX_ID_LEN : MAX_NOTES_LEN];
        int field_index = 0;

        memset(field, 0, sizeof(field));
        while (field_index < 20 && csv_read_field(&cursor, field[field_index], sizeof(field[field_index]))) {
            field_index++;
            if (*cursor == '\0') {
                break;
            }
        }

        if (field_index < 12) {
            continue;
        }

        g_state.status.sample_count++;
        if (strcmp(field[10], "1") == 0 && field[11][0] == '\0') {
            g_state.status.usable_count++;
        } else {
            g_state.status.excluded_count++;
        }

        int64_t timestamp_ms = strtoll(field[1], NULL, 10);
        if (timestamp_ms > g_state.status.last_capture_ms) {
            g_state.status.last_capture_ms = timestamp_ms;
        }
    }

    fclose(file);
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
        set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");
        g_state.sdcard_ready = false;
        return err;
    }

    if (ensure_dir(DATASET_DIR) != ESP_OK || ensure_dir(IMAGES_DIR) != ESP_OK ||
        ensure_metadata_file() != ESP_OK) {
        ESP_LOGE(TAG, "データセットディレクトリの準備に失敗しました");
        set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");
        g_state.sdcard_ready = false;
        return ESP_FAIL;
    }

    g_state.sdcard_ready = true;
    g_state.metadata_ready = true;
    set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "ok");

    if (xSemaphoreTake(g_state.storage_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        refresh_dataset_counts_locked();
        xSemaphoreGive(g_state.storage_mutex);
    }

    return ESP_OK;
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
        g_state.camera_ready = false;
        set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "error");
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_framesize(sensor, CAMERA_FRAME_SIZE);
        sensor->set_quality(sensor, CAMERA_JPEG_QUALITY);
    }

    g_state.camera_ready = true;
    set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "ok");
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
        g_state.wifi_connected = false;
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
        xEventGroupClearBits(g_state.wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        g_state.wifi_connected = true;
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "connected");
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
        set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
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
    static const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>prone collector</title>"
        "<style>"
        "body{font-family:sans-serif;background:#f4f5f7;color:#111;margin:0;padding:16px;}"
        ".wrap{max-width:980px;margin:0 auto;display:grid;gap:16px;}"
        ".panel{background:#fff;border-radius:12px;padding:16px;box-shadow:0 2px 10px rgba(0,0,0,.08);}"
        "img{width:100%;background:#111;border-radius:12px;display:block;}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}"
        "label{display:block;font-size:14px;margin-bottom:4px;}"
        "input,select,textarea,button{width:100%;padding:10px;border:1px solid #cbd5e1;border-radius:8px;box-sizing:border-box;}"
        "button{background:#1d4ed8;color:#fff;border:none;font-weight:700;cursor:pointer;min-height:52px;}"
        ".secondary{background:#475569;}"
        ".danger{background:#b91c1c;}"
        ".row{display:flex;gap:12px;flex-wrap:wrap;}"
        ".row > div{flex:1 1 200px;}"
        ".stream_shell{max-width:420px;margin:0 auto;display:grid;gap:12px;}"
        ".stream_capture{display:grid;grid-template-columns:1fr;gap:10px;}"
        ".stream_frame{max-width:420px;margin:0 auto;}"
        ".status{white-space:pre-wrap;font-family:monospace;font-size:13px;background:#0f172a;color:#e2e8f0;border-radius:8px;padding:12px;}"
        ".result{white-space:pre-wrap;font-family:monospace;font-size:13px;background:#172554;color:#dbeafe;border-radius:8px;padding:12px;min-height:48px;}"
        "</style></head><body><div class='wrap'>"
        "<div class='panel'><div class='stream_shell'>"
        "<h1>学習データ収集</h1>"
        "<div class='stream_capture'>"
        "<button id='capture_button' onclick='captureSample()'>撮影</button>"
        "</div>"
        "<div class='stream_frame'><img id='stream_view' alt='stream'></div>"
        "</div></div>"
        "<div class='panel'><div class='grid'>"
        "<div><label>subject_id</label><input id='subject_id' value='baby_001'></div>"
        "<div><label>session_id</label><input id='session_id' value='baby_001_20260315_am_setup01'></div>"
        "<div><label>location_id</label><input id='location_id' value='loc_01'></div>"
        "<div><label>lighting_id</label><input id='lighting_id' value='light_day'></div>"
        "<div><label>camera_position_id</label><input id='camera_position_id' value='campos_top'></div>"
        "<div><label>annotator_id</label><input id='annotator_id' value='ann_a01'></div>"
        "<div><label>label</label><select id='label'><option value='1'>うつ伏せ</option><option value='0'>非うつ伏せ</option></select></div>"
        "<div><label>学習利用可否</label><select id='is_usable_for_training'><option value='1'>利用可</option><option value='0'>利用不可</option></select></div>"
        "<div><label>exclude_reason</label><select id='exclude_reason'>"
        "<option value=''>空</option><option value='ambiguous_pose'>ambiguous_pose</option>"
        "<option value='face_not_visible'>face_not_visible</option>"
        "<option value='subject_missing'>subject_missing</option>"
        "<option value='multiple_subjects'>multiple_subjects</option>"
        "<option value='motion_blur'>motion_blur</option>"
        "<option value='poor_lighting'>poor_lighting</option>"
        "<option value='annotation_error'>annotation_error</option>"
        "<option value='other'>other</option></select></div>"
        "<div style='grid-column:1/-1'><label>notes</label><textarea id='notes' rows='4'></textarea></div>"
        "</div>"
        "<div class='row' style='margin-top:12px'>"
        "<div><button id='export_button' class='secondary' onclick='exportDataset()'>エクスポート</button></div>"
        "<div><button class='danger' onclick='resetDataset()'>SDカードリセット</button></div>"
        "</div></div>"
        "<div class='panel'><h2>状態</h2><div id='status' class='status'>loading...</div></div>"
        "<div class='panel'><h2>応答</h2><div id='result' class='result'>待機中</div></div>"
        "</div>"
        "<script>"
        "function el(id){return document.getElementById(id);}"
        "function setResult(message){el('result').textContent=message;}"
        "function streamUrl(){return location.protocol+'//'+location.hostname+':81/stream';}"
        "function sleep(ms){return new Promise(resolve=>setTimeout(resolve,ms));}"
        "function triggerDownload(blob,name){"
        "const url=URL.createObjectURL(blob);"
        "const a=document.createElement('a');"
        "a.href=url;a.download=name;document.body.appendChild(a);a.click();a.remove();"
        "setTimeout(()=>URL.revokeObjectURL(url),30000);"
        "}"
        "async function downloadBlob(url,name){"
        "const r=await fetch(url);"
        "if(!r.ok){throw new Error(url+' '+r.status);}"
        "const blob=await r.blob();"
        "triggerDownload(blob,name);"
        "}"
        "async function fetchManifestPage(page,pageSize){"
        "const r=await fetch('/api/export/manifest?page='+page+'&page_size='+pageSize);"
        "if(!r.ok){throw new Error('manifest '+r.status);}"
        "return await r.json();"
        "}"
        "async function fetchAllManifestItems(){"
        "const pageSize=50;"
        "let page=1;"
        "let items=[];"
        "while(true){"
        "const data=await fetchManifestPage(page,pageSize);"
        "items=items.concat(data.items||[]);"
        "if(!data.has_next){break;}"
        "page+=1;"
        "}"
        "return items;"
        "}"
        "async function refreshStatus(){"
        "try{"
        "const r=await fetch('/api/status');"
        "const j=await r.json();"
        "el('status').textContent=JSON.stringify(j,null,2);"
        "}catch(e){"
        "el('status').textContent='status 取得失敗\\n'+String(e);"
        "}"
        "}"
        "function payload(){return {"
        "subject_id:el('subject_id').value.trim(),session_id:el('session_id').value.trim(),location_id:el('location_id').value.trim(),"
        "lighting_id:el('lighting_id').value.trim(),camera_position_id:el('camera_position_id').value.trim(),"
        "annotator_id:el('annotator_id').value.trim(),label:Number(el('label').value),"
        "is_usable_for_training:Number(el('is_usable_for_training').value),exclude_reason:el('exclude_reason').value.trim(),"
        "notes:el('notes').value};}"
        "async function captureSample(){"
        "const button=el('capture_button');"
        "button.disabled=true;"
        "setResult('撮影リクエスト送信中');"
        "try{"
        "const r=await fetch('/api/capture',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload())});"
        "const text=await r.text();"
        "setResult('HTTP '+r.status+'\\n'+text);"
        "if(!r.ok){throw new Error('capture failed');}"
        "await refreshStatus();"
        "}catch(e){"
        "setResult(el('result').textContent+'\\n'+String(e));"
        "}finally{"
        "button.disabled=false;"
        "}"
        "}"
        "async function exportDataset(){"
        "const button=el('export_button');"
        "button.disabled=true;"
        "try{"
        "setResult('metadata.csv を取得中');"
        "await downloadBlob('/api/export/metadata','metadata.csv');"
        "const items=await fetchAllManifestItems();"
        "const manifestBlob=new Blob([JSON.stringify({total_samples:items.length,items:items},null,2)],{type:'application/json'});"
        "triggerDownload(manifestBlob,'manifest.json');"
        "for(let i=0;i<items.length;i+=1){"
        "const item=items[i];"
        "setResult('画像を取得中 '+(i+1)+' / '+items.length+'\\n'+item.capture_id);"
        "await downloadBlob('/api/export/image?capture_id='+encodeURIComponent(item.capture_id),item.capture_id+'.jpg');"
        "await sleep(150);"
        "}"
        "setResult('エクスポート完了\\nmetadata.csv, manifest.json, 画像 '+items.length+' 件');"
        "}catch(e){"
        "setResult('エクスポート失敗\\n'+String(e));"
        "}finally{"
        "button.disabled=false;"
        "}"
        "}"
        "async function resetDataset(){"
        "if(!confirm('dataset を削除します'))return;"
        "const r=await fetch('/api/reset',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({confirm:'RESET'})});"
        "const text=await r.text();"
        "setResult('HTTP '+r.status+'\\n'+text);"
        "await refreshStatus();"
        "}"
        "el('stream_view').src=streamUrl();"
        "refreshStatus();"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    char *body;
    esp_err_t err;

    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    cJSON_AddStringToObject(root, "wifi", g_state.status.wifi);
    cJSON_AddStringToObject(root, "camera", g_state.status.camera);
    cJSON_AddStringToObject(root, "sdcard", g_state.status.sdcard);
    cJSON_AddNumberToObject(root, "sample_count", g_state.status.sample_count);
    cJSON_AddNumberToObject(root, "usable_count", g_state.status.usable_count);
    cJSON_AddNumberToObject(root, "excluded_count", g_state.status.excluded_count);
    cJSON_AddNumberToObject(root, "last_capture_ms", (double) g_state.status.last_capture_ms);

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
    cJSON *root = NULL;
    capture_request_t request = {0};
    char reason[128];
    char capture_id[MAX_CAPTURE_ID_LEN];
    char image_path[MAX_IMAGE_PATH_LEN];
    cJSON *response = NULL;
    char *body = NULL;
    esp_err_t err;

    if (!g_state.camera_ready) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "カメラ異常のため撮影できません");
    }
    if (!g_state.sdcard_ready || !g_state.metadata_ready) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD カード異常のため保存できません");
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

    err = save_capture_locked(&request, capture_id, sizeof(capture_id), image_path, sizeof(image_path));
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
    if (!g_state.sdcard_ready || !g_state.metadata_ready) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sdcard error");
    }
    return send_file(req, METADATA_PATH, "text/csv");
}

static esp_err_t image_handler(httpd_req_t *req)
{
    char capture_id[MAX_CAPTURE_ID_LEN];
    char path[128];

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
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    char *body;
    esp_err_t err;

    if (root == NULL || items == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
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

    if (xSemaphoreTake(g_state.storage_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return send_text_error_with_status(req, HTTP_STATUS_503, "busy");
    }

    file = fopen(METADATA_PATH, "r");
    if (file == NULL) {
        xSemaphoreGive(g_state.storage_mutex);
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
                xSemaphoreGive(g_state.storage_mutex);
                cJSON_Delete(root);
                cJSON_Delete(items);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
            }

            csv_read_field(&cursor, capture_id, sizeof(capture_id));
            csv_read_field(&cursor, timestamp_ms, sizeof(timestamp_ms));
            for (int i = 0; i < 6; ++i) {
                csv_read_field(&cursor, discard, sizeof(discard));
            }
            csv_read_field(&cursor, label, sizeof(label));
            for (int i = 0; i < 4; ++i) {
                csv_read_field(&cursor, discard, sizeof(discard));
            }
            csv_read_field(&cursor, image_path, sizeof(image_path));

            cJSON_AddStringToObject(item, "capture_id", capture_id);
            cJSON_AddStringToObject(item, "image_path", image_path);
            cJSON_AddNumberToObject(item, "timestamp_ms", (double) strtoll(timestamp_ms, NULL, 10));
            cJSON_AddNumberToObject(item, "label", atoi(label));
            cJSON_AddItemToArray(items, item);
        }
        current_index++;
    }
    fclose(file);

    cJSON_AddNumberToObject(root, "total_samples", g_state.status.sample_count);
    cJSON_AddNumberToObject(root, "page", page);
    cJSON_AddNumberToObject(root, "page_size", page_size);
    cJSON_AddBoolToObject(root, "has_next", current_index > end_index);
    cJSON_AddItemToObject(root, "items", items);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    xSemaphoreGive(g_state.storage_mutex);

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
    cJSON *root = NULL;
    char confirm[16];
    DIR *dir;
    struct dirent *entry;
    cJSON *response = NULL;
    char *body = NULL;
    esp_err_t err;

    if (!g_state.sdcard_ready || !g_state.metadata_ready) {
        return send_json_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD カード異常のためリセットできません");
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

    if (xSemaphoreTake(g_state.storage_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
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
    err = ensure_metadata_file();
    if (err == ESP_OK) {
        refresh_dataset_counts_locked();
    }
    xSemaphoreGive(g_state.storage_mutex);

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
    char part_buf[64];
    camera_fb_t *fb = NULL;
    esp_err_t err = ESP_OK;

    if (!g_state.camera_ready) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "camera error");
    }

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    while (true) {
        fb = esp_camera_fb_get();
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
    set_status_text(g_state.status.wifi, sizeof(g_state.status.wifi), "disconnected");
    set_status_text(g_state.status.camera, sizeof(g_state.status.camera), "error");
    set_status_text(g_state.status.sdcard, sizeof(g_state.status.sdcard), "error");

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
