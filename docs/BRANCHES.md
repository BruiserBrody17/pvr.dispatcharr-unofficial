# Branches: Omega (Kodi 21) and Piers (Kodi 22)

*(Engineering notes, not user-facing. Opened 2026-10-14, when `Piers` stopped being mergeable from `Omega`.)*

## The model

Kodi names its addon branches after the Kodi release they build against, and so does this project: `Omega` is Kodi 21,
`Piers` is Kodi 22. Kodi 22's PVR API (9.2.0, minimum 9.2.0) refuses a binary built for Kodi 21's (8.3.0) and the other
way round, so each branch builds against its own Kodi tree (`KODI_BRANCH` / `KODI_COMMIT` in
`.github/workflows/build.yml`) and releases separately. `Omega` is the maintenance branch for Kodi 21; new work goes to
`Piers` unless it is a fix for something users of Kodi 21 hit.

`pvr.hts`, Kodi's own PVR addon, does the same (one long-lived branch per Kodi release, tags like `22.9.3-Piers` and
`21.2.6-Omega`); its version numbers encode the Kodi major, this project's do not.

## Versions and tags

- **One shared `0.x` line.** `Omega` releases are `0.13.x`; `Piers` releases start at `0.14.0`. The numbers never repeat,
  so a release asset's name (`addon-pvr.dispatcharr-unofficial-<v>-linux.zip`) is unique across both branches.
- **The addon version stays plain numeric** (`addon.xml.in`, `CHANGELOG.md` headings, zip names, `check_release_zip.py
  --expect-release`): Kodi reads a `-suffix` in an addon version as a revision.
- **Only the git tag and the GitHub release name carry the branch:** `0.14.0-Piers`, `0.13.1-Omega`. The existing
  `0.13.0` tag predates this and stays as it is. `build.yml`'s tag filter (`[0-9]*.[0-9]*.[0-9]*`) already matches a
  suffixed tag.
- **CoreELEC's `package.mk`** names the tag in `PKG_VERSION` (`0.14.0-Piers`), the way CoreELEC's own `pvr.hts` package
  does (`22.9.3-Piers`), because CoreELEC unpacks the tarball into `build/<PKG_NAME>-<PKG_VERSION>` and GitHub names a tag
  archive's top directory after the tag. `tools/tests/test_version_sync.py` ties it to `addon.xml.in` through
  `RELEASE_TAG_SUFFIX`, which is `-Omega` on `Omega` since its `0.13.1` (the `0.13.0` tag predates the suffix).
  Confirmed at the first Piers CoreELEC build (2026-10-16): the suffix does not reach the zip. CoreELEC
  writes `0.14.0.1` (the numeric version plus its own revision) into `addon.xml` and names the file
  `pvr.dispatcharr-unofficial-0.14.0.1.zip`; the library inside is `pvr.dispatcharr-unofficial.so.0.14.0`.

## Fixes that need to reach both

Until 2026-10-14 `Omega` was merged into `Piers` (last merge: `890ed3b`), because the two differed only in six
override signatures. The multiple-recorded-streams change (`147334e`, Piers only) rewrote the recorded-stream state in
`DispatcharrClient` and its callers, so a merge would conflict on every `Omega` fix touching those functions. From then on
**an `Omega` fix is ported to `Piers` by hand**: `git cherry-pick -x` where it applies, a hand port where it does not.
A fix made on `Piers` first goes the other way in the same manner; both directions are logged below.

What differs between the branches, beyond comments (a handful of comments name `ReadRecordingStreamImpl()` and the like):

| Area | Where |
|---|---|
| Recorded-stream state, API and wrappers | `src/DispatcharrClient.{h,cpp}` (everything with `InProgressRecordingStream` / `RecordingStream` in its name), `src/PVRDispatcharr.{h,cpp}` (the recorded-stream overrides, `GetCapabilities()`, the live-only `CanPauseStream()` / `CanSeekStream()` / `IsRealTimeStream()` / `GetStreamTimes()`), `src/RecordedStreamRegistry.h`, `src/InProgressSegmentCache.h` |
| Kodi ABI shape in the glue harness | `tests/glue/kodi_abi.h`, `collected.h`, `pvr_stub.cpp`, the recorded-stream parts of `pvr_driver.cpp` and `stream_driver.cpp`, `fake_dispatcharr.py` |
| Build pin | `.github/workflows/build.yml` (`Piers` branch trigger, `KODI_BRANCH`, `KODI_COMMIT`) |
| Release line | `addon.xml.in` and `package.mk` versions, `CHANGELOG.md` |
| Kodi 22 only | `src/StreamWarmUp.h` (the Off-mode warm-up) and `IsViewerRequestedOpen()` in `src/RecordingVisibility.{h,cpp}`, each with its test; `src/RecordedStreamRegistry.h` and the `InProgressSegmentCache.h` join check (above) with theirs; the matching lines of the root `CMakeLists.txt` and `tests/CMakeLists.txt` |
| Android link | the root `CMakeLists.txt` links brotli when it is present (Kodi 22's depends build curl with it, Kodi 21's do not) |
| Release gate and CI path filter | `tools/check_release_zip.py` names the branch's CoreELEC asset (`coreelec-aarch64` on `Piers`, `coreelec-armv7` on `Omega`); `tools/tests/test_version_sync.py` its tag suffix, `test_check_release_zip.py` the same; `tools/doc_refs_baseline.txt` each branch's own docs |

Everything else (the plugins, the rest of `tools/`, the other `src/` files, most docs) is the same file on both and
cherry-picks cleanly. A handful of small `src/` headers differ in comments only (`CatchUpUtil.h`, `LiveEdgeMargin.h`,
`RecordingHttpUtil.h`, `SegmentLookup.h`, `StreamPropertyUtil.h`, `StreamSeek.h`, checked 2026-11-12). The plugins are
shared code: a plugin fix is made once and lands on both. `tools/` is shared too, except where it names a branch's
assets, tag suffix or versions, as in the table.

### Ported from Omega

Last `Omega` commit accounted for: **`60bb1f8`** (everything up to it is on `Piers`; `5519d7a`, `c1daf84` and `7ef8e5d`
are ports from `Piers`, logged below). When porting, list every `Omega`
commit after the last one here (`git log --oneline <last>..Omega`), port or decide not to, record it below and move
"last accounted for" forward. A fix that is not ported on purpose says why.

| Omega commit | Ported as | Notes |
|---|---|---|
| `6cc7ee1`, `2ec3551`, `b0f542a` | already on `Piers` as `b26412c`, `48764de`, `ffe8c16` | The docs-audit pass, made on `Piers` first and ported to `Omega` without its Kodi 22-only parts. |
| `80a146f` | `git cherry-pick -x` (applied cleanly) | Made-up titles in the recording-directory tests, comments and docs. |
| `fe85ce3` | already on `Piers` as `59123ca` | The read-only audit's pure-logic fixes and two doc errors, made on `Piers` first and cherry-picked to `Omega`; only `OPEN_ITEMS.md`, `CLOSED_ITEMS.md` and `CHANGELOG.md` needed a hand fix there. |
| `66722df` | not ported: `Piers` already lists them | The read-only audit's 21 open items, copied to `Omega`'s `OPEN_ITEMS.md` and checked against its code (three reworded for `Omega`). |
| `7ae30bb` | already on `Piers` as `3a0897b` | A finished recording's file opened when its leftover HLS playlist is gone; made on `Piers` first and ported to `Omega` by hand (one recorded-stream slot there). |
| `8c69f44` | not ported: `Omega`-only wording | Says Kodi 21 never opens PVR recordings for thumbnails, so its quiet no-file fallback is a safeguard there; `Piers` has the Kodi 22 side in `ef4760b`. |
| `ad7e720` | already on `Piers` as `67ca439` | The timeshift reaper fix (`timeshift_buffer` 0.8.15), made on `Piers` first; `git cherry-pick -x` to `Omega`, only the running docs needed a hand fix. |
| `c51d455` | already on `Piers` as `f87f4cb` | Play and catch-up off Kodi's main thread, made on `Piers` first; on `Omega` `GetRecordingStreamProperties()` was ported by hand (no thumbnail-notice bookkeeping there) and the catch-up comment names Kodi 21's call chain. |
| `4340b7d` | already on `Piers` as `b342957` | Background tasks refused during shutdown, made on `Piers` first; the code applied cleanly, `dtor_late_report` was merged into `Omega`'s glue harness by hand. |
| `14db1cf` | already on `Piers` as `7abb9d9` | Nested JSON read in place, made on `Piers` first; applied cleanly apart from the running docs. |
| `b129c23` | not ported: `Piers` already lists them | The two open items from the 2026-10-19 live check (`ef2492c`), copied to `Omega`'s `OPEN_ITEMS.md` and checked against its code. |
| `639ae6e`, `d70d857`, `c52f392` | already on `Piers` as `984df9c`, `1c7d817`, `d9a3e74` | Ports from `Piers`; see the table below. |
| `1542cd3` | not ported: `Piers` closed it on 2026-10-15 | `Omega`'s copy of the Kodi 22 support item, still open there after `Piers` resolved it, moved to `Omega`'s `CLOSED_ITEMS.md` with a pointer to `Piers`'s entry and this file. |
| `426ebd5`, `5b148f6` | already on `Piers` as `056ba0b`, `7219c8e` | Ports from `Piers`; see the table below. |
| `c08f1c4` | already on `Piers` as `59a13fc` | The live check of the throttle and guide redirect fixes; see the table below. |
| `955c4ae` | already on `Piers` as `b53484f` | A port from `Piers`; see the table below. |
| `5174a8c` | not ported: `Omega`-only record | The Kodi 21.3 live check of the fatal-stream fix, noted under `Omega`'s copy of the entry; `Piers`'s has the Kodi 22 one. |
| `aded040`, `c28882c`, `202f7d3`, `1e259a1`, `baec5ef`, `797bd83`, `f55a57c`, `ba08b01`, `0d113ae`, `cce3e4c`, `1bb7a2d`, `fe141f3`, `e7f409d` | already on `Piers` | Ports from `Piers`; see the table below. |
| `fc1a677`, `b4eba68` | not ported: `Omega` release line | `0.13.1` and its CoreELEC checksum. The tag suffix it brought to `Omega` (`RELEASE_TAG_SUFFIX`, `PKG_VERSION`, the `BUILDING.md` note) is what `Piers` already had. |
| `64d0c41` | already on `Piers` as `b1ab0ed` | A port from `Piers`; see the table below. |
| `2617019` | already on `Piers` as `873c98d` | A port from `Piers`; see the table below. |
| `bbed9f0`, `956dd2a`, `8889482`, `83ea1a7`, `1f17183`, `34bc486`, `58b4b7a`, `3d8f7c8`, `eed9bd1`, `3451c4c`, `3c406af`, `27e4bf5`, `03b3307`, `8ea5aa1`, `bfc16ba`, `842e346`, `9d01709`, `552d704`, `40dd79f`, `d2c3c6b`, `7a9c511`, `2783b59`, `30edae9`, `eda478c`, `a7ec36a`, `34e0c75`, `a2988aa`, `1877892`, `0ca28db`, `10d7bd7`, `42556c6`, `6fc5360`, `06130d3`, `26f9231`, `c262f64`, `a920e11`, `5dbe8ef`, `08e0aa2`, `56eec11`, `674835e`, `98b99aa`, `2ed5972`, `27aa589`, `af68f04` | already on `Piers` | Ports from `Piers`; see the table below. |
| `583cfe1`, `c2e2576` | not ported: `Omega` release line | `0.13.2` and its CoreELEC checksum; `Piers` has its own `0.14.2` (`75a1ca8`, `8f6ae77`). |
| `e292c44`, `4a2aaa3`, `caca234`, `ce1ed11`, `f27ba96`, `dd8190a`, `f40c6d2`, `a231242`, `c472413`, `50d563c`, `d2dc3d6`, `b517ebb`, `d2922a3`, `16d8932`, `ccf20ce`, `04b0aa4`, `23f7433`, `757c5fc`, `ca22083`, `077e620`, `51c9fdb`, `7a5919c`, `8a4acca`, `740dc4d` | already on `Piers` | Ports from `Piers`; see the table below. |
| `43a8b7f`, `209fc1a` | not ported: `Omega` release line | `0.13.3` and its CoreELEC checksum; `Piers` has its own `0.14.3` (`07eaf96`, `fb36f8b`). |
| `7d3cb5a` | not ported: Kodi 21 only | Files the delay seen on Kodi 21 between a realtime event and the timers it announced; Kodi 22 showed them within 15 s. The `Omega` commits between `209fc1a` and it are ports from `Piers`, each logged under "Ported to Omega". |
| `60bb1f8` | `16e6caf` (`git cherry-pick -x`, applied cleanly) | The live check of the Kodi-version hold with the `Omega` build of the port, docs only |

### Ported to Omega

Last `Piers` commit accounted for: **`6e3e0d3`**. As above, in the other direction: list every `Piers` commit after it
(`git log --oneline <last>..Piers`), port it to `Omega` or record why not, and move "last accounted for" forward. A
commit that only edits this file (it exists on `Piers` alone) needs no row, nor does the merge `890ed3b`. Most `Piers`-only commits are Kodi 22 work
with nothing to port; the first sweep (2026-10-19) went back to the split.

| `Piers` commit | On `Omega` | Notes |
|---|---|---|
| `dd77977`, `147334e`, `aa805aa`, `3c63eb4`, `506811b` | not ported: Kodi 22 only | The port to Kodi 22's addon API, the recorded-stream registry keyed by Kodi 22's stream id, and this branch's `0.14.0` line and CoreELEC checksum. |
| `184128c`, `aba6bd4`, `f9c6970`, `db1e067`, `5fb5295`, `ed07865`, `1594bfa`, `222ab1a`, `20d9aca` | not ported: Kodi 22 only | The Kodi 22 RC1 live checks and platform builds, the README's Kodi 22 line, the Kodi 22 open item's release note, and a privacy rewording of three of those records. |
| `f9e4f67` | not ported: Kodi 22 only | The Off and Local mode warm-up. The 503 comes from Kodi 22's extra probe requests; Kodi 21 sends one plain `GET` and plays (`CLOSED_ITEMS.md`, "Kodi 22: the first attempt to play a channel nobody is watching failed in Off mode"). |
| `789184e`, `6538c1e`, `ef4760b` | not ported: Kodi 22 only | Two streams of one in-progress recording, the segment cache kept for Kodi 22's stream-details probe of a finished recording, and the no-file notice shown only on Play. Kodi 21 opens one recorded stream at a time and never opens a recording for its thumbnail (`8c69f44`). |
| `984df9c` | `639ae6e` (`git cherry-pick -x`; `TEST_COVERAGE.md` by hand) | The smoke harness waits for a seekable player in its recording seek checks. The slow start was the device's, not Kodi 22's. |
| `1c7d817` | `d70d857` (`git cherry-pick -x`) | The troubleshooting entry for the Kodi 21 to 22 guide database upgrade failure: the people who hit it are Kodi 21 users, who read `Omega`'s docs. |
| `d9a3e74` | `c52f392` (by hand) | The live check of four audit fixes ran on Kodi 22; `Omega`'s closed entries now say what it showed and that it was not repeated there. The open item it found was fixed on both (`3a0897b`, `7ae30bb`). |
| `b26412c`, `48764de`, `ffe8c16`, `cf80c8e`, `9e6061b`, `59123ca`, `3a0897b`, `67ca439`, `f87f4cb`, `b342957`, `7abb9d9`, `ef2492c` | their counterparts in the table above | Made on `Piers` first and already logged there. |
| `b1ab0ed` | `64d0c41` (`git cherry-pick -x`; `CHANGELOG.md`, `OPEN_ITEMS.md` and `TEST_COVERAGE.md` by hand) | The Kodi repository; ported after its first publish, step (5) of `CLOSED_ITEMS.md`'s "The Kodi repository is built but not published". The repository addon's changelog entry is dated with that publish on both. |
| `056ba0b` | `426ebd5` (`git cherry-pick -x`; one test and the running docs by hand) | The release-gate hardening. One test named `Piers`'s `coreelec-aarch64` zip; it now takes the CoreELEC name from the gate's own list on both branches. The hardened gate passes over the shipped 0.13.0 release. |
| `7219c8e` | `5b148f6` (`git cherry-pick -x`; by hand where noted) | The manifest throttle and six audit quick wins. `Omega`'s single in-progress stream state took the attempt stamp by hand; `m_apiKeyOwnershipVerified` stays a plain `bool` there (Kodi 21 opens one recorded stream at a time); the glue harness took only the new scenarios, with an `nlohmann/json.hpp` include its stream driver lacked. The new scenarios give the same numbers on both. |
| `5352d79` | already in `426ebd5` | Its one code change, the CoreELEC name in a release-contents test, was made the same way in the port; the rest is this file. |
| `59a13fc` | `c08f1c4` (by hand, the same text) | The live check of the throttle and guide redirect fixes on Kodi 22.0 RC1 and Kodi 21.3, and the open item for a fatal live stream that leaves Kodi buffering, which applies to both. |
| `b53484f` | `955c4ae` (`git cherry-pick -x`; the running counts by hand), then `5174a8c` | A fatal live stream answers 0 and tells the viewer, so Kodi ends playback; the same reader code on both. Confirmed live on Kodi 21.3 as well. |
| `46598ae` | `aded040` (by hand, the same results) | The live check of three of `7219c8e`'s fixes: owner stamps written only when they change (Kodi 21.3 and 22.0 RC1), the padding re-check (throwaway `Omega` builds on Kodi 21.3) and the Windows build's older-curl redirect options (`Omega`'s Windows build on Kodi 21.3). |
| `e177b55`, `93b189f` | `c28882c`, `202f7d3` (by hand, the same text) | Two closing notes and one open item: the owner stamps left stale when only the address changes (open), and two outside-review findings closed without a change (no token reaches `kodi.log`, checked live on Kodi 21.3; the plugin's JWT for `params["username"]`). |
| `8f8b36f` | `1e259a1` (`git cherry-pick -x`, without `[skip ci]`) | The code of conduct, the PR template and the issue chooser's config. The templates are read from the default branch, `Omega`. |
| `bd9ff8e` | `baec5ef` (`git cherry-pick -x`; the running counts by hand, without `[skip ci]`) | The API-key owner stamps are rewritten when the key is confirmed unchanged, so a changed form of the server's address no longer costs a key lookup at every start. The same code on both; confirmed live on Kodi 22.0 RC1 and Kodi 21.3. |
| `9fa1252`, `06980f5`, `7f56c6d`, `dd52033` | `797bd83`, `f55a57c`, `ba08b01`, `0d113ae` (`git cherry-pick -x`; the running counts by hand) | The lab scripts' passwords kept off command lines, and `timeshift_buffer` 0.8.15's hardening and teardown-race fix with the addon's matching stop token. The plugin is the same file on both; the addon and glue changes applied cleanly, and `Omega`'s glue harness passes the new `live_stop_token` scenario. `OPEN_ITEMS.md` keeps `Omega`'s own counts and wording. |
| `df0e9b1` | `cce3e4c` (`git cherry-pick -x`) | The live check of those `timeshift_buffer` fixes, run with `Omega`'s build on Kodi 21.3; the plugin and the addon's stop code are the same on both. |
| `bc64873` | `1bb7a2d` (`git cherry-pick -x`; applied cleanly) | The tombstone named the way the orphan scrub looks for it, found by the second live pass, and that pass's record. The same plugin file on both. |
| `5208c53` | `fe141f3` (`git cherry-pick -x`; the running counts and the entry's opened line by hand) | A recurring-rule edit diffed against the report Kodi's timer dialog was opened from, found by a token in the timer's series link. The same code on both (the series link is a `char` array in Kodi 21's `PVR_TIMER`, behind the same wrappers); reproduced and confirmed fixed live on Kodi 22.0 RC1 and Kodi 21.3, each with its own branch's builds. |
| `28aae26`, `7edeaa9` | `e7f409d` (one commit, by hand: `OPEN_ITEMS.md` and the glue README differ by branch) | `run_tsan.sh` retrying TSan's own start-up segfault (the script is the same file on both), and the `curl_global_init` and `recording_edl` scan-root audit items closed without a change, with the constructor comment at the first request (the same constructor on both). The glue README's note that `Piers` needs Kodi 22's headers is `Piers`-only. |
| `759dff9`, `41c4f31` | not ported: `Piers` release line | `0.14.1` and its CoreELEC checksum; `Omega` has its own `0.13.1` (`fc1a677`, `b4eba68`). |
| `3bc23f4`, `e819d6e` | not ported: `Piers`-only item | The Kodi repository's open item lived on `Piers` alone: its release names, then its closure. The changelog date is already in `64d0c41`. |
| `91666c9` | not ported: `Piers`-only item | The Kodi 22 install from the published repository, added to the closed entry that exists on `Piers` alone. |
| `873c98d` | `2617019` (by hand: `BUILDING.md` only) | "The GitHub repository" instead of "the public repository"; the other five spots are in that `Piers`-only closed entry. |
| `c29030f` | `bbed9f0` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The 2026-10-23 bug-hunting sweep's 17 open items, checked against `Omega`'s code: every one holds there (the plugin, `build_kodi_repo.py` and the time and text helpers are the same files, and the addon code at each finding's site matches). |
| `97b53e9` | `956dd2a` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The Dispatcharr 0.33.0 review's three live checks. `Omega` has no Kodi 22 warm-up, so its copy of the start-up item leaves out the first-play bullet; the guide-history, catch-up seek, slow-start and stream-switch checks apply as written. |
| `04f2e77` | `8889482` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The plugin heartbeat race the integration run turned up; `timeshift_buffer/plugin.py` and the harness are the same on both branches. |
| `338fde4` | `83ea1a7` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The Dispatcharr 0.33.0 live checks. `Omega`'s start-up entry leaves out the Kodi 22 warm-up result (no warm-up there); the API notes section, the troubleshooting step and the catch-up and guide records are the same. The output-format fix itself merged on 2026-10-24 (`ebb2b40` on `Piers`, `58b4b7a` on `Omega`). |
| `0501917`, `f60e6ee` | `1f17183`, `34bc486` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The "Fix known, not yet done" heading put back (moving the 0.33.0 start-up item had carried it into `CLOSED_ITEMS.md`), then the second 2026-10-24 sweep's seven findings: all hold on `Omega` (the plugin, `build_kodi_repo.py` and `ConnectionReport` are the same files; the gate differs only in the CoreELEC asset name). |
| `ebb2b40` | `58b4b7a` (by hand) | Pin the live proxy's output format to MPEG-TS, confirmed live on Kodi 22.0-RC1 and, for the port, Kodi 21.3. `Omega` has no Kodi 22 warm-up, so `tests/test_stream_warm_up.cpp`'s case and the glue `warm_up` URL check are not ported; the plugin, `StreamPropertyUtil` and the CHANGELOG entries are the same. |
| `e06d801` | `3d8f7c8` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The codec-changing stream switch filed as a live check, and the over-long "As opened" paragraphs in `CLOSED_ITEMS.md` reflowed (one on `Piers`, two on `Omega`, whose start-up entry has its own as-opened text). |
| `c3be196` and the next commit | `eed9bd1`, `3451c4c` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The upstream note that Dispatcharr's own DVR, HDHR and M3U output follow a non-MPEG-TS Default Output Format, then its paragraph rewrapped. |
| `c26c06b` | `3c406af` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The codec-changing stream switch checked live (Kodi 21 freezes in every mode; Kodi 22 only in Server-side mode; now a design question) and the Kodi 21 Server-side HEVC stall item. Both hold on `Omega`, whose plugin and live-read path are the same. |
| `539993f`, `09c4bd6` | `27e4bf5`, `03b3307` | The troubleshooting entry for a stream switch that changes video format, the 0.33.0 switch time in the stalls entry, then that bullet rewrapped; the same text on both. |
| `0a8cb0e` | `8ea5aa1` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The Server-side HEVC stall root-caused (an HEVC channel's opening seek probe answered with 0 leaves Kodi at end of file) and moved to "Fix known"; `Omega`'s live read path has the same give-up and the same `return 0`. |
| `67d7d24` | `842e346` (`git cherry-pick -x`; `OPEN_ITEMS.md` and the closed entry by hand) | A heartbeat that arrives after its viewer's own stop is ignored (`timeshift_buffer` 0.8.16). The plugin is identical on both branches before and after; `Omega`'s closed entry keeps its own as-opened text, and `Omega`'s `OPEN_ITEMS.md` has no release or CI item left. |
| `adeec73` | `bfc16ba` (`git cherry-pick -x`; `OPEN_ITEMS.md` and the closed entry's first line by hand) | A live seek probe that finds nothing answers -1, not 0. The bug was Kodi 21's: confirmed live with a Kodi 21.3 build of the port, and with the `Piers` build on Kodi 22.0-RC1, which never seeks on opening HEVC. |
| `e3b0492` | `9d01709` (by hand: `OPEN_ITEMS.md` keeps `Omega`'s own counts) | The live check of `timeshift_buffer` 0.8.16's late-heartbeat fix, tracked as a live check; the plugin is the same on both branches. |
| `933e5b1` | `3451c4c` | The upstream output-format note in `OPEN_ITEMS.md` rewrapped; the same text on both. |
| `e9bee5b` | `552d704` | The tzdata 2026e Manitoba change tracked in `OPEN_ITEMS.md`; `TimeZoneUtil`'s table and its checks are the same on both branches. |
| `56f72a3` | `d2c3c6b` (by hand: `Omega`'s step 3 has no Kodi 22 paragraph) | The CoreELEC build host's packages listed up front in `BUILDING.md`. |
| `2ad65f7` | `40dd79f` (`git cherry-pick -x`) | A contributor's own time zone left to them in `CONTRIBUTING.md` and the PR template. |
| `10678f5` | `7a9c511` (`git cherry-pick -x`, applied cleanly) | shellcheck in CI's lint job and the lint tools' versions in `tools/requirements-lint.txt`; the glue scripts it cleans up are the same on both, and the glue harness passed on Kodi 21 too. |
| `7bbeebd` | `2783b59` (`git cherry-pick -x`; the `Releasing` bullet by hand) | The shorter release and CoreELEC bullets in `CLAUDE.md`. `Omega`'s copy also says its tags and `PKG_VERSION` carry the `-Omega` suffix, which it had not said since `0.13.1`. |
| `0277051` | `30edae9` (`git cherry-pick -x`) | `TIMESHIFT.md`'s stall table says a seek probe's give-up answers -1 (`LiveTailGiveUpResult()`), and its lead-in that one 0 sets Kodi's end of file until a seek. |
| `0c6fc56` | `eda478c` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The two other live-read paths that answer 0 while the buffer is alive, filed under "Needs a live check"; `Omega`'s `ReadLiveTimeshiftStreamOnce()` has the same two. |
| `e71d531` | `a7ec36a` (`git cherry-pick -x`, applied cleanly) | The glue harness's `live_seek_probe` scenario, which fails with the call site put back to `return 0`; it passed against Kodi 21's headers too. |
| `f46e452` | `34e0c75` (`git cherry-pick -x`, applied cleanly; `TEST_COVERAGE.md`'s count by hand) | `check_doc_refs.py` looks cited function names up in a set; the checker was the same on both branches, and `Omega`'s docs give 161 identical findings to `Piers`'s 168. |
| `4611863` | `a2988aa` (`git cherry-pick -x`) | Two repeated `LiveTailGiveUpResult()` checks dropped from its unit test case; the test was the same on both branches. |
| `c8076c5` | `1877892` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand) | Manitoba kept on UTC-5 from 2026-11-01 in `TimeZoneUtil`'s table (tzdata 2026e), with the tests and the tzdata pytest's 2026c/2026d leave-out; the table and its checks were the same on both branches. |
| `42c899f` | `0ca28db` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The live check of `timeshift_buffer` 0.8.16's late-heartbeat fix closed, and the tz data Dispatcharr's scheduler reads (`ZoneInfo`, the image's system copy) recorded; the plugin and the docs' text are the same on both. |
| `7f07461` | `10d7bd7` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The guide-history re-measure on Dispatcharr 0.33.0 closed without a change; the guide fetch is the same code on both. |
| `0fad167` | `42556c6` (`git cherry-pick -x`, without `[skip ci]`; the CoreELEC library kind, one test case and the running docs by hand) | The release gate's three fixes from the 2026-10-23 and 2026-10-24 sweeps. `Omega`'s CoreELEC asset is `-coreelec-armv7`, an ELF32 ARM library, and its wrong-platform test feeds that name Kodi 22's 64-bit build. The ported gate passes the shipped 0.13.1 set beside that tag's own plugin sources. |
| `8bc634a` | `6fc5360` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and one entry's CoreELEC name by hand) | The repository tool's three fixes from the same sweeps. The tool and its tests are the same on both branches: it knows both Kodi lines. |
| `9037649` | `06130d3` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | `timeshift_buffer` removes a channel's directory only after releasing its start lock (the 2026-10-23 sweep's MEDIUM), and the gevent open item its review filed. The plugin is the same file on both branches before and after; the live check on Dispatcharr 0.33.0 ran with that file. |
| `fbd0e6b` | `26f9231` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand) | A Redirect-profile channel's buffer gets no token or viewer address on ffmpeg's `-headers` (the 2026-10-24 sweep's other MEDIUM). The plugin is the same file on both branches before and after; the live check on Dispatcharr 0.33.0 ran with that file. |
| `d646434` | `c262f64` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand) | The live checks of the sustained-outage item (closed without a change) and the live-read-0 item (recycled segment confirmed, moved to "Fix known, not yet done"). Both ran on `Omega`'s own CI build on Kodi 21, and both items read the same on both branches. |
| `403c404` | `a920e11` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | Files the flaky `test_concurrent_first_calls_start_exactly_one_http_server` as an open item; the test and the plugin are the same on both branches, and the failure it records was on `Omega`'s run. |
| `a5ef0b5` | `5dbe8ef` (the branch's three commits `git cherry-pick -x`'d onto `Omega` and squash-merged; `OPEN_ITEMS.md`'s counts by hand) | A 404 for a recycled live segment goes to the bounded segment-fetch retry instead of answering 0, with the glue scenario `live_segment_recycled`. The code is the same on both branches; the live check on Kodi 21.3 ran with the `Omega` build of it, and the glue harness passed against both branches' headers. |
| `0371335` | `08e0aa2` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | Files the Stop freeze during failed live segment fetches as an open item; the read loop is the same on both branches, and it was measured on `Omega`'s build. |
| `2f1d59b` | `56eec11` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand, which leaves `Omega` with no open release-and-CI item) | The `timeshift_buffer` server-lock tests use a stub server instead of a real bind (the flaky-test open item, closed). Test-only; the plugin and its tests are the same on both branches. |
| `acdcbe2` | `674835e` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | Files the outage notice that blames the file server's port as an open item; the give-up path and the message are the same on both branches, and the outage was seen on `Omega`'s build. |
| `77c70e9` | `98b99aa` (the branch's three commits `git cherry-pick -x`'d onto `Omega` and squash-merged; `OPEN_ITEMS.md`'s counts by hand) | A failing live read is handed back to Kodi with -1 after a one-second slice, so a Stop during an outage is not held, with the glue scenario `live_outage_handback`. The read loop is the same on both branches; the live check on Kodi 21.3 ran with the `Omega` build of it, and the glue suite passed against both branches' Kodi headers. |
| `2f43a87` | `2ed5972` (the branch's five commits `git cherry-pick -x`'d onto `Omega` and collapsed into one commit citing `2f43a87`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand) | When a whole-server outage ends live playback, the notice says the server can't be reached instead of naming the file server's port, including after the access token lapsed during the outage, with the glue scenario `live_lost_notice`. The give-up path and `EnsureAuthenticated()` are the same on both branches; the live check on Kodi 21.3 ran with the `Omega` build of it, and the glue suite passed against both branches' Kodi headers. |
| `65bf59c` | `27aa589` (`git cherry-pick -x`) | Records the Kodi 22 live checks of the outage-notice, Stop and recycled-segment fixes in `CLOSED_ITEMS.md`; the entries are the same on both branches. |
| `1a5d93e` | `af68f04` (`git cherry-pick -x`) | Records the remaining four Kodi 22 live checks of the Stop fix in `CLOSED_ITEMS.md`. |
| `75a1ca8`, `8f6ae77` | not ported: `Piers` release line | `0.14.2` and its CoreELEC checksum; `Omega` has its own `0.13.2` (`583cfe1`, `c2e2576`). |
| `9000c0c` | `e292c44` (`git cherry-pick -x`) | Records the throwaway-build live check of the live-read gap in `OPEN_ITEMS.md`; the entry is the same on both branches. |
| `93789bb` | `4a2aaa3` (`git cherry-pick -x`, without `[skip ci]`) | Six fixes from the 2026-10-23/24 bug sweeps: `timeshift_buffer` 0.8.17 (trimmed path and base URL, `segment_seconds` capped at 20), the rotated refresh token, the key saved before the owner stamps on a recording open, plain programme fields only in a guide timer, and C1 controls blanked in logged server errors. The code and the glue harness applied cleanly (Omega has the same cold-start constants); `CHANGELOG.md` and `OPEN_ITEMS.md` were resolved by hand with `Omega`'s own counts. The live checks recorded in `CLOSED_ITEMS.md` were made on Kodi 22 only (Kodi 21 followed, next row). |
| `6025b4f` | `caca234` (`git cherry-pick -x`) | Records the Kodi 21 live checks of those fixes and corrects two claims, the same in `CLOSED_ITEMS.md`, `CHANGELOG.md` and a `plugin.py` docstring on both branches; the shortened open-items summary was written for each branch's own counts by hand. |
| `dfd0b2e` | `ce1ed11` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand, and the two recorded-read functions under their `Omega` names) | Files the 2026-10-31 bug sweep's eleven findings as open items; all eleven were checked against `Omega`'s code and hold there (its gate differs only in the CoreELEC asset name). |
| `d6f744a` | `f27ba96` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro and one `BUILDING.md` sentence by hand) | The release gate's four fixes from the 2026-10-31 sweep: a plugin zip checked against the tree byte for byte, desktop metadata, local-header times, encrypted entries and a missing `$PRIVACY_BLOCKLIST`. The ported gate passes the shipped 0.13.2 set. |
| `ea862ff` | `dd8190a` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | `timeshift_buffer` 0.8.17's file-server hardening: 503 on a state-read failure, 404 on a symlink loop, throttled logs for both and for a taken port, and the Streams allowlist wording and 403 message, with the 2026-11-01 live checks; `plugin.py` is identical on both branches. |
| `43089ad` | `f40c6d2` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | A live read that lands in a gap goes through the bounded segment-failure handler instead of answering 0, with the `live_read_gap` glue scenario; the 2026-11-01 second-thread live check was run on an `Omega` build. |
| `7372d63` | `a231242` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | Five LOW fixes: 64-bit date conversion without the libc and four-digit date formatting, Unicode trimming of recording folder names, no channel-groups fallback route, -1 for a 401 whose re-login failed, and the `TrackDetachedThread()` join; the `auth_reauth_failed` glue scenario applied unchanged. |
| `c025297` | `c472413` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | A work bound on series-rule text matching: each distinct term searched for once per call, operands after a settled result skipped, and `kUnsupported` past `kMaxSeriesMatchWork` |
| `2533613` | `50d563c` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand) | The live checks of the heartbeat, recording Stop and spring-gap items, docs only |
| `a68be4b` | `d2dc3d6` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro and the plugin's `CHANGELOG.md` line by hand) | The live heartbeat sends the buffer's token, `timeshift_buffer` 0.8.17 declines a replaced buffer's token on a heartbeat or manifest write-back, and a length or duration poll that gets a fatal answer ends the resume; with the `live_paused_replaced` glue scenario |
| `55713ed` | `b517ebb` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand) | Comments on why a recording's read retries stay inside one call, the recording Stop item made a design question, and the new live-check item for the in-progress tail wait |
| `5f4b867` | `d2922a3` (`git cherry-pick -x`) | The live check of the heartbeat fix's plugin half, docs only |
| `925c7b7` | `16d8932` (by hand, for `OPEN_ITEMS.md`'s counts) | The live check of the in-progress tail wait under a recorder stall, docs only |
| `66040e9` | `ccf20ce` (by hand on a branch: cherry-picks with the stream's member and helper names, `OPEN_ITEMS.md`'s counts and the closed entry's function name by hand; the `CHANGELOG.md` line without the Kodi 22 clause) | The in-progress tail wait waits in one read again (the one-second slices ended a recording's playback a few seconds into a recorder stall), with `ip_tail_bound` asserting it |
| `8da329a` | `04b0aa4` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand, the `CHANGELOG.md` line without the Kodi 22 sleep clause) | The connection state repeated to Kodi on wake and on each recording-refresh tick, and an account answer with no level settling the access level. On Kodi 21 only a PVR manager restart drops a report |
| `52b19f0` | `23f7433` (`git cherry-pick -x`, without `[skip ci]`; the recorded-stream signatures, which carry no stream id on `Omega`, the `OpenRecordedStream()` handler and `OPEN_ITEMS.md`'s counts by hand), then `757c5fc` | Every PVR callback, the instance's create and `SetSetting` catch an escaping exception (42 overrides on `Omega`). `757c5fc` corrects the handlers' comments for Kodi 21, which closes a stream after a failed open when the player tears it down |
| `d1991fb` | `ca22083` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro and the plugin's `CHANGELOG.md` context line by hand) | `timeshift_buffer` lists at most 7200 segments (`_MAX_VISIBLE_SEGMENTS`), with a warning when the buffer length is shortened to it, and the live check of the cap |
| `cd7d678` | `077e620` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | A `timeshift_buffer` start renews its channel and slot locks before the spawn, after a reap that stopped something and before the state write, and answers retryable if either was lost; the owner file is written before the first Redis call after the spawn and never replaces another running ffmpeg's. With the live check of the fix |
| `2d6c580` | `51c9fdb` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand, `CLOSED_ITEMS.md` from `Omega`'s own text, whose function names differ) | The recording retry budget kept at 20 s (closed without a change) and the codec-changing stream switch deferred as documented. |
| `93bded6` | `7a5919c` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | Live segment reads name their viewer, so another viewer's Stop cannot end a buffer still being read (`timeshift_buffer` 0.8.17 plus the addon). The segment URL line and the file-server touch are the same on both; `Omega`'s glue suite passes against Kodi 21's headers, and the live check had already run on Kodi 21.3. |
| `6ebbd19` | `8a4acca` (`git cherry-pick -x`) | Generic wording for the spring-gap check's zone and three lab details, docs only |
| `c3f0366` | `740dc4d` (`git cherry-pick -x`) | The live server's worker count and SSH setup dropped from the live-check records, docs only |
| `07eaf96`, `fb36f8b` | not ported: `Piers` release line | `0.14.3` and its CoreELEC checksum; `Omega` has its own `0.13.3` (`43a8b7f`, `209fc1a`). |
| `d17266b` | `85e32a6` (`git cherry-pick -x`, without `[skip ci]`; `CHANGELOG.md`'s release heading by hand) | Every usable audio track buffered, teletext channels start, the audio map's spelling probed per ffmpeg (`timeshift_buffer` 0.8.18) |
| `02e684e` | `d6d74be` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The live checks the audio-track mapping still needs, as an open item |
| `c81eb77` | `1f584ec` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and intro by hand, and nine of the eleven entries) | Files the 2026-11-10 bug sweep's findings as open items; nine hold on `Omega`, and the two left out are `Piers` only (an in-progress recording's segment cache with two streams of it open, and this file's table of what differs) |
| `a7f94c7` | `589cd72` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand, the closed notes and `TEST_COVERAGE.md` citing `Omega`'s 0.13.3 set) | The release gate's and the CI path filter's five fixes from the 2026-11-10 sweep, tools and docs only |
| `09c2d46` | `960f18c` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and intro by hand) | The `timeshift_buffer` restart without the audio map for a start stuck on a video stream that is declared and never sent, with its live check on ffmpeg 8.1.2; the plugin and `TIMESHIFT.md` identical |
| `1db9aad` | `11597ed` (`git cherry-pick -x`, without `[skip ci]`, the segment-cache part left out; `OPEN_ITEMS.md`, `CLOSED_ITEMS.md`, `TEST_COVERAGE.md` and the CHANGELOG by hand) | The timer-id range (`IsEncodableTimerId()`) and the 64-bit comparison in `HasActiveOrImminentOccurrence()` from the 2026-11-10 sweep, with their unit tests. Not ported: the in-progress segment cache's join check (`PlanInProgressSegmentCacheUpdateFor()`) and the glue `ip_cache_diverge` scenario, since `Omega` keeps one in-progress stream open at a time |
| `43a8104` | `7f8ea81` (by hand, the timer-id note only) | The live checks of the 2026-11-10 sweep's addon fixes in `CLOSED_ITEMS.md`; the segment-cache note is `Piers` only |
| `9ab73ba` | `653735f` (`git cherry-pick -x`, without `[skip ci]`; the glue driver, `ci.sh`, README and `OPEN_ITEMS.md`/`CLOSED_ITEMS.md` by hand) | The glue fake's series rules keyed as Dispatcharr does, the sibling-delete step in `functional`, the `timer_id_range` scenario and the new pinned/unpinned delete item. Not ported: the `BRANCHES.md` table update (`Piers` only) |
| `5d1f7dd` | `e94600f` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand, and a line in the closed entry that the live check was of the `Piers` build) | The pinned/unpinned series delete guard (`SeriesRuleDeleteTakesSibling()`), its tests, the glue pair step and the notification wording |
| `74be21c` | `6a070ad` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand) | The title-less series rule notices no longer send the user to Dispatcharr's delete; the troubleshooting entry for both series-delete refusals |
| `5baf65a` | not ported: `Piers` only | A stray "None open." under the Kodi 22 open item, which `Omega` does not have |
| `aa83498` | `1298e4b` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The live check that closed the `timeshift_buffer` audio-track item, docs only |
| `d6916e4` | `5519d7a` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and source list by hand) | The deferred item for `timeshift_buffer` reaping a buffer that has no playlist yet, docs only |
| `f3fb349` | `c1daf84` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and source list by hand) | A "not ready" timeshift poll counts as activity, so the reaper no longer ends a buffer still waiting for its first playlist |
| `faa8326` | `7ef8e5d` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and source list by hand; the glue stubs report Kodi 21 and `live_stop_token` expects it; the changelog entry without its Kodi 22 sentence) | The `timeshift_buffer` restart after a stream switch, and the addon sending its Kodi version so only Kodi 22 viewers get the hold. On `Omega` it is what spares Kodi 21 the hold (0.13.4) |
| `f2a1602` | `43df915` (`git cherry-pick -x`, applied cleanly) | The live check that the Kodi-version map holds across workers, docs only |
| `73f5f1c` | `a20d1b1` (`git cherry-pick -x`; `MANUAL_TESTING.md` by hand, its Kodi 22 passages are not on `Omega`) | The test deployment's lineup size dropped from docs and comments, after the 0.13.4/0.14.4 pre-tag audit, docs only |
| `d7edc6a`, `d3e6cf2` | not ported: `Piers` release line | `0.14.4` and its CoreELEC checksum; `Omega` has its own `0.13.4` (`85d5bcf`, `c0b811d`). |
| `08267c4` | `d9b7985` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts and source list by hand, and the warm-up named as `Piers`'s) | The open item for Off-mode live TV ending by itself on an Android tablet, with the measured ghost-client timeouts, docs only |
| `e721e07` | `aa71c6f` (by hand; `OPEN_ITEMS.md`'s counts and source list, and the README item limited to the line `Omega` has) | The 2026-11-14 bug sweep's open items: sixteen of seventeen, the `BRANCHES.md` note being `Piers` only, docs only |
| `aec27d6` | `697011e` (by hand; the README line `Omega` has, its own counts, and the `BRANCHES.md` note left out) | The 2026-11-14 sweep's stale docs, plus `test_ci_build_needed.py`'s two Kodi-branch cases from `dd77977`, which make that file the same on both |
| `b37e268` | `0fcf987` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts by hand) | The 2026-11-14 sweep's four tooling findings: the release gate's entry names, local headers and data descriptors, non-ASCII terms, and the CI path filter |
| `d5b4531` | `f4caf1e` (`git cherry-pick -x`; `CHANGELOG.md`, `OPEN_ITEMS.md`'s counts and the glue driver by hand, the driver taking only the new scenario) | The 2026-11-14 sweep's MEDIUM: a short-bounded caller's wait for another thread's sign-in is bounded, and its give-up reports no response |
| `f990f2c` | `0aa2706` (`git cherry-pick -x`, without `[skip ci]`; `CHANGELOG.md` and `OPEN_ITEMS.md`'s counts by hand) | `timeshift_buffer` 0.8.19: a disable or delete stops the plugin in every Dispatcharr worker, through a stop token in Redis. The plugin, its tests and `TIMESHIFT.md` are the same files on both, so the live check made with `Piers` covers `Omega` too. |
| `ec3d226` | `0233d0c` (`git cherry-pick -x`; `OPEN_ITEMS.md`'s counts by hand) | The open item for DVB subtitles and teletext in Server-side mode; the plugin is the same file on both. |
| `5f7bbca` | `257dca5` (`git cherry-pick -x`, without `[skip ci]`; the comment above `EnsureAuthenticated()`, `OPEN_ITEMS.md`'s counts and the doc-ref baseline by hand, and a note in the `CLOSED_ITEMS.md` entry on what it means on Kodi 21) | The 2026-11-14 sweep's catch-up session reuse: the cache age drops from 30 s to 5 s, so a replay after Dispatcharr has ended the first session gets a new one instead of a 401. |
| `f2d31f2` | not ported: seen on Kodi 22 only | The open item for Kodi 22 reading two finished recordings end to end at start; port it if Kodi 21 does the same. |
| `b338e0d` | `be1c14e` (by hand: the same paragraph, placed after `Omega`'s port note in the `CLOSED_ITEMS.md` entry) | The Kodi 21 live check of the catch-up session cache age. |
| `5719693` | `c7556fc` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and sweep line by hand) | The 2026-11-14 sweep's two `timeshift_buffer` findings (0.8.19): a last viewer's stop follows a stream-switch restart, and a viewer re-added after a prune keeps its Kodi version. |
| `733d9a2` | `ee882fc` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts and sweep line by hand) | The 2026-11-14 sweep's three addon findings: the connection notice names the server in use, a series-rule timer two rules share is refused, and expired pending titles are pruned on every add. |
| `a052589` | `66d1be4` (`git cherry-pick -x`) | The Windows CI job retries its build-dependency downloads: `mirrors.kodi.tv` redirects to a random mirror, and one now chains to a root Windows does not trust out of the box. |
| `252d6a7` | `578c1f9` (`git cherry-pick -x`; the include list and `OPEN_ITEMS.md`'s counts resolved by hand) | A settings save delivered while the PVR instance is still being constructed is re-delivered once Kodi has queried the new instance; live-checked on Kodi 21.3 before the merge. |
| `6e3e0d3` | `908c593` (`git cherry-pick -x`, without `[skip ci]`; `OPEN_ITEMS.md`'s counts resolved by hand) | A plugin's own error result in Dispatcharr 0.34's HTTP 500 is read as the plugin's answer again (retryable start, not-ready cold start, buffer gone); live-checked on `Piers` against a real 0.34.0 server, the glue suite passed on Kodi 21 headers. |

## Dependencies that differ by Kodi release

- **Kodi pin:** `Piers` builds against the `22.0rc1-Piers` tag (`28ea2eac1eb7`). Its PVR headers are identical to the Piers
  head's, but the head declares `kodi.binary.global.main` 2.0.4 against RC1's 2.0.3 and Kodi 22 RC1 could refuse a build
  made with it. Raise the pin deliberately at Kodi 22.0 final.
- **`inputstream.ffmpegdirect`** (optional import, `3.0.0`): Kodi 22's build of it is `22.2.10` (its `Piers` branch), which
  satisfies the minimum, so the import needs no change.
- **Android:** Kodi 22's own `docs/README.Android.md` (at the RC1 commit) asks for NDK 28.2.13676358 with
  `platforms;android-37.0` and `build-tools;37.0.0`, against Kodi 21's NDK 21.4.7075529 / `android-34` / `33.0.1`.
- **CoreELEC:** the `coreelec-22` branch exists, with the `22.0-Piers_rc1` tag (it pins the same Kodi, `22.0rc1-Piers`);
  `coreelec-21` is Kodi 21. Its device is `Amlogic-no` and its userland is a real 64-bit one (`ARCH=aarch64`), where
  `coreelec-21`'s was 32-bit `arm` on `Amlogic-ng`. The CoreELEC zip of a `Piers` release is therefore
  `...-coreelec-aarch64.zip`, and `tools/check_release_zip.py` expects that name on this branch (`coreelec-armv7` on
  `Omega`).

The macOS, both Android and the CoreELEC zips were built by hand from `Piers` against Kodi 22 RC1 on 2026-10-15/16 and
pass the content checks of `tools/check_release_zip.py`; each was then run on a device against Kodi 22 RC1 on
2026-10-16 (`docs/MANUAL_TESTING.md`). `docs/BUILDING.md` has the differences each build needed.
