# Versioning, CI & Releases — Design & Implementation Plan

**Status:** plan for review, not yet implemented.
**Date:** 2026-06-05

This doc defines how `zmk-dynamic-macros` is versioned, tested, and released, and
lists the concrete changes needed to the workflows and the README. It supersedes
the brief versioning notes in `robustness-review.md` (findings #3/#4 there).

---

## 1. The model: a hybrid, *not* strict lockstep

We build on urob's [zmk-actions](https://github.com/urob/zmk-actions) for the
test harness, but **do not** adopt its strict ZMK-lockstep release scheme
(where every ZMK release forces a matching module release and ZMK owns the patch
number). For a new, evolving module we want to ship fixes on our own cadence.

The scheme:

> **Module `MAJOR.MINOR` tracks the ZMK line it targets; the `PATCH` is the
> module's own.**

- `v0.3.x` means "built and tested against ZMK `0.3`."
- `v0.3.0`, `v0.3.1`, `v0.3.2`, … are *our* iterations within that line — ship a
  bug fix any day and bump the patch, independent of ZMK's cadence.
- A floating `v0.3` tag points at our latest `0.3.x`, so users who pin `v0.3`
  get our patches automatically.
- When ZMK advances its **minor** (`0.3` → `0.4`), we start a `v0.4.x` line.
- A ZMK **patch** bump (`0.3.0` → `0.3.1`) is absorbed by CI with **no** module
  version change.

This decouples three things that the stock tooling entangles in one `VERSION`
file:

| Concept | Source of truth |
| --- | --- |
| Module version | `VERSION` file + git tags |
| ZMK version(s) tested against | the test workflows (`main` and the `v0.3` pin) |
| Module release trigger | pushing a `vX.Y.Z` tag |

### Tag scheme (mirrors what ZMK itself does)

ZMK publishes full tags (`v0.3.0`) **and** floating minor tags (`v0.3`), but
**no** floating major (`v0`); its maintenance branches are `v0.3-branch` etc.
We mirror this:

- **Full tag** `v0.3.2` — immutable, one commit.
- **Floating minor** `v0.3` — force-moved to the latest `0.3.x`.
- **No floating `v0`** — skipped, consistent with upstream.

### Releases vs tags

ZMK modules (leader-key, zmk-helpers, rgbled-widget, dongle-display) all ship
**git tags only — no GitHub Releases** (their `/releases` pages read "There
aren't any releases here"). west pins to a tag; there are no build artifacts to
attach. We follow the norm: **tags only, no `gh release create`.**

---

## 2. Workflow inventory

| File | Action | Purpose |
| --- | --- | --- |
| `test-main.yml` | keep as-is | Test against ZMK `main` (early warning) |
| `test-release.yml` | **modify** | Test against ZMK `v0.3` (our target line) |
| `upgrade-zmk.yml` | **remove** | Replaced by `track-zmk.yml` |
| `release.yml` | **replace** | Tag-maintenance (move floating `v0.3`), not lockstep |
| `track-zmk.yml` | **add** | Open a PR when ZMK's *minor* advances (PR-based) |

Net effect vs. what is currently committed: `test-main` unchanged;
`test-release` stops reading `VERSION` for the ZMK ref and pins `v0.3`; the
stock lockstep `upgrade-zmk` + `release` are dropped; a new PR-based `track-zmk`
and a new tag-maintenance `release` are added.

**PAT:** the PR-based `track-zmk` needs `ZMK_ACTIONS_TOKEN` (a Personal Access
Token with `contents` + `pull-requests` write) so the bump PR triggers CI —
GitHub's built-in `github.token` cannot trigger workflows on PRs it opens. (This
is the same PAT urob's stock bot uses.) All other workflows use `github.token`.

### 2.1 `test-main.yml` — unchanged

```yaml
name: Run tests (main)

on:
  workflow_dispatch:
  push:
    paths: ["dts/**", "include/**", "src/**", "tests/**"]
  pull_request:
    paths: ["dts/**", "include/**", "src/**", "tests/**"]
  schedule:
    - cron: "0 21 * * 0,3" # Sundays and Wednesdays at 21:00 UTC

jobs:
  test:
    uses: urob/zmk-actions/.github/workflows/run-tests.yml@v12.0.0
    with:
      toolchain: gnuarmemb
      zmk-version: main
```

### 2.2 `test-release.yml` — pin to the ZMK line (no `VERSION` fallback)

`run-tests` uses `${{ inputs.zmk-version }}` and only falls back to `cat VERSION`
when that input is empty. Passing `zmk-version: v0.3` explicitly means `VERSION`
is **no longer read for the ZMK ref** — the decoupling we want. ZMK keeps a
floating `v0.3` tag, so this auto-tracks ZMK `0.3.x` patches with zero custom
code.

```yaml
name: Run tests (release)

on:
  workflow_dispatch:
  push:
    paths: ["dts/**", "include/**", "src/**", "tests/**"]
  pull_request:
    paths: ["dts/**", "include/**", "src/**", "tests/**"]

jobs:
  test:
    uses: urob/zmk-actions/.github/workflows/run-tests.yml@v12.0.0
    with:
      toolchain: gnuarmemb
      zmk-version: v0.3 # ZMK line this module targets; bumped by track-zmk's PR
```

### 2.3 `track-zmk.yml` — PR-based, hybrid semantics (replaces stock `upgrade-zmk.yml`)

We take urob's **PR mechanism** but keep **hybrid semantics**: it fires only when
ZMK's `MAJOR.MINOR` advances (not on ZMK patch releases), and the PR edits the
*module's own* version/line — it does **not** force a release. Our current line
is read from `VERSION`. The PR pre-fills the mechanical edits (`VERSION`,
`test-release.yml`); the maintainer adapts code, updates the README pin, reviews
CI, and merges. The release itself happens later by pushing a tag (§2.4).

```yaml
name: Track ZMK releases

on:
  workflow_dispatch:
  schedule:
    - cron: "0 22 * * *" # daily at 22:00 UTC

permissions:
  contents: write
  pull-requests: write

jobs:
  track:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Open a PR if ZMK has a newer minor than our line
        env:
          GH_TOKEN: ${{ secrets.ZMK_ACTIONS_TOKEN }}
        run: |
          set -euo pipefail
          zmk=$(gh release view --repo zmkfirmware/zmk --json tagName -q .tagName)
          zmk_minor=${zmk%.*}                  # v0.3.0 -> v0.3
          ours_minor=$(cut -d. -f1,2 VERSION)  # v0.3.2 -> v0.3
          [ "$zmk_minor" = "$ours_minor" ] && { echo "Up to date ($ours_minor)."; exit 0; }

          branch="track-zmk-$zmk_minor"
          if git ls-remote --exit-code --heads origin "$branch" >/dev/null 2>&1; then
            echo "Branch $branch already exists."; exit 0
          fi

          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git switch -c "$branch"

          echo "$zmk_minor.0" > VERSION
          sed -i "s/zmk-version: v[0-9]\+\.[0-9]\+/zmk-version: $zmk_minor/" \
            .github/workflows/test-release.yml

          git commit -am "Track ZMK $zmk_minor: start $zmk_minor.0 line"
          git push -u origin "$branch"

          gh pr create --title "Track ZMK $zmk_minor" --body \
          "ZMK published **$zmk**. This starts the **$zmk_minor.0** module line.

          - \`VERSION\` -> \`$zmk_minor.0\`
          - \`test-release.yml\` now tests against ZMK \`$zmk_minor\`

          Before merging: adapt the code to any ZMK $zmk_minor API changes (CI on
          this PR tests against ZMK $zmk_minor) and update the README pin guidance
          to \`$zmk_minor\`. After merge, tag \`$zmk_minor.0\` to publish."
```

> Why a PAT: a PR opened with the built-in `github.token` does **not** trigger
> the `on: pull_request` test workflows, so we'd lose the "test against new ZMK
> before merging" value. `ZMK_ACTIONS_TOKEN` (PAT) restores that, matching urob.

### 2.4 `release.yml` — move the floating tag (replaces lockstep `upgrade-module`)

Triggered by pushing a full `vX.Y.Z` tag; force-moves the floating `vX.Y`. No
GitHub Release. The minor tag (`vX.Y`) does not match the `vX.Y.Z` trigger
pattern, so this cannot loop.

```yaml
name: Update floating tag

on:
  push:
    tags: ["v[0-9]+.[0-9]+.[0-9]+"]

permissions:
  contents: write

jobs:
  float:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - name: Move the floating minor tag to this release
        run: |
          set -euo pipefail
          tag="${GITHUB_REF_NAME}"   # v0.3.2
          minor="${tag%.*}"          # v0.3
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git tag -f "$minor" "$tag"
          git push -f origin "refs/tags/$minor"
          echo "Moved $minor -> $tag"
```

---

## 3. The `VERSION` file

Now means the **module** version and should equal the latest full tag (e.g.
`v0.3.2`). It is:
- read by `track-zmk.yml` to derive our current line, and
- a human-readable marker of the current release.

It is **no longer** read as the ZMK ref (that moved into `test-release.yml`).
Starting value: **`v0.3.0`** (the module's first line, targeting ZMK 0.3).

---

## 4. Procedures

**Cut a module patch (same ZMK line):**
1. Land fixes on `main`.
2. Bump `VERSION` to `v0.3.2`, commit.
3. `git tag -a v0.3.2 -m "v0.3.2" && git push origin main v0.3.2`.
4. `release.yml` moves `v0.3` → `v0.3.2`. Users on `revision: v0.3` get it next build.

**Start a new ZMK line (`track-zmk` opens a PR for `v0.4`):**
1. Review the PR; adapt code to ZMK `0.4`; confirm CI on the PR is green.
2. Update the README pin guidance to `v0.4` in the PR; merge.
   (The PR already set `VERSION` → `v0.4.0` and `test-release.yml` → `v0.4`.)
3. Tag `v0.4.0`, push → floating `v0.4` created.

**Bootstrap (first release):** `VERSION` is already `v0.3.0`; tag `v0.3.0` and
push → `release.yml` creates the floating `v0.3`. Until this first tag exists,
`revision: v0.3` does not resolve and only `main` works.

---

## 5. README changes to apply

The current Setup section (after the recent edit) has a YAML comment marked `**`,
a placeholder line, and an unfinished sentence ending in `However... **`. Replace
the trailing stub with a precise explanation. **Key correction:** the draft says
*"the module version tracks zmk version"* — that's lockstep. The accurate
statement is **major.minor tracks ZMK; patch is the module's own.**

**5.1 Fix the YAML block** (`### 1. Add to config/west.yml`):
- `zmk` project comment: `revision: v0.3 # Set to your ZMK release line`
- module comment: `revision: v0.3 # Match this to your ZMK major.minor`
- Replace the `.... further specified modules as desired ....` placeholder with a
  proper YAML comment, e.g. `# - name: <other-module> ...` (commented so the
  block stays valid YAML).

**5.2 Replace the `However... **` stub** with something like:

> This module is tested against ZMK via urob's
> [zmk-actions](https://github.com/urob/zmk-actions). Its version is a **hybrid**:
> the **major.minor** matches the ZMK line it targets (so `v0.3.x` works with ZMK
> `0.3`), while the **patch** is the module's own — bug fixes ship on the module's
> schedule, independent of ZMK's release cadence.
>
> **Pin the module to the same major.minor as your ZMK:**
>
> - `revision: v0.3` — floating minor tag; tracks the module's latest patch for the
>   ZMK 0.3 line (recommended).
> - `revision: v0.3.0` — an exact, immutable release.
> - a **full 40-char commit SHA** — fully immutable (west cannot resolve abbreviated
>   hashes); loses the version-matching convenience and auto-fixes.
> - `revision: main` — latest unreleased changes; may be unstable.
>
> See [docs/versioning-and-ci.md](versioning-and-ci.md) for the full scheme.

**5.3** Keep the `### Compatibility` section's wording consistent with the above
(it currently says "Released in lockstep with ZMK" — soften to "tracks ZMK's
major.minor; patches are the module's own").

---

## 6. Decisions (resolved)

1. **`track-zmk`: PR-based** (urob's PR mechanism, hybrid semantics). Reinstates
   the `ZMK_ACTIONS_TOKEN` PAT requirement. *(Not full lockstep — fires on minor
   changes only and does not auto-release.)*
2. **`release.yml`: tag-maintenance workflow** (move floating tag automatically).
3. **Tags only, no GitHub Releases** (ecosystem convention).
4. **Starting `VERSION` = `v0.3.0`.**

---

## 7. Implementation order

1. Modify `test-release.yml` (pin `v0.3`).
2. Add PR-based `track-zmk.yml`; delete `upgrade-zmk.yml`.
3. Replace `release.yml` with the tag-maintenance version.
4. Apply the README edits in §5.
5. Create the `ZMK_ACTIONS_TOKEN` repo secret (PAT, `contents` + `pull-requests`
   write) for `track-zmk`.
6. Bootstrap: tag `v0.3.0` and push; verify the floating `v0.3` appears and
   `revision: v0.3` resolves.
