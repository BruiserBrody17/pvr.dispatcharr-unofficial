#include "RecurringRuleUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

// Test time_t values are chosen as raw seconds-since-epoch so they equal
// their own "seconds since UTC midnight" directly (epoch itself is a UTC
// midnight) -- e.g. 52200 means both "time_t 52200" and "14:30:00".

TEST_CASE("ComputeRecurringRuleFields computes start/end seconds and the selected day", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(52200 /* 14:30 */, 55800 /* 15:30 */, 0 /* no firstDay -> use nowUtc */,
                                       0b0000010u /* Monday only */, 0 /* offsetMinutes */, 100000 /* nowUtc */,
                                       daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == 52200);
  CHECK(endSeconds == 55800);
  CHECK(startDate == 86400); // utcMidnight(100000) -- the start of the *next* epoch day
  CHECK(daysOfWeek == std::vector<int>{1});
}

TEST_CASE("ComputeRecurringRuleFields collects every selected weekday, in order", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 1, 0b0010011u /* bits 0, 1, 4 */, 0, 0, daysOfWeek, startSeconds,
                                       endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(daysOfWeek == std::vector<int>{0, 1, 4});
}

TEST_CASE("ComputeRecurringRuleFields fails when no weekday is selected", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 1, 0 /* no bits set */, 0, 0, daysOfWeek, startSeconds, endSeconds,
                                       startDate, error);

  CHECK_FALSE(ok);
  CHECK(error == "At least one day of the week must be selected");
}

TEST_CASE("ComputeRecurringRuleFields applies the timezone offset to start/end seconds", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  // -300 minutes (US Eastern standard), matching TimeZoneUtil's own
  // convention for a negative (behind-UTC) offset.
  bool ok = ComputeRecurringRuleFields(52200 /* 14:30 UTC */, 55800 /* 15:30 UTC */, 1, 0b1u, -300, 0, daysOfWeek,
                                       startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == 34200); // 52200 - 18000
  CHECK(endSeconds == 37800);   // 55800 - 18000
}

TEST_CASE("ComputeRecurringRuleFields leaves an out-of-range result unwrapped", "[RecurringRuleUtil]")
{
  // Deliberately not wrapped into [0, 86400) here -- TimeOfDayString()'s
  // own comment documents that callers may receive a negative or >24h
  // value and are expected to wrap it themselves when formatting.
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(3600 /* 01:00 UTC */, 3600, 1, 0b1u, -300, 0, daysOfWeek, startSeconds,
                                       endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startSeconds == -14400); // negative, not wrapped
}

TEST_CASE("ComputeRecurringRuleFields uses a real firstDay when provided, not nowUtc", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, 52200 /* mid-day firstDay */, 0b1u, 0, 999999999 /* should be ignored */,
                                       daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startDate == 0); // utcMidnight(52200) -- floors back to epoch start
}

TEST_CASE("ComputeRecurringRuleFields falls back to nowUtc for a negative firstDay too", "[RecurringRuleUtil]")
{
  std::vector<int> daysOfWeek;
  int startSeconds = 0, endSeconds = 0;
  time_t startDate = 0;
  std::string error;

  bool ok = ComputeRecurringRuleFields(0, 0, -1 /* negative, not just zero */, 0b1u, 0, 172800 /* nowUtc, day 2 */,
                                       daysOfWeek, startSeconds, endSeconds, startDate, error);

  REQUIRE(ok);
  CHECK(startDate == 172800); // utcMidnight(172800) == 172800, already a midnight
}
