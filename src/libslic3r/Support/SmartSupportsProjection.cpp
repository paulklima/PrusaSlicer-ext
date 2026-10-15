// Smart support painting: projection of paint states and support volumes onto the layers (plan §3.2; WP1a).
// WP0 stub: correctly sized, empty results.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

std::vector<Polygons> project_states(const AnalysisInput &in, const std::vector<const TriangleSelector::TriangleSplittingData*> &per_volume,
                                     const std::function<bool(TriangleStateType)> &filter, const ThrowOnCancel &cancel)
{
    assert(per_volume.size() == in.volumes.size());
    return std::vector<Polygons>(in.layers.size());
}

std::vector<Polygons> slice_modifiers(const AnalysisInput &in, bool enforcers, const ThrowOnCancel &cancel)
{
    return std::vector<Polygons>(in.layers.size());
}

} // namespace Slic3r::SmartSupports
