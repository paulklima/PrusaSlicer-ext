// Smart support painting: joint plan (stages, reasons, rows) and verification (plan §3.9, §3.10; WP2c1b).
// WP0 stub: no placements, no residual; per-layer vectors sized; verify() returns nothing.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

Plan make_plan(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    Plan plan;
    plan.stats.resize(in.layers.size());
    plan.zones_paintable.resize(in.layers.size());
    plan.zones_unpainted.resize(in.layers.size());
    return plan;
}

std::vector<Need> verify(const AnalysisInput &in, const BaseModel &base, Reachability &reach, const Plan &plan,
                         const std::vector<VolumePaint> &paint, const ThrowOnCancel &cancel)
{
    return {};
}

} // namespace Slic3r::SmartSupports
