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
