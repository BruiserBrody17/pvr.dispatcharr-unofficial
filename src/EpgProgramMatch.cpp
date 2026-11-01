#include "EpgProgramMatch.h"

#include <algorithm>
#include <cstdint>

namespace dispatcharr
{

namespace
{
// The share of the recording the best entry must cover: 4/5, compared as integers. A double ratio against 0.8 is not
// exact, and where doubles are computed in extended precision (32-bit x86's x87 unit) an overlap of exactly 80% came
// out just under the constant and failed the match; found by the 32-bit test job. `overlap >= duration - duration / 5`
// is exactly `5 * overlap >= 4 * duration` for whole numbers, without products that a span near the 64-bit limit
// would overflow.
bool CoversEnough(int64_t overlap, int64_t duration)
{
  return overlap >= duration - duration / 5;
}
} // namespace

// Differences are taken in 64 bits: on a 32-bit time_t two representable times can be further apart than a time_t
// holds (a garbage start date clamped to kEarliestRepresentableTime, TimeUtil.h, against today), and that subtraction
// overflowed (docs/CLOSED_ITEMS.md, "Windows (and 32-bit) parse a pre-1970 (pre-1901) date to a floor that later
// subtractions overflow").
int FindEpgEntryIndexCoveringRecording(const std::vector<EpgEntry>& entries, time_t recStart, time_t recEnd)
{
  const int64_t recDuration = static_cast<int64_t>(recEnd) - static_cast<int64_t>(recStart);
  if (recDuration <= 0)
    return -1;

  int bestIdx = -1;
  int64_t bestOverlap = 0;
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    const EpgEntry& entry = entries[i];
    if (entry.startTime >= recEnd || entry.endTime <= recStart)
      continue; // no overlap at all

    time_t overlapStart = std::max(recStart, entry.startTime);
    time_t overlapEnd = std::min(recEnd, entry.endTime);
    const int64_t overlap = static_cast<int64_t>(overlapEnd) - static_cast<int64_t>(overlapStart);
    if (overlap > bestOverlap)
    {
      bestOverlap = overlap;
      bestIdx = static_cast<int>(i);
    }
  }

  if (bestIdx >= 0 && CoversEnough(bestOverlap, recDuration))
    return bestIdx;
  return -1;
}

int FindEpgEntryIndexByStartTime(const std::vector<EpgEntry>& entries, time_t targetStartTime, int toleranceSeconds)
{
  int bestIdx = -1;
  int64_t bestDiff = 0;
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    const int64_t start = entries[i].startTime;
    const int64_t target = targetStartTime;
    const int64_t diff = start > target ? start - target : target - start;
    if (diff > toleranceSeconds)
      continue;
    if (bestIdx < 0 || diff < bestDiff)
    {
      bestDiff = diff;
      bestIdx = static_cast<int>(i);
    }
  }
  return bestIdx;
}

EpgOverlapWindow ResolveEpgOverlapWindow(time_t recStartTime, time_t recEndTime, time_t programStartTime,
                                         time_t programEndTime)
{
  if (programStartTime > 0 && programEndTime > programStartTime)
    return {programStartTime, programEndTime};
  return {recStartTime, recEndTime};
}

} // namespace dispatcharr
