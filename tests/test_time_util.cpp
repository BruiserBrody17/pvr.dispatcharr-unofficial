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
  setenv("TZ", "America/New_York", 1);
  tzset();

  tm tmValAgain = tmVal;
  time_t resultWithTz = PortableTimeGm(&tmValAgain);

  if (originalTz)
    setenv("TZ", savedTz.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();

  REQUIRE(resultWithTz == utcResult);
}
#endif
