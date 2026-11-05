# Home Assistant and Zigbee2MQTT

What HA computes and when it pushes. The sources of what runs in HA are in this repo, under the same paths as
in HA's config:

- `ha/custom_templates/greenlight.jinja`: the dataset rules and hashes, as macros;
- `ha/packages/greenlight.yaml`: `sensor.greenlight_dataset`, `sensor.greenlight_view` and
  `automation.greenlight_push`;
- `z2m/greenlight.mjs`: the Z2M external converter ([zigbee.md](zigbee.md#external-converter)).

Keep the repo copies and HA in sync; deploying is the `deploy-ha` skill. In Z2M, availability is disabled for
the device (for battery devices it only trips after hours, so it says nothing useful).

## Dataset

Built automatically with the rules of the "Bezárva?" dashboard (`dashboard-away`): one line per watched entity
**whatever its state**, so the dataset only changes when entities are added, removed or renamed. Nothing is
listed by hand; the label `exclude_from_alerts` keeps an entity out.

| Prio | Match | Icon | State word | Active when |
|---|---|---|---|---|
| 1 | `alarm_control_panel.*` (two lines) | 09 | `kikapcsolva` / `ismeretlen` | `disarmed` / neither `disarmed` nor `armed_*` |
| 2 | binary_sensor `moisture` | 27 | `nedves` | `on` |
| 3 | binary_sensor `problem` | 00 | `hiba` | `on` |
| 4-6 | binary_sensor `lock`, `door`, `window` | 08, 02, 04 | `nyitva` | `on` |
| 7 | `light.*` except groups (`entity_id` attribute) | 0D | `be` | `on` |
| 8 | `remote.*` | 13 | `be` | not `off`, `unavailable` or `unknown` |

- Each line is `II|<entity name>|<state word>` ([line format](data-model.md#line-format)); the name is cut so
  the line fits 64 UTF-8 bytes.
- Order: priority, then `entity_id`. Sorted rather than "append new entities at the end": an added entity
  changes the hash and resyncs anyway, and priority order is the better display order.
- More than 32 entities: the first 31 stay and line 31 is the fixed catch-all `01|…és még több|`, active when
  any of the remaining entities is; its text never changes, so trimming causes no resyncs.
- Like the dashboard, an `unavailable` sensor is not active.
- `ds_hash` (the state of `sensor.greenlight_dataset`) = 24-bit hash of the lines joined by newlines
  (`greenlight_hash24`, never 0).

## View and expected conditions

`sensor.greenlight_view`: its state is `view_id` = 24-bit hash of `"<ds_hash>:<bits>"` (never 0); its
attributes are `raw_bits` (which lines have a member in its problem state), `hidden`, `bits` and `ds`. The
`time` is added when the write is sent (now as local seconds since 1970).

**Expected conditions** are filtered out in an explicit step. The display hangs in the lobby next to the
alarm's arm panel, where the lobby door is open, unlocked and the alarm disarmed by definition. The small
living-room lamp is the night light: moving around the ground floor at dusk turns it on and arming turns it
off, so it is no problem for whoever is leaving. Those members (`greenlight_expected_members`:
`alarm_control_panel.house_alarm` `disarmed`, `binary_sensor.gf_lobby_door_lock_contact`,
`binary_sensor.gf_lobby_door_contact` and `light.gf_living_light_sw` `on`) keep their dataset lines, but
`greenlight_hidden_bits` masks out every line whose members are all expected: `bits` = `raw_bits` without
`hidden`, and `view_id` is computed from the filtered `bits`, so those entities changing push nothing. The
alarm's `ismeretlen` line stays a problem.

## Push automation

`automation.greenlight_push`, `mode: queued`, `max: 2`: a run can take a minute, and the waiting run reads the
newest values when it sends, so it covers any number of changes meanwhile. Triggers:

- **Wake** (`wake`): the device's wake counter changed. (1) If the cached `dataset_hash` != `ds_hash`, run the
  dataset sync (every `ds_entry`, 100 ms apart, then `ds_commit`; [zigbee.md](zigbee.md#dataset-sync)) and wait
  for `dataset_hash` to match, up to 20 s. (2) Send the view write (one message, all four attributes),
  **always**, even when `shown_view_id` already equals `view_id`: it makes the screen fresh.
- **Change** (`change`): `view_id` changed (which also changes with `ds_hash`), held for 1 s so a burst of
  entity changes gives one write. Sent only while the device is awake (below), after a dataset sync if
  `ds_hash` changed too.
- **Tick** (`tick`, every 5 s), with the same awake gate: send the view write. It keeps the display's
  [staleness flag](data-model.md#staleness-flag) off, so it is HA's liveness signal, and it repairs a lost
  change push within 5 s.

**The awake gate**: `awake` is on and was last reported less than 90 s ago (the heartbeat is every 30 s; this
covers a device that died without saying goodbye), **or** the wake counter changed after `awake` last did, less
than 90 s ago. The second part covers the few seconds between the wake counter and the `awake` = true that
follows it ([zigbee.md](zigbee.md#when-the-device-reports)); without it a change in that gap was dropped. If
`awake` = false was lost, HA keeps pushing (and Z2M logs failed sends) for up to 90 s.

**Acknowledgement and resend**: after each write, wait for `shown_view_id` == `view_id` (or `dataset_hash` ==
`ds_hash`) for up to 20 s, since a write right after a wake can take a Z2M recovery round of about 13 s. If it
is still missing, resend, up to 3 attempts. Never resend while Z2M may still retry the previous write. A resend
after a wake still finds the device awake: it waits up to `VIEW_WAIT_MS` (45 s) for its view
([behaviour.md](behaviour.md#timeouts)). Needed because Z2M sometimes drops a write for good (`NWK_TABLE_FULL`,
[roadmap.md](roadmap.md#open-questions)).

## MQTT and entity names

The device is `gf_lobby_greenlight`; HA publishes to `zigbee2mqtt/gf_lobby_greenlight/set`. The MQTT state keys
are the converter's names: `awake`, `shown_view_id`, `wake_count`, `dataset_hash`, plus `occupancy`, `battery`,
`battery_low` and `voltage`. In HA they are `sensor.gf_lobby_greenlight_wake_count`,
`binary_sensor.gf_lobby_greenlight_awake`, `sensor.gf_lobby_greenlight_dataset_hash`,
`sensor.gf_lobby_greenlight_shown_view_id`, and so on.
