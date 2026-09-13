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
