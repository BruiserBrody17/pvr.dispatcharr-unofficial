#include "InProgressSegmentCache.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ShouldReseedInProgressSegmentCache is false when the cache entry is exactly the stream's own prefix",
          "[InProgressSegmentCache]")
{
  // The ordinary case: cache already has 10 segments, this cycle probed 2
  // more, and the stream's current total is 12 -- append is safe.
  CHECK_FALSE(ShouldReseedInProgressSegmentCache(/*cachedSegmentCount=*/10, /*newlyProbedSegmentCount=*/2,
                                                 /*currentSegmentCount=*/12));
}

TEST_CASE("ShouldReseedInProgressSegmentCache is false on a fresh cache entry with nothing probed yet",
          "[InProgressSegmentCache]")
{
  CHECK_FALSE(ShouldReseedInProgressSegmentCache(0, 0, 0));
}

TEST_CASE("ShouldReseedInProgressSegmentCache is true right after a finished->false reset left the entry empty",
          "[InProgressSegmentCache]")
{
  // The exact real bug this fixes: `finished` briefly went true (erasing
  // the cache entry), then flipped back to false. The stream itself still
  // has all 12 of its own segments, but the freshly-recreated cache entry
  // has 0 -- appending just this cycle's 2 newly-probed segments onto it
  // would silently drop the other 10.
  CHECK(ShouldReseedInProgressSegmentCache(/*cachedSegmentCount=*/0, /*newlyProbedSegmentCount=*/2,
                                           /*currentSegmentCount=*/12));
}

TEST_CASE("ShouldReseedInProgressSegmentCache is true when nothing was newly probed but counts still disagree",
          "[InProgressSegmentCache]")
{
  CHECK(ShouldReseedInProgressSegmentCache(/*cachedSegmentCount=*/5, /*newlyProbedSegmentCount=*/0,
                                           /*currentSegmentCount=*/12));
}

TEST_CASE("ShouldReseedInProgressSegmentCache is true when the cache holds more segments than the current playlist",
          "[InProgressSegmentCache]")
{
  // The playlist shrank (a rolled-off or rewritten manifest): appending would keep segments the server no
  // longer lists, so the entry has to be rebuilt from the full current list.
  CHECK(ShouldReseedInProgressSegmentCache(10, 2, 11));
  CHECK(ShouldReseedInProgressSegmentCache(12, 0, 11));
}
