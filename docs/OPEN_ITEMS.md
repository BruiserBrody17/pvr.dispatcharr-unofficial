*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**8 open, 429 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One upstream note on Kodi from the 2026-11-16 live check of the settings re-delivery: a GUI deadlock after a PVR
  client restart.
- One from the 2026-11-12 live checks on Kodi 21: timers made on the server showed about 75 s after the realtime event.
- One user report from 2026-11-14: Off-mode live TV ending by itself on an Android tablet.
- One follow-up to issue #1 from 2026-11-16: DVB subtitles and teletext in Server-side mode.

Nothing is left from the 2026-11-14 bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (sixteen of its
seventeen findings held here, the seventeenth, a `BRANCHES.md` note, being `Piers` only; all fixed: the documentation and
tooling findings, the MEDIUM and the design question on 2026-11-15, the other seven on 2026-11-16), from the 2026-11-10
bug-hunting sweep, made on `Piers` and checked against `Omega`'s code (nine of its
eleven findings held here, the other two being `Piers` only; all fixed by 2026-11-12), from the 2026-10-18 read-only audit, made on `Piers` and checked against `Omega`'s code (all 21 findings
held here), from the 2026-10-19 live check of four audit fixes on `Piers`, from the 2026-10-23 sweep, made on `Piers`
and checked against `Omega`'s code (all 17 findings held here, the last fixed on 2026-11-04), from the 2026-10-24 sweep (one reviewer
over the whole project, seven findings), or from the 2026-10-31 sweep, made on `Piers` and checked against `Omega`'s
code (all 11 findings held here; nine fixed, two closed without a change, the last on 2026-11-04 by keeping a
recording's 20 s retry budget).

| Section | Entries |
|---|---|
| Open: Needs a live check | 3 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 3 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 376 |
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

#### `timeshift_buffer`: DVB subtitles and teletext never reach Kodi in Server-side mode

**Opened 2026-11-16, a follow-up to issue #1 (every audio track, fixed in 0.8.18); a local ffmpeg experiment, nothing
live yet.** The buffer's `hls` muxer hands any subtitle stream to a WebVTT muxer that refuses DVB subtitles and
teletext, so 0.8.18 leaves subtitles out (`docs/TIMESHIFT.md`, "Every audio track, not just the first (0.8.18)").
The `hls` muxer replaced the `segment` muxer in 0.8.0 because `segment` restarted every PID's continuity counter at
each file, a "Packet corrupt" at every splice of the addon's byte stream; the docs recorded no flag that stops that.
`segment` with `-individual_header_trailer 0` keeps one MPEG-TS muxer across its files. Measured with ffmpeg 6.1 on a
generated source (video, two audio languages, plus a DVB subtitle or a teletext stream; the raw inputs of the plugin's
own subtitle tests), mapping `0:v:0?`, every usable audio track and `0:s?`, 2 s segments:
- `hls`: the start fails ("Exactly one WebVTT stream is needed").
- `segment`, default: the subtitle stream is in every segment, but the ten files concatenated show 43 continuity
  breaks and ffmpeg reports 3 corrupt packets.
- `segment` with `-individual_header_trailer 0`: the subtitle stream is in every segment, no continuity break, no
  corrupt packet. Each segment still starts with SDT, PAT and PMT and opens on its own.

The addon passes Kodi whatever streams arrive, so only the plugin would change. What `segment` lacks and the plugin
would have to take over from `hls`: deleting segments (it never deletes; `-segment_wrap` recycles names, which 0.8.0
moved away from), and continuing the playlist after a stream-switch restart (`append_list`): a restarted `segment`
writes a fresh list whose `#EXT-X-MEDIA-SEQUENCE` starts at 0 even with `-segment_start_number`, so the sequence would
come from the file name and the plugin would keep the older entries itself. Also to settle: which subtitle streams
can be mapped without failing a start (a text subtitle from an HLS source was not tried).

**Next, live, before building anything:** find a real channel carrying DVB subtitles or teletext (probe through
Dispatcharr's proxy, as for the audio check) and see whether Kodi 21 and 22 show them in Off mode, which proves Kodi
and the proxy's side; rerun the muxer comparison with the container's ffmpeg 8.1.2 through a probe plugin. Then the
muxer change on a branch, with the plugin owning deletion and the restart's playlist, tests, the behaviour review,
and a live check like 0.8.18's (generated and real channels, Kodi 21 and 22, subtitles on through pause, rewind and a
stream switch).

### Fix known, not yet done

None open.

### Architectural / concurrency

None open.

### Design decision needed

None open.

### Release, CI and manual testing

None open.

### Upstream (Dispatcharr) or documentation accuracy

#### Kodi: the GUI can deadlock right after a PVR client restart

**Opened 2026-11-16, from the live check of the settings re-delivery (`CLOSED_ITEMS.md`, "A settings save delivered
while the PVR instance is still being constructed is dropped"); reproduced on Kodi 22 RC1, backtrace taken with gdb.**
Right after Kodi's PVR manager reports "Started" again following a client restart (the NEED_RESTART dialog's OK), Kodi's
main thread can stop for good: it holds the GUI lock while drawing a home-screen PVR widget, reaches a channel with no
guide yet, and `CPVREpgContainer::CreateChannelEpg()` waits in `WaitForUpdateFinish()` for the guide update. That update,
finishing, destroys its `CPVRGUIProgressHandler`, whose thread is waiting for the GUI lock in
`CGUIWindowManager::GetWindow()`. No addon code is on any of the three stacks. It took a restart that brings channels
Kodi had not seen (a client that started unreachable and then connected, with a large lineup):
an ordinary settings save answered with NEED_RESTART hung it in 2 of 6 runs without the re-delivery code and 5 of 10
with it, and 0 of 6 when no addon was enabled during startup, so the timing around it matters. Kodi 21's
`EpgContainer.cpp` has the same wait; on Kodi 21.3 the same restart did not hang in 4 runs without the re-delivery code
and 4 with it, too few to say it cannot. Not this addon's bug; worth reporting to Kodi with the
three stacks. Nothing here works around it.

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
