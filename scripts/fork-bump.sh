#!/usr/bin/env bash
# Move all fork features to a new upstream release (doc/FORK-WORKFLOW.md, "Upstream release").
# Installed by scripts/fork-setup.sh into <git-common-dir>/fork-tools/ and called as
#   git fork-bump plan   <version_X.Y.Z> [--skip <branch>]... [--rework <branch>]...
#   git fork-bump rebase <version_X.Y.Z> [--skip <branch>]... [--rework <branch>]...
#   git fork-bump merge  <version_X.Y.Z> [--skip <branch>]...
# plan   changes nothing: old line, branches, stacks, merge order.
# rebase moves every feature/* and fix/* of the old line onto the new tag (stacks from their
#        top with --update-refs), keeps the old tips as refs/fork-bump/<X.Y.Z>/<branch>, prints
#        a range-diff per branch. --skip leaves a branch behind (dropped feature); --rework
#        restarts it from the new tag with only its README commit (re-implement later).
# merge  creates dev/<X.Y.Z> and fork/<X.Y.Z> from the tag and merges the branches --no-ff in
#        the order of the old dev line (new branches appended).
# Stateless: everything is derived from refs, so after a conflict you finish the git operation
# and simply run the same command again.
set -euo pipefail

die()  { echo "fork-bump: $*" >&2; exit 1; }
say()  { echo "$*"; }
g()    { git "$@"; }
sha()  { g rev-parse --short=10 "$1"; }
anc()  { g merge-base --is-ancestor "$1" "$2" 2>/dev/null; }   # anc A B: A is ancestor of B
has()  { g rev-parse -q --verify "$1" >/dev/null; }
inlist() { local x=$1; shift; local e; for e in "$@"; do [ "$e" = "$x" ] && return 0; done; return 1; }

usage() { sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

cmd=${1:-}; [ -n "$cmd" ] || usage 1
case "$cmd" in plan|rebase|merge) ;; -h|--help|help) usage 0 ;; *) usage 1 ;; esac
shift
new=${1:-}; [ -n "$new" ] || die "missing <version_X.Y.Z>"
shift
skip=(); rework=()
while [ $# -gt 0 ]; do
    case "$1" in
        --skip)   [ $# -ge 2 ] || die "--skip needs a branch"; skip+=("$2"); shift 2 ;;
        --rework) [ $# -ge 2 ] || die "--rework needs a branch"; rework+=("$2"); shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ "$cmd" = merge ] && [ ${#rework[@]} -gt 0 ] && die "--rework belongs to 'rebase'"

# --- the new upstream tag: stable releases only (alpha/beta/rc are not a base) ---
[[ "$new" =~ ^version_([0-9]+\.[0-9]+\.[0-9]+)$ ]] || die "'$new' is not a stable upstream tag version_X.Y.Z"
newver=${BASH_REMATCH[1]}
if ! has "refs/tags/$new"; then
    say "Tag $new not found locally, fetching upstream tags ..."
    g fetch -q upstream --tags || die "git fetch upstream --tags failed"
    has "refs/tags/$new" || die "tag $new does not exist upstream"
fi
newc=$(g rev-parse "$new^{commit}")

# --- the old line: newest dev/* other than the new one ---
olddev=$(g for-each-ref --sort=-version:refname --format='%(refname:short)' refs/heads/dev/ |
         grep -vxF "dev/$newver" | head -1 || true)
[ -n "$olddev" ] || { has "refs/heads/dev/$newver" && die "$new is already the base of dev/$newver"; }
[ -n "$olddev" ] || die "no dev/* line found (fresh clone? run scripts/fork-setup.sh first)"
oldver=${olddev#dev/}
old="version_$oldver"
has "refs/tags/$old" || die "tag $old of $olddev not found"
oldc=$(g rev-parse "$old^{commit}")
[ "$oldc" != "$newc" ] || die "$new is already the base of $olddev"
anc "$newc" "$oldc" && die "$new is older than $old (base of $olddev)"
anc "$oldc" "$newc" || say "Note: $new does not contain $old (not a newer release on the same history?)"

for b in "${skip[@]}" "${rework[@]}"; do has "refs/heads/$b" || die "no branch '$b'"; done
for b in "${rework[@]}"; do inlist "$b" "${skip[@]}" && die "'$b' is both --skip and --rework"; done

# --- classify feature/* and fix/* ---
todo=(); done_=(); other=()
while read -r b; do
    [[ "$b" =~ -[0-9]+\.[0-9]+\.[0-9]+$ ]] && { other+=("$b(old-line fix)"); continue; }
    inlist "$b" "${skip[@]}" && continue
    if anc "$newc" "refs/heads/$b"; then done_+=("$b")
    elif anc "$oldc" "refs/heads/$b"; then todo+=("$b")
    else other+=("$b"); fi
done < <(g for-each-ref --format='%(refname:short)' refs/heads/feature/ refs/heads/fix/)

# below <A> <B>: B is stacked on A (A has own commits and is an ancestor of B)
below() { [ "$1" != "$2" ] && [ "$(g rev-parse "refs/heads/$1")" != "$oldc" ] && anc "refs/heads/$1" "refs/heads/$2"; }

# tops: branches of the old line that no other old-line branch is stacked on
tops=()
for b in "${todo[@]}"; do
    t=1; for c in "${todo[@]}"; do below "$b" "$c" && { t=0; break; }; done
    [ $t = 1 ] && tops+=("$b")
done
# a base shared by two tops would be rebased twice -> refuse
for b in "${todo[@]}"; do
    n=0; for t in "${tops[@]}"; do below "$b" "$t" && n=$((n + 1)); done
    [ $n -le 1 ] || die "$b is the base of $n stacked branches - rebase them by hand (doc/FORK-WORKFLOW.md)"
done
for b in "${rework[@]}"; do   # only stand-alone branches (stacks: by hand)
    inlist "$b" "${done_[@]}" && continue   # already reworked (re-run)
    inlist "$b" "${todo[@]}" || die "--rework $b: not a branch of $olddev"
    for t in "${todo[@]}"; do
        if below "$b" "$t" || below "$t" "$b"; then die "--rework $b: part of a stack with $t - rework stacks by hand (or --skip $t)"; fi
    done
done

# --- merge order: replay the old dev line, stack bottoms first, new branches appended ---
kept=("${todo[@]}" "${done_[@]}")
order=()
add() { inlist "$1" "${order[@]}" || order+=("$1"); }
add_with_bases() {   # bases (in stack order) before the branch itself
    local b=$1 c
    for c in "${kept[@]}"; do below "$c" "$b" && ! inlist "$c" "${order[@]}" && add_with_bases "$c"; done
    add "$b"
}
while read -r subj; do
    if [[ "$subj" =~ ^Merge\ (branch\ \')?([^\ \']+) ]]; then
        b=${BASH_REMATCH[2]}
        inlist "$b" "${kept[@]}" && add_with_bases "$b"
    fi
done < <(g log --first-parent --merges --reverse --format=%s "$olddev")
inlist feature/fork-tooling "${kept[@]}" && [ ${#order[@]} -eq 0 ] && add feature/fork-tooling
for b in "${kept[@]}"; do add_with_bases "$b"; done

report() {
    say "== fork-bump $cmd: $olddev ($old) -> $new"
    say "Rebase (old base): ${todo[*]:-none}"
    [ ${#tops[@]} -gt 0 ] && say "  rebased from the top (--update-refs): ${tops[*]}"
    [ ${#rework[@]} -gt 0 ] && say "  rework (README only on $new): ${rework[*]}"
    [ ${#done_[@]} -gt 0 ] && say "Already on $new: ${done_[*]}"
    [ ${#skip[@]} -gt 0 ] && say "Skipped (dropped for $newver): ${skip[*]}"
    [ ${#other[@]} -gt 0 ] && say "Ignored (other base): ${other[*]}"
    say "Merge order for dev/$newver: ${order[*]:-none}"
}

clean_or_die() {
    local gd; gd=$(g rev-parse --git-dir)
    for p in rebase-merge rebase-apply MERGE_HEAD CHERRY_PICK_HEAD REVERT_HEAD; do
        [ -e "$gd/$p" ] && die "a git operation is in progress ($p): finish it (git rebase --continue / git commit) or abort it, then run this command again"
    done
    [ -z "$(g status --porcelain --untracked-files=no)" ] || die "working tree has uncommitted changes"
}

start=$(g symbolic-ref --short -q HEAD || g rev-parse HEAD)
back() { g switch -q "$start" 2>/dev/null || g switch -q --detach "$start"; }

case "$cmd" in
plan)
    report
    say "Next: git fork-bump rebase $new ${skip[*]/#/--skip } ${rework[*]/#/--rework }"
    ;;

rebase)
    clean_or_die
    report
    for b in "${todo[@]}"; do   # remember the old tips once
        has "refs/fork-bump/$newver/$b" || g update-ref "refs/fork-bump/$newver/$b" "refs/heads/$b"
    done
    for b in "${rework[@]}"; do
        inlist "$b" "${done_[@]}" && continue   # reworked by an earlier run
        first=$(g rev-list --reverse "$oldc..refs/heads/$b" | head -1)
        [ -n "$first" ] || die "--rework $b: branch has no commits"
        say "-- rework $b: $new + $(g log -1 --format=%s "$first")"
        g switch -q "$b" && g reset -q --hard "$newc" && g cherry-pick "$first" >/dev/null ||
            die "rework of $b stopped - resolve, 'git cherry-pick --continue', run this command again"
    done
    for t in "${tops[@]}"; do
        inlist "$t" "${rework[@]}" && continue
        say "-- rebase $t onto $new"
        if ! g rebase -q --update-refs --onto "$newc" "$oldc" "$t"; then
            echo
            say "CONFLICT while rebasing $t (rerere may already have resolved parts of it)."
            say "  resolve the files, 'git add' them, 'git rebase --continue'  (or 'git rebase --abort')"
            say "  then run again: git fork-bump rebase $new ${skip[*]/#/--skip } ${rework[*]/#/--rework }"
            exit 1
        fi
    done
    back
    say
    say "== What changed per branch (old tip -> new tip; '<' = dropped, e.g. absorbed upstream)"
    for b in "${todo[@]}" "${done_[@]}"; do   # also the ones rebased by an earlier run
        o="refs/fork-bump/$newver/$b"
        has "$o" || continue
        ob=$oldc; nb=$newc; on=""        # stacked: compare only the branch's own commits
        for c in "${todo[@]}" "${done_[@]}"; do
            oc="refs/fork-bump/$newver/$c"
            if [ "$c" != "$b" ] && has "$oc" && [ "$(g rev-parse "$oc")" != "$oldc" ] &&
               anc "$oc" "$o" && anc "$ob" "$oc"; then ob=$oc; nb="refs/heads/$c"; on=" (on $c)"; fi
        done
        n=$(g rev-list --count "$nb..refs/heads/$b")
        say "-- $b$on: $(g rev-list --count "$ob..$o") -> $n commit(s)"
        g --no-pager range-diff -s "$ob..$o" "$nb..refs/heads/$b" || true
        [ "$n" -le 1 ] && ! inlist "$b" "${rework[@]}" &&
            say "   only the README commit is left: upstream absorbed it? -> decide drop (--skip) or keep"
    done
    say
    say "Next: decisions per feature (README decision log, doc/fork/lines/$newver.md on"
    say "feature/fork-tooling), then: git fork-bump merge $new ${skip[*]/#/--skip }"
    say "Old tips: refs/fork-bump/$newver/* (delete after the release of $newver:"
    say "  git for-each-ref --format='delete %(refname)' refs/fork-bump/$newver | git update-ref --stdin)"
    ;;

merge)
    clean_or_die
    report
    for b in "${todo[@]}"; do inlist "$b" "${order[@]}" && die "$b is still on $old - run 'git fork-bump rebase $new' first"; done
    dev="dev/$newver"; fork="fork/$newver"
    if has "refs/heads/$dev"; then
        anc "$newc" "refs/heads/$dev" || die "$dev exists but is not based on $new"
    else
        g branch "$dev" "$newc"
    fi
    has "refs/heads/$fork" || g branch "$fork" "$newc"
    g config "branch.$dev.mergeoptions" --no-ff
    g config "branch.$fork.mergeoptions" --no-ff
    g switch -q "$dev"
    for b in "${order[@]}"; do
        if anc "refs/heads/$b" "refs/heads/$dev"; then say "-- $b: already merged"; continue; fi
        say "-- merge $b"
        if ! g merge -q --no-ff -m "Merge $b @$(sha "refs/heads/$b") (base $new)" "refs/heads/$b"; then
            echo
            say "CONFLICT while merging $b into $dev."
            say "  resolve the files, 'git add' them, 'git commit --no-edit'  (or 'git merge --abort')"
            say "  then run again: git fork-bump merge $new ${skip[*]/#/--skip }"
            exit 1
        fi
    done
    root=$(g rev-parse --show-toplevel)
    [ -x "$root/scripts/fork-setup.sh" ] && "$root/scripts/fork-setup.sh" >/dev/null && say "fork-setup: done"
    say
    say "$dev is ready: build and test it (make && make run). Release only on the maintainer's decision."
    say "Push (maintainer, from the host):"
    say "  git push origin $new $dev $fork"
    say "  git push --force-with-lease origin ${order[*]}"
    say "GitHub: set the default branch to $dev (to $fork after its first release)."
    ;;
esac
