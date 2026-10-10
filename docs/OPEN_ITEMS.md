*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**9 open, 280 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). All nine open items came from the
2026-10-10 comparison with the Tvheadend addon, written up in [PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md).

| Section | Entries |
|---|---|
| Open: Needs a live check | 2 |
| Open: Fix known, not yet done | 2 |
| Open: Architectural / concurrency | 1 |
| Open: Design decision needed | 2 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 246 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 25 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### Does Dispatcharr expose recording-disk usage

**Opened 2026-10-10, from the pvr.hts comparison (`docs/PVR_HTS_COMPARISON.md`).** pvr.hts overrides `GetDriveSpace`, so
Kodi's PVR status screen shows the backend's recording disk (total and used). Nothing in `docs/API_NOTES.md` records a
Dispatcharr endpoint for storage statistics, and none was looked for yet. To close: check the live OpenAPI document
(`/api/schema/`) and Dispatcharr's `core`/`channels` views for anything reporting the recording directory's size or free
space; if one exists, confirm it against a real instance (values, auth needed, units) and the feature becomes a small
override returning kilobytes as Kodi expects; if none exists, close this without a change and record that.

#### Fill Kodi's signal-status panel with the active stream's account and profile

**Opened 2026-10-10, from the pvr.hts comparison.** pvr.hts fills `GetSignalStatus` with the tuner's provider, service
and mux names, which Kodi shows in the player's information panel for a live channel. The IPTV equivalent would be the M3U
account and stream profile serving the channel right now, and the channel's own name as the service. Whether the active
stream can be known cheaply is the open question: Dispatcharr's live-proxy events carry it (recorded as unused in
`docs/API_NOTES.md`'s "Dispatcharr 0.32.0 (2026-10-07)" section), and the channel's `streams` list gives the candidates
but not the one in use. To close: confirm live what the proxy events contain for a channel opened by this addon, and
whether a REST call reports the same without the WebSocket; decide from that whether the panel is worth the extra traffic.
Not a correctness feature; drop it if the answer needs a request per channel open.

### Fix known, not yet done

#### Map Dispatcharr's radio flag to Kodi's radio channel type

**Opened 2026-10-10, from the pvr.hts comparison.** Dispatcharr 0.32.0 added `is_radio` and `effective_is_radio` to the
channel (confirmed in `docs/API_NOTES.md`'s "Dispatcharr 0.32.0 (2026-10-07)" section, which already notes that radio
channels reach Kodi as TV). The addon declares `SetSupportsRadio(false)` and sets `SetIsRadio(false)` on every channel and
group in `GetChannels()` and `GetChannelGroups()`. The change: read `effective_is_radio` in the channel parser (with a
test in `tests/test_channel_parser.cpp`, or wherever the channel JSON is mapped), declare radio support, answer the
`radio` argument of `GetChannels()`, `GetChannelGroups()` and `GetChannelGroupMembers()` instead of returning everything
for TV and nothing for radio, and flag the recording's channel type where Kodi asks for it. A group holding both kinds of
channel needs a decision (Kodi groups are TV or radio, not mixed); the simplest rule is a group appears in each list that
has at least one member of that kind. Confirm live with a channel flagged radio on a 0.32.0 server, in Kodi's Radio
section, including playback and the guide; also confirm a server older than 0.32.0 (no field) still lists every channel
as TV.

#### Report the backend hostname

**Opened 2026-10-10, from the pvr.hts comparison.** pvr.hts overrides `GetBackendHostname`; this addon overrides
`GetBackendName()`, `GetBackendVersion()` and `GetConnectionString()` but not the hostname, so Kodi's PVR information
screen leaves that line empty. One override returning the configured host (not the user-facing connection string, which
already carries the scheme and port). Confirm by reading the field in Kodi's PVR information dialog; the glue harness's
`pvr_harness` can assert the value. Keep the hostname out of any log line the addon writes at default verbosity, matching
how the connection string is handled.

### Architectural / concurrency

#### One stated stalled-stream rule per read path, with one threshold

**Opened 2026-10-10, from the pvr.hts comparison.** pvr.hts has a single rule for a stalled live stream: no demux packet
for N seconds while not paused (default 10, user-adjustable) drops the connection and reconnects once. This addon's read
paths each bound their own waits (the sliced live tail wait and its catch-up-to-tail budget in
`ReadLiveTimeshiftStream()`, the segment-fetch give-up rules of `ShouldGiveUpAfterSegmentFetchFailure()`, the
in-progress recording's short give-up, `IsAtEndedTail` for a dead encoder with a playlist on disk), with constants chosen
case by case and documented in `docs/TIMESHIFT.md` and `docs/RECORDINGS.md`. Nothing is known to be wrong; the item is
a review. To close: write down, per read path, what "stalled" means, how long it is tolerated, and whether the response
is a reopen, an EOF or a `-1`, in one table in `docs/TIMESHIFT.md`; decide whether the thresholds should be one
user-facing setting (pvr.hts's choice) or stay fixed; and, if any path turns out to have no bounded stall handling at
all, add it with a test in the tested decision functions. The `Off` live mode hands Kodi a stream URL and is Kodi's to
handle, so it is out of scope.

### Design decision needed

#### Report the connection state to Kodi instead of hand-rolled notifications

**Opened 2026-10-10, from the pvr.hts comparison.** pvr.hts reports every connect, disconnect, refusal and recovery to
Kodi through the `ConnectionStateChange` callback; this addon never calls it, so Kodi holds it at
`PVR_CONNECTION_STATE_UNKNOWN` for its whole life and the addon shows its own messages through `kodi::QueueNotification`
(14 call sites across `src/PVRDispatcharr.cpp` and `src/DispatcharrClient.cpp`). What Kodi 21 does with a reported state
was confirmed by reading `xbmc/pvr/addons/PVRClient.cpp` and `PVRClients.cpp` (the facts are in
`docs/PVR_HTS_COMPARISON.md`'s "Kodi-side facts confirmed while reading (Kodi 21)" section): standard localized
messages for unreachable, access denied, lost and re-established; the first unreachable after start-up deliberately not
shown; and, on `CONNECTED`, a re-read of the addon's `PVRCapabilities`. That last point is the reason this is a design
item and not a cosmetic one: `GetCapabilities()` depends on `m_dvrManageAllowed`, known only after the first login, and
reporting `CONNECTED` after login is the sanctioned way to make Kodi pick up the real DVR capabilities. Decisions needed:
which of the addon's failure modes map to which state (a 401 after the backoff in `AuthBackoff` is "access denied"; a
connect failure is "unreachable"; a `/api/core/version/` that answers but is not Dispatcharr is "server mismatch"), which
of the existing notifications become redundant and go, and whether to report `CONNECTING` at start-up (it makes Kodi
ignore the client until `CONNECTED`, which changes start-up ordering). Confirm live: Kodi start with the server down,
server coming up later, credentials revoked mid-session, server restarted mid-playback, each on the Linux client and one
phone, watching Kodi's messages and the channel list. The glue harness's `pvr_harness` already fakes the server and can
drive the transitions.

#### Let a series rule's title and description match modes be set from Kodi

**Opened 2026-10-10, from the pvr.hts comparison.** pvr.hts lets a timer rule match its title as "contains" or as a
regular expression (one global setting) and search the full EPG text per rule (`PVR_TIMER_TYPE_SUPPORTS_FULLTEXT_EPG_MATCH`).
Dispatcharr's series rule has `title_mode`, `description` and `description_mode` (confirmed shape in
`docs/API_NOTES.md`'s "Confirmed against a live instance" table); `src/TimerRuleParser.cpp` reads them back, but
`AddTimer()` creates every rule with them blank, so a rule made from Kodi always gets Dispatcharr's defaults and a rule
edited from Kodi keeps whatever it had. Decisions: whether the title mode is a global addon setting (pvr.hts's choice,
simplest) or per rule (Kodi's timer dialog has no match-mode field, so per rule would ride on the full-text flag);
whether Kodi's full-text flag maps to "also match the description" with the search string as `description`; and what
the values of `title_mode` and `description_mode` are (confirm against Dispatcharr's source, not the schema, since the
addon only knows `exact` and `contains` from reading real rules). Confirm live by creating a rule from Kodi with each
mode and checking what Dispatcharr's web UI shows and which programmes it schedules.

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
