#ifndef VALIDATION_H
#define VALIDATION_H

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "esp_err.h"

bool is_digits_only(const char *value);
esp_err_t validate_capture_request(const capture_request_t *request, char *reason, size_t reason_len);

#endif
