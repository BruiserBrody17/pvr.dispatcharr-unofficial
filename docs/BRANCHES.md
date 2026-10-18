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

Last `Omega` commit accounted for: **`80a146f`** (everything up to it is on `Piers`). When porting, list every `Omega`
commit after the last one here (`git log --oneline <last>..Omega`), port or decide not to, record it below and move
"last accounted for" forward. A fix that is not ported on purpose says why.

| Omega commit | Ported as | Notes |
|---|---|---|
| `6cc7ee1`, `2ec3551`, `b0f542a` | already on `Piers` as `b26412c`, `48764de`, `ffe8c16` | The docs-audit pass, made on `Piers` first and ported to `Omega` without its Kodi 22-only parts. |
| `80a146f` | `git cherry-pick -x` (applied cleanly) | Made-up titles in the recording-directory tests, comments and docs. |

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
