*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 311 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one is open from
a user report, and one from the multiple-recorded-streams work on `Piers`.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 1 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 3 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 269 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 33 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

#### Support Kodi 22 (Piers): the addon is refused by Kodi 22 RC1

**Opened 2026-10-12, from a user's report: on Kodi 22 RC1 (a Windows laptop) the addon "is not supported yet".** Kodi 22's PVR API is 9.2.0 with a minimum of 9.2.0 (Omega, which this branch builds against: 8.3.0, minimum 8.2.0), and its main and general APIs moved to 2.0.4 and 1.0.5, so the addon's declared imports and an Omega-built binary are refused outright; it must be rebuilt against Kodi 22's headers. A syntax-only compile of every `src/` file against the Piers dev-kit headers (branch `Piers`, 2026-10-11) gave six errors, all in `PVRDispatcharr.h`: the five recorded-stream overrides (`OpenRecordedStream()` now also returns a stream id, `ReadRecordedStream()`, `SeekRecordedStream()`, `LengthRecordedStream()` and `CloseRecordedStream()` take it), and `GetChannelStreamProperties()`, which gains a `PVR_SOURCE` argument (regular live, or playing a guide entry as live). The C++ wrapper hides the rest of the change (the C structs now hold string pointers instead of fixed arrays, EDL and timer-type lists are heap arrays), so most of `src/` compiles unchanged. What the port still needs: (1) the stream id means Kodi may hold more than one recorded stream open, but `DispatcharrClient` keeps one `m_recordingStream` and one `m_inProgressRecordingStream`, so either the state is keyed by id or a second open is refused deliberately; (2) the glue harness's stub and driver use the C structs directly and need the new shapes; (3) new optional calls worth a look, `GetRecordedStreamTimes()`, `IsRecordedStreamRealTime()` and `PauseRecordedStream()`, which bear on how an in-progress recording behaves in the player and on the open item about watching live while a recording runs; (4) `addon.xml.in` import versions, `KODI_COMMIT` and `KODI_BRANCH` in the workflow, and the Windows, Android and CoreELEC builds against Piers (`docs/BUILDING.md` notes `coreelec-22` tracks it); (5) the branching policy in `CLAUDE.md`: branch `Piers` off `Omega` once the source differs, keep `Omega` as the Kodi 21 maintenance branch, and decide how fixes flow between them (merge `Omega` into `Piers`; the six signatures are the only divergence so far). Confirm live on Kodi 22 RC1 with a Windows build: the addon loads, channels and guide appear, a live channel plays in each timeshift mode, a recording plays (completed and in progress) and seeks, and the timers dialog works.

### Architectural / concurrency

### Design decision needed

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

#### Two streams of one in-progress recording each poll its manifest

**Opened 2026-10-14, with the multiple-recorded-streams work on `Piers`; deferred.** Kodi 22 may hold two streams of the
same recording open (`docs/API_NOTES.md`, "Single-instance assumption"), and each `InProgressRecordingStreamState`
refreshes the recording's HLS playlist and looks the recording up on its own, about every half second while it is read
and on every `GetRecordedStreamTimes()` poll. They share the sized-segment cache (`m_inProgressSegmentCache`) but not
the refresh itself, so the request rate doubles for that recording. Nothing is known to break, and nothing in Kodi
opens two streams of one recording on purpose that this project has found; pick it up if a log shows it, by sharing the
playlist fetch and the lookup per recording id behind the throttle that already exists.

### Tooling (tools/)
