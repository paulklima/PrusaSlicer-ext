// Smart support painting: prism painting of the placements (plan §4; WP1b).
// WP0 stub: every volume keeps its paint, changed = false.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

std::vector<VolumePaint> paint_placements(const AnalysisInput &in, const std::vector<Placement> &placements, uint8_t causes,
                                          Level lowest, const ThrowOnCancel &cancel)
{
    std::vector<VolumePaint> out;
    out.reserve(in.volumes.size());
    for (const VolumeInput &volume : in.volumes)
        out.push_back({ volume.volume_id, volume.paint, false });
    return out;
}

} // namespace Slic3r::SmartSupports
