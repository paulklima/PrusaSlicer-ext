# Timings — smart support painting (WP5)

Measured by the hidden benchmark `tests/fff_print/fff_print_tests "[sas-reference-bench]"`
(`Reference #25: phase timings`, `tests/fff_print/test_smart_supports.cpp`) on 2026-10-16 in the sandbox:
8 cores, Release (`-O3`), feature tip merged with dev/2.9.6 (`wp-build.sh`, lane 1). α 45°, §7 default config
(organic, interface layers 0, contact 0.2), prefer build plate, critical and likely, verification on.

Each phase is timed alone, in `run()`'s order. The avoidance chain is computed serially here; `run()` computes it on
the prefetch thread alongside the plan. "Verification" is the real cost: the projection and the contact check, plus one
evaluation pass if the paint's contact falls short of the plan somewhere (marked †). "Full pass" is that pass alone (the
worst case of the verification). `run()` is the wall time of the whole analysis with verification. Slicing (posSlice of
the slicer, not part of the analysis) is listed for comparison. All times in seconds.

## Budget (plan §3.13)

| Item | Budget (1000-layer figurine, 8 cores) | Measured, largest case |
|---|---|---|
| Phase A | 4–8 s | 0.98 s (sphere, 1.3M triangles, 400 layers); 0.13 s for the ≈ 1000-layer meshes |
| collisions | parallel with Phase A | ≤ 0.05 s |
| avoidance chain | 3–8 s, on the prefetch thread | ≤ 0.13 s |
| Phase B (plan) | 8–15 s | ≤ 1.74 s |
| paint | 1–3 s | ≤ 0.23 s (1.3M triangles) |
| verification | ≈ 0 without deficits | ≤ 0.17 s without, 1.21 s with a deficit pass |
| **total** | **≤ 30 s** | **≤ 3.1 s** (`run()`); 1.1–1.9 s for the ≈ 1000-layer meshes |

The budget is met by an order of magnitude. Verification stays on by default (plan §8.2 WP5: "verification as an
option" only if it exceeded 10 s). No hotspot needed fixing; the plan (Phase B) dominates everywhere.

## Per shape

| Shape | h | Layers | Triangles | Fronts | Placements | Slicing | Phase A | Collisions | Chain | Plan | Paint | Verification | Full pass | `run()` |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| frog_legs, stood up | 0.2 | 249 | 18 334 | 194 | 146 | 0.03 | 0.06 | 0.005 | 0.02 | 0.59 | 0.02 | 0.08 | 0.21 | 0.77 |
| frog_legs, stood up, ×2 | 0.2 | 497 | 18 334 | 428 | 100 | 0.05 | 0.06 | 0.013 | 0.04 | 0.46 | 0.02 | 0.05 | 0.25 | 0.60 |
| extruder_idler | 0.2 | 190 | 3 586 | 245 | 8 | 0.02 | 0.02 | 0.003 | 0.01 | 0.19 | 0.003 | 0.003 | 0.18 | 0.23 |
| extruder_idler ×3 | 0.2 | 569 | 3 586 | 766 | 50 | 0.03 | 0.06 | 0.010 | 0.06 | 1.51 | 0.03 | 1.21 † | 1.22 | 2.91 |
| figurine_proxy | 0.2 | 480 | 15 720 | 287 | 0 | 0.08 | 0.03 | 0.010 | 0.03 | 0.14 | 0 | 0 | 0.13 | 0.19 |
| sphere R 20 | 0.2 | 200 | 20 480 | 88 | 11 | 0.08 | 0.03 | 0.008 | 0.002 | 0.24 | 0.01 | 0.02 | 0.20 | 0.31 |
| sphere R 20, 1.3M triangles | 0.2 | 200 | 1 310 720 | 29 | 2 | 4.54 | 0.51 | 0.009 | 0.004 | 1.02 | 0.22 | 0.17 | 0.89 | 1.96 |
| figurine_proxy, 1M triangles | 0.2 | 480 | 985 440 | 46 | 0 | 3.22 | 0.37 | 0.014 | 0.04 | 0.43 | 0 | 0 | 0.44 | 0.81 |
| frog_legs, stood up | 0.1 | 497 | 18 334 | 432 | 219 | 0.05 | 0.11 | 0.013 | 0.03 | 0.95 | 0.04 | 0.13 | 0.38 | 1.26 |
| frog_legs, stood up, ×2 | 0.1 | 994 | 18 334 | 884 | 182 | 0.08 | 0.11 | 0.034 | 0.07 | 0.84 | 0.03 | 0.08 | 0.45 | 1.09 |
| extruder_idler | 0.1 | 380 | 3 586 | 506 | 10 | 0.02 | 0.04 | 0.010 | 0.03 | 0.26 | 0.004 | 0.004 | 0.24 | 0.32 |
| extruder_idler ×3 | 0.1 | 1139 | 3 586 | 1516 | 55 | 0.07 | 0.13 | 0.047 | 0.13 | 1.74 | 0.03 | 0.02 | 1.54 | 1.95 |
| figurine_proxy | 0.1 | 960 | 15 720 | 549 | 0 | 0.11 | 0.07 | 0.030 | 0.07 | 0.26 | 0 | 0 | 0.29 | 0.46 |
| sphere R 20 | 0.1 | 400 | 20 480 | 151 | 2 | 0.10 | 0.06 | 0.016 | 0.005 | 0.40 | 0.02 | 0.02 | 0.35 | 0.50 |
| sphere R 20, 1.3M triangles | 0.1 | 400 | 1 310 720 | 58 | 2 | 4.83 | 0.98 | 0.022 | 0.007 | 1.72 | 0.23 | 0.17 | 1.59 | 3.10 |
| figurine_proxy, 1M triangles | 0.1 | 960 | 985 440 | 188 | 0 | 3.68 | 0.68 | 0.049 | 0.08 | 0.87 | 0 | 0 | 0.87 | 1.64 |

Shapes: `frog_legs.obj` lies flat in `tests/data` (z 0–2 mm) and is stood up (90° about X, ≈ 50 mm tall); `×2`, `×3` scale
the mesh. `figurine_proxy` = spheres R 20 / 15 / 10 on a cylinder R 8 (sphere-heavy: many ring fronts, but every ring
stays below K_max, so nothing is placed); the 1M-triangle variants use finer tessellations of the same shapes.

## Memory

Peak RSS of the whole test process (slicing, prints and analysis together, cumulative over the run): 40 MB after the
first shape, 93 MB after the six coarse shapes at 0.2 mm (114 MB after all twelve in a run without the 1M shapes),
666 MB with the 1.3M-triangle sphere at 0.2 mm, 708 MB at 0.1 mm. The analysis itself keeps the
slices once, collisions and avoidance once each (plan §3.13).

## Notes

- Plan time is dominated by the cluster evaluations on layers with many fronts (extruder_idler ×3: 766 / 1516 fronts).
- The 1M-triangle cases: slicing (the slicer's posSlice) takes longer than the whole analysis; Phase A grows with the
  slice vertex count, paint with the triangles (0.22 s).
- extruder_idler ×3 at 0.2 mm (†): the actual paint falls short of the planned contact by ≥ A_min on some layer (prism
  under-approximation on its coarse triangles), so the verification runs its one evaluation pass (1.2 s); it finds no
  new need (no `RemainingAfterPaint`).
