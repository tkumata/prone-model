#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef void (*storage_status_update_fn)(int is_usable_for_training, int64_t timestamp_ms);

bool storage_csv_read_field(char **cursor, char *out, size_t out_len);
esp_err_t storage_ensure_ready(void);
void storage_refresh_dataset_counts(const char *metadata_path, collector_status_t *status);
int64_t storage_find_latest_capture_id(const char *metadata_path);
esp_err_t storage_save_capture_locked(const capture_request_t *request,
                                      bool camera_ready,
                                      SemaphoreHandle_t camera_mutex,
                                      storage_status_update_fn update_status,
                                      int64_t timestamp_ms,
                                      const char *capture_id,
                                      char *image_path,
                                      size_t image_path_len);

#endif
