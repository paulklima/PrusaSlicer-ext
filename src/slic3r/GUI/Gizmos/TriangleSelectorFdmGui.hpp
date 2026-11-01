#ifndef slic3r_TriangleSelectorFdmGui_hpp_
#define slic3r_TriangleSelectorFdmGui_hpp_

// Smart support painting: TriangleSelectorGUI subclass of the FDM supports gizmo that renders the smart ("auto")
// support states 3..9 with cause colours (plan §6.5; WP3). Manual enforcers and blockers keep the stock colours.
// Every state gets a valid seed-fill bucket (the stock renderer indexes a 3-element array by the state, R1).

#include <array>
#include <cstdint>

#include "GLGizmoPainterBase.hpp"

namespace Slic3r::GUI {

class TriangleSelectorFdmGui : public TriangleSelectorGUI {
public:
    // Render class of a leaf state. Unknown states (10..255, never written by this fork) render like blockers,
    // as the stock renderer does.
    enum class PaintClass : uint8_t { None, Enforcer, Blocker, AutoIsland, AutoOverhang, AutoLongSpan, Count };

    TriangleSelectorFdmGui() = delete;
    explicit TriangleSelectorFdmGui(const TriangleMesh &mesh) : TriangleSelectorGUI(mesh) {}
    ~TriangleSelectorFdmGui() override = default;

    void render(ImGuiWrapper *imgui, const Transform3d &matrix) override;

    // Auto states: island bit -> AutoIsland, else overhang (cantilever) bit -> AutoOverhang, else AutoLongSpan.
    static PaintClass       paint_class(TriangleStateType state);
    static const ColorRGBA &paint_color(PaintClass cls);

private:
    void update_render_data();

    static constexpr size_t ClassCount = size_t(PaintClass::Count);

    // Painted leaves per class (index None stays empty) and seed-filled leaves per class (including None).
    std::array<GLModel, ClassCount> m_iva_painted;
    std::array<GLModel, ClassCount> m_iva_seed_fill;
};

} // namespace Slic3r::GUI

#endif // slic3r_TriangleSelectorFdmGui_hpp_
