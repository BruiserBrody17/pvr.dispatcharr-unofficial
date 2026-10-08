# pvr.dispatcharr-unofficial

Unofficial, community-maintained -- not affiliated with the Dispatcharr
project or with Kodi/Team Kodi (see README.md's top note). Keep that
distinction clear in anything user-facing you write.

A Kodi PVR binary addon (C++) for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
plus two optional Python plugins that install on the Dispatcharr server
itself. See [README.md](README.md) for what it does; this file is for
working in the codebase, not using the addon. If you're a human
contributor rather than an AI assistant, [CONTRIBUTING.md](CONTRIBUTING.md)
is the shorter, more direct version of what's below.

## Repo layout

- `src/` -- the addon's C++ source (`DispatcharrClient` talks to
  Dispatcharr's REST/JSON-RPC API, `PVRDispatcharr` implements Kodi's PVR
  API surface, `XmlTvParser`/`WebSocketClient` are self-contained helpers).
- `pvr.dispatcharr-unofficial/` -- addon metadata Kodi actually loads: `addon.xml.in`
  (version lives here), `resources/settings.xml`,
  `resources/language/resource.language.en_gb/strings.po`.
- `dispatcharr-plugin/timeshift_buffer/`, `dispatcharr-plugin/recording_edl/`
  -- independent Python plugins for the Dispatcharr server, not built or
  installed through Kodi at all. Each has its own README and `plugin.json`.
- `packaging/coreelec/` -- out-of-tree CoreELEC package definition (not
  used by the GitHub Actions build).
- `docs/` -- engineering history: root causes, live-confirmed API
  behavior, things tried and reverted. Not user-facing.
- `docs/OPEN_ITEMS.md` -- the project's running punch-list; add new open
  items there rather than losing track of them in conversation, under its
  "Open" heading in whichever category fits, and move an item to "Closed"
  (don't delete it) once it's resolved.

## Building and testing

This addon cannot be compiled standalone -- it builds through Kodi's own
binary-addon build harness, which needs a matching Kodi source checkout.
Full instructions (Windows/macOS/Linux/CoreELEC, all previously verified
live) are in [docs/BUILDING.md](docs/BUILDING.md); don't guess at build
commands, read that file.

Small automated test suites exist on both the C++ and Python sides, but both are deliberately narrow -- only
Kodi/Dispatcharr-independent pure/filesystem logic is covered; the Kodi-API, HTTP, socket and Redis/Django-touching
code stays manual/live-tested.

- **C++** (`tests/`, Catch2; one `test_<module>.cpp` per pure-logic module in `src/`): a standalone CMake project, separate
  from the addon's own `CMakeLists.txt` (which only configures through Kodi's build harness; see `tests/CMakeLists.txt`).
  CI runs it as `unit-tests`, `unit-tests-sanitized` (ASan/UBSan) and `unit-tests-tsan`. `tests/glue/` compiles the real
  `DispatcharrClient.cpp`/`PVRDispatcharr.cpp` against Kodi's dev-kit headers, a stub Kodi runtime and a fake Dispatcharr
  (CI's `glue-harness`; run it by hand after touching the stream, auth or shared-curl paths, see its README).
  `WebSocketClient` has tests against a local server.
- **Python** (pytest, CI's `unit-tests-python`): `dispatcharr-plugin/{recording_edl,timeshift_buffer}/tests/` (the plugins'
  pure logic and their action dispatch with Redis/Django monkeypatched) and `tools/tests/` (the release gate, the zip
  tools, the doc-citation checker, the smoke harness's pure helpers, the CI path filter and cross-file consistency checks
  such as version sync, settings ids, dependency pins and the time zone table).
- **Not tested, on purpose:** a real Dispatcharr, a real Kodi, a real Redis/Django and real ffmpeg. Forcing a unit test that
  mocks around those would test the mock; they are verified live (`docs/MANUAL_TESTING.md`).
- **Where the detail is:** [docs/TEST_COVERAGE.md](docs/TEST_COVERAGE.md) has the module-by-module account (what each
  module is for, which incident or audit finding each test pins, how it was confirmed). Read the module's section before
  changing it, and add new entries there, not here.
- **A new module gets** a `tests/test_<module>.cpp` and a line in `tests/CMakeLists.txt` in the same change (see the
  convention below); a bug fix on the testable side gets a regression test that fails without the fix.

`.github/workflows/build.yml` also compiles
Windows and Linux (macOS is built by hand, see `docs/BUILDING.md`) and packages the two plugins -- it does not build or
test the CoreELEC package, and runs no addon against a real Dispatcharr or Kodi
(its `glue-harness` job drives the real client code against a fake Dispatcharr).
The compile-and-package jobs (`build-unix`, `build-windows`, `glue-harness`; `package-dispatcharr-plugins` on its own, narrower list: the plugin directories, the workflow and the two zip tools) are skipped on a push that touches only docs,
packaging or tools no build step runs (`tools/ci_build_needed.py`, tested; a tag, a manual run or anything unclear builds
everything); lint and every unit-test job run on every push, since the cross-file checks catch a docs-only commit that was not.

## Conventions specific to this repo

- **Keep this file short; history goes in `docs/`.** `CLAUDE.md` is loaded in full at the start of every session, and it had
  grown to about 200 KB because each fix appended its own story (audit pass, date, how it was confirmed). It says what to do
  and where things are; the "why, with dates and incidents" belongs in `docs/` (module history in `docs/TEST_COVERAGE.md`,
  fixes in `docs/OPEN_ITEMS.md`). A change that needs more than a sentence or two here is probably a docs entry plus a
  pointer. `tools/tests/test_claude_md_size.py` fails above 60 KB.
- **Docs split by audience**: `README.md` and each plugin's own `README.md`
  are concise and user-facing -- install/configure/use only. `docs/*.md`
  holds the "why" (investigations, root causes, API behavior confirmed
  against a live Dispatcharr instance, reverted approaches) and is never
  meant to be read by an end user. `CHANGELOG.md` is user-facing
  what-changed, not why. When you learn something new about Dispatcharr's
  API or fix a non-obvious bug, the explanation belongs in `docs/`, not
  buried in a commit message.
- **New or changed pure-logic code gets a test alongside it, not after, as
  of 2026-09-13.** The "Building and testing" section above draws a
  specific boundary: Kodi/Dispatcharr-independent pure/filesystem logic is
  unit-testable and covered; `PVRDispatcharr`/`DispatcharrClient`/
  `WebSocketClient`'s actual Kodi-API/HTTP/socket surface and either
  plugin's Redis/Django-touching code aren't, and stay manual/live-tested
  instead. Going forward, a feature or fix that adds or changes code on
  the testable side of that boundary should add Catch2/pytest coverage in
  the *same* change -- following the same extract-into-its-own-free-function
  pattern already used throughout `src/` and `dispatcharr-plugin/` when the
  new logic is buried inside a larger, mixed-purity function. This is what
  keeps the multi-pass catch-up work documented in
  `docs/OPEN_ITEMS.md`'s "No automated test suite exists" entry from
  needing to repeat: new code should arrive already covered rather than
  accumulating into another backlog to sweep later. Nothing changes for
  the untested side of the boundary -- forcing a "unit test" that mocks
  around Kodi's SDK or a real HTTP/Redis/Django call would test the mock,
  not the addon, so that verification stays manual as it always has.
- **"Confirmed live" citations matter**: Dispatcharr is young and its API
  schema has changed across releases. Comments and docs here frequently
  cite exactly how something was confirmed (a real endpoint response, a
  real device test) rather than just asserting behavior -- keep doing
  that instead of trusting Dispatcharr's own docs/schema at face value.
- **Never let a real personal/network detail land in the repo, as of
  2026-09-11.** Anything that becomes permanent here -- docs, code
  comments, commit messages, PR bodies -- must stay free of real
  channel/programme names (use this project's existing "Channel A"/
  "Channel B" generic-label convention, already throughout
  `docs/TIMESHIFT.md` and elsewhere), real hostnames/IP addresses, real
  account usernames, or anything else identifying a specific person's or
  specific deployment's real-world setup -- including the deployment's
  timezone in any form: a zone name, an offset ("so many hours behind UTC"), or
  a local time quoted beside its UTC equivalent. The "Confirmed live" citations
  bullet above still applies -- be specific about *mechanism* (a real
  endpoint response, a real device test) without being specific about
  *whose* deployment it was tested against. Genericizing before commit
  is far cheaper than a history rewrite after the fact.
- **When removing or changing code, a setting, or CI behavior, grep
  `docs/` (and `CHANGELOG.md`) for references to it before calling the
  change done.** `docs/*.md` cites specific function/setting names and
  exact CI commands as part of its "confirmed against real source"
  standard -- a removal or behavior change that isn't cross-checked
  leaves a dangling reference or a stale claim behind, silently, since
  nothing else in this repo reliably catches it (`tools/check_doc_refs.py`,
  wired into CI, flags an outright-removed reference, but not a claim that's
  merely gone stale in spirit; the unit test suite doesn't cover this either
  -- it's narrow, see "Building and testing" above). Real recurring failure
  mode, not hypothetical: found
  and fixed 9 of these in one pass (2026-09-10) tracing back to feature
  removals and CI changes that never got cross-checked this way.
- **Comments explain WHY, not WHAT**: this codebase's existing comments
  document non-obvious constraints, confirmed-live findings, and
  workarounds for specific bugs -- not a restatement of the code. Match
  that style; don't add narrative comments describing what a block of
  code obviously does.
- **Pre-1.0 (`0.x`) versioning on purpose, as of 2026-09-07** -- this
  project briefly reached `1.0.x` (addon) but stepped back down to `0.x`
  across all three pieces once it became clear that signaled more
  stability than actually existed: single-user, still turning up real
  playback bugs in testing, and depending on Dispatcharr itself, which is
  still pre-1.0 (`0.30.0` as of this writing) and can still make breaking
  changes of its own. Per SemVer's own convention, `0.x` means "anything
  may still change" -- an honest signal here. Move to `1.0.0` later based
  on *this project's own* track record (real testing beyond one person,
  no more of the kind of playback-breaking bugs still turning up as of
  this writing) -- not gated on Dispatcharr's own version number reaching
  `1.0.0`. Dispatcharr staying pre-1.0 is one reason this felt premature
  right now, not a literal dependency: Dispatcharr could stay `0.x` for
  years while being perfectly API-stable, or hit `1.0.0` while this addon
  still has open bugs -- either way, judge this project on its own merits.
- **Branch for anything nontrivial, as of 2026-09-07.** The dividing
  line is functional/behavioral risk, not line count or file count --
  a trivial, low-risk fix (including a doc-only wording/correction pass
  spanning multiple paragraphs or files, as long as it changes no code
  behavior) can still go straight to `Omega`, same as this project has
  done so far. Anything that changes real behavior -- a real bug
  investigation, a new feature, a risky refactor -- gets its own
  short-lived branch, merged back to `Omega` once verified, then
  deleted. This is deliberately not full GitFlow (no perpetual `develop`/
  `release` branches) -- there's only one person working on this repo
  right now, so there's no multi-contributor coordination problem to
  solve; the only goal is keeping `Omega` in a known-good state and
  having a diff to review before a change becomes permanent, given the
  unit test suite is narrow (see "Building and testing" above) and won't
  catch a half-finished change to the addon's actual PVR/HTTP logic.
  A branch that lives for a while (a real feature, not a quick fix)
  drifts out of sync with `Omega` as unrelated work merges in around
  it -- periodically run `git merge Omega` into it along the way rather
  than letting the gap grow for weeks and facing one large reconciliation
  at the end. This matters more here than on a project with real test
  coverage: a dependency whose *signature* changed underneath the branch
  fails loudly (won't compile) the moment Omega's changes are merged
  in, but a dependency whose *behavior* changed without its signature
  changing won't -- nothing will flag it except actually re-testing the
  branch after syncing, since the unit test suite doesn't cover this kind
  of behavioral drift either.
- **The main branch is named after the Kodi release it targets, as of 2026-10-05: `Omega`
  (Kodi 21), not `master`.** Kodi's own addons (e.g. `pvr.hts`) name branches after the Kodi
  release they build against (`Matrix`, `Nexus`, `Omega`, `Piers`...), so this follows the same
  convention; `KODI_BRANCH` in `.github/workflows/build.yml` is the matching Kodi tree. When a
  later Kodi release (Piers, Kodi 22) changes the PVR API enough that the addon needs separate
  per-Kodi versions, branch `Piers` off `Omega` at that point and keep `Omega` as the
  maintenance branch for Kodi 21, rather than renaming again. Older text in `docs/` and in this
  file that says `master` describes the branch as it was then and is left as written. The links to
  the public GitHub repo that still say `master` are tracked in `docs/OPEN_ITEMS.md`'s "Links to the
  public GitHub repo still name the master branch" entry.
- **Every external contributor's PR gets a real manual review before
  merging, as of 2026-09-11 -- never auto-merged on green CI alone.**
  The bullet above's "no multi-contributor coordination problem" framing
  stops holding the moment someone outside the project opens a PR.
  Green CI (`lint`/`unit-tests`/`unit-tests-sanitized`/`unit-tests-tsan`/`unit-tests-python`/`glue-harness` and the
  `build-unix`/`build-windows` builds) only proves
  it compiles, formats cleanly, and doesn't regress the narrow
  unit-tested pieces and the scenarios of the glue harness -- it proves nothing about whether a change to the
  actual PVR/HTTP/Redis/Django-touching logic (untested, see "Building
  and testing" above) is correct, so it's necessary but never sufficient
  on its own. This doesn't require GitHub's "require pull request reviews"
  branch-protection rule to be technically enforced: only the
  maintainer has write access to this repo right now, so an outside
  contributor's PR literally cannot merge itself regardless -- this
  bullet exists to keep that a stated, deliberate policy rather than an
  accident of current permissions, so it doesn't quietly erode later
  (e.g. if another collaborator ever gets write access).
- **Run a data-hardening sweep before cutting any tag/release, as of
  2026-09-25.** A tag/release is the moment this project's history
  actually becomes something a stranger might clone, browse, or
  download an asset from -- so it's the right gate to check before,
  not an arbitrary calendar cadence. Before
  tagging: run a hardening audit (an Opus subagent sweep across code,
  docs, full git history, and every hosting-side surface -- release
  assets, activity feed, issues/PRs -- checking known categories and a
  general sensitive-term sweep) and resolve any real
  finding via a `git-filter-repo` history rewrite (never a plain
  commit) before the tag goes out, not after. A local pre-push check
  (`.git/hooks/pre-push`) also guards ordinary commits against a list
  of disallowed terms -- it's a backstop, not a
  substitute for the pre-release sweep, which is what catches anything
  genuinely new. Binary release assets need the same gate as text: run
  `strings` over every built library (Android, macOS, CoreELEC, Windows) and
  look for `/home/`, `/Users/` and `C:\` paths before uploading -- the
  hand-built Android libraries once shipped unstripped debug builds that embedded the
  build host's directory layout. Publish stripped builds. Check every zip's
  entry timestamps too: a zip that stores both a local-clock and a UTC time
  per entry reveals the build machine's timezone (CI normalizes the Windows
  zip in PowerShell and the Linux zip with `tools/normalize_zip.py`, and the plugin
  zips are built with `TZ=UTC zip -X`; a hand-built zip needs `normalize_zip.py` before
  upload, see `docs/BUILDING.md`). These asset checks are one script,
  `tools/check_release_zip.py`, run over every release zip (see "Publishing a release's
  assets" below); do the `strings` look by hand only to investigate a failure.
- **Batch fixes into releases -- don't tag/release per individual fix.**
  Early on this project tagged and released (including the full manual
  CoreELEC build-and-upload dance) after nearly every single bug fix,
  which was far more release overhead than the project's single-user,
  actively-testing phase warrants. Commit fixes to `Omega` as they land;
  only cut an actual tag/release when a meaningful batch has accumulated
  or a real test pass is about to happen against a batch of changes.
- **Merge branches with squash-merge, as of 2026-09-07** -- when a
  nontrivial branch (see above) is done, squash it into one commit on
  `Omega` rather than preserving every individual commit from the
  branch. Keeps `Omega`'s history readable as "one commit = one logical
  change" instead of a trail of in-progress "wip"/"fix typo" commits.
- **Code formatting/linting, as of 2026-09-07**: `.clang-format`
  (C++, `src/` and `tests/`) and `ruff.toml` (Python, `dispatcharr-plugin/`, `tests/glue/`
  and `tools/`). Run `clang-format -i src/*.cpp src/*.h tests/*.cpp tests/glue/*.cpp` and
  `ruff format dispatcharr-plugin/ tools/ tests/glue/` before committing nontrivial C++
  or Python changes; `ruff check dispatcharr-plugin/ tools/ tests/glue/` catches some real bugs
  (unused variables, etc.), not just style. Both configs are
  deliberately conservative -- `.clang-format` has `SortIncludes: false`
  since a couple of files rely on include order for platform-conditional
  (`#if defined(_WIN32)`) blocks, and neither config imposes an
  unrelated style; both were derived from the codebase's own existing
  conventions rather than a generic preset.
- **Three independent version numbers, decoupled since 1.0.1** -- the
  addon and each of the two companion plugins version separately. Bump
  only the piece whose own files actually changed in a given release;
  don't bump a plugin just because the addon released, or vice versa
  (pre-1.0 releases moved all three together on purpose, to signal "the
  1.0-era plugins" -- that was a deliberate one-time exception, not the
  ongoing policy). Whichever piece(s) you *are* bumping, all of that
  piece's own version locations still need to move together -- it's easy
  to miss one:
  - Addon: `pvr.dispatcharr-unofficial/addon.xml.in` (`<addon version="...">`) and
    `packaging/coreelec/pvr.dispatcharr-unofficial/package.mk` (`PKG_VERSION`,
    with `PKG_SHA256` reset to the all-zeros placeholder until the tag
    exists and the real checksum can be computed) move together -- CoreELEC
    packages the addon binary, not either plugin, so this pair only moves
    when the addon's version does, regardless of what the plugins are doing.
  - Each plugin's `plugin.json` **and** its own `plugin.py`'s `Plugin`
    class `version` attribute move together -- `plugin.json` is only used
    for Dispatcharr's not-yet-trusted import preview; the hardcoded class
    attribute in `plugin.py` is what Dispatcharr actually runs once
    trusted. Bumping only `plugin.json` is a real, easy-to-miss mistake.
  A git tag/GitHub Release is still addon-version-scoped (that's what
  triggers CI); both plugins' zips get attached to it regardless of
  whether their own version moved, since `package-dispatcharr-plugins`
  zips whatever's currently committed either way. `CHANGELOG.md` should
  say explicitly which piece(s) moved in a given entry rather than
  implying all three share one number.
- **A GitHub Release's notes must inline any bundled plugin's own
  changelog entry, not just point at it, as of 2026-09-10.** When a tag
  bundles an already-published plugin version that didn't move for *this*
  release (the addon's own `CHANGELOG.md` entry says as much, e.g. "this
  release also bundles the already-published `timeshift_buffer` `0.6.1`
  fix"), the release notes need that plugin's actual Added/Fixed/etc.
  bullets reproduced in the release body itself -- not just a sentence
  pointing the reader at `CHANGELOG.md` (a separate file, off the release
  page entirely) or even at "its own entry below" (still real navigation
  friction if it's actually in a different scope). `CHANGELOG.md` itself
  can keep each piece's entry separate (that's still correct, per the
  bullet above) -- this only applies to the release notes actually posted
  to the GitHub Release, since duplicating content within `CHANGELOG.md`
  itself risks the two copies drifting apart on a later edit, while a
  past release's notes are already a frozen, one-time snapshot with no
  such risk.
- **A plugin-only fix that lands after its addon release already
  shipped needs that release's zip updated, not left stale until the
  next one, as of 2026-09-11.** Plugins don't get their own tags --
  their zips only ever get attached to whichever addon-version-scoped
  tag happens to exist when `package-dispatcharr-plugins` runs. A real
  case: the `client_ip` header-injection fix (`timeshift_buffer`
  `0.6.2`) merged the same day as `0.9.4`'s own tag/CI run, but hours
  after it -- so `0.9.4`'s already-published `timeshift_buffer.zip`
  kept shipping the vulnerable `0.6.1` code until this was caught.
  Given the "batch fixes, don't release per fix" convention above, the
  next real addon release could be a while -- don't wait, especially
  for anything security-relevant. Rebuild just that plugin's zip from
  current `Omega` (same structure
  `package-dispatcharr-plugins`'s own step produces: the plugin's own
  directory as the zip's top-level entry; from `dispatcharr-plugin/`:
  `TZ=UTC zip -X -r "../dist/${plugin}.zip" "$plugin" -x "$plugin/tests/*" -x "*/__pycache__/*"`,
  the exact command in the step, which a hand-built zip is not otherwise checked
  against; the `TZ=UTC` and `-X` matter, since a bare `zip` stores the local time and the
  UTC time of every entry, which publishes the build machine's timezone) and `gh release upload
  <tag> <zip> --clobber` onto the existing release -- no new tag, no
  new addon build, this is a plugin-only exception to the
  don't-release-per-fix rule above. Then update that release's own
  notes with a dated addendum inlining the actual fix (per the bullet
  above), not just a pointer -- and don't leave the original notes'
  now-false "neither plugin changed" claim standing uncorrected.
- **Publishing a release's assets, as of 2026-10-06; every platform, one gate, as of 2026-10-07.** A release's page, notes and zips are made by hand. **Every release ships all eight zips -- Windows, Linux, macOS, both Android ABIs, CoreELEC and
  the two plugins -- and no platform is left out or held to a weaker check:** after the tag's CI run is green,
  fetch its three artifacts, build the hand-built ones (macOS, Android, CoreELEC) from the tag's own source,
  normalize them, and run `python3 tools/check_release_zip.py --require-blocklist --expect-release <version>
  <all eight zips>` once over the lot. That one script is the data-hardening bullet's asset checks (build-machine
  paths in any binary, UTF-16 included, the private blocklist, zip timestamps and extra fields, unstripped
  libraries, debug files) and the check that the set is complete; CI runs its pattern half on every zip it
  builds. Only after it passes, `gh release create <tag> --title <tag> --notes-file <notes> <zips...>` (the
  notes inline the bundled plugins' changelog entries, per the bullet above). The steps are in
  `docs/BUILDING.md`'s "Release assets"; a failing gate is fixed at its cause and the zip rebuilt, never
  by editing the gate. The pre-push privacy hook does not see release assets.
- **The CoreELEC package isn't part of CI** and won't be (see
  `docs/BUILDING.md`'s "GitHub Actions job ... rejected" note --
  CoreELEC's build harness assumes persistent, self-hosted infrastructure
  and a large pre-built toolchain cache, not a one-shot cloud runner). A
  real tagged release needs its CoreELEC zip built and attached to the
  GitHub Release by hand; the exact steps, including the `gh release
  upload` command, are documented at the end of that file's CoreELEC
  section.
- **Commit messages**: short imperative summary line, no conventional-
  commit type prefixes (no `fix:`/`feat:`) -- match the existing git log.

## License

GPL-2.0-or-later.

## Security

Report vulnerabilities privately per [SECURITY.md](SECURITY.md), not in
a public issue.
