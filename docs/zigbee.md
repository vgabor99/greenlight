# Zigbee interface

The device's Zigbee interface: identity, clusters, the state it reports and the writes it accepts. The firmware
side is `main/zigbee/` (`zigbee_device.c` declares the clusters, `zigbee_reports.c` sends the reports,
`zigbee_display_input.c` handles the writes); the Z2M side is `z2m/greenlight.mjs`. What the values mean is in
[data-model.md](data-model.md).

The device is a **sleepy end device**: rx-on-when-idle off, it polls its parent every `PARENT_POLL_MS` (50 ms)
while awake and not at all while asleep.

## Identity and freeze rules

Z2M keeps what the interview found in `data/database.db` (endpoints, cluster lists, a few Basic attributes),
and all of that must be final, so the current pairing is the last one. Everything else can change without
re-pairing: the converter, the exposed names and units, and custom attributes added later.

1. **Identity never changes:** `manufacturerName` = `Greenlight`, `modelId` = `greenlight-eink42` (Basic
   strings are at most 32 characters). A new hardware variant gets a new model id, never a changed one.
2. **One endpoint (1), profile Home Automation, device id Simple Sensor (0x000C)**, with the cluster list
   below. The list is final, including the OTA client, which is declared but not used yet.
3. **No reporting configuration from Z2M.** The device sends all reports on its own, to the coordinator
   (0x0000), so nothing depends on Z2M's bind / Configure Reporting step succeeding on a sleeping device. The
   converter has no configure step and no readable exposes.
4. **Attribute ids are append-only.** Never reuse or renumber one. Retired: 0x0000-0x0007 (the rev 5 snapshot,
   [history.md](history.md)); in use: 0x0013 (`battery_mv`); reserved for future work: 0x0010-0x0012 and 0x0014
   ([roadmap.md](roadmap.md#future-work)).
5. Z2M availability is disabled for the device. `last_seen` (updated by every report) is the liveness signal
   and doubles as "last wake".

## Endpoint and clusters

| Cluster | Direction | Role |
|---|---|---|
| Basic 0x0000 | server | identity, power source, firmware version |
| Power Configuration 0x0001 | server | battery ([Battery](#battery)) |
| Occupancy Sensing 0x0406 | server | the PIR as an HA presence sensor |
| `greenlight` custom 0xFC8A | server | the display input ([Input](#input-ha-to-device), writable by the coordinator) and the device state ([State](#state-device-to-ha), reported) |
| OTA Upgrade 0x0019 | client | reserved for a later firmware update, so the cluster list never changes. Not implemented yet. |

No other endpoints. 0xFC8A was picked arbitrarily and is used **without** a manufacturer code in the ZCL frames
(the id is in the manufacturer-specific range 0xFC00-0xFFFF); the SDK, Z2M and the raw write accept that.

## State: device to HA

All reported by the device itself.

| Cluster / attribute | Type | Z2M name | Meaning |
|---|---|---|---|
| Power Configuration `batteryPercentageRemaining` 0x0021 | uint8, **0-200 = 0-100 %** | `battery` | battery charge ([Battery](#battery)) |
| Power Configuration `batteryVoltage` 0x0020 | uint8, units of 100 mV | (not exposed) | set, not reported (the stack refuses it); a read gets it |
| Power Configuration `batteryAlarmState` 0x003E | bitmap32 | `battery_low` | bit 0 = low |
| Occupancy Sensing `occupancy` 0x0000 | bitmap8, bit 0 | `occupancy` | the PIR is active; `occupancySensorType` 0x0001 = 0 (PIR) |
| 0xFC8A `battery_mv` 0x0013 | uint16 | `voltage` | the exact battery voltage in mV |
| 0xFC8A `wake_count` 0x0040 | uint32 (0x23) | `wake_count` | the wake counter, +1 on every boot (every wake is a boot), 24 bits, kept in NVS. **HA's push trigger.** |
| 0xFC8A `shown_view_id` 0x0041 | uint32 (0x23) | `shown_view_id` | `id` of the view on the panel (0 = none) |
| 0xFC8A `dataset_hash` 0x0042 | uint32 (0x23) | `dataset_hash` | hash of the stored dataset (0 = none) |
| 0xFC8A `awake` 0x0043 | boolean (0x10) | `awake` | joined and listening, can receive pushes |

The 0xFC8A state attributes are read-only for the network (readable and reportable). **Ids and hashes are 24
bits** (0 to 16777215). HA chooses them; the device stores and echoes them and never computes them.

### When the device reports

There is nothing periodic while asleep.

- **On each wake**, right after the (re)join, **only the wake counter**: it is the one uplink frame the push
  depends on (every frame after a wake risks a 13 s stall). HA decides from the values Z2M has cached:
  `dataset_hash` and `shown_view_id` only change when the device reports them, and the device does not change
  them while asleep, so the cached values are current.
- `WAKE_REPORTS_DELAY_MS` (2 s) after the wake counter, when the view push has normally been handled, the rest
  follows: `dataset_hash`, `shown_view_id`, `awake` = true, Power Configuration, Occupancy.
- In INTERVIEW (after a pairing, when Z2M has no cached values) all of them are reported at once, the wake
  counter last.
- **While awake**: `awake` + `shown_view_id` every 30 s (heartbeat); `shown_view_id` after each accepted
  view; `dataset_hash` after each accepted dataset and after an out-of-sync view; `occupancy` when it changes
  (on when the PIR is active, off `OCCUPANCY_HOLD_MS` after it was last active); the battery when it changes
  over the console.
- **At SHUTDOWN**, if joined (also after a low battery, after its battery report): `occupancy` = 0 and
  `awake` = false, the only reports that ask for a default response; the unconfirmed one is resent every
  second, for up to `GOODBYE_TIMEOUT_MS` (`zigbee_report_goodbye()`).

**Why a counter triggers the push, not `awake`.** `awake = false` is sent just before the radio goes off and
can be lost. Then Z2M keeps `awake = true` cached, the next wake's `true` is no change in HA, and a trigger on
`awake` misses that wake. Any message from the device is no good trigger either: Z2M already publishes the
cached state when the device announces itself after the reboot, about 0.5 s before the device is ready. The
counter changes on every wake, so its change means "awake and ready". `dataset_hash` and `shown_view_id` are
usually the same as at the last wake, so they cannot serve either. `awake` stays as the gate for pushes while
the device stays awake.

## Input: HA to device

Writes to the custom cluster 0xFC8A:

| Id | Name | Type | Content |
|---|---|---|---|
| 0x0020 | `view_bits` | uint32 (0x23) | view `bits`: bit *i* = dataset line *i* active |
| 0x0021 | `view_ds` | uint32 (0x23) | view `ds`: the dataset hash the bits refer to (24 bits) |
| 0x0022 | `view_time` | uint32 (0x23) | view `time`: local wall-clock seconds since 1970 (0 = unknown) |
| 0x0023 | `view_id` | uint32 (0x23) | view `id` (24 bits, not 0) |
| 0x0030 | `ds_entry` | string (0x42) | `HHHHHH\|II\|text`: the new dataset hash as 6 hex digits, `II` as 2 hex digits (bits 0-4 the line index 0-31, bits 5-6 the part 0-2, bit 7 "more parts follow"), the UTF-8 text of this part (0-26 bytes) |
| 0x0031 | `ds_commit` | uint32 (0x23) | `count * 16777216 + hash`: number of entries (0-32) in the top byte, the new dataset hash below |

HA sends them through Z2M's documented raw `write` message on `zigbee2mqtt/<device>/set`. Numeric attribute ids
work with the cluster known to the converter: zigbee-herdsman looks a string key up by name, and a numeric key
falls back to a raw write with the given type. The converter also declares the input attributes as writable,
so a write by name works as well (`{"viewBits": 5}`, no type).

### View push

**All four `view_*` attributes in one write, one frame** (28 bytes of ZCL payload, 4 records of 7 bytes). HA
sends it on every wake, whenever its view changes while the device is awake, and every 5 s while awake. The
device collects the four values and applies them once the whole frame has been processed (`apply_view()`,
deferred to the Zigbee task's next turn, after the frame), so the order of the records does not matter:

- `view_ds` != stored `dataset_hash`: **out of sync**. Nothing changes on the screen; `shown_view_id` is not
  reported. The device reports `dataset_hash` (so HA's cached value is right) and counts it as activity, so it
  stays awake for the resync.
- `view_id` == `shown_view_id`: nothing new to draw, but the view is fresh again. Store the time, report
  `shown_view_id` (the acknowledgement).
- otherwise: build the rows from the dataset and the bits ([layout rule](data-model.md#layout-rule)), store
  them with the id and the time, redraw, report `shown_view_id`.

### Dataset sync

**Rare: one `ds_entry` part per write, then `ds_commit`.** HA writes the entries of lines 0 to count-1 (any
order, 100 ms apart), then the commit.

- Each entry write is at most 40 bytes of payload (a 36-character string plus the 4-byte record header), so it
  carries at most 26 bytes of text. A line up to 26 bytes is one entry (part 0, no "more"). A longer line is
  split at character boundaries into 2 or 3 parts of at most 26 bytes; every part but the last has the "more"
  bit.
- Each write takes about 0.45 s to reach the sleepy device (one poll plus the response), with occasional stalls
  of up to about 15 s: a sync of 16 three-part and 16 one-part lines (64 writes) took 31 to 78 s on the bench;
  the worst case of 32 lines x 3 parts = 96 writes about 45 s or more.
- The device stages each part in RAM by index and part, together with its tag, and counts every write as
  activity, so it stays awake while a sync runs.
- On `ds_commit` it accepts only if every line 0 to count-1 is complete with that hash: a last part L without
  "more", and parts 0 to L all staged with that tag. A part from an older sync has another tag, so it is never
  mixed in. Then it joins the parts, stores the dataset in NVS (one blob), clears the staging and reports
  `dataset_hash`. Otherwise it rejects the commit and logs the first missing index; HA resends the whole
  dataset after its timeout.
- A staged sync that does not finish before the device sleeps is lost, and the next wake starts over.

### Why it looks like this

- **Plain attribute writes**: the SDK delivers them to the application as "attribute set" events; no custom
  command handler is needed, and Z2M's raw write is a stock feature.
- **One frame per write, at most 40 bytes of payload** ([Frame size](#frame-size-and-polling)). A Z2M `write`
  with several attributes is one ZCL frame with several records; the view write is well under the limit.
- **A single frame needs no tag.** A frame arrives whole or not at all (the NWK MIC protects it), so the device
  can never combine an old bitfield with a new id. The tag-and-commit scheme is only for the dataset, which
  needs several frames.
- **`view_ds` makes the view self-checking.** A view always says which dataset it refers to, so a device with an
  old or missing dataset knows and does not draw wrong text.
- **String attributes are registered with a full-length initial value.** The stack takes the storage size of a
  string attribute from the value it is registered with. With a zero-length initial string, every second write
  of a string longer than 11 characters was answered `UNSUPPORTED_ATTRIBUTE`. `ds_entry` is registered with a
  36-character placeholder, which is not a valid tag.
- **uint32 for the numbers**: the type proven through Z2M's raw write.
- Z2M reports a failed write on `zigbee2mqtt/bridge/logging` (level error), so a refusal is visible.

### Example

HA publishes these to `zigbee2mqtt/<device>/set`; ids in decimal, the format of the Z2M raw write; 64650 is
0xFC8A, the dataset hash 4821377 is 0x499181:

```json
{"write": {"cluster": 64650, "payload": {"48": {"value": "499181|00|02|Teraszajtó|nyitva", "type": 66}}}}
{"write": {"cluster": 64650, "payload": {"48": {"value": "499181|01|04|Háló ablak|nyitva", "type": 66}}}}
{"write": {"cluster": 64650, "payload": {"49": {"value": 71930241, "type": 35}}}}
{"write": {"cluster": 64650, "payload": {"32": {"value": 5, "type": 35}, "33": {"value": 4821377, "type": 35},
                                          "34": {"value": 1793112720, "type": 35}, "35": {"value": 9871234, "type": 35}}}}
```

The dataset commit is 4 * 16777216 + 0x499181; all four entries 00 to 03 are written, two are shown here. The
view shows lines 0 and 2.

## Battery

Once per wake the device reports `batteryPercentageRemaining` (half percents, from a rough LiPo curve: 3.3 V
0 %, 3.5 V 5 %, 3.6 V 12 %, 3.7 V 30 %, 3.8 V 50 %, 4.0 V 80 %, 4.2 V 100 %, interpolated;
`zigbee_battery_half_percent()`), `batteryAlarmState` (bit 0 = low) and the exact voltage as 0xFC8A
`battery_mv`. The same reports go out in the farewell before a low-battery shutdown. The static Power
Configuration attributes are set from the start: `batterySize` 0x0031 = 2 (Other), `batteryQuantity` 0x0033 =
1, `batteryRatedVoltage` 0x0034 = 37 (3.7 V), `batteryAHrRating` 0x0032 = 100 (units of 10 mAh).

- Percent pitfall: the ZCL percentage is in half percents and Z2M divides by 2, so 100 % is the raw value 200.
- Z2M names follow zigbee-herdsman-converters' `battery()` helper: `battery` (%), `voltage` (mV) and
  `battery_low` (binary, true = low), all diagnostic entities; there is no `battery_ok`. HA's battery binary
  sensor also reads on = low.
- `voltage` comes from `battery_mv` because the standard attribute has only 100 mV resolution (too coarse for a
  LiPo: 3.0-4.2 V is 12 steps): `battery()` with `voltage: false`, plus a numeric expose in the converter.

The voltage is measured once per boot ([firmware.md](firmware.md#simulated-parts)); the behaviour on a low
battery is in [behaviour.md](behaviour.md#low-battery).

## External converter

Z2M needs a *definition* for each device model: how to recognise it, which entities it exposes, how to turn
Zigbee messages into state and MQTT commands into Zigbee frames. For unknown devices it builds a generic one
from the *standard* clusters, but that cannot publish attributes of a **custom cluster** (and broke occupancy on
this device, [history.md](history.md)). So the repo has its own: `z2m/greenlight.mjs`, written against Z2M
2.12.1, zigbee-herdsman-converters 26.76 and zigbee-herdsman 10.6. It:

- declares 0xFC8A as the custom cluster `greenlight` (`deviceAddCustomCluster`) with every attribute (inputs
  writable, state read-only);
- uses `battery()` and `occupancy()` for Power Configuration and Occupancy Sensing, with reporting off;
- exposes `wake_count`, `shown_view_id`, `dataset_hash` (`numeric`) and `awake` (`binary`), plus `voltage`, all
  STATE only and diagnostic;
- has no configure step and no reads to a sleeping device (freeze rule 3).

Without it, Z2M's generated definition shows none of the custom state. A new attribute needs a line in the
custom cluster and an expose here, then a Z2M restart; no re-pair. A friendlier `toZigbee` (one `view` message
for the four writes, one `dataset` message for the whole sync) can be added later without a firmware change or
a re-pair. Installing it: the `deploy-ha` skill.

## Frame size and polling

- **40 bytes of ZCL payload per frame, at most.** A single 802.15.4 frame carries about 79 bytes of ZCL
  payload next to the coordinator, less with every relay of a source route; the worst case we design for (8
  relays, both IEEE addresses in the NWK header, a manufacturer code) leaves 43. The device's spot in the mesh
  is not assumed to be good, so every message type fits 40 bytes: view write 28, `ds_entry` 40 at most,
  `ds_commit` 7. APS fragmentation is not usable through Z2M + Z-Stack. Details:
  [research/zigbee-payload.md](research/zigbee-payload.md).
- **Poll every 50 ms while awake** (`PARENT_POLL_MS`): a write waits in the parent's queue until the next poll,
  so the poll interval sets the per-write latency (about 0.45 s per write including the response). The cost to
  the mesh is negligible.
- **Pace writes, never burst**: the parent router holds frames for sleepy children in a small queue (unknown
  size on cheap routers). HA sends dataset entries 100 ms apart.
- After a wake, frames in both directions are lost at about 5 to 7 % each, and each loss costs a 13 s Z2M
  recovery. Hence one frame per wake ([wake delivery bench](research/wake-delivery-bench.md)).

## Recovering from a stale Z2M cache

Cheapest first: restart Z2M (it reloads definitions); correct the Z2M side and republish; remove the device in
Z2M and pair again (hold the pair button 3 s: the device erases its network and pairs,
[behaviour.md](behaviour.md#pairing)); as a last resort delete the device entry from `data/database.db` with
Z2M stopped. The Z2M frontend also has a re-interview action, which needs the device awake, hence the 60 s
`INTERVIEW_MS`.
