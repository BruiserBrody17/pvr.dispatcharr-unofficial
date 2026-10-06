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
  // Owned by the addon -- only such a rule is ever renewed (ManagedRecurringRule.h).
  rule.name = "Test Show [Kodi]";
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

TEST_CASE("ShouldRenewRecurringRule never renews a rule the addon does not own, expired or not",
          "[RecurringRuleRenewal]")
{
  // A rule made in Dispatcharr's web UI with a deliberate end date: it used to be
  // extended again and again.
  for (time_t endDate : {kNow - 86400, kNow, kNow + 3 * 86400, kNow + 20 * 86400})
  {
    RecurringRule rule = MakeRule(1, endDate);
    rule.name = "Evening News";
    CHECK_FALSE(ShouldRenewRecurringRule(rule, {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
    rule.name = "";
    CHECK_FALSE(ShouldRenewRecurringRule(rule, {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  }
}

TEST_CASE("ShouldRenewRecurringRule revives an owned, enabled rule that has run past its end_date -- no Kodi ran "
          "for longer than the window",
          "[RecurringRuleRenewal]")
{
  CHECK(ShouldRenewRecurringRule(MakeRule(1, kNow - 86400), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  CHECK(ShouldRenewRecurringRule(MakeRule(1, kNow - 400 * 86400), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  // End date exactly now counts as expired.
  CHECK(ShouldRenewRecurringRule(MakeRule(1, kNow), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule gives an owned rule with no end_date at all one, since it cannot be saved "
          "without",
          "[RecurringRuleRenewal]")
{
  CHECK(ShouldRenewRecurringRule(MakeRule(1, /*endDate=*/0), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule still leaves a disabled owned rule alone, however long expired",
          "[RecurringRuleRenewal]")
{
  CHECK_FALSE(ShouldRenewRecurringRule(MakeRule(1, kNow - 86400, /*enabled=*/false), {}, true, kNow, kWindowDays,
                                       kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule does not revive an expired owned rule while one of its occurrences is recording "
          "or about to start, or when the recording list is unknown",
          "[RecurringRuleRenewal]")
{
  Recording running;
  running.recurringRuleId = 1;
  running.isInProgress = true;
  CHECK_FALSE(
      ShouldRenewRecurringRule(MakeRule(1, kNow - 86400), {running}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  CHECK_FALSE(ShouldRenewRecurringRule(MakeRule(1, kNow - 86400), {}, /*haveRecordings=*/false, kNow, kWindowDays,
                                       kSafetyMarginSeconds));
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
  rec.endTime = kNow + 5400;

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
  rec.endTime = kNow + 10800;

  CHECK(ShouldRenewRecurringRule(rule, {rec}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("ShouldRenewRecurringRule ignores a missed occurrence stuck at status==scheduled with a past end_time -- "
          "the real bug this fixes",
          "[RecurringRuleRenewal]")
{
  // A recurring occurrence that never actually ran (e.g. Dispatcharr/
  // Celery was down for its whole window) stays at status=="scheduled"
  // forever, with both start and end time now in the past --
  // ParseRecordingFields() treats "scheduled" as authoritatively
  // isUpcoming regardless of the clock. Before this fix, such an
  // occurrence's startTime - now was a large negative number, always
  // less than safetyMarginSeconds, so it read as permanently "imminent"
  // and silently blocked this rule from ever renewing again.
  RecurringRule rule = MakeRule(1, kNow + 5 * 86400);
  Recording rec;
  rec.recurringRuleId = 1;
  rec.isUpcoming = true;
  rec.startTime = kNow - 10 * 86400;
  rec.endTime = kNow - 10 * 86400 + 3600; // ended 10 days ago, well before kNow

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

// ---------------------------------------------------------------------
// ShouldExtendRecurringRuleEndDateOnUpdate
// ---------------------------------------------------------------------

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate is false when the edit leaves the rule disabled",
          "[RecurringRuleRenewal]")
{
  CHECK_FALSE(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/false, /*enabled=*/false,
                                                       /*cachedEndDate=*/kNow - 86400, kNow, kWindowDays));
}

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate is true for a re-enabled rule whose end_date already passed -- "
          "the real bug this fixes",
          "[RecurringRuleRenewal]")
{
  // The exact gap ShouldRenewRecurringRule()'s own already-expired-rule
  // fix opened: a rule left disabled long enough for its end_date to
  // pass, then re-enabled from Kodi -- UpdateRecurringRule()'s own PATCH
  // never touches end_date, so without this, the rule would be stuck
  // enabled-but-permanently-expired, since the periodic renewal loop now
  // correctly refuses to touch an already-past end_date.
  CHECK(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/false, /*enabled=*/true,
                                                 /*cachedEndDate=*/kNow - 86400, kNow, kWindowDays));
}

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate is false for an edit that doesn't touch enabled state at all, "
          "even if the rule is already past its window -- the real bug a later pass fixes",
          "[RecurringRuleRenewal]")
{
  // Checking only the post-edit `enabled` value (an earlier version of
  // this function) fired on ANY edit of an already-enabled rule -- a
  // rename, a schedule tweak -- not just an explicit re-enable, silently
  // resurrecting exactly the kind of deliberately-finite, already-expired,
  // still-enabled rule ShouldRenewRecurringRule()'s own "don't resurrect
  // an expired rule" fix exists to leave alone. Requiring the
  // wasEnabled -> enabled transition closes that: an edit that was
  // already enabled and stays enabled must not extend anything on its
  // own.
  CHECK_FALSE(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/true, /*enabled=*/true,
                                                       /*cachedEndDate=*/kNow - 86400, kNow, kWindowDays));
}

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate is false for a still-enabled rule comfortably inside its window",
          "[RecurringRuleRenewal]")
{
  CHECK_FALSE(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/false, /*enabled=*/true,
                                                       /*cachedEndDate=*/kNow + 20 * 86400, kNow, kWindowDays));
}

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate is true once less than half the window remains, even without "
          "having expired yet, for a genuine re-enable",
          "[RecurringRuleRenewal]")
{
  CHECK(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/false, /*enabled=*/true,
                                                 /*cachedEndDate=*/kNow + 10 * 86400, kNow, kWindowDays));
}

TEST_CASE("ShouldExtendRecurringRuleEndDateOnUpdate leaves a genuinely open-ended rule (no end_date at all) alone "
          "on re-enable -- the real bug this fixes",
          "[RecurringRuleRenewal]")
{
  // cachedEndDate == 0 is the same "no end_date at all" sentinel
  // ShouldRenewRecurringRule() above already relies on (a null or
  // unparseable end_date parses to 0, see ParseRecurringRuleJson()).
  // Without this guard, (0 - now) is a large negative number that would
  // otherwise pass the "less than half the window remains" check just
  // like a genuinely expired rule, silently converting an intentionally
  // permanent, open-ended rule into a rolling-window one on a plain
  // re-enable.
  CHECK_FALSE(ShouldExtendRecurringRuleEndDateOnUpdate(/*wasEnabled=*/false, /*enabled=*/true, /*cachedEndDate=*/0,
                                                       kNow, kWindowDays));
}

// ---------------------------------------------------------------------
// ComputeRecurringRuleEndDateForStartDateChange
// ---------------------------------------------------------------------

TEST_CASE("ComputeRecurringRuleEndDateForStartDateChange is 0 when the new start date stays safely inside the "
          "existing end_date",
          "[RecurringRuleRenewal]")
{
  CHECK(ComputeRecurringRuleEndDateForStartDateChange(/*newStartDate=*/kNow, /*cachedEndDate=*/kNow + 5 * 86400,
                                                      kWindowDays) == 0);
}

TEST_CASE("ComputeRecurringRuleEndDateForStartDateChange extends when the new start date reaches the existing "
          "end_date -- the real bug this fixes",
          "[RecurringRuleRenewal]")
{
  // A first-day edit landing exactly on the current end_date matters on
  // its own for an overnight rule, whose own combine(end_date, end_time)
  // <= combine(start_date, start_time) check upstream rejects even a
  // same-day (not just a later) start_date -- see this function's own
  // header comment.
  time_t cachedEndDate = kNow + 5 * 86400;
  time_t result =
      ComputeRecurringRuleEndDateForStartDateChange(/*newStartDate=*/cachedEndDate, cachedEndDate, kWindowDays);
  CHECK(result == cachedEndDate + static_cast<time_t>(kWindowDays) * 86400);
}

TEST_CASE("ComputeRecurringRuleEndDateForStartDateChange extends when the new start date is past the existing "
          "end_date",
          "[RecurringRuleRenewal]")
{
  time_t cachedEndDate = kNow + 5 * 86400;
  time_t newStartDate = cachedEndDate + 10 * 86400;
  CHECK(ComputeRecurringRuleEndDateForStartDateChange(newStartDate, cachedEndDate, kWindowDays) ==
        newStartDate + static_cast<time_t>(kWindowDays) * 86400);
}

TEST_CASE("ComputeRecurringRuleEndDateForStartDateChange is 0 for a genuinely open-ended rule (no end_date at all)",
          "[RecurringRuleRenewal]")
{
  CHECK(ComputeRecurringRuleEndDateForStartDateChange(/*newStartDate=*/kNow + 400 * 86400, /*cachedEndDate=*/0,
                                                      kWindowDays) == 0);
}

// ---------------------------------------------------------------------
// HasRecurringRuleEndDatePassed
// ---------------------------------------------------------------------

TEST_CASE("HasRecurringRuleEndDatePassed is false for a disabled rule regardless of end_date", "[RecurringRuleRenewal]")
{
  CHECK_FALSE(
      HasRecurringRuleEndDatePassed(/*enabled=*/false, /*endDate=*/kNow - 2 * 86400, kNow, /*offsetMinutes=*/0));
}

TEST_CASE("HasRecurringRuleEndDatePassed is false for a genuinely open-ended rule (no end_date at all)",
          "[RecurringRuleRenewal]")
{
  CHECK_FALSE(HasRecurringRuleEndDatePassed(/*enabled=*/true, /*endDate=*/0, kNow, 0));
}

TEST_CASE("HasRecurringRuleEndDatePassed is false while still comfortably before end_date", "[RecurringRuleRenewal]")
{
  CHECK_FALSE(HasRecurringRuleEndDatePassed(/*enabled=*/true, /*endDate=*/kNow + 5 * 86400, kNow, 0));
}

TEST_CASE("HasRecurringRuleEndDatePassed is false exactly at end_date's own UTC midnight -- Dispatcharr's own "
          "end_date is inclusive, the real bug this fixes",
          "[RecurringRuleRenewal]")
{
  // Confirmed against Dispatcharr's own real current upstream source, not
  // itself independently reproduced: sync_recurring_rule_impl()'s own
  // day-generation loop still schedules an occurrence ON the end_date
  // day itself, so the rule must not be flagged as expired the instant
  // UTC clock reaches midnight of that calendar day.
  CHECK_FALSE(HasRecurringRuleEndDatePassed(/*enabled=*/true, /*endDate=*/kNow, kNow, /*offsetMinutes=*/0));
}

TEST_CASE("HasRecurringRuleEndDatePassed is true a full day past end_date at UTC (offsetMinutes=0)",
          "[RecurringRuleRenewal]")
{
  CHECK(HasRecurringRuleEndDatePassed(/*enabled=*/true, /*endDate=*/kNow - 86400, kNow, /*offsetMinutes=*/0));
}

TEST_CASE("HasRecurringRuleEndDatePassed accounts for a zone behind UTC not yet having reached local midnight "
          "past end_date",
          "[RecurringRuleRenewal]")
{
  // America/New_York, EDT (-240 minutes). end_date's own UTC midnight
  // plus a day is still only 20:00 local the evening before Dispatcharr's
  // own local calendar actually turns over past end_date -- must not be
  // flagged as expired yet.
  time_t endDate = kNow - 86400; // end_date "yesterday" at UTC midnight
  CHECK_FALSE(
      HasRecurringRuleEndDatePassed(/*enabled=*/true, endDate, /*now=*/endDate + 86400, /*offsetMinutes=*/-240));
}

TEST_CASE("HasRecurringRuleEndDatePassed is true once Dispatcharr's own local calendar has fully moved past "
          "end_date for a zone behind UTC",
          "[RecurringRuleRenewal]")
{
  time_t endDate = kNow - 2 * 86400;
  time_t localMidnightPastEndDateUtc = endDate + 86400 - (-240 * 60); // 04:00 UTC the day after end_date
  CHECK_FALSE(HasRecurringRuleEndDatePassed(true, endDate, localMidnightPastEndDateUtc - 1, -240));
  CHECK(HasRecurringRuleEndDatePassed(true, endDate, localMidnightPastEndDateUtc, -240));
}

TEST_CASE("HasRecurringRuleEndDatePassed accounts for a zone ahead of UTC reaching local midnight past "
          "end_date before UTC does",
          "[RecurringRuleRenewal]")
{
  // India Standard Time (+330 minutes) reaches local midnight the day
  // after end_date several hours before UTC midnight of that same day.
  time_t endDate = kNow - 2 * 86400;
  time_t localMidnightPastEndDateUtc = endDate + 86400 - (330 * 60);
  CHECK_FALSE(HasRecurringRuleEndDatePassed(true, endDate, localMidnightPastEndDateUtc - 1, 330));
  CHECK(HasRecurringRuleEndDatePassed(true, endDate, localMidnightPastEndDateUtc, 330));
}

TEST_CASE("renewal is decided on the exact half-window boundary", "[RecurringRuleRenewal]")
{
  const time_t half = static_cast<time_t>(kWindowDays / 2) * 86400;
  // Exactly half a window left: comfortably inside, no renewal. One second under: renew.
  CHECK_FALSE(ShouldRenewRecurringRule(MakeRule(1, kNow + half), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  CHECK_FALSE(
      ShouldRenewRecurringRule(MakeRule(1, kNow + half + 86399), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
  CHECK(ShouldRenewRecurringRule(MakeRule(1, kNow + half - 1), {}, true, kNow, kWindowDays, kSafetyMarginSeconds));
}

TEST_CASE("the on-update extension uses the same exact half-window boundary", "[RecurringRuleRenewal]")
{
  const time_t half = static_cast<time_t>(kWindowDays / 2) * 86400;
  CHECK_FALSE(ShouldExtendRecurringRuleEndDateOnUpdate(false, true, kNow + half, kNow, kWindowDays));
  CHECK(ShouldExtendRecurringRuleEndDateOnUpdate(false, true, kNow + half - 1, kNow, kWindowDays));
}
