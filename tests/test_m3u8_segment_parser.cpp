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

TEST_CASE("ParseNewM3u8SegmentEntries clamps a negative EXTINF duration to 0, not UB", "[M3u8SegmentParser]")
{
  // "#EXTINF:-1," is a real M3U idiom, not just an adversarial input --
  // std::stod parses it as a plain, finite, negative double with no error.
  std::string playlist = "#EXTINF:-1,\nseg_00001.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].durationSec == 0.0);
}

TEST_CASE("ParseNewM3u8SegmentEntries clamps a finite-but-huge EXTINF duration instead of letting it overflow",
          "[M3u8SegmentParser]")
{
  // 1e300 passes std::isfinite() -- the caller's own
  // static_cast<int64_t>(durationSec * 1000) would be undefined behavior
  // without this clamp.
  std::string playlist = "#EXTINF:1e300,\nseg_00001.ts\n";

  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].durationSec == 86400.0);
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

// ---------------------------------------------------------------------
// CountLeadingProbedSegments
// ---------------------------------------------------------------------

TEST_CASE("CountLeadingProbedSegments counts every entry when every probe succeeded", "[M3u8SegmentParser]")
{
  CHECK(CountLeadingProbedSegments({100, 200, 300}) == 3);
}

TEST_CASE("CountLeadingProbedSegments stops at the first failed probe", "[M3u8SegmentParser]")
{
  // The exact bug this fixes: entry 1's probe failed (-1); entries 2 and
  // 3 succeeded despite that, but they must NOT be counted as merged --
  // cumulative byte offsets need complete, in-order knowledge of every
  // earlier segment's own size.
  CHECK(CountLeadingProbedSegments({100, -1, 300, 400}) == 1);
}

TEST_CASE("CountLeadingProbedSegments is zero when the very first probe failed", "[M3u8SegmentParser]")
{
  CHECK(CountLeadingProbedSegments({-1, 200, 300}) == 0);
}

TEST_CASE("CountLeadingProbedSegments treats a zero-byte probe result as failed too", "[M3u8SegmentParser]")
{
  CHECK(CountLeadingProbedSegments({100, 0, 300}) == 1);
}

TEST_CASE("CountLeadingProbedSegments is zero for an empty list", "[M3u8SegmentParser]")
{
  CHECK(CountLeadingProbedSegments({}) == 0);
}

TEST_CASE("CountLeadingProbedSegments end-to-end reproduces and then fixes the exact real incident",
          "[M3u8SegmentParser]")
{
  // Refresh 1: a 4-entry playlist, probe for entry 1 fails.
  std::string playlist4 = "#EXTINF:6.0,\nseg_0.ts\n"
                          "#EXTINF:6.0,\nseg_1.ts\n"
                          "#EXTINF:6.0,\nseg_2.ts\n"
                          "#EXTINF:6.0,\nseg_3.ts\n";
  std::vector<M3u8SegmentEntry> pending1 = ParseNewM3u8SegmentEntries(playlist4, kBaseDir, /*alreadyKnownCount=*/0);
  REQUIRE(pending1.size() == 4);
  std::vector<int64_t> probed1 = {100, -1, 100, 100}; // entry 1 (seg_1) fails to probe

  // The fix: only entries up to (not including) the first failure are
  // actually merged this round.
  size_t mergedCount1 = CountLeadingProbedSegments(probed1);
  REQUIRE(mergedCount1 == 1); // only seg_0

  // Refresh 2: the playlist gained a 5th entry. alreadyKnownCount is now
  // the *merged* count from refresh 1 (1), not pending1.size() (4) and
  // not a naive "segments vector size" that skipped the failure.
  std::string playlist5 = playlist4 + "#EXTINF:6.0,\nseg_4.ts\n";
  std::vector<M3u8SegmentEntry> pending2 =
      ParseNewM3u8SegmentEntries(playlist5, kBaseDir, /*alreadyKnownCount=*/mergedCount1);

  // Must re-offer seg_1 (the one that failed) plus everything after it --
  // never seg_0 again (already merged), and never silently drop seg_1.
  REQUIRE(pending2.size() == 4);
  CHECK(pending2[0].url == std::string(kBaseDir) + "seg_1.ts");
  CHECK(pending2[1].url == std::string(kBaseDir) + "seg_2.ts");
  CHECK(pending2[2].url == std::string(kBaseDir) + "seg_3.ts");
  CHECK(pending2[3].url == std::string(kBaseDir) + "seg_4.ts");
}

TEST_CASE("M3u8HasEndList finds the tag Dispatcharr appends once the recording's ffmpeg has stopped",
          "[M3u8SegmentParser]")
{
  const std::string recording = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nseg_00000.ts\n";
  CHECK_FALSE(M3u8HasEndList(recording));
  CHECK(M3u8HasEndList(recording + "#EXT-X-ENDLIST\n"));
  // Dispatcharr writes it last, but a file with no trailing newline, or with CRLF, is still a playlist.
  CHECK(M3u8HasEndList(recording + "#EXT-X-ENDLIST"));
  CHECK(M3u8HasEndList("#EXTM3U\r\n#EXTINF:4.0,\r\nseg_00000.ts\r\n#EXT-X-ENDLIST\r\n"));
}

TEST_CASE("M3u8HasEndList needs the whole line to be the tag", "[M3u8SegmentParser]")
{
  CHECK_FALSE(M3u8HasEndList(""));
  CHECK_FALSE(M3u8HasEndList("\n\n"));
  CHECK_FALSE(M3u8HasEndList("#EXT-X-ENDLIST-FAKE\n"));
  CHECK_FALSE(M3u8HasEndList("#EXT-X-ENDLIS\n"));
  CHECK_FALSE(M3u8HasEndList("x#EXT-X-ENDLIST\n"));
  CHECK_FALSE(M3u8HasEndList("# a comment mentioning #EXT-X-ENDLIST\n"));
  // A segment URI is never mistaken for it.
  CHECK_FALSE(M3u8HasEndList("#EXTINF:4.0,\nhttp://host/#EXT-X-ENDLIST\n"));
}

// ---------------------------------------------------------------------
// RebaseRecordingSegmentUrl
// ---------------------------------------------------------------------

TEST_CASE("RebaseRecordingSegmentUrl points a segment at the configured address when the playlist named an "
          "unreachable one",
          "[M3u8SegmentParser]")
{
  const std::string prefix = "/api/channels/recordings/42/hls/";
  // A proxy that forwards no port: the playlist names the internal 9191.
  CHECK(RebaseRecordingSegmentUrl("http://dispatcharr.lan:9191/api/channels/recordings/42/hls/seg_00001.ts",
                                  "https://dispatcharr.example.com:443", prefix) ==
        "https://dispatcharr.example.com:443/api/channels/recordings/42/hls/seg_00001.ts");
  CHECK(RebaseRecordingSegmentUrl("http://10.0.0.5:9191/api/channels/recordings/42/hls/seg_00001.ts",
                                  "http://10.0.0.5:9191",
                                  prefix) == "http://10.0.0.5:9191/api/channels/recordings/42/hls/seg_00001.ts");
}

TEST_CASE("RebaseRecordingSegmentUrl keeps a query string and tolerates a trailing slash on the base",
          "[M3u8SegmentParser]")
{
  CHECK(RebaseRecordingSegmentUrl("http://internal:9191/api/channels/recordings/42/hls/seg_1.ts?token=abc",
                                  "http://host:9191/", "/api/channels/recordings/42/hls/") ==
        "http://host:9191/api/channels/recordings/42/hls/seg_1.ts?token=abc");
}

TEST_CASE("RebaseRecordingSegmentUrl leaves anything that is not this recording's own segment path alone",
          "[M3u8SegmentParser]")
{
  const std::string prefix = "/api/channels/recordings/42/hls/";
  // A relative URI is resolved elsewhere.
  CHECK(RebaseRecordingSegmentUrl("seg_00001.ts", "http://host:9191", prefix) == "seg_00001.ts");
  // A different recording, or somewhere else entirely, is not rewritten.
  CHECK(RebaseRecordingSegmentUrl("http://other:1/api/channels/recordings/43/hls/seg_1.ts", "http://host:9191",
                                  prefix) == "http://other:1/api/channels/recordings/43/hls/seg_1.ts");
  CHECK(RebaseRecordingSegmentUrl("http://cdn.example.net/anything.ts", "http://host:9191", prefix) ==
        "http://cdn.example.net/anything.ts");
  // A lookalike host cannot smuggle a matching path: only the path prefix is tested, and
  // what is returned always starts with the configured base.
  CHECK(RebaseRecordingSegmentUrl("http://evil.example/api/channels/recordings/42/hls/x.ts", "http://host:9191", prefix)
            .rfind("http://host:9191/", 0) == 0);
  // No path at all, an empty base, and non-http schemes are returned untouched.
  CHECK(RebaseRecordingSegmentUrl("http://internal:9191", "http://host:9191", prefix) == "http://internal:9191");
  CHECK(RebaseRecordingSegmentUrl("http://internal:9191/api/channels/recordings/42/hls/a.ts", "", prefix) ==
        "http://internal:9191/api/channels/recordings/42/hls/a.ts");
  CHECK(RebaseRecordingSegmentUrl("ftp://internal/api/channels/recordings/42/hls/a.ts", "http://host:9191", prefix) ==
        "ftp://internal/api/channels/recordings/42/hls/a.ts");
}

TEST_CASE("RebaseRecordingSegmentUrl handles an https segment URL as well as an http one", "[M3u8SegmentParser]")
{
  CHECK(RebaseRecordingSegmentUrl("https://dispatcharr.lan/api/channels/recordings/42/hls/seg_1.ts", "http://host:9191",
                                  "/api/channels/recordings/42/hls/") ==
        "http://host:9191/api/channels/recordings/42/hls/seg_1.ts");
}

TEST_CASE("ParseNewM3u8SegmentEntries reads an EXTINF duration that has no trailing comma", "[M3u8SegmentParser]")
{
  // The comma and title are optional per the HLS spec; some muxers omit them.
  std::string playlist = "#EXTM3U\n#EXTINF:4\nseg_00001.ts\n#EXTINF:2.5\nseg_00002.ts\n";
  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].durationSec == 4.0);
  CHECK(entries[1].durationSec == 2.5);
}

TEST_CASE("M3u8HasEndList tolerates trailing blanks on the tag line but not leading ones", "[M3u8SegmentParser]")
{
  // A space or tab after the tag does not make it a different tag; without this a finished
  // recording stayed "not finished" for the whole 15 s no-tag grace period.
  CHECK(M3u8HasEndList("#EXTINF:4.0,\nseg.ts\n#EXT-X-ENDLIST \n"));
  CHECK(M3u8HasEndList("#EXTINF:4.0,\nseg.ts\n#EXT-X-ENDLIST\t\r\n"));
  CHECK_FALSE(M3u8HasEndList("#EXTINF:4.0,\nseg.ts\n #EXT-X-ENDLIST\n"));
  CHECK_FALSE(M3u8HasEndList("#EXTINF:4.0,\nseg.ts\n#EXT-X-ENDLIST x\n"));
}

TEST_CASE("a segment URI with no EXTINF of its own does not inherit the previous segment's duration",
          "[M3u8SegmentParser]")
{
  std::string playlist = "#EXTM3U\n"
                         "#EXTINF:6.006,\n"
                         "seg_00001.ts\n"
                         "seg_00002.ts\n";
  std::vector<M3u8SegmentEntry> entries = ParseNewM3u8SegmentEntries(playlist, kBaseDir, 0);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].durationSec == 6.006);
  CHECK(entries[1].durationSec == 0.0);
}
