#include "SeriesRuleMatching.h"

#include "SeriesRuleTextMatch.h"
#include "UnicodeText.h"

#include <string>

namespace dispatcharr
{

namespace
{

// A rule's title/description as Dispatcharr's own evaluation actually uses
// them -- stripped with Python's str.strip(), then read under the mode the
// rule carries -- computed once up front rather than per recording. See
// MatchRecordingsToSeriesRules()'s own header comment.
struct PreparedRule
{
  // False for a rule with neither a title nor a description, which the
  // server treats as invalid and never evaluates ("invalid_rule").
  bool evaluable = false;
  std::string title;
  SeriesTextMode titleMode = SeriesTextMode::kExact;
  std::string description;
  SeriesTextMode descriptionMode = SeriesTextMode::kContains;
};

PreparedRule PrepareRule(const TimerRule& rule)
{
  PreparedRule prepared;
  prepared.title = StripUnicodeWhitespace(rule.title);
  prepared.description = StripUnicodeWhitespace(rule.description);
  prepared.evaluable = !prepared.title.empty() || !prepared.description.empty();
  prepared.titleMode = ParseSeriesTextMode(rule.titleMode, /*isTitle=*/true);
  prepared.descriptionMode = ParseSeriesTextMode(rule.descriptionMode, /*isTitle=*/false);
  return prepared;
}

// Both of a rule's filters must hold, and either one being something this
// addon can't evaluate (a regex) leaves the rule unlinked -- the same state
// every non-exact rule used to be in, rather than a guess.
bool RuleMatchesRecording(const PreparedRule& rule, const Recording& rec)
{
  if (!rule.evaluable)
    return false;
  if (!rule.title.empty() && MatchSeriesText(rule.titleMode, rule.title, rec.title) != SeriesTextMatch::kMatches)
    return false;
  if (!rule.description.empty() &&
      MatchSeriesText(rule.descriptionMode, rule.description, rec.description) != SeriesTextMatch::kMatches)
    return false;
  return true;
}

} // namespace

SeriesRuleMatchResult MatchRecordingsToSeriesRules(const std::vector<Recording>& recordings,
                                                   const std::vector<TimerRule>& rules)
{
  SeriesRuleMatchResult result;
  result.recordingRuleIndex.assign(recordings.size(), -1);
  result.ruleEarliestRecordingIndex.assign(rules.size(), -1);

  std::vector<PreparedRule> preparedRules;
  preparedRules.reserve(rules.size());
  for (const TimerRule& rule : rules)
    preparedRules.push_back(PrepareRule(rule));

  for (std::size_t recIdx = 0; recIdx < recordings.size(); ++recIdx)
  {
    const Recording& rec = recordings[recIdx];
    if (!rec.isInProgress && !rec.isUpcoming)
      continue;
    if (rec.recurringRuleId != 0)
      continue; // linked to a recurring rule instead -- not a series-rule match candidate

    // Two passes, so a rule pinned to this recording's own channel is preferred
    // over a channel-less one that would also match it: the pinned rule is the
    // more specific, and the server treats them as separate rules either way.
    for (int pass = 0; pass < 2 && result.recordingRuleIndex[recIdx] < 0; ++pass)
    {
      for (std::size_t ruleIdx = 0; ruleIdx < rules.size(); ++ruleIdx)
      {
        const TimerRule& rule = rules[ruleIdx];
        const bool channelLess = rule.channelId <= 0;
        // Pass 0: same channel. Pass 1: a rule with no pinned channel (what
        // Dispatcharr's own guide "Record series" button creates) can never
        // equal a recording's channel; the identity it has instead is its
        // tvg_id, carried on the recording's own programme snapshot, and an
        // empty one means "any programme with this title, on any channel".
        const bool sameScope = pass == 0 ? rule.channelId == rec.channelId
                                         : channelLess && (rule.tvgId.empty() || rule.tvgId == rec.programTvgId);
        // The rule's own filters -- see this function's header comment for
        // which of Dispatcharr's own matching modes that covers and why each
        // comparison is case-insensitive the way it is.
        if (sameScope && RuleMatchesRecording(preparedRules[ruleIdx], rec))
        {
          result.recordingRuleIndex[recIdx] = static_cast<int>(ruleIdx);
          int& earliest = result.ruleEarliestRecordingIndex[ruleIdx];
          if (earliest < 0 || rec.startTime < recordings[static_cast<std::size_t>(earliest)].startTime)
            earliest = static_cast<int>(recIdx);
          break; // first matching rule wins, mirroring a plain linear scan
        }
      }
    }
  }
  return result;
}

} // namespace dispatcharr
