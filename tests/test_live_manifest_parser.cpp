#include "LiveManifestParser.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("ParseNewLiveManifestSegments keeps only sequences newer than lastKnownSequence", "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 1}, {"filename", "seg1.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 2}, {"filename", "seg2.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 3}, {"filename", "seg3.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  auto entries = ParseNewLiveManifestSegments(segments, /*lastKnownSequence=*/1);

  REQUIRE(entries.size() == 2);
  CHECK(entries[0].sequence == 2);
  CHECK(entries[1].sequence == 3);
}

TEST_CASE("ParseNewLiveManifestSegments returns every entry when lastKnownSequence is -1 (nothing known yet)",
          "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 0}, {"filename", "seg0.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].sequence == 0);
}

TEST_CASE("ParseNewLiveManifestSegments maps filename/byteSize/durationMs", "[LiveManifestParser]")
{
  json segments =
      json::array({{{"sequence", 5}, {"filename", "seg_00005.ts"}, {"byte_size", 188416}, {"duration_ms", 6006}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].filename == "seg_00005.ts");
  CHECK(entries[0].byteSize == 188416);
  CHECK(entries[0].durationMs == 6006);
}

TEST_CASE("ParseNewLiveManifestSegments drops an entry with a negative sequence", "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", -1}, {"filename", "seg.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  CHECK(ParseNewLiveManifestSegments(segments, -1).empty());
}

TEST_CASE("ParseNewLiveManifestSegments drops a malformed entry with an empty filename", "[LiveManifestParser]")
{
  // The documented real concern: a malformed entry must never be returned,
  // since it would corrupt the caller's own cumulative byte/time offsets
  // for every segment merged after it.
  json segments = json::array({{{"sequence", 1}, {"filename", ""}, {"byte_size", 100}, {"duration_ms", 2000}}});

  CHECK(ParseNewLiveManifestSegments(segments, -1).empty());
}

TEST_CASE("ParseNewLiveManifestSegments drops a malformed entry with a non-positive byte_size", "[LiveManifestParser]")
{
  json zeroSize = json::array({{{"sequence", 1}, {"filename", "seg.ts"}, {"byte_size", 0}, {"duration_ms", 2000}}});
  json negativeSize =
      json::array({{{"sequence", 1}, {"filename", "seg.ts"}, {"byte_size", -5}, {"duration_ms", 2000}}});

  CHECK(ParseNewLiveManifestSegments(zeroSize, -1).empty());
  CHECK(ParseNewLiveManifestSegments(negativeSize, -1).empty());
}

TEST_CASE("ParseNewLiveManifestSegments keeps well-formed entries even alongside a malformed one",
          "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 1}, {"filename", "seg1.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 2}, {"filename", ""}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 3}, {"filename", "seg3.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 2);
  CHECK(entries[0].sequence == 1);
  CHECK(entries[1].sequence == 3);
}

// The 2026-10-18 audit: only lastKnownSequence was checked, so a sequence that repeated or went back inside one
// response was appended into the byte stream a second time.
TEST_CASE("ParseNewLiveManifestSegments keeps only a strictly increasing run of sequences", "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 5}, {"filename", "seg5.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 6}, {"filename", "seg6.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 5}, {"filename", "seg5.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 6}, {"filename", "seg6b.ts"}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 7}, {"filename", "seg7.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  auto entries = ParseNewLiveManifestSegments(segments, /*lastKnownSequence=*/4);

  REQUIRE(entries.size() == 3);
  CHECK(entries[0].sequence == 5);
  CHECK(entries[1].sequence == 6);
  CHECK(entries[1].filename == "seg6.ts");
  CHECK(entries[2].sequence == 7);
}

TEST_CASE("ParseNewLiveManifestSegments does not let a malformed entry raise the bar for the next one",
          "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 5}, {"filename", ""}, {"byte_size", 100}, {"duration_ms", 2000}},
                               {{"sequence", 5}, {"filename", "seg5.ts"}, {"byte_size", 100}, {"duration_ms", 2000}}});

  auto entries = ParseNewLiveManifestSegments(segments, /*lastKnownSequence=*/4);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].filename == "seg5.ts");
}

TEST_CASE("ParseNewLiveManifestSegments returns nothing for an empty array", "[LiveManifestParser]")
{
  CHECK(ParseNewLiveManifestSegments(json::array(), -1).empty());
}

TEST_CASE("ParseNewLiveManifestSegments clamps a negative duration_ms to 0 rather than dropping the entry",
          "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 1}, {"filename", "seg.ts"}, {"byte_size", 100}, {"duration_ms", -500}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].durationMs == 0);
}

TEST_CASE("ParseNewLiveManifestSegments clamps an absurdly large duration_ms to a sane ceiling", "[LiveManifestParser]")
{
  // timeshift_buffer's own plugin.py has no upper bound of its own on the
  // #EXTINF-derived duration_ms it sends -- without this clamp, a bogus
  // value here would make the caller's cumulative totalDurationMs
  // non-monotonic, or contribute toward overflowing it.
  json segments =
      json::array({{{"sequence", 1}, {"filename", "seg.ts"}, {"byte_size", 100}, {"duration_ms", 999999999999LL}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].durationMs == 86400000);
}

TEST_CASE("ParseNewLiveManifestSegments clamps an absurdly large byte_size to a sane ceiling", "[LiveManifestParser]")
{
  // A 52nd-pass audit regression case: byte_size had no upper bound at
  // all before this, despite the same overflow risk durationMs's own
  // clamp above already exists for -- AppendSegmentOffsets() adds each
  // entry's byteSize into the caller's own running cumulative total,
  // and two sufficiently huge byteSize values summed is signed int64
  // overflow, undefined behavior in C++.
  json segments =
      json::array({{{"sequence", 1}, {"filename", "seg.ts"}, {"byte_size", 9999999999999999LL}, {"duration_ms", 0}}});

  auto entries = ParseNewLiveManifestSegments(segments, -1);

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].byteSize == 1099511627776LL);
}

TEST_CASE("OldestLiveManifestSequence is the lowest sequence in the plugin's current window", "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 40}, {"filename", "a.ts"}, {"byte_size", 100}},
                               {{"sequence", 41}, {"filename", "b.ts"}, {"byte_size", 100}},
                               {{"sequence", 42}, {"filename", "c.ts"}, {"byte_size", 100}}});
  CHECK(OldestLiveManifestSequence(segments) == 40);
}

TEST_CASE("OldestLiveManifestSequence doesn't assume the entries are sorted", "[LiveManifestParser]")
{
  json segments = json::array({{{"sequence", 12}}, {{"sequence", 7}}, {{"sequence", 9}}});
  CHECK(OldestLiveManifestSequence(segments) == 7);
}

TEST_CASE("OldestLiveManifestSequence ignores entries with no usable sequence", "[LiveManifestParser]")
{
  CHECK(OldestLiveManifestSequence(json::array()) == -1);
  CHECK(OldestLiveManifestSequence(json::array({{{"filename", "a.ts"}}})) == -1);
  CHECK(OldestLiveManifestSequence(json::array({{{"sequence", -3}}})) == -1);
  CHECK(OldestLiveManifestSequence(json::array({{{"sequence", "x"}}, {{"sequence", 5}}})) == 5);
  CHECK(OldestLiveManifestSequence(json::array({{{"sequence", 0}}, {{"sequence", 4}}})) == 0);
}

TEST_CASE("IsValidManifestRouteName accepts a plain file name and refuses anything that changes the URL",
          "[LiveManifestParser]")
{
  CHECK(IsValidManifestRouteName("manifest.json"));
  CHECK(IsValidManifestRouteName("a"));
  CHECK(IsValidManifestRouteName(std::string(64, 'm')));
  CHECK_FALSE(IsValidManifestRouteName(""));
  CHECK_FALSE(IsValidManifestRouteName(std::string(65, 'm')));
  CHECK_FALSE(IsValidManifestRouteName(".hidden"));
  CHECK_FALSE(IsValidManifestRouteName("a..b"));
  CHECK_FALSE(IsValidManifestRouteName("../manifest.json"));
  CHECK_FALSE(IsValidManifestRouteName("x/manifest.json"));
  CHECK_FALSE(IsValidManifestRouteName("manifest.json?token=x"));
  CHECK_FALSE(IsValidManifestRouteName("manifest json"));
  CHECK_FALSE(IsValidManifestRouteName("manifest%2ejson"));
  CHECK_FALSE(IsValidManifestRouteName(std::string("mani\0fest", 9)));
}

TEST_CASE("ReadStartAnswerManifestRoute reads the route a start_buffer answer names", "[LiveManifestParser]")
{
  const std::string uuidRoute = "/11111111-2222-3333-4444-555555555555/";
  std::string prefix = "unchanged";
  std::string name = "unchanged";

  SECTION("both fields, as timeshift_buffer 0.8.19 sends them")
  {
    const auto result = nlohmann::json{{"status", "ok"},
                                       {"http_port", 9192},
                                       {"playlist_route", uuidRoute + "live.m3u8"},
                                       {"segment_route_prefix", uuidRoute},
                                       {"manifest_route_name", "manifest.json"}};
    REQUIRE(ReadStartAnswerManifestRoute(result, prefix, name));
    CHECK(prefix == uuidRoute);
    CHECK(name == "manifest.json");
  }

  SECTION("an older plugin's answer, without them, leaves the cold start on the action")
  {
    const auto result =
        nlohmann::json{{"status", "ok"}, {"http_port", 9192}, {"playlist_route", uuidRoute + "live.m3u8"}};
    CHECK_FALSE(ReadStartAnswerManifestRoute(result, prefix, name));
    CHECK(prefix == "unchanged");
    CHECK(name == "unchanged");
  }

  SECTION("either one missing, of the wrong type or unsafe in a URL")
  {
    const nlohmann::json bad[] = {
        {{"segment_route_prefix", uuidRoute}},
        {{"manifest_route_name", "manifest.json"}},
        {{"segment_route_prefix", uuidRoute}, {"manifest_route_name", 7}},
        {{"segment_route_prefix", nullptr}, {"manifest_route_name", "manifest.json"}},
        {{"segment_route_prefix", uuidRoute}, {"manifest_route_name", "../manifest.json"}},
        {{"segment_route_prefix", "@otherhost/"}, {"manifest_route_name", "manifest.json"}},
        {{"segment_route_prefix", "relative/"}, {"manifest_route_name", "manifest.json"}},
    };
    for (const auto& result : bad)
    {
      CHECK_FALSE(ReadStartAnswerManifestRoute(result, prefix, name));
      CHECK(prefix == "unchanged");
      CHECK(name == "unchanged");
    }
  }
}

TEST_CASE("WrapManifestRouteAnswer wraps the plugin's answer as the action's envelope", "[LiveManifestParser]")
{
  nlohmann::json envelope;
  REQUIRE(WrapManifestRouteAnswer(true, 200, R"({"status":"ok","segments":[],"http_port":9192})", envelope));
  CHECK(envelope["success"] == true);
  CHECK(envelope["result"]["status"] == "ok");
  CHECK(envelope["result"]["http_port"] == 9192);

  // The plugin's own error answers ("not ready", fatal) are answers too: the caller reads them as the action's.
  nlohmann::json errorEnvelope;
  REQUIRE(WrapManifestRouteAnswer(true, 200, R"({"status":"error","fatal":true,"message":"gone"})", errorEnvelope));
  CHECK(errorEnvelope["result"]["fatal"] == true);
}

TEST_CASE("WrapManifestRouteAnswer refuses anything but the plugin's 200 JSON answer", "[LiveManifestParser]")
{
  nlohmann::json envelope = "untouched";
  const std::string ok = R"({"status":"ok"})";
  CHECK_FALSE(WrapManifestRouteAnswer(false, 200, ok, envelope)); // transport failure
  CHECK_FALSE(WrapManifestRouteAnswer(true, 404, ok, envelope));  // an older plugin
  CHECK_FALSE(WrapManifestRouteAnswer(true, 403, ok, envelope));  // buffer gone or replaced
  CHECK_FALSE(WrapManifestRouteAnswer(true, 503, ok, envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 206, ok, envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 200, "#EXTM3U\n", envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 200, "[1,2]", envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 200, R"({"segments":[]})", envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 200, R"({"status":1})", envelope));
  CHECK_FALSE(WrapManifestRouteAnswer(true, 200, "1" + std::string(400, '0'), envelope)); // parse throws out_of_range
  CHECK(envelope == "untouched");
}
