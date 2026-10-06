#include "DispatcharrClient.h"

#include "ApiKeyRecovery.h"
#include "AuthBackoff.h"
#include "CatchUpUtil.h"
#include "CatchupSessionCache.h"
#include "CatchupSessionRequest.h"
#include "ChannelParser.h"
#include "CurlCallbacks.h"
#include "DateTimeFormat.h"
#include "HlsViewerKeepAlive.h"
#include "InProgressSegmentCache.h"
#include "UnprobeableSegment.h"
#include "BoundedParallel.h"
#include "JsonFieldUtil.h"
#include "JsonResponse.h"
#include "LiveEdgeMargin.h"
#include "LiveManifestParser.h"
#include "M3u8SegmentParser.h"
#include "PendingTitleLookup.h"
#include "PluginRunResult.h"
#include "PluginUrlUtil.h"
#include "RecordingHttpUtil.h"
#include "RecordingParser.h"
#include "RecordingVisibility.h"
#include "RedirectPolicy.h"
#include "SegmentAppendOffsets.h"
#include "SegmentFetchFailure.h"
#include "SegmentLookup.h"
#include "Staleness.h"
#include "StreamSeek.h"
#include "StringUtil.h"
#include "TimeUtil.h"
#include "TimerRequestBuilder.h"
#include "TimerRuleParser.h"
#include "TimeZoneUtil.h"

#include <curl/curl.h>
#include <kodi/General.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <new>
#include <thread>

using json = nlohmann::json;

namespace dispatcharr
{

namespace
{

// ---------------------------------------------------------------------
// NOTE ON ASSUMED ENDPOINTS/FIELDS
// Paths marked "assumed" below follow standard Django REST Framework
// ModelViewSet conventions (matching the confirmed /api/channels/channels/
// and /api/channels/streams/ routes) but were not individually confirmed
// against a live Dispatcharr Swagger document. Check them against
// http://<your-server>:9191/swagger/ and adjust here if they've drifted.
// ---------------------------------------------------------------------
constexpr const char* kTokenPath = "/api/accounts/token/";
constexpr const char* kTokenRefreshPath = "/api/accounts/token/refresh/";
constexpr const char* kChannelsPath = "/api/channels/channels/";
// Confirmed against a live instance's own OpenAPI schema (GET /api/schema/).
// The old primary guess, /api/channels/channel-groups/, doesn't exist at all
// -- Dispatcharr's SPA catches unmatched routes and serves index.html for it,
// which returned HTTP 200 but wasn't JSON, so it always failed to parse.
constexpr const char* kChannelGroupsPath = "/api/channels/groups/";
constexpr const char* kChannelGroupsPathFallback =
    "/api/channels/channel-groups/"; // pre-confirmation guess, kept as a fallback in case older Dispatcharr versions
                                     // differ
constexpr const char* kEpgOutputPath = "/output/epg";
// Both confirmed against a live instance's own OpenAPI schema -- see the
// endpoint/payload notes in DispatcharrClient.h.
constexpr const char* kRecordingsPath = "/api/channels/recordings/";
constexpr const char* kSeriesRulesPath = "/api/channels/series-rules/";
// Confirmed against the live router: apps/epg/api_urls.py registers this
// viewset as "epgdata" (not "epg-data" or "epg/data/"), a real REST route
// with a path-addressable id -- unlike series rules, which have none.
constexpr const char* kEpgDataPath = "/api/epg/epgdata/";
constexpr const char* kRecurringRulesPath = "/api/channels/recurring-rules/";
constexpr const char* kCoreSettingsPath = "/api/core/settings/";
constexpr const char* kVersionPath = "/api/core/version/";
constexpr const char* kTimezonesPath = "/api/core/timezones/";
constexpr const char* kCurrentUserPath = "/api/accounts/users/me/";
// Confirmed against a live instance: every CoreSettings "group" (system
// timezone, DVR padding/comskip/path-templates, proxy tuning, ...) is one
// row in this generic key/value table, addressed by its own numeric id
// (not by key directly) -- GetDvrOffsetMinutes()/SetDvrOffsetMinutes()
// find the row with this key by listing and filtering, not by assuming a
// fixed id (that id is a plain auto-increment DB primary key and isn't
// guaranteed the same across different Dispatcharr installs).
constexpr const char* kDvrSettingsKey = "dvr_settings";
constexpr const char* kSystemSettingsKey = "system_settings";
constexpr const char* kLogosPath = "/api/channels/logos/";
// Confirmed against a live instance: creates a session-bound catch-up
// (archived programme) playback URL that stays valid via a sliding idle
// window for as long as it's actively used, rather than embedding a
// short-lived JWT directly in the stream URL.
constexpr const char* kCatchupSessionsPath = "/api/catchup/sessions/";
// Well under Dispatcharr's own catch-up-session handshake expiry
// (HANDSHAKE_TTL_SECONDS = 60s, apps/timeshift/sessions.py) -- see
// dispatcharr::ShouldReuseCachedCatchupSession()'s own comment
// (CatchupSessionCache.h) for what this exists to avoid.
constexpr std::chrono::seconds kMaxCatchupSessionCacheAge{30};
constexpr const char* kApiKeyGeneratePath = "/api/accounts/api-keys/generate/";
// GET on this same route returns the caller's own current key without
// touching it (APIKeyViewSet.list(), apps/accounts/api_views.py) --
// unlike POST .../generate/ above, which always replaces it.
constexpr const char* kApiKeyListPath = "/api/accounts/api-keys/";
// Fixed to this addon's own companion Dispatcharr plugin (see
// dispatcharr-plugin/timeshift_buffer/ in this repo) -- "timeshift_buffer"
// is that plugin's directory name, which Dispatcharr's loader uses
// verbatim as its registry key. Confirmed against Dispatcharr's own source
// (apps/plugins/api_urls.py) that the generic run endpoint takes the
// plugin key as a URL segment, not a request body field.
constexpr const char* kTimeshiftPluginRunPath = "/api/plugins/plugins/timeshift_buffer/run/";
// Same mechanism, this addon's other companion plugin (see
// dispatcharr-plugin/recording_edl/ in this repo).
constexpr const char* kRecordingEdlPluginRunPath = "/api/plugins/plugins/recording_edl/run/";

// Unique-enough per-Open()-session id for the plugin's viewer reference
// counting (see LiveTimeshiftStreamState::viewerId's own comment) -- only
// needs to not collide between viewers concurrently watching the same
// channel, not to be cryptographically unguessable, so a random 64-bit
// value hex-encoded is plenty.
std::string GenerateViewerId()
{
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist;
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(dist(rng)));
  return std::string(buf);
}

// Backs DispatcharrClient::m_curlShareState. A CURLSH connection/DNS/
// TLS-session cache shared across every easy handle this client creates,
// so short-lived per-call handles (Request(), the recording-stream probe)
// still get keep-alive/connection reuse -- unlike ReadRecordingStream's
// single reused CURL*, a lone shared easy handle isn't safe here since
// Kodi's PVR API can call into this client from multiple threads at once.
// libcurl doesn't lock a share object internally; these mutexes back the
// lock/unlock callbacks below, which is the standard, documented pattern
// for using one across threads (CURLSHOPT_LOCKFUNC/UNLOCKFUNC).
constexpr int kCurlShareLockCount = 8; // headroom above CURL_LOCK_DATA_LAST

struct CurlShareState
{
  CURLSH* handle = nullptr;
  std::array<std::mutex, kCurlShareLockCount> locks;
};

void CurlShareLock(CURL*, curl_lock_data data, curl_lock_access, void* userptr)
{
  auto* state = static_cast<CurlShareState*>(userptr);
  int index = static_cast<int>(data);
  if (index >= 0 && index < kCurlShareLockCount)
    state->locks[index].lock();
}

void CurlShareUnlock(CURL*, curl_lock_data data, void* userptr)
{
  auto* state = static_cast<CurlShareState*>(userptr);
  int index = static_cast<int>(data);
  if (index >= 0 && index < kCurlShareLockCount)
    state->locks[index].unlock();
}

CurlShareState* MakeCurlShareState(bool shareConnections)
{
  auto* state = new CurlShareState();
  state->handle = curl_share_init();
  if (state->handle)
  {
    curl_share_setopt(state->handle, CURLSHOPT_LOCKFUNC, CurlShareLock);
    curl_share_setopt(state->handle, CURLSHOPT_UNLOCKFUNC, CurlShareUnlock);
    curl_share_setopt(state->handle, CURLSHOPT_USERDATA, state);
    if (shareConnections)
      curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(state->handle, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  }
  return state;
}

void FreeCurlShareState(CurlShareState* state)
{
  if (state)
  {
    if (state->handle)
      curl_share_cleanup(state->handle);
    delete state;
  }
}

} // namespace

DispatcharrClient::DispatcharrClient(Config config) : m_config(std::move(config))
{
  // The connection cache is NOT shared (found by the thirteenth hardening sweep, 2026-10-05): sharing
  // CURL_LOCK_DATA_CONNECT across threads crashes inside libcurl's own connection-cache code. It was
  // reproduced on the stock Ubuntu 24.04 libcurl 8.5.0 (a SEGV in curl_easy_perform) both through the
  // real DispatcharrClient driven by 16 threads against a local fake server (4 of 5 runs; 0 of 6 with
  // this flag off) and by a 30-line plain-C program with no addon code (5 of 5 at 8 threads, none at 3, 4
  // and 6), so it is the same libcurl defect the probe share below sidesteps on macOS, not a macOS
  // peculiarity, and the addon can have 5 to 7 transfers in flight at once (the channel/EPG thread, the
  // recording refresh, the realtime reconnect refresh, the stream-read thread with its heartbeat and
  // manifest refreshes, GetStreamTimes() polling, the detached padding and AddTimer threads, and Kodi's
  // own timer calls). DNS and TLS-session sharing stay, and a persistent read handle keeps its own
  // keep-alive connection; what is lost is reusing one short request's connection for the next.
  m_curlShareState = MakeCurlShareState(/*shareConnections=*/false);
  // A second, separate share for ProbeSegmentByteSize(), kept as it was: confirmed live on a current macOS (real system
  // libcurl, crash report Kodi-2026-01-01-000000.ips) that sharing CURL_LOCK_DATA_CONNECT across
  // ProbeSegmentByteSize()'s concurrent probe burst (up to 16 threads at once, all against the same host, all calling
  // curl_easy_cleanup() within milliseconds of each other) crashes inside
  // that libcurl build's own connection-cache return/close path
  // (Curl_conncache_return_conn) -- a real bug in that specific libcurl,
  // not a gap in this addon's own lock/unlock callbacks (which are
  // correctly implemented and had already been running the *other*,
  // non-bursty concurrent access this client always allowed -- background
  // refresh threads alongside active playback -- crash-free through
  // extensive live testing). DNS and TLS-session sharing are far more
  // mature/battle-tested in libcurl's share interface than connection
  // sharing, so this second share keeps those (still meaningfully faster
  // for a same-host probe burst, especially the TLS handshake avoidance
  // over HTTPS) while never touching the connection cache at all --
  // sidestepping the crash mechanism entirely rather than working around
  // a specific libcurl version/platform.
  m_probeCurlShareState = MakeCurlShareState(/*shareConnections=*/false);
}

DispatcharrClient::~DispatcharrClient()
{
  // The persistent stream handles first: a handle still attached to a share when the share is cleaned up leaves
  // curl_share_cleanup() failing with CURLSHE_IN_USE, so both the handle and the share leaked. Only freed here, no
  // network (the stop request of a live Close is the caller's to make, and ~PVRDispatcharr() aborts what is in flight).
  CloseRecordingStream();
  {
    std::lock_guard<std::mutex> curlLock(m_liveCurlMutex);
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    if (m_liveTimeshiftStream.curl)
      curl_easy_cleanup(static_cast<CURL*>(m_liveTimeshiftStream.curl));
    m_liveTimeshiftStream.curl = nullptr;
  }
  CloseInProgressRecordingStream();
  FreeCurlShareState(static_cast<CurlShareState*>(m_curlShareState));
  FreeCurlShareState(static_cast<CurlShareState*>(m_probeCurlShareState));
}

void* DispatcharrClient::GetCurlShare() const
{
  auto* state = static_cast<CurlShareState*>(m_curlShareState);
  return state ? state->handle : nullptr;
}

void* DispatcharrClient::GetProbeCurlShare() const
{
  auto* state = static_cast<CurlShareState*>(m_probeCurlShareState);
  return state ? state->handle : nullptr;
}

std::string DispatcharrClient::BaseUrl() const
{
  std::string scheme = m_config.useHttps ? "https://" : "http://";
  return scheme + FormatHostForUrl(m_config.host) + ":" + std::to_string(m_config.port);
}

bool DispatcharrClient::IsOnConfiguredServer(const std::string& url) const
{
  return IsSameOrigin(url, BaseUrl());
}

int DispatcharrClient::PerformWithSafeRedirects(void* curlPtr, const std::string& startUrl,
                                                const std::function<void()>& onRedirect, bool switchPostToGet)
{
  CURL* curl = static_cast<CURL*>(curlPtr);
  // Followed here rather than by CURLOPT_FOLLOWLOCATION so that each hop can be
  // checked against dispatcharr::IsSafeRedirectTarget() first: this is only
  // used for requests carrying credentials (an X-API-Key header, a bearer
  // token, a login body), which libcurl would otherwise send to whatever host
  // and scheme a redirect named -- see that function's own comment.
  constexpr int kMaxRedirects = 5;
  std::string current = startUrl;
  for (int hop = 0;; ++hop)
  {
    curl_easy_setopt(curl, CURLOPT_URL, current.c_str());
    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
      return static_cast<int>(res);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    // Which statuses are redirects, when the chain is too long, and when a POST
    // is repeated as a GET: dispatcharr::DecideRedirectFollow() (RedirectPolicy.h,
    // tested). The Location and same-host checks sit between its outcomes.
    const RedirectFollowAction action = DecideRedirectFollow(code, hop, kMaxRedirects, switchPostToGet);
    if (action == RedirectFollowAction::kNotARedirect)
      return static_cast<int>(res);
    char* location = nullptr;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    if (!location || !*location)
      return static_cast<int>(res);
    std::string next = location;
    if (!IsSafeRedirectTarget(current, next))
    {
      // Not followed: the caller sees the 3xx itself, which every call site
      // already treats as a failure. Only hosts are logged -- a redirect's
      // query string can carry a token.
      const UrlOrigin from = ParseUrlOrigin(current);
      const UrlOrigin to = ParseUrlOrigin(next);
      kodi::Log(ADDON_LOG_WARNING,
                "pvr.dispatcharr-unofficial: refused to follow a redirect (HTTP %ld) from %s://%s to %s://%s -- "
                "credentials only follow a redirect to the same host, never to another host or from https to http",
                code, from.scheme.c_str(), from.host.c_str(), to.valid ? to.scheme.c_str() : "?",
                to.valid ? to.host.c_str() : "(not an absolute http(s) URL)");
      return static_cast<int>(res);
    }
    if (action == RedirectFollowAction::kTooManyRedirects)
      return static_cast<int>(CURLE_TOO_MANY_REDIRECTS);
    if (action == RedirectFollowAction::kFollowAsGet)
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    if (onRedirect)
      onRedirect();
    current = std::move(next);
  }
}

bool DispatcharrClient::Request(const std::string& method, const std::string& path, const json& body, json& responseOut,
                                std::string& error, bool withAuth, int retryOnAuthFailure, long* httpStatusOut,
                                long timeoutMsOverride)
{
  // Serialized before curl_easy_init(), and through TrySerializeJsonBody()
  // rather than a plain body.dump() -- see that function's own comment
  // (JsonFieldUtil.h) for the real std::terminate crash path a plain
  // dump() left open here.
  std::string bodyStr;
  if (!body.is_null() && !TrySerializeJsonBody(body, bodyStr, error))
    return false;

  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }

  std::string url = BaseUrl() + path;
  std::string responseBody;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  std::string authHeader;
  // The token this request is about to be sent with, kept so a 401 below can
  // tell "the token I used was rejected" from "another thread already
  // replaced it" -- see InvalidateAccessTokenIfCurrent().
  std::string usedToken;
  if (withAuth)
  {
    // Copy under the lock rather than reading m_accessToken directly here
    // -- this can run concurrently with Login()/RefreshAccessToken()
    // writing it from another thread (see m_authStateMutex's own comment).
    {
      std::lock_guard<std::mutex> lock(m_authStateMutex);
      usedToken = m_accessToken;
    }
    if (!usedToken.empty())
    {
      authHeader = "Authorization: Bearer " + usedToken;
      headers = curl_slist_append(headers, authHeader.c_str());
    }
  }

  BoundedStringSink responseSink{&responseBody, kMaxJsonResponseBytes};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, BoundedWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseSink);
  ApplyStandardCurlOptions(curl, GetCurlShare());
  // A shorter limit for a call whose answer is not worth the configured timeout (see ShortRequestTimeoutMs() and
  // friends): against a server that accepts connections and never answers, the configured timeout is what
  // Stop, a tail-wait refresh and the startup version check each waited out in full.
  if (timeoutMsOverride > 0)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMsOverride);

  if (method == "POST")
  {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
  }
  else if (method == "PATCH" || method == "DELETE" || method == "PUT")
  {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!bodyStr.empty())
    {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
    }
  }
  // else GET: nothing extra to set

  CURLcode res = static_cast<CURLcode>(PerformWithSafeRedirects(
      curl, url, [&responseBody]() { responseBody.clear(); }, /*switchPostToGet=*/method == "POST"));
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  // Set for every exit path below, including a transport failure -- 0
  // (curl_easy_getinfo()'s own value when no response was ever received)
  // is itself a meaningful signal to a caller like Login() that
  // distinguishes "no response at all" from any real HTTP status.
  if (httpStatusOut)
    *httpStatusOut = httpCode;
  if (res == CURLE_OK)
  {
    char* localIp = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_LOCAL_IP, &localIp) == CURLE_OK && localIp && *localIp)
    {
      std::lock_guard<std::mutex> lock(m_lastLocalIpMutex);
      m_lastLocalIp = localIp;
    }
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK)
  {
    error = responseSink.allocationFailed ? "Ran out of memory receiving the API response"
            : responseSink.exceeded ? "HTTP response exceeded the " + std::to_string(kMaxJsonResponseBytes >> 20) +
                                          " MiB limit this addon accepts for an API response"
                                    : std::string("HTTP request failed: ") + curl_easy_strerror(res);
    return false;
  }

  if (httpCode == 401 && withAuth && retryOnAuthFailure > 0)
  {
    // Through EnsureAuthenticated(), not RefreshAccessToken()/Login() called
    // directly (changed 2026-09-30, docs/OPEN_ITEMS.md): the direct calls
    // never touched m_consecutiveLoginFailures or the transient cooldown, so
    // a token Dispatcharr revoked server-side (a password change) while the
    // local ~4-minute expiry hint still looked valid sent every call in that
    // window through its own refresh-then-Login with no backoff at all --
    // exactly the login rate-limiter trip AuthBackoff.h exists to prevent,
    // just time-boxed. Invalidating the hint first makes EnsureAuthenticated()
    // actually refresh instead of reporting the rejected token as good, and
    // doing it only if the token is still the one this request was rejected
    // with means threads that all got a 401 together share one refresh
    // instead of each forcing another.
    InvalidateAccessTokenIfCurrent(usedToken);
    std::string refreshError;
    if (EnsureAuthenticated(refreshError, timeoutMsOverride))
      return Request(method, path, body, responseOut, error, withAuth, retryOnAuthFailure - 1, httpStatusOut,
                     timeoutMsOverride);
    error = "Authentication failed: " + refreshError;
    return false;
  }

  if (httpCode < 200 || httpCode >= 300)
  {
    error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + ": " +
            dispatcharr::SanitizeServerErrorBody(responseBody);
    return false;
  }

  if (responseBody.empty())
  {
    responseOut = json::object();
    return true;
  }

  return dispatcharr::ParseJsonResponseBody(responseBody, responseOut, error);
}

bool DispatcharrClient::Login(std::string& error, long* httpStatusOut, long timeoutMsOverride)
{
  // The caller holds m_authFlowMutex; m_authStateMutex is taken only for the write below, never across the
  // network call, so a thread that merely reads the cached token is not held up behind a login.
  json body = {{"username", m_config.username}, {"password", m_config.password}};
  json response;
  if (!Request("POST", kTokenPath, body, response, error, /*withAuth=*/false, /*retry=*/0, httpStatusOut,
               timeoutMsOverride))
    return false;

  if (!response.contains("access"))
  {
    error = "Login response did not contain an access token";
    return false;
  }

  std::lock_guard<std::mutex> stateLock(m_authStateMutex);
  m_accessToken = FieldOr<std::string>(response, "access", "");
  m_refreshToken = FieldOr<std::string>(response, "refresh", m_refreshToken);
  // SimpleJWT's default access-token lifetime is short (often 5 minutes);
  // we don't decode the JWT to read its real `exp`, we just re-authenticate
  // reactively on the next 401 (see Request()). This timestamp is kept only
  // as an optimisation hint, not a hard guarantee.
  m_accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(4);
  return true;
}

bool DispatcharrClient::RefreshAccessToken(std::string& error, long* httpStatusOut, long timeoutMsOverride)
{
  // The caller holds m_authFlowMutex (see Login()).
  std::string refreshToken;
  {
    std::lock_guard<std::mutex> stateLock(m_authStateMutex);
    refreshToken = m_refreshToken;
  }
  if (refreshToken.empty())
  {
    error = "No refresh token available";
    return false;
  }
  json body = {{"refresh", refreshToken}};
  json response;
  if (!Request("POST", kTokenRefreshPath, body, response, error, /*withAuth=*/false, /*retry=*/0, httpStatusOut,
               timeoutMsOverride))
    return false;

  if (!response.contains("access"))
  {
    error = "Refresh response did not contain an access token";
    return false;
  }
  std::lock_guard<std::mutex> stateLock(m_authStateMutex);
  m_accessToken = FieldOr<std::string>(response, "access", "");
  m_accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::minutes(4);
  return true;
}

void DispatcharrClient::InvalidateAccessTokenIfCurrent(const std::string& rejectedToken)
{
  std::lock_guard<std::mutex> lock(m_authStateMutex);
  if (m_accessToken == rejectedToken)
    m_accessTokenExpiry = std::chrono::steady_clock::time_point{};
}

void DispatcharrClient::InvalidateAccessToken()
{
  std::lock_guard<std::mutex> lock(m_authStateMutex);
  m_accessTokenExpiry = std::chrono::steady_clock::time_point{};
}

bool DispatcharrClient::EnsureAuthenticated(std::string& error, long timeoutMsOverride)
{
  // Fast path: a still-valid cached token needs only the short state lock, so a thread that has a token is
  // never held up behind another thread's login or refresh (docs/OPEN_ITEMS.md, "m_authMutex held across a
  // network round trip": ReadLiveTimeshiftStream()'s refresh used to wait out a whole login this way).
  {
    std::lock_guard<std::mutex> stateLock(m_authStateMutex);
    if (!m_accessToken.empty() && std::chrono::steady_clock::now() < m_accessTokenExpiry)
      return true;
  }

  // Only a thread that needs a new token waits here, one at a time, across the network calls below. After
  // getting the lock it looks again: whoever held it may have just authenticated, or failed and set a backoff
  // that now answers for this call too.
  std::lock_guard<std::mutex> flowLock(m_authFlowMutex);
  std::unique_lock<std::mutex> stateLock(m_authStateMutex);
  if (!m_accessToken.empty() && std::chrono::steady_clock::now() < m_accessTokenExpiry)
    return true;

  // Back off after repeated Login() failures instead of retrying it on
  // every single call -- see AuthBackoff.h's own comment for the real
  // incident (wrong/role-incompatible credentials tripping Dispatcharr's
  // own login rate-limiter) this closes.
  auto now = std::chrono::steady_clock::now();
  if (m_consecutiveLoginFailures > 0 && now < m_loginBackoffUntil)
  {
    error = m_lastLoginError;
    return false;
  }
  // Short, fixed, non-escalating cooldown for a transient failure (added
  // 2026-09-27, a 46th-pass audit, fixing a real, confirmed regression
  // found via a project-wide review, not itself independently reproduced
  // -- see m_transientLoginFailedAt's own comment, DispatcharrClient.h,
  // for the full account): without this, a transport failure/5xx got no
  // backoff at all once the fix below stopped it from counting toward
  // the real one, so EVERY EnsureAuthenticated() call from EVERY thread
  // re-attempted a full blocking Login() (holding the auth mutex for its
  // entire duration) during an extended outage.
  //
  // Checked *before* the refresh-token attempt too, not just before
  // Login() (fixed 2026-09-27, a 47th-pass audit, fixing a real,
  // confirmed gap the 46th-pass fix above itself left open, found via a
  // project-wide review, not itself independently reproduced): a refresh
  // token, once obtained, is never cleared on a failed refresh (see
  // RefreshAccessToken()'s own comment below), so once this addon has
  // logged in successfully even once, every later EnsureAuthenticated()
  // call tried RefreshAccessToken() -- which held the same auth mutex
  // for its own full blocking HTTP call -- completely bypassing both
  // gates below. That made a mid-session outage (as opposed to one right
  // at addon startup, before any login has ever succeeded) actually the
  // *more* common way to hit the exact thundering-herd/serialization
  // storm these gates exist to prevent, not a narrow corner case of it.
  const bool callerHasShortBound = timeoutMsOverride > 0 && timeoutMsOverride < m_config.timeoutSeconds * 1000L;
  if (dispatcharr::IsTransientCooldownBlocking(
          !dispatcharr::IsRetryDue(m_transientLoginFailedAt, now, std::chrono::seconds(kTransientLoginRetrySeconds)),
          m_transientLoginArmedByShortAttempt, callerHasShortBound))
  {
    error = m_lastLoginError;
    return false;
  }

  const bool haveRefreshToken = !m_refreshToken.empty();
  // Released across the network calls below (m_authFlowMutex alone keeps other authenticators out) and taken
  // again for each write to the shared state.
  stateLock.unlock();
  if (haveRefreshToken)
  {
    long refreshHttpStatus = 0;
    const bool refreshed = RefreshAccessToken(error, &refreshHttpStatus, timeoutMsOverride);
    stateLock.lock();
    if (refreshed)
    {
      // Reset here too, not just in Login()'s own success branch below
      // (fixed 2026-09-27, a 58th-pass audit, fixing a real, confirmed
      // gap found via a project-wide review, not itself independently
      // reproduced): without this, a stale, accumulated failure count
      // from an EARLIER, already-resolved Login() backoff episode
      // survives a successful refresh untouched, so the NEXT genuine
      // Login() failure -- whenever it happens, unrelated to the
      // earlier ones -- escalates ComputeLoginBackoffSeconds() from that
      // stale count instead of starting fresh, giving a longer backoff
      // than a first failure should get. Harmless in effect (only ever
      // makes the backoff longer than intended, never shorter or
      // skipped), but the intent is clearly "count resets once
      // authentication genuinely succeeds again", which this path is.
      m_consecutiveLoginFailures = 0;
      return true;
    }
    // The three outcomes and why each is handled this way are documented at
    // dispatcharr::ClassifyRefreshTokenFailure() (AuthBackoff.h), where the
    // mapping is unit-tested.
    switch (ClassifyRefreshTokenFailure(refreshHttpStatus))
    {
    case RefreshTokenFailure::kRejected:
      // Clear the dead refresh token so it isn't retried forever; Login()
      // below gets this addon a fresh pair of tokens instead.
      m_refreshToken.clear();
      break;
    case RefreshTokenFailure::kTransient:
      m_lastLoginError = error;
      m_transientLoginFailedAt = std::chrono::steady_clock::now();
      m_transientLoginArmedByShortAttempt = callerHasShortBound;
      return false;
    case RefreshTokenFailure::kOther:
      break; // fall through to Login() below, same as always
    }
    stateLock.unlock();
  }

  long httpStatus = 0;
  const bool loggedIn = Login(error, &httpStatus, timeoutMsOverride);
  stateLock.lock();
  if (loggedIn)
  {
    m_consecutiveLoginFailures = 0;
    m_transientLoginFailedAt = {};
    m_transientLoginArmedByShortAttempt = false;
    return true;
  }

  // A fresh timestamp here, not the entry-time `now` above (fixed
  // 2026-09-27, a 46th-pass audit, fixing a real, confirmed bug found via
  // a project-wide review while fixing the transient-cooldown gap just
  // above -- almost reintroduced the exact same mistake in this same
  // pass): Login()'s own CURL timeout is `timeoutSeconds` (30s default),
  // the same order of magnitude as both kTransientLoginRetrySeconds and
  // ComputeLoginBackoffSeconds()'s own first-failure wait -- stamping
  // either backoff from the stale, pre-call `now` could shrink or
  // entirely swallow the intended wait for a Login() call that itself
  // took a while to time out, the same class of bug
  // EnsureChannelsLoaded()/EnsureEpgLoaded()'s own failure timestamps
  // just had fixed (PVRDispatcharr.cpp).
  auto failedAt = std::chrono::steady_clock::now();
  m_lastLoginError = error;
  // Only a genuine credential/authorization rejection extends the
  // backoff -- see dispatcharr::ShouldCountTowardLoginBackoff()'s own
  // comment (AuthBackoff.h) for the real, confirmed gap this closes: a
  // transport failure or 5xx (Dispatcharr itself unreachable, still
  // starting up, or briefly erroring) leaves the counter untouched
  // instead, so a genuine outage doesn't leave this addon refusing to
  // even attempt Login() again for up to kMaxSeconds after Dispatcharr
  // has already recovered. Falls through to the short transient cooldown
  // above instead, so this doesn't collapse right back to "no backoff at
  // all".
  if (ShouldCountTowardLoginBackoff(httpStatus))
  {
    ++m_consecutiveLoginFailures;
    m_loginBackoffUntil = failedAt + std::chrono::seconds(ComputeLoginBackoffSeconds(m_consecutiveLoginFailures));
  }
  else
  {
    m_transientLoginFailedAt = failedAt;
    m_transientLoginArmedByShortAttempt = callerHasShortBound;
  }
  return false;
}

bool DispatcharrClient::GetAccessToken(std::string& tokenOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  std::lock_guard<std::mutex> lock(m_authStateMutex);
  tokenOut = m_accessToken;
  return true;
}

bool DispatcharrClient::GetChannels(std::vector<Channel>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kChannelsPath, json(), response, error))
    return false;

  // Django REST Framework's default pagination wraps results in
  // {count, next, previous, results:[...]}; handle both that and a bare
  // array in case pagination is disabled on this endpoint. The tolerance
  // itself lives in dispatcharr::UnwrapListResponse() (JsonFieldUtil.h,
  // shared by every list endpoint below) so it's unit-testable
  // standalone -- see that function's own comment.
  // A real, confirmed risk (this endpoint is documented as "paginated"
  // in this file's own top-of-file API summary), not reproduced live:
  // dispatcharr::IsTruncatedPaginatedResponse() (JsonFieldUtil.h) fails
  // this call loudly rather than silently caching only the first page
  // as if it were the complete channel list -- see its own comment.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "/api/channels/channels/ response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response);
  if (!list.is_array())
  {
    error = "Unexpected /api/channels/channels/ response shape";
    return false;
  }

  // The field-mapping logic itself lives in dispatcharr::ParseChannelJson()
  // (ChannelParser.{h,cpp}) so it's unit-testable standalone -- see that
  // function's own comment.
  out.clear();
  for (const auto& item : list)
    out.push_back(ParseChannelJson(item));
  return true;
}

bool DispatcharrClient::GetChannelGroups(std::vector<ChannelGroup>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Neither path was independently confirmed against a live Swagger doc
  // (see docs/API_NOTES.md); try the primary one and fall back to the
  // alternate on failure rather than guessing wrong and going silent.
  json response;
  if (!Request("GET", kChannelGroupsPath, json(), response, error))
  {
    std::string fallbackError;
    if (!Request("GET", kChannelGroupsPathFallback, json(), response, fallbackError))
    {
      error += " / " + fallbackError;
      return false;
    }
  }

  // Same truncated-pagination guard as GetChannels() above -- see
  // dispatcharr::IsTruncatedPaginatedResponse()'s own comment.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "channel-groups response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response);
  if (!list.is_array())
  {
    error = "Unexpected channel-groups response shape";
    return false;
  }

  out.clear();
  for (const auto& item : list)
    out.push_back(ParseChannelGroupJson(item));
  return true;
}

bool DispatcharrClient::GetXmlTvGuide(std::string& xmlOut, std::string& error, long* httpStatusOut, int prevDays,
                                      bool* responseUnusableOut)
{
  if (httpStatusOut)
    *httpStatusOut = 0;
  if (responseUnusableOut)
    *responseUnusableOut = false;
  // Deliberately does NOT call EnsureAuthenticated() (removed 2026-09-27,
  // a 48th-pass audit, fixing a real, confirmed bug found via a
  // project-wide review, confirmed against Dispatcharr's own real
  // current upstream source, not itself independently reproduced): this
  // request never sends an Authorization header at all (see below), and
  // confirmed against Dispatcharr's own real current upstream source
  // that `/output/epg` (`apps/output/views.py`'s `epg_endpoint()`) is a
  // plain Django view gated only by its own "M3U / EPG Endpoints"
  // network-access policy, not a login requirement -- so login being
  // currently backed off (up to 30 minutes for the credential backoff,
  // or the shorter transient cooldowns) used to needlessly block a guide
  // refresh this call has no actual auth dependency on at all, and each
  // blocked attempt itself stamped `m_epgLastFailedAt`
  // (`EnsureEpgLoaded()`, `PVRDispatcharr.cpp`), further delaying the
  // *next* attempt for a reason unrelated to the guide fetch itself.
  // `EnsureEpgLoaded()`'s own retry gate already throttles how often this
  // is attempted regardless.
  //
  // /output/epg returns raw XML, not JSON -- issue Request() manually via
  // a small local curl call instead of the JSON-oriented Request() helper.
  CURL* curl = curl_easy_init();
  if (!curl)
  {
    error = "Failed to initialise libcurl";
    return false;
  }
  std::string url = BaseUrl() + kEpgOutputPath;
  if (prevDays > 0)
    url += "?prev_days=" + std::to_string(prevDays);
  xmlOut.clear();
  BoundedStringSink xmlSink{&xmlOut, kMaxXmlTvResponseBytes};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, BoundedWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xmlSink);
  // The standard options first -- including the transfer-progress callback that lets
  // AbortInFlightRequests() end this transfer at once (added 2026-10-03, see
  // docs/OPEN_ITEMS.md's "Kodi's own threads waited on the guide fetch, and two
  // transfers could not be aborted at shutdown"): this is the largest single transfer the addon makes and it runs
  // on the background thread at every start, so a Kodi exit during a slow or stalled guide
  // download used to wait out the whole timeout below. Then the longer guide-sized
  // timeout, which overrides the standard one.
  ApplyStandardCurlOptions(curl, GetCurlShare());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds * 4));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);
  if (httpStatusOut && res == CURLE_OK)
    *httpStatusOut = httpCode;

  if (res != CURLE_OK)
  {
    if (responseUnusableOut)
      *responseUnusableOut = xmlSink.allocationFailed || xmlSink.exceeded;
    error = xmlSink.allocationFailed ? "Ran out of memory receiving the XMLTV guide"
            : xmlSink.exceeded       ? "The XMLTV guide exceeded the " + std::to_string(kMaxXmlTvResponseBytes >> 20) +
                                     " MiB limit this addon accepts"
                               : std::string("Failed to fetch XMLTV guide: ") + curl_easy_strerror(res);
    return false;
  }
  if (httpCode < 200 || httpCode >= 300)
  {
    // A 403 specifically here (as opposed to every other endpoint this
    // client calls, all of which use Dispatcharr's own auth) means
    // Dispatcharr's own Network Access restriction, not a login/permission
    // problem -- confirmed against Dispatcharr's own real current upstream
    // source (a 24th-pass audit, cloned into a scratchpad, never
    // committed to this repo -- stronger than the API shape alone, not
    // the same standard as a live test): `/output/epg`
    // (`apps/output/views.py`'s `epg_endpoint()`) is the *one* endpoint
    // whose "M3U / EPG Endpoints" network-access setting defaults to
    // local-network-only CIDRs (`dispatcharr/utils.py`'s
    // `LOCAL_NETWORK_CIDRS`: 127/8, 10/8, 172.16/12, 192.168/16, ::1,
    // fc00::/7, fe80::/10) -- every other endpoint this addon calls
    // defaults to allow-all. A Kodi client reachable to Dispatcharr from
    // outside those ranges (Tailscale/CGNAT addresses, a dual-stack LAN
    // where Dispatcharr's own host setting resolves to a global IPv6
    // address, remote access) gets a working login/channels/playback but
    // a silently empty guide, with nothing in this addon's own log
    // pointing at the real cause otherwise.
    if (httpCode == 403)
      error = "Dispatcharr returned HTTP 403 for /output/epg -- this is usually Dispatcharr's own Network Access "
              "restriction, not a login/permission problem: check Settings -> Network Access -> \"M3U / EPG "
              "Endpoints\" on the Dispatcharr server (it defaults to local-network-only, unlike every other "
              "endpoint this addon uses)";
    else
      error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " for /output/epg";
    return false;
  }
  return true;
}

std::string DispatcharrClient::GetLiveStreamUrl(const Channel& channel) const
{
  // `|Connection=close` is a real Kodi/CCurlFile URL option (see
  // xbmc/filesystem/CurlFile.cpp's protocol option handling) that adds a
  // literal `Connection: close` request header. This was tried while
  // diagnosing a "channel N+1 never plays" failure, on the theory that Kodi
  // was reusing/pooling a still-closing connection to the same host -- it
  // didn't fix that (the real cause turned out to be an unreachable IPv6
  // route to the host in that environment), but it's harmless to leave
  // in place and does prevent connection reuse in general.
  return BaseUrl() + "/proxy/ts/stream/" + channel.uuid + "|Connection=close";
}

std::string DispatcharrClient::GetChannelLogoUrl(int logoId) const
{
  return BaseUrl() + kLogosPath + std::to_string(logoId) + "/cache/";
}

bool DispatcharrClient::CreateCatchupSession(const std::string& channelUuid, time_t programmeStart, int durationMinutes,
                                             std::string& playbackUrlOut, std::string& error)
{
  // Fix for a real, confirmed, live-quantified bug (see docs/OPEN_ITEMS.md's
  // 2026-09-29 live check): Kodi's own CPVRGUIActionsPlayback::PlayEpgTag()
  // calls GetEpgTagStreamProperties() twice for the exact same catch-up
  // play, with identical (channelUuid, programmeStart, durationMinutes)
  // both times -- confirmed live to cost roughly 30% of the total delay
  // before playback actually starts. A cache hit skips straight past
  // EnsureAuthenticated() and the network entirely, not just the POST
  // itself -- there's nothing left to do once the exact same session is
  // already known good.
  {
    std::lock_guard<std::mutex> lock(m_catchupSessionCacheMutex);
    if (dispatcharr::ShouldReuseCachedCatchupSession(
            m_catchupSessionCache.channelUuid, m_catchupSessionCache.programmeStart,
            m_catchupSessionCache.durationMinutes, m_catchupSessionCache.cachedAt, channelUuid, programmeStart,
            durationMinutes, std::chrono::steady_clock::now(), kMaxCatchupSessionCacheAge))
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr-unofficial: CreateCatchupSession: cache hit for channel=%s "
                "start=%lld duration=%d, skipping a redundant POST",
                channelUuid.c_str(), static_cast<long long>(programmeStart), durationMinutes);
      playbackUrlOut = m_catchupSessionCache.playbackUrl;
      return true;
    }
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: CreateCatchupSession: cache miss for channel=%s start=%lld "
              "duration=%d (cached: channel=%s start=%lld duration=%d)",
              channelUuid.c_str(), static_cast<long long>(programmeStart), durationMinutes,
              m_catchupSessionCache.channelUuid.c_str(), static_cast<long long>(m_catchupSessionCache.programmeStart),
              m_catchupSessionCache.durationMinutes);
  }

  if (!EnsureAuthenticated(error))
    return false;

  // Body assembly (including the API's own 480-minute duration cap) and the
  // relative playback_url join both live in CatchupSessionRequest.h, tested.
  json body = BuildCatchupSessionBody(channelUuid, programmeStart, durationMinutes);

  json response;
  if (!Request("POST", kCatchupSessionsPath, body, response, error))
    return false;

  std::string playbackUrl = FieldOr<std::string>(response, "playback_url", "");
  if (playbackUrl.empty())
  {
    error = "Catch-up session response did not contain a playback_url";
    return false;
  }
  playbackUrl = ResolveCatchupPlaybackUrl(playbackUrl, BaseUrl());

  {
    std::lock_guard<std::mutex> lock(m_catchupSessionCacheMutex);
    m_catchupSessionCache.channelUuid = channelUuid;
    m_catchupSessionCache.programmeStart = programmeStart;
    m_catchupSessionCache.durationMinutes = durationMinutes;
    m_catchupSessionCache.playbackUrl = playbackUrl;
    m_catchupSessionCache.cachedAt = std::chrono::steady_clock::now();
  }

  playbackUrlOut = std::move(playbackUrl);
  return true;
}

bool DispatcharrClient::CallTimeshiftPluginAction(const std::string& action, const std::string& channelUuid,
                                                  std::string& playlistUrlOut, std::string& error,
                                                  const json& extraParams, bool* retryableOut)
{
  if (retryableOut)
    *retryableOut = false;
  if (!EnsureAuthenticated(error))
    return false;

  json params = {{"channel_uuid", channelUuid}};
  params.update(extraParams);
  json body = {
      {"action", action},
      {"params", params},
  };

  json response;
  // Request() already turns any non-2xx (403 disabled-plugin/non-admin
  // account, 404 plugin-not-installed, 500 exception inside the plugin's
  // own run()) into a failure here, with the raw response body folded into
  // `error` -- confirmed against apps/plugins/api_views.py's
  // PluginRunAPIView that every one of those paths pairs "success": false
  // with a matching non-2xx status, never 200. So this call only needs to
  // handle the 200 case below: PluginRunAPIView always wraps whatever the
  // plugin's own run() returned inside a top-level "result" key (alongside
  // its own "success": true), and the plugin can still report its own
  // *logical* failure (e.g. hitting max_concurrent_buffers, or a buffer
  // that's genuinely gone -- see docs/TIMESHIFT.md's "concurrent-stream
  // limit" section) as a normal 200 response with "result": {"status":
  // "error", ...} rather than an exception -- that's the case the
  // "result" parsing below actually exists to catch.
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error))
    return false;

  json result;
  if (!UnwrapPluginRunResult(response, "timeshift_buffer", result, error))
  {
    // result is still populated even on a logical (inner status != "ok")
    // failure -- see UnwrapPluginRunResult()'s own comment -- so this
    // structured field is readable regardless of why the call failed.
    if (retryableOut)
      *retryableOut = FieldOr(result, "retryable", false);
    return false;
  }

  int httpPort = FieldOr(result, "http_port", 0);
  std::string playlistRoute = FieldOr<std::string>(result, "playlist_route", "");
  if (httpPort <= 0 || playlistRoute.empty())
  {
    error = "timeshift_buffer plugin response was missing http_port/playlist_route";
    return false;
  }

  // Surfaces the plugin's own "started new" vs "reattached to an
  // already-running buffer" distinction (see plugin.py's start_buffer),
  // which this addon otherwise has no visibility into -- useful for
  // confirming the buffer is genuinely shared per-channel across viewers
  // rather than per-device, not just assumed from reading the plugin's
  // own source.
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: CallTimeshiftPluginAction(%s): %s (already_running=%d)",
            action.c_str(), FieldOr<std::string>(result, "message", "").c_str(),
            FieldOr(result, "already_running", false) ? 1 : 0);

  // The plugin's own file server requires a per-buffer access token on
  // every request (see plugin.py's _check_access_token) -- issued here,
  // via this authenticated action call, not readable any other way.
  // token_urlsafe()'s output (Python's secrets module) is already
  // URL-safe base64 ([A-Za-z0-9_-], no padding), so it's safe to append
  // to a query string directly with no escaping needed. Stored on
  // m_liveTimeshiftStream so ReadLiveTimeshiftStream()'s later segment
  // fetches (against segmentBaseUrl, built separately by
  // RefreshLiveManifest() from a different action's response) can reuse
  // it without needing every action's response to carry it. The URL
  // assembly itself lives in dispatcharr::BuildTimeshiftPlaylistUrl()
  // (PluginUrlUtil.h) so it's unit-testable standalone -- see that
  // function's own comment.
  std::string accessToken = FieldOr<std::string>(result, "access_token", "");
  if (!accessToken.empty())
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    m_liveTimeshiftStream.accessToken = accessToken;
  }
  playlistUrlOut = BuildTimeshiftPlaylistUrl(m_config.host, httpPort, playlistRoute, accessToken);

  return true;
}

bool DispatcharrClient::StartTimeshiftBuffer(const std::string& channelUuid, std::string& playlistUrlOut,
                                             std::string& error, bool* retryableOut)
{
  // Passed through so the plugin's ffmpeg connection (which otherwise
  // looks anonymous and container-local in Dispatcharr's own Stats screen,
  // since it runs server-side rather than from the viewer's own device --
  // confirmed live) can be attributed properly: username to the same
  // Dispatcharr account this addon is already configured with (no separate
  // plugin-side setting to keep in sync), client_ip to whichever local
  // interface this machine actually reaches Dispatcharr through (from
  // Request()'s own CURLINFO_LOCAL_IP, not a platform-specific "what's my
  // IP" lookup).
  json extraParams = {{"username", m_config.username}};
  {
    std::lock_guard<std::mutex> lock(m_lastLocalIpMutex);
    if (!m_lastLocalIp.empty())
      extraParams["client_ip"] = m_lastLocalIp;
  }
  // Lets the plugin reference-count viewers of a shared buffer -- see
  // LiveTimeshiftStreamState::viewerId's own comment and
  // StopTimeshiftBuffer()'s for the full mechanism this enables.
  std::string viewerId;
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    viewerId = m_liveTimeshiftStream.viewerId;
  }
  if (!viewerId.empty())
    extraParams["viewer_id"] = viewerId;
  return CallTimeshiftPluginAction("start_buffer", channelUuid, playlistUrlOut, error, extraParams, retryableOut);
}

long DispatcharrClient::ShortRequestTimeoutMs() const
{
  // The bound for Stop, the live-edge and in-progress steady-state refreshes and the startup version check, and the
  // authentication in front of them: see RequestTimeout.h for why these are short, and what they follow.
  return dispatcharr::ShortRequestTimeoutMs(m_config.timeoutSeconds);
}

bool DispatcharrClient::StopTimeshiftBuffer(const std::string& channelUuid, const std::string& viewerId,
                                            std::string& error)
{
  // Nothing to wait for once the client is shutting down: AbortInFlightRequests() ends a request in flight at
  // once, and the reaper handles the buffer.
  if (m_abortRequests)
  {
    error = "shutting down";
    return false;
  }
  // Doesn't use CallTimeshiftPluginAction(): that helper requires
  // http_port/playlist_route in the response, which stop_buffer's own
  // {"status": "ok"} reply doesn't carry.
  if (!EnsureAuthenticated(error, ShortRequestTimeoutMs()))
    return false;

  json params = {{"channel_uuid", channelUuid}};
  if (!viewerId.empty())
    params["viewer_id"] = viewerId;

  json body = {
      {"action", "stop_buffer"},
      {"params", params},
  };
  json response;
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1,
               nullptr, ShortRequestTimeoutMs()))
    return false;
  json result;
  return UnwrapPluginRunResult(response, "timeshift_buffer", result, error);
}

void DispatcharrClient::SendTimeshiftHeartbeat(const std::string& channelUuid, const std::string& viewerId)
{
  // Deliberately does NOT go through EnsureAuthenticated()/Request(), and
  // deliberately does NOT use m_config.timeoutSeconds -- confirmed live
  // (1.0.5's first version of this function used both) that this is what
  // actually matters here: this call rides along on the exact same thread
  // Kodi's demuxer depends on for continuous reads
  // (ReadLiveTimeshiftStream()), so it must be bounded to a small, fixed
  // worst case regardless of network/auth conditions, not "at most a
  // couple of ordinary request timeouts, usually fine." EnsureAuthenticated()
  // can trigger a full synchronous token refresh or re-login on a cache
  // miss (each its own Request() call, each allowed up to
  // m_config.timeoutSeconds -- default 30s), and Request()'s own
  // withAuth/retryOnAuthFailure default retries a 401 via that same
  // refresh-or-login path again before retrying the original call --
  // stacking up to roughly 150s of possible blocking in the worst
  // realistic case (a transient network hiccup right as the access token
  // needed refreshing). That's long enough for Kodi's own player to give
  // up on a stalled input and never recover, even once the slow call
  // eventually completed -- reproduced live as a total, non-recovering
  // "buffering" stall on the very first 1.0.5 playback attempt. Fixed by
  // reading whatever access token is already cached (a mutex lock, not a
  // network call) and using a short, fixed timeout on this call's own
  // curl handle. If the cached token is empty or has since expired, this
  // heartbeat is simply skipped rather than triggering a refresh itself --
  // RefreshLiveManifest() (called far more often than this, every
  // read-loop iteration) already keeps the token fresh via the normal
  // path in practice, so a skipped heartbeat here just means the next
  // one, ~10s later, tries again -- never worth blocking a live read to
  // guarantee any single heartbeat lands.
  if (viewerId.empty())
    return;

  std::string token;
  {
    std::lock_guard<std::mutex> lock(m_authStateMutex);
    // Checking m_accessTokenExpiry too, not just emptiness -- a gap found
    // via a project-wide review, not itself independently reproduced: an
    // expired-but-still-present token was previously sent anyway, eating
    // an avoidable 401 round trip on this call's own short fixed timeout,
    // contradicting this function's own comment above ("this heartbeat is
    // simply skipped" when the cached token "has since expired").
    if (!m_accessToken.empty() && std::chrono::steady_clock::now() < m_accessTokenExpiry)
      token = m_accessToken;
  }
  if (token.empty())
    return;

  json body = {
      {"action", "heartbeat"},
      {"params", {{"channel_uuid", channelUuid}, {"viewer_id", viewerId}}},
  };
  std::string bodyStr = body.dump();

  CURL* curl = curl_easy_init();
  if (!curl)
    return;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");
  std::string authHeader = "Authorization: Bearer " + token;
  headers = curl_slist_append(headers, authHeader.c_str());

  // Bounded to a 1-byte FixedBufferSink, not a plain unbounded
  // WriteCallback into a std::string (fixed 2026-09-27, a 58th-pass
  // audit, fixing a real, confirmed gap found via a project-wide review,
  // not itself independently reproduced): the response body is never
  // read below -- only `res` (the transfer result) is checked -- so
  // there's nothing here for an unbounded accumulation to ever be used
  // for, only a risk to guard against. FixedBufferWriteCallback never
  // aborts the transfer, matching this function's own existing
  // fire-and-forget, best-effort tolerance for whatever the server sends
  // back.
  uint8_t responseBuffer[1];
  FixedBufferSink responseSink{responseBuffer, 1, 0};
  std::string url = BaseUrl() + kTimeshiftPluginRunPath;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseSink);
  // Fixed and short, NOT m_config.timeoutSeconds -- see this function's own
  // comment. A heartbeat that can't complete quickly is exactly as useful
  // skipped as it is completed many seconds late.
  // The standard options (TLS verification, the share, NOSIGNAL and the shutdown abort
  // callback) first, then this call's own short timeout overriding the standard one.
  constexpr long kHeartbeatTimeoutMs = 2000;
  ApplyStandardCurlOptions(curl, GetCurlShare());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHeartbeatTimeoutMs);

  CURLcode res = static_cast<CURLcode>(
      PerformWithSafeRedirects(curl, url, [&responseSink]() { responseSink.written = 0; }, /*switchPostToGet=*/true));
  if (res != CURLE_OK)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: SendTimeshiftHeartbeat: request failed: %s",
              curl_easy_strerror(res));
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

bool DispatcharrClient::GetRecordings(std::vector<Recording>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kRecordingsPath, json(), response, error))
    return false;

  // Same truncated-pagination guard as GetChannels() -- see
  // dispatcharr::IsTruncatedPaginatedResponse()'s own comment.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "recordings response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response);
  if (!list.is_array())
  {
    error = "Unexpected recordings response shape";
    return false;
  }

  time_t now = time(nullptr);
  out.clear();
  out.reserve(list.size());
  for (const auto& item : list)
    out.push_back(ParseRecordingJson(item, now));
  return true;
}

bool DispatcharrClient::GetRecordingById(int id, Recording& out, std::string& error, long* httpStatusOut,
                                         long timeoutMsOverride, bool* authenticationNotCompletedOut)
{
  if (httpStatusOut)
    *httpStatusOut = 0;
  if (authenticationNotCompletedOut)
    *authenticationNotCompletedOut = false;
  if (!EnsureAuthenticated(error, timeoutMsOverride))
  {
    if (authenticationNotCompletedOut)
      *authenticationNotCompletedOut = true;
    return false;
  }

  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(id) + "/";
  if (!Request("GET", path, json(), response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1, httpStatusOut,
               timeoutMsOverride))
    return false;

  if (!IsRecordingResponseFor(response, id))
  {
    // A 2xx that is not this recording (an empty body, a list, another id) is not a lookup: see
    // IsRecordingResponseFor(). The HTTP status stays what the server sent, so a caller that
    // treats 404 specially still sees only a real 404 as "gone".
    error = "the server's answer for recording " + std::to_string(id) + " did not describe that recording";
    return false;
  }

  out = ParseRecordingJson(response, time(nullptr));
  return true;
}

Recording DispatcharrClient::ParseRecordingJson(const json& item, time_t now)
{
  // The field-mapping/status-override/title-fallback-chain logic itself
  // lives in dispatcharr::ParseRecordingFields() (RecordingParser.{h,cpp})
  // so it's unit-testable standalone -- see that function's own comment.
  // Only the PendingTitle-cache lookup below (member state behind
  // m_pendingTitlesMutex) and the final "Recording <id>" default stay
  // here, since neither is available to a free function.
  Recording r = dispatcharr::ParseRecordingFields(item, now);
  if (r.title.empty())
  {
    // See PendingTitle's comment on why this can be filled in before
    // Dispatcharr's own async enrichment has caught up, and why it's
    // matched by recording id.
    std::lock_guard<std::mutex> lock(m_pendingTitlesMutex);
    constexpr auto kPendingTitleTtl = std::chrono::minutes(3);
    // The prune and by-recording-id lookup itself live in
    // dispatcharr::PruneExpiredPendingTitles()/FindPendingTitleForRecording()
    // (PendingTitleLookup.h) so both are unit-testable standalone -- see
    // each function's own comment, including why a match is deliberately
    // not erased here.
    PruneExpiredPendingTitles(m_pendingTitles, std::chrono::steady_clock::now(), kPendingTitleTtl);
    if (const PendingTitle* match = FindPendingTitleForRecording(m_pendingTitles, r.id))
      r.title = match->title;
  }
  if (r.title.empty())
    r.title = "Recording " + std::to_string(r.id);
  return r;
}

bool DispatcharrClient::GetRecordingEdl(int recordingId, std::vector<RecordingEdlEntry>& out, std::string& error)
{
  out.clear();
  if (!EnsureAuthenticated(error))
    return false;

  json body = {
      {"action", "get_edl"},
      {"params", {{"recording_id", recordingId}}},
  };

  json response;
  // Same response shape as CallTimeshiftPluginAction()'s own comment
  // describes: Request() already turns any non-2xx (plugin not installed/
  // enabled, non-admin account, exception inside the plugin's own run())
  // into a failure here, so only the 200 case needs handling below.
  if (!Request("POST", kRecordingEdlPluginRunPath, body, response, error))
    return false;

  json result;
  if (!UnwrapPluginRunResult(response, "recording_edl", result, error))
    return false;

  // The field-mapping/validity-filter logic itself lives in
  // dispatcharr::ParseRecordingEdlEntryJson() (RecordingParser.{h,cpp}) so
  // it's unit-testable standalone -- see that function's own comment.
  const json& entries = result.contains("entries") ? result["entries"] : json();
  if (entries.is_array())
  {
    for (const auto& item : entries)
    {
      RecordingEdlEntry entry;
      if (ParseRecordingEdlEntryJson(item, entry))
        out.push_back(entry);
    }
  }
  return true;
}

bool DispatcharrClient::GenerateApiKey(std::string& keyOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("POST", kApiKeyGeneratePath, json(), response, error))
    return false;

  std::string key = FieldOr<std::string>(response, "key", "");
  if (key.empty())
  {
    error = "API key generation response did not contain a key";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_apiKeyMutex);
    m_config.apiKey = key;
  }
  keyOut = std::move(key);
  return true;
}

bool DispatcharrClient::FetchCurrentApiKey(ServerApiKeyLookup& lookupOut, std::string& error)
{
  // Not being able to log in at all is a failure to *find out*, never
  // evidence the account has no key -- kTransientError, so nothing gets
  // generated on the back of it.
  lookupOut = ServerApiKeyLookup{};
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  long httpStatus = 0;
  if (!Request("GET", kApiKeyListPath, json(), response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1,
               &httpStatus))
  {
    lookupOut = ClassifyApiKeyLookupFailure(httpStatus);
    return false;
  }
  lookupOut = ParseApiKeyListResponse(response);
  return true;
}

bool DispatcharrClient::ObtainApiKey(std::string& keyOut, std::string& error)
{
  // Held across the lookup AND any generate that follows -- see
  // m_apiKeyRecoveryMutex's own comment.
  std::lock_guard<std::mutex> recoveryLock(m_apiKeyRecoveryMutex);

  ServerApiKeyLookup lookup;
  std::string lookupError;
  FetchCurrentApiKey(lookup, lookupError);

  switch (DecideApiKeyRecovery(lookup))
  {
  case ApiKeyRecoveryAction::kUseServerKey:
  {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(m_apiKeyMutex);
      changed = m_config.apiKey != lookup.key;
      m_config.apiKey = lookup.key;
    }
    // Debug, not info: with several clients on one account this can fire
    // on every 401 recovery. Never logs the key itself.
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: %s",
              changed ? "adopted the account's existing API key (nothing regenerated)"
                      : "the account's current API key matches the one already held (nothing regenerated)");
    keyOut = std::move(lookup.key);
    return true;
  }
  case ApiKeyRecoveryAction::kGenerate:
    // Info: this is the one event that revokes the key for every other
    // client of the account, so it should be visible in the log when it
    // happens -- unlike before, it now only happens when there is no key.
    kodi::Log(ADDON_LOG_INFO,
              "pvr.dispatcharr-unofficial: the account has no API key yet (or this Dispatcharr can't report "
              "it) -- generating one");
    if (!GenerateApiKey(keyOut, error))
      return false;
    {
      // Two clients that both found the account keyless at the same moment
      // each generate one and Dispatcharr keeps only the last, so the key
      // just generated may already be dead. Re-read the account's actual key
      // and switch to it if it differs -- see ReconcileGeneratedApiKey()
      // (ApiKeyRecovery.h) for what this does and doesn't close. Best
      // effort: any failure to re-read just keeps the generated key.
      ServerApiKeyLookup verification;
      std::string verifyError;
      FetchCurrentApiKey(verification, verifyError);
      if (ReconcileGeneratedApiKey(keyOut, verification) == GeneratedKeyReconcileAction::kAdoptServerKey)
      {
        {
          std::lock_guard<std::mutex> lock(m_apiKeyMutex);
          m_config.apiKey = verification.key;
        }
        keyOut = std::move(verification.key);
        kodi::Log(ADDON_LOG_INFO,
                  "pvr.dispatcharr-unofficial: another client generated an API key at the same moment -- using "
                  "the account's current key instead of the one just generated");
      }
    }
    return true;
  case ApiKeyRecoveryAction::kFail:
    error = "could not read the account's current API key (" + lookupError +
            ") -- not generating a new one blind, since that would revoke the current one for every other "
            "client of this account";
    kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr-unofficial: %s", error.c_str());
    return false;
  }
  error = "unexpected API key recovery decision";
  return false;
}

bool DispatcharrClient::DeleteRecording(int recordingId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/";
  long httpStatus = 0;
  if (Request("DELETE", path, json(), response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1, &httpStatus))
    return true;
  // A 404 means the recording is already gone (added 2026-09-27, a
  // 48th-pass audit, fixing a real, confirmed bug found via a
  // project-wide review, not itself independently reproduced) -- exactly
  // the end state this call was trying to reach in the first place,
  // whether it was already deleted by another Kodi install sharing this
  // account, or Dispatcharr's own automatic cleanup. Confirmed against
  // Kodi's own real current SDK source that this matters, not just a
  // cosmetic nicety: AsyncRecordingAction::Run() (PVRGUIActionsRecordings.cpp)
  // surfaces any non-PVR_ERROR_NO_ERROR return here as a "backend error"
  // notification to the user, for something that's already true.
  return httpStatus == 404;
}

bool DispatcharrClient::StopRecording(int recordingId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/stop/";
  long httpStatus = 0;
  if (Request("POST", path, json(), response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1, &httpStatus))
    return true;
  // A 404 means the recording is already gone (added 2026-09-27, a
  // 49th-pass audit, fixing a real, confirmed bug found via a
  // project-wide review, not itself independently reproduced -- see
  // DeleteRecording()'s own comment above for the identical reasoning
  // and the real Kodi-side consequence this avoids): stopping something
  // that no longer exists has already reached the state this call was
  // trying to reach.
  return httpStatus == 404;
}

bool DispatcharrClient::RenameRecording(int recordingId, const std::string& newTitle, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json body = {{"title", newTitle}};
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/update-metadata/";
  return Request("POST", path, body, response, error);
}

bool DispatcharrClient::ExtendRecording(int recordingId, int extraMinutes, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json body = {{"extra_minutes", extraMinutes}};
  json response;
  std::string path = std::string(kRecordingsPath) + std::to_string(recordingId) + "/extend/";
  return Request("POST", path, body, response, error);
}

bool DispatcharrClient::GetTimerRules(std::vector<TimerRule>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kSeriesRulesPath, json(), response, error))
    return false;

  // Confirmed against a live instance: the response is {"rules": [...]},
  // not a bare array and not the usual DRF {"results": [...]} wrapper --
  // dispatcharr::UnwrapListResponse() (JsonFieldUtil.h) checks "rules"
  // first, "results" second.
  // Same truncated-pagination guard as GetChannels() -- see
  // dispatcharr::IsTruncatedPaginatedResponse()'s own comment. Harmless
  // here in practice (confirmed live this endpoint uses its own
  // {"rules": [...]} shape, not DRF pagination), but cheap and
  // consistent to check regardless.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "series-rules response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response, {"rules", "results"});
  if (!list.is_array())
  {
    error = "Unexpected series-rules response shape";
    return false;
  }

  // The field-mapping logic itself lives in dispatcharr::ParseTimerRuleJson()
  // (TimerRuleParser.{h,cpp}) so it's unit-testable standalone -- see that
  // function's own comment.
  out.clear();
  for (const auto& item : list)
    out.push_back(ParseTimerRuleJson(item));
  return true;
}

bool DispatcharrClient::CreateOneTimeRecording(int channelId, time_t start, time_t end, const std::string& title,
                                               bool isEpgBased, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Confirmed against a real recording: channel/start_time/end_time are the
  // only fields a MANUAL (non-EPG) recording needs to send. Deliberately
  // NOT sending its own title/id via custom_properties (e.g. {"title":
  // title}) -- confirmed that Dispatcharr auto-populates
  // custom_properties.program.{title,sub_title,description} (plus
  // status/file paths/poster logo) from whatever EPG programme was
  // actually airing on this channel at this time, and sending an explicit
  // title/id on create *replaces* that entirely rather than merging,
  // which would throw away the richer data for what's normally an exact
  // match anyway (Kodi's "record from guide" title already came from
  // that same EPG programme).
  //
  // isEpgBased DOES now send a title/id-free custom_properties.program
  // window (added 2026-09-29, fixing a real, confirmed, live-verified
  // bug -- see dispatcharr::BuildOneTimeRecordingCreateBody()'s own
  // comment, TimerRequestBuilder.h, for the full account, including why
  // this doesn't reintroduce the enrichment-replacement risk the
  // paragraph above warns about: confirmed live, not just from source,
  // that this narrower shape merges rather than replaces). The body
  // itself lives in dispatcharr::BuildOneTimeRecordingCreateBody()
  // (TimerRequestBuilder.h) so it's unit-testable standalone -- see that
  // function's own comment for the real, confirmed, previously-
  // undocumented dependency it also closes: Kodi's own instant recording
  // sends `start` as a literal 0, not the real current time.
  json body = dispatcharr::BuildOneTimeRecordingCreateBody(channelId, start, end, time(nullptr), isEpgBased);
  json response;
  if (!Request("POST", kRecordingsPath, body, response, error))
    return false;

  // See PendingTitle's comment: cache the title Kodi already gave us (from
  // the EPG tag the "Record" button was pressed on) so GetRecordings() can
  // show it immediately instead of "Recording <id>" while Dispatcharr's own
  // async enrichment catches up. Only useful for the EPG-matched case --
  // title is empty for a fully manual time range with nothing airing.
  // Keyed by the real recording id this response carries (RecordingSerializer
  // uses fields="__all__", which always includes it) -- not channelId, see
  // PendingTitle's own comment for the real bug that fixes.
  int recordingId = FieldOr<int>(response, "id", 0);
  if (!title.empty() && recordingId > 0)
  {
    std::lock_guard<std::mutex> lock(m_pendingTitlesMutex);
    m_pendingTitles.push_back({recordingId, title, std::chrono::steady_clock::now()});
  }
  return true;
}

bool DispatcharrClient::UpdateOneTimeRecording(int recordingId, time_t start, time_t end, int channelId,
                                               std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Both fields always included -- confirmed live that a PATCH omitting
  // them crashes server-side (see this method's own header comment). The
  // body itself lives in dispatcharr::BuildOneTimeRecordingPatchBody()
  // (TimerRequestBuilder.h) so that invariant is unit-tested.
  json body = dispatcharr::BuildOneTimeRecordingPatchBody(start, end, channelId);
  json response;
  return Request("PATCH", std::string(kRecordingsPath) + std::to_string(recordingId) + "/", body, response, error);
}

std::string DispatcharrClient::ResolveSeriesRuleTvgId(int epgDataId, const std::string& fallbackTvgId)
{
  if (epgDataId <= 0)
    return fallbackTvgId;

  std::string error;
  if (!EnsureAuthenticated(error))
    return fallbackTvgId;

  json response;
  std::string path = std::string(kEpgDataPath) + std::to_string(epgDataId) + "/";
  if (!Request("GET", path, json(), response, error))
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: ResolveSeriesRuleTvgId: lookup failed for epg_data %d: %s",
              epgDataId, error.c_str());
    return fallbackTvgId;
  }
  std::string resolvedTvgId = FieldOr<std::string>(response, "tvg_id", "");
  return resolvedTvgId.empty() ? fallbackTvgId : resolvedTvgId;
}

bool DispatcharrClient::CreateSeriesRule(int channelId, const std::string& tvgId, const std::string& titlePattern,
                                         bool recordNewOnly, const std::string& titleMode,
                                         const std::string& description, const std::string& descriptionMode,
                                         bool untaggedIsNew, int epgSourceId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Serializes this whole POST (and the evaluate/ call right after it)
  // against SetDvrOffsetMinutes()'s own GET-merge-PATCH on the shared
  // dvr_settings row -- fix for a real, confirmed race found via a
  // project-wide review (a 27th-pass audit), confirmed against
  // Dispatcharr's own real current upstream source, not itself
  // independently reproduced: Dispatcharr stores its own series-rule
  // list inside that exact same dvr_settings value (`CoreSettings.
  // get_dvr_series_rules()`/`set_dvr_series_rules()`), but reaches it
  // through a genuinely different write path than this addon's own
  // padding PATCH -- `SeriesRulesAPIView`'s own POST/DELETE calls
  // `CoreSettings._update_group()` directly (a server-side
  // read-merge-save of just the `series_rules` key, confirmed against
  // its own source), never going through the generic CoreSettings PATCH
  // endpoint `SetDvrOffsetMinutes()` itself uses. If this addon's own
  // padding push is mid-flight (past its own GET, not yet at its own
  // PATCH) at the exact moment this call lands its series-rule create
  // server-side, the padding push's own stale, pre-create copy of
  // `series_rules` still gets PATCHed back at the whole-value-replace
  // generic endpoint, silently undoing the create that just happened in
  // between. This only closes the addon-internal half of that race (the
  // same account's own web UI, or a second Kodi install, can still race
  // this from outside -- the endpoint has no ETag/If-Match support to
  // close that half from any client).
  std::lock_guard<std::mutex> lock(m_dvrSettingsMutex);

  // Confirmed against the live SeriesRuleRequest schema: "title" and
  // "channel_id" (not "title_pattern"/"channel"). tvg_id is genuinely
  // optional ("omit to match across all channels") so it's only sent when
  // non-empty rather than risking an empty string being read as an
  // explicit "match only channels with a blank tvg_id" filter. "mode"
  // defaults server-side to "all" (every matching episode, including
  // reruns); only sent explicitly when "new" (first-run only) is wanted.
  // channel_id is genuinely optional too, the same as tvg_id just above
  // -- confirmed against Dispatcharr's own real current upstream source
  // (an 18th-pass audit cloned it into a scratchpad, never committed to
  // this repo): its own serializer schema documents "Optional channel to
  // pin recordings to (defaults to lowest-numbered channel for the
  // EPG)", and its own validation rejects any non-empty value that isn't
  // a real channel id with a 400 ("channel_id does not exist"). This
  // addon's own "no pinned channel" convention is channelId <= 0 (see
  // PVR_CHANNEL_INVALID_UID's own use throughout this file/PVRDispatcharr.cpp)
  // -- a real, confirmed bug fixed here (found via a project-wide
  // review): sending that sentinel literally as channel_id (e.g. -1)
  // instead of omitting the key entirely always tripped that same 400,
  // so editing (or, in principle, creating) a channel-less series rule
  // -- reachable in practice via Dispatcharr's own "Record series" Guide
  // button, which creates one without a pinned channel -- failed
  // outright. The body itself lives in
  // dispatcharr::BuildSeriesRuleRequestBody() (TimerRequestBuilder.h) so
  // it's unit-testable standalone -- see that function's own comment.
  json body = dispatcharr::BuildSeriesRuleRequestBody(channelId, tvgId, titlePattern, recordNewOnly, titleMode,
                                                      description, descriptionMode, untaggedIsNew, epgSourceId);
  json response;
  if (!Request("POST", kSeriesRulesPath, body, response, error))
    return false;

  // Ask Dispatcharr to evaluate the new rule immediately so upcoming
  // recordings show up right away rather than waiting for its own
  // background scheduler.
  std::string evalError;
  json evalBody = tvgId.empty() ? json::object() : json{{"tvg_id", tvgId}};
  json evalResponse;
  Request("POST", std::string(kSeriesRulesPath) + "evaluate/", evalBody, evalResponse, evalError);
  // A failed evaluate call is non-fatal: the rule was still created.
  return true;
}

bool DispatcharrClient::DeleteSeriesRule(const std::string& title, const std::string& tvgId, int epgSourceId,
                                         std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  // Same race protection as CreateSeriesRule()'s own comment -- see
  // there for the full account.
  std::lock_guard<std::mutex> lock(m_dvrSettingsMutex);
  // Confirmed against the live schema: series rules are deleted by
  // title + tvg_id query params, not a path id -- there is no
  // /api/channels/series-rules/{id}/ route. The query-string assembly
  // itself lives in dispatcharr::BuildSeriesRuleDeleteQuery()
  // (TimerRequestBuilder.h) so it's unit-testable standalone.
  std::string path = std::string(kSeriesRulesPath) + dispatcharr::BuildSeriesRuleDeleteQuery(title, tvgId, epgSourceId);
  json response;
  return Request("DELETE", path, json(), response, error);
}

bool DispatcharrClient::GetRecurringRules(std::vector<RecurringRule>& out, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kRecurringRulesPath, json(), response, error))
    return false;

  // Same truncated-pagination guard as GetChannels() -- see
  // dispatcharr::IsTruncatedPaginatedResponse()'s own comment.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "recurring-rules response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response);
  if (!list.is_array())
  {
    error = "Unexpected recurring-rules response shape";
    return false;
  }

  // The field-mapping logic itself lives in dispatcharr::ParseRecurringRuleJson()
  // (TimerRuleParser.{h,cpp}) so it's unit-testable standalone -- see that
  // function's own comment.
  out.clear();
  for (const auto& item : list)
    out.push_back(ParseRecurringRuleJson(item));
  return true;
}

bool DispatcharrClient::CreateRecurringRule(int channelId, const std::string& name, const std::vector<int>& daysOfWeek,
                                            int startTimeOfDaySeconds, int endTimeOfDaySeconds, time_t startDate,
                                            time_t endDate, bool enabled, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;

  // Confirmed against the live RecurringRecordingRuleSerializer: channel is
  // a plain FK id (no uuid field on this model, unlike Channel itself),
  // days_of_week a non-empty list of ints 0-6, start_time/end_time plain
  // "HH:MM:SS" with no timezone suffix, and start_date/end_date are both
  // required despite the model declaring them nullable -- confirmed by its
  // validate() raising "Start date is required"/"End date is required"
  // when either is omitted, so both are always sent here.
  json body = {
      {"channel", channelId},
      {"name", name},
      {"days_of_week", daysOfWeek},
      {"start_time", TimeOfDayString(startTimeOfDaySeconds)},
      {"end_time", TimeOfDayString(endTimeOfDaySeconds)},
      {"start_date", DateStringFromTime(startDate)},
      {"end_date", DateStringFromTime(endDate)},
      {"enabled", enabled},
  };
  json response;
  return Request("POST", kRecurringRulesPath, body, response, error);
}

bool DispatcharrClient::UpdateRecurringRule(int ruleId, const dispatcharr::RecurringRuleEditPatch& patch,
                                            std::string& error, long* httpStatusOut)
{
  if (httpStatusOut)
    *httpStatusOut = 0;
  if (!EnsureAuthenticated(error))
    return false;

  // Only what changed, and no end_date unless the patch gives an open-ended rule
  // one -- see this method's own header comment and
  // dispatcharr::BuildRecurringRuleUpdateBody() (TimerRequestBuilder.h), which
  // keeps those invariants unit-tested rather than resting on a future edit not
  // breaking them.
  json body = dispatcharr::BuildRecurringRuleUpdateBody(patch);
  json response;
  return Request("PATCH", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", body, response, error,
                 /*withAuth=*/true, /*retryOnAuthFailure=*/1, httpStatusOut);
}

bool DispatcharrClient::GetRecurringRuleById(int ruleId, RecurringRule& out, std::string& error, long* httpStatusOut)
{
  if (httpStatusOut)
    *httpStatusOut = 0;
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", json(), response, error,
               /*withAuth=*/true, /*retryOnAuthFailure=*/1, httpStatusOut))
    return false;
  if (!response.is_object())
  {
    error = "Unexpected recurring-rule response shape";
    return false;
  }
  out = ParseRecurringRuleJson(response);
  return true;
}

bool DispatcharrClient::DeleteRecurringRule(int ruleId, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  long httpStatus = 0;
  if (Request("DELETE", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", json(), response, error,
              /*withAuth=*/true, /*retryOnAuthFailure=*/1, &httpStatus))
    return true;
  // A 404 means the rule is already gone (added 2026-09-27, a 49th-pass
  // audit, fixing a real, confirmed bug found via a project-wide review,
  // not itself independently reproduced) -- see DeleteRecording()'s own
  // comment above for the identical reasoning.
  return httpStatus == 404;
}

bool DispatcharrClient::ExtendRecurringRuleEndDate(int ruleId, time_t newEndDate, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json body = {{"end_date", DateStringFromTime(newEndDate)}};
  json response;
  return Request("PATCH", std::string(kRecurringRulesPath) + std::to_string(ruleId) + "/", body, response, error);
}

bool DispatcharrClient::FindCoreSettingsRow(const std::string& key, int& idOut, json& valueOut, std::string& error,
                                            bool* valueWasObjectOut)
{
  if (!EnsureAuthenticated(error))
    return false;

  json response;
  if (!Request("GET", kCoreSettingsPath, json(), response, error))
    return false;

  // Confirmed against a live instance: a bare array, not {"results": [...]}
  // -- but tolerate that wrapper too, matching this client's usual
  // defensive style for list endpoints (see GetChannels()'s own comment).
  // Same truncated-pagination guard as GetChannels() -- see
  // dispatcharr::IsTruncatedPaginatedResponse()'s own comment. Harmless
  // here in practice (confirmed live this endpoint returns a bare
  // array), but cheap and consistent to check regardless.
  if (IsTruncatedPaginatedResponse(response))
  {
    error = "/api/core/settings/ response is paginated with more pages than this addon currently follows";
    return false;
  }
  const json& list = UnwrapListResponse(response);
  if (!list.is_array())
  {
    error = "Unexpected /api/core/settings/ response shape";
    return false;
  }

  // The per-row matching itself lives in dispatcharr::FindSettingsRowByKey()
  // (JsonFieldUtil.h) so it's unit-testable standalone -- see that
  // function's own comment, including why valueWasObjectOut matters
  // beyond this function's own read-only callers.
  SettingsRowMatch match = FindSettingsRowByKey(list, key);
  if (!match.found)
  {
    error = "Dispatcharr has no " + key + " row in /api/core/settings/";
    return false;
  }
  if (match.id == 0)
  {
    error = "Dispatcharr's " + key + " row in /api/core/settings/ has no id";
    return false;
  }
  idOut = match.id;
  valueOut = match.value;
  if (valueWasObjectOut)
    *valueWasObjectOut = match.valueWasObject;
  return true;
}

bool DispatcharrClient::GetDvrOffsetMinutes(int& preMinutesOut, int& postMinutesOut, std::string& error)
{
  // Under the same mutex as the writes, so a read never lands between a push's own GET and its
  // PATCH and reports the value the PATCH is about to replace.
  std::lock_guard<std::mutex> lock(m_dvrSettingsMutex);
  int id = 0;
  json value;
  if (!FindCoreSettingsRow(kDvrSettingsKey, id, value, error))
    return false;
  preMinutesOut = FieldOr(value, "pre_offset_minutes", 0);
  postMinutesOut = FieldOr(value, "post_offset_minutes", 0);
  return true;
}

bool DispatcharrClient::SetDvrOffsetMinutes(const int* preMinutes, const int* postMinutes, std::string& error,
                                            const std::function<void(bool)>& onResult)
{
  // Serializes this whole GET-merge-PATCH against another concurrent
  // call -- see m_dvrSettingsMutex's own comment (DispatcharrClient.h)
  // for the real regression this fixes: a settings-dialog save that
  // changes both offsets at once fires two of these calls back to back,
  // and without this lock, each one's own "preserve the other key" merge
  // could be built from a value already stale by the time it's sent,
  // silently losing whichever edit's PATCH lands first. The caller's
  // result callback runs under the same lock, see the declaration.
  std::lock_guard<std::mutex> lock(m_dvrSettingsMutex);
  const bool ok = SetDvrOffsetMinutesLocked(preMinutes, postMinutes, error);
  if (onResult)
    onResult(ok);
  return ok;
}

bool DispatcharrClient::SetDvrOffsetMinutesChosen(
    const std::function<void(std::optional<int>& pre, std::optional<int>& post)>& choose, std::string& error,
    const std::function<void(bool)>& onResult)
{
  std::lock_guard<std::mutex> lock(m_dvrSettingsMutex);
  std::optional<int> pre, post;
  choose(pre, post);
  if (!pre && !post)
    return true;
  int preValue = pre.value_or(0), postValue = post.value_or(0);
  const bool ok = SetDvrOffsetMinutesLocked(pre ? &preValue : nullptr, post ? &postValue : nullptr, error);
  if (onResult)
    onResult(ok);
  return ok;
}

bool DispatcharrClient::SetDvrOffsetMinutesLocked(const int* preMinutes, const int* postMinutes, std::string& error)
{
  int id = 0;
  json value;
  bool valueWasObject = false;
  if (!FindCoreSettingsRow(kDvrSettingsKey, id, value, error, &valueWasObject))
    return false;
  // Fix for a real, confirmed gap: a dvr_settings row whose real "value"
  // wasn't a JSON object (however that happened server-side) used to be
  // silently treated as an empty {} here, which the merge below would
  // then PATCH straight back -- wiping every other key the real value
  // actually held, the same incident MergeDvrOffsetMinutes() below
  // exists to prevent, just reached via a different path (the value
  // being discarded before the merge function ever saw it, rather than
  // the merge itself dropping keys). Refuse rather than risk it.
  if (!valueWasObject)
  {
    error = "Dispatcharr's dvr_settings row has an unexpected (non-object) value -- refusing to overwrite it";
    return false;
  }

  // The merge itself lives in dispatcharr::MergeDvrOffsetMinutes()
  // (JsonFieldUtil.h) so it's unit-testable standalone -- see that
  // function's own comment.
  json body = {{"value", MergeDvrOffsetMinutes(value, preMinutes, postMinutes)}};
  json response;
  return Request("PATCH", std::string(kCoreSettingsPath) + std::to_string(id) + "/", body, response, error);
}

bool DispatcharrClient::GetSystemTimeZone(std::string& timeZoneOut, std::string& error)
{
  int id = 0;
  json value;
  if (!FindCoreSettingsRow(kSystemSettingsKey, id, value, error))
    return false;
  timeZoneOut = FieldOr<std::string>(value, "time_zone", "");
  if (timeZoneOut.empty())
  {
    error = "system_settings row has no time_zone value";
    return false;
  }
  return true;
}

void DispatcharrClient::DeferAuthenticationAfterUnresponsiveServer(const std::string& reason)
{
  // The flow mutex first, as EnsureAuthenticated() takes them, so a login already in progress finishes (and
  // sets its own state) before this overwrites the cooldown.
  std::lock_guard<std::mutex> flowLock(m_authFlowMutex);
  std::lock_guard<std::mutex> stateLock(m_authStateMutex);
  if (!m_accessToken.empty())
    return; // already authenticated: nothing to defer
  m_transientLoginFailedAt = std::chrono::steady_clock::now();
  m_transientLoginArmedByShortAttempt =
      false; // everyone waits: this exists to keep Kodi's own threads off a dead server
  m_lastLoginError = reason;
}

bool DispatcharrClient::GetServerVersion(std::string& versionOut, std::string& error, long* httpStatusOut)
{
  json response;
  // withAuth=false: confirmed AllowAny live, and this needs to work even
  // when login itself has failed (Kodi's PVR info screen still asks for a
  // backend version regardless).
  // A short limit: this is the first request at startup, and against a server that accepts connections and never
  // answers it held Kodi's instance creation for a whole timeout (then the login for another one).
  if (!Request("GET", kVersionPath, json(), response, error, /*withAuth=*/false, /*retryOnAuthFailure=*/0,
               httpStatusOut, ShortRequestTimeoutMs()))
    return false;
  versionOut = FieldOr<std::string>(response, "version", "");
  if (versionOut.empty())
  {
    error = "version response has no version value";
    return false;
  }
  return true;
}

bool DispatcharrClient::GetSupportedTimezones(std::vector<std::string>& timezonesOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  if (!Request("GET", kTimezonesPath, json(), response, error))
    return false;
  const json& list = response.contains("timezones") ? response["timezones"] : json();
  if (!list.is_array())
  {
    error = "Unexpected timezones response shape";
    return false;
  }
  timezonesOut.clear();
  for (const auto& item : list)
  {
    if (item.is_string())
      timezonesOut.push_back(item.get<std::string>());
  }
  return true;
}

bool DispatcharrClient::IsCurrentUserAdmin(bool& isAdminOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  if (!Request("GET", kCurrentUserPath, json(), response, error))
    return false;
  if (!response.contains("user_level"))
  {
    error = "user/me response has no user_level value";
    return false;
  }
  isAdminOut = FieldOr(response, "user_level", 0) >= 10;
  return true;
}

bool DispatcharrClient::GetCurrentUserDvrAccess(DvrAccess& accessOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  if (!Request("GET", kCurrentUserPath, json(), response, error))
    return false;
  if (!ParseDvrAccessFromUserJson(response, accessOut))
  {
    error = "user/me response has no usable user_level value";
    return false;
  }
  return true;
}

bool DispatcharrClient::IsCatchupEnabledGlobally(bool& enabledOut, std::string& error)
{
  int id = 0;
  json value;
  if (!FindCoreSettingsRow(kSystemSettingsKey, id, value, error))
    return false;
  enabledOut = FieldOr(value, "catchup_enabled", true);
  return true;
}

bool DispatcharrClient::IsCatchupEnabledForCurrentUser(bool& enabledOut, std::string& error)
{
  if (!EnsureAuthenticated(error))
    return false;
  json response;
  if (!Request("GET", kCurrentUserPath, json(), response, error))
    return false;
  const json& customProperties = response.contains("custom_properties") ? response["custom_properties"] : json();
  enabledOut = FieldOr(customProperties, "catchup_enabled", true);
  return true;
}

bool DispatcharrClient::ComputeKnownZoneOffsetMinutes(const std::string& ianaZoneName, time_t nowUtc,
                                                      int& offsetMinutesOut, bool applyZoneRuleChanges)
{
  // Delegates to the free function in TimeZoneUtil.{h,cpp} -- pulled out
  // specifically so this logic (zero Kodi/curl dependency) is unit-testable
  // standalone; see tests/test_timezone_util.cpp. This static method stays
  // as the public entry point since PVRDispatcharr's constructor already
  // calls it via this exact name.
  return dispatcharr::ComputeKnownZoneOffsetMinutes(ianaZoneName, nowUtc, offsetMinutesOut, applyZoneRuleChanges);
}

void* DispatcharrClient::AppendApiKeyHeaderIfPresent(void* headers, const std::string& apiKey)
{
  if (apiKey.empty())
    return headers;
  std::string apiKeyHeader = "X-API-Key: " + apiKey;
  return curl_slist_append(static_cast<struct curl_slist*>(headers), apiKeyHeader.c_str());
}

namespace
{
int XferInfoCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  return TransferAbortResult(static_cast<const std::atomic<bool>*>(clientp));
}
} // namespace

void DispatcharrClient::ApplyStandardCurlOptions(void* curlPtr, void* share) const
{
  CURL* curl = static_cast<CURL*>(curlPtr);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_config.verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_config.verifySsl ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(m_config.timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(share));
  // A libcurl built without its threaded resolver times out a name lookup
  // with SIGALRM, which is unsafe outside the main thread -- and this addon
  // calls curl from its own refresh threads and Kodi's read threads alike.
  // A no-op for a threaded-resolver build.
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // Abortable while the instance is shutting down -- see AbortInFlightRequests().
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &XferInfoCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_abortRequests);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

bool DispatcharrClient::FetchRawInProgressPlaylist(int recordingId, const std::string& playlistUrl,
                                                   std::string& playlistText, std::string& error, bool* wasNotFoundOut,
                                                   long* httpStatusOut, long timeoutMsOverride)
{
  if (wasNotFoundOut)
    *wasNotFoundOut = false;
  if (httpStatusOut)
    *httpStatusOut = 0;
  // Two attempts: the playlist fetch itself can self-heal on a 401, same
  // pattern as OpenRecordingStream().
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      error = "Failed to initialise libcurl";
      return false;
    }

    struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));

    playlistText.clear();
    BoundedStringSink playlistSink{&playlistText, kMaxPlaylistResponseBytes};
    curl_easy_setopt(curl, CURLOPT_URL, playlistUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, BoundedWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &playlistSink);
    ApplyStandardCurlOptions(curl, GetCurlShare());
    if (timeoutMsOverride > 0)
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMsOverride);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
      error = playlistSink.allocationFailed ? "Ran out of memory receiving the in-progress playlist"
              : playlistSink.exceeded
                  ? "The in-progress playlist exceeded the " + std::to_string(kMaxPlaylistResponseBytes >> 20) +
                        " MiB limit this addon accepts"
                  : std::string("HTTP request failed fetching playlist: ") + curl_easy_strerror(res);
      return false;
    }
    if (httpStatusOut)
      *httpStatusOut = httpCode;

    if (httpCode == 401 && attempt == 0)
    {
      std::string regenKey, regenError;
      if (ObtainApiKey(regenKey, regenError))
        continue;
    }

    if (httpCode < 200 || httpCode >= 300)
    {
      // A 3xx here (curl doesn't follow it, no CURLOPT_FOLLOWLOCATION set
      // on this handle) means the same thing as a 404 does for this
      // specific request: Dispatcharr's own HLS directory is gone. See
      // this function's own header comment for why a natural completion
      // takes this path instead of a 404.
      if (wasNotFoundOut)
        *wasNotFoundOut = (httpCode == 404) || (httpCode >= 300 && httpCode < 400);
      error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " fetching in-progress playlist";
      return false;
    }
    return true;
  }
  return false;
}

namespace
{
// A generous per-segment ceiling for a single in-progress-recording HLS
// segment's probed byte size (added 2026-09-27, a 53rd-pass audit,
// fixing a real, confirmed crash risk found via a project-wide review,
// not reproduced live -- escalates docs/OPEN_ITEMS.md's own 52nd-pass
// entry on this same unbounded value, found to be a real crash, not just
// a theoretical overflow-when-summed risk): this value flows straight
// into ReadInProgressRecordingStream()'s own single-segment fetch for a
// single segment, unsummed -- so a single absurd Content-Length response
// (this addon's own configured Dispatcharr server misbehaving, or a
// proxy in front of it) risked an unbounded allocation there, uncaught
// by anything on this call path all the way up through Kodi's own
// ADDON_ReadRecordedStream dev-kit wrapper (confirmed against Kodi's own
// real current SDK source, PVR.h -- a plain, try/catch-free passthrough),
// aborting the whole Kodi process from what looks like an ordinary
// playback read. (Updated 2026-09-27, a 56th-pass audit: at the time
// this ceiling was first added, that fetch really was a plain
// `body.reserve(static_cast<size_t>(segByteSize))` into an unbounded
// std::string -- a 55th-pass audit later replaced that mechanism
// entirely with a FixedBufferSink bounded to this same ceiling, so this
// comment no longer describes the current code; see that fetch's own
// comment for the current mechanism. The risk this ceiling itself
// exists to bound is unchanged either way.) Deliberately much smaller
// than LiveManifestParser's own
// 1 TiB byteSize ceiling (a different file, different producer, and
// mainly guarding against a cumulative-sum overflow rather than a single
// allocation) -- no real HLS TS segment (a few seconds of live TV)
// is ever legitimately anywhere near even this size at any realistic
// bitrate. Corrected 2026-09-27, a 55th-pass audit: this comment's own
// prior wording claimed a real segment is "confirmed well under 50MB
// even at 100 Mbps" -- Dispatcharr's own DVR ffmpeg command uses
// `-hls_time 4` with `-c copy` (confirmed against its real current
// upstream source), and 4s at 100 Mbps is already ~48MB before even
// accounting for `-c copy` cutting at the next keyframe, not exactly at
// 4s -- not actually "well under" at all, and nothing in this project's
// own docs supports "confirmed" at that specific bitrate (the only real
// bitrate documented elsewhere is ~14 Mbps, GetStreamReadChunkSize()'s
// own comment). 64 MiB is still a generous multiple of any bitrate this
// project has actually confirmed live, just not for the reason
// originally claimed.
//
// Lowered from an original 512 MiB to 64 MiB (fixed 2026-09-27, a
// 54th-pass audit, fixing a real, confirmed residual risk the original
// ceiling itself still left open, found via a project-wide review,
// confirmed against this project's own docs/BUILDING.md, not itself
// independently reproduced): this project's own primary real target
// platform, CoreELEC on an ODROID N2+, runs a 32-bit armhf userland
// despite the SoC itself being 64-bit (confirmed: the finished `.so` is
// `ELF 32-bit LSB shared object, ARM, EABI5`) -- a roughly 3GB, often
// heavily fragmented 32-bit virtual address space, where the fetch
// buffer's own single contiguous allocation (corrected 2026-09-27, a
// 58th-pass audit, fixing a real, confirmed incompleteness in a
// 56th-pass audit's own "reserve()"-to-"allocation" wording fix, found
// via a project-wide review, not itself independently reproduced: that
// pass fixed one nearby "reserve() call" reference but missed this one)
// could still throw even at 512 MiB, the exact crash this ceiling
// exists to prevent, just needing a smaller trigger value than an
// unbounded one did -- worth noting since a 56th-pass audit's own fix
// means up to two such buffers (the outgoing and incoming segment) can
// be live at once transiently, not just one (see
// ReadInProgressRecordingStream()'s own comment). 64 MiB remains a very
// generous multiple of any real segment's own actual size.
// Halved on a 32-bit address space, where a 64 MiB zero-filled buffer (two can be live at once) is the
// allocation that fails first -- the other three response ceilings are lowered there too, see
// CurlCallbacks.h (2026-10-04 third hardening sweep).
constexpr int64_t kMaxProbedSegmentByteSize = kIs64BitAddressSpace ? 67108864 : 33554432; // 64 / 32 MiB
} // namespace

int64_t DispatcharrClient::ProbeSegmentByteSize(const std::string& segmentUrl, long timeoutMsOverride) const
{
  // A playlist is server-supplied text. RebaseRecordingSegmentUrl() already points the
  // recording's own segment path at the configured address; a URL that is still not on it
  // names some other place (another host, another port) and is never sent the X-API-Key.
  // It is not requested at all: the segment counts as one that cannot be sized, which the
  // unprobeable-segment handling turns into a logged, zero-byte placeholder.
  if (!IsOnConfiguredServer(segmentUrl))
  {
    // Once per client at warning level: the probe is retried every refresh until the
    // unprobeable-segment handling gives up on the segment, and a line each time is a flood.
    kodi::Log(m_loggedForeignSegment.exchange(true) ? ADDON_LOG_DEBUG : ADDON_LOG_WARNING,
              "pvr.dispatcharr-unofficial: not requesting a recording segment that the playlist places outside the "
              "configured server (the API key is only ever sent there)");
    return -1;
  }
  CURL* curl = curl_easy_init();
  if (!curl)
    return -1;

  struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));

  int64_t totalLength = -1;
  curl_easy_setopt(curl, CURLOPT_URL, segmentUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ContentLengthHeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &totalLength);
  // GetProbeCurlShare(), not GetCurlShare() -- this is the one call site
  // invoked concurrently in a tight same-host burst (see
  // m_probeCurlShareState's own comment for why that specifically needs a
  // connection-cache-free share).
  ApplyStandardCurlOptions(curl, GetProbeCurlShare());
  if (timeoutMsOverride > 0)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMsOverride);

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || httpCode != 200)
    return -1;
  // Clamped, not just left to the caller's own `segSize <= 0` failed-probe
  // check -- that check only screens out a non-positive value, and a
  // genuinely huge-but-positive one is exactly what kMaxProbedSegmentByteSize's
  // own comment above documents the real risk of. Never touches the -1
  // sentinel (already returned above, well before this point).
  if (totalLength > kMaxProbedSegmentByteSize)
    return kMaxProbedSegmentByteSize;
  return totalLength;
}

void DispatcharrClient::MaybeSendInProgressHlsKeepAlive()
{
  auto now = std::chrono::steady_clock::now();
  uint64_t session;
  std::string segmentUrl;
  int64_t unreadBytes;
  {
    // Decided and claimed under the state lock; the request itself runs without it (see m_inProgressStateMutex).
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    const auto& stream = m_inProgressRecordingStream;
    // No segment known yet means nothing to point the request at, and also
    // nothing unread to protect.
    if (!stream.open || stream.segments.empty())
      return;
    if (!ShouldSendHlsViewerKeepAlive(stream.finished || stream.hlsKeepAliveGone, stream.position, stream.totalBytes,
                                      stream.nextHlsKeepAliveAt, now))
      return;

    // Copied out, not held by reference across the blocking request below --
    // same reasoning as ReadInProgressRecordingStream()'s own copy of a
    // segment's fields.
    session = m_inProgressSession;
    segmentUrl = stream.segments.back().url;
    // Never to another origin; see ProbeSegmentByteSize().
    if (!IsOnConfiguredServer(segmentUrl))
      return;
    unreadBytes = stream.totalBytes - stream.position;
    // Claimed before the request, so a second thread arriving meanwhile does not send one too (and so a handle
    // that cannot be created retries shortly); the outcome below replaces it.
    m_inProgressRecordingStream.nextHlsKeepAliveAt = NextHlsKeepAliveDueAt(now, HlsKeepAliveOutcome::kRetryLater);
  }

  CURL* curl = curl_easy_init();
  if (!curl)
    return;

  struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));
  // HEAD, like ProbeSegmentByteSize()'s own probe of the same URL space --
  // see HlsViewerKeepAlive.h for why that's enough to refresh the
  // server's viewer key without downloading a segment.
  curl_easy_setopt(curl, CURLOPT_URL, segmentUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  ApplyStandardCurlOptions(curl, GetCurlShare());
  // The stream has segments (checked above), so this is steady state: see ShortRequestTimeoutMs().
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, ShortRequestTimeoutMs());

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  HlsKeepAliveOutcome outcome = ClassifyHlsKeepAliveResponse(res == CURLE_OK, httpCode);
  // Scheduled from when the request finished, not from `now` above -- the
  // server's own key lapses a fixed TTL after it handled the request, so
  // measuring from before a slow round trip would schedule the next one
  // too late by however long this one took.
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    if (m_inProgressRecordingStream.open && m_inProgressSession == session)
    {
      m_inProgressRecordingStream.nextHlsKeepAliveAt = NextHlsKeepAliveDueAt(std::chrono::steady_clock::now(), outcome);
      if (outcome == HlsKeepAliveOutcome::kGone)
        m_inProgressRecordingStream.hlsKeepAliveGone = true;
    }
  }

  kodi::Log(outcome == HlsKeepAliveOutcome::kGone ? ADDON_LOG_WARNING : ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: MaybeSendInProgressHlsKeepAlive: HEAD on the newest known segment "
            "(%lld unread byte(s)) -> curlResult=%d httpCode=%ld (%s)",
            static_cast<long long>(unreadBytes), static_cast<int>(res), httpCode,
            outcome == HlsKeepAliveOutcome::kRefreshed
                ? "refreshed"
                : (outcome == HlsKeepAliveOutcome::kGone
                       ? "the server has already removed this recording's HLS directory, no further keep-alives"
                       : "will retry shortly"));
}

bool DispatcharrClient::RefreshInProgressRecordingManifest(bool force, std::string& error, bool* coldStartRetryableOut,
                                                           bool coldStart)
{
  if (coldStartRetryableOut)
    *coldStartRetryableOut = false;

  // One refresh at a time, exactly as in RefreshLiveManifest() (see m_inProgressRefreshMutex): two overlapping
  // refreshes both appended the same new segments to the recording's byte stream -- reproduced live with a
  // second thread forcing refreshes, a segment listed twice.
  std::unique_lock<std::mutex> refreshLock(m_inProgressRefreshMutex, std::defer_lock);
  if (force)
    refreshLock.lock();
  else if (!refreshLock.try_lock())
    return true;

  // Held for the state reads and writes below, released around every network call (see
  // m_inProgressStateMutex), and the stream is re-checked after each re-lock.
  std::unique_lock<std::mutex> stateLock(m_inProgressStateMutex);
  if (!m_inProgressRecordingStream.open)
  {
    error = "no in-progress recording stream is open";
    return false;
  }

  // Nothing left to ask once the server has confirmed this recording's
  // content gone: every request would only be answered with another 404
  // (found live: ~7.8/sec for as long as Kodi kept the stream open, ~100s).
  // Checked before anything else, including the throttle -- the failure
  // paths below never update lastManifestFetch, so the throttle alone never
  // slowed this down.
  if (m_inProgressRecordingStream.contentGone)
  {
    error = "this recording's HLS content is gone on the server";
    return false;
  }

  // Before the throttle below, not after: a paused viewer reaches this
  // function through GetStreamTimes()'s own polling, which is the only
  // thing still calling into this addon during a pause -- see
  // HlsViewerKeepAlive.h. It keeps its own schedule, so running it ahead
  // of the throttle costs nothing on a call where none is due.
  stateLock.unlock();
  MaybeSendInProgressHlsKeepAlive();
  stateLock.lock();
  if (!m_inProgressRecordingStream.open)
  {
    error = "no in-progress recording stream is open";
    return false;
  }
  const uint64_t session = m_inProgressSession;
  const int recordingId = m_inProgressRecordingStream.recordingId;
  auto stillSameStream = [&]() { return m_inProgressRecordingStream.open && m_inProgressSession == session; };

  // Throttle for the same reason as RefreshLiveManifest(): a tight
  // catch-up-loop/demux-read cycle can call this far more often than the
  // recording could possibly have grown.
  constexpr auto kMinRefreshInterval = std::chrono::milliseconds(500);
  auto now = std::chrono::steady_clock::now();
  // The throttle check itself lives in dispatcharr::ShouldThrottleRefresh()
  // (Staleness.h) so it's unit-testable standalone -- see that function's
  // own comment.
  if (ShouldThrottleRefresh(force, m_inProgressRecordingStream.lastManifestFetch, now, kMinRefreshInterval))
    return true;

  // Timing breakdown for diagnosing "seek to live took ~10s"-type reports:
  // this one call does up to three separate HTTP round trips (playlist
  // fetch, one ranged-GET probe per newly-discovered segment, and an
  // unconditional GetRecordingById() call below just to re-check this
  // one recording's isInProgress flag), any of which could plausibly
  // dominate depending on network conditions -- log each piece rather
  // than guessing which one matters.
  auto refreshStart = std::chrono::steady_clock::now();

  // Fresh in-progress check every call, not just at open -- this is what
  // lets ReadInProgressRecordingStream() eventually stop waiting for a
  // recording that's actually finished, and CanPauseStream()/
  // IsRealTimeStream() reflect current reality rather than whatever was
  // true when the stream was opened. Deliberately BEFORE the playlist
  // fetch below, not after -- a real, confirmed bug this ordering fixes
  // (found via code reading): probing thousands of already-elapsed
  // segments on a cold open was
  // confirmed live to take ~29s (see the probe loop's own comment below),
  // more than enough window for the recording to finish server-side
  // *after* this cycle's own playlist snapshot was taken but *before*
  // this status check used to run. ResolveInProgressFinished() would
  // then correctly compute finished=true from that now-stale snapshot --
  // every segment actually present in it did probe successfully, so
  // allProbesSucceeded is also true, there being no failed probe for a
  // segment nobody yet knew existed -- stranding whatever Dispatcharr
  // wrote after the snapshot as far as THIS read is concerned:
  // ReadInProgressRecordingStream() itself never calls this function
  // again once finished is true (see its own EOF short-circuit) --
  // although GetInProgressRecordingStreamLength()/DurationMs() do keep
  // calling this unconditionally regardless of `finished` for as long as
  // Kodi keeps polling them, so a late segment isn't necessarily lost
  // forever, just too late for a read that's already hit EOF and likely
  // already had its demuxer torn down by Kodi in response. Checking
  // status first
  // instead means the worst case shifts to the opposite, safer
  // direction: if the recording finishes in the moment between this
  // check and the playlist fetch just below, this cycle simply reports
  // finished=false for one extra cycle rather than losing data -- the
  // same erring-toward-not-finished margin ResolveInProgressFinished()
  // already documents for a failed lookup.
  auto getRecordingsStart = std::chrono::steady_clock::now();
  Recording rec;
  std::string recordingsError;
  long lookupHttpStatus = 0;
  // A stream that already has segments is in steady state: its requests get the short bound, a cold start the
  // configured timeout (ShortRequestTimeoutMs()).
  const bool steadyState =
      dispatcharr::IsInProgressSteadyState(coldStart, !m_inProgressRecordingStream.segments.empty());
  const long requestTimeoutMs = steadyState ? ShortRequestTimeoutMs() : 0;
  stateLock.unlock();
  bool lookupAuthenticationNotCompleted = false;
  bool lookupOk = GetRecordingById(recordingId, rec, recordingsError, &lookupHttpStatus, requestTimeoutMs,
                                   &lookupAuthenticationNotCompleted);
  double getRecordingsSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - getRecordingsStart).count();
  // No answer at all to the lookup, for a stream that is already playing: the server is very likely unreachable,
  // and the playlist fetch would only wait out another bound before failing the same way (a refresh was three
  // timeouts long). Not for a cold start, whose loop treats a failed refresh as retryable and whose playlist may
  // answer when the lookup did not.
  if (dispatcharr::ShouldSkipPlaylistAfterUnansweredLookup(steadyState, lookupOk, lookupHttpStatus,
                                                           lookupAuthenticationNotCompleted))
  {
    stateLock.lock();
    error = "Dispatcharr did not answer the recording lookup: " + recordingsError;
    return false;
  }

  std::string baseDir = BaseUrl() + kRecordingsPath + std::to_string(recordingId) + "/hls/";
  std::string playlistUrl = baseDir + "index.m3u8";

  auto playlistFetchStart = std::chrono::steady_clock::now();
  std::string playlistText;
  bool playlistWasNotFound = false;
  long playlistHttpStatus = 0;
  const bool playlistFetched = FetchRawInProgressPlaylist(recordingId, playlistUrl, playlistText, error,
                                                          &playlistWasNotFound, &playlistHttpStatus, requestTimeoutMs);
  stateLock.lock();
  if (!stillSameStream())
  {
    error = "the in-progress recording stream was closed while its playlist was being fetched";
    return false;
  }
  if (!playlistFetched)
  {
    // A definitive 404 OR 3xx here (Dispatcharr has genuinely removed this
    // recording's HLS directory, per docs/RECORDINGS.md's own documented
    // concat-plus-viewer-wait-grace removal -- see FetchRawInProgressPlaylist()'s
    // own header comment for why a *naturally*-completed recording, as
    // opposed to a user Stop, takes the 3xx path specifically), combined
    // with the server's own isInProgress already confirming this recording
    // is done, is a real "this recording is finished and gone" signal in
    // its own right -- worth recomputing `finished` from even though
    // there's no fresh playlist this cycle, rather than the previous
    // behavior of returning early with `finished` untouched. Real,
    // confirmed-by-code-reading gap this closes: without it, a recording
    // whose final pre-removal refresh cycle left `finished` false (its own
    // probe batch had a failure, deliberately holding `finished` at false
    // for a retry -- see ResolveInProgressFinished()'s own comment) never
    // became `finished` again, since every later refresh hit exactly this
    // early return forever. ReadInProgressRecordingStream() then looped
    // its full catch-up budget and returned 0 on every read indefinitely
    // instead of EOF -- confirmed by code reading to be the *only* path to
    // `finished` for a naturally-completed recording before the 3xx case
    // was added here, since a natural completion's own final "completed"/
    // "interrupted" status write (apps/channels/tasks.py) happens strictly
    // after the HLS directory is already removed, at which point this
    // addon's own next FetchRawInProgressPlaylist() call already sees the
    // redirect, not a 404 (Dispatcharr's `hls()` view never 404s a `.m3u8`
    // request as long as the final file exists, only a `.ts` one).
    //
    // Deliberately requires lookupOk too, not just the playlist 404/3xx --
    // a non-404/non-3xx playlist failure (network blip, 5xx), or
    // GetRecordingById() itself failing, still returns early with
    // `finished` untouched, the existing, safer "unknown means don't
    // assume finished" margin ResolveInProgressFinished() already applies
    // to a failed lookup. That left one case open, closed just below: a
    // recording *deleted* entirely mid-playback also 404s on
    // GetRecordingById() itself (lookupOk false, not just the playlist),
    // so this same stuck-forever symptom happened there.
    if (playlistWasNotFound)
      m_inProgressRecordingStream.finished = ResolveInProgressFinished(
          lookupOk, rec.isInProgress, m_inProgressRecordingStream.finished, /*allProbesSucceeded=*/true);
    // The gap the paragraph above documents as still open -- a recording
    // *deleted* outright, where the lookup itself 404s too -- is closed
    // here: both endpoints agreeing the recording doesn't exist is a
    // definitive "gone", unlike a lookup that merely failed. See
    // dispatcharr::IsInProgressContentGone() for the exact conditions.
    if (IsInProgressContentGone(!m_inProgressRecordingStream.segments.empty(), playlistWasNotFound, lookupOk,
                                lookupHttpStatus, rec.isInProgress))
    {
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: in-progress recording %d is gone on the server (%s) -- ending "
                "playback instead of polling it",
                m_inProgressRecordingStream.recordingId,
                lookupOk ? "finished, and its HLS directory has been removed" : "deleted");
      m_inProgressRecordingStream.contentGone = true;
      m_inProgressRecordingStream.finished = true;
      // Its cross-open segment cache entry is dead weight now too, the
      // same as for any other finished recording (see below).
      std::lock_guard<std::mutex> cacheLock(m_inProgressSegmentCacheMutex);
      m_inProgressSegmentCache.erase(m_inProgressRecordingStream.recordingId);
    }
    // A young recording has no HLS directory yet -- the caller's cold-start
    // loop can wait that out. Decided from the same lookup, taken before
    // the playlist fetch, that decides `finished` above.
    if (coldStartRetryableOut)
      *coldStartRetryableOut = ShouldRetryInProgressColdStart(playlistHttpStatus, lookupOk, rec.isInProgress);
    return false;
  }
  double fetchPlaylistSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - playlistFetchStart).count();

  // Append-only merge: segments before the count we already know about are
  // skipped (no rolling-window eviction for a recording -- see
  // InProgressRecordingStreamState's own comment), everything past it is
  // new. #EXTINF: precedes each segment URI line with its duration; HLS
  // never carries byte size, so each newly-discovered segment needs a tiny
  // ranged-GET probe for that -- done in a separate pass below (parse
  // first, probe second) so the probes, which don't depend on each other,
  // can run concurrently instead of one at a time: a cold open of a
  // recording that's been running a while has thousands of already-elapsed
  // segments to size on its very first refresh, and probing them fully
  // serially was confirmed live to take ~29s for a recording ~2h in (1,800
  // probes at a low latency each -- each probe itself was already fast, it was
  // purely the lack of concurrency).
  //
  // The parse itself (including the non-finite #EXTINF-duration UB guard)
  // lives in dispatcharr::ParseNewM3u8SegmentEntries() (M3u8SegmentParser.{h,cpp})
  // so it's unit-testable standalone -- see that function's own comment.
  size_t alreadyKnown = m_inProgressRecordingStream.segments.size();
  std::vector<M3u8SegmentEntry> pending = ParseNewM3u8SegmentEntries(playlistText, baseDir, alreadyKnown);
  // Segment URLs are absolute and name whatever address Dispatcharr thought it was
  // reached on; behind a reverse proxy that is not what this addon can reach. Use
  // the configured address -- see dispatcharr::RebaseRecordingSegmentUrl().
  {
    const std::string segmentPathPrefix =
        std::string(kRecordingsPath) + std::to_string(m_inProgressRecordingStream.recordingId) + "/hls/";
    const std::string configuredBase = BaseUrl();
    for (auto& entry : pending)
      entry.url = RebaseRecordingSegmentUrl(entry.url, configuredBase, segmentPathPrefix);
  }

  // Bounded fan-out: kMaxConcurrentProbes threads in flight at a time, each
  // doing the same HEAD-based probe as before. Every thread writes only its
  // own index of probedSizes, so no locking is needed here -- the merge
  // below, which needs playlist order to keep byte/time offsets correctly
  // cumulative, happens afterward on this thread once every probe in a
  // batch has finished.
  auto probeStart = std::chrono::steady_clock::now();
  std::vector<int64_t> probedSizes(pending.size(), -1);
  // A leading segment that failed on the previous refresh is probed alone: nothing
  // behind it can be merged until it answers, and re-probing the whole tail every
  // refresh (only to discard the sizes) is what flooded the server -- see
  // UnprobeableSegment.h.
  const bool probeOnlyLeading =
      !pending.empty() && ShouldProbeOnlyLeadingSegment(m_inProgressRecordingStream.unprobeableSegment, pending[0].url);
  const size_t probeCount = probeOnlyLeading ? 1 : pending.size();
  stateLock.unlock(); // the probes are network calls
  constexpr size_t kMaxConcurrentProbes = 16;
  RunInBoundedBatches(probeCount, kMaxConcurrentProbes, [this, &pending, &probedSizes, requestTimeoutMs](size_t i)
                      { probedSizes[i] = ProbeSegmentByteSize(pending[i].url, requestTimeoutMs); });
  double probeSegmentsSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - probeStart).count();
  stateLock.lock();
  if (!stillSameStream())
  {
    error = "the in-progress recording stream was closed while its segments were being sized";
    return false;
  }

  // A segment that can't be sized (transient network hiccup, or recycled
  // mid-probe) can't just be skipped in place -- cumulative byte/time
  // offsets need complete, in-order knowledge of every earlier segment's
  // own size, so a gap in the middle would corrupt every offset merged
  // after it. Real, confirmed incident this fixes: an earlier version
  // skipped a failed probe but still advanced past it, so `segments.size()`
  // (this function's own `alreadyKnown` above, on the *next* call) fell
  // behind how many playlist entries had actually been read -- the
  // following refresh then re-parsed starting one entry too early,
  // re-appending an already-merged segment as if new (a duplicate ~4s
  // chunk of MPEG-TS spliced into the stream) while the segment that
  // failed to probe was never retried and stayed a permanent gap. The
  // fix itself lives in dispatcharr::CountLeadingProbedSegments()
  // (M3u8SegmentParser.{h,cpp}) so it's unit-testable standalone -- see
  // that function's own comment: only entries up to (not including) the
  // first failed probe are merged this round, so a failed probe (and
  // anything sequentially after it) is retried next time instead of
  // silently dropped.
  //
  // One failure mode the above cannot resolve by waiting: a leading segment that
  // never answers. After a bounded wait it is merged as a zero-byte placeholder so
  // playback continues past it -- see UnprobeableSegment.h.
  bool gaveUpOnLeadingSegment = false;
  if (!pending.empty())
  {
    gaveUpOnLeadingSegment = UpdateUnprobeableSegmentTracker(
                                 m_inProgressRecordingStream.unprobeableSegment, pending[0].url, probedSizes[0] <= 0,
                                 std::chrono::steady_clock::now()) == UnprobeableSegmentAction::kGiveUp;
    if (gaveUpOnLeadingSegment)
    {
      probedSizes[0] = 0;
      kodi::Log(ADDON_LOG_WARNING,
                "pvr.dispatcharr-unofficial: in-progress recording %d: segment %s could not be sized for "
                "%lld seconds; continuing past it as an empty segment (a few seconds of picture will be missing)",
                m_inProgressRecordingStream.recordingId, pending[0].url.c_str(),
                static_cast<long long>(kUnprobeableSegmentMinWait.count()));
    }
  }
  size_t mergedCount = CountMergeableSegments(probedSizes, gaveUpOnLeadingSegment);
  size_t failedProbeCount = 0;
  for (size_t i = mergedCount; i < probedSizes.size(); ++i)
  {
    if (probedSizes[i] <= 0)
      ++failedProbeCount;
  }
  // The first segment that could not be sized, behind ones that could, is the leading one of the next refresh:
  // arm the tracker for it now, so that refresh probes it alone instead of the whole tail again.
  {
    const size_t armIndex = IndexToArmTrackerAfterMerge(probedSizes, mergedCount, gaveUpOnLeadingSegment, probeCount);
    if (armIndex < pending.size())
      UpdateUnprobeableSegmentTracker(m_inProgressRecordingStream.unprobeableSegment, pending[armIndex].url,
                                      /*leadingFailed=*/true, std::chrono::steady_clock::now());
  }
  for (size_t i = 0; i < mergedCount; ++i)
  {
    InProgressRecordingSegmentInfo info;
    info.url = std::move(pending[i].url);
    info.byteSize = probedSizes[i];
    int64_t durationMs = static_cast<int64_t>(pending[i].durationSec * 1000 + 0.5);
    SegmentAppendOffsets offsets =
        AppendSegmentOffsets(probedSizes[i], durationMs, m_inProgressRecordingStream.totalBytes,
                             m_inProgressRecordingStream.totalDurationMs);
    info.byteOffset = offsets.byteOffset;
    info.timeOffsetMs = offsets.timeOffsetMs;
    m_inProgressRecordingStream.segments.push_back(std::move(info));
  }
  size_t newSegmentsProbed = mergedCount;
  size_t newSegmentsProbeFailed = failedProbeCount;
  // A probe is a HEAD on a `.ts` URL, which refreshes the server's viewer
  // key just as a keep-alive would -- see HlsViewerKeepAlive.h.
  if (mergedCount > 0)
    m_inProgressRecordingStream.nextHlsKeepAliveAt =
        NextHlsKeepAliveDueAt(std::chrono::steady_clock::now(), HlsKeepAliveOutcome::kRefreshed);

  // lookupOk/rec.isInProgress were captured *before* the playlist fetch
  // above -- see that fetch's own comment for why this ordering matters.
  // GetRecordingById() (a single-item GET, not the full list) -- this
  // used to call GetRecordings() and scan the entire result just to read
  // one id's isInProgress flag, O(every recording) on a call made up to
  // twice a second. The decision itself lives in
  // dispatcharr::ResolveInProgressFinished() (RecordingVisibility.h) so
  // it's unit-testable standalone -- see that function's own comment,
  // including the two real bugs it fixes: a failed lookup (a transient
  // network blip or auth hiccup, not GetRecordingById()'s own return
  // value trustworthy about anything) used to be treated identically to
  // a genuinely completed recording, ending playback of one that was
  // actually still being written; and passing failedProbeCount from this
  // same cycle's own probe batch (above) keeps `finished` from going
  // true in the exact cycle where the recording's final segment failed
  // its byte-size probe, which would otherwise strand that segment
  // permanently.
  const bool statusFinished = ResolveInProgressFinished(
      lookupOk, rec.isInProgress, m_inProgressRecordingStream.finished, /*allProbesSucceeded=*/failedProbeCount == 0);
  // Not finished until the playlist says nothing more is coming: a user Stop
  // flips the status about three seconds before the final segment and the
  // ENDLIST tag appear -- see dispatcharr::GateFinishedOnEndList().
  m_inProgressRecordingStream.finished =
      GateFinishedOnEndList(statusFinished, M3u8HasEndList(playlistText),
                            m_inProgressRecordingStream.endListWaitingSince, now, kEndListGrace);
  if (statusFinished && !m_inProgressRecordingStream.finished)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: in-progress recording %d no longer recording, but its playlist has no "
              "#EXT-X-ENDLIST yet -- waiting for the final segment",
              m_inProgressRecordingStream.recordingId);
  }

  // Keep the cross-open cache (m_inProgressSegmentCache) in step. A
  // finished recording won't be opened through this path again -- it plays
  // back as a completed recording instead -- so its entry is just dead
  // weight once that happens. Otherwise, append only the segments this
  // call newly probed rather than recopying the whole vector: this refresh
  // runs roughly every 500ms during active playback of a long recording,
  // and m_inProgressRecordingStream.segments is always exactly this cache
  // entry's own prefix (seeded from it verbatim on open, append-only since)
  // plus whatever's new here, so the two stay in lockstep without a full
  // copy each time.
  if (newSegmentsProbed > 0 || m_inProgressRecordingStream.finished)
  {
    std::lock_guard<std::mutex> cacheLock(m_inProgressSegmentCacheMutex);
    if (m_inProgressRecordingStream.finished)
    {
      m_inProgressSegmentCache.erase(m_inProgressRecordingStream.recordingId);
    }
    else
    {
      auto& cacheEntry = m_inProgressSegmentCache[m_inProgressRecordingStream.recordingId];
      // Only a genuine prefix of the current stream state can be safely
      // appended to; reseed from the full current segment list instead
      // whenever it isn't. The decision itself lives in
      // dispatcharr::ShouldReseedInProgressSegmentCache()
      // (InProgressSegmentCache.h) so it's unit-testable standalone --
      // see that function's own comment for the exact gap this fixes.
      if (ShouldReseedInProgressSegmentCache(cacheEntry.segments.size(), newSegmentsProbed,
                                             m_inProgressRecordingStream.segments.size()))
        cacheEntry.segments = m_inProgressRecordingStream.segments;
      else
        cacheEntry.segments.insert(cacheEntry.segments.end(),
                                   m_inProgressRecordingStream.segments.end() - newSegmentsProbed,
                                   m_inProgressRecordingStream.segments.end());
      cacheEntry.totalBytes = m_inProgressRecordingStream.totalBytes;
      cacheEntry.totalDurationMs = m_inProgressRecordingStream.totalDurationMs;
      cacheEntry.lastUpdated = now;
    }

    // Opportunistic prune: drops any entry (for a different recording)
    // that hasn't been touched in kInProgressSegmentCacheTtlHours -- see
    // InProgressRecordingSegmentCache::lastUpdated's own comment for the
    // leak this closes. A generous TTL: any recording actually still
    // being watched touches its own entry roughly every 500ms, so
    // anything this stale was genuinely abandoned, not just idle between
    // reads.
    constexpr auto kInProgressSegmentCacheTtl = std::chrono::hours(24);
    for (auto it = m_inProgressSegmentCache.begin(); it != m_inProgressSegmentCache.end();)
    {
      if (now - it->second.lastUpdated > kInProgressSegmentCacheTtl)
        it = m_inProgressSegmentCache.erase(it);
      else
        ++it;
    }
  }

  // No separate API-key check here any more: the playlist fetch above goes
  // through FetchRawInProgressPlaylist(), which sends the same key to the same
  // URL and regenerates it on a 401 itself, so getting this far already proves
  // the key good. The extra GET this used to make (a Range 0-0 probe of the
  // same playlist URL) doubled the playlist traffic of every refresh cycle --
  // several a second near the live edge -- for nothing.

  double totalSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - refreshStart).count();
  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: RefreshInProgressRecordingManifest: %.3fs total (playlist fetch "
            "%.3fs, %zu new segment probe(s) %.3fs [%zu failed], GetRecordingById %.3fs), "
            "totalBytes=%lld totalDurationMs=%lld finished=%d",
            totalSec, fetchPlaylistSec, newSegmentsProbed, probeSegmentsSec, newSegmentsProbeFailed, getRecordingsSec,
            static_cast<long long>(m_inProgressRecordingStream.totalBytes),
            static_cast<long long>(m_inProgressRecordingStream.totalDurationMs),
            m_inProgressRecordingStream.finished ? 1 : 0);

  m_inProgressRecordingStream.lastManifestFetch = now;
  return true;
}

bool DispatcharrClient::OpenInProgressRecordingStream(int recordingId, time_t startTime, std::string& error)
{
  CloseInProgressRecordingStream();

  // Opened and seeded in one state-locked scope: a refresh from another thread (GetStreamTimes() polling) that sees the
  // stream open between two separate scopes would find no segments yet, merge the whole playlist, and then the seed
  // would add the cached copies again -- every segment twice in the byte stream. Reopening the same still-recording
  // (channel switch and back, resuming after a pause) shouldn't re-probe segments already sized on a previous open
  // anyway -- see m_inProgressSegmentCache's own comment. State lock before the cache lock, the order everywhere both
  // are held.
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    std::lock_guard<std::mutex> cacheLock(m_inProgressSegmentCacheMutex);
    m_inProgressRecordingStream.open = true;
    m_inProgressRecordingStream.recordingId = recordingId;
    m_inProgressRecordingStream.startTime = startTime;
    ++m_inProgressSession;
    auto cacheIt = m_inProgressSegmentCache.find(recordingId);
    if (cacheIt != m_inProgressSegmentCache.end())
    {
      m_inProgressRecordingStream.segments = cacheIt->second.segments;
      m_inProgressRecordingStream.totalBytes = cacheIt->second.totalBytes;
      m_inProgressRecordingStream.totalDurationMs = cacheIt->second.totalDurationMs;
    }
  }

  // Cold-start grace period, same reasoning as OpenLiveTimeshiftStream()'s:
  // Dispatcharr's own DVR ffmpeg needs a real few seconds to connect to
  // the live proxy and produce a full first HLS segment before there's
  // anything to report -- confirmed live this addon's own
  // ReadInProgressRecordingStream() catch-up loop wasn't a substitute for
  // this: Kodi's own CDVDDemuxFFmpeg::Open() format probe gave up after
  // ~38s with "error probing input format" rather than retrying patiently
  // the way this addon's own reads do, so Open() itself needs to already
  // have at least one real segment to hand it before returning. 45s, not
  // the 15s live-timeshift uses: confirmed live a 15s budget still wasn't
  // enough here and reading Dispatcharr's own DVR task source
  // (apps/channels/tasks.py) explains why -- it documents its own
  // `_first_segment_timeout = 15.0` for *just* the first-segment wait,
  // on top of whatever real time the recording task itself takes to get
  // scheduled and its own ffmpeg connected before that timer even starts.
  constexpr int kColdStartMaxAttempts = 90;
  constexpr int kColdStartSleepMs = 500;
  bool haveSegment = false;
  for (int attempt = 0; attempt < kColdStartMaxAttempts; ++attempt)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: OpenInProgressRecordingStream: cold-start attempt=%d",
              attempt);
    bool retryable = false;
    const bool refreshOk = RefreshInProgressRecordingManifest(/*force=*/true, error, &retryable, /*coldStart=*/true);
    bool haveSegments = false, finished = false;
    {
      std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
      haveSegments = !m_inProgressRecordingStream.segments.empty();
      finished = m_inProgressRecordingStream.finished;
    }
    // A recording opened within its first ~3s has no HLS directory yet and the
    // playlist answers 404 -- confirmed live to fail every time (6 of 6) when this
    // used to give up on the first failed refresh, even though the same budget
    // already waits patiently for a playlist that exists but has no segment in
    // it. The step taken for each outcome is dispatcharr::DecideInProgressColdStartStep()
    // (RecordingVisibility.h, tested); see dispatcharr::ShouldRetryInProgressColdStart()
    // for why only that one failure retries and anything else still fails fast.
    bool stopWaiting = false;
    switch (DecideInProgressColdStartStep(refreshOk, retryable, haveSegments, finished))
    {
    case InProgressColdStartStep::kReady:
      haveSegment = true;
      stopWaiting = true;
      break;
    case InProgressColdStartStep::kFinishedEmpty:
      stopWaiting = true; // finished with literally zero segments -- nothing to wait for
      break;
    case InProgressColdStartStep::kFail:
    {
      std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
      m_inProgressRecordingStream = InProgressRecordingStreamState();
      ++m_inProgressSession;
      return false;
    }
    case InProgressColdStartStep::kWait:
      if (!refreshOk)
        kodi::Log(ADDON_LOG_DEBUG,
                  "pvr.dispatcharr-unofficial: OpenInProgressRecordingStream: playlist not created yet for a "
                  "recording that is in progress (attempt=%d) -- waiting",
                  attempt);
      break;
    }
    if (stopWaiting)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kColdStartSleepMs));
  }
  bool finishedNow;
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    finishedNow = m_inProgressRecordingStream.finished;
  }
  if (!haveSegment && !finishedNow)
  {
    error = "recording hasn't produced any segments yet";
    {
      std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
      m_inProgressRecordingStream = InProgressRecordingStreamState();
      ++m_inProgressSession;
    }
    return false;
  }
  // Unlike live-timeshift, always start at true byte 0: there's no
  // server-side buffer this addon starts/stops, no "live edge" concept to
  // land near -- Dispatcharr's own DVR task has been writing this
  // recording since it began regardless of whether anything's reading it,
  // so "play a recording" naturally means "from the start", matching
  // every other recording/VOD convention (and the existing
  // completed-recording behaviour).
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  m_inProgressRecordingStream.position = 0;
  // The first read's own segment GET follows immediately, so nothing is
  // owed for the first interval -- see HlsViewerKeepAlive.h. Without this,
  // the zero "due now" default would send a needless request on the very
  // first poll after every open.
  m_inProgressRecordingStream.nextHlsKeepAliveAt =
      NextHlsKeepAliveDueAt(std::chrono::steady_clock::now(), HlsKeepAliveOutcome::kRefreshed);
  return true;
}

int DispatcharrClient::ReadInProgressRecordingStream(uint8_t* buffer, unsigned int size)
{
  // Held for the state reads and writes below and released around every network call and sleep (see
  // m_inProgressStateMutex); after each re-lock the stream is checked to still be the one this call started on.
  std::unique_lock<std::mutex> stateLock(m_inProgressStateMutex);
  if (!m_inProgressRecordingStream.open || size == 0)
    return 0;
  if (m_inProgressRecordingStream.permanentReadFailures.ended)
    return 0;
  const uint64_t session = m_inProgressSession;
  auto stillSameStream = [&]() { return m_inProgressRecordingStream.open && m_inProgressSession == session; };

  if (m_inProgressRecordingStream.position >= m_inProgressRecordingStream.totalBytes)
  {
    if (m_inProgressRecordingStream.finished)
      return 0; // genuine EOF -- the recording is done and we're at its true end

    constexpr int kCatchUpSleepMs = 250;

    // Same seek-probe-vs-genuine-catch-up distinction as
    // ReadLiveTimeshiftStream() -- see its own comment for the full
    // reasoning (confirmed live there; the same generic Kodi/ffmpeg
    // seek-probing behaviour applies here, since this uses the same
    // native-demuxer mechanism).
    constexpr auto kSeekProbeWindow = std::chrono::milliseconds(800);
    bool likelySeekProbe =
        IsLikelySeekProbe(m_inProgressRecordingStream.position, m_inProgressRecordingStream.lastShortGiveUpPosition,
                          m_inProgressRecordingStream.lastSeekTime, std::chrono::steady_clock::now(), kSeekProbeWindow);

    int64_t segmentDurationEstimateMs =
        EstimateSegmentDurationMs(m_inProgressRecordingStream.totalDurationMs, m_inProgressRecordingStream.segments);
    int catchUpAttempts = ComputeCatchUpAttempts(likelySeekProbe, segmentDurationEstimateMs, kCatchUpSleepMs);

    const int64_t catchUpBudgetMs = ComputeCatchUpWallClockBudgetMs(catchUpAttempts, kCatchUpSleepMs);
    auto catchUpStart = std::chrono::steady_clock::now();
    int attemptsUsed = 0;
    for (int attempt = 0;
         attempt < catchUpAttempts && m_inProgressRecordingStream.position >= m_inProgressRecordingStream.totalBytes;
         ++attempt)
    {
      attemptsUsed = attempt + 1;
      std::string refreshError;
      stateLock.unlock();
      RefreshInProgressRecordingManifest(/*force=*/true, refreshError);
      stateLock.lock();
      if (!stillSameStream())
        return -1;
      if (m_inProgressRecordingStream.position < m_inProgressRecordingStream.totalBytes)
        break;
      if (m_inProgressRecordingStream.finished)
        break; // finished while we were polling -- stop waiting, report EOF below
      if (HasCatchUpBudgetElapsed(
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - catchUpStart),
              catchUpBudgetMs))
        break; // the server is not answering promptly -- see kMaxCatchUpWallClockMs
      if (attempt + 1 < catchUpAttempts)
      {
        stateLock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(kCatchUpSleepMs));
        stateLock.lock();
        if (!stillSameStream())
          return -1;
      }
    }

    bool caughtUp = m_inProgressRecordingStream.position < m_inProgressRecordingStream.totalBytes;
    m_inProgressRecordingStream.lastShortGiveUpPosition =
        ComputeShortGiveUpPosition(likelySeekProbe, caughtUp, m_inProgressRecordingStream.position);

    double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - catchUpStart).count();
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: ReadInProgressRecordingStream: catch-up loop used %d/%d attempts, "
              "%.3fs (likelySeekProbe=%d, segmentDurationEstimateMs=%lld), position=%lld totalBytes=%lld "
              "-> %s",
              attemptsUsed, catchUpAttempts, elapsedSec, likelySeekProbe ? 1 : 0,
              static_cast<long long>(segmentDurationEstimateMs),
              static_cast<long long>(m_inProgressRecordingStream.position),
              static_cast<long long>(m_inProgressRecordingStream.totalBytes), caughtUp ? "caught up" : "gave up");

    if (!caughtUp)
      return 0;
  }

  const InProgressRecordingSegmentInfo* seg =
      FindSegmentContainingPosition(m_inProgressRecordingStream.position, m_inProgressRecordingStream.segments);
  if (!seg)
    return 0; // shouldn't happen (no rolling eviction here), but nothing safely readable if it did

  // Copied out of `seg` immediately rather than kept as a live pointer
  // across the blocking curl_easy_perform() call(s) below -- same
  // reasoning as ReadLiveTimeshiftStream()'s own identical copy:
  // GetStreamTimes() may run on a different Kodi thread than this one and
  // call RefreshInProgressRecordingManifest(), which push_backs onto this
  // same segments vector. The state lock is released across the network
  // wait below (see m_inProgressStateMutex), so a pointer into the vector
  // would dangle once a reallocation happened meanwhile.
  int64_t segByteOffset = seg->byteOffset;
  int64_t segByteSize = seg->byteSize;
  std::string segUrl = seg->url;
  // Defense in depth: a segment only gets a size (and so bytes to read) after its probe, and the
  // probe refuses a URL off the configured server -- but the key must not depend on that one
  // check, so the read refuses it too rather than send the X-API-Key anywhere else.
  if (!IsOnConfiguredServer(segUrl))
  {
    // EOF, not -1: Kodi retries a -1 read at once, a busy loop for something that cannot succeed.
    kodi::Log(m_loggedForeignSegment.exchange(true) ? ADDON_LOG_DEBUG : ADDON_LOG_WARNING,
              "pvr.dispatcharr-unofficial: refusing to read an in-progress recording segment that is not on the "
              "configured server");
    return 0;
  }

  int64_t offsetInSegment = m_inProgressRecordingStream.position - segByteOffset;

  // From here to the copy out of the cached bytes at the end, Close must not free the handle or the cache, so the
  // curl mutex is held (lock order: the curl mutex before the state mutex).
  const int64_t positionAtStart = m_inProgressRecordingStream.position;
  stateLock.unlock();
  std::unique_lock<std::mutex> curlLock(m_inProgressCurlMutex);
  stateLock.lock();
  if (!stillSameStream() || m_inProgressRecordingStream.position != positionAtStart)
    return -1; // closed, or repositioned by a seek, since the segment was chosen -- Kodi retries the read

  // Fetch this segment's full body once and cache it, rather than issuing a
  // ranged GET per read -- see InProgressRecordingStreamState's own comment
  // on cachedSegmentBytes for why a ranged read against this specific
  // endpoint would silently return the wrong bytes, not just be wasteful.
  if (m_inProgressRecordingStream.cachedSegmentByteOffset != segByteOffset)
  {
    // Whatever this segment was, the server has already confirmed there is
    // nothing left to fetch it from -- see contentGone's own comment. EOF
    // (not -1): Kodi retries a -1 read near-immediately rather than giving
    // up, which is the request storm this exists to end.
    if (m_inProgressRecordingStream.contentGone)
      return 0;
    // `attempt` counts only the 401 key recovery below; a transient failure (server unreachable, a gateway error)
    // retries within a budget instead of failing the read -- see dispatcharr::IsTransientReadFailure().
    constexpr std::chrono::milliseconds kTransientReadBudget{20000};
    const auto readStart = std::chrono::steady_clock::now();
    int transientFailures = 0;
    for (int attempt = 0; attempt < 2;)
    {
      auto segmentFetchStart = std::chrono::steady_clock::now();
      CURL* curl = static_cast<CURL*>(m_inProgressRecordingStream.curl);
      if (!curl)
      {
        curl = curl_easy_init();
        if (!curl)
          return -1;
        m_inProgressRecordingStream.curl = curl;
      }

      struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));

      // Bounded to segByteSize via FixedBufferSink, not a plain unbounded
      // WriteCallback into a std::string (fixed 2026-09-27, a 55th-pass
      // audit, fixing a real, confirmed bug a 53rd-pass audit's earlier
      // ProbeSegmentByteSize() clamp introduced, found via a project-wide
      // review, not itself independently reproduced): docs/OPEN_ITEMS.md's
      // own 8th-pass entry on this read never clamping to seg->byteSize
      // already documented the failure mode -- a GET body larger than the
      // segment's own recorded byteSize silently misaligns every later
      // segment's offset -- but called it "plausible only in an
      // already-anomalous scenario". ProbeSegmentByteSize()'s own
      // kMaxProbedSegmentByteSize clamp (added two passes before this one)
      // made this deterministically reachable for any segment genuinely
      // larger than that ceiling, not just an anomalous probe/GET
      // disagreement: this addon's own cumulative byte-offset bookkeeping
      // (AppendSegmentOffsets()) always uses the *clamped* byteSize, but
      // an unbounded fetch here would still cache the segment's real,
      // larger body -- so as `position` advanced past the clamped
      // boundary while still reading this segment's own real tail, the
      // *next* read would wrongly resolve to a small offset inside the
      // *next* segment's own real file instead, splicing in wrong-offset
      // bytes with no error raised (the "Packet corrupt"/audio-desync
      // failure class this project has repeatedly investigated
      // elsewhere). Bounding the fetch itself to segByteSize instead
      // means an oversized segment's tail is cleanly (if lossily)
      // dropped -- this addon's own official byte accounting and what it
      // actually delivers now agree for this specific case (a real GET
      // body *larger* than segByteSize; a body genuinely *smaller* than
      // segByteSize is a separate, pre-existing, unrelated behavior --
      // see the empty-read check below), so the next read correctly
      // moves on to the next segment's own real start rather than a
      // wrong offset into it.
      //
      // Fetched into a LOCAL buffer, not directly into the shared
      // m_inProgressRecordingStream.cachedSegmentBytes (fixed 2026-09-27,
      // a 56th-pass audit, fixing a real, confirmed cache-coherency
      // regression this same 55th-pass fix introduced, found via a
      // project-wide review, not itself independently reproduced): the
      // first version of this fix zeroed/resized the SHARED
      // cachedSegmentBytes before the fetch even started, unlike the
      // std::string local this replaced, which never touched shared
      // state until a fetch fully succeeded. That meant any of this
      // loop's several failure returns (a transport error, a non-200, a
      // 401 whose API key recovery itself failed, or exhausting both
      // attempts) left cachedSegmentByteOffset still pointing at the
      // PREVIOUS, still-valid segment while cachedSegmentBytes had
      // already been overwritten with zeros or a partial fetch of the
      // NEW one -- so the very next read landing back inside that
      // previous segment (a seek-back, or ffmpeg's own mpegts seek
      // bisection retrying after Kodi's negative read return) passed the
      // "already cached" check and silently served corrupted content,
      // with no error raised at all. Building the sink over a local
      // vector and only replacing cachedSegmentBytes/cachedSegmentByteOffset
      // together, once curl_easy_perform() and the status checks below
      // have all genuinely succeeded, restores the same
      // only-touch-shared-state-on-success invariant the original
      // std::string version already had, while keeping this fix's own
      // bounded-size safety property.
      std::vector<uint8_t> fetchedBytes;
      try
      {
        fetchedBytes.assign(static_cast<size_t>(segByteSize), 0);
      }
      catch (const std::bad_alloc&)
      {
        // This runs on Kodi's read thread, which has no handler: an escaped bad_alloc ended the
        // process. End the stream instead (EOF, not -1, which Kodi retries at once).
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharr-unofficial: out of memory reading a %lld-byte in-progress recording segment",
                  static_cast<long long>(segByteSize));
        return 0;
      }
      FixedBufferSink sink{fetchedBytes.data(), static_cast<unsigned int>(segByteSize), 0};
      curl_easy_setopt(curl, CURLOPT_URL, segUrl.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
      ApplyStandardCurlOptions(curl, GetCurlShare());

      stateLock.unlock();
      CURLcode res = curl_easy_perform(curl);
      long httpCode = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_slist_free_all(headers);
      stateLock.lock();
      if (!stillSameStream())
        return -1;

      // Uncached until now: this is the ONE unlogged blocking network call
      // in the whole read path -- a seek landing on a segment ordinary
      // playback hasn't reached yet pays this full synchronous
      // curl_easy_perform() with no visibility at all before this was
      // added, which the catch-up-loop and manifest-refresh logging next
      // to it could never explain by itself (this fires regardless of
      // whether position was ever >= totalBytes, so it's not gated on
      // "likelySeekProbe" the way those are).
      double fetchSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - segmentFetchStart).count();
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr-unofficial: ReadInProgressRecordingStream: segment body fetch (attempt=%d) "
                "byteSize=%lld took %.3fs, curlResult=%d httpCode=%ld url=%s",
                attempt, static_cast<long long>(segByteSize), fetchSec, static_cast<int>(res), httpCode,
                segUrl.c_str());

      if (httpCode == 401 && attempt == 0)
      {
        std::string regenKey, regenError;
        stateLock.unlock();
        const bool regenerated = ObtainApiKey(regenKey, regenError);
        stateLock.lock();
        if (!stillSameStream())
          return -1;
        if (regenerated)
        {
          ++attempt;
          continue;
        }
      }

      if (IsTransientReadFailure(res == CURLE_OK, httpCode))
      {
        if (res != CURLE_OK)
        {
          // Same "reused connection went stale" handling as ReadRecordingStream().
          curl_easy_cleanup(curl);
          m_inProgressRecordingStream.curl = nullptr;
        }
        ++transientFailures;
        const auto waited =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readStart);
        if (!m_abortRequests && ShouldKeepRetryingTransientRead(waited, kTransientReadBudget))
        {
          stateLock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(SegmentFetchRetryDelayMs(transientFailures)));
          stateLock.lock();
          if (!stillSameStream())
            return -1;
          continue;
        }
        return -1;
      }
      if (httpCode != 200)
      {
        // A segment the playlist listed 404s when the recording it belongs
        // to is gone (deleted, or finished with its HLS directory removed --
        // e.g. a viewer paused past the point Dispatcharr's own viewer-wait
        // gave up on it). A refresh asks the server directly and answers
        // definitively when it is gone; anything it can't explain still
        // fails the read the way it always did. Deliberately not forced:
        // an unexplained 404 (the playlist still fine) makes the refresh
        // succeed, which arms its own throttle, so Kodi's near-immediate
        // retries of the failed read don't multiply into a lookup and a
        // playlist fetch per retry on top of the segment fetch -- while the
        // gone case, whose refresh fails and so never arms it, is checked
        // on every attempt until it is confirmed.
        if (httpCode == 404)
        {
          std::string refreshError;
          stateLock.unlock();
          RefreshInProgressRecordingManifest(/*force=*/false, refreshError);
          stateLock.lock();
          if (!stillSameStream())
            return -1;
          if (m_inProgressRecordingStream.contentGone)
            return 0;
        }
        // A status that will not clear (a 403 or 500 on the segment, a 404 the refresh could not explain): -1
        // until the run has lasted long enough, then end the stream, see RecordPermanentReadFailure().
        if (RecordPermanentReadFailure(m_inProgressRecordingStream.permanentReadFailures,
                                       std::chrono::steady_clock::now()))
        {
          kodi::Log(ADDON_LOG_ERROR,
                    "pvr.dispatcharr-unofficial: in-progress recording %d: segment reads keep failing (last HTTP "
                    "status %ld); ending the stream instead of letting the player retry it endlessly",
                    m_inProgressRecordingStream.recordingId, httpCode);
          kodi::QueueNotification(QUEUE_ERROR, "", "This recording can't be read from Dispatcharr any more.");
          return 0;
        }
        return -1;
      }

      // Trims off the zero-padding above -- sink.written is the real byte
      // count actually written (at most segByteSize, per FixedBufferWriteCallback's
      // own capacity clamp), which can be smaller than the buffer's own
      // pre-sized capacity if the real body turned out shorter. Committed
      // to shared state only here, together, now that the fetch has
      // genuinely succeeded -- see this loop's own comment above on why
      // that atomicity matters.
      fetchedBytes.resize(sink.written);
      m_inProgressRecordingStream.cachedSegmentBytes = std::move(fetchedBytes);
      m_inProgressRecordingStream.cachedSegmentByteOffset = segByteOffset;
      // A real `.ts` GET refreshes the server's viewer key on its own --
      // see HlsViewerKeepAlive.h.
      m_inProgressRecordingStream.nextHlsKeepAliveAt =
          NextHlsKeepAliveDueAt(std::chrono::steady_clock::now(), HlsKeepAliveOutcome::kRefreshed);
      break;
    }
    if (m_inProgressRecordingStream.cachedSegmentByteOffset != segByteOffset)
      return -1; // both attempts failed
  }

  // A seek (or a close) during the fetch above, while the state lock was released: `offsetInSegment` is from the old
  // position, and advancing the new one by what is copied out would hand the demuxer the wrong bytes. The segment is
  // cached now, so Kodi's retry of the read costs nothing.
  if (!stillSameStream() || m_inProgressRecordingStream.position != positionAtStart)
    return -1;
  const auto& cached = m_inProgressRecordingStream.cachedSegmentBytes;
  if (offsetInSegment < 0 || static_cast<size_t>(offsetInSegment) >= cached.size())
    return 0; // segment turned out smaller than the probed byteSize -- nothing left to give

  int64_t available = static_cast<int64_t>(cached.size()) - offsetInSegment;
  unsigned int wantSize = static_cast<unsigned int>(std::min<int64_t>(size, available));
  std::memcpy(buffer, cached.data() + offsetInSegment, wantSize);
  m_inProgressRecordingStream.position += static_cast<int64_t>(wantSize);
  ResetPermanentReadFailures(m_inProgressRecordingStream.permanentReadFailures);
  return static_cast<int>(wantSize);
}

int64_t DispatcharrClient::SeekInProgressRecordingStream(int64_t position, int whence)
{
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    if (!m_inProgressRecordingStream.open)
      return -1;
    m_inProgressRecordingStream.lastSeekTime = std::chrono::steady_clock::now();
  }

  if (whence == SEEK_END)
  {
    // Before the state lock is taken: the refresh is a network call.
    std::string refreshError;
    RefreshInProgressRecordingManifest(/*force=*/true, refreshError);
  }

  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  if (!m_inProgressRecordingStream.open)
    return -1;

  int64_t newPos = ResolveSeekPosition(position, whence, m_inProgressRecordingStream.position,
                                       m_inProgressRecordingStream.totalBytes);
  if (newPos < 0)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: SeekInProgressRecordingStream(position=%lld, whence=%d) from "
              "current=%lld -> computed newPos=%lld < 0, failing",
              static_cast<long long>(position), whence, static_cast<long long>(m_inProgressRecordingStream.position),
              static_cast<long long>(newPos));
    return -1;
  }

  // Same live-backoff SeekLiveTimeshiftStream() already applies (via the
  // same shared dispatcharr::ComputeLiveEdgeTailTarget(), just with a
  // margin of 1 segment here instead of that path's 3), and for the
  // identical reason: clamping a forward seek to exactly totalBytes (the
  // tip) leaves zero read-ahead margin, so playback resumes, immediately
  // re-catches-up to the (still-)tail within moments of real playback,
  // and has to wait through another chunk of Dispatcharr's own DVR
  // ffmpeg's ~4s HLS segment cadence a second time right after what
  // looked like a completed seek -- exactly the "skip ahead to live took
  // ~10s" symptom this was added to investigate. Backing off by one
  // segment's worth of bytes means at least that much is already
  // available to play immediately, the same margin SeekLiveTimeshiftStream()
  // already keeps.
  // Only while genuinely still in progress -- once finished, nothing more
  // will ever be appended, so there's no future catch-up to hold read-ahead
  // margin against; clamping short here would just permanently cut off the
  // last segment's worth of an otherwise-complete recording (e.g. a
  // SEEK_END landing short of the true end). Real bug, not yet reproduced
  // live (found via code reading): this used to apply unconditionally.
  int64_t tailTarget =
      ComputeLiveEdgeTailTarget(m_inProgressRecordingStream.segments, m_inProgressRecordingStream.totalBytes, 1);
  // Never behind the reader's own position on a forward seek -- see
  // dispatcharr::ClampSeekToTail().
  bool clampedToTail = !m_inProgressRecordingStream.finished && newPos > tailTarget;
  if (clampedToTail)
    newPos = ClampSeekToTail(newPos, m_inProgressRecordingStream.position, tailTarget);

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: SeekInProgressRecordingStream(position=%lld, whence=%d) from "
            "current=%lld, totalBytes=%lld -> newPos=%lld%s",
            static_cast<long long>(position), whence, static_cast<long long>(m_inProgressRecordingStream.position),
            static_cast<long long>(m_inProgressRecordingStream.totalBytes), static_cast<long long>(newPos),
            clampedToTail ? " (clamped to tail)" : "");

  m_inProgressRecordingStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetInProgressRecordingStreamLength()
{
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    if (!m_inProgressRecordingStream.open)
      return -1;
  }
  std::string refreshError;
  RefreshInProgressRecordingManifest(/*force=*/false, refreshError);
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  return m_inProgressRecordingStream.totalBytes;
}

int64_t DispatcharrClient::GetInProgressRecordingStreamDurationMs()
{
  {
    std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
    if (!m_inProgressRecordingStream.open)
      return 0;
  }
  std::string refreshError;
  RefreshInProgressRecordingManifest(/*force=*/false, refreshError);
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  return m_inProgressRecordingStream.totalDurationMs;
}

time_t DispatcharrClient::GetInProgressRecordingStreamStartTime()
{
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  if (!m_inProgressRecordingStream.open)
    return 0;
  return m_inProgressRecordingStream.startTime;
}

void DispatcharrClient::CloseInProgressRecordingStream()
{
  // The curl mutex first: a segment read still in flight finishes before its handle and cached bytes go.
  std::lock_guard<std::mutex> curlLock(m_inProgressCurlMutex);
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  if (m_inProgressRecordingStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_inProgressRecordingStream.curl));
  m_inProgressRecordingStream = InProgressRecordingStreamState();
  ++m_inProgressSession;
}

bool DispatcharrClient::IsInProgressRecordingStreamOpen() const
{
  std::lock_guard<std::mutex> stateLock(m_inProgressStateMutex);
  return m_inProgressRecordingStream.open;
}

bool DispatcharrClient::IsLiveTimeshiftStreamOpen() const
{
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  return m_liveTimeshiftStream.open;
}

bool DispatcharrClient::OpenRecordingStream(int recordingId, std::string& error)
{
  CloseRecordingStream();

  std::string url = BaseUrl() + kRecordingsPath + std::to_string(recordingId) + "/file/";

  // Up to two attempts: Dispatcharr keeps only one active API key
  // account-wide, so another client regenerating that account's key (a
  // second Kodi install, a script) can silently invalidate this addon's
  // stored one between restarts. On a 401, re-read the account's current
  // key (ObtainApiKey() -- adopts the existing key, generating only when
  // the account has none) and retry once before failing outright -- see
  // GetApiKey()'s comment for why the caller still needs to re-persist
  // the result.
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      error = "Failed to initialise libcurl";
      return false;
    }

    struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));

    // A tiny ranged GET rather than a HEAD request: confirmed the "in
    // progress -> redirect to HLS" behaviour on this endpoint, and it's
    // safer to assume that only applies to the method a real player
    // actually uses (GET) rather than trust it also applies to HEAD.
    //
    // Bounded to a 1-byte FixedBufferSink, not a plain unbounded
    // WriteCallback into a std::string (fixed 2026-09-27, a 57th-pass
    // audit, fixing a real, confirmed gap found via a project-wide
    // review, not itself independently reproduced): "0-0" only asks for
    // 1 byte, but nothing here actually enforced that on the read side --
    // if the server (or a proxy in front of it) ignored the Range header
    // and answered with the full recording body instead of a 206 partial
    // response, this discarded write buffer would silently accumulate
    // however much of an entire real recording file (up to many GB)
    // arrives before ApplyStandardCurlOptions()'s own CURLOPT_TIMEOUT
    // (this addon's configured timeoutSeconds, 30s default) eventually
    // aborts the transfer -- reworded 2026-09-27, a 58th-pass audit,
    // fixing this comment's own "with no bound at all" overstatement,
    // found via a project-wide review: the transfer itself was never
    // literally unbounded (the existing timeout still applied), but a
    // fast connection can buffer a great deal of memory well within 30
    // seconds, so the practical risk this fixes is unchanged. Dispatcharr's
    // own RecordingViewSet.file() does honor Range (confirmed against its
    // real current upstream source), so this needs a misbehaving proxy
    // in front of it to trigger -- but the fix costs nothing: this code
    // never wanted more than 1 byte anyway.
    uint8_t discardBuffer[1];
    FixedBufferSink discardSink{discardBuffer, 1, 0};
    int64_t totalLength = -1;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discardSink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RecordingHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &totalLength);
    // A ranged GET of one byte answers with a Content-Length of 1, and a redirect or
    // an in-progress recording's playlist with a few KB, so a response that DECLARES
    // more than this is a server or proxy that dropped the Range header and is about
    // to send the whole recording. FixedBufferWriteCallback never aborts, so without
    // this the open waited for all of it until the 30 s timeout and then failed with
    // "Timeout was reached" for a large recording (found by the 2026-10-04
    // hardening sweep). libcurl checks the limit once the headers are in, so the
    // status code and Content-Type below are already known.
    constexpr curl_off_t kProbeMaxDeclaredBytes = 4 * 1024 * 1024;
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, kProbeMaxDeclaredBytes);
    ApplyStandardCurlOptions(curl, GetCurlShare());

    CURLcode res = static_cast<CURLcode>(PerformWithSafeRedirects(
        curl, url,
        [&discardSink, &totalLength]()
        {
          discardSink.written = 0;
          totalLength = -1;
        },
        /*switchPostToGet=*/false));
    // A response cut short for being larger than the probe wants is a response all
    // the same: its status and headers are what this reads. The length stays
    // unknown (-1) because a dropped Range means no Content-Range either.
    if (res == CURLE_FILESIZE_EXCEEDED)
      res = CURLE_OK;
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    char* effectiveUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    char* contentTypeRaw = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentTypeRaw);
    std::string resolvedUrl = effectiveUrl ? effectiveUrl : url;
    std::string contentType = contentTypeRaw ? contentTypeRaw : "";
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
      error = std::string("HTTP request failed: ") + curl_easy_strerror(res);
      return false;
    }

    if (httpCode == 401 && attempt == 0)
    {
      std::string regenKey, regenError;
      if (ObtainApiKey(regenKey, regenError))
        continue;
    }

    if (httpCode < 200 || httpCode >= 300)
    {
      error = "Dispatcharr returned HTTP " + std::to_string(httpCode) + " opening recording stream";
      const std::string userMessage = DescribeRecordingOpenFailure(httpCode);
      if (!userMessage.empty())
        kodi::QueueNotification(QUEUE_ERROR, "", userMessage);
      return false;
    }
    // The classification itself lives in dispatcharr::IsInProgressHlsRedirect()
    // (RecordingHttpUtil.h) so it's unit-testable standalone -- see that
    // function's own comment.
    if (IsInProgressHlsRedirect(contentType, resolvedUrl))
    {
      error = "This recording is still in progress; playback of in-progress "
              "recordings isn't supported yet, only completed ones";
      return false;
    }

    m_recordingStream.open = true;
    m_recordingStream.url = resolvedUrl;
    m_recordingStream.length = totalLength;
    m_recordingStream.position = 0;
    return true;
  }
  error = "Dispatcharr returned HTTP 401 opening recording stream even after refreshing the API key";
  return false;
}

int DispatcharrClient::FailRecordingReadPermanently(long httpCode)
{
  if (!RecordPermanentReadFailure(m_recordingStream.permanentReadFailures, std::chrono::steady_clock::now()))
    return -1;
  kodi::Log(ADDON_LOG_ERROR,
            "pvr.dispatcharr-unofficial: ReadRecordingStream: reads of this recording keep failing (last HTTP status "
            "%ld); ending the stream instead of letting the player retry it endlessly",
            httpCode);
  kodi::QueueNotification(QUEUE_ERROR, "", "This recording can't be read from Dispatcharr any more.");
  return 0;
}

int DispatcharrClient::ReadRecordingStream(uint8_t* buffer, unsigned int size)
{
  if (!m_recordingStream.open || size == 0)
    return 0;
  if (m_recordingStream.length >= 0 && m_recordingStream.position >= m_recordingStream.length)
    return 0; // EOF

  // Through dispatcharr::ComputeReadRangeEnd() (StreamSeek.h), not an
  // inline `position + size - 1` -- see that function's own comment for
  // the real signed-overflow gap this closes.
  int64_t rangeEnd = 0;
  if (!ComputeReadRangeEnd(m_recordingStream.position, size, rangeEnd))
    return -1;
  std::string range = std::to_string(m_recordingStream.position) + "-" + std::to_string(rangeEnd);

  // Already found to be a server that ignores Range -- see ServerIgnoredRangeRequest().
  if (m_recordingStream.rangeIgnored)
    return 0;
  if (m_recordingStream.permanentReadFailures.ended)
    return 0;

  // See OpenRecordingStream(): the API key can be invalidated mid-playback
  // by another install regenerating it, so retry once after a self-heal.
  // `attempt` counts only the 401 key recovery below; a transient failure retries within the budget instead.
  constexpr std::chrono::milliseconds kTransientReadBudget{20000};
  const auto readStart = std::chrono::steady_clock::now();
  int transientFailures = 0;
  for (int attempt = 0; attempt < 2;)
  {
    // Reuse one persistent handle across every read (see RecordingStreamState's
    // comment) instead of curl_easy_init()/cleanup() per call, so libcurl's
    // connection cache lets HTTP keep-alive apply across sequential reads.
    CURL* curl = static_cast<CURL*>(m_recordingStream.curl);
    if (!curl)
    {
      curl = curl_easy_init();
      if (!curl)
        return -1;
      m_recordingStream.curl = curl;
    }

    struct curl_slist* headers = static_cast<struct curl_slist*>(AppendApiKeyHeaderIfPresent(nullptr, GetApiKey()));

    FixedBufferSink sink{buffer, size, 0, /*truncated=*/false, /*abortWhenFull=*/true};
    int64_t serverReportedTotal = -1;
    curl_easy_setopt(curl, CURLOPT_URL, m_recordingStream.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RecordingHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &serverReportedTotal);
    ApplyStandardCurlOptions(curl, GetCurlShare());

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    // The buffer filled and the transfer was ended on purpose (see FixedBufferSink::abortWhenFull): not a failure.
    if (res == CURLE_WRITE_ERROR && sink.truncated)
      res = CURLE_OK;

    // The order of the checks is dispatcharr::ClassifyRecordingReadResponse()'s (RecordingHttpUtil.h, tested).
    RecordingReadOutcome outcome =
        ClassifyRecordingReadResponse(res == CURLE_OK, httpCode, /*apiKeyAlreadyRefreshed=*/attempt > 0,
                                      m_recordingStream.position, m_recordingStream.length, serverReportedTotal);
    if (outcome == RecordingReadOutcome::kRefreshApiKeyAndRetry)
    {
      std::string regenKey, regenError;
      if (ObtainApiKey(regenKey, regenError))
      {
        ++attempt;
        continue;
      }
      // No new key to be had: judged as it would have been had the key already been refreshed.
      outcome =
          ClassifyRecordingReadResponse(res == CURLE_OK, httpCode, /*apiKeyAlreadyRefreshed=*/true,
                                        m_recordingStream.position, m_recordingStream.length, serverReportedTotal);
    }

    switch (outcome)
    {
    case RecordingReadOutcome::kRefreshApiKeyAndRetry: // not reachable: the call above passed `true`
    case RecordingReadOutcome::kFailPermanently:
      return FailRecordingReadPermanently(httpCode);
    case RecordingReadOutcome::kRetryTransient:
    {
      if (res != CURLE_OK)
      {
        // A transport-level failure, as opposed to a bad HTTP status, might
        // mean the reused connection went stale/dead -- e.g. the server or an
        // intervening proxy silently closed a keep-alive connection during a
        // long pause. Drop the handle so the next read opens a fresh
        // connection instead of retrying the same broken one indefinitely.
        curl_easy_cleanup(curl);
        m_recordingStream.curl = nullptr;
      }
      ++transientFailures;
      const auto waited =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - readStart);
      if (!m_abortRequests && ShouldKeepRetryingTransientRead(waited, kTransientReadBudget))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(SegmentFetchRetryDelayMs(transientFailures)));
        continue;
      }
      return -1;
    }
    case RecordingReadOutcome::kRangeIgnored:
      // A plain 200 at a non-zero offset means the server ignored the Range header and sent
      // the file from its first byte: keeping those bytes would splice the wrong data into
      // playback. End the stream instead, once, with a message -- skipping to the offset
      // would mean downloading everything before it on every read.
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr-unofficial: ReadRecordingStream: the server answered a ranged read at byte %lld "
                "with a plain 200 (it ignored the Range header -- a proxy in front of Dispatcharr that drops it?); "
                "stopping this recording rather than play the wrong bytes",
                static_cast<long long>(m_recordingStream.position));
      kodi::QueueNotification(QUEUE_ERROR, "",
                              "This recording can't be played: the server (or a proxy in front of it) doesn't "
                              "support partial downloads.");
      m_recordingStream.rangeIgnored = true;
      return 0;
    case RecordingReadOutcome::kFileChanged:
      // See dispatcharr::HasRecordingFileChanged() (RecordingHttpUtil.h): a real, confirmed risk (Dispatcharr's own
      // comskip "cut" mode can replace this exact file with a shorter one while a client is already reading it) this
      // addon's own analogous live-timeshift read path already guards against the same way. Fail loudly rather than
      // silently keep reading the new file's bytes at this stream's own now-stale offsets.
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr-unofficial: ReadRecordingStream: recording's own file size "
                "(%lld, from Content-Range) disagrees with the length cached when the stream "
                "was opened (%lld) -- the file was likely replaced under this stream (e.g. "
                "comskip post-processing); giving up on this read rather than risk silent "
                "corruption",
                static_cast<long long>(serverReportedTotal), static_cast<long long>(m_recordingStream.length));
      return FailRecordingReadPermanently(httpCode);
    case RecordingReadOutcome::kOk:
      break;
    }

    m_recordingStream.position += static_cast<int64_t>(sink.written);
    ResetPermanentReadFailures(m_recordingStream.permanentReadFailures);
    return static_cast<int>(sink.written);
  }
  return -1;
}

int64_t DispatcharrClient::SeekRecordingStream(int64_t position, int whence)
{
  if (!m_recordingStream.open)
    return -1;

  int64_t newPos = ResolveSeekPosition(position, whence, m_recordingStream.position, m_recordingStream.length);
  if (newPos < 0)
    return -1;

  m_recordingStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetRecordingStreamLength() const
{
  return m_recordingStream.length;
}

void DispatcharrClient::CloseRecordingStream()
{
  if (m_recordingStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_recordingStream.curl));
  m_recordingStream = RecordingStreamState();
}

bool DispatcharrClient::RefreshLiveManifest(bool force, std::string& error, bool* fatalOut, bool coldStart)
{
  if (fatalOut)
    *fatalOut = false;

  // One refresh at a time (see m_liveRefreshMutex). Two overlapping refreshes both merged the same new
  // segments, splicing a repeated chunk into the byte stream -- reproduced live with a second thread forcing
  // refreshes. A throttled call that finds one already running has nothing to add and returns at once; a forced
  // one (a reader waiting at the tail) waits its turn and fetches again.
  std::unique_lock<std::mutex> refreshLock(m_liveRefreshMutex, std::defer_lock);
  if (force)
    refreshLock.lock();
  else if (!refreshLock.try_lock())
    return true;

  uint64_t session = 0;
  std::string channelUuid, viewerId, accessToken;
  auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    if (!m_liveTimeshiftStream.open)
    {
      error = "no live timeshift stream is open";
      return false;
    }

    // Throttle: ReadLiveTimeshiftStream()'s catch-up-to-the-tail loop (and a tight demux-read loop calling
    // GetLiveTimeshiftStreamLength() between reads) can end up calling this far more often than the buffer
    // could possibly have grown -- segment_seconds is typically several real seconds, so refetching more than
    // a couple of times a second just adds load without finding anything new. The check itself lives in
    // dispatcharr::ShouldThrottleRefresh() (Staleness.h, shared with RefreshInProgressRecordingManifest()).
    constexpr auto kMinRefreshInterval = std::chrono::milliseconds(500);
    if (ShouldThrottleRefresh(force, m_liveTimeshiftStream.lastManifestFetch, now, kMinRefreshInterval))
      return true;

    session = m_liveSession;
    channelUuid = m_liveTimeshiftStream.channelUuid;
    viewerId = m_liveTimeshiftStream.viewerId;
    accessToken = m_liveTimeshiftStream.accessToken;
  }

  const long requestTimeoutMs = coldStart ? 0 : ShortRequestTimeoutMs();
  if (!EnsureAuthenticated(error, requestTimeoutMs))
    return false;

  // viewer_id: this call is the *only* thing a paused client still sends -- SendTimeshiftHeartbeat() only ever
  // fires from ReadLiveTimeshiftStream(), which a pause stops calling entirely, but GetStreamTimes() keeps
  // polling GetLiveTimeshiftStreamLength() (and so this function) throughout. Without it a paused viewer's own
  // per-viewer heartbeat went stale and got pruned by the plugin's reaper (confirmed live, docs/OPEN_ITEMS.md).
  json params = {{"channel_uuid", channelUuid}, {"viewer_id", viewerId}};
  // Sent so the plugin can positively detect that this buffer was replaced by a different viewer's start_buffer
  // after this one's own ffmpeg died (docs/OPEN_ITEMS.md): it answers fatal: true on a mismatch instead of
  // silently serving the new buffer's unrelated segments, whose sequence numbers this function's own
  // merge-by-sequence logic would otherwise just filter out as "not new" while every segment read 403s against
  // the stale token.
  if (!accessToken.empty())
    params["access_token"] = accessToken;
  json body = {
      {"action", "get_live_manifest"},
      {"params", params},
  };
  json response;
  if (!Request("POST", kTimeshiftPluginRunPath, body, response, error, /*withAuth=*/true, /*retryOnAuthFailure=*/1,
               nullptr, requestTimeoutMs))
    return false;
  json result;
  if (!UnwrapPluginRunResult(response, "timeshift_buffer", result, error))
  {
    // result is still populated (empty json() if the outer envelope itself
    // failed) -- FieldOr() on that safely yields false either way, matching
    // this function's own pre-set default.
    if (fatalOut)
      *fatalOut = FieldOr(result, "fatal", false);
    return false;
  }

  int httpPort = FieldOr(result, "http_port", 0);
  std::string routePrefix = FieldOr<std::string>(result, "segment_route_prefix", "");
  if (httpPort <= 0 || routePrefix.empty() || !result.contains("segments") || !result["segments"].is_array())
  {
    error = "timeshift_buffer plugin manifest response was missing required fields";
    return false;
  }
  // The URL assembly itself lives in dispatcharr::BuildTimeshiftSegmentBaseUrl() (PluginUrlUtil.h) so it's
  // unit-testable standalone.
  std::string newSegmentBaseUrl = BuildTimeshiftSegmentBaseUrl(m_config.host, httpPort, routePrefix);

  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  if (!m_liveTimeshiftStream.open || m_liveSession != session)
  {
    error = "the live timeshift stream was closed while its manifest was being fetched";
    return false;
  }
  m_liveTimeshiftStream.segmentBaseUrl = std::move(newSegmentBaseUrl);

  // Segments come back ordered by sequence; only ones newer than what we already know get appended, each
  // extending OUR cumulative address space (not reusing the response's own relative offsets -- see this method's
  // header comment for why). The filtering itself lives in dispatcharr::ParseNewLiveManifestSegments()
  // (LiveManifestParser.{h,cpp}); byteOffset/timeOffsetMs stay computed here since they're cumulative over this
  // stream's own running totals. Read under the state lock, together with the appends, so what counts as "already
  // known" cannot change in between.
  int64_t lastKnownSequence =
      m_liveTimeshiftStream.segments.empty() ? -1 : m_liveTimeshiftStream.segments.back().sequence;

  for (const auto& entry : ParseNewLiveManifestSegments(result["segments"], lastKnownSequence))
  {
    LiveTimeshiftSegmentInfo info;
    info.filename = entry.filename;
    info.byteSize = entry.byteSize;
    info.sequence = entry.sequence;
    SegmentAppendOffsets offsets = AppendSegmentOffsets(
        entry.byteSize, entry.durationMs, m_liveTimeshiftStream.totalBytes, m_liveTimeshiftStream.totalDurationMs);
    info.byteOffset = offsets.byteOffset;
    info.timeOffsetMs = offsets.timeOffsetMs;
    m_liveTimeshiftStream.segments.push_back(std::move(info));
  }

  // What the plugin's rolling buffer has let go of by now -- see dispatcharr::FirstAvailableLiveSegmentIndex().
  // Only ever moves forward.
  if (FieldOr(result, "ended", false))
    m_liveTimeshiftStream.ended = true;

  const int64_t oldestSequence = OldestLiveManifestSequence(result["segments"]);
  if (oldestSequence > m_liveTimeshiftStream.oldestAvailableSequence)
    m_liveTimeshiftStream.oldestAvailableSequence = oldestSequence;
  // Under the state lock like every other access to the list; nothing keeps an
  // index or pointer into it across an unlock (the read path copies a segment's
  // fields out before it releases the lock).
  PruneRolledOffLiveSegments(m_liveTimeshiftStream.segments, m_liveTimeshiftStream.oldestAvailableSequence);

  m_liveTimeshiftStream.lastManifestFetch = now;
  return true;
}

bool DispatcharrClient::OpenLiveTimeshiftStream(const std::string& channelUuid, std::string& error)
{
  // Unlike OpenInProgressRecordingStream() (which already calls its own
  // CloseInProgressRecordingStream() first), this used to just overwrite
  // m_liveTimeshiftStream with a fresh default-constructed one directly --
  // safe in the normal case (Kodi always closes before reopening), but a
  // defensive gap found via a project-wide review: if Open() were ever
  // called again while a stream was already open (no live reproduction;
  // Kodi's own calling convention is assumed, not verified, to always
  // Close() first), the previous m_liveTimeshiftStream.curl handle would
  // leak (only ever cleaned up in CloseLiveTimeshiftStream(), never here)
  // and the previous viewer_id would never be deregistered server-side
  // (self-healing via the plugin's own idle-timeout reaper eventually,
  // but not immediately). Unconditional, matching the sibling function's
  // own pattern -- CloseLiveTimeshiftStream()'s own body already no-ops
  // cheaply when nothing was open (skips StopTimeshiftBuffer(), and
  // curl_easy_cleanup()/the state reset are both harmless against an
  // already-default state).
  CloseLiveTimeshiftStream();

  // This addon's own local bookkeeping always starts fresh on Open() --
  // the merge-by-sequence-number logic in RefreshLiveManifest() below
  // repopulates it from whatever the plugin's buffer currently holds,
  // which is the *server-side* buffer's history, not this addon's own.
  // (See the trim step further down: repopulating from everything the
  // buffer currently holds is an intermediate state, not the final local
  // address space this method leaves in place.)
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    m_liveTimeshiftStream = LiveTimeshiftStreamState();
    ++m_liveSession;
  }

  // Deliberately does NOT stop a buffer already running for this channel
  // before starting/reattaching -- see docs/TIMESHIFT.md's "Concurrent
  // viewers" section for the real, live-confirmed bug this used to cause:
  // a second viewer's Open() killed the first viewer's still-playing
  // buffer outright, and (via CloseLiveTimeshiftStream()'s own former
  // symmetric stop-on-close) the first viewer's eventual Close() then
  // killed the second viewer's fresh replacement too -- a cascading
  // failure that left *both* viewers broken, not just the first.
  //
  // This addon previously stopped-then-restarted the buffer on every
  // Open() specifically to fix a *different*, also real bug: seeking
  // after a Stop/reopen of the same channel used to silently fall back to
  // byte 0 instead of the requested target (see docs/TIMESHIFT.md's
  // "Fixed: seeking after a Stop/reopen didn't land on target"). Simply
  // no longer stopping the buffer here was **tried and confirmed live to
  // reintroduce that exact regression**: reattaching to a buffer that had
  // kept running (never restarted) since an earlier session, then seeking,
  // reproduced the identical failure signature (`CDVDDemuxFFmpeg::SeekTime`
  // landing on a garbage time near the MPEG-TS 33-bit PTS wraparound point)
  // from a plain -20s relative seek. Kodi's own demuxer genuinely can't
  // reliably seek backward into buffer content *this* demuxer instance
  // hasn't itself read forward through this session -- true regardless of
  // whether that content is part of one continuous, never-restarted
  // encoder run, which rules out "just don't restart the buffer" as a
  // complete fix on its own. The trim step below (discarding everything
  // except a small trailing window before this method returns) is what
  // actually restores correct seeking while still not touching the
  // server-side buffer -- see its own comment for the full reasoning and
  // what this means for the "join a running buffer and rewind into its
  // pre-join history" feature this was investigated alongside (not
  // achievable, confirmed by the same test).
  //
  // Buffer cleanup when a viewer stops watching is now the plugin's own
  // job via viewer reference counting, not something Open()/Close() force
  // by unconditionally stopping the buffer (see LiveTimeshiftStreamState::
  // viewerId's own comment and StopTimeshiftBuffer()'s for the mechanism).
  // A fresh viewer_id here registers this Open() as one of the buffer's
  // viewers; CloseLiveTimeshiftStream() deregisters it, and the plugin
  // only actually stops the buffer once no registered viewers remain --
  // safe with any number of concurrent viewers, and fast (no need to wait
  // out the plugin's own heartbeat idle-timeout, which stays only as a
  // backstop for a viewer that disappears without cleanly closing, e.g. a
  // crash). See docs/TIMESHIFT.md's "Concurrent viewers" section: an
  // earlier version of this fix relied on that idle-timeout alone for all
  // cleanup, which turned out to be a real problem of its own -- a
  // provider's own concurrent-stream limit stayed exhausted by an
  // orphaned buffer for up to that timeout's duration after its only real
  // viewer stopped, blocking a *different* channel from opening at all in
  // the meantime.
  const std::string viewerId = GenerateViewerId();
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    m_liveTimeshiftStream.viewerId = viewerId;
  }

  // Retries briefly on the plugin's own "retryable" signal (currently
  // just its buffer being mid-teardown, see StartTimeshiftBuffer()'s own
  // comment) instead of failing outright on the first attempt -- a real
  // gap found via a project-wide review: this window is normally brief
  // (bounded by the plugin's own _stop_ffmpeg()'s ~2s SIGTERM deadline
  // plus file removal), so a caller opening the same channel inside it
  // deserves a real chance to land just after teardown finishes, not an
  // immediate hard failure.
  // 20 x 500 ms: the plugin's teardown (SIGTERM grace, then removing a large buffer on slow storage) can outlast
  // the 3 s this used to wait, and a Close whose stop_buffer was cut off at ShortRequestTimeoutMs() leaves the buffer
  // mid-teardown for the quick same-channel reopen (fifteenth hardening sweep).
  constexpr int kStartBufferRetryableMaxAttempts = 20;
  constexpr int kStartBufferRetrySleepMs = 500;
  std::string unusedPlaylistUrl;
  bool started = false;
  bool lastAttemptRetryable = false;
  for (int attempt = 0; attempt < kStartBufferRetryableMaxAttempts; ++attempt)
  {
    bool retryable = false;
    if (StartTimeshiftBuffer(channelUuid, unusedPlaylistUrl, error, &retryable))
    {
      started = true;
      break;
    }
    lastAttemptRetryable = retryable;
    if (!retryable)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kStartBufferRetrySleepMs));
  }
  if (!started)
  {
    // Same reasoning as the `!manifestReady` cleanup further down: a
    // StartTimeshiftBuffer() call can fail here in a way that isn't
    // actually clean server-side -- a lost/timed-out response after the
    // plugin's own start_buffer already registered this viewer (see
    // CallTimeshiftPluginAction()'s own http_port/playlist_route
    // validation), or a non-retryable logical failure on a *reattach*
    // that still ran plugin-side. Without this, the viewer this method
    // registered via m_liveTimeshiftStream.viewerId above stays
    // registered server-side, and the buffer (and the provider slot it
    // holds) keeps running until the plugin's own idle-timeout reaper
    // eventually notices -- a real gap found via a project-wide review,
    // not itself independently reproduced. Harmless in the ordinary case
    // (nothing was ever actually registered): the plugin's own
    // stop_buffer just replies "no buffer was running".
    //
    // Skipped when the loop above ran out of attempts while every single
    // one came back "retryable" (`lastAttemptRetryable` still true here)
    // -- a real, confirmed gap found via a project-wide review, not
    // itself independently reproduced. That signal fires BEFORE any
    // registration attempt in the plugin's own start_buffer (see
    // StartTimeshiftBuffer()'s own comment: it means a buffer for this
    // channel is already mid-teardown elsewhere), so exhausting every
    // retry while it kept firing means, for certain, this viewer was
    // never registered anywhere -- nothing to clean up. Calling
    // stop_buffer anyway in that specific case risked tearing down a
    // *different*, unrelated buffer instance that a third caller had
    // legitimately restarted for this same channel in the meantime
    // (a buffer already reported mid-teardown is exactly the same
    // stale-teardown race already logged in docs/OPEN_ITEMS.md, just
    // reached via this path instead). Every other failure reason -- an
    // explicit non-retryable plugin refusal, or an ambiguous
    // transport-level failure where the response was simply lost --
    // still cleans up, since either could genuinely have left a
    // registration behind server-side.
    if (!lastAttemptRetryable)
    {
      std::string stopError;
      StopTimeshiftBuffer(channelUuid, viewerId, stopError);
    }
    {
      std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
      m_liveTimeshiftStream = LiveTimeshiftStreamState();
      ++m_liveSession;
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    m_liveTimeshiftStream.open = true;
    m_liveTimeshiftStream.channelUuid = channelUuid;
  }

  // Cold-start grace period: RefreshLiveManifest() can legitimately fail
  // here with "live playlist not found" for several real seconds after
  // StartTimeshiftBuffer() returns -- ffmpeg needs to connect to
  // Dispatcharr's live proxy and produce a full first segment
  // (segment_seconds, 6s by default) before there's anything to report.
  // Only a genuinely first-ever start of a channel's buffer (no earlier
  // viewer already has one running) hits this at all now that Open() no
  // longer force-restarts an existing buffer -- reattaching to one already
  // producing segments succeeds on this loop's very first attempt, same as
  // before this addon ever force-restarted anything. Kept from an earlier
  // version of this addon that *did* force-restart on every Open() (where
  // every single reopen paid this cold-start cost, and a failed attempt's
  // own since-removed restart-on-retry made a slow cold start actively
  // worse by repeatedly killing the previous attempt moments before it
  // would have finished) -- retry for a real cold start's worth of time
  // instead of failing on the first check.
  // fatal (as opposed to a plain retry-worthy failure) means the plugin
  // has confirmed ffmpeg already exited and this buffer will never
  // produce a segment on its own -- most commonly an upstream provider's
  // own concurrent-stream limit refusing the connection, confirmed live
  // via a genuine, dedicated test against the provider's own concurrent-stream cap. Breaking
  // out immediately here, instead of burning the full retry budget below
  // (~15s) against something that can't recover, is what actually fixes
  // the slow, unclear failure that test surfaced: without this, a
  // brand-new channel open that happens to be genuinely at the provider's
  // limit (not a race with something about to free up, which
  // CloseLiveTimeshiftStream()'s own synchronous stop already handles --
  // this is the "nothing is closing, the limit is just standing" case)
  // still took the full ~15s to fail instead of a couple hundred
  // milliseconds, with no indication in the error of why.
  constexpr int kColdStartMaxAttempts = 30;
  constexpr int kColdStartSleepMs = 500;
  bool manifestReady = false;
  for (int attempt = 0; attempt < kColdStartMaxAttempts; ++attempt)
  {
    bool fatal = false;
    if (RefreshLiveManifest(/*force=*/true, error, &fatal, /*coldStart=*/true))
    {
      manifestReady = true;
      break;
    }
    if (fatal)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(kColdStartSleepMs));
  }
  if (!manifestReady)
  {
    // Fix for a real, confirmed bug: this used to just reset local state
    // and return, leaving the viewer this method itself registered via
    // StartTimeshiftBuffer() above still registered server-side, and the
    // buffer (and the provider slot it holds) running until the plugin's
    // own idle-timeout reaper eventually noticed and tore it down --
    // several tens of seconds later, during which a *different* channel
    // open could fail on the provider's own concurrent-stream limit, the
    // exact failure CloseLiveTimeshiftStream()'s own synchronous stop
    // (see its comment) exists to prevent for the ordinary
    // switch-channels case. Deregistering here too closes that same gap
    // for a cold start that never actually succeeds. Harmless in the
    // `fatal` case above (the plugin has already torn the buffer down
    // itself and this just gets back a "no buffer was running" reply).
    std::string stopError;
    StopTimeshiftBuffer(channelUuid, viewerId, stopError);
    {
      std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
      m_liveTimeshiftStream = LiveTimeshiftStreamState();
      ++m_liveSession;
    }
    return false;
  }

  // Discard everything except the trailing kLiveEdgeMarginSegments worth of
  // segments from what the cold-start fetch above just returned, rebasing
  // the kept ones so the oldest of them becomes local byte 0 -- **confirmed
  // live** this is necessary even though the buffer above was never
  // stopped/restarted: reattaching to a channel whose buffer had been
  // running a while (this addon's own local state spanning everything the
  // plugin still had, tens of MB/several minutes) and then seeking into the
  // *older* part of that history reproduced the exact failure this addon's
  // Stop/reopen seeking fix (elsewhere in this file) was written to
  // prevent -- `CDVDDemuxFFmpeg::SeekTime` landing on a garbage time near
  // the MPEG-TS 33-bit PTS wraparound point (~26.5h) instead of anywhere
  // near the requested target, from a plain -20s relative seek, no
  // multi-minute rewind involved. Kodi's own demuxer, it turns out, still
  // can't reliably seek backward into buffer content *this* demuxer
  // instance hasn't itself read forward through this session, regardless of
  // whether that content is part of one continuous, never-restarted
  // encoder run -- continuity alone doesn't fix it, only trimming this
  // addon's own exposed address space down to what a genuinely fresh
  // session would have does. See docs/TIMESHIFT.md's "Concurrent viewers"
  // section for the full account, including why this also answers (in the
  // negative) whether a viewer can join an already-running buffer and
  // rewind into history from before they joined.
  // The trim itself lives in dispatcharr::TrimToTrailingLiveEdgeMargin()
  // (LiveEdgeMargin.h) so it's unit-testable standalone -- see that
  // function's own comment.
  constexpr size_t kLiveEdgeMarginSegments = 3;
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  TrimToTrailingLiveEdgeMargin(m_liveTimeshiftStream.segments, m_liveTimeshiftStream.totalBytes,
                               m_liveTimeshiftStream.totalDurationMs, kLiveEdgeMarginSegments);

  // See LiveTimeshiftStreamState::wallClockAnchor's own comment -- this is
  // the real-world moment local byte 0/PTS 0 above now corresponds to,
  // approximated as "now" (the kept margin segments span at most a few
  // seconds, not worth tracking more precisely for what this feeds).
  m_liveTimeshiftStream.wallClockAnchor = std::time(nullptr);

  // Start near the live edge, not the earliest content still known about in
  // the (now-trimmed) address space above -- position defaults to 0, which
  // without this would replay from the start of that trimmed window every
  // time a channel is (re)opened, rather than resuming at "now" the way a
  // plain live feed does. The OSD's rewind range reaches back to this
  // session's own local byte 0 (the trim above, not the buffer's true
  // beginning) via GetStreamTimes()/SeekLiveStream(); this only changes
  // where playback starts.
  //
  // Deliberately a few segments *behind* totalBytes, not exactly at it:
  // ffmpeg only exposes a segment once it's fully closed (segment_seconds
  // apart, 6s by default), so sitting exactly at the tail means there's
  // nothing to read until the next segment closes -- confirmed live, this
  // produced a periodic "stream stalled"/rebuffer cycle in Kodi tracking
  // segment_seconds almost exactly, independent of channel bitrate (a
  // fresh/small buffer has ~zero margin regardless of which channel it
  // is, which is what actually explained the earlier channel-to-channel
  // difference -- not a throughput problem, as first suspected). A few
  // segments' cushion gives the demuxer's own read-ahead something to
  // draw on between segment arrivals, while staying clearly "live" to the
  // viewer -- comparable to the inherent latency any real live-TV/DVR
  // service already has. Falls back to the true tail (0 margin) if fewer
  // than that many segments exist yet, e.g. right after a cold
  // StartTimeshiftBuffer() -- nothing to back up from yet in that case.
  // (After the trim above, segments.size() here is always <=
  // kLiveEdgeMarginSegments, so this always lands on the trimmed window's
  // own start -- kept in this same "few segments behind the tail" shape
  // rather than simplified to a flat 0, since a genuinely cold buffer with
  // fewer than kLiveEdgeMarginSegments segments still needs the tail
  // fallback below.)
  // The formula itself lives in dispatcharr::ComputeLiveEdgeStartPosition()
  // (LiveEdgeMargin.h) so it's unit-testable standalone -- see that
  // function's own comment.
  m_liveTimeshiftStream.position = ComputeLiveEdgeStartPosition(
      m_liveTimeshiftStream.segments, m_liveTimeshiftStream.totalBytes, kLiveEdgeMarginSegments);
  return true;
}

int DispatcharrClient::HandleLiveTimeshiftSegmentFetchFailure()
{
  // See dispatcharr::ShouldGiveUpAfterSegmentFetchFailure()'s own comment
  // (SegmentFetchFailure.h) for why 4 -- a real transient blip (a one-off
  // 5xx from Dispatcharr's own reverse proxy) resolves within a handful of
  // attempts; anything that doesn't isn't going to resolve itself no
  // matter how many more times this addon asks.
  constexpr int kMaxConsecutiveSegmentFetchFailures = 4;
  // Called with no lock held (it refreshes the manifest and sleeps), like everything below that does a network
  // call -- see m_liveStateMutex.
  uint64_t session;
  int failures;
  std::chrono::steady_clock::time_point failureStreakStart;
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    session = m_liveSession;
    failures = ++m_liveTimeshiftStream.consecutiveSegmentFetchFailures;
    if (failures == 1)
      m_liveTimeshiftStream.firstSegmentFetchFailureAt = std::chrono::steady_clock::now();
    failureStreakStart = m_liveTimeshiftStream.firstSegmentFetchFailureAt;
  }

  std::string refreshError;
  bool refreshFatal = false;
  RefreshLiveManifest(/*force=*/true, refreshError, &refreshFatal);

  // The streak has to have lasted this long as well as reached the count: a server that is briefly unreachable
  // (a restart, a blip) fails four times in about two seconds, and playback resumes fine once it is back.
  constexpr std::chrono::milliseconds kMinSegmentFetchFailureStreak{30000};
  const auto failingFor =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - failureStreakStart);
  if (ShouldGiveUpAfterSegmentFetchFailure(failures, refreshFatal, kMaxConsecutiveSegmentFetchFailures, failingFor,
                                           kMinSegmentFetchFailureStreak))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: giving up after %d consecutive segment fetch "
              "failure(s) (manifest refresh reported fatal=%s%s%s) -- marking stream fatal",
              failures, refreshFatal ? "true" : "false", refreshError.empty() ? "" : ": ", refreshError.c_str());
    {
      std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
      if (m_liveSession == session)
        m_liveTimeshiftStream.fatal = true;
    }
    return -1;
  }

  // Bounded retry with a growing delay, then another try inside the same Read() call -- see
  // ReadLiveTimeshiftStream(). Returning 0 to Kodi here instead was measured live to end playback: with
  // nothing buffered ahead (at the live edge), a few zero-byte reads in a row inside five or six seconds and
  // Kodi treats the stream as ended, however long the outage really is.
  std::this_thread::sleep_for(std::chrono::milliseconds(SegmentFetchRetryDelayMs(failures)));
  return kRetrySegmentFetch;
}

int DispatcharrClient::ReadLiveTimeshiftStream(uint8_t* buffer, unsigned int size)
{
  // A segment fetch that fails (the server unreachable, a gateway error) is retried inside this one call, for up
  // to the budget, rather than handed back to Kodi as a zero-byte read: Kodi ends the stream after a few of
  // those in quick succession when it has nothing buffered, which turned a brief outage into a dead stream
  // (docs/OPEN_ITEMS.md, "A brief server outage ended playback for good"). A blocked read is just buffering to
  // the viewer, and the give-up rule in HandleLiveTimeshiftSegmentFetchFailure() still ends a stream that stays
  // dead.
  constexpr std::chrono::milliseconds kOutageReadBudget{25000};
  const auto start = std::chrono::steady_clock::now();
  for (;;)
  {
    const int result = ReadLiveTimeshiftStreamOnce(buffer, size);
    if (result != kRetrySegmentFetch)
      return result;
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (m_abortRequests || !ShouldKeepRetryingTransientRead(waited, kOutageReadBudget))
      return 0;
  }
}

int DispatcharrClient::ReadLiveTimeshiftStreamOnce(uint8_t* buffer, unsigned int size)
{
  // Held for the state reads and writes below and released around every network call and sleep (see
  // m_liveStateMutex); after each re-lock the stream is checked to still be the one this call started on.
  std::unique_lock<std::mutex> stateLock(m_liveStateMutex);
  if (!m_liveTimeshiftStream.open || size == 0)
    return 0;
  const uint64_t session = m_liveSession;
  auto stillSameStream = [&]() { return m_liveTimeshiftStream.open && m_liveSession == session; };
  // See LiveTimeshiftStreamState::fatal's own comment -- a confirmed-dead
  // buffer short-circuits here instead of paying for another doomed
  // network round trip on every single Read() call.
  if (m_liveTimeshiftStream.fatal)
    return -1;

  // A position behind the oldest segment the plugin still has (a long pause,
  // or playback simply left far behind live, while its rolling buffer moved
  // on) can only be answered with a recycled file's unrelated content, which
  // the size cross-check below then rightly treats as fatal -- found live
  // (docs/OPEN_ITEMS.md): pausing for longer than twice the buffer length and
  // resuming ended the whole stream. Resume from the oldest segment that is
  // still real instead; MPEG-TS segments start on a keyframe, so the demuxer
  // picks up from there.
  {
    const int64_t firstAvailable = FirstAvailableLiveByteOffset();
    if (m_liveTimeshiftStream.position < firstAvailable)
    {
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: position %lld is behind the oldest segment the "
                "timeshift buffer still holds -- continuing from byte %lld",
                static_cast<long long>(m_liveTimeshiftStream.position), static_cast<long long>(firstAvailable));
      m_liveTimeshiftStream.position = firstAvailable;
    }
  }

  // Opportunistically refresh this viewer's own heartbeat on an interval
  // comfortably under idle_timeout_seconds' default (30s) -- see
  // SendTimeshiftHeartbeat()'s own comment for why. Piggybacked on
  // ordinary reads (this function is already called continuously for as
  // long as playback continues) rather than a dedicated thread, so the
  // occasional extra round trip lands on whichever read happens to cross
  // the interval -- SendTimeshiftHeartbeat() is itself responsible for
  // keeping that bounded to a small, fixed worst case (its own comment has
  // the full story, including a live-reproduced total playback stall in
  // 1.0.5 before it was bounded this way); this call site doesn't add any
  // timeout of its own on top.
  {
    constexpr auto kHeartbeatInterval = std::chrono::seconds(10);
    auto now = std::chrono::steady_clock::now();
    if (!m_liveTimeshiftStream.viewerId.empty() &&
        (m_liveTimeshiftStream.lastHeartbeatSent.time_since_epoch().count() == 0 ||
         now - m_liveTimeshiftStream.lastHeartbeatSent >= kHeartbeatInterval))
    {
      m_liveTimeshiftStream.lastHeartbeatSent = now;
      const std::string heartbeatChannel = m_liveTimeshiftStream.channelUuid;
      const std::string heartbeatViewer = m_liveTimeshiftStream.viewerId;
      stateLock.unlock();
      SendTimeshiftHeartbeat(heartbeatChannel, heartbeatViewer);
      stateLock.lock();
      if (!stillSameStream())
        return -1;
    }
  }

  // Caught up to the tail: give the buffer a bounded chance to grow rather
  // than reporting EOF immediately, which Kodi would read as "this live
  // stream just ended". A fixed 8 attempts * 250ms (2s total) here used to
  // be *far* short of the real gap between segments (confirmed live via
  // timing instrumentation: with the plugin's segment_seconds default of
  // 6s, this loop gave up almost every time, Kodi immediately retried the
  // read, landed right back in this same loop, and repeated -- 2-4 full
  // "gave up" cycles before a new segment actually existed was common,
  // turning what should be one ~6s wait into 12-16+ seconds. That's not
  // just wasted time during ordinary near-live playback (absorbed by
  // Kodi's own read-ahead cache most of the time, so not usually a visible
  // stall) -- it also directly padded out seek latency, since ffmpeg's own
  // internal seek probing routinely lands at/near the live edge for any
  // seek originating near "now", and each such probe paid this same cost.
  // Size the budget off the last known segment's own duration (with
  // margin) instead of a fixed guess, so it comfortably covers one real
  // gap between segments regardless of how segment_seconds is configured.
  if (IsAtEndedTail(m_liveTimeshiftStream.ended, m_liveTimeshiftStream.position, m_liveTimeshiftStream.totalBytes))
  {
    // ffmpeg has exited and this reader has caught up to everything it ever wrote: nothing
    // more is coming, so end rather than wait out the catch-up budget on every read.
    if (!m_liveTimeshiftStream.endedLogged)
    {
      m_liveTimeshiftStream.endedLogged = true;
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: the timeshift buffer's ffmpeg has exited and "
                "playback reached the end of what it recorded -- ending the stream");
    }
    return 0; // EOF: -1 is retried near-immediately by Kodi and never ends playback
  }
  if (m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes)
  {
    constexpr int kCatchUpSleepMs = 250;

    // A read landing at the tail shortly after a seek is far more likely
    // to be one of ffmpeg's own internal probes (its generic mpegts seek
    // does a real multi-step search, confirmed live via SeekLiveTimeshiftStream
    // tracing -- several probes in quick succession, one of which commonly
    // overshoots right up to the current tail while estimating) than a
    // genuine "caught up to live, please wait" read. Blocking a probe for
    // a full segment interval was the single largest contributor to seek
    // latency measured live (a 4.2s wait out of one seek's total ~4.4s).
    // ffmpeg can usually just try an earlier candidate instead of getting
    // this exact byte -- so give it a quick "not there" rather than making
    // it wait, and reserve the full segment-duration budget below for
    // reads that aren't part of an active seek's own probing.
    // The time window alone isn't quite enough: normal decode reads that
    // happen to land at the tail right after a seek completes (not part of
    // its internal probing at all, just where playback settled) also fall
    // inside it and genuinely need the full wait -- confirmed live giving
    // those the short budget too caused visible playback pauses right
    // after a seek that landed near live. If we already gave up quickly at
    // this *exact* position, it's not a fresh probe candidate anymore --
    // escalate to the full budget rather than repeating the short one
    // indefinitely against the same stuck position.
    constexpr auto kSeekProbeWindow = std::chrono::milliseconds(800);
    bool likelySeekProbe =
        IsLikelySeekProbe(m_liveTimeshiftStream.position, m_liveTimeshiftStream.lastShortGiveUpPosition,
                          m_liveTimeshiftStream.lastSeekTime, std::chrono::steady_clock::now(), kSeekProbeWindow);

    // Segment-duration estimate (averaged + floored) and the resulting
    // attempt budget (3x margin) are shared with
    // ReadInProgressRecordingStream() -- see EstimateSegmentDurationMs()'s
    // and ComputeCatchUpAttempts()'s own comments (just above that function)
    // for the full history of why each constant is what it is, including a
    // real crash this fixed. A separate server-side bug (see the
    // timeshift_buffer plugin's own history) was the dominant cause of the
    // *worst*, multi-second stalls on this particular path, but the margin
    // was measurably too tight even independent of that.
    int64_t segmentDurationEstimateMs =
        EstimateSegmentDurationMs(m_liveTimeshiftStream.totalDurationMs, m_liveTimeshiftStream.segments);
    int catchUpAttempts = ComputeCatchUpAttempts(likelySeekProbe, segmentDurationEstimateMs, kCatchUpSleepMs);

    const int64_t catchUpBudgetMs = ComputeCatchUpWallClockBudgetMs(catchUpAttempts, kCatchUpSleepMs);
    auto catchUpStart = std::chrono::steady_clock::now();
    int attemptsUsed = 0;
    for (int attempt = 0;
         attempt < catchUpAttempts && m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes; ++attempt)
    {
      attemptsUsed = attempt + 1;
      std::string refreshError;
      bool refreshFatal = false;
      // fatalOut wired up here too, not just OpenLiveTimeshiftStream()'s own
      // cold-start loop -- see LiveTimeshiftStreamState::fatal's own
      // comment for the bug this fixes (a buffer that dies mid-playback,
      // not just one that never started, retried forever with no
      // indication anything was actually wrong).
      stateLock.unlock();
      RefreshLiveManifest(/*force=*/true, refreshError, &refreshFatal);
      stateLock.lock();
      if (!stillSameStream())
        return -1;
      if (refreshFatal)
      {
        m_liveTimeshiftStream.fatal = true;
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: timeshift buffer reported fatal "
                  "mid-playback, giving up: %s",
                  refreshError.c_str());
        break;
      }
      if (m_liveTimeshiftStream.position < m_liveTimeshiftStream.totalBytes)
        break;
      if (m_liveTimeshiftStream.ended)
        break; // the refresh just learned ffmpeg is gone; waiting longer cannot help
      if (HasCatchUpBudgetElapsed(
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - catchUpStart),
              catchUpBudgetMs))
        break; // the server is not answering promptly -- see kMaxCatchUpWallClockMs
      // Don't sleep after the last attempt -- nothing left to wait for
      // before giving up, and for a likely seek probe (catchUpAttempts==1)
      // this is what makes the "not there yet" response fast instead of
      // paying a pointless 250ms before reporting it.
      if (attempt + 1 < catchUpAttempts)
      {
        stateLock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(kCatchUpSleepMs));
        stateLock.lock();
        if (!stillSameStream())
          return -1;
      }
    }
    if (m_liveTimeshiftStream.fatal)
      return -1;
    if (IsAtEndedTail(m_liveTimeshiftStream.ended, m_liveTimeshiftStream.position, m_liveTimeshiftStream.totalBytes))
      return 0; // learned during the loop above; EOF
    bool caughtUp = m_liveTimeshiftStream.position < m_liveTimeshiftStream.totalBytes;
    m_liveTimeshiftStream.lastShortGiveUpPosition =
        ComputeShortGiveUpPosition(likelySeekProbe, caughtUp, m_liveTimeshiftStream.position);

    double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - catchUpStart).count();
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: catch-up-to-tail loop used %d/%d "
              "attempts, %.3fs (budget %.1fs off ~%lldms/segment estimate), position=%lld "
              "totalBytes=%lld -> %s",
              attemptsUsed, catchUpAttempts, elapsedSec, catchUpAttempts * kCatchUpSleepMs / 1000.0,
              static_cast<long long>(segmentDurationEstimateMs), static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(m_liveTimeshiftStream.totalBytes), caughtUp ? "caught up" : "gave up");
  }
  if (m_liveTimeshiftStream.position >= m_liveTimeshiftStream.totalBytes)
    return 0; // genuinely nothing new yet

  const LiveTimeshiftSegmentInfo* seg =
      FindSegmentContainingPosition(m_liveTimeshiftStream.position, m_liveTimeshiftStream.segments);
  if (!seg)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: position=%lld is a gap (totalBytes=%lld, "
              "segments=%zu, first seg byteOffset=%lld, last seg end=%lld)",
              static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(m_liveTimeshiftStream.totalBytes), m_liveTimeshiftStream.segments.size(),
              m_liveTimeshiftStream.segments.empty()
                  ? -1LL
                  : static_cast<long long>(m_liveTimeshiftStream.segments.front().byteOffset),
              m_liveTimeshiftStream.segments.empty()
                  ? -1LL
                  : static_cast<long long>(m_liveTimeshiftStream.segments.back().byteOffset +
                                           m_liveTimeshiftStream.segments.back().byteSize));
    return 0; // position points into a gap/rolled-off region -- nothing safely readable here
  }

  // Copied out of `seg` immediately rather than kept as a live pointer: the
  // state lock is released across the blocking curl_easy_perform() call below
  // (see m_liveStateMutex), and GetStreamTimes() may run on a different Kodi
  // thread and call RefreshLiveManifest(), whose push_back can reallocate this
  // vector's backing storage meanwhile and leave `seg` dangling.
  int64_t segByteOffset = seg->byteOffset;
  int64_t segByteSize = seg->byteSize;
  int64_t segSequence = seg->sequence;
  std::string segFilename = seg->filename;

  int64_t offsetInSegment = m_liveTimeshiftStream.position - segByteOffset;
  int64_t available = segByteSize - offsetInSegment;
  unsigned int wantSize = static_cast<unsigned int>(std::min<int64_t>(size, available));

  int64_t rangeEnd = offsetInSegment + static_cast<int64_t>(wantSize) - 1;
  std::string range = std::to_string(offsetInSegment) + "-" + std::to_string(rangeEnd);
  // ?token=... is required by the plugin's own file server (see
  // StartTimeshiftBuffer()/CallTimeshiftPluginAction()'s own comment for
  // where accessToken comes from and why it needs no URL-escaping).
  std::string url = m_liveTimeshiftStream.segmentBaseUrl + segFilename + "?token=" + m_liveTimeshiftStream.accessToken;
  // Logged in place of `url` below -- that token grants unauthenticated
  // read access to this buffer's segments on the plugin's own exposed
  // port for the buffer's whole lifetime (see _check_access_token in
  // timeshift_buffer/plugin.py), and this project routinely asks testers
  // to share kodi.log; a real gap found via a project-wide review, not
  // itself independently reproduced.
  std::string redactedUrl = m_liveTimeshiftStream.segmentBaseUrl + segFilename + "?token=REDACTED";

  // The persistent handle is in use from here to the end of the transfer, so Close waits on this mutex before
  // freeing it (lock order: the curl mutex before the state mutex).
  const int64_t positionAtFetch = m_liveTimeshiftStream.position;
  stateLock.unlock();
  std::unique_lock<std::mutex> curlLock(m_liveCurlMutex);
  stateLock.lock();
  if (!stillSameStream() || m_liveTimeshiftStream.position != positionAtFetch)
    return -1; // closed, or repositioned by a seek, since the segment was chosen -- Kodi retries the read
  CURL* curl = static_cast<CURL*>(m_liveTimeshiftStream.curl);
  if (!curl)
  {
    curl = curl_easy_init();
    if (!curl)
      return -1;
    m_liveTimeshiftStream.curl = curl;
  }

  FixedBufferSink sink{buffer, wantSize, 0};
  // Cross-checked against seg->byteSize below -- see that check's own
  // comment for why this matters (it's the addon's only independent way
  // to catch a manifest-reported byte_size that doesn't match the real
  // file, before that mismatch can silently misalign every later
  // segment's computed byteOffset for the rest of this session).
  int64_t serverReportedTotal = -1;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FixedBufferWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RecordingHeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &serverReportedTotal);
  // Through ApplyStandardCurlOptions(), like the in-progress recording's own segment fetch,
  // so AbortInFlightRequests() can end a fetch stuck on an unresponsive server --
  // CloseLiveTimeshiftStream() waits on the curl mutex this fetch holds (added 2026-10-03,
  // docs/OPEN_ITEMS.md's "Kodi's own threads waited on the guide fetch, and two
  // transfers could not be aborted at shutdown").
  ApplyStandardCurlOptions(curl, GetCurlShare());

  auto segmentFetchStart = std::chrono::steady_clock::now();
  stateLock.unlock();
  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  stateLock.lock();

  // Same reasoning as ReadInProgressRecordingStream()'s own identical log
  // line: this was previously the ONE unlogged blocking network call in
  // this read path -- a reachability/firewall/wrong-port failure (see
  // docs/OPEN_ITEMS.md's own account of a real tester bug report) surfaced
  // to Kodi only as a generic downstream demuxer-probe error, with nothing
  // in this addon's own log pointing at why.
  double fetchSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - segmentFetchStart).count();
  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: segment body fetch took %.3fs, "
            "curlResult=%d httpCode=%ld url=%s",
            fetchSec, static_cast<int>(res), httpCode, redactedUrl.c_str());

  // The order of the checks below is dispatcharr::ClassifyLiveSegmentResponse()'s (RecordingHttpUtil.h, tested).
  const LiveSegmentOutcome outcome =
      ClassifyLiveSegmentResponse(res == CURLE_OK, httpCode, offsetInSegment, serverReportedTotal, segByteSize);
  if (outcome == LiveSegmentOutcome::kTransportFailure)
  {
    // Unlike the debug-only line above, this is logged regardless of
    // debug_logging -- a real tester bug report (docs/OPEN_ITEMS.md) took
    // a multi-round diagnosis to trace an otherwise-silent failure here
    // (segment-server port unreachable) back to this exact call, with
    // kodi.log showing only Kodi-core's own generic downstream
    // "error probing input format" and nothing from this addon at all.
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: segment body fetch failed: %s (url=%s)",
              curl_easy_strerror(res), redactedUrl.c_str());
    // Same "reused connection went stale" handling as ReadRecordingStream().
    curl_easy_cleanup(curl);
    m_liveTimeshiftStream.curl = nullptr;
    curlLock.unlock();
    stateLock.unlock();
    return HandleLiveTimeshiftSegmentFetchFailure();
  }
  if (outcome == LiveSegmentOutcome::kSegmentGone)
  {
    // Segment got recycled between our manifest fetch and this read -- a
    // real, expected race for a rolling buffer (the same one plugin.py's
    // own do_GET/_create_snapshot already tolerate). Treat as "nothing
    // readable here" rather than a hard error. Also a positive sign this
    // buffer's own file server is alive and answering normally, so this
    // resets the same failure count a genuine error would have been
    // building toward -- see HandleLiveTimeshiftSegmentFetchFailure()'s
    // own comment.
    m_liveTimeshiftStream.consecutiveSegmentFetchFailures = 0;
    m_liveTimeshiftStream.firstSegmentFetchFailureAt = {};
    return 0;
  }
  // The same plain-200-at-a-non-zero-offset check as ReadRecordingStream(): the plugin's file
  // server always answers a Range GET with 206, so a 200 here is something in between ignoring
  // Range, and the bytes would be the segment's first ones spliced in at `offsetInSegment`. Not
  // retried -- nothing about a retry changes it -- so the stream ends as fatal.
  if (outcome == LiveSegmentOutcome::kRangeIgnored)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: the segment server answered a ranged read at "
              "byte %lld of a segment with a plain 200 (it ignored the Range header -- a proxy in between that "
              "drops it?); marking the stream fatal rather than play the wrong bytes (url=%s)",
              static_cast<long long>(offsetInSegment), redactedUrl.c_str());
    m_liveTimeshiftStream.fatal = true;
    return -1;
  }
  if (outcome == LiveSegmentOutcome::kUnexpectedStatus)
  {
    // Real, confirmed bug this fixes (found live 2026-09-28, see
    // docs/OPEN_ITEMS.md): this used to just `return -1` unconditionally
    // here, forever, for any such status (401/403/5xx) -- see
    // HandleLiveTimeshiftSegmentFetchFailure()'s own comment for the full
    // incident this caused (an unbounded, zero-backoff request storm
    // against a dead buffer, confirmed live at ~7-8 requests/sec
    // sustained indefinitely).
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: segment body fetch returned unexpected HTTP "
              "%ld (url=%s)",
              httpCode, redactedUrl.c_str());
    curlLock.unlock();
    stateLock.unlock();
    return HandleLiveTimeshiftSegmentFetchFailure();
  }

  // The plugin's own file server always answers a Range GET (which this is)
  // with 206 + "Content-Range: bytes X-Y/TOTAL" -- TOTAL is the segment
  // file's real, current size, independent of whatever byte_size the
  // manifest response reported for it earlier. Those two are supposed to
  // always agree (get_live_manifest's own byte_size comes from the exact
  // same stat() this plugin instance would report here) -- but this
  // addon's own cumulative address space treats a segment's byteSize as
  // permanently fixed once merged (RefreshLiveManifest()'s own comment:
  // "an already-known segment's size can't legitimately change"), so if
  // that assumption is ever actually wrong for any reason, every later
  // segment's computed byteOffset silently drifts out of alignment with
  // its real file for the rest of this session -- reads would keep
  // "succeeding" against the wrong bytes, with no error anywhere, until
  // whatever got spliced together stops looking like valid MPEG-TS to
  // Kodi's own demuxer. Catching the very first disagreement here, loudly
  // and immediately, turns that into a clean, diagnosable failure instead.
  if (outcome == LiveSegmentOutcome::kSizeMismatch)
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: ReadLiveTimeshiftStream: segment %s (sequence %lld) real size "
              "(%lld, from Content-Range) disagrees with the manifest-reported size this "
              "session cached (%lld) -- every later segment's computed offset may already be "
              "misaligned; giving up on this stream rather than risk silent corruption",
              segFilename.c_str(), static_cast<long long>(segSequence), static_cast<long long>(serverReportedTotal),
              static_cast<long long>(segByteSize));
    m_liveTimeshiftStream.fatal = true;
    return -1;
  }

  // A seek (or a close) that landed while the state lock was released for the transfer: the bytes in the caller's
  // buffer are from the old position, so handing them back and advancing the NEW position by their length would splice
  // the wrong bytes into the stream. Kodi retries the read.
  if (!stillSameStream() || m_liveTimeshiftStream.position != positionAtFetch)
    return -1;
  m_liveTimeshiftStream.consecutiveSegmentFetchFailures = 0;
  m_liveTimeshiftStream.firstSegmentFetchFailureAt = {};
  m_liveTimeshiftStream.position += static_cast<int64_t>(sink.written);
  return static_cast<int>(sink.written);
}

int64_t DispatcharrClient::SeekLiveTimeshiftStream(int64_t position, int whence)
{
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    if (!m_liveTimeshiftStream.open)
      return -1;
    // See LiveTimeshiftStreamState::fatal's own comment.
    if (m_liveTimeshiftStream.fatal)
      return -1;

    m_liveTimeshiftStream.lastSeekTime = std::chrono::steady_clock::now();
  }

  if (whence == SEEK_END)
  {
    // "End" for a growing stream means the current known tail -- refresh
    // first so a seek-to-live lands as close to the real live edge as
    // possible rather than wherever we last happened to know about. Before
    // the state lock is taken: the refresh is a network call.
    std::string refreshError;
    RefreshLiveManifest(/*force=*/true, refreshError);
  }

  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  if (!m_liveTimeshiftStream.open || m_liveTimeshiftStream.fatal)
    return -1;

  int64_t newPos =
      ResolveSeekPosition(position, whence, m_liveTimeshiftStream.position, m_liveTimeshiftStream.totalBytes);
  bool clampedNegative = newPos < 0;
  if (clampedNegative)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: SeekLiveTimeshiftStream(position=%lld, whence=%d) from "
              "current=%lld -> computed newPos=%lld < 0, failing",
              static_cast<long long>(position), whence, static_cast<long long>(m_liveTimeshiftStream.position),
              static_cast<long long>(newPos));
    return -1;
  }
  // Clamp forward seeks to the known tail -- there's nothing to seek ahead
  // of yet for a genuinely live buffer. Deliberately backed off rather than
  // landing exactly on the absolute tail: confirmed live that landing
  // precisely at totalBytes leaves zero read-ahead margin, so playback
  // resumes, immediately re-catches-up to the (still-)tail within a couple
  // of seconds of real playback, and has to wait through a full
  // segment-production cycle a second time -- long enough (up to one whole
  // segment interval, ~5-7.5s in this instance) to exceed Kodi's own stall
  // tolerance and trigger a visible rebuffer right after what looked like
  // a completed seek. This backoff means at least that much is already
  // available to play immediately, the same "live edge minus a little"
  // margin real-world live players (HLS, DASH) keep for exactly this
  // reason -- imperceptibly behind true live, but enough to absorb normal
  // segment-to-segment timing jitter instead of stuttering on essentially
  // every seek-to-live.
  //
  // Backed off by kLiveEdgeSeekBackoffSegments trailing segments (not just
  // one) -- confirmed live (docs/TIMESHIFT.md's Packet-corrupt section)
  // that a live-edge seek can trigger a severe, cascading H.264
  // decode-error/audio-desync storm on some channels' streams, while an
  // otherwise-identical seek to a genuine mid-buffer point on the same
  // buffer does not. A direct A/B on the same buffer found zero
  // audio-sync-error lines from a mid-buffer seek vs. thousands from a
  // live-edge seek moments later, isolating the newest segment(s)
  // specifically (most plausibly still being written/finalized
  // server-side right when the demuxer resyncs into it) rather than a
  // general property of the stream. Backing off further reduces exposure
  // to that fragile window. 3 matches the margin OpenLiveTimeshiftStream()
  // already keeps for its own cold-start trim.
  // The backoff computation itself lives in
  // dispatcharr::ComputeLiveEdgeTailTarget() (LiveEdgeMargin.h, shared
  // with SeekInProgressRecordingStream()'s own margin-of-1 case) so it's
  // unit-testable standalone -- see that function's own comment.
  constexpr size_t kLiveEdgeSeekBackoffSegments = 3;
  int64_t tailTarget = ComputeLiveEdgeTailTarget(m_liveTimeshiftStream.segments, m_liveTimeshiftStream.totalBytes,
                                                 kLiveEdgeSeekBackoffSegments);
  // Never behind the reader's own position on a forward seek
  // (dispatcharr::ClampSeekToTail()), nor behind what the rolling buffer has
  // already dropped (see ReadLiveTimeshiftStream()'s own clamp for why) -- the
  // two together, in that order, are dispatcharr::ResolveLiveSeekTarget()
  // (LiveEdgeMargin.h, tested).
  const LiveSeekResolution resolved =
      ResolveLiveSeekTarget(newPos, m_liveTimeshiftStream.position, tailTarget, FirstAvailableLiveByteOffset());
  newPos = resolved.position;
  const bool clampedToTail = resolved.clampedToTail;
  const bool clampedToHead = resolved.clampedToHead;

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: SeekLiveTimeshiftStream(position=%lld, whence=%d) from "
            "current=%lld, totalBytes=%lld -> newPos=%lld%s",
            static_cast<long long>(position), whence, static_cast<long long>(m_liveTimeshiftStream.position),
            static_cast<long long>(m_liveTimeshiftStream.totalBytes), static_cast<long long>(newPos),
            clampedToTail ? " (clamped to tail)"
                          : (clampedToHead ? " (clamped to the oldest segment still held)" : ""));

  m_liveTimeshiftStream.position = newPos;
  return newPos;
}

int64_t DispatcharrClient::GetLiveTimeshiftStreamLength()
{
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    if (!m_liveTimeshiftStream.open)
      return -1;
    // See LiveTimeshiftStreamState::fatal's own comment -- totalBytes is
    // whatever it was when the buffer died, which is still the right answer
    // once nothing more is coming; no need to keep re-querying for it.
    if (m_liveTimeshiftStream.fatal)
      return m_liveTimeshiftStream.totalBytes;
  }
  std::string refreshError;
  RefreshLiveManifest(/*force=*/false, refreshError); // throttled, cheap to call often
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  return m_liveTimeshiftStream.totalBytes;
}

int64_t DispatcharrClient::GetLiveTimeshiftStreamDurationMs()
{
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    if (!m_liveTimeshiftStream.open)
      return 0;
    // See LiveTimeshiftStreamState::fatal's own comment, and
    // GetLiveTimeshiftStreamLength()'s identical short-circuit just above --
    // GetStreamTimes() calls both back-to-back, so without this a
    // confirmed-dead buffer still paid for a second doomed throttled network
    // round trip on every single call.
    if (m_liveTimeshiftStream.fatal)
      return m_liveTimeshiftStream.totalDurationMs;
  }
  std::string refreshError;
  RefreshLiveManifest(/*force=*/false, refreshError);
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  return m_liveTimeshiftStream.totalDurationMs;
}

int64_t DispatcharrClient::FirstAvailableLiveByteOffset() const
{
  const auto& stream = m_liveTimeshiftStream;
  const size_t index = FirstAvailableLiveSegmentIndex(stream.segments, stream.oldestAvailableSequence);
  return index < stream.segments.size() ? stream.segments[index].byteOffset : 0;
}

int64_t DispatcharrClient::GetLiveTimeshiftStreamBeginMs()
{
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  if (!m_liveTimeshiftStream.open)
    return 0;
  const auto& stream = m_liveTimeshiftStream;
  const size_t index = FirstAvailableLiveSegmentIndex(stream.segments, stream.oldestAvailableSequence);
  return index < stream.segments.size() ? stream.segments[index].timeOffsetMs : 0;
}

time_t DispatcharrClient::GetLiveTimeshiftStreamWallClockAnchor()
{
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  if (!m_liveTimeshiftStream.open)
    return 0;
  return m_liveTimeshiftStream.wallClockAnchor;
}

void DispatcharrClient::CloseLiveTimeshiftStream()
{
  // Tells the plugin this viewer is done, via StopTimeshiftBuffer() --
  // NOT the unconditional "kill the buffer" it used to be. Two earlier
  // designs were both tried and confirmed live to be real bugs, not just
  // theoretical concerns (see docs/TIMESHIFT.md's "Concurrent viewers"
  // section for the full account of both):
  //
  // 1. Unconditionally stopping the buffer here (the original design):
  //    this Close() had no way to know whether *another* viewer was still
  //    actively reading the same buffer, so it could (and did) kill a
  //    second viewer's playback that had only just started.
  // 2. Not stopping anything at all here, relying purely on the plugin's
  //    own heartbeat-driven idle-timeout reaper to eventually notice no
  //    one's fetching anymore (2 minutes by default): safe with multiple
  //    viewers, but left a since-abandoned buffer occupying one of a
  //    provider's own concurrent-stream slots for up to that timeout --
  //    confirmed live as a real problem, not just a slow cleanup: with a
  //    provider's small concurrent-stream limit already fully used by
  //    in-progress recordings and a live channel watched then stopped
  //    and immediately switched to a
  //    *different* channel, the new channel failed to start at all
  //    (Dispatcharr still showed the old channel's now-abandoned buffer
  //    as the active last stream) until that timeout finally elapsed.
  //
  // The fix: the plugin now reference-counts viewers per buffer
  // (registered by this viewer's own viewer_id at Open()/
  // StartTimeshiftBuffer() time -- see LiveTimeshiftStreamState::viewerId's
  // own comment). This call deregisters just this viewer; the plugin only
  // actually stops the underlying ffmpeg process once no registered
  // viewers remain, so it's safe to call unconditionally on every Close()
  // regardless of how many other viewers exist, and fast when this really
  // was the last one -- no need to wait out the idle-timeout reaper, which
  // remains only as a backstop for a viewer that disappears without
  // cleanly closing (a crash, a network drop).
  //
  // Deliberately SYNCHRONOUS, not a detached background thread (an earlier
  // version of this fix used one, matching how this call worked before
  // reference counting existed) -- confirmed live this was itself a real
  // bug: switching channels (Kodi calls this Close() then OpenLiveStream()
  // for the new channel, back to back, without waiting on anything of this
  // addon's own) raced a detached call here against the new channel's own
  // Open()/StartTimeshiftBuffer(). With a provider's own concurrent-stream
  // limit already fully used (by in-progress recordings and the channel
  // being switched away from), the new channel's own upstream connection attempt reached
  // Dispatcharr *before* this detached call had actually freed the old
  // channel's slot -- confirmed live: the old channel's stream visibly
  // went down in Dispatcharr's own status a few seconds *after* the new
  // channel had already failed to start and Kodi had already given up and
  // returned to the main menu, not before. `_stop_ffmpeg()` in plugin.py
  // (the thing that actually happens once this call determines no viewers
  // remain) already blocks until the ffmpeg process is confirmed dead
  // (SIGTERM, poll, escalate to SIGKILL after 2s) before its own HTTP
  // response returns -- so calling it synchronously here, and letting
  // Kodi's own sequential Close()-then-Open() calling convention do the
  // rest, is what actually guarantees the old slot is free before the new
  // channel's own Open() ever asks the provider for one. Kodi's calling
  // thread already blocks synchronously on comparable network I/O
  // elsewhere in this same class (every live-timeshift read/seek/manifest
  // call), so this isn't a new category of blocking for it, just this one
  // call site catching up to that same pattern.
  // Copied out so the stop request below (a network call) runs with no lock held.
  bool wasOpen;
  std::string channelUuid, viewerId;
  {
    std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
    wasOpen = m_liveTimeshiftStream.open;
    channelUuid = m_liveTimeshiftStream.channelUuid;
    viewerId = m_liveTimeshiftStream.viewerId;
    // Forgotten from here on: the stop request below can take its whole bound, and a manifest refresh or heartbeat
    // issued meanwhile that still named this viewer would register it again (the plugin re-adds a viewer a heartbeat
    // names, since 0.6.5), leaving it on a buffer with other viewers until the reaper prunes it.
    m_liveTimeshiftStream.viewerId.clear();
  }
  if (wasOpen && !channelUuid.empty())
  {
    std::string stopError;
    StopTimeshiftBuffer(channelUuid, viewerId, stopError);
  }

  // The curl mutex first: a segment fetch still in flight finishes before its handle is freed.
  std::lock_guard<std::mutex> curlLock(m_liveCurlMutex);
  std::lock_guard<std::mutex> stateLock(m_liveStateMutex);
  if (m_liveTimeshiftStream.curl)
    curl_easy_cleanup(static_cast<CURL*>(m_liveTimeshiftStream.curl));
  m_liveTimeshiftStream = LiveTimeshiftStreamState();
  ++m_liveSession;
}

} // namespace dispatcharr
