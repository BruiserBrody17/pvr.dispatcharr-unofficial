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
// it is safe, not just an optimization. Each kept entry must also be newer
// than the one kept before it: an entry that repeats or goes back (5, 6, 5)
// was appended a second time into the byte stream, which the order the
// plugin promises never produces but nothing checked (found by the
// 2026-10-18 read-only audit). A malformed entry (empty
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

// The lowest `sequence` the plugin's current manifest still lists, or -1 if
// it lists none with a usable one. get_live_manifest answers with the buffer's
// current visible window, so this moves forward as the rolling buffer rolls
// segments off -- which this addon's own append-only address space never
// otherwise learns about (docs/CLOSED_ITEMS.md, "Live-timeshift address space
// never drops rolled-off segments"). Unlike ParseNewLiveManifestSegments(),
// looks at every entry, including ones already merged.
int64_t OldestLiveManifestSequence(const nlohmann::json& segmentsArray);

// Whether `name`, the `manifest_route_name` a timeshift_buffer manifest names (0.8.19 and later), is safe to append to
// the segment base URL: 1 to 64 characters from [A-Za-z0-9._-], not starting with a dot. It reaches the URL unescaped,
// as a segment's filename does, so anything else (a slash, a query, "..") is refused and the addon keeps polling the
// get_live_manifest action.
bool IsValidManifestRouteName(const std::string& name);

// The manifest route a timeshift_buffer start_buffer answer names (0.8.19 and later): its `segment_route_prefix` and
// `manifest_route_name`, the same two fields the manifest answer carries. With them the cold start's polls go to the
// file server too, instead of the get_live_manifest action they used to need until the first segment closed. True
// when both are there and pass IsValidTimeshiftRoute()/IsValidManifestRouteName() (both reach a URL unescaped); false,
// with both outputs untouched, for an older plugin's answer or anything malformed, which leaves the cold start on the
// action until the first manifest names the route, as before.
bool ReadStartAnswerManifestRoute(const nlohmann::json& result, std::string& routePrefixOut, std::string& routeNameOut);

// Turns the file server's answer to a manifest-route GET (plugin.py's _MANIFEST_ROUTE_NAME) into the {"success": true,
// "result": ...} envelope Dispatcharr wraps the get_live_manifest action's answer in, so both go through
// UnwrapPluginRunResult() and the same merge. False, with `envelopeOut` untouched, for anything but a 200 whose body is
// a JSON object with a string "status": an older plugin's 404, a 403 once the buffer is gone or replaced, a 503, a
// transport failure or a body that is not the plugin's. The caller then asks the action, whose answer tells those
// apart.
bool WrapManifestRouteAnswer(bool transferOk, long httpStatus, const std::string& body, nlohmann::json& envelopeOut);

} // namespace dispatcharr
