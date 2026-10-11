*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**8 open, 293 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). All but two open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); the other two came from the
stalled-stream review of the addon's own read paths (`docs/TIMESHIFT.md`) and from a user report.

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 1 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 4 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 256 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 28 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### Find out whether a Stop during an open reaches the open's cold-start loop

**Opened 2026-10-11, left over from the open-time cap item (`docs/CLOSED_ITEMS.md`, "Put a wall-clock cap on a stream open").** The two cold-start loops are now bounded (about 60 s live and 75 s in progress at the default timeout, plus the try in flight), but nothing cancels one earlier: the abort flag (`m_abortRequests`) is set only when the addon instance is destroyed. What is not known is whether a viewer can still cause a Stop while an open is running: Kodi may call `CloseLiveStream()` from another thread during the spinner (then `m_liveTimeshiftStream.open`, already true during the loop, could be checked per try) or may wait for `OpenLiveStream()` to return first. Measure that first: with a forwarder holding `get_live_manifest` as in the closed entry, press Back during the spinner and time Kodi's return to the channel list, and read the log for whether `CloseLiveTimeshiftStream()` ran before the open ended. If Kodi waits, there is nothing for the addon to do and this closes as explained; if it calls close concurrently, the loop should stop on a closed session (a per-open flag or the session counter, not the shutdown flag).

### Fix known, not yet done

#### Say why a server-timeshift open fails when the buffer's file server cannot be reached

**Opened 2026-10-11, from a user report (Docker install, server timeshift: the channel spins for about 30 s and then exits, local timeshift works).** The buffer starts and its files appear on the server, so `start_buffer` and the manifest calls (through Dispatcharr's own port) succeed and the open completes; only the first segment read goes to the plugin's file server on `http_port` (default 9192, a separate port that has to be published by the container and match the plugin's setting), and when that port is not reachable from the Kodi device (not published, published under another number than the setting, a host firewall) every fetch fails to connect. `ReadLiveTimeshiftStream()` retries for its 25 s outage budget, answers 0, and Kodi ends the stream: the viewer sees a spinner and then nothing, and nothing in Kodi says why (the log has the connect failures at debug level only). The README says to map the port, but a user who maps a different one gets no signal. The fix: probe the file server at the end of the open (one request to `http://<host>:<http_port>/`, which answers 403 when it is alive; any HTTP answer counts as reachable, a connect failure or timeout within the short request bound does not), fail the open at once on a connect failure, and queue a notification that names the port and says it must be published and match the plugin's `http_port` setting; the same message when a running stream is given up on after connect failures to that port. The new pure part (classify a probe result) gets a unit test; the glue harness can refuse connections on the fake's file-server port. Confirm live by pointing the plugin's `http_port` at a port nothing publishes (or blocking it with the forwarder) and opening a channel: today the 30 s spinner, after the change a notification within a few seconds. Also add a line to `docs/TROUBLESHOOTING.md` and the plugin's README for the symptom (spinner then exit, buffer files on disk).

### Architectural / concurrency

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
whether Kodi's full-text flag maps to "also match the description" with the search string as `description`; and how
the four title modes are offered when Kodi has room for two. The values are confirmed from source (the Dispatcharr
source review, `docs/DISPATCHARR_SOURCE_REVIEW.md`): `title_mode` is `exact` (default), `contains`, `search` or `regex`;
`description_mode` is `contains` (default), `search` or `regex`; `untagged_is_new` is a separate boolean
(`SeriesRulesAPIView` in `apps/channels/api_views.py`). Confirm live by creating a rule from Kodi with each mode and
checking what Dispatcharr's web UI shows and which programmes it schedules.

#### List the two plugins in Dispatcharr's Plugin Hub

**Opened 2026-10-10, from the Dispatcharr source review; a decision about publishing, so it waits for the maintainer.**
Since Dispatcharr 0.23.0 an admin can browse, install and update plugins from repositories inside Dispatcharr, and the
official catalogue (the Plugins repository) accepts external plugins: a PR to its `main` branch adding
`plugins/<slug>/plugin.json` with `source_type: "external"`, a `source_url` that is an HTTPS link to a zip and contains
a `{version}` placeholder, `repo_url`, an OSI license (GPL-2.0-or-later qualifies) and `author` equal to the submitting
GitHub account; each version bump is another PR, reviewed by the catalogue's maintainers. Today both plugins install by
zip import only, and their zips are attached to addon-version releases, so no URL exists that is templated on the
plugin's own version. Options: per-plugin GitHub releases or tags cut by the release gate (satisfies the catalogue's
rule; Clapparr does this); a self-hosted repository manifest with absolute `latest_url`s, updated by the gate (the flat
format in Dispatcharr's `Plugin_repo.md`; users add its URL once; the name must not contain "official" or
"dispatcharr"); or staying with zip import. Either hosted option means the release gate also writes a manifest and
checksums, and the privacy gate must cover what it publishes. Decide first; the mechanics follow.

#### Use Dispatcharr's event hooks in the plugins

**Opened 2026-10-10, from the Dispatcharr source review.** An action whose definition carries an `"events": [...]` list
is run by `log_system_event()` when one of those system events fires (`iter_actions_for_event` in
`apps/plugins/loader.py`); the names are `channel_start`, `channel_stop`, `channel_buffering`, `channel_failover`,
`channel_reconnect`, `channel_error`, `client_connect`, `client_disconnect`, `recording_start`, `recording_end`,
`stream_switch`, the `epg_*` and login events, `vod_start` and `vod_stop`. Handlers run on a gevent greenlet under uWSGI
and must stay short. Neither plugin subscribes to anything. Candidates: `timeshift_buffer` reacting to
`channel_failover`, `channel_error` or `stream_switch` on a channel it is buffering (its dead-ffmpeg handling is a poll
today, `docs/TIMESHIFT.md`), and `recording_edl` running its sidecar scrub on `recording_end` instead of only on demand.
Decide per plugin whether an event hook adds anything a poll does not, keeping in mind the handler cannot block and the
event may arrive on a different worker than the one holding the buffer's state (Redis is the shared state, as today).
Confirm live by watching the plugin log while a stream fails over on a buffered channel and while a recording finishes.

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
