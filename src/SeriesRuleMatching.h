#pragma once

#include "DispatcharrClient.h"

#include <vector>

namespace dispatcharr
{

struct SeriesRuleMatchResult
{
  // Parallel to the `recordings` passed in: for each one, the index into
  // `rules` it matches (by channelId+title, the same identity
  // DeleteSeriesRule() uses), or -1 if it doesn't match any series rule
  // -- either because it's linked to a recurring rule instead
  // (recurringRuleId != 0), it's not currently upcoming/in-progress, or
  // genuinely no rule matches.
  std::vector<int> recordingRuleIndex;
  // Parallel to `rules`: the index into `recordings` of the earliest
  // upcoming/in-progress recording matching that rule, or -1 if none
  // matched yet.
  std::vector<int> ruleEarliestRecordingIndex;
};

// Pure matching core of PVRDispatcharr::GetTimers() -- a series rule has
// no fixed time of its own (it's an EPG-title match, not a schedule), so
// without tracking its earliest matching Recording, its own Kodi
// PVR_TIMER row had nothing real to show and defaulted to a
// zero-initialized StartTime/EndTime, which Kodi renders as the Unix
// epoch -- confirmed live as a real "12/31/1969" display bug, not a
// cosmetic nitpick. A series rule's own channelId + title is enough to
// identify which of its upcoming Recordings this is -- Dispatcharr's own
// rule identity (title+tvg_id+epg_source_id) already guarantees at most
// one rule per channel can share a title, so there's no realistic
// ambiguity. Only the first matching rule is ever recorded per recording
// (mirroring a plain linear "first match wins" scan), and only a
// recording that's currently upcoming or in-progress, with no
// recurringRuleId of its own, is considered a candidate at all.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_series_rule_matching.cpp.
SeriesRuleMatchResult MatchRecordingsToSeriesRules(const std::vector<Recording>& recordings,
                                                   const std::vector<TimerRule>& rules);

} // namespace dispatcharr
