# Building PrusaSlicer with Wayland Support on Linux

This guide covers building PrusaSlicer with native Wayland support.
The build uses GTK3 + EGL, which allows the application to run natively
on Wayland compositors (GNOME/Mutter, KDE/KWin, wlroots-based, etc.)
as well as on X11.

For general Linux build instructions, see `doc/How to build - Linux et al.md`.

---

## Prerequisites

Ubuntu 24.04 / Debian 12:

```shell
sudo apt-get install -y \
    git \
    build-essential \
    autoconf \
    cmake \
    libglu1-mesa-dev \
    libgl1-mesa-dev \
    libegl1-mesa-dev \
    libgtk-3-dev \
    libdbus-1-dev \
    libwebkit2gtk-4.1-dev \
    libwayland-dev \
    wayland-protocols \
    libxkbcommon-dev \
    texinfo
```

Compared to the standard build, the additional packages are:
`libegl1-mesa-dev`, `libgl1-mesa-dev`, `libwayland-dev`,
`wayland-protocols`, `libxkbcommon-dev`.

X11 development headers are still needed for transitive dependencies
even when targeting Wayland.

## Locale

The deps build requires a UTF-8 locale (Boost extraction fails
otherwise). If your locale is not UTF-8:

```bash
export LC_ALL=C.UTF-8
```

## Building

The steps are the same as the standard Linux build. The Wayland flags
(`DEP_WX_GTK3=ON`, `wxUSE_GLCANVAS_EGL=ON`, `GLEW_EGL=ON`) are now
the defaults on Linux — no extra flags needed.

### 1. Clone

```bash
git clone https://github.com/paulklima/PrusaSlicer-ext
cd PrusaSlicer-ext
git checkout fork/2.9.6   # released state; or a release tag 'v2.9.6+ext-v<x.y.z>', or dev/2.9.6 (in test)
# see doc/FORK-WORKFLOW.md for branches, tags and versions
```

### 2. Quick start — the `Makefile` wrapper (recommended)

The repo ships a top-level `Makefile` that drives the whole two-stage build and
handles the CMake-4 / Ubuntu-26.04 policy shim for you. From a fresh clone:

```bash
make          # builds static deps (once) into deps/build, then the app into build
make run      # launches build/src/prusa-slicer
make test     # ctest
```

Override parallelism with `make JOBS=8`. Each run overwrites the log of its build dir:
`build/build.log` (deps: `deps/build/build.log`), ending with `== build OK` or
`== build FAILED` plus the first errors.
Everything below (§3–§4) is what `make` runs under the hood — use it if you need
a non-default configuration.

### 3. Build dependencies manually

```bash
cmake -B deps/build -S deps -DDEP_WX_GTK3=ON
make -C deps/build -j$(nproc)
```

This builds wxWidgets 3.3.3 with GTK3 + EGL and GLEW with EGL support
automatically.

**Warning:** the dependency destdir is **not relocatable** — wxWidgets and other
deps hardcode the absolute install path into their pkg-config / CMake config
files. If you move or re-mount the tree at a different path, you must rebuild the
deps (or the app configure fails with `wx/version.h not found`).

### 4. Build PrusaSlicer manually

```bash
cmake -B build -S . -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PWD/deps/build/destdir/usr/local"
cmake --build build -j$(nproc)
```

On Ubuntu 26.04 (CMake 4) prefix the configure/build with
`CMAKE_POLICY_VERSION_MINIMUM=3.5` (the `Makefile` exports this for you).

> **Note:** `Release` is the default build type when none is specified,
> but passing it explicitly avoids surprises.

### 4b. Run (from the build tree)

```bash
./build/src/prusa-slicer      # or: make run
```

GTK3 auto-detects the display backend. On a Wayland session it will use
Wayland natively; on X11 it will use X11.

To force a specific backend:

```bash
GDK_BACKEND=wayland ./prusa-slicer    # force Wayland
GDK_BACKEND=x11 ./prusa-slicer        # force X11 (XWayland)
```

PrusaSlicer follows the system GTK theme. To use dark mode:

```bash
GTK_THEME=Adwaita:dark ./prusa-slicer
```

### 5. Install

There are two installation methods: **portable** (default) and **FHS**
(system-wide). Both are run from the `build/` directory.

#### Manual install

Copy the binary and resources directory to the target location. The
binary looks for `../resources/` relative to itself, so the layout
must be:

```
/opt/prusa/
├── bin/
│   └── prusa-slicer
└── resources/
```

```bash
sudo mkdir -p /opt/prusa/bin
sudo cp src/prusa-slicer /opt/prusa/bin/
sudo cp -r ../resources /opt/prusa/resources
```

Optionally add it to your `PATH`:

```bash
sudo ln -s /opt/prusa/bin/prusa-slicer /usr/local/bin/prusa-slicer
```

#### Desktop entry

A desktop file is provided at
`src/platform/unix/PrusaSlicer-wayland.desktop`. It points to the
`/opt/prusa/` install location. To install it:

```bash
sudo cp src/platform/unix/PrusaSlicer-wayland.desktop \
    /usr/local/share/applications/PrusaSlicer.desktop
sudo cp src/platform/unix/PrusaGcodeviewer-wayland.desktop \
    /usr/local/share/applications/PrusaGcodeviewer.desktop
```

This makes PrusaSlicer and the G-code viewer appear in your application
launcher. Icons are loaded directly from `/opt/prusa/resources/icons/`.

If you installed to a different prefix, edit the `Exec=` and `Icon=`
paths in both desktop files accordingly.

#### FHS install (system-wide)

For a standard Linux directory layout (`/usr/local/bin`,
`/usr/local/share`, etc.), rebuild with `-DSLIC3R_FHS=1`.
This flag is compile-time — the resource path gets baked into the
binary, so you must set it **before** building, not just at install
time.

```bash
# Back in the build/ directory:
cmake .. -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DCMAKE_BUILD_TYPE=Release \
    -DSLIC3R_FHS=1 \
    -DCMAKE_PREFIX_PATH=$(pwd)/../deps/build/destdir/usr/local
make -j$(nproc)
sudo cmake --install .
```

This installs:

| What | Where |
|------|-------|
| Binary | `/usr/local/bin/prusa-slicer` |
| Symlink | `/usr/local/bin/prusa-gcodeviewer` → `prusa-slicer` |
| Resources | `/usr/local/share/PrusaSlicer/` |
| Desktop files | `/usr/local/share/applications/` |
| App icons | `/usr/local/share/icons/hicolor/{32,128,192}x{...}/apps/` |
| udev rules | `/usr/local/lib/udev/rules.d/` |

To install to a different prefix (e.g. `/usr`), add
`-DCMAKE_INSTALL_PREFIX=/usr` at configure time, then
`sudo cmake --install .`.

#### FHS staged install (no root required)

If you are building inside a container or CI environment where you
cannot install directly to `/usr/local`, use `DESTDIR` to stage the
install into a local directory:

```bash
# Build with FHS as above, then:
DESTDIR=$(pwd)/staging cmake --install .
```

This creates the full FHS tree under `staging/`:

```
build/staging/
└── usr/
    └── local/
        ├── bin/
        │   ├── prusa-slicer
        │   └── prusa-gcodeviewer -> prusa-slicer
        ├── lib/
        │   └── udev/rules.d/...
        └── share/
            ├── PrusaSlicer/
            │   ├── icons/
            │   ├── localization/
            │   ├── profiles/
            │   ├── shaders/
            │   └── ...
            ├── applications/
            │   ├── PrusaSlicer.desktop
            │   └── PrusaGcodeviewer.desktop
            └── icons/hicolor/...
```

Copy `staging/usr/local/*` to `/usr/local/` on the target system.
The binary has `/usr/local/share/PrusaSlicer/` baked in at compile
time, so the paths must match.

> **Important:** `SLIC3R_FHS` and non-FHS builds are **not
> interchangeable**. A binary built without `SLIC3R_FHS` will not find
> resources at `/usr/local/share/PrusaSlicer/`, and vice versa.

## What the Wayland changes do

The changes live on `feature/wayland` (+ `feature/wx3.3`), merged into the
`dev/<version>` / `fork/<version>` lines (see `doc/FORK-WORKFLOW.md` and `doc/fork/features/wayland/`).
None of them affect non-Linux platforms.

| Change | File | Purpose |
|--------|------|---------|
| GTK3 default | `deps/+wxWidgets/wxWidgets.cmake` | GTK3 has a native Wayland backend; GTK2 does not |
| EGL enabled | `deps/+wxWidgets/wxWidgets.cmake` | OpenGL on Wayland requires EGL (GLX is X11-only) |
| GLEW EGL | `deps/+GLEW/GLEW.cmake` | GLEW must use `eglGetProcAddress` under EGL contexts |
| gtk-menu-images guard | `src/slic3r/GUI/GUI_App.cpp` | Property removed in GTK 3.10; causes runtime warnings |
| GDK_BACKEND removal | `src/CLI/Setup.cpp` | Was forcing X11 even when Wayland was available |
| WAYLAND_DISPLAY check | `src/CLI/GuiParams.cpp` | Pure Wayland sessions have no DISPLAY variable |
| GMP -std=gnu11 | `deps/+GMP/GMP.cmake` | Build fix for GCC 13+ |

## Troubleshooting

### `Unable to init glew library: Unknown error` + segfault

GLEW was not built with EGL support. Rebuild deps from scratch — the
`GLEW_EGL=ON` flag is now set automatically on Linux.

### `Pathname cannot be converted from UTF-8 to current locale`

Set `LC_ALL=C.UTF-8` before running the deps build. See Locale section
above.

### `DISPLAY not set, GUI mode not available`

You are on a pure Wayland session without XWayland. Make sure you have
the `WAYLAND_DISPLAY` fix from the `feature/wayland` branch
(`src/CLI/GuiParams.cpp`).

### `Gtk-CRITICAL: gtk_window_resize: assertion 'width > 0' failed`

This is a harmless GTK3 warning on first launch (fresh config). It does
not affect functionality.

### Resources directory not found (silent exit, code 0)

When running from the build directory, PrusaSlicer looks for
`../resources` relative to the binary. If you see a silent exit with
no error, check that `build/resources` is a valid symlink:

```bash
ls -la build/resources
# If broken, fix with:
ln -sfn ../resources build/resources
```

## Known limitations

- wxWidgets has been upgraded to **upstream 3.3.3** (from the Prusa fork of
  3.2.6), which brings the Wayland/EGL fixes. See
  `doc/fork/features/wx3.3/` for the migration analysis and
  `feature/wx3.3` for the API-compat commits.
- The `WEBKIT_DISABLE_COMPOSITING_MODE` and `WEBKIT_DISABLE_DMABUF_RENDERER`
  env vars are still set in `src/CLI/Setup.cpp` as workarounds for
  WebKitGTK compositor issues. These may no longer be needed on modern
  systems.
