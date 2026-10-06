#include "LiveManifestParser.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

namespace
{
// Same generous per-segment ceiling as M3u8SegmentParser's own #EXTINF
// clamp (see its own comment) -- kept in sync deliberately, not shared,
// since these two parsers cover unrelated JSON shapes from unrelated
// producers (this addon's own timeshift_buffer plugin here, vs. a
// Dispatcharr-server-generated HLS playlist there).
constexpr int64_t kMaxSegmentDurationMs = 86400000;
// A generous per-segment byte-size ceiling (1 TiB), added 2026-09-27, a
// 52nd-pass audit, fixing a real, confirmed gap found via a project-wide
// review, not reproduced live: unlike durationMs just below, byteSize
// had no upper bound at all before this, despite the exact same
// overflow risk this file's own comment on durationMs already
// documents -- AppendSegmentOffsets() (SegmentAppendOffsets.h) adds each
// entry's byteSize into the caller's own running cumulative total
// (totalBytesInOut += sizeBytes), and two sufficiently huge byteSize
// values summed is signed int64 overflow, undefined behavior in C++.
// No real HLS segment is ever legitimately anywhere near this large.
constexpr int64_t kMaxSegmentByteSize = 1099511627776; // 1 TiB
} // namespace

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
    if (entry.byteSize > kMaxSegmentByteSize)
      entry.byteSize = kMaxSegmentByteSize;

    // A negative or absurdly large duration_ms (found via a project-wide
    // review, not reproduced live -- timeshift_buffer's own plugin.py is
    // the only realistic producer, and its own #EXTINF-derived
    // computation has no bound of its own either) would otherwise make
    // the caller's cumulative totalDurationMs non-monotonic, or, at the
    // extreme, contribute toward overflowing it -- clamp rather than
    // drop the whole entry, since its filename/byteSize can still be
    // perfectly valid.
    if (entry.durationMs < 0)
      entry.durationMs = 0;
    else if (entry.durationMs > kMaxSegmentDurationMs)
      entry.durationMs = kMaxSegmentDurationMs;

    entry.sequence = sequence;
    out.push_back(std::move(entry));
  }
  return out;
}

int64_t OldestLiveManifestSequence(const nlohmann::json& segmentsArray)
{
  int64_t oldest = -1;
  for (const nlohmann::json& seg : segmentsArray)
  {
    const int64_t sequence = FieldOr<int64_t>(seg, "sequence", -1);
    if (sequence >= 0 && (oldest < 0 || sequence < oldest))
      oldest = sequence;
  }
  return oldest;
}

} // namespace dispatcharr
