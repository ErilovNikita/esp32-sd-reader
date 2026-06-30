idf_component_register(
    SRCS
        "sd_reader_app.c"
        "led_control.c"
        "sd_card_logic.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs driver led_strip
)
