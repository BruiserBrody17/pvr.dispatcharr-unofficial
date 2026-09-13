#include "LiveManifestParser.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

std::vector<LiveManifestSegmentEntry> ParseNewLiveManifestSegments(const nlohmann::json& segmentsArray,
                                                                   int64_t lastKnownSequence)
{
  std::vector<LiveManifestSegmentEntry> out;
  for (const nlohmann::json& seg : segmentsArray)
  {
    int64_t sequence = FieldOr<int64_t>(seg, "sequence", -1);
    if (sequence < 0 || sequence <= lastKnownSequence)
      continue;

    LiveManifestSegmentEntry entry;
    entry.filename = FieldOr<std::string>(seg, "filename", "");
    entry.byteSize = FieldOr<int64_t>(seg, "byte_size", 0);
    entry.durationMs = FieldOr<int64_t>(seg, "duration_ms", 0);
    if (entry.filename.empty() || entry.byteSize <= 0)
      continue; // malformed entry -- don't let it corrupt the caller's own cumulative offsets

    entry.sequence = sequence;
    out.push_back(std::move(entry));
  }
  return out;
}

} // namespace dispatcharr
