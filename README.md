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

## Building

### Before you start

This is a personal project tailored to my own needs, development environment, and home.

`CLAUDE.md` and the skills in `.claude/` describe my setup. Serial ports, installation paths, the MQTT broker's address,
and Home Assistant entity names all belong to one particular home. Change them to match yours. Development and testing
work best when an AI agent has access to your Home Assistant instance and MQTT broker.

The code and documentation are in English, but because the project was built for a Hungarian home,
you'll find a bit of Hungarian here and there, mostly in the screen texts
([docs/uispec/ui-spec.md](docs/uispec/ui-spec.md)). If you feel like learning Hungarian, godspeed!

### Firmware build and flash

You need ESP-IDF v6.1 and a Seeed XIAO ESP32-C6. Activate ESP-IDF ([CLAUDE.md](CLAUDE.md) shows how on my Windows and
Mac machines), then run:

```
idf.py build
idf.py -p <port> flash
```

### Zigbee2MQTT

Load `z2m/greenlight.mjs` into Zigbee2MQTT as an external converter
([docs/zigbee.md](docs/zigbee.md#external-converter)).

### Home Assistant

Copy the template from `ha/custom_templates/` and the package from `ha/packages/` into the same folders of your Home
Assistant configuration ([docs/home-assistant.md](docs/home-assistant.md)).

### Testing

The bench test (`tools/paced_write_test.py`) talks to the MQTT broker, so its MQTT user needs read access to the bridge
and read-write access to the device. For example, in a Mosquitto `acl_file`:

```
user claude
topic read zigbee2mqtt/bridge/#
topic read zigbee2mqtt/gf_lobby_greenlight/#
topic write zigbee2mqtt/gf_lobby_greenlight/set
```

Before running the test, disable the `Greenlight push` automation
([docs/home-assistant.md](docs/home-assistant.md#push-automation)) so that real data does not interfere with it.

## Documentation

[docs/README.md](docs/README.md) is the index: architecture, device behaviour, data model, Zigbee interface,
Home Assistant side, display, hardware, firmware layout, history and roadmap.

## License

- Firmware, tools, and the Home Assistant and Zigbee2MQTT files: [MIT](LICENSE).
- Hardware (`hardware/`): [CERN-OHL-P-2.0](hardware/LICENSE). Exception: the Seeed Studio XIAO library files in
  `hardware/lib/` and the parts derived from them are CC BY-SA 4.0 ([hardware/lib/README.md](hardware/lib/README.md)).
- Documentation and mockup images (`docs/`): [CC BY 4.0](docs/LICENSE).

Fonts, icons and libraries by others: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
