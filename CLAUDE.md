# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

"greenlight" is an ESP-IDF project (C, CMake) derived from the ESP-IDF `hello_world` example. It currently targets the **ESP32-C6** (`CONFIG_IDF_TARGET="esp32c6"` in `sdkconfig`). `app_main()` in `main/hello_world_main.c` draws a test screen into a 1-bit framebuffer and pushes it to a 4.2" 400x300 e-paper panel (Waveshare V2 rev 2.2, SSD1683) through the driver in `main/epd.c` (wiring/pins in `main/epd.h`, Seeed XIAO ESP32-C6). The README is still the stock example README.

## Build environment

`idf.py` is not on PATH by default. ESP-IDF v6.1 was installed with EIM; activate it in each new shell:

```
. ~/.espressif/tools/activate_idf_v6.1.sh
```

Use only this script (it is also what the VS Code ESP-IDF extension uses). Do not source `~/.espressif/v6.1/esp-idf/export.sh`: it fails here, and a build configured with a different Python venv makes `idf.py` refuse to run until `idf.py fullclean`.

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
- E-paper quirk: on this panel the full-refresh update (display mode 1, `0x22 = 0xF7`) never finishes (BUSY stays high), leaving a flash then grey. `epd_display()` instead repeats the quick mode-2 update (`0x22 = 0xFF`, ~420 ms) `EPD_REFRESH_PASSES` (12) times to reach full black, about 6 s total. Mode 2 only drives pixels where new RAM (`0x24`) != old RAM (`0x26`) and the controller scrambles its buffers after each update, so both RAMs are rewritten every pass with old = inverse of new. Re-triggering without rewriting RAM inverts the screen. BUSY (GPIO21/D3) must be wired; the internal-pull "is it connected" test is unreliable because the module drives BUSY weakly.
- `sdkconfig` is committed/generated in the repo root; `sdkconfig.ci` holds CI-only overrides (currently empty).
- `.devcontainer/` defines an ESP-IDF QEMU container; `.clangd` strips `-f*`/`-m*` flags so clangd copes with the RISC-V/Xtensa GCC flags in `compile_commands.json`.
