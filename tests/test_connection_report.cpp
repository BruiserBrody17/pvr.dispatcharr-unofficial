#include "ConnectionReport.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("A rejected login is reported as access denied", "[ConnectionReport]")
{
  // The statuses ShouldCountTowardLoginBackoff() treats as a durable rejection.
  CHECK(ReportForAuthFailure(400, false) == ConnectionReport::kAccessDenied);
  CHECK(ReportForAuthFailure(401, false) == ConnectionReport::kAccessDenied);
  CHECK(ReportForAuthFailure(403, false) == ConnectionReport::kAccessDenied);
  // A short caller deadline changes nothing about what an answer proves.
  CHECK(ReportForAuthFailure(401, true) == ConnectionReport::kAccessDenied);
}

TEST_CASE("No answer or a 5xx is reported as unreachable", "[ConnectionReport]")
{
  CHECK(ReportForAuthFailure(0, false) == ConnectionReport::kUnreachable);
  CHECK(ReportForAuthFailure(500, false) == ConnectionReport::kUnreachable);
  CHECK(ReportForAuthFailure(502, false) == ConnectionReport::kUnreachable);
  CHECK(ReportForAuthFailure(503, true) == ConnectionReport::kUnreachable);
}

TEST_CASE("Silence under a caller's short deadline is not reported as unreachable", "[ConnectionReport]")
{
  // A live read gives a login a few seconds, not the configured timeout; running out of them proves only that the
  // server was slower than that.
  CHECK(ReportForAuthFailure(0, true) == ConnectionReport::kNone);
}

TEST_CASE("The login throttle and statuses nobody has explained report nothing", "[ConnectionReport]")
{
  // 429 follows earlier rejections that were already reported.
  CHECK(ReportForAuthFailure(429, false) == ConnectionReport::kNone);
  // Other 4xx: a status Dispatcharr's token endpoint could plausibly give and nobody has seen; saying "not Dispatcharr"
  // about one would be a guess.
  CHECK(ReportForAuthFailure(410, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(415, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(422, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(499, false) == ConnectionReport::kNone);
  // Informational responses and the status of a request that never got one.
  CHECK(ReportForAuthFailure(100, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(-1, false) == ConnectionReport::kNone);
}

TEST_CASE("An answer Dispatcharr's token endpoint never gives is reported as a server mismatch", "[ConnectionReport]")
{
  // A 2xx that is not a token (a login that reaches the caller as a failure with the status 200: a captive portal's
  // page, another service's JSON), a redirect, a route that does not exist, a path that does not take POST, and a
  // server that does not know the method (a plain web server).
  for (long status : {200L, 201L, 204L, 301L, 302L, 307L, 308L, 399L, 404L, 405L, 501L})
    CHECK(ReportForAuthFailure(status, false) == ConnectionReport::kServerMismatch);
  // What an answer proves does not depend on the caller's deadline.
  CHECK(ReportForAuthFailure(404, true) == ConnectionReport::kServerMismatch);
}

TEST_CASE("A gateway error is still unreachable, not a mismatch", "[ConnectionReport]")
{
  // 501 is the one 5xx that is a mismatch; 500, 502, 503 and 504 are a Dispatcharr (or its proxy) that is not up.
  for (long status : {500L, 502L, 503L, 504L, 599L})
    CHECK(ReportForAuthFailure(status, false) == ConnectionReport::kUnreachable);
}

TEST_CASE("The credential statuses stay access denied", "[ConnectionReport]")
{
  // None of the mismatch statuses may swallow the three that mean "Dispatcharr refused you".
  CHECK(ReportForAuthFailure(400, false) == ConnectionReport::kAccessDenied);
  CHECK(ReportForAuthFailure(401, false) == ConnectionReport::kAccessDenied);
  CHECK(ReportForAuthFailure(403, false) == ConnectionReport::kAccessDenied);
}

TEST_CASE("A mismatch is delivered once, and corrected to connected when the login next works", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kServerMismatch));
  CHECK(latch.TakePending(out, false));
  CHECK(out == ConnectionReport::kServerMismatch);
  // The same answer again (every login retry) says nothing new.
  CHECK_FALSE(latch.Observe(ConnectionReport::kServerMismatch));
  CHECK_FALSE(latch.TakePending(out, true));
  // A reverse proxy's 404 while its backend was replaced: the next login works.
  CHECK(latch.Observe(ConnectionReport::kConnected));
  CHECK_FALSE(latch.TakePending(out, false));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kConnected);
}

TEST_CASE("A mismatch is not held back like connected", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kServerMismatch);
  CHECK(latch.TakePending(out, false));
  CHECK(out == ConnectionReport::kServerMismatch);
}

TEST_CASE("A mismatch after the link was up is delivered as a mismatch, and an outage after it as unreachable",
          "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kConnected);
  CHECK(latch.TakePending(out, true));
  latch.Observe(ConnectionReport::kServerMismatch);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kServerMismatch);
  // The link is no longer reported as up, so an outage now is "unreachable", not "lost".
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kUnreachable);
}

TEST_CASE("The latch delivers nothing until something was observed", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK_FALSE(latch.TakePending(out, true));
  CHECK_FALSE(latch.Observe(ConnectionReport::kNone));
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("The latch delivers a new state once", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kUnreachable));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kUnreachable);
  CHECK_FALSE(latch.TakePending(out, true));
  // The same state seen again is not a change, so no delivery is scheduled for it.
  CHECK_FALSE(latch.Observe(ConnectionReport::kUnreachable));
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("The latch delivers only where a burst of changes ended", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kUnreachable));
  CHECK(latch.Observe(ConnectionReport::kAccessDenied));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kAccessDenied);
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("A change that is undone before delivery delivers nothing new", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kAccessDenied));
  CHECK(latch.TakePending(out, true));
  // Out and back before the worker looked: Kodi is still showing access denied, which is right.
  CHECK(latch.Observe(ConnectionReport::kUnreachable));
  CHECK(latch.Observe(ConnectionReport::kAccessDenied));
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("Connected is held back until the DVR access level is known", "[ConnectionReport]")
{
  // Kodi re-reads the capabilities on CONNECTED; reporting it before the account's access level is known would make
  // that re-read return the same fail-open answer it already has.
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kConnected));
  CHECK_FALSE(latch.TakePending(out, false));
  CHECK_FALSE(latch.TakePending(out, false));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kConnected);
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("Only connected is held back, a failure is delivered at once", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  CHECK(latch.Observe(ConnectionReport::kAccessDenied));
  CHECK(latch.TakePending(out, false));
  CHECK(out == ConnectionReport::kAccessDenied);
}

TEST_CASE("Recovery after an outage is delivered as connected", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK(latch.TakePending(out, false));
  CHECK(out == ConnectionReport::kUnreachable);
  latch.Observe(ConnectionReport::kConnected);
  CHECK_FALSE(latch.TakePending(out, false));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kConnected);
}

TEST_CASE("A held connected is dropped if the outage returns before it was delivered", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kUnreachable);
  latch.TakePending(out, true);
  latch.Observe(ConnectionReport::kConnected);
  CHECK_FALSE(latch.TakePending(out, false));
  // Down again while it was held: Kodi is still showing unreachable, so there is nothing to deliver.
  CHECK(latch.Observe(ConnectionReport::kUnreachable));
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("Concurrent observers and one deliverer end on the last observed state", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  std::atomic<bool> go{false};
  std::vector<std::thread> observers;
  for (int i = 0; i < 4; ++i)
  {
    observers.emplace_back(
        [&latch, &go]()
        {
          while (!go.load())
            std::this_thread::yield();
          for (int n = 0; n < 2000; ++n)
          {
            latch.Observe(n % 2 ? ConnectionReport::kUnreachable : ConnectionReport::kAccessDenied);
          }
        });
  }
  std::atomic<bool> stop{false};
  ConnectionReport last = ConnectionReport::kNone;
  std::thread deliverer(
      [&]()
      {
        ConnectionReport out = ConnectionReport::kNone;
        while (!stop.load())
        {
          if (latch.TakePending(out, true))
            last = out;
        }
        if (latch.TakePending(out, true))
          last = out;
      });
  go = true;
  for (auto& t : observers)
    t.join();
  // Every observer's last write is one of the two states; whichever won must be what the deliverer ended on.
  latch.Observe(ConnectionReport::kConnected);
  stop = true;
  deliverer.join();
  CHECK(last == ConnectionReport::kConnected);
}

// ---------------------------------------------------------------------
// IsConnectLevelFailure / LinkHealth: a mid-session outage seen from the requests
// ---------------------------------------------------------------------

TEST_CASE("Only failures to reach the server count as the link being down", "[ConnectionReport]")
{
  CHECK(IsConnectLevelFailure(CURLE_COULDNT_CONNECT, false));
  CHECK(IsConnectLevelFailure(CURLE_COULDNT_RESOLVE_HOST, false));
  CHECK(IsConnectLevelFailure(CURLE_GOT_NOTHING, false));
  CHECK(IsConnectLevelFailure(CURLE_RECV_ERROR, false));
  CHECK_FALSE(IsConnectLevelFailure(CURLE_ABORTED_BY_CALLBACK, false));
  CHECK_FALSE(IsConnectLevelFailure(CURLE_SSL_CONNECT_ERROR, false));
  CHECK_FALSE(IsConnectLevelFailure(CURLE_WRITE_ERROR, false));
}

TEST_CASE("A timeout counts only when the connection was never established", "[ConnectionReport]")
{
  // The guide download takes 13 to 30 s against a 30 s default timeout; a timeout on an open connection is a slow
  // server, not a lost one.
  CHECK_FALSE(IsConnectLevelFailure(CURLE_OPERATION_TIMEDOUT, false));
  CHECK(IsConnectLevelFailure(CURLE_OPERATION_TIMEDOUT, true));
}

namespace
{
using Clock = std::chrono::steady_clock;
const Clock::time_point kT0 = Clock::time_point{} + std::chrono::hours(1);
Clock::time_point At(int seconds)
{
  return kT0 + std::chrono::seconds(seconds);
}
ConnectionReport Fail(LinkHealth& h, int seconds)
{
  return h.OnAttempt(At(seconds), CURLE_COULDNT_CONNECT, 0, true, false);
}
ConnectionReport Answer(LinkHealth& h, int seconds, long http = 200, bool authenticated = true)
{
  return h.OnAttempt(At(seconds), 0, http, authenticated, false);
}
} // namespace

TEST_CASE("Three failures over ten seconds report the link down", "[ConnectionReport]")
{
  LinkHealth h;
  CHECK(Fail(h, 0) == ConnectionReport::kNone);
  CHECK(Fail(h, 5) == ConnectionReport::kNone);
  CHECK(Fail(h, 10) == ConnectionReport::kUnreachable);
  // Each further failure in the streak repeats it, which the latch ignores.
  CHECK(Fail(h, 15) == ConnectionReport::kUnreachable);
}

TEST_CASE("A burst of failures inside ten seconds is a blip, not an outage", "[ConnectionReport]")
{
  // The live manifest is polled twice a second; three refusals in a second say nothing yet.
  LinkHealth h;
  CHECK(Fail(h, 0) == ConnectionReport::kNone);
  CHECK(Fail(h, 1) == ConnectionReport::kNone);
  CHECK(Fail(h, 2) == ConnectionReport::kNone);
  CHECK(Fail(h, 9) == ConnectionReport::kNone);
  CHECK(Fail(h, 10) == ConnectionReport::kUnreachable);
}

TEST_CASE("An answer ends the streak", "[ConnectionReport]")
{
  LinkHealth h;
  Fail(h, 0);
  Fail(h, 6);
  CHECK(Answer(h, 8, 404) == ConnectionReport::kNone);
  // The streak starts again from nothing.
  CHECK(Fail(h, 9) == ConnectionReport::kNone);
  CHECK(Fail(h, 19) == ConnectionReport::kNone);
  CHECK(Fail(h, 25) == ConnectionReport::kUnreachable);
}

TEST_CASE("Failures a long time apart are not one streak", "[ConnectionReport]")
{
  LinkHealth h;
  Fail(h, 0);
  Fail(h, 5);
  // A request that fails every five minutes (the recordings refresh) never forms a streak by itself.
  CHECK(Fail(h, 305) == ConnectionReport::kNone);
  CHECK(Fail(h, 610) == ConnectionReport::kNone);
  CHECK(Fail(h, 915) == ConnectionReport::kNone);
}

TEST_CASE("An authenticated success is reported as connected, any other answer is not", "[ConnectionReport]")
{
  LinkHealth h;
  CHECK(Answer(h, 0, 200, true) == ConnectionReport::kConnected);
  CHECK(Answer(h, 1, 204, true) == ConnectionReport::kConnected);
  // Unauthenticated (the version check works for a rejected account too), not 2xx, or a 4xx: the server answered, which
  // ends a streak, but it does not prove the account is logged in.
  CHECK(Answer(h, 2, 200, false) == ConnectionReport::kNone);
  CHECK(Answer(h, 3, 403, true) == ConnectionReport::kNone);
  CHECK(Answer(h, 4, 404, true) == ConnectionReport::kNone);
}

TEST_CASE("A gateway error neither counts nor ends a streak", "[ConnectionReport]")
{
  // A reverse proxy answering 502 for a dead Dispatcharr is not evidence the server is up.
  LinkHealth h;
  Fail(h, 0);
  CHECK(Answer(h, 3, 502) == ConnectionReport::kNone);
  Fail(h, 6);
  CHECK(Answer(h, 8, 503) == ConnectionReport::kNone);
  CHECK(Fail(h, 10) == ConnectionReport::kUnreachable);
}

TEST_CASE("Failures that are not connect-level do not count and do not end a streak", "[ConnectionReport]")
{
  LinkHealth h;
  Fail(h, 0);
  CHECK(h.OnAttempt(At(4), CURLE_OPERATION_TIMEDOUT, 0, true, false) == ConnectionReport::kNone);
  Fail(h, 6);
  CHECK(h.OnAttempt(At(8), CURLE_ABORTED_BY_CALLBACK, 0, true, false) == ConnectionReport::kNone);
  CHECK(Fail(h, 10) == ConnectionReport::kUnreachable);
}

TEST_CASE("A timeout without a connection counts like a refusal", "[ConnectionReport]")
{
  LinkHealth h;
  CHECK(h.OnAttempt(At(0), CURLE_OPERATION_TIMEDOUT, 0, true, true) == ConnectionReport::kNone);
  CHECK(h.OnAttempt(At(30), CURLE_OPERATION_TIMEDOUT, 0, true, true) == ConnectionReport::kNone);
  CHECK(h.OnAttempt(At(60), CURLE_OPERATION_TIMEDOUT, 0, true, true) == ConnectionReport::kUnreachable);
}

TEST_CASE("An unreachable server is delivered as connection lost once the link was up", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kConnected);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kConnected);
  CHECK(latch.Observe(ConnectionReport::kUnreachable));
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kDisconnected);
  // Still down, seen again (a failed refresh after the failed requests): nothing more to say, and it never turns
  // into "unreachable" afterwards.
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK_FALSE(latch.TakePending(out, true));
}

TEST_CASE("Recovery after a lost connection is delivered as connected, and a second loss as lost again",
          "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kConnected);
  latch.TakePending(out, true);
  latch.Observe(ConnectionReport::kUnreachable);
  latch.TakePending(out, true);
  CHECK(out == ConnectionReport::kDisconnected);
  latch.Observe(ConnectionReport::kConnected);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kConnected);
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kDisconnected);
}

TEST_CASE("An outage before the link was ever up stays unreachable", "[ConnectionReport]")
{
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK(latch.TakePending(out, true));
  CHECK(out == ConnectionReport::kUnreachable);
}

TEST_CASE("A held connected, then an outage, delivers nothing for the link that never showed as up",
          "[ConnectionReport]")
{
  // Connected was never delivered (the access level was not known), so Kodi still shows what it showed before.
  ConnectionReportLatch latch;
  ConnectionReport out = ConnectionReport::kNone;
  latch.Observe(ConnectionReport::kConnected);
  CHECK_FALSE(latch.TakePending(out, false));
  latch.Observe(ConnectionReport::kUnreachable);
  CHECK(latch.TakePending(out, false));
  CHECK(out == ConnectionReport::kUnreachable);
}
