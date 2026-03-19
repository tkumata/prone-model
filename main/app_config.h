#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define BOARD_NAME "Freenove ESP32-S3 WROOM CAM"
#define MOUNT_POINT "/sdcard"
#define DATASET_DIR MOUNT_POINT "/dataset"
#define IMAGES_DIR DATASET_DIR "/images"
#define METADATA_PATH DATASET_DIR "/metadata.csv"

#define MAX_HTTP_BODY_SIZE 4096
#define MAX_ID_LEN 64
#define MAX_NOTES_LEN 256
#define MAX_CSV_LINE_LEN 1024
#define MAX_CAPTURE_ID_LEN 40
#define MAX_IMAGE_PATH_LEN 96
#define MAX_FILE_PATH_LEN 384

#define DEFAULT_PAGE_SIZE 50
#define MAX_PAGE_SIZE 100
#define COLLECTOR_MAIN_TASK_STACK_SIZE 12288

#define CAMERA_JPEG_QUALITY 12

typedef struct {
    char subject_id[MAX_ID_LEN];
    char session_id[MAX_ID_LEN];
    char location_id[MAX_ID_LEN];
    char lighting_id[MAX_ID_LEN];
    char camera_position_id[MAX_ID_LEN];
    char annotator_id[MAX_ID_LEN];
    char exclude_reason[32];
    char notes[MAX_NOTES_LEN];
    int label;
    int is_usable_for_training;
} capture_request_t;

typedef struct {
    char wifi[16];
    char camera[16];
    char sdcard[16];
    int sample_count;
    int usable_count;
    int excluded_count;
    int64_t last_capture_ms;
} collector_status_t;

#endif
