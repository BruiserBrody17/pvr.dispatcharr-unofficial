#include "InProgressSegmentCache.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
SegmentCachePlan Plan(size_t cached, size_t stream, size_t newlyProbed)
{
  return PlanInProgressSegmentCacheUpdate(cached, stream, newlyProbed);
}
} // namespace

TEST_CASE("PlanInProgressSegmentCacheUpdate appends the newly probed segments on an ordinary refresh",
          "[InProgressSegmentCache]")
{
  // The cache already has 10 segments, this cycle probed 2 more, and the stream now holds 12.
  const auto plan = Plan(/*cached=*/10, /*stream=*/12, /*newlyProbed=*/2);
  CHECK(plan.action == SegmentCacheAction::kAppend);
  CHECK(plan.appendFrom == 10);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate does nothing on a fresh entry with nothing probed",
          "[InProgressSegmentCache]")
{
  CHECK(Plan(0, 0, 0).action == SegmentCacheAction::kNone);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate reseeds right after a finished->false reset left the entry empty",
          "[InProgressSegmentCache]")
{
  // The exact real bug the reseed exists for: `finished` briefly went true (erasing the entry), then flipped back.
  // The stream still has all 12 of its segments but the recreated entry has 0, so appending just this cycle's 2
  // would silently drop the other 10.
  const auto plan = Plan(/*cached=*/0, /*stream=*/12, /*newlyProbed=*/2);
  CHECK(plan.action == SegmentCacheAction::kReseed);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate reseeds when nothing was newly probed but the entry is short",
          "[InProgressSegmentCache]")
{
  CHECK(Plan(/*cached=*/5, /*stream=*/12, /*newlyProbed=*/0).action == SegmentCacheAction::kReseed);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate never shrinks an entry another stream has taken further",
          "[InProgressSegmentCache]")
{
  // Two streams of one recording: the one ahead has put 12 segments in the entry, the one behind has 10 and
  // probes 1 more. Cutting the entry back to 11 would throw away a segment already sized.
  CHECK(Plan(/*cached=*/12, /*stream=*/11, /*newlyProbed=*/1).action == SegmentCacheAction::kNone);
  CHECK(Plan(/*cached=*/12, /*stream=*/12, /*newlyProbed=*/2).action == SegmentCacheAction::kNone);
  CHECK(Plan(/*cached=*/12, /*stream=*/3, /*newlyProbed=*/0).action == SegmentCacheAction::kNone);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate appends only what the entry lacks when another stream is partly ahead",
          "[InProgressSegmentCache]")
{
  // The entry holds 11 (another stream got there first); this stream had 9, probed 4 and now holds 13. Only the
  // two the entry lacks are appended.
  const auto plan = Plan(/*cached=*/11, /*stream=*/13, /*newlyProbed=*/4);
  CHECK(plan.action == SegmentCacheAction::kAppend);
  CHECK(plan.appendFrom == 11);
}

TEST_CASE("PlanInProgressSegmentCacheUpdate does not underflow when more was probed than the stream holds",
          "[InProgressSegmentCache]")
{
  const auto plan = Plan(/*cached=*/0, /*stream=*/2, /*newlyProbed=*/5);
  // Cannot happen, but the subtraction must not wrap: with a short entry the answer is to take everything.
  CHECK(plan.action != SegmentCacheAction::kNone);
  CHECK(plan.appendFrom == 0);
}
