#ifndef WEB_SERVICE_H
#define WEB_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include "storage.h"

typedef void (*web_copy_status_snapshot_fn)(collector_status_t *out_status);
typedef void (*web_copy_runtime_snapshot_fn)(app_runtime_t *out_runtime);
typedef void (*web_update_capture_status_fn)(int is_usable_for_training, int64_t timestamp_ms);
typedef void (*web_generate_capture_id_fn)(char *capture_id, size_t capture_id_len, int64_t *timestamp_ms_out);
typedef void (*web_set_storage_resetting_fn)(bool resetting);
typedef void (*web_set_last_capture_id_fn)(int64_t capture_id);
typedef void (*web_set_storage_ready_fn)(bool sdcard_ready, bool metadata_ready);
typedef void (*web_reset_status_counts_fn)(void);

typedef struct {
    httpd_handle_t *server;
    httpd_handle_t *stream_server;
    SemaphoreHandle_t storage_mutex;
    SemaphoreHandle_t camera_mutex;
    const storage_context_t *storage_context;
    web_copy_status_snapshot_fn copy_status_snapshot;
    web_copy_runtime_snapshot_fn copy_runtime_snapshot;
    web_update_capture_status_fn update_capture_status;
    web_generate_capture_id_fn generate_capture_id;
    web_set_storage_resetting_fn set_storage_resetting;
    web_set_last_capture_id_fn set_last_capture_id;
    web_set_storage_ready_fn set_storage_ready;
    web_reset_status_counts_fn reset_status_counts;
} web_service_context_t;

esp_err_t web_service_start(web_service_context_t *context);
esp_err_t web_service_start_stream(web_service_context_t *context);

#endif
