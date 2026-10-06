#include "PaddingPush.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace dispatcharr;

namespace
{
constexpr int kMaxRetries = 3;
} // namespace

TEST_CASE("edits are numbered in the order they are made, and the newest is remembered", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t a = BeginPaddingEdit(s, true, 5);
  const uint64_t b = BeginPaddingEdit(s, true, 7);
  const uint64_t c = BeginPaddingEdit(s, false, 9);
  CHECK(a < b);
  CHECK(b < c);
  REQUIRE(s.preNewest.has_value());
  CHECK(s.preNewest->value == 7);
  CHECK(s.preNewest->seq == b);
  CHECK(s.postNewest->value == 9); // the sides are tracked separately
}

TEST_CASE("a direct push that fails parks the value for the background retry", "[PaddingPush]")
{
  PendingPaddingPush s;
  s.retries = 2; // left over from an earlier edit's retries
  const uint64_t seq = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, /*isPre=*/true, 5, seq, /*ok=*/false);
  CHECK(s.pre == 5);
  CHECK_FALSE(s.post.has_value());
  CHECK(s.HasPending());
  CHECK(s.retries == 0); // a new edit gets its own chances
}

TEST_CASE("the newest edit's own success clears what was parked", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, false, 7);
  ApplyDirectPaddingPushResult(s, false, 7, seq, false);
  REQUIRE(s.post.has_value());
  ApplyDirectPaddingPushResult(s, false, 7, seq, true);
  CHECK_FALSE(s.HasPending());
}

TEST_CASE("an older value parked while the server was down does not survive the newer successful push", "[PaddingPush]")
{
  // 5 saved while Dispatcharr was down (parked), 7 saved once it was back (pushed fine).
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  CHECK_FALSE(s.pre.has_value());
}

TEST_CASE("two direct pushes finishing out of order park the newer value for a resend", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  CHECK_FALSE(s.pre.has_value());
  ApplyDirectPaddingPushResult(s, true, 5, first, true); // the older request may have landed last
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7);
  CHECK(s.preSeq == second);
  CHECK(ApplyPaddingRetryResult(s, PaddingEdit{7, second}, std::nullopt, true, kMaxRetries) ==
        PaddingRetryOutcome::kSent);
  CHECK_FALSE(s.pre.has_value());
}

TEST_CASE("direct pushes that finish in order leave nothing parked", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, true);
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  CHECK_FALSE(s.pre.has_value());
}

TEST_CASE("an older direct push that FAILS after a newer one succeeded parks the newer value, not its own",
          "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7);
  CHECK(s.preSeq == second);
}

TEST_CASE("a failure of an older edit never replaces a newer parked value", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, false);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7);
}

TEST_CASE("a successful retry clears what it sent and keeps an edit made meanwhile", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false); // parked
  const PaddingEdit sent{5, first};
  const uint64_t second = BeginPaddingEdit(s, true, 7); // made while the retry is in flight
  ApplyDirectPaddingPushResult(s, true, 7, second, false);
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, true, kMaxRetries) == PaddingRetryOutcome::kSent);
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7); // the newer one is still to be sent
}

TEST_CASE("a stale retry that lands after a newer successful push is corrected", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false); // parked
  const PaddingEdit sent{5, first};                       // the retry takes its snapshot
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true); // lands, clears the parked 5
  CHECK_FALSE(s.pre.has_value());
  ApplyPaddingRetryResult(s, sent, std::nullopt, true, kMaxRetries); // the stale retry then reports ok
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7); // 7 is sent again
}

TEST_CASE("a stale retry that FAILS after a newer successful push also re-parks the newer value", "[PaddingPush]")
{
  // The sweep's reproducer (A): the failed request had landed, after 7.
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  const PaddingEdit sent{5, first};
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 7);
  CHECK(s.preSeq == second);
}

TEST_CASE("an older direct success after a successful retry of the newest re-parks the newest", "[PaddingPush]")
{
  // The sweep's reproducer (B): 9 was parked and sent by a retry; an older 7 then lands last.
  PendingPaddingPush s;
  const uint64_t p2 = BeginPaddingEdit(s, true, 7);
  const uint64_t p3 = BeginPaddingEdit(s, true, 9);
  ApplyDirectPaddingPushResult(s, true, 9, p3, false); // parked
  CHECK(ApplyPaddingRetryResult(s, PaddingEdit{9, p3}, std::nullopt, true, kMaxRetries) == PaddingRetryOutcome::kSent);
  CHECK_FALSE(s.pre.has_value());
  ApplyDirectPaddingPushResult(s, true, 7, p2, true); // the older edit's success arrives now
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 9);
}

TEST_CASE("a failed retry is tried again until the limit, then the edit is dropped", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, seq, false);
  const PaddingEdit sent{5, seq};
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(s.pre.has_value());
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kGaveUp);
  CHECK_FALSE(s.HasPending());
  CHECK(s.retries == 0);
}

TEST_CASE("a success between failures resets the retry count", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, seq, false);
  const PaddingEdit sent{5, seq};
  ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries);
  ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries);
  CHECK(s.retries == 2);
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, true, kMaxRetries) == PaddingRetryOutcome::kSent);
  CHECK(s.retries == 0);
  CHECK_FALSE(s.HasPending());
}

TEST_CASE("each side is settled independently by a retry that sent both", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t pre = BeginPaddingEdit(s, true, 3);
  const uint64_t post = BeginPaddingEdit(s, false, 4);
  ApplyDirectPaddingPushResult(s, true, 3, pre, false);
  ApplyDirectPaddingPushResult(s, false, 4, post, false);
  const uint64_t newerPre = BeginPaddingEdit(s, true, 8); // only pre gets a newer edit
  ApplyDirectPaddingPushResult(s, true, 8, newerPre, true);
  ApplyPaddingRetryResult(s, PaddingEdit{3, pre}, PaddingEdit{4, post}, true, kMaxRetries);
  REQUIRE(s.pre.has_value()); // the stale pre retry may have landed after 8
  CHECK(*s.pre == 8);
  CHECK_FALSE(s.post.has_value()); // post's retry was its newest: settled
}

// ---------------------------------------------------------------------
// The exhaustive check (the 2026-10-04 eighth hardening sweep found two interleavings by this
// method, so it lives here). Three edits of one side (5, 7, 9, made in that order). Each edit's
// push reaches the server in some order (all 6 permutations) and is ok, failed-and-landed, or
// failed-and-not-landed (3^3). Results are applied in the order the requests reached the server:
// the client reports each result while still holding its DVR-settings mutex, which is what
// SetDvrOffsetMinutes()'s callback guarantees. Then the server is reachable again and the
// background retry runs until nothing is parked. The invariant: the server ends on the newest
// edit's value, 9. (Without that application order no bookkeeping can know that an older request
// landed after the newest one, which is why the order is part of the contract.)
// ---------------------------------------------------------------------

namespace
{
enum class Outcome
{
  kOk,
  kFailedLanded,
  kFailedNotLanded
};
} // namespace

TEST_CASE("every landing order and outcome of three edits ends with the server on the newest value", "[PaddingPush]")
{
  const std::array<int, 3> values = {5, 7, 9};
  std::array<int, 3> landingOrder = {0, 1, 2};
  int checked = 0;
  do
  {
    for (int code = 0; code < 27; ++code)
    {
      std::array<Outcome, 3> outcome = {static_cast<Outcome>(code % 3), static_cast<Outcome>((code / 3) % 3),
                                        static_cast<Outcome>((code / 9) % 3)};
      PendingPaddingPush s;
      std::array<uint64_t, 3> seq{};
      for (int i = 0; i < 3; ++i)
        seq[i] = BeginPaddingEdit(s, true, values[i]);

      int server = 0;
      for (int idx : landingOrder)
      {
        if (outcome[idx] != Outcome::kFailedNotLanded)
          server = values[idx];
        ApplyDirectPaddingPushResult(s, true, values[idx], seq[idx], outcome[idx] == Outcome::kOk);
      }

      // The server is reachable again: the background retry pushes whatever is parked.
      for (int cycle = 0; cycle < 6 && s.pre.has_value(); ++cycle)
      {
        const PaddingEdit sent{*s.pre, s.preSeq};
        server = sent.value;
        ApplyPaddingRetryResult(s, sent, std::nullopt, /*ok=*/true, kMaxRetries);
      }
      INFO("landing " << landingOrder[0] << landingOrder[1] << landingOrder[2] << " outcomes " << code);
      CHECK(server == 9);
      CHECK_FALSE(s.pre.has_value());
      ++checked;
    }
  } while (std::next_permutation(landingOrder.begin(), landingOrder.end()));
  CHECK(checked == 6 * 27);
}

TEST_CASE("a retry in flight while edits land and report ends with the server on the newest value", "[PaddingPush]")
{
  // Edit 1 (5) fails, parked; a retry snapshots 5 and sends it; edit 2 (7) is made and lands before, or after,
  // that retry (both orders, results applied in landing order), and the retry itself succeeds or fails.
  for (bool retryLandsFirst : {true, false})
  {
    for (bool retryOk : {true, false})
    {
      for (bool edit2Ok : {true, false})
      {
        PendingPaddingPush s;
        const uint64_t e1 = BeginPaddingEdit(s, true, 5);
        ApplyDirectPaddingPushResult(s, true, 5, e1, false);
        const PaddingEdit sent{5, e1};
        const uint64_t e2 = BeginPaddingEdit(s, true, 7);
        int server = 5; // edit 1 landed despite reporting failure: the worst case
        auto retry = [&]()
        {
          server = 5;
          ApplyPaddingRetryResult(s, sent, std::nullopt, retryOk, kMaxRetries);
        };
        auto edit2 = [&]()
        {
          server = 7;
          ApplyDirectPaddingPushResult(s, true, 7, e2, edit2Ok);
        };
        if (retryLandsFirst)
        {
          retry();
          edit2();
        }
        else
        {
          edit2();
          retry();
        }
        for (int cycle = 0; cycle < 6 && s.pre.has_value(); ++cycle)
        {
          const PaddingEdit again{*s.pre, s.preSeq};
          server = again.value;
          ApplyPaddingRetryResult(s, again, std::nullopt, true, kMaxRetries);
        }
        INFO("retryLandsFirst=" << retryLandsFirst << " retryOk=" << retryOk << " edit2Ok=" << edit2Ok);
        CHECK(server == 7);
      }
    }
  }
}

TEST_CASE("a newer edit that replaces a parked one restarts the retry count", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  REQUIRE(s.pre == 5);
  const std::optional<PaddingEdit> sent = PaddingEdit{5, first};
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  REQUIRE(s.retries == 2);
  // The user edits again and that push fails too: a different value is parked, with its own chances.
  const uint64_t second = BeginPaddingEdit(s, true, 6);
  ApplyDirectPaddingPushResult(s, true, 6, second, false);
  CHECK(s.pre == 6);
  CHECK(s.retries == 0);
}

TEST_CASE("a failed retry of an older post value re-parks the newer post value", "[PaddingPush]")
{
  // The post side alone (the pre side must not be what makes this pass): edit 1 parked, the retry
  // sends it, edit 2's direct push succeeds meanwhile, then the retry fails.
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, false, 7);
  ApplyDirectPaddingPushResult(s, false, 7, first, false);
  const std::optional<PaddingEdit> sentPost = PaddingEdit{7, first};
  const uint64_t second = BeginPaddingEdit(s, false, 9);
  ApplyDirectPaddingPushResult(s, false, 9, second, true);
  CHECK_FALSE(s.post.has_value());
  CHECK(ApplyPaddingRetryResult(s, std::nullopt, sentPost, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(s.post == 9);
  CHECK(s.postSeq == second);
}

TEST_CASE("the retry sends what is parked when it holds the mutex, not what it read before waiting", "[PaddingPush]")
{
  // Edit 1 fails and is parked. The background thread would read it now ...
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  const auto readEarly = PendingPaddingEdits(s);
  REQUIRE(readEarly.first.has_value());
  CHECK(readEarly.first->value == 5);
  // ... but edit 2 gets the mutex first and succeeds, which clears the parked value.
  const uint64_t second = BeginPaddingEdit(s, true, 7);
  ApplyDirectPaddingPushResult(s, true, 7, second, true);
  // Choosing again, now that the retry holds the mutex: nothing is left to send. Sending the value
  // read earlier would have put 5 back on the server.
  const auto chosen = PendingPaddingEdits(s);
  CHECK_FALSE(chosen.first.has_value());
  CHECK_FALSE(chosen.second.has_value());
}

TEST_CASE("PendingPaddingEdits reports each parked side with its own sequence", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t a = BeginPaddingEdit(s, true, 3);
  const uint64_t b = BeginPaddingEdit(s, false, 4);
  ApplyDirectPaddingPushResult(s, true, 3, a, false);
  ApplyDirectPaddingPushResult(s, false, 4, b, false);
  const auto pending = PendingPaddingEdits(s);
  REQUIRE(pending.first.has_value());
  REQUIRE(pending.second.has_value());
  CHECK(pending.first->value == 3);
  CHECK(pending.first->seq == a);
  CHECK(pending.second->value == 4);
  CHECK(pending.second->seq == b);
}

TEST_CASE("a padding value read from the server is only written over Kodi's when nothing newer exists", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t atStart = s.nextSeq;
  CHECK(IsSafeToSyncPaddingFromServer(s, atStart));

  // An edit begun after the read started.
  const uint64_t seq = BeginPaddingEdit(s, true, 5);
  CHECK_FALSE(IsSafeToSyncPaddingFromServer(s, atStart));
  // Begun before the read, its push still queued or in flight: the read may predate its PATCH.
  CHECK_FALSE(IsSafeToSyncPaddingFromServer(s, s.nextSeq));
  // Its push failed: parked, so still not safe.
  ApplyDirectPaddingPushResult(s, true, 5, seq, false);
  CHECK_FALSE(IsSafeToSyncPaddingFromServer(s, s.nextSeq));
  // Settled by a successful push, and no edit since: safe.
  const uint64_t next = BeginPaddingEdit(s, true, 6);
  ApplyDirectPaddingPushResult(s, true, 6, next, true);
  CHECK(s.inFlightEdits == 0);
  CHECK(IsSafeToSyncPaddingFromServer(s, s.nextSeq));
  // The read started before that edit: not safe.
  CHECK_FALSE(IsSafeToSyncPaddingFromServer(s, seq));
}

TEST_CASE("a result applied without a matching edit never drives the in-flight count negative", "[PaddingPush]")
{
  PendingPaddingPush s;
  ApplyDirectPaddingPushResult(s, true, 5, 1, true);
  CHECK(s.inFlightEdits == 0);
}

// The sixteenth hardening sweep's mutation survivors.

TEST_CASE("an edit parked on the post side alone is pending, and holds the startup sync back", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, false, 4);
  ApplyDirectPaddingPushResult(s, false, 4, seq, false);
  REQUIRE(s.post.has_value());
  CHECK_FALSE(s.pre.has_value());
  CHECK(s.HasPending());
  CHECK_FALSE(IsSafeToSyncPaddingFromServer(s, s.nextSeq));
}

TEST_CASE("giving up drops a parked post edit as well as a pre one", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, false, 4);
  ApplyDirectPaddingPushResult(s, false, 4, seq, false);
  const PaddingEdit sent{4, seq};
  CHECK(ApplyPaddingRetryResult(s, std::nullopt, sent, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(ApplyPaddingRetryResult(s, std::nullopt, sent, false, kMaxRetries) == PaddingRetryOutcome::kRetryLater);
  CHECK(ApplyPaddingRetryResult(s, std::nullopt, sent, false, kMaxRetries) == PaddingRetryOutcome::kGaveUp);
  CHECK_FALSE(s.post.has_value());
  CHECK_FALSE(s.HasPending());
}

TEST_CASE("giving up drops both sides when both were parked", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t a = BeginPaddingEdit(s, true, 1);
  const uint64_t b = BeginPaddingEdit(s, false, 2);
  ApplyDirectPaddingPushResult(s, true, 1, a, false);
  ApplyDirectPaddingPushResult(s, false, 2, b, false);
  const PaddingEdit pre{1, a}, post{2, b};
  ApplyPaddingRetryResult(s, pre, post, false, kMaxRetries);
  ApplyPaddingRetryResult(s, pre, post, false, kMaxRetries);
  CHECK(ApplyPaddingRetryResult(s, pre, post, false, kMaxRetries) == PaddingRetryOutcome::kGaveUp);
  CHECK_FALSE(s.pre.has_value());
  CHECK_FALSE(s.post.has_value());
}

TEST_CASE("parking a side that was already parked from the same edit does not restart the retry count", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t seq = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, seq, false);
  const PaddingEdit sent{5, seq};
  ApplyPaddingRetryResult(s, sent, std::nullopt, false, kMaxRetries);
  REQUIRE(s.retries == 1);
  // The same edit failing again (an older direct result arriving late, say) keeps its count.
  ParkNewestPaddingEdit(s, true);
  CHECK(s.retries == 1);
  // A newer edit, though, is a fresh chance.
  const uint64_t newer = BeginPaddingEdit(s, true, 6);
  ApplyDirectPaddingPushResult(s, true, 6, newer, false);
  CHECK(s.retries == 0);
}

TEST_CASE("a parked edit's own sequence is what a later success compares against", "[PaddingPush]")
{
  PendingPaddingPush s;
  const uint64_t first = BeginPaddingEdit(s, true, 5);
  ApplyDirectPaddingPushResult(s, true, 5, first, false); // parked at `first`
  const uint64_t second = BeginPaddingEdit(s, true, 6);
  // `second` succeeds: it is the newest, and what is parked is older than it, so it is cleared.
  ApplyDirectPaddingPushResult(s, true, 6, second, true);
  CHECK_FALSE(s.pre.has_value());
  // `first` finishing late with a failure must not bring its old value back over the newer one.
  ApplyDirectPaddingPushResult(s, true, 5, first, false);
  REQUIRE(s.pre.has_value());
  CHECK(*s.pre == 6);
}
