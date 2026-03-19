#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "storage.h"

static const char *TAG = "storage_service";

static const char *METADATA_HEADER =
    "capture_id,timestamp_ms,subject_id,session_id,location_id,lighting_id,"
    "camera_position_id,annotator_id,label,label_name,is_usable_for_training,"
    "exclude_reason,notes,image_path,image_bytes,frame_width,frame_height,"
    "pixel_format,jpeg_quality,board_name\n";

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

static esp_err_t ensure_metadata_file(const char *metadata_path)
{
    FILE *file = fopen(metadata_path, "r");
    if (file != NULL) {
        fclose(file);
        return ESP_OK;
    }

    file = fopen(metadata_path, "w");
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

static esp_err_t csv_write_escaped_field(FILE *file, const char *value)
{
    const char *src = value == NULL ? "" : value;

    if (fputc('"', file) == EOF) {
        return ESP_FAIL;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        if (src[i] == '"' && fputc('"', file) == EOF) {
            return ESP_FAIL;
        }
        if (fputc(src[i], file) == EOF) {
            return ESP_FAIL;
        }
    }

    return fputc('"', file) == EOF ? ESP_FAIL : ESP_OK;
}

static esp_err_t append_metadata_line_locked(const capture_request_t *request,
                                             const camera_fb_t *fb,
                                             int64_t timestamp_ms,
                                             const char *capture_id,
                                             const char *image_path,
                                             storage_status_update_fn update_status)
{
    FILE *file = fopen(METADATA_PATH, "a");

    if (file == NULL) {
        return ESP_FAIL;
    }

    if (csv_write_escaped_field(file, capture_id) != ESP_OK ||
        fprintf(file, ",%lld,", (long long) timestamp_ms) < 0 ||
        csv_write_escaped_field(file, request->subject_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->session_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->location_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->lighting_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->camera_position_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->annotator_id) != ESP_OK ||
        fprintf(file, ",%d,", request->label) < 0 ||
        csv_write_escaped_field(file, label_name_from_value(request->label)) != ESP_OK ||
        fprintf(file, ",%d,", request->is_usable_for_training) < 0 ||
        csv_write_escaped_field(file, request->exclude_reason) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, request->notes) != ESP_OK ||
        fputc(',', file) == EOF ||
        csv_write_escaped_field(file, image_path) != ESP_OK ||
        fprintf(file, ",%u,%u,%u,", (unsigned int) fb->len, (unsigned int) fb->width, (unsigned int) fb->height) < 0 ||
        csv_write_escaped_field(file, pixel_format_name(fb->format)) != ESP_OK ||
        fprintf(file, ",%d,", CAMERA_JPEG_QUALITY) < 0 ||
        csv_write_escaped_field(file, BOARD_NAME) != ESP_OK ||
        fputc('\n', file) == EOF ||
        fflush(file) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    if (update_status != NULL) {
        update_status(request->is_usable_for_training, timestamp_ms);
    }
    return ESP_OK;
}

static void generate_capture_id_locked(int64_t *last_capture_id,
                                       char *capture_id,
                                       size_t capture_id_len,
                                       int64_t *timestamp_ms_out)
{
    int64_t capture_id_value = esp_timer_get_time();

    if (capture_id_value <= *last_capture_id) {
        capture_id_value = *last_capture_id + 1;
    }
    *last_capture_id = capture_id_value;

    snprintf(capture_id, capture_id_len, "%lld", (long long) capture_id_value);
    if (timestamp_ms_out != NULL) {
        *timestamp_ms_out = capture_id_value / 1000LL;
    }
}

bool storage_csv_read_field(char **cursor, char *out, size_t out_len)
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

esp_err_t storage_ensure_ready(void)
{
    if (ensure_dir(DATASET_DIR) != ESP_OK || ensure_dir(IMAGES_DIR) != ESP_OK ||
        ensure_metadata_file(METADATA_PATH) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void storage_refresh_dataset_counts(const char *metadata_path, collector_status_t *status)
{
    FILE *file = fopen(metadata_path, "r");
    char line[MAX_CSV_LINE_LEN];

    if (status == NULL) {
        return;
    }

    status->sample_count = 0;
    status->usable_count = 0;
    status->excluded_count = 0;
    status->last_capture_ms = 0;

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
        while (field_index < 20 && storage_csv_read_field(&cursor, field[field_index], sizeof(field[field_index]))) {
            field_index++;
            if (*cursor == '\0') {
                break;
            }
        }

        if (field_index < 12) {
            continue;
        }

        status->sample_count++;
        if (strcmp(field[10], "1") == 0 && field[11][0] == '\0') {
            status->usable_count++;
        } else {
            status->excluded_count++;
        }

        {
            int64_t timestamp_ms = strtoll(field[1], NULL, 10);
            if (timestamp_ms > status->last_capture_ms) {
                status->last_capture_ms = timestamp_ms;
            }
        }
    }

    fclose(file);
}

int64_t storage_find_latest_capture_id(const char *metadata_path)
{
    FILE *file = fopen(metadata_path, "r");
    char line[MAX_CSV_LINE_LEN];
    int64_t latest_capture_id = 0;

    if (file == NULL) {
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor = line;
        char capture_id[MAX_CAPTURE_ID_LEN] = {0};
        int64_t capture_id_value;

        if (!storage_csv_read_field(&cursor, capture_id, sizeof(capture_id))) {
            continue;
        }

        capture_id_value = strtoll(capture_id, NULL, 10);
        if (capture_id_value > latest_capture_id) {
            latest_capture_id = capture_id_value;
        }
    }

    fclose(file);
    return latest_capture_id;
}

esp_err_t storage_save_capture_locked(const capture_request_t *request,
                                      bool camera_ready,
                                      SemaphoreHandle_t camera_mutex,
                                      int64_t *last_capture_id,
                                      storage_status_update_fn update_status,
                                      char *capture_id,
                                      size_t capture_id_len,
                                      char *image_path,
                                      size_t image_path_len)
{
    int64_t timestamp_ms;
    char image_abs_path[MAX_FILE_PATH_LEN];
    camera_fb_t *fb = NULL;
    FILE *image_file = NULL;
    esp_err_t result = ESP_FAIL;

    if (!camera_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    generate_capture_id_locked(last_capture_id, capture_id, capture_id_len, &timestamp_ms);
    snprintf(image_path, image_path_len, "images/%s.jpg", capture_id);
    snprintf(image_abs_path, sizeof(image_abs_path), "%s/%s", DATASET_DIR, image_path);

    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "カメラ利用が混雑しています");
        return ESP_ERR_TIMEOUT;
    }

    fb = esp_camera_fb_get();
    xSemaphoreGive(camera_mutex);
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

    if (append_metadata_line_locked(request, fb, timestamp_ms, capture_id, image_path, update_status) != ESP_OK) {
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
