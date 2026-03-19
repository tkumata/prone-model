#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_csv.h"

enum {
    MANIFEST_SKIP_AFTER_TIMESTAMP = 6,
    MANIFEST_SKIP_AFTER_LABEL = 4,
};

static bool storage_csv_skip_fields(char **cursor, int count)
{
    char discard[MAX_NOTES_LEN];

    for (int i = 0; i < count; ++i) {
        if (!storage_csv_read_field(cursor, discard, sizeof(discard))) {
            return false;
        }
    }

    return true;
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

esp_err_t storage_csv_write_escaped_field(FILE *file, const char *value)
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

bool storage_csv_parse_manifest_record(char *line, storage_manifest_record_t *out_record)
{
    char *cursor = line;
    char timestamp_ms[32] = {0};
    char label[8] = {0};

    if (out_record == NULL) {
        return false;
    }

    memset(out_record, 0, sizeof(*out_record));
    if (!storage_csv_read_field(&cursor, out_record->capture_id, sizeof(out_record->capture_id)) ||
        !storage_csv_read_field(&cursor, timestamp_ms, sizeof(timestamp_ms))) {
        return false;
    }

    if (!storage_csv_skip_fields(&cursor, MANIFEST_SKIP_AFTER_TIMESTAMP)) {
        return false;
    }
    if (!storage_csv_read_field(&cursor, label, sizeof(label))) {
        return false;
    }
    if (!storage_csv_skip_fields(&cursor, MANIFEST_SKIP_AFTER_LABEL)) {
        return false;
    }
    if (!storage_csv_read_field(&cursor, out_record->image_path, sizeof(out_record->image_path))) {
        return false;
    }

    out_record->timestamp_ms = strtoll(timestamp_ms, NULL, 10);
    out_record->label = atoi(label);
    return true;
}
