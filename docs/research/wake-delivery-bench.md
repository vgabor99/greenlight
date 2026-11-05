# Wake delivery bench

Bench log, 2026-10-14 to 2026-11-03: how reliably Zigbee frames reach the sleepy device right after a wake,
and where the device sits in the mesh. It follows the research in [zigbee-payload.md](zigbee-payload.md) and
is the evidence behind "one frame per wake" ([architecture.md](../architecture.md#data-model)). Kept as
recorded; terms of the time: the "snapshot" is the rev 5 text push, `shown_hash` its acknowledgement
([history.md](../history.md)). The tool is `tools/paced_write_test.py` (the `bench-test` skill).

## Planned tests (2026-10-14)

1. **Single-frame ceiling on our path**: one string write to a slot, length stepped from 60 to 80 bytes.
   Once with the device next to the coordinator, once forced behind a router (power it from a router-only spot
   or block the coordinator). Expect the edge at ZCL payload 79 and 73 to 75. Note the error Z2M logs at the edge
   (`MEM_ERROR`, `INVALID_PARAMETER`, `MAC_*`).
2. **Poll interval vs. push time**: full snapshot with `PARENT_POLL_MS` = 250, 100, 50, 20; log the time from first
   MQTT publish to `shown_hash` report.
3. **Paced vs. burst** ([option 2](zigbee-payload.md#6-options-ranked)): 20 full pushes each way at the chosen `PARENT_POLL_MS`, count failed writes (Z2M
   logs them on `bridge/logging`) and resends. Run it with the device at a poor spot (behind the weakest router
   it can join), since that is the case the design must survive.
4. (Optional, curiosity) a 200-byte string write through Z2M, to see whether this Z-Stack build fragments at
   all. A sniffer (a second ESP32-C6 or nRF52840 with the network key in Wireshark) shows the APS extended
   header if it does.

## Where the device sits in the mesh

Network map, 2026-10-14, without routing tables.

- **Parent: `gf_study_plug_ac`**, a Sonoff S60ZBTPF plug (Telink TLSR8656), not the coordinator. Link
  device to plug: LQI 165 (good). The device reports tree depth 3, the plug depth 2.
- **The plug has no usable direct link to the coordinator** (LQI 1). Its neighbours are mostly weak: LQI 20 to 59
  to the other plugs, and one strong link, LQI 240, to `gf_living_plug_ac`.
- So a frame from the coordinator to the device is source-routed through **at least 2 relays** (some router,
  then the study plug), and it is likely 2 relays exactly: at most 73 bytes per write here. This is **not the
  final location**, and the design does not depend on it. The [40-byte rule](zigbee-payload.md#21-one-802154-frame-coordinator-to-device-unicast-nwk-security) covers 8 relays.
- The weak links upstream of the plug mean MAC retries and the occasional route repair are normal on this
  path. That is the kind of location the design must tolerate: pace the writes and keep the tag-and-commit
  safety net ([options](zigbee-payload.md#6-options-ranked)).
- The parent is a cheap Telink-based plug with an unknown indirect queue size. That is the case [APS fragmentation](zigbee-payload.md#22-aps-fragmentation-zigbee-pro-optional-feature)
  warns about for APS fragmentation windows, and one more reason not to send bursts to this device.
- Test 1 on the bench runs on this real path as long as the device stays in the lobby. A test next to the
  coordinator needs the device moved and re-joined.

## Writes lost right after a restart

Opened 2026-10-14; the post-wake loss itself is still open (parked).

Paced snapshot test (6 lines + commit, 100 ms apart, 50 ms polling), device in the lobby:

- **Device kept awake**: 10 of 10 snapshots arrived first time, every write answered.
- **Reset before every round** (the real wake path: reboot, rejoin, then HA pushes on the wake counter): about
  half the rounds lose single writes. Each lost write is delivered only after Z2M's 10 s response timeout plus
  its route discovery (about 13 s), so a round takes 14 to 42 s instead of about 1.5 s, and in 1 of 10 rounds
  two writes failed outright (the commit was then rejected, as designed). The device also sends some frames
  twice, so it misses acks as well: the loss is in the direction coordinator to device.
- **Ruled out**: the e-paper refresh (same result with the panel never driven) and the time between the wake
  report and the first write (a longer settle made no difference).
- **The parent is a Tuya router, never the coordinator**, and a reboot may pick a different one: in one run it
  was `a4c13811672cffff` (0xc91e, LQI 77 to 95) for rounds 1 to 6, then it flipped between that one and
  `a4c138119b08ffff` (0x28b5, LQI 84 to 102). 3 of the 4 rounds after the first flip were bad, but round 6 lost
  4 writes with an unchanged parent, so switching is not the whole story. Z2M's timeout recovery logs
  `Node relation 0 (... / 65534)`: the coordinator has no child entry for the device.
- The firmware logs its neighbour table (`neighbours (restored|joined|heartbeat)`) to follow the parent.
- **Minimal push (2026-10-27, branch `exp/min-snapshot`, `tools/paced_write_test.py --reset-each`)**: does a tiny
  wake push avoid the loss? Each round is one write, a count-0 commit with a new hash (7 bytes of ZCL payload),
  against a control of 6 lines + commit (100 ms apart) in the same session. Parents again alternated between 0xc91e
  and 0x28b5.

  | | Rounds | Fast | Stalled (~13 s, Z2M retry) | Push time when fast |
  |---|---|---|---|---|
  | Commit only | 39 | 36 | 2 (+1 lost write response, see below) | 0.04-0.96 s, avg 0.17 s |
  | 6 lines + commit | 20 | 12 | 8 | 1.3-3.9 s |

  - **A stall stops delivery partway through the burst.** In a stalled round the first 0 to 3 writes arrive, then
    nothing more until all the rest arrive together 13 to 14 s after the first publish. It is not one write lost
    out of several.
  - **Stalls scale with the number of frames, not their size.** 2 stalls in 39 single-write wakes (5 %) and 8 in
    20 seven-write wakes (40 %). Both fit about 5 to 7 % per frame sent after the rejoin. A tiny push cuts stalls
    roughly 8-fold and makes the push about 10 times faster, but a single 7-byte write still stalls about 1 wake
    in 20.
  - **Not tied to one parent.** Commit only: 3 of the 4 problems were with 0x28b5 (13 rounds), 1 with 0xc91e (26
    rounds). Control: 7 of 8 stalls were with 0xc91e (10 rounds), 1 with 0x28b5 (10 rounds).
  - **The device → coordinator direction loses frames too.** Round 7's commit was accepted at once, but Z2M
    apparently never got the response: it resent the commit about 14 s later. The resend reached the device in the
    next wake (`already shown`) and held up that wake's commit (8.2 s). In one wake the wake counter report (the
    last report, HA's trigger) never reached Z2M, so no push would have happened. Z2M also received duplicate and
    reordered reports in several wakes. Z2M logged no error for any of these, because its retries succeeded.
  - The panel failed at boot twice before the run (`timeout waiting for BUSY`, `busy before 0`, which stops the
    firmware in BOOT). A reboot cleared it.
- **Rev 6 firmware (2026-10-27, branch `dataset-view`)**: one 4-record view write per wake (28 bytes), the
  8-line dataset already stored, `--reset-each`, 39 wakes (round 1 is not counted: the script re-sent the
  dataset before the view):

  | | Wakes | Notes |
  |---|---|---|
  | Fast | 34 | 0.09-1.26 s, avg 0.47 s |
  | Stalled, then delivered by Z2M's retry | 3 | 13-15 s, all with parent 0x28b5 |
  | Lost | 2 | Z2M gave up: after its 10 s timeout the coordinator refused the route discovery with `ZDO extRouteDisc ... 0xc7: NWK_TABLE_FULL`. HA's resend would have to catch these. |

  The 4-record write always arrived as one frame (no `incomplete`). The uplink was clean: no missed wake
  counter, no stale resend. The stall rate (3 of 39) matches the single 7-byte commit (2 of 39), so a
  28-byte frame is no worse. `NWK_TABLE_FULL` is new and coordinator-side (Z-Stack's route request table). It
  may come from the test resetting the device every ~10 s; to check against normal wake rates. Parked
  ([roadmap Q6](../roadmap.md#open-questions)); `VIEW_WAIT_MS` (45 s) keeps the device awake for HA's resend.
- **Rev 6 path checks (2026-10-27, `--reset-each`, 0 Z2M errors, no `incomplete` view)**:
  - `--no-view`: with no view the device stayed in ACTIVE 45.0 s (`VIEW_WAIT_MS`) instead of 20 s. (The
    second wake hit the panel BUSY timeout at boot, a hardware fault unrelated to this.)
  - `--resync-each`, 9 new 8-line datasets, each followed by its view: all stored and all views accepted.
    Dataset time 1.8-4.1 s in 6 wakes, 20.5-29.5 s in 3 (stalls inside the 9-frame burst); the view after
    it took 0.3-0.7 s, once 13.6 s. The device stayed awake through every sync (each entry is activity). So
    a dataset change costs about 1 wake in 3 a 20-30 s delay, which is acceptable for a rare event.
  - `--oos`, 3 views for a dataset the device does not have: all rejected as out of sync, `shown_view_id`
    unchanged, `dataset_hash` reported.
  - `--same`, the same view id 3 times: first `accepted`, then `time only` twice (bits ignored, as the id
    defines the content).
- **`WAKE_REPORTS_DELAY_MS` 3 s -> 2 s (2026-11-03, `--reset-each`, 39 wakes, 0 Z2M errors)**: does reporting the rest
  of the wake 1 s sooner hold up the view? 35 fast (0.05-0.56 s), 4 stalled and delivered by Z2M's retry
  (13.2 s, 3 of them with parent 0x28b5), none lost. The rest went out 1.24-1.39 s after the view write in
  every wake, after every fast push had landed; in the 4 stalls the write was already overdue by then, so the
  reports did not cause them. Kept at 2 s (it brings `awake` and occupancy to HA 1 s sooner). The test pushes
  0.5 s after the wake counter; HA's own delay before its push is not measured.
- **Next test**: device next to the coordinator, so that it rejoins with the coordinator as parent, then the
  same reset-each run. Clean: the Tuya routers' handling of a rejoined sleepy child is the cause. Still lossy:
  look at the device's stack after a reboot.

