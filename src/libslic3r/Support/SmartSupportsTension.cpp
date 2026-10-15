// Smart support painting: anchors, domain edges, tension (ties, cuts), chord rule, hoop credit, classify_front
// (plan §3.8, D26, D27; theory §2.4, §2.5, §3.2; WP2b2).
//
// All functions are pure, serial and thread-safe; the evaluator calls them per cluster in parallel.
// Polylines are returned in a canonical order (plan §3.12): open polylines run first < last, closed loops start at
// their lexicographically smallest point, lists are sorted by (first point, last point, size).

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

namespace {

// Boundary pieces within this distance of a region belong to it (plan §3.8: 2 µm).
constexpr double edge_tol_mm        = 0.002;
// Local operations clip the band-wide inputs to bbox(C') inflated by this margin.
constexpr double local_margin_mm    = 0.05;
// Chord rule (D27): a free piece ends at an anchor if an anchor lies within this distance of its end point.
constexpr double touch_tol_mm       = 0.005;
// Chord rule: the direction of the closing anchor is taken to its first vertex at least this far from the end
// point (skips the 2 µm stub that the boundary tolerance leaves along the free edge).
constexpr double closure_probe_mm   = 0.01;
constexpr double closure_min_deg    = 45.;
// Chord rule: C' within this many external widths of a qualifying free piece takes its chord.
constexpr double chord_reach_widths = 2.;
// Spans of exactly B·ratio keep the lower level (theory §10: a 90 mm bridge is "should").
constexpr double span_tol_mm        = 0.001;
// Scanlines: phase within the spacing, so that axis aligned boundaries at round coordinates never coincide with a
// scanline; strip ends reach at most this many spacings beyond their segment (towards the gap midpoint).
constexpr double scan_phase         = 0.37;
constexpr double strip_reach        = 2.;
// Adjacent strips overlap by this much, so that rounding never leaves gaps between them.
constexpr double strip_overlap_mm   = 0.001;
constexpr int    max_scanlines      = 20000;
constexpr int    max_cuts_per_span  = 1000;

int    level_index(TLevel level) { return int(level); }
double cross2(const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); }
Point  to_point(const Vec2d &v) { return Point(coord_t(std::round(v.x())), coord_t(std::round(v.y()))); }

void canonicalize(Polylines &polylines)
{
    for (Polyline &pl : polylines) {
        pl.points.erase(std::unique(pl.points.begin(), pl.points.end()), pl.points.end());
        if (pl.points.size() >= 4 && pl.points.front() == pl.points.back()) {
            // Closed loop: start at the lexicographically smallest point, keep the orientation.
            pl.points.pop_back();
            std::rotate(pl.points.begin(), std::min_element(pl.points.begin(), pl.points.end()), pl.points.end());
            pl.points.push_back(pl.points.front());
        } else if (pl.points.size() >= 2 && pl.points.back() < pl.points.front())
            pl.reverse();
    }
    polylines.erase(std::remove_if(polylines.begin(), polylines.end(), [](const Polyline &pl) { return pl.points.size() < 2; }),
                    polylines.end());
    std::sort(polylines.begin(), polylines.end(), [](const Polyline &a, const Polyline &b) {
        if (a.points.front() != b.points.front())
            return a.points.front() < b.points.front();
        if (a.points.back() != b.points.back())
            return a.points.back() < b.points.back();
        return a.points.size() < b.points.size();
    });
}

// Band-wide input clipped to a local bbox (valid as a clip operand inside the bbox).
Polygons local(const ExPolygons &src, const BoundingBox &bbox)
{
    return ClipperUtils::clip_clipper_polygons_with_subject_bbox(src, bbox);
}

// Tension level of a finite span of t mm (theory §2.5): ≤ B covered, ≤ f·B may, ≤ f²·B should, else must.
TLevel span_level(double t, const Params &params)
{
    const double B = params.user.B_max;
    const double f = params.f_hard;
    if (t <= B + span_tol_mm)
        return TLevel::Covered;
    if (t <= f * B + span_tol_mm)
        return TLevel::May;
    if (t <= f * f * B + span_tol_mm)
        return TLevel::Should;
    return TLevel::Must;
}

// Row candidates of a long span (theory §3.2, plan §3.8): k = ⌈t/B⌉ − 1 cuts (one for t ≤ 2B); cut i (1-based) may
// lie anywhere in [t − (k + 1 − i)·B, i·B], measured from p along dir, so that all sub-spans can stay ≤ B.
// The interval centres are B apart and leave both end spans ≤ B.
void add_cuts(const Point &p, const Vec2d &dir, double t, const Params &params, std::vector<TensionMap::Cut> &out)
{
    const double B = params.user.B_max;
    if (B <= 0. || t <= B + span_tol_mm)
        return;
    const int k = std::clamp(int(std::ceil(t / B - 1e-6)) - 1, 1, max_cuts_per_span);
    for (int i = 1; i <= k; ++ i) {
        double lo = std::max(0., t - double(k + 1 - i) * B);
        double hi = std::min(t, double(i) * B);
        if (lo > hi)
            lo = hi = 0.5 * (lo + hi);
        out.push_back({ p, dir, t, lo, hi });
    }
}

// good[ℓ] = points of C' whose tension level is ℓ or better (ℓ = Covered … Must), nested. Partition into
// TensionMap::regions: Covered = good[0], May = good[1] − good[0], …, None = C' − good[3].
void fill_regions(const Polygons &cp, const std::array<Polygons, 4> &good, TensionMap &out)
{
    std::array<ExPolygons, 5> regions;
    out.covered_all = union_ex(good[0]);
    regions[0]      = out.covered_all;
    for (int l = 1; l < 4; ++ l)
        regions[l] = diff_ex(good[l], good[l - 1]);
    regions[4] = diff_ex(cp, good[3]);
    for (int l = 0; l < 5; ++ l)
        if (! regions[l].empty())
            out.regions.emplace_back(std::move(regions[l]), TLevel(l));
}

// ---------------------------------------------------------------------------------------------------------------
// Wide fronts: scanlines along each tied θ, spacing ½·ext_w, clipped to the D component (plan §3.8). A span is
// finite if both ends lie on anchor edges of D (GP ∪ F); free edges and the band border end it with t = ∞.
// The worst level over θ counts, as the slicer may pick any of the tied directions (theory §2.4, review 1 m5);
// covered_all = covered under all θ.

struct ScanSegment {
    int    line;      // scanline index j
    double l0, l1;    // positions along θ (scaled), l0 < l1
    Point  a, b;      // end points, a → b along θ
};

void wide_tension(const ExPolygons &cprime, const Polygons &cp, const FrontGeom &front, const ExPolygons &D_component,
                  const DomainEdges &edges, const Params &params, TensionMap &out)
{
    // bad[ℓ]: points worse than ℓ under some θ (incl. points that no strip of some θ reaches).
    std::array<Polygons, 4> bad;
    if (front.thetas.empty() || D_component.empty()) {
        fill_regions(cp, {}, out);                         // no bridge direction known: no tension
        return;
    }

    const BoundingBox bbox_c  = get_extents(cprime);
    const BoundingBox bbox_d  = get_extents(D_component);
    const Polygons    domain  = to_polygons(D_component);
    const double      spacing = scaled<double>(std::max(0.5 * params.ext_width, 0.05));
    const double      min_len = scaled<double>(edge_tol_mm);

    auto is_anchor = [&edges](const Point &p) {
        const double da = edges.anchor ? edges.anchor->distance_from_lines<false>(p) : std::numeric_limits<double>::infinity();
        const double df = edges.free_  ? edges.free_->distance_from_lines<false>(p)  : std::numeric_limits<double>::infinity();
        return std::isfinite(da) && da <= df;
    };
    auto project = [](const BoundingBox &bb, const Vec2d &v, double &lo, double &hi) {
        lo = std::numeric_limits<double>::max();
        hi = std::numeric_limits<double>::lowest();
        for (const Point &corner : { bb.min, Point(bb.max.x(), bb.min.y()), bb.max, Point(bb.min.x(), bb.max.y()) }) {
            const double d = corner.cast<double>().dot(v);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    };

    for (const Vec2d &theta : front.thetas) {
        if (theta.squaredNorm() <= 0.)
            continue;
        const Vec2d u = theta.normalized();
        const Vec2d n(- u.y(), u.x());
        double n_lo, n_hi, l_lo, l_hi;
        project(bbox_c, n, n_lo, n_hi);
        project(bbox_d, u, l_lo, l_hi);
        l_lo -= spacing;
        l_hi += spacing;
        const double step      = std::max(spacing, (n_hi - n_lo) / double(max_scanlines));
        const double half_step = 0.5 * step + scaled<double>(strip_overlap_mm);
        // Scanline j: n·p = (j + phase)·step; all lines whose strip reaches bbox(C').
        const int j0 = int(std::ceil(n_lo / step - scan_phase - 0.5));
        const int j1 = int(std::floor(n_hi / step - scan_phase + 0.5));
        Lines lines;
        lines.reserve(std::max(0, j1 - j0 + 1));
        for (int j = j0; j <= j1; ++ j) {
            const Vec2d o = n * ((double(j) + scan_phase) * step);
            lines.emplace_back(to_point(o + u * l_lo), to_point(o + u * l_hi));
        }
        std::vector<ScanSegment> segments;
        for (const Line &l : intersection_ln(lines, domain)) {
            ScanSegment s;
            s.a  = l.a;
            s.b  = l.b;
            s.l0 = s.a.cast<double>().dot(u);
            s.l1 = s.b.cast<double>().dot(u);
            if (s.l1 < s.l0) {
                std::swap(s.a, s.b);
                std::swap(s.l0, s.l1);
            }
            if (s.l1 - s.l0 < min_len)
                continue;
            s.line = int(std::round(0.5 * (s.a + s.b).cast<double>().dot(n) / step - scan_phase));
            segments.push_back(s);
        }
        std::sort(segments.begin(), segments.end(), [](const ScanSegment &x, const ScanSegment &y) {
            return x.line < y.line || (x.line == y.line && x.l0 < y.l0);
        });

        // Strip of a segment: its band of the scanline, extended along θ towards the midpoint of the neighbouring gap
        // (at most strip_reach spacings), so that C' next to slanted boundaries takes the level of its own segment.
        std::array<Polygons, 5> strips;
        for (size_t i = 0; i < segments.size(); ++ i) {
            const ScanSegment &s     = segments[i];
            const bool         first = i == 0 || segments[i - 1].line != s.line;
            const bool         last  = i + 1 == segments.size() || segments[i + 1].line != s.line;
            double lo = s.l0 - strip_reach * step;
            double hi = s.l1 + strip_reach * step;
            if (! first)
                lo = std::max(lo, 0.5 * (segments[i - 1].l1 + s.l0));
            if (! last)
                hi = std::min(hi, 0.5 * (s.l1 + segments[i + 1].l0));
            const Vec2d o = n * ((double(s.line) + scan_phase) * step);
            Polygon rect({ to_point(o + u * lo - n * half_step), to_point(o + u * hi - n * half_step),
                           to_point(o + u * hi + n * half_step), to_point(o + u * lo + n * half_step) });
            const double t     = unscale<double>((s.b - s.a).cast<double>().norm());
            const TLevel level = is_anchor(s.a) && is_anchor(s.b) ? span_level(t, params) : TLevel::None;
            if ((level == TLevel::May || level == TLevel::Should || level == TLevel::Must) &&
                ! intersection(Polygons{ rect }, cp).empty())
                add_cuts(s.a, u, t, params, out.cuts);
            strips[level_index(level)].emplace_back(std::move(rect));
        }

        Polygons all;
        for (const Polygons &s : strips)
            append(all, s);
        all = union_(all);
        Polygons unclassified = diff(cp, all);
        if (! unclassified.empty()) {
            // A line just outside D leaves a band of up to ½ spacing along boundaries parallel to θ: C' within one
            // spacing of a strip takes that strip's level (worst wins); farther C' has no known span.
            for (Polygons &s : strips)
                if (! s.empty())
                    append(s, intersection(unclassified, offset(s, float(step))));
            unclassified = diff(unclassified, offset(all, float(step)));
        }
        for (int l = 0; l < 4; ++ l) {
            append(bad[l], unclassified);
            for (int worse = l + 1; worse < 5; ++ worse)
                append(bad[l], strips[worse]);
        }
    }

    std::array<Polygons, 4> good;
    for (int l = 0; l < 4; ++ l)
        good[l] = diff(cp, bad[l]);
    fill_regions(cp, good, out);
}

// ---------------------------------------------------------------------------------------------------------------
// Thin fronts: the chord rule (D27, review 1 m6). A free piece Π of ∂C' gets t = |chord| if
//   - both its ends touch an anchor (not a "neither" boundary),
//   - it deviates from its chord by at most one external width, and
//   - both ends are closed: the anchor leaves each end at ≥ 45° to the chord (a side wall). A strip anchored only
//     along its long side (hole strip, lean-to with an open end) has an anchor parallel to the chord there.
// C' within chord_reach_widths·ext_w of Π takes its level; the best qualifying piece counts (each free edge is
// printed as its own perimeter).

struct AnchorIndex {
    const Polylines                                      *polylines = nullptr;
    std::vector<std::pair<size_t, size_t>>                owner;     // line → (polyline, segment)
    std::unique_ptr<AABBTreeLines::LinesDistancer<Line>>  distancer;
};

// Does the anchor through the foot point leave the end point e at ≥ 45° to the chord?
bool end_closed(const AnchorIndex &anchors, size_t line_idx, const Vec2d &foot, const Point &e, const Vec2d &chord_dir)
{
    const auto    [pl_idx, seg] = anchors.owner[line_idx];
    const Points &pts           = (*anchors.polylines)[pl_idx].points;
    // Walk away from e: towards the polyline end farther from the foot point (in arc length).
    double to_front = (foot - pts[seg].cast<double>()).norm();
    for (size_t k = 0; k < seg; ++ k)
        to_front += (pts[k + 1] - pts[k]).cast<double>().norm();
    double to_back = (pts[seg + 1].cast<double>() - foot).norm();
    for (size_t k = seg + 1; k + 1 < pts.size(); ++ k)
        to_back += (pts[k + 1] - pts[k]).cast<double>().norm();
    const double probe = scaled<double>(closure_probe_mm);
    const Vec2d  ed    = e.cast<double>();
    Vec2d        dir   = Vec2d::Zero();
    if (to_back >= to_front) {
        for (size_t k = seg + 1; k < pts.size(); ++ k)
            if ((dir = pts[k].cast<double>() - ed).norm() >= probe)
                break;
    } else {
        for (size_t k = seg + 1; k -- > 0;)
            if ((dir = pts[k].cast<double>() - ed).norm() >= probe)
                break;
    }
    const double len = dir.norm();
    return len > 0. && std::abs(cross2(dir / len, chord_dir)) >= std::sin(closure_min_deg * M_PI / 180.) - 1e-9;
}

void thin_tension(const Polygons &cp, const Anchors &anchors, const Params &params, TensionMap &out)
{
    std::array<Polygons, 4> good;
    Polylines               anchor_pls = anchors.material;
    append(anchor_pls, anchors.support);
    AnchorIndex index;
    index.polylines = &anchor_pls;
    Lines lines;
    for (size_t i = 0; i < anchor_pls.size(); ++ i)
        for (size_t k = 0; k + 1 < anchor_pls[i].points.size(); ++ k) {
            lines.emplace_back(anchor_pls[i].points[k], anchor_pls[i].points[k + 1]);
            index.owner.emplace_back(i, k);
        }
    if (! lines.empty() && ! anchors.free_edges.empty()) {
        index.distancer = std::make_unique<AABBTreeLines::LinesDistancer<Line>>(lines);
        const double touch   = scaled<double>(touch_tol_mm);
        const double max_dev = scaled<double>(params.ext_width + edge_tol_mm);
        const float  reach   = scaled<float>(chord_reach_widths * params.ext_width);
        for (const Polyline &piece : anchors.free_edges) {
            if (piece.points.size() < 2 || piece.points.front() == piece.points.back())
                continue;                                   // closed free loop: t = ∞
            const Point &e0 = piece.points.front();
            const Point &e1 = piece.points.back();
            const auto [d0, li0, foot0] = index.distancer->distance_from_lines_extra<false>(e0);
            const auto [d1, li1, foot1] = index.distancer->distance_from_lines_extra<false>(e1);
            if (! (d0 <= touch && d1 <= touch))
                continue;                                   // an end at a "neither" boundary
            const Vec2d  chord = (e1 - e0).cast<double>();
            const double len   = chord.norm();
            if (len <= scaled<double>(params.ext_width))
                continue;
            const Line chord_line(e0, e1);
            if (std::any_of(piece.points.begin(), piece.points.end(),
                            [&chord_line, max_dev](const Point &p) { return chord_line.distance_to(p) > max_dev; }))
                continue;                                   // curved free edge: t = ∞
            const Vec2d dir = chord / len;
            if (! end_closed(index, li0, foot0, e0, dir) || ! end_closed(index, li1, foot1, e1, dir))
                continue;
            const double   t     = unscale<double>(len);
            const TLevel   level = span_level(t, params);
            const Polygons zone  = offset(piece, reach, ClipperLib::jtRound, scaled<double>(0.01), ClipperLib::etOpenRound);
            for (int l = level_index(level); l < 4; ++ l)
                append(good[l], zone);
            add_cuts(e0, dir, t, params, out.cuts);
        }
    }
    for (int l = 0; l < 4; ++ l)
        if (! good[l].empty())
            good[l] = intersection(cp, union_(good[l]));
    fill_regions(cp, good, out);
}

// ---------------------------------------------------------------------------------------------------------------
// classify_front helpers

// Samples every step along a closed polygon, starting at its first point.
void sample_closed(const Polygon &poly, double step, std::vector<Point> &out)
{
    const size_t n    = poly.points.size();
    double       pos  = 0.;
    double       next = 0.;
    for (size_t i = 0; i < n; ++ i) {
        const Vec2d  a   = poly.points[i].cast<double>();
        const Vec2d  b   = poly.points[(i + 1) % n].cast<double>();
        const double len = (b - a).norm();
        if (len <= 0.)
            continue;
        for (; next <= pos + len; next += step)
            out.emplace_back(to_point(a + (b - a) * ((next - pos) / len)));
        pos += len;
    }
}

// Calls fn(row, x_lo, x_hi) for each interval of the line y = (row + ½)·res inside the polygons (even-odd rule; the
// polygons do not overlap), for row0 ≤ row ≤ row1.
template<typename Fn> void scan_rows(const Polygons &polys, coord_t res, int row0, int row1, Fn &&fn)
{
    struct Edge { double y0, y1, x0, slope; };
    std::vector<Edge> edges;
    for (const Polygon &poly : polys)
        for (size_t i = 0; i < poly.points.size(); ++ i) {
            const Point &p = poly.points[i];
            const Point &q = poly.points[(i + 1) % poly.points.size()];
            if (p.y() == q.y())
                continue;
            const Point &lo = p.y() < q.y() ? p : q;
            const Point &hi = p.y() < q.y() ? q : p;
            edges.push_back({ double(lo.y()), double(hi.y()), double(lo.x()), double(hi.x() - lo.x()) / double(hi.y() - lo.y()) });
        }
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.y0 < b.y0 || (a.y0 == b.y0 && a.x0 < b.x0); });
    std::vector<const Edge*> active;
    std::vector<double>      xs;
    size_t                   next_edge = 0;
    for (int row = row0; row <= row1; ++ row) {
        const double y = (double(row) + 0.5) * double(res);
        while (next_edge < edges.size() && edges[next_edge].y0 <= y)
            active.push_back(&edges[next_edge ++]);
        active.erase(std::remove_if(active.begin(), active.end(), [y](const Edge *e) { return e->y1 <= y; }), active.end());
        xs.clear();
        for (const Edge *e : active)
            xs.push_back(e->x0 + (y - e->y0) * e->slope);
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
            fn(row, xs[i], xs[i + 1]);
    }
}

// Axis aligned rectangle [x0 − h, x1 + h] × [y − h, y + h].
Polygon run_rect(coord_t x0, coord_t x1, coord_t y, coord_t h)
{
    return Polygon({ { x0 - h, y - h }, { x1 + h, y - h }, { x1 + h, y + h }, { x0 - h, y + h } });
}

} // namespace

// ===============================================================================================================

Anchors compute_anchors(const ExPolygons &cprime, const ExPolygon &C, const ExPolygons &grounded_printable,
                        const ExPolygons &F, const ExPolygons &L)
{
    Anchors out;
    if (cprime.empty())
        return out;
    const float       tol      = scaled<float>(edge_tol_mm);
    const BoundingBox bbox     = get_extents(cprime).inflated(scaled<coord_t>(local_margin_mm));
    const Polygons    boundary = to_polygons(cprime);
    const Polygons    F_local  = local(F, bbox);
    const Polygons    F_grown  = expand(F_local, tol);
    // material = ∂C' ∩ expand(GP, 2 µm), support = ∂C' ∩ expand(F, 2 µm) (plan §3.8). GP contains L ∩ F, so both
    // would contain the boundary along the support contact: support wins there (ĉ = 0), the lists are disjoint.
    out.support  = intersection_pl(boundary, F_grown);
    out.material = intersection_pl(boundary, diff(expand(local(grounded_printable, bbox), tol), F_grown));
    // free = ∂C' − expand((L − C) ∪ F, 2 µm): boundary against open air. Boundary against ungrounded or unprintable
    // material is neither an anchor nor free (theory §2.2).
    Polygons other = diff(local(L, bbox), to_polygons(C));
    append(other, F_local);
    out.free_edges = diff_pl(boundary, expand(other, tol));
    canonicalize(out.material);
    canonicalize(out.support);
    canonicalize(out.free_edges);
    return out;
}

DomainEdges domain_edges(const ExPolygons &D_component, const ExPolygons &stop)
{
    Lines anchor_lines;
    Lines free_lines;
    if (! D_component.empty()) {
        const BoundingBox bbox     = get_extents(D_component).inflated(scaled<coord_t>(local_margin_mm));
        const Polygons    boundary = to_polygons(D_component);
        const Polygons    grown    = expand(local(stop, bbox), scaled<float>(edge_tol_mm));
        Polylines anchor = intersection_pl(boundary, grown);
        Polylines free_  = diff_pl(boundary, grown);
        canonicalize(anchor);
        canonicalize(free_);
        anchor_lines = to_lines(anchor);
        free_lines   = to_lines(free_);
    }
    DomainEdges out;
    out.anchor = std::make_shared<const AABBTreeLines::LinesDistancer<Line>>(std::move(anchor_lines));
    out.free_  = std::make_shared<const AABBTreeLines::LinesDistancer<Line>>(std::move(free_lines));
    return out;
}

TensionMap tension_levels(const ExPolygons &cprime, const FrontGeom &front, const ExPolygons &D_component,
                          const DomainEdges &edges, const Anchors &anchors, const Params &params)
{
    TensionMap out;
    if (cprime.empty())
        return out;
    const Polygons cp = to_polygons(cprime);
    if (front.wide)
        wide_tension(cprime, cp, front, D_component, edges, params, out);
    else
        thin_tension(cp, anchors, params, out);
    return out;
}

ExPolygons hoop_credit(const ExPolygons &cprime, const FrontGeom &front, bool grounded, const Params &params)
{
    // D26: only a grounded front with exactly one hole, that hole being the fully free slice hole found by Phase A
    // (hoop_hole), and only if Feret(H) ≤ B_ceil; the credit reaches B_max·hoop_width_factor from the hole.
    if (! grounded || cprime.empty() || front.C.holes.size() != 1 || front.hoop_hole != 0 ||
        front.hoop_feret_mm > params.user.B_ceil)
        return {};
    const double width = params.user.B_max * params.hoop_width_factor;
    if (width <= 0.)
        return {};
    Polygon hole = front.C.holes.front();
    hole.make_counter_clockwise();
    return intersection_ex(cprime, offset(hole, scaled<float>(width), ClipperLib::jtRound, scaled<double>(0.005)));
}

FrontNeeds classify_front(const ExPolygons &cprime, const ReachQuery &reach, const TensionMap &tension,
                          const ExPolygons &hoop_band, float max_chat, const Params &params)
{
    FrontNeeds out;
    Polygons   zero = to_polygons(tension.covered_all);
    append(zero, to_polygons(hoop_band));
    out.zero = union_ex(zero);
    if (cprime.empty())
        return out;

    const double K      = params.user.K_max;
    const double K_hard = params.f_hard * K;
    // Bending level (theory §2.5): 0 covered (c ≤ K), 1 should (c ≤ f_hard·K), 2 must. A non-finite c (no seed reaches
    // the point) counts as must.
    auto bend_level = [K, K_hard](float c) { return ! std::isfinite(c) ? 2 : double(c) <= K ? 0 : double(c) <= K_hard ? 1 : 2; };
    auto sample     = [&out](float c) {
        if (std::isfinite(c))
            out.max_c = std::max(out.max_c, c);
    };

    const BoundingBox bbox = get_extents(cprime);
    coord_t           res  = std::max<coord_t>(reach.res(), scaled<coord_t>(0.01));
    {
        const double cells = double(bbox.size().x()) * double(bbox.size().y()) / (double(res) * double(res));
        const double cap   = double(std::max<size_t>(params.grid_max_pixels, 1));
        if (cells > cap)
            res = coord_t(std::ceil(double(res) * std::sqrt(cells / cap)));
    }
    const coord_t half = res / 2;

    // Boundary samples every res along ∂C' (thin parts of C' contain no pixel centre).
    std::vector<Point> boundary_samples;
    for (const ExPolygon &ex : cprime) {
        sample_closed(ex.contour, double(res), boundary_samples);
        for (const Polygon &hole : ex.holes)
            sample_closed(hole, double(res), boundary_samples);
    }

    // Early exit (plan §3.8): c ≤ max ĉ + perimeter/2 + width ≤ K everywhere. Valid for a single simply connected C'
    // with an anchor on its contour; callers pass max_chat = +∞ otherwise. max_c then comes from ∂C' only.
    if (cprime.size() == 1 && cprime.front().holes.empty() && std::isfinite(max_chat)) {
        const double width     = unscale<double>(std::min(bbox.size().x(), bbox.size().y()));
        const double perimeter = unscale<double>(cprime.front().contour.length());
        if (double(max_chat) + 0.5 * perimeter + width <= K) {
            for (const Point &p : boundary_samples)
                sample(reach.value(p));
            return out;
        }
    }

    // Interior samples at the pixel centres ((i + ½)·res, (j + ½)·res) of a global lattice inside C' (independent of
    // the front's bbox); runs of equal level in a row become one rectangle.
    const Polygons          cp = to_polygons(cprime);
    std::array<Polygons, 3> squares;
    {
        const int row0 = int(std::ceil(double(bbox.min.y()) / double(res) - 0.5));
        const int row1 = int(std::floor(double(bbox.max.y()) / double(res) - 0.5));
        auto centre = [res](int i) { return coord_t(std::round((double(i) + 0.5) * double(res))); };
        scan_rows(cp, res, row0, row1, [&](int row, double x_lo, double x_hi) {
            const int     i0 = int(std::ceil(x_lo / double(res) - 0.5));
            const int     i1 = int(std::floor(x_hi / double(res) - 0.5));
            const coord_t y  = centre(row);
            int run_level = -1;
            int run_start = i0;
            for (int i = i0; i <= i1 + 1; ++ i) {
                int level = -1;
                if (i <= i1) {
                    const float c = reach.value(Point(centre(i), y));
                    sample(c);
                    level = bend_level(c);
                }
                if (level != run_level) {
                    if (run_level > 0)
                        squares[run_level].emplace_back(run_rect(centre(run_start), centre(i - 1), y, half));
                    run_level = level;
                    run_start = i;
                }
            }
        });
    }
    for (const Point &p : boundary_samples) {
        const float c = reach.value(p);
        sample(c);
        if (const int level = bend_level(c); level > 0)
            squares[level].emplace_back(run_rect(p.x(), p.x(), p.y(), half));
    }

    // Bending regions; where squares of both levels overlap, must wins.
    const Polygons b_must   = squares[2].empty() ? Polygons() : intersection(cp, union_(squares[2]));
    const Polygons b_should = squares[1].empty() ? Polygons() : diff(intersection(cp, union_(squares[1])), b_must);
    if (b_must.empty() && b_should.empty())
        return out;

    // Tension regions by level; points of C' in no finite region have t = ∞.
    std::array<Polygons, 5> t;
    for (const auto &[region, level] : tension.regions)
        append(t[level_index(level)], to_polygons(region));
    Polygons covered = t[level_index(TLevel::Covered)];
    append(covered, zero);
    const Polygons unc_should = diff(b_should, covered);
    const Polygons unc_must   = diff(b_must, covered);
    Polygons       unc_all    = unc_should;
    append(unc_all, unc_must);

    // Level = the lower of tension and bending; cause = the mechanism giving it, long span on a tie (theory §2.5).
    const int cs = 0, ls = 1;
    const Polygons &t_may    = t[level_index(TLevel::May)];
    const Polygons &t_should = t[level_index(TLevel::Should)];
    const Polygons &t_must   = t[level_index(TLevel::Must)];
    if (! t_may.empty())
        out.need[ls][int(Level::May)] = intersection_ex(unc_all, t_may);
    if (! t_should.empty())
        out.need[ls][int(Level::Should)] = intersection_ex(unc_all, t_should);
    if (! t_must.empty())
        out.need[ls][int(Level::Must)] = intersection_ex(unc_must, t_must);
    Polygons finite = t_may;
    append(finite, t_should);
    out.need[cs][int(Level::Should)] = diff_ex(unc_should, finite);       // tension must or none
    append(finite, t_must);
    out.need[cs][int(Level::Must)] = diff_ex(unc_must, finite);           // no finite span
    return out;
}

} // namespace Slic3r::SmartSupports
