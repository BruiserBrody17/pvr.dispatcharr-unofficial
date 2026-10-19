#pragma once

#include "DateTimeFormat.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <string>

namespace dispatcharr
{

// The two pure pieces of DispatcharrClient::CreateCatchupSession() around its
// POST /api/catchup/sessions/ call, pulled out so they are pinned by a test
// (../tests/test_catchup_session_request.cpp) rather than read off the call site.

// Dispatcharr's own cap on a session's `duration` (minutes) -- confirmed live:
// the serializer rejects a larger value, so it is clamped here rather than
// sent as-is.
constexpr int kMaxCatchupSessionDurationMinutes = 480;

// The request body: `channel_uuid`, the programme start as an ISO UTC string,
// and `duration` only when a positive one is known (Dispatcharr derives one
// itself otherwise), clamped to the cap above.
inline nlohmann::json BuildCatchupSessionBody(const std::string& channelUuid, time_t programmeStart,
                                              int durationMinutes)
{
  nlohmann::json body = {
      {"channel_uuid", channelUuid},
      {"start", IsoFromTime(programmeStart)},
  };
  if (durationMinutes > 0)
    body["duration"] = std::min(durationMinutes, kMaxCatchupSessionDurationMinutes);
  return body;
}

// The `duration` (minutes) for a programme running from `programmeStart` to `programmeEnd`: rounded up, so a programme
// that is not a whole number of minutes is not asked for up to 59 s short (the same rounding as
// ComputeRecordingExtendMinutes(), TimerIdentity.h). Dispatcharr adds five minutes of its own to the duration it is
// sent (`client_duration_to_window()`, read in its source 2026-09-26), which hides the shortfall today; this does not
// lean on that. 0, so that no duration is sent, when the end is not after the start; a span longer than the cap above
// is capped here, before it is narrowed to an int.
inline int CatchupSessionDurationMinutes(time_t programmeStart, time_t programmeEnd)
{
  if (programmeEnd <= programmeStart)
    return 0;
  const long long seconds = static_cast<long long>(programmeEnd) - static_cast<long long>(programmeStart);
  if (seconds > static_cast<long long>(kMaxCatchupSessionDurationMinutes) * 60)
    return kMaxCatchupSessionDurationMinutes;
  return static_cast<int>((seconds + 59) / 60);
}

// The `playback_url` the session response carries is a relative path
// ("/proxy/catchup/<uuid>?session_id=...", confirmed live), so it is joined to
// the configured server's own base URL; an absolute http(s) URL is used as-is.
inline std::string ResolveCatchupPlaybackUrl(const std::string& playbackUrl, const std::string& baseUrl)
{
  if (playbackUrl.rfind("http://", 0) == 0 || playbackUrl.rfind("https://", 0) == 0)
    return playbackUrl;
  return baseUrl + playbackUrl;
}

} // namespace dispatcharr
