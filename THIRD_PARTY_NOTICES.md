# Third-party notices

Greenlight's own work is licensed as stated in the [README](README.md#license). This file lists the work of
others that is in this repository, or that the build fetches, and the license of each.

## In this repository

| What | Where | License |
|---|---|---|
| [Inter](https://github.com/rsms/inter) typeface, Copyright 2020 The Inter Project Authors | rendered to 1-bit glyphs in `main/screen/bitmaps.c`; text in the mockup images in `docs/uispec/mockups/` | [SIL Open Font License 1.1](https://openfontlicense.org) |
| [Material Design Icons](https://pictogrammers.com/library/mdi/) 7.4.47 (`@mdi/font`), by Pictogrammers | rendered to 1-bit icons in `main/screen/bitmaps.c`; icons in the mockup images in `docs/uispec/mockups/` | [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0) |
| Seeed Studio XIAO symbol library and footprints, from [OPL_Kicad_Library](https://github.com/Seeed-Studio/OPL_Kicad_Library) | `hardware/lib/Seeed_Studio_XIAO_Series.*`, and the parts derived from them in `hardware/lib/greenlight.*` (details: [hardware/lib/README.md](hardware/lib/README.md)) | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) |

`tools/gen_bitmaps.py` and `tools/gen_mockups.py` download the Inter and Material Design Icons font files when
they run; the font files themselves are not in the repository.

The e-paper driver (`main/panel/epaper.c`) follows the command sequence of Waveshare's EPD_4IN2_V2 example code
for the 4.2" module. No code is copied from it.

## Fetched by the build

The ESP-IDF component manager downloads these into `managed_components/` (versions pinned in
`dependencies.lock`). They are not in the repository, but they are part of the firmware binary.

| Component | License |
|---|---|
| [esp-zigbee-lib](https://components.espressif.com/components/espressif/esp-zigbee-lib) (Espressif) | Apache License 2.0 |
| [esp-zboss-lib](https://components.espressif.com/components/espressif/esp-zboss-lib) (DSR Corporation, Espressif) | its own license: binary use only, and only on Espressif chips (see the `LICENSE` file in the component) |
| [u8g2](https://github.com/olikraus/u8g2) (Oliver Kraus), used for the debug screen's text | 2-clause BSD |
| UW ttyp0 font, Copyright 2012-2025 Uwe Waldmann, in u8g2 as `u8g2_font_t0_16b_te` (debug screen only) | the ttyp0 license ([u8g2 font licenses](https://github.com/olikraus/u8g2/wiki/fntgrpttyp0)) |
| [ESP-IDF](https://github.com/espressif/esp-idf) (Espressif) | Apache License 2.0 |
