#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dispatcharr
{

// Cross-checks the addon's built-in zone table (TimeZoneUtil.cpp) against what Dispatcharr actually applies.
//
// A recurring rule is stored as a local time of day, and each occurrence the server materializes carries the real UTC
// instant it computed from it, so the pair says which UTC offset the server used for that day: a rule at 19:30 whose
// occurrence starts at 10:30 UTC was scheduled under UTC+9. The table can be wrong where the server's tz data is
// older or newer than the table's (British Columbia and Alberta stop changing their clocks on 2026-11-01; a server
// image with tz data from before 2026c still changes them -- docs/OPEN_ITEMS.md, closed 2026-10-06), and a rule made
// from a wrong offset records an hour off. An occurrence is not padded: Dispatcharr creates it straight from the
// rule's own start and end (confirmed against its recurring-rule scheduler, docs/RECORDINGS.md).
struct OffsetSample
{
  int ruleStartTimeOfDaySeconds = 0; // Dispatcharr-local, as the rule stores it
  time_t occurrenceStartUtc = 0;
};

// The UTC offset in minutes that turns occurrenceStartUtc into the rule's local start time of day, as the one closest
// to `nearOffsetMinutes` (the table's own answer): a time of day alone fixes an offset only modulo a whole day (+13 h
// and -11 h read the same), and the table is what says which of the two is meant. Both times come from the same
// instant, so the result is exact whenever the true offset is within half a day of the table's; a seconds difference
// between the two times (the occurrence carries seconds the rule does not) is rounded to the nearest minute (toward
// zero was wrong east of UTC: 59 for a +60 offset when the occurrence carries 28 seconds).
int ImpliedOffsetMinutes(int ruleStartTimeOfDaySeconds, time_t occurrenceStartUtc, int nearOffsetMinutes);

struct ServerOffsetVerdict
{
  enum class Kind
  {
    kNoEvidence, // nothing usable, or samples that contradict each other: leave things as they are
    kAgrees,     // at least one sample matches the table
    kDisagrees,  // every usable sample says the same offset, and it is not the table's
  };
  Kind kind = Kind::kNoEvidence;
  int impliedOffsetMinutes = 0; // meaningful for kDisagrees
  int tableOffsetMinutes = 0;   // meaningful for kDisagrees: what the table said at those samples
  // meaningful for kAgrees: the distinct offsets the table gave at the samples that agreed with it
  std::vector<int> agreeingTableOffsets;
};

// The most a server's offset can plausibly differ from the table's: one hour, a clock change the table does not (or
// no longer does) know about. A larger apparent difference is far more likely to be a rule whose time was edited
// while its occurrences still carry the old one, so such a sample is not evidence of anything.
constexpr int kMaxPlausibleOffsetDeltaMinutes = 60;

// Judges the table (`tableOffsetMinutesAt(utc, offsetOut)` returns false when the zone is unknown; disagreement is a
// different offset, not a different day) against the samples. A sample taken within a day of a clock change in the
// table is skipped: a local time that does not exist (a spring-forward gap) or exists twice (fall-back) is resolved by
// the server in a way that reads as the other offset without anything being wrong, and so is a sample implying more
// than kMaxPlausibleOffsetDeltaMinutes away from the table. Any agreeing sample wins (the table is right at least
// where it was looked at, and one odd occurrence must not override it); disagreement needs every usable sample to name
// the same other offset against the same table offset.
ServerOffsetVerdict CrossCheckServerOffset(const std::function<bool(time_t, int&)>& tableOffsetMinutesAt,
                                           const std::vector<OffsetSample>& samples);

// One materialized occurrence of a recurring rule, as far as the check needs it.
struct OccurrenceRef
{
  int recurringRuleId = 0;
  time_t startTime = 0;
};

// The samples to judge: only occurrences that have not started yet (what the server will record, so the offset a rule
// made or edited now will be scheduled with -- an occurrence from before the server's clock changed would otherwise
// outvote the ones after it for days or weeks, and a rule whose time was just edited still has its old occurrences in
// the past), whose rule is known, the `maxSamples` soonest.
std::vector<OffsetSample> SelectOffsetSamples(const std::vector<OccurrenceRef>& occurrences,
                                              const std::map<int, int>& ruleStartTimeOfDaySeconds, time_t now,
                                              size_t maxSamples);

// How the server's clock differs from the table.
struct ServerOffsetOverride
{
  enum class Mode
  {
    kNone,
    // The server applies the zone's rules as they were before the table's permanent-daylight change (an image with
    // tz data from before 2026c): resolved per instant by the table with that change left out.
    kLegacyZoneData,
    // Anything else: a fixed difference from the table, valid where the table gives `tableOffsetMinutes` again.
    kConstantDelta,
  };
  Mode mode = Mode::kNone;
  int deltaMinutes = 0;
  int tableOffsetMinutes = 0;
  bool operator==(const ServerOffsetOverride& o) const
  {
    return mode == o.mode && deltaMinutes == o.deltaMinutes && tableOffsetMinutes == o.tableOffsetMinutes;
  }
  bool operator!=(const ServerOffsetOverride& o) const
  {
    return !(*this == o);
  }
};

// The offset to use for an instant, given the table's answer for it and the legacy table's.
int ApplyServerOffsetOverride(const ServerOffsetOverride& ov, int tableOffsetMinutes, int legacyOffsetMinutes);

struct ServerOffsetJudgement
{
  ServerOffsetVerdict::Kind kind = ServerOffsetVerdict::Kind::kNoEvidence;
  ServerOffsetOverride proposed; // meaningful for kDisagrees
  // What the table with its recent rule change left out made of the same samples, and the table offsets the agreeing
  // samples were at: what decides whether an agreeing table contradicts an override already in force.
  ServerOffsetVerdict::Kind legacyKind = ServerOffsetVerdict::Kind::kNoEvidence;
  std::vector<int> agreeingTableOffsets;
};

// CrossCheckServerOffset() against the table and against the legacy table, to say which kind of difference a
// disagreement is. A disagreement the legacy table has no opinion on (every sample within a day of the legacy table's
// own clock change, as on the day a zone's rules differ) is no evidence at all: guessing a constant difference there
// put every summer date an hour off for most of one day.
ServerOffsetJudgement JudgeServerOffset(const std::function<bool(time_t, int&)>& tableOffsetMinutesAt,
                                        const std::function<bool(time_t, int&)>& legacyOffsetMinutesAt,
                                        const std::vector<OffsetSample>& samples);

// What one judgement changed.
enum class ServerOffsetChange
{
  kNone,
  kActivated, // an override came into force
  kChanged,   // a different one replaced it
  kCleared,   // it no longer applies (the table agrees again, the zone changed or went manual)
};

// The override in force and the candidate that has been seen but not yet confirmed.
struct ServerOffsetTracker
{
  std::string zone;
  ServerOffsetOverride active;
  ServerOffsetOverride pending;
  int pendingCount = 0;
};

// An override is adopted only when two evaluations in a row propose it: one bad snapshot (a stale cache holding a
// rule's new time beside its old occurrences, say) must not move every recurring rule.
constexpr int kServerOffsetConfirmations = 2;

// `zone` is the recurring_rule_timezone setting ("manual" when it is not a zone). A change of zone discards
// everything learnt about the previous one (and the new zone's first judgement still counts). An agreeing table clears
// an override only when the agreement contradicts it: a legacy-zone override stays while the legacy table agrees too (a
// server on older rules and the current table give the same answer all summer), a constant difference only while
// samples at the table offset it was measured against agree. No evidence leaves things as they are.
ServerOffsetChange UpdateServerOffsetTracker(ServerOffsetTracker& tracker, const std::string& zone,
                                             const ServerOffsetJudgement& judgement);

// "UTC-5:30", "UTC+1:00": the sign belongs to the whole offset, not its hour part (-30 minutes is not "+0:30").
std::string FormatUtcOffsetMinutes(int offsetMinutes);

} // namespace dispatcharr
