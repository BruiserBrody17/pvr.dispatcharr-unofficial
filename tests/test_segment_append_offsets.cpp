#include "SegmentAppendOffsets.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("AppendSegmentOffsets assigns the current totals as this segment's own offsets", "[SegmentAppendOffsets]")
{
  int64_t totalBytes = 1000;
  int64_t totalDurationMs = 5000;

  SegmentAppendOffsets offsets = AppendSegmentOffsets(200, 3000, totalBytes, totalDurationMs);

  CHECK(offsets.byteOffset == 1000);
  CHECK(offsets.timeOffsetMs == 5000);
}

TEST_CASE("AppendSegmentOffsets advances the running totals by the new segment's own size", "[SegmentAppendOffsets]")
{
  int64_t totalBytes = 1000;
  int64_t totalDurationMs = 5000;

  AppendSegmentOffsets(200, 3000, totalBytes, totalDurationMs);

  CHECK(totalBytes == 1200);
  CHECK(totalDurationMs == 8000);
}

TEST_CASE("AppendSegmentOffsets starts a fresh stream's first segment at offset zero", "[SegmentAppendOffsets]")
{
  int64_t totalBytes = 0;
  int64_t totalDurationMs = 0;

  SegmentAppendOffsets offsets = AppendSegmentOffsets(500, 6000, totalBytes, totalDurationMs);

  CHECK(offsets.byteOffset == 0);
  CHECK(offsets.timeOffsetMs == 0);
  CHECK(totalBytes == 500);
  CHECK(totalDurationMs == 6000);
}

TEST_CASE("AppendSegmentOffsets keeps successive segments contiguous", "[SegmentAppendOffsets]")
{
  int64_t totalBytes = 0;
  int64_t totalDurationMs = 0;

  SegmentAppendOffsets first = AppendSegmentOffsets(100, 1000, totalBytes, totalDurationMs);
  SegmentAppendOffsets second = AppendSegmentOffsets(200, 2000, totalBytes, totalDurationMs);

  CHECK(first.byteOffset == 0);
  CHECK(second.byteOffset == 100);
  CHECK(first.timeOffsetMs == 0);
  CHECK(second.timeOffsetMs == 1000);
  CHECK(totalBytes == 300);
  CHECK(totalDurationMs == 3000);
}
