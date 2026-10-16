// Smart support painting: GUI panel of the FDM supports gizmo (plan §6; WP3).

#include "SmartSupportsPanel.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <thread>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <GL/glew.h>
#include <LocalesUtils.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiPureWrap.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoPainterBase.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Gizmos/SmartSupportsWait.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

namespace Slic3r::GUI {

namespace {

namespace SS = Slic3r::SmartSupports;
using Clock = std::chrono::steady_clock;

constexpr const char *Section        = "smart_auto_supports";
constexpr auto        PollPeriod     = std::chrono::milliseconds(500);
constexpr int         PollFrameMs    = 100;
constexpr int         MaxVisibleRows = 10;

using SS::is_problem;                    // group of a report row, shared with build_report

bool is_canceled_exception(const std::exception_ptr &eptr)
{
    if (! eptr)
        return false;
    try {
        std::rethrow_exception(eptr);
    } catch (const CanceledException &) {
        return true;
    } catch (...) {
    }
    return false;
}

std::string exception_message(const std::exception_ptr &eptr)
{
    try {
        std::rethrow_exception(eptr);
    } catch (const std::exception &ex) {
        return ex.what();
    } catch (...) {
    }
    return _u8L("Unknown error");
}

bool same_user_params(const SS::UserParams &a, const SS::UserParams &b)
{
    return a.paint_causes == b.paint_causes && a.alpha_deg == b.alpha_deg && a.K_max == b.K_max && a.B_max == b.B_max &&
           a.B_ceil == b.B_ceil && a.H_s == b.H_s && a.beta_equals_alpha == b.beta_equals_alpha &&
           (a.beta_equals_alpha || a.beta_deg == b.beta_deg) && a.plate_policy == b.plate_policy &&
           a.lowest_level == b.lowest_level;
}

// Plan §6.7 rule 1: the selected instance if printable, else the first printable one; -1 if none.
int predicted_instance(const ModelObject &mo, int selected)
{
    if (selected >= 0 && selected < int(mo.instances.size()) && mo.instances[selected]->is_printable())
        return selected;
    for (size_t i = 0; i < mo.instances.size(); ++ i)
        if (mo.instances[i]->is_printable())
            return int(i);
    return -1;
}

std::vector<std::pair<ObjectID, uint64_t>> paint_timestamps(const ModelObject &mo)
{
    std::vector<std::pair<ObjectID, uint64_t>> out;
    for (const ModelVolume *mv : mo.volumes)
        if (mv->is_model_part())
            out.emplace_back(mv->id(), mv->supported_facets.timestamp());
    return out;
}

std::string fmt_number(double value, int precision)
{
    return float_to_string_decimal_point(value, precision);
}

void tooltip(const std::string &text)
{
    if (! text.empty())
        ImGuiPureWrap::tooltip(text, ImGui::GetFontSize() * 20.0f);
}

void text_wrapped_colored(const ImVec4 &color, const std::string &text, float width)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGuiPureWrap::text_wrapped(text, width);
    ImGui::PopStyleColor();
}

const ImVec4 ColGrey  { 0.62f, 0.62f, 0.62f, 1.f };
const ImVec4 ColError { 1.00f, 0.35f, 0.30f, 1.f };

ImU32 level_color(SS::Level level)
{
    switch (level) {
    case SS::Level::Must:   return ImGui::GetColorU32(ImVec4(0.92f, 0.26f, 0.20f, 1.f));
    case SS::Level::Should: return ImGui::GetColorU32(ImVec4(0.96f, 0.66f, 0.12f, 1.f));
    default:                return ImGui::GetColorU32(ImVec4(0.70f, 0.70f, 0.70f, 1.f));
    }
}

// ---- texts (plan §6.1, §6.2, §6.4)

std::string issue_text(SS::ContractIssue::Key key)
{
    switch (key) {
    case SS::ContractIssue::SupportOff:           return _u8L("Supports are off");
    case SS::ContractIssue::AutoOn:               return _u8L("Auto-generated supports are on");
    case SS::ContractIssue::NotOrganic:           return _u8L("Support style is not Organic");
    case SS::ContractIssue::EnforceLayers:        return _u8L("Enforced support layers are set");
    case SS::ContractIssue::ContactZero:          return _u8L("Top contact Z distance is 0 (no bridges)");
    case SS::ContractIssue::BuildplateMismatch:   return _u8L("'Support on build plate only' does not match Placement");
    case SS::ContractIssue::InterfaceLayers:      return _u8L("Top interface layers are set");
    case SS::ContractIssue::Raft:                 return _u8L("A raft counts as manual support");
    case SS::ContractIssue::ReachBelowTipSpacing: return _u8L("Max overhang reach is below half the support tip spacing; painted areas may get too few tips");
    }
    return {};
}

std::string cause_name(SS::Cause cause)
{
    switch (cause) {
    case SS::Cause::Island:     return _u8L("island");
    case SS::Cause::Cantilever: return _u8L("overhang");
    default:                    return _u8L("long bridge");
    }
}

std::string level_name(SS::Level level)
{
    switch (level) {
    case SS::Level::Must:   return _u8L("critical");
    case SS::Level::Should: return _u8L("likely");
    default:                return _u8L("possible");
    }
}

std::string state_text(const SS::ReportEntry &e, size_t analysed_instance)
{
    using RS = SS::ReportState;
    switch (e.state) {
    case RS::Unplaceable:               return _u8L("Cannot be supported from the plate");
    case RS::BlockedByUser:             return _u8L("Kept free by your blocker");
    case RS::WillBeDropped:             return _u8L("Your paint here will be dropped by the slicer");
    case RS::RemainingAfterPaint:       return _u8L("Paint does not fully cover this area");
    case RS::OnModelFallback:           return _u8L("Support on the model (no plate position)");
    case RS::OnModelTooShort:           return _u8L("Support on the model may be removed (branch < 1 mm)");
    case RS::MayMergeOntoModel:         return _u8L("Plate support may merge onto the model");
    case RS::PartlyOnModel:             return _u8L("Some tips of this area stand on the model");
    case RS::BelowLowestLevel:          return _u8L("Likely droop, not painted (Critical only)");
    case RS::LongSpanNotPainted:        return _u8L("Long bridge, not painted in this version");
    case RS::CauseDisabled:             return e.cause == SS::Cause::Island ? _u8L("Islands not painted (unchecked)") :
                                                                              _u8L("Overhangs not painted (unchecked)");
    case RS::ReliesOnDisabledCauses:    return _u8L("Painted areas rely on unpainted planned support (unchecked causes, long bridges)");
    case RS::TailWithoutDownwardFacets: return _u8L("Thin hanging part without downward faces");
    case RS::SeveralPrintObjects:       return format(_u8L("Computed for instance %1% only"), analysed_instance + 1);
    case RS::OtherObjectsNotConsidered: return _u8L("Other objects on the plate are not considered");
    }
    return {};
}

std::string advice_text(const SS::ReportEntry &e)
{
    using RS = SS::ReportState;
    switch (e.why) {
    case SS::Why::InterfaceGap:
        return _u8L("Top interface layers keep support 0.5 mm away from this area. Fix… lists what changes if you set them to 0.");
    case SS::Why::NoTipLayer:
        return _u8L("Too close to the bed for tree support.");
    case SS::Why::NotReachable:
        return _u8L("No support branch can reach this area.");
    case SS::Why::NoProgress:
        return _u8L("Painting here does not support this area any better.");
    default: break;
    }
    switch (e.state) {
    case RS::BlockedByUser:      return _u8L("Shrink or remove your blocker here if this area should be supported.");
    case RS::WillBeDropped:      return _u8L("Support from this paint cannot reach the build plate, so the slicer removes it.");
    case RS::OnModelFallback:    return _u8L("No position on the build plate supports this area; support stands on the model.");
    case RS::BelowLowestLevel:   return _u8L("Choose \"Critical and likely\" to paint these areas too.");
    case RS::CauseDisabled:      return _u8L("Check the cause under Paint to paint these areas.");
    default:                     return {};
    }
}

// "<option label>: <value>" of one recommended change (option key, serialized value).
std::string change_text(const std::string &key, const std::string &value)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr)
        return key + ": " + value;
    const std::string label = I18N::translate_utf8(def->full_label.empty() ? def->label : def->full_label);
    std::string val = value;
    if (def->type == coBool)
        val = value == "1" ? _u8L("on") : _u8L("off");
    else if (def->enum_def && def->enum_def->has_values()) {
        if (std::optional<int> idx = def->enum_def->value_to_index(value); idx && def->enum_def->has_labels())
            val = I18N::translate_utf8(def->enum_def->label(*idx));
    } else if (! def->sidetext.empty())
        val += " " + I18N::translate_utf8(def->sidetext);
    return label + ": " + val;
}

// ---- fake result (D28, AppConfig smart_auto_supports/debug_fake_result = 1)

// Reads the leaf state of original facets (protected in TriangleSelector).
class FakeSelector : public TriangleSelector {
public:
    explicit FakeSelector(const TriangleMesh &mesh) : TriangleSelector(mesh) {}
    bool unpainted_leaf(int facet_idx) const
    {
        const Triangle &tr = m_triangles[facet_idx];
        return tr.valid() && ! tr.is_split() && tr.get_state() == TriangleStateType::NONE;
    }
};

// Paints auto(island) on facets with world normal z < -0.95 and auto(overhang) on z < -0.7 (only the checked
// causes, manual paint kept, earlier smart paint replaced), and emits one report row of each ReportState with a
// marker on a painted facet where possible. It sleeps ~3 s in steps so progress and Cancel can be tested.
SS::Result fake_run(const SS::AnalysisInput &in, const SS::IsCanceled &is_canceled, const SS::ProgressFn &progress)
{
    using namespace SS;
    auto cancel = [&is_canceled]() { if (is_canceled()) throw CanceledException(); };

    Result out;
    const uint8_t causes = in.params.user.paint_causes;
    std::vector<Vec3d> markers;   // PO frame, mm: centres of painted facets
    out.paint.reserve(in.volumes.size());
    for (size_t vi = 0; vi < in.volumes.size(); ++ vi) {
        cancel();
        const VolumeInput &vol = in.volumes[vi];
        VolumePaint vp;
        vp.volume_id = vol.volume_id;
        vp.data      = vol.paint;
        if (vol.mesh) {
            FakeSelector sel(*vol.mesh);
            sel.deserialize(vol.paint, false);
            sel.remap_states(clear_causes_map(cbAll));
            const indexed_triangle_set &its = vol.mesh->its;
            const Matrix3d normal_matrix = vol.trafo.linear().inverse().transpose();
            bool painted[2] = { false, false };
            for (int f = 0; f < int(its.indices.size()); ++ f) {
                if ((f & 0x3fff) == 0)
                    cancel();
                const stl_triangle_vertex_indices &t = its.indices[f];
                const Vec3d a = its.vertices[t[0]].cast<double>();
                const Vec3d b = its.vertices[t[1]].cast<double>();
                const Vec3d c = its.vertices[t[2]].cast<double>();
                Vec3d n = normal_matrix * (b - a).cross(c - a);
                const double len = n.norm();
                if (len <= 0.)
                    continue;
                n /= len;
                const uint8_t bit = n.z() < -0.95 ? cbIsland : n.z() < -0.7 ? cbCantilever : 0;
                if (bit == 0 || (causes & bit) == 0 || ! sel.unpainted_leaf(f))
                    continue;
                sel.set_facet(f, auto_state(bit));
                painted[bit == cbIsland ? 0 : 1] = true;
                if (markers.size() < 64)
                    markers.emplace_back(vol.trafo * ((a + b + c) / 3.));
            }
            vp.data = sel.serialize();
            if (painted[0])
                ++ out.painted_areas[int(Cause::Island)];
            if (painted[1])
                ++ out.painted_areas[int(Cause::Cantilever)];
        }
        vp.changed = ! FacetsAnnotation::same_paint(vp.data, vol.paint);
        out.paint.emplace_back(std::move(vp));
        progress(int(10 + 40 * (vi + 1) / std::max<size_t>(1, in.volumes.size())));
    }

    // Simulated work.
    for (int i = 0; i < 30; ++ i) {
        cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        progress(50 + i * 50 / 30);
    }

    static constexpr std::array<ReportState, 15> states {
        ReportState::Unplaceable, ReportState::BlockedByUser, ReportState::WillBeDropped, ReportState::RemainingAfterPaint,
        ReportState::OnModelFallback, ReportState::OnModelTooShort, ReportState::MayMergeOntoModel, ReportState::PartlyOnModel,
        ReportState::BelowLowestLevel, ReportState::LongSpanNotPainted, ReportState::CauseDisabled,
        ReportState::ReliesOnDisabledCauses, ReportState::TailWithoutDownwardFacets, ReportState::SeveralPrintObjects,
        ReportState::OtherObjectsNotConsidered };
    const size_t num_layers = in.layers.size();
    for (size_t k = 0; k < states.size(); ++ k) {
        ReportEntry e;
        e.state = states[k];
        e.cause = (e.state == ReportState::BlockedByUser || e.state == ReportState::CauseDisabled || k % 3 == 0) ?
                      Cause::Island : Cause::Cantilever;
        e.level = k % 3 == 0 ? Level::Must : Level::Should;
        e.why   = e.state == ReportState::Unplaceable         ? Why::InterfaceGap :
                  e.state == ReportState::WillBeDropped       ? Why::NotReachable :
                  e.state == ReportState::RemainingAfterPaint ? Why::NoProgress :
                  e.state == ReportState::OnModelTooShort     ? Why::NoTipLayer : Why::None;
        e.count = e.state == ReportState::Unplaceable ? 4 : int(1 + k % 3);
        if (e.state == ReportState::SeveralPrintObjects)
            e.count = std::max(2, in.num_print_objects);
        const bool positioned = e.state != ReportState::SeveralPrintObjects && e.state != ReportState::OtherObjectsNotConsidered;
        if (positioned && num_layers > 0) {
            size_t layer = std::min(num_layers - 1, (k + 1) * num_layers / (states.size() + 1));
            Vec3d  p     = Vec3d::Zero();
            if (! markers.empty()) {
                p = markers[k % markers.size()];
                // the layer whose slab contains p
                layer = num_layers - 1;
                for (size_t l = 0; l < num_layers; ++ l)
                    if (p.z() < in.layers[l].slice_z + 0.5 * in.layers[l].height) {
                        layer = l;
                        break;
                    }
            } else {
                const LayerInput &li = in.layers[layer];
                if (! li.lslices.empty()) {
                    const BoundingBox bb = get_extents(li.lslices.front());
                    p = Vec3d(unscale<double>(bb.center().x()), unscale<double>(bb.center().y()), li.slice_z);
                }
            }
            e.layer_first = uint32_t(layer);
            e.layer_last  = uint32_t(std::min(num_layers - 1, layer + k % 3));
            e.z_first     = in.layers[e.layer_first].print_z;
            e.z_last      = in.layers[e.layer_last].print_z;
            e.centroid    = Point(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
            e.slice_z     = markers.empty() ? in.layers[layer].slice_z : p.z();
            e.area_mm2    = 2. + 3. * double(k);
        }
        out.report.emplace_back(e);
    }
    std::stable_partition(out.report.begin(), out.report.end(), [](const ReportEntry &e) { return is_problem(e); });
    out.report_more[0] = 2;
    progress(100);
    return out;
}

// ---- dialog (plan §6.4, review 6 m1): identical on all platforms

class SmartSupportsDialog : public MsgDialog
{
public:
    enum class Kind { Fix, Gate };

    SmartSupportsDialog(wxWindow *parent, const wxString &headline, const wxString &body, Kind kind, bool can_apply)
        : MsgDialog(parent, _L("Paint where needed"), headline, wxICON_WARNING)
    {
        auto *text = new wxStaticText(this, wxID_ANY, body);
        text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
        content_sizer->Add(text, 0, wxEXPAND);
        if (kind == Kind::Gate) {
            if (can_apply)
                add_button(wxID_YES, true, _L("Apply and paint"));
            add_button(wxID_NO, ! can_apply, _L("Paint anyway"));
        } else if (can_apply) {
            add_button(wxID_APPLY, true, _L("Apply"));
            add_button(wxID_YES, false, _L("Apply and paint"));
        }
        add_button(wxID_CANCEL, kind == Kind::Fix && ! can_apply, _L("Cancel"));
        finalize();
    }
};

} // namespace

// ============================================================================ construction, settings

SmartSupportsPanel::SmartSupportsPanel(SmartSupportsHost &host) : m_host(host) {}

SmartSupportsPanel::~SmartSupportsPanel()
{
    // No Plater access here (the panel may die at shutdown): a running job sees the flag and stops.
    if (m_cancel_flag)
        m_cancel_flag->store(true);
}

bool SmartSupportsPanel::fake_mode() const
{
    return wxGetApp().app_config->get_bool(Section, "debug_fake_result");
}

void SmartSupportsPanel::load_settings()
{
    m_loaded = true;
    const AppConfig &cfg = *wxGetApp().app_config;
    auto get_bool = [&cfg](const char *key, bool def) { return cfg.has(Section, key) ? cfg.get(Section, key) == "1" : def; };
    auto get_double = [&cfg](const char *key, double def, double lo, double hi) {
        const std::string s = cfg.get(Section, key);
        if (s.empty())
            return def;
        double v = def;
        try {
            v = string_to_double_decimal_point(s);
        } catch (...) {
            return def;
        }
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };

    const SS::UserParams def;
    m_section_open = get_bool("section_open", false);
    m_up.paint_causes = uint8_t((get_bool("cause_islands", true) ? SS::cbIsland : 0) |
                                (get_bool("cause_overhangs", true) ? SS::cbCantilever : 0));
    // First value of α: the effective support_material_threshold (D5), derived and persisted when the section is
    // opened. Users who never open it pay neither the config lookup nor an AppConfig write (review C m7).
    m_alpha_pending = ! cfg.has(Section, "overhang_angle");
    if (! m_alpha_pending)
        m_up.alpha_deg = int(std::lround(get_double("overhang_angle", 0., 0., 89.)));
    m_up.K_max             = get_double("k_max",  def.K_max,  0.5, 20.);
    m_up.B_max             = get_double("b_max",  def.B_max,  5., 200.);
    m_up.B_ceil            = get_double("b_ceil", def.B_ceil, 0., 200.);
    m_up.H_s               = get_double("h_s",    def.H_s,    0., 5.);
    m_up.beta_equals_alpha = get_bool("beta_equals_alpha", def.beta_equals_alpha);
    m_up.beta_deg          = get_double("beta", def.beta_deg, 1., 89.);
    m_up.plate_policy      = cfg.get(Section, "plate_policy") == "plate_only" ? SS::PlatePolicy::PlateOnly : SS::PlatePolicy::PreferPlate;
    m_up.lowest_level      = cfg.get(Section, "level") == "critical" ? SS::Level::Must : SS::Level::Should;
}

// First opening of the section: α starts from the effective support_material_threshold (D5), persisted from now on.
void SmartSupportsPanel::ensure_alpha()
{
    if (! m_alpha_pending)
        return;
    update_effective_config(true);
    if (! m_eff.valid)
        return;                                    // no object yet: next time
    m_up.alpha_deg  = std::clamp(m_eff.object.support_material_threshold.value, 0, 89);
    m_alpha_pending = false;
    save_settings();
}

void SmartSupportsPanel::save_settings() const
{
    if (m_alpha_pending)
        return;   // nothing chosen yet; the section has never been opened
    AppConfig &cfg = *wxGetApp().app_config;
    cfg.set(Section, "section_open",      m_section_open ? "1" : "0");
    cfg.set(Section, "cause_islands",     (m_up.paint_causes & SS::cbIsland) ? "1" : "0");
    cfg.set(Section, "cause_overhangs",   (m_up.paint_causes & SS::cbCantilever) ? "1" : "0");
    cfg.set(Section, "overhang_angle",    std::to_string(m_up.alpha_deg));
    cfg.set(Section, "k_max",             fmt_number(m_up.K_max, 2));
    cfg.set(Section, "b_max",             fmt_number(m_up.B_max, 1));
    cfg.set(Section, "b_ceil",            fmt_number(m_up.B_ceil, 1));
    cfg.set(Section, "h_s",               fmt_number(m_up.H_s, 2));
    cfg.set(Section, "beta_equals_alpha", m_up.beta_equals_alpha ? "1" : "0");
    cfg.set(Section, "beta",              fmt_number(m_up.beta_deg, 1));
    cfg.set(Section, "plate_policy",      m_up.plate_policy == SS::PlatePolicy::PlateOnly ? "plate_only" : "prefer_plate");
    cfg.set(Section, "level",             m_up.lowest_level == SS::Level::Must ? "critical" : "critical_likely");
}

void SmartSupportsPanel::reset_to_defaults()
{
    m_up = SS::UserParams{};
    update_effective_config(true);
    m_up.alpha_deg  = m_eff.valid ? std::clamp(m_eff.object.support_material_threshold.value, 0, 89) : 0;
    m_alpha_pending = false;
    save_settings();
}

// ============================================================================ polling

void SmartSupportsPanel::update_effective_config(bool force)
{
    const ModelObject *mo = m_host.model_object();
    if (mo == nullptr) {
        m_eff.valid = false;
        return;
    }
    const Clock::time_point now = Clock::now();
    if (! force && m_eff.valid && m_eff.mo_id == mo->id() && m_eff.mo_config_timestamp == static_cast<const ModelConfig &>(mo->config).timestamp() &&
        m_eff.policy == m_up.plate_policy && now - m_eff.when < PollPeriod)
        return;
    m_eff.full = wxGetApp().preset_bundle->full_config();
    m_eff.full.apply(mo->config.get(), true);
    m_eff.object.apply(m_eff.full, true);
    m_eff.issues              = SS::check_contract(m_eff.object, m_up.plate_policy, nullptr);
    m_eff.mo_id               = mo->id();
    m_eff.mo_config_timestamp = static_cast<const ModelConfig &>(mo->config).timestamp();
    m_eff.policy              = m_up.plate_policy;
    m_eff.when                = now;
    m_eff.valid               = true;
}

std::vector<SS::ContractIssue> SmartSupportsPanel::blocking_issues() const
{
    std::vector<SS::ContractIssue> out;
    if (m_eff.valid)
        for (const SS::ContractIssue &ci : m_eff.issues)
            if (! ci.advisory)
                out.emplace_back(ci);
    return out;
}

bool SmartSupportsPanel::has_smart_paint(const ModelObject &mo)
{
    std::vector<std::pair<ObjectID, uint64_t>> key = paint_timestamps(mo);
    if (key != m_paint_key) {
        m_paint_key_smart = std::any_of(mo.volumes.begin(), mo.volumes.end(), [](const ModelVolume *mv) {
            return mv->is_model_part() && mv->supported_facets.has_auto_states();
        });
        m_paint_key = std::move(key);
    }
    return m_paint_key_smart;
}

const SmartSupportsPanel::LastRun *SmartSupportsPanel::current_last_run() const
{
    const ModelObject *mo = m_host.model_object();
    if (mo == nullptr)
        return nullptr;
    auto it = m_last_runs.find(mo->id().id);
    return it == m_last_runs.end() ? nullptr : &it->second;
}

void SmartSupportsPanel::poll(bool force)
{
    const Clock::time_point now = Clock::now();
    ModelObject *mo = m_host.model_object();
    if (! force && m_poll.valid && mo != nullptr && m_poll.mo_id == mo->id() && now - m_poll.when < PollPeriod)
        return;
    m_poll       = PollInfo{};
    m_poll.valid = true;
    m_poll.when  = now;
    if (mo == nullptr)
        return;
    m_poll.mo_id = mo->id();
    update_effective_config(false);   // rebuilt on object config / policy changes and every PollPeriod
    m_poll.has_smart_paint = has_smart_paint(*mo);

    if (const LastRun *lr = current_last_run()) {
        m_poll.stale_paint  = paint_timestamps(*mo) != lr->paint_timestamps;
        const uint64_t fp   = m_eff.valid ? SS::config_fingerprint(m_eff.full, *mo, lr->analysed_instance, m_up) : lr->config_fp;
        m_poll.stale_config = fp != lr->config_fp || ! same_user_params(m_up, lr->user);
    }

    const int inst = predicted_instance(*mo, m_host.selected_instance());
    m_poll.printable = inst >= 0;
    if (inst >= 0) {
        // Predict the analysed instance before slicing (D15): rotated instances get their own PrintObjects.
        const Print &print = wxGetApp().plater()->active_fff_print();
        const bool   bg    = wxGetApp().app_config->get_bool("background_processing");
        const SS::PrintObjectChoice choice = SS::choose_print_object(print, *mo, size_t(std::max(0, m_host.selected_instance())), bg);
        const size_t num_po = std::count_if(print.objects().begin(), print.objects().end(),
                                            [mo](const PrintObject *po) { return po->model_object()->id() == mo->id(); });
        if (choice.po != nullptr && (num_po > 1 || choice.fallback))
            m_poll.instance_note = int(choice.analysed_instance);
    }
}

void SmartSupportsPanel::tick()
{
    poll(false);
    if (m_state == State::WaitingForSlices)
        poll_waiting();
    if (busy())
        m_host.request_redraw(PollFrameMs);
}

// ============================================================================ events

void SmartSupportsPanel::defer(std::function<void(SmartSupportsPanel &)> fn)
{
    std::weak_ptr<int> alive = m_alive;
    wxGetApp().CallAfter([this, alive, fn = std::move(fn)]() {
        // At shutdown the plater is gone before the deferred calls run (review C m1).
        if (alive.lock() && wxGetApp().plater() != nullptr)
            fn(*this);
    });
}

void SmartSupportsPanel::on_data_changed()
{
    const ModelObject *mo = m_host.model_object();
    const ObjectID     id = mo != nullptr ? mo->id() : ObjectID();
    if (busy() && (mo == nullptr || id != m_run_mo_id))
        cancel_run();
    if (id != m_shown_mo_id) {
        // Another object: its own last run (if any) is shown.
        m_shown_mo_id = id;
        m_selected_entry.reset();
        m_marker_key.reset();
        if (! busy()) {
            m_state = State::Idle;
            m_message.clear();
        }
    }
    m_poll.valid = false;
}

void SmartSupportsPanel::on_shutdown()
{
    if (busy())
        cancel_run();
    m_selected_entry.reset();
}

// ============================================================================ run state machine (plan §2.2, §6.6)

bool SmartSupportsPanel::can_start(std::string *why_not) const
{
    auto fail = [why_not](std::string msg) {
        if (why_not)
            *why_not = std::move(msg);
        return false;
    };
    const ModelObject *mo = m_host.model_object();
    if (mo == nullptr)
        return fail({});
    if (busy())
        return fail(_u8L("Painting where needed is running."));
    if (! wxGetApp().plater()->get_ui_job_worker().is_idle())
        return fail(_u8L("Another background task is running."));
    // Its backend result would overwrite the smart paint, our reslice would replace its task (review C m3).
    if (m_host.stock_autopaint_pending())
        return fail(_u8L("Automatic painting is still running."));
    if (m_poll.valid && ! m_poll.printable)
        return fail(_u8L("The object has no printable instance."));
    return true;
}

void SmartSupportsPanel::on_paint_clicked()
{
    const ModelObject *mo = m_host.model_object();
    if (mo == nullptr || ! can_start())
        return;
    update_effective_config(true);
    if (! blocking_issues().empty() && m_gate_answered.count(mo->id().id) == 0) {
        defer([](SmartSupportsPanel &p) { p.show_gate_dialog(); });
        return;
    }
    start_run();
}

void SmartSupportsPanel::set_failed(const std::string &message)
{
    m_state   = State::Failed;
    m_message = message;
    m_host.request_redraw();
}

void SmartSupportsPanel::start_run()
{
    ModelObject *mo = m_host.model_object();
    if (mo == nullptr || ! can_start())
        return;
    Plater *plater = wxGetApp().plater();

    const int inst = predicted_instance(*mo, m_host.selected_instance());
    if (inst < 0) {
        set_failed(_u8L("The object has no printable instance."));
        return;
    }
    // The bed of the analysed instance becomes the active one before slicing (plan §6.7).
    const auto &inst_map = s_multiple_beds.get_inst_map();
    if (auto it = inst_map.find(mo->instances[inst]->id()); it != inst_map.end() && s_multiple_beds.get_active_bed() != it->second)
        s_multiple_beds.set_active_bed(it->second);

    if (const std::string err = plater->active_fff_print().validate(); ! err.empty()) {
        set_failed(_u8L("Painting where needed requires a valid print setup.") + " " + err);
        return;
    }

    m_cancel_flag = std::make_shared<std::atomic<bool>>(false);
    m_job.reset();
    ++ m_run_id;
    m_run_mo_id         = mo->id();
    m_state             = State::WaitingForSlices;
    m_wait_start        = Clock::now();
    m_reslice_requested = false;
    m_idle_reslices     = 0;
    m_message.clear();
    m_selected_entry.reset();
    request_reslice();
    m_host.request_redraw(PollFrameMs);
}

// D33: reslice outside of rendering; Print::apply runs synchronously inside reslice_FFF_until_step().
void SmartSupportsPanel::request_reslice()
{
    m_reslice_requested = false;
    const uint64_t run_id = m_run_id;
    defer([run_id](SmartSupportsPanel &p) {
        if (p.m_run_id != run_id || p.m_state != State::WaitingForSlices)
            return;
        ModelObject *mo = p.m_host.model_object();
        if (mo == nullptr || mo->id() != p.m_run_mo_id) {
            p.cancel_run();
            return;
        }
        Plater *plater = wxGetApp().plater();
        plater->reslice_FFF_until_step(posSlice, *mo, false);
        // The reslice applied the current model and config: an invalid print would never be sliced.
        if (const std::string err = plater->active_fff_print().validate(); ! err.empty()) {
            p.cancel_run();
            p.set_failed(_u8L("Painting where needed requires a valid print setup.") + " " + err);
            return;
        }
        p.m_reslice_requested = true;
        p.m_last_reslice      = Clock::now();
        p.m_host.request_redraw(PollFrameMs);
    });
}

void SmartSupportsPanel::poll_waiting()
{
    ModelObject *mo = m_host.model_object();
    if (! m_host.is_open() || mo == nullptr || mo->id() != m_run_mo_id) {
        cancel_run();
        return;
    }
    namespace W = SmartSupportsWait;
    using Seconds = std::chrono::duration<double>;
    const Clock::time_point now    = Clock::now();
    Plater                 *plater = wxGetApp().plater();
    W::Observation          o;
    o.since_start_s    = Seconds(now - m_wait_start).count();
    o.resliced         = m_reslice_requested;
    o.since_reslice_s  = m_reslice_requested ? Seconds(now - m_last_reslice).count() : 0.;
    o.update_scheduled = plater->is_background_process_update_scheduled();
    o.idle_reslices    = m_idle_reslices;
    o.worker_idle      = plater->get_ui_job_worker().is_idle();
    const PrintStatus status = s_print_statuses[s_multiple_beds.get_active_bed()];
    o.slicing_running  = status == PrintStatus::running;
    o.slicing_failed   = ! is_sliceable(status);
    // Re-resolve the print and the PrintObject on every poll; never cache them (Print::apply may delete them).
    SS::PrintObjectChoice choice;
    if (o.resliced && ! o.update_scheduled) {
        const Print &print = plater->active_fff_print();
        const bool   bg    = wxGetApp().app_config->get_bool("background_processing");
        choice       = SS::choose_print_object(print, *mo, size_t(std::max(0, m_host.selected_instance())), bg);
        o.po_found   = choice.po != nullptr;
        o.ready      = o.po_found && choice.ready;
        o.consistent = o.ready && SS::input_consistent(*choice.po, *mo, choice.analysed_instance);
    }

    switch (W::decide(o)) {
    case W::Action::Wait:
        return;
    case W::Action::StartJob:
        start_job(*choice.po, *mo, choice.analysed_instance);
        return;
    case W::Action::Reslice:
        // The model was edited after the reslice: slice again.
        request_reslice();
        return;
    case W::Action::ResliceIdle:
        ++ m_idle_reslices;
        request_reslice();
        return;
    case W::Action::FailTimeout:
        cancel_run();
        set_failed(_u8L("Timed out while waiting for the slices."));
        return;
    case W::Action::FailNotInPrint:
        cancel_run();
        set_failed(choice.why_null == SS::PrintObjectChoice::Null::NoPrintableInstance ?
                       _u8L("The object has no printable instance.") :
                       _u8L("The object is not part of the print (is it outside of the bed?)."));
        return;
    case W::Action::FailSlicing:
        cancel_run();
        set_failed(_u8L("Slicing failed, see the slicing notification. Fix it and run again."));
        return;
    case W::Action::FailInterrupted:
        cancel_run();
        set_failed(_u8L("Slicing was interrupted (the object or the settings changed). Run again."));
        return;
    }
}

void SmartSupportsPanel::start_job(const PrintObject &po, ModelObject &mo, size_t analysed_instance)
{
    std::shared_ptr<const SS::AnalysisInput> input;
    {
        wxBusyCursor wait;
        try {
            input = std::make_shared<const SS::AnalysisInput>(SS::build_input(po, mo, analysed_instance, m_up));
        } catch (const std::exception &ex) {
            cancel_run();
            set_failed(ex.what());
            return;
        }
    }

    // Fingerprint of the settings and the object as analysed (review C M1): finalize discards the result if they changed
    // meanwhile, and the result is stale later exactly when they differ from this state.
    update_effective_config(true);
    const uint64_t fp_start = m_eff.valid ? SS::config_fingerprint(m_eff.full, mo, analysed_instance, input->params.user) : 0;

    auto result = std::make_shared<SS::Result>();
    m_job       = std::make_shared<JobShared>();
    const bool        fake   = fake_mode();
    m_run_fake               = fake;
    const std::string status = _u8L("Painting supports where needed");   // translated on the UI thread
    const uint64_t    run_id = m_run_id;
    std::weak_ptr<int> alive = m_alive;

    // Worker thread: touches only the input copy, the result slot and the atomics.
    auto process = [input, result, fake, status, cancel_flag = m_cancel_flag, job = m_job](Job::Ctl &ctl) {
        job->running.store(true);
        ScopeGuard guard([&ctl, &job, &status]() {
            ctl.update_status(100, status);   // closes the progress notification
            job->running.store(false);
        });
        const SS::IsCanceled is_canceled = [&ctl, cancel_flag]() { return ctl.was_canceled() || cancel_flag->load(); };
        // Monotone progress, status updates throttled to >= 100 ms; callable from any thread.
        std::mutex                 status_mutex;
        const Clock::time_point    t0 = Clock::now();
        std::atomic<long long>     last_ms { -1000 };
        const SS::ProgressFn progress = [&](int pct) {
            pct      = std::clamp(pct, 0, 100);
            int prev = job->progress.load();
            while (pct > prev && ! job->progress.compare_exchange_weak(prev, pct)) {}
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
            long long       last   = last_ms.load();
            if (now_ms - last < 100 || ! last_ms.compare_exchange_strong(last, now_ms))
                return;
            std::lock_guard<std::mutex> lock(status_mutex);
            ctl.update_status(std::min(job->progress.load(), 99), status);
        };
        *result = fake ? fake_run(*input, is_canceled, progress) :
                         SS::run(*input, SS::RunOptions{}, is_canceled, progress);
    };
    // UI thread.
    auto finalize = [this, alive, run_id, input, result, fp_start](bool canceled, std::exception_ptr &eptr) {
        if (! alive.lock() || wxGetApp().plater() == nullptr) {
            // The gizmo or the plater is gone (application shutdown: ~BoostThreadWorker delivers the finalize after
            // MainFrame::shutdown() cleared the plater, review C m1): nothing to apply, no Plater access.
            eptr = nullptr;
            return;
        }
        this->finalize_job(run_id, canceled, eptr, input, result, fp_start);
    };

    if (! queue_job(wxGetApp().plater()->get_ui_job_worker(), std::move(process), std::move(finalize))) {
        cancel_run();
        set_failed(_u8L("Another background task is running."));
        return;
    }
    m_state = State::Analysing;
    m_host.request_redraw(PollFrameMs);
}

void SmartSupportsPanel::finalize_job(uint64_t run_id, bool canceled, std::exception_ptr &eptr,
                                      const std::shared_ptr<const SS::AnalysisInput> &input,
                                      const std::shared_ptr<SS::Result> &result, uint64_t fp_start)
{
    if (run_id != m_run_id || m_state != State::Analysing) {
        // Superseded or cancelled by the panel.
        eptr = nullptr;
        return;
    }
    m_job.reset();
    if (canceled || is_canceled_exception(eptr)) {
        eptr    = nullptr;
        m_state = State::Idle;
        return;
    }
    if (eptr) {
        // PlaterJob shows the exception; the panel keeps one error line.
        set_failed(exception_message(eptr));
        return;
    }

    // Discard the result if the object, its meshes or its paint changed meanwhile (plan §2.4).
    ModelObject *mo   = m_host.model_object();
    auto        &sels = m_host.selectors();
    bool ok = m_host.is_open() && mo != nullptr && mo->id() == input->model_object_id &&
              result->paint.size() == input->volumes.size() && sels.size() == input->volumes.size();
    if (ok) {
        size_t k = 0;
        for (const ModelVolume *mv : mo->volumes) {
            if (! mv->is_model_part())
                continue;
            if (k >= input->volumes.size()) {
                ok = false;
                break;
            }
            const SS::VolumeInput &vi = input->volumes[k];
            if (mv->id() != vi.volume_id || mv->mesh_ptr().get() != vi.mesh.get() ||
                mv->supported_facets.timestamp() != vi.paint_timestamp || result->paint[k].volume_id != vi.volume_id) {
                ok = false;
                break;
            }
            ++ k;
        }
        ok = ok && k == input->volumes.size();
    }
    if (! ok) {
        m_state   = State::Idle;
        m_message = _u8L("The object or its paint changed during the analysis; the result was discarded. Run again.");
        m_host.request_redraw();
        return;
    }
    // Settings, instance orientation, z offset or volume matrices changed meanwhile (an undo of "Fix…", a sidebar edit, a
    // rotation): the fingerprint covers them all (review C M1).
    update_effective_config(true);
    if (! m_eff.valid || SS::config_fingerprint(m_eff.full, *mo, input->analysed_instance_idx, input->params.user) != fp_start) {
        m_state   = State::Idle;
        m_message = _u8L("The settings or the object's placement changed during the analysis; the result was discarded. Run again.");
        m_host.request_redraw();
        return;
    }

    bool changed = false;
    for (size_t i = 0; i < result->paint.size(); ++ i)
        if (result->paint[i].changed) {
            sels[i]->deserialize(result->paint[i].data, true);
            sels[i]->request_update_render_data();
            changed = true;
        }
    if (changed)
        m_host.commit_paint(_L("Smart support painting"));

    LastRun lr;
    lr.mo_id             = mo->id();
    lr.paint_timestamps  = paint_timestamps(*mo);
    lr.user              = input->params.user;
    lr.analysed_instance = input->analysed_instance_idx;
    lr.po_trafo_centered = input->po_trafo_centered;
    lr.instance_shift_mm = input->instance_shift_mm;
    lr.report            = std::move(result->report);
    lr.report_more[0]    = result->report_more[0];
    lr.report_more[1]    = result->report_more[1];
    for (int i = 0; i < 3; ++ i)
        lr.painted_areas[i] = result->painted_areas[i];
    for (const SS::ReportEntry &e : lr.report)
        if (e.why == SS::Why::InterfaceGap)
            lr.interface_gap_areas += std::max(1, e.count);
    for (const SS::ContractIssue &ci : input->contract)
        if (ci.key == SS::ContractIssue::Raft || ci.key == SS::ContractIssue::ReachBelowTipSpacing)
            lr.advisory.emplace_back(ci);
    lr.fake      = m_run_fake;
    lr.config_fp = fp_start;
    m_last_runs[mo->id().id] = std::move(lr);

    m_selected_entry.reset();
    m_marker_key.reset();
    m_state = State::Applied;
    poll(true);
    m_host.request_redraw();
}

void SmartSupportsPanel::cancel_run()
{
    // The job polls the flag (is_canceled) and ends by itself. No Worker::cancel(): between reading our running flag and
    // the call, our job may end and a foreign job start, which would be cancelled instead (review C m2).
    if (m_cancel_flag)
        m_cancel_flag->store(true);
    ++ m_run_id;   // a late finalize of the cancelled job is ignored
    m_job.reset();
    m_cancel_flag.reset();
    if (busy())
        m_state = State::Idle;
    m_host.request_redraw();
}

void SmartSupportsPanel::remove_smart_paint()
{
    ModelObject *mo = m_host.model_object();
    if (mo == nullptr || busy())
        return;
    // The button state may be up to one poll old: no undo step that changes nothing (review C m5).
    auto has_smart = [](const TriangleSelectorGUI &sel) {
        for (uint8_t mask = 1; mask <= SS::cbAll; ++ mask)
            if (sel.has_facets(SS::auto_state(mask)))
                return true;
        return false;
    };
    bool any = false;
    for (std::unique_ptr<TriangleSelectorGUI> &sel : m_host.selectors())
        if (has_smart(*sel)) {
            sel->remap_states(SS::clear_causes_map(SS::cbAll));
            sel->request_update_render_data();
            any = true;
        }
    if (any)
        m_host.commit_paint(_L("Remove smart paint"));
    m_last_runs.erase(mo->id().id);
    m_selected_entry.reset();
    m_state = State::Idle;
    m_message.clear();
    poll(true);
}

// ============================================================================ contract, "Fix…" (plan §6.4)

std::vector<std::pair<std::string, std::string>> SmartSupportsPanel::changes_for(bool include_advisory) const
{
    if (! m_eff.valid)
        return {};
    return SS::recommended_changes(m_eff.object, m_up.plate_policy, include_advisory);
}

void SmartSupportsPanel::apply_changes(bool include_advisory)
{
    ModelObject *mo = m_host.model_object();
    if (mo == nullptr || busy())
        return;
    update_effective_config(true);
    if (changes_for(include_advisory).empty())
        return;
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _L("Apply smart support settings"));
        try {
            SS::apply_recommended(mo->config, m_eff.object, m_up.plate_policy, include_advisory);
        } catch (const std::exception &ex) {
            set_failed(ex.what());
        }
    }
    wxGetApp().obj_list()->update_and_show_object_settings_item();
    m_host.canvas().post_event(SimpleEvent(EVT_GLCANVAS_FORCE_UPDATE));
    // The forced rebuild makes the warnings and "Apply and paint" see the new object config (D20).
    update_effective_config(true);
    poll(true);
}

void SmartSupportsPanel::show_fix_dialog(bool include_advisory)
{
    if (busy() || m_host.model_object() == nullptr)
        return;
    update_effective_config(true);
    const auto changes = changes_for(include_advisory);

    std::string body;
    if (! include_advisory)
        for (const SS::ContractIssue &ci : blocking_issues())
            body += "• " + issue_text(ci.key) + "\n";
    if (changes.empty())
        body += (body.empty() ? "" : "\n") + _u8L("No setting has to be changed.");
    else {
        body += (body.empty() ? "" : "\n") + _u8L("Set for this object only:") + "\n";
        for (const auto &[key, value] : changes)
            body += "• " + change_text(key, value) + "\n";
    }
    if (include_advisory) {
        body += "\n" + _u8L("Side effects:") + "\n";
        body += "• " + _u8L("no dense interface under any flat overhang") + "\n";
        body += "• " + _u8L("bottom interfaces also switch off") + "\n";
        body += "• " + _u8L("short on-model branches are removed (more 'Support on the model may be removed' entries)") + "\n";
    }
    const wxString headline = include_advisory ?
        _L("Top interface layers keep support away from steep slopes and hanging tips.") :
        _L("Some support settings do not match what painting where needed assumes.");

    SmartSupportsDialog dlg(wxGetApp().plater(), headline, from_u8(body), SmartSupportsDialog::Kind::Fix, ! changes.empty());
    const int res = dlg.ShowModal();
    if (res == wxID_APPLY)
        apply_changes(include_advisory);
    else if (res == wxID_YES) {
        apply_changes(include_advisory);
        start_run();   // no second prompt
    }
}

void SmartSupportsPanel::show_gate_dialog()
{
    const ModelObject *mo = m_host.model_object();
    if (mo == nullptr || ! can_start())
        return;
    update_effective_config(true);
    const std::vector<SS::ContractIssue> issues = blocking_issues();
    if (issues.empty()) {
        start_run();
        return;
    }
    const auto changes = changes_for(false);
    std::string body;
    for (const SS::ContractIssue &ci : issues)
        body += "• " + issue_text(ci.key) + "\n";
    if (! changes.empty()) {
        body += "\n" + _u8L("\"Apply and paint\" sets for this object only:") + "\n";
        for (const auto &[key, value] : changes)
            body += "• " + change_text(key, value) + "\n";
    }
    body += "\n" + _u8L("\"Paint anyway\" paints with the current settings; the slicer may then ignore or change the painted support.");

    const size_t mo_id = mo->id().id;
    SmartSupportsDialog dlg(wxGetApp().plater(), _L("Some support settings do not match what painting where needed assumes."),
                            from_u8(body), SmartSupportsDialog::Kind::Gate, ! changes.empty());
    const int res = dlg.ShowModal();
    if (res == wxID_YES) {
        apply_changes(false);
        start_run();
    } else if (res == wxID_NO) {
        m_gate_answered.insert(mo_id);   // asked once per object and session
        start_run();
    }
}

// ============================================================================ main window (plan §6.1)

namespace {

std::string entry_label()    { return _u8L("Paint where needed") + " »"; }
std::string degree_suffix()
{
    // Same msgid and context as the angle slider of the gizmo.
    return I18N::translate_utf8("°", "Degree sign to use in the respective slider in FDM supports gizmo,"
                                     "placed after the number with no whitespace in between.");
}

} // namespace

float SmartSupportsPanel::label_column_width() const
{
    float w = 0.f;
    for (const std::string &s : { _u8L("Paint"), _u8L("Overhang angle"), _u8L("Max overhang reach"), _u8L("Max bridge span"),
                                  _u8L("Support"), _u8L("Placement"), _u8L("Reach fade height"), _u8L("Max dome opening"),
                                  _u8L("Accumulation angle") })
        w = std::max(w, ImGuiPureWrap::calc_text_size(s).x);
    return w + wxGetApp().imgui()->scaled(1.f);
}

float SmartSupportsPanel::entry_line_width(const std::string &autopaint_label) const
{
    return ImGuiPureWrap::calc_button_size(autopaint_label).x + ImGui::GetStyle().ItemSpacing.x +
           ImGuiPureWrap::calc_button_size(entry_label()).x;
}

void SmartSupportsPanel::render_entry_line()
{
    if (! m_loaded)
        load_settings();
    if (ImGuiPureWrap::button(entry_label() + "##sas_entry")) {
        m_section_open      = true;
        m_scroll_to_section = true;
        ensure_alpha();
        save_settings();
        poll(true);
        m_host.request_redraw();
    }
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Opens the \"Paint where needed\" section at the end of this window."));
}

float SmartSupportsPanel::required_width() const
{
    if (! m_section_open)
        return 0.f;          // a closed section is one header line: the stock window width (review C m7)
    const ImGuiStyle &style = ImGui::GetStyle();
    const float frame   = ImGui::GetFrameHeight();
    const float spacing = style.ItemSpacing.x;
    auto text_w     = [](const std::string &s) { return ImGuiPureWrap::calc_text_size(s).x; };
    auto checkbox_w = [&](const std::string &s) { return frame + style.ItemInnerSpacing.x + text_w(s); };
    auto button_w   = [](const std::string &s) { return ImGuiPureWrap::calc_button_size(s).x; };
    const float labels = label_column_width();

    float w = labels + wxGetApp().imgui()->scaled(6.f) + ImGuiPureWrap::get_slider_icon_size().x;
    w = std::max(w, labels + checkbox_w(_u8L("Islands")) + spacing + checkbox_w(_u8L("Overhangs")));
    w = std::max(w, labels + checkbox_w(_u8L("Prefer build plate")) + spacing + checkbox_w(_u8L("Build plate only")));
    w = std::max(w, labels + text_w(_u8L("Critical and likely (recommended)")) + frame + 2.f * style.FramePadding.x);
    w = std::max(w, labels + checkbox_w(_u8L("Same as overhang angle")));
    w = std::max(w, button_w(_u8L("Paint where needed")) + spacing + button_w(_u8L("Remove smart paint")));
    return w;
}

void SmartSupportsPanel::render_section(float window_width)
{
    if (! m_loaded)
        load_settings();
    // The polling (config lookups, contract, PrintObject choice) only while it is shown or a run needs it (review C m7).
    if (m_section_open || busy())
        tick();

    ImGuiWrapper     &imgui = *wxGetApp().imgui();
    const ModelObject *mo   = m_host.model_object();

    ImGui::Separator();
    ImGui::SetNextItemOpen(m_section_open, ImGuiCond_Always);
    const bool open = ImGui::CollapsingHeader((_u8L("Paint where needed (geometric)") + "##sas_section").c_str());
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Paints support only where it is really needed, from the layer slices. Unlike Automatic painting "
                     "it keeps domes and short bridges free and keeps your own paint."));
    if (m_scroll_to_section) {
        ImGui::SetScrollHereY(0.f);
        m_scroll_to_section = false;
        m_host.request_redraw();
    }
    if (open != m_section_open) {
        m_section_open = open;
        if (open)
            ensure_alpha();
        save_settings();
        if (open)
            poll(true);
    }
    if (! open || mo == nullptr)
        return;
    ensure_alpha();

    const float labels_left = label_column_width();
    const float slider_w    = std::max(imgui.scaled(4.f), window_width - labels_left - ImGuiPureWrap::get_slider_icon_size().x);
    bool        changed     = false;
    auto label = [labels_left](const std::string &text) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(text);
        ImGui::SameLine(labels_left);
    };
    auto slider = [&](const char *id, float &value, float lo, float hi, const std::string &format, const std::string &tip) {
        ImGui::SetNextItemWidth(slider_w);
        return imgui.slider_float(id, &value, lo, hi, format.c_str(), 1.0f, true, from_u8(tip));
    };
    const std::string mm = " " + _u8L("mm");

    // Paint: [x] Islands [x] Overhangs
    label(_u8L("Paint"));
    bool islands   = (m_up.paint_causes & SS::cbIsland) != 0;
    bool overhangs = (m_up.paint_causes & SS::cbCantilever) != 0;
    if (ImGuiPureWrap::checkbox(_u8L("Islands"), islands))
        changed = true;
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Parts that start in mid-air: lowest points, floating parts, downward tips. They always need support."));
    ImGui::SameLine();
    if (ImGuiPureWrap::checkbox(_u8L("Overhangs"), overhangs))
        changed = true;
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Overhangs that hang from one side further than the maximum reach, including long shallow slopes "
                     "(the reach adds up over the layers)."));
    m_up.paint_causes = uint8_t((islands ? SS::cbIsland : 0) | (overhangs ? SS::cbCantilever : 0));

    // Overhang angle, 0 = auto
    label(_u8L("Overhang angle"));
    float alpha = float(m_up.alpha_deg);
    if (slider("##sas_alpha", alpha, 0.f, 89.f, m_up.alpha_deg == 0 ? _u8L("auto") : "%.0f" + degree_suffix(),
               _u8L("Slopes steeper than this angle (from horizontal) print without support. "
                    "0 = automatic (half an extrusion width per layer)."))) {
        m_up.alpha_deg = int(std::lround(std::clamp(alpha, 0.f, 89.f)));
        changed = true;
    }

    label(_u8L("Max overhang reach"));
    float k_max = float(m_up.K_max);
    if (slider("##sas_k_max", k_max, 0.5f, 20.f, "%.1f" + mm,
               _u8L("Longest unsupported overhang that still prints. Larger values give less support."))) {
        m_up.K_max = std::clamp(double(k_max), 0.5, 20.);
        changed = true;
    }

    label(_u8L("Max bridge span"));
    float b_max = float(m_up.B_max);
    if (slider("##sas_b_max", b_max, 5.f, 200.f, "%.0f" + mm,
               _u8L("Longest bridge that prints without support, along the slicer's bridge direction. "
                    "Longer bridges are only reported in this version."))) {
        m_up.B_max = std::clamp(double(b_max), 5., 200.);
        changed = true;
    }

    // Support level (D9)
    label(_u8L("Support"));
    {
        const std::array<std::string, 2> levels { _u8L("Critical only"), _u8L("Critical and likely (recommended)") };
        const std::array<std::string, 2> tips   { _u8L("Only where the print would fail."),
                                                  _u8L("Also where the print is likely to droop or curl.") };
        const int sel = m_up.lowest_level == SS::Level::Must ? 0 : 1;
        ImGui::SetNextItemWidth(window_width - labels_left);
        if (ImGui::BeginCombo("##sas_level", levels[sel].c_str())) {
            for (int i = 0; i < 2; ++ i) {
                if (ImGui::Selectable(levels[i].c_str(), i == sel) && i != sel) {
                    m_up.lowest_level = i == 0 ? SS::Level::Must : SS::Level::Should;
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    tooltip(tips[i]);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            tooltip(tips[sel]);
    }

    // Placement
    label(_u8L("Placement"));
    if (ImGuiPureWrap::radio_button(_u8L("Prefer build plate"), m_up.plate_policy == SS::PlatePolicy::PreferPlate) &&
        m_up.plate_policy != SS::PlatePolicy::PreferPlate) {
        m_up.plate_policy = SS::PlatePolicy::PreferPlate;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Support stands on the build plate wherever possible; on the model only where a part would otherwise fail."));
    ImGui::SameLine();
    if (ImGuiPureWrap::radio_button(_u8L("Build plate only"), m_up.plate_policy == SS::PlatePolicy::PlateOnly) &&
        m_up.plate_policy != SS::PlatePolicy::PlateOnly) {
        m_up.plate_policy = SS::PlatePolicy::PlateOnly;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        tooltip(_u8L("Never put support on the model. Areas that cannot reach the plate are only reported; the slicer would drop them."));

    // Advanced (no tree indentation, so the label column stays aligned)
    if (ImGui::TreeNodeEx((_u8L("Advanced") + "##sas_advanced").c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        label(_u8L("Reach fade height"));
        float h_s = float(m_up.H_s);
        if (slider("##sas_h_s", h_s, 0.f, 5.f, "%.1f" + mm,
                   _u8L("Height of plain material above an overhang after which it no longer passes its reach on (stairs). "
                        "0 = judge each layer alone."))) {
            m_up.H_s = std::clamp(double(h_s), 0., 5.);
            changed = true;
        }
        label(_u8L("Max dome opening"));
        float b_ceil = float(m_up.B_ceil);
        if (slider("##sas_b_ceil", b_ceil, 0.f, 200.f, "%.0f" + mm,
                   _u8L("Largest opening of a dome, funnel or counterbore ceiling that holds itself."))) {
            m_up.B_ceil = std::clamp(double(b_ceil), 0., 200.);
            changed = true;
        }
        const std::string beta_tip = _u8L("Slopes steeper than this do not add to the reach. Lower it to tolerate long "
                                          "slopes just below the overhang angle.");
        label(_u8L("Accumulation angle"));
        if (ImGuiPureWrap::checkbox(_u8L("Same as overhang angle"), m_up.beta_equals_alpha))
            changed = true;
        if (ImGui::IsItemHovered())
            tooltip(beta_tip);
        if (! m_up.beta_equals_alpha) {
            // β ∈ [10°, α_eff]; α_eff of auto α from the layer height and an estimated external perimeter width (§3.4).
            double alpha_eff = m_up.alpha_deg;
            if (alpha_eff <= 0.) {
                const double h     = m_eff.valid ? m_eff.object.layer_height.value : 0.2;
                const double ext_w = 0.45;
                alpha_eff = std::atan2(h, 0.5 * ext_w) * 180. / M_PI;
            }
            const float beta_max = float(std::max(10., std::floor(alpha_eff)));
            float beta = float(std::clamp(m_up.beta_deg, 10., double(beta_max)));
            ImGui::Dummy(ImVec2(0.f, 0.f));
            ImGui::SameLine(labels_left);
            if (slider("##sas_beta", beta, 10.f, beta_max, "%.0f" + degree_suffix(), beta_tip)) {
                m_up.beta_deg = std::clamp(double(beta), 10., double(beta_max));
                changed = true;
            }
        }
        if (ImGuiPureWrap::button(_u8L("Reset to defaults") + "##sas_reset")) {
            reset_to_defaults();
            changed = false;
            poll(true);
        }
    }
    if (changed) {
        save_settings();
        poll(true);
    }

    // Pre-run contract warning (non-advisory issues only, D30)
    if (const std::vector<SS::ContractIssue> issues = blocking_issues(); ! issues.empty()) {
        ImGui::AlignTextToFramePadding();
        const unsigned n = unsigned(issues.size());
        ImGuiPureWrap::text_colored(ImGuiPureWrap::COL_ORANGE_LIGHT,
            "! " + format(into_u8(_L_PLURAL("%1% support setting does not match", "%1% support settings do not match", n)), n));
        if (ImGui::IsItemHovered()) {
            std::string tip;
            for (const SS::ContractIssue &ci : issues)
                tip += (tip.empty() ? "" : "\n") + issue_text(ci.key);
            tooltip(tip);
        }
        ImGui::SameLine();
        imgui.disabled_begin(busy());
        if (ImGuiPureWrap::button(_u8L("Fix…") + "##sas_fix_pre"))
            defer([](SmartSupportsPanel &p) { p.show_fix_dialog(false); });
        imgui.disabled_end();
        if (ImGui::IsItemHovered()) {
            std::string tip = _u8L("Set for this object only:");
            for (const auto &[key, value] : changes_for(false))
                tip += "\n• " + change_text(key, value);
            tooltip(tip);
        }
    }
    if (m_poll.instance_note >= 0)
        text_wrapped_colored(ImGuiPureWrap::COL_ORANGE_LIGHT,
                             "! " + format(_u8L("Instances differ in rotation: computed for instance %1%"), m_poll.instance_note + 1),
                             window_width);

    // Run / cancel
    if (m_state == State::WaitingForSlices) {
        const long long secs = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - m_wait_start).count();
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(format(_u8L("Slicing… %1% s"), secs));
        ImGui::SameLine();
        if (ImGuiPureWrap::button(_u8L("Cancel") + "##sas_cancel_wait"))
            cancel_run();
    } else if (m_state == State::Analysing) {
        const int pct = m_job ? m_job->progress.load() : 0;
        const std::string overlay = format(_u8L("Analysing… %1% %%"), pct);
        const float cancel_w = ImGuiPureWrap::calc_button_size(_u8L("Cancel")).x;
        ImGui::ProgressBar(float(pct) / 100.f, ImVec2(std::max(imgui.scaled(4.f), window_width - cancel_w - ImGui::GetStyle().ItemSpacing.x), 0.f),
                           overlay.c_str());
        ImGui::SameLine();
        if (ImGuiPureWrap::button(_u8L("Cancel") + "##sas_cancel_run"))
            cancel_run();
    } else {
        std::string why_not;
        const bool can = can_start(&why_not);
        imgui.disabled_begin(! can);
        if (ImGuiPureWrap::button(_u8L("Paint where needed") + "##sas_paint"))
            on_paint_clicked();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            tooltip(can || why_not.empty() ?
                    _u8L("Slices the object if needed and paints enforcers where support is needed. Replaces earlier smart paint; "
                         "your own paint is kept. Painting over smart paint by hand turns it into your own paint.") : why_not);
        imgui.disabled_end();
        ImGui::SameLine();
        imgui.disabled_begin(! m_poll.has_smart_paint);
        if (ImGuiPureWrap::button(_u8L("Remove smart paint") + "##sas_remove"))
            remove_smart_paint();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            tooltip(_u8L("Removes all smart paint. Your own paint is kept."));
        imgui.disabled_end();
    }
    text_wrapped_colored(ColGrey, _u8L("Paint a blocker to keep an area free; erased smart paint returns on the next run."), window_width);

    // Result
    const LastRun *lr = current_last_run();
    if (m_state == State::Failed && ! m_message.empty())
        text_wrapped_colored(ColError, m_message, window_width);
    else if (! m_message.empty())
        text_wrapped_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, m_message, window_width);

    if (lr == nullptr) {
        if (m_poll.has_smart_paint && ! busy())
            text_wrapped_colored(ColGrey, _u8L("Smart paint present — run again for the report."), window_width);
        return;
    }

    int problems = lr->report_more[0], notes = lr->report_more[1];
    for (const SS::ReportEntry &e : lr->report)
        ++ (is_problem(e) ? problems : notes);
    notes += int(lr->advisory.size());

    if (m_poll.stale_paint)
        text_wrapped_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, _u8L("Result is stale: the paint changed — run again."), window_width);
    else if (m_poll.stale_config)
        text_wrapped_colored(ImGuiPureWrap::COL_ORANGE_LIGHT, _u8L("Result is stale: settings changed — run again."), window_width);
    else {
        const int total = lr->painted_areas[0] + lr->painted_areas[1] + lr->painted_areas[2];
        if (total > 0)
            ImGuiPureWrap::text_wrapped(format(_u8L("Painted %1% areas (islands %2%, overhangs %3%) on downward faces — press 2 for the bottom view."),
                                               total, lr->painted_areas[int(SS::Cause::Island)], lr->painted_areas[int(SS::Cause::Cantilever)]),
                                        window_width);
        else if (problems > 0)
            ImGuiPureWrap::text_wrapped(_u8L("Nothing was painted."), window_width);
        else
            ImGuiPureWrap::text_wrapped(_u8L("Nothing needs support with these settings."), window_width);

        // After-run interface line (D30): advisory, only after a run with InterfaceGap needs.
        if (lr->interface_gap_areas > 0) {
            text_wrapped_colored(ImGuiPureWrap::COL_ORANGE_LIGHT,
                "! " + format(_u8L("%1% areas (steep slopes, hanging tips) cannot be supported while Top interface layers > 0"),
                              lr->interface_gap_areas),
                window_width);
            imgui.disabled_begin(busy());
            if (ImGuiPureWrap::button(_u8L("Fix…") + "##sas_fix_iface"))
                defer([](SmartSupportsPanel &p) { p.show_fix_dialog(true); });
            imgui.disabled_end();
        }
    }
    if (problems + notes > 0) {
        ImGui::AlignTextToFramePadding();
        ImGuiPureWrap::text(format(_u8L("Report: %1% problems, %2% notes"), problems, notes));
        ImGui::SameLine();
        if (ImGuiPureWrap::button((m_report_open ? _u8L("Hide report") : _u8L("Show report")) + "##sas_report")) {
            m_report_open = ! m_report_open;
            if (! m_report_open)
                m_selected_entry.reset();
        }
    }
    if (lr->fake)
        text_wrapped_colored(ColGrey, "(debug_fake_result)", window_width);
}

// ============================================================================ report window (plan §6.2)

void SmartSupportsPanel::render_report_window(float x, float y, float bottom_limit)
{
    const LastRun *lr = current_last_run();
    if (! m_report_open || lr == nullptr || ! m_host.is_open() ||
        (lr->report.empty() && lr->advisory.empty() && lr->report_more[0] + lr->report_more[1] == 0))
        return;

    ImGuiWrapper     &imgui = *wxGetApp().imgui();
    const ImGuiStyle &style = ImGui::GetStyle();

    // Column widths over all rows, so both tables line up.
    ReportColumns cols;
    cols.dot = ImGui::GetFontSize();
    for (const SS::ReportEntry &e : lr->report) {
        cols.text = std::max(cols.text, ImGuiPureWrap::calc_text_size(state_text(e, lr->analysed_instance)).x);
        if (e.area_mm2 > 0.)
            cols.z = std::max(cols.z, ImGuiPureWrap::calc_text_size(format(_u8L("Z %1% mm"), fmt_number(e.z_first, 1))).x);
    }
    cols.table = cols.dot + cols.text + cols.z + 6.f * style.CellPadding.x + style.ScrollbarSize;

    const float max_h = std::max(imgui.scaled(8.f), std::min(0.6f * ImGui::GetIO().DisplaySize.y, bottom_limit - y));
    ImGuiPureWrap::set_next_window_pos(x, y, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, max_h));
    bool keep_open = true;
    ImGuiPureWrap::begin(_u8L("Smart support report"), &keep_open,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    const bool has_row    = m_selected_entry && *m_selected_entry < lr->report.size() && lr->report[*m_selected_entry].area_mm2 > 0.;
    const bool has_marker = has_row && marker_position(*lr, lr->report[*m_selected_entry]).has_value();
    imgui.disabled_begin(! has_marker);
    if (ImGuiPureWrap::button(_u8L("Clip at this height") + "##sas_clip"))
        clip_at_selected();
    if (has_row && ! has_marker && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        tooltip(format(_u8L("Computed for instance %1%: select it to see the marker."), lr->analysed_instance + 1));
    imgui.disabled_end();
    ImGui::SameLine();
    ImGuiPureWrap::text_colored(ColGrey, _u8L("Click a row to mark it in the 3D view."));

    render_report_group(*lr, true, cols);
    render_report_group(*lr, false, cols);

    ImGuiPureWrap::end();
    if (! keep_open) {
        m_report_open = false;
        m_selected_entry.reset();
    }
}

void SmartSupportsPanel::render_report_group(const LastRun &lr, bool problems, const ReportColumns &cols)
{
    std::vector<size_t> rows;
    for (size_t i = 0; i < lr.report.size(); ++ i)
        if (is_problem(lr.report[i]) == problems)
            rows.emplace_back(i);
    const int more = lr.report_more[problems ? 0 : 1];
    const std::vector<SS::ContractIssue> no_advisory;
    const std::vector<SS::ContractIssue> &advisory = problems ? no_advisory : lr.advisory;
    const int total = int(rows.size()) + more + int(advisory.size());

    const std::string title = format(problems ? _u8L("Problems (%1%)") : _u8L("Notes (%1%)"), total) +
                              (problems ? "###sas_problems" : "###sas_notes");
    if (! ImGui::CollapsingHeader(title.c_str(), problems ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        return;
    if (total == 0) {
        ImGuiPureWrap::text_colored(ColGrey, _u8L("None"));
        return;
    }

    // Advisory notes without a report state (Raft, ReachBelowTipSpacing), from in.contract.
    for (const SS::ContractIssue &ci : advisory)
        ImGuiPureWrap::text_wrapped(issue_text(ci.key), cols.table);

    if (! rows.empty()) {
        const ImGuiStyle &style   = ImGui::GetStyle();
        const float       row_h   = ImGui::GetTextLineHeight() + 2.f * style.CellPadding.y;
        const int         visible = std::min<int>(int(rows.size()), MaxVisibleRows);
        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable(problems ? "##sas_table_problems" : "##sas_table_notes", 3, flags,
                              ImVec2(cols.table, float(visible) * row_h + 2.f))) {
            ImGui::TableSetupColumn("##level", ImGuiTableColumnFlags_WidthFixed, cols.dot);
            ImGui::TableSetupColumn("##text",  ImGuiTableColumnFlags_WidthFixed, cols.text);
            ImGui::TableSetupColumn("##z",     ImGuiTableColumnFlags_WidthFixed, cols.z);
            const float line_h = ImGui::GetTextLineHeight();
            for (size_t idx : rows) {
                const SS::ReportEntry &e = lr.report[idx];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ImVec2 p        = ImGui::GetCursorScreenPos();
                const bool   selected = m_selected_entry && *m_selected_entry == idx;
                if (ImGui::Selectable(("##sas_row" + std::to_string(idx)).c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                    if (selected)
                        m_selected_entry.reset();
                    else
                        m_selected_entry = idx;
                    m_host.request_redraw();
                }
                const bool hovered = ImGui::IsItemHovered();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 0.5f * cols.dot, p.y + 0.5f * line_h), 0.3f * line_h,
                                                            level_color(e.level), 12);
                ImGui::TableSetColumnIndex(1);
                ImGuiPureWrap::text(state_text(e, lr.analysed_instance));
                ImGui::TableSetColumnIndex(2);
                if (e.area_mm2 > 0.)
                    ImGuiPureWrap::text(format(_u8L("Z %1% mm"), fmt_number(e.z_first, 1)));
                if (hovered) {
                    std::string tip = state_text(e, lr.analysed_instance);
                    if (e.area_mm2 > 0.) {
                        tip += "\n" + (e.layer_first == e.layer_last ?
                            format(_u8L("Layer %1%, Z %2% mm"), e.layer_first + 1, fmt_number(e.z_first, 2)) :
                            format(_u8L("Layers %1%–%2%, Z %3%–%4% mm"), e.layer_first + 1, e.layer_last + 1,
                                   fmt_number(e.z_first, 2), fmt_number(e.z_last, 2)));
                        const Vec2d bed = unscale(e.centroid) + lr.instance_shift_mm;
                        tip += "\n" + format(_u8L("Bed X %1% mm, Y %2% mm"), fmt_number(bed.x(), 1), fmt_number(bed.y(), 1));
                        tip += "\n" + format(_u8L("Area %1% mm²"), fmt_number(e.area_mm2, 1));
                    }
                    if (e.count > 1 && e.state != SS::ReportState::SeveralPrintObjects)
                        tip += "\n" + format(_u8L("%1% areas merged"), e.count);
                    tip += "\n" + format(_u8L("Cause: %1%, level: %2%"), cause_name(e.cause), level_name(e.level));
                    if (const std::string advice = advice_text(e); ! advice.empty())
                        tip += "\n\n" + advice;
                    tooltip(tip);
                }
            }
            ImGui::EndTable();
        }
    }
    if (more > 0)
        ImGuiPureWrap::text_colored(ColGrey, format(_u8L("+%1% more"), more));
}

// ============================================================================ marker (plan §6.3)

std::optional<Vec3d> SmartSupportsPanel::marker_position(const LastRun &lr, const SS::ReportEntry &e) const
{
    const ModelObject *mo = m_host.model_object();
    if (e.area_mm2 <= 0. || mo == nullptr || mo->id() != lr.mo_id || lr.analysed_instance >= mo->instances.size())
        return std::nullopt;
    // Drawn on the selected instance, the only one InstancesHider shows: valid if it has the analysed instance's
    // orientation and z offset (same PrintObject frame), else no marker and no clip (review C m4).
    const int sel = m_host.selected_instance();
    if (sel < 0 || size_t(sel) >= mo->instances.size())
        return std::nullopt;
    const ModelInstance &analysed = *mo->instances[lr.analysed_instance];
    const ModelInstance &shown    = *mo->instances[size_t(sel)];
    if (size_t(sel) != lr.analysed_instance &&
        (! shown.get_matrix_no_offset().isApprox(analysed.get_matrix_no_offset()) ||
         std::abs(shown.get_offset(Z) - analysed.get_offset(Z)) > EPSILON))
        return std::nullopt;
    // world = live instance · po_trafo_centered⁻¹ · (p, slice_z)   (plan §3.3)
    const Vec3d p(unscale<double>(e.centroid.x()), unscale<double>(e.centroid.y()), e.slice_z);
    return Vec3d(shown.get_matrix() * (lr.po_trafo_centered.inverse() * p));
}

void SmartSupportsPanel::clip_at_selected()
{
    const LastRun *lr = current_last_run();
    ModelObject   *mo = m_host.model_object();
    CommonGizmosDataObjects::ObjectClipper *clipper = m_host.clipper();
    if (lr == nullptr || mo == nullptr || clipper == nullptr || ! m_selected_entry || *m_selected_entry >= lr->report.size())
        return;
    const std::optional<Vec3d> pos = marker_position(*lr, lr->report[*m_selected_entry]);
    if (! pos)
        return;
    // Hide everything above the marker; the ratio keeps the clipping slider consistent (ObjectClipper::set_position_by_ratio).
    const int    inst   = std::clamp(m_host.selected_instance(), 0, int(mo->instances.size()) - 1);
    const double radius = std::max(1e-3, mo->instance_bounding_box(size_t(inst)).radius());
    const Vec3d  normal = Vec3d::UnitZ();
    const double offset = pos->z();
    const double dist   = normal.dot(mo->instances[inst]->get_offset());
    const double ratio  = std::clamp((dist + radius - offset) / (2. * radius), 0.001, 1.);
    clipper->set_range_and_pos(normal, offset, ratio);
    m_host.request_redraw();
}

void SmartSupportsPanel::render_3d()
{
    const LastRun *lr = current_last_run();
    if (! m_report_open || lr == nullptr || ! m_selected_entry || *m_selected_entry >= lr->report.size())
        return;
    const SS::ReportEntry     &e   = lr->report[*m_selected_entry];
    const std::optional<Vec3d> pos = marker_position(*lr, e);
    if (! pos)
        return;

    // Ring of radius max(1 mm, sqrt(area / pi)) plus a downward tick, as lines.
    const double radius = std::max(1., std::sqrt(e.area_mm2 / M_PI));
    if (! m_marker_key || m_marker_key->first != *m_selected_entry || m_marker_key->second != radius || ! m_marker.is_initialized()) {
        constexpr unsigned int segments = 48;
        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
        init_data.reserve_vertices(segments + 2);
        init_data.reserve_indices(2 * segments + 2);
        for (unsigned int i = 0; i < segments; ++ i) {
            const double a = 2. * M_PI * double(i) / double(segments);
            init_data.add_vertex(Vec3f(float(radius * std::cos(a)), float(radius * std::sin(a)), 0.f));
            init_data.add_line(i, (i + 1) % segments);
        }
        init_data.add_vertex(Vec3f(0.f, 0.f, 0.f));
        init_data.add_vertex(Vec3f(0.f, 0.f, -float(std::max(2., radius))));
        init_data.add_line(segments, segments + 1);
        m_marker.reset();
        m_marker.init_from(std::move(init_data));
        m_marker_key = std::make_pair(*m_selected_entry, radius);
    }

#if SLIC3R_OPENGL_ES
    GLShaderProgram *shader = wxGetApp().get_shader("dashed_lines");
#else
    GLShaderProgram *shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader("dashed_thick_lines") :
                                                                                wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES
    if (shader == nullptr)
        return;

    GLboolean depth_test = GL_FALSE;
    glsafe(::glGetBooleanv(GL_DEPTH_TEST, &depth_test));
    glsafe(::glDisable(GL_DEPTH_TEST));   // the marker sits under the model: always visible

    shader->start_using();
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(*pos));
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        const std::array<int, 4> &viewport = camera.get_viewport();
        shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
        shader->set_uniform("width", 0.25f);
        shader->set_uniform("gap_size", 0.0f);
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    m_marker.set_color(ColorRGBA(1.f, 0.9f, 0.2f, 1.f));
    m_marker.render();
    shader->stop_using();

    if (depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));
}

} // namespace Slic3r::GUI
