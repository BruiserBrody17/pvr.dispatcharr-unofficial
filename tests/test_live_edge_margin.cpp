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

// ---------------------------------------------------------------------
// ComputeLiveEdgeStartPosition
// ---------------------------------------------------------------------

TEST_CASE("ComputeLiveEdgeStartPosition lands on the trimmed window's own start, the real caller's shape",
          "[LiveEdgeMargin]")
{
  // The real caller (OpenLiveTimeshiftStream()) always calls this right
  // after TrimToTrailingLiveEdgeMargin(), so segments.size() <=
  // marginSegments and the oldest surviving segment's byteOffset is 0.
  std::vector<FakeSegment> segments = {{0, 100, 0}, {100, 100, 2000}, {200, 100, 4000}};
  CHECK(ComputeLiveEdgeStartPosition(segments, 300, 3) == 0);
}

TEST_CASE("ComputeLiveEdgeStartPosition picks the segment marginSegments back from the tail", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 100, 0}, {100, 100, 0}, {200, 100, 0}, {300, 100, 0}, {400, 100, 0}};
  // 5 segments, margin 3 -> index 2 (byteOffset=200).
  CHECK(ComputeLiveEdgeStartPosition(segments, 500, 3) == 200);
}

TEST_CASE("ComputeLiveEdgeStartPosition falls back to the true tail with no segments yet", "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments;
  CHECK(ComputeLiveEdgeStartPosition(segments, 0, 3) == 0);
}

TEST_CASE("ComputeLiveEdgeStartPosition falls back to totalBytes when fewer segments exist than the margin",
          "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 50, 0}};
  // Only 1 segment but margin is 3 -- marginIndex clamps to 0, and
  // segments[0].byteOffset (0) is what's actually returned here, not
  // totalBytes -- this branch is only reached with zero segments.
  CHECK(ComputeLiveEdgeStartPosition(segments, 50, 3) == 0);
}

// ---------------------------------------------------------------------
// FirstAvailableLiveSegmentIndex
// ---------------------------------------------------------------------

namespace
{
struct FakeSeqSegment
{
  int64_t sequence;
};
} // namespace

TEST_CASE("FirstAvailableLiveSegmentIndex is 0 until the server's oldest sequence is known", "[LiveEdgeMargin]")
{
  std::vector<FakeSeqSegment> segments = {{10}, {11}, {12}};
  CHECK(FirstAvailableLiveSegmentIndex(segments, -1) == 0);
}

TEST_CASE("FirstAvailableLiveSegmentIndex skips the segments the server's buffer has rolled off", "[LiveEdgeMargin]")
{
  std::vector<FakeSeqSegment> segments = {{10}, {11}, {12}, {13}, {14}};
  CHECK(FirstAvailableLiveSegmentIndex(segments, 10) == 0);
  CHECK(FirstAvailableLiveSegmentIndex(segments, 12) == 2);
  CHECK(FirstAvailableLiveSegmentIndex(segments, 14) == 4);
}

TEST_CASE("FirstAvailableLiveSegmentIndex copes with an oldest sequence below or beyond what this stream holds",
          "[LiveEdgeMargin]")
{
  std::vector<FakeSeqSegment> segments = {{10}, {11}, {12}};
  // This stream joined after the server's window began: nothing of its own has rolled off.
  CHECK(FirstAvailableLiveSegmentIndex(segments, 3) == 0);
  // Everything this stream holds is older than the window: index == size, so callers fall back to 0 themselves.
  CHECK(FirstAvailableLiveSegmentIndex(segments, 99) == 3);
  CHECK(FirstAvailableLiveSegmentIndex(std::vector<FakeSeqSegment>{}, 5) == 0);
}

TEST_CASE("FirstAvailableLiveSegmentIndex copes with a gap in the sequence numbers", "[LiveEdgeMargin]")
{
  // A segment the client dropped as malformed leaves a hole; the bound is still the first one >= oldest.
  std::vector<FakeSeqSegment> segments = {{10}, {11}, {13}, {14}};
  CHECK(FirstAvailableLiveSegmentIndex(segments, 12) == 2);
}

// ---------------------------------------------------------------------
// ClampSeekToTail
// ---------------------------------------------------------------------

TEST_CASE("ClampSeekToTail leaves a target at or before the tail alone", "[LiveEdgeMargin]")
{
  CHECK(ClampSeekToTail(500, 900, 1000) == 500);
  CHECK(ClampSeekToTail(1000, 900, 1000) == 1000);
  CHECK(ClampSeekToTail(0, 0, 0) == 0);
}

TEST_CASE("ClampSeekToTail lands on the tail target when the reader is not past it", "[LiveEdgeMargin]")
{
  // The ordinary seek-to-live: well behind the margin, jump forward to it.
  CHECK(ClampSeekToTail(5000, 200, 1000) == 1000);
  CHECK(ClampSeekToTail(5000, 1000, 1000) == 1000);
}

TEST_CASE("ClampSeekToTail never moves a reader that is already past the tail target backward on a forward seek",
          "[LiveEdgeMargin]")
{
  // The live-confirmed case: current P past tailTarget P-M.
  CHECK(ClampSeekToTail(167260968, 165408212, 164097492) == 165408212);
  // Requested only a little past the tail, but still ahead of current: stays at current.
  CHECK(ClampSeekToTail(1200, 1100, 1000) == 1100);
}

TEST_CASE("ClampSeekToTail does not push a short backward seek inside the margin further back", "[LiveEdgeMargin]")
{
  // Past the tail target but behind the reader: honour it (the old clamp sent it to 1000).
  CHECK(ClampSeekToTail(1050, 1100, 1000) == 1050);
}

TEST_CASE("ClampSeekToTail never lands past the requested target", "[LiveEdgeMargin]")
{
  for (int64_t requested : {0, 10, 999, 1000, 1001, 1500, 100000})
    for (int64_t current : {0, 500, 1000, 1100, 1900})
      for (int64_t tail : {0, 800, 1000})
      {
        const int64_t r = ClampSeekToTail(requested, current, tail);
        CHECK(r <= std::max(requested, tail));
        if (requested <= tail)
          CHECK(r == requested);
        else
          CHECK(r >= tail);
      }
}

// ---------------------------------------------------------------------
// IsAtEndedTail
// ---------------------------------------------------------------------

TEST_CASE("IsAtEndedTail ends a read only once ffmpeg has exited AND the reader reached the end", "[LiveEdgeMargin]")
{
  CHECK(IsAtEndedTail(true, 1000, 1000));
  CHECK(IsAtEndedTail(true, 1200, 1000));
}

TEST_CASE("IsAtEndedTail never stops a reader still behind the tail -- the rewind window survives", "[LiveEdgeMargin]")
{
  CHECK_FALSE(IsAtEndedTail(true, 0, 1000));
  CHECK_FALSE(IsAtEndedTail(true, 999, 1000));
}

TEST_CASE("IsAtEndedTail leaves a still-running buffer to the ordinary catch-up wait", "[LiveEdgeMargin]")
{
  CHECK_FALSE(IsAtEndedTail(false, 1000, 1000));
  CHECK_FALSE(IsAtEndedTail(false, 5000, 1000));
}

// ---------------------------------------------------------------------
// ResolveLiveSeekTarget
// ---------------------------------------------------------------------

TEST_CASE("ResolveLiveSeekTarget leaves a seek inside the window alone", "[LiveEdgeMargin]")
{
  LiveSeekResolution r = ResolveLiveSeekTarget(/*requested=*/5000, /*current=*/2000, /*tail=*/9000, /*first=*/1000);
  CHECK(r.position == 5000);
  CHECK_FALSE(r.clampedToTail);
  CHECK_FALSE(r.clampedToHead);
}

TEST_CASE("ResolveLiveSeekTarget holds a forward seek back from the live edge", "[LiveEdgeMargin]")
{
  LiveSeekResolution r = ResolveLiveSeekTarget(12000, 2000, 9000, 1000);
  CHECK(r.position == 9000);
  CHECK(r.clampedToTail);
  CHECK_FALSE(r.clampedToHead);
  // A reader already past the tail target stays where it is rather than moving back.
  r = ResolveLiveSeekTarget(12000, 9500, 9000, 1000);
  CHECK(r.position == 9500);
  CHECK(r.clampedToTail);
}

TEST_CASE("ResolveLiveSeekTarget moves a seek behind the rolling window up to the oldest segment", "[LiveEdgeMargin]")
{
  LiveSeekResolution r = ResolveLiveSeekTarget(200, 5000, 9000, 1000);
  CHECK(r.position == 1000);
  CHECK(r.clampedToHead);
  CHECK_FALSE(r.clampedToTail);
}

TEST_CASE("ResolveLiveSeekTarget lets the head clamp win over the tail clamp -- the window has moved past the margin",
          "[LiveEdgeMargin]")
{
  // The buffer dropped everything before byte 9500 while the tail margin still
  // says 9000: landing at 9000 would read a recycled file, so 9500 it is.
  LiveSeekResolution r = ResolveLiveSeekTarget(12000, 2000, 9000, 9500);
  CHECK(r.position == 9500);
  CHECK(r.clampedToTail);
  CHECK(r.clampedToHead);
}

// ---------------------------------------------------------------------
// PruneRolledOffLiveSegments
// ---------------------------------------------------------------------

namespace
{
struct FakeLiveSegment
{
  int64_t sequence;
  int64_t byteOffset;
};
} // namespace

TEST_CASE("PruneRolledOffLiveSegments drops the leading segments the server has let go of", "[LiveEdgeMargin]")
{
  std::vector<FakeLiveSegment> segments = {{10, 0}, {11, 100}, {12, 200}, {13, 300}, {14, 400}};
  CHECK(PruneRolledOffLiveSegments(segments, 12) == 2);
  REQUIRE(segments.size() == 3);
  // The survivors keep their absolute positions in the stream's address space.
  CHECK(segments[0].sequence == 12);
  CHECK(segments[0].byteOffset == 200);
  CHECK(segments.back().sequence == 14);
  CHECK(segments.back().byteOffset == 400);
  // The first-available index for the same bound now points at the front.
  CHECK(FirstAvailableLiveSegmentIndex(segments, 12) == 0);
}

TEST_CASE("PruneRolledOffLiveSegments does nothing until the server's oldest sequence is known or reached",
          "[LiveEdgeMargin]")
{
  std::vector<FakeLiveSegment> segments = {{10, 0}, {11, 100}, {12, 200}};
  CHECK(PruneRolledOffLiveSegments(segments, -1) == 0);
  CHECK(PruneRolledOffLiveSegments(segments, 10) == 0);
  CHECK(PruneRolledOffLiveSegments(segments, 3) == 0);
  CHECK(segments.size() == 3);
}

TEST_CASE("PruneRolledOffLiveSegments always keeps the newest segment so the next merge still knows the last "
          "sequence",
          "[LiveEdgeMargin]")
{
  // The window has overtaken everything this stream holds (a long pause): the
  // last entry stays, because its sequence is what the next manifest merge reads
  // as "already known" -- dropping all of them would append the whole window again.
  std::vector<FakeLiveSegment> segments = {{10, 0}, {11, 100}, {12, 200}};
  CHECK(PruneRolledOffLiveSegments(segments, 99) == 2);
  REQUIRE(segments.size() == 1);
  CHECK(segments[0].sequence == 12);

  std::vector<FakeLiveSegment> one = {{5, 0}};
  CHECK(PruneRolledOffLiveSegments(one, 99) == 0);
  CHECK(one.size() == 1);
  std::vector<FakeLiveSegment> none;
  CHECK(PruneRolledOffLiveSegments(none, 5) == 0);
}

TEST_CASE("PruneRolledOffLiveSegments is idempotent and copes with a gap in the sequence", "[LiveEdgeMargin]")
{
  std::vector<FakeLiveSegment> segments = {{10, 0}, {11, 100}, {13, 300}, {14, 400}};
  CHECK(PruneRolledOffLiveSegments(segments, 12) == 2);
  CHECK(PruneRolledOffLiveSegments(segments, 12) == 0);
  REQUIRE(segments.size() == 2);
  CHECK(segments[0].sequence == 13);
}

TEST_CASE("TrimToTrailingLiveEdgeMargin leaves a list exactly as long as the margin untouched, offsets included",
          "[LiveEdgeMargin]")
{
  // A trim of zero segments must not rebase onto segments[0]'s offsets.
  std::vector<FakeSegment> segments = {{500, 100, 2000}, {600, 100, 3000}, {700, 100, 4000}};
  int64_t totalBytes = 800;
  int64_t totalDurationMs = 5000;
  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 3);
  REQUIRE(segments.size() == 3);
  CHECK(segments[0].byteOffset == 500);
  CHECK(segments[0].timeOffsetMs == 2000);
  CHECK(totalBytes == 800);
  CHECK(totalDurationMs == 5000);

  // One more than the margin trims exactly one and rebases to the first survivor.
  segments.push_back({800, 100, 5000});
  totalBytes = 900;
  totalDurationMs = 6000;
  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 3);
  REQUIRE(segments.size() == 3);
  CHECK(segments[0].byteOffset == 0);
  CHECK(segments[0].timeOffsetMs == 0);
  CHECK(totalBytes == 300);
  CHECK(totalDurationMs == 3000);
}

TEST_CASE("PruneRolledOffLiveSegments prunes a two-segment list down to its last segment", "[LiveEdgeMargin]")
{
  std::vector<FakeLiveSegment> two = {{10, 0}, {11, 100}};
  CHECK(PruneRolledOffLiveSegments(two, 11) == 1);
  REQUIRE(two.size() == 1);
  CHECK(two[0].sequence == 11);
  // One segment is never pruned, even when the oldest available sequence has overtaken it.
  std::vector<FakeLiveSegment> one = {{10, 0}};
  CHECK(PruneRolledOffLiveSegments(one, 50) == 0);
  CHECK(one.size() == 1);
}

TEST_CASE("TrimToTrailingLiveEdgeMargin with a margin of nothing keeps nothing instead of reading past the end",
          "[LiveEdgeMargin]")
{
  std::vector<FakeSegment> segments = {{0, 100, 0}, {100, 100, 2000}, {200, 100, 4000}};
  int64_t totalBytes = 300;
  int64_t totalDurationMs = 6000;
  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 0);
  CHECK(segments.empty());
  CHECK(totalBytes == 0);
  CHECK(totalDurationMs == 0);
  // And an empty list with a margin of nothing is still the no-op it was.
  TrimToTrailingLiveEdgeMargin(segments, totalBytes, totalDurationMs, 0);
  CHECK(segments.empty());
}
