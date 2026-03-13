#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PRONE_INFERENCE_STATUS_NOT_READY = 0,
    PRONE_INFERENCE_STATUS_OK,
    PRONE_INFERENCE_STATUS_MODEL_MISSING,
    PRONE_INFERENCE_STATUS_FAULT,
} prone_inference_status_t;

#define PRONE_LANDMARK_COUNT 5
#define PRONE_LM_LEFT_EYE 0
#define PRONE_LM_RIGHT_EYE 1
#define PRONE_LM_NOSE 2
#define PRONE_LM_LEFT_MOUTH 3
#define PRONE_LM_RIGHT_MOUTH 4

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
    float confidence;
    bool valid;
    int landmarks[PRONE_LANDMARK_COUNT * 2];
    bool landmarks_valid;
} prone_face_box_t;

esp_err_t prone_inference_init(void);
esp_err_t prone_inference_run_jpeg(const uint8_t *jpeg_data,
                                   size_t jpeg_len,
                                   bool *is_face_detected,
                                   float *confidence,
                                   int *face_count,
                                   prone_face_box_t *out_box);
prone_inference_status_t prone_inference_get_status(void);
const char *prone_inference_status_to_string(prone_inference_status_t status);

#ifdef __cplusplus
}
#endif
