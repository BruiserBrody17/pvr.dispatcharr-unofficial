#include "PluginUrlUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

// ---------------------------------------------------------------------
// BuildTimeshiftPlaylistUrl
// ---------------------------------------------------------------------

TEST_CASE("BuildTimeshiftPlaylistUrl with no access token appends nothing", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftPlaylistUrl("dispatcharr.local", 9192, "/abc/live.m3u8", "") ==
        "http://dispatcharr.local:9192/abc/live.m3u8");
}

TEST_CASE("BuildTimeshiftPlaylistUrl appends ?token= when the route has no existing query string", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftPlaylistUrl("dispatcharr.local", 9192, "/abc/live.m3u8", "sekret") ==
        "http://dispatcharr.local:9192/abc/live.m3u8?token=sekret");
}

TEST_CASE("BuildTimeshiftPlaylistUrl appends &token= when the route already has a query string", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftPlaylistUrl("dispatcharr.local", 9192, "/abc/live.m3u8?foo=bar", "sekret") ==
        "http://dispatcharr.local:9192/abc/live.m3u8?foo=bar&token=sekret");
}

TEST_CASE("BuildTimeshiftPlaylistUrl uses the given port, not any default", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftPlaylistUrl("host", 12345, "/x", "") == "http://host:12345/x");
}

TEST_CASE("BuildTimeshiftPlaylistUrl brackets an IPv6 host literal", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftPlaylistUrl("::1", 9192, "/abc/live.m3u8", "") == "http://[::1]:9192/abc/live.m3u8");
}

// ---------------------------------------------------------------------
// BuildTimeshiftSegmentBaseUrl
// ---------------------------------------------------------------------

TEST_CASE("BuildTimeshiftSegmentBaseUrl assembles scheme, host, port, and prefix", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftSegmentBaseUrl("dispatcharr.local", 9192, "/segments/abc/") ==
        "http://dispatcharr.local:9192/segments/abc/");
}

TEST_CASE("BuildTimeshiftSegmentBaseUrl never appends a token itself -- the caller does that per segment",
          "[PluginUrlUtil]")
{
  std::string url = BuildTimeshiftSegmentBaseUrl("host", 9192, "/segments/abc/");
  CHECK(url.find('?') == std::string::npos);
}

TEST_CASE("BuildTimeshiftSegmentBaseUrl brackets an IPv6 host literal", "[PluginUrlUtil]")
{
  CHECK(BuildTimeshiftSegmentBaseUrl("2001:db8::1", 9192, "/segments/abc/") ==
        "http://[2001:db8::1]:9192/segments/abc/");
}

// IsTimeshiftFileServerReachable
TEST_CASE("IsTimeshiftFileServerReachable counts any HTTP answer, including the token-less 403", "[PluginUrlUtil]")
{
  CHECK(IsTimeshiftFileServerReachable(true, 403));
  CHECK(IsTimeshiftFileServerReachable(true, 200));
  CHECK(IsTimeshiftFileServerReachable(true, 404));
  CHECK(IsTimeshiftFileServerReachable(true, 501));
}

TEST_CASE("IsTimeshiftFileServerReachable rejects a failed transfer, whatever status was left behind",
          "[PluginUrlUtil]")
{
  // A refused or timed-out connection has no status (0); a stale one must not make it look reachable.
  CHECK_FALSE(IsTimeshiftFileServerReachable(false, 0));
  CHECK_FALSE(IsTimeshiftFileServerReachable(false, 403));
  CHECK_FALSE(IsTimeshiftFileServerReachable(true, 0));
}

TEST_CASE("BuildTimeshiftFileServerUnreachableMessage names the port", "[PluginUrlUtil]")
{
  const std::string message = BuildTimeshiftFileServerUnreachableMessage(9192);
  CHECK(message.find("9192") != std::string::npos);
  CHECK(message.find("http_port") != std::string::npos);
  CHECK(BuildTimeshiftFileServerUnreachableMessage(9393).find("9393") != std::string::npos);
}
