---
id: wayland
kind: feature
title: Native Wayland (GTK3 + EGL)
show: true
upstream_pr: none
---

# feature/wayland — native Wayland support

## What / why

Upstream PrusaSlicer on Linux runs through XWayland (wxWidgets GTK2/GLX); the official
Flatpak did not run on the maintainer's Wayland desktop. This feature makes the app run
natively on Wayland:

- wxWidgets built with **GTK3 + EGL** (`DEP_WX_GTK3=ON`, `wxUSE_GLCANVAS_EGL=ON`),
  GLEW with `GLEW_EGL=ON`.
- `GUI_App.cpp`: `gtk-menu-images` setting guarded (not available on GTK3/Wayland).
- `src/CLI/Setup.cpp` / `GuiParams.cpp`: no forced `GDK_BACKEND=x11`.
- Wayland `.desktop` entries for PrusaSlicer and the G-code viewer.

The build tooling (top-level `Makefile`, build-dir `.gitignore`, CMake 4 support) belongs to
`feature/fork-tooling`.

## Files touched

`deps/+GLEW/GLEW.cmake`, `deps/+wxWidgets/wxWidgets.cmake`, `src/CLI/GuiParams.cpp`,
`src/CLI/Setup.cpp`, `src/slic3r/GUI/GUI_App.cpp`, `src/platform/unix/*-wayland.desktop`.

## How to test

On `dev/<version>` (which also has the build tooling): `make && make run` on a Wayland session; `echo $XDG_SESSION_TYPE` = `wayland`, the app must
not show up in `xlsclients`. 3D view, gizmos and WebView (Printables / Connect) must work.

## Stack

`feature/wx3.3` is stacked on this branch (shares `deps/+wxWidgets/wxWidgets.cmake`).
Rebase the stack from its top: `git rebase --update-refs --onto <new> <old> feature/wx3.3`.

## Upstream status

Upstream-PR candidate (Wayland/CMake parts). Not yet proposed. Upstream 3.0 alphas also move
towards GTK3; re-check at every bump.

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.4 – 2.9.6 | keep | upstream still GTK2/X11-only on Linux |

## Notes

- `notes/wayland-migration-progress.md` — historical step-by-step log of the original
  migration (the branch names of that time no longer exist).
