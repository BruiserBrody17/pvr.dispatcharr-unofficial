#include "StreamPropertyUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
bool HasProperty(const std::vector<StreamPropertyPair>& properties, const std::string& name, const std::string& value)
{
  for (const auto& p : properties)
  {
    if (p.name == name)
      return p.value == value;
  }
  return false;
}

bool HasName(const std::vector<StreamPropertyPair>& properties, const std::string& name)
{
  for (const auto& p : properties)
  {
    if (p.name == name)
      return true;
  }
  return false;
}
} // namespace

TEST_CASE("BuildLiveChannelStreamProperties (Server mode) leaves streamurl unset", "[StreamPropertyUtil]")
{
  // The load-bearing invariant: Kodi uses STREAMURL directly via its
  // generic CCurlFile when it's set, bypassing this addon's own stream
  // callbacks entirely -- confirmed live.
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftServer, "http://example/stream.ts");
  CHECK_FALSE(HasName(properties, "streamurl"));
  CHECK(HasProperty(properties, "isrealtimestream", "true"));
}

TEST_CASE("BuildLiveChannelStreamProperties (Server mode) returns exactly one property", "[StreamPropertyUtil]")
{
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftServer, "http://example/stream.ts");
  CHECK(properties.size() == 1);
}

TEST_CASE("BuildLiveChannelStreamProperties (Off mode) sets a plain streamurl and mimetype", "[StreamPropertyUtil]")
{
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftOff, "http://example/stream.ts");
  CHECK(HasProperty(properties, "streamurl", "http://example/stream.ts"));
  CHECK(HasProperty(properties, "isrealtimestream", "true"));
  CHECK(HasProperty(properties, "mimetype", "video/mp2t"));
}

TEST_CASE("BuildLiveChannelStreamProperties (Off mode) does not set any ffmpegdirect property", "[StreamPropertyUtil]")
{
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftOff, "http://example/stream.ts");
  CHECK_FALSE(HasName(properties, "inputstream"));
  CHECK_FALSE(HasName(properties, "inputstream.ffmpegdirect.stream_mode"));
}

TEST_CASE("BuildLiveChannelStreamProperties (Local mode) adds the ffmpegdirect timeshift properties",
          "[StreamPropertyUtil]")
{
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftLocal, "http://example/stream.ts");
  CHECK(HasProperty(properties, "streamurl", "http://example/stream.ts"));
  CHECK(HasProperty(properties, "inputstream", "inputstream.ffmpegdirect"));
  CHECK(HasProperty(properties, "inputstream.ffmpegdirect.stream_mode", "timeshift"));
  CHECK(HasProperty(properties, "inputstream.ffmpegdirect.is_realtime_stream", "true"));
}

TEST_CASE("BuildLiveChannelStreamProperties (Local mode) returns exactly six properties", "[StreamPropertyUtil]")
{
  auto properties = BuildLiveChannelStreamProperties(kLiveTimeshiftLocal, "http://example/stream.ts");
  CHECK(properties.size() == 6);
}

// ---------------------------------------------------------------------
// BuildCatchupStreamProperties
// ---------------------------------------------------------------------

TEST_CASE("BuildCatchupStreamProperties always sets a plain streamurl, isrealtimestream=false, and mimetype",
          "[StreamPropertyUtil]")
{
  auto properties = BuildCatchupStreamProperties("http://example/catchup.ts", /*enableFfmpegdirectSeek=*/false);
  CHECK(HasProperty(properties, "streamurl", "http://example/catchup.ts"));
  CHECK(HasProperty(properties, "isrealtimestream", "false"));
  CHECK(HasProperty(properties, "mimetype", "video/mp2t"));
}

TEST_CASE("BuildCatchupStreamProperties with the seek setting off returns exactly three properties",
          "[StreamPropertyUtil]")
{
  auto properties = BuildCatchupStreamProperties("http://example/catchup.ts", /*enableFfmpegdirectSeek=*/false);
  CHECK(properties.size() == 3);
  CHECK_FALSE(HasName(properties, "inputstream"));
}

TEST_CASE("BuildCatchupStreamProperties with the seek setting on adds inputstream.ffmpegdirect and its own "
          "is_realtime_stream=false",
          "[StreamPropertyUtil]")
{
  auto properties = BuildCatchupStreamProperties("http://example/catchup.ts", /*enableFfmpegdirectSeek=*/true);
  CHECK(HasProperty(properties, "inputstream", "inputstream.ffmpegdirect"));
  CHECK(HasProperty(properties, "inputstream.ffmpegdirect.is_realtime_stream", "false"));
  CHECK(properties.size() == 5);
}

TEST_CASE("BuildCatchupStreamProperties never sets stream_mode or open_mode -- the documented dead ends this "
          "guards against",
          "[StreamPropertyUtil]")
{
  // Two earlier attempts at improving catch-up seek reliability
  // (inputstream.ffmpegdirect.stream_mode, and separately forcing
  // open_mode=ffmpeg) were both tried and reverted after live testing
  // showed each made seeking measurably worse -- pinned down here so a
  // future change doesn't repeat one of those dead ends blind.
  auto properties = BuildCatchupStreamProperties("http://example/catchup.ts", /*enableFfmpegdirectSeek=*/true);
  CHECK_FALSE(HasName(properties, "inputstream.ffmpegdirect.stream_mode"));
  CHECK_FALSE(HasName(properties, "inputstream.ffmpegdirect.open_mode"));
}
