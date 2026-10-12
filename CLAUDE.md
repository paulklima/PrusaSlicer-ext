# CLAUDE.md

Guidance for Claude Code (and any coding agent) working in this repository.

## What this repository is

PrusaSlicer — a C++ slicer for FFF and mSLA 3D printers — **forked from
`prusa3d/PrusaSlicer`** as **"PrusaSlicer ext"**. Per upstream release there is a development
line `dev/<upstream>` (features merged and tested together) and a release line
`fork/<upstream>` (releases only), tagged `v<upstream>+ext-v<our semver>` (e.g.
`v2.9.6+ext-v1.0.0`).

What the fork adds is **not listed here** — every feature/fix has its own folder
`doc/fork/features/<id>/README.md` (also shown in About / System Info). Read those folders and
**`doc/FORK-WORKFLOW.md`** (model, everyday work, upstream releases, rationale).

## Fork workflow rules (binding for agents)

- Work only on `feature/<id>` or `fix/<id>`, based on the upstream tag (`version_X.Y.Z`).
  **Ask the user** before creating one (feature or fix? which `<id>`?) and before merging,
  removing or archiving any branch.
- One feature = one capability with everything it needs (code, docs, tooling, display) —
  don't split it into pieces that cannot be used or tested alone.
- First commit of a new branch: `doc/fork/features/<id>/README.md` (frontmatter
  `id kind title show upstream_pr`). Plans, reviews, notes go into that folder only.
- Testing happens on `dev/<upstream>`: merge the feature there with `git merge --no-ff`, the
  user builds and tests. **Any change request = test failed**: fix on the feature branch,
  merge into `dev` again.
- **`fork/<upstream>` and tags only when the user says "release"**: `git merge --no-ff
  dev/<upstream>` into `fork/<upstream>`, then the tag. Never commit directly on `dev/*` or
  `fork/*`, never rebase/force-push them once pushed, never move release tags.
- `SLIC3R_VERSION` stays the upstream version; our version lives only in the tag.
- No pushes from the sandbox: prepare locally, give the user the exact push commands.
- Setup per clone: `scripts/fork-setup.sh` (session hook, `--no-ff` default, `CLAUDE.local.md`).
- New upstream release: `git fork-bump plan|rebase|merge version_X.Y.Z` (stable tags only);
  per-feature keep/drop/rework decisions go into `doc/fork/lines/<upstream>.md`.
- Upstream-PR heads: `pr/<id>` (code only, from `upstream/master`), never merged into `dev`/`fork`.

## Build

Two stages: static dependencies (`deps/`) first, then the app. **The dependency
destdir is NOT relocatable** — wxWidgets and other deps bake their absolute
install path into pkg-config/CMake config files. Moving the tree (or building the
same tree under a different mount path) breaks the app configure with
`wx/version.h not found`; rebuild the deps if that happens.

### Canonical path — the top-level `Makefile`

```bash
make            # deps (once) into deps/build, then app into build
make run        # ./build/src/prusa-slicer
make test       # ctest
make JOBS=8     # override parallelism
```

The `Makefile` exports `CMAKE_POLICY_VERSION_MINIMUM=3.5` for every cmake
invocation — required on **Ubuntu 26.04 (CMake 4.x)**, which dropped compatibility
with the `< 3.5` policy versions the vendored deps still request.

**Build log:** `build/build.log` (deps: `deps/build/build.log`), overwritten by every `make`
run: header (date, branch, commit, tools), configure/build output, and a final
`== build OK` / `== build FAILED … First errors:` line. When the user reports a failed host
build, read that file first.

### Manual (what `make` wraps)

```bash
cmake -B deps/build -S deps -DDEP_WX_GTK3=ON
make -C deps/build -j$(nproc)
cmake -B build -S . -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$PWD/deps/build/destdir/usr/local"
cmake --build build -j$(nproc)
```

Key flags: `SLIC3R_GTK=2|3` (must match how wx was built — this fork is 3),
`SLIC3R_STATIC`, `SLIC3R_GUI=no` (console-only), `SLIC3R_ASAN=ON`,
`CMAKE_BUILD_TYPE=Debug` (full assert coverage for tests), `SLIC3R_FHS=1`
(FHS install layout).

### System packages (Ubuntu / Debian)

```
git build-essential autoconf automake libtool libtool-bin cmake \
libglu1-mesa-dev libgl1-mesa-dev libegl1-mesa-dev libgtk-3-dev libdbus-1-dev \
libwebkit2gtk-4.1-dev libwayland-dev wayland-protocols libxkbcommon-dev \
texinfo libhidapi-dev
```

- `automake`/`libtool`/`libtool-bin` — needed by the autotools deps (GMP/MPFR);
  without them the deps build fails at `libtoolize`.
- `libhidapi-dev` — required since upstream 2.9.5, or the app configure fails with
  `Package 'hidapi-libusb' not found`.
- The EGL/Wayland/xkbcommon packages are the extras beyond a stock X11 build.

Full Linux/Wayland walkthrough: **`doc/How to build - Linux Wayland.md`**.

## Fork identity (version strings)

- `SLIC3R_VERSION` (`version.inc`) = upstream version, never edited by the fork.
- Fork values live in **`version_fork.inc`** (included right after `version.inc`, so upstream
  bumps never conflict): `SLIC3R_FORK_LABEL` (`ext`), `SLIC3R_FORK_ID`, `SLIC3R_FORK_URL` and a
  static `SLIC3R_BUILD_ID` (`PrusaSlicer-<version>+ext`, for Windows rc / macOS plist / SL1).
- Release / development state, branch, upstream tag + commit and the feature list are
  generated **at build time** (`cmake/modules/ForkGitVersion.cmake` → `fork_git_version.h`,
  used only by `src/libslic3r/ForkInfo.cpp`). No commit hash / dirty flag on purpose, so an
  ordinary commit never recompiles or relinks anything; tags and branch switches do. Details:
  `doc/fork/features/fork-tooling/README.md`.

## Tests

Catch2-based; build `-DCMAKE_BUILD_TYPE=Debug` to exercise asserts.

```bash
cmake --build build --target test    # or: cd build && ctest
./build/tests/fff_print/fff_print_tests "[some_tag]"
```

Test subdirs mirror the source libs: `tests/{libslic3r,fff_print,sla_print,arrange,slic3rutils,thumbnails}`.

## Architecture

Strict one-directional layering:

1. **`src/libslic3r/`** — pure slicing core, no GUI/wx. Models → G-code (FFF) or
   PNG layers (mSLA). Print logic, infill, supports, gcode emission, print config.
2. **`src/slic3r/`** — wxWidgets GUI over `libslic3r` (`GUI/`: windows, dialogs,
   `wxGLCanvas`/OpenGL 3D view, presets, webkit2gtk WebView integrations).
3. **`src/CLI/`** — thin CLI over `libslic3r`. Entry `PrusaSlicer.cpp`;
   `Setup.cpp` handles Linux WebKit/GTK env.

Supporting: `libvgcode/` (gcode preview), `clipper/` (vendored), `slic3r-arrange*`
(nesting), `occt_wrapper/` (STEP import), `platform/`. Dependencies are built by
`deps/+<name>/<name>.cmake`; the wx dep pulls `github.com/prusa3d/wxWidgets` — **the
repo to retarget when bumping wx**.

## Conventions

- Raw GTK APIs are guarded with `__WXGTK__` / `__WXGTK2__` / `__WXGTK3__`; any new
  raw GTK call must pick the right guard.
- HiDPI uses `wxBitmapBundle`, `wxDisplay::GetScaleFactor()`,
  `GetContentScaleFactor()`, `wxDPIChangedEvent` — don't reintroduce per-pixel
  bitmap paths.
- The `#undef Convex` in `GLCanvas3D.cpp` is an intentional X11 header-collision
  guard — leave it.
- Single-instance detection is D-Bus (`InstanceCheck.cpp`), Wayland-compatible —
  don't replace with X11 atoms.
- Commit style: `type(scope): summary` (`feat:`, `fix:`, `build:`, `docs(<id>):`).
  Keep each fork feature to its own branch and disjoint files where possible (see
  `doc/FORK-WORKFLOW.md`).

## Sandbox / shared-mount caveat (agents)

The host builds into `build/` + `deps/build/`. A sandbox that mounts this tree at a different
path (host `/home/paul/...`, sandbox `/home/nhp/project`) cannot use them: the deps bake
their absolute install path. **Never rewrite those paths** — that corrupts the host build
(`wx/version.h not found`). Normally the user builds on the host and the agent reads
`build/build.log`. Only for phases with many build iterations, after asking the user, use a
temporary sandbox build: `make SANDBOX=1` (`build-sandbox/` + `deps/build-sandbox/`, ~7 GB,
the first deps build takes long) and remove it afterwards with `make clean-sandbox`.
