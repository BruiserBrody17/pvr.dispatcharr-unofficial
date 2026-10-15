#pragma once

#include <cstddef>

namespace dispatcharr
{

enum class SegmentCacheAction
{
  kNone,   // the cache already covers this stream
  kAppend, // append the stream's segments from `appendFrom` on
  kReseed  // rebuild the entry from the stream's whole list
};

struct SegmentCachePlan
{
  SegmentCacheAction action = SegmentCacheAction::kNone;
  size_t appendFrom = 0;
};

// The pure decision core of RefreshInProgressRecordingManifest()'s own cross-open segment-cache update
// (m_inProgressSegmentCache): how a stream that just probed `newlyProbedCount` segments, and now holds
// `streamCount` of them, brings its recording's cache entry (`cachedCount` segments) up to date.
//
// The entry is shared by every open stream of that recording, and a recording's segments are append-only and sized
// once, so the entry and each stream hold the same segments in the same order and only their lengths differ. That
// makes the cases:
//  - the entry is at least as long as the stream (another stream of the recording is ahead): nothing to do. Never
//    shrink it to the stream's length: a stream that is behind would otherwise cut the entry back on every probe and
//    the next open of the recording would re-probe what the stream ahead had already sized.
//  - the entry reaches at least as far as the stream's previous length: append what it is missing, from its own
//    length on. On a lone stream that is exactly this cycle's newly probed segments, the ordinary case.
//  - the entry stops short of the stream's previous length: reseed from the whole list. Real, confirmed gap found via
//    a project-wide review, not itself independently reproduced: `finished` briefly went true (erasing the entry) and
//    flipped back to false per ResolveInProgressFinished()'s non-sticky logic, and appending only this cycle's suffix
//    onto the empty entry dropped every earlier segment while totalBytes/totalDurationMs still claimed the full
//    recording, stranding any read into the missing region on a later open as a permanent gap.
inline SegmentCachePlan PlanInProgressSegmentCacheUpdate(size_t cachedCount, size_t streamCount,
                                                         size_t newlyProbedCount)
{
  if (cachedCount >= streamCount)
    return {SegmentCacheAction::kNone, 0};
  const size_t streamPreviousCount = newlyProbedCount <= streamCount ? streamCount - newlyProbedCount : 0;
  if (cachedCount < streamPreviousCount)
    return {SegmentCacheAction::kReseed, 0};
  return {SegmentCacheAction::kAppend, cachedCount};
}

} // namespace dispatcharr
