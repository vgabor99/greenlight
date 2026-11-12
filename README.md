# Greenlight

*"And if you gaze long into a screen, the screen also gazes into you."<br>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;— Friedrich Nietzsche*

Greenlight is a battery-powered display that tells you whether the house is ready to be armed and left. Mounted
next to the alarm panel at the entrance, it lists anything that needs attention first, such as a window or door
left open or a light left on, and shows *All OK* when there is nothing to do.

In what it shows, it is close to a Home Assistant dashboard. The difference is that it runs on a battery, with a
target of one charge a year or less, so both the hardware and the software are designed around power use:

- **Hardware:** an e-paper panel keeps its image without power, so a screen costs nothing to keep showing.
- **Software:** a screen nobody is looking at doesn't need to be up to date. Greenlight spends most of its life in
  deep sleep and is awake only for the short periods when someone is in sight of it. As soon as nobody is
  watching, it goes back to sleep.

## Parts

- Hardware: an ESP32-C6 microcontroller, a 4.2" 400x300 e-paper display, a low-power PIR sensor, a pair button,
  and a few resistors and capacitors. A custom PCB holds them together in a 3D-printed enclosure. Details:
  [docs/hardware.md](docs/hardware.md).
- Firmware: ESP-IDF v6.1 (C, CMake), a Zigbee sleepy end device.
- Home Assistant side: Zigbee2MQTT with the external converter in `z2m/`, templates and an automation in `ha/`.

## Status

Works end to end on a breadboard, with real deep sleep and real battery and USB sensing. The PCB is designed but
not built yet. Details: [docs/roadmap.md](docs/roadmap.md).

## Repository layout

```
main/        firmware, grouped by concern: app/, board/, display/, panel/, screen/, zigbee/, util/
hardware/    KiCad 10 schematic and PCB, project libraries (lib/), board outline DXF (mechanical/)
tools/       bitmap and mockup generators, serial console, framebuffer dump, Zigbee bench test, docs checker
z2m/         Zigbee2MQTT external converter
ha/          Home Assistant templates and package
docs/        design, history, roadmap and research notes, UI spec and mockups (start at docs/README.md)
.claude/     Claude Code skills: bench testing, HA deploy, panel troubleshooting, bitmaps, docs check, KiCad
CLAUDE.md    build environment and conventions
```

## Build and flash

Activate ESP-IDF v6.1 first (see [CLAUDE.md](CLAUDE.md) for the Windows and Mac variants), then:

```
idf.py build
idf.py -p COM5 flash
```

## Documentation

[docs/README.md](docs/README.md) is the index: architecture, device behaviour, data model, Zigbee interface,
Home Assistant side, display, hardware, firmware layout, history and roadmap.
