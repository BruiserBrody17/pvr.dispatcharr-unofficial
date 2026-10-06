#include "SegmentFetchFailure.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr std::chrono::milliseconds kLong{600000};
constexpr std::chrono::milliseconds kNoMinimum{0};
} // namespace

TEST_CASE("ShouldGiveUpAfterSegmentFetchFailure gives up immediately when the manifest refresh confirms fatal",
          "[SegmentFetchFailure]")
{
  // The case actually reproduced live: get_live_manifest's own
  // _get_buffer_state() returned None, so the plugin already positively
  // confirmed this buffer is gone -- no reason to wait for more attempts.
  CHECK(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/1, /*refreshFatal=*/true, /*maxAttempts=*/4, kLong,
                                             kNoMinimum));
}

TEST_CASE("ShouldGiveUpAfterSegmentFetchFailure does not give up on an early, unconfirmed failure",
          "[SegmentFetchFailure]")
{
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/1, /*refreshFatal=*/false, /*maxAttempts=*/4,
                                                   kLong, kNoMinimum));
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/3, /*refreshFatal=*/false, /*maxAttempts=*/4,
                                                   kLong, kNoMinimum));
}

TEST_CASE("ShouldGiveUpAfterSegmentFetchFailure gives up once the unconfirmed failure count reaches maxAttempts",
          "[SegmentFetchFailure]")
{
  CHECK(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/4, /*refreshFatal=*/false, /*maxAttempts=*/4,
                                             kLong, kNoMinimum));
  CHECK(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/5, /*refreshFatal=*/false, /*maxAttempts=*/4,
                                             kLong, kNoMinimum));
}

TEST_CASE("ShouldGiveUpAfterSegmentFetchFailure does not give up one attempt short of maxAttempts",
          "[SegmentFetchFailure]")
{
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(/*consecutiveFailures=*/3, /*refreshFatal=*/false, /*maxAttempts=*/4,
                                                   kLong, kNoMinimum));
}

TEST_CASE("A streak of refused connections is not dead until it has lasted the minimum", "[SegmentFetchFailure]")
{
  using std::chrono::milliseconds;
  // four failures within about two seconds, the shape of a server that is briefly down
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(4, false, 4, milliseconds(1800), milliseconds(30000)));
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(12, false, 4, milliseconds(29999), milliseconds(30000)));
  CHECK(ShouldGiveUpAfterSegmentFetchFailure(12, false, 4, milliseconds(30000), milliseconds(30000)));
}

TEST_CASE("The duration minimum never delays a confirmed-fatal answer", "[SegmentFetchFailure]")
{
  using std::chrono::milliseconds;
  CHECK(ShouldGiveUpAfterSegmentFetchFailure(1, true, 4, milliseconds(0), milliseconds(30000)));
}

TEST_CASE("The duration alone does not give up before the attempt count", "[SegmentFetchFailure]")
{
  using std::chrono::milliseconds;
  CHECK_FALSE(ShouldGiveUpAfterSegmentFetchFailure(2, false, 4, milliseconds(90000), milliseconds(30000)));
}

TEST_CASE("SegmentFetchRetryDelayMs doubles from 250 ms and stops at 2 s", "[SegmentFetchFailure]")
{
  CHECK(SegmentFetchRetryDelayMs(1) == 250);
  CHECK(SegmentFetchRetryDelayMs(2) == 500);
  CHECK(SegmentFetchRetryDelayMs(3) == 1000);
  CHECK(SegmentFetchRetryDelayMs(4) == 2000);
  CHECK(SegmentFetchRetryDelayMs(5) == 2000);
  CHECK(SegmentFetchRetryDelayMs(1000) == 2000);
  CHECK(SegmentFetchRetryDelayMs(0) == 250);
}

TEST_CASE("A transport failure and a gateway error are transient reads, anything else is not", "[SegmentFetchFailure]")
{
  CHECK(IsTransientReadFailure(false, 0));
  CHECK(IsTransientReadFailure(false, 206));
  CHECK(IsTransientReadFailure(true, 500));
  CHECK(IsTransientReadFailure(true, 502));
  CHECK(IsTransientReadFailure(true, 503));
  CHECK(IsTransientReadFailure(true, 504));
  CHECK_FALSE(IsTransientReadFailure(true, 200));
  CHECK_FALSE(IsTransientReadFailure(true, 206));
  CHECK_FALSE(IsTransientReadFailure(true, 401));
  CHECK_FALSE(IsTransientReadFailure(true, 404));
  CHECK_FALSE(IsTransientReadFailure(true, 416));
  CHECK_FALSE(IsTransientReadFailure(true, 501));
  CHECK_FALSE(IsTransientReadFailure(true, 505));
  CHECK_FALSE(IsTransientReadFailure(true, 403));
}

TEST_CASE("A transient read keeps retrying until the budget is spent", "[SegmentFetchFailure]")
{
  using std::chrono::milliseconds;
  CHECK(ShouldKeepRetryingTransientRead(milliseconds(0), milliseconds(20000)));
  CHECK(ShouldKeepRetryingTransientRead(milliseconds(19999), milliseconds(20000)));
  CHECK_FALSE(ShouldKeepRetryingTransientRead(milliseconds(20000), milliseconds(20000)));
}

// ---------------------------------------------------------------------
// PermanentReadFailureTracker (the fourteenth hardening sweep)
// ---------------------------------------------------------------------

TEST_CASE("a run of read failures that will not clear ends the stream only once it has lasted long enough",
          "[SegmentFetchFailure]")
{
  using namespace std::chrono;
  const steady_clock::time_point t0{steady_clock::duration(1000000000)};
  PermanentReadFailureTracker tracker;
  // Plenty of failures in a burst is not enough: a blip of five failed reads in a few hundred milliseconds
  // is what a restarting proxy looks like.
  for (int i = 0; i < 8; ++i)
    CHECK_FALSE(RecordPermanentReadFailure(tracker, t0 + milliseconds(i * 100)));
  CHECK_FALSE(tracker.ended);
  // Enough span with at least the minimum count: ended, from the failure that completes both conditions.
  CHECK(RecordPermanentReadFailure(tracker, t0 + kPermanentReadFailureMinSpan));
  CHECK(tracker.ended);
  // ...and it stays ended.
  CHECK(RecordPermanentReadFailure(tracker, t0 + seconds(60)));
}

TEST_CASE("the permanent-failure run needs both its count and its span, at exactly the boundaries",
          "[SegmentFetchFailure]")
{
  using namespace std::chrono;
  const steady_clock::time_point t0{steady_clock::duration(1000000000)};
  {
    // Too few failures however long they last.
    PermanentReadFailureTracker tracker;
    for (int i = 0; i < kPermanentReadFailureMinCount - 1; ++i)
      CHECK_FALSE(RecordPermanentReadFailure(tracker, t0 + seconds(i * 10)));
  }
  {
    // The minimum count, one millisecond short of the span.
    PermanentReadFailureTracker tracker;
    for (int i = 0; i < kPermanentReadFailureMinCount - 1; ++i)
      RecordPermanentReadFailure(tracker, t0);
    CHECK_FALSE(RecordPermanentReadFailure(tracker, t0 + kPermanentReadFailureMinSpan - milliseconds(1)));
  }
  {
    // The minimum count, exactly the span.
    PermanentReadFailureTracker tracker;
    for (int i = 0; i < kPermanentReadFailureMinCount - 1; ++i)
      RecordPermanentReadFailure(tracker, t0);
    CHECK(RecordPermanentReadFailure(tracker, t0 + kPermanentReadFailureMinSpan));
  }
}

TEST_CASE("a read that delivered bytes ends the failure run, but never un-ends an ended stream",
          "[SegmentFetchFailure]")
{
  using namespace std::chrono;
  const steady_clock::time_point t0{steady_clock::duration(1000000000)};
  PermanentReadFailureTracker tracker;
  for (int i = 0; i < 4; ++i)
    RecordPermanentReadFailure(tracker, t0 + milliseconds(i));
  ResetPermanentReadFailures(tracker);
  CHECK(tracker.count == 0);
  // The span restarts too: the next failures are measured from their own first.
  CHECK_FALSE(RecordPermanentReadFailure(tracker, t0 + seconds(10)));
  CHECK(tracker.firstAt == t0 + seconds(10));

  PermanentReadFailureTracker ended;
  for (int i = 0; i < kPermanentReadFailureMinCount; ++i)
    RecordPermanentReadFailure(ended, t0 + kPermanentReadFailureMinSpan * (i + 1));
  REQUIRE(ended.ended);
  ResetPermanentReadFailures(ended);
  CHECK(ended.ended);
}
