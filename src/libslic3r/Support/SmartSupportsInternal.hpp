#ifndef slic3r_SmartSupportsInternal_hpp_
#define slic3r_SmartSupportsInternal_hpp_

// Smart support painting: internal types and functions of all analysis modules (plan §3.2).
// Not used by the GUI. The conventions of SmartSupports.hpp apply (layer vectors sized in.layers.size(),
// scaled polygons in the PrintObject frame, pure and thread-safe unless stated otherwise).
// Classes with private state use a private Impl defined in the owning module's .cpp file.
//
// Owners (plan §2.1): projection WP1a (SmartSupportsProjection.cpp), Phase A WP2a (SmartSupportsBase.cpp),
// Reachability WP2r (SmartSupportsReachability.cpp), raster/reach WP2b1 (SmartSupportsRaster.cpp,
// SmartSupportsReach.cpp), tension WP2b2 (SmartSupportsTension.cpp), report WP2c0 (SmartSupportsReport.cpp),
// evaluator WP2c1a (SmartSupportsEval.cpp), plan WP2c1b (SmartSupportsPlan.cpp, SmartSupports.cpp),
// paint WP1b (SmartSupportsPaint.cpp).

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Support/SmartSupports.hpp"

namespace Slic3r::SmartSupports {

// ===== Shared geometry helper (WP5; defined in SmartSupportsBase.cpp)
// Outward offset by delta > 0 that keeps thin input. ClipperUtils' offset() drops the vertices of a path that lie
// closer than ClipperOffsetShortestEdgeFactor·|delta| (0.5 %) to the previous one before offsetting, so a strip
// thinner than that degenerates and its offset is empty (a 13 µm slope front grown by the 2.8 mm cluster margin).
// Here the offset runs in two steps, 1 % of delta first: a strip survives down to 0.005 % of delta (0.14 µm at
// 2.8 mm). Joins as offset(); limit = miter limit or arc tolerance (the first step uses 2 % of its own distance). A
// round result lies in src ⊕ delta and contains src ⊕ (delta − 2.25·limit − 0.0008·delta): Clipper rounds the number
// of arc steps per vertex (up to 1.5 × the nominal step angle, 2.25 × its sagitta), and step 2 drops vertices of
// step 1's arcs. delta ≤ 0: plain offset(). Not for the mirrors of slicer offsets (Reachability): they must lose what
// the slicer loses.
Polygons offset_keep_thin(const ExPolygons &src, float delta, ClipperLib::JoinType join = DefaultJoinType,
                          double limit = DefaultMiterLimit);

// ===== Projection (WP1a). Result size = in.layers.size(); stub: all empty.
// Projects the facets of each volume whose state passes filter onto the layers, like
// PrintObject::project_and_append_custom_facets(): slice_mesh_slabs() of the facets with VolumeInput::trafo.
// Per layer the polygons of all volumes, in volume order; they may overlap (consumers union them).
// per_volume.size() == in.volumes.size(); nullptr = use VolumeInput::paint. Parallel over volumes inside.
std::vector<Polygons> project_states(const AnalysisInput &, const std::vector<const TriangleSelector::TriangleSplittingData*> &per_volume,
                                     const std::function<bool(TriangleStateType)> &filter, const ThrowOnCancel &);
// Slices the support enforcer (enforcers = true) or blocker modifiers at the layers' slice_z, like
// PrintObject::slice_support_volumes(); per layer the polygons of all such volumes (may overlap).
// Flips the triangle orientation if det(trafo) < 0.
std::vector<Polygons> slice_modifiers(const AnalysisInput &, bool enforcers, const ThrowOnCancel &);  // flips if det < 0

// ===== Phase A (WP2a), plan §3.5, §3.6. Stub: layers sized, z/h/w filled, no fronts, no trees.
// One front: a connected component C of the unsupported area U_n = L_n − S_n.
struct FrontGeom {
    ExPolygon C;
    BoundingBox bbox;                     // bbox of C
    double area_mm2 = 0;
    bool wide = false;                    // !opening_ex({C}, ext_width).empty()
    std::vector<Vec2d> thetas;            // wide only: unit directions of the bridge lines (n ≡ −n); ≤ max_tie_dirs, canonical order
    int hoop_hole = -1;                   // D26: index into C.holes of the hoop hole, −1: no hoop candidate
    double hoop_feret_mm = 0;             // Feret diameter of the matching slice hole
    std::vector<int> slices;              // indices into LayerInput::lslices of the slice(s) containing C
};
// Fronts whose windows overlap form a cluster; Phase B evaluates clusters independently (D25).
struct Cluster {
    BoundingBox win;                      // ∪ bbox(C) ⊕ (m + w); windows of a layer are disjoint
    std::vector<uint32_t> fronts;         // indices into LayerGeom::fronts, canonical order
    ExPolygons band;                      // L_n ∩ (∪C ⊕ m),  m = cluster_margin + rho_exact + w
    ExPolygons band_prev;                 // L_{n−1} ∩ (∪C ⊕ (m + w))
};
struct LayerGeom {
    const LayerInput *in = nullptr;       // &AnalysisInput::layers[n]
    double z_bot = 0, z_top = 0, h = 0;   // slab [slice_z − h/2, slice_z + h/2), mm
    coord_t w = 0;                        // self-supporting step w_n (§3.4); 0 at layer 0
    coord_t w_beta = 0;                   // accumulation step w_β (≥ w), = w if β = α
    std::vector<FrontGeom> fronts;        // all components of U_n (no area filter), canonical order
    std::vector<Cluster> clusters;        // disjoint windows
    ExPolygons unprintable;               // L − opening(L, ¼·ext_w) (small)
    std::vector<uint8_t> slice_bad;       // per slice: weak contact (§3.5), size lslices.size()
    std::vector<int32_t> slice_tree;      // per slice: IslandTree::id or −1 (grounded), size lslices.size()
    ExPolygons tolerated;                 // T_n
    std::vector<std::pair<int32_t, ExPolygons>> tail_needs;   // per tree (component-wise, w = 0)
    ExPolygons manual_enf, manual_blk;    // project_states(1 / 2)[n] ∪ slice_modifiers(enf / blk)[n]
};
struct IslandTree { int32_t id = -1; uint32_t first = 0, merge = UINT32_MAX; double volume_mm3 = 0; bool tolerated = false;
                    uint32_t tail_last = 0; bool tail_without_downward = false; };   // merge = UINT32_MAX: never
struct BaseModel {
    std::vector<LayerGeom> layers;        // size layers
    std::vector<IslandTree> trees;        // indexed by IslandTree::id
    uint32_t last_layer_with_fronts = 0;  // max n with fronts, tails or manual enforcers
};
BaseModel build_base_model(const AnalysisInput &, const ThrowOnCancel &, const ProgressFn &);   // parallel inside
double feret_diameter(const Polygon &);                                              // scaled; longest chord
// Bridge line directions of a wide front (BridgeDetector.hpp:88-128 cost loop): all candidates within
// tie_tol of the best cost, deduplicated modulo 180°, sorted by (cost, angle mod π), at most max_dirs.
std::vector<Vec2d> bridge_directions(const ExPolygons &bridge_region, const ExPolygons &lower_local, double tie_tol, int max_dirs);

// ===== Reachability (WP2r), plan §3.7. Stub: tips_possible = true; tips.bp = F_grown; predict_F = F_grown;
//       cover = F; plate_part = Z; model_part = ∅; ensure/prefetch no-op.
// Indices: coll/avoid are indexed by SUPPORT layer s (object layer + raft_support_layers); s < raft_support_layers
// → ∅ (no object there). tip_layer(n) returns the support-layer index (review 7 m3).
// E arguments are ALWAYS the union of all painted zones of layer n (manual + accepted + trial): the roof
// decision is per union component (review 7 M2).
class Reachability {
public:
    explicit Reachability(const AnalysisInput &);                    // keeps a reference to the input
    ~Reachability();                                                 // sets stop, joins the prefetch thread
    Reachability(const Reachability &) = delete;
    Reachability &operator=(const Reachability &) = delete;

    void   compute_collisions(const ThrowOnCancel &);                // parallel; once, before prefetch/ensure
    void   start_prefetch(int target_support_layer, IsCanceled);     // std::thread, single writer up to target
    void   ensure(int support_layer, const ThrowOnCancel &);         // serial caller; k < 0 no-op; k > target: join first,
                                                                     // then advance under the mutex; rethrows prefetch errors
    int    tip_layer(uint32_t n) const;                              // n + raft_support_layers − (z_top + 1)
    bool   tips_possible(uint32_t n) const;                          // tip_layer(n) ≥ max(raft_support_layers − (z_top+1), 1)
    double gap_band_mm() const;                                      // G = xy_min (review 7 M1)
    // const queries; thread-safe for tip_layer(n) ≤ last ensure();
    // window = bbox(arg) ⊕ (tip_d + extra_outset + r + xy + 1 mm)
    // (E ∩ O_n ⊕ tip_d) − L_{n−1}, O_n = L_n − L_{n−1} (TreeSupport.cpp:289-299)
    ExPolygons F_grown(const ExPolygons &E, uint32_t n) const;
    struct Tips { ExPolygons bp, model, spill_model; };              // status by Params::buildplate_only (D35)
    Tips       tips(const ExPolygons &E, uint32_t n) const;
    struct Contact {
        ExPolygons all, plate, model;                                // the slicer's contact (roof or tip path per component)
        ExPolygons tip_path_all;                                     // contact if every component took the tip path
        bool roof_gap = false;                                       // some component took the roof path (D30)
    };
    Contact    contact(const ExPolygons &E, uint32_t n) const;
    ExPolygons predict_F(const ExPolygons &E, uint32_t n) const { return contact(E, n).all; }
    ExPolygons cover(const ExPolygons &F, uint32_t n) const;         // F ∪ (L_n ∩ (F ⊕ G) ∩ (L_{n−1} ⊕ G))
    ExPolygons plate_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const;
        // Z ∩ cover(contact(E_layer ∪ Z).plate)
    ExPolygons model_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const;
        // Z ∩ cover(contact(E_layer ∪ Z).model) − plate_part
    bool       below_model_too_short(const ExPolygons &Z, uint32_t n) const;   // tip-path contacts only (D23)

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ===== Raster + reach (WP2b1), plan §3.8. Stubs: ReachField = Euclidean nearest seed + ĉ, Direct, res 0.1 mm; c_hat = 0.
// Cell (ix, iy) ↔ v[iy * cols + ix] / m[iy * cols + ix]; its centre is origin + ((ix + ½)·res, (iy + ½)·res).
struct Grid { Point origin; coord_t res = 0; int cols = 0, rows = 0; std::vector<float> v; std::vector<uint8_t> m; };
Grid       make_grid(const BoundingBox &, coord_t res);             // origin shifted (0.37, 0.61)·res; covers the bbox
void       rasterize(const ExPolygons &, Grid &, uint8_t value, bool conservative);   // m = value for cells inside
ExPolygons cells_to_expolygons(const Grid &, const std::function<bool(int idx)> &pred);  // union of the cell squares
struct AnchorSeg { Line l; float c0, c1; };                         // ĉ at endpoints, mm (linear in between)
class ReachQuery {                                                   // query interface (synthetic in WP2b2 tests)
public:
    virtual ~ReachQuery() = default;
    virtual float   value(const Point &p) const = 0;                 // mm; finite inside a grounded C'
    virtual coord_t res() const = 0;                                 // sample spacing for classification
    virtual bool    direct_mode() const = 0;
};
// c(p) of one D component (D11): exact within ρ of the seeds, raster Dijkstra beyond.
class ReachField final : public ReachQuery {                         // immutable after construction, thread-safe
public:
    ReachField(const ExPolygons &cprime /* C' of the component */, const ExPolygons &domain /* D component */,
               std::vector<AnchorSeg> seeds, double delta_beta_mm, const Params &, const ThrowOnCancel &);
    ~ReachField() override;
    float value(const Point &) const override; coord_t res() const override; bool direct_mode() const override;
    std::shared_ptr<const ReachQuery> cropped(const BoundingBox &bbox) const;   // for the carry (review 5 m8)

    struct Impl;
private:
    explicit ReachField(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> m_impl;
};
// One front of a committed layer, kept by ReachCarry for the ĉ lookup of the layers above.
struct FrontCarry { ExPolygon C; BoundingBox bbox; bool grounded = false; std::shared_ptr<const ReachQuery> field;
                    ExPolygons zero; std::shared_ptr<const AABBTreeLines::LinesDistancer<Line>> boundary; };
// Inherited reach ĉ (§2.3, §3.8): keeps the fronts of the layers within H_s below the current one.
class ReachCarry {
public:
    explicit ReachCarry(const Params &);
    ~ReachCarry();
    ReachCarry(ReachCarry &&) noexcept;
    ReachCarry &operator=(ReachCarry &&) noexcept;
    void  push(uint32_t layer, double z_top, coord_t w, std::vector<FrontCarry> &&all_fronts);  // serial; keeps the H_s window
    float c_hat(const Point &a, uint32_t n, double z_top_n, double h_n, coord_t w_n) const;     // pure; thread-safe

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
// min over s ∈ [0, len] of c0 + g·s + max(0, √(d² + (s − sp)²) − Δ), Δ = delta_beta_mm (§3.8 closed form), mm
float segment_min(const AnchorSeg &, const Point &p, double delta_beta_mm);

// ===== Tension, hoop, classification (WP2b2), plan §3.8. Stubs: anchors = material ∂C'; tension None; hoop ∅;
//       classify by c only.
struct Anchors { Polylines material, support, free_edges; };       // parts of ∂C'
Anchors compute_anchors(const ExPolygons &cprime, const ExPolygon &C, const ExPolygons &grounded_printable,
                        const ExPolygons &F, const ExPolygons &L);  // all band-local
// Distancers over the anchor edges (∂D ∩ stop) and the free edges of a D component.
struct DomainEdges { std::shared_ptr<const AABBTreeLines::LinesDistancer<Line>> anchor, free_; };
DomainEdges domain_edges(const ExPolygons &D_component, const ExPolygons &stop /* GP ∪ F */);
// Tension level of a point; Covered = t ≤ B_max; None = no finite span.
enum class TLevel : uint8_t { Covered, May, Should, Must, None };
struct TensionMap {
    std::vector<std::pair<ExPolygons, TLevel>> regions;   // worst over tied θ
    ExPolygons covered_all;                               // Covered under all θ
    struct Cut { Point p; Vec2d dir; double t; double lo, hi; };   // cut interval [lo, hi] along the span
    std::vector<Cut> cuts;                                // long-span row candidates
};
TensionMap tension_levels(const ExPolygons &cprime, const FrontGeom &, const ExPolygons &D_component,
                          const DomainEdges &, const Anchors &, const Params &);
// C' ∩ (H ⊕ B_max·hoop_width_factor) if the front is a grounded hoop candidate with hoop_feret ≤ B_ceil, else ∅
ExPolygons hoop_credit(const ExPolygons &cprime, const FrontGeom &, bool grounded, const Params &);
struct FrontNeeds {
    ExPolygons need[2][3];                                // [0 cantilever, 1 long span][level]
    ExPolygons zero;                                      // covered_all ∪ hoop band (c* = 0 for the carry)
    float max_c = 0;
};
FrontNeeds classify_front(const ExPolygons &cprime, const ReachQuery &, const TensionMap &,
                          const ExPolygons &hoop_band, float max_chat, const Params &);

// ===== Evaluator (WP2c1a), plan §3.8. Stub: evaluate returns empty needs; commit no-op; evaluate_pass empty.
struct LayerNeeds {
    ExPolygons need[3][3];                                // [cause][level] after tail, overrides, filter, folding
    std::vector<std::pair<int32_t, ExPolygons>> folded;   // per unplaced island tree
    std::vector<std::pair<ExPolygons, Why>> why;          // pieces with a known blocking reason (InterfaceGap, NoTipLayer)
    ExPolygons cprime;                                    // ∪ C' (for widening)
    std::vector<TensionMap::Cut> cuts;                    // long-span cut candidates
    LayerStats stats;
};
class LayerEvaluator {                                    // one caller thread; parallel over clusters inside
public:
    // Keeps references to all arguments; layers are evaluated bottom up from n = 1.
    LayerEvaluator(const AnalysisInput &, const BaseModel &, const Reachability &, const ThrowOnCancel &);
    ~LayerEvaluator();
    LayerEvaluator(const LayerEvaluator &) = delete;
    LayerEvaluator &operator=(const LayerEvaluator &) = delete;
    // n = last committed layer + 1. E = union of ALL painted zones of layer n (manual + accepted + trial);
    // the evaluator computes contact = reach.contact(E, n) itself (F = contact.all, tip_path_all for
    // InterfaceGap). overrides = (need region, cause) of accepted placements at n. touched = bbox of the
    // contact change since the previous evaluate(n) (nullopt = all clusters). The returned reference is
    // invalidated by the next evaluate()/commit(): callers comparing evaluations copy (review 7 m6).
    const LayerNeeds &evaluate(uint32_t n, const ExPolygons &E,
                               const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                               std::optional<BoundingBox> touched);
    void commit(uint32_t n);                              // UG_n, carry, stats of the last evaluate(n)
    void mark_tree_unplaced(int32_t tree);                // fold the needs hanging from this island tree

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
struct PassResult { std::vector<LayerNeeds> layers; };   // size = in.layers.size(); cprime/cuts cleared
// for n = 1…: ensure(tip_layer(n)); evaluate(n, E[n], {}, nullopt); commit(n). E_per_layer.size() == in.layers.size().
PassResult evaluate_pass(const AnalysisInput &, const BaseModel &, Reachability &,
                         const std::vector<ExPolygons> &E_per_layer /* painted zones */, const ThrowOnCancel &, const ProgressFn &);

// ===== Plan (WP2c1b), report (WP2c0), paint (WP1b)
// A need that is reported (not, or not fully, placed).
struct Need { Cause cause = Cause::Island; Level level = Level::Must; Why why = Why::None; uint32_t layer = 0;
              ExPolygons area; ReportState reason = ReportState::Unplaceable; };
struct Plan {
    std::vector<Placement> placements;
    std::vector<Need> residual;            // EVERYTHING to report: incl. folded trees, tails without downward
                                           // facets (area ∅), PartlyOnModel / MayMerge flags (WP2c1b fills)
    std::vector<LayerStats> stats;         // size layers
    std::vector<ExPolygons> zones_paintable;   // size layers; areas this run paints (paintable && cause checked)
    std::vector<ExPolygons> zones_unpainted;   // size layers; planned but not painted (long spans, unchecked causes)
                                               // the plan itself stays toggle-independent (P3); only this split reads the toggles
};
// The BaseModel is built and the prefetch started by run() before this call (review 7 m6).
Plan make_plan(const AnalysisInput &, const BaseModel &, Reachability &, const ThrowOnCancel &, const ProgressFn &);
// §3.10: needs left by the actual paint (RemainingAfterPaint); empty without a contact deficit.
std::vector<Need> verify(const AnalysisInput &, const BaseModel &, Reachability &, const Plan &,
                         const std::vector<VolumePaint> &, const ThrowOnCancel &);
// §3.11: merged, sorted, capped entries; report_more_out = hidden entries per group [problems, notes].
std::vector<ReportEntry> build_report(const AnalysisInput &, const Plan &, const std::vector<Need> &verify_residual,
                                      int report_more_out[2]);
// §4: paints the paintable placements of the given causes (CauseBit set) with level ≥ lowest onto private
// selectors; all earlier smart paint is removed first (a run replaces all smart paint), manual paint is kept.
// size = in.volumes.size(), volume order; parallel over volumes; stub: unchanged, changed = false
std::vector<VolumePaint> paint_placements(const AnalysisInput &, const std::vector<Placement> &, uint8_t causes,
                                          Level lowest, const ThrowOnCancel &);

} // namespace Slic3r::SmartSupports

#endif // slic3r_SmartSupportsInternal_hpp_
