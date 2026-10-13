---
id: smart-auto-supports
kind: feature
title: Smart automatic support painting (geometric island / bridge / cantilever detection)
show: false
upstream_pr: none
---

# feature/smart-auto-supports — smart automatic support painting

## What / why

The existing "Automatic painting" in the paint-on supports gizmo (`SupportSpotsGenerator`)
works on extrusions with a hard-coded 16 mm bridge limit that shrinks with curvature. It
paints areas that print fine — e.g. dome ceilings that close over a few mm — and ignores
that current printers bridge several cm.

Goal: a second, geometric auto mode that only paints where support is really needed:
islands / local minima (parts starting in mid-air), bridges longer than a configurable span,
and cantilevers longer than a configurable reach — detected by comparing the layer slices.

**Status: concept phase.** No code yet. See `notes/concept.md`.

## How to test

TBD (defined in the plan, once the concept is settled).

## Upstream status

Not proposed. Upstream has `SupportSpotsGenerator` (FFF, extrusion-based) and island
detection for SLA only (`src/libslic3r/SLA/SupportIslands/`).

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.6 | concept | started 2026-10-13 |

## Notes

- `notes/concept.md` — general concept, open questions (discussion document).
