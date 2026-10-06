#include "SocketWait.h"

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace dispatcharr;
using std::chrono::milliseconds;

namespace
{
struct SocketPair
{
  int a = -1, b = -1;
  SocketPair()
  {
    int fds[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    a = fds[0];
    b = fds[1];
  }
  ~SocketPair()
  {
    if (a >= 0)
      close(a);
    if (b >= 0)
      close(b);
  }
};
} // namespace

TEST_CASE("WaitForSocketReady times out when nothing is readable", "[SocketWait]")
{
  SocketPair p;
  CHECK(WaitForSocketReady(p.a, /*forWrite=*/false, milliseconds(40)) == 0);
}

TEST_CASE("WaitForSocketReady reports a socket readable once data arrives", "[SocketWait]")
{
  SocketPair p;
  REQUIRE(write(p.b, "x", 1) == 1);
  CHECK(WaitForSocketReady(p.a, false, milliseconds(1000)) > 0);
}

TEST_CASE("WaitForSocketReady reports a fresh socket writable", "[SocketWait]")
{
  SocketPair p;
  CHECK(WaitForSocketReady(p.a, /*forWrite=*/true, milliseconds(1000)) > 0);
}

TEST_CASE("WaitForSocketReady refuses an invalid socket instead of handing select() a bad descriptor", "[SocketWait]")
{
  // FD_SET(-1, ...) was the undefined behaviour when curl reported no active socket.
  CHECK(WaitForSocketReady(CURL_SOCKET_BAD, false, milliseconds(10)) < 0);
  CHECK(WaitForSocketReady(CURL_SOCKET_BAD, true, milliseconds(10)) < 0);
  CHECK(WaitForSocketReady(987654, false, milliseconds(10)) < 0); // not an open descriptor
}

TEST_CASE("WaitForSocketReady works for a descriptor numbered past FD_SETSIZE -- select() could not", "[SocketWait]")
{
  rlimit lim{};
  REQUIRE(getrlimit(RLIMIT_NOFILE, &lim) == 0);
  const rlimit originalLimit = lim;
  // Puts the descriptor limit back whatever happens below, so later tests see the limit they started with.
  struct RestoreLimit
  {
    rlimit saved;
    ~RestoreLimit()
    {
      setrlimit(RLIMIT_NOFILE, &saved);
    }
  } restoreLimit{originalLimit};
  const rlim_t wanted = 2048;
  if (lim.rlim_cur < wanted)
  {
    if (lim.rlim_max < wanted)
    {
      SKIP("the hard descriptor limit is below what this test needs");
    }
    lim.rlim_cur = wanted;
    REQUIRE(setrlimit(RLIMIT_NOFILE, &lim) == 0);
  }

  SocketPair p;
  const int high = 1500; // past FD_SETSIZE (1024)
  if (fcntl(high, F_GETFD) != -1)
    SKIP("descriptor 1500 is already in use in this process"); // dup2() would silently close it
  REQUIRE(dup2(p.a, high) == high);
  REQUIRE(write(p.b, "x", 1) == 1);
  CHECK(WaitForSocketReady(high, false, milliseconds(1000)) > 0);
  CHECK(WaitForSocketReady(high, true, milliseconds(1000)) > 0);
  close(high);
}

TEST_CASE("WaitForSocketReady with a negative remaining time returns at once instead of waiting forever",
          "[SocketWait]")
{
  // poll() takes a negative timeout as "wait indefinitely": the clamp to zero is what keeps a deadline
  // that has already passed from hanging the caller.
  SocketPair p;
  const auto start = std::chrono::steady_clock::now();
  CHECK(WaitForSocketReady(p.a, false, milliseconds(-5)) == 0);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

#endif
