#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

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

// Whether RefreshInProgressRecordingManifest() may take the recording lookup and playlist that another open stream of
// the same recording fetched, instead of asking the server again: Kodi 22 can hold two streams of one recording open,
// and each one refreshing on its own doubled the requests for that recording (measured with the glue harness's
// `ip_two_share`). `snapshotStartedAt` is when that fetch began (its lookup comes first, so nothing in it is older),
// `lastAppliedStartedAt` the same for the last fetch this stream used, its own or another's.
//  - Never one this stream has already used, its own included. A lone stream so refreshes exactly as before, and no
//    stream takes a playlist it has seen in place of asking for a newer one.
//  - No older than `maxAge` (the caller passes its throttle interval), forced refreshes included. A forced one (the
//    wait at the tail, SEEK_END, a cold start) that takes another stream's fetch is then never further behind than
//    that stream's own tail polling, which runs every 250 ms plus the refresh itself. A 250 ms limit for forced
//    refreshes missed most of those whenever a refresh took a little longer: 1.8x the requests of one stream at the
//    tail under AddressSanitizer, 1.0x to 1.2x with this one.
inline bool CanReuseSharedInProgressManifest(std::chrono::steady_clock::time_point snapshotStartedAt,
                                             std::chrono::steady_clock::time_point lastAppliedStartedAt,
                                             std::chrono::steady_clock::time_point now,
                                             std::chrono::steady_clock::duration maxAge)
{
  if (snapshotStartedAt <= lastAppliedStartedAt)
    return false;
  return now - snapshotStartedAt < maxAge;
}

// The sizes a recording's cross-open cache entry already holds for the playlist entries a refresh is about to probe
// (`pending`, the playlist's entries from index `alreadyKnown` on), -1 for each it does not: when another open stream
// of the recording sized them a moment ago, probing them again is one more HEAD per segment for nothing. An entry
// counts only at the same index with the same URL, and never a zero-byte placeholder (a segment given up on, see
// UnprobeableSegment.h): this stream probes that one itself rather than inherit another stream's give-up.
template <typename CachedSegment, typename PendingEntry>
std::vector<int64_t> SegmentSizesKnownToCache(const std::vector<CachedSegment>& cached,
                                              const std::vector<PendingEntry>& pending, size_t alreadyKnown)
{
  std::vector<int64_t> sizes(pending.size(), -1);
  for (size_t i = 0; i < pending.size(); ++i)
  {
    const size_t index = alreadyKnown + i;
    if (index >= cached.size())
      break;
    if (cached[index].url == pending[i].url && cached[index].byteSize > 0)
      sizes[i] = cached[index].byteSize;
  }
  return sizes;
}

} // namespace dispatcharr
