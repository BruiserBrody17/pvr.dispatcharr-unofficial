# Building pvr.dispatcharr-unofficial

Kodi binary addons aren't built standalone in the usual CMake sense --
they're built through Kodi's own addon build harness so the resulting
`.so`/`.dll`/`.dylib` links against exactly the right Kodi ABI. This repo's
`CMakeLists.txt` supplies the addon side of that (`find_package(Kodi
REQUIRED)` + `build_addon(...)`); the harness lives in Kodi's own source
tree at `tools/depends/target/binary-addons`.

This needs real network access (to clone Kodi, fetch nlohmann/json and
pugixml via CMake's `FetchContent`, and pull system libcurl) that this chat
sandbox doesn't have -- do the actual build with **Claude Code** or your own
machine/CI. The GitHub Actions workflow in `.github/workflows/build.yml`
automates the Windows/Linux steps below on every push -- macOS is
deliberately not part of that automation; see its own note in the
"Linux / macOS (x86_64)" section below for why, and how to build/release
it by hand instead.

## Linux / macOS (x86_64)

1. Clone a Kodi source tree matching your target Kodi major version (e.g.
   the `Omega` branch for Kodi 21.x, since that's roughly what CoreELEC on
   an ODROID N2+ ships):
   ```bash
   git clone --branch Omega --depth 1 https://github.com/xbmc/xbmc.git kodi-source
   ```
   (CI does not track the branch: `KODI_COMMIT` in `.github/workflows/build.yml` names the exact Kodi
   commit every job builds, so a release build never picks up whatever Kodi pushed that day. To
   reproduce a CI build exactly, check that commit out instead.)
2. Clone this addon next to it (any path):
   ```bash
   git clone https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial.git addons/pvr.dispatcharr-unofficial
   ```
3. Register the addon with the harness. `pvr.dispatcharr-unofficial` isn't in Kodi's
   official addon manifests, so `ADDON_SRC_PREFIX` alone won't find it --
   the harness needs an explicit definition file pointing at the local
   checkout via a `file://` URL:
   ```bash
   mkdir -p addon-defs/pvr.dispatcharr-unofficial
   echo "pvr.dispatcharr-unofficial file://$(pwd)/addons/pvr.dispatcharr-unofficial" \
     > addon-defs/pvr.dispatcharr-unofficial/pvr.dispatcharr-unofficial.txt
   ```
4. Run the addon build harness. `PREFIX` is required even for a native
   (non-cross-compiling) build -- point it at any writable install
   destination:
   ```bash
   cd kodi-source
   make -j$(nproc) -C tools/depends/target/binary-addons \
     ADDONS="pvr.dispatcharr-unofficial" \
     ADDONS_DEFINITION_DIR="$(pwd)/../addon-defs" \
     PREFIX="$(pwd)/../install" \
     EXTRA_CMAKE_ARGS="-DPACKAGE_ZIP=ON" \
     PACKAGE=1
   ```
   On macOS, this same `make` invocation works from a shell with Xcode
   command line tools installed; see the next section for Windows, which
   doesn't have `make` and needs a couple of extra dependency steps. If a
   prior run failed partway through, delete
   `tools/depends/target/binary-addons/.installed-native` first -- the
   harness touches that marker even after a failed configure, which
   otherwise makes it skip reconfiguring on the next run.

   **The same marker also needs clearing on an ordinary rebuild against
   updated source, not just after a failure** (confirmed live, 2026-09-09,
   rebuilding a persistent Linux build tree from an earlier session that
   was still at `0.9.0`): with `ADDONS_DEFINITION_DIR` pointing at a
   `file://` path (a live local checkout, not a version-pinned tarball
   URL the way CoreELEC's `package.mk` works), the harness has no signal
   that the addon's own source content changed at all -- same URL every
   time -- so it silently no-ops (`make: Nothing to be done for 'all'`)
   and leaves the old binary in place. Confirmed the fix needs *two*
   things removed, not just one: `.installed-native` itself, and the
   addon's own stale ExternalProject state,
   `tools/depends/target/binary-addons/native/pvr.dispatcharr-unofficial-prefix/`
   (its stamp files are what the *inner* CMake target checks, one layer
   below the outer marker) -- deleting only one of the two still no-op'd.
   A version bump alone doesn't fix this either, since the file:// URL
   itself never changes across versions the way a tagged tarball's would.
5. `-DPACKAGE_DIR` looks like it should redirect where CPack writes the
   zip, but doesn't actually take effect for this harness (confirmed:
   verified against a real run, CPack still wrote it deep inside the
   addon's own ExternalProject build tree regardless of that flag) --
   `PACKAGE_ZIP=ON` alone is what makes CPack build it at all, so find it
   afterward instead of trusting `PACKAGE_DIR`:
   ```bash
   find tools/depends/target/binary-addons -name 'addon-pvr.dispatcharr-unofficial-*.zip'
   ```
   That zip (named e.g. `addon-pvr.dispatcharr-unofficial-0.1.0-osx-arm64.zip`) is
   what you install via Kodi's
   "install from zip file" option, or publish in a self-hosted repository
   (see the "Distribution" section below).

**macOS is deliberately NOT part of CI, as of 2026-09-28 -- the same
"GitHub Actions job ... rejected" reasoning as the CoreELEC section below,
just a different underlying constraint.** A runner only picks up a queued job once it's actually online, and macOS hardware is not available as always-on infrastructure (a Windows runner was confirmed practical the same day, see
`.github/workflows/build.yml`'s own `build-windows` job and its comments).
Registering a persistent runner on an intermittently-online
device means either jobs queue indefinitely whenever it's offline, or the
machine stays running as a CI daemon full-time, which is not worth it. Build macOS manually per the steps above instead, whenever the hardware is available, and if
it's for a real tagged release, every release carries a macOS zip: build it from the tag's source,
then normalize and gate it with every other asset as "Release assets" below describes (the exact zip
filename depends on the Mac's own architecture -- `find tools/depends/target/binary-addons -name
'addon-pvr.dispatcharr-unofficial-*.zip'` from within the `kodi-source` checkout, per step 5 above, to
confirm the real name; a release expects `-osx-arm64`).

**Confirmed live on a real Linux install (0.4.0):** the steps
above work as documented -- gcc 14/cmake 3.31 from the distro's own repos
built it cleanly, no compatibility issues, once `libcurl-devel` was
installed (the one dependency not present by default). Kodi itself
wasn't packaged for that distribution release yet at the time (too new for the distribution's third-party repository); the official Kodi Flatpak (`flatpak install flathub
tv.kodi.Kodi`) worked as a real, officially-supported alternative --
confirmed the natively-built `.so` above loads and runs correctly inside
that Flatpak's sandboxed runtime with no ABI issues.

**A real, non-obvious gotcha if launching Kodi non-interactively (e.g.
over SSH) rather than from the desktop's own app launcher: Kodi's own
process crashed on startup** (`std::vector<...>::front() [CpuData]:
Assertion '!this->empty()' failed`, inside Kodi's own CPU-monitoring
code, before this addon was even instantiated) **when launched without
the logged-in session's actual display environment.** `ssh user@host
'flatpak run tv.kodi.Kodi'` doesn't inherit `WAYLAND_DISPLAY`/`DISPLAY`/
`DBUS_SESSION_BUS_ADDRESS` from the real graphical session at all --
confirmed fixed by pulling those from an already-running process that
*is* part of that session (e.g. `tr '\0' '\n' < /proc/<gnome-shell
pid>/environ`) and exporting them (along with `XDG_RUNTIME_DIR`) before
the `flatpak run` call. Once launched with the correct display
environment, Kodi ran completely stably -- this was an environmental
launch-context issue, not a bug in Kodi or this addon.

## Windows (x86_64)

Windows has no `make` and no system libcurl, so this doesn't go through
`tools/depends/target/binary-addons` -- instead it invokes the same
underlying `cmake/addons` project that Makefile wraps, directly, with the
Visual Studio generator. `.github/workflows/build.yml` automates all of
this; the steps below are what that workflow actually runs; every step here
was verified against a real build, not just written from Kodi's docs.

The `-G "Visual Studio ..."` generator string below must match whichever
Visual Studio version is actually installed -- CMake doesn't fall back to
whatever's present, it fails outright ("could not find any instance of
Visual Studio") if you name a version that isn't there. This has already
bitten CI once: GitHub's `windows-latest` runner moved to shipping only VS
2026 (`"Visual Studio 18 2026"`) with no VS2022 left on the image at all,
confirmed by running `vswhere -all -format json` in the workflow. A local
machine may well still have VS2022 (`"Visual Studio 17 2022"`) instead --
check what you actually have (Visual Studio Installer, or `vswhere -all`)
rather than assuming either one.

**Enable git's long-path support before cloning anything, or a deeply
nested dependency clone can fail outright.** `git config --global
core.longpaths true` (or `--system`, for CI). Confirmed live: CI's
`build-windows` job failed with a real `Filename too long` error cloning
`nlohmann-json` (a `FetchContent` dependency) -- some of that repo's own
historical test-report filenames are long enough that the *addon's own
build-directory path* nested above them (which embeds this addon's id)
pushed the combined path past Windows' 260-char `MAX_PATH`, and it got
12 characters worse twice over from the `pvr.dispatcharr` ->
`pvr.dispatcharr-unofficial` rename: once in the addon id itself
(appearing multiple times in the nested `<id>-prefix`/`<id>-build`
directory names CMake's `ExternalProject` generates), and again in the
GitHub repo name forming the root of CI's own checkout path
(`D:\a\<repo>\<repo>\...`). The exact same commit's CI run had passed
minutes earlier, before the repo got renamed -- the shorter
pre-rename checkout path was still just under the limit. A local build
can hit this too depending on how deep the workspace path already is;
this project's own `kodi-win-work` example workspace was only 7
characters under the limit for this exact file even before accounting
for the rename, so treat this as something to fix once globally on any
Windows build machine, not just a CI-specific patch.

1. Clone Kodi and this addon exactly as in steps 1-2 above (Omega branch,
   this repo checked out under `addons/pvr.dispatcharr-unofficial`), and register the
   addon exactly as in step 3.
2. Fetch curl and its own dependencies. Windows has no system libcurl, and
   the prebuilt curl Kodi's own dependency mirror serves needs OpenSSL and
   zlib to link against (confirmed by actually running this build -- curl's
   own `CURLConfig.cmake` pulls both in transitively). Point
   `ADDON_DEPENDS_PATH` at the *same* depends directory the main addon build
   will look in by default (`<BUILD_DIR>/depends`), so one `find_package`
   picks up both Kodi's own generated config and these. Use `https://`, not
   `http://` (fixed 2026-09-27, a 57th-pass audit, fixing a real, confirmed
   supply-chain-hardening gap found via a project-wide review, confirmed
   live against `mirrors.kodi.tv`): the plain-HTTP URL these were
   originally copied from (matching upstream Kodi's own docs at the time)
   redirects to an HTTPS mirror anyway, so the only effect of using
   `http://` is an unauthenticated first hop a network-position attacker
   could redirect elsewhere -- confirmed live that `https://` against the
   same host and path works identically (a 302 to an HTTPS mirror), so
   this costs nothing to fix. Also pass `-DCMAKE_TLS_VERIFY=ON` (added
   the very next, 58th, pass -- explicit from the 59th pass onward, not a
   fix, corrected 2026-09-27, a 59th-pass audit, fixing a real, confirmed
   factual error in the 58th-pass version of this same wording, found via
   a project-wide review, confirmed by directly reproducing the claim
   against both CMake 3.28 and a real, pip-installed CMake 4.4.3, plus
   CMake's own bundled documentation): the 58th-pass wording claimed
   CMake's own `ExternalProject_Add(URL ...)` -- what Kodi's own
   `add_internal()` (`cmake/addons/depends/windows/CMakeLists.txt`) uses
   for these three downloads -- "does NOT verify the peer's TLS
   certificate by default", reproduced only against a local CMake 3.28
   install. CMake's own `CMAKE_TLS_VERIFY.rst` documents that this
   default flipped to *on* in CMake 3.31 (confirmed by reproducing both
   sides: an unset default silently accepted a self-signed cert on 3.28,
   but correctly rejected the same cert with nothing set at all on
   4.4.3) -- and the `Visual Studio 18 2026` generator this same command
   already uses was only added in CMake 4.2 (confirmed against CMake's
   own `4.2.rst` release notes), so any environment able to run this
   exact command at all already has TLS verification on by default, with
   nothing further needed. This flag is kept anyway as an explicit pin,
   not a fix: it protects against the one real escape hatch that still
   exists on a verifying-by-default CMake -- the `CMAKE_TLS_VERIFY=0`
   environment variable, which a corporate network doing its own TLS
   interception might otherwise set to work around certificate errors
   (confirmed reproducible: an explicit `-D` at this same configure step
   overrides that environment variable either way, so if a corporate CA
   genuinely needs trusting here, `-DCMAKE_TLS_CAINFO=<path to that CA
   bundle>` is the correct escape hatch, not disabling verification) --
   rather than closing a real, currently-existing gap in this specific
   command:
   ```powershell
   $depends = "$pwd\build\build\depends"
   $prebuilt = "kodi-source\cmake\addons\depends\windows\prebuilt"
   $downloads = "$pwd\prebuilt-downloads"
   New-Item -ItemType Directory -Force -Path $downloads | Out-Null
   $archives = @(
     @{ id = "curl";    file = "curl-7.67.0-x64-v141-20200105.7z";    sha256 = "58106b42cbb3d6d951671e71e00703dd644b5b729cfd418b2ba08e4a02780145" },
     @{ id = "openssl"; file = "openssl-1.1.1q-x64-v142-20221017.7z"; sha256 = "d744eb075f7628156979cd8cb9545fb7c6cc567efe901a1de1ab9b103b85d4e7" },
     @{ id = "zlib";    file = "zlib-1.2.11-x64-v141-20200105.7z";    sha256 = "c596b93016d62f6bc85b38b2f18746c8382cf89b1a99c1c56f0d2bf51ecf54f1" }
   )
   foreach ($a in $archives) {
     $local = "$downloads\$($a.file)"
     Invoke-WebRequest -Uri "https://mirrors.kodi.tv/build-deps/win32/$($a.file)" -OutFile $local
     $actual = (Get-FileHash -Algorithm SHA256 -Path $local).Hash.ToLowerInvariant()
     if ($actual -ne $a.sha256) { throw "SHA256 mismatch for $($a.file): expected $($a.sha256), got $actual" }
     $registered = $local.Replace('\', '/')   # forward slashes: a backslash is an invalid escape in Kodi's generated CMake script
     "$($a.id) $registered" | Out-File -Encoding ascii "$prebuilt\$($a.id).txt"
   }
   cmake -S kodi-source\cmake\addons\depends\windows -B deps-build -G "Visual Studio 18 2026" -A x64 "-DADDON_DEPENDS_PATH=$depends" -DCMAKE_TLS_VERIFY=ON
   cmake --build deps-build --config Release
   ```
   Each archive is downloaded and checked against a pinned SHA256 *before*
   Kodi's own `add_internal()` sees it, and the registration `.txt` then
   names the verified local file rather than the mirror URL (added
   2026-10-02, closing the "no integrity check" gap noted above: TLS
   verification does not cover a compromise of the legitimate mirror itself).
   `add_internal()` has no hash argument to hook into and patching Kodi's own
   source is out of scope, but it hands `URL` to `ExternalProject_Add`, which
   accepts a local path -- so the file that was verified is the file that is
   installed, with no second download for a compromised mirror to answer
   differently. The local path must contain no spaces (the registration file is
   split on them) and must use forward slashes (it is pasted into a generated
   CMake script as a quoted string, where `\g` and the like are invalid escapes --
   the first CI run of this step failed exactly that way). To move to a newer archive, change its name and hash
   together; `.github/workflows/build.yml` carries the same step.
   Then delete the three `.txt` files you just created. The main build's own
   generic dependency scan (`add_addon_depends`) also globs this same
   `prebuilt/` directory, and having both declare a same-named CMake target
   is a hard configure error -- the files have done their job once the real
   `.lib`/`.dll` files are sitting in `$depends`.
3. Configure and build the addon itself. Quote every `-D` argument as one
   token (`"-DFOO=bar"`, not `-DFOO=bar` split across a line continuation) --
   PowerShell's backtick line continuation can otherwise truncate a value at
   the first `.` it contains, which silently turns `pvr.dispatcharr-unofficial` into
   just `pvr`:
   ```powershell
   cmake -S kodi-source\cmake\addons -B build -G "Visual Studio 18 2026" -A x64 `
     "-DADDONS_TO_BUILD=pvr.dispatcharr-unofficial" `
     "-DADDONS_DEFINITION_DIR=$pwd\addon-defs" `
     "-DCMAKE_INSTALL_PREFIX=$pwd\install" `
     "-DPACKAGE_ZIP=ON" `
     "-DPACKAGE_DIR=$pwd\dist" `
     "-DBUILD_DIR=$pwd\build\build"
   cmake --build build --config Release --target package-pvr.dispatcharr-unofficial
   ```
   Don't pass a custom `-DCMAKE_PREFIX_PATH` here even if you're tempted to
   point it at a separate curl install location: `cmake/addons/CMakeLists.txt`
   builds that variable's final value via an *unquoted* `${CMAKE_PREFIX_PATH}`
   expansion, which silently drops every entry but the first when it's a
   multi-item list. Installing curl straight into the default depends path
   in step 2, and leaving `CMAKE_PREFIX_PATH` alone here, sidesteps that bug
   entirely.
4. `-DPACKAGE_DIR` doesn't actually redirect CPack's output here either (same
   as the Linux/macOS harness above) -- confirmed via a real run's CPack log,
   it instead drops the zip in the OS temp dir (`$env:TEMP`, e.g.
   `addon-pvr.dispatcharr-unofficial-0.1.0-windows-x86_64.zip` under
   `C:\Users\<runner>\AppData\Local\Temp`). Find it there rather than in
   `dist`:
   ```powershell
   Get-ChildItem -Path $env:TEMP -Filter 'addon-pvr.dispatcharr-unofficial-*.zip' -Recurse
   ```
   On a machine that keeps its temp directory between builds (a CI runner), delete the zips an
   earlier build left before building: the name carries the version and a name sort puts `0.11.0` before
   `0.12.0`, so "the first match" can be an old release's zip. CI clears them first (including CPack's
   `_CPack_Packages` staging copy) and fails when more than one is found directly in the temp directory.
   That zip bundles `libcurl.dll` and `zlib.dll` alongside
   `pvr.dispatcharr-unofficial.dll` (see `CMakeLists.txt`'s `DISPATCHARR_ADDITIONAL_BINARY`),
   since a standalone Windows install can't assume those are already present
   the way Kodi's own bundled curl, or Linux's system libcurl, would be.
5. CPack writes each zip entry's time both as the build machine's local clock
   and as UTC (an extended field), so the pair gives away the build machine's
   timezone to anyone who downloads the zip (confirmed 2026-10-03 on a CI runner's output; GitHub-hosted runners run on UTC, which hid
   it). `build.yml`'s "Normalize zip timestamps" step rewrites the zip with
   every entry stamped with the commit time in UTC and no extended fields,
   same bytes otherwise. A hand-built zip for a release needs the same
   treatment before upload; the step's PowerShell runs as-is on any machine
   with `pwsh` and `git`. The Linux leg and every hand-built zip (macOS,
   CoreELEC, Android) take `tools/normalize_zip.py` instead, which needs only
   Python 3: `python3 tools/normalize_zip.py "$(git log -1 --format=%ct)" <zip>`,
   then `python3 tools/normalize_zip.py --check <zip>` (CI's Linux leg and the
   plugin zips run both; the Linux leg's zip shipped unnormalized until the
   pre-release sweep of 0.12.0 found it).

## CoreELEC on an ODROID N2+ (Amlogic S922X)

This is the part that most often causes "it built but Kodi won't load it"
problems, because CoreELEC ships its own cross-compiled Kodi build with a
specific compiler/libc/Kodi-commit combination -- a binary built against a
generic Linux aarch64 toolchain will very likely **not** be ABI-compatible,
even though it's technically the same architecture.

A GitHub Actions job for this was considered and rejected: CoreELEC's own
build harness (a LibreELEC fork) is designed around persistent, self-hosted
build infrastructure -- confirmed by reading LibreELEC's own addon-build
automation (`LibreELEC/actions` on GitHub), which runs on
`[self-hosted, nightly]` runners with a 12-hour timeout and host-mounted
`/sources`, `/target`, `/build-root` directories that persist across runs,
specifically so the cross toolchain and every dependency aren't rebuilt
from scratch every time. GitHub's hosted runners are ephemeral, capped at
6 hours, and give ~14GB free disk -- a real mismatch for a from-scratch
build, not just a "might be slow" one. Building locally is the practical
path.

### Building it as a CoreELEC package (recommended)

CoreELEC is a LibreELEC-derived buildroot distro; third-party binary Kodi
addons are added as a package under
`packages/mediacenter/kodi-binary-addons/<addon-name>/package.mk` in a
CoreELEC source checkout, following the same pattern as addons they
already ship. Unlike Kodi's own `tools/depends/target/binary-addons`
harness (used above), this doesn't support pointing at a live git branch --
every in-tree example (confirmed by reading CoreELEC/CoreELEC's real
`pvr.hts` and `pvr.iptvsimple` package.mk files) points `PKG_URL` at a
tagged release tarball with a pinned `PKG_SHA256`. A package definition for
this addon following that same pattern is checked in at
[`packaging/coreelec/pvr.dispatcharr-unofficial/package.mk`](../packaging/coreelec/pvr.dispatcharr-unofficial/package.mk).

**This whole path -- package.mk, checksum, branch, device, arch -- has now
been confirmed by an actual successful cross-compile** (`0.3.0`, against a real Ubuntu build machine): `ALL ADDONS BUILT SUCCESSFULLY`, producing
a genuine `pvr.dispatcharr-unofficial-0.3.0.1.zip` whose `.so` is a real stripped
ELF binary, and every specific claim below (branch, device, arch, output
path) reflects what that run actually did, not documentation guesswork.
Several real problems surfaced and were fixed along the way -- they're
called out inline so a future rebuild doesn't have to rediscover them.

1. Tag and publish a GitHub release matching the version in
   `pvr.dispatcharr-unofficial/addon.xml.in` (e.g. `0.4.0`, no `v` prefix -- CoreELEC's
   own package.mk files use the tag name verbatim in the archive URL, and
   keeping it identical to `PKG_VERSION` avoids a mismatch):
   ```bash
   git tag 0.4.0
   git push origin 0.4.0
   gh release create 0.4.0 --generate-notes
   ```
   **The GitHub repo must be public.** CoreELEC's build harness fetches
   `PKG_URL` with a plain anonymous `curl`/`wget` -- no auth, no token --
   so a private repo's archive URL 404s (GitHub's standard "hide
   existence" response to an unauthenticated request against a private
   repo). Confirmed live: this addon's own repo was still private from an
   earlier session, the cross-compile's very last step
   (`pvr.dispatcharr-unofficial:target`, after the entire toolchain had already
   built successfully) failed on exactly this 404, and switching the repo
   to public (`gh repo edit <repo> --visibility public
   --accept-visibility-change-consequences`) immediately fixed it -- no
   package.mk or build-harness change needed.
2. Compute the tarball's checksum and fill it into
   `packaging/coreelec/pvr.dispatcharr-unofficial/package.mk`'s `PKG_SHA256`:
   ```bash
   curl -L https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/archive/0.3.0.tar.gz | sha256sum
   ```
   Do this *after* the repo is public, against the real tarball -- doing
   it while the repo is still private silently hashes GitHub's 404 error
   page instead (confirmed live: this is exactly what happened building
   `0.3.0`, and the build failed with an explicit
   `Incorrect checksum calculated on downloaded file` once the repo was
   made public and the real tarball could finally be fetched, until the
   checksum was recomputed against the real content).
3. On a Debian/Ubuntu-based Linux build machine (not the N2+ itself --
   cross-compiling needs real CPU and disk; per CoreELEC's own build docs,
   budget 50GB free disk and expect the first build to take a while, since
   it also has to fetch/build the whole cross toolchain including GCC from
   source), clone CoreELEC at the branch matching your device's installed
   CoreELEC major version -- **use the branch matching CoreELEC's current
   *stable* release, not whatever branch happens to be default/newest**:
   `coreelec-22` tracks Kodi 22 "Piers", which as of this writing is still
   a beta (`22.0b2`) with a materially different PVR addon API (the
   recorded-stream methods gained a `streamId` parameter for
   concurrent-stream support) -- confirmed live by actually
   cross-compiling against it, which failed with `override` mismatches on
   `GetChannelStreamProperties`/`OpenRecordedStream`/
   `CloseRecordedStream`/`ReadRecordedStream`/`SeekRecordedStream`/
   `LengthRecordedStream`, all six exactly matching the API's real,
   deliberate signature change, not a bug in this addon's code. CoreELEC's
   actual latest stable release is `21.3-Omega` (confirmed against
   CoreELEC's own GitHub releases), tracking Kodi 21/Omega -- the same
   branch this addon's Windows/Linux CI already targets
   (`KODI_BRANCH` in `.github/workflows/build.yml`) and what a real device
   almost certainly runs unless deliberately flashed onto a dev/nightly
   build. This is the branch that actually built successfully:
   ```bash
   git clone --branch coreelec-21 --depth 1 https://github.com/CoreELEC/CoreELEC.git
   mkdir -p CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharr-unofficial
   cp packaging/coreelec/pvr.dispatcharr-unofficial/package.mk \
     CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharr-unofficial/
   ```
   **If this file was edited on Windows and copied into a Linux
   checkout, strip CRLF line endings before building** (confirmed live,
   2026-09-07): git's `core.autocrlf` normalizes this file to CRLF in a
   Windows working tree, and a plain `cp` into Linux carries that over
   byte-for-byte. The build harness's `PKG_IS_ADDON` comparison
   (`create_addon`'s `verify_addon`/`get_addons`) then compares against
   the literal string `"yes\r"`, not `"yes"` -- it fails silently (no
   error, `&>/dev/null`-suppressed), and the addon is simply missing from
   every listing (`create_addon pvr.dispatcharr-unofficial` and even `create_addon
   --show-only binary` both just omit it, with no indication why).
   Confirmed via `bash -x` tracing into `verify_addon`, which showed
   exactly this comparison failing. Fix:
   ```bash
   sed -i 's/\r$//' CoreELEC/packages/mediacenter/kodi-binary-addons/pvr.dispatcharr-unofficial/package.mk
   ```
4. Build it. Both the device name *and* the arch value have changed across
   CoreELEC branches -- don't assume either is stable across branches, and
   don't assume the SoC being 64-bit means the build's `ARCH` is
   `aarch64`:
   - **Device**: confirm by reading the target branch's own source tree
     for which device directory under `projects/Amlogic-ce/devices/` ships
     `Odroid_N2_boot.ini`. On `coreelec-21` it's `Amlogic-ng` (confirmed
     live -- `coreelec-22`, which this doc no longer recommends, instead
     used a single combined `Amlogic-no` device that doesn't exist on
     `coreelec-21`).
   - **Arch**: CoreELEC's own official `21.3-Omega` release for this
     device is literally named
     `CoreELEC-Amlogic-ng.arm-21.3-Omega-Odroid_N2.img.gz` (confirmed
     against CoreELEC's own GitHub releases) -- `arm`, not `aarch64`,
     despite the S922X being a 64-bit SoC: CoreELEC's `Amlogic-ng` device
     on the Omega branch runs a 32-bit (`armhf`/EABI5) userland on a
     64-bit kernel, not a true AArch64 userland. Passing `ARCH=aarch64`
     doesn't fail outright -- the build harness silently resolves the
     actual target to `arm` regardless (confirmed by the resulting build
     directory being named `...arm-21`, not `...aarch64-21`, and by the
     finished `.so` being `ELF 32-bit LSB shared object, ARM, EABI5`, not
     64-bit) -- but passing `ARCH=arm` explicitly matches reality and
     avoids the confusion:
   ```bash
   cd CoreELEC
   PROJECT=Amlogic-ce ARCH=arm DEVICE=Amlogic-ng ./scripts/create_addon pvr.dispatcharr-unofficial
   ```
   Two more real, confirmed-live failure modes to expect and how they were
   fixed, in case a future build hits them again (both are about *fetching
   dependency source tarballs*, not about this addon's own code):
   - **GNU mirror unreachability**: `ftpmirror.gnu.org` (and CoreELEC's
     own mirrors, `sources.coreelec.org` / `sources.libreelec.tv`) were
     unreachable from the build network, failing `make`,
     `m4`, `autoconf`, `autoconf-archive`, `automake`, `bison`,
     `libtool`, `mpc`, `mpfr`, `readline`, `gcc`, and `gmp` in turn (each
     one only discovered by retrying after the previous one was fixed).
     `ftp.gnu.org` (the canonical, non-redirector GNU host) worked
     throughout. If this happens again: fetch the exact `PKG_VERSION`
     tarball from `https://ftp.gnu.org/gnu/<pkg>/...` (or, for `gmp`,
     `https://ftp.gnu.org/gnu/gmp/...`, since `gmp` doesn't use the
     `ftpmirror.gnu.org` host in its own `package.mk` but is served by
     the same broken `sources.coreelec.org`/`sources.libreelec.tv`
     mirrors as a fallback), verify it against that package's own
     `PKG_SHA256` in `packages/.../<pkg>/package.mk`, then drop it into
     `sources/<pkg>/<pkg>-<version>.<ext>` in the CoreELEC checkout
     alongside two matching sidecar files, `<same filename>.sha256`
     (just the hex digest) and `<same filename>.url` (the original
     `PKG_URL`) -- the build script skips fetching anything it finds
     already staged there with a matching checksum.
   - **`repo.or.cz` bot wall**: `rtmpdump`'s pinned git-snapshot URL
     (`repo.or.cz/rtmpdump.git/snapshot/<commit>.tar.gz`) returned an
     Anubis bot-check HTML page instead of the real tarball, from a plain
     `curl`/`wget` with no way to pass the JS challenge nor via a
     browser-like `User-Agent` header. The Wayback Machine had a genuine
     cached copy from before the bot wall existed (fetched via
     `https://web.archive.org/web/2023/<original URL>` -- note this
     worked even though `https://archive.org/wayback/available?url=...`
     reported no snapshot for the same URL) that matched `rtmpdump`'s own
     pinned `PKG_SHA256` exactly; staged the same way as the GNU packages
     above.
   One more confirmed-live finding on this path, not a failure -- the
   build succeeds, but the resulting `addon.xml`'s `<platform>` tag comes
   out empty (`<platform></platform>`), unlike the Linux/macOS/Windows
   steps above which correctly produce `<platform>linux</platform>` etc.
   Traced to Kodi's own upstream CMake build, not anything in this
   addon's own `addon.xml.in`/`CMakeLists.txt`/`package.mk`:
   `cmake/KodiConfig.cmake.in`'s `PLATFORM_TAG` substitution is only ever
   computed by `cmake/scripts/common/PrepareEnv.cmake`, which only
   `cmake/addons/CMakeLists.txt` (Kodi's own `tools/depends/target/
   binary-addons` addon-build harness used by the Linux/macOS/Windows
   steps above) `include()`s -- Kodi's own top-level `CMakeLists.txt`
   (which builds `kodi.bin` itself and installs `KodiConfig.cmake` for
   external SDK use, the file CoreELEC's `kodi-binary-addons` packaging
   class builds addons against directly) never does, so `PLATFORM_TAG` is
   simply undefined at that point and substitutes empty. Confirmed this
   isn't cross-compile-specific: even CoreELEC's own host-native x86_64
   build of Kodi (bootstrapping its own build tools) produces the same
   blank `PLATFORM_TAG` in its installed `KodiConfig.cmake`, and
   `PrepareEnv.cmake`/`AddonHelpers.cmake` themselves are byte-identical
   to a fresh upstream checkout -- this is a structural property of how
   Kodi's own build generates its external addon-SDK config on this path,
   affecting any addon CoreELEC packages this way, not something specific
   to this repo's own config. Likely harmless in practice -- Kodi's addon
   loader keys off the `library_linux` attribute (correctly populated) to
   find the actual binary, not the `<platform>` metadata tag -- but that's
   inference from reading Kodi's own install/load logic, not yet
   confirmed against a real device.
   Per CoreELEC's own wiki, the result lands under `target/addons/`; this
   is now confirmed against a real run -- exact path is
   `target/addons/<DEVICE>/<KODI_MAJOR_VERSION>/<ARCH>/pvr.dispatcharr-unofficial/`,
   e.g. `target/addons/Amlogic-ng/21.3/arm/pvr.dispatcharr-unofficial/pvr.dispatcharr-unofficial-0.3.0.1.zip`
   for this build (the `.1` is CoreELEC's own `PKG_REV`, not part of this
   addon's own version).
5. Install the resulting zip on the N2+ via Kodi's "install from zip file"
   option, or copy it over SSH and extract into
   `/storage/.kodi/addons/pvr.dispatcharr-unofficial/` directly, then restart Kodi.
6. This build isn't automated in CI (see the "GitHub Actions job ...
   rejected" note above) -- if this is a real tagged release, also attach
   the zip to the GitHub Release by hand so CoreELEC users have something
   to download instead of having to build it themselves. Rename it first, to the scheme the other platforms use
   (the file name is not what Kodi installs by; the zip's own `addon.xml` is, and keeps CoreELEC's `<version>.<revision>`):
   ```bash
   cp target/addons/<DEVICE>/<KODI_MAJOR_VERSION>/<ARCH>/pvr.dispatcharr-unofficial/pvr.dispatcharr-unofficial-<version>.<revision>.zip \
      addon-pvr.dispatcharr-unofficial-<version>-coreelec-armv7.zip
   gh release upload <tag> addon-pvr.dispatcharr-unofficial-<version>-coreelec-armv7.zip
   ```

For later releases, only steps 1-2 and the `cp`/build in steps 3-4 need
repeating -- the CoreELEC checkout itself can be reused, and with the
toolchain and every dependency already built and cached, a rebuild that
only touches this addon's own package is fast (~1 minute wall clock,
confirmed live, versus well over an hour for the first build from
scratch).

### Manual cross-compile + side-load (fallback)

If you'd rather not maintain a package.mk (e.g. testing an unreleased
commit), cross-compile using the same toolchain CoreELEC's build system
uses for the N2+ (`Amlogic-ce`/`Amlogic-ng`, `arm` -- see the confirmed
details above; not `Amlogic-no`/`aarch64`, which was true of an older,
no-longer-recommended branch), matching their exact GCC version and the
Kodi commit their release is built from, then copy the
resulting `.so` and a rendered `addon.xml` into
`/storage/.kodi/addons/pvr.dispatcharr-unofficial/` over SSH and restart Kodi. This
is more fragile than the package.mk route above (nothing pins the toolchain
version for you), so prefer that unless you specifically need to test a
build that isn't tagged.

Either way, pin the Kodi source tree/version you build against to the same
major version CoreELEC's current release ships, or the addon will fail to
load with an API-version mismatch even if the library itself loads fine.

## Android (arm/arm64)

Confirmed live end to end (2026-09-15/16) against two real physical
devices on real Android builds, not an emulator: an older 32-bit ARM
device and a newer 64-bit ARM device, both against a real Dispatcharr
backend -- real channel list (a real, full-size channel lineup), real live
playback, real live-timeshift seek, on both architectures.

Unlike every other platform this addon has been built for, this addon's
own source (`src/*.cpp`) needed **zero** Android-specific changes --
only `CMakeLists.txt` needed one small addition (below). Kodi's own
upstream Android build tooling (`tools/depends`, the same "unified
depends" cross-compile system used under the hood for every non-native
target, including CoreELEC's own) has a real gap for binary addons that
CoreELEC's separate `package.mk`-based dependency wiring papers over,
and that this project's own Linux/macOS/Windows builds never hit either
(they all link a *shared* system/bundled libcurl) -- see "Android's
missing addon-dependency wiring" below before assuming a fresh Android
build will "just work" the way the other platforms do.

### 1. SDK/NDK toolchain

Kodi's own [`docs/README.Android.md`](https://github.com/xbmc/xbmc/blob/master/docs/README.Android.md)
in the Kodi source tree is the authoritative reference; summarized here
for this addon's own needs. Requires Android SDK cmdline-tools plus NDK
r21e specifically (Kodi's own recommended/most-tested revision for the
`Omega` branch this project already builds against everywhere else):

```bash
mkdir -p ~/android-build/sdk
curl -sL -o cmdline-tools.zip \
  "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
mkdir -p sdk/cmdline-tools
unzip -q cmdline-tools.zip -d sdk/cmdline-tools
mv sdk/cmdline-tools/cmdline-tools sdk/cmdline-tools/latest

cd sdk/cmdline-tools/latest/bin
yes | ./sdkmanager --sdk_root="$HOME/android-build/sdk" --licenses
./sdkmanager --sdk_root="$HOME/android-build/sdk" \
  platform-tools "platforms;android-34" "build-tools;33.0.1" "ndk;21.4.7075529"
```

Needs a JDK (confirmed live with OpenJDK 21) for `sdkmanager` itself --
unrelated to the addon's own C++ toolchain, which the NDK provides.

### 2. Configure and build Kodi's own dependency stack, per ABI

Each target ABI (`arm-linux-androideabi` for 32-bit ARM,
`aarch64-linux-android` for 64-bit ARM) needs its **own** configured
`tools/depends` tree -- confirmed live that running `./configure` twice
against the *same* Kodi source checkout for two different `--host`
triplets clobbers the first one's generated `Makefile.include` (there's
no per-target isolation the way `cmakebuildsys`'s own `BUILD_DIR` option
gives the full Kodi build). Use two separate checkouts (a cheap
`git worktree add --detach <path> <same-commit>` each, not a full second
clone) rather than reconfiguring one in place:

```bash
git worktree add --detach ~/android-build/kodi-source-arm64 <kodi-omega-commit>
git worktree add --detach ~/android-build/kodi-source-arm <kodi-omega-commit>

cd ~/android-build/kodi-source-arm64/tools/depends
./bootstrap
./configure --with-tarballs="$HOME/android-build/tarballs" \
  --host=aarch64-linux-android \
  --with-sdk-path="$HOME/android-build/sdk" \
  --with-ndk-path="$HOME/android-build/sdk/ndk/21.4.7075529" \
  --prefix="$HOME/android-build/xbmc-depends-arm64"
make -j$(nproc) -C tools/depends
```

Repeat for `~/android-build/kodi-source-arm` with
`--host=arm-linux-androideabi` and a separate `xbmc-depends-arm` prefix.
This builds curl, ffmpeg, and everything else Kodi itself needs, from
source, for that ABI -- expect a long first build (same shape as
CoreELEC's own from-scratch toolchain build). **`samba-gplv3` failing
with "Embedded Heimdal build requires flex but it was not found" is a
real, but harmless, failure for this addon specifically** -- it's
Kodi's own SMB/CIFS network-browsing support, entirely unrelated to a
Dispatcharr HTTP-only PVR client; confirmed the base dependency stack
(the part `tools/depends/target/binary-addons` actually needs) had
already logged `Dependencies built successfully.` before `make`
went on to attempt -- and fail on -- this unrelated optional package.
Install `flex` first if you'd rather avoid the failure entirely.

### 3. Android's missing addon-dependency wiring

`tools/depends/xbmc-addons.include` has dedicated `linux-system-libs`/
`linux-system-x11-libs`/etc. targets that symlink the host's own system
libraries into the addon build's own isolated dependency directory
(`tools/depends/target/binary-addons/<host-triplet>/build/depends/`,
confirmed by reading that file) -- but **no Android equivalent exists**.
Building this addon's own binary directly via
`tools/depends/target/binary-addons` (below) therefore fails
`find_package(CURL REQUIRED)` outright ("Could NOT find CURL") even
though curl was just built successfully in step 2 above, because
`CMAKE_FIND_ROOT_PATH_MODE_LIBRARY`/`_INCLUDE` are set to `ONLY`
(confirmed by reading the generated `Toolchain_binaddons.cmake`) and
`CMAKE_FIND_ROOT_PATH` never points at the real `xbmc-depends-arm64`/
`xbmc-depends-arm` prefix at all. Fixed by manually symlinking curl
(and, since Android's depends-built curl is static-only, its own
transitive link dependencies too) into that same isolated directory,
per ABI, before building:

```bash
DEPS_DIR=~/android-build/kodi-source-arm64/tools/depends/target/binary-addons/aarch64-linux-android-21-debug/build/depends
REAL_DEPENDS=~/android-build/xbmc-depends-arm64/aarch64-linux-android-21-debug
mkdir -p "$DEPS_DIR/include" "$DEPS_DIR/lib/pkgconfig"
ln -sf "$REAL_DEPENDS/include/curl" "$DEPS_DIR/include/curl"
for lib in libcurl.a libnghttp2.a libssl.a libcrypto.a libz.a; do
  ln -sf "$REAL_DEPENDS/lib/$lib" "$DEPS_DIR/lib/$lib"
done
```

(Repeat for the `arm`/`arm-linux-androideabi-21-debug` tree.) This still
isn't enough on its own -- `CMakeLists.txt` also needs to explicitly
list curl's own transitive static deps (OpenSSL, nghttp2, zlib) via
`find_library()`, since a bare library name in `target_link_libraries()`
becomes a plain `-lssl` passed straight to the linker (which only
searches its own default paths, not `CMAKE_FIND_ROOT_PATH`) -- confirmed
live this fails ("cannot find -lssl") even with the `.a` symlinked in
place above, since unlike zlib, Android's NDK doesn't bundle OpenSSL at
all. This addon's own `CMakeLists.txt` already has this handled (see its
`if(ANDROID)` block); a fresh third-party addon hitting the same "Could
NOT find CURL" error should start here rather than assuming its own
`CMakeLists.txt` is at fault.

### 4. Build the addon

Once steps 2-3 are done for both ABIs:

```bash
cd ~/android-build/kodi-source-arm64
make -j$(nproc) -C tools/depends/target/binary-addons \
  ADDONS="pvr.dispatcharr-unofficial" \
  ADDONS_DEFINITION_DIR="$HOME/android-build/addon-defs" \
  PREFIX="$HOME/android-build/install-arm64" \
  EXTRA_CMAKE_ARGS="-DPACKAGE_ZIP=ON" \
  PACKAGE=1
```

(`ADDONS_DEFINITION_DIR` points at a plain
`pvr.dispatcharr-unofficial.txt` containing
`pvr.dispatcharr-unofficial file:///path/to/this/repo`, same convention
as every other platform.) Repeat against the `kodi-source-arm` tree with
its own `PREFIX`. Produces
`addon-pvr.dispatcharr-unofficial-<version>-android-aarch64.zip`/
`-android-armv7.zip` under that build tree's own
`pvr.dispatcharr-unofficial-prefix/src/pvr.dispatcharr-unofficial-build/`.

**Strip the packaged `.so` before distributing it -- this build path
doesn't do it for you.** Unlike other platforms, where the build itself
already produces stripped output, this one builds against the
`-debug`-suffixed dependency trees "Android's missing addon-dependency
wiring" above requires, so the addon's own `.so` comes out with full
debug info -- including the local build machine's absolute paths -- still
attached. The zip has to be unpacked, stripped with the NDK's own tool for
that ABI (the host's `strip` does not know ARM), and packed again (the
release gate, below, then normalizes and checks it):

```bash
NDK_BIN=~/android-build/sdk/ndk/21.4.7075529/toolchains/llvm/prebuilt/linux-x86_64/bin
# aarch64: $NDK_BIN/aarch64-linux-android-strip   armv7: $NDK_BIN/arm-linux-androideabi-strip
rm -rf /tmp/android-zip && mkdir /tmp/android-zip && cd /tmp/android-zip
unzip -q <the-built>/addon-pvr.dispatcharr-unofficial-<version>-android-aarch64.zip
"$NDK_BIN/aarch64-linux-android-strip" --strip-unneeded pvr.dispatcharr-unofficial/libpvr.dispatcharr-unofficial.so
TZ=UTC zip -X -r ../addon-pvr.dispatcharr-unofficial-<version>-android-aarch64.zip pvr.dispatcharr-unofficial
```

**Stripping is not enough: two statically linked dependencies still carry the build machine's paths.** nghttp2's
assertion messages name their source files, and OpenSSL records its configure line, compiler and install
directories (found 2026-10-07 in the first stripped zips: both libraries come from Kodi's own `-debug`
dependency tree under the builder's home directory, and they are string data, not debug information).
Rebuilding that tree under a neutral path takes hours, so after stripping, blank every string in the library that
contains the home-directory prefix (everything up to the string's last `/` becomes `/build` plus NUL padding, same size, same
program: the text is only an error message, a configure line or a directory a device does not have; the tool refuses a string
that is not plain text). The last path component stays on purpose: the linker merges string tails, and libcurl's `"1.1"`
HTTP-version literal was the end of an OpenSSL `.../engines-1.1` path, so blanking that path whole sent every request as
`GET /... HTTP/` and nginx answered 400 (found 2026-10-07 on a real phone; an earlier whole-string version of this tool
shipped in no release). Always run the addon on a device against a real server after scrubbing:

```bash
python3 tools/scrub_zip_paths.py --prefix "$HOME" <both android zips>
```

The release gate (below) is what proves it worked; confirmed live that the scrubbed library is still a valid
shared object with the addon's entry points exported. The macOS zip needed none of this (its dylib links the
system's own libraries and passed the gate as built).

Confirmed live: the `0.11.0` release (the first Android release) shipped
both ABIs' `.so` unstripped, with the build machine's local paths
embedded, and the published assets were still the unstripped builds when
re-checked on 2026-10-03 (an earlier note here claiming they had been
corrected was wrong). That is why `tools/check_release_zip.py` now fails
any release zip whose ELF library still has `.debug_*` or `.symtab`
sections, or any member that still contains a build-machine path.

### 5. Install Kodi and side-load the addon on a real device

Kodi for Android isn't preinstalled by any stock/LineageOS image --
install the official APK matching the addon zip's own ABI from
[Kodi's mirror](https://mirrors.kodi.tv/releases/android/), e.g.
`arm64-v8a/kodi-21.3-Omega-arm64-v8a.apk` (confirmed real package id
`org.xbmc.kodi`, min SDK 21, target SDK 34 via `aapt dump badging` --
comfortably compatible with both devices
tested). Over `adb`:

```bash
adb install -r kodi-21.3-Omega-arm64-v8a.apk
adb shell am start -n org.xbmc.kodi/.Splash
```

First launch needs the "All files access" permission granted (Settings
-> Apps -> Kodi -> All files access) before it stops blocking on its own
"Kodi requires access to your device media and files" prompt -- confirmed
this is genuinely required, not optional, on both devices tested. Kodi's real home directory on Android is
`/sdcard/Android/data/org.xbmc.kodi/files/.kodi/` (confirmed live,
standard `addons`/`userdata`/etc. layout, same as every other platform)
-- side-load by extracting the built zip's own
`pvr.dispatcharr-unofficial/` directory straight into `.kodi/addons/`
over `adb push` (Kodi stopped first), then
`Addons.SetAddonEnabled` over JSON-RPC once restarted, exactly like this
project's other manual-drop platforms (Linux/Windows/CoreELEC).

**Enabling `services.webserver` via a direct `guisettings.xml` edit
(this project's usual provisioning trick on every other platform) isn't
enough on Android by itself -- confirmed live.** Kodi shows a one-time
"Web server" info dialog and silently re-disables the setting if a
username/password isn't *also* configured: "You have previously enabled
the web interface without setting up a password. The web server has
been disabled until you either explicitly allow this or set up
authentication." Set `services.webserverusername`/
`services.webserverpassword` in the same edit as `services.webserver`
to avoid this -- confirmed fixed once both were set together.

Once JSON-RPC is reachable, `tools/kodi_smoke_test.py` works completely
unmodified -- same as every other platform, since it only ever speaks
Kodi's own platform-generic JSON-RPC API.

### A large real channel count can outrun Kodi's own first-boot EPG sync on old/slow hardware

Confirmed live (2026-09-15/16) on an older, low-power 32-bit ARM device
against a real account with a real, full-size channel lineup: on a fresh install,
`PVR.GetBroadcasts` returned real, correctly-titled programme data for
every channel, but roughly 11% of channels were
consistently missing their own `broadcastid` field -- while a newer,
faster 64-bit ARM device, given far more elapsed idle time beforehand, showed
every channel correctly. This is **not an addon bug**: confirmed by
adding temporary diagnostic logging directly to `GetEPGForChannel()`
(logging both the computed id via `ComputeBroadcastId()` and an
immediate `tag.GetUniqueBroadcastId()` readback right before
`results.Add()`) that this addon's own id computation is correct --
plain, architecture-independent `uint32_t` arithmetic, verified to
produce real non-zero values -- for every channel Kodi actually asked
about. The affected channels were never asked about at all: this
addon's own `StartChannelEpgRefreshThread()` (see `src/PVRDispatcharr.cpp`)
calls Kodi's `TriggerEpgUpdate()` once per channel, for all channels
unconditionally, on a loop meant to repeat every `kChannelEpgRefreshCheckMinutes`
(10) -- but on this device, its own "background thread refreshed
channels/groups" log line appeared exactly **once** across a 28+ minute
observation window (confirmed via `kodi.log`, not inferred), meaning
Kodi's own handling of that very first round of `TriggerEpgUpdate()`
calls hadn't finished, and was in fact frozen at the same channel count
(still under 90% of the total) for over 15 consecutive minutes -- consistent with a Kodi-core-side
threading/performance limitation surfacing under an unusually large
channel count on old hardware, not a hang in this addon's own code (which
never blocks in that loop -- `TriggerEpgUpdate()` is a Kodi SDK call this
addon does not implement).
Reproduced on a genuinely fresh install (`adb shell pm clear
org.xbmc.kodi`, confirmed via `ls .kodi/addons/` showing the addon gone
and a fresh, ~32KB `Epg16.db`), ruling out stale state from repeated
side-load/redeploy cycles during testing as the cause. Real-world impact
looks narrow: live playback, live-timeshift seek, catch-up playback,
real timer/recurring-rule creation, and in-progress-recording
playback/seek all passed on this same device before this was
investigated -- normal use of the addon isn't blocked, only a subset of
channels' guide data (and, by extension, "Record" from a guide entry for
those specific channels) may be incomplete for a while after a very
first cold boot on old/slow hardware with a very large channel count.
Not pursued further -- root-causing past this point would need Kodi-core
thread-dump-level debugging, disproportionate given normal functionality
is unaffected. Revisit if a real user reports this specifically.

## Release assets

**Every release ships the same eight zips, built from the tag's own commit, and none is published before the
privacy gate passes on all of them together.**

| Asset | Built by |
| --- | --- |
| `addon-pvr.dispatcharr-unofficial-<v>-windows-x86_64.zip` | CI (`build-windows`) |
| `addon-pvr.dispatcharr-unofficial-<v>-linux.zip` | CI (`build-unix`) |
| `timeshift_buffer.zip`, `recording_edl.zip` | CI (`package-dispatcharr-plugins`) |
| `addon-pvr.dispatcharr-unofficial-<v>-osx-arm64.zip` | by hand on the Mac ("Linux / macOS" above) |
| `addon-pvr.dispatcharr-unofficial-<v>-android-aarch64.zip`, `...-android-armv7.zip` | by hand ("Android" above) |
| `addon-pvr.dispatcharr-unofficial-<v>-coreelec-armv7.zip` (the CoreELEC package, renamed from CoreELEC's `pvr.dispatcharr-unofficial-<v>.1.zip`; the `.1` is its own revision suffix and stays inside the zip's `addon.xml`) | by hand ("CoreELEC" below) |

Cutting a release, in order:

1. Tag the commit and push the tag; wait for CI on it to pass, and download its three artifacts.
2. Build the hand-built assets from that tag's source, not from a working tree: export it (`git archive <tag>`
   into a clean directory) and point the build's addon definition at the export, so a local edit can never
   ride along. Strip the Android libraries (above). Collect all eight zips in one directory.
3. Scrub the Android zips (`tools/scrub_zip_paths.py`, "Android" above), then normalize every hand-built zip,
   which removes the build machine's timezone from its entry timestamps:
   `python3 tools/normalize_zip.py "$(git log -1 --format=%ct <tag>)" <zip>...`.
4. Run the gate once over all of them, with the private blocklist (not in this repository; the script looks for
   `--blocklist FILE`, then `$PRIVACY_BLOCKLIST`, then `~/.privacy-blocklist.txt`):
   ```bash
   python3 tools/check_release_zip.py --require-blocklist --expect-release <version> <all eight zips>
   ```
   It fails on a build-machine path (`/home/`, `/Users/`, `C:\`, a CI runner's work directory) or a blocklist
   term anywhere in a member (as text and as UTF-16), any extra field in the zip (timezone, uid/gid), a zip
   comment, an unsafe entry name, a debug or log file, an ELF library that is not stripped, and a release that is
   missing an asset or carries an unexpected one. The handful of upstream strings it has to allow (the Windows
   zip's two prebuilt DLLs carry Kodi's own dependency-build path) are listed in the script with the reason each
   is safe. CI runs the pattern half of the same gate on every zip it builds; only a local run has the
   blocklist. Fix the cause and rebuild, never edit the gate to make
   a zip pass.
5. Only then publish: `gh release create <tag> --title <tag> --notes-file <notes> <the eight zips>`. The release page and its notes are made by hand; `CLAUDE.md`'s "Publishing a
   release's assets" bullet has the notes rules.

A zip that is rebuilt for any reason goes through steps 3 and 4 again; the gate checks the zip that will be
uploaded, not an earlier copy.

## Distribution (Windows/macOS, once built)

Kodi installs binary addons either as a manual zip, or from a self-hosted
repository (a small `repository.xml`-style addon whose own zip contains an
`addons.xml` index pointing at your built zips, served over `https://` or
GitHub Pages). Claude Code can generate that repository structure once the
desktop builds above are working, if you want "install from repository"
rather than "install from zip file."

## What Windows TLS this addon actually uses (2026-10-02)

The Windows release zip carries `libcurl.dll` and `zlib.dll` (curl 7.67.0 with an OpenSSL 1.1.1d compiled into it), but they are not what runs. On a stock Kodi for Windows, `kodi.exe` already has its own `libcurl.dll` and `zlib.dll` (the same versions) loaded from its install directory, and Windows will not load a second DLL under a name that is already loaded, so the addon's HTTPS goes through Kodi's copy -- checked on a real Kodi 21.3 install by listing the modules of the running process. That makes this addon's TLS exactly as current as Kodi's own, an upstream dependency shared by every addon and by Kodi itself; Kodi's dependency mirror has no newer prebuilt x64 curl than 7.67.0 to move to. The bundled copies are kept only so the addon still loads on a Kodi build that does not ship its own. If this ever needs to change, the realistic route is building a current curl from a pinned source commit with Schannel and linking it statically into the addon DLL on Windows -- a build-system change that diverges from Kodi's own choice and alters certificate and revocation behaviour, so it is a deliberate project, not a version bump (`docs/OPEN_ITEMS.md`, "Windows release bundles end-of-life OpenSSL 1.1.1d and curl 7.67.0").
