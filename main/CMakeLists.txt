idf_component_register(
    SRCS
        "app_main.c"
        "bt_scan.c"
        "a2dp_src.c"
        "tone_gen.c"
        "web_server.c"
    INCLUDE_DIRS "."
    EMBED_FILES "index.html"
    REQUIRES
        bt
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        nvs_flash
        json
)
