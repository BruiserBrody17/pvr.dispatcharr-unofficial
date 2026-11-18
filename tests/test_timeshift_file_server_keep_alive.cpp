#include <catch2/catch_test_macros.hpp>

#include "../src/TimeshiftFileServerKeepAlive.h"

using namespace dispatcharr;
using Clock = std::chrono::steady_clock;

TEST_CASE("A timeshift keep-alive is due once its time comes, never for a dead buffer", "[timeshift_keep_alive]")
{
  const Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);

  SECTION("never scheduled counts as due")
  {
    CHECK(ShouldSendTimeshiftKeepAlive(false, Clock::time_point{}, now));
  }
  SECTION("due at and after the scheduled time, not before")
  {
    CHECK(ShouldSendTimeshiftKeepAlive(false, now, now));
    CHECK(ShouldSendTimeshiftKeepAlive(false, now - std::chrono::seconds(1), now));
    CHECK_FALSE(ShouldSendTimeshiftKeepAlive(false, now + std::chrono::milliseconds(1), now));
  }
  SECTION("a fatal buffer is never kept alive")
  {
    CHECK_FALSE(ShouldSendTimeshiftKeepAlive(true, Clock::time_point{}, now));
    CHECK_FALSE(ShouldSendTimeshiftKeepAlive(true, now - std::chrono::minutes(5), now));
  }
}

TEST_CASE("Only a 2xx from the file server counts as a refreshed buffer", "[timeshift_keep_alive]")
{
  CHECK(IsTimeshiftKeepAliveAnswered(true, 200));
  CHECK(IsTimeshiftKeepAliveAnswered(true, 204));
  // A replaced buffer's token, a missing playlist, a server error, a redirect: nothing was refreshed.
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(true, 403));
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(true, 404));
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(true, 500));
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(true, 302));
  // A transport failure, whatever status curl left behind.
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(false, 200));
  CHECK_FALSE(IsTimeshiftKeepAliveAnswered(false, 0));
}

TEST_CASE("The next timeshift keep-alive is an interval away, or the retry delay after a failure",
          "[timeshift_keep_alive]")
{
  const Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);
  CHECK(NextTimeshiftKeepAliveDueAt(now, true) == now + kTimeshiftKeepAliveInterval);
  CHECK(NextTimeshiftKeepAliveDueAt(now, false) == now + kTimeshiftKeepAliveRetryDelay);
  CHECK(kTimeshiftKeepAliveRetryDelay < kTimeshiftKeepAliveInterval);
  // A paused viewer whose attempts time out still gets three in before the plugin's default idle timeout runs out,
  // counting from the last thing that reached the file server (an interval before the first) and each attempt's own
  // bound (the retry delay runs from when an attempt ended).
  const Clock::time_point lastContact = now - kTimeshiftKeepAliveInterval;
  const Clock::time_point second = NextTimeshiftKeepAliveDueAt(now + kTimeshiftKeepAliveTimeout, false);
  const Clock::time_point third = NextTimeshiftKeepAliveDueAt(second + kTimeshiftKeepAliveTimeout, false);
  CHECK(third - lastContact <= kTimeshiftDefaultIdleTimeout);
}

TEST_CASE("A wait at the live tail asks the file server once the manifest goes unanswered", "[timeshift_keep_alive]")
{
  const Clock::time_point never{};
  const Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);

  SECTION("nothing known yet: ask")
  {
    CHECK(DecideTailWaitFileServerCheck(false, never, never, now) == TailWaitFileServerCheck::kProbe);
  }
  SECTION("an answer within the interval is enough, without asking again")
  {
    CHECK(DecideTailWaitFileServerCheck(false, now - std::chrono::seconds(3), now - std::chrono::seconds(3), now) ==
          TailWaitFileServerCheck::kKnownAlive);
    CHECK(DecideTailWaitFileServerCheck(false, now - kTimeshiftKeepAliveInterval, never, now) ==
          TailWaitFileServerCheck::kKnownAlive);
  }
  SECTION("an older answer is asked again")
  {
    CHECK(DecideTailWaitFileServerCheck(false, now - kTimeshiftKeepAliveInterval - std::chrono::milliseconds(1), never,
                                        now) == TailWaitFileServerCheck::kProbe);
  }
  SECTION("a recent failed attempt is not repeated on every quarter-second refresh")
  {
    CHECK(DecideTailWaitFileServerCheck(false, never, now - std::chrono::seconds(1), now) ==
          TailWaitFileServerCheck::kUnknown);
    CHECK(DecideTailWaitFileServerCheck(false, now - std::chrono::minutes(1), now - std::chrono::seconds(4), now) ==
          TailWaitFileServerCheck::kUnknown);
    CHECK(DecideTailWaitFileServerCheck(false, never, now - kTimeshiftKeepAliveRetryDelay, now) ==
          TailWaitFileServerCheck::kProbe);
  }
  SECTION("a dead buffer is never alive")
  {
    CHECK(DecideTailWaitFileServerCheck(true, now - std::chrono::seconds(1), never, now) ==
          TailWaitFileServerCheck::kUnknown);
  }
}

TEST_CASE("The manifest route is tried once a manifest named it, and again a retry delay after a failure",
          "[timeshift_keep_alive]")
{
  const Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);
  CHECK(ShouldTryManifestRoute(true, Clock::time_point{}, now));
  CHECK_FALSE(ShouldTryManifestRoute(false, Clock::time_point{}, now)); // an older plugin names none
  const Clock::time_point retryAt = now + kManifestRouteRetryDelay;
  CHECK_FALSE(ShouldTryManifestRoute(true, retryAt, now));
  CHECK_FALSE(ShouldTryManifestRoute(true, retryAt, retryAt - std::chrono::milliseconds(1)));
  CHECK(ShouldTryManifestRoute(true, retryAt, retryAt));
  // Longer than the tail wait's quarter-second refreshes, so a failing route costs one extra round trip per delay.
  CHECK(kManifestRouteRetryDelay >= std::chrono::seconds(1));
  // A failed attempt adds less than the short request timeout's 5 s floor to the action that follows it.
  CHECK(kManifestRouteTimeout < std::chrono::seconds(5));
}

TEST_CASE("The heartbeat action is skipped while the file server refreshes the viewer through the manifest route",
          "[timeshift_keep_alive]")
{
  const Clock::time_point now = Clock::time_point{} + std::chrono::hours(1);
  constexpr auto kInterval = std::chrono::seconds(10);
  // An older plugin (no route): reads may not refresh this viewer, so the action is always sent.
  CHECK(ShouldSendTimeshiftHeartbeatAction(false, now, now, kInterval));
  // The route known but the file server never answered (it has failed since the first manifest): sent.
  CHECK(ShouldSendTimeshiftHeartbeatAction(true, Clock::time_point{}, now, kInterval));
  // Answered within the interval: the route poll or keep-alive already refreshed the viewer.
  CHECK_FALSE(ShouldSendTimeshiftHeartbeatAction(true, now, now, kInterval));
  CHECK_FALSE(ShouldSendTimeshiftHeartbeatAction(true, now - kInterval + std::chrono::milliseconds(1), now, kInterval));
  // Not answered for a whole interval (the route failing, its retry delay running): sent again.
  CHECK(ShouldSendTimeshiftHeartbeatAction(true, now - kInterval, now, kInterval));
}
