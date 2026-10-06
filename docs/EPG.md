# EPG data

*(part of the pvr.dispatcharr-unofficial notes -- see [API_NOTES.md](API_NOTES.md) for the index)*

## Confirmed channel JSON fields (`GET /api/channels/channels/`)

The response is a **bare JSON array**, not paginated/wrapped in
`{results: [...]}` (at least on the instance this was checked against) --
`DispatcharrClient::GetChannels()`'s `results`-or-bare-array handling covers
this correctly either way, so no change was needed there.

| Field | Notes |
|---|---|
| `id` | integer, channel's own id |
| `uuid` | string, used in the live-stream URL (confirmed against `/proxy/ts/stream/{channel_uuid}` -- see the endpoint table in [API_NOTES.md](API_NOTES.md)) |
| `name` | string |
| `channel_number` | number -- a float server-side, so whole numbers arrive as `1.0` and a subchannel as `5.1` (see "Fractional channel numbers" below) |
| `channel_group_id` | **bare integer**, not a nested `channel_group` object |
| `tvg_id` | bare string field directly on the channel, not nested under `epg_data` |
| `logo_id` | integer, FK to a separate Logo object -- **there is no `logo_url` field on Channel** (that field exists on the Stream model instead, which is a different object) |

`DispatcharrClient::GetChannels()` already handles the nested-object variants
defensively as a fallback, but the flat/bare forms above are what a real
instance actually returns.

## Fractional channel numbers (subchannels)

`channel_number` is a `FloatField` server-side (`apps/channels/models.py`),
so a channel can be numbered `5.1` as easily as `5` -- the ATSC-style
subchannel convention. Whole numbers come back as JSON floats (`5.0`);
fractional ones as-is.

Dispatcharr's XMLTV export keys `<channel id>` by `format_channel_number()`
(`apps/channels/utils.py`): a whole-valued float renders as an int (`"5"`),
a fractional one as Python's own `str(float)` (`"5.1"`). Confirmed live,
2026-09-30, against three disposable channels numbered 88881, 88881.1 and
88882.5, each mapped to the same real EPG row: the export carried exactly
three distinct ids -- `"88881"`, `"88881.1"`, `"88882.5"` -- each with its
own 424 `<programme>` entries, and the channel API returned `88881.0`,
`88881.1` and `88882.5` (the whole number as a float, as above).

**This addon used to truncate them.** `Channel::channelNumber` was a plain
`int`, and `nlohmann::json`'s `get<int>()` silently truncates a
floating-point value rather than throwing (`FieldOr`'s own null/wrong-type
catch-all never even fires), so `5.1` became channel `5`. Confirmed live on
the same three channels: Kodi listed the second as channel 88881 with
subchannel 0 and the third as 88882, and both 88881 and 88881.1 refused a
guide ("shared by another channel", `FindAmbiguousChannelNumbers()` -- two
channels genuinely did read as the same number). A real 5 / 5.1 pair would
have lost *both* guides, and a lone 5.1 would have been looked up under the
key `"5"` and found either nothing or a different channel's programmes.

`Channel::channelNumber` is now a `double`, and `SplitChannelNumber()`
(`ChannelNumber.h`, unit-tested) derives the three things that need it:

- Kodi's channel number -- the integer part (`SetChannelNumber()`);
- Kodi's sub-channel number -- the digits after the decimal point read as
  an integer (`5.1` -> 1, `5.25` -> 25; `SetSubChannelNumber()`), which
  Kodi displays as `5.1`. Lossy for a leading-zero fraction (`5.05` and
  `5.5` both give 5) and capped at nine digits; neither matters for the
  numbering this exists for, and nothing that has to tell channels apart
  uses it;
- the guide lookup key -- the exact text Dispatcharr wrote into
  `<channel id>`. For a fractional value that is the *shortest decimal
  string that reads back as the same double* (Python's `repr()` guarantee),
  not a fixed precision, and it's formatted through a classic-locale stream
  rather than `printf`/`to_string`, so a Kodi running under a comma-decimal
  locale can't turn `"5.1"` into `"5,1"`.

Everything that used the number as an identity now uses that key text --
`m_epgByChannelNumber`'s lookups in `GetEPGForChannel()` and
`ResolveRecordingBroadcastId()`, `FindAmbiguousChannelNumbers()`, and
`HaveChannelNumbersChanged()` -- so 5 and 5.1 are different channels
everywhere, not just on screen.

**A channel with no number, or a number Kodi can't show (2026-09-30).** The
paragraph this replaces said a null number, or one below 1, had an empty key
and was never looked up -- which left such a channel with no guide at all
although Dispatcharr exports one for it. Confirmed live against two
disposable channels mapped to a real EPG row, one with a null
`channel_number` (PATCHed to null) and one numbered 0: the real export
carried `<channel id="N">` for the first, N being the channel's own **database id**,
as `apps/output/epg.py` does when `format_channel_number()` returns `""` -- and
`<channel id="0">` for the second, each with its 408 `<programme>` entries.
(`ChannelSerializer.channel_number` sets no minimum, so 0 and negatives are
accepted; `format_channel_number(0)` is `0`, not the `""` that triggers the id
fallback.) `Channel::hasChannelNumber` now separates a null number from a
literal 0 (`ChannelParser` reads both as `channelNumber == 0.0`), and
`ChannelGuideKey()` (`ChannelRenumbering.h`) gives each channel the text its
export uses: the number's text for any finite number, including 0, negatives
and values below 1, or the channel's id when it has none. Kodi still shows
such a channel as number 0 -- what Kodi can display and what the export keys
by are now separate questions. After the fix, both channels showed 5 of 5
broadcasts through a real Kodi, where neither had any before.

Because Dispatcharr writes numbers and ids into the same `<channel id>`
space, an unnumbered channel 77 and a channel numbered 77 share a key and are
caught as ambiguous like any other collision, and two channels both numbered
0 are too. Only a number that isn't finite, or is at least 1e15, has an empty
key and is never looked up.

## Rich EPG data (posters, new/premiere/live badges, cast, genre) from XMLTV

Confirmed live against a real instance after a richer XMLTV-backed
guide source was added to Dispatcharr and mapped to a channel: the
`/output/epg` XMLTV Dispatcharr generates already carries all of this per
programme when the underlying EPG source provides it -- it isn't
Dispatcharr-specific or provider-specific, just standard XMLTV
elements this addon wasn't reading yet. `XmlTvParser` originally only
extracted title/sub-title/desc/category(×1)/xmltv_ns episode-num;
`GetEPGForChannel()` only set title/plot outline/plot/genre
description/series/episode number. Extended both to close the gap with
what TVHeadend's own `pvr.hts` shows for the same kind of source:

- `<icon src="...">` (a per-**programme** poster, distinct from the
  channel's own logo) → `PVREPGTag::SetIconPath()`. This is what actually
  produces poster art in Kodi's guide, not the channel icon.
- `<new/>` / `<premiere/>` / `<live/>` (empty presence-only elements) →
  `EPG_TAG_FLAG_IS_NEW` / `_PREMIERE` / `_LIVE` via `SetFlags()`.
  `EPG_TAG_FLAG_IS_SERIES` is set heuristically (season/episode number
  present, or any category text contains "Series") since XMLTV has no
  dedicated series-flag element.
- `<category>` (0+, repeatable -- confirmed live with up to 4 on one
  programme, e.g. `["Series", "Sports non-event", "News", "Sports talk"]`)
  → all joined into `GenreDescription`, plus a best-effort keyword scan
  (`MapCategoriesToGenreType()`, in `EpgTagUtil.cpp` as of 2026-09-13 --
  pulled out of `PVRDispatcharr.cpp` so it's unit-testable standalone, see
  `tests/test_epg_tag_util.cpp`) against Kodi's ETSI EN 300 468
  `EPG_EVENT_CONTENTMASK_*` values so the guide gets
  genre-based colour coding instead of every programme showing as "Other /
  Unknown" -- confirmed live: `["Series", "Sports non-event", ...]` now
  resolves to `Genre: Sports` in Kodi's own EPG info panel, not the
  generic fallback.
- `<credits>` sub-elements (confirmed live: `producer`/`actor` populated
  by this source; `director`/`writer`/`adapter`/`presenter`/`guest`/
  `commentator`/`composer`/`editor` also parsed for robustness even
  though unconfirmed against this particular source) → `director`/
  `writer` get their own fields, everyone else buckets into `Cast`
  (comma-joined, matching `EPG_STRING_TOKEN_SEPARATOR`).
- `<date>` → `Year` (leading 4 digits) and `FirstAired` (verbatim), both
  only when the programme also has a real season/episode number -- see
  the "A `<date>` value can be a series-level placeholder..." entry in
  `docs/RECORDINGS.md` for why episode-less programmes deliberately skip
  both (an earlier pass only guarded `FirstAired`, leaving `Year` to leak
  the same placeholder through as a bare year instead).
- `<sub-title>` → both `PlotOutline` (unchanged, existing behavior) and
  the new `EpisodeName` (confirmed live: e.g. a round or match name for a
  sports broadcast).

**Not mapped, deliberately:** `<star-rating>` (confirmed absent -- 0
occurrences -- across a live multi-megabyte, tens-of-thousands-of-programme fetch from
this source, so nothing to verify against) and parental `<rating>`
(present, but each `system` attribute is a different rating board --
one real ratings board was the one seen live -- with no clean,
non-misleading mapping to Kodi's single integer `ParentalRating` without
a per-system lookup table this wasn't worth building for the first pass).
`<previously-shown>` (present, confirmed) is also not mapped to anything
-- its absence is already implied by not setting `EPG_TAG_FLAG_IS_NEW`,
and there's no dedicated Kodi field for "last repeat date" distinct from
`FirstAired`.

**A real gotcha hit during live verification, worth remembering for next
time:** after rebuilding and redeploying the addon, `PVR.GetBroadcasts`
kept returning the old, pre-change field values (empty cast, `"Other /
Unknown"` genre) for several minutes even though the new DLL was
confirmed loaded (`SECTION:LoadDLL` in `kodi.log`, right size on disk) and
`EnsureEpgLoaded()`'s own staleness check should force a fetch on a fresh
addon instance. Root cause: Kodi's *own* EPG database is a separate cache
layer on top of whatever the addon returns, keyed off the channel's
persistent unique id (not the ephemeral per-session `channelid` JSON-RPC
exposes) -- with 7 days of guide already cached from a prior session, its
own `epg.epgupdate` interval (120 min default) meant it didn't feel a need
to re-poll the addon yet. Settings → PVR & Live TV → Guide → **Clear
data** forces an immediate full re-fetch through the addon and is the
reliable way to verify an EPG-mapping change live without waiting out the
interval.

## Background loading

`EnsureChannelsLoaded()`/`EnsureEpgLoaded()` (`PVRDispatcharr.cpp`) used to
be called only lazily, inline, on whichever Kodi-owned thread first asked
after the cache went stale -- meaning a real fetch (channel list +
channel groups, or a full XMLTV guide fetch+parse) could block that
calling thread. Fine on a small install, but confirmed live against a
real, full-size channel instance that this is a genuinely slow operation (~7
seconds) worth moving off Kodi's calling thread.

Fixed with `StartChannelEpgRefreshThread()`, a background thread (started
in the constructor, joined in the destructor -- same pattern as the
recording refresh thread documented in `docs/RECORDINGS.md`) that calls
the same two functions itself, immediately on start and then every 10
minutes (`kChannelEpgRefreshCheckMinutes`, not user-configurable -- it's
just how often the *check* happens, not how often a real fetch happens;
that's still governed by `channel_refresh_hours`/`epg_refresh_hours` as
before). `EnsureChannelsLoaded()`/`EnsureEpgLoaded()` themselves are
unchanged other than now returning `bool` (did this call actually
perform a fetch, vs. find the cache still fresh) -- Kodi's own calling
threads still call them directly and still get a correct answer either
way, so a fetch racing the very first moment after construction (before
the background thread's first pass completes) still works exactly as
before, just no longer the common case. When a background pass finds the
cache stale and actually refreshes it, it also calls
`TriggerChannelUpdate()`/`TriggerChannelGroupsUpdate()`/`TriggerEpgUpdate()`
(once per known channel -- confirmed against kodi-dev-kit's `PVR.h` that
there's no bulk/whole-guide trigger) so Kodi picks up the change promptly
rather than waiting out its own separate `epg.epgupdate` polling interval
mentioned above.

**Changed 2026-10-03: Kodi's own threads no longer call `EnsureEpgLoaded()`
at all.** `GetEPGForChannel()` calls `RequestGuideFetchIfWanted()` instead,
which wakes the background thread when `dispatcharr::ShouldAttemptGuideFetch()`
(`Staleness.h`) says a fetch would run now, and answers from the cache
(`PVR_ERROR_SERVER_ERROR` until the first guide has landed, after which the
per-channel `TriggerEpgUpdate()` pass above fills Kodi in). Before this, a Kodi
thread that found the guide stale either fetched it itself or waited on the
fetch in progress, and Kodi stops its PVR manager and EPG threads before it
destroys the addon instance, so a stalled guide download held a Kodi exit for
the whole guide timeout -- reproduced live, see `docs/OPEN_ITEMS.md`'s "Kodi's
own threads waited on the guide fetch, and two transfers could not be aborted at
shutdown". The channels fetch is still made on Kodi's threads (`GetChannels()`
must answer synchronously), bounded by `timeout`.

**Confirmed live**, debug logging enabled: `kodi.log` showed the
background thread's own log lines
(`background thread refreshed channels/groups`, then
`background thread refreshed EPG`) on a distinct thread id from both the
addon's construction (`creating PVR client instance`) and the realtime-
update thread, completing a few seconds after startup with no gap in
responsiveness elsewhere in Kodi. Verified the actual data too, not just
that the calls didn't error: `PVR.GetChannels` returned the full real
channel lineup and `PVR.GetBroadcasts` against a normal channel (not one of the
event-placeholder ones, which have no programming to
return) came back with a full day-plus of real programme data.

**Update (2026-09-26, a 25th-pass audit): "Kodi's own calling threads
still call them directly and still get a correct answer either way"
above missed a real gap, found via a project-wide review, not itself
independently reproduced.** A "correct answer" from `EnsureChannelsLoaded()`
itself (the return value, and this addon's own cache) was never the
whole story -- `TriggerChannelUpdate()`/`TriggerChannelGroupsUpdate()`
were only ever called from the background thread's own loop, not from
`EnsureChannelsLoaded()` itself. If a Kodi-owned thread (most commonly
`GetEPGForChannel()`, called on Kodi's own EPG-update thread) happened
to notice stale channel data and trigger a real fetch *before* the
background thread's own next 10-minute check got there first, that
fetch's successful commit updated this addon's own cache with no way
for Kodi to ever find out about it -- Kodi kept showing the stale
lineup (added/removed/renamed/renumbered channels) until a later
refresh the background thread happened to win instead, routinely a
full `channel_refresh_hours` (12h by default) away, not the next 10
minutes as the background-thread-only framing implied. Fixed by moving
both triggers into `EnsureChannelsLoaded()` itself, firing on every
successful commit regardless of which thread caused it (both are
fire-and-forget and safe to call from any thread, confirmed against
Kodi's own real source: `CPVRClient::cb_trigger_channel_update` just
schedules a Kodi-manager-side update for its own next main-loop
iteration) -- removed as redundant from the background thread's own
loop, which still logs its own debug line but no longer calls either
trigger directly.

## A channel renumbering could still show the wrong guide if the EPG side noticed first

Found via a project-wide review (a 26th-pass audit), confirmed against
Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- stronger than the API shape
alone, not the same standard as a live test), not reproduced live.

The 23rd/24th-pass `HaveChannelNumbersChanged()` fix (see
`docs/OPEN_ITEMS.md`'s history and this addon's own `ChannelRenumbering.h`)
only covers the ordering where the *channel* side notices a renumbering
before the EPG side does. The opposite ordering was still a gap:
`channel_refresh_hours` defaults to 12h, `epg_refresh_hours` to 4h, so
after a real server-side renumbering (Dispatcharr's own "compact
numbering" scheme closing a gap after a channel is hidden), the very
next XMLTV refresh is far more likely to land *before* the next channel
refresh, not after. That new XMLTV is already keyed by Dispatcharr's
own new `effective_channel_number`s (`apps/output/epg.py`), but
`m_channels` -- and therefore `GetEPGForChannel()`'s own lookup key --
still held the *old* numbers, so a renumbered channel served its former
neighbor's guide for however long the channel side stayed stale
(routinely most of a 12h window), with no realtime-update rescue
available either (Dispatcharr sends no WebSocket event for a
renumbering at all).

Fixed by having `EnsureEpgLoaded()` force a channel refresh
(`EnsureChannelsLoaded(/*forceStale=*/true)`, see `dispatcharr::
ShouldFetchChannels()`'s own new parameter, `Staleness.h`) immediately
before its own XMLTV fetch, not after -- committing the XMLTV first and
only discovering a renumbering afterward would just throw the
freshly-committed EPG away and force a second, redundant fetch. The
forced channel refresh bypasses only its own staleness check, not its
failure backoff, and a failed forced fetch doesn't block the XMLTV
fetch that follows it. Side effect worth knowing: channels now
effectively refresh at least as often as the EPG does (every
`epg_refresh_hours`, not just `channel_refresh_hours`) -- a real
increase in background API calls at the default 4h/12h split, judged
worth it against silently wrong per-channel guide data for hours at a
time. Also hardened the residual case where the forced re-fetch itself
fails or is delayed: `EnsureChannelsLoaded()`'s own renumbering-detected
branch now clears `m_epgByChannelNumber` outright (not just its own
loaded-at timestamp), so a renumbered channel shows no guide in that
window rather than continuing to serve the wrong one -- confirmed
against Kodi's own real source that its own EPG update merges entries
and keeps a channel's already-cached tags when this addon returns none
for it, rather than blanking the display.

**A manual renumber inside the server's guide cache (fixed 2026-10-02, confirmed live).** Dispatcharr keeps its generated XMLTV in a 300-second chunk cache that a manual channel-number edit does not invalidate, so even with the ordering above right, the guide fetched straight after a detected renumbering could still be keyed by the *old* numbers (a fetch 70 s after a renumber still listed the
old one; one a few minutes later listed the new one). `EnsureChannelsLoaded()`'s renumbering branch now also sets `m_epgRefetchDueAt` to the detection time plus `kGuideRefetchAfterRenumber` (330 s), `EnsureEpgLoaded()` treats the guide as due then, and the background loop polls at the one-minute interval rather than ten while one is pending. One extra guide download per renumbering. Chosen over
cross-checking each channel's name against the XMLTV `<display-name>`, which would also have caught the duplicate-number case but needs a parse the addon does not have and a look at how reliable that field is. Details and the live run: `docs/OPEN_ITEMS.md`, "Guide cache serves pre-rename channel numbers after a manual renumber".

## Broadcast IDs could collide across a channel's own EPG entries

Found via a project-wide code review, not a live incident.
`GetEPGForChannel()`'s `SetUniqueBroadcastId()` combined the channel id
and each entry's start time as `(channelUid << 16) ^ (entry.startTime &
0xFFFF)` -- masking `startTime` down to its low 16 bits before combining.
Any two programmes on the *same* channel whose start times differed by an
exact multiple of 65536 seconds (~18.2h) produced the identical id.
Given a typical multi-day EPG guide holds a few hundred entries per
channel, birthday-paradox math puts a meaningful chance of at least one
such collision within a single channel's own guide window, and that
compounds across an entire lineup -- not a rare edge case for a populated
guide.

Fixed by combining the channel id (multiplied by a large odd constant --
Knuth's multiplicative hash constant, `2654435761`, chosen over a shift
both to avoid the exact same truncation issue once a channel id exceeds
16 bits and to avoid simple additive collision patterns) with the *full*
`startTime`, not just its low bits. For two entries on the same channel,
this now collides only if their full 32-bit-truncated timestamps are
identical outright -- differing by an exact multiple of 2^32 seconds
(~136 years), rather than 65536.

Confirmed live (Windows): compiles cleanly and the addon reloads
normally. The actual consequence of the original collision (i.e., which
Kodi-side EPG functionality keys off `SetUniqueBroadcastId()` versus
deriving identity some other way instead) wasn't independently traced
through Kodi's own source this pass -- the fix is unambiguously correct
regardless, so it shipped without first pinning down the exact blast
radius of the bug it closes.


## Timers/recordings weren't refreshed once the EPG guide actually finished loading

Found via a project-wide review (a 44th-pass audit), confirmed against
Kodi's own real current SDK source, not itself independently reproduced.

`PVRDispatcharr::GetTimers()`/`GetRecordings()` compute their own
EPG-tag link (`SetEPGUid()`/`SetEPGEventId()`, see `docs/RECORDINGS.md`'s
own entries on `EpgProgramMatch.h`) purely from whatever
`m_epgByChannelNumber` already holds at the moment they're called -- but
at Kodi startup, `CPVRManager::UpdateComponents()` loads timers and
recordings *before* the EPG container itself starts, and this addon's
own XMLTV fetch/parse (a full-guide parse, seconds on a large one) on
the background thread routinely finishes well after that. Nothing
previously refreshed timers/recordings again once the EPG cache
actually did finish loading, so a recording created shortly before its
own EPG entry became available (or one whose start time got clamped,
see `EpgProgramMatch.h`'s own `FindEpgEntryIndexByStartTime()` comment)
kept showing "Record" instead of a recording indicator in Kodi's guide,
and pressing it again created a duplicate recording server-side --
until whatever unrelated timer/recording refresh happened to occur
next, or Kodi restarted. The same gap re-opens after a channel
renumbering clears `m_epgByChannelNumber` outright (see
`EnsureChannelsLoaded()`'s own comment above): that same renumbering
branch also resets `m_epgLoadedAt`, so this is really one underlying
condition ("the EPG cache just went from empty back to populated"), not
two separate cases needing separate handling.

Fixed by having `EnsureEpgLoaded()` capture whether the EPG cache had
never successfully loaded before committing its own fresh parse, and if
so, calling `InvalidateAndTriggerTimerUpdate()`/
`InvalidateAndTriggerRecordingUpdate()` afterward (timers first, per
the ordering `docs/RECORDINGS.md`'s own pass-41/42 entries establish) --
so Kodi re-polls both once real guide data is actually available to
link against, rather than waiting for the next refresh that happens to
occur for an unrelated reason.

A related, second gap in the same area: `StartRecordingRefreshThread()`'s
own background loop wait_for()'d *before* ever doing any real work, so
the very first recurring-rule renewal and timer/recording refresh
didn't happen until a full `recording_refresh_minutes` (default 5, up
to 60 per `settings.xml`) had already elapsed since the thread started
-- unlike `StartChannelEpgRefreshThread()`'s own loop, which always did
its first channels/EPG attempt immediately. Fixed the same way: the
loop's first iteration now skips the wait and runs its refresh work
right away, with every later iteration completely unchanged (including
the `m_recordingRefreshIntervalChanged` skip-without-refreshing
semantics a settings-dialog save already relies on).

## A slow channels/EPG failure defeated its own 1-minute retry gate

Found via a project-wide review (a 46th-pass audit), not itself
independently reproduced.

`EnsureChannelsLoaded()`/`EnsureEpgLoaded()` both capture `now` once, at
entry, before their own network call runs -- and, on a failure, stamped
`m_channelsLastFailedAt`/`m_epgLastFailedAt` with that same *entry-time*
`now`, not a fresh timestamp taken when the failure actually happened.
For a fast failure (connection refused, an immediate 4xx/5xx) the
difference is negligible. For a genuine *slow* failure -- a real
network timeout, or a large-guide transfer that never completes -- it
isn't: `GetXmlTvGuide()`'s own CURL timeout is `timeoutSeconds * 4`
(120s by default), double `kChannelEpgFailureRetryMinutes` (60s) even
at default settings, and `GetChannels()`'s own timeout
(`timeoutSeconds`, 30s default) can exceed it too once that setting is
raised. Stamping the failure with the stale, pre-call `now` meant
`dispatcharr::IsRetryDue()` (`Staleness.h`) saw the gate as already open
the instant the failure was even recorded -- completely defeating the
gate's own purpose for the exact case it exists to throttle (a
repeated, slow-to-fail attempt), while happening to still work for a
fast one.

Fixed by taking a fresh `std::chrono::steady_clock::now()` at each
failure site instead of reusing the entry-time value. The same mistake
was caught and fixed the same pass in `DispatcharrClient::EnsureAuthenticated()`'s
own new transient-login-failure cooldown (see `CLAUDE.md`'s own
`AuthBackoff` entry) before it ever shipped, once this pattern had
already been identified here.

**Update (2026-09-27, a 47th-pass audit): one failure site of this
exact same pattern was missed in the fix above, found via a project-wide
review, not itself independently reproduced.** `EnsureChannelsLoaded()`'s
own `m_groupsLastFailedAt = groupsOk ? {} : now;` line still stamped
the entry-time `now`, not a fresh timestamp -- `GetChannelGroups()` runs
its own primary-then-fallback HTTP attempts in sequence, each with its
own CURL timeout, so a slow groups failure alone can already exceed
`kChannelEpgFailureRetryMinutes` before the channels fetch's own time is
even added, defeating `ShouldFetchChannels()`'s own groups-retry gate
the same way the other three sites already had fixed. Fixed the same
way: a fresh timestamp taken at the point of failure.

## The XMLTV fetch was blocked by login backoff even though the endpoint needs no login

Found via a project-wide review (a 48th-pass audit), confirmed against
Dispatcharr's own real current upstream source, not itself independently
reproduced.

`GetXmlTvGuide()` called `EnsureAuthenticated()` first, but the request
it then makes never sends an `Authorization` header at all -- confirmed
against Dispatcharr's own real current upstream source that `/output/epg`
(`apps/output/views.py`'s `epg_endpoint()`) is a plain Django view gated
only by its own "M3U / EPG Endpoints" network-access policy, with no
login requirement whatsoever (the surrounding code already documents
that policy's own local-network-only default, see the 403 handling just
below this fetch). So login being currently backed off -- up to 30
minutes for the credential backoff, or the shorter transient cooldowns
`AuthBackoff.h` describes -- needlessly blocked a guide refresh this call
never actually depended on. Each blocked attempt also stamped
`m_epgLastFailedAt` (`EnsureEpgLoaded()`, `PVRDispatcharr.cpp`), further
delaying the *next* attempt for a reason unrelated to the guide fetch
itself.

Fixed by dropping the `EnsureAuthenticated()` call entirely --
`EnsureEpgLoaded()`'s own retry gate already throttles how often this is
attempted regardless, independent of login's own state.

## A rejected guide fetch now backs off; two refreshes fetch once

Two changes to `EnsureEpgLoaded()`/`EnsureChannelsLoaded()` (2026-09-30),
closing two entries in `docs/OPEN_ITEMS.md`.

**Retry interval for a durable rejection.** `/output/epg` is the one
endpoint gated by Dispatcharr's "M3U / EPG Endpoints" network-access policy, and
its view (`epg_endpoint()`, confirmed against Dispatcharr's own upstream source)
writes an `epg_blocked` system event on every 403 -- which also fires any
configured integration and trims the capped event history. The flat one-minute
retry (`kChannelEpgFailureRetryMinutes`) therefore turned one misconfigured
client into a steady stream of those events. A 4xx other than 408 now doubles
the retry interval from that minute up to 30 (`ComputeEpgFailureRetryInterval()`,
`Staleness.h`), while a transport failure, a 5xx or a 408 stays on the flat
minute: those mean "unreachable or restarting", and a server that has come
back should be asked again at once. That is also why this doesn't undo the
startup-recovery behavior described in `CLAUDE.md` for
`ShouldUseShortChannelEpgRefreshWait()`: that short wait is about channels, and
an unreachable server never counts as a durable rejection. The count resets on
a success and on a wake from sleep (the network may be a different one).

Confirmed live with a proxy in front of Dispatcharr answering `/output/epg`
with 403 from a fresh Kodi start: attempts at 0, 60, 180, 420 and 907 seconds,
each failure logging how many rejections in a row and the wait that follows.

**One fetch at startup.** The background refresh thread and Kodi's own EPG
thread (through `GetEPGForChannel()`) could both find the guide stale at the same
moment and each download and parse it. Each of the two `Ensure*Loaded()`
functions now holds a mutex across its fetch-and-commit; the second caller
waits, re-checks the gate, and finds the first one's result. The cache is stamped
with the *commit* time for this, so a waiter can tell that someone loaded it
after it started. A fresh start against a counting proxy made exactly one
`/output/epg` request.

## Startup-time syncs are retried, and a write Kodi drops is noticed

Four things `PVRDispatcharr` reads from Dispatcharr were tried exactly once, at
construction: the global DVR padding, the admin check, the two catch-up flags
(the timezone sync already had its own retry). A Dispatcharr that wasn't
reachable yet at Kodi startup left each of them at its default for the whole
session. `RetryDeferredServerSyncs()` now runs at the top of every background-
thread cycle (`StartChannelEpgRefreshThread()`: a minute apart while channels
or the guide have never loaded, ten minutes after) and retries whichever hasn't
answered yet; nothing is called once each has. It is retry-until-first-success,
not periodic -- a change made on the server afterwards is still only picked up
at the next restart, as before. A late catch-up answer of "off" also triggers
`TriggerChannelUpdate()`, since `GetChannels()` is what sets each channel's
has-archive flag from it.

**A padding edit that couldn't be pushed is resent, not reverted.** The
startup sync reads Dispatcharr's own value and overwrites Kodi's, so an edit
made while Dispatcharr was unreachable used to be silently replaced the next
time the sync ran. A failed push is now kept as pending
(`m_pendingPrePush`/`m_pendingPostPush`) and resent each cycle, and while one
is pending the sync-from-server is skipped so it can't undo the edit. After
three failed retries it is dropped with a notification, since a non-admin
account's push is refused every time. Confirmed live: with the server
unreachable, the padding edited in Kodi's own settings dialog from 1 to 3 logged
"failed to update Dispatcharr's DVR padding (will retry)", and about a minute
after the server came back the real DVR settings on the server read 3 -- and Kodi still
showed 3. (The server value was put back to 1 afterwards.) A restart before a
retry succeeds still loses the edit; nothing is persisted.

**Kodi discards an addon-initiated `SetSetting*()` while this addon's settings
dialog is open** -- confirmed live, not just from source, but only when the
dialog is opened the way a user does (Add-ons -> PVR clients -> Configure).
Opening it with `GUI.ActivateWindow(addonsettings, ...)` leaves the dialog
unbound to the addon (`UpdateSettingInActiveDialog()` compares the dialog's
addon id), so writes land normally and show nothing: an easy way to "test" the
wrong thing. With the real dialog open, a timezone write from the retry was
dropped, `settings.xml` kept the old value, and cancelling the dialog
(`ReloadSettings()` from disk) kept it that way until the next retry landed the
write. The writes that latch a "done" flag -- the timezone, the admin flag and
the padding sync -- now read the setting back and report failure if it didn't
take, so the next cycle writes it again. The API-key write in
`PersistApiKeyIfChanged()` was left alone at first, then fixed (2026-10-02,
confirmed live) by remembering what Kodi still stores after a dropped write, so
the old key it re-delivers later is not mistaken for an edit and the real key is
written again by the background retry; see `docs/OPEN_ITEMS.md`.

## A realtime reconnect refreshes timers and recordings

The realtime thread marks "resync on next connect" whenever a session ends or a
connect attempt fails, and on the next successful connect invalidates and
re-triggers timers, then recordings, for anything that changed while nothing was
listening (a recording finishing, a timer edited elsewhere) -- rather than waiting
up to `recording_refresh_minutes`. Not armed before the very first attempt, when
Kodi has just loaded both itself. Confirmed live: the proxy in front of
Dispatcharr went down and up, and the reconnect logged the refresh.

## The guide fetch defers when the channel refresh it must follow did not happen (2026-10-02)

`EnsureEpgLoaded()` forces a channel refresh just before its XMLTV fetch so a server-side renumbering is not committed against old channel numbers. When that forced refresh did not happen -- it failed, or its own failure backoff is running -- the guide fetch used to go ahead anyway and commit the new guide against the stale channel list, the mismatch the force exists to prevent. It now treats the guide attempt as failed too and retries both together on the ordinary retry interval, not counting toward the durable-failure backoff (that tracks the guide endpoint rejecting the addon). Cost, accepted: while channels keep failing the guide is as stale as they are. Not exercised live (it needs a channel fetch that fails while the guide fetch would succeed).

Measured for the still-open catch-up question (`docs/OPEN_ITEMS.md`, "Guide fetch never includes already-aired programmes"): against the real instance `prev_days=0` takes 24 s and `prev_days=1` (about 25% larger) 30 s, with 3, 7 and 30 the same size as 1 -- the source only holds about two days of history.

**The guide fetch now asks for already-aired programmes when catch-up is in use (2026-10-02).** `GetXmlTvGuide()` sends `?prev_days=N` with N from `ComputeGuidePrevDays()` (`EpgTagUtil.h`): the longest catch-up window among channels that offer it, capped at Dispatcharr's own 30, and no parameter when none does. Confirmed live that the request carries it and the returned past programmes reach Kodi; the measured cost is the ~25% extra guide traffic above. Closes `docs/OPEN_ITEMS.md`'s "Guide fetch never includes already-aired programmes".

## A finished refresh on the server triggers a fetch, instead of waiting out the polling window (2026-10-06)

The realtime "updates" socket already carried Dispatcharr's EPG and M3U refresh progress, and this addon dropped it: a guide
refreshed on the server was noticed at the next `epg_refresh_hours` window (4 h by default), a channel list at the next
`channel_refresh_hours` (12 h). Captured live against the lab, with an XMLTV source and an M3U account refreshed on demand
(the same thing their scheduler does every three hours; a passive listener on the socket logged every message):

- **EPG** (`type: "epg_refresh"`, `source: <id>`): `downloading` (progress 0 to 100, with speed and size), then `parsing_channels`
  (progress 0 to 100, ending with its own `status: "success"` and a channel count, a step and not the end), then
  `parsing_programs` (progress 0 to 100, "Staging programs..." with counts), whose last event is `progress: 100,
  status: "success"` with a message and an `updated_at`; Dispatcharr then sent `recordings_refreshed`. About sixty
  events in a minute for a large source.
- **M3U** (`type: "m3u_refresh"`, `account: <id>`): `processing_groups` (`status: "fetching"`), `parsing` (progress climbing to
  100 with `status: "parsing"` and `streams_processed`), one more `parsing` event with `status: "success"` carrying
  `streams_processed/created/updated/stale/deleted` and **`channels_created/updated/deleted/failed`** (the channel auto-sync's
  own result is part of this event), then `vod_refresh` with `status: "processing"`. About ten events in a few seconds.
- Every event is `{"type": "update", "data": {...}}` on the same socket the recording events use; nothing needs subscribing to.

`ClassifyRefreshEvent()` (`RefreshEvents.h`) picks out the two final events: `parsing_programs` at progress 100 with
`status: "success"` (a guide parsed) and `parsing` with `status: "success"` whose channel counts show a created, updated or deleted
channel, or do not say (a channel sync that changed nothing, the usual scheduled refresh, costs nothing). Everything else, a
failure included, is ignored. The two reactions:

- A parsed guide schedules one more guide fetch **330 s later** through the schedule a channel renumbering already uses
  (`m_epgRefetchDueAt`): Dispatcharr caches the exported guide for 300 s and a refresh does not invalidate it, so an earlier fetch
  is served the old guide and costs a full download. A second source finishing meanwhile keeps the pending time, so at most one
  guide fetch starts per window however many events arrive.
- A channel-changing M3U refresh ages the channel list to just-stale (never to the "never loaded" sentinel) and wakes the
  background thread, which fetches it: the lineup-change gate then decides whether Kodi is told, and a renumbering found by
  that fetch schedules its own guide refetch.

Confirmed live (a real Kodi on the Linux test VM against the lab): a source finishing parsing was logged at once, the guide was downloaded again 392 s later (330 s plus the background thread's one-minute check while a fetch is pending), and an M3U refresh finishing had the channel list fetched 11 s later, which found a lineup change and resynced Kodi.

The glue harness's fake Dispatcharr speaks a minimal realtime socket (`/__ctl?ws_push=`), and `refresh_events` checks the M3U
half end to end (a refresh that changed nothing causes no fetch, one that created channels causes one within seconds).
