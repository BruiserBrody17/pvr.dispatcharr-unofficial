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
// SaturatingTimeGm (the 2026-10-04 third hardening sweep): on a 32-bit time_t, timegm() returns -1
// for every date from 2038-01-19, which read as "no end date" for a rule ending in 2099.
// ---------------------------------------------------------------------

namespace
{
// timegm() as a 32-bit libc has it: -1 beyond INT32_MAX.
time_t Gm32(struct tm* t)
{
  const time_t r = timegm(t);
  return (r > 2147483647 || r < -2147483648LL) ? static_cast<time_t>(-1) : r;
}

// kLatestRepresentableTime as a 32-bit time_t has it: INT32_MAX less a year.
constexpr time_t kLatest32 = 2147483647LL - 366 * 86400;

tm Utc(int year, int month, int day)
{
  tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  return t;
}
} // namespace

TEST_CASE("SaturatingTimeGm saturates a date a 32-bit libc cannot represent instead of returning -1", "[TimeUtil]")
{
  tm farFuture = Utc(2099, 12, 31);
  CHECK(SaturatingTimeGm(&farFuture, Gm32, kLatest32) == kLatest32);
  tm justPast = Utc(2038, 1, 20);
  CHECK(SaturatingTimeGm(&justPast, Gm32, kLatest32) == kLatest32);
}

TEST_CASE("SaturatingTimeGm leaves every representable date, and the real -1 instant, alone", "[TimeUtil]")
{
  tm ordinary = Utc(2026, 10, 4);
  CHECK(SaturatingTimeGm(&ordinary, Gm32, kLatest32) == 1791072000);
  tm lastRepresentable = Utc(2038, 1, 19);
  CHECK(SaturatingTimeGm(&lastRepresentable, Gm32, kLatest32) == kLatest32); // representable, but above the ceiling
  tm epoch = Utc(1970, 1, 1);
  CHECK(SaturatingTimeGm(&epoch, Gm32, kLatest32) == 0);
  // 1969-12-31T23:59:59 genuinely is -1: not an overflow, so not saturated.
  tm minusOne = Utc(1969, 12, 31);
  minusOne.tm_hour = 23;
  minusOne.tm_min = 59;
  minusOne.tm_sec = 59;
  CHECK(SaturatingTimeGm(&minusOne, Gm32, kLatest32) == -1);
}

TEST_CASE("The saturated time leaves room for the day arithmetic callers do on an end date", "[TimeUtil]")
{
  // ShouldRenewRecurringRule()/HasRecurringRuleEndDatePassed() add a day and subtract an offset.
  static_assert(kLatestRepresentableTime <= std::numeric_limits<time_t>::max() - 366 * 86400);
  CHECK(kLatestRepresentableTime + 86400 + 24 * 3600 > kLatestRepresentableTime);
}

TEST_CASE("PortableTimeGmSaturating agrees with PortableTimeGm for an ordinary date", "[TimeUtil]")
{
  tm a = Utc(2026, 10, 4);
  tm b = a;
  CHECK(PortableTimeGmSaturating(&a) == PortableTimeGm(&b));
}

TEST_CASE("SaturatingTimeGm also clamps a representable date later than the ceiling", "[TimeUtil]")
{
  // 2037-06 is below INT32_MAX but above the ceiling: unclamped it sorted above the saturated 2099
  // value, and the callers' day arithmetic on it could overflow.
  tm june2037 = Utc(2037, 6, 1);
  const time_t clamped = SaturatingTimeGm(&june2037, Gm32, kLatest32);
  CHECK(clamped == kLatest32);
  tm far = Utc(2099, 12, 31);
  CHECK(SaturatingTimeGm(&far, Gm32, kLatest32) >= clamped);
  // ...while a date below the ceiling is untouched, to the second.
  tm early2037 = Utc(2037, 1, 1);
  CHECK(SaturatingTimeGm(&early2037, Gm32, kLatest32) == 2114380800);
  CHECK(2114380800 < kLatest32);
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
