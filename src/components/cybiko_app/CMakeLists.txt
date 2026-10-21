idf_component_register(
    SRCS "cybiko_files.c" "cybiko_app.c" "cybiko_ui.c" "cybiko_link.c"
    INCLUDE_DIRS "include"
    REQUIRES m5_sd display cybiko
    PRIV_REQUIRES input board config audio esp_timer esp_wifi esp_netif esp_event nvs_flash
)
