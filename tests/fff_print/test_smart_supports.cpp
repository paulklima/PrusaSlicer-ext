// Smart support painting: reference cases of plan §7.2 (theory §10) through run() (WP2c1b).
//
// Every case runs the whole pipeline (Phase A, plan, paint, report; verification where stated) on a sliced print with
// the §7 default config (interface layers 0) and α = 45° (theory §10 assumes w = 0.2 mm at h 0.2; UserParams defaults to
// auto α). Each case prints a one-line summary (placements, areas, report rows, time) for the work package log.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <oneapi/tbb/global_control.h>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double mm2(const ExPolygons &ex) { return area(ex) * SCALING_FACTOR * SCALING_FACTOR; }

UserParams ref_params()
{
    UserParams up;
    up.alpha_deg = 45;
    return up;
}

const char* cause_name(Cause c) { return c == Cause::Island ? "island" : c == Cause::Cantilever ? "cantilever" : "long span"; }
const char* level_name(Level l) { return l == Level::May ? "may" : l == Level::Should ? "should" : "must"; }
const char* state_name(ReportState s)
{
    static const char *names[] = { "Unplaceable", "BlockedByUser", "WillBeDropped", "RemainingAfterPaint", "OnModelFallback",
                                   "OnModelTooShort", "MayMergeOntoModel", "PartlyOnModel", "BelowLowestLevel", "LongSpanNotPainted",
                                   "CauseDisabled", "ReliesOnDisabledCauses", "TailWithoutDownwardFacets", "SeveralPrintObjects",
                                   "OtherObjectsNotConsidered" };
    return names[int(s)];
}
const char* why_name(Why w)
{
    static const char *names[] = { "", "NotReachable", "NoProgress", "InterfaceGap", "NoTipLayer" };
    return names[int(w)];
}

// Placements and report rows of a run, for the log.
void print_summary(const char *name, const SasRun &r, double seconds)
{
    std::ostringstream os;
    os << "[sas-ref] " << name << ": " << seconds << " s, " << r.input.layers.size() << " layers, " << r.result.placements.size()
       << " placements";
    std::map<std::tuple<int, int, int>, std::pair<int, double>> groups;   // (cause, level, on_model) → count, area
    for (const Placement &pl : r.result.placements) {
        auto &g = groups[{ int(pl.cause), int(pl.level), pl.on_model ? 1 : 0 }];
        ++ g.first;
        g.second += mm2(pl.area);
    }
    for (const auto &[key, g] : groups)
        os << " | " << cause_name(Cause(std::get<0>(key))) << " " << level_name(Level(std::get<1>(key))) << (std::get<2>(key) ? " on model" : "")
           << ": " << g.first << " / " << g.second << " mm²";
    if (! r.result.placements.empty())
        os << " | z " << r.input.layers[r.result.placements.front().layer].print_z << " … " << r.input.layers[r.result.placements.back().layer].print_z;
    os << " | report:";
    for (const ReportEntry &e : r.result.report)
        os << " " << state_name(e.state) << (e.why != Why::None ? "/" : "") << why_name(e.why) << "(" << cause_name(e.cause) << " "
           << level_name(e.level) << ", z " << e.z_first << "…" << e.z_last << ", " << e.area_mm2 << " mm²)";
    os << " | painted " << r.result.painted_areas[0] << "/" << r.result.painted_areas[1];
    std::cout << os.str() << std::endl;
}

// run_sas() with timing and a summary line.
SasRun run_ref(const char *name, const SasPrint &sp, const UserParams &up = ref_params(), const RunOptions &opts = {},
               const std::function<void(Params&)> &adjust = nullptr)
{
    const auto t0 = std::chrono::steady_clock::now();
    SasRun     r  = run_sas(sp.print, up, opts, adjust);
    print_summary(name, r, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return r;
}

// run() on a prepared input (modified copies, modifiers), with a summary line.
SasRun run_input(const char *name, AnalysisInput in, const RunOptions &opts = {}, const IsCanceled &is_canceled = nullptr,
                 const ProgressFn &progress = nullptr)
{
    SasRun     out;
    const auto t0 = std::chrono::steady_clock::now();
    out.input  = std::move(in);
    out.result = run(out.input, opts, is_canceled ? is_canceled : IsCanceled([]() { return false; }), progress ? progress : ProgressFn([](int) {}));
    print_summary(name, out, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return out;
}

AnalysisInput input_of(const SasPrint &sp, const UserParams &up = ref_params())
{
    AnalysisInput in = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, up);
    in.params.verification = false;
    return in;
}

std::vector<const Placement*> placements_of(const Result &r, Cause cause)
{
    std::vector<const Placement*> out;
    for (const Placement &pl : r.placements)
        if (pl.cause == cause)
            out.push_back(&pl);
    return out;
}

double placement_area(const Result &r, Cause cause)
{
    double a = 0.;
    for (const Placement *pl : placements_of(r, cause))
        a += mm2(pl->area);
    return a;
}

int rows_with(const Result &r, ReportState state)
{
    int count = 0;
    for (const ReportEntry &e : r.report)
        count += e.state == state;
    return count;
}

int rows_with(const Result &r, ReportState state, Why why)
{
    int count = 0;
    for (const ReportEntry &e : r.report)
        count += e.state == state && e.why == why;
    return count;
}

// Rows of the problems group (WP2c0 rule).
int problems(const Result &r)
{
    int count = 0;
    for (const ReportEntry &e : r.report)
        count += e.state == ReportState::BlockedByUser ? e.cause == Cause::Island : e.state <= ReportState::MayMergeOntoModel;
    return count;
}

int layer_at_print_z(const AnalysisInput &in, double z)
{
    for (size_t n = 0; n < in.layers.size(); ++ n)
        if (std::abs(in.layers[n].print_z - z) < 0.001)
            return int(n);
    return -1;
}

std::set<uint32_t> placement_layers(const Result &r)
{
    std::set<uint32_t> out;
    for (const Placement &pl : r.placements)
        out.insert(pl.layer);
    return out;
}

double max_c(const Result &r)
{
    float m = 0.f;
    for (const LayerStats &s : r.layer_stats)
        m = std::max(m, s.max_c);
    return m;
}

// Minimum / maximum distance of the placement outlines from a point (scaled), mm.
std::pair<double, double> radial_range(const Placement &pl, const Point &c)
{
    double lo = 1e9, hi = 0.;
    for (const ExPolygon &ex : pl.area)
        for (size_t i = 0; i < ex.num_contours(); ++ i)
            for (const Point &p : ex.contour_or_hole(i).points) {
                const double d = (p - c).cast<double>().norm() * SCALING_FACTOR;
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
    return { lo, hi };
}

// Minimum x of a placement in the object frame relative to the PO frame origin shift, via the lslices of layer 0.
double min_x(const ExPolygons &ex) { return get_extents(ex).min.x() * SCALING_FACTOR; }

// Bridge between two pillars (case #5, #6): pillars pillar × width × height, slab of the given thickness on top.
Parts bridge(double span, double width = 10., double pillar = 5., double height = 10., double thickness = 2.)
{
    const double x1 = pillar + span;
    return { box({ 0., 0., 0. }, { pillar, width, height }), box({ x1, 0., 0. }, { x1 + pillar, width, height }),
             box({ 0., 0., height }, { x1 + pillar, width, height + thickness }) };
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Shelves, T, bridges

TEST_CASE("Reference #1: shelf 2 mm needs nothing, max c 1.8", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_2x15", [] { return shelf_cube(2., 15.); }, sas_config());
    RunOptions opts;
    opts.collect_stats = true;
    const SasRun r = run_ref("#1 shelf 2 mm", sp, ref_params(), opts);
    CHECK(r.result.placements.empty());
    CHECK(problems(r.result) == 0);
    CHECK(max_c(r.result) == Approx(1.8).margin(0.1));
    for (const VolumePaint &vp : r.result.paint)
        CHECK(! vp.changed);
}

TEST_CASE("Reference #2: shelf 10 x 15, one layer, 102 mm², 3.2 mm from the cube, must", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const SasRun    r  = run_ref("#2 shelf 10 x 15", sp);
    REQUIRE(! r.result.placements.empty());
    CHECK(placement_layers(r.result).size() == 1);
    const uint32_t n = r.result.placements.front().layer;
    CHECK(r.input.layers[n].print_z == Approx(18.2).margin(0.01));
    CHECK(placement_area(r.result, Cause::Cantilever) == Approx(102.).margin(10.));
    CHECK(placement_area(r.result, Cause::Island) == 0.);
    bool must = false;
    ExPolygons all;
    for (const Placement &pl : r.result.placements) {
        must |= pl.level == Level::Must;
        CHECK(pl.paintable);
        CHECK(! pl.on_model);
        append(all, pl.area);
    }
    CHECK(must);
    // The cube face is the max x of the layer below; the zone starts 3.2 mm beyond it.
    const double cube_face = get_extents(r.input.layers[n - 1].lslices).max.x() * SCALING_FACTOR;
    CHECK(min_x(all) - cube_face == Approx(3.2).margin(0.3));
    CHECK(problems(r.result) == 0);
    CHECK(r.result.painted_areas[int(Cause::Cantilever)] == int(r.result.placements.size()));
    bool changed = false;
    for (const VolumePaint &vp : r.result.paint)
        changed |= vp.changed;
    CHECK(changed);
}

TEST_CASE("Reference #3: shelf 10 x 30 on a 30 mm cube, 204 mm²", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_10x30", [] { return shelf_cube(10., 30., 30.); }, sas_config());
    const SasRun    r  = run_ref("#3 shelf 10 x 30", sp);
    CHECK(placement_layers(r.result).size() == 1);
    CHECK(placement_area(r.result, Cause::Cantilever) == Approx(204.).epsilon(0.1));
    CHECK(problems(r.result) == 0);
}

TEST_CASE("Reference #4: T-shape 15 x 10, two zones 3.2 mm from the stem, 118 mm² each, must", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("t_shape_15", [] { return t_shape(); }, sas_config());
    const SasRun    r  = run_ref("#4 T-shape", sp);
    REQUIRE(placement_layers(r.result).size() == 1);
    const uint32_t n = r.result.placements.front().layer;
    // Stem = the layer below (10 × 10 centred); two arms.
    const BoundingBox stem = get_extents(r.input.layers[n - 1].lslices);
    double left = 0., right = 0.;
    bool   must = false;
    for (const Placement &pl : r.result.placements) {
        const BoundingBox bb = get_extents(pl.area);
        must |= pl.level == Level::Must;
        if (bb.max.x() <= stem.min.x()) {
            left += mm2(pl.area);
            CHECK((stem.min.x() - bb.max.x()) * SCALING_FACTOR == Approx(3.2).margin(0.3));
        } else if (bb.min.x() >= stem.max.x()) {
            right += mm2(pl.area);
            CHECK((bb.min.x() - stem.max.x()) * SCALING_FACTOR == Approx(3.2).margin(0.3));
        } else
            FAIL_CHECK("placement over the stem");
    }
    CHECK(must);
    CHECK(left == Approx(118.).epsilon(0.1));
    CHECK(right == Approx(118.).epsilon(0.1));
}

TEST_CASE("Reference #5: bridge 30 needs nothing", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("bridge_30", [] { return bridge(30.); }, sas_config());
    const SasRun    r  = run_ref("#5 bridge 30", sp);
    CHECK(r.result.placements.empty());
    CHECK(r.result.report.empty());
}

// ---------------------------------------------------------------------------------------------------------------
// Slopes and chains

TEST_CASE("Reference #13: wall 40°, first placement at 15.8 mm", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
    const SasRun    r  = run_ref("#13 wall 40°", sp);
    REQUIRE(! r.result.placements.empty());
    const double z_first = r.input.layers[r.result.placements.front().layer].print_z;
    std::cout << "#13: first placement at print_z " << z_first << std::endl;
    CHECK(z_first == Approx(15.8).margin(1.6));
    for (const Placement &pl : r.result.placements) {
        CHECK(r.input.layers[pl.layer].print_z > 14.0);
        CHECK(pl.cause == Cause::Cantilever);
        CHECK(! pl.on_model);
    }
    CHECK(problems(r.result) == 0);
}

TEST_CASE("Reference #14: wall 44° and wall 40° with H_s = 0 need nothing", "[SmartSupports][sas-reference]")
{
    {
        const SasPrint &sp = cached_print("slope_wall_44", [] { return slope_wall(44.); }, sas_config());
        const SasRun    r  = run_ref("#14 wall 44°", sp);
        CHECK(r.result.placements.empty());
        CHECK(r.result.report.empty());
    }
    {
        UserParams up = ref_params();
        up.H_s = 0.;
        const SasPrint &sp = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
        const SasRun    r  = run_ref("#14 wall 40°, H_s 0", sp, up);
        CHECK(r.result.placements.empty());
        CHECK(r.result.report.empty());
    }
}

TEST_CASE("Reference #15: stairs 2.5, risers 0.4 → steps 2, 4, 6; risers 1.2 → none", "[SmartSupports][sas-reference]")
{
    {
        const SasPrint &sp = cached_print("stairs_0.4", [] { return stairs(2.5, 0.4); }, sas_config());
        const SasRun    r  = run_ref("#15 stairs 0.4", sp);
        // Step k (k = 1 … 6) starts at print_z 5 + (k − 1)·0.4 + 0.2.
        std::set<int> steps;
        for (const Placement &pl : r.result.placements) {
            const double k = (r.input.layers[pl.layer].print_z - 5.2) / 0.4 + 1.;
            CHECK(std::abs(k - std::round(k)) < 0.01);       // only first layers of a tread
            steps.insert(int(std::lround(k)));
            CHECK(pl.cause == Cause::Cantilever);
        }
        CHECK(steps == std::set<int>{ 2, 4, 6 });
        CHECK(problems(r.result) == 0);
    }
    {
        const SasPrint &sp = cached_print("stairs_1.2", [] { return stairs(2.5, 1.2); }, sas_config());
        const SasRun    r  = run_ref("#15 stairs 1.2", sp);
        CHECK(r.result.placements.empty());
        CHECK(r.result.report.empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Islands and tails

TEST_CASE("Reference #26: inverted 60° cone, tail placements up to apex + 2.6, nothing unplaceable", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("inverted_cone_60", [] { return inverted_cone(60., 10., 5.); }, sas_config());
    const SasRun    r  = run_ref("#26 inverted cone 60°", sp);
    const double    apex = 5.;
    REQUIRE(! placements_of(r.result, Cause::Island).empty());
    double top = 0.;
    for (const Placement *pl : placements_of(r.result, Cause::Island))
        top = std::max(top, r.input.layers[pl->layer].slice_z - apex);
    std::cout << "#26: island placements up to slice_z − apex = " << top << std::endl;
    CHECK(top == Approx(2.6).margin(0.3));
    CHECK(rows_with(r.result, ReportState::Unplaceable) == 0);
    CHECK(problems(r.result) == 0);
}

namespace {

// A print of the parts with a transformed instance (scaled, mirrored, rotated, sinking), processed to posSlice.
std::unique_ptr<SasPrint> make_transformed_print(const Parts &parts, const DynamicPrintConfig &config,
                                                 const std::function<void(ModelInstance&)> &transform, double sink = 0.)
{
    auto out = std::make_unique<SasPrint>();
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(config);
    ModelObject *object = out->model.add_object();
    object->name = "smart_supports_object";
    for (const indexed_triangle_set &its : parts)
        object->add_volume(TriangleMesh(its));
    ModelInstance *inst = object->add_instance();
    if (transform)
        transform(*inst);
    out->model.center_instances_around_point({ 100., 100. });
    object->ensure_on_bed();
    if (sink > 0.)
        inst->set_offset(Z, inst->get_offset(Z) - sink);
    out->print.auto_assign_extruders(object);
    out->print.apply(out->model, full_config);
    if (std::string err = out->print.validate(); ! err.empty())
        throw std::runtime_error("make_transformed_print: " + err);
    out->print.set_status_silent();
    process_to_slices(out->print);
    return out;
}

// Sphere R 20 on the bed (cases #11, #20) and elevated above a small base block (#12, #18).
Parts sphere_on_bed() { return Parts{ translate(its_make_sphere(20., M_PI / 90.), Vec3d(0., 0., 20.)) }; }
Parts sphere_elevated() { return Parts{ box({ 30., -2., 0. }, { 34., 2., 2. }), translate(its_make_sphere(20., M_PI / 90.), Vec3d(0., 0., 25.)) }; }

// Lean-to ceiling (theory §10, case #16): a 2 mm slab rising at `angle` along +x from the bed, between two side walls
// `span` apart (the slab overlaps the walls by 1 mm).
Parts lean_to(double span, double angle = 30., double run = 20., double thickness = 2., double wall = 3.)
{
    const double rise = run * std::tan(angle * M_PI / 180.);
    const ExPolygon profile(polygon_mm({ { 0., 0. }, { run, rise }, { run, rise + thickness }, { 0., thickness } }));
    return { translate(extrude_xz(profile, span + 2.), Vec3d(0., -1., 0.)),
             box({ 0., -wall, 0. }, { run, 0., rise + thickness }), box({ 0., span, 0. }, { run, span + wall, rise + thickness }) };
}

// Bridge 30 with a 5 × 5 × 0.6 logo under the middle of its slab (case #17).
Parts bridge_logo()
{
    Parts out = bridge(30.);
    out.push_back(box({ 17.5, 2.5, 9.4 }, { 22.5, 7.5, 10. }));
    return out;
}

// Projection of the smart paint of a result (auto states with an island / cantilever bit) per layer.
std::vector<ExPolygons> projected_paint(const SasRun &r)
{
    std::vector<const TriangleSelector::TriangleSplittingData*> per_volume;
    for (const VolumePaint &vp : r.result.paint)
        per_volume.push_back(&vp.data);
    const std::vector<Polygons> proj = project_states(r.input, per_volume, [](TriangleStateType s) { return is_auto_state(s); }, {});
    std::vector<ExPolygons> out;
    for (const Polygons &pp : proj)
        out.push_back(union_ex(pp));
    return out;
}

// Centre of the sphere slices (PO frame).
Point sphere_centre(const AnalysisInput &in)
{
    return get_extents(in.layers[in.layers.size() / 2].lslices).center();
}

// Inner radius of each ring of placements (layers grouped while consecutive placements lie within 1 mm radially).
std::vector<std::pair<double, double>> rings(const SasRun &r, const Point &centre, Cause cause)
{
    std::vector<std::pair<double, double>> out;
    for (const Placement *pl : placements_of(r.result, cause)) {
        const auto [lo, hi] = radial_range(*pl, centre);
        if (out.empty() || lo > out.back().second + 1.)
            out.emplace_back(lo, hi);
        else
            out.back().second = std::max(out.back().second, hi);
    }
    return out;
}

} // namespace

TEST_CASE("Reference #6: bridge 80, one long-span row at mid-span, should, not painted", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("bridge_80", [] { return bridge(80.); }, sas_config());
    const SasRun    r  = run_ref("#6 bridge 80", sp);
    const auto      ls = placements_of(r.result, Cause::LongSpan);
    REQUIRE(ls.size() == 1);
    CHECK(! ls.front()->paintable);
    CHECK(ls.front()->level == Level::Should);
    // Mid-span: halfway between the pillars' inner faces (x 5 … 85 in the object frame; the PO frame is centred).
    const BoundingBox obj = get_extents(r.input.layers.front().lslices);
    const double      mid = 0.5 * (obj.min.x() + obj.max.x()) * SCALING_FACTOR;
    const BoundingBox row = get_extents(ls.front()->area);
    CHECK(0.5 * (row.min.x() + row.max.x()) * SCALING_FACTOR == Approx(mid).margin(1.));
    CHECK(rows_with(r.result, ReportState::LongSpanNotPainted) == 1);
    CHECK(placements_of(r.result, Cause::Cantilever).empty());
    CHECK(placements_of(r.result, Cause::Island).empty());
    for (const VolumePaint &vp : r.result.paint)
        CHECK(! vp.changed);
    CHECK(problems(r.result) == 0);
}

TEST_CASE("Reference #7: dome R 20 / 25 with B_ceil 0, first need at z 19.0; default B_ceil none", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("dome_20_25", [] { return dome(20., 25.); }, sas_config());
    {
        const SasRun r = run_ref("#7 dome R 20", sp);
        CHECK(r.result.placements.empty());
        CHECK(r.result.report.empty());
    }
    UserParams up = ref_params();
    up.B_ceil = 0.;
    const SasRun r = run_ref("#7 dome R 20, B_ceil 0", sp, up);
    REQUIRE(! r.result.placements.empty());
    const Placement &first = r.result.placements.front();
    const double     z     = r.input.layers[first.layer].slice_z;
    REQUIRE(r.input.layers[first.layer].lslices.size() == 1);
    REQUIRE(r.input.layers[first.layer].lslices.front().holes.size() == 1);
    const double r_hole = std::sqrt(std::abs(r.input.layers[first.layer].lslices.front().holes.front().area()) / M_PI) * SCALING_FACTOR;
    std::cout << "#7: first placement at slice_z " << z << ", hole r " << r_hole << ", level " << level_name(first.level) << std::endl;
    CHECK(z == Approx(19.0).margin(0.3));
    CHECK(r_hole == Approx(6.3).margin(0.4));
    CHECK(first.level == Level::Should);
    for (const Placement &pl : r.result.placements) {
        CHECK(r.input.layers[pl.layer].slice_z > 18.5);
        CHECK(pl.cause == Cause::Cantilever);
    }
}

TEST_CASE("Reference #8: dome R 30 / 35 with B_ceil 20, one ring at hole r 11.9", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("dome_30_35", [] { return dome(30., 35.); }, sas_config());
    UserParams up = ref_params();
    up.B_ceil = 20.;
    const SasRun r = run_ref("#8 dome R 30, B_ceil 20", sp, up);
    REQUIRE(! r.result.placements.empty());
    std::vector<double> hole_r;
    for (const Placement &pl : r.result.placements) {
        const LayerInput &li = r.input.layers[pl.layer];
        REQUIRE(li.lslices.size() == 1);
        REQUIRE(li.lslices.front().holes.size() == 1);
        hole_r.push_back(std::sqrt(std::abs(li.lslices.front().holes.front().area()) / M_PI) * SCALING_FACTOR);
        CHECK(pl.cause == Cause::Cantilever);
    }
    std::cout << "#8: placements at hole r " << hole_r.front() << " … " << hole_r.back() << ", first level " << level_name(r.result.placements.front().level)
              << ", layers " << placement_layers(r.result).size() << std::endl;
    CHECK(hole_r.front() == Approx(11.9).margin(0.5));
    CHECK(r.result.placements.front().level == Level::Should);
    for (double hr : hole_r)
        CHECK(hr >= 9.8);
}

TEST_CASE("Reference #9: horizontal holes Ø8 / Ø20 / Ø50: none / none at 0.2 mm (should at 0.1 mm) / must", "[SmartSupports][sas-reference]")
{
    {
        const SasPrint &sp = cached_print("hole_8", [] { return hole_block(8.); }, sas_config());
        const SasRun    r  = run_ref("#9 hole Ø8", sp);
        CHECK(r.result.placements.empty());
        CHECK(r.result.report.empty());
    }
    {
        // WP2c1a: the sliced maximum at 0.2 mm layers is c = 2.83 (theory's 4.1 is the limit x → 0).
        const SasPrint &sp = cached_print("hole_20", [] { return hole_block(20., 40., 20., 40.); }, sas_config());
        const SasRun    r  = run_ref("#9 hole Ø20", sp);
        CHECK(r.result.placements.empty());
        const SasPrint &sp1 = cached_print("hole_20", [] { return hole_block(20., 40., 20., 40.); },
                                           sas_config({ { "layer_height", 0.1 }, { "first_layer_height", 0.1 } }));
        const SasRun    r1  = run_ref("#9 hole Ø20, 0.1 mm layers", sp1);
        REQUIRE(! r1.result.placements.empty());
        Level top = Level::May;
        for (const Placement &pl : r1.result.placements)
            top = std::max(top, pl.level);
        CHECK(top == Level::Should);
    }
    {
        // Ø50: c reaches 8.3 without support (must). The joint plan places the should band where c first exceeds K_max
        // and the chain restarts from it, so with "critical and likely" the placements are should; with "critical only"
        // the should band is not planned and the must zone is placed.
        const SasPrint &sp = cached_print("hole_50", [] { return hole_block(50., 70., 20., 70.); }, sas_config());
        const SasRun    r  = run_ref("#9 hole Ø50", sp);
        REQUIRE(! r.result.placements.empty());
        CHECK(problems(r.result) == 0);
        UserParams up = ref_params();
        up.lowest_level = Level::Must;
        const SasRun rm = run_ref("#9 hole Ø50, critical only", sp, up);
        bool must = false;
        for (const Placement &pl : rm.result.placements) {
            CHECK(pl.level == Level::Must);
            must = true;
        }
        CHECK(must);
        CHECK(rows_with(rm.result, ReportState::BelowLowestLevel) >= 1);
    }
}

TEST_CASE("Reference #10: T on an 80 x 80 base: on the model (prefer plate) / unplaceable (plate only)", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("t_shape_on_80x80", [] { return t_shape(15., 10., 10., 20., 2., 80.); }, sas_config());
    {
        const SasRun r = run_ref("#10 T on base, prefer plate", sp);
        REQUIRE(! r.result.placements.empty());
        for (const Placement &pl : r.result.placements) {
            CHECK(pl.on_model);
            CHECK(pl.level == Level::Must);
        }
        CHECK(rows_with(r.result, ReportState::OnModelFallback) + rows_with(r.result, ReportState::OnModelTooShort) >= 1);
    }
    {
        UserParams up = ref_params();
        up.plate_policy = PlatePolicy::PlateOnly;
        const SasPrint &sp1 = cached_print("t_shape_on_80x80", [] { return t_shape(15., 10., 10., 20., 2., 80.); },
                                           sas_config({ { "support_material_buildplate_only", true } }));
        const SasRun r = run_ref("#10 T on base, plate only", sp1, up);
        CHECK(r.result.placements.empty());
        CHECK(rows_with(r.result, ReportState::Unplaceable) >= 1);
        for (const VolumePaint &vp : r.result.paint)
            CHECK(! vp.changed);
    }
}

TEST_CASE("Reference #11: sphere R 20 on the bed, rings at r 5.9 and 12.1", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("sphere_20", sphere_on_bed, sas_config());
    const SasRun    r  = run_ref("#11 sphere R 20", sp, ref_params(), {}, [](Params &p) { p.test_assume_reachable = true; });
    const auto      rr = rings(r, sphere_centre(r.input), Cause::Cantilever);
    std::ostringstream os;
    for (const auto &[lo, hi] : rr)
        os << " [" << lo << ", " << hi << "]";
    std::cout << "#11: rings" << os.str() << std::endl;
    REQUIRE(rr.size() >= 2);
    CHECK(rr[0].first == Approx(5.9).margin(0.6));
    CHECK(rr[1].first == Approx(12.1).margin(1.0));
    for (const auto &[lo, hi] : rr) {
        CHECK(lo > 5.2);
        CHECK(hi < 14.6);
    }
    CHECK(r.result.placements.front().level == Level::Should);
    CHECK(placements_of(r.result, Cause::Island).empty());
}

TEST_CASE("Reference #12: elevated sphere: island at its first layer, then rings as #11", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("sphere_20_elevated", sphere_elevated, sas_config());
    const SasRun    r  = run_ref("#12 elevated sphere", sp, ref_params(), {}, [](Params &p) { p.test_assume_reachable = true; });
    const auto      islands = placements_of(r.result, Cause::Island);
    REQUIRE(! islands.empty());
    // The sphere's first layer: its bottom at z 5.
    CHECK(r.input.layers[islands.front()->layer].print_z == Approx(5.2).margin(0.21));
    const auto rr = rings(r, sphere_centre(r.input), Cause::Cantilever);
    std::ostringstream os;
    for (const auto &[lo, hi] : rr)
        os << " [" << lo << ", " << hi << "]";
    std::cout << "#12: rings" << os.str() << std::endl;
    REQUIRE(! rr.empty());
    CHECK(rr.back().second < 14.6);
    CHECK(rows_with(r.result, ReportState::Unplaceable) == 0);
}

TEST_CASE("Reference #16: lean-to 30 / 50 apart: none / long-span may only", "[SmartSupports][sas-reference]")
{
    {
        const SasPrint &sp = cached_print("lean_to_30", [] { return lean_to(30.); }, sas_config());
        const SasRun    r  = run_ref("#16 lean-to 30", sp);
        CHECK(r.result.placements.empty());
        CHECK(problems(r.result) == 0);
    }
    {
        const SasPrint &sp = cached_print("lean_to_50", [] { return lean_to(50.); }, sas_config());
        const SasRun    r  = run_ref("#16 lean-to 50", sp);
        CHECK(r.result.placements.empty());
        CHECK(problems(r.result) == 0);
        int below = 0;
        for (const ReportEntry &e : r.result.report)
            if (e.state == ReportState::BelowLowestLevel) {
                ++ below;
                CHECK(e.cause == Cause::LongSpan);
                CHECK(e.level == Level::May);
            }
        CHECK(below >= 1);
    }
}

TEST_CASE("Reference #17: bridge 30 with a logo: V_min 20 none; default: island under the logo only", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("bridge_30_logo", bridge_logo, sas_config());
    {
        const SasRun r = run_ref("#17 bridge + logo, V_min 20", sp, ref_params(), {}, [](Params &p) { p.V_min = 20.; });
        CHECK(r.result.placements.empty());
        CHECK(problems(r.result) == 0);
    }
    const SasRun r = run_ref("#17 bridge + logo", sp);
    REQUIRE(! r.result.placements.empty());
    for (const Placement &pl : r.result.placements) {
        CHECK(pl.cause == Cause::Island);
        CHECK(r.input.layers[pl.layer].print_z < 10.01);
    }
    CHECK(placement_area(r.result, Cause::Island) == Approx(25.).epsilon(0.15));
    CHECK(problems(r.result) == 0);
}

TEST_CASE("Reference #18 (P3): the plan does not depend on the cause toggles; paint = union of single runs", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("sphere_20_elevated", sphere_elevated, sas_config());
    auto with = [&](const char *name, uint8_t causes) {
        UserParams up   = ref_params();
        up.paint_causes = causes;
        return run_ref(name, sp, up, {}, [](Params &p) { p.test_assume_reachable = true; });
    };
    const SasRun both = with("#18 {I, C}", cbIsland | cbCantilever);
    const SasRun isl  = with("#18 {I}", cbIsland);
    const SasRun cant = with("#18 {C}", cbCantilever);
    REQUIRE(! both.result.placements.empty());
    for (const SasRun *other : { &isl, &cant }) {
        REQUIRE(other->result.placements.size() == both.result.placements.size());
        for (size_t i = 0; i < both.result.placements.size(); ++ i) {
            const Placement &a = both.result.placements[i], &b = other->result.placements[i];
            CHECK((a.id == b.id && a.cause == b.cause && a.level == b.level && a.layer == b.layer && a.area == b.area));
        }
    }
    CHECK(isl.result.painted_areas[int(Cause::Cantilever)] == 0);
    CHECK(cant.result.painted_areas[int(Cause::Island)] == 0);
    CHECK(rows_with(isl.result, ReportState::CauseDisabled) == 1);
    CHECK(rows_with(cant.result, ReportState::CauseDisabled) == 1);
    // Paint of {I, C} = paint of {I} ∪ paint of {C}, per layer within e.
    const std::vector<ExPolygons> pb = projected_paint(both), pi = projected_paint(isl), pc = projected_paint(cant);
    const float e = scaled<float>(both.input.params.edge_limit);
    double worst = 0.;
    for (size_t n = 0; n < pb.size(); ++ n) {
        const ExPolygons singles = union_ex(pi[n], pc[n]);
        worst = std::max(worst, mm2(diff_ex(pb[n], offset_ex(singles, e))));
        worst = std::max(worst, mm2(diff_ex(singles, offset_ex(pb[n], e))));
    }
    std::cout << "#18: largest per-layer difference beyond e " << worst << " mm²" << std::endl;
    CHECK(worst < 0.01);
}

TEST_CASE("Reference #19 (P5): shelf with a blocker: no paint near the blocker, BlockedByUser", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    AnalysisInput   in = input_of(sp);
    // A blocker volume over the outer 3 mm of the shelf (object frame: shelf x 19 … 30, bottom at z 18).
    ModifierInput &blocker = in.modifiers.emplace_back();
    blocker.mesh     = std::make_shared<const TriangleMesh>(box({ 27., 0., 17.5 }, { 31., 20., 18.5 }));
    blocker.trafo    = in.po_trafo_centered;
    blocker.enforcer = false;
    const SasRun r = run_input("#19 shelf + blocker", std::move(in));
    REQUIRE(! r.result.placements.empty());
    CHECK(rows_with(r.result, ReportState::BlockedByUser) >= 1);
    for (const ReportEntry &e : r.result.report)
        if (e.state == ReportState::BlockedByUser)
            CHECK(e.cause == Cause::Cantilever);              // a note, not a problem
    CHECK(problems(r.result) == 0);
    // The paint keeps tip_d + extra_outset from the blocker (D10, theory §5.2).
    const Params &p = r.input.params;
    const double extra_outset = std::max(0., 0.5 * p.tip_diameter - 0.5 * p.support_line_width) + (p.min_xy_dist ? 0.5 * p.support_line_width : 0.);
    const std::vector<Polygons>   blk   = slice_modifiers(r.input, false, {});
    const std::vector<ExPolygons> paint = projected_paint(r);
    double inside = 0., painted = 0.;
    for (size_t n = 0; n < paint.size(); ++ n) {
        painted += mm2(paint[n]);
        if (! blk[n].empty())
            inside += mm2(intersection_ex(paint[n], offset(blk[n], scaled<float>(p.tip_diameter + extra_outset))));
    }
    std::cout << "#19: painted " << painted << " mm², inside blocker ⊕ clearance " << inside << " mm²" << std::endl;
    CHECK(painted > 10.);
    CHECK(inside < 0.01);
}

TEST_CASE("Reference #20: determinism, 1 thread = default", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("sphere_20_elevated", sphere_elevated, sas_config());
    auto adjust = [](Params &p) { p.test_assume_reachable = true; };
    const SasRun multi = run_ref("#20 default threads", sp, ref_params(), {}, adjust);
    SasRun single;
    {
        tbb::global_control one(tbb::global_control::max_allowed_parallelism, 1);
        single = run_ref("#20 1 thread", sp, ref_params(), {}, adjust);
    }
    REQUIRE(single.result.paint.size() == multi.result.paint.size());
    for (size_t i = 0; i < multi.result.paint.size(); ++ i)
        CHECK(single.result.paint[i].data == multi.result.paint[i].data);
    REQUIRE(single.result.placements.size() == multi.result.placements.size());
    for (size_t i = 0; i < multi.result.placements.size(); ++ i)
        CHECK(single.result.placements[i].area == multi.result.placements[i].area);
    CHECK(single.result.report.size() == multi.result.report.size());
}

TEST_CASE("Reference #21 (P4): a manual enforcer never adds needs", "[SmartSupports][sas-reference]")
{
    // 40° wall (a chain) and a manual enforcer volume across it at mid-height.
    const SasPrint &sp   = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
    AnalysisInput   in0  = input_of(sp);
    AnalysisInput   in1  = in0;
    ModifierInput  &enf  = in1.modifiers.emplace_back();
    enf.mesh     = std::make_shared<const TriangleMesh>(box({ 0., 0., 24. }, { 80., 20., 24.6 }));
    enf.trafo    = in1.po_trafo_centered;
    enf.enforcer = true;
    auto pass_of = [](const AnalysisInput &in) {
        const BaseModel base = build_base_model(in, {}, {});
        std::vector<ExPolygons> E;
        for (const LayerGeom &lg : base.layers)
            E.push_back(lg.manual_enf);
        Reachability reach(in);
        return evaluate_pass(in, base, reach, E, {}, {});
    };
    const PassResult without = pass_of(in0), with = pass_of(in1);
    double grew = 0., total_without = 0., total_with = 0.;
    for (size_t n = 0; n < without.layers.size(); ++ n) {
        double a0 = 0., a1 = 0.;
        for (int c = 0; c < 3; ++ c)
            for (int l = 0; l < 3; ++ l) {
                a0 += mm2(without.layers[n].need[c][l]);
                a1 += mm2(with.layers[n].need[c][l]);
            }
        grew = std::max(grew, a1 - a0);
        total_without += a0;
        total_with    += a1;
    }
    std::cout << "#21: needs without " << total_without << " mm², with the enforcer " << total_with << " mm², max growth per layer " << grew << std::endl;
    CHECK(grew < in0.params.A_min);
    CHECK(total_with < total_without);
    // And the run reports nothing as dropped: the enforcer is reachable.
    const SasRun r = run_input("#21 wall 40° + enforcer", in1);
    CHECK(rows_with(r.result, ReportState::WillBeDropped) == 0);
}

TEST_CASE("Reference #22: verification finds a dropped placement", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    RunOptions opts;
    opts.verification = 1;
    {
        const SasRun r = run_ref("#22 shelf, verification", sp, ref_params(), opts);
        CHECK(rows_with(r.result, ReportState::RemainingAfterPaint) == 0);
        CHECK(problems(r.result) == 0);
    }
    opts.test_mutate_before_paint = [](std::vector<Placement> &placements) { if (! placements.empty()) placements.erase(placements.begin()); };
    const SasRun r = run_ref("#22 shelf, verification, placement dropped", sp, ref_params(), opts);
    CHECK(rows_with(r.result, ReportState::RemainingAfterPaint) >= 1);
    double remaining = 0.;
    for (const ReportEntry &e : r.result.report)
        if (e.state == ReportState::RemainingAfterPaint)
            remaining += e.area_mm2;
    CHECK(remaining == Approx(102.).epsilon(0.15));
}

TEST_CASE("Reference #23: edge cases", "[SmartSupports][sas-reference]")
{
    SECTION("empty input") {
        AnalysisInput in;
        in.params = test_params();
        const Result r = run(in, {}, []() { return false; }, [](int) {});
        CHECK(r.placements.empty());
        CHECK(r.paint.empty());
        CHECK(r.report.empty());
    }
    SECTION("one layer, grounded cube") {
        const SasPrint &plate = cached_print("plate_0.2", [] { return Parts{ box({ 0., 0., 0. }, { 10., 10., 0.2 }) }; }, sas_config());
        const SasRun    r1    = run_ref("#23 one layer", plate);
        CHECK(r1.input.layers.size() == 1);
        CHECK(r1.result.placements.empty());
        const SasPrint &cube = cached_print("cube_20", [] { return Parts{ box({ 0., 0., 0. }, { 20., 20., 20. }) }; }, sas_config());
        const SasRun    r2   = run_ref("#23 grounded cube", cube);
        CHECK(r2.result.placements.empty());
        CHECK(r2.result.report.empty());
    }
    auto shelf_area = [](const SasRun &r) {
        CHECK(placement_layers(r.result).size() == 1);
        return placement_area(r.result, Cause::Cantilever);
    };
    SECTION("first layer 0.3 / layers 0.15") {
        const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); },
                                          sas_config({ { "first_layer_height", 0.3 }, { "layer_height", 0.15 } }));
        CHECK(shelf_area(run_ref("#23 shelf, first 0.3 / 0.15", sp)) == Approx(15. * (10. - 3. - 0.15)).epsilon(0.1));
    }
    SECTION("0.1 mm layers") {
        const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); },
                                          sas_config({ { "first_layer_height", 0.1 }, { "layer_height", 0.1 } }));
        CHECK(shelf_area(run_ref("#23 shelf, 0.1 mm", sp)) == Approx(15. * (10. - 3. - 0.1)).epsilon(0.1));
    }
    SECTION("raft 2") {
        const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config({ { "raft_layers", 2 } }));
        CHECK(shelf_area(run_ref("#23 shelf, raft 2", sp)) == Approx(102.).epsilon(0.1));
    }
    SECTION("scaled, mirrored, rotated, sinking") {
        auto sp_scaled = make_transformed_print(shelf_cube(5., 7.5, 10., 1.), sas_config(),
                                                [](ModelInstance &mi) { mi.set_scaling_factor(Vec3d(2., 2., 2.)); });
        CHECK(shelf_area(run_ref("#23 shelf, scaled ×2", *sp_scaled)) == Approx(102.).epsilon(0.1));
        auto sp_mirrored = make_transformed_print(shelf_cube(10., 15.), sas_config(), [](ModelInstance &mi) { mi.set_mirror(Vec3d(-1., 1., 1.)); });
        const SasRun rm = run_ref("#23 shelf, mirrored", *sp_mirrored);
        CHECK(shelf_area(rm) == Approx(102.).epsilon(0.1));
        bool changed = false;
        for (const VolumePaint &vp : rm.result.paint)
            changed |= vp.changed;
        CHECK(changed);
        // Rotated 90° about X: the shelf becomes a vertical fin 10 deep, 2 thick, hanging from the cube side: its bottom
        // (20 mm²) needs support beyond 3.2 mm.
        auto sp_rot = make_transformed_print(shelf_cube(10., 15.), sas_config(), [](ModelInstance &mi) { mi.set_rotation(X, 0.5 * M_PI); });
        CHECK(shelf_area(run_ref("#23 shelf, X-rotated 90°", *sp_rot)) == Approx(2. * 6.8).epsilon(0.2));
        auto sp_sink = make_transformed_print(shelf_cube(10., 15.), sas_config(), nullptr, 5.);
        const SasRun rs = run_ref("#23 shelf, sinking 5 mm", *sp_sink);
        CHECK(shelf_area(rs) == Approx(102.).epsilon(0.1));
        if (! rs.result.placements.empty())
            CHECK(rs.input.layers[rs.result.placements.front().layer].print_z == Approx(13.2).margin(0.01));
    }
}

TEST_CASE("Reference #24: cancel in every phase", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("sphere_20_elevated", sphere_elevated, sas_config());
    AnalysisInput   in = input_of(sp);
    in.params.test_assume_reachable = true;
    // threshold: the first progress value at or above it raises the cancel flag.
    for (int threshold : { 1, 16, 50, 80, 90 }) {
        std::atomic<bool> flag { false };
        RunOptions opts;
        if (threshold >= 90) {
            opts.verification = 1;
            opts.test_mutate_before_paint = [](std::vector<Placement> &pl) { if (! pl.empty()) pl.pop_back(); };
        }
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_THROWS_AS(run(in, opts, [&flag]() { return flag.load(); }, [&flag, threshold](int pct) { if (pct >= threshold) flag = true; }),
                        CanceledException);
        std::cout << "#24: canceled at progress ≥ " << threshold << " after "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
    }
    // Progress is monotone and ends at 100.
    std::vector<int> pct;
    run(in, {}, []() { return false; }, [&pct](int p) { pct.push_back(p); });
    REQUIRE(! pct.empty());
    CHECK(std::is_sorted(pct.begin(), pct.end()));
    CHECK(pct.back() == 100);
}

TEST_CASE("Reference #26b: inverted 75° cone; 60° cone at 0.1 mm layers: tail painted, nothing unplaceable", "[SmartSupports][sas-reference]")
{
    {
        const SasPrint &sp = cached_print("inverted_cone_75", [] { return inverted_cone(75., 10., 5.); }, sas_config());
        const SasRun    r  = run_ref("#26b inverted cone 75°", sp);
        CHECK(! placements_of(r.result, Cause::Island).empty());
        CHECK(rows_with(r.result, ReportState::Unplaceable) == 0);
        CHECK(r.result.painted_areas[int(Cause::Island)] > 0);
    }
    {
        const SasPrint &sp = cached_print("inverted_cone_60", [] { return inverted_cone(60., 10., 5.); },
                                          sas_config({ { "layer_height", 0.1 }, { "first_layer_height", 0.1 } }));
        const SasRun r = run_ref("#26b inverted cone 60°, 0.1 mm", sp);
        CHECK(r.input.params.z_top_layers == 2);
        CHECK(! placements_of(r.result, Cause::Island).empty());
        CHECK(rows_with(r.result, ReportState::Unplaceable) == 0);
    }
}

TEST_CASE("Reference #27: interface layers 3: wall InterfaceGap, advisory contract, shelf placed", "[SmartSupports][sas-reference]")
{
    const DynamicPrintConfig cfg = sas_config({ { "support_material_interface_layers", 3 } });
    {
        const SasPrint &sp = cached_print("slope_wall_40", [] { return slope_wall(40.); }, cfg);
        const SasRun    r  = run_ref("#27 wall 40°, interface 3", sp);
        CHECK(rows_with(r.result, ReportState::Unplaceable, Why::InterfaceGap) >= 1);
        bool advisory = false, in_gate = false;
        for (const ContractIssue &issue : r.input.contract)
            if (issue.key == ContractIssue::InterfaceLayers) {
                advisory |= issue.advisory;
                in_gate  |= ! issue.advisory;
            }
        CHECK(advisory);
        CHECK(! in_gate);
        for (const ContractIssue &issue : r.input.contract)
            CHECK((issue.advisory || issue.key != ContractIssue::InterfaceLayers));
    }
    {
        const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, cfg);
        const SasRun    r  = run_ref("#27 shelf, interface 3", sp);
        CHECK(placement_area(r.result, Cause::Cantilever) == Approx(102.).epsilon(0.1));
        CHECK(problems(r.result) == 0);
    }
}

TEST_CASE("Reference #25: timings", "[.benchmark][sas-reference-bench]")
{
    struct Shape { const char *name; std::function<Parts()> parts; };
    const std::vector<Shape> shapes {
        { "figurine_proxy", [] { return figurine_proxy(); } },
        { "t_shape_on_80x80", [] { return t_shape(15., 10., 10., 20., 2., 80.); } },
        { "hole_50", [] { return hole_block(50., 70., 20., 70.); } },
        { "dome_20_25", [] { return dome(20., 25.); } },
        { "sphere_20", sphere_on_bed },
        { "slope_wall_40", [] { return slope_wall(40.); } },
    };
    for (const Shape &shape : shapes) {
        const SasPrint &sp = cached_print(shape.name, shape.parts, sas_config());
        RunOptions opts;
        opts.verification = 1;
        std::vector<std::pair<int, double>> marks;
        const auto t0 = std::chrono::steady_clock::now();
        const AnalysisInput in = input_of(sp);
        const Result r = run(in, opts, []() { return false; }, [&](int pct) {
            marks.emplace_back(pct, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        });
        auto at = [&marks](int pct) {
            for (const auto &[p, t] : marks)
                if (p >= pct)
                    return t;
            return marks.empty() ? 0. : marks.back().second;
        };
        std::cout << "[sas-bench] " << shape.name << ": " << in.layers.size() << " layers, total " << at(100) << " s (Phase A " << at(15)
                  << ", plan " << at(80) - at(15) << ", paint " << at(90) - at(80) << ", verification + report " << at(100) - at(90)
                  << "), " << r.placements.size() << " placements, " << r.report.size() << " report rows" << std::endl;
    }
}
