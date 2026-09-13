#include "SegmentLookup.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
// Minimal stand-in for LiveTimeshiftSegmentInfo/InProgressRecordingSegmentInfo
// -- FindSegmentContainingPosition is templated purely on byteOffset/
// byteSize members, so neither real (heavier) type is needed here.
struct FakeSegment
{
  int64_t byteOffset;
  int64_t byteSize;
};
} // namespace

TEST_CASE("FindSegmentContainingPosition finds the segment containing a position", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments = {{0, 100}, {100, 200}, {300, 50}};

  const FakeSegment* seg = FindSegmentContainingPosition<FakeSegment>(150, segments);

  REQUIRE(seg != nullptr);
  CHECK(seg->byteOffset == 100);
  CHECK(seg->byteSize == 200);
}

TEST_CASE("FindSegmentContainingPosition matches the exact start of a segment (inclusive)", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments = {{0, 100}, {100, 200}};

  const FakeSegment* seg = FindSegmentContainingPosition<FakeSegment>(100, segments);

  REQUIRE(seg != nullptr);
  CHECK(seg->byteOffset == 100);
}

TEST_CASE("FindSegmentContainingPosition treats a segment's end as exclusive", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments = {{0, 100}, {100, 200}};

  const FakeSegment* seg = FindSegmentContainingPosition<FakeSegment>(300, segments);

  CHECK(seg == nullptr); // 300 is one past the last segment's own end (100 + 200)
}

TEST_CASE("FindSegmentContainingPosition returns nullptr for an empty segment list", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments;

  CHECK(FindSegmentContainingPosition<FakeSegment>(0, segments) == nullptr);
}

TEST_CASE("FindSegmentContainingPosition returns nullptr for a position before every segment", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments = {{100, 50}};

  CHECK(FindSegmentContainingPosition<FakeSegment>(50, segments) == nullptr);
}

TEST_CASE("FindSegmentContainingPosition finds the first segment", "[SegmentLookup]")
{
  std::vector<FakeSegment> segments = {{0, 100}, {100, 200}};

  const FakeSegment* seg = FindSegmentContainingPosition<FakeSegment>(0, segments);

  REQUIRE(seg != nullptr);
  CHECK(seg->byteOffset == 0);
}
