#include "RecurringRuleEdit.h"

#include <algorithm>

namespace dispatcharr
{

namespace
{
bool SameCalendarDay(time_t a, time_t b)
{
  return a / 86400 == b / 86400;
}

// A time of day crosses the wire as "HH:MM:SS" (TimeOfDayString() wraps it),
// so two values 86400 apart are one value. The edited side can arrive
// unwrapped from a zone shift (a 22:00 rule in a zone behind UTC as -7200)
// while the server reports 79200 -- compared raw, an unchanged rule was sent
// on every edit (docs/OPEN_ITEMS.md, "Unchanged recurring-rule times were
// re-sent on every edit for a rule near UTC midnight").
bool SameTimeOfDay(int a, int b)
{
  auto wrap = [](int s) { return ((s % 86400) + 86400) % 86400; };
  return wrap(a) == wrap(b);
}
} // namespace

RuleTimesOfDay ResolveRecurringRuleTimesOnEdit(const std::optional<RecurringRuleFields>& baseline, time_t timerStart,
                                               time_t timerEnd, const std::optional<time_t>& reportedStart,
                                               const std::optional<time_t>& reportedEnd, int derivedStartSeconds,
                                               int derivedEndSeconds)
{
  // Each side on its own: moving only the end must not drag the untouched start through the
  // re-derivation, which is an hour off for a start inside a spring-forward gap (found by the ninth
  // hardening sweep; the first version needed both instants unchanged).
  RuleTimesOfDay out{derivedStartSeconds, derivedEndSeconds};
  if (baseline && reportedStart && timerStart == *reportedStart)
    out.startSeconds = baseline->startTimeOfDaySeconds;
  if (baseline && reportedEnd && timerEnd == *reportedEnd)
    out.endSeconds = baseline->endTimeOfDaySeconds;
  return out;
}

RecurringRuleEditPatch ComputeRecurringRuleEditPatch(const std::optional<RecurringRuleFields>& baseline,
                                                     const RecurringRuleFields& edited)
{
  RecurringRuleEditPatch patch;
  if (!baseline)
  {
    patch.channelId = edited.channelId;
    patch.name = edited.name;
    patch.daysOfWeek = edited.daysOfWeek;
    patch.startTimeOfDaySeconds = edited.startTimeOfDaySeconds;
    patch.endTimeOfDaySeconds = edited.endTimeOfDaySeconds;
    patch.startDate = edited.startDate;
    patch.enabled = edited.enabled;
    return patch;
  }

  if (edited.channelId != baseline->channelId)
    patch.channelId = edited.channelId;
  if (edited.name != baseline->name)
    patch.name = edited.name;
  // Compared as sets: Dispatcharr stores the days sorted and de-duplicated, so a
  // reordered list from Kodi is the same rule.
  std::vector<int> editedDays = edited.daysOfWeek;
  std::vector<int> baselineDays = baseline->daysOfWeek;
  std::sort(editedDays.begin(), editedDays.end());
  editedDays.erase(std::unique(editedDays.begin(), editedDays.end()), editedDays.end());
  std::sort(baselineDays.begin(), baselineDays.end());
  baselineDays.erase(std::unique(baselineDays.begin(), baselineDays.end()), baselineDays.end());
  if (editedDays != baselineDays)
    patch.daysOfWeek = edited.daysOfWeek;
  if (!SameTimeOfDay(edited.startTimeOfDaySeconds, baseline->startTimeOfDaySeconds))
    patch.startTimeOfDaySeconds = edited.startTimeOfDaySeconds;
  if (!SameTimeOfDay(edited.endTimeOfDaySeconds, baseline->endTimeOfDaySeconds))
    patch.endTimeOfDaySeconds = edited.endTimeOfDaySeconds;
  if (!SameCalendarDay(edited.startDate, baseline->startDate))
    patch.startDate = edited.startDate;
  if (edited.enabled != baseline->enabled)
    patch.enabled = edited.enabled;
  return patch;
}

time_t ComputeEndDateForOpenEndedRuleEdit(time_t currentEndDate, time_t startDate, time_t now, int windowDays)
{
  if (currentEndDate > 0)
    return 0;
  // Never before the start date: a rule whose first day is still ahead needs its
  // window counted from there, or the end would land before the start and be
  // refused ("End date must be on or after start date").
  return std::max(now, startDate) + static_cast<time_t>(windowDays) * 86400;
}

} // namespace dispatcharr
