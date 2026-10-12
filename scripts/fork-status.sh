#!/usr/bin/env bash
# Fork workflow status (doc/FORK-WORKFLOW.md). Read-only and fast; never fails.
# Installed by scripts/fork-setup.sh into <git-common-dir>/fork-tools/ and run as a
# Claude Code SessionStart hook, so its output lands in the agent's context.
set +e
g() { git -c core.fsmonitor=false -c safe.directory='*' --no-optional-locks "$@" 2>/dev/null; }
g rev-parse --is-inside-work-tree >/dev/null || exit 0

say() { echo "$*"; }
cap() { # cap <list...>: first 10 words, then "+N"
    local n=$#; set -- "${@:1:10}"; echo -n "$*"; [ "$n" -gt 10 ] && echo -n " +$((n - 10))"; echo
}
base_of() { g describe --tags --abbrev=0 --match 'version_*' --exclude '*-alpha*' --exclude '*-beta*' --exclude '*-rc*' "$1"; }
has() { g rev-parse -q --verify "refs/heads/$1" >/dev/null; }

say "== Fork status (doc/FORK-WORKFLOW.md) =="

cur=$(g symbolic-ref --short -q HEAD)
case "$cur" in
    "")         kind="DETACHED at $(g describe --tags --always --match 'v*+*-v*' HEAD)" ;;
    dev/*)      kind="development line - features are merged here (--no-ff) and tested; build here" ;;
    fork/*)     kind="RELEASE line - do not work here; only 'merge --no-ff dev/<version>' on the user's release decision" ;;
    feature/*)  kind="feature branch" ;;
    fix/*)      kind="fix branch" ;;
    pr/*)       kind="upstream-PR head - code only, never merge into dev/fork" ;;
    *)          kind="UNMANAGED - not part of the fork workflow" ;;
esac
say "Branch: ${cur:-(detached)} [$kind] base: $(base_of HEAD)"
[ -n "$(g status --porcelain --untracked-files=no | head -1)" ] && say "Working tree has uncommitted changes."

devs=$(g for-each-ref --sort=-version:refname --format='%(refname:short)' refs/heads/dev/)
dev=$(echo "$devs" | head -1)
if [ -z "$dev" ]; then
    say "No dev/* line exists -> see doc/FORK-WORKFLOW.md."
    exit 0
fi
ver=${dev#dev/}
rel=$(g tag -l --sort=-version:refname "v$ver+*-v*" | grep -v -- '-rc\.' | head -1)
say "Lines: $dev (newest), fork/$ver $(has "fork/$ver" || echo '(missing)'); last release for $ver: ${rel:-none}"

if [ "$(g rev-parse --is-shallow-repository)" = "true" ]; then
    say "Shallow clone: merge checks skipped."
else
    if has "fork/$ver"; then
        n=$(g rev-list --count --merges --first-parent "fork/$ver..$dev")
        [ "${n:-0}" -gt 0 ] && say "Not released yet: $n merge(s) on $dev that are not in fork/$ver (release only on the user's decision)."
    fi
    unmerged=(); nodocs=(); otherbase=()
    for ref in $(g for-each-ref --format='%(refname)' refs/heads/feature refs/heads/fix); do
        b=${ref#refs/heads/}; id=${b#*/}
        base=$(base_of "$ref")
        [ "${base#version_}" = "$ver" ] || { otherbase+=("$b(${base:-no base})"); continue; }
        [ "$(g rev-list --count "$base..$ref")" = 0 ] && continue   # new, still empty branch
        if g merge-base --is-ancestor "$ref" "$dev"; then
            g cat-file -e "$dev:doc/fork/features/$id/README.md" ||
                g grep -q -F "\`$b\`" "$dev" -- doc/fork/features || nodocs+=("$b")
        else
            unmerged+=("$b(+$(g rev-list --count "$dev..$ref"))")
        fi
    done
    orphan=()
    for id in $(g ls-tree -d --name-only "$dev" doc/fork/features/ | sed 's#.*/##'); do
        has "feature/$id" || has "fix/$id" || orphan+=("$id")
    done
    [ ${#unmerged[@]} -gt 0 ]  && say "Not merged into $dev (in work / in test?): $(cap "${unmerged[@]}")"
    [ ${#nodocs[@]} -gt 0 ]    && say "Merged without doc/fork/features/<id>/: $(cap "${nodocs[@]}")"
    [ ${#orphan[@]} -gt 0 ]    && say "Doc folders on $dev without a branch: $(cap "${orphan[@]}")"
    [ ${#otherbase[@]} -gt 0 ] && say "Branches on another upstream base: $(cap "${otherbase[@]}")"
fi

missing=()
for l in $(g for-each-ref --format='%(refname:short)' refs/heads/dev/ refs/heads/fork/); do
    case " $(g config --get-all "branch.$l.mergeoptions") " in *--no-ff*) ;; *) missing+=("branch.$l.mergeoptions") ;; esac
done
[ "$(g config --get rerere.enabled)" = "true" ] || missing+=(rerere.enabled)
[ ${#missing[@]} -gt 0 ] && say "Setup missing: $(cap "${missing[@]}") -> run scripts/fork-setup.sh"

say "AGENT: work only on feature/<id> or fix/<id> (ask the user which, and the <id>, before creating one)."
say "  Merge into dev/* with --no-ff for testing; a change request from the user = test failed, keep fixing."
say "  Merge dev/* into fork/* and tag ONLY when the user says 'release'. Never push (the user pushes)."
exit 0
