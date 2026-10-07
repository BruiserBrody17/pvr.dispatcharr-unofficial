*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to **Closed** with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**5 open, 265 closed.**

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 4 |
| Open: Tooling (tools/) | 0 |
| Closed: Fixed | 234 |
| Closed: Closed without a change (refuted, explained or harmless) | 23 |
| Closed: Project history and test infrastructure | 8 |

## Open

### Needs a live check

### Fix known, not yet done

### Architectural / concurrency

### Design decision needed

### Release, CI and manual testing

#### Release upload shares jobs with pull-request builds

**Split out 2026-10-06 from the closed entry on the moving Kodi branch.** The jobs that hold `contents: write` for the release upload (`build-unix`, `package-dispatcharr-plugins`, `build-windows`) also compile pull-request code from same-repo branches, so a branch's build step runs with a token that can write releases. Fork pull requests are already excluded from the Windows job and run with a read-only token elsewhere, and only the maintainer can push same-repo branches, so the exposure is small. The fix is a tag-only release job that `needs:` the build jobs and downloads their artifacts, with `contents: write` only there. Not done: it moves every release asset through artifact upload and download (the workflow already uploads with the current artifact action on this runner; downloading in a later job has not been tried here), and a release can only be exercised by cutting a tag, so it should be done together with the next release rather than blind. **Added 2026-10-06 (sixteenth sweep):** the same job should `needs:` every test job (`lint`, the unit-test jobs, `glue-harness`): today the three upload jobs have no `needs:`, so a tag whose tests fail still publishes its assets, and the three jobs create the same release at once (a `softprops/action-gh-release` race).

### Upstream (Dispatcharr) or documentation accuracy

### Known gaps, deliberately deferred

#### Order-sensitive response decisions still inline in `DispatcharrClient`

**Opened 2026-10-06 by the sixteenth hardening sweep.** The recording read and the live segment fetch moved into `ClassifyRecordingReadResponse()`/`ClassifyLiveSegmentResponse()`; the same kind of chain remains, untested as a whole, in the in-progress segment fetch (`ReadInProgressRecordingStream()`), the recording open probe (`OpenRecordingStream()`: 401 retry, failure, an in-progress HLS redirect, a `CURLE_FILESIZE_EXCEEDED` read as success), `EnsureAuthenticated()`'s gate order (fast path, credential backoff, transient cooldown, refresh, classify, login: moving the cooldown after the refresh attempt reintroduces the 47th-pass storm and only the glue harness might catch it), `PerformWithSafeRedirects()` (unsafe-target check before the hop limit) and the segment span arithmetic shared by three read paths (a helper for it would need a guard for a non-positive available count). Each is a small enum-returning extraction like the two above. Deferred: none has a known bug, and each extraction touches a hot, lock-sensitive path that wants a live check after.

#### Nothing on a 64-bit build checks that the date parsers use the saturating time conversion

**Opened 2026-10-06 by the sixteenth hardening sweep (recorded before only inside closed entries).** `SaturatingTimeGm()` makes a date from 2038 on read correctly on a 32-bit `time_t`, and the tests that cover it run on the build host, where `time_t` is 64 bits and a plain `timegm()` would give the same answers: nothing fails if a parser goes back to the plain call. Closing it needs a 32-bit build in CI (the Linux runner can compile with `-m32` where the multilib packages exist) or a test seam that substitutes a narrow `time_t`. Deferred as low value: the only parsers affected are the ones a recurring rule's end date and an XMLTV time go through, and a regression would show on the Android and 32-bit ARM builds this project does not ship from CI.

#### A stop while the realtime socket is blocked in a send is not tested

**Opened 2026-10-06 by the sixteenth hardening sweep (recorded before only inside a closed entry).** `WebSocketClient`'s sends (`SendAll()`) wait on the peer's receive window, and the stop check added with the 2026-10-04 sweep is made between waits of 500 ms; a stop that arrives while a send is blocked on a full send buffer (a peer that has stopped reading) is believed to end within one slice but no test fills a send buffer to prove it. The local-server fixture could do it with a peer that never reads. Deferred: a pong or close frame is under 140 bytes, so the buffer would need to be full already.

#### A host clock step still misfires the reaper's idle checks (0.8.11 only fixed cross-host skew)

**Opened 2026-10-06 by the sixteenth hardening sweep, from reading.** `_shared_now()` (0.8.11) reads Redis's own `TIME`, which fixes timestamps written by a worker on one host and compared by another whose clock differs. It does not protect against the host's own clock being stepped (a manual change, a VM resume): Redis `TIME` is the Redis host's wall clock, containers on one host share it (the usual deployment; the all-in-one image runs Redis in the same container), so it moves exactly as far as `time.time()`, and the reaper's `now - last_heartbeat` compares misfire the way they did before. 0.8.11's changelog and docstring said otherwise and were corrected. A real fix makes the elected reaper measure age on its own monotonic clock: remember, per buffer and viewer, when it last saw the heartbeat value change, and reap on that age. Deferred: it is a redesign of the reaper's bookkeeping for a rare event (a clock stepped by more than `idle_timeout_seconds`, which makes viewers look idle, so buffers are stopped and the addon reports them gone), and a viewer's next start recovers.

### Tooling (tools/)

## Closed

### Fixed

#### The hand-built release zips had no mechanical privacy gate

**Fixed 2026-10-07 (found while cutting 0.12.0, when macOS and Android were asked to ship with every release; confirmed by running the new gate on the real `0.11.0` Android zip, which it fails).** The privacy rules for a release zip (no build-machine path in any binary, no timezone in the entry timestamps, stripped libraries) lived as a manual step in `CLAUDE.md`: run `strings`, look. That is how the `0.11.0` Android libraries shipped unstripped with the build machine's paths, how the Linux zip shipped with its local clock until the 0.12.0 sweep, and it left the hand-built platforms (macOS, Android, CoreELEC) with a weaker check than the CI-built ones. `tools/check_release_zip.py` is now the one gate, run over every zip whoever built it: build-machine paths (`/home/`, `/Users/`, `/root/`, `/var/folders/`, a CI runner's work directory, a `C:\` path, a `\Users\` path) in any member as text and as UTF-16 at both alignments, every term of the private blocklist (never in the repository; `--require-blocklist` fails closed without one), any extra field in the zip (the timezone field and the uid/gid field), a zip comment, unsafe entry names, debug and log members (`.pdb`, `.dSYM`, `.debug`, `.map`, `.log`), ELF libraries still carrying `.debug_*` or `.symtab`, and, with `--expect-release`, a set that is not exactly the release's eight assets (so a platform cannot be left out by forgetting a step). It allows only the strings it has to: the Windows zip's prebuilt curl and zlib DLLs carry Kodi's own dependency-build path (`C:\code\kodi-deps\Build\x64`, found by running the gate on the real 0.12.0 CI zips; identical in every copy of those DLLs), scoped to those two members so the same text in the addon's own binary still fails. CI runs the pattern half on the Linux, plugin and Windows zips (the Windows step warns instead of failing on a runner with no Python, because the release procedure runs the gate on that zip anyway). Tested with synthetic zips for every rule, an ELF built byte by byte, and 16 deliberate mutations of the script, all caught. `docs/BUILDING.md`'s "Release assets" is the procedure and `CLAUDE.md` says every release ships all eight zips. Building the first release set through it found one more thing: the stripped Android libraries still carried the builder's home directory, because the statically linked nghttp2 (its assertion messages) and OpenSSL (its configure line and install directories) come from Kodi's debug dependency tree and record their own build paths as string data. Rebuilding that tree under a neutral path takes hours, so `tools/scrub_zip_paths.py` overwrites the prefix in place with a same-length neutral one (29 replacements per ABI; the library is still a valid shared object with its entry points, and both zips then pass the gate with all blocklist terms); the gate stays the judge. The macOS zip passed as built, and the first draft of the gate's drive-path pattern flagged drive-shaped bytes in machine code, which is why it now needs a non-alphanumeric before the letter and path characters after the backslash.

#### The pre-release sweep of 0.12.0: the Linux release zip carried the build machine's timezone

**Fixed 2026-10-07 (the pre-release data-hardening sweep for 0.12.0; confirmed by reading the CI-built zip's entries).** The Linux leg of CI (`build-unix`) uploaded CPack's zip as it came: every entry stored its time twice, as the build machine's local clock and as UTC (the "UT" extended-timestamp field, 20 of them in the zip), so anyone downloading the release could read the runner's UTC offset off the difference. The Windows leg had its PowerShell rewrite (2026-10-03) and the plugin zips `TZ=UTC zip -X`, and this leg was missed; it would have gone out with the first tag. `tools/normalize_zip.py` (tested in `tools/tests/test_normalize_zip.py`: against a zip built the way CI built it, in several zones; permissions, order and contents survive; the result does not depend on the zone) rewrites a zip with every entry stamped with the commit's time in UTC and no extra fields, and `--check` fails when any entry still carries the field. The Linux leg runs both, the plugin job runs `--check`, and `docs/BUILDING.md` / `CLAUDE.md` now say a hand-built zip (macOS, CoreELEC, Android) takes the same two commands before upload. The pre-release sweep also tidied wording in the workflow's comments and a few documents (the README no longer says the workflow attaches release zips: they are published by hand).

#### Links to the public GitHub repo still name the master branch

**Fixed 2026-10-06 (opened 2026-10-05, when the main branch was renamed from `master` to `Omega`).** Four links pointed at `.../tree/master/...` or `.../blob/master/...` on GitHub: `help_url` in both plugins' `plugin.json` and `plugin.py` and the SECURITY.md link in `.github/ISSUE_TEMPLATE/bug_report.yml`. They now name `Omega`, so the public repo is right from its first day (`timeshift_buffer` 0.8.13 and `recording_edl` 0.2.3, one CHANGELOG entry each). The other places that name the GitHub repo (`PKG_SITE` and the `PKG_URL` tag archive in `packaging/coreelec/pvr.dispatcharr-unofficial/package.mk`, the repository link in `addon.xml.in`, the releases links in the READMEs, `SECURITY.md`) name it by its address and stay right as long as the repo keeps its name; the archive URL names no tag until the next release, and its checksum is still the all-zeros placeholder. `.github/dependabot.yml` is deleted (the pins are moved by hand, `tools/tests/test_dependency_pins.py` still checks they are SHAs), and the docs now say how a release's zips reach the GitHub release page by hand (`CLAUDE.md`'s "Publishing a release's assets" bullet, `docs/BUILDING.md`'s Distribution section).

#### Review of the sixteenth sweep's own changes: an end-offset regression, an override cleared every summer, and smaller findings

**Fixed 2026-10-06 (a narrow review of the branch's last changes by two Opus agents; the first finding was reproduced by simulating Kodi's real timer conversion against the real `RecurringRuleUtil.cpp`, the second and third by simulation against the real `ServerOffsetCrossCheck.cpp`).** (1) **A regression of the sweep's own, reverted:** `ComputeRecurringRuleFields()` converted a new rule's end with the offset at the end's own instant, on the model that Kodi converts each typed time with that instant's offset. It does not (`docs/RECURRING_RULES.md`, "A rule typed across a clock change"): both times arrive with the same flag, so the old single offset was exact, and the change stored 01:30-03:30 as 01:30-04:30, drifted an hour on each no-op edit, and cut a fall-back rule short. The new tests had modelled the same idealised Kodi, so they passed; they now model Kodi's real conversion (stable over four show-and-save rounds for a spring-forward, a fall-back and an overnight rule). (2) **A legacy-zone override was cleared every summer:** on a server with pre-2026c tz data the current table and the server agree from March to November, so the samples said "agrees" and the override went, coming back (with a second warning and notification) each winter, and a rule with a winter first day or start date was an hour off all summer. An agreeing table now clears an override only when the agreement contradicts it (`ServerOffsetJudgement::legacyKind`, `agreeingTableOffsets`). (3) **The wrong mode could be adopted for most of 2026-11-01:** `JudgeServerOffset()` took anything but "the legacy table agrees" for a constant difference, including the legacy table having no evidence (every sample within a day of its own clock change); six daily rules were enough to hold a wrong "-60 where the table says -420" for about 18 hours, putting every summer date an hour off. No evidence from the legacy table is now no evidence. (4) A zone change with an override in force dropped that evaluation's judgement although the comment said it counted, and the log line said the override was cleared "again" under the new zone's name. (5) `OnSystemWake()` bumped the channel generation but not the guide's, so a guide fetch running across a suspend committed as fresh. (6) `SCAN` can return a key twice (a keyspace resized between calls; this one is shared with Celery and the proxy), which counted one buffer twice against `max_concurrent_buffers`: deduplicated. (7) A hung TSan run (exit status 124 of `timeout`) was retried as if the runtime had refused the memory layout, three times until the job limit; it now fails at once. (8) The lint-scope documentation (`CONTRIBUTING.md`, `CLAUDE.md`) still gave a format command without `tests/glue/*.cpp`, which makes a contributor's glue change fail CI's lint job. Also: a test named for a later request body that tested none, a test that waited a fixed 0.2 s for handler threads to give their slots back (it polls now), the /64 grouping's shared quota noted in its docstring, and mutation survivors closed (a proposal equal to the override in force breaking another's streak, a 401 before a failed transfer in `ClassifyRecordingReadResponse()`, the exact half-minute rounding, all-negative float counts). Not changed, noted: the guide refetch can be postponed for as long as finished-refresh events keep arriving less than 330 s apart (a run of sources refreshing in sequence), bounded by the end of that run and not affecting the periodic fetch.

#### Several glue scenarios passed with the feature they name broken, UBSan never failed a run, and the Windows release step could attach an older build's zip

**Fixed 2026-10-06 (the sixteenth hardening sweep); the scenarios were proven by running them with the feature removed, the Windows step is from reading and depends on the runner keeping its temp directory between jobs (not checked: no runner access).** (1) ip play returned quietly when `OpenInProgressRecordingStream()` failed, so "in-progress recording" and "in-progress, 500 blip" reported ok (`RESULT fails=0`, `rc=0`) with the open hard-wired to fail; the `functional` scenario had no failure condition at all and passed with `GetEPGForChannel()` returning a server error on every channel (the `AddTimer` and `GetTimers` results were only logged); `surface` asserted nothing, not even that channels loaded; live play never checked that a byte was read, live tail close asserted nothing, and rec play counted failed reads without failing on them. Each now fails: an open that fails, no bytes from a live, completed or in-progress stream, a failed read of a completed recording with no fault injected, a Close slower than 8 s with a reader blocked at the tail (or leaving the stream open), no channels, a channel whose guide never loads or is empty or lists a duplicate id, any `AddTimer` that is not accepted, `GetTimers` failing or listing fewer than the four timers added. (2) The ASan variant ran UBSan without `-fno-sanitize-recover`, `run.sh` sets `halt_on_error=0`, and `ci.sh` read only `RESULT fails=0` and `rc=0`, so a UBSan report exited 0 (confirmed): `ci.sh` now fails a scenario whose output contains `runtime error:` (and reads the whole output, not the first 80 lines `run.sh` keeps by default). All 19 scenarios currently print none. `build.sh` ignored a failed compile (`xargs` still exited 0, and a stale object from an earlier build was linked in by hand runs): it now stops, and clears the objects first. (3) The Windows job runs that runs jobs directly on its machine on a machine that keeps `%TEMP%` between runs, and "Collect built zip" took the first `addon-pvr.dispatcharr-unofficial-*.zip` found there; the name carries the version and a name sort puts `0.11.0` before `0.12.0`, so the first tag after the next version bump could have attached the old build's zip to the new release with nothing checking. The build step now deletes the zips an earlier build left, and the collect step fails when it finds anything but exactly one (`docs/BUILDING.md` carries the same advice for a hand build). Also: `tools/check_doc_refs.py` now reads the workflow and the glue harness's files too (two heading citations in the workflow had been dangling since the headings were reworded when their items closed, unseen because only the docs, `src/` and the plugins were scanned), the two TSan and glue jobs have a 30 minute `timeout-minutes` and the TSan run a 10 minute limit per attempt, the lint job's clang-format check covers `tests/` and `tests/glue/` (both were already clean), the continuity-counter test fails instead of skipping when `REQUIRE_FFMPEG` is set and the ffmpeg cannot generate the stream (the outer skip was already closed that way), and `CLAUDE.md`, `CONTRIBUTING.md`, the bug-report template and `SECURITY.md` no longer say the glue harness is not in CI, that there is no automated test suite, or that two vulnerabilities were ever found; the user-facing text for the real-time setting (`README.md`, the setting's help, `docs/MANUAL_TESTING.md`) now says it also covers the guide and channel refreshes, and the README names the addon as Kodi does ("Dispatcharr PVR Client (Unofficial)"). `CHANGELOG.md`'s `[Unreleased]` section gained the eight user-visible fixes since 0.11.0 it was missing (login backoff, non-UTF-8 JSON, a long group name, the 1970 series rule, genre keywords, view-only accounts and guide links, onscreen episode numbers, the duplicate catch-up session).

#### Mutation survivors closed (sixteenth sweep)

**Closed 2026-10-06; about 300 hand mutations across 16 modules, the survivors below each now fail a test.** `TimeZoneUtil`: the nth-weekday arithmetic had no vector in a year whose March and November do not start on a Sunday (a missing wrap of the weekday difference passed every 2026 case); 2027 and 2028 transitions for a US and an EU zone were added. `RefreshEvents`: `progress >= 100.0` against 99.5 and 99.9, and each count field alone (only `channels_deleted: 0` must read as no change). `CurlCallbacks`: single-byte writes, `Content-Length:1234` with no space, the exact overflow boundary of `BoundedWriteCallback()` and the wrapping `used + totalBytes` form (told apart through `allocationFailed`: a guard that fires returns before any allocation is attempted). `TimerRequestBuilder`: the tvg_id of a series delete query was not URL-encoded in any test. `ManagedRecurringRule`: an occurrence that is neither upcoming nor running (stopped early, scheduled end still in the future) must not hold a rule back from renewal. `PaddingPush`: `HasPending()` for a post-only edit, giving up with a post edit or both parked, and the retry count of a re-parked edit. `Staleness`: the never-loaded sentinel is exactly zero, and a Kodi started within minutes of boot has real timestamps a few ticks from it. `RecordingHttpUtil`: a file swapped for an empty one (`Content-Range` total 0) is a change. `RecurringRuleEdit`: a baseline that lists a weekday twice. `RecurringRuleUtil`: the two refinements of the display offset resolve a start time inside a spring-forward gap to the offset before the gap (what Django's `make_aware` does), which holds only because the cap is even; a test now pins that. `LiveEdgeMargin`: `TrimToTrailingLiveEdgeMargin()` with a margin of 0 read one past the end (unreachable, the only caller passes 3; it now keeps nothing). `timeshift_buffer`: the body's absolute deadline could be removed without a failure, because the two existing tests were ended by the per-write timeout of a client that stalls (a new test uses a client that reads steadily but under the floor rate), and the manifest cache's recency (a reused channel kept its old slot). Not closed: `_DeadlineSocketReader` `remaining <= 0` against `< 0` and the refusal log's `<` against `<=`, equivalent in practice; the `parse_request` timeout restore, redundant now that `do_GET()` resets the socket timeout per chunk (left in as a second line of defence); `LiveEdgeMargin`'s `clampedToTail`/`clampedToHead` flags at exact equality, which only feed a log line.

#### An unauthenticated client could hold about 400 MB per worker in request headers

**Fixed 2026-10-06 in the timeshift_buffer plugin 0.8.12 (the sixteenth hardening sweep), reproduced against a scratch copy of the real `_BufferHTTPServer`.** The 10 s request deadline bounds how long a request may take, not how much it may send, and `http.server` accepts a 64 KiB request line and up to 100 header lines of 64 KiB each (about 6.5 MB per connection, held before the token is looked at). Each connection sent 99 header lines of 65 KB and stalled before the blank line: 20 connections took the process from 20 MB to 145 MB resident, 64 (the per-client cap) to 418 MB, and it stayed at 417 MB after the deadline dropped them. With 256 connections a worker, a few source addresses (or one host with an IPv6 /64, below) reach 1.6 GB per worker, needing about 42 MB/s of bandwidth per address. `_DeadlineSocketReader` now counts the bytes it returns while a request is being received and ends the request past a 16 KiB budget (a class attribute of the request handler; a real client sends under 1 KiB). Also closed from the same review: the per-client cap keyed on the full IPv6 address, so a host holding a /64 never reached it (`_client_key()` groups an IPv6 peer by its /64 and an IPv4-mapped address by its IPv4 address), `stop()` skipping `_stop_http_server()` when Redis was unreachable and the teardown before it raised (the listener of a deleted plugin stayed bound until a restart; now a `finally`), one buffer state that was not valid JSON or had no channel stopping every reaper tick, the orphan scrub and list/stop-all for up to its 10 minute TTL (skipped and logged once), `KEYS` on every reaper tick and under the start-slot lock (`SCAN`, with the glue Redis stand-in taught `SCAN` and `TIME`), the orphan check comparing an owner file stamped with the shared clock against the local one (stamped with the local clock now: with Redis on a host whose clock is S seconds ahead an untracked ffmpeg was stopped S seconds late), and the reaper trimming `ffmpeg.log` under the current `storage_path` setting rather than the buffer's own. Tests: a request exactly at the budget is served and one byte over is dropped, headers sent endlessly are cut off by size with the deadline set far away, the /64 grouping and the cap through the real `process_request()`, `stop()` with Redis raising, `SCAN` rather than `KEYS`, and the reaper's trim path. Not changed, noted: the recording_edl plugin's list_dvr_hls_staging_dirs action description still says it scans only `/data/recordings` (it also scans path-template roots; the README is right), left so as not to re-version that plugin for a text; and a teardown that loses ten compare-and-set races writes the caller's older copy of the state (it needs ten consecutive losses, so it could not be shown happening).

#### Regressions of the fifteenth sweep's request bounds: playback stalled during an authentication cooldown, and a reopened recording failed on a slow server

**Fixed 2026-10-06 (the sixteenth hardening sweep), from reading; each is a consequence of the bounds added the same week, and the first is the more likely to bite.** (1) `GetRecordingById()` reports HTTP status 0 both when the server did not answer and when authentication refused to try (the 30 s cooldown after a blip, up to 30 minutes after a rejected login), and the steady-state refresh read 0 as "the server is gone" and skipped the playlist fetch, which needs only the API key: a recording being watched at its tail stopped growing for as long as the cooldown ran, with the server fine. It now says when authentication did not complete (`authenticationNotCompletedOut`), and `ShouldSkipPlaylistAfterUnansweredLookup()` skips only for a lookup that reached the server and got nothing back. (2) `steadyState` was "has segments", which a reopened recording seeded from the segment cache already is, so a reopen got the 5 s bound on its authentication, lookup and playlist, and a failed first refresh was not retryable (`DecideInProgressColdStartStep(false, false, ...)` fails the open) where a first-ever open waits the full timeout for up to 45 s; the comment and the commit said a cold start keeps the configured timeout. `RefreshInProgressRecordingManifest()` now takes `coldStart` from the open's own loop (`IsInProgressSteadyState()`). (3) A short-bounded authentication that timed out armed the 30 s cooldown for every caller, so a channel switch whose Close authenticated at the 5 s bound failed the next channel's open at once with the old error, and the stream's own refresh (twice a second) won the next attempt each time the cooldown ended, starving the callers allowed the full timeout. The cooldown now remembers that a short-bounded attempt armed it, and a caller that may wait the whole connection timeout goes ahead; short-bounded callers are still turned away, and the startup check's deferral still stops everyone (`IsTransientCooldownBlocking()`), since it exists to keep Kodi's own threads off a dead server. Also closed, all from the same review: a seek that landed during a segment fetch (the live and in-progress read paths released the state lock for the transfer and then advanced the NEW position by the length of bytes read from the old one; both now return -1 for Kodi to retry), `CloseLiveTimeshiftStream()` deregistering the viewer only after the stop request had run, during which a manifest refresh naming it registered it again (the id is forgotten before the stop request), an in-progress open marking the stream open and seeding it from the cache in two lock scopes (a refresh from another thread in between merged the whole playlist and then the seed added the cached segments again; one scope now), the segment size probes (up to 16 at once) keeping the full timeout in steady state, and `~DispatcharrClient()` freeing the share objects with the persistent stream handles still attached (`curl_share_cleanup` then fails with `CURLSHE_IN_USE` and both leaked). The order-sensitive response checks of a recording read and a live segment fetch moved out of `ReadRecordingStream()`/`ReadLiveTimeshiftStreamOnce()` into `ClassifyRecordingReadResponse()` and `ClassifyLiveSegmentResponse()` (`RecordingHttpUtil.h`), where reordering any of them now fails a test (a mutation moving the 404 branch after the range check survived the whole suite before). Not done, noted: `ObtainApiKey()` on a 401 from the bounded playlist fetch still waits the full timeout, and the per-hop timeout of a redirect chain (`PerformWithSafeRedirects()`) can multiply a bound by the hops it follows (at most six), both rare paths.

#### The zone cross-check could be moved by an edited rule, looked at the past, and used one offset for every date

**Fixed 2026-10-06 (the sixteenth hardening sweep; two agents found the same cluster, and the late-detection case was reproduced with a scratch program against the real `ServerOffsetCrossCheck.cpp`/`TimeZoneUtil.cpp`).** The check added the same day (`ServerOffsetCrossCheck.h`) had five weaknesses, all in the case it was written for or next to it. (1) It took the six occurrences nearest to now in either direction, so occurrences from before a server's clock change (which agree with the table) outvoted the ones after it: on a pre-2026c server a weekly rule would have stayed on the table for about three weeks after 2026-11-01. It now takes the six soonest that have not started (`SelectOffsetSamples()`). (2) Nothing bounded how far a sample could be from the table, and a rule whose time had just been edited (or a stale snapshot pairing the new time with old occurrences) reads as a difference: a 90-minute edit read as a -150 minute zone and would have moved every rule and queued the warning. A sample implying more than an hour from the table (`kMaxPlausibleOffsetDeltaMinutes`) is now not evidence, and an override needs two evaluations in a row to propose the same thing (`UpdateServerOffsetTracker()`). (3) While overridden one constant was used for every instant: a rule dated in summer was shown an hour off, and one with a first day after the server's next clock change (March 2027) was stored an hour off. A disagreement the table with its recent rule change left out explains (`ComputeKnownZoneOffsetMinutes(..., applyZoneRuleChanges=false)`) is now followed instant by instant (`kLegacyZoneData`); any other is a delta applied only where the table gives the offset it was measured against. (4) The override was not reset when the zone setting changed, so a "no evidence" verdict left the previous zone's offset in force; the tracker remembers its zone and forgets everything on a change. (5) `ImpliedOffsetMinutes()` truncated toward zero, so an occurrence carrying 28 seconds gave 59 for a +60 offset and a false 59-minute override east of UTC; it rounds to the nearest minute. The warning's sign for a half-hour offset was also lost (`FormatUtcOffsetMinutes()`). The glue scenario now uses a fixed-offset zone and dates the rule and occurrence from the clock (an occurrence that has started is not a sample, so the old fixed dates would have failed from 2026-10-21 on), and fails if the override is adopted on a single evaluation. (A change to `ComputeRecurringRuleFields()` made in the same commit, giving a new rule's end the offset at its own instant, was a regression and was reverted: see the review entry below.) Not live-checked (no server disagrees with the table before 2026-11-01); the table's own transition formula had no test in a year whose March and November do not start on a Sunday (a missing weekday wrap would still pass), so 2027 and 2028 vectors were added.

#### A finished refresh could be lost: an overdue guide refetch kept its time, and a channel fetch in flight overwrote the aged mark

**Fixed 2026-10-06 (the sixteenth hardening sweep), from reading; the guide case is a window of one fetch's duration and the channel case likewise.** `ScheduleGuideRefetchForEvent()` kept an already scheduled fetch whatever its time, so a renumbering's follow-up that was due (or already running) when a finished-refresh event arrived fetched inside Dispatcharr's 300 s cache of the guide and cleared the schedule: the refresh was not seen until `epg_refresh_hours`. A fetch due sooner than the new event's own window now moves out to it (and a running fetch no longer matches the due time it saw, so it leaves it scheduled). The M3U event aged `m_channelsLoadedAt`, but a channel fetch already running committed with a fresh stamp, undoing it; channels now have a generation (`m_channelsGeneration`, bumped by the event and by `OnSystemWake()`) and a fetch that began before a bump commits what it read but is not called fresh. `CountField()` read a count with `get<long long>()` after `is_number()`, which for a float such as `1e300` is an out-of-range conversion (undefined behaviour on untrusted input); integers are read as integers and floats compared as doubles.

#### Changing a pinned series rule's channel kept the old EPG source, and the replacement likely matched nothing

**Fixed 2026-10-04 (the seventh hardening sweep), a regression of the sixth sweep's fix, from reading; the server side is inferred from `docs/RECORDINGS.md`'s v0.30.0 note and was not re-verified live.** The sixth sweep made `UpdateTimer()` echo the cached `epg_source_id` for any channel. The source is not channel-independent: Dispatcharr pins a rule to one source so its `tvg_id` resolves in that source's own guide data. A rule created by the Guide "Record series" button is (title, T1, source 5); changing it in Kodi to a channel fed by another source with `tvg_id` T2 sent (title, T2, source 5), created a new rule, then deleted the working one and purged its future recordings, and the new rule asked source 5 for a `tvg_id` it probably does not have: a rule that records nothing and no error. Before the echo a channel change sent source 0 and the rule kept recording (though it reset the hidden fields). `ResolveSeriesRuleSourceOnEdit()` (`TimerIdentity.h`) now echoes the pin only when the re-derived `tvg_id` equals the old one (the HD/SD case the echo was for, edited in place) and sends 0 otherwise; the old pinned rule is still deleted by its own source. Tests: the decision and the replace decision it feeds.

**Confirmed live the same day, against the server's own evaluator** (no Kodi needed: the question is what Dispatcharr does with the request the addon would send). The lab has two guide sources; a disposable channel was mapped to a record of the second one, and a programme that exists only in that source was used. A rule for that programme's `tvg_id` **pinned to the first source** -- what the sixth sweep's echo would have sent after a channel change -- evaluated to `scheduled: 0, status: no_epg_match`: the claim was right, such a rule records nothing and reports no error. The same rule **unpinned** (what the fix sends) evaluated to `scheduled: 3, status: ok`. The rules, the three recordings, and the channel were deleted afterwards (series rule, recordings and channel re-listed empty). One caveat: the lab has no `tvg_id` shared by two sources, so the case the pin exists for (the same `tvg_id` on both) was not exercised, only the wrong-source one.

#### An older padding push that failed after a newer one succeeded re-parked the stale value

**Fixed 2026-10-04 (the seventh hardening sweep), proven against the real `PaddingPush.h`.** Edits 1 (5) and 2 (7) pushed concurrently, edit 2 succeeds, edit 1 times out: `ApplyDirectPaddingPushResult()`'s failure branch parked 5, the next retry pushed it over 7, and if the follow-up retry of 7 failed three times the server stayed on 5 while Kodi showed 7. The failure branch now parks the newer successful value instead (its older request may also have reached the server late), and an older failure never replaces a newer parked one.

#### The file server's `Content-Length` could describe a different file than the body

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.6 (the seventh hardening sweep), proven by forcing the interleaving.** `do_GET()` took the size from a `stat()` of the path and read the body from a later `open()`; the muxer replaces `live.m3u8` by renaming a temp file over it, so a request in that window got one file's `Content-Length` and the other's (shorter) body: a truncated playlist. The addon reads the playlist through `get_live_manifest`, so only other HTTP consumers were affected; segments are never rewritten. The size now comes from `fstat()` of the open file. Tests: a stale stat result is ignored and the length matches the body; the vanished-file test now races the `open()`.

#### The ` [Kodi]` ownership tag could push a recurring-rule name past the server's 255-character limit

**Fixed 2026-10-04 (the seventh hardening sweep), from reading (the limit is `RecurringRecordingRule.name max_length=255`).** `AddTimer()` and a rename in `UpdateTimer()` sent `AddManagedRuleMarker(name)`, so a name of 249 to 255 characters that the server would have accepted failed with a 400 once tagged; adoption already skipped such a rule. `AddManagedRuleMarkerBounded()` cuts the display name to the characters that leave room for the tag, on a UTF-8 code-point boundary and counting characters rather than bytes; adoption keeps `AddManagedRuleMarker()` and skips a rule that would not fit rather than renaming the user's rule. Tests: the exact 248/249 boundary, an already-tagged name, and two-, three- and four-byte characters never split.

#### The start lock's release was a non-atomic GET then DEL

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.6 (the seventh hardening sweep), from reading.** If the 30 s lock expired between the two calls and the next caller took it, the DEL removed the next holder's lock (it needed a hold of about the whole TTL). The release is now one compare-and-delete script where Redis allows scripts, with the old two-step form as the fallback for a Redis that refuses them. Tests: a scripted fake (one script, no separate GET or DEL, never removes another holder's lock), the fallback, and a connection error propagating.

#### Mutation survivors closed (seventh sweep)

**2026-10-04.** 285 C++ and 59 Python mutations (33 survivors, 13 equivalent) gave these tests: the live manifest's in-place update of the newest segment's size (the "Packet corrupt" incident), a state with no access token never trusting the cache, a reused sequence under a different filename, `ended` for a state with no pid and the start-ticks check in both liveness calls, the first media sequence winning and filenames stripped, a cleared API key being a change rather than a stale re-delivery, a positive signed integer beyond `int` in `FieldOr`, `ParseRecordingFields` at `now == start`, `now == end` and an unparseable start, a channel with only `channel_num`, `days_of_week` with non-integers, the 1023/1024-byte group-name boundary, a locale-independent channel key under a comma-decimal global locale (a custom `numpunct`, so it needs no installed locale), a segment URI with no `#EXTINF`, an authority with two `@`, both catch-up uuids empty, and `ChannelGuideKey()`/`LocalTimeZoned()`, which no test referenced. Not changed: `HasChannelLineupChanged()` ignores `hasChannelNumber` (no visible effect, Kodi shows 0 either way), and each worker keeps a manifest-cache entry per channel it has served, bounded by the number of channels and never evicted.

#### Editing a series rule's channel reset its hidden fields and could leave a duplicate rule

**Fixed 2026-10-04 (the sixth hardening sweep), from reading.** `UpdateTimer()`'s series branch echoed the cached `title_mode`, `description`, `description_mode`, `untagged_is_new` and `epg_source_id` back only when the timer's channel matched the cached rule's. The series timer type offers channels and "any channel", so changing the channel in Kodi's dialog (including "any" to a specific channel) took the other branch and sent all five empty -- the bug the 2026-09-29 fix closed, reopened on this path: a `contains` rule became `exact` and stopped matching. A rule pinned to an EPG source (what Dispatcharr's own Guide "Record series" button creates whenever the channel has guide data) also lost its pin, so when the new channel resolved to the same `tvg_id` (HD and SD variants of one channel) the upsert key (title, tvg_id, no source) missed the pinned rule and created a second one; `ShouldReplaceSeriesRuleOnEdit()` compared only title and `tvg_id`, so the old rule was never deleted and both recorded. The rule's own five fields are now echoed whatever the channel (only `tvg_id` is re-derived), which makes the upsert edit the pinned rule in place; `ShouldReplaceSeriesRuleOnEdit()` also has a source-aware form that replaces a pinned rule whose source changed (never an unsourced one for a source alone, since an unscoped delete would take the new rule too). The comment that justified "title and `tvg_id` only" had claimed the source was echoed on every edit, which was not true on that branch. Tests: the replace decision with sources (a source-only difference, unpinned, a negative sentinel, title and `tvg_id` still deciding); the echo itself is glue in `UpdateTimer()` and is not unit-tested.

#### `GetRecordingById()` accepted any 2xx body, including an empty one, as a successful lookup

**Fixed 2026-10-04 (the sixth hardening sweep), from reading.** `Request()` turns an empty 2xx body into `{}`, and `ParseRecordingFields({})` is id 0 with neither upcoming nor in-progress, so `DecideDeleteTimerAction()` returned `kAlreadyDone` and `DeleteTimer()` reported success without deleting a scheduled recording; `UpdateTimer()` reduced an edit to a rename and `IsInProgressContentGone()` read the stream as finished. It needs a misbehaving proxy or server. `IsRecordingResponseFor()` (`RecordingParser.h`) requires an object whose integer `id` is the requested id; anything else is a failed lookup, which every caller already handles as unknown (and `DeleteTimer()` as `kRefuseUnverified`). Tests: an exact match, another id, `{}`, a list, `null`, a string, `null` and fractional ids, and id 0.

#### `FieldOr<int>` and `FieldOr<int64_t>` silently wrapped out-of-range JSON integers

**Fixed 2026-10-04 (the sixth hardening sweep), proven under UBSan.** Only the floating-point path was range-checked; nlohmann's integral `get<>()` is a plain cast, so `{"id":4294967301}` read as the int 5, an unsigned `18446744073709551615` as the int64 -1, and an EDL `"type":4294967297` as 1, past the 0-3 coercion. Ids that size cannot come from Django's int32 keys, so it needed a hostile server. Signed and unsigned integers outside the target type's range now read as the default, like the float case, and every in-range value (both ends of each width) still reads as before. Tests: wrap cases for `int`, `int64_t`, `uint32_t`, `uint64_t` and the exact boundaries.

#### The series-rule delete fallback still deleted across every EPG source

**Fixed 2026-10-04 (the sixth hardening sweep), from reading.** `DeleteTimer()`'s fallback for a rule missing from the loaded cache re-derived title and `tvg_id` from Kodi's own copy and sent no `epg_source_id`, so Dispatcharr removed every source's copy of a rule sharing that title and `tvg_id` -- the data loss the cached path closed on 2026-09-29. A rule that is not in the loaded list is either already gone or the load failed, so nothing is deleted any more: the call reports "the series rule was not found on the server" and Kodi drops the rule at its next timer refresh if the server really no longer has it. Not unit-tested (glue); the dead channel-derived lookup and its network call were removed.

#### Mutation survivors closed (sixth sweep)

**2026-10-04.** 136 C++ and 68 Python mutations, of which 64 and 22 survived, gave these tests: an empty dotted leaf (a fresh `.dvr_<id>_hls`, an empty `.timeshift`) in `_is_under_dotted_dir()` and the empty-directory prune (the 2026-09-05 incident class), `stop_buffer` pruning a phantom viewer, the stale-config teardown not retaking the start lock it holds (it would otherwise leak the old ffmpeg), the per-chunk write timeout in the body deadline (observed through the handler thread ending), the reaper's recovery reset, `-nostdin`, `_parse_edl` with a single-token line and a float type, a recording whose `_hls_dir` is another directory, strided case folding and an overlong UTF-8 encoding in `UnicodeText`, every table zone's January and July offset, the recurring-rule "today" default in Dispatcharr's calendar, `ResolveSeekPosition()` with an unknown length, two `xmltv_ns` entries (the first wins), a keyword first seen mid-word then at a word start, the 125/126-byte control-frame length boundary, and `ComputeSeriesRuleClientIndex()`'s flag bits. Still open: the three date parsers' use of the saturating form on a 64-bit CI, and the continuity-counter test's inner `pytest.skip` (an ffmpeg built without libx264 or aac) still skips silently under `REQUIRE_FFMPEG=1`.

#### A blocking `Read()` at the tail had no wall-clock limit during a server outage

**Fixed 2026-10-04 (the fifth hardening sweep), from reading; the budget arithmetic is unit-tested.** `ReadInProgressRecordingStream()` and `ReadLiveTimeshiftStreamOnce()` wait at the tail in a loop bounded only by an attempt count (`ComputeCatchUpAttempts()`: 49 at a 4 s segment estimate, 721 at the 60 s cap), and each attempt makes a forced refresh that can itself take a whole request timeout (30 s by default; an in-progress recording makes two requests per attempt, and no connect timeout is set). With the server unreachable in a way that times out rather than refuses (a routed host gone, a VPN drop, a firewall DROP) one `Read()` could block for about 49 x 60 s -- and Kodi's Stop does not take effect until a blocking read returns (the unprobeable-segment entry measured 4 min 24 s for that case), so Stop and likely exit hung that long. Both loops now also stop at a wall-clock budget, `ComputeCatchUpWallClockBudgetMs()` (`CatchUpUtil.h`): twice the attempts' own sleep time (a 4 s segment: 24.5 s against 12.25 s of sleeping, which leaves room for the refreshes' round trips) capped at 120 s, checked after each refresh. The attempt in flight when the budget expires still finishes, so the worst case is the budget plus one attempt's own requests. Tests: the budget for a 4 s segment, a seek probe, the 60 s estimate cap, never negative or overflowing, and the exact `>=` boundary.

**Measured live the same day** against a real Kodi client playing an in-progress recording through a forwarder that, on a flag, accepted every request and never answered it (what a firewall DROP does): playback was advancing at the live edge, the server was blackholed, and Stop was requested 12 s later. Every request then timed out at the addon's 30 s limit, in sequence (the recording lookup, the playlist fetch, a keep-alive HEAD, the next lookup), and the player was gone 140.7 s after Stop was requested (154.8 s after the blackhole began). Before the budget the same read could make 49 such attempts, each two sequential 30 s timeouts: over 40 minutes. A control run with a responsive server logged "catch-up loop used 4/49 attempts, 5.554s ... caught up", so ordinary tail waits are unaffected. What remains is the open item below: one refresh is itself two to three sequential timeouts and Stop waits for the one in flight.

#### `get_edl` read whatever file `custom_properties` named, whole and unbounded

**Fixed 2026-10-04 in `recording_edl` 0.2.2 (the fifth hardening sweep), from reading.** `_edl_path_for()` only checked that `comskip.edl` was a bare filename and joined it to `Path(file_path).parent`; `file_path` was not confined to any directory, the target was not required to be a regular file or end in `.edl`, and `read_text()` had no size cap. A recording whose stored data named a device directory with `edl` set to `zero`, or whose `edl` named its own multi-GB video, made the Dispatcharr worker read it into memory until it was killed when an admin client played that recording. The docs record that a DVR-manage (non-admin) account can write a recording's `custom_properties`, so a lower-privileged account could have triggered it; not verified live. The path must now end in `.edl` (case-insensitive), resolve (symlinks and `..` included) inside a DVR directory (`_dvr_hls_staging_scan_roots()`, which includes the recordings root; if the path templates cannot be read it falls back to the default recordings root alone, so the read is confined to that rather than refused -- corrected by the sixth sweep, which found the write-up had claimed a refusal), be a regular file (opened non-blocking so a FIFO cannot hang the worker) and hold at most 1 MiB (`_MAX_EDL_BYTES`). Tests: name filtering, the root check against a symlink escape, `..` and a name-prefix sibling, the regular-file and size checks (a directory, a FIFO, `/dev/zero`, exactly at and one past the cap), and the `get_edl` action refusing each case with an ok-and-empty response.

#### `_teardown_buffer()` on an absent state could destroy a buffer started in the meantime

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.5 (the fifth hardening sweep), proven by a reproducer.** When Redis held no state for the channel the teardown deliberately went on (stop the recorded ffmpeg, remove the directory, delete the state), but it did not take the channel's start lock (the scrub does since 0.8.4), so a `start_buffer` that completed after the teardown's read lost its new directory and Redis state; the owner file went with the directory, so a surviving ffmpeg was no longer findable. Reachable through the reaper (state read at the top of its tick), the `get_live_manifest` fatal path, and two `stop_buffer` calls overlapping a re-tune. `_teardown_untracked_buffer()` now stops the recorded pid, then removes the files and state only under the start lock and only if no different buffer has been tracked since (skipping when a start is in progress). `_start_buffer_locked()`'s own stale-config teardown passes `holds_start_lock=True` rather than take the lock twice. Tests: the reproducer, a held lock, and the lock-holding caller.

#### Chunked streaming turned the 60 s body bound into 60 s per 256 KiB chunk

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.5 (the fifth hardening sweep), proven.** Each chunk is a `sendall()` bounded by the handler timeout, so a client draining nearly that slowly per chunk held a thread, a connection slot and an open (possibly already deleted) segment for size/256 KiB times 60 s: with the timeout at 1 s and a 2 MiB segment, a client reading about one chunk every 0.6 s received all of it where 0.8.3 cut it off. A body now has one absolute deadline, the per-operation timeout plus the time the whole body needs at `body_min_rate_bytes_per_second` (32 KiB/s), and each write's timeout is what remains of it. Test: a slow drain against a raised floor rate is cut off, and fails with the deadline removed; the large-segment fast-reader test still gets everything. The handler comment that said `sendall()` bounds the whole send is corrected.

#### The reaper kept a Redis client taken once at thread start, and logged every failure

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.5 (the fifth hardening sweep), from reading.** `_reaper_loop()` fetched its client before its `try`, so a `None` there (the client may be `None` when Redis connects fail at start; not checked against upstream) or a connection that later died failed every tick for the life of the worker, and since the thread stayed alive nothing restarted it: no reaping or scrubbing from that worker until Dispatcharr restarted. Any Redis outage also logged a full traceback every 15 s per worker. The client is fetched inside the `try` each tick (a missing one is an error, not a crash), a repeating failure is logged with its traceback at most every 5 minutes (`_should_log_reaper_error()`) with a count, and recovery is reported. Tests: a `None` client, then an exception, then a working one; five failing ticks produce one traceback and one recovery line; the interval boundary.

#### `_delete_orphaned_dvr_hls_dirs()` acted on a classification built earlier

**Fixed 2026-10-04 in `recording_edl` 0.2.2 (the fifth hardening sweep), from reading.** The list is classified first and removed from later, and removing a large directory is not instant; a recording id reused after a database reset could have gone from orphaned to live in between. Each directory is re-classified immediately before its removal and skipped unless it is still orphaned. Test: one that became live, one that vanished and one still orphaned in the same stale list.

#### Mutation survivors closed (fifth sweep)

**2026-10-04.** 66 C++ and 35 Python mutations of the last two sweeps' code and the older high-risk modules left these survivors, each now an exact-boundary test: `CountMergeableSegments()` after a give-up, the unprobeable tracker's fifth failure at exactly 30 s, `TrimToTrailingLiveEdgeMargin()` at exactly the margin, `FindEpgEntryIndexByStartTime()` at the tolerance and the equidistant tie, `ResolveEpgOverlapWindow()` with an empty programme window, `TruncateRuleTimesToWholeMinutes()` on an equal start and end, `ShouldRenewRecurringRule()`/`ShouldExtendRecurringRuleEndDateOnUpdate()` at the half-window, `HasActiveOrImminentOccurrence()` at the margin, `EvaluateInitialAdoption()` at an end date of exactly now, `IsManagedRuleName(" [Kodi]")`, `_is_servable_name()` (`live.m3u8.tmp`, `seg_.ts`, `seg_1.ts.bak`), `_proc_start_ticks_from_stat()` with one field too few, the grace, idle and minimum-age comparisons, `segments_kept` with a falsy value, a same-stem directory as an apparent owning recording, and a plain file or a symlink named like a staging directory. Still open: the three date parsers' use of the saturating form on a 64-bit CI.

#### The orphan scrub could stop a freshly started, tracked buffer and delete its directory

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.4 (the fourth hardening sweep), proven with a reproducer.** The scrub built its whole orphan list first and then worked through it, and each untracked ffmpeg it stopped took up to two seconds (the SIGTERM poll). It never re-checked Redis for a later entry and never took that channel's start lock, so a `start_buffer` for a later channel could finish in the gap (it reaps the old ffmpeg, clears the directory, spawns a new one, writes an owner file and the Redis state) and the scrub, reaching that entry, read the new owner file, stopped the new ffmpeg and removed its directory under a playing viewer. It bites in exactly the lost-Redis case the owner-file fix is for: every buffer is an orphan at once, and with many of them the tick could outlive the 30 s leader TTL so a second worker scrubbed too. The `rmtree` half existed before; the new stop made the window seconds wide and added the process kill. `_scrub_orphaned_dirs()` now takes each entry's start lock (skipping a channel whose start is in progress), re-evaluates `_is_untracked_orphan()` (UUID name, no Redis state, age or live owner) under it, and releases it in a `finally`. Tests: the reproducer (a start finishing during the first stop; the new pid is never stopped and its directory survives -- and the test fails with the re-check removed), the lock taken and released per entry, a held lock skipped, the lock released when removal fails.

#### The series-rule bound still allowed seconds of matching per `GetTimers()`

**Fixed 2026-10-04 (the fourth hardening sweep), proven by timing.** The 4096-byte bound kept one `MatchSeriesText()` call to about 3 ms, but the AND/OR loop re-sliced and re-scanned its remainder per operator (quadratic), and the match is re-run for every recording of every rule on Kodi's thread: one channel-less rule with a 4 KB `a AND a AND ...` title against 300 upcoming recordings took 718 ms per `MatchRecordingsToSeriesRules()` call on fast x86 (a same-sized description doubles it, each further such rule adds as much, and an ARM box is several times slower). The scan is now one forward pass: 300 calls take 16 ms. Checked identical to the old implementation on 200,000 random queries over both text modes (the 110-case curated table is unchanged too).

#### The file server served any file in a channel directory and read it whole into memory

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.4 (the fourth hardening sweep), from reading.** With a valid token `/<uuid>/ffmpeg.log` (unbounded, see the open item) and `ffmpeg.owner.json` were served as well, and `do_GET()` read the whole file into memory before sending it, so a token holder asking for a large log from many connections at once (64 per address) could run the worker out of memory. Only `live.m3u8` and `seg_<digits>.ts` are served now, and the body is written in 256 KiB chunks from the open file with the declared length taken from `stat()`. Tests: every other name is a 404 for GET and HEAD, a ranged request is streamed from the right offset, and the 16 MiB slow-reader test now also goes through the chunked path.

#### The 32-bit saturation left the last year before 2038 unsaturated

**Fixed 2026-10-04 (the fourth hardening sweep), from reading.** `SaturatingTimeGm()` clamped only a `-1` result, so on a 32-bit `time_t` a real date between 2037-01-18 and 2038-01-19 came back as-is, larger than `kLatestRepresentableTime`: it sorted above a saturated 2099 and the callers' `endDate + 86400 - offset` on it could still overflow. It now also clamps anything above the ceiling (`latest` is a parameter so the test stands in for a 32-bit `time_t`). Related, on 64-bit: `static_cast<int>(end - start)` for a recording's duration wrapped negative for a row ending past about 2094; `ClampedDurationSeconds()` (`RecordingParser.h`) clamps to `INT_MAX` and is used at all three sites. Still unguarded on a 64-bit CI: that the three date parsers actually call the saturating form (a revert of any of them survives the suite); closing that needs a 32-bit test job, see below.

#### A read refusing an off-server segment returned -1, and the destructor comment was stale

**Fixed 2026-10-04 (the fourth hardening sweep), from reading.** `ReadInProgressRecordingStream()` returned `-1` when it refused a segment off the configured server, which Kodi retries at once; effectively unreachable (the probe refuses first and the segment becomes a zero-byte placeholder) but a silent busy loop if it ever happened. It now logs once and returns EOF. The destructor comment that said the delayed-refresh thread sleeps up to five seconds during the join is corrected.

#### Owner-based reaping and instances that share storage

**Closed 2026-10-04 by documenting it (the fourth hardening sweep), from reading.** Two Dispatcharr instances sharing a `storage_path` and a PID namespace (`--pid=host`, or two in one container) each see the other's live buffer as untracked in their own Redis, and after 300 s one stops the other's ffmpeg and deletes its directory; before 0.8.3 the directory's fresh mtime protected it. Separate containers are unaffected (each `/proc` cannot see the other's ffmpeg), and pid recycling is covered by the start-ticks check. A shared storage path is not a supported setup, so the plugin README now says so rather than the owner file growing an instance id.

#### Mutation survivors closed

**2026-10-04 (the fourth hardening sweep).** 75 C++ and 20 Python mutations of the earlier sweeps' code found these survivors, now with tests: a per-client slot leaked by a global refusal (the cap's refusal must give the slot back), `_live_owner()` dropping the recorded start ticks (a reused pid must not count as the owner), `tm_hour > 24` in `ParseXmlTvTime()` (hour 24), the exact 4096/4097 query bound, and `IsSameOrigin()` ignoring user info on one side. Left: the three date parsers' use of the saturating form on a 64-bit CI, and a stop while `WebSocketClient::SendAll()` waits on a full send buffer.

#### A buffer whose Redis state was lost kept its ffmpeg running, and the next start added a second

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.3 (the third hardening sweep), demonstrated against the real scan.** A Redis restart or flush while Dispatcharr and its ffmpeg children kept running removed every buffer's state key. The reaper only walks Redis-tracked buffers and the orphan scrub treated "directory idle for 300 s" as orphaned, but a running hls muxer adds and deletes a segment every few seconds, so the directory never looked idle (an untracked directory backdated an hour was reported as an orphan; after one simulated segment write it was not). The ffmpeg kept its provider slot and proxy connection, and the next `start_buffer` found no state, cleared the directory and spawned a second ffmpeg into it -- the two-writers corruption the 0.6.4 start lock was meant to rule out. `_start_ffmpeg()` now writes `ffmpeg.owner.json` (pid, `/proc` start ticks, start time) into the channel directory; `_find_orphaned_channel_dirs()` measures an untracked directory's age from the owner's start time while that process is alive, `_scrub_orphaned_dirs()` stops it before removing the directory, and a fresh start stops a live untracked one before `_clear_channel_dir()`. A file with no recorded start time is never acted on (a recycled pid could not be told from the ffmpeg), and `_stop_ffmpeg()` still refuses a pid whose start time changed. Buffers started by an older plugin version have no owner file and stay as recoverable as before.

**Confirmed live the same day**, with the buffer's Redis key deleted under a running ffmpeg by a throwaway plugin (a one-action probe that deleted that one key and listed the channel directory; imported, enabled, then deleted after the check). On 0.8.1: state gone, the ffmpeg still attached to the channel in Dispatcharr's proxy status (client count 1) for the whole 8.5-minute watch, well past the scrub's 300 s age, the directory present with a live segment writer and no owner file. On 0.8.3: a buffer started by a worker running the new code wrote `ffmpeg.owner.json` naming the ffmpeg; with its state deleted it was untouched by the automatic reaper for the 10-minute watch, because the lab's reaper leader was a stale copy of the old plugin (a plugin reload does not stop an older worker's reaper thread, the leak 0.6.7 closed for later reloads; a Dispatcharr restart clears it); one `scrub_orphaned_buffers` call that landed on a worker with the new code then reported the directory removed and the channel's proxy client count fell to 0, i.e. the ffmpeg was stopped, not just its files.

#### A large segment read slowly was cut off after about ten seconds (a 0.8.1 regression)

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.3 (the third hardening sweep), proven.** `_DeadlineSocketReader` sets the socket's timeout to what is left of the request deadline for each header read, and since Python 3.5 that timeout bounds a whole `sendall()`; `parse_request()` cleared the deadline but never restored `timeout`, so a response written to a slow reader inherited the seconds the headers had left. A 32 MiB segment with a one-second deadline and a client that waited three seconds before reading stopped after about 2.6 MiB (the 0.8.0 plugin delivered all of it). In practice a whole-segment client on a slow link or one that paused reading was cut off after about ten seconds instead of sixty; the addon's own small ranged reads mostly fit in socket buffers. `parse_request()` now restores `connection.settimeout(self.timeout)`. The test written for the 0.8.1 change served 1 KB and passed with that line deleted; it now serves 16 MiB to a client that waits past the deadline and fails without the restore.

#### A 401 during recurring-rule adoption was recorded as a permanent refusal

**Fixed 2026-10-04 (the third hardening sweep), from reading.** `Request()` reports the first attempt's 401 even when the re-login it triggered then failed (login backoff, a 429 from the login rate limiter, a 5xx from the login endpoint), and `ClassifyAdoptionPatchFailure()` read every 4xx but 408/429 as permanent, so one authentication hiccup during the one-time pass dropped every pending rule from the pending set and, once it was empty, persisted `done`: those rules were never tagged, never renewed, and died after their window. 401 is now retry-later (still bounded by `kMaxAdoptionRetriesPerSession`); a 403 stays permanent.

#### Dates from 2038 on parsed to -1 on 32-bit `time_t` builds

**Fixed 2026-10-04 (the third hardening sweep), confirmed by reading; `time_t` width confirmed by compiling a `static_assert(sizeof(time_t) == 4)` with the CoreELEC toolchain and the package's own flags (no `_TIME_BITS=64`).** `PortableTimeGm()` is `timegm()`, which returns -1 on overflow, so on CoreELEC armhf (the primary target) and 32-bit Android every date from 2038-01-19 on read as -1. A rule created in Dispatcharr's own UI with an `end_date` of 2099-12-31 then looked like a rule with no end date: adoption (and any Kodi edit of the rule, tagged or not) replaced it with a 30-day window, and `ShouldRenewRecurringRule()` read it as expired. `SaturatingTimeGm()`/`PortableTimeGmSaturating()` (`TimeUtil.h`) return `kLatestRepresentableTime` (the largest `time_t` less a year, so the callers' day arithmetic cannot overflow) for a year after 1970 that `timegm()` cannot represent, and `TimeFromIso()`, `TimeFromDateString()` and `ParseXmlTvTime()` use it. Tests inject a 32-bit `timegm` stand-in, since CI is 64-bit only. CoreELEC could additionally be built with `-D_TIME_BITS=64`, which needs Kodi built the same way; not done.

#### Thread creation could still throw onto Kodi's threads

**Fixed 2026-10-04 (the third hardening sweep), from reading.** The sibling of the probe-burst fix: `TrackDetachedThread()` (run from `AddTimer()` and `OnAddonSettingChanged()` on Kodi's threads) built its `std::thread` unguarded, and an insertion into the thread list that ran out of memory after the thread existed destroyed a joinable thread (`std::terminate`); the three worker threads created in the constructor could leave an earlier one joinable if a later creation threw. Room is now reserved before the thread is created, a failed creation is logged, the DVR-padding push runs inline instead (it must not be lost) and `AddTimer()`'s delayed refresh is dropped (the next periodic refresh covers it), and each worker's creation is caught and logged, leaving that feature off for the session. Not unit-tested (threading glue).

#### A 64 MiB segment buffer could still run a 32-bit box out of memory on Kodi's read thread

**Fixed 2026-10-04 (the third hardening sweep), from reading.** `ReadInProgressRecordingStream()` zero-fills a buffer of the segment's probed size on Kodi's read thread, clamped to 64 MiB, which the sweep that lowered the three response ceilings for 32-bit builds did not touch, and an escaped `bad_alloc` ended the process. The clamp is now 32 MiB on a 32-bit address space and an allocation failure ends the stream (EOF; `-1` would be retried at once) with an error line.

#### One client could hold every connection slot of the timeshift file server

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.3 (the third hardening sweep), from reading.** The 256-connection cap added in 0.8.1 can be held by any peer, since opening a connection needs no token: about 26 silent connections a second keep every slot full for the 10 s deadline, and each refusal logged a warning. A per-address cap (64) now leaves room for the real viewers (several behind one NAT share an address, hence generous), a refusal is reported at most every 30 seconds with a count, and the refusal paths give back both slot kinds (tested, including a thread start that fails).

#### The padding-push bookkeeping did not handle two direct pushes finishing out of order

**Fixed 2026-10-04 (the third hardening sweep), found by mutation testing: the mutation "always overwrite the newest-success record" survived the suite.** Edit 1 (5) and edit 2 (7) pushed concurrently, edit 2's response arriving first: edit 1's request may have been the last to reach the server, leaving 5 there while Kodi shows 7. `ApplyDirectPaddingPushResult()` now parks the newer value again when an older edit's success arrives after a newer one, the same correction `ApplyPaddingRetryResult()` already makes for a retry; resending a value the server already holds is harmless.

#### Test gaps closed

**2026-10-04 (the third hardening sweep).** The mutation pass re-applied 43 mutations to the earlier sweeps' fixes and 37 were caught; the six that were not now have tests: a stop during a TLS handshake the server never answers, `ClassifyAdoptionPatchFailure(401)`, out-of-order padding successes, the connection cap's slot release when a thread start fails, a port long enough to wrap `atoi` in `ParseUrlOrigin()`, and the large slow response above. One stays untested: a stop while `WebSocketClient::SendAll()` waits on a full send buffer (it is private and a pong is far too small to fill a buffer through the public API).

#### Stale claims corrected

**2026-10-04 (the third hardening sweep).** The 0.8.1 changelog entry and an OPEN_ITEMS entry said a late client gets a 408; the standard-library handler just closes the connection on a timeout (proven). Several comments and docs said the recurring-rule PATCH never sends `end_date`; adoption and an open-ended rule's edit do (`CLAUDE.md`, `RecurringRuleRenewal.h`, `PVRDispatcharr.cpp`, `docs/RECURRING_RULES.md`).

#### An API response with an integer beyond double range crashed Kodi

**Fixed 2026-10-04 (the second hardening sweep), proven against the vendored nlohmann 3.11.3.** `Request()` caught only `json::parse_error`, but the library throws `json::out_of_range` ("number overflow parsing") for an integer literal of about 1.8e308 or more, which Python's `json.dumps(10**400)` emits and a JSON field such as a recording's `custom_properties` can hold (a DVR-manage account can write it). Nothing above `Request()` caught anything, so on the recording-refresh thread (`RenewRecurringRules()` -> `GetRecordings()`, first thing at every start) it was `std::terminate` for every Kodi client of that server, on every start while the row existed; `std::bad_alloc` on a large body on a 32-bit box took the same path. `ParseJsonResponseBody()` (`JsonResponse.h`) catches every `std::exception` and reports a failed parse, and `RunGuardedWorker()` (`PVRDispatcharr.cpp`) now wraps each of the four long-lived thread bodies and `TrackDetachedThread()`'s work, so an exception is logged and the loop re-entered after a pause that watches the stop flag instead of ending the process. Tests: the helper against 400-digit, `1e400` and `-1e400` inputs. The thread guard is glue and not unit-tested.

#### An oversized or unparseable guide was re-downloaded every minute, forever

**Fixed 2026-10-04 (the second hardening sweep), proven by replaying the real gate for an hour (61 full downloads).** Only a 4xx counted toward the escalating guide-retry backoff. A guide past the size ceiling is aborted before any status is kept (0), and a parse failure (including the out-of-memory one turned into a failed refresh the same day) never touched the count, so both retried every minute: on a 32-bit box a quarter-gigabyte download and a parse attempt each minute for a guide that never loads. `ShouldCountTowardEpgFailureBackoff()` gained `responseUnusable`, `GetXmlTvGuide()` reports it for a size-exceeded or out-of-memory transfer, and a parse failure increments the count, so these now back off 1, 2, 4 ... 30 minutes like a rejection. `tests/test_staleness.cpp` replays the hour: 6 attempts for an unusable guide, still 61 for a plain outage. Not exercised live.

#### The series-rule text matcher recursed and re-sliced without bound

**Fixed 2026-10-04 (the second hardening sweep), proven with the real sources at -O2.** `ParseExpression()`/`ParseGrouped()` recurse once per parenthesised group and the AND/OR loop re-slices its remainder at every operator: about 25,000 `(a)` groups (75 KB) overran an 8 MB stack and about 3,000 (9 KB) a 1 MB one, and a 600 KB `a AND a AND ...` took 48 s, per rule and recording pair, on Kodi's thread in `GetTimers()`. A rule made in Dispatcharr's own UI or API has no length limit, so one runaway title crashed or hung every client. `MatchSeriesText()` now returns `kUnsupported` (rule left unlinked, like a regex) for a contains/search query over 4096 bytes or with more than 64 `(` outside quoted phrases (`kMaxSeriesQueryBytes`, `kMaxSeriesQueryGroups`); the upstream parser fails at a few hundred nested groups anyway. Tests: 100,000 groups, exactly at and one past the bound, quoted parentheses not counted, a query near the byte bound under two seconds.

#### A failed thread creation in the in-progress probe burst aborted Kodi

**Fixed 2026-10-04 (the second hardening sweep), from reading plus a test that makes creation fail on demand.** `RefreshInProgressRecordingManifest()` built each batch of up to 16 probe threads with `emplace_back()`, and a thread that cannot be created (EAGAIN under a thread or memory limit, plausible on low-RAM CoreELEC or Android) throws `std::system_error` out of it; the unwinding vector then destroyed the already-started threads while still joinable, which is `std::terminate`. `RunInBoundedBatches()` (`BoundedParallel.h`) joins what did start and runs that index and the rest of the batch on the calling thread, and swallows an exception out of a task. Tests: a creation failure at the first, second, middle and last position, no thread creatable at all, and a throwing task.

#### The timeshift plugin put a long-lived access token on ffmpeg's command line

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.2 (the second hardening sweep), from reading.** `_stream_attribution_headers()` mints an access JWT for the requesting account (often an admin) and the buffer's ffmpeg receives it as `-headers "Authorization: Bearer ..."`, which `/proc/<pid>/cmdline` shows to every local user (and any process in the container) for the token's whole default lifetime. ffmpeg authenticates once, when it connects (no `-reconnect` is set), so the token is now given a 120 s lifetime (`_STREAM_TOKEN_LIFETIME_SECONDS`, `set_exp`). The residual exposure for those two minutes is the command-line mechanism itself; an attribution channel that is not argv would remove it. The username/JWT branch had no test; fake `django.contrib.auth`/`rest_framework_simplejwt.tokens` modules now cover it (one `Authorization` line, the lifetime, ordering with `X-Real-IP`, a failed lookup or mint streaming anonymously with the IP kept). Not exercised live.

#### Exiting Kodi shortly after adding a timer waited out a five-second sleep

**Fixed 2026-10-04 (the second hardening sweep), from reading.** `AddTimer()`'s delayed refresh slept five seconds unconditionally and `~PVRDispatcharr()` joins it, so a Kodi exit or addon restart within five seconds of adding a timer was held for the rest of the sleep (against the project's shutdown-latency work) and then refreshed an instance being torn down. The wait is now `SleepUnlessShuttingDown()` on a condition variable the destructor signals first thing, and the thread returns without refreshing. Threading glue, not unit-tested.

#### `ParseXmlTvTime()` lacked the range checks `TimeFromIso()` has

**Fixed 2026-10-04 (the second hardening sweep), proven.** `"20261345256161 +0000"` came back as a time in February of the next year (`PortableTimeGm()` normalizes like `timegm()`) and an offset of `+9999` shifted a time by over four days. It now returns 0 for a month, day, hour, minute or second out of range (a leap second tolerated) and ignores an offset beyond +-23:59, like `TimeFromIso()`. Only Dispatcharr's own export feeds it, so the practical risk was low; the point is two parsers of the same kind of data agreeing.

#### The only guard on the hls-muxer continuity-counter fix was probably skipped in CI

**Fixed 2026-10-04 (the second hardening sweep).** `test_buffer_segments_keep_one_continuity_counter_across_files` skips when `ffmpeg` is absent, and the `unit-tests-python` job installed only pytest, with no known guarantee the runner image ships ffmpeg, so the one regression guard for the 0.8.0 "Packet corrupt" fix could silently never run. The job now installs ffmpeg, runs `pytest -rs`, and sets `REQUIRE_FFMPEG=1`, which turns the test's skip into a failure if the install ever stops working. The IPv6 dual-stack test (`_has_ipv6_loopback`) can still skip on a runner without IPv6 loopback; that one is a platform property rather than something CI can install.

#### The API key was attached to a segment URL on any host

**Fixed 2026-10-04, reproduced and re-verified live.** An in-progress recording's playlist can name absolute segment URLs, and `RebaseRecordingSegmentUrl()` only rewrites the ones under the recording's own `/api/channels/recordings/<id>/hls/` path; the size probe, the keep-alive HEAD and the segment read then attached `X-API-Key` to whatever host the URL named. `IsSameOrigin()` (`RedirectPolicy.h`, scheme, host and port, no userinfo) now gates all three through `DispatcharrClient::IsOnConfiguredServer()`: a segment on another origin is refused (probe returns unknown, keep-alive skipped, read fails), with one warning per session and debug lines after. A different port on the same host is a different origin and is refused too. Live, against a real Kodi client and an in-progress recording whose playlist was rewritten by an HTTP-aware proxy to name segments on a second proxy standing in for another host: the old build sent the key to the second host on every segment request, the new build sent none and logged the single warning. `docs/RECORDINGS.md`, here and `CLAUDE.md` had said the opposite and were corrected earlier the same day.

#### The timeshift file server could be held open by a client that drips bytes

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.1, reproduced live.** `_BufferRequestHandler.timeout` is a per-`recv` socket timeout, so one byte every few seconds kept a request line or header block open, holding a thread and a descriptor, before any token check. The handler now reads the request line and headers through `_DeadlineSocketReader` against one absolute 10 s deadline (`request_deadline_seconds`), closing the connection of a late client without a response (the standard-library handler does not send a 408 on a timeout), and the server caps concurrent connections at 256 (`max_connections`, a `BoundedSemaphore` in `process_request`). Live on the lab Dispatcharr with the plugin 0.8.0 loaded: a silent connection was closed at 65 s and a drip of one byte every 5 s was still open when the check stopped after more than 100 s. On a worker that had loaded 0.8.1 the dripping connection was closed at the deadline. **Caveat found doing it:** Dispatcharr runs several `SO_REUSEPORT` listeners and a plugin re-import only refreshes some of them, so the lab still had old-code listeners afterwards; a Dispatcharr restart is what puts every worker on the new code. `tests/test_http_handler.py` holds the unit-level drip, cap, slow-header and deadline tests.

#### The WebSocket connect and handshake ignored the shutdown abort

**Fixed 2026-10-04, reproduced and re-verified live.** `WebSocketClient` now takes a stop check (`SetStopCheck()`, wired to the realtime thread's own `shouldStop`), waits in slices of at most 500 ms in `Connect()`, `SendAll()` and the frame reads, and gives its transfer-progress callback the same check, so `~PVRDispatcharr()` no longer waits out the configured timeout. Live: with realtime updates on and the server's WebSocket upgrade held open by a proxy that never answered it, quitting Kodi took 30.1 s on the old build and 5.0 s on the new one. `tests/test_web_socket_client.cpp` covers the stop during connect, handshake, send and read.

#### A recurring-rule adoption PATCH that failed permanently was retried every cycle forever

**Fixed 2026-10-04, reproduced and re-verified live.** `ClassifyAdoptionPatchFailure()` (`ManagedRecurringRule.h`) reads a 4xx other than 408/429 as permanent for that rule (dropped from the pending set with one log line) and anything else as retry-later, counted per rule in `PVRDispatcharr::m_adoptionFailures` and abandoned for the session after `kMaxAdoptionRetriesPerSession` (5). Live: a rule whose name plus the tag exceeded the server's length limit, planted as pending adoption state, made the old build log an error every cycle and never finish; the new build logged it once, skipped the rule and completed adoption for the rest. The test rule was deleted afterwards.

#### A stale parked DVR-padding edit overwrote a newer successful one

**Fixed 2026-10-04 (the hardening sweep), proven with the real `PaddingPush.h` under ASan/UBSan.** A successful direct push cleared the parked value of that side only when the parked value *equalled* the one just sent. So with Dispatcharr unreachable the user saved pre-padding 5 (the push failed and 5 was parked), then saved 7 once it was back (the push succeeded) and the parked 5 survived: the next background cycle resent it, the server ended up on 5 and Kodi still showed 7. The existing test asserted the same value-only behaviour. Every edit now takes a sequence number when it is made (`BeginPaddingEdit()`), a success clears a parked value only when it is the same edit or an older one, a failure never replaces a newer parked value, and a retry that lands after a newer successful edit re-parks that newer value, so the server converges on the newest edit even when a slow retry races a direct push. `tests/test_padding_push.cpp` was rewritten around that (the 5-then-7 case, an older failure finishing last, the stale-retry correction). Not exercised live: the race needs Dispatcharr to go away and come back inside one settings session.

#### `std::bad_alloc` could escape the response write callbacks

**Fixed 2026-10-04, proven with `ulimit -v` against the real `CurlCallbacks.cpp`.** `BoundedWriteCallback()` grows a `std::string` by doubling, so near a ceiling it asks for one contiguous allocation of up to twice the body so far, and on a constrained or 32-bit address space (the CoreELEC userland on the ODROID N2+ is armhf) that threw out of a C callback long before the 512 MiB ceiling was reached (256 MiB with a 700 MB cap, 128 MiB with a 400 MB cap): `std::terminate()` for the whole Kodi process, the exact failure the ceilings were added for. The callbacks now catch the exception and fail the transfer (`BoundedStringSink::allocationFailed`, with its own error message at the three call sites), `XmlTvParser::Parse()` is wrapped the same way in `EnsureEpgLoaded()` so a guide too large to parse is a failed refresh rather than a crash on the background thread, and the ceilings are lower on a 32-bit build (64/256/32 MiB). Tests reach the catch path without exhausting memory by offering a chunk larger than a string can ever hold.

#### Guide `prev_days` ignored catch-up channels that report no depth

**Fixed 2026-10-04, proven with the real `EpgTagUtil.cpp`.** `ComputeGuidePrevDays()` took a catch-up depth of 0 ("the server did not say how far back it keeps programmes", which `IsWithinCatchupWindow()` already reads as the 7-day default) as 0 days, so an install whose catch-up channels report no depth never sent `prev_days` and the catch-up-on-a-fresh-install fix did not apply to it. A depth of zero or less now counts as the default window (7), still capped by the server's 30. The test that asserted `{0}` gave 0 was wrong and was replaced.

#### Channel-less series rules never linked to their recordings

**Fixed 2026-10-04 (the hardening sweep), from reading plus tests; this had been written up as "still open" inside the "Series rule edit with no channel_id" item, which was later closed on a different check, so the linking gap fell off the list.** `MatchRecordingsToSeriesRules()` required `rule.channelId == rec.channelId`, which a rule with no pinned channel (what Dispatcharr's own guide "Record series" button creates) can never satisfy: its timer showed "Any day at any time" and its occurrences stayed unparented one-time timers. `Recording::programTvgId` now carries `custom_properties.program.tvg_id` from the recording's own programme snapshot, and a channel-less rule matches on its `tvgId` (an empty one means any programme with that title on any channel). Two passes keep the more specific rule first: a rule pinned to the recording's channel is preferred over a channel-less one that also matches. Not verified live against a real channel-less rule's materialized recordings; the snapshot key is the one `docs/RECORDINGS.md` records. **Re-verified live the same day** against a real Kodi client and a rule with no pinned channel: the old build listed it with the Unix epoch as its start ("Any day at any time"), the new build with its real next occurrence.

#### The open-time Range probe downloaded a whole recording behind a Range-dropping proxy

**Fixed 2026-10-04, from reading plus the callback behaviour proven in a scratch test.** `OpenRecordingStream()`'s `Range: 0-0` probe used a one-byte `FixedBufferSink` that never aborts, so a server or proxy that dropped `Range` and answered 200 with the full body kept the transfer running until the 30 s timeout and a large recording then failed to open with "Timeout was reached". The probe now sets `CURLOPT_MAXFILESIZE_LARGE` (4 MiB: a ranged byte, a redirect or an HLS playlist all declare far less), and `CURLE_FILESIZE_EXCEEDED` is read as a response whose status and headers are what the open needs, with the length left unknown because a dropped `Range` means no `Content-Range` either. The heartbeat's sink was left alone: its small JSON reply would abort every time. A chunked body with no declared length is still bounded only by the timeout. **Re-verified live the same day:** a proxy that dropped `Range` on a multi-gigabyte recording made the old build fail to open it after 30.0 s ("Timeout was reached") and the new build open it in about one second. The follow-on gap (playback past the first read) is an open item.

#### The live-timeshift segment list grew without bound

**Fixed 2026-10-04, from reading.** Segments behind `oldestAvailableSequence` were clamped past but never erased from `LiveTimeshiftStreamState::segments`, so one open session accumulated an entry per segment for as long as it lasted (43,200 a day at the default 2 s, about 2.8 MB), and every `Read()` scanned the list from the front. `PruneRolledOffLiveSegments()` (`LiveEdgeMargin.h`) drops the leading rolled-off entries at the end of each manifest merge. Offsets are absolute, so the survivors are unaffected, and the newest segment is always kept because its sequence is what the next merge reads as "already known". The read path copies a segment's fields out before releasing the state lock, so no pointer or index is held across the erase.

#### The two persisted state files were written by truncate-then-write

**Fixed 2026-10-04, from reading.** `recording_epg_links.json` and `recurring_rule_adoption.json` were written by opening the real file for write (which truncates it) and then writing, so a power cut or a killed Kodi in between left a truncated file the loader discards whole; for the guide links that loses links for programmes the guide has already dropped, which cannot be rebuilt. `WriteStateFileAtomically()` writes `<path>.tmp` and renames it over the target, retries after removing the old file where the rename over an existing file is refused, and falls back to the old direct write if the rename still fails, so a save is never lost for want of a rename. Not unit-tested (Kodi VFS); built through the Kodi harness.

#### Hardening from the same sweep's missing-tests list

`TimeFromIso()` rejected nothing: `"2026-13-45T25:61:61Z"` came back as a time about a year later and `"+99:99"` shifted a result by over four days. Fields outside their real range now return 0 and only an in-range offset is applied. `XmlTvParser::Parse()` kept a programme whose stop was at or before its start (zero or negative airtime, and a reversed window matches nothing in the recording-to-guide linking); it is dropped now. `M3u8HasEndList()` required the tag to be the whole line, so one trailing space or tab held a finished recording open for the 15 s no-tag grace; trailing blanks are tolerated. New direct tests: the `WebSocketClient` extended-length, fragmentation, ping/pong, close, masked-frame, oversized-control-frame and handshake paths against the local-server fixture (the 126/127 length forms are what every realtime message over 125 bytes takes), `ParseRecordingEpgLinks()` caps and duplicates, `TimeFromIso()` offsets, XMLTV edge cases, the curl header callbacks, the timeshift plugin's file server (`do_GET`/`do_HEAD`, token checks, traversal, Range, the recycled-segment race, token redaction, over a real loopback socket: the only network-facing surface that needs no Dispatcharr authentication, previously uncovered) and `recording_edl`'s `get_edl`, HLS-directory classification and scan roots against fake Django modules.

#### A recurring-rule edit skipped its end-date checks on a cache miss

**Fixed 2026-10-03 (a project-wide pass), found by reading the code, not
reproduced live.** `UpdateTimer()`'s recurring-rule branch re-reads the rule
from the server before editing it, but the two end-date decisions that follow
-- extending `end_date` ahead of a First Day moved past it
(`ComputeRecurringRuleEndDateForStartDateChange()`) and restoring a stale
`end_date` on a disabled-to-enabled toggle
(`ShouldExtendRecurringRuleEndDateOnUpdate()`) -- were gated on the rule being
in `m_cachedRecurringRules`, while the open-ended-rule fallback right between
them already accepted the fresh read. On a cache miss with a successful fresh
read (the cache invalidated by a timer change, or not yet reloaded) a First Day
moved past the end date therefore reached the PATCH and was rejected with a 400,
and a re-enable left the rule's past `end_date` in place. All three decisions
now run whenever either source supplied an end date. No pure logic changed, so
no new test; the three decision functions keep their own.
**Checked live the same day, and found not reachable through Kodi -- so this is a
consistency change, not a user-visible fix.** The miss needs the rule absent from
the cache while Kodi still holds its timer, and three facts close that: the
rules cache is refreshed on a 2-second TTL and keeps its previous contents when a
fetch fails; Kodi only ever holds a timer this addon handed it from that cache;
and Kodi unloads its timers whenever the client is recreated (the Linux client's
log showed "PVR Manager: Stopped" and the PVRTimers thread terminating on an
addon disable/enable). With an HTTP-aware forwarder answering the recurring-rules
LIST endpoint with 500 (the detail endpoint untouched), a fresh addon instance
logged "failed to load recurring timer rules ... 500", listed no recurring rule
at all (only the occurrences, as plain timers), and so offered nothing to edit;
with the failure lifted and Kodi restarted, the rule was listed again. Left in
place: the fresh read is the better source when it exists, and the three
decisions now agree with each other.

#### Timer title edit from Kodi silently did nothing

**Editing a one-time timer's title from Kodi's own Timers-list edit
dialog silently did nothing, a real user-reported bug (2026-09-19),
fixed and merged (squash-merged to `master`).** `UpdateTimer()`'s
not-yet-started-one-time-recording branch (`PVRDispatcharr.cpp`,
around the `UpdateOneTimeRecording()` call) only ever PATCHed
`start_time`/`end_time` to Dispatcharr -- title was read on *create*
(`AddTimer()` -> `CreateOneTimeRecording()`, only for a client-side
`PendingTitle` display cache, never sent to the server either) but
completely ignored on *edit*, with no error and no indication to the
user that anything was dropped. `UpdateOneTimeRecording()`'s own PATCH
deliberately excludes title/`custom_properties` on purpose (a bare
partial PATCH there was confirmed live to crash server-side, see
`docs/RECORDINGS.md`'s "Scoped to times only" note) -- but the actual
rename mechanism, `RenameRecording()` -> Dispatcharr's dedicated
`update-metadata/` endpoint, already existed and already works; it was
just never wired to the Timers-list edit path, only to the Recordings-
list rename action.
**Fix:** `UpdateTimer()`'s one-time branch now also calls
`m_client.RenameRecording()` after the time-fields PATCH succeeds, but
only when `FindRecordingById()` shows the title actually changed --
deliberately not unconditional, so an edit that only touches start/end
time doesn't send a stale/duplicate title and prematurely mark
`user_edited`, which would block Dispatcharr's own EPG-based
auto-enrichment from ever filling the title in on a fully-manual
timer. Build verified through Kodi's real binary-addon harness
(`~/kodi-build`); no new pure-logic extraction here since this is
Kodi-API/HTTP-client glue, outside the unit-tested boundary described
in `CLAUDE.md`.
**Update: live-verified against a real Dispatcharr instance
(2026-09-19) -- the rename itself works correctly.** A real user tested
the PR branch build directly: renaming a scheduled-but-not-started
one-time timer via Kodi's Timers list now sticks server-side. Found a
separate, unrelated cosmetic issue in the same test -- the already-open
Timers window doesn't visually show the new title until the window is
left and re-entered -- traced to a confirmed Kodi-core UI-refresh
limitation (not fixable from this addon's side), written up in
`docs/TROUBLESHOOTING.md`'s "Known Kodi-core quirks" section rather
than tracked here.

#### Three recordings/timers hot-path inefficiencies

**Three real recordings/timers hot-path inefficiencies, found via a
full-codebase Efficiency review (2026-09-10, not yet applied -- would
need live-hardware verification this pass didn't have).**
`RefreshInProgressRecordingManifest()` re-fetches the *entire*
recordings list (a full `GET /api/channels/recordings/` plus JSON
parse of every recording) on every throttled call (up to ~2/sec
during in-progress-recording playback/scrubbing), just to read one
recording's `isInProgress` flag -- a targeted single-recording lookup
(Dispatcharr's REST API likely supports `GET
/api/channels/recordings/{id}/`, a standard DRF `retrieve` action
matching the already-used `{id}/stop/`/`{id}/extend/` siblings, but
this is unconfirmed against real source/a live instance) would replace
an O(all recordings) cost with O(1) on this hot path.
`GetRecordingsAmount()`/`GetRecordings()` and
`GetTimersAmount()`/`GetTimers()` are each called back-to-back by Kodi
on every recordings/timers refresh with no caching between the pair
(unlike channels/EPG, which have `EnsureChannelsLoaded()`/
`EnsureEpgLoaded()` staleness caches) -- doubles/triples the real
REST-fetch cost of what's logically one refresh. Not applied this
pass: adding a new, unconfirmed API endpoint or a caching layer with
its own staleness/thread-safety behavior are both real behavior
changes needing live verification, not pure refactors -- see
`chore/simplify-pass` for the full review this came out of
(including a `FindRecordingById()` helper that *did* get applied,
consolidating the repeated fetch-and-scan code without changing its
cost).
**Update: the single-recording-lookup piece implemented and confirmed
live (2026-09-10).** Confirmed `GET /api/channels/recordings/{id}/`
directly against the live instance first (real, HTTP 200, identical
shape to a list item), closing the "unconfirmed" gap. New
`DispatcharrClient::GetRecordingById()` (a REST call -- not to be
confused with the pre-existing `PVRDispatcharr::FindRecordingById()`,
which scans an already-fetched in-memory list) replaces the
`GetRecordings()` call in `RefreshInProgressRecordingManifest()`;
`GetRecordings()`'s own per-item parsing was pulled out into
`ParseRecordingJson()` so both call sites stay identical rather than
duplicating the field-mapping logic. Live-verified against an actual
in-progress recording during real playback: `kodi.log`'s own timing
breakdown showed `GetRecordingById 0.006-0.009s` on every refresh
(down from a full recording-list fetch+parse), `finished=0`
correctly reflected throughout.
**Update: the `GetRecordingsAmount()`/`GetTimersAmount()` double-fetch
piece also implemented and confirmed live (2026-09-10) -- all three
original findings now closed.** New `EnsureRecordingsLoaded()`/
`EnsureTimerRulesLoaded()`, mirroring `EnsureChannelsLoaded()`/
`EnsureEpgLoaded()`'s own staleness-cache shape but with a
`kRecordingsAndTimersCacheTtlSeconds` (2s) TTL instead of
`channel_refresh_hours`/`epg_refresh_hours` -- short enough that a
user's own change is never meaningfully delayed even before the
invalidation below, long enough to collapse the Amount()+List() pair
Kodi calls back-to-back into one real fetch.
`TriggerRecordingUpdate()`/`TriggerTimerUpdate()` (Kodi SDK base-class
methods, not something this addon defines, so their own implementation
can't be edited directly) are now only ever called through two new
wrappers, `InvalidateAndTriggerRecordingUpdate()`/
`InvalidateAndTriggerTimerUpdate()`, which reset the relevant cache
timestamp to force a fresh fetch on the very next call before
delegating to the real trigger -- applied mechanically across all ~10
existing call sites. `PVRDispatcharr::FindRecordingById()` was also
simplified to call the new `GetRecordingById()` directly instead of
its own separate full-list fetch+scan, now that that REST call exists,
a small additional win beyond the original three findings.
Live-verified against the real instance: 8 rapid-fire `PVR.GetTimers`/
`PVR.GetRecordings` calls (matching Kodi's own Amount()+List() pattern)
produced only 2 real cache refreshes in `kodi.log`, correctly
straddling the 2s TTL, instead of 8 independent fetches under the old
code. Separately confirmed invalidation: added a real one-time timer
via `PVR.AddTimer`, and the very next `PVR.GetTimers` call already
reflected the updated recordings count (a momentary miss right at the
exact instant of creation traced to a pre-existing sub-second race in
the `isInProgress`/`isUpcoming` time-window check, unrelated to this
cache -- resolved within ~3s and confirmed as the correct, pre-existing
behavior, not a regression from this change).

#### In-progress recording seek-bar wall-clock position

**In-progress recording playback had the same seek-bar
wall-clock-position bug live timeshift had -- fixed and live-confirmed
(2026-09-08).** `GetStreamTimes()`'s in-progress-recording branch
hardcoded `startTime=0`, the exact root cause that change fixed for live
timeshift. Fixed more simply than the live case: a recording's real
start time is already known (`PVRRecording::GetRecordingTime()`,
passed through `OpenRecordedStream()` into
`OpenInProgressRecordingStream()`), no cold-start-trim anchor needed.
Confirmed live: seeks in both directions landed at the exact expected
position during an actual in-progress recording's playback. See
`docs/TIMESHIFT.md`'s "PVR.TimeshiftProgress*"/seek bar section's
second "Update" for the full account.
**Update: also confirmed on CoreELEC/ODROID N2+ (2026-09-09).** Same
seek-both-directions test, same clean result. Windows and CoreELEC
now confirmed; macOS/Linux still untested for this specific
fix.

#### In-progress recording playback catch-up-budget hardening

~~In-progress recording playback never received the catch-up-budget
hardening that live timeshift already has~~ -- **Already fixed;
this entry was stale.** When this was written (2026-09-08, after
finding a substantial *uncommitted* refactor sitting on the Linux machine's checkout and discarding it per instruction, since its
authorship/testing history was unknown), the assumption was that the
in-progress-recording path still had the old, unhardened
single-segment/1.5x-margin catch-up math. It didn't -- that exact
refactor (shared `EstimateSegmentDurationMs()`/
`ComputeCatchUpAttempts()` helpers, 5-segment average, 1500ms floor,
3x margin) had already been committed to `master` three days
earlier (2026-09-05), and live-verified there (a
forced tail-seek showed the seek-probe path getting its fast
1-attempt budget and a genuine catch-up wait getting the full 3x
budget and succeeding). The Linux checkout that prompted this
entry was evidently just behind `master` at the time. That commit
never touched this file or `docs/TIMESHIFT.md`, which is why the
gap looked open later -- corrected here, and the fix itself is now
written up in `docs/TIMESHIFT.md`'s "Catch-up budget hardening
shared between live and recording playback" section.

#### Second Packet corrupt/freeze on buffer switch

**A second, related `Packet corrupt`/freeze, confirmed root-caused
and fixed, then re-verified live (2026-09-07).** Switching away from a
channel and back could reproduce a real segment-size disagreement --
this one *did* trigger the 1.0.7 diagnostic live. Root cause: the
plugin's per-worker manifest cache wasn't invalidated across a buffer
restart in a multi-worker deployment (a new ffmpeg process reuses the
same segment filenames/sequence numbers as its predecessor). A first
fix tied cache entries to the buffer's own process ID -- itself then
confirmed live to have a gap under heavy testing churn (OS pids get
recycled; a stale entry tagged with a since-reassigned pid passed the
check it should have failed). Fixed properly by keying on the
buffer's own access token instead (a fresh, random value per genuine
instance, no reuse risk regardless of churn). Re-verified live on
macOS across two independent repro conditions, including the specific
churn pattern that broke the pid-based version -- both clean. Shipped
in `1.0.7` / `timeshift_buffer` `1.0.5`. See `docs/TIMESHIFT.md`'s
"1.0.7 follow-up #2" section for the full account.

#### Overnight recurring rule showed an end time before its start

**Suspected: an overnight recurring rule may display an end time
before its start time in Kodi, flagged from a project-wide review,
confirmed and fixed by an 18th-pass audit (2026-09-26).** Confirmed
against Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- a real source read, not a
live test): `sync_recurring_rule_impl()` (`apps/channels/tasks.py`)
does exactly `if end_dt <= start_dt: end_dt += timedelta(days=1)`, so
Dispatcharr's own scheduler already treats such a rule as crossing
midnight. `ComputeRecurringRuleDisplayTimes()`
(`RecurringRuleUtil.{h,cpp}`) now applies the same `endTimeOfDaySeconds
<= startTimeOfDaySeconds` check and adds 86400 to the displayed end
time when it fires.

#### Fractional channel numbers (5.1) truncated

**Suspected: a fractional channel number (e.g. `5.1`, a real
Dispatcharr subchannel convention per docs/EPG.md's own note that it
arrives as a float like `1.0`) may get silently truncated, flagged
from a project-wide review (2026-09-26) -- confirmed live and fixed,
2026-09-30.** `ChannelParser.cpp`'s `ParseChannelJson()` reads
`channel_number` into `Channel::channelNumber`, a plain `int` --
`FieldOr<int>(item, "channel_number", ...)` calls nlohmann::json's own
`get<int>()`, which silently truncates rather than throwing when the
underlying JSON value is actually a floating-point number (confirmed:
this is documented nlohmann::json behavior, not a guess -- `5.1`/`5.9`
both become `5`, no `type_error` raised, so `FieldOr`'s own null/
wrong-type catch-all never even triggers). If Dispatcharr genuinely
uses fractional channel numbers for subchannels (e.g. `5.1`/`5.2`),
this would collapse them onto one Kodi channel number (5), and
`GetEPGForChannel()`'s own `std::to_string(ch->channelNumber)` lookup
key would then miss that channel's real EPG entirely or pick up a
different channel's. Needs a live check first: does a real Dispatcharr
instance actually assign non-integer channel numbers to real channels,
and if so, how does its own XMLTV export key `<channel id="...">` for
one? If confirmed, `Channel::channelNumber` would need to become a
`double` (with `kodi::addon::PVRChannel::SetSubChannelNumber()` and a
key-formatting fix for the EPG lookup), not just a `FieldOr` tweak.
**Update (2026-09-26, a 19th-pass audit): the "how does XMLTV key it"
half is now answered from Dispatcharr's own real upstream source
(cloned to a scratchpad, not committed to this repo -- stronger than
the API shape alone, still not a live confirmation).** `channel_number`
really is a `FloatField` server-side, and `format_channel_number()`
deliberately emits `"5.1"` (not `"5"`) for a genuinely fractional
value, `"5"` only for a whole one -- so a sub-channel's XMLTV key is
the fractional string, confirming this addon's own integer-truncated
lookup key would miss it, not just collapse two channels onto one
number as originally suspected. Separately, a channel with a *null*
`channel_number` is exported keyed by `str(channel.id)` instead --
`GetEPGForChannel()`'s own `channelNumber <= 0` guard already declines
to look up EPG for that channel at all, so this specific sub-case is
self-consistently a no-op today, not a mismatch, but would need its
own handling (fetching by channel id rather than number) if that guard
is ever loosened. Still needs the live check above before implementing
the `double` conversion.
**Live check (2026-09-30): confirmed.** Three disposable channels
numbered 88881, 88881.1 and 88882.5 (created via the API, deleted
afterward). Dispatcharr stores them correctly and its XMLTV export keys
them distinctly (`<channel id="88881">`, `"88881.1"`, `"88882.5"`), but
Kodi shows the second as channel 88881 (subchannel 0) and the third as
88882: the fractional part is silently dropped and the subchannel is
lost. The addon's own log shows the consequence -- channels 88881 and
88881.1 both refuse a guide ("channel N's own number (88881) is shared
by another channel ... refusing to guess which channel"), so a real 5 /
5.1 subchannel pair would lose *both* guides. None of this instance's
real channels has a fractional number, so the lab lineup is
unaffected.
**Fixed (2026-09-30).** `Channel::channelNumber` is now a `double`
(`ChannelParser.cpp`), and a new `ChannelNumber.{h,cpp}`
(`SplitChannelNumber()`, 12 test cases) derives Kodi's channel number
(the integer part), Kodi's sub-channel number (the digits after the
decimal point) and the exact text Dispatcharr's own export puts in
`<channel id>` -- Python's `str(float)`, reproduced as the shortest
decimal string that reads back as the same double, through a
classic-locale stream. `GetChannels()`/`GetChannelGroupMembers()` now set
both `SetChannelNumber()` and `SetSubChannelNumber()`, and everything that
used the number as an identity -- the two `m_epgByChannelNumber` lookups,
`FindAmbiguousChannelNumbers()`, `HaveChannelNumbersChanged()` -- uses that
key text, so 5 and 5.1 are different channels everywhere. See
`docs/EPG.md`'s new "Fractional channel numbers" section.
**Re-verified live (2026-09-30)**, before and after on the same three
channels (88881, 88881.1, 88882.5, each mapped to one real EPG row that
Dispatcharr exported 424 programmes for; deleted afterward, each
confirmed 404). Before: Kodi showed 88881/0, 88881/0 and
88882/0, the log showed the two 88881 channels both refusing a guide as
"shared", and after 8 minutes none of the three had any guide. After
(rebuilt, redeployed, Kodi restarted): 88881/0, 88881/1 and 88882/5, all
three with a guide (each with broadcasts in Kodi's window), and the
collision guard still refusing the lab lineup's genuine duplicate-number
channels, unchanged. Repeated with the same three channels
placed in a real channel group, which exercises
`GetChannelGroupMembers()` too: Linux's group view showed the same
88881/0, 88881/1 and 88882/5. On the Windows test client all three got a
guide as well (each with broadcasts); that profile has "use backend
channel numbers" off, so Kodi shows its own sequential numbers there and
the displayed number isn't a check of the addon on that client.

#### Paused live-TV viewer's heartbeat goes stale

**Suspected: a paused live-TV viewer's own per-viewer heartbeat may go
stale if Kodi stops calling `ReadLiveStream()` during a long pause,
letting another viewer's Stop tear down the paused viewer's buffer,
flagged from a project-wide review (2026-09-26) -- confirmed live,
fixed, and re-verified live, 2026-09-28.** The addon only ever sends the per-viewer `heartbeat`
(`SendTimeshiftHeartbeat()`) from `ReadLiveTimeshiftStream()` itself
(`DispatcharrClient.cpp`'s only call site) -- `RefreshLiveManifest()`
(used by `GetLiveTimeshiftStreamLength()`, which Kodi's own
`GetStreamTimes()` polling can still call while paused) sends only
the buffer-wide `channel_uuid` heartbeat, not a per-viewer one, and
`timeshift_buffer`'s own `_get_live_manifest_action()` calls
`_apply_heartbeat()` with no `viewer_id` either. `docs/TIMESHIFT.md`'s
own "Not expected to risk killing a real, actively-watched session,
including a paused one" reasoning was written before per-viewer
heartbeats existed and covers the buffer-wide heartbeat correctly,
but not `viewer_heartbeats` specifically. If two viewers share a
channel and one pauses long enough for Kodi to genuinely stop calling
`ReadLiveStream()` (unconfirmed), `_prune_stale_viewers` would drop
the paused viewer once its own heartbeat exceeds `idle_timeout_seconds`
(default 30s, plus up to one 15s reaper tick); if the other viewer
then stops, `_stop_buffer` sees no viewers left and tears the buffer
down under the paused one, breaking its resume. Single-viewer sessions
are unaffected (the buffer-wide heartbeat alone keeps the buffer
alive). Needs a live check first: does Kodi actually stop calling
`ReadLiveStream()` during a long pause? If confirmed, the fix is
either passing `viewer_id` through to `get_live_manifest`'s own params
(`_get_live_manifest_action` already accepts a `viewer_id` via
`_apply_heartbeat()`), or also sending the per-viewer heartbeat from
the throttled refresh path.

**Update (2026-09-28, live testing): confirmed, on both counts, against
a real Kodi client and the real lab instance.** Tuned a real Kodi
client to a live channel (server-side timeshift already on), joined a
synthetic second viewer via the plugin's own API to keep the buffer
alive independent of the real client, paused the real client via
JSON-RPC (`Player.PlayPause`, confirmed `speed: 0`), then watched
`list_buffers`' own viewer count every 10s while refreshing only the
synthetic viewer's heartbeat. The real client's own viewer entry was
pruned at the 30s mark, exactly matching `idle_timeout_seconds`'
default -- confirming Kodi does genuinely stop calling
`ReadLiveStream()` while paused, not just a theoretical possibility.
Stopping the synthetic viewer next (the only one left, from the
server's own perspective) tore the buffer down completely, exactly as
this entry predicted.
**The practical consequence turned out to be worse than "breaking
resume," and is logged as its own, broader entry** (see "A dead
live-timeshift buffer's segment fetch failure has no recovery path,
only an infinite tight retry loop") -- unpausing the real client
didn't produce a clean error at all; Kodi's own UI kept reporting
`speed: 1` and an advancing playback clock while the addon silently
hammered the now-gone buffer's segment endpoint at ~7-8 requests/sec
with no backoff, indefinitely (1,230 identical failed fetches logged
over ~2m37s before the test was manually stopped -- this would have
continued forever otherwise). This item's own originally-proposed fix
(passing `viewer_id` through to `get_live_manifest`, or sending the
per-viewer heartbeat from the throttled refresh path) would still
close the specific reference-counting gap described above, but
doesn't address the broader retry-storm defect on its own -- a buffer
can still die out from under an active read for other reasons (the
idle-timeout reaper, an admin's Stop All, a real server crash), and
`ReadLiveTimeshiftStream()` needs to actually recover or fail cleanly
in that case regardless of why the buffer went away.

**Fixed and re-verified live, 2026-09-28, same day.** Went with the
first of the two originally-proposed fixes: `viewer_id` is now passed
through to `get_live_manifest`'s own params
(`RefreshLiveManifest()`, `DispatcharrClient.cpp`, using
`m_liveTimeshiftStream.viewerId` -- the same field
`SendTimeshiftHeartbeat()`/`StopTimeshiftBuffer()` already use), and
`timeshift_buffer`'s own `_get_live_manifest_action()` now resolves
it via `_resolve_viewer_id(params)` and passes it into
`_apply_heartbeat()` instead of calling that with no `viewer_id` at
all. `_apply_heartbeat()` itself already safely no-ops for an
unrecognized `viewer_id` (matching `_heartbeat()`'s own established
convention), so this needed no change on that end. Plugin version
bumped to `0.6.3` (`plugin.json` and `plugin.py`'s `Plugin.version`
together, per this project's own versioning convention) and covered
by two new pytest cases (`test_run_get_live_manifest_refreshes_the_calling_viewers_own_heartbeat`,
`test_run_get_live_manifest_ignores_a_viewer_id_this_buffer_does_not_know_about`).
Re-verified live against the real lab instance and a real Kodi
client, reproducing the exact original test methodology: joined a
synthetic second viewer (its own heartbeat kept fresh manually, the
same way the original bug reproduction did), paused the real client,
then watched the viewer count for a full 50s -- past the 30s mark
that previously pruned the real client's own entry -- with **no
manual intervention on the real client's behalf at all**. Viewer
count stayed at 2 throughout, confirming Kodi's own automatic
`GetStreamTimes()` polling now keeps the real client's per-viewer
heartbeat fresh purely on its own. Unpausing resumed real playback
cleanly (confirmed progressing in real time, not just `speed: 1`).
Full C++ (627 test cases) and Python (320 test cases) suites pass.

#### Dead live-timeshift buffer: infinite tight segment-fetch retry

**A dead live-timeshift buffer's segment fetch failure has no
recovery path, only an infinite tight retry loop -- found live
2026-09-28 while reproducing the "Paused live-TV viewer's heartbeat goes stale" entry, not
specific to that scenario -- fixed and confirmed live the same day.**
`ReadLiveTimeshiftStream()`'s per-segment
fetch (`DispatcharrClient.cpp`) only ever calls `RefreshLiveManifest()`
-- the one call site that can detect and set `m_liveTimeshiftStream.fatal`
-- from its own "caught up to the tail, wait for a new segment" branch,
gated on `position >= totalBytes`. When the addon still believes an
already-manifested segment is waiting to be read (`position` inside a
segment it already knows about, from before the buffer died), that
branch never runs at all, so a segment-fetch response that isn't
200/206/404 (401/403/5xx -- confirmed live with a 403, from a buffer
whose access token died along with it) falls straight to `return -1`
with nothing else attempted: no re-manifest, no fatal flag, no
recovery of any kind. Kodi's own core retries a `-1` read near-
immediately rather than giving up, so this becomes an unbounded, ever-
repeating request against the same dead URL -- confirmed live at
roughly 7-8 requests/sec, sustained with zero backoff for as long as
playback wasn't manually stopped (1,230 identical failed fetches over
~2m37s in one test run). Worse than just "wastes requests": Kodi's own
UI showed `speed: 1` and a steadily advancing playback clock
throughout, giving no visible indication anything was wrong -- a
silent hang, not a clean failure a user (or this addon's own retry/
reopen logic) could react to. Reachable any time a buffer dies while
a client still has unread, already-manifested segments queued up --
not limited to the paused-viewer scenario; the idle-timeout
reaper, an admin's Stop All action, or a real plugin/server crash mid-
playback would all hit this same dead end. Not fixed blind this pass:
the right fix needs a design decision (e.g. having the `401`/`403`/
`5xx` branch itself call `RefreshLiveManifest()` to distinguish "buffer
genuinely gone, mark fatal" from "a transient plugin-side hiccup, retry
a bounded number of times" before falling back to `-1`) plus a second
live pass to confirm it actually recovers cleanly, not just fails
louder.

**Fixed and confirmed live, 2026-09-28, same day.** Added
`dispatcharr::ShouldGiveUpAfterSegmentFetchFailure()` (`SegmentFetchFailure.h`,
with its own Catch2 coverage, `tests/test_segment_fetch_failure.cpp`) and
a new `HandleLiveTimeshiftSegmentFetchFailure()` helper both the
transport-error (`res != CURLE_OK`) and bad-HTTP-status branches now
call: it calls `RefreshLiveManifest(force=true, ...)` exactly like the
existing "caught up to the tail" branch already does, and gives up
(marking `m_liveTimeshiftStream.fatal`) either the instant that refresh
positively confirms the buffer is gone, or once
`consecutiveSegmentFetchFailures` (a new per-Open() counter, reset on
any 200/206/404 response) reaches a small fixed bound (4) -- the
fail-safe for the one case a plain manifest refresh can't positively
resolve (a stale `access_token` against a buffer that silently
restarted with a fresh one, since `get_live_manifest`'s own response
never carries a token). Re-verified against a real Kodi build (native
Linux, through the real `tools/depends/target/binary-addons` harness,
not just the standalone Catch2 suite) and the same real lab instance,
reproducing the exact original precondition (paused long enough to
build a real unread-segment backlog, then the buffer torn down via
`stop_all` while still paused, then unpaused into the dead buffer):
exactly one failed segment fetch was logged before the stream was
marked fatal (down from 1,230+ before this fix, in a same-length test
window), and nothing further was logged or sent to the server
afterward -- the existing `if (m_liveTimeshiftStream.fatal) return -1;`
short-circuit at the top of `ReadLiveTimeshiftStream()` took over
exactly as it already does for the "caught up to the tail" fatal path.
**One caveat found during this same live check, left as-is rather than
chased further, since it's a distinct concern from the request-storm
this pass set out to fix**: Kodi's own player still didn't proactively
notice the stream had gone fatal and stop/error out on its own --
`Player.GetActiveProperties` kept reporting `speed: 1` with no error
shown, even though every `Read()` call was now returning `-1`
immediately with zero network activity. Kodi's own core apparently
needs more than the ~2 minutes this check waited before it gives up on
a persistently-failing live stream read on its own (if it ever does) --
a separate, Kodi-core-level question from the server-hammering bug
this pass actually fixes, and out of scope for it.

#### TimeFromIso() ignores a trailing UTC-offset suffix

**`DateTimeFormat.cpp`'s `TimeFromIso()` ignores any trailing
UTC-offset suffix on a Dispatcharr date-time field, flagged from a
project-wide review as a deliberate design choice worth re-confirming
-- confirmed correct by an 18th-pass audit (2026-09-26).**
`TimeFromIso()`'s own header comment already stated the reasoning:
it only parses the "YYYY-MM-DDTHH:MM:SS" prefix, "consistent with
every timestamp elsewhere in this API being UTC-normalized already."
Confirmed against Dispatcharr's own real current upstream source
(cloned into a scratchpad, never committed to this repo -- a real
source read, not a live test): `dispatcharr/settings.py` sets
`TIME_ZONE = "UTC"` and `USE_TZ = True`, with no `timezone.activate()`
call anywhere in the codebase -- DRF always emits UTC timestamps, so
the premise this function's own comment already relied on is correct,
not just assumed. A self-hosted instance could in principle override
`TIME_ZONE` in its own settings, which this can't rule out, but that's
no longer an open question about this addon's own logic.
**Fixed (2026-09-26, a 35th-pass audit): the "every timestamp
elsewhere in this API" scoping in this item's own conclusion turned
out to have a real, confirmed exception this pass found, not covered
by the 18th-pass analysis above -- `TimeFromIso()` now actually
applies a genuine trailing offset instead of ignoring it.**
`custom_properties.program.start_time`/`end_time` (the field
`Recording::programStartTime`/`programEndTime`, added the previous,
34th, pass, reads) is not a serialized Django model field at all --
Dispatcharr's own recurring-rule scheduler
(`sync_recurring_rule_impl()`, `apps/channels/tasks.py`) hand-builds
it via a timezone-*aware* Python datetime's own `isoformat()` call, in
that rule's own configured system timezone (an admin-configurable
value this addon's own `recurring_rule_timezone` setting already has
to bridge elsewhere -- not Django's global `TIME_ZONE`, which the
18th-pass analysis above correctly confirmed is pinned to UTC and
remains accurate for every field it actually covers). Ignoring a
genuine non-zero offset there returned a time off by the whole zone
offset for any Dispatcharr instance configured to a non-UTC system
timezone -- reachable in practice via the pass-34
`FindEpgEntryIndexByStartTime()` exact-match path, which fed a wrong
broadcast id straight into `SetEPGUid()`/`SetEPGEventId()` for a
recurring-rule occurrence, linking a timer/finished recording to an
unrelated programme hours away instead of its own. See
`TimeFromIso()`'s own updated header comment (`DateTimeFormat.h`) for
the full account.

#### In-progress recording cold start gives up too early

**In-progress recording cold start may give up too early against
Dispatcharr's own pre-first-segment response, flagged from a
project-wide review, and largely confirmed by an 18th-pass audit
(2026-09-26) -- confirmed live (the window is about 3 seconds) and
fixed, 2026-09-30.**
`OpenInProgressRecordingStream()`'s cold-start loop
(`DispatcharrClient.cpp`) returns failure immediately on the *first*
failed `RefreshInProgressRecordingManifest()` call -- it only actually
retries when the refresh *succeeds* but returns no segments yet,
unlike the live-timeshift path's own equivalent loop, which retries on
either. Confirmed against Dispatcharr's own real current upstream
source (an 18th-pass audit cloned it into a scratchpad, never
committed to this repo -- a real source read, not a live test):
`RecordingViewSet.hls()` (`apps/channels/api_views.py`) raises a 404
both when `_hls_dir` isn't set/created yet *and*, separately, when
`index.m3u8` doesn't exist inside an already-created directory --
either way, `FetchRawInProgressPlaylist()` sees a non-2xx status and
returns false, which this addon's own outer loop currently treats as
an immediate, unretried failure.
**The genuine conflict:** the cold-start loop's own comment, right
above it, says a 45s budget was specifically confirmed live as
necessary and sufficient here -- which shouldn't be possible if the
loop truly gives up on the very first 404, since that would mean
*every* sufficiently-early open attempt fails outright, not just ones
needing close to the full 45s. Nothing found in the fetched upstream
source explains the discrepancy (no special-cased "wait and retry"
behavior on the server side for this specific endpoint) -- possible
explanations, none confirmed: the live test that established "45s
needed" happened to open the recording after `_hls_dir` already
existed (so only the second 404 case, `index.m3u8` missing, was ever
actually hit, and something about that specific case behaves
differently in a way not yet identified); or the version of
Dispatcharr tested against handled this endpoint differently before.
Needs a fresh, deliberate live test (open a recording within the very
first second or two of it starting, before `_hls_dir` could plausibly
exist yet) to resolve which understanding is actually correct before
touching the retry loop itself. If the bug is confirmed, the fix is to
retry on a failed refresh the same way the live-timeshift path
already does, within the existing attempt budget.
**Live check (2026-09-30): confirmed, but the window is only about 3
seconds.** With realtime updates on, Kodi lists a newly started
recording ~1.0-1.2s after it is created. Opening it at +1.0, +1.2, +1.5,
+2.0 and +2.5s after creation failed every time (6 of 6): `Player.Open`
returned in 0.3s with "failed to open recording: Dispatcharr returned
HTTP 404 fetching in-progress playlist" after a single cold-start
attempt -- no retry inside the 45s budget, exactly the entry's reading
of the loop (it returns on the first *failed* refresh and only retries a
*successful* refresh that has no segments yet; the HLS directory doesn't
exist for the first couple of seconds). Opening at +3, +6, +9 and +12s
all succeeded on the first attempt. Real exposure is small (a person
can't reach a recording that fast; a script can), but the fix is simple:
retry a failed first refresh within the existing budget while the
recording is known to be in progress.
**Fixed (2026-09-30).** `RefreshInProgressRecordingManifest()` now reports
through a new optional `coldStartRetryableOut` whether a failed refresh is
worth waiting out, and `OpenInProgressRecordingStream()`'s cold-start loop
sleeps and retries on it instead of returning. The decision is
`dispatcharr::ShouldRetryInProgressColdStart()` (`RecordingVisibility.h`, 4
test cases), deliberately narrow: only an HTTP 404 (`FetchRawInProgressPlaylist()`
gained an `httpStatusOut` for this -- a 3xx means the directory was already
removed and the file is complete, and a transport failure, 401 or 5xx says
nothing about the recording being young) *and* a lookup that succeeded and
says the recording is in progress right now, so a finished, failed or
deleted recording still fails fast rather than burning the budget. The
budget is unchanged: the same 90 x 500ms the no-segments-yet case already
had.
**Re-verified live (2026-09-30)** on the Linux test client. The first
harness (opens at +1.5, +2.0 and +2.5s, plus one at +2.6s) went 4 for 4,
needing 5, 2, 1 and 3 attempts, where the same window failed 6 of 6 before.
Two more trials in that first run, opened at +1.1s and +2.1s, looked like
failures but were the harness's own timing: Dispatcharr's playlist took
more than 9s to appear that time, the harness gave up on playback after
~8s, and its cleanup stopped the recording mid-wait -- which is exactly
the case the fix declines to retry (the recording is no longer in
progress), so the open ended with the original 404 message. A patient
re-run (waits for genuinely advancing playback, up to 75s) opened four
recordings the instant Kodi listed them (0.9-1.0s after creation, earlier
than any failing trial before), each waited out 2-3 "playlist not created
yet" cycles and reached advancing playback within 4.1-5.8s: 4 of 4. On the
Windows test client three opens all played on their first attempt, but
Kodi there listed each recording 1.8-7.6s after creation, later than the
failing window, so the retry itself was only exercised on Linux.

#### EnsureAuthenticated() refresh-token attempt ran before the Login() backoff

**`EnsureAuthenticated()`'s refresh-token attempt runs before, and
independently of, the Login() backoff below it, flagged from a
project-wide review (2026-09-26) -- RESOLVED as of a 47th-pass audit
(2026-09-27), confirmed stale by a 57th-pass audit's own re-check
(2026-09-27), distinct from the still-open, already-tracked
`Request()` 401-retry-bypasses-`AuthBackoff` item ("Request()'s 401 retry bypasses AuthBackoff"; a different code path).** Originally: `if (!m_refreshToken.empty() &&
RefreshAccessToken(error)) return true;` ran unconditionally on every
call once a refresh token had ever been obtained, with no backoff of
its own -- only the subsequent `Login()` fallback was guarded by
`m_consecutiveLoginFailures`/`m_loginBackoffUntil`. A 47th-pass audit
(see `CLAUDE.md`'s own entry on it) moved both the `Login()` backoff
gate and the transient-failure cooldown gate to run *before* the
refresh-token attempt, closing exactly the scenario this entry
describes -- confirmed against the current source
(`EnsureAuthenticated()`, `DispatcharrClient.cpp`): a refresh attempt
can no longer run while either gate is active, and `RefreshAccessToken()`'s
own 401/403 now clears `m_refreshToken` so it isn't retried forever,
while a transient (0/5xx) refresh failure applies the same cooldown
`Login()`'s own failures do. This entry was left un-updated for 10
passes despite the fix already existing -- exactly the "grep docs/ on
a change" miss `CLAUDE.md`'s own conventions warn about.

#### In-progress read path didn't clamp segment read window to probed byteSize

**The in-progress recording read path doesn't clamp a segment's read
window to its own probed `byteSize`, flagged from an 8th-pass audit
(2026-09-26) -- RESOLVED as of a 55th-pass audit (2026-09-27), after
a more recent pass's own fix elsewhere made the previously-speculative
trigger deterministic instead.** Originally logged as defensive
hardening only: `ReadInProgressRecordingStream()`'s cached-segment-body
read clamped correctly against the *actual fetched body's* size
(`cached.size() - offsetInSegment`), but never cross-checked that
against `seg->byteSize` (the earlier HEAD-probed size) the way the
live-timeshift read path already did -- "plausible only in an
already-anomalous scenario" at the time, since this codebase's own
documented assumption is that ffmpeg only lists a segment once fully
written, so probe-then-fetch should be stable. `ProbeSegmentByteSize()`'s
own `kMaxProbedSegmentByteSize` clamp (added a 53rd-pass audit, 45
passes after this entry was first logged, corrected here in a
56th-pass audit after this entry's own "added two passes later" was
found to be a real timeline error, to fix an unrelated OOM-crash risk)
removed that "already-anomalous" caveat entirely: any segment
genuinely larger than that ceiling now deterministically triggers
exactly this gap, since the addon's own cumulative byte-offset
bookkeeping uses the *clamped* size while an unbounded fetch still
cached the segment's real, larger body -- silently splicing in
wrong-offset bytes from the *next* segment's own file once `position`
advanced past the clamped boundary, with no error raised (the same
"Packet corrupt"/audio-desync failure class this project has
repeatedly investigated elsewhere). Fixed a 55th-pass audit: the
segment fetch now writes into a `FixedBufferSink` bounded to
`segByteSize` instead of an unbounded `std::string`, so an oversized
segment's tail is cleanly (if lossily) dropped rather than silently
misaligning everything after it -- this addon's own official byte
accounting and what it actually delivers now agree for a real body
*larger* than `segByteSize` (corrected 2026-09-27, a 56th-pass audit,
fixing a real overstatement in this entry's own prior "in every case"
wording, found via a project-wide review, not itself independently
reproduced: a real body genuinely *smaller* than `segByteSize` is a
separate, pre-existing, unrelated behavior -- an empty/short read
there still reaches Kodi as an early EOF, not something this fix
touches at all), not literally every possible case. That same
55th-pass fix itself had its own real, confirmed regression, caught
and fixed the very next, 56th, pass: it zeroed/resized the *shared*
`cachedSegmentBytes` before the fetch even started, so any of this
read's several failure returns left `cachedSegmentByteOffset` still
pointing at the previous, still-valid segment while the bytes behind
it had already been overwritten -- fixed by fetching into a local
buffer instead, only replacing both together once the fetch has
genuinely succeeded, the same atomicity the original `std::string`
version (before the 55th-pass fix) already had.

#### DeleteRecording()/RenameRecording() trigger only a recordings refresh

**`DeleteRecording()`/`RenameRecording()` only trigger a recordings
refresh, not a timers one, flagged from a 9th-pass audit (2026-09-26)
-- RESOLVED as of a 50th-pass audit (2026-09-27).** Both functions'
own success paths already trigger timers-then-recordings (fixed pass
42, well before this entry was last touched). `RenameRecording()`'s
own *failure* path (added pass 48, after this entry was written) had
reopened the same gap in miniature -- a lone recordings-only trigger,
no timer trigger first -- fixed the same, 50th, pass this entry is
being resolved in; see `RenameRecording()`'s own comment
(`PVRDispatcharr.cpp`). The "deleting a now-stale timer entry calls
`StopRecording()` against an id Dispatcharr no longer has" concern
this entry originally raised is also resolved: `StopRecording()` now
treats a 404 as success (pass 49). `DeleteRecording()`'s own failure
path deliberately still has no trigger of any kind -- confirmed
against Kodi's own real current SDK source that
`CPVRGUIActionsRecordings::DeleteRecording()` does no optimistic
client-side mutation before calling into the addon the way `Rename()`
does, only an error dialog on failure, so there's nothing here for a
refresh to correct.

#### Pruned viewer can never be re-added to a buffer

**A viewer pruned by `_prune_stale_viewers` (idle_timeout_seconds,
default 30s) can never be re-added to a buffer's `viewers` list on its
own, flagged from a 10th-pass audit (2026-09-26) -- extends the "Paused live-TV viewer's heartbeat goes stale" entry.** `_apply_heartbeat()`'s own
docstring already documents, on purpose, that it never records a
heartbeat for a `viewer_id` not already in `state["viewers"]` -- so
once pruned, that viewer_id's later `heartbeat`/`get_live_manifest`
calls are silently ignored forever; only a fresh `start_buffer` call
(an addon-side channel open, not anything a paused/still-playing
session on the same channel would trigger) re-adds it via
`_start_buffer`'s own reattach branch. Worth noting for whoever
eventually picks up that other entry's proposed fix (passing
`viewer_id` through to `get_live_manifest`'s own params): that fix
alone would **not** close this gap, since `_apply_heartbeat` would
still ignore an already-pruned, unknown `viewer_id` even once it's
passed through -- the real fix needs `_apply_heartbeat` (or its
caller) to re-add a previously-known-but-pruned `viewer_id` on an
explicit heartbeat/manifest call, not just accept one already present.
**Live check + fixed (2026-09-29, `timeshift_buffer` 0.6.5).**
Confirmed live against the deployed 0.6.4 on the real lab instance
(sequential calls only): with one viewer registered, a `heartbeat` and
then a `get_live_manifest` call from a second, unregistered `viewer_id`
both returned `ok` but `list_buffers` still showed 1 viewer -- the
caller was silently never counted. Fixed by making
`_apply_heartbeat()` re-add a `viewer_id` missing from `viewers` (plus
its `viewer_heartbeats` entry) rather than ignoring it, covering both
call sites (`heartbeat`, and `get_live_manifest` -- the only thing a
paused viewer still sends). Re-verified live after deploying 0.6.5:
the same unregistered-viewer heartbeat now takes the buffer from 1
viewer to 2. Known, accepted cost: a straggler heartbeat arriving
after that viewer's own `stop_buffer` resurrects it, bounded by the
same `idle_timeout_seconds` prune (and moot when it was the last
viewer, since the buffer's state is already gone). This reverses this
file's own earlier deliberate "unknown viewer_id is never recorded"
convention (three tests retargeted, two new).

#### Recording status fix assumed Dispatcharr never writes a pre-start status

**`RecordingParser.cpp`'s `custom_properties.status`-clears-`isUpcoming`
fix (this session's 9th pass) assumed Dispatcharr never writes a
pre-start status value, flagged from a 10th-pass audit as worth
re-confirming -- confirmed true, and fixed, by an 18th-pass audit
(2026-09-26).** The hypothetical `"scheduled"` pre-start status this
entry worried about turned out real: an 18th-pass audit cloned
Dispatcharr's own current upstream source (`git clone --depth 1`, into
a scratchpad, never committed to this repo -- a real source read, not
a live test, but stronger evidence than the API shape alone) and
confirmed `apps/channels/tasks.py`'s `sync_recurring_rule_impl()`
creates every future recurring-rule occurrence with exactly
`custom_properties.status = "scheduled"`, gated by its own `if
start_dt <= now: continue` check (so this is only ever set for a
genuinely future `start_time`). This was a real regression on the
`extract-pure-logic-candidates` branch specifically (confirmed via
`git merge-base --is-ancestor`: the original fix's own commit
was never on `master`) -- caught and fixed before ever
merging, not a live production bug. Every future occurrence of every
recurring rule vanished from Kodi's Timers list
(`IsListedAsTimer()` needs `isInProgress || isUpcoming`, both false)
and incorrectly appeared in Recordings instead
(`IsListedAsRecording()` returns `!isUpcoming`), immediately on
creation. Fixed by giving `"scheduled"` its own explicit case
(authoritatively upcoming, not finished) instead of falling into the
same "any other non-empty status means finished" bucket as a genuine
terminal status -- the same pass also confirmed, directly from
`apps/channels/api_views.py`'s own `extend()` action, the exact
canonical terminal set this addon's own comment had guessed at:
`("completed", "stopped", "interrupted")`, alongside `"recording"`
(active) and `"scheduled"` (not yet started). No other non-empty
status value has ever been observed in the fetched source, so an
unrecognized one still falls back to the same "finished" treatment a
terminal status gets, per this file's own established "err toward the
more clearly-confirmed default" convention.

#### Addon can't detect its live-timeshift buffer was replaced

**Nothing lets the addon detect that its live-timeshift buffer was
replaced out from under it by a concurrent viewer, flagged from an
11th-pass audit (2026-09-26), low probability.** `RefreshLiveManifest()`
merges purely by sequence number, and `get_live_manifest`'s response
carries no buffer-instance identifier at all. If viewer A's ffmpeg dies
and viewer B's `start_buffer` restarts it before A's own next poll, A
never sees `fatal` (its next manifest read just looks like a
lower-than-expected sequence range, silently filtered out as "not
new" rather than recognized as a different buffer instance), and A's
cached `access_token` is now stale, so its own segment reads get a 403
and `ReadLiveTimeshiftStream()` returns -1 with no clear explanation in
the log. Suggested fix: have the plugin return a stable per-instance
identifier (e.g. a hash of `access_token`, already unique per genuine
new instance -- see `_start_buffer`'s own comment) in every
`get_live_manifest` response, and have the addon treat a change in
that identifier as fatal. This would also make the "_start_ffmpeg() can spawn inside a leftover channel directory" entry fail cleanly instead of stalling silently.
**Live check + fixed (2026-09-29, `timeshift_buffer` 0.6.5, addon
change in `RefreshLiveManifest()`).** Reproduced live on the deployed
0.6.4 with sequential calls only (no race, nothing to orphan): viewer A
started a buffer, an unconditional `stop_buffer` tore it down, viewer B
started a fresh one (different `access_token`, confirmed), and A's
`get_live_manifest` -- passing its own now-stale token -- came back
plain `ok` with B's 16 segments, no `fatal`. Fixed without the
suggested per-instance identifier in every response: the addon now
sends its own cached `access_token` as a `get_live_manifest` param and
the plugin (`_is_stale_access_token()`, a `compare_digest` against the
tracked state) answers `fatal: true` on a positively-confirmed
mismatch -- deliberately *without* tearing the buffer down, since the
buffer now tracked for that channel is a healthy one belonging to
someone else. Nothing supplied, or state predating access tokens, is
never treated as stale (an older addon keeps working unchanged).
Re-verified live after deploying 0.6.5: A's stale-token poll returned
`fatal: true` 4 of 4 times, B's buffer stayed alive and kept
producing segments. The rebuilt addon was also deployed to the real
Kodi test client: a live-timeshift playback + backward seek passed
end to end with the new param in play (the buffer was pre-warmed via
the API first -- see next sentence), and the full read-only
`tools/kodi_smoke_test.py` run (12 passed, 0 failed, 3 skipped)
doubled as the first live run of this session's harness fixes.
Side observation, not chased: today a cold `start_buffer` on two
different channels took ~15-25s to produce its first segment, at or
past the addon's own cold-start budget, so an un-warmed Kodi open
failed with "live playlist not found" and fell back to direct
playback -- provider latency this session, not a regression, but worth
keeping in mind if cold-start failures get reported. 10 new plugin
test cases (`_is_stale_access_token()` cases plus the
no-teardown-on-replaced-buffer `Plugin.run()` test).

#### _start_buffer has no per-channel lock

**`timeshift_buffer`'s `_start_buffer` has no per-channel lock around
its own classify-then-spawn sequence, flagged from a 12th-pass audit
(2026-09-26), architectural -- low probability, severe impact if hit,
not reproduced live.** Two simultaneous fresh `start_buffer` calls for
the same channel (two viewers opening it at almost the same instant,
or a caller retrying while a slow first attempt is still in flight)
can both read `_get_buffer_state()` before either writes, both
classify as `"none"`/`"dead"`, and both spawn their own ffmpeg process
into the same channel directory. The second `_set_buffer_state()`
write clobbers the first's, so the first ffmpeg is no longer tracked
by anything -- the reaper, `stop_buffer`, and `stop_all` can never kill
it, and it holds a provider stream slot until the container restarts.
Both processes write the same `live.m3u8`/segment files, and the first
caller's own `access_token` no longer matches the state that won,
so its own later reads all 403. `max_concurrent_buffers`' own
`len(_list_buffer_keys())` check has the identical race. A real fix
needs a per-channel Redis lock (`SET NX EX`, released after the state
write or on failure) around the classify-and-spawn step -- a genuine
architecture change (new failure/cleanup paths, TTL tuning), not a
quick patch, so it needs its own dedicated pass with live verification
(concurrent `start_buffer` calls against one channel) rather than a
blind change.
**Live check (2026-09-29): reproduced on the first attempt, exactly as
predicted, with a real, confirmed consequence beyond the theoretical
description above.** Fired two genuinely concurrent `start_buffer`
calls (backgrounded, near-simultaneously) for the same real channel
against the real lab instance, starting from a confirmed-clean state
(`list_buffers` showed no active buffers first). Both calls came back
`"already_running": false` with two different, real `access_token`
values -- direct, conclusive proof both independently spawned their
own ffmpeg process, not just a theoretical read-before-write
possibility. A follow-up `list_buffers` showed exactly one tracked
buffer with one viewer, confirming the second caller's own ffmpeg was
already orphaned at that point. Also confirmed live that
`_find_orphaned_channel_dirs()`/`scrub_orphaned_buffers` genuinely
cannot detect or clean up this specific orphan either, not just in
theory: it keys its own orphan check by channel *directory*
(`_get_buffer_state(entry.name) is not None`), and both the winning
and losing ffmpeg process write into the *same* channel-uuid-named
directory -- so the directory still reads as "tracked" (because the
surviving buffer's own state uses that identical key), hiding the
second process from this cleanup path too, not just from the reaper/
`stop_buffer`/`stop_all` the original entry already named. Real-world
consequence confirmed, not just inferred: this left an actual orphaned
ffmpeg process holding a real provider stream connection on the real
instance, with no way to reach or clean it up via any of this plugin's
own API surface -- resolved only by the user restarting the
Dispatcharr container themselves (the same "until the container
restarts" resolution this entry's own original text already named).
Confirms both the race itself and the severity assessment ("severe
impact if hit") are accurate, not overstated -- still not fixed, the
real per-channel-lock fix sketched above is unchanged and still needs
its own dedicated implementation pass.
**Fixed and re-verified live, 2026-09-29 (`timeshift_buffer` `0.6.4`).**
Implemented the per-channel Redis lock sketched above:
`_acquire_start_buffer_lock()`/`_release_start_buffer_lock()` (new,
`plugin.py`) wrap the whole classify-then-spawn sequence in `SET NX EX`
(the same primitive `_reaper_loop`'s own leader election already uses,
just per-channel/short-lived instead of singleton/continuously-renewed),
keyed by channel uuid so two different channels starting simultaneously
never contend with each other. A caller that loses the race gets the
same `retryable: true` structured-signal convention the existing
"stopping" case already uses, rather than blocking -- the lock's own
30s TTL (a safety net for a caller that dies mid-hold, not a normal-
case wait) keeps that window short regardless. `_start_buffer()`'s own
pre-existing classify-then-spawn body moved unchanged into a new
`_start_buffer_locked()`, called only while the lock is held, released
in a `finally` so a failed call (ffmpeg missing, any exception) can't
leave the channel stuck until the TTL expires. 7 new dedicated tests
(lock acquire/release/independence-per-channel/stale-token-doesn't-
clobber-a-fresh-holder, plus `Plugin.run()`-level tests for the
retryable-contention response and finally-block release-on-error) --
full plugin suite green throughout (211 tests).
Re-verified live end-to-end, the hard way -- two real live attempts hit
a genuine, real operational gotcha this plugin's own README already
documented ("If a code change to this plugin doesn't seem to take
effect") before a third, clean attempt actually confirmed the fix:
neither `POST /api/plugins/plugins/import/` (with `overwrite: true`)
nor the explicit `POST /api/plugins/plugins/reload/` reliably got
every worker process running the new code, so the first two live
concurrent-`start_buffer` attempts against the real lab instance
*still* reproduced the original race (both `already_running: false`,
two ffmpeg processes, one orphaned each time) even after each of those
reload paths reported success -- only a full Dispatcharr restart
actually did it. Each of the first two leaks needed a real restart of
the user's own live instance to clear, since neither is reachable via
any of this plugin's own cleanup actions (confirmed already, see the
live check above). After the second restart, confirmed the plugin
really was running the new code (`GET /api/plugins/plugins/` reported
`"version": "0.6.4"`, `"loaded": true`) *before* attempting the race a
third time -- that attempt produced exactly the intended fixed
behavior: one call got the new retryable "already in progress" error,
the other succeeded as a genuine fresh start, `list_buffers` showed
exactly one tracked buffer, and the refused caller's own retry
correctly reattached to it (viewer count went 1 -> 2, same
`access_token` throughout) -- no orphan, no data loss, the real
end-to-end flow working exactly as designed. Cleaned up via `stop_all`
immediately after confirming; verified `list_buffers` empty again.

#### In-progress recording deleted entirely loops at the tail

**An in-progress recording deleted entirely (not just finished
normally) still loops forever at the tail, flagged from a 13th-pass
audit (2026-09-26) -- confirmed live (bounded at ~100s, not forever)
and fixed, 2026-09-30.** The 404-and-
confirmed-not-in-progress fix (an earlier pass) only recomputes
`finished` when `GetRecordingById()` itself still succeeds
(`lookupOk == true`) -- but a recording *deleted* entirely also 404s
on `GetRecordingById()`, not just the playlist, so `lookupOk` is false
and `ResolveInProgressFinished()` correctly (per its own "unknown
means don't assume finished" principle) leaves `finished` untouched.
`ReadInProgressRecordingStream()` at the tail then loops its full
catch-up budget and returns 0 on every read indefinitely, same as the
bug the earlier fix addressed, just reached via a different trigger.
Fix direction: give `GetRecordingById()` a `wasNotFoundOut` too
(mirroring `FetchRawInProgressPlaylist()`), and treat "recording 404
AND playlist 404" as a strong, safe "definitively finished and gone"
signal -- both endpoints agreeing the resource doesn't exist is
stronger evidence than either alone. Not implemented this pass: the
cleanest way to expose the HTTP status code out of `GetRecordingById()`
means either widening the shared `Request()` helper's own signature
(used by dozens of call sites) or duplicating `FetchRawInProgressPlaylist()`'s
raw-curl approach just for this one call -- a real design choice, not
a one-line change. Also relevant to the deliberately-reverted "500 ms manifest refresh throttle never engages during sustained failure" item: a "recording 404 AND
playlist 404" signal, once it exists, is exactly the kind of
definitively-terminal failure that item's own fix direction calls for.
**Live check (2026-09-30): partly confirmed, and milder than
"forever".** Two runs against the real lab. With realtime updates on,
Kodi hears about the deletion at once and simply ends playback within
~10 seconds -- no polling. With them off (the default, and the only case
where Kodi can't find out), the addon keeps hammering the gone
recording: measured through a counting TCP proxy on the Kodi VM, 766
requests over 98 seconds after the server-side delete (~7.8/s, every one
a 404, alternating the recording lookup and the HLS playlist fetch
roughly every 0.3s) against ~9.9/s of ordinary at-the-tail polling
beforehand -- so the throttle never engages on failure, as the
neighbouring entry says. It stops only when Kodi's own no-data timeout
ends playback (~100s in two runs; 101s and 109s), after which activity
is zero. So it is bounded, not infinite, but the addon never gives up on
its own: ~800 wasted 404s and ~100s of dead air instead of a clean stop.
The addon's own debug counter for successful manifest refreshes reads 0
throughout, since failed refreshes log nothing -- the proxy was needed
to see it. Fix direction unchanged: treat lookup-404 plus playlist-404
as terminal and end the read at once.
**Fixed (2026-09-30), as that fix direction says.**
`GetRecordingById()` gained an optional `httpStatusOut` (`Request()`
already had one), `RefreshInProgressRecordingManifest()` passes it to
`dispatcharr::IsInProgressContentGone()` (`RecordingVisibility.h`, 6 test
cases) and, on a yes, sets a new sticky `contentGone` flag (plus
`finished`, and drops the recording's cross-open segment cache entry).
Two situations count, both requiring that the stream already had segments
(so a young recording whose HLS directory isn't there *yet* -- the
cold-start case -- is never mistaken for a gone one) and a playlist that
definitively answered 404 or a redirect: the lookup answered 404 (deleted
outright), or the lookup succeeded and says the recording is no longer in
progress (finished, directory removed). A lookup that merely failed (5xx,
transport) stays "unknown"; one that says the recording is still in
progress never qualifies, because a naturally completing recording's status
is only written after its directory is removed and that window resolves
itself. Once `contentGone` is set, every later refresh returns at once
without touching the network and a read that needs a segment it doesn't
already hold returns EOF; a segment 404 in the read path runs one
(throttled) refresh so a viewer mid-buffer finds out too. This also
answers the "500 ms manifest refresh throttle never engages during sustained failure" entry for the one case that
entry said needed handling (a definitively terminal failure); a transient
failure still doesn't arm the throttle.
**Re-verified live (2026-09-30)** on the Linux test client with the same
counting-proxy harness as the live check above (real-time updates off, a
30-minute recording, playback seeked to the live tail, then
`DELETE`d server-side). Before: 766 requests over 98s after the delete,
all 404, and the player ended after 101s and 109s. After: exactly two
requests (the recording lookup and the playlist, both 404, 0.6s into the
delete) and then silence for the rest of the run, the addon logged
`in-progress recording ... is gone on the server (deleted)`, and the
player ended ~10s later (playing out what Kodi had already buffered).
Two variants behaved the same way: a viewer paused with unread segments
when the recording was deleted (2 requests, none during the 25s pause or
after the resume, player ended 10s after resuming) and a viewer playing
mid-recording (2 requests, ended ~10s after the delete). The
finished-and-directory-removed situation also checked out: a paused viewer
whose addon could not reach the server (the proxy was stopped) while the
recording ended, finalized and had its directory removed answered, once
the proxy came back, with one lookup and one playlist request, logged
`gone on the server (finished, and its HLS directory has been removed)`,
and ended cleanly after the resume with no further requests -- where the
unfixed build produced 63 consecutive segment 404s and a player that gave
up 17s after resume.

#### Two threads regenerating the API key at once

**Very low confidence, not reproduced or fully traced: two threads
regenerating the API key at the same time, flagged from a 13th-pass
audit (2026-09-26).** E.g. `GetStreamTimes()`'s own proactive self-heal
and the read thread's 401-triggered regeneration, both reachable during
in-progress-recording playback. Both can call `GenerateApiKey()`;
`m_config.apiKey` could end up holding whichever one the server has
already invalidated (self-healing via another round trip, so likely
low-impact even if real). Separately, interleaved
`PersistApiKeyIfChanged()` calls could in principle race Kodi's own
`api_key` setting-change notification and return a spurious
`ADDON_STATUS_NEED_RESTART` -- depends on Kodi's own notification
timing, which wasn't confirmed either way. Related to, but distinct
from, the already-logged `m_liveTimeshiftStream`/ `m_inProgressRecordingStream` locking gap ("No locking around live-timeshift / in-progress stream state") -- worth a closer look
only if this ever actually surfaces as a real symptom.
**Addressed (2026-09-30) as a side effect of the fetch-first API key
fix below:** every recovery now goes through
`DispatcharrClient::ObtainApiKey()`, serialized end to end by its own
`m_apiKeyRecoveryMutex`, so two threads can no longer each generate a
key and leave `m_config.apiKey` holding the loser's. (And with the
account's existing key adopted instead of regenerated, the racing
regenerations this entry worried about mostly stop existing at all.)
The `PersistApiKeyIfChanged()` vs. Kodi's own setting-change
notification question is unchanged and still unconfirmed either way.

#### Concurrent playback on two installs regenerating the API key

**A bigger, sharper version of `docs/TROUBLESHOOTING.md`'s own
"One install's addon can silently invalidate another install's stored API key" entry --
concurrent *active playback* on two installs sharing one Dispatcharr
account could regenerate the key several times a second, flagged from
a 44th-pass audit (2026-09-26), mechanism confirmed against
Dispatcharr's own real current upstream source, real-world failure
rate not itself confirmed live -- needs a live two-install test before
attempting the fix below, not attempted blind this pass given how many
call sites (five) already depend on the current regenerate-on-401
behavior.** `GenerateApiKey()` always calls `POST
/api/accounts/api-keys/generate/` (`APIKeyViewSet.generate()`,
`apps/accounts/api_views.py`), which replaces `user.api_key`
unconditionally -- there's no way to fetch the account's *current* key
without also replacing it. `ReadRecordingStream()`/
`ReadInProgressRecordingStream()` regenerate on every 401, once per
~256KB read; two installs both actively playing back a recording on
the same account can alternate invalidating each other's key several
times a second, not just once per restart the way the already-fixed
entry's own "self-heals within one HTTP round-trip" framing assumes.
Two concrete, confirmed-by-source consequences beyond the already-
documented "invisible to the user" self-heal: (1) every regeneration
reaches `PersistApiKeyIfChanged()` -> `SetSettingString("api_key",
...)`, and Kodi's own `CAddon::SaveSettings()` (`Addon.cpp`) writes
`settings.xml` to disk and re-delivers every setting on each call --
several disk writes a second on both devices for as long as both keep
playing (real SD-card wear risk on CoreELEC); (2) if the *other*
device regenerates between this device's own `generate/` POST and its
same-request retry GET, that retry also gets a 401, and
`ReadRecordingStream()`/`ReadInProgressRecordingStream()`'s own
already-exhausted-the-one-retry path returns -1, ending playback on
*this* device -- the window is roughly one HTTP round-trip per read,
repeated several times a second on each device, so hitting it within
minutes of two simultaneous playback sessions seems plausible, not
just theoretical (not measured). Suggested fix, needing that live
two-install test first: add a `FetchCurrentApiKey()` using the
existing, currently-unused `GET /api/accounts/api-keys/`
(`APIKeyViewSet.list()`, same file) -- on a 401 or an empty stored
key, fetch first and use the server's current key if it differs from
whatever was just rejected, only falling back to `generate/` when the
server-side key is genuinely null (or the fetch itself 404s/405s, for
an older Dispatcharr version without this endpoint) -- letting
multiple installs converge on one shared, mutually-valid key instead
of a losing race to regenerate it.
**Live two-install test (2026-09-29): confirmed real and continuous,
but less severe in its user-visible consequence than feared.**
Started the exact same completed recording (a long, multi-hour one)
simultaneously on two real Kodi clients (one Linux, one Windows),
sharing one Dispatcharr account, and tracked all three of the Linux
client's own stored `api_key`, the Windows client's own stored
`api_key`, and the server's real current key
(`GET /api/accounts/api-keys/`) in parallel. Confirmed the race is
real and ongoing, not theoretical: the Linux client's stored key
changed on every single sample over a continuous ~4.5-minute window
(roughly every 24-25 seconds, a far slower cadence than this entry's
own "several times a second" estimate -- each `ReadRecordingStream()`
call apparently reads a much larger chunk in practice than the "once
per ~256KB" framing assumed), and one direct three-way snapshot
caught the Linux client's stored key, the Windows client's stored
key, and the server's own real current key all three
*simultaneously different* -- confirming this never actually settles
into a shared, mutually-valid state while both keep playing, exactly
the "losing race" this entry already predicted. Consequence (1)
(repeated `SetSettingString`/disk writes) is confirmed real, just at
this slower ~24s cadence rather than several-times-a-second -- real
cumulative wear over a long recording's full runtime, but much less
alarming than originally estimated. Consequence (2) (exhausted-retry
playback failure) did **not** materialize in this test: both devices'
own `Player.GetProperties` stayed at `speed: 1` with position
advancing normally the entire ~4.5 minutes, no stalls, no dropped
playback, on a low-latency home LAN -- the single self-heal retry
each read already has evidently absorbs this in practice, at least at
this cadence and latency; a slower/higher-latency link could still
land inside the doc's own theorized failure window, not tested here.
Also confirmed, independently, via direct source read
(`ReadRecordingStream()`/`ReadInProgressRecordingStream()`,
`DispatcharrClient.cpp`): neither 401-triggered regenerate-and-retry
branch has a `kodi::Log()` call of its own marking the event --
confirming this entry's own "invisible to the user" framing is
accurate; the only way to observe it happening at all was polling
`settings.xml` directly from outside Kodi, not anything in its own
log. Cleaned up: stopped playback on both devices after confirming;
no lasting effect on either install beyond the key itself continuing
to churn for as long as something else keeps reading with a stale
copy (self-heals normally afterward, same as the already-documented
single-install case).
**Second symptom, reported by a user against 0.11.0, and fixed with
the rest of this entry (2026-09-30): just *enabling* the addon revoked
the account's key for every other client.** With a username/password and
an empty `api_key` setting, the constructor called `GenerateApiKey()`
unconditionally -- which overwrites the account's single key -- so every
other consumer of that account's key (scripts, other
apps) started getting `401 {"detail":"Invalid API key"}`, with nothing
logged by this addon, since from its own side the generate call
succeeded. The five 401 self-heals did the same on every recovery.
Reproduced live on the old build in both shapes before fixing (real
Kodi clients against the real lab instance, fingerprints of the key
compared before/after, never the key itself): (A) a client starting
with an empty `api_key` replaced the account's existing key; (B) after
a third party rotated the key, playing a recording replaced *that*
key too, i.e. the addon's recovery revoked the other client's brand-new
key.
**Fix: read the account's existing key first, generate only when there
isn't one.** New `DispatcharrClient::ObtainApiKey()` is now the single
entry point for every "I need a key" path (first-run setup, a stored
key belonging to a different account, `OpenRecordedStream()`'s retry,
and all five 401 sites); `GenerateApiKey()` became private and is only
ever reached as that last resort. It calls
`GET /api/accounts/api-keys/` (`APIKeyViewSet.list()`,
`apps/accounts/api_views.py`: `{"key": <str|null>}` for the caller,
permission class `Authenticated` only, so any role; confirmed against
Dispatcharr's own real current upstream source and live -- it returned
the same key `GET /api/accounts/users/me/`'s `api_key` field did, either
would have worked, the dedicated one is just smaller and purpose-built).
The pure decision core, `ApiKeyRecovery.h`
(`ParseApiKeyListResponse()`/`ClassifyApiKeyLookupFailure()`/
`DecideApiKeyRecovery()`, 19 new Catch2 cases): a present key is
adopted; `null`/`""`/whitespace, a response with no `key` member, or a
non-transient 4xx (an older Dispatcharr without the endpoint) falls back
to generating; a *transient* failure (no response, 5xx, 408, 429, an
unusable 2xx body) deliberately does **not** generate -- rotating a key
blind could revoke a perfectly good one for everyone -- and just fails
so the caller's own retry tries again later. A key equal to the one
just rejected is still adopted rather than regenerated: Dispatcharr
resolves a key per request with a plain `User.objects.get(api_key=...)`
(no caching; an inactive account also answers 401), so an equal key
means the 401 wasn't the key's fault, and the callers' existing
retry-once-then-give-up loops bound the cost of trying it once more.
Also new: `m_apiKeyRecoveryMutex` serializes `ObtainApiKey()` end to end
(see the "Two threads regenerating the API key at once" entry), and the recovery now logs -- debug
when it adopts a key, info when it has to generate one (the one event
that still revokes the account's key), warning when a lookup failed
and it declined to generate; none ever includes the key. The
`api_key` setting's help text (`strings.po` `#30034`) no longer says
regenerating is the normal path.
**Re-verified live after the fix** (rebuilt, redeployed to the real
Linux Kodi test client, everything by key fingerprint):
(A') a client starting with an empty `api_key` while the account had a
key adopted it -- server key unchanged, the client's stored copy now
equal to it, "adopted the account's existing API key (nothing
regenerated)" logged; (B') after a third party rotated the key, playing
a recording recovered by adopting the rotated key -- server key still
the rotated one, the client's stored copy updated to match, no new key
minted; (C) the fixed Linux client and the still-old Windows client
(no Windows build of the fix was available) playing the same
multi-hour recording at once, sharing one account, started with three
disagreeing keys and had all three identical within the first 5-second
sample, then stayed identical with zero changes for the remaining ~4
minutes -- versus the roughly-every-24-seconds continuous churn and the
three-way disagreement the earlier two-old-clients run showed (the one
rotation in the run came from the old Windows build; the fixed client
adopted it instead of answering with its own); (D) with the account's
key revoked outright (`POST .../revoke/`), playback recovered and the
addon logged "the account has no API key yet ... generating one",
the server, and the client's stored copy, ending up equal -- the
fallback still works; (E) a real, still-recording one-off recording
(created on a working channel, played while in progress, deleted
afterward -- 204 then 404) with the key rotated by a third party
*mid-playback*: the in-progress paths (`RefreshInProgressRecordingManifest()`'s
proactive check and the playlist/segment 401 sites, which the plain
smoke run skips when nothing is recording) adopted the rotated key with
no third key and playback never stalled. A full `tools/kodi_smoke_test.py`
run also passed (13 passed, 0 failed, 2 skipped -- the two in-progress
checks, nothing recording at that moment). Not exercised live: the
transient-failure "decline to generate" branch and the older-Dispatcharr
fallback (both need a server that misbehaves on demand) -- unit-tested
only. One more real limit, inherent rather than fixable here: an
account with no key at all still gets one generated on first use (there
is nothing to adopt), and any *other* client still running the old
generate-on-401 behavior can still revoke the shared key -- the fixed
client just no longer answers in kind.
**Both clients on the fixed build (2026-09-30).** A Windows build of the
fix was then made by hand on the Windows runner, following
`build.yml`'s Windows job step by step from a short path (it compiled
cleanly under MSVC; the ~20-minute CMake re-configure the packaging
target kicks off was skipped and the freshly linked DLL deployed
directly, after confirming the new log strings are inside it), so the
Linux and Windows clients could be tested together on one account:
(G) both started at the same moment with an empty `api_key` -- both
logged "adopted the account's existing API key" about half a second
apart, the server key untouched, both stored copies equal to it; (F)
both playing the same recording while a third party rotated the key
three times (60s, 120s, 180s) -- after every rotation both clients had
matched the new key within one ~6-second sample, and across the whole
run the server never held a key other than the ones the script itself
rotated in, i.e. neither client generated anything, with playback
running throughout; (H) the account's key revoked, then both clients
started playing -- in three of four runs (including two that opened
both players at the same instant) exactly one client generated the key
and the other simply adopted it. In the fourth run both clients opened
within about 3 ms of each other, both found no key, and both generated:
the later generate replaced the earlier one's brand-new key, so the
loser's retry got a 401 and its *first open failed once* ("Playback
failed"), while the winner opened fine. The loser recovered on its very
next use (adopted the server's key, nothing generated), and everything
converged to one key. That window only exists while the account has
*no* key at all (once anyone has generated one, everyone adopts).
**Narrowed the same day (2026-09-30).** `ObtainApiKey()` now re-reads the
account's key right after a generate and switches to it if it differs
from the one just generated (`ReconcileGeneratedApiKey()`,
`ApiKeyRecovery.h`; 7 new Catch2 cases; info-logged as "another client
generated an API key at the same moment -- using the account's current
key instead of the one just generated"). What that does and doesn't do:
it resolves any collision where the other client's generate lands before
the re-read -- the near-simultaneous case that actually occurs -- but it
only *moves* the remaining window rather than closing it: another
client's generate landing between the re-read and the retry, about one
round trip later, still costs that one open (mid-playback the 401
recovery paths absorb the same thing without a visible failure). Giving
the 401 sites a third attempt is the direction that would remove it
outright; not done.
Re-verified live with both clients rebuilt with it (the Windows one
rebuilt incrementally by hand on the runner, about 20 minutes for the
five changed files): revoke the account's key, then open a recording on
both clients at the same instant (separate threads), repeated 9 times.
In 7 of the 9 exactly one client generated and the other simply adopted.
In the other 2 both clients found the account keyless and both
generated -- once the Windows client reconciled to the other's key (its
log shows "generating one" and then the reconcile line 23 ms later),
once the Linux client did -- and in both, both players opened first
time, and the server and both clients ended on the same key with
nothing changing afterward. That is 2 real collisions with 0 failed
opens, against the 1 collision in 3 comparable trials that failed one
open before the change (a small sample, but the previously failing
shape now resolves in both orderings). A full `tools/kodi_smoke_test.py`
run on the rebuilt Linux client still passes (13 passed, 0 failed, 2
skipped). Worth knowing when re-running that script on these two test
clients: long playbacks leave Kodi resume bookmarks on some recordings,
and its default recording pick can land on one -- the resume prompt then
stalls `Player.Open` and fails the recorded-playback checks (seen once
here, confirmed by the addon log showing no open call at all); pass
`--recording-id` for a bookmark-free one.

#### Series rule edit resets fields the addon never reads or sends

**Editing a series rule from Kodi may silently reset fields the addon
never reads or sends, flagged from a 16th-pass audit (2026-09-26),
confirmed live and fixed 2026-09-29 -- needs a live check against Dispatcharr's own server code -- not
available in this workspace, so not confirmed either way.**
`DispatcharrClient::CreateSeriesRule()` sends only `channel_id`/`title`
(now the resolved match-title, see the fix this same pass shipped),
optionally `tvg_id`, and `mode` (only when it's `"new"`);
`ParseTimerRuleJson()` reads only `mode`/`title`/`tvg_id`/`channel_id`
back; `DeleteSeriesRule()` sends `title`+`tvg_id` and never
`epg_source_id`.
`UpdateTimer()`'s own comment already documents that re-POSTing edits
"mode/title_mode/description/etc. of the existing rule in place" --
describing a replace-style upsert -- and `docs/API_NOTES.md`
separately documents that `title_mode`/`description`/`description_mode`/
`untagged_is_new`/`epg_source_id` all exist on the real request/rule
shape. If Dispatcharr's own serializer treats an omitted field on that
upsert as "reset to default" rather than "leave unchanged" (the exact
same class as the already-fixed `dvr_settings` "PATCH replaces, doesn't
merge" incident), then ANY edit from Kodi -- even a plain
enable/disable-equivalent toggle -- would silently reset a rule's own
`title_mode`/`description`/`untagged_is_new` filters back to their
server-side defaults if they'd been customized outside this addon
(e.g. via Dispatcharr's own web UI). Separately, `epg_source_id`
specifically (source-pinning, added in Dispatcharr v0.30.0 per
`docs/RECORDINGS.md`'s own "wrong EPG copy" entry) is never sent or
read at all -- for a rule pinned to a specific EPG
source, an edit from Kodi could create a second, unpinned duplicate
rather than editing the pinned one (the upsert identity wouldn't
match), a delete could silently match nothing, and two rules sharing
`title`+`tvgId` but differing only in `epg_source_id` would hash to
the same Kodi `ClientIndex` (`ComputeSeriesRuleClientIndex()` only
hashes `title`+`tvgId`). Needs a live check first: create a rule in
Dispatcharr's own UI with a non-default `title_mode`/`description` and
a pinned source, edit it from Kodi, then GET the rules list to see
what actually changed. If confirmed, the fix is to round-trip these
fields explicitly (always send `mode`, echo back cached
`title_mode`/`description`/`description_mode`/`untagged_is_new`/
`epg_source_id`, include `epg_source_id` in the DELETE call and in the
`ClientIndex` hash) -- a real behavior change needing live
verification, not a blind fix.
**Update (2026-09-26, a 17th-pass audit): confirmed against
Dispatcharr's own current upstream source (downloaded to a scratchpad
for review, not committed to this repo -- stronger than guessing from
the API shape, but not the same standard as a live test against a
real running instance), and one part of the original guess corrected.**
`SeriesRulesAPIView.post()` (`apps/channels/api_views.py`) confirms the
replace-style upsert exactly as suspected: it builds a fresh
`rule_record` from the request (defaults: `mode` "all", `title_mode`
"exact", `description_mode` "contains"; `untagged_is_new`/`epg_source_id`
are included only if the request actually sent them) and, on a match,
runs `existing.clear(); existing.update(rule_record)` -- so any Kodi
edit genuinely does reset `title_mode`/`description`/`untagged_is_new`/
`epg_source_id` to these defaults if the request omits them (as this
addon's own request always does for all but `mode`/`title`/`tvg_id`).
One silver lining: "always send `mode`" turns out unnecessary --
omitting it already resets to the correct default ("all") when a user
turns "new only" off from Kodi, so that specific toggle already worked
by coincidence.
**The delete-side guess was backwards -- corrected here, not just
confirmed.** `SeriesRulesAPIView.delete()`'s own `_matches()`
(same file) only restricts by `epg_source_id` when the *request*
supplies one (`if epg_source_id is None: return True`) -- since
`DeleteSeriesRule()` never sends it, a delete from Kodi doesn't
"silently match nothing" as originally guessed; it matches and removes
**every** rule sharing that `title`+`tvg_id`, regardless of each one's
own `epg_source_id`. Deleting one Kodi timer for a source-pinned series
could delete every other source's copy of that same rule (and, per
`delete()`'s own follow-up call to `future_recordings_for_series()`,
their future recordings too) in one action -- a real, if
narrow-audience (multi-EPG-source-per-tvg_id setups only), data-loss
risk once `epg_source_id`-pinned rules are in play. This raises the
priority of the "include `epg_source_id` in the DELETE call" half of
the fix direction above specifically, though it still isn't
implemented blind -- the live check above remains the right next step
before shipping a fix for either half.
**Update (2026-09-26, a 25th-pass audit): the "reset fields to
defaults" framing above is only half the story for a source-pinned
rule -- it doesn't even get *matched*, so Kodi's own edit appends a
brand-new, unpinned duplicate instead of touching the pinned rule at
all, confirmed against Dispatcharr's own real current upstream
source.** `SeriesRulesAPIView.post()`'s own upsert match requires
`parse_optional_epg_source_id(r.get("epg_source_id")) == incoming_source`
-- since `CreateSeriesRule()` never sends `epg_source_id`,
`incoming_source` is always `None` from this addon, so this exact
match fails against any rule that genuinely has one. The *only*
fallback match (`existing is None and incoming_source is not None`)
requires `incoming_source` to be non-`None` too -- so it never fires
for this addon's own requests either. Net effect: editing (or simply
re-saving via any Kodi timer action) a source-pinned series rule
doesn't reset its fields, it silently creates a second, unpinned
duplicate rule alongside the original untouched one. Both rules then
hash to the same Kodi `ClientIndex` (`ComputeSeriesRuleClientIndex()`
only hashes `title`+`tvgId`, not `epg_source_id`), so a later Kodi
delete removes both (already noted above for the delete side
specifically). **This also raises the priority of the whole item**:
Dispatcharr's own "Record series" guide button (`Guide.jsx`) sends
`epg_source_id` whenever the channel has EPG data at all -- so a
source-pinned rule is the *common* case for anything created via
Dispatcharr's own UI, not a multi-EPG-source niche as originally
framed. Fix direction unchanged (round-trip `epg_source_id` through
create/update/delete/`ClientIndex`), still gated on the same live
check before shipping.
**Live check (2026-09-29): the `title_mode`/`description` reset half
confirmed live, exactly as predicted, not just from source reading.**
Created a real (clearly-marked test-only, no `tvg_id`/`channel_id`)
series rule via `POST /api/channels/series-rules/` with non-default
`title_mode: "contains"`, `description: "a real test description"`,
`description_mode: "regex"`, confirmed those values stuck via a
fresh `GET`, then re-`POST`ed the exact same minimal shape
`CreateSeriesRule()` itself always sends (title only, in this case).
The very next `GET` showed `title_mode` reset to `"exact"`,
`description` wiped to `""`, and `description_mode` reset to
`"contains"` -- a genuine live round-trip through the real server,
not a simulation. Cleaned up (deleted) the test rule afterward;
confirmed no real rule was touched. Did not chase down a real
`epg_source_id` value this pass to also live-test the
duplicate-rule-creation half (the same root cause, already confirmed
from source in the 25th-pass update above) -- the `title_mode`/
`description` result alone is enough to close the "needs a live
check" gate this item's original flag asked for. Still not fixed --
this pass was live confirmation only, and the fix itself (round-trip
every field this addon currently drops) remains a real behavior
change to design carefully, not a blind patch.

**Fixed and re-verified live, 2026-09-29, same day.** Implemented
exactly the fix direction flagged above: `TimerRule` (`DispatcharrClient.h`)
gained `titleMode`/`description`/`descriptionMode`/`untaggedIsNew`/
`epgSourceId`, populated by `ParseTimerRuleJson()` from the same
fields `GetTimerRules()` already fetches -- so this addon now caches
every field Dispatcharr's own upsert can silently reset, not just the
four it already used. `CreateSeriesRule()`/`BuildSeriesRuleRequestBody()`
(`TimerRequestBuilder.h`, with new Catch2 coverage) gained matching
parameters: `AddTimer()`'s create path passes empty/zero sentinels
(nothing cached yet to echo, unchanged from before this fix), while
`UpdateTimer()`'s edit path now looks up the cached `TimerRule` (via
the existing `FindSeriesRuleIndexByClientIndex()`) and echoes its
`titleMode`/`description`/`descriptionMode`/`untaggedIsNew`/
`epgSourceId` straight back on every edit -- none of these five have
any Kodi-side UI of their own, so echoing the last-known cached value
is the only correct behavior. `DeleteSeriesRule()`/
`BuildSeriesRuleDeleteQuery()` gained `epgSourceId` too, closing the
companion data-loss bug from the 25th-pass update (a delete used to
remove every EPG source's own copy of a rule sharing title+tvgId).
`ComputeSeriesRuleClientIndex()` (`TimerIdentity.h`) now hashes
`epgSourceId` alongside `title`/`tvgId` (normalizing any non-positive
value to a single canonical "unpinned", matching Dispatcharr's own
`parse_optional_epg_source_id()`), so two rules sharing a title+tvgId
but pinned to different sources finally get distinct `ClientIndex`
values instead of colliding onto one.
Re-verified live against the real lab instance and a real Kodi
client: created a real series rule via `PVR.ToggleTimer` (exercising
the actual `AddTimer()` path), confirmed it landed server-side with
the expected default shape and appeared correctly in Kodi's own
Timers list (including the new rule and its already-matched recordings) -- a genuine end-to-end
regression check for the ordinary, non-source-pinned case, since this
change touches the exact `ClientIndex`/upsert machinery every series
rule goes through. Cleaned up afterward (a direct API `DELETE`, since
`PVR.ToggleTimer` against an already-existing rule re-created rather
than deleted it -- a Kodi-side timer-state staleness quirk, not
something this fix touches), confirming Kodi's own timer count
returned to exactly its baseline. Did not attempt a full
GUI-driven edit of an *existing* rule live (no `PVR.UpdateTimer`
JSON-RPC method exists to trigger one without blind GUI automation,
and `PVR.ToggleTimer` against an existing rule deletes rather than
edits it) -- confidence in the edit path itself instead comes from
direct, passing Catch2 coverage of the exact functions `UpdateTimer()`
calls with the exact parameters it would pass, plus the already-live-
confirmed server-side mechanism this whole fix targets. Full C++
suite (638 test cases) passes; a real build through the actual Kodi
`tools/depends/target/binary-addons` harness compiled clean.

#### Recurring rule on a DST-transition start date drifts an hour per edit

**Suspected: a recurring rule whose `start_date` lands on a DST
transition day drifts by an hour on every Kodi edit, flagged from a
19th-pass audit (2026-09-26), simulated against this addon's own real
`RecurringRuleUtil.cpp`/`TimeZoneUtil.cpp`/`DateTimeFormat.cpp`
compiled into a scratch harness, not reproduced live.** `GetTimers()`
resolves the auto-detected zone's UTC offset at `rule.startDate`
(UTC midnight); `UpdateTimer()`'s own `ComputeRecurringRuleFields()`
resolves it at the timer's actual start instant instead. The two
disagree whenever a DST transition falls between UTC midnight on
`startDate` and the real start time -- true for any rule whose
`start_date` is the zone's own transition date, with a start time
after the switch. Simulated: an America/New_York rule with
`start_date` 2026-03-08 (a spring-forward date) stored at 20:00 UTC
displayed as 21:00, and re-derived +1 hour on every subsequent edit
(including a bare enable/disable toggle) -- 20→21→22→23 across
successive edits; a Europe/London rule on 2026-10-25 (a fall-back
date) drifted -1 hour per edit instead, with `start_date` itself also
moving back a day. A manual (non-auto-detected) offset is unaffected,
since it's a fixed value, not resolved from a zone table at either
point in time. Only reachable for a rule whose `start_date` happens to
land exactly on a transition day -- rare, but not exotic (every zone
observing DST has two such dates a year). Fix sketch: resolve the
offset via a fixed-point iteration (offset at `startDate` → tentative
start instant → offset at *that* instant → recompute), capped at a
couple of iterations for the one-hour spring-forward gap; would need
`ComputeRecurringRuleDisplayTimes()` to take an offset-resolver
callback rather than a plain `offsetMinutes` int to be unit-testable.
Not implemented blind -- the DST edge itself is confirmed by
simulation against this addon's own real code, but the actual on-device
symptom (does Kodi's own display genuinely drift the way the simulated
values suggest) isn't.
**Live check (2026-09-29): confirmed, and worse than originally scoped
-- wrong from the very first read, not just "on every edit."** Created
a real recurring rule via a direct API round trip against the real lab
instance --
`start_date` on that zone's fall-back date (so the zone's offset at UTC midnight of that date differs from its offset later that day),
`start_time` "03:00:00" (chosen to fall on the post-transition/standard-time
side of the local clock, so the two DST-resolution instants this
entry describes genuinely disagree). Confirmed picked up by the real,
currently-installed addon (`0.11.0`) via `PVR.GetTimers`: displayed
`starttime` came back one hour earlier than the rule's own 03:00 local
start. Traced this exactly
against the real formula (`ComputeRecurringRuleDisplayTimes()`,
`RecurringRuleUtil.cpp`: `startDate + startTimeOfDaySeconds -
offsetSeconds`) using the offset `EffectiveRecurringRuleUtcOffsetMinutes(rule.startDate)`
actually resolves (the DST offset, since `rule.startDate`'s own UTC
midnight is still pre-transition) -- reproduces the displayed value exactly,
confirming Kodi's own JSON-RPC `PVR.GetTimers` reports this addon's
raw computed epoch verbatim (also independently confirmed against an
ordinary, non-DST-day rule already on this same instance: its
displayed value matched the same raw-epoch formula, not a
zone-localized one, ruling out a display-layer confound). The
correct epoch (resolving the offset at the real start instant instead,
the standard-time offset, matching Dispatcharr's own actual scheduling intent) is
one hour later -- a genuine, confirmed, live 1-hour-early
drift, in the predicted fall-back direction and magnitude. The one
correction to this entry's own original scoping: this is wrong on the
very first `GetTimers()` read of a freshly server-created rule,
before this addon has ever sent an edit for it at all -- `GetTimers()`
resolves this same offset unconditionally on every call, not only in
response to an edit round-trip, so "drifts on every edit" undersold
how immediately this bites. Cleaned up: deleted the test rule
(`ZZZ_TEST_DST_RULE_DO_NOT_USE`) via the API immediately after
confirming the display value; verified it's gone from a fresh `GET`
of the real instance's recurring-rules list (only the pre-existing
real rules remain).
**Fixed and re-verified live, 2026-09-29.** Implemented the fix sketch
above: `ComputeRecurringRuleDisplayTimes()` (`RecurringRuleUtil.h`/
`.cpp`) now takes a `resolveOffsetMinutes` callback
(`std::function<int(time_t)>`) instead of a single pre-resolved
`offsetMinutes` int, and resolves it via a fixed-point iteration:
start from the offset at `startDate`'s own UTC midnight, tentatively
convert, re-resolve at that tentative result, and repeat (capped at 2
refinements -- converges immediately for the simple +/-1h DST deltas
this addon ever computes, see `TimeZoneUtil.cpp`). `GetTimers()`'s own
call site (`PVRDispatcharr.cpp`) now passes
`[this](time_t at) { return EffectiveRecurringRuleUtcOffsetMinutes(at); }`
instead of a single `EffectiveRecurringRuleUtcOffsetMinutes(rule.startDate)`
value. `AddTimer()`/`UpdateTimer()`'s own edit-path
`ComputeRecurringRuleFields()` needed no change -- confirmed by
tracing it directly: it already resolves at `timer.GetStartTime()`, a
real absolute instant handed in directly, not a startDate+time-of-day
combination that needs resolving before it's even known which side of
a transition it lands on, so it never had this specific chicken-and-egg
problem to begin with. Added two new dedicated tests
(`tests/test_recurring_rule_util.cpp`) exercising the fixed-point
resolution itself against a synthetic single-transition resolver
(mirroring a DST zone's fall-back numbers, just with a
small synthetic transition instant): one confirming the refinement
actually fires and lands on the correct, converged value rather than
the naive single-shot one, one confirming the no-refinement-needed
case still short-circuits correctly. Every pre-existing test in that
file updated to pass a fixed-offset resolver lambda in place of the
old plain int. Full suite still green (1179 assertions, 640 test
cases). Re-verified live end-to-end: rebuilt the addon against Kodi's
own build harness (`~/kodi-build`, clearing the documented
`.installed-native`/`ExternalProject` stale-marker gotcha first, see
`docs/BUILDING.md`), redeployed the rebuilt `.so` to the real Kodi
test client, restarted it, and recreated the *exact same* test rule
(`ZZZ_TEST_DST_RULE_DO_NOT_USE`, `start_date` the same fall-back date, `start_time`
03:00:00) against the real lab instance again. `PVR.GetTimers` now
reports the rule's correct start (03:00 in its own zone), not the
hour-early value from before the fix. Deleted the test rule again
immediately after confirming; verified gone from a fresh `GET` (only
the pre-existing real rules remain).

#### Series rules with non-exact title_mode never link to their recordings

**Confirmed from Dispatcharr's own current upstream source, and
then live: a series rule created with `title_mode` other than exact
("contains", "search", "regex", or description-only with an empty
title) never links back to its own materialized recordings in Kodi,
reproducing the pre-fix "12/31/1969" epoch display bug through a path
the case-sensitivity fix (see `docs/RECORDINGS.md`) didn't cover,
flagged from a 19th-pass audit (2026-09-26) -- fixed for every mode
except regex, 2026-09-30.** `SeriesRuleMatching.cpp`'s
`MatchRecordingsToSeriesRules()` compares
`ToLower(rule.title) == ToLower(rec.title)` unconditionally --
correct for Dispatcharr's own default "exact" mode (confirmed
case-insensitive server-side, `title__iexact`), but a rule created via
Dispatcharr's own web UI with a non-exact `title_mode` never has that
relationship at all: its own materialized recordings only carry a
`program` snapshot, with no back-reference to the rule that created
them, so an exact-string comparison against a "contains"/"search"/
"regex" rule's own (partial, or entirely absent for a description-only
rule) title reliably fails to match anything. `TimerRuleParser.cpp`
also never reads `title_mode` from a series rule's own JSON at all
today, so this addon has no way to even distinguish the two cases yet.
Possible partial fix: read `title_mode`, and for `"contains"`
specifically, match via a case-insensitive substring test instead of
exact equality (matching Dispatcharr's own likely semantics for that
mode, not independently confirmed against its actual matching code
this pass). `"search"`/`"regex"` would need real parity with
Dispatcharr's own `parse_text_query()`/Python-regex evaluation to
match correctly and weren't looked at closely enough this pass to
design a fix for; leave those two modes unmatched (today's status quo)
rather than guessing.
**Live check (2026-09-30): confirmed.** A disposable `title_mode:
contains` rule ("ZZZ_TEST Alpha") beside a future recording titled
"ZZZ_TEST Alpha Report" on the same channel, with an exact-mode control
rule for that full title. Kodi links the control to its recording (shows
the recording's start time) but shows the `contains` rule as "Any day at
any time" despite the matching recording -- the graceful display since
the epoch fix, but still unlinked. No evaluation was triggered (the test
titles match nothing in the real guide), so nothing real was
materialized; the addon's matching is purely client-side, so none was
needed.
**Fixed (2026-09-30)**, after reading the rest of the server's side of
this rather than guessing at it (`_evaluate_series_rules_locked()`,
`apps/epg/query_utils.py`, Dispatcharr 0.31.0 -- the same version as the
lab). What the source actually says is more than the entry above
assumed: only `"exact"` is `title__iexact`; *every other* title mode,
including `"contains"` and any name the server doesn't know, goes through
`parse_text_query()` -- an `icontains` with an AND/OR/double-quote/
parenthesis grammar, applied strictly left to right, with `"search"`
anchoring each term on a regex word boundary and `"regex"` handing the
whole value to PostgreSQL as a regex. A rule's description is always run
through the same parser (there is no "exact" for it), and its title and
description filters are ANDed. And the materialized recording carries
only a `program` snapshot (title, description, tvg_id, epg_source_id),
never a reference to the rule, so the only way to link them is to
re-evaluate the rule against that snapshot.
`MatchRecordingsToSeriesRules()` now does that, through two new pure
modules: `SeriesRuleTextMatch` (`MatchSeriesText()`: exact, contains and
search, a faithful port of `parse_text_query()` quirks included --
operators only as a whole " AND "/" OR " with a space each side, a quote
only atomic when it is a whole operand, the parser's last-"("/first-")"
pairing that leaves a stray ")" behind a nested group, an empty operand
that is neutral rather than match-everything, and `\y` as a true word
boundary so "C++" only matches where a word character follows) and
`UnicodeText` (below). A regex filter is deliberately *not* evaluated --
PostgreSQL's regex syntax isn't ECMAScript's -- so that rule stays
unlinked, exactly as it was, rather than being guessed at; the
description filter is evaluated too, so a description-only rule (empty
title) now links by its description and a rule whose description doesn't
match its recording no longer links by title alone. The port was checked
against the real parser, not just against its documentation: Dispatcharr's
own `parse_text_query()` was loaded unmodified with Django's `Q` replaced
by a small fake that keeps `Q._combine()`'s empty-operand rule and
evaluates `icontains` with glibc's `towupper()` (what PostgreSQL's
`UPPER()` does), and ~108,000 random query/text pairs across the three
modes -- operators in mixed case, quoted phrases, nested/unbalanced
parentheses, empty operands, Unicode whitespace, accented and Greek and
Cyrillic text -- produced **zero** disagreements with the C++ port; 110 of
them, curated by feature, are now `tests/test_series_rule_text_match.cpp`'s
table. One divergence is known and deliberately not reproduced: the
server finds the operators in `text.upper()` and slices the original text
at those positions, so a query with a character whose uppercase form is
longer (a German sharp s) ahead of an operator splits in the wrong place
there.
**Re-verified live (2026-09-30)** on the Linux test client, real Kodi
against the real instance: eleven disposable rules pinned to one real channel
beside eight future recordings with chosen titles and descriptions. Every
one came out as predicted -- the contains-mode rule from the live check
above, a description-only rule, a whole-word search rule ("newsroom")
and a contains rule with AND all show their recording's start time, while
a search rule whose word only ever appears inside longer words, a contains
rule whose second AND term is absent, an exact-title rule whose
description filter doesn't match, and a regex rule all still read "Any day
at any time". (A first attempt had two rules legitimately matching the
same recording; the first in list order claims it, the same first-match-
wins the existing matching has always had, so the second correctly showed
nothing -- a flaw in that run's design, not in the matching.) Creating a
rule through the API does not evaluate it, so nothing real was scheduled;
the disposable rules and recordings were deleted afterward and the
instance's original rules left untouched.

#### Channels sharing a channel number get each other's EPG

**Two channels sharing the same effective channel number get each
other's EPG merged together -- confirmed live 2026-09-28, fixed and
re-verified live the same day.** Originally flagged from a 20th-pass audit (2026-09-26)
against Dispatcharr's own real current upstream source
(`ChannelSerializer`'s own help text states outright that "duplicate
channel_number values across channels are permitted," and
`apps/output/epg.py`'s `generate_epg()` only disambiguates a collision
when the export has a real `user` -- Dispatcharr's XC-client-only
remapping -- so this addon's own unauthenticated `/output/epg` fetch,
which has no user, exports two colliding channels both under the
identical `<channel id="N">`).
**Live check: this is not rare.** The real lab instance's own channel
list has many distinct channel-number collisions right
now, every single one following the same pattern -- a live, currently-
scheduled event-style channel from one provider group colliding with
an "Offline"/placeholder-style channel from a different provider
group that apparently auto-numbers independently into the same range.
Pulled the real `/output/epg` XMLTV feed (the exact endpoint this
addon fetches) and confirmed the predicted shape directly: two
separate `<channel id="N">` tags for one number (different `<display-name>`s,
no disambiguation at all), with real `<programme channel="N">`
entries attached to that one shared id. `XmlTvParser`'s own channel-
id-keyed programme map has no way to tell those programmes apart
by which of the two `<channel>` tags "really" owns them, so
`PVRDispatcharr::GetEPGForChannel()`'s `m_epgByChannelNumber[to_string(channelNumber)]`
lookup would hand the exact same guide to both Kodi channels
-- the live-scheduled one (correctly) and the "Offline" one
(incorrectly, showing a full day's schedule for a channel that has
nothing actually playing on it). Confirmed via the real exported XML
content itself, not just re-derived from source reading. A channel
with a genuinely null number is exported keyed by `str(channel.id)`
instead, which can itself collide with a different, numbered channel
whose own number happens to equal that id -- not separately checked
this pass (none of the real channels checked had a null
`channel_number`, so this sub-case stayed untested here). Possible
fix unchanged from the original flag: a pure
`FindAmbiguousEpgKeys(channels)` to detect the collision up front and
skip/log rather than silently show merged guide data for those
channels.

**Fixed and re-verified live, 2026-09-28, same day.** Added
`dispatcharr::FindAmbiguousChannelNumbers()` (`ChannelRenumbering.h`,
with its own Catch2 coverage, named after the original flag's
suggestion but placed alongside `HaveChannelNumbersChanged()` since
the two are already cross-referenced there) and a new
`PVRDispatcharr::m_ambiguousChannelNumbers` set, recomputed from
scratch alongside `m_channels` itself in `EnsureChannelsLoaded()`.
Both `GetEPGForChannel()` and `ResolveRecordingBroadcastId()` now
check membership before trusting `m_epgByChannelNumber` for a
channel -- an ambiguous channel now reports plain "no guide data"
(matching the existing null-`channelNumber` convention) instead of a
possibly-merged one, on both sides of the collision, not just
whichever channel happens to be "real." `GetEPGForChannel()` also
logs a debug line identifying which channel/number triggered this,
gated on the addon's own `debug_logging` setting so it doesn't spam a
user who hasn't opted into verbose logs.
Re-verified live against the real lab instance: the original
collisions had already dropped to a handful by the time of the fix (these are
dynamically-numbered PPV/event-style channels that rotate over time,
not a fixed set) -- picked one of the still-live collisions (two
channels both mapped to one channel number) and confirmed the exact
predicted bug first, pre-fix: one channel's own guide was showing the
*other* colliding channel's real programme titles verbatim, not just
theoretically mergeable. After deploying the fix and forcing Kodi to
rebuild its own EPG cache from scratch (a plain restart wasn't enough
-- Kodi caches EPG data locally and doesn't eagerly re-poll an
already-populated channel), both colliding channels correctly
reported empty guides (`PVR.GetBroadcasts` returning `total: 0`), and
a real debug log line fired for each: `GetEPGForChannel: channel
<id>'s own number (<number>) is shared by another channel on this
Dispatcharr instance -- refusing to guess which channel's guide data
is real, reporting no guide data for this channel instead`. A normal,
non-colliding channel (its real guide entries) was completely
unaffected -- confirmed no regression. Full C++ test suite (627 test
cases as of this fix) passes.

#### Catch-up disabled for the current user silently ignored

**Catch-up disabled for the current user is silently ignored --
confirmed live end-to-end, 2026-09-28, fixed and re-verified live the
same day.** Originally flagged from a
20th-pass audit (2026-09-26) against Dispatcharr's own real current
upstream source: `apps/timeshift/api_views.py`'s catch-up endpoint
returns 403 "Catch-up is disabled" when either `system_settings.
catchup_enabled` is false or the current user's own
`custom_properties.catchup_enabled` is false (`apps/channels/utils.py`'s
`is_catchup_enabled`) -- but `GetChannels()`'s `SetHasArchive(ch.
catchupEnabled)`, `IsWithinCatchupWindow()`, and
`GetEPGTagStreamProperties()` all ignore both flags entirely, so Kodi
keeps advertising archive/playable guide entries that fail outright
the moment either is off.
**Live check (the per-user flag half): confirmed exactly as
predicted.** Both field names/shapes are real and exactly as guessed --
`GET /api/core/settings/` id 16 (`system_settings`) has a plain
`catchup_enabled` boolean, and `GET /api/accounts/users/` shows
`custom_properties.catchup_enabled` per-user, both `true` by default
on the real lab account. Flipped only the per-user flag to `false`
(leaving the global one alone, since that would affect every real
user on the instance) and drove a real end-to-end attempt through a
real Kodi client: picked an already-aired programme
(`is_catchup=true` on its channel, well inside its `catchup_days`
window) from `PVR.GetBroadcasts`, then `Player.Open` on that
`broadcastid`. Result, straight from `kodi.log`: `failed to create
catch-up session: Dispatcharr returned HTTP 403: {"error":"Catch-up
is disabled"}` immediately followed by Kodi's own generic
`GetEpgTagStreamProperties: Add-on 3 returned an error: the command
failed` -- `Player.GetActivePlayers` confirmed nothing ever actually
started playing. Exactly the predicted failure mode: Kodi still
offered "Play" for that programme with no indication catch-up was
blocked account-wide, and the only trace of what actually went wrong
is that one line in a log file an ordinary user never opens. Setting
restored to `true` immediately afterward and re-confirmed working
(`POST /api/catchup/sessions/` back to a real `201`). The global-flag
half (`system_settings.catchup_enabled`) wasn't independently toggled
this pass (didn't want to disable catch-up instance-wide even
briefly) -- same server-side code path per Dispatcharr's own source,
so treated as confirmed by the same mechanism, not separately
live-tested. Relatively cheap to wire up -- this addon already
fetches a `system_settings` CoreSettings row (`GetSystemTimeZone()`)
and the current user (`IsCurrentUserAdmin()`) at startup, so both
flags are close to already in reach.

**Fixed and re-verified live, 2026-09-28, same day.** Added
`dispatcharr::ShouldOfferCatchup()` (`EpgTagUtil.h`, with its own
Catch2 coverage) folding the channel's own `catchupEnabled` together
with two new cached flags on `PVRDispatcharr`
(`m_catchupEnabledGlobally`/`m_catchupEnabledForCurrentUser`, fetched
once at construction via two new `DispatcharrClient` methods,
`IsCatchupEnabledGlobally()`/`IsCatchupEnabledForCurrentUser()` --
the latter reusing the exact `/api/accounts/users/me/` response
`IsCurrentUserAdmin()` already fetches, just reading
`custom_properties.catchup_enabled` from it too). Wired into all
three call sites the original flag named: `GetChannels()`'s
`SetHasArchive()`, `IsEPGTagPlayable()` (via its existing
`IsWithinCatchupWindow()` call, now given the combined flag instead
of the channel's own raw one), and `GetEPGTagStreamProperties()`,
which now also has its own early-exit check (with a `QueueNotification`
explaining why) as defense in depth for a stale cached "is playable"
decision, rather than only relying on `IsEPGTagPlayable()` upstream.
Both new cached flags default `true` (fail open) on a fetch error,
matching `dispatcharr_is_admin`'s own established reasoning --
Dispatcharr's own 403 stays the authoritative enforcement regardless.
Re-verified live against the real lab account, both directions: with
the per-user flag disabled again and Kodi restarted (these flags are
startup-only, matching the admin-check sync's own convention -- a
mid-session Dispatcharr-side change needs a restart to take effect),
`PVR.GetChannelDetails` now correctly reports `hasarchive: false` for
a catch-up-capable channel, and attempting `Player.Open` on an
already-aired broadcast's `broadcastid` now gets rejected by **Kodi's
own core** before ever reaching this addon at all
(`{"error":{"code":-32602,"message":"Invalid params."}}` -- confirmed
via `kodi.log` that neither `IsEPGTagPlayable`'s nor
`GetEpgTagStreamProperties`'s own log lines appear for this attempt,
a strictly better outcome than the original fix even anticipated,
since `GetEPGTagStreamProperties()`'s own new early-exit branch never
even needed to run). With the flag restored to `true` and Kodi
restarted again, `hasarchive` reports `true` again and real catch-up
playback was re-confirmed working end-to-end (a real
`inputstream.ffmpegdirect` session, real active player). The
global-flag half (`system_settings.catchup_enabled`) still wasn't
independently toggled live (same reasoning as before -- didn't want
to disable catch-up instance-wide even briefly), but shares the exact
same `ShouldOfferCatchup()` code path as the per-user flag, which was
live-verified.

#### Every catch-up play creates two Dispatcharr sessions

**Every catch-up play creates two Dispatcharr catch-up sessions,
confirmed against Kodi's own real current SDK source (a 48th-pass
audit, 2026-09-27), not reproduced live.** `CPVRGUIActionsPlayback::PlayEpgTag()`
(`PVRGUIActionsPlayback.cpp`) calls `GetEpgTagStreamProperties()` once
just to check `EPGPlaybackAsLive()`, then calls `StartPlayback()`,
which calls it again (`PVRPlaybackState.cpp`) -- so this addon's own
`GetEPGTagStreamProperties()` POSTs `/api/catchup/sessions/` twice for
every single catch-up play, each POST able to block for up to
`timeout` (30s default) plus a possible login. The first session is
never actually used and just sits in Dispatcharr's own Redis store
until its 60-second handshake expiry (`HANDSHAKE_TTL_SECONDS`,
`apps/timeshift/sessions.py`) -- harmless server-side, but it doubles
the real delay before playback actually starts. A fix (caching the
last `(channel uuid, start, duration) -> playbackUrl` result for well
under that 60s window, e.g. 30s, and returning it on the second call)
is pure/unit-testable logic, but wasn't attempted blind this pass --
worth a live test first to confirm the actual delay this adds is
worth the added cache-invalidation surface.
**Live check (2026-09-29): confirmed and quantified -- worth fixing.**
Played a real already-aired programme via `Player.Open`'s own
`broadcastid` path against the real lab instance (confirmed this
reaches the same `PlayEpgTag()` Kodi core function the source read
above already identified: `PlayerOperations.cpp`'s own JSON-RPC
`Player.Open` handler calls it directly for a broadcast id, not some
separate path). The whole `Player.Open` round trip (which blocked
until playback had actually started, not just queued) took ~1.1s.
Timed a single, direct `/api/catchup/sessions/` POST against the same
instance/channel in isolation, three times back to back: a
consistent ~0.31-0.32s each. Two sequential POSTs (~0.62-0.65s)
account for roughly 30% of the observed total -- a real, measurable,
user-perceptible chunk of the delay before catch-up playback starts,
not a rounding-error-sized cost. Confirms the fix sketch above is
worth implementing, not just theoretically correct.
**Fixed and re-verified live, 2026-09-29.** Added
`dispatcharr::ShouldReuseCachedCatchupSession()` (`CatchupSessionCache.h`,
new file) -- requires an exact match on all three key fields
(`channelUuid`/`programmeStart`/`durationMinutes`), not just the
channel, and reuses `IsStaleSince()` (`Staleness.h`) for the age check
against a new `kMaxCatchupSessionCacheAge` (30s, deliberately well
under Dispatcharr's own 60s handshake expiry). `DispatcharrClient::CreateCatchupSession()`
now checks a new `m_catchupSessionCache` (its own dedicated mutex,
`DispatcharrClient.h`) first -- a cache hit skips `EnsureAuthenticated()`
and the network entirely, not just the POST itself, and returns the
cached `playbackUrl` directly. Also added debug logging for both the
hit and miss paths (there was none here at all before this fix,
despite this being a real blocking network call) -- both genuinely
useful for diagnosing this exact code path in the future and what
ended up resolving a real ambiguity during this fix's own live
verification (see below).
Re-verified live end-to-end: rebuilt, redeployed to the real Kodi test
client, restarted it, and played a fresh already-aired programme. Two
informal `Player.Open` timing comparisons (before/after) turned out
too noisy (single-sample, ~1.1s total dominated by more than just the
two POSTs, normal run-to-run jitter) to cleanly show the expected
~0.3s improvement -- but the addon's own new debug log settled it
directly, not just by inference: `CreateCatchupSession` logged a cache
*miss* for the first call, then a cache *hit* exactly 12ms later for
the second, confirming the redundant POST is genuinely skipped now,
regardless of what the noisier timing numbers showed. Full C++ test
suite green throughout (1186 assertions, 647 test cases, 7 new for
`ShouldReuseCachedCatchupSession()` -- see
`tests/test_catchup_session_cache.cpp`: fresh exact match, mismatched
channel/start/duration each independently, never-cached, past-maxAge,
and right at the maxAge boundary).

#### Episode number without a season silently dropped

**Suspected: an episode number without a season is silently dropped,
flagged from a 23rd-pass audit (2026-09-26), confirmed against
Dispatcharr's own real current upstream source, not reproduced live.**
`XmlTvParser.cpp` only reads a programme's `xmltv_ns` field for its
episode number, but Dispatcharr's own XMLTV export
(`apps/output/epg.py`) only emits `xmltv_ns` when *both* season and
episode are known -- an episode-only programme (no season) is instead
exported only as `<episode-num system="onscreen">E{n}</episode-num>`,
which this addon never reads at all, so such a programme loses its
episode number, `EPG_TAG_FLAG_IS_SERIES`, and (per
`ShouldIncludeEpisodeDates()`'s own existing logic) its Year/FirstAired
display. Fix direction: a pure `ParseOnscreenEpisodeNum(onscreenText)`
fallback for the `S(\d+)E(\d+)`/`E(\d+)` onscreen forms, used only
when no `xmltv_ns` is present. Deliberately not implemented blind this pass --
making a non-zero episode number flow through `ShouldIncludeEpisodeDates()`
reopens that function's own "placeholder `<date>`" rationale
(`docs/RECORDINGS.md`) for this specific episode-only case, which
needs its own explicit decision, not a side effect of an unrelated
parsing fix. (Separately, and not this addon's own bug: Dispatcharr's
own XMLTV *import* path does a bare `int(parts[1])` on `xmltv_ns`, so
an already-malformed value like `.4/10.` loses its episode number
entirely before this addon ever sees it.)
**Live check (2026-09-29): negative on the real lab instance, but not
a full refutation.** Parsed the real, current `/output/epg` XMLTV
feed in full (a real multi-day sample with an `episode-num`
of either kind): every single `system="onscreen"` entry was paired
with a `system="xmltv_ns"` one on the same programme -- zero
onscreen-only entries found. Whatever this instance's own EPG
source(s) actually are, they evidently never supply an episode number
without also supplying a season for it, at least right now. Doesn't
rule out a different provider/EPG source genuinely doing this (the
mechanism itself, confirmed against Dispatcharr's own source, is real
and would still misbehave in that case) -- just that this specific,
real, fairly large sample doesn't currently exercise it.
**Fixed (2026-09-29), not itself independently reproduced live given
the check above, but implemented and tested against every real
scenario Dispatcharr's own source shows can occur.** Added
`ParseOnscreenEpisodeNum()` (`XmlTvParser.h`/`.cpp`, see CLAUDE.md's own
entry), tried only when no `xmltv_ns` entry is present on the same
programme -- confirmed against Dispatcharr's own real current upstream
source that this is exactly when it emits onscreen as a substitute, not
an addition. Recognizes `S<n>E<m>`/bare `E<m>` case-insensitively (onscreen
numbers are already 1-indexed, unlike xmltv_ns -- no `+1` applied here);
leaves both unknown for anything else rather than guessing, matching
`ParseEpisodeNum()`'s own established convention. On the
`ShouldIncludeEpisodeDates()` interaction this entry's own original pass
deliberately deferred: decided explicitly (documented in
`EpgTagUtil.h`'s own comment) to let an episode-only match flow through
the existing `episodeNumber > 0` branch like any other known-episode
case, rather than special-casing it -- the placeholder-`<date>` problem
that function guards against is specifically a programme with *no*
episode identity of its own repeating one `<date>` across genuinely
distinct airings, not an onscreen-sourced one, which is a real,
deliberate per-episode identity a provider chose to express without a
season. 9 new Catch2 tests (direct `ParseOnscreenEpisodeNum()` coverage:
`SxEy`, bare `Ey`, case-insensitivity, whitespace, no-1-indexing, and
four "don't guess" rejection cases -- empty, no marker, `S` with no
digits, `E` with no digits, trailing garbage after the digits) plus 2
full-parse-path tests (onscreen-only now parses; xmltv_ns still wins
when both are present on the same programme) -- one pre-existing test
("ignores a non-xmltv_ns system") was retargeted to a genuinely
unrecognized system (`dd_progid`) rather than onscreen, since onscreen
is no longer unconditionally ignored. Full C++ suite green throughout
(1218 assertions, 662 test cases).

#### Genre-category substring false positives

**Genre-category substring matching has a few false positives, flagged
from a 23rd-pass audit (2026-09-26), cosmetic, low priority.**
`EpgTagUtil.cpp`'s `MapCategoriesToGenreType()` matches "Darts" and
"Martial arts" to Arts/Culture through its own `arts` keyword
substring check (no word-boundary matching), and `CategoriesIndicateSeries()`
is case-sensitive while the genre map itself is case-insensitive --
an inconsistency between the two, not a bug in either alone. A fix
needs to keep the existing "Feature Film" test passing and keep
"Sports" matching via the `sport` substring, so isn't a trivial
word-boundary flip; not pursued this pass given the low practical
impact (miscategorized genre icon/grouping only, no functional
breakage).
**Fixed (2026-09-29), unit-tested only (pure logic, nothing to check
live).** `MapCategoriesToGenreType()` now requires a keyword to start
at a word boundary (`ContainsAtWordStart()`, prefix-only, so `sport`
still matches "Sports"/"Sportscast" but `arts` no longer matches
"Darts"); a new `martial arts` -> Sports entry precedes `arts` in the
first-match-wins table so "Martial arts" doesn't land in Arts/Culture
via its own word-start "arts". `CategoriesIndicateSeries()` is now
case-insensitive. Known trade-off: a keyword that only ever appeared
mid-word ("Docudrama" via `drama`) no longer matches -- judged
acceptable for a cosmetic genre-icon mapping; "World Series" still
reads as a series, unchanged. 4 new Catch2 tests.

#### One-time recordings from Kodi miss Dispatcharr's pre/post padding

**One-time recordings created from Kodi never get Dispatcharr's own
pre/post padding applied, flagged from a 25th-pass audit (2026-09-26),
confirmed against Dispatcharr's own real current upstream source, not
reproduced live -- see `docs/RECORDINGS.md`'s own follow-up note for
the full account.** `DispatcharrClient::CreateOneTimeRecording()`
deliberately sends no `custom_properties` at all (to avoid replacing
Dispatcharr's own auto-enrichment), but `RecordingSerializer.validate()`
only applies the global pre/post offset when
`isinstance(custom_properties.get("program"), dict)` -- so pressing
"Record" on a Kodi EPG guide entry gets exactly the raw EPG start/end
times, no padding, regardless of this addon's own
`recording_pre/post_offset_minutes` settings. This addon's own
user-facing text previously overclaimed coverage here too (corrected
the same day: `strings.po` #30055/#30057,
`DispatcharrClient.h`'s own comment). Possible fix: send
`custom_properties: {"program": {"start_time", "end_time"}}` (no
title/id, so Dispatcharr's own enrichment still runs afterward) only
for an EPG-based timer (`timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID`) --
needs a live test first to confirm this doesn't suppress that
enrichment the way an existing, different full-`custom_properties`
create call is already documented to. A later padding-setting change
would then also retroactively reach these recordings via
`reschedule_upcoming_recordings_for_offset_change` (see the
recurring-rule-duplication item elsewhere in this doc) -- the same
behavior Dispatcharr's own UI-created recordings already have, just
new for this addon's own creates specifically.
**Live check (2026-09-29): confirmed exactly as predicted.** The real
lab instance's own `dvr_settings` has real, non-zero padding
configured (`pre_offset_minutes: 1`, `post_offset_minutes: 2`).
Pressed "Record" on a real future EPG guide entry via `PVR.AddTimer`'s
own `broadcastid` path (the same `CreateFromEpg()`-equivalent flow a
real guide "Record" button press uses) for a programme scheduled
18:00-19:00 UTC. The resulting Dispatcharr recording came back with
`start_time`/`end_time` of exactly `18:00:00Z`/`19:00:00Z` -- the raw
EPG times, unpadded -- and `custom_properties` held only
`poster_logo_id`, no `program` key at all, confirming
`CreateOneTimeRecording()` genuinely sends none. A clean, direct
contrast was sitting in the same recordings list from unrelated,
already-existing Dispatcharr-side-created recordings: two others,
each carrying a real `custom_properties.program` block, showed a raw
`11:00-12:00` programme window correctly padded to
`10:59-12:02` -- exactly `-1`/`+2` minutes, matching the instance's
own configured offsets precisely. Same instance, same settings,
same moment in time -- only the Kodi-created recording was missing
the padding. Cleaned up: deleted the test timer via
`PVR.DeleteTimer`; confirmed the recording gone with a 404 on a fresh
`GET` of its own id.
**Follow-up live check (2026-09-29): the fix's own side-effect risk
(suppressing auto-enrichment) is confirmed safe -- clear to implement.**
First found the actual mechanism via Dispatcharr's own real current
source (cloned into a scratchpad): auto-enrichment for a not-yet-
started recording runs via `prefetch_recording_artwork` (`tasks.py`),
dispatched with a 1-second delay from a `post_save` signal on every
`Recording` create (`signals.py`) -- not something that waits for the
recording to actually start, which is what this addon's own existing
`PendingTitle` client-side bridge already implies but this hadn't been
traced to its actual trigger before. Its own gating condition,
`not program.get("user_edited") and not program.get("id") and not
program.get("title")`, only skips enrichment when the program dict
already has an `id` or `title` -- exactly why the fix sketch above
specifically omits both. Confirmed live directly, not just from
source: created two real recordings via a direct API round trip
against the real lab instance on the same channel, one with
`custom_properties: {"program": {"start_time", "end_time"}}` (the
exact shape the fix would send) and one with no `custom_properties`
at all (today's actual baseline). Both came back fully enriched
(`title`/`sub_title`/`description`/`id`/`poster_logo_id`) within
seconds of the same async task -- and the first recording's own sent
`start_time`/`end_time` merged cleanly alongside the new fields rather
than being replaced, confirming this is a genuine merge for this
narrower shape, not the same "replaces entirely" behavior a full,
title-including `custom_properties` on create is documented to
trigger. Cleaned up: deleted both test recordings immediately after
confirming; verified both gone with a 404 on a fresh `GET` of each.
Clear to implement the fix sketch above now -- both the bug and the
one real risk in its own proposed fix are confirmed live, not
theoretical either way.
**Fixed and re-verified live, 2026-09-29.**
`BuildOneTimeRecordingCreateBody()` (`TimerRequestBuilder.{h,cpp}`)
gained an `includeEpgProgramWindow` parameter -- when true, adds
`custom_properties: {"program": {"start_time", "end_time"}}` (no
title/id, per the confirmed-safe fix sketch above).
`DispatcharrClient::CreateOneTimeRecording()` gained a matching
`isEpgBased` parameter threaded through to it; `AddTimer()`'s
one-time-recording branch (`PVRDispatcharr.cpp`) now passes
`timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID`, so a genuinely manual
(non-EPG) recording is unaffected -- it has no real programme window
to associate with the padding mechanism in the first place, matching
this fix's own deliberately narrower scope. Re-verified live:
rebuilt, redeployed to the real Kodi test client, restarted it, and
pressed "Record" on a fresh real EPG guide entry (19:00-20:00 UTC) via
`PVR.AddTimer`. The resulting Dispatcharr recording came back
`18:59:00Z`-`20:02:00Z` -- exactly `-1`/`+2` minutes, the real
configured padding correctly applied this time -- with
`custom_properties.program` holding only the raw `start_time`/
`end_time`, no title/id, exactly the intended shape. Cleaned up:
deleted the test timer via `PVR.DeleteTimer`; confirmed the recording
gone with a 404. Full C++ test suite green throughout (1193
assertions, 650 test cases, 4 new for
`BuildOneTimeRecordingCreateBody()`'s new parameter -- omitted
entirely when false, present with no title/id when true, and using
the same now-mapped effective start time as the top-level field for
Kodi's own instant-recording sentinel).
**A separate, real Dispatcharr-side gap found live while re-verifying
this fix, NOT caused by it and not something this addon can control --
logged separately as "Dispatcharr recording enrichment ignores a channel's EPG override" rather than left buried in this entry's own
live-check paragraph:** see "Dispatcharr's own recording auto-
enrichment queries a channel's raw `epg_data`, not its effective/
override one" entry.

#### PendingTitle cache keyed by channel id, not recording id

**`PendingTitle` (the short-lived title-enrichment cache
`ParseRecordingJson()` falls back to) is keyed by channel id, not the
actual recording id Dispatcharr's own create response returns, flagged
from a 25th-pass audit (2026-09-26), low impact, not reproduced
live.** `CreateOneTimeRecording()` discards its own POST response body
(which does carry the new recording's real `id`), so a second
recording started on the *same channel* within the cache's own TTL
window (a manual recording with no EPG match, started shortly after
another one on that channel) could borrow the first one's own
in-flight title instead of getting its own. Keying by the real
recording id instead would still handle the existing
start-time-clamping case this design already exists for. Low impact
in practice: Dispatcharr's own `prefetch_recording_artwork` already
enriches the real title within about a second via its `post_save`
signal, so this cache exists only to bridge a very brief window to
begin with.
**Fixed (2026-09-26, a 32nd-pass audit): the blast radius turned out
wider than "low impact" -- a project-wide review found a real
permanent-corruption path, not just a display glitch.** Matching by
channel alone let ANY other untitled recording on that channel borrow
the title, not just a second brand-new one: an old completed "Custom
Recording" that never gets a title from Dispatcharr at all, or an
unrelated future scheduled timer. If that borrowed title landed in
Kodi's own cached copy of the unrelated timer and the user edited it
before Kodi's next refresh, `ShouldRenameOnTimerEdit()`'s own
placeholder guard (`TimerIdentity.h`) only recognizes *that*
recording's own synthesized "Recording &lt;id&gt;" default, not an
entirely different borrowed title -- so the edit's own upsert could
permanently write the wrong title back via `RenameRecording()` and mark
it `user_edited`, silently corrupting an unrelated recording's real
title server-side and blocking its own future auto-enrichment
entirely. `CreateOneTimeRecording()` now parses `id` from its own POST
response and keys the cache by it; `ParseRecordingJson()`'s own lookup
(`dispatcharr::FindPendingTitleForRecording()`, renamed from
`FindLatestPendingTitleForChannel()`, `PendingTitleLookup.h`) matches
the exact recording id instead.

#### Series rule with no match displays as the Unix epoch

**A series rule with no current upcoming/in-progress match still
displays as the Unix epoch, flagged from a 26th-pass audit
(2026-09-26), confirmed against Kodi's own real source, not reproduced
live.** This is distinct from the already-fixed title-case-mismatch and
channel-less-rule epoch bugs (`docs/RECORDINGS.md`) -- `GetTimers()`
only calls `SetStartTime()`/`SetEndTime()` on a series rule's own row
when `MatchRecordingsToSeriesRules()` finds an `earliestIdx >= 0`
match; a correctly-linkable rule with genuinely no upcoming/in-progress
occurrence right now (a show between seasons, a "new episodes only"
rule while only reruns currently air, a show outside the fetched EPG
window) leaves both at Kodi's own zero-initialized default, rendering
as 12/31/1969 or 1/1/1970 the same way the already-fixed cases did.
`tools/kodi_smoke_test.py`'s own epoch-check test can't catch this,
since its own test rule always seeds from a real, currently-scheduled
broadcast, so a match always exists in that harness. Fix direction: a
pure `ComputeSeriesRuleDisplayFields()` using Kodi's own
`SetStartAnyTime()`/`SetEndAnyTime()`/`SetWeekdays(PVR_WEEKDAY_ALLDAYS)`
(rendered as "Any day at any time") when no match exists -- not
implemented blind: adding the corresponding `SUPPORTS_START_ANYTIME`/
`END_ANYTIME` timer-type flags would also change Kodi's own timer
dialog and `CreateFromEpg()` behavior, a real regression risk that
needs a live test to confirm before shipping.
**Live check (2026-09-29): confirmed exactly as predicted.** Created a
real series rule via a direct API round trip against the real lab
instance, with a title guaranteed to never match any real programme
(`ZZZ_TEST_NOMATCH_RULE_DO_NOT_USE`), so `MatchRecordingsToSeriesRules()`
can never find an `earliestIdx >= 0` for it. Once picked up by
`GetTimers()`, `PVR.GetTimers` reported both `starttime` and `endtime`
as `1970-01-01 00:00:00` -- the exact epoch-display bug this entry
describes, confirmed live end-to-end, not just by source reasoning.
Cleaned up: deleted the test rule via the API immediately after
confirming; verified gone from a fresh `GET` of the real instance's
series-rules list (the real rules remain, no test residue).
**Fixed and re-verified live, 2026-09-29 -- and the fix's own actual
risk turned out narrower than this entry's original "fix direction"
worried.** Re-read Kodi's own real current source before implementing,
which corrected that original worry: `SetStartAnyTime()`/
`SetEndAnyTime()`/`SetWeekdays(PVR_WEEKDAY_ALLDAYS)` are set directly
on the `kodi::addon::PVRTimer` instance `GetTimers()` builds, and
`CPVRTimerInfoTag::UpdateSummary()` (`PVRTimerInfoTag.cpp`) -- the
function that builds the list-view summary text -- substitutes "any
time"/"Any day" for these unconditionally, entirely independent of
whether the timer *type* itself declares
`PVR_TIMER_TYPE_SUPPORTS_START_ANYTIME`/`END_ANYTIME`. That pair only
gates a separate concern this fix doesn't touch: a user-facing toggle
in Kodi's own *edit* dialog (`GUIDialogPVRTimerSettings.cpp`) for
manually picking "any time" on a new/edited timer. `GetTimerTypes()`'s
own `kTimerTypeSeries` declaration is deliberately left unchanged --
this addon never offers "any time" as an editable choice, only
computes it here for read-only display, and the series type already
has no `SUPPORTS_START_TIME`/`END_TIME` of its own for the edit
dialog to react to either way -- so the dialog/`CreateFromEpg()`
regression risk this entry originally flagged doesn't actually apply
to this fix's real scope. `GetTimers()`'s series-rule loop
(`PVRDispatcharr.cpp`) now calls the three setters above in the
`earliestIdx < 0` (no-match) branch instead of leaving `SetStartTime()`/
`SetEndTime()` unset. Re-verified live end-to-end: rebuilt the addon,
redeployed to the real Kodi test client, restarted it, and recreated
the *exact same* no-match test rule again. `PVR.GetTimers`'s own
`summary` property (confirmed against Kodi's real JSON-RPC schema to
be the exact field `UpdateSummary()` populates) now reads
`"Any day at any time"` instead of computing dates from the epoch --
the real, user-visible fix confirmed working, not just the raw
`starttime`/`endtime` values (which JSON-RPC still reports as the
literal epoch time_t, same as before -- expected, since Kodi's own
Timers-list GUI never displays those raw fields directly, only
`summary`). Deleted the test rule again immediately after confirming;
verified gone from a fresh `GET` (the real rules remain). Full C++ test
suite still green (1179 assertions, 640 test cases) -- this fix has no
pure-logic component to unit-test on its own, matching this project's
own established boundary (`PVRDispatcharr`'s actual Kodi-API surface
stays manual/live-tested, see CLAUDE.md's "Building and testing"
section).

#### kodi_smoke_test.py harness bugs

**`tools/kodi_smoke_test.py` (the manual live-test harness) has its
own real bugs, flagged from a 26th-pass audit (2026-09-26), confirmed
against Dispatcharr's own real current upstream source for the first
one, Kodi/code reasoning for the rest, not reproduced live.**
- `_find_in_progress_recording_id()` treats a bare `endtime > now` as
  "in progress" -- a stopped/interrupted recording (whose `end_time`
  stays at its originally-scheduled value, per the just-fixed
  stopped-duration bug above) can pass this check too, silently
  exercising the completed-file playback path instead of the
  in-progress one the test believes it's covering. Fixing the addon's
  own duration bug above helps the "stopped" case; a `PVR.GetTimers`
  state=="recording" check would be a more direct, independent signal.
- `DispatcharrApiClient.call()`/`_authenticate()` don't catch
  `ConnectionError`/`json.JSONDecodeError` the way `JsonRpcClient`
  already does (after its own confirmed-live incidents) --
  `SmokeTestRun.record()` also only catches
  `JsonRpcError`/`DispatcharrApiError`/`AssertionError`, so any
  `KeyError`/`TypeError`/`OSError` kills the whole run with a
  traceback instead of a clean per-test failure. Worse:
  `check_realtime_update_push()`'s own POST sits outside its
  try/finally, so a POST that times out or returns non-JSON after the
  server already created the recording leaves a real, uncleaned-up
  recording on the backend -- the same "succeeds server-side, fails
  client-side" class this same file already documents and guards
  against elsewhere (`Player.Open`/`PVR.AddTimer`), just missed here.
- `_add_and_verify_timer()`'s broadcast-selection fallback
  (`next(<non-overlapping>, future[0])`) can fall back to a broadcast
  that overlaps an existing timer -- exactly what its own docstring
  already documents as causing a hang or a `-32100` failure. It should
  skip that candidate entirely instead of falling back to it.
- `_seek_within_recording_and_verify()`'s `target = min(30, total - 15
  - before)` can go negative if playback happens to auto-resume near
  the very end of the recording, silently testing a backward seek
  while the "forward seek" assertion still passes.
- Extraction candidate for all of these: the pure helpers involved
  (overlap detection, new-item lookup, the in-progress filter, the
  broadcast-selection pick, the seek-target math) could move under
  `tools/tests/`, the same precedent `check_doc_refs.py` already set
  for this directory -- the broadcast-selection and seek-target bugs
  specifically would likely have been caught by a test alongside the
  original code, per this project's own "new pure-logic code gets a
  test alongside it" convention.
**Fixed (2026-09-29), not itself independently reproduced live (this is
the manual harness itself, not the addon -- these bugs would only ever
surface as a false pass/incorrect skip/crashed run during some future
live pass, not against this real lab instance today), all 5 bullets
above addressed together, each extracted into its own pure, tested
function per the "extraction candidate" bullet's own suggestion:**
- `_find_in_progress_recording_id()` now matches a `PVR.GetTimers`
  `state=="recording"` entry to its own `PVR.GetRecordings` row by
  title (`_match_recording_by_title()`) rather than `endtime > now`.
- `DispatcharrApiClient._authenticate()`/`call()` now catch
  `ConnectionError`/`json.JSONDecodeError` too, wrapped in
  `DispatcharrApiError` the same way `JsonRpcClient` already does.
  `SmokeTestRun.record()` now also catches a bare `Exception` (any
  check's own unanticipated bug reports as a clean per-check failure
  instead of killing every check after it; `KeyboardInterrupt`/
  `SystemExit` aren't `Exception` subclasses, so Ctrl-C still aborts as
  normal). `check_realtime_update_push()`'s creating POST moved inside
  the same try/finally as everything else, with a new
  `_find_matching_recording()` lookup (channel + exact start/end time)
  as a best-effort recovery path when the POST itself raises despite
  Dispatcharr having already created the row -- the same
  "did it actually happen despite the error" reasoning
  `_add_and_verify_timer()`'s own `PVR.AddTimer` retry already uses for
  Kodi's side of an equivalent race.
- `_add_and_verify_timer()`'s broadcast pick
  (`_pick_conflict_free_broadcast()`) now returns `None` -- causing a
  clean `SkipCheck`, never a possibly-conflicting fallback -- when
  every future broadcast overlaps an existing timer.
- `_seek_within_recording_and_verify()`'s seek-target math
  (`_compute_forward_seek_target()`) is now checked for `<= 0` before
  seeking, raising `SkipCheck` instead of silently testing a backward/
  no-op seek while still claiming to test a forward one.
- `_select_new_timers()` (the pre-existing `find_new` closure,
  extracted unchanged in behavior) covers the recurring-rule-plus-
  auto-spawned-occurrence matching this function's own cleanup relies
  on.
All five now live in `tools/kodi_smoke_test.py` as plain, dependency-free
module-level functions, with a new `tools/tests/test_kodi_smoke_test.py`
(31 tests: each extracted function directly, plus
`SmokeTestRun.record()`'s new broad-catch behavior and
`DispatcharrApiClient`'s new `ConnectionError`/`JSONDecodeError`
handling via `monkeypatch`ing `urllib.request.urlopen`) -- the same
"new pure-logic code gets a test alongside it" convention as the addon's
own C++/plugin code, per CLAUDE.md. Full Python suite green throughout
(358 tests, `ruff check`/`ruff format --check` both clean).

#### Redundant Kodi channel/group resyncs

**Redundant Kodi channel/group resyncs, flagged from a 26th-pass audit
(2026-09-26), confirmed against Kodi's own real source, efficiency
only, not reproduced live.** `EnsureChannelsLoaded()` (as of the
25th-pass fix) fires both triggers on every successful commit, whether
or not anything actually changed -- combined with the already-logged
concurrent-duplicate-fetch-at-startup item, this can add up to two
redundant full Kodi channel/group resyncs at startup, plus one on
every otherwise-uneventful periodic refresh. Confirmed harmless
(Kodi's own `CPVRManagerJobQueue` dedupes pending jobs of the same
type, and runs them outside its own lock, so there's no deadlock or
loop risk either way) -- purely wasted work, not a correctness issue.
Candidate fix: a pure `HasChannelLineupChanged(oldChannels, oldGroups,
newChannels, newGroups)` gate, only firing the triggers when something
genuinely differs. Not pursued this pass.
**Fixed and re-verified live (2026-09-29).** New `ChannelLineupChange.h`
(header-only, see CLAUDE.md): `HasChannelLineupChanged()` compares the
cached and just-fetched lineup order-insensitively by id across every
`Channel` field plus the `(id, name)` group set, deliberately
conservative (any difference counts). One design departure from the
candidate above, on purpose: a pure change gate would also remove the
old every-refresh trigger's incidental self-healing for a poke Kodi's
manager missed (both triggers are fire-and-forget), so
`ShouldTriggerKodiChannelSync()` also fires an unchanged lineup once
it's been 24h (`kChannelSyncMaxSilenceHours`) since the last poke.
`EnsureChannelsLoaded()` decides under its existing lock, against the
cache before it's overwritten. Re-verified live on the real Kodi test
client with new debug logging at the decision: a fresh start logged
"channel lineup changed -- triggering Kodi resync" for the first load
and "channel lineup unchanged -- skipping Kodi resync" for the duplicate
startup fetch 0.4s later (the concurrent-duplicate-fetch case this entry
called out), with Kodi still showing all of its channel groups afterward.
Not exercised live: a real lineup change on the server (would need
editing real channels) -- covered by unit tests only (16 new cases, one
per `Channel` field plus groups, ordering, duplicates, first load).

#### kKnownTimeZones vs. settings.xml drift

**`kKnownTimeZones` (`TimeZoneUtil.cpp`) drifting out of sync with
`settings.xml`'s own `recurring_rule_timezone` options has already
caused one real regression (2026-09-09), flagged from a 27th-pass
audit (2026-09-26), low priority, test-coverage gap only -- the two
currently match (52 zones plus "manual").** A cross-file check (in
Catch2, or `tools/tests` following `check_doc_refs.py`'s own
precedent for this kind of cross-file consistency check) would lock
this in going forward instead of relying on a human noticing drift.
**Fixed (2026-09-29).** New `tools/tests/test_timezone_settings_sync.py`
(the `tools/tests` route, following `check_doc_refs.py`'s precedent):
parses `kKnownTimeZones` out of `TimeZoneUtil.cpp` and the
`recurring_rule_timezone` options out of `settings.xml` and asserts the
two zone sets match exactly in both directions (plus "manual" as the
settings-only sentinel), that each list has no duplicates, that every
option's label has a `strings.po` entry, and that the extractors
themselves find a plausible number of zones so a silently-matching-
nothing regex can't make the comparison vacuous. Confirmed it can
actually fail: temporarily deleting one zone row from the C++ side
produced the expected "offered in settings.xml but unknown to
kKnownTimeZones" failure. Currently in sync (52 zones).

#### Pausing past a recording's end can kill playback on resume

**Pausing growing-buffer in-progress-recording playback for more than
~20s after the recording has already ended can kill playback on
resume, flagged from a 28th-pass audit (2026-09-26), confirmed
against Dispatcharr's own real current upstream source -- confirmed
live and fixed, 2026-09-30.**
Dispatcharr removes a finished recording's HLS directory once its own
`dvr:hls_viewer:{id}` Redis key (20s TTL) has gone unrefreshed, and
only a `.ts` GET refreshes that key (`apps/channels/api_views.py`).
While this addon's growing-buffer read path is paused, none of its own
background traffic is a `.ts` GET: `GetStreamTimes()` calls
`GetRecordingById()`/fetches the `.m3u8` playlist (neither refreshes
the key), and `ProbeSegmentByteSize()` only HEADs newly-discovered
segments -- none exist once the recording has ended. If a pause
outlasts the 20s window, the directory is removed server-side; on
resume, the next segment GET gets a `.ts` 404 ("HLS content not
available"), which `ReadInProgressRecordingStream()` currently treats
as a hard stream-read failure (`httpCode != 200` returns -1), ending
playback with the rest of the recording unplayed. Reopening the
recording from Kodi's Recordings list does work, since it plays back
through `/file/` once the recording shows as complete. A candidate fix
(not implemented, needs live confirmation the pause/resume path
actually reaches this failure mode as read): issue a periodic
keep-alive (e.g. a HEAD on the last known segment URL every ~10s) from
`RefreshInProgressRecordingManifest()` while the recording is still
open and unfinished, since DRF maps HEAD to the same view function
that sets the Redis key.
**Live check (2026-09-30): confirmed.** A real 4-minute recording,
played while still recording and paused at ~48s (position 24s). The
recording ended at +240s; at +301s the server's HLS playlist URL
answered 302 to the completed file (HLS directory gone), the file URL
served 206 `video/x-matroska`, and the recording's status read
`completed`. Resumed at +306s: playback ran ~12s from Kodi's own cache,
then 63 consecutive segment fetches came back 404 over ~8s (~7.8/s) and
the player ended by itself 17s after resume.
**Fixed (2026-09-30), with the keep-alive.** `MaybeSendInProgressHlsKeepAlive()`
(`DispatcharrClient.cpp`, decision core `dispatcharr::ShouldSendHlsViewerKeepAlive()`
in `HlsViewerKeepAlive.h`, 16 test cases) sends a HEAD on the newest known
segment every 10s while a viewer is paused with unread segments, called
from the top of `RefreshInProgressRecordingManifest()`. One deliberate
departure from the candidate fix sketched above ("while the recording is
still open and unfinished"), found while designing it: it only runs while
the reader still has unread bytes (`position < totalBytes`). Dispatcharr
only finalizes a finished recording -- removes the HLS directory, then
flips its status to `completed` -- after the viewer key lapses, and this
addon only learns a recording is finished from exactly that finalization,
so a reader waiting at the tail that also kept the key alive would hold
the recording open forever waiting for an EOF its own keep-alive
prevents. The other candidate fix (falling back to the completed file
when a segment 404s) was not pursued: the HLS byte stream and the
finished MKV are different containers with different byte offsets, so a
read position can't be carried from one to the other.
**Re-verified live (2026-09-30)** against a real 3-minute recording on
the Linux test client: played, paused at position 18s, and left paused
for about 110s past the recording's end (the unfixed build had already
lost the directory by 60s past it). The addon sent 12
keep-alives, every ~10.1s, all answered 200; at +300s the server's HLS
playlist still answered 200 (not 302) and the recording's status still
read `recording`. Resumed: playback ran through to the recording's real
end (position 190s) with **zero** segment 404s (unfixed: 63), stalled at
the tail for ~22s while Dispatcharr's viewer key lapsed, then ended by
itself; the server then reported the recording `completed` and the HLS
playlist answered 302. That last part is the check on the
`position < totalBytes` rule: no keep-alive was sent after the reader
reached the tail, and the recording finalized normally rather than being
held open. `GetStreamTimes()`'s polling during the pause turned out to be
frequent (392 real manifest refreshes over ~245s, roughly twice a
second, the 500ms throttle), so the 10s interval has plenty of headroom
against the server's 20s TTL.
Also re-verified on the Windows test client (the MSVC build of the same
commits, which compiled cleanly): a 2-minute recording, paused at position
22s and left paused ~60s past its end -- the HLS playlist still answered
200 with the recording still `recording`, keep-alives went out every
~10.1s all answered 200, and after resume playback ran to the recording's
real end (position 118s) with zero segment 404s, stalled ~15s at the
tail, ended by itself and finalized (`completed`, playlist 302).
The narrower residual case -- the directory already gone by the time a
paused viewer resumes -- is its own entry, "In-progress segment 404 has no bounded retry".

#### In-progress segment 404 has no bounded retry

**`ReadInProgressRecordingStream()` treats a segment-body 404 as a bare
`return -1` with no bounded retry, so a recording whose HLS directory is
already gone makes Kodi re-issue the read at ~7-8 requests/sec until its
own player gives up, found live 2026-09-30 while reproducing the paused-viewer bug ("Pausing past a recording's end can kill playback on resume") -- fixed the same day, along with the "In-progress recording deleted entirely loops at the tail" entry.** The keep-alive closes the common
way to lose the directory, but the read path itself is unchanged for
the ones it can't prevent: a pause past Dispatcharr's own 4-hour
viewer-wait cap, the device asleep or the network down for longer than the
20s viewer-key TTL, or a recording deleted mid-playback. Measured before
the keep-alive existed: 63 consecutive segment 404s over ~8s (~7.8/s),
and the player ended by itself 17s after resume with no diagnosable
error surfaced to the user. This is the same unbounded-storm shape
`ShouldGiveUpAfterSegmentFetchFailure()` (`SegmentFetchFailure.h`)
already closed for live timeshift, where Kodi's core retries a `-1` read
near-immediately rather than giving up. Candidate fix, not implemented:
on a segment 404, force a manifest refresh and, once it positively
confirms the content is gone (`FetchRawInProgressPlaylist()`'s
`wasNotFoundOut`, the same 404-or-3xx signal `finished` already keys
off), mark the stream fatal so later reads return `-1` immediately
without touching the network -- with a small consecutive-failure bound
as the fail-safe for a 404 that isn't explained by the playlist. The
keep-alive already logs a warning and stops asking once one of its own
HEADs learns the directory is gone (`hlsKeepAliveGone`), so that signal
exists on the paused path; the read path just doesn't use it. Falling
back to the completed file isn't possible (different container,
different byte offsets).
**Fixed (2026-09-30)** by the same `contentGone` mechanism that closed
the "deleted entirely" entry above rather than the candidate fix as
sketched: a segment 404 now runs one (throttled) refresh, and once that
confirms the content gone -- deleted, or finished with its directory
removed -- the read returns EOF (not `-1`, which Kodi retries
near-immediately) and every later read needing an uncached segment does
the same without touching the network. An unexplained 404 (the playlist
still fine) still fails the read with `-1` exactly as before, and the
refresh it triggers arms its own 500ms throttle, so Kodi's retries of
that read cost at most one extra lookup and playlist fetch per half
second instead of per retry; it has no consecutive-failure bound of its
own, which remains the gap against live timeshift's
`ShouldGiveUpAfterSegmentFetchFailure()`. Live: a paused viewer whose
recording finalized and had its HLS directory removed while the addon
couldn't reach the server ended cleanly after resuming with two
requests in total (see the "deleted entirely" entry's re-verification),
where the unfixed build produced 63 consecutive segment 404s. (That run
detected it through `GetStreamTimes()`'s polling, which resumed the moment
the server was reachable again and so got there before the resume; the
segment-404 branch itself is covered by the unit-tested decision and code
reading, not exercised live.)

#### Recurring-rule weekday bitmask never timezone-shifted

**A recurring rule's own weekday bitmask is never timezone-shifted
anywhere, unlike its start/end/first-day, so a day-boundary crossing
could schedule occurrences on the wrong weekday -- flagged from a
33rd-pass audit (2026-09-26), confirmed against Kodi's own real
current SDK source for the mechanism, confirmed live 2026-09-30 and
fixed the same day, both directions, re-verified live through Kodi's
own timer dialog.** (An earlier draft of this same finding
cited Kodi's `CPVRTimerRuleMatcher`/`MatchDayOfWeek()` as the
mechanism -- traced further and confirmed that class only applies to
a *non-client-owned* Kodi-local reminder timer
(`if (!timer->IsOwnedByClient())`, `PVRTimers.cpp`), never to this
addon's own client-owned recurring/series rules; it plays no part in
how this addon's rules are actually scheduled. Corrected here so a
future pass doesn't repeat that specific wrong citation.)
`RecurringRuleUtil.h`'s own comment (see there for the fuller account)
now documents the actually-confirmed mechanism: Kodi's own timer
dialog (`GUIDialogPVRTimerSettings.cpp`) sets `m_iWeekdays` directly
from its own weekday checkboxes with no local/UTC conversion at all,
unlike `m_firstDayLocalTime`, which the dialog explicitly converts via
`SetFirstDayFromLocalTime()` -- and `FillAddonTimer()`
(`PVRTimerInfoTag.cpp`) copies that same raw, unconverted bitmask
straight through to this addon. `ComputeRecurringRuleFields()` then
passes it through into `daysOfWeekOut` completely unshifted, while
`startDateOut` (derived from `firstDay`) IS correctly shifted by
`offsetMinutes` (bridging UTC to Dispatcharr's own system timezone)
and re-floored to whatever UTC-then-offset-shifted midnight that
lands on. If that shift crosses a calendar-day boundary -- plausible
whenever the rule's own start time-of-day, once shifted by
`offsetMinutes`, is close enough to midnight -- `startDateOut`'s own
day-of-week could then disagree with the (unshifted) weekday bitmask
still being sent alongside it, since nothing re-derives or corrects
the bitmask to match. Separately, and independently of Dispatcharr's
own offset: Kodi's own dialog performs no shift at all between the
user's *local* weekday selection and the UTC semantics this addon's
own `startTime`/`firstDay` already assume, so the same kind of
mismatch could also arise purely from Kodi's own system timezone
differing from UTC, with no Dispatcharr-side offset involved at all.
Both of these are currently unhandled; this file's existing
`ComputeRecurringRuleDisplayTimes()`/overnight-rule fixes already show
this exact class of day-boundary math has needed several successive
corrections in this project's own history, so a blind fix here risks
the same fate without a live test against a Kodi instance and a
Dispatcharr instance in two genuinely different timezones (or, more
narrowly, just Kodi in a non-UTC zone) to actually observe which
calendar day a rule created near local midnight really lands on.
**Live check (2026-09-30): confirmed as a display mismatch when Kodi's
timezone differs from Dispatcharr's; the edit direction still can't be
exercised.** Used a real weekday rule already on the instance and changed Kodi's own
`locale.timezone` (edited in `guisettings.xml` while stopped -- Flatpak's `TZ`
variable is overridden by that setting). With Kodi in Dispatcharr's own zone
the rule reads "Mo-Tu-We-Th-Fr" at its own time of day, correct. With Kodi in
a zone far enough away that the rule's time of day lands on a different
calendar day, it reads the same weekday list at the corresponding local time
of day: the time of day moved with the zone but the weekday list did not,
while the rule's own occurrences (absolute timers) fall on the shifted days --
so the label names a day with no occurrence and omits one that has one, off by
one. Cosmetic in
this direction (nothing is scheduled wrongly). The Kodi-to-Dispatcharr
direction (`ComputeRecurringRuleFields()` on an edit) is unreachable
over JSON-RPC because no `PVR.UpdateTimer` method exists, so whether an
edit made from a mismatched zone lands on the wrong weekday remains
untested.
**Fixed (2026-09-30)**, after reading Kodi's side of it more closely
(`GUIDialogPVRTimerSettings.cpp`, `PVRTimerInfoTag.cpp`) than the first pass
had. A repeating timer is one *local* rule: "these weekdays by the local
clock, at this local time, from this local date." Start and end time are
converted between local time and UTC with libc's `localtime()` of the very
instant being converted (`CPVRTimerInfoTag::ConvertUTCToLocalTime()`/
`ConvertLocalTimeToUTC()`), so per date, daylight saving included; the
weekday checkboxes are copied through with no conversion at all; and first
day is only ever edited as a *date* -- `SetDateFromIndex()` replaces the
date part of the local first-day value and keeps whatever time of day it
already had. Dispatcharr reads `days_of_week` and `start_date` in its own
configured zone. So the two calendars agree about which day an occurrence
falls on unless its UTC time of day `U` lands on different sides of
midnight in the two zones, in which case Dispatcharr's date is
`floor((U + offD) / 86400) - floor((U + offK) / 86400)` days later than
Kodi's (both offsets at that occurrence's instant) -- a number that
depends only on `U` and the two offsets, never on the date, which is what
lets one rotation translate a whole weekday list.
`ComputeRecurringRuleDayShift()`/`RotateWeekdaysBitmask()`
(`RecurringRuleUtil`) implement it; `ComputeRecurringRuleFields()` now
takes a resolver for Kodi's own offset, rotates the weekday list forward by
the shift and builds `start_date` from the *Kodi-local date* of first day
(Kodi's offset at first day itself) moved by the same shift (it used to
take the Dispatcharr-local date of the first-day *instant*, which is the
wrong question: the dialog can hand back any time of day with it, so that
date is only right by accident); `ComputeRecurringRuleDisplayTimes()` takes
the same resolver, reports the shift for `GetTimers()` to rotate the list
back by, and names `firstDayOut` as Kodi-local *noon* of the date the first
occurrence falls on in *Kodi's* calendar, so that what Kodi echoes back on
any later edit reconstructs `start_date` exactly (noon rather than
midnight so the date survives a transition that day, and a local midnight
that doesn't exist or happens twice). Kodi's offset is
`LocalUtcOffsetMinutes(at)` (`TimeUtil.h`), the process's own local offset
at an instant -- the same `localtime()` call Kodi's own conversion makes.
The shift is zero whenever the two zones agree, at every time of year, so
the common same-zone setup behaves exactly as before; the bug only ever
bit a Kodi device in a different zone than its Dispatcharr server, and
only for rules whose time lands near enough to midnight for the zones to
disagree about the day -- which for a server left on UTC and a Kodi in the
Americas is every evening rule (9 PM in US Eastern summer time is 01:00 UTC
the next morning, so "Monday to Friday" became Sunday to Thursday evenings).
**A first version of this fix was wrong, and the live check caught it.**
It read Kodi's offset once, at "now", on the belief that Kodi applies one
cached bias (`CDateTime::GetTimezoneBias()`) to every date -- which is what
that function is, but timer conversion doesn't use it. A rule created with a start date in the other daylight-saving period from today, and viewed today by a Kodi in the *same* zone read
shifted weekdays: "now" and the rule's first occurrence fell in different daylight-saving periods, so the two offsets differed by an hour and
the shift came out nonzero for a zone that hadn't changed. (Kodi itself
showed that rule's time of day correctly, which is what first
showed it converts per date.) Both offsets are now taken at the rule's own
start instant, and a same-zone Kodi reads it as Mo-Fr again.
Checked against a model written without the formula rather than just
against worked examples: for every pair among 27 real-world offsets
(-12:00 to +14:00, including the half- and quarter-hour zones), 13 times
of day, 6 weekday selections and first days at different times of day,
the set of instants Dispatcharr will record for the rule this produces
equals the set of instants Kodi's dialog describes -- and, the other way,
the rule shown in Kodi for a Dispatcharr rule fires on the same instants;
the same for every pair of nine real zones from the addon's own DST table,
over transition-free stretches; plus display-then-edit round-trip identity,
no start-date drift over repeated edits, and -- the regression above -- that
Kodi and Dispatcharr in the same daylight-saving zone never shift a rule on
any day of 2026, at times of day around midnight. Nine deliberate
breakages of the implementation (rotation the wrong way, start date
without the shift, first day without it, the display shift computed
against the wrong zone, the old Dispatcharr-zone date of the first-day
instant, Kodi's offset read at the wrong instant in four places) each fail
the suite.
**Re-verified live (2026-09-30)**, real Kodi on the Linux test client against
the real instance, Kodi's own zone set through its `locale.timezone`.
*Display:* disposable disabled rules, Monday-Friday late in the evening and in
the early hours, and both again with a `start_date` in the other daylight-saving period. In Kodi in the
instance's own zone all four read as created. With Kodi set to several other
zones, each rule's weekday list moved by the day the model predicts for that
pair of zones (in either direction), and the rules whose start dates fall on
opposite sides of a DST change read an hour apart where they should. The old
code passed the list through unshifted. *Edit, through Kodi's own Timers
dialog* (driven with JSON-RPC `Input` actions and screenshots, see
`docs/RECURRING_RULES.md`), with enabled off so nothing was scheduled:
creating a rule with a weekday list and a first day reached Dispatcharr with
weekdays and `start_date` shifted as the model predicts, and Kodi read it back
with the weekdays and first day it was created with; opening it and pressing
OK changed nothing on the server; changing the weekdays, moving first day, and
changing the start time to one that no longer crosses midnight each changed
only the fields they should have; Activate then Deactivate from the context
menu changed only `enabled`; and the same checks held with Kodi in a zone on
the other side. The addon's debug log line for each edit ("recurring rule from
Kodi: weekdays ... day shift ...") matched every expectation. Disposable rules
were deleted afterward.

#### ProbeSegmentByteSize() unbounded byte size

**`ProbeSegmentByteSize()`'s real curl-probed segment size had the same
unbounded-byte-size risk `LiveManifestParser`'s own `byte_size` field
just got a ceiling for, flagged from a 52nd-pass audit (2026-09-27) --
RESOLVED as of a 53rd-pass audit (2026-09-27), which also found a more
severe consequence than the one originally flagged.** Originally
logged only as the summed-overflow risk: `ContentLengthHeaderCallback`
(`CurlCallbacks.cpp`) writes a real HTTP response's `Content-Length`
header straight through `std::stoll` with no upper bound, and
`RefreshInProgressRecordingManifest()` sums each probed size via
`AppendSegmentOffsets()`. The 53rd-pass audit found a real, more
severe, single-value (not summed) consequence: `ReadInProgressRecordingStream()`'s
own `body.reserve(static_cast<size_t>(segByteSize))` -- caching one
in-progress-recording HLS segment's full body for a single read -- gets
this same unbounded value directly, unsummed. A single absurd
`Content-Length` response throws `std::bad_alloc`/`std::length_error`
there, uncaught by anything on this call path -- confirmed against
Kodi's own real current SDK source that `ADDON_ReadRecordedStream`
(`kodi-dev-kit/include/kodi/addon-instance/PVR.h`) is a plain,
try/catch-free passthrough -- aborting the whole Kodi process from
what looks like an ordinary playback read. Fixed by clamping
`ProbeSegmentByteSize()`'s own return value to a new, much smaller
`kMaxProbedSegmentByteSize` ceiling, deliberately far below
`LiveManifestParser`'s own 1 TiB `byteSize` ceiling -- a real HLS TS
segment is never legitimately anywhere near even this size, and this
value flows straight into a single `reserve()` call, not just a
cumulative sum -- explicitly preserving the `-1` failed-probe sentinel
(checked separately, before the clamp). The ceiling itself was
originally 512 MiB, lowered to 64 MiB the very next, 54th, pass for
this project's own primary real target platform (see
`kMaxProbedSegmentByteSize`'s own comment) -- corrected here in a
55th-pass audit after this entry was found to still cite the stale
original figure, per this project's own "grep docs/ on a change" rule.
Still not unit-testable (a
real HTTP curl call, `DispatcharrClient`'s own untested surface) --
verified by code trace and a full rebuild through the real Kodi
harness only, matching this function's own already-established
verification boundary.
**Update (2026-09-27, a 56th-pass audit): the fetch mechanism this
entry describes (a plain `body.reserve()` into an unbounded
`std::string`) is no longer current -- a 55th-pass audit replaced it
entirely with a `FixedBufferSink` bounded to this same ceiling, fixing
a separate, already-logged gap this same replacement also closed (see "In-progress read path didn't clamp segment read window to probed byteSize"). The
underlying risk this entry describes (an unbounded value reaching an
allocation sized from an untrusted `Content-Length`) is unchanged in
spirit, just via a different, now-current mechanism.**

#### Series-rule title comparison: non-ASCII whitespace and case

**`MatchRecordingsToSeriesRules()`'s title comparison still only
mirrors Dispatcharr's own "exact" `title_mode` match for ASCII, flagged
from a 68th-pass audit (2026-09-27) -- both gaps confirmed live, the
database-locale question settled, and fixed, 2026-09-30.** The same pass fixed the
ASCII-whitespace half (`SeriesRuleMatching.cpp` now strips the rule's
title the way `evaluate_series_rules_impl()`'s own `.strip()` does, see
`docs/RECORDINGS.md`'s own entry on it), but two non-ASCII gaps remain,
both confirmed against Dispatcharr's own real current upstream source,
neither independently reproduced: Python's `str.strip()` also strips
non-ASCII Unicode whitespace (e.g. a no-break space) this addon's own
strip leaves in place, and `title__iexact` compiles to PostgreSQL's
`UPPER()`, which is Unicode-aware under a UTF-8 `LC_CTYPE` while this
addon's own `dispatcharr::ToLower()` (`StringUtil.h`) is ASCII-only --
so a rule whose title differs from its programmes' only in non-ASCII
letter case (e.g. an accented capital) would still record server-side
but never link back here, the same Unix-epoch/unparented-timer symptom.
Whether the second one is real at all depends on the database's own
`LC_CTYPE`, which `docker/init/02-postgres.sh`'s own `initdb` calls
don't set explicitly (inherited from the container environment) -- not
checked. A fix needs real Unicode case folding and whitespace
classification (a dependency this project doesn't currently pull in)
or a narrower, UTF-8-aware subset; not attempted blind.
**Live check (2026-09-30): both non-ASCII gaps confirmed on the addon
side.** Disposable exact-mode rules pinned to one real channel against future
recordings with chosen titles: a rule with a trailing no-break space
(the server stores the rule title unstripped, so the API does not
normalize it) stays unlinked from a recording with the same title minus
the space, and a rule "ZZZ_TEST ÜNDER TEST" stays unlinked from a
recording "ZZZ_TEST ünder test" (Kodi shows both rules as "Any day
at any time"). A first attempt at the case test accidentally used a
capital Ü in both titles and linked, which is the ASCII-only
folding working as described. What this run does NOT show: whether
Dispatcharr's own `title__iexact` evaluation folds non-ASCII case on
this instance's database -- the tempting signal (deleting the case-
mismatched rule did not purge its recording) is not evidence, because
`future_recordings_for_series()` purges with a case-sensitive exact JSON
match, unrelated to evaluation. Still needs a real EPG title to settle.
Not fixed.

**Database locale settled (2026-09-30), by a probe that needs no real EPG
title.** Dispatcharr's channel list filters with `icontains`, which
PostgreSQL evaluates as `UPPER(name) LIKE UPPER(...)` -- the same function
`title__iexact` uses. A disposable channel named "ZZZ_TEST ÜNDER Probe"
was found by `?name=zzz_test ünder probe` (lowercase ü, every letter's
case different), by the ASCII-only-differing "ZZZ_TEST ÜNDER PROBE", and by
the SearchFilter's own `search=` parameter: **the real instance's database
folds non-ASCII case**, so the second gap below is real on the server side
too, not just on this addon's. (The channel was deleted afterward, 404
confirmed.)
**Fixed (2026-09-30).** New `UnicodeText` (`UnicodeText.{h,cpp}`):
`FoldCaseForDatabaseMatch()`, `StripUnicodeWhitespace()` and
`IsDatabaseWordCodePoint()`, UTF-8 in and out, no locale dependency, an
invalid byte passed through as an opaque character of its own. Whitespace
is exactly Python's `str.isspace()` set (29 code points -- the no-break
space, U+0085, U+1680, U+2000-U+200A, U+2028/2029, U+202F, U+205F, U+3000,
and the ASCII and \x1c-\x1f separators), because that is what the server's
`.strip()` removes. Case folding is a fixed, generated snapshot of the
*simple* uppercase mapping (`src/UnicodeTables.inc`, produced by
`tools/gen_unicode_tables.py` from glibc's `towupper()`/`iswalnum()` --
what PostgreSQL's `UPPER()` and regex `\y` use on a libc-collation
database, the stock Debian-based postgres image -- rather than from
Python, whose `str.upper()` is the *full* mapping: it turns a sharp s into
"SS" and expands 27 Greek letters with prosgegrammeni into two code
points, where glibc maps them one-to-one, and whose `isalnum()` disagrees
with `iswalnum()` on a couple of thousand code points). 1,424 mappings in
199 ranges, 770 alphanumeric ranges. Checked exhaustively, not sampled:
every one of the 1,112,064 scalar values was folded, word-classified,
UTF-8 round-tripped in both directions and whitespace-stripped, and
compared with glibc (for the first three) or Python (for the last) --
zero mismatches. A deployment whose database uses a different collation
provider (ICU, a non-glibc libc, a `C` locale) would fold differently;
the stock image doesn't, and that is what this follows. The two gaps
below are closed by `MatchRecordingsToSeriesRules()` calling these (see "Series rules with non-exact title_mode never link to their recordings" for the rest of that change).
**Re-verified live (2026-09-30)**, real Kodi against the real instance, the
same two rules as the live check above: a rule with a trailing no-break
space (the server stores it unstripped) now links to a recording with the
same title minus the space, and a rule "ZZZ_TEST ÜNDER TEST" now links to a
recording "ZZZ_TEST ünder test" -- both previously "Any day at any time",
now showing the recording's start time.

#### Auth retry loop never gives up or backs off on a hard auth failure

**Auth-retry loop never gives up or backs off on a hard authentication
failure, surfaced by a real early-tester bug report (2026-09-17).** A
new tester reported being unable to connect at all, with Dispatcharr
showing both "too many requests" and "no account found" against two
different credential sets they tried (a web-GUI admin account and an
Xtream-Codes-style "streamer" user/pass). Root cause of the "no account
found" half: this addon only supports Dispatcharr's native REST login
(`POST /api/accounts/token/`) -- there's no XC auth path at all, and
`docs/API_NOTES.md` already documents that a Dispatcharr "Streamer"-role
account gets rejected by that endpoint outright ("No active account
found"), regardless of whether the password is correct.

Added a note to `README.md`'s own Configuration section warning about
this -- doc-only, landed directly on `master`.

Still open, and the likely source of the "too many requests" half:
`EnsureAuthenticated()` re-attempts `Login()` before nearly every API
call with no memory of a previous hard failure, and the realtime-updates
WebSocket reconnect loop retries with backoff (2s, doubling to a 60s
cap) indefinitely on top of that -- with wrong or role-incompatible
credentials, neither ever stops retrying, so the addon keeps hammering
`/api/accounts/token/` on every periodic refresh and every reconnect
cycle. That's a plausible way to trip Dispatcharr's own login
rate-limiter on its own, independent of whatever the user first got
wrong. Not yet fixed: would need `EnsureAuthenticated()` (and the
realtime-update reconnect loop) to distinguish a hard/permanent auth
failure from a transient one and stop retrying (or back off much
further) rather than retrying forever -- a real behavior change, not a
doc fix, so it needs its own branch and live verification (a wrong-
password/streamer-role account against a real instance) before merging,
per this file's own conventions.
**Update: implemented, on branch `fix/auth-retry-backoff` (2026-09-17),
live verification against a real bad-credentials case still pending.**
New `dispatcharr::ComputeLoginBackoffSeconds()` (`AuthBackoff.h`/`.cpp`,
covered by `tests/test_auth_backoff.cpp`) -- exponential, 30s initial,
doubling, capped at 30 minutes. `EnsureAuthenticated()` now tracks
`m_consecutiveLoginFailures`/`m_loginBackoffUntil` (reset only on a
successful `Login()`) and, while backed off, returns the cached error
from the last real attempt instead of calling `Login()` again --
applies uniformly to every caller through the one shared client
(periodic channel/EPG/recording refreshes and the realtime-update
reconnect loop's own `GetAccessToken()` call alike), so no separate fix
was needed in `PVRDispatcharr.cpp`'s reconnect loop itself. Standalone
Catch2 suite (`cmake -S tests -B build-tests && cmake --build
build-tests`) passes in full (525 assertions/228 cases, including the 5
new ones), and a full rebuild through Kodi's own binary-addon harness
(`~/kodi-build`, both stale-marker locations cleared first per this
file's own documented gotcha) also succeeds cleanly.
**Update: merged (squash-merged to `master`, 2026-09-17).**
Still not done: a real live run against an actual wrong-password/
Streamer-role account to confirm the backoff visibly stops the
repeated `/api/accounts/token/` POSTs in `kodi.log` -- the fix shipped
on the strength of the unit tests and a clean build alone, not a live
bad-credentials reproduction.

**Live check (2026-09-30): confirmed, and closed out.** Real Kodi on the Linux test client, its addon pointed (Kodi stopped, settings edited) at a counting TCP forwarder in front of the real instance, with a username that doesn't exist, a wrong password and an empty `api_key`, left running for four minutes. `POST /api/accounts/token/` went out at +0s, +31s, +124s and +245s: the 30s / 60s / 120s exponential waits `ComputeLoginBackoffSeconds()` calls for, each attempt landing on the first 1-minute background-thread tick after its wait expired -- four logins in four minutes where the unthrottled loop issued one per call. Six requests of any kind reached the server in total (four 401s, two 200s). Settings and the forwarder were restored afterward.

#### UpdateTimer() drops a channel change on a not-yet-started one-time recording

**`UpdateTimer()`'s not-yet-started one-time-recording branch silently
drops a channel change, flagged from a project-wide review
(2026-09-26), needs live confirmation before fixing.** Kodi's own
timer-edit dialog lets a user change a scheduled (not yet started)
one-time recording's channel (`GetTimerTypes()` declares
`PVR_TIMER_TYPE_SUPPORTS_CHANNELS` for this timer type), but
`DispatcharrClient::UpdateOneTimeRecording()` only ever PATCHes
`start_time`/`end_time` -- a channel change is accepted with no error
and simply never reaches Dispatcharr. `CreateOneTimeRecording()`
confirms `channel` is a real, writable field on this same Recording
resource at creation time, so it's plausible the same PATCH endpoint
also accepts it -- but unconfirmed whether Dispatcharr's own
serializer handles a channel change safely for an *existing* recording
(e.g. whether it correctly resets `custom_properties.program`'s
EPG-derived title/subtitle/description for the new channel, or leaves
a stale match tied to the old one). Not changed blind. If confirmed
live that a bare `{"channel": ...}` PATCH works cleanly, add it
alongside `start_time`/`end_time`; if Dispatcharr doesn't support this
at all, the honest fix is rejecting the edit outright (comparing
`timer.GetClientChannelUid()` against the recording's own
`rec.channelId`, both already available at this call site) rather than
silently accepting and discarding it.
**Update (2026-09-26, an 18th-pass audit): much more strongly
confirmed via Dispatcharr's own real current upstream source (cloned
into a scratchpad, never committed to this repo -- still not the same
standard as a live test, which remains the one gate this entry
deliberately set before changing anything).** `RecordingSerializer`
does use `fields = "__all__"`, so `channel` is genuinely writable via
a bare PATCH. The `revoke_old_task_on_update` pre_save signal
(`apps/channels/signals.py`) revokes the recording's own Celery task
whenever `channel_id` (among `start_time`/`end_time`) changes -- unless
`custom_properties.status == "recording"`, in which case it
deliberately skips revoking (exactly this addon's own already-fixed
in-progress-edit routing: a channel change on an *in-progress*
recording should go through the dedicated extend path instead, not
this one, which only ever handles the not-yet-started case) -- and the
paired `schedule_task_on_save` post_save signal reschedules afterward.
A `{"channel": X}` PATCH on a not-yet-started recording should
therefore reschedule cleanly. The one remaining, still-unconfirmed
caveat stands: `custom_properties.program` (the EPG-derived title
snapshot) isn't reset by either signal, so it would stay tied to the
old channel's programme data after a channel change -- cosmetic
(title/subtitle/description), not a scheduling risk, but still real if
true. A live test remains the right final gate before adding this,
given how much stronger the confirmation already is.

**Live check and fix (2026-09-30).** Against the real instance, a bare `{"channel": ...}` PATCH on a scheduled recording is a **500** (the same uncaught `end_time < now` on a missing `end_time` as any other partial update, see `UpdateOneTimeRecording()`'s comment), while the same PATCH with both times alongside it is a 200 that moves the recording to the new channel; `custom_properties.program` is left exactly as it was, so a recording that had an EPG-derived snapshot keeps the old channel's title -- the cosmetic caveat above, confirmed. `BuildOneTimeRecordingPatchBody()` now takes an optional channel id and sends it beside the two times, and `ChannelToSendOnOneTimeEdit()` (`TimerRequestBuilder.h`, tested) passes only a real channel that differs from the recording's own, so an edit that doesn't touch the channel never rewrites it and Kodi's "no channel" sentinel never reaches the server. Verified through Kodi's own edit dialog on the Linux client: a scheduled manual recording on one channel, Channel changed to the next one in the list, OK -- the server recording's `channel` moved and its times and program snapshot were unchanged. The recording was deleted afterward.

#### Series rule edit with no channel_id: 400 error and Any-channel support

**Editing a series rule with no `channel_id` used to fail outright
with a 400, not create a duplicate as originally guessed -- confirmed
and partly fixed by an 18th-pass audit (2026-09-26).** Confirmed
against Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- a real source read, not a
live test): channel-less series rules are real and reachable in
practice, not just a hypothetical -- Dispatcharr's own "Record series"
Guide button creates one with no pinned channel. `UpdateTimer()`'s
cache lookup does miss for such a rule (comparing the cached
`channelId <= 0` against `PVR_CHANNEL_INVALID_UID`, -1 -- two
different sentinel conventions for "no channel" that were never
reconciled), but the fallback path was traced through and found benign
for `tvgId` specifically (both paths converge on the same empty
value) -- left as a minor, inconsequential inconsistency, not fixed.
**The real, confirmed bug was elsewhere:** `CreateSeriesRule()`
unconditionally included `channel_id` in its POST body (unlike
`tvg_id`, already conditionally omitted) -- sending the sentinel
literally (`channel_id: -1`) instead of omitting it, which
Dispatcharr's own validation rejects outright with a 400 ("channel_id
does not exist"), since Dispatcharr's own schema documents `channel_id`
as genuinely optional too ("Optional channel to pin recordings to").
Fixed by omitting `channel_id` from the body whenever `channelId <= 0`,
mirroring the existing `tvg_id` pattern -- both editing and (in
principle) creating a channel-less series rule now succeed instead of
failing outright. **Still open, not fixed this pass:** such a rule
still displays the Unix-epoch placeholder in Kodi's Timers list and
its recordings still appear unparented, since
`MatchRecordingsToSeriesRules()` requires `channelId == rec.channelId`
and a channel-less rule's own `channelId` is 0 -- confirmed a better
linking key exists (`custom_properties.program.tvg_id` on the
recording, matching the server's own identity concept for such a
rule), but using it needs `RecordingParser` to also read that field, a
larger change than this pass's other fixes, not attempted blind.
**Update (2026-09-26, a 30th-pass audit, confirmed against Kodi's own
real current SDK source, not itself independently reproduced): this
item's own "left as a minor, inconsequential inconsistency" and "both
paths converge on the same empty value" claims were wrong, and the fix
above never actually reached a real Kodi-driven edit at all.** Traced
through Kodi's real `GUIDialogPVRTimerSettings.cpp`: for an *existing*
timer whose channel uid is already `PVR_CHANNEL_INVALID_UID`, Kodi's
own dialog only auto-selects a channel-list entry when the timer type
declares `PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL` (this addon's series
type never did) -- its "select first regular channel entry" fallback
applies only to a *brand-new* timer, never an edit. Without the flag,
`m_channel` stayed at its own default (`clientId -1`), which Kodi's
`CPVRTimerInfoTag::UpdateOnClient()` then failed to resolve to any
real PVR client at all -- silently failing the *entire* edit dialog
("Could not update the timer") before this addon's own `UpdateTimer()`
was ever called, for any field, not just the channel. So the 18th
pass's own "fallback path... found benign" trace was necessarily never
exercised by a real Kodi-driven edit of a channel-less rule -- there
was no live or JSON-RPC-automated test of the actual dialog save path,
only source reading, and the dialog-level blocker above was missed.
Separately, the "converges on the same empty value" claim was also
wrong on its own terms whenever the rule actually has a non-empty
cached `tvgId` -- true for every guide-created channel-less rule
(Dispatcharr's own "Record series" button always sends a `tvg_id`) --
since the fallback (`channelFound=false` -> `tvgId=""`) does not equal
a non-empty cached value; Dispatcharr's own upsert would then append a
brand-new, match-every-channel rule instead of editing the original.
**Fixed this pass:** `PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL` added to the
series timer type (`GetTimerTypes()`), and the cache-match comparison
in `UpdateTimer()` now goes through `dispatcharr::IsSameSeriesRuleChannel()`
(`TimerIdentity.h`), which treats a cached `channelId <= 0` and a
timer's `PVR_CHANNEL_INVALID_UID` the same rather than comparing them
directly. **Still needs a live check, not done this pass:** adding
`SUPPORTS_ANY_CHANNEL` also changes Kodi's dialog for a *brand-new*
manually-added series timer (an "Any channel" entry becomes
selectable there too, not just reachable when editing an existing
channel-less rule) -- confirmed as the intended, working mechanism via
source reading, but the actual dialog's on-screen behavior hasn't been
observed live.

**Live check (2026-09-30): the open item above is confirmed and closed.** Real Kodi on the Linux client. A channel-less series rule created through the API (no `channel_id`, as Dispatcharr's own guide "Record series" button makes) is listed with no channel; opening its edit dialog shows Channel "Any channel" and pressing OK after changing "Prevent duplicate episodes" updated that one rule (`mode` all -> new) with no duplicate rule and no error -- before `SUPPORTS_ANY_CHANNEL` this dialog failed before reaching the addon. For a brand-new series timer the Channel list now starts with "Any channel" (the channels plus it), and creating one with "Any channel" and a "Search guide for" text produced a rule with `channel_id` null and that text as its title. Disposable rules were deleted afterward; the instance's original rules are untouched.

#### Channel/EPG caches don't age while the device is suspended

**Suspected: channel/EPG caches don't age while the device is
suspended (Linux/Android/CoreELEC), flagged from a 21st-pass audit
(2026-09-26), confirmed against Kodi's own real source for the
mechanism, not reproduced live.** Every staleness timestamp
(`Staleness.h`'s `IsStaleSince()`/`ShouldFetchChannels()`, backing
`EnsureChannelsLoaded()`/`EnsureEpgLoaded()`) uses
`std::chrono::steady_clock`, which on Linux (libstdc++/libc++) is
backed by `CLOCK_MONOTONIC` -- a clock that does not advance during
system suspend. Kodi's own `CPVRManager::OnWake()`
(`xbmc/pvr/PVRManager.cpp`) already re-requests channels and EPG on
wake, but this addon's own `OnSystemWake()` only nudges the
realtime-update WebSocket thread, not `m_channelsLoadedAt`/
`m_epgLoadedAt` -- so a device suspended for multiple days could wake
up still serving a channel list/XMLTV guide fetched before the
suspend, for up to `channel_refresh_hours`/`epg_refresh_hours` of
*awake* time afterward (the dummy/fallback EPG window is only ~3 days
server-side, confirmed against `apps/output/epg.py`, so a long enough
suspend could leave the cached guide entirely in the past on wake).
Not confirmed whether Windows (`QueryPerformanceCounter`) or macOS
(libc++) `steady_clock` behaves the same way across sleep -- only
Linux's `CLOCK_MONOTONIC` semantics were checked. Fix direction:
`OnSystemWake()` resets both loaded-at timestamps under
`m_dataMutex`, forcing a fresh fetch on the very next
`EnsureChannelsLoaded()`/`EnsureEpgLoaded()` call -- low risk, but
needs a live suspend/resume test to confirm the on-device symptom
before implementing blind.

**Fixed defensively (2026-09-30); the symptom itself could not be reproduced.** The mechanism is plain (`CLOCK_MONOTONIC` does not count suspended time, and every staleness check here runs on `steady_clock`), but suspend could not be exercised for real (no `CanSuspend`, no `rtcwake`), and a spoofed logind `PrepareForSleep(false)` signal never reached Kodi's wake handler. So the fix is reasoned and unit-tested rather than observed. `OnSystemWake()` now ages the channel and guide loaded-at timestamps to "just stale" (`MarkStaleForWake()`, `Staleness.h`) so Kodi's own re-request right after the wake (`CPVRManager::OnWake()`) gets a fresh fetch instead of the pre-sleep cache. Deliberately not reset to the zero "never loaded" sentinel: that would make `GetChannels()` and friends return an error on a cache that holds real data, which Kodi reads as permission to delete it. A cache that had never loaded stays that way.

#### WebSocket reconnect can reuse an expired JWT after suspend

**Suspected: the same `steady_clock`-doesn't-advance-during-suspend
mechanism as the "Channel/EPG caches don't age while the device is suspended" entry also lets the WebSocket reconnect
reuse an already-expired JWT after a long suspend, flagged from a
61st-pass audit (2026-09-27), confirmed against Dispatcharr's own real
upstream source for the mechanism, not reproduced live.**
`EnsureAuthenticated()`'s own access-token cache
(`m_accessTokenExpiry = steady_clock::now() + minutes(4)`,
`DispatcharrClient.cpp`) has the identical monotonic-clock blind spot,
just a much shorter (4-minute) window than the channel/EPG caches'
own hours-long one -- a token obtained less than 4 *monotonic* minutes
before a suspend lasting longer than Dispatcharr's own real 30-minute
`ACCESS_TOKEN_LIFETIME` (confirmed against `dispatcharr/settings.py`)
still reads as fresh to `GetAccessToken()` on wake. Confirmed against
Dispatcharr's own real upstream source (`dispatcharr/jwt_ws_auth.py`/
`consumers.py`) that this genuinely breaks the reconnect, not just in
theory: `MyWebSocketConsumer.connect()` calls `self.close()` *before*
`self.accept()` when the JWT fails validation, so
`IsWebSocketHandshakeAccepted()` correctly reports this as a rejected
handshake -- but unlike `Request()`'s own reactive 401-triggered
refresh/login path, nothing on this WebSocket path ever invalidates
`m_accessTokenExpiry`, so the reconnect loop keeps retrying with the
same stale-but-monotonically-still-valid token (with its own backoff)
until either the 4-minute hint naturally elapses or some unrelated
HTTP call happens to get a real 401 and refreshes it first. Real
impact is likely small in practice: `OnSystemWake()` already triggers
a timer refresh, which routes through the reactive 401 path and would
usually refresh the token within seconds anyway -- so this is more a
latent gap than a confirmed user-visible failure. Possible fix
direction (not attempted blind): add something like
`DispatcharrClient::InvalidateAccessToken()` and call it either from
`OnSystemWake()` (folding into that entry's own eventual fix) or
from `Connect()`'s own handshake-rejected path specifically -- needs a
live suspend/resume test first to confirm real-world reachability and
timing before choosing between those directions.

**Fixed defensively (2026-09-30), same caveat as the entry above: not reproduced, no way to suspend the test VMs.** `OnSystemWake()` now calls the new `DispatcharrClient::InvalidateAccessToken()`, which drops the cached token's freshness hint so the next authenticated call -- including the WebSocket reconnect's -- refreshes first, using the existing refresh-then-login path. The alternative this entry floated (invalidating from `Connect()`'s handshake-rejected path) was not needed.

#### Legacy/alias IANA zone names unrecognized by kKnownTimeZones

**Low confidence, needs a live check before acting: a timezone name
chosen through Dispatcharr's own web UI settings picker may use a
legacy/alias IANA id `kKnownTimeZones` (`TimeZoneUtil.cpp`) doesn't
recognize, flagged from a 36th-pass audit (2026-09-26).** Dispatcharr's
own timezone picker (`frontend/src/utils/dateTimeUtils.js`) builds its
options from the browser's own `Intl.supportedValuesOf('timeZone')`
and seeds a default from `Intl.DateTimeFormat().resolvedOptions().timeZone`
(`UiSettingsForm.jsx`) -- not from `pytz.common_timezones` server-side.
Chromium/V8 has historically returned CLDR-legacy ids for some zones
(e.g. `Asia/Calcutta` rather than `Asia/Kolkata`, `Europe/Kiev` rather
than `Europe/Kyiv`) that don't necessarily appear in `pytz.common_timezones`
either -- not independently re-verified for a current 2026 browser, so
this is a plausible mechanism, not a confirmed one. If a real instance's
configured system timezone is one of these, this addon's constructor
would treat it as unrecognized and silently fall back to the manual
offset setting (default 0) -- silently mis-shifting every recurring
rule's own schedule by the zone's full offset (5.5 hours for
`Asia/Calcutta`/`Asia/Kolkata`) with no error surfaced anywhere.
Needs a live check first (what does a real Dispatcharr instance's own
`system_settings.time_zone` actually contain, chosen through its own
browser-based picker) before implementing anything -- if confirmed, the
fix is a small alias-to-canonical map inside
`ComputeKnownZoneOffsetMinutes()`.
**Live check (2026-09-29): negative on the real lab instance.**
`GET /api/core/settings/`'s `system_settings.time_zone` is a standard, canonical IANA name, not one of the
legacy/alias ids this entry worried about. Doesn't refute the general
mechanism (this instance's own configured zone just never exercises
it) -- still needs a real instance actually configured with one of
the legacy names (`Asia/Calcutta`, `Europe/Kiev`, etc.) to confirm or
refute for real.

**Confirmed, fixed and re-verified live (2026-09-30).** The premise holds: Dispatcharr's settings page fills its picker from `Intl.supportedValuesOf('timeZone')`, and a real V8 (Node 22, ICU 78) lists `Asia/Calcutta`, `Europe/Kiev` and `America/Indianapolis` and **none** of `Asia/Kolkata`, `Europe/Kyiv` or `America/Indiana/Indianapolis` -- the three table zones it spells differently (every other table zone is offered under its table name). So anyone who picked India, Ukraine or Indianapolis in Chrome got `recurring_rule_timezone` = manual. `TimeZoneUtil.cpp` now has an alias table (those three, plus IANA's `backward` links to table zones: `US/*`, `Canada/*`, the old country-name ids) and `CanonicalKnownZoneName()`; `ComputeKnownZoneOffsetMinutes()` accepts an alias and `SyncTimezoneFromDispatcharr()` selects the canonical name in the dropdown, which only knows the modern one. Live, on the Linux client with a forwarder rewriting the real instance's reported `system_settings.time_zone` to `Asia/Calcutta`: the addon logged `setting recurring_rule_timezone=Asia/Kolkata (known zone)` and stored it, `dispatcharr_timezone_info` still showing the raw `Asia/Calcutta`. Settings were restored afterward.

#### Moving one recurring occurrence duplicates it; rule edits revert it

**Confirmed from Dispatcharr's own current upstream source, not live:
moving one occurrence of a recurring rule from Kodi creates a
duplicate within the hour, and any rule-level edit (including this
addon's own periodic renewal) silently reverts it, flagged from a
19th-pass audit (2026-09-26).** `UpdateTimer()`'s plain one-time
branch is reachable for a recurring rule's own materialized child (a
`Recording` carrying `recurringRuleId`/`rule.id`), and nothing in this
addon or, per the cloned source, in Dispatcharr's `maintain_recurring_recordings`
Celery beat task (running `sync_recurring_rule_impl(drop_existing=False)`
roughly hourly) tracks "this specific occurrence was already
rescheduled" -- that task only checks whether a recording already
exists at the rule's own *original*, unmodified `start_time` for that
slot. Editing a child's start time client-side leaves the original
slot looking empty to the next beat-task pass, which recreates it --
producing two recordings covering the same nominal occurrence, using
two separate provider streams. Separately, *any* full rule PATCH
(`perform_update`) runs `sync(drop_existing=True)`, purging and
regenerating every future occurrence from the rule's own base
schedule -- which silently discards a per-occurrence time edit or
rename the next time this addon's own `RenewRecurringRules()` pushes
`end_date` forward (every ~`kRecurringRuleWindowDays`/2 days), or the
user toggles the rule's own enabled state. `docs/RECURRING_RULES.md`
already documents that *deleting* a single occurrence can reappear via
the same idempotency-check mechanism; the duplicate-on-edit and
revert-on-renewal consequences of *editing* one are new information,
not previously written down anywhere. Possible fix: reject a time/date
edit on a child with `rec.recurringRuleId != 0` outright (Kodi already
has no problem with a timer type declining a particular edit), rather
than silently producing behavior a user watching Kodi's own timer list
would have no way to explain. Worth a live check first to confirm the
beat task actually behaves this way in practice (task queue backlog,
timing skew, etc. could all change the practical window) before
changing behavior.

**Live check (2026-09-30): both halves confirmed, and the edit is now refused.** Against the real instance, with a disposable enabled daily rule (ten occurrences, later thirty after a renewal). *Duplicate:* one occurrence's `start_time`/`end_time` PATCHed two hours later (as `UpdateOneTimeRecording()` would); Dispatcharr's hourly `maintain_recurring_recordings` task ran 48 minutes after the edit, and a **new recording appeared at the original 09:00Z slot beside the edited 11:00Z one** -- 31 recordings where there had been 30, two provider streams for one nominal occurrence. *Revert:* the first edit was wiped without any action of mine four minutes after it, when the addon's own `RenewRecurringRules()` (running on the Linux client) PATCHed the rule's `end_date` from 10 to 30 days out: Dispatcharr purged and regenerated every occurrence with new ids and the edited slot was back at its original time. `IsRecurringOccurrenceReschedule()` (`RecordingVisibility.h`, tested) now makes `UpdateTimer()` refuse a start/end change on a recording that belongs to a recurring rule, before any server call, with a log line and a notification pointing at the rule; a title or channel edit on such an occurrence is unaffected. Verified through Kodi's own dialog on the Linux client: an occurrence's context menu offers "Edit timer rule" (which edits the rule, and worked) and a plain "Edit" for the occurrence itself; changing that one's start time produced Kodi's "Could not update the timer" error and the addon's notification, and the server recording was left exactly as it was. The disposable rule and its occurrences were deleted afterward.

#### CURLOPT_NOSIGNAL is never set

**`CURLOPT_NOSIGNAL` is never set on any curl handle
(`ApplyStandardCurlOptions()`), flagged from a project-wide review
(2026-09-26), low priority.** Only matters for a libcurl built without
the threaded resolver (rare on any platform this addon actually
targets) using a timeout from a non-main thread, where libcurl's
default signal-based timeout handling can misbehave. Cheap, safe
one-line hardening (`curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L)`) --
not applied this pass alongside everything else, but worth picking up
opportunistically next time this function is touched.

**Fixed (2026-09-30).** Every curl handle this addon creates now sets
`CURLOPT_NOSIGNAL`: `ApplyStandardCurlOptions()` for the ones that go through
it, and directly on the four that set their own timeout (the XMLTV fetch, the
timeshift heartbeat, the recording-stream probe and the WebSocket
handshake). A no-op for a threaded-resolver libcurl, so nothing to observe;
it was built and exercised live with the rest of this batch.

#### GetConnectionString() doesn't bracket an IPv6 host

**`PVRDispatcharr::GetConnectionString()` doesn't bracket an IPv6 host
literal the way every other URL/Host header this addon builds now
does, flagged from a 9th-pass audit (2026-09-26), cosmetic, not fixed
this pass.** Purely a display string shown in Kodi's own connection-
status UI, never used for an actual network operation -- a bare IPv6
host would render as e.g. `::1:9191` instead of `[::1]:9191`, visually
ambiguous but not functionally broken. Also reads the *live* `host`/
`port` settings rather than `m_lastAppliedConfig`'s own applied value,
which may or may not be the more useful thing to show in a status
display (genuinely ambiguous, not obviously a bug) -- left alone
rather than changed on a guess.

**Fixed (2026-09-30).** `GetConnectionString()` now goes through
`FormatHostForUrl()` like every other host:port this addon builds, so an IPv6
literal reads `[::1]:9191`. The second half of the entry (live settings vs
`m_lastAppliedConfig`) turned out not to matter: a changed connection setting
updates `m_lastAppliedConfig` on the same call and then asks Kodi to restart
the addon, so the two never differ for any length of time. Left as is.

#### SUPPORTS_TITLE_EPG_MATCH declared on one-time timer types

**`PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH` is also declared on both
one-time timer types, where it does nothing, flagged from a 16th-pass
audit (2026-09-26), cosmetic only.** Only the series type's own
`AddTimer()`/`UpdateTimer()` branches read `GetEPGSearchString()` (see
this same pass's fix); the two one-time types never look at it at
all, so Kodi's "Search guide for" field shows up in their
timer-settings dialogs as a dead, do-nothing field. Not removed this
pass: the flag's own broader role in Kodi's own timer-type selection
during `CreateFromEpg()` (the "Record" button on an EPG guide entry)
wasn't fully traced, so removing it blind risked breaking that
create-from-guide flow for one-time recordings rather than just
tidying a cosmetic field -- left alone until that's confirmed safe.

**Fixed (2026-09-30).** The flag is gone from both one-time types. The
question the entry left open -- whether it matters to the guide's "Record"
button -- is answered by Kodi's own source: `CPVRTimerInfoTag::CreateFromEpg()`
reads the title-match attribute only on its repeating-rule branch
(`bCreateRule`), the one-time branch picks a type by other attributes, and
`CPVRTimerRuleMatcher` uses the flag only to match a repeating rule's own
search text. Confirmed live: Kodi's "Add timer" dialog for the manual
one-time type no longer shows "Search guide for", and a one-time recording
created through Kodi's own Record action on a guide entry is still made.

#### Two refreshes can fetch and parse the full XMLTV guide concurrently

**Two channel/EPG refreshes can both fetch and parse the full XMLTV
guide concurrently, flagged from a 24th-pass audit (2026-09-26), low
severity, not reproduced live.** Neither `EnsureChannelsLoaded()` nor
`EnsureEpgLoaded()` guards against two callers both seeing a stale
cache and both starting a fetch -- at startup specifically, the
background channel/EPG refresh thread's own first XMLTV fetch and
Kodi's own `GetEPGForChannel()` call path (Kodi's own EPG thread) can
both race in this window. Dispatcharr's own EPG output is cached
server-side, so the server cost is mostly absorbed; the client pays
double bandwidth and double XML parsing for one redundant fetch. Not
pure logic and not pursued this pass -- could be folded into the
same generation-counter fix already sketched for the "Cache-invalidation triggers can be overwritten by an in-flight fetch" entry, if that's ever tackled.

**Fixed (2026-09-30).** `EnsureChannelsLoaded()` and `EnsureEpgLoaded()` each
serialize their fetch-and-commit on a mutex of their own
(`m_channelsFetchMutex`/`m_epgFetchMutex`); a second caller that found the
cache stale at the same moment waits, re-runs the same gate, and finds the
first caller's result instead of fetching again. Both now stamp the cache
with the commit time rather than the entry time, so a waiter can tell "someone
loaded this after I started" from "nothing was due" -- and in the first case
`EnsureEpgLoaded()` returns true, so when Kodi's own thread wins the race the
background thread still runs its per-channel `TriggerEpgUpdate()` pass. A
forced channel refresh (`forceStale`) still bypasses only the staleness check,
not the failure backoff, exactly as before. Confirmed live: a fresh Kodi start
against a counting proxy made exactly one `/output/epg` request.

**Superseded in part (2026-10-03).** Only the background thread calls
`EnsureEpgLoaded()` now, so a Kodi thread never waits on this mutex -- see
"Kodi's own threads waited on the guide fetch, and two transfers could not be
aborted at shutdown". The mutex stays as the guard it is.

#### Whitespace in pasted manual-test settings is misreported

**Trivial UX nit: a pasted `test_channel_uuid`/`test_recording_id`
manual-test-only setting value with surrounding whitespace fails
validation and is misreported, flagged from a 24th-pass audit
(2026-09-26).** `uuid.UUID()` (or the equivalent recording-id parse)
rejects the untrimmed value, and the resulting error message says
"channel_uuid is required" rather than something pointing at the
actual problem (stray whitespace) -- these are diagnostic-only manual
test settings, not part of any real user-facing flow, so essentially
cosmetic; not pursued this pass.

**Fixed (2026-09-30)** for `test_channel_uuid`: `_resolve_channel_uuid()`
strips the value before parsing, so a pasted UUID with a trailing newline or
space works and a whitespace-only value is refused as "required", which is
what it is. `test_recording_id` needed nothing: Django's integer lookup
already tolerates surrounding whitespace, and a value that really is invalid
gets its own "Invalid recording_id" message. `timeshift_buffer`'s version was
not bumped for a change to a manual-test-only setting; it rides along with
the next release's own bump.

#### Persistent /output/epg failure is retried with no backoff

**A persistent `/output/epg` failure (e.g. the Network Access 403 a
24th-pass audit gave a diagnostic for) is retried with no backoff and
can flood Dispatcharr's own system-event log, flagged from a 25th-pass
audit (2026-09-26), confirmed against Dispatcharr's own real current
upstream source, not reproduced live.** `EnsureEpgLoaded()`'s own
failure-retry gate (`kChannelEpgFailureRetryMinutes`, a flat 1 minute)
has no exponential backoff the way `AuthBackoff`'s login-failure
handling already does -- called by the background thread every 10
minutes and on every Kodi `GetEPGForChannel()` sweep, a *persistent*
(not transient) failure like a Network Access block retries roughly
every minute indefinitely. Dispatcharr's own `epg_endpoint()` calls
`log_system_event('epg_blocked', ...)` on every single 403, which also
fires any configured integration and trims Dispatcharr's own
`max_system_events`-capped (default 100) event history -- so this
addon's own retry cadence alone could push every *other* system event
out of that history within about a day, and re-fire any configured
alert/integration hook that often. Suggested fix: a capped exponential
backoff for repeated EPG/channel fetch failures, mirroring
`ComputeLoginBackoffSeconds()`'s own shape -- a moderate change, not
attempted blind this pass.

**Update (2026-09-27, a 45th-pass audit): a 43rd-pass fix made this
worse for a channels/EPG fetch that has never succeeded even once
(not just a persistent failure after an initial success), then a
45th-pass fix partially restored the original rate -- still an open
design question, this update doesn't close it.** `ShouldUseShortChannelEpgRefreshWait()`
(`Staleness.h`, see `CLAUDE.md`'s own entry) shrank the background
channel/EPG refresh thread's own loop interval from 10 minutes to 1
while channels/EPG have never successfully loaded -- correctly fixing
a real startup-unplayable window (see that entry), but also making
`EnsureEpgLoaded()`'s own retry (and this same `epg_blocked`
amplification) up to 10x more frequent for the specific case this item
already describes, for as long as the underlying failure persists, not
just briefly at startup. A same-pass, 45th-pass fix raised
`kForceChannelRefreshMinAgeMinutes` from 2 to 10 minutes specifically
to restore the *channel refetch's* own original ~10-minute
amplification rate (see that constant's own comment,
`PVRDispatcharr.h`) -- but that only throttles the *channel*-side
refetch this force triggers, not `EnsureEpgLoaded()`'s own XMLTV
fetch attempt itself, which still retries every
`kChannelEpgFailureRetryMinutes` (1 minute) for as long as the
never-loaded condition holds. The capped-exponential-backoff direction
sketched above would still close this properly (and would need to
distinguish "never loaded" from "loaded once, now persistently
failing" if it's to avoid reintroducing the 43rd-pass startup-window
regression this update is about) -- not attempted this pass either.

**Fixed (2026-09-30).** A *durable* `/output/epg` rejection (any 4xx but 408:
chiefly the Network Access 403) now grows the retry interval -- 1, 2, 4, 8, 16
minutes, capped at 30 (`ComputeEpgFailureRetryInterval()`,
`ShouldCountTowardEpgFailureBackoff()` in `Staleness.h`, the EPG sibling of
`AuthBackoff`'s login split) -- while an outage (no response, a 5xx, 408)
stays on the flat one-minute retry, so a server that comes back gets its guide
within a minute. That split is what answers the update's worry about the
43rd-pass startup-recovery fix: the short wait for a never-loaded state is
about *channels*, which are untouched here, and an unreachable server never
counts as durable. The count resets on success, on a transient failure and on
a wake from sleep. `GetXmlTvGuide()` reports the HTTP status for it.
Confirmed live: a proxy answering `/output/epg` with 403 from a fresh Kodi
start saw attempts at 0, 60, 180, 420 and 907 seconds (the expected 1/2/4/8
minutes plus the background thread's own tick), each failure logging
"rejected N times in a row -- next retry in M minute(s)", against one per
minute before. Flipping the proxy back to pass-through, the next attempt
loaded the guide.

#### Recurring rule keeps dialog-carried seconds

**A recurring rule created from Kodi's timer dialog keeps whatever seconds
the dialog's start and end time carry, found live 2026-09-30 while
verifying the weekday shift, not fixed.** The dialog's default start time
is "now" and keeps its seconds (Kodi displays only minutes), and
a time typed on the numeric pad keeps the seconds of the value it replaced
rather than zeroing them. `ComputeRecurringRuleFields()` passes them
straight into `start_time`/`end_time` (`TimeOfDayString()` formats all three
fields), so a rule made from the default time reached Dispatcharr as
`HH:MM:28`-`HH:MM:28` and records from HH:MM:28, up to a minute after the
time the user saw. Truncating to whole minutes here would be wrong on its
own: Kodi echoes back the seconds it was shown on any edit (even a bare
enable/disable toggle), so every such edit would silently rewrite a rule
whose server-side seconds were nonzero. A fix would have to tell a
user-entered time from an echoed one, or zero the seconds only when
creating.

**Fixed (2026-09-30) for creation, not for edits -- as the entry's last
sentence suggested.** `AddTimer()` passes the rule's start and end time of day
through `TruncateRuleTimesToWholeMinutes()` (`RecurringRuleUtil.h`) after
`ComputeRecurringRuleFields()`; `UpdateTimer()` does not, since Kodi echoes the
seconds it was shown on every edit and truncating there would rewrite a rule
whose server-side seconds are nonzero (a rule made in Dispatcharr's own UI, or
by an older build). The one guard: if truncating would collapse a
start/end pair that differed into an equal one (a window inside a single
minute) both are left alone, because Dispatcharr reads `end <= start` as an
overnight rule. Confirmed live: a rule created from Kodi's own dialog with its
default times reached Dispatcharr as `HH:MM:00`-`HH:MM:00`.

#### DVR padding sync and admin check run only once, at startup

**The DVR padding sync and the admin-account check still run only once,
at construction, the same gap the timezone sync had before its own
47th-pass fix -- flagged from a 48th-pass audit (2026-09-27), confirmed
against this addon's own real current source, not reproduced live.**
`PVRDispatcharr`'s constructor syncs `recording_pre/post_offset_minutes`
from Dispatcharr's own real global DVR padding, and `dispatcharr_is_admin`
from a live permission check, both exactly once -- if either fails at
startup (Dispatcharr not yet reachable, the same startup-timing class
of gap fixed for channels/EPG/the API key/the timezone), neither ever
retries. Impact is lower than the timezone sync's own: the padding sync
is purely a *display* value (`SetDvrOffsetMinutes()`'s own merge-
preserving PATCH already reads Dispatcharr's real current value fresh
before pushing an edit, so a stale *displayed* value doesn't corrupt
what actually gets sent), and the admin check fails open (leaves
`dispatcharr_is_admin` at its default `true`), so the only real
consequence is the "Server-side mode with a non-admin account" warning
never appearing if the check itself failed at startup. Given the low
impact, not fixed this pass -- if picked up later, the same
extract-and-retry-from-the-background-thread pattern
`SyncTimezoneFromDispatcharr()` already established would apply
directly.

**Fixed (2026-09-30), and widened to the two catch-up flags, which had the same
gap.** `RetryDeferredServerSyncs()` runs at the top of each background-thread
cycle and retries whichever of the four startup syncs hasn't answered yet
(`SyncDvrPaddingFromDispatcharr()`, `SyncAdminStatusFromDispatcharr()`,
`SyncCatchupFlagsFromDispatcharr()`; the timezone sync already had its own
retry) -- extracted from the constructor, which still makes the first attempt.
Retry-until-first-success, not periodic: a server-side change made after a
successful first read is still picked up at the next restart, as before. The
catch-up flags became atomics (now written from the background thread), and a
late "off" triggers `TriggerChannelUpdate()` so the has-archive flag catches up.
The entry's impact assessment held; the catch-up flags mattered more than the
two it named, since failing open there means Kodi offering playback Dispatcharr
will refuse. Confirmed live: started against an unreachable server (nothing
synced, padding/admin/timezone left at deliberately wrong values), brought it
back, and the next cycle set all of them. See `docs/EPG.md`.

#### Failed DVR-padding push isn't retried on restart

**A silently-failed DVR-padding push isn't retried on the next
restart -- it's silently reverted, flagged from a 21st-pass audit
(2026-09-26), confirmed against this addon's own real code (the
constructor's own startup sync), not reproduced live.**
`OnAddonSettingChanged()`'s own comment on its detached
`SetDvrOffsetMinutes()` push used to describe the constructor's own
next-startup sync-from-Dispatcharr as "a natural retry" if the push
itself silently fails (a transient network blip, Dispatcharr
unreachable at the exact moment of the settings save). That's
actually backwards: the constructor's sync reads Dispatcharr's own
*real, current* value and overwrites Kodi's local setting to match
it -- so a push that never actually reached the server gets Kodi's
own displayed value quietly reverted back to the old one on the next
restart, not resent. The comment itself was corrected this pass; an
actual retry (e.g. a background retry queue, or simply re-attempting
the push once more before giving up) wasn't implemented -- low
severity (the user can always just re-save the setting), not pursued
this pass.

**Fixed (2026-09-30) -- by retrying in the background rather than on restart.** A
push that fails is kept as pending per side and resent each background cycle;
while one is pending the sync-from-server is skipped, because that sync is what
reverted the edit. After three failed retries the edit is dropped with a
notification (a non-admin account's push is refused every time). Confirmed live:
with the server unreachable, padding edited in Kodi's real settings dialog from 1
to 3 logged "will retry", and about a minute after the server came back the real
`dvr_settings` read 3 while Kodi still showed 3 (the server value was restored
to 1 afterwards). Not fixed: a restart before a retry succeeds still loses the
edit, since nothing is persisted. See `docs/EPG.md`.

#### Realtime reconnect doesn't force a refresh for missed events

**Realtime-update reconnect doesn't force a refresh to cover events
possibly missed while disconnected, flagged from a 20th-pass audit
(2026-09-26), code-reasoning only, low severity.** Nothing calls this
addon's own cache-invalidate/trigger-refresh path after a successful
`Connect()` that followed a dropped session -- a change made entirely
during the gap (a recording finishing, a timer being edited elsewhere)
waits for the next periodic refresh (`recording_refresh_minutes`, up
to 5 minutes by default) rather than showing up as soon as the
reconnect itself succeeds. Not pure logic, and low severity (the
periodic refresh already catches it eventually) -- not pursued this
pass.

**Fixed (2026-09-30).** The realtime thread arms "resync on next connect" when a
session ends or a connect fails, and on the next successful connect runs
`InvalidateAndTriggerTimerUpdate()` then `InvalidateAndTriggerRecordingUpdate()`
(timers first, for the `IsInProgress()` latch reason documented in
`HandleRealtimeUpdateMessage()`). Not armed before the very first attempt. The
cost is one extra timers-and-recordings fetch per reconnect. Confirmed live: after
a run where the first connect attempts failed, the reconnect logged the refresh.

#### Channel with a null (or zero/negative) channel number never gets a guide

**A channel with a genuinely null channel number never gets a guide at
all, flagged from a 24th-pass audit (2026-09-26), confirmed against
Dispatcharr's own real current upstream source, not reproduced live.**
`ChannelParser` maps a null `effective_channel_number` to `0`, and
`GetEPGForChannel()` returns early whenever `channelNumber <= 0` --
but Dispatcharr's own XMLTV export (`apps/output/epg.py`) still
exports such a channel, keyed by `str(channel.id)` instead of a
number. That guide data exists server-side but is never looked up,
since this addon's own cache is keyed by channel number only. Distinct
from (and simpler than) the already-logged duplicate-channel-number
EPG-collision item -- this is a channel with *no* number at all, not
two channels sharing one. How common a null channel number actually is
in practice is unknown; low priority, not pursued this pass.
**Update (2026-09-27, a 69th-pass audit): a literal channel number of
`0` (or a negative one) hits the same guard, confirmed against
Dispatcharr's own real current upstream source, not reproduced
live.** `ChannelSerializer.channel_number` (`apps/channels/serializers.py`)
has no `min_value` at all -- only the `ChannelOverride` serializer's
own `channel_number` sets `min_value=0.0001` -- so a channel saved with
number `0` via the API is accepted, and `apps/output/epg.py` exports
it keyed `"0"` (`format_channel_number(0)` is `0`, not the `""` that
would trigger the `str(channel.id)` fallback). `ChannelParser`'s own
null-to-`0` mapping makes that indistinguishable from the null case
here, and `GetEPGForChannel()`'s `channelNumber <= 0` guard skips it
the same way, so its real guide data is never looked up. Same fix
direction as the null case (it needs a separate "has a number" signal
from `ChannelParser`, not just the number itself); not pursued this
pass either.

**Fixed (2026-09-30), both halves.** Confirmed live first against two
disposable channels mapped to a real EPG row: a channel with a null
`channel_number` is exported as `<channel id="<its id>">` and one numbered 0 as
`<channel id="0">`, each with its programmes, exactly as the source reading said;
before the fix neither got a guide, after it both showed 5 of 5 broadcasts in a
real Kodi. `Channel::hasChannelNumber` separates null from a literal 0,
`FormatChannelGuideKey()`/`ChannelGuideKey()` produce the text the export uses
(the number's, including 0, negatives and values below 1, or the id for a
channel with no number), and every lookup and both collision checks use it.
Because numbers and ids share one `<channel id>` space, an unnumbered channel 77
and a channel numbered 77 are caught as ambiguous, as are two channels numbered
0. See `docs/EPG.md`.

#### Segment-body curl fetch in the live read path is unlogged

**`ReadLiveTimeshiftStream()`'s segment-body curl fetch is the one
unlogged blocking network call left in that read path, found
diagnosing a real tester bug report (2026-09-17).** A tester on
Windows, `live_timeshift_mode=Server-side`, had `start_buffer` succeed
(control-plane call, main Dispatcharr API port) but every actual
segment read then fail silently, surfacing to Kodi only as a generic
`CDVDDemuxFFmpeg::Open - error probing input format` with nothing in
the addon's own log pointing at why. Root cause: segment bytes are
fetched via a *separate* direct HTTP connection to the
`timeshift_buffer` plugin's own file server (`http_port`, default
`9192`, see `DispatcharrClient.cpp:2288-2295`) -- distinct from the
main API port the control-plane calls use -- and this tester's `9192`
wasn't port-forwarded/reachable from the Kodi machine (confirmed:
opening that port fixed playback). `docs/TIMESHIFT.md`/the plugin's
own README already document this as a real prerequisite, but nothing
in `ReadLiveTimeshiftStream()` itself logs the failure -- the
`res != CURLE_OK` branch at `DispatcharrClient.cpp:2721-2727` just
`return -1`s with no `kodi::Log` call, so a user/maintainer staring at
`kodi.log` alone has no way to tell "segment fetch failed" from any
other demuxer-probe failure.
`ReadInProgressRecordingStream()`'s own analogous segment-body fetch
(`DispatcharrClient.cpp:1888-1897`) already solved exactly this: an
unconditional `kodi::Log(ADDON_LOG_DEBUG, ...)` right after
`curl_easy_perform()`, logging `fetchSec`/`curlResult`/`httpCode`/
`url` regardless of outcome -- its own comment explains it was added
specifically because this was "the ONE unlogged blocking network call
in the whole read path." `ReadLiveTimeshiftStream()` needs the same
treatment: a log line (debug on success, at least error-level on
`res != CURLE_OK`) around its own `curl_easy_perform()` at
`DispatcharrClient.cpp:2717`, so the next time this happens
(reachability, firewall, wrong port) `kodi.log` says so directly
instead of requiring exactly this kind of multi-round diagnosis again.
Not yet implemented -- logging-only, low risk, but still a real
behavior-adjacent change (new log volume on a hot read path) that
should go through its own branch/PR per this file's own conventions
rather than landing ad hoc.

**Closed (2026-09-30): already implemented, the entry was stale.**
`ReadLiveTimeshiftStream()` logs the segment-body fetch (debug: time, curl result, HTTP
status, redacted URL) and, on a failed transfer, an *error*-level line regardless of
debug logging -- the treatment this entry asked for. Seen live on the way to another test:
with the addon pointed at an address whose plugin port wasn't reachable, `kodi.log` showed
`segment body fetch failed: Could not connect to server (url=...)` and then the bounded
"giving up after 4 consecutive segment fetch failure(s)" line, instead of only Kodi's
generic "error probing input format".

#### Live-timeshift address space never drops rolled-off segments

**The live-timeshift address space never drops a segment once the
plugin's own rolling buffer has rolled it off, flagged from an
8th-pass audit (2026-09-26), not reproduced live.**
`RefreshLiveManifest()`'s own merge is append-only by design (segments
are never evicted locally, matching how this addon's own fixed-origin
byte addressing works), but the *server-side* buffer does roll off
segments older than `buffer_minutes` (60 by default) -- this addon's
own local list has no matching mechanism, so an old segment stays
listed (and seekable from Kodi's own seek bar) long after the plugin
has actually deleted the underlying file. In a session longer than
`buffer_minutes`, rewinding toward the start of the seek bar lands on
a now-deleted segment; every read for it gets a 404 (already treated
as "recycled by -segment_wrap", i.e. `return 0`), and the read
position never advances, stalling playback until the user seeks
forward again. Also affects `GetStreamTimes()`, which always reports
`PTSBegin=0` regardless of how much of the buffer has actually rolled
off. A fix needs the oldest still-live server-side sequence number
(available from the same `get_live_manifest` response
`ParseNewLiveManifestSegments` already reads) to compute a real
oldest-available byte offset, then use it for both `PTSBegin` and to
clamp a backward seek -- not attempted this pass given the scope
(touches both the manifest-merge and seek-clamping code together).

**Update (2026-09-26, a 22nd-pass audit): the described "404, stalls"
failure mode is wrong -- the actual, more common outcome is a fatal
session teardown, confirmed against this addon's own code and
`timeshift_buffer/plugin.py`'s own segment-file-recycling scheme, not
reproduced live.** The plugin doesn't delete rolled-off segment files
at all -- `_build_ffmpeg_command()`'s own `-segment_wrap` (set to `2 x
visible_segments`, `_compute_segment_counts()`) makes ffmpeg
*overwrite* old segment filenames in place once the wrap count is
reached, not remove them. So a read for a segment between 1x and 2x
`buffer_minutes` behind live actually **succeeds** -- the old file
still exists with its original, correct bytes. Only past 2x
`buffer_minutes` behind live (e.g. pausing live TV for over 2 hours at
the 60-minute default) has that exact filename already been reused for
unrelated, newer content -- and at that point,
`ReadLiveTimeshiftStream()`'s own `Content-Range`-vs-cached-size
cross-check (added independently, see the "Recordings" doc's own
22nd-pass entry on the analogous recording-file case) sees a segment
whose real size disagrees with what the manifest reported, sets
`fatal = true`, and kills the *entire* live-timeshift session outright
-- not a stalled read at one position. A 404 is only a narrow race
window (a read landing in the brief gap between one process reusing a
filename and the write actually starting), not the steady-state
outcome. The suggested fix above still applies, but the "recycle"
check it needs must run *before* the fetch/cross-check, not just at
seek-clamp time, or a rolled-off segment keeps turning into a fatal
error instead of the intended graceful "recycled, treat as
unavailable" outcome.

**Fixed (2026-09-30), reproduced live first.** The 22nd-pass update's correction held:
with `buffer_minutes` set to 1 on the server, watching live, pausing 182 seconds and
resuming made the first read behind the window log "real size ... disagrees with the
manifest-reported size ... giving up on this stream", and playback ended. The addon now learns
the plugin's oldest listed sequence from every manifest (`OldestLiveManifestSequence()`) and
clamps reads, seeks and `PTSBegin` to the first segment still held
(`FirstAvailableLiveSegmentIndex()`); the same pause then logged "position ... is behind the
oldest segment the timeshift buffer still holds -- continuing from byte ..." and played on with
no fatal. The server setting was restored afterwards. See `docs/TIMESHIFT.md`.

#### User Stop plus status-before-playlist reorder can strand a recording's final segment

**A user-initiated Stop may combine with the 7th pass's own status-
before-playlist reorder to strand an in-progress recording's genuinely
final segment, flagged from an 8th-pass audit (2026-09-26), needs a
live check before changing anything further.** `docs/RECORDINGS.md`
already documents, confirmed against Dispatcharr's own `tasks.py`,
that its stop endpoint flips `custom_properties.status` away from
`"recording"` *synchronously*, before the HLS-to-MKV concat (and,
plausibly, before ffmpeg itself has fully stopped writing new
segments) actually finishes. If that's true of the underlying HLS
`index.m3u8` too -- i.e. a genuinely new segment can still appear in
it for a moment *after* `isInProgress` already reads false -- then
`RefreshInProgressRecordingManifest()`'s own status-first ordering
(7th pass) could still compute `finished=true` from a playlist
snapshot that's missing that final segment: every segment actually
present did probe successfully, so `allProbesSucceeded` is also true.
Unconfirmed either way: it's equally plausible Dispatcharr's stop
endpoint kills ffmpeg (and therefore the playlist stops changing)
in the same synchronous step as the status flip, in which case this
window doesn't actually exist. A suggested fix, not implemented
without confirming the premise first: treat `#EXT-X-ENDLIST` in the
playlist (which ffmpeg's HLS muxer writes on a clean shutdown) as a
more authoritative "genuinely done" signal than `isInProgress` alone,
falling back to today's isInProgress-based logic if Dispatcharr's own
DVR pipeline never actually emits that tag.
**Update (2026-09-26, an 18th-pass audit): the premise is confirmed,
and the suggested fix is viable -- still not implemented this pass.**
Confirmed against Dispatcharr's own real current upstream source
(cloned into a scratchpad, never committed to this repo -- a real
source read, not a live test): `RecordingViewSet.stop()` does write
`status = "stopped"` synchronously first, while the actual stream
teardown and Celery task revoke run later in a background thread --
so segments genuinely can still appear in the playlist for a moment
after `isInProgress` already reads false, exactly the window this
entry worried about. Separately, capture itself uses
`-hls_flags append_list+omit_endlist` (deliberately suppressing
`#EXT-X-ENDLIST` while still recording), but a dedicated
`_dvr_ensure_hls_endlist()` step always appends that tag before the
HLS-to-MKV remux runs, and the `hls` API action serves the playlist
file verbatim -- so this addon would reliably see the tag once it's
genuinely done, making the suggested fix workable as described. Not
implemented this pass: needs `M3u8SegmentParser` to also surface
whether `#EXT-X-ENDLIST` was present, and `RefreshInProgressRecordingManifest()`
to treat that as authoritative alongside (not instead of) the existing
`isInProgress`-based logic -- a real behavior change worth its own
pass with a test, not squeezed in at the end of this one.

**Fixed (2026-09-30), the premise confirmed live first.** With the playlist polled every
~30ms around a Stop: the status flips, then for about three seconds the playlist stays one
segment short with no `#EXT-X-ENDLIST`, then it gains the final segment and the tag together,
then the endpoint redirects. A viewer at the live edge reproduced the loss: `finished=1` at
its final byte count, a further segment probed 2.6s later. `M3u8HasEndList()` and
`GateFinishedOnEndList()` now hold `finished` until the tag arrives (15-second grace for a
recording that never writes it); re-run, the final segment was merged in the same cycle that
set `finished=1`. See `docs/RECORDINGS.md`.

#### Redundant API-key self-heal re-check on every in-progress manifest refresh

**`RefreshInProgressRecordingManifest()`'s own proactive API-key
self-heal (`IsApiKeyValidFor(playlistUrl)`) re-checks the same URL
`FetchRawInProgressPlaylist()` just fetched successfully moments
earlier, flagged from an 8th-pass audit (2026-09-26), performance
only.** `FetchRawInProgressPlaylist()` already handles its own 401;
reaching this check at all means the playlist fetch already succeeded
with the current key, so re-validating it is redundant -- one extra
GET of the same (small, ~4s-cadence-limited) playlist per refresh
cycle, roughly twice a second during active playback. Low priority,
not fixed this pass; a real fix would need `FetchRawInProgressPlaylist()`
to report whether it needed a mid-call key regeneration itself, rather
than this separate, redundant re-check afterward.
**Update (2026-09-26, a 39th-pass audit): the "roughly twice a second"
rate above understates the actual request rate significantly while a
viewer sits near the live edge of an in-progress recording.**
`ReadInProgressRecordingStream()`'s own catch-up-to-tail loop
(`DispatcharrClient.cpp`) calls `RefreshInProgressRecordingManifest(force=true)`
every `kCatchUpSleepMs` (250ms) while waiting for the next segment to
appear -- confirmed by code reading, not this specific rate measured
live -- so during that phase each cycle's own three HTTP requests
(`GetRecordingById()`, the full playlist GET, and this redundant
`IsApiKeyValidFor()` GET) run roughly 4 times a second, i.e. 10-12
requests/second sustained for most of every ~4s Dispatcharr HLS
segment-production gap, not twice a second. The playlist itself also
grows over the life of the recording (Dispatcharr's own DVR ffmpeg
command uses `-hls_list_size 0` with `append_list`, confirmed against
its real current upstream source, so nothing is ever pruned from it),
making each of those GETs progressively larger too. Doesn't change the
fix direction above, just the urgency/impact estimate.

**Fixed (2026-09-30).** The re-check is gone (and `IsApiKeyValidFor()` with it): the playlist
fetch right before it already sends the same key and regenerates it on a 401. Measured through a
counting proxy with a viewer at a recording's live edge: 130 playlist requests per 40 seconds
before, 65 after; recording lookups unchanged. The update's higher estimate was about right --
the rate is one cycle per ~0.65s, each of which used to cost two playlist GETs. See
`docs/RECORDINGS.md`.

#### _start_ffmpeg() can spawn inside a leftover channel directory

**`timeshift_buffer`'s `_start_ffmpeg()` can spawn a fresh ffmpeg
process inside a leftover, non-empty channel directory from a previous
buffer instance, flagged from an 11th-pass audit (2026-09-26), not
reproduced live.** The directory can exist with no matching Redis state
after a container restart (Redis doesn't persist across one while the
storage volume does), `_BUFFER_STATE_TTL` (600s) expiring, or a
`_teardown_buffer()` `rmtree` partially failing before its state delete
still ran. `_start_ffmpeg()` only ever `mkdir(exist_ok=True)`s, never
clears existing contents. Until the new ffmpeg's first segment closes
and it rewrites `live.m3u8`, `_get_live_manifest()` (a cold cache, new
`access_token`) would serve the stale leftover playlist with high
sequence numbers; the addon's cold-start loop accepts it and trims to
the last few stale segments; once the new ffmpeg writes its own
playlist restarting sequence numbers at 0,
`ParseNewLiveManifestSegments(..., lastKnownSequence)` filters all of
them out as "not new" -- the viewer plays a few stale seconds, then
stalls at the tail indefinitely with no `fatal` until the new sequence
numbers pass the old ones (tens of minutes at default settings).
Related: a reaper tick can `rmtree` the directory as a 300s-old orphan
out from under a buffer that's just starting (nothing updates the
directory's own mtime until the first segment closes), and if that
lands before `Popen`, the missing `cwd` surfaces as a misleading
"ffmpeg not found in this container" rather than the real cause. Fix
direction: clear the channel directory on every fresh-start path before
spawning, and/or persist a provisional state before spawning so the
reaper doesn't treat it as orphaned.

**Update (2026-10-01): fixed in `timeshift_buffer` 0.6.6 and unit-tested; awaiting a live check
on a real Dispatcharr.** `_clear_channel_dir()` removes the leftover directory (playlist, segments, log)
on the fresh-start path before ffmpeg spawns, which also gives the recreated directory a fresh mtime, so
neither the stale-playlist stall nor the reaper-rmtree/"ffmpeg not found" variant can occur. Tests include
`_start_ffmpeg()` end to end against a seeded stale directory (13 of the new 0.6.6 tests fail on 0.6.5). See
`docs/TIMESHIFT.md`.

**Confirmed live (2026-10-01), `timeshift_buffer` 0.6.5 -> 0.6.6, on a real Dispatcharr.** A made-up
channel's directory was seeded with a stale `live.m3u8` (`MEDIA-SEQUENCE:5000`), a segment and an old
`ffmpeg.log`, all backdated two hours, with no Redis state. On 0.6.5, right after `start_buffer` returned,
the stale playlist and segment were still there beside the new log. On 0.6.6 the directory held only a fresh,
empty `ffmpeg.log` and no playlist, and its mtime had moved from two hours ago to now -- which is also what
keeps the reaper's 300s orphan scrub off a buffer that is still starting. (An unseeded first attempt did not
show the mtime half, because creating the new `ffmpeg.log` itself touches the directory; a real leftover
already contains one, so the seeded run is the faithful one.)

#### Stale-state teardown can destroy a just-restarted buffer

**A stale-state teardown in `get_live_manifest`'s `BufferFailedError`
branch can destroy a buffer a concurrent `start_buffer` just
restarted, flagged from an 11th-pass audit (2026-09-26) -- a sharper,
more concrete consequence of the already-logged the "timeshift_buffer Redis viewer/heartbeat writes are unguarded read-modify-write" entry,
not a separate root cause.** `_get_live_manifest_action()`'s
`BufferFailedError` handler calls `_teardown_buffer(state)` using the
`state` read at the *top* of the call, before the potentially slow
`_get_live_manifest()` itself ran. If another viewer's `start_buffer`
classifies the buffer as "dead" and restarts it inside that window,
the stale teardown then writes the *old* state's `stopping=True` over
the new one, `rmtree`s the directory the new ffmpeg is now writing
into, and deletes the new state. Low probability (needs a specific
interleaving), but a much cheaper mitigation exists than the full
Redis-transaction fix the broader item calls for: have
`_teardown_buffer` re-read the current state first and only proceed if
identifying fields (e.g. `access_token`/`pid`) still match what it was
called with.

**Update (2026-10-01): fixed in `timeshift_buffer` 0.6.6 and unit-tested; awaiting a live check
on a real Dispatcharr.** `_teardown_buffer()` re-reads the state and does nothing if it now holds a
different instance (`pid`/`started_at`), as the entry suggested, so every caller is covered, not just
`get_live_manifest`. The window is narrowed, not closed -- a Redis transaction would close it. See
`docs/TIMESHIFT.md`.

**Confirmed live (2026-10-01), `timeshift_buffer` 0.6.5 -> 0.6.6, against the real Redis.** A made-up
channel's state was replaced by a newer instance (different `pid`/`started_at`) and then a teardown decided
from the old copy was run. On 0.6.5 it returned `None` and the replacement's state and directory were both
destroyed. On 0.6.6 it returned `False`, logged "not tearing down ... its buffer was replaced since this
teardown was decided", and left both intact. Still a narrowing of a read-then-write window rather than a
transaction.

#### timeshift_buffer HTTP file server binds IPv4 only

**`timeshift_buffer`'s own HTTP file server binds IPv4 only
(`0.0.0.0`), flagged from a 20th-pass audit (2026-09-26), low
severity.** This addon explicitly supports IPv6-literal hosts
elsewhere for timeshift URLs (`FormatHostForUrl()`,
`BuildTimeshiftPlaylistUrl()`) -- a non-Docker Dispatcharr install
(upstream ships a `debian_install.sh` path alongside its Docker one)
configured with an IPv6-only or IPv6-preferred host couldn't reach
this plugin's own port. Docker's own port-proxy layer covers the far
more common deployment, so this is low-impact -- not pursued this
pass.

**Update (2026-10-01): fixed in `timeshift_buffer` 0.6.6 and unit-tested (including a real loopback
IPv4 + IPv6 connection); awaiting a live check inside the real container.** A dual-stack `::` socket with
`IPV6_V6ONLY` off, falling back to IPv4 only where the host has no IPv6. See `docs/TIMESHIFT.md`.

**Confirmed live (2026-10-01), `timeshift_buffer` 0.6.5 -> 0.6.6, inside the real container.** On 0.6.5 the
port had seven listeners, all in `/proc/net/tcp` (IPv4), and a request to `http://[::1]:9192/` was refused
(`000`). On 0.6.6, after a restart, none are in `/proc/net/tcp` and two are in `/proc/net/tcp6` (dual-stack);
the plugin server answers its own 404 over both `127.0.0.1` and `[::1]`. Fewer listeners than before is
expected: each worker binds on its first plugin action, and the old count included leaks from an API
import's reload. The IPv4-only fallback is covered by unit tests only, since this container has IPv6.

#### recording_edl staging-directory scrub ignores non-default recordings roots

**Suspected: `recording_edl`'s HLS-staging-directory scrub tools never
look outside the default recordings root, flagged from a 21st-pass
audit (2026-09-26), confirmed against this addon's own two scan
implementations disagreeing, not reproduced live.**
`_list_dvr_hls_staging_dirs()`/`_delete_orphaned_dvr_hls_dirs()` only
`rglob` the default recordings root, while `_dvr_sidecar_scan_roots()`
(used elsewhere in the same file) explicitly also covers
absolute-path recording-path templates outside it. A library
configured with such a template would have any orphaned `.dvr_<id>_hls`
staging dir there silently never listed or cleaned by these two
functions -- a false negative (nothing gets deleted that shouldn't),
not a data-loss risk, just an incomplete cleanup. Fix direction: reuse
the already-tested `_resolve_scan_root`/`_dedupe_scan_roots` helpers
the sidecar scan already relies on -- but doing so adds a Django-model
dependency (reading the configured templates) to what's currently a
read-only, dependency-free scan, so this needs its own separate review
before changing, not a blind extension.

**Update (2026-10-01): fixed in `recording_edl` 0.2.1 and unit-tested; awaiting a live check on a
real Dispatcharr.** Confirmed against Dispatcharr's own source that the staging directory is created beside
the recording's final file, so an absolute template puts it outside `/data/recordings`.
`_dvr_hls_staging_scan_roots()` adds the template roots to the default one, de-duplicated, falling back to the
default root alone if the templates can't be read -- the Django dependency the entry worried about is handled
by that fallback. The deletion rule is unchanged. See `docs/RECORDING_EDL.md`.

**Confirmed live (2026-10-01), `recording_edl` 0.2.0 -> 0.2.1, against the real Django models.** With the
template lookup pointed at an external library holding a `.dvr_<id>_hls` directory for an id no recording
has, 0.2.0's `_list_dvr_hls_staging_dirs()` printed `NOT FOUND` while 0.2.1 listed it as `orphaned`. The
template lookup itself was substituted for the test, so a library configured through a real absolute path
template is covered by the unit tests rather than this run.

#### X-API-Key could follow a cross-host or downgrading redirect

**`X-API-Key` could follow a cross-host or protocol-downgrading
redirect, flagged from a 57th-pass audit (2026-09-27), a design
decision, not fixed this pass.** `OpenRecordingStream()` sends
`X-API-Key` as a plain custom header (`AppendApiKeyHeaderIfPresent()`)
alongside `CURLOPT_FOLLOWLOCATION(1L)`. libcurl only strips
`Authorization`/`Cookie` on a cross-host redirect by default -- a
custom header like `X-API-Key` is not covered by that protection, so
it would still be sent to whatever host/scheme a redirect response
named, including an http downgrade from https. Dispatcharr's own
in-progress-recording redirect (the exact behavior
`CURLOPT_FOLLOWLOCATION` exists here for) is same-host, so this needs
a misconfigured or malicious redirect from the user's own configured
server or a proxy in front of it to actually leak the key -- not
attacker-controlled input, but not nothing either, since a
misconfigured reverse proxy is a real, if narrow, real-world scenario.
Not fixed blind: `CURLOPT_REDIR_PROTOCOLS`/an explicit redirect-target
check would need to not also break the legitimate same-host redirect
this code already depends on, so this needs a deliberate design
decision (which protocols/hosts to actually allow) rather than a
reflexive lockdown.
**Update (2026-09-27, a 58th-pass audit): `CURLOPT_FOLLOWLOCATION` has
three more call sites, confirmed by grepping every use in the current
source, not itself independently reproduced.** `Request()` (the
general-purpose HTTP client -- carries `Login()`/`RefreshAccessToken()`'s
own credentials/refresh token in the POST body, which curl re-sends on
a 307/308 redirect regardless of host/scheme, with no libcurl
protection covering a request body at all, only specific headers) and
`SendTimeshiftHeartbeat()` (a Bearer `Authorization` header -- covered
by libcurl's own header-stripping *only* since curl 7.58, and this
project's own Windows release currently bundles curl 7.67.0, so this
one specific case is already covered there, but not necessarily on
every platform/libcurl this addon could be built against).
`GetXmlTvGuide()` sends no credentials of any kind and is harmless.
Same reasoning and same "needs a live redirect scenario, not
attacker-controlled input" caveat as `OpenRecordingStream()`'s already-
logged case above -- widening this entry's own scope rather than
fixing anything, since the same design decision (which protocols/hosts
to actually allow without breaking each call site's own legitimate
same-host redirect use) applies uniformly across all four.

**Fixed (2026-09-30) for all four call sites that carry credentials, reproduced live first.**
`Request()`, `SendTimeshiftHeartbeat()` and `OpenRecordingStream()` now follow redirects themselves,
vetting each hop with `IsSafeRedirectTarget()` (same host, never https to http, no user info, five hops
at most); a different port or an http-to-https upgrade on the same host is still followed, so the legitimate
same-host redirect this code depends on is unaffected -- the design decision the entry asked for. `GetXmlTvGuide()`
sends no credentials and is left alone. With a proxy answering the login POST with a 307 to a different host,
the old build re-sent the password to it; the new one refused and logged the two hosts. The `X-API-Key` paths use the
same helper and are covered by the policy's unit tests rather than a separate live run. See `docs/API_NOTES.md`.

#### WriteCallback has no upper bound on the response body

**`WriteCallback` (`CurlCallbacks.cpp`) has no upper bound on the
response body it accumulates, flagged from a 54th-pass audit
(2026-09-27), a design decision, not fixed this pass.** The 53rd/54th-pass
`ProbeSegmentByteSize()` fix only bounded the `reserve()` *hint* built
from a probed `Content-Length` -- the actual body bytes for every
`WriteCallback` user still accumulate via a plain, unbounded
`out->append(ptr, size * nmemb)` inside a libcurl C callback. A
genuinely huge response body (the same untrusted-server-misbehavior
premise the `reserve()` fix already accepts) still ends in
`std::bad_alloc`, just from real accumulated bytes rather than a lying
size hint -- and since this throws from inside a plain C callback
invoked by libcurl, it would very likely unwind through C stack frames
with no destructors run cleanly, probably reaching `std::terminate()`
rather than a clean C++ exception path.
**Update (2026-09-27, a 56th-pass audit): this entry's own original
call-site list is now stale, confirmed against the current source, not
itself independently reproduced.** A 55th-pass audit moved the
in-progress-recording segment fetch off `WriteCallback` entirely onto
a `FixedBufferSink` bounded to `segByteSize` -- that call site is now
fixed, not merely bounded on the `reserve()` hint side, and no longer
belongs on this entry's own list of remaining `WriteCallback` users.
**Update (2026-09-27, a 57th-pass audit): the 56th-pass update's own
"remaining callers" list was itself incomplete, confirmed by grepping
every `CURLOPT_WRITEFUNCTION, WriteCallback` call site in the current
source, not itself independently reproduced.** The full remaining list
is `Request()` (JSON responses), `GetXmlTvGuide()` (the XMLTV guide
fetch -- these two were already named), plus three missed the same
pass: `SendTimeshiftHeartbeat()`, `IsApiKeyValidFor()` (a `Range: 0-0`
GET of the ever-growing HLS playlist -- see this file's own separate
entry on that same call re-downloading the whole thing every refresh),
and `FetchRawInProgressPlaylist()`. A sixth caller,
`OpenRecordingStream()`'s own `Range: 0-0` probe of `/file/`, was
fixed outright the same, 57th, pass instead of merely added to this
list: unlike the others, it never legitimately wants more than 1 byte
(the whole point of "0-0"), so bounding it to a 1-byte
`FixedBufferSink` was a strictly safe fix with no real-large-payload
tradeoff to weigh, closing the one caller on this list where a
Range-stripping proxy in front of this addon's own configured
Dispatcharr server could otherwise buffer an entire multi-GB recording
file into memory.
**Update (2026-09-27, a 58th-pass audit): two more of the "remaining
five" callers were fixed the same way, confirmed by re-reading each
one's own actual body-use, not itself independently reproduced.**
`IsApiKeyValidFor()` is the exact same "0-0" probe pattern as
`OpenRecordingStream()`'s already-fixed case (only `httpCode` is ever
read) -- and, unlike that one, this target endpoint is *confirmed*
(not just hypothetically, via this file's own separate entry on it) to
ignore `Range` and always serve the full, ever-growing HLS playlist
body regardless, so this fix applies to a real, already-observed
behavior, not only a misbehaving-proxy scenario. `SendTimeshiftHeartbeat()`'s
own response body was never read at all (only the transfer's own
`CURLcode` result is checked) -- nothing to weigh there either. Both
now use a 1-byte `FixedBufferSink`, the same non-aborting sink
(`FixedBufferWriteCallback` always reports the full byte count
consumed, regardless of its own capacity) that keeps this safe for
`IsApiKeyValidFor()` specifically: an *aborting* callback would have
made a 401 whose body got cut off read as `httpCode != 401` --
wrongly "valid" -- so non-aborting truncation, not a hard stop, is
what this fix actually needs. Only `Request()`, `GetXmlTvGuide()`, and
`FetchRawInProgressPlaylist()` remain open now, each for the reason
already given below (a genuinely large, legitimate real payload this
addon's own code actually reads afterward). Deliberately not fixed
this pass: unlike a single HLS segment or a 1-byte probe,
`WriteCallback` is still shared by these three call sites with
genuinely large, legitimate real payloads (a big XMLTV EPG export can
easily be tens of MB on its own; the HLS playlist itself grows
unbounded over a long recording), so picking one safe ceiling for
every remaining caller risks a real regression (truncating a
legitimate large response) in a way the four now-fixed cases didn't
have to weigh. Needs either a per-call-site ceiling (threaded through
as a parameter) or a live check of how large a real XMLTV export/
playlist actually gets before choosing one ceiling that's safe
everywhere.

**Fixed (2026-09-30) with per-call-site ceilings chosen from live sizes, the second option the entry gave.**
`BoundedWriteCallback` replaces `WriteCallback` at the three remaining callers: API JSON 128 MiB (largest real
response: the channel list, far below it), XMLTV 512 MiB (measured well below it), in-progress playlist 64 MiB. Past the limit the
transfer fails with an error naming it. Confirmed live with a proxy streaming an endless channel-list body: cut off
at 136 MB with "HTTP response exceeded the 128 MiB limit", Kodi unaffected. See `docs/API_NOTES.md`.

#### Request()'s 401 retry bypasses AuthBackoff

**Known gap: `DispatcharrClient::Request()`'s own 401-triggered retry
bypasses `AuthBackoff`, flagged from a project-wide review
(2026-09-26), not yet reproduced.** `Request()` calls
`RefreshAccessToken()`/`Login()` directly on a 401 response, not
through `EnsureAuthenticated()` -- so it never touches
`m_consecutiveLoginFailures` or `ComputeLoginBackoffSeconds()`. If
Dispatcharr revokes a token server-side (e.g. a password change) while
this addon's own local `m_accessTokenExpiry` hint still looks valid
(up to its ~4-minute window), `EnsureAuthenticated()` keeps reporting
success, so every call in that window independently hits 401 then
retries `Login()` with no backoff at all -- the same rate-limiter risk
`AuthBackoff` was added to close (see its own `CLAUDE.md` entry), just
time-boxed to that window instead of indefinite. Not a pure-logic
extraction candidate on its own; worth revisiting alongside
`AuthBackoff` if this ever needs closing.

**Fixed (2026-09-30), reproduced live first.** A 401 now invalidates the freshness hint (only if the token is still
the rejected one) and goes through `EnsureAuthenticated()`, so the backoff and transient cooldown apply. With a
proxy answering every API call 401 after login: 11 login POSTs and 11 refresh POSTs in 170 seconds for 10 requests
before, 3 logins, 1 refresh and 1 request after. See `docs/API_NOTES.md`.

#### WebSocket Sec-WebSocket-Accept is never validated

**Known gap: a WebSocket server's `Sec-WebSocket-Accept` response
header is never validated, flagged from a project-wide review
(2026-09-26).** RFC 6455 requires a compliant client to check this
against the SHA-1/base64 hash of its own `Sec-WebSocket-Key` -- doing
that needs a SHA-1 implementation this project doesn't currently pull
in, so `IsWebSocketHandshakeAccepted()` only checks for the HTTP `101`
status line today. Low real-world risk (Dispatcharr's own server isn't
going to send back a bogus accept value), so not urgent, but worth
tracking as a known spec-compliance gap rather than an oversight.

**Fixed (2026-09-30).** A dependency-free SHA-1 (`Sha1.h`, tested against the FIPS vectors and a reference across the
padding boundaries) and `IsWebSocketHandshakeAcceptedForKey()`, an exact comparison. Confirmed live: a proxy rewriting the
accept value made the old build report "connected"; the new build refuses, with a message that says the answer was
wrong, and still connects to the real server. See `docs/API_NOTES.md`.

#### Plugin reload leaks timeshift_buffer's listener and reaper per worker

**A Dispatcharr plugin reload (triggered by installing, enabling,
disabling, or deleting *any* plugin -- not just `timeshift_buffer`
itself) leaks `timeshift_buffer`'s own HTTP listener and reaper thread
per Dispatcharr worker process, flagged from a 31st-pass audit
(2026-09-26), confirmed against Dispatcharr's own real current
upstream source, not itself independently reproduced -- needs a live
check against a real plugin reload before implementing a fix, not
attempted blind given how much already-battle-tested reaper/HTTP-server
lifecycle logic this would touch.** Dispatcharr's own
`PluginManager.discover_plugins(force_reload=True)`
(`apps/plugins/loader.py`) unconditionally pops every enabled plugin's
modules from `sys.modules` and re-imports them fresh, for *every*
enabled plugin, on any plugin install/enable/disable/delete or hub
install (`apps/plugins/api_views.py`) -- it never calls the old
module's own `stop()` first. Only `PluginReloadAPIView` and this
specific plugin's own disable path call `stop()`, and only in the one
worker that happens to handle that particular request; every *other*
uWSGI worker picks up the reload on its own next `discover_plugins()`
call with no `stop()` at all. Since `_http_server`/`_http_server_thread`/
`_reaper_thread`/`_latest_settings_dict` are plain module-level
globals, a fresh re-import gives the module brand-new copies of all
of them (`None`/`{}`) with no reference back to whatever was already
running -- `_ensure_http_server_running()`/`_ensure_reaper_running()`
then start a *second* listener (silently succeeding via `SO_REUSEPORT`,
which this plugin's own `_BufferHTTPServer.server_bind()` already
sets) and a *second* reaper per affected worker, both real, running,
resource-holding objects, not just leaked references -- the old ones
are never told to stop. The old reaper keeps reading the *old*
module's own now-frozen `_latest_settings_dict`, exactly reintroducing
the stale-settings bug `_ensure_reaper_running()`'s own comment already
documents as fixed (a later `storage_path`/`idle_timeout_seconds`
change in Dispatcharr's Plugin Settings UI stops reaching it, whenever
the old reaper happens to hold Redis leadership); the old listener
keeps serving segment requests from its own stale `storage_path`,
so a later `storage_path` change can make a random share of segment
requests 404, split unpredictably between the old and new listener by
the kernel's own `SO_REUSEPORT` load-balancing. Disabling or deleting
the plugin later only stops the newest listener/reaper in whichever
one worker handles that request -- every older, orphaned pair in every
other worker keeps running (old listeners still answering on the
port, old reapers still running `_scrub_orphaned_dirs`) until
Dispatcharr itself restarts. `dispatcharr-plugin/timeshift_buffer/README.md`'s
own "if a code change doesn't take effect" troubleshooting note
describes the visible symptom but not this mechanism, and its advice
that the reload endpoint fixes this is only true for the one worker
that handles that specific request. A real fix needs the server/reaper/
stop-event state to live somewhere that survives a module reload --
e.g. discovered via `threading.enumerate()` by the already-descriptive
thread names this plugin already gives them (`timeshift_buffer_http`/
`timeshift_buffer_reaper`), with any state the reaper needs to keep
reading live (like `_latest_settings_dict`) moved onto the persistent
thread object itself rather than a plain module global -- a real,
live-verifiable change to lifecycle code with a lot of its own
hard-won history (see this same file's surrounding comments), not
attempted blind this pass.
- **Context, not itself a bug:** Dispatcharr runs this plugin inside
  gevent-monkey-patched uWSGI workers (`docker/uwsgi.ini`'s
  `gevent = 400`, `dispatcharr/gevent_patch.py`'s `monkey.patch_all()`),
  confirmed against Dispatcharr's own source -- this plugin's own
  "threads" are actually greenlets. Correctness looks unaffected (the
  blocking calls this plugin makes -- `time.sleep`/`select`/socket
  I/O -- all yield under gevent's patching), but a long blocking
  filesystem operation inside a request greenlet (e.g. `shutil.rmtree`
  of a large buffer in `_teardown_buffer()`, especially over network
  storage) would stall every other greenlet on that same worker,
  including Dispatcharr's own live-stream proxy traffic -- not
  measured, low confidence, worth a documentation note rather than a
  code change.

**Update (2026-09-27, a 41st-pass audit): a distinct, separately-fixed
bug in the same `stop()` function, not the leak described above.**
`stop()` used to tear down every live buffer unconditionally on
*any* call, including a plain `reason="reload"` triggered by an
unrelated plugin's own install/enable/disable/delete -- ending
playback server-wide for every viewer with server-side timeshift
active, with no actual intent to disable or delete this plugin at
all. Fixed by gating the teardown on `reason != "reload"`; see
`CLAUDE.md`'s own `timeshift_buffer` testing entry for `Plugin.stop()`
for the full account. This fix does not touch the leaked-listener/
reaper problem described above at all -- that's still open exactly as
described, a separate mechanism (module-level globals losing their
reference on re-import) than this one (a single function's own
unconditional teardown call).

**Update (2026-09-27, a 62nd-pass audit): the same "different workers
serving different `storage_path`s, split unpredictably by
`SO_REUSEPORT`" symptom this entry already describes for a plugin
reload also has a simpler, plugin-reload-free trigger, confirmed
against Dispatcharr's own real current upstream source, not itself
independently reproduced.** `PluginSettingsAPIView.post()` ->
`PluginManager.update_settings()` (`apps/plugins/loader.py`) only ever
saves the new settings to the DB row and returns -- confirmed it never
calls `run_action()` or touches any already-running worker's own
module state at all, in any worker, proactively. Each worker only
actually picks up a changed `storage_path`/`http_port` the next time
*that specific worker* happens to handle any `run()` call for this
plugin (`_ensure_http_server_running()`'s own already-existing comment,
"called unconditionally at the top of every run(), on every worker",
already says as much) -- so immediately after a plain settings save,
with no reload/re-import involved at all, some workers have already
restarted their own listener on the new path while others haven't yet
handled any `run()` call since the change. The kernel's own
`SO_REUSEPORT` load-balancing then splits incoming segment requests
across both groups the same way it already does in the reload-leak
scenario above -- just without a second, permanently-orphaned listener
left behind once every worker eventually catches up on its own next
`run()` call, so this specific trigger is self-healing within
whatever cadence real traffic (heartbeats, manifest refreshes) drives
each worker's own `run()` calls at, unlike the reload-leak case. Same
fix direction already given above (state that survives per-worker
rather than being re-derived lazily per `run()` call) would also close
this simpler case; not attempted blind this pass either, for the same
reason.

**Update (2026-09-28, live testing, later same day): retracted --
the apparent reload-kills-buffer regression reported earlier today
(below, struck through in spirit if not in markdown) did not
reproduce after a full Dispatcharr restart.** The original finding
(`start_buffer` -> immediate `list_buffers` confirming it present ->
`POST /api/plugins/plugins/reload/` -> buffer gone, 1.45s total
elapsed, with a same-timing no-reload control reliably finding it
present at 0.63s) was real *as observed*, but the session's leading
hypothesis for *why* -- a freshly-started reaper thread's first tick
running immediately, with no initial delay, before
`stop_event.wait(15)` is ever reached -- doesn't hold up: that first
tick's own idle-timeout check (`now - state.get("last_heartbeat", 0)
> idle_timeout`) compares against a `last_heartbeat` that
`_start_buffer()` already sets to the real creation time, not a
missing/zero sentinel, so a genuinely fresh buffer reads as 0s idle
to that tick, not infinitely idle. After the user restarted the
Dispatcharr instance fully (clearing every worker's in-memory
module state, not just this one plugin's), the exact same sequence
-- `start_buffer`, `list_buffers` (present), `reload`, `list_buffers`
immediately after -- was repeated 4 times in a row (once at first
retest, then 3 more back-to-back rounds), and the buffer survived
every single time, with `age_seconds` correctly incrementing each
round rather than resetting. The most likely explanation: this
session had already driven many dozens of manual reload/test cycles
against this same long-running Dispatcharr instance earlier the same
day (including testing older, pre-41st-pass plugin code before the
fix was deployed) -- exactly the conditions the leaked-listener/
reaper entry just above already documents as leaving orphaned,
never-stopped reaper threads and stale Redis leader-lock tokens
behind in whichever workers didn't happen to handle the specific
reload request each time. A stray orphaned reaper thread from one of
those earlier, pre-restart cycles -- still holding (or intermittently
re-acquiring) Redis leadership, and still ticking on its own stale
15s cadence independent of anything this session was actively doing
-- reaping the test buffer on an unlucky tick is a far more
consistent explanation than the fresh-thread-first-tick theory above,
which doesn't actually survive reading `_start_buffer()`'s own
`last_heartbeat` initialization. Leaving the 41st-pass fix's own
status as genuinely fixed and confirmed live now, on a clean
restart -- this update itself is the retraction, not a new
reopening. The separate leaked-listener/reaper-pair entry above is
unaffected by this correction and remains open on its own merits.

**Attempted (2026-09-30): not observable from here.** The leak is in thread and listener counts inside Dispatcharr's uWSGI workers; the only handle this workspace has on the instance is its HTTP API, which exposes none of that, and the suggested indirect test (change `storage_path` and watch for intermittent segment 404s) would touch the real plugin's settings. Needs a shell on the Dispatcharr host (`ps`/`ss` before and after a plugin reload).

**Update (2026-10-01): fixed in `timeshift_buffer` 0.6.7 and unit-tested, including two real imports of
the plugin file with the second stopping the first's real listener; awaiting a live check on a real Dispatcharr.**
The listener and reaper threads now carry handles to themselves (`tsb_server`, `tsb_stop_event`) and each import
of the module, on its first `run()`, finds the threads an earlier import left (by their names, in
`threading.enumerate()`) and stops them. Threads from 0.6.6 and earlier have no handles and can only be reported,
once; a Dispatcharr restart clears them. Observable evidence already seen on the real instance: listener
counts on the plugin port grew to seven IPv4 sockets after repeated reloads and an API import.

**Fixed (2026-10-01) in `timeshift_buffer` 0.6.7, reproduced and confirmed live on a real Dispatcharr.**
A reload through the API only re-imports the plugin in the worker that handled the request; every
other worker re-imports it the next time it serves `GET /api/plugins/plugins/` (it notices the shared
reload token), and Dispatcharr calls no `stop()` on the copy it drops there. So reproducing the leak needs that
GET *and* a `run/` call to start the new copy's listener -- `run/` calls alone, which the first attempt
used, left the count flat and proved nothing. With both, on 0.6.6 the listener count on the plugin port
went 7 -> 10 -> 13 -> 16 across three reloads (one more per worker that didn't handle the request, four
workers). On 0.6.7, after a container restart, the identical loop held at 4 -> 4 -> 4 -> 4. See `docs/TIMESHIFT.md`.

#### timeshift_buffer Redis viewer/heartbeat writes are unguarded read-modify-write

**`timeshift_buffer`'s Redis viewer/heartbeat state writes are
unguarded read-modify-write, so a concurrent heartbeat can overwrite a
viewer registration or the "stopping" marker, flagged from a
project-wide review (2026-09-26), architectural -- a real fix needs
Redis WATCH/MULTI or splitting heartbeats into their own key(s), not a
quick patch.** Every writer (`_touch_heartbeat`, `_heartbeat`,
`_start_buffer`'s reattach, `_stop_buffer`'s keep branch, the reaper's
prune write-back, `_teardown_buffer`'s own `stopping` write) does a
plain GET-then-SET with no transaction. `_apply_heartbeat`'s own
docstring already documents that re-reading state immediately before
each write-back *narrows* this window (a real, previously-fixed race)
but doesn't eliminate it: `_touch_heartbeat` fires on every segment/
playlist HTTP fetch (several times a second per viewer) with no
re-read immediately before its own write, so it can still land between
another request's read and write. Two concrete consequences: (a) the
original "Concurrent viewers" bug (`docs/TIMESHIFT.md`) recurring
through this narrower window instead of the one already fixed, and (b)
a heartbeat overwriting `_teardown_buffer`'s `state["stopping"] = True`
marker (added for the 5th-pass `_classify_existing_buffer` fix) with a
stale copy that never had it set, letting a concurrent `start_buffer`
reattach to a buffer that's still mid-teardown -- reopening the exact
race that fix closed. Not reproduced live (needs a specific
multi-request interleaving); found via code reading. A proper fix
needs either Redis transactions around every read-modify-write, or
moving per-viewer heartbeats into their own Redis key/hash field so a
heartbeat write physically can't touch `viewers`/`stopping` at all --
both bigger changes than this pass's other fixes, deliberately not
attempted blind.

**Update (2026-09-26, a 24th-pass audit): a third consequence (c),
same root cause.** `_touch_heartbeat` can read a buffer's state just
before `_teardown_buffer()`/`_delete_buffer_state()` removes it, then
write that stale read back afterward -- resurrecting a deleted state
entry with a full TTL, pointing at a dead pid and an already-removed
directory. Self-heals on the next "dead" classification
(`_classify_existing_buffer()`) or reaper tick, but can briefly count
against `max_concurrent_buffers` in the meantime. Not reproduced live,
same fix as (a)/(b) above.

**Update (2026-10-01): fixed in `timeshift_buffer` 0.6.7 and unit-tested against a fake Redis that loses
races on demand; awaiting a live check on a real Dispatcharr.** Every writer now goes through
`_update_buffer_state()`: read, mutate, and write back only if the stored text is still exactly what was read, as
one Lua compare-and-set, retrying against the new state on a lost race. That closes all three consequences: a
heartbeat overwriting a viewer registration, a heartbeat overwriting the `stopping` marker (so `start_buffer` could
reattach to a buffer mid-teardown -- the attach now also refuses a stopping buffer), and a late heartbeat resurrecting
a deleted state (an absent state is never written). The teardown takes an `abort_if` predicate checked atomically with
its marker, so `stop_buffer` and the reaper no longer tear down a buffer that gained a viewer or heartbeat in the
meantime. If a Redis refuses scripts it falls back to the old unguarded write, once-logged. See `docs/TIMESHIFT.md`.

**Fixed (2026-10-01) in `timeshift_buffer` 0.6.7, confirmed live against the real Redis.** Three races
reproduced by injecting another writer's change between a heartbeat's read and its write, through the plugin's own
`_heartbeat()`: a viewer registered in between, a `stopping` marker set in between, and the state deleted in
between. On 0.6.6 all three lost (the viewer and marker overwritten, the deleted state recreated); on 0.6.7 all three
held, and `EVAL` is available on that Redis. The compare-and-set itself and its retry/fallback paths are covered
by unit tests against a fake Redis that loses races on demand. See `docs/TIMESHIFT.md`.

#### Forward seek near the live edge can clamp backward or be absorbed silently

**Low confidence, needs a live check before acting: a forward seek
near the live/tail edge can clamp backward past where playback
already naturally is, flagged from a 37th-pass audit (2026-09-26).**
`SeekLiveTimeshiftStream()`/`SeekInProgressRecordingStream()`
(`DispatcharrClient.cpp`) both clamp any forward-seek target exceeding
`tailTarget` (the deliberately-backed-off live edge --
`ComputeLiveEdgeTailTarget()`, `LiveEdgeMargin.h`) down to
`tailTarget` itself, via a plain `newPos > tailTarget` check -- neither
compares against the stream's own *current* position first. A viewer
who has played naturally into the last few segments (well within the
backoff margin: 3 segments for live timeshift, 1 for an in-progress
recording) already sits past `tailTarget` without ever having issued a
seek at all. Any small forward seek from there (e.g. a 30s-skip) then
computes a `newPos` that's still past `tailTarget`, gets clamped down
to it, and lands *behind* the position the viewer was already
watching from -- a visible jump backward instead of the requested
forward skip. Not reproduced live, and deliberately not fixed blind:
a naive `max(currentPosition, tailTarget)` clamp risks reintroducing
the exact problem the backoff margin exists to prevent in the first
place (the documented live-edge H.264 decode-error/audio-desync storm,
`docs/TIMESHIFT.md`'s "Packet corrupt" section) whenever the current
position has *itself* already drifted past the backed-off tail
through ordinary playback, not a seek -- landing there regardless of
what the seek clamps to. Needs a live check first: does this actually
happen in practice (is normal playback ever really left sitting past
`tailTarget`, given the backoff margin is deliberately generous), and
if so, what's the right behavior (refuse the seek / clamp to current
position and accept the corruption risk / something else) -- not
obvious without seeing a real instance of the symptom.
**Live check (2026-09-29): the specific "lands behind where you
already were" symptom was not reproduced, but a related, milder
symptom was.** Tuned into a live channel with an already-running,
still-growing server-side buffer (reattached partway through, ~30
minutes of existing content) and tracked `Player.GetProperties`'
`time` against real elapsed wall-clock time over several checks: under
ordinary 1x real-time playback, `time` advanced in lockstep with real
time (matching the expected "OpenLiveTimeshiftStream() starts you a
fixed backoff margin behind the tail, and both position and the true
tail advance at the same rate under steady playback, so the gap
neither closes nor widens on its own" reasoning). A large forward-seek
probe (+36000s, meant to force the `newPos > tailTarget` clamp and
land exactly at `tailTarget`) produced no visible jump at all --
position simply continued advancing at 1x from wherever it already
was, consistent with current position already sitting at essentially
`tailTarget` itself at that moment, not behind it with room to spare.
A subsequent ordinary, realistic forward-seek (+30s, the kind of step
a "skip forward" button press sends) issued from that same
already-at-the-margin position also produced no visible jump -- `time`
only advanced by the ~1 real second the request round-trip itself
took, i.e. the seek was silently clamped down to essentially where
playback already was and became a no-op, not a forward skip and not a
backward jump either. This suggests the specific reported symptom
needs current position to have already drifted *strictly past*
`tailTarget` before any seek is even issued -- not reachable via
ordinary steady playback or via seeking itself (the clamp's whole job
is preventing exactly that), so it likely needs an accelerated
catch-up read (Kodi/ffmpeg consuming backlog faster than real-time
after a stall) actually outrunning a `tailTarget` recomputed from a
discrete, bursty segment-arrival pattern -- not reproduced here, and
not straightforward to force deliberately via JSON-RPC alone. Found
along the way, worth its own note even though it isn't the symptom
this entry is about: a forward-seek attempt issued while already
sitting at the live-edge margin is silently absorbed as a no-op with
no feedback to the viewer, which could itself read as "skip forward
isn't working" -- a real, if minor, UX rough edge distinct from this
entry's own backward-jump concern.

**Reviewed (2026-09-30): no new evidence.** Re-read against the code and the 2026-09-29 live check above; nothing new can be reproduced through JSON-RPC alone, and the silently-absorbed seek is the one real finding. Left as is.

**Live check (2026-10-01): the backward clamp is real but small, and the silently absorbed seek is the same mechanism.**
Played a young, still-recording five-minute recording and sat at its tail (the reader waiting for the next segment, which is
past `tailTarget` with no seek at all), then issued Kodi's own forward steps while the addon's seek log recorded the byte
positions. The clamp did land behind the reader's position: `current=P -> newPos=P-M` (1,310,720 bytes
back), `176,881,072 -> 176,356,784` (524,288 back), and four more at exactly 262,144 back -- every one under a quarter of a
segment (segments here ran a few MB), which is the ceiling by construction, since `tailTarget` is one segment behind the
end and the reader cannot be further ahead of it than the end. Kodi's displayed time never went backward after any of them.
The "absorbed" step is the same clamp seen from the other side: a +30 s step with less than that left makes Kodi request a
position at the very end (`position=144,947,624` of 144,948,564), which clamps to `tailTarget` a quarter-megabyte behind
the reader, and Kodi's own follow-up probing of the end then settles where playback already was (`time` 189.6 before and
after). So the earlier suspicion that this needs an accelerated catch-up to outrun `tailTarget` was wrong -- merely waiting at
the tail does it. Live timeshift (margin 3 segments) was not exercised and could move back up to three. Verdict: minor,
and the fix is small and safe -- on a forward seek that clamps, never land behind the current position
(`newPos = max(tailTarget, min(current, requested))`): the reader is already past `tailTarget` in exactly that case, so it adds
no exposure to the live-edge decode errors the margin exists for. The no-feedback part is how a live edge behaves anywhere
and stays as is. See `docs/RECORDINGS.md`.

**Fixed (2026-10-01), re-verified live.** `ClampSeekToTail()` (`LiveEdgeMargin.h`), used by both
`SeekInProgressRecordingStream()` and `SeekLiveTimeshiftStream()`, lands a seek past the tail target on the tail target -- or on the
current position when the reader is already past it, or on the requested position when that is behind the reader -- so a seek
that was not backward never moves backward. Live, at the tail of a young in-progress recording with the same Kodi forward steps that
produced 262,144 to 1,310,720 bytes of backward movement before: every clamped forward seek now leaves the position unchanged
(delta 0 in all ten), the seek-to-live from far behind still lands on the tail target, and Kodi's own short backward seeks inside the
margin land where they were asked (previously pulled further back). The live-timeshift call uses the same helper; its three-segment
margin was not exercised live. The "skip forward does nothing with under 30 s left" behaviour is unchanged by design -- there is no
further to go. See `docs/RECORDINGS.md`.

#### Permanently unprobeable in-progress segment could stall playback

**A permanently-unprobeable in-progress-recording segment could stall
playback and start a probe storm forever, flagged from a 55th-pass
audit (2026-09-27), needs a live test, not fixed this pass.**
`CountLeadingProbedSegments()` (`M3u8SegmentParser.cpp`) only merges
entries up to (not including) the first probe that returned `<= 0`,
retrying the unmerged rest every cycle -- by design, for a transient
probe failure (see that function's own comment). But
`RefreshInProgressRecordingManifest()`'s own `finished` flag
(`ResolveInProgressFinished()`, `RecordingVisibility.h`) only ever
becomes true when `allProbesSucceeded` -- `failedProbeCount == 0` --
which stays false forever if the SAME segment index fails to probe on
every single cycle, not just transiently: a permanently 0-byte or
permanently non-200 listed segment would leave `finished` stuck false
even after the recording has genuinely completed server-side, so the
catch-up loop in `ReadInProgressRecordingStream()` never sees EOF and
every later segment (real, otherwise-probeable content) stays
unmerged behind it too -- indefinite playback stall, plus every
refresh cycle re-probing that segment and everything after it (up to
16-way concurrent fan-out) for as long as the recording exists.
Couldn't confirm a real trigger this pass: would need either an ffmpeg
HLS-muxer source check (does `hlsenc.c` ever list a genuinely 0-byte
segment, e.g. at a stream restart/discontinuity) or a live recording
test deliberately inducing one. If confirmed, a possible direction
(not confident) is accepting a 0-byte probe as a legitimate empty
segment rather than a failure, and/or capping consecutive-failure
retries for the same segment index before giving up on it specifically
rather than blocking every later one indefinitely.

**Attempted (2026-09-30): can't be induced here.** A segment the addon can't probe but the server lists needs either `hlsenc.c` writing a genuinely 0-byte listed segment or a server that answers the probe and the fetch differently; neither can be arranged against Dispatcharr's real recordings, and a forwarder that fails one segment's requests would just test the ordinary missing-segment path. Stays open.

**Live check (2026-10-01): reproduced, and worse than the entry described.** A server that lists a segment the addon
cannot size is easy to arrange once you notice that the playlist lists *absolute* segment URLs at the server's own host: a
proxy that rewrites them back through itself sees every probe and can answer one segment's HEAD with `Content-Length: 0`
(the earlier attempt forwarded only the playlist and API, so the probes never passed through it). With one segment of an in-progress
recording broken that way: playback stalled at exactly the start of that segment and never moved again; the addon sent
about 14 HEAD probes a second (415, 427 and 426 in successive 30-second windows) because every refresh re-probes *every* later
segment too, each refresh took 3-5 seconds, and a single `Read()` blocked for 200-255 seconds (`catch-up loop used 49/49
attempts`), so Kodi's Stop, issued during one, did not take effect for 4 minutes 24 seconds. Worst, the recording never finished: its
playlist carried `#EXT-X-ENDLIST` and all 92 segments within minutes, but its status stayed `recording` for about ten
minutes past its scheduled end, because each probe is a `.ts` request that refreshes the server's viewer key and Dispatcharr
finalizes only after that key lapses; it flipped to `completed` 25 seconds after the addon closed the stream. So a single bad
segment pins a recording open for as long as anyone has it open. A 404 for the segment takes the same branch (`res != CURLE_OK || httpCode != 200`),
but wasn't run separately. Still unknown: whether a real server ever lists such a segment. Fix direction, now concrete: (1) once the
leading unmerged segment has failed on consecutive refreshes, probe only that one until it answers, instead of the whole tail
(the tail's sizes are discarded anyway until it does); (2) after a bounded number of consecutive failures or seconds, merge it
as a zero-byte placeholder (offsets stay consistent, the byte-offset bug the leading-merge rule exists for cannot recur, and
playback continues past a few seconds of missing data) and log that once. See `docs/RECORDINGS.md`.

**Fixed (2026-10-01), re-verified live against the same fault.** `UnprobeableSegment.h`: a leading segment that failed on the previous
refresh is probed alone (nothing behind it can be merged), and after five consecutive failed refreshes spanning 30 seconds it is merged as a
zero-byte placeholder with one warning in the log, so playback continues past it. Live, with segment 20 of a four-minute
recording answering its HEAD with a zero length: 47 probes of that one segment in the 30 seconds (about 1.5 a second, each refresh ~0.07 s)
instead of ~14 a second against the whole tail, the warning at 30 s, playback carried on to the real end of the recording, and the
recording finalized within seconds of the player ending (before: playback never resumed, the recording stayed in `recording`
status indefinitely). With the break placed ahead of a viewer waiting at the live edge, playback stalled for 28 s and then resumed. Not
changed: whether a real server ever lists such a segment, and the 200+ s catch-up budget of a single blocking read, which the fast
refreshes now keep short in this case but which still applies to any other wait. See `docs/RECORDINGS.md`.

#### timeshift_buffer HTTP-server and reaper startup checks are unlocked

**`timeshift_buffer`'s own HTTP-server-startup and reaper-thread-startup
checks are unlocked, flagged from an 8th-pass audit (2026-09-26), low
priority -- narrow window, not reproduced live.**
`_ensure_http_server_running()`/`_ensure_reaper_running()` both
check-then-create with no lock; two `run()` calls landing on the very
first request at nearly the same instant (in different threads of the
same worker process) could both pass the "not running yet" check
before either finishes starting its own server/thread. For the HTTP
server specifically, `SO_REUSEPORT` would let a second bind silently
succeed, orphaning the first server's own reference so `stop()` can
never shut it down. For the reaper, `_reaper_stop_event` would get
overwritten, leaving the first thread unstoppable. Needs a lock around
the check-and-create in each function to fix properly; not attempted
this pass given how narrow the window is (only the very first `run()`
call(s) after a fresh worker start, not steady-state operation).

**Fixed (2026-10-02) in `timeshift_buffer` 0.6.8, unit-tested, not reproduced live (as the entry said, the window is only the first requests after a worker starts).**
`_ensure_http_server_running()` now runs its check-and-create under a reentrant lock (the config-change restart calls the stop path while
holding it) and `_stop_http_server()` takes the same lock; `_ensure_reaper_running()` does its check-and-start under its own lock. New tests
start eight threads at once on a fresh module and assert exactly one server is created and exactly one reaper started (each creation is
slowed so an unlocked check-then-create loses the race), that a storage-path change restarts the server without deadlocking, and fail
with the locks removed. See `docs/TIMESHIFT.md`.

#### CI release-upload actions pinned to mutable tags

**CI's release-upload actions are pinned to mutable tags, not commit
SHAs, flagged from a 58th-pass audit (2026-09-27), a design decision,
not fixed this pass.** `actions/checkout@v7`, `actions/upload-artifact@v7`,
and `softprops/action-gh-release@v3` (`.github/workflows/build.yml`)
are all pinned to a major-version tag, which its own publisher can
repoint to a different commit at any time -- a compromised or
malicious update to any of these (most concerning for the
less-widely-scrutinized third-party `softprops/action-gh-release`)
would run with this workflow's own `contents: write` permission on the
three release-upload jobs (see this same pass's own fix scoping that
permission down from workflow-wide). SHA-pinning each action (with a
`# vN` comment noting which released version the pinned commit
corresponds to, so a future intentional upgrade stays legible) would
close this, at the cost of needing a manual SHA lookup/update on every
future upgrade instead of `@v7` picking up patch releases automatically
-- not attempted blind this pass given that ongoing maintenance
tradeoff.

**Fixed (2026-10-02).** Every `uses:` in `.github/workflows/build.yml` is now `@<commit SHA> # vX.Y.Z`: `actions/checkout` `3d3c42e5...` (v7.0.1),
`actions/upload-artifact` `043fb46d...` (v7.0.1), `softprops/action-gh-release` `efb35369...` (v3.0.3, which is what `v3` resolved to). The ongoing-maintenance
cost the entry weighed is already covered: `.github/dependabot.yml` has the `github-actions` ecosystem on a weekly schedule, and it updates SHA pins and their
version comments together. `tools/tests/test_dependency_pins.py` fails if a workflow action is ever pinned to a tag again, or a pinned one loses its version comment.

#### FetchContent GIT_TAG pins are mutable tags

**`FetchContent_Declare()`'s own `GIT_TAG` pins are mutable version
tags too, flagged from a 59th-pass audit (2026-09-27), the same design
decision as the "CI release-upload actions pinned to mutable tags" entry, not fixed this pass.**
`CMakeLists.txt` (`GIT_TAG v3.11.3` for nlohmann/json, `GIT_TAG v1.14`
for pugixml) and `tests/CMakeLists.txt` (the same two, plus `GIT_TAG
v3.16.0` for Catch2) all pin a tag, not a commit SHA -- confirmed by
reading both files directly. Arguably higher-stakes than the CI-action
case: `CMakeLists.txt`'s own two dependencies (unlike the test-only
project's) get vendored directly into the actual shipped addon binary
on every platform this project builds for, not just one CI job's own
ephemeral runner. Same fix and same tradeoff as that sibling entry
(pin a commit SHA with a version comment, at the cost of losing
automatic patch-version pickup on a bare tag bump) -- not attempted
blind this pass, logged alongside it rather than actioned separately.

**Fixed (2026-10-02).** All five `GIT_TAG`s in `CMakeLists.txt` and `tests/CMakeLists.txt` are commit SHAs with the release in a trailing comment (nlohmann/json
`9cca280a...` v3.11.3, pugixml `db78afc2...` v1.14, Catch2 `317ac1ed...` v3.16.0 -- the commit the annotated tag points at, not the tag object). A fresh configure and build of the test
project with the SHA pins fetched all three and passed all 918 tests; the addon's own CMake file builds through the Kodi harness the same way. Covered by the same pin test as the
CI actions.

#### Prebuilt Windows dependency archives have no integrity check

**The three prebuilt Windows dependency archives (curl/OpenSSL/zlib)
have no integrity check beyond TLS, flagged from a 58th-pass audit
(2026-09-27), a design decision, not fixed this pass -- see
`-DCMAKE_TLS_VERIFY=ON`'s own comment (`.github/workflows/build.yml`/
`docs/BUILDING.md`) for the closely related gap that flag is actually
scoped to (corrected 2026-09-27, a 59th-pass audit, fixing a real,
confirmed overstatement in this entry's own original wording, found
via a project-wide review, confirmed against CMake's own bundled
documentation: that flag was NOT a fix for a real, currently-existing
gap in this exact command -- any CMake new enough to run this
command's own `Visual Studio 18 2026` generator already defaults to
verifying TLS certificates, confirmed against CMake's own release
notes -- it's an explicit pin against one narrow escape hatch, see its
own comment for the full account).** Even with genuine TLS
certificate verification in place (whether by that explicit pin or,
more accurately, already by CMake's own current default), a compromise
of the legitimate `mirrors.kodi.tv` redirect target itself (not just a
network-position attacker) would still go undetected: nothing pins a
SHA256 for any of the three archives, and their contents
(`libcurl.dll`/`openssl.dll`/`zlib.dll`) ship inside this project's own
Windows release zip. Not fixed this pass: Kodi's own `add_internal()`
(`cmake/addons/depends/windows/CMakeLists.txt`, upstream Kodi source,
not this repo) calls `externalproject_add(URL ...)` with no `URL_HASH`
argument and no plumbing to accept one from the
simple `id url` text-file format this repo's own fetch step writes --
adding a hash check would mean patching Kodi's own upstream CMakeLists.txt,
out of scope for this repo, or verifying the SHA256 as a separate,
new step after download but before the `cmake --build` that consumes
it (fetched and confirmed live this same pass, for reference: curl
`58106b42cbb3d6d951671e71e00703dd644b5b729cfd418b2ba08e4a02780145`,
openssl `d744eb075f7628156979cd8cb9545fb7c6cc567efe901a1de1ab9b103b85d4e7`,
zlib `c596b93016d62f6bc85b38b2f18746c8382cf89b1a99c1c56f0d2bf51ecf54f1` --
not wired into the build itself this pass).

**Fixed (2026-10-02), the "separate step after download" option the entry offered, made stronger.** The Windows job now downloads each archive itself, checks it against a
pinned SHA256 (hashes recomputed from a fresh download today; they match the ones recorded in this entry on 2026-09-27) and fails on a mismatch, then registers the *local verified file* with
Kodi's `add_internal()` instead of the mirror URL -- `ExternalProject_Add(URL ...)` accepts a local path, so no patch to Kodi's source is needed, and the file checked is the file installed, with no
second download a compromised mirror could answer differently. `docs/BUILDING.md`'s copy of the step is updated, and `tools/tests/test_dependency_pins.py` checks the two copies pin the same hashes.
The first real CI run of the step (2026-10-02, on the Windows runner) passed the hash checks but failed in Kodi's generated CMake script: the local path pasted in with Windows backslashes is an invalid string escape (`\g`), so the path is registered with forward slashes. The end-of-life OpenSSL/curl versions those archives contain is a separate open item.

#### Kodi's own threads waited on the guide fetch, and two transfers could not be aborted at shutdown

**Found 2026-10-03 by a code-reading pass, reproduced live, fixed and
re-verified the same day.** Two gaps left by "Kodi shutdown / NEED_RESTART
teardown can block for minutes" (fixed 2026-10-02). `GetXmlTvGuide()` and the
live-timeshift segment fetch in `ReadLiveTimeshiftStreamOnce()` built their
curl handles by hand and never went through `ApplyStandardCurlOptions()`, so
neither carried the transfer-progress callback `AbortInFlightRequests()`
relies on (the heartbeat POST likewise, harmless at its fixed 2 s timeout).
The guide fetch is the largest transfer the addon makes, runs on the
background thread at every start, and is allowed four times `timeout` (120 s
by default). Installing the callback there changed nothing live, which
exposed the larger half: `GetEPGForChannel()` called `EnsureEpgLoaded()`,
which either runs the guide fetch on the calling thread or waits on
`m_epgFetchMutex` for a fetch another thread is running. Kodi calls
`GetEPGForChannel()` on its own PVR manager thread while it loads timers at
start (`CPVRTimerInfoTag::GetEpgInfoTag()` -> `CPVREpg::GetTagBetween(...,
bUpdateFromClient = true)` -> `UpdateFromScraper()`, confirmed against Kodi's
source) and on its EPG thread afterwards, and `CPVRManager::Stop()` waits for
those threads before the addon instance is destroyed -- so with a guide
download stalled, a Kodi thread sat inside the addon and the destructor, the
only place the abort flag is set, never ran until the download timed out.

Live, through an HTTP-aware forwarder in front of Dispatcharr that forwards
everything but holds `GET /output/epg` open without answering, with Kodi told
to quit about ten seconds into the stalled fetch: the previous build exited
108.7 s later (the rest of the 120 s guide timeout; its log shows "Timeout was
reached" and only then "unload skin"); with only the callback fix, 108.8 s,
the log showing "PVR Manager: Stopping" and then nothing until the fetch timed
out, the next line being the PVR manager thread's `GetEPGForChannel` error;
with both fixes, 3.7 s, the fetch ending with "Operation was aborted by an
application callback" 1.7 s after "PVR Manager: Stopping".

**Fixed (2026-10-03), live-confirmed as above.** The guide is fetched only by
the background channel/EPG thread. `GetEPGForChannel()` no longer calls
`EnsureEpgLoaded()`: it calls `RequestGuideFetchIfWanted()`, which wakes that
thread (`m_channelEpgRefreshRequested`, set under the thread's mutex) when
`dispatcharr::ShouldAttemptGuideFetch()` (`Staleness.h`, unit-tested; the same
gate `EnsureEpgLoaded()` applies -- stale or a post-renumbering refetch due,
and the failure backoff run out) says a fetch would run now, and answers from
the cache. At startup that answer is `PVR_ERROR_SERVER_ERROR` until the guide
lands (Kodi logs each such call at error level for those few seconds; it
already did whenever the background thread held the fetch first), after which
the thread's existing per-channel `TriggerEpgUpdate()` pass and its
timers/recordings re-trigger bring Kodi up to date -- the path Kodi already
took whenever the background thread won the startup race. The three hand-built
handles now go through `ApplyStandardCurlOptions()` (the guide fetch and the
heartbeat then override the timeout). Re-verified against the real instance
after the change: Kodi's PVR manager started 10 s after the instance was
created instead of waiting for the guide, the guide landed 13 s later and
triggered the timers/recordings refresh, and Kodi then held a full guide for a
checked channel; `tools/kodi_smoke_test.py` against that
build then passed 17 of 18 checks (live, recorded, in-progress and catch-up
playback with seeks, timer/rule create and delete, the realtime push), the
one skip being its live-seek check reading `canseek` while a freshly started
server buffer was still filling. Residual and left alone, each bounded by
`timeout`: the channels, recordings and timers fetches still run on Kodi's
threads (those callbacks must answer synchronously), and the realtime
WebSocket connect is bounded by its own connect timeout.

#### Unchanged recurring-rule times were re-sent on every edit for a rule near UTC midnight

**Fixed 2026-10-03 (a hardening pass), found by reading the code, confirmed
by unit test and against Dispatcharr's own 0.31.0 source, and reproduced and
re-verified live the same day through Kodi's own timer dialog (see the end of
this entry).** The edit diff
added on 2026-10-02 (`ComputeRecurringRuleEditPatch()`, `RecurringRuleEdit.cpp`)
compared the time of day Kodi's echoed timer converts to
(`ComputeRecurringRuleFields()`, which deliberately left a result that
crossed UTC midnight unwrapped -- a 22:00 rule in a zone behind UTC came
out as -7200, and a 06:00 rule in a zone far enough ahead of it as 108000) against
the value the server reports, which is always 0..86399 (79200, 21600). They
never compared equal, so for any rule whose Dispatcharr-local time of day
falls on the other side of UTC midnight from its UTC time of day -- evening
prime time anywhere in the Americas, early morning in Asia -- an edit that
changed nothing still sent `start_time` and `end_time`, and so did a bare
enable/disable toggle or a rename. The same wire value, so no data changed,
but `RecurringRecordingRuleViewSet.perform_update()` (`apps/channels/api_views.py`)
calls `sync_recurring_rule_impl(rule.id, drop_existing=True)` on every PATCH
of an enabled rule: every future occurrence dropped and regenerated, the
exact cost the diff exists to avoid, and the stale-cache "last writer wins"
exposure left open for those two fields. A sibling in the same
representation: `TruncateRuleTimesToWholeMinutes()` (create only) used C++'s
`%`, which truncates toward zero, so a negative value rounded UP -- 21:59:30
as -7230 became 22:00:00, a minute later than the dialog showed.
Fixed in three places, each with a test: `ComputeRecurringRuleFields()` now
wraps both times into [0, 86400) (its header comment and the test that pinned
the unwrapped value are updated; the round-trip test's display instant moved
from a day before the first day onto the first day itself, since the
display function was being fed the unwrapped value there -- production feeds
it the server's own value and is unchanged), `ComputeRecurringRuleEditPatch()`
compares times of day modulo one day (the same "compare as it crosses the
wire" rule it already applied to start dates), and the truncation uses a
floor.
**Live, 2026-10-03, against the real instance and the
real Linux Kodi client:** a disposable Monday-Friday rule of the shape described above was created through the API, its edit dialog was opened
from the Timer rules window with Kodi's Info action and OK pressed with
nothing changed. On the previous build the server answered with a burst of
`recording_updated` events and every one of the rule's 21 future occurrences
came back under a new id: the unchanged save had sent
the times and the server dropped and regenerated the lot. On the fixed build
the same save logged "UpdateTimer: recurring rule ... unchanged, nothing to
send" and all 21 occurrence ids were identical afterwards. The rule was then
deleted (404 on re-read, no occurrences left).
**The create path was driven live the same day too, through Kodi's own Add
timer dialog on the Linux client (the only path that reaches
`TruncateRuleTimesToWholeMinutes()`, and the first time rule creation from that
dialog was exercised live at all):** a "Recurring recording (day-of-week)" rule
named through the on-screen keyboard, every day, start and end typed on the numeric pad. Dispatcharr stored the typed times exactly (no carried seconds, no rounding up a
minute), the name with its `[Kodi]` tag, days `[0..6]`, the dialog's first day
as `start_date` and a 30-day `end_date`; the addon's own debug line reported
weekdays 0x7f and a day shift of 0; Kodi listed the rule as "Any day" at the typed times with first day today and its 31 generated
occurrences as child timers. Deleted through the API afterwards (204, 404 on
re-read, 0 occurrences left).

#### Recurring-rule edits send back Kodi's possibly-stale cached fields

**Every recurring-rule edit sends back Kodi's own possibly-stale
cached copy of every field, not just the one the user actually
changed -- a "last writer wins" gap with no way to tell the two apart,
flagged from a 63rd-pass audit (2026-09-27), confirmed against the
current source, not itself independently reproduced.**
`BuildRecurringRuleUpdateBody()` (`TimerRequestBuilder.cpp`)
unconditionally sends `channel`/`name`/`days_of_week`/`start_time`/
`end_time`/`start_date`/`enabled` together on every `UpdateTimer()`
edit, all sourced from Kodi's own cached `kodi::addon::PVRTimer`
object -- itself only as fresh as this addon's own last `GetTimers()`
refresh (`recording_refresh_minutes`, default 5 minutes, or sooner via
a realtime event if enabled). A change made to any *other* field via
Dispatcharr's own web UI, or a second Kodi install sharing the same
account, within that staleness window is silently overwritten the
moment this Kodi sends back any edit at all -- even one as narrow as
toggling `enabled`, which has nothing to do with the reverted field.
There's no ETag/version check of any kind to detect this. The
one-time-recording rename path has the same shape on a smaller scale:
`ShouldRenameOnTimerEdit()` (`TimerIdentity.h`) compares Kodi's own
stale cached title against the fresh server-side one specifically to
decide whether an edit should *also* rename the recording -- so a
schedule-only edit made shortly after a title change from elsewhere
sends back the OLD title, reverting that rename (and, per
`RenameRecording()`'s own real Dispatcharr-side effect, marking the
recording `user_edited` in the process). A real fix would mean
re-fetching the rule/recording fresh immediately before building the
PATCH body and diffing only the fields Kodi's own edit actually
touched, rather than trusting the timer object Kodi hands back --
needs a live check first to confirm the practical window (how stale
the cache realistically gets under normal use) is wide enough to
matter before attempting that.
**Live-test attempt (2026-09-29): blocked, not reachable via
JSON-RPC at all -- confirmed by checking Kodi's own real current
JSON-RPC schema (`interfaces/json-rpc/schema/methods.json`), the same
hard limit already established elsewhere this session for anything
living in `UpdateTimer()`'s own edit-only code path.** There is no
`PVR.UpdateTimer` method (confirmed: no such method exists in the
schema at all, matching the earlier-established finding). `PVR.ToggleTimer`
-- this project's only other JSON-RPC lever that reaches into
`UpdateTimer()`/`AddTimer()` at all -- is keyed by `broadcastid` and
only ever creates or deletes a one-time timer or a *series* rule
(`timerrule: true`), never a plain channel+day+time *recurring* rule
(`kTimerTypeRecurring`) at all -- there's no broadcast to key off of,
so it can't reach this code path even indirectly. Reaching
`UpdateTimer()`'s recurring-rule branch genuinely requires Kodi's own
GUI timer-edit dialog or the enable/disable toggle action, neither of
which JSON-RPC can drive. Re-verified the underlying claim by reading
current `PVRDispatcharr.cpp`'s own `UpdateTimer()` recurring-rule
branch directly instead (a source-level reconfirmation, not a live
round trip): `channel`/`days_of_week`/`start_time`/`end_time`/
`start_date`/`enabled` are still sent to `UpdateRecurringRule()`
exactly as this entry describes, straight from Kodi's own possibly-
stale `timer` object, with no diff against a fresh fetch anywhere in
between -- unchanged since the 63rd-pass audit that first flagged
this, no regression and no fix either. One real nuance this entry's
original wording didn't capture: `name` specifically already has a
narrow guard, `ResolveRecurringRuleNameForUpdate()` (`TimerIdentity.h`)
-- but only for the one placeholder-title case
(`kodiTitle == "Recurring recording <id>"`), added for a *different*
bug (see that function's own comment). Any other, real (even if
stale) cached title still passes through verbatim
(`return kodiTitle`) -- the general "last writer wins" exposure this
entry describes is unchanged for `name` too, just narrower than a
fully blind pass-through in that one specific edge case.
single upcoming episode of a series rule from Kodi doesn't stick
either, flagged from a 32nd-pass audit (2026-09-26), confirmed against
Dispatcharr's own real current upstream source, not itself
independently reproduced -- this is Dispatcharr's own limitation (its
web UI shares it), so there's no addon-side fix beyond a possible
warning or outright refusal, not attempted blind.** Reached via
`DeleteTimer()`'s one-time-recording branch, when Kodi's own "delete
only this timer" is chosen for a series-rule child. Why it comes back:
`evaluate_series_rules_impl()` (`apps/channels/tasks.py`) deduplicates
only against `Recording` rows that currently exist
(`existing_program_keys`/`existing_episode_keys`/`existing_program_index`,
all built from a fresh `Recording.objects.filter(end_time__gte=now)`
query) -- Dispatcharr keeps no persistent "the user explicitly deleted
this specific episode" exclusion list anywhere, so once the row is
gone, evaluation has no way to know it was a deliberate delete rather
than "nothing scheduled here yet." When it comes back: this
evaluation runs after every EPG source refresh
(`apps/epg/tasks.py`), and also on this addon's own `evaluate/` call
after every `CreateSeriesRule()` -- which includes any series-rule
*edit* via `UpdateTimer()`'s own upsert, not just a genuinely new
rule. So the deleted episode can be silently re-scheduled within
seconds of an unrelated series-rule edit made from Kodi, not just on
the next EPG refresh. Kodi's own "delete just this one" UX makes it
look like it worked, with nothing telling the user it didn't stick.

**Fixed (2026-10-02), live-confirmed -- and the first version of the fix was wrong in a way only the live run showed.** An edit now sends only the fields the user changed
(`RecurringRuleEdit.h`: `ComputeRecurringRuleEditPatch()` against a baseline, `BuildRecurringRuleUpdateBody()` taking the resulting patch), and a save that changes nothing sends nothing, which
also stops the server dropping and regenerating every future occurrence for a no-op edit. The baseline matters: first it was the cached rule, but the cache is refreshed between Kodi
filling its dialog and the user saving, so a change made elsewhere in that gap read as the user's own and was sent straight back -- reproduced live (a rule renamed and given new days from outside,
then deactivated in Kodi: name and days reverted). The baseline is now `m_reportedRecurringRules`, the rules exactly as `GetTimers()` last handed them to Kodi, which is what the dialog was filled
from. Re-run live with the same sequence: only `enabled` changed, name and days kept; the addon made one GET of the rule and one PATCH. The one-time-recording rename path keeps its existing guard
(`ShouldRenameOnTimerEdit()`).

#### Recurring rule with a null end_date can't be edited

**Confirmed from Dispatcharr's own current upstream source, not live:
a recurring rule with a genuinely null `end_date` can't be edited at
all through this addon, contradicting `docs/RECURRING_RULES.md`'s own
"leaves an open-ended rule exactly as it was" framing, flagged from a
20th-pass audit (2026-09-26); see that doc's own follow-up note for
the full account.** `RecurringRecordingRuleSerializer.validate()`
raises "End date is required" whenever *neither* the PATCH body *nor*
the existing instance has an `end_date` -- so `UpdateRecurringRule()`'s
own omit-`end_date` PATCH doesn't preserve a null `end_date`, it fails
the whole edit (even a bare enable/disable toggle) with a 400. Only
matters if a null-`end_date` rule can actually reach this addon in the
first place -- `CreateRecurringRule()`'s own comment already notes
Dispatcharr's serializer requires one on *create* through this addon's
own flow, so this would need a legacy row or one created another way
entirely (Django admin/shell). Not fixed this pass -- needs
confirmation such a rule genuinely exists in practice before deciding
whether `UpdateRecurringRule()` should start sending a fallback
`end_date` whenever `cachedEndDate <= 0`.

**Live check (2026-09-30): confirmed, and such a rule is reachable.** Creating one is refused (`POST` with `end_date` omitted or null: 400 "End date is required"), but a `PATCH` of `{"end_date": null}` on an existing rule is accepted, so a rule can lose its end date through the API or the web UI. On such a rule every edit this addon can send -- the full body `UpdateRecurringRule()` builds, and a bare `{"enabled": false}` toggle -- is a **400 "End date is required"**, exactly as read from the source. So an open-ended rule can't be edited, enabled or disabled from Kodi at all. The open question is what to do: sending a fallback `end_date` turns "records forever" into "records only while this addon keeps renewing it" (the conversion `ShouldExtendRecurringRuleEndDateOnUpdate()` already refuses to make), while leaving it means the edit fails with the server's message. Moved to design decisions.

**Fixed (2026-10-02), live-confirmed.** `ComputeEndDateForOpenEndedRuleEdit()` (`RecurringRuleEdit.h`): when the rule has no end date -- read fresh from the server on every edit now, not from the
cache -- the PATCH carries one, a rolling window counted from today (or from the start date when that is still ahead, so it never lands before it). Server behaviour checked directly first: a partial PATCH
of one field is accepted and leaves the rest alone, a toggle of a rule with a null `end_date` is a 400 "End date is required", and the same toggle with an `end_date` is a 200. Live: a rule stripped of its
end date, then Activated from Kodi: succeeded, `end_date` now 30 days out, with the addon logging that it gave the rule a rolling window. The cost is the one the entry named: an open-ended rule becomes
"records while a Kodi running this addon renews it".

#### Editing a series rule's match pattern creates a duplicate

**Editing a series rule's real match pattern ("Search guide for")
creates a duplicate rule instead of renaming the original, flagged
from a 17th-pass audit (2026-09-26) -- a pre-existing, already-
documented limitation, now reached via the field it was always meant
to apply to; see `docs/RECORDINGS.md`'s own follow-up note on the
"Search guide for" fix above for the full account.** Confirmed against
Dispatcharr's own current upstream source
(`SeriesRulesAPIView.post()`, downloaded to a scratchpad for review,
not committed to this repo -- stronger than the API shape alone, not
the same standard as a live test): the upsert matches by
`(tvg_id, title, epg_source_id)`, so any genuine identity change
(a new search pattern, or a channel change re-deriving `tvgId`)
appends a second rule rather than editing the first -- the old rule,
and its own future recordings, are left fully in place. A more
complete fix would detect the identity change against the cached rule
(a pure, testable decision -- e.g.
`ShouldReplaceSeriesRuleOnEdit(oldTitle, oldTvgId, newTitle, newTvgId)`)
and delete the old rule as part of the edit, but the ordering has a
real data-loss subtlety: delete-then-create risks losing the rule
entirely if the create call then fails, while create-then-delete
risks the old rule's own delete wiping recordings the new rule's
server-side evaluation had already materialized (Dispatcharr's own
evaluation skips a programme that already has a recording, so a
delete landing after a fresh evaluate could remove recordings the new
rule now depends on having created). Needs a live test to pick the
right ordering before shipping, not a blind implementation.

**Fixed (2026-10-02); the server semantics checked live, the edit path itself only compiled and unit-tested.** Approved ordering: create the new rule first, delete the old one only after that succeeds
(`ShouldReplaceSeriesRuleOnEdit()`, `TimerIdentity.h`, true for a changed title or `tvg_id`, compared exactly like the server's upsert). The entry's worry about the delete wiping recordings the new rule had
materialized does not apply: Dispatcharr does not evaluate a rule at create time (its own handler says the frontend calls evaluate separately), and the delete purges only recordings of the *old* identity. Source
changes are deliberately not treated as an identity change, because a delete without a source matches every source's rule with that title and `tvg_id`, including a legacy rule the same save upgraded in place. Checked against the real
server through its API: two rules created, the old one deleted by `(tvg_id, title)`, the new one left intact. If the delete fails the new rule exists, so the edit is reported as saved with a warning notification to remove the old one.

#### View-only account is offered Record/Delete/Rename/Stop

**A view-only Dispatcharr account is offered Record/Delete/Rename/Stop
in Kodi even though every one of those always fails server-side --
design decision, not a bug, flagged from a 48th-pass audit
(2026-09-27), confirmed against Dispatcharr's own real current
upstream source, not reproduced live.** `GetCapabilities()` advertises
delete/rename/timer support unconditionally, for every account.
Dispatcharr's own default `dvr_access` for a Standard user with no
explicit `custom_properties.dvr_access` set is `"view"` (confirmed
already, see this doc's own 41st-pass entries on the same default),
and every one of create/update/destroy/stop/extend/`update_metadata`
requires `IsAdminOrDVRManager` -- so such an account sees the same
Kodi UI as an admin, but every action it tries fails with a generic
error. `GET /api/accounts/users/me/`'s own `custom_properties.dvr_access`
(admin-only-writable) could detect this reliably, but `GetCapabilities()`
is only read once per instance -- the real choices are a one-time
startup notification (matching the existing Server-side/non-admin
warning) or greying out the relevant settings (matching
`dispatcharr_is_admin`'s own pattern). Needs a decision on which UX,
not attempted blind this pass.

**Fixed (2026-10-02), live-confirmed, with the "grey out" option.** `DvrAccess.h` mirrors the server's `get_dvr_access()` (admin manage, streamer none, a Standard user takes `custom_properties.dvr_access`, and an unset one is view);
`GetCapabilities()` now advertises timers, recording delete and recording rename only for an account that can manage, so Kodi hides the Record button, the Timers window and the delete/rename actions instead of offering things that
only fail. Fails open when the level cannot be read, retried by the background thread; because Kodi reads capabilities once, a level learned only on a retry cannot change what is offered until a restart, and the user is told so. Live, with a
disposable Standard user nobody configured (`user_level` 1, empty `custom_properties`): the account was refused 403 on a DVR write, the addon logged "DVR access is view-only", Kodi reported `supportstimers: false`, and the recordings list and
playback still worked (the recordings list intact). The user was deleted afterward and the Kodi settings restored.

#### DeleteTimer(): failed-but-not-confirmed-gone lookup

**`DeleteTimer()`'s one-time-recording branch has no real answer for a
failed-but-not-confirmed-gone `FindRecordingById()` lookup, flagged
from a 49th-pass audit (2026-09-27), a genuine design question rather
than a confirmed bug.** `ShouldStopInsteadOfDelete(forceDelete,
lookupOk, serverSaysInProgress)` (`RecordingVisibility.cpp`) reduces to
plain `forceDelete` whenever `lookupOk` is false -- a lookup that fails
for any reason (a transient network error, a timeout, not just a
confirmed 404) with Kodi's own cached `forceDelete=false` still routes
straight to the destructive `DeleteRecording()` call, with no
confirmation of whether the recording actually exists, is in progress,
or already finished server-side. Both `DeleteRecording()` (pass 48) and
`StopRecording()` (this same, 49th, pass) now tolerate a 404 as success,
so "the recording is already gone" is handled equivalently either way --
but that doesn't resolve the real question: whether `StopRecording()`
behaves safely (rather than erroring, or silently no-opping while
leaving the timer/recording still present) when called against a
recording that's actually already finished, not in progress. Defaulting
to Stop-on-unknown-lookup-failure is only safer than the status quo if
that's true; if it isn't, it would silently fail a user's genuine
delete/cancel action for an ambiguous reason instead.

**Update (2026-09-27, a 50th-pass audit): answered from Dispatcharr's
own real current upstream source instead of a live test (the source
itself is unambiguous here, so no live check is needed) --
Stop-on-unknown-lookup-failure would NOT be a safe default either.**
`RecordingViewSet.stop()` (`apps/channels/api_views.py`) only rejects
with a 409 for a status already in `{"completed", "interrupted",
"failed"}`. Every other status -- including `"stopped"` (idempotent,
200) and, critically, `"scheduled"`/empty (a recording that hasn't
started yet) -- gets an unconditional 200 that sets
`custom_properties.status = "stopped"` and revokes the Celery task.
So calling Stop against a *scheduled, not-yet-started* recording
(exactly the case a failed lookup can't rule out) doesn't cleanly
cancel it the way `DeleteRecording()` would -- it leaves a permanent
row with a terminal-looking status and no real file, which
`RecordingParser`'s own status handling then displays as an ordinary
finished recording: a phantom entry, not a clean cancellation. Both
existing options carry a real failure mode on a lookup failure --
Delete risks destroying an actually-in-progress recording; Stop risks
creating a phantom entry for a not-yet-started one -- so this remains
a genuine design decision (which failure mode is worse, and how often
a lookup actually fails this way in practice), not fixed this pass.

**Fixed (2026-10-02), live-confirmed.** Looked at closely: both guesses are wrong in a real way (Delete destroys a running recording; Stop leaves a terminal-status row with no file for one not started), so a failed lookup is no longer guessed past.
`DecideDeleteTimerAction()` (`RecordingVisibility.h`): a 404 means already gone (success); any other failure with Kodi's own `forceDelete` set keeps Stop; any other failure otherwise refuses with a notification to try again, since the delete would most
likely have failed on the same server trouble anyway. Tested exhaustively that an unverified lookup can never reach Delete. Live: with the lookup of a scheduled recording made to fail (500), `PVR.DeleteTimer` was refused and the recording stayed;
with a working lookup the same call deleted it.

#### OpenLiveStream() torn atomic read across Kodi callbacks

**`OpenLiveStream()` has a wider-window instance of the same
torn-atomic-read class a 61st-pass audit fixed in
`GetChannelStreamProperties()`, flagged from a 62nd-pass audit
(2026-09-27), a design decision, not fixed this pass.**
`OpenLiveStream()`'s own `if (m_liveTimeshiftMode != kLiveTimeshiftServer)
return false;` (`PVRDispatcharr.cpp`) is a second, independent read of
the same atomic, in a function Kodi calls separately and later, only
when `GetChannelStreamProperties()` left `STREAMURL` unset (Server
mode, per that function's own comment). If a settings save flips the
mode between those two separate Kodi callbacks, the same failure mode
the 61st-pass fix closed within one function -- an open attempt failing
because the two decisions disagree -- can still happen across the two.
The window here is real but larger than the one already fixed (it
spans however long Kodi takes to go from one callback to the other,
not just this addon's own function-local work), and this check is
already documented as "just defense in depth", reached only when
`GetChannelStreamProperties()` already committed to Server mode --
so the fix direction itself is a design call (drop the check entirely,
or accept the residual race as an acceptable defense-in-depth cost)
rather than a clear-cut correctness requirement, unlike the
within-one-function case that was fixed outright.

**Fixed (2026-10-02) by dropping the redundant check.** `OpenLiveStream()` no longer re-reads `m_liveTimeshiftMode`: Kodi only calls it because `GetChannelStreamProperties()` left `STREAMURL` unset, which already means Server mode, and the plugin call that follows is the real
gate and fails clearly on its own.

#### GetRecordings() EPG link doesn't survive EPG cache rotation

**`GetRecordings()`'s own new `SetEPGEventId()` link (a 34th-pass audit
fix, 2026-09-26) doesn't survive this addon's own EPG cache rotating
out an already-ended entry, so it only reliably links a finished
recording back to its EPG tag until the next XMLTV refresh (up to
`epg_refresh_hours`, 4h by default) -- flagged by that same pass,
needs a design decision, not fixed blind.** `ResolveRecordingBroadcastId()`
(shared by `GetTimers()`/`GetRecordings()`) looks the EPG entry up in
`m_epgByChannelNumber` fresh on every call -- but per this file's own
already-logged `prev_days` entry, an entry whose own end time has
passed drops out of that cache at the next XMLTV refresh (Dispatcharr's
`/output/epg` export itself only ever includes upcoming/current
programmes, confirmed against its own real current upstream source).
So a recording watched (or its guide entry clicked) soon after it
finishes links correctly -- the common case, and real, tangible value
over never linking at all -- but the same lookup against an
older recording (after the guide has refreshed past its own original
air time) silently finds nothing and the link is simply absent again,
no different from before this pass's fix, not actively wrong. A
durable fix needs to persist the resolved broadcast id (or the
programme's own start time) somewhere that outlives the EPG cache
itself -- e.g. once computed while the recording was still a timer
(`GetTimers()`'s own call already resolves it every refresh) -- keyed
by recording id, the same class of persistence this file's own
`PendingTitle` cache already does for a different field, but this
needs its own design (in particular whether/how it should be pruned,
and whether persisting past this addon's own process lifetime is
worth the complexity) rather than reusing that mechanism blind.

**Fixed (2026-10-02); persistence confirmed live, the rotation itself not forced.** `RecordingEpgLinks.h`: the start time of the EPG programme a recording is matched to is remembered per recording id the first time a match succeeds (usually while it is still a timer), because the
broadcast id is just `ComputeBroadcastId(channelId, programme start)` and that is all that is needed to recompute it. Used as the fallback whenever the guide no longer holds the programme, and trusted even when the channel is ambiguous now since a link is only ever remembered from an
unambiguous match. Persisted as `recording_epg_links.json` in the addon's user directory so it survives restarts; entries are validated against the recording's channel and start time (a moved recording, or an id reused after a database reset, is not trusted), pruned against a successfully loaded recordings
list, capped, and a corrupt or truncated file is ignored and rebuilt. Live: after normal use the file held its links; a Kodi restart kept them (one fewer after one recording was deleted and pruned) with no "unreadable" warning.

#### Residual gaps in the forced channel refresh before the EPG fetch

**Two residual gaps in the "force a channel refresh before the EPG
fetch" fix, flagged from a 27th-pass audit (2026-09-26), confirmed
against Dispatcharr's own real current upstream source, not
reproduced live.**
- **(a) If the forced refresh itself fails or is skipped, the XMLTV
  fetch still proceeds and commits against the stale channel list --
  the exact mismatch the fix exists to prevent, just reached a
  different way.** `EnsureEpgLoaded()` deliberately ignores
  `EnsureChannelsLoaded(true)`'s own return value (by design -- a
  failed forced fetch must not block the XMLTV fetch, see that call
  site's own comment), but that means a forced fetch that fails (a
  plausible trigger: `GetChannels()` uses a 1x timeout while
  `GetXmlTvGuide()` uses 4x, so a slow server with a large channel
  list could time out the channel fetch while the XMLTV one still
  succeeds) leaves the exact pre-fix pairing in place, now waiting out
  a full `channel_refresh_hours` to self-heal (recovery only retries
  once channels are already stale or a groups retry is pending).
  Needs a real design choice, not a blind fix: one option is marking
  the EPG attempt itself as failed too when the forced refresh didn't
  succeed, so both retry together -- the tradeoff being the EPG goes
  stale right along with channels if channels keep failing.
- **(b) Dispatcharr's own server-side XMLTV response cache (a 300s
  Redis chunk cache, confirmed against its real
  `apps/output/streaming_chunk_cache.py`) isn't invalidated by a
  manual channel-number edit.** `Channel`'s own `serializers.py`
  `update()` just calls `instance.save()` with no `update_fields`, and
  `signals.py`'s own `refresh_epg_programs` (plus a `ChannelOverride`
  save) only invalidates that cache when `epg_data` itself changed --
  not a plain renumber. So within 5 minutes of a manual renumber, this
  addon's own forced channel refresh correctly sees the new numbers
  and clears its own EPG cache, but the immediately-following
  `GetXmlTvGuide()` call can still get served the *cached*, pre-rename
  XMLTV -- committed as fresh for a full `epg_refresh_hours` (4h by
  default). Rough exposure: ~5 minutes out of a 4-hour window, roughly
  2% of manual renumbers by this estimate. (M3U-refresh-driven repacks
  do invalidate this cache correctly -- only manual single-channel
  edits are affected.) Possible more robust fix, and a pure-logic
  extraction candidate either way: cross-check each Kodi channel's own
  name against the `<display-name>` in the XMLTV entry for its number
  key (`XmlTvParser` doesn't currently parse that field at all) -- a
  mismatch would mean stale or colliding data, so skip or refetch.
  Would also cover finding (a) above and the already-logged
  duplicate-channel-number collision item. Needs a live check before
  choosing a behavior.

**Live check of (b) (2026-09-30): confirmed.** With a disposable channel numbered 88881 mapped to a real guide source: the first `/output/epg` fetch listed `<channel id="88881">`; after renumbering the channel to 88882 through the API, three further fetches over the next ~70 s (25 s each) **still listed 88881 and never 88882** -- the server's 300 s chunk cache is not invalidated by a manual renumber. So an EPG fetch that lands within five minutes of a manual renumber is committed against the new channel list but keyed by the old numbers, for a full `epg_refresh_hours`. (a) was not exercised (it needs a channel fetch that fails while the guide fetch succeeds). The channel was deleted afterward. Both halves need a design choice (mark the guide attempt failed when the forced refresh did; re-fetch a few minutes after a detected renumber; cross-check `<display-name>`), so this moves to design decisions.

**(a) fixed 2026-10-02, not exercised live; (b) was confirmed live earlier and stays a separate matter.** When the forced channel refresh before a guide fetch does not happen (it failed, or its own failure backoff is running), the guide attempt is now treated as failed too and retried on the ordinary
interval, rather than committing XMLTV keyed by the new channel numbers against a possibly stale channel list. Kept out of the durable-failure count (that tracks the guide endpoint rejecting the addon, which did not happen). The accepted cost: while channels keep failing the guide stays as stale as they are. Half (b), the server's
300 s guide cache serving pre-rename numbers after a manual renumber, is unchanged: it needs its own fix (re-fetch a few minutes after a detected renumber, or cross-check `<display-name>`) and is logged as that.

Half (b) was fixed 2026-10-02 -- see "Guide cache serves pre-rename channel numbers after a manual renumber".

#### Dispatcharr 0.31.0 bug fixes that may explain past symptoms; reverse-proxy DVR playback unchecked

**Two Dispatcharr `0.31.0` bug fixes plausibly explain past
playback/logo symptoms rather than introducing anything new to track
-- noted from the release notes (2026-09-19), not yet confirmed
against a live upgrade.** (1) "HLS finalize introducing periodic
audio/video gaps by concatenating segments" -- Dispatcharr's own
server-side raw-copy finalize, not this addon's client-side
timeshift-buffer segment handling, but the same general class of
symptom this project has chased before (`docs/TIMESHIFT.md`'s
`Packet corrupt`/continuity-counter investigations) -- a
finished-recording audio/video gap report going forward could just be
an old-Dispatcharr-version issue now fixed upstream, not this addon.
(2) "DVR playback redirects/playlists (and plugin logos) dropping or
mishandling non-standard ports and forwarded host/scheme behind a
reverse proxy" -- directly touches this addon's own recording
playback path (`.../recordings/{id}/file/`'s redirect to an HLS
playlist, documented at the top of `DispatcharrClient.cpp`) and both
companion plugins' own logo/manifest serving behind whatever reverse
proxy a user runs. Worth remembering the next time a reverse-proxied
deployment reports recording-playback or logo weirdness: check the
Dispatcharr version first, since `0.31.0`+ may have already fixed it
server-side.
**Update: (1) confirmed clean, (2) deliberately deferred
(2026-09-21).** (1) Downloaded a real finalized recording (raw-copy,
default output profile) straight
from a live `0.31.0` instance and ran a full `ffmpeg -v error -f null`
decode over the entire file -- zero errors or warnings across every
frame decoded start to finish; a codec-level check, not just
playback observation. Playback-side stalls seen along the way in an
earlier pass (via Kodi JSON-RPC monitoring plus `kodi.log`'s
`CVideoPlayerAudio::Process - stream stalled` lines) turned out to
have clean, unrelated explanations once isolated: ordinary cold-start
buffering in the first ~48s of a fresh `Player.Open`, and a one-time
`ActiveAE` clock-resync blip immediately after a large `Player.Seek`
jump (the same benign raw-clock-before-it-locks-on mechanism this
file's own "A consistent ~89.4s audio-sync-error reading" entry
already documents for stream *open*, evidently the same story for a
big seek) -- a second playback pass seeking straight into an
untouched section of the same file ran a continuous 78s with zero
stalls. No addon-side or Dispatcharr-side gap bug found. (2) Not
attempted -- this lab has no reverse proxy in front of Dispatcharr,
and standing one up (e.g. on the Kodi test VM itself, repointing the
addon's connection settings at it) was explicitly ruled out for now
rather than improvised mid-session. Still an open, real gap: a report
matching this exact symptom (reverse-proxied deployment, non-standard
port, DVR playback/logo failures) still needs checking against a real
reverse-proxy setup before ruling this addon's own code in or out.

**Live check (2026-09-30): the server side of the reverse-proxy half, simulated with request headers against the real 0.31.0 instance.** No real reverse proxy was set up; instead a one-second in-progress recording's `.../file/` redirect was requested with the headers a proxy would send. Dispatcharr builds the `Location` from the request, and gets it right whenever the proxy forwards where the client actually connected: `Host: name:8443` -> `http://name:8443/...`; `Host: name` plus `X-Forwarded-Port: 8443` -> `:8443`; `X-Forwarded-Host: name:8443` (with `X-Forwarded-Proto: https`) -> `https://name:8443/...`. With a `Host` that has no port and no `X-Forwarded-*` at all -- what a stock `proxy_set_header Host $host` sends -- it falls back to the port it is itself listening on, so the redirect points at the internal port, which a client behind the proxy can't reach. So the 0.31.0 fix covers a proxy that forwards the port, and the remaining failure is a proxy that doesn't. The addon follows that `Location` for an in-progress recording's playlist, so it would fail the same way; whether it should rebuild the playlist URL from the base URL it was configured with instead of trusting the redirect's authority is a design question (it also bears on "X-API-Key could follow a cross-host or downgrading redirect"). Moved to design decisions.

**Closed (2026-10-02): (1) was confirmed clean in 2026-09-21; (2) fixed on the addon side, live-confirmed.** The remaining failure was a proxy that forwards neither the port nor `X-Forwarded-*`: Dispatcharr then names its own internal port in the in-progress playlist's absolute segment URLs, which the addon
used as-is. `RebaseRecordingSegmentUrl()` (`M3u8SegmentParser.h`) now points every segment of the recording's own `/api/channels/recordings/<id>/hls/` path at the configured address and leaves any other URL alone, so a playlist that names an address of the server's own ends up at the configured one. (The sentence that used to stand here claimed the key could never be steered *away* from the configured server; a segment URL naming another host was passed through with the key attached, fixed 2026-10-04 -- see "The API key was attached to a segment URL on any host".) Live: with a proxy that does not rewrite
the playlist (so it names the server's real address, as it would an internal one), every segment HEAD and GET (31 in the run) went through the configured address, where before they bypassed it. The file redirect of a *completed* recording still takes the redirect's `Location` through the same-host policy and was not changed.

#### Guide fetch never includes already-aired programmes (catch-up)

**Confirmed from Dispatcharr's own current upstream source, not live:
this addon's guide fetch never includes already-aired programmes, so
catch-up only works for whatever Kodi happened to have cached before
the programme aired, flagged from a 19th-pass audit (2026-09-26).**
`GetXmlTvGuide()` fetches `/output/epg` with no `prev_days` query
parameter; Dispatcharr's own `apps/output/epg.py` defaults `prev_days`
to 0 for an unauthenticated/profile-less fetch (this addon's own,
confirmed via `TVSettingsAPIView`/the export view's own default) and
filters programmes to `end_time__gte=now` regardless. Practically:
right after a fresh install, an EPG database reset, or newly enabling
catch-up for a channel, any programme that aired before Kodi's own EPG
cache first picked it up is simply absent from every subsequent guide
fetch too -- `IsEPGTagPlayable()`'s own catch-up window can't reach a
tag that was never in the guide to begin with. Possible fix: append
`?prev_days=N` with `N = min(30, largest catchupDays among catch-up-
enabled channels)` -- the server itself caps at 30, confirmed against
its own source, so no addon-side clamp beyond that is even needed
beyond picking `N`, itself a small, pure computation. Needs a live
check first: how far back does Kodi's own guide-entry retention
actually reach in practice (would a much larger XMLTV response just
get truncated by Kodi's own guide-window pruning anyway), and what's
the real size/latency cost of a `prev_days=30` export against a
populated instance.

**Live check (2026-09-30): the cost is measured, and the benefit on this instance is small.** Against the real instance: the default `/output/epg` takes about 24 s. `?prev_days=3` about 30 s -- +25% size and +5 s. `?prev_days=30` returns byte-for-byte the same programmes, because the instance's EPG sources only hold about a day of past data (earliest programme: the previous evening); the server cap never comes into play. On the Kodi side, `epg.pastdaystodisplay` is 3 on the Windows client, so Kodi would keep what it is given. So `prev_days=N` would add roughly a day of already-aired programmes here for a quarter more download on a fetch that is already large; whether that is worth it depends on how much past data a deployment's sources keep, which the addon can't know in advance. Moved to design decisions.

**Live check (2026-10-02) and proposed decision.** Measured against the real instance (anonymous fetch, as the addon does): `prev_days=0` takes 24 s; `prev_days=1` is +25% in size, 30 s;
`prev_days=3`, `7` and `30` are all the same size as `prev_days=1`, i.e. the guide source only holds about two and a bit days of history, so asking for more buys nothing. Only a small share of the channels offer catch-up (all with 3 days), and the Kodi test profile shows 3 past days (Kodi's own default is 1). **Proposal:** send `prev_days=N` only when at least one channel offers catch-up, with
N = the largest `catchupDays` among catch-up-enabled channels (capped at the server's 30), so anyone who does not use catch-up pays nothing and anyone who does pays about a quarter more guide traffic once per `epg_refresh_hours`; the benefit is that a fresh install or EPG reset can catch up on the past ~2 days immediately instead of only what Kodi happened to cache. Awaiting your call.

**Fixed (2026-10-02), as proposed and approved.** `ComputeGuidePrevDays()` (`EpgTagUtil.h`): the guide fetch asks for `?prev_days=N`, N being the longest catch-up window among channels that actually offer catch-up (`ShouldOfferCatchup()`), capped at the server's 30, and sends no parameter at all when none does, so an install that never uses catch-up pays nothing. Live: the addon's request was `GET /output/epg?prev_days=3` (the size measured above) and Kodi then held already-aired broadcasts on catch-up channels. That Kodi's guide also held older entries from earlier fetches means the live run shows the request and the data arriving, not the fresh-install benefit itself, which needs an empty guide database to demonstrate.

#### Can't tell addon-managed rolling rules from user-created ones

**This addon has no way to tell an addon-managed rolling-window
recurring rule apart from one a user created directly via
Dispatcharr's own web UI, flagged from a 31st-pass audit
(2026-09-26), confirmed against Dispatcharr's own real current
upstream source, not itself independently reproduced -- needs a
design decision on how (or whether) to mark addon-owned rules, not a
blind fix.** `RenewRecurringRules()`/`ShouldRenewRecurringRule()` act
on every enabled rule `GetRecurringRules()` returns, with no signal
distinguishing the two: Dispatcharr's own web UI recurring-rule form
has an explicit End Date picker, and its serializer requires a
non-null `end_date` on every rule regardless of origin
(`RecurringRecordingRuleSerializer.validate()`,
`apps/channels/serializers.py`). A user creating a rule with, say, a
10-day `end_date` via the web UI, while any Kodi instance running this
addon happens to be running too, gets that end_date silently pushed
forward to `now + kRecurringRuleWindowDays` (30 days) once fewer than
half the window remains -- and again every ~15 days after that, for
as long as any such instance keeps running -- converting a
deliberately time-limited rule into one that quietly records forever,
using disk and provider streams the user never asked to commit to.
Each renewal PATCH also drops and regenerates every future occurrence
(`sync_recurring_rule_impl(drop_existing=True)`, confirmed against
Dispatcharr's own source), assigning new recording ids. One accidental
partial mitigation: a rule left at the web UI's own default end date
(today) parses to 00:00 UTC, which already reads as expired by the
time any renewal check would see it, so it's never touched. The
`RecurringRule` model itself has no free-form field this addon could
repurpose as an ownership marker (`apps/channels/models.py`: channel,
days, times, enabled, name, start/end date only) -- the realistic
options are a naming convention (fragile: a user could name a web-UI
rule anything), a locally-cached list of this addon's own created rule
ids (breaks across multiple Kodi installs sharing one Dispatcharr
instance, and doesn't survive `m_cachedRecurringRules` being rebuilt
from a fresh `GetRecurringRules()` after this addon restarts), or
simply never renewing a rule this specific addon instance didn't
itself just create in the current session (safest, but means a
restarted addon can no longer maintain its own rules created in a
previous session either, reopening finding below). None attempted
blind.
**Update (2026-09-26, a 36th-pass audit, confirmed against
Dispatcharr's own real current upstream source, not itself
independently reproduced): `ShouldRenewRecurringRule()`'s own
`rule.endDate <= now` expiry check has the same inclusive/exclusive
off-by-one `HasRecurringRuleEndDatePassed()` had before this same pass
fixed it for the *display* side (`GetTimers()`'s
`PVR_TIMER_STATE_ERROR`) -- deliberately left unfixed here, not missed.**
Dispatcharr's own `end_date` is inclusive (a real occurrence still airs
ON that calendar day, in Dispatcharr's own configured system timezone
-- `sync_recurring_rule_impl()`'s own day-generation loop,
`apps/channels/tasks.py`), so `ShouldRenewRecurringRule()` currently
refuses to renew a rule up to a full day (or more, depending on the
zone) before it's actually expired. This is exactly what makes the
"accidental partial mitigation" described above work at all: a web-UI
rule left at the default end date (today) reads as already-expired
the instant UTC clock reaches midnight of that day, which is *earlier*
than a correctly-inclusive check would consider it expired. Applying
the same `+86400 - offsetSeconds` fix here would close that
off-by-one, but would also remove this mitigation -- such a rule
would then read as having `daysLeft` comfortably within
`kRecurringRuleWindowDays/2`, and get renewed/extended to a full
30-day rolling rule the same as any other. Any future fix to
`ShouldRenewRecurringRule()`'s own boundary needs to resolve this
design question first (the same "which rules does this addon
actually own" decision this whole item is about), not be done as an
isolated correctness fix the way the display-side one was.

**Fixed (2026-10-02), live-confirmed -- option A, a name tag.** A recurring rule has no free-form field, so ownership is a visible `[Kodi]` at the end of its name (`ManagedRecurringRule.h`): a rule created from Kodi is stored as `Show name [Kodi]`, Kodi is shown the name without it, and a rename from
Kodi puts it back. `ShouldRenewRecurringRule()` now renews only a rule that carries it, so a rule made in Dispatcharr's web UI with a deliberate end date is never extended again. Removing the tag makes the addon stop managing a rule (fails safe); adding it by hand makes it manage one. Another Kodi install on the same account recognises the first one's rules,
and nothing but the addon and the user can add the tag. **Rules that predate ownership** are tagged exactly once (approved): on the first run of a build that knows about ownership, every untagged rule that has not already run its course is tagged (`EvaluateInitialAdoption()`; an enabled rule already past its end date is the "deliberately finished" case and is left alone; a rule with no end date is taken
and given one). It is done at once, with nothing tagged, for an account with no rules or one where any rule is already tagged (another install, or this one with its state file lost, got there first), so a rule made in the web UI later is never swept up. The state (`recurring_rule_adoption.json`) tracks rules still pending, because a rule with a recording running or about to start is left for a later cycle (a rule PATCH makes the server drop and regenerate its future
occurrences), and it survives a restart in the middle. Live, against the real instance: the real rule and a disposable one were tagged on first run and the state file ended `done`; a rule created afterwards from outside was left untouched while an owned rule 6 days from its end was renewed; Kodi showed every name without the tag; a rename through Kodi's own edit dialog kept the tag (`ZZZ_TEST_renamed [Kodi]`). **Side effect found live and handled:** a rule's occurrences take its
name as their title, so the tag leaked into every timer and recording it generated; `ParseRecordingFields()` now strips it from the title of a recording that belongs to a recurring rule (and only those). What it cannot reach is the server: the folder and file Dispatcharr writes for such a recording are named from the rule name, so they carry `[Kodi]`. Rule creation from Kodi's own Add timer dialog (which adds the tag) was not driven live.

#### Rolling recurring rules die if no Kodi runs for ~15-30 days

**The flip side of the same root cause: this addon's own rolling
recurring rules stop renewing (and eventually die) for good if no
Kodi instance running this addon runs for roughly 15-30 days, flagged
from a 31st-pass audit (2026-09-26), confirmed against Dispatcharr's
own real current upstream source, not itself independently
reproduced.** `ShouldRenewRecurringRule()`'s own pass-19 "never
resurrect an expired rule" fix is deliberately permanent once
`end_date` has passed -- correct in isolation (see "Can't tell addon-managed rolling rules from user-created ones" for why it can't safely distinguish "this addon's own rule, safe to keep
renewing" from "a user's deliberately time-limited web-UI rule"), but
means a rule that legitimately is this addon's own dies the same way
if renewal ever lapses for too long: a seasonal box, a long trip, the
addon disabled or the Kodi install itself down for longer than the
window remaining since the rule's last renewal. Dispatcharr's own
scheduler then generates no further occurrences past that `end_date`
regardless of the rule's own `enabled` flag -- confirmed against its
real current upstream source (`apps/channels/tasks.py`'s own
`target_date > end_limit` cutoff) -- so the rule silently stops
recording anything at all. `GetTimers()` now shows this state as
`PVR_TIMER_STATE_ERROR` rather than a healthy `PVR_TIMER_STATE_SCHEDULED`
(fixed this same pass, `dispatcharr::HasRecurringRuleEndDatePassed()`
in `RecurringRuleRenewal.h`), so the user at least has a visible signal
something needs attention -- but recovering still requires a manual
disable-then-re-enable of the rule in Kodi (the only path that pushes
`end_date` forward again, via `ShouldExtendRecurringRuleEndDateOnUpdate()`),
which nothing currently prompts the user to do. `docs/RECURRING_RULES.md`'s
own "create once, forget about it" framing has been given a follow-up
note pointing here rather than rewritten, since it's still accurate
for the common case (an addon instance that keeps running).

**Fixed (2026-10-02), live-confirmed, with #1.** An owned (`[Kodi]`-tagged), enabled rule whose end date has passed -- or that has none -- is revived by the next renewal cycle with a new window (`ShouldRenewRecurringRule()`; the occurrence-safety check still applies, and a disabled rule is left alone), instead of staying dead. Renewal runs every `recording_refresh_minutes`, so this happens within minutes of Kodi starting. Live: an owned rule moved to two days past its end date was revived to
a 30-day window within one cycle, with the addon logging that it was one of its own; an unowned rule near its end was left alone. Missed occurrences while no Kodi ran are still missed; only the rule comes back. A user who wants to stop an owned rule disables or deletes it (ending it by lowering its end date in the web UI would be undone at the next cycle); removing the tag also works.

#### Ranged GET at a non-zero offset accepts a plain HTTP 200

**Known gap: `ReadRecordingStream()`/`ReadLiveTimeshiftStream()` both
accept a plain HTTP 200 response to a ranged GET at a non-zero
offset, flagged from a project-wide review (2026-09-26), not yet
reproduced live.** Both check `httpCode != 200 && httpCode != 206`
(`DispatcharrClient.cpp`, ~line 2217 and ~line 2786) -- a 200 means
the server ignored the `Range` header and sent the file from byte 0,
but both readers still advance their own `position` as if the
returned bytes started at the requested offset, which would silently
splice the wrong bytes into playback. `ReadLiveTimeshiftStream()`'s
own `Content-Range`-based size cross-check (`serverReportedTotal`)
can't catch this either, since a 200 response carries no
`Content-Range` header at all, so the check is silently skipped
rather than firing. Purely defensive today -- both Dispatcharr itself
and the timeshift_buffer plugin's own file server are confirmed to
always honor a well-formed `bytes=X-Y` request with a real 206 -- so
not changed without first confirming live that a real response can
actually take this path (and, if so, whether treating a 200 as a hard
read error is safe, e.g. for a proxy/server that doesn't support
Range at all).

**Live check (2026-09-30): Dispatcharr's own file endpoint honours ranges.** `GET /api/channels/recordings/{id}/file/` with `Range: bytes=0-99` and `bytes=100000-100099` on three completed recordings returned **206** with a correct `Content-Range` and `Accept-Ranges: bytes` every time. (The timeshift plugin's file server was already known to answer a Range GET with 206.) So neither real server takes the 200 path; only a proxy in front that drops `Range` could, which nothing here has seen. Left as a known, deliberately deferred gap: turning a 200 at a non-zero offset into an error would, for a server that never honours ranges, just replace silent corruption with a retry storm (Kodi retries a `-1` read near-immediately), so the right handling -- skipping the body up to the offset, or failing the stream outright -- needs a real case to design against.

**Fixed (2026-10-02), live-confirmed.** `ServerIgnoredRangeRequest()` (`RecordingHttpUtil.h`): a plain 200 to a ranged read that began past offset 0 means the server ignored `Range` and sent the file from its first byte. Both readers used to advance their position by however many bytes arrived, splicing the wrong data into playback. The decision the earlier entry left open: neither retrying (a `-1` is retried
by Kodi near-immediately -- a storm) nor skipping to the offset (that downloads everything before it on every read) is right, so `ReadRecordingStream()` ends the stream (EOF) once, logs it, and shows a notification ("the server (or a proxy in front of it) doesn't support partial downloads"), with every later read returning EOF without another request; `ReadLiveTimeshiftStream()` marks the stream fatal. A 200 at offset 0 is still accepted (the bytes
really do start there). Live, with a proxy that answers every ranged read with a 200 and no `Content-Range`: normal playback of a completed recording was unaffected, and with the proxy lying the player stopped at the first non-zero read with the logged error and notification. The in-progress path fetches whole segments and has no ranged read.

#### timeshift_buffer trusts a Redis-stored pid

**`timeshift_buffer` trusts a Redis-stored pid without verifying it's
still the same ffmpeg process it started, flagged from a project-wide
review (2026-09-26), needs a live check first.** `_stop_ffmpeg`/
`_is_process_alive` act on whatever pid is in Redis state with no
verification that it hasn't since been reused by an unrelated process
(pid wraparound under heavy buffer churn is already confirmed live per
`docs/TIMESHIFT.md`). A resurrected/stale state (from the Redis race
above, or state surviving a Dispatcharr container restart -- unclear
whether Redis itself survives that restart, needs a live check) could
get an unrelated process group SIGTERM'd/SIGKILL'd, or let
`start_buffer` "reattach" to a buffer that no longer exists. A
possible fix: record `/proc/<pid>/stat`'s own starttime field (22) at
spawn and verify it matches before signalling -- not implemented
without first confirming live whether Redis state actually survives a
container restart, and whether pid reuse under normal (not just
heavy-churn) operation is a real practical risk here.
**Update (2026-09-26, an 18th-pass audit): the standard deployment
makes this more plausible than "unclear", not less -- still not
implemented, still needs a live check on the specific failure rate,
not the premise.** Confirmed against Dispatcharr's own real current
upstream source (`docker/docker-compose.yml`, cloned into a
scratchpad, never committed to this repo -- a real source read, not a
live test): the standard multi-container deployment runs Redis as its
own separate container (`redis-server --timeout 300`, no persistent
volume shown for its own data directory), independent of the
Dispatcharr app container. An ordinary app-container-only restart
(a code deploy, an app crash-and-restart -- far more common than a
full-stack restart) leaves Redis, and therefore every buffer's own
cached pid, completely untouched, while the app container's own pid
namespace resets from scratch -- low pid numbers get reused quickly in
a small, freshly-started container, so a stale cached pid pointing at
a since-exited ffmpeg process could plausibly now belong to a
genuinely different process (even the app's own Django/gunicorn
worker) well within `_BUFFER_STATE_TTL` (600s). The single-container
"all-in-one" image's own Redis persistence is a separate, still-open
question (depends on its own default RDB snapshotting behavior and
working directory, not confirmed either way this pass).
**Resolved for the single-container case (2026-09-29), without needing
a live restart at all: confirmed non-persistent.** The real lab
instance runs the single-container "all-in-one" image --
checking that container's own mapped volumes directly (no restart
needed) confirmed Redis's own data directory is NOT mapped to
persistent storage. Restarting this specific kind of deployment is
inherently a full-stack restart (there's no separate Redis container
to leave running) -- and since Redis isn't persistent, a restart wipes
its state *entirely*, not partially. That's actually the reassuring
outcome, not the alarming one: the specific danger this item describes
(a stale-but-present Redis entry surviving to point at a pid the
restart's own fresh process table has since reused) can't happen from
a restart here, since there's no leftover entry left to be wrong about
-- `start_buffer`'s existing "no Redis state for this channel -> start
fresh" path already handles that correctly, for every buffer, on every
restart. Narrows what's actually still open: the pid-reuse risk this
entry describes remains real, but only from ordinary pid recycling
during a single long *uninterrupted* run (already confirmed live,
`docs/TIMESHIFT.md`, unrelated to a restart at all) -- not from
anything a restart itself introduces, at least for this deployment
shape. A multi-container deployment (Redis in its own, separately-
persisted or separately-restarted container) could still hit the
original worry -- not this instance's own topology, and still not
tested live either way.

**Reviewed (2026-09-30): nothing further can be checked from here.** What is left is the multi-container deployment (Redis in its own container that outlives an app restart), which the lab isn't; the single-container case is resolved above. Moved to deferred gaps, to be picked up if a multi-container report appears or a test setup becomes available.

**Fixed (2026-10-02) in `timeshift_buffer` 0.6.9, verified against real `/proc` and the kernel's own process table, not yet on the deployed instance.** The buffer state now records the ffmpeg's start time (`/proc/<pid>/stat` field 22, `pid_start_ticks`), which a pid reused by an unrelated process cannot share. `_stop_ffmpeg()` refuses to signal a pid whose start time has changed (it would SIGTERM then SIGKILL an unrelated process group) and `_is_process_alive()` reports such a pid
as dead without reaping it; state written by an older version, or on a host with no readable `/proc`, behaves as before. All three call sites (`start_buffer`'s dead-buffer check, `get_live_manifest`) pass the recorded value. Checked against a real running process: the recorded ticks are plausible against the boot time, a wrong value makes `_is_process_alive()` false and `_stop_ffmpeg()` leave the process running, the right value stops it, and a different
process has a different value. Unit tests (11) fail with the mismatch checks removed. Multi-container deployments, where Redis outlives an app restart, were the case this protects; the single-container case was already shown not to keep state across a restart.

#### Cache-invalidation triggers can be overwritten by an in-flight fetch

**`PVRDispatcharr.cpp`'s cache-invalidation triggers can be overwritten
by a fetch already in flight, flagged from a project-wide review
(2026-09-26), not reproduced live.** `InvalidateAndTriggerRecordingUpdate()`/
`InvalidateAndTriggerTimerUpdate()` reset `m_recordingsCachedAt`/
`m_timerRulesCachedAt` to force a fresh fetch, but `EnsureRecordingsLoaded()`/
`EnsureTimerRulesLoaded()` capture `now` *before* their own network
call and only write `...CachedAt = now` after it returns. If an
invalidation lands while a fetch is already in flight (started before
the invalidation, still running when it happens), that fetch's own
stale result still gets committed as fresh once it finishes,
overwriting the invalidation. Kodi's own re-poll inside the 2s TTL
would then see the pre-change data. `UpdateTimer()`/`DeleteTimer()`
have no delayed re-trigger the way `AddTimer()` does (see its own
`TrackDetachedThread()` call), so an edit/delete landing in that exact
window could stay stale until the next 5-minute-default background
refresh, not just a few seconds. A real fix needs a generation counter
(bump on invalidate, only commit a fetch's result if the generation
hasn't changed since it started) -- bigger than this pass's other
fixes given how many call sites touch these four caches, not attempted
blind.

**Update (2026-09-26, a 24th-pass audit): the same race applies to
`m_epgLoadedAt` too, now that a 23rd-pass fix added a second writer
for it.** `EnsureChannelsLoaded()`'s own `HaveChannelNumbersChanged()`
check (`ChannelRenumbering.h`) resets `m_epgLoadedAt` to force a
re-fetch after a channel renumbering -- but a concurrent
`EnsureEpgLoaded()` call that started *before* that reset and finishes
*after* it can still commit its own pre-renumber XMLTV parse as fresh,
overwriting the reset the exact same way described above. Needs a
specific timing window (a renumbering landing mid-fetch), not
reproduced live -- the same generation-counter fix sketched above
would cover this case too, not a separate mechanism.

**Update (2026-09-27, a 41st-pass audit): `m_timerRulesCachedAt` no
longer exists.** A 41st-pass fix split it into two independent
timestamps, `m_seriesRulesCachedAt`/`m_recurringRulesCachedAt` (see
`CLAUDE.md`'s own `Staleness`/`GetTimersAmount()` entry for why). This
open race applies identically to both of the new timestamps -- nothing
about the fix touched the fetch-then-commit ordering this entry is
about -- so the race itself is still open, just against a slightly
different pair of member names than when this entry was written.

**Fixed (2026-10-02), unit-tested at the helper, not exercised live (the window is a fetch already running when an edit lands).** The generation counter the entry sketched: each of the recordings, series-rules, recurring-rules and guide caches has one (`m_recordingsGeneration` etc.), bumped by `InvalidateAndTriggerRecordingUpdate()`/`InvalidateAndTriggerTimerUpdate()` and by the channel-renumbering reset. A fetch records the generation when it starts and
`dispatcharr::IsFetchStillCurrent()` (`Staleness.h`) decides at commit whether it may mark the cache fresh: for recordings and rules the data is kept but the cache is left stale, so the very next call fetches again; for the guide a fetch that straddled a renumbering is discarded (its parse is keyed by the old channel numbers) and not stamped as failed, so the next cycle fetches straight away. The guide's generation is read after the forced
channel refresh, so a renumbering that refresh itself finds does not discard the guide fetched after it.

#### Kodi shutdown / NEED_RESTART teardown can block for minutes

**Kodi shutdown / an `ADDON_STATUS_NEED_RESTART` teardown can block for
minutes, flagged from a project-wide review (2026-09-26), not
reproduced live.** The recording-refresh thread holds
`m_recordingRefreshMutex` for its *entire* loop body, including
`RenewRecurringRules()`'s own network calls -- `StopWorkerThread()`'s
`lock_guard` in the destructor can't set the stop flag until that
finishes. None of the curl calls anywhere in this addon are abortable
early (no `CURLOPT_TIMEOUT` short enough to matter mid-shutdown), so a
slow/unresponsive Dispatcharr instance could make a normal Kodi exit
or addon restart hang for up to several multiples of the configured
`timeout` setting (default 30s, user-configurable up to much higher).
A real fix needs either a genuinely interruptible HTTP layer (a
`CURLOPT_XFERINFOFUNCTION` progress callback checking the stop flag)
or restructuring the refresh thread to check the stop flag between
each network call rather than holding one lock across all of them --
a larger change than this pass's other fixes, not attempted blind.

**Fixed (2026-10-02), live-confirmed.** Two changes. Every request now carries a libcurl transfer-progress callback (`TransferAbortResult()`, `CurlCallbacks.h`) that ends it at once when `DispatcharrClient::AbortInFlightRequests()` has been called, which the destructor does first -- libcurl calls it about once a second even when no data moves, which is what makes a request stuck on an unresponsive server abortable. And the recording-refresh thread no longer holds its mutex across the refresh work
(only while waiting), so the destructor can set the stop flag immediately. Live, with a request to Dispatcharr made to hang while Kodi was told to quit: the old build took 27.1 s to exit (the rest of the 30 s request timeout; longer if the user has raised `timeout`), the new one 2.1 s.

#### notify_all() without holding the condition variable's mutex

**`OnSystemWake()`/`OnAddonSettingChanged()`'s `recording_refresh_minutes`
branch both call their own condition variable's `notify_all()` without
holding that cv's mutex first, flagged from a project-wide review
(2026-09-26), not reproduced live -- a known, textbook condition-variable
pitfall (a notify with no lock held can race a waiter that's checked
its predicate but not yet actually entered the wait, missing the
wakeup) rather than something specific to this codebase.** Bounded,
non-destructive impact either way: a missed wakeup here just means
waiting out the normal periodic timeout (the realtime-update thread's
own backoff, up to 60s, or up to `recording_refresh_minutes` for the
other) instead of reacting immediately -- not a lost update, just a
delayed one. Both call sites would need the same fix (acquire the
matching mutex before setting the flag and notifying), so this is
logged as one item covering both rather than fixing only one in
isolation.
**Caveat (2026-09-27, a 62nd-pass audit, confirmed by code trace, not
itself independently reproduced): the naive version of that fix is
actually unsafe for the recording-refresh half specifically.**
`StartRecordingRefreshThread()`'s own `std::unique_lock<std::mutex>
lock(m_recordingRefreshMutex)` (`PVRDispatcharr.cpp`) is held for the
*entire* loop body, not just the wait -- including `RenewRecurringRules()`,
which makes several blocking HTTP calls (`GetRecurringRules()`,
`GetRecordings()`, one `ExtendRecurringRuleEndDate()` per rule), each
up to `timeoutSeconds` (30s default, up to 180s per `settings.xml`).
`OnAddonSettingChanged()` runs on Kodi's own settings thread while
`CAddonDispatcharr::SetSetting()` (`addon.cpp`) already holds
`m_instancesMutex` -- so naively taking `m_recordingRefreshMutex`
there too, right before `notify_all()`, could block that settings save
(and, transitively, every other addon instance's own settings
delivery serialized behind the same `m_instancesMutex`) for however
long the refresh thread's own in-flight HTTP work takes, whenever the
two happen to overlap. The realtime-update thread's own equivalent
mutex is only ever held briefly around the flag/notify itself, so the
proposed fix is fine for that half unmodified. For the recording-refresh
half specifically, a correct fix needs narrowing that lock to cover
only the wait (releasing it around `RenewRecurringRules()`'s own work)
-- **corrected 2026-09-27, a 63rd-pass audit, fixing a real, confirmed
logical error in this same caveat's own prior wording, found via a
project-wide review, confirmed by code trace, not itself independently
reproduced: an "empty lock/unlock scope right before `notify_all()`"
is NOT a safe alternative to narrowing the lock, contrary to what this
caveat originally said.** Acquiring the mutex at all -- even for an
empty scope with nothing inside it -- is itself the blocking operation
in question: `std::mutex::lock()` doesn't return until whoever
currently holds it releases it, and the refresh thread only releases
`m_recordingRefreshMutex` from inside `wait_for()`, not while
`RenewRecurringRules()`'s own blocking HTTP calls are running. So an
empty lock/unlock scope taken from `OnAddonSettingChanged()` would
block for exactly as long as the naive fix this whole caveat exists to
warn against -- it doesn't avoid that wait at all, it just moves where
the wait happens. Narrowing the lock (or setting the flag under it and
notifying, but only once it's already scoped to just the wait) is the
only correct direction for this half.

**Fixed (2026-10-02) together with the shutdown item above, which is what made it safe.** The caveat recorded here was that taking the recording-refresh mutex before notifying could stall Kodi's settings thread, because the refresh thread held it across network calls. It no longer does, so `OnAddonSettingChanged()` (the `recording_refresh_minutes` branch) and `OnSystemWake()` now set their flag under the mutex the waiter checks its predicate under before notifying, closing the lost-wakeup window.

#### Lower-severity WebSocketClient.cpp gaps

**A handful of lower-severity `WebSocketClient.cpp` gaps, flagged from
a project-wide review (2026-09-26), none reproduced live -- logged
together rather than fixed piecemeal, since each needs its own
judgment call about a real socket-handling change on the untested side
of this project's Kodi-API/HTTP/socket boundary:**
- `WaitForSocketReady()`'s `FD_SET(sockfd, &fds)` has no `sockfd <
  FD_SETSIZE` guard -- undefined behavior (or a glibc `_FORTIFY_SOURCE`
  abort) if the process already has 1024+ file descriptors open when
  this connects. Only plausible on an already fd-exhausted, long-running
  Kodi process; `poll()` avoids the limit entirely but is a real
  behavioral change to verify across platforms (this file already has
  a `#ifdef _WIN32` branch). **Refinement (2026-09-27, a 61st-pass
  audit, not itself independently reproduced): a plain `sockfd <
  FD_SETSIZE` guard alone wouldn't catch `sockfd == CURL_SOCKET_BAD`
  (-1)** -- both call sites (`SendAll()`/`FillBuffer()`) discard
  `curl_easy_getinfo(CURLINFO_ACTIVESOCKET)`'s own return value, so if
  that call ever genuinely failed to report an active socket (both
  sites do initialize `sockfd` to `CURL_SOCKET_BAD` first, so this
  isn't a stale/garbage value, just the sentinel itself reaching
  `FD_SET` unchanged), `FD_SET(-1, ...)` is its own separate instance
  of the identical undefined-behavior class on POSIX. A future fix here
  should guard `sockfd != CURL_SOCKET_BAD` too, not just the upper
  bound.
- `FillBuffer()`'s own read deadline restarts on every chunk received,
  so a peer trickling data in slowly enough could keep
  `ReceiveTextMessage()` running well past its nominal budget --
  **the same shutdown-hang consequence the handshake-loop bullet just
  below documents** (noted 2026-09-27, a 60th-pass audit, not itself
  independently reproduced): a declared frame up to `kMaxFramePayload`
  (10 MB) read one trickled chunk at a time gets a fresh multi-second
  window per `FillBuffer()` call here too, and this loop also never
  returns to check `shouldStop()` while a continuous stream of
  ping/pong/binary frames keeps arriving -- `ReceiveTextMessage()` runs
  on the same realtime-update thread `StopWorkerThread()` joins, so
  the same class of stuck-shutdown risk applies here, just via a
  continuously-fed connection rather than a stalled handshake. Only a
  misbehaving peer or proxy can trigger either, so this stays one
  combined bullet rather than a separate item.
- **RESOLVED (2026-09-27, both halves now fixed -- see below; kept in
  this list rather than removed, matching this entry's own established
  append-only convention).** The handshake response header loop in
  `Connect()` had no cap on
  total header size, re-copying the accumulated buffer every
  iteration. The size half
  was fixed a 58th-pass audit (2026-09-27): a 16 KiB cap now fails the
  connection outright rather than growing the buffer without limit.
  The time half -- confirmed by a 59th-pass audit's own re-check
  (2026-09-27, not itself independently reproduced) that the size fix
  alone bounds memory, not wall-clock time, since each `FillBuffer()`
  call inside the loop got its own fresh `connectTimeoutSeconds`
  deadline (default 30s) rather than the loop having one overall
  deadline, so a peer trickling data in just under that timeout per
  chunk, with the `\r\n\r\n` terminator never actually sent, could keep
  this loop running for roughly (16384 bytes / 1 byte per chunk) x
  30s -- many days -- before the size cap ever triggered -- **was
  fixed the same, 59th, pass** (corrected 2026-09-27, a 60th-pass
  audit, fixing a real, confirmed self-contradiction found via a
  project-wide review: this bullet's own prior wording described the
  fix and then said "not implemented" in the very same commit that
  implemented it): one overall handshake deadline is now computed
  before the loop starts, with whatever time actually remains (via
  `std::chrono::ceil<std::chrono::seconds>`, not a fresh
  `connectTimeoutSeconds`) passed to each `FillBuffer()` call instead,
  failing outright once that deadline passes. Doesn't cover the
  initial `SendAll()` handshake-request send or the underlying curl
  connect/TLS handshake before this loop even starts (each has its own
  separate `connectTimeoutSeconds` budget already) -- so the whole of
  `Connect()`'s own worst case is now bounded at roughly 3x
  `connectTimeoutSeconds` plus one second, not unbounded, but still not
  a single tight deadline covering the entire function. Tightening
  that further (one deadline covering `SendAll()` too) is a design
  decision, not pursued.
- Incoming frames aren't validated against two RFC 6455 rules this
  client assumes rather than checks: a control frame must be ≤125
  bytes with FIN set, and the server must never mask a frame it sends.
  A ping longer than 65535 bytes would also produce a malformed pong,
  since `BuildMaskedControlFrame()` (`WebSocketFrame.cpp`) only encodes
  a 16-bit extended length -- moot in practice today since Dispatcharr
  itself is the only real peer and its own pings stay within the
  spec's 125-byte cap, but worth knowing if that ever changes.

**Fixed (2026-10-02) except what is noted, unit-tested against a real local WebSocket server and confirmed live.** (1) The descriptor wait is now `WaitForSocketReady()` (`SocketWait.h`): `poll()` on POSIX, which has no 1024-descriptor limit, and an invalid socket (`CURL_SOCKET_BAD`, which curl reports when it has no active socket, and which `FD_SET` hit just as badly) is refused up front; Windows keeps `select()`, whose set counts sockets, not values.
A test makes the failure concrete: a descriptor numbered 1500 is waited on correctly. (2) The read deadline restarting on every chunk: reads now take an absolute deadline. `ReceiveTextMessage()` has one for the whole call (so a flood of ping/pong/binary frames cannot keep it running past `timeoutSeconds` -- it returns 0 at a frame boundary, the same safe place a header timeout does) and one for each frame once its header has arrived (a frame dripped in a byte at a time
now fails "mid-frame" within `timeoutSeconds`; a test with a server dripping 60 bytes at 300 ms intervals took 18 s to fail before and ~1 s after). (3) The RFC 6455 rules the client assumed: `IsValidServerFrame()` (`WebSocketFrame.h`) refuses a masked server frame and a control frame that is over 125 bytes or fragmented, as a protocol error that reconnects; that also keeps a pong within `BuildMaskedControlFrame()`'s
16-bit length. The handshake-loop half was already fixed. Live, with the new build and realtime updates on: a recording created straight against Dispatcharr appeared in Kodi's timers 2.9 s later, so the connect, ping/pong-free read path and message delivery all work. The connect still has separate budgets for the TCP/TLS connect, the request send and the response read (worst case about three times `timeout`), unchanged.

#### Two small settings-notification loose ends

**Two small settings-notification loose ends, flagged from a 23rd-pass
audit (2026-09-26), both low severity, not reproduced live.** (1)
**RESOLVED as of a 49th-pass audit (2026-09-27), confirmed stale by a
62nd-pass audit's own re-check (2026-09-27, not itself independently
reproduced).** Originally: changing `recurring_rule_utc_offset_minutes`/
`recurring_rule_timezone` didn't trigger a Timers-list update, so an
edited offset's effect on displayed recurring-rule times wasn't
visible until the next periodic `recording_refresh_minutes` refresh --
described here as "purely a display lag, not a scheduling error". That
second half was itself wrong even at the time, not just later fixed:
`docs/RECURRING_RULES.md`'s own 47th/48th-pass entries already document
that a stale cached display time round-tripped through an edit shifts
the rule by the full old/new offset difference -- a real scheduling
error, not merely cosmetic. The 49th-pass audit's
`OnAddonSettingChanged()` fix (confirmed against the current source:
both the `recurring_rule_utc_offset_minutes` and `recurring_rule_timezone`
branches call `InvalidateAndTriggerTimerUpdate()` on a genuine value
change) closes the visibility gap this entry originally described. (2)
`Config::debugLogging` is set from settings but never actually read
anywhere -- `m_debugLogging` (a separate member) is what every
`if (m_debugLogging)` check in this codebase actually uses, so this
field looks like dead/vestigial state, not a real gap; worth a closer
look before removing it outright (grepping `docs/`/`CHANGELOG.md`
first, per this project's own removal-safety convention) rather than
assuming it's safe to delete blind -- still open, unaffected by (1)'s
resolution.

**Fixed (2026-10-02).** (1) was already resolved. (2) `Config::debugLogging` was set from the setting and never read anywhere -- `m_debugLogging` is what every check uses -- so the field and its assignment are removed (`docs/` and `CHANGELOG.md` cited neither, per the removal-safety convention).

#### check_doc_refs: heading-match check may be too loose

**Suspected: `tools/check_doc_refs.py`'s heading-match check is too
loose, flagged from a project-wide review (2026-09-26), not yet
reproduced against a real false-negative.** Its substring check
(`normalized_title in h or h in normalized_title`) matches in *either*
direction, so a short real heading (several exist in `docs/TIMESHIFT.md`
and elsewhere, e.g. one-or-two-word headings) can satisfy a citation of
a completely different, longer section whose title happens to contain
it as a substring -- the checker would then miss a genuinely dangling
citation. Needs a real audit of current citations against this before
deciding whether to tighten the match (e.g. requiring a minimum
heading length, or a word-boundary match) and re-running
`--update-baseline`, since tightening it may surface previously-hidden
stale citations that need fixing, not just a checker change.
**Audit done (2026-09-29): zero real citations currently rely on the
loose direction -- nothing hidden right now, safe to tighten later but
not urgent.** Wrote a one-off scratch script (not committed) reusing
`check_doc_refs.py`'s own real extraction/heading logic (imported the
module directly, not reimplemented) to re-check every real
`"..."`-section citation across every real doc against BOTH match
directions separately: whether the citation itself is a substring of
a real heading (the legitimate, documented case -- `"Concurrent
viewers"` citing a real, longer `"## Concurrent viewers (a real,
live-confirmed bug)"` heading), versus whether it only passes because
some short real heading happens to be a substring *of* the citation
(the suspected loophole). Every currently-passing citation in this
repo's real docs matched via the first, legitimate direction --
none currently depend on the second, loophole direction at all.
Sanity-checked the audit script itself against a synthetic loophole
case first (a fabricated long citation containing a short fabricated
heading as a substring, matching only the loophole direction) to
confirm it would actually catch a real instance if one existed, not
just trivially return zero. Since nothing is currently hidden,
tightening the match now would be a pure hardening move with no
citations to actually go fix alongside it -- not pursued this pass
given the low payoff, but the audit itself is done and this entry no
longer needs re-verifying from scratch if tightening is ever
revisited.

**Fixed (2026-10-02).** The checker's heading-matching function now accepts a citation only when it is a substring of a real heading, not also when a real heading is a substring of the citation. The 2026-09-29 audit had found no real citation relying on the loose direction, and running the checker afterwards confirmed nothing new surfaced. The test that documented the weakness now asserts the loophole is closed.

#### Dead-buffer detection destroys a paused/rewound viewer's rewind window

**Pass 10's broadened live-timeshift dead-buffer detection can destroy
a paused/rewound viewer's entire buffered rewind window the instant
ffmpeg dies, not just once that viewer's own read reaches the tail --
flagged from a 13th-pass audit (2026-09-26), a real, non-trivial design
gap, not fixed this pass.** Two separate problems compound:
- **Plugin side:** `_get_live_manifest()` raises `BufferFailedError`
  whenever the tracked ffmpeg pid is dead, *even if a playlist with
  real, already-served content already exists* (this is exactly what
  pass 10 added, to stop a viewer sitting at the live edge from
  looping forever against a buffer that will never grow again).
  `_get_live_manifest_action()`'s handler for that error immediately
  calls `_teardown_buffer()` -- `rmtree`s every segment file and
  deletes the Redis state -- as a synchronous self-heal. `GetStreamTimes()`
  keeps polling `get_live_manifest` while a stream is paused (see the "Paused live-TV viewer's heartbeat goes stale" entry), so the *first* poll
  after ffmpeg dies (an upstream drop, no `-reconnect` flag set on
  ffmpeg) wipes the whole rewind buffer out from under a viewer who is
  behind live and still has plenty of already-buffered content worth
  serving -- not just the genuinely-exhausted tail case pass 10 was
  written for.
- **Addon side:** `GetLiveTimeshiftStreamLength()`/
  `GetLiveTimeshiftStreamDurationMs()`/`SeekLiveTimeshiftStream()`'s own
  SEEK_END refresh all call `RefreshLiveManifest()` with no `fatalOut`
  at all -- only `ReadLiveTimeshiftStream()`'s own catch-up loop and its
  Content-Range mismatch check ever set `m_liveTimeshiftStream.fatal`.
  `docs/TIMESHIFT.md` already names Seek/Length as missing this, but
  completing it naively is **not safe**: `ReadLiveTimeshiftStream()`'s
  own top-of-function check (`if (m_liveTimeshiftStream.fatal) return
  -1;`) is unconditional, regardless of whether the requested position
  is at the tail or far behind live -- so if `GetStreamTimes()` set
  `fatal` the instant it saw the plugin's own `fatal: true` (which,
  per the plugin-side problem above, can now happen while content is
  still genuinely readable), every future read of *already-known,
  still-valid* segments would also start failing immediately. That
  would just move the same "destroy a paused/rewound viewer's
  playback" problem from the plugin side to the addon side instead of
  fixing it.
- **Real fix needs a design change on both sides, not a quick patch:**
  the plugin should distinguish "ffmpeg is dead, so no more new
  content is coming" from "there is nothing left worth serving" --
  e.g. still return the frozen manifest (with a flag indicating no
  further growth is possible) instead of raising `BufferFailedError`
  immediately when a playlist already exists, leaving actual teardown
  to `stop_buffer`/the idle reaper (whose own timeout still fires
  within its normal ~45s worst case regardless, since a dead buffer's
  own fetches can no longer succeed to keep refreshing
  `last_heartbeat`). The addon side then needs `fatal` (or an
  equivalent "no more growth" signal) to only block a read that's
  genuinely caught up to the tail, not one still reading behind it --
  which likely means separating "no more growth" from the outright
  "stop reading anything" meaning `fatal` currently has. Needs its own
  dedicated pass with live verification (pause/rewind a live-timeshift
  session, kill ffmpeg server-side, confirm buffered content still
  plays before the tail is reached) rather than a blind change.

**Fixed (2026-10-02) in `timeshift_buffer` 0.7.0 and the addon, with before/after measured live.** The design the entry laid out, both sides. **Plugin:** `_get_live_manifest()` no longer raises `BufferFailedError` for a dead ffmpeg when the playlist already exists -- that raise is what made the handler tear the whole buffer down at the first poll, and a paused client keeps polling. It returns the frozen manifest with `ended: true` (the never-produced-anything case still fails fast, unchanged), and teardown is left to
`stop_buffer` and the idle reaper, which fire once nobody is polling. **Addon:** `RefreshLiveManifest()` records `ended`, and `IsAtEndedTail()` (`LiveEdgeMargin.h`) ends a read only when ffmpeg has exited *and* the reader has consumed everything that was ever going to exist -- a reader still behind the tail is unaffected, which is the point. The end is EOF (a read returning 0), not `-1`: the first attempt returned `-1`, which Kodi retried about five times a second
without ever stopping playback (the same finding the in-progress-recording path recorded), and the log line is written once. **Live** (server-side timeshift, a real channel, the upstream stopped through Dispatcharr's own channel-stop endpoint while the viewer was paused ~45 s behind live): with 0.6.9, 25 s after ffmpeg died the buffer was gone ("no buffer running for this channel") and playback ended 11 s after resume -- only Kodi's own read-ahead. With 0.7.0 the buffer was
still there at that point (50 segments, `ended: true`), resume played about 72 s of the buffered content, and then the player ended cleanly by itself. A viewer at the live edge when ffmpeg dies now also ends by itself, within 14 s, instead of idling forever on `-1`. A second viewer starting the same channel still replaces a dead buffer, as before. The seek step buttons did not move the position in these runs (they landed a few seconds later, not
back), which is why the scenario used pause instead; that is a separate observation, not examined here.

#### Guide cache serves pre-rename channel numbers after a manual renumber

**Split out (2026-10-02) from "Residual gaps in the forced channel refresh before the EPG fetch", half (b), confirmed live 2026-09-30.** Dispatcharr's own 300-second XMLTV chunk cache is not invalidated by a manual channel-number edit, so a guide fetch within five minutes of one
is committed against the new channel list but keyed by the old numbers, for a full `epg_refresh_hours`. Three options, none chosen: re-fetch a few minutes after a detected renumber; cross-check each channel's name against the XMLTV `<display-name>` (which `XmlTvParser` does not read) and skip or re-fetch on a mismatch (this would also catch the duplicate-number collision case); or accept
the roughly 2% exposure. Needs your call, and the display-name cross-check needs a live look at how reliable that field is.

**Fixed (2026-10-02), confirmed live; the display-name cross-check and accepting the exposure were not chosen.** A detected channel renumbering now also schedules one more guide fetch `kGuideRefetchAfterRenumber` (330 s: the server's 300 s cache plus a margin) after the detection (`m_epgRefetchDueAt`, `IsGuideRefetchPending()`/`IsGuideRefetchDue()` in `Staleness.h`). `EnsureEpgLoaded()` treats the guide as due once that time passes, the background loop wakes at the one-minute retry
interval while one is pending (instead of ten), and the commit clears it only when the fetch that finished was one that started at or after the due time -- a fetch begun before it, or a second renumbering that rescheduled it mid-fetch, leaves it pending. The detection time stands in for the edit time (the addon cannot know the latter), which is safe in both directions: a renumbering noticed hours
later costs one extra guide download, and one noticed within the cache window gets its second fetch after the window. Live, against the real instance with a disposable channel mapped to a guide source, on a test build whose channel refresh interval was minutes instead of hours (the committed code is unchanged): the server's guide was warmed, the channel
renumbered 88881 to 88882, and a fetch 70 s later still listed `88881` and not `88882` (the cache, as found 2026-09-30); the addon noticed, and 409 s after the renumber logged the second fetch; the server then listed `88882`, and Kodi showed the channel under its new number with all its programmes. Not observed directly: the addon's own first fetch inside the cache window (it
logs nothing for a plain fetch), only the server's stale answer at the same moment. The test channel was deleted (404 on re-GET).

#### No locking around live-timeshift / in-progress stream state

**`m_liveTimeshiftStream`/`m_inProgressRecordingStream` (`DispatcharrClient.h`)
have no locking at all around the fields `GetStreamTimes()` and the
read/seek paths share, flagged from an 11th-pass audit (2026-09-26),
architectural -- only the narrowest, highest-risk slice of this was
fixed this pass, not the whole thing.** `GetStreamTimes()` runs on a
different Kodi thread than `ReadLiveTimeshiftStream()`/
`ReadInProgressRecordingStream()` -- confirmed live, not just inferred
(see `docs/TIMESHIFT.md`'s "1.0.5 regression" section: `kodi.log`
showed the read function's own logging stop entirely while
`GetStreamTimes` kept polling, only possible if they run concurrently)
-- and reaches `RefreshLiveManifest()`/`RefreshInProgressRecordingManifest()`,
which `push_back` onto `segments` and write `totalBytes`/
`totalDurationMs`/`lastManifestFetch`, with no synchronization against
the read/seek paths touching the same fields. **Also
`segmentBaseUrl`** (noted 2026-09-27, a 53rd-pass audit, found via a
project-wide review, not itself independently reproduced; corrected
the very next, 54th, pass -- see below): `RefreshLiveManifest()`
reassigns `m_liveTimeshiftStream.segmentBaseUrl` (a plain `std::string`)
on every successful refresh, while `ReadLiveTimeshiftStream()`
concatenates that same string on its own thread -- a data race on a
`std::string` by the same standing definition this entry's own fields
already are, just missed from the original list. The same underlying
gap, not a separate one -- covered by the same eventual fix this entry
already describes, not a fix of its own.
**Correction (2026-09-27, a 54th-pass audit): this entry's own original
"mostly benign... no reallocation actually occurs" framing was wrong,
found via a project-wide review, confirmed empirically against
libstdc++, not itself independently reproduced live.** Two
independently move-assigned `std::string` temporaries with identical
*content* still generally occupy *different* heap buffers -- confirmed
empirically (the underlying data pointer changed on every
reassignment, regardless of whether the content differed). The old
buffer is freed on every single refresh cycle this string is
reassigned, roughly every 500ms during live playback, not only on a
content change -- a genuine, if narrow, use-after-free window against
the concurrent read, on every cycle rather than a rare one. Partially
mitigated the same pass (not a full fix -- the underlying architectural
gap, a real per-stream mutex never held across a network call, is
still open): `RefreshLiveManifest()` now only actually reassigns this
field when the newly-built URL genuinely differs from the current one
(a plain, race-free read-vs-read comparison beforehand; only the write
itself is unsafe) -- in practice, `httpPort`/`routePrefix` (read fresh
from the plugin's own response every refresh) essentially never change
once a session is underway, so this closes the race for the
overwhelmingly common case, leaving it open only for the genuinely
rare case where they do. This pass fixed
the specific, confirmed-exploitable danger of holding a raw segment
pointer across a blocking `curl_easy_perform()` call in both read
functions (a dangling-pointer read once the vector reallocates
underneath it) by copying the needed fields out before the network
call -- but that's a narrow patch, not real synchronization. Still
open: (a) `FindSegmentContainingPosition()`'s own initial scan can
still race a concurrent `push_back` in principle (a much smaller
window than the one just closed, but not zero); (b) two threads can
both read a stale `lastKnownSequence` before either appends, then both
append the same new segments -- a duplicate ~2s chunk spliced into the
byte stream, a plausible, untested contributor to this project's own
"Packet corrupt"/audio-desync reports; (c) `CloseLiveTimeshiftStream()`/
`CloseInProgressRecordingStream()` reset the whole struct while another
thread may be mid-refresh. A real fix needs a per-stream mutex that's
never held across a network call (the same "don't hold a lock across
`Request()`" lesson the "m_authMutex held across a network round trip" entry already
describes needing) -- a genuine concurrency redesign across every
read/seek/refresh/open/close call site, not a quick patch, so it needs
its own dedicated pass with live verification (e.g. logging
`std::this_thread::get_id()` in `GetStreamTimes`/`ReadLiveStream` to
directly confirm the thread split before touching anything).

**Fixed (2026-10-02), the duplicate-append race reproduced live before and gone after; the thread split itself was measured and is narrower than this entry assumed.**

*Which threads.* An instrumented build logged the thread of every stream callback (open, read, seek, length, `GetStreamTimes`, close, and the refreshes and keep-alive they reach) and flagged any two threads inside at once. On the Linux Kodi 21 test client every one ran on a single thread, for live timeshift and for an in-progress recording, through play, pause, resume, step seeks and stop; no overlap. So on that platform the race below is not reachable from Kodi itself. The older "`GetStreamTimes` kept polling while the read logged nothing" report came from macOS, and neither macOS nor Windows was measured, so the locking is kept rather than argued away: Kodi's contract does not promise one thread.

*The race, reproduced.* With a second thread forcing `RefreshInProgressRecordingManifest()` while Kodi played an in-progress recording (a throwaway build, never committed), the segment list gained a duplicate within seconds: the same segment URL twice at contiguous offsets, which splices a repeated chunk into the byte stream (gap (b) below, the plausible contributor to "Packet corrupt" reports) -- 20 violations logged in 150 s. Both refreshes read `segments.size()` before their network calls and each appended what it found. The same check on live timeshift stayed clean (its merge reads the last known sequence after the network call, a microsecond-wide window).

*The fix* (`DispatcharrClient.h`'s block at `m_liveStateMutex` states the rules). Per stream, three mutexes and a session counter: a state mutex guarding the struct, held only for short reads and writes and never across a network call, a sleep or a call into another locking function; a refresh mutex making the manifest refresh one-at-a-time (an unforced caller that finds one running returns at once, a forced one waits); a curl mutex held while the persistent handle or the cached segment bytes are in use, which `Close` takes first, so it cannot free them under a read. Every function that drops the state lock around a network call remembers the session first and discards its result if the stream was closed or reopened meanwhile; a read also gives up (Kodi retries it) when a seek moved the position during its fetch. Lock order: curl or refresh mutex, then state mutex, then the segment-cache mutex. The keep-alive claims its next due time before its request so two threads do not both send one. This closes (a) (a scan racing a push_back), (b) (duplicate append) and (c) (`Close` resetting the struct mid-refresh), and makes the `segmentBaseUrl` reassignment guard unnecessary (the field is written under the lock now).

*Checked live* on the real instance with the stress thread forcing refreshes throughout: in-progress playback with periodic seeks, zero violations (20 before, same test); live timeshift through pause, resume and four step seeks, zero violations, every seek in the right direction. Regression runs on the fixed build without the stress thread: a paused viewer whose buffer's ffmpeg died (ended by itself after the buffered content, as with 0.7.0); an in-progress recording played through to its natural end (the player ended by itself, the recording finalized); a recording deleted on the server mid-playback (the player ended, one "gone on the server" line, no further requests); and the unprobeable-segment fault through the proxy (the 30 s give-up logged, playback reached the real end). Not exercised: `Close` overlapping a read in flight and the session-discard branches -- Kodi does not produce either, and no harness here can.

#### SetSetting*() swallowed while the settings dialog is open

**Suspected: an addon-initiated `SetSetting*()` call made while this
addon's own settings dialog happens to be open gets silently swallowed
by the dialog, flagged from a 22nd-pass audit (2026-09-26), confirmed
against Kodi's own real source, not reproduced live.** Kodi's
`Interface_Base::UpdateSettingInActiveDialog()` (`AddonBase.cpp`)
routes a `SetSetting*()` call into the *open dialog's* own pending
value instead of actually persisting it, whenever
`WINDOW_DIALOG_ADDON_SETTINGS` is open for this specific addon -- if
the user then cancels that dialog, the value is discarded entirely,
not just deferred. Concretely: `PersistApiKeyIfChanged()` regenerates
the API key during playback and calls `SetSettingString("api_key",
...)`, updating `m_lastAppliedConfig.apiKey` to match -- but if the
settings dialog happened to be open and gets cancelled, Kodi keeps the
old key while this addon's own in-memory cache already reflects the
new one. The next settings save re-delivers the *old* key, which no
longer matches `m_lastAppliedConfig.apiKey`, triggering
`ADDON_STATUS_NEED_RESTART` -- an unneeded instance restart (the exact
harm `docs/TIMESHIFT.md`'s own spurious-restart section already
documents as confirmed live for a different root cause), tearing down
playback for no real settings change. Low likelihood (needs the
dialog open at the exact moment of a key self-heal) and self-healing
either way (a mismatched key just triggers another regeneration on the
next 401) -- there's no clean addon-side fix (Kodi's own dialog
behavior isn't something this addon can influence), so logged rather
than acted on.

**Update (2026-09-27, a 48th-pass audit): the same mechanism now also
applies to `SyncTimezoneFromDispatcharr()`'s own background-thread
retry, confirmed against Kodi's own real current source, not
reproduced live.** If this addon's settings dialog happens to be open
when that retry's own `SetSettingString("recurring_rule_timezone", ...)`
call lands, and the dialog is then cancelled, the write is discarded
the same way -- but `SyncTimezoneFromDispatcharr()` still returns
`true` regardless (it only checks whether *Dispatcharr's own* answer
was read successfully, not whether the resulting write actually
landed), so `m_timezoneSynced` latches `true` and the retry never runs
again this session, silently leaving `recurring_rule_timezone` at
whatever it was before the cancelled dialog. Same low-likelihood,
no-clean-addon-side-fix reasoning as above -- a possible mitigation
would be reading the setting back after the write and only treating
the sync as complete once it matches the desired value, logged rather
than attempted this pass.

**Update (2026-09-30): confirmed live, the timezone half fixed, the API-key half
left.** Reproduced against Kodi itself, with one important caveat: the swallowing
only happens when the dialog is opened the way a user does it (Add-ons -> PVR
clients -> Configure). `GUI.ActivateWindow(addonsettings, ...)` leaves the dialog
unbound to the addon, so `UpdateSettingInActiveDialog()` declines and writes land
normally -- an easy way to conclude the premise is false. With the real dialog
open, the timezone write from the background retry was dropped (`settings.xml`
kept the old value) and cancelling the dialog kept it dropped.

`SyncTimezoneFromDispatcharr()` now reads the setting back and reports failure if
it didn't take, so `m_timezoneSynced` stays false and the next cycle writes it
again; the admin flag and the padding sync got the same read-back, since they latch
the same way. Confirmed live: the "write didn't land -- will retry" line, the old
values still on disk, and, after cancelling the dialog, the next retry landing them.
What stays open is `PersistApiKeyIfChanged()`: the read-back that would catch a
dropped key write pulls in opposite directions depending on what the user then does
with the dialog (OK saves the dropped value, cancel discards it), so aligning the
`m_lastAppliedConfig.apiKey` cache either way trades one spurious restart for
another. The consequence is unchanged -- low likelihood, self-healing.

**The API-key half fixed (2026-10-02), confirmed live; the dilemma the entry gave for it was resolved by not guessing which way the dialog went.** The two outcomes were never ambiguous once the addon remembers what Kodi still stores. `PersistApiKeyIfChanged()` now reads the setting back after its write; if it did not land (the dialog is open) it records what Kodi still holds (`m_apiKeyStoredAfterDroppedWrite`). When Kodi later re-delivers exactly that key through `OnAddonSettingChanged()`, `ClassifyApiKeyDelivery()` calls it a stale re-delivery -- no restart, and the key in use is kept (the old one is already revoked server-side, one key per account); any other different key is a user's edit and restarts as before, and clears the record. The background thread's `RetryDeferredServerSyncs()` then calls `ReassertApiKeySetting()` each cycle, which writes the current key once `DecideApiKeyStoreAction()` finds the stale one still stored, and stops if the user entered a key of their own. If the dialog is saved instead, Kodi stores the swallowed value itself and the record clears on the next cycle. The three decisions are unit-tested; the read-back and the retry are glue.

Live, on the Linux client with the settings dialog opened the way a user does it (Add-ons -> PVR clients -> Configure; opened through `Addons` browser navigation with `Input.*`, focus checked via the `System.CurrentControl` label): with the dialog open, the account's key was rotated from outside and a recording played, which made the addon adopt the new key -- logged "the new API key was not stored", and `settings.xml` kept the old key while the dialog still showed it. After the dialog closed, reopening it, toggling a harmless setting and pressing OK re-delivered the old key: the addon logged "Kodi re-delivered the previous API key ... keeping the current key" and did not restart (the previous build treated it as a change and restarted the instance). About six minutes later the retry logged "the API key setting is stored now" and `settings.xml` held the server's key.

#### Non-fatal Packet corrupt on server-side live timeshift

**Recurring, non-fatal `Packet corrupt` on server-side live timeshift,
post-1.0 (surfaced during 1.0.7 verification, 2026-09-07).** Confirmed
live on macOS (Channel B 1080p): ffmpeg's mpegts demuxer logs `Packet
corrupt` roughly once every ~2.5s throughout an otherwise-healthy
4+ minute playback session (close to the plugin's 2s
`segment_seconds`), with Kodi's own demuxer evidently resyncing
cleanly each time -- no stall, no visible playback impact, real seeks
worked fine. Ruled out as the *same* mechanism as 1.0.7's
permanent-freeze fix (that fix's own diagnostic, which directly
detects a segment-size disagreement, never fired once across many
occurrences of this). **Refined hypothesis (2026-09-07, still not
confirmed, deliberately not pursued further):** ffmpeg's `-f segment`
muxer opens a fresh `AVFormatContext` per segment file -- `-c copy`
skips re-encoding, but the TS *muxing* layer (including each PID's
continuity counter) is regenerated fresh per file, normal for
independently-playable HLS segments but not for this addon's own
design of concatenating them into one raw byte stream. Real
corroborating precedent already in this codebase: `-reset_timestamps
1` was deliberately removed for the exact same class of problem
(per-segment muxer state resetting), just for PTS continuity instead
of continuity counters. Deliberately left unfixed: no known ffmpeg
flag suppresses the reset, and the alternative (binary-patching
continuity counters at each splice in the live read path) is real
complexity/risk for a symptom still confirmed cosmetic. See
`docs/TIMESHIFT.md`'s "1.0.6 follow-up" section's second "Update" note
for the full reasoning. Not blocking any release -- flagged so it
doesn't get lost, not because it's currently causing visible harm.
(Not the same bug as the "Second Packet corrupt/freeze on buffer switch" entry -- that one *did* trigger
the size-disagreement diagnostic; this one still hasn't, across many
occurrences.)
**Update (2026-09-07): stopped being cosmetic once.** First observed
real failure on macOS (a news channel, one mid-session channel
switch): two `Stream stalled, start buffering` events, the second with
a severe, sustained audio desync (`ActiveAE - large audio sync error`
holding at -8000ms) and `[h264]` decode errors, ending in Kodi's own
player tearing the stream down on its own (no permanent hang, no
crash). Same underlying noise, same ~2-4s rate, still not the
size-disagreement mechanism (zero diagnostic firings this session
either) -- just the first time it's caused visible harm instead of a
silent resync. One occurrence, not yet a reliable repro. This is now
a real, needs-fixing signal rather than a purely theoretical
"revisit if it stops being cosmetic" trigger. See `docs/TIMESHIFT.md`'s
"The 'cosmetic' Packet corrupt noise stopped being cosmetic once"
section. Next step: try to force a live repro deliberately rather
than waiting on another incidental occurrence.
**Update (2026-09-08): a 15-minute, 105-switch rapid-channel-cycling
stress test (Windows, addon 0.9.0) did not reproduce it.** Same
benign `Packet corrupt` rate as always, zero size-disagreement
firings, zero errors/crashes, and every audio-sync/buffer-timeout
warning landed within seconds of a channel switch (normal
pipeline-reset noise), not a standalone problem. Rapid switching
specifically doesn't appear to be the trigger -- the original report
was continuous playback with only one switch. Next attempt should
try long continuous dwell on a single channel instead. See
`docs/TIMESHIFT.md`'s same section for the full test breakdown.
**Update (2026-09-08): a 30-minute continuous Channel B dwell (the
follow-up angle above) also did not reproduce it.** Same benign
`Packet corrupt` rate, zero size-disagreement firings, zero audio
desync, zero stalls, zero `catch-up-to-tail` "gave up" exhaustions
across the full 30 minutes. Two different stress angles tried now
(rapid switching and long dwell), neither reproduced it -- still a
single, unreplicated occurrence. See `docs/TIMESHIFT.md`'s same
section for the full breakdown.
**Update (2026-09-08): a second, real occurrence -- this time
self-recovered.** macOS, addon 0.9.0, ~20-hour continuous session
(Channel A): a burst of three `Stream stalled` events in ~3.6s,
audio desync peaking at -5000ms, but this time Kodi's own resync
machinery pulled it back under threshold within about a second with
no manual intervention and no user-visible interruption -- unlike the
first occurrence, which required manually stopping the player. Same
noise, same rate, zero size-disagreement firings across the full
session. Two data points now with different outcomes (fatal vs.
self-recovering), suggesting a second branch point beyond just
cosmetic-vs-escalates. Still not enough signal to act on. See
`docs/TIMESHIFT.md`'s same section for the full detail.
**Update (2026-09-08): a reliable, on-demand trigger found --
seeking to the live edge, reproduced 3-for-3 in one session
(macOS).** Severity escalated across the three reproductions,
peaking at 250,000ms (over 4 minutes) of desync and 400
hardware-decoder failures in the third one, still not fully
recovered when the user gave up waiting and restarted. Confirmed
not a regression from that change (its diff doesn't touch the seek/read
path). This changes the situation from "wait for it to happen
incidentally" to "can be deliberately tested" -- worth deciding
whether to actually chase this now rather than keep just recording
occurrences. See `docs/TIMESHIFT.md`'s same section for the full
writeup and the reasoning for why live-edge seeks are a plausible
trigger (landing on an arbitrary point with no guaranteed clean
H.264 keyframe/SPS-PPS boundary, unlike a seek into already-settled
earlier segment data).
**Update: the segment-wraparound hypothesis is ruled out -- this is
channel/stream-specific.** Real segment-file-reuse threshold for the
affected instance is far larger than a typical buffer age; both real
incidents happened at 1-5 minutes of buffer age, nowhere close.
Confirmed on Windows too: 6 live-edge seeks on a 2-minute buffer,
zero corruption. Controlled trials then found the real variable:
Channel B (1080p) resisted 4 escalating trials (up to 1,500 decode
errors, zero audio-sync errors); Channel A (720p) reproduced the
severe form instantly on the *mildest* method (220 decode errors,
180 audio-sync errors, 100s peak desync, first attempt). Same
method/client/machine -- only the channel changed. Next: more Channel A
trials to confirm, plus a same-resolution comparison channel to
isolate whether it's resolution or this specific stream's encode.
**Update: likely root cause found.** A bare `ffprobe` probe of Channel A's raw Dispatcharr proxy stream -- no seek, no Kodi, no addon,
just a cold TCP connection -- throws ~12 `non-existing PPS 0
referenced`/`no frame!` errors per second, continuously, for the
whole 10s test window; Channel B's same probe is essentially clean (1
unrelated warning). Channel A's keyframe interval is a steady
interval exactly matching this instance's `segment_seconds=2` setting;
Channel B's is a steady but unaligned, different interval. Working theory: Channel A's
segment-aligned GOP means Dispatcharr's segmenter cuts every segment
exactly on a keyframe, and something in that exact-alignment path
corrupts the PPS NAL unit at those cut points almost every time --
explaining both the worse baseline noise and why a live-edge seek
(an extra demuxer resync) tips it into the severe cascade so much
more easily than on Channel B. Points at Dispatcharr's own segmenter for
this channel's encode, not this addon/Kodi/OS. Not yet confirmed
against a second segment-aligned channel or a second misaligned one.
**Update: confirmed via real GUI keypresses, not just synthetic
seeks.** Peer report -- `StepBack`x7 then `StepForward`x6 (real
remote/keyboard input, same path as the original Channel B incidents)
drove a live-edge seek on Channel A and reproduced the identical
failure signature, peaking ~170s desync, recovering at real-time
pace. Closes the GUI-vs-JSON-RPC confound. Also: this buffer was
well over half an hour old (not freshly opened), so reproducibility on this
channel doesn't depend on buffer age either.
**Update: mid-buffer seek control test says this is fixable
client-side.** Same Channel A buffer, direct A/B: seeking to a
genuine mid-buffer point produced only ordinary baseline noise (0
audio-sync-error lines); seeking to the live edge moments later on
that same buffer immediately produced 2,400 audio-sync-error lines.
The cascade is specific to the live edge, not the corrupted stream
in general -- reopens increasing `SeekLiveTimeshiftStream()`'s tail
backoff margin (currently 1 segment) as a plausible, worth-trying
client-side mitigation.
**Update: implemented and tested -- backing off 3 segments instead
of 1 eliminated the cascade across 12/12 trials on Channel A
(Windows), vs. reproducing on the 2nd of 6 attempts pre-fix.** Zero
`large audio sync error` lines in any of 12 attempts; ordinary
baseline noise unchanged. Not proof it's fully eliminated (12 clean
trials, not exhaustive), and not yet tested against an older buffer
or on macOS/CoreELEC -- worth continued normal-use monitoring.
**Update: macOS confirmation, clean pass.** 10/10 scripted attempts
(same method as Windows) plus a separate 69-keypress real-keyboard
session, both on Channel A -- zero `large audio sync error` lines
in either. Two platforms, two input methods, the one channel that
reproduced instantly pre-fix, all clean. Buffer age and CoreELEC
still untested.
**Update: CoreELEC/ODROID N2+ confirmation, clean pass -- and a
stronger one than the desktop platforms got (2026-09-09).** 12/12
live-edge-seek attempts on Channel B (the other real channel used in
this investigation, not Channel A -- deliberately avoided this run),
zero `large audio sync error`
lines in any attempt. Notable: the baseline `non-existing PPS 0
referenced` noise ran much higher here than Channel B's established
clean baseline on Windows/macOS (10-269 per ~20s attempt window here
vs. essentially none there) -- see the new note in
`docs/TIMESHIFT.md`'s same section. Despite that elevated baseline
noise, the fix still held with zero escalations, a more demanding
condition than the earlier clean passes.
**Update: re-run on Channel A itself on CoreELEC -- the hardest
channel, still clean (2026-09-09).** Same 12-attempt method, this
time on the actual channel that's reliably reproduced the severe
cascade instantly on every other platform. Zero `large audio sync
error` lines across all 12, with baseline noise running even higher
than the Channel B run above. All four platform/channel combinations
tested now (Windows/Channel A, macOS/Channel A, CoreELEC/Channel B,
CoreELEC/Channel A) are clean.
**Update: the live-edge-seek fix is now fully closed --
older-buffer condition tested too, clean (2026-09-09).** Every prior
trial used a fresh 2-5 minute buffer; the real incident that started
this investigation happened on a well-aged one. Left a Channel A
buffer open and completely untouched for 40 minutes on Windows, then
ran the same 12-attempt test -- zero `large audio sync error` lines,
baseline noise in the same ordinary range as every fresh-buffer
trial. Every dimension tested (two channels, three platforms, two
input methods, fresh and aged buffers) is now clean. The remaining
cosmetic baseline `Packet corrupt`/`non-existing PPS 0 referenced`
noise this whole investigation started from is still present and
still deliberately unfixed (see this item's own opening paragraph) -- that part was never the target of that change, only the severe
escalation was.

**Root cause confirmed and fixed (2026-10-02, `timeshift_buffer` 0.8.0): the baseline `Packet corrupt` was every PID's MPEG-TS continuity counter restarting at 0 in each segment file the `segment` muxer wrote -- one break per splice in the addon's concatenated byte stream.** Measured on 14 consecutive real segments (a break at nearly every PID at every boundary; ffmpeg over the concatenation: 14 `Packet corrupt`, 12 `corrupt input packet`; zero once the counters were rewritten), then fixed by running the plugin's ffmpeg with the `hls` muxer, which keeps one muxer across files. Live in Kodi, same scripted run before and after: 82 `Packet corrupt` lines, then 0; the audio-sync warnings that follow seeks and the `non-existing PPS 0 referenced` noise are unchanged, so they were never this mechanism (the PPS noise is the channel's own stream, the sync warnings a normal seek reset). The earlier severe-escalation work (the live-edge backoff, 2026-09-08/09) is separate and stands. Full account, the measurement and the live comparison in `docs/TIMESHIFT.md`'s "1.0.6 follow-up" section's 2026-10-02 update. Not done: a long soak of the `hls` muxer's `delete_segments` (aging checked over ~3 minutes at a 1-minute buffer, not over hours), and the CoreELEC/macOS/Windows clients on 0.8.0 -- the addon-side path is unchanged.

#### A brief server outage ended playback for good

**Found by the manual-testing pass and fixed (2026-10-02), confirmed live.** With a forwarder in front of Dispatcharr that could refuse, drop or hang connections, three stream types ended playback in an outage a user would call a blip. *Live timeshift:* four refused segment fetches in a row (about two seconds) tripped `ShouldGiveUpAfterSegmentFetchFailure()`'s count bound and marked the stream fatal for good, though playback resumes fine once the server is back -- an 8 s outage ended playback. *Completed recording:* `ReadRecordingStream()` returned -1 on the first transport failure and Kodi ended playback once its read-ahead drained, about 8 s into a 20 s outage. *In-progress recording:* the segment fetch did the same.

The give-up now needs the failure streak to have lasted 30 s as well as reached four (`firstSegmentFetchFailureAt`, `ShouldGiveUpAfterSegmentFetchFailure()`'s two new duration parameters), with the retry delay doubling from 250 ms to 2 s (`SegmentFetchRetryDelayMs()`) so half a minute costs about twenty requests, not a hundred; a manifest refresh that positively reports the buffer gone still ends it at once. The recording reads retry a transient failure -- an unreachable server, or a 502/503/504 (`IsTransientReadFailure()`) -- for up to 20 s within the one `Read()` (`ShouldKeepRetryingTransientRead()`), aborted at once by `AbortInFlightRequests()`; any other status still fails immediately. The decisions are unit-tested; the retry loops are glue.

Live: an 8 s refused outage during live timeshift now resumes (position kept advancing); 20 s of hung connections left the UI answering every ping in 0.3 s and playback resumed; a 12 s outage during a completed and an in-progress recording both resumed. Longer outages still end playback cleanly: a 45 s one ended it at about 40 s (Kodi's own EOF, which lines up with the plugin's 30 s idle timeout reaping the buffer anyway), the UI stayed responsive throughout. The numbers (30 s streak, 20 s budget) were chosen against those two limits, not tuned further.

**Correction and addition (2026-10-02, found while testing the auth fix): the live half also needed the retry inside the `Read()` call.** The first version above handed Kodi a zero-byte read after each failed fetch. That survived an 8 s outage in the first run only because Kodi had read-ahead buffered; replayed at the live edge (nothing buffered) it ended playback after about five seconds of zero-byte reads, on the committed build and the previous one alike, so the earlier pass was luck. `ReadLiveTimeshiftStream()` is now a wrapper that repeats `ReadLiveTimeshiftStreamOnce()` while a segment fetch fails (`HandleLiveTimeshiftSegmentFetchFailure()` returns `kRetrySegmentFetch` after its backoff sleep), for up to 25 s, aborted at once by `AbortInFlightRequests()`; the 30 s give-up and an unreachable-buffer `fatal` still end a stream that stays dead. Live, at the live edge: 8 s and 20 s refused outages resume (position kept advancing); a 45 s one still ends playback cleanly.

#### Manual-testing checklist for a Kodi sanity pass

**Manual-testing checklist for a Kodi sanity pass, covering what
neither the unit test suite nor the JSON-RPC-driven smoke-test tooling
can reach (2026-09-15, updated 2026-09-16 once macOS, CoreELEC, and
Android reached the same coverage as Linux and Windows).** The unit
suites cover pure logic; `tools/kodi_smoke_test.py` (platform-generic
-- it only speaks Kodi's own JSON-RPC API, confirmed live unchanged
against a Linux/Flatpak install, a native Windows install, a real
CoreELEC/ODROID N2+ install, a real macOS install -- run
entirely by a peer Claude Code session on that last one -- and two real physical
Android devices, one older 32-bit ARM device and one newer 64-bit ARM
device, the first two platforms this project has ever run the
same live check against on both a 32-bit and a 64-bit build of the
addon) covers the addon's live PVR-API surface against a real
Dispatcharr backend, provisioned via
`tools/kodi_provision_linux_flatpak.sh` or
`tools/kodi_provision_windows.sh` (CoreELEC, macOS, and Android were
provisioned by hand, not via a dedicated script -- see
`docs/BUILDING.md`). Three real bugs (a `Player.Seek` stale-readback
race and its still-growing-recording follow-up, and a timer/
recurring-rule orphan-cleanup gap) and two non-bug scares (a
recordings-count mismatch, a duplicate-timer creation against an
already-airing broadcast) came out of the four-platform pass that
preceded Android -- see `docs/RECORDINGS.md`/`docs/TIMESHIFT.md`/
`docs/TROUBLESHOOTING.md` for the full accounts. Android itself needed
a real, permanent `CMakeLists.txt` fix (Kodi's own upstream Android
build tooling has no way to expose depends-built curl to an addon's
CMake configure step) before it would build at all -- see
`docs/BUILDING.md`'s new Android section. Every target platform this
project set out to support is now confirmed live; what's left needs a
human, on any platform:
- **Anything visual/GUI**: the AddonSettings dialog's own
  settings-entry flow (JSON-RPC synthetic input can't drive it --
  confirmed live on three separate real devices/builds now, two
  Linux and one Windows, see `docs/TIMESHIFT.md`'s AddonSettings-
  dialog finding); actual picture/audio quality during playback
  (JSON-RPC can only confirm position advanced, not that the picture
  isn't corrupted or audio isn't desynced); `DialogConfirm.xml`
  scheduling-conflict/live-vs-recording-collision dialogs (the smoke
  test avoids triggering these rather than testing them); EPG grid/
  channel-logo/Recordings-list rendering in the real skin.
- **Real hardware, not a virtualized GPU**: real hardware-decode
  (VAAPI on actual Intel/AMD/NVIDIA Linux hardware, DXVA2/D3D11VA on
  real Windows hardware) and CoreELEC/ODROID N2+ specifically (a
  separate Linux target with its own history of hardware-decode
  bugs) aren't substituted for by a VM's virtual GPU. Audio
  passthrough (AC3/DTS bitstreaming to a real AVR) is in the same
  boat on either platform.
- **Long-running/soak conditions**: the smoke test only plays for
  seconds per check -- the server-side timeshift buffer actually
  rolling over its full retention window, `RenewRecurringRules()`'s
  periodic renewal firing on its real schedule, and extended-playback
  memory/stability all need real elapsed time.
- **Failure/edge-case resilience against the real backend**,
  deliberately not simulated: Dispatcharr becoming unreachable or a
  network drop mid-playback, a provider-side concurrent-stream-limit
  hit (a real condition stumbled into by accident during this
  project's own testing, never deliberately exercised), disk-full
  during a recording.
- **Fresh-install and upgrade paths**: a truly clean install (first
  EPG fetch, first channel sync, entering credentials for the first
  time) and upgrading the addon in place from an older version.
- **Multiple concurrent Kodi clients** against the same backend --
  already flagged as an inherent limitation elsewhere in `docs/`, but
  never driven end-to-end.

**Resolved (2026-10-02): the checklist now exists as `docs/MANUAL_TESTING.md`, and what the lab can drive was driven.** What stays human-only is listed there by section (picture and audio judgement, the settings dialog's own entry flow, real hardware decode and audio passthrough, hours-long soak and rule renewal, and the genuinely destructive failures: a provider stream limit, a full disk). Run from the lab: server outages on live, completed and in-progress playback (found and fixed the bug above), a clean first run, a second viewer on a shared buffer, the rolling-window resume, and a 30-minute soak with memory sampling (result in the document).

#### m_authMutex held across a network round trip

**`m_authMutex` is held across a real network round trip inside
`Login()`/`RefreshAccessToken()`/`EnsureAuthenticated()`, flagged from
a 10th-pass audit (2026-09-26), architectural -- not fixed this pass.**
Every other caller that just needs the current token (`Request()`'s
own `withAuth` branch, `SendTimeshiftHeartbeat()`) only holds the lock
briefly to copy it, but `Login()`/`RefreshAccessToken()` themselves
call `Request()` while still holding the same recursive
`std::recursive_mutex` -- for up to `m_config.timeoutSeconds` (30s
default, configurable to 180s). While any one thread is inside a real
login/refresh network call, every other thread's own attempt to just
read the cached token (including `ReadLiveTimeshiftStream()`'s own
playback read loop via `SendTimeshiftHeartbeat()`) blocks until that
call finishes or times out -- directly contradicting
`SendTimeshiftHeartbeat()`'s own comment, which already documents
fixing a 1.0.5-era stall on the premise that reading the cached token
is "a mutex lock, not a network call". Not reproduced live (needs a
specific interleaving: a background refresh thread's own login/token
refresh landing at the same moment as a live playback read). A real
fix means restructuring these three functions so `m_authMutex` is only
ever held for member-variable read/write, never across `Request()`
itself -- a genuine concurrency redesign (two threads racing to
refresh/login simultaneously needs its own correctness argument), not
a quick patch, so it needs its own dedicated pass with live
verification rather than a blind change bundled into this one.
(`SendTimeshiftHeartbeat()`'s own narrower gap -- sending an
expired-but-present token instead of skipping, per its own comment's
stated intent -- was fixed this same pass; this item is the deeper,
remaining one.)

**Fixed (2026-10-02), the stall reproduced live before and gone after, and one refresh per herd confirmed.** The one recursive mutex is now two plain ones (`DispatcharrClient.h`, the block at `m_authStateMutex`). `m_authStateMutex` guards the token fields and the login-failure state and is held only for short reads and writes, never across a network call. `m_authFlowMutex` serializes the login/refresh flow and is held across its network calls, but only by a thread that actually has to authenticate: `EnsureAuthenticated()` returns on a still-valid cached token after the short state lock alone, and looks again after getting the flow mutex (another thread may just have succeeded, or failed and set a backoff that answers for this call too). `Login()` and `RefreshAccessToken()` require the flow mutex and take the state mutex only to write their result. Neither needs to be recursive: nothing under the flow mutex re-enters `EnsureAuthenticated()`. The lock order is flow, then state, and the API-key recovery mutex still comes before both.

Measured with a throwaway probe (never committed) on the Linux client, the server's connections made to hang so a token refresh ran its full 30 s timeout: a call that only reads the cached token (the timeshift heartbeat) waited **29,708 ms** behind that refresh on the committed code, and **0 ms** with the split locks. And with the probe forcing 16 threads to need a token at the same instant, exactly one refresh request went out and all 16 were authenticated, three rounds running. Playback, recordings and the outage scenarios were rerun on the fixed build without the probe. The decision logic (backoff, which failures count) was already pure and tested and is unchanged.

#### macOS build and smoke test of the hardening branch is stale

**The last macOS build predates this work; nothing from the `timeshift_buffer` 0.6.7/0.6.8 batch's C++ counterpart (redirect policy, bounded response sink, SHA-1 handshake check, the
unprobeable-segment and tail-seek fixes) has been built or run on macOS, 2026-10-02.** The Mac used for manual builds (see `docs/BUILDING.md`) was unreachable when this was attempted. Everything else was exercised on 2026-10-02: CI ran the workflow on the branch for the first time (lint, Python and C++ unit tests, the sanitized C++ tests, the Linux build, plugin packaging and the Windows build all green), and the CI-built Windows DLL
ran in the Windows Kodi against the real instance: the smoke harness passed 17 of 18 (the live-timeshift seek check skipped, server-side timeshift not ready on that profile), including the
realtime push, which exercises the new `Sec-WebSocket-Accept` check. To close: build the branch tip on the Mac per `docs/BUILDING.md` and run it in a macOS Kodi if one is available.

**Update (2026-10-02): the build half is done.** The Mac was reachable, and the branch tip builds cleanly on macOS (arm64, Kodi Omega harness, `addon-pvr.dispatcharr-unofficial-0.11.0-osx-arm64.zip`, 347 KB), three times as the branch moved, the last of them at the final commit. Not done: running it in Kodi and driving the smoke harness. A macOS Kodi is installed on that Mac, but launching it and pointing it at
the real instance needs the maintainer's say-so; the build was not copied anywhere. Everything else on the branch has now been exercised on Linux and Windows, including the realtime push, so what a macOS run would add is the platform-specific socket and `poll()` behaviour. Left open for that run.

**Closed (2026-10-02): the branch tip was built on macOS and run in a macOS Kodi against the real instance.** Built on the Mac through the usual harness (arm64, Kodi Omega 21.3, 352 KB zip, no errors); run in a Kodi 21.3 with a separate, throwaway profile (a different `HOME`, deleted afterwards). The smoke harness: **16 passed, 0 failed, 2 skipped** (the two in-progress-recording checks, nothing was recording), with `--allow-mutations` and the realtime-push check, so the macOS socket path is covered: the WebSocket connected with the `Sec-WebSocket-Accept` check and `poll()` in place, and a recording created behind Kodi's back appeared with no restart. Then by hand: a 2.5-minute server-side-timeshift session with a pause and four step seeks on the channel that raised the original macOS `Packet corrupt` reports -- 565 segment fetches, **0 `Packet corrupt`** (plugin 0.8.0), 0 audio-sync errors, no fatal stream, no addon errors -- and an in-progress recording played with seeks and a pause (30 segment fetches, no errors). Not done on macOS: the server-outage scenarios and a soak (done on Linux, `docs/MANUAL_TESTING.md`).

#### GetTimers reported a server error right after a timer change

**Found on the real N2+ and fixed (2026-10-02), measured on Linux: 6 errors in two smoke runs before, 0 after.** Kodi logged `GetTimers: Add-on N returned an error: server error` around every timer add/delete (also seen earlier on the Linux client). Cause, a regression from the 2026-10-02 generation-counter work: `GetTimers()`'s "never loaded" guard (`HasNeverLoadedSuccessfully(m_recordingsCachedAt / m_seriesRulesCachedAt)`) read a zero cached-at time as "no data ever", but invalidation resets that time to zero on purpose, and a fetch discarded as out of date (an invalidation landed mid-fetch) leaves it zero too -- while the data is held and fine. Kodi keeps its timers on an error, so the harm was a spurious error and a delayed update, not data loss. The guard now reads `m_recordingsEverLoaded` / `m_seriesRulesEverLoaded`, set when data is committed and never reset by invalidation; the timestamps stay what they were (freshness only). The GetRecordings link-prune guard uses the same flag. Not unit-tested (member state); confirmed live by the same smoke runs, and recorded and live playback were rechecked on the fixed build.

#### CoreELEC-built addon.xml has an empty <platform> tag

**CoreELEC-built `addon.xml`'s `<platform>` tag comes out empty, root
cause confirmed live but real-device impact still unverified (found
2026-09-13, rebuilding the CoreELEC package from a fresh environment).**
Full root-cause trace is in `docs/BUILDING.md`'s CoreELEC section --
it's a structural property of how Kodi's own upstream build generates
the `KodiConfig.cmake` that CoreELEC's `kodi-binary-addons` packaging
class builds addons against (`PLATFORM_TAG` never gets computed on
that path), not anything specific to this addon's own
`addon.xml.in`/`CMakeLists.txt`/`package.mk` -- affects any addon
CoreELEC packages this way. Believed harmless (Kodi's addon loader
keys off `library_linux`, which is correctly populated, not the
`<platform>` metadata tag) but that's inference from reading Kodi's
own source, not a real N2+ install test. Next real CoreELEC zip build
(release or otherwise) should include actually installing it on the
N2+ and confirming Kodi loads it normally despite the blank tag.

**Update (2026-10-02): the source says it is harmless, a test zip built from the branch tip without a tag is ready, and only the device check remains.** *From source:* Kodi's `CAddonInfoBuilder` (`xbmc/addons/addoninfo/AddonInfoBuilder.cpp`) parses `<platform>` only when `GetText()` is non-null, so an empty element leaves `m_platforms` empty, and `PlatformSupportsAddon()` returns true for an empty list -- checked in both upstream Omega (`f8815ee`) and the very Kodi tree CoreELEC builds (`kodi-acc0fc66...`, in `~/coreelec-build`), where the two functions are identical. So an empty tag is accepted, not rejected. *Build, no tag:* `package.mk` needs a tagged release tarball only because that is where it fetches from; staging a `git archive` of the tip at `sources/pvr.dispatcharr-unofficial/pvr.dispatcharr-unofficial-0.11.0.tar.gz` with its `.sha256` (and putting that hash in the tree's copy of `package.mk`, never the repo's) makes `create_addon` build it with no tag and no public repo. It built (`ALL ADDONS BUILT SUCCESSFULLY`, 32-bit ARM EABI5, `Amlogic-ng`/`arm`, needs only `libcurl`/`libstdc++`/`libm`/`libgcc_s`/`libc`), the installed `addon.xml` has `<platform></platform>`, and -- the gotcha seen on 2026-09-28 again -- the harness did not re-package, leaving the 2026-09-16 zip in `target/addons`, so the fresh `.so` and `addon.xml` were zipped by hand (`docs/BUILDING.md`'s fallback). What is left is `docs/MANUAL_TESTING.md`'s section 2 check on a real N2+: install that zip and confirm the addon enables and plays. If it does, this item closes as harmless; if Kodi on the device refuses it, the fix is in the package build (set `PLATFORM_TAG` / post-process the rendered `addon.xml`), not in the addon.

**Closed as harmless (2026-10-02): confirmed on a real ODROID N2+.** CoreELEC Omega (`Amlogic-ng`, 32-bit userland on a 64-bit kernel), the addon built from the branch tip through CoreELEC's own harness with the blank `<platform></platform>` tag intact and put in `/storage/.kodi/addons`. Kodi found it (`CAddonMgr::FindAddons ... installed`), enabled it and created the PVR client with no `No platform for add-on` line, and the addon went on to adopt the account's existing API key, sync the timezone and load the channel lineup. (The box's earlier install, a CoreELEC-packaged 0.10.1.1 with the same blank tag, had also been accepted by Kodi.) Then, against the real instance: the smoke harness **14 passed, 0 failed** on a 1080p channel (Channel B), run twice after the fix below (3 skipped: nothing recording, no catch-up programme to play), including live-timeshift seek, recorded playback and its seek, and timer and recurring-rule creation; and a 2.5-minute live session with a pause and four step seeks on that 1080p channel: 1,503 segment fetches, **0 `Packet corrupt`**, 0 audio-sync errors, no fatal stream, no addon errors, and the log shows `CAMLCodec::OpenDecoder` at 1920x1080 -- Kodi's Amlogic hardware decoder was used (its FFmpeg open just before it is the probe). The same checks also found and fixed the `GetTimers` server-error bug (see its own entry). Left as it was: the harness not re-packaging the zip on this branch (`docs/BUILDING.md`'s hand-zip fallback), and the tag itself, which stays blank in CoreELEC builds because that is how their `KodiConfig.cmake` is generated.

#### Recurring timers for Vancouver and Edmonton would have been an hour off from 2026-11-01

**Fixed 2026-10-04 (the eighth hardening sweep), time-critical; checked against the installed tzdata 2026c.** British Columbia and Alberta stop changing their clocks and stay on daylight time from 2026-11-01 (the zone data for `America/Vancouver` and `America/Edmonton` says so), but `TimeZoneUtil.cpp`'s table still modelled both as ordinary North American zones that fall back that day, so a recurring rule created while the server timezone was one of them would have been stored an hour off for as long as the addon ran an unfixed build. Each of the two entries now carries `permanentDaylightFromUtc` (`kPermanentDaylightFromNov2026Utc`, 2026-11-01T00:00:00Z; any instant before the would-be fall-back at 09:00Z gives the same answer, because until then the zone really is on daylight time), and `ComputeKnownZoneOffsetMinutes()` answers the standard offset plus one hour from then on, for the zone's aliases too (`Canada/Pacific`, `Canada/Mountain`, `America/Yellowknife`). The new `tools/tests/test_timezone_table_vs_zoneinfo.py` compares every table entry with the system's zone data for a span of dates so the next legislative change fails CI instead of waiting to be noticed (it skips where no tz database is installed). Unit-tested, not exercised live.

#### A late DVR-padding result could still overwrite a newer edit

**Fixed 2026-10-04 (the eighth hardening sweep), unit-tested.** The first fix of this family (an earlier entry) compared values, so with two edits in flight an older value's late result could still park itself over, or clear, a newer one. `PaddingPush.h` is now sequence-numbered: `BeginPaddingEdit()` numbers every edit, a result only clears a parked value when it belongs to the newest edit of that side, and any other outcome parks the newest value for the background retry. `DispatcharrClient::SetDvrOffsetMinutes()` takes an `onResult` callback that runs under the same mutex as the request, so results are applied in the order the server saw them (the premise the model in `tests/test_padding_push.cpp` relies on; the test enumerates every landing order against every outcome).

#### Editing a recurring rule whose start fell in a spring-forward gap re-sent its times

**Fixed 2026-10-04 (the eighth hardening sweep), unit-tested.** A rule whose time of day does not exist on its first day in Kodi's own zone (inside the clock-forward gap) round-trips to a different time of day, so an edit that changed nothing about the times still differed from the stored ones and sent them, regenerating every occurrence. `GetTimers()` now remembers the instants it reported per rule (`m_reportedRecurringRuleTimes`) and `ResolveRecurringRuleTimesOnEdit()` (`RecurringRuleEdit.h`) keeps the rule's stored times of day whenever Kodi hands back exactly those instants.

#### Deleting a series rule could remove a source-pinned rule with the same title

**Fixed 2026-10-04 (the eighth hardening sweep), unit-tested.** Dispatcharr's series-rule delete only restricts to an EPG source when the request names one, so deleting (or replacing, on a title or `tvg_id` edit) an unpinned rule while a pinned rule with the same title and `tvg_id` existed removed both. `HasPinnedSiblingSeriesRule()` (`TimerIdentity.h`) now makes the replace path skip the delete with a notification, and `DeleteTimer()` refuse with an error telling the user to delete it in Dispatcharr.

#### A Redis permission error escaped the script fallbacks

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.7, unit-tested.** An ACL without scripting makes Redis answer `NOPERM`, which redis-py raises as `NoPermissionError`, a *subclass* of `ResponseError`; `_release_start_buffer_lock()` and `_cas_buffer_state()` matched the class name exactly and re-raised it. `_is_redis_response_error()` looks through the exception's whole MRO.

#### The SIGTERM grace in `_stop_ffmpeg` ran on the wall clock

**Fixed 2026-10-04 in `timeshift_buffer` 0.8.7, unit-tested.** A system-clock step (NTP, a VM resume) during the two-second grace either SIGKILLed a process that was exiting normally or waited far past it. The deadline, the poll loop and the debug log now use `time.monotonic()`. The cross-worker heartbeat ages are a different matter, see Known gaps.

#### An infinite value in a numeric plugin setting broke every action

**Fixed 2026-10-04 in timeshift_buffer 0.8.7, unit-tested.** `_int_setting()` caught `TypeError`/`ValueError` but `int(float("inf"))` raises `OverflowError`, which a JSON `Infinity` in a setting can produce; it now falls back to the default like the other bad values.

#### Impossible calendar dates were accepted by two parsers

**Fixed 2026-10-04 (the eighth hardening sweep), unit-tested.** `TimeFromIso()`, `TimeFromDateString()` and `ParseXmlTvTime()` range-checked month and day separately, so February 30 or April 31 normalized to a date in the next month. `IsValidCivilDate()` (`TimeUtil.h`, leap years included) now rejects them (returning 0, the parsers' own "unknown"), and `ParseXmlTvTime()` reads a UTC offset after any run of spaces rather than at a fixed column.

#### A hostile or broken server error body went into the log and the user's notification verbatim

**Fixed 2026-10-04 (the eighth hardening sweep), unit-tested.** `Request()` appended the whole HTTP error body to its message, which is logged and sometimes shown. `SanitizeServerErrorBody()` (`JsonResponse.h`) replaces control characters with spaces, cuts at 512 bytes on a UTF-8 boundary and notes how much was dropped.

#### Docs and source comments cited headings that no longer existed, and the checker could not see it

**Fixed 2026-10-04 (the eighth hardening sweep).** Six `docs/OPEN_ITEMS.md's "..."` citations in `src/` pointed at headings that had been renamed or only existed as a multi-line bold paragraph, and three docs still said a segment URL on another host got the API key attached (fixed the same day). `tools/check_doc_refs.py` gains a fourth check for the `docs/X.md's "Title"` form (no trailing "section" needed) over the docs, `src/` and both `plugin.py` files, with comment and paragraph wrapping undone before matching; it is tested in `tools/tests/test_check_doc_refs.py`.

#### Test gaps the eighth sweep's mutation pass found

**Closed 2026-10-04.** Mutations of the existing code that no test noticed now fail one: a cache bigger than the playlist must reseed, `EstimateSegmentDurationMs()` with a zero span, the exact edge of `IsLikelySeekProbe()`, a known zero length in `HasRecordingFileChanged()`, a tab after the colon in `FindWebSocketAcceptHeader()`, a 300 in `ClassifyHlsKeepAliveResponse()`, the guest credit role in XMLTV, and on the plugin side: an unsorted `_dedupe_scan_roots()`, a templated-only `_resolve_scan_root()`, a directory named `x.ts` in `_count_ts_segments()`, `0 segs` versus `? segs`, a scope id in `client_ip` on its own, the zombie state letter, and the pinned ffmpeg log level.

#### A recurring-rule edit re-sent the untouched time an hour off when only the other time changed

**Fixed 2026-10-04 (the ninth hardening sweep), confirmed with a program built on the real sources, unit-tested.** The eighth sweep's `ResolveRecurringRuleTimesOnEdit()` kept the stored times only when *both* instants came back as `GetTimers()` reported them. For a rule whose start sits in a spring-forward gap (shown an hour late) the user moving only the end then had the untouched start re-derived an hour off, and the PATCH moved every future occurrence and regenerated them. Each side is now resolved on its own: a side whose instant is unchanged keeps its stored time of day, whatever happened to the other. The test that asserted the old both-or-nothing behaviour is replaced, with the mirror case added.

#### The background padding retry could put an older value back on the server

**Fixed 2026-10-04 (the ninth hardening sweep), from the code, unit-tested.** `RetryDeferredServerSyncs()` read the parked padding value, then waited for the client's DVR-settings mutex; a direct push that succeeded in between cleared what was parked, yet the retry sent the value it had read before waiting, so the server held an older padding until the next cycle, or for good if Kodi exited first (the next start syncs *from* the server and overwrites Kodi's newer setting). The retry now decides what to send under that mutex (`DispatcharrClient::SetDvrOffsetMinutesChosen()`, `PendingPaddingEdits()`). The startup sync's read had the related race: it ran outside the mutex, so it could return the value an in-flight PATCH was about to replace and write that over Kodi's setting. `GetDvrOffsetMinutes()` now takes the mutex, and `IsSafeToSyncPaddingFromServer()` (an edit parked, in flight (`inFlightEdits`) or begun since the read started) defers the sync to the next cycle. No lock-order problem: the callbacks take `m_paddingPushMutex` under the client mutex and nothing holds it while calling into the client.

#### The tzdata cross-check could fail CI on an older runner image

**Fixed 2026-10-04 (the ninth hardening sweep).** `tools/tests/test_timezone_table_vs_zoneinfo.py` reads the machine's tz database, and a runner whose image predates tzdata 2026c would fail it from 2026-11-01 for no fault of the table. It now skips, saying why, when the database still has Vancouver changing its clocks in 2027 (detected by behaviour, tested with fake zones). A CI that goes red on a stale image is worse than one that skips; the table itself is what the Catch2 transition tests and this check guard on an up-to-date image.

#### The citation checker missed the comma form, and one such citation dangled

**Fixed 2026-10-04 (the ninth hardening sweep).** `POSSESSIVE_CITATION_RE` now also matches `docs/X.md, "..."`, `docs/X.md: "..."` and `docs/X.md's own "..."`; wrapped comment and docstring text is unwrapped with its indentation collapsed (the indentation had ended up inside quoted titles). `src/RedirectPolicy.h` cited the renamed "The API key was attached to a segment URL on any host" heading by its old name.

#### Two concurrent saves of `recording_epg_links.json` could corrupt it

**Fixed 2026-10-04 (the ninth hardening sweep), plausible rather than reproduced (not confirmed that Kodi calls `GetTimers()` and `GetRecordings()` in parallel).** Both call `SaveRecordingEpgLinksIfDirty()`, which serialized under `m_dataMutex` but wrote and renamed outside it through one shared `.tmp` path, so two saves could interleave their bytes or rename the older text in last, and a damaged file is discarded whole on load (links the guide can no longer rebuild). `m_recordingEpgLinksSaveMutex` is held across serialize, write and rename. Threading glue, not unit-tested.

#### Test gaps the ninth sweep's mutation pass found

**Closed 2026-10-04.** About 330 C++ and 21 Python mutations were tried; these survived and now fail a test (each re-checked by applying the mutation again): `continue` to `break` in both `EpgProgramMatch` loops (the match was always the first entry in every test), three series-query operator mutations (OR before a group, the first character before an AND-group, a trailing partial operator), two `PaddingPush` mutations (the retry count restart when a newer value replaces a parked one, the post side of a failed retry), `IsValidCivilDate()` December, a negative hour in `TimeFromIso()` and `ParseXmlTvTime()`, a fourth `HH:MM:SS` field, a malformed five-character XMLTV offset, a key-less channel ahead of a renumbered one, an already-tagged over-long rule name, and in `timeshift_buffer` the reaper's exact idle boundary, an untracked teardown of a restarted buffer that was handed the same pid, and an old directory mtime with a freshly started owner.

#### The `timeshift_buffer` README's manual test always got a 403

**Fixed 2026-10-05 in `timeshift_buffer` 0.8.8 (the tenth hardening sweep), confirmed from the code, unit-tested.** The README said the playlist at `http://<host>:<http_port><playlist_route>` is fetchable after "Start Test Buffer", but the file server answers 403 to any request without `?token=<access_token>` (`_check_access_token()`), and the Plugins page shows only an action's message, which never carried the token. An admin following the documented check always got a 403 and concluded the plugin was broken. "Start Test Buffer" (the only caller with empty params; it falls back to `test_channel_uuid`) now ends its message with the URL including the token, started or reattached; a caller that passes `channel_uuid` (the addon) never gets the token in the message, because that message is logged and the token is already a field of the response. The README says so.

#### The citation checker missed the backtick form and did not read CLAUDE.md

**Fixed 2026-10-05 (the tenth hardening sweep), reproduced.** The possessive-citation check needed `docs/X.md's` with nothing between `.md` and `'s`, so the backtick form `` `docs/X.md`'s "Title" `` (59 of the 83 citations of this kind, mostly in this file) matched nothing; `CLAUDE.md` and `CONTRIBUTING.md` were never scanned for citations (`EXTRA_CITING_FILES`, read only by that check). its heading reader also read a bold lead that wraps across lines ("- **One install's addon can ... stored" / "API key, ...**") as no heading at all, and the title normalizer now ignores quote characters, so a title that contains quotes is matched whichever kind the citation uses. The newly visible findings were triaged: one paraphrased citation was corrected in place (`docs/TROUBLESHOOTING.md`'s "One install's addon can silently invalidate another install's stored API key"), five that cite a phrase inside an entry or a section since removed rather than a heading were baselined, as the earlier ones are.

#### The documented hand rebuild of a plugin zip would have put tests/ back

**Fixed 2026-10-05 (the tenth hardening sweep).** The `CLAUDE.md` bullet on updating a published release's plugin zip said to match what CI produces without giving the new `-x` exclusions, and a hand-built zip is the one path the workflow's contents check does not see. The bullet now quotes the exact command, and `tools/tests/test_plugin_zip_contents.py` fails if the workflow's exclusions and that quote drift apart.

#### A padding push that threw would have deferred the startup padding sync for the whole session

**Fixed 2026-10-05 (the tenth hardening sweep), plausible and very unlikely, glue not unit-tested.** `BeginPaddingEdit()` counts an edit as in flight and only the push's result callback ends it. `SetDvrOffsetMinutes()` always calls it on both success and failure, but an exception out of the request (an allocation failure, a `system_error`) skips it, `RunBackgroundTask` swallows the exception, and `IsSafeToSyncPaddingFromServer()` would then defer the sync from the server until the process ended. The push lambda now reports a failed push if the callback never ran (so the edit is parked for the retry like any other failure) and rethrows.

#### Test gaps the tenth sweep's mutation pass found

**Closed 2026-10-05.** About 660 C++ and 225 Python mutants were tried (one at a time, in scratch trees), plus a 1.3-million-input fuzz of every pure parser under ASan/UBSan, which found nothing. These survivors now fail a test, each re-checked by applying the mutation again: an EDL cut (type 0) read as a commercial break, `stopped_at`/`ended_at` equal to the start, an EPG start exactly the tolerance away, `FieldOr<int>` of the float INT_MIN, an upcoming occurrence ending exactly now, pruning a two-segment live list, `SEEK_END` against a known zero length, a persisted link for channel 0, `IsSameSeriesRuleChannel(0, 0)`, an unsourced old series rule, and in the plugins each of the three conditions of the EDL filename check, a NUL byte in an EDL path, a regular file or an unstat-able entry in the orphan scan, and a `/proc` stat line with nothing after the command name.

#### The plugin zips published the build machine's timezone

**Fixed 2026-10-05 (the eleventh hardening sweep), reproduced.** Info-ZIP's `zip` stores each entry's time twice: a DOS field in local time and a UT extra field (0x5455) in UTC. Built under a non-UTC `TZ` the entry's local and UTC times differed, so anyone downloading the asset could read the build machine's offset, the leak the release gate already describes for the Windows zip. CI's plugin zips were only safe while the runner's clock is UTC (true on GitHub, not guaranteed on a CI runner, which is what exposed the Windows zip on 2026-10-03), and the hand-rebuild documented in `CLAUDE.md` for a plugin-only fix after a release ran the bare command and uploaded it, unchecked. Both now run `TZ=UTC zip -X -r ...` (`-X` drops the extra fields), and `tools/tests/test_plugin_zip_contents.py` runs the workflow's own command under a non-UTC POSIX zone and requires no extra field and a DOS time equal to the file's UTC time (it fails for the bare command, and for `TZ=UTC` without `-X`). The tenth sweep's quote of the command in `CLAUDE.md` is what introduced the unguarded hand path.

#### A pull request could run its own build steps on the Windows runner

**Fixed 2026-10-05 (the eleventh hardening sweep), plausible, depends on repository settings that could not be checked from here.** `build.yml` runs pull-request events, and `build-windows` runs that runs jobs directly on its machine on the machine that also builds the release DLL zip, so a pull request from a fork or an outside account, if CI ran it without manual approval, would execute its own `CMakeLists.txt` and build steps on that machine. The job now has `if: github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository`: a push, a tag, a manual dispatch and a pull request from a branch of this repository still run it. If the event context turns out not to carry the head repository it skips every pull request, which fails closed. The cost is that a fork's pull request gets no Windows compile check from CI. Not exercised on a real fork pull request.

#### CONTRIBUTING.md and CLAUDE.md described a narrower lint and test scope than CI enforces

**Fixed 2026-10-05 (the eleventh hardening sweep).** Both said `ruff` on `dispatcharr-plugin/` only, while the lint job also checks `tools/`, and `CONTRIBUTING.md` listed 26 of about 55 pure-logic modules as the ones that require running the C++ suite. Both now name `tools/`, and the module list is replaced by the rule it was approximating: anything under `src/` except `PVRDispatcharr`, `DispatcharrClient` and `addon.cpp`.

#### Test gaps the eleventh sweep's mutation pass found

**Closed 2026-10-05.** 69 mutations across 11 modules (one at a time, in a scratch copy, full Catch2 run each), most in `WebSocketClient`, which the earlier passes had not mutated; each of these now fails a test, re-checked by applying the mutation again (one of them made the suite hang, which the re-check run treats as a failure): a second message on a connection carrying the first one's text (`m_assembledMessage` not cleared), fragments that each fit but together pass the 10 MiB cap, a back-to-back ping flood with no gap (only the between-frames deadline ends it), `Close()` not forgetting a half-received fragmented message, a negative remaining time in `WaitForSocketReady()` (`poll()` would wait forever), `BoundedWriteCallback()` on a string already over its limit, and 0x1E/0x1F in `SanitizeServerErrorBody()`. The other survivors were equivalent mutants. The pass also checked the redirect policy against libcurl's own URL parser on 3 million adversarial URLs (no disagreement that could send a request to another host, scheme or port) and the offset table against tzdata 2026c at 15-minute steps over 2026 to 2030, about 19 million samples (no mismatch).

#### The `timeshift_buffer` manifest cache only grew in every worker but the one that stopped the buffer

**Fixed 2026-10-05 in `timeshift_buffer` 0.8.9 (the twelfth hardening sweep), confirmed from the code, memory measured, not run against a live server.** `_manifest_cache` holds one entry per channel a worker has answered `get_live_manifest` for, a dict of every visible segment (about 400 KB at the defaults), and the only removal was `_delete_buffer_state()`, which runs in the worker that tears the buffer down. Every other worker kept the entry for the life of the process, so 100 channels surfed was about 40 MB per worker. `_remember_manifest()` now keeps the 16 most recently used channels (`_MANIFEST_CACHE_MAX_ENTRIES`, well above `max_concurrent_buffers`); an evicted channel is rebuilt cold on its next call, which is what a worker that never served it does anyway, so correctness is unaffected.

#### addon.xml.in declared an icon that was never committed

**Fixed 2026-10-05 (the twelfth hardening sweep), confirmed.** `<icon>resources/icon.png</icon>` pointed at a file that has never existed in the history, so every release zip declared an icon it did not contain. The `<assets>` block is removed (add the block back together with a real PNG); `tools/tests/test_addon_metadata.py` fails if any declared asset path is missing, and is itself checked against a missing one.

#### Metadata drift: five headers missing from CMakeLists.txt, and CLAUDE.md still said CI builds macOS

**Fixed 2026-10-05 (the twelfth hardening sweep).** `DISPATCHARR_HEADERS` lacked `CatchupSessionCache.h`, `CatchupSessionRequest.h`, `ChannelLineupChange.h`, `PaddingPush.h` and `SegmentFetchFailure.h` (an IDE-listing list only, the build was unaffected; the same test file now fails on any header in `src/` that is not listed), and `CLAUDE.md` said CI compiles macOS (it has not since 2026-09-28) and left `unit-tests-sanitized` out of its list of CI jobs.

#### CHANGELOG.md had no addon entry for anything since 0.11.0

**Fixed 2026-10-05 (the twelfth hardening sweep).** About 470 commits had landed without an addon entry or an Unreleased heading, and `test_version_sync.py` only requires an entry for the *current* version, so nothing caught it. There is now an addon section headed Unreleased with the user-visible fixes, to be turned into the next release's entry; keeping it current as fixes land is the part no test enforces.

#### Test gaps the twelfth sweep's mutation pass found

**Closed 2026-10-05.** About 670 C++ mutants over 16 modules (zero survivors; most `ChannelParser`/`TimerRuleParser` mutants did not compile, so their field fallbacks got only light coverage) and about 100 Python ones. These survivors now fail a test, each re-checked: in `recording_edl`, `_prune_empty_directories()` stopping at one non-empty or unlistable or unremovable directory, and the sidecar scrub stopping at the first scan root that does not exist; in `timeshift_buffer`, a listed segment whose `stat()` fails dropping every later one, the manifest fast-path key not noticing a same-size playlist rewrite with a new mtime, a trailing `#EXTINF` with no URI, and the contended `retryable` flag of `stop_buffer`.

#### Concurrent requests could crash libcurl through the shared connection cache

**Fixed 2026-10-05 (the thirteenth hardening sweep), reproduced with the real client; the crash threshold on a live Kodi was not measured.** `DispatcharrClient`'s main `CURLSH` shared `CURL_LOCK_DATA_CONNECT`, and `docs/RECORDINGS.md` treated the macOS 1.0.1 crash in that code as specific to macOS's libcurl and the main share as proven crash-free. The sweep compiled the real `DispatcharrClient.cpp` against Kodi's dev-kit headers with about twenty lines of stub Kodi functions, linked it with ASan/UBSan and drove it from 16 threads (`GetChannels`/`GetRecordings`/`InvalidateAccessToken`) against a local fake server: a SEGV inside `curl_easy_perform` on the stock Ubuntu 24.04 libcurl 8.5.0 in 4 of 5 runs, and none in 6 with connection sharing off; a 30-line plain C program with the same lock callbacks crashed 5 of 5 at 8 threads and 3 of 3 at 16, never with sharing off, and not at 3, 4 or 6 threads. The addon can have five to seven transfers in flight at once (the channel/EPG thread, the recording refresh, the realtime reconnect refresh, the stream-read thread with its heartbeat and manifest refreshes, `GetStreamTimes()` polling, the detached padding and `AddTimer()` threads, Kodi's own timer calls), just under the measured threshold, so a crash was possible but rare. The main share now shares DNS and TLS sessions only, like the probe share; the persistent read handles keep their own keep-alive connections, and what is lost is reusing one short request's connection for the next. Re-run after the change: 6 of 6 clean. Which libcurl version fixed the defect is not established.

#### std::tolower and std::isalnum followed Kodi's character locale

**Fixed 2026-10-05 (the thirteenth hardening sweep), plausible, not reproduced (the lab host has only the C locale).** Kodi sets `LC_CTYPE` to the user's region; under a Turkish locale `tolower('I')` is not `'i'` (and in a single-byte locale it can alter the bytes of a UTF-8 sequence), so EPG category keywords such as "series" missed a capital I, and header names (the `Content-Range`/`Content-Length` probes) and host names were lowered locale-dependently. `AsciiToLower()` and `IsAsciiAlnum()` (`StringUtil.h`) now do the case folding and the word-boundary test in `StringUtil`, `CurlCallbacks` and `EpgTagUtil`; `LC_NUMERIC` is pinned to "C" by Kodi, so number parsing was never affected. Tested for every byte, and with a Turkish locale where one is installed (skipped with a warning otherwise).

#### A time test passed without a tz database, and two tests left state behind

**Fixed 2026-10-05 (the thirteenth hardening sweep), confirmed.** `PortableTimeGm ignores the process TZ setting` set `TZ=America/New_York`, which glibc silently reads as UTC where no tz database exists, so a regression back to `mktime()` passed on a bare CI image (shown with `TZDIR=/nonexistent`); it now uses a POSIX zone string and an absolute expected value. The `WaitForSocketReady` test that raised `RLIMIT_NOFILE` restores it, and refuses to dup2 over an already open descriptor 1500. `test_addon_metadata.py` reaches its own module through `sys.modules` (the `__import__(__name__)` it used returns the top package under some import modes).

#### During a server blackhole, live-timeshift Stop, a tail-wait read and the PVR constructor each waited out a full request timeout

**Fixed 2026-10-05 (the fourteenth hardening sweep), reproduced with the glue harness; re-measured after the change.** Against a server that accepts connections and never answers (a firewall DROP), with the 30 s timeout: `CloseLiveTimeshiftStream()` took 30.0 s because it waits on the synchronous `stop_buffer` request, a concurrent `Read()` blocked 30.3 s (past its 25 s outage budget, which is only checked between attempts) because one forced manifest refresh is one full request, and the PVR constructor took two timeouts (the version check, then the login: 10.0 s at a 5 s timeout) before the login backoff gated the rest. `Request()` takes a per-call `timeoutMsOverride`: `stop_buffer` and `get_live_manifest` get 5 s and the startup version check 5 s, `stop_buffer` is skipped once the client is shutting down (the plugin has already dropped the viewer and the idle reaper tears an abandoned buffer down), and a startup version check that gets no response at all starts the same cooldown a failed login does (`DeferAuthenticationAfterUnresponsiveServer()`) instead of waiting out a login of its own, so the background thread's deferred syncs reconnect. Re-measured with the same scenarios at a 30 s timeout setting: Close 5.0 s (was 30.0), the blocked read 10.5 s (was 30.3), the constructor 5.0 s (was 10.0 at a 5 s timeout, about 60 s at the default). A failed *Open* against a hung server still takes a full timeout, which is unavoidable; the in-progress-recording Stop case is the existing open item.

#### An unsizeable segment behind sized ones made a cold open probe the whole tail again on every refresh

**Fixed 2026-10-05 (the fourteenth hardening sweep), reproduced with the glue harness.** A 2000-segment backlog with segment 3 answering 500 cost 6249 HEAD requests in 40 s: the cold open probed all 2000 and merged segments 0 to 2, the unprobeable-segment tracker was not armed (it only ever looked at the leading segment, which had answered), so the next refresh probed the whole tail again and threw the sizes away. `IndexToArmTrackerAfterMerge()` (`UnprobeableSegment.h`) names the first unsized segment behind merged ones and the refresh arms the tracker for it, so the next refresh probes it alone until it gives up; the same scenario now costs 4250 HEADs in 40 s (two full tail passes, the cold open and the one after the give-up, plus the single probes). Keeping the sizes already probed, keyed by URL, would remove the second pass; not done.

#### A recording read that failed for a reason that will not clear made Kodi retry it at once for ever

**Fixed 2026-10-05 (the fourteenth hardening sweep), reproduced with the glue harness for a completed recording whose file starts answering 404.** A read answered with a 403, 404 or 500 (or a file replaced under the stream) returned -1 on every call, and Kodi retries a failed read near-immediately: 226 segment GETs in 9 s in the harness, about 8 per second in Kodi until the player gave up. `PermanentReadFailureTracker` (`SegmentFetchFailure.h`) ends the stream as EOF, once, with a notification and the cause logged, after five such failures spread over at least three seconds (a restarting proxy's burst is not enough), for both `ReadRecordingStream()` and `ReadInProgressRecordingStream()`; a read that delivers bytes ends the run. The same scenario now stops after 134 GETs and a notification.

#### A transient probe failure made an in-progress backlog merge one segment per refresh

**Fixed 2026-10-05 (the fifteenth hardening sweep), reproduced with the glue harness and a regression of the fourteenth sweep's tracker arming.** `IndexToArmTrackerAfterMerge()` read the `-1` of a segment that was never probed as a failed probe: a refresh that probes only the leading segment (it failed last time) and now merges it armed the tracker for the next segment, which then probed alone, and so on. A 400-segment backlog with one 500 on the open's probe merged 22 segments in 11 s (24 playlist GETs) instead of all 400 in 1 s, leaving Kodi's seekable range behind live for minutes on a long recording. The function now takes the probed count and arms only for an index that was probed; `ip_cascade` in the glue harness fails on the old code (3.2 of 5.9 MB after 10 s) and passes.

#### Stop and the live-edge refresh still waited the full timeout in the authentication step

**Fixed 2026-10-05 (the fifteenth hardening sweep), reproduced with the glue harness; corrects the fourteenth sweep's measurement.** `EnsureAuthenticated()` ran its token refresh or login with the configured timeout before the 5 s request began, and the 401 retry inside `Request()` did the same, so once the 4-minute token hint had lapsed (always, within a sustained blackhole) Close took 30.0 s and a tail-wait read 30.0 s. The fourteenth sweep's figures (Close 5.0 s, read 10.5 s) held only with a fresh hint. `EnsureAuthenticated()`, `Login()` and `RefreshAccessToken()` take the same `timeoutMsOverride`, and Stop, the manifest refresh and the 401 retry pass it. `live_blackhole_close`/`live_blackhole_read` (token hint invalidated, API hung) measure 5.0 s and a worst single read of 9.5 s; `live_blackhole_close` fails at 30 s on the old code.

#### An HTTP 500 on a recording read ended playback for good

**Fixed 2026-10-05 (the fifteenth hardening sweep), reproduced with the glue harness.** 500 was not a transient read failure (only 502/503/504 and transport failures were), and since the fourteenth sweep a run of five failures over three seconds ends the read permanently, so a three-second blip of 500s (Django's answer to a transient database or I/O error) ended the recording read although the server recovered at 5.5 s. 500 is transient now: the read retries for its 20 s budget inside one `Read()`. `IP_EXPECT_RESUME=1` makes the in-progress driver fail when the stream has not resumed after the fault clears; it fails without this change.

#### A quick reopen right after a Close whose stop_buffer was cut off could fail as busy

**Fixed 2026-10-05 (the fifteenth hardening sweep), by code reading rather than reproduction.** With Close bounded at 5 s, a server-side teardown that takes longer (SIGTERM grace plus removing a large buffer on slow storage) leaves the buffer mid-teardown for a same-channel reopen, whose retry on the plugin's `retryable` reply lasted 3 s. It now lasts 10 s (20 attempts, 500 ms apart).

#### The unresponsive-server timeout changes, checked live

**Confirmed live 2026-10-05, with a real Kodi on the Linux test VM against the lab Dispatcharr through a forwarder that could stall, delay or fail the API port (the plugin's file-server port always forwarded).** The built addon (fourteenth and fifteenth sweep changes) behaved as the glue harness predicted: with the API hung and playback running, every `get_live_manifest` and `stop_buffer` request was given up on after 4.8 to 4.9 s; the 4-minute token hint lapsed during a five-minute hang and the token refresh was given up on after 4.85 s (30 s before the fix), with the next attempt one 30 s cooldown later, and playback kept running on its buffer throughout; a Stop during a hang cost two bounded requests, about 10 s, instead of about a minute; with 3 s of added latency on every API request playback, Stop and the manifest refresh all worked; and with Kodi started while the API accepted connections and never answered, the version check gave up after 4.87 s, Kodi's JSON-RPC answered 4.2 s after launch, and once the server answered the first channel fetch followed 5.2 s later. It also found the next entry. Not run live: the in-progress recording 500 blip and backlog cascade (glue scenarios only), and a real firewall DROP (the forwarder stalls the same way).

#### With 6 s of API latency a live stream could not be opened

**Fixed 2026-10-05, found by the live check above and reproduced with the glue harness (`live_slow_open`).** The short bound on the live manifest refresh also applied to Open's wait for the first segment, so against an API that merely answers slowly (6 s per request, well under the configured 30 s) every cold-start attempt timed out and the open failed after about 165 s (182 s in the harness); the old 30 s timeout would have succeeded. `RefreshLiveManifest()` takes `coldStart`, which `OpenLiveTimeshiftStream()`'s cold-start loop sets: that refresh, and the authentication in front of it, use the configured timeout. Re-measured: the harness opens in 18 s, and live, with 6 s of latency on every request, playback starts after about 47 s and advances. Steady-state behaviour at that latency is the next entry.

#### Stopping an in-progress recording during a server blackhole waited about two minutes

**Fixed 2026-10-05, reproduced with the glue harness (`ip_blackhole_close`); the bounded requests were confirmed live the next day (see the entry on that check).** Kodi closes a stream only after its read thread has left `Read()`, and a read waiting at the tail of an in-progress recording ran one refresh per attempt, each a recording lookup, a playlist fetch and a keep-alive HEAD of the full request timeout (140.7 s measured live at the 30 s default; 62.8 s in the harness). A stream that already has segments now gives each of those requests, and the authentication in front of the lookup, a 5 s bound (`kInProgressRefreshRequestTimeoutMs`; a cold start keeps the configured timeout), and a refresh whose lookup got no answer at all skips the playlist fetch, since the server is then very likely gone. The worst blocked read in the harness is 13.0 s. Letting Close abort its own stream's requests, the other option the entry listed, was not needed: Close does not wait for a refresh (it only waits for a segment read in flight, which is bounded by its own retry budget), and aborting the requests of a read that Kodi is waiting on would not shorten that wait further. The sustained-latency consequence is the known gap on API latency above 5 s, which now covers this path too.

#### A server that ignores `Range` made a recording read download the whole file to throw it away

**Fixed 2026-10-05, reproduced with the glue harness (`rec_range_dropped`).** A ranged recording read against a proxy that drops `Range` was answered with the whole file, which the read received to the last byte or the request timeout while keeping only the bytes that fit: 12.1 s for a 3 MB file sent slowly in the harness, up to the 30 s timeout (and gigabytes of traffic) for a real recording. `FixedBufferSink` gained `abortWhenFull`/`truncated`, `ReadRecordingStream()` ends the transfer when the buffer is full and treats that as success, and the read at a non-zero offset now reaches `ServerIgnoredRangeRequest()` and ends the stream with its message after 0.3 s. Playback past the first read of such a server is still impossible by design: skipping to the offset would mean downloading everything before it on every read, so this entry's title stays true; what is gone is the wasted download and the 30 s wait. Dispatcharr itself honours `Range`.

#### `ffmpeg.log` could grow without bound while a buffer ran

**Fixed 2026-10-05 in `timeshift_buffer` 0.8.10 (unit-tested; deployed to the lab 2026-10-06).** The reaper (the elected worker, every 15 s) trims each tracked buffer's `ffmpeg.log` to its last 256 KiB, started on a whole line behind a one-line marker, once it passes 16 MiB (`_cap_ffmpeg_log()`). ffmpeg holds the file open with `O_APPEND`, so truncating in place is safe: its next write lands at the new end (tested with a writer that stays open, no sparse hole). The last lines are kept because they are what an "ffmpeg exited" error quotes.

#### `max_concurrent_buffers` could be exceeded by starts on different channels, and stale ffmpeg wording

**Fixed 2026-10-05 in `timeshift_buffer` 0.8.10 (unit-tested, and confirmed live 2026-10-06).** The count of running buffers was taken under the per-channel start lock only, so two channels starting at the same moment each saw room. Counting and registering a new buffer now happen under one short global lock (`_START_SLOT_LOCK_ID`, the same `SET NX EX` primitive as the per-channel lock; a start that finds it taken answers `retryable`, which the addon's reopen retry already handles), tested with a start for a second channel arriving while the first is mid-spawn. The `segment_seconds` help text names `-hls_time`, and the stale `-segment_time`/`-segment_wrap` comments in the plugin and `LiveEdgeMargin.h` are corrected (the historical mentions that say "the old" stay).

#### The in-progress blackhole bound, the ranged-read abort and plugin 0.8.10, checked live

**Confirmed live 2026-10-06 (a real Kodi on the Linux test VM and the lab Dispatcharr, through the forwarder of the earlier live check, with disposable recordings that were deleted afterwards and verified gone).** the 0.8.10 timeshift plugin was imported into the lab: with `max_concurrent_buffers` set to 1 temporarily and five channels started at the same instant, twelve rounds produced exactly one running buffer per round (12 ok, 45 retryable from the new start-slot lock, 3 refused at the limit) and no round over the limit; the setting was restored afterwards. For an in-progress recording with the API port stalled, the recording lookup, the keep-alive HEAD and the token refresh were each given up on after 4.85 to 4.87 s (30 s before), the player was gone 6.0 to 7.5 s after Stop in three runs, and playback had kept running through the stall. Normal recorded playback with the ranged-read abort in place was healthy: 92 ranged GETs of one completed recording, forward and backward step seeks landing where expected, playback advancing after them, and Stop followed by a resume working, with no addon error in Kodi's log. Not reached live: a reader waiting at the very tail during the stall (Kodi's step seeks never got closer than about 160 s of a growing 285 s recording, so the three runs were mid-recording, one of them inside a segment download), a proxy that really drops `Range`, and the `ffmpeg.log` trim (the plugin has no shell access here; it is unit-tested with a writer that stays open). A segment body download that hangs still waits the configured timeout (29.9 s seen): a stalled transfer is not bounded separately from a slow one, and the plugin-independent retry budget makes a stall in the middle of a read cost one such wait; the glue harness covers the tail case (13.0 s worst blocked read).

#### A stopped recording with no file failed to play with only Kodi's generic dialog

**Fixed 2026-10-06 (the design decision was approved: keep listing the row, say why it cannot play).** Dispatcharr can hold a recording row in a terminal state with no file at all (one met in the lab: an instant recording stopped 84 minutes after its start that never produced anything), whose `/file/` endpoint answers 404 for ever. The row stays listed, since hiding it would also hide it from Delete, and opening it now queues "Dispatcharr has no file for this recording. It may have been stopped before anything was recorded, or its file was removed." (`DescribeRecordingOpenFailure()`, `RecordingHttpUtil.h`, unit-tested; a 403 gets its own line, every other status keeps Kodi's generic dialog). `rec_open_missing` in the glue harness prints the notification. Not exercised in a real Kodi: the lab row it was found on is no longer there.

#### Kodi was cloned from a moving branch in the release builds

**Fixed 2026-10-06 (the first half of the entry; the second half is below).** Both build jobs now fetch the exact commit named by `KODI_COMMIT` in `build.yml` (the head of `Omega` the by-hand and glue builds were verified against, `f8815ee`) with an `init`/`fetch --depth 1`/`checkout` sequence, instead of `git clone --branch`. `tools/tests/test_dependency_pins.py` checks that it is a full SHA, that both jobs fetch it, and that no job clones Kodi by branch. Confirmed on CI: the Unix and the Windows job (whose pwsh steps check `$LASTEXITCODE` after each git call) built from it. `docs/BUILDING.md` says CI builds the pinned commit.

#### ThreadSanitizer had never been run on the C++ suite

**Fixed 2026-10-06.** The Catch2 suite passes under TSan with no warning (1250 cases, locally and on the CI runner), and `unit-tests-tsan` in `build.yml` now runs it on every build: no-PIE, `-O1`, a run loop that retries when the runtime refuses the address-space layout, a failure when it never reaches Catch2's summary in 40 attempts. The first CI attempt failed for a reason that is worth keeping: Catch2's test discovery ran the freshly linked executable as a build step to list the tests, and under TSan that run segfaulted on the runner, failing the build before the loop was reached; discovery is `PRE_TEST` now (`tests/CMakeLists.txt`), and the job then started on its first attempt. What this covers is only what the suite exercises: the one threaded fixture is `WebSocketClient`'s local-server test, plus the pure helpers. The real `DispatcharrClient`/`PVRDispatcharr` threading is covered by the by-hand harness in `tests/glue/` under TSan (no report), which is the known gap that it is not in CI.

#### The glue harness now runs in CI

**Fixed 2026-10-06.** A `glue-harness` job in `build.yml` fetches only the dev-kit headers of the pinned Kodi commit (a sparse, blob-filtered fetch of well under a megabyte), builds the harness under ASan + UBSan and runs the nineteen scenarios of `tests/glue/ci.sh` (every stream path, the hung-API and slow-API cases, the probe cascade, the 500 blip, the PVR surface): 4 min 14 s end to end on CI, all passed. A scenario name the drivers do not know is now a failure instead of a quiet pass (a typo in the list would have gone green). What stays by hand: the TSan variant of the harness, `ip_chaos`, the plugin integration run (it needs ffmpeg and a longer budget) and the longer fault matrices; `tests/glue/README.md` says which.

#### The recurring-rule display took the end's offset from the start instant

**Fixed 2026-10-06.** `ComputeRecurringRuleDisplayTimes()` resolves the end time's offset on its own (the start's offset as the first guess, then the same fixed-point refinement), so a rule whose first day is a clock-change day and whose start and end fall on opposite sides of the change shows its real end: a 01:30 to 03:30 rule on a spring-forward day is one hour long, a 00:30 to 03:00 rule on a fall-back day ends at 09:00 UTC, an overnight rule across the change keeps its real length. Both sides already resolved per side on edit, so display and edit now agree. Unit-tested with spring and fall resolvers (the old single-offset answer was an hour off in each), and a mutant that skips the end's refinement fails three assertions.

#### Cross-worker buffer timestamps came from each host's wall clock

**Fixed 2026-10-06 in `timeshift_buffer` 0.8.11 (unit-tested, deployed to the lab afterwards; the plugin integration scenarios against the fake Redis pass).** `last_heartbeat`, `viewer_heartbeats`, `stopping_since` and the reaper's idle checks stored `time.time()` in Redis and compared it in other workers. `_shared_now()` now reads Redis's own `TIME` for every one of them, and falls back to the process's wall clock when Redis cannot answer (not reachable, or a stand-in without the command), which is what they always were; directory mtimes, compared with the local filesystem, stay on the local clock. Fifteen call sites were moved and each was mutated back to `time.time()` in turn to check that a test fails: the first pass found seven that nothing pinned, tests were written for them, and the last pass left none standing but the file server's per-request heartbeat, which now has its own test over a real loopback request. The plugin integration run's fake Redis answers `TIME` too.

#### The 5 s bound on Stop and steady-state refreshes was a constant

**Fixed 2026-10-06 (the known gap on sustained API latency above 5 s).** `ShortRequestTimeoutMs()` (`RequestTimeout.h`, unit-tested) derives the bound from the configured connection timeout: a sixth of it, at least 5 s, at most 30 s, never more than the timeout itself. At the default of 30 s that is the 5 s of the fourteenth and fifteenth sweeps, so nothing changes for anyone who did not touch the setting; a deployment whose server answers slowly (the case that broke: 6 s on every request) raises the one setting it already has, and the bound follows (60 s gives 10 s, 180 s gives 30 s). No new setting: the help text of "Connection timeout" now says what else it scales. Every use of the old constants (Stop, the authentication before it, the live-edge refresh, the in-progress steady-state requests, the keep-alive, the startup version check) goes through it; an open still waits the whole timeout. The glue harness measures it at both ends: the hung-API Close scenario of the glue harness asserts the Close time sits inside the bound (about 5 s at a 30 s timeout, 10 s at 60 s, so a fixed bound fails it from either side), and `ci.sh` runs it at 60 s too. A mutant of each clause of the function fails its tests, except the one that is equivalent.

#### The offset table assumed the server's tz data was 2026c or newer

**Fixed 2026-10-06 (the table is now checked against the server instead of trusted).** `ServerOffsetCrossCheck.h` reads the UTC offset Dispatcharr actually applied from a recurring rule's stored local start time and the UTC start of an occurrence it materialized (unpadded, from the rule's own start), and `GetTimers()` uses it when it contradicts the table for the configured zone: `EffectiveRecurringRuleUtcOffsetMinutes()` answers with the server's offset, with a warning in the log and one notification per distinct offset, and the table takes over again as soon as the occurrences agree with it. The details that matter are in `docs/RECURRING_RULES.md`'s new section: the offset is the one nearest the table's (a time of day fixes it only modulo a day), samples within a day of a clock change are skipped, one agreeing sample wins and disagreement needs every sample to name the same other offset. Unit tests (the offset recovered for every real offset from -12 h to +14 h at several instants, and each verdict rule), and the glue scenario `offset_cross_check` fails by exactly six hours without the wiring. Not live-checked: no server disagrees with the table yet (the first day one can is 2026-11-01).

#### Realtime channel/EPG refresh events were dropped

**Fixed 2026-10-06 (the last known gap).** The scope the entry set when it was parked was followed: the real event sequence was captured first (a passive listener on the lab's realtime socket, then an EPG source and an M3U account refreshed on demand, the same thing their scheduler does every three hours), `docs/EPG.md`'s new section records both, `ClassifyRefreshEvent()` (`RefreshEvents.h`) is the pure decision, and the reactions reuse existing machinery. A finished EPG refresh (`parsing_programs`, progress 100, status success; the earlier `parsing_channels` success is a step, not the end) schedules one guide fetch 330 s later through `m_epgRefetchDueAt`, past Dispatcharr's own 300 s cache of the exported guide, with several sources coalesced into the pending fetch; a finished M3U refresh whose channel auto-sync created, updated or deleted a channel (the counts are in the same final event) ages the channel list to just-stale and wakes the background thread. A refresh that changed nothing, a failed one and every progress event cost nothing. The "cost to watch" in the entry (a guide fetch is large) is bounded by construction: at most one fetch per 330 s window. Unit tests use the real event shapes with the numbers replaced (every mutant of the classifier and the schedule fails one), and the glue harness gained a minimal WebSocket in its fake server and `refresh_events` (it fails when the wiring is removed). `docs/MANUAL_TESTING.md` has the check a person runs. **Confirmed live the same day** (a real Kodi on the Linux test VM, realtime updates on, through a plain TCP forwarder so the WebSocket passes): an EPG refresh of the lab's XMLTV source finished on the server, the addon logged it at once, and the guide was downloaded again 392 s after the event (the 330 s schedule plus the background thread's one-minute check while a fetch is pending), against the 4-hour window it replaces; an M3U refresh was logged and the channel list was fetched 11 s later, finding a lineup change and resyncing Kodi.

### Closed without a change (refuted, explained or harmless)

#### Pinning pytest's own dependencies was tried and reverted

**Closed without a change 2026-10-05 (the seventh sweep's "unpinned transitive dev dependencies" item).** `dispatcharr-plugin/requirements-dev.txt` pins only `pytest`; its dependencies (`pluggy`, `iniconfig`, `packaging`, `Pygments`) float. Pinning them to the versions CI was verified with failed the very next run: the CI runner's image has some of them installed by the OS package manager, and pip cannot replace them ("Cannot uninstall packaging 24.0, RECORD file not found"), so the whole Python job failed before running a test. The pins were removed again, with a comment in the file saying why. The exposure is CI hygiene only (a dev-only job, not a release artefact); a real fix would be a virtualenv in the job (`python -m venv`, then the pinned install) so nothing system-installed is touched.

#### An addon older than the ownership tag keeps renewing rules the current one leaves alone

**Observed live 2026-10-03, explained, no change needed in this addon.** While
checking the recurring-rule cache-miss case (see "A recurring-rule edit skipped
its end-date checks on a cache miss"), a disposable rule created with a 3-day
`end_date` and no `[Kodi]` tag was found with a 30-day one minutes later, and a
second one went the same way while watched. The Linux client under test had
not renewed it (its build only renews an owned rule, `ShouldRenewRecurringRule()`);
a Windows client, still on a build from before 2026-10-02, had -- its log
showed "renewed recurring rule <id> end_date forward" for both, on its own
5-minute cycle. That is exactly the pre-ownership behaviour `ManagedRecurringRule`
replaced: every enabled rule inside half its window was renewed, owned or not.
Consequence for a real account shared by installs of different versions: a
finite rule a user made in Dispatcharr's own web UI, or a rule the current build
deliberately leaves to expire, stays alive for as long as any pre-2026-10-02
install on that account keeps running. Nothing the current code can do about an
older client's PATCHes; the remedy is upgrading every install. Noted here so the
next live test of rule end dates stops that client first.

#### DVR Output Profile (transcoded recordings) vs. in-progress playback

**Dispatcharr `0.31.0`'s new DVR Output Profile setting (recordings can
now be transcoded instead of always raw-copied) is untested against
this addon's in-progress-recording read path, flagged from the
release notes (2026-09-19), not yet reproduced.** Everything this
addon assumes about a recording's on-disk shape --
`M3u8SegmentParser`'s `#EXTINF`/segment-URI scan,
`RefreshInProgressRecordingManifest()`'s append-only-merge convention,
the HLS-segments-then-finalized-file lifecycle documented in
`docs/RECORDINGS.md` -- was confirmed live only against Dispatcharr's
previous raw-copy-only DVR behavior. If a user sets a transcoding
output profile for DVR, the actual container/segment shape reaching
this addon while a recording is still in progress could differ in
ways nothing here has ever been exercised against (a real behavior
change on Dispatcharr's side, not something this addon's own code
changed to trigger). Needs a real live test once this addon is
actually run against a `0.31.0`+ instance: record with a non-default
DVR Output Profile set, confirm in-progress playback, seeking, and
`recording_edl` markers still work the same as the raw-copy case.
Not urgent -- the default (unset) still keeps the old raw-copy
behavior, so nothing breaks for a user who never touches this new
setting -- but worth checking deliberately rather than discovering it
from a bug report.
**Update: confirmed live and closed out (2026-09-20).** Tested against
a real `0.31.0` instance with `dvr_output_profile_id` set to
Dispatcharr's built-in "Media Server (AC3 Audio)" profile (video-copy,
audio transcoded to AC3 -- confirmed via `GET /api/core/outputprofiles/`
that both of Dispatcharr's own built-in profiles are audio-transcode-
only, not full video re-encodes). Created a real instant recording
under that profile on a genuinely live channel; confirmed via
`tools/kodi_smoke_test.py`'s own `_play_recording_and_verify()`/
`_seek_within_recording_and_verify()` (driven directly against Kodi's
JSON-RPC, targeting the specific new recording rather than relying on
auto-discovery) that in-progress playback, in-progress seeking, and
playback after the recording completed (`status: completed`,
`remux_success: true`) all work identically to the raw-copy case --
no addon code changes needed. `recording_edl`/comskip markers weren't
separately re-verified (a 5-minute test clip has nothing for comskip
to mark), but nothing in that path depends on the output profile
either. One real false alarm along the way, worth remembering: a first
attempt failed because the test channel picked had no live stream data
at all (Dispatcharr's own `no_stream_data` interruption, unrelated to
the output profile), and a second, seemingly genuine failure on a
genuinely-live channel turned out to be an unrelated wedged Kodi session
(`Player.Open` silently stuck for *any* channel/recording, live or not)
that a hard reset and Kodi restart fully resolved -- neither
was a real DVR-transcoding compatibility bug. Test recordings and the
output-profile setting were cleaned up/reverted afterward.

#### ~89.4 s audio-sync-error reading on fresh stream opens

**A consistent ~89.4s audio-sync-error reading on fresh stream opens,
harmless -- mechanism found, closed (2026-09-08/09).** Seen independently on Windows and Linux (addon 0.9.0 on both), clustered right around -89,400 to
-89,500ms, only near the start of a stream and never recurring. No
playback impact on either machine. Purely informational -- not
chased further. The leading guess (the buffer's own configured
visible-window duration, `visible_segments * segment_seconds`,
coincidentally landing in this range) was checked live against
`timeshift_buffer`'s real source (2026-09-09) and ruled out: that
formula always reduces to just `buffer_minutes * 60`, three orders
of magnitude too large for any realistic buffer setting to land near
89-90s.
**Update: mechanism found, via Kodi-core's own source (2026-09-09).**
Reproduced a fourth time (Windows). `ActiveAE.cpp`'s `large audio
sync error` warning logs the *raw, uncapped* clock-vs-PTS difference
before it gets clamped to a sane bound for actual use -- right at
stream open, before the audio clock locks onto a stable reference,
that raw value can transiently read something enormous, then never
recurs for the rest of that session. Addon-independent: this is
Kodi-core's own audio clock bootstrap, not anything this addon does.
See `docs/TIMESHIFT.md`'s section of the same name for the detail.

#### Periodic ~8.6 s ActiveAE::SyncStream error spike

~~A periodic, self-correcting `ActiveAE::SyncStream` error spike
on a suspiciously exact ~8.6s cadence~~ -- **Closed: not actually
periodic, confirmed on both platforms that ever saw it
(2026-09-08/09).** Originally found by the macOS peer during what
looked like ordinary steady-state playback. Tested on Windows (14
min, zero seeking): one occurrence total, landing 4 seconds after
that session's own ~89.4s large-sync-error transient at the same
fresh `Player.Open` -- not periodic. Re-tested on macOS itself (21.6
min, zero seeking, addon 0.9.1): same result, one occurrence ~3.9s
after buffer open, nothing else. The peer also traced their original
observation to a mislabeled sample -- those log lines actually landed
shortly after a batch of scripted live-edge-seek attempts, not during
clean steady-state; each clamped-to-tail seek forcing its own small
demuxer-resync transient plausibly explains the "recurring" look
without any genuine time-based periodicity. Same family as the
~89.4s transient above: a one-off clock-settling blip at stream
open, not a recurring issue. See `docs/TIMESHIFT.md`'s "A periodic,
self-correcting ~8.6s `ActiveAE::SyncStream` spike" section for the
full account.

#### Unexplained anomaly from the macOS pass

~~A genuine, unexplained anomaly from the macOS pass~~ -- **Likely
already explained and fixed the next day, entry just never
reconciled (2026-09-08 review).** Original note (2026-09-05):
`kodi.log` showed a full `UpdateClients: Recreating PVR client`
(clean DLL unload/reload, no crash, playback fine right after)
immediately after dismissing a resume-prompt dialog, with no
settings change involved -- ruling out the known beta.2
spurious-restart bug (settings-write-triggered), and not
reproducing on a second attempt. One day later (2026-09-06), a separate macOS session hit the identical log signature
-- `UpdateClients: Recreating PVR client`, clean reload, no crash --
this time while opening an in-progress recording, and that one *was*
root-caused and fixed: `OpenRecordedStream()`/`ReadRecordedStream()`
occasionally self-heal a near-expiry API key mid-call and persist it
via `SetSettingString()`, which loops back through
`OnAddonSettingChanged()` and (correctly, by that guard's own logic)
requests a restart, tearing down the stream that had just opened.
See `docs/RECORDINGS.md`'s "A self-heal API-key regeneration..."
section for the full account. Dismissing a resume-prompt dialog for
a recording leads directly into `OpenRecordedStream()` -- the exact
vulnerable call site -- and "no settings change involved" is exactly
how a silent, addon-internal self-heal write would look from a
tester's side; not reproducing on a second attempt also fits, since
the trigger depends on the API key's natural expiry timing rather
than firing every time. Not provable with certainty (the original
log's own API-key-regeneration line isn't preserved to check
directly), but strong enough that this shouldn't still read as an
open mystery. Left here, corrected, rather than deleted, for the
same reason the original was kept: useful context if anything like
it resurfaces.

#### Catch-up and recurring-timer creation not drivable via JSON-RPC alone

~~Catch-up and recurring-timer creation have no clean way to exercise
via Kodi's JSON-RPC alone~~ -- **Decided: counted as tested anyway, not
worth building a dedicated driving mechanism for.** The underlying
logic is platform-independent and already has real live-verification
from earlier sessions; the gap is in this testing technique
(JSON-RPC-driven, not GUI-driven), not in the feature.

#### Catch-up sessions cut short by up to 59 s from rounding

**Suspected: catch-up sessions may be cut short by up to 59s from
rounding, flagged from a project-wide review (2026-09-26), not yet
reproduced.** `PVRDispatcharr::GetEPGTagStreamProperties()` computes
`durationMinutes` from a programme's start/end time via plain integer
division (`/ 60`), which rounds down -- a 90m30s programme requests
`duration=90`, not 91. `DispatcharrClient::CreateCatchupSession()`
then drops that field entirely when it's `<= 0` and caps it at 480.
Contrast with `TimerIdentity::ComputeRecordingExtendMinutes()`, which
deliberately rounds up for exactly this reason. Needs a live check
before treating this as a real bug: does Dispatcharr's own `duration`
parameter actually bound how long the archive/catch-up stream stays
playable, or is it just informational? If it's load-bearing, round up
the same way `ComputeRecordingExtendMinutes()` does.
**Update (2026-09-26, an 18th-pass audit): refuted, not a real
problem.** Confirmed against Dispatcharr's own real current upstream
source (cloned into a scratchpad, never committed to this repo -- a
real source read, not a live test): `apps/timeshift/helpers.py`'s
`client_duration_to_window()` adds a `DURATION_BUFFER_MINUTES = 5`
buffer to whatever duration the client sends
(`min(minutes + DURATION_BUFFER_MINUTES, MAX_DURATION_MINUTES)`) --
five full minutes comfortably absorbs the addon's own up-to-59-second
rounding-down error, so the catch-up stream's own playable window is
never actually cut short by this.

#### 500 ms manifest refresh throttle never engages during sustained failure

**`RefreshInProgressRecordingManifest()`/`RefreshLiveManifest()`'s own
500ms throttle never actually engages during a sustained failure,
flagged from a 12th-pass audit (2026-09-26), low severity, deliberately
not fixed blind this pass.** `lastManifestFetch` (what
`ShouldThrottleRefresh()` checks) is only updated on a *successful*
return -- every failure path, including a recording whose HLS
directory has genuinely 404'd, leaves it untouched, so every later
`GetStreamTimes()`/`Length()` poll (Kodi keeps calling these for as
long as it displays a stream, independent of whether playback itself
still needs anything new) repeats the full `GetRecordingById()` plus a
playlist fetch that will keep failing identically, with no throttling
at all. A short-circuit was drafted and reverted this same pass once
it became clear it would conflict with a real, already-fixed
correctness guarantee: `m_inProgressRecordingStream.finished` is
deliberately allowed to flip back to false if a late segment appears
after a false-positive "finished" (the pass-11 cache-reseed fix's own
reasoning depends on this function still actually running the
playlist fetch/probe logic even after `finished` was once true) --
short-circuiting on `finished` alone would silently disable that
mechanism, permanently losing any content Dispatcharr writes after
this addon first (possibly wrongly) calls a recording finished. Also
worth noting: `GetStreamTimes()` itself already polls at roughly the
same ~500ms cadence this throttle uses, so even a correctly-engaging
throttle would only meaningfully reduce load in the sustained-failure
case, not the ordinary one. A real fix needs to update
`lastManifestFetch` on at least the failure paths that are
*definitively* terminal (e.g. the confirmed-404-and-not-in-progress
case), not blindly on every failure, and should be verified against a
live sustained-404 scenario rather than assumed correct from reading
the code alone.
**Update (2026-09-30):** the definitively-terminal case this calls for now
exists -- a sticky `contentGone` flag, set once the server has confirmed a
recording deleted or finished-with-its-directory-removed, makes every later
refresh return at once (see the "In-progress recording deleted entirely loops at the tail" entry). A
*transient* failure (a 5xx, a transport error) still never updates
`lastManifestFetch`, so the throttle still doesn't engage there -- left as
it was deliberately, since a transient failure is exactly where retrying
promptly is wanted.

**Closed without a change (2026-09-30).** The entry's own 2026-09-30
update already settled it: the sticky `contentGone` flag is the
definitively-terminal case its last sentence asked for, and a transient
failure (a 5xx, a transport error) is exactly where retrying promptly is
wanted. Arming the throttle on those would only cap the failure rate at the
~2 polls a second `GetStreamTimes()` already makes, which is the rate the
throttle itself allows -- no reduction worth the risk to the non-sticky
`finished` mechanism the entry describes.

#### Series rule title over 1023 bytes duplicates on edit

**A series rule whose title is over 1023 bytes still gets a duplicate
rule created on any Kodi-side edit, flagged from a 66th-pass audit
(2026-09-27), not fixed this pass (confirmed by code trace against
Kodi's own real current SDK source and Dispatcharr's own real current
upstream source, not reproduced live).** The same pass fixed the
one-time-recording half of this class (`ShouldRenameOnTimerEdit()`'s
new `kKodiTimerTitleMaxBytes` guard, `TimerIdentity.h`) and the
crash half (`TrySerializeJsonBody()`, `JsonFieldUtil.h`): PVR_TIMER's
`strTitle`/`strEpgSearchString` are fixed `char[1024]` buffers filled
via a byte-level `strncpy(..., 1023)`, both by `GetTimers()`'s own
`SetTitle()`/`SetEPGSearchString()` and by Kodi's own
`CPVRTimerInfoTag::FillAddonData()`. A series rule's `title` has no
length limit server-side (Dispatcharr stores rules as a plain JSON
list in `CoreSettings`), so a rule title over 1023 bytes comes back
into `UpdateTimer()` truncated, `ResolveSeriesRuleMatchTitle()` sends
that truncated string to `CreateSeriesRule()`, and Dispatcharr's own
upsert (keyed by `(tvg_id, title, epg_source_id)`,
`SeriesRulesAPIView.post()`, `apps/channels/api_views.py`) finds no
match and appends a second rule matching on the truncated prefix,
leaving the original in place. (If the cut lands mid-UTF-8-sequence,
the new `TrySerializeJsonBody()` guard now fails the request cleanly
instead.) Not fixed: a 1000+-byte series match pattern is far less
plausible than a long recording title (which Dispatcharr's own
`update-metadata` action makes easy to create), and the real fix
belongs with the already-logged "Editing a series rule's match pattern creates a duplicate" entry -- round-tripping the rule's real identity
instead of Kodi's own copy of its title -- rather than a
truncation-specific patch here. A recurring rule's `name` can't hit
this at all: `RecurringRecordingRule.name` is `max_length=255`
characters (`apps/channels/models.py`), at most 1020 bytes of UTF-8.

**Closed (2026-10-02) by decision: leave as documented.** A series match pattern over 1000 bytes is far less plausible than the long recording titles already handled, and the failure is a duplicate rule, not data loss.

#### DeleteTimer(): design option for a known in-progress recording

**A separate, unrelated design option for `DeleteTimer()`'s
*known*-in-progress case (not the case in "DeleteTimer(): failed-but-not-confirmed-gone lookup"), flagged
from a 51st-pass audit (2026-09-27), not implemented, needs a design
decision.** `DeleteTimer()` currently silently calls `StopRecording()`
whenever `ShouldStopInsteadOfDelete()` decides a recording is actually
in progress, with no user-facing confirmation of any kind. Kodi's own
SDK defines exactly this case: `PVR_ERROR_RECORDING_RUNNING` ("a
recording is running, so the timer can't be deleted without doing a
forced delete", `pvr_general.h`) -- returning it here instead maps to
`TimerOperationResult::RECORDING` (`CPVRTimerInfoTag::DeleteFromClient()`,
`PVRTimerInfoTag.cpp`), which `CPVRGUIActionsTimers::DeleteTimer()`
(`PVRGUIActionsTimers.cpp`) surfaces as an explicit "This timer is
still recording. Are you sure you want to delete this timer?"
confirmation, retrying with `forceDelete=true` (routing to Stop here,
same as today) only on a yes -- confirmed against Kodi's own real
current SDK source, not itself independently reproduced. Trade-off:
an explicit confirmation instead of a silent Stop, at the cost of
JSON-RPC callers (`PVR.DeleteTimer`/`PVR.ToggleTimer`,
`PVROperations.cpp`) treating that same return as a plain
`FailedToExecute` rather than success -- so this could regress any
external automation driving this addon over JSON-RPC (Kodi's own
webserver, HTTP API, a remote/companion app) that currently expects a
clean success here. Possibly an input to the lookup-failure design
decision just above rather than fully separate, but reachable
independently of it (a *confirmed* in-progress recording, not just an
ambiguous lookup) -- not implemented this pass pending that decision.

**Closed (2026-10-02) by decision: keep the silent Stop.** Live-confirmed 2026-09-30 to keep the recording; returning `PVR_ERROR_RECORDING_RUNNING` for a confirmation would make JSON-RPC callers see a plain failure and could break automation.

#### Live-manifest newest-segment re-stat correction doesn't reach the addon

**The timeshift_buffer live-manifest's own "always re-stat the newest
segment" self-correction doesn't actually reach this addon, flagged
from a 27th-pass audit (2026-09-26), confirmed against this addon's
own code, uncertain real-world impact, not reproduced live.**
`RefreshLiveManifest()`/`ParseNewLiveManifestSegments()` only ever
take sequences newer than the last one already merged, and never
revisit a segment's own `byte_size` once merged (`DispatcharrClient.cpp`'s
own "an already-known segment's size can't legitimately change"
comment) -- so a size the plugin's own later re-stat corrects (its own
fast-path/changed-path re-stat logic) never reaches the addon at all;
whatever this addon sampled first is permanent for that segment. The
only real protection this addon actually has is its own
`Content-Range` cross-check, which ends the *entire* live-timeshift
session as fatal on a mismatch rather than gracefully absorbing a
corrected size. Matters only if the underlying stat-visibility lag the
plugin's own re-stat logic exists to catch is actually real in
practice (e.g. more pronounced on a network filesystem) -- ffmpeg's
own `segment.c` confirms the sequence-numbering premise this whole
mechanism relies on is sound (`#EXT-X-MEDIA-SEQUENCE` stays monotonic
across `-segment_wrap`). At minimum, `docs/TIMESHIFT.md`/the plugin's
own docstring claims about this being a "self-correcting" mechanism
should be corrected to note this addon-side gap -- not done this pass.
A real fix (the plugin holding back an unverified tail segment before
reporting it, or the addon accepting a size update for an unread tail
segment) is a bigger design decision, not attempted blind.

**Update (2026-09-30): the docs half is done, the design question stays open.**
`docs/TIMESHIFT.md` now says plainly that the plugin's newest-segment re-stat corrects the
*plugin's* cached size only; this addon takes a size once, at a sequence's first appearance, and
the `Content-Range` cross-check is its only protection. Not reproduced, and no stronger a case than
before: ffmpeg's segment muxer lists a segment only after closing it, and nothing in these tests
(including a forced buffer wrap) ever showed a size disagreeing for a segment in the listed window.
The fix options remain what the entry says -- have the plugin hold back an unverified tail segment,
or let the addon accept a size update for a not-yet-read tail segment -- both a real design decision
for a failure that hasn't been seen; a held-back segment would cost every viewer a segment of latency.

**Closed (2026-10-02) by decision: docs corrected, no code change.** `docs/TIMESHIFT.md` already says plainly that the plugin's newest-segment re-stat corrects only the plugin's own cached size and that the `Content-Range` cross-check is the addon's only protection. The failure it guards against has never been seen -- including under a forced buffer wrap -- and the two fixes (holding back an unverified tail segment, or accepting a size update for an unread one) would cost every viewer a segment of latency. Reopen if a size disagreement is ever observed.

#### Windows release bundles end-of-life OpenSSL 1.1.1d and curl 7.67.0

**The Windows release bundles curl 7.67.0 with a statically-embedded,
end-of-life OpenSSL 1.1.1d, flagged from a 58th-pass audit
(2026-09-27), informational/needs more research before any decision,
not fixed this pass.** Confirmed by downloading the real prebuilt
archive and running `strings` on its own `libcurl.dll`: it reports
`libcurl/7.67.0-DEV` and `OpenSSL 1.1.1d 10 Sep 2019`, and imports no
separate `libssl`/`libcrypto` DLL -- meaning the SEPARATELY fetched
`openssl-1.1.1q` archive this same build step also downloads is
link-time only (satisfying `CURLConfig.cmake`'s own dependency
declaration), not what actually runs; the real TLS implementation at
runtime is whatever's compiled into `libcurl.dll` itself, an OpenSSL
version that predates the CVE-2022-0778 fix for an infinite loop on a
crafted server certificate. This matches Kodi Omega's own upstream
Windows dependency list exactly (confirmed against its real current
source), so this isn't a mistake unique to this addon's own build
steps. Whether this actually matters depends on something not checked
this pass: whether a real installed Kodi-for-Windows already loads its
own, separately-updated `libcurl.dll` that this addon's binary then
resolves against at runtime (in which case this bundled, older one
might never actually run), or whether this addon's own `.dll` takes
precedence -- needs that checked before deciding whether this is worth
pursuing further (and if so, whether pursuing it even makes sense
given it would mean matching or diverging from Kodi's own upstream
dependency choice, not just this addon's own).

**Research done (2026-10-02): the question the entry left open is answered, and it changes what the choice is.** On the real Windows Kodi 21.3 install, the running `kodi.exe` has `C:\Program Files\Kodi\libcurl.dll` (7.67.0-DEV) and `zlib.dll` (1.2.11) loaded -- Kodi's own copies, not the ones in this addon's folder (same versions, so the bundled files are redundant on a stock install; Windows will not load a second DLL under a name it already has loaded, so even a newer
`libcurl.dll` shipped beside the addon would lose). So the end-of-life TLS stack is **Kodi's own upstream dependency**, shared by every addon and by Kodi's own HTTPS, and nothing this addon bundles changes it. Kodi's dependency mirror agrees there is no newer prebuilt x64 curl to switch to: its newest is still 7.67.0 (January 2020), whereas it does carry current OpenSSL (3.5.7, June 2026) and zlib (1.3.2) -- which only matters for a curl
that is built against them, and the 7.67.0 one is not. **Options:** (A) accept it and document that this addon's HTTPS on Windows is exactly as strong as Kodi's own, which can only be fixed by Kodi updating its dependency -- the status quo, and what I would do; or (B) build a current curl from a pinned source commit with Windows' own TLS (Schannel) and link it statically into the addon DLL on Windows, so this addon's traffic stops depending on Kodi's copy at all and
no longer ships `libcurl.dll`/`zlib.dll`. B is a real build-system change that diverges from Kodi's own dependency choice and changes TLS behaviour (certificate store, revocation checking) for Windows users, so it needs a decision, not a drive-by. Awaiting your call.

**Closed (2026-10-02) by decision: Option A, accept and document.** This addon's HTTPS on Windows uses Kodi's own `libcurl.dll`, so it is exactly as strong as Kodi's -- fixable only by Kodi updating its own dependency. Documented in `docs/BUILDING.md`. Option B (a statically linked curl built from source with Schannel) stays available if that ever stops being acceptable; reopen if Kodi's Windows dependency is not updated
or a report shows the old TLS stack mattering in practice.

#### List endpoints fail loudly on paginated responses instead of following next

**`DispatcharrClient.cpp`'s list endpoints (`GetChannels()`/
`GetChannelGroups()`/`GetRecordings()`/`GetTimerRules()`/
`GetRecurringRules()`/`FindCoreSettingsRow()`) fail loudly rather than
actually following a paginated response's `next` link, flagged from a
9th-pass audit (2026-09-26), not reproduced live.** The fix that pass
landed (`dispatcharr::IsTruncatedPaginatedResponse()`, `JsonFieldUtil.h`)
deliberately chose the smaller, safer half of this problem: turning a
previously-silent, previously-untested truncation (only the first page
cached as if it were the complete list) into a clear, diagnosable
error, without touching any call site's own HTTP-request flow. It does
not make the addon actually usable against an instance whose channel
list (or any of the others) is genuinely paginated across multiple
pages -- that install would now get a clean error instead of missing
channels, which is safer but still not a full fix. A complete fix
needs each call site to loop, following `response["next"]` (an
already-authenticated GET, same as the first page) and appending each
page's own results, until `next` is null -- bigger and riskier than
the truncation-detection fix (request-loop safety, confirming whether
`next` is always a fully-qualified URL or sometimes needs resolving
against `BaseUrl()`, deciding a sane page-count safety cap), not
attempted blind. Worth implementing for real once (or if) a live
instance is actually confirmed to hit the new error.
**Update (2026-09-26, an 18th-pass audit): refined -- not reachable
today for the channels list specifically, at least.** Confirmed
against Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- a real source read, not a
live test): `ChannelPagination.paginate_queryset()`
(`apps/channels/api_views.py`) explicitly disables pagination and
returns the full, unpaginated queryset unless the request supplies a
`page` or `page_size` query parameter -- which this addon's own
`GetChannels()` never sends -- and no global
`DEFAULT_PAGINATION_CLASS` is configured
(`dispatcharr/settings.py`) to force it on regardless. So
`DispatcharrClient.h`'s own top-of-file "the channel list is
paginated" claim appears stale for the current upstream version, at
least for that one endpoint. The other list endpoints
(`GetChannelGroups()`/`GetRecordings()`/`GetTimerRules()`/
`GetRecurringRules()`/`FindCoreSettingsRow()`) weren't individually
re-checked this pass. `IsTruncatedPaginatedResponse()` itself stays --
purely defensive, harmless to keep even if it rarely or never actually
fires against a default install, and still useful for a self-hosted
instance with different pagination defaults configured.
**Live check (2026-09-29): the remaining endpoints confirmed too --
none of them paginate by default on this real instance.** Hit
`/api/channels/groups/`, `/api/channels/recordings/`,
`/api/channels/recurring-rules/`, and `/api/core/settings/` directly
against the real lab instance (`/api/channels/series-rules/` uses its
own custom `{"success", "rules"}` envelope, not DRF pagination, so it
was never in scope for this check the same way the others are) -- all
four returned a bare JSON array with no `{"results", "next"}` envelope
at all, the same shape the 18th-pass source read already confirmed for
channels specifically. `IsTruncatedPaginatedResponse()`'s own guard is
confirmed dormant across every endpoint that could reach it on this
real, current (`0.31.0`) instance's default configuration -- still
worth keeping for the self-hosted-instance-with-different-defaults
case the entry above already reasons about, just confirmed live now
that it isn't firing here, not merely inferred from source.

**Closed (2026-10-02): checked live, nothing to follow.** Against the real instance every list endpoint the addon reads returns a plain array -- channels, groups, recordings, recurring rules, settings -- and series rules a plain object, none with a `next`; Dispatcharr only paginates the channel list when the request carries a page or page-size query parameter (with a page size of 5 it answers `{count, next, previous, results}`), which this addon never sends,
and no global default pagination is configured. So the guard that fails loudly (`IsTruncatedPaginatedResponse()`) stays as pure defence and a following loop would be code that never runs. If it ever matters, note that the `next` URL names the server's own address (here its real address and port), so a loop would have to reuse only its path and query against the configured base URL, as the segment-URL rebase does.

#### recording_edl empty-directory scrub vs. an in-progress recording's folder

**`recording_edl`'s empty-directory scrub may remove an in-progress
recording's own destination folder before it's ever written to,
flagged from a 9th-pass audit (2026-09-26), needs checking against
Dispatcharr's real `tasks.py` before changing anything.**
`_prune_empty_directories()` (via `_scrub_orphaned_recording_sidecars()`)
removes every empty directory under a scan root with no guard for a
recording that's currently active. Unconfirmed whether Dispatcharr's
own `_build_output_paths()`/`run_recording` create the show/season
destination folder (via `makedirs`) at the moment a recording starts,
before the HLS-to-MKV concat ever writes a real file into it -- if so,
that folder sits genuinely empty for the recording's entire duration,
and a scrub landing in that window would remove it, breaking the
concat's own finalize step (recording ends up `preserved_failure`,
needing manual recovery). If confirmed, a fix needs to skip any
directory that's an ancestor of a currently-active Recording's own
`file_path` (a Django query), or re-check emptiness after a minimum
age.
**Update (2026-09-26, an 18th-pass audit): essentially refuted.**
Confirmed against Dispatcharr's own real current upstream source
(cloned into a scratchpad, never committed to this repo -- a real
source read, not a live test): `_build_output_paths()` does create the
destination folder, but the recording's own `.dvr_<id>_hls` HLS
directory is created inside it (`os.makedirs(hls_dir)`) just a few
statements later, in the same call -- and this plugin's own scrub
already skips dotted directories as scrub *candidates*, but a dotted
child directory still makes its own *parent* directory non-empty from
`iterdir()`'s own perspective, so the parent is never actually seen as
"empty" for the recording's whole active duration after all. The only
real window is the brief gap before the HLS directory itself is
created (which can include a network poster/artwork lookup) -- and
even if a scrub genuinely hit that exact window, `os.makedirs(hls_dir)`
would simply recreate the whole parent chain moments later. Left
logged rather than closed outright, since the brief pre-HLS-dir window
is still a real, if much narrower, gap than originally described.

**Closed (2026-10-02): refuted, no change.** As the earlier update found, the recording's own `.dvr_<id>_hls` directory is created inside the show folder in the same call that creates the folder, and a dotted child still makes its parent non-empty, so the parent is never seen as empty while recording. The only gap is the moment before that directory exists, and even a scrub landing in it would be undone: `os.makedirs(hls_dir)` recreates the whole parent chain. Nothing to guard.

#### check_doc_refs: section-title citation can bind to the wrong doc

**`tools/check_doc_refs.py`'s `check_section_titles()` can bind a "..."
section citation to the wrong `docs/X.md` mention when a paragraph
names two different doc files close together, flagged from a
project-wide review (2026-09-26), low priority -- the checker's own
docstring already discloses it's "best-effort... not a real parser".**
`last_doc_file` is updated on every `docs/X.md` mention within the
current paragraph with no attempt to tell which specific sentence a
later quoted title actually belongs to, so a citation is always bound
to the *nearest preceding* mention -- wrong if a paragraph legitimately
discusses two different doc files and the quoted title belongs to the
first one, not the second. Fixing this properly needs real
sentence/paragraph-structure-aware matching, disproportionate effort
for a lint-style tool whose own stated design is "expect to eyeball
the output, not treat every hit as a guaranteed bug". Not changing
this now; worth a real fix only if it starts producing actual false
positives/negatives in practice (none observed so far).

**Closed (2026-10-02) as an accepted limitation.** The checker is documented as best-effort; binding a quoted title to the nearest preceding `docs/X.md` mention has produced no false positive or negative in practice, and a sentence-aware parser would be disproportionate for a lint. Reopen if it ever misreports a real citation.

#### check_doc_refs: check_settings() coverage gaps

**`tools/check_doc_refs.py`'s `check_settings()` has several coverage
gaps, flagged from a 9th-pass audit (2026-09-26), confirmed by running
it against the repo, low priority.** Only lines containing the literal
word "setting" are checked at all (no paragraph-lookback the way
`check_section_titles()` has for its own citations); `SETTING_REF_RE`
requires an underscore in the id, so single-word settings like `host`/
`port`/`timeout`/`username`/`password` can never be flagged even if
removed; `README.md`, each plugin's own README, and `CLAUDE.md` itself
aren't in `DOC_FILES` at all despite citing real setting ids; and the
known-ids set merges Kodi settings with both plugins' own field/action
ids, so a Kodi-setting citation that happens to collide with an
unrelated plugin id would pass unnoticed. None of these produced an
actual false negative when checked against this repo's current real
settings (confirmed by running the tool), so this is a coverage gap in
the checker itself, not a currently-live stale-doc problem -- not
fixed this pass given `tools/check_doc_refs.py`'s own already-tracked
"best-effort, not a real parser" framing (see this file's own earlier
entry on `check_section_titles()`'s similar imprecision).

**Closed (2026-10-02) as an accepted limitation.** None of the four gaps (only lines containing "setting", no single-word ids, README/CLAUDE.md not scanned, plugin ids merged with Kodi's) produced a wrong result when run against this repo, and widening it would mostly add noise from prose that uses those words. Reopen if a stale setting citation is ever found that it missed.

#### kodi_smoke_test.py residual fixes

**`tools/kodi_smoke_test.py` residual fixes not yet implemented,
flagged from a 27th-pass audit (2026-09-26), all judged safe to
implement from static analysis alone since this is test-harness code,
not addon code -- fixing a bug here can't itself break real
user-facing behavior the way an addon-side fix could.** Beyond the
seek-target/broadcast-fallback fixes already logged from the prior
pass:
- `DispatcharrApiClient.call()`/`_authenticate()` should also catch
  `ConnectionError`/`json.JSONDecodeError`/`KeyError`, matching
  `JsonRpcClient`'s own already-hardened handling; `SmokeTestRun.record()`
  could safely catch a bare `Exception` as a clean FAIL (including the
  exception's own type name) instead of letting an unexpected one kill
  the whole run with a traceback.
- `check_realtime_update_push()`'s own POST sits outside its
  try/finally entirely -- a POST that times out or returns non-JSON
  *after* the server already created the recording leaves a real,
  uncleaned-up recording on the backend, the same "succeeds
  server-side, fails client-side" class this same file already
  documents and guards against elsewhere (`Player.Open`/`PVR.AddTimer`),
  just missed here. A separate, smaller gap: an exception raised by
  the cleanup DELETE itself inside a `finally` block currently
  replaces the original assertion's own failure message.
- The in-progress-recording detection this test relies on
  (`_find_in_progress_recording_id()`) still needs a real, independent
  signal beyond `endtime > now` for the `"interrupted"` case
  specifically (the `"stopped"` case is now handled by this addon's own
  duration fix two passes ago) -- a `PVR.GetTimers` entry with
  `state == "recording"` and a matching `starttime` would be a more
  direct check, confirmed against Kodi's real source
  (`PVRTimerInfoTag.cpp`) that both come from the same underlying
  `rec.startTime`.

**Closed (2026-10-02): the entry is stale.** Re-read against the harness as it stands: `DispatcharrApiClient` wraps connection and JSON errors like `JsonRpcClient`, `SmokeTestRun.record()` catches any exception as a clean FAIL, the realtime check's POST is inside its try/finally with a look-up-the-recording recovery, its cleanup DELETE is suppressed so it cannot replace the real failure, and in-progress detection uses a `state == "recording"` timer. Nothing left to implement.

#### recording_edl edl_skip_field assumption vs. a custom comskip.ini

**`recording_edl`'s `edl_skip_field` assumption can be wrong for a
custom comskip.ini, flagged from a 23rd-pass audit (2026-09-26),
confirmed against Dispatcharr's own real current upstream source, not
reproduced live.** `docs/RECORDING_EDL.md`'s own confirmed-type-3
claim assumes Dispatcharr's shipped `docker/comskip.ini`
(`edl_skip_field=3`) is what actually generated a given `.edl` file --
but Dispatcharr's own DVR settings let a user supply a custom comskip
ini, which takes precedence over the shipped one, confirmed via its
own ini comments that comskip's own upstream default for this field is
`0` (Kodi's own `PVR_EDL_TYPE_CUT`, a hard cut, not `COMBREAK`).
`RecordingParser`'s own out-of-range `type` coercion already guards the
UB risk for a genuinely invalid value (see its own CLAUDE.md entry),
but a custom ini producing valid-but-different type values (a hard cut
instead of a skippable break) would pass through as "confirmed type 3"
when it may not actually be -- a doc-accuracy nuance, not a crash or
data-loss risk. Not pursued this pass.

**Closed without a change (2026-10-02): documented, nothing to fix here.** The wording that overstated it was corrected where it lived -- `dispatcharr-plugin/recording_edl/plugin.py`'s module docstring and `docs/RECORDING_EDL.md` now say 3 is what the shipped ini produces and a custom ini can yield other valid types, which pass through unchanged.

#### recording_edl absolute recording-path templates no longer fully supported upstream

**`recording_edl`'s "absolute recording-path templates are legitimate,
already-supported" framing is now only half true, flagged from a
23rd-pass audit (2026-09-26), confirmed against Dispatcharr's own real
current upstream source, not reproduced live.** Dispatcharr's own
`RecordingViewSet.file()`/`destroy()` now resolve `file_path` through
`resolve_safe_local_data_path`, which requires the resolved realpath to
sit under `/data/recordings` -- a recording created via an absolute
template pointing outside that tree now 404s on playback and can't be
deleted server-side either, upstream of anything this addon or its
companion plugins do. `recording_edl`'s own docstring/scan-root
handling for such templates is unaffected functionally (it still finds
and can scrub sidecars there correctly), but the premise that such a
recording is otherwise "legitimate, already-supported" no longer holds
on a current Dispatcharr version -- an upstream limitation, not
something to fix here, just a doc-accuracy note for whoever next
touches that code.

**Closed without a change (2026-10-02): documented, nothing to fix here.** The wording that overstated it was corrected where it lived -- `docs/RECORDING_EDL.md`'s new "Upstream limitations to know" section and the comment in `plugin.py` that called such templates "already-supported".

#### Comskip cut mode leaves temp files after a failed cut

**Dispatcharr's own comskip "cut" mode leaves temp files behind
forever on a failed cut, flagged from a 26th-pass audit (2026-09-26),
confirmed against Dispatcharr's own real current upstream source, an
upstream limitation not actionable from this addon, not reproduced
live.** `comskip_process_recording()`'s own cut-mode code path has no
cleanup on its exception path, so a failed cut leaves `segment_NNN.mkv`/
`concat_list.txt`/`<base>.cut.mkv` in the recording's own folder
indefinitely -- these block `recording_edl`'s own empty-folder cleanup
and waste disk, and since the temp filenames are fixed per folder, two
cut-mode comskip runs in the same show's folder could even overwrite
each other's segments. Separately, a failed cut leaves
`custom_properties.comskip` as `{"status":"error"}` with no `edl` key,
so a still-genuinely-valid `.edl` from an earlier successful run isn't
served afterward either -- marginal, probably an acceptable outcome
given the recording itself is presumably also broken/incomplete in
that scenario. Nothing to fix addon-side beyond documenting it.

**Closed without a change (2026-10-02): documented, nothing to fix here.** The wording that overstated it was corrected where it lived -- `docs/RECORDING_EDL.md`'s "Upstream limitations to know" section, including why the plugin deliberately does not delete those files.

#### Dispatcharr recording enrichment ignores a channel's EPG override

**Dispatcharr's own recording auto-enrichment queries a channel's raw
`epg_data`, not its effective/override one, found live 2026-09-29
while re-verifying the DVR-padding fix above -- a real, confirmed,
Dispatcharr-server-side gap, not something this addon's own client
code can control or work around.** `prefetch_recording_artwork`/
`run_recording`'s shared `_match_epg_program_by_timeslot(channel_epg_data,
rec_start, rec_end)` (`apps/channels/tasks.py`, confirmed against
Dispatcharr's own real current upstream source) is called with
`rec.channel.epg_data` -- the channel model's own raw `epg_data`
foreign key. On a channel with a per-channel EPG override configured
(`ChannelOverride.epg_data_id`, the same mechanism `ChannelParser.cpp`'s
own `effective_epg_data_id` preference already accounts for on this
addon's own side, per its CLAUDE.md entry), the raw and effective
`epg_data_id` can point at two different `EPGData` rows entirely.
Confirmed live against a real channel on the real lab instance: raw
`epg_data_id` and `effective_epg_data_id` were two different values.
A recording on that channel for a slot roughly a day out failed to
enrich at all (no `title`/`sub_title`/`description`/`id` ever
appeared, `poster_logo_id` fell back to the plain channel logo) even
though Kodi's own guide -- sourced from the *effective* EPG data, the
correct one -- clearly showed a real programme covering that exact
slot; a similar recording on the same channel for a slot only a few
minutes out (the earlier live check just above) enriched successfully
within seconds. The most likely explanation, not itself independently
confirmed: the raw `epg_data` source's own import simply doesn't cover
as far into the future as the effective one does, so a near-term slot
happens to exist in both while a farther-out one only exists in the
correct, effective source -- consistent with everything observed, but
not traced further this pass (would need direct DB/admin access to
Dispatcharr's own `EPGData`/`ProgramData` rows to confirm definitively,
not available from this addon's own API-only vantage point). Not
something to fix here: this is server-side Dispatcharr code this
addon has no way to influence, and this addon's own recordings already
correctly resolve guide data via the effective/override chain on the
client side (`ChannelParser.cpp`) -- logged for awareness (a future
Dispatcharr upstream fix, or worth reporting there) and so a future
audit pass doesn't waste time re-diagnosing "why doesn't this
recording's title/description ever fill in" as if it were this
addon's own bug.

**Closed without a change (2026-10-02): documented, nothing to fix here.** The wording that overstated it was corrected where it lived -- `docs/RECORDINGS.md`, next to the enrichment notes it belongs with; reporting it to the Dispatcharr project is the only remaining step and is the maintainer's call.

#### The startup backend-version read is never retried

**Closed 2026-10-05 without a change (the fifteenth sweep's finding, checked against Kodi's source).** The finding was that a server too slow to answer at startup leaves Kodi's backend-version field empty for the whole session. Retrying from the background thread, as the other startup reads are, cannot change that: Kodi reads `GetBackendVersion()` once, when the client connects (its client property read, run at creation and on a connection-state change), and caches it; nothing the addon does later reaches the cache short of reporting a connection-state change, which makes Kodi reconnect the client and reload its channels, timers and guide for a cosmetic field. A restarted client reads it again anyway. Not worth it.

### Project history and test infrastructure

#### Project rename to pvr.dispatcharr-unofficial plus non-affiliation disclaimer

**Rename the project a second time, from `pvr.dispatcharr` to
`pvr.dispatcharr-unofficial`, plus add an explicit non-affiliation
disclaimer (requested 2026-09-11, shortly after first sharing the
project with a few people under the `pvr.dispatcharr` name).** Same
mechanical shape as the first rename (`pvr.dispatcharrai` ->
`pvr.dispatcharr`): directories, `addon.xml.in`'s `<addon id="...">`,
`CMakeLists.txt`, the CI workflow's addon-defs paths/`ADDONS_TO_BUILD`/
artifact names, the CoreELEC `package.mk`, and the `~50` hardcoded
`"pvr.dispatcharr: "` log-prefix strings in `src/*.cpp` all moved to
the new id. The GitHub repo itself is being renamed to match this
time (decided upfront, not deferred as a separate question the way it
was for the first rename).
**A real mistake caught before committing, worth recording so it
doesn't repeat:** a blind find-and-replace of every `pvr.dispatcharr`
occurrence also rewrote *frozen historical records* --
`CHANGELOG.md`'s dated, version-numbered entries and
`docs/OPEN_ITEMS.md`'s own blow-by-blow account of the *first*
rename's platform-by-platform verification -- to falsely claim those
past events happened under the `-unofficial` id, which didn't exist
yet at the time. Reverted both files entirely and added fresh, forward
-dated entries instead, leaving the historical record of the first
rename exactly as it happened. `docs/BUILDING.md` was deliberately
*not* reverted -- it's a living how-to (its own instructions need to
stay executable against the current id), not a dated ledger, and it
was already treated that way through the first rename without
complaint. The plugins' `README.md`/`plugin.py` mentions of the
addon's id were also left renamed -- they're present-tense
architecture descriptions ("the addon now exposes..."), not dated
version-specific claims, so updating them doesn't misrepresent
anything that already shipped.
Added an explicit "this is unofficial, not affiliated with
Dispatcharr" disclaimer in three places: `addon.xml.in`'s `<name>`
(now "Dispatcharr PVR Client (Unofficial)") and `<description>` --
visible in Kodi's own add-on browser at install time, not just in
this repo -- plus `README.md`'s opening paragraph.
Addon version bumped to `0.10.0` (id change is a real,
addon-version-scoped change, same reasoning as the first rename);
`PKG_SHA256` reset to the all-zeros placeholder per the usual
convention until a real tag exists.
**Update: build verified and PR merged (2026-09-11).** Full
from-scratch Windows build under the renamed target succeeded
(curl/openssl/zlib deps reused from the existing workspace); hit the
same "stale ExternalProject state" gotcha the first rename's own
Linux/CoreELEC builds already documented, just on the Windows/
ExternalProject harness this time -- deleting the addon's own
`<build-dir>/pvr.dispatcharr-unofficial-prefix/` directory (the
Windows-harness equivalent of `.installed-native`/the `*-prefix/`
directory already called out for the Linux case) forced a real
reconfigure and fixed it. Rendered `addon.xml` inspected directly
from the built zip to confirm id/version/name/description/disclaimer
all correct before merging. That change merged via squash, same as the
first rename.
**Update: GitHub repo renamed too (2026-09-11), decided upfront this
time rather than deferred.** `BruiserBrody17/pvr.dispatcharr` is now
`BruiserBrody17/pvr.dispatcharr-unofficial`. `gh repo rename`, then
this Windows workspace's own `origin` updated via `git remote
set-url` -- confirmed still tracking correctly afterward, same
redirect-makes-it-seamless result as the first rename's repo-rename
step. A repo-wide sweep for leftover GitHub URLs still pointing at
the old repo name found none outside the deliberately-preserved
historical text describing the *first* rename's own repo-rename
event.
**Update: `0.10.0` tagged and released (2026-09-11), relabeled from
the never-shipped `0.9.5` pending bump rather than adding a separate
changelog entry for the same work.** Release notes pulled from
`CHANGELOG.md`; CoreELEC zip built via the cached toolchain and
attached by hand, real `PKG_SHA256` filled in same as every prior
release. Caught and fixed one real regression along the way: CI's
`build-windows` job failed on the very next push after this rename
(a docs-only commit, no code change) with a genuine `Filename too
long` error cloning `nlohmann-json` -- both this rename and the
matching GitHub repo rename each added 12 characters to the
checkout/build path, pushing an already-deeply-nested dependency
file past Windows' 260-char `MAX_PATH`. Fixed by enabling git's
`core.longpaths` in CI (and on this local machine, whose own build
had survived by only 7 characters of margin even before this rename)
-- full account in `docs/BUILDING.md`'s Windows section.
**Update: fresh-install and verify across platforms now done too
(2026-09-16), closing this out fully.** Not done as one dedicated
pass -- covered incidentally, but thoroughly, by the four-platform
smoke-test matrix run since (see the "Manual-testing checklist for a Kodi sanity pass" entry): Linux, Windows, CoreELEC/ODROID N2+, and macOS have each since
been fresh-installed and heavily exercised under the
`pvr.dispatcharr-unofficial` id/build, with no id-migration-specific
problems surfacing on any of them.

#### Project rename from pvr.dispatcharrai to pvr.dispatcharr

**Rename the project from `pvr.dispatcharrai` to `pvr.dispatcharr`
(requested 2026-09-09).** Mechanically straightforward in-repo: `git
grep -il dispatcharrai` found 25 files at request time (27 by the time
this was actually done, since more docs/tooling had landed by then),
plus the two directories whose names carried the id
(`pvr.dispatcharrai/`/`packaging/coreelec/pvr.dispatcharrai/`). Needed
updating: `addon.xml.in`'s `<addon id="...">`, `CMakeLists.txt`'s
`project()`/`build_addon()`, `.github/workflows/build.yml` (addon-defs
paths, `ADDONS_TO_BUILD`, artifact/zip names), the CoreELEC
`package.mk` (`PKG_NAME`/`PKG_SITE`/`PKG_URL`/`PKG_SHORTDESC`), docs
(`docs/BUILDING.md` heaviest), and ~50 hardcoded `"pvr.dispatcharrai: "`
log-prefix strings across `src/*.cpp` (cosmetic, not functionally
required). The two companion Python plugins
(`dispatcharr-plugin/recording_edl`, `dispatcharr-plugin/timeshift_buffer`)
keep their own unrelated ids but reference `pvr.dispatcharrai` by name
in READMEs/`plugin.json` `help_url`s/many `plugin.py` comments -- those
needed updating too. Renaming the GitHub repo itself
(`BruiserBrody17/pvr.dispatcharrai`) was a separate decision, made
after the in-repo rename landed -- see the "Update" below for the
actual rename and the URL cleanup that followed it. Every GitHub URL
(repo `<source>`/`PKG_SITE`/`PKG_URL`/`help_url`s/clone commands/README
release links) was deliberately left pointing at the *old* repo name
in the initial in-repo-rename commit, specifically because this
decision hadn't been made yet at that point -- only the addon's own
id, directory names, and in-repo local-checkout-directory conventions
changed in that first pass.
**The real cost isn't the repo, it's that Kodi treats an id change as
a brand-new addon, not an upgrade.** The addon id is both the
installed folder name and the `userdata/addon_data/<id>/settings.xml`
storage key, so every device currently running this addon (Windows,
Linux, ODROID N2+/CoreELEC, macOS) needs the old addon
removed and the new-id build installed fresh -- existing settings
(host/port/credentials, API key, timezone selection, padding,
timeshift mode) do **not** carry over automatically; either hand-copy
each device's `addon_data` folder to the new id or reconfigure from
scratch, still undecided. Also expect to need the same "Settings ->
PVR & Live TV -> Guide -> Clear data" step already documented above
(Kodi's EPG database keys off the client id) on every device after the
switch. Proposed order: rename on a branch -> decide
GitHub-repo-rename yes/no -> rebuild + fresh-install Windows first and
verify clean -> roll the same fresh-install to Linux,
ODROID/CoreELEC, and macOS (via the peer session) -> settle the
settings-carryover question per device -> cut a release under the new
name once all four platforms are confirmed working.
**Update: mechanical in-repo rename done and confirmed compiling
(2026-09-10), on branch `rename/pvr-dispatcharr` -- not yet merged,
not yet installed anywhere.** All 27 files updated; every GitHub URL
deliberately left pointing at the real, current repo name (see above).
`PKG_SHA256` in the renamed `package.mk` reset to the all-zeros
placeholder, per this file's own versioning convention -- the existing
real checksum was computed against the old (un-renamed) `0.9.3` tag's
actual tarball content, so it no longer matches anything this renamed
source would produce. Verified live: reconfigured the local Windows
build workspace with a new `addon-defs/pvr.dispatcharr/` entry
(pointing the same `file://` URL at this repo's root) and did a full,
from-scratch `cmake`+MSBuild build under the new target name -- built
and installed cleanly to `install/pvr.dispatcharr/pvr.dispatcharr.dll`
with no errors. `clang-format`/`ruff` both pass (the log-prefix string
length change shifted a handful of multi-line `kodi::Log()` calls'
wrapping, caught by `clang-format --dry-run -Werror` and fixed).
`tools/check_doc_refs.py` also updated (its own hardcoded
`resources/settings.xml` path) and still passes clean against the
baseline.
**Update: GitHub repo renamed too (2026-09-10) -- `BruiserBrody17/
pvr.dispatcharrai` is now `BruiserBrody17/pvr.dispatcharr`, decided and
executed the same session.** `gh repo rename`, then this Windows
workspace's own `origin` updated via `git remote set-url` (confirmed
still tracking correctly afterward -- GitHub's redirect made the
transition seamless, no re-clone needed). Every GitHub URL this repo's
own files deliberately left pointing at the old name in the initial
in-repo-rename commit above (repo `<source>`/`PKG_SITE`/`PKG_URL`/
`help_url`s/clone commands/README release links, 11 files) was then
updated to the new name too, now that the reason to hold off no longer
applies -- rather than leaning on GitHub's redirect indefinitely.
`docs/BUILDING.md`'s pinned-checksum example URL
(`.../archive/0.3.0.tar.gz`) updated the same way; the tag itself
(`0.3.0`) didn't change, just which repo name it's addressed through.
Still pending: the Linux machine, ODROID/CoreELEC, and macOS
peer-session workspaces each have their own `origin` still pointing at
the old repo name -- each needs its own `git remote set-url` (or a
fresh clone) whenever that device is next touched; not urgent since
the old URL keeps working via GitHub's redirect, just cleanup.
**Update: fresh-installed and verified live on this Windows machine
(2026-09-10) -- Windows now confirmed clean, the first of four
platforms.** Rather than reconfiguring from scratch, carried over the
existing `addon_data/pvr.dispatcharrai/settings.xml` verbatim to a new
`addon_data/pvr.dispatcharr/` -- both are just files, no id baked into
the content itself, so this preserves host/port/credentials/API key/
timezone/padding/timeshift settings exactly. Installed the renamed
build alongside the old one (different ids, so no conflict), disabled
`pvr.dispatcharrai` and enabled `pvr.dispatcharr` via
`Addons.SetAddonEnabled`, confirmed via `PVR.GetClients` that exactly
one client (`pvr.dispatcharr`) was active afterward, not both. A burst
of `PVR::CPVREpg::Update: ... Client '-1' not found` errors appeared
once, right at the enable/disable transition -- Kodi's EPG database
cleaning up the old client's now-orphaned tables, a one-time artifact
of the switch, not a recurring problem (confirmed: no further
occurrences afterward). Everything else came up clean with the carried-
over settings: realtime updates connected, background channel/EPG
refresh succeeded, recordings/timer-rules caches populated with the
real live counts (the same recording/series-rule counts as under the old addon, confirming the same account), and `PVR.GetChannels`
returned the full real channel lineup. Live playback smoke-tested
end to end, not just data loading: opened a real channel (Channel H,
same generic-label convention as Channels A-G elsewhere in this file's
history -- see the earlier "Packet corrupt"/EPG-matching entries),
confirmed via `kodi.log` the stream URL was genuinely routed through
the new addon (`pvr.dispatcharr_1.pvr`), audio decoder opened
successfully, only the same already-documented benign startup noise
(`non-existing SPS/PPS referenced`, a transient audio-sync
adjustment) -- no new errors. The old `pvr.dispatcharrai` install was
left in place, disabled rather than deleted, as a rollback path.
Still not done: the other three devices (Linux,
ODROID/CoreELEC, macOS via the peer session), or the eventual release
cut.
**Update: fresh-installed and verified live on macOS (2026-09-10) --
second of four platforms.** No persistent build workspace survived
from earlier sessions, so this was a from-scratch setup (fresh Kodi
source checkout, a separate local addon copy synced via `rsync -az
--delete`, `addon-defs` pointing at it) -- built cleanly on the first
attempt. Same settings-carryover approach as Windows: copied
`addon_data/pvr.dispatcharrai/settings.xml` verbatim to a new
`addon_data/pvr.dispatcharr/settings.xml` (byte-identical, confirmed
via `diff`). Installed the renamed build alongside the old one,
recorded real baseline counts under the old addon first (the full real channel lineup, a handful of recordings and timers via `PVR.GetChannels`/
`GetRecordings`/`GetTimers`), then disabled `pvr.dispatcharrai` and
enabled `pvr.dispatcharr` via `Addons.SetAddonEnabled`. `PVR.GetClients`
confirmed exactly one active client afterward (`pvr.dispatcharr`,
clientid 2 -- the old one had been clientid 1). Unlike Windows, no
`Client '-1' not found` burst appeared at all here (not a discrepancy --
the task description flagged it as "likely", not guaranteed); no
errors in `kodi.log` either way. Re-ran the same three counts under
the new addon: identical (the same channel lineup, and the same handful of recordings and timers), confirming the same account/
data, not something broken. EPG also confirmed loading real programme
data for a real channel (`PVR.GetBroadcasts` on Channel H returned many
real broadcasts with real, distinct titles -- not placeholder/empty
data). Live playback smoke-tested end to end on Channel H: `kodi.log`
confirmed the stream
genuinely routed through the new addon
(`pvr.dispatcharr_1.pvr`, `CallTimeshiftPluginAction(start_buffer)`
logged under the `pvr.dispatcharr` prefix), and a real screenshot
confirmed live video actually playing (not just a JSON-RPC state
claim -- `Player.GetActivePlayers`/`GetProperties` returned transient
empty/`None` responses immediately after `Player.Open` that turned out
to be socket-timing noise in the test harness, not a real problem;
retrying a few seconds later, and the screenshot, confirmed playback
was actually healthy the whole time: `canseek: true, speed: 1`). Only
the same already-documented benign startup noise appeared
(`non-existing SPS/PPS referenced`, a `-245ms` self-correcting
`ActiveAE::SyncStream` adjustment) -- no new errors. One channel-id
gotcha worth noting for future sessions: Kodi's PVR `channelid`s are
scoped to the active client instance, not stable across a client
switch -- a channelid cached from before the switch (2, used in
earlier macOS sessions) returned "Invalid params" against the new
client and had to be re-looked-up by channel label instead. Old
`pvr.dispatcharrai` left in place, disabled, as a rollback path.
**Update: fresh-installed and verified live on the Linux machine
(2026-09-10) -- third of four platforms, driven directly this time
(no more separate Claude Code peer sessions for the remaining
platforms).** Persistent build workspace (`~/kodi-linux-build/`)
survived from earlier sessions; synced the current branch tip into it
via a fresh `git clone` + `rsync -az --delete` (the live-checkout
stale-marker gotcha already documented in `docs/BUILDING.md` applied
again -- `.installed-native` had to be deleted before the build
harness would do anything, even though `pvr.dispatcharr` itself had
never been built here before; clearing just that one marker was
enough this time, no `*-prefix/` directory existed yet for the new
name). Also discovered Kodi wasn't actually running when this started
(a stale pid from an earlier check, confirmed via `kodi.log`'s own
"Exiting the application..." line) -- relaunched it using the
documented display-environment workaround, sourcing
`DISPLAY`/`WAYLAND_DISPLAY` from a live session process
(`/proc/<pid>/environ`) rather than the launching shell's own
(nonexistent) environment. Same settings-carryover approach as the
other two platforms: copied `addon_data/pvr.dispatcharrai/settings.xml`
verbatim to `addon_data/pvr.dispatcharr/settings.xml`. Installed
alongside the old addon, disabled `pvr.dispatcharrai` and enabled
`pvr.dispatcharr` via `Addons.SetAddonEnabled`; `PVR.GetClients`
confirmed exactly one active client afterward. Real data loaded
correctly: `PVR.GetChannels` returned the full real channel lineup,
recordings/timer-rules caches populated with real live counts (the same order-of-magnitude recording/series-rule counts -- didn't
capture a specific "before" baseline on this platform since the old addon was disabled before
checking, but the counts are clearly real, non-zero data, and the
general order of magnitude matches the other two platforms; small
differences between platforms are expected here since recording/timer
counts are genuinely live and change over time, not a discrepancy to
chase). Live playback smoke-tested end to end on Channel H (same
generic-label convention as Channels A-G elsewhere in this file's
history): `kodi.log` confirmed the stream genuinely routed through the
new addon (`pvr.dispatcharr_1.pvr`, the same real channel id as
the other two platforms), audio decoder opened successfully, only the
same already-documented benign startup noise -- no new errors. Old
`pvr.dispatcharrai` left in place, disabled, as a rollback path.
**Update: fresh-installed and verified live on CoreELEC/ODROID N2+
(2026-09-10) -- fourth and final platform, driven directly.** No
tagged release exists yet for this rename (deliberately deferred until
all platform testing is done), and CoreELEC's package.mk convention
only supports pointing `PKG_URL` at a resolvable ref, not a live
branch -- worked around by pointing it at this branch's exact commit
SHA instead of a tag (GitHub serves an archive tarball for any ref,
not just tags) and computing the real `PKG_SHA256` for that tarball
directly (`curl -L .../archive/<sha>.tar.gz | sha256sum`). This edit
was only ever made to the out-of-tree package.mk copy inside the
Ubuntu CoreELEC build checkout (`~/coreelec-build-21/packages/
mediacenter/kodi-binary-addons/pvr.dispatcharr/package.mk`), never to
this repo's own tracked `packaging/coreelec/pvr.dispatcharr/
package.mk` -- so no revert was needed before this could be part of a
merge; the tracked file stayed on the all-zeros `PKG_SHA256` placeholder
the whole time. That build machine's persistent toolchain cache
survived from earlier sessions, so the cross-compile
(`PROJECT=Amlogic-ce ARCH=arm DEVICE=Amlogic-ng ./scripts/create_addon
pvr.dispatcharr`) finished in seconds and produced
`pvr.dispatcharr-0.9.3.1.zip`. Deployed it alongside the existing
`pvr.dispatcharrai` install via `scp`+`unzip` (no in-tree CoreELEC
package-manager install path available for an out-of-tree/unreleased
build), copied `addon_data/pvr.dispatcharrai/settings.xml` verbatim to
`addon_data/pvr.dispatcharr/settings.xml`. Unlike the other three
platforms, Kodi was already running before the new addon directory was
dropped in, so it needed an explicit restart to discover it --
`Application.Quit` via JSON-RPC, then CoreELEC's own systemd unit
auto-restarted Kodi (~40s) and the new addon showed up in
`Addons.GetAddons`. Disabled `pvr.dispatcharrai` and enabled
`pvr.dispatcharr` via `Addons.SetAddonEnabled`; confirmed exactly one
active client afterward. Real data loaded correctly: `PVR.GetChannels`
returned the full real channel lineup, and the addon's own
recordings/timer-rules cache-refresh log lines reported the same order-of-magnitude recording/series-rule counts as the other
platforms (exact counts drift over time since this is a live account,
not a discrepancy to chase). Live playback smoke-tested end to end on
Channel H (same generic-label convention as Channels A-G elsewhere in
this file's history): `kodi.log` confirmed the stream genuinely routed
through the new addon (`pvr.dispatcharr_1.pvr`, the same real
channel id as the other three platforms), `CallTimeshiftPluginAction
(start_buffer)` started a new buffer, and `ReadLiveTimeshiftStream`'s
catch-up-to-tail loop completed normally across several consecutive
segments -- no errors. One harness-only gotcha worth noting for future
sessions, not an addon bug: chaining `Player.Open` together with
several follow-up JSON-RPC calls in one long `ssh`-piped shell command
left the whole chain hanging indefinitely even though the webserver
itself stayed responsive to fresh, independent connections the whole
time -- switching to one plain, single-purpose SSH command per JSON-RPC
call resolved it immediately. Old `pvr.dispatcharrai` left in place,
disabled, as a rollback path. All four platforms (Windows, macOS,
Linux, CoreELEC/ODROID) are now fresh-installed and verified live
under the renamed addon id. Still not done: merging this branch and
cutting the real tagged release (real `PKG_SHA256`, CoreELEC zip
attached by hand per `docs/BUILDING.md`), and the deferred
git-history purge.
**Update: fully closed out (2026-09-11).** That change (the rename)
squash-merged to `master`; `0.9.4` tagged and released with all three
addon platform zips, the CoreELEC zip (built and uploaded by hand per
`docs/BUILDING.md`), and both plugin zips, release notes pulled
verbatim from `CHANGELOG.md`. The deferred git-history purge also
ran, fully complete. Not
addon-rename-specific, but found and fixed in the same window: two
real security bugs from a full-codebase security review (a
`channel_uuid` path-traversal and a `client_ip` header-injection, both
in `timeshift_buffer`, see `docs/TIMESHIFT.md`) -- unrelated to the
rename itself, just concurrent work before this project's first wider
release to other testers.

#### Follow-up API survey: three more implementable findings

**Follow-up API survey: three more genuinely implementable findings,
beyond the recording-management ones below (found 2026-09-08, all
four resolved by 2026-09-09).** Diffed all ~196 of Dispatcharr's real API paths
(its live `/api/schema/`) against every endpoint this addon actually
calls (grepped from `src/DispatcharrClient.cpp`), then checked the
promising gaps against Dispatcharr's real source rather than guessing:
(1) `GET /api/core/version/` -- public, no auth, `{version,
timestamp}` -- closes `GetBackendVersion()`'s own documented "no
confirmed server-version endpoint" gap directly; (2) `GET
/api/core/timezones/` -- the full ~400+ real IANA timezone list vs.
this addon's own curated ~25, could broaden `recurring_rule_timezone`
coverage; (3) Dispatcharr's `SystemNotification` system (`GET
/api/core/notifications/` etc.) is real and operationally relevant
(version-update/setting-recommendation/warning/info, with a real
priority field), not just dev chatter -- a plausible, secondary
feature to surface high-priority ones as Kodi GUI notifications, no
design work done beyond confirming it's real. Also checked and ruled
low-value: `POST .../catchup/sessions/{id}/position/` is purely
cosmetic for Dispatcharr's own admin dashboard, doesn't affect this
addon's playback. Also noted: Dispatcharr has a whole separate VOD
API (`/api/vod/*` -- movies/series/episodes/categories) this addon
doesn't touch, but that's architecturally out of scope for a
`kodi.pvrclient`-type addon, not a gap to close here. Full detail in
`docs/API_NOTES.md`'s table and its new "System notifications" section.
**Update: a fourth finding, from a broader "what Dispatcharr product
features exist" pass rather than just an endpoint diff.** Dispatcharr
supports named, curated channel subsets (`ChannelProfile`/
`ChannelProfileMembership`, with user accounts assignable to one) --
a real feature (e.g. a "Kids" or "Sports only" lineup) this addon has
no way to let a user pick; `GetChannels()` always pulls every channel,
unfiltered. Confirmed against the real `ChannelViewSet.get_queryset()`
that this is opt-in filtering (`?channel_profile_id=`), not a
server-enforced access boundary -- so not implementing it isn't a
security gap, just a missed curation feature. `GET
/api/channels/profiles/` lists what's available (empty on this
particular single-user instance, but the mechanism is real). See
`docs/API_NOTES.md`'s new "Channel profiles" section.
**Update: items (1) and (2) implemented and confirmed live
(2026-09-09).** `GetBackendVersion()` now reports Dispatcharr's real
version (confirmed live: `0.30.0`, visible in Kodi's own System Info ->
PVR service panel), fetched once at startup via new
`DispatcharrClient::GetServerVersion()`. `recurring_rule_timezone`'s
known-zone table broadened from 25 to ~50 entries using
`GET /api/core/timezones/` as the reference for what's real vs.
hand-guessed -- the practical scope turned out narrower than "genuinely
comprehensive": the actual bottleneck is DST *rule* coverage (only two
hand-verified rule families exist), not the zone name list, so this
stayed within those families plus confirmed no-DST zones rather than
claiming all ~440. New `DispatcharrClient::GetSupportedTimezones()`
also feeds a refined startup diagnostic (distinguishes "real zone, no
DST rule for it" from "not a recognized zone at all"). A genuine,
unrelated regression was found and fixed along the way: a dropdown
entry in `kKnownTimeZones`/`settings.xml`/`strings.po` had been
accidentally broken, silently breaking DST auto-detection for that
zone. Confirmed via code
review that the functional locations show the correct entry again,
and via a restart that `recurring_rule_timezone` auto-detection works
correctly for known zones generally. Full account in
`docs/RECURRING_RULES.md`'s "Update" note.
**Update: 0.9.2 (batching this item plus the three recording-management features ("Three recording-management features (rename, file size, extend in-progress)")) confirmed on CoreELEC/ODROID N2+ (2026-09-09).** Real
cross-compile via the CoreELEC package.mk path (`docs/BUILDING.md`),
deployed over SSH. `PVR.BackendVersion` correctly read back `0.30.0` via
`XBMC.GetInfoLabels` (no GUI screenshot needed for this one -- a real
Kodi skin infolabel, more direct than the Windows check). Timezone fix
also confirmed independently on this device: `recurring_rule_timezone`
auto-resolved correctly on first load of the new build. Basic
live-playback smoke check
(Channel A) also came back clean, no errors in `kodi.log`. Recording
rename/file size/extend-recording themselves were **not** re-tested on
this device -- all three are plain REST calls plus generic Kodi PVR
API plumbing with no platform-specific code path, the same reasoning
already applied when deciding not to cross-platform-test rename after
it was first confirmed on Windows.
**Update: also confirmed on the Linux machine (2026-09-09) --
it had been sitting at `0.9.0`, several releases behind.** Same checks,
same clean result: `PVR.BackendVersion` read back `0.30.0`,
`recurring_rule_timezone` auto-resolved correctly,
live playback of Channel A came back clean with no errors. Hit two build/
deploy issues specific to this platform, both now written up in
`docs/BUILDING.md`: the live-checkout build harness silently no-ops
against updated source unless two separate stale-marker locations are
cleared first (not just one, and not just after a genuine failure --
already-documented advice that turned out incomplete), and launching
the Kodi Flatpak non-interactively over SSH needs the real logged-in
session's display environment exported first or Kodi's own process
crashes on startup (a pre-existing documented gotcha, just re-hit
here).
**Update: also confirmed on macOS (2026-09-09).** No persistent build
workspace survived from earlier sessions, so this was a from-scratch
setup: a fresh Kodi source checkout plus a separate local copy of the
addon (not the real repo directly), with `addon-defs` pointing at that
copy and synced from the real repo via `rsync -az --delete` first, same
pattern as Windows/Linux. Built cleanly on the first attempt --
a brand-new `ExternalProject` tree has no stale stamps to clear, so the
live-checkout stale-marker gotcha above didn't actually come up this
time. Same checks, same clean result: addon reports `0.9.2` via
`Addons.GetAddonDetails`, `PVR.BackendVersion` read back `0.30.0` via
`XBMC.GetInfoLabels`, `recurring_rule_timezone` auto-resolved correctly. Live-playback
smoke test needed a
second attempt: the first channel tried was a placeholder EVENT-type
channel with no real stream behind it and failed with the addon's own
generic "ffmpeg exited before producing any segments" error -- not a
regression, just a dead test channel; switching to Channel A gave a
clean fresh-buffer open with no errors in `kodi.log`. All four
platforms (Windows, CoreELEC/ODROID, Linux, macOS) are now
confirmed current on `0.9.2`.
**Update: (3) and (4) considered and deliberately not pursued
(2026-09-09).** System notifications: Kodi GUI notifications are
toast-style interruptions over whatever's currently playing --
version-available/setting-recommendation chatter is exactly the kind
of backend-admin noise that has no business popping up mid-playback in
a living-room context, and this was only ever "plausible, secondary,
no design work done" to begin with, never a real commitment. Channel
profiles: if the filtering already happens server-side (Dispatcharr's
own admin UI), a Kodi-side profile picker is a second UI for the same
thing -- the one scenario where it would've earned its keep is
multiple Kodi devices sharing one Dispatcharr account wanting
*different* subsets (a device-level need account-level filtering can't
express), but that's theoretical here: this instance's channel
profiles are empty, not an actual gap for the current setup. Neither
ruled out permanently -- revisit if either premise changes (e.g. a
second Kodi device actually wants a different lineup than the main
one).

#### Three recording-management features (rename, file size, extend in-progress)

**Three recording-management features TVHeadend has that this addon
doesn't, all confirmed implementable against Dispatcharr's real API
(found 2026-09-08, all three implemented by 2026-09-09).** Recording rename/
description edit (`POST /api/channels/recordings/{id}/update-metadata/`,
writes into the exact `custom_properties.program.*` fields already
read on the way in), recording file size (not a JSON field, but a
`HEAD` request against `/api/channels/recordings/{id}/file/` gets it
from `Content-Length`), and extending an in-progress recording (`POST
.../extend/`, a real dedicated endpoint this addon currently doesn't
call at all). Rename is the most user-visible and cleanest to add.
Two other candidates (recording undelete, per-recording retention)
were checked and ruled out -- Dispatcharr's `DELETE` is immediately
destructive with no trash table, and there's no retention/lifetime
concept anywhere in its API. A fourth (resume position/play count)
has no server-side backing either; only implementable as a weaker,
addon-local-only hack, not pursued. Full write-up, including how each
was confirmed against Dispatcharr's actual source (not just its
OpenAPI schema, which mis-describes this endpoint's request body) in
`docs/RECORDINGS.md`'s "Recording-management feature gaps vs.
TVHeadend" section.
**Update: rename implemented and confirmed live (2026-09-09).**
`SetSupportsRecordingsRename(true)` plus a `RenameRecording()`
callback calling `DispatcharrClient::RenameRecording()` (new). Tested
end-to-end via Kodi's own GUI (its rename dialog, not JSON-RPC --
Kodi has no JSON-RPC method for this at all): renamed a real
in-progress-turned-stopped recording from its real EPG-sourced show
name to "RENAMETEST",
confirmed both through Kodi's own `PVR.GetRecordings` and directly
against Dispatcharr's REST API -- `custom_properties.program.title`
updated to the new value, `user_edited: true` set, description left
untouched (confirming the addon correctly sends only `{"title": ...}`
for a pure rename). File size and extending an in-progress recording
remain unimplemented.
**Update: file size implemented and confirmed live (2026-09-09).**
Turned out simpler than originally scoped above -- no `HEAD` request
needed at all. `custom_properties.bytes_written` is already present in
the same `GetRecordings()` payload this addon already fetches;
confirmed against Dispatcharr's real source
(`apps/channels/tasks.py`) that it's a sum of the recording's HLS
segment file sizes, written once at finalization, not updated live
during an active recording. `Recording::bytesWritten` parses it
(defaulting to 0 when the key is absent), `SetSupportsRecordingSize(true)`
plus `PVRRecording::SetSizeInBytes()` surface it. Tested live end to
end on Windows across the full lifecycle of a real instant recording:
in-progress (`custom_properties` genuinely lacks the key, confirmed via
temporary debug logging -- 0 shown), just-stopped-not-yet-finalized
(key still absent), and finalized (key present, real value). Kodi's own
GUI showed the result two ways -- the recordings list's per-folder
"Total: <its size>", and a dedicated "Size: <its size>" line in the
recording's own info panel -- both matching the raw byte count
(its true byte count) exactly. Also confirmed the boundary case: a recording
that never captured real stream data (`status=interrupted`, a test
channel with no live backing) correctly showed `bytesWritten=0` and
Kodi's info panel omitted the Size line entirely rather than showing a
misleading zero.
**Update: extending an in-progress recording implemented and
confirmed live (2026-09-09) -- all three original items now done.**
Routed through `GetTimers()`'s existing `PVR_TIMER_STATE_RECORDING`
timer for an in-progress recording: editing its end time in Kodi's
Timers window now calls new `DispatcharrClient::ExtendRecording()`
(`POST .../extend/`, `{"extra_minutes": N}`) instead of the generic
reschedule PATCH a not-yet-started timer still uses. That distinction
mattered: checked against the real `extend` endpoint's own source
first and found it deliberately bypasses Django's `pre_save` signal
because that signal revokes the running Celery recording task -- a
plain PATCH against an already-recording item would have stopped it,
not extended it. Tested live: extended a real in-progress recording's
end time by 15 minutes via Kodi's actual Timer-edit dialog, confirmed
via `PVR.GetTimers` that the end time moved forward *and* `state`
stayed `"recording"` throughout -- direct proof the task kept running
rather than being revoked. Full account, including the numeric-pad
GUI quirks hit along the way, in `docs/RECORDINGS.md`.

#### Smoke-test pass completed on all four platforms

**All four platforms now have a completed smoke-test pass** (Windows,
Linux, CoreELEC/ODROID, macOS) -- Local timeshift mode is
confirmed live on all four, closing out what was the last real gap in
platform coverage.

#### Automated test suite

**No automated test suite exists (requested 2026-09-10) -- both
plugins' and the C++ addon's known Kodi/Dispatcharr-independent pure
logic now fully covered (2026-09-13); only the Redis/Django/Kodi-ABI
boundary work remains, by design.** Two genuinely separable problems,
since this addon can't be compiled standalone (needs Kodi's own
binary-addon build harness) while the two companion plugins are plain
Python with no such constraint:
- **Python plugins** (`dispatcharr-plugin/recording_edl`,
  `dispatcharr-plugin/timeshift_buffer`): the lower-effort starting
  point -- `pytest` (pinned in `dispatcharr-plugin/requirements-dev.txt`,
  configured via root `pyproject.toml`, wired into CI's
  `unit-tests-python` job) alongside the existing `ruff.toml`. Both
  plugins already have real, isolated bugs that were verified with
  one-off manual test scripts during development (e.g.
  `docs/RECORDING_EDL.md`'s `_parse_edl()` nan/inf fix, "Verified
  with a test reproducing the exact pre-fix crash") -- a real pytest
  suite would just formalize and keep that same style of test instead
  of writing it, running it once, then discarding it.
  **Update: `recording_edl/tests/test_recording_edl.py` built
  (2026-09-13), 26 test cases.** Covers `_parse_edl` (including the
  exact documented nan/inf regression), `_sidecar_base_name`,
  `_is_under_dotted_dir`, and `_prune_empty_directories` directly (zero
  Dispatcharr dependency, confirmed via each function's own code --
  `_prune_empty_directories` in particular needed no mocking at all,
  since Python's `tmp_path` fixture gives a real temporary filesystem
  per test, unlike the C++ addon side's Kodi-ABI coupling problem).
  `_dvr_sidecar_scan_roots()`'s pure per-template resolution and
  deduplication logic was extracted into new `_resolve_scan_root()`/
  `_dedupe_scan_roots()` (pure code motion, same pattern as the C++
  side's `TimeZoneUtil`/`EpgTagUtil` extractions) specifically so the
  exact 2026-09-05 bare-root-exclusion incident this project's own
  docs describe has a real regression test now, without needing
  Dispatcharr's `core.models.CoreSettings` (a deferred import inside
  the now-thin wrapper, untouched). `_scrub_orphaned_recording_sidecars()`
  itself -- the actual destructive action, including the
  dotted-directory protection from that same incident -- is tested
  end-to-end against a real temp filesystem by `monkeypatch`ing just
  `_dvr_sidecar_scan_roots` (the one call with a Django dependency),
  not by re-testing its already-covered pure helpers separately.
  Deliberately stops at the same boundary as the C++ side: anything
  touching Dispatcharr's Django models directly
  (`_classify_dvr_hls_dir`/`_list_dvr_hls_staging_dirs`/
  `_delete_orphaned_dvr_hls_dirs`, and `Plugin.run`'s `get_edl`/HLS
  action dispatch) stays untested -- would mean faking Django's ORM
  surface, the same fragile-mock problem flagged for Kodi's own addon
  ABI on the C++ side.
  **Update: `timeshift_buffer/tests/test_timeshift_buffer.py` built
  (2026-09-13), 42 test cases -- the bigger, different-shaped piece
  this item originally flagged (redis, an embedded HTTP server, ffmpeg
  subprocess management) turned out to have a real Kodi-independent
  pure-logic surface just like `recording_edl` did.** Covers
  `_channel_dir` (a real security boundary -- rejects a non-UUID
  `channel_uuid`, e.g. a path-traversal attempt, before it ever reaches
  a `mkdir`/`rmtree` call), `_BufferRequestHandler._parse_range` (HTTP
  Range-header parsing: explicit/open-ended/suffix ranges, clamping,
  unsatisfiable-range rejection, multi-range's documented
  first-range-only behavior), `_proxy_url`, `_prune_stale_viewers`
  (already had a `now` param "purely for testability", same pattern
  this project's own C++ `ComputeKnownZoneOffsetMinutes()` uses), and
  `_find_orphaned_channel_dirs`/`_scrub_orphaned_dirs` (via
  `monkeypatch`ing their one Redis-dependent call, real filesystem
  scan/removal logic against `tmp_path`).
  `_BufferRequestHandler._resolve_path()`'s traversal-guard logic was
  extracted into new `_resolve_request_path()` (pure code motion, same
  pattern as `recording_edl`'s extractions) since it needed a real
  HTTP request/server instance to call as an instance method --
  includes a real symlink-escape regression test (a request path with
  no literal `..` that still resolves outside `storage_path` via a
  symlink), confirming the `.resolve()`-based containment check
  catches what the string-level `..` guard alone wouldn't.
  `_stream_attribution_headers()` is tested for its `client_ip`
  validation path only -- including the exact documented header-
  injection security fix (`docs/TIMESHIFT.md`'s "client_ip header
  injection" section) -- since its username/JWT branch's Django import
  is deferred inside `if username:` and simply never executes when no
  test passes a `username` param, no mocking needed to stay clear of
  it.
  `_get_live_manifest()` -- state is passed in directly rather than
  fetched from Redis internally, so it turned out to have zero
  Redis/Django dependency at all -- is tested end-to-end against a real
  temp filesystem (real `.m3u8` + segment files), including regression
  tests for both cache-invalidation incidents this function's own
  docstring documents at length: the newest-segment-always-restatted
  guard (pre-seeding `_manifest_cache` with a wrong size for what will
  be the newest segment, confirming the real stat wins) and the
  instance-token mismatch guard (pre-seeding a cache entry whose
  `playlist_mtime_ns`/`playlist_size` exactly match the real file but
  whose `instance_token` doesn't match `state["access_token"]`,
  confirming a full reparse happens instead of trusting it -- the exact
  "Packet corrupt" incident shape). `_manifest_cache` is real
  module-global mutable state, so an autouse fixture clears it around
  every test.
  Still untested, same boundary as `recording_edl`: `_redis`/
  `_get_buffer_state`/etc., the real HTTP server (`_BufferRequestHandler.do_GET`/
  `do_HEAD`/`_check_access_token`, `_BufferHTTPServer`), ffmpeg
  subprocess management (`_start_ffmpeg`/`_stop_ffmpeg`), the reaper
  thread, and `Plugin`'s own action dispatch.
  **Update (2026-09-13): `Plugin._resolve_channel_uuid` also covered**
  -- a static method, zero Redis dependency (params/settings fallback
  plus UUID validation), the same validation pattern `_channel_dir`
  already had tested but one layer up, at the `run()`-params boundary
  every action handler goes through before ever reaching a filesystem
  path. `Plugin`'s own action dispatch (`_start_buffer`/`_stop_buffer`/
  etc.) is still untested -- unlike `recording_edl`'s `Plugin.run()`,
  none of `timeshift_buffer`'s action handlers are Django/Redis-free,
  so extending coverage there the same way isn't a given.
  **Update (2026-09-13): two more pure pieces found inside
  `_start_ffmpeg`/`_is_process_alive`, both previously only reached
  indirectly (or not at all).** New `_compute_segment_counts(buffer_minutes,
  segment_seconds)` pulls `_start_ffmpeg`'s `visible_segments`/
  `wrap_segments` sizing math out on its own -- worth doing specifically
  because `docs/TIMESHIFT.md` already cited this exact formula
  (`visible_segments * segment_seconds == buffer_minutes * 60`) as
  live reasoning to rule out a suspected ~89s audio-sync-error
  correlation during a real investigation; a test now locks in that
  invariant directly rather than leaving it as an unverified claim in
  a doc. Also covers the `max(1, ...)` floor for a `segment_seconds`
  longer than the whole configured buffer window. `_is_process_alive`
  (previously only ever `monkeypatch`ed away by other tests, never
  exercised itself) is now tested directly via `monkeypatch`ing its
  one external dependency, `os.killpg` -- including the documented
  deliberate choice to treat any failure other than
  `ProcessLookupError` (e.g. a `PermissionError` against a recycled,
  unrelated pid) as "still alive," to avoid reaping something still
  running. 10 new test cases, 56 total in `timeshift_buffer`'s own
  suite now.
  Both plugins' Kodi-independent pure/filesystem logic is now covered;
  what remains untested on the Python side is exactly the
  Redis/Django/real-process boundary, by design.
  **Update (2026-09-13): `recording_edl`'s own `Plugin.run()` dispatch
  now covered too, for every action branch that's Django-free or where
  the one Django-dependent call can itself be `monkeypatch`ed** (the
  same technique already used for `_scrub_orphaned_recording_sidecars`,
  just one layer up the call chain) -- `scrub_orphaned_sidecars` end to
  end, `list_dvr_hls_staging_dirs`/`delete_orphaned_dvr_hls_dirs`'s
  message formatting (classification-count summaries, the
  directory/directories singular-plural text, error-count appending)
  via monkeypatching `_list_dvr_hls_staging_dirs`/
  `_delete_orphaned_dvr_hls_dirs` themselves rather than their Django
  internals, an unknown action's error response, and `get_edl`'s
  `recording_id`-missing validation (the deferred `Recording` import
  happens *after* that check, so it's Django-free too). This tests the
  actual response dict/message text a client sees, not just the
  underlying helper functions in isolation. `timeshift_buffer`'s own
  `Plugin` dispatch is a separate, not-yet-done piece -- none of its
  action handlers are this Django-light, so extending it the same way
  isn't a given.
  **Update (2026-09-13): `_classify_dvr_hls_dir`'s own classification
  decision logic now covered too, not just its downstream message
  formatting.** Previously only reached indirectly, via `run()`'s
  `list_dvr_hls_staging_dirs`/`delete_orphaned_dvr_hls_dirs` tests
  monkeypatching the whole function away. New
  `_classify_hls_dir_info(hls_dir, recording_id, recording_exists,
  custom_properties, segment_count)` pulls the actual
  `active`/`preserved_failure`/`referenced`/`orphaned` decision tree
  out of `_classify_dvr_hls_dir`, taking the already-resolved
  `recording_exists`/`custom_properties` instead of querying Django's
  `Recording` model directly -- `_classify_dvr_hls_dir` itself becomes
  a thin wrapper doing just the regex match, the Django query, and the
  segment-count filesystem scan. 6 new test cases cover all four
  classifications, including the `preserved_failure` case this
  function's own docstring flags as the one that must never be treated
  as safe to delete (a failed concat/remux leaves Dispatcharr
  deliberately keeping the directory as the only surviving copy of
  that recording's video) and a Recording row whose own `_hls_dir`
  points somewhere else entirely (the "needs manual review" case,
  distinct from a genuinely missing Recording row). 41 total test
  cases in `recording_edl`'s own suite now.
- **C++ addon**: don't attempt to test `PVRDispatcharr`/
  `DispatcharrClient` wholesale -- that would mean mocking Kodi's
  entire addon-instance API and/or standing up a fake Dispatcharr
  HTTP server, disproportionate effort for what's fundamentally still
  manual/live-hardware verification territory (per this project's own
  established `CLAUDE.md`/README convention). Instead, add a small,
  separate CMake target/executable (a header-only framework like
  Catch2 would avoid a real dependency) that compiles and tests only
  the Kodi/Dispatcharr-independent pure-logic pieces already living in
  `src/` -- `XmlTvParser`'s field extraction, the recurring-rule
  timezone offset math (`ComputeKnownZoneOffsetMinutes()` and its
  nth-weekday/last-weekday helpers), `MapCategoriesToGenreType()`'s
  keyword scan, and the broadcast-id hash in `GetEPGForChannel()`.
  None of these touch `kodi::`-namespaced types, so none need the
  Kodi ABI at all -- a real, buildable-standalone test target, not a
  redesign of the addon's own architecture.
  **Update: `XmlTvParser`/`TimeUtil` piece built (2026-09-13).**
  `tests/` is a standalone Catch2 project (`tests/CMakeLists.txt`,
  deliberately not a subdirectory of the main `CMakeLists.txt`, which
  can only configure through Kodi's own build harness), wired into
  CI's new `unit-tests` job -- 13 test cases / 60 assertions covering
  `XmlTvParser::Parse()`'s field extraction (credits joining, xmltv_ns
  episode-num parsing including partial/multipart values, timezone
  offset handling, malformed/missing-channel/no-`<tv>`-root error
  paths) and `TimeUtil`'s `PortableTimeGm()`/`GmTimeUtc()` (including a
  regression guard that `PortableTimeGm()` stays TZ-independent, its
  whole reason for existing per its own header comment).
  **The `ComputeKnownZoneOffsetMinutes()`/`MapCategoriesToGenreType()`/
  broadcast-id-hash piece is corrected, not yet done.** This item's
  original "none of these touch `kodi::`-namespaced types" claim
  doesn't hold for two of the three once actually checked against the
  real `#include`s: `MapCategoriesToGenreType()` and `GetEPGForChannel()`
  live in `PVRDispatcharr.cpp`, which pulls in the *full*
  `<kodi/addon-instance/PVR.h>` (via `PVRDispatcharr.h`) -- a much
  heavier, more version-sensitive dependency than `DispatcharrClient.cpp`'s
  `<kodi/General.h>` alone, and one that would mean either vendoring
  Kodi's PVR dev-kit headers into the test build or checking out Kodi
  source in CI, undermining the whole "fast, no Kodi checkout" point of
  this suite. The broadcast-id hash also isn't its own function yet --
  it's inline inside `GetEPGForChannel()`, which is a real
  `CInstancePVRClient` member doing far more than hashing (cache
  access, transferring entries back to Kodi); testing it standalone
  would need extracting it into its own small function first, a real
  (if small) refactor, not a pure test-only addition.
  **Update: `ComputeKnownZoneOffsetMinutes()` piece built (2026-09-13),
  via a third option neither of the two originally considered.** Rather
  than stubbing Kodi's `AddonGlobalInterface` or adding an injectable
  logging abstraction to sidestep `DispatcharrClient.cpp`'s unrelated
  `kodi::Log()` calls, the DST logic itself (`NthWeekdayOfMonth()`/
  `LastWeekdayOfMonth()`/`IsUsCanadaDstInEffect()`/`IsEuDstInEffect()`/
  `kKnownTimeZones`/`ComputeKnownZoneOffsetMinutes()`) moved into its
  own new `src/TimeZoneUtil.{h,cpp}`, matching the existing precedent
  for exactly this reason (`TimeUtil.h`'s own header comment: "pulled
  out here... since both are meant to stay self-contained"). Pure code
  motion, no logic change -- `DispatcharrClient::ComputeKnownZoneOffsetMinutes()`
  (the public static method every existing caller already uses) is now
  a one-line delegate to the new free function, so nothing outside this
  file needed to change. Confirmed the real addon still compiles
  correctly post-refactor via the actual Kodi binary-addons harness
  (not just the standalone test build). New `tests/test_timezone_util.cpp`
  covers both DST families at their exact real 2026 transition instants
  (computed independently via `date -u`, not derived from the code under
  test) plus fixed-offset zones and an unrecognized-zone case -- 18 test
  cases / more assertions total across the whole suite now, still 0 Kodi
  checkout needed.
  **Update: `MapCategoriesToGenreType()`/the broadcast-id hash also built
  (2026-09-13) -- both turned out easier than this item's own prior
  "not done" writeup assessed, on closer inspection.** Two corrections
  to that writeup, not just new work:
  - The `<kodi/addon-instance/PVR.h>` concern conflated "the *file*
    `PVRDispatcharr.cpp` includes the heavy C++ header" with "the
    *function* needs it" -- they're not the same thing.
    `EPG_EVENT_CONTENTMASK_*`'s actual definition
    (`kodi/c-api/addon-instance/pvr/pvr_epg.h`, confirmed by reading it
    in a real Kodi checkout) is a leaf, plain-`extern "C"` header (its
    own only include, `pvr_defines.h`, has zero includes of its own) --
    nothing like the full `CInstancePVRClient` class hierarchy. Ended up
    not even depending on that header, though: since these values are
    the ETSI EN 300 468 DVB-SI content-descriptor top nibble (an
    external broadcast standard Kodi's header just mirrors 1:1, not a
    Kodi-specific value that could drift on its own), new
    `src/EpgTagUtil.{h,cpp}` hardcodes them directly with a citation
    comment -- zero Kodi SDK dependency at all, matching
    `XmlTvParser`/`TimeUtil`/`TimeZoneUtil` exactly, not just a lighter
    one.
  - The broadcast-id hash turned out to already be a fully
    self-contained expression (`channelUid`/`entry.startTime` only, no
    other `GetEPGForChannel()` state) -- extracting it into
    `ComputeBroadcastId()` in the same new file was direct, not the
    refactor-first blocker this item previously described.
  `PVRDispatcharr.cpp`'s `GetEPGForChannel()` now just calls both as
  `dispatcharr::MapCategoriesToGenreType()`/`dispatcharr::ComputeBroadcastId()`
  -- pure code motion, confirmed the real addon still compiles via the
  actual Kodi binary-addons harness post-refactor. New
  `tests/test_epg_tag_util.cpp` covers every genre-keyword family,
  case-insensitivity, substring matching, the documented
  scan-all-categories-not-just-the-first behavior, a concrete
  hand-computed broadcast-id value (locks the exact hash algorithm in
  place against an accidental future change), and a direct regression
  test for the specific 65536-second-collision bug
  `docs/EPG.md` documents this hash as the fix for.
  All four original candidates from this item are now done.
  **Update (2026-09-13): the "nothing else identified" claim above
  didn't hold up -- a follow-up survey found four more.** `StringUtil.{h,cpp}`
  (`Base64Encode`/`ToLower`, pulled out of `WebSocketClient.cpp` --
  zero Kodi/curl dependency despite living in a file that includes
  `<curl/curl.h>`; `Base64Encode` backs the WebSocket handshake's
  `Sec-WebSocket-Key`, confirmed against RFC 4648's own test vectors),
  `DateTimeFormat.{h,cpp}` (`IsoFromTime`/`TimeFromIso`/`TimeOfDayString`/
  `SecondsSinceMidnightFromString`/`DateStringFromTime`/`TimeFromDateString`,
  pulled out of `DispatcharrClient.cpp` -- Dispatcharr's own date-time
  string formats, only needing `TimeUtil.h`), `UrlEncode.{h,cpp}`
  (needs a real `curl_easy_escape` call at runtime, unlike the others
  here, but no Kodi dependency -- new `tests/CMakeLists.txt` now also
  `find_package(CURL REQUIRED)`s, same module-mode setup as the main
  addon build), and `JsonFieldUtil.h` (the `FieldOr<T>()` template --
  header-only, 48 call sites across `DispatcharrClient.cpp`, the
  widest blast-radius of any candidate found so far since it's what
  nearly every response field read goes through; new
  `tests/CMakeLists.txt` now also `FetchContent`s `nlohmann/json`,
  same `v3.11.3` pin as the main addon build). 54 test cases total
  across the whole C++ suite now. Confirmed the real addon still
  compiles via the actual Kodi binary-addons harness after each
  extraction, same verification discipline as every prior C++
  extraction this item tracks. Lesson for next time: don't trust a
  "nothing else remains" claim in this file without re-surveying --
  the same caution `CLAUDE.md`'s own data-hardening convention already
  learned the hard way for a different kind of "we checked, it's
  done" claim.
  **Update (2026-09-13): one more found, again after "nothing else
  remains" -- `FixedBufferWriteCallback`/`RecordingHeaderCallback`/
  `ContentLengthHeaderCallback`/`WriteCallback`, all plain libcurl
  `CURLOPT_WRITEFUNCTION`/`CURLOPT_HEADERFUNCTION` callbacks in
  `DispatcharrClient.cpp`.** None of the four touch a `CURL*`
  themselves -- curl calls them during a live transfer, but nothing
  stops calling them directly with synthetic data, so they moved into
  new `src/CurlCallbacks.{h,cpp}` with zero new dependency (not even
  curl headers, despite conceptually being curl callbacks). Real
  parsing logic that was previously only exercised through live HTTP:
  `FixedBufferWriteCallback`'s capacity-clamping (verified it returns
  the *full* byte count to curl even when only part of it actually fit
  in the buffer -- documenting curl's own contract as currently
  implemented, not asserting it's ideal), and the two header
  callbacks' case-insensitive `Content-Range`/`Content-Length` parsing
  including their malformed-input fallback paths (no slash, non-numeric
  size). 14 new test cases, 68 total across the C++ suite now.
  **Update (2026-09-13): two more, both with real documented incidents
  behind them -- `EstimateSegmentDurationMs<SegmentT>`/`ComputeCatchUpAttempts`,
  shared by `ReadInProgressRecordingStream()` and
  `ReadLiveTimeshiftStream()`'s catch-up-to-tail wait.** Moved into new
  header-only `src/CatchUpUtil.h` (the former is a template, so it has
  to stay header-only anyway) -- `EstimateSegmentDurationMs` only needs
  a `timeOffsetMs` member on its `SegmentT`, so the test uses a minimal
  local struct rather than depending on either real (heavier) segment
  type. Test cases include a direct regression for the exact incident
  the function's own comment documents: a real instance once produced
  a burst of segments only ~151ms apart, which without the averaging +
  floor would have collapsed the retry budget until ffmpeg read the
  resulting stall as genuine end-of-stream and closed playback outright
  -- and for `ComputeCatchUpAttempts`, the 3x-vs-1.5x margin fix
  (confirmed live that 1.5x ran too thin under ordinary jitter). 9 new
  test cases, 77 total across the C++ suite now.
  **Update (2026-09-13): one more, `PVRDispatcharr::ComputeRecurringRuleFields`'s
  pure integer-arithmetic core.** Unlike the other `PVRDispatcharr.cpp`
  extractions above, this one took a `kodi::addon::PVRTimer&` and called
  `time(nullptr)` directly, so the extraction also had to change its
  signature -- the new `dispatcharr::ComputeRecurringRuleFields()` in
  `src/RecurringRuleUtil.{h,cpp}` takes plain `time_t`/`unsigned int`
  values plus an explicit `nowUtc` parameter (used only when `firstDay`
  is `<= 0`, meaning Kodi didn't supply one) instead. Confirmed this is
  still pure code motion, not a behavior change: the member function
  that callers (`AddTimer()`/`UpdateTimer()`) actually call is now a
  thin wrapper passing `timer.GetStartTime()`/`GetEndTime()`/`GetFirstDay()`/
  `GetWeekdays()` and a live `time(nullptr)` through unchanged. The
  logic itself -- converting Kodi's UTC-based weekday bitmask/start-end-
  time-of-day/first-day into Dispatcharr's own representation via a
  UTC time_t's own modulo-86400 split (exact and DST-free, no
  gmtime/timegm round-trip needed) plus the one real timezone shift
  (`EffectiveRecurringRuleUtcOffsetMinutes()`, bridging to Dispatcharr's
  own non-UTC-by-default system timezone) -- is untouched. New
  `tests/test_recurring_rule_util.cpp` covers normal weekday/time/date
  conversion, collecting multiple selected weekdays in bitmask order,
  the "no weekday selected" validation error, the UTC offset shift
  applied to both start and end seconds, an out-of-range (negative)
  result left unwrapped for the caller to handle (matching
  `TimeOfDayString()`'s own documented contract), and the `firstDay <=
  0` fallback to `nowUtc` (covering both zero and a negative value,
  since Kodi's own "no first day set" sentinel wasn't confirmed to
  always be exactly zero). 7 new test cases, 84 total across the C++
  suite now. Confirmed the real addon still compiles via the actual
  Kodi binary-addons harness post-refactor, same verification
  discipline as every prior C++ extraction this item tracks.
  **Update (2026-09-13): one more, `DispatcharrClient::ParseRecordingJson`'s
  pure field-mapping core -- a real bug-history-backed piece, unlike a
  few of the smaller mechanical extractions above.** New
  `dispatcharr::ParseRecordingFields()` in `src/RecordingParser.{h,cpp}`
  covers the id/channel/time-window field mapping, the
  `custom_properties.status`-over-time-window `isInProgress` override
  (the exact documented incident: a recording stopped early keeps its
  originally-scheduled `end_time`, so the time-window check alone kept
  reporting it in-progress for the rest of that window --
  `docs/RECORDINGS.md`), `hlsDirStillPresent`, `bytesWritten`, and the
  `custom_properties.program`-then-flat title/subtitle/description
  fallback chain. Confirmed `DispatcharrClient.h` itself (where
  `Recording` is defined) has zero Kodi/curl dependency despite living
  right next to a class that has plenty -- it deliberately keeps
  curl-typed members as `void*` specifically to avoid needing
  `<curl/curl.h>` in the header (see `GetCurlShare()`'s own comment) --
  so the new header can `#include` it directly with no new test-build
  dependency. Deliberately does NOT extract the two pieces that
  genuinely need `DispatcharrClient`'s own state: the `PendingTitle`
  cache lookup (member state behind `m_pendingTitlesMutex`, matched by
  channel with no clean way to pass in short of duplicating that whole
  mechanism) and the final `"Recording <id>"` default title -- both
  stay in `ParseRecordingJson()` itself, now a thin wrapper that calls
  the new free function first. Pure code motion otherwise, confirmed
  via `git diff` and a rebuild through the real Kodi binary-addons
  harness. 12 new test cases, 96 total across the C++ suite now.
  **Update (2026-09-13): two more, both found by re-surveying
  `DispatcharrClient.cpp`/`PVRDispatcharr.cpp` a further time rather
  than trusting the prior "nothing else identified" pass.**
  `DispatcharrClient::UnwrapPluginRunResult()` (shared by every
  companion-plugin `run/` caller: `CallTimeshiftPluginAction()`,
  `StopTimeshiftBuffer()`, `GetRecordingEdl()`, `RefreshLiveManifest()`
  -- 4 call sites) already had zero member-state dependency at all, so
  unlike every extraction above it moved out into new
  `src/PluginRunResult.{h,cpp}` *entirely*, with the private class
  declaration removed rather than kept as a delegating wrapper --
  confirmed every call site still resolves correctly unqualified,
  since `DispatcharrClient.cpp`'s own `namespace dispatcharr { ... }`
  wrapper makes normal namespace-scope lookup find the free function
  once the member overload is gone. Checks the outer
  `{"success", "error"}` envelope every plugin `run/` response is
  wrapped in, then the plugin's own inner `{"status", "message"}`
  result -- a real two-layer response shape, not a hypothetical one.
  6 new test cases, including one that caught this session's own test
  mistake, not a code bug: an assumption that an absent `"result"` key
  left `resultOut` as an empty JSON *object* rather than JSON `null`
  (`json()`'s actual default-construction value, unchanged pre-existing
  behavior) -- caught by the test itself failing, fixed in the test,
  not the extracted code.
  `PVRDispatcharr::HandleRealtimeUpdateMessage()`'s wire-shape/
  relevant-event-type classification also moved out, into new
  `src/RealtimeUpdateParser.{h,cpp}` (`ParseRelevantRealtimeUpdateEventType()`)
  -- confirmed against Dispatcharr's own `consumers.py`/`utils.py`
  (the `{"type": "update", "data": {"type": "<event>", ...}}` wire
  shape) and `apps/channels/tasks.py`/`api_views.py` (which event
  names on that shared "updates" channel are actually recording/timer-
  relevant, versus EPG matching progress/M3U refresh/stream stats,
  which must be silently ignored rather than erroring). Kept as a
  delegating wrapper this time (`HandleRealtimeUpdateMessage()` still
  does the `m_debugLogging`-gated `kodi::Log()` call and the two
  `InvalidateAndTrigger*Update()` member calls afterward). 5 new test
  cases, 107 total across the C++ suite now. Both extractions
  confirmed as pure code motion via `git diff` and a rebuild through
  the real Kodi binary-addons harness.
  **Update (2026-09-13): one more, `RefreshInProgressRecordingManifest()`'s
  `#EXTINF`/segment-URI playlist scan -- the most complex piece
  extracted from `DispatcharrClient.cpp` so far, but already fully
  self-contained (touches zero class members, only local variables) so
  the extraction itself needed no signature changes at all.** New
  `dispatcharr::ParseNewM3u8SegmentEntries(playlistText, baseDir,
  alreadyKnownCount)` in `src/M3u8SegmentParser.{h,cpp}` covers the
  append-only-merge skip-count convention (no rolling-window eviction
  for a recording, so segments at or before `alreadyKnownCount` are
  always the same ones already known), relative-vs-absolute segment
  URL resolution, and -- the actual reason this was worth pulling out
  on its own, not just a mechanical move -- the non-finite (`NaN`/
  `inf`) `#EXTINF`-duration guard the original code's own comment
  already flagged as a real (if never live-reproduced) undefined-
  behavior risk: `std::stod` accepts `"inf"`/`"nan"` as valid input
  without throwing, unlike `std::stoi`, so a plain `try`/`catch`
  around the parse can't be relied on to keep the result finite before
  it feeds a `static_cast<int64_t>()` later. 10 new test cases,
  including explicit `inf`/`nan`/`-inf` regression cases for exactly
  that guard, plus CRLF line endings, a missing trailing newline, and
  the empty-playlist case. 117 total across the C++ suite now.
  Confirmed pure code motion via `git diff` and a rebuild through the
  real Kodi binary-addons harness.
  **Update (2026-09-13): an exhaustive line-by-line read of the three
  remaining large C++ files (`DispatcharrClient.cpp`,
  `PVRDispatcharr.cpp`, `WebSocketClient.cpp` -- prompted by "why does
  every survey pass keep finding more?"; answer: prior passes only
  grepped for standalone-looking function signatures, which misses
  pure logic buried inside larger, mixed-purity member functions)
  found 8 further real candidates, roughly three PR-sized batches.
  First: `FindSegmentContainingPosition<SegmentT>()`
  (`src/SegmentLookup.h`, header-only, same templating approach as
  `CatchUpUtil`) -- the "find the segment containing this byte
  position" linear scan was identical, duplicated code in both
  `ReadInProgressRecordingStream()` and `ReadLiveTimeshiftStream()`;
  now one shared template. 6 new test cases, 123 total across the C++
  suite now.
  **Update (2026-09-13): second batch done -- the per-item
  JSON-to-struct mapping loops and the series-rule client-index
  hash.** New `src/ChannelParser.{h,cpp}` (`ParseChannelJson()`/
  `ParseChannelGroupJson()`, used by `GetChannels()`/
  `GetChannelGroups()`) and `src/TimerRuleParser.{h,cpp}`
  (`ParseTimerRuleJson()`/`ParseRecurringRuleJson()`, used by
  `GetTimerRules()`/`GetRecurringRules()`) follow the exact
  `RecordingParser` pattern -- each preserves real documented fallback
  chains (`channel_group` nested-object-or-bare-id, `tvgId`'s nested-
  `epg_data`-then-effective-then-plain chain, `title`/`title_pattern`
  for a series rule, `days_of_week`'s integer-only array filter).
  `RecordingParser` itself also gained `ParseRecordingEdlEntryJson()`
  (used by `GetRecordingEdl()`), since it's the same recording domain
  as the rest of that file. Separately, `PVRDispatcharr::GetTimers()`'s
  `std::hash`-based series-rule identity scheme moved into new
  `src/TimerIdentity.h` (`ComputeSeriesRuleClientIndex()`, header-only)
  -- deliberately not asserting an exact hash value in its tests (only
  guaranteed stable within one running process, not across compilers/
  platforms), instead testing the masking behavior itself (the
  `0x40000000` flag bit always set, `0x80000000` never set) and that
  different inputs produce different indices. 29 new test cases, 152
  total across the C++ suite now. All confirmed as pure code motion
  via `git diff` and a rebuild through the real Kodi binary-addons
  harness. Remaining 5 candidates from the original 8 (live-manifest
  segment parsing, live-edge trim/rebase, live-edge seek backoff, a
  recurring-rule renewal decision, and WebSocket frame encoding) still
  tracked for follow-up passes.
  **Update (2026-09-13): third batch done -- `RefreshLiveManifest()`'s
  segment-merge filtering, the live-timeshift counterpart to
  `M3u8SegmentParser` above.** New `src/LiveManifestParser.{h,cpp}`
  (`ParseNewLiveManifestSegments()`) covers the same two real concerns
  as its in-progress-recording sibling but for the `get_live_manifest`
  plugin response's JSON `"segments"` array instead of a raw m3u8
  playlist: keeping only sequences newer than the highest one already
  merged, and dropping a malformed entry (empty filename or
  non-positive `byte_size`) before it can corrupt the caller's own
  cumulative byte/time offsets for every segment merged after it.
  Deliberately does NOT compute `byteOffset`/`timeOffsetMs` itself
  (cumulative over the caller's own running totals, not available to a
  free function) -- `RefreshLiveManifest()` still assigns those while
  applying each returned entry in order, same split already used for
  the in-progress-recording sibling. 8 new test cases, 160 total
  across the C++ suite now. Confirmed pure code motion via `git diff`
  and a rebuild through the real Kodi binary-addons harness -- this
  time actually reading the harness log body rather than trusting the
  wrapper `make` command's own exit code, after the previous batch's
  extraction shipped a real miss (a call site missing its new header's
  `#include`) that the wrapper's always-0 exit code silently let
  through locally; only caught by CI's `build-windows` job, fixed in a
  follow-up commit on that same PR before merging.
  **Update (2026-09-13): fourth batch done -- the live-edge margin
  math, unifying two of the three remaining candidates into one file
  since they turned out to be the same computation.** New
  `src/LiveEdgeMargin.h` (header-only, two templates duck-typed the
  same way as `SegmentLookup`/`CatchUpUtil`): `ComputeLiveEdgeTailTarget()`
  is a forward-seek clamp target backed off from the true tail by the
  combined size of the trailing N segments -- turned out to be the
  *exact same formula* `SeekInProgressRecordingStream()` (margin 1)
  and `SeekLiveTimeshiftStream()` (margin 3) had each independently
  reimplemented, each with its own real confirmed-live incident behind
  the backoff (see each function's own comment: a "skip ahead to live
  took ~10s" investigation, and separately a live-edge-only H.264
  decode-error/audio-desync storm documented in
  `docs/TIMESHIFT.md`'s "Packet corrupt" section) -- now one shared
  function instead of two. `TrimToTrailingLiveEdgeMargin()` is
  `OpenLiveTimeshiftStream()`'s own cold-start trim/rebase, confirmed
  live as necessary to avoid a `CDVDDemuxFFmpeg::SeekTime` landing
  near the MPEG-TS 33-bit PTS wraparound point on a long-running,
  reattached buffer.
  One thing caught and corrected during this extraction, not shipped:
  an initial test exercised `TrimToTrailingLiveEdgeMargin()` with
  `marginSegments=0`, which triggered a real out-of-bounds vector
  access in the generalized template -- the original, non-generalized
  code never hit this since its one real call site always passes the
  literal `3`. Rather than add defensive handling for an input no real
  caller ever produces (this project's own "don't design for
  hypothetical requirements" convention), the test itself was removed
  and the header comment now states the `marginSegments > 0`
  precondition explicitly instead.
  8 new test cases, 168 total across the C++ suite now. Confirmed pure
  code motion via `git diff` and a rebuild through the real Kodi
  binary-addons harness, log body actually read this time. Two
  candidates remain from the original 8 -- a recurring-rule renewal
  decision and WebSocket frame encoding -- still tracked for follow-up
  passes.
  **Update (2026-09-13): fifth batch done -- the recurring-rule
  renewal decision.** New `src/RecurringRuleRenewal.{h,cpp}`
  (`ShouldRenewRecurringRule()`) covers whether a rolling recurring
  rule's `end_date` should be pushed forward this cycle: skipping a
  disabled rule, one still comfortably inside its window (more than
  half of `kRecurringRuleWindowDays` remaining), one with an
  occurrence currently recording or starting within
  `kRecurringRuleRenewalSafetyMarginSeconds` (defense in depth around
  Dispatcharr's own regeneration behavior), or -- erring toward
  skipping rather than renewing blind -- one where `GetRecordings()`
  itself failed and that occurrence-safety check can't be evaluated at
  all. Takes `windowDays`/`safetyMarginSeconds` as explicit parameters
  rather than `PVRDispatcharr`'s own private static constants (same
  pattern as `RecurringRuleUtil`'s own `offsetMinutes` parameter), so
  it's unit-testable standalone with no Kodi SDK dependency. 9 new
  test cases, 177 total across the C++ suite now. Confirmed pure code
  motion via `git diff` and a rebuild through the real Kodi
  binary-addons harness, log body read directly. One candidate
  remains from the original 8: WebSocket frame encoding, still
  tracked for a follow-up pass.
  **Update (2026-09-13): sixth and final batch done -- WebSocket frame
  encoding, closing out all 8 candidates the exhaustive read found.**
  New `src/WebSocketFrame.{h,cpp}` (`BuildMaskedControlFrame()`)
  covers the RFC 6455 masked-frame byte layout behind
  `WebSocketClient::SendPong()`/`SendClose()`: the FIN/opcode/MASK-bit
  header byte, the extended 2-byte length prefix for a payload over
  125 bytes (defensive -- never actually hit in practice, a pong
  payload always echoes a spec-capped-at-125-byte ping), and the
  mask-key XOR cycling. Takes the mask key as an explicit parameter
  rather than generating it internally (the real caller still does,
  via `RandomBytes()`, per the spec's own masking requirement), the
  same pattern `RecurringRuleUtil`'s `nowUtc` parameter and
  `WebSocketFrame`'s own sibling extractions already established, so
  the exact byte output is testable against a known key -- including a
  direct byte-for-byte comparison against `SendClose()`'s own
  original hardcoded 6-byte array, confirming the generalized builder
  reproduces it exactly. 6 new test cases, 183 total across the C++
  suite now. Confirmed pure code motion via `git diff` and a rebuild
  through the real Kodi binary-addons harness, log body read directly.
  All 8 candidates from the original exhaustive-read survey are now
  done, across six PRs (#30-#35) -- see this item's own "an exhaustive
  line-by-line read" update above for the full original list.
  **Update (2026-09-13): the same exhaustive-read treatment applied to
  both Python plugins -- `recording_edl` had no real gaps, but
  `timeshift_buffer`'s own `Plugin.run()` dispatch (flagged above as
  "not-yet-done... isn't a given") turned out to be genuinely
  testable the same way `recording_edl`'s already was.** Every action
  handler (`start_buffer`, `stop_buffer`, `heartbeat`,
  `get_live_manifest`, `list_buffers`, `stop_all`,
  `scrub_orphaned_buffers`) is now covered by `monkeypatch`ing the
  module-level Redis-touching functions
  (`_get_buffer_state`/`_set_buffer_state`/`_delete_buffer_state`/
  `_list_buffer_keys`/`_iter_buffer_states`) and process-management
  functions (`_start_ffmpeg`/`_remove_channel_files`/
  `_teardown_buffer`/`_is_process_alive`) each one calls, plus the two
  lifecycle calls `run()` itself makes unconditionally before ever
  dispatching (`_ensure_http_server_running`/`_ensure_reaper_running`,
  stubbed to no-ops via a new test-only dispatch helper -- a real HTTP
  server/reaper thread has no place in a unit test). This is real
  behavior that had zero test coverage at any level before now: the
  reference-counted stop/reattach decisions (dead-buffer cleanup and
  fresh-start on `start_buffer`, the viewer-count-based choice between
  "still active for other viewers" and a full teardown on
  `stop_buffer`), the `access_token` retrofit for state written by a
  pre-upgrade plugin version, and the `BufferFailedError`-vs-plain-
  `RuntimeError` distinction in `get_live_manifest` (only the former
  tears the buffer down and reports `fatal: true`, so a caller's
  cold-start retry loop can stop immediately instead of waiting out
  its full budget against something that will never recover).
  One thing caught and fixed during this pass, not shipped: an early
  version of the "remove one of several viewers" test used an
  obviously-stale-looking heartbeat timestamp (`100`, i.e. 1970) for
  the viewer that was supposed to stay active, which
  `_prune_stale_viewers` correctly treated as genuinely stale too --
  a mistake in the test's own fixture data, not a bug in the plugin;
  fixed by using a real `time.time()` value instead.
  30 new test cases, 127 total across the Python suite now (41
  `recording_edl` + 86 `timeshift_buffer`). Anything touching
  Dispatcharr's own Django models or Redis directly (the deferred
  imports inside `_dvr_sidecar_scan_roots`/`_classify_dvr_hls_dir`, and
  the real `_redis()` client, the real HTTP server, and real ffmpeg
  subprocess management on both plugins) stays untested, on the same
  principle as the C++ side stopping at the Kodi SDK boundary -- this
  is now believed to be the actual, final boundary on the Python side
  too, not just another "nothing else remains" claim taken at face
  value.

#### Doc linting

**No doc-linting exists (requested 2026-09-10) -- built (2026-09-10).**
Motivated directly by this session's own experience: found and fixed 9
dangling/stale references across `docs/`/`CHANGELOG.md` in one pass,
several tracing back to a feature removal or CI change that was never
cross-checked against the docs describing it (see `CLAUDE.md`'s new
convention bullet on this). An off-the-shelf markdown-link-checker
(e.g. `markdown-link-check`) would catch a genuinely different class of
bug than what this session actually found -- every dangling reference
found here was a plain-English citation (`See docs/EPG.md's "Section
Title" section`, or a backtick-quoted function/setting name), not a
broken `[text](url)` hyperlink.
`tools/check_doc_refs.py`: a small, dependency-free script specific to
this project's own citation convention, three checks against
`docs/*.md`/`CHANGELOG.md`: (1) `docs/X.md's "..." section`-style
citations (also recognizing `[X.md](X.md)`-style relative links and
this project's bold `**Title**:` paragraph markers, not just real `#`
headings) against that file's real headings; (2) backtick-quoted
`Name()` function citations against `src/*.cpp`/`*.h` *and* both
plugins' `plugin.py` (an early pass missed the plugins entirely, a real
gap, not just noise); (3) backtick-quoted setting ids on lines
mentioning "setting" against `resources/settings.xml` *and* both
plugins' own settings/action ids (a separate namespace from the Kodi
addon's settings.xml).
First real run found 78 hits after fixing several bugs in the checker
itself found via its own output (the multi-line lookback for which file
a citation belongs to, the markdown-link form, the bold-paragraph
heading style, and the missing plugin.py corpus) -- the remaining hits
are overwhelmingly genuine, correct citations to *external* code this
project's docs deliberately reference by design (Kodi-core source,
hls.js, Python stdlib/syscalls) or to deliberately-documented
historical/removed settings, not real dangling references. Confirmed by
spot-checking rather than assumed: `enable_live_timeshift` (flagged as
"setting not found") is explicitly, correctly documented in
`docs/TIMESHIFT.md` as a removed setting, replaced by
`live_timeshift_mode`. This is inherent to a lightweight static-text
heuristic given how citation-heavy this project's own docs style is by
design (see `CLAUDE.md`'s "Confirmed live" citations bullet).
**Update: baselined and wired into CI (2026-09-10).** Re-triaging the
same 76 known-legitimate hits (a couple resolved themselves once the
checker's own remaining bugs above were fixed) on every run wasn't
useful, so `tools/doc_refs_baseline.txt` records them (keyed on the
citation itself, not its line number, so an unrelated doc edit doesn't
shift the baseline) and the CI `lint` job now runs
`python3 tools/check_doc_refs.py` unconditionally, failing only on a
genuinely new dangling reference not already in the baseline --
confirmed by a throwaway test file with a fake dangling function
reference, which the script correctly flagged and failed on (exit 1),
while a real, clean run stayed silent (exit 0). `--update-baseline`
accepts new legitimate hits (a real external citation, or documenting a
newly-reverted approach) into the baseline by hand when that judgment
call is needed -- this still isn't a fully automatic pass/fail gate for
*legitimacy*, just for *novelty*.
**Update (2026-09-13): `tools/check_doc_refs.py` itself gained a real
test suite (`tools/tests/test_check_doc_refs.py`, 29 test cases),
found via the same exhaustive-read approach applied to the C++ addon
and both Python plugins -- it's real parsing/matching logic wired
into CI, and until now had zero coverage of its own despite this very
item documenting several real bugs already found and fixed in it
during development.** Covers the module's own `normalize`/
`get_headings` helpers (both plain `#` headings and the
bold-pseudo-heading convention), all three `check_*()` functions
against synthetic pytest-fixture-provided docs/src/config, and its
`load_baseline`/`write_baseline`/`main` exit-code/`--update-baseline`/
resolved-entry-reporting behavior. Includes dedicated
regression tests for the exact three bugs this item's own "First real
run" paragraph above already named: the multi-line file-lookback
within a citing paragraph, the `[X.md](X.md)`-style markdown-link
citation form, and the missing-`plugin.py`-corpus gap. Every
module-level path constant the script computes at import time against
this repo's own real layout (`REPO_ROOT`/`DOC_FILES`/`DOCS_DIR`/
`SRC_DIR`/`PLUGIN_FILES`/`SETTINGS_XML`/`BASELINE_PATH`) is
`monkeypatch`ed per test to point at synthetic files instead, so
nothing in this suite depends on (or could ever accidentally flag)
this repo's own real docs.
Wired into the existing CI jobs rather than new ones:
`pyproject.toml`'s `testpaths` widened to `["dispatcharr-plugin",
"tools"]` (picked up by `unit-tests-python`'s already-bare `pytest`
call), and the `lint` job's `ruff format --check`/`ruff check` calls
widened from `dispatcharr-plugin/` alone to `dispatcharr-plugin/
tools/` -- `tools/check_doc_refs.py` was previously never itself
ruff-formatted or linted in CI at all. 156 total across the Python
suite now (41 `recording_edl` + 86 `timeshift_buffer` + 29
`check_doc_refs`).
**Update (2026-09-13): one more pass over `PVRDispatcharr.cpp` after
the Python side was declared done, prompted directly by "is there
anywhere else the tests make sense?" -- found 3 more real candidates
the original exhaustive read had scanned past.** New
`src/SeriesRuleMatching.{h,cpp}` (`MatchRecordingsToSeriesRules()`)
pulls the matching core out of `GetTimers()`: which recording belongs
to which series rule (by `channelId`+`title`, the same identity
`DeleteSeriesRule()` uses), and each rule's own earliest upcoming/
in-progress match -- a series rule has no fixed time of its own, so
without this its own row displayed as the Unix epoch ("12/31/1969")
in Kodi, a real confirmed-live display bug this item's earlier
updates never got around to testing directly. New header-only
`src/RecurringRuleWeekdays.h` (`ComputeRecurringRuleWeekdaysBitmask()`)
covers the small days-of-week-to-`PVR_WEEKDAY`-bitmask conversion from
the same function, re-confirmed against Kodi's real header
(`PVR_WEEKDAY_NONE = 0`, `PVR_WEEKDAY_MONDAY = (1 << 0)`) that no bit
reordering is needed. New header-only `src/ChannelGroupFilter.h`
(`FilterChannelGroupsWithChannels()`) covers `EnsureChannelsLoaded()`'s
own "drop a channel group with no member channels left" filter --
Dispatcharr's `/api/channels/groups/` returns every group that's ever
existed, including ones no longer enabled for any M3U account, and
"enabled" isn't a property of the group itself to check directly. All
three types involved (`Recording`/`TimerRule`/`Channel`/`ChannelGroup`)
were already confirmed Kodi-independent plain structs from earlier
extractions, so none of this needed new type design. 21 new test
cases, 204 total across the C++ suite now. Confirmed pure code motion
via `git diff` and a rebuild through the real Kodi binary-addons
harness, log body read directly. A further re-read of `addon.cpp` (the
one C++ file never previously read in full) and every header for
stray `inline`/`constexpr` logic found nothing else -- `addon.cpp` is
pure Kodi-instance-lifecycle glue with no real business logic to
extract, and no header defines an inline function outside the small,
already-tested utility headers this item already tracks.
**Update (2026-09-13): one more check, this time a full line-by-line
re-read of `DispatcharrClient.cpp` and `WebSocketClient.cpp` end to
end (the two large files that hadn't had their own dedicated
follow-up pass yet, only `PVRDispatcharr.cpp` had) -- `DispatcharrClient.cpp`
had nothing left (every remaining function is curl/network I/O or
member-state mutation, already routed through the pure helpers pulled
out earlier this item), but `WebSocketClient.cpp` had one real gap:
`ReceiveTextMessage()`'s own frame-header decode, the RFC 6455 decode
counterpart to `BuildMaskedControlFrame()` this item already covered
encode-side, had never itself been pulled out or tested.** Added to
the existing `src/WebSocketFrame.{h,cpp}` module (same file, not a new
one): `ParseFrameHeaderBytes()` (the FIN/opcode/MASK-bit/7-bit-length
bit-twiddling from a frame's first 2 header bytes), `DecodeExtendedPayloadLength16()`/
`DecodeExtendedPayloadLength64()` (the big-endian 126/127
extended-length sentinel decode), and `UnmaskPayload()` (the same
XOR-cycling `BuildMaskedControlFrame()` already applies when masking,
exposed separately since the receive path unmasks an already-received
payload rather than building one). 11 new test cases -- including a
regression case confirming the 3 reserved (RSV1-3) header bits don't
leak into the decoded opcode, and one confirming `UnmaskPayload()` is
its own inverse -- 215 total across the C++ suite now. Confirmed pure
code motion via `git diff` and a rebuild through the real Kodi
binary-addons harness (log body read directly, not just the wrapper's
own exit code -- see this project's own build-harness notes). With
this, both files the original exhaustive read named are now fully
mined; the only further candidates found while re-reading
`PVRDispatcharr.cpp`'s remaining large functions (`GetStreamTimes()`,
`GetEPGTagStreamProperties()`, `UpdateTimer()`) were single-line
arithmetic expressions (a seconds-to-minutes ceiling divide, a
duration-in-minutes computation) tightly wrapped by Kodi-API/network
calls -- deliberately not extracted on their own, per this project's
own against-premature-abstraction convention (see `CLAUDE.md`'s
"Conventions" section).
**Update (2026-09-13): one more independent audit pass (a fresh,
from-scratch read rather than continuing the one above) found two
more real gaps, both closed the same day.** New
`src/WebSocketHandshake.h` (`IsWebSocketHandshakeAccepted()`) pulls
`WebSocketClient::Connect()`'s own handshake-acceptance check out of
that curl-driven function -- the "101" status-line plus
"Upgrade: websocket" response-header predicate that decides whether
Dispatcharr actually accepted the WebSocket upgrade request. This is
the real path a user hits on a WS auth failure, previously untested
despite living right next to `WebSocketFrame`'s already-tested
encode/decode siblings in the same file. 8 new test cases, 223 total
across the C++ suite now. On the Python side,
`timeshift_buffer`'s `_remove_channel_files()` -- real
`shutil.rmtree` cleanup plus its invalid-`channel_uuid` guard, zero
Redis/Django dependency -- had every existing caller `monkeypatch` it
away rather than exercise it directly; 4 new tests now cover it
directly against a real `tmp_path` (normal removal, an already-gone
directory being a no-op, the invalid-uuid refusal never touching
disk, and a genuine `OSError` from `shutil.rmtree` being logged
rather than raised). 160 total across the Python suite now. Both
confirmed via a rebuild through the real Kodi binary-addons harness
(log body read directly, not just the wrapper's own exit code) and a
full `pytest`/`ruff` pass. No further gaps found in this pass.

#### The sweeps now have a harness for the real client and PVR code

**Closed 2026-10-05.** `tests/glue/` (see `tests/glue/README.md`) is the harness the thirteenth and fourteenth sweeps built in scratch and found a crash and four slow-path defects with: the real `DispatcharrClient.cpp` and `PVRDispatcharr.cpp` against Kodi's headers with stub Kodi functions, a fake Dispatcharr, drivers for the live, completed-recording and in-progress-recording stream paths and for the Kodi-facing PVR surface, and a two-worker plugin integration run with a fake Redis and a real ffmpeg. All scenarios were re-run after this change under ASan/UBSan and the main ones under TSan (no report). ThreadSanitizer, which the earlier known gap said could never run, runs on this harness with a no-PIE build and a retry loop.
