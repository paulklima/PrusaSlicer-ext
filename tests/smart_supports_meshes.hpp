#ifndef slic3r_tests_smart_supports_meshes_hpp_
#define slic3r_tests_smart_supports_meshes_hpp_

// Smart support painting tests (plan §7): meshes, synthetic analysis inputs and test parameters.
// Header only, libslic3r only (no Print): usable from tests/libslic3r and tests/fff_print.
//
// All lengths in mm. A shape is a list of parts (Parts): closed, outward oriented solids in one common frame
// that may overlap. synthetic_input() unites them per layer; init_print_parts() (fff_print) adds one model part
// per mesh. WP-specific shapes belong to the WP's own test file (plan §7 rules).

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Support/SmartSupports.hpp"

namespace Slic3r::SmartSupports::Testing {

using Parts = std::vector<indexed_triangle_set>;

// ---------------------------------------------------------------------------------------------------------------
// 2D helpers (input in mm, output scaled)

// Polygon from mm points, counter-clockwise for contours, clockwise for holes.
inline Polygon polygon_mm(const std::vector<Vec2d> &pts)
{
    Polygon out;
    out.points.reserve(pts.size());
    for (const Vec2d &p : pts)
        out.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    return out;
}

// Axis aligned rectangle [x0, x1] × [y0, y1].
inline ExPolygon rectangle_mm(double x0, double y0, double x1, double y1)
{
    return ExPolygon(polygon_mm({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }));
}

// Regular polygon approximating a circle, counter-clockwise.
inline Polygon circle_mm(const Vec2d &center, double r, int segments = 128)
{
    std::vector<Vec2d> pts;
    pts.reserve(segments);
    for (int i = 0; i < segments; ++ i) {
        const double a = 2. * M_PI * double(i) / double(segments);
        pts.emplace_back(center.x() + r * cos(a), center.y() + r * sin(a));
    }
    return polygon_mm(pts);
}

// ---------------------------------------------------------------------------------------------------------------
// Generic meshes

inline indexed_triangle_set translate(indexed_triangle_set its, const Vec3d &v)
{
    its_translate(its, Vec3f(v.cast<float>()));
    return its;
}

// Rotation around the x axis by deg degrees (counter-clockwise looking from +x): (x, y, z) → (x, y·c − z·s, y·s + z·c).
inline indexed_triangle_set rotate_x(indexed_triangle_set its, double deg)
{
    Transform3d t = Transform3d::Identity();
    t.rotate(Eigen::AngleAxisd(deg * M_PI / 180., Vec3d::UnitX()));
    its_transform(its, t);
    return its;
}

// Axis aligned box [min, max].
inline indexed_triangle_set box(const Vec3d &min, const Vec3d &max)
{
    const Vec3d size = max - min;
    return translate(its_make_cube(size.x(), size.y(), size.z()), min);
}

// Prism of the scaled shape from z0 to z0 + h. Caps from triangulate_expolygon_3d(), walls per contour edge,
// vertices merged (exact coordinates), so the mesh is closed.
inline indexed_triangle_set extrude(const ExPolygon &shape, double h, double z0 = 0.)
{
    const double z1 = z0 + h;
    indexed_triangle_set its;
    its_merge(its, triangulate_expolygon_3d(shape, z0, true));    // bottom, normals down
    its_merge(its, triangulate_expolygon_3d(shape, z1, false));   // top, normals up
    auto add_walls = [&its, z0, z1](const Polygon &poly) {
        for (size_t i = 0; i < poly.size(); ++ i) {
            const Point &a = poly.points[i];
            const Point &b = poly.points[(i + 1) % poly.size()];
            const int    idx = int(its.vertices.size());
            its.vertices.emplace_back(Vec3d(unscale<double>(a.x()), unscale<double>(a.y()), z0).cast<float>());
            its.vertices.emplace_back(Vec3d(unscale<double>(b.x()), unscale<double>(b.y()), z0).cast<float>());
            its.vertices.emplace_back(Vec3d(unscale<double>(b.x()), unscale<double>(b.y()), z1).cast<float>());
            its.vertices.emplace_back(Vec3d(unscale<double>(a.x()), unscale<double>(a.y()), z1).cast<float>());
            // Contours are CCW, holes CW: the solid is left of every edge, the wall faces right (outwards).
            its.indices.emplace_back(idx, idx + 1, idx + 2);
            its.indices.emplace_back(idx, idx + 2, idx + 3);
        }
    };
    add_walls(shape.contour);
    for (const Polygon &hole : shape.holes)
        add_walls(hole);
    its_merge_vertices(its);
    return its;
}

// Prism of a profile drawn in the (x, z) plane (ExPolygon x → x, y → z), extruded along y from 0 to width.
inline indexed_triangle_set extrude_xz(const ExPolygon &profile_xz, double width)
{
    return translate(rotate_x(extrude(profile_xz, width), 90.), Vec3d(0., width, 0.));
}

// Solid of revolution around the z axis. rz: closed profile polygon in the (r, z) half plane, r ≥ 0,
// counter-clockwise with r to the right and z up; points with r == 0 lie on the axis (one vertex).
inline indexed_triangle_set revolve(const std::vector<Vec2d> &rz, int segments)
{
    indexed_triangle_set its;
    std::vector<int>     first(rz.size());
    auto on_axis = [&rz](size_t i) { return rz[i].x() <= 1e-9; };
    for (size_t i = 0; i < rz.size(); ++ i) {
        first[i] = int(its.vertices.size());
        if (on_axis(i))
            its.vertices.emplace_back(0.f, 0.f, float(rz[i].y()));
        else
            for (int k = 0; k < segments; ++ k) {
                const double a = 2. * M_PI * double(k) / double(segments);
                its.vertices.emplace_back(float(rz[i].x() * cos(a)), float(rz[i].x() * sin(a)), float(rz[i].y()));
            }
    }
    auto vertex = [&](size_t i, int k) { return on_axis(i) ? first[i] : first[i] + k % segments; };
    for (size_t i = 0; i < rz.size(); ++ i) {
        const size_t j = (i + 1) % rz.size();
        if (on_axis(i) && on_axis(j))
            continue;
        for (int k = 0; k < segments; ++ k) {
            const int a0 = vertex(i, k), a1 = vertex(i, k + 1), b0 = vertex(j, k), b1 = vertex(j, k + 1);
            if (! on_axis(i))
                its.indices.emplace_back(a0, a1, b1);
            if (! on_axis(j))
                its.indices.emplace_back(a0, b1, b0);
        }
    }
    return its;
}

// Arc points (r cos t, r sin t) in the (r, z) plane for t from deg0 to deg1, steps + 1 points; values snapped
// to 0 at the axis.
inline std::vector<Vec2d> arc_rz(double r, double deg0, double deg1, int steps)
{
    std::vector<Vec2d> out;
    for (int i = 0; i <= steps; ++ i) {
        const double t = (deg0 + (deg1 - deg0) * double(i) / double(steps)) * M_PI / 180.;
        Vec2d p(r * cos(t), r * sin(t));
        if (std::abs(p.x()) < 1e-9) p.x() = 0.;
        if (std::abs(p.y()) < 1e-9) p.y() = 0.;
        out.push_back(p);
    }
    return out;
}

// Triangular prism with a sloped underside at angle_deg from horizontal (overhang facing −z/+x), along y.
// Profile (x, z): (0, 0), (length, length·tan α), (0, length·tan α).
inline indexed_triangle_set wedge(double angle_deg, double length = 20., double width = 10.)
{
    const double h = length * tan(angle_deg * M_PI / 180.);
    return extrude_xz(ExPolygon(polygon_mm({ { 0., 0. }, { length, h }, { 0., h } })), width);
}

// ---------------------------------------------------------------------------------------------------------------
// Named shapes (reference cases of plan §7.2 / theory §10)

// Overhanging stairs: base block (base_w × width × base_h), then `steps` slabs of height `riser`, step k
// reaching base_w + k·tread in +x.
inline Parts stairs(double tread = 2.5, double riser = 0.4, int steps = 6, double width = 10., double base_w = 10., double base_h = 5.)
{
    Parts out { box({ 0., 0., 0. }, { base_w, width, base_h }) };
    for (int k = 1; k <= steps; ++ k)
        out.push_back(box({ 0., 0., base_h + (k - 1) * riser }, { base_w + k * tread, width, base_h + k * riser }));
    return out;
}

// Cube with a horizontal shelf (depth in +x, length along y, centred) flush with the cube top.
// The shelf overlaps the cube by 1 mm.
inline Parts shelf_cube(double depth, double length, double cube = 20., double thickness = 2.)
{
    return { box({ 0., 0., 0. }, { cube, cube, cube }),
             box({ cube - 1., 0.5 * (cube - length), cube - thickness }, { cube + depth, 0.5 * (cube + length), cube }) };
}

// T: a stem (stem × width × height) centred at x = 0 and a bar on top with two arms of length `arm`.
// base_size > 0: the T stands on a base plate base_size × base_size × base_h (case #10).
inline Parts t_shape(double arm = 15., double width = 10., double stem = 10., double height = 20., double thickness = 2.,
                     double base_size = 0., double base_h = 2.)
{
    const double z0 = base_size > 0. ? base_h : 0.;
    Parts out { box({ -0.5 * stem, -0.5 * width, z0 }, { 0.5 * stem, 0.5 * width, z0 + height }),
                box({ -0.5 * stem - arm, -0.5 * width, z0 + height - thickness }, { 0.5 * stem + arm, 0.5 * width, z0 + height }) };
    if (base_size > 0.)
        out.push_back(box({ -0.5 * base_size, -0.5 * base_size, 0. }, { 0.5 * base_size, 0.5 * base_size, base_h }));
    return out;
}

// Open hemispherical shell (inner radius r_in, outer r_out) standing on its rim, apex up (cases #7, #8).
inline Parts dome(double r_in = 20., double r_out = 25., int segments = 128, int arc_steps = 64)
{
    std::vector<Vec2d> rz;
    for (const Vec2d &p : arc_rz(r_out, 0., 90., arc_steps))    // (r_out, 0) … (0, r_out)
        rz.push_back(p);
    std::vector<Vec2d> inner = arc_rz(r_in, 90., 0., arc_steps); // (0, r_in) … (r_in, 0)
    rz.insert(rz.end(), inner.begin(), inner.end());
    return { revolve(rz, segments) };
}

// Block w (x) × depth (y) × h (z) with a horizontal through-hole of diameter hole_d along y, centred (case #9).
inline Parts hole_block(double hole_d, double w = 20., double depth = 20., double h = 20., int segments = 128)
{
    ExPolygon profile = rectangle_mm(0., 0., w, h);
    Polygon   hole    = circle_mm(Vec2d(0.5 * w, 0.5 * h), 0.5 * hole_d, segments);
    hole.reverse();
    profile.holes.push_back(std::move(hole));
    return { extrude_xz(profile, depth) };
}

// Wall leaning towards +x, its underside at angle_deg from horizontal (cases #13, #14). Horizontal thickness
// `thickness`, length along y.
inline Parts slope_wall(double angle_deg = 40., double height = 50., double thickness = 5., double length = 20.)
{
    const double s = height / tan(angle_deg * M_PI / 180.);
    return { extrude_xz(ExPolygon(polygon_mm({ { 0., 0. }, { thickness, 0. }, { thickness + s, height }, { s, height } })), length) };
}

// Inverted cone (apex down at z = apex_z, flanks at flank_deg from horizontal, height cone_h, axis z at x = y = 0)
// (cases #26, #26b). apex_z > 0: the cone hangs from a 2 mm top plate carried by a 5 × 5 post at x > R + 5.
inline Parts inverted_cone(double flank_deg = 60., double cone_h = 10., double apex_z = 5., int segments = 128)
{
    const double R     = cone_h / tan(flank_deg * M_PI / 180.);
    const double z_top = apex_z + cone_h;
    Parts out { revolve({ { 0., apex_z }, { R, z_top }, { 0., z_top } }, segments) };
    if (apex_z > 0.) {
        out.push_back(box({ -R, -R, z_top - 0.5 }, { R + 10., R, z_top + 2. }));
        out.push_back(box({ R + 5., -2.5, 0. }, { R + 10., 2.5, z_top }));
    }
    return out;
}

// Sphere-heavy figurine proxy: spheres R 20 / 15 / 10 stacked on a cylinder R 8 × 20 (benchmark #25).
inline Parts figurine_proxy()
{
    const double fa = M_PI / 45.;
    return { its_make_cylinder(8., 20., fa),
             translate(its_make_sphere(20., fa), Vec3d(0., 0., 36.)),
             translate(its_make_sphere(15., fa), Vec3d(0., 0., 65.)),
             translate(its_make_sphere(10., fa), Vec3d(0., 0., 86.)) };
}

// ---------------------------------------------------------------------------------------------------------------
// Analysis inputs

// Params of the §7 default config (layer 0.2, first 0.2, nozzle 0.4, organic, threshold 45, contact 0.2,
// raft 0) with interface layers 0 and verification off.
inline Params test_params()
{
    Params params;
    params.user.alpha_deg  = 45;
    params.interface_layers = 0;
    params.verification    = false;
    return params;
}

// Analysis input of the parts without a Print: object frame = mesh frame (identity trafos, no raft).
// Layers: [0, first_h], then layer_h each up to the top of the parts; slice_z = slab centre.
// Slices: slice_mesh_ex() per part at slice_z (closing radius 0.049 as the slicer), united per layer.
// Links: bbox-filtered intersection areas of the slices shrunk by 1 µm (as Layer::build_up_down_graph()).
// One VolumeInput per part (ids 1000 + i, empty paint), no modifiers, ext_width_avg_mm = params.ext_width.
// params.layer_height / first_layer_height are set to layer_h / first_h.
inline AnalysisInput synthetic_input(const std::vector<indexed_triangle_set> &parts, double layer_h, double first_h, const Params &params)
{
    AnalysisInput in;
    in.model_object_id           = ObjectID(999);
    in.params                    = params;
    in.params.layer_height       = layer_h;
    in.params.first_layer_height = first_h;

    double z_max = 0.;
    for (const indexed_triangle_set &its : parts)
        for (const stl_vertex &v : its.vertices)
            z_max = std::max(z_max, double(v.z()));

    // Layering.
    std::vector<float> zs;
    for (double lo = 0.; lo + EPSILON < z_max;) {
        const double h = in.layers.empty() ? first_h : layer_h;
        LayerInput &li = in.layers.emplace_back();
        li.height  = h;
        li.slice_z = lo + 0.5 * h;
        li.print_z = lo + h;
        li.ext_width_avg_mm = params.ext_width;
        zs.push_back(float(li.slice_z));
        lo += h;
    }

    // Slices.
    MeshSlicingParamsEx slicing_params;
    slicing_params.closing_radius = 0.049f;
    std::vector<ExPolygons> slices(zs.size());
    for (const indexed_triangle_set &its : parts) {
        std::vector<ExPolygons> part_slices = slice_mesh_ex(its, zs, slicing_params);
        for (size_t n = 0; n < zs.size() && n < part_slices.size(); ++ n)
            append(slices[n], std::move(part_slices[n]));
    }
    for (size_t n = 0; n < zs.size(); ++ n) {
        LayerInput &li = in.layers[n];
        li.lslices = parts.size() > 1 ? union_ex(slices[n]) : std::move(slices[n]);
        li.slices.resize(li.lslices.size());
        for (size_t i = 0; i < li.lslices.size(); ++ i)
            li.slices[i].bbox = get_extents(li.lslices[i]);
    }

    // Links between successive layers.
    std::vector<ExPolygons> shrunk_below;
    for (size_t n = 0; n < in.layers.size(); ++ n) {
        LayerInput             &li = in.layers[n];
        std::vector<ExPolygons> shrunk;
        shrunk.reserve(li.lslices.size());
        for (const ExPolygon &ex : li.lslices)
            shrunk.push_back(shrink_ex({ ex }, scaled<float>(0.001)));
        if (n > 0) {
            LayerInput &below = in.layers[n - 1];
            for (size_t i = 0; i < li.lslices.size(); ++ i)
                for (size_t j = 0; j < below.lslices.size(); ++ j)
                    if (li.slices[i].bbox.overlap(below.slices[j].bbox)) {
                        const double a = area(intersection_ex(shrunk[i], shrunk_below[j]));
                        if (a > 0.) {
                            li.slices[i].below.push_back({ int32_t(j), float(a) });
                            below.slices[j].above.push_back({ int32_t(i), float(a) });
                        }
                    }
        }
        shrunk_below = std::move(shrunk);
    }

    // One model part per mesh.
    for (size_t i = 0; i < parts.size(); ++ i) {
        VolumeInput &vi = in.volumes.emplace_back();
        vi.volume_id = ObjectID(1000 + i);
        vi.mesh      = std::make_shared<const TriangleMesh>(parts[i]);
        vi.trafo     = Transform3d::Identity();
    }
    return in;
}

} // namespace Slic3r::SmartSupports::Testing

#endif // slic3r_tests_smart_supports_meshes_hpp_
