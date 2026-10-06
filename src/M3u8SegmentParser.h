#pragma once

#include <cstddef>
#include <cstdint>
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
// std::stoi), negative, or finite-but-huge #EXTINF duration escape into a
// returned entry's durationSec -- found via a project-wide UB review, not
// reproduced live: this value later feeds a static_cast<int64_t>()
// (durationSec * 1000) in the caller, and casting a non-finite double (or
// one whose scaled value is outside int64_t's range) to an integer is
// undefined behavior in C++ (unlike the equivalent Python-side nan/inf
// gaps this project's own companion plugins had, see docs/TIMESHIFT.md/
// docs/RECORDING_EDL.md -- including the same finite-but-overflows-once-
// scaled case _parse_edl already guards against). Non-finite or negative
// clamps to 0; anything over a generous 1-day-per-segment ceiling clamps
// to that ceiling instead of 0, since it's a real (if absurd) value, not
// an unparseable one.
//
// Zero Kodi/curl/member-state dependency -- pulled out here specifically
// so it's unit-testable standalone; see ../tests/test_m3u8_segment_parser.cpp.
std::vector<M3u8SegmentEntry> ParseNewM3u8SegmentEntries(const std::string& playlistText, const std::string& baseDir,
                                                         size_t alreadyKnownCount);

// Points an absolute segment URL at the server address this addon was configured
// with, instead of the one the playlist names.
//
// Dispatcharr builds each segment's URL from the request it served the playlist
// to. Behind a reverse proxy that forwards neither the port nor an
// `X-Forwarded-*` header -- a stock `proxy_set_header Host $host` -- it falls
// back to the port it listens on itself, so the playlist names an internal address
// the client cannot reach (confirmed against a real 0.31.0 instance with those
// headers simulated, docs/OPEN_ITEMS.md, "Dispatcharr 0.31.0 bug fixes ..."). The
// playlist itself is fetched from the configured address, so only the segments
// failed. `segmentUrl` is rebased onto `baseUrl` (`scheme://host:port`, no trailing
// slash) when it is absolute and its path begins with `requiredPathPrefix` -- the
// recording's own `/api/channels/recordings/<id>/hls/` -- and left alone otherwise,
// so nothing the playlist points elsewhere is rewritten, and a request carrying the
// API key can only ever be steered *to* the configured server, never away from it.
std::string RebaseRecordingSegmentUrl(const std::string& segmentUrl, const std::string& baseUrl,
                                      const std::string& requiredPathPrefix);

// Whether the playlist carries `#EXT-X-ENDLIST`, the tag that says no more
// segments will ever be appended. Dispatcharr's DVR capture runs ffmpeg with
// `-hls_flags append_list+omit_endlist`, so it is absent for as long as the
// recording is being written, and a dedicated `_dvr_ensure_hls_endlist()` step
// appends it once ffmpeg has really stopped -- which, after a user Stop, is
// about three seconds AFTER the recording's status has already flipped, and
// with the last segment added in the same write (see
// RefreshInProgressRecordingManifest(), docs/RECORDINGS.md). A line that is
// exactly the tag, not one that merely contains it.
bool M3u8HasEndList(const std::string& playlistText);

// Fix for a real, confirmed bug in RefreshInProgressRecordingManifest()'s
// own merge loop: `alreadyKnownCount` above (the skip count the *next*
// refresh passes back in) used to be taken from
// `m_inProgressRecordingStream.segments.size()`, but a segment whose
// size-probe failed was skipped -- never appended to `segments` -- so
// that count silently fell behind how many playlist entries had
// actually been read. The following refresh then re-parsed starting
// one entry too early, re-appending an already-merged segment as if it
// were new (a duplicate ~4s chunk of MPEG-TS spliced into the stream,
// a real PTS-jump-backwards/"Packet corrupt"-class symptom) while the
// segment that failed to probe was never retried and stayed a
// permanent gap.
//
// Cumulative byte/time offsets require complete, in-order knowledge of
// every earlier segment's own size (a later segment's own byteOffset
// depends on it), so a gap in the middle genuinely can't just be
// skipped over the way the original code assumed -- this returns how
// many *leading* entries of `probedSizes` (in the same order
// ParseNewM3u8SegmentEntries() returned them) can actually be merged
// this round, stopping at the first failed probe (a non-positive
// size). The caller should then use that same count -- not
// `segments.size()` -- as the *next* refresh's `alreadyKnownCount`, so
// a failed probe (and anything after it this round) is retried next
// time instead of silently skipped forever.
size_t CountLeadingProbedSegments(const std::vector<int64_t>& probedSizes);

} // namespace dispatcharr
