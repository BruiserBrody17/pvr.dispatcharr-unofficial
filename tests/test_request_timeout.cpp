#include "RequestTimeout.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ShortRequestTimeoutMs is five seconds at the default connection timeout", "[RequestTimeout]")
{
  CHECK(ShortRequestTimeoutMs(30) == 5000);
}

TEST_CASE("ShortRequestTimeoutMs follows the configured timeout in proportion", "[RequestTimeout]")
{
  // A sixth of it, so a deployment that raised the connection timeout for a slow server gets a larger bound too.
  CHECK(ShortRequestTimeoutMs(60) == 10000);
  CHECK(ShortRequestTimeoutMs(90) == 15000);
  CHECK(ShortRequestTimeoutMs(120) == 20000);
  CHECK(ShortRequestTimeoutMs(150) == 25000);
}

TEST_CASE("ShortRequestTimeoutMs stops at thirty seconds", "[RequestTimeout]")
{
  CHECK(ShortRequestTimeoutMs(180) == 30000); // the setting's own maximum
  CHECK(ShortRequestTimeoutMs(181) == 30000);
  CHECK(ShortRequestTimeoutMs(100000) == 30000);
}

TEST_CASE("ShortRequestTimeoutMs never goes under five seconds for a sane timeout", "[RequestTimeout]")
{
  // The boundary below which the sixth would be shorter than the floor: 30 s gives exactly 5 s, 29 s is floored to it.
  CHECK(ShortRequestTimeoutMs(29) == 5000);
  CHECK(ShortRequestTimeoutMs(10) == 5000);
  CHECK(ShortRequestTimeoutMs(6) == 5000);  // the floor is below the timeout itself here
  CHECK(ShortRequestTimeoutMs(31) == 5166); // a plain sixth, not rounded
  CHECK(ShortRequestTimeoutMs(36) == 6000);
}

TEST_CASE("ShortRequestTimeoutMs is never longer than the timeout itself", "[RequestTimeout]")
{
  CHECK(ShortRequestTimeoutMs(5) == 5000); // the setting's own minimum: the bound equals the timeout
  CHECK(ShortRequestTimeoutMs(4) == 4000);
  CHECK(ShortRequestTimeoutMs(1) == 1000);
}

TEST_CASE("ShortRequestTimeoutMs tolerates a zero or negative timeout", "[RequestTimeout]")
{
  // Not reachable through the setting (minimum 5), but this is a bound on a request, so it must still be a usable
  // positive number if the config is ever handed something odd.
  CHECK(ShortRequestTimeoutMs(0) == kMinShortRequestTimeoutMs);
  CHECK(ShortRequestTimeoutMs(-30) == kMinShortRequestTimeoutMs);
}

TEST_CASE("ShortRequestTimeoutMs never decreases as the timeout grows", "[RequestTimeout]")
{
  long previous = 0;
  for (int seconds = 5; seconds <= 400; ++seconds)
  {
    const long now = ShortRequestTimeoutMs(seconds);
    CHECK(now >= previous);
    CHECK(now <= static_cast<long>(seconds) * 1000);
    previous = now;
  }
}

TEST_CASE("IsInProgressSteadyState is a refresh that already has segments and is not an open's cold start",
          "[RequestTimeout]")
{
  CHECK(IsInProgressSteadyState(false, true));
  CHECK_FALSE(IsInProgressSteadyState(false, false)); // nothing merged yet
  // A reopened recording is seeded from the segment cache, so it has segments, and is a cold start all the same.
  CHECK_FALSE(IsInProgressSteadyState(true, true));
  CHECK_FALSE(IsInProgressSteadyState(true, false));
}

TEST_CASE("ShouldSkipPlaylistAfterUnansweredLookup skips only for a server that did not answer", "[RequestTimeout]")
{
  // Steady state, the lookup failed with no HTTP status, and authentication was not the reason: nobody is answering.
  CHECK(ShouldSkipPlaylistAfterUnansweredLookup(true, false, 0, false));
  // Authentication refused the call (a cooldown or backoff): no request was made, and the playlist needs only the key.
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(true, false, 0, true));
  // The server answered, however it answered.
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(true, false, 404, false));
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(true, false, 500, false));
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(true, true, 200, false));
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(true, true, 0, false));
  // A cold start never skips: its loop treats a failed refresh as retryable and its playlist may answer when the lookup
  // did not.
  CHECK_FALSE(ShouldSkipPlaylistAfterUnansweredLookup(false, false, 0, false));
}
