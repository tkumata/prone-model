#ifndef FACE_DETECTION_H
#define FACE_DETECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_FACE_DETECTION_BOXES 8

typedef struct {
    int x;
    int y;
    int width;
    int height;
    float score;
} face_detection_box_t;

typedef struct {
    bool detector_ready;
    int frame_width;
    int frame_height;
    size_t box_count;
    int64_t updated_at_ms;
    face_detection_box_t boxes[MAX_FACE_DETECTION_BOXES];
} face_detection_result_t;

esp_err_t face_detection_init(void);
void face_detection_deinit(void);
void face_detection_result_reset(face_detection_result_t *result, int frame_width, int frame_height, int64_t updated_at_ms);
esp_err_t face_detection_run_jpeg(const uint8_t *jpeg_data,
                                  size_t jpeg_len,
                                  int frame_width,
                                  int frame_height,
                                  int64_t updated_at_ms,
                                  face_detection_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
