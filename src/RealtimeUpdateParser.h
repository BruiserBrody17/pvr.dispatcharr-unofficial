#pragma once

#include <string>

namespace dispatcharr
{

// Pure classification core of PVRDispatcharr::HandleRealtimeUpdateMessage.
//
// Wire shape, confirmed by reading Dispatcharr's own consumers.py/utils.py:
// send_websocket_update('updates', 'update', {..., "type": "<event>", ...})
// is delivered to this socket as {"type": "update", "data": {..., "type":
// "<event>", ...}}. Only the recording/timer-relevant event names
// Dispatcharr actually sends (confirmed in apps/channels/tasks.py and
// api_views.py) are considered relevant here -- everything else on this
// shared "updates" channel (EPG matching progress, M3U refresh, stream
// stats, ...) is irrelevant and should be silently ignored, not treated
// as an error.
//
// Returns the event type string when `message` is well-formed JSON with a
// relevant data.type; returns an empty string for anything else (malformed
// JSON, a missing/non-string data.type, or a recognized-but-irrelevant
// event on the same channel). Takes a plain string rather than parsing
// inline in the caller specifically so this is unit-testable standalone
// with no Kodi SDK dependency; see ../tests/test_realtime_update_parser.cpp.
std::string ParseRelevantRealtimeUpdateEventType(const std::string& message);

} // namespace dispatcharr
