// Fork feature "config-backup", see ConfigBackup.hpp and doc/fork/features/config-backup/README.md.

#include "ConfigBackup.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <set>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ForkInfo.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/miniz_extension.hpp"

#include "../GUI/GUI.hpp"
#include "../GUI/GUI_App.hpp"
#include "../GUI/I18N.hpp"
#include "../GUI/MainFrame.hpp"
#include "../GUI/MsgDialog.hpp"
#include "../GUI/NotificationManager.hpp"
#include "../GUI/Plater.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {
namespace ConfigBackup {

const char *const KEY_DIR            = "config_backup_dir";
const char *const KEY_KEEP           = "config_backup_keep";
const char *const KEY_ON_PRESET_SAVE = "config_backup_on_preset_save";
const char *const KEY_LAST_VERSION   = "config_backup_last_version";
const char *const APP_CONFIG_FILE    = SLIC3R_APP_KEY ".ini";
const char *const REDACTED           = "<redacted>";

static const char *const MANIFEST_FILE = "backup.ini";
static const char *const FILE_PREFIX   = SLIC3R_APP_KEY "-config_";
static const int         DEFAULT_KEEP  = 100;
static const char *const SUBDIRS[]     = { "print", "sla_print", "filament", "sla_material", "printer", "physical_printer", "vendor" };
static const char *const SECRET_KEYS[] = { "printhost_apikey", "printhost_password" };

std::string reason_id(Reason reason)
{
    switch (reason) {
    case Reason::Manual:             return "manual";
    case Reason::PresetSave:         return "preset-save";
    case Reason::BeforeUpdate:       return "before-update";
    case Reason::BeforeConfigUpdate: return "before-config-update";
    case Reason::BeforeRestore:      return "before-restore";
    }
    return "unknown";
}

static bool is_pruned_reason(const std::string &reason)
{
    return reason == reason_id(Reason::PresetSave) || reason == reason_id(Reason::BeforeConfigUpdate);
}

std::string version_id()
{
    std::string id = fork_info().id;
    if (boost::starts_with(id, "v"))
        id.erase(0, 1);
    return id.empty() ? std::string(SLIC3R_VERSION) : id;
}

std::string default_backup_dir()
{
    fs::path dir = fs::path(data_dir());
    // data_dir() may end with a separator.
    while (! dir.empty() && (dir.filename() == "." || dir.filename().empty()))
        dir = dir.parent_path();
    return (dir.parent_path() / (dir.filename().string() + "-backups")).make_preferred().string();
}

std::string backup_dir(const AppConfig &app_config)
{
    std::string dir = app_config.get(KEY_DIR);
    return dir.empty() ? default_backup_dir() : dir;
}

int keep_count(const AppConfig &app_config)
{
    const std::string value = app_config.get(KEY_KEEP);
    if (value.empty())
        return DEFAULT_KEEP;
    try {
        return std::max(0, std::stoi(value));
    } catch (...) {
        return DEFAULT_KEEP;
    }
}

// ---- helpers ----

static std::string read_file(const fs::path &path)
{
    boost::nowide::ifstream ifs(path.string(), std::ios::binary);
    if (! ifs)
        throw Slic3r::RuntimeError("Cannot read " + path.string());
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static void write_file(const fs::path &path, const std::string &content)
{
    boost::nowide::ofstream ofs(path.string(), std::ios::binary | std::ios::trunc);
    if (! ofs)
        throw Slic3r::RuntimeError("Cannot write " + path.string());
    ofs.write(content.data(), std::streamsize(content.size()));
    ofs.close();
    if (! ofs)
        throw Slic3r::RuntimeError("Cannot write " + path.string());
}

static std::string now_string(const char *format)
{
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

static std::string sanitize_for_file_name(std::string s)
{
    for (char &c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ' ' || c == '_')
            c = '-';
    return s.empty() ? std::string("unknown") : s;
}

static bool is_secret_file(const std::string &rel_path)
{
    return boost::starts_with(rel_path, "printer/") || boost::starts_with(rel_path, "physical_printer/");
}

static bool is_secret_key(const std::string &key)
{
    for (const char *k : SECRET_KEYS)
        if (key == k)
            return true;
    return false;
}

// Split an ini line "key = value" (line without '\n'). Returns false for other lines.
static bool split_ini_line(const std::string &line, std::string &key, std::string &value)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
        return false;
    key   = boost::algorithm::trim_copy(line.substr(0, eq));
    value = boost::algorithm::trim_copy(line.substr(eq + 1));
    return true;
}

// Apply fn(key, value, line_without_cr) -> replacement line (without '\n' / '\r') for every secret key line.
template<typename Fn> static std::string process_secret_lines(const std::string &content, Fn fn)
{
    std::string out;
    out.reserve(content.size());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t      end  = content.find('\n', pos);
        bool        nl   = end != std::string::npos;
        std::string line = content.substr(pos, (nl ? end : content.size()) - pos);
        pos              = nl ? end + 1 : content.size();
        bool cr = ! line.empty() && line.back() == '\r';
        if (cr)
            line.pop_back();
        std::string key, value;
        if (split_ini_line(line, key, value) && is_secret_key(key))
            line = fn(key, value, line);
        out += line;
        if (cr)
            out += '\r';
        if (nl)
            out += '\n';
    }
    return out;
}

static std::string redact(const std::string &content, int &count)
{
    return process_secret_lines(content, [&count](const std::string &key, const std::string &value, const std::string &line) {
        // "stored" refers to the system keyring and is no secret.
        if (value.empty() || value == "stored" || value == REDACTED)
            return line;
        ++ count;
        return key + " = " + REDACTED;
    });
}

static std::map<std::string, std::string> secret_values(const std::string &content)
{
    std::map<std::string, std::string> values;
    process_secret_lines(content, [&values](const std::string &key, const std::string &value, const std::string &line) {
        values[key] = value;
        return line;
    });
    return values;
}

// Put back secrets of the current file (if any) for redacted values.
static std::string unredact(const std::string &content, const std::map<std::string, std::string> &current)
{
    return process_secret_lines(content, [&current](const std::string &key, const std::string &value, const std::string &line) {
        if (value != REDACTED)
            return line;
        auto it = current.find(key);
        return key + " = " + ((it == current.end() || it->second == REDACTED) ? std::string() : it->second);
    });
}

// Allowed entries of a backup: manifest, app config, <subdir>/<name>.ini.
static bool is_valid_entry(const std::string &name)
{
    if (name == MANIFEST_FILE || name == APP_CONFIG_FILE)
        return true;
    size_t slash = name.find('/');
    if (slash == std::string::npos || name.find('/', slash + 1) != std::string::npos || name.find('\\') != std::string::npos)
        return false;
    const std::string subdir = name.substr(0, slash);
    const std::string file   = name.substr(slash + 1);
    if (file.empty() || file == "." || file == ".." || boost::starts_with(file, ".") || ! boost::iends_with(file, ".ini"))
        return false;
    for (const char *s : SUBDIRS)
        if (subdir == s)
            return true;
    return false;
}

// Configuration files of the data directory.
static FileMap read_data_dir(bool do_redact, int *redacted)
{
    FileMap  files;
    int      count = 0;
    fs::path root(data_dir());
    for (const char *subdir : SUBDIRS) {
        fs::path dir = root / subdir;
        if (! fs::is_directory(dir))
            continue;
        for (const fs::directory_entry &entry : fs::directory_iterator(dir))
            if (Slic3r::is_ini_file(entry)) {
                std::string rel     = std::string(subdir) + "/" + entry.path().filename().string();
                std::string content = read_file(entry.path());
                if (do_redact && is_secret_file(rel))
                    content = redact(content, count);
                files.emplace(std::move(rel), std::move(content));
            }
    }
    fs::path app_config = root / APP_CONFIG_FILE;
    if (fs::is_regular_file(app_config))
        files.emplace(APP_CONFIG_FILE, read_file(app_config));
    if (redacted)
        *redacted = count;
    return files;
}

// Sections of PrusaSlicer.ini that belong to the configuration (installed printers / materials,
// preset selection). The rest (window positions, recent files, ...) changes all the time.
static std::string stable_app_config_part(const std::string &content)
{
    std::istringstream ss(content);
    std::string        line, out;
    bool               take = false;
    while (std::getline(ss, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (! line.empty() && line.front() == '[') {
            const std::string section = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            take = section == "presets" || section == "filaments" || section == "sla_materials" || boost::starts_with(section, "vendor:");
        }
        if (take)
            out += line + "\n";
    }
    return out;
}

// Stable FNV-1a hash over the preset / vendor files and the configuration part of PrusaSlicer.ini.
static std::string content_hash(const FileMap &files)
{
    uint64_t h   = 1469598103934665603ULL;
    auto     add = [&h](const std::string &s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        h ^= 0xff;
        h *= 1099511628211ULL;
    };
    for (const auto &[path, content] : files)
        if (path == APP_CONFIG_FILE) {
            add(path);
            add(stable_app_config_part(content));
        } else if (path != MANIFEST_FILE) {
            add(path);
            add(content);
        }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

static std::string one_line(std::string s)
{
    for (char &c : s)
        if (c == '\n' || c == '\r')
            c = ' ';
    return boost::algorithm::trim_copy(s);
}

static std::string make_manifest(const BackupInfo &info)
{
    std::ostringstream ss;
    ss << "# " SLIC3R_APP_KEY " configuration backup\n"
       << "format = 1\n"
       << "created = " << info.created << "\n"
       << "version = " << info.version << "\n"
       << "app_version = " << info.app_version << "\n"
       << "reason = " << info.reason << "\n"
       << "comment = " << one_line(info.comment) << "\n"
       << "content_hash = " << info.content_hash << "\n"
       << "data_dir = " << info.data_dir << "\n"
       << "redacted = " << info.redacted << "\n";
    return ss.str();
}

static bool parse_manifest(const std::string &content, BackupInfo &info)
{
    std::istringstream ss(content);
    std::string        line, key, value;
    bool               valid = false;
    while (std::getline(ss, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (! split_ini_line(line, key, value))
            continue;
        if (key == "format") valid = true;
        else if (key == "created") info.created = value;
        else if (key == "version") info.version = value;
        else if (key == "app_version") info.app_version = value;
        else if (key == "reason") info.reason = value;
        else if (key == "comment") info.comment = value;
        else if (key == "content_hash") info.content_hash = value;
        else if (key == "data_dir") info.data_dir = value;
        else if (key == "redacted") info.redacted = std::atoi(value.c_str());
    }
    return valid;
}

namespace {
struct ZipReader : public MZ_Archive
{
    explicit ZipReader(const std::string &path)
    {
        if (! open_zip_reader(&arch, path))
            throw Slic3r::FileIOError("Cannot open " + path + ": " + get_errorstr());
    }
    ~ZipReader() { close_zip_reader(&arch); }

    std::string extract(mz_uint index, size_t size)
    {
        std::string buf(size, '\0');
        if (size > 0 && ! mz_zip_reader_extract_to_mem(&arch, index, buf.data(), buf.size(), 0))
            throw Slic3r::FileIOError("Cannot extract from backup: " + get_errorstr());
        return buf;
    }
};
} // namespace

static bool read_manifest(const std::string &zip_path, BackupInfo &info)
{
    try {
        ZipReader zip(zip_path);
        int       idx = mz_zip_reader_locate_file(&zip.arch, MANIFEST_FILE, nullptr, 0);
        if (idx < 0)
            return false;
        mz_zip_archive_file_stat stat;
        if (! mz_zip_reader_file_stat(&zip.arch, mz_uint(idx), &stat))
            return false;
        if (! parse_manifest(zip.extract(mz_uint(idx), size_t(stat.m_uncomp_size)), info))
            return false;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "Config backup: ignoring " << zip_path << ": " << ex.what();
        return false;
    }
    info.path      = zip_path;
    info.file_name = fs::path(zip_path).filename().string();
    boost::system::error_code ec;
    info.size = fs::file_size(zip_path, ec);
    return true;
}

// Backup zip files of a directory, newest first (modification time, then file name).
static std::vector<fs::path> backup_files(const std::string &dir)
{
    std::vector<fs::path>     files;
    boost::system::error_code ec;
    if (! fs::is_directory(dir, ec))
        return files;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (boost::starts_with(name, FILE_PREFIX) && boost::iends_with(name, ".zip") && fs::is_regular_file(entry.path(), ec))
            files.push_back(entry.path());
    }
    std::vector<std::pair<std::time_t, fs::path>> sorted;
    for (const fs::path &p : files)
        sorted.emplace_back(fs::last_write_time(p, ec), p);
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second.filename().string() > b.second.filename().string();
    });
    files.clear();
    for (auto &[t, p] : sorted)
        files.push_back(std::move(p));
    return files;
}

// ---- public API ----

FileMap read_current()
{
    return read_data_dir(true, nullptr);
}

FileMap read_backup(const std::string &zip_path, BackupInfo *info)
{
    FileMap   files;
    ZipReader zip(zip_path);
    mz_uint   n = mz_zip_reader_get_num_files(&zip.arch);
    for (mz_uint i = 0; i < n; ++ i) {
        mz_zip_archive_file_stat stat;
        if (! mz_zip_reader_file_stat(&zip.arch, i, &stat) || mz_zip_reader_is_file_a_directory(&zip.arch, i))
            continue;
        const std::string name = stat.m_filename;
        if (! is_valid_entry(name)) {
            BOOST_LOG_TRIVIAL(warning) << "Config backup: ignoring unexpected entry " << name << " in " << zip_path;
            continue;
        }
        files[name] = zip.extract(i, size_t(stat.m_uncomp_size));
    }
    auto it = files.find(MANIFEST_FILE);
    if (it == files.end())
        throw Slic3r::RuntimeError(zip_path + " is not a " SLIC3R_APP_KEY " configuration backup (no " + MANIFEST_FILE + ").");
    if (info) {
        parse_manifest(it->second, *info);
        info->path      = zip_path;
        info->file_name = fs::path(zip_path).filename().string();
    }
    files.erase(it);
    return files;
}

std::vector<BackupInfo> list_backups(const std::string &dir)
{
    std::vector<BackupInfo> out;
    for (const fs::path &path : backup_files(dir)) {
        BackupInfo info;
        if (read_manifest(path.string(), info))
            out.push_back(std::move(info));
    }
    return out;
}

std::vector<FileChange> compare(const FileMap &from, const FileMap &to)
{
    std::vector<FileChange> changes;
    for (const auto &[path, content] : to) {
        if (path == APP_CONFIG_FILE)
            continue;
        auto it = from.find(path);
        if (it == from.end())
            changes.push_back({ path, FileChange::Added });
        else if (it->second != content)
            changes.push_back({ path, FileChange::Changed });
    }
    for (const auto &[path, content] : from)
        if (path != APP_CONFIG_FILE && to.find(path) == to.end())
            changes.push_back({ path, FileChange::Removed });
    std::sort(changes.begin(), changes.end(), [](const FileChange &a, const FileChange &b) { return a.path < b.path; });
    return changes;
}

CreateResult create_backup(const AppConfig &app_config, Reason reason, const std::string &comment, bool skip_if_identical,
                           const std::string &version)
{
    CreateResult result;
    const std::string dir = backup_dir(app_config);

    BackupInfo info;
    FileMap    files     = read_data_dir(true, &info.redacted);
    info.content_hash    = content_hash(files);

    if (skip_if_identical) {
        // Compare with the newest readable backup.
        for (const fs::path &path : backup_files(dir)) {
            BackupInfo newest;
            if (read_manifest(path.string(), newest)) {
                if (newest.content_hash == info.content_hash) {
                    result.path = newest.path;
                    return result;
                }
                break;
            }
        }
    }

    info.created     = now_string("%Y-%m-%d %H:%M:%S");
    info.version     = version.empty() ? version_id() : version;
    info.app_version = version_id();
    info.reason      = reason_id(reason);
    info.comment     = comment;
    info.data_dir    = data_dir();

    fs::create_directories(dir);
    const std::string stem = std::string(FILE_PREFIX) + now_string("%Y-%m-%d_%H%M%S") + "_" + sanitize_for_file_name(info.version) + "_" + info.reason;
    fs::path path = fs::path(dir) / (stem + ".zip");
    for (int i = 2; fs::exists(path); ++ i)
        path = fs::path(dir) / (stem + "-" + std::to_string(i) + ".zip");
    const fs::path tmp_path = path.string() + ".tmp";

    try {
        {
            Zipper zipper(tmp_path.string(), Zipper::FAST_COMPRESSION);
            const std::string manifest = make_manifest(info);
            zipper.add_entry(MANIFEST_FILE, manifest.data(), manifest.size());
            for (const auto &[name, content] : files)
                zipper.add_entry(name, content.data(), content.size());
            zipper.finalize();
        } // closes the file
        // Read the zip back: a full disk may truncate it without any error while writing.
        BackupInfo written_info;
        if (read_backup(tmp_path.string(), &written_info) != files || written_info.content_hash != info.content_hash)
            throw Slic3r::RuntimeError("Verification of the written backup failed: " + tmp_path.string());
        fs::rename(tmp_path, path);
    } catch (...) {
        boost::system::error_code ec;
        fs::remove(tmp_path, ec);
        throw;
    }

    BOOST_LOG_TRIVIAL(info) << "Config backup created: " << path.string() << " (" << info.reason << ")";
    result.created = true;
    result.path    = path.string();

    try {
        // Only automatic backups prune; never while a manual / restore backup is taken.
        if (is_pruned_reason(info.reason))
            prune_backups(app_config);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Config backup: pruning failed: " << ex.what();
    }
    return result;
}

void prune_backups(const AppConfig &app_config)
{
    const int keep = keep_count(app_config);
    if (keep <= 0)
        return;
    int kept = 0;
    for (const BackupInfo &info : list_backups(backup_dir(app_config)))
        if (is_pruned_reason(info.reason) && ++ kept > keep) {
            BOOST_LOG_TRIVIAL(info) << "Config backup: removing old automatic backup " << info.path;
            fs::remove(info.path);
        }
}

namespace {
// Temporary directory with the files of a backup, removed on destruction.
struct ExtractedBackup
{
    fs::path dir;
    explicit ExtractedBackup(const FileMap &files)
    {
        dir = fs::temp_directory_path() / fs::unique_path(SLIC3R_APP_KEY "-backup-%%%%-%%%%-%%%%");
        fs::create_directories(dir);
        for (const char *subdir : SUBDIRS)
            fs::create_directories(dir / subdir);
        for (const auto &[name, content] : files)
            write_file(dir / fs::path(name), content);
    }
    ~ExtractedBackup()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};
} // namespace

void load_backup_into_bundle(const std::string &zip_path, PresetBundle &bundle)
{
    ExtractedBackup extracted(read_backup(zip_path));
    AppConfig       config(AppConfig::EAppMode::Editor);
    const fs::path  app_config_path = extracted.dir / APP_CONFIG_FILE;
    if (fs::exists(app_config_path))
        if (std::string err = config.load(app_config_path.string()); ! err.empty())
            BOOST_LOG_TRIVIAL(warning) << "Config backup: cannot read " << APP_CONFIG_FILE << " of " << zip_path << ": " << err;
    bundle.load_presets(config, ForwardCompatibilitySubstitutionRule::EnableSilent, PresetBundle::PresetPreferences(), extracted.dir.string());
}

// Secrets of printer / physical printer files seen during restores, so that a restore which
// removes a printer and a later restore of the "before restore" backup (redacted) do not lose
// them. Lives in the data directory, never included in backups.
static const char *const SECRETS_FILE = "config_backup_secrets.ini";
using SecretStore = std::map<std::string, std::map<std::string, std::string>>;

static SecretStore load_secret_store()
{
    SecretStore     store;
    const fs::path  path = fs::path(data_dir()) / SECRETS_FILE;
    if (! fs::exists(path))
        return store;
    std::istringstream ss(read_file(path));
    std::string        line, section, key, value;
    while (std::getline(ss, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (! line.empty() && line.front() == '[' && line.back() == ']')
            section = line.substr(1, line.size() - 2);
        else if (! section.empty() && split_ini_line(line, key, value) && is_secret_key(key) && ! value.empty() && value != REDACTED)
            store[section][key] = value;
    }
    return store;
}

static void save_secret_store(const SecretStore &store)
{
    std::string content = "# Secrets of printers for restoring configuration backups, never part of a backup.\n";
    for (const auto &[section, values] : store) {
        content += "[" + section + "]\n";
        for (const auto &[key, value] : values)
            content += key + " = " + value + "\n";
    }
    const fs::path path = fs::path(data_dir()) / SECRETS_FILE;
    write_file(path, content);
    boost::system::error_code ec;
    fs::permissions(path, fs::owner_read | fs::owner_write, ec);
}

// Write back the files read before a failed restore. Returns error messages, empty on success.
static std::string rollback_files(const FileMap &current)
{
    std::string     errors;
    const fs::path  root(data_dir());
    boost::system::error_code ec;
    for (const char *subdir : SUBDIRS)
        for (const fs::directory_entry &entry : fs::directory_iterator(root / subdir, ec))
            if (Slic3r::is_ini_file(entry))
                fs::remove(entry.path(), ec);
    for (const auto &[name, content] : current) {
        if (name == APP_CONFIG_FILE)
            continue;
        try {
            write_file(root / fs::path(name), content);
        } catch (const std::exception &ex) {
            errors += std::string(ex.what()) + "\n";
        }
    }
    return errors;
}

void restore_backup(const std::string &zip_path, AppConfig &app_config, bool restore_preferences)
{
    const FileMap files = read_backup(zip_path);
    size_t        num_presets = 0;
    for (const auto &[name, content] : files)
        if (name != APP_CONFIG_FILE)
            ++ num_presets;
    if (num_presets == 0)
        throw Slic3r::RuntimeError(zip_path + " contains no configuration files.");

    // Secrets of the current files, to replace the redacted values.
    const FileMap current = read_data_dir(false, nullptr);

    // Parse the backup's application config before touching anything.
    std::unique_ptr<AppConfig> backup_config;
    boost::property_tree::ptree backup_tree;
    if (auto it = files.find(APP_CONFIG_FILE); it != files.end()) {
        std::istringstream ss(it->second);
        boost::property_tree::read_ini(ss, backup_tree);
        const fs::path tmp = fs::temp_directory_path() / fs::unique_path(SLIC3R_APP_KEY "-backup-%%%%-%%%%-%%%%.ini");
        write_file(tmp, it->second);
        backup_config = std::make_unique<AppConfig>(AppConfig::EAppMode::Editor);
        std::string err = backup_config->load(tmp.string());
        boost::system::error_code ec;
        fs::remove(tmp, ec);
        if (! err.empty())
            throw Slic3r::RuntimeError(std::string("Cannot read ") + APP_CONFIG_FILE + " of the backup: " + err);
    }

    // Remember the current secrets (they survive even if the restore removes a printer).
    SecretStore secrets = load_secret_store();
    for (const auto &[name, content] : current)
        if (is_secret_file(name))
            for (const auto &[key, value] : secret_values(content))
                if (! value.empty() && value != REDACTED)
                    secrets[name][key] = value;
    save_secret_store(secrets);

    // Replace the preset and vendor files, put the old ones back on any error.
    const fs::path root(data_dir());
    try {
        for (const char *subdir : SUBDIRS) {
            const fs::path dir = root / subdir;
            fs::create_directories(dir);
            for (const fs::directory_entry &entry : fs::directory_iterator(dir))
                if (Slic3r::is_ini_file(entry))
                    fs::remove(entry.path());
        }
        for (const auto &[name, content] : files) {
            if (name == APP_CONFIG_FILE)
                continue;
            std::string data = content;
            if (is_secret_file(name)) {
                // Current file first, then the remembered secrets.
                std::map<std::string, std::string> known;
                if (auto it = secrets.find(name); it != secrets.end())
                    known = it->second;
                if (auto it = current.find(name); it != current.end())
                    for (const auto &[key, value] : secret_values(it->second))
                        if (! value.empty() && value != REDACTED)
                            known[key] = value;
                data = unredact(data, known);
            }
            write_file(root / fs::path(name), data);
        }
    } catch (const std::exception &ex) {
        const std::string rollback_errors = rollback_files(current);
        BOOST_LOG_TRIVIAL(error) << "Config backup: restore of " << zip_path << " failed: " << ex.what() << "; rollback: "
                                 << (rollback_errors.empty() ? "ok" : rollback_errors);
        throw Slic3r::RuntimeError(std::string(ex.what()) + "\n\n" +
            (rollback_errors.empty() ? std::string("The previous configuration files were put back.") :
                                       "Putting back the previous configuration files failed too:\n" + rollback_errors));
    }

    // Application settings.
    if (backup_config) {
        if (restore_preferences) {
            for (const auto &section : backup_tree) {
                if (section.second.empty()) {
                    const std::string &key = section.first;
                    if (key != "version" && key != "on_snapshot" && ! boost::starts_with(key, "config_backup_") && ! section.second.data().empty())
                        app_config.set(key, section.second.data());
                } else if (! boost::starts_with(section.first, "vendor:") && backup_config->has_section(section.first))
                    app_config.set_section(section.first, backup_config->get_section(section.first));
            }
        }
        // Preset selection and installed materials belong to the configuration.
        for (const char *section : { "presets", "filaments", "sla_materials" })
            if (backup_config->has_section(section))
                app_config.set_section(section, backup_config->get_section(section));
        app_config.set_vendors(*backup_config);
    }
    // The configuration is not a snapshot anymore.
    app_config.set("on_snapshot", "");
    BOOST_LOG_TRIVIAL(info) << "Config backup restored: " << zip_path;
}

// ---- GUI triggers ----

static bool ask_continue(const wxString &message, const wxString &continue_label, const wxString &abort_label)
{
    RichMessageDialog dlg(static_cast<wxWindow *>(wxGetApp().mainframe), message, _L("PrusaSlicer error"), wxYES_NO);
    dlg.SetYesNoLabels(continue_label, abort_label);
    return dlg.ShowModal() == wxID_YES;
}

bool backup_on_version_change(AppConfig &app_config)
{
    const std::string current = version_id();
    std::string       last    = app_config.get(KEY_LAST_VERSION);
    if (last.empty() && app_config.orig_version().valid())
        last = app_config.orig_version().to_string();
    if (! last.empty() && last != current) {
        try {
            create_backup(app_config, Reason::BeforeUpdate, "before update to " + current, false, last);
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(error) << "Config backup before update failed: " << ex.what();
            if (! ask_continue(_L("Backing up the configuration before the update failed.") + "\n\n" + from_u8(ex.what()) + "\n\n" +
                                   _L("Backup folder") + ": " + from_u8(backup_dir(app_config)) + "\n\n" +
                                   _L("Continue without a backup? The configuration may be migrated to the new version."),
                               _L("Continue"), _L("Quit")))
                return false;
        }
    }
    app_config.set(KEY_LAST_VERSION, current);
    return true;
}

bool backup_before_config_update(const AppConfig &app_config, const std::string &snapshot_reason, bool ask_on_error)
{
    try {
        create_backup(app_config, Reason::BeforeConfigUpdate, "before configuration snapshot (" + snapshot_reason + ")", true);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Config backup before configuration update failed: " << ex.what();
        const wxString msg = _L("Backing up the configuration failed.") + "\n\n" + from_u8(ex.what());
        if (ask_on_error)
            return ask_continue(msg + "\n\n" + _L("Continue without a backup?"), _L("Continue"), _L("Abort"));
        show_error(static_cast<wxWindow *>(wxGetApp().mainframe), msg);
    }
    return true;
}

static std::atomic<bool> s_preset_save_pending { false };
static bool              s_preset_save_suppressed { false };

void suppress_preset_save_trigger(bool suppress)
{
    s_preset_save_suppressed = suppress;
}

void install_preset_save_trigger()
{
    set_preset_files_changed_callback([]() {
        if (s_preset_save_suppressed || s_preset_save_pending.exchange(true))
            return;
        // Once per GUI action: run after the current event handler finished all its saves.
        wxGetApp().CallAfter([]() {
            s_preset_save_pending = false;
            AppConfig *app_config = wxGetApp().app_config;
            if (s_preset_save_suppressed || app_config == nullptr || app_config->get(KEY_ON_PRESET_SAVE) == "0")
                return;
            try {
                if (app_config->dirty())
                    app_config->save();
                create_backup(*app_config, Reason::PresetSave, std::string(), true);
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(error) << "Config backup after preset save failed: " << ex.what();
                if (Plater *plater = wxGetApp().plater(); plater && plater->get_notification_manager())
                    plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                        NotificationManager::NotificationLevel::WarningNotificationLevel,
                        _u8L("Configuration backup after saving a preset failed") + ": " + ex.what());
            }
        });
    });
}

} // namespace ConfigBackup
} // namespace GUI
} // namespace Slic3r
