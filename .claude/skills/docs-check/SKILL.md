---
name: docs-check
description: "Check greenlight's documentation for consistency: broken links and #anchors (doc to doc, and the docs/<file>.md#<heading> references in code comments and skills), docs that no longer match the code (source layout, NVS keys, build switches, attribute ids, line icons, function names, file paths), and docs that contradict each other or state one fact in two places. Use when asked to check, audit or review the docs, after renaming a heading or a file, after a change that touches several design files, or before a release."
---

# Documentation consistency check

Two passes: a script for what can be checked mechanically, then a read for what needs judgement. Report what is
found; fix only what the user approves (or what they asked to fix), following the docs rule in CLAUDE.md: each
fact in one place, replaced designs to docs/history.md when the reason matters.

**The code is the truth: fix the docs, never the code.** This check changes no file under `main/`, `tools/`,
`ha/` or `z2m/`, not even a comment; a stale code comment is listed in the report for the user to handle.

## 1. Mechanical: `tools/check_docs.py`

```
python tools/check_docs.py
```

No dependencies; exits 1 and prints `file:line: problem` lines if anything is wrong. It checks:

- **Links**: every relative markdown link in tracked `.md` files resolves, and every `#anchor` matches a heading
  (GitHub's slug rules).
- **Doc references**: every `docs/<file>.md#<heading>` in code comments, skills and docs (also the
  `, #other-heading` list form after a reference) points to an existing heading.
- **Code names in the design docs** (not docs/history.md or docs/research/, which describe the past): file paths,
  `name()` functions and `UPPER_CASE` names in backticks exist in the repo / code.
- **Tables that mirror the code**: docs/firmware.md#source-layout lists every `main/` file;
  docs/firmware.md#persistent-state matches the `STORE_KEY_*` strings; docs/firmware.md#build-switches matches
  `main/CMakeLists.txt`; every custom attribute id in `zigbee_internal.h` is in docs/zigbee.md, and the custom
  cluster id is the code's; docs/data-model.md#line-icons matches `LINE_ICONS` in `tools/gen_bitmaps.py`.
- **Indexes**: every skill is listed in docs/README.md and CLAUDE.md; every doc is listed in docs/README.md.

A false positive (a name that lives outside the repo, such as a ZCL status or a path on the HA host) goes into
`EXTERNAL_NAMES` / `EXTERNAL_PREFIXES` at the top of the script, not around the check. When a new table in the
docs mirrors a code table, add a check for it.

## 2. Judgement: read the docs against each other and the code

Read docs/README.md first, then each design file. Look for:

1. **Contradictions between design files**: numbers and rules stated in more than one place (timeouts, intervals,
   byte limits, the 40-byte frame rule, attribute types, state names and transitions, staleness rules, battery
   thresholds). Compare docs/behaviour.md with `main/state_machine.c`, docs/zigbee.md with `main/zigbee/` and
   `z2m/greenlight.mjs`, docs/home-assistant.md and docs/data-model.md with `ha/`, docs/display.md and
   docs/uispec/ui-spec.md with `main/screen/`.
2. **Doc vs code**: behaviour a doc describes that the code no longer does (or the reverse), and constants whose
   value differs. The code wins: the doc is changed.
3. **One fact, one place**: a fact stated in full in two files; the file that owns it (docs/README.md's table)
   keeps it, the other links to it.
4. **Wrong owner**: a fact in a file whose question (docs/README.md) it does not answer.
5. **Stale present tense**: a design file describing what docs/history.md says was replaced, or docs/roadmap.md
   listing as open what is already done (or as done what the code lacks).
6. **Skills**: commands, paths, ports and log lines in `.claude/skills/*/SKILL.md` still match the code and
   `tools/` (log lines that `tools/` matches must exist in `main/` as written).
7. **Comments in code** that restate a doc fact with a different value.
8. **Hungarian** (the docs rule in CLAUDE.md): Hungarian in prose that does not follow its English in parentheses,
   or a screen not called by its name in docs/uispec/ui-spec.md#device-screens, in italics. docs/uispec/ui-spec.md,
   code blocks and images are exempt.

docs/history.md and docs/research/ are records: do not "fix" their facts, only their links.

## Report

Group by file, most serious first (contradictions and doc-vs-code errors, then duplicates, then wording). For
each: where (`file:line`), what each side says, which side is right and the proposed fix. End with the script's
result after any fixes (`docs check: OK`).
