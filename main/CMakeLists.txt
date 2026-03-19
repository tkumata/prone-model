idf_component_register(
    SRCS "main.c" "validation.c" "storage.c" "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_event esp_netif nvs_flash fatfs json esp_timer
)
