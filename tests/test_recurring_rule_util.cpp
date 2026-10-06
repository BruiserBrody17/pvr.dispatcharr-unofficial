#include "RecurringRuleUtil.h"

#include "TimeZoneUtil.h"

#include <catch2/catch_test_macros.hpp>

#include "TimeUtil.h"

#include <bitset>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace dispatcharr;

// Test time_t values are chosen as raw seconds-since-epoch so they equal
// their own "seconds since UTC midnight" directly (epoch itself is a UTC
// midnight) -- e.g. 52200 means both "time_t 52200" and "14:30:00".

namespace
{

using OffsetFn = std::function<int(time_t)>;

// A resolver that returns the same offset regardless of the instant
// asked about -- the fixed-point iteration is then a no-op (the first
// guess is already stable), matching every pre-existing test's own
// fixed-offset intent below. Also how a test names a Kodi (or Dispatcharr)
// in a zone with no daylight saving.
OffsetFn FixedOffset(int offsetMinutes)
{
  return [offsetMinutes](time_t) { return offsetMinutes; };
}

// A real zone, daylight saving included, from the addon's own hardcoded
// table (TimeZoneUtil.cpp) -- the same function the addon resolves
// Dispatcharr's offset with in production.
OffsetFn ZoneOffset(const std::string& ianaZoneName)
{
  return [ianaZoneName](time_t at)
  {
    int offset = 0;
    REQUIRE(ComputeKnownZoneOffsetMinutes(ianaZoneName, at, offset));
    return offset;
  };
}

} // namespace

TEST_CASE("ComputeRecurringRuleFields computes start/end seconds and the selected day", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(52200 /* 14:30 */, 55800 /* 15:30 */, 0 /* no firstDay -> use nowUtc */,
                                       0b0000010u /* Tuesday only */, 0 /* offsetMinutes */, FixedOffset(0),
                                       100000 /* nowUtc */, daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == 52200);
  CHECK(endSeconds == 55800);
  CHECK(startDate == 86400); // utcMidnight(100000) -- the start of the *next* epoch day
  CHECK(daysOfWeek == std::vector<int>{1});
}

TEST_CASE("ComputeRecurringRuleFields collects every selected weekday, in order", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 1, 0b0010011u /* bits 0, 1, 4 */, 0, FixedOffset(0), 0, daysOfWeek,
                                       startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(daysOfWeek == std::vector<int>{0, 1, 4});
}

TEST_CASE("ComputeRecurringRuleFields fails when no weekday is selected", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 1, 0 /* no bits set */, 0, FixedOffset(0), 0, daysOfWeek, startSeconds,
                                       endSeconds, startDate, error);

  CHECK_FALSE(ok);
  CHECK(error == "At least one day of the week must be selected");
}

TEST_CASE("ComputeRecurringRuleFields applies the timezone offset to start/end seconds", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  // -480 minutes (US Pacific standard), matching TimeZoneUtil's own
  // convention for a negative (behind-UTC) offset.
  bool ok = ComputeRecurringRuleFields(52200 /* 14:30 UTC */, 55800 /* 15:30 UTC */, 1, 0b1u, -480, FixedOffset(-480),
                                       0, daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == 23400); // 52200 - 28800
  CHECK(endSeconds == 27000);   // 55800 - 28800
}

TEST_CASE("ComputeRecurringRuleFields wraps a result that crosses midnight into one day", "[RecurringRuleUtil]")
{
  // 01:00 UTC is 17:00 the previous evening at UTC-8. This used to come back
  // as -25200 ("callers wrap it when formatting"), which the edit diff then
  // compared against the server's own 61200 and read as a change on every
  // save -- see test_recurring_rule_edit.cpp.
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(3600 /* 01:00 UTC */, 3600, 1, 0b1u, -480, FixedOffset(-480), 0, daysOfWeek,
                                       startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == 17 * 3600);
  CHECK(endSeconds == 17 * 3600);

  // And past the end of the day in a zone ahead of UTC: 21:00 UTC is 06:00
  // the next morning in Tokyo, not 30:00.
  REQUIRE(ComputeRecurringRuleFields(21 * 3600, 22 * 3600, 1, 0b1u, 540, FixedOffset(540), 0, daysOfWeek, startSeconds,
                                     endSeconds, startDate, error));
  CHECK(startSeconds == 6 * 3600);
  CHECK(endSeconds == 7 * 3600);
}

TEST_CASE("ComputeRecurringRuleFields uses a real firstDay when provided, not nowUtc", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 52200 /* mid-day firstDay */, 0b1u, 0, FixedOffset(0),
                                       999999999 /* should be ignored */, daysOfWeek, startSeconds, endSeconds,
                                       startDate, error);

  REQUIRE(ok);
  CHECK(startDate == 0); // utcMidnight(52200) -- floors back to epoch start
}

TEST_CASE("ComputeRecurringRuleFields falls back to nowUtc for a negative firstDay too", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok =
      ComputeRecurringRuleFields(0, 0, -1 /* negative, not just zero */, 0b1u, 0, FixedOffset(0),
                                 172800 /* nowUtc, day 2 */, daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startDate == 172800); // utcMidnight(172800) == 172800, already a midnight
}

// ---------------------------------------------------------------------
// ComputeRecurringRuleDisplayTimes
// ---------------------------------------------------------------------

TEST_CASE("ComputeRecurringRuleDisplayTimes is the exact inverse of ComputeRecurringRuleFields with no offset",
          "[RecurringRuleUtil]")
{
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/86400, /*startTimeOfDaySeconds=*/3600, /*endTimeOfDaySeconds=*/7200,
                                   FixedOffset(0), FixedOffset(0), startTime, endTime, firstDay, dayShift);
  CHECK(startTime == 86400 + 3600);
  CHECK(endTime == 86400 + 7200);
  // A zone with no offset from UTC: Kodi's local noon of startDate's own date.
  CHECK(firstDay == 86400 + 43200);
  CHECK(dayShift == 0);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes shifts local time-of-day back to UTC", "[RecurringRuleUtil]")
{
  // offsetMinutes=-480 (UTC-8, matching ComputeRecurringRuleFields' own
  // test convention above) means Dispatcharr's own local time-of-day is
  // 8 hours *behind* UTC, so converting back to UTC subtracts a negative
  // offset -- i.e. adds 8 hours.
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/0, /*startTimeOfDaySeconds=*/3600, /*endTimeOfDaySeconds=*/7200,
                                   FixedOffset(-480), FixedOffset(-480), startTime, endTime, firstDay, dayShift);
  CHECK(startTime == 3600 + 28800);
  CHECK(endTime == 7200 + 28800);
  CHECK(firstDay == 12 * 3600 + 28800); // local noon of startDate (0), at UTC-8
}

TEST_CASE("ComputeRecurringRuleDisplayTimes resolves the end's offset on its own", "[RecurringRuleUtil]")
{
  // A zone that goes -420 -> -360 minutes at 2026-03-08 09:00 UTC (02:00 standard time springing forward) and back
  // -360 -> -420 at 2026-11-01 08:00 UTC (02:00 daylight time falling back). startDate is the rule's first day at UTC
  // midnight.
  constexpr time_t kSpringDate = 1772928000, kSpringChange = 1772960400;
  constexpr time_t kFallDate = 1793491200, kFallChange = 1793520000;
  auto spring = [](time_t at) { return at >= kSpringChange ? -360 : -420; };
  auto fall = [](time_t at) { return at >= kFallChange ? -420 : -360; };
  time_t startTime, endTime, firstDay;
  int dayShift;

  // A 01:30-03:30 rule on the spring-forward day starts on standard time and ends on daylight time: 08:30 UTC to 09:30
  // UTC, an hour long (the start's offset alone gave an end of 10:30 UTC, shown an hour late).
  ComputeRecurringRuleDisplayTimes(kSpringDate, 5400, 12600, spring, FixedOffset(-420), startTime, endTime, firstDay,
                                   dayShift);
  CHECK(startTime == kSpringDate + 8 * 3600 + 1800);
  CHECK(endTime == kSpringDate + 9 * 3600 + 1800);

  // 00:30-03:00 on the fall-back day starts on daylight time (06:30 UTC) and ends on standard time (10:00 UTC).
  ComputeRecurringRuleDisplayTimes(kFallDate, 1800, 10800, fall, FixedOffset(-360), startTime, endTime, firstDay,
                                   dayShift);
  CHECK(startTime == kFallDate + 6 * 3600 + 1800);
  CHECK(endTime == kFallDate + 10 * 3600);

  // An overnight rule on the day before a spring-forward: 23:00 standard time to 04:00 daylight time the next morning,
  // four hours long in real time, 06:00 UTC to 10:00 UTC.
  const time_t saturday = kSpringDate - 86400;
  ComputeRecurringRuleDisplayTimes(saturday, 23 * 3600, 4 * 3600, spring, FixedOffset(-420), startTime, endTime,
                                   firstDay, dayShift);
  CHECK(startTime == saturday + 30 * 3600);
  CHECK(endTime == saturday + 34 * 3600);

  // A day with no change between the two keeps the old single-offset answer, either side of the transition.
  ComputeRecurringRuleDisplayTimes(kSpringDate - 2 * 86400, 7200, 10800, spring, FixedOffset(-420), startTime, endTime,
                                   firstDay, dayShift);
  CHECK(endTime - startTime == 3600);
  ComputeRecurringRuleDisplayTimes(kSpringDate + 2 * 86400, 7200, 10800, spring, FixedOffset(-360), startTime, endTime,
                                   firstDay, dayShift);
  CHECK(endTime - startTime == 3600);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes round-trips through ComputeRecurringRuleFields", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;
  ComputeRecurringRuleFields(/*startTime=*/3600, /*endTime=*/7200, /*firstDay=*/86400, 0b1u, /*offsetMinutes=*/-480,
                             FixedOffset(-480), /*nowUtc=*/0, daysOfWeek, startSeconds, endSeconds, startDate, error);

  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(startDate, startSeconds, endSeconds, FixedOffset(-480), FixedOffset(-480), startTime,
                                   endTime, firstDay, dayShift);

  // The rule's first day is the Kodi-local date of firstDay (day 0 at UTC-8),
  // and 01:00 UTC on it is 17:00 local the day before -- so its first
  // occurrence is 17:00 local ON day 0, i.e. 01:00 UTC of day 1. Before
  // ComputeRecurringRuleFields() wrapped its time of day, this came out as
  // 3600, a day before the first day; the same time of day either way.
  CHECK(startTime == 86400 + 3600);
  CHECK(endTime == 86400 + 7200);
  CHECK(startSeconds == 17 * 3600);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes's own firstDayOut round-trips exactly back through "
          "ComputeRecurringRuleFields -- the real bug this fixes",
          "[RecurringRuleUtil]")
{
  // The exact scenario a Kodi Timers-list edit reproduces: GetTimers()
  // calls SetFirstDay(firstDayOut) with whatever this function computes;
  // on ANY later edit (including one that doesn't touch the date/time
  // fields at all, e.g. toggling enabled/disabled), Kodi echoes that
  // same value back as timer.GetFirstDay(), fed straight into
  // ComputeRecurringRuleFields()'s own `firstDay` parameter. Before this
  // fix, GetTimers() passed startDate straight through unshifted, so
  // this second call applied ComputeRecurringRuleFields()'s own
  // `+ offsetSeconds` shift a *second* time -- for a zone behind UTC,
  // that always floored to the *previous* day (utcMidnight() of an
  // already-exact-midnight value plus a negative sub-day offset), moving
  // the rule's own start date back one full day on every single edit.
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;
  constexpr int kOffsetMinutes = -480; // a zone behind UTC -- the failing case pre-fix
  ComputeRecurringRuleFields(/*startTime=*/3600, /*endTime=*/7200, /*firstDay=*/86400, 0b1u, kOffsetMinutes,
                             FixedOffset(kOffsetMinutes), /*nowUtc=*/0, daysOfWeek, startSeconds, endSeconds, startDate,
                             error);

  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(startDate, startSeconds, endSeconds, FixedOffset(kOffsetMinutes),
                                   FixedOffset(kOffsetMinutes), startTime, endTime, firstDay, dayShift);

  // Simulates Kodi echoing firstDay back unchanged on a later edit.
  std::vector<int> daysOfWeek2;
  int startSeconds2 = 0, endSeconds2 = 0;
  time_t startDateRoundTripped = -1;
  std::string error2;
  ComputeRecurringRuleFields(startTime, endTime, firstDay, 0b1u, kOffsetMinutes, FixedOffset(kOffsetMinutes),
                             /*nowUtc=*/0, daysOfWeek2, startSeconds2, endSeconds2, startDateRoundTripped, error2);

  CHECK(startDateRoundTripped == startDate); // must reconstruct exactly, not drift a day
}

TEST_CASE("ComputeRecurringRuleDisplayTimes rolls an overnight rule's end time forward a day -- the real bug this "
          "fixes",
          "[RecurringRuleUtil]")
{
  // A 23:00-01:00 rule: endTimeOfDaySeconds (3600, i.e. 01:00) is
  // numerically less than startTimeOfDaySeconds (82800, i.e. 23:00).
  // Matches Dispatcharr's own scheduler, confirmed against its real
  // source: sync_recurring_rule_impl() adds a day to end_dt whenever
  // end_dt <= start_dt.
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/0, /*startTimeOfDaySeconds=*/82800, /*endTimeOfDaySeconds=*/3600,
                                   FixedOffset(0), FixedOffset(0), startTime, endTime, firstDay, dayShift);
  CHECK(startTime == 82800);
  CHECK(endTime == 3600 + 86400);
  CHECK(endTime > startTime); // the real bug this fixes: this used to be false
}

TEST_CASE("ComputeRecurringRuleDisplayTimes does not adjust an ordinary same-day rule", "[RecurringRuleUtil]")
{
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/0, /*startTimeOfDaySeconds=*/3600, /*endTimeOfDaySeconds=*/7200,
                                   FixedOffset(0), FixedOffset(0), startTime, endTime, firstDay, dayShift);
  CHECK(startTime == 3600);
  CHECK(endTime == 7200); // no +86400 -- end is genuinely later the same day
}

TEST_CASE("ComputeRecurringRuleDisplayTimes treats an exactly-equal start/end time as overnight too, matching "
          "Dispatcharr's own <= comparison",
          "[RecurringRuleUtil]")
{
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/0, /*startTimeOfDaySeconds=*/3600, /*endTimeOfDaySeconds=*/3600,
                                   FixedOffset(0), FixedOffset(0), startTime, endTime, firstDay, dayShift);
  CHECK(endTime == 3600 + 86400);
}

// ---------------------------------------------------------------------
// ComputeRecurringRuleDisplayTimes's fixed-point DST resolution -- the
// real, live-confirmed bug fixed 2026-09-29 (see docs/OPEN_ITEMS.md).
// A resolver simulating a single DST transition: +120 (e.g. CEST) before
// the transition instant, +60 (CET) at or after it -- the same one-hour
// fall-back shape the bug was confirmed against live (there in a real zone),
// just with a small synthetic transition instant instead of a real
// late-October date.
// ---------------------------------------------------------------------

TEST_CASE("ComputeRecurringRuleDisplayTimes refines its offset when startDate's own offset disagrees with the "
          "resolved start instant's -- the real bug this fixes",
          "[RecurringRuleUtil]")
{
  constexpr time_t kTransitionInstant = 3600; // 01:00 UTC -- the instant Central Europe falls back
  auto resolver = [](time_t at) { return at < kTransitionInstant ? 120 : 60; };

  // startDate's own UTC midnight (0) is before the transition, so a
  // single, unrefined resolution would use +120 (CEST) throughout --
  // computing startTime = 0 + 10800 - 7200 = 3600, which is *itself*
  // already at the transition instant. The fix re-resolves at that
  // tentative result, finds +60 (CET) instead, and recomputes.
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/0, /*startTimeOfDaySeconds=*/10800 /* 03:00 */,
                                   /*endTimeOfDaySeconds=*/14400 /* 04:00 */, resolver, FixedOffset(60), startTime,
                                   endTime, firstDay, dayShift);

  CHECK(startTime == 7200);            // 0 + 10800 - 3600 (+60min) -- NOT 3600, the unrefined/buggy value
  CHECK(endTime == 10800);             // same refined offset applied to endTimeOfDaySeconds too
  CHECK(firstDay == 12 * 3600 - 3600); // Kodi (UTC+1)'s local noon of startDate's date

  // The result is a genuine fixed point: re-resolving at the final
  // startTime must agree with the offset actually used to compute it,
  // or this isn't really converged.
  CHECK(resolver(startTime) == 60);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes converges immediately when startDate's own offset already agrees",
          "[RecurringRuleUtil]")
{
  constexpr time_t kTransitionInstant = 3600;
  auto resolver = [](time_t at) { return at < kTransitionInstant ? 120 : 60; };

  // startDate's own UTC midnight is already at/after the transition, so
  // the first guess (+60) is already correct -- no refinement needed,
  // and the ordinary (non-DST-edge) case above this test file's other
  // cases already implicitly assumed stays correct.
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(/*startDate=*/kTransitionInstant, /*startTimeOfDaySeconds=*/10800,
                                   /*endTimeOfDaySeconds=*/14400, resolver, FixedOffset(60), startTime, endTime,
                                   firstDay, dayShift);

  CHECK(startTime == kTransitionInstant + 10800 - 3600);
}

// ---------------------------------------------------------------------
// The weekday/first-day shift between Kodi's local calendar and
// Dispatcharr's -- the open item from a 33rd-pass audit (2026-09-26),
// confirmed live 2026-09-30 (see docs/OPEN_ITEMS.md).
//
// Every expectation below is either a hand-worked vector with its
// arithmetic spelled out, or checked against a brute-force definition
// written independently of the formula under test: take an actual
// instant, render it in each zone, and compare the two calendar dates.
// ---------------------------------------------------------------------

namespace
{

constexpr time_t kDay = 86400;

int64_t FloorDivTest(int64_t a, int64_t b)
{
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0)))
    --q;
  return q;
}

// The calendar day number (days since 1970-01-01) an instant falls on in a
// zone `offsetMinutes` east of UTC.
int64_t LocalDay(int64_t instant, int offsetMinutes)
{
  return FloorDivTest(instant + int64_t{offsetMinutes} * 60, kDay);
}

// 0 = Monday .. 6 = Sunday, from a day number (1970-01-01 was a Thursday).
int WeekdayOfDay(int64_t dayNumber)
{
  return static_cast<int>((((dayNumber + 3) % 7) + 7) % 7);
}

// A spread of real-world offsets, east positive, in minutes: the extremes
// (Baker Island to Kiritimati), the half- and quarter-hour zones, and the
// common ones in between.
const int kRealWorldOffsets[] = {-720, -600, -480, -420, -360, -300, -240, -210, -180, 0,   60,  120, 180, 210,
                                 240,  270,  330,  345,  480,  525,  540,  570,  600,  720, 765, 780, 840};

unsigned int MaskOf(const std::vector<int>& days)
{
  unsigned int mask = 0;
  for (int d : days)
    mask |= 1u << d;
  return mask;
}

} // namespace

TEST_CASE("ComputeRecurringRuleDayShift is zero whenever the two zones share an offset", "[RecurringRuleUtil]")
{
  for (int offset : kRealWorldOffsets)
  {
    for (int utcSeconds = 0; utcSeconds < 86400; utcSeconds += 900)
      CHECK(ComputeRecurringRuleDayShift(utcSeconds, offset, offset) == 0);
    CHECK(ComputeRecurringRuleDayShift(86399, offset, offset) == 0);
  }
}

TEST_CASE("ComputeRecurringRuleDayShift: Kodi in Buenos Aires, Dispatcharr in UTC", "[RecurringRuleUtil]")
{
  // 11:00 PM in Buenos Aires (UTC-3) is 02:00 UTC the next day, so UTC's calendar is a day ahead.
  CHECK(ComputeRecurringRuleDayShift(2 * 3600, -180, 0) == 1);
  // Midnight Kodi-local (03:00 UTC) is the first instant both calendars agree again...
  CHECK(ComputeRecurringRuleDayShift(3 * 3600, -180, 0) == 0);
  // ...and one second before it they still don't.
  CHECK(ComputeRecurringRuleDayShift(3 * 3600 - 1, -180, 0) == 1);
  CHECK(ComputeRecurringRuleDayShift(12 * 3600, -180, 0) == 0);
  CHECK(ComputeRecurringRuleDayShift(0, -180, 0) == 1);
}

TEST_CASE("ComputeRecurringRuleDayShift: Kodi in Tokyo, Dispatcharr in Lima", "[RecurringRuleUtil]")
{
  // 12:00 Tokyo is 03:00 UTC, which is 22:00 the previous evening in Lima (UTC-5, no DST).
  CHECK(ComputeRecurringRuleDayShift(3 * 3600, 540, -300) == -1);
  // 15:00 UTC is Tokyo midnight (the next day) and 10:00 Lima the same day as UTC.
  CHECK(ComputeRecurringRuleDayShift(15 * 3600, 540, -300) == -1);
  CHECK(ComputeRecurringRuleDayShift(15 * 3600 - 1, 540, -300) == 0);
  // 05:00 UTC is Lima midnight (the start of its day): from there to 15:00 UTC both calendars agree.
  CHECK(ComputeRecurringRuleDayShift(5 * 3600, 540, -300) == 0);
  CHECK(ComputeRecurringRuleDayShift(5 * 3600 - 1, 540, -300) == -1);
}

TEST_CASE("ComputeRecurringRuleDayShift can reach two days when the zones are far enough apart", "[RecurringRuleUtil]")
{
  // Kiritimati (UTC+14) against Baker Island (UTC-12), 26 hours apart:
  // at 10:00 UTC it is 00:00 the next day in one and 22:00 the day before
  // in the other.
  CHECK(ComputeRecurringRuleDayShift(10 * 3600, 840, -720) == -2);
  CHECK(ComputeRecurringRuleDayShift(10 * 3600, -720, 840) == 2);
}

TEST_CASE("ComputeRecurringRuleDayShift wraps an out-of-range time of day into one day", "[RecurringRuleUtil]")
{
  CHECK(ComputeRecurringRuleDayShift(2 * 3600 + 86400, -360, 0) == ComputeRecurringRuleDayShift(2 * 3600, -360, 0));
  CHECK(ComputeRecurringRuleDayShift(2 * 3600 - 86400, -360, 0) == ComputeRecurringRuleDayShift(2 * 3600, -360, 0));
  CHECK(ComputeRecurringRuleDayShift(-1, -360, 0) == ComputeRecurringRuleDayShift(86399, -360, 0));
}

TEST_CASE("ComputeRecurringRuleDayShift matches rendering a real instant in both zones, on every date",
          "[RecurringRuleUtil]")
{
  // The property the whole fix rests on: the shift depends on the time of
  // day and the two offsets and nothing else, so it must equal the actual
  // difference between the two calendar dates of an instant with that time
  // of day, on any date at all.
  // Arbitrary dates from before the epoch to well past now.
  const time_t dates[] = {0, 17 * kDay, 19000 * kDay, 20454 * kDay, 21000 * kDay, -400 * kDay};
  int checked = 0;
  for (int kodi : kRealWorldOffsets)
  {
    for (int dispatcharr : kRealWorldOffsets)
    {
      for (int utcSeconds : {0, 1, 899, 900, 3599, 3600, 7200, 21599, 21600, 43199, 43200, 64799, 64800, 86399})
      {
        const int shift = ComputeRecurringRuleDayShift(utcSeconds, kodi, dispatcharr);
        for (time_t date : dates)
        {
          const int64_t instant = date + utcSeconds;
          REQUIRE(shift == LocalDay(instant, dispatcharr) - LocalDay(instant, kodi));
          ++checked;
        }
      }
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("RotateWeekdaysBitmask moves each selected day and wraps the week", "[RecurringRuleUtil]")
{
  constexpr unsigned int kMon = 1u << 0, kTue = 1u << 1, kWed = 1u << 2, kSat = 1u << 5, kSun = 1u << 6;
  CHECK(RotateWeekdaysBitmask(kMon, 1) == kTue);
  CHECK(RotateWeekdaysBitmask(kTue, -1) == kMon);
  CHECK(RotateWeekdaysBitmask(kSun, 1) == kMon);  // Sunday + 1 -> Monday
  CHECK(RotateWeekdaysBitmask(kMon, -1) == kSun); // Monday - 1 -> Sunday
  CHECK(RotateWeekdaysBitmask(kSat | kSun, 1) == (kSun | kMon));
  CHECK(RotateWeekdaysBitmask(kMon | kWed, 2) == (kWed | (1u << 4)));
  CHECK(RotateWeekdaysBitmask(0b1111111u, 3) == 0b1111111u);
  CHECK(RotateWeekdaysBitmask(0, 5) == 0);
}

TEST_CASE("RotateWeekdaysBitmask by a multiple of seven is the identity", "[RecurringRuleUtil]")
{
  for (unsigned int mask = 0; mask <= 0x7F; ++mask)
  {
    CHECK(RotateWeekdaysBitmask(mask, 0) == mask);
    CHECK(RotateWeekdaysBitmask(mask, 7) == mask);
    CHECK(RotateWeekdaysBitmask(mask, -7) == mask);
    CHECK(RotateWeekdaysBitmask(mask, 14) == mask);
  }
}

TEST_CASE("RotateWeekdaysBitmask keeps the number of selected days and is undone by the opposite rotation",
          "[RecurringRuleUtil]")
{
  for (unsigned int mask = 0; mask <= 0x7F; ++mask)
  {
    for (int days = -9; days <= 9; ++days)
    {
      const unsigned int rotated = RotateWeekdaysBitmask(mask, days);
      CHECK(std::bitset<8>(rotated).count() == std::bitset<8>(mask).count());
      CHECK(rotated <= 0x7F);
      CHECK(RotateWeekdaysBitmask(rotated, -days) == mask);
    }
  }
}

TEST_CASE("RotateWeekdaysBitmask drops bits above the seventh", "[RecurringRuleUtil]")
{
  CHECK(RotateWeekdaysBitmask(0xFFFFFF80u, 0) == 0);
  CHECK(RotateWeekdaysBitmask(0x81u, 1) == (1u << 1)); // Monday only survives, moved to Tuesday
}

TEST_CASE("ComputeRecurringRuleFields rotates the weekday list when Kodi's zone differs from Dispatcharr's",
          "[RecurringRuleUtil]")
{
  // Kodi in US Eastern in winter (EST, UTC-5), Dispatcharr in UTC. "Monday to
  // Friday at 8:00 PM" in Kodi is 01:00 UTC the next morning, which
  // Dispatcharr's own calendar calls Tuesday to Saturday.
  constexpr time_t kStart = 1767225600 + 1 * 3600; // 2026-01-01 01:00 UTC (only the time of day matters)
  constexpr time_t kEnd = 1767225600 + 2 * 3600;   // 02:00 UTC
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  REQUIRE(ComputeRecurringRuleFields(kStart, kEnd, /*firstDay=*/0, /*Mon-Fri=*/0b0011111u, /*offsetMinutes=*/0,
                                     FixedOffset(-300), /*nowUtc=*/1767225600, daysOfWeek, startSeconds, endSeconds,
                                     startDate, error));

  CHECK(daysOfWeek == std::vector<int>{1, 2, 3, 4, 5});
  CHECK(startSeconds == 3600);
  CHECK(endSeconds == 7200);
}

TEST_CASE("ComputeRecurringRuleFields wraps a shifted weekday around the end of the week", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  // Kodi Sunday 8 PM Eastern (winter, UTC-5) -> Dispatcharr (UTC) Monday 01:00.
  REQUIRE(ComputeRecurringRuleFields(3600, 7200, 0, /*Sunday=*/1u << 6, 0, FixedOffset(-300), 1767225600, daysOfWeek,
                                     startSeconds, endSeconds, startDate, error));
  CHECK(daysOfWeek == std::vector<int>{0});

  // Kodi Monday noon Tokyo (03:00 UTC) -> Dispatcharr in Bogota (UTC-5) is Sunday 22:00.
  REQUIRE(ComputeRecurringRuleFields(3 * 3600, 4 * 3600, 0, /*Monday=*/1u << 0, -300, FixedOffset(540), 1767225600,
                                     daysOfWeek, startSeconds, endSeconds, startDate, error));
  CHECK(daysOfWeek == std::vector<int>{6});
  CHECK(startSeconds == 22 * 3600); // 22:00 the previous evening
}

TEST_CASE("ComputeRecurringRuleFields leaves the weekday list alone when the time of day stays on the same day in "
          "both zones",
          "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  // Midday in both zones: 17:00 UTC is 12:00 Kodi-local (UTC-5, e.g. Bogota) and 17:00 in UTC.
  REQUIRE(ComputeRecurringRuleFields(17 * 3600, 18 * 3600, 0, 0b0011111u, 0, FixedOffset(-300), 1767225600, daysOfWeek,
                                     startSeconds, endSeconds, startDate, error));
  CHECK(daysOfWeek == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("ComputeRecurringRuleFields still rejects an empty weekday list after rotation", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  CHECK_FALSE(ComputeRecurringRuleFields(3600, 7200, 0, 0, 0, FixedOffset(-300), 0, daysOfWeek, startSeconds,
                                         endSeconds, startDate, error));
  CHECK(error == "At least one day of the week must be selected");
  // A mask with only bits above Sunday set selects no real weekday either.
  error.clear();
  CHECK_FALSE(ComputeRecurringRuleFields(3600, 7200, 0, 0xFFFFFF80u, 0, FixedOffset(-300), 0, daysOfWeek, startSeconds,
                                         endSeconds, startDate, error));
  CHECK(error == "At least one day of the week must be selected");
}

TEST_CASE("ComputeRecurringRuleFields takes the start date from first day's date in Kodi's zone, moved by the shift",
          "[RecurringRuleUtil]")
{
  // Kodi in Bogota (UTC-5, no DST), Dispatcharr UTC. The user picks Wednesday
  // 2026-05-13 as the first day, and the dialog hands back Kodi-local
  // midnight of it: 2026-05-13 05:00 UTC. A rule at 20:00 Kodi-local starts
  // producing occurrences that evening, which is Thursday 2026-05-14 01:00
  // UTC -- so Dispatcharr's own start date must be 2026-05-14, not the UTC
  // date of the instant the dialog returned.
  constexpr time_t kFirstDay = 1778648400;             // 2026-05-13 05:00:00 UTC == 2026-05-13 00:00 Bogota
  constexpr time_t kStartTime = kFirstDay + 20 * 3600; // 2026-05-14 01:00:00 UTC == 2026-05-13 20:00 Bogota
  constexpr time_t kEndTime = kStartTime + 3600;
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  REQUIRE(ComputeRecurringRuleFields(kStartTime, kEndTime, kFirstDay, 0b1111111u, 0, FixedOffset(-300), 0, daysOfWeek,
                                     startSeconds, endSeconds, startDate, error));
  CHECK(startDate == 1778716800); // 2026-05-14 00:00:00 UTC
}

TEST_CASE("ComputeRecurringRuleFields ignores the time of day of first day", "[RecurringRuleUtil]")
{
  // Kodi's dialog only ever swaps the date part of first day, so the time
  // carried along with it is whatever it happened to be; the result must
  // depend only on the Kodi-local date.
  constexpr time_t kStart = 1778648400 + 20 * 3600; // 2026-05-13 20:00 Bogota (UTC-5)
  const time_t localMidnight = 1778648400;          // 2026-05-13 00:00 Bogota
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0, unused = 0;
  time_t expected = -1, other = -2;
  std::string error;

  REQUIRE(ComputeRecurringRuleFields(kStart, kStart + 3600, localMidnight, 0b1111111u, 0, FixedOffset(-300), 0,
                                     daysOfWeek, startSeconds, endSeconds, expected, error));
  for (time_t secondOfDay : {1, 3600, 43200, 86399})
  {
    REQUIRE(ComputeRecurringRuleFields(kStart, kStart + 3600, localMidnight + secondOfDay, 0b1111111u, 0,
                                       FixedOffset(-300), 0, daysOfWeek, unused, unused, other, error));
    CHECK(other == expected);
  }
}

TEST_CASE("ComputeRecurringRuleFields with no first day starts on Dispatcharr's own current date regardless of the "
          "shift",
          "[RecurringRuleUtil]")
{
  // Dispatcharr is in Bogota (UTC-5); Kodi is Tokyo. "Now" is
  // 2026-05-07 10:00 UTC, which is 05:00 that day in Bogota -- Dispatcharr's
  // date is 2026-05-07 even though this rule's own shift is -1.
  constexpr time_t kNow = 1778148000; // 2026-05-07 10:00:00 UTC
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  REQUIRE(ComputeRecurringRuleFields(3 * 3600, 4 * 3600, /*firstDay=*/0, 0b1111111u, -300, FixedOffset(540), kNow,
                                     daysOfWeek, startSeconds, endSeconds, startDate, error));
  CHECK(startDate == 1778112000); // 2026-05-07 00:00:00 UTC, read as Bogota's own calendar date
}

TEST_CASE("ComputeRecurringRuleDisplayTimes reports the day shift and names the first day in Kodi's own calendar",
          "[RecurringRuleUtil]")
{
  // The mirror of the create-path vector above: a Dispatcharr (UTC) rule on
  // Tuesday-Saturday at 01:00-02:00, starting Thursday 2026-05-14, shown in
  // Kodi running in Bogota (UTC-5). 01:00 UTC is 20:00 the evening
  // before there, so Kodi's calendar is a day behind and the rule is
  // Monday-Friday at 8:00 PM, starting Wednesday 2026-05-13 (named by its
  // local noon, 2026-05-13 12:00 Bogota time).
  constexpr time_t kStartDate = 1778716800; // 2026-05-14 00:00:00 UTC
  time_t startTime = 0, endTime = 0, firstDay = 0;
  int dayShift = 0;

  ComputeRecurringRuleDisplayTimes(kStartDate, /*startTimeOfDaySeconds=*/3600, /*endTimeOfDaySeconds=*/7200,
                                   FixedOffset(0), FixedOffset(-300), startTime, endTime, firstDay, dayShift);

  CHECK(startTime == kStartDate + 3600);
  CHECK(endTime == kStartDate + 7200);
  CHECK(dayShift == 1);
  CHECK(firstDay == 1778648400 + 12 * 3600); // 2026-05-13 17:00:00 UTC == Wednesday 12:00 in Bogota
  CHECK(RotateWeekdaysBitmask(0b0111110u /* Tue-Sat */, -dayShift) == 0b0011111u /* Mon-Fri */);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes reports no shift when both zones match", "[RecurringRuleUtil]")
{
  for (int offset : kRealWorldOffsets)
  {
    time_t startTime = 0, endTime = 0, firstDay = 0;
    int dayShift = 99;
    ComputeRecurringRuleDisplayTimes(/*startDate=*/20000 * kDay, /*startTimeOfDaySeconds=*/82800,
                                     /*endTimeOfDaySeconds=*/3600, FixedOffset(offset), FixedOffset(offset), startTime,
                                     endTime, firstDay, dayShift);
    CHECK(dayShift == 0);
    // Kodi's local noon of startDate's own date.
    CHECK(firstDay == 20000 * kDay + 12 * 3600 - offset * 60);
  }
}

namespace
{

// The instants a Dispatcharr recurring rule fires in [from, to): every date
// from `startDate` on whose weekday is selected, at `startSecondsOfDay` in a
// zone `offsetMinutes` east of UTC.
std::vector<int64_t> DispatcharrOccurrences(unsigned int dispatcharrMask, int startSecondsOfDay, int64_t startDate,
                                            int offsetMinutes, int64_t from, int64_t to)
{
  std::vector<int64_t> instants;
  const int64_t timeOfDay = ((startSecondsOfDay % kDay) + kDay) % kDay;
  for (int64_t day = startDate / kDay; day < startDate / kDay + 90; ++day)
  {
    const int64_t instant = day * kDay + timeOfDay - int64_t{offsetMinutes} * 60;
    if (instant >= from && instant < to && (dispatcharrMask & (1u << WeekdayOfDay(day))))
      instants.push_back(instant);
  }
  return instants;
}

// The instants a recurring timer, as Kodi's own dialog describes it, means:
// every Kodi-local date from first day on whose weekday is selected, at the
// start instant's time of day *by Kodi's local clock*.
std::vector<int64_t> KodiOccurrences(unsigned int kodiMask, int64_t startInstant, int64_t firstDayInstant,
                                     int offsetMinutes, int64_t from, int64_t to)
{
  std::vector<int64_t> instants;
  const int64_t timeOfDay = ((startInstant + int64_t{offsetMinutes} * 60) % kDay + kDay) % kDay;
  const int64_t firstLocalDay = LocalDay(firstDayInstant, offsetMinutes);
  for (int64_t day = firstLocalDay; day < firstLocalDay + 90; ++day)
  {
    const int64_t instant = day * kDay + timeOfDay - int64_t{offsetMinutes} * 60;
    if (instant >= from && instant < to && (kodiMask & (1u << WeekdayOfDay(day))))
      instants.push_back(instant);
  }
  return instants;
}

constexpr int kWindowDays = 28;

const unsigned int kSampleMasks[] = {0b0000001u, 0b1000000u, 0b0011111u, 0b1100000u, 0b0101010u, 0b1111111u};
const int kSampleUtcSeconds[] = {0, 1800, 3600, 7200, 21599, 21600, 36000, 43200, 54000, 64799, 64800, 79200, 86399};

} // namespace

TEST_CASE("A recurring timer made in Kodi fires on exactly the instants Kodi's own dialog describes, for every pair "
          "of zones",
          "[RecurringRuleUtil]")
{
  // The end-to-end meaning of the fix, against a model written without any
  // reference to the shift: the set of instants Dispatcharr will record,
  // from the rule this function produces, must equal the set of instants
  // "these local weekdays, at this local time, from this local date" means
  // -- for every combination of Kodi's zone, Dispatcharr's zone, time of
  // day, weekday selection and first day.
  int combinations = 0;
  for (int kodiOffset : kRealWorldOffsets)
  {
    for (int dispatcharrOffset : kRealWorldOffsets)
    {
      for (int utcSeconds : kSampleUtcSeconds)
      {
        for (unsigned int kodiMask : kSampleMasks)
        {
          for (int64_t firstLocalDay : {20000, 20454})
          {
            for (int64_t firstDayTimeOfDay : {0, 43200, 86399})
            {
              const int64_t startInstant = 20100 * kDay + utcSeconds;
              const int64_t firstDayInstant = firstLocalDay * kDay - int64_t{kodiOffset} * 60 + firstDayTimeOfDay;

              std::vector<int> days;
              int startSeconds = 0, endSeconds = 0;
              time_t startDate = 0;
              std::string error;
              REQUIRE(ComputeRecurringRuleFields(
                  static_cast<time_t>(startInstant), static_cast<time_t>(startInstant + 3600),
                  static_cast<time_t>(firstDayInstant), kodiMask, dispatcharrOffset, FixedOffset(kodiOffset),
                  /*nowUtc=*/0, days, startSeconds, endSeconds, startDate, error));

              const int64_t from = firstLocalDay * kDay - int64_t{kodiOffset} * 60;
              const int64_t to = from + kWindowDays * kDay;
              const auto fromKodi = KodiOccurrences(kodiMask, startInstant, firstDayInstant, kodiOffset, from, to);
              const auto fromDispatcharr =
                  DispatcharrOccurrences(MaskOf(days), startSeconds, startDate, dispatcharrOffset, from, to);
              REQUIRE(fromKodi.size() > 0);
              REQUIRE(fromKodi == fromDispatcharr);
              ++combinations;
            }
          }
        }
      }
    }
  }
  CHECK(combinations > 0);
}

TEST_CASE("A recurring rule read from Dispatcharr is shown in Kodi on exactly the instants it fires, for every pair "
          "of zones",
          "[RecurringRuleUtil]")
{
  int combinations = 0;
  for (int kodiOffset : kRealWorldOffsets)
  {
    for (int dispatcharrOffset : kRealWorldOffsets)
    {
      for (int utcSeconds : kSampleUtcSeconds)
      {
        for (unsigned int dispatcharrMask : kSampleMasks)
        {
          for (int64_t startDay : {20000, 20454})
          {
            // The rule's time of day as Dispatcharr stores it: a wall-clock
            // time in its own zone, 0..86399.
            const int startTimeOfDay =
                static_cast<int>(((utcSeconds + dispatcharrOffset * 60) % 86400 + 86400) % 86400);
            const int endTimeOfDay = (startTimeOfDay + 3600) % 86400;

            time_t startTime = 0, endTime = 0, firstDay = 0;
            int dayShift = 0;
            ComputeRecurringRuleDisplayTimes(static_cast<time_t>(startDay * kDay), startTimeOfDay, endTimeOfDay,
                                             FixedOffset(dispatcharrOffset), FixedOffset(kodiOffset), startTime,
                                             endTime, firstDay, dayShift);
            const unsigned int kodiMask = RotateWeekdaysBitmask(dispatcharrMask, -dayShift);

            const int64_t from = LocalDay(firstDay, kodiOffset) * kDay - int64_t{kodiOffset} * 60;
            const int64_t to = from + kWindowDays * kDay;
            const auto fromDispatcharr =
                DispatcharrOccurrences(dispatcharrMask, startTimeOfDay, startDay * kDay, dispatcharrOffset, from, to);
            const auto fromKodi = KodiOccurrences(kodiMask, startTime, firstDay, kodiOffset, from, to);
            REQUIRE(fromDispatcharr.size() > 0);
            REQUIRE(fromKodi == fromDispatcharr);
            ++combinations;
          }
        }
      }
    }
  }
  CHECK(combinations > 0);
}

TEST_CASE("Showing a Dispatcharr rule in Kodi and sending it back unchanged reproduces it exactly, for every pair "
          "of zones",
          "[RecurringRuleUtil]")
{
  // Kodi echoes back what it was given on any edit, including one that
  // changes nothing about the schedule -- so a round trip that drifted by
  // even a day would silently move the rule on every enable/disable toggle.
  for (int kodiOffset : kRealWorldOffsets)
  {
    for (int dispatcharrOffset : kRealWorldOffsets)
    {
      for (int utcSeconds : kSampleUtcSeconds)
      {
        for (unsigned int dispatcharrMask : kSampleMasks)
        {
          const int startTimeOfDay = static_cast<int>(((utcSeconds + dispatcharrOffset * 60) % 86400 + 86400) % 86400);
          const int endTimeOfDay = (startTimeOfDay + 5400) % 86400;
          const time_t startDate = 20454 * kDay;

          time_t startTime = 0, endTime = 0, firstDay = 0;
          int dayShift = 0;
          ComputeRecurringRuleDisplayTimes(startDate, startTimeOfDay, endTimeOfDay, FixedOffset(dispatcharrOffset),
                                           FixedOffset(kodiOffset), startTime, endTime, firstDay, dayShift);
          const unsigned int kodiMask = RotateWeekdaysBitmask(dispatcharrMask, -dayShift);

          std::vector<int> days;
          int startSeconds = 0, endSeconds = 0;
          time_t startDateBack = 0;
          std::string error;
          REQUIRE(ComputeRecurringRuleFields(startTime, endTime, firstDay, kodiMask, dispatcharrOffset,
                                             FixedOffset(kodiOffset), /*nowUtc=*/0, days, startSeconds, endSeconds,
                                             startDateBack, error));

          CHECK(MaskOf(days) == dispatcharrMask);
          CHECK(startDateBack == startDate);
          CHECK(((startSeconds % 86400) + 86400) % 86400 == startTimeOfDay);
          CHECK(((endSeconds % 86400) + 86400) % 86400 == endTimeOfDay);
        }
      }
    }
  }
}

TEST_CASE("A Kodi-side edit of a recurring rule keeps naming the same first day when nothing else changed",
          "[RecurringRuleUtil]")
{
  // Applying the round trip twice must be the same as applying it once --
  // no per-edit drift of the start date, which was the original bug
  // ComputeRecurringRuleDisplayTimes()'s own firstDayOut exists to prevent.
  for (int kodiOffset : kRealWorldOffsets)
  {
    for (int dispatcharrOffset : {-360, 0, 540})
    {
      time_t startDate = 20454 * kDay;
      int startTimeOfDay = 82800, endTimeOfDay = 3600;
      unsigned int dispatcharrMask = 0b0011111u;
      const time_t originalStartDate = startDate;
      const unsigned int originalMask = dispatcharrMask;

      for (int edit = 0; edit < 4; ++edit)
      {
        time_t startTime = 0, endTime = 0, firstDay = 0;
        int dayShift = 0;
        ComputeRecurringRuleDisplayTimes(startDate, startTimeOfDay, endTimeOfDay, FixedOffset(dispatcharrOffset),
                                         FixedOffset(kodiOffset), startTime, endTime, firstDay, dayShift);
        const unsigned int kodiMask = RotateWeekdaysBitmask(dispatcharrMask, -dayShift);
        std::vector<int> days;
        int startSeconds = 0, endSeconds = 0;
        std::string error;
        REQUIRE(ComputeRecurringRuleFields(startTime, endTime, firstDay, kodiMask, dispatcharrOffset,
                                           FixedOffset(kodiOffset), 0, days, startSeconds, endSeconds, startDate,
                                           error));
        dispatcharrMask = MaskOf(days);
        startTimeOfDay = ((startSeconds % 86400) + 86400) % 86400;
        endTimeOfDay = ((endSeconds % 86400) + 86400) % 86400;
      }
      CHECK(startDate == originalStartDate);
      CHECK(dispatcharrMask == originalMask);
    }
  }
}

// ---------------------------------------------------------------------
// Daylight saving. Kodi turns a timer time into local time and back with
// libc's localtime() of *that instant* (CPVRTimerInfoTag::
// ConvertUTCToLocalTime()/ConvertLocalTimeToUTC()), not with one bias for
// every date, so the Kodi offset the shift needs is the one in effect at
// each instant it is asked about -- exactly like Dispatcharr's. A first
// attempt at this fix read it once, at "now", and live it rotated the
// weekdays of a Kodi and a Dispatcharr in the SAME zone: a Monday-Friday
// rule whose start date was in the other daylight-saving period from the day it was viewed, read
// shifted weekdays. Both resolvers below are the addon's real DST table.
// ---------------------------------------------------------------------

TEST_CASE("Kodi and Dispatcharr in the same daylight-saving zone never shift a rule, at any time of year",
          "[RecurringRuleUtil]")
{
  const std::vector<std::string> zones = {"America/New_York", "America/Los_Angeles", "America/Denver", "Europe/London",
                                          "Europe/Berlin"};
  int checked = 0;
  for (const std::string& zone : zones)
  {
    const OffsetFn offset = ZoneOffset(zone);
    // Every day of 2026 and a few weeks either side, across every transition.
    for (int64_t day = 20454 - 30; day < 20454 + 365 + 30; day += 1)
    {
      const time_t startDate = static_cast<time_t>(day * kDay);
      // Times of day kept clear of the 02:00-03:00 gap a spring-forward
      // leaves, where a local time may not exist at all.
      for (int timeOfDay : {0, 1800, 3600, 82800, 84600, 85500, 86399})
      {
        time_t startTime = 0, endTime = 0, firstDay = 0;
        int dayShift = 99;
        ComputeRecurringRuleDisplayTimes(startDate, timeOfDay, (timeOfDay + 1800) % 86400, offset, offset, startTime,
                                         endTime, firstDay, dayShift);
        REQUIRE(dayShift == 0);

        // ...and the way back is the identity on the weekday list and on the
        // start date, whichever side of a transition the dialog's instants fall.
        std::vector<int> days;
        int startSeconds = 0, endSeconds = 0;
        time_t startDateBack = 0;
        std::string error;
        REQUIRE(ComputeRecurringRuleFields(startTime, endTime, firstDay, 0b0011111u, offset(startTime), offset, 0, days,
                                           startSeconds, endSeconds, startDateBack, error));
        REQUIRE(days == std::vector<int>{0, 1, 2, 3, 4});
        REQUIRE(startDateBack == startDate);
        ++checked;
      }
    }
  }
  CHECK(checked > 0);
}

TEST_CASE("A late-night rule whose start date is in the other daylight-saving period is not shifted when Kodi and "
          "Dispatcharr are both in US Eastern",
          "[RecurringRuleUtil]")
{
  const OffsetFn newYork = ZoneOffset("America/New_York");
  constexpr time_t kStartDate = 1767571200; // 2026-01-05 00:00:00 UTC, a Monday, EST
  time_t startTime = 0, endTime = 0, firstDay = 0;
  int dayShift = 99;

  // Monday-Friday 23:30-23:59 New York time, which is 04:30 UTC the next day in winter.
  ComputeRecurringRuleDisplayTimes(kStartDate, 23 * 3600 + 30 * 60, 23 * 3600 + 59 * 60, newYork, newYork, startTime,
                                   endTime, firstDay, dayShift);

  CHECK(startTime == kStartDate + 86400 + 4 * 3600 + 30 * 60);
  CHECK(dayShift == 0);
  // Kodi names the first day by its local noon, 2026-01-05 12:00 EST.
  CHECK(firstDay == kStartDate + 17 * 3600);
}

TEST_CASE("ComputeRecurringRuleFields asks for Kodi's offset at the start time for the shift and at first day for "
          "its date",
          "[RecurringRuleUtil]")
{
  // Kodi in US Eastern: EDT (-240) before 2026-11-01 06:00 UTC, EST (-300) from then on.
  constexpr time_t kFallBack = 1793512800;
  const OffsetFn kodi = [](time_t at) { return at < kFallBack ? -240 : -300; };
  std::vector<int> days;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  // Start 2026-05-16 04:30 UTC: 00:30 EDT by Kodi's clock, the same day as
  // UTC's own, so a Dispatcharr in UTC sees no shift -- where the EST offset
  // a later instant would use (23:30 the evening before) would wrongly give
  // a day.
  constexpr time_t kStart = 1778905800; // 2026-05-16 04:30:00 UTC
  REQUIRE(ComputeRecurringRuleFields(kStart, kStart + 3600, /*firstDay=*/kStart, 0b0000001u, /*offsetMinutes=*/0, kodi,
                                     0, days, startSeconds, endSeconds, startDate, error));
  CHECK(days == std::vector<int>{0});

  // First day 2026-11-05 04:30 UTC is 23:30 on Nov 4 by EST, Kodi's offset
  // at that instant -- the date is Nov 4 (plus the shift, here 0), where
  // reading it with the start's EDT offset would give Nov 5.
  constexpr time_t kFirstDay = 1793853000; // 2026-11-05 04:30:00 UTC
  REQUIRE(ComputeRecurringRuleFields(kStart, kStart + 3600, kFirstDay, 0b0000001u, /*offsetMinutes=*/-240, kodi, 0,
                                     days, startSeconds, endSeconds, startDate, error));
  CHECK(startDate == 1793750400); // 2026-11-04 00:00:00 UTC
}

TEST_CASE("ComputeRecurringRuleDisplayTimes names first day by Kodi's own local noon even across a transition",
          "[RecurringRuleUtil]")
{
  // Kodi in US Mountain falls back from MDT (-360) to MST (-420) at
  // 2026-11-01 08:00 UTC. A Dispatcharr in UTC has a rule at 06:30 starting
  // 2026-11-01: the first occurrence (06:30 UTC) is 00:30 MDT, Nov 1 by
  // Kodi's clock, but noon that day is 12:00 MST -- after the transition --
  // which is 19:00 UTC, not the 18:00 UTC the start's own MDT offset would
  // name.
  constexpr time_t kFallBack = 1793520000;
  const OffsetFn kodi = [](time_t at) { return at < kFallBack ? -360 : -420; };
  constexpr time_t kStartDate = 1793491200; // 2026-11-01 00:00:00 UTC
  time_t startTime = 0, endTime = 0, firstDay = 0;
  int dayShift = 99;

  ComputeRecurringRuleDisplayTimes(kStartDate, 6 * 3600 + 1800, 7 * 3600 + 1800, FixedOffset(0), kodi, startTime,
                                   endTime, firstDay, dayShift);

  CHECK(startTime == kStartDate + 6 * 3600 + 1800);
  CHECK(dayShift == 0);
  CHECK(firstDay == 1793559600); // 2026-11-01 19:00:00 UTC == 12:00 MST
  // Asked what date that instant is, Kodi's own offset at it gives Nov 1 back.
  CHECK(LocalDay(firstDay, kodi(firstDay)) == kStartDate / kDay);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes asks for Kodi's offset at the first occurrence, not at the start "
          "date's midnight",
          "[RecurringRuleUtil]")
{
  // A synthetic Kodi zone that falls back from -300 to -360 at 2026-11-01
  // 05:00 UTC. A Dispatcharr in UTC has a rule at 05:30 starting 2026-11-01:
  // the start date's own midnight (00:00 UTC) is still on the -300 side, but
  // the first occurrence (05:30 UTC) is on the -360 side, where Kodi's clock
  // reads 23:30 on Oct 31 -- a day behind UTC's, so the shift is +1. Asking
  // at the midnight instead would read 00:30 Nov 1 and report no shift at all.
  constexpr time_t kTransition = 1793491200 + 5 * 3600; // 2026-11-01 05:00:00 UTC
  const OffsetFn kodi = [](time_t at) { return at < kTransition ? -300 : -360; };
  constexpr time_t kStartDate = 1793491200; // 2026-11-01 00:00:00 UTC
  time_t startTime = 0, endTime = 0, firstDay = 0;
  int dayShift = 99;

  ComputeRecurringRuleDisplayTimes(kStartDate, 5 * 3600 + 1800, 6 * 3600 + 1800, FixedOffset(0), kodi, startTime,
                                   endTime, firstDay, dayShift);

  CHECK(dayShift == 1);
  // Kodi's date for that occurrence is Oct 31, named by its local noon
  // (12:00 at the pre-transition -300 offset): 17:00 UTC.
  CHECK(firstDay == kStartDate - 86400 + 17 * 3600);
  CHECK(LocalDay(firstDay, kodi(firstDay)) == kStartDate / kDay - 1);
}

TEST_CASE("For Kodi and Dispatcharr in different daylight-saving zones a rule still fires on the instants Kodi "
          "describes, in any stretch with no transition",
          "[RecurringRuleUtil]")
{
  // Transition-free stretches of 2026 for both the US and the EU tables:
  // mid-January, mid-June and mid-December.
  const std::vector<std::string> zones = {"America/New_York", "America/Los_Angeles", "America/Denver",
                                          "America/Phoenix",  "Pacific/Honolulu",    "Europe/London",
                                          "Europe/Berlin",    "Europe/Helsinki",     "Europe/Lisbon"};
  int combinations = 0;
  for (const std::string& kodiZone : zones)
  {
    for (const std::string& dispatcharrZone : zones)
    {
      const OffsetFn kodi = ZoneOffset(kodiZone);
      const OffsetFn dispatcharr = ZoneOffset(dispatcharrZone);
      for (int64_t startLocalDay : {20468 /* 2026-01-15 */, 20619 /* 2026-06-15 */, 20802 /* 2026-12-15 */})
      {
        const int64_t from0 = startLocalDay * kDay - 2 * kDay;
        const int64_t to0 = from0 + (kWindowDays + 6) * kDay;
        // A stretch with a transition in it is a different (and, for one
        // weekday list, inexpressible) question -- see the header comment.
        REQUIRE(kodi(static_cast<time_t>(from0)) == kodi(static_cast<time_t>(to0)));
        REQUIRE(dispatcharr(static_cast<time_t>(from0)) == dispatcharr(static_cast<time_t>(to0)));
        const int kodiOffset = kodi(static_cast<time_t>(from0));
        const int dispatcharrOffset = dispatcharr(static_cast<time_t>(from0));

        for (int utcSeconds : kSampleUtcSeconds)
        {
          for (unsigned int kodiMask : kSampleMasks)
          {
            const int64_t startInstant = startLocalDay * kDay + utcSeconds;
            const int64_t firstDayInstant = startLocalDay * kDay - int64_t{kodiOffset} * 60 + 12 * 3600;
            std::vector<int> days;
            int startSeconds = 0, endSeconds = 0;
            time_t startDate = 0;
            std::string error;
            REQUIRE(ComputeRecurringRuleFields(static_cast<time_t>(startInstant),
                                               static_cast<time_t>(startInstant + 3600),
                                               static_cast<time_t>(firstDayInstant), kodiMask, dispatcharrOffset, kodi,
                                               0, days, startSeconds, endSeconds, startDate, error));

            const int64_t from = startLocalDay * kDay - int64_t{kodiOffset} * 60;
            const int64_t to = from + kWindowDays * kDay;
            REQUIRE(KodiOccurrences(kodiMask, startInstant, firstDayInstant, kodiOffset, from, to) ==
                    DispatcharrOccurrences(MaskOf(days), startSeconds, startDate, dispatcharrOffset, from, to));
            ++combinations;
          }
        }
      }
    }
  }
  CHECK(combinations > 0);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes drops the seconds a Kodi dialog carried", "[RecurringRuleUtil]")
{
  // HH:MM:28 - HH:MM:28, the exact shape seen live from the dialog's default.
  int start = 12 * 3600 + 42 * 60 + 28;
  int end = 14 * 3600 + 42 * 60 + 28;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 12 * 3600 + 42 * 60);
  CHECK(end == 14 * 3600 + 42 * 60);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes leaves whole-minute times alone", "[RecurringRuleUtil]")
{
  int start = 20 * 3600;
  int end = 21 * 3600 + 30 * 60;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 20 * 3600);
  CHECK(end == 21 * 3600 + 30 * 60);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes truncates each time on its own", "[RecurringRuleUtil]")
{
  int start = 8 * 3600 + 59;
  int end = 9 * 3600 + 1;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 8 * 3600);
  CHECK(end == 9 * 3600);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes keeps an overnight rule overnight", "[RecurringRuleUtil]")
{
  int start = 23 * 3600 + 15;
  int end = 1 * 3600 + 15;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 23 * 3600);
  CHECK(end == 1 * 3600);
  CHECK(end < start);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes won't collapse a sub-minute window into a 24-hour rule",
          "[RecurringRuleUtil]")
{
  // Both land on 12:00 once truncated; Dispatcharr would read end <= start as
  // overnight, so the original (still valid) times are kept.
  int start = 12 * 3600 + 10;
  int end = 12 * 3600 + 50;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 12 * 3600 + 10);
  CHECK(end == 12 * 3600 + 50);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes leaves an identical pair identical", "[RecurringRuleUtil]")
{
  int start = 12 * 3600 + 30;
  int end = 12 * 3600 + 30;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == end);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes truncates a negative value toward the earlier minute", "[RecurringRuleUtil]")
{
  // 21:59:30 and 22:30:45, expressed from the previous UTC day. C++'s `%`
  // truncates toward zero, which rounded these UP to 22:00:00 and 22:31:00.
  int start = -7230;
  int end = -5355;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == -7260); // 21:59:00
  CHECK(end == -5400);   // 22:30:00
}

TEST_CASE("TruncateRuleTimesToWholeMinutes handles midnight", "[RecurringRuleUtil]")
{
  int start = 0;
  int end = 59;
  TruncateRuleTimesToWholeMinutes(start, end);
  // 0 and 59 both truncate to 0 -> collapse -> kept as given.
  CHECK(start == 0);
  CHECK(end == 59);
}

TEST_CASE("TruncateRuleTimesToWholeMinutes truncates an equal start and end that both carry seconds",
          "[RecurringRuleUtil]")
{
  // They were equal before and stay equal; only a pair that would COLLAPSE into equal ones is left
  // alone (that reads as an overnight rule server-side).
  int start = 36028;
  int end = 36028;
  TruncateRuleTimesToWholeMinutes(start, end);
  CHECK(start == 36000);
  CHECK(end == 36000);

  int s2 = 36010;
  int e2 = 36050; // different, but the same minute once truncated: left alone
  TruncateRuleTimesToWholeMinutes(s2, e2);
  CHECK(s2 == 36010);
  CHECK(e2 == 36050);
}

TEST_CASE("ComputeRecurringRuleFields defaults the start date to today in Dispatcharr's own calendar",
          "[RecurringRuleUtil]")
{
  // No first day from Kodi: "today" is read in Dispatcharr's zone. At 23:30 UTC with Dispatcharr two hours
  // ahead it is already the next calendar day there, so the default is the NEXT UTC midnight.
  const time_t nowUtc = 1767225600 + 23 * 3600 + 30 * 60; // 2026-01-01T23:30:00Z
  const time_t start = 1767225600 + 20 * 3600;
  const time_t end = start + 3600;
  std::vector<int> days;
  int startSeconds = 0;
  int endSeconds = 0;
  time_t startDate = 0;
  std::string error;
  REQUIRE(ComputeRecurringRuleFields(
      start, end, /*firstDay=*/0, 0x7F, /*offsetMinutes=*/120, [](time_t) { return 0; }, nowUtc, days, startSeconds,
      endSeconds, startDate, error));
  CHECK(startDate == 1767225600 + 86400); // 2026-01-02T00:00:00Z
  // The same instant at offset 0 is still the same day.
  REQUIRE(ComputeRecurringRuleFields(
      start, end, 0, 0x7F, /*offsetMinutes=*/0, [](time_t) { return 0; }, nowUtc, days, startSeconds, endSeconds,
      startDate, error));
  CHECK(startDate == 1767225600);
}

TEST_CASE("ComputeRecurringRuleDisplayTimes resolves a start time inside a spring-forward gap to the offset before it",
          "[RecurringRuleUtil]")
{
  // 02:30 does not exist on the day the clocks go from -420 to -360 at 09:00 UTC. Resolving it oscillates (09:30 UTC is
  // after the change, so -360, which puts it at 08:30 UTC, before it, so -420...). The two refinements this takes
  // land on the pre-transition offset, which is what Django's make_aware resolves a nonexistent time to, and an
  // odd number of refinements would not. This pins that.
  constexpr time_t kSpringDate = 1772928000;
  constexpr time_t kSpringChange = 1772960400;
  auto spring = [](time_t at) { return at >= kSpringChange ? -360 : -420; };
  time_t startTime, endTime, firstDay;
  int dayShift;
  ComputeRecurringRuleDisplayTimes(kSpringDate, 2 * 3600 + 1800, 4 * 3600, spring, FixedOffset(-420), startTime,
                                   endTime, firstDay, dayShift);
  CHECK(startTime == kSpringDate + 9 * 3600 + 1800);
}

#if !defined(_WIN32)
// ---------------------------------------------------------------------
// A rule typed across a clock change, converted the way Kodi really does it (the sixteenth sweep's review of a first
// attempt that gave the end its own instant's offset: it stored 01:30-03:30 as 01:30-04:30 and grew by an hour on
// every no-op edit). CPVRTimerInfoTag::ConvertLocalTimeToUTC() takes the daylight-saving flag from localtime() of the
// typed fields READ AS A UTC INSTANT -- an early-morning time lands on the previous evening, before a spring-forward --
// and then calls mktime(), so for a rule typed across a change both times arrive with the same flag. Needs POSIX
// setenv/tzset and a POSIX TZ string (no tzdata).
// ---------------------------------------------------------------------

namespace
{

template <typename Body> void WithKodiZone(Body body)
{
  const char* originalTz = getenv("TZ");
  const std::string savedTz = originalTz ? originalTz : "";
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();
  body();
  if (originalTz)
    setenv("TZ", savedTz.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();
}

// Kodi's ConvertLocalTimeToUTC() applied to the local fields typed in the dialog.
time_t KodiLocalFieldsToUtc(int year, int month1, int day, int hour, int minute)
{
  tm fields{};
  fields.tm_year = year - 1900;
  fields.tm_mon = month1 - 1;
  fields.tm_mday = day;
  fields.tm_hour = hour;
  fields.tm_min = minute;
  const time_t pseudoInstant = PortableTimeGm(&fields); // the local fields read as a UTC instant
  tm local{};
  localtime_r(&pseudoInstant, &local);
  tm gm{};
  GmTimeUtc(pseudoInstant, &gm);
  gm.tm_isdst = local.tm_isdst;
  return mktime(&gm);
}

// What the dialog shows for an instant and what a save of it turns back into.
time_t KodiShowAndSave(time_t instant)
{
  tm local{};
  localtime_r(&instant, &local);
  return KodiLocalFieldsToUtc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);
}

struct StoredRule
{
  int startSeconds;
  int endSeconds;
};

StoredRule StoreAsKodiSendsIt(time_t start, time_t end)
{
  const OffsetFn dispatcharr = ZoneOffset("America/New_York");
  std::vector<int> days;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;
  REQUIRE(ComputeRecurringRuleFields(start, end, start, 0x7Fu, dispatcharr(start), LocalUtcOffsetMinutes, start, days,
                                     startSeconds, endSeconds, startDate, error));
  return {startSeconds, endSeconds};
}

} // namespace

TEST_CASE("A rule typed across a spring-forward is stored as typed, with Kodi's own conversion", "[RecurringRuleUtil]")
{
  WithKodiZone(
      []
      {
        // 01:30 to 03:30 on 2027-03-14: both arrive as standard-time instants (06:30Z and 08:30Z).
        const time_t start = KodiLocalFieldsToUtc(2027, 3, 14, 1, 30);
        const time_t end = KodiLocalFieldsToUtc(2027, 3, 14, 3, 30);
        CHECK(start == 1805005800);
        CHECK(end == 1805013000);
        const StoredRule stored = StoreAsKodiSendsIt(start, end);
        CHECK(stored.startSeconds == 5400);
        CHECK(stored.endSeconds == 12600);
        // The evening before, overnight: 23:00 to 04:00.
        const StoredRule overnight =
            StoreAsKodiSendsIt(KodiLocalFieldsToUtc(2027, 3, 13, 23, 0), KodiLocalFieldsToUtc(2027, 3, 14, 4, 0));
        CHECK(overnight.startSeconds == 82800);
        CHECK(overnight.endSeconds == 14400);
      });
}

TEST_CASE("A rule typed across a fall-back is stored as typed, with Kodi's own conversion", "[RecurringRuleUtil]")
{
  WithKodiZone(
      []
      {
        const StoredRule stored =
            StoreAsKodiSendsIt(KodiLocalFieldsToUtc(2027, 11, 7, 0, 30), KodiLocalFieldsToUtc(2027, 11, 7, 3, 0));
        CHECK(stored.startSeconds == 1800);
        CHECK(stored.endSeconds == 10800);
      });
}

TEST_CASE("Showing a rule typed across a clock change and saving it unchanged is stable", "[RecurringRuleUtil]")
{
  WithKodiZone(
      []
      {
        // What GetTimers() would show for the stored rule, what Kodi's dialog makes of it on a no-op save, stored
        // again, repeated: the stored times must not drift (the first attempt at an end-instant offset went 03:30,
        // 04:30, 05:30).
        const OffsetFn dispatcharr = ZoneOffset("America/New_York");
        struct Case
        {
          time_t startDate; // UTC midnight of the rule's first day
          int startSeconds;
          int endSeconds;
        };
        for (const Case& c : {Case{1804982400, 5400, 12600},   // 01:30-03:30, 2027-03-14
                              Case{1825545600, 1800, 10800},   // 00:30-03:00, 2027-11-07
                              Case{1804896000, 82800, 14400}}) // 23:00-04:00, 2027-03-13
        {
          StoredRule rule{c.startSeconds, c.endSeconds};
          for (int round = 0; round < 4; ++round)
          {
            time_t shownStart, shownEnd, firstDay;
            int dayShift;
            ComputeRecurringRuleDisplayTimes(c.startDate, rule.startSeconds, rule.endSeconds, dispatcharr,
                                             LocalUtcOffsetMinutes, shownStart, shownEnd, firstDay, dayShift);
            rule = StoreAsKodiSendsIt(KodiShowAndSave(shownStart), KodiShowAndSave(shownEnd));
            INFO("round " << round << " start date " << c.startDate);
            CHECK(rule.startSeconds == c.startSeconds);
            CHECK(rule.endSeconds == c.endSeconds);
          }
        }
      });
}
#endif
