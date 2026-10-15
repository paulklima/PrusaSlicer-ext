#ifndef slic3r_tests_smart_supports_test_utils_hpp_
#define slic3r_tests_smart_supports_test_utils_hpp_

// Smart support painting tests on sliced prints (plan §7): multi-part prints, posSlice-only processing,
// a print cache and run_sas() through the real build_input().

#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "test_data.hpp"
#include "smart_supports_meshes.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SmartSupports.hpp"

namespace Slic3r::SmartSupports::Testing {

// The §7 default config: layer 0.2, first 0.2, nozzle 0.4, perimeters 2, support on, auto off, organic,
// threshold 45, contact 0.2, enforce 0, buildplate_only 0, raft 0, interface layers 0; then the overrides.
inline DynamicPrintConfig sas_config(std::initializer_list<ConfigBase::SetDeserializeItem> overrides = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", 0.2 }, { "first_layer_height", 0.2 }, { "nozzle_diameter", "0.4" }, { "perimeters", 2 },
        { "support_material", true }, { "support_material_auto", false }, { "support_material_style", "organic" },
        { "support_material_threshold", 45 }, { "support_material_contact_distance", 0.2 },
        { "support_material_enforce_layers", 0 }, { "support_material_buildplate_only", false }, { "raft_layers", 0 },
        { "support_material_interface_layers", 0 } });
    config.set_deserialize_strict(overrides);
    return config;
}

// One ModelObject with one model part per mesh (all meshes in one frame), one instance, centred at (100, 100)
// and dropped onto the bed; print.apply() and validate() (throws on a validation error). The model must outlive
// the print. Returns the (live) ModelObject.
inline ModelObject* init_print_parts(const Parts &parts, Print &print, Model &model, const DynamicPrintConfig &config)
{
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(config);
    ModelObject *object = model.add_object();
    object->name = "smart_supports_object";
    for (const indexed_triangle_set &its : parts)
        object->add_volume(TriangleMesh(its));
    object->add_instance();
    model.center_instances_around_point({ 100., 100. });
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, full_config);
    if (std::string err = print.validate(); ! err.empty())
        throw std::runtime_error("init_print_parts: " + err);
    print.set_status_silent();
    return object;
}

// Process all PrintObjects up to posSlice only (no perimeters, no support).
inline void process_to_slices(Print &print)
{
    Print::TaskParams task;
    task.to_object_step = int(posSlice);
    print.set_task(task);
    print.process();
    print.finalize();
}

// A live model with its print.
struct SasPrint {
    Model model;
    Print print;
    ModelObject*       model_object()       { return model.objects.front(); }
    const PrintObject* print_object() const { return print.objects().front(); }
};

// New print of the parts, processed to posSlice.
inline std::unique_ptr<SasPrint> make_sliced_print(const Parts &parts, const DynamicPrintConfig &config)
{
    auto out = std::make_unique<SasPrint>();
    init_print_parts(parts, out->print, out->model, config);
    process_to_slices(out->print);
    return out;
}

// Sliced print cache for the lifetime of the test binary. The key identifies the parts (e.g. "shelf_10x15"); the
// config is added to the key automatically. Never modify a cached print or its model: use make_sliced_print().
inline const SasPrint& cached_print(const std::string &key, const std::function<Parts()> &make_parts, const DynamicPrintConfig &config)
{
    static std::map<std::string, std::unique_ptr<SasPrint>> cache;
    std::string full_key = key;
    for (const std::string &opt_key : config.keys())
        full_key += "\n" + opt_key + "=" + config.opt_serialize(opt_key);
    auto it = cache.find(full_key);
    if (it == cache.end())
        it = cache.emplace(full_key, make_sliced_print(make_parts(), config)).first;
    return *it->second;
}

// The analysis input and result of one run.
struct SasRun {
    AnalysisInput input;
    Result        result;
};

// Analysis of the first PrintObject of a sliced print, its own ModelObject copy acting as the live object
// (instance 0) through the real build_input(). Params::verification is set to false (plan §7 rules), then
// adjust (if any) may change any Params value; RunOptions::verification can still force it. Never canceled.
inline SasRun run_sas(const Print &print, const UserParams &up = {}, const RunOptions &opts = {},
                      const std::function<void(Params&)> &adjust = nullptr)
{
    if (print.objects().empty())
        throw std::runtime_error("run_sas: empty print");
    const PrintObject &po = *print.objects().front();
    const ModelObject &mo = *po.model_object();
    if (! input_consistent(po, mo, 0))
        throw std::runtime_error("run_sas: print not sliced or inconsistent");
    SasRun out;
    out.input = build_input(po, mo, 0, up);
    out.input.params.verification = false;
    if (adjust)
        adjust(out.input.params);
    out.result = run(out.input, opts, []() { return false; }, [](int) {});
    return out;
}

} // namespace Slic3r::SmartSupports::Testing

#endif // slic3r_tests_smart_supports_test_utils_hpp_
