# Test coverage: what each tested module is for

Moved here from `CLAUDE.md` (2026-10-08), where this account had grown to about 200 KB and was loaded in full at the
start of every session. `CLAUDE.md` keeps the rules (what is tested, where, and the boundary); this file keeps the
history: why each module was extracted, which audit pass or live incident each test pins, and how it was confirmed.
Read the section for a module before changing it, and add new entries here, not to `CLAUDE.md`.

Older entries in `docs/OPEN_ITEMS.md`, `CHANGELOG.md` and the source comments that cite "`CLAUDE.md`'s own `<Module>` entry" mean
the module's section in this file.

Text below is as it was in `CLAUDE.md`; "this list", "the first group" and similar wording refers to the module list at
the start of the C++ part, and a phrase such as "the branch this addon lives on, not `master`" describes the project as
it was when written.

Small automated test suites exist on both the C++ and Python sides as of
2026-09-13 (C++ side last extended 2026-10-06), but both are
deliberately narrow -- only Kodi/Dispatcharr-independent pure/filesystem
logic is covered anywhere.

**C++** (`tests/`, Catch2, wired into CI's `unit-tests` job, a standalone
CMake project separate from the addon's own `CMakeLists.txt` since that
one can only be configured through Kodi's own build harness -- see
`tests/CMakeLists.txt`'s own comment). Every module covered as of
2026-09-13: `XmlTvParser`, `TimeUtil`, `TimeZoneUtil`, `EpgTagUtil`,
`StringUtil`, `DateTimeFormat`, `UrlEncode`, `JsonFieldUtil`,
`CurlCallbacks`, `CatchUpUtil`, `RecurringRuleUtil`, `RecordingParser`,
`PluginRunResult`, `RealtimeUpdateParser`, `M3u8SegmentParser`,
`SegmentLookup`, `ChannelParser`, `TimerRuleParser`, `TimerIdentity`,
`LiveManifestParser`, `LiveEdgeMargin`, `RecurringRuleRenewal`,
`WebSocketFrame`, `SeriesRuleMatching`, `RecurringRuleWeekdays`,
`ChannelGroupFilter`, `AuthBackoff`, `WebSocketHandshake`, `StreamSeek`,
`SegmentAppendOffsets`, `PendingTitleLookup`, `RecordingVisibility`,
`Staleness`, `StreamPropertyUtil`, `PluginUrlUtil`, `RecordingHttpUtil`,
`RecordingDirectory`, `ChannelRenumbering`, `InProgressSegmentCache`,
`EpgProgramMatch`, `TimerRequestBuilder`, `SegmentFetchFailure`,
`CatchupSessionCache`, `ChannelLineupChange`, `ApiKeyRecovery`,
`HlsViewerKeepAlive`, `ChannelNumber`, `SeriesRuleTextMatch`, `UnicodeText`,
`CatchupSessionRequest`, `PaddingPush` (the last two added 2026-10-03, see the
extraction note before the paragraph on what is deliberately not tested)
(the last three of the first group added 2026-09-26, alongside new
coverage added the same day to several already-listed modules -- see
each module's own comment; this second group of five already had their
own test files as of that date too, just missing from this list until
now; `RecordingDirectory` added the same day in a later, 21st, pass;
`ChannelRenumbering` added the same day in a still-later, 23rd, pass;
`InProgressSegmentCache` added the same day in a still-later, 28th,
pass; `EpgProgramMatch`/`TimerRequestBuilder` added the same day in a
still-later, 32nd, pass; `SegmentFetchFailure` added 2026-09-28, fixing a
real, live-confirmed bug -- see its own comment; `CatchupSessionCache`
added 2026-09-29, fixing a real, live-quantified-and-confirmed bug --
see its own comment; `HlsViewerKeepAlive` added 2026-09-30, fixing a
real, live-reproduced-and-re-verified bug -- see its own comment;
`ChannelNumber` added the same day, likewise live-confirmed and
re-verified; `SeriesRuleTextMatch`/`UnicodeText` added the same day too,
for the two series-rule title-matching gaps confirmed live).
`StringUtil` through `CatchUpUtil` in that list were
pulled out of `WebSocketClient.cpp`/`DispatcharrClient.cpp`
specifically so this small,
widely-used logic (Base64/lowercasing, the IPv6-literal-bracketing
`FormatHostForUrl()` every "host:port" URL/Host header this addon
builds shares (`DispatcharrClient::BaseUrl()`, `BuildTimeshiftPlaylistUrl()`/
`BuildTimeshiftSegmentBaseUrl()` in `PluginUrlUtil.h`, and
`BuildWebSocketHandshakeRequest()`'s own `Host:` header in
`WebSocketHandshake.h`), Dispatcharr's own date-time
string formats, curl-based URL escaping, the null-safe JSON field
reader nearly every response parse in `DispatcharrClient.cpp` goes
through, the plain libcurl write/header callbacks -- none of which
touch a `CURL*` themselves, despite conceptually being curl callbacks --
and the segment-duration-estimate/catch-up-attempt-count math shared by
live-timeshift and in-progress-recording playback, each with real
documented incidents behind them) is unit-testable standalone.
`XmlTvParser`'s own `ParseEpisodeNum()` gained a guard against a real
signed-integer-overflow UB gap (fixed 2026-09-27, a 42nd-pass audit,
found via a project-wide UB review, not reproduced live): `parsed + 1`
at `INT_MAX` is undefined behavior, and a provider's own XMLTV feed is
untrusted input. **Correction (2026-09-27, a 51st-pass audit): this
entry's own original reasoning for how `parsed == INT_MAX` is reachable
was factually wrong, found via a project-wide review, confirmed by
direct compilation (g++ -std=c++17), not itself independently
reproduced live beyond that.** `std::stoi` does NOT clamp an overlong
digit string to `INT_MAX` -- it throws `std::out_of_range` for any value
outside `int`'s range (confirmed: `"99999999999999999999"` and
`"2147483648"` both throw; `"2147483647"` itself parses to `INT_MAX`
cleanly), so an overlong string is already caught by the existing
`catch (const std::exception&)`, never reaching this guard at all. The
only way `parsed == INT_MAX` is reachable is the field being the exact
literal string `"2147483647"` -- still untrusted provider EPG input, and
still real, undefined-behavior-risking overflow for that one value, so
the fix and its conclusion (treat it as unknown, same as any other
unparseable value) are unchanged -- only the "how is this reachable"
explanation was wrong.
`XmlTvParser` also gained `ParseOnscreenEpisodeNum()` (added 2026-09-29,
fixing a real, confirmed gap found via a project-wide review, confirmed
against Dispatcharr's own real current upstream source, not itself
independently reproduced live -- see `docs/OPEN_ITEMS.md`'s own entry,
including a live check against a real multi-day XMLTV sample that
found zero onscreen-only entries on that specific instance, so this
fixes a real, confirmed-from-source mechanism without itself confirming
it currently bites this addon's own lab account): a fallback for
`<episode-num system="onscreen">`, used only when no `xmltv_ns` entry is
present on the same programme at all. Dispatcharr's own XMLTV export
(`apps/output/epg.py`) only emits `xmltv_ns` when *both* season and
episode are known -- an episode-only programme (no season) is instead
exported solely as an onscreen value like `"E12"`, which `ParseEpisodeNum()`
never reads (it only looks at `system="xmltv_ns"`), silently losing that
episode's own number, its `EPG_TAG_FLAG_IS_SERIES` flag, and (per
`ShouldIncludeEpisodeDates()`) its Year/FirstAired display. Recognizes
`S<n>E<m>`/bare `E<m>` case-insensitively; unlike `ParseEpisodeNum()`'s
own 0-indexed `xmltv_ns` convention, an onscreen number is already
exactly what it says, so neither field gets a `+1` adjustment here.
Leaves both unknown for anything else (no episode marker at all, `S`/`E`
with no digits after it, or trailing garbage after the digits) rather
than guessing, the same "don't guess" convention `ParseEpisodeNum()`
already uses. `ShouldIncludeEpisodeDates()` (`EpgTagUtil.h`) needed no
code change but did need an explicit decision this entry's own earlier
pass had deliberately deferred (see that function's own comment): an
episode-only match now flows through its existing `episodeNumber > 0`
branch like any other known-episode case, since the placeholder-`<date>`
problem that function guards against is specifically a programme with
*no* episode identity of its own repeating one `<date>` across genuinely
distinct airings -- not true of an onscreen-sourced episode number,
which is a real, deliberate per-episode identity a provider chose to
express without a season.
`RecurringRuleUtil` is the pure integer-arithmetic core of
`PVRDispatcharr::ComputeRecurringRuleFields`, pulled out of
`PVRDispatcharr.cpp` instead -- it converts Kodi's UTC start-end-time-of-day/
first-day and its own weekday bitmask (which Kodi's dialog sets in its own
*local* calendar with no conversion at all, unlike start/end/first-day --
see `ComputeRecurringRuleDayShift()` below; this entry's own earlier
"Kodi's UTC-based weekday bitmask" wording was wrong, corrected in a
33rd-pass audit, 2026-09-26) into Dispatcharr's own
representation via plain modulo-86400 day/time-of-day math, with the real
timezone shifts (bridging to Dispatcharr's own non-UTC-by-default system
timezone, and Kodi's own local timezone, see below) taken as explicit
offset parameters rather than computed internally. Its own inverse, `ComputeRecurringRuleDisplayTimes()`,
rolls an overnight rule's (e.g. 23:00-01:00) displayed end time forward
a day (added 2026-09-26, confirmed against Dispatcharr's own real
current upstream source that its scheduler already treats
`end_time <= start_time` as crossing midnight the same way -- an
18th-pass audit cloned it into a scratchpad, never committed to this
repo) -- before this, such a rule displayed an end time before its
start time in Kodi's Timers list.
`ComputeRecurringRuleDisplayTimes()` also gained a `resolveOffsetMinutes`
callback parameter in place of a single pre-resolved `offsetMinutes` int
(added 2026-09-29, fixing a real, confirmed, LIVE bug -- see
`docs/OPEN_ITEMS.md`'s 2026-09-29 entries): resolving the offset only
once, at `startDate`'s own UTC midnight, was wrong by the DST delta for
any rule whose `startTimeOfDaySeconds` itself falls on the *other* side
of a transition landing that same calendar day -- confirmed live against
a real fall-back-date rule, off by exactly one hour,
wrong from the very first `GetTimers()` read of a freshly
server-created rule, not just after an edit round trip. Resolves this
via a fixed-point iteration (start from the offset at `startDate`,
tentatively convert, re-resolve at that tentative result, repeat),
capped at 2 refinements as a defensive bound, not because more are ever
expected to matter -- this addon only ever computes a simple +/-1h DST
delta (`TimeZoneUtil.cpp`), so this converges immediately in every real
case. The single refined offset is reused for `endTimeOut`/`firstDayOut`
too, matching this function's own pre-existing "one offset for the
whole rule" design. `AddTimer()`/`UpdateTimer()`'s own edit-path
`ComputeRecurringRuleFields()` needed no equivalent change: it already
resolves at `timer.GetStartTime()`, a real absolute instant handed in
directly, not a startDate+time-of-day combination that needs resolving
before it's even known which side of a transition it lands on, so it
never had this specific chicken-and-egg problem. Re-verified live after
the fix, against the exact same real rule that reproduced the bug: the
addon was rebuilt, redeployed to the real Kodi test client, and
`PVR.GetTimers` then reported the rule's correct start instead of the
hour-early value.
`RecurringRuleUtil` also holds `ComputeRecurringRuleDayShift()` and
`RotateWeekdaysBitmask()` (added 2026-09-30, fixing a real bug confirmed
against Kodi's own real source and live -- see `docs/OPEN_ITEMS.md`'s own
entry): a repeating timer's weekday list and first day are read in Kodi's
local calendar (the dialog sets `m_iWeekdays` straight from its checkboxes
and only ever swaps the *date* part of first day, while start/end time are
converted with libc's `localtime()` of each instant,
`CPVRTimerInfoTag::ConvertUTCToLocalTime()`/`ConvertLocalTimeToUTC()`, so
per date, daylight saving included), but Dispatcharr reads `days_of_week`
and `start_date` in its own zone, so the two calendars disagree about which
day an occurrence falls on exactly when its UTC time of day lands on
different sides of midnight in the two zones -- by
`floor((U + offD)/86400) - floor((U + offK)/86400)` days for UTC time of day
`U` and both offsets taken at that occurrence's own instant, independent of
its date. `ComputeRecurringRuleFields()` (which now takes a
`kodiOffsetMinutesAt` resolver, rotates the weekday list forward by that
shift, and takes `startDateOut` from first day's *Kodi-local date* -- Kodi's
offset at first day itself -- moved by it, where it used to take the
Dispatcharr-local date of the firstDay *instant*) and
`ComputeRecurringRuleDisplayTimes()` (the same resolver, reporting the
`dayShiftOut` the caller rotates the list back by, and naming `firstDayOut`
as Kodi-local *noon* of the first occurrence's Kodi date -- noon so the date
survives a transition that day and a local midnight that doesn't exist or
happens twice -- so the round trip stays exact) are the identity whenever
the two zones agree, at every time of year. **A first attempt read Kodi's
offset once, at "now", assuming one cached bias for every date (what
`CDateTime::GetTimezoneBias()` is -- used elsewhere in Kodi, not by timer
conversion); caught live the same day: a Monday-Friday rule whose
start date was in the other daylight-saving period from today read
shifted weekdays in a Kodi that was in the *same* zone as its Dispatcharr.**
Zero Kodi/`DispatcharrClient` dependency, so unit-testable: besides
hand-worked vectors, the suite checks both directions against a model
written without the formula (for every pair of 27 real-world offsets, the
instants Dispatcharr would record equal the instants the Kodi dialog's rule
describes), the same for every pair of nine real zones from the addon's own DST table in
transition-free stretches, display-then-edit round-trip identity, no
start-date drift over repeated edits, and that Kodi and Dispatcharr in the
same daylight-saving zone never shift a rule on any day of the year -- and
each of nine deliberate mutations of the implementation makes it fail. Kodi's
own offset comes from `TimeUtil.h`'s `LocalUtcOffsetMinutes(at)` (the process's
own local offset at an instant via `localtime_r()`/`localtime_s()`, the same
call Kodi's own timer conversion makes), tested on POSIX against `TZ` strings
needing no tzdata, including the exact fall-back instant.
`RecurringRuleUtil` also holds `TruncateRuleTimesToWholeMinutes()` (added
2026-09-30, fixing a real bug found live -- see `docs/OPEN_ITEMS.md`'s entry on
dialog-carried seconds): `AddTimer()` (create only, never `UpdateTimer()`, since
Kodi echoes the seconds it was shown on every edit) drops the seconds Kodi's dialog
carried on a new recurring rule's start and end time, which reached Dispatcharr as
`HH:MM:28`-`HH:MM:28`. It leaves both alone when truncating would collapse a
start/end pair that differed into an equal one, because Dispatcharr reads
`end <= start` as an overnight rule.
`RecordingParser` is the pure field-mapping core of
`DispatcharrClient::ParseRecordingJson()` -- maps a single
`/api/channels/recordings/` item onto a `Recording`, including its
documented time-window/`custom_properties.status`-override
`isInProgress` logic (a real incident: a recording stopped early keeps
its originally-scheduled `end_time`, so the time window alone kept
reporting it in-progress after it actually finished -- see
`docs/RECORDINGS.md`) -- `status == "scheduled"` is authoritatively
upcoming (`isUpcoming = true`, `isInProgress = false`), `status ==
"recording"` is authoritatively in-progress, and any other non-empty
status is treated as finished (both false) -- confirmed against
Dispatcharr's own real current upstream source (an 18th-pass audit,
2026-09-26, cloned it into a scratchpad, never committed to this repo):
`sync_recurring_rule_impl()` (`apps/channels/tasks.py`) sets
`"scheduled"` on every future recurring-rule occurrence, and
`extend()`'s own status check (`apps/channels/api_views.py`) is the
canonical terminal set, `("completed", "stopped", "interrupted")`. An
earlier version of this fix (this session's 9th pass) treated any
non-empty status, including `"scheduled"`, as clearing `isUpcoming` --
a real regression caught before merging (the branch this addon lives
on, not `master`): every future occurrence of every recurring rule
vanished from Kodi's Timers list and incorrectly appeared in
Recordings instead, immediately on creation. Deliberately excludes
`ParseRecordingJson()`'s `PendingTitle`-cache lookup (member state
behind a mutex, not available
to a free function) and its final `"Recording <id>"` default title,
both of which stay in the thin wrapper that calls it first.
`PluginRunResult` is `DispatcharrClient::UnwrapPluginRunResult()`
(shared by every companion-plugin `run/` caller: `CallTimeshiftPluginAction()`,
`StopTimeshiftBuffer()`, `GetRecordingEdl()`, `RefreshLiveManifest()`) --
already had zero member-state dependency, so it moved out entirely
rather than staying behind a delegating wrapper; checks the outer
`{"success", "error"}` envelope every plugin `run/` response is wrapped
in, then the plugin's own inner `{"status", "message"}` result.
`RealtimeUpdateParser` is the pure wire-shape/relevant-event-type
classification core of `PVRDispatcharr::HandleRealtimeUpdateMessage()`
-- confirmed against Dispatcharr's own `consumers.py`/`utils.py`/
`tasks.py`/`api_views.py` for both the `{"data": {"type": ...}}` wire
shape and which event names on that shared "updates" channel are
actually recording/timer-relevant (everything else, e.g. EPG matching
progress, is silently ignored, not an error).
`M3u8SegmentParser` is the pure parsing core of
`DispatcharrClient::RefreshInProgressRecordingManifest()`'s `#EXTINF`/
segment-URI scan -- parses an in-progress-recording HLS playlist into
only its newly-discovered segments (the append-only-merge skip-count
convention), resolving a relative segment URI against the playlist's
own base directory. Guards against a non-finite (`NaN`/`inf`) `#EXTINF`
duration escaping into a returned entry -- found via a project-wide UB
review, not reproduced live: `std::stod` (unlike `std::stoi`) accepts
either as valid input without throwing, and a non-finite double later
feeding a `static_cast<int64_t>()` in the caller would be undefined
behavior (the C++-side counterpart to the nan/inf parsing bugs this
project's own companion plugins already had fixed, see
`docs/TIMESHIFT.md`/`docs/RECORDING_EDL.md`). Also clamps a
finite-but-huge or negative `#EXTINF` duration (added 2026-09-26, a
28th-pass audit, fixing a real UB gap the non-finite guard above didn't
cover, found via a project-wide review, not reproduced live):
`std::isfinite()` alone doesn't catch e.g. `1e300`, and the caller's
own `static_cast<int64_t>(durationSec * 1000 + 0.5)` is still undefined
behavior once that's out of `int64_t`'s range once scaled -- the exact
class of bug already fixed in this project's own Python-side
`_parse_edl` (a finite value overflowing only once scaled). A negative
value (`"#EXTINF:-1,"` is a real M3U idiom, not just adversarial input)
also passed the old check and would make `timeOffsetMs`/
`totalDurationMs` non-monotonic. Clamps negative/non-finite to 0 and
anything over a generous 1-day-per-segment ceiling to that ceiling.
`SegmentLookup` is `FindSegmentContainingPosition<SegmentT>()`, a
template (duck-typed on a `byteOffset`/`byteSize` member pair, matching
`CatchUpUtil`'s own templating approach) shared by
`ReadInProgressRecordingStream()` and `ReadLiveTimeshiftStream()` --
finds which known segment, if any, contains a given byte position in
that stream's own cumulative address space. Was identical, duplicated
logic in both functions before this extraction.
`ChannelParser` (`ParseChannelJson()`/`ParseChannelGroupJson()`) and
`TimerRuleParser` (`ParseTimerRuleJson()`/`ParseRecurringRuleJson()`)
are the same per-item field-mapping-core pattern as `RecordingParser`,
applied to `DispatcharrClient::GetChannels()`/`GetChannelGroups()`/
`GetTimerRules()`/`GetRecurringRules()`'s own per-item loops --
`ParseChannelJson()` in particular preserves real documented fallback
chains (`channel_group` as a nested object or a bare id, `tvgId` from a
nested `epg_data` object or the channel's own effective/plain field, and
-- added 2026-09-26, fixing a real asymmetry found via a project-wide
review, confirmed against Dispatcharr's own real current upstream
source -- `name`/`channelNumber`/`logoId`/the bare-id `groupId` each
preferring their own `effective_name`/`effective_channel_number`/
`effective_logo_id`/`effective_channel_group_id` over the raw field,
the same per-field `ChannelOverride` mechanism `tvgId`/`epgDataId`
already preferred theirs for. The channel-number case matters beyond
display: Dispatcharr's own XMLTV export keys `<channel id>` by
`effective_channel_number` in its default `tvg_id_source` mode -- the
mode this addon's own channel-number-keyed EPG lookup already assumes
-- so an overridden channel number this addon didn't also read broke
EPG matching for that channel entirely, not just its displayed number).
`RecordingParser` also gained `ParseRecordingEdlEntryJson()` the same
way, covering `GetRecordingEdl()`'s per-entry mapping and its
end-after-start validity filter -- and, as of 2026-09-26 (fixing a real
gap found via a project-wide review, not reproduced live), coercing an
out-of-range `type` value to `3` (COMBREAK): Kodi's own `PVR_EDL_TYPE`
is an unscoped enum with no fixed underlying type, so
`GetRecordingEdl()`'s `static_cast<PVR_EDL_TYPE>()` is undefined
behavior in C++ for a value outside its defined `0`-`3` range.
`TimerIdentity` is `ComputeSeriesRuleClientIndex()`, the hash-based
`(title, tvgId)` identity scheme `PVRDispatcharr::GetTimers()` uses to
give a series rule -- which has no real numeric id in Dispatcharr's own
API -- a stable Kodi `ClientIndex`, masked into the lower 30 bits so the
high "series rule" flag bit is never disturbed.
`LiveManifestParser` is the pure filtering core of
`DispatcharrClient::RefreshLiveManifest()`'s segment-merge loop --
the live-timeshift counterpart to `M3u8SegmentParser`, but for the
`get_live_manifest` plugin response's JSON `"segments"` array rather
than a raw m3u8 playlist. Keeps only sequences newer than the highest
one already merged, and drops a malformed entry (empty filename or
non-positive `byte_size`) rather than letting it corrupt the caller's
own cumulative byte/time offsets for every segment merged after it.
Deliberately does not compute `byteOffset`/`timeOffsetMs` itself, since
those are cumulative over the caller's own running totals (member
state) -- the caller still assigns those while applying each returned
entry in order. Also gained an upper clamp on `byteSize` (added
2026-09-27, a 52nd-pass audit, fixing a real, confirmed gap found via a
project-wide review, not reproduced live): unlike `durationMs`
(clamped since the 28th pass, see its own entry below), `byteSize` had
no upper bound at all, despite the exact same overflow risk --
`AppendSegmentOffsets()` sums each entry's `byteSize` into the caller's
own cumulative total, and two sufficiently huge values summed is signed
int64 overflow, undefined behavior in C++. No real HLS segment is ever
legitimately anywhere near the new, generous 1 TiB-per-segment ceiling.
`LiveEdgeMargin` holds two templates (same `byteOffset`/`byteSize`/
`timeOffsetMs`-duck-typed approach as `SegmentLookup`/`CatchUpUtil`):
`ComputeLiveEdgeTailTarget()` computes a forward-seek clamp target
backed off from the true tail by the combined size of the trailing N
segments -- shared by `SeekInProgressRecordingStream()` (margin 1) and
`SeekLiveTimeshiftStream()` (margin 3), unifying what had been two
separately-written but mathematically-identical computations, each with
its own real confirmed-live incident behind the backoff (a "skip ahead
to live took ~10s" investigation, and separately a live-edge-only H.264
decode-error/audio-desync storm documented in
`docs/TIMESHIFT.md`'s "Packet corrupt" section).
`TrimToTrailingLiveEdgeMargin()` is `OpenLiveTimeshiftStream()`'s own
cold-start trim/rebase (discards everything but the trailing margin,
shifting the survivors so the oldest becomes local byte/time 0) --
confirmed live as necessary to avoid a `CDVDDemuxFFmpeg::SeekTime`
landing near the MPEG-TS 33-bit PTS wraparound point on a
long-running, reattached buffer.
`RecordingVisibility` is `IsListedAsRecording()`/`IsListedAsTimer()`/
`ShouldUseGrowingBufferPlayback()` (whether a recording belongs under
Kodi's Recordings vs. Timers view, and whether it needs the
growing-buffer read path), `ResolveInProgressFinished()` (the
non-sticky in-progress-manifest-refresh decision `RefreshInProgressRecordingManifest()`
uses), `ShouldStopInsteadOfDelete()` (`DeleteTimer()`'s Stop-vs-Delete
decision, erring toward Stop whenever either Kodi's own possibly-stale
`forceDelete` or a fresh server lookup says a recording is still in
progress -- a real, confirmed data-loss fix: Kodi's cached timer state
lagging `recording_refresh_minutes` behind used to let a recording that
had genuinely already started reach the destructive `DeleteRecording()`
path instead of `StopRecording()`) -- **confirmed live 2026-09-30**:
`PVR.DeleteTimer` on a timer that was actually recording stopped the
recording and kept it (server status `stopped`, file recorded, listed by
Kodi as a finished recording), while the same call on a not-yet-started
timer deleted it, and `IsAlreadyFinishedRecording()`
(added 2026-09-26, fixing a second data-loss risk the fix above didn't
cover: a recording short enough to both start and finish inside one
`recording_refresh_minutes` interval could still show as a SCHEDULED
timer to Kodi's stale cache even though it already completed normally
-- checked before `ShouldStopInsteadOfDelete()` in `DeleteTimer()`, and
when true, neither Stop nor Delete runs at all, since there's nothing
left recording and the file is real, wanted content, not an empty
timer). `IsAlreadyFinishedRecording()` also gained a second call site in
`UpdateTimer()` (added 2026-09-26, the missed counterpart of the
`DeleteTimer()` fix): the same stale-cache window let an edit on an
already-finished recording fall into the "not yet started" PATCH
branch, resending an already-past `end_time` that Dispatcharr's own
validation rejects outright -- failing even a harmless title-only edit,
since `RenameRecording()` was never reached once that PATCH failed
first.
`IsListedAsTimer()` also gained a `time_t now` parameter and a new
companion, `IsMissedOccurrence()` (both added 2026-09-26, a 20th-pass
audit, confirmed against Dispatcharr's own real current upstream
source, not itself independently reproduced): a recurring occurrence
whose entire scheduled window elapsed before Dispatcharr ever moved it
out of `status=="scheduled"` (e.g. Dispatcharr/Celery down for its
whole window -- the same scenario `RecurringRuleRenewal`'s own
`endTime > now` fix above already accounts for on the renewal side)
stays `isUpcoming` forever, since nothing server-side ever revisits
such a row afterward (`purge_recurring_rule_impl` only deletes
`start_time >= now` rows; startup recovery only resumes/finalizes a row
still in-window or already `"recording"`). Before this,
`IsListedAsTimer()` listed it as a perpetual SCHEDULED timer Kodi itself
never expires (its own pruning only applies to a non-client-owned
timer, confirmed against Kodi's real source) and this addon can't
safely edit either (any edit's PATCH sends an already-past `end_time`,
rejected outright). `IsMissedOccurrence()` excludes it from
`IsListedAsTimer()` (an in-progress occurrence still wins outright,
regardless of its own `endTime` -- not "missed", just running past its
originally-scheduled end per `ParseRecordingFields()`'s own
status-override handling); `IsListedAsRecording()` already excluded it
(still `isUpcoming`), so a missed occurrence is now simply invisible in
Kodi rather than stuck in an unreachable, unfixable state.
`RecordingDirectory` is `SanitizeRecordingDirectory()` (added
2026-09-26, a 21st-pass audit, fixing a real, confirmed bug found via a
project-wide review, confirmed against Dispatcharr's own real current
upstream source, not itself independently reproduced): mirrors
Dispatcharr's own `_safe_name()` (`apps/channels/tasks.py`) filename
sanitization -- strips the same forbidden-filename-character set
(`\ / : * ? " < > |`) and trims -- so `GetRecordings()`'s own
`SetDirectory(rec.title)` call matches the single flat per-show folder
Dispatcharr itself actually wrote the recording under. A title
containing a forward slash (e.g. "Face/Off", "20/20") passed straight
through used to turn into a *nested* Kodi folder instead, confirmed
against Kodi's real source: `CPVRRecordingsPath` treats a raw `/` in
`Directory` as a path separator, and only the title itself gets
`CURL::Encode()`'d, not the directory. Falls back to `"Recording"` for
the rare case sanitizing consumes the whole title, matching this
addon's own already-existing "never an empty Directory" convention.
**Confirmed live 2026-09-30**: an API-created recording titled `Face/Off`
appears under the single flat folder `FaceOff`, and a title containing every
forbidden character (`/ : * ? " < > | \`) under one sanitized folder, with
no nested folder in either.
`RecordingHttpUtil` is `IsInProgressHlsRedirect()` (the pure
classification core of `OpenRecordingStream()`'s own "is this actually
an in-progress recording's HLS redirect, not a completed file" check)
and, added 2026-09-26 (a 22nd-pass audit, fixing a real, confirmed bug
found via a project-wide review, confirmed against Dispatcharr's own
real current upstream source, not itself independently reproduced),
`HasRecordingFileChanged()`: whether a completed recording's own file
appears to have been swapped out from under an already-open
`ReadRecordingStream()`. Dispatcharr's own `comskip_process_recording()`
runs automatically right after a recording finishes (when comskip is
enabled), and its own "cut" mode `os.replace()`s (or, on that failing, a
non-atomic `shutil.copy()` over) the original file with a shorter,
commercial-trimmed one -- reachable while a client is already
mid-playback if it started watching the recording right after it
finished, before comskip (which can take minutes) is done. Without this
check, `ReadRecordingStream()` kept using the length cached at open time
against the new, shorter file's actual bytes at the same offsets --
silently reading the wrong content with no error anywhere.
`ReadLiveTimeshiftStream()` already guards its own analogous case the
same way (a `Content-Range`-reported total disagreeing with a cached
size); this mirrors that, via the same `RecordingHeaderCallback`
(`CurlCallbacks.h`) already used by `OpenRecordingStream()`'s own probe.
`RecordingVisibility` also gained `ClassifyOneTimeTimerEdit()` and its
`OneTimeTimerEditAction` enum (added 2026-09-26, a 22nd-pass audit): the
pure classification core of `UpdateTimer()`'s own one-time-recording
branch, an inline, order-sensitive 4-way decision (in progress -> missed
occurrence -> already finished -> not yet started) that two separate
earlier passes (20 and 21) each added a new case to, each time
inserting it at a specific point in what used to be an if/else-if chain
to get the ordering right relative to the others. Extracted so that
ordering is locked in by a test (including that `isInProgress` must win
over every other case, even when another case's own condition would
also otherwise match) instead of resting on each future change getting
it right again by inspection.
`RecordingVisibility` also gained `ShouldRetryInProgressColdStart()` (added
2026-09-30, fixing a real, live-confirmed bug -- see `docs/OPEN_ITEMS.md`'s
entry on the in-progress cold start): whether
`OpenInProgressRecordingStream()`'s cold-start loop should keep waiting
after a *failed* playlist fetch. It used to return on the first one, so a
recording opened within roughly its first three seconds -- before
Dispatcharr's HLS directory or `index.m3u8` exists, both of which the
`hls()` view answers with a 404 -- failed every time (6 of 6 live), even
though the same loop's 45s budget already waited patiently for a playlist
that exists but has no segment yet. Deliberately narrow: only an HTTP 404
(not a 3xx, which means the directory was already removed and the file is
complete, nor a transport failure, 401 or 5xx) *and* a lookup that
succeeded and says the recording is in progress right now, so a finished,
failed or deleted recording still fails fast instead of burning the whole
budget. `FetchRawInProgressPlaylist()` gained an `httpStatusOut` for it and
`RefreshInProgressRecordingManifest()` a `coldStartRetryableOut`. Re-verified
live: opens made 0.9-1.0s after creation retried the missing playlist 2-3
times and reached advancing playback in 4.1-5.8s (4 of 4). (Two trials in
an earlier run looked like failures but were the test harness stopping the
recording mid-wait after ~8s -- exactly the not-in-progress case this
declines to retry.)
`RecordingVisibility` also gained `IsInProgressContentGone()` (added
2026-09-30, fixing a real, live-confirmed bug -- see `docs/OPEN_ITEMS.md`'s
entry on an in-progress recording deleted while watched): whether an
in-progress stream's server-side HLS content is permanently gone. A
recording deleted on the server while a viewer sat at its tail (real-time
updates off, so Kodi couldn't learn of it any other way) made the addon
alternate a recording lookup and a playlist fetch, every one a 404, at
~7.8/s for ~100s until Kodi's own no-data timeout closed the stream --
`ResolveInProgressFinished()`'s "a failed lookup is unknown" rule swallowed
the lookup's 404, and the refresh throttle only arms on a successful
refresh. Requires that the stream already had segments (so a young
recording whose HLS directory isn't there *yet* is never mistaken for a
gone one), a playlist that definitively answered 404 or a redirect, and
either a lookup that answered 404 (deleted) or one that succeeded and says
the recording is no longer in progress (finished, directory removed); a
lookup that merely failed stays unknown and one that says the recording is
still in progress never counts (a naturally completing recording's status
is only written after its directory is removed). `DispatcharrClient` sets a
sticky `InProgressRecordingStreamState::contentGone` on a yes, after which
refreshes return without a request and a read needing an uncached segment
returns EOF (not `-1`, which Kodi retries near-immediately); a segment 404
runs one throttled refresh so a mid-buffer viewer finds out too. Re-verified
live: after the delete the addon made exactly two requests and went silent
(vs 766 over 98s), for a viewer at the tail, paused, and playing
mid-recording, and for a paused viewer whose recording finalized and lost
its directory while the addon couldn't reach the server.
`RecordingVisibility` also gained `IsRecurringOccurrenceReschedule()` (added 2026-09-30,
fixing a real bug confirmed live -- see `docs/OPEN_ITEMS.md`'s entry on moving one
recurring occurrence): `UpdateTimer()` refuses a start/end change on a recording that
belongs to a recurring rule. Dispatcharr's hourly `maintain_recurring_recordings` only
checks for a recording at the rule's *original* slot, so it recreated that slot beside the
edited occurrence (a duplicate provider stream, seen 48 minutes after the edit), and any
rule-level PATCH -- including this addon's own `end_date` renewal -- purged and regenerated
every future occurrence, reverting the edit (seen four minutes after one). A title or
channel edit doesn't move the slot and is unaffected.
`RecordingVisibility` also gained `GateFinishedOnEndList()`, with `M3u8HasEndList()`
(`M3u8SegmentParser.h`) (added 2026-09-30, fixing a real bug reproduced live -- see
`docs/OPEN_ITEMS.md`'s entry on a user Stop stranding the final segment and
`docs/RECORDINGS.md`): Dispatcharr's stop endpoint writes `status = "stopped"`
synchronously while ffmpeg is torn down afterwards, and measured live the playlist stayed
one segment short, with no `#EXT-X-ENDLIST`, for about three seconds after the flip, then
gained the final segment and the tag in one write -- so a viewer at the live edge was
handed EOF a segment early (`finished=1` at its final byte count, a further segment
probed 2.6s later). `RefreshInProgressRecordingManifest()` now holds `finished` false
while the status says finished but the playlist has no tag, with a 15-second grace for a
recording that dies without writing it; the path where Dispatcharr already removed the HLS
directory (a redirect/404 on the playlist) is not gated. The same change removed the
refresh's separate `Range: 0-0` API-key re-check of the playlist URL (and its
`IsApiKeyValidFor()` helper), which duplicated what `FetchRawInProgressPlaylist()` already
proves: 130 playlist GETs per 40s at the live edge became 65.
`LiveEdgeMargin` also gained `FirstAvailableLiveSegmentIndex()` and `LiveManifestParser`
`OldestLiveManifestSequence()` (added 2026-09-30, fixing a real bug reproduced live -- see
`docs/TIMESHIFT.md`'s "Resuming after the plugin's rolling buffer has moved on"):
`timeshift_buffer` lists `buffer_minutes` of segments but ffmpeg's `-segment_wrap` recycles
filenames at twice that, and this addon's append-only address space kept listing segments the
plugin had let go of, so a position left behind the window (a long pause) read a recycled
file and the `Content-Range` cross-check ended the whole stream. The manifest's lowest
sequence now marks what is still real: `ReadLiveTimeshiftStream()` moves a stale position up to
it, `SeekLiveTimeshiftStream()` clamps to it, and `GetStreamTimes()` reports it as `PTSBegin`.
Reproduced with the server's `buffer_minutes` at 1 (182s pause killed playback), then re-run
with no fatal.
`RecurringRuleRenewal` is `ShouldRenewRecurringRule()`, the per-rule
decision core of `PVRDispatcharr::RenewRecurringRules()`'s loop --
whether a rolling recurring rule's `end_date` should be pushed forward
this cycle, skipping a disabled rule, one whose `end_date` has already
passed (added 2026-09-26, fixing a real bug found via a project-wide
review: a negative `daysLeft` was never caught by the "comfortably
inside the window" check below it, so an expired-but-still-enabled
rule fell through to the same renewal logic as one genuinely nearing
its window limit, silently resurrecting it), one still comfortably
inside its window, one with an occurrence currently recording or
starting within a safety margin (also requiring `rec.endTime > now`,
added 2026-09-26 in a 19th-pass audit, confirmed against Dispatcharr's
own real current upstream source, not itself independently reproduced:
without it, a recurring occurrence that never actually ran -- e.g.
Dispatcharr/Celery down for its whole window -- stays at
`status=="scheduled"` forever with both start and end time now in the
past, per `ParseRecordingFields()`'s own authoritative handling of that
status; its `rec.startTime - now` is then always a large negative
number, always less than the safety margin, so it read as permanently
"imminent" and silently blocked this rule from ever renewing again,
right up until its own `end_date` ran out and this same function
started refusing it as expired instead -- defense in depth around
Dispatcharr's
own regeneration behavior), or -- erring toward skipping rather than
renewing blind -- one where `GetRecordings()` itself failed and that
occurrence-safety check can't be evaluated at all.
`RecurringRuleRenewal` also gained `ShouldExtendRecurringRuleEndDateOnUpdate()`
(added 2026-09-26, fixing a real gap the fix above opened): whether
`PVRDispatcharr::UpdateTimer()`'s own recurring-rule edit should also
restore a stale `end_date`, since `UpdateRecurringRule()`'s own PATCH
deliberately omits it -- without this, a rule left disabled long enough
for its `end_date` to pass, then re-enabled from Kodi, stayed enabled
with an already-past `end_date` forever, since the periodic renewal
loop now correctly refuses to touch one. Requires a genuine
`wasEnabled=false -> enabled=true` transition, not just the post-edit
`enabled` value alone (refined the same day, in a later pass, fixing a
real gap found via a project-wide review: checking only the post-edit
state fired on *any* edit of an already-enabled rule -- a rename, a
schedule tweak -- not just an explicit re-enable, reopening the
"don't resurrect an expired rule" protection above through an edit that
had nothing to do with enabling anything). Also guards against a
zero/unparseable `cachedEndDate` (added 2026-09-26, a further pass of
the same review): that's the same "no end_date at all" sentinel
`ShouldRenewRecurringRule()` already relies on for a genuinely
open-ended rule -- without this guard, re-enabling one would give it a
brand-new finite `end_date`, silently converting "records forever" into
"records only while this addon keeps running to renew it".
`RecurringRuleRenewal` also gained `ComputeRecurringRuleEndDateForStartDateChange()`
(added 2026-09-26, a 19th-pass audit, confirmed against Dispatcharr's
own real current upstream source, not itself independently reproduced):
whether `PVRDispatcharr::UpdateTimer()`'s own recurring-rule edit needs
to push `end_date` forward *before* its main PATCH, not after -- a
distinct failure from `ShouldExtendRecurringRuleEndDateOnUpdate()`
above, reachable on a plain schedule edit to an already-*enabled* rule,
not just a disabled->enabled transition. `UpdateRecurringRule()`'s PATCH
omits `end_date`, relying on `RecurringRecordingRuleSerializer`'s
partial-update fallback to the instance's existing value -- but that
same `validate()` rejects `end_date < start_date`, and (an overnight
rule's own case, where the two dates can land equal)
`combine(end_date, end_time) <= combine(start_date, start_time)` too.
Kodi's own First Day picker offers dates up to a year out, against a
renewed rule's own cached `end_date` sitting at most
`kRecurringRuleWindowDays` out, so moving First Day forward past (or,
for an overnight rule, onto) the rule's own current `end_date` used to
fail the edit outright with a 400. `UpdateTimer()` now extends
`end_date` first (via `ExtendRecurringRuleEndDate()`) whenever this
returns nonzero, before ever sending the main PATCH.
`RecurringRuleRenewal` also gained `HasRecurringRuleEndDatePassed()`
(added 2026-09-26, a 31st-pass audit, fixing a real, confirmed gap
found via a project-wide review, confirmed against Dispatcharr's own
real current upstream source, not itself independently reproduced):
whether `PVRDispatcharr::GetTimers()` should display an enabled
recurring rule as `PVR_TIMER_STATE_ERROR` rather than a healthy
`PVR_TIMER_STATE_SCHEDULED` once its own `end_date` has passed --
`ShouldRenewRecurringRule()` above already refuses to push such a
rule's `end_date` forward again (deliberately, since it can't tell an
addon-managed rolling-window rule apart from one a user created
directly via Dispatcharr's own web UI with a genuine, finite
`end_date`), so before this, a rule in exactly that state displayed as
perfectly healthy while silently recording nothing at all.
Gained an `offsetMinutes` parameter (added 2026-09-26, a 36th-pass
audit, fixing a real, confirmed off-by-one-day bug in the fix above,
also confirmed against Dispatcharr's own real current upstream source,
not itself independently reproduced): the original `endDate <= now`
check ignored that Dispatcharr's own `end_date` is *inclusive* --
`sync_recurring_rule_impl()`'s own day-generation loop
(`apps/channels/tasks.py`) still schedules a real occurrence ON the
`end_date` day itself (in Dispatcharr's own configured system timezone,
not UTC), only skipping a day once `target_date > end_limit` -- so a
rule was flagged `PVR_TIMER_STATE_ERROR` the instant UTC clock reached
midnight of the `end_date` calendar day, which for a zone behind UTC
(e.g. America/New_York) is still the *previous evening* in
Dispatcharr's own local time, up to a full day or more before that
rule's own last occurrence had even aired. Now uses
`endDate + 86400 - offsetSeconds` -- the UTC instant marking
Dispatcharr's own local calendar moving past `end_date` entirely,
via the same offset convention `ComputeRecurringRuleDisplayTimes()`'s
own `firstDayOut` already uses -- deliberately conservative (a whole
extra day's margin) rather than also accounting for the `end_date`
occurrence's own end-of-day/overnight-rollover time, matching this
codebase's own established tolerance elsewhere for "a valid day, not
necessarily the exact moment". Deliberately *not* applied to
`ShouldRenewRecurringRule()`'s own `endDate <= now` check in the same
pass -- see `docs/OPEN_ITEMS.md` for why that half needs its own
design decision first (it would remove an existing, if accidental,
protection against the "which rules does this addon actually own"
ambiguity that function's own comment already documents).
`WebSocketFrame` is `BuildMaskedControlFrame()`, the RFC 6455
masked-frame byte-layout builder behind `WebSocketClient::SendPong()`/
`SendClose()` -- covers the FIN/opcode/MASK-bit header byte, the
extended 2-byte length prefix for a payload over 125 bytes (defensive;
never actually hit in practice, a pong payload always echoes a
spec-capped-at-125-byte ping), and the mask-key XOR cycling. Takes the
mask key as an explicit parameter rather than generating it internally
(a real caller always does, via `RandomBytes()`, per the spec's own
masking requirement), so the exact byte output is testable against a
known key. The same file also holds the decode counterpart used by
`WebSocketClient::ReceiveTextMessage()`: `ParseFrameHeaderBytes()` (the
FIN/opcode/MASK-bit/7-bit-length-field bit-twiddling from a frame's
first 2 header bytes), `DecodeExtendedPayloadLength16()`/
`DecodeExtendedPayloadLength64()` (the big-endian 126/127 extended-length
sentinels), and `UnmaskPayload()` (the same XOR-cycling `BuildMaskedControlFrame()`
applies when masking, exposed separately since the receive path unmasks
an already-received payload rather than building one). Also holds
`DecideWebSocketDataFrameAction()` (added 2026-09-26, fixing a real bug
found via a project-wide review, not reproduced live): the
accumulate/drop/complete decision for a just-received data frame,
tracking which kind of message (none/text/binary) `ReceiveTextMessage()`
is currently assembling across fragmented (continuation, opcode `0x0`)
frames. Replaced an inline switch that had two bugs -- a dead `if (fin
&& !assembling) continue; continue;` branch, and a non-FIN binary
frame's own continuation frames (which share opcode `0x0` with a text
message's continuations; RFC 6455 doesn't tag which message type a
continuation belongs to) getting silently accumulated and returned as
text once FIN arrived.
`SeriesRuleMatching` is `MatchRecordingsToSeriesRules()`, the matching
core of `PVRDispatcharr::GetTimers()` -- which recording belongs to
which series rule (by `channelId`+`title`, the same identity
`DeleteSeriesRule()` uses), and each rule's own earliest upcoming/
in-progress match. Exists because a series rule has no fixed time of
its own; without this it displayed as the Unix epoch ("12/31/1969") in
Kodi -- a real confirmed-live display bug, not a cosmetic nitpick. The
title comparison is case-insensitive (added 2026-09-26, fixing a real
mismatch found via a project-wide review, confirmed against
Dispatcharr's own real upstream source rather than just its API shape:
`apps/channels/tasks.py`'s `evaluate_series_rules_impl()` matches
"exact" `title_mode` case-insensitively, `title__iexact`) -- a rule
whose title differed only in case from the matching recording's own EPG
title still recorded correctly server-side, but this addon's own
client-side re-linking never matched it, reintroducing the same
Unix-epoch display bug via a different path.
`SeriesRuleMatching` also reads a rule's own `title_mode`/`description`/
`description_mode` and strips/folds with Unicode-aware helpers (added
2026-09-30, fixing two real, live-confirmed gaps -- see
`docs/OPEN_ITEMS.md`'s series-rule title-matching entries and
`docs/RECORDINGS.md`'s 2026-09-30 update to the epoch-bug history). A
materialized recording carries only a `program` snapshot, never a reference
to its rule, so linking means re-running the rule's filters against it, and
this used to re-implement only `title__iexact` and only for ASCII.
`SeriesRuleTextMatch` is `MatchSeriesText()` and `ParseSeriesTextMode()`,
mirroring Dispatcharr 0.31.0's own `_evaluate_series_rules_locked()`:
`"exact"` is a case-insensitive whole-string match, every other title mode
but `"regex"`/`"search"` (so `"contains"` and any name the server doesn't
know) is `parse_text_query()`'s `icontains` with its AND/OR/quote/
parenthesis grammar (strictly left to right, operators only as a whole
" AND "/" OR ", a quote atomic only as a whole operand, the last-"("/
first-")" pairing that leaves a stray ")" behind a nested group, an empty
operand neutral rather than match-everything), `"search"` anchors each term
on a true `\y` word boundary, and a description always goes through the
same parser and is ANDed with the title. A regex filter is reported
`kUnsupported` and leaves the rule unlinked (PostgreSQL's regex syntax isn't
ECMAScript's) -- the status quo for every non-exact rule before this. Checked
against the real upstream parser, loaded unmodified with Django's `Q`
faked, on ~108,000 random query/text pairs: zero disagreements; 110 curated
ones are the test table. `UnicodeText` is `FoldCaseForDatabaseMatch()`,
`StripUnicodeWhitespace()` and `IsDatabaseWordCodePoint()` (UTF-8, no
locale, invalid bytes passed through): Python's `str.strip()` whitespace set
(so a trailing no-break space in a stored-unstripped rule goes) and simple
uppercase folding from `src/UnicodeTables.inc`, generated by
`tools/gen_unicode_tables.py` from glibc's `towupper()`/`iswalnum()` -- what
PostgreSQL's `UPPER()` and `\y` use on a libc-collation database -- rather
than from Python, whose full `str.upper()` mapping differs (a sharp s
becomes "SS", 27 Greek letters expand to two code points). The lab
database's locale was settled live first (an `icontains` for "ünder" found a
channel named "ÜNDER"). Verified exhaustively against glibc/Python for all
1,112,064 scalar values, and live: real Kodi against the real instance,
eleven rules covering every mode beside eight recordings, each linked or
left unlinked exactly as predicted.
`TimeZoneUtil` also has an alias table and `CanonicalKnownZoneName()` (added
2026-09-30, fixing a real gap confirmed against a real V8 -- see
`docs/RECURRING_RULES.md`): Chromium's `Intl.supportedValuesOf('timeZone')`,
which Dispatcharr's settings page stores verbatim, offers `Asia/Calcutta`,
`Europe/Kiev` and `America/Indianapolis` rather than the table's modern
names, so picking those zones in Chrome left recurring timers on the manual
offset. `ComputeKnownZoneOffsetMinutes()` accepts any alias (those three plus
IANA's `backward` links: `US/*`, `Canada/*`, country-name ids) and
`SyncTimezoneFromDispatcharr()` selects the canonical name in the dropdown.
`Staleness` also gained `MarkStaleForWake()` (added 2026-09-30, fixing a real
mechanism that could not be reproduced, since suspend could not be tested): every
staleness check runs on `steady_clock`, which does not advance during suspend,
so `OnSystemWake()` now ages the channel and guide loaded-at timestamps to
just-stale (never to the zero "never loaded" sentinel, which would make
`GetChannels()` and friends report an error on a cache holding real data) and
`DispatcharrClient::InvalidateAccessToken()` drops the cached token's
freshness hint. `TimerRequestBuilder` also gained `ChannelToSendOnOneTimeEdit()`
and a `channelId` on `BuildOneTimeRecordingPatchBody()` (added 2026-09-30, fixing
a real bug confirmed live -- see `docs/OPEN_ITEMS.md`): Kodi's edit dialog
lets a scheduled one-time recording's channel be changed, and the PATCH used
to drop it silently. A bare `{"channel": ...}` PATCH is a 500 server-side, so
it is sent beside both times; only a real channel that differs from the
recording's own is sent.
`RecurringRuleWeekdays` is `ComputeRecurringRuleWeekdaysBitmask()`,
converting Dispatcharr's `days_of_week` list into a Kodi `PVR_WEEKDAY`
bitmask -- confirmed against Kodi's real header that the two share the
same 0=Monday..6=Sunday bit order, so no reordering is needed, just a
plain `1 << day` (the calendar-day rotation between Kodi's zone and
Dispatcharr's is applied on top of this, see `RecurringRuleUtil`).
`ChannelGroupFilter` is `FilterChannelGroupsWithChannels()`, dropping a
channel group with no member channels left in the just-fetched channel
list -- Dispatcharr's `/api/channels/groups/` returns every group that
has ever existed, including ones no longer enabled for any M3U account,
and "enabled" isn't itself a property of the group to check directly.
`ChannelRenumbering` is `HaveChannelNumbersChanged()` (added 2026-09-26,
a 23rd-pass audit, fixing a real, confirmed bug found via a project-wide
review, confirmed against Dispatcharr's own real current upstream
source, not itself independently reproduced): whether any channel
common to two channel snapshots (matched by id) has a different
`channelNumber` between them. Dispatcharr's own "compact numbering"
scheme (`apps/channels/compact_numbering.py`, whose own docstring says
as much) shifts every channel number after a hidden/unhidden one to
close the gap, so hiding a single channel in a compact-numbered group
renumbers every later channel in it. `GetEPGForChannel()` keys its own
cache by channel number, and this addon's own channel and EPG refreshes
run on completely independent timers (`channel_refresh_hours`, default
12h, vs. `epg_refresh_hours`, default 4h) with nothing telling the EPG
side a renumbering just happened -- so a shifted channel could show its
former neighbor's guide (or none at all) for up to the full
`epg_refresh_hours` window even after the channel list itself already
picked up the new numbers. `EnsureChannelsLoaded()` now resets
`m_epgLoadedAt` (not `m_epgLastFailedAt`, so the failure backoff still
applies) whenever this returns true, comparing against `m_channels`
before it's overwritten -- naturally false on a first load (an empty
snapshot has no common id to disagree on yet), no special-casing
needed. Also catches a channel *id* new to this addon reusing a number
a different, since-removed id used to hold (added 2026-09-26, a
24th-pass audit, fixing a real, confirmed gap in the fix above found
via a project-wide review, confirmed against Dispatcharr's own real
current upstream source, not itself independently reproduced):
`Channel.get_next_available_channel_number()` (`apps/channels/models.py`)
hands out the lowest free number, so deleting a channel and creating
(or auto-syncing) a brand-new one can reuse the exact same number an
id-only comparison would otherwise miss entirely, since the two
snapshots share no common id at all -- the identical
wrong-guide-shown symptom, just reached a different way. Deliberately
does NOT also trigger for a genuinely new number never seen before (a
brand-new channel with nothing to reuse) -- that's "no guide yet", not
"wrong guide", the same tolerated delay any newly-added channel already
has until its own next natural EPG refresh.
`ChannelLineupChange` is `HasChannelLineupChanged()` and
`ShouldTriggerKodiChannelSync()` (added 2026-09-29, fixing a real,
live-confirmed inefficiency -- see `docs/OPEN_ITEMS.md`'s own entry):
`EnsureChannelsLoaded()` used to poke Kodi
(`TriggerChannelUpdate()`/`TriggerChannelGroupsUpdate()`) on every
successful commit, so each otherwise-uneventful periodic refresh -- and
the concurrent duplicate startup fetch -- cost a full pointless channel/
group resync. The gate compares the old and new lineup
order-insensitively by id over every `Channel` field plus the group
`(id, name)` set, and additionally fires an unchanged lineup once 24h
have passed since the last poke, so a fire-and-forget trigger Kodi missed
isn't lost until the next real change. Also see
`tools/tests/test_timezone_settings_sync.py`, a cross-file check that
`TimeZoneUtil.cpp`'s `kKnownTimeZones` and `settings.xml`'s
`recurring_rule_timezone` options list the same zones (drift between them
already caused one real regression, 2026-09-09).
`ApiKeyRecovery` is `ParseApiKeyListResponse()`,
`ClassifyApiKeyLookupFailure()` and `DecideApiKeyRecovery()` (added
2026-09-30, fixing a real, live-confirmed bug reported against 0.11.0 --
see `docs/OPEN_ITEMS.md`'s own entry): the pure decision core of
`DispatcharrClient::ObtainApiKey()`, which every "I need a working API key"
path now goes through (first-run setup, a stored key belonging to a
different account, and all five 401 self-heals). Dispatcharr keeps exactly
ONE key per account and `POST /api/accounts/api-keys/generate/` overwrites
it unconditionally (`APIKeyViewSet.generate()`, confirmed against its own
real upstream source), so this addon generating on first run and on every
401 silently revoked the key for every other client of the account --
other Kodi installs, scripts, MCP/automation tools -- just by being
enabled with an empty `api_key` setting. Reproduced live on the old build
two ways before fixing: a client starting with an empty `api_key`
replaced the account's existing key, and a 401 recovery replaced a key
another client had just rotated. `ObtainApiKey()` now reads the account's
existing key first (`GET /api/accounts/api-keys/`, `APIKeyViewSet.list()`:
`{"key": <str|null>}` for the caller, permission `Authenticated` only, so
any role) and adopts it as-is; `DecideApiKeyRecovery()` only chooses to
generate when the account has no key (`null`/`""`/whitespace) or an older
Dispatcharr can't report one (no `key` member, or a non-transient 4xx
such as 404/405), and refuses to generate on a *transient* lookup failure
(no response, 5xx, 408, 429, or an unusable 2xx body) -- rotating blind
could revoke a perfectly good key, so the caller's own retry tries again
later instead. A key equal to the one just rejected is still adopted
rather than regenerated: Dispatcharr resolves a key per request with a
plain `User.objects.get(api_key=...)` (no caching), so an equal key means
the 401 wasn't the key's fault, and the callers' existing
retry-once-then-give-up loops bound the cost. `GenerateApiKey()` is now
private and only reachable as that last resort, and `ObtainApiKey()` is
serialized by its own `m_apiKeyRecoveryMutex` (two threads recovering at
once could previously each generate a key and leave `m_config.apiKey`
holding the loser's).
`ReconcileGeneratedApiKey()` (added 2026-09-30, fixing a real,
live-reproduced residual in that fix -- see `docs/OPEN_ITEMS.md`'s same
entry) covers the one case where generating still happens concurrently:
two clients finding the account keyless at the same instant each generate
a key and Dispatcharr keeps only the last, so the earlier one's retry got
a 401 and its first open failed once (seen with a Linux and a Windows
client opening within ~3 ms). After a generate, `ObtainApiKey()` now
re-reads the account's key and switches to it when it differs from the one
just generated (`kAdoptServerKey`); a re-read that says "none", can't say,
or fails keeps the generated key (`kKeepGenerated` -- generating again would
only invite a loop). This resolves the near-simultaneous collision; it
moves the remaining failure window (another client's generate landing
between the re-read and the retry, about a round trip later) rather than
closing it.
`ChannelRenumbering` also gained `FindAmbiguousChannelNumbers()` (added
2026-09-28, fixing a real, confirmed bug found live -- the exact
duplicate-channel-number EPG-collision this file's own comments already
cross-referenced before this fix existed): Dispatcharr genuinely permits
two channels sharing the same `channel_number`, and its own
unauthenticated `/output/epg` export (this addon's only export path)
has no way to disambiguate a collision -- both colliding channels get
exported under the identical `<channel id="N">`. Confirmed live against
a real instance: pulled the real XMLTV feed and found exactly the
predicted shape (two separate `<channel id>` tags for one number, no
disambiguation, with real `<programme>` entries attached to that one
shared id), then confirmed the practical consequence through a real
Kodi client -- one colliding channel's own guide was showing the
*other* colliding channel's real programme titles verbatim.
`PVRDispatcharr::m_ambiguousChannelNumbers`, recomputed alongside
`m_channels` on every `EnsureChannelsLoaded()`, now gates both
`GetEPGForChannel()` and `ResolveRecordingBroadcastId()`'s own
`m_epgByChannelNumber` lookups -- an ambiguous channel now reports
plain "no guide data" on both sides of the collision (matching the
existing null-`channelNumber` convention) rather than risk showing
either channel's guide as the wrong one's. Re-verified live after the
fix: the same two colliding channels both correctly reported empty
guides, with a debug log line identifying which channel/number
triggered it, while an unrelated, non-colliding channel's own real
guide (its full guide) was completely unaffected.
`Staleness` is `IsStaleSince()`/`IsRetryDue()`/`ShouldThrottleRefresh()`
(the shared cache-staleness/failure-backoff/throttle sentinels behind
every `Ensure*Loaded()` pair and the live-timeshift/in-progress-recording
manifest refresh throttle) and `ShouldFetchChannels()` (the full gating
decision for `EnsureChannelsLoaded()`, combining channel staleness, a
pending groups-only retry, and the channels-failure backoff into one
call). `ShouldFetchChannels()` gained a `forceStale` parameter (added
2026-09-26, a 26th-pass audit, fixing a real, confirmed bug found via a
project-wide review, confirmed against Dispatcharr's own real current
upstream source, not itself independently reproduced): bypasses only the
ordinary channels-staleness check, not the failure backoff, so a caller
can force a channel refresh attempt without risking hammering a server
that's genuinely failing. `EnsureEpgLoaded()` now calls
`EnsureChannelsLoaded(/*forceStale=*/true)` immediately before its own
XMLTV fetch -- this addon's own channel and EPG refreshes run on
completely independent timers (`channel_refresh_hours`, default 12h, vs.
`epg_refresh_hours`, default 4h), so at these defaults a server-side
channel renumbering (Dispatcharr's own "compact numbering" scheme) is
far more likely to have its very next XMLTV refresh land *before* the
next channel refresh, not after -- the exact opposite ordering
`HaveChannelNumbersChanged()`'s own detection (`ChannelRenumbering.h`)
can't help with, since that only fires when the *channel* side notices a
renumbering first. Without this, the new XMLTV (already keyed by
Dispatcharr's own new channel numbers) got parsed and committed while
`m_channels` still held the old ones, silently pairing each renumbered
channel with its former neighbor's guide for however long the channel
side stayed stale (routinely most of a 12h window) -- a failed forced
fetch here doesn't block the XMLTV fetch itself, and channels now
effectively refresh at least as often as the EPG does as a deliberate
side effect. `EnsureChannelsLoaded()`'s own renumbering-detected branch
also now clears `m_epgByChannelNumber` outright, not just its own
loaded-at timestamp, so a subsequent forced re-fetch that's itself
delayed or fails leaves a renumbered channel showing no guide rather
than continuing to serve the wrong one (confirmed against Kodi's own
real source that its EPG update merges entries and keeps a channel's
existing tags when this addon returns none for it, rather than blanking
the display).
`Staleness` also gained `HasNeverLoadedSuccessfully()` (added 2026-09-27,
a 40th-pass audit, fixing a real, confirmed bug found via a project-wide
review, confirmed against Kodi's own real current SDK source, not itself
independently reproduced): whether a cache has never successfully loaded
even once, distinct from `IsStaleSince()`'s own "merely stale" check --
`PVRDispatcharr::GetChannels()`/`GetChannelsAmount()`/`GetChannelGroups()`/
`GetChannelGroupsAmount()`/`GetChannelGroupMembers()` all called
`EnsureChannelsLoaded()` and unconditionally returned `PVR_ERROR_NO_ERROR`
regardless of whether it actually succeeded, even on an addon instance
where the underlying Dispatcharr fetch has *never* once succeeded (so
`m_channels`/`m_groups` are still at their initial empty state, not
merely stale). Confirmed against Kodi's own real source that this is a
real data-loss bug, not just a wrong error code: `CPVRClients::ForClients()`
(`addons/PVRClients.cpp`) only adds a client to its own `failedClients`
list when a per-client call returns something other than
`PVR_ERROR_NO_ERROR`/`PVR_ERROR_NOT_IMPLEMENTED`, and
`CPVRChannelGroup::HasValidDataForClient()` (`PVRChannelGroup.cpp`) treats
a client NOT in that list as having reported valid (if empty) data --
safe to delete any channel or group member missing from what it just
returned. Since this addon never returned anything but
`PVR_ERROR_NO_ERROR` from these five callbacks, Kodi read an empty
channel/group list, on an instance that had never loaded anything, as
authoritative and deleted every channel, channel-group membership, and
associated EPG entry from its own database rather than merely leaving
the stale-but-present ones in place until the next successful refresh.
Fixed by returning `PVR_ERROR_SERVER_ERROR` from each of the five
callbacks instead, gated on a new, groups-specific `m_groupsLoadedAt`
timestamp (`PVRDispatcharr.h`) for the three group-related callbacks and
the existing `m_channelsLoadedAt` for the two channel-related ones --
kept deliberately separate rather than reusing one flag for both, since
groups are best-effort in `EnsureChannelsLoaded()` (a channels-only
success still advances `m_channelsLoadedAt` on a cycle where the groups
fetch itself fails, especially the very first one, when there's no
previously-known-good groups list to fall back to either) and checking
`m_channelsLoadedAt` alone for the groups callbacks would miss exactly
that case. `GetChannelGroupMembers()` checks only `m_groupsLoadedAt`,
not both: `EnsureChannelsLoaded()` never even attempts the groups fetch
unless the channels fetch already succeeded that cycle, so
`m_groupsLoadedAt` advancing already implies channels loaded too.
The same pass also independently re-verified (not just assumed
equivalent from the channels case) and fixed the identical bug in
`GetTimersAmount()`/`GetTimers()`, gated on `m_recordingsCachedAt`/
`m_timerRulesCachedAt` (both already used the same zero-`time_point`
"never loaded" sentinel, `EnsureRecordingsLoaded()`/
`EnsureTimerRulesLoaded()`'s own comments) -- confirmed against Kodi's
own real current SDK source that `CPVRTimers::UpdateEntries()`
(`PVRTimers.cpp`) deletes a client-owned timer missing from a just-
returned list via the exact same `failedClients`-membership check as
`CPVRChannelGroup::HasValidDataForClient()`, so every real series rule,
recurring rule, and pending one-time recording could vanish from Kodi's
own Timers list, not just its EPG-linked display. Deliberately did NOT
extend the same fix to `GetRecordingsAmount()`/`GetRecordings()` --
confirmed against Kodi's own real current SDK source that it would have
no effect there even if added: `CPVRRecordings::UpdateFromClients()`
(`PVRRecordings.cpp`) makes two back-to-back `ForClients()` calls into
this addon (`GetRecordings(false, ...)` then `GetRecordings(true, ...)`,
the `deleted` branch), sharing one `failedClients` vector that
`ForClients()` itself unconditionally clears on entry to each call, so
the second call always wins regardless of what either returns -- unlike
`CPVRTimers::Update()`, which also makes two such calls
(`UpdateTimerTypes()` then `GetTimers()`) but where the *last* one is
what survives, and this addon's own `GetTimers()` is that last call --
the reason the fix helps there but not for `GetRecordings()`.
**Correction (2026-09-27, a 50th-pass audit): the reasoning above for
*why* the second call wins was itself wrong, found via a project-wide
review, confirmed against Kodi's own real current SDK source, not
itself independently reproduced.** This entry originally said the
`deleted` branch "always returns `PVR_ERROR_NO_ERROR` unconditionally",
implying that return value is what lets the second call wipe out a real
failure the first recorded. That branch's own return value is
irrelevant, because it never runs at all:
`CPVRClient::GetRecordingsAmount()`/`GetRecordings()` (`PVRClient.cpp`)
gate the `deleted=true` call on
`m_clientCapabilities.SupportsRecordingsUndelete()` -- never declared
true anywhere in this addon -- via `DoAddonCall()`'s own
`bIsImplemented` parameter, which short-circuits to
`PVR_ERROR_NOT_IMPLEMENTED` *before* ever calling into this addon's own
function for that call. `ForClients()` also explicitly excludes
`PVR_ERROR_NOT_IMPLEMENTED` from counting as a failure at all
(`currentError != PVR_ERROR_NO_ERROR && currentError !=
PVR_ERROR_NOT_IMPLEMENTED`) -- so even setting aside that this addon's
own `deleted`-branch code is unreachable, returning anything else from
it wouldn't change what counts as a failure either. The conclusion
(no effect from adding the guard here) still holds, just for the
simpler, more fundamental reason already given above (the
unconditional `clear()` on entry to each `ForClients()` call). See each
function's own comment (`PVRDispatcharr.cpp`) for the full account.
That same `GetTimersAmount()`/`GetTimers()` fix introduced its own real,
confirmed regression, caught and corrected one pass later (a 41st-pass
audit, 2026-09-27, confirmed against Dispatcharr's own real current
upstream source): both functions gated on a single, shared
`m_timerRulesCachedAt` ever having advanced, but that one timestamp only
ever advanced when *both* the series-rules fetch and the recurring-rules
fetch succeeded in the same cycle (`EnsureTimerRulesLoaded()`'s own
long-standing "don't cache a partial result" design) -- and the two are
gated by different Dispatcharr-side permission classes:
`SeriesRulesAPIView.get_permissions()`'s GET only needs `IsDVRViewer`
(view or manage), but `RecurringRecordingRuleViewSet.get_permissions()`
needs `IsAdminOrDVRManager` for every method, including GET. A Standard
account with no `custom_properties.dvr_access` set at all defaults to
`"view"` (`apps/channels/dvr_access.py`'s own `get_dvr_access()`
docstring: "Absent or unrecognized: view (opt-out via explicit none)"),
so such an account's `GetRecurringRules()` call gets a permanent 403,
never a transient failure that might eventually clear. Before the
pass-40 fix this was harmless (recurring rules just stayed an empty
list forever, series rules/one-time recordings still displayed fine),
but the pass-40 fix's own never-loaded guard made this account's
*entire* Timers list -- one-time recordings and series rules included --
return `PVR_ERROR_SERVER_ERROR` forever instead. Fixed by splitting
`m_timerRulesCachedAt` into two independent timestamps,
`m_seriesRulesCachedAt`/`m_recurringRulesCachedAt` (`PVRDispatcharr.h`),
each committed independently based on its own fetch's own success --
`GetTimersAmount()`/`GetTimers()` now gate only on
`m_seriesRulesCachedAt` (recurring rules stay best-effort, exactly as
before pass 40), matching the same channels-vs-groups split
`m_groupsLoadedAt` already established one pass earlier.
`GetEPGForChannel()` also gained the same `HasNeverLoadedSuccessfully()`
guard (added 2026-09-27, a 43rd-pass audit, fixing a real, confirmed
bug found via a project-wide review, confirmed against Kodi's own real
current SDK source, not itself independently reproduced): it used to
return `PVR_ERROR_NO_ERROR` with zero tags whenever channels or EPG had
never successfully loaded even once, telling Kodi's own
`CPVREpg::Update()` (`Epg.cpp`) this was a genuinely completed, empty
scan rather than a transient startup gap -- `UpdateFromScraper()`'s own
`client->GetEPGForChannel(...) == PVR_ERROR_NO_ERROR` check short-
circuits `Update()`'s own `UpdateEntries()` call (which sets/persists
`m_lastScanTime`) only on an *error* return, so a success-with-nothing
result made Kodi wait its full advanced-settings
`m_iEpgUpdateEmptyTagsInterval` (2 hours by default) before asking
again for that channel, rather than the much shorter
`m_iEpgUpdateCheckInterval` (5 minutes) an error return actually
triggers. `CPVREpg::IsValid()` never looks at scan success/failure (only
the channel's own client id), so returning an error here doesn't risk
the EPG table itself being torn down. Deliberately checked before the
per-channel/per-EPG-entry lookups, not folded into their own existing
`!ch`/cache-miss "nothing to report" cases -- a genuinely unknown
channel, or a real channel with no guide data once channels/EPG have
actually loaded at least once, should stay a plain empty success, not
force Kodi to retry every 5 minutes forever for a channel that will
never have guide data regardless.
`Staleness` also gained `ShouldUseShortChannelEpgRefreshWait()` (added
2026-09-27, a 43rd-pass audit, fixing a real, confirmed gap found via a
project-wide review, not itself independently reproduced):
`PVRDispatcharr::StartChannelEpgRefreshThread()`'s own background loop
used one fixed sleep interval (`kChannelEpgRefreshCheckMinutes`, 10
minutes) between attempts regardless of whether channels/EPG had ever
successfully loaded, so a first-ever load failure right at addon
startup (Dispatcharr not yet reachable -- a slower-booting server than
the Kodi device itself, or a VPN/network path that comes up after Kodi
does) left the *next* attempt a full 10 minutes away, even though
`ShouldFetchChannels()`'s own 1-minute failure-retry gate
(`kChannelEpgFailureRetryMinutes`) would otherwise have allowed a retry
much sooner -- true whenever channels or EPG have never successfully
loaded even once, in which case the loop now sleeps for
`kChannelEpgFailureRetryMinutes` instead. Combined with the
`GetEPGForChannel()` fix above (Kodi's own per-channel EPG retry
cadence also recovers much sooner once it can no longer mistake a
startup gap for a completed scan), this closes most of a real,
confirmed startup-unplayable window: previously, a device that boots
before its Dispatcharr server (or whose network path comes up after
Kodi does) could leave every channel showing but unplayable for up to
the full 10-minute (or, without the `GetEPGForChannel()` fix, up to
2-hour) window even after Dispatcharr became reachable again.
This same fix reopened a real regression risk in `kForceChannelRefreshMinAgeMinutes`'s
own throttle (`PVRDispatcharr.h`, caught and corrected a pass later, a
45th-pass audit, 2026-09-27, not itself independently reproduced): that
constant's original 2-minute margin was chosen specifically to absorb a
background-thread loop that only iterated once every 10 minutes at the
time -- once this fix shrank that loop's own wait to 1 minute for as
long as channels/EPG have *never* successfully loaded (not just briefly
at startup, but indefinitely for a fetch that's been failing since its
very first attempt, e.g. a Network Access 403 present from initial
install), a 2-minute margin no longer absorbed the loop's new cadence at
all -- `EnsureEpgLoaded()`'s own forced channel+groups refetch (and both
`Trigger*Update()` calls) fired again roughly every 2-3 minutes,
indefinitely, amplified further by `GetEPGForChannel()`'s own
now-every-5-minutes-per-channel retry (up from every 2 hours) each also
calling `EnsureEpgLoaded()`. Raised to 10 minutes, restoring the same
amplification rate a persistent failure had before this fix shipped --
see `kForceChannelRefreshMinAgeMinutes`'s own comment for the full
account, and `docs/OPEN_ITEMS.md`'s already-logged "no backoff on a
persistent EPG failure" entry, which this interacts with but does not
fully resolve.
The never-loaded and startup-recovery fixes above (`HasNeverLoadedSuccessfully()`, the
`PVR_ERROR_SERVER_ERROR` returns, `ShouldUseShortChannelEpgRefreshWait()`) were
**confirmed live 2026-09-30**, previously source-only: with Dispatcharr unreachable at
Kodi startup (the addon pointed at a dead port), Kodi kept all of its channels and groups
and the guide data -- its TV database on disk had identical row counts afterward -- and
the addon retried on a steady 1-minute cadence with no escalation; once a forwarder made
the server reachable it recovered in 64 seconds and everything (channels, groups, timers,
recordings, guide) came back. Timers and recordings read empty during the outage, which is
expected: Kodi does not persist them across a restart, it re-fetches them from the addon.
`Staleness` also gained `ShouldCountTowardEpgFailureBackoff()` and
`ComputeEpgFailureRetryInterval()` (added 2026-09-30, fixing a real gap -- see
`docs/OPEN_ITEMS.md`'s entry on the `/output/epg` retry): a *durable* guide-fetch
rejection (any 4xx but 408, chiefly the Network Access 403, which Dispatcharr logs as
an `epg_blocked` system event every time) doubles the retry interval from one minute
to a 30-minute cap, while an outage (no response, 5xx, 408) stays on the flat
one-minute retry -- the same durable-versus-transient split
`ShouldCountTowardLoginBackoff()` makes for `Login()`. `EnsureEpgLoaded()` and
`EnsureChannelsLoaded()` also serialize their fetch-and-commit on a mutex each, so two
callers that both see a stale cache (at startup, the background thread and Kodi's EPG
thread) fetch once; that part is threading, not pure logic, and was confirmed live (one
`/output/epg` request at startup) rather than unit-tested. Confirmed live too: a 403 from
a proxy produced attempts at 0, 60, 180, 420 and 907 seconds.
`StreamPropertyUtil` is `BuildLiveChannelStreamProperties()` (the pure
field-mapping core of `GetChannelStreamProperties()`'s own live-channel
stream-property decision) and, added 2026-09-26 (a 24th-pass audit),
`BuildCatchupStreamProperties()`: the same pattern applied to
`GetEPGTagStreamProperties()`'s own catch-up branch -- always a plain
streamurl+isrealtimestream=false+mimetype, plus (only when
`enable_catchup_ffmpegdirect_seek` is on) `inputstream.ffmpegdirect`
with its own `is_realtime_stream=false`. Extracted specifically to lock
in this exact property set with tests: this branch has the richest
tried-and-reverted history in `PVRDispatcharr.cpp` (setting
`inputstream.ffmpegdirect.stream_mode`, forcing `open_mode=ffmpeg`, and
`PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE` were each tried and reverted
after live testing showed each made catch-up seeking measurably worse,
not better -- see `GetEPGTagStreamProperties()`'s own comment for the
full account), so a test pinning down exactly what this returns (and,
just as importantly, what it deliberately never sets) guards against a
future change silently reintroducing one of those dead ends.
`AuthBackoff` is `ComputeLoginBackoffSeconds()`, the pure exponential-
backoff-with-cap arithmetic behind `DispatcharrClient::EnsureAuthenticated()`'s
Login()-failure handling -- a real user-reported incident (found
2026-09-17): `EnsureAuthenticated()` previously retried `Login()`
unconditionally on every call, including from the realtime-update
WebSocket reconnect loop, so wrong or role-incompatible credentials
(confirmed: a Dispatcharr "Streamer"-role account, see
`docs/API_NOTES.md`) meant every periodic refresh and every reconnect
cycle re-POSTed to `/api/accounts/token/` indefinitely -- a real,
reported way to trip Dispatcharr's own login rate-limiter. Takes just
`consecutiveFailures` as a plain `int` rather than reading
`DispatcharrClient`'s own failure-count member directly, so the
growth/cap curve itself is unit-testable with no auth-mutex/HTTP
dependency; `EnsureAuthenticated()` still owns tracking that count and
resetting it on a successful `Login()`.
`AuthBackoff` also holds `ComputeNextReconnectBackoffSeconds()`, the
realtime-update WebSocket reconnect loop's own separate, much shorter
backoff scheme (2s doubling to a 60s cap), and `WasSessionHealthy()`
(added 2026-09-26, fixing a real reconnect-storm risk found via a
project-wide review, not reproduced live): whether a session that just
ended lasted long enough (`kMinHealthySessionSeconds`, 30s) to trust the
connection as genuinely healthy and reset backoff to the floor, rather
than resetting the instant `Connect()` itself succeeded -- which meant a
handshake that succeeds but drops again almost immediately (a
misbehaving reverse proxy, an overloaded server dropping connections
right after accepting them) reconnected every
`kInitialReconnectBackoffSeconds` forever instead of ever actually
backing off.
`AuthBackoff` also gained `ShouldCountTowardLoginBackoff()` (added
2026-09-27, a 43rd-pass audit, fixing a real, confirmed gap found via a
project-wide review, confirmed against Dispatcharr's own real current
upstream source, not itself independently reproduced):
`EnsureAuthenticated()` used to count every `Login()` failure toward
`ComputeLoginBackoffSeconds()`'s own escalating backoff the same way,
regardless of *why* it failed -- but that backoff's own documented
purpose (see its own comment) is specifically a durable,
retrying-sooner-won't-help failure (wrong or role-incompatible
credentials), not a transient one. A curl transport failure (no
response received at all) or a 5xx (Dispatcharr itself unreachable,
still starting up, or briefly erroring) is exactly the transient case:
without this distinction, an extended outage escalated the same backoff
meant for a credentials lockout, so `EnsureAuthenticated()` could still
refuse to even attempt `Login()` again for up to `kMaxSeconds` (30
minutes) after Dispatcharr had already recovered. Only 400/401/403/429
now count -- confirmed against `TokenObtainPairView.post()`
(`apps/accounts/api_views.py`): 403 for its own
`network_access_allowed()` policy block or a re-raised SimpleJWT
validation error DRF maps to 400, 401 for SimpleJWT's own wrong-
credentials `AuthenticationFailed`, and 429 from its own
`LoginRateThrottle` -- the exact rate-limiter this whole backoff exists
to avoid tripping, so a 429 is if anything a *stronger* signal to back
off, not a reason to treat it as transient. `Request()` gained an
optional `httpStatusOut` out-parameter (left null at every other call
site, so nothing else is affected) so `Login()` -- and, through it,
`EnsureAuthenticated()` -- can read the real HTTP status back out
(`0` for a transport failure, a curl-level sentinel no real HTTP
response ever produces) without needing to string-parse it back out of
`Request()`'s own error message.
That same `ShouldCountTowardLoginBackoff()` fix reopened a real,
confirmed regression it didn't have before (found via a project-wide
review one pass later, a 46th-pass audit, 2026-09-27, not itself
independently reproduced -- how much it hurts in practice depends on
whether an outage times out slowly rather than fails fast, so this
needs a live test against a blackholed host to fully confirm): a
transport failure/5xx correctly stopped escalating the same exponential
backoff a genuine credential rejection does, but was left with *no*
backoff at all -- `m_consecutiveLoginFailures` stays 0, so
`EnsureAuthenticated()`'s own gate never engages, so every call from
every thread (the background channel/EPG thread, now waking every
minute for a never-loaded state per the 43rd-pass fix; the recording-
refresh thread; the realtime-update WebSocket reconnect loop; and
Kodi's own per-channel `GetEPGForChannel()` sweep, now retrying every 5
minutes per that same fix) re-attempts a full blocking `Login()` --
holding `m_authMutex`, the same mutex `Request()`'s own token-copy and
`RefreshAccessToken()` also use, for the call's entire duration (up to
`timeoutSeconds`, 30s default) -- for as long as a genuine network
outage (a blackholed host, a firewall `DROP` causing a slow TCP-level
timeout rather than a fast refused/RST) lasts. Fixed with a new, short,
fixed, non-escalating cooldown (`m_transientLoginFailedAt`,
`kTransientLoginRetrySeconds` = 30s, matching
`ComputeLoginBackoffSeconds()`'s own first-failure wait but never
escalating past it) set on exactly the failures
`ShouldCountTowardLoginBackoff()` says shouldn't count toward the real
backoff -- checked the same way as `m_channelsLastFailedAt`/
`dispatcharr::IsRetryDue()` elsewhere in this codebase. While fixing
this, caught and fixed the same stale-failure-timestamp mistake in this
exact function before it ever shipped, one that `EnsureChannelsLoaded()`/
`EnsureEpgLoaded()` already had (see `docs/EPG.md`'s own entry on that
fix for the full account): `m_transientLoginFailedAt`/`m_loginBackoffUntil`
were about to be stamped from the same stale, pre-`Login()`-call `now`.
That same fix left one path completely unthrottled, caught and fixed
the very next pass (a 47th-pass audit, 2026-09-27, confirmed by code
trace, not itself independently reproduced): `EnsureAuthenticated()`
tried `RefreshAccessToken()` *before* either backoff gate, and
`RefreshAccessToken()` never clears `m_refreshToken` on its own failure
-- so once this addon has logged in successfully even once (covering
any outage that starts mid-session, as opposed to one right at addon
startup before any login has ever succeeded), every later
`EnsureAuthenticated()` call tried a full blocking `RefreshAccessToken()`
call -- holding the same `m_authMutex` -- completely bypassing both
gates. That made a mid-session outage the *more* common way to hit the
exact thundering-herd/serialization storm these gates exist to prevent,
not a narrow corner case of it. Fixed by moving both gates to run
before the refresh-token attempt too, and giving `RefreshAccessToken()`
its own `httpStatusOut` (matching `Login()`'s own pattern) so its
failure can be classified the same way: a 401/403 (the refresh token
itself genuinely rejected, not an outage) clears `m_refreshToken` so
it isn't retried forever and falls through to `Login()` for a fresh
pair of tokens; a transport failure/5xx sets the same transient cooldown
`Login()`'s own failures do and stops there, rather than also attempting
a second full blocking call against a host that just failed to answer
the first one.
`WebSocketHandshake` is `IsWebSocketHandshakeAccepted()`, the plain
HTTP-101-response-header check `WebSocketClient`'s own connect path
uses to confirm the server actually upgraded the connection rather than
just returning an ordinary HTTP response -- extracted the same way as
`WebSocketFrame`'s own frame-level helpers, just for the handshake
response instead of a data frame. (Already extracted and tested before
this list was last revised; only the doc entry itself was missing.)
Also holds `BuildWebSocketHandshakeRequest()` (added 2026-09-26): the
RFC 6455 handshake request itself, taking the already-generated
base64-encoded `Sec-WebSocket-Key` nonce as an explicit parameter (same
convention as `WebSocketFrame.h`'s own mask-key parameter) so its exact
byte layout is testable against a known key. Doesn't validate the
peer's own `Sec-WebSocket-Accept` response header against that key --
see `docs/OPEN_ITEMS.md`'s entry on that known spec-compliance gap.
`WebSocketHandshake` also holds `ComputeWebSocketAccept()`,
`FindWebSocketAcceptHeader()` and `IsWebSocketHandshakeAcceptedForKey()` (added
2026-10-01, fixing a gap RFC 6455 section 4.1 requires a client to close, reproduced
live: a proxy rewriting `Sec-WebSocket-Accept` still made the old build report
"connected"): an exact, case-sensitive comparison against base64(SHA-1(key + the
RFC's GUID)). `Sha1` is the small dependency-free SHA-1 behind it, tested against the
FIPS vectors, a million `a`s and every padding-boundary length against a reference.
`CurlCallbacks` also gained `BoundedStringSink`/`BoundedWriteCallback` and the three
per-call-site ceilings (API JSON 128 MiB, XMLTV 512 MiB, in-progress playlist 64
MiB, each chosen from a size measured live) -- an unbounded `WriteCallback` ended a
runaway response in `std::bad_alloc` thrown inside a C callback, so an overflow now
fails the transfer with an error naming the limit; the size multiplication is
checked before it wraps. `RedirectPolicy` is `ParseUrlOrigin()`/`IsSafeRedirectTarget()`,
the per-hop policy of `DispatcharrClient::PerformWithSafeRedirects()` (same host,
never https to http, no userinfo, absolute http(s) only; a different port is fine),
which replaced `CURLOPT_FOLLOWLOCATION` on every request that carries credentials,
because libcurl forwards an `X-API-Key` header and a 307/308 POST body to any host
(reproduced live: a login password re-sent to a different host). `Request()`'s 401 path
now goes through `InvalidateAccessTokenIfCurrent()` and `EnsureAuthenticated()`
instead of calling `RefreshAccessToken()`/`Login()` directly (11 logins and 11
refreshes in 170s became 3 and 1 against a proxy that answers every call 401).
All four confirmed live; see `docs/API_NOTES.md`.
`timeshift_buffer` 0.6.7's compare-and-set state writes
(`_update_buffer_state()`, `_cas_buffer_state()`) and orphaned-thread sweep
(`_stop_orphaned_threads()`) are unit-tested against a fake Redis that loses races
on demand and against two real imports of the plugin file; their live confirmation
on a real Dispatcharr was done 2026-10-01 (listener count 7/10/13/16 on 0.6.6, flat at 4 on 0.6.7;
three injected races lost on 0.6.6, held on 0.6.7).
`UnprobeableSegment` is `ShouldProbeOnlyLeadingSegment()`, `UpdateUnprobeableSegmentTracker()` and
`CountMergeableSegments()` (added 2026-10-01, fixing a real bug reproduced live -- see `docs/OPEN_ITEMS.md`'s entry and
`docs/RECORDINGS.md`'s "Two live checks of the in-progress read path"): what `RefreshInProgressRecordingManifest()` does about a playlist
entry it cannot size. One such segment used to stop playback there for good, flood the server (~14 HEADs a second, the whole unmerged tail
re-probed every refresh and the sizes thrown away), run a blocking read's full 200-255 s catch-up budget, and hold the finished recording
open indefinitely, since every probe refreshes Dispatcharr's viewer key. Now a leading segment that failed last refresh is probed alone, and after five
failed refreshes spanning 30 s it is merged as a zero-byte placeholder (offsets stay exact; a few seconds of picture are lost) with one logged
warning. Reproduced by rewriting the playlist's absolute segment URLs through an HTTP-aware proxy (they bypass a plain forwarder), re-verified
against the same fault: 47 probes of the one segment in 30 s, playback to the real end, the recording finalized. `LiveEdgeMargin` also gained
`ClampSeekToTail()` (same day, the other open item from those checks): a seek past the tail target lands on it, or on the current position when the reader
is already past it, so a forward seek at the tail never moves backward (live: 262,144 to 1,310,720 bytes back before, 0 in ten of ten after).
`tools/tests/test_dependency_pins.py` (added 2026-10-02, closing three `docs/OPEN_ITEMS.md` entries on mutable pins): fails if any workflow `uses:` is
not `@<commit SHA>` with a `# vX.Y.Z` comment (moved together by hand now; Dependabot is not used), any FetchContent `GIT_TAG` is not a commit SHA, or the Windows job's
SHA256-pinned prebuilt archives (curl/OpenSSL/zlib, downloaded and hash-checked before being handed to Kodi's `add_internal()` as a local file) differ from `docs/BUILDING.md`'s copy.
`timeshift_buffer` 0.6.8 serializes `_ensure_http_server_running()`/`_ensure_reaper_running()` under locks (tested with eight simultaneous first calls, and that the tests fail without the locks).
`RecurringRuleEdit` is `ComputeRecurringRuleEditPatch()`, `RecurringRuleEditPatch` and `ComputeEndDateForOpenEndedRuleEdit()` (added 2026-10-02, fixing
two real bugs, both confirmed live -- see `docs/OPEN_ITEMS.md` and `docs/RECURRING_RULES.md`): a recurring-rule edit sends only the fields the user changed, diffed
against `m_reportedRecurringRules` (the rules as `GetTimers()` last handed them to Kodi -- NOT the cache, which is refreshed between Kodi filling its dialog and the user
saving; the first version diffed against the cache and reverted a change made elsewhere, caught live), a no-change save sends nothing, and a rule with no `end_date` is given
a rolling-window one because the server refuses any save of it otherwise. `BuildRecurringRuleUpdateBody()` now takes the patch (end_date only when it carries one). The diff compares times of day modulo one day and `ComputeRecurringRuleFields()` wraps its results into [0, 86400) (2026-10-03, see `docs/CLOSED_ITEMS.md`'s "Unchanged recurring-rule times were re-sent on every edit for a rule near UTC midnight"): the Kodi-derived side used to arrive unwrapped (-7200 for a 22:00 rule behind UTC, 108000 for a 06:00 rule far enough ahead) against the server's wrapped value, so an unchanged rule of that shape was re-sent -- and its occurrences regenerated -- on every edit (reproduced and re-verified live through Kodi's own timer dialog).
`TimerIdentity` also gained `ShouldReplaceSeriesRuleOnEdit()`: a series-rule edit that changes title or `tvg_id` creates the new rule first and deletes the old only after
that succeeds (source-only changes are excluded on purpose, see its comment). `DvrAccess` is `ComputeDvrAccess()`/`ParseDvrAccessFromUserJson()`/`CanManageDvr()`, mirroring the
server's `get_dvr_access()`; `GetCapabilities()` advertises timers/recording delete/rename only for an account that can manage (a view-only account used to be offered actions
that only failed; confirmed live, `supportstimers: false`). `RecordingVisibility` gained `DecideDeleteTimerAction()`: a failed server lookup is no longer guessed past -- 404 is
already-gone, `forceDelete` keeps Stop, anything else is refused (confirmed live), and it can never reach Delete unverified (tested exhaustively). `RecordingEpgLinks` remembers the
EPG programme start each recording was matched to (the broadcast id is `ComputeBroadcastId(channel, programme start)`), persisted as `recording_epg_links.json`, so a finished
recording's guide link survives the guide dropping the ended programme (file persistence confirmed live; tolerant parse, validated against channel and recording start, pruned, capped).
`M3u8SegmentParser` gained `RebaseRecordingSegmentUrl()`: segment URLs of the recording's own HLS path are pointed at the configured address instead of whatever address Dispatcharr
named (a reverse proxy that forwards no port made it the internal one) -- confirmed live, every segment request went through the configured address; other URLs are never rewritten,
so a playlist that names an address of the server's own ends up at the configured one (a segment URL naming a different host is no longer sent the key either, fixed 2026-10-04, see `docs/OPEN_ITEMS.md`). `EnsureEpgLoaded()` defers when the forced channel refresh it follows did not happen (not unit-tested -- member state --
and not exercised live), and `OpenLiveStream()` no longer re-checks the timeshift mode a settings save could flip between Kodi's two callbacks.
`EpgTagUtil` also gained `ComputeGuidePrevDays()` (added 2026-10-02): the guide fetch sends `?prev_days=N`, N the longest catch-up window among channels that offer catch-up (capped at the server's 30, none when no channel does), so catch-up works on a fresh install or guide reset instead of only for what Kodi cached before a programme aired. Measured: +25% guide traffic for any N >= 1, and 3/7/30 identical because the source only holds ~2 days of history; confirmed live that the request carries it.
`ManagedRecurringRule` (added 2026-10-02, fixing two real bugs -- see `docs/OPEN_ITEMS.md` and `docs/RECURRING_RULES.md`): which recurring rules the addon owns and may renew. Ownership is a `[Kodi]` tag at the end of the rule's stored name (`IsManagedRuleName()`/`AddManagedRuleMarker()`/`StripManagedRuleMarker()`; a rule has no free-form field), Kodi is shown the name without it,
`ParseRecordingFields()` strips it from the titles of the occurrences a rule generates, and `ShouldRenewRecurringRule()` renews only an owned rule -- reviving an owned, enabled one that has run past its end date, which used to stay dead because the addon could not tell it from a user's finished rule. `EvaluateInitialAdoption()`/`RecurringRuleAdoptionState` do the one-time tagging of rules that predate this (state persisted in
`recurring_rule_adoption.json`, resumable across restarts, skipping a rule with a recording starting), and `HasActiveOrImminentOccurrence()` is the occurrence-safety check shared with renewal. Confirmed live against the real instance, including a rename through Kodi's edit dialog keeping the tag, and (2026-10-03) creation from Kodi's Add timer dialog: a rule typed in the dialog was stored at exactly the typed times with the tag, every day and a 30-day window, and Kodi listed it with its 31 occurrences (see `docs/CLOSED_ITEMS.md`'s "Unchanged recurring-rule times were re-sent on every edit for a rule near UTC midnight").
`RecordingHttpUtil` also gained `ServerIgnoredRangeRequest()` (added 2026-10-02): a plain 200 to a ranged read past offset 0 means the server dropped `Range`; the recording reader ends the stream (EOF, once, notified) and the live-timeshift reader goes fatal instead of splicing the wrong bytes in (confirmed live behind a Range-dropping proxy). `CurlCallbacks` gained `TransferAbortResult()`: every request carries a transfer-progress callback so
`DispatcharrClient::AbortInFlightRequests()` -- called first in `~PVRDispatcharr()` -- ends them at once, and the recording-refresh thread no longer holds its mutex across its network calls, which together took Kodi's exit with a request hung from 27.1 s to 2.1 s (live) and made the `notify_all()`-under-the-mutex fix safe. The guide fetch, the live segment fetch and the heartbeat POST had hand-built handles that missed that callback (fixed 2026-10-03), and -- the larger half, found when fixing them changed nothing live -- `GetEPGForChannel()` used to wait in `EnsureEpgLoaded()` on a guide fetch in progress (or run it) on Kodi's own PVR manager and EPG threads, which Kodi stops before it destroys the instance, so the destructor's abort flag was never reached while a guide download stalled (108.7 s to exit before, 3.7 s after). Only the background thread fetches the guide now; `GetEPGForChannel()` calls `RequestGuideFetchIfWanted()`, gated by `dispatcharr::ShouldAttemptGuideFetch()` (`Staleness.h`, unit-tested), and answers from the cache -- see `docs/CLOSED_ITEMS.md`'s "Kodi's own threads waited on the guide fetch, and two transfers could not be aborted at shutdown". `Staleness` gained `IsFetchStillCurrent()`: per-cache generation counters (bumped by the invalidation triggers and a channel renumbering)
stop a fetch that was already running from committing its pre-change result as fresh (unit-tested helper; not exercised live). `timeshift_buffer` 0.6.9 records the ffmpeg's start time (`pid_start_ticks`) and refuses to signal, or call alive, a pid whose start time changed -- a recycled pid is not our process (unit-tested, and checked against a real `/proc`).
`Staleness` also gained `kGuideRefetchAfterRenumber`, `IsGuideRefetchPending()` and `IsGuideRefetchDue()` (added 2026-10-02, fixing a gap confirmed live -- see `docs/EPG.md`): a detected channel renumbering schedules one more guide fetch 330 s later, past Dispatcharr's 300 s guide cache that a manual renumber does not invalidate (a fetch 70 s after one still listed the old number; the follow-up listed the new one). The scheduling and clearing live in `PVRDispatcharr` (`m_epgRefetchDueAt`) and were confirmed live, not unit-tested.
`SegmentFetchFailure` also gained a duration to `ShouldGiveUpAfterSegmentFetchFailure()`, `SegmentFetchRetryDelayMs()`, `IsTransientReadFailure()` and `ShouldKeepRetryingTransientRead()` (added 2026-10-02, fixing a real bug found by the manual-testing pass and confirmed live -- see `docs/CLOSED_ITEMS.md`'s "A brief server outage ended playback for good" and `docs/MANUAL_TESTING.md`): four refused segment fetches in about two seconds used to mark a live stream fatal for good, and a completed or in-progress recording's read returned -1 on the first transport failure (ending playback once Kodi's read-ahead drained, about 8 s into an outage). A live give-up now needs the failure streak to have lasted 30 s as well as reached four, with the retry delay doubling from 250 ms to 2 s; a recording read retries an unreachable server or a 502/503/504 for up to 20 s inside the one `Read()`. A manifest refresh that positively reports the buffer gone still ends the stream at once. The live retry happens *inside* the one `Read()` call (`ReadLiveTimeshiftStream()` wraps `ReadLiveTimeshiftStreamOnce()`, 25 s budget): returning zero-byte reads to Kodi instead ended playback after about five seconds when nothing was buffered ahead. The decisions are unit-tested, the loops are glue.
`DispatcharrClient`'s auth state has two plain mutexes (2026-10-02, `docs/CLOSED_ITEMS.md`'s "m_authMutex held across a network round trip"): `m_authStateMutex` for the token fields and failure state, never held across a network call, and `m_authFlowMutex` held across the login/refresh calls by a thread that has to authenticate. `EnsureAuthenticated()` answers a valid cached token with the state lock alone, so reading the token no longer waits out another thread's login (29.7 s before, 0 ms after, with a hung server); lock order is flow, then state. Threading glue, confirmed live, not unit-tested.
`ApiKeyRecovery` also gained `ClassifyApiKeyDelivery()` and `DecideApiKeyStoreAction()` (added 2026-10-02, see `docs/CLOSED_ITEMS.md`'s "SetSetting*() swallowed while the settings dialog is open"): while the addon's own settings dialog is open Kodi swallows the addon's `api_key` write, and the old key it still stores later comes back through `OnAddonSettingChanged()` looking like a user edit and restarted the instance. `PersistApiKeyIfChanged()` reads the write back and remembers the stale value; a delivery equal to it is ignored and `ReassertApiKeySetting()` (background thread only) writes the real key again. Confirmed live with the dialog opened the way a user opens it.
`timeshift_buffer` 0.8.0 (2026-10-02, see `docs/TIMESHIFT.md`'s "1.0.6 follow-up" section and `docs/CLOSED_ITEMS.md`'s "Non-fatal Packet corrupt on server-side live timeshift"): the buffer's ffmpeg now uses the `hls` muxer (`-hls_time`, `-hls_list_size`, `-hls_delete_threshold` = the old 2x window's extra half, `-hls_flags delete_segments+omit_endlist`) instead of the `segment` muxer, because the `segment` muxer restarts every PID's MPEG-TS continuity counter in each file and the addon splices the files into one byte stream: one demuxer `Packet corrupt` per splice (measured 14 of 14; 82 lines against 0 in a scripted Kodi run before and after). Same `seg_%05d.ts` names and playlist shape, so the manifest parser and the addon are unchanged; old files are deleted rather than renamed over. `_build_ffmpeg_command()`'s tests include a real-ffmpeg check of the counters across files (skipped without ffmpeg).
`DispatcharrClient` stream locking (added 2026-10-02, see `docs/CLOSED_ITEMS.md`'s "No locking around live-timeshift / in-progress stream state"; threading glue, confirmed live rather than unit-tested): each growing stream has a state mutex (never held across a network call, a sleep or another locking call), a refresh mutex (one manifest refresh at a time; an unforced caller that finds one running returns) and a curl mutex (held while the persistent handle or cached segment bytes are in use; `Close` takes it first), plus a session counter that makes a function which released the state lock around a network call discard its result if the stream was closed or reopened. The rules and lock order are in the comment at `m_liveStateMutex` in `DispatcharrClient.h`; keep to them when touching a read, seek, refresh, open or close path. A second thread forcing refreshes duplicated a segment in the in-progress byte stream before this (20 times in 150 s) and never after. On the Linux Kodi 21 client every stream callback ran on one thread (measured), so the race is a platform-contract guard there, not an observed Kodi behaviour.
`PVRDispatcharr::m_recordingsEverLoaded`/`m_seriesRulesEverLoaded` (2026-10-02, see `docs/CLOSED_ITEMS.md`'s "GetTimers reported a server error right after a timer change"): what `GetTimers()`/`GetTimersAmount()`'s never-loaded guard reads, instead of a zero cached-at timestamp, because invalidation and a fetch discarded as out of date both leave that timestamp zero while the data is held -- which made Kodi log a spurious server error around every timer change. The timestamps remain freshness only.
`SocketWait` is `WaitForSocketReady()` (added 2026-10-02): the socket wait behind `WebSocketClient`, `poll()` on POSIX (no 1024-descriptor limit; `select()`/`FD_SET` was undefined behaviour past it and for `CURL_SOCKET_BAD`), refusing an invalid socket up front; tested with socketpairs, including a descriptor numbered 1500. `WebSocketFrame` gained `IsValidServerFrame()` (a server never masks; a control frame is <= 125 bytes and unfragmented). `WebSocketClient` reads now run against absolute deadlines -- one per
`ReceiveTextMessage()` call and one per frame -- so a ping flood or a frame dripped in a byte at a time can no longer keep the realtime thread from returning; `tests/test_web_socket_client.cpp` exercises it against a real local server (the first test coverage of that class), and a live run confirmed the realtime push still works. `tools/check_doc_refs.py`'s heading match now accepts only a citation that is a substring of a real heading.
`LiveEdgeMargin` also gained `IsAtEndedTail()` (added 2026-10-02, with `timeshift_buffer` 0.7.0 -- see `docs/TIMESHIFT.md`'s "A dead ffmpeg no longer takes the rewind window with it"): a dead ffmpeg with a playlist on disk no longer raises `BufferFailedError` (which tore the buffer down at the first poll, wiping a paused viewer's rewind window); `get_live_manifest` returns the frozen manifest with `ended: true`, and the addon ends a read only when `ended` and the reader has consumed everything -- as EOF (0), not `-1`, which Kodi retries forever. Verified live before/after against a paused viewer (11 s of playback after resume with 0.6.9, ~72 s with 0.7.0).
`StreamSeek` is `ResolveSeekPosition()`, the `SEEK_SET`/`SEEK_CUR`/
`SEEK_END` switch and negative-result validation duplicated identically
across `SeekRecordingStream()`, `SeekInProgressRecordingStream()`, and
`SeekLiveTimeshiftStream()` -- unified into one function each of the
three now calls, taking the stream's own current position and a
reference length (the recording's own length, or `nullptr`-equivalent
`-1` when not yet known, for the `SEEK_END`-with-unknown-length guard)
as plain parameters.
`SegmentAppendOffsets` is `AppendSegmentOffsets()`, the identical
`byteOffset`/`timeOffsetMs` assignment `RefreshInProgressRecordingManifest()`
and `RefreshLiveManifest()` each performed once per newly-discovered
segment while merging it into their own running totals -- against two
differently-typed segment info structs, so this returns the new
offsets rather than writing into one directly; the caller still applies
them to its own struct and advances its own totals via the same call.
`PendingTitleLookup` is `PruneExpiredPendingTitles()`/
`FindPendingTitleForRecording()` (renamed from
`FindLatestPendingTitleForChannel()` in a 32nd-pass audit, when the
cache itself was re-keyed by recording id instead of channel id -- see
`docs/OPEN_ITEMS.md`'s own entry on the real corruption path that
fixed), the TTL-based prune and by-recording-id lookup behind
`ParseRecordingJson()`'s `PendingTitle` cache fallback (a short-lived,
best-effort bridge that fills in a recording's title from the "Record"
request that started it, before Dispatcharr's own async enrichment has
caught up) -- duck-typed the same way as `SegmentLookup`/`LiveEdgeMargin`
rather than depending
on the real, private `PendingTitle` type directly. The mutex guarding
the real cache, and the vector mutation itself, stay in
`ParseRecordingJson()` -- neither is available to a free function.
`InProgressSegmentCache` is `ShouldReseedInProgressSegmentCache()`
(added 2026-09-26, a 28th-pass audit, pure refactor extracted from an
inline check with no behavior change): the decision core of
`RefreshInProgressRecordingManifest()`'s own cross-open segment cache
(`m_inProgressSegmentCache`) update -- whether this cycle's cache entry
must be reseeded from the full current segment list rather than having
just this cycle's newly-probed segments appended to it. False (append
is safe) on every ordinary refresh cycle; true only after `finished`
briefly went true (erasing the entry) and then flipped back to false
again, the same pass-11-era gap `ResolveInProgressFinished()`'s own
non-sticky logic already documents -- appending onto the
freshly-recreated (empty) entry would otherwise silently drop every
earlier segment already known before that reset.
`CatchUpUtil` also gained `IsLikelySeekProbe()`/`ComputeShortGiveUpPosition()`
the same way, covering the "is this read likely one of ffmpeg's own
seek probes, or a genuine catch-up wait" decision (and the
`lastShortGiveUpPosition` update rule afterward) `ReadInProgressRecordingStream()`
and `ReadLiveTimeshiftStream()` each independently computed identically.
`EstimateSegmentDurationMs()` also gained an upper bound,
`kMaxSegmentDurationMs` (60 seconds, added 2026-09-26, a 39th-pass
audit, fixing a real, confirmed gap found via a project-wide review, not
itself independently reproduced): with no ceiling, one outlier segment
among the last `kSegmentDurationSampleCount` could dominate the average
and inflate `ComputeCatchUpAttempts()`'s own blocking-read budget from
seconds to potentially many hours (a lone 86400s segment -- the same
maximum `M3u8SegmentParser.cpp`/`LiveManifestParser.cpp`'s own UB/
overflow clamps still allow through -- averaged with 4 near-zero ones
turns into a roughly 14.4-hour blocking `Read()` call via the 3x-margin
formula). A genuine plausible trigger, not just a defensive guard:
Dispatcharr's own DVR ffmpeg command (confirmed against its real current
upstream source) doesn't remove a mid-stream PTS discontinuity from a
glitchy IPTV source, and the HLS muxer derives each segment's own
duration from PTS deltas.
`EpgTagUtil` also gained `ShouldIncludeEpisodeDates()`,
`CategoriesIndicateSeries()`, `JoinCategories()`, and
`ComputeEpgTagFlags()`, covering four small field-mapping decisions
`GetEPGForChannel()`'s per-entry loop made inline -- the last mirrors
`EPG_TAG_FLAG_*` as raw bit shifts the same way `MapCategoriesToGenreType()`
already mirrors the content-mask constants.
`EpgTagUtil` also gained `ShouldOfferCatchup()` (added 2026-09-28, fixing
a real, confirmed bug found live: `GetChannels()`'s `SetHasArchive()`,
`IsEPGTagPlayable()`'s own `IsWithinCatchupWindow()` call, and
`GetEPGTagStreamProperties()` all used to ignore Dispatcharr's own
instance-wide and per-user catch-up on/off switches entirely --
`apps/timeshift/api_views.py`'s catch-up endpoint 403s "Catch-up is
disabled" when either is off, independent of any per-channel
`catchupEnabled` flag, so Kodi kept offering "Play" for an already-aired
programme that was always going to fail, with nothing surfaced to the
user beyond a debug log line. Folds a channel's own `catchupEnabled`
together with two new `PVRDispatcharr`-cached flags
(`m_catchupEnabledGlobally`/`m_catchupEnabledForCurrentUser`, fetched
once at construction via `DispatcharrClient::IsCatchupEnabledGlobally()`/
`IsCatchupEnabledForCurrentUser()` -- the latter reusing the exact
`/api/accounts/users/me/` response `IsCurrentUserAdmin()` already
fetches, just also reading `custom_properties.catchup_enabled` from it).
Both cached flags default `true` (fail open) on a fetch error, matching
`dispatcharr_is_admin`'s own established reasoning -- Dispatcharr's own
403 stays the authoritative enforcement either way, this is purely
about not advertising something that's going to fail.
`GetEPGTagStreamProperties()` also gained its own early-exit check using
the same function (with a `QueueNotification` explaining why) as defense
in depth for a stale cached "is playable" decision, rather than relying
on `IsEPGTagPlayable()` alone. Re-verified live against the real lab
account, both directions: with the per-user flag disabled (and Kodi
restarted, since these flags are startup-only, matching the admin-check
sync's own convention), `PVR.GetChannelDetails` correctly reported
`hasarchive: false`, and `Player.Open` on an already-aired broadcast was
rejected by **Kodi's own core** before ever reaching this addon at all
(`Invalid params`, confirmed via `kodi.log` that neither
`IsEPGTagPlayable`'s nor `GetEpgTagStreamProperties`'s own log lines
appear for that attempt) -- a strictly better outcome than the fix's own
defense-in-depth branch even anticipated, since that branch never needed
to run. Re-enabling and restarting confirmed `hasarchive: true` and real
catch-up playback both working normally again.
`LiveEdgeMargin` also gained a third template, `ComputeLiveEdgeStartPosition()`
-- `OpenLiveTimeshiftStream()`'s own live-edge starting-position formula
(a segment-index-based backoff, distinct from `ComputeLiveEdgeTailTarget()`'s
byte-size-based one).
`EpgProgramMatch` is `FindEpgEntryIndexCoveringRecording()` (added
2026-09-26, a 32nd-pass audit, fixing a real, confirmed bug found via a
project-wide review, confirmed against both Kodi's own real current SDK
source and Dispatcharr's own real current upstream source, not itself
independently reproduced): `PVRDispatcharr::GetTimers()` never called
`SetEPGUid()` on a one-time recording's timer, so Kodi had no direct
link back to the EPG tag it was recorded from. Kodi's own fallback probe
(`CPVRTimerInfoTag::GetEpgInfoTag()`, `PVRTimerInfoTag.cpp`) then tries
`GetTagBetween(timerStart-2min, timerEnd+2min)` instead, requiring the
EPG tag's own start time to fall at or after `timerStart-2min` -- but
`rec.startTime` can already be later than the programme's own real
start: `RecordingSerializer.validate()` silently clamps a past
`start_time` to `now` on create/update. Pressing "Record" on a
programme already more than 2 minutes into its own run therefore left
Kodi with no way to link the resulting timer back to its EPG tag at
all -- the guide kept offering "Record" instead of showing a recording
indicator, and a second press created a second, duplicate `Recording`
row server-side (Dispatcharr has no overlap guard of its own).
`FindEpgEntryIndexCoveringRecording()` finds the right entry anyway,
mirroring Dispatcharr's own server-side match
(`_match_epg_program_by_timeslot()`, `apps/channels/tasks.py`): the
entry with the largest overlap against the recording's own window, but
only when that overlap covers at least 80% of the recording's own
duration (a recording spanning multiple programmes with no single
dominant one returns no match, same as Dispatcharr's own "Custom
Recording" fallback for that case). This overlap-ratio approach works
despite the same clamped `rec.startTime`, for the same reason
Dispatcharr's own server-side auto-enrichment (which populates
`custom_properties.program` from this exact same clamped `start_time`)
is unaffected by it: both the overlap and the recording's own duration
are measured from the same clamped start, so the ratio stays consistent
regardless of how late the recording actually started -- unlike Kodi's
own fixed +/-2-minute window, which isn't. `GetTimers()` calls this
against this addon's own already-cached, channel-number-keyed XMLTV
data (`m_epgByChannelNumber`) and, on a match, computes the same
`ComputeBroadcastId()` (`EpgTagUtil.h`) `GetEPGForChannel()` itself
would have assigned that exact entry, so the two stay consistent.
`EpgProgramMatch` also gained `FindEpgEntryIndexByStartTime()` (added
2026-09-26, a 34th-pass audit, fixing a real, confirmed gap in the fix
above found via a project-wide review, confirmed against Dispatcharr's
own real current upstream source, not itself independently reproduced):
the overlap-ratio approach alone still fails a *padded* recording --
Dispatcharr's own web UI Guide "Record" button and series-rule-
materialized occurrences both apply global pre/post-padding directly to
`start_time`/`end_time`, which can push the overlap ratio below 80%
well before the programme is even over (fails once post-padding alone
exceeds a quarter of the programme's own remaining runtime), especially
combined with the same past-start clamp -- the exact pass-32 symptom
again, just reached via a Dispatcharr-web-UI-created recording instead
of a Kodi-created one. `FindEpgEntryIndexByStartTime()` matches by a
recording's own true, never-padded `Recording::programStartTime`
(`RecordingParser.cpp`, parsed from `custom_properties.program.start_time`
when present -- never present for this addon's own recordings, since
`CreateOneTimeRecording()` deliberately omits `custom_properties` on
create and Dispatcharr's own later enrichment for those only ever
backfills id/title/sub_title/description, never times) within a small
tolerance, tried first; the overlap-ratio function above is the
fallback when it isn't available or doesn't match. Both are now shared
by `GetRecordings()` too (previously only `GetTimers()` linked a
still-recording timer to its EPG tag, never a finished recording to its
own) via a new `PVRDispatcharr::ResolveRecordingBroadcastId()` helper --
see docs/OPEN_ITEMS.md for the one durability gap this doesn't close
(a match depends on the EPG entry still being present in this addon's
own cached guide, which drops an already-ended entry at its own next
XMLTV refresh).
`EpgProgramMatch` also gained `ResolveEpgOverlapWindow()` (added
2026-09-26, a 36th-pass audit, fixing a real, confirmed gap in the fix
above found via a project-wide review, not itself independently
reproduced): `ResolveRecordingBroadcastId()`'s own overlap-ratio
fallback always ran on the recording's own possibly-padded/clamped
`[recStartTime, recEndTime)`, even when `FindEpgEntryIndexByStartTime()`'s
own exact match had already failed for a *known*, non-zero
`programStartTime` (which happens routinely -- the durability gap just
above) -- risking a *wrong* match, not just a missing one, for a padded
recording: with enough post-padding relative to a short programme's own
duration, the padded window's own >=80% overlap can land on the *next*
programme instead, linking this recording's timer/EPGEventId to an
unrelated show for as long as it airs.
`ResolveEpgOverlapWindow()` prefers `[programStartTime, programEndTime)`
-- the recording's own true, never-padded programme window -- whenever
both are valid, all-or-nothing (never pairing one end from the
programme window with the other from the padded/clamped recording),
falling back to `[recStartTime, recEndTime)` only when they aren't. A
recurring-rule occurrence's own `program.start_time`/`end_time` are
never separately padded in the first place -- confirmed against
Dispatcharr's own real current upstream source,
`sync_recurring_rule_impl()` (`apps/channels/tasks.py`): both are
derived from the exact same `start_dt`/`end_dt` as the recording's own
top-level `start_time`/`end_time`, via a plain `Recording.objects.create()`
that never goes through `RecordingSerializer.validate()`'s own padding
logic at all -- so preferring the programme window there is a no-op for
that case, not a behavior change.
`TimerRequestBuilder` is the pure request-body/query-string assembly for
the timer-CRUD HTTP calls in `DispatcharrClient.cpp` (added 2026-09-26, a
32nd-pass audit, pure refactor extracted from each call's own inline
JSON literal, no behavior change): `BuildSeriesRuleRequestBody()`
(`CreateSeriesRule()`'s own POST/upsert body -- omits `channel_id`/
`tvg_id` when not applicable, the 18th-pass 400 fix), `BuildRecurringRuleUpdateBody()`
(`UpdateRecurringRule()`'s own PATCH body -- includes `end_date` only when
the edit's patch carries one, i.e. adoption and the rolling-window end date
an open-ended rule must be given (`RecurringRuleEdit.h`); an ordinary edit
omits it, which `RecurringRecordingRuleSerializer`'s own partial-update
fallback depends on), `BuildOneTimeRecordingPatchBody()`
(`UpdateOneTimeRecording()`'s own PATCH body -- always includes both
`start_time`/`end_time`, confirmed live that omitting either crashes
server-side), and `BuildSeriesRuleDeleteQuery()` (`DeleteSeriesRule()`'s
own query-string suffix, via the already-tested `UrlEncode()`). None of
these had any test coverage before this, despite each one's own
documented, incident-backed invariant -- pulled out specifically so a
future edit can't silently violate one without a test catching it.
`TimerRequestBuilder` also gained `BuildOneTimeRecordingCreateBody()`
(added 2026-09-26, a 39th-pass audit, fixing a real, confirmed,
previously-undocumented dependency found via a project-wide review,
confirmed against Kodi's own real current SDK source, not itself
independently reproduced -- this same finding was actually raised, then
wrongly dismissed as a non-issue, by an earlier, 32nd-pass audit that
stopped reading Kodi's own source one step too early; see this
function's own comment for the corrected account): `CreateOneTimeRecording()`'s
own POST body, mapping a non-positive `start` to `now` before sending
it. Kodi's own instant recording genuinely sends a start time of exactly
0 -- `CPVRTimerInfoTag::CreateFromDate()` (`PVRTimerInfoTag.cpp`) briefly
sets the timer's start to the real current UTC time only to build its
own "Instant recording: ..." display string, then deliberately resets it
back to the raw `0` sentinel immediately afterward before the timer is
ever handed to this addon. This has only ever worked in practice because
`RecordingSerializer.validate()` silently rewrites any past `start_time`
to `now` server-side (confirmed against Dispatcharr's own real current
upstream source) -- an undocumented reliance on that clamp, not
something this addon ever did on purpose.
`BuildOneTimeRecordingCreateBody()` also gained an
`includeEpgProgramWindow` parameter (added 2026-09-29, fixing a real,
confirmed, live-verified bug -- see `docs/OPEN_ITEMS.md`): when true,
adds `custom_properties: {"program": {"start_time", "end_time"}}` --
title/id deliberately omitted so Dispatcharr's own later auto-
enrichment still runs. `RecordingSerializer.validate()` only applies
the instance's own configured pre/post padding when
`custom_properties.get("program")` is a dict -- `CreateOneTimeRecording()`
previously never sent one at all (see its own comment on why, above),
so a recording created by pressing "Record" in Kodi's own EPG guide got
exactly the raw EPG start/end times, no padding, regardless of this
addon's own `recording_pre/post_offset_minutes` settings.
`DispatcharrClient::CreateOneTimeRecording()` gained a matching
`isEpgBased` parameter; `AddTimer()`'s one-time-recording branch
(`PVRDispatcharr.cpp`) passes `timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID`
-- a manual, non-EPG recording is deliberately excluded, since it has no
real programme window to associate with the padding mechanism in the
first place. Confirmed live, twice, not just from source: first, that
this narrower (title/id-free) shape genuinely merges into Dispatcharr's
own later enrichment rather than replacing it (the real risk a full,
title-including `custom_properties` on create is separately documented
to trigger) -- two real recordings, one with this shape and one with
none, both came back fully enriched within seconds of the same async
task. Second, after implementing, that the fix itself works end-to-end
against a real Kodi client and the real lab instance: pressing "Record"
on a real EPG guide entry produced a recording padded by exactly the
instance's own configured `-1`/`+2` minutes, correctly matching a
directly-comparable Dispatcharr-side-created recording's own padding
from the same instance. One real, separate finding surfaced by this
same live re-verification, NOT caused by this fix and not something
this addon's own client code can control: Dispatcharr's own recording
auto-enrichment (`prefetch_recording_artwork`/`run_recording`,
`apps/channels/tasks.py`) queries a channel's raw `epg_data` foreign
key, not its effective/override one (`ChannelOverride.epg_data_id`,
the same per-field override mechanism `ChannelParser.cpp`'s own
`effective_epg_data_id` preference already accounts for on this
addon's own side) -- confirmed live that a channel with an EPG override
configured can have its recordings silently never enrich for a slot
outside the raw source's own coverage, even though this addon's own
guide display (correctly using the effective source) shows real
programme data for that exact slot. See `docs/OPEN_ITEMS.md`'s own
separate entry for the full account -- a Dispatcharr-upstream gap, not
an open item for this addon to act on.
`BuildSeriesRuleRequestBody()` also gained `titleMode`/`description`/
`descriptionMode`/`untaggedIsNew`/`epgSourceId` parameters, and
`BuildSeriesRuleDeleteQuery()` gained `epgSourceId` (both added
2026-09-29, fixing a real, confirmed bug found live and fixed the same
day -- see `docs/OPEN_ITEMS.md`): Dispatcharr's own `SeriesRulesAPIView.post()`
does a full `existing.clear(); existing.update(rule_record)` on match,
and `rule_record` only ever includes these fields when the request
itself sends them -- so every edit from this addon (which never sent
any of them) silently reset a rule's own `title_mode`/`description`/
`description_mode` customized via Dispatcharr's own web UI back to
their plain defaults, confirmed with a real round-trip against a real
test rule. None of these five have any Kodi-side UI of their own, so
`AddTimer()`'s create path passes empty/zero sentinels (nothing cached
yet to echo) while `UpdateTimer()`'s edit path passes the cached
`TimerRule`'s own last-known values -- an edit to an unrelated field no
longer collaterally resets them. `title_mode`/`description_mode` follow
the same "empty omits the key" convention `tvgId` already used;
`description` reuses it too, since an empty description produces the
exact same server-side outcome as omitting it entirely (Dispatcharr's
own default is also `""`). `untaggedIsNew` is only ever sent alongside
`mode=new`, mirroring Dispatcharr's own `if mode == "new" and
untagged_is_new` gating. `epgSourceId` omits `epg_source_id` for any
non-positive value, the same convention `channelId` already uses for
`channel_id` -- separately closing a real data-loss bug in
`BuildSeriesRuleDeleteQuery()`: Dispatcharr's own delete only restricts
by `epg_source_id` when the request supplies one, so omitting it (as
every call used to) matched and removed *every* EPG source's own copy
of a rule sharing `title`+`tvg_id`, not just the pinned copy the user
meant to delete.
`TimerIdentity` also gained `DecodeTimerClientIndex()` (the
series-rule/recurring-rule/plain-recording classification and id
decoding `UpdateTimer()`/`DeleteTimer()` each independently re-derived
from a timer's `ClientIndex`) and `ComputeRecordingExtendMinutes()`
(`UpdateTimer()`'s own "extend an already-recording timer" delta-to-
whole-minutes conversion, ceiling rather than flooring, and
non-positive-delta rejection). It also holds `ShouldRenameOnTimerEdit()`,
guarding both of `UpdateTimer()`'s one-time-recording branches against
Kodi's own stale cached copy of a timer's synthesized "Recording <id>"
placeholder title being mistaken for a real rename and sent back,
permanently overwriting a real, since-enriched title -- and (added
2026-09-26, fixing a real gap found via a project-wide review: the
recurring-rule branch never got the equivalent guard when the one above
was added) `ResolveRecurringRuleNameForUpdate()`, the recurring-rule
counterpart -- since `UpdateRecurringRule()`'s own single PATCH has no
separate "don't touch name" option the way a one-time recording's
`RenameRecording()` call does, this resolves the actual value to send
instead of deciding whether to skip a separate action.
`TimerIdentity` also holds `ResolveSeriesRuleMatchTitle()` (added
2026-09-26, fixing a real bug found via a project-wide review: Kodi's
own timer-settings dialog treats "Search guide for"
(`GetEPGSearchString()`) as the actual EPG-title match pattern for a
series timer and "Name" (`GetTitle()`) as a separate, purely cosmetic
label -- confirmed against Kodi's own source -- but
`AddTimer()`/`UpdateTimer()` sent `GetTitle()` to `CreateSeriesRule()`
as the match pattern and never read `GetEPGSearchString()` at all,
silently dropping any edit to "Search guide for" and letting an edit to
the cosmetic "Name" field change the match rule instead). Falls back to
`title` only when `epgSearchString` is empty, matching Kodi's own
dialog convention for a brand-new timer. `DeleteTimer()`'s own rare
cache-miss fallback (`PVRDispatcharr.cpp`) now calls this too (added
2026-09-26, a 19th-pass audit, fixing a real gap found via a
project-wide review: it previously took `timer.GetTitle()` directly) --
a series rule whose match pattern differs from its displayed name
silently failed to delete on that fallback path otherwise, the same
failure mode `ResolveSeriesRuleTvgId()` (`docs/RECORDINGS.md`) already
exists to avoid on the tvg_id half of that same title+tvg_id delete
identity.
`ComputeSeriesRuleClientIndex()` gained an `epgSourceId` parameter
(added 2026-09-29, fixing a real, confirmed bug found live and fixed
the same day -- see `docs/OPEN_ITEMS.md`): Dispatcharr's own series-rule
upsert matches by `(title, tvg_id, epg_source_id)`, not just the first
two, so two genuinely distinct rules pinned to different EPG sources
used to collide onto the identical `ClientIndex` here. Normalizes any
non-positive value to a single canonical "unpinned" before hashing,
matching Dispatcharr's own `parse_optional_epg_source_id()` treating
anything `<= 0` the same way. `FindSeriesRuleIndexByClientIndex()`'s own
duck-typed `RuleT` now needs an `epgSourceId` member too, alongside the
`title`/`tvgId` it already required.
`JsonFieldUtil` also gained `MergeDvrOffsetMinutes()`, `SetDvrOffsetMinutes()`'s
own merge-preserving PATCH body assembly -- Dispatcharr's PATCH replaces
the whole `value` blob rather than merging it server-side, so this sets
only the offset key(s) actually being changed and returns every other
already-present key (comskip settings, path templates, ..., and, as of
2026-09-26, the *other* offset when it isn't the one changing this
call) untouched. `preMinutes`/`postMinutes` are nullable (`const int*`,
changed from plain `int` in a 20th-pass audit, fixing a real, confirmed
bug found via a project-wide review, not itself independently
reproduced): a null pointer leaves that key exactly as `value`'s own
fresh fetch found it. Before this, `PVRDispatcharr::OnAddonSettingChanged()`'s
own "Dispatcharr's DVR padding is global-only, always push both
together" convention read whichever offset *didn't* just change from
Kodi's own local settings copy -- synced from Dispatcharr only once, at
addon construction -- so a genuine server-side change to the untouched
offset made since (Dispatcharr's own web UI, a second Kodi install)
silently reverted the next time the *other* offset was edited from this
Kodi, discarding `SetDvrOffsetMinutes()`'s own fresh GET of the real
current value in favor of a stale one. Originally guarded against a
real incident where a partial PATCH silently wiped a real instance's
own `dvr_settings` -- that protection (preserving every *other* key in
the blob) is unchanged. Also gained `IsTruncatedPaginatedResponse()` (added
2026-09-26, fixing a real gap found via a project-wide review, not
reproduced live): every `UnwrapListResponse()` call site
(`GetChannels()`/`GetChannelGroups()`/`GetRecordings()`/`GetTimerRules()`/
`GetRecurringRules()`/`FindCoreSettingsRow()`) already tolerated Django
REST Framework's `{"results": [...], "next": ...}` pagination envelope,
but never checked `next` itself -- `DispatcharrClient.h`'s own
top-of-file API summary documents the channel list specifically as
"paginated", so a genuinely multi-page response would have been cached
as a complete, successful fetch silently missing everything past page
one. Each call site now fails loudly instead when `next` is genuinely
non-null, rather than attempting to follow it and fetch more pages (a
bigger fix logged to `docs/OPEN_ITEMS.md`).
`FieldOr<T>()`'s own out-of-range guard (the 29th-pass UB fix) had a
boundary gap of its own, fixed 2026-09-27 in a 52nd-pass audit (fixing a
real, confirmed UB gap found via a project-wide review, confirmed with
UBSan against the vendored header, not reproduced live): for a 64-bit
integral `T`, `numeric_limits<T>::max()` isn't exactly representable as
a `double` -- it rounds *up* to exactly the next power of two, the same
value a genuinely out-of-range input (e.g. `9223372036854775807.0` for
`int64_t`) also parses to. The old `d > static_cast<double>(max)` check
compared `d` against that same already-rounded-up value, so an input
landing exactly on it read as in-range and reached an unchecked
`static_cast` to `T` -- undefined behavior. Fixed by comparing against
an explicit `max + 1.0` exclusive upper bound instead of `max` itself
with `>`. **Correction (2026-09-27, a 53rd-pass audit): this entry's own
original "no-op for a narrower type" claim about `+ 1.0` was backwards,
found via a project-wide review, confirmed by direct UBSan testing, not
itself independently reproduced.** For 64-bit `T`, `max + 1.0`
numerically rounds right back to `max` itself (already established
above) -- so `+ 1.0` genuinely is the no-op there, and switching the
comparison from `>` to `>=` is what actually fixes that case. For a
narrower type where `max` IS exactly representable (`int`, `uint32_t`,
...), `+ 1.0` is the opposite of a no-op: it's what keeps the true max
value itself correctly accepted under the new `>=` comparison, which
would otherwise wrongly reject it. Both together give the correct "one
past the true max" exclusive boundary either way -- 2^(bits-1) for a
signed type, 2^bits for unsigned, not a flat "2^bits" for both as this
entry originally, imprecisely, said.
`SegmentFetchFailure` is `ShouldGiveUpAfterSegmentFetchFailure()` (added
2026-09-28, fixing a real, confirmed bug found live while reproducing a
paused-viewer heartbeat scenario, not specific to it): the bounded-retry-
vs-give-up decision behind `ReadLiveTimeshiftStream()`'s new
`HandleLiveTimeshiftSegmentFetchFailure()` helper, called on a segment-
body fetch failure (a transport-level curl error, or an HTTP status other
than 200/206/404). Before this, such a failure just `return -1`'d
unconditionally, forever -- Kodi's own core retries a `-1` read near-
immediately rather than giving up, so a single dead/replaced buffer
turned into an unbounded, zero-backoff request storm against the same
doomed URL, confirmed live at ~7-8 requests/sec sustained indefinitely
(1,230 identical failed fetches over ~2m37s in one test run), with
Kodi's own UI showing normal-looking advancing playback throughout --
a silent hang, not a diagnosable failure. The fix mirrors the existing
"caught up to the tail" branch's own already-established pattern
(`RefreshLiveManifest(force=true, ...)`, checking its `fatalOut`) rather
than inventing a new one, and adds a small fixed bound
(`consecutiveSegmentFetchFailures`, a new per-Open() counter on
`LiveTimeshiftStreamState`) as a fail-safe for the one case a plain
manifest refresh can't positively resolve on its own: a stale
`access_token` cached from before the buffer died and got silently
replaced by a fresh one for the same channel, which `get_live_manifest`'s
own response has no way to signal, since it never carries an
`access_token` (only `start_buffer`'s/a reattach's response does).
Re-verified live against a real Kodi client and the real lab instance,
reproducing the exact original precondition (a real pause long enough to
build a genuine unread-segment backlog, then the buffer torn down while
still paused): exactly one failed fetch was logged before the stream was
marked fatal, down from 1,230+, with nothing further sent to the server
afterward. One caveat found the same live check, left as a distinct,
out-of-scope concern rather than chased further this pass: Kodi's own
player still didn't proactively notice the stream had gone fatal and
stop/error out on its own within the ~2 minutes this check waited, even
though every `Read()` was by then returning `-1` immediately -- a
separate, Kodi-core-level question from the server-hammering bug this
fix actually closes.
`CatchupSessionCache` is `ShouldReuseCachedCatchupSession()` (added
2026-09-29, fixing a real, confirmed, live-quantified bug -- see
docs/OPEN_ITEMS.md's own 2026-09-29 entry): whether
`DispatcharrClient::CreateCatchupSession()` can reuse its own last
result instead of POSTing another, redundant Dispatcharr catch-up
session. Confirmed against Kodi's own real current source
(`CPVRGUIActionsPlayback::PlayEpgTag()`, `PVRGUIActionsPlayback.cpp`,
and `CPVRPlaybackState::StartPlayback()`, `PVRPlaybackState.cpp`) that a
single catch-up play genuinely calls `GetEpgTagStreamProperties()`
twice, with identical `(channelUuid, programmeStart, durationMinutes)`
both times -- the first only to check `EPGPlaybackAsLive()` (which this
addon's own stream properties never set, per
`BuildCatchupStreamProperties()`'s own comment on the tried-and-reverted
history of that property), so the second call's own session is the only
one actually used; the first just sits in Dispatcharr's own store until
its own handshake expiry. Confirmed live, 2026-09-29: a single direct
`/api/catchup/sessions/` POST against the real lab instance took a
consistent ~0.31-0.32s, accounting for roughly 30% of catch-up
playback's own total ~1.1s startup delay when doubled -- a real,
user-perceptible cost, not a rounding error. Requires an exact match on
all three key fields (not just the channel -- a request landing within
the cache window for a genuinely different programme/duration must
never reuse a stale URL), and reuses `IsStaleSince()` (`Staleness.h`)
for the age check against a fixed 30-second `kMaxCatchupSessionCacheAge`
-- deliberately well under Dispatcharr's own 60-second catch-up-session
handshake expiry (`HANDSHAKE_TTL_SECONDS`, `apps/timeshift/sessions.py`)
so a reused URL is never handed out for a session that may have already
expired server-side. `CreateCatchupSession()`'s own cache hit skips
`EnsureAuthenticated()` and the network entirely, not just the POST
itself. Re-verified live: rebuilt, redeployed to the real Kodi test
client, and played a fresh already-aired programme -- informal
before/after `Player.Open` timing comparisons turned out too noisy
(single-sample, normal run-to-run jitter against a ~1.1s total dominated
by more than just the two POSTs) to cleanly show the expected ~0.3s
improvement, but new debug logging added alongside this fix (there was
none at all here before, despite this being a real blocking network
call) settled it directly: `CreateCatchupSession` logged a cache *miss*
for the first call, then a cache *hit* exactly 12ms later for the
second, confirming the redundant POST is genuinely skipped now.
`HlsViewerKeepAlive` is `ShouldSendHlsViewerKeepAlive()`,
`ClassifyHlsKeepAliveResponse()` and `NextHlsKeepAliveDueAt()` (added
2026-09-30, fixing a real, live-reproduced bug -- see
`docs/OPEN_ITEMS.md`'s 28th-pass entry on it): the pure decision core of
`DispatcharrClient::MaybeSendInProgressHlsKeepAlive()`. Dispatcharr only
keeps a finished recording's HLS directory while its own
`dvr:hls_viewer:{id}` Redis key (20s TTL, set only by a `.ts` request)
exists, and a viewer paused past the recording's end sends nothing that
refreshes it -- so the directory was removed out from under a viewer still
holding unread segments, and resume then hit a wall of segment 404s
(confirmed live: 63 in ~8s, the player gave up 17s after resume). The
fix is a HEAD on the newest known segment every 10s (the constants are
`static_assert`ed to stay inside half the server's TTL), scheduled from
each request that itself lands on a `.ts` URL so a playing stream never
sends one, and driven from `RefreshInProgressRecordingManifest()` because
`GetStreamTimes()`'s polling is the one thing still calling in during a
pause (confirmed live: roughly twice a second). The one non-obvious rule,
locked in by a test: it stays quiet once the reader has caught up to the
tail (`position >= totalBytes`). Dispatcharr only finalizes a finished
recording after the viewer key lapses, and this addon only learns a
recording is finished from that finalization, so a keep-alive from a
reader waiting at the tail would hold the recording open forever waiting
for an EOF it prevents. Re-verified live after the fix: a real 3-minute
recording paused ~110s past its own end kept its HLS directory (playlist
still 200, not 302), resumed and played through to the real end with zero
segment 404s, stalled ~22s at the tail, ended by itself, and finalized
(`completed`, playlist 302) -- 12 keep-alives, every ~10.1s, all 200.
`ChannelNumber` is `SplitChannelNumber()` and its three accessors
(added 2026-09-30, fixing a real, live-confirmed bug -- see
`docs/OPEN_ITEMS.md`'s entry on fractional channel numbers and
`docs/EPG.md`'s "Fractional channel numbers" section): Dispatcharr's
`channel_number` is a float server-side, so a subchannel like 5.1 is
real, but `Channel::channelNumber` was a plain `int` and
nlohmann::json's `get<int>()` silently truncates rather than throwing --
so 5.1 reached Kodi as channel 5, and two channels then read as the same
number to `FindAmbiguousChannelNumbers()`, which made *both* refuse a
guide. Confirmed live against three disposable channels numbered 88881,
88881.1 and 88882.5 (Kodi showed 88881/0, 88881/0 and 88882/0, and none
of the three got a guide in 8 minutes), and re-verified after the fix on
the same data (88881/0, 88881/1, 88882/5, all three with a guide).
`Channel::channelNumber` is now a `double`; `SplitChannelNumber()` derives
Kodi's channel number (the integer part), Kodi's sub-channel number (the
digits after the decimal point read as an integer -- lossy for a
leading-zero fraction and capped at nine digits, which is why nothing that
must tell channels apart uses it) and the guide-lookup key: the exact text
Dispatcharr's own `format_channel_number()` puts in `<channel id>` (an int
for a whole value, otherwise Python's `str(float)`, reproduced as the
shortest decimal string that reads back as the same double, through a
classic-locale stream so a comma-decimal Kodi locale can't turn "5.1" into
"5,1"). `HaveChannelNumbersChanged()`, `FindAmbiguousChannelNumbers()` and
both `m_epgByChannelNumber` lookups now compare that key text rather than
an int, so 5 and 5.1 are different channels everywhere. What Kodi can
display and what the export keys by are separate questions (changed
2026-09-30, fixing a real bug confirmed live -- see `docs/OPEN_ITEMS.md`'s
entry on null and zero channel numbers and `docs/EPG.md`): a number Kodi
can't show (0, negative, below 1) still has a key -- Dispatcharr exports
a channel numbered 0 as `<channel id="0">` -- and a channel with NO number
(`Channel::hasChannelNumber`, false for a null `channel_number`, which
`ChannelParser` otherwise reads as 0.0) is exported under its own id.
`FormatChannelGuideKey()` and `ChannelGuideKey()` (`ChannelRenumbering.h`)
produce that text for a whole channel, and numbers and ids share one
`<channel id>` space, so an unnumbered channel 77 and a channel numbered 77
are caught as ambiguous like any collision. Only a number that isn't
finite or is at least 1e15 has an empty key and is never looked up.
`PVRDispatcharr` also retries four startup-time reads it used to make once
(DVR padding, the admin check, the two catch-up flags) from the background
thread until each has answered (`RetryDeferredServerSyncs()`, added
2026-09-30), resends a DVR-padding edit whose push failed instead of letting
the next sync revert it, and reads back the settings it writes that latch a
"done" flag, because Kodi drops an addon-initiated `SetSetting*()` while this
addon's own settings dialog is open (confirmed live -- only when the dialog is
opened through Add-ons -> Configure, not `GUI.ActivateWindow`); the realtime
thread refreshes timers and recordings on a reconnect. That is threading and
Kodi-settings glue, confirmed live rather than unit-tested -- see `docs/EPG.md`.
A test-extraction pass on 2026-10-03 (a pure refactor, no behaviour change;
the addon was rebuilt through Kodi's harness afterwards) pulled five more
decisions out of the mixed-purity functions that made them inline:
`ClassifyRefreshTokenFailure()` (`AuthBackoff.h`, the 401/403-vs-transient-vs-
other mapping `EnsureAuthenticated()` applies after a failed token refresh),
`PlanRecurringRuleAdoption()` (`ManagedRecurringRule.h`, which pending rules an
adoption cycle tags and which it defers), `BuildCatchupSessionBody()`/
`ResolveCatchupPlaybackUrl()` (`CatchupSessionRequest.h`, the 480-minute
duration cap and the relative `playback_url` join), `PendingPaddingPush` with
`ApplyDirectPaddingPushResult()`/`ApplyPaddingRetryResult()` (`PaddingPush.h`,
the per-side pending DVR padding edit, its clear-only-what-was-sent rule and
the give-up-after-three-retries rule, replacing three members on
`PVRDispatcharr`), and `ParseXmlTvTime()` (now public in `XmlTvParser.h`, so the
"+0530"/"-0400"/malformed-offset handling is tested directly rather than only
through whole documents). The Python side gained direct tests for
`timeshift_buffer`'s `_same_buffer_instance()` and the smoke harness's
`_time_to_seconds()`.
A second pass the same day pulled out three more: `DecideInProgressColdStartStep()`
(`RecordingVisibility.h`, the per-attempt step of `OpenInProgressRecordingStream()`'s
cold-start loop -- open, wait, fail, or stop on a recording finished with no
segment -- locking in that a failed refresh never consults stale flags and only a
young recording's missing playlist is retried), `ResolveLiveSeekTarget()`
(`LiveEdgeMargin.h`, the live seek's tail clamp followed by its head clamp, head
winning when the rolling window has moved past the tail margin) and
`DesiredRecurringRuleTimezoneSetting()` (`TimeZoneUtil.h`, the canonical-name-or-
"manual" choice `SyncTimezoneFromDispatcharr()` writes). `timeshift_buffer`'s
`_get_live_manifest()` lost its playlist text parse to `_parse_live_playlist_lines()`
(media sequence plus `(sequence, filename, duration_ms)` triples), tested directly;
the parse now also skips an `#EXTINF:` followed by a blank line, which used to
stat() the channel directory itself as a segment. That pass also made
`UpdateTimer()`'s recurring-rule end-date decisions accept the fresh server read
when the cache misses (docs/OPEN_ITEMS.md) -- checked live the same day and found
not reachable through Kodi, which unloads its timers whenever the client is
recreated, so a consistency change rather than a fix.
A third pass the same day added `DecideRedirectFollow()` (`RedirectPolicy.h`, which
statuses `PerformWithSafeRedirects()` follows, when a chain is too long, and when
a POST is repeated as a GET), pulled the two plugins' Plugins-page summary lines
into `_staging_dirs_message()` (`recording_edl`) and `_buffers_summary()`
(`timeshift_buffer`), and added two cross-file suites under `tools/tests/`:
`test_version_sync.py` (addon.xml.in against the CoreELEC package.mk, each
plugin.json against its plugin.py `Plugin.version`, and a CHANGELOG entry for
each current version -- which found nine plugin versions with no entry, written
then) and `test_settings_ids_sync.py` (every setting id the C++ references is
declared in settings.xml and vice versa, and every label/help id has a
strings.po entry).
A hardening sweep on 2026-10-04 (an Opus read-only pass; the fixes and tests are
`docs/OPEN_ITEMS.md`'s Fixed entries of that date) changed pure logic as well as adding
tests. `PaddingPush.h` numbers every padding edit (`BeginPaddingEdit()`,
`PaddingEdit`) so a late result can tell an older edit from a newer one, where
comparing values let a parked older value overwrite a newer successful push.
`CurlCallbacks` catches an allocation failure in `BoundedWriteCallback()`/
`WriteCallback()` (`BoundedStringSink::allocationFailed`) and has lower response
ceilings on a 32-bit build. `ComputeGuidePrevDays()` reads a catch-up depth of 0 as
the 7-day default. `Recording::programTvgId` (from `custom_properties.program.tvg_id`)
lets `MatchRecordingsToSeriesRules()` link a channel-less series rule's recordings,
in a second pass after the channel-pinned rules. `PruneRolledOffLiveSegments()`
(`LiveEdgeMargin.h`) trims the live segment list. `TimeFromIso()` rejects out-of-range
fields and offsets, `XmlTvParser` drops a programme that ends at or before its start,
and `M3u8HasEndList()` tolerates trailing blanks. `WriteStateFileAtomically()` (not
unit-tested, Kodi VFS) writes the two persisted JSON files through a temp file.
New tests: the `WebSocketClient` paths against the local-server fixture (extended
lengths, fragmentation, ping/pong, close, protocol violations, handshake failures),
`dispatcharr-plugin/timeshift_buffer/tests/test_http_handler.py` (the plugin's file
server over a real loopback socket) and
`dispatcharr-plugin/recording_edl/tests/test_django_boundary.py` (`get_edl`, HLS
staging classification and scan roots against fake `apps.channels.models`/
`core.models` modules in `sys.modules`, which is how that boundary can be tested after
all). Four of the items it left open were fixed and live-verified the same day:
`IsSameOrigin()` (`RedirectPolicy.h`) gates the API key and every request to a segment
URL on the configured origin (`DispatcharrClient::IsOnConfiguredServer()`);
`timeshift_buffer` 0.8.1 reads a request through `_DeadlineSocketReader` against one
absolute 10 s deadline and caps concurrent connections at 256; `WebSocketClient` takes
a stop check (`SetStopCheck()`) and waits in 500 ms slices, so exit no longer waits out
the connect timeout (30.1 s to 5.0 s live); `ClassifyAdoptionPatchFailure()`
(`ManagedRecurringRule.h`) treats a 4xx other than 408/429 as permanent for a rule and
bounds the other retries. What stays open from it: the unpinned Kodi clone in CI, and
the small items in `docs/OPEN_ITEMS.md`.
A second sweep the same day (`docs/OPEN_ITEMS.md`, the eight "second hardening sweep"
entries) added `ParseJsonResponseBody()` (`JsonResponse.h`, every `std::exception` from a
response parse is a failed parse -- an integer beyond double range used to escape
`Request()` and end the process), `RunInBoundedBatches()` (`BoundedParallel.h`, the
probe fan-out that survives a thread that cannot be created), bounds on
`MatchSeriesText()` (`kMaxSeriesQueryBytes`, `kMaxSeriesQueryGroups`, beyond which a
rule is `kUnsupported`), range checks in `ParseXmlTvTime()`, and a `responseUnusable`
flag on `ShouldCountTowardEpgFailureBackoff()` for an oversized or unparseable guide.
`RunGuardedWorker()` (`PVRDispatcharr.cpp`, glue) keeps an exception out of the four
worker-thread bodies, and `SleepUnlessShuttingDown()` ends `AddTimer()`'s delayed refresh
at once on exit. `timeshift_buffer` 0.8.2 shortens the access token on ffmpeg's command
line to 120 s; the Python CI job installs ffmpeg and sets `REQUIRE_FFMPEG` so the
continuity-counter test cannot silently skip.
A third sweep the same day (the ten "third hardening sweep" entries in
`docs/OPEN_ITEMS.md`) added `SaturatingTimeGm()`/`PortableTimeGmSaturating()`
(`TimeUtil.h`: on a 32-bit `time_t`, a date from 2038 on saturates instead of reading as
-1, which made a rule ending in 2099 look open-ended), made a 401 a retry-later adoption
failure, re-parks the newer padding value when two direct pushes finish out of order,
and gave `TrackDetachedThread()` and the three worker-thread creations the same
thread-creation guard `RunInBoundedBatches()` has. `timeshift_buffer` 0.8.3 records the
ffmpeg that owns each channel directory in `ffmpeg.owner.json`, so one whose Redis state
was lost is stopped by the orphan scrub or a fresh start instead of running forever
beside a second one, restores the socket timeout after the request headers (0.8.1 had
cut a slowly read large segment off after ten seconds), and caps connections per client
address.
A fourth sweep (the seven "fourth hardening sweep" entries in `docs/OPEN_ITEMS.md`)
found the scrub's stale orphan list stopping a buffer that had just been started: the scrub
(`_scrub_orphaned_dirs()`, `timeshift_buffer` 0.8.4) now re-checks each entry
(`_is_untracked_orphan()`) under that channel's start lock. The file server serves only
`live.m3u8` and `seg_<n>.ts`, in chunks. The series-query AND/OR scan is one linear pass
(identical to the old one on 200,000 random queries), `SaturatingTimeGm()` also clamps a
representable date above the ceiling, and `ClampedDurationSeconds()` (`RecordingParser.h`)
keeps a recording's duration from wrapping.
A fifth sweep (the seven fifth-sweep entries in
`docs/OPEN_ITEMS.md`) added a wall-clock budget to both blocking tail waits
(`ComputeCatchUpWallClockBudgetMs()`/`HasCatchUpBudgetElapsed()`, `CatchUpUtil.h`: twice the
attempts' own sleep time, at most 120 s, so one `Read()` cannot block 25 to 50 minutes against
a server that times out), and confined `recording_edl`'s `get_edl` read (`_is_under_any_root()`,
`_read_edl_text()`, `.edl` suffix, 1 MiB cap, regular files only; 0.2.2). `timeshift_buffer`
0.8.5 tears down an absent-state buffer under the start lock (`_teardown_untracked_buffer()`),
gives a response body one absolute deadline, and fetches the reaper's Redis client per tick.
Many exact-boundary tests came from the mutation pass.
A sixth sweep (the five "sixth hardening sweep" fix entries and one mutation entry in
`docs/OPEN_ITEMS.md`) fixed a series-rule channel edit that reset the rule's hidden fields
(`UpdateTimer()` now echoes all five from the cached rule whatever the channel;
`ShouldReplaceSeriesRuleOnEdit()` has a source-aware form), made a recording lookup
that does not describe the requested recording a failed lookup
(`IsRecordingResponseFor()`), range-checked integer `FieldOr<T>()` reads, and stopped
`DeleteTimer()`'s cache-miss fallback from issuing an unscoped series-rule delete.
A seventh sweep (the seven seventh-sweep entries in
`docs/OPEN_ITEMS.md`) corrected a regression in that series-rule echo
(`ResolveSeriesRuleSourceOnEdit()`: the EPG source pin is echoed only while the re-derived
`tvg_id` is unchanged, otherwise 0), made an older padding push's failure park the newer
successful value, bounded the recurring-rule name so the ownership tag cannot push it past the
server's 255 characters (`AddManagedRuleMarkerBounded()`), and, in `timeshift_buffer` 0.8.6,
took the file server's response size from `fstat()` of the open file and made the start-lock
release one atomic script.
An eighth sweep (the eleven eighth-sweep entries in `docs/OPEN_ITEMS.md`, the first of them
time-critical) added `permanentDaylightFromUtc` to `TimeZoneUtil.cpp`'s table
(`kPermanentDaylightFromNov2026Utc`: Vancouver and Edmonton stay on daylight time from
2026-11-01, so the table's fall-back would have put recurring rules an hour off) with
`tools/tests/test_timezone_table_vs_zoneinfo.py` comparing the table against the system's
zone data; redesigned `PaddingPush.h` around numbered edits (`BeginPaddingEdit()`; the result
callback of `SetDvrOffsetMinutes()` runs under the request's own mutex so results are applied
in landing order); added `ResolveRecurringRuleTimesOnEdit()` (a rule whose time falls in a
spring-forward gap keeps its stored times when Kodi hands back the instants `GetTimers()`
reported), `HasPinnedSiblingSeriesRule()` (a series delete never takes a source-pinned rule
with it), `IsValidCivilDate()` (February 30 is not a date) and `SanitizeServerErrorBody()`; and,
in `timeshift_buffer` 0.8.7, a monotonic SIGTERM grace, `_is_redis_response_error()` (a Redis
permission error is a `ResponseError` subclass) and an `OverflowError` guard in
`_int_setting()`. `tools/check_doc_refs.py` gained a fourth check, the `docs/X.md's "Title"`
form, over the docs and the source comments, and six dangling citations in `src/` were fixed.
The cross-worker wall-clock heartbeat ages were a recorded known gap here (closed for the reaper in `timeshift_buffer` 0.8.14, below).
A ninth sweep (the six ninth-sweep entries in `docs/OPEN_ITEMS.md`) resolved each side of a recurring-rule
edit on its own in `ResolveRecurringRuleTimesOnEdit()`, made the padding retry choose what to send under
the client's settings mutex (`DispatcharrClient::SetDvrOffsetMinutesChosen()`, `PendingPaddingEdits()`;
`GetDvrOffsetMinutes()` takes the same mutex, and `IsSafeToSyncPaddingFromServer()` /
`PendingPaddingPush::inFlightEdits` defer the startup sync while an edit is pending or in flight), serialized
the `recording_epg_links.json` save (`m_recordingEpgLinksSaveMutex`), made the tzdata cross-check skip on a
database older than 2026c, extended the citation checker to the comma and colon forms, and added tests for
every mutation survivor its pass found. No plugin code changed, so neither plugin was re-versioned.
A tenth sweep (the five tenth-sweep entries in `docs/OPEN_ITEMS.md`; no high or medium finding) made the
Plugins page's "Start Test Buffer" print the playlist URL with its access token (`timeshift_buffer` 0.8.8,
`_manual_test_hint()`; an API caller never gets the token in its message), extended `check_doc_refs.py`
again (the backtick citation form, `CLAUDE.md`/`CONTRIBUTING.md` via `EXTRA_CITING_FILES`, bold leads that
wrap across lines, quote-insensitive titles), made a padding push that threw report itself failed so
`inFlightEdits` cannot defer the startup sync forever, and added tests for every mutation survivor.
An eleventh sweep (the four eleventh-sweep entries and one known gap in `docs/OPEN_ITEMS.md`; no bug in the
code) made both the workflow and the documented hand rebuild build the plugin zips with `TZ=UTC zip -X -r`
(a bare `zip` publishes the build machine's timezone in every entry; `test_plugin_zip_contents.py` runs the
workflow's own command under a non-UTC zone), gated the `build-windows` job off for pull requests
from forks, corrected the lint and test scope in `CONTRIBUTING.md`/`CLAUDE.md`, and added `WebSocketClient`,
`SocketWait`, `CurlCallbacks` and `SanitizeServerErrorBody` tests for the mutation survivors (a hung suite
counts as a kill). ThreadSanitizer could not be run locally (recorded as a known gap).
A twelfth sweep (the five twelfth-sweep entries in `docs/OPEN_ITEMS.md`; no bug affecting correctness) bounded
`timeshift_buffer`'s per-worker manifest cache (0.8.9, `_remember_manifest()`, 16 channels), dropped the
never-committed icon from `addon.xml.in`, listed the five missing headers in `CMakeLists.txt` (both checked by
`tools/tests/test_addon_metadata.py`), started an `[Unreleased] -- addon` section in `CHANGELOG.md` (keep it
current as user-visible fixes land), and added tests for the plugin mutation survivors.
A thirteenth sweep (the three thirteenth-sweep entries and one known gap in `docs/OPEN_ITEMS.md`) found the
first real defect in a while by compiling the real `DispatcharrClient.cpp` against Kodi's dev-kit headers
with a stub runtime and driving it from 16 threads: sharing `CURL_LOCK_DATA_CONNECT` across threads crashes
stock libcurl 8.5.0, so the main share (`m_curlShareState`) now shares DNS and TLS sessions only, like the
probe share (the macOS-only reading in `docs/RECORDINGS.md` is corrected). Also `AsciiToLower()`/
`IsAsciiAlnum()` (`StringUtil.h`) replace `std::tolower`/`std::isalnum`, which follow Kodi's `LC_CTYPE`, and a
`PortableTimeGm` test that passed without a tz database now uses a POSIX zone string.
A fourteenth sweep (the four fourteenth-sweep entries and the updated glue-harness entries in `docs/OPEN_ITEMS.md`)
checked the scratch harness in as `tests/glue/` (see its README: the real `DispatcharrClient.cpp` and
`PVRDispatcharr.cpp` against Kodi's dev-kit headers, stub Kodi functions and a fake Dispatcharr, under ASan/UBSan or
TSan; its `ci.sh` scenarios run in CI since 2026-10-06 as the `glue-harness` job, and it is for running by hand after
touching the stream, auth or shared-curl paths) and fixed what it found on the
slow paths: `Request()`'s `timeoutMsOverride` (`ShortRequestTimeoutMs()` -- a sixth of the connection timeout, 5 s at the
default -- for `stop_buffer`, the live manifest refresh and the startup
version check; `DeferAuthenticationAfterUnresponsiveServer()` after a startup check that gets no response),
`PermanentReadFailureTracker` (`SegmentFetchFailure.h`: a recording read that keeps failing for a reason that will
not clear ends as EOF with a notification) and `IndexToArmTrackerAfterMerge()` (`UnprobeableSegment.h`). The timeout
changes alter runtime behaviour and need a live check before the branch merges.
A fifteenth sweep and its follow-ups (the fifteenth-sweep entries and the ones dated 2026-10-06 in
`docs/OPEN_ITEMS.md`) fixed a tracker-arming regression of the fourteenth (`IndexToArmTrackerAfterMerge()` takes the probed count),
bounded the authentication step and the in-progress steady-state requests like the live ones (`timeoutMsOverride`,
`ShortRequestTimeoutMs()`; Open's cold start keeps the configured timeout), made 500 a transient recording-read failure,
ended a ranged recording read when its buffer is full (`FixedBufferSink::abortWhenFull`), added `DescribeRecordingOpenFailure()`
(`RecordingHttpUtil.h`), pinned CI's Kodi to `KODI_COMMIT` and added the `unit-tests-tsan` job (Catch2 test discovery is
`PRE_TEST` for it); `timeshift_buffer` 0.8.10 holds `max_concurrent_buffers` across channels and trims `ffmpeg.log`.
Live-checked against the lab 2026-10-05/06; `tests/glue/` gained `live_blackhole_*`, `live_slow_open`, `ip_cascade`,
`ip_blackhole_close`, `rec_range_dropped` and `rec_open_missing`, each of which fails on the code it replaced.
Three modules from the same stretch were not in this list: `RequestTimeout` (`ShortRequestTimeoutMs()`, a sixth of the
connection timeout clamped to 5-30 s, the bound of Stop, the steady-state refreshes and the authentication before them;
`IsInProgressSteadyState()` and `ShouldSkipPlaylistAfterUnansweredLookup()`), `RefreshEvents` (`ClassifyRefreshEvent()`
reads Dispatcharr's realtime `epg_refresh`/`m3u_refresh` events, and `ScheduleGuideRefetchForEvent()` puts the guide
fetch 330 s later, past the server's 300 s cache of the exported guide) and `ServerOffsetCrossCheck` (reads the UTC
offset Dispatcharr actually applied from a recurring rule and its upcoming occurrences, bounded to an hour from the table,
followed instant by instant when the zone's older rules explain it, adopted after two evaluations in a row and forgotten
when the zone setting changes).
A sixteenth sweep (the sixteenth-sweep entries in `docs/OPEN_ITEMS.md`) reworked that cross-check after finding it late for
the 2026-11-01 case it exists for, fixed regressions of the request bounds (an authentication cooldown no longer stops an
in-progress recording from growing, a reopened one keeps the configured timeout), moved the order-sensitive response
checks of a recording read and a live segment fetch into `ClassifyRecordingReadResponse()`/`ClassifyLiveSegmentResponse()`
(`RecordingHttpUtil.h`), made `tests/glue/`'s scenarios able to fail (and UBSan reports fail a CI run), and released
`timeshift_buffer` 0.8.12 (a byte budget on request headers, IPv6 peers counted by /64).
`PVRDispatcharr`/`DispatcharrClient`/`WebSocketClient`'s actual PVR API
surface, HTTP client, and socket handling -- where the real bugs live --
aren't attempted by this Catch2 suite (`WebSocketClient` has tests against a
local server, and `tests/glue/` compiles the real client and PVR code
against a fake Dispatcharr, a stub Kodi runtime and Kodi's own dev-kit
headers, which CI runs as the `glue-harness` job); a real Dispatcharr and a
real Kodi remain manual/live-hardware verification territory.

**Python** (`dispatcharr-plugin/{recording_edl,timeshift_buffer}/tests/`,
pytest, wired into CI's `unit-tests-python` job): `recording_edl` covers
`_parse_edl` (including a milliseconds-overflow regression: a
finite-but-huge value passes `isfinite()` on the raw seconds value but
overflows to `inf` once scaled by 1000, so the finiteness check runs on
the scaled value actually passed to `round()`) and `_edl_path_for`
(besides its path-traversal guard, also returns `None` rather than
raising for a non-dict `custom_properties` itself, non-dict `comskip`,
or non-str `edl`/`file_path` -- externally sourced from Dispatcharr's
own DB column, so its shape isn't trusted without checking -- and, added
2026-09-26 in a 23rd-pass audit, fixing a real, confirmed bug found via
a project-wide review and confirmed against Dispatcharr's own real
current upstream source: also returns `None` when `comskip` contains
`segments_kept`, the signal Dispatcharr's own default "cut" comskip mode
sets right after it deletes the `.edl` file it just cut against
(`os.remove(edl_path)`, `apps/channels/tasks.py`) -- that branch still
leaves `edl` pointing at the now-deleted filename, unlike the "mark"
mode or the no-commercials-skipped branch, neither of which ever
deletes it, so `segments_kept` -- not a `mode` key, which "mark" sets
but "cut" doesn't -- is the real distinguishing signal),
`_sidecar_base_name`, `_is_under_dotted_dir`,
`_prune_empty_directories`, `_resolve_scan_root`/`_dedupe_scan_roots`,
`_scrub_orphaned_recording_sidecars` end-to-end via `monkeypatch`ing its
one Django-model-dependent call, `_classify_hls_dir_info` (the pure
classification core of `_classify_dvr_hls_dir`, taking an
already-resolved `recording_exists`/`custom_properties` instead of
querying Django directly -- covers all four classifications (falling
into "referenced" rather than raising for a non-dict `custom_properties`
or a non-str/PathLike `_hls_dir` too, the same reasoning as
`_edl_path_for`'s own hardening -- the caller's own scan loop has no
try/except around this call, so one malformed entry used to abort the
whole scan), including
the "never delete the only surviving copy of a failed recording"
`preserved_failure` case), and `Plugin.run()`'s own dispatch and
response text for every action branch that's Django-free or where the
one Django-dependent call can itself be `monkeypatch`ed (the actual
response a client sees, not just the underlying helpers in isolation).
`timeshift_buffer` covers `_channel_dir`, `Plugin._resolve_channel_uuid`,
`_resolve_request_path`, `_BufferRequestHandler._parse_range`, `_proxy_url`,
`_int_setting` (added 2026-09-26, a 20th-pass audit, fixing a real,
confirmed bug found via a project-wide review, not itself independently
reproduced: every one of this plugin's numeric settings was read with a
bare `int(settings_dict.get(key, default))` -- Dispatcharr's own
`_merge_settings_with_defaults()` only fills in a missing key, not one
present but emptied, and its own frontend number field can save a
cleared field as `""`, so clearing any one of these raised an uncaught
`ValueError` at the very top of `run()`, breaking every action for that
plugin instance, including `stop_buffer`/`stop_all` -- exactly the
actions someone would need to actually recover from a bad setting; its
own `minimum` parameter also floors an in-range-but-nonsensical parsed
value, e.g. a parsed `segment_seconds=0` dividing by zero in
`_compute_segment_counts` below, or `idle_timeout_seconds=0` making the
reaper treat every viewer as stale on every tick). Also gained a
`maximum` parameter (added 2026-09-26, a 21st-pass audit, fixing a real,
confirmed bug in the same class, found via a project-wide review, not
itself independently reproduced): an in-range-looking `http_port` above
65535 parsed fine as a plain `int`, but `socket.bind()` then raised
`OverflowError` -- not `OSError`, the only exception
`_ensure_http_server_running()` actually catches -- breaking every
action the same way an unparseable value did, since that call runs
unconditionally at the very top of `run()`, before action dispatch).
`_str_setting` (added 2026-09-26, a 23rd-pass audit, fixing a real,
confirmed bug found via a project-wide review, not itself independently
reproduced) is the same class of fix for a string setting instead of a
numeric one: `storage_path`/`internal_base_url` were read raw via a bare
`settings_dict.get(key, default)`, so clearing either in Dispatcharr's
settings UI silently redirected every buffer's files (and the HTTP file
server's own served root) to the worker process's own current working
directory (`Path("")`), or made every `start_buffer` fail against ffmpeg
with an unhelpful error, instead of falling back to the documented
default. Falls back to `default` for a missing, non-string, or
empty/whitespace-only (after `strip()`) value -- deliberately doesn't
reject a relative path, since that's unusual but not itself invalid.
`_compute_segment_counts` (the pure segment-count-sizing core of
`_start_ffmpeg`, including the `visible_segments * segment_seconds ==
buffer_minutes * 60` invariant `docs/TIMESHIFT.md` cites to rule out a
suspected audio-sync-error correlation, and the floor-at-1 edge case),
`_is_process_alive` (via `monkeypatch`ing `os.killpg`/`os.waitpid`/its
own `_read_proc_pid_stat` helper, including the documented conservative
"assume alive" fallback on a non-`ProcessLookupError` `killpg` failure
or an unreadable `/proc/<pid>/stat`, and the zombie-detection path this
adds: `kill(pid, 0)` alone can't tell a zombie -- already exited, not
yet reaped by its real parent -- apart from a genuinely running
process, since both answer signal 0 successfully; `_is_zombie_proc_stat`
is the pure `/proc/<pid>/stat`-state-field parser behind that check,
covering its own comm-containing-spaces-or-parens caveat),
`_build_ffmpeg_command` (the pure argv-assembly core of `_start_ffmpeg`,
including the `-headers`-must-precede-`-i` ordering and the deliberate
absence of `-reset_timestamps`), `_stop_ffmpeg`'s own SIGTERM/poll/
SIGKILL sequence (via `monkeypatch`ing `os.killpg`/`_is_process_alive`/
`time.time`/`time.sleep`, covering the escalate-to-SIGKILL-after-the-
deadline path, the no-poll-at-all path when SIGTERM itself fails
unexpectedly, and the zombie-treated-as-exited-not-still-running path --
see `_stop_ffmpeg`'s own comment on why its poll goes through
`_is_process_alive()` rather than a bare `os.killpg(pid, 0)`),
`_reaper_loop` for one real tick end-to-end (a one-shot fake
`stop_event` plus an in-memory fake Redis client standing in for
`_redis()`, covering fresh leadership acquisition, TTL renewal when
already leader, skipping all work when another process holds the
leader lock, and exception handling that still reaches the
between-tick `wait()`), `_ensure_reaper_running` (covering the real bug
fixed alongside this: a settings change made after the reaper thread's
first start must still reach it, via the module-level
`_latest_settings_dict` every `run()` call now updates unconditionally
rather than a closure frozen over whichever call happened to start the
thread), `_stream_attribution_headers` (client_ip
validation path only -- the username/JWT branch stays untested),
`_prune_stale_viewers`,
`_find_orphaned_channel_dirs`/`_scrub_orphaned_dirs` (via `monkeypatch`ing
their one Redis-dependent call), and `_get_live_manifest` end-to-end
against a real temp filesystem -- including regression tests for its
documented cache-invalidation incidents (the newest-segment-always-
restatted race, extended to also cover the fast nothing-changed-since-
last-read path -- see `docs/TIMESHIFT.md`'s own follow-up on that gap --
and the instance-token "Packet corrupt" bug).
`timeshift_buffer`'s own `Plugin.run()` dispatch and every action
handler (`start_buffer`, `stop_buffer`, `heartbeat`, `get_live_manifest`,
`list_buffers`, `stop_all`, `scrub_orphaned_buffers`) is covered too, the
same way as `recording_edl`'s: by `monkeypatch`ing the module-level
Redis-touching functions (`_get_buffer_state`/`_set_buffer_state`/
`_delete_buffer_state`/`_list_buffer_keys`/`_iter_buffer_states`) and
process-management functions (`_start_ffmpeg`/`_remove_channel_files`/
`_teardown_buffer`/`_is_process_alive`) each handler calls, plus the two
lifecycle calls `run()` itself makes unconditionally before ever
dispatching (`_ensure_http_server_running`/`_ensure_reaper_running`,
stubbed to no-ops -- a real HTTP server/reaper thread has no place in a
unit test). Covers real behavior previously untested at any level: the
reference-counted stop/reattach messages (dead-buffer cleanup and
fresh-start on `start_buffer`, the viewer-count-based decision between
"still active for other viewers" and a full teardown on `stop_buffer`),
the `access_token` retrofit for pre-upgrade state, and the
`BufferFailedError`-vs-plain-`RuntimeError` distinction in
`get_live_manifest` (only the former tears the buffer down and reports
`fatal: true`). `_classify_existing_buffer()` is the pure decision core
of `start_buffer`'s own existing-buffer branch -- "dead" (ffmpeg gone,
clean up and start fresh), "stopping" (`_teardown_buffer()` has marked
it mid-teardown, within `_TEARDOWN_GRACE_SECONDS`, so `start_buffer`
refuses instead of reattaching to -- or starting a duplicate alongside --
a buffer whose files/state are about to be removed out from under it, a
real race `_teardown_buffer()`'s own multi-second `_stop_ffmpeg()`
sequence otherwise left open), or "reattach" (the normal case, alive and
not recently marked stopping). "dead" only wins over a *stale* "stopping"
marker (older than the grace period, or one with no `stopping_since` at
all) so a crash partway through teardown still self-heals via the
existing cleanup path rather than getting stuck reporting "stopping" for
the full `_BUFFER_STATE_TTL`. The grace-period check itself (added
2026-09-26, a 27th-pass audit, fixing a real, confirmed race found via a
project-wide review, not itself independently reproduced) checks
`stopping`/`stopping_since` *before* `is_alive` now, not after: without
it, the process no longer testing alive (which `_stop_ffmpeg()` alone
can cause well before `_teardown_buffer()` actually finishes removing
files and deleting its own Redis state -- file removal for a large
buffer can itself take real time) read as "dead" for that entire
remaining window, letting a concurrent `start_buffer` spawn a brand-new,
untracked ffmpeg that the still-in-flight *original* teardown then
orphaned by deleting its state out from under it -- an ffmpeg left
holding a provider stream slot until the container restarts, not just a
theoretical race. The "stopping" refusal's own response also carries
`retryable: True` -- the same structured-signal convention as
`get_live_manifest`'s own `fatal` -- which is what lets the addon's own
`OpenLiveTimeshiftStream()` retry briefly instead of failing outright
when it lands in that window.
`Plugin.stop()` (Dispatcharr's own lifecycle hook, called on plugin
disable/delete/reload -- covered the same way, by `monkeypatch`ing
`_iter_buffer_states`/`_teardown_buffer`/`_scrub_orphaned_dirs`/
`_stop_http_server`) gained a `reason`-gated branch (fixed 2026-09-27, a
41st-pass audit, fixing a real, confirmed bug found via a project-wide
review, confirmed against Dispatcharr's own real current upstream
source, not itself independently reproduced): `PluginReloadAPIView`
(`apps/plugins/api_views.py`) calls `PluginManager.stop_all_plugins(reason="reload")`
-- fired by the Plugins page's own "Reload" button *and* its "refresh
all repos" button, on *every* enabled plugin, for *any* plugin
install/enable/disable/delete, not just this one -- before its own
`disable()`/`delete()` set `reason="disable"`/`"delete"`
(`PluginManager.stop_plugin()`, `apps/plugins/loader.py`). `stop()`
used to tear down every live buffer (`_stop_all()`/
`_scrub_orphaned_buffers()`) unconditionally regardless of which of the
three reasons triggered it, so pressing either Plugins-page button for
some *unrelated* plugin -- with no intent to touch this one at all --
ended playback server-wide for every viewer with server-side timeshift
active (the addon's own `get_live_manifest` then reports `fatal: true`).
Now skips that teardown specifically for `reason == "reload"`, falling
back to the full teardown for every other value including a
missing/unrecognized one (`"disable"`/`"delete"`, or a
Dispatcharr-internal `"shutdown"` its own test suite uses but no real
call site does) -- the reaper stop event and this module instance's own
HTTP server still stop unconditionally either way, since those are
purely in-process objects, not shared Redis/ffmpeg state. A distinct
bug from the already-logged `docs/OPEN_ITEMS.md` entry on a plugin
reload leaking a *stale* listener/reaper pair in every *other*
Dispatcharr worker -- that entry never covered this one worker's own
`stop()` tearing down real, running buffers it shouldn't have. Anything
touching Dispatcharr's own Django models or
Redis directly (the deferred imports inside `_dvr_sidecar_scan_roots`/
`_classify_dvr_hls_dir`, and the real `_redis()` client, the real HTTP
server, and real ffmpeg subprocess management on both plugins) stays
untested, on the same principle as the C++ side stopping at the Kodi SDK
boundary.
`_get_live_manifest_action()` also gained a `viewer_id` resolve-and-pass-
through into `_apply_heartbeat()` (fixed 2026-09-28, `timeshift_buffer`
`0.6.3`, fixing a real, confirmed bug found live: a paused viewer's own
per-viewer heartbeat -- distinct from the buffer-wide one this call
already refreshed -- went stale, since `get_live_manifest` is the
*only* thing a paused Kodi client still calls (via the addon's own
`GetStreamTimes()` polling; `SendTimeshiftHeartbeat()` only ever fires
from `ReadLiveTimeshiftStream()`, which a pause stops calling
entirely). A stale per-viewer heartbeat got pruned by the reaper, and
if every other viewer then stopped, the buffer was torn down out from
under the still-paused viewer -- confirmed live end-to-end against a
real Kodi client and the real lab instance, both before the fix
(pruned at the 30s mark matching `idle_timeout_seconds`) and after
(survived a full 50s window with zero manual intervention on the real
client's behalf, purely through Kodi's own automatic polling). The
matching C++-side fix (`DispatcharrClient::RefreshLiveManifest()` now
sends `m_liveTimeshiftStream.viewerId` in the request) has no test of
its own, on the same "DispatcharrClient's actual HTTP call-building
stays untested" principle as the rest of that boundary. Covered by two
new tests, `test_run_get_live_manifest_refreshes_the_calling_viewers_own_heartbeat`
and `test_run_get_live_manifest_ignores_a_viewer_id_this_buffer_does_not_know_about`
(the latter confirming `_apply_heartbeat()`'s own existing
unknown-`viewer_id` no-op still holds through this new call site).
`_acquire_start_buffer_lock()`/`_release_start_buffer_lock()` (added
2026-09-29, `timeshift_buffer` `0.6.4`, fixing a real, live-confirmed
race -- see `docs/OPEN_ITEMS.md`'s own entry for the full account,
including the real reload-propagation gotcha hit while re-verifying it)
are a per-channel `SET NX EX` lock (the same primitive `_reaper_loop`'s
own leader election already uses, just per-channel/short-lived instead
of singleton/continuously-renewed) wrapped around `_start_buffer()`'s
own classify-then-spawn sequence, closing a real race where two
near-simultaneous callers for the same channel could each spawn their
own ffmpeg process with only one ever getting tracked -- the other
permanently orphaned, invisible to the reaper/`stop_buffer`/`stop_all`
*and* the orphan-directory scrub (confirmed live: both processes write
into the same channel-uuid-named directory, so it still reads as
"tracked" once either one's own state write lands). Covered by its own
7 dedicated tests, reusing `_FakeReaperRedisClient` (the reaper tests'
own fake in-memory Redis client, since this is the same primitive):
acquire succeeds when free, fails when already held, is independent
per channel, and release only removes a lock still holding the exact
token it was given (not one a different caller has since legitimately
re-acquired after this caller's own hold outlived its TTL) -- plus two
`Plugin.run()`-level tests confirming the retryable-contention response
and that the lock is released even when the locked call itself errors.
The pre-existing classify-then-spawn body moved, unchanged, into a new
`_start_buffer_locked()`, called only while `_start_buffer()` holds the
lock.
`_apply_heartbeat()` also now re-adds a `viewer_id` missing from `viewers`
instead of ignoring it, and `_is_stale_access_token()` (`timeshift_buffer`
`0.6.5`, both added 2026-09-29, both live-confirmed -- see
`docs/OPEN_ITEMS.md`'s own entries) gives `get_live_manifest` a
positively-confirmed "your buffer was replaced" signal: the addon's
`RefreshLiveManifest()` now sends its cached `access_token`, and a
mismatch against the tracked state answers `fatal: true` without tearing
down the (healthy, someone else's) current buffer. Covered by retargeted + new re-add
tests, the stale-token unit cases, and a `Plugin.run()`-level
no-teardown test; the addon-side param stays on the untested
`DispatcharrClient` side of the boundary.

`timeshift_buffer` 0.6.6 and `recording_edl` 0.2.1 (2026-10-01; unit-tested, live confirmation tracked in
`docs/OPEN_ITEMS.md`): `_clear_channel_dir()` empties a leftover channel directory on every fresh start
(a stale `live.m3u8` with high sequence numbers used to stall a new viewer, and an old directory mtime
exposed it to the reaper's orphan scrub); `_teardown_buffer()` re-reads the state and returns False without
touching anything if Redis now holds a different `pid`/`started_at` (a stale teardown used to destroy a
concurrent restart); `_create_http_server()` prefers a dual-stack `::` socket and falls back to IPv4 where
there is no IPv6 (covered with real loopback connections, skipped without IPv6 loopback);
and `recording_edl`'s `_dvr_hls_staging_scan_roots()` adds every DVR path-template root to the default
`/data/recordings` for the `.dvr_<id>_hls` listing/deletion, because Dispatcharr creates the directory beside
the recording's final file. New tests fail on the previous versions (13 for `timeshift_buffer`, 5 for
`recording_edl`).

**`tools/`** (`tools/tests/`, pytest, same `unit-tests-python`/`lint` CI
jobs as the two plugins): `tools/check_release_zip.py` (added 2026-10-07), the privacy gate every
release zip passes whoever built it, is tested against synthetic zips for each rule (a build-machine path as
text and as UTF-16 at both alignments, a blocklist term, an extra field, a zip comment, an unsafe entry name,
a debug file, an unstripped ELF built byte by byte, the release-set check, the allowlist being scoped to its
own members, and fail-closed without a blocklist) and by 16 deliberate mutations of the script, all caught;
it fails the real `0.11.0` Android zip that shipped the build machine's paths. `tools/scrub_zip_paths.py` (same
day) blanks the build machine's directory prefix inside a zip's members (each binary string containing it becomes `/build`
plus NULs, same size; a text member has the prefix overwritten) -- the
Android libraries statically link nghttp2 and OpenSSL, which carry their own build paths as string data that
stripping cannot remove -- and is tested for length, offsets, modes and members preserved and (eighteen mutations
caught) for a prefix too short or relative, a string that is not plain text and an absurdly long one all being refused; the gate, not the scrub, is what decides. `tools/check_doc_refs.py` itself -- the
doc-citation checker described below -- has its own full test suite,
since it's real parsing/matching logic wired into CI, not just a
one-off script. Covers the module's own text-normalization and
markdown-heading-extraction helpers (both plain `#` headings and this
project's bold-pseudo-heading convention), all three `check_*()`
functions against synthetic per-test docs/src/config (never this
repo's own real ones), its on-disk baseline load/save round-trip, and
its overall exit codes/`--update-baseline` handling/resolved-entry
reporting. Includes regression tests for bugs this checker's own module
docstring already documented as found-and-fixed during its development
(the multi-line file-lookback within a citing paragraph, the
`[X.md](X.md)`-style markdown-link citation form, and the earlier
gap that left both plugins' `plugin.py` files out of the function-name
corpus) -- previously fixed with no test locking any of them in.

`tools/kodi_smoke_test.py` (added 2026-09-29, fixing several real,
confirmed bugs found via a project-wide review -- see
`docs/OPEN_ITEMS.md`'s own entry) also gained a `tools/tests/`
suite of its own, for the same reason: this manual live-test harness's
own selection/lookup/arithmetic logic (which broadcast to pick, which
new timer(s) a create actually produced, whether a forward seek target
is still positive, matching a `"recording"`-state timer to its own
`PVR.GetRecordings` row) is exactly the kind of pure, Kodi/Dispatcharr-
independent logic this project's own "new pure-logic code gets a test
alongside it" convention already covers -- extracted into plain
module-level functions (`_match_recording_by_title`, `_windows_overlap`/
`_pick_conflict_free_broadcast`, `_select_new_timers`,
`_compute_forward_seek_target`, `_find_matching_recording`) the same way
`check_doc_refs.py`'s own checks are, each fixing a real bug a prior pass
found with no test guarding against a recurrence (a broadcast-selection
fallback that could pick a broadcast already confirmed live to hang Kodi
or fail with -32100; a seek-target formula that could go negative near a
recording's own end; an `endtime > now` heuristic that also matched an
already-finished, early-stopped recording). Also covers
`SmokeTestRun.record()`'s broadened exception handling (an unanticipated
exception from one check must not abort every check after it) and
`DispatcharrApiClient`'s new `ConnectionError`/`json.JSONDecodeError`
wrapping (matching `JsonRpcClient`'s own, already-confirmed-live
equivalent), via `monkeypatch`ing `urllib.request.urlopen`. The script's
own end-to-end behavior against a real Kodi/Dispatcharr instance stays
exactly as untested as before -- this is the same boundary as every
other Kodi/Dispatcharr-touching code in this project, not a new
exception to it.

`timeshift_buffer` 0.8.14 (2026-10-09) closed the host-clock-step gap in the reaper: `_IdleTracker` ages each heartbeat
value on a monotonic clock from when it last changed (`_stale_viewers()`, `_drop_stale_viewers()`), and the reaper
no longer subtracts two stamps. The tests drive real `_reaper_loop()` ticks against a fake monotonic clock
(a test driver): a forward step with an unchanged stamp keeps the buffer, a backward step still reaps an idle one, stamps
that run backwards while the viewer is watching keep it, the first sighting is age zero, the strict `>` boundary, a viewer
pruned while another keeps the buffer alive, a prune write-back that spares a viewer that registered or heartbeated in the
gap, leadership loss clearing what was seen, a vanished buffer being seen afresh, and the teardown's last look comparing
values instead of reading a clock. Mutation-checked: putting the old subtraction back fails five of them. Not covered:
`stop_buffer`'s own viewer prune still compares stamps (it runs in a request worker with no memory between calls), and
nothing runs the reaper thread against a real stepped clock.

2026-10-09: `WebSocketClient` gained a test for a stop during a blocked send (a server that floods pings and never reads fills the
client's pong sends until one blocks; the stop must end it within a slice; skipped on a kernel whose buffers absorb the flood).
`DecideRedirectStep()` (`RedirectPolicy.h`) and `DecideAuthGate()` (`AuthBackoff.h`) were extracted from
`PerformWithSafeRedirects()` and `EnsureAuthenticated()` with table tests that pin the order of the checks: an unsafe redirect
target is refused before the hop limit is looked at, a redirect without a Location is handed back even at the limit, and the
login backoff and the transient cooldown both apply before any refresh or login (the 47th-pass storm). Mutation-checked: swapping
either order fails them. The in-progress segment fetch, the recording open probe and the segment span arithmetic are still inline
(`docs/OPEN_ITEMS.md`).

2026-10-09: `tools/tests/test_time_conversion_guard.py` pins where the saturating and the plain UTC conversions may be called (the
two server-date parsers must saturate, nothing outside `TimeUtil.h` calls the C library's directly, the plain one's uses are
counted), and CI's `unit-tests-32bit` job runs the whole C++ suite built with `-m32`. Its first run caught
`FindEpgEntryIndexCoveringRecording()` failing an exact 80% overlap on x87 (a double ratio against 0.8); the check is integer
arithmetic now (`tests/test_epg_program_match.cpp` already pinned 79%, 80% and 100%).

2026-10-09: the live tail wait is taken in one-second slices (`kLiveTailWaitSliceMs`, `TailWaitEpisode`, `DecideTailWaitOutcome()`,
`HasTailWaitSliceElapsed()` in `CatchUpUtil.h`, unit-tested: episodes continue at one position and restart on a moved position or a
gap, the give-up decision needs the whole budget or a seek probe). The glue harness gained `live_tail_bound` (a frozen live tail: the
longest read, the `-1` retries before the first `0`, the total wait, and the stream resuming), which is how the change was measured
(4515 ms to 1004 ms); it is in CI's glue run. What no test reaches is how ffmpeg reacts to the `-1`s on a slow device
(`docs/OPEN_ITEMS.md`).

See `docs/CLOSED_ITEMS.md`'s "No automated test suite exists" entry for
the full reasoning and what's still open on both sides. Verification of
everything else stays manual: smoke-testing against a real Dispatcharr
instance and real/emulated Kodi installs (Windows, macOS, Linux via Kodi
Flatpak, CoreELEC on an ODROID N2+), driven via Kodi's JSON-RPC webserver.
