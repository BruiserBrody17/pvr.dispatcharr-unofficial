#include "TimerRuleParser.h"

#include "DateTimeFormat.h"
#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

#include <cstdint>

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
  // See TimerRule::titleMode's own comment (DispatcharrClient.h) for why
  // these are cached at all -- defaults here match Dispatcharr's own
  // SeriesRulesAPIView.post() server-side defaults exactly, so a rule
  // that genuinely has none of these customized round-trips identically
  // either way.
  t.titleMode = FieldOr<std::string>(item, "title_mode", "exact");
  t.description = FieldOr<std::string>(item, "description", "");
  t.descriptionMode = FieldOr<std::string>(item, "description_mode", "contains");
  t.untaggedIsNew = FieldOr(item, "untagged_is_new", false);
  t.epgSourceId = FieldOr(item, "epg_source_id", 0);
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
  const nlohmann::json& days = FieldRefOrNull(item, "days_of_week");
  if (days.is_array())
  {
    // Read as int64 and range-checked: get<int>() wrapped an unsigned or out-of-range value, so 4294967297 became
    // the valid day 1 (found by the 2026-10-18 read-only audit). Anything outside 0-6 is not a day.
    for (const auto& d : days)
    {
      if (!d.is_number_integer())
        continue;
      const bool isDay =
          d.is_number_unsigned() ? d.get<uint64_t>() <= 6 : d.get<int64_t>() >= 0 && d.get<int64_t>() <= 6;
      if (isDay)
        rule.daysOfWeek.push_back(static_cast<int>(d.get<int64_t>()));
    }
  }
  return rule;
}

} // namespace dispatcharr
