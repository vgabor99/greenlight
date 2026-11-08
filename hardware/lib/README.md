# Project libraries

`Seeed_Studio_XIAO_Series.kicad_sym` (the whole symbol library) and the two XIAO ESP32-C6 footprints in
`Seeed_Studio_XIAO_Series.pretty/` are copied unchanged from Seeed Studio's
[OPL_Kicad_Library](https://github.com/Seeed-Studio/OPL_Kicad_Library), folder "Seeed Studio XIAO Series
Library", commit `b0035c51eb0348bb3e165fdb2f2765fa3d1d17bd` (2026-06-23). License: CC BY-SA 4.0.

- `XIAO-ESP32-C6-SMD`: castellated pads 1-14 plus the bottom pads 15-24 (JTAG, EN, BOOT, VBAT, GND). The symbol
  `XIAO-ESP32-C6-SMD` points to this footprint; it is the one that reaches the battery pads.
- `XIAO-ESP32-C6-DIP`: the 14 header pins only (through hole and SMD pads per pin).

The 3D model paths (`${AMZPATH}`) point to models that are not here, so the 3D view shows no XIAO. To update,
copy the files again from a newer commit and change the commit above.

`greenlight.kicad_sym` holds the symbols made for this project:

- `EKMB1205111`: the PIR. Pin numbers 1 VDD, 2 OUT, 3 GND are taken from sibling EKMB parts; check them against
  Panasonic's dimension drawing before the PCB.
- `Waveshare_ePaper_4.2_Module`: the panel module, pins numbered in the cable's order (VCC, GND, DIN, CLK, CS,
  DC, RST, BUSY).
