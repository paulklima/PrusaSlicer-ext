// Smart support painting: anchors, domain edges, tension (ties, cuts), chord rule, hoop credit, classify_front
// (plan §3.8; WP2b2).
// WP0 stub: anchors = the whole ∂C' as material anchor; tension None everywhere; hoop ∅; classification by c only.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

Anchors compute_anchors(const ExPolygons &cprime, const ExPolygon &C, const ExPolygons &grounded_printable,
                        const ExPolygons &F, const ExPolygons &L)
{
    Anchors out;
    out.material = to_polylines(cprime);
    return out;
}

DomainEdges domain_edges(const ExPolygons &D_component, const ExPolygons &stop)
{
    DomainEdges out;
    out.anchor = std::make_shared<const AABBTreeLines::LinesDistancer<Line>>(Lines());
    out.free_  = std::make_shared<const AABBTreeLines::LinesDistancer<Line>>(Lines());
    return out;
}

TensionMap tension_levels(const ExPolygons &cprime, const FrontGeom &front, const ExPolygons &D_component,
                          const DomainEdges &edges, const Anchors &anchors, const Params &params)
{
    TensionMap out;
    if (! cprime.empty())
        out.regions.emplace_back(cprime, TLevel::None);
    return out;
}

ExPolygons hoop_credit(const ExPolygons &cprime, const FrontGeom &front, bool grounded, const Params &params)
{
    return {};
}

FrontNeeds classify_front(const ExPolygons &cprime, const ReachQuery &reach, const TensionMap &tension,
                          const ExPolygons &hoop_band, float max_chat, const Params &params)
{
    FrontNeeds out;
    {
        ExPolygons zero = tension.covered_all;
        append(zero, hoop_band);
        out.zero = union_ex(zero);
    }
    if (cprime.empty())
        return out;

    // Samples at the pixel centres of a grid over C'; bending levels only (theory §2.5).
    const BoundingBox bbox     = get_extents(cprime);
    const double      K        = params.user.K_max;
    const double      K_hard   = params.f_hard * K;
    coord_t           res      = std::max<coord_t>(reach.res(), scaled<coord_t>(0.01));
    const double      nsamples = double(bbox.size().x()) * double(bbox.size().y()) / (double(res) * double(res));
    if (nsamples > 1e6)
        res = coord_t(double(res) * std::sqrt(nsamples / 1e6));
    Polygons squares[3];
    for (coord_t y = bbox.min.y() + res / 2; y < bbox.max.y(); y += res)
        for (coord_t x = bbox.min.x() + res / 2; x < bbox.max.x(); x += res) {
            const Point pt(x, y);
            if (std::none_of(cprime.begin(), cprime.end(), [&pt](const ExPolygon &ex) { return ex.contains(pt); }))
                continue;
            const float c = reach.value(pt);
            if (! std::isfinite(c))
                continue;
            out.max_c = std::max(out.max_c, c);
            if (c <= K)
                continue;
            const int level = c <= K_hard ? int(Level::Should) : int(Level::Must);
            squares[level].push_back(Polygon({ { x - res / 2, y - res / 2 }, { x + res / 2, y - res / 2 },
                                               { x + res / 2, y + res / 2 }, { x - res / 2, y + res / 2 } }));
        }
    for (int level = 0; level < 3; ++ level)
        if (! squares[level].empty())
            out.need[0][level] = intersection_ex(union_ex(squares[level]), cprime);
    return out;
}

} // namespace Slic3r::SmartSupports
