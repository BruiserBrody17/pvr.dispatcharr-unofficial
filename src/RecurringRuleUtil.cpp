#include "RecurringRuleUtil.h"

namespace dispatcharr
{

bool ComputeRecurringRuleFields(time_t startTime, time_t endTime, time_t firstDay, unsigned int weekdaysBitmask,
                                int offsetMinutes, time_t nowUtc, std::vector<int>& daysOfWeekOut, int& startSecondsOut,
                                int& endSecondsOut, time_t& startDateOut, std::string& error)
{
  constexpr time_t kSecondsPerDay = 86400;
  auto floorMod = [](time_t a, time_t m) { return ((a % m) + m) % m; };
  auto utcMidnight = [&](time_t t) { return t - floorMod(t, kSecondsPerDay); };
  auto secondsSinceUtcMidnight = [&](time_t t) { return static_cast<int>(floorMod(t, kSecondsPerDay)); };

  int offsetSeconds = offsetMinutes * 60;
  startSecondsOut = secondsSinceUtcMidnight(startTime) + offsetSeconds;
  endSecondsOut = secondsSinceUtcMidnight(endTime) + offsetSeconds;

  time_t effectiveFirstDay = firstDay;
  if (effectiveFirstDay <= 0)
    effectiveFirstDay = nowUtc; // Kodi didn't supply one -- default to "today"
  startDateOut = utcMidnight(effectiveFirstDay + offsetSeconds);

  daysOfWeekOut.clear();
  for (int day = 0; day <= 6; ++day)
  {
    if (weekdaysBitmask & (1u << day))
      daysOfWeekOut.push_back(day);
  }
  if (daysOfWeekOut.empty())
  {
    error = "At least one day of the week must be selected";
    return false;
  }
  return true;
}

} // namespace dispatcharr
