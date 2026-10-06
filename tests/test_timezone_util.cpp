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

TEST_CASE("The names Chromium's timezone picker offers resolve to the table's zones", "[TimeZoneUtil]")
{
  // Dispatcharr's settings page stores what the browser's
  // Intl.supportedValuesOf('timeZone') returns, and V8 lists these rather than
  // the modern names.
  CHECK(CanonicalKnownZoneName("Asia/Calcutta") == "Asia/Kolkata");
  CHECK(CanonicalKnownZoneName("Europe/Kiev") == "Europe/Kyiv");
  CHECK(CanonicalKnownZoneName("America/Indianapolis") == "America/Indiana/Indianapolis");

  int offset = 0;
  REQUIRE(ComputeKnownZoneOffsetMinutes("Asia/Calcutta", 1767268800, offset));
  CHECK(offset == 330);
  REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/Kiev", 1782907200 /* midsummer */, offset));
  CHECK(offset == 180);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Indianapolis", 1782907200, offset));
  CHECK(offset == -240);
}

TEST_CASE("Older US and Canada ids resolve and follow daylight saving like the zone they alias", "[TimeZoneUtil]")
{
  int winter = 0, summer = 0;
  REQUIRE(ComputeKnownZoneOffsetMinutes("US/Mountain", 1767268800, winter));
  REQUIRE(ComputeKnownZoneOffsetMinutes("US/Mountain", 1782907200, summer));
  CHECK(winter == -420);
  CHECK(summer == -360);
  REQUIRE(ComputeKnownZoneOffsetMinutes("Canada/Pacific", 1782907200, summer));
  CHECK(summer == -420);
}

TEST_CASE("CanonicalKnownZoneName leaves a modern name, an unknown name and an empty name alone", "[TimeZoneUtil]")
{
  CHECK(CanonicalKnownZoneName("America/Chicago") == "America/Chicago");
  CHECK(CanonicalKnownZoneName("Asia/Kolkata") == "Asia/Kolkata");
  CHECK(CanonicalKnownZoneName("Mars/Olympus_Mons") == "Mars/Olympus_Mons");
  CHECK(CanonicalKnownZoneName("").empty());
}

TEST_CASE("Every alias resolves to a zone the table itself knows", "[TimeZoneUtil]")
{
  // Guards a typo in an alias's target: it would otherwise silently leave that
  // alias unrecognized, exactly the failure this table exists to prevent.
  const char* aliases[] = {"US/Eastern",
                           "US/Mountain",
                           "Navajo",
                           "US/Pacific",
                           "US/Alaska",
                           "US/Arizona",
                           "US/Hawaii",
                           "US/Michigan",
                           "US/East-Indiana",
                           "Canada/Eastern",
                           "Canada/Central",
                           "Canada/Mountain",
                           "Canada/Atlantic",
                           "Canada/Saskatchewan",
                           "Mexico/General",
                           "GB",
                           "GB-Eire",
                           "Eire",
                           "Portugal",
                           "Poland",
                           "Japan",
                           "PRC",
                           "Hongkong",
                           "Singapore",
                           "Zulu",
                           "UCT",
                           "Universal",
                           "Europe/Belfast",
                           "Europe/Vatican",
                           "Europe/Bratislava",
                           "Asia/Chungking",
                           "Etc/Zulu"};
  int offset = 0;
  for (const char* alias : aliases)
    CHECK(ComputeKnownZoneOffsetMinutes(alias, 1767268800, offset));
}

TEST_CASE("A zero UTC-offset EU zone (Europe/London) still shifts across DST", "[TimeZoneUtil]")
{
  int offset = 0;

  REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/London", 1767268800 /* midwinter */, offset));
  CHECK(offset == 0);

  REQUIRE(ComputeKnownZoneOffsetMinutes("Europe/London", 1782907200 /* midsummer */, offset));
  CHECK(offset == 60);
}

// ---------------------------------------------------------------------
// DesiredRecurringRuleTimezoneSetting
// ---------------------------------------------------------------------

TEST_CASE("DesiredRecurringRuleTimezoneSetting selects a known zone by its canonical name", "[TimeZoneUtil]")
{
  bool known = false;
  CHECK(DesiredRecurringRuleTimezoneSetting("America/Chicago", 1767268800, &known) == "America/Chicago");
  CHECK(known);
  // An alias Chromium's timezone list offers -- the live-confirmed gap.
  CHECK(DesiredRecurringRuleTimezoneSetting("Asia/Calcutta", 1767268800, &known) == "Asia/Kolkata");
  CHECK(known);
  CHECK(DesiredRecurringRuleTimezoneSetting("US/Mountain", 1767268800) == "America/Denver");
}

TEST_CASE("DesiredRecurringRuleTimezoneSetting falls back to manual for a zone without DST rules here",
          "[TimeZoneUtil]")
{
  bool known = true;
  CHECK(DesiredRecurringRuleTimezoneSetting("Antarctica/Troll", 1767268800, &known) == "manual");
  CHECK_FALSE(known);
  CHECK(DesiredRecurringRuleTimezoneSetting("", 1767268800, &known) == "manual");
  CHECK_FALSE(known);
  CHECK(DesiredRecurringRuleTimezoneSetting("not a zone", 1767268800) == "manual");
}

TEST_CASE("Every table zone has the right January and July offset", "[TimeZoneUtil]")
{
  struct ZoneExpectation
  {
    const char* name;
    int january;
    int july;
  };
  // Mid-month instants well clear of any transition (2026-01-15 and 2026-07-15 12:00 UTC).
  const time_t kJanuary = 1768478400;
  const time_t kJuly = 1784116800;
  const ZoneExpectation zones[] = {
      {"America/New_York", -300, -240},  {"America/Chicago", -360, -300},
      {"America/Denver", -420, -360},    {"America/Los_Angeles", -480, -420},
      {"America/Anchorage", -540, -480}, {"America/Phoenix", -420, -420},
      {"Pacific/Honolulu", -600, -600},  {"America/Detroit", -300, -240},
      {"America/Boise", -420, -360},     {"America/Toronto", -300, -240},
      {"America/Winnipeg", -360, -300},  {"America/Edmonton", -420, -360},
      {"America/Vancouver", -480, -420}, {"America/Halifax", -240, -180},
      {"America/Regina", -360, -360},    {"America/Mexico_City", -360, -360},
      {"Europe/London", 0, 60},          {"Europe/Dublin", 0, 60},
      {"Europe/Lisbon", 0, 60},          {"Europe/Paris", 60, 120},
      {"Europe/Berlin", 60, 120},        {"Europe/Madrid", 60, 120},
      {"Europe/Warsaw", 60, 120},        {"Europe/Helsinki", 120, 180},
      {"Europe/Athens", 120, 180},       {"Europe/Kyiv", 120, 180},
      {"Asia/Tokyo", 540, 540},          {"Asia/Kolkata", 330, 330},
      {"Asia/Dubai", 240, 240},          {"Africa/Johannesburg", 120, 120},
  };
  for (const auto& zone : zones)
  {
    int january = 0;
    int july = 0;
    INFO(zone.name);
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone.name, kJanuary, january));
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone.name, kJuly, july));
    CHECK(january == zone.january);
    CHECK(july == zone.july);
  }
}

TEST_CASE("Vancouver and Edmonton stay on their summer offset from 2026-11-01 (tzdata 2026c)", "[TimeZoneUtil]")
{
  // tzdata 2026c: no fall-back on 2026-11-01 for British Columbia or Alberta. Before it, DST works as usual.
  const time_t kJuly2026 = 1784116800;       // 2026-07-15T12:00Z
  const time_t kOct31 = 1793448000;          // 2026-10-31T12:00Z, still daylight time
  const time_t kNov2 = 1793620800;           // 2026-11-02T12:00Z, would have been standard time
  const time_t kJan2027 = 1800014400;        // 2027-01-15T12:00Z
  const time_t kJul2027 = 1815652800;        // 2027-07-15T12:00Z
  const time_t kJan2026 = 1768478400;        // 2026-01-15T12:00Z, before the change: standard time
  const time_t kLastBefore = 1793491200 - 1; // one second before the switch: daylight, so continuous
  const time_t kSwitch = 1793491200;

  struct Expected
  {
    const char* zone;
    int summer;
    int formerWinter;
  };
  for (const Expected& e : {Expected{"America/Vancouver", -420, -480}, Expected{"America/Edmonton", -360, -420},
                            Expected{"Canada/Pacific", -420, -480}, Expected{"Canada/Mountain", -360, -420},
                            Expected{"America/Yellowknife", -360, -420}})
  {
    INFO(e.zone);
    int offset = 0;
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kJan2026, offset));
    CHECK(offset == e.formerWinter); // the winter before the change is still standard time
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kJuly2026, offset));
    CHECK(offset == e.summer);
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kOct31, offset));
    CHECK(offset == e.summer);
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kLastBefore, offset));
    CHECK(offset == e.summer);
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kSwitch, offset));
    CHECK(offset == e.summer); // continuous across the switch
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kNov2, offset));
    CHECK(offset == e.summer); // the fall-back no longer happens
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kJan2027, offset));
    CHECK(offset == e.summer);
    REQUIRE(ComputeKnownZoneOffsetMinutes(e.zone, kJul2027, offset));
    CHECK(offset == e.summer);
  }
}

TEST_CASE("Other Canadian and US zones still fall back on 2026-11-01", "[TimeZoneUtil]")
{
  const time_t kNov2 = 1793620800; // 2026-11-02T12:00Z
  int offset = 0;
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Toronto", kNov2, offset));
  CHECK(offset == -300);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Winnipeg", kNov2, offset));
  CHECK(offset == -360);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Halifax", kNov2, offset));
  CHECK(offset == -240);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Denver", kNov2, offset));
  CHECK(offset == -420);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Los_Angeles", kNov2, offset));
  CHECK(offset == -480);
}

TEST_CASE("The US/Canada and EU transitions land on the right instant in years whose March and November start on "
          "other weekdays",
          "[TimeZoneUtil]")
{
  // Every 2026 vector above has March 1 and November 1 on a Sunday, which is the one calendar where a wrong "nth
  // Sunday" formula (a missing wrap of the weekday difference) still lands on the right day. 2027 starts both months
  // on a Monday and 2028 on a Wednesday. Instants computed independently of this code.
  struct Case
  {
    const char* zone;
    time_t springUtc; // the first instant on the daylight offset
    time_t fallUtc;   // the first instant back on standard time
    int standard;
    int daylight;
  };
  for (const Case& c : {Case{"America/New_York", 1805007600, 1825567200, -300, -240}, // 2027-03-14, 2027-11-07
                        Case{"America/New_York", 1836457200, 1857016800, -300, -240}, // 2028-03-12, 2028-11-05
                        Case{"Europe/Paris", 1806195600, 1824944400, 60, 120},        // 2027-03-28, 2027-10-31
                        Case{"Europe/Paris", 1837645200, 1856394000, 60, 120}})       // 2028-03-26, 2028-10-29
  {
    INFO(c.zone << " spring " << c.springUtc);
    int offset = 0;
    REQUIRE(ComputeKnownZoneOffsetMinutes(c.zone, c.springUtc - 1, offset));
    CHECK(offset == c.standard);
    REQUIRE(ComputeKnownZoneOffsetMinutes(c.zone, c.springUtc, offset));
    CHECK(offset == c.daylight);
    REQUIRE(ComputeKnownZoneOffsetMinutes(c.zone, c.fallUtc - 1, offset));
    CHECK(offset == c.daylight);
    REQUIRE(ComputeKnownZoneOffsetMinutes(c.zone, c.fallUtc, offset));
    CHECK(offset == c.standard);
  }
}

TEST_CASE("Leaving out a zone's recent rule change gives what an older server still applies", "[TimeZoneUtil]")
{
  const time_t kJan2027 = 1800014400;          // 2027-01-15T12:00Z
  const time_t kJul2027 = 1815652800;          // 2027-07-15T12:00Z
  const time_t kBeforeSpring2027 = 1805018399; // 2027-03-14T09:59:59Z: 02:00 local standard time is 10:00Z
  const time_t kSpring2027 = 1805018400;
  const time_t kJul2026 = 1784116800;
  int offset = 0;
  for (const char* zone : {"America/Vancouver", "Canada/Pacific"})
  {
    INFO(zone);
    // Current rules: daylight time throughout.
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kJan2027, offset));
    CHECK(offset == -420);
    // Older tz data: back on standard time for the winter, and into daylight time on the second Sunday of March.
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kJan2027, offset, /*applyZoneRuleChanges=*/false));
    CHECK(offset == -480);
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kBeforeSpring2027, offset, false));
    CHECK(offset == -480);
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kSpring2027, offset, false));
    CHECK(offset == -420);
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kJul2027, offset, false));
    CHECK(offset == -420);
    // Before the change the two agree.
    REQUIRE(ComputeKnownZoneOffsetMinutes(zone, kJul2026, offset, false));
    CHECK(offset == -420);
  }
  // A zone with no recent change is unaffected.
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/New_York", kJan2027, offset, false));
  CHECK(offset == -300);
  REQUIRE(ComputeKnownZoneOffsetMinutes("America/Phoenix", kJan2027, offset, false));
  CHECK(offset == -420);
}
