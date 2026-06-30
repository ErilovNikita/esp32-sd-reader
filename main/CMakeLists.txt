idf_component_register(
    SRCS "sd_cid_reader.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs driver led_strip
)
