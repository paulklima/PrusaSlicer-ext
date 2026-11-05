---
id: config-backup
kind: feature
title: Configuration backups (zip history, compare, restore)
show: true
upstream_pr: none
---

# feature/config-backup — configuration backups with history, compare and restore

## What / why

All user configuration (print / filament / printer / SLA presets, physical printers, installed
vendor profiles, `PrusaSlicer.ini`) lives in the data directory. Upstream only offers
*configuration snapshots* (stored **inside** that same directory, no diff) and the single-file
*config bundle* export (no history, no app settings, no vendor profiles). Before moving the fork
to a new upstream base we want a real, external backup with a history that can be compared
and restored.

## Design

### Backup archive

- One **zip per backup** in a backup folder outside the data directory
  (default `<data_dir>-backups`, e.g. `~/.config/PrusaSlicer-backups`; changeable in the backup
  dialog, folders inside the data directory are refused).
- File name contains time, the PrusaSlicer (fork) version the configuration belongs to and the
  reason, e.g. `PrusaSlicer-config_2026-11-05_143012_2.9.6+ext-v1.1.0_manual.zip`.
- Contents: `backup.ini` (manifest: format, version, fork id, reason, comment, created, source
  data dir, content hash, redacted entries), `PrusaSlicer.ini`, and the `*.ini` files of
  `print sla_print filament sla_material printer physical_printer vendor`.
- **Secrets are redacted**: `printhost_apikey` and plain-text `printhost_password` are replaced
  with `<redacted>` (`stored` = keyring reference is kept). On restore the current value of the
  same file is put back, otherwise the value becomes empty.

### Triggers

| Reason | When |
| --- | --- |
| `manual` | Configuration → *Backup Configuration…* (optional comment), or the backup dialog |
| `before-update` | At startup, when the last-run version (`config_backup_last_version`, fallback `version` of `PrusaSlicer.ini`) differs from this build — taken before any migration / import / vendor check touches the data directory. The name carries the **old** version. |
| `before-config-update` | Before every configuration snapshot the app takes itself (vendor profile updates / downgrades, wizard, rollback) |
| `preset-save` | After user preset / physical printer files were saved, renamed or deleted (debounced, once per GUI action) |
| `before-restore` | Automatically before a restore |

Automatic backups are skipped when the preset content is identical to the newest backup
(content hash over the preset/vendor files). Only `preset-save` and `before-config-update`
backups are pruned (keep the newest N, default 100, 0 = all); manual, `before-update` and
`before-restore` backups are never deleted automatically.

Settings (`PrusaSlicer.ini`, edited in the backup dialog): `config_backup_dir`,
`config_backup_keep`, `config_backup_on_preset_save`; `config_backup_last_version` is the
version of the last run.

### Backup dialog (Configuration → *Configuration Backups…*)

Backup folder (change / open), the automatic-backup settings and the list of all backups (date,
version, reason, comment, size) with:

- **Back up now**, **Delete**.
- **Changes** (shown on selection): file-level summary of the selected backup against the current
  configuration (or of two selected backups): added / removed / changed presets per category.
- **Compare presets**: opens a read-only *Compare Presets* dialog with the backup loaded on one
  side and the current configuration (or the second selected backup) on the other — option-level
  diff using the existing upstream dialog.
- **Restore**: asks to save unsaved changes, takes a `before-restore` backup, replaces the preset
  and vendor files, restores preset selections and installed vendors (optionally all application
  preferences) and reloads the presets.

### Code

- `src/slic3r/Config/ConfigBackup.{hpp,cpp}` — zip writing / reading, manifest, redaction,
  pruning, listing, restore, loading a backup into a `PresetBundle`, and the GUI trigger helpers.
- `src/slic3r/GUI/ConfigBackupDialog.{hpp,cpp}` — the dialog and *Back Up Configuration…*.
- Small hooks (marked `Fork (config-backup)` / `fork: config-backup`):
  - `GUI_App.{hpp,cpp}` — menu entries, startup trigger in `on_init_inner()` right before
    `check_older_app_config()`, preset-save trigger installed after `load_current_presets()`.
  - `Snapshot.cpp` — `take_config_snapshot_report_error` / `_cancel_on_error` back up first
    (covers updater, wizard, rollback, older-config import).
  - `libslic3r/Preset.{hpp,cpp}` — `set_preset_files_changed_callback()`, notified from
    `Preset::save`, `PhysicalPrinter::save`, preset / physical-printer deletion; `Tab.cpp` on rename.
  - `libslic3r/PresetBundle.{hpp,cpp}` — optional `presets_dir` for `load_presets()` /
    `load_system_presets()`.
  - `UnsavedChangesDialog.{hpp,cpp}` — `DiffPresetDialog` constructor for two given bundles
    (read-only, own column labels, presets looked up per side).

### Bump notes

Hooks are small and anchored on stable functions; on an upstream bump check that the startup
order in `GUI_App::on_init_inner()` still has no data-directory writes before the hook, and that
new preset write / delete paths in `Preset.cpp` call `notify_preset_files_changed()`.

## How to test

1. Configuration → *Backup Configuration…* → zip appears in the backup folder; open it, check
   that API keys show `<redacted>`.
2. Change and save a filament preset → a `preset-save` backup appears (only one per save);
   saving without change does not create another one.
3. *Configuration Backups…* → select the older backup → *Changes* lists the filament; *Compare
   presets* shows the changed options.
4. *Restore* the older backup → the filament change is gone, a `before-restore` backup exists,
   physical printers still have their API keys.
5. Edit `config_backup_last_version` in `PrusaSlicer.ini` to `0.0.0`, start → a
   `before-update` backup named with `0.0.0` is created before anything else.
