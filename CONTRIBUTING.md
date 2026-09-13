# Contributing to pvr.dispatcharr-unofficial

Thanks for looking at this. A few things worth knowing before you open
a PR -- this project is pre-1.0, single-maintainer, and has only a
narrow automated test suite, all of which shape how contributions get
handled here.

## Before you start

- **The automated test suite is narrow, on both sides.** `tests/`
  (Catch2, run via CI's `unit-tests` job) covers only C++ source files
  with no Kodi SDK dependency at all -- `XmlTvParser`/`TimeUtil`/
  `TimeZoneUtil`/`EpgTagUtil`/`StringUtil`/`DateTimeFormat`/`UrlEncode`/
  `JsonFieldUtil`/`CurlCallbacks`/`CatchUpUtil`/`RecurringRuleUtil`/`RecordingParser`/
  `PluginRunResult`/`RealtimeUpdateParser`/`M3u8SegmentParser`/`SegmentLookup` as of 2026-09-13. The addon's actual PVR API surface
  and HTTP/WebSocket handling (`PVRDispatcharr`/`DispatcharrClient`/
  `WebSocketClient`) aren't covered by anything automated.
  `dispatcharr-plugin/{recording_edl,timeshift_buffer}/tests/` (pytest,
  run via CI's `unit-tests-python` job) cover each plugin's
  Dispatcharr-independent pure/filesystem logic as of 2026-09-13 --
  neither plugin's Redis- or Django-model-touching code is tested.
  Verification of everything else is manual: smoke-testing against a
  real Dispatcharr instance and a real (or emulated) Kodi install. If
  your change touches the C++ addon or either Python plugin's actual
  behavior beyond what's covered, you'll need a way to test it live --
  a Dispatcharr instance you control, plus Kodi on at least one
  platform. If you can't test a change end-to-end, say so plainly in
  the PR rather than asserting it works.
- **The addon can't be built standalone.** It builds through Kodi's own
  binary-addon build harness. Full, previously-verified build steps for
  Windows/macOS/Linux/CoreELEC are in [docs/BUILDING.md](docs/BUILDING.md)
  -- start there rather than guessing at commands. The C++ unit test
  suite is a separate, plain CMake project (`tests/CMakeLists.txt`) that
  doesn't need any of that -- see its own comment.
- **CI only covers part of this.** `.github/workflows/build.yml`
  compiles the addon on Windows/macOS/Linux, packages the two plugins
  as zips, and runs both narrow unit test suites (`unit-tests` for C++,
  `unit-tests-python` for the plugins) -- it doesn't build or test the
  CoreELEC package, and doesn't exercise runtime behavior on any
  platform. Green CI means "it compiles, lints, and doesn't regress the
  unit-tested pieces," not "it works."

## Where things live

- `src/` -- the addon's C++ source.
- `pvr.dispatcharr-unofficial/` -- addon metadata Kodi actually loads
  (`addon.xml.in`, `resources/settings.xml`, the language file).
- `dispatcharr-plugin/timeshift_buffer/`, `dispatcharr-plugin/recording_edl/`
  -- the two server-side Python plugins, independent of the addon and
  of each other. Each has its own README.
- `docs/` -- engineering history for this project: root causes, things
  confirmed live against a real Dispatcharr instance, approaches tried
  and reverted. Written for people working on the code, not end users
  -- if you're looking for how to *use* the addon, see
  [README.md](README.md) instead.
- `docs/OPEN_ITEMS.md` -- the running punch-list of known gaps and
  in-progress investigations. Worth checking before starting something
  substantial, in case it's already tracked (or already ruled out).

## Writing your change

- **Format before you push.** `clang-format -i src/*.cpp src/*.h` for
  C++, `ruff format dispatcharr-plugin/` for Python. `ruff check
  dispatcharr-plugin/` catches some real bugs too (unused variables,
  etc.), not just style -- run it.
- **If you touch `XmlTvParser`/`TimeUtil`/`TimeZoneUtil`/`EpgTagUtil`/
  `StringUtil`/`DateTimeFormat`/`UrlEncode`/`JsonFieldUtil`/`CurlCallbacks`/
  `CatchUpUtil`/`RecurringRuleUtil`/`RecordingParser`/`PluginRunResult`/
  `RealtimeUpdateParser`/`M3u8SegmentParser`/`SegmentLookup` (or add new Kodi-independent C++ pure-logic code), run the C++ unit test suite**
  before pushing (needs libcurl's dev headers, e.g. `libcurl4-openssl-dev`
  on Debian/Ubuntu, for `UrlEncode`'s own test -- CI hit this as a real
  "Could NOT find CURL" failure the first time this suite gained that
  dependency):
  `cmake -S tests -B build-tests && cmake --build build-tests && ctest
  --test-dir build-tests --output-on-failure`. Add test cases for new
  behavior rather than just confirming existing ones still pass.
- **If you touch either plugin's pure/filesystem logic (or add new
  Dispatcharr-independent code), run the pytest suite** before pushing:
  `pip install -r dispatcharr-plugin/requirements-dev.txt && pytest`.
  Add test cases for new behavior rather than just confirming existing
  ones still pass.
- **Comments explain WHY, not WHAT.** Document non-obvious constraints,
  something you confirmed live, or a workaround for a specific bug --
  not a restatement of what the next line obviously does.
- **If you confirmed something against a real Dispatcharr instance,
  say so specifically** -- a real endpoint response, a real device
  test, the actual result. Dispatcharr's own API has changed shape
  across releases, so "I checked the real behavior" is much more
  useful here than "this should work per the docs."
- **Never include a real hostname, IP address, account username, or
  similar identifying detail** in code, comments, docs, commit
  messages, or the PR description itself -- including your own, and
  including anything from your own test instance's logs or output you
  might paste in as evidence. If you're documenting a real channel or
  programme name from your own setup, genericize it the way this
  project's own docs already do elsewhere (e.g. "Channel A"). This
  isn't a formality: getting this wrong after the fact means a git
  history rewrite, not just an edit.
- **Explain what you changed and why in the PR description**, including
  how you tested it (or that you couldn't, and why). This project's own
  commit history favors a real explanation over a one-line summary --
  a short PR that just says "fixes bug" with no context is much harder
  to review with no test suite backing it up.
- Don't worry about crafting a clean commit history on your own
  branch -- accepted PRs get squash-merged into one commit, so your
  branch's intermediate commits don't end up in `master`'s history
  either way.

## Versioning

The addon and each of the two plugins version independently (see
[CHANGELOG.md](CHANGELOG.md) for the pattern). You don't need to bump a
version yourself as part of a PR -- the maintainer handles that at
merge/release time. If you do want to include one, see `CLAUDE.md`'s
versioning bullets for the exact mechanics (there are a few
easy-to-miss spots per piece).

## What happens after you open a PR

Every external PR gets an actual manual review before merging -- never
auto-merged just because CI is green. With only narrow unit test
suites on both sides, passing CI proves the change compiles, formats
cleanly, and doesn't regress the unit-tested pieces -- not that a
change to the untested majority of this codebase (the addon's actual
PVR/HTTP logic, either plugin's Redis/Django-touching code) is correct,
so a real read-through matters more here than on a project with full
test coverage. This is a
single-maintainer project,
so review may take a while; that's not a signal your PR was rejected.

## Reporting a security issue

Don't open a public issue for a security report -- see
[SECURITY.md](SECURITY.md) for how to report privately instead.

## License

Contributions are made under this project's license,
GPL-2.0-or-later.
