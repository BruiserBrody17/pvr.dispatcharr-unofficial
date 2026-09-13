#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace dispatcharr
{

struct LiveManifestSegmentEntry
{
  int64_t sequence = -1;
  std::string filename;
  int64_t byteSize = 0;
  int64_t durationMs = 0;
};

// Pure filtering core of DispatcharrClient::RefreshLiveManifest()'s
// segment-merge loop -- takes the plugin's raw "segments" JSON array
// (get_live_manifest's own response) and `lastKnownSequence` (the
// highest sequence number already merged, or -1 if none yet), and
// returns only the newly-discovered, well-formed entries in order.
//
// Segments come back ordered by sequence; only ones newer than
// `lastKnownSequence` are kept -- an already-known segment's size can't
// legitimately change (ffmpeg only ever appends new, closed segments to
// the playlist), so silently skipping it here rather than re-verifying
// it is safe, not just an optimization. A malformed entry (empty
// filename or non-positive byte_size) is dropped rather than returned,
// so it can't corrupt the caller's own cumulative byte/time offsets.
//
// Deliberately does NOT compute byteOffset/timeOffsetMs itself -- those
// are cumulative over the caller's own running totals (member state),
// not available to a free function; the caller assigns them while
// applying each returned entry in order. Zero Kodi/curl/member-state
// dependency otherwise -- pulled out here specifically so it's
// unit-testable standalone; see ../tests/test_live_manifest_parser.cpp.
std::vector<LiveManifestSegmentEntry> ParseNewLiveManifestSegments(const nlohmann::json& segmentsArray,
                                                                   int64_t lastKnownSequence);

} // namespace dispatcharr
