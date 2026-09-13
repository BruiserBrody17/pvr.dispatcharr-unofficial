#include "DateTimeFormat.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("IsoFromTime formats a known epoch", "[DateTimeFormat]")
{
  CHECK(IsoFromTime(1767225600) == "2026-01-01T00:00:00Z");
  CHECK(IsoFromTime(1781526645) == "2026-06-15T12:30:45Z");
}

TEST_CASE("TimeFromIso parses Dispatcharr's Z-suffixed format", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T00:00:00Z") == 1767225600);
  CHECK(TimeFromIso("2026-06-15T12:30:45Z") == 1781526645);
}

TEST_CASE("TimeFromIso ignores a trailing offset/fractional part", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T00:00:00+00:00") == 1767225600);
  CHECK(TimeFromIso("2026-01-01T00:00:00.123456Z") == 1767225600);
}

TEST_CASE("TimeFromIso returns 0 for unparseable input", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("") == 0);
  CHECK(TimeFromIso("not-a-date") == 0);
  CHECK(TimeFromIso("2026-01-01") == 0); // too short, no time component
}

TEST_CASE("IsoFromTime/TimeFromIso round-trip", "[DateTimeFormat]")
{
  CHECK(TimeFromIso(IsoFromTime(1781526645)) == 1781526645);
}

TEST_CASE("TimeOfDayString formats seconds-since-midnight", "[DateTimeFormat]")
{
  CHECK(TimeOfDayString(0) == "00:00:00");
  CHECK(TimeOfDayString(3661) == "01:01:01");
  CHECK(TimeOfDayString(86399) == "23:59:59");
}

TEST_CASE("TimeOfDayString wraps a negative value into [0, 86400)", "[DateTimeFormat]")
{
  // A UTC time-of-day shifted by recurring_rule_utc_offset_minutes can go
  // negative or past 24h before this is called.
  CHECK(TimeOfDayString(-3600) == "23:00:00");
  CHECK(TimeOfDayString(86400 + 3600) == "01:00:00");
}

TEST_CASE("SecondsSinceMidnightFromString parses HH:MM:SS", "[DateTimeFormat]")
{
  CHECK(SecondsSinceMidnightFromString("01:01:01") == 3661);
  CHECK(SecondsSinceMidnightFromString("00:00:00") == 0);
  CHECK(SecondsSinceMidnightFromString("23:59:59") == 86399);
}

TEST_CASE("SecondsSinceMidnightFromString accepts HH:MM with no seconds", "[DateTimeFormat]")
{
  CHECK(SecondsSinceMidnightFromString("01:30") == 5400);
}

TEST_CASE("SecondsSinceMidnightFromString returns 0 for unparseable input", "[DateTimeFormat]")
{
  CHECK(SecondsSinceMidnightFromString("") == 0);
  CHECK(SecondsSinceMidnightFromString("not-a-time") == 0);
}

TEST_CASE("TimeOfDayString/SecondsSinceMidnightFromString round-trip", "[DateTimeFormat]")
{
  CHECK(SecondsSinceMidnightFromString(TimeOfDayString(3661)) == 3661);
}

TEST_CASE("DateStringFromTime formats the UTC calendar date", "[DateTimeFormat]")
{
  CHECK(DateStringFromTime(1767225600) == "2026-01-01");
  CHECK(DateStringFromTime(1781526645) == "2026-06-15");
}

TEST_CASE("TimeFromDateString parses YYYY-MM-DD as UTC midnight", "[DateTimeFormat]")
{
  CHECK(TimeFromDateString("2026-01-01") == 1767225600);
}

TEST_CASE("TimeFromDateString returns 0 for unparseable input", "[DateTimeFormat]")
{
  CHECK(TimeFromDateString("") == 0);
  CHECK(TimeFromDateString("short") == 0);
}

TEST_CASE("DateStringFromTime/TimeFromDateString round-trip", "[DateTimeFormat]")
{
  CHECK(TimeFromDateString(DateStringFromTime(1767225600)) == 1767225600);
}
