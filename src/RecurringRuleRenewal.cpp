#include "RecurringRuleRenewal.h"

#include "ManagedRecurringRule.h"

namespace dispatcharr
{

bool ShouldRenewRecurringRule(const RecurringRule& rule, const std::vector<Recording>& recordings, bool haveRecordings,
                              time_t now, int windowDays, int safetyMarginSeconds)
{
  if (!rule.enabled)
    return false; // nothing materializes for a disabled rule anyway

  // Only a rule this addon owns is ever renewed -- see ManagedRecurringRule.h. A rule
  // made in Dispatcharr's web UI with a deliberate end date used to be extended
  // again and again here, and that same inability to tell them apart is why an
  // expired rule was never revived.
  if (!IsManagedRuleName(rule.name))
    return false;

  // An owned rule that has run past its end date (no Kodi running this addon for
  // longer than the window) is revived rather than left dead: it is ours, so
  // nothing says it was meant to end. A rule with no end date at all is the same
  // case (it cannot be saved without one). Neither is "nearing its limit", so the
  // window check below does not apply; the occurrence check still does.
  const bool revival = rule.endDate <= now;
  if (!revival)
  {
    time_t daysLeft = (rule.endDate - now) / 86400;
    if (daysLeft >= windowDays / 2)
      return false; // still comfortably inside the window
  }

  // Skip a rule with an occurrence currently recording or about to start soon (a
  // rule PATCH makes Dispatcharr drop and regenerate its future occurrences). If
  // GetRecordings() itself failed, err toward skipping rather than renewing blind.
  return !HasActiveOrImminentOccurrence(rule, recordings, haveRecordings, now, safetyMarginSeconds);
}

bool ShouldExtendRecurringRuleEndDateOnUpdate(bool wasEnabled, bool enabled, time_t cachedEndDate, time_t now,
                                              int windowDays)
{
  if (wasEnabled || !enabled)
    return false; // only a genuine disabled -> enabled transition qualifies
  // A zero/unparseable cachedEndDate means "no end_date at all" (a null
  // end_date, or one TimeFromDateString() couldn't parse, both parse to
  // 0 -- see ParseRecurringRuleJson()) -- the same sentinel
  // ShouldRenewRecurringRule() above already relies on to leave a
  // genuinely open-ended rule alone. Fix for a real, confirmed gap found
  // via a project-wide review in a later pass than the one that added
  // this function, not itself independently reproduced: without this
  // guard, (cachedEndDate - now) is a large negative number, which
  // passed the "less than half the window remains" check below just
  // like a genuinely expired rule would -- re-enabling an intentionally
  // permanent, open-ended rule from Kodi would give it a brand-new
  // finite end_date (now + windowDays), silently converting "records
  // forever" into "records only while this addon keeps running to
  // periodically renew it." UpdateRecurringRule()'s own PATCH already
  // never touches end_date on its own (see its own comment), so simply
  // not extending here leaves an open-ended rule exactly as it was.
  if (cachedEndDate <= 0)
    return false;
  time_t daysLeft = (cachedEndDate - now) / 86400;
  return daysLeft < windowDays / 2;
}

time_t ComputeRecurringRuleEndDateForStartDateChange(time_t newStartDate, time_t cachedEndDate, int windowDays)
{
  if (cachedEndDate <= 0)
    return 0; // open-ended rule -- nothing to protect
  if (newStartDate < cachedEndDate)
    return 0; // still safely inside the existing end_date
  return newStartDate + static_cast<time_t>(windowDays) * 86400;
}

bool HasRecurringRuleEndDatePassed(bool enabled, time_t endDate, time_t now, int offsetMinutes)
{
  if (!enabled || endDate <= 0)
    return false;
  time_t offsetSeconds = static_cast<time_t>(offsetMinutes) * 60;
  return now >= endDate + 86400 - offsetSeconds;
}

} // namespace dispatcharr
