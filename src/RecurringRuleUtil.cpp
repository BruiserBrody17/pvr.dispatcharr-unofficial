#include "RecurringRuleUtil.h"

#include <cstdint>

namespace dispatcharr
{

namespace
{

constexpr std::int64_t kSecondsPerDay = 86400;

std::int64_t FloorDiv(std::int64_t a, std::int64_t b)
{
  std::int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}

} // namespace

int ComputeRecurringRuleDayShift(int utcSecondsOfDay, int kodiOffsetMinutes, int dispatcharrOffsetMinutes)
{
  const std::int64_t utc = ((utcSecondsOfDay % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay;
  const std::int64_t kodiDay = FloorDiv(utc + std::int64_t{kodiOffsetMinutes} * 60, kSecondsPerDay);
  const std::int64_t dispatcharrDay = FloorDiv(utc + std::int64_t{dispatcharrOffsetMinutes} * 60, kSecondsPerDay);
  return static_cast<int>(dispatcharrDay - kodiDay);
}

unsigned int RotateWeekdaysBitmask(unsigned int bitmask, int days)
{
  const unsigned int week = bitmask & 0x7Fu;
  const int shift = ((days % 7) + 7) % 7;
  if (shift == 0)
    return week;
  return ((week << shift) | (week >> (7 - shift))) & 0x7Fu;
}

bool ComputeRecurringRuleFields(time_t startTime, time_t endTime, time_t firstDay, unsigned int weekdaysBitmask,
                                int offsetMinutes, const std::function<int(time_t)>& kodiOffsetMinutesAt, time_t nowUtc,
                                std::vector<int>& daysOfWeekOut, int& startSecondsOut, int& endSecondsOut,
                                time_t& startDateOut, std::string& error)
{
  auto floorMod = [](time_t a, time_t m) { return ((a % m) + m) % m; };
  auto utcMidnight = [&](time_t t) { return t - floorMod(t, static_cast<time_t>(kSecondsPerDay)); };
  auto secondsSinceUtcMidnight = [&](time_t t)
  { return static_cast<int>(floorMod(t, static_cast<time_t>(kSecondsPerDay))); };

  int offsetSeconds = offsetMinutes * 60;
  const int startUtcSeconds = secondsSinceUtcMidnight(startTime);
  // Wrapped into one day: shifting a UTC time of day into Dispatcharr's zone
  // lands outside [0, 86400) whenever the two calendars disagree about the
  // day (an evening rule in a zone behind UTC, an early-morning one ahead of
  // it). Dispatcharr only ever sees the "HH:MM:SS" this wraps to, so a
  // consumer comparing these against a value the server reported
  // (ComputeRecurringRuleEditPatch()) or truncating them
  // (TruncateRuleTimesToWholeMinutes()) must see the same representation --
  // left unwrapped, an unchanged 22:00 rule read as -7200 against the
  // server's 79200 and was re-sent on every edit.
  auto wrapSecondsOfDay = [](int seconds) { return ((seconds % 86400) + 86400) % 86400; };
  startSecondsOut = wrapSecondsOfDay(startUtcSeconds + offsetSeconds);
  // The start's offset for the end too, on purpose. Kodi turns each time typed in its dialog into UTC with
  // CPVRTimerInfoTag::ConvertLocalTimeToUTC(), which takes the daylight-saving flag from localtime() of the local
  // fields READ AS A UTC INSTANT (an early-morning time lands on the previous evening, before a spring-forward) and
  // then calls mktime(): for a rule typed across a clock change both times arrive with the same flag, so the same
  // offset undoes both. Converting the end with the offset at its own instant (tried 2026-10-06) stored 01:30-03:30 as
  // 01:30-04:30 and then grew by an hour on every no-op edit; see docs/RECURRING_RULES.md.
  endSecondsOut = wrapSecondsOfDay(secondsSinceUtcMidnight(endTime) + offsetSeconds);

  const int dayShift = ComputeRecurringRuleDayShift(startUtcSeconds, kodiOffsetMinutesAt(startTime), offsetMinutes);

  if (firstDay > 0)
  {
    // The calendar date Kodi shows for first day, in Kodi's own zone at that
    // instant, moved into Dispatcharr's calendar -- see this function's
    // header comment for why it's the date and not the instant that matters.
    startDateOut = utcMidnight(firstDay + static_cast<time_t>(kodiOffsetMinutesAt(firstDay)) * 60) +
                   static_cast<time_t>(dayShift) * static_cast<time_t>(kSecondsPerDay);
  }
  else
  {
    startDateOut = utcMidnight(nowUtc + offsetSeconds); // Kodi didn't supply one -- default to "today"
  }

  daysOfWeekOut.clear();
  const unsigned int dispatcharrWeekdays = RotateWeekdaysBitmask(weekdaysBitmask, dayShift);
  for (int day = 0; day <= 6; ++day)
  {
    if (dispatcharrWeekdays & (1u << day))
      daysOfWeekOut.push_back(day);
  }
  if (daysOfWeekOut.empty())
  {
    error = "At least one day of the week must be selected";
    return false;
  }
  return true;
}

void ComputeRecurringRuleDisplayTimes(time_t startDate, int startTimeOfDaySeconds, int endTimeOfDaySeconds,
                                      const std::function<int(time_t)>& resolveOffsetMinutes,
                                      const std::function<int(time_t)>& kodiOffsetMinutesAt, time_t& startTimeOut,
                                      time_t& endTimeOut, time_t& firstDayOut, int& dayShiftOut)
{
  int offsetMinutes = resolveOffsetMinutes(startDate);
  time_t offsetSeconds = static_cast<time_t>(offsetMinutes) * 60;
  startTimeOut = startDate + startTimeOfDaySeconds - offsetSeconds;
  for (int i = 0; i < 2; ++i)
  {
    int refined = resolveOffsetMinutes(startTimeOut);
    if (refined == offsetMinutes)
      break;
    offsetMinutes = refined;
    offsetSeconds = static_cast<time_t>(offsetMinutes) * 60;
    startTimeOut = startDate + startTimeOfDaySeconds - offsetSeconds;
  }

  // The end is resolved on its own, like the start: when a clock change falls between a rule's start and its end that
  // day, the end is a real instant under the OTHER offset (a rule 01:30 to 03:30 on a spring-forward day is one hour
  // long, and its 19:45 end on that day is an hour earlier in UTC than the start's offset says -- found by the ninth
  // hardening sweep, which left the display one hour off for exactly the rule whose first day is the transition day).
  // The overnight day below is part of the end's local time, so it is added before the offset is resolved.
  time_t endLocal = startDate + endTimeOfDaySeconds;
  // An overnight rule (e.g. 23:00-01:00, where endTimeOfDaySeconds is
  // numerically <= startTimeOfDaySeconds) crosses midnight -- matching
  // Dispatcharr's own scheduler, confirmed against its real current
  // upstream source (an 18th-pass audit cloned it into a scratchpad,
  // never committed to this repo): sync_recurring_rule_impl()
  // (apps/channels/tasks.py) does `if end_dt <= start_dt: end_dt +=
  // timedelta(days=1)`. Fix for a real, confirmed display bug found via
  // a project-wide review (this addon's own create path already passes
  // such a rule through unchanged, so it's reachable from Kodi's own
  // timer dialog, not hypothetical): without this, the displayed end
  // time landed *before* the displayed start time in Kodi's Timers
  // list.
  if (endTimeOfDaySeconds <= startTimeOfDaySeconds)
    endLocal += 86400;
  int endOffsetMinutes = offsetMinutes;
  endTimeOut = endLocal - static_cast<time_t>(endOffsetMinutes) * 60;
  for (int i = 0; i < 2; ++i)
  {
    const int refined = resolveOffsetMinutes(endTimeOut);
    if (refined == endOffsetMinutes)
      break;
    endOffsetMinutes = refined;
    endTimeOut = endLocal - static_cast<time_t>(endOffsetMinutes) * 60;
  }

  // startDate is the Dispatcharr-local date of the rule's first occurrence,
  // and dayShiftOut is how much later that is than the same occurrence's
  // date in Kodi's zone, so the Kodi-local date is startDate's minus it. Noon
  // of that date in Kodi's zone is what the inverse
  // (ComputeRecurringRuleFields()'s own firstDay handling) maps straight
  // back to startDate -- see this function's header comment.
  const time_t startUtcSeconds =
      ((startTimeOut % static_cast<time_t>(kSecondsPerDay)) + static_cast<time_t>(kSecondsPerDay)) %
      static_cast<time_t>(kSecondsPerDay);
  const int kodiStartOffsetMinutes = kodiOffsetMinutesAt(startTimeOut);
  dayShiftOut = ComputeRecurringRuleDayShift(static_cast<int>(startUtcSeconds), kodiStartOffsetMinutes, offsetMinutes);
  const time_t kodiLocalNoon = startDate - static_cast<time_t>(dayShiftOut) * static_cast<time_t>(kSecondsPerDay) +
                               static_cast<time_t>(kSecondsPerDay / 2);
  // Kodi's offset at the instant being named, which can differ from the one
  // at the first occurrence if a transition falls between the two days.
  const int kodiFirstDayOffsetMinutes =
      kodiOffsetMinutesAt(kodiLocalNoon - static_cast<time_t>(kodiStartOffsetMinutes) * 60);
  firstDayOut = kodiLocalNoon - static_cast<time_t>(kodiFirstDayOffsetMinutes) * 60;
}

void TruncateRuleTimesToWholeMinutes(int& startSeconds, int& endSeconds)
{
  // Floor, not C++'s truncation toward zero: for a negative value (a time of
  // day expressed from the previous UTC day, before it is wrapped) `%` would
  // round UP to the next minute -- 21:59:30 as -7230 became 22:00:00.
  auto floorMod = [](int a, int m) { return ((a % m) + m) % m; };
  const int start = startSeconds - floorMod(startSeconds, 60);
  const int end = endSeconds - floorMod(endSeconds, 60);
  if (start == end && startSeconds != endSeconds)
    return;
  startSeconds = start;
  endSeconds = end;
}

} // namespace dispatcharr
