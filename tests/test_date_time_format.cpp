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

TEST_CASE("TimeFromIso ignores a trailing zero offset/fractional part", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T00:00:00+00:00") == 1767225600);
  CHECK(TimeFromIso("2026-01-01T00:00:00.123456Z") == 1767225600);
}

TEST_CASE("TimeFromIso applies a genuine non-zero trailing offset -- the real bug this fixes", "[DateTimeFormat]")
{
  // custom_properties.program.start_time for a recurring-rule occurrence
  // is a real, confirmed exception to every other UTC-normalized
  // timestamp in this API (a 35th-pass audit, not itself independently
  // reproduced): Dispatcharr's own recurring-rule scheduler writes it
  // via a timezone-aware Python datetime's own isoformat() call in the
  // system's own configured timezone, not UTC. "2026-01-01T00:00:00-04:00"
  // is 04:00 UTC -- ignoring the offset used to silently return
  // midnight UTC instead, off by the whole zone offset.
  CHECK(TimeFromIso("2026-01-01T00:00:00-04:00") == TimeFromIso("2026-01-01T04:00:00Z"));
  CHECK(TimeFromIso("2026-01-01T20:00:00-04:00") == TimeFromIso("2026-01-02T00:00:00Z"));
}

TEST_CASE("TimeFromIso applies a positive trailing offset", "[DateTimeFormat]")
{
  // "+02:00" is 2 hours ahead of UTC, so the UTC instant is 2 hours
  // *earlier* than the local wall-clock value shown.
  CHECK(TimeFromIso("2026-01-01T02:00:00+02:00") == TimeFromIso("2026-01-01T00:00:00Z"));
}

TEST_CASE("TimeFromIso applies a non-zero offset with a non-zero minutes component", "[DateTimeFormat]")
{
  // India Standard Time, a real, commonly-configured non-whole-hour
  // offset.
  CHECK(TimeFromIso("2026-01-01T05:30:00+05:30") == TimeFromIso("2026-01-01T00:00:00Z"));
}

TEST_CASE("TimeFromIso applies a non-zero offset with no colon (+HHMM form)", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T00:00:00-0400") == TimeFromIso("2026-01-01T04:00:00Z"));
}

TEST_CASE("TimeFromIso applies a non-zero offset even with fractional seconds present", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T00:00:00.123456-04:00") == TimeFromIso("2026-01-01T04:00:00Z"));
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

TEST_CASE("SecondsSinceMidnightFromString keeps the old sscanf-based tolerances", "[DateTimeFormat]")
{
  // Dispatcharr's own TimeField can serialize fractional seconds.
  CHECK(SecondsSinceMidnightFromString("12:30:15.250000") == 45015);
  CHECK(SecondsSinceMidnightFromString(" 01:02:03") == 3723);
  CHECK(SecondsSinceMidnightFromString("01:02:xx") == 3720);
  CHECK(SecondsSinceMidnightFromString("7:5:9") == 7 * 3600 + 5 * 60 + 9);
  CHECK(SecondsSinceMidnightFromString("12") == 0); // a lone hour field was never enough
}

TEST_CASE("SecondsSinceMidnightFromString rejects out-of-range fields without overflowing", "[DateTimeFormat]")
{
  // Each of these used to reach signed-integer overflow (or sscanf's own
  // out-of-range %d undefined behavior) in the old implementation --
  // confirmed under -fsanitize=undefined. Now rejected as unparseable.
  CHECK(SecondsSinceMidnightFromString("600000:00:00") == 0);
  CHECK(SecondsSinceMidnightFromString("00:40000000:00") == 0);
  CHECK(SecondsSinceMidnightFromString("99999999999:00:00") == 0);
  CHECK(SecondsSinceMidnightFromString("00:00:99999999999") == 0);
  CHECK(SecondsSinceMidnightFromString("24:00:00") == 0);
  CHECK(SecondsSinceMidnightFromString("23:60:00") == 0);
  CHECK(SecondsSinceMidnightFromString("23:59:60") == 0);
  CHECK(SecondsSinceMidnightFromString("-1:00:00") == 0);
}

TEST_CASE("TimeOfDayString/SecondsSinceMidnightFromString round-trip", "[DateTimeFormat]")
{
  CHECK(SecondsSinceMidnightFromString(TimeOfDayString(3661)) == 3661);
  // Every real time of day survives the round trip under the bounded,
  // hand-rolled parser, not just one sample value.
  bool allRoundTrip = true;
  for (int s = 0; s < 86400; ++s)
  {
    if (SecondsSinceMidnightFromString(TimeOfDayString(s)) != s)
    {
      allRoundTrip = false;
      break;
    }
  }
  CHECK(allRoundTrip);
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

TEST_CASE("TimeFromIso leaves a malformed or truncated trailing offset as UTC", "[DateTimeFormat]")
{
  // Found worth pinning by the 2026-10-04 hardening sweep: only a complete, in-range
  // "+HH:MM" / "+HHMM" is applied; anything else is read as UTC, as documented.
  const time_t utc = TimeFromIso("2026-01-01T12:00:00Z");
  CHECK(TimeFromIso("2026-01-01T12:00:00+5:30") == utc);  // hours not two digits
  CHECK(TimeFromIso("2026-01-01T12:00:00+05:3") == utc);  // minutes cut off
  CHECK(TimeFromIso("2026-01-01T12:00:00+05") == utc);    // no minutes at all
  CHECK(TimeFromIso("2026-01-01T12:00:00.") == utc);      // a fractional part with no digits
  CHECK(TimeFromIso("2026-01-01T12:00:00+99:99") == utc); // out of range, not a four-day shift
  CHECK(TimeFromIso("2026-01-01T12:00:00-24:00") == utc);
  CHECK(TimeFromIso("2026-01-01T12:00:00+05:60") == utc);
}

TEST_CASE("TimeFromIso applies a one-digit fractional second before an offset", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-01-01T12:00:00.5-04:00") == TimeFromIso("2026-01-01T16:00:00Z"));
  // Seconds beyond the offset's minutes are ignored, not misread.
  CHECK(TimeFromIso("2026-01-01T12:00:00-04:00:30") == TimeFromIso("2026-01-01T16:00:00Z"));
  // The extreme legitimate offsets still work.
  CHECK(TimeFromIso("2026-01-01T12:00:00+14:00") == TimeFromIso("2025-12-31T22:00:00Z"));
  CHECK(TimeFromIso("2026-01-01T12:00:00-12:00") == TimeFromIso("2026-01-02T00:00:00Z"));
}

TEST_CASE("TimeFromIso rejects a date or time with a field outside its range", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-13-01T12:00:00Z") == 0);
  CHECK(TimeFromIso("2026-00-10T12:00:00Z") == 0);
  CHECK(TimeFromIso("2026-01-00T12:00:00Z") == 0);
  CHECK(TimeFromIso("2026-01-32T12:00:00Z") == 0);
  CHECK(TimeFromIso("2026-01-01T24:00:00Z") == 0);
  CHECK(TimeFromIso("2026-01-01T12:60:00Z") == 0);
  CHECK(TimeFromIso("2026-01-01T12:00:61Z") == 0);
  CHECK(TimeFromIso("2026-13-45T25:61:61Z") == 0);
  // A leap second is a real value and is accepted.
  CHECK(TimeFromIso("2026-06-30T23:59:60Z") != 0);
}

TEST_CASE("A far-future date parses to a real far-future time on a 64-bit time_t", "[DateTimeFormat]")
{
  // A 32-bit build saturates instead (SaturatingTimeGm); either way never -1 or 0, so a rule ending
  // in 2099 is never read as having no end date.
  CHECK(TimeFromIso("2099-12-31T00:00:00Z") > 2000000000LL);
  CHECK(TimeFromDateString("2099-12-31") > 2000000000LL);
  if (sizeof(time_t) > 4)
  {
    CHECK(TimeFromIso("2099-12-31T00:00:00Z") == 4102358400LL);
    CHECK(TimeFromDateString("2099-12-31") == 4102358400LL);
  }
}

TEST_CASE("An impossible calendar date is rejected, not rolled over into the next month", "[DateTimeFormat]")
{
  // timegm() turns Nov 31 into Dec 1: a wrong-but-plausible time that fed scheduling arithmetic.
  CHECK(TimeFromIso("2019-11-31T04:01:54+00:00") == 0);
  CHECK(TimeFromIso("2001-06-31T00:00:00Z") == 0);
  CHECK(TimeFromIso("2026-02-30T00:00:00Z") == 0);
  CHECK(TimeFromIso("2026-02-29T00:00:00Z") == 0);
  CHECK(TimeFromIso("2028-02-29T00:00:00Z") != 0); // a real leap day
  CHECK(TimeFromIso("2026-04-30T23:59:59Z") != 0);
  CHECK(TimeFromDateString("2001-06-31") == 0);
  CHECK(TimeFromDateString("2026-02-29") == 0);
  CHECK(TimeFromDateString("2028-02-29") != 0);
  CHECK(TimeFromDateString("2026-04-30") != 0);
}

TEST_CASE("TimeFromIso rejects a negative hour, minute or second", "[DateTimeFormat]")
{
  CHECK(TimeFromIso("2026-10-04T-1:00:00Z") == 0);
  CHECK(TimeFromIso("2026-10-04T01:-1:00Z") == 0);
  CHECK(TimeFromIso("2026-10-04T01:00:-1Z") == 0);
}

TEST_CASE("SecondsSinceMidnightFromString ignores everything after the third field", "[DateTimeFormat]")
{
  // A fourth field must not be read into a table of three bounds.
  CHECK(SecondsSinceMidnightFromString("01:02:03:04") == 3723);
  CHECK(SecondsSinceMidnightFromString("01:02:03") == 3723);
}
