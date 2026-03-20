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

typedef struct {
    const char *error_message;
    bool (*validator)(const capture_request_t *request);
} request_validation_rule_t;
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

static bool has_prefix(const char *value, const char *prefix)
{
    return value != NULL && prefix != NULL && strncmp(value, prefix, strlen(prefix)) == 0;
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
    return is_valid_generic_token(value) &&
           has_prefix(value, "baby_") &&
           strlen(value) >= 8 &&
           is_digits_only(value + 5);
}

static bool is_valid_prefixed_token(const char *value, const char *prefix, size_t min_length)
{
    return is_valid_generic_token(value) &&
           has_prefix(value, prefix) &&
           strlen(value) >= min_length;
}

static bool is_valid_location_id(const char *value)
{
    return is_valid_prefixed_token(value, "loc_", 6);
}

static bool is_valid_lighting_id(const char *value)
{
    return is_valid_prefixed_token(value, "light_", 7);
}

static bool is_valid_camera_position_id(const char *value)
{
    return is_valid_prefixed_token(value, "campos_", 8);
}

static bool is_valid_annotator_id(const char *value)
{
    return is_valid_prefixed_token(value, "ann_", 5);
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

static bool is_valid_label_value(int value)
{
    return value == 0 || value == 1;
}

static bool is_valid_training_flag_value(int value)
{
    return value == 0 || value == 1;
}

static bool contains_newline(const char *value)
{
    if (value == NULL) {
        return false;
    }

    for (size_t i = 0; value[i] != '\0'; ++i) {
        if (value[i] == '\n' || value[i] == '\r') {
            return true;
        }
    }
    return false;
}

static bool validate_subject_rule(const capture_request_t *request)
{
    return is_valid_subject_id(request->subject_id);
}

static bool validate_session_rule(const capture_request_t *request)
{
    return is_valid_session_id(request->subject_id, request->session_id);
}

static bool validate_location_rule(const capture_request_t *request)
{
    return is_valid_location_id(request->location_id);
}

static bool validate_lighting_rule(const capture_request_t *request)
{
    return is_valid_lighting_id(request->lighting_id);
}

static bool validate_camera_position_rule(const capture_request_t *request)
{
    return is_valid_camera_position_id(request->camera_position_id);
}

static bool validate_annotator_rule(const capture_request_t *request)
{
    return is_valid_annotator_id(request->annotator_id);
}

static bool validate_label_rule(const capture_request_t *request)
{
    return is_valid_label_value(request->label);
}

static bool validate_training_flag_rule(const capture_request_t *request)
{
    return is_valid_training_flag_value(request->is_usable_for_training);
}

static bool validate_exclude_reason_rule(const capture_request_t *request)
{
    return is_valid_exclude_reason(request->exclude_reason);
}

static bool validate_notes_rule(const capture_request_t *request)
{
    return !contains_newline(request->notes);
}

static bool validate_usable_reason_consistency_rule(const capture_request_t *request)
{
    return request->is_usable_for_training != 1 || request->exclude_reason[0] == '\0';
}

static bool validate_excluded_reason_consistency_rule(const capture_request_t *request)
{
    return request->is_usable_for_training != 0 || request->exclude_reason[0] != '\0';
}

esp_err_t validate_capture_request(const capture_request_t *request, char *reason, size_t reason_len)
{
    static const request_validation_rule_t rules[] = {
        {.error_message = "subject_id が不正です", .validator = validate_subject_rule},
        {.error_message = "session_id が不正です", .validator = validate_session_rule},
        {.error_message = "location_id が不正です", .validator = validate_location_rule},
        {.error_message = "lighting_id が不正です", .validator = validate_lighting_rule},
        {.error_message = "camera_position_id が不正です", .validator = validate_camera_position_rule},
        {.error_message = "annotator_id が不正です", .validator = validate_annotator_rule},
        {.error_message = "label は 0 または 1 が必要です", .validator = validate_label_rule},
        {.error_message = "is_usable_for_training は 0 または 1 が必要です", .validator = validate_training_flag_rule},
        {.error_message = "exclude_reason が不正です", .validator = validate_exclude_reason_rule},
        {.error_message = "notes に改行は使用できません", .validator = validate_notes_rule},
        {.error_message = "学習利用可のとき exclude_reason は空にしてください", .validator = validate_usable_reason_consistency_rule},
        {.error_message = "学習利用不可のとき exclude_reason は必須です", .validator = validate_excluded_reason_consistency_rule},
    };

    if (request == NULL || reason == NULL || reason_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
        if (!rules[i].validator(request)) {
            snprintf(reason, reason_len, "%s", rules[i].error_message);
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}
