#pragma once

#include "Staleness.h"

#include <chrono>
#include <ctime>
#include <string>

namespace dispatcharr
{

// Whether a just-cached catch-up session's own playbackUrl can be reused
// for this request instead of creating (and immediately discarding) a
// second, redundant Dispatcharr catch-up session -- see docs/OPEN_ITEMS.md's
// "Every catch-up play creates two Dispatcharr sessions" entry for
// the real, live-confirmed cost this exists to avoid: a single catch-up
// play calls DispatcharrClient::CreateCatchupSession() twice in a row with
// identical (channelUuid, programmeStart, durationMinutes) -- Kodi's own
// CPVRGUIActionsPlayback::PlayEpgTag() (PVRGUIActionsPlayback.cpp) calls
// GetEpgTagStreamProperties() once just to check EPGPlaybackAsLive(), then
// calls StartPlayback(), which calls it again (PVRPlaybackState.cpp) --
// confirmed live, 2026-09-29: the second, wasted POST accounts for roughly
// 30% of the total delay before catch-up playback actually starts.
//
// Requires an EXACT match on all three key fields, not just the channel --
// a request landing within the cache window for a genuinely different
// programme or duration (e.g. the user backed out and picked a different
// EPG entry) must never reuse a stale URL. `maxAge` is deliberately well
// under Dispatcharr's own 60-second catch-up-session handshake expiry
// (HANDSHAKE_TTL_SECONDS, apps/timeshift/sessions.py) so a reused URL is
// never handed out for a session that may have already expired
// server-side by the time it's actually used.
//
// Reuses IsStaleSince() (Staleness.h) for the age check rather than
// duplicating its own zero-time_point "never cached yet" sentinel
// convention -- a default-constructed `cachedAt` already reads as
// infinitely stale there, so an empty cache correctly never matches
// without this function needing its own separate "is there even anything
// cached" check first.
inline bool ShouldReuseCachedCatchupSession(const std::string& cachedChannelUuid, time_t cachedProgrammeStart,
                                            int cachedDurationMinutes, std::chrono::steady_clock::time_point cachedAt,
                                            const std::string& requestedChannelUuid, time_t requestedProgrammeStart,
                                            int requestedDurationMinutes, std::chrono::steady_clock::time_point now,
                                            std::chrono::steady_clock::duration maxAge)
{
  if (cachedChannelUuid.empty() || cachedChannelUuid != requestedChannelUuid)
    return false;
  if (cachedProgrammeStart != requestedProgrammeStart)
    return false;
  if (cachedDurationMinutes != requestedDurationMinutes)
    return false;
  return !IsStaleSince(cachedAt, now, maxAge);
}

} // namespace dispatcharr
