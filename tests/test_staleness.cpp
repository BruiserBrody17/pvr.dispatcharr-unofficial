#include "Staleness.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

// ---------------------------------------------------------------------
// IsStaleSince
// ---------------------------------------------------------------------

TEST_CASE("IsStaleSince is true for a zero/never-refreshed timestamp", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverRefreshed; // epoch, time_since_epoch() == 0
  auto now = std::chrono::steady_clock::now();
  CHECK(IsStaleSince(neverRefreshed, now, std::chrono::hours(1)));
}

TEST_CASE("IsStaleSince is false within the interval", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::minutes(30);
  CHECK_FALSE(IsStaleSince(lastRefresh, now, std::chrono::hours(1)));
}

TEST_CASE("IsStaleSince is true once past the interval", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::hours(2);
  CHECK(IsStaleSince(lastRefresh, now, std::chrono::hours(1)));
}

TEST_CASE("IsStaleSince is false exactly at the interval boundary", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::hours(1);
  CHECK_FALSE(IsStaleSince(lastRefresh, now, std::chrono::hours(1))); // strictly > interval, not >=
}

// ---------------------------------------------------------------------
// IsRetryDue
// ---------------------------------------------------------------------

TEST_CASE("IsRetryDue is true for a zero/never-failed timestamp", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverFailed; // epoch, time_since_epoch() == 0
  auto now = std::chrono::steady_clock::now();
  CHECK(IsRetryDue(neverFailed, now, std::chrono::minutes(1)));
}

TEST_CASE("IsRetryDue is false right after a failure, within the retry interval", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastFailedAttempt = now - std::chrono::seconds(1);
  CHECK_FALSE(IsRetryDue(lastFailedAttempt, now, std::chrono::minutes(1)));
}

TEST_CASE("IsRetryDue is true once the retry interval has fully elapsed", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastFailedAttempt = now - std::chrono::minutes(2);
  CHECK(IsRetryDue(lastFailedAttempt, now, std::chrono::minutes(1)));
}

TEST_CASE("IsRetryDue is true exactly at the retry interval boundary", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastFailedAttempt = now - std::chrono::minutes(1);
  CHECK(IsRetryDue(lastFailedAttempt, now, std::chrono::minutes(1))); // >=, not strictly >
}

// ---------------------------------------------------------------------
// ShouldThrottleRefresh
// ---------------------------------------------------------------------

TEST_CASE("ShouldThrottleRefresh never throttles a forced call", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::milliseconds(1);
  CHECK_FALSE(ShouldThrottleRefresh(/*force=*/true, lastRefresh, now, std::chrono::milliseconds(500)));
}

TEST_CASE("ShouldThrottleRefresh never throttles a zero/never-refreshed timestamp", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverRefreshed;
  auto now = std::chrono::steady_clock::now();
  CHECK_FALSE(ShouldThrottleRefresh(/*force=*/false, neverRefreshed, now, std::chrono::milliseconds(500)));
}

TEST_CASE("ShouldThrottleRefresh throttles a call that lands well inside the interval", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::milliseconds(100);
  CHECK(ShouldThrottleRefresh(/*force=*/false, lastRefresh, now, std::chrono::milliseconds(500)));
}

TEST_CASE("ShouldThrottleRefresh does not throttle once the interval has fully elapsed", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::milliseconds(600);
  CHECK_FALSE(ShouldThrottleRefresh(/*force=*/false, lastRefresh, now, std::chrono::milliseconds(500)));
}

TEST_CASE("ShouldThrottleRefresh does not throttle exactly at the interval boundary", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastRefresh = now - std::chrono::milliseconds(500);
  CHECK_FALSE(ShouldThrottleRefresh(/*force=*/false, lastRefresh, now, std::chrono::milliseconds(500)));
}

// ---------------------------------------------------------------------
// ShouldFetchChannels
// ---------------------------------------------------------------------

namespace
{
constexpr auto kRefreshInterval = std::chrono::hours(12);
constexpr auto kRetryInterval = std::chrono::minutes(1);
} // namespace

TEST_CASE("ShouldFetchChannels is false when nothing is stale and no groups retry is pending", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1);
  std::chrono::steady_clock::time_point neverFailed;
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, neverFailed, neverFailed, now, kRefreshInterval, kRetryInterval));
}

TEST_CASE("ShouldFetchChannels is true when channels are stale and nothing has ever failed", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverLoaded;
  std::chrono::steady_clock::time_point neverFailed;
  auto now = std::chrono::steady_clock::now();
  CHECK(ShouldFetchChannels(neverLoaded, neverFailed, neverFailed, now, kRefreshInterval, kRetryInterval));
}

TEST_CASE("ShouldFetchChannels is true when only a groups retry is pending, channels otherwise fresh", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1); // fresh
  std::chrono::steady_clock::time_point neverFailedChannels;
  auto groupsLastFailedAt = now - std::chrono::minutes(2); // past the 1-minute retry interval
  CHECK(ShouldFetchChannels(channelsLoadedAt, neverFailedChannels, groupsLastFailedAt, now, kRefreshInterval,
                            kRetryInterval));
}

TEST_CASE("ShouldFetchChannels stays false for a groups-only retry when a groups-only retry interval hasn't "
          "elapsed yet",
          "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1); // fresh
  std::chrono::steady_clock::time_point neverFailedChannels;
  auto groupsLastFailedAt = now - std::chrono::seconds(1); // well within the 1-minute retry interval
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, neverFailedChannels, groupsLastFailedAt, now, kRefreshInterval,
                                  kRetryInterval));
}

TEST_CASE("ShouldFetchChannels applies the channels-failure backoff even when only a groups retry is pending -- "
          "the real bug this fixes",
          "[Staleness]")
{
  // Confirmed, real bug: an earlier version only applied the
  // channels-failure backoff when channels/EPG were themselves stale --
  // but the function body this gates always calls GetChannels() first
  // regardless of which of the two triggers fired, so a groups-only
  // retry left that channels call completely unthrottled. Once it failed,
  // channelsLastFailedAt was set but never consulted again on this path
  // (channelsStale stays false, since nothing writes channelsLoadedAt on
  // a failure) -- every later call kept re-hammering GetChannels()
  // through the groups-retry route instead of backing off.
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1);     // fresh -- channelsStale would be false
  auto channelsLastFailedAt = now - std::chrono::seconds(1); // failed moments ago, well within the retry interval
  auto groupsLastFailedAt = now - std::chrono::minutes(2);   // groups retry is due
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, channelsLastFailedAt, groupsLastFailedAt, now, kRefreshInterval,
                                  kRetryInterval));
}

TEST_CASE("ShouldFetchChannels proceeds once the channels-failure backoff itself has elapsed, groups retry still "
          "pending",
          "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1);     // fresh
  auto channelsLastFailedAt = now - std::chrono::minutes(2); // past the 1-minute retry interval
  auto groupsLastFailedAt = now - std::chrono::minutes(2);   // groups retry also due
  CHECK(ShouldFetchChannels(channelsLoadedAt, channelsLastFailedAt, groupsLastFailedAt, now, kRefreshInterval,
                            kRetryInterval));
}

TEST_CASE("ShouldFetchChannels applies the channels-failure backoff when channels/EPG are stale too", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::hours(24);      // stale
  auto channelsLastFailedAt = now - std::chrono::seconds(1); // failed moments ago
  std::chrono::steady_clock::time_point neverFailedGroups;
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, channelsLastFailedAt, neverFailedGroups, now, kRefreshInterval,
                                  kRetryInterval));
}

TEST_CASE("ShouldFetchChannels's forceStale bypasses freshness even when channels are otherwise fresh -- the real "
          "bug this fixes",
          "[Staleness]")
{
  // EnsureEpgLoaded() forces a channel refresh immediately before its
  // own XMLTV fetch so a server-side renumbering can't land ahead of a
  // stale channel list -- this only works if forceStale genuinely
  // bypasses the ordinary 12h freshness check.
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1); // fresh
  std::chrono::steady_clock::time_point neverFailed;
  CHECK(ShouldFetchChannels(channelsLoadedAt, neverFailed, neverFailed, now, kRefreshInterval, kRetryInterval,
                            /*forceStale=*/true));
}

TEST_CASE("ShouldFetchChannels's forceStale still respects the channels-failure backoff", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1);     // fresh
  auto channelsLastFailedAt = now - std::chrono::seconds(1); // failed moments ago, well within the retry interval
  std::chrono::steady_clock::time_point neverFailedGroups;
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, channelsLastFailedAt, neverFailedGroups, now, kRefreshInterval,
                                  kRetryInterval, /*forceStale=*/true));
}

TEST_CASE("ShouldFetchChannels defaults forceStale to false when omitted", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto channelsLoadedAt = now - std::chrono::minutes(1); // fresh
  std::chrono::steady_clock::time_point neverFailed;
  CHECK_FALSE(ShouldFetchChannels(channelsLoadedAt, neverFailed, neverFailed, now, kRefreshInterval, kRetryInterval));
}

// ---------------------------------------------------------------------
// HasNeverLoadedSuccessfully
// ---------------------------------------------------------------------

TEST_CASE("HasNeverLoadedSuccessfully is true for a zero/default-constructed time_point", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverLoaded;
  CHECK(HasNeverLoadedSuccessfully(neverLoaded));
}

TEST_CASE("HasNeverLoadedSuccessfully is false once a real load timestamp has been set", "[Staleness]")
{
  auto loadedAt = std::chrono::steady_clock::now();
  CHECK_FALSE(HasNeverLoadedSuccessfully(loadedAt));
}

TEST_CASE("HasNeverLoadedSuccessfully is false even for a long-stale but once-successful load -- distinct from "
          "IsStaleSince",
          "[Staleness]")
{
  // The whole point of this being a separate check from IsStaleSince():
  // a cache that's stale but has real data cached is safe to serve with
  // PVR_ERROR_NO_ERROR; only a cache that has NEVER loaded isn't.
  auto now = std::chrono::steady_clock::now();
  auto loadedAt = now - std::chrono::hours(24 * 365);
  CHECK(IsStaleSince(loadedAt, now, std::chrono::hours(12)));
  CHECK_FALSE(HasNeverLoadedSuccessfully(loadedAt));
}

// ---------------------------------------------------------------------
// ShouldUseShortChannelEpgRefreshWait
// ---------------------------------------------------------------------

TEST_CASE("ShouldUseShortChannelEpgRefreshWait is true when channels have never loaded", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverLoaded;
  auto epgLoadedAt = std::chrono::steady_clock::now();
  CHECK(ShouldUseShortChannelEpgRefreshWait(neverLoaded, epgLoadedAt));
}

TEST_CASE("ShouldUseShortChannelEpgRefreshWait is true when EPG has never loaded", "[Staleness]")
{
  auto channelsLoadedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point neverLoaded;
  CHECK(ShouldUseShortChannelEpgRefreshWait(channelsLoadedAt, neverLoaded));
}

TEST_CASE("ShouldUseShortChannelEpgRefreshWait is true when neither has ever loaded", "[Staleness]")
{
  std::chrono::steady_clock::time_point neverLoaded;
  CHECK(ShouldUseShortChannelEpgRefreshWait(neverLoaded, neverLoaded));
}

TEST_CASE("MarkStaleForWake makes a loaded cache stale without making it look never-loaded", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  auto interval = std::chrono::hours(12);
  auto justLoaded = now - std::chrono::seconds(5);

  auto aged = MarkStaleForWake(justLoaded, now, interval);
  CHECK(IsStaleSince(aged, now, interval));
  CHECK_FALSE(HasNeverLoadedSuccessfully(aged));
  // The stale mark is only just past the interval, not an arbitrary epoch.
  CHECK(now - aged == interval + std::chrono::seconds(1));
}

TEST_CASE("MarkStaleForWake leaves a never-loaded cache never-loaded", "[Staleness]")
{
  auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point neverLoaded;
  auto aged = MarkStaleForWake(neverLoaded, now, std::chrono::hours(4));
  CHECK(HasNeverLoadedSuccessfully(aged));
}

TEST_CASE("MarkStaleForWake still works when uptime is shorter than the interval", "[Staleness]")
{
  // steady_clock's epoch is boot time, so subtracting a long interval from a
  // short uptime gives a negative time_point -- still not the zero sentinel.
  std::chrono::steady_clock::time_point now(std::chrono::minutes(3));
  auto aged = MarkStaleForWake(now - std::chrono::seconds(1), now, std::chrono::hours(12));
  CHECK(IsStaleSince(aged, now, std::chrono::hours(12)));
  CHECK_FALSE(HasNeverLoadedSuccessfully(aged));
}

TEST_CASE("ShouldUseShortChannelEpgRefreshWait is false once both have loaded at least once, even if long stale",
          "[Staleness]")
{
  // Distinct from ordinary staleness, same reasoning as
  // HasNeverLoadedSuccessfully's own test above -- this is specifically
  // about the background thread's OWN sleep interval before its first
  // ever success, not a general "is it time to refresh" signal.
  auto now = std::chrono::steady_clock::now();
  auto longAgo = now - std::chrono::hours(24 * 365);
  CHECK_FALSE(ShouldUseShortChannelEpgRefreshWait(longAgo, longAgo));
}

// ---------------------------------------------------------------------
// ShouldCountTowardEpgFailureBackoff / ComputeEpgFailureRetryInterval
// ---------------------------------------------------------------------

TEST_CASE("Only a durable 4xx rejection counts toward the EPG failure backoff", "[Staleness]")
{
  CHECK(ShouldCountTowardEpgFailureBackoff(403)); // Network Access policy
  CHECK(ShouldCountTowardEpgFailureBackoff(400));
  CHECK(ShouldCountTowardEpgFailureBackoff(404));
  CHECK(ShouldCountTowardEpgFailureBackoff(429));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(0)); // transport failure
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(408));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(500));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(502));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(503));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(200));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(302));
}

TEST_CASE("An unusable guide response counts toward the EPG failure backoff whatever its status", "[Staleness]")
{
  // Past the size ceiling the transfer is aborted before any status is kept (0); out of memory and a
  // parse failure follow a 200. All three used to leave the count at 0.
  CHECK(ShouldCountTowardEpgFailureBackoff(0, /*responseUnusable=*/true));
  CHECK(ShouldCountTowardEpgFailureBackoff(200, /*responseUnusable=*/true));
  CHECK(ShouldCountTowardEpgFailureBackoff(503, /*responseUnusable=*/true));
  CHECK_FALSE(ShouldCountTowardEpgFailureBackoff(0, /*responseUnusable=*/false));
  CHECK(ShouldCountTowardEpgFailureBackoff(403, /*responseUnusable=*/false));
}

TEST_CASE("A guide that is always unusable is fetched at a doubling cadence, not every minute", "[Staleness]")
{
  // Replays the gate EnsureEpgLoaded() applies, minute by minute for an hour: the sweep measured 61
  // full downloads for an oversized guide before the count was kept.
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  const Clock::time_point never{};
  Clock::time_point lastFailedAt{};
  int durableFailures = 0;
  int attempts = 0;
  for (int minute = 0; minute <= 60; ++minute)
  {
    const auto now = start + std::chrono::minutes(minute);
    if (!ShouldAttemptGuideFetch(never, never, lastFailedAt, durableFailures, now, std::chrono::hours(4),
                                 std::chrono::minutes(1)))
      continue;
    ++attempts;
    lastFailedAt = now;
    durableFailures = ShouldCountTowardEpgFailureBackoff(0, /*responseUnusable=*/true) ? durableFailures + 1 : 0;
  }
  // Minutes 0, 1, 3, 7, 15, 31 (1, 2, 4, 8, 16 minute gaps), then the next is past the hour.
  CHECK(attempts == 6);

  // The same replay for a plain outage stays on the flat one-minute retry.
  lastFailedAt = {};
  durableFailures = 0;
  attempts = 0;
  for (int minute = 0; minute <= 60; ++minute)
  {
    const auto now = start + std::chrono::minutes(minute);
    if (!ShouldAttemptGuideFetch(never, never, lastFailedAt, durableFailures, now, std::chrono::hours(4),
                                 std::chrono::minutes(1)))
      continue;
    ++attempts;
    lastFailedAt = now;
    durableFailures = ShouldCountTowardEpgFailureBackoff(0, /*responseUnusable=*/false) ? durableFailures + 1 : 0;
  }
  CHECK(attempts == 61);
}

TEST_CASE("ComputeEpgFailureRetryInterval starts at the base and doubles to the cap", "[Staleness]")
{
  const auto base = std::chrono::minutes(1);
  CHECK(ComputeEpgFailureRetryInterval(0, base) == std::chrono::minutes(1));
  CHECK(ComputeEpgFailureRetryInterval(1, base) == std::chrono::minutes(1));
  CHECK(ComputeEpgFailureRetryInterval(2, base) == std::chrono::minutes(2));
  CHECK(ComputeEpgFailureRetryInterval(3, base) == std::chrono::minutes(4));
  CHECK(ComputeEpgFailureRetryInterval(4, base) == std::chrono::minutes(8));
  CHECK(ComputeEpgFailureRetryInterval(5, base) == std::chrono::minutes(16));
  CHECK(ComputeEpgFailureRetryInterval(6, base) == std::chrono::minutes(kMaxEpgFailureRetryMinutes));
  CHECK(ComputeEpgFailureRetryInterval(7, base) == std::chrono::minutes(kMaxEpgFailureRetryMinutes));
}

TEST_CASE("ComputeEpgFailureRetryInterval can't overflow on an absurd failure count", "[Staleness]")
{
  CHECK(ComputeEpgFailureRetryInterval(1000000, std::chrono::minutes(1)) ==
        std::chrono::minutes(kMaxEpgFailureRetryMinutes));
  CHECK(ComputeEpgFailureRetryInterval(-5, std::chrono::minutes(1)) == std::chrono::minutes(1));
}

TEST_CASE("ComputeEpgFailureRetryInterval never exceeds the cap even for a large base", "[Staleness]")
{
  CHECK(ComputeEpgFailureRetryInterval(1, std::chrono::minutes(45)) ==
        std::chrono::minutes(kMaxEpgFailureRetryMinutes));
}

// ---------------------------------------------------------------------
// IsFetchStillCurrent
// ---------------------------------------------------------------------

TEST_CASE("IsFetchStillCurrent holds only while no invalidation has landed since the fetch began", "[Staleness]")
{
  CHECK(IsFetchStillCurrent(0, 0));
  CHECK(IsFetchStillCurrent(41, 41));
  CHECK_FALSE(IsFetchStillCurrent(41, 42));
  CHECK_FALSE(IsFetchStillCurrent(0, 5));
}

TEST_CASE("IsFetchStillCurrent reports an invalidation even after several", "[Staleness]")
{
  uint64_t generation = 7;
  const uint64_t atStart = generation;
  generation += 3;
  CHECK_FALSE(IsFetchStillCurrent(atStart, generation));
}

// ---------------------------------------------------------------------
// IsGuideRefetchPending / IsGuideRefetchDue
// ---------------------------------------------------------------------

TEST_CASE("The post-renumbering guide refetch outlasts the server's 300 s guide cache", "[Staleness]")
{
  CHECK(kGuideRefetchAfterRenumber > std::chrono::seconds(300));
}

TEST_CASE("IsGuideRefetchPending is false only for the unset time_point", "[Staleness]")
{
  CHECK_FALSE(IsGuideRefetchPending({}));
  CHECK(IsGuideRefetchPending(std::chrono::steady_clock::time_point{} + std::chrono::seconds(1)));
}

TEST_CASE("IsGuideRefetchDue waits for the scheduled time and never fires when nothing is scheduled", "[Staleness]")
{
  const auto base = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
  const auto dueAt = base + kGuideRefetchAfterRenumber;
  CHECK_FALSE(IsGuideRefetchDue(dueAt, base));
  CHECK_FALSE(IsGuideRefetchDue(dueAt, dueAt - std::chrono::seconds(1)));
  CHECK(IsGuideRefetchDue(dueAt, dueAt));
  CHECK(IsGuideRefetchDue(dueAt, dueAt + std::chrono::hours(1)));
  CHECK_FALSE(IsGuideRefetchDue({}, base + std::chrono::hours(100)));
}

// ---------------------------------------------------------------------
// ShouldAttemptGuideFetch
// ---------------------------------------------------------------------

TEST_CASE("ShouldAttemptGuideFetch fetches a never-loaded or stale guide and leaves a fresh one alone", "[Staleness]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(100);
  const auto ttl = std::chrono::hours(4);
  const auto retry = std::chrono::minutes(1);
  CHECK(ShouldAttemptGuideFetch({}, {}, {}, 0, now, ttl, retry));
  CHECK(ShouldAttemptGuideFetch(now - ttl - std::chrono::seconds(1), {}, {}, 0, now, ttl, retry));
  CHECK_FALSE(ShouldAttemptGuideFetch(now - std::chrono::hours(1), {}, {}, 0, now, ttl, retry));
}

TEST_CASE("ShouldAttemptGuideFetch honours the failure backoff, including the grown durable one", "[Staleness]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(100);
  const auto ttl = std::chrono::hours(4);
  const auto retry = std::chrono::minutes(1);
  // Stale, but failed 30 s ago: too soon.
  CHECK_FALSE(ShouldAttemptGuideFetch({}, {}, now - std::chrono::seconds(30), 0, now, ttl, retry));
  CHECK(ShouldAttemptGuideFetch({}, {}, now - std::chrono::seconds(60), 0, now, ttl, retry));
  // Three durable rejections in a row: the interval has grown to 4 minutes.
  CHECK_FALSE(ShouldAttemptGuideFetch({}, {}, now - std::chrono::minutes(3), 3, now, ttl, retry));
  CHECK(ShouldAttemptGuideFetch({}, {}, now - std::chrono::minutes(4), 3, now, ttl, retry));
}

TEST_CASE("ShouldAttemptGuideFetch fetches a fresh guide again when the post-renumbering refetch is due", "[Staleness]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(100);
  const auto ttl = std::chrono::hours(4);
  const auto retry = std::chrono::minutes(1);
  const auto fresh = now - std::chrono::minutes(2);
  CHECK_FALSE(ShouldAttemptGuideFetch(fresh, now + std::chrono::minutes(3), {}, 0, now, ttl, retry));
  CHECK(ShouldAttemptGuideFetch(fresh, now, {}, 0, now, ttl, retry));
  CHECK(ShouldAttemptGuideFetch(fresh, now - std::chrono::seconds(1), {}, 0, now, ttl, retry));
}

// ---------------------------------------------------------------------
// The "never happened" sentinel is exactly a zero time_since_epoch(), and nothing near it. steady_clock counts from
// boot, so a Kodi started shortly after the machine booted has real timestamps a few seconds or minutes from zero --
// the sixteenth sweep's mutation of the sentinel to 1 survived because every test used a large `now`.
// ---------------------------------------------------------------------

TEST_CASE("a timestamp one tick after the clock's epoch is a real one, not the never-loaded sentinel", "[Staleness]")
{
  using Clock = std::chrono::steady_clock;
  const Clock::time_point justAfterZero{Clock::duration(1)};
  const Clock::time_point soonAfter{Clock::duration(std::chrono::seconds(30))};
  const Clock::time_point later{Clock::duration(std::chrono::minutes(2))};

  CHECK_FALSE(IsStaleSince(justAfterZero, soonAfter, std::chrono::hours(12)));
  CHECK_FALSE(IsRetryDue(justAfterZero, soonAfter, std::chrono::minutes(1)));
  CHECK(IsRetryDue(justAfterZero, later, std::chrono::minutes(1)));
  CHECK(ShouldThrottleRefresh(false, justAfterZero, Clock::time_point{Clock::duration(100)}, std::chrono::seconds(1)));
  // A real timestamp 30 s after boot, checked a minute later with a 12 h interval, is fresh (a booted-recently Kodi).
  const Clock::time_point loaded{Clock::duration(std::chrono::seconds(30))};
  CHECK_FALSE(IsStaleSince(loaded, later, std::chrono::hours(12)));
  // The sentinel itself is still all three.
  const Clock::time_point zero{};
  CHECK(IsStaleSince(zero, soonAfter, std::chrono::hours(12)));
  CHECK(IsRetryDue(zero, soonAfter, std::chrono::hours(12)));
  CHECK_FALSE(ShouldThrottleRefresh(false, zero, soonAfter, std::chrono::hours(12)));
}
