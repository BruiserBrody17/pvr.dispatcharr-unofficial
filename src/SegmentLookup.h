#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dispatcharr
{

// Shared by DispatcharrClient::ReadInProgressRecordingStream() and
// ReadLiveTimeshiftStream(): finds the segment whose [byteOffset,
// byteOffset + byteSize) range contains `position` in that stream's own
// cumulative, fixed-origin address space (see each segment type's own
// comment on why offsets are cumulative rather than per-fetch-relative).
// Returns nullptr if `position` falls in a gap (shouldn't happen -- no
// rolling-window eviction on either stream -- but nothing is safely
// readable there if it somehow did) or past every known segment.
//
// SegmentT only needs byteOffset/byteSize members (duck-typed via the
// template, works for both LiveTimeshiftSegmentInfo and
// InProgressRecordingSegmentInfo without depending on either) -- pulled
// out here, rather than left as the identical loop duplicated in both
// Read*Stream() functions, specifically so it's unit-testable standalone;
// see ../tests/test_segment_lookup.cpp.
template <typename SegmentT>
const SegmentT* FindSegmentContainingPosition(int64_t position, const std::vector<SegmentT>& segments)
{
  for (const auto& s : segments)
  {
    if (position >= s.byteOffset && position < s.byteOffset + s.byteSize)
      return &s;
  }
  return nullptr;
}

} // namespace dispatcharr
