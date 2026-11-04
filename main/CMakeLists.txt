idf_component_register(SRCS "main.c" "state_machine.c" "inputs.c" "persistent_store.c" "battery.c" "console.c" "view_freshness.c"
                            "wake_reason.c" "sleep.c" "board.c" "display_model.c" "display.c" "panel.c" "frame_dump.c"
                            "zb.c" "render.c" "screen.c" "epd.c" "utf8.c" "framebuffer.c" "bitmaps.c"
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag nvs_flash esp_timer esp_app_format u8g2
                       INCLUDE_DIRS ".")

# Build switches (CMake caches them: pass =0 to switch back):
#   idf.py -DDEBUG_SCREEN=1 build   the RnD debug screen instead of the proper screen (render.h)
#   idf.py -DSCREEN_DUMP=1 build    print every frame on the console for tools/fb_dump.py
#   idf.py -DSLEEP_TIMER_S=30 build a timer also ends deep sleep after 30 s (bench tests; =0 switches it off)
#   idf.py -DSIM_SLEEP=1 build      simulated sleep (wait for an input, then restart): USB and the log stay up
if(DEBUG_SCREEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RENDER_DEBUG_SCREEN=1)
endif()
if(SCREEN_DUMP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SCREEN_DUMP=1)
endif()
if(SIM_SLEEP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SIM_SLEEP=1)
endif()
if(SLEEP_TIMER_S)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SLEEP_TIMER_S=${SLEEP_TIMER_S})
endif()
