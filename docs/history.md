# History and lessons

How the design got to where it is: past designs, rejected options and the lessons behind them, with the
numbers that decided them. The current design is in the other files of [docs/](README.md); git history has the
full detail.

## Data model revisions

### Rev 5: text snapshot (2026-10-13)

HA pushed the finished screen as text: 6 tagged line slots, a footer and a commit, 8 writes per push, attributes
0x0000-0x0007 (now retired). No external converter: the state was in standard clusters (Binary Input for
`awake`, Analog Input for the hashes, on extra endpoints because an Analog Input holds one float per endpoint),
which is also why ids and hashes are 24 bits (a float's exact integer range). The tag-and-commit scheme was born
here: a lost write costs a resend, never a wrong screen.

- Bench: a full push took about 3.5 s at 250 ms polling. Faster polling (50 ms) and pacing the writes 100 ms
  apart brought it to about 1.5 s with the device kept awake ([research](research/zigbee-payload.md#6-options-ranked)).
- But after a wake (reboot + rejoin) about half the pushes stalled: each lost write costs a 13 s Z2M recovery.
  6-line pushes stalled in 8 of 20 wakes, a single 7-byte write in 2 of 39: about 5 to 7 % per frame sent after
  the rejoin ([wake delivery bench](research/wake-delivery-bench.md)).

### Rev 6: dataset + view (2026-10-27)

The lesson of rev 5: after a wake, the number of frames matters, not their size. So the text moved into a
**dataset** synced rarely, and each wake gets one **view** write: a bitfield over the dataset in a single
4-record frame. Bench: 34 of 39 wakes got their view in 0.09-1.26 s; 3 stalled (13-15 s), 2 were lost
(`NWK_TABLE_FULL`, [roadmap.md](roadmap.md#open-questions)), so `VIEW_WAIT_MS` (45 s) keeps the device awake for
HA's resend. `dataset_hash` lived on a third endpoint. The same day: Hungarian accents on the display, and lines
up to 64 bytes in up to 3 parts.

### Rev 7: one endpoint and a converter (2026-10-29)

Z2M's generated definition for a device with several endpoints is broken for occupancy: it exposes `occupancy`
but its parser publishes `occupancy_1`, so the HA entity stayed null; the other state got meaningless names
(`analog_input_1/2/3`, `binary_input_1`). With a converter Z2M can publish custom-cluster attributes, so the
device state moved into 0xFC8A, endpoints 2 and 3 and the Analog / Binary Input clusters went, and the OTA client
was declared so the cluster list is final. Needed a re-pair; the freeze rules apply from here
([zigbee.md](zigbee.md#identity-and-freeze-rules)). HA's dataset and view templates and the push automation went
live the same day.

### Rev 8: the designed screen (2026-10-31)

The proper "Bezárva?" screen from the UI spec replaced the debug screen, which stays behind a build switch. The
line format became `II|name|state` with an icon id from an append-only table; fonts (Inter) and icons (MDI) are
generated bitmaps instead of u8g2 fonts.

### Since rev 8

- 2026-11-01: the view's time on screen was replaced by the staleness badge (the viewer should not have to work
  out how old a time is), with HA pushing every 5 s while awake as the liveness signal. Expected conditions at
  the install spot (lobby door, its lock, the disarmed alarm; later the night light) are masked out in HA.
- 2026-11-02: low battery with a simulated battery; exact voltage via `battery_mv`.
- 2026-11-03: the real PIR replaced the stand-in button; real deep sleep replaced the simulated one (31 wakes in
  35 min, each rejoined 1.4-1.5 s after boot); `WAKE_REPORTS_DELAY_MS` 3 s -> 2 s (no effect on the view push in
  39 wakes, brings `awake` to HA 1 s sooner); UNPAIRED shows how to pair instead of "Csatlakozási hiba".
- 2026-11-04: the sources were split into one file per purpose and grouped into folders; `INTERVIEW_MS` got the
  full minute.

## Rejected options

| Option | Why not |
|---|---|
| rx-on-when-idle while awake | lost about half of Z2M's writes; a sleepy end device polling every 50 ms works |
| Generic Z2M mapping, no converter (rev 5-6) | cannot publish custom clusters; broke occupancy on a multi-endpoint device |
| Two lines per write, or more per frame | about 62 bytes: fails beyond about 2 relays; the 40-byte rule wins over a few saved frames |
| APS fragmentation (one 200-byte write) | not usable through Z2M + Z-Stack; unproven TI-to-ZBOSS; bursty at the parent ([research](research/zigbee-payload.md#23-why-fragmentation-is-off-the-table-for-now)) |
| Leaving Zigbee for bulk data (OpenEPaperLink style) | gives up the mesh, Z2M and HA; out of scope |
| Trigger HA's push on `awake` or on any device message | a lost `awake = false` hides the next wake; the device announce comes before it is ready ([zigbee.md](zigbee.md#when-the-device-reports)) |
| Showing the view's time | replaced by the staleness badge |
| `IDLE_TIMEOUT_MS` 5 s | too short to see a push arrive after a wake (up to about 20 s) |
| A "monitoring" LOW_BATTERY state forced at any transition | monitoring means staying awake or waking by timer, and after real deep sleep every check is a boot anyway; a transition-only check misses ACTIVE and UNPAIRED, which can run indefinitely; and the cleanup depends on the state |
| Retained RAM for the dataset and view | NVS survives a battery change too, and one write per wake is negligible wear |
| Holding view writes in Z2M while asleep | parked, not rejected: [roadmap.md](roadmap.md#future-work) |

## E-paper driver

What was tried on the SSD1683 panel and did not work (what works is in
[hardware.md](hardware.md#e-paper-panel)):

- The real full-refresh waveform (mode 1, `0x22 = 0xF7`, also `0xC7` / `0x91`) never finishes, even with `0x01`
  driver output control and `0x18 = 0x80` set.
- A custom full-refresh waveform (Waveshare's 4-gray `LUT_ALL` via `0x32`, `0x22 = 0xCF`) hung as well, so it was
  removed.
- Border `0x3C = 0x80` (Waveshare's value): the border drifts to black/grey over many updates.
- Rewriting old RAM (`0x26`) with the previous frame before a partial: no difference.
- More drive makes the white background grainier, not cleaner: 10 black/white cycles, 12 white passes and going
  to black first were all worse than one white pass. A checkerboard stage made the grain worse too, and with
  1-pixel squares BUSY hung (suspected supply sag).
- The BUSY hang was first suspected to come from radio TX, pixel density, refresh count or time awake; all ruled
  out by the 2026-10-15 stress test, which traced it to the display GND contact.

## Lessons

- **Register string attributes with a full-length initial value.** The stack sizes a string attribute from its
  registration value; with a zero-length string every second write longer than 11 characters was refused with
  `UNSUPPORTED_ATTRIBUTE` (2026-10-13).
- **Z-Stack's `MEM_ERROR (0x10)`** on a 77-byte batch write meant "too big for this route", not "too many
  records": 77 + 3 bytes of ZCL header left no room for the source route, and Z-Stack's fragmentation path failed.
- **A payload that works next to the coordinator can fail behind two routers.** Size every frame for the worst
  case path.
- **HA's awake gap**: `awake` = true follows the wake counter by a few seconds (8 s seen). A lamp turned on 4.4 s
  after a wake never reached the panel (2026-10-29) until the automation's gate also accepted "the wake counter
  changed after `awake` did".
- **Z2M converter files must belong to Z2M's user**: with root ownership loading fails with `EACCES`
  ("Invalid external converter ... was ignored").
- **A full hex framebuffer dump took 3 s per frame** on the console and made HA's dataset commits time out;
  `SCREEN_DUMP` prints only the non-white runs and is off in normal builds.
- **1 px fonts are too faint on this panel**: the debug screen uses Terminus Bold.
- **Bench writes need the serial log running first**, and the dataset hash must come from the device, not an
  old log: 20 views with a stale hash were all out of sync, unnoticed (2026-10-27).
