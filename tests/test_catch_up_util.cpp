#include "CatchUpUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
// Minimal stand-in for LiveTimeshiftSegmentInfo/InProgressRecordingSegmentInfo
// -- EstimateSegmentDurationMs is templated purely on a `timeOffsetMs`
// member, so neither real (heavier) type is needed here.
struct FakeSegment
{
  int64_t timeOffsetMs;
};
} // namespace

// ---------------------------------------------------------------------
// EstimateSegmentDurationMs
// ---------------------------------------------------------------------

TEST_CASE("EstimateSegmentDurationMs averages the last N segments", "[CatchUpUtil]")
{
  std::vector<FakeSegment> segments = {{0}, {2000}, {4000}, {6000}, {8000}};
  int64_t estimate = EstimateSegmentDurationMs(10000, segments);
  CHECK(estimate == 2000);
}

TEST_CASE("EstimateSegmentDurationMs works with fewer than the sample count", "[CatchUpUtil]")
{
  std::vector<FakeSegment> segments = {{0}, {3000}};
  int64_t estimate = EstimateSegmentDurationMs(6000, segments);
  CHECK(estimate == 3000);
}

TEST_CASE("EstimateSegmentDurationMs falls back to the default with no segments", "[CatchUpUtil]")
{
  std::vector<FakeSegment> segments;
  int64_t estimate = EstimateSegmentDurationMs(0, segments);
  CHECK(estimate == 6000); // the hardcoded default, floor doesn't change it
}

TEST_CASE("EstimateSegmentDurationMs floors an implausibly small average -- the real incident", "[CatchUpUtil]")
{
  // The exact documented crash: a real instance produced a burst of
  // segments only ~151ms apart, which -- without the floor -- would
  // collapse the retry budget until ffmpeg read the resulting stall as
  // genuine end-of-stream and closed playback outright.
  std::vector<FakeSegment> segments = {{0}, {0}, {0}, {0}, {0}};
  int64_t estimate = EstimateSegmentDurationMs(150, segments);
  CHECK(estimate == kMinSegmentDurationMs); // 30ms raw average, floored to 1500
}

TEST_CASE("EstimateSegmentDurationMs falls back to the default on nonsensical inputs", "[CatchUpUtil]")
{
  // totalDurationMs less than the sample window's own start offset --
  // inconsistent data the function shouldn't trust.
  std::vector<FakeSegment> segments = {{5000}};
  int64_t estimate = EstimateSegmentDurationMs(0, segments);
  CHECK(estimate == 6000);
}

TEST_CASE("EstimateSegmentDurationMs caps an implausibly large average -- the real bug this fixes", "[CatchUpUtil]")
{
  // One outlier segment (a real, plausible trigger: a mid-stream PTS
  // discontinuity from a glitchy IPTV source, confirmed against
  // Dispatcharr's own real current upstream source, not itself
  // independently reproduced) among the last few otherwise-normal ones
  // used to inflate the average -- and, downstream,
  // ComputeCatchUpAttempts()'s own blocking-read budget -- from seconds
  // to potentially hours. A lone 1-hour (3,600,000ms) segment among 4
  // near-zero ones averages to 720,000ms raw; without a cap this
  // function would return that, not the ceiling.
  std::vector<FakeSegment> segments = {{0}, {0}, {0}, {0}, {3600000}};
  int64_t estimate = EstimateSegmentDurationMs(3600000, segments);
  CHECK(estimate == kMaxSegmentDurationMs);
}

TEST_CASE("EstimateSegmentDurationMs leaves a realistic average well under the cap unchanged", "[CatchUpUtil]")
{
  std::vector<FakeSegment> segments = {{0}, {4000}, {8000}, {12000}, {16000}};
  int64_t estimate = EstimateSegmentDurationMs(20000, segments);
  CHECK(estimate == 4000);
}

// ---------------------------------------------------------------------
// ComputeCatchUpAttempts
// ---------------------------------------------------------------------

TEST_CASE("ComputeCatchUpAttempts always allows exactly one attempt for a likely seek probe", "[CatchUpUtil]")
{
  CHECK(ComputeCatchUpAttempts(true, 2000, 250) == 1);
  CHECK(ComputeCatchUpAttempts(true, 999999, 1) == 1);
}

TEST_CASE("ComputeCatchUpAttempts uses a 3x margin, not 1.5x", "[CatchUpUtil]")
{
  // The exact documented fix: 1.5x ran too thin in practice, confirmed
  // live that ordinary non-error catch-up cycles routinely used
  // 60-95% of that budget under normal jitter.
  CHECK(ComputeCatchUpAttempts(false, 2000, 250) == 25); // (2000*3)/250 + 1
}

TEST_CASE("ComputeCatchUpAttempts at the floored segment duration", "[CatchUpUtil]")
{
  CHECK(ComputeCatchUpAttempts(false, kMinSegmentDurationMs, 250) == 19); // (1500*3)/250 + 1
}

TEST_CASE("ComputeCatchUpAttempts still returns at least one attempt for a zero estimate", "[CatchUpUtil]")
{
  CHECK(ComputeCatchUpAttempts(false, 0, 250) == 1);
}

TEST_CASE("ComputeCatchUpAttempts stays bounded to a few minutes at most at the capped segment duration -- "
          "confirms EstimateSegmentDurationMs's own cap actually closes the real-world symptom",
          "[CatchUpUtil]")
{
  int attempts = ComputeCatchUpAttempts(false, kMaxSegmentDurationMs, 250);
  double worstCaseSeconds = attempts * 250 / 1000.0;
  CHECK(worstCaseSeconds < 300.0); // well under 5 minutes, not the ~14 hours an uncapped outlier could reach
}

// ---------------------------------------------------------------------
// IsLikelySeekProbe
// ---------------------------------------------------------------------

TEST_CASE("IsLikelySeekProbe is true shortly after a real seek", "[CatchUpUtil]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastSeekTime = now - std::chrono::milliseconds(100);
  CHECK(IsLikelySeekProbe(/*position=*/1000, /*lastShortGiveUpPosition=*/-1, lastSeekTime, now,
                          std::chrono::milliseconds(800)));
}

TEST_CASE("IsLikelySeekProbe is false once outside the seek probe window", "[CatchUpUtil]")
{
  auto now = std::chrono::steady_clock::now();
  auto lastSeekTime = now - std::chrono::milliseconds(900);
  CHECK_FALSE(IsLikelySeekProbe(/*position=*/1000, /*lastShortGiveUpPosition=*/-1, lastSeekTime, now,
                                std::chrono::milliseconds(800)));
}

TEST_CASE("IsLikelySeekProbe is false when nothing has ever seeked (zero lastSeekTime)", "[CatchUpUtil]")
{
  auto now = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point neverSeeked; // epoch, time_since_epoch() == 0
  CHECK_FALSE(IsLikelySeekProbe(/*position=*/1000, /*lastShortGiveUpPosition=*/-1, neverSeeked, now,
                                std::chrono::milliseconds(800)));
}

TEST_CASE("IsLikelySeekProbe escalates once a read already gave up quickly at this exact position", "[CatchUpUtil]")
{
  // Otherwise inside the seek-probe window, but this exact position already
  // got the short budget last time and still didn't catch up -- treat it as
  // a genuine stuck read now, not a fresh probe candidate.
  auto now = std::chrono::steady_clock::now();
  auto lastSeekTime = now - std::chrono::milliseconds(100);
  CHECK_FALSE(IsLikelySeekProbe(/*position=*/1000, /*lastShortGiveUpPosition=*/1000, lastSeekTime, now,
                                std::chrono::milliseconds(800)));
}

// ---------------------------------------------------------------------
// ComputeShortGiveUpPosition
// ---------------------------------------------------------------------

TEST_CASE("ComputeShortGiveUpPosition remembers the position for a probe that didn't catch up", "[CatchUpUtil]")
{
  CHECK(ComputeShortGiveUpPosition(/*likelySeekProbe=*/true, /*caughtUp=*/false, /*position=*/1000) == 1000);
}

TEST_CASE("ComputeShortGiveUpPosition clears back to -1 once caught up", "[CatchUpUtil]")
{
  CHECK(ComputeShortGiveUpPosition(/*likelySeekProbe=*/true, /*caughtUp=*/true, /*position=*/1000) == -1);
}

TEST_CASE("ComputeShortGiveUpPosition clears back to -1 for a genuine (non-probe) give-up", "[CatchUpUtil]")
{
  // A real catch-up that exhausted its full budget without a seek probe --
  // don't leave a stale short-give-up position behind for an unrelated
  // future read to match against.
  CHECK(ComputeShortGiveUpPosition(/*likelySeekProbe=*/false, /*caughtUp=*/false, /*position=*/1000) == -1);
}

// ---------------------------------------------------------------------
// The wall-clock budget (the 2026-10-04 fifth hardening sweep): the attempt count alone let one
// blocking Read() run 25 to 50 minutes against a server that times out instead of refusing.
// ---------------------------------------------------------------------

TEST_CASE("ComputeCatchUpWallClockBudgetMs is twice the attempts' own sleep time, capped", "[CatchUpUtil]")
{
  // A 4 s segment: 49 attempts of 250 ms sleep is 12.25 s; the budget leaves room for round trips.
  const int attempts = ComputeCatchUpAttempts(false, 4000, 250);
  CHECK(attempts == 49);
  CHECK(ComputeCatchUpWallClockBudgetMs(attempts, 250) == 24500);
  // A seek probe: one attempt.
  CHECK(ComputeCatchUpWallClockBudgetMs(1, 250) == 500);
  // The segment-estimate ceiling (60 s) would be 721 attempts: capped, not 6 minutes.
  const int worst = ComputeCatchUpAttempts(false, kMaxSegmentDurationMs, 250);
  CHECK(worst == 721);
  CHECK(ComputeCatchUpWallClockBudgetMs(worst, 250) == kMaxCatchUpWallClockMs);
  CHECK(kMaxCatchUpWallClockMs <= 120000);
}

TEST_CASE("ComputeCatchUpWallClockBudgetMs is never negative or overflowing", "[CatchUpUtil]")
{
  CHECK(ComputeCatchUpWallClockBudgetMs(0, 250) == 0);
  CHECK(ComputeCatchUpWallClockBudgetMs(-5, 250) == 0);
  CHECK(ComputeCatchUpWallClockBudgetMs(2000000000, 250) == kMaxCatchUpWallClockMs);
}

TEST_CASE("HasCatchUpBudgetElapsed is true exactly at the budget", "[CatchUpUtil]")
{
  using std::chrono::milliseconds;
  CHECK_FALSE(HasCatchUpBudgetElapsed(milliseconds(0), 24500));
  CHECK_FALSE(HasCatchUpBudgetElapsed(milliseconds(24499), 24500));
  CHECK(HasCatchUpBudgetElapsed(milliseconds(24500), 24500));
  CHECK(HasCatchUpBudgetElapsed(milliseconds(24501), 24500));
}

TEST_CASE("EstimateSegmentDurationMs falls back to the default when the sampled span is zero or negative",
          "[CatchUpUtil]")
{
  // Every sampled segment shares the total duration as its offset: no span to average, so the 6 s default.
  std::vector<FakeSegment> flat{{5000}, {5000}, {5000}};
  CHECK(EstimateSegmentDurationMs(5000, flat) == 6000);
  // A total behind the first sampled offset (a stale total) is the negative case.
  CHECK(EstimateSegmentDurationMs(1000, flat) == 6000);
}

TEST_CASE("IsLikelySeekProbe is false at exactly the window's edge and true just inside it", "[CatchUpUtil]")
{
  using namespace std::chrono;
  const steady_clock::time_point seek = steady_clock::time_point(seconds(100));
  const auto window = milliseconds(2000);
  CHECK(IsLikelySeekProbe(5, -1, seek, seek + window - milliseconds(1), window));
  CHECK_FALSE(IsLikelySeekProbe(5, -1, seek, seek + window, window));
  CHECK_FALSE(IsLikelySeekProbe(5, -1, seek, seek + window + milliseconds(1), window));
}

TEST_CASE("HasTailWaitSliceElapsed hands a read back once its slice is used", "[CatchUpUtil]")
{
  CHECK_FALSE(HasTailWaitSliceElapsed(std::chrono::milliseconds(0)));
  CHECK_FALSE(HasTailWaitSliceElapsed(std::chrono::milliseconds(kLiveTailWaitSliceMs - 1)));
  CHECK(HasTailWaitSliceElapsed(std::chrono::milliseconds(kLiveTailWaitSliceMs)));
  CHECK(HasTailWaitSliceElapsed(std::chrono::milliseconds(30000)));
}

TEST_CASE("TailWaitEpisode counts the wait across the calls of one episode at one position", "[CatchUpUtil]")
{
  using namespace std::chrono;
  const auto t0 = steady_clock::now();
  TailWaitEpisode episode;
  CHECK(episode.WaitedMs(t0) == 0); // nothing entered yet
  CHECK(episode.Enter(5000, t0));   // a new episode
  CHECK(episode.WaitedMs(t0 + milliseconds(1000)) == 1000);
  episode.Touch(t0 + milliseconds(1000)); // a call returned -1; Kodi retries at once
  CHECK_FALSE(episode.Enter(5000, t0 + milliseconds(1010)));
  CHECK(episode.WaitedMs(t0 + milliseconds(2300)) == 2300); // counted from the first call, not the last
}

TEST_CASE("TailWaitEpisode starts again when the position moved or the previous call is too old", "[CatchUpUtil]")
{
  using namespace std::chrono;
  const auto t0 = steady_clock::now();
  TailWaitEpisode episode;
  episode.Enter(5000, t0);
  episode.Touch(t0 + milliseconds(1000));
  // a seek or a read of fresh data changed the position
  CHECK(episode.Enter(6000, t0 + milliseconds(1010)));
  CHECK(episode.WaitedMs(t0 + milliseconds(1010)) == 0);
  // the same position, but Kodi was paused (no read for longer than the episode gap)
  episode.Touch(t0 + milliseconds(1010));
  CHECK(episode.Enter(6000, t0 + milliseconds(1010 + kTailWaitEpisodeGapMs + 1)));
  CHECK(episode.WaitedMs(t0 + milliseconds(1010 + kTailWaitEpisodeGapMs + 1)) == 0);
  // exactly at the gap it still continues
  episode.Touch(t0 + milliseconds(5000));
  CHECK_FALSE(episode.Enter(6000, t0 + milliseconds(5000 + kTailWaitEpisodeGapMs)));
}

TEST_CASE("TailWaitEpisode::Reset forgets the episode", "[CatchUpUtil]")
{
  using namespace std::chrono;
  const auto t0 = steady_clock::now();
  TailWaitEpisode episode;
  episode.Enter(5000, t0);
  episode.Reset();
  CHECK(episode.WaitedMs(t0 + milliseconds(4000)) == 0);
  CHECK(episode.Enter(5000, t0 + milliseconds(4001))); // the same position is a new episode again
}

TEST_CASE("DecideTailWaitOutcome gives up only with the whole budget used, or for a seek probe", "[CatchUpUtil]")
{
  CHECK(DecideTailWaitOutcome(false, 0, 6250) == TailWaitOutcome::kRetry);
  CHECK(DecideTailWaitOutcome(false, 1000, 6250) == TailWaitOutcome::kRetry);
  CHECK(DecideTailWaitOutcome(false, 6249, 6250) == TailWaitOutcome::kRetry);
  CHECK(DecideTailWaitOutcome(false, 6250, 6250) == TailWaitOutcome::kGiveUp); // exactly the budget
  CHECK(DecideTailWaitOutcome(false, 20000, 6250) == TailWaitOutcome::kGiveUp);
  // a likely seek probe never waits in slices: the quick "not there" answer is the point
  CHECK(DecideTailWaitOutcome(true, 0, 250) == TailWaitOutcome::kGiveUp);
}
