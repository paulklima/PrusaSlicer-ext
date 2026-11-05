#ifndef slic3r_GUI_ConfigBackup_hpp_
#define slic3r_GUI_ConfigBackup_hpp_

// Fork feature "config-backup" (doc/fork/features/config-backup/README.md):
// zip backups of the user configuration (presets, physical printers, vendor profiles,
// PrusaSlicer.ini) outside of the data directory, with history, compare and restore.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class AppConfig;
class PresetBundle;

namespace GUI {
namespace ConfigBackup {

enum class Reason
{
    Manual,
    PresetSave,
    BeforeUpdate,       // PrusaSlicer version change, taken before any migration
    BeforeConfigUpdate, // before a configuration snapshot (vendor updates, wizard, rollback)
    BeforeRestore,
};

std::string reason_id(Reason reason);

// AppConfig keys (all optional).
extern const char *const KEY_DIR;             // backup folder, empty = default_backup_dir()
extern const char *const KEY_KEEP;            // number of automatic backups to keep, 0 = all
extern const char *const KEY_ON_PRESET_SAVE;  // "1" (default) / "0"
extern const char *const KEY_LAST_VERSION;    // version_id() of the last run

// Name of the file in a backup / the data directory holding the application settings.
extern const char *const APP_CONFIG_FILE;
// Value replacing secrets inside backups.
extern const char *const REDACTED;

struct BackupInfo
{
    std::string path;          // full path of the zip
    std::string file_name;
    std::string created;       // "2026-11-05 14:30:12"
    std::string version;       // version of the configuration inside
    std::string app_version;   // version of the PrusaSlicer that wrote the backup
    std::string reason;        // reason_id()
    std::string comment;
    std::string content_hash;
    std::string data_dir;      // data directory the backup was taken from
    int         redacted { 0 };
    uintmax_t   size { 0 };
};

// Relative path ("filament/My PLA.ini", "PrusaSlicer.ini") -> file content.
using FileMap = std::map<std::string, std::string>;

struct FileChange
{
    enum Kind { Added, Removed, Changed };
    std::string path;
    Kind        kind;
};

// Version id of this build, e.g. "2.9.6+ext-v1.1.0" or "2.9.6+ext-dev".
std::string version_id();
std::string default_backup_dir();
std::string backup_dir(const AppConfig &app_config);
int         keep_count(const AppConfig &app_config);

// Current configuration files of the data directory, secrets redacted (as they would be backed up).
FileMap     read_current();
// All files of a backup zip. Throws on error.
FileMap     read_backup(const std::string &zip_path, BackupInfo *info = nullptr);
// Newest backup first. Zips without a manifest are ignored.
std::vector<BackupInfo> list_backups(const std::string &dir);
// Changes from `from` to `to`, PrusaSlicer.ini is ignored.
std::vector<FileChange> compare(const FileMap &from, const FileMap &to);

struct CreateResult
{
    bool        created { false };
    std::string path;
};
// Create a backup of the data directory. Throws on error.
// skip_if_identical: don't create it when the presets equal the newest backup.
// version: version of the configuration, empty = version_id().
CreateResult create_backup(const AppConfig &app_config, Reason reason, const std::string &comment,
                           bool skip_if_identical, const std::string &version = std::string());
// Delete automatic backups beyond keep_count().
void         prune_backups(const AppConfig &app_config);

// Write the preset files of a backup into a new temporary directory and load them into `bundle`
// (selections from the backup's PrusaSlicer.ini). Throws on error.
void         load_backup_into_bundle(const std::string &zip_path, PresetBundle &bundle);

// Replace the preset / vendor files of the data directory with those of the backup and restore
// the preset selections and installed vendors in app_config (all settings if restore_preferences).
// Redacted secrets are taken over from the current files. The caller reloads the presets. Throws.
void         restore_backup(const std::string &zip_path, AppConfig &app_config, bool restore_preferences);

// ---- GUI triggers (show errors themselves) ----

// At startup, before anything touches the data directory. Returns false if the user chose to quit.
bool         backup_on_version_change(AppConfig &app_config);
// Before a configuration snapshot is taken. Returns false if the user chose to abort.
bool         backup_before_config_update(const AppConfig &app_config, const std::string &snapshot_reason, bool ask_on_error);
// Install the preset-save notification (debounced backup after preset files changed).
void         install_preset_save_trigger();
// Suppress preset-save backups (while restoring).
void         suppress_preset_save_trigger(bool suppress);

} // namespace ConfigBackup
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ConfigBackup_hpp_
