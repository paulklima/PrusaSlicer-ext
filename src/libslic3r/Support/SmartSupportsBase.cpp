// Smart support painting: Phase A (fronts, clusters, bands, θ, hoop candidates, weak contact, unprintable),
// Phase A2 (island trees, tolerance, tail), Feret diameter (plan §3.5, §3.6; WP2a).
//
// Phase A is computed per layer in parallel and depends only on the slices (never on the plan). Phase A2 walks
// the slice graph bottom up. Everything is deterministic (plan §3.12): per-layer results go into their own slot,
// all orders are canonical, Phase A2 is serial.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <thread>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/PrincipalComponents2D.hpp"

namespace Slic3r::SmartSupports {

namespace {

// Arc tolerance of the round offset L_{n−1} ⊕ w_n that defines S_n (plan §3.5, D24), mm.
constexpr double ArcTolStep  = 0.001;
// Arc tolerance of the cluster band offsets, mm. Clipper puts the arc vertices on the circle, so a band is
// at most this much smaller than the exact ∪C ⊕ m.
constexpr double ArcTolBand  = 0.005;
// Tolerance of the "lies on" tests (hoop rule §3.5, floating edges of the bridge directions), mm.
constexpr double OnTol       = 0.002;
// Hoop rule (§3.5): the matching slice hole may differ by this much in its bbox (mm) and in its area (relative);
// at most this fraction of the hole's length may lie on S_n.
constexpr double HoopBBoxTol = 0.002;
constexpr double HoopAreaTol = 0.01;
constexpr double HoopHoleOnS = 0.01;

inline double area_mm2(double scaled_area) { return scaled_area * SCALING_FACTOR * SCALING_FACTOR; }

// Unit direction modulo 180°: the representative with its angle in [0, π).
Vec2d canonical_direction(Vec2d d)
{
    if (std::abs(d.y()) < 1e-12)
        d.y() = 0.;
    if (d.y() < 0. || (d.y() == 0. && d.x() < 0.))
        d = - d;
    return d;
}

double angle_mod_pi(const Vec2d &canonical)
{
    const double a = std::atan2(canonical.y(), canonical.x());
    return a >= M_PI ? a - M_PI : std::max(0., a);
}

// Smallest angle between two directions modulo 180°.
double angle_distance_mod_pi(double a, double b)
{
    const double d = std::abs(a - b);
    return std::min(d, M_PI - d);
}

// Length of the closed outline of poly inside the clip polygons.
double length_inside(const Polygon &poly, const Polygons &clip)
{
    return clip.empty() ? 0. : total_length(intersection_pl(Polylines{ to_polyline(poly) }, clip));
}

BoundingBox slice_bbox(const LayerInput &li, size_t idx)
{
    if (idx < li.slices.size() && li.slices[idx].bbox.defined)
        return li.slices[idx].bbox;
    return get_extents(li.lslices[idx].contour);
}

// Indices of the slices of the layer that contain the front (normally exactly one).
std::vector<int> containing_slices(const LayerInput &li, const FrontGeom &f)
{
    std::vector<int> candidates;
    for (size_t i = 0; i < li.lslices.size(); ++ i)
        if (slice_bbox(li, i).inflated(SCALED_EPSILON).contains(f.bbox))
            candidates.push_back(int(i));
    if (candidates.empty())
        // Not expected (C ⊂ L); be tolerant against rounding of the bounding boxes.
        for (size_t i = 0; i < li.lslices.size(); ++ i)
            if (slice_bbox(li, i).overlap(f.bbox))
                candidates.push_back(int(i));
    if (candidates.size() <= 1)
        return candidates;
    // Nested slices (a slice inside the hole of another one): decide by overlap.
    std::vector<int> out;
    for (int i : candidates)
        if (! intersection(li.lslices[size_t(i)], f.C).empty())
            out.push_back(i);
    return out;
}

// Canonical order of the fronts (plan §3.12): (bbox.min.y, bbox.min.x, area); Clipper's order breaks ties.
void sort_fronts(std::vector<FrontGeom> &fronts)
{
    std::stable_sort(fronts.begin(), fronts.end(), [](const FrontGeom &a, const FrontGeom &b) {
        if (a.bbox.min.y() != b.bbox.min.y())
            return a.bbox.min.y() < b.bbox.min.y();
        if (a.bbox.min.x() != b.bbox.min.x())
            return a.bbox.min.x() < b.bbox.min.x();
        return a.area_mm2 < b.area_mm2;
    });
}

// Clusters of the fronts whose windows bbox(C) ⊕ window_margin overlap (§3.5). The windows of the result are
// disjoint (not even touching); fronts in canonical order, clusters by (win.min.y, win.min.x, first front).
std::vector<Cluster> make_clusters(const std::vector<FrontGeom> &fronts, coord_t window_margin)
{
    std::vector<Cluster> clusters;
    clusters.reserve(fronts.size());
    for (uint32_t i = 0; i < uint32_t(fronts.size()); ++ i) {
        Cluster &cl = clusters.emplace_back();
        cl.win = fronts[i].bbox.inflated(coordf_t(window_margin));
        cl.fronts.push_back(i);
    }
    // Sweep over the windows sorted by min.x. A merged window may reach windows skipped before, so repeat until
    // a sweep merges nothing.
    for (bool merged = true; merged;) {
        merged = false;
        std::sort(clusters.begin(), clusters.end(), [](const Cluster &a, const Cluster &b) {
            return a.win.min.x() < b.win.min.x() || (a.win.min.x() == b.win.min.x() && a.fronts.front() < b.fronts.front());
        });
        std::vector<uint8_t> dead(clusters.size(), 0);
        for (size_t i = 0; i < clusters.size(); ++ i) {
            if (dead[i])
                continue;
            for (size_t j = i + 1; j < clusters.size() && clusters[j].win.min.x() <= clusters[i].win.max.x(); ++ j)
                if (! dead[j] && clusters[i].win.overlap(clusters[j].win)) {
                    clusters[i].win.merge(clusters[j].win);
                    append(clusters[i].fronts, std::move(clusters[j].fronts));
                    dead[j] = 1;
                    merged  = true;
                }
        }
        if (merged) {
            size_t k = 0;
            for (size_t i = 0; i < clusters.size(); ++ i)
                if (! dead[i]) {
                    if (k != i)
                        clusters[k] = std::move(clusters[i]);
                    ++ k;
                }
            clusters.resize(k);
        }
    }
    for (Cluster &cl : clusters)
        std::sort(cl.fronts.begin(), cl.fronts.end());
    std::sort(clusters.begin(), clusters.end(), [](const Cluster &a, const Cluster &b) {
        if (a.win.min.y() != b.win.min.y())
            return a.win.min.y() < b.win.min.y();
        if (a.win.min.x() != b.win.min.x())
            return a.win.min.x() < b.win.min.x();
        return a.fronts.front() < b.fronts.front();
    });
    return clusters;
}

// Hoop candidate (D26, §3.5): the front has exactly one hole, that hole is a hole H of its slice (bbox ± 2 µm,
// area ± 1 %), H lies (almost) nowhere on S_n, and ≥ hoop_outer_fraction of the front's contour lies on S_n.
// S_on = S_n of the band, expanded by the "lies on" tolerance.
void detect_hoop(FrontGeom &f, const LayerInput &li, const Polygons &S_on, const Params &params)
{
    if (f.C.holes.size() != 1 || S_on.empty())
        return;
    const Polygon     &hole      = f.C.holes.front();
    const BoundingBox  hole_bbox = get_extents(hole);
    const double       hole_area = std::abs(hole.area());
    const coord_t      bbox_tol  = scaled<coord_t>(HoopBBoxTol);
    auto close = [bbox_tol](const Point &a, const Point &b) { return std::abs(a.x() - b.x()) <= bbox_tol && std::abs(a.y() - b.y()) <= bbox_tol; };
    for (int slice_idx : f.slices)
        for (const Polygon &H : li.lslices[size_t(slice_idx)].holes) {
            const BoundingBox bb = get_extents(H);
            if (! close(bb.min, hole_bbox.min) || ! close(bb.max, hole_bbox.max))
                continue;
            const double a = std::abs(H.area());
            if (std::abs(a - hole_area) > HoopAreaTol * a)
                continue;
            // H is fully free: nothing of it rests on material below, so support cannot change it.
            if (length_inside(H, S_on) > HoopHoleOnS * H.length())
                return;
            // The outer boundary lies on S_n (material below, grounded or not).
            if (length_inside(f.C.contour, S_on) < params.hoop_outer_fraction * f.C.contour.length())
                return;
            f.hoop_hole     = 0;
            f.hoop_feret_mm = unscale<double>(feret_diameter(H));
            return;
        }
}

// Weak contact (§3.5, theory §2.2): Σ overlap below < κ·area and the extent of the overlap < a_min.
// A slice without any overlap below is weak as well.
void detect_weak_contact(LayerGeom &lg, const ExPolygons &Lp, const Params &params)
{
    const LayerInput &li    = *lg.in;
    const double      a_min = scaled<double>(params.a_min_widths * params.ext_width);
    for (size_t i = 0; i < li.lslices.size(); ++ i) {
        double overlap = 0.;
        if (i < li.slices.size())
            for (const LayerInput::Link &link : li.slices[i].below)
                overlap += double(link.area);
        if (overlap >= params.kappa * li.lslices[i].area())
            continue;
        const ExPolygons common = intersection_ex(li.lslices[i],
            ClipperUtils::clip_clipper_polygons_with_subject_bbox(Lp, slice_bbox(li, i).inflated(SCALED_EPSILON)));
        double extent = 0.;
        if (! common.empty()) {
            const BoundingBox bb = get_extents(common);
            extent = double(std::max(bb.size().x(), bb.size().y()));
        }
        if (extent < a_min)
            lg.slice_bad[i] = 1;
    }
}

// Phase A of one layer (§3.5); z, h, w, w_β, slice_bad and slice_tree are already set up.
void build_layer_geometry(const AnalysisInput &in, size_t n, LayerGeom &lg)
{
    const Params     &params = in.params;
    const LayerInput &li     = *lg.in;
    const ExPolygons &L      = li.lslices;
    const double      ext_w  = scaled<double>(params.ext_width);
    if (L.empty())
        return;
    // Material thinner than the minimum feature width is never an anchor (theory §2.1). The safety offset of
    // the clip removes the rounding slivers along the outline.
    if (const double r = 0.5 * params.printable_width_factor * ext_w; r > 0.)
        lg.unprintable = diff_ex(L, opening_ex(L, float(r)), ApplySafetyOffset::Yes);
    if (n == 0)
        return;                                                 // L_{−1} is the bed: U_0 = ∅
    const ExPolygons &Lp = in.layers[n - 1].lslices;

    detect_weak_contact(lg, Lp, params);

    // Unsupported area U_n = L_n − S_n = L_n − (L_{n−1} ⊕ w_n), round offset (D24). No area filter (theory §2.1).
    ExPolygons U = Lp.empty() ? L :
        lg.w > 0 ? diff_ex(L, offset(Lp, float(lg.w), ClipperLib::jtRound, scaled<double>(ArcTolStep))) : diff_ex(L, Lp);
    lg.fronts.reserve(U.size());
    for (ExPolygon &C : U)
        if (const double a = C.area(); a > 0.) {
            FrontGeom &f = lg.fronts.emplace_back();
            f.bbox     = get_extents(C.contour);
            f.area_mm2 = area_mm2(a);
            f.C        = std::move(C);
        }
    if (lg.fronts.empty())
        return;
    sort_fronts(lg.fronts);

    // Clusters and their bands (D25): m = cluster_margin + ρ + w, window = bbox(C) ⊕ (m + w).
    const coord_t m = scaled<coord_t>(params.cluster_margin + params.rho_exact) + lg.w;
    lg.clusters = make_clusters(lg.fronts, m + lg.w);
    const bool   custom_theta = params.custom_bridge_angle_deg > 0.;
    const double arc_band     = scaled<double>(ArcTolBand);
    for (Cluster &cl : lg.clusters) {
        ExPolygons Cs;
        Cs.reserve(cl.fronts.size());
        bool any_wide = false, any_one_hole = false;
        for (uint32_t idx : cl.fronts) {
            FrontGeom &f = lg.fronts[idx];
            Cs.push_back(f.C);
            f.wide        = ! shrink_ex(ExPolygons{ f.C }, float(ext_w)).empty();   // opening by ext_w is not empty
            f.slices      = containing_slices(li, f);
            any_wide     |= f.wide;
            any_one_hole |= f.C.holes.size() == 1;
        }
        const BoundingBox clip_box = cl.win.inflated(SCALED_EPSILON);
        cl.band      = intersection_ex(offset(Cs, float(m), ClipperLib::jtRound, arc_band),
                                       ClipperUtils::clip_clipper_polygons_with_subject_bbox(L, clip_box));
        cl.band_prev = intersection_ex(offset(Cs, float(m + lg.w), ClipperLib::jtRound, arc_band),
                                       ClipperUtils::clip_clipper_polygons_with_subject_bbox(Lp, clip_box));

        // Bridge regions: the slicer's bottom surfaces L_n − L_{n−1} near the cluster, slivers removed.
        ExPolygons bridge_regions;
        if (any_wide && ! custom_theta)
            bridge_regions = opening_ex(diff_ex(cl.band, cl.band_prev), float(0.1 * ext_w));
        // S_n inside the band (the cluster's fronts are the only part of U_n there), expanded for "lies on".
        Polygons S_on;
        if (any_one_hole)
            if (ExPolygons S_band = diff_ex(cl.band, Cs); ! S_band.empty())
                S_on = expand(S_band, scaled<float>(OnTol));

        for (uint32_t idx : cl.fronts) {
            FrontGeom &f = lg.fronts[idx];
            if (f.wide) {
                if (custom_theta) {
                    const double a = params.custom_bridge_angle_deg * M_PI / 180.;
                    f.thetas = { canonical_direction(Vec2d(std::cos(a), std::sin(a))) };
                } else {
                    // B_C = the components of the bridge regions intersecting C (C itself if none survived).
                    ExPolygons B_C;
                    for (const ExPolygon &b : bridge_regions)
                        if (get_extents(b.contour).overlap(f.bbox) && ! intersection(b, f.C).empty())
                            B_C.push_back(b);
                    if (B_C.empty())
                        B_C.push_back(f.C);
                    f.thetas = bridge_directions(B_C, cl.band_prev, params.tie_tolerance, params.max_tie_dirs);
                }
            }
            detect_hoop(f, li, S_on, params);
        }
    }
}

// Phase A2 (§3.6): island trees, tolerance, tails. Serial, bottom up.
void build_island_trees(const AnalysisInput &in, BaseModel &base, const ThrowOnCancel &cancel)
{
    const Params &params = in.params;
    const size_t  N      = in.layers.size();
    // Global slice index g = first_slice[n] + i.
    std::vector<size_t> first_slice(N + 1, 0);
    for (size_t n = 0; n < N; ++ n)
        first_slice[n + 1] = first_slice[n] + in.layers[n].lslices.size();
    const size_t total = first_slice[N];
    // Calls fn(global index of the slice below) for the valid below links of slice i of layer n.
    auto for_each_below = [&](size_t n, size_t i, auto &&fn) {
        const LayerInput &li = in.layers[n];
        if (n == 0 || i >= li.slices.size())
            return;
        for (const LayerInput::Link &link : li.slices[i].below)
            if (link.idx >= 0 && size_t(link.idx) < in.layers[n - 1].lslices.size())
                fn(first_slice[n - 1] + size_t(link.idx));
    };

    // Connected to the bed through overlaps below, support ignored (theory §3.1); weak contact breaks it.
    std::vector<uint8_t> bed(total, 0);
    for (size_t n = 0; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i) {
            const size_t g = first_slice[n] + i;
            if (n == 0)
                bed[g] = 1;
            else if (! base.layers[n].slice_bad[i])
                for_each_below(n, i, [&](size_t j) { bed[g] |= bed[j]; });
        }

    // Union-find over the ungrounded slices along their links below.
    std::vector<size_t> parent(total);
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto find = [&parent](size_t x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t n = 1; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
            if (const size_t g = first_slice[n] + i; ! bed[g])
                for_each_below(n, i, [&](size_t j) {
                    if (! bed[j])
                        if (const size_t a = find(g), b = find(j); a != b)
                            parent[std::max(a, b)] = std::min(a, b);
                });

    // Trees, numbered in the order of their first slice (bottom up, slice order).
    std::vector<int32_t>     tree_of(total, -1);
    std::vector<int32_t>     tree_of_root(total, -1);
    std::vector<IslandTree> &trees = base.trees;
    for (size_t n = 0; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
            if (const size_t g = first_slice[n] + i; ! bed[g]) {
                const size_t r = find(g);
                if (tree_of_root[r] < 0) {
                    tree_of_root[r] = int32_t(trees.size());
                    IslandTree &tree = trees.emplace_back();
                    tree.id    = tree_of_root[r];
                    tree.first = uint32_t(n);
                }
                tree_of[g] = tree_of_root[r];
                base.layers[n].slice_tree[i] = tree_of[g];
            }
    if (trees.empty())
        return;

    // Merge = the lowest grounded slice that links into the tree.
    for (size_t n = 1; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
            if (bed[first_slice[n] + i])
                for_each_below(n, i, [&](size_t j) {
                    if (tree_of[j] >= 0)
                        trees[size_t(tree_of[j])].merge = std::min(trees[size_t(tree_of[j])].merge, uint32_t(n));
                });
    // Volume below the merge, tolerance (theory §3.1, D22).
    for (size_t n = 0; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
            if (const int32_t t = tree_of[first_slice[n] + i]; t >= 0 && n < trees[size_t(t)].merge)
                trees[size_t(t)].volume_mm3 += area_mm2(in.layers[n].lslices[i].area()) * in.layers[n].height;
    for (IslandTree &tree : trees)
        tree.tolerated = tree.merge != UINT32_MAX &&
                         in.layers[tree.merge].slice_z - in.layers[tree.first].slice_z <= params.H_merge + EPSILON &&
                         tree.volume_mm3 < params.V_min;
    for (size_t n = 0; n < N; ++ n)
        for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
            if (const int32_t t = tree_of[first_slice[n] + i]; t >= 0 && trees[size_t(t)].tolerated)
                base.layers[n].tolerated.push_back(in.layers[n].lslices[i]);   // slices are disjoint: no union needed

    // Tails of the non-tolerated trees (theory §3.1), per branch: a branch starts at a root slice (no tree slice
    // below it) and continues upwards through the linked tree slices while the slice is thin (inscribed width
    // < W_stiff) and within H_tail of the root. A grounded slice ends the branch (it merges). Where branches join,
    // the latest root counts (the longer remaining tail).
    const float          stiff_half = scaled<float>(0.5 * params.W_stiff);
    const float          noise      = scaled<float>(0.5 * params.eps);
    std::vector<uint8_t> in_tail(total, 0);
    std::vector<double>  root_z(total, 0.);
    for (size_t n = 0; n < N; ++ n) {
        if ((n & 63) == 0 && cancel)
            cancel();
        const LayerInput             &li = in.layers[n];
        std::map<int32_t, ExPolygons> needs;                    // ordered by tree id
        for (size_t i = 0; i < li.lslices.size(); ++ i) {
            const size_t  g = first_slice[n] + i;
            const int32_t t = tree_of[g];
            if (t < 0 || trees[size_t(t)].tolerated)
                continue;
            bool   root = true, from_tail = false;
            double rz   = - std::numeric_limits<double>::max();
            for_each_below(n, i, [&](size_t j) {
                if (tree_of[j] >= 0) {
                    root = false;
                    if (in_tail[j]) {
                        from_tail = true;
                        rz = std::max(rz, root_z[j]);
                    }
                }
            });
            if (root)
                rz = li.slice_z;
            else if (! from_tail)
                continue;
            if (li.slice_z - rz > params.H_tail + EPSILON)
                continue;
            const ExPolygon &part = li.lslices[i];
            if (! shrink_ex(ExPolygons{ part }, stiff_half).empty())
                continue;                                       // stiff enough: the tail ends here
            in_tail[g] = 1;
            root_z[g]  = rz;
            IslandTree &tree = trees[size_t(t)];
            tree.tail_last = std::max(tree.tail_last, uint32_t(n));
            ExPolygons tn;
            if (root)
                tn.push_back(part);
            else {
                // Only downward facets can carry tips: w = 0 inside the tail.
                tn = diff_ex(part, ClipperUtils::clip_clipper_polygons_with_subject_bbox(in.layers[n - 1].lslices,
                                                                                          get_extents(part.contour).inflated(SCALED_EPSILON)));
                if (opening_ex(tn, noise).empty()) {
                    tn.clear();
                    tree.tail_without_downward = true;
                }
            }
            if (! tn.empty())
                append(needs[t], std::move(tn));
        }
        for (auto &[t, area] : needs)
            base.layers[n].tail_needs.emplace_back(t, std::move(area));
    }
}

} // namespace

BaseModel build_base_model(const AnalysisInput &in, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    const Params &params = in.params;
    const size_t  N      = in.layers.size();
    BaseModel     base;
    base.layers.resize(N);
    for (size_t n = 0; n < N; ++ n) {
        const LayerInput &li = in.layers[n];
        LayerGeom        &lg = base.layers[n];
        lg.in    = &li;
        lg.h     = li.height;
        lg.z_bot = li.slice_z - 0.5 * li.height;
        lg.z_top = li.slice_z + 0.5 * li.height;
        if (n > 0) {
            // §3.4: w_n = 0.5·ext_width_avg(n−1) for α = 0, else h_{n−1} / tan α; w_β = h_{n−1} / tan β, β ∈ (0, α_eff].
            const LayerInput &below     = in.layers[n - 1];
            const double      w_auto    = 0.5 * below.ext_width_avg_mm;
            const double      alpha_eff = params.user.alpha_deg > 0 ? double(params.user.alpha_deg) :
                                          (w_auto > 0. ? 180. / M_PI * atan(below.height / w_auto) : 90.);
            const double      w_mm      = params.user.alpha_deg > 0 ? below.height / tan(M_PI / 180. * double(params.user.alpha_deg)) : w_auto;
            lg.w = scaled<coord_t>(w_mm);
            if (params.user.beta_equals_alpha || params.user.beta_deg >= alpha_eff || params.user.beta_deg <= 0.)
                lg.w_beta = lg.w;
            else
                lg.w_beta = std::max(lg.w, scaled<coord_t>(below.height / tan(M_PI / 180. * params.user.beta_deg)));
        }
        lg.slice_bad.assign(li.lslices.size(), 0);
        lg.slice_tree.assign(li.lslices.size(), -1);
    }
    if (N == 0)
        return base;

    // Progress 0..100 of this phase, reported from the calling thread only (monotone):
    // manual zones 0–10, layers 10–90, island trees 90–100.
    const std::thread::id caller   = std::this_thread::get_id();
    int                   last_pct = -1;
    auto report = [&](int pct) {
        if (progress && std::this_thread::get_id() == caller && pct > last_pct) {
            last_pct = pct;
            progress(pct);
        }
    };
    report(0);

    // Manual zones: painted enforcers / blockers (states 1 / 2 only, never smart paint) and support volumes.
    const std::vector<const TriangleSelector::TriangleSplittingData*> own_paint(in.volumes.size(), nullptr);
    const std::vector<Polygons> enf_paint = project_states(in, own_paint, [](TriangleStateType s) { return s == TriangleStateType::ENFORCER; }, cancel);
    const std::vector<Polygons> blk_paint = project_states(in, own_paint, [](TriangleStateType s) { return s == TriangleStateType::BLOCKER; }, cancel);
    const std::vector<Polygons> enf_vol   = slice_modifiers(in, true, cancel);
    const std::vector<Polygons> blk_vol   = slice_modifiers(in, false, cancel);
    report(10);

    const Polygons      no_polygons;
    std::atomic<size_t> done { 0 };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, N), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t n = range.begin(); n < range.end(); ++ n) {
            if (cancel)
                cancel();
            LayerGeom &lg = base.layers[n];
            build_layer_geometry(in, n, lg);
            const Polygons &ep = n < enf_paint.size() ? enf_paint[n] : no_polygons, &ev = n < enf_vol.size() ? enf_vol[n] : no_polygons;
            const Polygons &bp = n < blk_paint.size() ? blk_paint[n] : no_polygons, &bv = n < blk_vol.size() ? blk_vol[n] : no_polygons;
            if (! ep.empty() || ! ev.empty())
                lg.manual_enf = union_ex(ep, ev);
            if (! bp.empty() || ! bv.empty())
                lg.manual_blk = union_ex(bp, bv);
            report(10 + int(80 * (done.fetch_add(1, std::memory_order_relaxed) + 1) / N));
        }
    });

    build_island_trees(in, base, cancel);
    for (size_t n = N; n > 0; -- n) {
        const LayerGeom &lg = base.layers[n - 1];
        if (! lg.fronts.empty() || ! lg.tail_needs.empty() || ! lg.manual_enf.empty()) {
            base.last_layer_with_fronts = uint32_t(n - 1);
            break;
        }
    }
    report(100);
    return base;
}

double feret_diameter(const Polygon &polygon)
{
    if (polygon.size() < 2)
        return 0.;
    const Polygon hull = Geometry::convex_hull(polygon.points);
    const Points &pts  = hull.size() >= 2 ? hull.points : polygon.points;
    double best = 0.;
    for (size_t i = 0; i < pts.size(); ++ i)
        for (size_t j = i + 1; j < pts.size(); ++ j)
            best = std::max(best, (pts[j] - pts[i]).cast<double>().squaredNorm());
    return std::sqrt(best);
}

std::vector<Vec2d> bridge_directions(const ExPolygons &bridge_region, const ExPolygons &lower_local, double tie_tol, int max_dirs)
{
    std::vector<Vec2d> out;
    if (bridge_region.empty() || max_dirs <= 0)
        return out;
    // detect_bridging_direction(to_cover, anchors_area), BridgeDetector.hpp:131-136: the floating edges are the
    // outline of the overhang away from the anchors.
    const Polygons overhang = lower_local.empty() ? to_polygons(bridge_region) : diff(bridge_region, lower_local);
    const Lines    floating = to_lines(lower_local.empty() ? to_polylines(overhang) :
                                       diff_pl(to_polylines(overhang), expand(lower_local, scaled<float>(OnTol))));

    // Candidate normals of the floating edges, quantized by angle (BridgeDetector.hpp:100-105) and taken modulo
    // 180° (n and −n cost the same). Edges with the same quantized direction are summed up, which keeps the
    // cost loop at O(directions²).
    struct Dir { Vec2d normal; double length = 0.; };
    std::map<long long, Dir> dirs;                              // ordered: deterministic
    for (const Line &l : floating) {
        const Vec2d  v   = (l.b - l.a).cast<double>();
        const double len = v.norm();
        if (len <= 0.)
            continue;
        const Vec2d normal = canonical_direction(Vec2d(v.y(), - v.x()) / len);
        dirs.emplace(std::llround(std::ceil(angle_mod_pi(normal) * 1000.)), Dir{ normal, 0. }).first->second.length += len;
    }
    if (dirs.empty()) {
        // Anchored all around: the minor principal axis, i.e. the shortest bridges (BridgeDetector.hpp:90-96).
        const auto [pc1, pc2] = compute_principal_components(overhang);
        out.push_back(pc2 == Vec2f::Zero() ? Vec2d(1., 0.) : canonical_direction(pc2.cast<double>().normalized()));
        return out;
    }

    struct Candidate { Vec2d dir; double angle; double cost; };
    std::vector<Candidate> candidates;
    candidates.reserve(dirs.size());
    double min_cost = std::numeric_limits<double>::max();
    for (const auto &[key, n] : dirs) {
        // Cost of the normal n: Σ |edge · n|. The bridge lines run perpendicular to n, along the cheapest edges.
        double cost = 0.;
        for (const auto &[key2, e] : dirs)
            cost += e.length * std::abs(Vec2d(- e.normal.y(), e.normal.x()).dot(n.normal));
        const Vec2d dir = canonical_direction(Vec2d(n.normal.y(), - n.normal.x()));
        candidates.push_back({ dir, angle_mod_pi(dir), cost });
        min_cost = std::min(min_cost, cost);
    }
    // All directions within the tie tolerance of the best one (theory §2.4), sorted by (cost, angle mod π).
    const double limit = min_cost * (1. + tie_tol) + 1.;       // + 1 scaled unit: exact ties at zero cost
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [limit](const Candidate &c) { return c.cost > limit; }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.cost < b.cost || (a.cost == b.cost && a.angle < b.angle);
    });
    // Nearly equal directions give nearly equal spans: keep directions at least π / max_dirs apart (greedy in cost
    // order), so that up to max_dirs ties sample the whole half circle (round fronts).
    const double        min_separation = M_PI / double(max_dirs) - 1e-9;
    std::vector<double> kept_angles;
    for (const Candidate &c : candidates) {
        if (int(out.size()) >= max_dirs)
            break;
        if (std::all_of(kept_angles.begin(), kept_angles.end(), [&](double a) { return angle_distance_mod_pi(a, c.angle) >= min_separation; })) {
            out.push_back(c.dir);
            kept_angles.push_back(c.angle);
        }
    }
    return out;
}

} // namespace Slic3r::SmartSupports
