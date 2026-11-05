# Fork workflow — "PrusaSlicer ext"

How this fork of [`prusa3d/PrusaSlicer`](https://github.com/prusa3d/PrusaSlicer) is organised.
For maintainers and AI agents; the short binding rules are in `CLAUDE.md`
("Fork workflow rules").

## Goals

1. Upstream stays the visible base: every Prusa release `version_X.Y.Z` gets its own lines.
2. Every feature/fix is its own branch, so at each upstream release we decide per feature:
   keep, drop (upstream fixed it / obsolete) or rework (likely for 3.x).
3. Features are tested together before anything is released; releases are never rewritten.
4. Each build shows exactly what it is (About / System Info): upstream version, our version or
   development state, included features.

## The model

```
feature/x ─┐
feature/y ─┼─▶ dev/2.9.6 ──(user: "release")──▶ fork/2.9.6 + tag v2.9.6+ext-v1.0.0
fix/z ─────┘   development / integration tests    releases only
                   ▲                                   │
                   └──── merged back after each release ┘
```

| Branch | Purpose |
|---|---|
| `feature/<id>`, `fix/<id>` | The work. Based on the **newest release tag of the line** (`v2.9.6+ext-v1.0.0`; the upstream tag `version_2.9.6` only while the line has no release yet). The release already contains the build tooling and the platform features (Wayland, wx 3.3), so `make && make run` works on the branch itself; the branch's own commits are `<base tag>..<branch>`. Stacked on another feature only if it really extends it (`feature/wx3.3` on `feature/wayland`). Owns `doc/fork/features/<id>/`. |
| `dev/<upstream>` | Development line for **all** releases on this upstream base: upstream tag + `git merge --no-ff` of the features, `feature/fork-tooling` first. All features are tested **together** here; this is the branch you build and run. After each release the release is merged back, so the next release continues from the previous one. |
| `fork/<upstream>` | Release line: only `git merge --no-ff dev/<upstream>` when the maintainer decides to release, then the release tag. |
| `pr/<id>` | Upstream-PR head (see appendix): code only, never merged into `dev`/`fork`. |

All merges are merge commits (`--no-ff`, set as default for `dev/*` and `fork/*` by
`scripts/fork-setup.sh`) with the message `Merge <branch> @<sha10> (base <base tag>)`;
`git fork-bump` replays the merge order from these messages (it ignores the back-merges
`Merge fork/<upstream> @<sha10> (release <tag>)`). `dev/*` and `fork/*` are never
rebased or force-pushed after they were pushed. `git log --first-parent dev/2.9.6` lists the
merged features and back-merges; `git log --first-parent fork/2.9.6` lists the releases.

**Granularity:** one feature = one capability with everything it needs (code, docs, tooling,
its display). Separate features are things that can be kept or dropped independently at an
upstream bump. Example: the build tooling, workflow docs and the build identity in About /
System Info are one feature, `feature/fork-tooling`.

## Everyday work

**New feature or fix** — the agent asks first: feature or fix, and the `<id>`.
```bash
base=$(git describe --tags --abbrev=0 --match 'v2.9.6+*-v*' --exclude '*-rc.*' fork/2.9.6)   # newest release, e.g. v2.9.6+ext-v1.0.0
git switch -c feature/<id> "$base"
# first commit: doc/fork/features/<id>/README.md (frontmatter, see below)
# ... work, commit; build and try the feature alone on its branch:
make && make run
git switch dev/2.9.6                                        # integration test with all other features
git merge --no-ff -m "Merge feature/<id> @$(git rev-parse --short=10 feature/<id>) (base $base)" feature/<id>
make && make run
```
Why the newest release and not `dev` (git flow starts features from `develop`): `dev` may hold
features that are still in test and may be dropped — a branch started from `dev` would carry them
and bring them back with its next merge. The release tag never moves, so the branch's own commits
stay exactly `<release>..<branch>`. A feature started on an older release keeps working; rebase it
onto a newer release only if it needs something from it.

**Test round failed** (any change request = not passed): fix on `feature/<id>`, merge it into
`dev/2.9.6` again, rebuild. No tags during testing — the build shows itself as
`Development build on dev/2.9.6 (after <last release tag>)`.

`dev/2.9.6` is pushed together with a release, not in between: everything merged since the
last release stays local, so `dev` can still be rebuilt without a feature that is not ready
(see "Remove a feature").

**Release** — only when the maintainer says so (example: the second release on 2.9.6):
```bash
# on feature/fork-tooling: add '### v2.9.6+ext-v1.1.0 — <date>' to doc/fork/lines/2.9.6.md, merge into dev
git switch fork/2.9.6
git merge --no-ff -m "Release v2.9.6+ext-v1.1.0: merge dev/2.9.6 @$(git rev-parse --short=10 dev/2.9.6)" dev/2.9.6
git tag -a --cleanup=verbatim -F <release-notes section> 'v2.9.6+ext-v1.1.0'
make && make run                                            # About shows "Release v2.9.6+ext-v1.1.0"
# back-merge: the release (tag) into the history of dev, the files do not change
git switch dev/2.9.6
git merge --no-ff -m "Merge fork/2.9.6 @$(git rev-parse --short=10 fork/2.9.6) (release v2.9.6+ext-v1.1.0)" fork/2.9.6
# maintainer pushes:
git push origin dev/2.9.6 fork/2.9.6 'v2.9.6+ext-v1.1.0' feature/<id> ...
```
`--cleanup=verbatim` keeps the markdown `#` headings of the notes in the tag message; the same
text is the body of the GitHub Release. The back-merge must be `--no-ff` (the default for
`dev/*`): a fast-forward would move `dev` onto the release commit, whose first parent is the
upstream tag — `git log --first-parent dev/2.9.6` and `git fork-bump` would no longer see the
feature merges. Without the back-merge the release tag is not in the history of `dev`, and dev
builds cannot show which release they continue.

**Next release on the same base** — `dev/2.9.6` simply continues; there is no new branch per
release. After the back-merge it holds exactly the released state; new features/fixes and
new rounds of released ones are merged on top, `feature/fork-tooling` again at the end with the
release notes. Our version: MINOR if a feature was added, removed or reworked, PATCH for fixes
only (see "Versions"). A feature that is merged on `dev` but not ready at release time is left
out first (see "Remove a feature"); it stays on its branch and is merged again after the release.

**Remove a feature**
- Merged since the last release, `dev` not pushed since: rebuild `dev` without it — reset to
  the last pushed state and merge the remaining unreleased features again, in the same order:
  `git switch dev/2.9.6 && git reset --hard origin/dev/2.9.6`, then the merges as above.
  (`origin/dev/2.9.6` = the back-merge of the last release. Before the first push of a line it
  does not exist yet: reset to the upstream tag and merge all remaining features.)
- Already released or pushed: `git revert -m 1 <merge commit of the feature>` on `dev/2.9.6`
  (its doc folder disappears too, so About no longer lists it); the next release ships without
  it. To bring it back later: revert that revert first, then merge again. Never reset a pushed
  `dev` — GitHub refuses the force push.
- At the next upstream release: `git fork-bump … --skip <branch>`.

## Upstream release (e.g. 2.9.6 → 2.9.7)

Only stable tags `version_X.Y.Z` are a base (alpha/beta/rc are not; 3.x work starts at
`version_3.0.0`). `git fork-bump` (installed by `scripts/fork-setup.sh`, works on any branch):

```bash
git fork-bump plan   version_2.9.7                   # changes nothing: branches, stacks, merge order
git fork-bump rebase version_2.9.7 [--skip b] [--rework b]
#   every feature/fix of dev/2.9.6 onto the new tag (stacks from the top, --update-refs),
#   old tips in refs/fork-bump/2.9.7/*, range-diff per branch (what upstream absorbed);
#   conflict: resolve, git rebase --continue, run the same command again (rerere helps)
#   --skip: dropped for 2.9.7; --rework: restart from the tag with only the README commit
# decide per feature keep / drop / rework: decision log in each README and
# doc/fork/lines/2.9.7.md (on feature/fork-tooling)
git fork-bump merge  version_2.9.7 [--skip b]
#   dev/2.9.7 + fork/2.9.7 from the tag, --no-ff merges in the order of dev/2.9.6
```
Test on `dev/2.9.7`, release as above (new base = MINOR version). Push — a GitHub fork never
receives new upstream tags by itself, and rebased feature branches need a force push:
```bash
git push origin version_2.9.7 dev/2.9.7 fork/2.9.7
git push --force-with-lease origin feature/<id> fix/<id> ...
```
Set the GitHub default branch to the newest line. Manual fallback (what the tool does):
`git rebase --update-refs --onto version_2.9.7 version_2.9.6 <top branch>`,
`git range-diff version_2.9.6..<old tip> version_2.9.7..<branch>`, then the `dev` merges.

**Release-based branches** (started from a `v2.9.6+ext-v…` tag) contain the whole release, so
only their own commits move to the new line — "start from scratch" on the new upstream tag:
```bash
git rebase --onto version_2.9.7 <its release tag> <branch>          # own commits only
git range-diff <its release tag>..<old tip> version_2.9.7..<branch>
```
`git fork-bump` does this by hand-off for now: `plan`/`rebase` list such branches as "release-based"
and `merge` refuses until each is ported (then it is on the new tag and handled normally). On the
new line they are tag-based again until its first release; new branches then start from that
release.

The old lines stay for patch releases: they only take new `fix/<id>-2.9.6` branches (the
`-<upstream>` suffix keeps them out of `git fork-bump`). Upstream 3.x restructures the tree:
expect `--rework` for most features (old branch tip in `refs/fork-bump/…` = specification).

## Versions

Release tag: `v<upstream>+ext-v<our version>`, e.g. `v2.9.6+ext-v1.0.0`.

- `SLIC3R_VERSION` always stays the upstream version (profiles, updater, 3MF, G-code depend on it).
- Our version is semver of our changes: MAJOR per upstream major line (2.x → `1.y.z`,
  3.x → `2.y.z`) or a breaking change of ours; MINOR = feature added/removed/reworked or new
  upstream base; PATCH = fixes only.
- Always match our tags with `'v*+ext-v*'` (upstream also has unrelated old `v3.x` tags).

What a build shows (generated by git at build time):
```
Version 2.9.6
PrusaSlicer ext - fork of prusa3d/PrusaSlicer
Release v2.9.6+ext-v1.0.0          | Development build on dev/2.9.6 (after <last tag>)
Based on upstream version_2.9.6 (b028299c77)
Extended features: fork-tooling, mmu-temp-sync, wayland, wx3.3
https://github.com/paulklima/PrusaSlicer-ext
```

## Per-feature documentation

Every `feature/<id>` / `fix/<id>` owns `doc/fork/features/<id>/` and is the only branch that
writes there (no merge conflicts): `README.md` (required), optional `plan.md`, `reviews/`,
`notes/`. The README starts with frontmatter — the build reads it for the feature list:

```
---
id: mmu-temp-sync          # = folder name
kind: feature              # feature | fix | tooling
title: MMU toolchange temperature sync (early M104 + M109 barrier)
show: true                 # listed in About (System Info lists all)
upstream_pr: none          # none | <url> | merged:<version_X.Y.Z>
---
```
Then: what/why, files touched, how to test, upstream status, decision log per upstream base.
Which release first contained a feature is recorded in `doc/fork/lines/<upstream>.md`, not in
the README (that would need a commit on every branch per release).

Shared docs belong to `feature/fork-tooling`: this file, `CLAUDE.md`, the README banner, the
build guide, `doc/fork/lines/<upstream>.md` (per upstream line: feature decisions on that base,
incl. dropped features, and the release notes), `doc/fork/backlog/` (ideas without a branch yet).

## Tooling (part of `feature/fork-tooling`)

- `Makefile` — `make` (deps once into `deps/build` + app into `build`), `make run`,
  `make test`; log `build/build.log`, overwritten per run; exports
  `CMAKE_POLICY_VERSION_MINIMUM=3.5` for CMake 4 / Ubuntu 26.04. `make SANDBOX=1` = temporary
  agent-sandbox build in `build-sandbox/` (see `CLAUDE.md`).
- `scripts/fork-setup.sh` — once per clone and after a new line: local `dev/*`/`fork/*` from
  `origin` in a fresh clone, `--no-ff` default for them, rerere, `fork-status.sh` +
  `fork-bump.sh` into `<git-common-dir>/fork-tools/`, alias `git fork-bump`, the status script
  as Claude Code session hook, `CLAUDE.local.md` (the rules, available on every branch).
- `scripts/fork-status.sh` — shows the current branch, unmerged/unreleased work, missing docs.
- `scripts/fork-bump.sh` — `git fork-bump`, see "Upstream release".
- Build identity: `version_fork.inc`, `cmake/modules/ForkGitVersion.cmake`,
  `src/libslic3r/ForkInfo.*` (details in `doc/fork/features/fork-tooling/README.md`).
- Pushing happens from the maintainer's machine (the agent sandbox cannot push).

## GitHub

- `paulklima/PrusaSlicer-ext` is a GitHub fork of `prusa3d/PrusaSlicer` (needed for upstream
  PRs). Actions are disabled (the upstream workflows use `secrets: inherit`).
- Rulesets: `dev/*`, `fork/*` — no force push, no deletion; tags `v*+ext-v*` — no update, no
  deletion.
- Default branch: the newest `fork/<upstream>` (its `dev/<upstream>` until its first release).

## Why this model (decision record)

| Option | Pros | Cons |
|---|---|---|
| **feature branches + `dev` + `fork` per upstream release** (chosen) | features stay separate and re-evaluable per upstream release; integration testing before release; release history never rewritten | feature branches are rebased at each upstream release; conflicts between our features are resolved again (stack features that always conflict) |
| one long-lived main, merging upstream into it | simplest | features tangle with upstream and each other; dropping one needs reverts; base becomes fuzzy |
| classic git flow (`master` + `develop`) | familiar | one long-lived develop across upstream releases; no clean "rebase onto next upstream" step |
| **resulting mix** (chosen in practice) | inside a line it *is* git flow — `fork/*` = master (tagged releases), `dev/*` = develop, features merged into `dev`, releases merged back; across lines the features are a patch stack replayed onto each new upstream tag (`rebase --onto`, rerere, range-diff — like OpenShift's carry patches) | features start from the release tag, not from `develop` (see "Everyday work") |
| patch files (quilt) | perfect isolation | history outside git, manual refresh, poor fit for large C++ changes |

## Appendix (rarely needed)

- **Release candidate** for testers: `v2.9.6+ext-v1.1.0-rc.1` on `fork/2.9.6`, made and
  merged back like a release.
- **Upstream PR** of a fix/feature: Prusa takes PRs against `master`, so the PR head is a
  separate, short-lived branch with the code commits only (no fork docs):
  `git switch -c pr/<id> upstream/master && git cherry-pick <code commits of fix/<id>>`;
  set `upstream_pr: <url>` in the README; delete `pr/<id>` when the PR is merged or closed
  (`upstream_pr: merged:<version_X.Y.Z>`, drop the branch at that bump).
- **Retired branches** are deleted; their last state stays reachable through the `dev` merge
  that contained them.
