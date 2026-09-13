#include "M3u8SegmentParser.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr const char* kBaseDir = "http://example/recordings/1/hls/";
} // namespace

TEST_CASE("ParseNewM3u8SegmentEntries parses segment URLs with their EXTINF duration", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTM3U\n"
                         "#EXTINF:6.006,\n"
                         "seg_00001.ts\n"
                         "#EXTINF:6.000,\n"
                         "seg_00002.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 2);
  CHECK(entries[0].url == "http://example/recordings/1/hls/seg_00001.ts");
  CHECK(entries[0].durationSec == 6.006);
  CHECK(entries[1].url == "http://example/recordings/1/hls/seg_00002.ts");
  CHECK(entries[1].durationSec == 6.000);
}

TEST_CASE("ParseNewM3u8SegmentEntries keeps an already-absolute segment URL as-is", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:2.0,\nhttps://other-host/seg.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].url == "https://other-host/seg.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries skips segments at or before alreadyKnownCount", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:2.0,\nseg_00001.ts\n"
                         "#EXTINF:2.0,\nseg_00002.ts\n"
                         "#EXTINF:2.0,\nseg_00003.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 2);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].url == "http://example/recordings/1/hls/seg_00003.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries returns nothing new when alreadyKnownCount covers every segment",
          "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:2.0,\nseg_00001.ts\n#EXTINF:2.0,\nseg_00002.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 5);

  CHECK(entries.empty());
}

TEST_CASE("ParseNewM3u8SegmentEntries ignores non-#EXTINF/segment tag lines", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTM3U\n"
                         "#EXT-X-VERSION:3\n"
                         "#EXT-X-TARGETDURATION:6\n"
                         "#EXTINF:6.0,\n"
                         "seg_00001.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].url == "http://example/recordings/1/hls/seg_00001.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries defaults an unparseable EXTINF duration to 0", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:not-a-number,\nseg_00001.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].durationSec == 0.0);
}

TEST_CASE("ParseNewM3u8SegmentEntries defaults a non-finite EXTINF duration to 0, not UB", "[M3u8SegmentParser]")
{
  // The documented real concern this function guards against: std::stod
  // (unlike std::stoi) accepts "inf"/"nan" as valid input and does NOT
  // throw for either, so this can't be caught the same way a genuinely
  // unparseable string is. A non-finite double later feeding a
  // static_cast<int64_t>() in the caller would be undefined behavior.
  std::string infPlaylist = "#EXTINF:inf,\nseg_00001.ts\n";
  std::string nanPlaylist = "#EXTINF:nan,\nseg_00001.ts\n";
  std::string negInfPlaylist = "#EXTINF:-inf,\nseg_00001.ts\n";

  CHECK(ParseNewM3u8SegmentEntries(infPlaylist, kBaseDir, 0)[0].durationSec == 0.0);
  CHECK(ParseNewM3u8SegmentEntries(nanPlaylist, kBaseDir, 0)[0].durationSec == 0.0);
  CHECK(ParseNewM3u8SegmentEntries(negInfPlaylist, kBaseDir, 0)[0].durationSec == 0.0);
}

TEST_CASE("ParseNewM3u8SegmentEntries handles CRLF line endings", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:6.0,\r\nseg_00001.ts\r\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].url == "http://example/recordings/1/hls/seg_00001.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries handles a playlist with no trailing newline", "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTINF:6.0,\nseg_00001.ts";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].url == "http://example/recordings/1/hls/seg_00001.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries returns nothing for an empty playlist", "[M3u8SegmentParser]")
{
  CHECK(ParseNewM3u8SegmentEntries("", kBaseDir, 0).empty());
}
