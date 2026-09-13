#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Pure integer-arithmetic core of PVRDispatcharr::ComputeRecurringRuleFields
// -- converts Kodi's UTC-based weekday bitmask/start-end-time-of-day/
// first-day into Dispatcharr's own representation (0-6 day list, its
// configured-system-timezone-local time-of-day via offsetMinutes, a
// UTC-midnight start date). Returns false (with error set) only when no
// weekday is selected at all -- everything else here is pure, infallible
// conversion.
//
// Takes plain values (not a kodi::addon::PVRTimer&) and an explicit
// `nowUtc` (used only when `firstDay` is <= 0, meaning Kodi didn't supply
// one) specifically so this is unit-testable standalone with no Kodi SDK
// dependency; see ../tests/test_recurring_rule_util.cpp. Every value here
// (startTime/endTime/firstDay) is already UTC (this addon's convention
// throughout), and a UTC time_t's own modulo-86400 gives an exact,
// DST-free calendar-day/time-of-day split with no gmtime/timegm
// round-trip needed. The *only* place a real timezone enters is the
// explicit offsetMinutes shift, bridging to Dispatcharr's own
// (non-UTC-by-default) system timezone.
bool ComputeRecurringRuleFields(time_t startTime, time_t endTime, time_t firstDay, unsigned int weekdaysBitmask,
                                int offsetMinutes, time_t nowUtc, std::vector<int>& daysOfWeekOut, int& startSecondsOut,
                                int& endSecondsOut, time_t& startDateOut, std::string& error);

} // namespace dispatcharr
