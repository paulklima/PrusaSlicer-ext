# Fork identity (doc/fork/features/fork-tooling), included by CMakeLists right after
# version.inc. Kept in its own file so upstream version bumps of version.inc never conflict.
#
# SLIC3R_FORK_LABEL: label in our release tags v<upstream>+<label>-v<our semver>, e.g.
#                    v2.9.6+ext-v1.0.0.
# SLIC3R_FORK_ID:    human label in About / System Info (ASCII).
# SLIC3R_FORK_URL:   project page linked in About / System Info.
# SLIC3R_VERSION stays the upstream version. Our version, commit, upstream base and the
# feature list are generated at build time (cmake/modules/ForkGitVersion.cmake).
set(SLIC3R_FORK_LABEL "ext")
set(SLIC3R_FORK_ID "PrusaSlicer ext - fork of prusa3d/PrusaSlicer")
set(SLIC3R_FORK_URL "https://github.com/paulklima/PrusaSlicer-ext")

# Static build id (Windows rc / macOS plist / SL1 archives). Deliberately not derived from
# git: it ends up in libslic3r_version.h, which almost every file includes.
if (SLIC3R_BUILD_ID MATCHES "UNKNOWN")
    set(SLIC3R_BUILD_ID "${SLIC3R_APP_NAME}-${SLIC3R_VERSION}+${SLIC3R_FORK_LABEL}")
endif ()
