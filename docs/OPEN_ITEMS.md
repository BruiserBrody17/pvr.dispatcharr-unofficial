*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**23 open, 320 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the two
2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one is open from a user
report. The other 20 come from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code the
same day (all 21 held here; three are worded for `Omega`; one fixed on 2026-10-19 and moved to `CLOSED_ITEMS.md`), each marked with how sure the audit was: *confirmed*
(reproduced, or the whole code path read end to end), *plausible* (the logic is there, the trigger is not demonstrated)
or *low confidence*.

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 7 |
| Open: Architectural / concurrency | 4 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 7 |
| Closed (in CLOSED_ITEMS.md): Fixed | 278 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 33 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### The addon never calls `curl_global_init` itself

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); low
confidence.** libcurl initialises itself lazily on the first `curl_easy_init`, which is not thread-safe before curl
7.84, and the addon starts transfers from several threads at once. The Windows build bundles curl 7.67.0 and Android
links a static curl. To check: whether Kodi's own earlier initialisation covers the addon's copy on those platforms. If
it does not, one `curl_global_init(CURL_GLOBAL_ALL)` in the addon's create path (and the matching cleanup on destroy) is
the fix.

### Fix known, not yet done

#### Support Kodi 22 (Piers): the addon is refused by Kodi 22 RC1

**Opened 2026-10-12, from a user's report: on Kodi 22 RC1 (a Windows laptop) the addon "is not supported yet".** Kodi 22's PVR API is 9.2.0 with a minimum of 9.2.0 (Omega, which this branch builds against: 8.3.0, minimum 8.2.0), and its main and general APIs moved to 2.0.4 and 1.0.5, so the addon's declared imports and an Omega-built binary are refused outright; it must be rebuilt against Kodi 22's headers. A syntax-only compile of every `src/` file against the Piers dev-kit headers (branch `Piers`, 2026-10-11) gave six errors, all in `PVRDispatcharr.h`: the five recorded-stream overrides (`OpenRecordedStream()` now also returns a stream id, `ReadRecordedStream()`, `SeekRecordedStream()`, `LengthRecordedStream()` and `CloseRecordedStream()` take it), and `GetChannelStreamProperties()`, which gains a `PVR_SOURCE` argument (regular live, or playing a guide entry as live). The C++ wrapper hides the rest of the change (the C structs now hold string pointers instead of fixed arrays, EDL and timer-type lists are heap arrays), so most of `src/` compiles unchanged. What the port still needs: (1) the stream id means Kodi may hold more than one recorded stream open, but `DispatcharrClient` keeps one `m_recordingStream` and one `m_inProgressRecordingStream`, so either the state is keyed by id or a second open is refused deliberately; (2) the glue harness's stub and driver use the C structs directly and need the new shapes; (3) new optional calls worth a look, `GetRecordedStreamTimes()`, `IsRecordedStreamRealTime()` and `PauseRecordedStream()`, which bear on how an in-progress recording behaves in the player and on the open item about watching live while a recording runs; (4) `addon.xml.in` import versions, `KODI_COMMIT` and `KODI_BRANCH` in the workflow, and the Windows, Android and CoreELEC builds against Piers (`docs/BUILDING.md` notes `coreelec-22` tracks it); (5) the branching policy in `CLAUDE.md`: branch `Piers` off `Omega` once the source differs, keep `Omega` as the Kodi 21 maintenance branch, and decide how fixes flow between them (merge `Omega` into `Piers`; the six signatures are the only divergence so far). Confirm live on Kodi 22 RC1 with a Windows build: the addon loads, channels and guide appear, a live channel plays in each timeshift mode, a recording plays (completed and in progress) and seeks, and the timers dialog works.

#### Deeply nested `custom_properties` can overflow the stack on Android

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed.**
`cond ? item["x"] : nlohmann::json()` bound to a `const nlohmann::json&` is a prvalue, so it deep-copies the whole
subtree, and nlohmann's copy constructor recurses (parsing and destruction do not). It is in `ParseRecordingFields()`
and two more places in `RecordingParser.cpp`, and the same shape is in `TimerRuleParser.cpp`, `PluginRunResult.cpp` and
three places in `DispatcharrClient.cpp` (one copies the signed-in user's own `custom_properties`). A recording whose
`custom_properties` nests about 8000 deep crashed the real parser on a 1 MiB thread stack (bionic's default; 5000
passed). Python's json round-trips that depth, so a user with DVR access could store it and every client would crash on
each recording refresh. Not checked: whether Dispatcharr caps the depth anywhere. Fix: a `static const nlohmann::json
kNull;` fallback (or pointers) so nothing is copied, verified to depth 100000; optionally a depth cap in
`ParseJsonResponseBody()`. Regression test: a 100000-deep value parsed on a 1 MiB-stack thread in
`tests/test_recording_parser.cpp`.

#### Pressing Play on a recording can freeze the UI for the full request timeout

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** `GetRecordingStreamProperties()` runs a full-timeout `GetRecordingById()` (which can also log in and retry
once on a 401) on Kodi's main thread, where Kodi calls it while preparing playback, so a hung server freezes the UI for
30 to 180 s before playback starts. On Kodi 21 its only output, ISREALTIMESTREAM, is ignored for a PVR recording:
`CInputStreamPVRBase::IsRealtime()` asks the addon's `IsRealTimeStream()` instead of reading the property (checked
against the Kodi 21 source). Fix: drop the lookup, or bound it with `ShortRequestTimeoutMs()`, or answer from the cached
recordings. Same shape, smaller: `GetEPGTagStreamProperties()` makes the catch-up POST on the main thread with the full
timeout, and its duration in minutes is `(end - start) / 60`, which rounds down, so a programme that is not a whole
number of minutes asks for a session up to 59 s short. Live-only (no unit test).

#### `PersistApiKeyIfChanged()` writes three settings on the stream read thread

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** Each write is a `settings.xml` save plus a full re-delivery of the settings to the addon. Write the owner
stamps only when they actually change.

#### The XMLTV download follows redirects with no limit or protocol restriction

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); hardening
only.** `GetXmlTvGuide()` sets `CURLOPT_FOLLOWLOCATION` with no redirect limit (`CURLOPT_MAXREDIRS`) and no protocol
restriction. No credentials go with that request, so nothing leaks; it can loop or be sent to a non-HTTP scheme. Set
both (the protocol option's name depends on the curl version: the Windows build bundles 7.67.0), or follow hops by hand
under `RedirectPolicy` as the authenticated requests already do.

#### timeshift_buffer: a corrupt buffer state aborts the orphan scrub, and client disconnects print tracebacks

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** (1) A buffer state key in Redis that is not valid JSON makes `_get_buffer_state` raise inside
`_is_untracked_orphan`, which ends the whole scrub pass rather than skipping that one entry. (2) A client that
disconnects mid-response raises `BrokenPipeError` or `ConnectionResetError` into `socketserver`'s default
`handle_error`, which prints a raw traceback; override it on the file server to log one line. Regression tests in the
plugin's `tests/`.

#### timeshift_buffer: smaller hardening (setting caps, ffmpeg I/O timeout, process liveness, token rows)

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day).** (1)
`buffer_minutes`, `max_concurrent_buffers` and `idle_timeout_seconds` have no upper limit; cap them. (2) The ffmpeg
input has no I/O timeout; add `-rw_timeout` (low confidence that it helps with the provider streams in use). (3)
`_is_process_alive` treats a `PermissionError` as alive before it compares the start ticks (low confidence; depends on
whether the container runs as root). (4) `RefreshToken.for_user` may add a database row per buffer start if Django's
`token_blacklist` app is installed (low confidence; not checked on a real server).

### Architectural / concurrency

#### A background task scheduled during shutdown can crash Kodi

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading, narrow window.** The destructor moves `m_detachedThreads` out and joins it *before* it stops the three worker
threads, and `TrackDetachedThread()` has no shutdown check. Two callers can still add a thread after that drain:
`RetryDeferredServerSyncs()` calls `ScheduleConnectionReport()` without checking `m_shuttingDown`, on the channel/EPG
thread that is still running, and `OnConnectionObserved()` checks the flag and then schedules, which races the flag
being set. A joinable `std::thread` left in the vector when it is destroyed calls `std::terminate`, and the task holds
`this` while the object is being torn down. The closed fix for this list covered only the `AddTimer` and settings
callers; the connection-report caller came later. Fix: in `TrackDetachedThread()`, check `m_shuttingDown` under
`m_detachedThreadsMutex` and drop the task (or run it inline); drain the list a second time after the
`StopWorkerThread()` calls. Glue harness, not a unit test.

#### timeshift_buffer: a teardown that outlasts its grace period can delete a restarted buffer

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); plausible.**
`_teardown_buffer` ends with an unconditional `_delete_buffer_state` and does not hold the start lock; the "stopping"
marker keeps `start_buffer` away for only `_TEARDOWN_GRACE_SECONDS` (30 s). If removing the segments takes longer
(thousands of them, slow storage; with no cap on `buffer_minutes`), a `start_buffer` for the same channel classifies the
buffer as dead and starts a new ffmpeg, and the old teardown then deletes the new state. The viewer gets 403s and the
new ffmpeg holds a provider slot, untracked, until the orphan scrub about 300 s later; the final `rmdir` of the old
rmtree can also remove the new directory. `docs/TEST_COVERAGE.md` describes this race as closed by the grace period,
which does not cover a teardown longer than 30 s; correct it with the fix. Fix: a compare-and-delete for the state (a
Lua script that deletes only if the pid and `started_at` still match), and rename the directory to a tombstone before
removing it. In the same area: `stop_buffer` is not bound to one buffer instance, so a late stop can tear down a
replacement; send and check its `access_token`. Regression tests in the plugin's `tests/`.

#### A refresh while Kodi's timer dialog is open can revert another client's change to a series rule

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); plausible.**
`UpdateTimer()` diffs the dialog's values against `m_reportedRecurringRules` and `m_reportedRecurringRuleTimes`, but any
`GetTimers()` that finishes while the dialog is open overwrites that baseline. If another client changes the rule's name
or days meanwhile and a WebSocket event or the 5-minute refresh picks it up, the dialog's old values look like edits and
are PATCHed back: the revert the 2026-10-02 fix was meant to stop. Smaller race in the same code: `GetTimers()` clears
the times map and refills it rule by rule, so an `UpdateTimer()` in that gap falls back to re-derived times and brings
back the DST time-shift edit. Fix: keep the snapshot the dialog was opened from per rule, and build the times map
locally and swap it in under one lock. The baseline choice is testable in `RecurringRuleEdit`.

#### An unsynchronised spot in `PVRDispatcharr`'s padding sync

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); harmless in
practice.** The padding sync checks under the mutex but writes after releasing it, so a timer-dialog edit that lands in
that window can be overwritten and then latched. (`Piers`'s entry has a second spot, `m_apiKeyOwnershipVerified` written
by concurrent recorded-stream opens; `Omega` opens one recorded stream at a time, so it does not apply here.)

### Design decision needed

#### recording_edl: a DVR path template may resolve outside the recordings root

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); deliberate
today.** `_resolve_scan_root` refuses only the recordings root itself or an ancestor of it; a template that is absolute,
or whose `..` segments lead somewhere else, is scanned and scrubbed there on purpose (its docstring explains why: such
recordings' sidecars still need finding). Templates are set by a Dispatcharr admin, so this is not reachable by an
ordinary user. Decision: keep it as is, or require the resolved root to be inside a configured allow-list of
directories.

### Release, CI and manual testing

#### The 32-bit unit tests run without UBSan

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day).**
`unit-tests-32bit` builds with `-m32` only, and `unit-tests-sanitized` is 64-bit, so an overflow that only happens with
a 32-bit `time_t` or `long` is caught only when it gives a wrong answer a test checks, never as the overflow itself (the
`catchup_days` one, fixed in `CLOSED_ITEMS.md`'s "Integer overflows in pure helpers, two of them 32-bit-only", has a
test that fails there by its wrong answer). Add `-fsanitize=undefined` to the 32-bit job.

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### Wake-on-LAN for a Dispatcharr host that sleeps

**Opened 2026-10-10, from the pvr.hts comparison; deferred.** pvr.hts sends a Wake-on-LAN packet to a configured MAC
address (`kodi::network::WakeOnLan`, part of Kodi's addon API) before its first connect, for a backend that sleeps when
idle. Cheap to add as an optional setting, but Dispatcharr is usually a container on an always-on host and nobody has
asked. Pick it up if a user reports a sleeping server; the whole change is the setting, one call before the first
request, and a note in the README.

#### Multiple Dispatcharr servers from one Kodi

**Opened 2026-10-10, from the pvr.hts comparison; deferred.** Kodi 20 and later let one PVR addon run several backend
instances (`instance-settings.xml` next to `settings.xml`); pvr.hts added it in 20.4.0 with a one-time migration of the
pre-instance settings in its `addon.cpp`. This addon is one instance, and `docs/API_NOTES.md`'s "Single-instance
assumption: partially hardened, not fully" section lists what else assumes that (the API-key persistence, the recurring
rule tag). Worth doing only if someone runs two Dispatcharr servers, or wants two accounts (admin for the companion
plugins, non-admin otherwise) against one server from one Kodi; the migration step and the per-instance settings
handling in `OnAddonSettingChanged()` are the work. Not started.

### Tooling (tools/)

#### The release gate does not catch local-clock entry timestamps

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed with a
crafted zip.** The gate only catches the UT/ux extra-field form of a timestamp; a zip whose DOS times are local clock
time and carry no extra field passes (exit 0), and the difference against the public commit or upload time gives away
the offset. That is what a zip made without `tools/normalize_zip.py` looks like (Python's `shutil`, Explorer).
`CLOSED_ITEMS.md` describes the time zone in entry timestamps as mechanically gated, which this narrows. The 0.13.0 zips
are clean (checked 2026-10-18: one time shared by every entry of each zip, in UTC, and no extra fields; the addon zips
carry the tag commit's time, the plugin zips CI's checkout time). Fix: require one shared `date_time` across all
entries, and with `--expect-release`, optionally require it to equal the tag commit's time. Test in `tools/tests/`.

#### `--expect-release` checks file names only

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** `check_release_set` never opens a zip, so a stale zip renamed to the new version's name passes. Fix: read
`addon.xml` in each addon zip and require the version (CoreELEC: the version plus its revision), and in each plugin zip
cross-check `plugin.json` against the `Plugin` class `version` in `plugin.py`. Test in `tools/tests/`.

#### `--require-blocklist` can pass with a list that blocks nothing

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed, all
latent with today's list.** In `load_blocklist`: an empty, comments-only or UTF-16 file passes; a mistyped explicit
`--blocklist` path silently falls back to `$PRIVACY_BLOCKLIST` or the home-directory list; a byte-order mark sticks to
the first term so it never matches; and a non-ASCII term never matches UTF-8 content. Fix: an error on a missing
explicit path, a failure on zero terms, reading with `utf-8-sig` and refusing NUL bytes, and searching each term's UTF-8
bytes. Test in `tools/tests/`.

#### The release gate's path, debug and structure checks have gaps

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day).** Paths it does
not recognise: `C:/...` with a forward slash, UNC paths, `/media/<user>`, `/run/media` and `/Volumes`. Debug content it
misses: members inside a `.dSYM` bundle and `.zdebug_*` sections. Zip structure it does not look at: extra fields only
in the local headers, nested archives, symlink entries and drive-letter entry names. Each is a small pattern or check
plus a test in `tools/tests/`.

#### `scrub_zip_paths.py` can leave a username when a string ends at the prefix

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** A string that ends exactly at `/home/<user>` (no trailing slash) keeps the name. Fix the pattern to match end
of string too; test in `tools/tests/`.

#### CI skips the build when a file is renamed out of a build path

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed in a
scratch repo.** `tools/ci_build_needed.py` runs `git diff --name-only`, which detects renames and prints only the new
path, so `git mv src/Foo.cpp docs/` (or renaming a tool the workflow calls) reports no build needed and skips the very
jobs that would fail. Fix: add `--no-renames`; test in `tools/tests/test_ci_build_needed.py`. Related, in the workflow:
the `changes` step pipes the script through `tee` with no `pipefail`, so if the script crashes the outputs are empty and
every build job is skipped silently; use `shell: bash` and make the job conditions `!= 'false'` so an empty output
builds.

#### Lab scripts put passwords on command lines

**Opened 2026-10-18, from the read-only audit of `Piers` (checked against `Omega`'s code the same day); confirmed by
reading.** `tools/kodi_provision_windows.sh`'s `--enable-autologon` sends the account password inside the remote
command, which contradicts the script's own header and breaks on a `"` in the password; send it over stdin.
`tools/kodi_smoke_test.py` takes passwords as arguments (visible in the process list) and only speaks plain HTTP; take
them from the environment or a prompt.