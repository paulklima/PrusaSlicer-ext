---
id: smart-auto-supports
kind: feature
title: Support planner (layer analysis for paint-on supports: islands, overhangs, bridges)
show: true
upstream_pr: none
---

# feature/smart-auto-supports — support planner (layer analysis for paint-on supports)

## What / why

The existing "Automatic painting" in the paint-on supports gizmo (`SupportSpotsGenerator`)
works on extrusions with a hard-coded 16 mm bridge limit that shrinks with curvature. It
paints areas that print fine — e.g. dome ceilings that close over a few mm — and ignores
that current printers bridge several cm.

Goal: a second, geometric auto mode that only paints where support is really needed:
islands / local minima (parts starting in mid-air), bridges longer than a configurable span,
and cantilevers whose accumulated reach exceeds a limit (dome and ring ceilings exempt) —
detected by comparing the layer slices.
Aimed at organic supports: placements prefer positions whose branches reach the build
plate, and each detector can be used alone or combined without changing the others.

**Status: v1 released in `v2.9.6+ext-v1.1.0` (2026-11-01).** Host-tested by the maintainer:
foot model (near-bed islands fixed, 2026-10-16), overhang test print on an MK4S (defaults
confirmed, 2026-10-17), renamed UI build (2026-10-30).
All work packages are integrated; unit and reference tests pass, also with asserts on plain
2.9.6; the analysis takes 1–3 s for 1000-layer models (`notes/timings.md`). Plan: `notes/plan.md`
(revision 4, reviewed in three rounds); specification: `notes/theory.md`. Work packages per
plan §8; their progress and deviations are in `notes/wp-log*.md`, the integration results and
the known limitations in `notes/wp-log-wp5.md`.

v1 scope (plan §0): a "Support planner (layer analysis)" section at the end of the paint-on
supports gizmo. It paints support enforcers for islands and overhangs (cantilevers) only where
the slicer's own tree contact would support them; bridges and hoop bands (domes, funnels,
counterbores) are exempt, long bridges are planned and reported but not painted. Planned support
is stored as TriangleSelector states 3–9 (the slicer treats them as enforcers), never overwrites
manual paint, and is baked to plain enforcers in 3MF for stock PrusaSlicer.

## Files touched

New module `src/libslic3r/Support/SmartSupports*` (analysis, pure libslic3r), GUI
`src/slic3r/GUI/Gizmos/{SmartSupportsPanel,TriangleSelectorFdmGui}.*`; changes in
`TriangleSelector`, `Model` (`FacetsAnnotation`), `PrintObject.cpp` (projection), `Print.cpp`
(warning), `Format/3mf.cpp`, `GLGizmoFdmSupports`, `GLGizmoPainterBase.cpp`; tests in
`tests/libslic3r` and `tests/fff_print`. Full list: plan §2.1.

## How to test

**Unit tests** (from the build directory; tag `[SmartSupports]`, one module tag each, e.g.
`[sas-reach]`; `[slow]` cases run a full slice, `[.benchmark]` is hidden):

```
tests/fff_print/fff_print_tests "[SmartSupports]"
tests/libslic3r/libslic3r_tests "[SmartSupports]"
tests/fff_print/fff_print_tests "[sas-reference-bench]"     # hidden: phase timings (notes/timings.md)
tests/fff_print/fff_print_tests "[sas-merge-calibration]"   # hidden: MayMergeOntoModel vs the slicer
tests/slic3rutils/slic3rutils_tests "[SmartSupports]"       # GUI build: the panel's wait logic (no display)
```

With asserts on (Debug or `-UNDEBUG`), exclude `"Reachability: real contact cross-check against
process()"`: stock 2.9.6 organic support asserts in `MutablePolygon.cpp:244` on its plain manual
enforcers (upstream, `notes/wp-log-wp5.md`):
`fff_print_tests "[SmartSupports]~\"Reachability: real contact cross-check against process()\""`.

**On the host** (after the sandbox build passes; full checklist in plan §7.3):
- Panel: fields and collapse state persist; the gizmo window stays on the canvas at
  1366×768 / 1920×1080, 100 % / 150 %, and scrolls; the report window sits right of it.
- T-shape: Analyse and paint → "Slicing…" → "Analysing layers… %" → cyan paint beyond ~2.5 mm;
  one undo removes it. Hollow dome: nothing painted (Automatic painting paints its top).
- Manual paint: enforcers and blockers stay, nothing is painted inside a blocker ("Kept free
  by your blocker"); unchecking a cause and running again removes its paint; "Remove planned
  support" keeps manual paint.
- Plate policy: T on a wide base — Prefer build plate paints with "Support on the model",
  Build plate only paints nothing and lists problems.
- Contract: pre-run warnings for support off / auto on / grid style, none for interface
  layers; with interface layers 3 a 40° wall shows the after-run "Fix…" line with its side
  effects; "Apply and paint" uses the new config; undo reverts.
- Preview: organic tips on the plate under planned support; on-model tips only where reported.
- 3MF: reopened in the fork keeps the colours ("analyse again for the report"); stock PrusaSlicer
  2.9.x shows blue enforcers, same support, no crash.
- Robustness: Smart fill / Triangles brush over planned support; cancel while slicing and
  analysing; closing the gizmo, switching or deleting objects, loading a project or quitting
  during analysis; brush and buttons disabled while busy; background processing on and off.
- Variants: object on bed 2, instances with different rotations, non-printable instance,
  multi-part objects with modifiers / negative volumes / enforcer and blocker volumes,
  scaled / mirrored / rotated objects, raft, variable layer height, first layer ≠ layer
  height; copy/paste, split, Cut, Simplify; "Automatic painting" after a planner run.
- Report row → marker at the right place; "Clip at this height".
- A ~1M-triangle figurine: run time, paint and 3MF size, undo memory, brush FPS.
- Language switch, colour-blindness palette check, dev build (wx 3.3, Wayland).

## Upstream status

Not proposed. Upstream has `SupportSpotsGenerator` (FFF, extrusion-based) and island
detection for SLA only (`src/libslic3r/SLA/SupportIslands/`). The 3.0 alphas (up to alpha12)
add nothing on this topic (`notes/theory.md` §12).

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.6 | concept | started 2026-10-13 |
| version_2.9.6 | implement v1 | theory reviewed and amended, plan reviewed in three rounds; started 2026-10-15 |
| version_2.9.6 | rename in the UI | "Smart painting" → "Support planner (layer analysis)", "smart paint" → "planned support", clearer field names (2026-10-19): "smart" is taken by upstream's Smart fill; id, branch and code names unchanged |
| version_2.9.6 | release v1 | host tests passed; shipped in `v2.9.6+ext-v1.1.0` (2026-11-01) |
| version_2.9.7 | keep | ported from `v2.9.6+ext-v1.0.0` (own commits only), no conflicts |

## Notes

- `guide.md` — end-user guide: panel, colours, report, own paint, saving, limits (v1).
- `notes/concept.md` — general concept, open questions (discussion document).
- `notes/theory.md` — theory: failure model, shared base model, detector classes (islands,
  tension regions, cantilevers, stability), plate reachability and the slicer's contact,
  composition rules, reference cases; amended for v1 (2026-10-15).
- `notes/plan.md` — implementation plan v1: decisions, architecture and interfaces, test
  plan, host checklist, work packages, review resolution, open questions.
- `notes/research-algo.md`, `notes/research-gui.md` — code research on 2.9.6 (libslic3r
  algorithm base; gizmo, paint storage, 3MF).
- `notes/wp-log*.md` — per work package: exact deviations from the plan, decisions on
  ambiguities, notes for later work packages; `wp-log-wp5.md`: integration, settled reference
  expectations, calibration, known limitations.
- `notes/timings.md` — phase timings on real meshes vs the plan's budget.
- `reviews/` — the plan reviews (`plan-review-{1..7}-*.md`, three rounds).
