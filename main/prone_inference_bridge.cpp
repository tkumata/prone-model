#include "prone_inference_bridge.h"

#include <list>
#include <vector>

#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "human_face_detect.hpp"

static const char *TAG = "prone_inference";
static constexpr float DETECTOR_MSR_SCORE_TH = 0.30f;
static constexpr float DETECTOR_MSR_NMS_TH = 0.45f;
static constexpr float DETECTOR_MNP_SCORE_TH = 0.35f;
static constexpr float DETECTOR_MNP_NMS_TH = 0.45f;
static constexpr float FACE_DETECTED_SCORE_TH = 0.40f;

static human_face_detect::MSRMNP *s_detector;
static prone_inference_status_t s_status = PRONE_INFERENCE_STATUS_NOT_READY;

esp_err_t prone_inference_init(void)
{
    if (s_detector != nullptr) {
        s_status = PRONE_INFERENCE_STATUS_OK;
        return ESP_OK;
    }

    s_detector = new human_face_detect::MSRMNP("human_face_detect_msr_s8_v1.espdl",
                                               human_face_detect::MSR::default_score_thr,
                                               human_face_detect::MSR::default_nms_thr,
                                               "human_face_detect_mnp_s8_v1.espdl",
                                               human_face_detect::MNP::default_score_thr,
                                               human_face_detect::MNP::default_nms_thr);
    if (s_detector == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_ERR_NO_MEM;
    }

    s_detector->set_score_thr(DETECTOR_MSR_SCORE_TH, 0);
    s_detector->set_nms_thr(DETECTOR_MSR_NMS_TH, 0);
    s_detector->set_score_thr(DETECTOR_MNP_SCORE_TH, 1);
    s_detector->set_nms_thr(DETECTOR_MNP_NMS_TH, 1);

    s_status = PRONE_INFERENCE_STATUS_OK;
    ESP_LOGI(TAG, "顔検出モデル読み込み完了");
    return ESP_OK;
}

esp_err_t prone_inference_run_jpeg(const uint8_t *jpeg_data,
                                   size_t jpeg_len,
                                   bool *is_face_detected,
                                   float *confidence,
                                   int *face_count,
                                   prone_face_box_t *out_box)
{
    if (jpeg_data == nullptr || jpeg_len == 0 || is_face_detected == nullptr || confidence == nullptr ||
        face_count == nullptr || out_box == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_detector == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_NOT_READY;
        return ESP_ERR_INVALID_STATE;
    }

    dl::image::jpeg_img_t jpeg = {
        .data = (void *)jpeg_data,
        .data_len = jpeg_len,
    };
    dl::image::img_t rgb = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (rgb.data == nullptr) {
        s_status = PRONE_INFERENCE_STATUS_FAULT;
        return ESP_FAIL;
    }

    std::list<dl::detect::result_t> &result = s_detector->run(rgb);
    *face_count = (int)result.size();

    float best = 0.0f;
    int best_x0 = -1;
    int best_y0 = -1;
    int best_x1 = -1;
    int best_y1 = -1;
    std::vector<int> best_keypoint;

    for (const auto &r : result) {
        if (r.score > best) {
            best = r.score;
            if (r.box.size() >= 4) {
                best_x0 = r.box[0];
                best_y0 = r.box[1];
                best_x1 = r.box[2];
                best_y1 = r.box[3];
            }
            best_keypoint = r.keypoint;
        }
    }

    const bool score_passed = best >= FACE_DETECTED_SCORE_TH;
    const bool box_valid = (best_x0 >= 0) && (best_y0 >= 0) && (best_x1 > best_x0) && (best_y1 > best_y0);

    *confidence = best;
    *is_face_detected = score_passed && box_valid;

    out_box->x0 = best_x0;
    out_box->y0 = best_y0;
    out_box->x1 = best_x1;
    out_box->y1 = best_y1;
    out_box->confidence = best;
    out_box->valid = *is_face_detected;

    if (best_keypoint.size() >= PRONE_LANDMARK_COUNT * 2) {
        for (int i = 0; i < PRONE_LANDMARK_COUNT * 2; ++i) {
            out_box->landmarks[i] = best_keypoint[i];
        }
        out_box->landmarks_valid = out_box->valid;
    } else {
        memset(out_box->landmarks, 0, sizeof(out_box->landmarks));
        out_box->landmarks_valid = false;
    }

    heap_caps_free(rgb.data);
    s_status = PRONE_INFERENCE_STATUS_OK;
    return ESP_OK;
}

prone_inference_status_t prone_inference_get_status(void)
{
    return s_status;
}

const char *prone_inference_status_to_string(prone_inference_status_t status)
{
    switch (status) {
    case PRONE_INFERENCE_STATUS_NOT_READY:
        return "not_ready";
    case PRONE_INFERENCE_STATUS_OK:
        return "ok";
    case PRONE_INFERENCE_STATUS_MODEL_MISSING:
        return "model_missing";
    case PRONE_INFERENCE_STATUS_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}
