#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_camera.h"
#include "esp_log.h"

#include "storage_csv.h"
#include "validation.h"
#include "web_service.h"
#include "web_ui.h"

#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=frame"
#define STREAM_BOUNDARY "\r\n--frame\r\n"
#define STREAM_PART "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

static const char *TAG = "web_service";
static const char *HTTP_STATUS_400 = "400 Bad Request";
static const char *HTTP_STATUS_409 = "409 Conflict";
static const char *HTTP_STATUS_500 = "500 Internal Server Error";
static const char *HTTP_STATUS_503 = "503 Service Unavailable";

static const char *http_status_text(httpd_err_code_t status)
{
    switch (status) {
        case HTTPD_400_BAD_REQUEST:
            return HTTP_STATUS_400;
        case HTTPD_500_INTERNAL_SERVER_ERROR:
            return HTTP_STATUS_500;
        default:
            return HTTP_STATUS_500;
    }
}

static esp_err_t send_json_error(httpd_req_t *req, httpd_err_code_t status, const char *message)
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

    httpd_resp_set_status(req, http_status_text(status));
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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
    web_service_context_t *context = req->user_ctx;
    collector_status_t status_snapshot;
    cJSON *root = cJSON_CreateObject();
    char *body;
    esp_err_t err;

    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
    }

    context->copy_status_snapshot(&status_snapshot);
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
    web_service_context_t *context = req->user_ctx;
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

    context->copy_runtime_snapshot(&runtime_snapshot);
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

    if (xSemaphoreTake(context->storage_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return send_json_error_with_status(req, HTTP_STATUS_503, "保存処理が混雑しています");
    }

    context->generate_capture_id(capture_id, sizeof(capture_id), &timestamp_ms);
    err = storage_save_capture_locked(context->storage_context,
                                      &request,
                                      runtime_snapshot.camera_ready,
                                      context->camera_mutex,
                                      context->update_capture_status,
                                      timestamp_ms,
                                      capture_id,
                                      image_path,
                                      sizeof(image_path));
    xSemaphoreGive(context->storage_mutex);
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
    web_service_context_t *context = req->user_ctx;
    app_runtime_t runtime_snapshot;

    context->copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.sdcard_ready || !runtime_snapshot.metadata_ready) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sdcard error");
    }
    if (runtime_snapshot.storage_resetting) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "busy");
    }
    return send_file(req, context->storage_context->metadata_path, "text/csv");
}

static esp_err_t image_handler(httpd_req_t *req)
{
    web_service_context_t *context = req->user_ctx;
    app_runtime_t runtime_snapshot;
    char capture_id[MAX_CAPTURE_ID_LEN];
    char path[128];

    context->copy_runtime_snapshot(&runtime_snapshot);
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

    snprintf(path, sizeof(path), "%s/%s.jpg", context->storage_context->images_dir, capture_id);
    return send_file(req, path, "image/jpeg");
}

static esp_err_t manifest_handler(httpd_req_t *req)
{
    web_service_context_t *context = req->user_ctx;
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

    context->copy_runtime_snapshot(&runtime_snapshot);
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

    file = fopen(context->storage_context->metadata_path, "r");
    if (file == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metadata error");
    }

    fgets(line, sizeof(line), file);
    while (fgets(line, sizeof(line), file) != NULL) {
        if (current_index >= start_index && current_index < end_index) {
            storage_manifest_record_t record;
            cJSON *item = cJSON_CreateObject();

            if (item == NULL) {
                fclose(file);
                cJSON_Delete(root);
                cJSON_Delete(items);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json error");
            }
            if (!storage_csv_parse_manifest_record(line, &record)) {
                cJSON_Delete(item);
                continue;
            }

            cJSON_AddStringToObject(item, "capture_id", record.capture_id);
            cJSON_AddStringToObject(item, "image_path", record.image_path);
            cJSON_AddNumberToObject(item, "timestamp_ms", (double) record.timestamp_ms);
            cJSON_AddNumberToObject(item, "label", record.label);
            cJSON_AddItemToArray(items, item);
        }
        current_index++;
    }
    fclose(file);

    context->copy_status_snapshot(&status_snapshot);
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
    web_service_context_t *context = req->user_ctx;
    app_runtime_t runtime_snapshot;
    cJSON *root = NULL;
    char confirm[16];
    DIR *dir;
    struct dirent *entry;
    cJSON *response = NULL;
    char *body = NULL;
    esp_err_t err;

    context->copy_runtime_snapshot(&runtime_snapshot);
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

    context->set_storage_resetting(true);
    if (xSemaphoreTake(context->storage_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        context->set_storage_resetting(false);
        return send_json_error_with_status(req, HTTP_STATUS_503, "リセット処理が混雑しています");
    }

    dir = opendir(context->storage_context->images_dir);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            char file_path[MAX_FILE_PATH_LEN];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            snprintf(file_path, sizeof(file_path), "%s/%s", context->storage_context->images_dir, entry->d_name);
            unlink(file_path);
        }
        closedir(dir);
    }

    unlink(context->storage_context->metadata_path);
    err = storage_ensure_ready(context->storage_context);
    if (err == ESP_OK) {
        context->reset_status_counts();
        context->set_last_capture_id(0);
        context->set_storage_ready(true, true);
    } else {
        context->set_storage_ready(false, false);
    }
    xSemaphoreGive(context->storage_mutex);
    context->set_storage_resetting(false);

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
    web_service_context_t *context = req->user_ctx;
    app_runtime_t runtime_snapshot;
    char part_buf[64];
    camera_fb_t *fb = NULL;
    esp_err_t err = ESP_OK;

    context->copy_runtime_snapshot(&runtime_snapshot);
    if (!runtime_snapshot.camera_ready) {
        return send_text_error_with_status(req, HTTP_STATUS_503, "camera error");
    }

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    while (true) {
        if (xSemaphoreTake(context->camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        fb = esp_camera_fb_get();
        xSemaphoreGive(context->camera_mutex);
        if (fb == NULL) {
            err = ESP_FAIL;
            break;
        }

        if ((err = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY))) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        {
            int header_len = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
            if ((err = httpd_resp_send_chunk(req, part_buf, header_len)) != ESP_OK) {
                esp_camera_fb_return(fb);
                break;
            }
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

esp_err_t web_service_start(web_service_context_t *context)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = context};
    httpd_uri_t uri_status = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = context};
    httpd_uri_t uri_capture = {.uri = "/api/capture", .method = HTTP_POST, .handler = capture_handler, .user_ctx = context};
    httpd_uri_t uri_manifest = {.uri = "/api/export/manifest", .method = HTTP_GET, .handler = manifest_handler, .user_ctx = context};
    httpd_uri_t uri_metadata = {.uri = "/api/export/metadata", .method = HTTP_GET, .handler = metadata_handler, .user_ctx = context};
    httpd_uri_t uri_image = {.uri = "/api/export/image", .method = HTTP_GET, .handler = image_handler, .user_ctx = context};
    httpd_uri_t uri_reset = {.uri = "/api/reset", .method = HTTP_POST, .handler = reset_handler, .user_ctx = context};
    esp_err_t err;

    config.max_uri_handlers = 12;
    config.server_port = 80;
    config.stack_size = 12288;
    config.max_open_sockets = 4;
    config.backlog_conn = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    err = httpd_start(context->server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Web サーバ起動に失敗しました: %s", esp_err_to_name(err));
        *context->server = NULL;
        return err;
    }

    httpd_register_uri_handler(*context->server, &uri_root);
    httpd_register_uri_handler(*context->server, &uri_status);
    httpd_register_uri_handler(*context->server, &uri_capture);
    httpd_register_uri_handler(*context->server, &uri_manifest);
    httpd_register_uri_handler(*context->server, &uri_metadata);
    httpd_register_uri_handler(*context->server, &uri_image);
    httpd_register_uri_handler(*context->server, &uri_reset);
    ESP_LOGI(TAG, "Web サーバを起動しました");
    return ESP_OK;
}

esp_err_t web_service_start_stream(web_service_context_t *context)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uri_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = context};
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

    err = httpd_start(context->stream_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ストリームサーバ起動に失敗しました: %s", esp_err_to_name(err));
        *context->stream_server = NULL;
        return err;
    }

    httpd_register_uri_handler(*context->stream_server, &uri_stream);
    ESP_LOGI(TAG, "ストリームサーバを起動しました");
    return ESP_OK;
}
