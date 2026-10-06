#include "StreamSeek.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <limits>

using namespace dispatcharr;

TEST_CASE("ResolveSeekPosition SEEK_SET goes to the absolute position", "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(500, SEEK_SET, /*current=*/100, /*length=*/1000) == 500);
  CHECK(ResolveSeekPosition(0, SEEK_SET, /*current=*/100, /*length=*/1000) == 0);
}

TEST_CASE("ResolveSeekPosition SEEK_CUR is relative to the current position", "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(50, SEEK_CUR, /*current=*/100, /*length=*/1000) == 150);
  CHECK(ResolveSeekPosition(-50, SEEK_CUR, /*current=*/100, /*length=*/1000) == 50);
}

TEST_CASE("ResolveSeekPosition SEEK_END is relative to the reference length", "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(0, SEEK_END, /*current=*/100, /*length=*/1000) == 1000);
  CHECK(ResolveSeekPosition(-100, SEEK_END, /*current=*/100, /*length=*/1000) == 900);
}

TEST_CASE("ResolveSeekPosition fails a SEEK_END request when the reference length isn't known yet", "[StreamSeek]")
{
  // Matches SeekRecordingStream()'s own pre-existing "length < 0" guard --
  // a recording whose real length hasn't been discovered yet can't
  // meaningfully honor a request relative to its end.
  CHECK(ResolveSeekPosition(0, SEEK_END, /*current=*/100, /*length=*/-1) == -1);
}

TEST_CASE("ResolveSeekPosition fails on a negative computed position", "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(-500, SEEK_SET, /*current=*/100, /*length=*/1000) == -1);
  CHECK(ResolveSeekPosition(-200, SEEK_CUR, /*current=*/100, /*length=*/1000) == -1);
  CHECK(ResolveSeekPosition(-2000, SEEK_END, /*current=*/100, /*length=*/1000) == -1);
}

TEST_CASE("ResolveSeekPosition fails on an unrecognized whence value", "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(0, /*whence=*/999, /*current=*/100, /*length=*/1000) == -1);
}

TEST_CASE("ResolveSeekPosition SEEK_SET to exactly zero is not treated as a failure", "[StreamSeek]")
{
  // Regression guard: a naive "newPos <= 0 -> fail" check (instead of the
  // correct "< 0") would wrongly reject a perfectly valid seek to the very
  // start of the stream.
  CHECK(ResolveSeekPosition(0, SEEK_SET, /*current=*/500, /*length=*/1000) == 0);
}

TEST_CASE("ResolveSeekPosition fails instead of overflowing on a relative seek past INT64_MAX", "[StreamSeek]")
{
  // Regression: `currentPosition + position` used to be computed
  // unchecked -- signed overflow, undefined behavior (confirmed with
  // UBSan), before the "< 0" check ever ran.
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  CHECK(ResolveSeekPosition(kMax, SEEK_CUR, /*current=*/5, /*length=*/-1) == -1);
  CHECK(ResolveSeekPosition(10, SEEK_END, /*current=*/0, /*length=*/kMax) == -1);
  CHECK(ResolveSeekPosition(std::numeric_limits<int64_t>::min(), SEEK_CUR, /*current=*/-1, /*length=*/-1) == -1);
}

TEST_CASE("ResolveSeekPosition still reaches exactly INT64_MAX without overflowing", "[StreamSeek]")
{
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  CHECK(ResolveSeekPosition(kMax - 100, SEEK_CUR, /*current=*/100, /*length=*/-1) == kMax);
  CHECK(ResolveSeekPosition(0, SEEK_END, /*current=*/0, /*length=*/kMax) == kMax);
  CHECK(ResolveSeekPosition(kMax, SEEK_SET, /*current=*/0, /*length=*/-1) == kMax);
}

TEST_CASE("ComputeReadRangeEnd returns the inclusive last byte of an ordinary read", "[StreamSeek]")
{
  int64_t end = -1;
  REQUIRE(ComputeReadRangeEnd(0, 32768, end));
  CHECK(end == 32767);
  REQUIRE(ComputeReadRangeEnd(1000, 1, end));
  CHECK(end == 1000);
}

TEST_CASE("ComputeReadRangeEnd fails instead of overflowing near INT64_MAX", "[StreamSeek]")
{
  // Regression: ReadRecordingStream() computed `position + size - 1`
  // inline -- signed overflow (confirmed with UBSan) for a SEEK_SET
  // within one read's size of INT64_MAX, reachable whenever the
  // recording's own length is unknown (no length to clamp a seek to).
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  int64_t end = -1;
  CHECK_FALSE(ComputeReadRangeEnd(kMax - 10, 32768, end));
  CHECK_FALSE(ComputeReadRangeEnd(kMax, 1, end));
  // The last read that still fits: position + size == INT64_MAX exactly,
  // so the caller's own `position += bytesRead` can't overflow either.
  REQUIRE(ComputeReadRangeEnd(kMax - 32768, 32768, end));
  CHECK(end == kMax - 1);
}

TEST_CASE("ComputeReadRangeEnd rejects a negative position or a zero-byte read", "[StreamSeek]")
{
  int64_t end = -1;
  CHECK_FALSE(ComputeReadRangeEnd(-1, 100, end));
  CHECK_FALSE(ComputeReadRangeEnd(0, 0, end));
}

TEST_CASE("ResolveSeekPosition refuses SEEK_END when the reference length is unknown", "[StreamSeek]")
{
  // The unknown-length guard: without it a "-1" length made SEEK_END resolve to position + (-1) + offset.
  CHECK(ResolveSeekPosition(5, SEEK_END, 0, -1) == -1);
  CHECK(ResolveSeekPosition(0, SEEK_END, 100, -1) == -1);
  CHECK(ResolveSeekPosition(-5, SEEK_END, 0, 100) == 95);
}

TEST_CASE("ResolveSeekPosition SEEK_END against a known zero length is position zero, not an unknown length",
          "[StreamSeek]")
{
  CHECK(ResolveSeekPosition(0, SEEK_END, /*current=*/0, /*length=*/0) == 0);
  CHECK(ResolveSeekPosition(-1, SEEK_END, /*current=*/0, /*length=*/0) == -1);
  CHECK(ResolveSeekPosition(0, SEEK_END, /*current=*/0, /*length=*/-1) == -1);
}
