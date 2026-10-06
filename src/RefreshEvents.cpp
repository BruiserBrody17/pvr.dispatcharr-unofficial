#include "RefreshEvents.h"

#include <climits>

#include <nlohmann/json.hpp>

namespace dispatcharr
{

namespace
{
// A channel count the event reports, or -1 when the field is missing or is not a number.
long long CountField(const nlohmann::json& data, const char* key)
{
  const auto it = data.find(key);
  if (it == data.end() || !it->is_number())
    return -1;
  // An integer is read as one; anything else (a float is not something the server sends here, but this is the wire)
  // is read as a double and compared, because get<long long>() on a double beyond the integer range is undefined
  // behaviour.
  if (it->is_number_integer())
    return it->is_number_unsigned() && it->get<unsigned long long>() > static_cast<unsigned long long>(LLONG_MAX)
               ? LLONG_MAX
               : it->get<long long>();
  const double value = it->get<double>();
  if (!(value > 0))
    return value < 0 ? -1 : 0; // NaN is not a count either
  return value >= 9.0e18 ? LLONG_MAX : static_cast<long long>(value);
}
} // namespace

RefreshEventKind ClassifyRefreshEvent(const std::string& message)
{
  try
  {
    const nlohmann::json parsed = nlohmann::json::parse(message);
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object())
      return RefreshEventKind::kNone;
    const nlohmann::json& data = parsed["data"];
    const auto field = [&data](const char* key) -> std::string
    {
      const auto it = data.find(key);
      return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const std::string type = field("type");
    const std::string action = field("action");
    const std::string status = field("status");
    if (status != "success")
      return RefreshEventKind::kNone;

    if (type == "epg_refresh")
    {
      const auto progress = data.find("progress");
      const bool complete = progress != data.end() && progress->is_number() && progress->get<double>() >= 100.0;
      return action == "parsing_programs" && complete ? RefreshEventKind::kGuideParsed : RefreshEventKind::kNone;
    }
    if (type == "m3u_refresh" && action == "parsing")
    {
      const long long created = CountField(data, "channels_created");
      const long long updated = CountField(data, "channels_updated");
      const long long deleted = CountField(data, "channels_deleted");
      // Not saying is not the same as saying nothing changed.
      if (created < 0 && updated < 0 && deleted < 0)
        return RefreshEventKind::kChannelsSynced;
      return created > 0 || updated > 0 || deleted > 0 ? RefreshEventKind::kChannelsSynced : RefreshEventKind::kNone;
    }
    return RefreshEventKind::kNone;
  }
  catch (const nlohmann::json::exception&)
  {
    return RefreshEventKind::kNone;
  }
}

std::chrono::steady_clock::time_point ScheduleGuideRefetchForEvent(std::chrono::steady_clock::time_point currentDueAt,
                                                                   std::chrono::steady_clock::time_point now)
{
  const auto due = now + kGuideRefetchAfterRefreshEvent;
  // An already scheduled fetch that is due later than this refresh needs keeps its time. One due sooner (or already
  // due, or running) would fetch inside Dispatcharr's cache window of this refresh and be served the old guide, then
  // clear the schedule: it moves out to this event's time instead, and a fetch running now no longer matches the due
  // time it saw, so it does not clear it.
  if (currentDueAt != std::chrono::steady_clock::time_point{} && currentDueAt >= due)
    return currentDueAt;
  return due;
}

} // namespace dispatcharr
