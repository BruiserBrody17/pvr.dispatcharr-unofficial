#pragma once

#include "DispatcharrClient.h"

#include <nlohmann/json_fwd.hpp>

#include <ctime>

namespace dispatcharr
{

// The whole seconds from `start` to `end`, 0 when `end` is not later, and at most INT_MAX. A recording
// row ending past about 2094 made `static_cast<int>(end - start)` wrap to a negative duration for Kodi
// (found by the 2026-10-04 fourth hardening sweep; a 64-bit time_t can hold the difference, an int
// cannot).
int ClampedDurationSeconds(time_t start, time_t end);

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

// Whether a single-recording GET answered with that recording: a JSON object whose "id" is
// `expectedId`. Request() turns an empty 2xx body into `{}`, and ParseRecordingFields({}) is
// id 0 with neither upcoming nor in progress -- which DecideDeleteTimerAction() reads as "already
// finished, nothing to do", so DeleteTimer() reported success without deleting a scheduled
// recording, UpdateTimer() reduced an edit to a rename and IsInProgressContentGone() read the
// stream as finished (found by the 2026-10-04 sixth hardening sweep; needs a misbehaving proxy or
// server). A lookup that did not return the recording is a failed lookup, which every caller
// already handles as "unknown".
bool IsRecordingResponseFor(const nlohmann::json& item, int expectedId);

// Pure field-mapping core of DispatcharrClient::GetRecordingEdl()'s
// per-entry loop -- maps a single recording_edl plugin entry onto a
// RecordingEdlEntry. Returns false (leaving `out` unmodified) for a
// malformed entry whose end isn't after its start, matching the
// original loop's own silent-skip behavior for that case.
bool ParseRecordingEdlEntryJson(const nlohmann::json& item, RecordingEdlEntry& out);

} // namespace dispatcharr
