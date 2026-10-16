#ifndef slic3r_SmartSupports_hpp_
#define slic3r_SmartSupports_hpp_

// Smart support painting (fork feature smart-auto-supports): public API.
//
// A geometric analysis of the layer slices of one PrintObject (doc/fork/features/smart-auto-supports/notes/
// theory.md, plan.md) decides where support is really needed (islands, cantilevers, long spans) and paints
// support enforcers there as auto states 3..9 (SmartSupportStates.hpp).
//
// Usage (plan §2.2): on the GUI thread, after the PrintObject reached posSlice,
//     choice = choose_print_object(...); if (choice.ready && input_consistent(...)) in = build_input(...);
// then on a worker thread
//     result = run(in, options, is_canceled, progress);
// The worker never touches Print, Model or wx: AnalysisInput is a self-contained copy.
//
// Conventions of this header and of SmartSupportsInternal.hpp:
// - every per-layer vector has exactly in.layers.size() entries (object layers, no raft layers);
// - polygons are scaled coord_t in the PrintObject frame (po.trafo_centered() applied, plan §3.3);
//   z values are in mm in the slice_z frame (object frame, no raft);
// - functions are pure and thread-safe unless stated otherwise;
// - cancellation: ThrowOnCancel throws CanceledException (PrintBase.hpp); results are then discarded.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class ModelConfig;
class ModelObject;
class Print;
class PrintObject;
class PrintObjectConfig;

namespace SmartSupports {

// Why support is needed. The cause bit of an auto state is 1 << int(cause) (CauseBit).
enum class Cause : uint8_t { Island, Cantilever, LongSpan };
// Need level (theory §2.5), ordered: May < Should < Must. Used as an array index [0..2].
enum class Level : uint8_t { May, Should, Must };
// Placement policy of the planner (§3.9). The slicer's own support_material_buildplate_only is Params::buildplate_only.
enum class PlatePolicy : uint8_t { PreferPlate, PlateOnly };

inline constexpr uint8_t cause_bit(Cause c) { return uint8_t(1u << int(c)); }

// User parameters: panel fields, persisted in AppConfig (plan §6.8). Lengths in mm, angles in degrees.
// Defaults: Prusa MK4S, 0.4 nozzle, with a safety margin (guide §4 "Why these defaults"); the theory placeholders of
// theory §9 (α 45, K_max 3, B_max 40, B_ceil 40, H_s 1) are what the reference tests pin.
struct UserParams {
    uint8_t     paint_causes = cbIsland | cbCantilever; // CauseBit set of causes to paint; cbLongSpan is ignored in v1
    int         alpha_deg    = 45;                      // overhang angle α from horizontal; 0 = auto: w_n = 0.5·ext_width_avg(n−1)
    double      K_max  = 2.5;                           // max accumulated overhang reach
    double      B_max  = 25.0;                          // max bridge span
    double      B_ceil = 25.0;                          // max Feret diameter of a hoop hole (dome opening)
    double      H_s    = 1.0;                           // reach fade (stiffening) height; 0 = per layer
    bool        beta_equals_alpha = true;               // accumulation angle β = α
    double      beta_deg = 45.0;                        // β if !beta_equals_alpha, clamped to (0, α_eff] (§3.4)
    PlatePolicy plate_policy = PlatePolicy::PreferPlate;
    Level       lowest_level = Level::Should;           // lowest painted level: Should or Must (D9)
};

// All parameters of one analysis run. Lengths in mm, areas in mm², volumes in mm³, angles in degrees.
struct Params {
    UserParams user;
    // ---- derived by derive_params() (WP0, impl). Defaults: PrintConfig defaults of 2.9.6, nozzle 0.4
    //      (interface_layers = 3 is the PrintConfig default; tests use test_params(), which sets 0).
    double ext_width = 0.45;                                      // max external perimeter width over regions (TreeSupportCommon.cpp:36-40)
    double support_line_width = 0.4;                              // support_material_flow(&po, layer_height).width()
    double tip_diameter = 0.8;                                    // support_tree_tip_diameter clamped to [0, branch_diameter]
    double branch_diameter = 2.0;                                 // support_tree_branch_diameter
    double tree_angle_deg = 40.0;                                 // support_tree_angle, clamped to [0, 90)
    double tree_angle_slow_deg = 25.0;                            // support_tree_angle_slow, clamped to [0, tree_angle] (TreeSupportCommon.cpp:83)
    double branch_distance = 1.0;                                 // support_tree_branch_distance
    double tip_spacing = 0.4 * 100. / 15. + 0.4;                  // connect_length = lw·100/top_rate + max(2r − lw, 0), TreeSupport.cpp:1258 (≈ 3.07)
    double xy_distance = 0.225;                                   // support_material_xy_spacing of ext_width (TreeSupportCommon.cpp:58)
    double xy_min_distance = 0.225;                               // min(xy, 0.5·ext_width) (TreeSupportCommon.cpp:60, 108)
    bool   min_xy_dist = false;                                   // xy_distance > xy_min_distance (TreeSupport.cpp:1252)
    double layer_height = 0.2;                                    // PrintObjectConfig::layer_height
    double first_layer_height = 0.2;                              // SlicingParameters::first_object_layer_height
    double gcode_resolution = 0.0125;
    double contact_distance = 0.2;                                // top contact Z distance (support_material_contact_distance)
    int    z_top_layers = 1;                                      // round(gap_support_object / layer_height) (TreeSupportCommon.cpp:113)
    int    z_bottom_layers = 1;                                   // round(gap_object_support / layer_height)
    int    interface_layers = 3;                                  // support_material_interface_layers (D30)
    int    raft_support_layers = 0;                               // = TreeSupportSettings::raft_layers.size() (TreeSupportCommon.cpp:147-176)
    bool   buildplate_only = false;                               // support_material_buildplate_only (D35)
    double custom_bridge_angle_deg = 0.0;                         // > 0: θ fixed (region bridge_angle)
    // ---- constants (theory §9 placeholders; mm, mm², mm³)
    double f_hard = 1.5;                                          // step between need levels
    double kappa = 0.10;                                          // weak contact: relative overlap
    double a_min_widths = 2.0;                                    // weak contact: overlap extent, in ext widths
    double eps = 0.01;                                            // noise filter: minimal need width (opening ε/2)
    double A_min = 0.1;                                           // minimal need component / progress per placement
    double delta_widths = 0.1;                                    // hanging tolerance δ, in ext widths
    double V_min = 1.0, H_merge = 1.0;                            // tolerated island trees (D22)
    double W_stiff = 3.0, H_tail = 16.0;                          // island tail: inscribed width, max height
    double hoop_width_factor = 0.5;                               // hoop band width = factor·B_max (D26)
    double hoop_outer_fraction = 0.90;                            // part of the hoop's outer contour on S_n
    double tie_tolerance = 0.10;                                  // bridge directions within 10 % of the best cost
    double flag_clearance_extra = 0.2;                            // clearance margin beyond tip_d + extra_outset (flags only, D10)
    double min_height_to_model = 1.0;                             // D23
    double minimum_roof_area = 1.0;                               // TreeSupportCommon.hpp:131
    double printable_width_factor = 0.5;                          // printable = opening by factor·ext_width/2
    double edge_limit = 0.25;                                     // painting: prism inflation e and selector edge limit (D17)
    double rho_exact = 0.5;                                       // exact near-anchor zone ρ (D11)
    double cluster_margin = 2.0;                                  // band margin m (§3.5)
    double grid_res_min = 0.05, grid_res_max = 0.1;
    size_t grid_max_pixels = 4'000'000;
    int    max_place_iterations = 4, max_tie_dirs = 8;
    bool   verification = true;                                   // run the verification (D13)
    bool   test_assume_reachable = false;                         // tests only: every position counts as reachable
};

// One deviation of the print config from what the analysis assumes (plan §6.4).
struct ContractIssue {
    enum Key : uint8_t { SupportOff, AutoOn, NotOrganic, EnforceLayers, ContactZero, BuildplateMismatch,
                         InterfaceLayers, Raft, ReachBelowTipSpacing } key = SupportOff;
    bool advisory = false;                              // InterfaceLayers, Raft, ReachBelowTipSpacing: never in the
                                                        // pre-run warning / "Apply and paint" gate (D30, D34)
    std::string current, recommended;                   // serialised option values ("" for info keys)
};

// One object layer, copied from Layer after posSlice.
struct LayerInput {
    double slice_z = 0, print_z = 0, height = 0;        // mm; slab [slice_z − h/2, slice_z + h/2)
    ExPolygons lslices;                                 // Layer::lslices
    struct Link { int32_t idx; float area; };           // index into lslices of the layer below / above; area scaled²
    struct SliceInfo { BoundingBox bbox; std::vector<Link> below, above; };   // Layer::lslices_ex
    std::vector<SliceInfo> slices;                      // parallel to lslices
    double ext_width_avg_mm = 0;                        // average external perimeter width of the layer's regions;
                                                        // auto α: w = 0.5·this of layer n−1 (TreeSupport.cpp:259-266)
};

// One model part (ModelVolume::is_model_part()) with its support paint.
struct VolumeInput {
    ObjectID volume_id;
    std::shared_ptr<const TriangleMesh> mesh;           // ModelVolume::mesh_ptr(), shared, immutable
    Transform3d trafo { Transform3d::Identity() };      // mesh → PO frame (mm): po.trafo_centered() * mv->get_matrix()
    TriangleSelector::TriangleSplittingData paint;      // supported_facets.get_data()
    uint64_t paint_timestamp = 0;                       // supported_facets.timestamp()
};

// One support enforcer / blocker volume, sliced in the worker (slice_modifiers()).
struct ModifierInput {
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d trafo { Transform3d::Identity() };      // mesh → PO frame (mm)
    bool enforcer = false;                              // true: SUPPORT_ENFORCER, false: SUPPORT_BLOCKER
};

// Self-contained copy of everything the worker needs.
struct AnalysisInput {                                  // immutable after build; shared read-only by all threads
    ObjectID model_object_id;
    size_t analysed_instance_idx = 0;                   // index into the live ModelObject::instances
    Transform3d po_trafo_centered { Transform3d::Identity() };   // marker: world = live_instance · po_trafo_centered⁻¹ · p
    Vec2d instance_shift_mm { Vec2d::Zero() };          // bed-local: bed XY = unscale(p) + instance_shift_mm
    std::vector<LayerInput> layers;                     // object layers, bottom up
    std::vector<VolumeInput> volumes;                   // model parts in ModelObject::volumes order
    std::vector<ModifierInput> modifiers;               // support enforcer / blocker volumes
    int num_print_objects = 1;                          // PrintObjects of this ModelObject in the print (rotated instances)
    bool other_objects_on_bed = false;                  // other ModelObjects are in the print (not considered)
    std::vector<ContractIssue> contract;                // check_contract(po->config(), policy, &params) — empty until WP2c0
    Params params;
};

// A support zone planned at one layer.
struct Placement {
    uint32_t id = 0;                                    // unique per run, in plan order
    Cause cause = Cause::Island;
    Level level = Level::Must;
    uint32_t layer = 0;                                 // object layer index
    ExPolygons area;                                    // zone to paint (projected to the downward facets above it)
    bool on_model = false;                              // tips stand on the model (OnModelFallback)
    bool paintable = true;                              // paintable = false: LongSpan (v1)
};

enum class ReportState : uint8_t {
    Unplaceable, BlockedByUser, WillBeDropped, RemainingAfterPaint, OnModelFallback, OnModelTooShort,
    MayMergeOntoModel,                                                          // ← problems
    PartlyOnModel, BelowLowestLevel, LongSpanNotPainted, CauseDisabled, ReliesOnDisabledCauses,
    TailWithoutDownwardFacets, SeveralPrintObjects, OtherObjectsNotConsidered,
    HangsFromUnplacedIsland };                                                  // ← notes
// HangsFromUnplacedIsland: material that hangs from an island start reported as a problem (Unplaceable, BlockedByUser):
// folded into one note per start instead of a cascade of island needs (theory §3.1). Its own row, not the island's state.
// Known reason why a need could not be placed (report tooltip).
enum class Why : uint8_t { None, NotReachable, NoProgress, InterfaceGap, NoTipLayer };

// One (merged) report row.
struct ReportEntry {
    ReportState state = ReportState::Unplaceable;
    Why why = Why::None;
    Cause cause = Cause::Island;
    Level level = Level::Must;
    uint32_t layer_first = 0, layer_last = 0;           // object layer indices
    double z_first = 0, z_last = 0;                     // print_z, mm
    Point centroid;                                     // PO frame, scaled
    double slice_z = 0;                                 // of the centroid's layer (marker position)
    double area_mm2 = 0;
    int count = 1;                                      // merged pieces
};
// Report group of a row (§3.11): the problems are the states up to MayMergeOntoModel, except BlockedByUser, which is
// a problem only for islands (a blocked island fails; a blocked overhang is the user's choice). Shared by build_report
// and the GUI.
inline bool is_problem(const ReportEntry &e)
{
    if (e.state == ReportState::BlockedByUser)
        return e.cause == Cause::Island;
    return uint8_t(e.state) <= uint8_t(ReportState::MayMergeOntoModel);
}


// Paint result of one VolumeInput.
struct VolumePaint { ObjectID volume_id; TriangleSelector::TriangleSplittingData data; bool changed = false; };
struct LayerStats  { float max_c = 0; float need_area[3][3] = {}; };   // [cause][level] mm²

struct Result {
    std::vector<Placement> placements;
    std::vector<VolumePaint> paint;                     // paint.size() == in.volumes.size(), volume order
    std::vector<ReportEntry> report;                    // §3.11 order, ≤ 50 per group + counts
    int report_more[2] = {};                            // hidden entries per group [problems, notes]
    std::vector<LayerStats> layer_stats;                // only with collect_stats (then size layers), else empty
    uint64_t fingerprint = 0;
    int painted_areas[3] = {};                          // painted placements per Cause
};

struct RunOptions {
    bool paint = true;                                  // false: plan and report only, paint = unchanged volumes
    bool collect_stats = false;
    int verification = -1;                              // −1: Params::verification, 0: off, 1: on
    std::function<void(std::vector<Placement>&)> test_mutate_before_paint;   // tests (#22)
};

using ThrowOnCancel = std::function<void()>;            // throws CanceledException
using IsCanceled    = std::function<bool()>;            // thread-safe predicate (prefetch thread may call it)
using ProgressFn    = std::function<void(int)>;         // percent 0..100, monotone

// ---- GUI thread (WP0 impl) — all read live Print/Model; never call from a worker

// derive_params mirrors the TreeSupport formulas directly from po.config() / print config / regions; it
// never constructs TreeSupportMeshGroupSettings (asserts when support is off or not tree/organic, review 7 m9).
// Valid for any support configuration (support off, grid style): values then come from the config as is.
// The soluble adjustment of TreeSupportSettings (contact distance 0) is applied for this object only.
// Returns up in Params::user and the constants at their defaults.
Params derive_params(const PrintObject &po, const UserParams &up);

struct PrintObjectChoice {
    const PrintObject *po = nullptr;                    // nullptr: see why_null
    size_t analysed_instance = 0;                       // index into live_mo.instances
    bool fallback = false;                              // fallback: not the selected instance's PrintObject
    bool ready = false;                                 // po->is_step_done(posSlice)
    enum class Null : uint8_t { None, NoPrintableInstance, NotInPrint } why_null = Null::None;
};
// Rules of plan §6.7. Call again on every poll; never cache the PrintObject pointer (Print::apply may delete it).
PrintObjectChoice choose_print_object(const Print &active, const ModelObject &live_mo,
                                      size_t selected_instance, bool background_processing);
// Plan §2.4: po is sliced, the Print copy of the analysed instance has the live matrix without offset and the
// live z offset (bed-shift safe), and all volumes match the live ones (count, ids, types, meshes, matrices).
bool          input_consistent(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance);
// Copies the layers of po, the model parts (with their paint) and support volumes of live_mo and derive_params().
// precondition: input_consistent; copies only (no slicing), O(layers + volumes).
AnalysisInput build_input(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance,
                          const UserParams &up);

// ---- WP2c0 (stubs: empty / 0 / no-op)
// derived == nullptr (GUI, before slicing): config keys only; with derived params also ReachBelowTipSpacing.
std::vector<ContractIssue> check_contract(const PrintObjectConfig &cfg, PlatePolicy policy, const Params *derived = nullptr);
// include_advisory = false: pre-run "Fix…"; true: the after-run InterfaceGap "Fix…" (adds Top interface layers = 0)
std::vector<std::pair<std::string, std::string>> recommended_changes(const PrintObjectConfig &cfg, PlatePolicy policy,
                                                                     bool include_advisory); // (option key, new value)
// Writes recommended_changes(effective, …) into the object's config (object-level overrides).
void     apply_recommended(ModelConfig &object_config, const PrintObjectConfig &effective, PlatePolicy policy,
                           bool include_advisory);
uint64_t config_fingerprint(const DynamicPrintConfig &effective, const ModelObject &live_mo, size_t instance,
                            const UserParams &up);      // §6.8 keys

// ---- worker (WP2c1b; stub: paint = unchanged volumes, changed = false, rest empty).
// Builds cancel = [&]{ if (is_canceled()) throw CanceledException(); } internally; does parallel_invoke,
// start_prefetch, make_plan, paint, verify, build_report (§2.2). Throws CanceledException when canceled.
Result run(const AnalysisInput &in, const RunOptions &opt, const IsCanceled &is_canceled, const ProgressFn &progress);

} // namespace SmartSupports
} // namespace Slic3r

#endif // slic3r_SmartSupports_hpp_
