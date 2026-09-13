#include "RecurringRuleRenewal.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
constexpr time_t kNow = 1767225600; // 2026-01-01T00:00:00Z
constexpr int kWindowDays = 30;
constexpr int kSafetyMarginSeconds = 3600;

RecurringRule MakeRule(int id, time_t endDate, bool enabled = true)
{
  RecurringRule rule;
  rule.id = id;
  rule.endDate = endDate;
  rule.enabled = enabled;
  return rule;
}
} // namespace

TEST_CASE("ShouldRenewRecurringRule is false for a disabled rule", "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow - 86400, /*enabled=*/false); // already past end_date too

  CHECK_FALSE(ShouldRenewRecurringRule(rule, {}, /*haveRecordings=*/true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is false while comfortably inside the window", "[RecurringRuleRenewal]")
{
  // More than half of kWindowDays (15 days) remaining.
  RecurringRule rule = MakeRule(1, kNow + 20 * 86400);

  CHECK_FALSE(ShouldRenewRecurringRule(rule, {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is true once less than half the window remains, with no occurrences",
          "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 10 * 86400); // 10 days left, less than 15

  CHECK(ShouldRenewRecurringRule(rule, {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is false when GetRecordings() itself failed -- err toward skipping",
          "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);

  CHECK_FALSE(ShouldRenewRecurringRule(rule, {}, /*haveRecordings=*/false, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is false when the rule has an in-progress occurrence", "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 1;
  rec.isInProgress = true;

  CHECK_FALSE(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is false when the rule has an occurrence starting within the safety margin",
          "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 1;
  rec.isUpcoming = true;
  rec.startTime = kNow + 1800; // 30 minutes out, inside the 1-hour safety margin

  CHECK_FALSE(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule is true when the rule's next occurrence is beyond the safety margin",
          "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 1;
  rec.isUpcoming = true;
  rec.startTime = kNow + 7200; // 2 hours out, beyond the 1-hour safety margin

  CHECK(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule ignores an occurrence belonging to a different rule", "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 99; // a different rule
  rec.isInProgress = true;

  CHECK(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule ignores a completed (neither in-progress nor upcoming) occurrence",
          "[RecurringRuleRenewal]")
{
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 1;
  rec.isInProgress = false;
  rec.isUpcoming = false;

  CHECK(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}
