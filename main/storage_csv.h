#ifndef STORAGE_CSV_H
#define STORAGE_CSV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    char capture_id[MAX_CAPTURE_ID_LEN];
    char image_path[MAX_IMAGE_PATH_LEN];
    int64_t timestamp_ms;
    int label;
} storage_manifest_record_t;

bool storage_csv_read_field(char **cursor, char *out, size_t out_len);
esp_err_t storage_csv_write_escaped_field(FILE *file, const char *value);
bool storage_csv_parse_manifest_record(char *line, storage_manifest_record_t *out_record);

#endif
