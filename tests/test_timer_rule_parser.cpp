#include "TimerRuleParser.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("ParseTimerRuleJson maps the confirmed field names", "[TimerRuleParser]")
{
  json item = {{"id", 1}, {"channel_id", 5}, {"tvg_id", "abc"}, {"title", "My Show"}, {"mode", "new"}};

  TimerRule t = ParseTimerRuleJson(item);

  CHECK(t.id == 1);
  CHECK(t.channelId == 5);
  CHECK(t.tvgId == "abc");
  CHECK(t.title == "My Show");
  CHECK(t.titlePattern == "My Show");
  CHECK(t.isSeries);
  CHECK(t.recordNewOnly);
}

TEST_CASE("ParseTimerRuleJson falls back to channel when channel_id is absent", "[TimerRuleParser]")
{
  CHECK(ParseTimerRuleJson(json{{"channel", 9}}).channelId == 9);
}

TEST_CASE("ParseTimerRuleJson falls back to title_pattern when title is absent", "[TimerRuleParser]")
{
  json item = {{"title_pattern", "Pattern Title"}};

  TimerRule t = ParseTimerRuleJson(item);

  CHECK(t.title == "Pattern Title");
  CHECK(t.titlePattern == "Pattern Title");
}

TEST_CASE("ParseTimerRuleJson defaults recordNewOnly to false for mode=all or absent", "[TimerRuleParser]")
{
  CHECK_FALSE(ParseTimerRuleJson(json{{"mode", "all"}}).recordNewOnly);
  CHECK_FALSE(ParseTimerRuleJson(json::object()).recordNewOnly);
}

TEST_CASE("ParseTimerRuleJson maps title_mode/description/description_mode/untagged_is_new/epg_source_id",
          "[TimerRuleParser]")
{
  json item = {{"title_mode", "contains"},
               {"description", "a real filter"},
               {"description_mode", "regex"},
               {"untagged_is_new", true},
               {"epg_source_id", 7}};

  TimerRule t = ParseTimerRuleJson(item);

  CHECK(t.titleMode == "contains");
  CHECK(t.description == "a real filter");
  CHECK(t.descriptionMode == "regex");
  CHECK(t.untaggedIsNew);
  CHECK(t.epgSourceId == 7);
}

TEST_CASE("ParseTimerRuleJson defaults title_mode/description/description_mode/untagged_is_new/epg_source_id "
          "to Dispatcharr's own server-side defaults when absent",
          "[TimerRuleParser]")
{
  // Real, confirmed bug this fix exists to prevent (docs/OPEN_ITEMS.md):
  // these defaults must match SeriesRulesAPIView.post()'s own exactly,
  // so a rule that genuinely has none of these customized round-trips
  // identically either way -- a mismatch here would make every such
  // rule look "customized" to UpdateTimer()'s own echo-back logic when
  // it isn't.
  TimerRule t = ParseTimerRuleJson(json::object());

  CHECK(t.titleMode == "exact");
  CHECK(t.description.empty());
  CHECK(t.descriptionMode == "contains");
  CHECK_FALSE(t.untaggedIsNew);
  CHECK(t.epgSourceId == 0);
}

TEST_CASE("ParseRecurringRuleJson maps the bare fields", "[TimerRuleParser]")
{
  json item = {{"id", 3}, {"channel", 7}, {"name", "Weekly Show"}, {"enabled", false}};

  RecurringRule rule = ParseRecurringRuleJson(item);

  CHECK(rule.id == 3);
  CHECK(rule.channelId == 7);
  CHECK(rule.name == "Weekly Show");
  CHECK_FALSE(rule.enabled);
}

TEST_CASE("ParseRecurringRuleJson defaults enabled to true when absent", "[TimerRuleParser]")
{
  CHECK(ParseRecurringRuleJson(json::object()).enabled);
}

TEST_CASE("ParseRecurringRuleJson parses start_time/end_time/start_date/end_date", "[TimerRuleParser]")
{
  json item = {
      {"start_time", "14:30:00"}, {"end_time", "15:30:00"}, {"start_date", "2026-01-01"}, {"end_date", "2026-01-15"}};

  RecurringRule rule = ParseRecurringRuleJson(item);

  CHECK(rule.startTimeOfDaySeconds == 14 * 3600 + 30 * 60);
  CHECK(rule.endTimeOfDaySeconds == 15 * 3600 + 30 * 60);
  CHECK(rule.startDate == 1767225600); // 2026-01-01T00:00:00Z
  CHECK(rule.endDate == 1768435200);   // 2026-01-15T00:00:00Z
}

TEST_CASE("ParseRecurringRuleJson collects only integer entries from days_of_week", "[TimerRuleParser]")
{
  json item = {{"days_of_week", {0, "not-an-int", 3, nullptr, 6}}};

  RecurringRule rule = ParseRecurringRuleJson(item);

  CHECK(rule.daysOfWeek == std::vector<int>{0, 3, 6});
}

TEST_CASE("ParseRecurringRuleJson leaves daysOfWeek empty when days_of_week is absent or not an array",
          "[TimerRuleParser]")
{
  CHECK(ParseRecurringRuleJson(json::object()).daysOfWeek.empty());
  CHECK(ParseRecurringRuleJson(json{{"days_of_week", "not-an-array"}}).daysOfWeek.empty());
}

TEST_CASE("ParseRecurringRuleJson keeps only integer weekdays from days_of_week", "[TimerRuleParser]")
{
  // 1.5 and "2" are not integers; the weekday list must not pick them up by coercion.
  auto item = nlohmann::json::parse(R"({"id":3,"days_of_week":[1.5,"2",null,true]})");
  CHECK(ParseRecurringRuleJson(item).daysOfWeek.empty());
  auto good = nlohmann::json::parse(R"({"id":3,"days_of_week":[0,2,4]})");
  CHECK(ParseRecurringRuleJson(good).daysOfWeek == std::vector<int>{0, 2, 4});
}
