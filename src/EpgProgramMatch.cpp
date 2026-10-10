#include "EpgProgramMatch.h"

#include <algorithm>
#include <cstdint>

namespace dispatcharr
{

namespace
{
// The share of the recording the best entry must cover: 4/5, compared as integers. A double ratio against 0.8 is not
// exact, and where doubles are computed in extended precision (32-bit x86's x87 unit) an overlap of exactly 80% came
// out just under the constant and failed the match; found by the 32-bit test job.
constexpr int64_t kMinOverlapNumerator = 4;
constexpr int64_t kMinOverlapDenominator = 5;
} // namespace

int FindEpgEntryIndexCoveringRecording(const std::vector<EpgEntry>& entries, time_t recStart, time_t recEnd)
{
  time_t recDuration = recEnd - recStart;
  if (recDuration <= 0)
    return -1;

  int bestIdx = -1;
  time_t bestOverlap = 0;
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    const EpgEntry& entry = entries[i];
    if (entry.startTime >= recEnd || entry.endTime <= recStart)
      continue; // no overlap at all

    time_t overlapStart = std::max(recStart, entry.startTime);
    time_t overlapEnd = std::min(recEnd, entry.endTime);
    time_t overlap = overlapEnd - overlapStart;
    if (overlap > bestOverlap)
    {
      bestOverlap = overlap;
      bestIdx = static_cast<int>(i);
    }
  }

  if (bestIdx >= 0 && static_cast<int64_t>(bestOverlap) * kMinOverlapDenominator >=
                          static_cast<int64_t>(recDuration) * kMinOverlapNumerator)
    return bestIdx;
  return -1;
}

int FindEpgEntryIndexByStartTime(const std::vector<EpgEntry>& entries, time_t targetStartTime, int toleranceSeconds)
{
  int bestIdx = -1;
  time_t bestDiff = 0;
  for (std::size_t i = 0; i < entries.size(); ++i)
  {
    time_t diff = entries[i].startTime > targetStartTime ? entries[i].startTime - targetStartTime
                                                         : targetStartTime - entries[i].startTime;
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
