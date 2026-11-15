*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**14 open, 421 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One from the 2026-11-12 live checks on Kodi 21: timers made on the server showed about 75 s after the realtime event.
- One user report from 2026-11-14: Off-mode live TV ending by itself on an Android tablet.
- Eight from the 2026-11-14 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (all LOW; the
  seventeenth, a `BRANCHES.md` note, is `Piers` only). Its three documentation findings that held here, its four
  tooling findings and its one MEDIUM were fixed on 2026-11-15.

Nothing is left from the 2026-11-10 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (nine of its
eleven findings held here, the other two being `Piers` only; all fixed by 2026-11-12), nor from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code (all 21 findings
held here), from the 2026-10-19 live check of four audit fixes on `Piers`, from the 2026-10-23 sweep, made on `Piers`
and checked against `Omega`'s code (all 17 findings held here, the last fixed on 2026-11-04), from the 2026-10-24 sweep (one reviewer
over the whole project, seven findings), or from the 2026-10-31 sweep, made on `Piers` and checked against `Omega`'s
code (all 11 findings held here; nine fixed, two closed without a change, the last on 2026-11-04 by keeping a
recording's 20 s retry budget).

| Section | Entries |
|---|---|
| Open: Needs a live check | 2 |
| Open: Fix known, not yet done | 7 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 368 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 43 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

#### Kodi 21 showed timers made on the server about 75 s after the realtime event

**Opened 2026-11-12, seen once during the live check of the 2026-11-10 sweep's timer-id fix; not investigated.** On
Kodi 21.3 with the `Omega` CI build (realtime updates on, no proxy), a recurring rule and a one-off recording created
through Dispatcharr's API were followed within seconds by `recordings_refreshed` and `recording_updated` realtime
events, which the addon logged; `PVR.GetTimers` still had neither 15 s and 35 s later, and the addon's next "recordings
cache refreshed" and "timer-rules cache refreshed" lines came about 75 s after the events, after which Kodi listed
both. On Kodi 22.0-RC1 with the `Piers` build, the same steps showed the timers within 15 s. The handler calls
`InvalidateAndTriggerTimerUpdate()` (caches cleared, then `TriggerTimerUpdate()`), so the addon asked at once; the gap
is in when Kodi 21 next called `GetTimers()`. Unconfirmed lead: it was about a minute after Kodi started, while Kodi was
still importing the guide (its startup log has many `GetEPGForChannel` errors in
the same minute), so Kodi's PVR job queue may simply have been busy. Next step: repeat on Kodi 21 well after startup, with
Kodi's PVR component logging on (`debug.setextraloglevel`, LOGPVR) to see when the timer update job runs; if it is
prompt then, close this as a startup-load effect.

#### Off-mode live TV ends by itself on an Android tablet; an HLS output option was suggested

**Opened 2026-11-14, from a user report in the community chat; not reproduced, waiting on the reporter's Kodi log.**
Two Android tablets on Kodi 21.3, Off mode, the same channel: one plays for hours, the other stops every so often and
the channel has to be reopened. The reporter traced it to Android power saving (changing the power settings did not
help) and found that another client's "Force HLS" option seemed to help, then pointed at a community fork's commit
(on top of `Omega` 0.13.3). That commit adds a "Streaming format" setting
(MPEG-TS, fMP4, HLS) for Off and Local mode: the URL asks for that `output_format`, Kodi is given the matching
mimetype, and fMP4 and HLS go through `inputstream.ffmpegdirect`. Server-side mode was suggested to the reporter; no
answer yet on whether it helped.

**What Dispatcharr does with a client that stops reading (confirmed live 2026-11-14, Dispatcharr 0.33.0, one test
client on an otherwise idle channel).** An MPEG-TS client that stopped reading, its connection left open, kept its
stream through a 20 s and a 45 s pause and caught up afterwards; a 75 s pause was dropped about 50 s in as a ghost
client, after which the channel stopped (it had no other client) and the connection reached EOF once the buffered data
was read. An HLS client that stopped polling survived 8 s; after 15 s and 30 s it had been dropped about 12 s in, and
its next playlist request got 410 `Session expired`. Both match the source (`apps/proxy/live_proxy/client_manager.py`,
`_ghost_timeout_for_client()`): TS and fMP4 clients get `CLIENT_HEARTBEAT_INTERVAL` x `GHOST_CLIENT_MULTIPLIER`
(5 s x 10), HLS clients `HLS_SEGMENT_DURATION` x `HLS_CLIENT_GHOST_SEGMENTS` (4 s x 3), and the 410 expects the player
to come back through `/proxy/ts/stream/` (`apps/proxy/live_proxy/output/hls/views.py`), which ffmpeg's HLS reader does
not do. So Dispatcharr's HLS output tolerates a paused client a quarter as long as MPEG-TS does. It could still help
where the failure is a short loss of the *connection* rather than a pause: an MPEG-TS stream handed to Kodi is one
connection and ends with it, while an HLS reader opens a new one for every request.

**Next:** read the reporter's Kodi debug log for how playback ends (curl's low-speed abort with no data arriving, a
connection reset, or EOF from the proxy), and whether Server-side mode helped. Only then decide whether to add the
format setting; if it is added, the fork commit needs the tests updated (the signatures change), `Piers`'s Kodi 22 warm-up
taught about HLS (it takes the 302 as a final answer), the setting hidden in Server-side mode, and a decision on making
`inputstream.ffmpegdirect` (an optional import) required in Off mode. If the log shows a dropped connection, ffmpeg's
own HTTP reconnect options through `inputstream.ffmpegdirect` are worth comparing with HLS.

### Fix known, not yet done

#### A catch-up replay 9 to 30 s after the first Play reuses a session Dispatcharr has deleted

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading Dispatcharr's v0.33.0 source.** `CreateCatchupSession()` reuses a cached session for
`kMaxCatchupSessionCacheAge` (30 s), sized against the server's 60 s handshake expiry only. Dispatcharr also deletes a
catch-up session once viewing has genuinely ended: a connection that ran at least 2 s and 64 KiB is followed, 1.5 s
after it closes plus a 5 s grace, by `_finalize_playback_session_auth()` (`apps/timeshift/views.py`), after which the
session's URL answers 401 "Invalid or expired playback session". So playing an aired programme for a few seconds,
stopping, and playing it again between about 9 s and 30 s after the first Play hands Kodi the deleted session's URL, and
every retry in that window does the same. The cache only exists for Kodi's two calls per Play, about 12 ms apart
(`CatchupSessionCache.h`). Fix: an age of about 5 s, or reuse each entry once; note the server's lifecycle in that
header. Not run live.

#### A settings save delivered while the PVR instance is still being constructed is dropped

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading this addon's and Kodi's code.** `CAddonDispatcharr::CreateInstance()` adds the new
instance to `m_instances` only after its constructor returns, and `SetSetting()` delivers a change only to listed
instances, answering OK otherwise. Kodi holds only its instance-creation lock across the constructor, and a settings
save (`CAddonDll::SaveSettings()` then `TransferSettings()`) does not take it, so a save can arrive mid-construction. The
constructor read every setting at its start and can spend a full request timeout in its version check against a host
that does not answer. Scenario: the user saves a wrong host, Kodi restarts the addon, and while the new instance waits
on that host the user corrects it and saves again: no restart is asked for and nothing is applied, so the instance keeps
using the wrong host until the next settings save or a Kodi restart (settings applied live, such as the timeshift mode,
are lost the same way). Fix: after registering the instance, compare the settings with what the constructor used and
apply the live ones; for a changed connection setting, write that setting back so Kodi re-delivers it and the existing
check answers `ADDON_STATUS_NEED_RESTART`. Or queue deliveries that arrive during construction and replay them.

#### Kodi's connection notice names the configured server, not the one in use

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading.** `GetConnectionString()` and `GetBackendHostname()` read the saved `host` and `port`
settings, not the address the client is using, so while an instance is still on an older address (a restart not yet
done, or the window in "A settings save delivered while the PVR instance is still being constructed is dropped") Kodi's
connection-state notice and Event Log name a server the addon is not talking to, which points the user at the wrong
cause. Fix: build both from `m_client.GetConnectionSettings()`, as the realtime thread does.

#### Two series rules can share one Kodi ClientIndex

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced with the real header.** `ComputeSeriesRuleClientIndex()` (`TimerIdentity.h`) is
`std::hash` of (title, `tvg_id`, `epg_source_id`) cut to 30 bits, with nothing to detect two rules landing on the same
value, and `FindSeriesRuleIndexByClientIndex()` returns the first match. With libstdc++ (fixed seed) the titles
"Show 13765" and "Show 57079" with no `tvg_id` and no source collide on 64-bit, "Show 1864" and "Show 53313" with `-m32`
(found after about 55,000 titles, the 30-bit birthday bound). Two colliding rules show in Kodi under one ClientIndex, and
an edit or delete of the second (`UpdateTimer()`, `DeleteTimer()`) acts on the first. Unlikely by chance (about
N^2 / 2^31 for N rules), but anyone who can create rules can pick such a pair. Fix: have the lookup answer -1 when more
than one cached rule matches, so the edit or delete is refused rather than misapplied, with the pair above as a test; or
probe to the next free index, in list order, when the rules are reported.

#### Pending recording titles are pruned only when an untitled recording arrives

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading.** `CreateOneTimeRecording()` appends to `m_pendingTitles` for every titled recording it
creates, but `PruneExpiredPendingTitles()` runs only in `ParseRecordingJson()`'s branch for a recording that arrived
without a title. Since the guide-based create sends the programme's fields, Dispatcharr returns titled recordings, the
prune never runs, and the list grows by one small entry per recording for the life of the process. Fix: prune before the
append as well.

#### `timeshift_buffer`: a last viewer's stop can lose to a stream-switch restart and leave ffmpeg running

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, reproduced with a pytest against the plugin.** In `_stop_buffer()`'s per-viewer path, the last viewer's
stop writes the empty viewer list and then calls `_teardown_buffer()`. If a stream-switch restart in another worker adopts
its new ffmpeg (`_restart_ffmpeg_locked()`) between the two, the teardown is declined as "replaced", the stop answers
"viewer removed; buffer still active for other viewers", and the new ffmpeg keeps its proxy connection with no viewers
until the reaper's idle check ends it (`idle_timeout_seconds` plus up to one 15 s tick). `_teardown_following_a_restart()`
was written for exactly this race but covers only a stop without `viewer_id` and stop-all. The window is one Redis round
trip, but a user who sees the picture held during a switch is likely to press Stop just then. Repro: a test that injects
the adoption inside `_teardown_buffer()` ends with no viewers, the new pid and `_stop_ffmpeg()` never called. Fix: when
the teardown answers False, read the state again and, if it was restarted from the instance just stopped and has no
viewers, tear it down once, as `_teardown_following_a_restart()` does.

#### `timeshift_buffer`: a viewer re-added after a prune loses its Kodi version

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading.** `_record_viewer_kodi_version()` drops the version of every viewer not currently
listed whenever any viewer starts. A viewer the reaper pruned (its polls failed for longer than `idle_timeout_seconds`)
loses its entry if another device starts the same channel meanwhile, and its next poll re-adds it through
`_apply_heartbeat()` without one, because the addon sends `kodi_version` only with `start_buffer`. From then on
`_stream_switch_needs_hold()` counts it as unknown, so a Kodi 21 viewer gets the stream-switch hold that addon 0.13.4 was
meant to spare it. Fix: drop entries only on a stop and on the reaper's prune, or have the addon send `kodi_version` on
its heartbeat and manifest polls and have `_apply_heartbeat()` record it.

### Architectural / concurrency

None open.

### Design decision needed

#### `timeshift_buffer` keeps running in Dispatcharr's other workers after it is disabled or deleted

**Opened 2026-11-14, from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading Dispatcharr's v0.33.0 source.** Disabling or deleting a plugin calls its `stop()` only in
the worker that served the request (`apps/plugins/api_views.py`); the loader tells no other worker. The plugin's other
workers clean up old threads only in `_stop_orphaned_threads()`, on the first `run()` of a fresh import, and a disabled or
deleted plugin is never run. So until Dispatcharr restarts, every other worker keeps its file-server listener on the
segment port (answering 403 and 404, and keeping the port from anything else) and its reaper thread, which goes on
electing a leader and scrubbing the old `storage_path` every 15 s. The README says to restart Dispatcharr after an
update, not after disabling or deleting. Options: document the restart for both (cheapest), or have each reaper tick
check a Redis marker that `stop()` sets for a disable or delete and shut that worker's threads down. Same class as the
per-worker deploys recorded in `TIMESHIFT.md`.

### Release, CI and manual testing

None open.

### Upstream (Dispatcharr) or documentation accuracy

#### Dispatcharr schedules a recurring rule's occurrence that starts inside a spring-forward gap to end before it starts

**Opened 2026-11-01, from the live check of the display item it closed (`CLOSED_ITEMS.md`, "A recurring rule starting
inside a spring-forward gap on its first day is shown ending before it starts"); confirmed against Dispatcharr 0.33.0.**
`sync_recurring_rule_impl()` (`apps/channels/tasks.py`) builds both times with `timezone.make_aware(datetime.combine(
...), tz)` on a `ZoneInfo` and rolls the end over only `if end_dt <= start_dt`. Two aware datetimes sharing one
`tzinfo` compare by wall clock, so a 02:30 that does not exist is still "before" 03:15 and nothing rolls over; stored,
02:30 takes the offset before the change. A disposable rule from 02:30 to 03:15 on the next spring-forward day of a
zone whose clocks go from 02:00 to 03:00 got an occurrence stored ending 15 minutes before it starts. What the recorder then
does with it
was not run (it is months away). Not this addon's bug, and only a rule whose start falls in the skipped hour on a
change day. Worth reporting to Dispatcharr: compare in UTC, or normalise the start out of the gap first.

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
