// Fork feature "config-backup", see doc/fork/features/config-backup/README.md.

#include "ConfigBackupDialog.hpp"

#include <algorithm>
#include <map>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dirdlg.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "UnsavedChangesDialog.hpp"

namespace Slic3r {
namespace GUI {

namespace CB = ConfigBackup;

static wxString reason_label(const std::string &reason)
{
    if (reason == CB::reason_id(CB::Reason::Manual))             return _L("Manual");
    if (reason == CB::reason_id(CB::Reason::PresetSave))         return _L("Preset saved");
    if (reason == CB::reason_id(CB::Reason::BeforeUpdate))       return _L("Before PrusaSlicer update");
    if (reason == CB::reason_id(CB::Reason::BeforeConfigUpdate)) return _L("Before configuration update");
    if (reason == CB::reason_id(CB::Reason::BeforeRestore))      return _L("Before restore");
    return from_u8(reason);
}

static wxString category_label(const std::string &subdir)
{
    if (subdir == "print")            return _L("Print settings");
    if (subdir == "sla_print")        return _L("SLA print settings");
    if (subdir == "filament")         return _L("Filament settings");
    if (subdir == "sla_material")     return _L("SLA material settings");
    if (subdir == "printer")          return _L("Printer settings");
    if (subdir == "physical_printer") return _L("Physical printers");
    if (subdir == "vendor")           return _L("Vendor profiles");
    return from_u8(subdir);
}

static wxString size_label(uintmax_t size)
{
    if (size >= 1024 * 1024)
        return wxString::Format("%.1f MB", double(size) / (1024. * 1024.));
    return wxString::Format("%.0f kB", double(size) / 1024.);
}

static wxString backup_label(const CB::BackupInfo &info)
{
    return from_u8(info.created + " (" + info.version + ")");
}

bool ConfigBackupDialog::backup_now(wxWindow *parent)
{
    GUI_App &app = wxGetApp();
    if (! app.check_and_save_current_preset_changes(_L("Configuration backup"),
            _L("Some presets are modified and the unsaved changes will not be included in the backup."), false, true))
        return false;

    wxTextEntryDialog dlg(parent, _L("Optional comment for this backup:"), _L("Back Up Configuration"));
    app.UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK)
        return false;

    try {
        if (app.app_config->dirty())
            app.app_config->save();
        CB::CreateResult res = CB::create_backup(*app.app_config, CB::Reason::Manual, into_u8(dlg.GetValue()), false);
        MessageDialog(parent, _L("Configuration backup saved:") + "\n\n" + from_u8(res.path), _L("Back Up Configuration"), wxOK | wxICON_INFORMATION).ShowModal();
        return true;
    } catch (const std::exception &ex) {
        show_error(parent, _L("Backing up the configuration failed.") + "\n\n" + from_u8(ex.what()));
        return false;
    }
}

ConfigBackupDialog::ConfigBackupDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Configuration Backups"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, "config_backup_dialog", wxGetApp().normal_font().GetPointSize())
{
    this->SetFont(wxGetApp().normal_font());
    wxGetApp().UpdateDarkUI(this);
    const int  em     = em_unit();
    AppConfig &config = *wxGetApp().app_config;

    auto *top = new wxBoxSizer(wxVERTICAL);

    // Backup folder
    auto *dir_sizer = new wxBoxSizer(wxHORIZONTAL);
    dir_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Backup folder") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_dir_text = new wxStaticText(this, wxID_ANY, from_u8(CB::backup_dir(config)), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    dir_sizer->Add(m_dir_text, 1, wxALIGN_CENTER_VERTICAL);
    auto *btn_dir = new wxButton(this, wxID_ANY, _L("Change") + dots);
    btn_dir->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_change_dir(); });
    dir_sizer->Add(btn_dir, 0, wxLEFT, em / 2);
    auto *btn_open = new wxButton(this, wxID_ANY, _L("Open folder"));
    btn_open->Bind(wxEVT_BUTTON, [](wxCommandEvent &) {
        const std::string dir = CB::backup_dir(*wxGetApp().app_config);
        boost::system::error_code ec;
        boost::filesystem::create_directories(dir, ec);
        desktop_open_folder(boost::filesystem::path(dir));
    });
    dir_sizer->Add(btn_open, 0, wxLEFT, em / 2);
    top->Add(dir_sizer, 0, wxEXPAND | wxALL, em);

    // Automatic backups
    auto *auto_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_on_preset_save = new wxCheckBox(this, wxID_ANY, _L("Back up automatically after saving presets"));
    m_on_preset_save->SetValue(config.get(CB::KEY_ON_PRESET_SAVE) != "0");
    m_on_preset_save->SetToolTip(_L("Backups are always taken before PrusaSlicer updates and configuration updates. "
                                    "Identical configurations are not backed up twice."));
    m_on_preset_save->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        wxGetApp().app_config->set(CB::KEY_ON_PRESET_SAVE, m_on_preset_save->GetValue() ? "1" : "0");
    });
    auto_sizer->Add(m_on_preset_save, 0, wxALIGN_CENTER_VERTICAL);
    auto_sizer->AddStretchSpacer();
    auto_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Keep automatic backups (0 = all)") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    m_keep = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(8 * em, -1), wxSP_ARROW_KEYS, 0, 10000, CB::keep_count(config));
    m_keep->SetToolTip(_L("Only backups taken after saving presets and before configuration updates are deleted automatically. "
                          "Manual backups and backups taken before PrusaSlicer updates or restores are kept."));
    m_keep->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) {
        wxGetApp().app_config->set(CB::KEY_KEEP, std::to_string(m_keep->GetValue()));
    });
    auto_sizer->Add(m_keep, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(auto_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    // List
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(90 * em, 22 * em), wxLC_REPORT);
    m_list->AppendColumn(_L("Date"),    wxLIST_FORMAT_LEFT, 17 * em);
    m_list->AppendColumn(_L("Version"), wxLIST_FORMAT_LEFT, 16 * em);
    m_list->AppendColumn(_L("Reason"),  wxLIST_FORMAT_LEFT, 18 * em);
    m_list->AppendColumn(_L("Comment"), wxLIST_FORMAT_LEFT, 30 * em);
    m_list->AppendColumn(_L("Size"),    wxLIST_FORMAT_RIGHT, 8 * em);
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED,   [this](wxListEvent &) { update_buttons(); update_changes(); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent &) { update_buttons(); update_changes(); });
    top->Add(m_list, 3, wxEXPAND | wxLEFT | wxRIGHT, em);

    top->Add(new wxStaticText(this, wxID_ANY,
                 _L("Select one backup to compare it with the current configuration, or two backups to compare them with each other.")),
             0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // Changes
    m_changes = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 14 * em), wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    top->Add(m_changes, 2, wxEXPAND | wxALL, em);

    // Buttons
    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    auto *btn_backup = new wxButton(this, wxID_ANY, _L("Back up now") + dots);
    btn_backup->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (backup_now(this)) reload_list(); });
    btns->Add(btn_backup, 0, wxRIGHT, em / 2);
    m_btn_compare = new wxButton(this, wxID_ANY, _L("Compare presets") + dots);
    m_btn_compare->SetToolTip(_L("Compare the presets option by option"));
    m_btn_compare->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_compare(); });
    btns->Add(m_btn_compare, 0, wxRIGHT, em / 2);
    m_btn_restore = new wxButton(this, wxID_ANY, _L("Restore") + dots);
    m_btn_restore->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_restore(); });
    btns->Add(m_btn_restore, 0, wxRIGHT, em / 2);
    m_btn_delete = new wxButton(this, wxID_ANY, _L("Delete"));
    m_btn_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete(); });
    btns->Add(m_btn_delete, 0, wxRIGHT, em / 2);
    btns->AddStretchSpacer();
    auto *btn_close = new wxButton(this, wxID_CLOSE, _L("Close"));
    btn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    btns->Add(btn_close, 0);
    top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);
    SetEscapeId(wxID_CLOSE);

    SetSizer(top);
    top->SetSizeHints(this);
    reload_list();
    CenterOnParent();
}

void ConfigBackupDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

std::vector<long> ConfigBackupDialog::selected_rows() const
{
    std::vector<long> rows;
    for (long item = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); item != -1;
         item = m_list->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED))
        if (item >= 0 && size_t(item) < m_backups.size())
            rows.push_back(item);
    return rows;
}

void ConfigBackupDialog::reload_list()
{
    wxBusyCursor wait;
    m_dir_text->SetLabel(from_u8(CB::backup_dir(*wxGetApp().app_config)));
    m_backups = CB::list_backups(CB::backup_dir(*wxGetApp().app_config));
    m_list->DeleteAllItems();
    for (size_t i = 0; i < m_backups.size(); ++ i) {
        const CB::BackupInfo &b = m_backups[i];
        long row = m_list->InsertItem(long(i), from_u8(b.created));
        m_list->SetItem(row, 1, from_u8(b.version));
        m_list->SetItem(row, 2, reason_label(b.reason));
        m_list->SetItem(row, 3, from_u8(b.comment));
        m_list->SetItem(row, 4, size_label(b.size));
    }
    update_buttons();
    update_changes();
}

void ConfigBackupDialog::update_buttons()
{
    const size_t n = selected_rows().size();
    m_btn_compare->Enable(n == 1 || n == 2);
    m_btn_restore->Enable(n == 1);
    m_btn_delete->Enable(n >= 1);
}

void ConfigBackupDialog::update_changes()
{
    const std::vector<long> rows = selected_rows();
    if (rows.empty() || rows.size() > 2) {
        m_changes->SetValue(m_backups.empty() ? _L("No backups yet.") : wxString());
        return;
    }
    wxBusyCursor wait;
    try {
        // The list is sorted newest first: rows[1] is the older one.
        const CB::BackupInfo &first = m_backups[rows.back()];
        CB::FileMap from = CB::read_backup(first.path);
        CB::FileMap to;
        wxString    header;
        if (rows.size() == 1) {
            to     = CB::read_current();
            header = GUI::format_wxstr(_L("Changes from backup %1% to the current configuration:"), backup_label(first));
        } else {
            const CB::BackupInfo &second = m_backups[rows.front()];
            to     = CB::read_backup(second.path);
            header = GUI::format_wxstr(_L("Changes from backup %1% to backup %2%:"), backup_label(first), backup_label(second));
        }
        const std::vector<CB::FileChange> changes = CB::compare(from, to);
        wxString text = header + "\n";
        if (changes.empty())
            text += "\n" + _L("No changes in presets, physical printers and vendor profiles.");
        std::string last_dir;
        for (const CB::FileChange &c : changes) {
            const size_t      slash = c.path.find('/');
            const std::string dir   = c.path.substr(0, slash);
            std::string       name  = c.path.substr(slash + 1);
            if (name.size() > 4)
                name.erase(name.size() - 4);
            if (dir != last_dir) {
                text += "\n" + category_label(dir) + ":\n";
                last_dir = dir;
            }
            text += (c.kind == CB::FileChange::Added ? "  + " : c.kind == CB::FileChange::Removed ? "  - " : "  ~ ") + from_u8(name) +
                    (c.kind == CB::FileChange::Added ? " (" + _L("added") + ")" : c.kind == CB::FileChange::Removed ? " (" + _L("removed") + ")" : " (" + _L("changed") + ")") + "\n";
        }
        if (! changes.empty())
            text += "\n" + _L("Use \"Compare presets\" to see the changed options.");
        m_changes->SetValue(text);
    } catch (const std::exception &ex) {
        m_changes->SetValue(_L("Cannot read the backup") + ": " + from_u8(ex.what()));
    }
}

void ConfigBackupDialog::on_change_dir()
{
    AppConfig  &config = *wxGetApp().app_config;
    wxDirDialog dlg(this, _L("Select the folder for configuration backups"), from_u8(CB::backup_dir(config)), wxDD_DEFAULT_STYLE);
    if (dlg.ShowModal() != wxID_OK)
        return;
    std::string dir = into_u8(dlg.GetPath());
    // Backups inside the data directory would be lost together with it.
    const boost::filesystem::path data = boost::filesystem::weakly_canonical(boost::filesystem::path(data_dir()));
    const boost::filesystem::path sel  = boost::filesystem::weakly_canonical(boost::filesystem::path(dir));
    if (boost::starts_with(sel.generic_string() + "/", data.generic_string() + "/")) {
        show_error(this, _L("Please select a folder outside of the configuration folder") + ":\n" + from_u8(data_dir()));
        return;
    }
    config.set(CB::KEY_DIR, dir == CB::default_backup_dir() ? std::string() : dir);
    reload_list();
}

void ConfigBackupDialog::on_compare()
{
    const std::vector<long> rows = selected_rows();
    if (rows.empty() || rows.size() > 2)
        return;
    const CB::BackupInfo &first = m_backups[rows.back()];
    try {
        PresetBundle left;
        PresetBundle right;
        wxString     right_label;
        {
            wxBusyCursor wait;
            CB::load_backup_into_bundle(first.path, left);
            if (rows.size() == 1) {
                right       = *wxGetApp().preset_bundle;
                right_label = _L("Current configuration");
            } else {
                const CB::BackupInfo &second = m_backups[rows.front()];
                CB::load_backup_into_bundle(second.path, right);
                right_label = _L("Backup") + " " + backup_label(second);
            }
        }
        const wxString left_label = _L("Backup") + " " + backup_label(first);
        DiffPresetDialog dlg(this, std::move(left), std::move(right), _L("Compare Presets") + ": " + left_label + " / " + right_label,
                             left_label, right_label);
        dlg.show_modal();
    } catch (const std::exception &ex) {
        show_error(this, _L("Cannot load the backup") + ":\n" + from_u8(ex.what()));
    }
}

void ConfigBackupDialog::on_restore()
{
    const std::vector<long> rows = selected_rows();
    if (rows.size() != 1)
        return;
    const CB::BackupInfo info = m_backups[rows.front()];
    GUI_App &app = wxGetApp();

    if (! app.check_and_save_current_preset_changes(_L("Restoring a configuration backup"), "", false))
        return;

    RichMessageDialog confirm(this,
        GUI::format_wxstr(_L("Restore the configuration backup %1%?"), backup_label(info)) + "\n\n" +
        _L("All print, filament, SLA and printer presets, physical printers and vendor profiles are replaced by the ones of the backup, "
           "the preset selection and installed printers are restored. A backup of the current configuration is taken first.") + "\n\n" +
        _L("API keys and passwords are not stored in backups; the current values of physical printers with the same name are kept."),
        _L("Restore configuration backup"), wxYES_NO | wxICON_QUESTION);
    confirm.ShowCheckBox(_L("Also restore all application preferences"), false);
    confirm.SetYesNoLabels(_L("Restore"), _L("Cancel"));
    if (confirm.ShowModal() != wxID_YES)
        return;
    const bool restore_preferences = confirm.IsCheckBoxChecked();

    CB::suppress_preset_save_trigger(true);
    try {
        try {
            if (app.app_config->dirty())
                app.app_config->save();
            CB::create_backup(*app.app_config, CB::Reason::BeforeRestore, "before restoring " + info.file_name, false);
        } catch (const std::exception &ex) {
            RichMessageDialog dlg(this, _L("Backing up the current configuration failed.") + "\n\n" + from_u8(ex.what()) + "\n\n" +
                                  _L("Restore anyway?"), _L("PrusaSlicer error"), wxYES_NO);
            dlg.SetYesNoLabels(_L("Restore"), _L("Cancel"));
            if (dlg.ShowModal() != wxID_YES) {
                CB::suppress_preset_save_trigger(false);
                return;
            }
        }
        {
            wxBusyCursor wait;
            CB::restore_backup(info.path, *app.app_config, restore_preferences);
            app.app_config->save();
            if (PresetsConfigSubstitutions subs = app.preset_bundle->load_presets(*app.app_config, ForwardCompatibilitySubstitutionRule::Enable);
                ! subs.empty())
                show_substitutions_info(subs);
            app.load_current_presets();
            if (app.plater())
                app.plater()->set_bed_shape();
        }
        CB::suppress_preset_save_trigger(false);
        MessageDialog(this, _L("The configuration backup was restored.") +
                      (restore_preferences ? "\n\n" + _L("Some preferences take effect after restarting PrusaSlicer.") : wxString()),
                      _L("Restore configuration backup"), wxOK | wxICON_INFORMATION).ShowModal();
    } catch (const std::exception &ex) {
        CB::suppress_preset_save_trigger(false);
        show_error(this, _L("Restoring the configuration backup failed.") + "\n\n" + from_u8(ex.what()) + "\n\n" +
                   _L("The configuration before the restore is available as backup \"Before restore\"."));
    }
    reload_list();
}

void ConfigBackupDialog::on_delete()
{
    const std::vector<long> rows = selected_rows();
    if (rows.empty())
        return;
    MessageDialog dlg(this, GUI::format_wxstr(_L("Delete %1% selected backup(s)? This cannot be undone."), rows.size()),
                      _L("Delete configuration backups"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return;
    for (long row : rows) {
        boost::system::error_code ec;
        boost::filesystem::remove(m_backups[row].path, ec);
        if (ec)
            show_error(this, _L("Cannot delete") + " " + from_u8(m_backups[row].path) + ": " + from_u8(ec.message()));
    }
    reload_list();
}

} // namespace GUI
} // namespace Slic3r
