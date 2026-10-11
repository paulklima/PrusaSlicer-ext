---
id: wx3.3
kind: feature
title: wxWidgets 3.3.3 (upstream) + API compatibility
show: true
upstream_pr: none
---

# feature/wx3.3 — wxWidgets 3.3.3

Stacked on `feature/wayland` (shares `deps/+wxWidgets/wxWidgets.cmake` and GUI files).

## What / why

Upstream 2.9.x builds against Prusa's wxWidgets 3.2.6 fork. wx 3.3 brings ~17 Wayland/EGL
fixes (clipboard, DnD, HiDPI GL scaling, gestures, runtime GLX/EGL selection via
`PreferGLX()`).

- `deps/+wxWidgets/wxWidgets.cmake`: upstream wxWidgets **3.3.3** release tarball
  (SHA256 `81b09d6dd9f1ed9301f8c55a968a488d0491f264dc2bab19a7e407ac67009482`),
  `wx-fixes.patch` dropped (fixed upstream), `wx-wayland-protocol.patch` added
  (`#ifdef wxHAVE_WAYLAND_CLIENT` → `#if`, still needed in 3.3.3).
- `cmake/modules/FindwxWidgets.cmake`: handle CMake target names in `wx-config` output.
- GUI API compat (16 files): `const wxString&` → value, `SetLabel` → `ChangeValue`,
  ambiguous `attach()`, `char + wxString`, `PreferGLX` in `OpenGLManager`.
- `ImGuiWrapper`: `wxTextDataObject::GetTextLength()` → `!GetText().IsEmpty()` (wx 3.3 API).
- The general GCC 15 / CMake 4 warning cleanup lives in `feature/fork-tooling`.

## How to test

Deps must be rebuilt after switching wx (`make` rebuilds `deps/build`). Check
About → System Info shows `wxWidgets 3.3.3`, copy/paste in text fields, drag & drop of STL,
HiDPI scaling of the 3D view, WebView pages.

## Known runtime noise (harmless)

- `gtk_widget_get_style_context` GTK-CRITICAL warnings (custom widgets style before realize).
- `wxStaticBoxSizer` child-parenting debug messages (stricter checks in wx 3.3).

## Upstream status

Upstream moved to stock wxWidgets but pins a 3.3.1-era snapshot — re-check at every bump
whether this branch is still ahead. Upstream-PR candidate for the API-compat part.

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.6 | keep | upstream 2.9.6 still on Prusa wx 3.2.6 fork |

## Notes

- `notes/wxwidgets-wayland-migration-analysis.md` — historical analysis of the Wayland/wx
  migration incl. the wx 3.3 audit (moved from `doc/wxWidgets-Wayland-Migration-Linux.md`).
