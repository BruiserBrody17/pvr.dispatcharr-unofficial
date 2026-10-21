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
  `RELEASE_TAG_SUFFIX`. On `Omega` the suffix is empty until its next release is tagged (`0.13.0` has no suffix, and its
  just-set checksum names that tarball); the version-bump commit of that release adds `-Omega` to `PKG_VERSION` and to the
  test's constant. Confirmed at the first Piers CoreELEC build (2026-10-16): the suffix does not reach the zip. CoreELEC
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

Everything else (the plugins, `tools/`, the other `src/` files, most docs) is the same file on both and cherry-picks
cleanly. The plugins are shared code: a plugin fix is made once and lands on both.

### Ported from Omega

Last `Omega` commit accounted for: **`cce3e4c`** (everything up to it is on `Piers`). When porting, list every `Omega`
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
| `aded040`, `c28882c`, `202f7d3`, `1e259a1`, `baec5ef`, `797bd83`, `f55a57c`, `ba08b01`, `0d113ae`, `cce3e4c` | already on `Piers` | Ports from `Piers`; see the table below. |

### Ported to Omega

Last `Piers` commit accounted for: **`df0e9b1`**. As above, in the other direction: list every `Piers` commit after it
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
| `b1ab0ed` | not yet | The Kodi repository; ported after its first publish, step (5) of `OPEN_ITEMS.md`'s "The Kodi repository is built but not published". |
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
