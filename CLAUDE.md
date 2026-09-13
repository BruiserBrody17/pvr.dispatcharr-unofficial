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
  items there rather than losing track of them in conversation, in its
  "Ongoing" section.

## Building and testing

This addon cannot be compiled standalone -- it builds through Kodi's own
binary-addon build harness, which needs a matching Kodi source checkout.
Full instructions (Windows/macOS/Linux/CoreELEC, all previously verified
live) are in [docs/BUILDING.md](docs/BUILDING.md); don't guess at build
commands, read that file.

Small automated test suites exist on both the C++ and Python sides as of
2026-09-13, but both are deliberately narrow -- only Kodi/Dispatcharr-
independent pure/filesystem logic is covered anywhere.

**C++** (`tests/`, Catch2, wired into CI's `unit-tests` job, a standalone
CMake project separate from the addon's own `CMakeLists.txt` since that
one can only be configured through Kodi's own build harness -- see
`tests/CMakeLists.txt`'s own comment): `XmlTvParser`, `TimeUtil`,
`TimeZoneUtil`, `EpgTagUtil`, `StringUtil`, `DateTimeFormat`, `UrlEncode`,
`JsonFieldUtil`, `CurlCallbacks`, `CatchUpUtil` -- the last six pulled out
of `WebSocketClient.cpp`/`DispatcharrClient.cpp` specifically so this small,
widely-used logic (Base64/lowercasing, Dispatcharr's own date-time
string formats, curl-based URL escaping, the null-safe JSON field
reader nearly every response parse in `DispatcharrClient.cpp` goes
through, the plain libcurl write/header callbacks -- none of which
touch a `CURL*` themselves, despite conceptually being curl callbacks --
and the segment-duration-estimate/catch-up-attempt-count math shared by
live-timeshift and in-progress-recording playback, each with real
documented incidents behind them) is unit-testable standalone.
`PVRDispatcharr`/`DispatcharrClient`/`WebSocketClient`'s actual PVR API
surface, HTTP client, and socket handling -- where the real bugs live --
aren't attempted: that would mean mocking Kodi's entire addon-instance
API and/or standing up a fake Dispatcharr HTTP server, disproportionate
effort for what's fundamentally still manual/live-hardware verification
territory.

**Python** (`dispatcharr-plugin/{recording_edl,timeshift_buffer}/tests/`,
pytest, wired into CI's `unit-tests-python` job): `recording_edl` covers
`_parse_edl`, `_sidecar_base_name`, `_is_under_dotted_dir`,
`_prune_empty_directories`, `_resolve_scan_root`/`_dedupe_scan_roots`,
`_scrub_orphaned_recording_sidecars` end-to-end via `monkeypatch`ing its
one Django-model-dependent call, and `Plugin.run()`'s own dispatch and
response text for every action branch that's Django-free or where the
one Django-dependent call can itself be `monkeypatch`ed (the actual
response a client sees, not just the underlying helpers in isolation).
`timeshift_buffer` covers `_channel_dir`, `Plugin._resolve_channel_uuid`,
`_resolve_request_path`, `_BufferRequestHandler._parse_range`, `_proxy_url`,
`_stream_attribution_headers` (client_ip validation path only -- the
username/JWT branch stays untested), `_prune_stale_viewers`,
`_find_orphaned_channel_dirs`/`_scrub_orphaned_dirs` (via `monkeypatch`ing
their one Redis-dependent call), and `_get_live_manifest` end-to-end
against a real temp filesystem -- including regression tests for its
documented cache-invalidation incidents (the newest-segment-always-
restatted race and the instance-token "Packet corrupt" bug). Anything
touching Dispatcharr's own Django models or Redis directly (the deferred
imports inside `_dvr_sidecar_scan_roots`/`_classify_dvr_hls_dir`/
`Plugin.run`, and all of `_redis`/`_get_buffer_state`/etc., the real HTTP
server, and ffmpeg subprocess management) stays untested, on the same
principle as the C++ side stopping at the Kodi SDK boundary.

See `docs/OPEN_ITEMS.md`'s "No automated test suite exists" entry for
the full reasoning and what's still open on both sides. Verification of
everything else stays manual: smoke-testing against a real Dispatcharr
instance and real/emulated Kodi installs (Windows, macOS, Linux via Kodi
Flatpak, CoreELEC on an ODROID N2+), driven via Kodi's JSON-RPC webserver.
`.github/workflows/build.yml` also compiles
Windows/macOS/Linux and packages the two plugins -- it does not build or
test the CoreELEC package, and doesn't exercise runtime behavior on any
platform.

## Conventions specific to this repo

- **Docs split by audience**: `README.md` and each plugin's own `README.md`
  are concise and user-facing -- install/configure/use only. `docs/*.md`
  holds the "why" (investigations, root causes, API behavior confirmed
  against a live Dispatcharr instance, reverted approaches) and is never
  meant to be read by an end user. `CHANGELOG.md` is user-facing
  what-changed, not why. When you learn something new about Dispatcharr's
  API or fix a non-obvious bug, the explanation belongs in `docs/`, not
  buried in a commit message.
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
  specific deployment's real-world setup. The "Confirmed live" citations
  bullet above still applies -- be specific about *mechanism* (a real
  endpoint response, a real device test) without being specific about
  *whose* deployment it was tested against. Not hypothetical: cleaning
  up leaks of exactly this kind (real channel/show names, a real
  account username, real-instance location claims) took three separate
  passes in one session (2026-09-10/11), two of which needed a full
  `git-filter-repo` history rewrite and force-push across every branch
  and tag -- far more expensive after the fact than just genericizing
  the specific detail before it's ever committed.
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
  behavior) can still go straight to `master`, same as this project has
  done so far. Anything that changes real behavior -- a real bug
  investigation, a new feature, a risky refactor -- gets its own
  short-lived branch, merged back to `master` once verified, then
  deleted. This is deliberately not full GitFlow (no perpetual `develop`/
  `release` branches) -- there's only one person working on this repo
  right now, so there's no multi-contributor coordination problem to
  solve; the only goal is keeping `master` in a known-good state and
  having a diff to review before a change becomes permanent, given the
  unit test suite is narrow (see "Building and testing" above) and won't
  catch a half-finished change to the addon's actual PVR/HTTP logic.
  A branch that lives for a while (a real feature, not a quick fix)
  drifts out of sync with `master` as unrelated work merges in around
  it -- periodically run `git merge master` into it along the way rather
  than letting the gap grow for weeks and facing one large reconciliation
  at the end. This matters more here than on a project with real test
  coverage: a dependency whose *signature* changed underneath the branch
  fails loudly (won't compile) the moment master's changes are merged
  in, but a dependency whose *behavior* changed without its signature
  changing won't -- nothing will flag it except actually re-testing the
  branch after syncing, since the unit test suite doesn't cover this kind
  of behavioral drift either.
- **Every external contributor's PR gets a real manual review before
  merging, as of 2026-09-11 -- never auto-merged on green CI alone.**
  The bullet above's "no multi-contributor coordination problem" framing
  stops holding the moment someone outside the project opens a PR.
  Green CI (`build`/`lint`/`unit-tests`/`unit-tests-python`) only proves
  it compiles, formats cleanly, and doesn't regress the narrow
  unit-tested pieces -- it proves nothing about whether a change to the
  actual PVR/HTTP/Redis/Django-touching logic (untested, see "Building
  and testing" above) is correct, so it's necessary but never sufficient
  on its own. This doesn't require GitHub's "require pull request reviews"
  branch-protection rule to be technically enforced: only the
  maintainer has write access to this repo right now, so an outside
  contributor's PR literally cannot merge itself regardless -- this
  bullet exists to keep that a stated, deliberate policy rather than an
  accident of current permissions, so it doesn't quietly erode later
  (e.g. if another collaborator ever gets write access).
- **Batch fixes into releases -- don't tag/release per individual fix.**
  Early on this project tagged and released (including the full manual
  CoreELEC build-and-upload dance) after nearly every single bug fix,
  which was far more release overhead than the project's single-user,
  actively-testing phase warrants. Commit fixes to `master` as they land;
  only cut an actual tag/release when a meaningful batch has accumulated
  or a real test pass is about to happen against a batch of changes.
- **Merge branches with squash-merge, as of 2026-09-07** -- when a
  nontrivial branch (see above) is done, squash it into one commit on
  `master` rather than preserving every individual commit from the
  branch. Keeps `master`'s history readable as "one commit = one logical
  change" instead of a trail of in-progress "wip"/"fix typo" commits.
- **Code formatting/linting, as of 2026-09-07**: `.clang-format`
  (C++, `src/`) and `ruff.toml` (Python, `dispatcharr-plugin/`). Run
  `clang-format -i src/*.cpp src/*.h` and `ruff format
  dispatcharr-plugin/` before committing nontrivial C++ or Python
  changes; `ruff check dispatcharr-plugin/` catches some real bugs
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
  current `master` (same structure
  `package-dispatcharr-plugins`'s own step produces: the plugin's own
  directory as the zip's top-level entry) and `gh release upload
  <tag> <zip> --clobber` onto the existing release -- no new tag, no
  new addon build, this is a plugin-only exception to the
  don't-release-per-fix rule above. Then update that release's own
  notes with a dated addendum inlining the actual fix (per the bullet
  above), not just a pointer -- and don't leave the original notes'
  now-false "neither plugin changed" claim standing uncorrected.
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
