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

// Seconds since 1970-01-01T00:00:00Z of the UTC date and time in `tmVal`, computed in 64 bits on every platform rather
// than through the libc: Windows' _mkgmtime() answers -1 for any date outside 1970-3000, and a 32-bit time_t's timegm()
// for any date outside 1901-12-13 to 2038-01-19, so a pre-1970 date parsed on Windows (a garbage or hostile server
// timestamp) reached the floor below instead of its real value (docs/CLOSED_ITEMS.md, "Windows (and 32-bit) parse a
// pre-1970 (pre-1901) date to a floor that later subtractions overflow"). Fields outside their range carry over the
// way timegm() carries them (a month of 12 is January of the next year, a second of 60 the next minute). The day count
// is Howard Hinnant's days_from_civil (proleptic Gregorian calendar).
inline long long UtcCivilSeconds(const struct tm& tmVal)
{
  long long year = 1900LL + tmVal.tm_year + tmVal.tm_mon / 12;
  long long month = tmVal.tm_mon % 12; // 0-11 after this
  if (month < 0)
  {
    month += 12;
    --year;
  }
  const long long y = month < 2 ? year - 1 : year;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const long long yearOfEra = y - era * 400;
  const long long monthFromMarch = month < 2 ? month + 10 : month - 2;
  const long long dayOfYear = (153 * monthFromMarch + 2) / 5;
  const long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const long long days = era * 146097 + dayOfEra - 719468 + (tmVal.tm_mday - 1);
  return days * 86400 + tmVal.tm_hour * 3600LL + tmVal.tm_min * 60LL + tmVal.tm_sec;
}

// The bounds PortableTimeGmSaturating() clamps to. With a 64-bit time_t they are a quarter of its range (about 73
// billion years either side), so the difference of any two clamped values, and a day or an offset added to or taken
// from one, fits: the earlier floor, the smallest time_t plus a year, overflowed `now - t` and `end - start` in the
// 64-bit arithmetic of ClampedDurationSeconds(), IsWithinCatchupWindow() and CatchupSessionDurationMinutes(). A 32-bit
// time_t has no range to spare (2038 is near), so it keeps a year of headroom at each end, and its callers subtract
// in 64 bits.
constexpr bool kTimeTIs64Bit = sizeof(time_t) >= 8;
constexpr time_t kLatestRepresentableTime =
    kTimeTIs64Bit ? std::numeric_limits<time_t>::max() / 4 : std::numeric_limits<time_t>::max() - 366 * 86400;
constexpr time_t kEarliestRepresentableTime =
    kTimeTIs64Bit ? std::numeric_limits<time_t>::lowest() / 4 : std::numeric_limits<time_t>::lowest() + 366 * 86400;

// UtcCivilSeconds() clamped to [earliest, latest]. Found by the 2026-10-04 third hardening sweep: on a 32-bit time_t
// (CoreELEC armhf, 32-bit Android) timegm() answered -1 for every date from 2038-01-19, so a rule created in
// Dispatcharr's own UI with an end date of 2099-12-31 read as "no end date" and adoption (and any Kodi edit) replaced
// it with a 30-day window; a date that is representable but later than `latest` (2037-01-18 to 2038-01-19) is clamped
// as well, or it sorted above the saturated value and the callers' day arithmetic on it could overflow (the fourth
// sweep). The low end mirrors it (the 2026-10-18 read-only audit). `latest` and `earliest` are parameters only so a
// test can stand in for a 32-bit time_t on a 64-bit host.
inline time_t SaturatingTimeGm(const struct tm* tmVal, time_t latest = kLatestRepresentableTime,
                               time_t earliest = kEarliestRepresentableTime)
{
  const long long seconds = UtcCivilSeconds(*tmVal);
  if (seconds > static_cast<long long>(latest))
    return latest;
  if (seconds < static_cast<long long>(earliest))
    return earliest;
  return static_cast<time_t>(seconds);
}

inline time_t PortableTimeGmSaturating(const struct tm* tmVal)
{
  return SaturatingTimeGm(tmVal);
}

// The inverse of UtcCivilSeconds(): fills `tmVal` (weekday and day of the year included, tm_isdst 0) with the UTC date
// and time `seconds` after the epoch, in 64 bits on every platform. Windows' gmtime_s() refuses a time after 3000,
// which UtcCivilSeconds() can now produce from a server date (it sets every field to -1, per Microsoft's
// documentation, and the CRT's strftime() rejects that). Returns false, leaving `tmVal` alone, only when the year does
// not fit an int. Howard Hinnant's civil_from_days.
inline bool UtcCivilFromSeconds(long long seconds, struct tm* tmVal)
{
  long long days = seconds / 86400;
  long long secondOfDay = seconds % 86400;
  if (secondOfDay < 0)
  {
    secondOfDay += 86400;
    --days;
  }
  const long long z = days + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const long long dayOfEra = z - era * 146097;
  const long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const long long dayOfYearFromMarch = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const long long monthFromMarch = (5 * dayOfYearFromMarch + 2) / 153;
  const long long day = dayOfYearFromMarch - (153 * monthFromMarch + 2) / 5 + 1;
  const long long month = monthFromMarch < 10 ? monthFromMarch + 3 : monthFromMarch - 9; // 1-12
  const long long year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
  if (year - 1900 > std::numeric_limits<int>::max() || year - 1900 < std::numeric_limits<int>::lowest())
    return false;
  struct tm january1
  {
  };
  january1.tm_year = static_cast<int>(year - 1900);
  january1.tm_mday = 1;
  const long long weekday = (days % 7 + 11) % 7; // 1970-01-01 was a Thursday (4)
  *tmVal = {};
  tmVal->tm_year = static_cast<int>(year - 1900);
  tmVal->tm_mon = static_cast<int>(month - 1);
  tmVal->tm_mday = static_cast<int>(day);
  tmVal->tm_hour = static_cast<int>(secondOfDay / 3600);
  tmVal->tm_min = static_cast<int>(secondOfDay / 60 % 60);
  tmVal->tm_sec = static_cast<int>(secondOfDay % 60);
  tmVal->tm_wday = static_cast<int>(weekday);
  tmVal->tm_yday = static_cast<int>(days - UtcCivilSeconds(january1) / 86400);
  return true;
}

// Portable gmtime(): fills tmValOut from t, interpreting t as UTC (UtcCivilFromSeconds(), so every platform agrees,
// whatever range its libc's gmtime() accepts).
inline void GmTimeUtc(time_t t, struct tm* tmValOut)
{
  UtcCivilFromSeconds(static_cast<long long>(t), tmValOut);
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
