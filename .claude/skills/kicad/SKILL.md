---
name: kicad
description: "Work on greenlight's KiCad 10 project in hardware/ - the schematic, the PCB, the project libraries (Seeed XIAO ESP32-C6 symbol and footprints) - with kicad-cli (ERC, DRC, PDF/SVG/Gerber export, 3D render) and KiCad's Python (pcbnew). Use before reading or changing any file under hardware/, when checking the wiring against docs/hardware.md or main/board/board.h, or when exporting the schematic or board to look at."
---

# KiCad project

The project is `hardware/greenlight.kicad_pro`, one schematic sheet (`greenlight.kicad_sch`) and one board
(`greenlight.kicad_pcb`). KiCad 10.0 is installed under `C:\Program Files\KiCad\10.0`, set to English. Read
check results from the JSON reports rather than the console text.

| What | Where |
|---|---|
| CLI | `"/c/Program Files/KiCad/10.0/bin/kicad-cli.exe"` |
| Python with `pcbnew` | `"/c/Program Files/KiCad/10.0/bin/python.exe"` (run with `-I`, script outside the repo) |
| Project libraries | `hardware/lib/`, listed in `hardware/sym-lib-table` and `hardware/fp-lib-table` as `${KIPRJMOD}/lib/...`; source and license in `hardware/lib/README.md` |
| Standard libraries | KiCad's global tables (`%APPDATA%\kicad\10.0`), nickname = library name (`Device`, `Connector_PinHeader_2.54mm`, ...) |

The wiring follows docs/hardware.md#pins, which follows `main/board/board.h`. A change to a pin goes into all
three; if the schematic and the docs disagree, ask which one is right.

## Before editing a file

KiCad does not notice changes made on disk to a file it has open, and saving from the GUI overwrites them. Before
editing `X.kicad_sch` or `X.kicad_pcb`, check for its lock file `hardware/~X.lck` (for example
`~greenlight.kicad_sch.lck`); if it exists, ask the user to close that editor (or save and close) first. After
editing, tell the user to reopen it.

## Check and look

Write outputs to the scratchpad, not into `hardware/`.

```
KC="/c/Program Files/KiCad/10.0/bin/kicad-cli.exe"
"$KC" sch erc --format json --severity-all -o <out>/erc.json hardware/greenlight.kicad_sch
"$KC" pcb drc --format json --severity-all --schematic-parity -o <out>/drc.json hardware/greenlight.kicad_pcb
"$KC" sch export pdf -o <out>/sch.pdf hardware/greenlight.kicad_sch       # then Read the PDF to see it
"$KC" pcb export svg --layers F.Cu,B.Cu,F.SilkS,Edge.Cuts --mode-single -o <out>/pcb.svg hardware/greenlight.kicad_pcb
"$KC" pcb render --side top -o <out>/pcb.png hardware/greenlight.kicad_pcb  # 3D view as PNG
```

Run ERC after every schematic edit and DRC after every board edit: they are also the parse check for a
hand-edited file (a broken file fails to load). An ERC/DRC violation that is intended gets an exclusion with a
comment in the GUI, not a silent ignore.

## Editing the schematic as text

`.kicad_sch` is an S-expression file. Rules that keep KiCad happy:

- Every placed symbol needs its definition copied into `(lib_symbols ...)` under the full `"Lib:Name"` id, taken
  from the library file (`hardware/lib/...` or `C:\Program Files\KiCad\10.0\share\kicad\symbols\<Lib>.kicad_sym`).
- Every symbol, wire, label and junction gets a fresh UUID; every symbol an `(instances (project "greenlight"
  (path "/<sheet uuid>" (reference "R1") (unit 1))))` block.
- Coordinates are in mm on the 1.27 mm (50 mil) grid; a wire connects only where its end lands exactly on a pin
  end. Prefer short wires with net labels over long wires: they are easier to get right as text and to read.
  A signal net is named after its define in `main/board/board.h` (`PIR_PIN`, `EPAPER_PIN_BUSY`, ...).
- After the edit: ERC, then export the PDF and look at it.

For anything bigger than a few symbols, propose the change and let the user place it in the GUI, or build it in
small steps with an ERC and a PDF after each.

## The board

- The board is hand-edited (tracks tidied in the PCB Editor): never rebuild it from the netlist or re-route
  it whole, which would throw that work away. Make targeted changes (one footprint, one zone, one text) with
  `pcbnew`, or ask the user to do them in the GUI.
- After the user edits the board, check that the rule areas are all still there (`XIAO`, `battery`,
  `panel parts`, `edges`, `edge bottom`, `edge left`, `edge right`, `hole 1`-`hole 4`): one is easily deleted
  by accident. Then DRC with `--refill-zones --schematic-parity`. The `XIAO` area (no fill) also covers the
  antenna; it does not keep tracks out, so check that none runs under the XIAO's left (antenna) end.
- KiCad 10 keeps a local history in `hardware/.history/` (its own git repository; ignored here).
- Footprints come into the board from the schematic only through the GUI ("Update PCB from Schematic", F8);
  kicad-cli cannot do it. Ask the user to run it after schematic changes, then check with DRC
  `--schematic-parity`.
- Placement, outline (`Edge.Cuts`), tracks and zones can be scripted with `pcbnew` in KiCad's Python
  (`pcbnew.LoadBoard()`, change, `board.Save()`), with KiCad closed; then DRC and the SVG/PNG export.
- Routing: Freerouting 2.5 at `C:\Tools\freerouting\freerouting-2.5.0.jar`, run with Java at
  `C:\Program Files\Eclipse Adoptium\jre-25.0.5.8-hotspot\bin\java.exe` (not on the Git Bash PATH). With KiCad
  closed:
  1. Export with `pcbnew.ExportSpecctraDSN(board, "x.dsn")` after removing, in memory only, the tracks, the
     copper fills and every rule area that allows tracks. KiCad exports any rule area as a track keep-out, and a
     fill as a plane, which makes Freerouting leave the pads on that net unrouted.
  2. `java -jar freerouting-2.5.0.jar -de x.dsn -do x.ses --router.autorouter.max_passes=30 -l en --gui.enabled=false`
  3. In a new process: load the board, add the GND fills, then `pcbnew.ImportSpecctraSES(board, "x.ses")` and save.
     Do every zone operation before the import: afterwards KiCad's Python hands out untyped objects, and filling
     in the same process crashes it.
  4. In another new process: fill the zones (`pcbnew.ZONE_FILLER`), save, DRC.
  Track widths come from the net classes in `greenlight.kicad_pro` (Default 0.3 mm, Power 0.5 mm for +3V3 and GND).
- KiCad's Python does not expose line styles: a dashed line is written into the `.kicad_pcb` as
  text (`(stroke (width 0.15) (type dash))`), with KiCad closed.
- Mechanical constraints (outline, heights, keep-outs, the Fusion coordinate mapping, assembly) are in
  docs/hardware.md#board. Check a placement against them and against `hardware/mechanical/pcb-layout.dxf`.
- SMD parts are placed by JLCPCB: basic parts only, top side only, with the part number in the symbol's `LCSC`
  field. A new SMD part needs a basic LCSC part (look it up, for example on jlcsearch.tscircuit.com with
  `is_basic=true`).
