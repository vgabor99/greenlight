---
name: deploy-ha
description: Deploy greenlight's Home Assistant and Zigbee2MQTT side - ha/custom_templates/greenlight.jinja, ha/packages/greenlight.yaml and z2m/greenlight.mjs - to the live HA / Z2M, and check that the deployed copy matches the repo. Use after editing any file under ha/ or z2m/, or when HA's greenlight sensors or push automation misbehave.
---

# Deploying the HA and Z2M side

The files under `ha/` are the source of what runs in HA, under the same paths in HA's config
(`config/custom_templates/greenlight.jinja`, `config/packages/greenlight.yaml`). The repo copy and HA must stay
in sync: every change to one goes to the other in the same task. What the files do: docs/home-assistant.md.

Load the `home-assistant-profile` skill before the first Home Assistant MCP call, and the ha-mcp tool schemas
with ToolSearch.

**Agreed changes are deployed without asking again**: run ha-mcp's preview/confirm flow in one go, check the
preview diff holds only the intended change, confirm, reload, and report the diff afterwards. This covers
changes the user agreed to, not new ones.

## Macros: greenlight.jinja

1. `ha_write_file` the whole file to `custom_templates/greenlight.jinja`.
2. Call the service `homeassistant.reload_custom_templates`.
3. Check: `ha_eval_template` with a macro call, e.g.
   `{% from 'greenlight.jinja' import greenlight_dataset_items %}{{ (greenlight_dataset_items | as_function)() | map(attribute='text') | list }}`.

## Package: greenlight.yaml

1. `ha_config_set_yaml` for the package's top-level keys `template` and `automation`.
2. Reload template entities and automations (`ha_reload_core`, or the `template.reload` / `automation.reload`
   services).
3. Check: `ha_get_state` of `sensor.greenlight_dataset` and `sensor.greenlight_view` (state and attributes), and
   `ha_get_automation_traces` for `automation.greenlight_push` after the next wake or 5 s tick.

## Converter: z2m/greenlight.mjs

1. Z2M needs `advanced.enable_external_js: true` (restart after turning it on).
2. Copy the file to Z2M's `data/external_converters/greenlight.mjs`, or publish it to
   `zigbee2mqtt/bridge/request/converter/save`.
3. A copied file and its folder must belong to Z2M's user (`chown -R --reference=data data/external_converters`):
   Z2M copies the file to a temp file in that folder before loading it, and with root ownership that fails
   (`EACCES`, "Invalid external converter ... was ignored").
4. Restart Z2M. No re-pair: Z2M picks the definition by the device's fingerprint. A new custom attribute needs a
   line in the cluster and an expose in the converter.
5. Check: the device's Exposes tab in Z2M, or the MQTT state on `zigbee2mqtt/gf_lobby_greenlight` showing
   `wake_count`, `shown_view_id`, `dataset_hash`, `awake`, `voltage`.

## Verify the sync

`ha_read_file` each deployed file and compare it with the repo copy; report any difference instead of
overwriting silently if HA holds a change the repo doesn't.
