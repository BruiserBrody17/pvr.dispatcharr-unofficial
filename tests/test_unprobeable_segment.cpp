#include "UnprobeableSegment.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;
using Clock = std::chrono::steady_clock;

namespace
{
const Clock::time_point kT0 = Clock::time_point(std::chrono::hours(1));
Clock::time_point At(int seconds)
{
  return kT0 + std::chrono::seconds(seconds);
}
} // namespace

TEST_CASE("ShouldProbeOnlyLeadingSegment is true only for the URL that failed last time", "[UnprobeableSegment]")
{
  UnprobeableSegmentTracker t;
  CHECK_FALSE(ShouldProbeOnlyLeadingSegment(t, "seg_1.ts"));

  UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(0));
  CHECK(ShouldProbeOnlyLeadingSegment(t, "seg_1.ts"));
  CHECK_FALSE(ShouldProbeOnlyLeadingSegment(t, "seg_2.ts"));
  CHECK_FALSE(ShouldProbeOnlyLeadingSegment(t, ""));
}

TEST_CASE("a leading segment that answers, or none pending, resets the tracker", "[UnprobeableSegment]")
{
  UnprobeableSegmentTracker t;
  UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(0));
  UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(1));
  REQUIRE(t.consecutiveFailures == 2);

  CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", false, At(2)) == UnprobeableSegmentAction::kKeepRetrying);
  CHECK(t.consecutiveFailures == 0);
  CHECK(t.url.empty());

  UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(3));
  CHECK(UpdateUnprobeableSegmentTracker(t, "", true, At(4)) == UnprobeableSegmentAction::kKeepRetrying);
  CHECK(t.consecutiveFailures == 0);
}

TEST_CASE("it gives up only once both the failure count and the wait are met", "[UnprobeableSegment]")
{
  SECTION("enough failures too soon keeps retrying")
  {
    UnprobeableSegmentTracker t;
    for (int i = 0; i < 20; ++i)
      CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(i)) == UnprobeableSegmentAction::kKeepRetrying);
  }

  SECTION("enough time with too few failures keeps retrying")
  {
    UnprobeableSegmentTracker t;
    CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(0)) == UnprobeableSegmentAction::kKeepRetrying);
    CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(100)) == UnprobeableSegmentAction::kKeepRetrying);
  }

  SECTION("both met gives up, exactly at the boundary, and resets")
  {
    UnprobeableSegmentTracker t;
    for (int i = 0; i < kUnprobeableSegmentMinFailures - 1; ++i)
      CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(i)) == UnprobeableSegmentAction::kKeepRetrying);
    CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(29)) == UnprobeableSegmentAction::kKeepRetrying);
    CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(30)) == UnprobeableSegmentAction::kGiveUp);
    CHECK(t.url.empty());
    CHECK(t.consecutiveFailures == 0);
  }
}

TEST_CASE("a different leading segment restarts the count and the clock", "[UnprobeableSegment]")
{
  UnprobeableSegmentTracker t;
  for (int i = 0; i < 10; ++i)
    UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(i));
  REQUIRE(t.consecutiveFailures == 10);

  CHECK(UpdateUnprobeableSegmentTracker(t, "seg_2.ts", true, At(40)) == UnprobeableSegmentAction::kKeepRetrying);
  CHECK(t.url == "seg_2.ts");
  CHECK(t.consecutiveFailures == 1);
  CHECK(t.firstFailedAt == At(40));
}

TEST_CASE("CountMergeableSegments matches CountLeadingProbedSegments unless the leader is given up on",
          "[UnprobeableSegment]")
{
  CHECK(CountMergeableSegments({}, false) == 0);
  CHECK(CountMergeableSegments({}, true) == 0);
  CHECK(CountMergeableSegments({100, 200, -1, 300}, false) == 2);
  CHECK(CountMergeableSegments({-1, 200, 300}, false) == 0);

  // Given up on: the failed leader counts, and the rest is judged as usual.
  CHECK(CountMergeableSegments({-1, 200, 300}, true) == 3);
  CHECK(CountMergeableSegments({0, 200, -1, 300}, true) == 2);
  CHECK(CountMergeableSegments({-1, -1, -1}, true) == 1);
  CHECK(CountMergeableSegments({-1}, true) == 1);
}

// ---------------------------------------------------------------------
// Exact boundaries (the 2026-10-04 fifth hardening sweep's mutation survivors)
// ---------------------------------------------------------------------

TEST_CASE("it gives up on the fifth failure at exactly the minimum wait, not one second sooner", "[UnprobeableSegment]")
{
  UnprobeableSegmentTracker t;
  for (int i = 0; i < 4; ++i)
    CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(i)) == UnprobeableSegmentAction::kKeepRetrying);
  // The fifth failure, 29 s after the first: enough failures, not enough wait.
  CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(29)) == UnprobeableSegmentAction::kKeepRetrying);
  // The sixth, at exactly 30 s: both met.
  CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(30)) == UnprobeableSegmentAction::kGiveUp);
}

TEST_CASE("the fifth failure itself gives up when it lands at exactly the minimum wait", "[UnprobeableSegment]")
{
  UnprobeableSegmentTracker t;
  UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(0));
  for (int i = 1; i < 4; ++i)
    UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(i));
  CHECK(t.consecutiveFailures == 4);
  CHECK(UpdateUnprobeableSegmentTracker(t, "seg_1.ts", true, At(30)) == UnprobeableSegmentAction::kGiveUp);
}

TEST_CASE("CountMergeableSegments after a give-up merges the placeholder and the probed ones, stopping at a zero",
          "[UnprobeableSegment]")
{
  // The leading entry is merged as a placeholder whatever its size; the next entry is merged only
  // while it has a size (a zero is another unprobeable one, which waits its own turn).
  CHECK(CountMergeableSegments({0, 0, 1000}, /*leadingGivenUp=*/true) == 1);
  CHECK(CountMergeableSegments({0, 5, 0, 7}, true) == 2);
  CHECK(CountMergeableSegments({0, 5, 6}, true) == 3);
  CHECK(CountMergeableSegments({0}, true) == 1);
  // Without a give-up the leading zero stops everything.
  CHECK(CountMergeableSegments({0, 5, 6}, false) == 0);
}

TEST_CASE("IndexToArmTrackerAfterMerge names the first unsized segment behind sized ones", "[UnprobeableSegment]")
{
  // The leading segment answered and one in the middle did not: the next refresh's leading segment is that one,
  // and the tracker is armed for it so the whole tail is not probed again (6249 HEADs in 40 s for 2000 segments).
  CHECK(IndexToArmTrackerAfterMerge({100, 100, 100, -1, 100, 100}, 3, false, 6) == 3);
  CHECK(IndexToArmTrackerAfterMerge({100, 0, 100}, 1, false, 3) == 1); // a zero-size answer is not a size either
  CHECK(IndexToArmTrackerAfterMerge({100, 100, -1, 100}, 2, false, 4) == 2);
  // Nothing to arm: everything merged, the leading segment itself failed (the ordinary tracker covers it), or
  // the leading segment was given up on (it was merged as a placeholder).
  CHECK(IndexToArmTrackerAfterMerge({100, 100, 100}, 3, false, 3) == 3);
  CHECK(IndexToArmTrackerAfterMerge({-1, 100, 100}, 0, false, 3) == 3);
  CHECK(IndexToArmTrackerAfterMerge({0, 100, -1}, 2, true, 3) == 3);
  CHECK(IndexToArmTrackerAfterMerge({0, 100, -1, 100}, 2, true, 4) == 4); // behind a placeholder: no arm
  CHECK(IndexToArmTrackerAfterMerge({}, 0, false, 0) == 0);
}

TEST_CASE("IndexToArmTrackerAfterMerge never reads an unprobed entry as a failure", "[UnprobeableSegment]")
{
  // A refresh that probes only the leading segment (it failed last time) leaves every other entry at -1; when the
  // leading one now answers, the next segment must NOT be armed -- that cascaded into one segment per refresh.
  CHECK(IndexToArmTrackerAfterMerge({100, -1, -1}, 1, false, 1) == 3);
  CHECK(IndexToArmTrackerAfterMerge({100, -1, -1, -1}, 1, false, 1) == 4);
  // The same shape with every entry probed is a real failure and does arm.
  CHECK(IndexToArmTrackerAfterMerge({100, -1, -1}, 1, false, 3) == 1);
  // The boundary: the last probed entry itself is the one that failed.
  CHECK(IndexToArmTrackerAfterMerge({100, 100, -1, -1}, 2, false, 3) == 2);
  CHECK(IndexToArmTrackerAfterMerge({100, 100, -1, -1}, 2, false, 2) == 4);
}
