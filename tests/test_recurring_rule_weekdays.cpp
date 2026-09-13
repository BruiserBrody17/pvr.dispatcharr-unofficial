#include "RecurringRuleWeekdays.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask maps Monday (0) to bit 0", "[RecurringRuleWeekdays]")
{
  // Confirmed against Kodi's real header: PVR_WEEKDAY_MONDAY = (1 << 0).
  CHECK(ComputeRecurringRuleWeekdaysBitmask({0}) == 0x01u);
}

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask maps Sunday (6) to bit 6", "[RecurringRuleWeekdays]")
{
  CHECK(ComputeRecurringRuleWeekdaysBitmask({6}) == 0x40u);
}

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask combines multiple days with no reordering", "[RecurringRuleWeekdays]")
{
  // Monday (bit 0) + Wednesday (bit 2) + Friday (bit 4).
  CHECK(ComputeRecurringRuleWeekdaysBitmask({0, 2, 4}) == 0x15u);
}

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask returns 0 (PVR_WEEKDAY_NONE) for an empty list",
          "[RecurringRuleWeekdays]")
{
  CHECK(ComputeRecurringRuleWeekdaysBitmask({}) == 0u);
}

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask silently skips an out-of-range day", "[RecurringRuleWeekdays]")
{
  CHECK(ComputeRecurringRuleWeekdaysBitmask({-1, 7, 100}) == 0u);
  CHECK(ComputeRecurringRuleWeekdaysBitmask({-1, 0, 7}) == 0x01u); // the valid 0 still counts
}

TEST_CASE("ComputeRecurringRuleWeekdaysBitmask sets all 7 bits for every day", "[RecurringRuleWeekdays]")
{
  CHECK(ComputeRecurringRuleWeekdaysBitmask({0, 1, 2, 3, 4, 5, 6}) == 0x7Fu);
}
