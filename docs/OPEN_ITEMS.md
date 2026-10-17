*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 314 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); the other two come from the
Kodi 22 work on `Piers`: what remains of Kodi 22 support, and Kodi 22's stream-details probe of a just-finished
recording.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 1 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 272 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 33 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

#### Kodi 22's stream-details probe of a just-finished recording sizes every segment again

**Opened 2026-10-17, from the live check of the two-streams change (`CLOSED_ITEMS.md`, "Two streams of one
in-progress recording each poll its manifest").** When a recording finishes while it is watched, Kodi 22's thumbnail
loader opens it to read its stream details once its timer is gone, and the addon sends the open through the
in-progress path because the recording's HLS folder is still there (`hlsDirStillPresent`). By then a stream has seen
the recording `finished`, and `RefreshInProgressRecordingManifest()` has erased its cross-open segment cache
(`m_inProgressSegmentCache`) on the assumption that a finished recording is not opened through this path again. So
the probe HEADs every segment: two probes from two threads cost 340 HEADs in 1.7 s for an 11-minute recording, which
would be about 3,600 for a two-hour one. Nothing breaks. Fix options: keep the entry for a finished recording until
its HLS folder is gone (or for a short grace after `finished`), or open a finished recording through the completed
path even while its folder exists (check first what the completed `/file/` endpoint serves while the server is still
joining the segments). Kodi's probe also opens one recording from several threads at once (seen for an older
recording whose playlist answers 404), which the first option would serve too.

### Architectural / concurrency

### Design decision needed

### Release, CI and manual testing

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: take the pre-release mark off `0.14.0-Piers` (or cut `0.14.1` from the final) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

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
