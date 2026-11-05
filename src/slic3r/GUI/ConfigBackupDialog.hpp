#ifndef slic3r_GUI_ConfigBackupDialog_hpp_
#define slic3r_GUI_ConfigBackupDialog_hpp_

// Fork feature "config-backup": list, compare and restore configuration backups.

#include <vector>

#include "GUI_Utils.hpp"
#include "slic3r/Config/ConfigBackup.hpp"

class wxListCtrl;
class wxTextCtrl;
class wxStaticText;
class wxCheckBox;
class wxSpinCtrl;
class wxButton;

namespace Slic3r {
namespace GUI {

class ConfigBackupDialog : public DPIDialog
{
public:
    explicit ConfigBackupDialog(wxWindow *parent);

    // Configuration → "Back Up Configuration…": ask for a comment and create a manual backup.
    static bool backup_now(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void reload_list();
    void update_buttons();
    void update_changes();
    std::vector<long> selected_rows() const;

    void on_change_dir();
    void on_compare();
    void on_restore();
    void on_delete();

    std::vector<ConfigBackup::BackupInfo> m_backups;

    wxStaticText *m_dir_text         { nullptr };
    wxCheckBox   *m_on_preset_save   { nullptr };
    wxSpinCtrl   *m_keep             { nullptr };
    wxListCtrl   *m_list             { nullptr };
    wxTextCtrl   *m_changes          { nullptr };
    wxButton     *m_btn_compare      { nullptr };
    wxButton     *m_btn_restore      { nullptr };
    wxButton     *m_btn_delete       { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ConfigBackupDialog_hpp_
