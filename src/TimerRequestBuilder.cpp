#include "TimerRequestBuilder.h"

#include "DateTimeFormat.h"
#include "UrlEncode.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

nlohmann::json BuildSeriesRuleRequestBody(int channelId, const std::string& tvgId, const std::string& titlePattern,
                                          bool recordNewOnly, const std::string& titleMode,
                                          const std::string& description, const std::string& descriptionMode,
                                          bool untaggedIsNew, int epgSourceId)
{
  nlohmann::json body = {
      {"title", titlePattern},
  };
  if (channelId > 0)
    body["channel_id"] = channelId;
  if (!tvgId.empty())
    body["tvg_id"] = tvgId;
  if (recordNewOnly)
  {
    body["mode"] = "new";
    if (untaggedIsNew)
      body["untagged_is_new"] = true;
  }
  if (!titleMode.empty())
    body["title_mode"] = titleMode;
  if (!description.empty())
    body["description"] = description;
  if (!descriptionMode.empty())
    body["description_mode"] = descriptionMode;
  if (epgSourceId > 0)
    body["epg_source_id"] = epgSourceId;
  return body;
}

nlohmann::json BuildRecurringRuleUpdateBody(const RecurringRuleEditPatch& patch)
{
  nlohmann::json body = nlohmann::json::object();
  if (patch.channelId)
    body["channel"] = *patch.channelId;
  if (patch.name)
    body["name"] = *patch.name;
  if (patch.daysOfWeek)
    body["days_of_week"] = *patch.daysOfWeek;
  if (patch.startTimeOfDaySeconds)
    body["start_time"] = TimeOfDayString(*patch.startTimeOfDaySeconds);
  if (patch.endTimeOfDaySeconds)
    body["end_time"] = TimeOfDayString(*patch.endTimeOfDaySeconds);
  if (patch.startDate)
    body["start_date"] = DateStringFromTime(*patch.startDate);
  if (patch.enabled)
    body["enabled"] = *patch.enabled;
  if (patch.endDate)
    body["end_date"] = DateStringFromTime(*patch.endDate);
  return body;
}

nlohmann::json BuildOneTimeRecordingPatchBody(time_t start, time_t end, int channelId)
{
  nlohmann::json body = {
      {"start_time", IsoFromTime(start)},
      {"end_time", IsoFromTime(end)},
  };
  // A bare {"channel": ...} PATCH is a 500 server-side, the same uncaught
  // `end_time < now` on a missing end_time as any other partial update --
  // confirmed live 2026-09-30; with both times alongside it, the channel
  // change is accepted and the recording moves.
  if (channelId > 0)
    body["channel"] = channelId;
  return body;
}

int ChannelToSendOnOneTimeEdit(int kodiChannelUid, int currentChannelId)
{
  return (kodiChannelUid > 0 && kodiChannelUid != currentChannelId) ? kodiChannelUid : 0;
}

nlohmann::json BuildOneTimeRecordingCreateBody(int channelId, time_t start, time_t end, time_t now,
                                               bool includeEpgProgramWindow)
{
  time_t effectiveStart = start > 0 ? start : now;
  nlohmann::json body = {
      {"channel", channelId},
      {"start_time", IsoFromTime(effectiveStart)},
      {"end_time", IsoFromTime(end)},
  };
  if (includeEpgProgramWindow)
  {
    body["custom_properties"] = {
        {"program", {{"start_time", IsoFromTime(effectiveStart)}, {"end_time", IsoFromTime(end)}}},
    };
  }
  return body;
}

std::string BuildSeriesRuleDeleteQuery(const std::string& title, const std::string& tvgId, int epgSourceId)
{
  std::string query = "?title=" + UrlEncode(title);
  if (!tvgId.empty())
    query += "&tvg_id=" + UrlEncode(tvgId);
  if (epgSourceId > 0)
    query += "&epg_source_id=" + std::to_string(epgSourceId);
  return query;
}

} // namespace dispatcharr
