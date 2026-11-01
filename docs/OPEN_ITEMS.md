*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**20 open, 382 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- Eight, all LOW, from the 2026-10-23 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code the same day
  (all 17 findings held here). Nine of its 17 are fixed or closed; two of the eight are what is left of findings half
  fixed on 2026-10-31.
- One, LOW, from the 2026-10-24 sweep (one reviewer over the whole project). Six of its seven are fixed.
- One from the 2026-10-25 stream-switch re-test on Dispatcharr 0.33.0: a codec-changing switch, now a design question.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0.
- One from the 2026-10-26 review of the HEVC seek-probe fix: a live read in a gap that answers 0 while the buffer is
  alive.
- One from the 2026-10-28 review of the start-lock removal fix: a start parked on a gevent worker can still outlive its
  lock.
- Five, all LOW, from the 2026-10-31 bug-hunting sweep (five read-only reviews by area), made on `Piers` and
  checked against `Omega`'s code the same day (all 11 findings held here). Six of its eleven are fixed: the release
  gate's four and two in `timeshift_buffer`'s file server.

Nothing is left from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code (all 21 findings
held here), or from the 2026-10-19 live check of four audit fixes on `Piers`.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 13 |
| Open: Architectural / concurrency | 2 |
| Open: Design decision needed | 2 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 1 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 332 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 40 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

None open.

### Fix known, not yet done

#### A live read that lands in a gap answers 0 while the buffer is alive, which sets Kodi's end of file

**Opened 2026-10-26, from a read-only review of the HEVC seek-probe fix; LOW; reproduced live with a throwaway build,
not reached from outside on Kodi 21.3 or Kodi 22.0 RC1 for Linux.** Filed as "Two more live-read paths answer 0 while the buffer is alive"; the other path, a recycled segment,
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

**Checked live 2026-10-31** with such a build: the `Omega` tip (addon 0.13.2) plus a second thread calling
`GetLiveTimeshiftStreamDurationMs()`, what `GetStreamTimes()` calls, every 250 ms, standing in for a Kodi that polls
stream times off the reader's thread. Same VM, channel and fault proxy, on Kodi 21.3. Each run paused for 30 s and
resumed, held one heartbeat for 1.8 s while manifests dropped the segment being read, then froze the manifest so the
reader met a silent stall once it caught up.
- **Control (no pruning):** the tail wait reached 64.5 s of its 70 s and playback resumed once the manifest moved.
- **Pruning:** the second thread's refresh landed inside the held heartbeat, the read logged "is a gap" and answered 0,
  and the next read continued from the oldest segment. Playback carried on normally for about 56 s; once the reader
  reached the frozen tail, Kodi logged "VideoPlayer: next stream" after 3.2 s of tail waiting and ended playback, with
  no notice, the same as the recycled segment before its fix. (A first run that started further behind live never
  reached the tail; playback ran on for the two minutes the freeze lasted.)
- **Kodi 22.0 RC1, the released 0.14.2 Linux build (no second thread), twice:** no manifest request reached the proxy
  during the held heartbeat, no gap was logged, and the stall was waited out to about 69 s of the 70 s budget. Kodi 22
  on Linux polls stream times on the reader's thread too.

So the 0 ends playback as described once a refresh can run on another thread; neither Linux Kodi runs one there, and
other platforms were not checked.

Fix: a bound first, since -1 on a gap that never fills would make Kodi retry every 100 ms forever.
`HandleLiveTimeshiftSegmentFetchFailure()` is one now that the recycled segment goes through it: its forced refresh
lets the retry's clamp move past a pruned segment, and a gap that persists ends through its give-up rule with a notice.

#### The live heartbeat carries no access token, so a resumed viewer can stick to a replacement buffer

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading.** `SendTimeshiftHeartbeat()`
sends only the channel and viewer id, and the plugin's `_heartbeat()`/`_apply_heartbeat()` add that viewer to whatever
buffer is current. Since 0.8.15 a stop with an old token is declined. So: viewer A pauses past the idle timeout and is
reaped, B starts the channel, A resumes and its overdue heartbeat registers it on B's buffer, A then goes fatal on a 403
and its stop is declined, and when B stops the buffer and its provider slot stay up until the reaper prunes A. Fix: send
`access_token` with the heartbeat and have `_heartbeat()` skip and answer "replaced" when `_is_stale_access_token()`
matches, as `get_live_manifest` does. Also reword `CLOSED_ITEMS.md`'s 0.8.15 entry, which says heartbeats carried the
token in the probe (the heartbeat action's body does not).

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

#### A search-mode series rule costs terms times text length per recording

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, measured.** `SeriesRuleTextMatch.cpp`: `Leaf()` rescans
the folded text for every operand, repeated terms included, `ContainsWholeWord()` visits every occurrence that fails the
word-boundary check, and `Combine()` evaluates every leaf. The 4 KiB query cap bounds the query, not the work: 680
copies of one short term ORed together take 105 ms against 10 KB of text and 10.8 s against 1 MB, per recording, on
every `GetTimers()`. Needs a runaway or hostile rule. Fix: memoize leaves per call, precompute a word-boundary table
once, and optionally answer `kUnsupported` past a terms-times-text budget. Test: a timing-bounded case and unchanged
results in `test_series_rule_text_match.cpp`.

#### SanitizeRecordingDirectory trims with std::isspace

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, confirmed by reading (the locale case not reproduced
here).** Filed as "Two text-handling gaps: SanitizeRecordingDirectory's std::isspace, and C1 controls in
SanitizeServerErrorBody"; the C1 half was fixed on 2026-10-31 (`CLOSED_ITEMS.md`, "SanitizeServerErrorBody passes C1
control characters into kodi.log"). `RecordingDirectory.h` trims with `std::isspace`, which depends on the C locale Kodi
sets from the user's region; under a Windows code page that counts 0xA0 as a space, a title ending in "à" (UTF-8 `C3
A0`) loses its last byte and the folder name is invalid UTF-8. It also trims only ASCII whitespace, where the function
says it mirrors Dispatcharr's `_safe_name()` (check whether that uses Python's Unicode `.strip()`). Fix:
`StripUnicodeWhitespace()` from `UnicodeText.h`; tests for "Voilà" and a U+3000 edge.

#### timeshift_buffer: the 1440-minute cap makes manifests heavy

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, measured.** Filed as "timeshift_buffer:
segment_seconds has no upper limit, and the 1440-minute cap makes manifests heavy"; the `segment_seconds` half was
fixed on 2026-10-31 (`CLOSED_ITEMS.md`, "timeshift_buffer: segment_seconds has no upper limit"). At the new
`_MAX_BUFFER_MINUTES` with 2 s segments a cold manifest build takes 3.2 s on local disk (137 ms at the default), returns
5.9 MB of JSON, and holds the single per-worker `_manifest_cache_lock` throughout, serialising every channel's
`get_live_manifest` in that worker; the 16-entry cache was sized for the default. Fix: a lower cap, a byte-based cache
limit, or the stat sweep outside the lock.

#### CONNECTED is held back with no limit while the DVR access level is unknown

**Opened 2026-10-23, from the 2026-10-23 bug-hunting sweep; LOW, plausible.** `TakePending(report, m_dvrAccessChecked)`
delivers `CONNECTED` only once `SyncDvrAccessFromDispatcharr()` has succeeded, which needs a numeric `user_level` from
`/api/accounts/users/me/`. A server or proxy that omits it, after a start that reported `UNREACHABLE`, leaves Kodi
showing the client unreachable and never re-reading `GetCapabilities()`; playback is unaffected. Every Dispatcharr seen
so far sends the field. Fix: treat a parse failure (as opposed to a network failure) as checked and fail open, or
deliver `CONNECTED` after a few failed reads; testable in `ConnectionReport`.

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

#### A Stop while a recording rides out a transient failure holds Kodi's interface for up to 20 s

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep (five read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading, not reproduced.** `ReadRecordingStream()` and
`ReadInProgressRecordingStream()` retry a transient failure (a refused connection, 500/502/503/504) inside one read
for up to `kTransientReadBudget` (20 s, plus up to one request timeout against a server that never answers), and only
`m_abortRequests` at teardown cuts the loop short. Kodi acts on a Stop only between reads, so a Stop during an outage
holds its interface that long: the mechanism `CLOSED_ITEMS.md`'s "A Stop during a run of failed live segment fetches
holds Kodi's interface for up to 25 s" measured and fixed on the live path, not carried over here. Two comments
disagree on whether that fix applies: `IsTransientReadFailure()`'s (`SegmentFetchFailure.h`) says Kodi does not keep
retrying a failed recorded read, while the in-progress tail wait already answers -1 so a stop can land, confirmed live
(`CLOSED_ITEMS.md`, "Slice the in-progress recording's tail wait the way the live one is"). Fix: if -1 is retried on a
recorded stream, as the tail wait suggests, slice both retry loops the way the live path does, with the budget counted
across calls; if not, say so beside `kTransientReadBudget` and consider a shorter budget for a finished recording's
small ranged reads. Correct whichever comment is wrong. Needs a live Stop-latency check before and after.

#### GetChannelGroups() falls back to a route that does not exist

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep; LOW, confirmed by reading.** When `/api/channels/groups/`
fails, `GetChannelGroups()` retries `kChannelGroupsPathFallback` (`/api/channels/channel-groups/`), which the constant's
own comment and `API_NOTES.md` say Dispatcharr does not have: its web app answers it with 200 and `index.html`. So in an
outage the refresh waits out two request timeouts instead of one, a real 403 or 500 from the primary is reported with
"Failed to parse JSON response" appended, and the access token goes to a route that is not an API. The comment inside
the function ("Neither path was independently confirmed") is stale. Fix: drop the fallback (or fall back only on a 404
from the primary).

#### A 401 whose re-authentication fails is reported to the caller as a 401

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep; LOW, latent, confirmed by reading.** `Request()`'s 401 retry
calls `EnsureAuthenticated()` without a status pointer, so when the re-login itself fails (a backoff, a 5xx, a transport
error) `httpStatusOut` still holds the first attempt's 401. Every current caller decides correctly anyway (traced:
`ClassifyApiKeyLookupFailure()` sends a 401 to generating a key, but `GenerateApiKey()` re-runs `EnsureAuthenticated()`
and fails the same way; the recording lookup's playlist skip acts only on status 0; the delete and stop paths treat only
404 specially), but a new caller that takes 401 as a lasting refusal would misread an outage. `CLOSED_ITEMS.md`'s "A 401
during recurring-rule adoption was recorded as a permanent refusal" fixed one caller, not this. Fix: report the
re-authentication's own status, or a "no request made" value, instead of the stale 401.

#### TrackDetachedThread() joins threads inside a std::remove_if predicate

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep; LOW, standards conformance, confirmed by reading.** The
opportunistic prune hands `std::remove_if` a predicate that calls `entry.thread.join()`, and [algorithms.requirements]
forbids a predicate applying a non-constant function through the iterator. It works on libstdc++, libc++ and MSVC
because each calls the predicate once per element, in order, before moving that element, which is also what makes the
later move-assignment onto the slot safe (move-assigning onto a joinable `std::thread` is `std::terminate`). An
implementation that did otherwise would throw `std::system_error` under the lock, on a path reachable from
`OnAddonSettingChanged()` ("Nothing stops an exception crossing from a PVR callback into Kodi", below). Fix, with no
change in behaviour: join the finished entries in a plain loop, then `remove_if` on `!thread.joinable()`.

#### A recurring rule starting inside a spring-forward gap on its first day is shown ending before it starts

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep; LOW, reproduced with a Catch2 case built against the real
`RecurringRuleUtil.cpp` and `TimeZoneUtil.cpp`.** `ComputeRecurringRuleDisplayTimes()` decides the overnight rollover
on the seconds of day (`endTimeOfDaySeconds <= startTimeOfDaySeconds`), then resolves the start under the pre-gap offset
and the end on its own (since `CLOSED_ITEMS.md`'s "The recurring-rule display took the end's offset from the start
instant", which covers a rule that starts before the gap). For a short rule that starts inside the gap on its
`start_date`, nothing re-checks the order of the two instants: in a zone whose clocks go from 02:00 to 03:00, a rule
from 02:30 to 03:15 is shown on the change day ending 15 minutes before it starts. Only the first occurrence's displayed
window, and only when `start_date` is the change day. Fix: once both instants are resolved, roll the end over on the
instants too (`if (endTimeOut <= startTimeOut) endTimeOut += 86400`), and add the case to `test_recurring_rule_util.cpp`.
Which window Dispatcharr itself records depends on whether its scheduler compares the two datetimes in UTC (pytz) or by
wall clock (zoneinfo); check that live before deciding what the display should match.

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
