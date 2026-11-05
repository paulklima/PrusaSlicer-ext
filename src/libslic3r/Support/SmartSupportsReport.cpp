// Smart support painting: report, print config contract, recommended changes, fingerprint (plan §3.11, §6.4,
// §6.8; WP2c0).
//
// build_report() merges the reported pieces of the plan and of the verification into report rows, adds the
// summary notes (disabled causes, several PrintObjects, other objects), sorts the rows into the problems and the
// notes group and caps each group. check_contract() is the slicer contract P7 (theory §6), recommended_changes()
// and apply_recommended() its "Fix…" (object-level overrides), config_fingerprint() the staleness key of a run.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <tuple>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r::SmartSupports {

// ---------------------------------------------------------------------------------------------------------------
// Contract (P7, plan §6.4)

// Option that resolves a contract issue; nullptr for the info keys Raft and ReachBelowTipSpacing.
static const char* contract_option_key(ContractIssue::Key key)
{
    switch (key) {
    case ContractIssue::SupportOff:         return "support_material";
    case ContractIssue::AutoOn:             return "support_material_auto";
    case ContractIssue::NotOrganic:         return "support_material_style";
    case ContractIssue::EnforceLayers:      return "support_material_enforce_layers";
    case ContractIssue::ContactZero:        return "support_material_contact_distance";
    case ContractIssue::BuildplateMismatch: return "support_material_buildplate_only";
    case ContractIssue::InterfaceLayers:    return "support_material_interface_layers";
    default:                                return nullptr;
    }
}

std::vector<ContractIssue> check_contract(const PrintObjectConfig &cfg, PlatePolicy policy, const Params *derived)
{
    std::vector<ContractIssue> out;
    auto add = [&out, &cfg](ContractIssue::Key key, const char *recommended, bool advisory) {
        ContractIssue &issue = out.emplace_back();
        issue.key      = key;
        issue.advisory = advisory;
        if (const char *opt_key = contract_option_key(key); opt_key != nullptr) {
            issue.current     = cfg.opt_serialize(opt_key);
            issue.recommended = recommended;
        }
    };
    const bool plate_only = policy == PlatePolicy::PlateOnly;
    if (! cfg.support_material.value)
        add(ContractIssue::SupportOff, "1", false);
    if (cfg.support_material_auto.value)
        add(ContractIssue::AutoOn, "0", false);
    if (cfg.support_material_style.value != smsOrganic)
        add(ContractIssue::NotOrganic, "organic", false);
    if (cfg.support_material_enforce_layers.value > 0)
        add(ContractIssue::EnforceLayers, "0", false);
    // Zero contact distance: every bottom surface is a non-bridge (PrintObject.cpp:1061-1063).
    if (cfg.support_material_contact_distance.value < EPSILON)
        add(ContractIssue::ContactZero, "0.2", false);
    if (cfg.support_material_buildplate_only.value != plate_only)
        add(ContractIssue::BuildplateMismatch, plate_only ? "1" : "0", false);
    // Advisory (D30, D34): never in the pre-run warning or in the "Apply and paint" gate.
    if (cfg.support_material_interface_layers.value > 0)
        add(ContractIssue::InterfaceLayers, "0", true);
    if (cfg.raft_layers.value > 0)
        add(ContractIssue::Raft, "", true);
    if (derived != nullptr && derived->user.K_max < 0.5 * derived->tip_spacing)
        add(ContractIssue::ReachBelowTipSpacing, "", true);
    return out;
}

std::vector<std::pair<std::string, std::string>> recommended_changes(const PrintObjectConfig &cfg, PlatePolicy policy, bool include_advisory)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const ContractIssue &issue : check_contract(cfg, policy, nullptr))
        if (const char *opt_key = contract_option_key(issue.key); opt_key != nullptr && (include_advisory || ! issue.advisory))
            out.emplace_back(opt_key, issue.recommended);
    return out;
}

void apply_recommended(ModelConfig &object_config, const PrintObjectConfig &effective, PlatePolicy policy, bool include_advisory)
{
    // One set per change: the config is touched only if something changes.
    for (const auto &[opt_key, value] : recommended_changes(effective, policy, include_advisory))
        object_config.set_deserialize_strict(opt_key, value);
}

// ---------------------------------------------------------------------------------------------------------------
// Fingerprint (plan §6.8)

namespace {

// 64-bit FNV-1a over a canonical byte stream. Mesh pointers are hashed as values, so a fingerprint is only
// comparable within one session (D14).
class Fingerprint
{
public:
    void bytes(const void *data, size_t size)
    {
        const auto *p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++ i) {
            m_hash ^= p[i];
            m_hash *= 0x100000001b3ull;
        }
    }
    void u64(uint64_t v)           { this->bytes(&v, sizeof(v)); }
    void f64(double v)             { if (v == 0.) v = 0.; this->bytes(&v, sizeof(v)); }   // −0 hashes as +0
    void str(const std::string &s) { this->u64(s.size()); this->bytes(s.data(), s.size()); }
    void transform(const Transform3d &t)
    {
        for (int r = 0; r < 4; ++ r)
            for (int c = 0; c < 4; ++ c)
                this->f64(t.matrix()(r, c));
    }
    uint64_t value() const { return m_hash; }

private:
    uint64_t m_hash { 0xcbf29ce484222325ull };
};

} // namespace

// Options that change the analysis: the slices and the layering (build_input()), the flows and the support
// geometry (derive_params()) and the contract (check_contract()). Speeds, patterns and the like are left out, so
// changing them does not mark a result stale.
static const char* const fingerprint_keys[] = {
    "bridge_angle", "elefant_foot_compensation", "external_perimeter_extrusion_width", "extruder", "extrusion_width",
    "filament_shrinkage_compensation_xy", "filament_shrinkage_compensation_z", "first_layer_extrusion_width",
    "first_layer_height", "gcode_resolution", "layer_height", "nozzle_diameter", "perimeter_extruder",
    "perimeter_extrusion_width", "raft_contact_distance", "raft_layers", "slice_closing_radius", "slicing_mode",
    "support_material", "support_material_auto", "support_material_bottom_contact_distance",
    "support_material_bottom_interface_layers", "support_material_buildplate_only", "support_material_contact_distance",
    "support_material_enforce_layers", "support_material_extruder", "support_material_extrusion_width",
    "support_material_interface_layers", "support_material_style", "support_material_xy_spacing", "support_tree_angle",
    "support_tree_angle_slow", "support_tree_branch_diameter", "support_tree_branch_distance", "support_tree_tip_diameter",
    "support_tree_top_rate", "xy_size_compensation",
};

static void hash_config(Fingerprint &fp, const DynamicPrintConfig &cfg)
{
    for (const char *key : fingerprint_keys) {
        const ConfigOption *opt = cfg.option(key);
        fp.u64(opt != nullptr);
        if (opt != nullptr)
            fp.str(opt->serialize());
    }
}

uint64_t config_fingerprint(const DynamicPrintConfig &effective, const ModelObject &live_mo, size_t instance, const UserParams &up)
{
    Fingerprint fp;

    // User parameters; β only counts when it is used.
    fp.u64(up.paint_causes);
    fp.u64(uint64_t(int64_t(up.alpha_deg)));
    fp.f64(up.K_max);
    fp.f64(up.B_max);
    fp.f64(up.B_ceil);
    fp.f64(up.H_s);
    fp.u64(up.beta_equals_alpha);
    if (! up.beta_equals_alpha)
        fp.f64(up.beta_deg);
    fp.u64(uint64_t(up.plate_policy));
    fp.u64(uint64_t(up.lowest_level));

    // Print, object and region options of the effective config (preset + object config).
    hash_config(fp, effective);

    // Variable layer height: the profile and the layer range configs, by content.
    const std::vector<coordf_t> &profile = live_mo.layer_height_profile.get();
    fp.u64(profile.size());
    for (coordf_t v : profile)
        fp.f64(v);
    fp.u64(live_mo.layer_config_ranges.size());
    for (const auto &[range, config] : live_mo.layer_config_ranges) {
        fp.f64(range.first);
        fp.f64(range.second);
        hash_config(fp, config.get());
    }

    // The analysed instance: rotation, scaling, mirroring and z (sinking); XY is bed-local and does not matter.
    fp.u64(live_mo.id().id);
    if (instance < live_mo.instances.size()) {
        const ModelInstance &mi = *live_mo.instances[instance];
        fp.u64(1);
        fp.transform(mi.get_matrix_no_offset());
        fp.f64(mi.get_offset(Z));
    } else
        fp.u64(0);

    // All volumes: ids, types, matrices, meshes and their region overrides.
    fp.u64(live_mo.volumes.size());
    for (const ModelVolume *mv : live_mo.volumes) {
        fp.u64(mv->id().id);
        fp.u64(uint64_t(int64_t(mv->type())));
        fp.transform(mv->get_matrix());
        fp.u64(uint64_t(reinterpret_cast<uintptr_t>(mv->mesh_ptr().get())));
        hash_config(fp, mv->config.get());
    }
    return fp.value();
}

// ---------------------------------------------------------------------------------------------------------------
// Report (plan §3.11)

// Pieces merge when their bounding boxes are at most this far apart (on the same or adjacent layers).
static constexpr double report_merge_gap_mm  = 1.;
static constexpr int    report_cap_per_group = 50;

// 0 = problems, 1 = notes (is_problem(), SmartSupports.hpp).
static int report_group(const ReportEntry &e) { return is_problem(e) ? 0 : 1; }

// Summary rows (one per run or per cause) come first in the notes group, so the cap never hides them.
static bool is_summary_state(ReportState s)
{
    return s == ReportState::CauseDisabled || s == ReportState::ReliesOnDisabledCauses ||
           s == ReportState::SeveralPrintObjects || s == ReportState::OtherObjectsNotConsidered;
}

// Marker point of a piece: the area centroid if it lies inside, else its projection onto the boundary (rings).
static Point representative_point(const ExPolygon &ex)
{
    if (ex.contour.empty())
        return Point::Zero();
    const Vec2d ref = ex.contour.points.front().cast<double>();
    double      a2  = 0.;
    Vec2d       c   = Vec2d::Zero();
    for (size_t i = 0; i < ex.num_contours(); ++ i) {
        const Points &pts = ex.contour_or_hole(i).points;
        if (pts.size() < 3)
            continue;
        Vec2d p1 = pts.back().cast<double>() - ref;
        for (const Point &pt : pts) {
            const Vec2d  p2 = pt.cast<double>() - ref;
            const double cr = cross2(p1, p2);   // holes are clockwise: negative
            a2 += cr;
            c  += (p1 + p2) * cr;
            p1  = p2;
        }
    }
    if (a2 <= 0.)
        return ex.contour.points.front();
    const Point p(Vec2d(ref + c / (3. * a2)));
    return ex.contains(p) ? p : ex.point_projection(p);
}

static double area_mm2(const ExPolygon &ex) { return ex.area() * SCALING_FACTOR * SCALING_FACTOR; }

namespace {

// One component of a reported need.
struct Piece {
    ReportState state;
    Why         why;
    Cause       cause;
    Level       level;
    uint32_t    layer;
    bool        has_area;       // false: need without geometry (tail without downward facets); never merges
    BoundingBox bbox;
    double      area;           // mm²
    Point       pos;
    size_t      src;            // input order, last tie-break
};

auto merge_key(const Piece &p) { return std::make_tuple(p.state, p.why, p.cause, p.level); }

// Accumulates pieces into one row; the marker goes to the largest piece.
struct EntryAcc {
    ReportEntry entry;
    double      rep_area  = -1.;
    uint32_t    rep_layer = 0;
    bool        empty     = true;

    void add(uint32_t layer, double area, const Point &pos)
    {
        if (empty) {
            entry.layer_first = entry.layer_last = layer;
            entry.area_mm2    = 0.;
            empty             = false;
        } else {
            entry.layer_first = std::min(entry.layer_first, layer);
            entry.layer_last  = std::max(entry.layer_last, layer);
        }
        entry.area_mm2 += area;
        if (area > rep_area) {
            rep_area       = area;
            rep_layer      = layer;
            entry.centroid = pos;
        }
    }
    ReportEntry finish(const AnalysisInput &in, int count)
    {
        auto print_z = [&in](uint32_t layer) { return layer < in.layers.size() ? in.layers[layer].print_z : 0.; };
        entry.z_first = print_z(entry.layer_first);
        entry.z_last  = print_z(entry.layer_last);
        entry.slice_z = rep_layer < in.layers.size() ? in.layers[rep_layer].slice_z : 0.;
        entry.count   = count;
        return entry;
    }
};

} // namespace

// Pieces of all reported needs (plan residual first, then verification), in canonical order:
// (state, why, cause, level, layer, bbox min y, bbox min x, area, input order).
static std::vector<Piece> collect_pieces(const std::vector<Need> &residual, const std::vector<Need> &verify_residual)
{
    std::vector<Piece> pieces;
    size_t             src = 0;
    for (const std::vector<Need> *needs : { &residual, &verify_residual })
        for (const Need &need : *needs) {
            Piece piece { need.reason, need.why, need.cause, need.level, need.layer, false, BoundingBox(), 0., Point::Zero(), 0 };
            if (need.area.empty()) {
                piece.src = src ++;
                pieces.push_back(piece);
                continue;
            }
            for (const ExPolygon &ex : need.area) {
                if (ex.contour.size() < 3)
                    continue;
                piece.has_area = true;
                piece.bbox     = get_extents(ex);
                piece.area     = area_mm2(ex);
                piece.pos      = representative_point(ex);
                piece.src      = src ++;
                pieces.push_back(piece);
            }
        }
    std::sort(pieces.begin(), pieces.end(), [](const Piece &a, const Piece &b) {
        return std::make_tuple(merge_key(a), a.layer, a.bbox.min.y(), a.bbox.min.x(), a.area, a.src) <
               std::make_tuple(merge_key(b), b.layer, b.bbox.min.y(), b.bbox.min.x(), b.area, b.src);
    });
    return pieces;
}

// Merge rule (§3.11): same (state, why, cause, level), same or adjacent layers, bounding boxes at most
// report_merge_gap_mm apart; transitive. Returns the root (smallest canonical index) of each piece.
static std::vector<size_t> merge_pieces(const std::vector<Piece> &pieces)
{
    std::vector<size_t> parent(pieces.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&parent](size_t i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    auto unite = [&parent, &find](size_t a, size_t b) {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    };
    const coord_t gap  = scaled<coord_t>(report_merge_gap_mm);
    auto          near = [gap](const BoundingBox &a, const BoundingBox &b) {
        return a.min.x() <= b.max.x() + gap && b.min.x() <= a.max.x() + gap &&
               a.min.y() <= b.max.y() + gap && b.min.y() <= a.max.y() + gap;
    };
    for (size_t group_begin = 0; group_begin < pieces.size();) {
        size_t group_end = group_begin + 1;
        while (group_end < pieces.size() && merge_key(pieces[group_end]) == merge_key(pieces[group_begin]))
            ++ group_end;
        // Layer runs of the group: [run_begin, run_end) at layer l, [run_end, next_end) at layer l + 1.
        for (size_t run_begin = group_begin; run_begin < group_end;) {
            const uint32_t layer   = pieces[run_begin].layer;
            size_t         run_end = run_begin;
            while (run_end < group_end && pieces[run_end].layer == layer)
                ++ run_end;
            size_t next_end = run_end;
            while (next_end < group_end && pieces[next_end].layer == layer + 1)
                ++ next_end;
            for (size_t i = run_begin; i < run_end; ++ i)
                if (pieces[i].has_area)
                    for (size_t j = i + 1; j < next_end; ++ j)
                        if (pieces[j].has_area && near(pieces[i].bbox, pieces[j].bbox))
                            unite(i, j);
            run_begin = run_end;
        }
        group_begin = group_end;
    }
    for (size_t i = 0; i < pieces.size(); ++ i)
        parent[i] = find(i);
    return parent;
}

// Summary rows from the placements (§3.14 #7): CauseDisabled per unchecked cause that has placements this run
// would have painted, and one ReliesOnDisabledCauses for the painted placements planned after the first disabled
// one (plan order: layer, then cause, §3.9), which may lean on it. Long spans are never painted in v1 and are
// reported as LongSpanNotPainted, not as a disabled cause.
static void add_cause_summaries(const AnalysisInput &in, const Plan &plan, std::vector<ReportEntry> &out)
{
    const UserParams &up          = in.params.user;
    auto              would_paint = [&up](const Placement &p) { return p.paintable && p.level >= up.lowest_level; };
    std::optional<std::pair<uint32_t, int>> first_disabled;
    for (Cause cause : { Cause::Island, Cause::Cantilever }) {
        if ((up.paint_causes & cause_bit(cause)) != 0)
            continue;
        EntryAcc acc;
        acc.entry.state = ReportState::CauseDisabled;
        acc.entry.cause = cause;
        acc.entry.level = Level::May;
        int count = 0;
        for (const Placement &p : plan.placements)
            if (p.cause == cause && would_paint(p)) {
                ++ count;
                acc.entry.level = std::max(acc.entry.level, p.level);
                for (const ExPolygon &ex : p.area)
                    acc.add(p.layer, area_mm2(ex), representative_point(ex));
                if (const auto key = std::make_pair(p.layer, int(cause)); ! first_disabled || key < *first_disabled)
                    first_disabled = key;
            }
        if (count > 0 && ! acc.empty)
            out.push_back(acc.finish(in, count));
    }
    // v1 never paints long-span rows (paintable = false): they are planned support of a cause that is always off. The
    // plan keeps them as anchors (P3), so the placements above them rely on them like on an unchecked cause (review B m1).
    // Their own rows are LongSpanNotPainted notes; no CauseDisabled row.
    for (const Placement &p : plan.placements)
        if (p.cause == Cause::LongSpan && ! p.paintable && p.level >= up.lowest_level)
            if (const auto key = std::make_pair(p.layer, int(p.cause)); ! first_disabled || key < *first_disabled)
                first_disabled = key;
    if (! first_disabled)
        return;
    EntryAcc acc;
    acc.entry.state = ReportState::ReliesOnDisabledCauses;
    acc.entry.level = Level::May;
    int    count    = 0;
    double rep_area = -1.;
    for (const Placement &p : plan.placements)
        if ((up.paint_causes & cause_bit(p.cause)) != 0 && would_paint(p) && std::make_pair(p.layer, int(p.cause)) > *first_disabled) {
            ++ count;
            acc.entry.level = std::max(acc.entry.level, p.level);
            double area = 0.;
            for (const ExPolygon &ex : p.area) {
                const double a = area_mm2(ex);
                area += a;
                acc.add(p.layer, a, representative_point(ex));
            }
            if (area > rep_area) {
                rep_area        = area;
                acc.entry.cause = p.cause;
            }
        }
    if (count > 0 && ! acc.empty)
        out.push_back(acc.finish(in, count));
}

std::vector<ReportEntry> build_report(const AnalysisInput &in, const Plan &plan, const std::vector<Need> &verify_residual,
                                      int report_more_out[2])
{
    // 1. Merge the pieces of the needs into rows; id of a row = canonical index of its first piece.
    const std::vector<Piece>  pieces = collect_pieces(plan.residual, verify_residual);
    const std::vector<size_t> root   = merge_pieces(pieces);
    std::vector<ReportEntry>  entries;
    std::vector<size_t>       ids;
    {
        std::vector<size_t>   slot(pieces.size(), size_t(-1));
        std::vector<EntryAcc> accs;
        std::vector<int>      counts;
        for (size_t i = 0; i < pieces.size(); ++ i) {
            const Piece &piece = pieces[i];
            size_t      &s     = slot[root[i]];
            if (s == size_t(-1)) {
                // Roots are the smallest index of their row: the first piece of a row creates it.
                s = accs.size();
                EntryAcc &acc   = accs.emplace_back();
                acc.entry.state = piece.state;
                acc.entry.why   = piece.why;
                acc.entry.cause = piece.cause;
                acc.entry.level = piece.level;
                counts.push_back(0);
                ids.push_back(root[i]);
            }
            accs[s].add(piece.layer, piece.area, piece.pos);
            ++ counts[s];
        }
        entries.reserve(accs.size() + 5);
        for (size_t s = 0; s < accs.size(); ++ s)
            entries.push_back(accs[s].finish(in, counts[s]));
    }

    // 2. Summary notes. They carry no position (area 0) except the cause summaries.
    std::vector<ReportEntry> summaries;
    add_cause_summaries(in, plan, summaries);
    if (in.num_print_objects > 1) {
        ReportEntry &e = summaries.emplace_back();
        e.state    = ReportState::SeveralPrintObjects;
        e.level    = Level::May;
        e.area_mm2 = 0.;
        e.count    = in.num_print_objects;
    }
    if (in.other_objects_on_bed) {
        ReportEntry &e = summaries.emplace_back();
        e.state    = ReportState::OtherObjectsNotConsidered;
        e.level    = Level::May;
        e.area_mm2 = 0.;
    }
    for (const ReportEntry &e : summaries) {
        entries.push_back(e);
        ids.push_back(0);
    }

    // 3. Order (group, summary rows first, level desc, area desc, layer, x, y, id); cap per group.
    std::vector<size_t> order(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&entries, &ids](size_t ia, size_t ib) {
        const ReportEntry &a  = entries[ia];
        const ReportEntry &b  = entries[ib];
        const int          ga = report_group(a), gb = report_group(b);
        if (ga != gb)
            return ga < gb;
        const bool sa = is_summary_state(a.state), sb = is_summary_state(b.state);
        if (sa != sb)
            return sa;
        if (sa)
            return std::make_tuple(a.state, a.cause, ia) < std::make_tuple(b.state, b.cause, ib);
        return std::make_tuple(b.level, b.area_mm2, a.layer_first, a.centroid.x(), a.centroid.y(), ids[ia]) <
               std::make_tuple(a.level, a.area_mm2, b.layer_first, b.centroid.x(), b.centroid.y(), ids[ib]);
    });
    std::vector<ReportEntry> out;
    int                      shown[2] = { 0, 0 };
    report_more_out[0] = report_more_out[1] = 0;
    for (size_t i : order) {
        const int group = report_group(entries[i]);
        if (shown[group] < report_cap_per_group) {
            ++ shown[group];
            out.push_back(entries[i]);
        } else
            ++ report_more_out[group];
    }
    return out;
}

} // namespace Slic3r::SmartSupports
