*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**9 open, 401 closed.** The closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md), each with when and how it was
confirmed. The open ones came from:

- Two from the 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and
  Dispatcharr with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).
- One from the Kodi 22 work on `Piers`: what remains of Kodi 22 support.
- One, LOW, from the 2026-10-23 bug-hunting sweep (five read-only reviews by area, each finding checked against
  the code before it was filed). Sixteen of its 17 are fixed or closed.
- One from the 2026-10-25 stream-switch re-test on Dispatcharr 0.33.0: a codec-changing switch, now a design question.
- One upstream note from the 2026-10-25 review of Dispatcharr 0.33.0, and one from the 2026-11-01 live check of a
  recurring rule on a spring-forward day.
- One from the 2026-10-28 review of the start-lock removal fix: a start parked on a gevent worker can still outlive its
  lock.
- One, LOW, from the 2026-10-31 bug-hunting sweep (five read-only reviews by area, each finding checked against the
  code before it was filed), confirmed live, now a design question. Nine of its eleven are fixed (the release gate's four, two in
  `timeshift_buffer`'s file server and three in the addon's request and thread handling) and one was closed without a
  change once checked live.

Nothing is left from the 2026-10-18 read-only audit of the whole project (28 findings) or from the 2026-10-24 sweep
(one reviewer over the whole project, seven findings).

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 1 |
| Open: Design decision needed | 3 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 2 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 350 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 41 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 10 |

## Open

### Needs a live check

None open.

### Fix known, not yet done

None open.

### Architectural / concurrency

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

#### A Stop while a recording rides out a transient failure holds Kodi's interface for up to 20 s

**Opened 2026-10-31, from the 2026-10-31 bug-hunting sweep (five read-only reviews by area, each finding checked against
the code before it was filed); LOW, confirmed by reading, not reproduced.** `ReadRecordingStreamImpl()` and
`ReadInProgressRecordingStreamImpl()` retry a transient failure (a refused connection, 500/502/503/504) inside one read
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

**Checked live 2026-11-01, before any change** (Kodi 21.3 with the current `Omega` build, behind a proxy on the Kodi
machine that answered recording reads with 503 while a flag file existed; a finished recording, and a disposable
recording made through the API and played while it recorded). Both comments are right, each for one path. **A finished
recording:** one read retried a 503 every 2 s from 2.1 s to 21.9 s, answered -1, and Kodi read no further: playback
ended, so a -1 there is final and slicing that loop would end playback at the first slice. **An in-progress
recording:** the read of one segment gave up after about 22 s and Kodi called the read again three more times (at
26.8, 48.7 and 70.5 s); when the 84 s outage ended, playback carried on. **A Stop pressed during the retry**, about 5 s
into the outage: `Player.Stop` returned in 0.3 s, but Kodi's interface (an `XBMC.GetInfoLabels` call, which needs its
main thread) was held for 16.7 s on the finished recording and 18.1 s on the in-progress one, and the player was gone
17.4 s and 18.8 s after the Stop, which is the rest of the 20 s budget. Without a Stop the interface stayed responsive
(0.4 s), since the reads run on Kodi's own reading thread. So: slice the in-progress loop the way the live path does,
with the budget counted across calls; for a finished recording keep one call's budget but consider a shorter one, and
say beside `kTransientReadBudget` that a -1 there ends playback.

**Tried 2026-11-02 and reverted: the retries cannot be handed back to Kodi.** The fix above (slice the in-progress
loop the way the live path does, the failure count carried on the stream so the delay keeps growing) passed a glue
scenario: the longest read went from 21.8 s to 2.0 s through 12 s of 503s. Live it ended playback. On Kodi 21.3 with
a throwaway build of `Omega` plus the change, an instant recording opened from its start and played 20 s, then 25 s of
503 on its segments through the same proxy: playback ended 14.3 s and 9.2 s into the outage, Kodi logging "eof reading
from demuxer"; the unchanged build on the same recording played on and advanced 19 s in the 21 s after it. Kodi's
source says why: a recording's `NextPVRStream()` answers `NEXTSTREAM_NONE`, finished or in progress
(`InputStreamPVRRecording.cpp`), so after a failed read `CVideoPlayer::Process()` drains its decoders and reads again
only while the audio or video player still holds data, and with the stream marked not initialised it no longer pauses
to buffer, so the queue empties. A channel answers `NEXTSTREAM_RETRY` and is simply read again. The 2026-10-02 entry
"A brief server outage ended playback for good" had measured the same thing, which is why the budget is inside one
call. The live check above saw an in-progress read retried after a 22 s -1, and a finished one not: what each queue
held at that moment, not a rule by stream type. The comments now say this (`IsTransientReadFailure()`, and beside
both `kTransientReadBudget`). What is left is a trade: a shorter budget shortens the hold after a Stop and the outage
a recording rides out together, or Kodi itself would need to act on a Stop during a read. Left at 20 s until someone
asks for otherwise. The in-progress tail wait cited above as answering -1 was reverted to one read on 2026-11-02 for
the same reason (`CLOSED_ITEMS.md`, "The in-progress tail wait hands back -1 every second").

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

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: cut the next `Piers` release from the final without the pre-release mark (`0.14.0-Piers` to `0.14.2-Piers` were cut from RC1 and all carry it) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

### Upstream (Dispatcharr) or documentation accuracy

#### Dispatcharr schedules a recurring rule's occurrence that starts inside a spring-forward gap to end before it starts

**Opened 2026-11-01, from the live check of the display item it closed (`CLOSED_ITEMS.md`, "A recurring rule starting
inside a spring-forward gap on its first day is shown ending before it starts"); confirmed against Dispatcharr 0.33.0.**
`sync_recurring_rule_impl()` (`apps/channels/tasks.py`) builds both times with `timezone.make_aware(datetime.combine(
...), tz)` on a `ZoneInfo` and rolls the end over only `if end_dt <= start_dt`. Two aware datetimes sharing one
`tzinfo` compare by wall clock, so a 02:30 that does not exist is still "before" 03:15 and nothing rolls over; stored,
02:30 takes the offset before the change. A disposable rule from 02:30 to 03:15 on the lab zone's next spring-forward
day (clocks from 02:00 to 03:00) got an occurrence stored ending 15 minutes before it starts. What the recorder then
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
