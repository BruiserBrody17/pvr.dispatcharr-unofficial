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
// even on failure, so a caller needing a field from it either way (e.g.
// RefreshLiveManifest()'s "fatal") still can.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_plugin_run_result.cpp.
bool UnwrapPluginRunResult(const nlohmann::json& response, const char* pluginLabel, nlohmann::json& resultOut,
                           std::string& error);

} // namespace dispatcharr
