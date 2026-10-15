// Smart support painting: 3MF round trip, bake for stock PrusaSlicer, legacy files (plan §5, §7.1; WP1a).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using Catch::Approx;

namespace {

constexpr const char *MODEL_FILE = "3D/3dmodel.model";
constexpr const char *EXT_ATTR   = "slic3rpe:ext_custom_supports";

double facets_area(const indexed_triangle_set &its)
{
    double area = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d p0 = its.vertices[f(0)].cast<double>(), p1 = its.vertices[f(1)].cast<double>(), p2 = its.vertices[f(2)].cast<double>();
        area += 0.5 * (p1 - p0).cross(p2 - p0).norm();
    }
    return area;
}

double state_area(const ModelVolume &mv, TriangleStateType state)
{
    return facets_area(mv.supported_facets.get_facets_strict(mv, state));
}

double state_area(const ModelVolume &mv, const std::vector<TriangleStateType> &states)
{
    return facets_area(mv.supported_facets.get_facets_strict(mv, states));
}

// Paints the volume's mesh with the selector operations of paint.
void paint(ModelVolume &mv, const std::function<void(TriangleSelector&, const TriangleMesh&)> &ops)
{
    TriangleSelector selector(mv.mesh());
    selector.deserialize(mv.supported_facets.get_data());
    ops(selector, mv.mesh());
    mv.supported_facets.set(selector);
}

// Bottom facets (lowest z) of a mesh.
std::vector<int> bottom_facets(const TriangleMesh &mesh)
{
    const float z_min = mesh.bounding_box().min.cast<float>().z();
    std::vector<int> out;
    for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[i];
        if (mesh.its.vertices[f(0)].z() == z_min && mesh.its.vertices[f(1)].z() == z_min && mesh.its.vertices[f(2)].z() == z_min)
            out.push_back(int(i));
    }
    return out;
}

// Sphere brush stroke at the centroid of an original facet.
void paint_sphere(TriangleSelector &selector, const TriangleMesh &mesh, int facet, float radius, TriangleStateType state)
{
    const stl_triangle_vertex_indices &f = mesh.its.indices[facet];
    const Vec3f centroid = (mesh.its.vertices[f(0)] + mesh.its.vertices[f(1)] + mesh.its.vertices[f(2)]) / 3.f;
    const Transform3d trafo = Transform3d::Identity();
    selector.select_patch(facet,
        TriangleSelector::SinglePointCursor::cursor_factory(centroid, centroid - Vec3f(0.f, 0.f, 10.f), radius,
            TriangleSelector::CursorType::SPHERE, trafo, TriangleSelector::ClippingPlane()),
        state, trafo, true);
}

// Smart paint of the whole bottom with the rule of one cause bit.
void paint_bottom_smart(TriangleSelector &selector, const TriangleMesh &mesh, uint8_t cause_bit)
{
    const float z_min = mesh.bounding_box().min.cast<float>().z();
    TriangleSelector::PredicateCursor::Predicates predicates;
    predicates.point_inside = [z_min](const Vec3f &p) { return p.z() < z_min + 1e-3f; };
    predicates.seed_facets  = bottom_facets(mesh);
    REQUIRE(! predicates.seed_facets.empty());
    selector.set_paint_rule(tool_paint_rule(cause_bit));
    selector.select_patch(predicates.seed_facets.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.1f, Transform3d::Identity()),
                          auto_state(cause_bit), Transform3d::Identity(), true, 89.99f);
    selector.clear_paint_rule();
}

// One object with three cube volumes: 0 smart and manual paint (states 0, 1, 2, 3, 5, 9 and split facets),
// 1 manual paint only, 2 unpainted.
void make_model(Model &model, bool smart)
{
    ModelObject *mo = model.add_object();
    mo->name = "smart_supports_3mf";
    for (int i = 0; i < 3; ++ i) {
        ModelVolume *mv = mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        mv->set_offset(Vec3d(15. * i, 0., 0.));
    }
    paint(*mo->volumes[0], [smart](TriangleSelector &s, const TriangleMesh &mesh) {
        const std::vector<int> bottom = bottom_facets(mesh);
        // States 0, 1, 2, 3, 5, 9 on the side facets.
        int facet = 0;
        for (const int state : { 0, 1, 2, 3, 5, 9 }) {
            while (std::find(bottom.begin(), bottom.end(), facet) != bottom.end())
                ++ facet;
            if (smart || state <= 2)
                s.set_facet(facet, TriangleStateType(state));
            ++ facet;
        }
        // Manual spots and smart paint around them on the bottom.
        paint_sphere(s, mesh, bottom.front(), 1.5f, TriangleStateType::ENFORCER);
        paint_sphere(s, mesh, bottom.back(), 1.5f, TriangleStateType::BLOCKER);
        if (smart) {
            paint_bottom_smart(s, mesh, cbIsland);
            paint_bottom_smart(s, mesh, cbCantilever);
        }
    });
    paint(*mo->volumes[1], [](TriangleSelector &s, const TriangleMesh &mesh) {
        const std::vector<int> bottom = bottom_facets(mesh);
        paint_sphere(s, mesh, bottom.front(), 2.f, TriangleStateType::ENFORCER);
        paint_sphere(s, mesh, bottom.back(), 2.f, TriangleStateType::BLOCKER);
    });
    mo->add_instance();
    mo->instances.front()->set_offset(Vec3d(100., 100., 0.));
    REQUIRE(mo->volumes[0]->supported_facets.has_auto_states() == smart);
    REQUIRE(! mo->volumes[1]->supported_facets.has_auto_states());
    REQUIRE(mo->volumes[2]->supported_facets.empty());
}

// Unique temporary file (tests of several binaries may run in parallel).
std::string temp_file(const std::string &name)
{
    return (boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sas-%%%%-%%%%-%%%%-" + name)).string();
}

void store(const std::string &path, Model &model)
{
    REQUIRE(store_3mf(path.c_str(), &model, nullptr, false));
}

void load(const std::string &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
    boost::optional<Semver>   version;
    REQUIRE(load_3mf(path.c_str(), config, ctxt, &model, false, version));
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 3);
}

using ZipEntries = std::vector<std::pair<std::string, std::string>>;

ZipEntries read_zip(const std::string &path)
{
    ZipEntries     out;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++ i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        size_t size = 0;
        void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        REQUIRE(data != nullptr);
        out.emplace_back(stat.m_filename, std::string(static_cast<const char*>(data), size));
        mz_free(data);
    }
    close_zip_reader(&zip);
    return out;
}

void write_zip(const std::string &path, const ZipEntries &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path));
    for (const auto &[name, data] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
}

std::string& model_file(ZipEntries &entries)
{
    auto it = std::find_if(entries.begin(), entries.end(), [](const auto &e) { return e.first == MODEL_FILE; });
    REQUIRE(it != entries.end());
    return it->second;
}

size_t count_substr(const std::string &s, const std::string &what)
{
    size_t cnt = 0;
    for (size_t pos = s.find(what); pos != std::string::npos; pos = s.find(what, pos + what.size()))
        ++ cnt;
    return cnt;
}

} // namespace

TEST_CASE("3MF round trip keeps smart paint", "[SmartSupports][sas-3mf]")
{
    Model src;
    make_model(src, true);
    const std::string path = temp_file("roundtrip.3mf");
    store(path, src);

    ZipEntries entries = read_zip(path);
    const std::string &xml = model_file(entries);
    // The fork attribute is written for the smart painted volume only (7 of its 12 facets are painted: states 1, 2, 3,
    // 5, 9 and the bottom), the standard attribute for both painted volumes; the painting version stays 1.
    CHECK(count_substr(xml, EXT_ATTR) == 7);
    CHECK(count_substr(xml, "slic3rpe:custom_supports=") == 7 + 2);
    CHECK(xml.find("FdmSupportsPaintingVersion\">1<") != std::string::npos);

    Model dst;
    load(path, dst);
    boost::filesystem::remove(path);
    for (size_t i = 0; i < 3; ++ i) {
        INFO("volume " << i);
        const ModelVolume &a = *src.objects.front()->volumes[i];
        const ModelVolume &b = *dst.objects.front()->volumes[i];
        CHECK(FacetsAnnotation::same_paint(a.supported_facets.get_data(), b.supported_facets.get_data()));
    }
    const ModelVolume &smart = *dst.objects.front()->volumes.front();
    CHECK(smart.supported_facets.has_auto_states());
    for (const int state : { 1, 2, 3, 5, 9 })
        CHECK(smart.supported_facets.has_facets(smart, TriangleStateType(state)));
}

TEST_CASE("3MF bake: stock PrusaSlicer sees enforcers", "[SmartSupports][sas-3mf]")
{
    Model src;
    make_model(src, true);
    const std::string path          = temp_file("bake.3mf");
    const std::string path_stripped = temp_file("bake-stripped.3mf");
    store(path, src);

    // What stock PrusaSlicer reads: the file without the fork attribute.
    ZipEntries   entries = read_zip(path);
    std::string &xml     = model_file(entries);
    REQUIRE(count_substr(xml, EXT_ATTR) > 0);
    xml = std::regex_replace(xml, std::regex(std::string(" ") + EXT_ATTR + "=\"[0-9A-F]*\""), "");
    REQUIRE(count_substr(xml, EXT_ATTR) == 0);
    write_zip(path_stripped, entries);

    Model dst;
    load(path_stripped, dst);
    boost::filesystem::remove(path);
    boost::filesystem::remove(path_stripped);

    const ModelVolume &a = *src.objects.front()->volumes.front();
    const ModelVolume &b = *dst.objects.front()->volumes.front();
    CHECK(! b.supported_facets.has_auto_states());
    for (int state = 3; state < int(TRIANGLE_STATE_TYPE_COUNT); ++ state)
        CHECK(! b.supported_facets.get_data().used_states[state]);
    // Enforcers = manual enforcers + smart paint, blockers unchanged.
    REQUIRE(state_area(a, enforcer_like_states()) > state_area(a, TriangleStateType::ENFORCER));
    CHECK(state_area(b, TriangleStateType::ENFORCER) == Approx(state_area(a, enforcer_like_states())).margin(1e-6));
    CHECK(state_area(b, TriangleStateType::BLOCKER) == Approx(state_area(a, TriangleStateType::BLOCKER)).margin(1e-6));
    // Volumes without smart paint are not affected.
    CHECK(FacetsAnnotation::same_paint(src.objects.front()->volumes[1]->supported_facets.get_data(),
                                       dst.objects.front()->volumes[1]->supported_facets.get_data()));
    CHECK(dst.objects.front()->volumes[2]->supported_facets.empty());
}

TEST_CASE("3MF without smart paint is written and read as before", "[SmartSupports][sas-3mf]")
{
    Model src;
    make_model(src, false);
    const std::string path = temp_file("legacy.3mf");
    store(path, src);

    ZipEntries entries = read_zip(path);
    const std::string &xml = model_file(entries);
    CHECK(count_substr(xml, EXT_ATTR) == 0);
    CHECK(xml.find("FdmSupportsPaintingVersion\">1<") != std::string::npos);

    // A stock file: the same file read back gives the same paint.
    Model dst;
    load(path, dst);
    boost::filesystem::remove(path);
    for (size_t i = 0; i < 3; ++ i) {
        INFO("volume " << i);
        CHECK(FacetsAnnotation::same_paint(src.objects.front()->volumes[i]->supported_facets.get_data(),
                                           dst.objects.front()->volumes[i]->supported_facets.get_data()));
        CHECK(! dst.objects.front()->volumes[i]->supported_facets.has_auto_states());
    }
}
