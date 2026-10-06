#pragma once

#include <cstdint>

namespace dispatcharr
{

// The byteOffset/timeOffsetMs a newly-discovered segment gets when it's
// appended to a stream's own cumulative, fixed-origin address space (see
// SegmentLookup.h's own comment on why offsets are cumulative rather than
// per-fetch-relative).
struct SegmentAppendOffsets
{
  int64_t byteOffset;
  int64_t timeOffsetMs;
};

// Shared by RefreshInProgressRecordingManifest() and RefreshLiveManifest():
// each, once per newly-discovered segment, assigns that segment's own
// offsets at the current end of the stream's running totals, then advances
// those totals to include it -- identical bookkeeping in both, just against
// each stream's own differently-typed segment info struct (so this returns
// the offsets rather than writing directly into one). Pulled out
// specifically so this exact arithmetic can't drift apart between the two
// call sites the way the loops it lived in once did before either was ever
// shared; see ../tests/test_segment_append_offsets.cpp.
inline SegmentAppendOffsets AppendSegmentOffsets(int64_t sizeBytes, int64_t durationMs, int64_t& totalBytesInOut,
                                                 int64_t& totalDurationMsInOut)
{
  SegmentAppendOffsets offsets{totalBytesInOut, totalDurationMsInOut};
  totalBytesInOut += sizeBytes;
  totalDurationMsInOut += durationMs;
  return offsets;
}

} // namespace dispatcharr
