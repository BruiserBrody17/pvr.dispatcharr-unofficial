*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**12 open, 433 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One from the Kodi 22 work on `Piers`: what remains of Kodi 22 support.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One user report from 2026-11-14: Off-mode live TV ending by itself on an Android tablet.
- One follow-up to issue #1 from 2026-11-16: DVB subtitles and teletext in Server-side mode.
- One observation from the 2026-11-16 live check of the catch-up session cache: Kodi 22 reading two finished
  recordings end to end at start.
- Four from the 2026-11-14 bug-hunting sweep (six read-only reviews by area, seventeen findings), all LOW. Its
  four documentation and four tooling findings, its one MEDIUM and its one design question were closed on 2026-11-15,
  the catch-up session reuse and the two `timeshift_buffer` findings on 2026-11-16.

Nothing is left from the 2026-10-18 read-only audit of the whole project (28 findings), from the 2026-10-23 sweep
(five read-only reviews by area, 17 findings, the last fixed on 2026-11-04), from the 2026-10-24 sweep
(one reviewer over the whole project, seven findings), from the 2026-10-31 sweep (eleven findings: nine fixed, two
closed without a change, the last on 2026-11-04 by keeping a recording's 20 s retry budget) or from the 2026-11-10
sweep (eleven findings, all fixed: the release gate, the CI path filter and a `timeshift_buffer` regression on
2026-11-10, the addon's three, the glue fake and the `BRANCHES.md` table on 2026-11-12).

| Section | Entries |
|---|---|
| Open: Needs a live check | 3 |
| Open: Fix known, not yet done | 4 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 380 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 43 |
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

### Architectural / concurrency

None open.

### Design decision needed

None open.

### Release, CI and manual testing

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: cut the next `Piers` release from the final without the pre-release mark (`0.14.0-Piers` to `0.14.4-Piers` were cut from RC1 and all carry it) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

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
