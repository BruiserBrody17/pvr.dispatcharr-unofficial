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
