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
