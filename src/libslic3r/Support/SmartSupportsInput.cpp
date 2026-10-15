// Smart support painting: GUI-thread input functions (plan §2.4, §3.4, §6.7; WP0).
// derive_params(), choose_print_object(), input_consistent(), build_input().

#include "SmartSupports.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

namespace Slic3r::SmartSupports {

// Number of support layers below the first object layer, mirrors the raft_layers loop of the
// TreeSupportSettings constructor (TreeSupportCommon.cpp:147-176).
static int raft_support_layer_count(const SlicingParameters &slicing_params)
{
    if (slicing_params.raft_layers() == 0)
        return 0;
    int    cnt = 1;
    double z   = slicing_params.first_print_layer_height;
    // Raft base layers.
    for (size_t i = 1; i < slicing_params.base_raft_layers; ++ i) {
        z += slicing_params.base_raft_layer_height;
        ++ cnt;
    }
    // Raft interface layers.
    for (size_t i = 0; i + 1 < slicing_params.interface_raft_layers; ++ i) {
        z += slicing_params.interface_raft_layer_height;
        ++ cnt;
    }
    // Raft contact layer.
    if (slicing_params.raft_layers() > 1) {
        z = slicing_params.raft_contact_top_z;
        ++ cnt;
    }
    // Layers between the raft contacts and the bottom of the object.
    if (double dist_to_go = slicing_params.object_print_z_min - z; dist_to_go > EPSILON && slicing_params.max_suport_layer_height > 0.)
        cnt += int(ceil(dist_to_go / slicing_params.max_suport_layer_height));
    return cnt;
}

Params derive_params(const PrintObject &po, const UserParams &up)
{
    const PrintConfig       &print_config   = po.print()->config();
    const PrintObjectConfig &config         = po.config();
    const SlicingParameters &slicing_params = po.slicing_parameters();

    Params params;
    params.user = up;

    // Maximum external perimeter width over all printing regions at the default layer height (TreeSupportCommon.cpp:36-40).
    double ext_width = 0.;
    for (size_t region_id = 0; region_id < po.num_printing_regions(); ++ region_id)
        ext_width = std::max<double>(ext_width, po.printing_region(region_id).flow(po, frExternalPerimeter, config.layer_height).width());
    if (ext_width > 0.)
        params.ext_width = ext_width;
    // First region with a fixed bridge angle.
    for (size_t region_id = 0; region_id < po.num_printing_regions(); ++ region_id)
        if (double angle = po.printing_region(region_id).config().bridge_angle.value; angle > 0.) {
            params.custom_bridge_angle_deg = angle;
            break;
        }

    params.layer_height        = config.layer_height.value;
    params.first_layer_height  = slicing_params.first_object_layer_height;
    params.gcode_resolution    = print_config.gcode_resolution.value;
    params.support_line_width  = support_material_flow(&po, float(config.layer_height.value)).width();
    params.branch_diameter     = config.support_tree_branch_diameter.value;
    params.tip_diameter        = std::clamp(config.support_tree_tip_diameter.value, 0., params.branch_diameter);
    params.tree_angle_deg      = std::clamp(config.support_tree_angle.value, 0., 90.);
    params.branch_distance     = config.support_tree_branch_distance.value;
    // connect_length of generate_initial_areas() (TreeSupport.cpp:1258), r = tip_diameter / 2.
    const double top_rate      = config.support_tree_top_rate.value;
    params.tip_spacing         = (top_rate > 0. ? params.support_line_width * 100. / top_rate : 0.) +
                                 std::max(params.tip_diameter - params.support_line_width, 0.);

    // XY distances (TreeSupportCommon.cpp:58-60, 108-109, 132-138; TreeSupport.cpp:1252).
    params.xy_distance         = config.support_material_xy_spacing.get_abs_value(params.ext_width);
    params.xy_min_distance     = std::min(params.xy_distance, 0.5 * params.ext_width);
    if (config.support_material_contact_distance.value < EPSILON) {
        // TreeSupportSettings::soluble, set by the slicer if any object has a zero contact distance.
        params.xy_min_distance = std::max(params.xy_min_distance, 0.1);
        params.xy_distance     = std::max(params.xy_distance, params.xy_min_distance);
    }
    params.min_xy_dist         = params.xy_distance > params.xy_min_distance;

    // Z distances in layers (TreeSupportCommon.cpp:112-113).
    params.contact_distance    = config.support_material_contact_distance.value;
    params.z_top_layers        = params.layer_height > 0. ? int(std::round(slicing_params.gap_support_object / params.layer_height)) : 0;
    params.z_bottom_layers     = params.layer_height > 0. ? int(std::round(slicing_params.gap_object_support / params.layer_height)) : 0;
    params.interface_layers    = config.support_material_interface_layers.value;
    params.raft_support_layers = raft_support_layer_count(slicing_params);
    params.buildplate_only     = config.support_material_buildplate_only.value;
    return params;
}

static const ModelInstance* find_instance(const ModelObject &mo, ObjectID id)
{
    for (const ModelInstance *mi : mo.instances)
        if (mi->id() == id)
            return mi;
    return nullptr;
}

static bool po_has_instance(const PrintObject &po, ObjectID id)
{
    for (const PrintInstance &pi : po.instances())
        if (pi.model_instance->id() == id)
            return true;
    return false;
}

PrintObjectChoice choose_print_object(const Print &active, const ModelObject &live_mo, size_t selected_instance, bool background_processing)
{
    PrintObjectChoice out;

    // 1. Analysed instance: the selected one if printable, else the first printable one.
    size_t analysed = size_t(-1);
    if (selected_instance < live_mo.instances.size() && live_mo.instances[selected_instance]->is_printable())
        analysed = selected_instance;
    else
        for (size_t i = 0; i < live_mo.instances.size(); ++ i)
            if (live_mo.instances[i]->is_printable()) {
                analysed = i;
                break;
            }
    if (analysed == size_t(-1)) {
        out.why_null = PrintObjectChoice::Null::NoPrintableInstance;
        return out;
    }
    out.analysed_instance = analysed;

    // 2., 3. Matching PrintObjects in the print order; preferred = the one printing the analysed instance.
    const ObjectID     analysed_id = live_mo.instances[analysed]->id();
    const PrintObject *first       = nullptr;
    const PrintObject *preferred   = nullptr;
    for (const PrintObject *po : active.objects())
        if (po->model_object()->id() == live_mo.id()) {
            if (first == nullptr)
                first = po;
            if (preferred == nullptr && po_has_instance(*po, analysed_id))
                preferred = po;
        }

    const PrintObject *choice = nullptr;
    if (background_processing) {
        // 4. The whole print is processed: analyse the selected instance's PrintObject, no fallback.
        choice = preferred;
    } else if (first != nullptr) {
        // 5. Only the first matching PrintObject is processed (set_task_impl(), PrintBase.hpp:640-662).
        choice = first;
        if (first != preferred) {
            out.fallback = true;
            // The live index of the first printable instance of that PrintObject.
            bool found = false;
            for (const PrintInstance &pi : first->instances()) {
                for (size_t i = 0; i < live_mo.instances.size(); ++ i)
                    if (live_mo.instances[i]->id() == pi.model_instance->id() && live_mo.instances[i]->is_printable()) {
                        out.analysed_instance = i;
                        found = true;
                        break;
                    }
                if (found)
                    break;
            }
            if (! found)
                // The print does not match the live model (Print::apply pending).
                choice = nullptr;
        }
    }
    if (choice == nullptr) {
        out.why_null = PrintObjectChoice::Null::NotInPrint;
        return out;
    }
    out.po    = choice;
    // 6.
    out.ready = choice->is_step_done(posSlice);
    return out;
}

bool input_consistent(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance)
{
    if (! po.is_step_done(posSlice))
        return false;
    const ModelObject *mo_copy = po.model_object();
    if (mo_copy == nullptr || mo_copy->id() != live_mo.id() || analysed_instance >= live_mo.instances.size())
        return false;

    // The analysed instance: same matrix without offset and same z offset. XY may differ (bed shift of
    // with_single_bed_model_fff(), Plater.cpp:2486-2489).
    const ModelInstance *live    = live_mo.instances[analysed_instance];
    const ModelInstance *mi_copy = find_instance(*mo_copy, live->id());
    if (mi_copy == nullptr || ! po_has_instance(po, live->id()) ||
        ! mi_copy->get_matrix_no_offset().isApprox(live->get_matrix_no_offset()) ||
        std::abs(mi_copy->get_offset(Z) - live->get_offset(Z)) > EPSILON)
        return false;

    // All volumes (model parts, negative volumes, modifiers, support volumes).
    if (mo_copy->volumes.size() != live_mo.volumes.size())
        return false;
    for (size_t i = 0; i < live_mo.volumes.size(); ++ i) {
        const ModelVolume *a = mo_copy->volumes[i];
        const ModelVolume *b = live_mo.volumes[i];
        if (a->id() != b->id() || a->type() != b->type() || a->mesh_ptr() != b->mesh_ptr() ||
            ! a->get_matrix().isApprox(b->get_matrix()))
            return false;
    }
    return true;
}

AnalysisInput build_input(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance, const UserParams &up)
{
    assert(analysed_instance < live_mo.instances.size());

    AnalysisInput in;
    in.model_object_id       = live_mo.id();
    in.analysed_instance_idx = analysed_instance;
    in.po_trafo_centered     = po.trafo_centered();
    in.params                = derive_params(po, up);

    // Bed-local shift of the analysed instance (PrintInstance::shift = instance offset + center offset).
    if (analysed_instance < live_mo.instances.size()) {
        const ObjectID id = live_mo.instances[analysed_instance]->id();
        for (const PrintInstance &pi : po.instances())
            if (pi.model_instance->id() == id) {
                in.instance_shift_mm = unscale(pi.shift);
                break;
            }
    }

    // Layers.
    in.layers.reserve(po.layer_count());
    for (const Layer *layer : po.layers()) {
        LayerInput &li = in.layers.emplace_back();
        li.slice_z = layer->slice_z;
        li.print_z = layer->print_z;
        li.height  = layer->height;
        li.lslices = layer->lslices;
        li.slices.resize(li.lslices.size());
        if (layer->lslices_ex.size() == li.lslices.size()) {
            for (size_t i = 0; i < li.lslices.size(); ++ i) {
                const LayerSlice     &src = layer->lslices_ex[i];
                LayerInput::SliceInfo &dst = li.slices[i];
                dst.bbox = src.bbox;
                dst.below.reserve(src.overlaps_below.size());
                for (const LayerSlice::Link &link : src.overlaps_below)
                    dst.below.push_back({ link.slice_idx, link.area });
                dst.above.reserve(src.overlaps_above.size());
                for (const LayerSlice::Link &link : src.overlaps_above)
                    dst.above.push_back({ link.slice_idx, link.area });
            }
        } else {
            // Not expected after posSlice: no links.
            assert(false);
            for (size_t i = 0; i < li.lslices.size(); ++ i)
                li.slices[i].bbox = get_extents(li.lslices[i]);
        }
        // Average external perimeter width over the layer's regions (TreeSupport.cpp:259-266).
        if (layer->region_count() > 0) {
            double w = 0.;
            for (const LayerRegion *layerm : layer->regions())
                w += layerm->flow(frExternalPerimeter).width();
            li.ext_width_avg_mm = w / double(layer->region_count());
        } else
            li.ext_width_avg_mm = in.params.ext_width;
    }

    // Volumes: model parts with their paint, support enforcer / blocker volumes. Other modifiers and negative
    // volumes are already contained in the slices.
    const Transform3d trafo_centered = po.trafo_centered();
    for (const ModelVolume *mv : live_mo.volumes) {
        if (mv->is_model_part()) {
            VolumeInput &vi    = in.volumes.emplace_back();
            vi.volume_id       = mv->id();
            vi.mesh            = mv->mesh_ptr();
            vi.trafo           = trafo_centered * mv->get_matrix();
            vi.paint           = mv->supported_facets.get_data();
            vi.paint_timestamp = mv->supported_facets.timestamp();
        } else if (mv->is_support_enforcer() || mv->is_support_blocker()) {
            ModifierInput &mi = in.modifiers.emplace_back();
            mi.mesh           = mv->mesh_ptr();
            mi.trafo          = trafo_centered * mv->get_matrix();
            mi.enforcer       = mv->is_support_enforcer();
        }
    }

    // Other PrintObjects of the active print.
    in.num_print_objects    = 0;
    in.other_objects_on_bed = false;
    for (const PrintObject *other : po.print()->objects()) {
        if (other->model_object()->id() == live_mo.id())
            ++ in.num_print_objects;
        else
            in.other_objects_on_bed = true;
    }
    in.num_print_objects = std::max(in.num_print_objects, 1);

    in.contract = check_contract(po.config(), up.plate_policy, &in.params);
    return in;
}

} // namespace Slic3r::SmartSupports
