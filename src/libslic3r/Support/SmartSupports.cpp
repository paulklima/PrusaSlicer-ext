// Smart support painting: run() orchestration, progress, cancel (plan §2.2, §2.3; WP2c1b).
// WP0 stub: paint = unchanged volumes with changed = false, everything else empty.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

Result run(const AnalysisInput &in, const RunOptions &opt, const IsCanceled &is_canceled, const ProgressFn &progress)
{
    Result result;
    result.paint.reserve(in.volumes.size());
    for (const VolumeInput &volume : in.volumes)
        result.paint.push_back({ volume.volume_id, volume.paint, false });
    if (opt.collect_stats)
        result.layer_stats.resize(in.layers.size());
    return result;
}

} // namespace Slic3r::SmartSupports
