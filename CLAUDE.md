# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

"greenlight" is an ESP-IDF project (C, CMake) derived from the ESP-IDF `hello_world` example. It currently targets the **ESP32-C6** (`CONFIG_IDF_TARGET="esp32c6"` in `sdkconfig`). `app_main()` in `main/hello_world_main.c` runs the device state machine and draws test screens into a 1-bit framebuffer and pushes it to a 4.2" 400x300 e-paper panel (Waveshare V2 rev 2.2, SSD1683) through the driver in `main/epd.c` (wiring/pins in `main/epd.h`, Seeed XIAO ESP32-C6). The README is still the stock example README.

## Build environment

`idf.py` is not on PATH by default. ESP-IDF v6.1 was installed with EIM; activate it in each new shell:

```
. ~/.espressif/tools/activate_idf_v6.1.sh
```

On the Windows machine (IDF in `C:\esp\v6.1\esp-idf`, tools in `C:\Espressif\tools`) use PowerShell instead: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`, and flash with `idf.py -p COM5 flash`. The board's USB port is exclusive: close any IDF monitor or debug (gdb) session before flashing. `idf.py monitor` needs a TTY, so read the log with a pyserial script instead.

On the Mac, use only this script (it is also what the VS Code ESP-IDF extension uses). Do not source `~/.espressif/v6.1/esp-idf/export.sh`: it fails here, and a build configured with a different Python venv makes `idf.py` refuse to run until `idf.py fullclean`.

## Commands

```
idf.py build                          # build into ./build
idf.py -p /dev/cu.usbmodem101 flash   # flash (board shows up as this port on the dev Mac)
idf.py -p /dev/cu.usbmodem101 monitor # serial output (Ctrl+] to exit)
idf.py set-target <chip>              # switch target; regenerates sdkconfig
idf.py menuconfig                     # edit sdkconfig
```

Tests are pytest-embedded scripts (`pytest_hello_world.py`, markers: generic, host_test, qemu) that expect "Hello world!" on the device/QEMU/Linux build. They need the ESP-IDF pytest environment and are normally run with `pytest --target <chip>` after building.

## Architecture notes

- Top-level `CMakeLists.txt` sets `MINIMAL_BUILD ON`, so only `main` and the components it depends on are built. A new component dependency must be declared in `main/CMakeLists.txt` (`REQUIRES` / `PRIV_REQUIRES`), otherwise it won't be built; `spi_flash` is currently the only one.
- E-paper quirk: this panel's real full-refresh waveform (mode 1: `0x22 = 0xF7`, also `0xC7`/`0x91` which load it) never finishes (BUSY stays high even after 40 s, screen ends up garbled grey), even with `0x01` driver output control and `0x18 = 0x80` set. Only the mode-2 update (`0x22 = 0xFF`, ~440 ms) works. `epd_display_partial()` follows Waveshare's partial example (`0x21=0x00`, write only RAM `0x24`, `0xFF`) but with border register `0x3C=0x05` instead of Waveshare's `0x80`: with `0x80` the panel border drifts to black/grey over many updates. Rewriting old RAM (`0x26`) with the previous frame made no difference. Black is weak and a small amount of grain on the white background is normal. A custom full-refresh waveform (Waveshare's 4-gray `LUT_ALL` via `0x32`, `0x22 = 0xCF`) hung as well, so it was removed. `epd_clear()` drives the whole screen to white with `drive_pass()` (mode 2); one pass (`EPD_WHITE_PASSES`, ~0.5 s) gave the cleanest white. Tuning results: more drive makes the white background grainier, not cleaner (10 black/white cycles, 12 white passes, and going to black first were all worse). A checkerboard stage (removed) made the grain worse too, and with 1-pixel squares BUSY hung (suspected supply sag). Mode 2 only drives pixels where new RAM (`0x24`) != old RAM (`0x26`) and the controller scrambles its buffers after each update, so `drive_pass()` rewrites both every pass with old = inverse of new; re-triggering without rewriting RAM inverts the screen. `app_main` calls `epd_clear()` once, then cycles ONE..TEN with back-to-back partial refreshes (~470 ms each, logged as `#n WORD (ms)`) to observe ghosting over many updates. BUSY (GPIO21/D3) must be wired; the internal-pull "is it connected" test is unreliable because the module drives BUSY weakly.
- Inputs and state machine (design in `DESIGN.md`, section 3): D0 (GPIO0, `PIR_PIN`) stands in for the PIR (push button to GND, internal pull-up, `PIR_ACTIVE_LEVEL` 0) and D1 (GPIO1, `PAIR_BUTTON_PIN`) is the pair button. Panel DC/RST are on D4/D5 so D0/D1 stay deep-sleep wake pins; a button wired straight to GND (or any load on D1/D2) shows up as `inputs at boot: ... =0` in the log. Both inputs are polled and debounced by `input_task` (level based). `app_main()` runs a state machine: BOOT, UNPAIRED_WAIT, PAIRING, INTERVIEW, REJOIN, ACTIVE, ERASE_NETWORK, SHUTDOWN, SLEEP. Activity (D0 or D1 pressed) holds the `RUN_MS` (5 s) timeout; it counts down after release. Holding D1 for 3 s does the same in both unpaired and paired states: in UNPAIRED_WAIT it starts PAIRING, in ACTIVE it erases the (stubbed) network and goes straight to PAIRING; short presses only wake. Zigbee is stubbed (join succeeds after 3 s, rejoin after 1 s); `INTERVIEW_MS` is temporarily 10 s (design value 60 s). The state that must survive sleep (`paired`, `pair_now`) is in NVS. SLEEP simulates deep sleep: wait for D0 or D1, then `esp_restart()`, so every wake takes the real boot path. The panel shows a placeholder word per state (ACTIVE keeps the ONE..TEN refresh test). To get a factory-new device: `python -m esptool --chip esp32c6 -p COM5 erase-region 0x9000 0x6000`.
- `sdkconfig` is committed/generated in the repo root; `sdkconfig.ci` holds CI-only overrides (currently empty).
- `.devcontainer/` defines an ESP-IDF QEMU container; `.clangd` strips `-f*`/`-m*` flags so clangd copes with the RISC-V/Xtensa GCC flags in `compile_commands.json`.
