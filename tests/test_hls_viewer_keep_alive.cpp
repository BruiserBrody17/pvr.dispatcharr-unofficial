#include "HlsViewerKeepAlive.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace dispatcharr;
using Clock = std::chrono::steady_clock;

namespace
{
// A fixed, non-zero origin: a default-constructed time_point is this
// codebase's own "never happened yet" sentinel (see Staleness.h), so a
// test standing in for "some real moment" must not accidentally be it.
const Clock::time_point kNow = Clock::time_point(std::chrono::hours(100));
} // namespace

TEST_CASE("the keep-alive schedule stays inside the server's own viewer-key TTL", "[HlsViewerKeepAlive]")
{
  // The whole point of the constants -- Dispatcharr's viewer key lapses
  // kDispatcharrHlsViewerTtl after its last refresh, so a keep-alive
  // interval at or beyond it would let the directory go before the next
  // one fires, and one at more than half of it leaves no room to retry a
  // single failed attempt. (Also static_assert'd in the header; this keeps
  // the relationship visible in the test report.)
  CHECK(kHlsKeepAliveInterval * 2 <= kDispatcharrHlsViewerTtl);
  CHECK(kHlsKeepAliveRetryDelay < kHlsKeepAliveInterval);
  CHECK(kDispatcharrHlsViewerTtl == std::chrono::seconds(20));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive sends one for a paused viewer with unread bytes once due",
          "[HlsViewerKeepAlive]")
{
  CHECK(ShouldSendHlsViewerKeepAlive(/*contentGone=*/false, /*position=*/1000, /*totalBytes=*/5000,
                                     /*nextDueAt=*/kNow - std::chrono::seconds(1), kNow));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive waits until the scheduled time", "[HlsViewerKeepAlive]")
{
  const auto due = kNow + std::chrono::seconds(4);
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(false, 1000, 5000, due, kNow));
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(false, 1000, 5000, due, due - std::chrono::milliseconds(1)));
  // Exactly on the boundary counts as due -- a poll landing on it should
  // not slip a whole further poll interval.
  CHECK(ShouldSendHlsViewerKeepAlive(false, 1000, 5000, due, due));
  CHECK(ShouldSendHlsViewerKeepAlive(false, 1000, 5000, due, due + std::chrono::milliseconds(1)));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive treats a never-scheduled keep-alive as due", "[HlsViewerKeepAlive]")
{
  CHECK(ShouldSendHlsViewerKeepAlive(false, 0, 5000, Clock::time_point{}, kNow));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive stays quiet once the reader has caught up to the tail", "[HlsViewerKeepAlive]")
{
  // The load-bearing rule: a reader with nothing unread must NOT hold the
  // directory open, or Dispatcharr never finalizes the recording and the
  // reader never sees the EOF that finalization is what signals.
  const auto overdue = kNow - std::chrono::seconds(30);
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(false, /*position=*/5000, /*totalBytes=*/5000, overdue, kNow));
  // One byte still unread is enough to need it.
  CHECK(ShouldSendHlsViewerKeepAlive(false, /*position=*/4999, /*totalBytes=*/5000, overdue, kNow));
  // A position past the known total (a seek/probe overshoot) is also "caught up".
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(false, /*position=*/6000, /*totalBytes=*/5000, overdue, kNow));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive stays quiet when there are no bytes known at all", "[HlsViewerKeepAlive]")
{
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(false, 0, 0, Clock::time_point{}, kNow));
}

TEST_CASE("ShouldSendHlsViewerKeepAlive stays quiet once the content is known to be gone", "[HlsViewerKeepAlive]")
{
  // contentGone covers a finished recording and a keep-alive that already
  // came back 404 -- there is nothing left to keep alive either way, even
  // with unread bytes and an overdue schedule.
  CHECK_FALSE(ShouldSendHlsViewerKeepAlive(/*contentGone=*/true, 1000, 5000, kNow - std::chrono::seconds(30), kNow));
}

TEST_CASE("ClassifyHlsKeepAliveResponse treats any 2xx as refreshed", "[HlsViewerKeepAlive]")
{
  CHECK(ClassifyHlsKeepAliveResponse(true, 200) == HlsKeepAliveOutcome::kRefreshed);
  CHECK(ClassifyHlsKeepAliveResponse(true, 206) == HlsKeepAliveOutcome::kRefreshed);
  CHECK(ClassifyHlsKeepAliveResponse(true, 299) == HlsKeepAliveOutcome::kRefreshed);
}

TEST_CASE("ClassifyHlsKeepAliveResponse treats 404 and redirects as the directory being gone", "[HlsViewerKeepAlive]")
{
  // The same "directory is gone" signal a playlist request gets --
  // FetchRawInProgressPlaylist() reads a 404 or any 3xx that way.
  CHECK(ClassifyHlsKeepAliveResponse(true, 404) == HlsKeepAliveOutcome::kGone);
  CHECK(ClassifyHlsKeepAliveResponse(true, 301) == HlsKeepAliveOutcome::kGone);
  CHECK(ClassifyHlsKeepAliveResponse(true, 302) == HlsKeepAliveOutcome::kGone);
  CHECK(ClassifyHlsKeepAliveResponse(true, 399) == HlsKeepAliveOutcome::kGone);
}

TEST_CASE("ClassifyHlsKeepAliveResponse retries anything that doesn't say the content is gone", "[HlsViewerKeepAlive]")
{
  // 401: another client regenerated the account's key -- the manifest
  // refresh's own self-heal fixes that, so this is worth retrying, not
  // abandoning. 403/5xx/other 4xx likewise say nothing about the directory.
  for (long status : {400L, 401L, 403L, 408L, 429L, 500L, 502L, 503L, 504L})
    CHECK(ClassifyHlsKeepAliveResponse(true, status) == HlsKeepAliveOutcome::kRetryLater);
}

TEST_CASE("ClassifyHlsKeepAliveResponse retries a transport failure regardless of the status field",
          "[HlsViewerKeepAlive]")
{
  // curl reports a status of 0 when nothing came back at all, but a caller
  // could also hand over a stale non-zero one -- transportOk decides.
  CHECK(ClassifyHlsKeepAliveResponse(false, 0) == HlsKeepAliveOutcome::kRetryLater);
  CHECK(ClassifyHlsKeepAliveResponse(false, 200) == HlsKeepAliveOutcome::kRetryLater);
  CHECK(ClassifyHlsKeepAliveResponse(false, 404) == HlsKeepAliveOutcome::kRetryLater);
}

TEST_CASE("NextHlsKeepAliveDueAt schedules the full interval after a success", "[HlsViewerKeepAlive]")
{
  CHECK(NextHlsKeepAliveDueAt(kNow, HlsKeepAliveOutcome::kRefreshed) == kNow + kHlsKeepAliveInterval);
}

TEST_CASE("NextHlsKeepAliveDueAt schedules the short retry delay after a transient failure", "[HlsViewerKeepAlive]")
{
  CHECK(NextHlsKeepAliveDueAt(kNow, HlsKeepAliveOutcome::kRetryLater) == kNow + kHlsKeepAliveRetryDelay);
}

TEST_CASE("NextHlsKeepAliveDueAt uses the full interval for a gone directory", "[HlsViewerKeepAlive]")
{
  // Moot in practice (the caller stops asking once it has seen kGone), but
  // it must never come back sooner than a normal success would.
  CHECK(NextHlsKeepAliveDueAt(kNow, HlsKeepAliveOutcome::kGone) == kNow + kHlsKeepAliveInterval);
}

TEST_CASE("a paused viewer's keep-alive cadence over a simulated pause", "[HlsViewerKeepAlive]")
{
  // Walks the exact case this exists for: a viewer paused with unread
  // bytes, polled once a second, the last real `.ts` request just made.
  // Every request must land inside the server's TTL of the previous one,
  // for a pause far longer than the TTL itself.
  Clock::time_point lastServerRefresh = kNow;
  Clock::time_point nextDue = NextHlsKeepAliveDueAt(kNow, HlsKeepAliveOutcome::kRefreshed);
  int sent = 0;
  for (int second = 1; second <= 300; ++second)
  {
    const auto now = kNow + std::chrono::seconds(second);
    // The server's key would have lapsed before this poll if nothing had
    // refreshed it recently enough.
    REQUIRE(now - lastServerRefresh <= kDispatcharrHlsViewerTtl);
    if (ShouldSendHlsViewerKeepAlive(false, /*position=*/100, /*totalBytes=*/5000, nextDue, now))
    {
      ++sent;
      lastServerRefresh = now;
      nextDue = NextHlsKeepAliveDueAt(now, HlsKeepAliveOutcome::kRefreshed);
    }
  }
  // One every kHlsKeepAliveInterval, not one per poll.
  CHECK(sent == 300 / kHlsKeepAliveInterval.count());
}

TEST_CASE("a failed attempt is retried well before the server's TTL would lapse", "[HlsViewerKeepAlive]")
{
  // The scenario the retry delay exists for: the attempt that was due at
  // +interval fails transiently. The retry must still land inside the TTL
  // measured from the last *successful* refresh, or the directory is
  // already gone by the time it arrives.
  const Clock::time_point lastServerRefresh = kNow;
  Clock::time_point nextDue = NextHlsKeepAliveDueAt(kNow, HlsKeepAliveOutcome::kRefreshed);
  bool firstAttemptFailed = false;
  for (int second = 1; second <= 20; ++second)
  {
    const auto now = kNow + std::chrono::seconds(second);
    if (!ShouldSendHlsViewerKeepAlive(false, 100, 5000, nextDue, now))
      continue;
    if (!firstAttemptFailed)
    {
      firstAttemptFailed = true;
      CHECK(now == kNow + kHlsKeepAliveInterval);
      nextDue = NextHlsKeepAliveDueAt(now, HlsKeepAliveOutcome::kRetryLater);
      continue;
    }
    CHECK(now == kNow + kHlsKeepAliveInterval + kHlsKeepAliveRetryDelay);
    CHECK(now - lastServerRefresh < kDispatcharrHlsViewerTtl);
    return;
  }
  FAIL("the failed attempt was never retried inside the server's TTL");
}

TEST_CASE("ClassifyHlsKeepAliveResponse maps a redirect with a transport success to gone", "[HlsViewerKeepAlive]")
{
  CHECK(ClassifyHlsKeepAliveResponse(true, 300) == HlsKeepAliveOutcome::kGone);
  CHECK(ClassifyHlsKeepAliveResponse(true, 399) == HlsKeepAliveOutcome::kGone);
  CHECK(ClassifyHlsKeepAliveResponse(true, 299) == HlsKeepAliveOutcome::kRefreshed);
  CHECK(ClassifyHlsKeepAliveResponse(true, 400) == HlsKeepAliveOutcome::kRetryLater);
  CHECK(ClassifyHlsKeepAliveResponse(false, 404) == HlsKeepAliveOutcome::kRetryLater);
}
