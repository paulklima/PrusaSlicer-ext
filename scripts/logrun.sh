#!/usr/bin/env bash
# Build log helper for the Makefile: one log file per build directory, overwritten by
# every `make` run, so the last build can always be read (also by an agent) in full.
#
#   logrun.sh new  <log> <title>          start a new log (header: date, branch, commit, tools)
#   logrun.sh step <log> <name> <cmd...>  run cmd, output to terminal and log, end with
#                                         "== <name> OK" or "== <name> FAILED" + first errors
set -uo pipefail

mode=$1 log=$2 title=$3
shift 3

case "$mode" in
new)
    mkdir -p "$(dirname "$log")"
    {
        echo "== $title - $(date '+%F %T')"
        g() { git -c safe.directory='*' --no-optional-locks "$@" 2>/dev/null; }
        changes=$(g status --porcelain --untracked-files=no | head -1)
        echo "branch: $(g rev-parse --abbrev-ref HEAD)  commit: $(g rev-parse --short HEAD)${changes:+  (uncommitted changes)}"
        echo "cmake: $(cmake --version | head -1 | sed 's/cmake version //')  gcc: $(gcc -dumpfullversion 2>/dev/null)  host: $(hostname)"
        echo
    } > "$log"
    ;;
step)
    mkdir -p "$(dirname "$log")"
    echo "== $title: $*" >> "$log"
    "$@" 2>&1 | tee -a "$log"
    status=${PIPESTATUS[0]}
    warnings=$(grep -v '^== ' "$log" | grep -c 'warning:')
    if [ "$status" -eq 0 ]; then
        echo "== $title OK ($warnings warnings in log)" | tee -a "$log"
    else
        {
            echo "== $title FAILED (exit $status, $warnings warnings in log). First errors:"
            grep -v '^== ' "$log" | grep -m 15 -E 'error:|Error [0-9]+|CMake Error|fatal'
        } | tee -a "$log"
    fi
    exit "$status"
    ;;
*)
    echo "usage: logrun.sh new|step <log> <title> [cmd...]" >&2
    exit 2
    ;;
esac
