#include "RecurringRuleEdit.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
RecurringRuleFields Rule()
{
  RecurringRuleFields r;
  r.channelId = 7;
  r.name = "Evening News";
  r.daysOfWeek = {0, 1, 2, 3, 4};
  r.startTimeOfDaySeconds = 22 * 3600;
  r.endTimeOfDaySeconds = 23 * 3600;
  r.startDate = 1767225600; // 2026-01-01
  r.enabled = true;
  return r;
}
} // namespace

TEST_CASE("an edit that changes nothing produces an empty patch", "[RecurringRuleEdit]")
{
  CHECK(ComputeRecurringRuleEditPatch(Rule(), Rule()).IsEmpty());
}

TEST_CASE("a bare enable/disable toggle sends only enabled", "[RecurringRuleEdit]")
{
  RecurringRuleFields edited = Rule();
  edited.enabled = false;
  RecurringRuleEditPatch patch = ComputeRecurringRuleEditPatch(Rule(), edited);
  REQUIRE(patch.enabled);
  CHECK(*patch.enabled == false);
  CHECK_FALSE(patch.channelId);
  CHECK_FALSE(patch.name);
  CHECK_FALSE(patch.daysOfWeek);
  CHECK_FALSE(patch.startTimeOfDaySeconds);
  CHECK_FALSE(patch.endTimeOfDaySeconds);
  CHECK_FALSE(patch.startDate);
  CHECK_FALSE(patch.endDate);
}

TEST_CASE("each field is detected independently", "[RecurringRuleEdit]")
{
  {
    RecurringRuleFields e = Rule();
    e.name = "Late News";
    auto p = ComputeRecurringRuleEditPatch(Rule(), e);
    REQUIRE(p.name);
    CHECK(*p.name == "Late News");
    CHECK_FALSE(p.enabled);
  }
  {
    RecurringRuleFields e = Rule();
    e.channelId = 9;
    CHECK(*ComputeRecurringRuleEditPatch(Rule(), e).channelId == 9);
  }
  {
    RecurringRuleFields e = Rule();
    e.startTimeOfDaySeconds += 1800;
    auto p = ComputeRecurringRuleEditPatch(Rule(), e);
    REQUIRE(p.startTimeOfDaySeconds);
    CHECK_FALSE(p.endTimeOfDaySeconds);
  }
  {
    RecurringRuleFields e = Rule();
    e.endTimeOfDaySeconds += 1800;
    auto p = ComputeRecurringRuleEditPatch(Rule(), e);
    REQUIRE(p.endTimeOfDaySeconds);
    CHECK_FALSE(p.startTimeOfDaySeconds);
  }
  {
    RecurringRuleFields e = Rule();
    e.daysOfWeek = {0, 1, 2, 3, 4, 5};
    auto p = ComputeRecurringRuleEditPatch(Rule(), e);
    REQUIRE(p.daysOfWeek);
    CHECK(*p.daysOfWeek == std::vector<int>{0, 1, 2, 3, 4, 5});
  }
  {
    RecurringRuleFields e = Rule();
    e.startDate += 3 * 86400;
    CHECK(ComputeRecurringRuleEditPatch(Rule(), e).startDate);
  }
}

TEST_CASE("a time of day expressed from the neighbouring UTC day is not a change", "[RecurringRuleEdit]")
{
  // The baseline is what the server reports (0..86399). The edited side used to
  // arrive unwrapped from ComputeRecurringRuleFields(): a 22:00-23:00 rule in a
  // zone behind UTC as -7200/-3600. Compared raw, an unchanged rule sent both
  // times on every edit -- and every PATCH makes the server drop and regenerate
  // its future occurrences.
  RecurringRuleFields e = Rule();
  e.startTimeOfDaySeconds = 22 * 3600 - 86400;
  e.endTimeOfDaySeconds = 23 * 3600 - 86400;
  CHECK(ComputeRecurringRuleEditPatch(Rule(), e).IsEmpty());

  e.startTimeOfDaySeconds = 22 * 3600 + 86400;
  e.endTimeOfDaySeconds = 23 * 3600 + 86400;
  CHECK(ComputeRecurringRuleEditPatch(Rule(), e).IsEmpty());

  // A real change a day away is still one.
  e.startTimeOfDaySeconds = 21 * 3600 - 86400;
  e.endTimeOfDaySeconds = 23 * 3600;
  RecurringRuleEditPatch p = ComputeRecurringRuleEditPatch(Rule(), e);
  REQUIRE(p.startTimeOfDaySeconds);
  CHECK_FALSE(p.endTimeOfDaySeconds);
}

TEST_CASE("the same days in another order, or listed twice, are not a change", "[RecurringRuleEdit]")
{
  RecurringRuleFields e = Rule();
  e.daysOfWeek = {4, 3, 2, 1, 0, 0};
  CHECK_FALSE(ComputeRecurringRuleEditPatch(Rule(), e).daysOfWeek);
}

TEST_CASE("a start date within the same calendar day is not a change", "[RecurringRuleEdit]")
{
  RecurringRuleFields e = Rule();
  e.startDate += 12 * 3600;
  CHECK_FALSE(ComputeRecurringRuleEditPatch(Rule(), e).startDate);
  e.startDate = Rule().startDate + 86400;
  CHECK(ComputeRecurringRuleEditPatch(Rule(), e).startDate);
}

TEST_CASE("with no baseline every field is sent, as before", "[RecurringRuleEdit]")
{
  RecurringRuleEditPatch p = ComputeRecurringRuleEditPatch(std::nullopt, Rule());
  CHECK(p.channelId);
  CHECK(p.name);
  CHECK(p.daysOfWeek);
  CHECK(p.startTimeOfDaySeconds);
  CHECK(p.endTimeOfDaySeconds);
  CHECK(p.startDate);
  CHECK(p.enabled);
  CHECK_FALSE(p.endDate);
}

TEST_CASE("a field changed elsewhere is left alone by an edit of a different field", "[RecurringRuleEdit]")
{
  // The point of the change: baseline = what Kodi was shown. The server has since
  // moved to other days, but the user only renamed the rule -- days are not sent.
  RecurringRuleFields e = Rule();
  e.name = "Renamed";
  RecurringRuleEditPatch p = ComputeRecurringRuleEditPatch(Rule(), e);
  CHECK(p.name);
  CHECK_FALSE(p.daysOfWeek);
}

TEST_CASE("ComputeEndDateForOpenEndedRuleEdit gives only an open-ended rule an end date", "[RecurringRuleEdit]")
{
  const time_t now = 1767225600 + 10 * 86400;
  CHECK(ComputeEndDateForOpenEndedRuleEdit(1769904000, 1767225600, now, 30) == 0);
  CHECK(ComputeEndDateForOpenEndedRuleEdit(0, 1767225600, now, 30) == now + 30 * 86400);
}

TEST_CASE("the fallback end date counts its window from a start date that is still ahead", "[RecurringRuleEdit]")
{
  const time_t now = 1767225600;
  const time_t futureStart = now + 90 * 86400;
  CHECK(ComputeEndDateForOpenEndedRuleEdit(0, futureStart, now, 30) == futureStart + 30 * 86400);
  CHECK(ComputeEndDateForOpenEndedRuleEdit(0, futureStart, now, 30) >= futureStart);
}

TEST_CASE("ResolveRecurringRuleTimesOnEdit keeps the stored times when Kodi handed back exactly what it was shown",
          "[RecurringRuleEdit]")
{
  RecurringRuleFields baseline;
  baseline.startTimeOfDaySeconds = 9000; // 02:30
  baseline.endTimeOfDaySeconds = 71100;  // 19:45
  // The display showed 03:30-20:45 (the skipped hour), Kodi returns those instants unchanged, and
  // re-deriving gave 12600/75300: an hour off, and not what the user changed.
  const RuleTimesOfDay unchanged = ResolveRecurringRuleTimesOnEdit(baseline, 1000, 2000, std::optional<time_t>(1000),
                                                                   std::optional<time_t>(2000), 12600, 75300);
  CHECK(unchanged.startSeconds == 9000);
  CHECK(unchanged.endSeconds == 71100);
}

TEST_CASE("ResolveRecurringRuleTimesOnEdit uses the derived times once the user touched either instant",
          "[RecurringRuleEdit]")
{
  RecurringRuleFields baseline;
  baseline.startTimeOfDaySeconds = 9000;
  baseline.endTimeOfDaySeconds = 71100;
  // Start changed: the untouched end keeps its stored value.
  RuleTimesOfDay r = ResolveRecurringRuleTimesOnEdit(baseline, 1500, 2000, std::optional<time_t>(1000),
                                                     std::optional<time_t>(2000), 12600, 75300);
  CHECK(r.startSeconds == 12600);
  CHECK(r.endSeconds == 71100);
  // End changed: the untouched start keeps its stored value, only the end is derived.
  r = ResolveRecurringRuleTimesOnEdit(baseline, 1000, 2500, std::optional<time_t>(1000), std::optional<time_t>(2000),
                                      12600, 75300);
  CHECK(r.startSeconds == 9000);
  CHECK(r.endSeconds == 75300);
  // Nothing was ever reported, or there is no baseline: derived values are all there is.
  r = ResolveRecurringRuleTimesOnEdit(baseline, 1000, 2000, std::nullopt, std::nullopt, 12600, 75300);
  CHECK(r.startSeconds == 12600);
  r = ResolveRecurringRuleTimesOnEdit(std::nullopt, 1000, 2000, std::optional<time_t>(1000),
                                      std::optional<time_t>(2000), 12600, 75300);
  CHECK(r.endSeconds == 75300);
  // Only the start was reported: that side is known unchanged, the other is derived.
  r = ResolveRecurringRuleTimesOnEdit(baseline, 1000, 2000, std::optional<time_t>(1000), std::nullopt, 12600, 75300);
  CHECK(r.startSeconds == 9000);
  CHECK(r.endSeconds == 75300);
  // Only the end was reported (the mirror).
  r = ResolveRecurringRuleTimesOnEdit(baseline, 1000, 2000, std::nullopt, std::optional<time_t>(2000), 12600, 75300);
  CHECK(r.startSeconds == 12600);
  CHECK(r.endSeconds == 71100);
}

TEST_CASE("a baseline that lists a weekday twice is the same rule as an edit that lists it once", "[RecurringRuleEdit]")
{
  // Days are compared as sets on both sides: removing the de-duplication of the baseline's list made [0, 0, 1] differ
  // from [0, 1] and resend an unchanged rule (whose occurrences the server then regenerates).
  RecurringRuleFields baseline = Rule();
  baseline.daysOfWeek = {0, 0, 1};
  RecurringRuleFields edited = Rule();
  edited.daysOfWeek = {1, 0};
  CHECK(ComputeRecurringRuleEditPatch(baseline, edited).IsEmpty());
  edited.daysOfWeek = {1, 0, 1, 0};
  CHECK(ComputeRecurringRuleEditPatch(Rule(), edited).daysOfWeek.has_value());
  baseline.daysOfWeek = {0, 1};
  CHECK(ComputeRecurringRuleEditPatch(baseline, edited).IsEmpty());
}

namespace
{
ReportedRecurringRule Report(const RecurringRuleFields& fields = Rule())
{
  ReportedRecurringRule r;
  r.fields = fields;
  r.start = 1767304800; // 2026-01-01 22:00 UTC
  r.end = 1767308400;
  return r;
}
} // namespace

TEST_CASE("a report's token follows its content", "[RecurringRuleEdit]")
{
  const std::string token = ComputeReportedRecurringRuleToken(Report());
  CHECK(token.rfind("rr-", 0) == 0);
  CHECK(ComputeReportedRecurringRuleToken(Report()) == token);

  auto changed = [&token](void (*edit)(ReportedRecurringRule&))
  {
    ReportedRecurringRule r = Report();
    edit(r);
    return ComputeReportedRecurringRuleToken(r) != token;
  };
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.channelId = 8; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.name = "Late News"; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.daysOfWeek = {0, 1, 2, 3}; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.startTimeOfDaySeconds += 60; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.endTimeOfDaySeconds += 60; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.startDate += 86400; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.fields.enabled = false; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.start += 3600; }));
  CHECK(changed([](ReportedRecurringRule& r) { r.end += 3600; }));
  // Separators keep neighbouring values from running together.
  ReportedRecurringRule a = Report(), b = Report();
  a.fields.daysOfWeek = {1, 2};
  b.fields.daysOfWeek = {12};
  CHECK(ComputeReportedRecurringRuleToken(a) != ComputeReportedRecurringRuleToken(b));
}

TEST_CASE("an edit is diffed against the report its dialog was opened from, not a newer one", "[RecurringRuleEdit]")
{
  // The dialog opens on the first report; another client then renames the rule and a refresh reports that while
  // the dialog is still open. The user changes only the days.
  ReportedRecurringRule opened = Report();
  RecurringRuleFields renamedElsewhere = Rule();
  renamedElsewhere.name = "Renamed Elsewhere";
  ReportedRecurringRuleHistory history;
  history.Record({{5, opened}});
  history.Record({{5, Report(renamedElsewhere)}});

  RecurringRuleFields edited = Rule();
  edited.daysOfWeek = {5, 6};
  auto baseline = history.Find(5, ComputeReportedRecurringRuleToken(opened));
  REQUIRE(baseline);
  RecurringRuleEditPatch p = ComputeRecurringRuleEditPatch(baseline->fields, edited);
  CHECK(p.daysOfWeek);
  CHECK_FALSE(p.name); // diffed against the newest report, the old name was sent back
}

TEST_CASE("a report's times come back with its own fields", "[RecurringRuleEdit]")
{
  ReportedRecurringRule first = Report();
  ReportedRecurringRule moved = Report();
  moved.start += 3600;
  moved.end += 3600;
  ReportedRecurringRuleHistory history;
  history.Record({{5, first}});
  history.Record({{5, moved}});
  auto found = history.Find(5, ComputeReportedRecurringRuleToken(first));
  REQUIRE(found);
  CHECK(found->start == first.start);
  CHECK(found->end == first.end);
}

TEST_CASE("an unknown token falls back to the rule's newest report", "[RecurringRuleEdit]")
{
  RecurringRuleFields newer = Rule();
  newer.name = "Newer";
  ReportedRecurringRuleHistory history;
  history.Record({{5, Report()}});
  history.Record({{5, Report(newer)}});
  for (const std::string token : {std::string(), std::string("rr-0000000000000000"), std::string("unrelated")})
  {
    auto found = history.Find(5, token);
    REQUIRE(found);
    CHECK(found->fields.name == "Newer");
  }
  CHECK_FALSE(history.Find(6, ""));
}

TEST_CASE("a rule missing from a report is forgotten", "[RecurringRuleEdit]")
{
  ReportedRecurringRuleHistory history;
  history.Record({{5, Report()}, {6, Report()}});
  history.Record({{6, Report()}});
  CHECK_FALSE(history.Find(5, ComputeReportedRecurringRuleToken(Report())));
  CHECK(history.Find(6, ""));
}

TEST_CASE("a report seen again becomes the newest without being kept twice", "[RecurringRuleEdit]")
{
  RecurringRuleFields other = Rule();
  other.name = "Other";
  ReportedRecurringRuleHistory history;
  history.Record({{5, Report()}});
  history.Record({{5, Report(other)}});
  history.Record({{5, Report()}});
  CHECK(history.Find(5, "")->fields.name == "Evening News");
  CHECK(history.Find(5, ComputeReportedRecurringRuleToken(Report(other)))->fields.name == "Other");

  // Unchanged refreshes do not push older reports out.
  for (std::size_t i = 0; i < ReportedRecurringRuleHistory::kMaxReportsPerRule * 2; ++i)
    history.Record({{5, Report()}});
  CHECK(history.Find(5, ComputeReportedRecurringRuleToken(Report(other)))->fields.name == "Other");
}

TEST_CASE("only the newest reports of a rule are kept", "[RecurringRuleEdit]")
{
  ReportedRecurringRuleHistory history;
  std::vector<std::string> tokens;
  for (std::size_t i = 0; i <= ReportedRecurringRuleHistory::kMaxReportsPerRule; ++i)
  {
    RecurringRuleFields f = Rule();
    f.name = "Name " + std::to_string(i);
    tokens.push_back(ComputeReportedRecurringRuleToken(Report(f)));
    history.Record({{5, Report(f)}});
  }
  const std::string newest = "Name " + std::to_string(ReportedRecurringRuleHistory::kMaxReportsPerRule);
  CHECK(history.Find(5, tokens.front())->fields.name == newest); // evicted: newest instead
  CHECK(history.Find(5, tokens[1])->fields.name == "Name 1");
}
