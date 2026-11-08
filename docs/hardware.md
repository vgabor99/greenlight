# Hardware

The schematic and the board are a KiCad 10 project in [hardware/](../hardware/) (being drawn; until it is done,
the pin table below is the wiring).

## Parts

| Part                  | Type                                                                                                 | Reason for choice                                                                                                                                       |
|-----------------------|------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| **MCU**               | Seeed XIAO ESP32-C6 (ESP-IDF target `esp32c6`)                                                       | Low-power<br>Supports deep sleep<br>Supports Zigbee<br>Has battery management<br>Sufficient GPIOs and peripherals<br>Support external antenna if needed |
| **Panel**             | 4.2" 400x300 1-bit e-paper, Waveshare V2 rev 2.2, SSD1683 controller; driver `main/display/epaper.c` | Low-power<br>Supports partial updates (~0.5s)<br>Sufficient size and resolution for glanceability                                                       |
| **PIR**               | Panasonic EKMB1205111 (2 uA, dT=4°C, 36°x150°) on 3V3                                                | Ultra low-power<br>High senstivity<br>Wide lens fits the shape of the room<br>Voltage levels compatible with the 3.3V board                             |
| **Pair button**       | One I had lying around                                                                               | User facing button for (re-) pairing                                                                                                                    |
| **Resistor**          | 100k                                                                                                 | External pull-down resistor for the PIR output<br>Resistance is chosen to keep the sensor's output current within spec, under 100 uA                    |
| **Battery** (planned) | 1000 mAh LiPo cell with a protection circuit                                                         | Long battery life<br>Protect the battery from under- and overvoltage<br>Not burning my home down                                                        |

The battery voltage and USB power are simulated for now ([firmware.md](firmware.md#simulated-parts)).

## Pins

Both wake inputs must be deep-sleep wake pins, which on the ESP32-C6 are GPIO0-7 (on the XIAO header: D0, D1,
D2). So the panel's DC and RST lines are on D4/D5, not D1/D2. Pin map in `main/board/board.h`.

| Pin | Use |
|---|---|
| D0 (GPIO0) | **PIR output** (`PIR_PIN`), active high (`PIR_ACTIVE_LEVEL` 1), external 100k pull-down to GND, no internal pull (keeps the sensor's output current under 100 uA); wake pin. `PIR_ACTIVE_LEVEL` 0 = a test button to GND with the internal pull-up. |
| D1 (GPIO1) | **pair button** (`PAIR_BUTTON_PIN`), to GND, internal pull-up; wake pin |
| D2 (GPIO2) | free (spare wake pin); planned: VBUS sense ([Power and battery](#power-and-battery)) |
| D3 (GPIO21) | panel BUSY. Must be wired: the internal-pull "is it connected" test is unreliable because the module drives BUSY weakly. |
| D4 (GPIO22) | panel DC |
| D5 (GPIO23) | panel RST |
| D8, D9, D10 | panel SCK, CS, MOSI |

The panel pins are held low before power-off; that works on any GPIO. Avoid D6/D7 (UART0) in case the console
moves there. A button wired straight to GND (or any load on D1/D2) shows up as `inputs at boot: ... =0` in the
log.

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

Fix (hardware, still to do): a solid display GND (new or doubled wire, soldered) and a bulk capacitor (47-220 uF
+ 100 nF) at the display's VCC/GND pins. Until then, don't touch the board while it refreshes. The firmware does
not recover on purpose ([behaviour.md](behaviour.md#failure-modes)).

## Power and battery

- The ESP32-C6 cannot switch itself off; deep sleep is its floor. On the XIAO the cell, the charger and the
  system share the BAT pad, so a switch that cuts the system would also stop charging.
- The real LOW_BATTERY will be `esp_deep_sleep_start()` with VBUS sense as the only wake source: planned as a
  divider from the 5V pad to D2 (GPIO2, an LP wake pin, ext1 any-high), and the PIR's supply cut if it is
  switchable. The protection circuit is the last cutoff.
- Battery measurement (planned): ADC on a spare pin with a high-value divider and a capacitor at the pin; first
  check whether the XIAO ESP32-C6 already has a battery sense path. Measured once per wake before the radio and
  the panel start.
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
