# Greenlight

Greenlight is a battery-powered display that tells you whether the house is ready to be armed
and left. Mounted next to the alarm panel at the entrance, it lists anything that needs attention first, such
as a window or door left open or a light left on, and shows "All OK" when there is nothing to do.

In what it shows, it is close to a Home Assistant dashboard. The difference is that it runs on a battery, with a
target of one charge a year or less, so both the hardware and the software are designed around power use:

- **Hardware:** an e-paper panel keeps its image without power, so a screen costs nothing to keep showing.
- **Software:** a screen nobody is looking at doesn't need to be up to date. Greenlight is a sleepy Zigbee
  end device that spends most of its life in deep sleep. A low-power PIR sensor wakes it when someone steps in
  front of it. It then rejoins the network and gets the latest state from Home Assistant. It stays live while
  someone is there and goes back to sleep afterwards.

Hardware and software in brief:

- Hardware: Seeed XIAO ESP32-C6, 4.2" 400x300 e-paper (Waveshare V2, SSD1683), Panasonic EKMB1205111
  low-power PIR sensor, pair button.
- Firmware: ESP-IDF v6.1 (C, CMake), a Zigbee sleepy end device.
- Home Assistant side: Zigbee2MQTT with the external converter in `z2m/`, templates and an automation in `ha/`.

Status: works end to end on the bench. Deep sleep and the battery are still simulated (DESIGN.md 3.6, 6.6).

## Layout

```
main/        firmware: state machine (hello_world_main.c), Zigbee (zb.c), panel driver (epd.c),
             screens (screen.c), framebuffer and debug screen (render.c), generated bitmaps (bitmaps.c)
tools/       bitmap generator, serial console, framebuffer dump, Zigbee bench test
z2m/         Zigbee2MQTT external converter
ha/          Home Assistant templates and package
DESIGN.md    the design: behaviour, data model, Zigbee, HA, display, open questions, plan
ZIGBEE_PAYLOAD.md  Zigbee frame size and delivery measurements
CLAUDE.md    build environment, commands and hardware quirks
```

## Build and flash

Activate ESP-IDF v6.1 first (see CLAUDE.md for the Mac and Windows variants), then:

```
idf.py build
idf.py -p COM5 flash
```

Build switches: `-DDEBUG_SCREEN=1` (RnD debug screen), `-DSCREEN_DUMP=1` (frames on the console for
`tools/fb_dump.py`).
