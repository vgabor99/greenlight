# Data model

What the display shows, in two parts that are sent separately:

- the **dataset**: the fixed list of every line the screen can show. The lines stand for physical things in the
  house (doors, windows, lights), so it changes rarely (a device added or renamed in HA) and is synced only
  when its hash differs;
- the **view**: which of those lines are active right now. It changes often and is sent on every wake, in one
  Zigbee frame.

How they travel over Zigbee is in [zigbee.md](zigbee.md#input-ha-to-device); how HA builds them is in
[home-assistant.md](home-assistant.md).

## Dataset

```json
{ "hash": 4821377,
  "lines": [ "02|Teraszajtó|nyitva", "04|Háló ablak|nyitva", "0D|Nappali lámpa|be", "27|Mosogató|nedves" ] }
```

- `lines`: **at most 32** entries (`ZIGBEE_DATASET_MAX_LINES`); the position is the line's **index** (0-31),
  which is its bit in the view. The order is the display order, so HA puts the most important lines first.
  Text is **UTF-8**, **at most 64 bytes** (`ZIGBEE_LINE_MAX_BYTES`): HA cuts it at a character boundary. A
  Hungarian accented letter takes 2 bytes, so a line holds 32 to 64 characters. On the wire a line longer than
  26 bytes comes in up to 3 parts. The device replaces invalid UTF-8 and control characters with `?`.
- `hash`: **24-bit**, chosen by HA, identifying the ordered list. **0 is reserved** for "no dataset" and never
  used. The device stores and echoes it; it never computes it. A changed text, a changed order or a new line
  gives a new hash, and HA resyncs.
- The device keeps the accepted dataset in NVS (about 2.1 KB) and exposes its hash as `dataset_hash`.

### Line format

`II|name|state`. `II` is the line icon's id, 2 hex digits ([Line icons](#line-icons)); `name` is drawn in
bold, `state` (one or two words, may be empty) right-aligned ([display.md](display.md#house-screens)). HA cuts
the name so the line fits 64 bytes; the panel cuts what does not fit its row with "…". There is no other
format: a line that does not start with 2 hex digits and `|`, or has no second `|` or an empty name, is drawn
as "??? ???" with the fallback icon; an unknown icon id only gets the fallback icon.

### Line icons

Material Design Icons 7.4.47 (the set HA uses). **The table is append-only**: HA's templates use the ids, so an
entry is never moved or removed; new icons are added at the end (`LINE_ICONS` in `tools/gen_bitmaps.py`, then
regenerate, see the `regen-bitmaps` skill). Most are not used yet; they are in the firmware so new kinds of
lines need no reflash. Ids used by HA today: 00 (`problem`), 01 (catch-all), 02 (door), 04 (window), 08 (lock),
09 (alarm), 0D (light), 13 (remote), 27 (moisture).

| Id | Icon | Id | Icon | Id | Icon | Id | Icon |
|---|---|---|---|---|---|---|---|
| 00 | alert-circle (fallback) | 10 | led-strip-variant | 20 | washing-machine | 30 | thermometer |
| 01 | dots-horizontal (overflow) | 11 | power-plug | 21 | tumble-dryer | 31 | water-percent |
| 02 | door-open | 12 | television | 22 | dishwasher | 32 | weather-pouring |
| 03 | door-sliding-open | 13 | remote-tv | 23 | robot-vacuum | 33 | weather-windy |
| 04 | window-open | 14 | speaker | 24 | robot-mower | 34 | smoke-detector-variant |
| 05 | window-shutter-open | 15 | fan | 25 | ev-station | 35 | fire |
| 06 | garage-open-variant | 16 | air-conditioner | 26 | car | 36 | molecule-co |
| 07 | gate-open | 17 | air-humidifier | 27 | water | 37 | molecule-co2 |
| 08 | lock-open-variant | 18 | radiator | 28 | faucet | 38 | gas-burner |
| 09 | shield-off | 19 | heat-pump | 29 | valve | 39 | trash-can |
| 0A | shield-home | 1A | fireplace | 2A | water-pump | 3A | mailbox |
| 0B | motion-sensor | 1B | stove | 2B | water-boiler | 3B | battery-low |
| 0C | cctv | 1C | iron | 2C | sprinkler-variant | 3C | wifi-off |
| 0D | lightbulb-on | 1D | coffee-maker | 2D | pool | | |
| 0E | ceiling-light | 1E | kettle | 2E | fridge | | |
| 0F | lamp | 1F | printer | 2F | snowflake | | |

No alert markers on line icons (`fridge`, not `fridge-alert`): the device is an alert device already.

## View

```json
{ "ds": 4821377, "bits": 5, "time": 1793112720, "id": 9871234 }
```

- `bits`: bitfield over the dataset, bit *i* set = line *i* is active. 0 = "All OK".
- `ds`: the dataset hash the bits refer to. The device accepts the view only if `ds` equals its stored
  `dataset_hash`; otherwise it is **out of sync** and leaves the screen as it is.
- `id`: **24-bit**, chosen by HA, identifying `ds` + `bits`. **Not 0**. The device echoes it as
  `shown_view_id` when the view is on the panel; it never computes it. The time is **not** part of the id: a
  new time alone must not look like new content.
- `time`: local wall-clock time as seconds since 1970-01-01, i.e. HA's Unix time plus its UTC offset. The
  device stores it with the view but does not draw it (the [staleness flag](#staleness-flag) shows freshness
  instead). 0 = unknown.

The device stores the rendered rows of the shown view (at most 6 texts, the id and the time), not only the
bits. A new dataset therefore never changes the screen by itself: the old rows stay until a view for the new
dataset arrives (HA sends one right after the sync).

### Layout rule

The active lines in dataset order, at most 6 rows. With more than 6 active lines the device shows the first 5
and "további N" (dots icon) as the sixth row. Code: `main/screen/view_layout.c`.

## Staleness flag

The viewer must see at a glance whether the screen is current, without working out how old a time is. So an
inverted `sync` badge is drawn over the top-right corner while the view is **stale**, and nothing otherwise
([display.md](display.md#house-screens)).

- Every received view (new content, or the same `id` again) is valid for `VIEW_VALID_MS` (8 s): the device
  stores the deadline `valid_until_us` = now + 8 s (`main/display/view_freshness.c`). Stale means now > the
  deadline. An out-of-sync or incomplete view does not count.
- "Now" is the RTC counter (`esp_rtc_get_time_us()`): monotonic, it keeps counting through deep sleep and a
  software restart. The deadline lives in RTC memory (`RTC_NOINIT_ATTR`), which survives the same, so no flash
  writes. After a power-on or brownout reset (the counter starts at 0 again), or when the stored deadline is
  implausibly far ahead, it is cleared: stale.
- HA pushes the view on every wake, on every change while the device is awake, and **every 5 s while it is
  awake**. The periodic push is the liveness signal: if HA, Z2M or the radio path stops, the badge appears at
  most 8 s after the last view. A lost change push is repaired by the next periodic push.
- At boot the stored view is normally stale, so the boot screen has the badge and the wake push removes it.
- The flag is redrawn when it flips: the state loops call `display_wait_ms()` every 20-100 ms, which redraws
  when the flag (on the debug screen, the shown age) differs from what was drawn. No timer.
- SHUTDOWN draws the flag as it is at that moment; the sleep screen keeps it until the next wake, so a wake
  that ended without fresh data sleeps with the badge.
- A view with the same `id` as the shown one changes nothing on the screen apart from the flag, and is
  acknowledged with `shown_view_id` like any other.

## State exposed by the device

`awake`, `shown_view_id`, `dataset_hash` and `wake_count`, next to the battery and occupancy
([zigbee.md](zigbee.md#state-device-to-ha)). `shown_view_id` after a redraw is the acknowledgement of a view
push, `dataset_hash` after a store is the acknowledgement of a dataset sync.
