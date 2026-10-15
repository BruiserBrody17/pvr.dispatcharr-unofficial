*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 313 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); the other two come from the
Kodi 22 work on `Piers`: two streams of one recording each polling its manifest, and what remains of Kodi 22 support.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 1 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 3 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 271 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 33 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done


### Architectural / concurrency

### Design decision needed

### Release, CI and manual testing

#### Kodi 22 (`Piers`): the builds for the other platforms, the pinned Kodi commit and the first release

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked). (1) The macOS, Android and CoreELEC builds against Kodi 22 have not been run; `docs/BUILDING.md` lists the differences known from Kodi's own documentation (NDK 28.2.13676358, `android-37.0`, build-tools 37.0.0, `coreelec-22`), and `docs/BRANCHES.md` the open question of what version and zip name CoreELEC gives a package whose `PKG_VERSION` carries the `-Piers` suffix. (2) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (3) The first `Piers` release, `0.14.0` (unreleased in `CHANGELOG.md`, tag `0.14.0-Piers`), which needs the user's go-ahead for the version, the tag and any push to GitHub. (4) Not checked on Kodi 22: a second install's worth of the things the lab cannot do (Android devices, a CoreELEC box, macOS), and the `inputstream.ffmpegdirect` catch-up seek setting.

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
