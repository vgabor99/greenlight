idf_component_register(SRCS "hello_world_main.c" "epd.c" "zb.c" "render.c" "screen.c" "bitmaps.c"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio nvs_flash esp_timer esp_app_format u8g2
                       INCLUDE_DIRS ".")

# Build switches (CMake caches them: pass =0 to switch back):
#   idf.py -DDEBUG_SCREEN=1 build   the RnD debug screen instead of the proper screen (render.h)
#   idf.py -DSCREEN_DUMP=1 build    print every frame on the console for tools/fb_dump.py
if(DEBUG_SCREEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RENDER_DEBUG_SCREEN=1)
endif()
if(SCREEN_DUMP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SCREEN_DUMP=1)
endif()
