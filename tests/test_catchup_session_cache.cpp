#include "CatchupSessionCache.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr auto kMaxAge = std::chrono::seconds(30);
} // namespace

TEST_CASE("ShouldReuseCachedCatchupSession is false when nothing is cached yet", "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point neverCached; // default-constructed, zero
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  CHECK_FALSE(ShouldReuseCachedCatchupSession("", 1000, 30, neverCached, "uuid-a", 1000, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is true for an exact, fresh match", "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + std::chrono::seconds(1);
  CHECK(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is false for a different channel", "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + std::chrono::seconds(1);
  CHECK_FALSE(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-b", 1000, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is false for a different programme start", "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + std::chrono::seconds(1);
  CHECK_FALSE(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1001, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is false for a different duration", "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + std::chrono::seconds(1);
  CHECK_FALSE(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 31, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is false once maxAge has elapsed -- the real bug this fixes stays "
          "bounded, not a permanent reuse",
          "[CatchupSessionCache]")
{
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + kMaxAge + std::chrono::seconds(1);
  CHECK_FALSE(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is still true right at the maxAge boundary", "[CatchupSessionCache]")
{
  // IsStaleSince() itself is a strict `>` -- landing exactly on the
  // boundary is still fresh, matching its own established convention
  // (see Staleness.h's own comment on ShouldThrottleRefresh() for why
  // this project keeps that boundary literal rather than re-deriving it).
  std::chrono::steady_clock::time_point cachedAt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point now = cachedAt + kMaxAge;
  CHECK(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 30, now, kMaxAge));
}

TEST_CASE("ShouldReuseCachedCatchupSession is false when both channel ids are empty", "[CatchupSessionCache]")
{
  // Two empty uuids are equal strings, but an empty cached id means "nothing cached".
  const auto now = std::chrono::steady_clock::now();
  CHECK_FALSE(ShouldReuseCachedCatchupSession("", 1000, 30, now, "", 1000, 30, now, kMaxAge));
}

TEST_CASE("A session cached 12 ms earlier is reused (Kodi 21's second call for the same Play)", "[CatchupSessionCache]")
{
  const auto cachedAt = std::chrono::steady_clock::now();
  CHECK(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 30,
                                        cachedAt + std::chrono::milliseconds(12), kCatchupSessionCacheMaxAge));
}

TEST_CASE("A replay of the same programme 6.5 s after the POST gets a new session", "[CatchupSessionCache]")
{
  // Dispatcharr can delete a session 6.5 s after its stream closes (kCatchupSessionCacheMaxAge's comment), and the
  // stream closes after the POST at the earliest; the old 30 s age handed the deleted URL to a replay in that window.
  const auto cachedAt = std::chrono::steady_clock::now();
  CHECK_FALSE(ShouldReuseCachedCatchupSession("uuid-a", 1000, 30, cachedAt, "uuid-a", 1000, 30,
                                              cachedAt + std::chrono::milliseconds(6500), kCatchupSessionCacheMaxAge));
}

TEST_CASE("kCatchupSessionCacheMaxAge stays under Dispatcharr's earliest session deletion", "[CatchupSessionCache]")
{
  // _STATS_RECONNECT_SETTLE_SECONDS + _STATS_DISCONNECT_GRACE_SECONDS in Dispatcharr v0.33.0, counted from the
  // stream's close, which is always after the POST that cached the session.
  constexpr auto kEarliestDeletion = std::chrono::milliseconds(1500) + std::chrono::seconds(5);
  STATIC_CHECK(kCatchupSessionCacheMaxAge < kEarliestDeletion);
}
