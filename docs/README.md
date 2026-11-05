# Greenlight documentation

Each file answers one kind of question. The design files describe the system as it is now; the past is in
[history.md](history.md), what is still to do in [roadmap.md](roadmap.md).

| Question | File |
|---|---|
| What is it for, how do the parts fit together, what principles drive it? | [architecture.md](architecture.md) |
| What does the device do on a wake, in each state, on a low battery, on a failure? | [behaviour.md](behaviour.md) |
| What is a dataset, a view, a line, the staleness flag? | [data-model.md](data-model.md) |
| Which clusters and attributes, what is reported when, what HA writes? | [zigbee.md](zigbee.md) |
| How does HA build the lines and decide when to push? | [home-assistant.md](home-assistant.md) |
| What does each screen look like, how are fonts and icons made? | [display.md](display.md) |
| Which parts and pins, what are the panel's quirks? | [hardware.md](hardware.md) |
| Where is the code for X, what is stored where, which build switches exist? | [firmware.md](firmware.md) |
| Why is it like this, what was tried and dropped? | [history.md](history.md) |
| What is real vs simulated, what is open, what comes next? | [roadmap.md](roadmap.md) |

Research and bench logs (dated, kept as recorded):

- [research/zigbee-payload.md](research/zigbee-payload.md): how much fits in one Zigbee frame, and the options
  for more.
- [research/wake-delivery-bench.md](research/wake-delivery-bench.md): frame losses right after a wake, measured.

Build and coding conventions are in [CLAUDE.md](../CLAUDE.md).

## How-to

Procedures are Claude Code skills in [.claude/skills/](../.claude/skills/), plain markdown that people can follow
as well:

| Skill | For |
|---|---|
| [bench-test](../.claude/skills/bench-test/SKILL.md) | serial console, simulated battery, Zigbee write tests, factory reset |
| [deploy-ha](../.claude/skills/deploy-ha/SKILL.md) | deploying `ha/` and `z2m/` changes to Home Assistant and Zigbee2MQTT |
| [panel-troubleshoot](../.claude/skills/panel-troubleshoot/SKILL.md) | reading the `panel:` log, BUSY hangs |
| [regen-bitmaps](../.claude/skills/regen-bitmaps/SKILL.md) | regenerating fonts and icons, adding a line icon, checking frames against the mockups |
