idf_component_register(SRCS "main.c" "state_machine.c" "console.c" "persistent_store.c" "data_model.c"
                            "board/board.c" "board/inputs.c" "board/battery.c" "board/battery_sense.c" "board/usb_sense.c" "board/sleep.c" "board/wake_reason.c"
                            "display/display.c" "display/display_model.c" "display/view_freshness.c"
                            "panel/epaper.c" "panel/panel.c" "panel/frame_dump.c"
                            "screen/render.c" "screen/status_screen.c" "screen/debug_screen.c" "screen/view_layout.c"
                            "screen/bitmap_text.c" "screen/bitmaps.c" "screen/framebuffer.c" "screen/sample_screens.c"
                            "util/utf8.c"
                            "zigbee/zigbee.c" "zigbee/zigbee_device.c" "zigbee/zigbee_display_input.c" "zigbee/zigbee_reports.c"
                       PRIV_REQUIRES esp_adc esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag nvs_flash esp_timer esp_app_format u8g2
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
