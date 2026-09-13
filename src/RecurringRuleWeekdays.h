#pragma once

#include <vector>

namespace dispatcharr
{

// Dispatcharr's days_of_week (0=Monday..6=Sunday) already matches Kodi's
// own PVR_WEEKDAY_MONDAY=(1<<0)..PVR_WEEKDAY_SUNDAY=(1<<6) bit order --
// confirmed against Kodi's real header (kodi-dev-kit's
// pvr_timers.h: PVR_WEEKDAY_NONE = 0, PVR_WEEKDAY_MONDAY = (1 << 0), ...)
// -- so converting between the two is a plain `1 << day`, no reordering.
// The return value's `0` case matches PVR_WEEKDAY_NONE's own value,
// hardcoded here rather than including Kodi's header, the same
// confirmed-equivalent-constant approach EpgTagUtil already uses for
// Kodi's EPG content-mask values. A `day` outside [0, 6] (shouldn't
// happen, but this is reading server-supplied data) is silently
// skipped rather than treated as an error.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see
// ../tests/test_recurring_rule_weekdays.cpp.
inline unsigned int ComputeRecurringRuleWeekdaysBitmask(const std::vector<int>& daysOfWeek)
{
  unsigned int weekdays = 0;
  for (int day : daysOfWeek)
  {
    if (day >= 0 && day <= 6)
      weekdays |= (1u << day);
  }
  return weekdays;
}

} // namespace dispatcharr
