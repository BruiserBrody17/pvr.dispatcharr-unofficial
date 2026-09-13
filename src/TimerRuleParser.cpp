#include "TimerRuleParser.h"

#include "DateTimeFormat.h"
#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

TimerRule ParseTimerRuleJson(const nlohmann::json& item)
{
  TimerRule t;
  t.id = FieldOr(item, "id", 0);
  t.channelId = FieldOr(item, "channel_id", FieldOr(item, "channel", 0));
  t.tvgId = FieldOr<std::string>(item, "tvg_id", "");
  t.title = FieldOr(item, "title", FieldOr<std::string>(item, "title_pattern", ""));
  t.titlePattern = FieldOr<std::string>(item, "title_pattern", t.title);
  t.isSeries = true;
  t.recordNewOnly = FieldOr<std::string>(item, "mode", "all") == "new";
  return t;
}

RecurringRule ParseRecurringRuleJson(const nlohmann::json& item)
{
  RecurringRule rule;
  rule.id = FieldOr(item, "id", 0);
  rule.channelId = FieldOr(item, "channel", 0);
  rule.name = FieldOr<std::string>(item, "name", "");
  rule.enabled = FieldOr(item, "enabled", true);
  rule.startTimeOfDaySeconds = SecondsSinceMidnightFromString(FieldOr<std::string>(item, "start_time", ""));
  rule.endTimeOfDaySeconds = SecondsSinceMidnightFromString(FieldOr<std::string>(item, "end_time", ""));
  rule.startDate = TimeFromDateString(FieldOr<std::string>(item, "start_date", ""));
  rule.endDate = TimeFromDateString(FieldOr<std::string>(item, "end_date", ""));
  const nlohmann::json& days = item.contains("days_of_week") ? item["days_of_week"] : nlohmann::json();
  if (days.is_array())
  {
    for (const auto& d : days)
    {
      if (d.is_number_integer())
        rule.daysOfWeek.push_back(d.get<int>());
    }
  }
  return rule;
}

} // namespace dispatcharr
