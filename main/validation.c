#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "validation.h"

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

bool is_digits_only(const char *value)
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
    if (value == NULL || value[0] == '\0') {
        return true;
    }

    for (size_t i = 0; i < sizeof(allowed_exclude_reasons) / sizeof(allowed_exclude_reasons[0]); ++i) {
        if (strcmp(value, allowed_exclude_reasons[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t validate_capture_request(const capture_request_t *request, char *reason, size_t reason_len)
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
