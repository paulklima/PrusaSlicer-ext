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
```

| Branch | Purpose |
|---|---|
| `feature/<id>`, `fix/<id>` | The work. Based on the upstream tag (`version_2.9.6`); stacked on another feature only if it really extends it (`feature/wx3.3` on `feature/wayland`). Owns `doc/fork/features/<id>/`. |
| `dev/<upstream>` | Development line: upstream tag + `git merge --no-ff` of the features. All features are tested **together** here; this is the branch you build and run. |
| `fork/<upstream>` | Release line: only `git merge --no-ff dev/<upstream>` when the maintainer decides to release, then the release tag. |
| `pr/<id>` | Upstream-PR head (see appendix): code only, never merged into `dev`/`fork`. |

All merges are merge commits (`--no-ff`, set as default for `dev/*` and `fork/*` by
`scripts/fork-setup.sh`) with the message `Merge <branch> @<sha10> (base <upstream tag>)`;
`git fork-bump` replays the merge order from these messages. `dev/*` and `fork/*` are never
rebased or force-pushed after they were pushed. `git log --first-parent dev/2.9.6` lists the
merged features; `git log --first-parent fork/2.9.6` lists the releases.

**Granularity:** one feature = one capability with everything it needs (code, docs, tooling,
its display). Separate features are things that can be kept or dropped independently at an
upstream bump. Example: the build tooling, workflow docs and the build identity in About /
System Info are one feature, `feature/fork-tooling`.

## Everyday work

**New feature or fix** — the agent asks first: feature or fix, and the `<id>`.
```bash
git switch -c feature/<id> version_2.9.6
# first commit: doc/fork/features/<id>/README.md (frontmatter, see below)
# ... work, commit ...
git switch dev/2.9.6
git merge --no-ff -m "Merge feature/<id> @$(git rev-parse --short=10 feature/<id>) (base version_2.9.6)" feature/<id>
make && make run                                            # test
```

**Test round failed** (any change request = not passed): fix on `feature/<id>`, merge it into
`dev/2.9.6` again, rebuild. No tags during testing — the build shows itself as
`Development build on dev/2.9.6`.

**Release** — only when the maintainer says so:
```bash
# on feature/fork-tooling: add '### v2.9.6+ext-v1.0.0 — <date>' to doc/fork/lines/2.9.6.md, merge into dev
git switch fork/2.9.6 && git merge --no-ff dev/2.9.6
git tag -a --cleanup=verbatim -F <release-notes section> 'v2.9.6+ext-v1.0.0'
make && make run                                            # About shows "Release v2.9.6+ext-v1.0.0"
# maintainer pushes:
git push origin dev/2.9.6 fork/2.9.6 'v2.9.6+ext-v1.0.0' feature/<id> ...
```
`--cleanup=verbatim` keeps the markdown `#` headings of the notes in the tag message; the same
text is the body of the GitHub Release.

**Remove a feature**
- Not released yet (the merge is only on `dev`, not pushed): rebuild `dev` without it — reset
  `dev/2.9.6` to the upstream tag and merge the remaining features again.
- Already released or pushed: `git revert -m 1 <merge commit of the feature>` on `dev/2.9.6`
  (its doc folder disappears too, so About no longer lists it); the next release ships without
  it. To bring it back later: revert that revert first, then merge again.
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
| patch files (quilt) | perfect isolation | history outside git, manual refresh, poor fit for large C++ changes |

## Appendix (rarely needed)

- **Release candidate** for testers: `v2.9.6+ext-v1.1.0-rc.1` on `fork/2.9.6`.
- **Upstream PR** of a fix/feature: Prusa takes PRs against `master`, so the PR head is a
  separate, short-lived branch with the code commits only (no fork docs):
  `git switch -c pr/<id> upstream/master && git cherry-pick <code commits of fix/<id>>`;
  set `upstream_pr: <url>` in the README; delete `pr/<id>` when the PR is merged or closed
  (`upstream_pr: merged:<version_X.Y.Z>`, drop the branch at that bump).
- **Retired branches** are deleted; their last state stays reachable through the `dev` merge
  that contained them.
