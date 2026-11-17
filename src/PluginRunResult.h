#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace dispatcharr
{

// Shared by every plugin run/ caller in DispatcharrClient.cpp
// (CallTimeshiftPluginAction(), StopTimeshiftBuffer(), GetRecordingEdl(),
// RefreshLiveManifest()): checks the outer {"success", "error"} envelope
// PluginRunAPIView always wraps a response in, then the plugin's own inner
// {"status", "message"} result -- see CallTimeshiftPluginAction()'s own
// comment for why both layers need checking. `pluginLabel` (e.g.
// "timeshift_buffer", "recording_edl") only feeds the two generic fallback
// error messages used when the response doesn't carry its own. `resultOut`
// is always set to the (possibly empty) inner result object on return,
// even on failure -- including the outer-failure envelope Dispatcharr 0.34
// sends with a result (IsPluginRunErrorEnvelope() below) -- so a caller
// needing a field from it either way (e.g. RefreshLiveManifest()'s
// "fatal") still can. The result is moved out of
// `response`, not copied: nlohmann's copy constructor recurses once per
// nesting level, which a deeply nested value turns into a stack overflow
// on Android's 1 MiB thread stacks (JsonFieldUtil.h, FieldRefOrNull()).
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_plugin_run_result.cpp.
bool UnwrapPluginRunResult(nlohmann::json&& response, const char* pluginLabel, nlohmann::json& resultOut,
                           std::string& error);

// Dispatcharr 0.34.0 runs a plugin action as a Celery task and turns a result whose "status" is "error" into an
// HTTP 500 carrying {"success": false, "error": ..., "result": <the plugin's own result>} (apps/plugins/tasks.py's
// PluginActionResultError, which exists so the result survives the trip through Celery; api_views.py's
// PluginRunAPIView). Before 0.34 the same result came back as a 200 inside {"success": true, "result": ...}. True for
// that 0.34 shape: the plugin itself answered, so its "retryable", "fatal" and not-ready status mean what they meant in
// the 200. The other non-2xx answers (403 disabled, 404 not installed, 504 stopped waiting, 500 from an exception)
// carry no result object. docs/CLOSED_ITEMS.md, "Dispatcharr 0.34 answers a plugin's own error with an HTTP 500".
bool IsPluginRunErrorEnvelope(const nlohmann::json& body);

} // namespace dispatcharr
