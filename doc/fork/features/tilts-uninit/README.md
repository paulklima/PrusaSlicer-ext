---
id: tilts-uninit
kind: fix
title: Fix uninitialized val1/val2 in update_tilts_by_mode (PrintConfig)
show: false
upstream_pr: none
---

# fix/tilts-uninit — uninitialized `val1/val2` in `update_tilts_by_mode`

## What / why

In the `coEnums` case of `update_tilts_by_mode()` (`src/libslic3r/PrintConfig.cpp`),
`val1/val2` are only assigned in known branches; the fallback is `assert(false)`, compiled
out in release builds, so an unexpected key would build `ConfigOptionEnumsGeneric` from
uninitialized ints. Both are now initialised to 0. Flagged by GCC 15
`-Wmaybe-uninitialized`.

## Files touched

`src/libslic3r/PrintConfig.cpp` (one line).

## How to test

Build with GCC 15: no `-Wmaybe-uninitialized` warning for `update_tilts_by_mode`. SLA printer
profiles with tilt settings (SL1/SL1S) still switch tilt modes as before.

## Upstream status

Not proposed. Upstream master (3.0 alphas) rewrote `update_tilts_by_mode()` in
`Slic3r/Biz/Config/Legacy/PrintConfig.cpp` without the `coEnums` case, so a PR against master
is moot; the fix only matters for the 2.9.x line. If a PR is wanted after all: `pr/tilts-uninit`
(see `doc/FORK-WORKFLOW.md`, appendix).

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.6 | keep | live bug on 2.9.6; drop at the 3.x bump (code gone upstream) |
