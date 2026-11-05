// Smart support painting: projection of paint states and support volumes onto the layers (plan §3.2, §3.5; WP1a).
// Both functions mirror the slicer, so that the analysis sees the manual paint and the support enforcer / blocker
// volumes exactly as the support generator does: PrintObject::project_and_append_custom_facets() and
// PrintObject::slice_support_volumes().

#include "SmartSupportsInternal.hpp"

#include <cassert>
#include <cmath>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::SmartSupports {

// Slicing planes of the layers, as zs_from_layers().
static std::vector<float> layer_zs(const AnalysisInput &in)
{
    std::vector<float> zs;
    zs.reserve(in.layers.size());
    for (const LayerInput &layer : in.layers)
        zs.emplace_back(float(layer.slice_z));
    return zs;
}

// Facets in the slicer's slicing frame (WP5): slice_mesh_slabs() converts the vertices of the slab and horizontal edges
// by truncation (to_2d(v).cast<coord_t>()), those of the edges lying on a slicing plane by rounding
// (v3f_scaled_to_contour_point(), TriangleMeshSlicer.cpp). A vertex exactly on a slicing plane - TriangleSelector's
// midpoint splits put many there, e.g. z 7.5 on a flank from z 5 to 15 - then gets two positions 1 nm apart and
// chain_lines_by_triangle_connectivity() asserts (Release: a 1 nm kink; the slicer's own projection of the same paint is
// affected the same way). The vertices are therefore transformed exactly as transform_mesh_vertices_for_slicing() does
// (same float z, so the same slabs) and XY is rounded to whole scaled units, where both conversions agree. A mirroring
// trafo flips the triangles, so that the face orientation stays the slicer's. Returns the trafo to pass with them.
static Transform3d to_slicing_frame(indexed_triangle_set &its, const Transform3d &trafo)
{
    static constexpr const double s = 1. / SCALING_FACTOR;
    if (trafo.matrix() == Transform3d::Identity().matrix()) {          // as the slicer's is_identity()
        for (stl_vertex &v : its.vertices) {
            v.x() = std::round(v.x() * float(s));
            v.y() = std::round(v.y() * float(s));
        }
    } else {
        Transform3d t = trafo;
        t.prescale(Vec3d(s, s, 1.));
        const Transform3f tf = t.cast<float>();
        for (stl_vertex &v : its.vertices) {
            v     = tf * v;
            v.x() = std::round(v.x());
            v.y() = std::round(v.y());
        }
        if (trafo.matrix().block(0, 0, 3, 3).determinant() < 0.)
            its_flip_triangles(its);
    }
    // slice_mesh_slabs() prescales by (s, s, 1) again: diag(SCALING_FACTOR·s, ·, 1) becomes the float identity.
    Transform3d back = Transform3d::Identity();
    back.matrix()(0, 0) = SCALING_FACTOR;
    back.matrix()(1, 1) = SCALING_FACTOR;
    return back;
}

// Concatenates the per volume results layer by layer, in volume order (deterministic, plan §3.12).
static std::vector<Polygons> append_per_volume(std::vector<std::vector<Polygons>> &&per_volume, size_t num_layers)
{
    std::vector<Polygons> out(num_layers);
    for (std::vector<Polygons> &layers : per_volume) {
        assert(layers.empty() || layers.size() == num_layers);
        for (size_t layer_idx = 0; layer_idx < num_layers && layer_idx < layers.size(); ++ layer_idx)
            append(out[layer_idx], std::move(layers[layer_idx]));
    }
    return out;
}

std::vector<Polygons> project_states(const AnalysisInput &in, const std::vector<const TriangleSelector::TriangleSplittingData*> &per_volume,
                                     const std::function<bool(TriangleStateType)> &filter, const ThrowOnCancel &cancel)
{
    assert(per_volume.size() == in.volumes.size());
    const ThrowOnCancel throw_on_cancel = cancel ? cancel : ThrowOnCancel([]() {});

    std::vector<TriangleStateType> states;
    for (size_t state = 0; state < TRIANGLE_STATE_TYPE_COUNT; ++ state)
        if (filter(TriangleStateType(state)))
            states.emplace_back(TriangleStateType(state));
    // Unpainted facets are NONE: a volume without paint contributes only if NONE passes the filter.
    const bool none_passes = ! states.empty() && states.front() == TriangleStateType::NONE;

    const std::vector<float>           zs = layer_zs(in);
    std::vector<std::vector<Polygons>> projected(in.volumes.size());
    if (! states.empty() && ! zs.empty())
        tbb::parallel_for(tbb::blocked_range<size_t>(0, in.volumes.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t volume_idx = range.begin(); volume_idx < range.end(); ++ volume_idx) {
                throw_on_cancel();
                const VolumeInput                             &volume = in.volumes[volume_idx];
                const TriangleSelector::TriangleSplittingData &paint  = volume_idx < per_volume.size() && per_volume[volume_idx] ?
                    *per_volume[volume_idx] : volume.paint;
                if (! volume.mesh || (paint.triangles_to_split.empty() && ! none_passes))
                    continue;
                // As FacetsAnnotation::get_facets_strict(): T-joints triangulated.
                TriangleSelector selector(*volume.mesh);
                selector.deserialize(paint, false);
                indexed_triangle_set facets = selector.get_facets_strict(states);
                if (facets.indices.empty())
                    continue;
                // As PrintObject::project_and_append_custom_facets(): the downward facing facets are projected upwards
                // onto the slicing plane of their layer.
                const Transform3d trafo = to_slicing_frame(facets, volume.trafo);
                slice_mesh_slabs(facets, zs, trafo, nullptr, &projected[volume_idx], throw_on_cancel);
            }
        });
    return append_per_volume(std::move(projected), in.layers.size());
}

std::vector<Polygons> slice_modifiers(const AnalysisInput &in, bool enforcers, const ThrowOnCancel &cancel)
{
    const ThrowOnCancel throw_on_cancel = cancel ? cancel : ThrowOnCancel([]() {});

    const std::vector<float>           zs = layer_zs(in);
    std::vector<std::vector<Polygons>> sliced(in.modifiers.size());
    if (! zs.empty())
        tbb::parallel_for(tbb::blocked_range<size_t>(0, in.modifiers.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
            for (size_t modifier_idx = range.begin(); modifier_idx < range.end(); ++ modifier_idx) {
                const ModifierInput &modifier = in.modifiers[modifier_idx];
                if (modifier.enforcer != enforcers || ! modifier.mesh || modifier.mesh->its.indices.empty())
                    continue;
                throw_on_cancel();
                // As slice_volume() of PrintObject::slice_support_volumes(): no closing radius, no offset, no
                // simplification. A mirroring transformation reverses the orientation of the facets.
                indexed_triangle_set its = modifier.mesh->its;
                if (modifier.trafo.linear().determinant() < 0.)
                    its_flip_triangles(its);
                MeshSlicingParamsEx params;
                params.trafo = modifier.trafo;
                std::vector<ExPolygons> slices = slice_mesh_ex(its, zs, params, throw_on_cancel);
                std::vector<Polygons>  &out    = sliced[modifier_idx];
                out.reserve(slices.size());
                for (ExPolygons &expolygons : slices)
                    out.emplace_back(to_polygons(std::move(expolygons)));
            }
        });
    return append_per_volume(std::move(sliced), in.layers.size());
}

} // namespace Slic3r::SmartSupports
