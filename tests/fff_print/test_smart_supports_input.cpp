// Smart support painting: GUI-thread input functions (plan §7.1, WP0): derive_params, build_input,
// choose_print_object, input_consistent; plus checks of the shared test infrastructure.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "smart_supports_test_utils.hpp"

#include "libslic3r/Layer.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

// The tree support settings the slicer would use for this PrintObject (support on, tree / organic only).
struct SlicerTreeSettings {
    explicit SlicerTreeSettings(const PrintObject &po) : mesh_group(po), settings(mesh_group, po.slicing_parameters()) {}
    FFFTreeSupport::TreeSupportMeshGroupSettings mesh_group;
    FFFTreeSupport::TreeSupportSettings          settings;
};

double mm(coord_t v) { return unscale<double>(v); }

} // namespace

TEST_CASE("derive_params mirrors the tree support settings", "[SmartSupports][sas-input]")
{
    for (int top_rate : { 15, 30 }) {
        SECTION("support_tree_top_rate " + std::to_string(top_rate)) {
            auto sp = make_sliced_print(shelf_cube(10., 15.), sas_config({ { "support_tree_top_rate", top_rate } }));
            const PrintObject &po = *sp->print_object();
            const Params p = derive_params(po, UserParams());
            const SlicerTreeSettings slicer(po);
            const FFFTreeSupport::TreeSupportSettings &s = slicer.settings;

            CHECK(p.support_line_width == Approx(mm(s.support_line_width)).margin(1e-5));
            CHECK(p.tip_diameter == Approx(2. * mm(s.min_radius)).margin(1e-5));
            CHECK(p.branch_diameter == Approx(2. * mm(s.branch_radius)).margin(1e-5));
            CHECK(p.tree_angle_deg == Approx(slicer.mesh_group.support_tree_angle * 180. / M_PI).margin(1e-6));
            CHECK(p.tree_angle_slow_deg == Approx(slicer.mesh_group.support_tree_angle_slow * 180. / M_PI).margin(1e-6));
            CHECK(p.branch_distance == Approx(mm(slicer.mesh_group.support_tree_branch_distance)).margin(1e-5));
            CHECK(p.xy_distance == Approx(mm(s.xy_distance)).margin(1e-5));
            CHECK(p.xy_min_distance == Approx(mm(s.xy_min_distance)).margin(1e-5));
            CHECK(p.min_xy_dist == (s.xy_distance > s.xy_min_distance));
            CHECK(p.layer_height == Approx(mm(s.layer_height)).margin(1e-5));
            CHECK(p.gcode_resolution == Approx(mm(s.resolution)).margin(1e-5));
            CHECK(p.z_top_layers == int(s.z_distance_top_layers));
            CHECK(p.z_bottom_layers == int(s.z_distance_bottom_layers));
            CHECK(p.interface_layers == int(slicer.mesh_group.support_roof_layers));
            CHECK(p.raft_support_layers == int(s.raft_layers.size()));
            CHECK(p.buildplate_only == ! s.support_rests_on_model);

            // connect_length of generate_initial_areas() (TreeSupport.cpp:1258).
            const coord_t connect_length = coord_t((s.support_line_width * 100. / slicer.mesh_group.support_tree_top_rate) +
                                                   std::max(2. * s.min_radius - 1.0 * s.support_line_width, 0.0));
            CHECK(p.tip_spacing == Approx(mm(connect_length)).margin(1e-5));

            // External perimeter width: max over the printing regions.
            double ext_width = 0.;
            for (size_t i = 0; i < po.num_printing_regions(); ++ i)
                ext_width = std::max<double>(ext_width, po.printing_region(i).flow(po, frExternalPerimeter, po.config().layer_height).width());
            CHECK(p.ext_width == Approx(ext_width));
        }
    }

    SECTION("raft 2: raft support layers") {
        auto sp = make_sliced_print(shelf_cube(10., 15.), sas_config({ { "raft_layers", 2 } }));
        const PrintObject &po = *sp->print_object();
        const SlicerTreeSettings slicer(po);
        const Params p = derive_params(po, UserParams());
        CHECK(p.raft_support_layers > 0);
        CHECK(p.raft_support_layers == int(slicer.settings.raft_layers.size()));
    }

    SECTION("interface layers, buildplate only and contact distance are read from the config") {
        auto sp = make_sliced_print(shelf_cube(10., 15.), sas_config({ { "support_material_interface_layers", 3 },
            { "support_material_buildplate_only", true }, { "support_material_contact_distance", 0.4 } }));
        const PrintObject &po = *sp->print_object();
        const SlicerTreeSettings slicer(po);
        const Params p = derive_params(po, UserParams());
        CHECK(p.interface_layers == 3);
        CHECK(p.buildplate_only);
        CHECK(p.buildplate_only == ! slicer.settings.support_rests_on_model);
        CHECK(p.contact_distance == Approx(0.4));
        CHECK(p.z_top_layers == 2);
        CHECK(p.z_top_layers == int(slicer.settings.z_distance_top_layers));
    }

    SECTION("support off and grid style: no assert, values from the config") {
        for (const char *style : { "organic", "grid" }) {
            auto sp = make_sliced_print(shelf_cube(10., 15.), sas_config({ { "support_material", false },
                { "support_material_style", style }, { "support_tree_tip_diameter", 1.0 }, { "support_material_interface_layers", 2 } }));
            const Params p = derive_params(*sp->print_object(), UserParams());
            CHECK(p.tip_diameter == Approx(1.0));
            CHECK(p.branch_diameter == Approx(2.0));
            CHECK(p.interface_layers == 2);
            CHECK(p.layer_height == Approx(0.2));
            CHECK(p.support_line_width > 0.);
            CHECK(p.tip_spacing > 0.);
        }
    }

    SECTION("user params are passed through") {
        const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
        UserParams up;
        up.alpha_deg    = 40;
        up.K_max        = 4.5;
        up.plate_policy = PlatePolicy::PlateOnly;
        up.lowest_level = Level::Must;
        const Params p = derive_params(*sp.print_object(), up);
        CHECK(p.user.alpha_deg == 40);
        CHECK(p.user.K_max == 4.5);
        CHECK(p.user.plate_policy == PlatePolicy::PlateOnly);
        CHECK(p.user.lowest_level == Level::Must);
    }
}

TEST_CASE("UserParams defaults: Prusa MK4S 0.4 with a safety margin", "[SmartSupports][sas-input]")
{
    const UserParams up;
    CHECK(up.alpha_deg == 45);   // fixed, not the profile's support_material_threshold
    CHECK(up.K_max == 2.5);
    CHECK(up.B_max == 25.);
    CHECK(up.B_ceil == 25.);
    CHECK(up.H_s == 1.);
    CHECK(up.beta_equals_alpha);
    CHECK(up.paint_causes == (cbIsland | cbCantilever));
    CHECK(up.plate_policy == PlatePolicy::PreferPlate);
    CHECK(up.lowest_level == Level::Should);
    // No ReachBelowTipSpacing note at the 2.9.6 support defaults (tip spacing ≈ 3.07).
    CHECK(up.K_max >= 0.5 * Params().tip_spacing);
}

TEST_CASE("test_params() matches derive_params() of the default test config", "[SmartSupports][sas-input]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const Params t = test_params();
    const Params p = derive_params(*sp.print_object(), t.user);
    CHECK(p.ext_width == Approx(t.ext_width).margin(1e-4));
    CHECK(p.support_line_width == Approx(t.support_line_width).margin(1e-4));
    CHECK(p.tip_diameter == Approx(t.tip_diameter));
    CHECK(p.branch_diameter == Approx(t.branch_diameter));
    CHECK(p.tree_angle_deg == Approx(t.tree_angle_deg));
    CHECK(p.tree_angle_slow_deg == Approx(t.tree_angle_slow_deg));
    CHECK(p.branch_distance == Approx(t.branch_distance));
    CHECK(p.tip_spacing == Approx(t.tip_spacing).margin(1e-3));
    CHECK(p.xy_distance == Approx(t.xy_distance).margin(1e-4));
    CHECK(p.xy_min_distance == Approx(t.xy_min_distance).margin(1e-4));
    CHECK(p.min_xy_dist == t.min_xy_dist);
    CHECK(p.layer_height == Approx(t.layer_height));
    CHECK(p.first_layer_height == Approx(t.first_layer_height));
    CHECK(p.gcode_resolution == Approx(t.gcode_resolution));
    CHECK(p.contact_distance == Approx(t.contact_distance));
    CHECK(p.z_top_layers == t.z_top_layers);
    CHECK(p.z_bottom_layers == t.z_bottom_layers);
    CHECK(p.interface_layers == t.interface_layers);
    CHECK(p.raft_support_layers == t.raft_support_layers);
    CHECK(p.buildplate_only == t.buildplate_only);
    CHECK(p.custom_bridge_angle_deg == t.custom_bridge_angle_deg);
}

TEST_CASE("build_input copies layers, links, volumes and modifiers", "[SmartSupports][sas-input]")
{
    Model model;
    Print print;
    const DynamicPrintConfig config = sas_config();
    ModelObject *mo = init_print_parts(t_shape(), print, model, config);
    // Support volumes and paint on the live model, then apply again.
    mo->add_volume(TriangleMesh(box({ -20., -3., 10. }, { -15., 3., 15. })), ModelVolumeType::SUPPORT_ENFORCER);
    mo->add_volume(TriangleMesh(box({ 15., -3., 10. }, { 20., 3., 15. })), ModelVolumeType::SUPPORT_BLOCKER);
    {
        ModelVolume     *mv = mo->volumes.front();
        TriangleSelector selector(mv->mesh());
        selector.set_facet(0, TriangleStateType::ENFORCER);
        selector.set_facet(1, auto_state(cbCantilever));
        mv->supported_facets.set(selector);
    }
    print.apply(model, config);
    print.set_status_silent();
    process_to_slices(print);

    REQUIRE(print.objects().size() == 1);
    const PrintObject &po = *print.objects().front();
    REQUIRE(input_consistent(po, *mo, 0));
    UserParams up;
    up.K_max = 5.;
    const AnalysisInput in = build_input(po, *mo, 0, up);

    CHECK(in.model_object_id == mo->id());
    CHECK(in.analysed_instance_idx == 0);
    CHECK(in.po_trafo_centered.isApprox(po.trafo_centered()));
    CHECK(in.instance_shift_mm.isApprox(unscale(po.instances().front().shift)));
    CHECK(in.num_print_objects == 1);
    CHECK(! in.other_objects_on_bed);
    CHECK(in.params.user.K_max == 5.);
    CHECK(in.params.tip_spacing == Approx(derive_params(po, up).tip_spacing));

    // Layers and links.
    REQUIRE(in.layers.size() == po.layer_count());
    size_t num_links = 0;
    for (size_t n = 0; n < in.layers.size(); ++ n) {
        const Layer      &layer = *po.get_layer(int(n));
        const LayerInput &li    = in.layers[n];
        CHECK(li.slice_z == layer.slice_z);
        CHECK(li.print_z == layer.print_z);
        CHECK(li.height == layer.height);
        CHECK(li.ext_width_avg_mm > 0.);
        REQUIRE(li.lslices.size() == layer.lslices.size());
        REQUIRE(li.slices.size() == layer.lslices_ex.size());
        for (size_t i = 0; i < li.slices.size(); ++ i) {
            CHECK(li.lslices[i] == layer.lslices[i]);
            CHECK(li.slices[i].bbox == layer.lslices_ex[i].bbox);
            REQUIRE(li.slices[i].below.size() == layer.lslices_ex[i].overlaps_below.size());
            REQUIRE(li.slices[i].above.size() == layer.lslices_ex[i].overlaps_above.size());
            for (size_t k = 0; k < li.slices[i].below.size(); ++ k) {
                CHECK(li.slices[i].below[k].idx == layer.lslices_ex[i].overlaps_below[k].slice_idx);
                CHECK(li.slices[i].below[k].area == layer.lslices_ex[i].overlaps_below[k].area);
            }
            for (size_t k = 0; k < li.slices[i].above.size(); ++ k)
                CHECK(li.slices[i].above[k].idx == layer.lslices_ex[i].overlaps_above[k].slice_idx);
            num_links += li.slices[i].below.size();
        }
    }
    CHECK(num_links > 0);
    CHECK(in.layers.front().slices.front().below.empty());

    // Model parts with trafo_centered() * matrix and their paint; support volumes copied, not sliced.
    REQUIRE(in.volumes.size() == 2);
    REQUIRE(in.modifiers.size() == 2);
    size_t ivolume = 0, imodifier = 0;
    for (const ModelVolume *mv : mo->volumes) {
        if (mv->is_model_part()) {
            const VolumeInput &vi = in.volumes[ivolume ++];
            CHECK(vi.volume_id == mv->id());
            CHECK(vi.mesh == mv->mesh_ptr());
            CHECK(vi.trafo.isApprox(po.trafo_centered() * mv->get_matrix()));
            CHECK(vi.paint == mv->supported_facets.get_data());
            CHECK(vi.paint_timestamp == mv->supported_facets.timestamp());
        } else {
            const ModifierInput &mi = in.modifiers[imodifier ++];
            CHECK(mi.mesh == mv->mesh_ptr());
            CHECK(mi.trafo.isApprox(po.trafo_centered() * mv->get_matrix()));
            CHECK(mi.enforcer == mv->is_support_enforcer());
        }
    }
    CHECK(! in.volumes.front().paint.triangles_to_split.empty());
    CHECK(in.volumes.back().paint.triangles_to_split.empty());
}

TEST_CASE("choose_print_object follows the rules of plan 6.7", "[SmartSupports][sas-input]")
{
    Model model;
    Print print;
    ModelObject *mo = model.add_object();
    for (const indexed_triangle_set &its : shelf_cube(10., 15.))
        mo->add_volume(TriangleMesh(its));
    ModelInstance *i0 = mo->add_instance();
    i0->set_offset(Vec3d(70., 100., 0.));
    ModelInstance *i1 = mo->add_instance();
    i1->set_offset(Vec3d(140., 100., 0.));
    i1->set_rotation(Z, 0.5 * M_PI);
    mo->ensure_on_bed();
    print.auto_assign_extruders(mo);
    print.apply(model, sas_config());
    print.set_status_silent();
    REQUIRE(print.objects().size() == 2);

    // The PrintObject printing live instance i.
    auto po_of = [&](size_t i) -> const PrintObject* {
        for (const PrintObject *po : print.objects())
            for (const PrintInstance &pi : po->instances())
                if (pi.model_instance->id() == mo->instances[i]->id())
                    return po;
        return nullptr;
    };
    auto instance_of = [&](const PrintObject *po) -> size_t {
        for (size_t i = 0; i < mo->instances.size(); ++ i)
            if (po->instances().front().model_instance->id() == mo->instances[i]->id())
                return i;
        return size_t(-1);
    };
    REQUIRE(po_of(0) != nullptr);
    REQUIRE(po_of(1) != nullptr);
    REQUIRE(po_of(0) != po_of(1));
    const PrintObject *first = print.objects().front();

    SECTION("not sliced yet: not ready") {
        const PrintObjectChoice c = choose_print_object(print, *mo, 0, true);
        CHECK(c.po == po_of(0));
        CHECK(! c.ready);
    }

    process_to_slices(print);

    SECTION("background processing on: the selected instance's PrintObject, no fallback") {
        for (size_t sel : { 0, 1 }) {
            const PrintObjectChoice c = choose_print_object(print, *mo, sel, true);
            CHECK(c.po == po_of(sel));
            CHECK(c.analysed_instance == sel);
            CHECK(! c.fallback);
            CHECK(c.ready);
            CHECK(c.why_null == PrintObjectChoice::Null::None);
        }
    }

    SECTION("background processing off: the first matching PrintObject, fallback otherwise") {
        int fallbacks = 0;
        for (size_t sel : { 0, 1 }) {
            const PrintObjectChoice c = choose_print_object(print, *mo, sel, false);
            CHECK(c.po == first);
            CHECK(c.ready);
            if (po_of(sel) == first) {
                CHECK(! c.fallback);
                CHECK(c.analysed_instance == sel);
            } else {
                ++ fallbacks;
                CHECK(c.fallback);
                CHECK(c.analysed_instance == instance_of(first));
                CHECK(c.analysed_instance != sel);
            }
        }
        CHECK(fallbacks == 1);
    }

    SECTION("non-printable selected instance: the first printable one") {
        mo->instances[0]->printable = false;
        const PrintObjectChoice c = choose_print_object(print, *mo, 0, true);
        CHECK(c.analysed_instance == 1);
        CHECK(c.po == po_of(1));
        mo->instances[1]->printable = false;
        const PrintObjectChoice none = choose_print_object(print, *mo, 0, true);
        CHECK(none.po == nullptr);
        CHECK(none.why_null == PrintObjectChoice::Null::NoPrintableInstance);
        mo->instances[0]->printable = true;
        mo->instances[1]->printable = true;
    }

    SECTION("object not in the print") {
        ModelObject *other = model.add_object();
        other->add_volume(TriangleMesh(box({ 0., 0., 0. }, { 10., 10., 10. })));
        other->add_instance();
        for (bool bp : { true, false }) {
            const PrintObjectChoice c = choose_print_object(print, *other, 0, bp);
            CHECK(c.po == nullptr);
            CHECK(c.why_null == PrintObjectChoice::Null::NotInPrint);
        }
    }
}

TEST_CASE("input_consistent is bed-shift safe and detects live changes", "[SmartSupports][sas-input]")
{
    Model model;
    Print print;
    ModelObject *mo = init_print_parts(shelf_cube(10., 15.), print, model, sas_config());
    const PrintObject &po = *print.objects().front();
    CHECK(! input_consistent(po, *mo, 0));
    process_to_slices(print);
    CHECK(input_consistent(po, *mo, 0));
    CHECK(! input_consistent(po, *mo, 1));

    SECTION("live XY move (bed shift): still consistent") {
        mo->instances[0]->set_offset(X, mo->instances[0]->get_offset(X) + 5.);
        mo->instances[0]->set_offset(Y, mo->instances[0]->get_offset(Y) - 3.);
        CHECK(input_consistent(po, *mo, 0));
    }
    SECTION("live Z move or rotation: inconsistent") {
        const Vec3d offset = mo->instances[0]->get_offset();
        mo->instances[0]->set_offset(Z, offset.z() + 1.);
        CHECK(! input_consistent(po, *mo, 0));
        mo->instances[0]->set_offset(offset);
        CHECK(input_consistent(po, *mo, 0));
        mo->instances[0]->set_rotation(Z, 0.3);
        CHECK(! input_consistent(po, *mo, 0));
    }
    SECTION("live volume change: inconsistent") {
        mo->volumes.back()->set_offset(X, mo->volumes.back()->get_offset(X) + 0.5);
        CHECK(! input_consistent(po, *mo, 0));
    }
    SECTION("live volume added: inconsistent") {
        mo->add_volume(TriangleMesh(box({ 0., 0., 0. }, { 5., 5., 5. })), ModelVolumeType::SUPPORT_BLOCKER);
        CHECK(! input_consistent(po, *mo, 0));
    }
}

TEST_CASE("test meshes are closed and synthetic_input matches the slicer", "[SmartSupports][sas-input]")
{
    SECTION("named shapes are closed and outward oriented") {
        const std::vector<Parts> shapes { stairs(), shelf_cube(10., 15.), t_shape(), t_shape(15., 10., 10., 20., 2., 80.), dome(),
                                          hole_block(8.), slope_wall(), inverted_cone(), inverted_cone(75., 5., 0.), figurine_proxy(),
                                          Parts{ wedge(30.) }, Parts{ wedge(45.) } };
        for (const Parts &parts : shapes)
            for (const indexed_triangle_set &its : parts) {
                CHECK(! its.indices.empty());
                CHECK(its_num_open_edges(its) == 0);
                CHECK(its_volume(its) > 0.f);
            }
        CHECK(its_volume(hole_block(8.).front()) == Approx(20. * 20. * 20. - M_PI * 16. * 20.).epsilon(0.01));
        CHECK(its_volume(dome().front()) == Approx(2. / 3. * M_PI * (25. * 25. * 25. - 20. * 20. * 20.)).epsilon(0.01));
    }

    SECTION("synthetic_input of a shelf equals the sliced print up to the frame") {
        const Parts parts = shelf_cube(10., 15.);
        const SasPrint &sp = cached_print("shelf_10x15", [&parts] { return parts; }, sas_config());
        const AnalysisInput real = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams());
        const AnalysisInput syn  = synthetic_input(parts, 0.2, 0.2, test_params());
        REQUIRE(syn.layers.size() == real.layers.size());
        REQUIRE(syn.volumes.size() == parts.size());
        CHECK(syn.params.interface_layers == 0);
        for (size_t n = 0; n < syn.layers.size(); ++ n) {
            const LayerInput &a = syn.layers[n];
            const LayerInput &b = real.layers[n];
            CHECK(a.slice_z == Approx(b.slice_z).margin(1e-6));
            CHECK(a.height == Approx(b.height).margin(1e-6));
            REQUIRE(a.lslices.size() == b.lslices.size());
            CHECK(area(a.lslices) == Approx(area(b.lslices)).epsilon(0.005));
            for (size_t i = 0; i < a.slices.size(); ++ i) {
                CHECK(a.slices[i].below.size() == b.slices[i].below.size());
                CHECK(a.slices[i].above.size() == b.slices[i].above.size());
            }
        }
    }
}
