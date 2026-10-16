// Smart support painting: island trees, tolerance, tails and cluster bands on sliced prints (plan §3.5, §3.6,
// §7.1; WP2a).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double area_mm2(const ExPolygons &e) { return area(e) * SCALING_FACTOR * SCALING_FACTOR; }

// Analysis input and base model of the first PrintObject of a cached print.
struct Base {
    AnalysisInput in;
    BaseModel     base;
};
Base base_of(const std::string &key, const std::function<Parts()> &make_parts)
{
    const SasPrint &sp = cached_print(key, make_parts, sas_config());
    UserParams up;
    up.alpha_deg = 45;
    Base out;
    out.in   = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, up);
    out.base = build_base_model(out.in, [] {}, [](int) {});
    return out;
}

// Index of the layer whose slab contains z (object frame, the object stands on the bed).
size_t layer_at(const AnalysisInput &in, double z)
{
    for (size_t n = 0; n < in.layers.size(); ++ n)
        if (z < in.layers[n].slice_z + 0.5 * in.layers[n].height)
            return n;
    return in.layers.size() - 1;
}

// Tail needs of one tree at layer n (∅ if none).
ExPolygons tail_of(const LayerGeom &lg, int32_t tree)
{
    for (const auto &[t, area] : lg.tail_needs)
        if (t == tree)
            return area;
    return {};
}

// Cone with its apex down at (x, y, apex_z), flanks at flank_deg from horizontal, top at z_top.
indexed_triangle_set pendant_cone(double x, double y, double flank_deg, double apex_z, double z_top)
{
    const double R = (z_top - apex_z) / std::tan(flank_deg * M_PI / 180.);
    return translate(revolve({ { 0., apex_z }, { R, z_top }, { 0., z_top } }, 128), Vec3d(x, y, 0.));
}

// A 20 × 20 × 2 plate at z 10..12 on a 4 × 4 post at its corner, plus the given pendant parts.
Parts plate_with(Parts pendants)
{
    Parts out { box({ -10., -10., 10. }, { 10., 10., 12. }), box({ 6., 6., 0. }, { 10., 10., 10. }) };
    append(out, std::move(pendants));
    return out;
}

} // namespace

TEST_CASE("Island tree of a pendant cone", "[SmartSupports][sas-islands]")
{
    // inverted_cone(): 60° flanks, apex at z = 5, 10 high; the plate (z 14.5 … 17) rests on a post.
    const auto [in, base] = base_of("inverted_cone_60", [] { return inverted_cone(); });
    REQUIRE(base.trees.size() == 1);
    const IslandTree &tree = base.trees.front();
    CHECK(tree.id == 0);
    CHECK(in.layers[tree.first].slice_z > 5.);
    CHECK(in.layers[tree.first].slice_z < 5.5);
    REQUIRE(tree.merge != UINT32_MAX);
    CHECK(in.layers[tree.merge].slice_z == Approx(14.6).margin(0.25));
    // Volume of the cone below the plate: π/3 · (9.5 / tan 60°)² · 9.5.
    CHECK(tree.volume_mm3 == Approx(M_PI / 3. * sqr(9.5 / std::tan(M_PI / 3.)) * 9.5).epsilon(0.1));
    CHECK(! tree.tolerated);
    // Tail up to an inscribed width of W_stiff = 3: apex + 1.5·tan 60° = apex + 2.6 (± 0.3).
    CHECK(in.layers[tree.tail_last].slice_z - 5. == Approx(2.6).margin(0.3));
    CHECK(! tree.tail_without_downward);

    // Slices: the cone belongs to the tree up to the merge, the post never.
    const size_t n10 = layer_at(in, 10.);
    REQUIRE(in.layers[n10].lslices.size() == 2);
    CHECK(std::count(base.layers[n10].slice_tree.begin(), base.layers[n10].slice_tree.end(), 0) == 1);
    CHECK(std::count(base.layers[n10].slice_tree.begin(), base.layers[n10].slice_tree.end(), -1) == 1);
    for (size_t n = tree.merge; n < base.layers.size(); ++ n)
        CHECK(base.layers[n].slice_tree == std::vector<int32_t>(in.layers[n].lslices.size(), -1));
    CHECK(base.layers[n10].slice_bad == std::vector<uint8_t>(2, 0));
    // The apex slice has no overlap below (weak), the post slice of that layer is grounded.
    CHECK(std::count(base.layers[tree.first].slice_bad.begin(), base.layers[tree.first].slice_bad.end(), 1) == 1);

    // Tail needs: the first slice, then the downward ring of each layer up to tail_last.
    for (size_t n = 0; n < base.layers.size(); ++ n) {
        const ExPolygons tn = tail_of(base.layers[n], 0);
        CHECK(tn.empty() == (n < tree.first || n > tree.tail_last));
    }
    // At the root the tail is the whole apex slice (that layer also holds the post slice).
    double apex_area = 0.;
    for (size_t i = 0; i < in.layers[tree.first].lslices.size(); ++ i)
        if (base.layers[tree.first].slice_tree[i] == 0)
            apex_area += area_mm2({ in.layers[tree.first].lslices[i] });
    CHECK(apex_area > 0.);
    CHECK(area_mm2(tail_of(base.layers[tree.first], 0)) == Approx(apex_area).epsilon(1e-6));
    const size_t n61 = layer_at(in, 6.1);
    CHECK(area_mm2(tail_of(base.layers[n61], 0)) == Approx(M_PI * (sqr(1.1) - sqr(0.9)) / 3.).epsilon(0.15));
    CHECK(base.layers[tree.first].tolerated.empty());
    CHECK(base.last_layer_with_fronts >= tree.tail_last);
}

TEST_CASE("Tolerated island trees", "[SmartSupports][sas-islands]")
{
    // Pendant boxes under the plate: A 0.6 × 0.6 × 0.6 (tolerated), B 2 × 2 × 0.6 (volume), C 0.6 × 0.6 × 1.6 (height).
    const auto [in, base] = base_of("pendant_boxes", [] {
        return plate_with({ box({ -6.3, -0.3, 9.4 }, { -5.7, 0.3, 10.5 }), box({ -1., -1., 9.4 }, { 1., 1., 10.5 }),
                            box({ 2.7, -0.3, 8.4 }, { 3.3, 0.3, 10.5 }) });
    });
    REQUIRE(base.trees.size() == 3);
    int tolerated = 0;
    for (const IslandTree &tree : base.trees) {
        INFO("tree " << tree.id << " first z " << in.layers[tree.first].slice_z << " volume " << tree.volume_mm3);
        REQUIRE(tree.merge != UINT32_MAX);
        CHECK(in.layers[tree.merge].slice_z == Approx(10.1).margin(0.01));
        if (tree.tolerated) {
            ++ tolerated;
            CHECK(tree.volume_mm3 == Approx(0.216).epsilon(0.15));
            CHECK(in.layers[tree.first].slice_z == Approx(9.5).margin(0.01));
        } else if (in.layers[tree.first].slice_z < 9.) {
            CHECK(tree.volume_mm3 == Approx(0.576).epsilon(0.15));        // C: small, but 1.6 mm high
        } else {
            CHECK(tree.volume_mm3 == Approx(2.4).epsilon(0.1));           // B
        }
        // Vertical sides: no downward facets above the first layer.
        if (! tree.tolerated)
            CHECK(tree.tail_without_downward);
    }
    CHECK(tolerated == 1);
    // T_n: A's slice at its layers, nothing else.
    CHECK(area_mm2(base.layers[layer_at(in, 9.5)].tolerated) == Approx(0.36).epsilon(0.1));
    CHECK(area_mm2(base.layers[layer_at(in, 9.9)].tolerated) == Approx(0.36).epsilon(0.1));
    CHECK(base.layers[layer_at(in, 8.5)].tolerated.empty());
    CHECK(base.layers[layer_at(in, 10.1)].tolerated.empty());
    // No tail for the tolerated tree.
    for (const LayerGeom &lg : base.layers)
        for (const auto &[t, area] : lg.tail_needs)
            CHECK(! base.trees[size_t(t)].tolerated);
}

TEST_CASE("Tail of a hanging needle has no downward facets", "[SmartSupports][sas-islands]")
{
    // Needle Ø 1, z 5 … 10.5, hanging from the plate.
    const auto [in, base] = base_of("pendant_needle", [] {
        return plate_with({ translate(its_make_cylinder(0.5, 5.5, 2. * M_PI / 64.), Vec3d(0., 0., 5.)) });
    });
    REQUIRE(base.trees.size() == 1);
    const IslandTree &tree = base.trees.front();
    CHECK(in.layers[tree.first].slice_z == Approx(5.1).margin(0.01));
    REQUIRE(tree.merge != UINT32_MAX);
    CHECK(tree.tail_without_downward);
    CHECK(tree.tail_last + 1 == tree.merge);                          // thin up to the merge (5 mm < H_tail)
    for (size_t n = 0; n < base.layers.size(); ++ n)
        CHECK(tail_of(base.layers[n], 0).empty() == (n != tree.first));
}

TEST_CASE("Sibling tails in one island tree", "[SmartSupports][sas-islands]")
{
    // Cones A (apex z 5, at x 0) and B (apex z 6, at x 16) joined by a bar at z 10 … 11, both hanging from a plate
    // (z 14.5 … 17) on a post: one tree, two roots, a tail from each apex.
    const auto [in, base] = base_of("sibling_cones", [] {
        return Parts { pendant_cone(0., 0., 60., 5., 15.), pendant_cone(16., 0., 60., 6., 15.),
                       box({ 0., -1., 10. }, { 16., 1., 11. }), box({ -7., -7., 14.5 }, { 30., 7., 17. }),
                       box({ 24., -2., 0. }, { 28., 2., 14.5 }) };
    });
    REQUIRE(base.trees.size() == 1);
    const IslandTree &tree = base.trees.front();
    CHECK(! tree.tolerated);
    CHECK(! tree.tail_without_downward);
    CHECK(in.layers[tree.first].slice_z == Approx(5.1).margin(0.25));
    // B's tail ends 2.6 (± 0.3) above its apex.
    CHECK(in.layers[tree.tail_last].slice_z - 6. == Approx(2.6).margin(0.3));
    const ExPolygons both = tail_of(base.layers[layer_at(in, 7.1)], 0);
    REQUIRE(both.size() == 2);
    const ExPolygons only_b = tail_of(base.layers[layer_at(in, 8.1)], 0);
    REQUIRE(only_b.size() == 1);
    // The remaining piece is B's (the one with the larger x).
    const double x_b = std::max(both[0].contour.centroid().x(), both[1].contour.centroid().x());
    CHECK(unscale<double>(only_b.front().contour.centroid().x()) == Approx(unscale<double>(x_b)).margin(0.5));
    CHECK(tail_of(base.layers[layer_at(in, 9.1)], 0).empty());
}

TEST_CASE("Cluster bands contain all fronts ⊕ m", "[SmartSupports][sas-islands]")
{
    struct Case { std::string key; std::function<Parts()> parts; size_t arm_clusters; };
    const std::vector<Case> cases {
        { "t_shape", [] { return t_shape(); }, 2 },                              // arms 10.4 apart: two clusters
        { "t_shape_stem4", [] { return t_shape(15., 10., 4.); }, 1 },             // arms 4.4 apart: one cluster
        { "dome_20_25", [] { return dome(20., 25.); }, 0 },
        { "inverted_cone_60", [] { return inverted_cone(); }, 0 },
        { "figurine_proxy", [] { return figurine_proxy(); }, 0 },
    };
    const float tol = scaled<float>(0.01);
    for (const Case &c : cases) {
        SECTION(c.key) {
            const auto [in, base] = base_of(c.key, c.parts);
            size_t fronts = 0;
            for (size_t n = 1; n < base.layers.size(); ++ n) {
                const LayerGeom  &lg = base.layers[n];
                const ExPolygons &L  = in.layers[n].lslices, &Lp = in.layers[n - 1].lslices;
                const coord_t     m  = scaled<coord_t>(in.params.cluster_margin + in.params.rho_exact) + lg.w;
                INFO("layer " << n << " slice_z " << in.layers[n].slice_z);
                fronts += lg.fronts.size();
                // Canonical front order.
                for (size_t i = 1; i < lg.fronts.size(); ++ i) {
                    const BoundingBox &a = lg.fronts[i - 1].bbox, &b = lg.fronts[i].bbox;
                    CHECK((a.min.y() < b.min.y() || (a.min.y() == b.min.y() && a.min.x() <= b.min.x())));
                }
                // Every front in exactly one cluster, windows disjoint.
                std::vector<int> owner(lg.fronts.size(), 0);
                for (size_t c1 = 0; c1 < lg.clusters.size(); ++ c1) {
                    const Cluster &cl = lg.clusters[c1];
                    CHECK(std::is_sorted(cl.fronts.begin(), cl.fronts.end()));
                    for (uint32_t f : cl.fronts)
                        ++ owner[f];
                    for (size_t c2 = c1 + 1; c2 < lg.clusters.size(); ++ c2)
                        CHECK(! cl.win.overlap(lg.clusters[c2].win));
                    // Bands inside the window and inside L / L_{n−1}.
                    if (! cl.band.empty())
                        CHECK(cl.win.contains(get_extents(cl.band)));
                    if (! cl.band_prev.empty())
                        CHECK(cl.win.contains(get_extents(cl.band_prev)));
                    CHECK(area_mm2(diff_ex(cl.band, L)) < 1e-4);
                    CHECK(area_mm2(diff_ex(cl.band_prev, Lp)) < 1e-4);
                    for (uint32_t f : cl.fronts) {
                        const FrontGeom &front = lg.fronts[f];
                        CHECK(cl.win.contains(front.bbox.inflated(coordf_t(m + lg.w))));
                        const ExPolygons near      = intersection_ex(offset(front.C, float(m) - tol, ClipperLib::jtRound, scaled<double>(0.001)), L);
                        const ExPolygons near_prev = intersection_ex(offset(front.C, float(m + lg.w) - tol, ClipperLib::jtRound, scaled<double>(0.001)), Lp);
                        CHECK(area_mm2(diff_ex(near, cl.band)) < 1e-4);
                        CHECK(area_mm2(diff_ex(near_prev, cl.band_prev)) < 1e-4);
                    }
                }
                CHECK(owner == std::vector<int>(lg.fronts.size(), 1));
            }
            CHECK(fronts > 0);
            if (c.arm_clusters > 0) {
                const LayerGeom &arms = base.layers[layer_at(in, 18.1)];
                CHECK(arms.fronts.size() == 2);
                CHECK(arms.clusters.size() == c.arm_clusters);
                CHECK(base.trees.empty());
            }
        }
    }
}

// Host test 1 (foot model, wp-log-wp5.md): every slice of a tree without contact below is an island start of its own
// (folding is per start, theory §3.1); below the first tip layer such slices rest on the plate.
TEST_CASE("Island starts: one per slice without contact below, none below the first tip layer", "[SmartSupports][sas-islands]")
{
    // PO-frame point of a point of the toe's design frame (all parts share one volume matrix).
    auto po_point = [](const AnalysisInput &in, double x, double y) {
        const Vec3d p = in.volumes.front().trafo * Vec3d(x, y, 0.);
        return Point::new_scale(p.x(), p.y());
    };
    SECTION("tip at z 1: one tree, four starts with their own tails") {
        const auto [in, base] = base_of("toe_" + std::to_string(1.), [] { return toe(1.); });
        REQUIRE(base.trees.size() == 1);
        REQUIRE(base.starts.size() == 4);
        const double bottoms[4] = { 1., toe_knuckle_bottom[0], toe_knuckle_bottom[1], toe_knuckle_bottom[2] };
        const double xs[4]      = { 0., toe_knuckle_x[0], toe_knuckle_x[1], toe_knuckle_x[2] };
        for (size_t s = 0; s < 4; ++ s) {
            const IslandStart &start = base.starts[s];
            CHECK(start.id == int32_t(s));
            CHECK(start.tree == 0);
            CHECK(in.layers[start.layer].slice_z == Approx(bottoms[s] + 0.1).margin(0.01));
            REQUIRE(start.slice >= 0);
            CHECK(in.layers[start.layer].lslices[size_t(start.slice)].contains(po_point(in, xs[s], 0.)));
            CHECK(base.layers[start.layer].slice_start[size_t(start.slice)] == start.id);
            CHECK(base.layers[start.layer].slice_bad[size_t(start.slice)] == 1);
            // A root's tail starts with its whole slice, keyed by its start.
            bool found = false;
            for (const auto &[id, area] : base.layers[start.layer].tail_starts)
                found |= id == start.id && ! area.empty();
            CHECK(found);
        }
        // tail_starts holds exactly the pieces of tail_needs, split by the start of their branch.
        for (const LayerGeom &lg : base.layers) {
            double a_tree = 0., a_start = 0.;
            for (const auto &[t, area] : lg.tail_needs)
                a_tree += area_mm2(area);
            for (const auto &[s, area] : lg.tail_starts) {
                CHECK((s >= 0 && size_t(s) < base.starts.size()));
                a_start += area_mm2(area);
            }
            CHECK(a_start == Approx(a_tree).epsilon(1e-9));
        }
    }
    SECTION("tip 0.15 / 0.35 above the bed: it rests on the plate, the knuckles are their own trees") {
        for (const double hover : { 0.15, 0.35 }) {
            const auto [in, base] = base_of("toe_" + std::to_string(hover), [hover] { return toe(hover); });
            const uint32_t first_tip = first_tip_layer(in.params);
            REQUIRE(first_tip == 3);                              // z_top 1, no raft: tips from object layer 3 on
            // Tip, column and rod are grounded through the tip resting on the plate; each knuckle merges into the rod.
            CHECK(base.trees.size() == 3);
            CHECK(base.starts.size() == 3);
            for (const IslandTree &tree : base.trees)
                CHECK(tree.first >= first_tip);
            for (const IslandStart &start : base.starts)
                CHECK(start.layer >= first_tip);
            // The tip's slices below the first tip layer: grounded, no weak contact, no fronts.
            const Point tip = po_point(in, 0., 0.);
            int tip_layers = 0;
            for (uint32_t n = 1; n < first_tip; ++ n)
                for (size_t i = 0; i < in.layers[n].lslices.size(); ++ i)
                    if (in.layers[n].lslices[i].contains(tip)) {
                        ++ tip_layers;
                        CHECK(base.layers[n].slice_tree[i] == -1);
                        CHECK(base.layers[n].slice_bad[i] == 0);
                        for (const FrontGeom &f : base.layers[n].fronts)
                            CHECK(intersection(ExPolygons{ f.C }, ExPolygons{ in.layers[n].lslices[i] }).empty());
                    }
            CHECK(tip_layers == (hover < 0.2 ? 2 : 1));
        }
    }
}
