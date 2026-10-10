# What pvr.hts (Tvheadend) does that this addon does not, and which of it transfers (2026-10-10)

*Engineering notes, not user documentation. Prompted by a "can we borrow anything from the Tvheadend addon" question. The
earlier, narrower pass of the same kind is `docs/RECORDINGS.md`'s "Recording-management feature gaps vs. TVHeadend, checked
against Dispatcharr's real API (2026-09-08)" section, which covered only the recording-management capabilities; this one
covers the whole addon.*

## What was compared, and how

`pvr.hts`, the Kodi-team Tvheadend client, `Omega` branch at commit `3afc127` (2025-12-04, version 21.2.6), read side by
side with this addon's `Omega`. Nothing was run; every statement below about pvr.hts is from reading its source (its PVR API
overrides, the `PVRCapabilities` it declares, its two settings files and their English strings, its changelog back to the
Kodi 18 era), and every statement about Kodi's side is from reading the Kodi 21 source in the local build tree
(`xbmc/pvr/addons/PVRClient.cpp`, `xbmc/pvr/addons/PVRClients.cpp`). Where a feature would need something from Dispatcharr
that this project has not confirmed live, that is said rather than assumed.

The shape of the two addons differs in a way that decides most of the comparison before any feature does. pvr.hts speaks a
persistent binary protocol (HTSP) over one socket, receives channel, tag, DVR and EPG changes as pushed messages, and does
its own demuxing (`SetHandlesDemuxing(true)`, a `DemuxRead` loop fed by a packet buffer). This addon polls a REST API,
receives a narrower set of push events over a WebSocket, and hands Kodi either a stream URL or a byte stream for Kodi's
own demuxer. So pvr.hts features that are really protocol features (per-event EPG push, trick-play speeds negotiated with
the server, descramble and signal data from the tuner) have no counterpart to build, and the rest divides into what
Dispatcharr can back and what it cannot.

## Side by side

| pvr.hts has | This addon | Dispatcharr backing | Outcome |
|---|---|---|---|
| Reports its connection state to Kodi (connecting, connected, unreachable, access denied, disconnected) through the `ConnectionStateChange` callback | Never calls it; shows its own notifications (`kodi::QueueNotification`, 14 call sites across `src/PVRDispatcharr.cpp` and `src/DispatcharrClient.cpp`) | Not needed; the addon already knows when a request fails, is refused or recovers | **Open item** ("Report the connection state to Kodi instead of hand-rolled notifications") |
| Radio channels (`SetSupportsRadio(true)`, channels and groups flagged radio) | `SetSupportsRadio(false)`; every channel and group is `SetIsRadio(false)` | Dispatcharr 0.32.0 added `is_radio` / `effective_is_radio` to the channel (see `docs/API_NOTES.md`'s "Dispatcharr 0.32.0 (2026-10-07)" section) | **Open item** ("Map Dispatcharr's radio flag to Kodi's radio channel type") |
| Timer rules match the title as "contains" or as a regular expression (one add-on-wide option, "autorec_use_regex"), and a rule can also search the full EPG text (`PVR_TIMER_TYPE_SUPPORTS_FULLTEXT_EPG_MATCH`) | A series rule is created with `title_mode`, `description` and `description_mode` left blank (`AddTimer()` in `src/PVRDispatcharr.cpp`), although `src/TimerRuleParser.cpp` reads them back | Dispatcharr's series rule carries `title_mode`, `description` and `description_mode` (confirmed shape in `docs/API_NOTES.md`'s "Confirmed against a live instance" table) | **Open item** ("Let a series rule's title and description match modes be set from Kodi") |
| One explicit stalled-stream rule: no demux packet for N seconds while not paused (default 10, the "stream_stalled_threshold" setting) drops the connection and reconnects once | Each read path has its own bounds (the sliced live tail wait, the segment-fetch give-up rules, `IsAtEndedTail`), with different constants and no user-facing threshold | Not needed | **Open item** ("One stated stalled-stream rule per read path, with one threshold") |
| `GetBackendHostname` | Not overridden (`GetBackendName()`, `GetBackendVersion()` and `GetConnectionString()` are) | The configured host is already known | **Open item** ("Report the backend hostname") |
| Wake-on-LAN of the backend before connecting ("wol_mac" setting; `kodi::network::WakeOnLan`) | Nothing | Not needed | **Open item**, deferred ("Wake-on-LAN for a Dispatcharr host that sleeps") |
| `GetDriveSpace` (Kodi's PVR status shows the backend's recording disk) | Not overridden | Unknown: no Dispatcharr endpoint for storage statistics is recorded in `docs/API_NOTES.md` | **Open item**, needs a check ("Does Dispatcharr expose recording-disk usage") |
| `GetSignalStatus` filled with provider, service and mux names, shown in Kodi's player information panel | Not overridden | The active stream of a channel is reported in Dispatcharr's live-proxy events, which the addon currently ignores (noted as unused in the 0.32.0 section of `docs/API_NOTES.md`) | **Open item**, needs a check ("Fill Kodi's signal-status panel with the active stream's account and profile") |
| Multiple backend instances (`instance-settings.xml`, pvr.hts 20.4.0, with a one-time migration of pre-instance settings in its `addon.cpp`) | One instance; `docs/API_NOTES.md`'s "Single-instance assumption: partially hardened, not fully" section lists what assumes it | Not needed | **Open item**, deferred ("Multiple Dispatcharr servers from one Kodi") |
| Predictive tuning: when the user zaps, the next and previous channel by number are subscribed in advance so the following zap is instant ("pretuner_enabled", "total_tuners", "pretuner_closedelay") | Nothing | Every open stream costs a provider connection slot; running out of slots is a documented, reproduced failure mode here (`CHANGELOG.md`, the concurrent-stream-limit entries) | **Not adopted.** The zap latency it addresses was measured on this project's devices (about 1.2 to 3.6 s on the Linux test client), so the problem is real, but pre-opening streams is the wrong fix for an IPTV backend |
| Recording play count and last played position stored on the server ("dvr_playstatus" setting), per-recording lifetime, undelete, recording priority | Kodi keeps play status locally; none of the rest | None: already checked against Dispatcharr's source, see the `docs/RECORDINGS.md` section named at the top | **Already ruled out** |
| Per-timer start and end margins (`PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN`) | Pre/post padding is Dispatcharr's global pair, synced through the `recording_pre_offset_minutes` / `recording_post_offset_minutes` settings | Global only | **Not applicable** |
| Timer rules keyed on an EPG series link (`PVR_TIMER_TYPE_REQUIRES_EPG_SERIESLINK_ON_CREATE`) | Series rules keyed on title plus `tvg_id` | XMLTV as Dispatcharr serves it carries no series identifier | **Not applicable** |
| Per-event EPG push (`SetSupportsAsyncEPGTransfer`, `EpgEventStateChange` on each changed programme) | A WebSocket message says when an EPG source finished refreshing, and the guide is re-fetched | Dispatcharr's guide is a bulk XMLTV export, not a delta feed | **Not applicable** |
| Own demuxer, server-negotiated trick-play speeds, descramble information | Kodi's demuxer; speed handled by Kodi against a seekable byte stream; `SetSupportsDescrambleInfo(false)` | DVB/HTSP specific | **Not applicable** |
| `OnSystemSleep` closes every stream and blocks reconnects until `OnSystemWake` | Only `OnSystemWake()` is overridden, to nudge the WebSocket thread; the reason is in the comment above it in `src/PVRDispatcharr.h` and in `docs/API_NOTES.md`'s "OS sleep/wake and the real-time-updates WebSocket" section | Stateless HTTP needs nothing on sleep | **Already decided** |
| EPG tags also carry `SetSeriesLink`, `SetParentalRating`, `SetStarRating`, `SetOriginalTitle`, `SetIMDBNumber` | Not set | `docs/EPG.md`'s "Not mapped, deliberately" paragraph: no star ratings in a real multi-megabyte guide fetch, and parental ratings come from several rating boards with no honest single-integer mapping; XMLTV here has no IMDB ids or original titles | **Already decided** |
| Separate connect and response timeouts ("connect_timeout", "response_timeout") | One `timeout` setting | Not needed | **Not adopted**; nothing observed so far needed the split |
| Translations in about seventy languages through Weblate | `en_gb` only | Not applicable | **Project-level gap**, accepted for an unofficial, single-maintainer addon; a translation would be taken as a contribution |

## What pvr.hts does not have

pvr.hts has no unit tests (its CI builds and packages; there is no test target in its `CMakeLists.txt`), no equivalent of
`tests/glue/` and no engineering history beyond the changelog. This project's Catch2 and pytest suites, the glue harness
and `docs/` have no counterpart there, so nothing on that side was there to borrow. Its release workflows
(`increment-version.yml`, `changelog-and-release.yml`) automate the version bump and changelog; this project's release is
deliberately manual because of the privacy gate (`CLAUDE.md`, "Releasing").

## Kodi-side facts confirmed while reading (Kodi 21)

These matter for the connection-state item and were checked in the Kodi source rather than assumed:

- A client starts at `PVR_CONNECTION_STATE_UNKNOWN` and stays there if the addon never reports a state
  (`PVRClient.cpp`, the constructor and `SetConnectionState`). That is this addon's situation today, and Kodi tolerates
  it.
- When an addon reports `PVR_CONNECTION_STATE_CONNECTED`, Kodi re-reads the addon's properties, including its
  `PVRCapabilities` (`SetConnectionState`: "some will only be available after add-on is connected to backend"). This
  addon's `GetCapabilities()` depends on `m_dvrManageAllowed`, which is only known after the first login, so reporting
  `CONNECTED` after login is the sanctioned way to make Kodi pick up the real DVR capabilities.
- Kodi's own notifications for each state (`PVRClients.cpp`, `ConnectionStateChange`): "Server is unreachable" is
  deliberately not shown for the very first failure after `UNKNOWN` or `CONNECTING` (the backend-not-up-yet-at-boot
  complaint), "Connection established" is not shown for the first connect, and an addon-supplied message replaces the
  stock one when given. A client that reported `CONNECTING` from `UNKNOWN` is ignored by the PVR manager until it
  reports `CONNECTED`.
