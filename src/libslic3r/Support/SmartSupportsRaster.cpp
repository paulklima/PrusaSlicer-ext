// Smart support painting: raster grid, rasterisation, cells → polygons (plan §3.8; WP2b1).
//
// Cell (ix, iy) covers [origin + (ix, iy)·res, origin + (ix + 1, iy + 1)·res); its centre is origin + (ix + ½, iy + ½)·res.
// The origin is shifted off the bounding box by (0.37, 0.61)·res, so that cell centres rarely hit the vertices of CAD
// meshes, which lie on round coordinates (review 4 M3).

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

Grid make_grid(const BoundingBox &bbox, coord_t res)
{
    Grid grid;
    if (! bbox.defined || res <= 0)
        return grid;
    grid.res    = res;
    grid.origin = bbox.min - Point(coord_t(0.37 * double(res)), coord_t(0.61 * double(res)));
    grid.cols   = int((bbox.max.x() - grid.origin.x()) / res) + 1;
    grid.rows   = int((bbox.max.y() - grid.origin.y()) / res) + 1;
    grid.v.assign(size_t(grid.cols) * size_t(grid.rows), 0.f);
    grid.m.assign(size_t(grid.cols) * size_t(grid.rows), 0);
    return grid;
}

// Scanline fill of the cell centres with the non-zero winding rule over all contours and holes, so overlapping
// ExPolygons are filled as their union. An edge crosses the scanline y if min(a.y, b.y) <= y < max(a.y, b.y);
// a cell is inside if its centre x lies in [x_enter, x_leave).
void rasterize(const ExPolygons &expolygons, Grid &grid, uint8_t value, bool conservative)
{
    if (expolygons.empty() || grid.cols <= 0 || grid.rows <= 0 || grid.res <= 0)
        return;
    ExPolygons grown;
    if (conservative)
        // A cell counts as inside if it touches the polygons: its centre lies in the polygons ⊕ res/2 (review 4 M3).
        // Thin necks and the unprintable medium stay connected.
        grown = offset_ex(expolygons, float(grid.res) * 0.5f, ClipperLib::jtRound, double(scaled<coord_t>(0.001)));
    const ExPolygons &src = conservative ? grown : expolygons;

    const double res = double(grid.res);
    const double ox  = double(grid.origin.x());
    const double oy  = double(grid.origin.y());
    // First row / column whose cell centre is >= y / x.
    auto first_row = [&](double y) { return int(std::ceil((y - oy) / res - 0.5)); };
    auto first_col = [&](double x) { return std::clamp(int(std::ceil((x - ox) / res - 0.5)), 0, grid.cols); };

    struct Crossing { int row; double x; int dir; };
    std::vector<Crossing> crossings;
    auto add_polygon = [&](const Polygon &poly) {
        const Points &pts = poly.points;
        for (size_t i = 0; i < pts.size(); ++ i) {
            const Point &a = pts[i];
            const Point &b = pts[i + 1 == pts.size() ? 0 : i + 1];
            if (a.y() == b.y())
                continue;
            const int    r0   = std::max(first_row(double(std::min(a.y(), b.y()))), 0);
            const int    r1   = std::min(first_row(double(std::max(a.y(), b.y()))), grid.rows);   // exclusive
            const double dxdy = double(b.x() - a.x()) / double(b.y() - a.y());
            for (int r = r0; r < r1; ++ r)
                crossings.push_back({ r, double(a.x()) + (oy + (double(r) + 0.5) * res - double(a.y())) * dxdy, b.y() > a.y() ? 1 : -1 });
        }
    };
    for (const ExPolygon &expoly : src) {
        add_polygon(expoly.contour);
        for (const Polygon &hole : expoly.holes)
            add_polygon(hole);
    }
    std::sort(crossings.begin(), crossings.end(), [](const Crossing &l, const Crossing &r) {
        return std::tie(l.row, l.x, l.dir) < std::tie(r.row, r.x, r.dir);
    });

    for (size_t i = 0; i < crossings.size();) {
        const int row = crossings[i].row;
        size_t    end = i;
        while (end < crossings.size() && crossings[end].row == row)
            ++ end;
        uint8_t *cells   = grid.m.data() + size_t(row) * size_t(grid.cols);
        int      winding = 0;
        for (size_t k = i; k + 1 < end; ++ k)
            if ((winding += crossings[k].dir) != 0)
                std::fill(cells + first_col(crossings[k].x), cells + std::max(first_col(crossings[k].x), first_col(crossings[k + 1].x)), value);
        i = end;
    }
}

// Runs of predicate cells per row, merged with identical runs of the rows below into rectangles, then united.
ExPolygons cells_to_expolygons(const Grid &grid, const std::function<bool(int idx)> &pred)
{
    if (grid.cols <= 0 || grid.rows <= 0 || grid.res <= 0)
        return {};
    struct Run { int x0, x1, y0; };                          // [x0, x1) × [y0, current row)
    Polygons                         rects;
    std::vector<Run>                 open, next;
    std::vector<std::pair<int, int>> runs;
    auto emit = [&](const Run &run, int y1) {
        const Point p0 = grid.origin + Point(coord_t(run.x0) * grid.res, coord_t(run.y0) * grid.res);
        const Point p1 = grid.origin + Point(coord_t(run.x1) * grid.res, coord_t(y1) * grid.res);
        rects.emplace_back(Points{ p0, Point(p1.x(), p0.y()), p1, Point(p0.x(), p1.y()) });
    };
    for (int iy = 0; iy <= grid.rows; ++ iy) {
        runs.clear();
        if (iy < grid.rows)
            for (int ix = 0; ix < grid.cols;) {
                if (! pred(iy * grid.cols + ix)) {
                    ++ ix;
                    continue;
                }
                const int x0 = ix;
                while (ix < grid.cols && pred(iy * grid.cols + ix))
                    ++ ix;
                runs.emplace_back(x0, ix);
            }
        // Both lists are sorted by x0 and disjoint: open runs that repeat exactly continue, the others are closed.
        next.clear();
        size_t j = 0;
        for (const Run &run : open) {
            for (; j < runs.size() && runs[j].first < run.x0; ++ j)
                next.push_back({ runs[j].first, runs[j].second, iy });
            if (j < runs.size() && runs[j].first == run.x0 && runs[j].second == run.x1) {
                next.push_back(run);
                ++ j;
            } else
                emit(run, iy);
        }
        for (; j < runs.size(); ++ j)
            next.push_back({ runs[j].first, runs[j].second, iy });
        open.swap(next);
    }
    return union_ex(rects);
}

} // namespace Slic3r::SmartSupports
