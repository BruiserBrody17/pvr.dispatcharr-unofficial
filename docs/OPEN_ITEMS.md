*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**22 open, 367 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two came from the two 2026-10-10
source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself with its
plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)). Nothing is left from the 2026-10-19
live check of four audit fixes on `Piers` (two findings held here too: the manifest throttle, fixed the same day, and
the TSan start-up crash, fixed on 2026-10-22) or from the 2026-10-18 read-only audit, made on `Piers` and checked
against `Omega`'s code the same day (all 21 held here; 14 fixed on 2026-10-19, four on 2026-10-21, one on 2026-10-22 and
the last two closed without a change on 2026-10-22, all in `CLOSED_ITEMS.md`). 12 more come from the 2026-10-23
bug-hunting sweep of the whole project, made on `Piers` and checked against `Omega`'s code the same day (all 17 held
here), all LOW; three more of its 17 (the release gate's unscanned bytes and two in `tools/build_kodi_repo.py`) were
fixed on 2026-10-27, its one MEDIUM (the timeshift start lock) on 2026-10-28, and one (a sustained outage ending
playback silently) was closed without a change after a live check on 2026-10-28.

Three (2026-10-24) come from a second bug-hunting sweep, one reviewer over the whole project with each finding
checked against the code before it was filed, all LOW; three more of its seven (two release-gate findings, a stale
plugin zip, MEDIUM, and the unchecked binaries, and one in `tools/build_kodi_repo.py`) were fixed on 2026-10-27, and its
other MEDIUM (the timeshift plugin's token reaching a Redirect-profile provider) on 2026-10-28. One more (the same day) came out of the
0.33.0 stream-switch re-test (a codec-changing switch, checked live on 2026-10-25 and now a design question), and on
2026-10-25 an upstream note from the 0.33.0 review (a Server-side stall that check turned up was fixed the same day
and is in `CLOSED_ITEMS.md`). One (2026-10-26) came out of a read-only review of the
HEVC seek-probe fix: a live read in a gap that answers 0 while the buffer is alive (its other half, a recycled
segment, was fixed on 2026-10-29). One (2026-10-28) came out of the review of the
start-lock removal fix: a start parked on a gevent worker can still outlive its lock. One (2026-10-29) is
what the 2026-10-28 live check of a sustained outage left: the notice that ends playback blames the file server's port.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 14 |
| Open: Architectural / concurrency | 2 |
| Open: Design decision needed | 2 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 317 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 40 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

None open.

### Fix known, not yet done

#### A live read that lands in a gap answers 0 while the buffer is alive, which sets Kodi's end of file

**Opened 2026-10-26, from a read-only review of the HEVC seek-probe fix; LOW; cannot be reached from outside on Kodi 21
for Linux.** Filed as "Two more live-read paths answer 0 while the buffer is alive"; the other path, a recycled segment,
was fixed on 2026-10-29 (`CLOSED_ITEMS.md`, "A recycled live segment answered 0, so playback ended at the next
stall"). `ReadLiveTimeshiftStreamOnce()` still answers 0 when the read position falls in a gap no segment covers (its
"is a gap" debug line). That happens while the buffer is alive, but a 0 sets Kodi's end of file until the next seek
(`CLOSED_ITEMS.md`'s "Server-side mode: an HEVC channel's opening seek probe leaves Kodi at end of file, so its first
stall ends playback"), so playback can carry on and then end at the next stall instead of waiting out
`kLiveStallWaitBudgetMs`. The way in is a manifest refresh that prunes the segment being read between the read's clamp
to the oldest segment and its segment lookup.

**Checked live 2026-10-28** on Kodi 21.3 on the Linux VM (the setup is in the recycled-segment entry in `CLOSED_ITEMS.md`): holding the heartbeat
for 1.8 s (the only network call in that window) while answering manifests with the segment being read removed changed
nothing. Kodi 21 on Linux calls `GetStreamTimes()`, the only other caller of the refresh, on the reader's own thread,
so no refresh can land in that window. Reaching it here needs a throwaway build with a second thread.

Fix: a bound first, since -1 on a gap that never fills would make Kodi retry every 100 ms forever.
`HandleLiveTimeshiftSegmentFetchFailure()` is one now that the recycled segment goes through it: its forced refresh
lets the retry's clamp move past a pruned segment, and a gap that persists ends through its give-up rule with a notice.

#### A whole-server outage ends live playback with a notice that blames the file server's port

**Opened 2026-10-29, from the 2026-10-28 live check of a sustained outage (`CLOSED_ITEMS.md`, "A sustained server outage
may still end live playback with no notice"); LOW, seen live.** When `HandleLiveTimeshiftSegmentFetchFailure()` gives
up, it probes the plugin's file server with `ProbeTimeshiftFileServer()` and, if that fails, queues
`BuildTimeshiftFileServerUnreachableMessage()`: "Can't reach Dispatcharr's timeshift file server on port N. Publish that
port and make it match the plugin's http_port setting." The probe cannot tell a port that is not published from a server
that is down, so in an outage of the whole server (two of the three runs on Kodi 21.3, connections reset on both
Dispatcharr ports) that is the notice playback ends with, and it sends the viewer to their configuration. Kodi's own
connection-lost toast, about 12 s into the outage, says what really happened, but the addon's notice comes last.

Fix: mid-session, the file server answered when the stream opened, so a failed probe there is more likely an outage
than a port left unpublished. Before choosing the message, also try Dispatcharr's own API port (or use the last
connection state the addon reported); if that fails too, say the server could not be reached instead. Keep the port
wording for `OpenLiveTimeshiftStream()`, where the API has just answered. The choice of message is pure logic and gets a
test beside `BuildTimeshiftFileServerUnreachableMessage()`'s in `tests/test_plugin_url_util.cpp`.

#### The live heartbeat carries no access token, so a resumed viewer can stick to a replacement buffer

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `SendTimeshiftHeartbeat()`
sends only the channel and viewer id, and the plugin's `_heartbeat()`/`_apply_heartbeat()` add that viewer to whatever
buffer is current. Since 0.8.15 a stop with an old token is declined. So: viewer A pauses past the idle timeout and is
reaped, B starts the channel, A resumes and its overdue heartbeat registers it on B's buffer, A then goes fatal on a 403
and its stop is declined, and when B stops the buffer and its provider slot stay up until the reaper prunes A. Fix: send
`access_token` with the heartbeat and have `_heartbeat()` skip and answer "replaced" when `_is_stale_access_token()`
matches, as `get_live_manifest` does. Also reword `CLOSED_ITEMS.md`'s 0.8.15 entry, which says heartbeats carried the
token in the probe (the heartbeat action's body does not).

#### OpenRecordedStream() writes the API-key owner stamps before the key they vouch for

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading, narrow window.** On a successful
`ObtainApiKey()` it calls `WriteApiKeyOwnerStampsIfChanged()` at once, but the adopted or regenerated key reaches
`api_key` only at the end of the open, via `PersistApiKeyIfChanged(keyBefore)`; the constructor does it in the right
order. If Kodi exits or crashes during the open (or the settings dialog is open and cancelled, so the `api_key` write is
dropped), the next start finds stamps for the new account beside the old account's key, sets
`m_apiKeyOwnershipVerified`, and on the same server that key authorises as the wrong user with no 401. Fix:
`PersistApiKeyIfChanged(keyBefore)` first, then the stamps, then refresh `keyBefore`. A glue variant of the
`owner_stamps` scenario can check the stamps are never on disk without their key.

#### Windows (and 32-bit) parse a pre-1970 (pre-1901) date to a floor that later subtractions overflow

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced with an injected `gm` under UBSan.**
`_mkgmtime` returns -1 outside 1970-3000, so since the 2026-10-18 floor fix any date before 1969 on Windows saturates to
`kEarliestRepresentableTime` (`INT64_MIN` plus a year) instead of -1; on 32-bit the same happens before 1901-12-13. The
floor leaves room for an offset, not for `now - t` or `end - start`: with a start of 1960 `ClampedDurationSeconds()`
overflows, `IsWithinCatchupWindow()` answers true and `CatchupSessionDurationMinutes()` a large negative number (also
`EpgProgramMatch.cpp` and `ComputeRecordingExtendMinutes`). Only a garbage or hostile server timestamp reaches it.
Related: a date after 3000 on Windows saturates to a value `gmtime_s` cannot format. Fix: a portable int64
days-from-civil in place of `_mkgmtime`/`timegm`, saturated to `time_t`, or have the parsers answer 0 (unknown) at the
floor. Tests: a Windows-like `gm` in `test_time_util.cpp` and floor-valued starts in the recording, EPG-tag and
catch-up-session tests (they fail under UBSan today).

#### ParseProgramLookupResponse() and the timer body copy server subtrees of any depth

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, reproduced on a 1 MiB stack.**
`TimerRequestBuilder.cpp` copies `id`/`title`/`sub_title`/`description`/`tvg_id` from the EPG search response (`out[key]
= *it`) and into the timer body (`program[key] = *it`) whatever their type, and the body is later `dump()`ed: the
recursive copy the 2026-10-19 `FieldRefOrNull` fix removed elsewhere. A `description` nested 12,000 deep segfaults on a
1 MiB thread. Dispatcharr sends strings, so only a hostile server reaches it. Fix: copy a key only when it is a string
(an integer for `id`). Test: a 100,000-deep value via `RunOnSmallStack` in `test_timer_request_builder.cpp`.

#### A search-mode series rule costs terms times text length per recording

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, measured.** `SeriesRuleTextMatch.cpp`: `Leaf()` rescans
the folded text for every operand, repeated terms included, `ContainsWholeWord()` visits every occurrence that fails the
word-boundary check, and `Combine()` evaluates every leaf. The 4 KiB query cap bounds the query, not the work: 680
copies of one short term ORed together take 105 ms against 10 KB of text and 10.8 s against 1 MB, per recording, on
every `GetTimers()`. Needs a runaway or hostile rule. Fix: memoize leaves per call, precompute a word-boundary table
once, and optionally answer `kUnsupported` past a terms-times-text budget. Test: a timing-bounded case and unchanged
results in `test_series_rule_text_match.cpp`.

#### Two text-handling gaps: SanitizeRecordingDirectory's std::isspace, and C1 controls in SanitizeServerErrorBody

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading (the locale case not reproduced
here).** (1) `RecordingDirectory.h` trims with `std::isspace`, which depends on the C locale Kodi sets from the user's
region; under a Windows code page that counts 0xA0 as a space, a title ending in "à" (UTF-8 `C3 A0`) loses its last byte
and the folder name is invalid UTF-8. It also trims only ASCII whitespace, where the function says it mirrors
Dispatcharr's `_safe_name()` (check whether that uses Python's Unicode `.strip()`). Fix: `StripUnicodeWhitespace()` from
`UnicodeText.h`; tests for "Voilà" and a U+3000 edge. (2) `SanitizeServerErrorBody()` (`JsonResponse.cpp`) blanks C0 and
DEL but passes C1 controls, raw 0x80-0x9F and UTF-8 U+0080-U+009F (`C2 9B` is CSI), into `kodi.log`. Fix: blank those
too, with a test in `test_json_response.cpp`.

#### timeshift_buffer: segment_seconds has no upper limit, and the 1440-minute cap makes manifests heavy

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, (1) confirmed by reading, (2) measured.** (1)
`segment_seconds` is read with `minimum=1` only, missed by the 2026-10-18 caps: a typo such as 3600 means no segment
ever closes, every open fails the addon's cold start with "live playlist not found", and one `.ts` file grows until the
buffer stops. Cap it (60, and no more than `buffer_minutes*60`) and say so in the help text. (2) At the new
`_MAX_BUFFER_MINUTES` with 2 s segments a cold manifest build takes 3.2 s on local disk (137 ms at the default), returns
5.9 MB of JSON, and holds the single per-worker `_manifest_cache_lock` throughout, serialising every channel's
`get_live_manifest` in that worker; the 16-entry cache was sized for the default. Fix: a lower cap, a byte-based cache
limit, or the stat sweep outside the lock.

#### timeshift_buffer: a Redis outage logs a traceback per file-server request and drops the connection

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `_check_access_token()` is
called from the request handler with no try, and redis-py's `ConnectionError`/`TimeoutError` are not the built-in ones,
so each playlist or segment request during an outage (or with a corrupt state, through `ValueError`) reaches
`handle_error`'s `logger.exception` and the client gets a dropped connection instead of a status. The reaper rate-limits
the same errors. Fix: catch in `_resolve_and_authorize`, answer 503, and rate-limit the log as
`_should_log_reaper_error` does; test with a raising `_get_buffer_state` through `_fake_handler`.

#### RefreshAccessToken() drops a rotated refresh token

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible: depends on Dispatcharr's SimpleJWT
settings.** Only `access` is kept from the refresh response, unlike `Login()`. With `ROTATE_REFRESH_TOKENS` and
blacklisting on, every second refresh would send a blacklisted token, get a 401 (`kRejected`) and fall back to a full
password login, feeding the login rate limit `AuthBackoff` exists to avoid. Without rotation it costs nothing. Fix: keep
`refresh` when present, under the state lock; check Dispatcharr's `SIMPLE_JWT` settings while there.

#### CONNECTED is held back with no limit while the DVR access level is unknown

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible.** `TakePending(report, m_dvrAccessChecked)`
delivers `CONNECTED` only once `SyncDvrAccessFromDispatcharr()` has succeeded, which needs a numeric `user_level` from
`/api/accounts/users/me/`. A server or proxy that omits it, after a start that reported `UNREACHABLE`, leaves Kodi
showing the client unreachable and never re-reading `GetCapabilities()`; playback is unaffected. Every Dispatcharr seen
so far sends the field. Fix: treat a parse failure (as opposed to a network failure) as checked and fail open, or
deliver `CONNECTED` after a few failed reads; testable in `ConnectionReport`.

#### timeshift_buffer: string settings are not stripped, so a pasted space breaks storage_path or internal_base_url

**Opened 2026-10-24, from the 2026-10-24 bug-hunting sweep (one reviewer, each finding checked against the code before
it was filed); LOW, reproduced with pytest against the real file.** `_str_setting()` rejects a blank value but returns
the value unstripped, and Dispatcharr stores plugin settings as typed. A `storage_path` of " /data/timeshift" becomes a
relative path, so buffers land under the worker's working directory inside the container and nothing flags it; an
`internal_base_url` with a trailing space puts the space into ffmpeg's input URL, so every start fails with the message
about a provider-side stream limit, pointing away from the cause. The plugin already strips `test_channel_uuid` for this
reason. Fix: return `value.strip()`; tests for both settings.

#### A connection state Kodi ignores while its PVR manager is stopped is marked delivered and never re-sent

**Opened 2026-10-24, from the 2026-10-24 bug-hunting sweep (one reviewer, each finding checked against the code before
it was filed); LOW, confirmed by reading the addon and Kodi 22's source, not reproduced.**
`ConnectionReportLatch::TakePending()` records the state as delivered before `ReportConnectionToKodi()` calls
`ConnectionStateChange()`, which returns nothing. Kodi drops the callback while the client's calls are blocked
(`CPVRClient::HandleAddonCallback()`, "Ignoring callback from PVR client", when `m_bBlockAddonCalls` is set by
`CPVRClient::Stop()` during a PVR manager restart: a PVR addon enabled, disabled or updated, a database reset). If the
addon's report thread delivers in that window (for example the held-back CONNECTED after a start with the server down),
Kodi keeps the old state and skips the `GetCapabilities()` re-read, and the latch never tries again until the state
really changes. Fix: allow re-delivering the current observation now and then (Kodi ignores a state equal to the one it
has), or keep "delivered" apart from "observed".

### Architectural / concurrency

#### Nothing stops an exception crossing from a PVR callback into Kodi

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, hardening, confirmed by reading.** The worker threads
and detached tasks run under `RunGuardedWorker()`/`RunBackgroundTask()`, but the `CInstancePVRClient` overrides Kodi
calls on its own threads (`GetTimers`, `GetChannels`, `UpdateTimer` and the rest) and the constructor reached from
`CreateInstance` in `addon.cpp` are not, and neither the dev-kit thunks nor `CPVRClient::DoAddonCall()` catch anything.
A `std::bad_alloc` copying the recordings or rules vectors on a 32-bit or low-memory box ends the Kodi process (and
crosses a CRT boundary on Windows). Fix: a small guard that catches `std::exception` and `...`, logs, and returns
`PVR_ERROR_FAILED`/`false`/`-1`, around each override, and around `new PVRDispatcharr` returning `ADDON_STATUS_UNKNOWN`.

#### timeshift_buffer: a start parked by another greenlet's blocking work can outlive its start lock

**Opened 2026-10-28, from the review of the start-lock removal fix (`CLOSED_ITEMS.md`, "removals under the 30 s start
lock let a second start run beside the first"); LOW, plausible from reading, older than that fix.** Dispatcharr's
workers are gevent-patched (`CLOSED_ITEMS.md`'s gevent note), so a `start_buffer` that yields while holding the channel
and slot locks (the `_list_buffer_keys()` SCAN, a Redis call) cannot resume while another greenlet on the same worker
runs a blocking `shutil.rmtree`: a `stop_buffer` or reaper teardown of a large buffer, or another start's deferred
removal. Past `_START_BUFFER_LOCK_TTL` a start on another worker can take the lock, spawn and write its state; the parked
start then resumes, `_reap_untracked_ffmpeg()` stops the other's ffmpeg as untracked, and its own state write replaces
the other's, whose viewer gets 403s. Fix candidates: re-check `GET lock == token` before `_start_ffmpeg()` and before
`_set_buffer_state()` and answer retryable (a loss after the spawn then has to stop its own ffmpeg), or move the
blocking removals off the request greenlet (`gevent.threadpool`, or `rmtree` in a subprocess). Reproducing it needs a
slow removal and a start on the same worker.

### Design decision needed

#### timeshift_buffer: a stop from the last listed viewer ends a buffer another viewer is still reading

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible.** 0.8.15 stopped the reaper ending a buffer
whose viewers went stale while its segments are still read; `stop_buffer` has the same gap. If viewer A's API calls fail
for 30 s or more while its segment reads work, the reaper drops A from the list, and when B stops, the empty list tears
the buffer down under A (`abort_if=viewers` only). Choices: let the empty-list case defer to the reaper's buffer-wide
idle check (costing up to the idle timeout plus a tick of provider slot), or have segment fetches carry the viewer id so
they refresh per-viewer stamps.

#### Server-side mode freezes the picture after a stream switch that changes codec (and Kodi 21 does in every mode)

**Opened 2026-10-24 from the Dispatcharr 0.33.0 stream-switch re-test; checked live 2026-10-25 and turned into a design
question.** Three disposable channels on the lab, each with a synthetic first stream that stops sending 20 s after it
connects and a synthetic second stream (no provider involved): 720p H.264 then 1080p HEVC, the reverse, and H.264 both
times as the control. Dispatcharr switched at about 45 s every time. The control played on in every mode. With a codec
change, watched for 120 s with screenshots compared at 50, 80 and 110 s: on Kodi 22.0-RC1, Off and Local mode switched
decoder (H.264 1280 ↔ HEVC 1920) and played on in both directions; on Kodi 21.3, Off and Local mode froze the picture on
the old stream's last frame while sound and the player time went on (Kodi kept feeding the new stream to the old
decoder, logging `[h264] no frame!` over and over), which is Kodi's demuxer not acting on the PMT's new stream type for
the same PID; and in Server-side mode both Kodi versions froze in both directions, the decoder never changed and the
player time reset to 0 every few seconds from then on (the symptom first seen on 2026-10-24 with a real channel). So
Server-side is the case on the addon's own path: the buffer's ffmpeg (`-c copy -f hls`) keeps writing segments for the
codec it opened with. Options: watch the buffer's ffmpeg for a codec or stream-layout change on its input and restart it
(a fresh segmenter; the addon then has to treat the new segments like a buffer restart), or re-mux each segment's codec
parameters some other way; or accept it and document it (on Kodi 21 nothing on the addon side helps in Off or Local
mode). Real channels change codec on a switch only when their streams come from different sources, so how often this
bites is unknown.

### Release, CI and manual testing

None open.

### Upstream (Dispatcharr) or documentation accuracy

#### timeshift_buffer: the Streams network allowlist applies to the viewer's address, not localhost as the plugin says

**Opened 2026-10-24, from the 2026-10-24 bug-hunting sweep (one reviewer, each finding checked against the code before
it was filed); LOW, confirmed by reading the plugin and Dispatcharr's source, not run.** The module docstring says a
loopback request needs only localhost in Settings → Network Access → Streams. But the buffer's request carries
`X-Real-IP: <viewer's address>`, and Dispatcharr trusts that header from 127.0.0.1 (`get_client_ip()`), so
`network_access_allowed(request, "STREAMS")` is judged against the Kodi device. A device outside a narrowed Streams list
gets a 403, ffmpeg exits, and the addon reports a provider-side stream limit. (Direct playback from that device would be
refused the same way, so the outcome is right; the explanation and the message are not.) Fix: correct the docstring and
the plugin README, and show the proxy's HTTP status from ffmpeg's log in the start failure.

#### Dispatcharr's own DVR, HDHR and M3U output follow a non-MPEG-TS Default Output Format

**Opened 2026-10-25, from the review of Dispatcharr 0.33.0; read in the `v0.33.0` source, not run.** For a request that
names no `output_format` and has no per-user override, `_resolve_output_format()` (`apps/proxy/live_proxy/views.py`)
falls back to Settings → Stream Settings → Default Output Format. Dispatcharr's own DVR builds its capture URL without
one (`_dvr_capture_url()`, `apps/channels/tasks.py`: `/proxy/ts/stream/<uuid>`, plus `?output_profile=` when set), the
HDHR lineup does the same (`apps/hdhr/api_views.py`), and the M3U output adds `output_format` to its stream URLs only
when the playlist request itself carried one (`apps/output/views.py`). So a server set to HLS hands its own recorder a
redirect to an HLS playlist and set to fMP4 hands it MP4 fragments; what ffmpeg then records, and what an HDHR client
makes of either, was not checked. Not this addon's bug: the addon and `timeshift_buffer` ask for `mpegts` since
`CLOSED_ITEMS.md`'s "Pin the live proxy's output format to MPEG-TS". Worth reporting to Dispatcharr once seen: on a
server nobody else uses (the lab's setting is shared with its other clients), set Default Output Format to HLS, record a
short programme, and check the file.

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

None open.
