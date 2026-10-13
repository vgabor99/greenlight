idf_component_register(SRCS "hello_world_main.c" "epd.c" "zb.c"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio nvs_flash esp_timer esp_app_format
                       INCLUDE_DIRS ".")
