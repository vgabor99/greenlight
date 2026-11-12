# CLAUDE.md

Guidance for Claude Code in this repository.

## Overview

Greenlight is a battery-powered Zigbee e-paper display that shows Home Assistant's list of things to fix before
leaving the house (as HA's `dashboard-away` ("Bezárva?") dashboard does). Firmware: ESP-IDF v6.1 (C, CMake) on
a Seeed XIAO ESP32-C6, a sleepy end device with a 4.2" 400x300 SSD1683 panel, a PIR and a pair button.
Schematic and PCB: `hardware/` (KiCad 10). HA side: `ha/` (templates and an automation) and `z2m/` (Zigbee2MQTT
converter).

Documentation: start at [docs/README.md](docs/README.md), which says which file answers what. The design files
describe the current state; [docs/history.md](docs/history.md) holds past designs and lessons,
[docs/roadmap.md](docs/roadmap.md) what is open. Code comments point to docs as `docs/<file>.md#<heading>`.

## Skills

Load the matching skill before doing these:

- `bench-test`: serial log, console commands, reset, Zigbee writes through Z2M, `paced_write_test.py`, factory reset.
- `deploy-ha`: any change under `ha/` or `z2m/`.
- `docs-check`: checking the docs for broken links, contradictions or drift from the code; after renaming a
  heading or a file.
- `kicad`: anything under `hardware/` (the KiCad 10 schematic and PCB).
- `panel-troubleshoot`: panel failures in the log, or changes to `epaper.c` / `panel.c`.
- `regen-bitmaps`: anything that changes `main/screen/bitmaps.c`, a new line icon, checking screens against the mockups.

## Build environment

`idf.py` is not on PATH. ESP-IDF v6.1 was installed with EIM.

**Windows** (IDF in `C:\esp\v6.1\esp-idf`, tools in `C:\Espressif\tools`, board on COM5): use PowerShell and
source `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`. From Git Bash, `idf.py` refuses to run
("MSys/Mingw is no longer supported") because PowerShell inherits `MSYSTEM` and friends, so strip them first
(Git Bash quoting):

```
powershell.exe -NoProfile -Command 'Get-ChildItem Env: | Where-Object { $_.Name -match '\''MSYS|MINGW'\'' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }; . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 *> $null; idf.py -p COM5 flash'
```

**Mac** (board on `/dev/cu.usbmodem101`): `. ~/.espressif/tools/activate_idf_v6.1.sh` in each new shell (also
what the VS Code ESP-IDF extension uses). Don't source `~/.espressif/v6.1/esp-idf/export.sh`: it fails, and a
build configured with another Python venv makes `idf.py` refuse to run until `idf.py fullclean`.

## Commands

```
idf.py build                  # build into ./build
idf.py -p COM5 flash          # flash
idf.py menuconfig             # then: idf.py save-defconfig (sdkconfig.defaults is the tracked config)
```

- The USB port is exclusive: close any monitor or gdb session before flashing. A board in deep sleep has no USB:
  wake it first.
- `idf.py monitor` needs a TTY; read the log with `tools/console.py` (the `bench-test` skill).
- Build switches (CMake caches them, `=0` switches back): `-DDEBUG_SCREEN=1`, `-DSCREEN_DUMP=1`,
  `-DSIM_SLEEP=1`, `-DSLEEP_TIMER_S=N` ([docs/firmware.md](docs/firmware.md#build-switches)).
- There are no automated tests; testing is on the board.

## Code conventions

- Firmware files live under `main/`, grouped in folders by concern (`app/`, `board/`, `display/`, `panel/`, `screen/`, `zigbee/`,
  `util/`), one purpose per file, named after it. File map and dependency rule:
  [docs/firmware.md](docs/firmware.md#source-layout).
- Each module's functions carry its file name as prefix (`inputs_`, `battery_`, `zigbee_`, ...).
- Includes name the path from `main/` (`#include "zigbee/zigbee.h"`, also within a folder; `INCLUDE_DIRS` is
  only `.`). A new `.c` goes into `SRCS` in `main/CMakeLists.txt`; a new component dependency into its
  `PRIV_REQUIRES` (the top level sets `MINIMAL_BUILD ON`).
- Log tags stay `greenlight`, `zb` and `epd`. Log lines that `tools/` matches must not change.
- `main/screen/bitmaps.c` is generated: never edit it by hand.

## Rules

- **Zigbee attribute ids and line icon ids are append-only**; identity, endpoint and cluster list are frozen
  ([docs/zigbee.md](docs/zigbee.md#identity-and-freeze-rules)). Breaking these forces a re-pair or breaks HA.
- **Every frame to the device carries at most 40 bytes of ZCL payload**, and writes are paced, never burst:
  design for a bad spot in the mesh.
- **NVS key strings never change** (they address state on devices).
- **Keep `ha/` and `z2m/` in sync with what runs in HA / Z2M** (`deploy-ha` skill).
- **Panel failures stay visible**: no automatic recovery until it is decided ([docs/roadmap.md](docs/roadmap.md)).
- **Docs**: when behaviour changes, update the design file that owns it, move what it replaces to
  `docs/history.md` if the reason matters, and keep each fact in one place.
- **Docs style**: short, clean and to the point. Readers may not be native English speakers: use simple
  grammar, and choose clarity over style.
- **Docs are for readers who do not speak Hungarian.** English comes first: screens by their names in
  [docs/uispec/ui-spec.md](docs/uispec/ui-spec.md#device-screens), in italics where mentioned (*All OK*,
  *Problems*, *Pairing*, ...), HA dashboards by their name. Where the Hungarian is worth showing (HA state words, a
  dashboard's title, example data in prose), it follows in parentheses: off (`kikapcsolva`), `dashboard-away`
  ("Bezárva?"). Exempt: docs/uispec/ui-spec.md (it specifies the Hungarian panel texts), code, code blocks and
  images.
