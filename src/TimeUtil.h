#pragma once

// Shared by DispatcharrClient.cpp and XmlTvParser.cpp, which were each
// independently defining byte-identical copies of these two functions --
// pulled out here rather than having one include the other, since both are
// meant to stay self-contained w.r.t. each other (see CLAUDE.md's repo
// layout note).

#include <ctime>
#include <limits>

namespace dispatcharr
{

// Portable timegm(): interprets a struct tm as UTC and returns a time_t,
// without touching the process-wide TZ setting (unlike mktime()).
inline time_t PortableTimeGm(struct tm* tmVal)
{
#if defined(_WIN32)
  return _mkgmtime(tmVal);
#else
  return timegm(tmVal);
#endif
}

// Whether year/month (1-12)/day name a real calendar date, leap years included. The range checks the
// parsers do (day 1-31) let "2019-11-31" through, and timegm() rolls that over into December 1:
// a wrong-but-plausible time (found by the 2026-10-04 eighth hardening sweep against Python's datetime,
// 363 of 20,000 random strings).
inline bool IsValidCivilDate(int year, int month, int day)
{
  if (month < 1 || month > 12 || day < 1)
    return false;
  static const int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = kDaysInMonth[month - 1];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap)
    days = 29;
  return day <= days;
}

// The latest time PortableTimeGmSaturating() returns: the largest time_t less a year, so that the
// day-sized additions and offset arithmetic callers do on an end date cannot themselves overflow.
constexpr time_t kLatestRepresentableTime = std::numeric_limits<time_t>::max() - 366 * 86400;

// timegm() with `gm` (injectable so a test can stand in for a 32-bit libc), except that a date after
// 1970 it cannot represent saturates to kLatestRepresentableTime instead of coming back as -1.
// Found by the 2026-10-04 third hardening sweep: on a 32-bit time_t build (CoreELEC armhf, the primary
// target, and 32-bit Android) every date from 2038-01-19 on parsed to -1, so a rule created in
// Dispatcharr's own UI with an end date of 2099-12-31 read as "no end date" and adoption (and any Kodi
// edit) replaced it with a 30-day window. -1 is also a real time (1969-12-31T23:59:59), so only a year
// after 1970 -- where it can only be the failure value -- is taken as overflow.
//
// A date that IS representable but later than `latest` (on a 32-bit time_t, 2037-01-18 to 2038-01-19)
// is clamped to it as well: left alone it sorted above the saturated value and the callers' day
// arithmetic on it could still overflow (found by the 2026-10-04 fourth hardening sweep). `latest`
// is a parameter only so a test can stand in for a 32-bit time_t on a 64-bit host.
template <typename GmFn>
inline time_t SaturatingTimeGm(struct tm* tmVal, GmFn gm, time_t latest = kLatestRepresentableTime)
{
  const int yearBefore = tmVal->tm_year;
  const time_t t = gm(tmVal);
  if ((t == static_cast<time_t>(-1) && yearBefore > 70) || t > latest)
    return latest;
  return t;
}

inline time_t PortableTimeGmSaturating(struct tm* tmVal)
{
  return SaturatingTimeGm(tmVal, [](struct tm* t) { return PortableTimeGm(t); });
}

// Portable gmtime(): fills tmValOut from t, interpreting t as UTC.
inline void GmTimeUtc(time_t t, struct tm* tmValOut)
{
#if defined(_WIN32)
  gmtime_s(tmValOut, &t);
#else
  gmtime_r(&t, tmValOut);
#endif
}

// Portable localtime(): fills tmValOut from t, interpreting t in this
// process's own local timezone. Returns false if the conversion failed.
inline bool LocalTimeZoned(time_t t, struct tm* tmValOut)
{
#if defined(_WIN32)
  return localtime_s(tmValOut, &t) == 0;
#else
  return localtime_r(&t, tmValOut) != nullptr;
#endif
}

// This process's own UTC offset at `at`, in minutes east of UTC (UTC+9 is
// +540, UTC-6 is -360) -- the same convention TimeZoneUtil's
// ComputeKnownZoneOffsetMinutes() and the recurring_rule_utc_offset_minutes
// setting already use. Exists because Kodi's timer dialog converts every
// timer time between local time and UTC by asking libc's localtime() about
// the very instant being converted (CPVRTimerInfoTag::ConvertUTCToLocalTime()/
// ConvertLocalTimeToUTC(), PVRTimerInfoTag.cpp), so this is exactly the
// offset Kodi applies to that instant, daylight saving included, and a
// recurring rule's weekday list is read in that same local calendar -- see
// RecurringRuleUtil.h's ComputeRecurringRuleDayShift(). Computed as the
// difference between the broken-down local time read back as UTC and `at`
// itself rather than from the non-standard tm_gmtoff, so it needs nothing
// the Windows CRT lacks. Returns 0 if the local conversion fails (Windows'
// localtime_s rejects a negative time_t).
inline int LocalUtcOffsetMinutes(time_t at)
{
  struct tm local = {};
  if (!LocalTimeZoned(at, &local))
    return 0;
  const time_t asIfUtc = PortableTimeGm(&local);
  return static_cast<int>((asIfUtc - at) / 60);
}

} // namespace dispatcharr
