# Fork build provenance, run at BUILD time:
#   cmake -DSRC=<source root> -DTEMPLATE=<template> -DOUT=<header> -DLABEL=<fork label>
#         [-DGIT=<git executable>] [-DSLIC3R_VERSION=<x.y.z>] [-DOVERRIDE=<release tag>]
#         -P ForkGitVersion.cmake
#
# Writes OUT via configure_file(), which only touches the file when its content changes,
# so a build without new commits/tags/feature folders recompiles nothing.
# Git is only read (no index refresh / lock files), so it is safe to run while git is used.
# The header holds no commit hash / dirty flag, so ordinary commits do not relink the app.
# See doc/fork/features/fork-tooling/README.md.

cmake_minimum_required(VERSION 3.13)

foreach (_var SRC TEMPLATE OUT LABEL)
    if (NOT DEFINED ${_var})
        message(FATAL_ERROR "ForkGitVersion.cmake: -D${_var}= is required")
    endif ()
endforeach ()
if (NOT GIT)
    set(GIT git)
endif ()

# Run a read-only git command in SRC; result in out_var, empty on any failure.
function(fork_git out_var)
    execute_process(
        COMMAND "${GIT}" -c safe.directory=* -c core.fsmonitor=false --no-optional-locks -C "${SRC}" ${ARGN}
        OUTPUT_VARIABLE _out
        ERROR_QUIET
        RESULT_VARIABLE _rc
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (_rc EQUAL 0)
        set(${out_var} "${_out}" PARENT_SCOPE)
    else ()
        set(${out_var} "" PARENT_SCOPE)
    endif ()
endfunction()

set(_pattern "v*+${LABEL}-v*")

# Only values that change on a branch switch, a tag or an upstream bump go into the header:
# no commit hash, no dirty flag - otherwise every commit would recompile and relink the app.
set(FORK_LABEL "${LABEL}")
set(FORK_TAG "")
set(FORK_LAST_TAG "")
set(FORK_BRANCH "unknown")
set(FORK_UPSTREAM_TAG "unknown")
set(FORK_UPSTREAM_COMMIT "unknown")
set(FORK_IS_RELEASE 0)
set(FORK_IS_RC 0)

fork_git(_head rev-parse --verify -q HEAD)
if (OVERRIDE)
    # Packager / tarball build without .git: trust the given release tag.
    set(FORK_TAG "${OVERRIDE}")
    set(FORK_LAST_TAG "${OVERRIDE}")
    if (OVERRIDE MATCHES "-rc\\.")
        set(FORK_IS_RC 1)
    else ()
        set(FORK_IS_RELEASE 1)
    endif ()
elseif (_head)
    fork_git(_branch rev-parse --abbrev-ref HEAD)
    if (_branch STREQUAL "HEAD")
        set(FORK_BRANCH "detached")
    elseif (_branch)
        set(FORK_BRANCH "${_branch}")
    endif ()

    fork_git(_at_tag describe --tags --exact-match --match "${_pattern}" HEAD)
    fork_git(_last   describe --tags --abbrev=0 --match "${_pattern}" HEAD)
    set(FORK_LAST_TAG "${_last}")
    if (_at_tag)
        # Exactly at a release / rc tag: only a clean checkout counts as that release.
        fork_git(_status status --porcelain --untracked-files=no)
        if (NOT _status)
            fork_git(_release describe --tags --exact-match --match "${_pattern}" --exclude "*-rc.*" HEAD)
            if (_release)
                set(FORK_TAG "${_release}")
                set(FORK_IS_RELEASE 1)
            else ()
                set(FORK_TAG "${_at_tag}")
                set(FORK_IS_RC 1)
            endif ()
        endif ()
    endif ()

    fork_git(_up describe --tags --abbrev=0 --match "version_*"
        --exclude "*-alpha*" --exclude "*-beta*" --exclude "*-rc*" HEAD)
    if (_up)
        set(FORK_UPSTREAM_TAG "${_up}")
        fork_git(_up_commit rev-parse --short=10 "${_up}^{commit}")
        if (_up_commit)
            set(FORK_UPSTREAM_COMMIT "${_up_commit}")
        endif ()
        if (SLIC3R_VERSION AND NOT _up STREQUAL "version_${SLIC3R_VERSION}")
            message(WARNING "Fork: nearest upstream tag ${_up} does not match SLIC3R_VERSION ${SLIC3R_VERSION}")
        endif ()
    endif ()
endif ()

# Build id and our own semver: release/rc tag (v2.9.6+ext-v1.1.0 -> 1.1.0), else a dev id.
set(FORK_VERSION "")
if (FORK_IS_RELEASE OR FORK_IS_RC)
    set(FORK_ID "${FORK_TAG}")
    if (FORK_TAG MATCHES "\\+${LABEL}-v(.+)$")
        set(FORK_VERSION "${CMAKE_MATCH_1}")
    endif ()
else ()
    set(FORK_ID "v${SLIC3R_VERSION}+${LABEL}-dev")
endif ()

# Feature manifest: frontmatter of doc/fork/features/<id>/README.md, one folder per topic branch.
set(FORK_FEATURES_INIT "")
file(GLOB _readmes LIST_DIRECTORIES false "${SRC}/doc/fork/features/*/README.md")
list(SORT _readmes)
foreach (_readme IN LISTS _readmes)
    get_filename_component(_dir "${_readme}" DIRECTORY)
    get_filename_component(_folder "${_dir}" NAME)
    file(STRINGS "${_readme}" _lines LIMIT_COUNT 20 ENCODING UTF-8)
    foreach (_key id kind title show upstream_pr)
        set(_fm_${_key} "")
    endforeach ()
    set(_in_fm 0)
    foreach (_line IN LISTS _lines)
        if (_line STREQUAL "---")
            if (_in_fm)
                break()
            endif ()
            set(_in_fm 1)
        elseif (_in_fm AND _line MATCHES "^([a-z_]+): *(.*)$")
            set(_fm_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif ()
    endforeach ()
    # A doc slip must not break the build: warn and skip (scripts/fork-status.sh reports it too).
    foreach (_key title upstream_pr)
        if (_fm_${_key} MATCHES "^\"(.*)\"$")
            set(_fm_${_key} "${CMAKE_MATCH_1}")
        elseif (_fm_${_key} MATCHES "^'(.*)'$")
            set(_fm_${_key} "${CMAKE_MATCH_1}")
        endif ()
    endforeach ()
    if (NOT _fm_id STREQUAL _folder)
        message(WARNING "Fork feature manifest ${_readme}: 'id: ${_fm_id}' must equal the folder name '${_folder}' - skipped")
        continue()
    endif ()
    if (NOT _fm_kind MATCHES "^(feature|fix|tooling)$")
        message(WARNING "Fork feature manifest ${_readme}: 'kind' must be feature, fix or tooling - skipped")
        continue()
    endif ()
    if ("${_fm_title}${_fm_upstream_pr}" MATCHES "[\"\\\\<>&]")
        message(WARNING "Fork feature manifest ${_readme}: values must not contain quotes, backslashes, < > & - skipped")
        continue()
    endif ()
    if (_fm_show STREQUAL "true")
        set(_show true)
    else ()
        set(_show false)
    endif ()
    string(APPEND FORK_FEATURES_INIT
        " \\\n    { \"${_fm_id}\", \"${_fm_kind}\", \"${_fm_title}\", \"${_fm_upstream_pr}\", ${_show} },")
endforeach ()

configure_file("${TEMPLATE}" "${OUT}" @ONLY)
