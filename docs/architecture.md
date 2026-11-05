# Architecture

## Goal

A battery-powered Zigbee device with a 4.2" 400x300 e-paper panel that shows Home Assistant's list of
"problems" (doors and windows left open, lights left on, ...). Each line is an icon, a name and a state,
e.g. `Teraszajtó | nyitva`. An empty list shows a large "Minden OK" check instead. A badge in the corner shows
when the screen is no longer confirmed up to date.

The device sleeps (deep sleep) whenever nobody is around and is alive and live-updating while someone is
looking at it. The PIR sensor and the pair button are the only things that wake it, and either one keeps it
awake. Pairing comes first: an unpaired device does nothing useful until it is paired.

## System overview

```
  HA (dataset: all possible lines,          Zigbee2MQTT (+ external converter)         device
      view: which are active now)
  ─────────────────────────────────        ─────────────────────────────              ──────
                                                                                      PIR on -> wake
  automation <──── wake counter   <──────── custom cluster report <────────────────── rejoin, report wake
  dataset_hash (cached) != HA's?  ───────►  /set: ds_entry x N, ds_commit  ─────────► store dataset (rare)
  every wake, any change, 5 s     ───────►  /set: one view write (1 frame) ─────────► redraw lines, fresh 8 s,
                 shown_view_id    <──────── custom cluster report <────────────────── report shown_view_id
                                                                                      PIR off + timeout
                 awake=false      <──────────────────────────────────────────────────  report, deep sleep
```

The pieces: the firmware ([firmware.md](firmware.md)), the Zigbee interface ([zigbee.md](zigbee.md)), the
Zigbee2MQTT converter in `z2m/` and the Home Assistant templates and automation in `ha/`
([home-assistant.md](home-assistant.md)).

## Principles

- **Push from HA, state from the device.** The device never asks for data. It exposes `awake`,
  `shown_view_id` and `dataset_hash`; HA pushes on every wake and whenever its view changes while awake.
- **One frame per wake.** Every Zigbee frame right after a wake can stall the exchange for about 13 s
  ([wake delivery bench](research/wake-delivery-bench.md)), so the per-wake exchange is one report up and one
  write down. Everything that needs more frames (the dataset) is rare and checked by a hash.
- **Full views, not deltas.** Every view push is the complete bitfield, so a lost, repeated or late frame
  never leaves lasting damage, and a device that slept through any number of changes catches up in one push.
- **Robust over payload.** No frame relies on a good position in the mesh: every frame fits the worst-case
  path (40 bytes of ZCL payload, [zigbee.md](zigbee.md#frame-size-and-polling)), and writes to the sleepy
  device are paced, never burst.
- **The panel is the memory.** The e-paper keeps its image without power.
- **The device is dumb.** HA writes the final text (in the dataset) and decides which lines are active and
  in which order they appear; the device only lays them out.
