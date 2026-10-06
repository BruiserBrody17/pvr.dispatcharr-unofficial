#include "UnprobeableSegment.h"

#include "M3u8SegmentParser.h"

namespace dispatcharr
{

bool ShouldProbeOnlyLeadingSegment(const UnprobeableSegmentTracker& tracker, const std::string& leadingPendingUrl)
{
  return tracker.consecutiveFailures > 0 && !tracker.url.empty() && tracker.url == leadingPendingUrl;
}

UnprobeableSegmentAction UpdateUnprobeableSegmentTracker(UnprobeableSegmentTracker& tracker,
                                                         const std::string& leadingUrl, bool leadingFailed,
                                                         std::chrono::steady_clock::time_point now)
{
  if (leadingUrl.empty() || !leadingFailed)
  {
    tracker = UnprobeableSegmentTracker();
    return UnprobeableSegmentAction::kKeepRetrying;
  }

  if (tracker.url != leadingUrl)
  {
    tracker.url = leadingUrl;
    tracker.consecutiveFailures = 1;
    tracker.firstFailedAt = now;
  }
  else
  {
    ++tracker.consecutiveFailures;
  }

  if (tracker.consecutiveFailures >= kUnprobeableSegmentMinFailures &&
      now - tracker.firstFailedAt >= kUnprobeableSegmentMinWait)
  {
    tracker = UnprobeableSegmentTracker();
    return UnprobeableSegmentAction::kGiveUp;
  }
  return UnprobeableSegmentAction::kKeepRetrying;
}

size_t IndexToArmTrackerAfterMerge(const std::vector<int64_t>& probedSizes, size_t mergedCount, bool leadingGivenUp,
                                   size_t probedCount)
{
  // Merged at least one segment, so the leading segment of this refresh answered (and the tracker, if it was armed
  // for it, was reset by that success); the first one that did not is the next refresh's leading segment.
  if (leadingGivenUp || mergedCount == 0 || mergedCount >= probedSizes.size() || mergedCount >= probedCount)
    return probedSizes.size();
  return probedSizes[mergedCount] <= 0 ? mergedCount : probedSizes.size();
}

size_t CountMergeableSegments(const std::vector<int64_t>& probedSizes, bool leadingGivenUp)
{
  if (!leadingGivenUp || probedSizes.empty())
    return CountLeadingProbedSegments(probedSizes);

  size_t count = 1;
  while (count < probedSizes.size() && probedSizes[count] > 0)
    ++count;
  return count;
}

} // namespace dispatcharr
