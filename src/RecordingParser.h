#pragma once

#include "DispatcharrClient.h"

#include <nlohmann/json_fwd.hpp>

#include <ctime>

namespace dispatcharr
{

// Pure field-mapping core of DispatcharrClient::ParseRecordingJson -- maps
// a single /api/channels/recordings/ item onto a Recording, including its
// documented time-window/custom_properties.status-override isInProgress
// logic (see Recording::isInProgress's own comment and docs/RECORDINGS.md
// for the real incident that made the status override necessary: a
// recording stopped early keeps its originally-scheduled end_time, so the
// time-window check alone kept reporting it in-progress for the rest of
// that window), hlsDirStillPresent, bytesWritten, and the
// custom_properties.program-then-flat title/subtitle/description fallback
// chain.
//
// Deliberately does NOT apply DispatcharrClient's own PendingTitle-cache
// fallback (member state behind DispatcharrClient::m_pendingTitlesMutex,
// not available to a free function) or the final "Recording <id>" default
// title -- both stay in ParseRecordingJson() itself, which calls this
// first and fills in whichever of those still applies to the result
// afterward. `now` is a parameter (rather than a direct time(nullptr)
// call) specifically so this is unit-testable standalone; see
// ../tests/test_recording_parser.cpp.
Recording ParseRecordingFields(const nlohmann::json& item, time_t now);

} // namespace dispatcharr
