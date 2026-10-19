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
- `kodi-repository/` -- the Kodi repository addon; `tools/build_kodi_repo.py` builds the site Kodi installs and updates
  the addon from (served by GitHub Pages from `gh-pages`; `docs/BUILDING.md`, "Distribution: the Kodi repository").
- `docs/` -- engineering history: root causes, live-confirmed API
  behavior, things tried and reverted. Not user-facing.
- `docs/OPEN_ITEMS.md` -- the project's running punch-list; add new open
  items there rather than losing track of them in conversation, in whichever
  category fits, and move an item to `docs/CLOSED_ITEMS.md` (don't delete
  it) once it's resolved, with a note of how it was confirmed. An older
  citation of `OPEN_ITEMS.md` for a resolved item means its entry there.

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
  CI runs it as `unit-tests`, `unit-tests-32bit` (built with `-m32`, under UBSan), `unit-tests-sanitized` (ASan/UBSan) and `unit-tests-tsan`. `tests/glue/` compiles the real
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
  fixes in `docs/CLOSED_ITEMS.md`). A change that needs more than a sentence or two here is probably a docs entry plus a
  pointer. `tools/tests/test_claude_md_size.py` fails above 30 KB.
- **Docs split by audience**: `README.md` and each plugin's own `README.md`
  are concise and user-facing -- install/configure/use only. `docs/*.md`
  holds the "why" (investigations, root causes, API behavior confirmed
  against a live Dispatcharr instance, reverted approaches) and is never
  meant to be read by an end user, except `docs/TROUBLESHOOTING.md`: the short, symptom-first guide for users
  (what you see, why, what to do), with the long accounts in `docs/TROUBLESHOOTING_DETAIL.md`. `CHANGELOG.md` is user-facing
  what-changed, not why. When you learn something new about Dispatcharr's
  API or fix a non-obvious bug, the explanation belongs in `docs/`, not
  buried in a commit message.
- **New or changed pure-logic code gets a test in the same change, not after.** On the testable side of the boundary in
  "Building and testing" above, a feature or fix adds Catch2/pytest coverage alongside it, extracting the logic into its own
  free function (the pattern used throughout `src/` and `dispatcharr-plugin/`) when it is buried in a mixed-purity
  function, so coverage never piles up into another catch-up backlog (`docs/CLOSED_ITEMS.md`'s "No automated test suite
  exists" entry). The untested side stays manual.
- **"Confirmed live" citations matter**: Dispatcharr is young and its API
  schema has changed across releases. Comments and docs here frequently
  cite exactly how something was confirmed (a real endpoint response, a
  real device test) rather than just asserting behavior -- keep doing
  that instead of trusting Dispatcharr's own docs/schema at face value.
- **Never let a real personal/network detail land in the repo.** Anything that becomes permanent here -- docs, code
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
  change done.** `docs/*.md` cites exact function/setting names and CI commands; `tools/check_doc_refs.py` (in CI) flags
  an outright-removed reference, but nothing catches a claim that has merely gone stale.
- **Comments explain WHY, not WHAT**: this codebase's existing comments
  document non-obvious constraints, confirmed-live findings, and
  workarounds for specific bugs -- not a restatement of the code. Match
  that style; don't add narrative comments describing what a block of
  code obviously does.
- **Pre-1.0 (`0.x`) versioning on purpose.** The project reached `1.0.x` once and stepped back (single user, real
  playback bugs still turning up, Dispatcharr itself pre-1.0); `0.x` means "anything may still change". Move to `1.0.0` on
  this project's own track record (testing beyond one person, no more playback-breaking bugs), not when Dispatcharr's
  version does.
- **Branch for anything nontrivial.** The dividing line is behavioural risk, not size: a trivial,
  low-risk fix (including a docs-only pass) goes straight to the release branch it belongs to (`Omega` or `Piers`, below); a
  bug investigation, feature or risky refactor gets a short-lived branch, merged once verified, then deleted. Not GitFlow:
  one maintainer, and the point is a known-good release branch and a diff to review. A branch that lives a while gets its
  base merged in along the way; a dependency whose *behaviour* changed without its signature fails nothing at compile time,
  so re-test after syncing.
- **Branches are named after the Kodi release they target: `Omega` (Kodi 21, this branch, now maintenance) and `Piers`
  (Kodi 22, new work), not `master`.** Kodi's own addons (e.g. `pvr.hts`) do the same; `KODI_BRANCH` in
  `.github/workflows/build.yml` is each branch's Kodi tree. Kodi 22 changed the PVR API enough that `Piers` was branched
  off `Omega` and the two release separately; `Omega` fixes are ported to `Piers` by hand, not merged (the `Piers` branch's
  `docs/BRANCHES.md` has what differs and what has been ported). Older text in `docs/` and in this file that says `master`
  describes the branch as it was then and is left as written; the public GitHub links that still say it are tracked in
  `docs/CLOSED_ITEMS.md`'s "Links to the public GitHub repo still name the master branch" entry.
- **Every external contributor's PR gets a real manual review before merging -- never auto-merged on green CI alone.**
  This is where the one-maintainer premise above stops holding. Green CI (`lint`, every `unit-tests*` job, `glue-harness`
  and the `build-unix`/`build-windows` builds; `build-windows` is skipped for a PR from a fork) proves it compiles, formats
  cleanly and doesn't regress the unit-tested pieces and the glue harness's scenarios -- nothing about the untested
  PVR/HTTP/Redis/Django-touching logic, so it is necessary but never sufficient. This is stated policy, not an accident of
  only the maintainer having write access, and holds if that changes.
  An accepted PR is applied as one squash commit that credits the contributor (`Co-Authored-By:` or `--author`), and the
  PR is closed with a link to it; see CONTRIBUTING.md, "What happens after you open a PR".
- **Merge branches with squash-merge**: a finished nontrivial branch (see above) becomes one commit on the branch it
  targets (`Piers` for new work, `Omega` for Kodi 21 fixes), so history reads "one commit = one logical change".
  When the target branch has not moved since the branch was cut and the squash commit's tree equals the PR head's
  (`git rev-parse HEAD^{tree}` against `git rev-parse <branch>^{tree}`), put `[skip ci]` on its own line in the squash
  commit's message: the PR's CI run already tested exactly that tree. If the target branch moved, the trees differ or the
  PR came from a fork (its run skipped `build-windows`), leave it off, so the push run tests the real result. A tag push
  always builds everything.
- **Code formatting/linting**: `.clang-format`
  (C++, `src/` and `tests/`) and `ruff.toml` (Python, `dispatcharr-plugin/`, `tests/glue/`
  and `tools/`). Run `clang-format -i src/*.cpp src/*.h tests/*.cpp tests/glue/*.cpp` and
  `ruff format dispatcharr-plugin/ tools/ tests/glue/` before committing nontrivial C++
  or Python changes; `ruff check dispatcharr-plugin/ tools/ tests/glue/` catches some real bugs
  (unused variables, etc.), not just style. `.clang-format`'s `SortIncludes: false` is deliberate: a couple of files rely
  on include order for `#if defined(_WIN32)` blocks.
- **Independent version numbers, decoupled since 1.0.1**: the addon, each plugin and the Kodi repository addon version separately. Bump only the
  piece whose files changed, and move all of that piece's version locations together:
  - Addon: `pvr.dispatcharr-unofficial/addon.xml.in` (`<addon version="...">`) and
    `packaging/coreelec/pvr.dispatcharr-unofficial/package.mk` (`PKG_VERSION`, with `PKG_SHA256` reset to the all-zeros
    placeholder until the tag exists and the real checksum can be computed). CoreELEC packages the addon only, so this
    pair moves only with the addon's version.
  - Each plugin: `plugin.json` **and** the `Plugin` class `version` attribute in its `plugin.py`. `plugin.json` is only the
    not-yet-trusted import preview; the class attribute is what Dispatcharr runs. Bumping only `plugin.json` is the
    easy mistake.

  - The Kodi repository addon: the `version` in `kodi-repository/repository.dispatcharr-unofficial/addon.xml.in` only,
    moved only when that file changes.

  A tag/release is addon-version-scoped (it triggers CI, which builds everything and publishes nothing) and carries both plugin zips whether or not their version moved.
  `CHANGELOG.md` says which piece(s) moved in an entry.
- **Releasing (every platform, one gate).** A tag is the moment history becomes something a stranger can clone, so before
  tagging run a data-hardening audit (an Opus subagent sweep over code, docs, full history and every hosting-side surface:
  release assets, issues/PRs) and resolve any real finding with a `git-filter-repo` rewrite, never a plain commit. The
  local pre-push and commit hooks are a backstop, not a substitute, and they do not see release assets. Batch fixes into
  releases; do not tag per fix. **Every release ships all eight zips** (Windows, Linux, macOS, both Android ABIs,
  CoreELEC, the two plugins), built from the tag's own source, and none is published until
  `python3 tools/check_release_zip.py --require-blocklist --expect-release <version> <all eight zips>` passes over the lot.
  A failing gate is fixed at its cause and the zip rebuilt, never by editing the gate. Steps, the release-notes rules and
  when a plugin-only fix replaces a shipped zip: `docs/BUILDING.md`'s "Release assets".
- **The CoreELEC package isn't part of CI** and won't be (see
  `docs/BUILDING.md`'s "GitHub Actions job ... rejected" note --
  CoreELEC's build harness assumes persistent, self-hosted infrastructure
  and a large pre-built toolchain cache, not a one-shot cloud runner). A
  release's CoreELEC zip is built by hand (that file's CoreELEC section) and published with the other seven through the
  gate in its "Release assets".
- **Commit messages**: short imperative summary line, no conventional-
  commit type prefixes (no `fix:`/`feat:`) -- match the existing git log.

## License

GPL-2.0-or-later.

## Security

Report vulnerabilities privately per [SECURITY.md](SECURITY.md), not in
a public issue.
