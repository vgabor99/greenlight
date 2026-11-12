# Hardware

The schematic and the board are a KiCad 10 project in [hardware/](../hardware/) ([Board](#board)).

## Parts

| Part                  | Type                                                                                                 | Reason for choice                                                                                                                                       |
|-----------------------|------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| **MCU**               | Seeed XIAO ESP32-C6 (ESP-IDF target `esp32c6`)                                                       | Low-power<br>Supports deep sleep<br>Supports Zigbee<br>Has battery management<br>Sufficient GPIOs and peripherals<br>Support external antenna if needed |
| **Panel**             | 4.2" 400x300 1-bit e-paper, Waveshare V2 rev 2.2, SSD1683 controller; driver `main/panel/epaper.c` | Low-power<br>Supports partial updates (~0.5s)<br>Sufficient size and resolution for glanceability                                                       |
| **PIR**               | Panasonic EKMB1205111 (2 uA, dT=4°C, 36°x150°) on 3V3                                                | Ultra low-power<br>High sensitivity<br>Wide lens fits the shape of the room<br>Voltage levels compatible with the 3.3V board                             |
| **Pair button**       | Right-angle tactile switch (AliExpress, generic, sold as 6x6x5): body 7.5 x 7 mm, SPST, through hole, Ø3.5 actuator centred 4 mm above the PCB, its tip 5 mm in front of the switch pins; footprint `SW_Tactile_SPST_Angled_PTS645Vx31-2LFS` (same pin layout), its peg holes widened on the board from 1.3 to 1.5 mm for the snap-in legs | User facing button for (re-) pairing<br>Right angle: hidden inside the enclosure, pressed with a pin through its bottom                                       |
| **Resistors, capacitors** | SMD, 0603 (C2 1206); values and LCSC part numbers in the schematic, uses in [Pins](#pins) and [Power and battery](#power-and-battery) | JLCPCB basic parts, placed by JLCPCB ([Board](#board))                                                                                   |
| **Battery** (planned) | 1000 mAh LiPo cell with a protection circuit                                                         | Long battery life<br>Protect the battery from under- and overvoltage<br>Not burning my home down                                                        |

The battery voltage and USB power are measured on the breadboard
([firmware.md](firmware.md#battery-and-usb-power)); the PCB is not built yet.

## Pins

The wake inputs must be deep-sleep wake pins, which on the ESP32-C6 are GPIO0-7 (on the XIAO header: D0, D1, D2; on
its underside the JTAG pads MTMS, MTDI, MTCK, MTDO = GPIO4-7). So the panel's DC and RST lines are on D4/D5, not
D1/D2. The ADC also reaches only GPIO0-6 (D0-D2 on the header), so D2 goes to the battery measurement, and USB
sense, LOW_BATTERY's wake, goes to the MTDO pad. Pin map in `main/board/board.h`; the schematic names its nets
after these defines.

| Pin | Use |
|---|---|
| D0 (GPIO0) | **PIR output** (`PIR_PIN`), active high (`PIR_ACTIVE_LEVEL` 1), external 100k pull-down to GND, no internal pull (keeps the sensor's output current under 100 uA); wake pin. `PIR_ACTIVE_LEVEL` 0 = a test button to GND with the internal pull-up. |
| D1 (GPIO1) | **pair button** (`PAIR_BUTTON_PIN`), to GND, internal pull-up; wake pin |
| D2 (GPIO2) | **battery sense** (`BATTERY_SENSE_PIN`), ADC ([Power and battery](#power-and-battery)) |
| D3 (GPIO21) | panel BUSY. Must be wired: the internal-pull "is it connected" test is unreliable because the module drives BUSY weakly. |
| D4 (GPIO22) | panel DC |
| D5 (GPIO23) | panel RST |
| D8, D9, D10 | panel SCK, CS, MOSI |
| MTDO pad (GPIO7), underside | **USB sense** (`VBUS_SENSE_PIN`), digital input with no pulls; LOW_BATTERY's wake pin ([Power and battery](#power-and-battery)) |

The panel pins are held low before power-off; that works on any GPIO. D6 and D7 are free. MTMS and MTDI (GPIO4/5)
are strapping pins, read at reset: not for a signal that may be high at power-on. A button wired straight to GND
(or any load on D1/D2) shows up as `inputs at boot: ... =0` in the log.

## Board

The outline and the positions of the XIAO, the PIR, the panel wires and the mounting holes come from the
enclosure's Fusion 360 sketch, exported as [hardware/mechanical/pcb-layout.dxf](../hardware/mechanical/pcb-layout.dxf)
(mm, origin at the centre of the panel's outline, seen from the front; in KiCad x = 100 + X, y = 100 - Y).

- 103 x 98.5 mm, two layers. The upper 103 x 78.5 mm sits behind the panel; the 20 mm strip below it holds the
  XIAO (bottom right, USB to the right edge), the PIR (bottom centre) and the pair button (bottom left, a
  right-angle switch facing the bottom edge, its actuator's tip 0.7 mm past it, pressed with a pin through the
  enclosure).
- All parts are on the front, which faces the panel; the back lies against the enclosure.
- Heights: under the panel the gap is 6 mm. Nothing goes on the battery (59 x 37 mm, behind the panel) or above
  it, where the panel has parts on its back; right of the battery only flat SMD parts. Below the battery 8 mm.
  The PIR sticks out of the enclosure; keep its 27 x 14 mm lens (centred on it) clear. KiCad rule areas mark the battery
  and the area above it; that one is L-shaped, leaving a strip left of the panel wire holes for flat SMD parts
  (C2, C3, not fitted: [BUSY hang](#busy-hang)).
- Antenna: the XIAO lies entirely below the panel, its chip antenna at the left end (marked in the Fusion
  sketch), so no metal is over it. No copper fill under the XIAO, and no copper at all (fill, tracks, vias)
  under its antenna end, 7 mm left of it and up to 0.8 mm above the top pin row; tracks reach the XIAO from
  above. The
  footprint is a header-only copy of Seeed's (`XIAO-ESP32-C6-Header`): Seeed's also has the SMD pads, which
  stick out 0.8 mm past the XIAO's long edges.
- The battery is soldered to the XIAO's BAT pads; a wire from BAT+ comes up to J1 above the XIAO for the
  battery measurement. The dividers (R2-R5, C1) sit above the XIAO too, not under it, where the battery wires
  are.
- The panel connects by 8 wires soldered into a row of holes at the right edge, labelled on the silkscreen
  (VCC at the bottom, BUSY at the top), so the panel and the board open like a book.
- Four M3 holes for standoffs.
- Routing keep-outs (KiCad rule areas): no tracks or vias on the front under the battery (the back may carry
  them), none within an 8 mm circle around each mounting hole (ground fill may reach the hole); above the lower holes no tracks or vias within 3 mm of
  the left, top and right edges (2.6 mm on the right, beside the panel wire holes); below them 0.5 mm, as
  Freerouting does not know JLCPCB's 0.3 mm copper-to-edge rule.
- Silkscreen: the battery's outline, the PIR lens (a dashed 27 x 14 mm rectangle), a 6 mm circle around
  each mounting hole, the panel wire names, BAT+, and
  "Greenlight v1 / Gabor Varga 2026" above the pair button.
- Routed with Freerouting, then tidied by hand in KiCad; ground fill on both layers. Tracks 0.3 mm, +3V3 and GND 0.5 mm. The rules follow
  JLCPCB's limits: 0.3 mm copper to the board edge, 0.15 mm minimum track (Freerouting narrows tracks between
  the XIAO's pads).
- Assembly: JLCPCB places the SMD resistors and capacitors (economic assembly, basic parts only, top side; their
  part numbers are in each symbol's LCSC field), except those marked DNP (C2, C3). The XIAO, the PIR, the button
  and the wires are soldered by hand.

## E-paper panel

What works on this panel (what was tried and failed is in [history.md](history.md#e-paper-driver)):

- **Only the mode-2 update works** (`0x22 = 0xFF`, about 440 ms). The real full-refresh waveform (mode 1:
  `0x22 = 0xF7`, also `0xC7` / `0x91`, which load it) never finishes: BUSY stays high even after 40 s and the
  screen ends up garbled grey.
- **Partial refresh** (`epaper_refresh_partial()`) follows Waveshare's partial example (`0x21 = 0x00`, write only
  RAM `0x24`, `0xFF`) but with border register `0x3C = 0x05` instead of Waveshare's `0x80`, which lets the
  border drift to black/grey over many updates.
- **Clear** (`epaper_clear()`) drives the whole screen to white with `drive_pass()` (mode 2); one pass
  (`WHITE_PASSES`, about 0.5 s) gives the cleanest white.
- Mode 2 only drives pixels where new RAM (`0x24`) != old RAM (`0x26`), and the controller scrambles its
  buffers after each update, so `drive_pass()` rewrites both every pass with old = inverse of new. Re-triggering
  without rewriting RAM inverts the screen.
- Black is weak and a small amount of grain on the white background is normal.
- Every clear and refresh logs a `panel: ...` line (duration, BUSY level before, black and changed pixel counts):
  a clear takes 500 ms, a partial 470 ms however many pixels change (tested up to about 22,600 black pixels per
  frame). Reading these lines: the `panel-troubleshoot` skill.

### BUSY hang

Symptom: a refresh never finishes, BUSY stays high, the panel goes grainy grey, and every later panel command
fails (`busy before 1`) until a hardware reset (sometimes the first reset is not enough while the contact is
still bad).

Cause (found 2026-10-14/15): a bad display GND contact on the breadboard. A stress test gave 10 hangs in about
365 refreshes while the board and wires were handled, 0 in 1,546 untouched, and every later hang matched a touch
of the display's GND jumper. Ruled out: radio TX (bursts of reports during the refresh), pixel density, refresh
count, time awake.

Fix: a solid display GND, which the PCB's soldered wires and ground fill give. Until it is built, don't touch the
breadboard while it refreshes. The firmware does not recover on purpose
([behaviour.md](behaviour.md#failure-modes)).

No bulk capacitor at the display: C2 (100 uF) and C3 (100 nF) have footprints at the panel wire holes but are not
fitted (DNP), and get soldered on only if the board shows supply trouble. Reasons:

- The hangs needed a touch: 0 in 1,546 untouched refreshes on the breadboard, with no bulk capacitor.
- The module decouples its own supply. Waveshare's rev 2.x modules feed VCC (1 uF) into an RT9193-33 LDO,
  followed by 1 uF and 2 x 4.7 uF at the panel's booster; the panel's reference circuit asks for only 4.7 uF + 100
  nF. A refresh draws about 5.6 mA (typical, panel datasheet).
- The XIAO's +3V3 already carries its SGM6029C buck's recommended 10 uF output capacitor plus about 4 uF at the
  ESP32-C6.
- On the PCB the wires to the module lie between C2/C3 and the module, so they would mostly sit beside the XIAO's
  own 10 uF.
- Costs: the buck's datasheet recommends 10 uF and gives no maximum, so 10 times that is untested territory for
  its control loop; the 100 uF ceramic's leakage is typically well under 1 uA but specified only to a few uA,
  against a deep-sleep budget of a few tens of uA. The startup inrush (about 0.6-0.9 A over the 410 us
  soft-start) stays under the buck's 1.58 A current limit.

## Power and battery

- The ESP32-C6 cannot switch itself off; deep sleep is its floor. On the XIAO the cell, the charger and the
  system share the BAT pad, so a switch that cuts the system would also stop charging.
- The battery (planned) is soldered to the XIAO's BAT pads on its underside, not to the PCB: the XIAO sits on
  header pins, so those pads do not reach the board. A thin wire from the BAT+ pad to a PCB pad (J1 in the
  schematic) feeds the battery measurement.
- Battery measurement: the XIAO ESP32-C6 has no battery sense path of its own. VBAT goes through a 1M/1M divider
  (about 2 uA) with 100 nF at the pin to D2. When it is measured: [behaviour.md](behaviour.md#low-battery);
  how: [firmware.md](firmware.md#battery-and-usb-power). Any two equal resistors keep the ratio; a meter
  with a 1 MΩ input reads the divider's halves low, the ADC does not. On the breadboard (681k/681k, 100 nF) the ADC
  read 3872 mV against 3880 mV on the meter, without a correction factor; check again on the PCB.
- USB sense: the 5V pin through 100k/150k (about 3 V) to the XIAO's MTDO pad (GPIO7), by a thin wire from the pad
  to a PCB pad (J2), like the battery's (J1). The firmware reads it without pulls, and LOW_BATTERY deep-sleeps
  until it goes high ([firmware.md](firmware.md#battery-and-usb-power)). On the breadboard (82k/140k) it read 0.2 V
  on battery, awake and asleep, and high on USB, and a USB plug-in woke LOW_BATTERY. The 5V pin is not quite 0 V on
  battery: about 0.33 V from a leak, so the divider draws about 1.2 uA ([roadmap.md](roadmap.md#future-work)). The
  protection circuit is the last cutoff.
- On USB the sleep current means nothing (the XIAO's regulator and charger); measure it on the battery.

## USB console and deep sleep

The console is the USB-Serial-JTAG port (COM5 on the Windows bench). It is exclusive: close any monitor or gdb
session before flashing.

- While the chip sleeps the USB port is gone: a sleeping board cannot be flashed or reset over USB (wake it
  first, or BOOT + RESET).
- **No log after a deep-sleep wake.** At the wake Windows lists COM5 again and opens it, but no byte arrives for
  the whole wake (with or without DTR). IDF's USB Serial/JTAG console guide: after sleeping while connected, the
  connection "can be re-established by unplugging and re-plugging the USB cable". Tried without effect: a clean
  disconnect before the sleep (D+ pull-up off), installing the console driver only after the host settled. Boots
  after a reset over USB (flash, esptool) log normally. Without the battery a replug is a power-on reset, so it
  logs a fresh boot, not the wake.
- So watch wakes in HA (`wake_count`, `awake`), or build with the simulated sleep for bench work
  ([firmware.md](firmware.md#build-switches)).
