// Smart support painting: accumulated reach c(p) — ReachField, ReachCarry, segment minimiser
// (theory §2.3; plan §3.8, D11; WP2b1).
//
// c_n(p) = min over seeds a of ( max(0, dist(p, a) − Δ) + ĉ(a) ),  Δ = w_β − w_n (0 for β = α).
// The seeds are anchor segments with ĉ linear between their end points. dist is geodesic inside the D component:
// - exact zone: within ρ of the seeds the Euclidean minimum over the seed segments is exact (closed form);
// - Direct mode (all of C' inside the exact zone): the Euclidean minimum over all seeds, no raster;
// - Grid mode: 16-neighbour Dijkstra over the conservative raster of the domain, seeded with the exact values of the
//   cells inside the exact zone; a query takes the minimum of the exact zone and the 4×4 cell neighbourhood
//   (cell value + straight distance), with the Euclidean minimum over all seeds as the fallback for points the raster
//   does not reach, so a grounded C' never gets +∞.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

namespace {

constexpr float  INF_F = std::numeric_limits<float>::infinity();
constexpr double INF_D = std::numeric_limits<double>::infinity();

// min over s ∈ [0, len] of c0 + g·s + max(0, √(d² + (s − sp)²) − Δ), all in mm. f is convex, so its minimum lies at an
// end of the segment, at the smooth stationary point sp − g·d/√(1 − g²) (|g| < 1), at a kink √(d² + (s − sp)²) = Δ
// or at the foot point sp (flat part, g = 0). Optionally returns the Euclidean distance of p to the segment.
double segment_min_mm(const Vec2d &a, const Vec2d &b, double c0, double c1, const Vec2d &p, double delta, double *dist = nullptr)
{
    const Vec2d  ab  = b - a;
    const Vec2d  ap  = p - a;
    const double len = ab.norm();
    if (len < 1e-9) {
        const double d = ap.norm();
        if (dist)
            *dist = d;
        return std::min(c0, c1) + std::max(0., d - delta);
    }
    const Vec2d  u  = ab / len;
    const double sp = ap.dot(u);
    const double d  = std::abs(u.x() * ap.y() - u.y() * ap.x());
    const double g  = (c1 - c0) / len;
    if (dist) {
        const double ds = std::clamp(sp, 0., len) - sp;
        *dist = std::sqrt(d * d + ds * ds);
    }
    auto f = [&](double s) {
        const double ds = s - sp;
        return c0 + g * s + std::max(0., std::sqrt(d * d + ds * ds) - delta);
    };
    double best      = std::min(f(0.), f(len));
    auto   candidate = [&](double s) {
        if (s > 0. && s < len)
            best = std::min(best, f(s));
    };
    candidate(sp);
    if (std::abs(g) < 1.)
        candidate(sp - g * d / std::sqrt(1. - g * g));
    if (delta > d) {
        const double k = std::sqrt(delta * delta - d * d);
        candidate(sp - k);
        candidate(sp + k);
    }
    return best;
}

// Seed segments with an AABB tree, shared by a field and its cropped copies. The tree is built over double lines in mm:
// radius and nearest queries of LinesDistancer<Line> (integer boxes) do not prune and scan every line (measured ~20 µs
// per query on 1800 lines against 0.25 µs for Linef).
struct SeedSet
{
    std::vector<AnchorSeg>               segs;
    AABBTreeLines::LinesDistancer<Linef> distancer;
    std::vector<Vec2d>                  a, b;       // end points in mm
    double                              c_min = 0.; // lowest ĉ of all seeds
    Vec2d                               bbox_center = Vec2d::Zero();   // mm
    double                              bbox_radius = 0.;              // half diagonal, mm

    explicit SeedSet(std::vector<AnchorSeg> &&seeds) : segs(std::move(seeds)), distancer(lines_of(segs))
    {
        a.reserve(segs.size());
        b.reserve(segs.size());
        c_min = segs.empty() ? 0. : INF_D;
        BoundingBoxf bbox;
        for (const AnchorSeg &seg : segs) {
            a.emplace_back(unscale(seg.l.a));
            b.emplace_back(unscale(seg.l.b));
            bbox.merge(a.back());
            bbox.merge(b.back());
            c_min = std::min(c_min, double(std::min(seg.c0, seg.c1)));
        }
        if (bbox.defined) {
            bbox_center = bbox.center();
            bbox_radius = 0.5 * bbox.size().norm();
        }
    }

    static Linesf lines_of(const std::vector<AnchorSeg> &segs)
    {
        Linesf lines;
        lines.reserve(segs.size());
        for (const AnchorSeg &seg : segs)
            lines.emplace_back(unscale(seg.l.a), unscale(seg.l.b));
        return lines;
    }

    double eval(size_t i, const Vec2d &p, double delta, double *dist = nullptr) const
    {
        return segment_min_mm(a[i], b[i], double(segs[i].c0), double(segs[i].c1), p, delta, dist);
    }

    // Minimum over the seeds within radius_mm of p (the exact zone); +∞ if there is none.
    double near_min(const Point &p, double radius_mm, double delta) const
    {
        double      best = INF_D;
        const Vec2d pm   = unscale(p);
        for (size_t i : distancer.all_lines_in_radius(pm, radius_mm))
            best = std::min(best, eval(i, pm, delta));
        return best;
    }

    // Euclidean minimum over all seeds. The search radius grows until a seed is found; a seed at distance d_s has a value
    // ≥ c_min + d_s − Δ, so then only the seeds within (best − c_min + Δ) can still be better. Radius queries only: the
    // nearest-line query descends into every node whose box contains the point (most of the tree inside a ring).
    double euclidean_min(const Point &p, double delta) const
    {
        if (segs.empty())
            return INF_D;
        const Vec2d pm     = unscale(p);
        const double r_max = (pm - bbox_center).norm() + bbox_radius + 0.001;
        double       best  = INF_D;
        double       r     = 0.5;
        for (;; r = std::min(4. * r, r_max)) {
            for (size_t i : distancer.all_lines_in_radius(pm, r))
                best = std::min(best, eval(i, pm, delta));
            if (best < INF_D || r >= r_max)
                break;
        }
        if (const double r2 = best - c_min + delta; r2 > r)
            for (size_t i : distancer.all_lines_in_radius(pm, r2 + 1e-6))
                best = std::min(best, eval(i, pm, delta));
        return best;
    }
};

// Indexed binary min-heap over the cells of a grid, keyed by (v, index): decrease-key keeps the memory at
// two int32 per cell however often a cell improves.
class CellHeap
{
public:
    explicit CellHeap(const std::vector<float> &key) : m_key(key), m_pos(key.size(), -1) {}
    bool    empty() const { return m_heap.empty(); }
    bool    settled(int32_t i) const { return m_pos[i] == SETTLED; }
    void    push_or_decrease(int32_t i)
    {
        if (m_pos[i] < 0) {
            m_pos[i] = int32_t(m_heap.size());
            m_heap.push_back(i);
        }
        sift_up(size_t(m_pos[i]));
    }
    int32_t pop()
    {
        const int32_t top = m_heap.front();
        m_pos[top]        = SETTLED;
        const int32_t last = m_heap.back();
        m_heap.pop_back();
        if (! m_heap.empty()) {
            m_heap.front() = last;
            m_pos[last]    = 0;
            sift_down(0);
        }
        return top;
    }

private:
    static constexpr int32_t SETTLED = -2;
    bool less(int32_t l, int32_t r) const { return m_key[l] < m_key[r] || (m_key[l] == m_key[r] && l < r); }
    void place(size_t at, int32_t i) { m_heap[at] = i; m_pos[i] = int32_t(at); }
    void sift_up(size_t at)
    {
        const int32_t i = m_heap[at];
        while (at > 0) {
            const size_t parent = (at - 1) / 2;
            if (! less(i, m_heap[parent]))
                break;
            place(at, m_heap[parent]);
            at = parent;
        }
        place(at, i);
    }
    void sift_down(size_t at)
    {
        const int32_t i = m_heap[at];
        for (;;) {
            size_t child = 2 * at + 1;
            if (child >= m_heap.size())
                break;
            if (child + 1 < m_heap.size() && less(m_heap[child + 1], m_heap[child]))
                ++ child;
            if (! less(m_heap[child], i))
                break;
            place(at, m_heap[child]);
            at = child;
        }
        place(at, i);
    }

    const std::vector<float> &m_key;
    std::vector<int32_t>      m_pos;    // heap position, −1 never queued, SETTLED popped
    std::vector<int32_t>      m_heap;
};

// 16-neighbour Dijkstra on grid.v over the active cells (grid.m != 0), starting from the finite cells. Steps 1, √2, √5;
// a knight step needs both cells its segment crosses (review 4 M2d), a diagonal step only its target.
void dijkstra16(Grid &grid, const ThrowOnCancel &cancel)
{
    struct Step { int dx, dy; float len; bool knight; int c0x, c0y, c1x, c1y; };   // knight: crossed cells relative to the source
    const float res = float(unscale<double>(grid.res));
    const float d1 = res, d2 = float(M_SQRT2) * res, d5 = float(std::sqrt(5.)) * res;
    const Step steps[16] = {
        {  1,  0, d1, false }, { -1,  0, d1, false }, {  0,  1, d1, false }, {  0, -1, d1, false },
        {  1,  1, d2, false }, { -1,  1, d2, false }, {  1, -1, d2, false }, { -1, -1, d2, false },
        {  2,  1, d5, true,  1,  0,  1,  1 }, { -2,  1, d5, true, -1,  0, -1,  1 },
        {  2, -1, d5, true,  1,  0,  1, -1 }, { -2, -1, d5, true, -1,  0, -1, -1 },
        {  1,  2, d5, true,  0,  1,  1,  1 }, { -1,  2, d5, true,  0,  1, -1,  1 },
        {  1, -2, d5, true,  0, -1,  1, -1 }, { -1, -2, d5, true,  0, -1, -1, -1 },
    };
    const int cols = grid.cols, rows = grid.rows;
    CellHeap  heap(grid.v);
    for (int32_t i = 0; i < int32_t(grid.v.size()); ++ i)
        if (grid.v[i] < INF_F)
            heap.push_or_decrease(i);
    size_t pops = 0;
    while (! heap.empty()) {
        const int32_t u  = heap.pop();
        const int     ux = u % cols;
        const int     uy = u / cols;
        const float   vu = grid.v[u];
        for (const Step &s : steps) {
            const int nx = ux + s.dx;
            const int ny = uy + s.dy;
            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows)
                continue;
            const int32_t n = ny * cols + nx;
            if (grid.m[n] == 0 || heap.settled(n))
                continue;
            if (s.knight && (grid.m[(uy + s.c0y) * cols + ux + s.c0x] == 0 || grid.m[(uy + s.c1y) * cols + ux + s.c1x] == 0))
                continue;
            if (const float nv = vu + s.len; nv < grid.v[n]) {
                grid.v[n] = nv;
                heap.push_or_decrease(n);
            }
        }
        if ((++ pops & 0xFFFF) == 0 && cancel)
            cancel();
    }
}

// All of C' lies within ρ of the seeds: offset with round joins and round ends at ρ − 1 µm (arc tolerance 1 µm), so the
// query at ρ + 1 µm always finds a seed (review 4 M3). Consecutive seeds are chained into polylines first.
bool inside_exact_zone(const ExPolygons &cprime, const std::vector<AnchorSeg> &segs, double rho)
{
    if (cprime.empty())
        return true;
    if (rho <= 0.002 || segs.empty())
        return false;
    Polylines lines;
    for (const AnchorSeg &seg : segs)
        if (! lines.empty() && lines.back().last_point() == seg.l.a)
            lines.back().points.push_back(seg.l.b);
        else
            lines.emplace_back(seg.l.a, seg.l.b);
    const Polygons zone = offset(lines, float(scaled<double>(rho - 0.001)), ClipperLib::jtRound, scaled<double>(0.001), ClipperLib::etOpenRound);
    double missing = 0.;
    for (const ExPolygon &piece : diff_ex(cprime, zone))
        missing += piece.area();
    return missing * SCALING_FACTOR * SCALING_FACTOR < 1e-6;
}

} // namespace

struct ReachField::Impl
{
    std::shared_ptr<const SeedSet> seeds;
    double             delta  = 0.;      // Δ = w_β − w_n, mm
    double             rho    = 0.;      // radius of the exact zone in Grid mode, mm
    bool               direct = true;
    coord_t            res    = 0;       // classification sample spacing: grid resolution, Direct: grid_res_max
    // Grid mode: values at the cell centres (+∞ outside the domain or not reached); cell (ix, iy) ↔ v[iy * cols + ix].
    Point              origin;
    int                cols = 0, rows = 0;
    std::vector<float> v;

    float value(const Point &p) const;
    void  build_grid(const ExPolygons &cprime, const ExPolygons &domain, const Params &params, const ThrowOnCancel &cancel);
};

float ReachField::Impl::value(const Point &p) const
{
    if (! seeds || seeds->segs.empty())
        return INF_F;
    if (direct)
        return float(seeds->euclidean_min(p, delta));
    double best = seeds->near_min(p, rho + 0.001, delta);
    if (cols > 0) {
        const double res_d = double(res);
        const double fx    = double(p.x() - origin.x()) / res_d - 0.5;
        const double fy    = double(p.y() - origin.y()) / res_d - 0.5;
        const int    ix0   = int(std::floor(fx)) - 1;
        const int    iy0   = int(std::floor(fy)) - 1;
        for (int iy = std::max(iy0, 0); iy < std::min(iy0 + 4, rows); ++ iy)
            for (int ix = std::max(ix0, 0); ix < std::min(ix0 + 4, cols); ++ ix)
                if (const float val = v[size_t(iy) * size_t(cols) + size_t(ix)]; val < INF_F) {
                    const double dx = (double(p.x() - origin.x()) - (double(ix) + 0.5) * res_d) * SCALING_FACTOR;
                    const double dy = (double(p.y() - origin.y()) - (double(iy) + 0.5) * res_d) * SCALING_FACTOR;
                    best = std::min(best, double(val) + std::sqrt(dx * dx + dy * dy));
                }
    }
    if (best == INF_D)
        best = seeds->euclidean_min(p, delta);
    return float(best);
}

void ReachField::Impl::build_grid(const ExPolygons &cprime, const ExPolygons &domain, const Params &params, const ThrowOnCancel &cancel)
{
    BoundingBox bbox = get_extents(domain);
    bbox.merge(get_extents(cprime));
    for (const AnchorSeg &seg : seeds->segs) {
        bbox.merge(seg.l.a);
        bbox.merge(seg.l.b);
    }
    const Vec2d  size   = unscale(bbox.size());
    const double res_mm = std::clamp(std::sqrt(size.x() * size.y() / double(std::max<size_t>(params.grid_max_pixels, 1))),
                                     params.grid_res_min, params.grid_res_max);
    direct = false;
    res    = std::max<coord_t>(scaled<coord_t>(res_mm), 1);
    // Δ ≤ ρ keeps the cells on the rim of the exact zone in the linear part of max(0, dist − Δ), so the Dijkstra
    // continues them exactly; a larger Δ (β ≪ α) widens the zone.
    rho    = std::max(params.rho_exact, delta + 1.5 * unscale<double>(res));
    // Margin: the conservative raster (res/2) plus the 4×4 query neighbourhood of points on the boundary.
    bbox.offset(double(res / 2 + 3 * res));
    Grid grid = make_grid(bbox, res);
    rasterize(domain, grid, 1, true);
    rasterize(cprime, grid, 1, true);
    std::fill(grid.v.begin(), grid.v.end(), INF_F);

    // Exact zone: the cells within ρ of a seed get the exact minimum over these seeds.
    const double res_d   = double(res);
    const double rho_s   = scaled<double>(rho);
    size_t       counter = 0;
    for (size_t i = 0; i < seeds->segs.size(); ++ i) {
        const Line &l = seeds->segs[i].l;
        const int   ix0 = std::max(int(std::ceil((double(std::min(l.a.x(), l.b.x()) - grid.origin.x()) - rho_s) / res_d - 0.5)), 0);
        const int   ix1 = std::min(int(std::floor((double(std::max(l.a.x(), l.b.x()) - grid.origin.x()) + rho_s) / res_d - 0.5)), grid.cols - 1);
        const int   iy0 = std::max(int(std::ceil((double(std::min(l.a.y(), l.b.y()) - grid.origin.y()) - rho_s) / res_d - 0.5)), 0);
        const int   iy1 = std::min(int(std::floor((double(std::max(l.a.y(), l.b.y()) - grid.origin.y()) + rho_s) / res_d - 0.5)), grid.rows - 1);
        for (int iy = iy0; iy <= iy1; ++ iy)
            for (int ix = ix0; ix <= ix1; ++ ix) {
                const size_t idx = size_t(iy) * size_t(grid.cols) + size_t(ix);
                if (grid.m[idx] == 0)
                    continue;
                const Vec2d c((double(grid.origin.x()) + (double(ix) + 0.5) * res_d) * SCALING_FACTOR,
                              (double(grid.origin.y()) + (double(iy) + 0.5) * res_d) * SCALING_FACTOR);
                double      dist;
                const double val = seeds->eval(i, c, delta, &dist);
                if (dist <= rho && val < double(grid.v[idx]))
                    grid.v[idx] = float(val);
            }
        if ((counter += size_t(ix1 - ix0 + 1) * size_t(iy1 - iy0 + 1)) > 0xFFFF) {
            counter = 0;
            if (cancel)
                cancel();
        }
    }

    dijkstra16(grid, cancel);
    origin = grid.origin;
    cols   = grid.cols;
    rows   = grid.rows;
    v      = std::move(grid.v);
}

ReachField::ReachField(const ExPolygons &cprime, const ExPolygons &domain, std::vector<AnchorSeg> seeds, double delta_beta_mm,
                       const Params &params, const ThrowOnCancel &cancel)
{
    auto impl   = std::make_shared<Impl>();
    impl->delta = std::max(0., delta_beta_mm);
    impl->res   = scaled<coord_t>(params.grid_res_max);
    impl->seeds = std::make_shared<const SeedSet>(std::move(seeds));
    if (! impl->seeds->segs.empty() && ! inside_exact_zone(cprime, impl->seeds->segs, params.rho_exact))
        impl->build_grid(cprime, domain, params, cancel);
    m_impl = std::move(impl);
}

ReachField::ReachField(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

ReachField::~ReachField() = default;

float ReachField::value(const Point &p) const { return m_impl->value(p); }

coord_t ReachField::res() const { return m_impl->res; }

bool ReachField::direct_mode() const { return m_impl->direct; }

// Keeps the cells whose 4×4 query neighbourhood can be used by a point of the bbox, with whole-cell offsets so the
// cropped field returns exactly the same values there. Seeds are shared.
std::shared_ptr<const ReachQuery> ReachField::cropped(const BoundingBox &bbox) const
{
    const Impl &src = *m_impl;
    if (src.direct || src.cols == 0 || ! bbox.defined)
        return std::shared_ptr<const ReachQuery>(new ReachField(m_impl));
    const double res_d = double(src.res);
    const int    ix0   = std::max(int(std::floor(double(bbox.min.x() - src.origin.x()) / res_d)) - 3, 0);
    const int    iy0   = std::max(int(std::floor(double(bbox.min.y() - src.origin.y()) / res_d)) - 3, 0);
    const int    ix1   = std::min(int(std::floor(double(bbox.max.x() - src.origin.x()) / res_d)) + 4, src.cols);
    const int    iy1   = std::min(int(std::floor(double(bbox.max.y() - src.origin.y()) / res_d)) + 4, src.rows);
    auto impl    = std::make_shared<Impl>();
    impl->seeds  = src.seeds;
    impl->delta  = src.delta;
    impl->rho    = src.rho;
    impl->direct = false;
    impl->res    = src.res;
    if (ix0 < ix1 && iy0 < iy1) {
        impl->origin = src.origin + Point(coord_t(ix0) * src.res, coord_t(iy0) * src.res);
        impl->cols   = ix1 - ix0;
        impl->rows   = iy1 - iy0;
        impl->v.reserve(size_t(impl->cols) * size_t(impl->rows));
        for (int iy = iy0; iy < iy1; ++ iy) {
            const auto row = src.v.begin() + ptrdiff_t(size_t(iy) * size_t(src.cols));
            impl->v.insert(impl->v.end(), row + ix0, row + ix1);
        }
    }
    return std::shared_ptr<const ReachQuery>(new ReachField(std::move(impl)));
}

float segment_min(const AnchorSeg &seg, const Point &p, double delta_beta_mm)
{
    return float(segment_min_mm(unscale(seg.l.a), unscale(seg.l.b), double(seg.c0), double(seg.c1), unscale(p), delta_beta_mm));
}

// ===== ReachCarry

namespace {

struct CarryFront
{
    ExPolygon                            C;
    BoundingBox                          bbox;
    bool                                 grounded = false;
    std::shared_ptr<const ReachQuery>    field;
    ExPolygons                           zero;        // grown by 2 µm: q on ∂C counts as inside
    BoundingBox                          zero_bbox;
    AABBTreeLines::LinesDistancer<Linef> edges;       // ∂C in scaled double coordinates (see SeedSet)

    // Hanging point q of a within reach r: a itself inside C, else the nearest point of ∂C (radius query only, see
    // SeedSet::euclidean_min).
    bool hangs(const Point &a, coord_t r, Point &q) const
    {
        const Vec2d ad = a.cast<double>();
        if (bbox.contains(a)) {
            int out = edges.outside(ad);
            if (out == 0)
                out = C.contains(a) ? -1 : 1;
            if (out < 0) {
                q = a;
                return true;
            }
        }
        double best  = double(r) * double(r);
        bool   found = false;
        for (size_t i : edges.all_lines_in_radius(ad, double(r) + 1.)) {
            Vec2d        np;
            const double d2 = line_alg::distance_to_squared(edges.get_line(i), ad, &np);
            if (d2 <= best) {
                best  = d2;
                q     = Point(coord_t(std::round(np.x())), coord_t(std::round(np.y())));
                found = true;
            }
        }
        return found;
    }

    // c*(q): 0 on covered (tension, hoop) or supported material, else c(q).
    double c_star(const Point &q) const
    {
        if (! zero.empty() && zero_bbox.contains(q))
            for (const ExPolygon &z : zero)
                if (z.contains(q))
                    return 0.;
        return field ? double(field->value(q)) : 0.;
    }
};

struct CarryLayer
{
    static constexpr coord_t CELL = scaled<coord_t>(2.);   // 2 mm uniform grid over the fronts

    uint32_t                layer = 0;
    double                  z_top = 0.;
    double                  w     = 0.;       // mm
    std::vector<CarryFront> fronts;
    Point                   origin;
    int                     cols = 0, rows = 0;
    std::vector<uint32_t>   cell_begin, cell_fronts;   // CSR: fronts whose bbox overlaps the cell

    int cx(coord_t x) const { return std::clamp(int((x - origin.x()) / CELL), 0, cols - 1); }
    int cy(coord_t y) const { return std::clamp(int((y - origin.y()) / CELL), 0, rows - 1); }

    void build_index()
    {
        BoundingBox all;
        for (const CarryFront &f : fronts)
            all.merge(f.bbox);
        if (! all.defined)
            return;
        origin = all.min;
        cols   = int((all.max.x() - origin.x()) / CELL) + 1;
        rows   = int((all.max.y() - origin.y()) / CELL) + 1;
        cell_begin.assign(size_t(cols) * size_t(rows) + 1, 0);
        for (int pass = 0; pass < 2; ++ pass) {
            std::vector<uint32_t> fill;
            if (pass == 1) {
                for (size_t i = 1; i < cell_begin.size(); ++ i)
                    cell_begin[i] += cell_begin[i - 1];
                cell_fronts.assign(cell_begin.back(), 0);
                fill.assign(cell_begin.begin(), cell_begin.end() - 1);
            }
            for (uint32_t fi = 0; fi < uint32_t(fronts.size()); ++ fi) {
                const BoundingBox &bb = fronts[fi].bbox;
                for (int y = cy(bb.min.y()); y <= cy(bb.max.y()); ++ y)
                    for (int x = cx(bb.min.x()); x <= cx(bb.max.x()); ++ x) {
                        const size_t cell = size_t(y) * size_t(cols) + size_t(x);
                        if (pass == 0)
                            ++ cell_begin[cell + 1];
                        else
                            cell_fronts[fill[cell] ++] = fi;
                    }
            }
        }
    }

    // Calls fn once for every front whose bbox ⊕ r contains a. A front registered in several query cells is visited
    // only in the lowest common cell of its bbox and the query range.
    template<typename Fn> void for_fronts_near(const Point &a, coord_t r, Fn &&fn) const
    {
        if (cols == 0)
            return;
        const int qx0 = cx(a.x() - r), qx1 = cx(a.x() + r), qy0 = cy(a.y() - r), qy1 = cy(a.y() + r);
        for (int y = qy0; y <= qy1; ++ y)
            for (int x = qx0; x <= qx1; ++ x) {
                const size_t cell = size_t(y) * size_t(cols) + size_t(x);
                for (uint32_t k = cell_begin[cell]; k < cell_begin[cell + 1]; ++ k) {
                    const CarryFront &f = fronts[cell_fronts[k]];
                    if (x != std::max(cx(f.bbox.min.x()), qx0) || y != std::max(cy(f.bbox.min.y()), qy0))
                        continue;
                    if (f.bbox.inflated(double(r)).contains(a))
                        fn(f);
                }
            }
    }
};

} // namespace

struct ReachCarry::Impl
{
    explicit Impl(const Params &params) : params(params), delta(params.delta_widths * params.ext_width) {}
    Params                 params;
    double                 delta;   // hanging tolerance δ, mm
    std::deque<CarryLayer> layers;  // increasing layer index; only the H_s window below the next layer
};

ReachCarry::ReachCarry(const Params &params) : m_impl(std::make_unique<Impl>(params)) {}
ReachCarry::~ReachCarry() = default;
ReachCarry::ReachCarry(ReachCarry &&) noexcept = default;
ReachCarry &ReachCarry::operator=(ReachCarry &&) noexcept = default;

void ReachCarry::push(uint32_t layer, double z_top, coord_t w, std::vector<FrontCarry> &&all_fronts)
{
    Impl &impl = *m_impl;
    // A layer pushed again replaces itself and everything above it.
    while (! impl.layers.empty() && impl.layers.back().layer >= layer)
        impl.layers.pop_back();
    const double H_s = impl.params.user.H_s;
    if (H_s <= 0.)
        return;

    CarryLayer cl;
    cl.layer = layer;
    cl.z_top = z_top;
    cl.w     = unscale<double>(w);
    cl.fronts.reserve(all_fronts.size());
    for (FrontCarry &fc : all_fronts) {
        if (fc.C.contour.points.size() < 3)
            continue;
        CarryFront f;
        f.bbox     = fc.bbox.defined ? fc.bbox : get_extents(fc.C);
        f.grounded = fc.grounded;
        f.field    = std::move(fc.field);
        // FrontCarry::boundary is not used: the carry keeps ∂C as double lines (see SeedSet).
        Linesf edges;
        for (const Line &l : to_lines(fc.C))
            edges.emplace_back(l.a.cast<double>(), l.b.cast<double>());
        f.edges    = AABBTreeLines::LinesDistancer<Linef>(std::move(edges));
        if (! fc.zero.empty()) {
            f.zero      = offset_ex(fc.zero, float(scaled<double>(0.002)));
            f.zero_bbox = get_extents(f.zero);
        }
        f.C = std::move(fc.C);
        cl.fronts.emplace_back(std::move(f));
    }
    cl.build_index();
    impl.layers.emplace_back(std::move(cl));
    // The layer above (z_bot = z_top) only hangs from layers m with z_top − z_top_m < H_s.
    while (impl.layers.size() > 1 && z_top - impl.layers.front().z_top >= H_s + EPSILON)
        impl.layers.pop_front();
}

// ĉ(a) (theory §2.2-§2.3, plan §3.8): the topmost layer m below n with z_top_n − z_top_m − h_n < H_s that has a front
// within R = w_n + Σ_{k=m+1..n−1} w_k + δ of a. Only its grounded fronts count (none grounded → 0, §3.14 #11):
// ĉ = min over them of c*(q)·max(0, 1 − (z_top_n − z_top_m − h_n)/H_s), q = the point of the front nearest to a.
float ReachCarry::c_hat(const Point &a, uint32_t n, double z_top_n, double h_n, coord_t w_n) const
{
    const Impl  &impl = *m_impl;
    const double H_s  = impl.params.user.H_s;
    if (H_s <= 0.)
        return 0.f;
    double reach = unscale<double>(w_n) + impl.delta;
    for (auto it = impl.layers.rbegin(); it != impl.layers.rend(); ++ it) {
        const CarryLayer &cl = *it;
        if (cl.layer >= n)
            continue;
        const double dz = z_top_n - cl.z_top - h_n;
        if (! (dz < H_s))
            break;
        const coord_t r             = scaled<coord_t>(reach);
        bool          near          = false;
        bool          near_grounded = false;
        double        best          = INF_D;
        cl.for_fronts_near(a, r, [&](const CarryFront &f) {
            Point q;
            if (! f.hangs(a, r, q))
                return;
            near = true;
            if (! f.grounded)
                return;
            near_grounded = true;
            if (const double c = f.c_star(q); std::isfinite(c))
                best = std::min(best, c);
        });
        if (near)
            return near_grounded && best < INF_D ? float(best * std::max(0., 1. - std::max(0., dz) / H_s)) : 0.f;
        reach += cl.w;
    }
    return 0.f;
}

} // namespace Slic3r::SmartSupports
