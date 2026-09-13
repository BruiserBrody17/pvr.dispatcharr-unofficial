#include "LiveEdgeMargin.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
// Minimal stand-in for LiveTimeshiftSegmentInfo/InProgressRecordingSegmentInfo
// -- both functions here are templated purely on byteOffset/timeOffsetMs/
// byteSize members, so neither real (heavier) type is needed here.
struct FakeSegment
{
  int64_t byteOffset;
  int64_t byteSize;
  int64_t timeOffsetMs;
};
} // namespace

// ---------------------------------------------------------------------
// ComputeLiveEdgeTailTarget
// ---------------------------------------------------------------------

TEST_CASE("ComputeLiveEdgeTailTarget backs off by the combined byteSize of the trailing margin segments",
          "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 100, 0}, {100, 100, 0}, {200, 100, 0}, {300, 100, 0}};

  // Trailing 2 segments (byteSize 100 each) -> back off 200 from totalBytes=400.
  CHECK(ComputeLiveEdgeTailTarget(segments, 400, 2) == 200);
}

TEST_CASE("ComputeLiveEdgeTailTarget matches the in-progress-recording margin-of-1 case exactly", "[LiveEdgeMargin]")
{
  // The exact invariant SeekInProgressRecordingStream() relies on: with
  // marginSegments=1, this must equal totalBytes minus just the last
  // segment's own byteSize.
  std::vector<FakeSegment> segments = {{0, 50, 0}, {50, 70, 0}, {120, 30, 0}};

  CHECK(ComputeLiveEdgeTailTarget(segments, 150, 1) == 120); // 150 - 30 (last segment's byteSize)
}

TEST_CASE("ComputeLiveEdgeTailTarget returns totalBytes unchanged when there are no segments", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments;

  CHECK(ComputeLiveEdgeTailTarget(segments, 0, 1) == 0);
  CHECK(ComputeLiveEdgeTailTarget(segments, 0, 3) == 0);
}

TEST_CASE("ComputeLiveEdgeTailTarget backs off by every segment when fewer exist than the margin", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 40, 0}, {40, 60, 0}};

  // Only 2 segments exist but margin is 3 -- back off by both (100 total).
  CHECK(ComputeLiveEdgeTailTarget(segments, 100, 3) == 0);
}

TEST_CASE("ComputeLiveEdgeTailTarget never goes negative", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 1000, 0}};

  CHECK(ComputeLiveEdgeTailTarget(segments, 500, 1) == 0);
}

// ---------------------------------------------------------------------
// TrimToTrailingLiveEdgeMargin
// ---------------------------------------------------------------------

TEST_CASE("TrimToTrailingLiveEdgeMargin drops everything but the trailing margin, rebasing the rest",
          "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {
      {0, 100, 0}, {100, 100, 2000}, {200, 100, 4000}, {300, 100, 6000}, {400, 100, 8000}};
  int64_t totalBytes = 500;
  int64_t totalDurationMs = 10000;

  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 3);

  REQUIRE(segments.size() == 3);
  // The oldest surviving segment (originally byteOffset=200/timeOffsetMs=4000)
  // becomes local byte/time 0.
  CHECK(segments[0].byteOffset == 0);
  CHECK(segments[0].timeOffsetMs == 0);
  CHECK(segments[1].byteOffset == 100);
  CHECK(segments[1].timeOffsetMs == 2000);
  CHECK(segments[2].byteOffset == 200);
  CHECK(segments[2].timeOffsetMs == 4000);
  CHECK(totalBytes == 300);
  CHECK(totalDurationMs == 6000);
}

TEST_CASE("TrimToTrailingLiveEdgeMargin is a no-op when there aren't more segments than the margin", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 100, 0}, {100, 100, 2000}};
  int64_t totalBytes = 200;
  int64_t totalDurationMs = 4000;

  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 3);

  REQUIRE(segments.size() == 2);
  CHECK(segments[0].byteOffset == 0);
  CHECK(segments[1].byteOffset == 100);
  CHECK(totalBytes == 200);
  CHECK(totalDurationMs == 4000);
}

TEST_CASE("TrimToTrailingLiveEdgeMargin is a no-op for an empty segment list", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments;
  int64_t totalBytes = 0;
  int64_t totalDurationMs = 0;

  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 3);

  CHECK(segments.empty());
  CHECK(totalBytes == 0);
  CHECK(totalDurationMs == 0);
}
