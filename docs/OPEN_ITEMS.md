*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**15 open, 281 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Every open item came from the two
2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)).

| Section | Entries |
|---|---|
| Open: Needs a live check | 1 |
| Open: Fix known, not yet done | 5 |
| Open: Architectural / concurrency | 2 |
| Open: Design decision needed | 4 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 1 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 246 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 26 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

#### Fill Kodi's signal-status panel with the active stream's account and profile

**Opened 2026-10-10, from the pvr.hts comparison; narrowed the same day by the Dispatcharr source review
(`docs/DISPATCHARR_SOURCE_REVIEW.md`).** pvr.hts fills `GetSignalStatus` with the tuner's provider, service and mux
names, which Kodi shows in the player's information panel for a live channel. The IPTV equivalent is what Dispatcharr's
`GET /proxy/ts/status/{channel_id}` already returns for a running channel: `stream_profile`, `stream_name`, `video_codec`,
`resolution`, `source_fps`, `ffmpeg_speed`, `client_count`, uptime and a computed bitrate (read from the Redis channel
metadata, `apps/proxy/live_proxy/channel_status.py`). The catch, confirmed from source: that view is
`@permission_classes([IsAdmin])`, so a non-admin account gets nothing, and the panel would be an admin-only feature like
the two companion plugins. To close: decide whether an admin-only panel is worth one request per channel open plus a
periodic refresh (Kodi polls `GetSignalStatus` while the panel is open); if yes, confirm live what the detailed body
holds for a channel this addon opened, through the proxy path it uses, and keep every field out of the log at default
verbosity. Not a correctness feature; drop it if the answer is "admin only and one more request".

### Fix known, not yet done

#### Show a recording's poster, season and episode and rating from Dispatcharr's enrichment

**Opened 2026-10-10, from the Dispatcharr source review (`docs/DISPATCHARR_SOURCE_REVIEW.md`).** Dispatcharr's artwork
prefetch task writes `poster_url`, `poster_logo_id`, `rating`, `rating_system`, `season`, `episode` and
`onscreen_episode` into a recording's `custom_properties` when the matched EPG programme supplies them, and the capture
task adds `stream_info`. `src/RecordingParser.cpp` reads only `program.title`, `program.sub_title` and
`program.description`, and `GetRecordings()` never calls `SetIconPath()` on a recording, so a recording in Kodi has no
artwork, no season or episode number and no rating while the guide entry it came from has all three. The change: parse
the keys in `RecordingParser` (table-tested in `tests/test_recording_parser.cpp`, including absent and malformed
values), build the poster URL from `poster_logo_id` the way channel logos are built (`poster_url` as the fallback), and
set the icon, series and episode numbers on the Kodi recording. Two decisions ride along: whether season/episode are
trusted at all (`docs/RECORDINGS.md` saw a daily programme whose every airing carried one series-level date, which is why
`FirstAired` is guarded; apply the same scepticism), and whether `rating_system` now allows the parental-rating mapping
that `docs/EPG.md`'s "Not mapped, deliberately" paragraph declined for the guide. Confirm live with one enriched and one
unenriched recording on the Linux client and a phone, checking the recordings list and the recording's information
dialog.

#### Skip the EDL plugin call when the recording's comskip metadata says there is nothing to fetch

**Opened 2026-10-10, from the Dispatcharr source review.** After comskip runs, Dispatcharr writes
`custom_properties.comskip` with `status` (`completed`, `skipped`, `error`), `mode` (`mark`, or absent for cut),
`commercials` and the EDL basename (`comskip_process_recording` in `apps/channels/tasks.py`; the EDL content itself is
still not served anywhere, so `recording_edl` stays). The addon does not read that key and asks the plugin for every
recording it opens, including those where comskip never ran, failed, or ran in cut mode and deleted the file. The change:
a pure decision (comskip metadata in, "ask the plugin" or "no markers" out, with "ask" for an absent key so older
servers and recordings behave as today), table-tested, consulted before `GetRecordingEdl()` calls the plugin. Confirm
against the plugin's own handling of the same cases (`dispatcharr-plugin/recording_edl/plugin.py` distinguishes them
by the file, not the mode key) so the two never disagree on a recording that does have markers, and live with one
marked and one cut recording.

#### Pin the XMLTV export's channel-id source and day window explicitly

**Opened 2026-10-10, from the Dispatcharr source review.** `/output/epg` (`epg_endpoint` in `apps/output/views.py`)
takes `tvg_id_source` (`channel_number` by default, also `tvg_id` and `gracenote`), `days` (defaulting to the account's
`epg_days` custom property, 0 meaning no limit) and `prev_days`. `GetXmlTvGuide()` sends only `prev_days`; the parser's
assumption that `<channel id>` is the channel number (`docs/API_NOTES.md`'s table) rests on the server default, and the
forward window rests on whatever the account's `epg_days` happens to be. The change: send `tvg_id_source=channel_number`
always, and consider sending `days` from Kodi's own EPG future-days value (`SetEPGMaxFutureDays`, not yet overridden)
instead of inheriting the account's. Confirm live that the export is byte-identical with and without the explicit
source parameter on a 0.32.0 server, and that an account with a non-zero `epg_days` still gets the window Kodi asked
for.

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
handle, so it is out of scope. One input from the Dispatcharr source review (`docs/DISPATCHARR_SOURCE_REVIEW.md`): since
0.32.0 the `channel_buffering`, `channel_failover`, `channel_reconnect`, `channel_error` and `stream_switch` system events
name the active stream, so "the server is already failing over" is knowable rather than inferred from byte starvation;
whether the addon or the plugin should listen for them belongs to this review.

#### Confirm no plugin thread uses the ORM without closing its connection

**Opened 2026-10-10, from the Dispatcharr source review.** `PluginManager.run_action()` and `stop_plugin()` call
`close_old_connections()` after the plugin returns, but Dispatcharr's `Plugins.md` is explicit that this covers only the
calling greenlet: every thread or greenlet a plugin spawns that touches the ORM must call it in its own `finally`, or it
holds one of the eight pooled connections per uWSGI worker. `timeshift_buffer` runs its own threads (the segment file
server, the reaper). Whether any of them executes ORM code was not checked during the review. To close: read each thread
body in `dispatcharr-plugin/timeshift_buffer/plugin.py` for Django model access (Redis use is fine); if any exists, add
the `finally` and a test that the hook is called; if none, record that here and close. Reading is enough to confirm.

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

#### Point the troubleshooting notes at the server-side stream-profile plugins

**Opened 2026-10-10, from the Dispatcharr source review (`docs/DISPATCHARR_SOURCE_REVIEW.md`).** Dispatcharr's plugin
catalogue carries three stream profiles built to absorb CDN gaps and timestamp breaks before the stream reaches any
client (Reservoarr, Profilarr, Segmentarr), and one that lowers the proxy's prebuffer per channel group to speed up
channel start (Audio Buffer Tuner). A stall or a slow zap that this addon cannot fix on the client is often one of those
on the server, and `docs/TROUBLESHOOTING.md` does not mention that a server-side remedy exists. To close: add a short,
neutral paragraph there (and one line in the README's troubleshooting pointer if there is one) saying such plugins exist
in Dispatcharr's own catalogue and what each addresses, without recommending one, since none was tested with this addon.
Reading is enough; no live check.

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
