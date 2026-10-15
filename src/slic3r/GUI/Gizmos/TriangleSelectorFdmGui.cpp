// Smart support painting: FDM paint renderer with cause colours (plan §6.5; WP3).

#include "TriangleSelectorFdmGui.hpp"

#include "libslic3r/Support/SmartSupportStates.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r::GUI {

TriangleSelectorFdmGui::PaintClass TriangleSelectorFdmGui::paint_class(TriangleStateType state)
{
    using namespace SmartSupports;
    if (state == TriangleStateType::NONE)
        return PaintClass::None;
    if (state == TriangleStateType::ENFORCER)
        return PaintClass::Enforcer;
    if (is_auto_state(state)) {
        const uint8_t mask = auto_mask(state);
        return (mask & cbIsland)     ? PaintClass::AutoIsland :
               (mask & cbCantilever) ? PaintClass::AutoOverhang :
                                       PaintClass::AutoLongSpan;
    }
    return PaintClass::Blocker;
}

const ColorRGBA &TriangleSelectorFdmGui::paint_color(PaintClass cls)
{
    // Plan §6.5. The auto colours differ from each other and from the manual ones mainly in luminance,
    // so they stay distinguishable with colour vision deficiencies.
    static const std::array<ColorRGBA, ClassCount> colors {
        GLVolume::NEUTRAL_COLOR,               // None (seed fill only)
        ColorRGBA{ 0.47f, 0.47f, 1.00f, 1.f }, // manual enforcer
        ColorRGBA{ 1.00f, 0.44f, 0.44f, 1.f }, // manual blocker
        ColorRGBA{ 0.95f, 0.65f, 0.10f, 1.f }, // auto, island bit (amber)
        ColorRGBA{ 0.10f, 0.80f, 0.85f, 1.f }, // auto, overhang (cyan)
        ColorRGBA{ 0.60f, 0.90f, 0.30f, 1.f }, // auto, long span only (unused in v1)
    };
    return colors[std::min(size_t(cls), ClassCount - 1)];
}

void TriangleSelectorFdmGui::render(ImGuiWrapper *imgui, const Transform3d &matrix)
{
    if (m_update_render_data) {
        update_render_data();
        m_update_render_data = false;
    }

    auto *shader = wxGetApp().get_current_shader();
    if (! shader)
        return;
    assert(shader->get_name() == "gouraud");

    for (size_t i = 0; i < ClassCount; ++ i) {
        const ColorRGBA &color = paint_color(PaintClass(i));
        if (i != size_t(PaintClass::None)) {
            m_iva_painted[i].set_color(color);
            m_iva_painted[i].render();
        }
        m_iva_seed_fill[i].set_color(TriangleSelectorGUI::get_seed_fill_color(color));
        m_iva_seed_fill[i].render();
    }

    render_paint_contour(matrix);

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    if (imgui)
        render_debug(imgui);
    else
        assert(false); // If you want debug output, pass ptr to ImGuiWrapper.
#else
    (void)imgui;
#endif // PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
}

void TriangleSelectorFdmGui::update_render_data()
{
    std::array<GLModel::Geometry, ClassCount> painted_data;
    std::array<GLModel::Geometry, ClassCount> seed_fill_data;
    std::array<unsigned int, ClassCount>      painted_cnt {};
    std::array<unsigned int, ClassCount>      seed_fill_cnt {};
    for (size_t i = 0; i < ClassCount; ++ i) {
        m_iva_painted[i].reset();
        m_iva_seed_fill[i].reset();
        painted_data[i].format   = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
        seed_fill_data[i].format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    }

    // Small value used to offset triangles along their normal to avoid z-fighting (as the stock renderer).
    static constexpr float offset = 0.001f;

    for (const Triangle &tr : m_triangles) {
        if (! tr.valid() || tr.is_split())
            continue;
        const bool seed_fill = tr.is_selected_by_seed_fill();
        if (tr.get_state() == TriangleStateType::NONE && ! seed_fill)
            continue;

        const size_t       cls = size_t(paint_class(tr.get_state()));
        GLModel::Geometry &iva = seed_fill ? seed_fill_data[cls] : painted_data[cls];
        unsigned int      &cnt = seed_fill ? seed_fill_cnt[cls] : painted_cnt[cls];

        const Vec3f &v0       = m_vertices[tr.verts_idxs[0]].v;
        const Vec3f &v1       = m_vertices[tr.verts_idxs[1]].v;
        const Vec3f &v2       = m_vertices[tr.verts_idxs[2]].v;
        const Vec3f  n        = (v1 - v0).cross(v2 - v1).normalized();
        const Vec3f  offset_n = offset * n;
        iva.add_vertex(v0 + offset_n, n);
        iva.add_vertex(v1 + offset_n, n);
        iva.add_vertex(v2 + offset_n, n);
        iva.add_triangle(cnt, cnt + 1, cnt + 2);
        cnt += 3;
    }

    for (size_t i = 0; i < ClassCount; ++ i) {
        if (! painted_data[i].is_empty())
            m_iva_painted[i].init_from(std::move(painted_data[i]));
        if (! seed_fill_data[i].is_empty())
            m_iva_seed_fill[i].init_from(std::move(seed_fill_data[i]));
    }

    update_paint_contour();
}

} // namespace Slic3r::GUI
