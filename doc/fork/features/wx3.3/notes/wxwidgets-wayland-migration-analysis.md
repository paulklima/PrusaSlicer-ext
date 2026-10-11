# wxWidgets Wayland Migration Analysis — Linux Only

**Date:** April 18, 2026 (analysis), April 19, 2026 (implementation verified)
**Status:** **COMPLETE** — native Wayland support working. Original migration branch was `wayland-migration`; today the work lives on `feature/wayland` + `feature/wx3.3` (see `doc/FORK-WORKFLOW.md`). Commit hashes below are from the original migration and have since been re-based.
**Scope:** PrusaSlicer — the *initial* native-Wayland enablement, which kept the
wxWidgets 3.2.6 fork (GTK3 + EGL flipped on). The tables below describe that phase.

> **Update:** wxWidgets has since been upgraded to **upstream 3.3.3** on the
> stacked `feature/wx3.3` branch. Where this
> analysis says "3.2.6 unchanged" or lists a wx bump as advisable/future, that
> bump is now **done**. See `doc/FORK-WORKFLOW.md` for the current set. (Historical document.)

---

## 1. wxWidgets Setup

| Item | Before (master) | After (wayland-migration) |
|------|-----------------|--------------------------|
| Base version | wxWidgets **3.2.6** | wxWidgets **3.2.6** (unchanged) |
| Fork | `github.com/prusa3d/wxWidgets` | same |
| Fork commit | `5462e7d7cfac645926188443e842171e107b312c` | same |
| Linux toolkit | **GTK2** by default (`DEP_WX_GTK3=OFF`) | **GTK3** by default (`DEP_WX_GTK3=ON`) |
| GTK3 option | Available but off by default | **ON by default** |
| EGL support | **Disabled** (`-DwxUSE_GLCANVAS_EGL=OFF`) | **Enabled** (`-DwxUSE_GLCANVAS_EGL=ON`) |
| GLEW EGL | Not configured | **`GLEW_EGL=ON`** on Linux |
| WebView | `wxUSE_WEBVIEW=ON` | same |
| SecretStore | Disabled on Linux (`wxUSE_SECRETSTORE=OFF`) | same |
| Unicode UTF-8 | Forced on (`wxUSE_UNICODE_UTF8=ON`) — Linux only | same |

### Patch applied to the fork (`wx-fixes.patch`)
A single patch exists — it fixes a **macOS 14.4+ screen capture crash** (`CGDisplayCreateImage` removed in macOS 15). This patch is macOS-only and has **no Linux relevance**.

The fork's latest commit message ("Linux specific: wxBitmapComboBox: Workaround for bitmap rendering on HDPI — since pixbufs can only have a scaling factor of 1…") indicates active Linux HiDPI work was still being patched as recently as January 2025.

---

## 2. Why Wayland Doesn't Work Today

### 2.1 Root cause — GTK2
Native Wayland requires **GTK3+**. The build uses GTK2 by default. GTK2 has no Wayland backend; it forces X11 regardless of the compositor.

### 2.2 EGL disabled — blocks OpenGL on Wayland
The CMake flag `-DwxUSE_GLCANVAS_EGL=OFF` disables EGL support. On Wayland, GLX (the X11 OpenGL extension) is not available. Without EGL, `wxGLCanvas` cannot create an OpenGL context natively on Wayland. This is the **single most impactful flag to change**.

### 2.3 Explicit GDK_BACKEND workaround
`src/CLI/Setup.cpp` had `setenv("GDK_BACKEND", "x11", …)` which forced X11 even when Wayland was available. **Fixed:** this call is now commented out. The two `WEBKIT_DISABLE_*` env vars are kept for now (WebKit compositor workarounds).

### 2.4 GLEW does not support EGL contexts
GLEW's default build uses GLX to resolve OpenGL function pointers. Under EGL (required for Wayland), `glewInit()` fails with "Unknown error" and the app segfaults. **Fixed:** GLEW is now built with `-DGLEW_EGL=ON` on Linux.

### 2.5 GUI startup rejects pure Wayland sessions
`src/CLI/GuiParams.cpp` only checked for `DISPLAY` (X11). Pure Wayland sessions without XWayland have no `DISPLAY` set. **Fixed:** now also checks `WAYLAND_DISPLAY`.

---

## 3. Changes Required

### 3.1 `deps/+wxWidgets/wxWidgets.cmake` — Build flags (HIGH IMPACT)

| Change | Current | Required | Impact |
|--------|---------|----------|--------|
| Enable GTK3 | `DEP_WX_GTK3=OFF` | Set default to `ON` | **Critical** — prerequisite for Wayland |
| Enable EGL | `-DwxUSE_GLCANVAS_EGL=OFF` | `-DwxUSE_GLCANVAS_EGL=ON` | **Critical** — OpenGL on Wayland |
| wxWidgets version | 3.2.6 fork | 3.2 latest or 3.3 | Advisable — Wayland fixes ongoing in wx |

For wx 3.3, change the `URL` and `URL_HASH` to point to the upstream 3.3 release (or a maintained Prusa fork rebased on 3.3). No structural CMake changes are needed beyond the flags.

### 3.2 `src/CLI/Setup.cpp` — Wayland environment setup (LOW — already done)

The `GDK_BACKEND` force is already commented out. The two `WEBKIT_*` env vars remain:
- `WEBKIT_DISABLE_COMPOSITING_MODE=1` — needed because WebKit's GPU compositing crashes under some Wayland compositors. **Keep for now.**
- `WEBKIT_DISABLE_DMABUF_RENDERER=1` — prevents dmabuf renderer issues in WebKitGTK. **Keep for now.**

No code change required here unless Wayland testing proves these are no longer needed.

### 3.3 `src/slic3r/GUI/OpenGLManager.cpp` — EGL context (MEDIUM IMPACT)

With EGL enabled, the existing `wxGLContextAttrs`/`wxGLCanvas` creation code is compatible with wx 3.2/3.3 and **does not need rewriting**. wx handles the GLX→EGL switch transparently when compiled with EGL support. Verify the context attribute sequence still works after enabling EGL.

```cpp
// OpenGLManager.cpp — already uses the modern wxGLAttributes API (wx 3.0+)
wxGLAttributes attribList;
attribList.PlatformDefaults().RGBA().DoubleBuffer().MinRGBA(8, 8, 8, 8).Depth(24).SampleBuffers(1).Samplers(i);
attribList.SetNeedsARB(true);
attribList.EndList();
```

### 3.4 GTK direct API usage — compatibility with GTK3 (MEDIUM IMPACT)

Several files use GTK APIs directly under `__WXGTK__`. All must be verified against GTK3.

| File | GTK API Used | GTK3 Compatible? | Notes |
|------|-------------|-----------------|-------|
| `src/slic3r/GUI/GUI_App.cpp` | `g_object_set(gtk_settings_get_default(), "gtk-menu-images", TRUE)` | **NO** — removed in GTK 3.10 | Must be removed or guarded with `#ifdef __WXGTK2__` |
| `src/slic3r/GUI/BitmapComboBox.cpp` | `#include <gtk/gtk.h>`, pixbuf scaling workaround | Needs review | HiDPI pixbuf workaround may behave differently on GTK3 |
| `src/slic3r/GUI/PresetComboBoxes.cpp` | `gtk_container_get_children`, `gtk_entry_set_width_chars`, `gtk_entry_get_layout` | **Partially deprecated** — `gtk_container_get_children` removed in GTK4 (OK for GTK3); `gtk_entry_get_layout` OK in GTK3 | Low risk for GTK3 target |
| `src/slic3r/GUI/Widgets/DropDown.cpp` | `gtk_popup_key_press`, `gtk_get_event_widget`, `gtk_widget_get_parent`, `gtk_window_resize` | OK on GTK3 | Should work as-is |
| `src/slic3r/GUI/ExtraRenderers.cpp` | `g_value_set_string`, `g_object_set_property`, `wxGTK_CONV_FONT` — currently **commented out** | N/A | Already dead code |
| `src/slic3r/GUI/WebViewPlatformUtilsLinux.cpp` | `WebKit2/webkit2.h`, `libsoup`, GLib signals | GTK3 + webkit2gtk required | Should remain compatible; webkit2gtk requires GTK3 anyway |

**Critical fix:** `GUI_App.cpp` line 1368:
```cpp
// REMOVE or guard — "gtk-menu-images" property does not exist in GTK 3.10+
g_object_set(gtk_settings_get_default(), "gtk-menu-images", TRUE, NULL);
```
Replace with:
```cpp
#ifdef __WXGTK2__
g_object_set(gtk_settings_get_default(), "gtk-menu-images", TRUE, NULL);
#endif
```

### 3.5 `src/slic3r/GUI/GLCanvas3D.cpp` — `#undef Convex` (LOW — may still be needed)

```cpp
// GLCanvas3D.cpp lines 234–237
#if defined(__linux__) && defined(Convex)
#undef Convex  // Collision with X11/X.h macro
#endif
```
This X11 macro collision exists when X11 headers are pulled in. With GTK3 + Wayland this may or may not be triggered depending on whether X11 headers are still included transitively. **Keep the guard — it is harmless.**

### 3.6 HiDPI / `GetContentScaleFactor` (LOW — already handled)

The codebase already uses:
- `wxDisplay::GetScaleFactor()` — supported in wx 3.1+ on all platforms
- `GetContentScaleFactor()` — works on GTK3/Wayland in wx 3.2+
- `RetinaHelper` already has a `__WXGTK3__` specialisation using `GetContentScaleFactor()`
- `wxDPIChangedEvent` — available in wx 3.1+

No changes needed here.

### 3.7 `wxBitmapBundle` (LOW — already used)

164 usages of `wxBitmapBundle` detected. `wxBitmapBundle` is the correct modern wx API for HiDPI-aware bitmaps and is already in use throughout the codebase. This is positive for Wayland/GTK3 HiDPI support.

### 3.8 Instance check / D-Bus (NO CHANGE NEEDED)

`src/slic3r/GUI/InstanceCheck.cpp` already uses D-Bus (not X11 atoms) for single-instance detection on Linux. This is Wayland-compatible as-is.

### 3.9 `wxUSE_UNICODE_UTF8` (NO CHANGE NEEDED)

Forced `ON` for Linux. GTK3 expects UTF-8 throughout. This is correct.

---

## 4. WebKit / WebView on Wayland

`WebViewPlatformUtilsLinux.cpp` uses `webkit2gtk` directly. On GTK3 + Wayland:
- webkit2gtk 2.x runs fine on Wayland
- The two env vars in `Setup.cpp` (`WEBKIT_DISABLE_COMPOSITING_MODE`, `WEBKIT_DISABLE_DMABUF_RENDERER`) are workarounds for known Wayland + WebKit compositor issues
- `libsoup` usage (cookie management) is backend-agnostic

No changes expected here. However, `webkit2gtk` **requires GTK3** — so enabling GTK3 is a prerequisite for the WebView to even build.

---

## 5. Summary — Required Changes

### Completed (blockers — all done)

| # | File | Change | Status |
|---|------|--------|--------|
| 1 | `deps/+wxWidgets/wxWidgets.cmake` | Set `DEP_WX_GTK3` default to `ON` | **Done** (`c2dd28170`) |
| 2 | `deps/+wxWidgets/wxWidgets.cmake` | Change `wxUSE_GLCANVAS_EGL=OFF` → `ON` | **Done** (`c2dd28170`) |
| 3 | `src/slic3r/GUI/GUI_App.cpp` | Guard `gtk-menu-images` with `#ifdef __WXGTK2__` | **Done** (`b0b6db23f`) |
| 4 | `src/CLI/Setup.cpp` | Comment out `GDK_BACKEND=x11` forcing | **Done** (`a67b3633b`) |
| 5 | `deps/+GLEW/GLEW.cmake` | Enable `GLEW_EGL=ON` on Linux | **Done** (`5103f5e51`) |
| 6 | `src/CLI/GuiParams.cpp` | Accept `WAYLAND_DISPLAY` for pure Wayland sessions | **Done** (`5103f5e51`) |
| 7 | `deps/+GMP/GMP.cmake` | Add `-std=gnu11` for GCC 13+ | **Done** (`5103f5e51`) |

### Not done (deferred)

| # | File | Change | Effort |
|---|------|--------|--------|
| 8 | `deps/+wxWidgets/wxWidgets.cmake` | Update fork URL to wx 3.2 latest or 3.3 upstream | Low (URL + hash update) |

### Recommended / Risk mitigation (not yet verified)

| # | File | Change | Effort |
|---|------|--------|--------|
| 9 | `src/slic3r/GUI/BitmapComboBox.cpp` | Verify HiDPI pixbuf workaround on GTK3 | Low |
| 10 | `src/slic3r/GUI/PresetComboBoxes.cpp` | Review GTK3 widget traversal (gtk_container_get_children OK in GTK3) | Low |
| 11 | `src/slic3r/GUI/OpenGLManager.cpp` | Test EGL context creation after enabling `wxUSE_GLCANVAS_EGL=ON` | Medium (testing) |
| 12 | `src/CLI/Setup.cpp` | Re-evaluate `WEBKIT_DISABLE_COMPOSITING_MODE` / `WEBKIT_DISABLE_DMABUF_RENDERER` after GTK3 testing | Low |

### No changes required

- All `__WXGTK__` preprocessor guards throughout the codebase
- `wxGLContextAttrs` / `wxGLCanvas` API usage
- `wxBitmapBundle` usage
- D-Bus instance check
- HiDPI scaling code (`GetContentScaleFactor`, `GetScaleFactor`, `wxDPIChangedEvent`)
- `#undef Convex` guard
- `wxUSE_UNICODE_UTF8`
- The existing patch in `wx-fixes.patch` (macOS-only)

---

## 6. wx 3.2 vs. 3.3 for Wayland

| | wx 3.2.x (latest) | wx 3.3.x (dev/stable) |
|--|--|--|
| GTK3 Wayland support | Yes (via GDK Wayland backend) | Yes + improvements |
| EGL/OpenGL on Wayland | Yes, with `wxUSE_GLCANVAS_EGL=ON` | Yes + further fixes |
| `wxBitmapBundle` | Yes | Yes |
| API compatibility with current code | **Full** — no API breaks | **Mostly** — check for deprecated removals |
| HiDPI on Wayland | Works but has edge cases | More actively maintained |
| Recommendation | **Lower risk** path; sufficient for Wayland | Better long-term; minor migration work |

For minimal effort, targeting **wx 3.2 latest with GTK3 + EGL enabled** achieves native Wayland support. Upgrading to **3.3** adds future-proofing and ongoing Wayland fixes but requires verifying any deprecated APIs removed between 3.2 and 3.3.

---

## 7. Estimated Total Work

| Category | Lines/Files Touched | Effort Estimate |
|----------|-------------------|-----------------|
| Build flags (GTK3 + EGL) | 2 lines in 1 file | 15 minutes |
| `gtk-menu-images` removal | 3 lines in 1 file | 15 minutes |
| wx version URL bump | 2 lines in 1 file | 30 minutes + SHA verification |
| GTK3 API compat testing | 3 files | 1–2 days (compile + test) |
| OpenGL EGL validation | 1 file | 1–2 days (device testing) |
| WebView smoke testing | 1 Linux file | 0.5 day |
| **Total code changes** | **~10 lines across 4 files** | **~2 hours** |
| **Total testing** | — | **2–4 days** |

The code changes are minimal. The risk and effort lie entirely in **testing** the OpenGL + EGL pipeline and GTK3 widget rendering under Wayland compositors (GNOME/Mutter, KDE/KWin, wlroots-based).

---

## 8. Re-applying Wayland Changes After an Upstream PrusaSlicer Update

### 8.1 Background — existing EGL experiment as a reference

There is already a branch `origin/tm_wxEGL_GOOD` in the repository that attempted an earlier EGL/wx upgrade. That branch is currently:

- **8 commits ahead** of `master` (the actual Wayland-related changes)  
- **7,150 commits behind** `master` (it was never rebased after the initial work)

This branch shows exactly what happens when Wayland changes are done in isolation and not kept in sync: the divergence becomes so large that a simple rebase is no longer practical and the whole migration must be redone from scratch. It also confirms that a previous migration attempt existed and was abandoned mid-flight.

### 8.2 Nature of the Wayland-specific changes

The 4 changes identified in Section 5 are deliberately minimal and well-isolated:

| Change | File | Type | Upstream churn risk |
|--------|------|------|---------------------|
| GTK3 default + EGL flag | `deps/+wxWidgets/wxWidgets.cmake` | Build config only | **Very low** — Prusa only touches this file when bumping wx |
| URL + hash bump (wx version) | `deps/+wxWidgets/wxWidgets.cmake` | URL string | **Medium** — every wx version bump changes this |
| `gtk-menu-images` guard | `src/slic3r/GUI/GUI_App.cpp` | 3-line `#ifdef` | **Very low** — this specific GTK init block is rarely touched by feature work |

### 8.3 Merge effort per upstream scenario

#### Scenario A — Minor PrusaSlicer release (e.g. 2.9.x → 2.9.y, patch/RC)

These releases typically touch: profiles, localization, bug fixes in print logic. GUI files change but `wxWidgets.cmake` and the `GUI_App.cpp` GTK init block are almost never modified.

| Wayland change | Conflict probability | Re-apply effort |
|----------------|---------------------|-----------------|
| GTK3 + EGL flags | ~0% | Automatic merge |
| `gtk-menu-images` guard | ~0% | Automatic merge |
| wx URL/hash | 0% (unchanged) | Nothing to do |
| **Total** | | **< 5 minutes** (verify merge, rebuild) |

#### Scenario B — Major PrusaSlicer feature release (e.g. 2.9 → 2.10 or 3.0)

These releases refactor GUI code, may bump wx, and occasionally restructure `GUI_App.cpp`.

| Wayland change | Conflict probability | Re-apply effort |
|----------------|---------------------|-----------------|
| GTK3 + EGL flags in `wxWidgets.cmake` | ~10% (if they bump wx) | Trivial — re-apply 2 flag lines to new cmake |
| wx URL/hash | ~80% (likely bumped) | Low — update URL + compute new SHA256 |
| `gtk-menu-images` guard | ~5% | Trivial — re-add 2-line `#ifdef` guard |
| GTK3 API compat (PresetComboBoxes, BitmapComboBox) | ~20% | Low — verify GTK3 calls still valid in new code |
| EGL context testing | — | Medium — must re-test OpenGL on Wayland after any wx bump |
| **Total** | | **2–4 hours code + 1–2 days retesting** |

#### Scenario C — PrusaSlicer bumps wxWidgets to 3.3 themselves

If upstream migrates to wx 3.3 on their own, most of the work in Section 3 is done for you. The remaining effort reduces to:

- Verify GTK3 is now the default in their updated `wxWidgets.cmake` — if not, re-add the flag
- Verify `wxUSE_GLCANVAS_EGL=ON` is set — if not, add it
- Check the `gtk-menu-images` guard is no longer needed (upstream may have removed the call)
- **Net effort: ~30 minutes**

### 8.4 Recommended strategy to minimize re-apply effort

**Option 1 — Patch file approach (recommended)**  
Maintain the Wayland changes as a single `wayland-support.patch` file tracked separately (e.g. in a `patches/` directory). Each upstream update is then:
```
git merge upstream/master
git apply patches/wayland-support.patch
# Resolve any conflicts (expected: minimal)
```
Because the changes touch only ~10 lines in well-isolated locations, `git apply` will succeed without conflicts in the vast majority of upstream updates. Conflicts only arise when upstream itself modifies `wxWidgets.cmake` (wx version bump) or the specific `g_object_set` line in `GUI_App.cpp`.

**Option 2 — Long-lived feature branch with regular rebase**  
Maintain a `wayland` branch and rebase it onto each new upstream tag:
```
git checkout wayland
git rebase upstream/master  # or the new tag
```
With only 4 trivially small commits, this rebase should auto-resolve in most cases. The `tm_wxEGL_GOOD` cautionary tale applies here: **rebase must be done with every upstream release**, not deferred.

**Option 3 — Upstream the changes**  
Contribute the GTK3 + EGL flag changes back to `prusa3d/PrusaSlicer` as a PR. The build-flag changes (`DEP_WX_GTK3`, `wxUSE_GLCANVAS_EGL`) are clearly beneficial for all Linux users and have a high chance of acceptance. If merged upstream, the re-apply effort drops to **zero** for future updates.

### 8.5 Files most likely to cause merge conflicts on upstream updates

Based on git activity patterns, these are the files in the Wayland changeset ordered by upstream churn risk:

| File | Expected upstream churn | Notes |
|------|------------------------|-------|
| `src/slic3r/GUI/GUI_App.cpp` | **High** — central app file, touched frequently | Change is a 3-line guard around 1 specific line; conflict highly localizable |
| `deps/+wxWidgets/wxWidgets.cmake` | **Medium** — changes only on wx bumps | URL + 2 flag lines; straightforward to re-apply |
| `src/slic3r/GUI/BitmapComboBox.cpp` | Low | Only compile-test needed after upstream changes |
| `src/slic3r/GUI/PresetComboBoxes.cpp` | Low | Verify GTK3 widget traversal still valid |

### 8.6 Summary

The Wayland migration changeset is unusually small (~10 lines) and highly localized. As long as the changes are maintained as a clean patch or rebased regularly, the ongoing maintenance cost per upstream PrusaSlicer release is:

- **Minor releases:** near-zero, automatic merge
- **Major releases / wx bumps:** 2–4 hours code + 1–2 days retesting
- **If upstream adopts GTK3/EGL themselves:** one-time verification pass only

The lesson from `tm_wxEGL_GOOD` (7,150 commits of drift) is that deferring the rebase is the only real risk — the changes themselves are not the problem.

---

## 9. Official wxWidgets 3.2 → 3.3 Migration Path

Source: upstream `docs/changes.txt` of wxWidgets 3.3.2 (March 2026) — section "INCOMPATIBLE CHANGES". wxWidgets 3.3.0 was released June 2025, 3.3.1 in July 2025, 3.3.2 in March 2026. Upstream states: *"almost fully compatible with 3.2; updating existing applications shouldn't require much effort"* — but the following items **must** be audited in this codebase before building.

### 9.1 MUST-fix (source, build, or runtime breaks)

Items that either fail to compile or silently change behavior. Each one must be checked against PrusaSlicer sources before the first wx 3.3 build is attempted.

#### Build system

| # | Change | Action in this repo |
|---|--------|---------------------|
| M1 | **`wxUSE_UNICODE=0` no longer supported** | Already forced ON (`wxUSE_UNICODE_UTF8=ON`). No change. |
| M2 | **`wxUSE_STD_CONTAINERS` now defaults to 1**, `wxUSE_STL` option removed | Audit `libslic3r`/`slic3r` for code relying on wx's own list/array semantics; set `wxUSE_STD_CONTAINERS=0` in `wxWidgets.cmake` as a short-term escape hatch if needed. |
| M3 | **Deprecated-in-3.0 symbols hidden** unless `WXWIN_COMPATIBILITY_3_0=1`; 2.8 symbols gone | Grep for any wx 2.x-era APIs still in use; add the compat flag in `wxWidgets.cmake` if many hits, otherwise port. |
| M4 | **wxMotif and wxGTK1 ports removed** | Not used. No change. |
| M5 | **CMake install path now versioned** (`lib/cmake/wxWidgets-3.3`) | The deps flow uses `CMAKE_PREFIX_PATH` pointing at `destdir`, so this is transparent. Verify nothing hardcodes `wxWidgets-3.2`. |
| M6 | **`wxGLCanvas::CreateSurface()` unavailable in EGL builds** | Grep — if any code calls `CreateSurface()`, it must be removed/guarded when EGL is on (and EGL is required for Wayland, so this is a hard constraint). |

#### API / runtime

| # | Change | Action in this repo |
|---|--------|---------------------|
| M7 | **`wxGetTranslation()` returns `wxString` by value**, not `const wxString&` | Grep all `_(...)` / `wxGetTranslation` usages stored in `const wxString&` locals — change to `wxString`. Very common pattern in the GUI. |
| M8 | **All wx-type operators moved out of global scope** | Implicit conversions may stop compiling. Audit `operator==` / `operator+` on `wxString`, `wxPoint`, `wxSize`, `wxColour` with mixed types. |
| M9 | **`wxString` brace-init ambiguity** (e.g. `s = {"x", 2}`) | Grep for brace-initialised wxString assignments; disambiguate with explicit `wxString{...}`. |
| M10 | **`wxSizer::Detach()` now takes `wxWindowBase*`** | Check subclassed sizer overrides. Low risk. |
| M11 | **DC arguments changed to `wxReadOnlyDC`** on several virtual functions | Any custom `OnPaint`-ish overrides of wx virtuals taking a DC must update signatures; otherwise they silently become non-overrides. Grep `override` + DC. |
| M12 | **`wxBitmap::Create(size, dc)` no longer scales by content scale factor** | Audit HiDPI bitmap creation in `BitmapComboBox.cpp`, theme/icon code — remove compensating divides if present. |
| M13 | **`wxImageList` size now in physical pixels** | Already using `wxBitmapBundle` throughout (see §3.7). Audit remaining `wxImageList` users. |
| M14 | **`wxClientDC` / `wxPaintDC` origin now correctly offset by toolbar** | Remove any manual toolbar-height compensation in custom paint code. Check `GLCanvas3D.cpp` and any notebook/dialog painting. |
| M15 | **`wxGLCanvas` no longer multi-samples by default** | `OpenGLManager.cpp` already calls `SampleBuffers(1).Samplers(i)` explicitly — OK. Verify all `wxGLCanvas` creation sites. |
| M16 | **`wxAuiGenericTabArt` subclasses must override `DrawPageTab`/`GetPageTabSize`** instead of `DrawTab`/`GetTabSize` | Check for custom AUI tab art. |
| M17 | **`wxAuiNotebook` page index is now logical, not visual** | If the code reorders AUI pages and uses indices for positioning, call `GetPagePosition()` explicitly. |
| M18 | **`wxAUI_MGR_HINT_FADE` removed from default style** | Add back explicitly if the fade hint is desired. |
| M19 | **Default fatal-error exit code is 255** (was 127 on MSVC) | Likely irrelevant; call `SetFatalErrorExitCode()` if a specific code is expected. |
| M20 | **`wxColourDatabase` values now match CSS** | If the code looks up colours by name and compares, call `wxColourDatabase::UseScheme(wxColourDatabase::Traditional)`. |
| M21 | **`wxFileConfig` now uses XDG `~/.config/<app>.conf`** | PrusaSlicer manages its own config dir, but if `wxFileConfig` is used anywhere, pass `wxCONFIG_USE_HOME` to preserve old paths. |
| M22 | **`wxTextCtrl::SetLabel()` now does nothing and asserts** | Grep for `SetLabel` on `wxTextCtrl` → replace with `SetValue` / `ChangeValue`. |
| M23 | **`wxTextCtrl::Save/LoadFile` auto-uses RTF for `.rtf`** | Low relevance; audit any `.rtf` paths. |
| M24 | **`wxWindow::Raise()` no longer implies Show() on all ports** | If raise-to-front expects visibility, call `Show()` explicitly first. |
| M25 | **`wxTRANSPARENT_WINDOW` is a no-op** | Grep; remove if present. |
| M26 | **`wxListCtrl::EditLabel` asserts without `wxLC_EDIT_LABELS`** | Ensure the style is set wherever edit-label is invoked. |
| M27 | **`wxSystemAppearance::IsDark()` now returns app-scoped**, not system-scoped | Use `IsSystemDark()` if the old meaning is wanted. |
| M28 | **wxGTK `wxDirButton::Create()` dropped the `wildcard` parameter** | Grep — likely no hits. |
| M29 | **`wxStyledTextCtrl::AddSelection()` now returns `void`** | Remove any use of the return value. |
| M30 | **`wxTEST_DIALOG()` macro now requires trailing semicolon** | Only affects tests using this macro. |

### 9.2 SHOULD-fix (deprecations, recommended alignment)

Not build-breaking, but worth addressing while the migration is open.

| # | Change | Why |
|---|--------|-----|
| S1 | **Chrome-based wxWebView backend available** | Potential replacement for webkit2gtk on Linux if WebKit keeps causing Wayland compositor issues. Evaluate later; don't adopt in this migration. |
| S2 | **Pinned / multiline tabs in `wxAuiNotebook`** | Nice-to-have UX improvement; not required. |
| S3 | **wxWidgets 3.3.1 adds Wayland "app id" support** (`SetAppId`) | Correct app-id means desktop files/icons attach properly under Wayland compositors. **Recommended** — set the app id to `com.prusa3d.PrusaSlicer` (or the fork's reverse-DNS id) at startup in `GUI_App`. |
| S4 | **Persistence support for wxRadioButton / wxCheckBox** | Optional; only if the existing preference persistence benefits. |
| S5 | **`wxPGCellRenderer::DrawCaptionSelectionRect` deprecated overload** | If any custom PropertyGrid cell renderer exists, switch to the new overload. |
| S6 | **Legacy memory tracing (`wxUSE_MEMORY_TRACING`) removed** | Already disabled; use ASan via `SLIC3R_ASAN=ON` instead — which the build already supports. |
| S7 | **Dark-mode support on Windows** | Not relevant to the Linux Wayland goal; ignore for this migration. |
| S8 | **`wxGLCanvas` can now mix GLX and EGL contexts** | For Wayland only EGL matters; enabling `wxUSE_GLCANVAS_EGL=ON` is sufficient. |
| S9 | **`WarpPointer()` now works on Wayland compositors** | If any 3D view gesture uses `WarpPointer`, it will behave correctly on Wayland — no source change needed, just good to know. |
| S10 | **wxWidgets 3.3 minimum platform bumps**: macOS 10.10, GTK ≥ 2.6 | Check `deps/+wxWidgets/wxWidgets.cmake` for any min-version overrides. |

### 9.3 Recommended target

Pin to **wxWidgets 3.3.2** (or the latest `v3.3.x` tag) as the new `URL`/`URL_HASH` in `deps/+wxWidgets/wxWidgets.cmake`. 3.3.2 is the current Wayland-best release with almost a year of Wayland/EGL fixes on top of 3.3.0. If the macOS screen-capture fix from the current `wx-fixes.patch` is still needed, verify whether upstream 3.3.2 already includes the equivalent fix before re-applying the patch.

### 9.4 Audit checklist (to run before the first 3.3 build)

```text
# Incompatibility greps (non-exhaustive):
grep -rn 'const wxString& *[A-Za-z_][A-Za-z_0-9]* *= *_(' src/       # M7
grep -rn 'SetLabel' src/slic3r src/libslic3r                        # M22 (filter to wxTextCtrl)
grep -rn 'wxTRANSPARENT_WINDOW'                                     # M25
grep -rn 'CreateSurface'                                            # M6
grep -rn 'wxImageList'                                              # M13
grep -rn 'AddSelection'                                             # M29 (StyledTextCtrl)
grep -rn 'wxAuiGenericTabArt\|DrawTab\|GetTabSize' src/             # M16
grep -rn 'DrawCaptionSelectionRect'                                 # S5
```

The combined audit is expected to produce fewer than a few dozen hits total — the migration remains a ~10-line build-flag change plus a bounded list of localized source touch-ups.

Sources:
- [wxWidgets 3.3.0 release announcement (June 2025)](https://wxwidgets.org/news/2025/06/wxwidgets-3.3.0-released/)
- [wxWidgets 3.3.1 release announcement (July 2025)](https://wxwidgets.org/news/2025/07/wxwidgets-3.3.1-released/)
- [wxWidgets 3.2.10 and 3.3.2 release announcement (March 2026)](https://wxwidgets.org/news/2026/03/wxwidgets-3.2.10-and-3.3.2-released/)
- [Upstream `docs/changes.txt` for v3.3.2 — INCOMPATIBLE CHANGES](https://raw.githubusercontent.com/wxWidgets/wxWidgets/v3.3.2/docs/changes.txt)
