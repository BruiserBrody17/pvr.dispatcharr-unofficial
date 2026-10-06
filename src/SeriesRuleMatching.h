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
// A rule's own `title`/`description` are read the way Dispatcharr's own
// evaluation reads them (apps/channels/tasks.py's
// `evaluate_series_rules_impl()`, whose per-rule loop lives in
// `_evaluate_series_rules_locked()`; confirmed against its real 0.31.0
// source) -- see SeriesRuleTextMatch.h for the modes: "exact" is a
// case-insensitive whole-string comparison (`title__iexact`), every other
// title_mode except "regex" and "search" goes through the same
// AND/OR/quote/parenthesis substring grammar the description always does,
// "search" anchors each term on word boundaries, and a rule's title and
// description filters are ANDed. A rule with neither is invalid to the server
// and never matches anything here either. A regex filter isn't evaluated --
// its PostgreSQL syntax isn't ECMAScript's -- so that rule stays unlinked
// rather than being guessed at, which is exactly how every non-exact rule
// used to behave.
//
// Both the stripping and the case folding are Unicode-aware
// (UnicodeText.h), because the server's are. Python's `.strip()` also strips
// a no-break space and the other Unicode whitespace, and the rule is stored
// unstripped -- a trailing no-break space survives in the API response -- so
// an ASCII-only strip left a rule that recorded correctly but never linked
// back. And `UPPER()` on the real instance's database folds non-ASCII case
// (confirmed live: an `icontains` for "ünder" matched a channel named
// "ÜNDER"), so a title differing from its programmes' only in the case of an
// accented letter matched on the server and must here too. Both were
// confirmed live against this addon as unlinked rules that Kodi shows as
// "Any day at any time".
//
// The first two rounds of that history, kept because each is a real
// mismatch found and fixed here: the comparison used to be case-sensitive
// (fixed in a project-wide review against `title__iexact`), and then
// ASCII-whitespace-stripped only (a 68th-pass audit). Only the rule side is
// ever stripped, not the recording's: the server compares its stripped rule
// title against each programme's own raw, unstripped title, so stripping the
// recording's title too would link recordings the server itself never
// matched.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_series_rule_matching.cpp.
SeriesRuleMatchResult MatchRecordingsToSeriesRules(const std::vector<Recording>& recordings,
                                                   const std::vector<TimerRule>& rules);

} // namespace dispatcharr
