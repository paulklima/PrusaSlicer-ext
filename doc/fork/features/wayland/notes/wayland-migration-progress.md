# Wayland Migration — Progress Log

Historical progress log of the original migration (the branch names of that time no longer
exist). Kept for reference only; the current workflow is described in `doc/FORK-WORKFLOW.md`.
Goal: native Wayland support on Linux.
Status: **Working** — builds and launches natively on Wayland (tested 2026-04-19).

For the full analysis and future wx 3.3 upgrade path, see
`doc/fork/features/wx3.3/notes/wxwidgets-wayland-migration-analysis.md`.

Each step below is a single commit so it can be reproduced, skipped,
cherry-picked or reverted independently.

---

## Step 0 — Sandbox prep

**Commit:** `042ba057b` — `sandbox: add Agent.Dockerfile with GTK3/Wayland build deps`

**What:** Creates `.axtesys/casket/Agent.Dockerfile`. Installs:

- `build-essential`, `autoconf`, `automake`, `libtool`, `pkg-config`,
  `cmake`, `ninja-build`, `git`, `texinfo`, `m4`, `perl`, `python3`
- GL / EGL: `libglu1-mesa-dev`, `libgl1-mesa-dev`, `libegl1-mesa-dev`
- GTK3 + Wayland: `libgtk-3-dev`, `libwayland-dev`, `wayland-protocols`,
  `libxkbcommon-dev`
- X11 dev headers still needed for transitive deps: `libxrandr-dev`,
  `libxi-dev`, `libxcursor-dev`, `libxinerama-dev`
- WebView: `libwebkit2gtk-4.1-dev`, `libsoup` (pulled in), `libsecret-1-dev`
- Misc: `libdbus-1-dev`, `libcurl4-openssl-dev`, `libssl-dev`,
  `libudev-dev`, `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`,
  `libnotify-dev`

**Skip if:** you are building outside casket with the packages from
`doc/How to build - Linux et al.md` already installed.

---

## Step 1 — wxWidgets build flags

**Commit:** `c2dd28170` — `wx: default to GTK3 and enable EGL for Wayland`

**What:** in `deps/+wxWidgets/wxWidgets.cmake`:

- `DEP_WX_GTK3` default `OFF` → `ON` (prerequisite for Wayland)
- `-DwxUSE_GLCANVAS_EGL=OFF` → `ON` (OpenGL context on Wayland)

**Why:** GTK3 + EGL are the two blockers on the wx side.

**Not done in *this* step:** bumping wx version (this commit kept the prusa3d
fork @ `5462e7d` ~ 3.2.6). The wx bump was a separate, later step and **has since
been completed** — upstream wxWidgets **3.3.3** on `feature/wx3.3` (see the "Done
since" note below and `doc/fork/features/wx3.3/notes/wxwidgets-wayland-migration-analysis.md`).

---

## Step 2 — GTK3 source compat: `gtk-menu-images`

**Commit:** `b0b6db23f` — `gui: guard gtk-menu-images for GTK2 only`

**What:** `src/slic3r/GUI/GUI_App.cpp` — the
`g_object_set(gtk_settings_get_default(), "gtk-menu-images", TRUE, …)`
call is now guarded by `#ifdef __WXGTK2__` only.

**Why:** the `gtk-menu-images` property was removed from GtkSettings
in GTK 3.10. Setting it on GTK3 triggers runtime warnings.

---

## Step 3 — CLI: stop forcing X11

**Commit:** `a67b3633b` — `cli: stop forcing GDK_BACKEND=x11`

**What:** `src/CLI/Setup.cpp` — `setenv("GDK_BACKEND", "x11", …)`
is commented out.

**Why:** forcing `x11` prevents GTK3 from attaching to Wayland.
The two `WEBKIT_DISABLE_*` env vars are kept — they work around
known WebKitGTK/Wayland compositor issues.

---

## Step 4 — GLEW EGL, Wayland display check, build docs

**Commit:** `5103f5e51` — `fix: GLEW EGL support, Wayland display check, build docs`

**What:**

- `deps/+GLEW/GLEW.cmake` — enable `GLEW_EGL=ON` on Linux so
  `glewInit()` works under EGL contexts (required for Wayland OpenGL).
  Without this, GLEW uses GLX to resolve function pointers, which fails
  under an EGL context and causes a segfault.

- `deps/+GMP/GMP.cmake` — add `-std=gnu11` to fix GMP build with
  GCC 13+.

- `src/CLI/GuiParams.cpp` — accept `WAYLAND_DISPLAY` as alternative
  to `DISPLAY` for GUI startup. Pure Wayland sessions (no XWayland)
  have no `DISPLAY` set and were rejected.

- `doc/How to build - Linux et al.md` — added Wayland support section,
  updated outdated EGL note in dynamic linking section, added locale
  requirement note (Boost extraction needs UTF-8).

---

## Build instructions

See `doc/How to build - Linux Wayland.md` for a complete Wayland build
walkthrough.

---

## Remaining work

Verified working but not yet stress-tested:

- `BitmapComboBox.cpp` — HiDPI pixbuf workaround on GTK3
- `PresetComboBoxes.cpp` — `gtk_container_get_children` (fine on GTK3,
  removed in GTK4)
- `OpenGLManager.cpp` — EGL context attribute edge cases
- `WEBKIT_DISABLE_*` env vars — may no longer be needed on modern
  WebKitGTK + Wayland compositors

Done since:

- **wxWidgets upgraded to upstream 3.3.3** (`feature/wx3.3`), bringing the
  Wayland/EGL fixes — see `doc/fork/features/wx3.3/notes/wxwidgets-wayland-migration-analysis.md` for the
  audit checklist and `doc/FORK-WORKFLOW.md` for how it ships.

---

## Rollback

See "Remove a feature" in `doc/FORK-WORKFLOW.md`.
