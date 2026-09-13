#include "SeriesRuleMatching.h"

namespace dispatcharr
{

SeriesRuleMatchResult MatchRecordingsToSeriesRules(const std::vector<Recording>& recordings,
                                                   const std::vector<TimerRule>& rules)
{
  SeriesRuleMatchResult result;
  result.recordingRuleIndex.assign(recordings.size(), -1);
  result.ruleEarliestRecordingIndex.assign(rules.size(), -1);

  for (std::size_t recIdx = 0; recIdx < recordings.size(); ++recIdx)
  {
    const Recording& rec = recordings[recIdx];
    if (!rec.isInProgress && !rec.isUpcoming)
      continue;
    if (rec.recurringRuleId != 0)
      continue; // linked to a recurring rule instead -- not a series-rule match candidate

    for (std::size_t ruleIdx = 0; ruleIdx < rules.size(); ++ruleIdx)
    {
      if (rules[ruleIdx].channelId == rec.channelId && rules[ruleIdx].title == rec.title)
      {
        result.recordingRuleIndex[recIdx] = static_cast<int>(ruleIdx);
        int& earliest = result.ruleEarliestRecordingIndex[ruleIdx];
        if (earliest < 0 || rec.startTime < recordings[static_cast<std::size_t>(earliest)].startTime)
          earliest = static_cast<int>(recIdx);
        break; // first matching rule wins, mirroring a plain linear scan
      }
    }
  }
  return result;
}

} // namespace dispatcharr
