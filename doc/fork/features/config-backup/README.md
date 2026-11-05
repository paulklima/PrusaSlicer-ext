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
  (preference, default `<data_dir>-backups`, e.g. `~/.config/PrusaSlicer-backups`).
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
backups are pruned (keep the newest N, preference, default 100); manual, `before-update` and
`before-restore` backups are never deleted automatically.

### Backup dialog (Configuration → *Configuration Backups…*)

List of all backups (date, version, reason, comment, size) with:

- **Back up now**, **Open folder**, **Delete**.
- **Changes**: file-level summary of the selected backup against the current configuration (or
  of two selected backups): added / removed / changed presets per category.
- **Compare presets**: opens a read-only *Compare Presets* dialog with the backup loaded on one
  side and the current configuration (or the second selected backup) on the other — option-level
  diff using the existing upstream dialog.
- **Restore**: asks to save unsaved changes, takes a `before-restore` backup, replaces the preset
  and vendor files, restores preset selections and installed vendors (optionally all application
  preferences) and reloads the presets.

### Code

- `src/slic3r/Config/ConfigBackup.{hpp,cpp}` — zip writing / reading, manifest, redaction,
  pruning, listing, restore, extraction for compare (no wx dependency beyond `GUI_App` access in
  the trigger helpers).
- `src/slic3r/GUI/ConfigBackupDialog.{hpp,cpp}` — the dialog.
- Small hooks: `GUI_App.cpp` (startup trigger, menu, preset-save debounce), `Snapshot.cpp`
  (`before-config-update`), `Preset.cpp` (file-changed notification), `Tab.cpp` (rename),
  `PresetBundle` (`load_presets` from another directory), `UnsavedChangesDialog`
  (`DiffPresetDialog` with external bundles), `Preferences.cpp` / `AppConfig.cpp` (settings).

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
