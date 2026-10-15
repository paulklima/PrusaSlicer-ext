#ifndef slic3r_SmartSupportsPanel_hpp_
#define slic3r_SmartSupportsPanel_hpp_

// Smart support painting: the "Paint where needed" section of the FDM supports gizmo, the report window,
// the 3D marker, the run state machine, the UI job, the "Fix…" dialogs and the fake result (plan §6; WP3).
//
// The gizmo owns one SmartSupportsPanel and implements SmartSupportsHost (plan §6.9). The panel never keeps
// pointers to Print / PrintObject data across frames (Print::apply may delete them); the analysis runs on the
// shared UI worker on a self-contained SmartSupports::AnalysisInput and touches no Model / Print / wx object.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/SmartSupports.hpp"

#include "slic3r/GUI/GLModel.hpp"

namespace Slic3r {

class ModelObject;

namespace GUI {

class GLCanvas3D;
class TriangleSelectorGUI;
namespace CommonGizmosDataObjects { class ObjectClipper; }

// What the panel needs from the gizmo (plan §6.9; extended by canvas()).
struct SmartSupportsHost {
    virtual ~SmartSupportsHost() = default;
    // The edited object, nullptr when none.
    virtual ModelObject *model_object() = 0;
    // Index of the selected instance of model_object().
    virtual int selected_instance() const = 0;
    // One selector per model part, in ModelObject::volumes order.
    virtual std::vector<std::unique_ptr<TriangleSelectorGUI>> &selectors() = 0;
    // Takes a GizmoAction snapshot of the model (still holding the old paint), then writes the selectors into the
    // model (update_model_object()). The caller has already changed the selectors.
    virtual void commit_paint(const wxString &snapshot_name) = 0;
    // delay_ms > 0: schedule a frame after the delay (polling); 0: redraw now.
    virtual void request_redraw(int delay_ms = 0) = 0;
    virtual bool is_open() const = 0;
    virtual CommonGizmosDataObjects::ObjectClipper *clipper() = 0;
    virtual GLCanvas3D &canvas() = 0;
};

class SmartSupportsPanel {
public:
    explicit SmartSupportsPanel(SmartSupportsHost &host);
    ~SmartSupportsPanel();
    SmartSupportsPanel(const SmartSupportsPanel &) = delete;
    SmartSupportsPanel &operator=(const SmartSupportsPanel &) = delete;

    // ---- ImGui, called inside the gizmo window
    // Width of the entry line ("Automatic painting" button + the entry), see render_entry_line().
    float entry_line_width(const std::string &autopaint_label) const;
    // Renders the entry next to the "Automatic painting" button (call ImGui::SameLine() first).
    void  render_entry_line();
    // Minimal content width of the section rows.
    float required_width() const;
    // Polls the state machine, then renders the section at the end of the gizmo window.
    void  render_section(float window_width);
    // Separate window to the right of the gizmo window (plan §6.2).
    void  render_report_window(float x, float y, float bottom_limit);
    // 3D marker of the selected report row (inside render_painter_gizmo()).
    void  render_3d();

    // ---- events
    void  on_data_changed();     // selection / model / undo-redo changes
    void  on_shutdown();         // gizmo closes: cancels a running run, keeps the last results
    bool  busy() const { return m_state == State::WaitingForSlices || m_state == State::Analysing; }

    // ---- window height clamp (plan §6.1): desired height of the gizmo window in the last frame
    float last_window_height() const { return m_last_window_height; }
    void  set_last_window_height(float h) { m_last_window_height = h; }

private:
    enum class State : uint8_t { Idle, WaitingForSlices, Analysing, Applied, Failed };

    // Shared between the UI thread and the job (atomics only).
    struct JobShared {
        std::atomic<bool> running  { false };
        std::atomic<int>  progress { 0 };
    };

    // Results of the last completed run of one object (session memory, plan §2.4 / D14).
    struct LastRun {
        ObjectID                                    mo_id;
        std::vector<std::pair<ObjectID, uint64_t>>  paint_timestamps;   // model parts after applying the paint
        uint64_t                                    config_fp = 0;
        SmartSupports::UserParams                   user;
        size_t                                      analysed_instance = 0;
        Transform3d                                 po_trafo_centered { Transform3d::Identity() };
        Vec2d                                       instance_shift_mm { Vec2d::Zero() };
        std::vector<SmartSupports::ReportEntry>     report;
        int                                         report_more[2]   = {};
        int                                         painted_areas[3] = {};
        int                                         interface_gap_areas = 0;
        std::vector<SmartSupports::ContractIssue>   advisory;           // Raft, ReachBelowTipSpacing (in.contract)
        bool                                        fake = false;
    };

    // Effective object config (preset full_config() + object overrides), cached (plan §6.4, D20).
    struct EffectiveConfig {
        bool                                        valid = false;
        ObjectID                                    mo_id;
        uint64_t                                    mo_config_timestamp = 0;
        SmartSupports::PlatePolicy                  policy = SmartSupports::PlatePolicy::PreferPlate;
        std::chrono::steady_clock::time_point       when;
        DynamicPrintConfig                          full;
        PrintObjectConfig                           object;
        std::vector<SmartSupports::ContractIssue>   issues;
    };

    // Cheap state refreshed at most twice per second.
    struct PollInfo {
        bool                                        valid = false;
        std::chrono::steady_clock::time_point       when;
        ObjectID                                    mo_id;
        bool                                        has_smart_paint = false;
        bool                                        stale_paint = false;
        bool                                        stale_config = false;
        bool                                        printable = true;
        int                                         instance_note = -1;   // >= 0: live index of the analysed instance
    };

    // settings
    void load_settings();
    void save_settings() const;
    void reset_to_defaults();
    bool fake_mode() const;

    // polling
    void tick();
    void poll(bool force);
    void update_effective_config(bool force);
    bool has_smart_paint(const ModelObject &mo);
    std::vector<SmartSupports::ContractIssue> blocking_issues() const;
    const LastRun *current_last_run() const;

    // run state machine
    bool can_start(std::string *why_not = nullptr) const;
    void on_paint_clicked();
    void start_run();
    void poll_waiting();
    void start_job(const PrintObject &po, ModelObject &mo, size_t analysed_instance);
    void finalize_job(uint64_t run_id, bool canceled, std::exception_ptr &eptr,
                      const std::shared_ptr<const SmartSupports::AnalysisInput> &input,
                      const std::shared_ptr<SmartSupports::Result> &result);
    void cancel_run();
    void set_failed(const std::string &message);
    void remove_smart_paint();

    // contract, "Fix…"
    std::vector<std::pair<std::string, std::string>> changes_for(bool include_advisory) const;
    void apply_changes(bool include_advisory);
    void show_fix_dialog(bool include_advisory);
    void show_gate_dialog();

    // layout
    float label_column_width() const;

    // report, marker
    struct ReportColumns { float dot = 0.f, text = 0.f, z = 0.f, table = 0.f; };
    void render_report_group(const LastRun &lr, bool problems, const ReportColumns &columns);
    std::optional<Vec3d> marker_position(const LastRun &lr, const SmartSupports::ReportEntry &entry) const;
    void clip_at_selected();

    // Runs fn on the UI thread after the current event, if the panel still exists.
    void defer(std::function<void(SmartSupportsPanel &)> fn);

    SmartSupportsHost                          &m_host;
    std::shared_ptr<int>                        m_alive { std::make_shared<int>(0) };

    // settings (AppConfig section "smart_auto_supports", plan §6.8)
    bool                                        m_loaded        = false;
    bool                                        m_section_open  = false;
    SmartSupports::UserParams                   m_up;

    // UI
    bool                                        m_scroll_to_section = false;
    bool                                        m_report_open       = false;
    std::optional<size_t>                       m_selected_entry;
    float                                       m_last_window_height = 0.f;
    std::set<size_t>                            m_gate_answered;     // objects answered with "Paint anyway"

    // state machine (plan §6.6)
    State                                       m_state = State::Idle;
    uint64_t                                    m_run_id = 0;
    ObjectID                                    m_run_mo_id;
    bool                                        m_reslice_requested = false;
    std::chrono::steady_clock::time_point       m_wait_start;
    std::chrono::steady_clock::time_point       m_last_reslice;
    std::shared_ptr<std::atomic<bool>>          m_cancel_flag;
    std::shared_ptr<JobShared>                  m_job;
    std::string                                 m_message;           // Failed: error; Idle: notice

    // caches
    EffectiveConfig                             m_eff;
    PollInfo                                    m_poll;
    std::vector<std::pair<ObjectID, uint64_t>>  m_paint_key;         // model-part timestamps of m_paint_key_smart
    bool                                        m_paint_key_smart = false;
    std::map<size_t, LastRun>                   m_last_runs;         // by ModelObject id

    // marker geometry of the selected entry
    GLModel                                     m_marker;
    std::optional<std::pair<size_t, double>>    m_marker_key;        // (entry, radius)
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_SmartSupportsPanel_hpp_
