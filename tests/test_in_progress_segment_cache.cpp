#include "InProgressSegmentCache.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

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

namespace
{
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds kMaxAge{500};

bool CanReuse(Clock::time_point snapshot, Clock::time_point lastApplied, Clock::time_point now)
{
  return CanReuseSharedInProgressManifest(snapshot, lastApplied, now, kMaxAge);
}

struct CachedSeg
{
  std::string url;
  int64_t byteSize = 0;
};

struct PendingSeg
{
  std::string url;
};
} // namespace

TEST_CASE("CanReuseSharedInProgressManifest takes another stream's fetch while it is younger than the limit",
          "[InProgressSegmentCache]")
{
  const Clock::time_point lastApplied = Clock::time_point{} + std::chrono::hours(1);
  const auto snapshot = lastApplied + milliseconds(100);
  CHECK(CanReuse(snapshot, lastApplied, snapshot + milliseconds(0)));
  CHECK(CanReuse(snapshot, lastApplied, snapshot + milliseconds(499)));
  CHECK_FALSE(CanReuse(snapshot, lastApplied, snapshot + milliseconds(500)));
  CHECK_FALSE(CanReuse(snapshot, lastApplied, snapshot + milliseconds(2000)));
}

TEST_CASE("CanReuseSharedInProgressManifest never hands a stream a fetch it has already used",
          "[InProgressSegmentCache]")
{
  // A lone stream finds only its own snapshot, which it applied: it must fetch, exactly as before the sharing, or it
  // would poll half as often as it asks to. The same holds for another stream's snapshot it has already taken.
  const auto own = Clock::time_point{} + std::chrono::hours(1);
  CHECK_FALSE(CanReuse(own, own, own + milliseconds(10)));
  // Older than what this stream last used (it fetched since): not new information either.
  CHECK_FALSE(CanReuse(own - milliseconds(50), own, own + milliseconds(10)));
}

TEST_CASE("CanReuseSharedInProgressManifest lets a stream that has never refreshed take a fresh fetch",
          "[InProgressSegmentCache]")
{
  // A second stream's cold start: nothing applied yet (the zero time point).
  const auto snapshot = Clock::time_point{} + std::chrono::hours(1);
  CHECK(CanReuse(snapshot, Clock::time_point{}, snapshot + milliseconds(50)));
}

TEST_CASE("SegmentSizesKnownToCache takes the sizes another stream has already probed", "[InProgressSegmentCache]")
{
  const std::vector<CachedSeg> cached{{"s0", 100}, {"s1", 200}, {"s2", 300}, {"s3", 400}};
  // This stream holds 2 segments; the playlist lists s2..s4, the cache knows s2 and s3.
  const std::vector<PendingSeg> pending{{"s2"}, {"s3"}, {"s4"}};
  CHECK(SegmentSizesKnownToCache(cached, pending, 2) == std::vector<int64_t>{300, 400, -1});
}

TEST_CASE("SegmentSizesKnownToCache knows nothing past the cache or for a different URL", "[InProgressSegmentCache]")
{
  const std::vector<CachedSeg> cached{{"s0", 100}, {"s1", 200}};
  CHECK(SegmentSizesKnownToCache(cached, std::vector<PendingSeg>{{"s2"}, {"s3"}}, 2) == std::vector<int64_t>{-1, -1});
  // Same index, different segment: never borrow a size for it.
  CHECK(SegmentSizesKnownToCache(cached, std::vector<PendingSeg>{{"other"}, {"s1"}}, 0) ==
        std::vector<int64_t>{-1, 200});
  CHECK(SegmentSizesKnownToCache(std::vector<CachedSeg>{}, std::vector<PendingSeg>{{"s0"}}, 0) ==
        std::vector<int64_t>{-1});
  CHECK(SegmentSizesKnownToCache(cached, std::vector<PendingSeg>{}, 0).empty());
}

TEST_CASE("SegmentSizesKnownToCache does not inherit another stream's zero-byte placeholder",
          "[InProgressSegmentCache]")
{
  // The other stream gave up on s1 and merged it as empty; this stream probes s1 itself.
  const std::vector<CachedSeg> cached{{"s0", 100}, {"s1", 0}, {"s2", 300}};
  CHECK(SegmentSizesKnownToCache(cached, std::vector<PendingSeg>{{"s1"}, {"s2"}}, 1) == std::vector<int64_t>{-1, 300});
}

TEST_CASE("ShouldPruneInProgressSegmentCacheEntry keeps a finished recording's entry for a while after its last use",
          "[InProgressSegmentCache]")
{
  // Kodi 22's stream-details probe opens a just-finished recording once more; erasing the entry the moment the
  // recording finished made that open size every segment again.
  const auto touched = Clock::time_point{} + std::chrono::hours(1);
  CHECK_FALSE(ShouldPruneInProgressSegmentCacheEntry(touched, /*finished=*/true, touched));
  CHECK_FALSE(ShouldPruneInProgressSegmentCacheEntry(touched, true, touched + std::chrono::minutes(14)));
  CHECK_FALSE(ShouldPruneInProgressSegmentCacheEntry(touched, true, touched + std::chrono::minutes(15)));
  CHECK(ShouldPruneInProgressSegmentCacheEntry(touched, true, touched + std::chrono::minutes(15) + milliseconds(1)));
}

TEST_CASE("ShouldPruneInProgressSegmentCacheEntry keeps a still-recording entry for a day", "[InProgressSegmentCache]")
{
  const auto touched = Clock::time_point{} + std::chrono::hours(1);
  CHECK_FALSE(ShouldPruneInProgressSegmentCacheEntry(touched, /*finished=*/false, touched + std::chrono::hours(2)));
  CHECK_FALSE(ShouldPruneInProgressSegmentCacheEntry(touched, false, touched + std::chrono::hours(24)));
  CHECK(ShouldPruneInProgressSegmentCacheEntry(touched, false, touched + std::chrono::hours(24) + milliseconds(1)));
}
