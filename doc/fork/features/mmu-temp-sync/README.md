---
id: mmu-temp-sync
kind: feature
title: MMU toolchange temperature sync (early M104 + M109 barrier)
show: true
upstream_pr: none
---

# feature/mmu-temp-sync — MMU hybrid toolchange temperature sync

## What / why

On MMU/SEMM toolchanges between filaments with different temperatures, upstream emits a
single non-waiting temperature command at the wrong place: the new filament is loaded and
purged into a nozzle that is still too cold (cold → hot: under-extrusion, clicking, jams) or
too hot (hot → cool: oozing, stringing).

Fix in the wipe tower (`WipeTower.cpp/.hpp` only):

- early non-blocking `M104` during unload,
- blocking `M109` barrier before the purge so it runs at the new temperature,
- terminal `M109` safety net before `TOOLCHANGE END`.

Direction-aware (cold→hot, hot→cool, same temp); gated on `m_semm`; no new config keys.

## How to test

Slice a multi-material model with two filaments at different temperatures (e.g. PLA 215 /
PETG 240) for an MK4S/MK3.9 + MMU3 profile and inspect the toolchange blocks in the G-code:
`M104 S<new>` early in unload, `M109 S<new>` before the purge, no wait when temps are equal.
A green build is not sufficient — check the G-code.

## Upstream status

Upstream-PR candidate. Not yet proposed. Re-check at every bump whether upstream changed
the wipe-tower temperature handling (`WipeTower::toolchange_*`).

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.5 – 2.9.6 | keep | upstream still emits a single non-waiting M104 |

## Notes

- `notes/mmu-01-temperature-wait.md` — root-cause research and solution survey.
- Related, not implemented: `doc/fork/backlog/mmu-0{3,4,5}-*.md`.
