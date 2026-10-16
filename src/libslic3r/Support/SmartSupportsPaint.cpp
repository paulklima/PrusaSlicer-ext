// Smart support painting: prism painting of the placements (plan §4, D17; WP1b).
//
// The placements of one cause are turned into one prism region per layer slab,
//     R_k = (∪_{|j−k| ≤ k_d} P_j) ⊕ e,   e = Params::edge_limit,   k_d = max(1, ⌈e / h_min⌉),
// and painted onto the downward facing facets of every model part through TriangleSelector::PredicateCursor with
// the cause's paint rule (tool_paint_rule(): manual paint is kept, the cause bit is OR-ed into auto states).
//
// Slabs mirror the slicer's enforcer projection (slice_mesh_slabs() as called by
// PrintObject::project_and_append_custom_facets()): a downward facet piece between the slicing planes slice_z_{k−1}
// and slice_z_k is projected onto layer k (layer 0: everything below slice_z_0; nothing above the topmost plane);
// a horizontal facet exactly on a slicing plane belongs to the layer above. Hence slab k = [slice_z_{k−1}, slice_z_k),
// compared in float as the slicer does.
//
// The triangle predicate is sound: a (sub)triangle is painted as a whole only if its piece in every slab it crosses
// lies inside that slab's R_k (exact test: no boundary segment of R_k within 10 nm of the convex piece and one point
// of it inside). Partially covered triangles are split down to the edge limit e; leaves still crossing a boundary
// stay unpainted. Therefore
//   - upper bound: the projection of the paint onto layer k lies inside R_k;
//   - lower bound: every point of P_k on a downward facet of slab k is painted (a leaf touching P_k has a diameter
//     ≤ e and spans at most k_d slab boundaries, so all its slab pieces lie in P_k ⊕ e ⊂ R_j, |j − k| ≤ k_d).

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <memory>

#include <boost/log/trivial.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::SmartSupports {

namespace {

// A boundary of R_k closer than this (scaled, 10 nm) to a triangle piece counts as crossing it (conservative).
constexpr double TOUCH_TOLERANCE = 10.;
// Facets whose normalised world normal has z ≥ −this are not downward facing: never painted.
constexpr double DOWNWARD_NZ = 1e-4;
// Smallest edge limit handed to the selector (a zero limit would split forever).
constexpr double MIN_EDGE_LIMIT = 0.01;
// The predicate calls cancel() every that many triangle classifications (plan §2.3).
constexpr size_t CANCEL_INTERVAL = 4096;
// Size guard (plan §4): more painted leaves than that many times the seed facets are logged.
constexpr size_t SIZE_GUARD_FACTOR = 20;

using LineTree = AABBTreeIndirect::Tree<2, coord_t>;

// Prism region R_k of one slab.
struct Slab {
    Lines       lines;          // boundary of R_k (contours and holes)
    LineTree    tree;           // over lines
    BoundingBox bbox;           // of R_k
    bool empty() const { return lines.empty(); }
};

// Prism regions of one cause, one slab per layer.
struct CauseRegion {
    uint8_t           bit = 0;
    std::vector<Slab> slabs;    // size in.layers.size()
};

// Slicing planes as the slicer sees them (zs_from_layers()).
std::vector<float> slicing_planes(const AnalysisInput &in)
{
    std::vector<float> out;
    out.reserve(in.layers.size());
    for (const LayerInput &layer : in.layers)
        out.emplace_back(float(layer.slice_z));
    return out;
}

// Slab of a point at height z: k with planes[k − 1] ≤ z < planes[k]; planes.size() above the topmost plane.
inline size_t slab_of(const std::vector<float> &planes, float z)
{
    return size_t(std::upper_bound(planes.begin(), planes.end(), z) - planes.begin());
}

// Slabs [first, last] in which a triangle with the z extent [zmin, zmax] has a piece of positive height (a horizontal
// triangle: its single slab). last == planes.size(): a piece above the topmost plane (never projected by the slicer).
inline std::pair<size_t, size_t> slab_range(const std::vector<float> &planes, float zmin, float zmax)
{
    if (zmin == zmax) {
        const size_t k = slab_of(planes, zmin);
        return { k, k };
    }
    return { size_t(std::upper_bound(planes.begin(), planes.end(), zmin) - planes.begin()),
             size_t(std::lower_bound(planes.begin(), planes.end(), zmax) - planes.begin()) };
}

double min_layer_height(const AnalysisInput &in)
{
    double h_min = std::numeric_limits<double>::max();
    for (const LayerInput &layer : in.layers)
        if (layer.height > 0.)
            h_min = std::min(h_min, layer.height);
    if (h_min == std::numeric_limits<double>::max())
        h_min = in.params.layer_height > 0. ? in.params.layer_height : 0.2;
    return h_min;
}

// R_k of every cause in `causes` with a paintable placement of level ≥ lowest. Parallel over layers, each layer writes
// its own slot (deterministic).
std::vector<CauseRegion> build_regions(const AnalysisInput &in, const std::vector<Placement> &placements, uint8_t causes,
                                       Level lowest, const ThrowOnCancel &cancel)
{
    const size_t num_layers = in.layers.size();
    const double e          = std::max(0., in.params.edge_limit);
    const int    k_d        = std::max(1, int(std::ceil(e / min_layer_height(in) - 1e-9)));

    std::vector<CauseRegion> out;
    for (const uint8_t bit : { uint8_t(cbIsland), uint8_t(cbCantilever) }) {
        if ((causes & bit) == 0)
            continue;
        std::vector<ExPolygons> P(num_layers);
        bool any = false;
        for (const Placement &placement : placements)
            if (placement.paintable && cause_bit(placement.cause) == bit && placement.level >= lowest &&
                placement.layer < num_layers && ! placement.area.empty()) {
                append(P[placement.layer], placement.area);
                any = true;
            }
        if (! any)
            continue;
        CauseRegion &region = out.emplace_back();
        region.bit = bit;
        region.slabs.resize(num_layers);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_layers), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t k = range.begin(); k < range.end(); ++ k) {
                ExPolygons src;
                for (size_t j = size_t(std::max(0, int(k) - k_d)); j < std::min(num_layers, k + size_t(k_d) + 1); ++ j)
                    append(src, P[j]);
                if (src.empty())
                    continue;
                cancel();
                ExPolygons R = union_ex(src);
                if (e > 0.)
                    R = offset_ex(R, float(scaled<double>(e)));
                if (R.empty())
                    continue;
                Slab &slab = region.slabs[k];
                slab.lines = to_lines(R);
                slab.tree  = AABBTreeLines::build_aabb_tree_over_indexed_lines(slab.lines);
                slab.bbox  = get_extents(R);
            }
        });
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Exact geometry of a convex piece (≤ 5 vertices, scaled XY) against the boundary segments of R_k.

struct Piece {
    std::array<Vec2d, 6> p;
    int                  n = 0;
};

inline double cross2d(const Vec2d &a, const Vec2d &b) { return a.x() * b.y() - a.y() * b.x(); }

inline double point_segment_distance_sq(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + t * ab - p).squaredNorm();
}

inline double segment_segment_distance_sq(const Vec2d &a, const Vec2d &b, const Vec2d &c, const Vec2d &d)
{
    const double d1 = cross2d(b - a, c - a), d2 = cross2d(b - a, d - a);
    const double d3 = cross2d(d - c, a - c), d4 = cross2d(d - c, b - c);
    if (((d1 > 0. && d2 < 0.) || (d1 < 0. && d2 > 0.)) && ((d3 > 0. && d4 < 0.) || (d3 < 0. && d4 > 0.)))
        return 0.;   // proper crossing
    return std::min(std::min(point_segment_distance_sq(a, c, d), point_segment_distance_sq(b, c, d)),
                    std::min(point_segment_distance_sq(c, a, b), point_segment_distance_sq(d, a, b)));
}

// Is p inside the counter-clockwise convex polygon q or within TOUCH_TOLERANCE of it?
inline bool point_in_convex(const Vec2d &p, const Piece &q)
{
    for (int i = 0; i < q.n; ++ i) {
        const Vec2d &a   = q.p[i];
        const Vec2d  e   = q.p[(i + 1) % q.n] - a;
        const double len = e.norm();
        if (len > 0. && cross2d(e, p - a) < - TOUCH_TOLERANCE * len)
            return false;
    }
    return true;
}

// Does the segment ab come within TOUCH_TOLERANCE of the convex piece q (interior or boundary)?
inline bool segment_touches_piece(const Vec2d &a, const Vec2d &b, const Piece &q, bool oriented)
{
    if (oriented && (point_in_convex(a, q) || point_in_convex(b, q)))
        return true;
    const double tol2 = TOUCH_TOLERANCE * TOUCH_TOLERANCE;
    if (q.n == 1)
        return point_segment_distance_sq(q.p[0], a, b) <= tol2;
    for (int i = 0; i < q.n; ++ i)
        if (segment_segment_distance_sq(a, b, q.p[i], q.p[(i + 1) % q.n]) <= tol2)
            return true;
    return false;
}

enum class Rel : uint8_t { Outside, Inside, Partial };

// Position of a convex piece (absolute scaled XY) relative to the region of the slab.
Rel classify_piece(const Slab &slab, const Piece &abs_piece)
{
    if (slab.empty() || abs_piece.n == 0)
        return Rel::Outside;
    Vec2d lo = abs_piece.p[0], hi = abs_piece.p[0];
    for (int i = 1; i < abs_piece.n; ++ i) {
        lo = lo.cwiseMin(abs_piece.p[i]);
        hi = hi.cwiseMax(abs_piece.p[i]);
    }
    const Point bb_min(coord_t(std::floor(lo.x() - TOUCH_TOLERANCE)), coord_t(std::floor(lo.y() - TOUCH_TOLERANCE)));
    const Point bb_max(coord_t(std::ceil(hi.x() + TOUCH_TOLERANCE)), coord_t(std::ceil(hi.y() + TOUCH_TOLERANCE)));
    if (! BoundingBox(bb_min, bb_max).overlap(slab.bbox))
        return Rel::Outside;

    // Local frame: relative coordinates keep the double cross products accurate.
    const Point origin(coord_t(std::floor(abs_piece.p[0].x())), coord_t(std::floor(abs_piece.p[0].y())));
    const Vec2d origin_d = origin.cast<double>();
    Piece  q;
    double area2 = 0.;
    q.n = abs_piece.n;
    for (int i = 0; i < q.n; ++ i)
        q.p[i] = abs_piece.p[i] - origin_d;
    for (int i = 0; i < q.n; ++ i)
        area2 += cross2d(q.p[i], q.p[(i + 1) % q.n]);
    if (area2 < 0.)
        std::reverse(q.p.begin(), q.p.begin() + q.n);
    const bool oriented = q.n >= 3 && area2 != 0.;

    bool touches = false;
    const Eigen::AlignedBox<coord_t, 2> box(bb_min, bb_max);
    AABBTreeIndirect::traverse(slab.tree, AABBTreeIndirect::intersecting(box), [&](const LineTree::Node &node) {
        const Line &l = slab.lines[node.idx];
        if (segment_touches_piece((l.a - origin).cast<double>(), (l.b - origin).cast<double>(), q, oriented)) {
            touches = true;
            return false;   // stop the traversal
        }
        return true;
    });
    if (touches)
        return Rel::Partial;
    // No boundary near the piece: it is entirely inside or entirely outside.
    const Point p0(coord_t(std::llround(abs_piece.p[0].x())), coord_t(std::llround(abs_piece.p[0].y())));
    const int   outside = AABBTreeLines::point_outside_closed_contours(slab.lines, slab.tree, p0);
    return outside < 0 ? Rel::Inside : outside > 0 ? Rel::Outside : Rel::Partial;
}

// Part of the triangle with z in [lo, hi] (infinite bounds allowed), projected to scaled XY.
void clip_triangle_to_slab(const std::array<Vec3d, 3> &tri, double lo, double hi, Piece &out)
{
    std::array<Vec3d, 6> a, b;
    int na = 3;
    std::copy(tri.begin(), tri.end(), a.begin());
    // Sutherland–Hodgman against one horizontal plane.
    auto clip = [](const std::array<Vec3d, 6> &in, int nin, std::array<Vec3d, 6> &res, double zc, bool keep_above) {
        int nout = 0;
        for (int i = 0; i < nin; ++ i) {
            const Vec3d &p   = in[i];
            const Vec3d &q   = in[(i + 1) % nin];
            const bool   pin = keep_above ? p.z() >= zc : p.z() <= zc;
            const bool   qin = keep_above ? q.z() >= zc : q.z() <= zc;
            if (pin)
                res[nout ++] = p;
            if (pin != qin)
                res[nout ++] = p + ((zc - p.z()) / (q.z() - p.z())) * (q - p);
        }
        return nout;
    };
    if (std::isfinite(lo)) {
        na = clip(a, na, b, lo, true);
        a  = b;
    }
    if (std::isfinite(hi) && na > 0) {
        na = clip(a, na, b, hi, false);
        a  = b;
    }
    out.n = na;
    for (int i = 0; i < na; ++ i)
        out.p[i] = Vec2d(a[i].x() / SCALING_FACTOR, a[i].y() / SCALING_FACTOR);
}

// The sound per-slab triangle predicate of one cause (plan §4). One instance per select_patch(), single threaded.
class PrismPredicate
{
public:
    PrismPredicate(const std::vector<float> &planes, const CauseRegion &region, const ThrowOnCancel &cancel)
        : m_planes(planes), m_slabs(region.slabs), m_cancel(cancel) {}

    bool point_inside(const Vec3f &p) const
    {
        const size_t k = slab_of(m_planes, p.z());
        if (k >= m_slabs.size() || m_slabs[k].empty())
            return false;
        const Slab &slab = m_slabs[k];
        const Point pt(coord_t(std::llround(double(p.x()) / SCALING_FACTOR)), coord_t(std::llround(double(p.y()) / SCALING_FACTOR)));
        return slab.bbox.contains(pt) && AABBTreeLines::point_outside_closed_contours(slab.lines, slab.tree, pt) < 0;
    }

    // 3: every slab piece lies inside its R_k (the whole triangle is painted); 1: some piece touches an R_k without
    // all being inside (the selector splits further); 0: no piece touches any R_k.
    int classify(const std::array<Vec3f, 3> &tri)
    {
        if (++ m_calls % CANCEL_INTERVAL == 0)
            m_cancel();
        m_last_tri    = tri;
        m_last_result = this->classify_impl(tri);
        return m_last_result;
    }

    // The selector asks only after classify() returned 0 for the same triangle.
    bool touches(const std::array<Vec3f, 3> &tri)
    {
        if (tri != m_last_tri)
            this->classify(tri);
        return m_last_result != 0;
    }

private:
    int classify_impl(const std::array<Vec3f, 3> &tri) const
    {
        if (m_slabs.empty())
            return 0;
        const float zmin = std::min({ tri[0].z(), tri[1].z(), tri[2].z() });
        const float zmax = std::max({ tri[0].z(), tri[1].z(), tri[2].z() });
        const auto [first, last_or_above] = slab_range(m_planes, zmin, zmax);
        const size_t last = std::min(last_or_above, m_slabs.size() - 1);
        if (first > last)
            return 0;   // only above the topmost slicing plane: never projected
        const std::array<Vec3d, 3> t { tri[0].cast<double>(), tri[1].cast<double>(), tri[2].cast<double>() };
        const double inf = std::numeric_limits<double>::infinity();
        bool  all_inside = true, any = false;
        Piece piece;
        for (size_t k = first; k <= last; ++ k) {
            if (zmin == zmax) {
                piece.n = 3;
                for (int i = 0; i < 3; ++ i)
                    piece.p[i] = Vec2d(t[i].x() / SCALING_FACTOR, t[i].y() / SCALING_FACTOR);
            } else
                clip_triangle_to_slab(t, k == 0 ? -inf : double(m_planes[k - 1]), double(m_planes[k]), piece);
            const Rel rel = classify_piece(m_slabs[k], piece);
            all_inside &= rel == Rel::Inside;
            any        |= rel != Rel::Outside;
            if (any && ! all_inside)
                return 1;
        }
        // A piece above the topmost plane is not projected by the slicer: it neither blocks nor touches.
        return all_inside ? 3 : any ? 1 : 0;
    }

    const std::vector<float> &m_planes;
    const std::vector<Slab>  &m_slabs;
    const ThrowOnCancel      &m_cancel;
    size_t                    m_calls = 0;
    std::array<Vec3f, 3>      m_last_tri { Vec3f(NAN, NAN, NAN), Vec3f(NAN, NAN, NAN), Vec3f(NAN, NAN, NAN) };
    int                       m_last_result = 0;
};

// Downward facing original facet with the slabs it crosses and its scaled XY bbox (PO frame).
struct DownwardFacet {
    int         facet = 0;
    size_t      first = 0, last = 0;
    BoundingBox bbox;
};

std::vector<DownwardFacet> downward_facets(const VolumeInput &volume, const std::vector<float> &planes)
{
    std::vector<DownwardFacet> out;
    if (planes.empty())
        return out;
    const indexed_triangle_set &its      = volume.mesh->its;
    const Transform3f           trafo_f  = volume.trafo.cast<float>();   // as the cursor transforms the vertices
    const Matrix3d              normal_m = volume.trafo.linear().inverse().transpose();
    for (size_t facet = 0; facet < its.indices.size(); ++ facet) {
        const stl_triangle_vertex_indices &f = its.indices[facet];
        const Vec3d v0 = its.vertices[f(0)].cast<double>(), v1 = its.vertices[f(1)].cast<double>(), v2 = its.vertices[f(2)].cast<double>();
        // Outward world normal M⁻ᵀ·n, valid for mirroring transformations as well (the slicer's orientation test
        // multiplies by the sign of the determinant, slice_mesh_slabs()).
        const Vec3d  n   = normal_m * (v1 - v0).cross(v2 - v0);
        const double len = n.norm();
        if (! (len > 0.) || n.z() >= - DOWNWARD_NZ * len)
            continue;
        const std::array<Vec3f, 3> w { trafo_f * its.vertices[f(0)], trafo_f * its.vertices[f(1)], trafo_f * its.vertices[f(2)] };
        const auto [first, last_or_above] = slab_range(planes, std::min({ w[0].z(), w[1].z(), w[2].z() }),
                                                               std::max({ w[0].z(), w[1].z(), w[2].z() }));
        const size_t last = std::min(last_or_above, planes.size() - 1);
        if (first > last)
            continue;
        DownwardFacet &df = out.emplace_back();
        df.facet = int(facet);
        df.first = first;
        df.last  = last;
        for (const Vec3f &p : w)
            df.bbox.merge(Point(coord_t(std::llround(double(p.x()) / SCALING_FACTOR)), coord_t(std::llround(double(p.y()) / SCALING_FACTOR))));
        df.bbox.offset(coord_t(TOUCH_TOLERANCE) + 1);
    }
    return out;
}

size_t count_auto_leaves(const TriangleSelector &selector)
{
    size_t out = 0;
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        out += size_t(selector.num_facets(auto_state(mask)));
    return out;
}

// Plan §4 for one model part on its own selector.
VolumePaint paint_volume(const VolumeInput &volume, const std::vector<CauseRegion> &regions, const std::vector<float> &planes,
                         float edge_limit, const ThrowOnCancel &cancel)
{
    VolumePaint out { volume.volume_id, volume.paint, false };
    if (! volume.mesh || volume.mesh->its.indices.empty())
        return out;
    cancel();

    // Seeds per cause: downward facets whose bbox overlaps a non-empty R_k of a slab they cross.
    std::vector<std::vector<int>> seeds(regions.size());
    bool any_seed = false;
    if (! regions.empty()) {
        const std::vector<DownwardFacet> downward = downward_facets(volume, planes);
        for (size_t r = 0; r < regions.size(); ++ r) {
            for (const DownwardFacet &df : downward)
                for (size_t k = df.first; k <= df.last; ++ k) {
                    const Slab &slab = regions[r].slabs[k];
                    if (! slab.empty() && slab.bbox.overlap(df.bbox)) {
                        seeds[r].emplace_back(df.facet);
                        break;
                    }
                }
            any_seed |= ! seeds[r].empty();
        }
    }
    if (! any_seed && volume.paint.triangles_to_split.empty())
        return out;   // no paint before, nothing to paint now

    cancel();
    TriangleSelector selector(*volume.mesh);
    selector.deserialize(volume.paint, false);
    // A run replaces all smart paint: clear every cause, keep the manual paint.
    selector.remap_states(clear_causes_map(cbAll));

    Transform3d linear = Transform3d::Identity();
    linear.linear()    = volume.trafo.linear();
    size_t num_seeds = 0;
    for (size_t r = 0; r < regions.size(); ++ r) {
        if (seeds[r].empty())
            continue;
        cancel();
        num_seeds += seeds[r].size();
        auto predicate = std::make_shared<PrismPredicate>(planes, regions[r], cancel);
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = [predicate](const Vec3f &p) { return predicate->point_inside(p); };
        predicates.classify     = [predicate](const std::array<Vec3f, 3> &tri) { return predicate->classify(tri); };
        predicates.touches      = [predicate](const std::array<Vec3f, 3> &tri) { return predicate->touches(tri); };
        const int facet_start   = seeds[r].front();
        predicates.seed_facets  = std::move(seeds[r]);
        const uint8_t bit = regions[r].bit;
        const PaintRuleScope rule(selector, tool_paint_rule(bit));          // cleared on a cancel too
        selector.select_patch(facet_start,
                              std::make_unique<TriangleSelector::PredicateCursor>(std::move(predicates), edge_limit, volume.trafo),
                              auto_state(bit), linear, true, 89.99f);
    }

    out.data    = selector.serialize();
    out.changed = ! FacetsAnnotation::same_paint(volume.paint, out.data);
    if (num_seeds > 0) {
        const size_t leaves = count_auto_leaves(selector);
        if (leaves > SIZE_GUARD_FACTOR * num_seeds)
            BOOST_LOG_TRIVIAL(info) << "Smart support painting: volume " << volume.volume_id.id << ": " << leaves
                                    << " painted leaves on " << num_seeds << " seed facets";
    }
    return out;
}

} // namespace

std::vector<VolumePaint> paint_placements(const AnalysisInput &in, const std::vector<Placement> &placements, uint8_t causes,
                                          Level lowest, const ThrowOnCancel &cancel)
{
    const ThrowOnCancel throw_on_cancel = cancel ? cancel : ThrowOnCancel([]() {});
    // v1 paints islands and cantilevers only (long spans are planned and reported, D7).
    const uint8_t paint_causes = uint8_t(causes & (cbIsland | cbCantilever));

    const std::vector<float>       planes  = slicing_planes(in);
    const std::vector<CauseRegion> regions = planes.empty() ? std::vector<CauseRegion>() :
        build_regions(in, placements, paint_causes, lowest, throw_on_cancel);
    const float edge_limit = float(std::max(MIN_EDGE_LIMIT, in.params.edge_limit));

    std::vector<VolumePaint> out(in.volumes.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, in.volumes.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx)
            out[idx] = paint_volume(in.volumes[idx], regions, planes, edge_limit, throw_on_cancel);
    });
    return out;
}

} // namespace Slic3r::SmartSupports
