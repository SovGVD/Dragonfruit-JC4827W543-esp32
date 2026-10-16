idf_component_register(
    SRCS "wifi_gateway.c" "portal_protocol.c" "web_proxy.c" "web_proxy_http.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip esp_http_client mbedtls
)
