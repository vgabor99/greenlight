---
name: panel-troubleshoot
description: "Diagnose greenlight's e-paper panel - a grey, garbled, inverted, ghosting or frozen screen, `panel: ... FAILED` or `busy before 1` in the log, BUSY timeouts, or changes to the SSD1683 driver (main/panel/epaper.c, panel.c). Use before changing refresh or clear code, or when a panel failure shows up in a test."
---

# Panel troubleshooting

What works on this panel and why: docs/hardware.md#e-paper-panel. What was tried and failed (do not retry
without a new reason): docs/history.md#e-paper-driver.

## Reading the log

Every clear and refresh logs one line (`main/panel/panel.c`; don't change the format, tools match log lines):

```
panel: clear (<reason>) <ms> ms, busy before <0|1>[, FAILED]
panel: load (sleep frame) <ms> ms, busy before <0|1>, partials <n>
panel: refresh #<partials since clear> (<label>) <ms> ms, busy before <0|1>, black <n>, changed <n>[, FAILED]
```

| Seen | Meaning |
|---|---|
| clear about 500 ms, refresh about 470 ms | normal, whatever the pixel count |
| `load (sleep frame)` at boot instead of `clear (boot)` | normal after a deep sleep: the frame from the last wake went into the controller, no update. `clear (boot)` after a power-on, flash, crash or panel failure |
| `busy before 1` | the controller is still stuck from an earlier hang: every command fails until a hardware reset |
| `timeout waiting for BUSY` (tag `epd`) and `, FAILED` after 10 s | a refresh never finished: a BUSY hang |
| `busy before 0` at boot followed by a timeout | the panel failed at init; a reboot usually clears it. The wake ends at once (`panel failed: ending the wake`, `BOOT -> SHUTDOWN`), so the next wake retries |

## BUSY hang

Symptom, cause (a bad display GND contact on the breadboard) and the hardware fix still to do:
docs/hardware.md#busy-hang. So:

1. Ask whether the board or wires were touched during the refresh before suspecting the code.
2. Don't touch the board during refreshes in a test.
3. Recovery: reset the board (the next wake's `epaper_init()` resets the panel). With real deep sleep the next
   wake does it.
4. The firmware does not recover on purpose, so failures stay visible (docs/behaviour.md#failure-modes);
   automatic recovery is planned in docs/roadmap.md#future-work. Don't add it unasked.

## Other symptoms

- **Inverted screen**: the update was re-triggered without rewriting both RAM buffers (mode 2 drives only pixels
  where `0x24` != `0x26`, and the controller scrambles them after each update; `drive_pass()` rewrites both).
- **Border drifting dark**: border register must be `0x3C = 0x05`, not `0x80`.
- **Ghosting**: expected to grow until the full clear every `PARTIALS_BETWEEN_CLEARS` (50) partials.
- **Grain on white**: normal in small amounts; more drive passes make it worse, not better.
- **BUSY never connected**: BUSY (D3 / GPIO21) must be wired; an internal-pull test cannot tell, the module
  drives BUSY weakly.
