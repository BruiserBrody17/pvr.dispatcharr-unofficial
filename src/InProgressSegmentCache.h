#pragma once

#include <cstddef>

namespace dispatcharr
{

// The pure decision core of RefreshInProgressRecordingManifest()'s own
// cross-open segment-cache update (m_inProgressSegmentCache) -- whether
// this cycle's cache entry must be reseeded from the full current segment
// list rather than having just this cycle's newly-probed segments appended
// to it.
//
// False (append is safe) on every ordinary refresh cycle: the cache
// entry's own segment count plus this cycle's newly-probed count always
// exactly equals the stream's current total, since the cache entry is
// always exactly that stream's own prefix, seeded from it verbatim on
// open and appended to in lockstep since.
//
// True only after `finished` briefly went true (erasing this entry
// entirely) and then flipped back to false again, per
// ResolveInProgressFinished()'s own non-sticky isInProgress/
// allProbesSucceeded logic -- a real, confirmed gap found via a
// project-wide review, not itself independently reproduced: appending
// just this cycle's suffix onto the freshly-recreated (empty) entry would
// silently drop every earlier segment from it, and
// OpenInProgressRecordingStream() seeds the stream's own segment list
// straight from this cache on a later reopen (channel switch and back,
// pause/resume) -- so that reopen would be missing its own earlier
// segments entirely while totalBytes/totalDurationMs (also copied from
// this same entry) still claim the full recording length, stranding any
// read/seek into that now-missing region as a permanent "gap".
inline bool ShouldReseedInProgressSegmentCache(size_t cachedSegmentCount, size_t newlyProbedSegmentCount,
                                               size_t currentSegmentCount)
{
  return cachedSegmentCount + newlyProbedSegmentCount != currentSegmentCount;
}

} // namespace dispatcharr
