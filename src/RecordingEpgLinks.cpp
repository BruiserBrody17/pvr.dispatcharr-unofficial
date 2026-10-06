#include "RecordingEpgLinks.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

namespace
{
constexpr int kFormatVersion = 1;
}

std::string SerializeRecordingEpgLinks(const RecordingEpgLinkMap& links)
{
  nlohmann::json entries = nlohmann::json::array();
  for (const auto& [id, link] : links)
  {
    entries.push_back({{"id", id},
                       {"channel", link.channelId},
                       {"recording_start", static_cast<long long>(link.recordingStartTime)},
                       {"program_start", static_cast<long long>(link.programStartTime)}});
  }
  return nlohmann::json({{"version", kFormatVersion}, {"links", entries}}).dump();
}

bool ParseRecordingEpgLinks(const std::string& text, RecordingEpgLinkMap& out)
{
  out.clear();
  nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!doc.is_object() || FieldOr(doc, "version", 0) != kFormatVersion || !doc.contains("links") ||
      !doc["links"].is_array())
    return false;

  for (const auto& entry : doc["links"])
  {
    if (out.size() >= kMaxRecordingEpgLinks)
      break;
    if (!entry.is_object())
      continue;
    const int id = FieldOr(entry, "id", 0);
    RecordingEpgLink link;
    link.channelId = FieldOr(entry, "channel", 0);
    link.recordingStartTime = static_cast<time_t>(FieldOr<long long>(entry, "recording_start", 0));
    link.programStartTime = static_cast<time_t>(FieldOr<long long>(entry, "program_start", 0));
    if (id <= 0 || link.channelId <= 0 || link.recordingStartTime <= 0 || link.programStartTime <= 0)
      continue;
    out[id] = link;
  }
  return true;
}

bool RememberRecordingEpgLink(RecordingEpgLinkMap& links, int recordingId, const RecordingEpgLink& link)
{
  if (recordingId <= 0 || link.channelId <= 0 || link.recordingStartTime <= 0 || link.programStartTime <= 0)
    return false;
  auto it = links.find(recordingId);
  if (it != links.end() && it->second.channelId == link.channelId &&
      it->second.recordingStartTime == link.recordingStartTime && it->second.programStartTime == link.programStartTime)
    return false;
  if (it == links.end() && links.size() >= kMaxRecordingEpgLinks)
    return false;
  links[recordingId] = link;
  return true;
}

time_t LookupRememberedProgramStart(const RecordingEpgLinkMap& links, int recordingId, int channelId,
                                    time_t recordingStartTime)
{
  auto it = links.find(recordingId);
  if (it == links.end())
    return 0;
  if (it->second.channelId != channelId || it->second.recordingStartTime != recordingStartTime)
    return 0;
  return it->second.programStartTime;
}

std::size_t PruneRecordingEpgLinks(RecordingEpgLinkMap& links, const std::unordered_set<int>& liveRecordingIds)
{
  std::size_t removed = 0;
  for (auto it = links.begin(); it != links.end();)
  {
    if (liveRecordingIds.count(it->first))
    {
      ++it;
    }
    else
    {
      it = links.erase(it);
      ++removed;
    }
  }
  return removed;
}

} // namespace dispatcharr
