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
