#include "StreamWarmUp.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ShouldWarmLiveStream warms the modes that return a URL and not server-side timeshift", "[StreamWarmUp]")
{
  const std::string url = "http://host/proxy/ts/stream/abc|Connection=close";
  CHECK(ShouldWarmLiveStream(kLiveTimeshiftOff, url));
  CHECK(ShouldWarmLiveStream(kLiveTimeshiftLocal, url));
  // Server mode leaves the URL empty and reads through the addon's own callbacks: there is nothing to start.
  CHECK_FALSE(ShouldWarmLiveStream(kLiveTimeshiftServer, ""));
  CHECK_FALSE(ShouldWarmLiveStream(kLiveTimeshiftServer, url));
  CHECK_FALSE(ShouldWarmLiveStream(kLiveTimeshiftOff, ""));
}

TEST_CASE("StreamUrlWithoutKodiOptions drops the pipe options only", "[StreamWarmUp]")
{
  CHECK(StreamUrlWithoutKodiOptions("http://h:9191/proxy/ts/stream/u|Connection=close") ==
        "http://h:9191/proxy/ts/stream/u");
  CHECK(StreamUrlWithoutKodiOptions("http://h:9191/proxy/ts/stream/u") == "http://h:9191/proxy/ts/stream/u");
  CHECK(StreamUrlWithoutKodiOptions("http://h/x|a=b&c=d") == "http://h/x");
  CHECK(StreamUrlWithoutKodiOptions("").empty());
}

TEST_CASE("WarmStreamBudgetMs is four times the short bound and stays within 20 to 60 seconds", "[StreamWarmUp]")
{
  // 2 to 7 s was measured on a healthy server, and one slow channel took more than 12 s, so the default clears both.
  CHECK(WarmStreamBudgetMs(30) == 20000);
  CHECK(WarmStreamBudgetMs(5) == 20000);
  CHECK(WarmStreamBudgetMs(1) == 4 * ShortRequestTimeoutMs(1));
  CHECK(WarmStreamBudgetMs(180) == 60000);
  CHECK(WarmStreamBudgetMs(600) == 60000);
  CHECK(WarmStreamBudgetMs(30) >= 12000);
}

TEST_CASE("DecideWarmStreamStep is ready only for a 200 with data that the addon itself ended", "[StreamWarmUp]")
{
  CHECK(DecideWarmStreamStep(false, 200, true, 3000, 10000) == WarmStreamStep::kReady);
  // Ready even when the budget is spent by the time the data arrived: the channel is running, which is the point.
  CHECK(DecideWarmStreamStep(false, 200, true, 10500, 10000) == WarmStreamStep::kReady);
  // A 200 whose body ended with no data is an empty answer, not a channel coming up.
  CHECK(DecideWarmStreamStep(false, 200, false, 3000, 10000) == WarmStreamStep::kGiveUp);
}

TEST_CASE("DecideWarmStreamStep retries while the channel is coming up and gives up on a final answer",
          "[StreamWarmUp]")
{
  // The 503 + Retry-After the proxy sends while it starts a channel, and a gateway saying the same.
  CHECK(DecideWarmStreamStep(false, 503, false, 1000, 10000) == WarmStreamStep::kRetry);
  CHECK(DecideWarmStreamStep(false, 502, false, 1000, 10000) == WarmStreamStep::kRetry);
  CHECK(DecideWarmStreamStep(false, 504, false, 1000, 10000) == WarmStreamStep::kRetry);
  // A plain 500, answered at once while the proxy was setting the channel up (seen live: the next attempt worked).
  CHECK(DecideWarmStreamStep(false, 500, false, 1000, 10000) == WarmStreamStep::kRetry);
  // No answer at all: refused, reset or timed out.
  CHECK(DecideWarmStreamStep(true, 0, false, 1000, 10000) == WarmStreamStep::kRetry);
  CHECK(DecideWarmStreamStep(true, 200, false, 1000, 10000) == WarmStreamStep::kRetry);
  // The server's answer about the channel itself: asking again changes nothing.
  CHECK(DecideWarmStreamStep(false, 404, false, 1000, 10000) == WarmStreamStep::kGiveUp);
  CHECK(DecideWarmStreamStep(false, 401, false, 1000, 10000) == WarmStreamStep::kGiveUp);
  CHECK(DecideWarmStreamStep(false, 403, false, 1000, 10000) == WarmStreamStep::kGiveUp);
  CHECK(DecideWarmStreamStep(false, 501, false, 1000, 10000) == WarmStreamStep::kGiveUp);
}

TEST_CASE("DecideWarmStreamStep gives up once the budget is spent whatever the last answer was", "[StreamWarmUp]")
{
  CHECK(DecideWarmStreamStep(false, 503, false, 10000, 10000) == WarmStreamStep::kGiveUp);
  CHECK(DecideWarmStreamStep(true, 0, false, 12000, 10000) == WarmStreamStep::kGiveUp);
  CHECK(DecideWarmStreamStep(false, 503, false, 9999, 10000) == WarmStreamStep::kRetry);
}

TEST_CASE("ShouldKeepWarmHold holds for kWarmHoldMs after the first bytes unless stopped", "[StreamWarmUp]")
{
  CHECK(ShouldKeepWarmHold(false, 0));
  CHECK(ShouldKeepWarmHold(false, kWarmHoldMs - 1));
  CHECK_FALSE(ShouldKeepWarmHold(false, kWarmHoldMs));
  CHECK_FALSE(ShouldKeepWarmHold(false, kWarmHoldMs + 5000));
  // A stop (a new warm-up, an abort, the client going away) ends it at once, however recent the first bytes.
  CHECK_FALSE(ShouldKeepWarmHold(true, 0));
  // Longer than Kodi needs to get its own connection up (about 8 s), shorter than leaving a channel running unwatched.
  CHECK(kWarmHoldMs >= 10000);
  CHECK(kWarmHoldMs <= 30000);
}
