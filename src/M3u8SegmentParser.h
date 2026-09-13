#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dispatcharr
{

struct M3u8SegmentEntry
{
  std::string url;
  double durationSec = 0.0;
};

// Pure parsing core of DispatcharrClient::RefreshInProgressRecordingManifest's
// #EXTINF/segment-URI scan. Parses an in-progress-recording HLS playlist
// into an ordered list of newly-discovered segments only -- the first
// `alreadyKnownCount` segment lines are skipped (this addon's own
// append-only merge convention: no rolling-window eviction for a
// recording, so segments before the count already known about are always
// still the same ones, see RefreshInProgressRecordingManifest's own
// comment). A relative segment URI is resolved against `baseDir`; an
// absolute (`http(s)://`-prefixed) one is kept as-is.
//
// Never lets a non-finite (NaN/inf, both valid per std::stod though not
// std::stoi) #EXTINF duration escape into a returned entry's durationSec
// -- found via a project-wide UB review, not reproduced live: this value
// later feeds a static_cast<int64_t>() (durationSec * 1000) in the
// caller, and casting a non-finite double to an integer is undefined
// behavior in C++ (unlike the equivalent Python-side nan/inf gaps this
// project's own companion plugins had, see docs/TIMESHIFT.md/
// docs/RECORDING_EDL.md).
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_m3u8_segment_parser.cpp.
std::vector<M3u8SegmentEntry> ParseNewM3u8SegmentEntries(const std::string& playlistText, const std::string& baseDir,
                                                         size_t alreadyKnownCount);

} // namespace dispatcharr
