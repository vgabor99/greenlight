idf_component_register(SRCS "hello_world_main.c" "epd.c" "zb.c" "render.c"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio nvs_flash esp_timer esp_app_format u8g2
                       INCLUDE_DIRS ".")
