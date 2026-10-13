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

std::string SeriesRuleTitleModeForSetting(int setting)
{
  switch (setting)
  {
  case kSeriesTitleModeContains:
    return "contains";
  case kSeriesTitleModeSearch:
    return "search";
  default:
    return "";
  }
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
                                               bool includeEpgProgramWindow, const nlohmann::json& programFields)
{
  time_t effectiveStart = start > 0 ? start : now;
  nlohmann::json body = {
      {"channel", channelId},
      {"start_time", IsoFromTime(effectiveStart)},
      {"end_time", IsoFromTime(end)},
  };
  if (includeEpgProgramWindow)
  {
    nlohmann::json program = {{"start_time", IsoFromTime(effectiveStart)}, {"end_time", IsoFromTime(end)}};
    if (programFields.is_object())
    {
      for (const char* key : {"id", "title", "sub_title", "description", "tvg_id"})
      {
        auto it = programFields.find(key);
        if (it != programFields.end() && !it->is_null())
          program[key] = *it;
      }
    }
    body["custom_properties"] = {{"program", program}};
  }
  return body;
}

nlohmann::json BuildOneTimeRecordingCreateBody(int channelId, time_t start, time_t end, time_t now,
                                               bool includeEpgProgramWindow)
{
  return BuildOneTimeRecordingCreateBody(channelId, start, end, now, includeEpgProgramWindow, nlohmann::json());
}

std::string BuildProgramLookupQuery(int channelId, time_t start)
{
  return "?channel_id=" + std::to_string(channelId) + "&start_after=" + UrlEncode(IsoFromTime(start - 1)) +
         "&start_before=" + UrlEncode(IsoFromTime(start + 1)) +
         "&fields=id,title,sub_title,description,tvg_id,start_time,end_time";
}

nlohmann::json ParseProgramLookupResponse(const nlohmann::json& response, time_t start, time_t end)
{
  const nlohmann::json* list = nullptr;
  if (response.is_array())
    list = &response;
  else if (response.is_object() && response.contains("results") && response["results"].is_array())
    list = &response["results"];
  if (!list)
    return nullptr;

  const nlohmann::json* best = nullptr;
  bool bestEndsRight = false;
  for (const auto& entry : *list)
  {
    if (!entry.is_object())
      continue;
    const auto id = entry.find("id");
    const auto title = entry.find("title");
    if (id == entry.end() || !id->is_number_integer() || id->get<long long>() <= 0 || title == entry.end() ||
        !title->is_string() || title->get<std::string>().empty())
      continue;
    const auto st = entry.find("start_time");
    if (st == entry.end() || !st->is_string() || TimeFromIso(st->get<std::string>()) != start)
      continue;
    const auto en = entry.find("end_time");
    const bool endsRight = en != entry.end() && en->is_string() && TimeFromIso(en->get<std::string>()) == end;
    if (!best || (endsRight && !bestEndsRight))
    {
      best = &entry;
      bestEndsRight = endsRight;
    }
  }
  if (!best)
    return nullptr;

  nlohmann::json out = nlohmann::json::object();
  for (const char* key : {"id", "title", "sub_title", "description", "tvg_id"})
  {
    auto it = best->find(key);
    if (it != best->end() && !it->is_null())
      out[key] = *it;
  }
  return out;
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
