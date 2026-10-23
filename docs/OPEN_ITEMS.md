*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**4 open, 351 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Two open items came from the two
2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one comes from the Kodi 22
work on `Piers`: what remains of Kodi 22 support; one tracks publishing the Kodi repository added on 2026-10-19. Nothing
is left from the 2026-10-18 read-only audit of the whole project (addon, both plugins, tools, CI and packaging; 28
findings: seven fixed the same day, 14 on 2026-10-19, four on 2026-10-21, one on 2026-10-22 and the last two closed
without a change on 2026-10-22, all in `CLOSED_ITEMS.md`).

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 0 |
| Open: Release, CI and manual testing | 2 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 305 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 37 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

None open.

### Fix known, not yet done

None open.

### Architectural / concurrency

None open.

### Design decision needed

None open.

### Release, CI and manual testing

#### The Kodi repository is built but not published

**Opened 2026-10-19, when the repository was added** (`tools/build_kodi_repo.py`, `kodi-repository/`; how it works and
what was confirmed live: `docs/BUILDING.md`'s "Distribution: the Kodi repository"). Nothing is public yet, and
README.md's "Installing" already describes it, so the site has to exist before README.md reaches the public repository.
First publish: (1) create the `gh-pages` branch on the public repository holding only the site: `index` for Kodi 21
from `0.13.1-Omega` and for Kodi 22 from `0.14.1-Piers` (the gated zips; whichever releases are newest at the time), then `repository`; gate the repository zip;
(2) let the local pre-push hook allow `gh-pages` on the public remote; (3) turn on GitHub Pages for that branch and
check the site URL and a range request (206); (4) install the repository on a real Kodi from the public URL and
install the addon from it. Then (5) port the tool, the repository addon and README.md's "Installing" to `Omega`
(GitHub's default branch, whose README Kodi 21 users read), and record it in `docs/BRANCHES.md`.

#### Kodi 22 (`Piers`): the pinned Kodi commit, checks still missing, and the pre-release mark

**Opened 2026-10-15, what is left of the Kodi 22 port once the addon was confirmed on Kodi 22.0-RC1** (the closed entry "Kodi 22 (Piers) refused the addon" in `CLOSED_ITEMS.md` has what was done and how it was checked, including the other platforms' builds, their device runs and the first release). (1) Raise the pinned Kodi commit (`KODI_COMMIT`) from the `22.0rc1-Piers` tag to 22.0 final when it ships, and repeat the smoke run. (2) Not checked on Kodi 22: audio passthrough, a long soak and HEVC. (3) When Kodi 22.0 final ships and (1) is done: cut the next `Piers` release from the final without the pre-release mark (`0.14.0-Piers` and `0.14.1-Piers` were cut from RC1 and both carry it) so the newest-release badge can follow Kodi 22, and decide whether the repository's default branch should move from `Omega` to `Piers` (it stays on `Omega` while most users are on Kodi 21: the badge and the front-page README should send a Kodi 21 user to a build that works).

### Upstream (Dispatcharr) or documentation accuracy

None open.

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
