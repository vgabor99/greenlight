# Project libraries

`Seeed_Studio_XIAO_Series.kicad_sym` (the whole symbol library) and the two XIAO ESP32-C6 footprints in
`Seeed_Studio_XIAO_Series.pretty/` are copied unchanged from Seeed Studio's
[OPL_Kicad_Library](https://github.com/Seeed-Studio/OPL_Kicad_Library), folder "Seeed Studio XIAO Series
Library", commit `b0035c51eb0348bb3e165fdb2f2765fa3d1d17bd` (2026-06-23). License: CC BY-SA 4.0.

- `XIAO-ESP32-C6-SMD`: castellated pads 1-14 plus the bottom pads 15-24 (JTAG, EN, BOOT, VBAT, GND).
- `XIAO-ESP32-C6-DIP`: the 14 header pins (through hole and SMD pads per pin).

The 3D model paths (`${AMZPATH}`) point to models that are not here, so the 3D view shows no XIAO. To update,
copy the files again from a newer commit and change the commit above.

`greenlight.kicad_sym` and `greenlight.pretty/` hold the parts made for this project:

- `XIAO-ESP32-C6-Header` (footprint): Seeed's `XIAO-ESP32-C6-DIP` without the SMD pads (they stick out 0.8 mm
  past the XIAO's long edges) and without its 3D models (they belong to other XIAO boards). The board uses it.
- `XIAO-ESP32-C6-DIP` (symbol): Seeed's `XIAO-ESP32-C6-SMD` symbol without the bottom pads 15-24, to match the
  header-pin footprint.
- `EKMB1205111` (symbol) and `PIR_Panasonic_EKMB_TO5` (footprint): the PIR, pins 1 VDD, 2 OUT, 3 GND. Seen from
  the top with the can's tab at the lower right: GND left, OUT top, VDD right (checked on the breadboard). The
  footprint draws the 27 x 14 mm lens on F.Fab.
- `Waveshare_ePaper_4.2_Module`: the panel module, pins numbered in the cable's order (VCC, GND, DIN, CLK, CS,
  DC, RST, BUSY).
