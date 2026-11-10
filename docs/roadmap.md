# Status and roadmap

## Status

As of 2026-11-10, working end to end on the bench, with the device in the lobby:

- **Real**: the e-paper driver and the designed screens, the PIR and the pair button, the state machine, deep
  sleep with wake on D0 / D1, Zigbee (pairing, rejoin, reports, view and dataset writes), the Z2M converter,
  the HA templates and push automation, and the battery measurement on D2 with the confirmed low-battery report
  (tested by pulling the divider during a wake and before one).
- **Simulated**: USB power (set over the console), and with it LOW_BATTERY's wake
  ([firmware.md](firmware.md#simulated-parts)). The USB sense on D7 is wired (82k/140k on the breadboard) and
  read correctly, but only logged: using it, with LOW_BATTERY's timer wake, is the next step.
- **Breadboard**: the display GND contact still causes occasional BUSY hangs
  ([hardware.md](hardware.md#busy-hang)); the battery divider is 681k/681k with 100 nF, not the PCB's 1M/1M.

## Open questions

- **Q1 Rejoin time over many wakes.** 1.4-1.5 s after boot in the deep-sleep test (31 wakes). A rejoin after a
  long sleep works ([behaviour.md](behaviour.md#deep-sleep)); its time, and the time end to end (wake to view on
  screen), are still to measure. `join_time_ms` would make it visible in HA.
- **Q2 Retained RAM across `esp_restart()`.** Fine across deep sleep on the C6, unverified across a software
  restart. Only matters for flash wear, not for correctness.
- **Q4 OTA.** The OTA client cluster is declared, so the cluster list never changes; the partition table (two
  app slots in 2 MB) is a later, USB-flashed change.
- **Q6 `NWK_TABLE_FULL` (parked).** The coordinator sometimes refuses Z2M's route discovery to the device with
  `ZDO extRouteDisc ... 0xc7: NWK_TABLE_FULL` (Z-Stack's route request table), and Z2M then drops the write: 2 of
  39 wakes on the rev 6 bench. Possibly caused by the test resetting the device every ~10 s; to check at a normal
  wake rate. `VIEW_WAIT_MS` plus HA's resend cover it meanwhile.

(Q3, the converter question, and Q5, rx-on-when-idle vs polling, are settled: [history.md](history.md).)

## Remaining checks

- The frame counter survives deep sleep (the SDK should persist it).
- End-to-end latency from a wake to the view on screen, including HA's own delay before its push.
- Wake delivery with the device next to the coordinator: if clean, the Tuya routers' handling of a rejoined
  sleepy child is the cause of the post-wake losses; if still lossy, look at the device's stack after a reboot
  ([wake delivery bench](research/wake-delivery-bench.md)).

## Future work

**Hardware**: the PCB, whose soldered display GND fixes the BUSY hang ([hardware.md](hardware.md#busy-hang)); the
sleep current measured on the battery, including whether the panel module's LDO (about 90 uA if it stays on) is
off while RST is held low; the dividers
on the PCB ([hardware.md](hardware.md#power-and-battery)).

**USB sense divider values** (decide after the sleep current is measured): on battery the XIAO's 5V pin sits at
about 0.33 V from a stiff leak (about 20k behind it), so the USB sense divider draws about 1.2 uA in deep sleep
with 100k/150k. 1M/1.5M would cut that to about 0.14 uA and still read a clean low (about 0.2 V on D7); the
cost is a USB reading that lingers a few seconds after unplugging. Worth it only if the measured sleep current
is in the single-digit uA.

**USB power**: use the D7 reading instead of the console's simulation, and make LOW_BATTERY deep sleep with a
timer wake that checks it.

**Reserved attributes** (0xFC8A; append-only ids, so they can be added without a re-pair; each also needs a line
in the converter):

| Id | Name | Type | Meaning |
|---|---|---|---|
| 0x0010 | `join_time_ms` | uint16 | boot to joined of the latest wake (Q1) |
| 0x0011 | `wake_reason` | enum8 | 0 cold boot, 1 PIR, 2 pair button, 3 other / unknown |
| 0x0012 | `last_awake_s` | uint16 | duration of the previous wake cycle, for battery life insight |
| 0x0013 | `battery_mv` | uint16 | in use: the exact battery voltage |
| 0x0014 | `panel_error` | enum8 | 0 ok, 1 panel init failed, 2 refresh failed, 3 BUSY timeout |

**Panel recovery** (once the hang's cause is fixed or its rate is known to be acceptable): on a BUSY timeout,
`epaper_init()` + `epaper_clear()` + redraw (about 1.5 s), with the timeout cut to about 2 s (a refresh takes
470 ms), and count recoveries in the log or in `panel_error`.

**Panel optimizations**:
- Do not touch the panel on wake if nothing changed (clear lazily, only when a redraw is needed).
- Skip the final clean refresh at SHUTDOWN when no partial update happened since the last one.

**Screens rendered on the PC.** Build the screen code (`main/screen/`: `status_screen.c`, `render.c`,
`bitmap_text.c`, `bitmaps.c`; no ESP-IDF dependencies) for the PC, render each [mockup](uispec/ui-spec.md)'s content
to PNG, and write a mockup | firmware | diff image per screen. A layout change could then be checked without
flashing, and a new screen designed as a firmware change. Needs a C compiler for the PC (e.g. MinGW-w64); the
sample content now in `frame_dump_samples()` would move to a file both builds share.

**Grey levels.** The panel (Waveshare 4.2" V2, SSD1683) has a 4-level grey mode in Waveshare's driver
(`Init_4Gray` / `display_4Gray`); the firmware and the [UI spec](uispec/ui-spec.md#panel) use 1 bit today. Grey
could anti-alias the text and icons, and de-emphasise the separators. To check first:
- that it works on our panel with our driver ([hardware.md](hardware.md#e-paper-panel));
- refresh: Waveshare's 4-grey path uses a full refresh only, while every redraw is a partial refresh today
  (about 470 ms, no flashing; [display.md](display.md#refresh-policy));
- memory: 2 bits per pixel doubles the framebuffer to 30 KB;
- glanceability: *All OK* must stay the only large black shape, so grey stays out of large areas.

The mockups would move to 4 levels as well (`tools/gen_mockups.py` thresholds to 1 bit today).

**OTA** firmware updates (Q4). A friendlier converter `toZigbee` (one `view` message, one `dataset` message).

**Hold view writes in Z2M while asleep** (only if the wake push turns out too slow once deployed). HA would send
every view change, asleep or not, and zigbee-herdsman would deliver it on the next frame from the device (the
device announce, about 0.5 s after the reboot, before the wake counter). That saves maybe 0.5-1 s per wake. From
the herdsman source (checked 2026-10-29): a write whose delivery fails is queued and sent on the device's next
message, and a newer write to the same attributes replaces the queued one (`keep-command`), but only if the
device's `pendingRequestTimeout` is > 0, which herdsman sets only for Poll Control devices or a known check-in
interval; for ours it is 0 and the write is just lost. Needed:
- the converter sets `pendingRequestTimeout` (e.g. 1 day) on every Z2M start (it is not persisted);
- the automation sends view changes without the `awake` gate, and without the ack wait and resends while asleep
  (otherwise each change blocks the queued automation for 3 x 20 s and delays the next wake push);
- dataset writes get `options: {sendPolicy: 'immediate'}` in the MQTT `write` (Z2M passes `options` through):
  every `ds_entry` part writes the same attribute 0x0030, so queued parts would replace each other.

Drawbacks: every change while asleep puts a frame in the parent's indirect queue for about 8 s before it fails
and is held (the small-router overflow of [research](research/zigbee-payload.md#22-aps-fragmentation-zigbee-pro-optional-feature)),
and the display sleeps most of the time while lights change often; Z2M logs a failed send per change while
asleep and a "superseded" error per replaced write; writes would land during BOOT / REJOIN, an untested firmware
path; and the wake push still sends the view again with the new time, so a wake gets two refreshes.
