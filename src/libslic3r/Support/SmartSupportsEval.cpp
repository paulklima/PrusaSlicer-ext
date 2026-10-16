// Smart support painting: Phase B cluster evaluator — LayerEvaluator, evaluate_pass (theory §2.2–§2.5, §3.1;
// plan §3.8, D25; WP2c1a).
//
// One layer n is evaluated for a given set of painted zones E (manual + accepted + trial):
// - contact = reach.contact(E, n), F = contact.all, F⁺ = cover(F) (support contact plus the slicer's gap band);
// - the ungrounded part of S_n (material above ungrounded material of L_{n−1}, weak-contact slices) once per layer;
// - per front cluster, band-local and in parallel (D25): grounded printable material GP, C' = C − F⁺ − T_n, anchors,
//   the reach domain D = band − GP − F⁺, one ReachField per D component seeded with the anchors (ĉ from the carry,
//   resampled every 0.1 mm, snapped to 1 µm, memoised per cluster), tension, hoop credit and classify_front;
// - per layer, serially: island needs, cause overrides, tail override, noise filter, folding of unplaced island
//   trees, the known reasons (InterfaceGap, NoTipLayer) and the statistics.
// commit(n) stores the ungrounded material UG_n and pushes all fronts of n to the ReachCarry.
//
// Determinism (plan §3.12): per-cluster results go into indexed slots and are combined in cluster order; the ĉ memo
// stores values computed at the snapped key point, so results never depend on the evaluation order or on which
// earlier evaluate() filled the memo. Cluster results are cached per layer keyed by the contact clipped to the
// cluster window, so repeated evaluations with the same contact (restore after a rejected trial) are free.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <unordered_map>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

namespace {

constexpr float INF_F = std::numeric_limits<float>::infinity();

// ĉ memo key grid: anchor samples are snapped to it before the carry is queried (plan §3.8, review 5 m4), scaled.
constexpr coord_t SnapGrid      = scaled<coord_t>(0.001);
// Material anchors are resampled at most this far apart (plus every vertex), mm.
constexpr double SampleStepMm   = 0.1;
// "Lies on" tolerance of the band-local tests (as compute_anchors / domain_edges), mm.
constexpr double TolMm          = 0.002;
// S_n within w + this of grounded material of L_{n−1} counts as grounded: the offset that defined S_n has an arc
// tolerance of 1 µm, so S_n − ((L_{n−1} − UG) ⊕ w) would otherwise leave rounding slivers along ∂S_n, mm.
constexpr double GroundTolMm    = 0.002;
// The contact is clipped to the cluster window ⊕ (G + this): cover() inside the window is then exact, mm.
constexpr double ClipMarginMm   = 0.02;
// Misses of the ĉ memo are evaluated in parallel from this many on.
constexpr size_t ParallelMisses = 512;

inline double area_mm2(double scaled_area) { return scaled_area * SCALING_FACTOR * SCALING_FACTOR; }

// Clip operand: src prepared for a boolean operation whose subject lies inside bbox. Not a crop: vertices outside
// the bbox may remain (ClipperUtils::clip_clipper_polygons_with_subject_bbox).
inline Polygons clip(const ExPolygons &src, const BoundingBox &bbox)
{
    return ClipperUtils::clip_clipper_polygons_with_subject_bbox(src, bbox);
}

// src ∩ bbox (exact); ExPolygons inside the bbox are copied, the others are intersected with it.
ExPolygons crop(const ExPolygons &src, const BoundingBox &bbox)
{
    ExPolygons out;
    ExPolygons partial;
    for (const ExPolygon &ex : src) {
        const BoundingBox bb = get_extents(ex.contour);
        if (! bb.overlap(bbox))
            continue;
        if (bbox.contains(bb))
            out.push_back(ex);
        else
            partial.push_back(ex);
    }
    if (! partial.empty())
        append(out, intersection_ex(partial, Polygons{ bbox.polygon() }));
    return out;
}

bool bbox_overlaps(const ExPolygons &a, const BoundingBox &bbox)
{
    for (const ExPolygon &ex : a)
        if (get_extents(ex.contour).overlap(bbox))
            return true;
    return false;
}

// Appends disjoint pieces (no union needed).
void append_ex(ExPolygons &dst, const ExPolygons &src) { dst.insert(dst.end(), src.begin(), src.end()); }

coord_t snap_coord(coord_t v) { return coord_t(std::llround(double(v) / double(SnapGrid))) * SnapGrid; }
Point   snap(const Point &p) { return Point(snap_coord(p.x()), snap_coord(p.y())); }
uint64_t memo_key(const Point &snapped)
{
    return (uint64_t(uint32_t(int32_t(snapped.x() / SnapGrid))) << 32) | uint64_t(uint32_t(int32_t(snapped.y() / SnapGrid)));
}

// Samples of a polyline: every vertex plus evenly spaced points at most `step` apart; consecutive duplicates dropped.
Points resample(const Polyline &pl, double step)
{
    Points out;
    for (size_t i = 0; i + 1 < pl.points.size(); ++ i) {
        const Vec2d a = pl.points[i].cast<double>();
        const Vec2d b = pl.points[i + 1].cast<double>();
        const int   k = std::max(1, int(std::ceil((b - a).norm() / step)));
        for (int j = 0; j < k; ++ j) {
            const Vec2d p = a + (b - a) * (double(j) / double(k));
            const Point q(coord_t(std::llround(p.x())), coord_t(std::llround(p.y())));
            if (out.empty() || out.back() != q)
                out.push_back(q);
        }
    }
    if (! pl.points.empty() && (out.empty() || out.back() != pl.points.back()))
        out.push_back(pl.points.back());
    return out;
}

// Squared distance from p to the boundary of ex.
double boundary_distance2(const ExPolygon &ex, const Point &p)
{
    double best = std::numeric_limits<double>::max();
    auto   scan = [&best, &p](const Polygon &poly) {
        for (size_t i = 0; i < poly.points.size(); ++ i)
            best = std::min(best, Line(poly.points[i], poly.points[(i + 1) % poly.points.size()]).distance_to_squared(p));
    };
    scan(ex.contour);
    for (const Polygon &hole : ex.holes)
        scan(hole);
    return best;
}

// Index of the piece whose boundary carries the polyline (pieces: the components of C').
size_t piece_of(const ExPolygons &pieces, const Polyline &pl)
{
    if (pieces.size() <= 1 || pl.points.empty())
        return 0;
    const Point p = pl.points.size() >= 2 ? Point((pl.points[0] + pl.points[1]) / 2) : pl.points.front();
    size_t best   = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < pieces.size(); ++ i)
        if (const double d = boundary_distance2(pieces[i], p); d < best_d) {
            best_d = d;
            best   = i;
        }
    return best;
}

// Index of the component of D (a disjoint union, bboxes given) that contains the connected piece; −1 if none.
int component_of(const ExPolygons &D, const std::vector<BoundingBox> &bboxes, const ExPolygon &piece)
{
    const BoundingBox pb  = get_extents(piece.contour);
    const double      tol = scaled<double>(0.01);
    std::vector<int>  candidates;
    for (size_t k = 0; k < D.size(); ++ k)
        if (bboxes[k].inflated(tol).contains(pb))
            candidates.push_back(int(k));
    if (candidates.size() <= 1)
        return candidates.empty() ? -1 : candidates.front();
    for (int k : candidates)
        if (! intersection(ExPolygons{ piece }, ExPolygons{ D[size_t(k)] }).empty())
            return k;
    return candidates.front();
}

// The reach of a front whose C' lies in several D components: the field of the component containing p, else the
// lowest value of all (points on the boundary between pieces).
class PieceField final : public ReachQuery
{
public:
    struct Part { BoundingBox bbox; ExPolygons area; std::shared_ptr<const ReachQuery> field; };
    explicit PieceField(std::vector<Part> parts) : m_parts(std::move(parts)) {}
    float value(const Point &p) const override
    {
        for (const Part &part : m_parts)
            if (part.bbox.contains(p))
                for (const ExPolygon &ex : part.area)
                    if (ex.contains(p))
                        return part.field->value(p);
        float best = INF_F;
        for (const Part &part : m_parts)
            best = std::min(best, part.field->value(p));
        return best;
    }
    coord_t res() const override { return m_parts.empty() ? scaled<coord_t>(0.1) : m_parts.front().field->res(); }
    bool    direct_mode() const override { return false; }

private:
    std::vector<Part> m_parts;
};

// One field of a front: the C' pieces it covers and the (uncropped) field.
struct FieldPart { ExPolygons area; BoundingBox bbox; std::shared_ptr<const ReachField> field; };

// Per front of a cluster: what commit() needs for the carry and for UG_n.
struct FrontOut {
    bool                   grounded = false;
    int32_t                tag      = -1;          // non-grounded: island tree the material hangs from (−1 unknown)
    std::vector<FieldPart> fields;                 // grounded fronts with a field
    ExPolygons             zero;                   // covered_all ∪ hoop ∪ (C ∩ F⁺): c* = 0 for the carry
};

// Result of one cluster for one contact.
struct ClusterOut {
    ExPolygons                                  Fc;          // key: the contact clipped to the cluster window
    ExPolygons                                  need[3][3];  // [cause][level]; islands = non-grounded C'
    std::vector<std::pair<int32_t, ExPolygons>> island_src;  // (tag, C') of the non-grounded fronts
    ExPolygons                                  cprime;      // ∪ C' of all fronts
    std::vector<TensionMap::Cut>                cuts;
    float                                       max_c = 0;
    std::vector<FrontOut>                       fronts;      // parallel to Cluster::fronts
};

using Memo = std::unordered_map<uint64_t, float>;   // snapped anchor point → ĉ (lookups only, never iterated)

struct ClusterSlot {
    std::shared_ptr<const ClusterOut> cur, prev;    // the last two evaluations of this layer
    Memo                              memo;
};

// Regions tagged by island tree (−1: unknown), sorted by tag.
using Tagged = std::vector<std::pair<int32_t, ExPolygons>>;

ExPolygons union_of(const Tagged &tagged)
{
    ExPolygons all;
    for (const auto &entry : tagged)
        append_ex(all, entry.second);
    return tagged.size() > 1 ? union_ex(all) : all;
}

} // namespace

struct LayerEvaluator::Impl
{
    Impl(const AnalysisInput &in, const BaseModel &base, const Reachability &reach, const ThrowOnCancel &cancel);

    const LayerNeeds &evaluate(uint32_t n, const ExPolygons &E, const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                               const std::optional<BoundingBox> &touched);
    void commit(uint32_t n);

    void begin_layer(uint32_t n);
    std::shared_ptr<const Reachability::Contact> contact_of(const ExPolygons &E, uint32_t n);
    std::shared_ptr<const ClusterOut> eval_cluster(uint32_t n, size_t ci, ExPolygons &&Fc, Memo &memo) const;
    void       assemble(uint32_t n, const std::vector<std::pair<ExPolygons, Cause>> &overrides);
    ExPolygons cover_near(const ExPolygons &F, const BoundingBox &bbox, uint32_t n) const;
    int32_t    ungrounded_tag(const ExPolygon &C) const;

    const AnalysisInput &in;
    const BaseModel     &base;
    const Reachability  &reach;
    ThrowOnCancel        cancel;
    const Params        &params;
    coord_t              gap_band;              // G
    ReachCarry           carry;
    std::vector<uint8_t> unplaced;              // per island tree

    uint32_t   last_committed = 0;              // layer 0 is committed by construction
    // Ungrounded material UG of the last committed layer, tagged by island tree.
    Tagged     ug_prev;
    ExPolygons ug_prev_all;

    // State of the layer being evaluated.
    int64_t                  cur_n = -1;
    ExPolygons               ugs;               // ungrounded part of S_n
    Tagged                   ugs_tagged;
    std::vector<ClusterSlot> slots;
    std::deque<std::pair<ExPolygons, std::shared_ptr<const Reachability::Contact>>> contacts;   // ≤ 2, most recent first
    std::shared_ptr<const Reachability::Contact> contact;   // of the last evaluate()
    bool                     evaluated = false;
    LayerNeeds               needs;
};

LayerEvaluator::Impl::Impl(const AnalysisInput &in, const BaseModel &base, const Reachability &reach, const ThrowOnCancel &cancel) :
    in(in), base(base), reach(reach), cancel(cancel), params(in.params), gap_band(scaled<coord_t>(reach.gap_band_mm())),
    carry(in.params), unplaced(base.trees.size(), 0)
{
    // Layer 0 rests on the bed: no fronts, nothing ungrounded. Its w is part of the reach R of the layers above.
    if (! base.layers.empty())
        carry.push(0, base.layers.front().z_top, base.layers.front().w, {});
}

// cover(F) for the points inside bbox: F clipped to bbox ⊕ (G + margin) suffices (cover is local within G).
ExPolygons LayerEvaluator::Impl::cover_near(const ExPolygons &F, const BoundingBox &bbox, uint32_t n) const
{
    if (F.empty() || ! bbox.defined)
        return {};
    const ExPolygons Fn = crop(F, bbox.inflated(double(gap_band) + scaled<double>(ClipMarginMm)));
    return Fn.empty() ? ExPolygons() : reach.cover(Fn, n);
}

// Tree of the ungrounded material of S_n that touches C (lowest tree id, −1 if none is known).
int32_t LayerEvaluator::Impl::ungrounded_tag(const ExPolygon &C) const
{
    if (ugs_tagged.empty())
        return -1;
    const BoundingBox bb = get_extents(C.contour).inflated(scaled<double>(TolMm));
    Polygons          grown;
    for (const auto &[tag, area] : ugs_tagged) {
        if (tag < 0)
            continue;
        for (const ExPolygon &ex : area)
            if (get_extents(ex.contour).overlap(bb)) {
                if (grown.empty())
                    grown = offset(C, scaled<float>(TolMm));
                if (! intersection(grown, to_polygons(ex)).empty())
                    return tag;                                     // ugs_tagged is sorted by tag
            }
    }
    return -1;
}

// Ungrounded part of S_n (plan §3.8), once per layer: in W = bbox(UG_{n−1}) ⊕ (w + 1 mm),
// ((L − U) ∩ W) − ((L_{n−1} − UG_{n−1}) ⊕ w), plus (L − U) of the weak-contact slices; tagged by island tree.
void LayerEvaluator::Impl::begin_layer(uint32_t n)
{
    cur_n     = int64_t(n);
    evaluated = false;
    ugs.clear();
    ugs_tagged.clear();
    contacts.clear();
    contact.reset();
    const LayerGeom &lg = base.layers[n];
    slots.assign(lg.clusters.size(), ClusterSlot());

    const bool any_bad = std::any_of(lg.slice_bad.begin(), lg.slice_bad.end(), [](uint8_t b) { return b != 0; });
    if (ug_prev_all.empty() && ! any_bad)
        return;                                                     // fast path: everything below is grounded
    const ExPolygons &L = lg.in->lslices;
    std::map<int32_t, ExPolygons> acc;
    if (! ug_prev_all.empty() && n > 0) {
        const BoundingBox W = get_extents(ug_prev_all).inflated(double(lg.w) + scaled<double>(1.));
        Polygons fronts_W;
        for (const FrontGeom &f : lg.fronts)
            if (f.bbox.overlap(W))
                append(fronts_W, to_polygons(f.C));
        // Exact crops (clip() keeps vertices outside the box: it only prepares clip operands).
        const Polygons S_W = diff(crop(L, W), fronts_W);
        if (! S_W.empty()) {
            const Polygons   grounded = diff(crop(in.layers[n - 1].lslices, W.inflated(double(lg.w) + scaled<double>(0.01))), ug_prev_all);
            const ExPolygons ug       = grounded.empty() ? union_ex(S_W) :
                diff_ex(S_W, offset(grounded, float(lg.w + scaled<coord_t>(GroundTolMm)), ClipperLib::jtRound, scaled<double>(0.001)));
            const float reach_tol = float(lg.w + 2 * scaled<coord_t>(GroundTolMm));
            for (const ExPolygon &X : ug) {
                // The material X stands on: the lowest known tree among the touching pieces of UG_{n−1}.
                int32_t tag = -1;
                if (ug_prev.size() == 1)
                    tag = ug_prev.front().first;
                else {
                    const BoundingBox xb = get_extents(X.contour).inflated(double(reach_tol));
                    for (const auto &[t, area] : ug_prev)
                        if (t >= 0 && bbox_overlaps(area, xb) && ! intersection(ExPolygons{ X }, offset(area, reach_tol)).empty()) {
                            tag = t;
                            break;
                        }
                }
                acc[tag].push_back(X);
            }
        }
    }
    if (any_bad)
        for (size_t i = 0; i < L.size(); ++ i)
            if (lg.slice_bad[i]) {
                Polygons own_fronts;
                for (const FrontGeom &f : lg.fronts)
                    if (std::find(f.slices.begin(), f.slices.end(), int(i)) != f.slices.end())
                        append(own_fronts, to_polygons(f.C));
                const int32_t tag = i < lg.slice_tree.size() ? lg.slice_tree[i] : -1;
                append_ex(acc[tag], own_fronts.empty() ? ExPolygons{ L[i] } : diff_ex(L[i], own_fronts));
            }
    for (auto &[tag, area] : acc)
        if (ExPolygons u = union_ex(area); ! u.empty())
            ugs_tagged.emplace_back(tag, std::move(u));
    ugs = union_of(ugs_tagged);
}

std::shared_ptr<const Reachability::Contact> LayerEvaluator::Impl::contact_of(const ExPolygons &E, uint32_t n)
{
    for (auto it = contacts.begin(); it != contacts.end(); ++ it)
        if (it->first == E) {
            auto found = *it;
            contacts.erase(it);
            contacts.push_front(found);
            return found.second;
        }
    std::shared_ptr<const Reachability::Contact> c = std::make_shared<Reachability::Contact>(E.empty() ? Reachability::Contact() : reach.contact(E, n));
    contacts.emplace_front(E, c);
    while (contacts.size() > 2)
        contacts.pop_back();
    return c;
}

std::shared_ptr<const ClusterOut> LayerEvaluator::Impl::eval_cluster(uint32_t n, size_t ci, ExPolygons &&Fc, Memo &memo) const
{
    auto out = std::make_shared<ClusterOut>();
    out->Fc  = std::move(Fc);
    const LayerGeom  &lg   = base.layers[n];
    const Cluster    &cl   = lg.clusters[ci];
    const BoundingBox box  = cl.win.inflated(scaled<double>(0.01));
    const ExPolygons &Lc   = cl.band;                      // ⊇ the fronts (Phase A, offset_keep_thin)
    const ExPolygons  Fcov = out->Fc.empty() ? ExPolygons() : reach.cover(out->Fc, n);
    const Polygons    Tc   = clip(lg.tolerated, box);

    // Grounded printable material: GP = (S − ungrounded − T − unprintable) ∪ (L ∩ F⁺ − T), S = band − ∪C.
    Polygons not_gp = ugs.empty() ? Polygons() : clip(ugs, box);
    append(not_gp, Tc);
    append(not_gp, clip(lg.unprintable, box));
    for (uint32_t idx : cl.fronts)
        append(not_gp, to_polygons(lg.fronts[idx].C));
    Polygons gp = diff(Lc, not_gp);
    if (! Fcov.empty())
        append(gp, Tc.empty() ? intersection(Lc, Fcov) : diff(intersection(Lc, Fcov), Tc));
    const ExPolygons GPc = union_ex(gp);
    Polygons removed = to_polygons(Fcov);
    append(removed, Tc);
    const BoundingBox removed_bb = get_extents(removed);

    // ---- per front: C', anchors, groundedness; non-grounded fronts are island needs (theory §3.1)
    const size_t nf = cl.fronts.size();
    struct FrontWork {
        ExPolygons       cprime;
        Anchors          anchors;
        std::vector<int> piece_comp;                    // D component of each piece of C'
        std::vector<int> material_comp, support_comp;   // D component of each anchor polyline
    };
    std::vector<FrontWork> fw(nf);
    out->fronts.resize(nf);
    for (size_t i = 0; i < nf; ++ i) {
        const FrontGeom &front = lg.fronts[cl.fronts[i]];
        FrontWork       &w     = fw[i];
        FrontOut        &fo    = out->fronts[i];
        w.cprime = removed.empty() || ! removed_bb.overlap(front.bbox) ? ExPolygons{ front.C } : diff_ex(front.C, removed);
        if (w.cprime.empty())
            // Fully supported (or tolerated): grounded if it rests on support.
            fo.grounded = ! Fcov.empty() && ! intersection(ExPolygons{ front.C }, Fcov).empty();
        else {
            w.anchors   = compute_anchors(w.cprime, front.C, GPc, Fcov, Lc);
            fo.grounded = ! w.anchors.material.empty() || ! w.anchors.support.empty();
        }
        append_ex(out->cprime, w.cprime);
        if (fo.grounded && ! Fcov.empty() && removed_bb.overlap(front.bbox))
            fo.zero = intersection_ex(ExPolygons{ front.C }, Fcov);
        if (! fo.grounded && ! w.cprime.empty()) {
            for (int s : front.slices)
                if (s >= 0 && size_t(s) < lg.slice_tree.size() && lg.slice_tree[size_t(s)] >= 0) {
                    fo.tag = lg.slice_tree[size_t(s)];
                    break;
                }
            if (fo.tag < 0)
                fo.tag = this->ungrounded_tag(front.C);
            append_ex(out->need[int(Cause::Island)][int(Level::Must)], w.cprime);
            out->island_src.emplace_back(fo.tag, w.cprime);
        }
    }
    bool any_grounded = false;
    for (size_t i = 0; i < nf; ++ i)
        any_grounded |= out->fronts[i].grounded && ! fw[i].cprime.empty();
    if (! any_grounded)
        return out;

    // ---- reach domain D = band − GP − F⁺; the components holding grounded C' get a field
    ExPolygons D;
    {
        Polygons stop = to_polygons(GPc);
        append(stop, to_polygons(Fcov));
        D = diff_ex(Lc, stop);
    }
    std::vector<BoundingBox> D_bb;
    for (const ExPolygon &d : D)
        D_bb.push_back(get_extents(d.contour));
    for (size_t i = 0; i < nf; ++ i) {
        FrontWork &w = fw[i];
        if (! out->fronts[i].grounded || w.cprime.empty())
            continue;
        for (const ExPolygon &piece : w.cprime) {
            int k = component_of(D, D_bb, piece);
            if (k < 0) {
                // Not expected (C' ⊂ D up to rounding): the piece is a component of its own.
                k = int(D.size());
                D.push_back(piece);
                D_bb.push_back(get_extents(piece.contour));
            }
            w.piece_comp.push_back(k);
        }
        for (const Polyline &pl : w.anchors.material)
            w.material_comp.push_back(w.piece_comp[piece_of(w.cprime, pl)]);
        for (const Polyline &pl : w.anchors.support)
            w.support_comp.push_back(w.piece_comp[piece_of(w.cprime, pl)]);
    }

    // ---- groups (front, D component): tension and hoop credit; fully covered groups need no field (§3.8 early exit)
    struct Group { size_t f; int k; ExPolygons cprime; TensionMap tension; ExPolygons hoop; bool covered = false; };
    std::vector<Group> groups;
    for (size_t i = 0; i < nf; ++ i) {
        const FrontWork &w = fw[i];
        std::map<int, ExPolygons> by_comp;
        for (size_t p = 0; p < w.piece_comp.size(); ++ p)
            by_comp[w.piece_comp[p]].push_back(w.cprime[p]);
        for (auto &[k, pieces] : by_comp)
            groups.push_back({ i, k, std::move(pieces), {}, {}, false });
    }
    ExPolygons                 stop_ex;      // GP ∪ F⁺ for the domain edges (overlaps are fine)
    std::map<int, DomainEdges> edges;
    std::vector<uint8_t>       need_field(D.size(), 0);
    for (Group &g : groups) {
        const FrontGeom &front = lg.fronts[cl.fronts[g.f]];
        DomainEdges      e;
        if (front.wide) {
            auto it = edges.find(g.k);
            if (it == edges.end()) {
                if (stop_ex.empty()) {
                    stop_ex = GPc;
                    append_ex(stop_ex, Fcov);
                }
                it = edges.emplace(g.k, domain_edges(ExPolygons{ D[size_t(g.k)] }, stop_ex)).first;
            }
            e = it->second;
        }
        g.tension = tension_levels(g.cprime, front, ExPolygons{ D[size_t(g.k)] }, e, fw[g.f].anchors, params);
        g.hoop    = hoop_credit(g.cprime, front, true, params);
        Polygons covered = to_polygons(g.tension.covered_all);
        append(covered, to_polygons(g.hoop));
        g.covered = ! covered.empty() && diff_ex(g.cprime, covered).empty();
        if (! g.covered)
            need_field[size_t(g.k)] = 1;
        append(out->cuts, g.tension.cuts);
        FrontOut &fo = out->fronts[g.f];
        append_ex(fo.zero, g.tension.covered_all);
        append_ex(fo.zero, g.hoop);
    }

    // ---- ĉ at the material anchors of the components that need a field: snapped samples, cluster memo,
    //      misses evaluated in parallel into indexed slots and inserted serially (plan §3.8, review 6 M5b)
    struct Samples { Points pts; std::vector<uint64_t> keys; std::vector<float> c; };
    std::vector<std::vector<Samples>> samples(nf);
    std::vector<std::pair<uint64_t, Point>> misses;
    const double step = scaled<double>(SampleStepMm);
    for (size_t i = 0; i < nf; ++ i) {
        const FrontWork &w = fw[i];
        samples[i].resize(w.anchors.material.size());
        for (size_t j = 0; j < w.anchors.material.size(); ++ j) {
            if (! need_field[size_t(w.material_comp[j])])
                continue;
            Samples &s = samples[i][j];
            s.pts = resample(w.anchors.material[j], step);
            s.keys.reserve(s.pts.size());
            for (const Point &p : s.pts) {
                const Point    q   = snap(p);
                const uint64_t key = memo_key(q);
                s.keys.push_back(key);
                if (memo.find(key) == memo.end())
                    misses.emplace_back(key, q);
            }
        }
    }
    if (! misses.empty()) {
        std::sort(misses.begin(), misses.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        misses.erase(std::unique(misses.begin(), misses.end(), [](const auto &a, const auto &b) { return a.first == b.first; }), misses.end());
        std::vector<float> vals(misses.size(), 0.f);
        auto eval_range = [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++ i)
                vals[i] = carry.c_hat(misses[i].second, n, lg.z_top, lg.h, lg.w);
        };
        if (misses.size() >= ParallelMisses)
            tbb::parallel_for(tbb::blocked_range<size_t>(0, misses.size(), 256), [&](const tbb::blocked_range<size_t> &r) {
                eval_range(r.begin(), r.end());
            });
        else
            eval_range(0, misses.size());
        for (size_t i = 0; i < misses.size(); ++ i)
            memo.emplace(misses[i].first, vals[i]);
    }
    for (std::vector<Samples> &per_front : samples)
        for (Samples &s : per_front) {
            s.c.reserve(s.keys.size());
            for (uint64_t key : s.keys)
                s.c.push_back(memo.at(key));
        }

    // ---- seeds and one field per D component that needs one
    const double delta = unscale<double>(lg.w_beta - lg.w);
    std::vector<std::shared_ptr<const ReachField>> fields(D.size());
    std::vector<std::vector<float>>                max_chat(nf, std::vector<float>(D.size(), -1.f));   // −1: no seed
    for (size_t k = 0; k < D.size(); ++ k) {
        if (! need_field[k])
            continue;
        std::vector<AnchorSeg> seeds;
        ExPolygons             cprime_k;
        for (const Group &g : groups)
            if (g.k == int(k))
                append_ex(cprime_k, g.cprime);
        for (size_t i = 0; i < nf; ++ i) {
            const FrontWork &w  = fw[i];
            float           &mc = max_chat[i][k];
            for (size_t j = 0; j < w.anchors.material.size(); ++ j) {
                if (w.material_comp[j] != int(k))
                    continue;
                const Samples &s = samples[i][j];
                for (size_t p = 0; p + 1 < s.pts.size(); ++ p) {
                    const Point &a  = s.pts[p];
                    const Point &b  = s.pts[p + 1];
                    const float  ca = s.c[p];
                    const float  cb = s.c[p + 1];
                    mc = std::max({ mc, ca, cb });
                    if ((ca <= 0.f) != (cb <= 0.f)) {
                        // Between a zero and a non-zero sample: constant halves (plan §3.8, review 1 m4).
                        const Point m((a + b) / 2);
                        if (m != a)
                            seeds.push_back({ Line(a, m), ca, ca });
                        if (m != b)
                            seeds.push_back({ Line(m, b), cb, cb });
                    } else
                        seeds.push_back({ Line(a, b), ca, cb });
                }
            }
            for (size_t j = 0; j < w.anchors.support.size(); ++ j) {
                if (w.support_comp[j] != int(k))
                    continue;
                const Points &pts = w.anchors.support[j].points;
                for (size_t p = 0; p + 1 < pts.size(); ++ p)
                    if (pts[p] != pts[p + 1])
                        seeds.push_back({ Line(pts[p], pts[p + 1]), 0.f, 0.f });
                mc = std::max(mc, 0.f);
            }
        }
        fields[k] = std::make_shared<const ReachField>(cprime_k, ExPolygons{ D[k] }, std::move(seeds), delta, params, cancel);
    }

    // ---- classification
    for (Group &g : groups) {
        const std::shared_ptr<const ReachField> &field = fields[size_t(g.k)];
        if (! field)
            continue;
        out->fronts[g.f].fields.push_back({ g.cprime, get_extents(g.cprime), field });
        if (g.covered)
            continue;
        // max ĉ enables the early exit of classify_front only for a single, simply connected C' that carries the
        // anchors (WP2b2 note); +∞ disables it.
        const float mc  = max_chat[g.f][size_t(g.k)];
        const float cap = g.cprime.size() == 1 && g.cprime.front().holes.empty() && mc >= 0.f ? mc : INF_F;
        FrontNeeds  fn  = classify_front(g.cprime, *field, g.tension, g.hoop, cap, params);
        out->max_c = std::max(out->max_c, fn.max_c);
        for (int l = 0; l < 3; ++ l) {
            append_ex(out->need[int(Cause::Cantilever)][l], fn.need[0][l]);
            append_ex(out->need[int(Cause::LongSpan)][l], fn.need[1][l]);
        }
    }
    for (FrontOut &fo : out->fronts)
        if (fo.zero.size() > 1)
            fo.zero = union_ex(fo.zero);
    return out;
}

const LayerNeeds &LayerEvaluator::Impl::evaluate(uint32_t n, const ExPolygons &E, const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                                                 const std::optional<BoundingBox> &touched)
{
    if (cancel)
        cancel();
    if (n == 0 || n >= base.layers.size()) {
        needs = LayerNeeds();
        return needs;
    }
    if (cur_n != int64_t(n)) {
        assert(n == last_committed + 1);                            // n = last committed layer + 1
        this->begin_layer(n);
    }
    const LayerGeom &lg = base.layers[n];

    // The contact matters only where something is evaluated or stored.
    const bool active = ! lg.clusters.empty() || ! lg.tail_needs.empty() || ! ugs.empty() || ! lg.tolerated.empty();
    contact = active ? this->contact_of(E, n) : std::make_shared<Reachability::Contact>();
    const ExPolygons &F = contact->all;

    // Clusters to (re)compute: those whose clipped contact changed; with `touched` only clusters near the change.
    const double            clip_margin = double(gap_band) + scaled<double>(ClipMarginMm);
    std::vector<size_t>     todo;
    std::vector<ExPolygons> todo_Fc;
    for (size_t ci = 0; ci < lg.clusters.size(); ++ ci) {
        ClusterSlot      &slot = slots[ci];
        const BoundingBox win  = lg.clusters[ci].win.inflated(clip_margin);
        if (slot.cur && touched && ! (touched->defined && touched->overlap(win)))
            continue;
        ExPolygons Fc = crop(F, win);
        if (slot.cur && slot.cur->Fc == Fc)
            continue;
        if (slot.prev && slot.prev->Fc == Fc) {
            std::swap(slot.cur, slot.prev);
            continue;
        }
        todo.push_back(ci);
        todo_Fc.push_back(std::move(Fc));
    }
    std::vector<std::shared_ptr<const ClusterOut>> results(todo.size());
    auto run_one = [&](size_t i) {
        if (cancel)
            cancel();
        results[i] = this->eval_cluster(n, todo[i], std::move(todo_Fc[i]), slots[todo[i]].memo);
    };
    if (todo.size() == 1)
        run_one(0);
    else if (! todo.empty())
        tbb::parallel_for(tbb::blocked_range<size_t>(0, todo.size(), 1), [&](const tbb::blocked_range<size_t> &r) {
            for (size_t i = r.begin(); i < r.end(); ++ i)
                run_one(i);
        });
    for (size_t i = 0; i < todo.size(); ++ i) {
        ClusterSlot &slot = slots[todo[i]];
        slot.prev = std::move(slot.cur);
        slot.cur  = std::move(results[i]);
    }

    this->assemble(n, overrides);
    evaluated = true;
    return needs;
}

void LayerEvaluator::Impl::assemble(uint32_t n, const std::vector<std::pair<ExPolygons, Cause>> &overrides)
{
    const LayerGeom  &lg = base.layers[n];
    const ExPolygons &F  = contact->all;
    LayerNeeds        out;
    Tagged            island_src;
    for (const ClusterSlot &slot : slots) {
        const ClusterOut &co = *slot.cur;
        for (int c = 0; c < 3; ++ c)
            for (int l = 0; l < 3; ++ l)
                append_ex(out.need[c][l], co.need[c][l]);
        island_src.insert(island_src.end(), co.island_src.begin(), co.island_src.end());
        append_ex(out.cprime, co.cprime);
        out.cuts.insert(out.cuts.end(), co.cuts.begin(), co.cuts.end());
        out.stats.max_c = std::max(out.stats.max_c, co.max_c);
    }

    // Cause overrides (theory §2.5): needs inside the need region of an accepted placement keep its cause.
    for (const auto &[region, cause] : overrides) {
        if (region.empty())
            continue;
        const BoundingBox rb = get_extents(region);
        for (int c = 0; c < 3; ++ c) {
            if (c == int(cause))
                continue;
            for (int l = 0; l < 3; ++ l) {
                ExPolygons &src = out.need[c][l];
                if (src.empty() || ! bbox_overlaps(src, rb))
                    continue;
                ExPolygons moved = intersection_ex(src, region);
                if (moved.empty())
                    continue;
                src = diff_ex(src, region);
                ExPolygons &dst = out.need[int(cause)][l];
                append_ex(dst, moved);
                dst = union_ex(dst);
            }
        }
    }

    // Tail override (theory §3.1, review 5 B1): tail points are island musts regardless of reach, tension or hoop;
    // only support (F⁺) resolves them.
    ExPolygons tail;
    for (const auto &entry : lg.tail_needs)
        append_ex(tail, entry.second);
    if (! tail.empty()) {
        const BoundingBox tb      = get_extents(tail);
        Polygons          removed = to_polygons(this->cover_near(F, tb, n));
        append(removed, clip(lg.tolerated, tb.inflated(scaled<double>(0.01))));
        tail = removed.empty() ? union_ex(tail) : diff_ex(tail, removed);
    }
    if (! tail.empty()) {
        const BoundingBox tb = get_extents(tail);
        for (int c = 0; c < 3; ++ c)
            for (int l = 0; l < 3; ++ l)
                if (ExPolygons &src = out.need[c][l]; ! src.empty() && bbox_overlaps(src, tb))
                    src = diff_ex(src, tail);
        ExPolygons &dst = out.need[int(Cause::Island)][int(Level::Must)];
        append_ex(dst, tail);
        dst = union_ex(dst);
    }

    // Noise filter (D21): ε is a minimal width (opening ε/2) of the union of the layer's needs; components < A_min go.
    {
        Polygons all;
        for (int c = 0; c < 3; ++ c)
            for (int l = 0; l < 3; ++ l)
                append(all, to_polygons(out.need[c][l]));
        if (! all.empty()) {
            ExPolygons   kept  = opening_ex(union_ex(all), scaled<float>(0.5 * params.eps));
            const double a_min = params.A_min / (SCALING_FACTOR * SCALING_FACTOR);
            kept.erase(std::remove_if(kept.begin(), kept.end(), [a_min](const ExPolygon &ex) { return ex.area() < a_min; }), kept.end());
            for (int c = 0; c < 3; ++ c)
                for (int l = 0; l < 3; ++ l)
                    if (! out.need[c][l].empty())
                        out.need[c][l] = kept.empty() ? ExPolygons() : intersection_ex(out.need[c][l], kept);
        }
    }

    // Folding (theory §3.1 "Unplaced islands"): island needs hanging from an unplaced tree become one entry per tree.
    // Component-wise (the noise filter moved the boundaries slightly): a need component belongs to the unplaced tree
    // whose region covers most of it, if that is at least half of it.
    if (std::any_of(unplaced.begin(), unplaced.end(), [](uint8_t u) { return u != 0; })) {
        std::map<int32_t, Polygons> regions;
        auto is_unplaced = [this](int32_t t) { return t >= 0 && size_t(t) < unplaced.size() && unplaced[size_t(t)] != 0; };
        for (const auto &[tag, area] : island_src)
            if (is_unplaced(tag))
                append(regions[tag], to_polygons(area));
        for (const auto &[tree, area] : lg.tail_needs)
            if (is_unplaced(tree))
                append(regions[tree], to_polygons(area));
        if (! regions.empty()) {
            std::vector<std::pair<int32_t, BoundingBox>> region_bb;
            for (const auto &[tree, region] : regions)
                region_bb.emplace_back(tree, get_extents(region));
            std::map<int32_t, ExPolygons> folded;
            for (int l = 0; l < 3; ++ l) {
                ExPolygons &src = out.need[int(Cause::Island)][l];
                ExPolygons  keep;
                for (ExPolygon &X : src) {
                    const BoundingBox xb   = get_extents(X.contour);
                    int32_t           best = -1;
                    double            best_area = 0.;
                    for (const auto &[tree, rb] : region_bb)
                        if (rb.overlap(xb))
                            if (const double a = area(intersection(ExPolygons{ X }, regions[tree])); a > best_area) {
                                best_area = a;
                                best      = tree;
                            }
                    if (best >= 0 && best_area >= 0.5 * X.area())
                        folded[best].push_back(std::move(X));
                    else
                        keep.push_back(std::move(X));
                }
                src = std::move(keep);
            }
            for (auto &[tree, area] : folded)
                out.folded.emplace_back(tree, union_ex(area));
        }
    }

    // Known reasons: no tips below the first support layer; needs that only the roof gap blocks (D30, review 7 m8).
    ExPolygons all_needs;
    for (int c = 0; c < 3; ++ c)
        for (int l = 0; l < 3; ++ l)
            append_ex(all_needs, out.need[c][l]);
    if (! all_needs.empty()) {
        all_needs = union_ex(all_needs);
        if (! reach.tips_possible(n))
            out.why.emplace_back(all_needs, Why::NoTipLayer);
        else if (contact->roof_gap && ! contact->tip_path_all.empty()) {
            const BoundingBox nb  = get_extents(all_needs);
            ExPolygons        gap = intersection_ex(all_needs, this->cover_near(contact->tip_path_all, nb, n));
            if (! gap.empty())
                if (const ExPolygons cov = this->cover_near(F, nb, n); ! cov.empty())
                    gap = diff_ex(gap, cov);
            if (! gap.empty())
                out.why.emplace_back(std::move(gap), Why::InterfaceGap);
        }
    }

    for (int c = 0; c < 3; ++ c)
        for (int l = 0; l < 3; ++ l)
            out.stats.need_area[c][l] = float(area_mm2(area(out.need[c][l])));
    needs = std::move(out);
}

// UG_n = (ungrounded S ∪ C of the non-grounded fronts ∪ T_n) − (L ∩ F⁺) (plan §3.8), tagged; carry push of all fronts.
void LayerEvaluator::Impl::commit(uint32_t n)
{
    if (n == 0 || n >= base.layers.size())
        return;
    if (cur_n != int64_t(n) || ! evaluated)
        this->evaluate(n, {}, {}, std::nullopt);
    const LayerGeom &lg = base.layers[n];

    std::map<int32_t, ExPolygons> ug;
    for (const auto &[tag, area] : ugs_tagged)
        append_ex(ug[tag], area);
    std::vector<FrontCarry> fronts;
    fronts.reserve(lg.fronts.size());
    for (size_t ci = 0; ci < lg.clusters.size(); ++ ci) {
        const Cluster    &cl = lg.clusters[ci];
        const ClusterOut &co = *slots[ci].cur;
        for (size_t i = 0; i < cl.fronts.size(); ++ i) {
            const FrontGeom &front = lg.fronts[cl.fronts[i]];
            const FrontOut  &fo    = co.fronts[i];
            FrontCarry       fc;
            fc.C        = front.C;
            fc.bbox     = front.bbox;
            fc.grounded = fo.grounded;
            fc.zero     = fo.zero;
            if (fo.fields.size() == 1)
                fc.field = fo.fields.front().field->cropped(front.bbox);
            else if (fo.fields.size() > 1) {
                std::vector<PieceField::Part> parts;
                for (const FieldPart &part : fo.fields)
                    parts.push_back({ part.bbox, part.area, part.field->cropped(front.bbox) });
                fc.field = std::make_shared<const PieceField>(std::move(parts));
            }
            if (! fo.grounded)
                ug[fo.tag].push_back(front.C);
            fronts.push_back(std::move(fc));
        }
    }
    carry.push(n, lg.z_top, lg.w, std::move(fronts));

    if (! lg.tolerated.empty())
        for (size_t i = 0; i < lg.in->lslices.size(); ++ i)
            if (const int32_t t = i < lg.slice_tree.size() ? lg.slice_tree[i] : -1;
                t >= 0 && size_t(t) < base.trees.size() && base.trees[size_t(t)].tolerated)
                ug[t].push_back(lg.in->lslices[i]);

    const ExPolygons &F = contact->all;
    ug_prev.clear();
    for (auto &[tag, area] : ug) {
        ExPolygons X = union_ex(area);
        if (! X.empty() && ! F.empty())
            if (const ExPolygons cov = this->cover_near(F, get_extents(X), n); ! cov.empty())
                X = diff_ex(X, cov);
        if (! X.empty())
            ug_prev.emplace_back(tag, std::move(X));
    }
    ug_prev_all    = union_of(ug_prev);
    last_committed = n;

    cur_n     = -1;
    evaluated = false;
    slots.clear();
    contacts.clear();
    contact.reset();
    ugs.clear();
    ugs_tagged.clear();
}

LayerEvaluator::LayerEvaluator(const AnalysisInput &in, const BaseModel &base, const Reachability &reach, const ThrowOnCancel &cancel) :
    m_impl(std::make_unique<Impl>(in, base, reach, cancel)) {}

LayerEvaluator::~LayerEvaluator() = default;

const LayerNeeds &LayerEvaluator::evaluate(uint32_t n, const ExPolygons &E, const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                                           std::optional<BoundingBox> touched)
{
    return m_impl->evaluate(n, E, overrides, touched);
}

void LayerEvaluator::commit(uint32_t n) { m_impl->commit(n); }

void LayerEvaluator::mark_tree_unplaced(int32_t tree)
{
    if (tree >= 0 && size_t(tree) < m_impl->unplaced.size())
        m_impl->unplaced[size_t(tree)] = 1;
}

PassResult evaluate_pass(const AnalysisInput &in, const BaseModel &base, Reachability &reach,
                         const std::vector<ExPolygons> &E_per_layer, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    PassResult out;
    out.layers.resize(in.layers.size());
    const size_t N = std::min(in.layers.size(), base.layers.size());
    // Above the last layer with fronts, tails or manual enforcers nothing can be a need.
    const size_t last     = std::min<size_t>(N, size_t(base.last_layer_with_fronts) + 1);
    int          last_pct = -1;
    auto report = [&](int pct) {
        if (progress && pct > last_pct) {
            last_pct = pct;
            progress(pct);
        }
    };
    report(0);
    if (last > 1) {
        const ExPolygons none;
        LayerEvaluator   ev(in, base, reach, cancel);
        for (size_t n = 1; n < last; ++ n) {
            if (cancel)
                cancel();
            reach.ensure(reach.tip_layer(uint32_t(n)), cancel);
            const LayerNeeds &ln  = ev.evaluate(uint32_t(n), n < E_per_layer.size() ? E_per_layer[n] : none, {}, std::nullopt);
            LayerNeeds       &dst = out.layers[n];
            for (int c = 0; c < 3; ++ c)
                for (int l = 0; l < 3; ++ l)
                    dst.need[c][l] = ln.need[c][l];
            dst.folded = ln.folded;
            dst.why    = ln.why;
            dst.stats  = ln.stats;
            ev.commit(uint32_t(n));
            report(int(100 * n / (last - 1)));
        }
    }
    report(100);
    return out;
}

} // namespace Slic3r::SmartSupports
