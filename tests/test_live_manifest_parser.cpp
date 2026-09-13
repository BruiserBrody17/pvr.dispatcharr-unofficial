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
