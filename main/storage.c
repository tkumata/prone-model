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

#include "storage_csv.h"
#include "storage.h"

static const char *TAG = "storage_service";

static const char *METADATA_HEADER =
    "capture_id,timestamp_ms,subject_id,session_id,location_id,lighting_id,"
    "camera_position_id,annotator_id,label,label_name,is_usable_for_training,"
    "exclude_reason,notes,image_path,image_bytes,frame_width,frame_height,"
    "pixel_format,jpeg_quality,board_name\n";

#define STORAGE_COUNT_FIELD_WIDTH (MAX_NOTES_LEN > MAX_ID_LEN ? MAX_NOTES_LEN : MAX_ID_LEN)
#define STORAGE_COUNT_FIELD_COUNT 20
#define STORAGE_USABLE_FIELD_INDEX 10
#define STORAGE_EXCLUDE_REASON_FIELD_INDEX 11
#define STORAGE_TIMESTAMP_FIELD_INDEX 1

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

static bool flush_truncated_line(FILE *file, char *line)
{
    if (strchr(line, '\n') != NULL || feof(file)) {
        return false;
    }

    while (true) {
        int ch = fgetc(file);
        if (ch == '\n' || ch == EOF) {
            break;
        }
    }
    return true;
}

static bool metadata_row_is_usable(char fields[STORAGE_COUNT_FIELD_COUNT][STORAGE_COUNT_FIELD_WIDTH])
{
    return strcmp(fields[STORAGE_USABLE_FIELD_INDEX], "1") == 0 &&
           fields[STORAGE_EXCLUDE_REASON_FIELD_INDEX][0] == '\0';
}

static bool parse_metadata_fields(char *line,
                                  char fields[STORAGE_COUNT_FIELD_COUNT][STORAGE_COUNT_FIELD_WIDTH],
                                  int *field_count_out)
{
    char *cursor = line;
    int field_count = 0;

    memset(fields, 0, sizeof(char[STORAGE_COUNT_FIELD_COUNT][STORAGE_COUNT_FIELD_WIDTH]));
    while (field_count < STORAGE_COUNT_FIELD_COUNT &&
           storage_csv_read_field(&cursor, fields[field_count], STORAGE_COUNT_FIELD_WIDTH)) {
        field_count++;
        if (*cursor == '\0') {
            break;
        }
    }

    *field_count_out = field_count;
    return field_count > 0;
}

static void reset_dataset_counts(collector_status_t *status)
{
    status->sample_count = 0;
    status->usable_count = 0;
    status->excluded_count = 0;
    status->last_capture_ms = 0;
}

static void update_counts_from_metadata_fields(collector_status_t *status,
                                               char fields[STORAGE_COUNT_FIELD_COUNT][STORAGE_COUNT_FIELD_WIDTH],
                                               int field_count)
{
    int64_t timestamp_ms;

    if (field_count <= STORAGE_EXCLUDE_REASON_FIELD_INDEX) {
        return;
    }

    status->sample_count++;
    if (metadata_row_is_usable(fields)) {
        status->usable_count++;
    } else {
        status->excluded_count++;
    }

    timestamp_ms = strtoll(fields[STORAGE_TIMESTAMP_FIELD_INDEX], NULL, 10);
    if (timestamp_ms > status->last_capture_ms) {
        status->last_capture_ms = timestamp_ms;
    }
}

static esp_err_t append_metadata_line_locked(const capture_request_t *request,
                                             const storage_context_t *context,
                                             const camera_fb_t *fb,
                                             int64_t timestamp_ms,
                                             const char *capture_id,
                                             const char *image_path,
                                             storage_status_update_fn update_status)
{
    FILE *file = fopen(context->metadata_path, "a");

    if (file == NULL) {
        return ESP_FAIL;
    }

    if (storage_csv_write_escaped_field(file, capture_id) != ESP_OK ||
        fprintf(file, ",%lld,", (long long) timestamp_ms) < 0 ||
        storage_csv_write_escaped_field(file, request->subject_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->session_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->location_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->lighting_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->camera_position_id) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->annotator_id) != ESP_OK ||
        fprintf(file, ",%d,", request->label) < 0 ||
        storage_csv_write_escaped_field(file, label_name_from_value(request->label)) != ESP_OK ||
        fprintf(file, ",%d,", request->is_usable_for_training) < 0 ||
        storage_csv_write_escaped_field(file, request->exclude_reason) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, request->notes) != ESP_OK ||
        fputc(',', file) == EOF ||
        storage_csv_write_escaped_field(file, image_path) != ESP_OK ||
        fprintf(file, ",%u,%u,%u,", (unsigned int) fb->len, (unsigned int) fb->width, (unsigned int) fb->height) < 0 ||
        storage_csv_write_escaped_field(file, pixel_format_name(fb->format)) != ESP_OK ||
        fprintf(file, ",%d,", CAMERA_JPEG_QUALITY) < 0 ||
        storage_csv_write_escaped_field(file, context->board_name) != ESP_OK ||
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

esp_err_t storage_ensure_ready(const storage_context_t *context)
{
    if (context == NULL || ensure_dir(context->dataset_dir) != ESP_OK ||
        ensure_dir(context->images_dir) != ESP_OK ||
        ensure_metadata_file(context->metadata_path) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void storage_refresh_dataset_counts(const storage_context_t *context, collector_status_t *status)
{
    FILE *file;
    char line[MAX_CSV_LINE_LEN];
    char fields[STORAGE_COUNT_FIELD_COUNT][STORAGE_COUNT_FIELD_WIDTH];
    int field_count;

    if (status == NULL || context == NULL) {
        return;
    }

    file = fopen(context->metadata_path, "r");
    reset_dataset_counts(status);

    if (file == NULL) {
        return;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (flush_truncated_line(file, line)) {
            continue;
        }
        if (!parse_metadata_fields(line, fields, &field_count)) {
            continue;
        }
        update_counts_from_metadata_fields(status, fields, field_count);
    }

    fclose(file);
}

int64_t storage_find_latest_capture_id(const storage_context_t *context)
{
    FILE *file;
    char line[MAX_CSV_LINE_LEN];
    int64_t latest_capture_id = 0;

    if (context == NULL) {
        return 0;
    }

    file = fopen(context->metadata_path, "r");
    if (file == NULL) {
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (flush_truncated_line(file, line)) {
            continue;
        }

        char capture_id[MAX_CAPTURE_ID_LEN] = {0};
        int64_t capture_id_value;
        char *cursor = line;

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

esp_err_t storage_save_capture_locked(const storage_context_t *context,
                                      const capture_request_t *request,
                                      bool camera_ready,
                                      SemaphoreHandle_t camera_mutex,
                                      storage_status_update_fn update_status,
                                      int64_t timestamp_ms,
                                      const char *capture_id,
                                      char *image_path,
                                      size_t image_path_len)
{
    char image_abs_path[MAX_FILE_PATH_LEN];
    camera_fb_t *fb = NULL;
    FILE *image_file = NULL;
    esp_err_t result = ESP_FAIL;

    if (context == NULL || !camera_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(image_path, image_path_len, "images/%s.jpg", capture_id);
    snprintf(image_abs_path, sizeof(image_abs_path), "%s/%s", context->dataset_dir, image_path);

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

    if (append_metadata_line_locked(request, context, fb, timestamp_ms, capture_id, image_path, update_status) != ESP_OK) {
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
