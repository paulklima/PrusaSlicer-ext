// Smart support painting: contract, recommended changes, report merge / sort / cap, fingerprint (plan §7.1; WP2c0).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <random>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

// A config that fulfils the contract for PlatePolicy::PreferPlate.
PrintObjectConfig good_config()
{
    PrintObjectConfig cfg;
    cfg.support_material.value                  = true;
    cfg.support_material_auto.value             = false;
    cfg.support_material_style.value            = smsOrganic;
    cfg.support_material_enforce_layers.value   = 0;
    cfg.support_material_contact_distance.value = 0.2;
    cfg.support_material_buildplate_only.value  = false;
    cfg.support_material_interface_layers.value = 0;
    cfg.raft_layers.value                       = 0;
    return cfg;
}

// Every key of the contract violated.
PrintObjectConfig bad_config()
{
    PrintObjectConfig cfg;
    cfg.support_material.value                  = false;
    cfg.support_material_auto.value             = true;
    cfg.support_material_style.value            = smsGrid;
    cfg.support_material_enforce_layers.value   = 3;
    cfg.support_material_contact_distance.value = 0.;
    cfg.support_material_buildplate_only.value  = true;
    cfg.support_material_interface_layers.value = 3;
    cfg.raft_layers.value                       = 2;
    return cfg;
}

std::vector<ContractIssue::Key> keys_of(const std::vector<ContractIssue> &issues, bool non_advisory_only = false)
{
    std::vector<ContractIssue::Key> out;
    for (const ContractIssue &issue : issues)
        if (! non_advisory_only || ! issue.advisory)
            out.push_back(issue.key);
    return out;
}

using Changes = std::vector<std::pair<std::string, std::string>>;

// Layers of 0.2 mm without geometry: build_report() reads only z.
AnalysisInput report_input(size_t num_layers = 60)
{
    AnalysisInput in;
    in.params = test_params();
    for (size_t n = 0; n < num_layers; ++ n) {
        LayerInput &li = in.layers.emplace_back();
        li.height  = 0.2;
        li.print_z = 0.2 * double(n + 1);
        li.slice_z = li.print_z - 0.1;
    }
    return in;
}

Plan empty_plan(const AnalysisInput &in)
{
    Plan plan;
    plan.stats.resize(in.layers.size());
    plan.zones_paintable.resize(in.layers.size());
    plan.zones_unpainted.resize(in.layers.size());
    return plan;
}

Need make_need(ReportState state, Why why, Cause cause, Level level, uint32_t layer, ExPolygons area)
{
    Need need;
    need.reason = state;
    need.why    = why;
    need.cause  = cause;
    need.level  = level;
    need.layer  = layer;
    need.area   = std::move(area);
    return need;
}

Placement make_placement(Cause cause, Level level, uint32_t layer, ExPolygons area, bool paintable = true)
{
    static uint32_t next_id = 0;
    Placement p;
    p.id        = next_id ++;
    p.cause     = cause;
    p.level     = level;
    p.layer     = layer;
    p.area      = std::move(area);
    p.paintable = paintable;
    return p;
}

ExPolygons square(double x, double y, double size) { return { rectangle_mm(x, y, x + size, y + size) }; }

// The documented group rule (plan §3.11, WP2c0): the reference for SmartSupports::is_problem().
bool expected_problem(const ReportEntry &e)
{
    return e.state == ReportState::BlockedByUser ? e.cause == Cause::Island : e.state <= ReportState::MayMergeOntoModel;
}

std::vector<ReportEntry> entries_with(const std::vector<ReportEntry> &report, ReportState state)
{
    std::vector<ReportEntry> out;
    std::copy_if(report.begin(), report.end(), std::back_inserter(out), [state](const ReportEntry &e) { return e.state == state; });
    return out;
}

bool same_entry(const ReportEntry &a, const ReportEntry &b)
{
    return a.state == b.state && a.why == b.why && a.cause == b.cause && a.level == b.level &&
           a.layer_first == b.layer_first && a.layer_last == b.layer_last && a.z_first == b.z_first && a.z_last == b.z_last &&
           a.centroid == b.centroid && a.slice_z == b.slice_z && a.area_mm2 == b.area_mm2 && a.count == b.count;
}

} // namespace

TEST_CASE("check_contract: keys, values and advisory flags", "[SmartSupports][sas-report]")
{
    SECTION("a matching config has no issues") {
        CHECK(check_contract(good_config(), PlatePolicy::PreferPlate).empty());
        CHECK(check_contract(good_config(), PlatePolicy::PreferPlate, nullptr).empty());
    }
    SECTION("every key, in enum order") {
        const std::vector<ContractIssue> issues = check_contract(bad_config(), PlatePolicy::PreferPlate);
        REQUIRE(keys_of(issues) == std::vector<ContractIssue::Key>{ ContractIssue::SupportOff, ContractIssue::AutoOn,
            ContractIssue::NotOrganic, ContractIssue::EnforceLayers, ContractIssue::ContactZero, ContractIssue::BuildplateMismatch,
            ContractIssue::InterfaceLayers, ContractIssue::Raft });
        const std::vector<std::pair<std::string, std::string>> values {
            { "0", "1" }, { "1", "0" }, { "grid", "organic" }, { "3", "0" }, { "0", "0.2" }, { "1", "0" }, { "3", "0" }, { "", "" } };
        for (size_t i = 0; i < issues.size(); ++ i) {
            INFO("issue " << i);
            CHECK(issues[i].current == values[i].first);
            CHECK(issues[i].recommended == values[i].second);
            // InterfaceLayers and Raft are advisory (D30).
            CHECK(issues[i].advisory == (i >= 6));
        }
    }
    SECTION("build plate only follows the policy") {
        const std::vector<ContractIssue> plate_only = check_contract(good_config(), PlatePolicy::PlateOnly);
        REQUIRE(plate_only.size() == 1);
        CHECK(plate_only.front().key == ContractIssue::BuildplateMismatch);
        CHECK(plate_only.front().current == "0");
        CHECK(plate_only.front().recommended == "1");
        CHECK(! plate_only.front().advisory);
        PrintObjectConfig cfg = good_config();
        cfg.support_material_buildplate_only.value = true;
        CHECK(check_contract(cfg, PlatePolicy::PlateOnly).empty());
        CHECK(keys_of(check_contract(cfg, PlatePolicy::PreferPlate)) == std::vector<ContractIssue::Key>{ ContractIssue::BuildplateMismatch });
    }
    SECTION("interface layers > 0 are advisory only (D30)") {
        PrintObjectConfig cfg = good_config();
        cfg.support_material_interface_layers.value = 3;   // the PrintConfig default
        const std::vector<ContractIssue> issues = check_contract(cfg, PlatePolicy::PreferPlate);
        REQUIRE(keys_of(issues) == std::vector<ContractIssue::Key>{ ContractIssue::InterfaceLayers });
        CHECK(issues.front().advisory);
        CHECK(issues.front().current == "3");
        CHECK(issues.front().recommended == "0");
        CHECK(keys_of(issues, true).empty());
    }
    SECTION("a raft is an advisory info key") {
        PrintObjectConfig cfg = good_config();
        cfg.raft_layers.value = 2;
        const std::vector<ContractIssue> issues = check_contract(cfg, PlatePolicy::PreferPlate);
        REQUIRE(keys_of(issues) == std::vector<ContractIssue::Key>{ ContractIssue::Raft });
        CHECK(issues.front().advisory);
        CHECK(issues.front().current.empty());
        CHECK(issues.front().recommended.empty());
    }
    SECTION("reach below half the tip spacing needs derived params (D34)") {
        Params params = test_params();
        params.tip_spacing = 0.4 * 100. / 15. + 0.4;
        params.user.K_max  = 1.;
        const std::vector<ContractIssue> issues = check_contract(good_config(), PlatePolicy::PreferPlate, &params);
        REQUIRE(keys_of(issues) == std::vector<ContractIssue::Key>{ ContractIssue::ReachBelowTipSpacing });
        CHECK(issues.front().advisory);
        CHECK(issues.front().current.empty());
        CHECK(check_contract(good_config(), PlatePolicy::PreferPlate, nullptr).empty());
        params.user.K_max = 3.;
        CHECK(check_contract(good_config(), PlatePolicy::PreferPlate, &params).empty());
    }
}

TEST_CASE("recommended_changes and apply_recommended", "[SmartSupports][sas-report]")
{
    const Changes pre_run {
        { "support_material", "1" }, { "support_material_auto", "0" }, { "support_material_style", "organic" },
        { "support_material_enforce_layers", "0" }, { "support_material_contact_distance", "0.2" },
        { "support_material_buildplate_only", "0" } };
    Changes after_run = pre_run;
    after_run.emplace_back("support_material_interface_layers", "0");

    SECTION("pre-run and after-run change lists") {
        CHECK(recommended_changes(bad_config(), PlatePolicy::PreferPlate, false) == pre_run);
        CHECK(recommended_changes(bad_config(), PlatePolicy::PreferPlate, true) == after_run);
        CHECK(recommended_changes(good_config(), PlatePolicy::PreferPlate, false).empty());
        CHECK(recommended_changes(good_config(), PlatePolicy::PreferPlate, true).empty());
        CHECK(recommended_changes(good_config(), PlatePolicy::PlateOnly, false) == Changes{ { "support_material_buildplate_only", "1" } });
        PrintObjectConfig cfg = good_config();
        cfg.support_material_interface_layers.value = 2;
        cfg.raft_layers.value = 3;   // info only, never changed
        CHECK(recommended_changes(cfg, PlatePolicy::PreferPlate, false).empty());
        CHECK(recommended_changes(cfg, PlatePolicy::PreferPlate, true) == Changes{ { "support_material_interface_layers", "0" } });
    }
    SECTION("apply writes object-level overrides that resolve the contract") {
        ModelConfig object_config;
        object_config.set_deserialize_strict("perimeters", "3");
        const uint64_t ts0 = object_config.timestamp();
        apply_recommended(object_config, good_config(), PlatePolicy::PreferPlate, true);
        CHECK(object_config.timestamp() == ts0);   // nothing to change: not touched
        CHECK(object_config.size() == 1);

        apply_recommended(object_config, bad_config(), PlatePolicy::PreferPlate, false);
        CHECK(object_config.timestamp() != ts0);
        CHECK(object_config.opt_serialize("perimeters") == "3");
        for (const auto &[key, value] : pre_run) {
            INFO(key);
            REQUIRE(object_config.has(key));
            CHECK(object_config.opt_serialize(key) == value);
        }
        CHECK(! object_config.has("support_material_interface_layers"));

        // The effective config of the object (preset + object config) now fulfils the contract except advisories.
        PrintObjectConfig effective = bad_config();
        effective.apply(object_config.get(), true);
        CHECK(keys_of(check_contract(effective, PlatePolicy::PreferPlate), true).empty());
        CHECK(keys_of(check_contract(effective, PlatePolicy::PreferPlate)) ==
              std::vector<ContractIssue::Key>{ ContractIssue::InterfaceLayers, ContractIssue::Raft });

        // After-run "Fix…" (InterfaceGap): also sets the top interface layers to 0.
        apply_recommended(object_config, effective, PlatePolicy::PreferPlate, true);
        CHECK(object_config.opt_serialize("support_material_interface_layers") == "0");
        effective.apply(object_config.get(), true);
        CHECK(keys_of(check_contract(effective, PlatePolicy::PreferPlate)) == std::vector<ContractIssue::Key>{ ContractIssue::Raft });
    }
}

TEST_CASE("config_fingerprint sensitivity", "[SmartSupports][sas-report]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    mo->add_instance();
    mo->add_instance()->set_offset(Vec3d(30., 0., 0.));
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const UserParams         up;
    const uint64_t           fp0 = config_fingerprint(config, *mo, 0, up);

    CHECK(config_fingerprint(config, *mo, 0, up) == fp0);
    CHECK(config_fingerprint(DynamicPrintConfig(config), *mo, 0, up) == fp0);
    // Another instance with the same rotation, scale and z: same analysis.
    CHECK(config_fingerprint(config, *mo, 1, up) == fp0);
    CHECK(config_fingerprint(config, *mo, 2, up) != fp0);

    SECTION("user parameters") {
        auto changed = [&](const std::function<void(UserParams&)> &change) {
            UserParams p = up;
            change(p);
            return config_fingerprint(config, *mo, 0, p) != fp0;
        };
        CHECK(changed([](UserParams &p) { p.paint_causes = cbIsland; }));
        CHECK(changed([](UserParams &p) { p.alpha_deg = 40; }));
        CHECK(changed([](UserParams &p) { p.K_max = 4.; }));
        CHECK(changed([](UserParams &p) { p.B_max = 30.; }));
        CHECK(changed([](UserParams &p) { p.B_ceil = 20.; }));
        CHECK(changed([](UserParams &p) { p.H_s = 0.5; }));
        CHECK(changed([](UserParams &p) { p.beta_equals_alpha = false; }));
        CHECK(changed([](UserParams &p) { p.plate_policy = PlatePolicy::PlateOnly; }));
        CHECK(changed([](UserParams &p) { p.lowest_level = Level::Must; }));
        // β counts only when it is used.
        CHECK(! changed([](UserParams &p) { p.beta_deg = 30.; }));
        CHECK(changed([](UserParams &p) { p.beta_equals_alpha = false; p.beta_deg = 30.; }) );
    }
    SECTION("config options") {
        auto changed = [&](const std::string &key, const std::string &value) {
            DynamicPrintConfig cfg = config;
            INFO(key << " = " << value << " (was " << cfg.opt_serialize(key) << ")");
            REQUIRE(cfg.opt_serialize(key) != value);
            cfg.set_deserialize_strict(key, value);
            return config_fingerprint(cfg, *mo, 0, up) != fp0;
        };
        const std::string style = config.opt_serialize("support_material_style") == "organic" ? "grid" : "organic";
        CHECK(changed("support_material", config.opt_bool("support_material") ? "0" : "1"));
        CHECK(changed("support_material_auto", config.opt_bool("support_material_auto") ? "0" : "1"));
        CHECK(changed("support_material_style", style));
        CHECK(changed("support_material_interface_layers", config.opt_int("support_material_interface_layers") == 0 ? "2" : "0"));
        CHECK(changed("support_material_contact_distance", "0.15"));
        CHECK(changed("support_material_buildplate_only", config.opt_bool("support_material_buildplate_only") ? "0" : "1"));
        CHECK(changed("support_tree_tip_diameter", "1.2"));
        CHECK(changed("support_tree_angle", "35"));
        CHECK(changed("support_tree_angle_slow", "30"));
        CHECK(changed("raft_layers", "2"));
        CHECK(changed("layer_height", "0.15"));
        CHECK(changed("first_layer_height", "0.25"));
        CHECK(changed("nozzle_diameter", "0.6"));
        CHECK(changed("external_perimeter_extrusion_width", "0.61"));
        CHECK(changed("perimeter_extrusion_width", "0.62"));
        CHECK(changed("bridge_angle", "30"));
        CHECK(changed("gcode_resolution", "0.02"));
        CHECK(changed("filament_shrinkage_compensation_xy", "1%"));
        CHECK(changed("filament_shrinkage_compensation_z", "1%"));
        // Options that do not change the analysis.
        CHECK(! changed("perimeter_speed", "77"));
        CHECK(! changed("support_material_speed", "77"));
        CHECK(! changed("support_material_threshold", "33"));
        CHECK(! changed("fill_density", "33%"));
    }
    SECTION("instance") {
        ModelInstance &mi = *mo->instances.front();
        SECTION("XY offset (bed-local)") {
            mi.set_offset(Vec3d(5., 7., mi.get_offset(Z)));
            CHECK(config_fingerprint(config, *mo, 0, up) == fp0);
        }
        SECTION("z offset (sinking)") {
            mi.set_offset(Z, -2.);
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("rotation") {
            mi.set_rotation(Vec3d(0., 0., M_PI / 6.));
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("scaling") {
            mi.set_scaling_factor(Vec3d(1., 1., 1.5));
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
    }
    SECTION("volumes") {
        SECTION("matrix") {
            mo->volumes.front()->set_offset(Vec3d(0., 0., 1.));
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("type") {
            mo->volumes.front()->set_type(ModelVolumeType::PARAMETER_MODIFIER);
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("added support blocker") {
            mo->add_volume(TriangleMesh(its_make_cube(2., 2., 2.)), ModelVolumeType::SUPPORT_BLOCKER);
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("mesh") {
            mo->volumes.front()->set_mesh(TriangleMesh(its_make_cube(10., 10., 10.)));
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("region override of a volume") {
            mo->volumes.front()->config.set_deserialize_strict("external_perimeter_extrusion_width", "0.7");
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("irrelevant override of a volume") {
            mo->volumes.front()->config.set_deserialize_strict("perimeter_speed", "77");
            CHECK(config_fingerprint(config, *mo, 0, up) == fp0);
        }
    }
    SECTION("variable layer height") {
        SECTION("profile") {
            mo->layer_height_profile.set(std::vector<coordf_t>{ 0., 0.2, 10., 0.1 });
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
        SECTION("layer range") {
            mo->layer_config_ranges[{ 2., 4. }].set_deserialize_strict("layer_height", "0.1");
            CHECK(config_fingerprint(config, *mo, 0, up) != fp0);
        }
    }
}

TEST_CASE("build_report: merge rule", "[SmartSupports][sas-report]")
{
    const AnalysisInput in   = report_input();
    Plan                plan = empty_plan(in);
    const auto          U    = ReportState::Unplaceable;
    const auto          NR   = Why::NotReachable;
    const auto          C    = Cause::Cantilever;
    // A: one area drifting by 0.3 mm per layer over layers 10..12.
    for (uint32_t n = 10; n <= 12; ++ n)
        plan.residual.push_back(make_need(U, NR, C, Level::Should, n, square(0.3 * (n - 10), 0., 2.)));
    // F: 0.8 mm right of A at layer 12 → merges into A. G: 1.2 mm right of F → separate.
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 12, { rectangle_mm(3.4, 0., 4.4, 1.), rectangle_mm(5.6, 0., 6.6, 1.) }));
    // B: one layer gap above A. C: far away from A. D: other level. E: other why.
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 14, square(0.6, 0., 2.)));
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 11, square(20., 0., 2.)));
    plan.residual.push_back(make_need(U, NR, C, Level::Must, 10, square(0., 0., 2.)));
    plan.residual.push_back(make_need(U, Why::NoProgress, C, Level::Should, 10, square(0., 0., 2.)));
    // Needs without geometry never merge.
    plan.residual.push_back(make_need(ReportState::TailWithoutDownwardFacets, Why::None, Cause::Island, Level::Must, 20, {}));
    plan.residual.push_back(make_need(ReportState::TailWithoutDownwardFacets, Why::None, Cause::Island, Level::Must, 21, {}));

    int more[2] = { -1, -1 };
    const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
    CHECK(more[0] == 0);
    CHECK(more[1] == 0);

    const std::vector<ReportEntry> unplaceable = entries_with(report, U);
    REQUIRE(unplaceable.size() == 6);
    auto find = [&unplaceable](Level level, Why why, uint32_t first, double min_x_mm) -> const ReportEntry* {
        for (const ReportEntry &e : unplaceable)
            if (e.level == level && e.why == why && e.layer_first == first && unscale<double>(e.centroid.x()) >= min_x_mm)
                return &e;
        return nullptr;
    };
    const ReportEntry *a = find(Level::Should, NR, 10, -1.);
    REQUIRE(a != nullptr);
    CHECK(a->layer_last == 12);
    CHECK(a->count == 4);
    CHECK(a->area_mm2 == Approx(3. * 4. + 1.));
    CHECK(a->z_first == Approx(in.layers[10].print_z));
    CHECK(a->z_last == Approx(in.layers[12].print_z));
    CHECK(a->cause == C);
    // Marker on the largest piece (layer 10, the first of the equal squares in canonical order).
    CHECK(a->slice_z == Approx(in.layers[10].slice_z));
    CHECK(unscale<double>(a->centroid.x()) == Approx(1.).margin(1e-6));
    CHECK(unscale<double>(a->centroid.y()) == Approx(1.).margin(1e-6));

    const ReportEntry *g = find(Level::Should, NR, 12, 5.);
    REQUIRE(g != nullptr);
    CHECK(g->count == 1);
    CHECK(g->area_mm2 == Approx(1.));
    const ReportEntry *b = find(Level::Should, NR, 14, -1.);
    REQUIRE(b != nullptr);
    CHECK(b->count == 1);
    CHECK(b->layer_last == 14);
    const ReportEntry *c = find(Level::Should, NR, 11, 19.);
    REQUIRE(c != nullptr);
    CHECK(c->count == 1);
    CHECK(find(Level::Must, NR, 10, -1.) != nullptr);
    CHECK(find(Level::Should, Why::NoProgress, 10, -1.) != nullptr);

    const std::vector<ReportEntry> tails = entries_with(report, ReportState::TailWithoutDownwardFacets);
    REQUIRE(tails.size() == 2);
    for (const ReportEntry &e : tails) {
        CHECK(e.count == 1);
        CHECK(e.area_mm2 == 0.);
        CHECK(e.layer_first == e.layer_last);
        CHECK(e.z_first == Approx(in.layers[e.layer_first].print_z));
    }
}

TEST_CASE("build_report: marker position", "[SmartSupports][sas-report]")
{
    const AnalysisInput in   = report_input();
    Plan                plan = empty_plan(in);
    // A ring (sphere ring, tail ring): the area centroid is in the hole; the marker goes onto the ring.
    ExPolygon ring(circle_mm(Vec2d(0., 0.), 12.));
    ring.holes.push_back(circle_mm(Vec2d(0., 0.), 11.));
    ring.holes.back().reverse();
    plan.residual.push_back(make_need(ReportState::Unplaceable, Why::NotReachable, Cause::Cantilever, Level::Should, 3, { ring }));
    // A merged entry: the marker goes to the largest piece.
    plan.residual.push_back(make_need(ReportState::Unplaceable, Why::NoProgress, Cause::Cantilever, Level::Should, 3, square(9.5, 9.5, 1.)));
    plan.residual.push_back(make_need(ReportState::Unplaceable, Why::NoProgress, Cause::Cantilever, Level::Should, 4, square(9., 9., 2.)));

    int more[2];
    const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
    REQUIRE(report.size() == 2);
    for (const ReportEntry &e : report) {
        if (e.why == Why::NotReachable) {
            const double r = unscale(e.centroid).norm();
            CHECK(r >= 11. - 0.01);
            CHECK(r <= 12. + 0.01);
            CHECK(e.slice_z == Approx(in.layers[3].slice_z));
            CHECK(e.area_mm2 == Approx(M_PI * (12. * 12. - 11. * 11.)).epsilon(0.01));
        } else {
            CHECK(e.count == 2);
            CHECK(e.layer_first == 3);
            CHECK(e.layer_last == 4);
            CHECK(e.slice_z == Approx(in.layers[4].slice_z));
            CHECK(unscale<double>(e.centroid.x()) == Approx(10.).margin(1e-6));
            CHECK(unscale<double>(e.centroid.y()) == Approx(10.).margin(1e-6));
        }
    }
}

TEST_CASE("build_report: groups and order", "[SmartSupports][sas-report]")
{
    // The shared group rule (SmartSupports.hpp, used by build_report and the GUI) is the documented one.
    for (int st = 0; st <= int(ReportState::OtherObjectsNotConsidered); ++ st)
        for (Cause cause : { Cause::Island, Cause::Cantilever, Cause::LongSpan }) {
            ReportEntry e;
            e.state = ReportState(st);
            e.cause = cause;
            CHECK(is_problem(e) == expected_problem(e));
        }

    const AnalysisInput in   = report_input();
    Plan                plan = empty_plan(in);
    const auto          U    = ReportState::Unplaceable;
    const auto          NR   = Why::NotReachable;
    const auto          C    = Cause::Cantilever;
    // Problems, expected order: level desc, area desc, layer, x.
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 1, square(10., 0., 2.)));           // 4: area 4, layer 1, x 11
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 1, square(0., 0., 2.)));            // 3: area 4, layer 1, x 1
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 3, square(0., 20., 3.)));           // 1: area 9
    plan.residual.push_back(make_need(U, NR, C, Level::Must, 5, square(0., 40., 1.)));             // 0: must
    plan.residual.push_back(make_need(U, NR, C, Level::Should, 0, square(30., 0., 2.)));           // 2: area 4, layer 0
    plan.residual.push_back(make_need(ReportState::BlockedByUser, Why::None, Cause::Island, Level::Must, 7, square(50., 0., 0.5)));
    // Notes.
    plan.residual.push_back(make_need(ReportState::BlockedByUser, Why::None, C, Level::Must, 7, square(60., 0., 5.)));
    plan.residual.push_back(make_need(ReportState::BelowLowestLevel, Why::None, C, Level::May, 9, square(70., 0., 8.)));
    plan.residual.push_back(make_need(ReportState::PartlyOnModel, Why::None, C, Level::Must, 9, square(80., 0., 1.)));
    plan.residual.push_back(make_need(ReportState::LongSpanNotPainted, Why::None, Cause::LongSpan, Level::Should, 9, square(90., 0., 1.)));
    // Verification residual: a problem.
    const std::vector<Need> verify_residual { make_need(ReportState::RemainingAfterPaint, Why::None, C, Level::Should, 2, square(100., 0., 2.5)) };

    int more[2] = { -1, -1 };
    const std::vector<ReportEntry> report = build_report(in, plan, verify_residual, more);
    REQUIRE(report.size() == 11);
    CHECK(more[0] == 0);
    CHECK(more[1] == 0);
    // Problems first.
    const auto first_note = std::find_if(report.begin(), report.end(), [](const ReportEntry &e) { return ! is_problem(e); });
    REQUIRE(first_note - report.begin() == 7);
    CHECK(std::all_of(first_note, report.end(), [](const ReportEntry &e) { return ! is_problem(e); }));

    // Problems: must (2 entries: Unplaceable area 1, BlockedByUser island area 0.25), then should by area, layer, x.
    CHECK(report[0].state == U);
    CHECK(report[0].level == Level::Must);
    CHECK(report[1].state == ReportState::BlockedByUser);
    CHECK(report[1].cause == Cause::Island);
    CHECK(report[2].area_mm2 == Approx(9.));
    CHECK(report[3].state == ReportState::RemainingAfterPaint);
    CHECK(report[3].area_mm2 == Approx(6.25));
    CHECK(report[4].layer_first == 0);
    CHECK(report[5].layer_first == 1);
    CHECK(unscale<double>(report[5].centroid.x()) == Approx(1.));
    CHECK(report[6].layer_first == 1);
    CHECK(unscale<double>(report[6].centroid.x()) == Approx(11.));
    // Notes: must (area desc), then should, then may.
    CHECK(report[7].state == ReportState::BlockedByUser);
    CHECK(report[7].cause == C);
    CHECK(report[8].state == ReportState::PartlyOnModel);
    CHECK(report[9].state == ReportState::LongSpanNotPainted);
    CHECK(report[10].state == ReportState::BelowLowestLevel);
}

TEST_CASE("build_report: cap and report_more", "[SmartSupports][sas-report]")
{
    AnalysisInput in   = report_input();
    Plan          plan = empty_plan(in);
    // 60 separate problems with distinct areas, 53 notes.
    for (int i = 0; i < 60; ++ i)
        plan.residual.push_back(make_need(ReportState::Unplaceable, Why::NotReachable, Cause::Cantilever, Level::Should, 5,
                                          square(5. * (i % 10), 5. * (i / 10), 1. + 0.05 * i)));
    for (int i = 0; i < 53; ++ i)
        plan.residual.push_back(make_need(ReportState::BelowLowestLevel, Why::None, Cause::Cantilever, Level::May, uint32_t(i),
                                          square(100. + 5. * (i % 10), 5. * (i / 10), 1.)));

    SECTION("50 per group") {
        int more[2] = { -1, -1 };
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        CHECK(more[0] == 10);
        CHECK(more[1] == 3);
        REQUIRE(report.size() == 100);
        // The largest 50 problems are kept, in descending area.
        const std::vector<ReportEntry> problems = entries_with(report, ReportState::Unplaceable);
        REQUIRE(problems.size() == 50);
        CHECK(problems.front().area_mm2 == Approx(sqr(1. + 0.05 * 59)));
        CHECK(problems.back().area_mm2 == Approx(sqr(1. + 0.05 * 10)));
        CHECK(std::is_sorted(problems.begin(), problems.end(), [](const ReportEntry &l, const ReportEntry &r) { return l.area_mm2 > r.area_mm2; }));
        // Notes of equal area by layer.
        const std::vector<ReportEntry> notes = entries_with(report, ReportState::BelowLowestLevel);
        REQUIRE(notes.size() == 50);
        CHECK(notes.front().layer_first == 0);
        CHECK(notes.back().layer_first == 49);
    }
    SECTION("summary notes are never hidden") {
        in.num_print_objects    = 2;
        in.other_objects_on_bed = true;
        int more[2] = { -1, -1 };
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        CHECK(more[0] == 10);
        CHECK(more[1] == 5);
        REQUIRE(report.size() == 100);
        CHECK(report[50].state == ReportState::SeveralPrintObjects);
        CHECK(report[51].state == ReportState::OtherObjectsNotConsidered);
    }
}

TEST_CASE("build_report: summary notes", "[SmartSupports][sas-report]")
{
    AnalysisInput in   = report_input();
    Plan          plan = empty_plan(in);
    plan.placements.push_back(make_placement(Cause::Cantilever, Level::Should, 2, square(0., 0., 1.)));    // below the islands
    plan.placements.push_back(make_placement(Cause::Island, Level::Must, 5, square(10., 0., 2.)));
    plan.placements.push_back(make_placement(Cause::Cantilever, Level::Should, 5, square(20., 0., 1.)));   // after the island at 5
    plan.placements.push_back(make_placement(Cause::Island, Level::Should, 7, square(30., 0., 1.)));
    plan.placements.push_back(make_placement(Cause::Cantilever, Level::Must, 8, square(40., 0., 3.)));
    plan.placements.push_back(make_placement(Cause::LongSpan, Level::Should, 9, square(50., 0., 4.), false));
    plan.residual.push_back(make_need(ReportState::BelowLowestLevel, Why::None, Cause::Cantilever, Level::Must, 1, square(0., 50., 9.)));

    SECTION("islands unchecked") {
        in.params.user.paint_causes = cbCantilever;
        int more[2];
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        REQUIRE(report.size() == 3);
        // Summary rows first.
        CHECK(report[0].state == ReportState::CauseDisabled);
        CHECK(report[1].state == ReportState::ReliesOnDisabledCauses);
        CHECK(report[2].state == ReportState::BelowLowestLevel);

        const ReportEntry &disabled = report[0];
        CHECK(disabled.cause == Cause::Island);
        CHECK(disabled.level == Level::Must);
        CHECK(disabled.count == 2);
        CHECK(disabled.area_mm2 == Approx(5.));
        CHECK(disabled.layer_first == 5);
        CHECK(disabled.layer_last == 7);
        CHECK(unscale<double>(disabled.centroid.x()) == Approx(11.));

        const ReportEntry &relies = report[1];
        CHECK(relies.count == 2);
        CHECK(relies.layer_first == 5);
        CHECK(relies.layer_last == 8);
        CHECK(relies.level == Level::Must);
        CHECK(relies.cause == Cause::Cantilever);
        CHECK(relies.area_mm2 == Approx(10.));
    }
    SECTION("overhangs unchecked") {
        in.params.user.paint_causes = cbIsland;
        int more[2];
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        REQUIRE(report.size() == 3);
        CHECK(report[0].state == ReportState::CauseDisabled);
        CHECK(report[0].cause == Cause::Cantilever);
        CHECK(report[0].count == 3);
        // Islands planned after the first cantilever placement (layer 2) may lean on it.
        CHECK(report[1].state == ReportState::ReliesOnDisabledCauses);
        CHECK(report[1].cause == Cause::Island);
        CHECK(report[1].count == 2);
    }
    SECTION("lowest level Must: should placements are not painted anyway") {
        in.params.user.paint_causes = cbCantilever;
        in.params.user.lowest_level = Level::Must;
        int more[2];
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        const std::vector<ReportEntry> disabled = entries_with(report, ReportState::CauseDisabled);
        REQUIRE(disabled.size() == 1);
        CHECK(disabled.front().count == 1);
        const std::vector<ReportEntry> relies = entries_with(report, ReportState::ReliesOnDisabledCauses);
        REQUIRE(relies.size() == 1);
        CHECK(relies.front().count == 1);
        CHECK(relies.front().layer_first == 8);
    }
    SECTION("all causes checked") {
        in.num_print_objects    = 3;
        in.other_objects_on_bed = true;
        int more[2];
        const std::vector<ReportEntry> report = build_report(in, plan, {}, more);
        REQUIRE(report.size() == 3);
        CHECK(report[0].state == ReportState::SeveralPrintObjects);
        CHECK(report[0].count == 3);
        CHECK(report[0].area_mm2 == 0.);
        CHECK(report[1].state == ReportState::OtherObjectsNotConsidered);
        CHECK(report[2].state == ReportState::BelowLowestLevel);
        CHECK(entries_with(report, ReportState::CauseDisabled).empty());
        CHECK(entries_with(report, ReportState::ReliesOnDisabledCauses).empty());
    }
}

TEST_CASE("build_report: empty plan and determinism", "[SmartSupports][sas-report]")
{
    const AnalysisInput in = synthetic_input({ box({ 0., 0., 0. }, { 10., 10., 2. }) }, 0.2, 0.2, test_params());
    SECTION("empty plan") {
        int more[2] = { -1, -1 };
        CHECK(build_report(in, empty_plan(in), {}, more).empty());
        CHECK(more[0] == 0);
        CHECK(more[1] == 0);
    }
    SECTION("input order does not matter") {
        Plan plan = empty_plan(in);
        std::mt19937 rng(7);
        for (int i = 0; i < 200; ++ i) {
            const auto state = std::array<ReportState, 4>{ ReportState::Unplaceable, ReportState::BelowLowestLevel,
                                                           ReportState::OnModelFallback, ReportState::PartlyOnModel }[rng() % 4];
            const Level    level = Level(rng() % 3);
            const uint32_t layer = uint32_t(rng() % in.layers.size());
            plan.residual.push_back(make_need(state, Why(rng() % 3), Cause(rng() % 2), level, layer,
                                              square(double(rng() % 40), double(rng() % 40), 0.5 + 0.1 * double(rng() % 20))));
        }
        int more_a[2], more_b[2];
        const std::vector<ReportEntry> a = build_report(in, plan, {}, more_a);
        std::shuffle(plan.residual.begin(), plan.residual.end(), rng);
        const std::vector<ReportEntry> b = build_report(in, plan, {}, more_b);
        REQUIRE(a.size() == b.size());
        CHECK(more_a[0] == more_b[0]);
        CHECK(more_a[1] == more_b[1]);
        for (size_t i = 0; i < a.size(); ++ i) {
            INFO("entry " << i);
            CHECK(same_entry(a[i], b[i]));
        }
    }
}
