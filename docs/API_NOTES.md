# Dispatcharr API notes

*This file and the ones it indexes below are engineering/development
notes -- root causes, real API behavior confirmed live, decisions tried
and reverted -- kept as project history, not user documentation. If
you're looking to install or configure the addon, see the main
[README.md](../README.md) instead.*

`src/DispatcharrClient.cpp` talks to Dispatcharr's own REST API. Dispatcharr
ships a live OpenAPI document at `http://<your-server>:9191/api/schema/`
(and a Swagger UI at `/swagger/`) -- **check that first** against anything
below before shipping a build, since Dispatcharr is a young, fast-moving
project and its schema has changed across recent releases (v0.23, v0.25 both
touched API schemas).

## Confirmed against a live instance

Verified 2026-08-30 against a real Dispatcharr instance's own
`/api/schema/` and actual API responses (not just docs/issues):

| Purpose | Method | Path |
|---|---|---|
| Login | POST | `/api/accounts/token/` (returns `{access, refresh}`) |
| Refresh | POST | `/api/accounts/token/refresh/` |
| API key auth | header | `X-API-Key: <key>` -- accepted as an alternative to the JWT bearer token on nearly every endpoint. Read the account's existing key via `GET /api/accounts/api-keys/` (`{"key": <str|null>}`, any authenticated role, does not replace it); generate one via `POST /api/accounts/api-keys/generate/` **only when the account has none**, since that call overwrites the account's single key and silently revokes it for every other client. **Accounts with restricted ("streamer") permissions may not be able to log in via `/api/accounts/token/` at all** (confirmed: a real streamer-role account got "No active account found" from the login endpoint) -- if login fails for a permissions reason rather than a wrong-password reason, an API key is the working alternative. The addon implements both: it logs in with username/password to get a JWT pair, then obtains and caches the account's API key (`ObtainApiKey()`/`HasApiKey()` in `DispatcharrClient.cpp` -- adopts the existing key, generating only when there isn't one), re-obtaining it automatically on a 401. |
| List channels | GET | `/api/channels/channels/` -- returns **every** channel regardless of the caller's account; there's no server-side, per-account restriction based on channel profile membership (confirmed against the real `ChannelViewSet.get_queryset()`: the base queryset is just `Channel.objects.all()`/`super().get_queryset()`, and `channel_profile_id` is only applied as a filter if the *caller* passes it as a query param -- opt-in curation, not access control). See "Channel profiles" below. |
| Channel groups | GET | `/api/channels/groups/` (**not** `/api/channels/channel-groups/`, which doesn't exist -- Dispatcharr's SPA serves its own `index.html` for unmatched routes, so that guess returned a misleading HTTP 200 of HTML, not JSON) |
| List streams | GET | `/api/channels/streams/` |
| Channel logo | GET | `/api/channels/logos/{id}/cache/` -- `{id}` is the channel's `logo_id`, **not the channel's own id** |
| XMLTV guide | GET | `/output/epg` -- **its `<channel id="...">` is the channel's `channel_number`, not `tvg_id`** (confirmed: Channel G, `channel_number` 1 and `tvg_id` "ChannelG.example", appears in the XMLTV as `<channel id="1">`; checked against two other channels too). This addon originally matched EPG programmes to channels by `tvg_id`, which never matched anything -- see `XmlTvParser.h`. |
| Live stream | GET | `/proxy/ts/stream/{channel_uuid}` -- confirmed via a real instance that a channel's `uuid` field is accepted here (a wrong identifier would 404 immediately; instead it attempted to reach the upstream source and only failed there) |
| Stream session management | GET/POST | `/proxy/ts/status`, `/proxy/ts/status/{channel_id}`, `POST /proxy/ts/stop/{channel_id}`, `POST /proxy/ts/change_stream/{channel_id}`, `POST /proxy/ts/next_stream/{channel_id}` -- exist and look relevant to channel-switching reliability, but this addon doesn't currently call any of them (see the "channel switching" note below) |
| Recordings list/create | GET/POST | `/api/channels/recordings/` -- **bare array** on GET, not `{results:[...]}`. `Recording` has only `id`, `start_time`, `end_time`, `task_id`, `custom_properties`, `channel` -- **no title/subtitle/description/duration/in-progress fields at all.** Those are derived: duration from `end_time - start_time`, in-progress from `start_time <= now < end_time` (overridden by `custom_properties.status == "recording"` when present -- see `docs/RECORDINGS.md`), and title/subtitle/description confirmed (against real recordings, not just guessed) as `custom_properties.program.{title,sub_title,description}` when Dispatcharr auto-enriches from the airing EPG programme, falling back to a flat `custom_properties.{title,sub_title,description}` for anything that set them directly -- see `docs/RECORDINGS.md`'s first entry for how that was confirmed and why sending your own `custom_properties` on create replaces rather than merges with the auto-enrichment. |
| Recording delete | DELETE | `/api/channels/recordings/{id}/` |
| Recording playback | GET | `/api/channels/recordings/{id}/file/` for a completed recording -- **not anonymous** despite the schema listing anonymous access as an allowed security scheme; a Bearer token or `X-API-Key` header is actually required (confirmed: flat 403 without one), and this endpoint properly honors `Range`. An in-progress (still-recording) one instead redirects to `.../hls/index.m3u8` -- same auth requirement, but each individual `seg_NNNNN.ts` segment **ignores `Range` entirely** and always serves its full body with a 200 regardless of what was requested (confirmed live; see `docs/RECORDINGS.md`'s in-progress-recording section for the two bugs this caused and their fixes) -- use `HEAD`/`Content-Length` to size a segment, and cache/slice client-side rather than relying on a ranged GET against it for partial reads. |
| Series rule evaluation | POST | `/api/channels/series-rules/evaluate/` -- body `{tvg_id}`, both optional |
| Series rules list | GET | `/api/channels/series-rules/` -- returns **`{"rules": [...]}`**, not a bare array or `{results:[...]}`. Per-item fields confirmed against a real populated rule: `{mode, title, tvg_id, channel_id, title_mode, description, description_mode}` -- **no numeric id field at all** (see `docs/RECORDINGS.md` for why `GetTimers()` hashes `(title, tvgId)` instead of using one). |
| Series rule create | POST | `/api/channels/series-rules/` -- body is `{title, tvg_id?, channel_id?, mode?, title_mode?, description?, description_mode?, untagged_is_new?, epg_source_id?}`. **Not** `{channel, title_pattern}** -- confirmed against the live `SeriesRuleRequest` schema. `channel_id`/`tvg_id` are both optional (channel_id "defaults to lowest-numbered channel for the EPG" if omitted). |
| Series rule delete | DELETE | `/api/channels/series-rules/?title=...&tvg_id=...&epg_source_id=...` (query params) -- confirmed against the live schema. **There is no `/api/channels/series-rules/{id}/` route**; series rules have no path-addressable id at all. |
| Catch-up session | POST | `/api/catchup/sessions/` -- body `{channel_uuid, start (ISO-8601), duration (minutes, optional)}`; response's `playback_url` is a **relative path**, prepend `BaseUrl()`. Confirmed end-to-end against a real instance: creates a session-bound URL that streams real MPEG-TS data immediately with no further auth. Per Dispatcharr's own docs, the session stays valid via a 10-minute *sliding* idle window (refreshed by each range/seek request), so unlike embedding a JWT directly in the URL (`GET /proxy/catchup/{uuid}?start=...&token=...`, also confirmed working but not used here), it won't expire mid-playback of a long programme. A separate `POST /api/catchup/sessions/{session_id}/position/` (body `{position_secs, paused?}`) exists too, but checked against its real source (`apps/timeshift/api_views.py`) and it's purely cosmetic for *Dispatcharr's own admin stats dashboard* -- "does **not** seek the provider stream," doesn't affect this addon's playback at all. Its one side effect that could matter is also refreshing that same idle TTL, but since ordinary Range requests already do that, it would only help a session survive a pause longer than 10 minutes with zero reads -- narrow enough that it's not implemented. |
| Backend version | GET | `/api/core/version/` -- **public, no auth needed at all** (`AllowAny`), returns `{"version": ..., "timestamp": ...}` straight from Dispatcharr's own `version.py`. Confirmed against the live source (`core/api_views.py`), not just the schema. **Wired up (2026-09-09):** `DispatcharrClient::GetServerVersion()`, fetched once at startup and cached; `GetBackendVersion()` in `PVRDispatcharr.cpp` reports the real value now (confirmed live: `0.30.0`, shown in Kodi's own System Info -> PVR service panel), no longer this addon's own placeholder protocol string. |
| Full timezone list | GET | `/api/core/timezones/` -- **requires auth** (confirmed live: 401 without it, matching its view's `Authenticated()` permission -- unlike the version endpoint above, not `AllowAny`), returns `{"timezones": [...], "grouped": {...}, "count": N}`, `timezones` being `sorted(pytz.common_timezones)`, ~440 real IANA names (confirmed against the live source, `core/api_views.py`'s `TimezoneListView`, not just a live call). **Wired up (2026-09-09), scope narrower than "genuinely comprehensive" turned out to be honest:** the real bottleneck was never the zone *name* list, it's DST *rule* coverage -- this addon can only auto-compute a correct offset for a zone matching one of two hand-verified rule families (`kUsCanada`, `kEu`) or a confirmed no-DST zone, and Southern Hemisphere zones need a third (inverted-season) rule family this addon doesn't have, so they're deliberately still excluded rather than risking a wrong offset half the year. `DispatcharrClient::GetSupportedTimezones()` calls this endpoint; used in the startup timezone-sync diagnostic to tell "a real IANA zone, no DST rule for it yet" apart from "not a recognized zone at all" for whatever Dispatcharr reports itself configured to. Separately, `kKnownTimeZones` (`TimeZoneUtil.cpp` as of 2026-09-13, pulled out of `DispatcharrClient.cpp` so this logic is unit-testable standalone -- see `tests/test_timezone_util.cpp`) itself was broadened from ~25 to ~50 entries within those two rule families -- see `docs/RECURRING_RULES.md`. |
| Current user / own access level | GET, PATCH | `/api/accounts/users/me/` -- confirmed against the real source (`apps/accounts/api_views.py`'s `UserViewSet.me`): needs only ordinary authentication, not admin, unlike the admin-only user list/detail routes on the same ViewSet. Returns the full `UserSerializer` shape for your own account, including `user_level` (`0` Streamer / `1` Standard / `10` Admin, from `apps/accounts/models.py`'s `User.UserLevel`) and `is_staff`/`is_superuser`. Confirmed against `apps/accounts/permissions.py` that `IsAdmin` -- the exact permission class gating `CoreSettingsViewSet.update`/`partial_update`, i.e. the recording-padding write this addon already makes -- is precisely `user_level >= 10`. **Wired up (2026-09-09):** `DispatcharrClient::IsCurrentUserAdmin()`, checked once at startup and written into a hidden `dispatcharr_is_admin` addon setting, which gates (via the same `<dependencies>` mechanism already used elsewhere in `settings.xml`) whether `recording_pre_offset_minutes`/`recording_post_offset_minutes` render enabled -- see `README.md`'s recording-padding bullet. |

**Update (2026-09-26, a 26th-pass audit): the "no server-side, per-account
restriction" claim above and the "it's opt-in" claim just below are both
now stale against a current Dispatcharr version, confirmed against
Dispatcharr's own real current upstream source (cloned into a
scratchpad, never committed to this repo -- stronger than the API shape
alone, not the same standard as a live test).** `ChannelViewSet.get_queryset()`
now does automatically restrict a non-admin (`user_level < 10`) account's
own `list`/`get_ids`/`summary` results: to channels at or below the
caller's own `user_level` (plus hiding adult content per that user's own
preference), and, if the account has any `ChannelProfile`s assigned at
all, to only the *enabled* memberships within those assigned profiles --
without the caller ever passing `channel_profile_id` explicitly. Every
`list`-style read (matching the actions above) also now excludes
`hidden_from_output` channels by default (`visibility_filter="active"`),
regardless of account level -- a single `retrieve`/`update`/`destroy` by
id still reaches a hidden channel, matching the "frontend can unhide"
comment in that code. An admin account (this addon's own documented,
recommended setup -- see the login-endpoint note above about a
restricted "streamer" account's own separate, different problem) is
unaffected by the `user_level`/profile restriction either way. This
doesn't necessarily mean this addon's own channel list is currently
missing anything (that depends entirely on which account level/profile
assignment a given install actually uses), just that the "opt-in
curation only, not access control" framing no longer fully describes
current Dispatcharr for a non-admin account -- worth a live check
against a real non-admin, profile-assigned account before relying on
either claim as still fully accurate.

## Dispatcharr 0.32.0 (2026-10-07)

Checked by reading the upstream diff of `v0.31.0..v0.32.0` against every endpoint and field this addon and its two plugins
use, then live against a 0.32.0 server (a real recording, a real client on a 64-bit phone, the 32-bit N2+ and a second phone).
**Nothing the addon relies on changed incompatibly.** What changed, and what was confirmed:

- **In-progress recording playlist:** the DVR muxer now runs with `-hls_playlist_type event` (still `-hls_list_size 0`,
  `omit_endlist`, `append_list`, `independent_segments`), so the playlist carries `#EXT-X-VERSION:6`,
  `#EXT-X-PLAYLIST-TYPE:EVENT`, `#EXT-X-INDEPENDENT-SEGMENTS` and an `#EXT-X-DISCONTINUITY` ahead of the first segment (and
  after a provider splice), durations to six decimals, absolute segment URLs, and `Cache-Control: no-cache` on the response. The
  server still adds `#EXT-X-ENDLIST` itself when it finalizes, the `dvr:hls_viewer:{id}` key (20 s) still holds the HLS directory,
  and the stored statuses are unchanged (`scheduled`, `recording`, `completed`, `stopped`, `interrupted`; a finished recording
  also gains `remux_success` in its `custom_properties`). The addon already ignored every tag it did not need, so no code changed;
  `tests/test_m3u8_segment_parser.cpp` pins the captured shape. Live: a 9-minute recording played in real time through its whole
  length on the phone and the stream ended by itself at the real end (the only audio-sync lines were at the final teardown), with
  the server finishing `completed`, remuxed, file present.
- **`recording_end`:** now fires after post-processing with `outcome` (`success`, `failed`, `cancelled`), `status`,
  `remux_success` and `interrupted_reason`; the old `interrupted` boolean is gone. This is a Connect/system event; the
  websocket `updates` types the addon follows (`recording_started`, `recording_ended`, `recording_stopped`,
  `recording_extended`, `recording_updated`, `recording_cancelled`, `recordings_refreshed`) are the same set in both versions, and the
  real-time push was confirmed live (a recording created outside Kodi appeared with no restart).
- **Channels:** `is_radio` and `effective_is_radio` are new on the channel (and a stream filter `is_radio`, a channel filter
  `only_radio`); nothing was removed or renamed, `streams` is still a list of ids, and the list is still unpaginated for the
  addon's request (a full-size lineup, `next` null). The addon does not read the radio flag yet, so radio channels still reach Kodi
  as TV; mapping it to Kodi's radio type is a possible feature, not a compatibility issue (now an open item, "Map Dispatcharr's
  radio flag to Kodi's radio channel type" in `docs/OPEN_ITEMS.md`, from the 2026-10-10 pvr.hts comparison). Sorting, search and filtering now use the
  override-aware values (what the addon already reads as `effective_*`).
- **Catch-up:** `POST /api/catchup/sessions/` is untouched; the server-side `Range: bytes=0-` restart and scrub-offset fixes are
  in the proxy path. Live on the phone with `inputstream.ffmpegdirect` seeking on: a 2-minute forward seek landed in 36 s, a 90-second
  back seek in 40 s and a 5-minute forward seek in 22 s, each about 20 to 25 s short of its target (the imprecision
  `docs/CATCHUP.md` already records), none stuck and none restarting at the original position. `utc` is accepted as an alias for
  `start` and the 301 session redirect is now client-cacheable; neither is used by the addon.
- **Settings and accounts:** `GET /api/core/version/`, the settings rows, `GET /api/accounts/users/me/` (still `api_key` and
  `custom_properties` with `dvr_access` and `catchup_enabled`), token and API-key endpoints are unchanged. New and unused:
  `POST /api/accounts/auth/proxy-login/` (reverse proxy header auth, off unless `DISPATCHARR_TRUSTED_PROXIES` names the proxy).
  Locked stream and output profiles can no longer be edited through the API.
- **Plugins:** `RedisClient.get_client()` gained optional arguments (`max_retry_interval`, `disable_persistence`) and is cached;
  called with none, as `timeshift_buffer` does, it behaves as before. Live: `timeshift_buffer` 0.8.13 and `recording_edl` 0.2.3 run
  on 0.32.0, `list_buffers` answers, and server-side live timeshift played, seeked and paused on three devices.
- **Not used by the addon:** the M3U and XC output changes (radio flag, catch-up tags, `catchup-timezone`), richer live-proxy events,
  the stream-profile renames, Redis TLS and `REDIS_URL` handling, and the logging changes. The XMLTV export (`/output/epg`) has no
  diff.

## Channel profiles: a real curated-lineup feature, unused by this addon

Dispatcharr supports named, curated channel subsets --
`ChannelProfile` (just a name) plus `ChannelProfileMembership`
(`channel_profile` x `channel`, with its own `enabled` toggle), and a
user account can be assigned one (`apps/accounts/migrations/
0002_remove_user_channel_groups_user_channel_profiles_and_more.py`
confirms accounts moved to this model, not the reverse). Checked the
real `ChannelViewSet.get_queryset()` (`apps/channels/api_views.py`) to
see whether this is enforced automatically or opt-in: it's opt-in --
`GET /api/channels/channels/` returns every channel regardless of
account by default; passing `?channel_profile_id=<id>` is what
actually filters. So this isn't an access-control gap (nothing this
addon does bypasses a security boundary that exists), it's a missed
curation feature: Dispatcharr can define e.g. a "Kids" or "Sports
only" lineup, and right now this addon has no way to let a user pick
one -- it always pulls the full, unfiltered channel list. `GET
/api/channels/profiles/` lists the available profiles (confirmed live
against a real instance: returns `[]` here, since this is a
single-user setup with none configured -- the feature is real and
available regardless of this particular instance not using it).
Plausible implementation: a dropdown addon setting populated from that
endpoint, passed as `channel_profile_id` on `GetChannels()`'s own
`/api/channels/channels/` call. Not investigated further than
confirming the mechanism is real and currently unused.

**Update (2026-09-26, a 26th-pass audit): "it's opt-in" above is now
stale for a non-admin, profile-assigned account -- see the correction
note just above this section for the full account.**

## System notifications: a real, underused feature surface

Dispatcharr has a full in-app notification system (`core/models.py`'s
`SystemNotification`, `GET /api/core/notifications/` +
`.../count/`/`.../{id}/dismiss/`/`.../dismiss-all/`) that this addon
doesn't touch at all. Checked the model directly rather than guessing
from the name: it's genuinely operationally relevant, not just
developer/project chatter -- `notification_type` is one of
`version_update`/`setting_recommendation`/`announcement`/`warning`/
`info`, with a `priority` of `low`/`normal`/`high`/`critical` and a
`source` of `system` (server-generated) vs. `developer` (project-team-
authored, condition-evaluated). A `setting_recommendation` or a
`high`/`critical` `warning` could plausibly be something a user
actually wants to see (e.g. a real Dispatcharr-detected misconfiguration),
not just a version-bump nag. Polling this and surfacing high-priority
ones via Kodi's own `kodi::gui::dialogs::Notification` (or similar) --
independent of the PVR API proper, which has no generic "backend
alert" callback of its own -- is a plausible, if secondary, feature.
Not investigated further than confirming the model/endpoints are real
and what they contain; no dedup/dismissal-state design done yet.

## Feature notes

This file used to hold everything in one place; it grew past 1,600
lines across a long multi-session history and got split by topic so
a given task doesn't need to load all of it. Each file below carries
the same "confirmed live, not just assumed" standard as this one.

- [EPG.md](EPG.md) -- channel/EPG API field shapes, and mapping rich
  XMLTV data (posters, new/premiere/live badges, cast, genre) into
  Kodi's EPG
- [CATCHUP.md](CATCHUP.md) -- catch-up ("play from guide") playback
  and its seeking-reliability trade-offs
- [TIMESHIFT.md](TIMESHIFT.md) -- live TV pause/rewind, both local
  (inputstream.ffmpegdirect) and server-side (the companion
  Dispatcharr plugin), including the confirmed-broken seek
  investigation for the server-side snapshot path
- [RECORDINGS.md](RECORDINGS.md) -- recordings and timers, confirmed
  end-to-end against real data (the largest file here)
- [RECORDING_EDL.md](RECORDING_EDL.md) -- commercial-break markers
  (comskip EDL) on the recording seekbar, and the companion plugin that
  makes them reachable at all
- [RECURRING_RULES.md](RECURRING_RULES.md) -- recurring (day-of-week)
  timer rules, backed by Dispatcharr's own RecurringRecordingRule model,
  and the timezone-offset setting bridging Kodi's UTC timers against
  Dispatcharr's own configured system timezone
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) -- known Kodi-core quirks
  that aren't this addon's bug, multi-client limitations, and what's
  still unconfirmed
- [PVR_HTS_COMPARISON.md](PVR_HTS_COMPARISON.md) -- what the Tvheadend
  addon (pvr.hts) does that this one does not, which of it Dispatcharr
  can back, and what was decided about each (2026-10-10)

## OS sleep/wake and the real-time-updates WebSocket

Found via a comparative-architecture review against `pvr.hts`/Tvheadend,
which explicitly hooks Kodi's PVR `OnSystemSleep()`/`OnSystemWake()`
because HTSP is a persistent, stateful subscription that a suspend
silently kills, needing explicit disconnect/reconnect handling to avoid
post-wake stuttering. This addon's actual live/recording read paths are
stateless HTTP polls (a fresh request each time, byte position kept in
this addon's own memory) -- a suspend gap just looks like "no reads
happened for a while," and playback resumes wherever it left off with no
explicit handling needed. So most of what pvr.hts guards against doesn't
apply here architecturally.

The one place it does apply: `enable_realtime_updates`'s WebSocket
(`PVRDispatcharr::StartRealtimeUpdateThread()`) is this addon's one
actual persistent connection, and a suspend kills it the same way it'd
kill any other network interruption. The thread's own read timeout
already notices and reconnects on its own via exponential backoff (2s
doubling to a 60s cap, reset on a successful connection) -- so it was
already going to self-heal regardless -- but if backoff had climbed
toward the 60s cap before sleep, that's how long recording/timer sync
could sit dead after waking with no proactive nudge.

Implemented `PVRDispatcharr::OnSystemWake()` to cut short whatever
backoff wait the thread is currently in (`m_wakeRealtimeUpdateThread`,
checked alongside the existing stop flag in the same `wait_for()`
predicate) and reset backoff to its initial value, so a genuine wake
retries right away instead of continuing to wait out however much of the
pre-sleep backoff interval was left. Deliberately did *not* add an
`OnSystemSleep()` override -- there's nothing this addon's stateless
architecture needs to do proactively on sleep, and an empty override
that does nothing isn't worth having just for API completeness.

Confirmed live (Windows): the normal connect path is unaffected by the
`wait_for()` predicate change (same "realtime updates: connected" log
line, immediately on startup). The wake-nudge path itself wasn't
independently live-triggered -- doing so would mean actually suspending
the machine this addon was being tested on, which would have also cut
off the remote session driving the test. Confidence here comes from
tracing all three cases the modified predicate has to handle (stop,
wake, natural timeout) by hand, not a live repro.

**Update -- "the thread's own read timeout already notices and
reconnects" above was only accurate for a connection that actually
receives a close signal, a real gap found via a project-wide review
(2026-09-26), not yet re-confirmed live.** A plain read *timeout*
(`ReceiveTextMessage()` returning 0, "nothing new yet") is not itself
evidence of a dead connection -- it's the normal, expected outcome
whenever nothing happens to arrive within the 5-second read window, and
the loop just tries again indefinitely either way. Only a genuine
socket-level error (`result < 0`) triggers a reconnect, and a *half-open*
connection (the peer vanishes without ever sending a FIN/RST -- an
application crash a reverse proxy or the OS's own TCP stack doesn't
surface, a NAT mapping silently expiring, a network path dropping) never
produces one on its own: `recv()` on a socket like that just keeps
returning "no data yet", indistinguishable from a healthy idle
connection, since this client never sends anything of its own accord
either (only answers the server's own pings) to ever provoke a failure.
This directly matches `docs/RECORDINGS.md`'s own previously-unexplained
observation (a real-time-updates WebSocket "appearing not to reconnect
after a Dispatcharr outage-and-recovery," only one "connected" log line
the whole session) -- exactly the symptom a half-open connection would
produce. Fixed by enabling TCP keepalive on the connect-only curl handle
(`WebSocketClient::Connect()`) -- lets the OS itself eventually surface
a dead peer as a real socket error, which this thread's existing
`result < 0` reconnect path already handles correctly. Not yet
re-confirmed against a fresh live outage-and-recovery test.

**Update -- `OnSystemWake()`'s own nudge only ever helped the
*disconnected* case, a related gap found in the same project-wide
review (2026-09-26), not yet re-confirmed live.** The paragraph above
describes cutting short "whatever backoff wait the thread is currently
in" -- but that's only one of the two states a suspend can catch this
thread in. If the thread is currently *connected* (sitting in the inner
read loop, sending nothing itself and just waiting on
`ReceiveTextMessage()`) at the moment of suspend, `m_wakeRealtimeUpdateThread`
being set on wake was never checked anywhere in that loop -- only in
the *outer* reconnect wait. A session that looked "connected" going
into suspend was left completely unchecked, falling back to whatever
TCP keepalive eventually notices (up to ~195s on Linux, per the update
above) instead of reacting to the wake immediately -- defeating the
entire point of an explicit wake nudge for exactly the scenario a
suspend/resume is most likely to produce. Fixed by also checking the
flag inside the inner read loop: a plain read timeout with a pending
wake nudge (and no message having arrived since to prove the session
survived) now forces an immediate reconnect, the same as the outer wait
already did for a disconnected thread.

Found via a project-wide code review (not a live incident): `SendAll()`'s
retry loop on `CURLE_AGAIN` called `select()` with a fixed 5s timeout,
then unconditionally `continue`d back to `curl_easy_send()` regardless of
whether `select()` actually returned because the socket became writable,
timed out, or errored -- its return value wasn't even checked. Unlike its
sibling `FillBuffer()` a few lines below (which does track a real
deadline via `std::chrono::steady_clock` and returns 0 on timeout), this
send-side loop had no overall bound at all: a stalled/zombie connection
(one that completes the TCP connect but never drains what's sent to it)
would retry forever, sleeping up to 5s between attempts. Since
`Connect()`'s own handshake request and every ping/pong/close frame
`ReceiveTextMessage()` sends all go through this same function, an
unbounded hang here would silently freeze
`StartRealtimeUpdateThread()`'s background thread indefinitely, with
nothing logged to explain why -- the exact same *class* of bug as the
1.0.5 heartbeat-blocking regression (see `docs/TIMESHIFT.md`), just in
different code, found by deliberately going looking for the same failure
shape elsewhere in the codebase after that one.

Fixed by giving `SendAll()` its own `timeoutSeconds` parameter and the
same deadline-tracking pattern `FillBuffer()` already uses -- `Connect()`
passes its own `connectTimeoutSeconds`; `SendPong()`/`SendClose()` (both
plumbed a `timeoutSeconds` parameter through from `ReceiveTextMessage()`)
use its read timeout. `select()`'s own return value is still not treated
as authoritative (a spurious wakeup just loops back to
`curl_easy_send()`), only the tracked deadline actually bounds the wait,
matching `FillBuffer()`'s own reasoning.

Confirmed live (Windows): compiles cleanly and the addon reloads
normally. Not independently live-triggered against a genuinely stalled
connection (would need a way to simulate a TCP peer that accepts a
connect but never drains -- not attempted this pass); confidence comes
from the fix directly mirroring `FillBuffer()`'s already-proven pattern
in the same file.

**Update -- the realtime-update connection could never complete its
handshake at all behind a reverse proxy that negotiates HTTP/2, found
via a project-wide review (a 30th-pass audit, 2026-09-26), confirmed
against the real curl 8.6.0 source this addon's own CoreELEC and
Android depends builds actually link against (both build with nghttp2),
not itself reproduced live.** `Connect()` never set `CURLOPT_HTTP_VERSION`.
A curl built with nghttp2 defaults its own `httpwant` to
`CURL_HTTP_VERSION_2TLS` (curl's `url.c`), and `CURLOPT_CONNECT_ONLY`
doesn't change what ALPN offers -- `alpn_get_spec()` (curl's `vtls.c`)
only looks at `httpwant`, so this handle's TLS `ClientHello` offered
"h2, http/1.1" regardless. If the peer (a reverse proxy in front of
Dispatcharr -- Caddy/Traefik negotiate h2 by default, nginx does with
`http2 on`) picked h2, curl installed its own HTTP/2 filter on the
connection unconditionally (`cf-https-connect.c`'s `baller_connected()`
doesn't check connect-only mode either). Every later `curl_easy_send()`
of this handshake's raw HTTP/1.1 upgrade request then went through that
filter's own `cf_h2_send()` -> `h2_submit()` (curl's `http2.c`), which
parses the raw bytes as an HTTP/1 request and re-encodes them as an
HTTP/2 HEADERS frame -- silently dropping the `Connection`/`Upgrade`
headers a WebSocket handshake depends on, neither valid in HTTP/2. The
server then never answers `HTTP/1.1 101`, `IsWebSocketHandshakeAccepted()`
always failed, and realtime updates could never connect at all behind
such a proxy -- silently, at debug log level only, with no user-visible
error and a permanent fallback to the periodic refresh. Fixed by
pinning `CURLOPT_HTTP_VERSION` to `CURL_HTTP_VERSION_1_1` on this
handle -- safe regardless of what the peer would otherwise have picked,
since nothing on this connection needs (or, per RFC 6455, is defined
for) anything beyond HTTP/1.1 anyway. Only relevant with `use_https` on
and only behind a proxy that actually negotiates h2; this project's own
lab has no reverse proxy in front of Dispatcharr (see this file's own
"How to verify quickly" section), so the fix is unconfirmed against a
real h2-terminating proxy.

## Single-instance assumption: partially hardened, not fully

Also found via the `pvr.hts`/Tvheadend comparison above: `pvr.hts`
added support for Kodi requesting a *second* concurrent recording stream
open on the same addon (its v22.5.0 changelog entry says specifically
for Kodi's own recording-thumbnail generation). This addon has never
supported that, and still doesn't fully today -- for two separate
reasons:

1. `CAddonDispatcharr` (`addon.cpp`) used to track the single PVR
   instance it expected via one raw pointer, unconditionally cleared on
   any `DestroyInstance()` call regardless of which instance was being
   destroyed. If Kodi ever *did* request a second instance, creating it
   would silently overwrite the first instance's tracking, and
   destroying either one would wipe settings-routing (`SetSetting()`) for
   whichever instance was still alive. **Fixed**: now tracks a real
   collection of instances (erasing only the one matching the destroyed
   handle) and broadcasts `SetSetting()` to all of them -- correct
   either way, since Kodi's `SetSetting()` call itself carries no
   per-instance identity to begin with (settings.xml is one shared
   config for the whole addon). A no-op change in the single-instance
   case that's actually been observed in practice; a real fix if a
   second instance is ever requested.
2. **Not fixed, deliberately**: even with instance-tracking hardened,
   Kodi's `OpenRecordedStream(const kodi::addon::PVRRecording&)` API
   itself (confirmed in Kodi's own `kodi-dev-kit/include/kodi/addon-
   instance/PVR.h`) takes no stream-handle/id -- it's a single implicit
   slot per instance. `RecordingStreamState`/`InProgressRecordingStreamState`
   in `DispatcharrClient` are built around that same single-slot
   assumption throughout (`OpenRecordingStream()` closes whatever's
   already open before opening the new one). Supporting a second
   *concurrent* stream on one instance would mean turning that single
   state struct into a small collection keyed by some caller-supplied
   identity Kodi doesn't actually provide at this API level -- a real
   redesign, not a small fix, and not attempted here without a confirmed
   live trigger. Whether Kodi's PVR manager ever actually requests this
   for this addon's recordings specifically (network-streamed HLS, not
   pvr.hts's local-file-like VFS) is unconfirmed -- no evidence either
   way from this addon's own testing history. Worth revisiting if a real
   symptom (e.g. thumbnail generation silently failing or interrupting
   active playback) ever surfaces.

## How to verify quickly

From a machine that can reach your Dispatcharr instance:

```bash
# Get a token (only works for accounts with full login permissions)
curl -X POST http://<host>:9191/api/accounts/token/ \
  -H 'Content-Type: application/json' \
  -d '{"username":"<user>","password":"<pass>"}'

# Or use an API key instead (works for restricted/streamer-role accounts too)
curl http://<host>:9191/api/channels/channels/ \
  -H "X-API-Key: <your-api-key>" | jq '.[0] // .results[0]'

# Inspect the recordings schema via the live OpenAPI doc
curl http://<host>:9191/api/schema/ | grep -A30 '/api/channels/recordings/:'
```

Once you've confirmed a field/path, update the corresponding line in
`DispatcharrClient.cpp` (they're grouped near the top and clearly commented)
and update TROUBLESHOOTING.md's "Still unconfirmed" list.

## Credentials and redirects, response limits, and the WebSocket handshake (2026-09-30)

Four small hardening changes in `DispatcharrClient`/`WebSocketClient`, each confirmed live against a
real Kodi and the real instance with a proxy in front that injected the failure.

**Credentialed requests only follow redirects to the same host.** `Request()` (a bearer token, and a login's
username and password in the POST body), `SendTimeshiftHeartbeat()` and `OpenRecordingStream()` (an `X-API-Key`
header) used libcurl's `CURLOPT_FOLLOWLOCATION`, which withholds only an `Authorization` header on a cross-host
redirect (and only since curl 7.58): an `X-API-Key` header is sent wherever the redirect points, a 307/308
re-sends the whole POST body, and an `http://` target sends it in the clear. They now follow redirects
themselves (`PerformWithSafeRedirects()`), vetting each hop with `IsSafeRedirectTarget()` (`RedirectPolicy.h`):
same host, never https to http, no `user:pass@` in the target, at most five hops. A different port or an
http-to-https upgrade on the same host is still followed (a reverse proxy does both), and a refused redirect
is returned as the 3xx itself, with only the hosts logged since a query string can carry a token.
**Reproduced first** with a proxy answering the login POST with a 307 to a different loopback address:
the previous build re-sent the password to it (a listener on the other address logged a request with the
password in its body, twice); the new build sent nothing there and logged "refused to follow a redirect (HTTP 307) from
... to ...". `GetXmlTvGuide()` carries no credentials and still follows redirects normally.

**A response can no longer grow without limit.** `WriteCallback` appended every byte it was given, so a
runaway response ended in `std::bad_alloc` thrown inside a C callback, very likely `std::terminate()`.
`BoundedWriteCallback` stops at a per-call-site ceiling, chosen from sizes measured on the live instance
rather than one number for all: API JSON 128 MiB (the largest real one, the channel list, is far below it), the XMLTV guide 512 MiB (measured well below it), an in-progress recording's playlist 64 MiB (about 100 bytes
per segment). Past it the transfer fails with an error that names the limit. Confirmed live: a proxy
streaming an endless body in answer to the channel list was cut off after 136 MB (the 128 MiB limit plus what was
already in flight) with "failed to load channels: HTTP response exceeded the 128 MiB limit", and Kodi carried on.

**A 401 now goes through `EnsureAuthenticated()`.** `Request()` handled a 401 by calling `RefreshAccessToken()`
and `Login()` directly, bypassing `AuthBackoff` and the transient cooldown. A token Dispatcharr revoked while the
local four-minute freshness hint still looked valid therefore sent every request in that window through its
own refresh and login. It now invalidates the hint, but only if the token is still the one that was
rejected (threads that got a 401 together share one refresh), and lets `EnsureAuthenticated()` apply its
gates. Confirmed live with a proxy that answers every API call 401 once the addon is logged in:
**11 login POSTs and 11 refresh POSTs in 170 seconds for 10 authenticated requests before; 3 logins, 1 refresh
and 1 request after.**

**The WebSocket handshake checks `Sec-WebSocket-Accept`.** RFC 6455 requires the client to verify it
(base64 of the SHA-1 of its key plus a fixed GUID); only the 101 and the `Upgrade` header were checked, so a proxy
that never read the connection's key could pass. `Sha1.h` is a small dependency-free SHA-1 (checked against the
FIPS vectors, a million `a`s and the padding boundaries against a reference), and
`IsWebSocketHandshakeAcceptedForKey()` compares exactly, because base64 is case-sensitive. Confirmed live: a
proxy rewriting the accept value made the previous build report "connected"; the new build refuses with a
message that says the answer was wrong rather than blaming the account, and still connects to the real server.

## WebSocket reads are bounded by deadlines, and the socket wait uses poll() (2026-10-02)

`WebSocketClient` (the realtime-update connection) now works from absolute deadlines. `ReceiveTextMessage()` has one for the whole call -- a peer feeding a steady stream of ping/pong/binary frames used to keep it from ever returning, and the realtime thread has to come back up to notice a stop request -- and one per frame, started when its header arrives: a frame dripped in a byte at a time fails "mid-frame" within the timeout instead of after a payload's worth of timeouts. Frames are checked against two RFC 6455 rules the client used to assume (a server never masks; a control frame is at most 125 bytes and unfragmented). The wait for the socket is `WaitForSocketReady()` (`SocketWait.h`): `poll()` on POSIX, because `select()`'s `fd_set` cannot hold a descriptor numbered 1024 or more (`FD_SET` on one is undefined behaviour) and `FD_SET(-1)` is no better when curl reports no active socket; an invalid socket is refused up front. All of it is tested against a real local WebSocket server (`tests/test_web_socket_client.cpp`) and was confirmed live (a recording created straight against Dispatcharr reached Kodi's timers in under 3 s).
