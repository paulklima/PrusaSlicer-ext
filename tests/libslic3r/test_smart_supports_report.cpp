// Smart support painting: contract, recommended changes, report merge / sort / cap, fingerprint (plan §7.1; WP2c0).
// WP0: one smoke test of the stubs; WP2c0 replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("build_report on an empty plan (WP0 stub)", "[SmartSupports][sas-report]")
{
    const AnalysisInput in = synthetic_input({ box({ 0., 0., 0. }, { 10., 10., 2. }) }, 0.2, 0.2, test_params());
    Plan plan;
    plan.stats.resize(in.layers.size());
    plan.zones_paintable.resize(in.layers.size());
    plan.zones_unpainted.resize(in.layers.size());
    int more[2] = { -1, -1 };
    const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
    CHECK(more[0] >= 0);
    CHECK(more[1] >= 0);
    CHECK(report.size() <= 100);
}
