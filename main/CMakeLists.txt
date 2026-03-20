idf_component_register(
    SRCS "web_ui.c" "main.c" "validation.c" "storage.c" "storage_csv.c" "web_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_event esp_netif nvs_flash fatfs json esp_timer
    EMBED_TXTFILES "web/index.html" "web/app.css" "web/app.js"
)
