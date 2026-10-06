#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dispatcharr
{

// The decision core of what RefreshInProgressRecordingManifest() does about an
// in-progress recording's playlist entry whose size it cannot get.
//
// A segment is merged into the stream only once every segment before it is
// (cumulative byte/time offsets need complete, in-order sizes -- see
// CountLeadingProbedSegments()), so a segment that never probes holds back
// everything after it. Reproduced live (docs/RECORDINGS.md, "Two live checks of
// the in-progress read path"): with one listed segment answering its HEAD with a
// zero length, playback stopped at that segment for good, the addon sent ~14
// HEADs a second (every refresh re-probed the whole unmerged tail, only to throw
// the sizes away again behind the first failure), a blocking read ran its full
// 200-255 s catch-up budget, and -- since every probe is a `.ts` request that
// refreshes the server's viewer key -- the finished recording stayed in
// "recording" status for as long as the stuck stream was open.
//
// Two measures, both here so they are testable without a server:
//   * while the leading unmerged segment keeps failing, probe that one alone
//     (ShouldProbeOnlyLeadingSegment) -- nothing behind it can be merged anyway;
//   * after kUnprobeableSegmentMinFailures consecutive failed refreshes spanning at
//     least kUnprobeableSegmentMinWait, stop waiting on it and merge it as a
//     zero-byte placeholder (UpdateUnprobeableSegmentTracker -> kGiveUp,
//     CountMergeableSegments). A placeholder keeps every later offset exact and
//     is never mistaken for the byte-offset bug the leading-merge rule exists
//     for (it occupies no bytes, so no stream position falls inside it); the
//     cost is a few seconds of missing picture where the segment was.
//
// The wait is long enough that a segment briefly absent while the server
// finishes writing it never trips it, and short enough that the stall it ends
// is bounded. Refreshes only reach the probe at all when the playlist fetch
// itself succeeded, so a server that is down never counts toward it.
constexpr int kUnprobeableSegmentMinFailures = 5;
constexpr std::chrono::seconds kUnprobeableSegmentMinWait{30};

struct UnprobeableSegmentTracker
{
  std::string url; // the leading segment that failed last refresh; empty = none
  int consecutiveFailures = 0;
  std::chrono::steady_clock::time_point firstFailedAt{};
};

enum class UnprobeableSegmentAction
{
  kKeepRetrying,
  kGiveUp,
};

// True when this refresh should size only `leadingPendingUrl` rather than the
// whole unmerged tail: it is the segment that failed on the previous refresh.
bool ShouldProbeOnlyLeadingSegment(const UnprobeableSegmentTracker& tracker, const std::string& leadingPendingUrl);

// Feed this refresh's outcome for the leading unmerged segment (`leadingUrl`
// empty when nothing was pending). Resets the tracker whenever the leading
// segment answered or there is none, counts consecutive failures of one URL
// otherwise, and reports kGiveUp -- resetting the tracker too -- once both
// thresholds are met.
UnprobeableSegmentAction UpdateUnprobeableSegmentTracker(UnprobeableSegmentTracker& tracker,
                                                         const std::string& leadingUrl, bool leadingFailed,
                                                         std::chrono::steady_clock::time_point now);

// After a refresh merged `mergedCount` leading segments: the index of the first segment that could not be sized, when
// it is now the leading one of the next refresh and the tracker did not already cover it (the leading segment itself
// answered, a later one did not), or `probedSizes.size()` for none. Only the first `probedCount` entries were actually
// probed (a refresh that probes the leading segment alone leaves the rest at their -1 initialiser, which is "not
// probed", not "failed"): reading those as failures re-armed the tracker for the next segment on every refresh, so a
// backlog merged one segment per refresh after a single transient probe failure (found by the fifteenth hardening
// sweep: 400 segments took 11 s to merge 22, instead of 1 s for all). A cold open of a long backlog with one unsizeable
// segment in the middle used to leave the tracker unarmed, so the next refresh probed the whole tail again and threw
// the sizes away -- 6249 HEADs in 40 s for a 2000-segment backlog with one failing segment (found by the fourteenth
// hardening sweep).
size_t IndexToArmTrackerAfterMerge(const std::vector<int64_t>& probedSizes, size_t mergedCount, bool leadingGivenUp,
                                   size_t probedCount);

// How many of the pending segments to merge: CountLeadingProbedSegments(), plus
// the leading one itself when it is being given up on (it then counts even though
// its probed size is not positive, and the entries behind it are judged as usual).
size_t CountMergeableSegments(const std::vector<int64_t>& probedSizes, bool leadingGivenUp);

} // namespace dispatcharr
