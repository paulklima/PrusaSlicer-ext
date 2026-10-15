// Smart support painting: raster grid, rasterisation, cells → polygons (plan §3.8; WP2b1).
// WP0 stub: make_grid sizes the grid, rasterize is a no-op, cells_to_expolygons returns ∅.

#include "SmartSupportsInternal.hpp"

#include <cmath>

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

void rasterize(const ExPolygons &expolygons, Grid &grid, uint8_t value, bool conservative) {}

ExPolygons cells_to_expolygons(const Grid &grid, const std::function<bool(int idx)> &pred)
{
    return {};
}

} // namespace Slic3r::SmartSupports
