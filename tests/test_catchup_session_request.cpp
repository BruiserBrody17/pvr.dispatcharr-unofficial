#include "CatchupSessionRequest.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("BuildCatchupSessionBody sends the channel, the ISO start and a positive duration", "[CatchupSessionRequest]")
{
  const nlohmann::json body = BuildCatchupSessionBody("abc-123", 1767268800, 90);
  CHECK(body["channel_uuid"] == "abc-123");
  CHECK(body["start"] == IsoFromTime(1767268800));
  CHECK(body["duration"] == 90);
  CHECK(body.size() == 3);
}

TEST_CASE("BuildCatchupSessionBody omits the duration when none is known", "[CatchupSessionRequest]")
{
  for (int duration : {0, -1, -480})
  {
    const nlohmann::json body = BuildCatchupSessionBody("abc-123", 1767268800, duration);
    CHECK_FALSE(body.contains("duration"));
    CHECK(body.size() == 2);
  }
}

TEST_CASE("BuildCatchupSessionBody clamps the duration to the API's own maximum", "[CatchupSessionRequest]")
{
  CHECK(BuildCatchupSessionBody("c", 0, 480)["duration"] == 480);
  CHECK(BuildCatchupSessionBody("c", 0, 481)["duration"] == kMaxCatchupSessionDurationMinutes);
  CHECK(BuildCatchupSessionBody("c", 0, 100000)["duration"] == 480);
}

TEST_CASE("ResolveCatchupPlaybackUrl joins a relative playback_url to the configured server", "[CatchupSessionRequest]")
{
  CHECK(ResolveCatchupPlaybackUrl("/proxy/catchup/uuid?session_id=s1", "http://dispatcharr.example:9191") ==
        "http://dispatcharr.example:9191/proxy/catchup/uuid?session_id=s1");
}

TEST_CASE("ResolveCatchupPlaybackUrl leaves an absolute http(s) URL alone", "[CatchupSessionRequest]")
{
  CHECK(ResolveCatchupPlaybackUrl("http://other.example/x", "http://dispatcharr.example:9191") ==
        "http://other.example/x");
  CHECK(ResolveCatchupPlaybackUrl("https://other.example/x", "http://dispatcharr.example:9191") ==
        "https://other.example/x");
  // Only a real scheme prefix counts: a path that merely mentions one is still relative.
  CHECK(ResolveCatchupPlaybackUrl("/redirect?to=http://x", "http://d.example") ==
        "http://d.example/redirect?to=http://x");
}
