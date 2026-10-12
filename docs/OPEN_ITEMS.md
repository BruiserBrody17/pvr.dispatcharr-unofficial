*(The project's running punch-list -- not part of the "confirmed live" API_NOTES.md family of docs. New items go under **Open**, in whichever category fits; when one is resolved, move it to [CLOSED_ITEMS.md](CLOSED_ITEMS.md) with a short note of how it was confirmed, rather than deleting it -- the closed entries are the record of what was tried and why. This file holds only what is still open, so it stays readable in one sitting; the history is there. A citation of this file for an item that is resolved (`docs/OPEN_ITEMS.md` in a code comment or an older doc) means that item's entry in `CLOSED_ITEMS.md`. The 1.0-era release-checklist history this file used to keep was removed once `1.0` stopped being a near-term target (see `CLAUDE.md`'s versioning note); it's preserved in git history.)*

# Open items

## Status at a glance

**9 open, 298 closed** (the closed entries are in [CLOSED_ITEMS.md](CLOSED_ITEMS.md)). Six open items came from the
two 2026-10-10 source reviews: the Tvheadend addon ([PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md)) and Dispatcharr itself
with its plugin catalogue ([DISPATCHARR_SOURCE_REVIEW.md](DISPATCHARR_SOURCE_REVIEW.md)); one came from the
untitled-recording fix, one from the live check of a Stop during an open (`docs/TIMESHIFT.md`'s stalled-streams section) and one
from a user's report.

| Section | Entries |
|---|---|
| Open: Needs a live check | 0 |
| Open: Fix known, not yet done | 0 |
| Open: Architectural / concurrency | 0 |
| Open: Design decision needed | 7 |
| Open: Release, CI and manual testing | 0 |
| Open: Upstream (Dispatcharr) or documentation accuracy | 0 |
| Open: Known gaps, deliberately deferred | 2 |
| Open: Tooling (tools/) | 0 |
| Closed (in CLOSED_ITEMS.md): Fixed | 260 |
| Closed (in CLOSED_ITEMS.md): Closed without a change (refuted, explained or harmless) | 29 |
| Closed (in CLOSED_ITEMS.md): Project history and test infrastructure | 9 |

## Open

### Needs a live check

### Fix known, not yet done

### Architectural / concurrency

### Design decision needed

#### Decide whether a stalled open should hold Kodi's UI for less than a minute

**Opened 2026-10-11, from the live check of a Stop during an open (`docs/CLOSED_ITEMS.md`, "Find out whether a Stop during an open reaches the open's cold-start loop").** Kodi runs `CVideoPlayer::CloseFile()` on its main thread and the call waits for the player thread, which is parked inside `OpenLiveStream()`; the addon is told nothing until the open returns. So when a viewer presses Stop during an open whose server has stalled, Kodi's whole UI freezes for the rest of the open (measured: 55 s of a 61 s open at the default 30 s timeout). The wall-clock cap bounds it and nothing in the addon can shorten an open already running, so the only lever is the bound itself. The condition is narrow (a server that accepts the connection and answers `start_buffer`, then goes silent on `get_live_manifest`; a dead or refusing server fails fast). Options: leave it; give the live first-manifest request a per-try timeout shorter than the general one (`RefreshLiveManifest()` uses the full configured timeout while `coldStart` is set), at the cost of failing a slow-to-start encoder that currently succeeds on a late try; or shrink the budget's allowance of one full extra timeout (`ColdStartWallClockBudgetMs()`). Decide; if it changes, confirm with the same forwarder hold (Stop 5 s into an open, UI back within the new bound) and that a normal cold open on a slow channel still succeeds.

#### Watching live while a recording runs on the channel starts a second buffer

**Opened 2026-10-12, from a user's report (Server-side mode; they were recording a programme, then tuned the channel and answered "live" at Kodi's "play recording or tune to live" prompt).** `GetChannelStreamProperties()` and `OpenLiveStream()` never look for a recording on the channel: in Server-side mode every live open starts (or joins) the timeshift-buffer plugin's own buffer for it, a second ffmpeg and a rolling buffer on disk next to the recording's own. The user's expectation, from Tvheadend: the in-progress recording already allows pause and rewind, so watching live should use it and start no buffer. The cost today is the extra process and disk, not an extra provider connection (the buffer and the recording should both be clients of Dispatcharr's one proxy stream for the channel; not checked live). The idea is feasible in outline, since the addon already plays an in-progress recording with seek and pause (`OpenInProgressRecordingStream()`), but Kodi plays a channel through the live-stream callbacks (`OpenLiveStream()`, `ReadLiveStream()`, `SeekLiveStream()`, `GetStreamTimes()`) and a recording through the recorded-stream ones, so the live path would have to delegate to the recording's reader and report stream times for it. The blocking questions are semantic. (1) **The recording ends** at the programme's end plus post-padding, so a viewer who chose "live" reaches the end of the stream mid-channel; handing over to a timeshift buffer without a gap (or without Kodi leaving the channel) is not something the callbacks obviously allow, and a reopen drops the viewer to the channel with a gap. (2) **The recording can be stopped or deleted** (from Kodi or Dispatcharr) while someone watches it as live, which ends a stream they never asked to end. (3) **The live edge** of a recording is a segment or two behind the real one, as the timeshift buffer's is; rewind reaches back only to the recording's start, which is probably what a viewer wants. (4) **Tvheadend parity is unverified**: the report says it plays the recording instead of starting timeshift when "live" is chosen over a joined recording; that was not checked against pvr.hts or Tvheadend. Options: leave it (it works, it costs a process and disk); a setting that plays the channel plainly (no pause or rewind) when a recording already runs on it, which saves the buffer but loses the pause the user wants; or the delegation above, preceded by a live experiment of what Kodi does when a live stream's reader returns end-of-stream mid-playback (does it leave, retry the open, or hold the player?), which decides (1). Confirm live with a recording running on a channel and the channel opened as live: today's behaviour is a buffer in `list_buffers` for that channel (the baseline to record), and after any change none, with pause, rewind and the recording's own end handled as decided.

#### Keep a title the user typed for a manual timer

**Opened 2026-10-11, left over from the untitled-recording fix (`docs/CLOSED_ITEMS.md`, the entry on a recording nothing titled).** `AddTimer()` keeps the title Kodi sends for three minutes (`PendingTitle`) and never writes it to the server, because the one write available, `RenameRecording()` (the `update-metadata` endpoint), stores `custom_properties.program.title` and marks the recording `user_edited`, which stops Dispatcharr's own later enrichment (description, poster, programme data) for it. For a recording Kodi named from the guide or the channel that is the right trade (enrichment supplies the title, the channel name stands in now), but a manual timer whose Name field the user typed loses that name after three minutes and shows the channel name instead. The difficulty is telling a typed name from Kodi's default: the default is the localized string "New Timer", which the addon cannot compare against reliably. Decisions: whether to persist the title for a manual timer (`timer.GetEPGUid() == PVR_TIMER_NO_EPG_UID`) only when it differs from the channel's name and from "New Timer" in the languages Kodi ships, or to persist it client-side in the addon's data like `RecordingEpgLinks` (survives restarts, not other Kodi clients), or to leave it. Confirm live with a manual timer named by hand: after the change the name must survive the three minutes and a Kodi restart, and a manual timer left at Kodi's default must still pick up the programme's title from Dispatcharr's enrichment.

#### Report a server that is not Dispatcharr to Kodi, and drop the restart warning

**Opened 2026-10-10 from the pvr.hts comparison; the login-driven part and the mid-session outage are done (`docs/CLOSED_ITEMS.md`, "Tell Kodi when Dispatcharr is unreachable or refuses the login" and "Tell Kodi when the server goes away mid-session"), this is what is left.** (1) **A server that answers but is not Dispatcharr** (a 404 or 405 from a proxy on the token path, a 200 with no token, a version endpoint with no version) is left unreported; Kodi has `SERVER_MISMATCH` ("Server does not respond properly") for it. The signals exist (`ReportForAuthFailure()` already sees the status; the startup version check sees a missing version), the open question is only which answers are unambiguous enough: a 404 on a path that exists on every Dispatcharr is, a 200 with an unexpected body from behind a captive portal may be, a transient 404 during a Dispatcharr upgrade is not. (2) **The "Restart Kodi to hide the record, delete and rename actions" warning** (`SyncDvrAccessFromDispatcharr()`) exists because Kodi reads capabilities once; since the held `CONNECTED` report now forces a re-read after the access level is learned, it may be removable, but nobody has seen Kodi's UI drop the actions without a restart. Confirm live with a view-only account (a disposable user on the lab server, which changes the lab: ask first) and the server unreachable at Kodi start, then reachable: the Record button, Timers window and delete/rename must disappear after "Connection established", and if they do the warning goes. Neither changes what a viewer sees often; (2) is the one with a concrete gain (no restart).

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
