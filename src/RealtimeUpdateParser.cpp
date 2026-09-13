#include "RealtimeUpdateParser.h"

#include <nlohmann/json.hpp>

#include <unordered_set>

namespace dispatcharr
{

std::string ParseRelevantRealtimeUpdateEventType(const std::string& message)
{
  static const std::unordered_set<std::string> kRelevantEventTypes = {
      "recording_started", "recording_ended",     "recording_stopped",   "recording_extended",
      "recording_updated", "recording_cancelled", "recordings_refreshed"};
  try
  {
    nlohmann::json parsed = nlohmann::json::parse(message);
    if (!parsed.contains("data") || !parsed["data"].is_object())
      return "";
    const nlohmann::json& data = parsed["data"];
    if (!data.contains("type") || !data["type"].is_string())
      return "";
    std::string eventType = data["type"].get<std::string>();
    if (kRelevantEventTypes.count(eventType) == 0)
      return "";
    return eventType;
  }
  catch (const nlohmann::json::exception&)
  {
    // Malformed/unexpected payload -- not this connection's problem to
    // solve; just treat it as irrelevant and keep listening.
    return "";
  }
}

} // namespace dispatcharr
