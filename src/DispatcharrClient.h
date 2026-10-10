#pragma once

// DispatcharrClient talks to a Dispatcharr server's native REST API
// (NOT the Xtream Codes compatibility layer) so that DVR actions taken in
// Kodi map directly onto Dispatcharr's own recording engine.
//
// Confirmed against a live instance's own OpenAPI schema (GET /api/schema/)
// at the time this was written:
//   POST   {base}/api/accounts/token/                -> {access, refresh} JWT
//   POST   {base}/api/accounts/token/refresh/         -> {access}
//   GET    {base}/api/channels/channels/              -> paginated channel list
//   GET    {base}/api/channels/streams/               -> paginated stream list
//   GET    {base}/api/channels/logos/{id}/cache/      -> channel logo image
//   GET    {base}/output/epg                          -> full XMLTV guide document
//   GET    {base}/proxy/ts/stream/{channel_uuid}      -> live MPEG-TS stream
//   GET    {base}/api/channels/recordings/            -> bare array of Recording
//                                                         {id, start_time, end_time,
//                                                         task_id, custom_properties,
//                                                         channel} -- NOT title/
//                                                         subtitle/description/
//                                                         duration/in_progress, see
//                                                         GetRecordings() below
//   POST   {base}/api/channels/recordings/            -> create; body is
//                                                         {channel, start_time, end_time,
//                                                         custom_properties?} -- no
//                                                         title/name field exists
//   DELETE {base}/api/channels/recordings/{id}/       -> delete one recording
//   GET    {base}/api/channels/recordings/{id}/file/  -> recording playback,
//                                                         Range-seekable, redirects to
//                                                         .../hls/index.m3u8 while
//                                                         still recording -- despite
//                                                         its documented security
//                                                         schemes including anonymous
//                                                         access, a real instance
//                                                         returned 403 for both this
//                                                         and the redirect target
//                                                         without an X-API-Key header
//                                                         or Bearer token, confirmed
//                                                         for both an in-progress and
//                                                         a completed recording -- see
//                                                         OpenRecordingStream()
//   GET    {base}/api/channels/series-rules/          -> {"rules": [...]}, NOT a bare
//                                                         array or {results: [...]}
//   POST   {base}/api/channels/series-rules/          -> body is {title, tvg_id?,
//                                                         channel_id?, mode?,
//                                                         title_mode?, ...} -- NOT
//                                                         {channel, title_pattern}
//   DELETE {base}/api/channels/series-rules/?title=&tvg_id=&epg_source_id=
//                                                      -> deletes by query params,
//                                                         NOT /{id}/ -- series rules
//                                                         have no path-addressable id
//   POST   {base}/api/channels/series-rules/evaluate/ -> evaluate series rules
//   GET    {base}/api/accounts/api-keys/              -> {key: <str|null>}. The calling
//                                                         account's CURRENT key, read
//                                                         without replacing it -- what
//                                                         ObtainApiKey() tries first
//   POST   {base}/api/accounts/api-keys/generate/     -> {key, user}. Regenerating
//                                                         replaces the previous key
//                                                         (confirmed: calling this
//                                                         twice returns two different
//                                                         keys) -- only when the
//                                                         account has no key at all,
//                                                         see ObtainApiKey()
//
// A Recording's custom_properties key names (title/subtitle/description
// nested under "program", plus status/file paths/poster logo) and a
// series-rules list item's shape are both confirmed against real created
// objects too -- see docs/API_NOTES.md for the details.

#include "ApiKeyRecovery.h"
#include "CatchUpUtil.h"
#include "DvrAccess.h"
#include "RecurringRuleEdit.h"
#include "RequestTimeout.h"
#include "SegmentFetchFailure.h"
#include "UnprobeableSegment.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace dispatcharr
{

struct Config
{
  std::string host;
  int port = 9191;
  bool useHttps = false;
  std::string username;
  std::string password;
  bool verifySsl = true;
  int timeoutSeconds = 30;
  // Long-lived Dispatcharr API key, used only to authenticate recording
  // playback (see OpenRecordingStream()) -- unlike the JWT access token
  // (30-minute lifetime), this doesn't expire on its own. It CAN still go
  // stale, though: Dispatcharr keeps only one active key account-wide, so
  // any other client regenerating that account's key (another Kodi
  // install, a script) silently invalidates this one --
  // OpenRecordingStream()/ReadRecordingStream() detect that (a 401) and
  // self-heal by re-reading the account's current key and retrying (see
  // ObtainApiKey()). If left empty, the account's existing key is read
  // and persisted back to the addon's own settings on first use -- a new
  // one is only generated when the account has none; see PVRDispatcharr's
  // constructor.
  std::string apiKey;
};

struct Channel
{
  int id = 0;
  std::string uuid; // used to build the live-stream proxy URL
  std::string name;
  int logoId = -1; // -1 means no logo; pass to GetChannelLogoUrl()
  // Dispatcharr's channel_number is a float server-side, so a subchannel
  // like 5.1 is real -- see ChannelNumber.h for how it splits into Kodi's
  // channel/sub-channel numbers and the exact text (also what the XMLTV
  // guide's <channel id="..."> uses, not tvgId) the guide lookup keys by.
  // 0 for a null channel_number too -- `hasChannelNumber` below tells the two
  // apart.
  double channelNumber = 0.0;
  // False for a null channel_number (which `channelNumber` above reads as 0):
  // the guide export keys such a channel by its id, not a number -- see
  // FormatChannelGuideKey() (ChannelNumber.h). A literal 0 is a number.
  bool hasChannelNumber = true;
  int groupId = -1;
  std::string groupName;
  std::string tvgId;
  // The channel's *effective* epg_data id (its own, or a channel-level
  // override's -- see effective_epg_data_id in the live schema). NOT
  // guaranteed to be the EPGData row tvgId actually came from: a
  // channel-level EPG-data override can repoint this at a different EPG
  // source's row without updating the channel's own tvg_id field,
  // confirmed live (an auto-channel-merge tool did exactly this) -- see
  // ResolveSeriesRuleTvgId()'s own comment. 0 if absent.
  int epgDataId = 0;
  // Catch-up/archive playback, backed by the upstream provider's own
  // archive (Xtream "tv_archive"), not a generic Dispatcharr-side rolling
  // timeshift buffer for every channel -- see docs/API_NOTES.md.
  bool catchupEnabled = false;
  int catchupDays = 0;
};

struct ChannelGroup
{
  int id = 0;
  std::string name;
};

struct Recording
{
  int id = 0;
  // Dispatcharr's Recording object itself carries no title/subtitle/
  // description fields at all (confirmed against its live OpenAPI schema:
  // just id, start_time, end_time, task_id, custom_properties, channel) --
  // these are read out of custom_properties on a best-effort basis, see
  // DispatcharrClient.cpp.
  std::string title;
  std::string subtitle;
  std::string description;
  time_t startTime = 0;
  time_t endTime = 0;
  int durationSeconds = 0;
  int channelId = 0;
  bool isInProgress = false; // startTime <= now < endTime
  bool isUpcoming = false;   // now < startTime (scheduled, not yet started)
  // True while custom_properties._hls_dir is still present -- Dispatcharr
  // sets status away from "recording" (to "stopped") the instant the user
  // stops it, synchronously in the stop endpoint, well before the HLS-to-MKV
  // concat that happens afterward in the background recording task actually
  // finishes (confirmed in tasks.py: the stop endpoint's own comment says so
  // outright, and _hls_dir is only ever popped from custom_properties after
  // the directory is actually removed, post-concat, post-viewer-wait). So
  // isInProgress alone going false does NOT mean a stable, complete file
  // exists yet to open for byte-range playback -- confirmed live: opening a
  // just-stopped recording as a completed one errored outright (the file
  // didn't exist yet) or played without seeking (the file existed but was
  // still being actively written by the concat, an unstable Content-Length
  // this addon's completed-recording path was never designed to handle).
  // OpenRecordedStream() uses this, independent of isInProgress, to decide
  // whether to keep using the growing-buffer HLS reader -- which handles a
  // frozen (no-longer-growing) manifest correctly already, since
  // RefreshInProgressRecordingManifest() ties "finished" to isInProgress,
  // not to whether the underlying ffmpeg process happens to still be alive.
  // Deliberately NOT folded into isInProgress itself: that field also drives
  // Kodi's timer-state UI (PVR_TIMER_STATE_RECORDING), which must keep
  // reflecting Dispatcharr's real status, not this file-readiness detail.
  bool hlsDirStillPresent = false;
  // 0 while genuinely still recording -- Dispatcharr only writes this into
  // custom_properties at finalization, see the comment where this is parsed
  // in DispatcharrClient.cpp.
  int64_t bytesWritten = 0;
  // Non-zero when this Recording was materialized by Dispatcharr's own
  // recurring-rule scheduler (custom_properties.rule.{type:"recurring",id})
  // rather than created directly -- see RecurringRule below. Used to link
  // this occurrence back to its parent rule as a Kodi PVR_TIMER child
  // (PVRTimer::SetParentClientIndex()).
  int recurringRuleId = 0;
  // The EPG programme's own, never-padded/never-clamped start/end time,
  // from custom_properties.program.start_time/end_time -- 0 when absent
  // (added 2026-09-26, a 34th-pass audit, fixing a real, confirmed gap
  // found via a project-wide review, confirmed against Dispatcharr's own
  // real current upstream source, not itself independently reproduced).
  // Only present for a recording created with an explicit
  // custom_properties.program in its own create/update request --
  // Dispatcharr's own web UI Guide "Record" button always sends one
  // (frontend/src/pages/Guide.jsx), and series-rule-materialized
  // occurrences carry one from their own snapshot (apps/channels/tasks.py)
  // -- never this addon's own CreateOneTimeRecording(), which
  // deliberately omits custom_properties on create (see that function's
  // own comment) and whose later server-side enrichment
  // (_match_epg_program_by_timeslot(), apps/channels/tasks.py) only ever
  // fills in id/title/sub_title/description, never times. This is the
  // one place this addon can recover a recording's real programme window
  // independent of RecordingSerializer.validate()'s own pre/post-padding
  // and past-start-time clamping (both applied to startTime/endTime
  // above, never to this field) -- see
  // dispatcharr::FindEpgEntryIndexByStartTime()'s own comment
  // (EpgProgramMatch.h) for why the padded/clamped window alone isn't
  // always enough to find the right EPG entry.
  time_t programStartTime = 0;
  time_t programEndTime = 0;
  // custom_properties.program.tvg_id: the guide id of the programme this
  // recording was materialized from, empty when the snapshot carries none. A
  // series rule with no pinned channel is identified by its own tvg_id instead of
  // a channel, so this is what links such a recording back to its rule (see
  // MatchRecordingsToSeriesRules()).
  std::string programTvgId;
};

// A scheduled recording: either a one-off (isSeries == false) or a
// standing series rule (isSeries == true) evaluated by Dispatcharr itself.
struct TimerRule
{
  int id = 0;
  int channelId = 0;
  std::string tvgId;
  std::string title;
  std::string titlePattern; // series rules only
  time_t startTime = 0;
  time_t endTime = 0;
  bool isSeries = false;
  bool recordNewOnly = false; // Dispatcharr's mode == "new" vs "all"
  // Series rules only, added 2026-09-29 fixing a real, confirmed bug
  // (see docs/OPEN_ITEMS.md): CreateSeriesRule()'s own POST/upsert never
  // sent these, and Dispatcharr's own SeriesRulesAPIView.post() does a
  // full existing.clear(); existing.update(rule_record) on match -- so
  // any edit from Kodi (even a bare enable/disable-equivalent toggle)
  // silently reset a rule's own title_mode/description/description_mode
  // customized via Dispatcharr's own web UI back to these plain
  // defaults. Cached here (populated by ParseTimerRuleJson()) so
  // UpdateTimer()'s own series-rule branch can echo them straight back
  // on every edit instead of letting them default. epgSourceId <= 0
  // means "not pinned to a specific EPG source" (Dispatcharr's own
  // "omit to use every mapped copy of that tvg_id" default), matching
  // this project's established channelId <= 0 "no channel" convention.
  std::string titleMode = "exact";
  std::string description;
  std::string descriptionMode = "contains";
  bool untaggedIsNew = false;
  int epgSourceId = 0;
};

// A recurring day-of-week rule, backed by Dispatcharr's own
// RecurringRecordingRule model/scheduler (confirmed against its live
// source and API: GET/POST /api/channels/recurring-rules/, an hourly
// Celery task materializing real Recording rows up to 14 days ahead,
// each tagged custom_properties.rule.{type:"recurring",id} -- see
// GetRecurringRules()/CreateRecurringRule() below). Distinct from
// TimerRule (series rules): this one has a real numeric id, and its
// schedule is a fixed weekly time-of-day pattern rather than
// EPG-title matching.
struct RecurringRule
{
  int id = 0;
  int channelId = 0;
  std::string name;
  // 0=Monday .. 6=Sunday (confirmed against Dispatcharr's own source:
  // RecurringRecordingRuleSerializer's validation error text and
  // sync_recurring_rule_impl's date.weekday() comparison both agree on
  // this convention) -- conveniently the same convention Kodi's own
  // PVR_WEEKDAY_MONDAY=(1<<0)..PVR_WEEKDAY_SUNDAY=(1<<6) bitmask uses, so
  // converting between the two is a plain 1 << day, no reordering.
  std::vector<int> daysOfWeek;
  // Seconds since midnight, in Dispatcharr's own configured system
  // timezone (NOT UTC -- confirmed via source that Dispatcharr's
  // recurring-rule scheduler combines these naive values with its system
  // timezone CoreSetting, with no server-side conversion; see
  // recurring_rule_utc_offset_minutes in settings.xml for how this addon
  // bridges that against Kodi's UTC-based timer times).
  int startTimeOfDaySeconds = 0;
  int endTimeOfDaySeconds = 0;
  time_t startDate = 0; // UTC midnight of Dispatcharr's local start_date
  time_t endDate = 0;   // likewise, end_date
  bool enabled = true;
};

// One comskip-detected commercial break (or other marker), as returned by
// the recording_edl companion plugin's get_edl action -- see
// GetRecordingEdl()'s own comment.
struct RecordingEdlEntry
{
  int64_t startMs = 0;
  int64_t endMs = 0;
  int type = 3; // matches Kodi's own PVR_EDL_TYPE_COMBREAK; see plugin.py
};

// Thin, synchronous REST client. Callers (PVRDispatcharr) are responsible
// for running these off Kodi's calling thread where the PVR API allows it;
// none of the calls here touch Kodi's own API.
class DispatcharrClient
{
public:
  explicit DispatcharrClient(Config config);
  ~DispatcharrClient();

  // Logs in with username/password and stores the JWT pair. Safe to call
  // repeatedly; it is a no-op if a still-valid token is already held.
  // timeoutMsOverride, when positive, bounds the login/refresh request this call may have to make (a caller that
  // promises a short worst case, like Stop and the live-edge manifest refresh, must not wait out the configured
  // timeout in the authentication step before its own bounded request even starts -- found by the fifteenth
  // hardening sweep: Close took 30 s, not 5, once the token hint had lapsed during a blackhole).
  bool EnsureAuthenticated(std::string& error, long timeoutMsOverride = 0);
  // Forgets the cached access token's freshness hint so the next authenticated
  // call refreshes it first. For a wake from suspend: the hint is a
  // steady_clock deadline a few minutes out, and steady_clock does not advance
  // while the system sleeps, so a token minted just before a long sleep reads as
  // fresh long after the server's own lifetime has passed it.
  void InvalidateAccessToken();
  // Makes EnsureAuthenticated() stop trusting the cached token, but only if it
  // is still `rejectedToken` -- a request that was answered 401 uses this so
  // that a token another thread has already replaced isn't invalidated again.
  void InvalidateAccessTokenIfCurrent(const std::string& rejectedToken);

  bool GetChannels(std::vector<Channel>& out, std::string& error);
  bool GetChannelGroups(std::vector<ChannelGroup>& out, std::string& error);

  // Full-body caller must fetch and parse this with XmlTvParser; this
  // client only returns the raw document.
  // `httpStatusOut` (optional) receives the HTTP status, or 0 for a
  // transport failure -- EnsureEpgLoaded() needs it to tell a durable
  // rejection (Network Access 403) from an outage, see
  // dispatcharr::ShouldCountTowardEpgFailureBackoff().
  // `prevDays` > 0 asks for that many days of already-aired programmes too
  // (`?prev_days=N`), see dispatcharr::ComputeGuidePrevDays().
  // Makes every request in flight (and any started later) fail immediately. Called when
  // the addon instance is being destroyed, so shutdown does not wait out a request to an
  // unresponsive server -- see CurlCallbacks.h's TransferAbortResult().
  void AbortInFlightRequests()
  {
    m_abortRequests = true;
  }
  // `responseUnusableOut` is set when the server answered but the body went past the size ceiling or
  // exhausted memory (no HTTP status is kept for those) -- see ShouldCountTowardEpgFailureBackoff().
  bool GetXmlTvGuide(std::string& xmlOut, std::string& error, long* httpStatusOut = nullptr, int prevDays = 0,
                     bool* responseUnusableOut = nullptr);

  // Plain live-stream URL passthrough for live_timeshift_mode's "Off"
  // setting -- no server-side buffering, no admin-account requirement.
  std::string GetLiveStreamUrl(const Channel& channel) const;
  // logoId is a Logo object's own id (Channel::logoId), not the channel's id.
  std::string GetChannelLogoUrl(int logoId) const;

  // Creates a catch-up (archived-programme) playback session via
  // POST /api/catchup/sessions/ and returns a fully-qualified, session-bound
  // URL that plays and seeks without needing any further auth for the life
  // of the session (a 10-minute *sliding* idle window, refreshed by each
  // range/seek request -- i.e. it doesn't expire mid-playback the way a
  // short-lived JWT embedded directly in the URL would). Only meaningful
  // for a channel with Channel::catchupEnabled set; programmeStart must be
  // the EPG entry's own start time, not when the viewer pressed play.
  bool CreateCatchupSession(const std::string& channelUuid, time_t programmeStart, int durationMinutes,
                            std::string& playbackUrlOut, std::string& error);

  // Starts (or, if already running, confirms) a server-side rolling live
  // buffer for a channel via this addon's companion Dispatcharr plugin
  // (dispatcharr-plugin/timeshift_buffer/ in this repo -- not built into
  // Dispatcharr itself, must be installed and enabled separately). Returns
  // a fully-qualified URL to the buffer's rolling HLS playlist, built from
  // this client's own configured host plus the port/path the plugin
  // reports back for its own file server -- deliberately always http://
  // regardless of the use_https setting, since the plugin's minimal file
  // server has no TLS of its own and isn't assumed to sit behind whatever
  // reverse proxy/TLS termination the main API port might (see
  // docs/API_NOTES.md for the limitation this implies if your setup splits
  // those differently). Requires the Dispatcharr account this addon is
  // configured with to be an admin account -- confirmed against
  // Dispatcharr's own source (apps/accounts/permissions.py) that the
  // plugin run endpoint requires IsAdmin (user_level >= 10) for POST, not
  // just any authenticated user.
  // retryableOut (when non-null) is set from the plugin's own "retryable"
  // response field -- true means this specific failure (currently just
  // its own buffer being mid-teardown, see plugin.py's
  // _classify_existing_buffer()) is expected to clear up on its own
  // shortly, unlike a genuinely permanent one (e.g. max_concurrent_buffers,
  // ffmpeg not found). OpenLiveTimeshiftStream() uses this to retry
  // briefly instead of failing outright -- a real gap found via a
  // project-wide review: this window is normally brief (bounded by
  // _stop_ffmpeg's own ~2s SIGTERM deadline plus file removal), but a
  // caller opening the same channel inside it used to get a hard,
  // non-retried failure.
  bool StartTimeshiftBuffer(const std::string& channelUuid, std::string& playlistUrlOut, std::string& error,
                            bool* retryableOut = nullptr);
  // Tells the plugin this specific viewer (m_liveTimeshiftStream.viewerId)
  // is done with the channel's buffer -- NOT an unconditional stop. The
  // plugin reference-counts viewers per buffer (registered by
  // StartTimeshiftBuffer()'s own viewer_id param) and only actually stops
  // the underlying ffmpeg process once the *last* registered viewer leaves;
  // if others are still registered, this just deregisters the caller and
  // the buffer keeps running for them. Only called from
  // CloseLiveTimeshiftStream(), synchronously -- see its own comment for
  // why a detached background thread here (an earlier version of this
  // fix) was itself a real bug: it let a fast channel switch (Kodi's own
  // back-to-back Close-then-Open) race the actual teardown, so a
  // provider's own concurrent-stream limit could still be fully consumed
  // by the channel just switched away from at the exact moment the new
  // channel's own Open() asked for a slot. See docs/TIMESHIFT.md's
  // "Concurrent viewers" section for the full account, including the
  // original bug this reference-counted design itself fixes (a buffer
  // staying exhausted well after its only real viewer stopped, since
  // nothing used to proactively tell the plugin so). Best-effort:
  // "nothing was running" is success, not an error.
  bool StopTimeshiftBuffer(const std::string& channelUuid, const std::string& viewerId, std::string& error);
  // Refreshes this specific viewer's own last-seen time on the plugin side
  // (plugin.py's heartbeat action, viewer_id param), separate from the
  // buffer-wide liveness that ordinary segment fetches already provide.
  // Needed so a viewer that later crashes without calling
  // StopTimeshiftBuffer() can be told apart, by the plugin, from one still
  // genuinely watching -- otherwise a stale viewer_id left behind by a
  // crash blocks the reference count from ever reaching zero for the
  // viewers who really do stop cleanly afterwards (see plugin.py's
  // _prune_stale_viewers and docs/TIMESHIFT.md). Called periodically from
  // ReadLiveTimeshiftStream() while a stream is open, not on every read.
  // Best-effort: a failure here is logged, not surfaced to the caller --
  // losing one heartbeat isn't worth interrupting playback over, since the
  // buffer-wide heartbeat from this same read's own manifest/segment
  // fetches already keeps the buffer itself alive regardless. Bounded by
  // design to a small, fixed worst-case duration and never touches
  // EnsureAuthenticated()/Login()/RefreshAccessToken() -- see this
  // method's own .cpp comment for the live-reproduced stall this fixes:
  // riding along on ReadLiveTimeshiftStream()'s own thread means an
  // unbounded call here is an unbounded stall of Kodi's own demuxer read.
  void SendTimeshiftHeartbeat(const std::string& channelUuid, const std::string& viewerId);

  bool GetRecordings(std::vector<Recording>& out, std::string& error);
  // Confirmed live: GET /api/channels/recordings/{id}/ is a real,
  // standard DRF retrieve route -- same response shape as one item from
  // GetRecordings()'s own list, just scoped to a single recording.
  // RefreshInProgressRecordingManifest() uses this instead of a full
  // GetRecordings() call every ~500ms during in-progress playback, since
  // it only ever needs one recording's current state (was previously
  // O(every recording) just to check one id's isInProgress flag).
  // `httpStatusOut`, if given, is set to the real HTTP status the server
  // answered with, or left 0 when no response arrived at all (or
  // authentication itself failed first) -- lets a caller tell a lookup that
  // answered 404 (the recording is gone) apart from one that merely failed.
  // Bound for the requests a viewer waits on when the server is gone (Stop, steady-state refreshes, the startup
  // version check and the authentication before them); follows the configured connection timeout. RequestTimeout.h.
  long ShortRequestTimeoutMs() const;
  // `authenticationNotCompletedOut`, if given, is set when authentication itself did not succeed (refused during a
  // backoff, or failed): no request about the recording was made, which is not the same as the server not answering it.
  bool GetRecordingById(int id, Recording& out, std::string& error, long* httpStatusOut = nullptr,
                        long timeoutMsOverride = 0, bool* authenticationNotCompletedOut = nullptr);
  // Fetches comskip-detected commercial-break markers for a completed
  // recording via this addon's companion Dispatcharr plugin
  // (dispatcharr-plugin/recording_edl/ in this repo -- not built into
  // Dispatcharr itself, must be installed and enabled separately, same
  // admin-account requirement as StartTimeshiftBuffer()'s own plugin).
  // Confirmed against Dispatcharr's own source that there is no other way
  // to reach this data over HTTP at all: the /file/ endpoint always
  // serves exactly custom_properties.file_path with no way to redirect it
  // at a sibling .edl file, and no generic static route reaches
  // /data/recordings/... either -- see the plugin's own README for the
  // full investigation. Returns true with an empty `out` (not an error)
  // when the recording simply has no markers -- comskip never ran, found
  // nothing, or ran in "cut" mode (which deletes the .edl file once it's
  // done physically removing the commercials, so there's nothing left to
  // report) -- since that's the normal outcome for most recordings, not a
  // failure. Only returns false for a genuine call failure (plugin not
  // installed/enabled, wrong account, network error).
  bool GetRecordingEdl(int recordingId, std::vector<RecordingEdlEntry>& out, std::string& error);
  bool DeleteRecording(int recordingId, std::string& error);
  // Confirmed against the live schema: POST .../recordings/{id}/stop/
  // "Stop[s] a recording early while retaining the partial content for
  // playback" -- distinct from DeleteRecording(), which removes the file
  // entirely. Kodi's "Stop Recording" action (and "Delete" on a timer
  // it knows is still recording) both call this addon's DeleteTimer()
  // with forceDelete=true specifically to mean "this is still recording"
  // (confirmed against Kodi's own source, xbmc/pvr/timers/PVRTimers.cpp);
  // see PVRDispatcharr::DeleteTimer() for why that maps to this call, not
  // DeleteRecording().
  bool StopRecording(int recordingId, std::string& error);
  // Confirmed against Dispatcharr's real source (apps/channels/
  // api_views.py's RecordingViewSet.update_metadata), not its OpenAPI
  // schema -- DRF-spectacular's auto-generated requestBody for this
  // custom @action just reuses the whole Recording serializer and
  // doesn't mention title/description at all, despite the endpoint's
  // own docstring saying exactly that's what it updates. The real body
  // is a plain {"title": ...}; the view writes it into
  // custom_properties.program.title -- the exact field GetRecordings()
  // already reads on the way in (see its own comment) -- and sets
  // custom_properties.program.user_edited = true so the EPG
  // auto-enrichment task won't overwrite it on a later sync. Empty/
  // whitespace-only titles are rejected server-side (400), so callers
  // don't need their own blank-title guard on top.
  bool RenameRecording(int recordingId, const std::string& newTitle, std::string& error);
  // POST /api/channels/recordings/{id}/extend/, {"extra_minutes": N} --
  // deliberately NOT just another UpdateOneTimeRecording() PATCH call for
  // an in-progress recording. Confirmed against this endpoint's own real
  // source (apps/channels/api_views.py's RecordingViewSet.extend): a
  // generic PATCH goes through the model's normal .save() path, which
  // fires a pre_save signal that revokes the scheduled/running Celery
  // recording task -- i.e. would stop the recording rather than extend
  // it. This endpoint instead uses queryset.update() specifically to
  // bypass that signal; the still-running task's own 2-second polling
  // loop re-reads end_time from the DB and extends its deadline live.
  // Server-side rejects extra_minutes <= 0 (400) and an already-finished
  // recording (400, checked via custom_properties.status) -- both
  // surfaced to the caller as a plain false/error, not a crash.
  bool ExtendRecording(int recordingId, int extraMinutes, std::string& error);

  // True if Config::apiKey is already set. Callers use this to decide
  // whether ObtainApiKey() is worth calling at all. Deliberately reads
  // m_config.apiKey directly rather than through GetApiKey() -- safe
  // unlocked only because its one real call site (PVRDispatcharr's
  // constructor) runs before any thread that could concurrently call
  // ObtainApiKey() exists yet; do not add a second call site without
  // reconsidering that.
  bool HasApiKey() const
  {
    return !m_config.apiKey.empty();
  }
  // The connection settings (host/port/useHttps/verifySsl/timeoutSeconds)
  // this client was actually constructed with -- added 2026-09-27, a
  // 47th-pass audit, fixing a real, confirmed bug found via a
  // project-wide review, not itself independently reproduced: unlike
  // apiKey, none of these ever change after construction (a host/
  // username/etc. settings change always returns
  // ADDON_STATUS_NEED_RESTART instead, tearing down this whole
  // instance -- OnAddonSettingChanged()'s own comment), so this is safe
  // to call from any thread with no locking, the same reasoning as
  // m_apiKeyOwnerHost/m_apiKeyOwnerUsername (PVRDispatcharr.h).
  // StartRealtimeUpdateThread()'s own reconnect loop used to call
  // PVRDispatcharr::LoadConfigFromSettings() fresh on every iteration
  // instead -- a *live* settings read, not a construction-time snapshot
  // -- while using this client's own (construction-time) JWT
  // (GetAccessToken() below) for the connection itself. During the
  // window between a host/username change being delivered and Kodi's
  // own blocking NEED_RESTART OK dialog actually being dismissed (this
  // old, not-yet-destroyed instance keeps running the whole time,
  // confirmed against Kodi's real source, AddonStatusHandler.cpp), a
  // reconnect landing in that window sent the *previous* server's own
  // JWT to whatever *new* host/port was just typed in -- a real
  // credential-disclosure risk if that new host happens to accept it
  // (e.g. a cloned/staging instance sharing the same Django SECRET_KEY),
  // and at best a pointless connection attempt otherwise.
  void GetConnectionSettings(std::string& host, int& port, bool& useHttps, bool& verifySsl, int& timeoutSeconds) const
  {
    host = m_config.host;
    port = m_config.port;
    useHttps = m_config.useHttps;
    verifySsl = m_config.verifySsl;
    timeoutSeconds = m_config.timeoutSeconds;
  }
  // A currently-valid JWT access token, for the real-time-updates
  // WebSocket connection (see PVRDispatcharr's realtime-update thread) --
  // logs in/refreshes first via EnsureAuthenticated() if needed.
  // Dispatcharr's own WebSocket auth middleware only checks the token
  // once, at connect time (confirmed by reading its JWTAuthMiddleware
  // source), so an already-open connection keeps working past the
  // token's own 30-minute expiry; a fresh one is only needed when
  // (re)connecting.
  bool GetAccessToken(std::string& tokenOut, std::string& error);
  // Current API key, e.g. to re-persist it after OpenRecordingStream()/
  // ReadRecordingStream() have silently swapped a stale one for the
  // account's current key (see ObtainApiKey()) -- this client has no
  // knowledge of Kodi's settings storage, so the caller must notice the
  // change and save it itself. Thread-safe (m_apiKeyMutex) --
  // ObtainApiKey() can be triggered by a self-healing 401 recovery from
  // any of this client's several stream-opening call sites, on whichever
  // Kodi/background thread happens to be using them, concurrently with
  // another thread reading the key for its own request.
  std::string GetApiKey() const
  {
    std::lock_guard<std::mutex> lock(m_apiKeyMutex);
    return m_config.apiKey;
  }
  // Makes sure this client holds a working API key for the configured
  // account, storing it in this client's own config for immediate use by
  // OpenRecordingStream()/ReadRecordingStream() -- the caller must persist
  // it itself (see GetApiKey()). The one entry point for every "I need a
  // key" path: first-run setup, a stored key that belongs to a different
  // account, and every 401 self-heal.
  //
  // Reads the account's EXISTING key from Dispatcharr first
  // (GET /api/accounts/api-keys/) and adopts it as-is; only generates a
  // new one when the account genuinely has none, or an older Dispatcharr
  // can't report it. Dispatcharr keeps exactly ONE key per account and
  // generating overwrites it unconditionally (confirmed against a live
  // instance and its real upstream source), so every generate silently
  // revokes the key for every other client of that account -- other Kodi
  // installs, scripts, MCP/automation tools. This used to generate
  // unconditionally on first run and on every 401; a real report against
  // 0.11.0 showed just enabling the addon broke every other client on the
  // account, and a live two-install test (docs/OPEN_ITEMS.md) showed two
  // Kodi installs playing back at once churned the key roughly every 24s.
  // With this, every install and tool on one account converges on the one
  // shared key instead.
  //
  // Returns false without generating anything when the lookup itself
  // failed transiently (no response, 5xx, 408/429): rotating a key blind
  // could revoke a perfectly good one, so the caller's own retry
  // (OpenRecordedStream() re-tries on every open, each 401 path once per
  // read) tries again later instead. Serialized by m_apiKeyRecoveryMutex --
  // two threads recovering at once used to be able to each generate a key
  // and leave m_config.apiKey holding the loser's. Never logs the key.
  bool ObtainApiKey(std::string& keyOut, std::string& error);

  bool GetTimerRules(std::vector<TimerRule>& out, std::string& error);
  // title is used only as a client-side placeholder (see GetRecordings()'s
  // pending-title cache) -- not sent to Dispatcharr itself; see the .cpp for
  // why. isEpgBased (added 2026-09-29, fixing a real, confirmed,
  // live-verified bug -- see dispatcharr::BuildOneTimeRecordingCreateBody()'s
  // own comment, TimerRequestBuilder.h) should be true only for a
  // genuinely EPG-based timer (timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID)
  // -- lets Dispatcharr's own configured pre/post padding actually apply
  // to a recording created by pressing "Record" in Kodi's own EPG guide,
  // which previously always got the raw, unpadded EPG start/end times
  // instead.
  bool CreateOneTimeRecording(int channelId, time_t start, time_t end, const std::string& title, bool isEpgBased,
                              std::string& error);
  // Reschedules an existing one-time recording's start/end time via
  // PATCH /api/channels/recordings/{id}/. Deliberately sends ONLY
  // start_time/end_time, mirroring CreateOneTimeRecording()'s own choice
  // not to touch custom_properties -- confirmed two things live against a
  // real EPG-matched recording before relying on either: (1) a PATCH
  // that omits both times crashes with an uncaught 500 (Dispatcharr's own
  // RecordingSerializer.validate() does `end_time < now` with end_time
  // still None on a bare partial update -- a real server-side bug, not
  // something this addon can prevent except by never sending that shape
  // of request), so both fields are always included, never a bare
  // partial; (2) resending the *same* start/end time an EPG-matched
  // recording already had did NOT drift them via repeated pre/post-offset
  // reapplication, despite that being a real risk suggested by
  // validate()'s own source (it re-derives the offset-adjusted times
  // whenever custom_properties.program is a dict and both times are
  // present) -- confirmed live, not just theorized either way, so this
  // method sends exactly the new times without trying to work around a
  // compounding-offset bug that didn't actually reproduce.
  //
  // channelId (0 = leave unchanged) moves a not-yet-started recording to
  // another channel, alongside the two times (see
  // BuildOneTimeRecordingPatchBody()). Confirmed live 2026-09-30 that this
  // reschedules cleanly; Dispatcharr leaves the recording's
  // custom_properties.program snapshot (an EPG-derived title/subtitle/
  // description) as it was, so those stay the old channel's.
  bool UpdateOneTimeRecording(int recordingId, time_t start, time_t end, int channelId, std::string& error);
  // recordNewOnly maps to Dispatcharr's SeriesRuleRequest.mode ("new" vs
  // the server default "all") -- confirmed against the live schema: "all"
  // records every matching episode including reruns, "new" only
  // first-run ones.
  //
  // titleMode/description/descriptionMode/untaggedIsNew/epgSourceId
  // added 2026-09-29 -- see dispatcharr::BuildSeriesRuleRequestBody()'s
  // own comment (TimerRequestBuilder.h) for the real, live-confirmed bug
  // this fixes. AddTimer()'s create path (nothing cached yet) passes
  // ""/""/""/false/0; UpdateTimer()'s edit path passes the cached
  // TimerRule's own values so an edit doesn't collaterally reset them.
  bool CreateSeriesRule(int channelId, const std::string& tvgId, const std::string& titlePattern, bool recordNewOnly,
                        const std::string& titleMode, const std::string& description,
                        const std::string& descriptionMode, bool untaggedIsNew, int epgSourceId, std::string& error);
  // Series rules have no numeric id in Dispatcharr's API at all -- they're
  // deleted by DELETE /api/channels/series-rules/?title=...&tvg_id=...
  // (confirmed against the live OpenAPI schema), not by path id.
  //
  // epgSourceId added 2026-09-29 -- see
  // dispatcharr::BuildSeriesRuleDeleteQuery()'s own comment
  // (TimerRequestBuilder.h) for the real, confirmed data-loss bug this
  // fixes (omitting it deleted every EPG source's own copy of a rule
  // sharing title+tvgId, not just the one pinned copy the user meant).
  bool DeleteSeriesRule(const std::string& title, const std::string& tvgId, int epgSourceId, std::string& error);
  // Resolves the tvg_id that actually backs a channel's *effective* EPG
  // data row (its epgDataId), rather than trusting the channel's own
  // (possibly stale) tvgId field directly -- confirmed live against a real
  // instance: a channel-level EPG-data override had repointed epgDataId at
  // a different EPG source's row (an auto-channel-merge tool's doing)
  // without updating the channel's own tvg_id, so the channel's tvgId
  // matched a *different*, non-effective EPGData row entirely. Series-rule
  // create/evaluate on Dispatcharr's side matches purely by tvg_id, so
  // that stale value made it resolve against the wrong EPG copy and
  // silently schedule nothing -- POST .../series-rules/ and .../evaluate/
  // both reported success throughout. Best-effort: falls back to
  // fallbackTvgId (the channel's own tvgId) if epgDataId is 0 or the
  // lookup fails, so a channel without this kind of drift -- the common
  // case -- behaves exactly as before, at the cost of one extra request
  // per series-rule add/update/delete (a rare, user-triggered action, not
  // part of bulk channel refresh).
  std::string ResolveSeriesRuleTvgId(int epgDataId, const std::string& fallbackTvgId);

  bool GetRecurringRules(std::vector<RecurringRule>& out, std::string& error);
  // daysOfWeek: 0=Monday..6=Sunday (see RecurringRule's own comment).
  // startTimeOfDaySeconds/endTimeOfDaySeconds: seconds since midnight in
  // Dispatcharr's own configured system timezone, already offset-adjusted
  // by the caller (PVRDispatcharr::AddTimer()) using the
  // recurring_rule_utc_offset_minutes setting -- this method sends them
  // through as plain "HH:MM:SS" with no further conversion. startDate/
  // endDate: UTC time_t values (midnight); only the calendar date portion
  // is sent, as Dispatcharr's own start_date/end_date DateField expects.
  // Dispatcharr's serializer requires both start_date and end_date on
  // create despite the model declaring them nullable (confirmed against
  // its live validation code) -- endDate should already reflect the
  // caller's chosen "how far out" default (see AddTimer()), not left at 0.
  // `enabled` reflects Kodi's own timer-settings dialog Enabled toggle at
  // create time -- fixed for a real, confirmed bug found via a
  // project-wide review, not itself independently reproduced: this used
  // to always send `enabled: true` unconditionally, silently ignoring a
  // user unchecking Enabled while creating a new recurring timer (the
  // same dialog UpdateTimer()'s own edit path already honors this for,
  // since PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE -- see GetTimerTypes() --
  // is a per-timer-type capability Kodi's shared Add/Edit dialog applies
  // to both, not an edit-only affordance) -- a rule created disabled, to
  // stage it for later, instead started materializing and recording
  // occurrences immediately.
  bool CreateRecurringRule(int channelId, const std::string& name, const std::vector<int>& daysOfWeek,
                           int startTimeOfDaySeconds, int endTimeOfDaySeconds, time_t startDate, time_t endDate,
                           bool enabled, std::string& error);
  // Edits an existing recurring rule -- also how Kodi's own "enable/
  // disable" timer action reaches this rule type
  // (PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE), since Dispatcharr has no
  // separate enable/disable endpoint, just this same field on the rule
  // itself. Deliberately a PARTIAL PATCH that omits end_date -- confirmed
  // against RecurringRecordingRuleSerializer's own source that a missing
  // field falls back to the existing instance's value rather than
  // failing validation (unlike RecordingSerializer's create-oriented
  // validate(), this one was written partial-update-safe), so the rule's
  // existing end_date (set once at creation, see CreateRecurringRule()'s
  // own comment on why that's a somewhat arbitrary "far enough out"
  // value) is preserved automatically rather than needing to be
  // re-fetched and resent on every edit.
  //
  // Sends only the fields in `patch` (see RecurringRuleEdit.h) -- an edit of one
  // field no longer re-sends, and so reverts, the others.
  // `httpStatusOut` (optional) receives the response's HTTP status, 0 when there was none -- what
  // lets a caller tell a rejection that will repeat from a transient failure.
  bool UpdateRecurringRule(int ruleId, const dispatcharr::RecurringRuleEditPatch& patch, std::string& error,
                           long* httpStatusOut = nullptr);
  // One rule, fresh from the server (the cached list can be minutes old). A
  // missing rule is reported through `httpStatusOut` (404), not just `error`.
  bool GetRecurringRuleById(int ruleId, RecurringRule& out, std::string& error, long* httpStatusOut = nullptr);
  bool DeleteRecurringRule(int ruleId, std::string& error);
  // Extends an existing recurring rule's end_date forward -- a partial
  // PATCH sending only that one field, the same partial-update-safe
  // pattern UpdateRecurringRule() relies on. Called periodically by
  // PVRDispatcharr's background renewal (see its own comment) to keep a
  // "permanent" recurring rule's materialized-occurrence window topped up,
  // now that CreateRecurringRule() itself sets a much shorter initial
  // end_date than it originally did -- confirmed live that Dispatcharr
  // eagerly materializes every occurrence between start_date and end_date
  // synchronously on create/update, not a lazy rolling window as this
  // addon's own docs used to (incorrectly) assume, so a single far-future
  // end_date isn't "cheap" the way it was once thought to be; periodic
  // small extensions here are what actually keeps that cost bounded.
  // Confirmed live, separately: an update like this one, that changes
  // end_date, only regenerates *future* (not yet started) occurrences --
  // an already in-progress or completed one survives untouched, same id,
  // same file, `started_at` unchanged -- see docs/RECURRING_RULES.md. The
  // caller (PVRDispatcharr::RenewRecurringRules()) still skips calling
  // this at all for a rule with an occurrence currently recording or about
  // to start soon, as defense in depth rather than relying solely on that
  // server-side scoping.
  bool ExtendRecurringRuleEndDate(int ruleId, time_t newEndDate, std::string& error);

  // Dispatcharr's global recording pre/post padding, in minutes
  // (custom_properties has no per-recording/per-rule equivalent -- this
  // is genuinely global-only, confirmed against Dispatcharr's own
  // source). Only actually applied server-side to EPG-based scheduling
  // where custom_properties.program is present -- confirmed a
  // recurring (day-of-week) rule's own scheduler never reads or applies
  // this at all, a real inconsistency on Dispatcharr's side this addon
  // can't fix, just report accurately. Corrected 2026-09-26 (a 25th-pass
  // audit, confirmed against Dispatcharr's own real current upstream
  // source): "an EPG-matched one-time recording" here used to describe
  // this addon's own CreateOneTimeRecording()-created recordings too --
  // it doesn't. That call deliberately sends no custom_properties at
  // all (see its own comment), so RecordingSerializer.validate()'s own
  // isinstance(custom_properties.get("program"), dict) check is never
  // true for a recording created through this addon, and no padding is
  // ever applied to it -- only true of one created through Dispatcharr's
  // own UI. See docs/RECORDINGS.md's own follow-up note and
  // docs/OPEN_ITEMS.md for the full account; not yet fixed, needs a live
  // test first.
  bool GetDvrOffsetMinutes(int& preMinutesOut, int& postMinutesOut, std::string& error);
  // Read-modify-write: Dispatcharr stores this alongside several other,
  // unrelated settings (comskip mode/hw-accel, recording path templates)
  // in the same single JSON blob (CoreSettings key "dvr_settings") --
  // confirmed live that this is one shared row, not a dedicated one for
  // just padding. Fetches the current blob first and only changes the
  // two offset keys within it, so a naive whole-field overwrite doesn't
  // silently wipe out the unrelated settings sharing that same row.
  //
  // preMinutes/postMinutes are nullable: nullptr leaves that offset
  // exactly as this call's own fresh fetch found it, rather than
  // overwriting it with a value the caller supplies -- see
  // dispatcharr::MergeDvrOffsetMinutes()'s own comment (JsonFieldUtil.h)
  // for the real, confirmed bug this fixes.
  //
  // `onResult`, when given, is called with the outcome while the call still holds the
  // DVR-settings mutex, so the order callers learn their results in is the order the requests
  // reached the server (found by the 2026-10-04 eighth hardening sweep: results applied after
  // the call returned could be applied in a different order than the PATCHes landed, which no
  // bookkeeping can undo). It must not call back into this client.
  // The same push, but with what to send decided under m_dvrSettingsMutex: `choose` fills in the
  // sides to send (left empty = not sent), after any push that was ahead of it has finished. When it
  // leaves both empty nothing is sent, `onResult` is not called and true is returned.
  bool SetDvrOffsetMinutesChosen(const std::function<void(std::optional<int>& pre, std::optional<int>& post)>& choose,
                                 std::string& error, const std::function<void(bool)>& onResult);
  bool SetDvrOffsetMinutes(const int* preMinutes, const int* postMinutes, std::string& error,
                           const std::function<void(bool)>& onResult = nullptr);

  // Dispatcharr's own configured system timezone (CoreSettings key
  // "system_settings", field "time_zone"), as a raw IANA zone name --
  // confirmed live against a real instance. Surfaced
  // as read-only info next to recurring_rule_utc_offset_minutes so the
  // user has a concrete reference for what numeric offset to enter there,
  // without this addon needing to bundle a real timezone database just to
  // compute that offset itself (see docs/RECURRING_RULES.md for why that
  // was deliberately ruled out).
  bool GetSystemTimeZone(std::string& timeZoneOut, std::string& error);

  // Auto-computes the current UTC offset (minutes) for the small set of
  // well-known IANA zones (US/Canada, UK/EU) hardcoded in
  // TimeZoneUtil.cpp, using their real, stable DST transition rules -- see
  // docs/RECURRING_RULES.md for why a full timezone database isn't bundled
  // to do this for every possible zone instead. Returns false
  // (offsetMinutesOut untouched) for any zone not in that short list, in
  // which case recurring_rule_utc_offset_minutes still needs to be set
  // manually. Pure computation, no network/instance state needed --
  // static so PVRDispatcharr's constructor can call it directly. Just a
  // thin delegate to the free function in TimeZoneUtil.h -- the actual
  // logic lives there specifically so it's unit-testable standalone (no
  // Kodi/curl dependency); this static method stays as the public entry
  // point other callers already use.
  // `nowUtc` is a parameter purely for testability; real callers should
  // always pass the actual current time.
  static bool ComputeKnownZoneOffsetMinutes(const std::string& ianaZoneName, time_t nowUtc, int& offsetMinutesOut,
                                            bool applyZoneRuleChanges = true);

  // GET /api/core/version/ -- public, no auth required at all (confirmed
  // against the live source, core/api_views.py's `version` view:
  // @permission_classes([AllowAny])), returns {"version": ..., "timestamp":
  // ...} straight from Dispatcharr's own version.py. Used by
  // PVRDispatcharr::GetBackendVersion(), which previously had no real
  // server-version source and reported this addon's own protocol version
  // instead.
  // `httpStatusOut` (when non-null) is the HTTP status, or 0 when no response was received at all.
  bool GetServerVersion(std::string& versionOut, std::string& error, long* httpStatusOut = nullptr);

  // Starts the same short, non-escalating cooldown a transient Login() failure does, without attempting a
  // login: for a caller that has just learned the server is not answering (the startup version check timed out)
  // and would otherwise make every later startup call, beginning with the login, wait out a full timeout of its
  // own. The background thread's deferred syncs and the next EnsureAuthenticated() after the cooldown try again.
  void DeferAuthenticationAfterUnresponsiveServer(const std::string& reason);

  // GET /api/core/timezones/ -- requires auth (confirmed live: 401 without
  // it, matching its view's plain `Authenticated()` permission, unlike
  // GetServerVersion()'s AllowAny). Confirmed against the real source
  // (core/api_views.py's TimezoneListView): returns
  // {"timezones": [...], "grouped": {...}, "count": N} where "timezones"
  // is sorted(pytz.common_timezones), ~440 real IANA names. This addon
  // still can't compute a correct DST-adjusted offset for most of them
  // (see kKnownTimeZones's own comment for why only two DST families are
  // modeled) -- used only to distinguish, in the startup timezone-sync
  // diagnostic, "a real zone this addon just doesn't have DST rules for"
  // from "not a recognized IANA zone at all" for whatever Dispatcharr
  // reports itself configured to.
  bool GetSupportedTimezones(std::vector<std::string>& timezonesOut, std::string& error);

  // GET /api/accounts/users/me/ -- confirmed against the real source
  // (apps/accounts/api_views.py's UserViewSet.me): needs only ordinary
  // authentication, not admin, to read your own account -- a deliberately
  // low-privilege "check my own access level" endpoint, not the
  // admin-only user list/detail routes. Returns user_level among other
  // fields; confirmed against apps/accounts/permissions.py that IsAdmin
  // is exactly user_level >= 10 (matches Dispatcharr's own
  // User.UserLevel.ADMIN choice), and separately confirmed
  // CoreSettingsViewSet's update/partial_update actions (what
  // SetDvrOffsetMinutes() calls) require exactly that IsAdmin permission
  // -- this really is the same admin check the padding write itself
  // needs, not a guess. Used only to
  // decide whether to grey out the recording-padding settings in Kodi's
  // UI (see PVRDispatcharr's constructor); not itself a permission gate
  // this addon enforces -- Dispatcharr's own server-side check is still
  // what actually matters.
  bool IsCurrentUserAdmin(bool& isAdminOut, std::string& error);

  // CoreSettings key "system_settings", field "catchup_enabled" -- the
  // instance-wide catch-up on/off switch. Confirmed live 2026-09-28
  // (docs/OPEN_ITEMS.md) that Dispatcharr's own catch-up endpoint
  // (apps/timeshift/api_views.py) 403s "Catch-up is disabled" when this
  // is false, entirely independent of any per-channel catchupEnabled
  // flag -- see dispatcharr::ShouldOfferCatchup()'s own comment
  // (EpgTagUtil.h) for what this addon does with it. Defaults
  // enabledOut to `true` and returns the fetch's own success/failure
  // separately, so a startup-time caller can fail open (same reasoning
  // as IsCurrentUserAdmin()'s own comment: Dispatcharr's own 403 is
  // still the authoritative enforcement either way) rather than
  // spuriously hiding catch-up entirely just because this one read
  // failed.
  // The account's DVR access level (see DvrAccess.h), from the same
  // /api/accounts/users/me/ response IsCurrentUserAdmin() reads.
  bool GetCurrentUserDvrAccess(dispatcharr::DvrAccess& accessOut, std::string& error);
  bool IsCatchupEnabledGlobally(bool& enabledOut, std::string& error);

  // GET /api/accounts/users/me/ -- same endpoint IsCurrentUserAdmin()
  // already calls, just reading a different field
  // (custom_properties.catchup_enabled) from the same response. Kept as
  // its own separate call rather than an extra out-parameter on
  // IsCurrentUserAdmin() itself, so each function still answers exactly
  // one question -- the extra round trip is a one-time startup cost, not
  // a hot path. See IsCatchupEnabledGlobally()'s own comment for the
  // fail-open convention this matches.
  bool IsCatchupEnabledForCurrentUser(bool& enabledOut, std::string& error);

  // Raw byte-range recording playback, called through the addon's
  // OpenRecordedStream/ReadRecordedStream/SeekRecordedStream/
  // LengthRecordedStream. Kodi's kodi-dev-kit docs describe
  // PVR_STREAM_PROPERTY_STREAMURL as a fallback used only when an addon
  // doesn't implement these -- but confirmed against a real failure (a live
  // kodi.log showed Kodi's generic CCurlFile hitting a populated STREAMURL
  // directly, bypassing these entirely, including the 401-retry logic
  // below) that populating STREAMURL anyway is NOT harmless once these are
  // implemented: Kodi will happily use it instead, silently skipping this
  // code path. GetRecordingStreamProperties() deliberately leaves STREAMURL
  // unset for a completed recording for that reason. Only supports a
  // completed recording (a real, Range-seekable file) -- an in-progress one
  // is instead served via OpenInProgressRecordingStream() below.
  bool OpenRecordingStream(int recordingId, std::string& error);
  int ReadRecordingStream(uint8_t* buffer, unsigned int size);
  int64_t SeekRecordingStream(int64_t position, int whence);
  int64_t GetRecordingStreamLength() const;
  void CloseRecordingStream();

  // Growing, seekable byte-stream access to the server-side live timeshift
  // buffer -- the actual consumer of StartTimeshiftBuffer() above. Exposes
  // the buffer to Kodi via OpenLiveStream/ReadLiveStream/SeekLiveStream
  // (PVRCapabilities::SetHandlesInputStream), the same "one growing/
  // seekable byte source, Kodi's own internal demuxer does the actual
  // MPEG-TS parsing and PTS-based seek refinement" pattern already proven
  // for completed recordings (OpenRecordingStream/ReadRecordingStream/
  // SeekRecordingStream above), just against the companion plugin's
  // per-segment Range-served files instead of one Dispatcharr-served
  // recording file -- confirmed live: real pause/rewind/fast-forward/
  // live-follow on a real channel, including a 95-second rewind spanning
  // several manifest refreshes. This replaced an earlier STREAMURL +
  // inputstream.ffmpegdirect approach that routed through ffmpegdirect's
  // own generic HLS seek instead of Kodi's native demuxer -- confirmed
  // broken 100% of the time regardless of direction or position (see
  // docs/TIMESHIFT.md for that investigation). Calls StartTimeshiftBuffer()
  // itself first to ensure a buffer is actually running for this channel
  // (same as the plain-Play path already does).
  bool OpenLiveTimeshiftStream(const std::string& channelUuid, std::string& error);
  int ReadLiveTimeshiftStream(uint8_t* buffer, unsigned int size);
  int64_t SeekLiveTimeshiftStream(int64_t position, int whence);
  int64_t GetLiveTimeshiftStreamLength();
  // Duration of the buffer currently known to be available, in milliseconds
  // -- for PVRDispatcharr::GetStreamTimes()'s ptsEnd, which must grow as the
  // live buffer does (see kodi-dev-kit's own PVRStreamTimes doc comment:
  // "For Live TV, this must be ... point to end of the timeshift buffer").
  int64_t GetLiveTimeshiftStreamDurationMs();
  // Where the seekable part of the live-timeshift stream begins, as a
  // timestamp in ms (matching GetLiveTimeshiftStreamDurationMs()'s own
  // timeline): 0 until the plugin's rolling buffer has started letting
  // segments go, then the start of the oldest one it still has. For
  // GetStreamTimes()'s PTSBegin. 0 when no stream is open.
  int64_t GetLiveTimeshiftStreamBeginMs();

  // Byte position of the oldest segment still real server-side -- see
  // dispatcharr::FirstAvailableLiveSegmentIndex(). 0 when nothing has rolled
  // off yet. The caller must hold m_liveStateMutex.
  int64_t FirstAvailableLiveByteOffset() const;
  // Real UTC wall-clock moment corresponding to this session's local byte
  // 0/PTS 0 (see LiveTimeshiftStreamState::wallClockAnchor's own comment)
  // -- for GetStreamTimes()'s startTime, which must NOT be 0/unset: Kodi-
  // core's CPVRGUITimesInfo::UpdateTimeshiftData() treats a falsy start
  // time as "no real timeshift bounds available" and substitutes the
  // *current playback position* for both its internal min and max time
  // instead, collapsing them to the same value. That makes its own
  // "is timeshifting supported" check (end > start) always false, which
  // makes the on-screen seek bar's position tracking fall back to raw
  // wall-clock time -- confirmed live via screenshots: the displayed
  // position kept climbing with real time regardless of where a seek
  // actually landed, both backward and forward, while the underlying
  // playback content genuinely did seek correctly. See
  // docs/TIMESHIFT.md's "PVR.TimeshiftProgress*"/seek bar section.
  time_t GetLiveTimeshiftStreamWallClockAnchor();
  void CloseLiveTimeshiftStream();
  // Genuine "is a live-timeshift stream currently open" state, as opposed to
  // just "is server-side timeshift mode enabled in settings" -- the latter
  // doesn't change once a stream closes, so PVRDispatcharr's GetStreamTimes()/
  // CanPauseStream()/CanSeekStream()/IsRealTimeStream() need this to avoid
  // misreporting live-timeshift state while an in-progress recording (or a
  // plain completed recording) is what's actually open. See
  // IsInProgressRecordingStreamOpen()'s own comment for why these callbacks
  // need per-stream-flavour state at all.
  bool IsLiveTimeshiftStreamOpen() const;

  // Growing, seekable byte-stream access to an in-progress recording --
  // the same "expose a growing HLS source as one fixed-origin byte
  // address space, let Kodi's own native demuxer handle MPEG-TS parsing
  // and seek refinement" pattern as OpenLiveTimeshiftStream() above,
  // applied to a recording instead of a live channel. Replaced an earlier
  // STREAMURL + inputstream.ffmpegdirect approach (see git history /
  // docs/RECORDINGS.md) that needed a "play from start (seek)"
  // vs. "play live (follow, no seek)" toggle -- a limitation of routing
  // through libavformat's own HLS demuxer, which won't offer seeking
  // without a #EXT-X-ENDLIST-terminated (i.e. static, no-longer-growing)
  // playlist. CInputStreamPVRRecording extends the same
  // CInputStreamPVRBase as CInputStreamPVRChannel (confirmed in Kodi-core
  // source), so GetStreamTimes()/CanPauseStream()/CanSeekStream()/
  // IsRealTimeStream() apply identically here -- a recording reported
  // through those the same way the live buffer is gets real seek and
  // live-follow simultaneously, no toggle needed. Unlike live-timeshift,
  // there's no server-side buffer to start/stop: Dispatcharr's own DVR
  // task keeps writing the recording regardless of whether this addon is
  // reading it, so opening always starts at true byte 0, matching normal
  // recording/VOD conventions (and the existing completed-recording
  // behaviour). startTime is the recording's own real, known start time
  // (kodi::addon::PVRRecording::GetRecordingTime(), the exact value
  // PVRDispatcharr::GetRecordings() already populated it with) -- simpler
  // than live-timeshift's own "anchor at the trim point" approach below,
  // since a recording always starts at true byte 0 with a genuinely known
  // start time, no cold-start trim to anchor instead.
  bool OpenInProgressRecordingStream(int recordingId, time_t startTime, std::string& error);
  int ReadInProgressRecordingStream(uint8_t* buffer, unsigned int size);
  int64_t SeekInProgressRecordingStream(int64_t position, int whence);
  int64_t GetInProgressRecordingStreamLength();
  // Mirrors GetLiveTimeshiftStreamDurationMs() -- for GetStreamTimes()'s
  // ptsEnd, which must grow as the recording does.
  int64_t GetInProgressRecordingStreamDurationMs();
  // Mirrors GetLiveTimeshiftStreamWallClockAnchor() -- for GetStreamTimes()'s
  // startTime, same "must not be 0/unset" requirement (see that function's
  // own comment for the Kodi-core mechanism). The value itself is simpler
  // here: the recording's real start time, passed in at Open() time,
  // rather than something computed from a cold-start trim.
  time_t GetInProgressRecordingStreamStartTime();
  void CloseInProgressRecordingStream();
  // PVRDispatcharr uses this to tell which of OpenRecordedStream()'s two
  // implementations (this one, or the plain completed-recording one) is
  // the one currently open, since ReadRecordedStream()/SeekRecordedStream()/
  // LengthRecordedStream()/GetStreamTimes()/CanPauseStream()/CanSeekStream()/
  // IsRealTimeStream() are all shared Kodi PVR client callbacks with no
  // parameter telling them which recording-stream flavour is active.
  bool IsInProgressRecordingStreamOpen() const;

private:
  // SetDvrOffsetMinutes()'s GET-merge-PATCH; the caller holds m_dvrSettingsMutex.
  bool SetDvrOffsetMinutesLocked(const int* preMinutes, const int* postMinutes, std::string& error);

  std::string BaseUrl() const;
  // Whether `url` (taken from a server-supplied playlist) is on exactly the configured
  // scheme, host and port: the only place the X-API-Key may be sent for it. A request to
  // anything else is not made at all -- see dispatcharr::IsSameOrigin().
  bool IsOnConfiguredServer(const std::string& url) const;
  // Whether the one warning about such a segment has been logged yet (it is retried every refresh).
  mutable std::atomic<bool> m_loggedForeignSegment{false};
  // httpStatusOut (added 2026-09-27, a 43rd-pass audit -- see
  // dispatcharr::ShouldCountTowardLoginBackoff()'s own comment,
  // AuthBackoff.h, for why): the real HTTP status /api/accounts/token/
  // returned, or 0 if no response was received at all (a transport
  // failure). Left null by the 401-retry call site inside Request()
  // itself, which only cares about the plain bool; EnsureAuthenticated()
  // passes a real pointer so it can decide whether this failure should
  // count toward its own consecutive-failure backoff.
  bool Login(std::string& error, long* httpStatusOut = nullptr, long timeoutMsOverride = 0);
  // httpStatusOut (added 2026-09-27, a 47th-pass audit, fixing a real,
  // confirmed gap found via a project-wide review, not itself
  // independently reproduced -- see EnsureAuthenticated()'s own comment
  // for the full account): before this, EnsureAuthenticated() tried
  // RefreshAccessToken() *before* either of its own backoff gates, with
  // no way to classify a failure here the way Login()'s own
  // httpStatusOut lets it. A refresh token, once obtained, never expires
  // from this addon's own perspective until it's actually rejected --
  // unlike Login(), which only runs once per outage until its own
  // backoff clears -- so this was actually the *more* common path for a
  // mid-session outage to hit completely unthrottled, not a rare corner
  // of it.
  bool RefreshAccessToken(std::string& error, long* httpStatusOut = nullptr, long timeoutMsOverride = 0);

  // Finds the one CoreSettings row with the given key and returns its id
  // and full value blob (unmodified) -- GetDvrOffsetMinutes()/
  // SetDvrOffsetMinutes() (key kDvrSettingsKey) and GetSystemTimeZone()
  // (key kSystemSettingsKey) all need this same lookup, just against
  // different rows of the same /api/core/settings/ list.
  bool FindCoreSettingsRow(const std::string& key, int& idOut, nlohmann::json& valueOut, std::string& error,
                           bool* valueWasObjectOut = nullptr);

  // Shared by GetRecordings() (once per list item) and GetRecordingById()
  // (once, for its single item) so the two never drift apart -- same
  // Recording, whether Dispatcharr handed it over as part of a list or on
  // its own. `now` is passed in (rather than called here) so a caller
  // parsing a whole list only reads the clock once, not once per item.
  Recording ParseRecordingJson(const nlohmann::json& item, time_t now);

  // Fetches the raw HLS playlist text for an in-progress recording, with a
  // self-healing retry on a 401. Returns false (with `error` set) on a
  // genuine network/HTTP failure. `wasNotFoundOut`, if given, is set true
  // for a definitive HTTP 404, OR a 3xx redirect (Dispatcharr has genuinely
  // removed this recording's HLS directory, per docs/RECORDINGS.md's own
  // documented concat-plus-viewer-wait-grace removal), and false for every
  // other failure (a transient network/5xx blip) -- lets
  // RefreshInProgressRecordingManifest() distinguish "this recording is
  // genuinely gone" from "try again next cycle" instead of treating both
  // identically. The redirect case is real, confirmed against Dispatcharr's
  // own current upstream source, not itself independently reproduced: on a
  // *naturally*-completed recording (not a user Stop), the server's own
  // `hls()` view (apps/channels/api_views.py) 302-redirects a `.m3u8`
  // request to the permanent `/file/` endpoint once its own HLS directory
  // is gone, rather than 404ing the way a Stop/deleted-recording case does
  // -- this addon never sets CURLOPT_FOLLOWLOCATION here, so that 3xx
  // otherwise came back indistinguishable from a plain 5xx/network blip,
  // and RefreshInProgressRecordingManifest()'s own `finished` flag never
  // went true for this case. Called from RefreshInProgressRecordingManifest().
  // `httpStatusOut`, if given, is set to the last HTTP status the server
  // answered with (0 when no response arrived at all) -- what lets
  // RefreshInProgressRecordingManifest() tell a plain 404 ("no playlist yet,
  // the recording is very young") apart from a 3xx, which `wasNotFoundOut`
  // deliberately lumps together with it.
  bool FetchRawInProgressPlaylist(int recordingId, const std::string& playlistUrl, std::string& playlistText,
                                  std::string& error, bool* wasNotFoundOut = nullptr, long* httpStatusOut = nullptr,
                                  long timeoutMsOverride = 0);

  // The CURLSH* behind m_curlShareState, or nullptr if it failed to
  // initialise -- pass to CURLOPT_SHARE on every easy handle this client
  // creates (Request(), OpenRecordingStream()'s probe, ReadRecordingStream()'s
  // persistent handle) so they all pull from one connection/DNS/TLS-session
  // cache. Returns void* (actually CURLSH*) so this header doesn't need
  // <curl/curl.h>; defined in the .cpp, which does.
  void* GetCurlShare() const;

  // A second, separate CURLSH for ProbeSegmentByteSize()'s concurrent probe
  // burst only -- shares DNS/TLS-session but deliberately not connections,
  // to sidestep a real macOS-libcurl connection-cache crash under that
  // specific concurrency pattern. See m_probeCurlShareState's own comment.
  void* GetProbeCurlShare() const;

  // Appends "X-API-Key: <apiKey>" to a curl_slist if apiKey is non-empty,
  // returning the (possibly unchanged) list -- the "attach the current API
  // key if we have one" step every raw-curl call site below needs. `headers`
  // and the return value are void* (actually curl_slist*) for the same
  // <curl/curl.h>-avoidance reason as GetCurlShare().
  static void* AppendApiKeyHeaderIfPresent(void* headers, const std::string& apiKey);

  // Sets the small group of curl options every easy handle in this class
  // configures identically: SSL verification per m_config.verifySsl,
  // m_config.timeoutSeconds, and the given share handle (GetCurlShare() for
  // most callers, GetProbeCurlShare() for ProbeSegmentByteSize()'s
  // concurrent probe burst -- see its own comment for why that one's kept
  // separate). `curl`/`share` are void* (actually CURL*/CURLSH*) for the
  // same reason as GetCurlShare().
  void ApplyStandardCurlOptions(void* curl, void* share) const;
  // Set once the addon instance is being torn down: every request in flight, and every
  // one started afterwards, then ends at once instead of running to its timeout.
  // Mutable because ApplyStandardCurlOptions() is const and hands libcurl its address.
  mutable std::atomic<bool> m_abortRequests{false};

  // curl_easy_perform() on `curl` for `startUrl`, following up to five redirects
  // but only ones dispatcharr::IsSafeRedirectTarget() allows (same host, never
  // https -> http); anything else is returned as the 3xx response. For requests
  // that carry credentials, in place of CURLOPT_FOLLOWLOCATION. `onRedirect`
  // runs before each followed hop to reset the caller's response buffers;
  // `switchPostToGet` repeats a POST answered 301/302/303 as a GET, as libcurl
  // does. Sets CURLOPT_URL itself. Returns a CURLcode (as an int, since this
  // header doesn't include <curl/curl.h>).
  int PerformWithSafeRedirects(void* curl, const std::string& startUrl, const std::function<void()>& onRedirect,
                               bool switchPostToGet);

  // GET /api/accounts/api-keys/ (APIKeyViewSet.list(): `{"key": <str|null>}`
  // for the authenticated caller, permission class `Authenticated` only, so
  // any role can use it) -- reads the account's current key WITHOUT
  // replacing it, unlike GenerateApiKey(). Always sets `lookupOut`
  // (classified via dispatcharr::ParseApiKeyListResponse()/
  // ClassifyApiKeyLookupFailure(), ApiKeyRecovery.h); returns whether the
  // request itself succeeded. Only ObtainApiKey() calls this.
  bool FetchCurrentApiKey(dispatcharr::ServerApiKeyLookup& lookupOut, std::string& error);

  // POST /api/accounts/api-keys/generate/ -- creates a new key and stores
  // it in this client's own config. LAST RESORT, only ever called from
  // ObtainApiKey() when the account genuinely has no key: Dispatcharr keeps
  // one key per account and this overwrites it unconditionally
  // (`user.api_key = secrets.token_urlsafe(40)`, confirmed against
  // Dispatcharr's own real upstream source), so every call silently
  // revokes the previous key for every other client of the account. Kept
  // private so nothing can reach for it directly again.
  bool GenerateApiKey(std::string& keyOut, std::string& error);

  // Performs one HTTP call. `body` is sent as the JSON request body for
  // POST/PATCH/DELETE-with-body; pass an empty object for bodyless calls.
  // On success, parses the response into `responseOut` (may be left null
  // for 204 No Content) and returns true. `httpStatusOut` (added
  // 2026-09-27, a 43rd-pass audit), if non-null, is set to the real HTTP
  // status received, or left at 0 for a transport failure (no response
  // received at all) -- every existing call site leaves this null and is
  // unaffected; only Login() currently passes a real pointer through, so
  // EnsureAuthenticated() can distinguish a genuine credential rejection
  // from a transient network/server failure (see
  // dispatcharr::ShouldCountTowardLoginBackoff()'s own comment,
  // AuthBackoff.h).
  bool Request(const std::string& method, const std::string& path, const nlohmann::json& body,
               nlohmann::json& responseOut, std::string& error, bool withAuth = true, int retryOnAuthFailure = 1,
               long* httpStatusOut = nullptr, long timeoutMsOverride = 0);

  // Calls the timeshift_buffer plugin's run/ endpoint for `action` and
  // unwraps a {status, http_port, playlist_route} response shape. Only
  // StartTimeshiftBuffer() uses this now (SnapshotTimeshiftBuffer(), the
  // other original caller, was removed once server-side timeshift stopped
  // using STREAMURL+ffmpegdirect -- see docs/TIMESHIFT.md). Left as its own
  // function rather than folded into StartTimeshiftBuffer() since
  // RefreshLiveManifest() below is the same kind of "POST an action, unwrap
  // the envelope" call against a differently-shaped response, so the split
  // still documents the shared pattern even with one caller of this exact
  // signature.
  // retryableOut (when non-null) is set from the plugin's own structured
  // "retryable" response field, the same convention RefreshLiveManifest()'s
  // own fatalOut already uses for "fatal" -- see StartTimeshiftBuffer()'s
  // own comment for why this exists (a caller opening a channel while its
  // buffer is mid-teardown gets this instead of a permanent failure).
  bool CallTimeshiftPluginAction(const std::string& action, const std::string& channelUuid, std::string& playlistUrlOut,
                                 std::string& error, const nlohmann::json& extraParams, bool* retryableOut = nullptr);

  Config m_config;
  // Guards only m_config.apiKey -- every other Config field is set once in
  // the constructor (from LoadConfigFromSettings()) and never written
  // again, so reading them elsewhere needs no synchronization; apiKey
  // alone can be rewritten later, at any time, by GenerateApiKey() (see
  // its own comment on why -- a self-healing regenerate-on-401, callable
  // from several different stream-opening code paths on whichever thread
  // happens to be using them). Mutable so the several const read sites
  // (GetApiKey()) can still lock it.
  mutable std::mutex m_apiKeyMutex;
  // Serializes ObtainApiKey() end to end (unlike m_apiKeyMutex, which only
  // guards the string itself and is never held across network I/O): the
  // in-progress-recording manifest refresh and the read thread can each hit
  // a 401 at the same time, and two unserialized recoveries could each
  // generate a key, leaving m_config.apiKey holding whichever finished
  // storing last while the server holds the other's. Lock order:
  // m_apiKeyRecoveryMutex -> m_authFlowMutex -> m_authStateMutex ->
  // m_apiKeyMutex, never the reverse (nothing holding either auth mutex
  // ever calls ObtainApiKey()).
  std::mutex m_apiKeyRecoveryMutex;
  // Two mutexes, so that a thread that only needs the current token is never held up behind a login or refresh
  // (docs/CLOSED_ITEMS.md, "m_authMutex held across a network round trip"; it was one recursive mutex held across
  // the whole HTTP call, so the live read's manifest refresh, a heartbeat or the realtime thread could wait a
  // full request timeout for another thread's login):
  //   - m_authStateMutex guards the token fields and the login-failure state below. Held only for short reads
  //     and writes, never across a network call or a call that locks the other.
  //   - m_authFlowMutex serializes the login/refresh flow itself and is held across its network calls, but only
  //     a thread that actually has to authenticate ever takes it: EnsureAuthenticated() returns on a valid cached
  //     token before reaching it, and looks again after getting it (another thread may just have succeeded, or
  //     failed and set a backoff that answers for this call too).
  // Lock order: m_authFlowMutex, then m_authStateMutex. Login() and RefreshAccessToken() require the flow mutex
  // to be held and take the state mutex only to write their result. Neither needs to be recursive: nothing
  // under the flow mutex re-enters EnsureAuthenticated() (they call Request() without auth and without retry).
  std::mutex m_authStateMutex;
  std::mutex m_authFlowMutex;
  std::string m_accessToken;
  std::string m_refreshToken;
  std::chrono::steady_clock::time_point m_accessTokenExpiry;
  // Login()-failure backoff state, also guarded by m_authStateMutex -- see
  // AuthBackoff.h's ComputeLoginBackoffSeconds() for why this exists.
  // Reset to 0/empty only by a successful Login() (EnsureAuthenticated()'s
  // own job); otherwise persists for the process lifetime, which is fine
  // since a credentials/settings fix needs a Kodi restart anyway.
  int m_consecutiveLoginFailures = 0;
  std::chrono::steady_clock::time_point m_loginBackoffUntil;
  std::string m_lastLoginError;
  // A short, fixed, non-escalating cooldown for a transient Login()
  // failure (added 2026-09-27, a 46th-pass audit, fixing a real,
  // confirmed regression in the 43rd-pass fix that added
  // dispatcharr::ShouldCountTowardLoginBackoff() -- see its own comment,
  // AuthBackoff.h, for the fix that made this necessary): that fix
  // correctly stopped a transport failure/5xx from escalating the same
  // exponential backoff a genuine credential rejection does, but left a
  // transient failure with *no* backoff at all -- m_consecutiveLoginFailures
  // stays 0, so the gate above never engages, so `EnsureAuthenticated()`
  // re-attempts `Login()` on every single call from every thread during
  // an extended network outage (a blackholed host, a firewall DROP
  // causing a slow TCP-level timeout rather than a fast refused/RST).
  // `Login()` ran under the one auth mutex that `EnsureAuthenticated()`/
  // `RefreshAccessToken()`/`Request()`'s own token-copy all used (now split, see m_authFlowMutex) -- for its
  // own full blocking HTTP call (up to `timeoutSeconds`, 30s default), so
  // this addon's own several concurrent callers (the background channel/
  // EPG thread, now waking every minute for a never-loaded state per the
  // 43rd-pass fix, the recording-refresh thread, the realtime-update
  // WebSocket reconnect loop, and Kodi's own per-channel
  // GetEPGForChannel() sweep, now retrying every 5 minutes per that same
  // fix) would each serialize behind, then repeat, a full ~30s blocking
  // login attempt for as long as the outage lasts -- exactly the
  // thundering-herd/serialization storm AuthBackoff.h's own original
  // comment already documents a real incident for for bad credentials,
  // just reopened here for a genuine outage instead. Set (not escalated)
  // on any Login() failure ShouldCountTowardLoginBackoff() says shouldn't
  // count toward the real backoff; checked the same way as
  // m_channelsLastFailedAt/dispatcharr::IsRetryDue() elsewhere in this
  // codebase.
  std::chrono::steady_clock::time_point m_transientLoginFailedAt;
  // Whether that cooldown was armed by an attempt that only waited the short bound
  // (dispatcharr::IsTransientCooldownBlocking()).
  bool m_transientLoginArmedByShortAttempt = false;
  // Matches ComputeLoginBackoffSeconds()'s own kInitialSeconds (AuthBackoff.cpp)
  // -- a transient failure gets the same first-failure wait a genuine
  // credential rejection does, it just never escalates beyond it.
  static constexpr int kTransientLoginRetrySeconds = 30;
  // Local IP curl reports (CURLINFO_LOCAL_IP) for the most recent
  // successful Request() -- i.e. the interface this machine actually
  // reaches Dispatcharr through. OpenLiveTimeshiftStream() passes this to
  // start_buffer so the timeshift plugin's ffmpeg connection (which
  // otherwise looks like it comes from Dispatcharr's own container, since
  // it runs server-side) can be attributed to the real viewing device via
  // an X-Forwarded-For-style header instead of showing 127.0.0.1.
  std::mutex m_lastLocalIpMutex;
  std::string m_lastLocalIp;

  // Opaque pointer to a small heap-allocated struct (CurlShareState, defined
  // in the .cpp) holding a CURLSH* and the mutexes that guard it. Every
  // curl_easy_init() this client does (Request(), the recording-stream
  // helpers) is still a fresh easy handle per call/open recording -- unlike
  // ReadRecordingStream's single reused CURL*, a lone shared easy handle
  // isn't safe here, since Kodi's PVR API can call into this client from
  // multiple threads at once (see the class comment above). A CURLSH share
  // object is libcurl's own answer to exactly that: a connection/DNS/
  // TLS-session cache safely shared across separate, concurrently-used easy
  // handles, as long as the application supplies lock/unlock callbacks
  // (libcurl doesn't lock it internally) -- see GetCurlShare() and the
  // constructor/destructor. Found necessary by a companion session's real
  // measurements: a single "Record" press fires several Request() calls in
  // a row (create, then a timer/recordings refresh), and on WiFi each one
  // independently exposed to a fresh-connection latency spike produced a
  // visible (1.8s-10s observed) delay before Kodi's own "recording started"
  // notification appeared, versus a low latency under calm conditions.
  void* m_curlShareState =
      nullptr; // DNS and TLS-session sharing only, never the connection cache (see the constructor)

  // Second CURLSH, used only by ProbeSegmentByteSize() when called from
  // RefreshInProgressRecordingManifest()'s concurrent probe fan-out (up to
  // 16 threads at once, all against the same host). Confirmed live on
  // a current macOS (real system libcurl -- crash report
  // Kodi-2026-01-01-000000.ips) that sharing CURL_LOCK_DATA_CONNECT across
  // that specific burst crashes inside that libcurl build's own
  // connection-cache return/close path, not this addon's lock/unlock
  // callbacks (which were already correctly handling the *other* kind of
  // concurrent access this client always allowed -- occasional background-
  // thread calls alongside active playback -- crash-free through extensive
  // live testing). Shares DNS and TLS-session (far more mature in
  // libcurl's share interface than connection sharing, and still a real
  // win for a same-host burst, especially TLS handshake avoidance over
  // HTTPS) but never touches the connection cache at all, sidestepping the
  // crash mechanism entirely rather than working around one specific
  // libcurl build/version.
  void* m_probeCurlShareState = nullptr;

  // Caches the last successful CreateCatchupSession() result so a
  // second call with the exact same (channelUuid, programmeStart,
  // durationMinutes) within a short window can reuse it instead of
  // creating (and immediately discarding) a redundant Dispatcharr
  // catch-up session -- see dispatcharr::ShouldReuseCachedCatchupSession()'s
  // own comment (CatchupSessionCache.h) for the real, live-confirmed bug
  // this fixes and why kMaxCatchupSessionCacheAge is well under
  // Dispatcharr's own handshake-expiry window. Guarded by its own mutex
  // rather than m_apiKeyMutex/the auth mutexes -- unrelated state, and
  // CreateCatchupSession() itself already calls EnsureAuthenticated()
  // (which takes the auth mutexes internally) before ever touching this, so
  // reusing either existing mutex here would risk a confusing,
  // unnecessary lock-ordering dependency between otherwise-unrelated
  // state for no real benefit.
  struct CatchupSessionCacheState
  {
    std::string channelUuid;
    time_t programmeStart = 0;
    int durationMinutes = 0;
    std::string playbackUrl;
    std::chrono::steady_clock::time_point cachedAt; // default-constructed == "never cached"
  };
  std::mutex m_catchupSessionCacheMutex;
  CatchupSessionCacheState m_catchupSessionCache;

  // Kodi only ever has one recording open for playback at a time.
  struct RecordingStreamState
  {
    bool open = false;
    std::string url; // final URL after following any redirect
    int64_t length = -1;
    int64_t position = 0;
    // Persistent libcurl easy handle, reused across every ReadRecordingStream()
    // call for the current open recording so HTTP keep-alive actually applies
    // across sequential range reads -- a fresh curl_easy_init()/cleanup() per
    // read meant a brand-new TCP connection (and TLS handshake, over HTTPS)
    // for every single demuxer read, which is negligible on a low-latency LAN
    // but confirmed (via a companion session's real WiFi measurements: a single
    // bulk connection vs. many small reads with a fresh connection each,
    // both against the same host) to starve playback on a
    // higher-latency/jittery link even with plenty of raw bandwidth for the
    // recording's bitrate. Created lazily on the first read, cleaned up in
    // CloseRecordingStream(). Stored as void* rather than CURL* so this
    // header doesn't need <curl/curl.h>; CURL is itself just an opaque alias
    // for void in curl.h, so the cast back in the .cpp is exact.
    void* curl = nullptr;
    // Set when a ranged read came back as a plain 200 -- the server (or a proxy
    // in front of it) ignores Range, so the bytes cannot be trusted at any offset
    // past 0. Every later read then ends the stream (EOF) without another request,
    // see ServerIgnoredRangeRequest().
    bool rangeIgnored = false;
    // Consecutive reads that failed for a reason that will not clear, see RecordPermanentReadFailure().
    PermanentReadFailureTracker permanentReadFailures;
  };
  RecordingStreamState m_recordingStream;
  // Counts a read of a completed recording that failed for a reason that will not clear: -1 until the run has
  // lasted long enough (RecordPermanentReadFailure()), then 0 (end of stream) from then on.
  int FailRecordingReadPermanently(long httpCode);

  struct LiveTimeshiftSegmentInfo
  {
    std::string filename;
    int64_t sequence = 0;   // HLS media-sequence-derived, stable across refetches
    int64_t byteOffset = 0; // in this stream's own fixed-origin address space
    int64_t byteSize = 0;
    int64_t timeOffsetMs = 0; // ditto, fixed-origin
  };

  // Only one live-timeshift stream open at a time, same as recordings.
  struct LiveTimeshiftStreamState
  {
    bool open = false;
    std::string channelUuid;
    // Generated fresh by OpenLiveTimeshiftStream() and sent as start_buffer's
    // viewer_id -- lets the plugin reference-count viewers of a shared
    // buffer (registers on start, deregisters on the matching
    // StopTimeshiftBuffer() at Close), so it can tell whether *this* viewer
    // was the last one before actually tearing anything down, instead of
    // either killing a buffer other viewers still need (the original
    // concurrent-viewer bug) or never proactively tearing one down at all
    // (which starves a provider's concurrent-stream limit -- see
    // docs/TIMESHIFT.md's "Concurrent viewers" section for both).
    std::string viewerId;
    // Per-buffer token the plugin's own file server requires on every
    // request (see plugin.py's _check_access_token) -- captured once from
    // StartTimeshiftBuffer()'s response (CallTimeshiftPluginAction()'s own
    // comment has the full mechanism) and reused for every later segment
    // fetch, since it doesn't change for the life of the buffer.
    std::string accessToken;
    std::string segmentBaseUrl; // "http://host:port/<uuid>/" -- filename appended per-request
    // Ordered by sequence, append-only for the life of this open stream --
    // byteOffset/timeOffsetMs are this stream's OWN fixed-origin addressing,
    // deliberately NOT the plugin response's own (relative-to-that-fetch)
    // offsets: the plugin's rolling window means "byte 0" in a fresh fetch
    // shifts to newer content over time, which would silently invalidate
    // any position already handed to Kodi's demuxer. See
    // RefreshLiveManifest()'s merge logic and get_live_manifest's own
    // docstring in plugin.py for why sequence is the stable join key.
    std::vector<LiveTimeshiftSegmentInfo> segments;
    // The lowest sequence the plugin's latest manifest still lists (-1 until
    // the first one arrives) -- everything in `segments` below it has been
    // rolled off server-side. See dispatcharr::FirstAvailableLiveSegmentIndex()
    // (LiveEdgeMargin.h) for what depends on it and why.
    int64_t oldestAvailableSequence = -1;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
    int64_t position = 0;
    // Real UTC wall-clock moment this session's local byte 0/PTS 0
    // corresponds to -- set once, right when OpenLiveTimeshiftStream()'s
    // own trim-to-live-edge-margin rebases the kept segments to local 0
    // (see that trim's own comment for why local 0 isn't the server-side
    // buffer's true beginning), and never touched again for the life of
    // this open stream. Approximate to within the few seconds the kept
    // margin segments span -- good enough for GetStreamTimes()'s startTime,
    // which only needs to be non-zero and roughly right, not
    // sub-segment-precise; see GetLiveTimeshiftStreamWallClockAnchor()'s
    // own comment for what actually depends on it being non-zero at all.
    time_t wallClockAnchor = 0;
    std::chrono::steady_clock::time_point lastManifestFetch{};
    // Set by SeekLiveTimeshiftStream() on every call. ReadLiveTimeshiftStream()
    // uses this to tell "this read is likely one of ffmpeg's own internal
    // seek probes" apart from "normal sequential playback that's caught up
    // to live" when deciding how long to wait for the tail to grow -- see
    // its own comment for why that distinction matters.
    std::chrono::steady_clock::time_point lastSeekTime{};
    // Position where a short (likely-probe) catch-up wait last gave up, so
    // a later read landing at that exact same position -- meaning it's not
    // a fresh probe candidate anymore, ffmpeg is genuinely stuck waiting
    // there -- escalates to the full segment-duration budget instead of
    // repeating the short one indefinitely. See ReadLiveTimeshiftStream()'s
    // own comment.
    int64_t lastShortGiveUpPosition = -1;
    // Where the current wait at the live tail began (see TailWaitEpisode, CatchUpUtil.h).
    dispatcharr::TailWaitEpisode tailWait;
    // Last time SendTimeshiftHeartbeat() was actually called for this
    // viewer, not the last manifest/segment fetch -- ReadLiveTimeshiftStream()
    // uses this to send one on an interval (kHeartbeatInterval, local to
    // that function) instead of on every single read.
    std::chrono::steady_clock::time_point lastHeartbeatSent{};
    void* curl = nullptr; // persistent handle, same rationale as RecordingStreamState::curl
    // Set once a manifest says the plugin's ffmpeg has exited (`ended`): the buffer is
    // frozen, so a read at the tail ends the stream instead of waiting, while a read
    // behind the tail still plays -- see IsAtEndedTail(). Sticky, unlike `fatal`, it never
    // blocks reading what is already known.
    bool ended = false;
    bool endedLogged = false; // the end-of-buffer message is logged once, not on every read
    // Set once RefreshLiveManifest() reports the plugin's own `fatal` flag
    // during a *steady-state* refresh (not the cold-start one OpenLiveTimeshiftStream()
    // already handles) -- confirmed this buffer will never produce another
    // segment, most commonly the underlying ffmpeg process having died or
    // lost its upstream connection sometime after playback was already
    // under way. Before this existed, ReadLiveTimeshiftStream()'s catch-up
    // loop had no way to tell that apart from an ordinary "just waiting for
    // the next segment" gap, so it retried forever, every single Read()
    // call, silently returning 0 with nothing worse than a debug log line --
    // to a user, playback just froze indefinitely with no explanation.
    // Checked up front so a confirmed-dead buffer short-circuits
    // immediately, without paying for another doomed network round trip.
    bool fatal = false;
    // This Open() session's own running count of segment-body fetches that
    // have failed with an HTTP status other than 200/206/404 in a row --
    // reset to 0 by any 200/206/404 response. See
    // dispatcharr::ShouldGiveUpAfterSegmentFetchFailure()'s own comment
    // (SegmentFetchFailure.h) for the real, live-confirmed request-storm
    // bug this bounds: without a cap, a segment fetch failing for a reason
    // RefreshLiveManifest() itself can't positively confirm as fatal (a
    // stale access_token against a buffer that silently restarted with a
    // fresh one is the known case; see that header's own comment) retried
    // forever with no backoff.
    int consecutiveSegmentFetchFailures = 0;
    // When the current streak of those failures began (value-initialized when there is none), so a give-up needs the
    // streak to have lasted -- see dispatcharr::ShouldGiveUpAfterSegmentFetchFailure().
    std::chrono::steady_clock::time_point firstSegmentFetchFailureAt{};
  };
  LiveTimeshiftStreamState m_liveTimeshiftStream;

  // Locking for the live-timeshift state above (docs/CLOSED_ITEMS.md, "No locking around live-timeshift /
  // in-progress stream state"). Kodi may call GetStreamTimes()/the length getters (which refresh the manifest)
  // on a different thread from Read/Seek/Open/Close, so:
  //   - m_liveStateMutex guards every field of m_liveTimeshiftStream and m_liveSession. It is held only for
  //     short reads and writes of that state, never across a network call, a sleep, or a call into another
  //     function that locks it (it is not recursive).
  //   - m_liveRefreshMutex makes RefreshLiveManifest() one-at-a-time: a refresh that finds one already running
  //     returns at once unless forced, in which case it waits its turn. Without it two refreshes merged the same
  //     new segments twice.
  //   - m_liveCurlMutex is held while the persistent segment-fetch handle (`curl`) is in use or being freed, so
  //     Close cannot free it under a transfer.
  //   - m_liveSession changes on every open and close. Anything that drops m_liveStateMutex around a network
  //     call remembers the session first and discards its result if it moved, so a result for a closed stream
  //     never lands in the next one.
  // Lock order, outermost first: m_liveCurlMutex or m_liveRefreshMutex, then m_liveStateMutex.
  mutable std::mutex m_liveStateMutex;
  std::mutex m_liveRefreshMutex;
  std::mutex m_liveCurlMutex;
  uint64_t m_liveSession = 0;

  // Fetches the plugin's get_live_manifest action and merges any segments
  // not already known into m_liveTimeshiftStream, extending its fixed-origin
  // address space -- called on open, and again whenever a read/seek/length
  // call needs to know about content newer than what's already known. Not
  // merely a cache refresh: an unconditional replace would shift byte 0 out
  // from under a position already handed to Kodi's demuxer. `force` bypasses
  // the small throttle (see the .cpp) that keeps a tight demux-read loop
  // from re-fetching the manifest on every single call. `fatalOut`, if
  // non-null, is set true when the plugin reports the buffer will never
  // succeed (ffmpeg already exited -- see plugin.py's own
  // BufferFailedError) rather than just "not ready yet" -- only
  // OpenLiveTimeshiftStream()'s cold-start retry loop passes one, to stop
  // retrying immediately instead of waiting out its full budget against
  // something that can't recover on its own.
  // `coldStart` (OpenLiveTimeshiftStream()'s own wait for the first segment) uses the configured request timeout
  // instead of the short bound steady-state refreshes get: Open is allowed to take a full timeout, and with the
  // short bound an API that merely answers slowly (6 s per request) could never start a stream -- live check,
  // docs/CLOSED_ITEMS.md's "The unresponsive-server timeout changes".
  bool RefreshLiveManifest(bool force, std::string& error, bool* fatalOut = nullptr, bool coldStart = false);

  // Shared give-up-or-retry handling for ReadLiveTimeshiftStream()'s
  // segment-body fetch, called after a failure (a transport-level curl
  // error, or an HTTP status other than 200/206/404) the caller has
  // already logged its own failure-specific detail for. See
  // dispatcharr::ShouldGiveUpAfterSegmentFetchFailure()'s own comment
  // (SegmentFetchFailure.h) for the full incident and design -- this owns
  // the RefreshLiveManifest() call and the bounded-retry-vs-fatal
  // decision, returning the value ReadLiveTimeshiftStream() itself should
  // return (kRetrySegmentFetch to retry within the same call, -1 once given up and
  // m_liveTimeshiftStream.fatal is set).
  int HandleLiveTimeshiftSegmentFetchFailure();
  // One pass of ReadLiveTimeshiftStream(); returns kRetrySegmentFetch when a segment fetch failed and the
  // caller should try again.
  int ReadLiveTimeshiftStreamOnce(uint8_t* buffer, unsigned int size);
  static constexpr int kRetrySegmentFetch = -2;

  struct InProgressRecordingSegmentInfo
  {
    std::string url;        // absolute, already resolved against the playlist's own baseDir
    int64_t byteOffset = 0; // in this stream's own fixed-origin address space
    int64_t byteSize = 0;
    int64_t timeOffsetMs = 0; // ditto, fixed-origin
  };

  // Only one in-progress-recording stream open at a time, same as
  // completed recordings and the live-timeshift buffer.
  struct InProgressRecordingStreamState
  {
    bool open = false;
    int recordingId = -1;
    // Real UTC wall-clock start time of the underlying recording -- see
    // GetInProgressRecordingStreamStartTime()'s own comment.
    time_t startTime = 0;
    // Append-only for the life of this open stream: Dispatcharr's own HLS
    // output for a recording, unlike the live-timeshift plugin's rolling
    // buffer, never evicts old segments (a recording is meant to be kept
    // in full), so there's no rolling-window/sequence-number complication
    // to handle here -- "already have N segments, only look at any past
    // that" is enough.
    std::vector<InProgressRecordingSegmentInfo> segments;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
    int64_t position = 0;
    // Set once Dispatcharr reports this recording as no longer in
    // progress (checked on every manifest refresh) -- lets
    // ReadInProgressRecordingStream() treat "caught up to the known tail"
    // as genuine EOF instead of polling for more that will never come.
    bool finished = false;
    // When `finished` was first held back for want of an #EXT-X-ENDLIST --
    // see dispatcharr::GateFinishedOnEndList().
    std::optional<std::chrono::steady_clock::time_point> endListWaitingSince;
    std::chrono::steady_clock::time_point lastManifestFetch{};
    // The leading unmerged segment that could not be sized, and for how long -- see
    // UnprobeableSegment.h.
    dispatcharr::UnprobeableSegmentTracker unprobeableSegment;
    // Same seek-probe-vs-real-catch-up distinction as
    // LiveTimeshiftStreamState -- see ReadLiveTimeshiftStream()'s comment
    // for why this matters; the same generic Kodi/ffmpeg seek-probing
    // behaviour applies here too, since this uses the same native-demuxer
    // mechanism.
    std::chrono::steady_clock::time_point lastSeekTime{};
    int64_t lastShortGiveUpPosition = -1;
    void* curl = nullptr; // persistent handle, same rationale as RecordingStreamState::curl
    // Whole-segment cache for ReadInProgressRecordingStream() -- required,
    // not just an optimisation: unlike the completed-recording `/file/`
    // endpoint, Dispatcharr's in-progress-recording HLS segment endpoint
    // ignores the Range header entirely and always serves the full segment
    // body from its own byte 0 regardless of what was requested (confirmed
    // live -- see ProbeSegmentByteSize()'s own comment for the same finding
    // against a HEAD/ranged-GET probe). A per-read ranged GET against that
    // endpoint would therefore silently hand back the segment's own leading
    // bytes on every read past the first, corrupting the reconstructed
    // stream from the second read of each segment onward -- confirmed live
    // by a companion session as continuous H.264 decode errors and growing
    // audio desync from the very start of playback. Fetching each segment's
    // full body exactly once and serving every read against it from memory
    // sidesteps the server's lack of Range support entirely, the same way
    // the HEAD-based size probe does for sizing. cachedSegmentByteOffset is
    // that segment's own byteOffset (its address in this stream's
    // fixed-origin space), -1 when nothing is cached; only ever holds the
    // one segment current reads are landing in, evicted (replaced) the
    // moment position moves to a different segment.
    std::vector<uint8_t> cachedSegmentBytes;
    int64_t cachedSegmentByteOffset = -1;
    // When MaybeSendInProgressHlsKeepAlive() next owes the server a request
    // to keep this recording's HLS directory alive -- see
    // HlsViewerKeepAlive.h for why a paused viewer needs one. Pushed out
    // by every request that itself lands on a `.ts` URL (segment fetches,
    // size probes), so a playing stream never sends any. Zero means due.
    std::chrono::steady_clock::time_point nextHlsKeepAliveAt{};
    // Set once a keep-alive came back 404/3xx (the directory is already
    // gone), so a paused viewer doesn't keep asking every retry interval
    // for something that can't come back.
    bool hlsKeepAliveGone = false;
    // The server has positively confirmed this recording's HLS content is
    // permanently gone (deleted outright, or finished with its directory
    // already removed) -- see dispatcharr::IsInProgressContentGone().
    // Sticky, unlike `finished`: nothing that is gone comes back, so every
    // later refresh returns immediately without touching the network and a
    // read needing a segment it doesn't already hold reports EOF instead
    // of asking a server that will only answer 404.
    bool contentGone = false;
    // Consecutive segment reads that failed for a reason that will not clear, see RecordPermanentReadFailure().
    PermanentReadFailureTracker permanentReadFailures;
  };
  InProgressRecordingStreamState m_inProgressRecordingStream;

  // The same rules as the live-timeshift state's (see the block at m_liveStateMutex). m_inProgressStateMutex
  // guards every field of m_inProgressRecordingStream and m_inProgressSession. m_inProgressCurlMutex is held
  // for a whole segment fetch and the copy out of `cachedSegmentBytes` that follows it, so Close, which takes it
  // before resetting the state, never frees the handle or the cached bytes under a read.
  mutable std::mutex m_inProgressStateMutex;
  std::mutex m_inProgressRefreshMutex;
  std::mutex m_inProgressCurlMutex;
  uint64_t m_inProgressSession = 0;

  // Cross-open cache of already-probed segments for an in-progress
  // recording, keyed by recordingId. Dispatcharr's in-progress HLS output
  // is append-only (see InProgressRecordingStreamState::segments' own
  // comment) -- a segment probed on one open is still valid, at the same
  // byte offset, on the next -- so without this, reopening the same
  // still-recording (e.g. after a channel switch, or resuming after
  // pausing playback in the Kodi UI) would otherwise re-probe every single
  // already-known segment from scratch every time -- on top of the cost
  // every *first* open already pays for a recording that's been running a
  // while, which was confirmed live (a ~2h-in recording took 29.4s to
  // open, entirely spent probing 1,800 already-elapsed segments one at a
  // time). Cleared for a recording once RefreshInProgressRecordingManifest()
  // sees it's finished -- a finished recording is played back through the
  // completed-recording path instead, so its entry here would just sit
  // unused.
  struct InProgressRecordingSegmentCache
  {
    std::vector<InProgressRecordingSegmentInfo> segments;
    int64_t totalBytes = 0;
    int64_t totalDurationMs = 0;
    // Set every time this entry is touched -- backs the opportunistic
    // TTL-based prune RefreshInProgressRecordingManifest() runs alongside
    // its own normal cache-touch. Real, confirmed leak this fixes (found
    // via code reading): the "finished" cleanup above only ever runs if
    // this exact recording's stream is reopened again after it finishes
    // -- a recording watched partway while in progress, then abandoned
    // (channel changed, never tuned back in) before it naturally
    // completes, otherwise leaked its entry for the rest of this
    // DispatcharrClient instance's lifetime (a completed recording is
    // watched through OpenRecordingStream() instead, which never looks
    // at this map at all).
    std::chrono::steady_clock::time_point lastUpdated{};
  };
  std::map<int, InProgressRecordingSegmentCache> m_inProgressSegmentCache;
  std::mutex m_inProgressSegmentCacheMutex;

  // Fetches the recording's current HLS playlist (FetchRawInProgressPlaylist)
  // and merges any segments not already known into
  // m_inProgressRecordingStream, extending its fixed-origin address space,
  // probing each newly-discovered segment's byte size with a tiny HEAD
  // request (HLS playlists carry durations via #EXTINF, never byte sizes) --
  // same pattern as RefreshLiveManifest(), adapted for a plain HLS text
  // response instead of the timeshift plugin's own JSON manifest action.
  // `force` bypasses the small throttle that keeps a tight demux-read loop
  // from re-fetching on every single call. `coldStartRetryableOut`, if
  // given, is set true when a failed refresh is worth waiting out rather
  // than giving up on -- see dispatcharr::ShouldRetryInProgressColdStart()
  // (RecordingVisibility.h); only OpenInProgressRecordingStream()'s
  // cold-start loop reads it.
  // `coldStart` is set by OpenInProgressRecordingStream()'s own loop: an open keeps the configured timeout even when
  // it was seeded with segments from a previous open of the same recording (dispatcharr::IsInProgressSteadyState()).
  bool RefreshInProgressRecordingManifest(bool force, std::string& error, bool* coldStartRetryableOut = nullptr,
                                          bool coldStart = false);

  // Sends the periodic HEAD on the newest known segment that keeps
  // Dispatcharr from removing a finished recording's HLS directory out from
  // under a viewer that is paused with segments still unread -- see
  // HlsViewerKeepAlive.h for the full story, including why it deliberately
  // stops once the reader has caught up to the tail. Cheap no-op unless one
  // is actually due; called from RefreshInProgressRecordingManifest(),
  // which GetStreamTimes()'s polling keeps reaching through a pause.
  void MaybeSendInProgressHlsKeepAlive();

  // A tiny HEAD request to learn one segment's total byte size via
  // Content-Length -- HLS playlists carry each segment's duration
  // (#EXTINF) but never its size. Not a ranged GET: Dispatcharr's
  // in-progress-recording HLS segment endpoint ignores Range entirely and
  // always serves the full body with a 200 (see ContentLengthHeaderCallback's
  // own comment), so this reads Content-Length off a HEAD instead. Returns
  // -1 on any failure (network error, non-200, or no parseable
  // Content-Length); the caller skips a segment it can't size rather than
  // corrupting the cumulative offsets that follow.
  // `timeoutMsOverride` > 0 bounds the HEAD (a steady-state refresh, see dispatcharr::IsInProgressSteadyState(): up to
  // 16 of them run at once, and each would otherwise wait the whole connection timeout on a server that has stopped
  // answering).
  int64_t ProbeSegmentByteSize(const std::string& segmentUrl, long timeoutMsOverride = 0) const;

  // Client-side placeholder for a just-created one-time recording's title,
  // matched by the real Dispatcharr recording id (parsed from
  // CreateOneTimeRecording()'s own POST response) to whatever this addon
  // was called with. Dispatcharr only learns a recording's real title
  // asynchronously (custom_properties.program.title, populated a moment
  // after the recording actually starts, see GetRecordings()), but Kodi
  // already told AddTimer() the correct EPG-derived title *before* this
  // client ever calls Dispatcharr -- CreateFromEpg() reads it from the EPG
  // tag the user clicked "Record" on. Caching that and using it in
  // GetRecordings() in place of the "Recording <id>" fallback means the
  // correct title shows immediately, without needing to wait for
  // Dispatcharr's enrichment or a later refresh to catch up at all, for
  // the common EPG-matched case.
  //
  // Keyed by recordingId, not channelId (changed 2026-09-26, a 32nd-pass
  // audit, fixing a real, confirmed bug flagged but not fixed by an
  // earlier, 25th-pass audit, not itself independently reproduced): the
  // start-time-clamping case above (Dispatcharr silently clamps an
  // already-airing recording's own stored start_time to the moment it
  // actually began, not the EPG programme's own start time this addon
  // sent) is why this was never matched on start time either, but
  // matching by channel ALONE went further than that clamp required --
  // it let ANY other untitled recording on the same channel (an old
  // completed "Custom Recording" that never gets a title from Dispatcharr
  // at all, or an unrelated future scheduled timer) borrow this one's
  // in-flight title for the rest of the cache entry's TTL, not just the
  // specific recording this addon actually just created. See
  // dispatcharr::FindPendingTitleForRecording()'s own comment
  // (PendingTitleLookup.h) for the real corruption path that opened.
  // Entries expire after a few minutes regardless
  // (pruned in GetRecordings()) since Dispatcharr's own enrichment should
  // have long since caught up by then, and to avoid an unbounded cache.
  struct PendingTitle
  {
    int recordingId = 0;
    std::string title;
    std::chrono::steady_clock::time_point insertedAt;
  };
  std::mutex m_pendingTitlesMutex;
  std::vector<PendingTitle> m_pendingTitles;

  // Serializes SetDvrOffsetMinutes()'s own GET-merge-PATCH -- fix for a
  // real, confirmed regression found via a project-wide review (a
  // 21st-pass audit), not itself independently reproduced: a settings
  // dialog save that changes BOTH recording_pre_offset_minutes and
  // recording_post_offset_minutes in one go delivers two
  // OnAddonSettingChanged() notifications back to back (confirmed
  // against Kodi's own source: CAddonDll::SaveSettings()'s own
  // TransferSettings() re-delivers every changed setting synchronously,
  // in settings.xml order), each spawning its own detached
  // SetDvrOffsetMinutes() call. Before this addon's own nullable-pointer
  // fix (see MergeDvrOffsetMinutes()'s own comment, JsonFieldUtil.h),
  // both concurrent calls pushed the *same* two values (both read from
  // Kodi's own already-saved local settings), so the race was harmless
  // regardless of interleaving. That fix made each call push only the
  // one offset it's actually responsible for, relying on its own fresh
  // GET to preserve the other -- but two such calls running concurrently
  // can each GET the row *before* either one's PATCH lands, so the
  // second PATCH's own "preserve the other key" merge is built from a
  // value that's already stale by the time it's sent, silently losing
  // the first call's own edit. Without serializing the two, a single
  // settings-dialog save that changes both offsets at once can leave
  // Dispatcharr with only one of the two edits actually applied, while
  // Kodi's own local settings show both as changed successfully.
  //
  // Also guards CreateSeriesRule()/DeleteSeriesRule() (added 2026-09-26,
  // a 27th-pass audit) -- see either one's own comment: Dispatcharr
  // stores its own series-rule list inside this exact same shared
  // dvr_settings row, reached through a different, non-merging write
  // path than the padding PATCH above, so the two need to be serialized
  // from this addon's own side to avoid one undoing the other.
  std::mutex m_dvrSettingsMutex;
};

} // namespace dispatcharr
