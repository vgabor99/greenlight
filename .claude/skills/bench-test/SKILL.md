---
name: bench-test
description: "Test the greenlight board on the bench - read its serial log, override the battery voltage / simulate USB over the console, reset it, send view/dataset writes through Zigbee2MQTT, run tools/paced_write_test.py, or make it factory new. Use whenever a task needs the device's serial output, a test on the real hardware, or writes sent to the device."
---

# Bench testing

The board is on COM5 (Windows). The port is exclusive: one process at a time, and close any IDF monitor or
gdb session first. `idf.py monitor` needs a TTY, so read the log with `tools/console.py` (or a pyserial script
with `dtr=False, rts=False`, so opening the port doesn't reset the board). The IDF venv's Python has pyserial:
`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`.

## Know the board's sleep state first

- With real deep sleep, a sleeping board has no USB: it cannot be flashed, reset or read over USB. Wake it
  first (PIR or pair button), or BOOT + RESET.
- After a deep-sleep wake no log reaches the PC at all, although COM5 comes back and opens. A boot after a USB
  reset logs normally. Watch real wakes in HA instead (`sensor.gf_lobby_greenlight_wake_count`,
  `binary_sensor.gf_lobby_greenlight_awake`).
- For autonomous testing build with the simulated sleep: `idf.py -DSIM_SLEEP=1 build` (wait for D0/D1, then
  `esp_restart()`): USB, the log, the console and resets keep working. `-DSLEEP_TIMER_S=N` adds a timer wake.
  Both are cached by CMake: build with `=0` to switch back, and say so when handing the board back.

## Serial console: tools/console.py

Log monitor and command sender in one process (because COM5 is exclusive); reconnects when the board restarts.

```
python tools/console.py [PORT] [--reset] [--send CMD]... [--for SECONDS] [--log FILE]
python tools/console.py --send "bat 3450" --send "20:usb 1" --for 40
```

- `--reset`: reset the board first (RTS, like esptool): a wake without pressing a button. Only on an awake board.
- `--send "SECONDS:CMD"` sends CMD that many seconds after the start; plain `--send "CMD"` right away.
- Console commands: `bat <mV>` (2500-4500) overrides the measured battery voltage, `bat 0` goes back to the
  measurement; `usb 0|1` sets the simulated USB power; `bat` / `usb` alone print them. Both persist in NVS: set
  `bat 0` after a test. `vbat` measures and logs (D2: pin and VBAT mV). Low battery is below 3500 mV; while
  latched it needs 3700 mV or `usb 1` (docs/behaviour.md#low-battery).
- Low-battery test without the console: pull the BAT side of the divider, not the D2 wire (a floating pin reads
  anything). The latch is set only once Z2M confirms the report; check
  `binary_sensor.gf_lobby_greenlight_battery_low` in HA. To recover, reconnect it and reset. A meter with a 1 MΩ input reads the divider's halves low.

## Writes through Zigbee2MQTT

Before sending any view or dataset write:

1. Start a serial logger in the background first, and report a verdict per write from it.
2. Take the current dataset hash from the device (debug screen `DS xxxxxx n=N`, or the serial log), not from an
   old test log. A view with another hash is just "out of sync".
3. Send one write and check its verdict, then the rest, one at a time (no parallel publishes when spacing
   matters).

Write formats: docs/zigbee.md#input-ha-to-device (topic `zigbee2mqtt/gf_lobby_greenlight/set`). The MQTT account
cannot publish to `<device>/get`, and Z2M state is not retained.

## tools/paced_write_test.py

The dataset + view write test. **The user runs it in their own shell** (it needs the MQTT credentials):

```
set MQTT_USER=... & set MQTT_PASS=...        (MQTT_HOST defaults to 192.168.0.32)
%TEMP%\gl_test\venv\Scripts\python tools\paced_write_test.py ROUNDS [SPACING_MS] [GAP_S] [--reset-each] ...
```

Modes (full list in the script's docstring): `--reset-each` (the real wake path), `--resync-each` (new dataset
every round), `--oos` (out of sync), `--same` (same view id), `--no-view` (expects `VIEW_WAIT_MS`), `--hu` /
`--hu-long` (accented, multi-part lines), `--drop-part` (expects a rejected commit). Logs go to
`%TEMP%\gl_test\paced_write_test_<date>_<time>.log`. `--reset-each` needs an awake board (or `SIM_SLEEP`).
Record notable results in docs/research/wake-delivery-bench.md.

## Factory-new device

```
python -m esptool --chip esp32c6 -p COM5 erase-region 0x9000 0x6000
```

Erases NVS: the network, dataset, view, wake counter and battery override. Remove the device in Z2M too
before re-pairing for a clean start.
