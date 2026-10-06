#pragma once

#include <chrono>
#include <cstdint>

namespace dispatcharr
{

// The "has this cache gone stale" check, duplicated identically across
// PVRDispatcharr::EnsureChannelsLoaded()/EnsureEpgLoaded()/
// EnsureRecordingsLoaded()/EnsureTimerRulesLoaded(): a zero/default-
// constructed `lastRefresh` (time_since_epoch() == 0, i.e. never
// assigned since this addon started) always counts as stale -- there's
// nothing cached yet to be fresh. Otherwise stale once more than
// `interval` has elapsed since the last successful load. Pulled out
// specifically so this shared sentinel/comparison is unit-testable
// standalone; see ../tests/test_staleness.cpp.
inline bool IsStaleSince(std::chrono::steady_clock::time_point lastRefresh, std::chrono::steady_clock::time_point now,
                         std::chrono::steady_clock::duration interval)
{
  if (lastRefresh.time_since_epoch().count() == 0)
    return true;
  return now - lastRefresh > interval;
}

// A failure-backoff check for PVRDispatcharr::EnsureChannelsLoaded()/
// EnsureEpgLoaded(): true when it's been at least `retryInterval` since
// the last FAILED fetch attempt (or there's never been one -- a
// zero/default `lastFailedAttempt`, same "never happened yet" sentinel
// convention as IsStaleSince()'s own `lastRefresh`). Exists specifically
// because a fetch failure alone doesn't update the success timestamp
// IsStaleSince() checks, so without this a failure would otherwise be
// retried on every single call in a tight loop (GetEPGForChannel() calls
// both Ensure*Loaded() functions once per channel while Kodi populates
// its EPG grid) rather than once, backed off, and left alone briefly.
// Pulled out specifically so it's unit-testable standalone; see
// ../tests/test_staleness.cpp.
inline bool IsRetryDue(std::chrono::steady_clock::time_point lastFailedAttempt,
                       std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration retryInterval)
{
  if (lastFailedAttempt.time_since_epoch().count() == 0)
    return true;
  return now - lastFailedAttempt >= retryInterval;
}

// The inverse-direction "too soon to bother" throttle check, duplicated
// identically across DispatcharrClient::RefreshInProgressRecordingManifest()/
// RefreshLiveManifest(): true when a non-forced call landed too soon
// after the last real refresh to be worth repeating (a tight catch-up-
// loop/demux-read cycle can call these far more often than the
// underlying stream could possibly have grown). `force` always bypasses
// the throttle; a zero/default `lastRefresh` (never refreshed yet) never
// throttles either, matching IsStaleSince()'s own "always stale" case
// above. Deliberately a strict `<` rather than IsStaleSince()'s own
// negation (`<=`), so the two callers still refresh right at the exact
// interval boundary instead of throttling it away -- in practice never
// observably different (steady_clock's resolution makes landing exactly
// on the boundary vanishingly unlikely), but kept literal rather than
// derived, so behavior is unchanged from before this extraction.
inline bool ShouldThrottleRefresh(bool force, std::chrono::steady_clock::time_point lastRefresh,
                                  std::chrono::steady_clock::time_point now,
                                  std::chrono::steady_clock::duration interval)
{
  if (force)
    return false;
  if (lastRefresh.time_since_epoch().count() == 0)
    return false;
  return now - lastRefresh < interval;
}

// The full gating decision for PVRDispatcharr::EnsureChannelsLoaded(),
// which has two independent reasons to (re)fetch -- channels/EPG gone
// stale, or a channel-groups-only failure whose own retry interval has
// elapsed (see m_groupsLastFailedAt's own comment, PVRDispatcharr.h: a
// groups failure doesn't fail the whole function, so it needs its own
// retry schedule rather than waiting out the full channels refresh
// interval) -- but only ONE underlying fetch (GetChannels(), always
// called first regardless of which reason triggered it). Fixes a real,
// confirmed bug found via a project-wide review: an earlier version
// gated the channels-failure backoff (IsRetryDue(channelsLastFailedAt))
// only when channelsStale was true, on the theory that a groups-only
// retry should always be allowed through even when "too soon" for
// channels. That reasoning didn't account for the actual mechanics
// below it -- a groups-retry-triggered call still hits GetChannels()
// first, completely unthrottled -- so once that channels call itself
// failed, channelsLastFailedAt was set but never consulted again on
// this path (channelsStale stays false, since nothing wrote
// channelsLoadedAt on a failure), reopening the exact per-channel-
// hammering bug this whole mechanism exists to prevent, just reached via
// the groups-retry path instead of the original one. Applying the
// channels-failure backoff unconditionally (whenever a fetch is about to
// happen for either reason) fixes this without weakening the groups-only
// retry's own schedule: IsRetryDue() already treats a zero/never-failed
// channelsLastFailedAt as "go ahead" (matching IsStaleSince()'s own
// always-stale convention for a zero timestamp), so this only changes
// behavior when channels have actually failed recently. Pulled out
// specifically so this decision is unit-testable standalone; see
// ../tests/test_staleness.cpp.
//
// `forceStale` (added 2026-09-26, a 26th-pass audit, fixing a real,
// confirmed bug found via a project-wide review, confirmed against
// Dispatcharr's own real current upstream source, not itself
// independently reproduced) bypasses the `channelsStale` check only --
// the channels-failure backoff below still applies regardless, so a
// forced call can't hammer the server if channels are genuinely failing.
// Needed because this addon's own channel and EPG refreshes run on
// completely independent timers (channel_refresh_hours, default 12h, vs.
// epg_refresh_hours, default 4h): a server-side channel renumbering
// (Dispatcharr's own "compact numbering" scheme) is far more likely to
// have its very next XMLTV refresh land *before* its next channel
// refresh at these defaults, so `EnsureEpgLoaded()` forces a channel
// refresh immediately before its own XMLTV fetch (see its own comment)
// rather than trusting whichever cycle happens to win the race --
// `HaveChannelNumbersChanged()`'s own detection (`ChannelRenumbering.h`)
// only helps once the *channel* side notices a renumbering first, which
// this closes the gap on for the opposite ordering.
inline bool ShouldFetchChannels(std::chrono::steady_clock::time_point channelsLoadedAt,
                                std::chrono::steady_clock::time_point channelsLastFailedAt,
                                std::chrono::steady_clock::time_point groupsLastFailedAt,
                                std::chrono::steady_clock::time_point now,
                                std::chrono::steady_clock::duration channelsRefreshInterval,
                                std::chrono::steady_clock::duration failureRetryInterval, bool forceStale = false)
{
  bool channelsStale = forceStale || IsStaleSince(channelsLoadedAt, now, channelsRefreshInterval);
  bool groupsRetryPending =
      groupsLastFailedAt.time_since_epoch().count() != 0 && IsRetryDue(groupsLastFailedAt, now, failureRetryInterval);
  if (!channelsStale && !groupsRetryPending)
    return false;
  return IsRetryDue(channelsLastFailedAt, now, failureRetryInterval);
}

// The loaded-at timestamp a cache should carry after the device has slept, so
// its very next Ensure*Loaded() sees it as stale and refetches (added
// 2026-09-30, see PVRDispatcharr::OnSystemWake()). Every staleness check here
// runs on std::chrono::steady_clock, which on Linux is CLOCK_MONOTONIC and
// does not advance while the system is suspended, so a device asleep for days
// would otherwise wake still believing a pre-sleep channel list or guide was
// minutes old. Deliberately NOT the zero "never loaded" sentinel -- that would
// trip HasNeverLoadedSuccessfully() and make GetChannels() and friends report
// a failure, which Kodi reads as permission to delete data, for a cache that
// does hold real data, only old. A cache that has never loaded stays exactly
// that. Stale by one second more than `interval` rather than by exactly it:
// IsStaleSince() compares with a strict `>`.
inline std::chrono::steady_clock::time_point MarkStaleForWake(std::chrono::steady_clock::time_point loadedAt,
                                                              std::chrono::steady_clock::time_point now,
                                                              std::chrono::steady_clock::duration interval)
{
  if (loadedAt.time_since_epoch().count() == 0)
    return loadedAt;
  return now - interval - std::chrono::seconds(1);
}

// Whether a cache has never successfully loaded even once since this addon
// instance started -- the same zero/default-constructed time_point sentinel
// IsStaleSince() etc. already treat as "always stale", but broken out
// separately here because PVRDispatcharr::GetChannels()/GetChannelsAmount()/
// GetChannelGroups()/GetChannelGroupsAmount()/GetChannelGroupMembers() need
// to distinguish it from ordinary staleness, not just fold it into the same
// refetch decision: a merely-stale cache still holds real, previously-
// fetched data that's safe to hand back with PVR_ERROR_NO_ERROR, but a cache
// that has NEVER loaded holds nothing at all, and reporting PVR_ERROR_NO_ERROR
// with an empty result in that case is a real, confirmed bug found via a
// project-wide review (a 40th-pass audit), confirmed against Kodi's own
// real current SDK source: CPVRChannelGroup::HasValidDataForClient()
// (PVRChannelGroup.cpp) treats this client's data as valid (safe to delete
// any channel/group-member missing from the just-returned list) unless the
// client is in CPVRClients::ForClients()'s own failedClients list
// (PVRClients.cpp) -- which only ever gets a client added to it from a
// return value other than PVR_ERROR_NO_ERROR/PVR_ERROR_NOT_IMPLEMENTED.
// These five callbacks always returned PVR_ERROR_NO_ERROR regardless of
// whether the underlying Dispatcharr fetch had ever actually succeeded, so
// if it never had (Dispatcharr unreachable from the very first call this
// addon instance ever made), Kodi read the resulting empty channel/group
// list as authoritative and deleted every channel, channel-group
// membership, and associated EPG entry from its own database -- not merely
// hidden until the next successful refresh, but gone.
// Despite the name, also true any time a caller has explicitly reset
// loadedAt back to this same zero sentinel after a genuine prior success
// -- not only "truly never loaded even once" (noted 2026-09-27, a
// 51st-pass audit, clarifying real, confirmed but previously
// undocumented behavior found via a project-wide review, not itself
// independently reproduced). InvalidateAndTriggerTimerUpdate()/
// InvalidateAndTriggerRecordingUpdate() (PVRDispatcharr.cpp) do exactly
// this to m_recordingsCachedAt/m_seriesRulesCachedAt/m_recurringRulesCachedAt,
// as does the channel-renumbering branch to m_epgLoadedAt -- so a
// mid-session invalidation immediately followed by a failed refetch (an
// outage hitting right after an edit, or right after a renumbering is
// detected) makes this true again for a cache that has, in fact, loaded
// successfully before. The callers gated on this (GetTimersAmount()/
// GetTimers()/GetEPGForChannel()) returning PVR_ERROR_SERVER_ERROR in
// that window is still safe, not a regression: confirmed against Kodi's
// own real current SDK source that CPVRTimers::UpdateEntries()
// (PVRTimers.cpp) simply keeps this client's existing timers unchanged
// when it's in the failed-clients list, the same outcome the old,
// merely-stale-cache behavior already had.
inline bool HasNeverLoadedSuccessfully(std::chrono::steady_clock::time_point loadedAt)
{
  return loadedAt.time_since_epoch().count() == 0;
}

// Whether PVRDispatcharr::StartChannelEpgRefreshThread()'s own background
// loop should sleep for the short failure-retry interval instead of its
// normal, much longer steady-state one before its next wake -- added
// 2026-09-27, a 43rd-pass audit, fixing a real, confirmed gap found via a
// project-wide review, not itself independently reproduced: that loop's
// own wait_for() used a single fixed interval (kChannelEpgRefreshCheckMinutes,
// 10 minutes) regardless of whether the channels/EPG fetch it had just
// attempted actually succeeded, so a first-ever load failure right at
// addon startup (Dispatcharr not yet reachable -- a slower-booting
// server than the Kodi device itself, or a VPN/network path that comes
// up after Kodi does) left the *next* retry attempt a full 10 minutes
// away, even though dispatcharr::ShouldFetchChannels()'s own 1-minute
// failure-retry gate (kChannelEpgFailureRetryMinutes) would otherwise
// have allowed retrying much sooner. True whenever channels or EPG have
// never successfully loaded even once -- deliberately not "whenever the
// last attempt merely failed", since a channels/EPG pair that has loaded
// at least once already gets its own much shorter, independent failure
// backoff via IsRetryDue() every time EnsureChannelsLoaded()/
// EnsureEpgLoaded() themselves run, on whatever cadence calls them
// (including Kodi's own GetEPGForChannel() sweep); this is specifically
// about how long this *background thread itself* sleeps between its own
// attempts before that has ever succeeded even once.
inline bool ShouldUseShortChannelEpgRefreshWait(std::chrono::steady_clock::time_point channelsLoadedAt,
                                                std::chrono::steady_clock::time_point epgLoadedAt)
{
  return HasNeverLoadedSuccessfully(channelsLoadedAt) || HasNeverLoadedSuccessfully(epgLoadedAt);
}

// How long after a detected channel renumbering the guide is fetched once more.
// Dispatcharr keeps its generated XMLTV in a 300-second chunk cache that a manual
// channel-number edit does not invalidate (confirmed live: three fetches over ~70 s
// after a renumber still listed the old number), so a guide fetch that lands inside
// that window comes back keyed by the OLD numbers while this addon already holds the
// new channel list. The detection time is the best available stand-in for the edit
// time, so a fetch made at least this long afterwards is past the cache. 330 s is
// the 300 s TTL plus a margin for the clock difference between the two sides.
constexpr std::chrono::seconds kGuideRefetchAfterRenumber{330};

// Whether the follow-up guide fetch scheduled by a renumbering is waiting to run.
// `dueAt` is the zero time_point when none is scheduled.
inline bool IsGuideRefetchPending(std::chrono::steady_clock::time_point dueAt)
{
  return dueAt != std::chrono::steady_clock::time_point{};
}

// Whether that follow-up fetch may run now. Never true before `dueAt`: an earlier
// fetch would just be served the same stale cache again and burn a full guide download.
inline bool IsGuideRefetchDue(std::chrono::steady_clock::time_point dueAt, std::chrono::steady_clock::time_point now)
{
  return IsGuideRefetchPending(dueAt) && now >= dueAt;
}

// Whether a failed /output/epg fetch counts toward the escalating EPG retry
// backoff below (ComputeEpgFailureRetryInterval()) rather than the flat
// kChannelEpgFailureRetryMinutes -- the same durable-vs-transient split
// ShouldCountTowardLoginBackoff() (AuthBackoff.h) makes for Login(). A 4xx
// is a rejection retrying sooner won't fix: chiefly the 403 of Dispatcharr's
// own Network Access policy, and `epg_endpoint()` writes an `epg_blocked`
// system event (firing any configured integration and trimming the capped
// event history) on every single one, so a flat 1-minute retry was enough to
// push every other event out of that history within about a day
// (docs/OPEN_ITEMS.md, the "no backoff on a persistent /output/epg failure"
// entry). A transport failure (httpCode 0), a 5xx or a 408 is the transient
// case -- Dispatcharr unreachable or restarting -- and stays on the flat
// interval so a recovered server gets its guide back within a minute.
//
// `responseUnusable` is the third durable case (found by the 2026-10-04 second hardening
// sweep, proven by replaying the real gate for an hour: 61 full guide downloads): the server
// answered, but what it sent cannot be used -- it went past the size ceiling (no HTTP status
// is kept for that, so the status alone read as a transport failure and reset the count), ran
// the process out of memory, or did not parse. Retrying a minute later gets the same bytes, so
// it escalates like a rejection; on a 32-bit box that is a quarter-gigabyte download and a
// parse attempt every minute, forever, for a guide that never loads.
inline bool ShouldCountTowardEpgFailureBackoff(long httpCode, bool responseUnusable = false)
{
  return responseUnusable || (httpCode >= 400 && httpCode < 500 && httpCode != 408);
}

// How long EnsureEpgLoaded() waits before retrying after
// `consecutiveDurableFailures` failures in a row that
// ShouldCountTowardEpgFailureBackoff() accepted: `baseInterval` for the
// first (and for zero, no durable failure yet), doubling each time after
// that, capped at kMaxEpgFailureRetryMinutes. The cap is deliberately short
// enough that someone who fixes the policy on the server (widening Network
// Access) sees a guide within half an hour without restarting Kodi, while
// cutting a steady 1-per-minute retry to 1 per 30 minutes (48/day).
constexpr int kMaxEpgFailureRetryMinutes = 30;

inline std::chrono::steady_clock::duration
ComputeEpgFailureRetryInterval(int consecutiveDurableFailures, std::chrono::steady_clock::duration baseInterval)
{
  const std::chrono::steady_clock::duration maxInterval = std::chrono::minutes(kMaxEpgFailureRetryMinutes);
  auto interval = baseInterval;
  // Bounded loop rather than a shift: no overflow for any input, and it
  // stops as soon as the cap is reached.
  for (int i = 1; i < consecutiveDurableFailures && interval < maxInterval; ++i)
    interval *= 2;
  return interval < maxInterval ? interval : maxInterval;
}

// Whether a guide fetch would be attempted right now: the guide is stale (or a
// post-renumbering refetch is due) and its failure backoff has run out. Shared by
// EnsureEpgLoaded() (which fetches) and GetEPGForChannel() (which only wakes the
// background thread that fetches, see RequestGuideFetchIfWanted()), so a Kodi
// callback never wakes it for a fetch it would refuse anyway.
inline bool ShouldAttemptGuideFetch(std::chrono::steady_clock::time_point epgLoadedAt,
                                    std::chrono::steady_clock::time_point refetchDueAt,
                                    std::chrono::steady_clock::time_point lastFailedAt, int consecutiveDurableFailures,
                                    std::chrono::steady_clock::time_point now,
                                    std::chrono::steady_clock::duration refreshInterval,
                                    std::chrono::steady_clock::duration failureRetryBaseInterval)
{
  if (!IsStaleSince(epgLoadedAt, now, refreshInterval) && !IsGuideRefetchDue(refetchDueAt, now))
    return false;
  return IsRetryDue(lastFailedAt, now,
                    ComputeEpgFailureRetryInterval(consecutiveDurableFailures, failureRetryBaseInterval));
}

// Whether a fetch that started when a cache's generation counter read `generationAtStart`
// may still be committed as fresh at `generationNow`. The counter is bumped whenever the
// cache is deliberately invalidated (an edit, a delete, a detected channel renumbering); a
// fetch that was already running when that happened holds pre-change data, and committing
// it as fresh would silently undo the invalidation for a whole refresh interval
// (docs/OPEN_ITEMS.md, "Cache-invalidation triggers can be overwritten by an in-flight fetch").
inline bool IsFetchStillCurrent(uint64_t generationAtStart, uint64_t generationNow)
{
  return generationAtStart == generationNow;
}

} // namespace dispatcharr
