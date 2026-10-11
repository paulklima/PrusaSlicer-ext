---
id: fork-tooling
kind: feature
title: Fork workflow, build tooling (CMake 4 / GCC 15) and build identity in About / System Info
show: true
upstream_pr: none
---

# feature/fork-tooling — fork workflow, build tooling and build identity

Fork-only. Everything that runs the fork as a project: how it is built, how it is organised,
how it follows upstream releases, and how a build shows what it is — the About / System Info
display is the visible end of the same tooling (it reads the release tags and the per-feature
doc folders), so it is one feature. Merged first into every `dev/<upstream>` line.

## Part 1 — build tooling

- `Makefile` — two-stage build wrapper: `make` (deps once into `deps/build`, then the app into
  `build`), `make run`, `make test`, `make reconfigure`, `JOBS=`; exports
  `CMAKE_POLICY_VERSION_MINIMUM=3.5` so every nested cmake works on CMake 4 (Ubuntu 26.04).
  `make SANDBOX=1 …` builds into `build-sandbox/` + `deps/build-sandbox/` instead — a temporary
  build for an agent sandbox that mounts the tree at another path (`make clean-sandbox`).
- `scripts/logrun.sh` — one log per build dir (`build/build.log`, `deps/build/build.log`),
  overwritten by every run: header (date, branch, commit, tools), output, final OK / FAILED
  line with the first errors — so a failed host build can be analysed from the log alone.
- `deps/CMakeLists.txt` — the same policy minimum in `DEP_CMAKE_OPTS` for non-make builds.
- `.gitignore` — build directories (`build/` (upstream), `build-sandbox/`, `deps/build*`).
- Warning cleanup for GCC 15 / CMake 4 (behaviour-preserving):
  - CMake 4.2 CMP0175: explicit `POST_BUILD` in `build-utils/CMakeLists.txt` (~1000 warnings),
    no-op `DEPENDS` removed from the l10n `add_custom_command(TARGET …)` calls in `CMakeLists.txt`.
  - `src/libslic3r/CMakeLists.txt`: `-Wno-overloaded-virtual` (PUBLIC) — the Cereal
    `serialize(Archive&)` hook in `Config.hpp` "hides" the virtual `ConfigOption::serialize()`,
    ~5200 false positives with GCC 13+.
  - `-Wparentheses` (`src/CLI/ProcessActions.cpp`), `-Wenum-compare` (`Plater.cpp`),
    `-Wsign-compare` (`WipeTowerIntegration.hpp`).

## Part 2 — workflow, docs, scripts

- `doc/FORK-WORKFLOW.md` — model, decision record (pros/cons), names, versions, checklists.
- `CLAUDE.md` — repo orientation + binding "Fork workflow rules" for agents.
- `README.md` — fork banner.
- `doc/How to build - Linux Wayland.md` — fork build guide.
- `doc/fork/lines/<upstream>.md` — one record per upstream line: feature decisions on that base
  and the release notes of its releases.
- `doc/fork/backlog/` — researched ideas not yet on a branch (MMU issues 3–5).
- `scripts/fork-setup.sh` — per-clone setup: `--no-ff` for `dev/*` / `fork/*`, rerere, installs
  `fork-status.sh` and `fork-bump.sh` into `<git-common-dir>/fork-tools/` (so they work on
  feature branches, which have no `scripts/`), the `git fork-bump` alias, the Claude Code
  session hook and `CLAUDE.local.md`.
- `scripts/fork-status.sh` — status report (session hook).
- `scripts/fork-bump.sh` (`git fork-bump plan|rebase|merge <version_X.Y.Z>`) — moves all
  features to a new upstream release (see `doc/FORK-WORKFLOW.md`, "Upstream release").

## Part 3 — build identity in the app

Every binary says exactly what it is: which fork release (or development state), which
upstream release it is based on and which fork features it contains.

| Shown | Source |
|---|---|
| `Version 2.9.6` | `SLIC3R_VERSION` — **stays upstream** (profile bundles, updater, 3MF, G-code producer detection depend on it) |
| `PrusaSlicer ext - fork of prusa3d/PrusaSlicer` | `SLIC3R_FORK_ID` in `version_fork.inc` |
| `https://github.com/paulklima/PrusaSlicer-ext` (clickable) | `SLIC3R_FORK_URL` in `version_fork.inc` |
| `Release v2.9.6+ext-v1.0.0` / `Release candidate …` / `Development build on <branch> (after <last tag>)` | git, at build time |
| `Based on upstream version_2.9.6 (b028299c77)` | nearest `version_*` tag (no alpha/beta/rc), at build time |
| `Extended features: mmu-temp-sync, wayland, …` | frontmatter of `doc/fork/features/*/README.md` (`show: true`), at build time |

System Info lists all fields plus every feature/fix with title and upstream-PR status (its
upper panel is sized to its content, it has no scrollbar); the About "copy" button copies
them. Build id (System Info, title bar, `prusa-slicer --help`): `PrusaSlicer-<release/rc tag>`,
or `PrusaSlicer-v2.9.6+ext-dev` for development builds.

Also: the "send system info to Prusa" dialog is disabled (fork data would skew Prusa's
statistics). The upstream update check stays on: it signals that a new upstream release
exists (= time for an upstream bump).

### How it works

- `cmake/modules/ForkGitVersion.cmake` runs on **every build** (`fork_git_version` custom
  target, dependency of `libslic3r`) and writes `<build>/src/libslic3r/fork_git_version.h`
  via `configure_file()` — only when the content changes. Tags, branch switches and feature
  folders are picked up by a plain `make`, without reconfigure; only `ForkInfo.cpp` recompiles.
- **No commit hash and no dirty flag** in the header: they change with every commit and would
  recompile + relink the app each time. A release is exact anyway (its tag); development
  builds show the branch and the last tag.
- Git is only read (`--no-optional-locks`, `-c safe.directory=*` for sandbox/other-uid builds);
  without git everything is `unknown`.
  Packagers can set `-DFORK_VERSION_OVERRIDE=<release tag>`.
- Release = HEAD exactly at a `v*+ext-v*` tag without `-rc.` and a clean tree;
  rc = exactly at an `-rc.` tag; everything else is a development build.
- `src/libslic3r/ForkInfo.{hpp,cpp}`: `fork_info()` with `id`, `summary_line()`,
  `upstream_line()`, `feature_ids()`.
- `version_fork.inc` (included after `version.inc`, so upstream version bumps never conflict):
  `SLIC3R_FORK_LABEL` (`ext`), `SLIC3R_FORK_ID`, `SLIC3R_FORK_URL`, and a static
  `SLIC3R_BUILD_ID` `PrusaSlicer-<version>+ext` (Windows rc / macOS plist / SL1). It is
  deliberately not taken from git: it is in `libslic3r_version.h`, which almost every file
  includes, so a git-based id would rebuild everything after each commit.
- A malformed feature README only gives a CMake warning and is skipped (never breaks the build).

## Files touched

Build tooling and docs/scripts listed in parts 1 and 2; code: `version_fork.inc`, `CMakeLists.txt`,
`build-utils/CMakeLists.txt`, `cmake/modules/ForkGitVersion.cmake`,
`src/libslic3r/{CMakeLists.txt,ForkInfo.hpp,ForkInfo.cpp,fork_git_version.h.in,libslic3r_version.h.in}`,
`src/libslic3r/GCode/WipeTowerIntegration.hpp`,
`src/slic3r/GUI/{AboutDialog,SysInfoDialog,MainFrame,SendSystemInfoDialog,Plater}.cpp`,
`src/CLI/{PrintHelp,ProcessActions}.cpp`.

## How to test

On `dev/<upstream>` (this branch is merged there first):

1. `make` from a fresh clone builds deps and app (CMake 4) into `deps/build` + `build`;
   `make run` starts it; `build/build.log` ends with `== build OK`; configure prints no
   CMP0175 developer warnings.
2. `scripts/fork-setup.sh` → prints the status report; a new Claude Code session shows it at
   startup; `git config --get branch.dev/<upstream>.mergeoptions` = `--no-ff`; `git fork-bump`
   works on a feature branch; `CLAUDE.local.md` exists and is git-excluded.
3. Script alone:
   `cmake -DSRC=$PWD -DTEMPLATE=src/libslic3r/fork_git_version.h.in -DOUT=/tmp/f.h -DLABEL=ext -DSLIC3R_VERSION=2.9.6 -P cmake/modules/ForkGitVersion.cmake && cat /tmp/f.h`
4. Help → About and Help → System Info: development build line, upstream line, extended
   features (System Info: all entries with titles, still complete after moving the dialog to
   a monitor with other scaling, features as an aligned id | title table), project link
   clickable in both; a new commit + `make` must not recompile or relink anything.
5. `git fork-bump plan <tag>` prints the branches, stacks and merge order without changing
   anything.

## Upstream status

Fork-only. Upstream master (3.0 alphas) already sets the CMake policy minimum and has done its
own warning rounds — re-check part 1 at the 3.x bump.

## Decision log

| Upstream base | Decision | Reason |
|---|---|---|
| version_2.9.6 | keep | fork-only; tooling, workflow, warning cleanup and build identity are one feature |
