#pragma once

#include <cstdint>

namespace dispatcharr
{

// Resolves a Kodi PVR seek request (position/whence, matching POSIX
// SEEK_SET/SEEK_CUR/SEEK_END semantics) to an absolute byte position --
// pulled out as a free function since the exact same switch/validation was
// written three times: DispatcharrClient::SeekRecordingStream(),
// SeekInProgressRecordingStream(), and SeekLiveTimeshiftStream(). See
// ../tests/test_stream_seek.cpp.
//
// currentPosition is the stream's own current byte position (used for
// SEEK_CUR); referenceLength is what SEEK_END is relative to (a
// recording's known total length, or a live/in-progress stream's current
// totalBytes) -- pass a negative value if that length genuinely isn't
// known yet (matching SeekRecordingStream's own pre-existing "can't seek
// from an unknown end" check).
//
// Returns -1 -- matching every caller's own existing "seek failed"
// convention, since a valid byte position is never negative -- for an
// unrecognized whence value, a SEEK_END request against an unknown
// referenceLength, or a computed position that would be negative.
// Does NOT apply the separate live-edge-tail clamp
// SeekInProgressRecordingStream()/SeekLiveTimeshiftStream() layer on top
// of this for a forward seek -- that's dispatcharr::ComputeLiveEdgeTailTarget()
// in LiveEdgeMargin.h, a distinct concern (how far ahead of the buffer's
// current tail a seek may land) from whence/position arithmetic itself.
//
// Also returns -1 when the sum itself would overflow int64_t (added
// 2026-09-27, a 67th-pass audit, fixing a real UB gap found via a
// project-wide review, confirmed with UBSan, not reproduced live) -- a
// signed overflow is undefined behavior, so the old "computed newPos < 0"
// check alone couldn't be relied on to catch it. Defensive in practice
// for this addon's own playback: ffmpeg's own avio_seek() (libavformat/
// aviobuf.c) already resolves a relative SEEK_CUR itself, with its own
// overflow guard, into the absolute SEEK_SET it hands Kodi's
// dvd_file_seek() (DVDDemuxFFmpeg.cpp) -- its only other SEEK_CUR is a
// zero-offset position query -- but Kodi passes whence through to this
// addon unchanged, so nothing guarantees every caller resolves it first.
int64_t ResolveSeekPosition(int64_t position, int whence, int64_t currentPosition, int64_t referenceLength);

// The inclusive end byte of a `size`-byte read starting at `position` --
// ReadRecordingStream()'s own HTTP Range end -- or false when that can't
// be represented (a negative position, a zero size, or position + size
// overflowing int64_t). Fix for a real UB gap (added 2026-09-27, a
// 67th-pass audit, found via a project-wide review, confirmed with UBSan,
// not reproduced live): ReadRecordingStream() computed
// `position + size - 1` inline, and a SEEK_SET is accepted at any
// non-negative position (ResolveSeekPosition() above, which has no
// length to clamp against when the recording's own length isn't known --
// OpenRecordingStream() leaves it at -1 when its 0-0 probe comes back
// with no parseable Content-Range, e.g. a proxy that ignores Range) --
// so a SEEK_SET within one read's size of INT64_MAX (ffmpeg's own
// SEEK_SET offsets can come straight from a container's own byte-offset
// fields, e.g. a corrupt index) made the very next read's arithmetic
// signed overflow. Also guarantees `position + size` itself fits, so the
// caller's own `position += bytesRead` afterward can't overflow either.
// See ../tests/test_stream_seek.cpp.
bool ComputeReadRangeEnd(int64_t position, unsigned int size, int64_t& rangeEndOut);

// How many bytes one read may take from a segment of `segmentSize` bytes when the stream is `offsetInSegment` bytes
// into it and the caller asked for `requested`: the smaller of the request and what is left of the segment. False,
// writing nothing, when there is nothing to give: a negative offset, an offset at or past the segment's end (the
// available count would be zero or negative, and a negative one cast to unsigned is an enormous read) or a zero
// request. The in-progress recording's cached copy and the live timeshift's ranged fetch both size their read this way.
// See ../tests/test_stream_seek.cpp.
bool ComputeSegmentReadSize(int64_t offsetInSegment, int64_t segmentSize, unsigned int requested,
                            unsigned int& sizeOut);

} // namespace dispatcharr
