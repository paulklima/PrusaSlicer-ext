# Concept — smart automatic support painting

Discussion document. General concept only; the implementation plan comes later in
`notes/plan.md`. Status: **draft, open for discussion** (2026-10-13).

## 1. Problem

Paint-on supports offers three ways to find support areas, none of which matches
"support only what really fails":

| Tool | What it does | Problem |
|---|---|---|
| Highlight overhang by angle + Enforce | every facet steeper than the angle | far too much: knows nothing about anchoring or bridging |
| Smart fill | flood fill over neighbouring facets within the *relative* angle | selects whole surfaces; it is a selection tool, not a detector |
| Automatic painting (`SupportSpotsGenerator`) | walks every extrusion, checks support from the layer below, stability model | see below |

Why "Automatic painting" over-supports (2.9.6 code):

- `bridge_distance = 16.0f` mm is hard-coded (`SupportSpotsGenerator.hpp:56`), not a setting.
- The allowed unsupported length is `bridge_distance / (1 + |curvature|)^3`, floor 3 mm
  (`SupportSpotsGenerator.cpp:368-371`, `431-435`).
- Near the top of a dome each layer steps far inwards; the perimeter circles hang almost
  completely in the air and are strongly curved → limit drops to 3 mm → `FloatingExtrusion`
  support points → `enforce_spot` paints a sphere around each (`TriangleSelectorWrapper.cpp:24`).
- A dome ceiling is a *maximum* of the inner surface and bridges from the wall on all sides;
  the extrusion-based check cannot see that.

## 2. Idea

Detect support needs **geometrically on the layer slices** (`layer->lslices`, ExPolygons),
independent of extrusion paths, infill pattern and perimeter generator. Compare each layer
with the one below and classify every newly unsupported region:

```
new_n = L_n − offset(L_{n−1}, w)        w = layer_height / tan(overhang_angle)
```

The overhang angle is thereby part of the slice comparison (the offset), not a separate
per-facet check.

Each connected component of `new_n` is one of:

### 2.1 Island (local minimum)

Component of `L_n` with **no overlap** with `L_{n−1}` — material would start in mid-air.

- Catches every local minimum, including downward tips (cones) that have **no** horizontal
  facet — a pure angle/normal check misses or misjudges those.
- Dome ceilings are never islands → ignored automatically.
- Always needs support, unless filtered (see 3).

### 2.2 Bridge (anchored on two opposite sides)

Unsupported region whose boundary touches `L_{n−1}` on (roughly) opposite sides.

- Span and direction from the existing `BridgeDetector` (anchors in the layer below,
  tests directions, prefers most lines with both ends anchored).
- Needs support only if span > **max bridge length** (user setting; modern printers: several
  cm). Support then as pillars along the span, not under the full area.
- Open: curved / ring bridges (toroid, ring-shaped ceilings) — straight lines do not work;
  treat as "span = max distance to nearest anchor"?

### 2.3 Cantilever (anchored on one side only)

Unsupported region that touches `L_{n−1}` only on one side: shelves, T-shapes, flat
sideways overhangs.

- Not a local minimum → island detection alone misses it.
- Needs support if the reach (max distance from the anchor edge into the region) >
  **max cantilever length** (user setting).

## 3. Filters / thresholds

- **Island size over height, not per layer.** A cone tip starts with ~0 mm²; a single-layer
  area threshold would wrongly drop it. Accumulate area/volume over the first ~1–2 mm of
  height of the island and decide on that.
- **Merge window.** If an island merges with the main body within k layers (mesh noise, tiny
  dents, text), treat it as a small overhang instead of an island.
- **Overhang angle** → offset `w` (see 2).
- **Max bridge length**, **max cantilever length** — user settings, sensible defaults
  (to discuss: 30–50 mm bridge, ~5 mm cantilever?).

## 4. Output and integration (rough)

- New libslic3r module working on slices after `posSlice` (faster, more robust than the
  extrusion walk).
- Result: support spots with a cause (island / bridge / cantilever), painted as enforcers via
  the existing `enforce_spot` path → user can still edit the result by hand.
- GUI: second button "Automatic painting (geometric)" next to the existing one, with sliders
  for the thresholds. Existing automatic painting stays unchanged.
- Template for tracking components across layers: SLA island code
  (`src/libslic3r/SLA/SupportPointGenerator.*`, `src/libslic3r/SLA/SupportIslands/`).

## 5. Prior art (research 2026-10-13)

- Layer-difference detection `O_n = L_n − L_{n−1}` with offset: classic (survey arXiv
  1705.03811; patent US 8903533). Known weakness: thin protrusions sometimes wrongly treated
  as supported.
- Vertex local minima ("lower than all neighbours"): used in mesh-based detection; the slice
  island test is the layer equivalent.
- OrcaSlicer / Bambu Studio "Support critical regions only": supports only "sharp tails"
  (≈ islands) and cantilevers — closest existing implementation; read their source before
  implementing (not verified yet).
- PrusaSlicer `SupportSpotsGenerator`: extrusion-based, stability model (center of mass, bed
  adhesion, weak necks). Ideas worth keeping: stability checks (`WeakObjectPart`,
  `SeparationFromBed`) are something a pure geometric check does not cover.
- Arc / wave overhangs (CNC Kitchen, OrcaSlicer-WaveOverhangs): alternative for curved
  overhangs without support.

## 6. Open questions

1. Scope: only islands first, or islands + bridges + cantilevers in one go? (Feature rule:
   one usable capability — islands alone might already be usable.)
2. Keep the stability checks from `SupportSpotsGenerator` (combine) or purely geometric?
3. Curved / ring bridges: how to measure the span?
4. Defaults for max bridge length / max cantilever length; per-filament or global?
5. Painting: spots (spheres) or paint the whole unsupported facet region of an island?
6. Should the thresholds be print settings (also usable without the gizmo, e.g. a new
   "auto (smart)" support mode) or gizmo-only?
