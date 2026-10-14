*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 310 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); two are open from
user reports.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 1 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 1 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 268 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 33 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

#### Support Kodi 22 (Piers): the addon is refused by Kodi 22 RC1

**Opened 2026-10-12, from a user's report: on Kodi 22 RC1 (a Windows laptop) the addon "is not supported yet".** Kodi 22's PVR API is 9.2.0 with a minimum of 9.2.0 (Omega, which this branch builds against: 8.3.0, minimum 8.2.0), and its main and general APIs moved to 2.0.4 and 1.0.5, so the addon's declared imports and an Omega-built binary are refused outright; it must be rebuilt against Kodi 22's headers. A syntax-only compile of every `src/` file against the Piers dev-kit headers (branch `Piers`, 2026-10-11) gave six errors, all in `PVRDispatcharr.h`: the five recorded-stream overrides (`OpenRecordedStream()` now also returns a stream id, `ReadRecordedStream()`, `SeekRecordedStream()`, `LengthRecordedStream()` and `CloseRecordedStream()` take it), and `GetChannelStreamProperties()`, which gains a `PVR_SOURCE` argument (regular live, or playing a guide entry as live). The C++ wrapper hides the rest of the change (the C structs now hold string pointers instead of fixed arrays, EDL and timer-type lists are heap arrays), so most of `src/` compiles unchanged. What the port still needs: (1) the stream id means Kodi may hold more than one recorded stream open, but `DispatcharrClient` keeps one `m_recordingStream` and one `m_inProgressRecordingStream`, so either the state is keyed by id or a second open is refused deliberately; (2) the glue harness's stub and driver use the C structs directly and need the new shapes; (3) new optional calls worth a look, `GetRecordedStreamTimes()`, `IsRecordedStreamRealTime()` and `PauseRecordedStream()`, which bear on how an in-progress recording behaves in the player and on the open item about watching live while a recording runs; (4) `addon.xml.in` import versions, `KODI_COMMIT` and `KODI_BRANCH` in the workflow, and the Windows, Android and CoreELEC builds against Piers (`docs/BUILDING.md` notes `coreelec-22` tracks it); (5) the branching policy in `CLAUDE.md`: branch `Piers` off `Omega` once the source differs, keep `Omega` as the Kodi 21 maintenance branch, and decide how fixes flow between them (merge `Omega` into `Piers`; the six signatures are the only divergence so far). Confirm live on Kodi 22 RC1 with a Windows build: the addon loads, channels and guide appear, a live channel plays in each timeshift mode, a recording plays (completed and in progress) and seeks, and the timers dialog works.

### Architectural / concurrency

### Design decision needed

#### A channel that is slow to start: Kodi gives up after 20 seconds in Off and Local live mode

**Opened 2026-10-12, from a user's report: two channels take a while to start, a playback error appears, and pressing OK and opening the channel again plays it at once.** The reporter's answer on 2026-10-13 (timeshift Off, an Android device, two free-to-air HLS channels, logs from Kodi and from Dispatcharr) showed the cause is not in the addon's open path. There are two separate causes, one per live mode; the first is open, the second is fixed and waits for a release.

**Off and Local mode (open, decision on hold since 2026-10-13).** The addon only hands Kodi the proxy URL (`BuildLiveChannelStreamProperties()`), and Kodi opens it itself: `CFileCache::Open ... failed to open`, preceded by `CCurlFile::CReadState::FillBuffer ... Timeout was reached(28)`, 20.0 to 20.6 s after the open began, every time; the immediate second open plays. Dispatcharr's log for the same two opens: the first client registers at once and the channel moves `initializing -> connecting`, the proxy's stream generator then sits at `channel_ready=False` and sends the client nothing, and the buffer fills (`connecting -> active`) 24 s after the request for one channel and 21 s for the other, so the first byte arrives after Kodi has given up. The second open lands within the proxy's 5 s shutdown delay after Kodi's first client left, joins the running channel and gets data in half a second. So the stall is the interval between Dispatcharr accepting the client and having a buffered chunk (here ffmpeg opening an HLS master playlist and its variants; the reporter links it to the profile's stream mapping, which has not been checked here). The 20 s is Kodi's `curllowspeedtime` default (`m_curllowspeedtime = 20` in `AdvancedSettings.cpp`, read from `advancedsettings.xml` as `<network><curllowspeedtime>`); the only per-URL timeout option `CCurlFile` parses is `connection-timeout`, which sets the connect timeout, not this one, so the addon cannot lengthen it through the URL. An earlier version of this item said the proxy answers at once with keep-alive packets while a channel initialises; it does not for this server version, the generator sent nothing for 20 s.

Two ways forward, nothing built:
- **(a) Document the Kodi setting as the workaround.** Cheap, global to all of Kodi's curl reads, and not something the addon can set. **Checked live on 2026-10-13** (lab VM, a real Kodi, timeshift Off, a forwarder in front of the server that held the proxy stream request for 28 s before sending anything and passed everything else through): with Kodi's default the open failed with the playback-failed dialog at 20.9 s; after adding `<advancedsettings><network><curllowspeedtime>60</curllowspeedtime></network></advancedsettings>` to `advancedsettings.xml` and restarting Kodi, the same open played, at 35.7 s on the first try. The lab was restored afterwards.
- **(b) Warm the channel in `GetChannelStreamProperties()` when the mode is Off or Local.** The addon opens the proxy URL itself, waits for the first bytes (bounded, about 30 s), and keeps that connection until Kodi has attached, since closing the only client would start the 5 s shutdown (or an immediate stop with a delay of 0) and make Kodi's open pay the full start again. It blocks whichever Kodi thread calls it, which has to be measured first (the Server-side mode's open already costs a UI freeze, see "Cut the time a stalled open holds Kodi's UI"), and it adds a client to the channel for the length of the warm-up.

The decision between them is on hold (2026-10-13); the workaround is not in the README.

**Server-side mode (fixed on `Omega` as `142772f`, 2026-10-13; closes once a tagged release carries it).** The live cold-start loop (`OpenLiveTimeshiftStream()`) tried `get_live_manifest` 30 times, 500 ms apart, so it waited about 15 s for the first segment before failing with "no live playlist"; a channel whose provider took longer to deliver its first full segment (plus the plugin's segment length) outlasted it, and the second try worked because the failed open had deregistered the viewer while Dispatcharr kept the channel up for its shutdown delay. The fix uses what the plugin already says: it answers `fatal` once ffmpeg has exited, so a non-fatal "live playlist not found" means the channel is still starting. `RefreshLiveManifest()` reports that case (`pluginAnsweredNotReadyOut`) and the loop's limit is `ColdStartTryLimit()`: 60 tries (about 30 s, chosen over 45 s to halve the worst-case Stop freeze) while the plugin keeps answering it, the old 30 and the old 45 s budget for a try that got no answer (a silent server); the give-up warning is now logged for either reason (it used to be logged only when the wall-clock budget ran out). **Reproduced and checked live on 2026-10-12 and 2026-10-13** (lab VM, Server-side mode, a forwarder in front of the server answering the plugin's own "live playlist not found" reply for a set time after each `start_buffer` and passing everything else through, the plugin's file-server port forwarded as well since segment URLs use the configured host): before the change, 25 s of "not ready" made the addon stop the buffer 15.1 s after starting it and Kodi showed its playback-failed dialog, while opening again with the delay off played at once; after it, 25 s of "not ready" played **on the first try, at 29.7 s**; with 40 s it gave up at 29 s, logged `no live playlist after 60 tries and 29s, giving up`, and showed the dialog at 31.3 s, and opening again with the delay off played in 5.2 s; a real channel whose ffmpeg had died stopped the wait on the plugin's `fatal` answer after about 24 s, as designed. **The freeze this costs, measured:** Stop pressed 5 s into a 40 s "not ready" open held Kodi's main thread for 26.4 s, until the loop gave up (JSON-RPC answered at once; any call needing the UI thread blocked); see the closed entry "Cut the time a stalled open holds Kodi's UI". A Server-side user who still sees this should send the error `failed to open server-side timeshift stream for channel <uuid>: live playlist not found -- ...` (a slow start longer than about 30 s) or `ffmpeg exited before producing any segments` (the plugin's own fatal answer, a provider limit or a bad stream, which waiting does not fix). The reporter has not been able to confirm on a build that has the fix: it is not in a tagged release yet.

### Release, CI and manual testing

### Upstream (Dispatcharr) or documentation accuracy

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
