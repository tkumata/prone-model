#include "face_detection.h"

#include <cstdlib>
#include <new>

#include "dl_detect_define.hpp"
#include "dl_image_define.hpp"
#include "esp_log.h"
#include "human_face_detect.hpp"
#include "img_converters.h"

namespace {
constexpr const char *TAG = "face_detection";

HumanFaceDetect *g_detector = nullptr;

esp_err_t ensure_detector()
{
    if (g_detector != nullptr) {
        return ESP_OK;
    }

    g_detector = new (std::nothrow) HumanFaceDetect();
    if (g_detector == nullptr) {
        ESP_LOGE(TAG, "顔検出モデルの初期化に失敗しました");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
} // namespace

extern "C" esp_err_t face_detection_init(void)
{
    return ensure_detector();
}

extern "C" void face_detection_deinit(void)
{
    delete g_detector;
    g_detector = nullptr;
}

extern "C" void face_detection_result_reset(face_detection_result_t *result,
                                            int frame_width,
                                            int frame_height,
                                            int64_t updated_at_ms)
{
    size_t index;

    if (result == nullptr) {
        return;
    }

    result->detector_ready = g_detector != nullptr;
    result->frame_width = frame_width;
    result->frame_height = frame_height;
    result->box_count = 0;
    result->updated_at_ms = updated_at_ms;
    for (index = 0; index < MAX_FACE_DETECTION_BOXES; index++) {
        result->boxes[index].x = 0;
        result->boxes[index].y = 0;
        result->boxes[index].width = 0;
        result->boxes[index].height = 0;
        result->boxes[index].score = 0.0f;
    }
}

extern "C" esp_err_t face_detection_run_jpeg(const uint8_t *jpeg_data,
                                             size_t jpeg_len,
                                             int frame_width,
                                             int frame_height,
                                             int64_t updated_at_ms,
                                             face_detection_result_t *out_result)
{
    uint8_t *rgb888_buffer = nullptr;
    dl::image::img_t image = {};
    std::list<dl::detect::result_t> *detections;
    size_t box_index = 0;
    esp_err_t err;

    if (jpeg_data == nullptr || jpeg_len == 0 || out_result == nullptr || frame_width <= 0 || frame_height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_detector();
    face_detection_result_reset(out_result, frame_width, frame_height, updated_at_ms);
    if (err != ESP_OK) {
        return err;
    }
    out_result->detector_ready = true;

    rgb888_buffer = static_cast<uint8_t *>(malloc(static_cast<size_t>(frame_width) * static_cast<size_t>(frame_height) * 3U));
    if (rgb888_buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (!fmt2rgb888(jpeg_data, jpeg_len, PIXFORMAT_JPEG, rgb888_buffer)) {
        ESP_LOGW(TAG, "JPEG から RGB888 への変換に失敗しました");
        free(rgb888_buffer);
        return ESP_FAIL;
    }

    image.data = rgb888_buffer;
    image.width = static_cast<uint16_t>(frame_width);
    image.height = static_cast<uint16_t>(frame_height);
    image.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

    detections = &g_detector->run(image);
    for (const dl::detect::result_t &detection : *detections) {
        int left;
        int top;
        int right;
        int bottom;

        if (detection.box.size() < 4 || box_index >= MAX_FACE_DETECTION_BOXES) {
            continue;
        }

        left = detection.box[0];
        top = detection.box[1];
        right = detection.box[2];
        bottom = detection.box[3];
        if (right <= left || bottom <= top) {
            continue;
        }

        out_result->boxes[box_index].x = left;
        out_result->boxes[box_index].y = top;
        out_result->boxes[box_index].width = right - left;
        out_result->boxes[box_index].height = bottom - top;
        out_result->boxes[box_index].score = detection.score;
        box_index++;
    }

    out_result->box_count = box_index;
    free(rgb888_buffer);
    return ESP_OK;
}
