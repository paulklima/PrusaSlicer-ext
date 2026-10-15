// Smart support painting: slicer integration — projection of auto states, organic support from auto paint,
// _SUPPORTS_OFF warning, project_states / slice_modifiers (plan §7.1; WP1a).
// WP0: one smoke test of the projection stubs on a sliced print; WP1a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("project_states and slice_modifiers are sized per layer (WP0 stub)", "[SmartSupports][sas-slicer]")
{
    const SasPrint     &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const AnalysisInput in = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams());
    const std::vector<const TriangleSelector::TriangleSplittingData*> per_volume(in.volumes.size(), nullptr);
    CHECK(project_states(in, per_volume, [](TriangleStateType s) { return acts_as_enforcer(s); }, [] {}).size() == in.layers.size());
    CHECK(slice_modifiers(in, true, [] {}).size() == in.layers.size());
    CHECK(slice_modifiers(in, false, [] {}).size() == in.layers.size());
}
