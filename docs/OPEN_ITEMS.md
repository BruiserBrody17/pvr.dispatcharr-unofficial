*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**10 open, 438 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One from the Kodi 22 work on `Piers`: what remains of Kodi 22 support.
- One from the 2026-11-17 review of Dispatcharr 0.34.0: plugin actions running in Celery workers.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One upstream note on Kodi from the 2026-11-16 live check of the settings re-delivery: a GUI deadlock after a PVR
  client restart.
- One user report from 2026-11-14: Off-mode live TV ending by itself on an Android tablet.
- One follow-up to issue #1 from 2026-11-16: DVB subtitles and teletext in Server-side mode.
- One observation from the 2026-11-16 live check of the catch-up session cache: Kodi 22 reading two finished
  recordings end to end at start.

Nothing is left from the 2026-10-18 read-only audit of the whole project (28 findings), from the 2026-10-23 sweep
(five read-only reviews by area, 17 findings, the last fixed on 2026-11-04), from the 2026-10-24 sweep
(one reviewer over the whole project, seven findings), from the 2026-10-31 sweep (eleven findings: nine fixed, two
closed without a change, the last on 2026-11-04 by keeping a recording's 20 s retry budget) or from the 2026-11-10
sweep (eleven findings, all fixed: the release gate, the CI path filter and a `timeshift_buffer` regression on
2026-11-10, the addon's three, the glue fake and the `BRANCHES.md` table on 2026-11-12) or from the 2026-11-14 sweep
(six read-only reviews by area, seventeen findings, all fixed: the documentation and tooling findings, the MEDIUM and
the design question on 2026-11-15, the other seven on 2026-11-16).

| Section | Entries |
|---|---|
| Open: Needs a live check | 4 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 3 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 385 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 43 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

#### Dispatcharr 0.34 runs plugin actions in Celery workers, so `timeshift_buffer`'s file server and ffmpeg move there

**Opened 2026-11-17, from a source review of Dispatcharr 0.34.0 against 0.33.0; partly checked live the same day
(below).** Up to 0.33,
`PluginRunAPIView` called `PluginManager.run_action()` in the web worker that took the request. 0.34 queues every `run/`
action as `run_plugin_action_task` on a `plugins` queue and waits for it (`DISPATCHARR_PLUGIN_RUN_SYNC_TIMEOUT_SECONDS`,
default 55 s, then a 504). The default prefork worker now takes that queue as well as `celery`
(`-Q celery,plugins`, `--autoscale`, `docker/uwsgi.ini` and `docker/entrypoint.celery.sh`). `timeshift_buffer` starts its
file server, its reaper thread and each buffer's ffmpeg from inside an action (`_ensure_http_server_running()`,
`_ensure_reaper_running()`, `_start_buffer()`), so all three now live in Celery child processes. To check against a real
0.34 server:
- Start, play, pause, seek and stop a buffer; two viewers on one channel; a stream switch.
- A Celery child exiting: autoscale shrinking the pool after a burst, or the 512 MB recycle
  (`CELERY_WORKER_MAX_MEMORY_PER_CHILD`). Its file server goes with it until the next action starts one in another
  child, and the addon's segment reads fail in between. ffmpeg runs in its own session and should survive.
- Action latency while an M3U or EPG refresh occupies the same worker pool: heartbeats and manifest polls queue
  behind it once the pool is at its maximum (`CELERY_MAX_WORKERS`, default 6). Seen live and fixed in the addon
  (below).
- A deployment that runs Celery in its own container: the file server's port has to be published on that container,
  not the web one. If so, the plugin README's port instructions need a line.
- Side effects to note, not fix: every action is now recorded in the plugin's task history and broadcast to open web
  UIs as `plugin_task_complete`, and every `{"status": "error"}` result logs a failed Celery task.

**Seen live 2026-11-17** (Dispatcharr 0.34.0 in one container, a throwaway read-only probe plugin reading `/proc`, Kodi
22 RC1 playing a disposable channel through a logging forwarder):
- The file server's listening sockets belong to Celery children of the default `-Q celery,plugins` worker, one per
  child that has run a `timeshift_buffer` action (two at rest, four after the pool grew), and to no uwsgi worker; the
  buffer's ffmpeg is a child of the Celery child that started it. In one container the port answers as before.
- From the client's own network, an action took 40 to 70 ms; Celery's routing is not a visible cost.
- **Pool saturation:** twelve parallel 20 s probe actions held all six children for about 40 s. The addon's manifest
  polls then took up to 40 s to be answered and its heartbeats up to 30 s (three timed out at the client's 2 s). Playback
  did not stop (151 segment reads over 4 minutes, all good; segments come from the file server, not Celery), but the
  plugin's idle timeout is 30 s, so a longer saturation could reap a buffer that is being watched. Since fixed in the
  addon for a paused viewer and one waiting at the live edge, with a keep-alive to the file server
  (`docs/CLOSED_ITEMS.md`, "A paused viewer's timeshift buffer can be ended while Dispatcharr's task pool is busy (and
  a viewer at the live edge loses playback)",
  which also has what in Dispatcharr's source can hold the pool that long: bulk guide assignment, not a scheduled
  refresh on its own). A channel opened during such a burst still waits in the queue.
- The pool did not shrink within 3.5 minutes of the burst, so a child exiting under a running buffer was not seen.
- Still open: a seek, two viewers on one channel and a stream switch on 0.34; a Celery child exiting under a running
  buffer (the pool shrinking, or the 512 MB recycle); a deployment with Celery in its own container. Start, play, pause
  and stop were seen on 0.34 (this item, and the live checks of the 500-with-result reading and of the keep-alive).

**0.34.1 reviewed 2026-11-18** (source diff against 0.34.0): it changes only plugin sockets used as context managers,
where a plugin's file-field uploads are stored and how a plugin's data directory is named (`apps/plugins/sandbox.py`,
`apps/plugins/api_views.py`). Nothing above changes: the action queue, its timeout, the 500-with-result answer and the
worker setup are as in 0.34.0, and neither plugin opens a socket with `with`, declares a file field or uses its data
directory.

The 0.34 plugin sandbox does not apply: it is enforced only for a manifest declaring `manifest_version` 2, and ours
declare none (version 0, `apps/plugins/capabilities.py`'s `MANIFEST_SCHEMA_POLICIES`). Its import wrappers are still
installed for every plugin (`apps/plugins/sandbox.py`) but only check permissions, which version 0 always passes. 0.34's
persistent services (`persistent_service`, `network_listener`) are where the file server and reaper would belong long
term; that needs manifest version 1 and Dispatcharr 0.34.0 or later.

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
format setting; if it is added, the fork commit needs the tests updated (the signatures change), the Kodi 22 warm-up
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

#### Kodi 22 reads two finished recordings end to end at start

**Opened 2026-11-16, seen during the live check of the catch-up session cache (`CLOSED_ITEMS.md`, "A catch-up replay 9
to 30 s after the first Play reuses a session Dispatcharr has deleted"); one observation, not chased.** Kodi 22.0-RC1
(Linux) started on the lab profile, with a logging forwarder between the addon and a real Dispatcharr 0.33.0 and
nothing played: within its first three minutes, two finished recordings' `/api/channels/recordings/<id>/file/` were
read from byte 0 to a few GB each, in consecutive 256 KiB `Range` requests (thousands per recording),
while every other finished recording got 3 to 6 requests. 256 KiB is the addon's
`GetStreamReadChunkSize()`, so these are the addon's `ReadRecordedStream()` reads on a stream Kodi opened and kept
reading. Kodi 22's stream-details probe (the thumbnail loader, `docs/API_NOTES.md`) is the likely caller, but which
component kept reading was not checked: a demuxer scanning for a duration or stream it could not find, or Kodi's
file cache reading ahead for as long as the probe held the stream open. Nor was it checked what set those two recordings apart (length, a missing index,
codecs), whether it happens at every start or once per recording, or whether Kodi 21 does the same. Cost on a real
setup: gigabytes from the server's disk and network at each Kodi start, on a client that is only browsing.

**Next:** at a Kodi 22 start, log which thread opens the two recordings and calls `ReadRecordedStream()` (the addon's open logging, Kodi's debug log for
`CVideoThumbLoader` and the cache) and whether `CloseRecordedStream()` comes when the reading stops; compare the two recordings'
`ffprobe` output with a recording that got 3 requests; repeat a start to see whether it recurs; and check Kodi 21 on
the same profile. If the probe is the reader, see whether the addon can cap or refuse a stream-details open (it cannot
tell a probe from Play today; `CLOSED_ITEMS.md`, "Kodi 22's thumbnail loader made the "no file for this recording"
notice pop up while browsing", has how far that got).

### Fix known, not yet done

None open.

### Architectural / concurrency

None open.

### Design decision needed

None open.

### Release, CI and manual testing

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: cut the next `Piers` release from the final without the pre-release mark (`0.14.0-Piers` to `0.14.4-Piers` were cut from RC1 and all carry it) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

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
