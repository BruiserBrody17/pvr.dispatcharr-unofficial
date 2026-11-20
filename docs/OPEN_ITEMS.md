*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**10 open, 443 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One from the Kodi 22 work on `Piers`: what remains of Kodi 22 support.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One upstream note on Kodi from the 2026-11-16 live check of the settings re-delivery: a GUI deadlock after a PVR
  client restart.
- One user report from 2026-11-14: Off-mode live TV ending by itself on an Android tablet.
- One follow-up to issue #1 from 2026-11-16: DVB subtitles and teletext in Server-side mode.
- Two from the 2026-11-18 live checks: Dispatcharr 0.34 leaking each plugin action's Redis connections, and Dispatcharr
  giving two overlapping recordings of one programme the same file.

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
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 1 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 5 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 388 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 45 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

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

### Fix known, not yet done

#### `timeshift_buffer`: DVB subtitles and teletext never reach Kodi in Server-side mode

**Opened 2026-11-16, a follow-up to issue #1 (every audio track, fixed in 0.8.18), from a local ffmpeg experiment;
checked live 2026-11-20 (below) and moved here from "Needs a live check".** The buffer's `hls` muxer hands any subtitle stream to a WebVTT muxer that refuses DVB subtitles and
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

**Checked live 2026-11-20 (Dispatcharr 0.34.1; Kodi 21.3 with addon 0.13.3, Kodi 22 RC1 with 0.14.4, both in Off
mode).**
- *No real channel used.* The check needed known subtitle and teletext streams, so it used a
  generated channel: H.264, two audio languages, a DVB subtitle stream (ffmpeg's
  `dvbsub` encoder over a teletext page rendered by libzvbi) and an EBU teletext stream with subtitle page 888, each
  showing a changing counter, served at its own bitrate as a disposable channel behind the default Proxy profile.
- *The proxy passes both through.* Probed and decoded through `/proxy/ts/stream/`: both subtitle PIDs present and
  both still decode (the DVB bitmap renders, page 888's text reads back).
- *Kodi, Off mode, the same on 21 and 22.* Both audio languages are offered, and one subtitle: the DVB stream, which
  shows on screen when turned on. Teletext is not offered as a subtitle; `VideoPlayer.HasTeletext` is true and Kodi's
  teletext viewer decodes page 888 (its header; the boxed row of this generated page was not drawn there, and the
  viewer stopped refreshing the header, not chased). So what a subtitle-carrying buffer would give a viewer is the
  DVB subtitles in the subtitle menu and teletext in the teletext viewer, as Off mode does.
- *The muxer comparison in the container.* Dispatcharr 0.34.1 ships ffmpeg 9.0.2 (not 8.1.2), checked with a probe
  plugin running the comparison over 40 s of the generated source, mapping both subtitle streams: `hls` still fails
  the start ("webvtt muxer supports only codec webvtt for type subtitle"); `segment` keeps both subtitle PIDs in all
  20 segments with 97 continuity breaks across them; `segment` with `-individual_header_trailer 0` keeps both in all
  20 with none. ffmpeg 6.1 on the 5-minute source gave the same split (144 breaks over 30 segments, and none).

**Next:** the muxer change on a branch, with the plugin owning deletion and the restart's playlist, tests, the
behaviour review, and a live check like 0.8.18's (the generated channel; Kodi 21 and 22; subtitles on through pause,
rewind and a stream switch). A real subtitled channel stays worth one look.

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

#### Dispatcharr gives two overlapping recordings of one programme the same output file

**Opened 2026-11-18, from closing "Kodi 22 reads two finished recordings end to end at start" (`CLOSED_ITEMS.md`); seen
on the lab's server (recorded under 0.33.0) and read in the `v0.34.1` source.** `_build_output_paths()`
(`apps/channels/tasks.py`) renders a recording's path from the DVR templates, and avoids a clash only with an MKV that
already exists and is not empty when the recording starts. The MKV is written by the remux at the end, so two
recordings running at once whose templates render the same path both get it. Here it was one programme on two
channels, started within 2 ms of each other, through the TV fallback template, whose file name is the start time. Both
remuxes reported success, and left one file of tens of GB that ffmpeg reads with structural
errors and two of each stream; both recordings list that one file. Not this addon's bug (it plays what the server
serves), but one of the two recordings is lost outright, and Kodi 22 reads a third of the damaged file to make a
thumbnail. Worth reporting to Dispatcharr: reserve the path when a recording starts (an empty placeholder would not be
enough, since the check skips empty files), or add the recording id to a name that clashes with a running recording's.

#### Dispatcharr 0.34 leaks two Redis connections per plugin action, so Server-side mode stops working after about 2,000 actions

**Opened 2026-11-18, during the live check of the Celery item above (its "Checked live 2026-11-18"); measured against
Dispatcharr 0.34.1, the cause read in its source and a server traceback, not confirmed in Celery's own code.** Partway
through that check, with two viewers on one buffer, `run/` calls for every plugin (`timeshift_buffer` and a probe
plugin alike) began failing with Django's bare HTML 500: at first now and then, and within about three minutes every
time. Other API calls kept working, and a plugin import got a 502. The server's log has `OSError: [Errno 24] Too many
open files` in the uwsgi workers, raised while kombu opened a new Redis connection inside
`run_plugin_action_task.apply_async()` (`PluginRunAPIView._run_via_worker()`, `apps/plugins/api_views.py`). That call
sits before the view's `try`, hence HTML rather than the JSON error the view returns for a failed action. After a
container restart the probe plugin counted each uwsgi worker's descriptors through `/proc`: 3 to 7 Redis sockets at
rest, 141 to 167 after 300 `list_buffers` calls (about two per call), and none released after two idle minutes. The
workers' limit is 1024 (the entrypoint's `ulimit -n` does not reach them), so the web workers stop dispatching
plugin actions after about 2,000 calls in all, until Dispatcharr restarts. The likely cause, read in the configuration:
uwsgi runs Django under gevent (`gevent = 400`, `docker/uwsgi.ini`), whose monkey-patching makes Celery's per-thread
producer and result-backend state per greenlet, and every request is a new greenlet. Up to 0.33 an action ran in the
web worker itself and opened no such connection.

Why this addon reaches it quickly: a Kodi 22 viewer at the live edge polls `get_live_manifest` two to three times a
second (1,229 polls and 55 heartbeats in the check's first 9.5 minutes), so one viewer alone can use up a freshly
started server in about a quarter of an hour; a paused viewer's heartbeats would take hours. Once it happens,
Server-side mode can neither start, poll nor stop a buffer. A running buffer still played from the file server, the
addon's `stop_buffer` failed, and the plugin's reaper ended the buffer about 45 s after the viewers stopped.
`recording_edl` fails the same way.

**Decided 2026-11-18 (the user): the addon makes fewer `run/` calls; mitigated on `Piers` and
`Omega`.** `timeshift_buffer` 0.8.19's file server answers the manifest poll itself, and the addon
polls it there once `start_buffer`'s answer or a manifest names the route, skipping the heartbeat action while the file
server answers for the viewer; the action stays the fallback (`docs/TIMESHIFT.md`, "The manifest from the file server
(0.8.19)"). That leaves `start_buffer` and `stop_buffer` per viewer (the cold start's polls went too, before the
release; a cold start took 2 `run/` calls live), so the leak is slowed, not stopped: it stays open as an
upstream report to Dispatcharr (traceback and counts above), and until Dispatcharr fixes it a restart is the only cure.
Confirmed live 2026-11-18 on Kodi 22: 17 minutes at the live edge took 9 `run/` calls instead of about 2,600, and a
5½-minute pause none; on Kodi 21, 19 minutes with a pause took 25, 16 of them during one 10 s fallback after the
file server dropped a request (the TIMESHIFT.md section has both checks).

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
