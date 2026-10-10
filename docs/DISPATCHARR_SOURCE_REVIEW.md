# What Dispatcharr's source and its plugin catalogue offer that this project does not use yet (2026-10-10)

*Engineering notes, not user documentation. Prompted by a "can we borrow or improve anything from Dispatcharr's own source,
and from the published plugins" question, the day after the same exercise for the Tvheadend addon
(`docs/PVR_HTS_COMPARISON.md`). Everything here is from reading source; nothing was run against a server. Where a finding
changed an open item, the item in `docs/OPEN_ITEMS.md` carries the actionable version and this file keeps the evidence.*

## What was read

- Dispatcharr at commit `eab4b4f` (2026-10-09). `version.py` says 0.32.0 and `CHANGELOG.md`'s Unreleased section is
  empty, so this tree is the released 0.32.0 the addon was already checked against (`docs/API_NOTES.md`'s "Dispatcharr
  0.32.0 (2026-10-07)" section) plus a documentation commit. Read: every `apps/*/api_urls.py` and `core/api_urls.py`,
  `apps/channels/models.py` and the recording, series-rule and comskip code in `apps/channels/api_views.py` and
  `apps/channels/tasks.py`, the whole `apps/timeshift` app, `apps/plugins/loader.py`, `api_views.py` and `models.py`,
  `apps/proxy/live_proxy` (status views, `channel_status.py`, `constants.py`), `apps/output/views.py`'s export
  parameters, `core/models.py`, `Plugins.md`, `Plugin_repo.md` and the changelog back to 0.10.
- The Plugins repository: the `releases` branch at `84e14cc` (2026-10-10; 35 plugins, `manifest.json`, every per-plugin
  README and manifest) and the `main` branch's `CONTRIBUTING.md` and `README.md` for the submission rules.
- On this side: `src/RecordingParser.cpp`, the recording and EPG fetch code in `src/DispatcharrClient.cpp` and
  `src/PVRDispatcharr.cpp`, both plugins' `plugin.json` and `Plugin` classes, and the existing decisions in
  `docs/RECORDINGS.md`, `docs/EPG.md`, `docs/API_NOTES.md` and `docs/CLOSED_ITEMS.md`, so nothing below re-proposes
  something already ruled out.

## Dispatcharr core

### Recording enrichment the addon ignores

`apps/channels/tasks.py` (the artwork prefetch task, around the `_resolve_poster_for_program` call) writes these into a
recording's `custom_properties` whenever the matched EPG programme supplies them: `poster_url` and `poster_logo_id`,
`rating` and `rating_system`, `season`, `episode`, `onscreen_episode`; the capture task adds `stream_info` (video codec,
resolution, frame rate, video and audio bitrates, audio codec, sample rate, channels) from the live stream metadata; and
`RecordingViewSet` has a `POST .../recordings/{id}/refresh-artwork/` action that re-runs the poster pipeline.
`src/RecordingParser.cpp` reads `program.title`, `program.sub_title` and `program.description` (and the same three keys
at the top level for an older shape) and nothing else; in `src/PVRDispatcharr.cpp` only channels and EPG tags get
`SetIconPath()`, so a recording has no artwork in Kodi, no season or episode number and no parental rating.
`poster_logo_id` points at the same logo-cache endpoint the addon already turns into a URL for channel logos.

Two cautions from this project's own history before mapping them: `docs/RECORDINGS.md` found a daily programme whose
every airing carried the same date and no per-episode identifier, which is why `GetEPGForChannel()` guards `FirstAired`;
season and episode from `custom_properties` deserve the same scepticism. And `docs/EPG.md`'s "Not mapped, deliberately"
paragraph explains why parental ratings were left out of the guide (several rating boards, one integer field in Kodi);
`rating_system` is now available alongside `rating`, which may allow a per-system mapping, or the same decision may stand.
Open item: "Show a recording's poster, season and episode and rating from Dispatcharr's enrichment".

### Comskip metadata on the recording itself

After comskip runs, `custom_properties.comskip` holds `status` (`completed`, `skipped`, `error`), `mode` (`mark` or
absent for cut), `edl` (the file's basename only), `commercials`, `segments_kept` and `ini_path`, with the reasons
`comskip_not_installed`, `comskip_failed`, `edl_not_found` and `duration_unknown` on failure (`comskip_process_recording`
in `apps/channels/tasks.py`). The EDL *content* is still not served by any endpoint, so the `recording_edl` plugin keeps
its purpose. But the addon does not read the `comskip` key at all, so it asks the plugin for every recording it opens,
including the ones where comskip never ran, failed, or ran in cut mode (where there are no markers to show by design).
Open item: "Skip the EDL plugin call when the recording's comskip metadata says there is nothing to fetch".

### Live-proxy events name the active stream

Since 0.32.0 the `channel_buffering`, `channel_failover`, `channel_reconnect`, `channel_error` and `stream_switch` system
events carry the active stream. The addon follows a different set of WebSocket message types (recordings and refresh
events) and the `timeshift_buffer` plugin learns about a failing stream only by polling its own ffmpeg. Recorded as an
input to the open item "One stated stalled-stream rule per read path, with one threshold" and to the plugin event-hook
item below, not as a separate item.

### The status endpoint is admin-only, and what it carries

`GET /proxy/ts/status/{channel_id}` is decorated `@permission_classes([IsAdmin])`
(`apps/proxy/live_proxy/views.py`). Its detailed body (`get_detailed_channel_info` in `channel_status.py`) includes
`video_codec`, `resolution`, `source_fps`, `ffmpeg_speed`, `stream_profile`, `stream_name`, `client_count`, uptime, total
bytes and a computed bitrate, all read from the Redis channel metadata whose field names are in
`apps/proxy/live_proxy/constants.py` (`ChannelMetadataField`). So a Kodi signal-status panel fed from it would work for
admin accounts only, which is now written into the open item "Fill Kodi's signal-status panel with the active stream's
account and profile".

### No disk usage anywhere

There is no `shutil.disk_usage`, no `statvfs` and no storage endpoint in `apps/` or `core/`. `CoreSettings` has DVR path
templates, comskip options and the pre/post offsets, nothing about space. The open item "Does Dispatcharr expose
recording-disk usage" is closed on this evidence (`docs/CLOSED_ITEMS.md`).

### Series-rule match modes, confirmed values

`SeriesRulesAPIView` (`apps/channels/api_views.py`) validates `title_mode` against `exact`, `contains`, `search` and
`regex` (default `exact`) and `description_mode` against `contains`, `search` and `regex` (default `contains`), and
accepts `untagged_is_new`. The open item "Let a series rule's title and description match modes be set from Kodi" now
carries these values instead of the two the addon had seen in real rules.

### `/output/epg` parameters

`epg_endpoint` in `apps/output/views.py` reads `tvg_id_source` (default `channel_number`; also `tvg_id` and `gracenote`),
`days` (default: the account's `epg_days` custom property, 0 meaning no limit), `prev_days`, `cachedlogos` and `direct`.
`GetXmlTvGuide()` sends only `prev_days`; the parser's assumption that `<channel id>` is the channel number (recorded in
`docs/API_NOTES.md`'s table) rests on the server default. Open item: "Pin the XMLTV export's channel-id source and day
window explicitly".

### Looked at and left alone

- `/api/epg/grid/` (0.31.0): programmes overlapping a caller-chosen window, as streamed JSON `{"data": [...]}`. Built for
  the web guide; it would only be interesting if the one multi-megabyte XMLTV fetch ever became a problem, and the
  XMLTV path is the tested one.
- `/api/epg/current-programs/`: now-playing for a set of channel UUIDs; Kodi derives this from the guide it holds.
- The catch-up session's `expires_at` response field (unused; the session cache has its own TTL handling) and the
  `position` endpoint (already ruled cosmetic in `docs/CLOSED_ITEMS.md`). `apps/timeshift` is the catch-up
  implementation only; Dispatcharr has no live rewind buffer of its own, so `timeshift_buffer` remains the only one.
- `is_radio` on `Channel` (confirmed in `apps/channels/models.py`; already an open item from the pvr.hts comparison).
- VOD, HDHomeRun emulation, backups, channel profiles and system notifications: out of scope or already recorded as
  unused features in `docs/API_NOTES.md`.

## The plugin platform

### What the two plugins already do right

Checked against `Plugins.md` and `apps/plugins/loader.py`: both ship `plugin.json` with `author` and `help_url`; both use an
`info` field for the in-UI explanation; every destructive action carries `confirm` (two in each plugin);
`timeshift_buffer` implements `stop()`, which Dispatcharr calls when a plugin is disabled, deleted or reloaded;
`recording_edl` is stateless and needs none; neither asks for a Dispatcharr URL or credentials, which the platform docs
forbid. The loader's `_build_context` passes `settings` (merged with field defaults), `logger` and `actions`; nothing
else, so there is no request user or client address to use. `POST /api/plugins/plugins/{key}/run/` resolves its
permission through the global `permission_classes_by_method` (`apps/accounts/permissions.py`: `POST` is `IsAdmin`), so
the admin requirement both READMEs state is Dispatcharr's rule, confirmed from source.

### Event hooks

An action whose definition has an `"events": [...]` list is run by `log_system_event()` when one of those system events
fires (`iter_actions_for_event` in the loader; dispatched on a gevent greenlet under uWSGI, synchronously elsewhere). The
event names (`core/models.py`, `SystemEvent.event_type`): `channel_start`, `channel_stop`, `channel_buffering`,
`channel_failover`, `channel_reconnect`, `channel_error`, `client_connect`, `client_disconnect`, `recording_start`,
`recording_end`, `stream_switch`, `epg_refresh`, `epg_download`, `epg_error`, `epg_blocked`, `login_success`,
`login_failed`, `logout`, `vod_start`, `vod_stop`. Neither plugin subscribes to any. Handlers must stay short (the
platform docs say to defer heavy work to Celery). Open item: "Use Dispatcharr's event hooks in the plugins".

### Database connections from plugin threads

`run_action` and `stop_plugin` call `close_old_connections()` after the plugin returns, but only for the calling greenlet;
`Plugins.md` says any thread or greenlet a plugin spawns that touches the ORM must call it in its own `finally`.
`timeshift_buffer` runs its own threads (the file server, the reaper). Whether any of them touches the ORM was not
checked here. Open item: "Confirm no plugin thread uses the ORM without closing its connection".

### Periodic tasks

Plugins may define Celery `@shared_task`s driven by beat; 0.25.0 fixed their registration after worker restarts. The
sanctioned scheduler, if the reaper thread ever becomes a problem; noted, not proposed.

### The Plugin Hub and the official catalogue

Since 0.23.0 an admin can browse, install and update plugins from repositories inside Dispatcharr
(`/api/plugins/repos/...`); the official repository's manifest is pre-configured. The catalogue accepts *external*
plugins (`Plugins/main`'s `CONTRIBUTING.md`): a PR adding `plugins/<slug>/plugin.json` with `source_type: "external"`,
a `source_url` that is an HTTPS link to a zip and contains a `{version}` placeholder, `repo_url`, an OSI-approved SPDX
license (GPL-2.0-or-later qualifies), `author` equal to the submitting GitHub username, optional
`min_dispatcharr_version`; on merge the registry downloads the zip, computes checksums, re-hosts it as a release on the
registry and signs the manifest; every version bump is another PR. Alternatively a third-party repository manifest can
be hosted anywhere (`Plugin_repo.md`; the flat format needs only `registry_name` and absolute `latest_url`s, and the
name must not contain "official" or "dispatcharr").

The fit problem: `{version}` is the *plugin's* version, and this project attaches the plugin zips to addon-version
releases (0.12.0 carries `timeshift_buffer` 0.8.14, refreshed from 0.8.13 on 2026-10-10), so there is no URL templated on the plugin version to point at.
Listing would need per-plugin GitHub releases or tags, or a self-hosted manifest with absolute URLs updated by the
release gate. Open item (a decision, since it changes what gets published and where): "List the two plugins in
Dispatcharr's Plugin Hub".

### The catalogue itself

Thirty-five plugins; none overlaps either of ours (no timeshift, comskip, EDL or catch-up plugin). Related entries:

- **Clapparr** writes Kodi-format NFO sidecars, posters and episode thumbnails for finished recordings, triggered by
  `recording_end`. Complementary to this addon (it serves anyone scanning the recordings directory as a library) and an
  example of a `recording_end` hook and of per-plugin releases that satisfy the `{version}` rule.
- **Audio Buffer Tuner** lowers the TS-proxy prebuffer for chosen channel groups to speed up channel start. A server-side
  knob that bears directly on the zap latency measured on this project's devices; nothing for the addon to do, but
  worth knowing when a user reports slow zaps.
- **Reservoarr**, **Profilarr**, **Segmentarr**: stream profiles that absorb CDN gaps and timestamp breaks. The server-side
  answer to stalls a client cannot fix; a candidate pointer for `docs/TROUBLESHOOTING.md` (open item: "Point the
  troubleshooting notes at the server-side stream-profile plugins").
- **Dustarr** records which channels are watched from `client_connect` events; **Newsflasharr** routes other plugins'
  events to notification services. Neither concerns this addon.

Most catalogue plugins ship no tests; both of ours do.

## Dispatcharr's own tests

`tests/` at the top level plus `tests/` directories in fifteen apps, pytest with Django (`apps/timeshift/tests` covers
its views, sessions and stats with the Django test client). Nothing to borrow for the plugins: this project deliberately
does not test against a real Django or Redis, and the catalogue plugins that do have tests follow the same monkeypatch
approach as `dispatcharr-plugin/*/tests`.
