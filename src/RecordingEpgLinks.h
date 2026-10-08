#pragma once

#include <cstddef>
#include <ctime>
#include <map>
#include <string>
#include <unordered_set>

namespace dispatcharr
{

// Which EPG programme each recording was matched to, remembered across guide
// refreshes and Kodi restarts.
//
// A recording's Kodi-side EPG link (SetEPGUid()/SetEPGEventId(), what makes the
// guide show a recording indicator and offer "Play recording" instead of "Record")
// is the broadcast id of the matched programme -- ComputeBroadcastId(channelId,
// programme start time). Finding the programme means looking it up in this addon's
// cached guide, and that cache only ever holds current and upcoming programmes: an
// ended one drops out at the next XMLTV refresh (Dispatcharr's `/output/epg` export
// never includes it). So a link that was right while the programme was still in
// the guide silently vanished at the next refresh or restart (docs/CLOSED_ITEMS.md,
// "GetRecordings() EPG link doesn't survive EPG cache rotation").
//
// The matched programme's start time is all that is needed to recompute the id, so
// that is what is kept, once per recording id, the first time a match succeeds --
// usually while the recording is still a timer. Persisted as a small JSON file in
// the addon's own user directory.
struct RecordingEpgLink
{
  int channelId = 0;
  // The recording's own start time when the match was made. A mismatch later
  // means the recording was moved (or the id reused after a database reset), and
  // the remembered programme no longer describes it.
  time_t recordingStartTime = 0;
  // Start time of the EPG programme that was matched.
  time_t programStartTime = 0;
};

using RecordingEpgLinkMap = std::map<int, RecordingEpgLink>;

// A file this large is not one this addon wrote.
constexpr std::size_t kMaxRecordingEpgLinks = 20000;

std::string SerializeRecordingEpgLinks(const RecordingEpgLinkMap& links);

// Parses what SerializeRecordingEpgLinks() wrote. Tolerant by design -- the file
// is a cache that is rebuilt on its own, so anything unreadable (truncated by a
// crash mid-write, a future format, hand-edited) yields an empty map and false
// rather than an error the caller has to handle; individual malformed entries are
// skipped and the rest kept.
bool ParseRecordingEpgLinks(const std::string& text, RecordingEpgLinkMap& out);

// Records `link` for `recordingId`. True when that changed the map (a new entry or
// a different value), i.e. when it needs saving.
bool RememberRecordingEpgLink(RecordingEpgLinkMap& links, int recordingId, const RecordingEpgLink& link);

// The programme start time remembered for this recording, or 0 when nothing is
// remembered or what is no longer describes it (a different channel or recording
// start time).
time_t LookupRememberedProgramStart(const RecordingEpgLinkMap& links, int recordingId, int channelId,
                                    time_t recordingStartTime);

// Drops every entry whose recording is not in `liveRecordingIds`; returns how many.
std::size_t PruneRecordingEpgLinks(RecordingEpgLinkMap& links, const std::unordered_set<int>& liveRecordingIds);

} // namespace dispatcharr
