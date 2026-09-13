#include "RecurringRuleRenewal.h"

namespace dispatcharr
{

bool ShouldRenewRecurringRule(const RecurringRule& rule, const std::vector<Recording>& recordings, bool haveRecordings,
                              time_t now, int windowDays, int safetyMarginSeconds)
{
  if (!rule.enabled)
    return false; // nothing materializes for a disabled rule anyway

  time_t daysLeft = (rule.endDate - now) / 86400;
  if (daysLeft >= windowDays / 2)
    return false; // still comfortably inside the window

  // Skip a rule with an occurrence currently recording or about to start
  // soon. If GetRecordings() itself failed, err toward skipping rather
  // than renewing blind.
  bool hasActiveOrImminentOccurrence = !haveRecordings;
  if (haveRecordings)
  {
    for (const auto& rec : recordings)
    {
      if (rec.recurringRuleId != rule.id)
        continue;
      if (rec.isInProgress || (rec.isUpcoming && rec.startTime - now < safetyMarginSeconds))
      {
        hasActiveOrImminentOccurrence = true;
        break;
      }
    }
  }
  return !hasActiveOrImminentOccurrence;
}

} // namespace dispatcharr
