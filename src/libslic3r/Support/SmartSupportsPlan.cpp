// Smart support painting: joint plan (stages, reasons, rows) and verification (plan §3.9, §3.10, theory §5.2, §5.3,
// §6, §7; WP2c1b).
//
// make_plan() walks the layers bottom up. Per layer the painted zones E start with the manual enforcers; the needs of
// all causes are placed in the order islands → cantilevers → long spans, independent of the cause toggles (P3):
// - stages per iteration: plate part of the need zone (rows across the cut intervals for long spans), widened zone
//   within C' ∩ (need ⊕ K_max), must points on the model (prefer-plate only); each stage falls through when it finds
//   nothing or makes no progress;
// - a trial is evaluated on the union E ∪ trial (D10, review 7 M2) and accepted only if the needs of its cause fall by
//   at least A_min (and, with roofs, no contact loss brings back needs of other causes, theory §5.2); otherwise the
//   evaluator is restored and the trial leaves no trace;
// - blockers keep paint tip_d + extra_outset (+ e, the paint inflation) away (P5);
// - everything that is not placed, and every note about a placement, goes into Plan::residual (the report's only
//   per-area source).
// verify() projects the actual paint and re-evaluates only when its contact falls short of the planned one (D13).
//
// Determinism (plan §3.12): the plan loop is serial; the verification's per-layer deficits go into indexed slots.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

namespace {

// Residual pieces below this area are boolean slivers of the reason split (blocker, known why), mm².
constexpr double SliverMm2      = 0.01;
// Long-span rows: length of a row square along the span and the closing that joins the squares of neighbouring
// scanlines, mm (plan §3.9 rows()).
constexpr double RowLengthMm    = 1.0;
constexpr double RowClosingMm   = 0.5;
// Sample step along a cut interval when searching the row position, mm.
constexpr double RowSampleMm    = 0.05;
// Verification: needs within this distance of a reported plan residual of the same layer are not new, mm (§3.10).
constexpr double VerifyMarginMm = 0.5;
// MayMergeOntoModel heuristic: a painted plate placement within branch_diameter + Δz·tan(tree angle) of a painted
// on-model placement at most this far below it, mm.
constexpr double MergeHeightMm  = 10.0;

inline double mm2(double scaled_area) { return scaled_area * SCALING_FACTOR * SCALING_FACTOR; }
inline double mm2(const ExPolygons &ex) { return mm2(area(ex)); }
inline double mm2(const Polygons &pp) { return mm2(area(pp)); }

// a ∩ b ≠ ∅ with a bbox prefilter.
bool intersects(const ExPolygons &a, const ExPolygons &b)
{
    if (a.empty() || b.empty() || ! get_extents(a).overlap(get_extents(b)))
        return false;
    return ! intersection(a, b).empty();
}

ExPolygons united(const ExPolygons &a, const ExPolygons &b)
{
    if (a.empty())
        return b.size() > 1 ? union_ex(b) : b;
    if (b.empty())
        return a;
    return union_ex(a, b);
}

// Noise filter of the evaluator (D21): opening ε/2, components below A_min dropped.
ExPolygons noise_filter(const ExPolygons &src, const Params &p)
{
    if (src.empty())
        return {};
    ExPolygons   out   = opening_ex(src, scaled<float>(0.5 * p.eps));
    const double a_min = p.A_min / (SCALING_FACTOR * SCALING_FACTOR);
    out.erase(std::remove_if(out.begin(), out.end(), [a_min](const ExPolygon &ex) { return ex.area() < a_min; }), out.end());
    return out;
}

// Union of the needs of a cause at the levels ≥ lowest.
ExPolygons needs_of(const LayerNeeds &L, Cause cause, Level lowest)
{
    ExPolygons out;
    int        parts = 0;
    for (int l = int(lowest); l < 3; ++ l)
        if (! L.need[int(cause)][l].empty()) {
            append(out, L.need[int(cause)][l]);
            ++ parts;
        }
    return parts > 1 ? union_ex(out) : out;
}

bool any_need(const LayerNeeds &L)
{
    for (int c = 0; c < 3; ++ c)
        for (int l = 0; l < 3; ++ l)
            if (! L.need[c][l].empty())
                return true;
    return false;
}

// Cell lookup in a raster of WP2b1 (cell (ix, iy) ↔ m[iy·cols + ix], centre origin + (i + ½)·res).
bool grid_at(const Grid &g, const Vec2d &q)
{
    if (g.res <= 0 || g.cols <= 0 || g.rows <= 0)
        return false;
    const double fx = (q.x() - double(g.origin.x())) / double(g.res);
    const double fy = (q.y() - double(g.origin.y())) / double(g.res);
    if (fx < 0. || fy < 0.)
        return false;
    const int ix = int(fx), iy = int(fy);
    return ix < g.cols && iy < g.rows && g.m[size_t(iy) * size_t(g.cols) + size_t(ix)] != 0;
}

coord_t grid_res(const BoundingBox &bb, const Params &p)
{
    const double a   = double(bb.size().x()) * double(bb.size().y());
    const double res = std::max(scaled<double>(p.grid_res_max), std::sqrt(a / double(std::max<size_t>(p.grid_max_pixels, 1))));
    return std::max<coord_t>(coord_t(res), 1);
}

// Rectangle centred at c: half_len along dir, half_width across.
Polygon oriented_rect(const Vec2d &c, const Vec2d &dir, double half_len, double half_width)
{
    const Vec2d u = dir * half_len, v = Vec2d(- dir.y(), dir.x()) * half_width;
    Polygon     out;
    for (const Vec2d &q : { Vec2d(c - u - v), Vec2d(c + u - v), Vec2d(c + u + v), Vec2d(c - u + v) })
        out.points.emplace_back(coord_t(std::llround(q.x())), coord_t(std::llround(q.y())));
    return out;
}

struct Planner
{
    Planner(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const ThrowOnCancel &cancel);
    Plan run(const ProgressFn &progress);

    // ---- one layer
    void plan_layer(uint32_t layer);
    void manual_drops();
    void place_cause(Cause cause);
    std::vector<Placement> stage_plate(Cause cause, const ExPolygons &needs, ExPolygons &plate);
    std::vector<Placement> stage_widen(Cause cause, const ExPolygons &needs, const ExPolygons &plate);
    std::vector<Placement> stage_model(Cause cause);
    ExPolygons rows(const ExPolygons &needs, bool plate_positions);
    void accept(std::vector<Placement> &&accepted);
    void residual(Cause cause, const ExPolygons &tried, const std::array<ExPolygons, 5> &why, bool roof_rejected, bool exhausted);
    bool contact_lost(const ExPolygons &E_trial) const;

    // ---- helpers
    ExPolygons plate_of(const ExPolygons &Z) const;
    ExPolygons minus_blk(const ExPolygons &Z) const { return blk.empty() || Z.empty() ? Z : diff_ex(Z, blk); }
    ExPolygons blocker_zone(uint32_t layer) const;
    Level      level_near(Cause cause, const ExPolygons &piece, coord_t radius) const;
    Level      max_level(Cause cause) const;
    std::vector<Placement> as_placements(Cause cause, const ExPolygons &area, coord_t radius) const;
    std::optional<BoundingBox> touched_of(const ExPolygons &added) const;
    ExPolygons add_need(Cause cause, Level level, Why why, ReportState reason, ExPolygons area);
    void       mark_islands(const ExPolygons &pieces, ReportState reason, Why why);
    bool       painted(const Placement &pl) const;

    // ---- after the loop
    void tails_without_downward();
    void may_merge();

    const AnalysisInput &in;
    const BaseModel     &base;
    Reachability        &reach;
    const ThrowOnCancel  cancel;              // never empty
    const Params        &p;
    const UserParams    &up;
    const size_t         N;
    const Level          lowest;
    const double         a_min;               // A_min, scaled²
    coord_t              clearance    = 0;    // blocker clearance tip_d + extra_outset + e
    coord_t              touch_margin = 0;    // contact change around a trial at interface 0 (Reachability window + G)
    int                  k_d          = 1;    // paint prism reach in layers (D17)
    LayerEvaluator       ev;
    Plan                 plan;
    uint32_t             next_id      = 0;

    // Island trees with an unplaced need (folding): the layer, reason and why of their first residual.
    struct TreeMark { bool marked = false; uint32_t layer = 0; ReportState reason = ReportState::Unplaceable; Why why = Why::None; };
    std::vector<TreeMark> trees;

    // ---- state of the current layer
    uint32_t                                  n = 0;
    ExPolygons                                E;          // painted zones: manual + accepted
    std::vector<std::pair<ExPolygons, Cause>> overrides;
    LayerNeeds                                L;          // evaluation of (E, overrides)
    ExPolygons                                blk;        // blocker zone (projections ⊕ clearance)
    std::vector<size_t>                       layer_placements;
    std::vector<std::pair<ExPolygons, std::pair<ReportState, Why>>> island_residual;   // island pieces reported at n
};

Planner::Planner(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const ThrowOnCancel &cancel_in) :
    in(in), base(base), reach(reach), cancel(cancel_in ? cancel_in : ThrowOnCancel([]() {})), p(in.params), up(in.params.user),
    N(std::min(in.layers.size(), base.layers.size())), lowest(in.params.user.lowest_level),
    a_min(in.params.A_min / (SCALING_FACTOR * SCALING_FACTOR)), ev(in, base, reach, this->cancel)
{
    const double tip_r        = 0.5 * std::clamp(p.tip_diameter, 0., std::max(p.branch_diameter, 0.));
    const double extra_outset = std::max(0., tip_r - 0.5 * p.support_line_width) + (p.min_xy_dist ? 0.5 * p.support_line_width : 0.);
    clearance    = scaled<coord_t>(std::max(0., p.tip_diameter) + extra_outset + std::max(0., p.edge_limit));
    touch_margin = scaled<coord_t>(std::max(0., p.tip_diameter) + extra_outset + tip_r + std::max(p.xy_distance, p.xy_min_distance) + 1. +
                                   std::max(0., p.xy_min_distance));
    double h_min = std::numeric_limits<double>::max();
    for (const LayerInput &layer : in.layers)
        if (layer.height > 0.)
            h_min = std::min(h_min, layer.height);
    if (h_min < std::numeric_limits<double>::max() && p.edge_limit > 0.)
        k_d = std::max(1, int(std::ceil(p.edge_limit / h_min - 1e-9)));
    trees.assign(base.trees.size(), TreeMark());
    plan.stats.resize(in.layers.size());
    plan.zones_paintable.resize(in.layers.size());
    plan.zones_unpainted.resize(in.layers.size());
}

// The placement becomes paint in this run (the only place where the plan reads the toggles, P3).
bool Planner::painted(const Placement &pl) const
{
    return pl.paintable && (up.paint_causes & cause_bit(pl.cause) & (cbIsland | cbCantilever)) != 0 && pl.level >= lowest;
}

// Blocker projections of the layers whose paint prisms reach this layer (|j − n| ≤ k_d), grown by the clearance: the
// slicer grows enforced areas by tip_d (plus the tip outset) without clipping, and the paint may exceed a zone by e.
ExPolygons Planner::blocker_zone(uint32_t layer) const
{
    ExPolygons   src;
    const size_t lo = layer > uint32_t(k_d) ? size_t(layer) - size_t(k_d) : 0;
    const size_t hi = std::min(N, size_t(layer) + size_t(k_d) + 1);
    for (size_t j = lo; j < hi; ++ j)
        append(src, base.layers[j].manual_blk);
    if (src.empty())
        return {};
    return clearance > 0 ? offset_ex(union_ex(src), float(clearance)) : union_ex(src);
}

// plate_part(Z, E) (theory §5.2); with Params::test_assume_reachable every contact counts (tests, #11).
ExPolygons Planner::plate_of(const ExPolygons &Z) const
{
    if (Z.empty())
        return {};
    if (! p.test_assume_reachable)
        return reach.plate_part(Z, E, n);
    const Reachability::Contact c = reach.contact(E.empty() ? Z : union_ex(E, Z), n);
    return c.all.empty() ? ExPolygons() : intersection_ex(Z, reach.cover(c.all, n));
}

// Contact change of a trial: with interface layers 0 the contact is local to the added zone (superadditive, P4); with
// roofs a merged component may move its contact anywhere (review 7 M2): all clusters.
std::optional<BoundingBox> Planner::touched_of(const ExPolygons &added) const
{
    if (p.interface_layers > 0 || added.empty())
        return std::nullopt;
    return get_extents(added).inflated(double(touch_margin));
}

// Highest need level of the cause whose pieces meet piece ⊕ radius; the highest level of the cause otherwise.
Level Planner::level_near(Cause cause, const ExPolygons &piece, coord_t radius) const
{
    if (! piece.empty()) {
        const BoundingBox bb    = get_extents(piece).inflated(double(radius) + scaled<double>(0.01));
        const Polygons    grown = radius > 0 ? offset(piece, float(radius)) : to_polygons(piece);
        for (int l = 2; l >= int(lowest); -- l)
            for (const ExPolygon &ex : L.need[int(cause)][l])
                if (get_extents(ex.contour).overlap(bb) && ! intersection(grown, to_polygons(ex)).empty())
                    return Level(l);
    }
    return this->max_level(cause);
}

Level Planner::max_level(Cause cause) const
{
    for (int l = 2; l > int(lowest); -- l)
        if (! L.need[int(cause)][l].empty())
            return Level(l);
    return lowest;
}

// One placement per component of area (≥ A_min), level from the needs within radius.
std::vector<Placement> Planner::as_placements(Cause cause, const ExPolygons &area, coord_t radius) const
{
    std::vector<Placement> out;
    for (const ExPolygon &ex : area)
        if (ex.area() >= a_min) {
            Placement &pl = out.emplace_back();
            pl.cause      = cause;
            pl.area       = { ex };
            pl.level      = this->level_near(cause, pl.area, radius);
            pl.paintable  = cause != Cause::LongSpan;
        }
    return out;
}

ExPolygons Planner::add_need(Cause cause, Level level, Why why, ReportState reason, ExPolygons area)
{
    const double sliver = SliverMm2 / (SCALING_FACTOR * SCALING_FACTOR);
    area.erase(std::remove_if(area.begin(), area.end(), [sliver](const ExPolygon &ex) { return ex.area() < sliver; }), area.end());
    if (area.empty())
        return {};
    Need &need  = plan.residual.emplace_back();
    need.cause  = cause;
    need.level  = level;
    need.why    = why;
    need.layer  = n;
    need.area   = area;
    need.reason = reason;
    return area;
}

// The island trees of residual island pieces are unplaced: the evaluator folds the needs hanging from them into one
// entry per tree (WP2c1a: the tree of the slice containing the piece).
void Planner::mark_islands(const ExPolygons &pieces, ReportState reason, Why why)
{
    const LayerGeom  &lg     = base.layers[n];
    const ExPolygons &slices = in.layers[n].lslices;
    for (const ExPolygon &piece : pieces) {
        const BoundingBox pb = get_extents(piece.contour);
        for (size_t i = 0; i < slices.size() && i < lg.slice_tree.size(); ++ i) {
            const int32_t t = lg.slice_tree[i];
            if (t < 0 || size_t(t) >= trees.size() || trees[size_t(t)].marked || ! in.layers[n].slices[i].bbox.overlap(pb))
                continue;
            if (! intersection(ExPolygons{ piece }, ExPolygons{ slices[i] }).empty()) {
                trees[size_t(t)] = { true, n, reason, why };
                ev.mark_tree_unplaced(t);
            }
        }
    }
}

// Manual enforcers the slicer will drop (P5, theory §5.3): the painted overhang that the contact of the manual zones
// (plus the gap band) does not cover.
void Planner::manual_drops()
{
    if (E.empty() || n == 0)
        return;
    const BoundingBox bb = get_extents(E).inflated(scaled<double>(0.01));
    const ExPolygons  O  = noise_filter(diff_ex(intersection_ex(E, ClipperUtils::clip_clipper_polygons_with_subject_bbox(in.layers[n].lslices, bb)),
                                                ClipperUtils::clip_clipper_polygons_with_subject_bbox(in.layers[n - 1].lslices, bb)), p);
    if (O.empty())
        return;
    if (! reach.tips_possible(n)) {
        this->add_need(Cause::Cantilever, Level::Must, Why::NoTipLayer, ReportState::WillBeDropped, O);
        return;
    }
    const Reachability::Contact c = reach.contact(E, n);
    ExPolygons dropped = noise_filter(c.all.empty() ? O : diff_ex(O, reach.cover(c.all, n)), p);
    if (dropped.empty())
        return;
    if (c.roof_gap && ! c.tip_path_all.empty()) {
        const ExPolygons tip_cover = reach.cover(c.tip_path_all, n);
        this->add_need(Cause::Cantilever, Level::Must, Why::InterfaceGap, ReportState::WillBeDropped,
                       noise_filter(intersection_ex(dropped, tip_cover), p));
        dropped = noise_filter(diff_ex(dropped, tip_cover), p);
    }
    this->add_need(Cause::Cantilever, Level::Must, Why::NotReachable, ReportState::WillBeDropped, std::move(dropped));
}

// rows(cuts, needs) (plan §3.9): per cut whose interval meets the needs, the position in [lo, hi] nearest the interval
// centre that lies in plate_part(C', E) (plate_positions), else the centre; squares (scanline spacing × 1 mm) closed by
// 0.5 mm, inside C'.
ExPolygons Planner::rows(const ExPolygons &needs, bool plate_positions)
{
    if (L.cuts.empty() || needs.empty())
        return {};
    const BoundingBox nb = get_extents(needs).inflated(scaled<double>(0.2));
    Grid gn = make_grid(nb, grid_res(nb, p));
    rasterize(needs, gn, 1, true);
    auto pos = [](const TensionMap::Cut &cut, double s_mm) { return Vec2d(cut.p.cast<double>() + cut.dir * scaled<double>(s_mm)); };

    std::vector<const TensionMap::Cut*> selected;
    BoundingBox                         cuts_bb;
    for (const TensionMap::Cut &cut : L.cuts) {
        if (! (cut.hi >= cut.lo))
            continue;
        const int k = std::max(1, int(std::ceil((cut.hi - cut.lo) / RowSampleMm)));
        for (int i = 0; i <= k; ++ i)
            if (grid_at(gn, pos(cut, cut.lo + (cut.hi - cut.lo) * double(i) / double(k)))) {
                selected.push_back(&cut);
                for (double s : { cut.lo, cut.hi }) {
                    const Vec2d q = pos(cut, s);
                    cuts_bb.merge(Point(coord_t(std::llround(q.x())), coord_t(std::llround(q.y()))));
                }
                break;
            }
    }
    if (selected.empty())
        return {};

    Grid gp;
    if (plate_positions) {
        const BoundingBox wb = cuts_bb.inflated(scaled<double>(RowLengthMm));
        const ExPolygons  cp = this->minus_blk(intersection_ex(L.cprime, Polygons{ wb.polygon() }));
        if (const ExPolygons pp = this->plate_of(cp); ! pp.empty()) {
            gp = make_grid(wb, grid_res(wb, p));
            rasterize(pp, gp, 1, false);
        }
    }
    const double half_width = 0.5 * 1.05 * scaled<double>(std::max(0.5 * p.ext_width, 0.05));
    const double half_len   = 0.5 * scaled<double>(RowLengthMm);
    Polygons     squares;
    for (const TensionMap::Cut *cut : selected) {
        const double centre = 0.5 * (cut->lo + cut->hi);
        double       best   = centre;
        if (gp.cols > 0) {
            const int k = int(std::ceil(0.5 * (cut->hi - cut->lo) / RowSampleMm));
            for (int i = 0; i <= k; ++ i) {
                const double d = double(i) * RowSampleMm;
                if (centre - d >= cut->lo - 1e-9 && grid_at(gp, pos(*cut, centre - d))) { best = centre - d; break; }
                if (centre + d <= cut->hi + 1e-9 && grid_at(gp, pos(*cut, centre + d))) { best = centre + d; break; }
            }
        }
        squares.push_back(oriented_rect(pos(*cut, best), cut->dir, half_len, half_width));
    }
    const ExPolygons closed = closing_ex(union_ex(squares), scaled<float>(RowClosingMm));
    return intersection_ex(closed, L.cprime);
}

// Stage 1: the plate part of the need zone (rows for long spans), one placement per need component (level = its max).
std::vector<Placement> Planner::stage_plate(Cause cause, const ExPolygons &needs, ExPolygons &plate)
{
    const ExPolygons Z = this->minus_blk(cause == Cause::LongSpan ? this->rows(needs, true) : needs);
    if (Z.empty())
        return {};
    plate = this->plate_of(Z);
    if (plate.empty())
        return {};
    if (cause == Cause::LongSpan)
        return this->as_placements(cause, plate, scaled<coord_t>(RowLengthMm));
    // Assign the plate pieces to the need components (plate ⊂ Z ⊂ needs).
    std::vector<ExPolygons>  per(needs.size());
    std::vector<BoundingBox> nb;
    for (const ExPolygon &N : needs)
        nb.push_back(get_extents(N.contour).inflated(scaled<double>(0.01)));
    ExPolygons pieces = plate;
    for (ExPolygon &piece : pieces) {
        const BoundingBox pb   = get_extents(piece.contour);
        size_t            best = size_t(-1);
        double            best_area = 0.;
        for (size_t i = 0; i < needs.size(); ++ i)
            if (nb[i].overlap(pb)) {
                const double a = needs.size() == 1 ? piece.area() : area(intersection(ExPolygons{ piece }, ExPolygons{ needs[i] }));
                if (a > best_area) {
                    best_area = a;
                    best      = i;
                }
            }
        if (best != size_t(-1))
            per[best].push_back(std::move(piece));
    }
    std::vector<Placement> out;
    for (size_t i = 0; i < needs.size(); ++ i)
        if (! per[i].empty() && area(per[i]) >= a_min) {
            Placement &pl = out.emplace_back();
            pl.cause      = cause;
            pl.level      = this->level_near(cause, ExPolygons{ needs[i] }, 0);
            pl.area       = std::move(per[i]);
        }
    return out;
}

// Stage 2: need components without a plate part are widened within C' ∩ (need ⊕ K_max) (D12). Not for long spans:
// their rows already take the plate-feasible position nearest the interval centre (theory §5.2).
std::vector<Placement> Planner::stage_widen(Cause cause, const ExPolygons &needs, const ExPolygons &plate)
{
    if (cause == Cause::LongSpan || L.cprime.empty())
        return {};
    ExPolygons        Nw;
    const BoundingBox plate_bb = get_extents(plate);
    for (const ExPolygon &N : needs)
        if (plate.empty() || ! plate_bb.overlap(get_extents(N.contour)) || mm2(intersection(plate, ExPolygons{ N })) < p.A_min)
            Nw.push_back(N);
    if (Nw.empty())
        return {};
    const coord_t    K  = scaled<coord_t>(std::max(0., up.K_max));
    const ExPolygons Zw = this->minus_blk(intersection_ex(L.cprime, offset(Nw, float(K))));
    return this->as_placements(cause, this->plate_of(Zw), K);
}

// Stage 3 (prefer plate only, theory §5.3): the must points on the model, kept where the contact (plate or model)
// covers at least A_min of them. Long spans: their must-level rows at the interval centres.
std::vector<Placement> Planner::stage_model(Cause cause)
{
    if (up.plate_policy != PlatePolicy::PreferPlate)
        return {};
    const ExPolygons &must = L.need[int(cause)][int(Level::Must)];
    if (must.empty())
        return {};
    const ExPolygons M = noise_filter(this->minus_blk(cause == Cause::LongSpan ? this->rows(must, false) : must), p);
    if (M.empty())
        return {};
    const Reachability::Contact c = reach.contact(E.empty() ? M : union_ex(E, M), n);
    if (c.all.empty())
        return {};
    const ExPolygons covered = intersection_ex(M, reach.cover(c.all, n));
    if (covered.empty())
        return {};
    std::vector<Placement> out;
    for (const ExPolygon &comp : M)
        if (mm2(intersection(covered, ExPolygons{ comp })) >= p.A_min) {
            Placement &pl = out.emplace_back();
            pl.cause      = cause;
            pl.level      = Level::Must;
            pl.area       = { comp };
            pl.on_model   = true;
            pl.paintable  = cause != Cause::LongSpan;
        }
    return out;
}

// With roofs, a trial whose union turns a tip-path component into a roof can lose contact elsewhere (review 7 M2).
bool Planner::contact_lost(const ExPolygons &E_trial) const
{
    if (E.empty())
        return false;
    const ExPolygons F0 = reach.predict_F(E, n);
    if (F0.empty())
        return false;
    const ExPolygons F1 = reach.predict_F(E_trial, n);
    return mm2(diff_ex(reach.cover(F0, n), F1.empty() ? ExPolygons() : reach.cover(F1, n))) >= p.A_min;
}

// Accepted placements: ids, flags (theory §5.3, plan §3.9) and the long-span notes.
void Planner::accept(std::vector<Placement> &&accepted)
{
    for (Placement &pl : accepted) {
        pl.id    = next_id ++;
        pl.layer = n;
        if (pl.cause == Cause::LongSpan)
            this->add_need(pl.cause, pl.level, Why::None, ReportState::LongSpanNotPainted, pl.area);
        else if (pl.on_model)
            this->add_need(pl.cause, pl.level, Why::None,
                           reach.below_model_too_short(pl.area, n) ? ReportState::OnModelTooShort : ReportState::OnModelFallback, pl.area);
        else if (! p.buildplate_only && ! p.test_assume_reachable) {
            if (const Reachability::Tips t = reach.tips(pl.area, n); mm2(t.spill_model) >= p.A_min)
                this->add_need(pl.cause, pl.level, Why::None, ReportState::PartlyOnModel, pl.area);
        }
        layer_placements.push_back(plan.placements.size());
        plan.placements.push_back(std::move(pl));
    }
}

// Residual reasons (plan §3.9, review 5 m11), per piece: blocker zone → BlockedByUser; a known why (NoTipLayer,
// InterfaceGap: from the current and the trial evaluations) → Unplaceable with it; components no stage found a
// position near → NotReachable; else the progress rule failed → NoProgress (InterfaceGap if a trial was rejected for a
// roof merge that lost contact).
void Planner::residual(Cause cause, const ExPolygons &tried, const std::array<ExPolygons, 5> &why, bool roof_rejected, bool exhausted)
{
    const coord_t     K    = scaled<coord_t>(std::max(0., up.K_max)) + scaled<coord_t>(0.01);
    const Polygons    near = tried.empty() ? Polygons() : offset(tried, float(K));
    const BoundingBox nb   = get_extents(near);
    auto report = [&](Level level, Why w, ReportState reason, ExPolygons area) {
        ExPolygons kept = this->add_need(cause, level, w, reason, std::move(area));
        if (cause == Cause::Island && ! kept.empty())
            island_residual.emplace_back(std::move(kept), std::make_pair(reason, w));
    };
    for (int l = int(lowest); l < 3; ++ l) {
        ExPolygons X = L.need[int(cause)][l];
        if (X.empty())
            continue;
        const Level level = Level(l);
        if (! blk.empty() && intersects(X, blk)) {
            report(level, Why::None, ReportState::BlockedByUser, intersection_ex(X, blk));
            X = diff_ex(X, blk);
        }
        for (Why w : { Why::NoTipLayer, Why::InterfaceGap })
            if (const ExPolygons &W = why[size_t(w)]; ! W.empty() && intersects(X, W)) {
                report(level, w, ReportState::Unplaceable, intersection_ex(X, W));
                X = diff_ex(X, W);
            }
        ExPolygons no_progress, not_reachable;
        for (ExPolygon &comp : X) {
            const bool found = exhausted ||
                (! near.empty() && get_extents(comp.contour).overlap(nb) && ! intersection(ExPolygons{ comp }, near).empty());
            (found ? no_progress : not_reachable).push_back(std::move(comp));
        }
        report(level, roof_rejected ? Why::InterfaceGap : Why::NoProgress, ReportState::Unplaceable, std::move(no_progress));
        report(level, Why::NotReachable, ReportState::Unplaceable, std::move(not_reachable));
    }
}

// The placement loop of one cause at the current layer (plan §3.9).
void Planner::place_cause(Cause cause)
{
    ExPolygons                tried;
    std::array<ExPolygons, 5> why;
    auto add_why = [&why](const LayerNeeds &ln) {
        for (const auto &[pieces, w] : ln.why)
            if (! pieces.empty())
                why[size_t(w)] = united(why[size_t(w)], pieces);
    };
    add_why(L);
    bool roof_rejected = false;
    island_residual.clear();
    for (int it = 0;; ++ it) {
        const ExPolygons needs  = needs_of(L, cause, lowest);
        const double     before = area(needs);
        if (before < a_min)
            break;
        if (it >= p.max_place_iterations) {
            this->residual(cause, tried, why, roof_rejected, true);
            break;
        }
        bool       progressed = false;
        ExPolygons plate;
        for (int stage = 0; stage < 3 && ! progressed; ++ stage) {
            cancel();
            std::vector<Placement> cand = stage == 0 ? this->stage_plate(cause, needs, plate) :
                                          stage == 1 ? this->stage_widen(cause, needs, plate) : this->stage_model(cause);
            if (cand.empty())
                continue;
            ExPolygons added;
            for (const Placement &pl : cand)
                append(added, pl.area);
            append(tried, added);
            ExPolygons E2  = united(E, added);
            auto       ov2 = overrides;
            ov2.emplace_back(needs, cause);
            const std::optional<BoundingBox> touched = this->touched_of(added);
            LayerNeeds T = ev.evaluate(n, E2, ov2, touched);           // copy: the reference is invalidated below
            add_why(T);
            const double after = area(needs_of(T, cause, lowest));
            // Theory §5.2: the needs of no other cause may grow. Without roofs adding paint never removes contact (P4); other
            // causes then change only by reclassification. With roofs a merge can lose contact: such a trial is rejected.
            bool lost = false;
            if (p.interface_layers > 0)
                for (Cause other : { Cause::Island, Cause::Cantilever, Cause::LongSpan })
                    if (other != cause && area(needs_of(T, other, lowest)) - area(needs_of(L, other, lowest)) >= a_min) {
                        lost = this->contact_lost(E2);
                        break;
                    }
            if (before - after >= a_min && ! lost) {
                E         = std::move(E2);
                overrides = std::move(ov2);
                L         = std::move(T);
                this->accept(std::move(cand));
                progressed = true;
            } else {
                roof_rejected |= lost;
                ev.evaluate(n, E, overrides, touched);                 // restore the evaluator (served from its cache)
            }
        }
        if (! progressed) {
            this->residual(cause, tried, why, roof_rejected, false);
            break;
        }
    }
    if (cause == Cause::Island && ! island_residual.empty()) {
        for (const auto &[pieces, reason] : island_residual)
            this->mark_islands(pieces, reason.first, reason.second);
        island_residual.clear();
        L = ev.evaluate(n, E, overrides, std::nullopt);               // the needs hanging from them fold from here on
    }
}

void Planner::plan_layer(uint32_t layer)
{
    n = layer;
    const LayerGeom &lg = base.layers[n];
    E = lg.manual_enf;
    overrides.clear();
    layer_placements.clear();
    blk = this->blocker_zone(n);
    reach.ensure(reach.tip_layer(n), cancel);
    this->manual_drops();

    L = ev.evaluate(n, E, overrides, std::nullopt);
    plan.stats[n] = L.stats;
    if (any_need(L)) {
        for (Cause cause : { Cause::Island, Cause::Cantilever, Cause::LongSpan })
            this->place_cause(cause);
        // Needs below the lowest painted level are reported, not planned (D9).
        for (int c = 0; c < 3; ++ c)
            for (int l = 0; l < int(lowest); ++ l)
                if (! L.need[c][l].empty())
                    this->add_need(Cause(c), Level(l), Why::None, ReportState::BelowLowestLevel, L.need[c][l]);
    }
    // Folded needs of unplaced island trees: one report entry per tree (the pieces merge across the layers). The layer
    // that marked a tree has reported its pieces already.
    for (const auto &[t, area] : L.folded)
        if (t >= 0 && size_t(t) < trees.size() && trees[size_t(t)].marked && trees[size_t(t)].layer != n)
            this->add_need(Cause::Island, Level::Must, trees[size_t(t)].why, trees[size_t(t)].reason, area);
    ev.commit(n);

    ExPolygons paintable, unpainted;
    for (size_t idx : layer_placements)
        append(this->painted(plan.placements[idx]) ? paintable : unpainted, plan.placements[idx].area);
    plan.zones_paintable[n] = paintable.size() > 1 ? union_ex(paintable) : paintable;
    plan.zones_unpainted[n] = unpainted.size() > 1 ? union_ex(unpainted) : unpainted;
}

// Island trees whose tail has layers without downward facets: that part of the tail cannot be painted (theory §10
// needle). The row's marker is the tree's slice at the last tail layer.
void Planner::tails_without_downward()
{
    for (const IslandTree &tree : base.trees) {
        if (! tree.tail_without_downward || tree.tolerated)
            continue;
        const uint32_t layer = tree.tail_last > 0 ? tree.tail_last : tree.first;
        Need &need  = plan.residual.emplace_back();
        need.cause  = Cause::Island;
        need.level  = Level::Must;
        need.why    = Why::None;
        need.layer  = layer < N ? layer : 0;
        need.reason = ReportState::TailWithoutDownwardFacets;
        if (layer < N)
            for (size_t i = 0; i < base.layers[layer].slice_tree.size() && i < in.layers[layer].lslices.size(); ++ i)
                if (base.layers[layer].slice_tree[i] == tree.id)
                    need.area.push_back(in.layers[layer].lslices[i]);
    }
}

// MayMergeOntoModel (theory §5.3, v1 heuristic): a painted plate placement that a painted on-model placement at most
// MergeHeightMm below it comes within branch_diameter + Δz·tan(tree angle) of: their branches may merge and end on the
// model (TreeSupport.cpp:2057).
void Planner::may_merge()
{
    std::vector<size_t> on_model;
    for (size_t i = 0; i < plan.placements.size(); ++ i)
        if (plan.placements[i].on_model && this->painted(plan.placements[i]))
            on_model.push_back(i);
    if (on_model.empty())
        return;
    const double tan_a = std::tan(std::clamp(p.tree_angle_deg, 0., 89.) * M_PI / 180.);
    for (size_t i = 0; i < plan.placements.size(); ++ i) {
        const Placement &pl = plan.placements[i];
        if (pl.on_model || ! this->painted(pl) || pl.area.empty())
            continue;
        const BoundingBox pb = get_extents(pl.area);
        for (size_t j : on_model) {
            const Placement &q = plan.placements[j];
            if (q.layer > pl.layer || q.area.empty())
                continue;
            const double dz = in.layers[pl.layer].print_z - in.layers[q.layer].print_z;
            if (dz > MergeHeightMm)
                continue;
            const coord_t r = scaled<coord_t>(p.branch_diameter + dz * tan_a);
            if (! get_extents(q.area).inflated(double(r)).overlap(pb) || intersection(offset(q.area, float(r)), to_polygons(pl.area)).empty())
                continue;
            n = pl.layer;
            this->add_need(pl.cause, pl.level, Why::None, ReportState::MayMergeOntoModel, pl.area);
            break;
        }
    }
}

Plan Planner::run(const ProgressFn &progress)
{
    int  last_pct = -1;
    auto report   = [&](int pct) {
        if (progress && pct > last_pct) {
            last_pct = pct;
            progress(pct);
        }
    };
    report(0);
    // Above the last layer with fronts, tails or manual enforcers nothing can be a need (WP2c1a evaluate_pass).
    const size_t last = std::min(N, size_t(base.last_layer_with_fronts) + 1);
    for (size_t layer = 1; layer < last; ++ layer) {
        cancel();
        this->plan_layer(uint32_t(layer));
        report(int(100 * layer / last));
    }
    this->tails_without_downward();
    this->may_merge();
    report(100);
    return std::move(plan);
}

} // namespace

Plan make_plan(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    Planner planner(in, base, reach, cancel);
    return planner.run(progress);
}

// Verification (plan §3.10, D13): E_act = projection of the actual paint (manual enforcers and the painted causes) plus
// the enforcer volumes; deficit where cover(F_plan) − cover(F_act) ≥ A_min; only then one evaluation pass with the
// actual zones (plus the planned, unpainted ones, P3) from layer 0. Needs that the plan did not report already (residual
// of the same layer ⊕ 0.5 mm) are RemainingAfterPaint.
std::vector<Need> verify(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const Plan &plan,
                         const std::vector<VolumePaint> &paint, const ThrowOnCancel &cancel_in)
{
    const ThrowOnCancel cancel = cancel_in ? cancel_in : ThrowOnCancel([]() {});
    const size_t        N      = std::min(in.layers.size(), base.layers.size());
    if (N < 2)
        return {};
    const size_t  last = std::min(N, size_t(base.last_layer_with_fronts) + 1);
    const Params &p    = in.params;

    std::vector<const TriangleSelector::TriangleSplittingData*> per_volume(in.volumes.size(), nullptr);
    for (size_t i = 0; i < in.volumes.size() && i < paint.size(); ++ i)
        per_volume[i] = &paint[i].data;
    const uint8_t painted_bits = uint8_t(p.user.paint_causes & (cbIsland | cbCantilever));
    const std::vector<Polygons> proj = project_states(in, per_volume, [painted_bits](TriangleStateType s) {
        return s == TriangleStateType::ENFORCER || (is_auto_state(s) && (auto_mask(s) & painted_bits) != 0);
    }, cancel);

    static const ExPolygons none;
    auto at = [](const std::vector<ExPolygons> &v, size_t n) -> const ExPolygons & { return n < v.size() ? v[n] : none; };
    std::vector<ExPolygons> E_act(in.layers.size());
    std::vector<uint8_t>    deficit(in.layers.size(), 0);
    if (last > 1) {
        reach.ensure(reach.tip_layer(uint32_t(last - 1)), cancel);
        tbb::parallel_for(tbb::blocked_range<size_t>(1, last), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t n = range.begin(); n < range.end(); ++ n) {
                cancel();
                const ExPolygons &manual = base.layers[n].manual_enf;
                const ExPolygons  act    = united(united(n < proj.size() ? union_ex(proj[n]) : ExPolygons(), manual), at(plan.zones_unpainted, n));
                const ExPolygons  plan_E = united(united(manual, at(plan.zones_paintable, n)), at(plan.zones_unpainted, n));
                E_act[n] = act;
                if (plan_E.empty())
                    continue;
                const ExPolygons F_plan = reach.predict_F(plan_E, uint32_t(n));
                if (F_plan.empty())
                    continue;
                const ExPolygons F_act = act.empty() ? ExPolygons() : reach.predict_F(act, uint32_t(n));
                const ExPolygons lost  = diff_ex(reach.cover(F_plan, uint32_t(n)), F_act.empty() ? ExPolygons() : reach.cover(F_act, uint32_t(n)));
                deficit[n] = mm2(lost) >= p.A_min;
            }
        });
    }
    if (std::none_of(deficit.begin(), deficit.end(), [](uint8_t d) { return d != 0; }))
        return {};

    const PassResult        pass = evaluate_pass(in, base, reach, E_act, cancel, {});
    std::vector<ExPolygons> reported(in.layers.size());
    for (const Need &need : plan.residual)
        if (need.layer < reported.size())
            append(reported[need.layer], need.area);
    std::vector<Need> out;
    const Level       lowest = p.user.lowest_level;
    for (size_t n = 1; n < pass.layers.size(); ++ n) {
        const LayerNeeds &ln = pass.layers[n];
        Polygons          known;
        bool              known_done = false;
        for (int c = 0; c < 3; ++ c)
            for (int l = int(lowest); l < 3; ++ l) {
                if (ln.need[c][l].empty())
                    continue;
                if (! known_done) {
                    known_done = true;
                    if (! reported[n].empty())
                        known = offset(reported[n], scaled<float>(VerifyMarginMm));
                }
                ExPolygons rest = noise_filter(known.empty() ? ln.need[c][l] : diff_ex(ln.need[c][l], known), p);
                if (rest.empty())
                    continue;
                Need &need  = out.emplace_back();
                need.cause  = Cause(c);
                need.level  = Level(l);
                need.why    = Why::None;
                need.layer  = uint32_t(n);
                need.area   = std::move(rest);
                need.reason = ReportState::RemainingAfterPaint;
            }
    }
    return out;
}

} // namespace Slic3r::SmartSupports
