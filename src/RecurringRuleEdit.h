#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace dispatcharr
{

// What UpdateTimer() sends when Kodi's timer dialog saves a recurring rule, built
// from the *difference* between what the user was shown and what they saved.
//
// Before, every edit PATCHed all seven fields from Kodi's own copy of the timer --
// a copy only as fresh as the addon's last GetTimers() refresh (5 minutes by
// default) -- so a change made to any other field from Dispatcharr's web UI or a
// second Kodi in that window was silently reverted by an edit as narrow as an
// enable/disable toggle, and every edit also made the server drop and regenerate
// every future occurrence whether or not anything it depends on had changed
// (docs/OPEN_ITEMS.md, "Recurring-rule edits send back Kodi's possibly-stale cached
// fields"). Now only what the user actually changed is sent, and a save that
// changed nothing sends nothing.
struct RecurringRuleFields
{
  int channelId = 0;
  std::string name;
  std::vector<int> daysOfWeek;
  int startTimeOfDaySeconds = 0;
  int endTimeOfDaySeconds = 0;
  time_t startDate = 0;
  bool enabled = true;
};

struct RecurringRuleEditPatch
{
  std::optional<int> channelId;
  std::optional<std::string> name;
  std::optional<std::vector<int>> daysOfWeek;
  std::optional<int> startTimeOfDaySeconds;
  std::optional<int> endTimeOfDaySeconds;
  std::optional<time_t> startDate;
  std::optional<bool> enabled;
  // Only ever set to give an open-ended rule an end date -- see
  // ComputeEndDateForOpenEndedRuleEdit(). Never a plain edit of an existing one.
  std::optional<time_t> endDate;

  bool IsEmpty() const
  {
    return !channelId && !name && !daysOfWeek && !startTimeOfDaySeconds && !endTimeOfDaySeconds && !startDate &&
           !enabled && !endDate;
  }
};

// Every field of `edited` that differs from `baseline`. With no baseline (the
// rule is not in the cache, so nothing says what the user changed) every field is
// included, which is the previous behaviour. Start dates compare by calendar day:
// they cross the wire as "YYYY-MM-DD", so two instants in one day are one value.
// Times of day compare modulo one day for the same reason (they cross as
// "HH:MM:SS"): a zone shift can express the edited side as a negative or >24h
// value the server would never report.
// The start and end times of day an edit should be diffed with. Kodi hands back the start and end
// instants it was shown; re-deriving a Dispatcharr time of day from them is not always the inverse of the
// display: a rule whose start time falls in the zone's skipped spring-forward hour on its own start date
// is DISPLAYED an hour later than stored, and re-deriving from that instant gave a time an hour off, so an
// unchanged save (even the enabled toggle) sent both times and the server regenerated every occurrence an
// hour away for good (found by the 2026-10-04 eighth hardening sweep: 967 of 658,398 round trips, every
// one in a nonexistent hour). When both instants are exactly what GetTimers() reported, the user did not
// touch them, so the stored times of day are used as they are; otherwise the derived values stand.
struct RuleTimesOfDay
{
  int startSeconds = 0;
  int endSeconds = 0;
};
RuleTimesOfDay ResolveRecurringRuleTimesOnEdit(const std::optional<RecurringRuleFields>& baseline, time_t timerStart,
                                               time_t timerEnd, const std::optional<time_t>& reportedStart,
                                               const std::optional<time_t>& reportedEnd, int derivedStartSeconds,
                                               int derivedEndSeconds);

RecurringRuleEditPatch ComputeRecurringRuleEditPatch(const std::optional<RecurringRuleFields>& baseline,
                                                     const RecurringRuleFields& edited);

// The end date to send so an edit of an open-ended rule (no `end_date` on the
// server) is accepted, or 0 when none is needed. Dispatcharr's serializer refuses
// any save of a rule that has no end date and is not being given one ("End date is
// required"), even a bare enable/disable toggle -- and such a rule is reachable: a
// PATCH of `{"end_date": null}` is accepted, while a create without one is refused
// (confirmed live 2026-09-30, docs/RECURRING_RULES.md). The rule gets the same
// rolling window a rule created from Kodi does, so the addon renews it like any
// other; that turns "records until you stop it" into "records while a Kodi running
// this addon renews it", which is the cost of being editable at all.
// `currentEndDate` is the rule's end date as the server last reported it (0 = none),
// `startDate` the start date the rule will have after the edit.
time_t ComputeEndDateForOpenEndedRuleEdit(time_t currentEndDate, time_t startDate, time_t now, int windowDays);

} // namespace dispatcharr
