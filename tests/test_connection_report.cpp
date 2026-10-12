#include "ConnectionReport.h"

#include <atomic>
#include <thread>
#include <vector>

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

TEST_CASE("The login throttle and unrelated statuses report nothing", "[ConnectionReport]")
{
  // 429 follows earlier rejections that were already reported.
  CHECK(ReportForAuthFailure(429, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(404, false) == ConnectionReport::kNone);
  CHECK(ReportForAuthFailure(405, false) == ConnectionReport::kNone);
  // A 200 whose body held no token reaches the caller with the status 200.
  CHECK(ReportForAuthFailure(200, false) == ConnectionReport::kNone);
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
