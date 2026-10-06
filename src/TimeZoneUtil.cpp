#include "TimeZoneUtil.h"

#include "TimeUtil.h"

namespace dispatcharr
{

namespace
{

// Day-of-month (1-31) of the nth (1-based) occurrence of `weekday`
// (0=Sunday) in the given UTC calendar month.
int NthWeekdayOfMonth(int year, int month0, int weekday, int n)
{
  tm first{};
  first.tm_year = year - 1900;
  first.tm_mon = month0;
  first.tm_mday = 1;
  time_t firstT = PortableTimeGm(&first);
  tm resolved{};
  GmTimeUtc(firstT, &resolved);
  int offset = (weekday - resolved.tm_wday + 7) % 7;
  return 1 + offset + (n - 1) * 7;
}

// Day-of-month of the LAST occurrence of `weekday` in the given UTC
// calendar month -- the EU DST rule below is defined this way, not by a
// fixed nth-occurrence count.
int LastWeekdayOfMonth(int year, int month0, int weekday)
{
  static constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int days = kDaysInMonth[month0];
  if (month0 == 1) // February leap-year check
  {
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (leap)
      days = 29;
  }
  tm last{};
  last.tm_year = year - 1900;
  last.tm_mon = month0;
  last.tm_mday = days;
  time_t lastT = PortableTimeGm(&last);
  tm resolved{};
  GmTimeUtc(lastT, &resolved);
  int diff = (resolved.tm_wday - weekday + 7) % 7;
  return days - diff;
}

// US/Canada DST rule, stable since the Energy Policy Act of 2005 (in
// effect from 2007 onward): starts 2:00am LOCAL STANDARD time on the 2nd
// Sunday of March, ends 2:00am LOCAL DAYLIGHT time on the 1st Sunday of
// November. Using the calendar year of `nowUtc` for both transition dates
// works correctly year-round with no special-casing: for a "now" before
// this year's March transition, that transition is still in the future
// (not yet DST, correct), and for a "now" after this year's November
// transition, that's also correctly in the past (standard time again).
bool IsUsCanadaDstInEffect(time_t nowUtc, int standardOffsetMinutes)
{
  tm nowTm{};
  GmTimeUtc(nowUtc, &nowTm);
  int year = nowTm.tm_year + 1900;

  tm springLocal{};
  springLocal.tm_year = year - 1900;
  springLocal.tm_mon = 2; // March
  springLocal.tm_mday = NthWeekdayOfMonth(year, 2, 0, 2);
  springLocal.tm_hour = 2;
  time_t springUtc = PortableTimeGm(&springLocal) - standardOffsetMinutes * 60;

  tm fallLocal{};
  fallLocal.tm_year = year - 1900;
  fallLocal.tm_mon = 10; // November
  fallLocal.tm_mday = NthWeekdayOfMonth(year, 10, 0, 1);
  fallLocal.tm_hour = 2;
  int daylightOffsetMinutes = standardOffsetMinutes + 60;
  time_t fallUtc = PortableTimeGm(&fallLocal) - daylightOffsetMinutes * 60;

  return nowUtc >= springUtc && nowUtc < fallUtc;
}

// UK/EU DST rule: starts 01:00 UTC on the last Sunday of March, ends
// 01:00 UTC on the last Sunday of October -- defined directly in UTC
// (unlike the US/Canada rule above), so no per-zone offset is needed for
// the transition moments themselves.
bool IsEuDstInEffect(time_t nowUtc)
{
  tm nowTm{};
  GmTimeUtc(nowUtc, &nowTm);
  int year = nowTm.tm_year + 1900;

  tm springUtcTm{};
  springUtcTm.tm_year = year - 1900;
  springUtcTm.tm_mon = 2; // March
  springUtcTm.tm_mday = LastWeekdayOfMonth(year, 2, 0);
  springUtcTm.tm_hour = 1;
  time_t springUtc = PortableTimeGm(&springUtcTm);

  tm fallUtcTm{};
  fallUtcTm.tm_year = year - 1900;
  fallUtcTm.tm_mon = 9; // October
  fallUtcTm.tm_mday = LastWeekdayOfMonth(year, 9, 0);
  fallUtcTm.tm_hour = 1;
  time_t fallUtc = PortableTimeGm(&fallUtcTm);

  return nowUtc >= springUtc && nowUtc < fallUtc;
}

enum class DstFamily
{
  kNone, // fixed offset year-round, no DST
  kUsCanada,
  kEu,
};

// From 2026-11-01 tzdata (2026c) keeps British Columbia and Alberta on their summer offset for good:
// America/Vancouver stays at UTC-7 and America/Edmonton at UTC-6 instead of falling back, and
// Dispatcharr's scheduler (current tzdata or pytz) follows. The table's US/Canada rule would fall back
// on that date, putting every recurring rule in those zones an hour off (found by the 2026-10-04 eighth
// hardening sweep, by comparing the whole table against zoneinfo). 2026-11-01T00:00:00Z is a moment both
// zones are still on daylight time, so switching to the fixed summer offset there is continuous.
constexpr time_t kPermanentDaylightFromNov2026Utc = 1793491200; // 2026-11-01T00:00:00Z

struct KnownTimeZone
{
  const char* ianaName;
  int standardOffsetMinutes;
  DstFamily family;
  // When non-zero: from this instant the zone stays on standardOffsetMinutes + 60 year-round.
  time_t permanentDaylightFromUtc = 0;
};

// Deliberately narrow: only zones with simple, stable, well-documented DST
// rules (or none at all). Anything not listed here falls back to the
// existing manual recurring_rule_utc_offset_minutes entry -- see
// ComputeKnownZoneOffsetMinutes's own doc comment in this file's header.
// Broadened 2026-09-09 against the real, comprehensive list Dispatcharr's
// own GET /api/core/timezones/ returns (confirmed live and against its
// source, core/api_views.py's TimezoneListView -- sorted pytz.common_timezones,
// ~440 entries): still deliberately far short of that full list, not a
// regression -- every zone here is one this addon can actually compute a
// correct DST-adjusted offset for (kUsCanada or kEu, both hand-verified
// against real, stable, well-documented transition rules) or one confirmed
// to have no DST at all (kNone). A zone merely appearing in Dispatcharr's
// list doesn't mean it's addable here: Southern Hemisphere zones (e.g.
// Australia/Sydney, Pacific/Auckland) transition on the opposite calendar
// schedule (DST starts in local spring, i.e. the Northern Hemisphere's
// autumn) -- neither IsUsCanadaDstInEffect() nor IsEuDstInEffect() models
// that, so adding one under either family would silently compute the
// *wrong* offset for roughly half of every year, worse than the honest
// "unrecognized, falls back to manual" behavior for an unlisted zone. Not
// pursued without a genuine third rule engine for that pattern.
constexpr KnownTimeZone kKnownTimeZones[] = {
    // United States
    {"America/New_York", -300, DstFamily::kUsCanada},
    {"America/Chicago", -360, DstFamily::kUsCanada},
    {"America/Denver", -420, DstFamily::kUsCanada},
    {"America/Los_Angeles", -480, DstFamily::kUsCanada},
    {"America/Anchorage", -540, DstFamily::kUsCanada},
    {"America/Phoenix", -420, DstFamily::kNone},  // Arizona: no DST
    {"Pacific/Honolulu", -600, DstFamily::kNone}, // Hawaii: no DST
    {"America/Detroit", -300, DstFamily::kUsCanada},
    {"America/Indiana/Indianapolis", -300, DstFamily::kUsCanada},
    {"America/Boise", -420, DstFamily::kUsCanada},
    // Canada
    {"America/Toronto", -300, DstFamily::kUsCanada},
    {"America/Winnipeg", -360, DstFamily::kUsCanada},
    {"America/Edmonton", -420, DstFamily::kUsCanada, kPermanentDaylightFromNov2026Utc},
    {"America/Vancouver", -480, DstFamily::kUsCanada, kPermanentDaylightFromNov2026Utc},
    {"America/Halifax", -240, DstFamily::kUsCanada},
    {"America/Regina", -360, DstFamily::kNone}, // Saskatchewan: no DST
    // Mexico: DST abolished nationally in 2022 (except the US-border
    // strip, not modeled here) -- fixed offset, confirmed current policy.
    {"America/Mexico_City", -360, DstFamily::kNone},
    // UK/Ireland
    {"Europe/London", 0, DstFamily::kEu},
    {"Europe/Dublin", 0, DstFamily::kEu},
    {"Europe/Lisbon", 0, DstFamily::kEu}, // Portugal: WET/WEST, same EU dates as UK
    // Central Europe
    {"Europe/Paris", 60, DstFamily::kEu},
    {"Europe/Berlin", 60, DstFamily::kEu},
    {"Europe/Madrid", 60, DstFamily::kEu},
    {"Europe/Rome", 60, DstFamily::kEu},
    {"Europe/Amsterdam", 60, DstFamily::kEu},
    {"Europe/Brussels", 60, DstFamily::kEu},
    {"Europe/Vienna", 60, DstFamily::kEu},
    {"Europe/Zurich", 60, DstFamily::kEu},
    {"Europe/Warsaw", 60, DstFamily::kEu},
    {"Europe/Prague", 60, DstFamily::kEu},
    {"Europe/Stockholm", 60, DstFamily::kEu},
    {"Europe/Copenhagen", 60, DstFamily::kEu},
    {"Europe/Oslo", 60, DstFamily::kEu},
    {"Europe/Budapest", 60, DstFamily::kEu},
    // Eastern Europe
    {"Europe/Helsinki", 120, DstFamily::kEu},
    {"Europe/Athens", 120, DstFamily::kEu},
    {"Europe/Bucharest", 120, DstFamily::kEu},
    {"Europe/Riga", 120, DstFamily::kEu},
    {"Europe/Vilnius", 120, DstFamily::kEu},
    {"Europe/Sofia", 120, DstFamily::kEu},
    {"Europe/Kyiv", 120, DstFamily::kEu}, // EU-aligned transition dates
    // Asia/Middle East -- all fixed offset, none observe DST
    {"Asia/Tokyo", 540, DstFamily::kNone},
    {"Asia/Shanghai", 480, DstFamily::kNone},
    {"Asia/Hong_Kong", 480, DstFamily::kNone},
    {"Asia/Singapore", 480, DstFamily::kNone},
    {"Asia/Kolkata", 330, DstFamily::kNone},
    {"Asia/Dubai", 240, DstFamily::kNone},
    {"Asia/Karachi", 300, DstFamily::kNone},
    // Africa -- fixed offset, no DST
    {"Africa/Johannesburg", 120, DstFamily::kNone},
    {"Africa/Lagos", 60, DstFamily::kNone},
    // No-DST reference
    {"UTC", 0, DstFamily::kNone},
    {"Etc/UTC", 0, DstFamily::kNone},
};

// Older names for zones in kKnownTimeZones. Dispatcharr's own settings page
// fills its timezone picker from the browser's Intl.supportedValuesOf(
// 'timeZone'), and Chromium's engine still lists the pre-rename ids -- checked
// against a real V8 (Node 22, ICU 78): it offers America/Indianapolis,
// Asia/Calcutta and Europe/Kiev and none of America/Indiana/Indianapolis,
// Asia/Kolkata or Europe/Kyiv -- so picking India in Chrome stores
// "Asia/Calcutta", which the exact-match lookup below used to reject, leaving
// recurring timers on the manual offset. The rest are IANA's own `backward`
// links to a zone in the table (the old US/*, Canada/* and country-name ids a
// server admin might set through the API), all the same rules today.
struct ZoneAlias
{
  const char* alias;
  const char* canonical;
};

constexpr ZoneAlias kZoneAliases[] = {
    {"US/Eastern", "America/New_York"},
    {"US/Central", "America/Chicago"},
    {"US/Mountain", "America/Denver"},
    {"Navajo", "America/Denver"},
    {"America/Shiprock", "America/Denver"},
    {"US/Pacific", "America/Los_Angeles"},
    {"US/Alaska", "America/Anchorage"},
    {"US/Arizona", "America/Phoenix"},
    {"US/Hawaii", "Pacific/Honolulu"},
    {"Pacific/Johnston", "Pacific/Honolulu"},
    {"US/Michigan", "America/Detroit"},
    {"America/Indianapolis", "America/Indiana/Indianapolis"},
    {"America/Fort_Wayne", "America/Indiana/Indianapolis"},
    {"US/East-Indiana", "America/Indiana/Indianapolis"},
    {"Canada/Eastern", "America/Toronto"},
    {"America/Montreal", "America/Toronto"},
    {"America/Nipigon", "America/Toronto"},
    {"America/Thunder_Bay", "America/Toronto"},
    {"Canada/Central", "America/Winnipeg"},
    {"America/Rainy_River", "America/Winnipeg"},
    {"Canada/Mountain", "America/Edmonton"},
    {"America/Yellowknife", "America/Edmonton"},
    {"Canada/Pacific", "America/Vancouver"},
    {"Canada/Atlantic", "America/Halifax"},
    {"Canada/Saskatchewan", "America/Regina"},
    {"Mexico/General", "America/Mexico_City"},
    {"GB", "Europe/London"},
    {"GB-Eire", "Europe/London"},
    {"Europe/Belfast", "Europe/London"},
    {"Eire", "Europe/Dublin"},
    {"Portugal", "Europe/Lisbon"},
    {"Europe/San_Marino", "Europe/Rome"},
    {"Europe/Vatican", "Europe/Rome"},
    {"Europe/Busingen", "Europe/Zurich"},
    {"Poland", "Europe/Warsaw"},
    {"Europe/Bratislava", "Europe/Prague"},
    {"Arctic/Longyearbyen", "Europe/Oslo"},
    {"Atlantic/Jan_Mayen", "Europe/Oslo"},
    {"Europe/Mariehamn", "Europe/Helsinki"},
    {"Europe/Kiev", "Europe/Kyiv"},
    {"Europe/Uzhgorod", "Europe/Kyiv"},
    {"Europe/Zaporozhye", "Europe/Kyiv"},
    {"Japan", "Asia/Tokyo"},
    {"PRC", "Asia/Shanghai"},
    {"Asia/Chongqing", "Asia/Shanghai"},
    {"Asia/Chungking", "Asia/Shanghai"},
    {"Asia/Harbin", "Asia/Shanghai"},
    {"Hongkong", "Asia/Hong_Kong"},
    {"Singapore", "Asia/Singapore"},
    {"Asia/Calcutta", "Asia/Kolkata"},
    {"Etc/UCT", "Etc/UTC"},
    {"Etc/Universal", "Etc/UTC"},
    {"Etc/Zulu", "Etc/UTC"},
    {"UCT", "Etc/UTC"},
    {"Universal", "Etc/UTC"},
    {"Zulu", "Etc/UTC"},
};

} // namespace

std::string CanonicalKnownZoneName(const std::string& ianaZoneName)
{
  for (const auto& alias : kZoneAliases)
  {
    if (ianaZoneName == alias.alias)
      return alias.canonical;
  }
  return ianaZoneName;
}

bool ComputeKnownZoneOffsetMinutes(const std::string& requestedZoneName, time_t nowUtc, int& offsetMinutesOut,
                                   bool applyZoneRuleChanges)
{
  const std::string ianaZoneName = CanonicalKnownZoneName(requestedZoneName);
  for (const auto& zone : kKnownTimeZones)
  {
    if (ianaZoneName != zone.ianaName)
      continue;
    if (applyZoneRuleChanges && zone.permanentDaylightFromUtc != 0 && nowUtc >= zone.permanentDaylightFromUtc)
    {
      offsetMinutesOut = zone.standardOffsetMinutes + 60;
      return true;
    }
    switch (zone.family)
    {
    case DstFamily::kNone:
      offsetMinutesOut = zone.standardOffsetMinutes;
      break;
    case DstFamily::kUsCanada:
      offsetMinutesOut = IsUsCanadaDstInEffect(nowUtc, zone.standardOffsetMinutes) ? zone.standardOffsetMinutes + 60
                                                                                   : zone.standardOffsetMinutes;
      break;
    case DstFamily::kEu:
      offsetMinutesOut = IsEuDstInEffect(nowUtc) ? zone.standardOffsetMinutes + 60 : zone.standardOffsetMinutes;
      break;
    }
    return true;
  }
  return false;
}

std::string DesiredRecurringRuleTimezoneSetting(const std::string& ianaZoneName, time_t nowUtc, bool* knownOut)
{
  int unusedOffset = 0;
  const bool known = ComputeKnownZoneOffsetMinutes(ianaZoneName, nowUtc, unusedOffset);
  if (knownOut)
    *knownOut = known;
  return known ? CanonicalKnownZoneName(ianaZoneName) : "manual";
}

} // namespace dispatcharr
