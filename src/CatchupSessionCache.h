#pragma once

#include "Staleness.h"

#include <chrono>
#include <ctime>
#include <string>

namespace dispatcharr
{

// Dispatcharr deletes a catch-up session once viewing has ended, well before its 60 s handshake expiry
// (HANDSHAKE_TTL_SECONDS, apps/timeshift/sessions.py): in v0.33.0's apps/timeshift/views.py, a playback connection that
// was not a startup probe (it yielded at least _STATS_GRACE_MIN_YIELDED_BYTES = 64 KiB or ran at least
// _STATS_GRACE_MIN_ELAPSED_SECONDS = 2 s) arms a deadline _STATS_RECONNECT_SETTLE_SECONDS (1.5 s) +
// _STATS_DISCONNECT_GRACE_SECONDS (5 s) after it closes, and _run_stats_disconnect_grace() then calls
// _finalize_playback_session_auth(), after which the session's URL answers 401 "Invalid or expired playback session".
// A stream can only close after the POST that cached its session, so a session is good for at least 6.5 s after it was
// cached; the age stays under that. Was 30 s, which handed a deleted session to a replay of the same programme between
// roughly 7 and 30 s after the first Play (docs/CLOSED_ITEMS.md). A cached session is deliberately not cleared after
// one use: a replay inside the age reusing it is safe, and a new session there would leave the old one's client
// counted against a per-user stream limit until its grace ends.
constexpr std::chrono::seconds kCatchupSessionCacheMaxAge{5};

// Whether a just-cached catch-up session's own playbackUrl can be reused
// for this request instead of creating (and immediately discarding) a
// second, redundant Dispatcharr catch-up session -- see docs/CLOSED_ITEMS.md's
// "Every catch-up play creates two Dispatcharr sessions" entry for
// the real, live-confirmed cost this exists to avoid. On Kodi 21 a single
// catch-up play calls DispatcharrClient::CreateCatchupSession() twice in a row
// with identical (channelUuid, programmeStart, durationMinutes) --
// CPVRGUIActionsPlayback::PlayEpgTag() (PVRGUIActionsPlayback.cpp) calls
// GetEpgTagStreamProperties() once just to check EPGPlaybackAsLive(), then
// StartPlayback() calls it again (PVRPlaybackState.cpp), about 12 ms apart --
// confirmed live, 2026-09-29: the second, wasted POST accounts for roughly
// 30% of the total delay before catch-up playback actually starts. Kodi 22's
// PlayEpgTag() no longer asks (22.0 RC1: the only call is
// CPVRPlaybackState::ResolveEPG()), so there it is one call per Play and a
// hit is only a replay of the same programme inside the age.
//
// Requires an EXACT match on all three key fields, not just the channel --
// a request landing within the cache window for a genuinely different
// programme or duration (e.g. the user backed out and picked a different
// EPG entry) must never reuse a stale URL. `maxAge` is bounded by
// kCatchupSessionCacheMaxAge above, not by the 60 s handshake expiry.
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

struct CatchupSessionCacheEntry
{
  std::string channelUuid;
  time_t programmeStart = 0;
  int durationMinutes = 0;
  std::string playbackUrl;
  std::chrono::steady_clock::time_point cachedAt; // default-constructed == "never cached"
};

} // namespace dispatcharr
