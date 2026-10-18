#pragma once

#include "DispatcharrClient.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dispatcharr
{

// Whether asking the recording_edl plugin for a recording's markers can return anything, from what comskip recorded
// about it (`custom_properties.comskip`, see ComskipInfo). Zero Kodi/curl dependency, tested in
// ../tests/test_edl_decision.cpp.
//
// "Ask" is the answer whenever there is any doubt -- an absent key (comskip not run yet, off, or an older server), an
// error, a status this code does not know -- so a recording that does have markers is never denied them. "Don't ask" is
// only for results that leave no EDL file to read, read from comskip_process_recording (apps/channels/tasks.py) and
// the plugin, which decides by the file rather than by this metadata:
//   - status "skipped": comskip is not installed on the server;
//   - completed with `skipped: true`: comskip found no commercials (an EDL under half a second long counts as none);
//   - completed in cut mode (`segments_kept`): the commercials were cut out of the file and the EDL file removed.
// Mark mode (`mode: "mark"`, an `edl` name and a commercial count) is the case with markers, and asks.
inline bool ShouldAskPluginForEdl(const ComskipInfo& comskip)
{
  if (!comskip.present)
    return true;
  if (comskip.status == "skipped")
    return false;
  if (comskip.status == "completed" && (comskip.noCommercials || comskip.cutMode))
    return false;
  return true;
}

// Kodi's PVR_EDL_TYPE_CUT and PVR_EDL_TYPE_COMBREAK, the two kinds it skips over (mute and scene markers do not).
constexpr int kEdlTypeCut = 0;
constexpr int kEdlTypeCommercialBreak = 3;

// A skip covering at least this share of the recording leaves nothing to watch.
constexpr int kWholeRecordingBreakPercent = 90;

// Drops the skips that cover (nearly) the whole recording, keeping every other entry and the order.
//
// Found live (2026-10-10): comskip on a 35-second recording marked all of it as one commercial break, Kodi started
// playback at "the end of the first commercial break", sought to the last second and reached the end of the file in
// two seconds; the recording read as watched. A skip that leaves nothing is never what anyone wants from a marker, and
// ignoring it costs nothing: the recording simply plays.
//
// `durationSeconds` is the recording's own length as the addon reports it (an early-stopped recording uses its stop
// time, see ParseRecordingFields()); 0 or less means unknown (still recording), and nothing is dropped. An entry that
// runs past the end, as comskip's does by a second or two, is clamped to the duration before the share is taken.
//
// Both ends are clamped into [0, duration] before anything is subtracted or multiplied: an entry far outside the
// recording (the plugin passes whatever the .edl file holds, and ParseRecordingEdlEntryJson() only requires end >
// start) made `covered` a huge negative number, and `covered * 100` a signed overflow (found by the 2026-10-18
// read-only audit, confirmed under UBSan). Clamped, `covered` is at most the duration, which is at most INT_MAX
// seconds in milliseconds, so the products cannot overflow.
inline std::vector<RecordingEdlEntry> WithoutBreaksCoveringRecording(std::vector<RecordingEdlEntry> entries,
                                                                     int durationSeconds)
{
  if (durationSeconds <= 0)
    return entries;
  const int64_t durationMs = static_cast<int64_t>(durationSeconds) * 1000;
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const RecordingEdlEntry& e)
                               {
                                 if (e.type != kEdlTypeCut && e.type != kEdlTypeCommercialBreak)
                                   return false;
                                 const int64_t start = std::clamp<int64_t>(e.startMs, 0, durationMs);
                                 const int64_t end = std::clamp<int64_t>(e.endMs, 0, durationMs);
                                 const int64_t covered = end - start;
                                 return covered > 0 && covered * 100 >= durationMs * kWholeRecordingBreakPercent;
                               }),
                entries.end());
  return entries;
}

} // namespace dispatcharr
