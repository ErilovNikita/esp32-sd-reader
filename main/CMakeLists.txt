idf_component_register(
    SRCS
        "app/sd_reader_app.c"
        "led/led_control.c"
        "sd/sd_card_logic.c"
        "web/web_status.c"
        "web/web_server.c"
    INCLUDE_DIRS
        "."
        "config"
        "led"
        "sd"
        "web"
    EMBED_TXTFILES "web/web_status_template.html"
    REQUIRES fatfs driver led_strip esp_wifi esp_http_server esp_netif esp_event nvs_flash
)
