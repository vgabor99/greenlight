# How much data fits in one Zigbee transaction

Research note, 2026-10-14. Question: the snapshot push (DESIGN.md 6.5: 6 lines + footer + commit, 8 writes,
3.5 s on the bench) is slow and has many points of failure. What is the largest payload that can be sent
reliably to the device in one transaction, what exists to go further, and what each option costs in
reliability, latency and mesh stability.

Our setup, because the answer depends on it: Z-Stack 3.x coordinator (Koenkk firmware `ZStack3x0` revision
20260310, TI IEEE prefix 0x00124b, channel 25, transmit power 20 dBm) behind Zigbee2MQTT 2.12.1 (zigbee-herdsman
10.6.1, `zstack` adapter, default concurrency), in a mesh of 47 devices (14 routers, 32 end devices). The device is
`gf_lobby_greenlight` (0x10bda3fffea02724), a child of the router `gf_study_plug_ac`, at least 2 relays from
the coordinator (7.1), the ESP32-C6 as a **sleepy end device** (ZBOSS via esp-zigbee-lib 2.0.4,
`rx_on_when_idle = false`, polls its parent every 250 ms while awake). NWK security on, no APS security. Payload of
a full snapshot: about 6 x 27 + 30 + 4 = about 200 bytes.

## 1. Short answer

| Transfer | Practical maximum (ZCL payload) | Status for us |
|---|---|---|
| One unfragmented frame, device is a direct child of the coordinator | about **79 bytes** (82-byte APS payload minus the 3-byte ZCL header) | works |
| Same, device behind *n* routers (the coordinator source-routes) | 79 - 2 - 2n bytes, so **73 bytes with 2 relays**, 69 with 4 | works |
| Worst case we design for: 8 relays (+18), both IEEE addresses in the NWK header (+16), manufacturer code (+2) | 79 - 36 = **43 bytes** | |
| **Design limit** for a single ZCL frame | **40 bytes** (the worst case minus a small margin) | **adopted** (rule in 2.1) |
| APS fragmentation (one ZCL frame split by the APS layer) | spec: up to 255 blocks; ZBOSS on the device: 8 blocks, about **600 bytes**; Digi/Ember hosts: 255 bytes | **not usable from Z2M + Z-Stack today** (2.3) |
| Application-level chunking (what we do now, OTA cluster, Partition cluster) | unlimited | works, cost grows with the number of round trips |

The 77-byte batch write that Z-Stack refused with `MEM_ERROR (0x10)` (DESIGN.md 6.5) fits this picture: 77 bytes
of attributes + 3 bytes ZCL header = 80-byte ASDU. That leaves no room for a source route, so Z-Stack took its
fragmentation path, and that path fails with `0x10` (2.3). The batch was 1 to 6 bytes too big, not far off.

**Conclusion.** One transaction cannot reliably carry the whole 200-byte snapshot with this coordinator and
Z2M. The real problem is the number of round trips, not their size, and the radio airtime is negligible (4). The
biggest gains come from faster round trips that don't burst (section 6): poll faster while awake, and pace the
writes so the parent router holds at most one or two of them. The device's location is not assumed to be good:
every frame stays at or below 40 bytes of ZCL payload, which holds on any realistic path. One tagged line per
write (31 bytes) already fits, so the current one-line-per-write layout stays. Packing more per frame would save a
few bytes and messages, at the price of failing on long paths. Not worth it.

## 2. Limits in detail

### 2.1 One 802.15.4 frame (coordinator to device, unicast, NWK security)

| Layer | Bytes | Notes |
|---|---|---|
| PHY payload (aMaxPHYPacketSize) | 127 | |
| MAC header + FCS | -11 | short addresses, PAN-ID compression |
| NWK header | -8 | +8 per IEEE address the stack adds; +2+2n with a source route of n relays |
| NWK security (aux header 14 + MIC 4) | -18 | always on in Zigbee 3.0 |
| APS header | -8 | unicast, endpoint addressing; +2 with fragmentation (extended header) |
| **APS payload (ASDU)** | **82** | ZBOSS `ZB_APS_GUARANTEED_PAYLOAD_SIZE_WO_SECURITY`, Ember `MAXIMUM_APS_PAYLOAD_LENGTH` |
| ZCL header | -3 | -5 if manufacturer-specific |
| **ZCL payload** | **79** | |
| APS security, if used | -9 | not used for our ZCL traffic |

A ZCL Write Attributes record costs 4 bytes plus the value for a char string (id 2, type 1, length 1), so one
string attribute can hold at most 75 characters in a perfect case. With a 2-relay source route it can hold 69.

Vendor figures agree: Digi documents 84 bytes unencrypted and 66 bytes NWK-encrypted for XBee unicast. Nordic and
TI forum reports put the largest working frame at 78 to 81 bytes, failing earlier once source routing is involved.
Z-Stack computes the limit per destination (`afDataReqMTU`), so it changes with the route. Our coordinator
firmware runs as a concentrator with source routing (`CONCENTRATOR_ENABLE TRUE`, `MAX_RTG_SRC_ENTRIES 250`), so
any device not directly attached to it gets a smaller limit. **A payload that works on the bench next to the
coordinator can fail in the house behind two routers.**

**Rule: the location of the device is not assumed to be good.** Every frame to the device must fit the worst
case: 8 relays, both IEEE addresses in the NWK header (stacks add them, for example after an address conflict), and
a manufacturer code. That leaves 43 bytes. **The design limit is 40 bytes of ZCL payload per frame**, checked
for every message type the device accepts:

| Message | ZCL payload | |
|---|---|---|
| One tagged line (`HHHHHH\|` + about 21 characters, 4-byte record header) | about 31 bytes; 40 at most for a 29-character line | fits; HA must cap the line length so this holds |
| Footer (about 30 characters) | about 34 bytes | fits |
| Commit (uint32) | 7 bytes | fits |
| Line + commit in one write (option 3) | about 38 bytes | fits, barely; only with the line length cap |
| Two lines in one write | about 62 bytes | **does not fit; rejected** |
| Rev 6 view write (`view_bits`, `view_ds`, `view_time`, `view_id`, 4 x uint32) | 28 bytes | fits (DESIGN.md 6.5) |
| Rev 6 dataset entry (`HHHHHH\|II\|` + 26 characters) | 40 bytes at most | fits, at the limit; the 26-character cap is what keeps it there |
| Rev 6 dataset commit (uint32) | 7 bytes | fits |

### 2.2 APS fragmentation (Zigbee PRO, optional feature)

How it works: the sender splits one ASDU into blocks of one frame each, with an APS extended header (block
number, and the block count in the first block). It sends up to `apsMaxWindowSize` blocks (1 to 8). The
receiver acknowledges each window with a bitmap APS ack, and only missing blocks are resent. It is unicast only,
needs APS acks, and both ends must support it. The receiver's node descriptor advertises a "maximum incoming
transfer size". The feature is mandatory in Smart Energy (default 128-byte ASDU), optional elsewhere, and rarely
used in home automation.

Support along our path:

| Component | Fragmentation | Evidence |
|---|---|---|
| ESP32-C6 (ZBOSS in esp-zigbee-lib 2.0.4) | **Supported for receive and send.** `APS_FRAGMENTATION` is defined, max 8 fragments (`ZB_APS_MAX_FRAGMENT_NUM`), 1500-byte reassembly buffer, 3 parallel incoming transfers, window size and inter-frame delay configurable (`ezb_aps_set_fragment_max_window_size`, `ezb_aps_set_fragment_interframe_delay`) | `managed_components/espressif__esp-zboss-lib/include/zb_config_common.h`, `zb_config.h`, `espressif__esp-zigbee-lib/include/ezbee/aps.h` |
| Z-Stack coordinator | Compile-time option (`ZIGBEE_FRAGMENTATION`), closed library. Known problems: `afStatus_MEM_FAIL (0x10)` on large sends, and fragmented sends do not account for the source-route header (a TI E2E thread exists only to patch that in). Koenkk's `firmware.patch` does not touch it. | TI E2E threads (sources), our own `MEM_ERROR` |
| zigbee-herdsman `zstack` adapter | **Receive**: fixed in 2025; large `AF_INCOMING_MSG_EXT` is now fetched with `AF_DATA_RETRIEVE` (`dataRetrieveAll`), reported working under about 220 bytes. **Send**: ZCL unicasts go through plain `AF_DATA_REQUEST` with `options = 0` (no APS ack requested), and herdsman never asks for fragmentation. Whether Z-Stack fragments that request on its own is unknown, and it is what returned `MEM_ERROR` for us. | `src/adapter/z-stack/adapter/zStackAdapter.ts` (`dataRequest`), Z2M discussion #28395 |
| zigbee-herdsman `ember` adapter | No fragmentation (in EmberZNet it is a host-side library that herdsman does not implement). Hard cap at 82 bytes. | `src/adapter/ember/consts.ts` |

Reliability: a fragmented transfer is all or nothing. One block lost beyond its retries (Z-Stack waits
`APSC_ACK_WAIT_DURATION_POLLED` for a sleepy destination) loses the whole ASDU. Missing blocks are resent
selectively, though, so on a good link it is at least as reliable as the same number of separate frames, and it
adds an end-to-end APS ack. Cross-vendor interoperability (TI to ZBOSS) is the weak spot: the feature is rarely
used outside Smart Energy, so it gets little testing between vendors.

Latency: about one round trip for the whole window instead of one per message. To a sleepy end device each block
is still an indirect message that the device must poll for.

Mesh impact: for a **sleepy end device** every block waits in the *parent's* indirect queue until the device
polls. The coordinator firmware has generous buffers (`MAC_CFG_TX_DATA_MAX 50`, held 8 s by
`NWK_INDIRECT_MSG_TIMEOUT 8`). A cheap third-party router as parent may hold only one or two frames per child,
so a window of 8 can overflow its queue. That drops frames for that router's other children too, and the
retries add load. If fragmentation is ever used, use window size 1 to 2 for a sleepy end device.

### 2.3 Why fragmentation is off the table for now

Using it would take three things we do not have: (1) a herdsman change to request APS acks and to send
through `AF_DATA_REQUEST_EXT`, (2) a Z-Stack build whose fragmentation works together with source routing, and
(3) proof that TI-to-ZBOSS fragmentation interoperates with a sleepy end device. The first is an upstream PR. The
second is in TI's closed stack. It contradicts the "no external converter / stock Z2M" rule of DESIGN.md 6.2,
and it would tie the device to one coordinator family (Ember coordinators cannot do it at all through herdsman).

## 3. Existing solutions for larger data

| Approach | How | Size / speed | Fit for us |
|---|---|---|---|
| **OTA Upgrade cluster (0x0019)** | Device *pulls* `Image Block Request`s at its own pace; server answers with blocks; resumable by offset | herdsman serves **50-byte blocks** by default (40 for some vendors); hundreds of kB, but minutes to an hour; Z2M OTA of many devices at once is a known source of instability | Robust and sleepy-friendly, but Z2M serves only firmware images from its OTA index, not data produced by HA. Abusing it would need a converter plus a file pipeline. No. |
| **Partition cluster (0x0016, ZCL)** | Application-level partitioning with acks, for frames larger than APS fragmentation allows | any size | Rarely implemented: not in ZBOSS, not in herdsman. No. |
| **Smart Energy Tunneling (0x0704)** | Byte stream over ZCL with APS fragmentation, negotiated transfer size (128-byte default) | about 128 bytes per transfer | Needs fragmentation and an SE stack; not in Z2M. No. |
| **Tuya 0xEF00 data points** | Vendor cluster, raw / string DPs | single frame (under about 70 bytes) | Same limit as ours. |
| **Commercial Zigbee e-paper** (for example the lobsnet "E-Paper Dashboard Creator", Z2M compatible) | Uploads a layout binary, then only values | not documented | Confirms the pattern: send the layout once, then only small values. |
| **OpenEPaperLink** (e-paper shelf tags on 802.15.4) | **Leaves Zigbee**: own protocol on raw 802.15.4, tag pulls 4 KB blocks made of 42 parts x 99 bytes, re-requests missing parts by bitmap, then sleeps until the next check-in | images of tens of kB in seconds, single hop to its own AP | This is the "push the limit" design, but it gives up the mesh, Z2M and HA integration. Out of scope. |

Common pattern among the solutions that move a lot of data: **the receiver pulls, in blocks that each fit one
frame, with selective retransmission**. Nobody relies on a single huge Zigbee frame.

## 4. Where the 3.5 s actually goes

Airtime is not the problem. 802.15.4 runs at 250 kbit/s, so a full 127-byte frame takes about 4 ms on air. The
whole snapshot is about 3 frames' worth of data, about 12 ms of airtime per hop.

Per write, the path is: Z2M/herdsman queue, then `AF_DATA_REQUEST`, then the coordinator (or parent router)
queues the frame as an indirect message. It **waits for the next poll** (up to `POLL_MS` = 250 ms, average
125 ms), then the device processes it. Then comes the ZCL Write Attributes Response back, and herdsman resolves
the request. Herdsman runs up to 16 requests concurrently on Z-Stack 3.x, but the device fetches one queued frame
per poll. 8 writes at about 440 ms each matches "one poll interval plus one response round trip per write".

Frames on air per write: about 6 (data, MAC ack, poll, MAC ack, write response, MAC ack), plus 2 more per router
hop. 8 writes is about 50 frames. That is small for the mesh, but each one is also a chance to fail. With an
illustrative 99 % success per write, 8 writes all succeed 92 % of the time and 4 writes 96 %. Our tag-and-commit
scheme turns a failure into a 5 s retry instead of a wrong screen. It costs time, never correctness.

## 5. Effect on the mesh as a whole

- **Polling faster while awake**: a poll is a 12-byte MAC command plus an ack, about 1 ms on air. Polling every
  50 ms for the 5 to 10 s the device is awake is about 2 % channel use from one device next to its parent. It is
  local to the parent's neighbourhood and does not touch routing. Negligible.
- **Fewer, fuller frames**: better for the mesh (fewer MAC/NWK headers, acks and polls per byte), but only up to
  the worst-case limit. A frame that only fits on a short path fails, triggers route repair broadcasts and
  resends, and costs far more than the bytes it saved. Hence the 40-byte rule (2.1).
- **Bursts to a sleepy device**: every write waits in the parent router's queue for sleepy children until the
  device polls. Writes published all at once pile up there. Pacing them (option 2) keeps the queue at one or
  two frames, whatever router ends up as the parent.
- **APS fragmentation**: bursty. A window of blocks queued at the parent stresses small router buffers (2.2). A
  failed transfer retransmits blocks, and Z-Stack's known source-route bug can drop it after it was already
  partly sent.
- **Broadcasts** must never be used for payloads. They are limited to 74 to 92 bytes, unfragmented, unacked, and
  repeated by every router (Z-Stack `MAX_BCAST 30`). Not relevant to us, but the main way devices hurt a mesh.
- **Route discovery**: herdsman retries a failed unicast with a route rediscovery (`discoverRoute`, a broadcast).
  More messages per snapshot means more chances to trigger one. Fewer messages helps here too.

## 6. Options, ranked

| # | Change | Latency (estimate, to verify) | Reliability | Mesh | Cost |
|---|---|---|---|---|---|
| 1 | **Poll faster while awake**: `POLL_MS` 250 to about 50 ms (only in the awake states; sleep is unaffected) | about 3.5 s to about 1.5 s | better: frames spend less time in the parent's queue, so fewer expire | negligible (5) | one constant; a few mA for a few seconds |
| 2 | **Pace the writes**: HA publishes one write, then waits about 2 x `POLL_MS` (100 ms at 50 ms polling) before the next, instead of all at once. Today all 8 go out together and herdsman sends up to 16 in parallel, so they pile up in the parent router's queue for sleepy children, whose size is unknown for cheap routers (7.1). | +0.1 s per write, mostly hidden by option 1 | better: at most 1 to 2 frames queued at the parent; no overflow that also drops the parent's other children's traffic | smoother, no burst | HA automation only |
| 3 | Fold the commit into the last line write (2 attributes, about 38 bytes, see 2.1) | one write less | same guarantees; fits the 40-byte limit only with the line length cap | one message less | small firmware + HA change; optional |
| 4 | **Cap the line length in HA** so a tagged line never exceeds the 40-byte rule (29 characters at most, the panel shows about 21 anyway) | none | removes the only way to exceed the limit | | HA template only |
| - | ~~Two lines per write~~ | | fails beyond about 2 relays (2.1) | | rejected |
| - | ~~APS fragmentation, one 200-byte write~~ | about 1 s at best | unproven on our path, interop risk | bursty at the parent | rejected (2.3) |
| - | ~~Leave Zigbee for bulk data (OpenEPaperLink style)~~ | fast | own protocol to maintain | separate channel | out of scope |

Options 1, 2 and 4 together should bring a full push to about 1.5 s **without** relying on a good location. Every
frame stays under the worst-case limit, and the parent never holds more than one or two frames for the device.
Option 3 saves one more message if wanted. None of them changes Z2M, the coordinator or the "no converter"
rule. The tag-and-commit scheme stays the safety net: a lost write costs a resend, never a wrong screen.

## 7. Bench tests to settle the numbers

1. **Single-frame ceiling on our path**: one string write to a slot, length stepped from 60 to 80 bytes.
   Once with the device next to the coordinator, once forced behind a router (power it from a router-only spot
   or block the coordinator). Expect the edge at ZCL payload 79 and 73 to 75. Note the error Z2M logs at the edge
   (`MEM_ERROR`, `INVALID_PARAMETER`, `MAC_*`).
2. **Poll interval vs. push time**: full snapshot with `POLL_MS` = 250, 100, 50, 20; log the time from first
   MQTT publish to `shown_hash` report.
3. **Paced vs. burst** (option 2): 20 full pushes each way at the chosen `POLL_MS`, count failed writes (Z2M
   logs them on `bridge/logging`) and resends. Run it with the device at a poor spot (behind the weakest router
   it can join), since that is the case the design must survive.
4. (Optional, curiosity) a 200-byte string write through Z2M, to see whether this Z-Stack build fragments at
   all. A sniffer (a second ESP32-C6 or nRF52840 with the network key in Wireshark) shows the APS extended
   header if it does.

### 7.1 Where the device sits in the mesh (network map, 2026-10-14, without routing tables)

- **Parent: `gf_study_plug_ac`**, a Sonoff S60ZBTPF plug (Telink TLSR8656), not the coordinator. Link
  device to plug: LQI 165 (good). The device reports tree depth 3, the plug depth 2.
- **The plug has no usable direct link to the coordinator** (LQI 1). Its neighbours are mostly weak: LQI 20 to 59
  to the other plugs, and one strong link, LQI 240, to `gf_living_plug_ac`.
- So a frame from the coordinator to the device is source-routed through **at least 2 relays** (some router,
  then the study plug), and it is likely 2 relays exactly: at most 73 bytes per write here. This is **not the
  final location**, and the design does not depend on it. The 40-byte rule (2.1) covers 8 relays.
- The weak links upstream of the plug mean MAC retries and the occasional route repair are normal on this
  path. That is the kind of location the design must tolerate: pace the writes and keep the tag-and-commit
  safety net (section 6).
- The parent is a cheap Telink-based plug with an unknown indirect queue size. That is the case section 2.2
  warns about for APS fragmentation windows, and one more reason not to send bursts to this device.
- Test 1 on the bench runs on this real path as long as the device stays in the lobby. A test next to the
  coordinator needs the device moved and re-joined.

### 7.2 Writes lost right after a restart (open, parked 2026-10-14)

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
  (DESIGN.md Q6); `VIEW_WAIT_MS` (45 s) keeps the device awake for HA's resend.
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
- **Next test**: device next to the coordinator, so that it rejoins with the coordinator as parent, then the
  same reset-each run. Clean: the Tuya routers' handling of a rejoined sleepy child is the cause. Still lossy:
  look at the device's stack after a reboot.

## Sources

- Digi, XBee 3 Zigbee maximum RF payload size: https://docs.digi.com/resources/documentation/digidocs/90001539/reference/r_zb_max_rf_payload_size.htm
- Digi, Zigbee fragmentation: https://docs.digi.com/resources/documentation/digidocs/90002002/content/concepts/c_zb_fragmentation.htm
- Nordic DevZone, "Transmit large data using Zigbee" (82-byte calculation, 8-fragment limit, Partition cluster not in ZBOSS): https://devzone.nordicsemi.com/f/nordic-q-a/103545/transmit-large-data-using-zigbee
- Nordic DevZone, "Maximum payload size for Zigbee cluster command": https://devzone.nordicsemi.com/f/nordic-q-a/126019/maximum-payload-size-for-zigbee-cluster-command/559070
- ZBOSS config reference (`APS_IN_FRAG_MAX_BUF_SIZE`): https://developer.nordicsemi.com/nRF_Connect_SDK/doc/zboss_api_doc/group___z_b___c_o_n_f_i_g.html
- ESP Zigbee SDK, APS API (fragment window size): https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c3/api-reference/aps/esp_zigbee_aps.html
- Silicon Labs EZSP messaging frames (`maximumPayloadLength`): https://docs.silabs.com/zigbee/8.2.1/sisdk-ezsp-reference-guide/08-messaging-frames
- TI E2E, Z-Stack fragmentation issue (source routing, 0x10 memory fail): https://e2e.ti.com/support/wireless-connectivity/zigbee-thread-group/zigbee-and-thread/f/zigbee-thread-forum/386634/z-stack-fragmentation-issue
- TI E2E, making source routing support fragmentation on CC2652P: https://e2e.ti.com/support/wireless-connectivity/zigbee-thread-group/zigbee-and-thread/f/zigbee-thread-forum/1459664/cc2652p-i-have-try-to-make-source-routing-sending-support-data-unit-fragmentation
- TI E2E, CC2674P10 large message and fragmentation with Z-Stack: https://e2e.ti.com/support/wireless-connectivity/zigbee-thread-group/zigbee-and-thread/f/zigbee-thread-forum/1421511/cc2674p10-reporting-issues-with-large-message-size-and-fragmentation-with-z-stack
- TI E2E, fragmentation on Z-Stack 2.5.0 (MEM_FAIL on large sends): https://e2e.ti.com/support/wireless-connectivity/zigbee-thread-group/zigbee-and-thread/f/zigbee-thread-forum/131559/zigbee-fragmentation-on-z-stack-2-5-0-using-znp-zap-model
- Z2M discussion #28395, "Does herdsman support APS fragmentation?": https://github.com/Koenkk/zigbee2mqtt/discussions/28395
- zigbee-herdsman source (master, read 2026-10-14): `src/adapter/z-stack/adapter/zStackAdapter.ts`, `src/adapter/ember/consts.ts`, `src/controller/helpers/ota.ts`: https://github.com/Koenkk/zigbee-herdsman
- Koenkk Z-Stack coordinator firmware build patch (buffer, routing, indirect timeout settings): https://github.com/Koenkk/Z-Stack-firmware/blob/master/coordinator/Z-Stack_3.x.0/firmware.patch
- OpenEPaperLink protocol (`proto.h`: 99-byte parts, 4 KB blocks, bitmap re-request): https://github.com/OpenEPaperLink/OpenEPaperLink/blob/master/ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/main/proto.h
- E-Paper Dashboard Creator (Zigbee e-paper, Z2M): https://community.home-assistant.io/t/e-paper-dashboard-creator/951562
- Silicon Labs, Partition cluster API: https://docs.silabs.com/d/zigbee-af-api/7.5.0/zcl-partition-cluster-transfer-partitioned-frame-command
