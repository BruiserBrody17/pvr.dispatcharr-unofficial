#include "TimeZoneUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

// 2026 real DST transition instants, computed independently of this code
// (via `date -u -d ...`), not derived from it:
//   US/Canada: 2nd Sunday of March (2026-03-08) 2:00am LOCAL STANDARD time;
//              1st Sunday of November (2026-11-01) 2:00am LOCAL DAYLIGHT time.
//   UK/EU:     last Sunday of March (2026-03-29) 01:00 UTC;
//              last Sunday of October (2026-10-25) 01:00 UTC.

TEST_CASE("Returns false for an unrecognized IANA zone", "[TimeZoneUtil]")
{
  int offset = 12345;
  REQUIRE_FALSE(ComputeKnownZoneOffsetMinutes("Not/AZone", 1767268800, offset));
  CHECK(offset == 12345); // untouched
}

TEST_CASE("Fixed-offset zones ignore the DST calendar entirely", "[TimeZoneUtil]")
{
  int offset = 0;

  SECTION("Arizona has no DST despite matching the US/Canada region")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/Phoenix", 1782907200 /* mid-July */, offset));
    CHECK(offset == -420);
  }

  SECTION("UTC is always zero")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("UTC", 1782907200, offset));
    CHECK(offset == 0);
  }

  SECTION("Fixed-offset zone stays constant across a DST boundary that would matter for a DST zone")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Asia/Tokyo", 1772953199 /* right at a US spring transition */, offset));
    CHECK(offset == 540);
    REQUIRE(ComputeKnownZoneOffsetMinutes("Asia/Tokyo", 1772953200, offset));
    CHECK(offset == 540);
  }
}

TEST_CASE("US/Canada zone (America/New_York) transitions at the correct instants", "[TimeZoneUtil]")
{
  int offset = 0;

  SECTION("just before spring-forward: still standard time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1772953199, offset));
    CHECK(offset == -300);
  }

  SECTION("exactly at spring-forward: daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1772953200, offset));
    CHECK(offset == -240);
  }

  SECTION("midsummer: daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1782907200, offset));
    CHECK(offset == -240);
  }

  SECTION("just before fall-back: still daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1793512799, offset));
    CHECK(offset == -240);
  }

  SECTION("exactly at fall-back: standard time again")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1793512800, offset));
    CHECK(offset == -300);
  }

  SECTION("midwinter: standard time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", 1767268800, offset));
    CHECK(offset == -300);
  }
}

TEST_CASE("EU zone (Europe/Paris) transitions at the correct instants", "[TimeZoneUtil]")
{
  int offset = 0;

  SECTION("just before spring-forward: still standard time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1774745999, offset));
    CHECK(offset == 60);
  }

  SECTION("exactly at spring-forward: daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1774746000, offset));
    CHECK(offset == 120);
  }

  SECTION("midsummer: daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1782907200, offset));
    CHECK(offset == 120);
  }

  SECTION("just before fall-back: still daylight time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1792889999, offset));
    CHECK(offset == 120);
  }

  SECTION("exactly at fall-back: standard time again")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1792890000, offset));
    CHECK(offset == 60);
  }

  SECTION("midwinter: standard time")
  {
    REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Paris", 1767268800, offset));
    CHECK(offset == 60);
  }
}

TEST_CASE("A zero UTC-offset EU zone (Europe/London) still shifts across DST", "[TimeZoneUtil]")
{
  int offset = 0;

  REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/London", 1767268800 /* midwinter */, offset));
  CHECK(offset == 0);

  REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/London", 1782907200 /* midsummer */, offset));
  CHECK(offset == 60);
}
