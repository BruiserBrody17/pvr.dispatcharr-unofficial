#include "TimeUtil.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

using namespace dispatcharr;

TEST_CASE("PortableTimeGm converts a known UTC date to the correct epoch", "[TimeUtil]")
{
  tm tmVal{};
  tmVal.tm_year = 2026 - 1900;
  tmVal.tm_mon = 0; // January
  tmVal.tm_mday = 1;
  tmVal.tm_hour = 0;
  tmVal.tm_min = 0;
  tmVal.tm_sec = 0;

  REQUIRE(PortableTimeGm(&tmVal) == 1767225600);
}

TEST_CASE("GmTimeUtc fills a struct tm matching a known UTC epoch", "[TimeUtil]")
{
  time_t t = 1781526645; // 2026-06-15 12:30:45 UTC
  tm tmVal{};
  GmTimeUtc(t, &tmVal);

  CHECK(tmVal.tm_year == 2026 - 1900);
  CHECK(tmVal.tm_mon == 5); // June, 0-indexed
  CHECK(tmVal.tm_mday == 15);
  CHECK(tmVal.tm_hour == 12);
  CHECK(tmVal.tm_min == 30);
  CHECK(tmVal.tm_sec == 45);
}

TEST_CASE("PortableTimeGm and GmTimeUtc round-trip", "[TimeUtil]")
{
  time_t original = 1781526645;
  tm tmVal{};
  GmTimeUtc(original, &tmVal);
  REQUIRE(PortableTimeGm(&tmVal) == original);
}

#if !defined(_WIN32)
// PortableTimeGm/GmTimeUtc exist specifically to avoid mktime()/localtime()'s
// dependence on the process-wide TZ setting (see TimeUtil.h's own comment).
// This guards against a regression back to that behavior on POSIX, where
// setenv/tzset are standard; _mkgmtime/gmtime_s are already documented to
// ignore TZ on Windows, so there's nothing analogous to regress there.
TEST_CASE("PortableTimeGm ignores the process TZ setting", "[TimeUtil]")
{
  tm tmVal{};
  tmVal.tm_year = 2026 - 1900;
  tmVal.tm_mon = 0;
  tmVal.tm_mday = 1;
  tmVal.tm_hour = 0;
  tmVal.tm_min = 0;
  tmVal.tm_sec = 0;
  time_t utcResult = PortableTimeGm(&tmVal);

  const char* originalTz = getenv("TZ");
  std::string savedTz = originalTz ? originalTz : "";
  // A POSIX TZ string needs no tz database: an IANA name is silently read as UTC where there is none, which
  // made this test pass for a regression back to mktime() on a bare CI image (found by the thirteenth sweep).
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
  tzset();

  tm tmValAgain = tmVal;
  time_t resultWithTz = PortableTimeGm(&tmValAgain);

  if (originalTz)
    setenv("TZ", savedTz.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();

  REQUIRE(utcResult == 1767225600); // 2026-01-01T00:00:00Z
  REQUIRE(resultWithTz == utcResult);
}

namespace
{

// Runs `body` with the process TZ set to a POSIX TZ string (not an IANA
// name, so this needs no tzdata on the machine running the tests), restoring
// whatever was there before.
template <typename Body> void WithTimeZone(const char* posixTz, Body body)
{
  const char* originalTz = getenv("TZ");
  const std::string savedTz = originalTz ? originalTz : "";
  setenv("TZ", posixTz, 1);
  tzset();
  body();
  if (originalTz)
    setenv("TZ", savedTz.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();
}

constexpr time_t kMidJanuary2026 = 1768478400; // 2026-01-15 12:00:00 UTC
constexpr time_t kMidJuly2026 = 1784116800;    // 2026-07-15 12:00:00 UTC

} // namespace

TEST_CASE("LocalUtcOffsetMinutes is the process's own UTC offset, east positive", "[TimeUtil]")
{
  WithTimeZone("UTC0",
               []
               {
                 CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == 0);
                 CHECK(LocalUtcOffsetMinutes(kMidJuly2026) == 0);
               });
  WithTimeZone("JST-9", [] { CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == 540); });
  WithTimeZone("<-03>3", [] { CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == -180); });
}

TEST_CASE("LocalUtcOffsetMinutes handles a zone that isn't a whole number of hours", "[TimeUtil]")
{
  WithTimeZone("<+0530>-5:30", [] { CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == 330); });
  WithTimeZone("<+0545>-5:45", [] { CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == 345); });
  WithTimeZone("<-0330>3:30", [] { CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == -210); });
}

TEST_CASE("LocalUtcOffsetMinutes follows daylight saving time", "[TimeUtil]")
{
  // US Eastern, POSIX form: EST (UTC-5) in winter, EDT (UTC-4) from the
  // second Sunday in March to the first in November.
  WithTimeZone("EST5EDT,M3.2.0,M11.1.0",
               []
               {
                 CHECK(LocalUtcOffsetMinutes(kMidJanuary2026) == -300);
                 CHECK(LocalUtcOffsetMinutes(kMidJuly2026) == -240);
               });
}

TEST_CASE("LocalUtcOffsetMinutes switches exactly at a daylight saving transition", "[TimeUtil]")
{
  // 2026-11-01 06:00:00 UTC is the instant US Eastern falls back from EDT.
  constexpr time_t kFallBack = 1793512800;
  WithTimeZone("EST5EDT,M3.2.0,M11.1.0",
               []
               {
                 CHECK(LocalUtcOffsetMinutes(kFallBack - 1) == -240);
                 CHECK(LocalUtcOffsetMinutes(kFallBack) == -300);
               });
}
#endif

// ---------------------------------------------------------------------
// UtcCivilSeconds/SaturatingTimeGm. The 2026-10-04 third hardening sweep: on a 32-bit time_t, timegm() returned -1
// for every date from 2038-01-19, which read as "no end date" for a rule ending in 2099. The 2026-10-23 sweep: on
// Windows _mkgmtime() returned -1 for every date before 1970, which saturated to a floor that later subtractions
// overflowed. Both are now computed in 64 bits without the libc, and clamped to bounds a test can set to a 32-bit
// time_t's.
// ---------------------------------------------------------------------

namespace
{
// kLatestRepresentableTime and kEarliestRepresentableTime as a 32-bit time_t has them: a year inside each end.
constexpr time_t kLatest32 = 2147483647LL - 366 * 86400;
constexpr time_t kEarliest32 = -2147483647LL - 1 + 366 * 86400;

tm Utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0)
{
  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  return t;
}
} // namespace

TEST_CASE("UtcCivilSeconds gives the real time of dates on both sides of 1970", "[TimeUtil]")
{
  CHECK(UtcCivilSeconds(Utc(1970, 1, 1)) == 0);
  CHECK(UtcCivilSeconds(Utc(1969, 12, 31, 23, 59, 59)) == -1);
  CHECK(UtcCivilSeconds(Utc(1960, 1, 1)) == -315619200LL);
  CHECK(UtcCivilSeconds(Utc(1900, 1, 1)) == -2208988800LL);
  CHECK(UtcCivilSeconds(Utc(1, 1, 1)) == -62135596800LL);
  CHECK(UtcCivilSeconds(Utc(2000, 2, 29)) == 951782400LL);
  CHECK(UtcCivilSeconds(Utc(2026, 10, 4)) == 1791072000LL);
  CHECK(UtcCivilSeconds(Utc(2099, 12, 31)) == 4102358400LL);
  CHECK(UtcCivilSeconds(Utc(9999, 12, 31, 23, 59, 59)) == 253402300799LL);
}

TEST_CASE("UtcCivilSeconds carries fields outside their range the way timegm() does", "[TimeUtil]")
{
  CHECK(UtcCivilSeconds(Utc(2026, 13, 1)) == UtcCivilSeconds(Utc(2027, 1, 1)));
  CHECK(UtcCivilSeconds(Utc(2026, 0, 1)) == UtcCivilSeconds(Utc(2025, 12, 1)));
  CHECK(UtcCivilSeconds(Utc(2026, -12, 1)) == UtcCivilSeconds(Utc(2024, 12, 1)));
  CHECK(UtcCivilSeconds(Utc(2019, 11, 31)) == UtcCivilSeconds(Utc(2019, 12, 1)));
  CHECK(UtcCivilSeconds(Utc(2026, 6, 30, 23, 59, 60)) == UtcCivilSeconds(Utc(2026, 7, 1)));
}

#if !defined(_WIN32)
TEST_CASE("UtcCivilSeconds agrees with the libc's timegm() wherever a 64-bit time_t can check it", "[TimeUtil]")
{
  if (sizeof(time_t) < 8)
    SKIP("a 32-bit timegm() cannot represent most of this range");
  int checked = 0;
  for (long long day = -150000; day <= 150000; day += 37) // about 1559 to 2380
  {
    tm fromLibc{};
    const time_t t = static_cast<time_t>(day * 86400 + (day % 86400 + 86400) % 86400);
    gmtime_r(&t, &fromLibc);
    tm copy = fromLibc;
    REQUIRE(UtcCivilSeconds(copy) == static_cast<long long>(timegm(&fromLibc)));
    ++checked;
  }
  CHECK(checked > 8000);
}
#endif

#if !defined(_WIN32)
TEST_CASE("UtcCivilFromSeconds agrees with the libc's gmtime_r() field for field", "[TimeUtil]")
{
  const long long lowest = sizeof(time_t) < 8 ? -2147483648LL : -150000LL * 86400;
  const long long highest = sizeof(time_t) < 8 ? 2147483647LL : 150000LL * 86400;
  int checked = 0;
  for (long long t = lowest; t <= highest && t >= lowest; t += 37 * 86400 + 4111)
  {
    const time_t asTimeT = static_cast<time_t>(t);
    tm fromLibc{};
    REQUIRE(gmtime_r(&asTimeT, &fromLibc) != nullptr);
    tm ours{};
    REQUIRE(UtcCivilFromSeconds(t, &ours));
    INFO(t);
    REQUIRE(ours.tm_year == fromLibc.tm_year);
    REQUIRE(ours.tm_mon == fromLibc.tm_mon);
    REQUIRE(ours.tm_mday == fromLibc.tm_mday);
    REQUIRE(ours.tm_hour == fromLibc.tm_hour);
    REQUIRE(ours.tm_min == fromLibc.tm_min);
    REQUIRE(ours.tm_sec == fromLibc.tm_sec);
    REQUIRE(ours.tm_wday == fromLibc.tm_wday);
    REQUIRE(ours.tm_yday == fromLibc.tm_yday);
    REQUIRE(UtcCivilSeconds(ours) == t);
    ++checked;
  }
  CHECK(checked > 1000);
}
#endif

TEST_CASE("UtcCivilFromSeconds formats a date Windows' gmtime_s() refuses, and refuses a year past an int",
          "[TimeUtil]")
{
  tm out{};
  REQUIRE(UtcCivilFromSeconds(UtcCivilSeconds(Utc(3500, 6, 15, 12, 30, 45)), &out));
  CHECK(out.tm_year == 3500 - 1900);
  CHECK(out.tm_mon == 5);
  CHECK(out.tm_mday == 15);
  CHECK(out.tm_hour == 12);
  CHECK(out.tm_min == 30);
  CHECK(out.tm_sec == 45);
  REQUIRE(UtcCivilFromSeconds(-1, &out));
  CHECK(out.tm_year == 69);
  CHECK(out.tm_mon == 11);
  CHECK(out.tm_mday == 31);
  CHECK(out.tm_sec == 59);
  CHECK(out.tm_wday == 3); // a Wednesday
  CHECK(out.tm_yday == 364);
  tm untouched = Utc(2026, 1, 1);
  CHECK_FALSE(UtcCivilFromSeconds(std::numeric_limits<long long>::max(), &untouched));
  CHECK(untouched.tm_year == 2026 - 1900);
}

TEST_CASE("A pre-1970 date parses to its real time on every platform, not to the floor", "[TimeUtil]")
{
  // Windows' _mkgmtime() answered -1 here, which saturated to the floor.
  tm sixties = Utc(1960, 1, 1);
  CHECK(PortableTimeGmSaturating(&sixties) == static_cast<time_t>(-315619200LL));
}

TEST_CASE("SaturatingTimeGm saturates a date a 32-bit time_t cannot represent", "[TimeUtil]")
{
  tm farFuture = Utc(2099, 12, 31);
  CHECK(SaturatingTimeGm(&farFuture, kLatest32, kEarliest32) == kLatest32);
  tm justPast = Utc(2038, 1, 20);
  CHECK(SaturatingTimeGm(&justPast, kLatest32, kEarliest32) == kLatest32);
  tm longAgo = Utc(1800, 1, 1);
  CHECK(SaturatingTimeGm(&longAgo, kLatest32, kEarliest32) == kEarliest32);
}

TEST_CASE("SaturatingTimeGm leaves every date between the bounds, and the real -1 instant, alone", "[TimeUtil]")
{
  tm ordinary = Utc(2026, 10, 4);
  CHECK(SaturatingTimeGm(&ordinary, kLatest32, kEarliest32) == 1791072000);
  tm epoch = Utc(1970, 1, 1);
  CHECK(SaturatingTimeGm(&epoch, kLatest32, kEarliest32) == 0);
  // 1969-12-31T23:59:59 genuinely is -1: not an overflow, so not saturated.
  tm minusOne = Utc(1969, 12, 31, 23, 59, 59);
  CHECK(SaturatingTimeGm(&minusOne, kLatest32, kEarliest32) == -1);
  tm early = Utc(1950, 1, 1);
  CHECK(SaturatingTimeGm(&early, kLatest32, kEarliest32) == -631152000);
}

TEST_CASE("SaturatingTimeGm also clamps a representable date beyond either bound", "[TimeUtil]")
{
  // 2037-06 is below INT32_MAX but above the ceiling: unclamped it sorted above the saturated 2099
  // value, and the callers' day arithmetic on it could overflow.
  tm lastRepresentable = Utc(2038, 1, 19);
  CHECK(SaturatingTimeGm(&lastRepresentable, kLatest32, kEarliest32) == kLatest32);
  tm june2037 = Utc(2037, 6, 1);
  CHECK(SaturatingTimeGm(&june2037, kLatest32, kEarliest32) == kLatest32);
  // ...while a date below the ceiling is untouched, to the second.
  tm early2037 = Utc(2037, 1, 1);
  CHECK(SaturatingTimeGm(&early2037, kLatest32, kEarliest32) == 2114380800);
  CHECK(2114380800 < kLatest32);
  // 1901-12-14 is representable in 32 bits but below the floor.
  tm nearFloor = Utc(1901, 12, 14);
  CHECK(SaturatingTimeGm(&nearFloor, kLatest32, kEarliest32) == kEarliest32);
}

TEST_CASE("PortableTimeGmSaturating agrees with PortableTimeGm for an ordinary date", "[TimeUtil]")
{
  tm a = Utc(2026, 10, 4);
  tm b = a;
  CHECK(PortableTimeGmSaturating(&a) == PortableTimeGm(&b));
}

TEST_CASE("The bounds leave room for the day and offset arithmetic callers do", "[TimeUtil]")
{
  // ShouldRenewRecurringRule()/HasRecurringRuleEndDatePassed() add a day and subtract an offset.
  static_assert(kLatestRepresentableTime <= std::numeric_limits<time_t>::max() - 366 * 86400);
  static_assert(kEarliestRepresentableTime >= std::numeric_limits<time_t>::lowest() + 366 * 86400);
  CHECK(kLatestRepresentableTime + 86400 + 24 * 3600 > kLatestRepresentableTime);
  CHECK(kEarliestRepresentableTime - 24 * 3600 < kEarliestRepresentableTime);
}

TEST_CASE("On a 64-bit time_t the difference of the two bounds fits in 64 bits", "[TimeUtil]")
{
  // The old floor, the smallest time_t plus a year, overflowed `now - floor` in 64-bit arithmetic.
  if (!kTimeTIs64Bit)
    SKIP("a 32-bit time_t's callers subtract in 64 bits instead");
  static_assert(!kTimeTIs64Bit || kLatestRepresentableTime <= std::numeric_limits<long long>::max() / 2);
  static_assert(!kTimeTIs64Bit || kEarliestRepresentableTime >= std::numeric_limits<long long>::lowest() / 2);
  CHECK(static_cast<long long>(kLatestRepresentableTime) - static_cast<long long>(kEarliestRepresentableTime) > 0);
}

TEST_CASE("LocalTimeZoned fills the broken-down local time and reports success", "[TimeUtil]")
{
  tm out{};
  CHECK(LocalTimeZoned(1767268800, &out));
  CHECK(out.tm_year == 126); // 2026
  // Whatever the zone, the broken-down time is within a day of the UTC date.
  CHECK((out.tm_mday >= 1 && out.tm_mday <= 2));
}

TEST_CASE("IsValidCivilDate knows month lengths and leap years", "[TimeUtil]")
{
  CHECK(IsValidCivilDate(2026, 1, 31));
  CHECK_FALSE(IsValidCivilDate(2026, 4, 31));
  CHECK_FALSE(IsValidCivilDate(2019, 11, 31));
  CHECK(IsValidCivilDate(2026, 11, 30));
  CHECK_FALSE(IsValidCivilDate(2026, 2, 29));
  CHECK(IsValidCivilDate(2028, 2, 29));       // divisible by 4
  CHECK_FALSE(IsValidCivilDate(2100, 2, 29)); // century, not divisible by 400
  CHECK(IsValidCivilDate(2000, 2, 29));       // divisible by 400
  CHECK_FALSE(IsValidCivilDate(2026, 2, 30));
  CHECK_FALSE(IsValidCivilDate(2026, 0, 1));
  CHECK_FALSE(IsValidCivilDate(2026, 13, 1));
  CHECK_FALSE(IsValidCivilDate(2026, 5, 0));
  CHECK_FALSE(IsValidCivilDate(2026, 5, -1));
}

TEST_CASE("IsValidCivilDate accepts the last day of December and refuses the day after it", "[TimeUtil]")
{
  // December is the one month whose next month is in the next year: its length must not borrow a day.
  CHECK(IsValidCivilDate(2026, 12, 31));
  CHECK_FALSE(IsValidCivilDate(2026, 12, 32));
  CHECK(IsValidCivilDate(2026, 1, 31));
  CHECK_FALSE(IsValidCivilDate(2026, 1, 32));
  CHECK_FALSE(IsValidCivilDate(2026, 4, 31));
  CHECK_FALSE(IsValidCivilDate(2026, 11, 31));
}
