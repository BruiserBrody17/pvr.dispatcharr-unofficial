#pragma once

#include "SegmentFetchFailure.h"

#include <cstdint>
#include <string>

namespace dispatcharr
{

// The pure classification core of DispatcharrClient::OpenRecordingStream()'s
// own "is this actually an in-progress recording's HLS redirect, not a
// completed file" check -- confirmed against a live instance: an
// in-progress recording's /file/ redirects to an HLS playlist
// (.../hls/index.m3u8), which the flat-byte-range completed-recording
// reader doesn't understand; treating it as a flat byte range would
// just hand the demuxer m3u8 text instead of video.
//
// Matches contentType case-insensitively (a real, if minor, gap found
// via a project-wide review: the original inline check was
// case-sensitive, so a server reporting e.g. "application/x-mpegURL" --
// a real-world casing HTTP doesn't guarantee against -- would only have
// been caught by the resolvedUrl fallback below, not this check on its
// own). resolvedUrl's own "/hls/" match stays case-sensitive, matching
// Dispatcharr's own consistent lowercase URL path.
bool IsInProgressHlsRedirect(const std::string& contentType, const std::string& resolvedUrl);

// Whether a completed recording's own file appears to have been swapped
// out from under an already-open ReadRecordingStream() -- confirmed
// against Dispatcharr's own real current upstream source (a 22nd-pass
// audit, cloned into a scratchpad, never committed to this repo --
// stronger than the API shape alone, not the same standard as a live
// test): `comskip_process_recording()` runs automatically right after a
// recording finishes (gated on the comskip-enabled setting), and its own
// "cut" mode `os.replace()`s (or, on that failing, a non-atomic
// `shutil.copy()` over) the original file with a shorter,
// commercial-trimmed one -- reachable while a client is already
// mid-playback if it started watching the recording right after it
// finished, before comskip (which can take minutes) is done.
//
// Without this check, `ReadRecordingStream()` kept using the length
// cached at open time against the new, shorter file's actual bytes at
// the same offsets -- silently reading the wrong content with no error
// anywhere (the live-timeshift read path already guards its own
// analogous "did the underlying file change size" case the same way;
// this mirrors that).
//
// `cachedLength < 0` (unknown length at open time) never reports a
// change -- there's nothing to compare a fresh read's own reported total
// against. Matches ReadLiveTimeshiftStream()'s own `serverReportedTotal
// >= 0` guard against a header callback that never actually found a
// Content-Range line.
inline bool HasRecordingFileChanged(int64_t cachedLength, int64_t serverReportedTotal)
{
  return cachedLength >= 0 && serverReportedTotal >= 0 && serverReportedTotal != cachedLength;
}

// Whether a response to a ranged GET means the server ignored the Range header
// and is sending the resource from its first byte: a plain 200 for a request that
// started past offset 0 (a 206 is the only correct answer there). Both byte-range
// readers advance their position by however many bytes arrive, so accepting that
// response would silently splice the wrong bytes into playback, from the start
// of the file where the requested offset was meant. A 200 at offset 0 is fine --
// the bytes really do start where the reader thinks they do.
//
// Neither Dispatcharr's own file endpoint nor the timeshift plugin's file server
// has been seen to do this (both answer 206 with a correct Content-Range, checked
// live), so what reaches this is a proxy in front of one that drops Range -- which
// is why the callers stop with a clear message rather than retry, and do not try to
// skip ahead to the offset (that would download the whole preceding file on every read).
inline bool ServerIgnoredRangeRequest(int64_t requestedOffset, long httpCode)
{
  return httpCode == 200 && requestedOffset > 0;
}

// What to tell the viewer when opening a recording's file fails with `httpCode`, or an empty string for a status that
// has no better explanation than Kodi's own generic "playback failed". A 404 is the case that mattered: Dispatcharr can
// hold a recording row in a terminal state ("stopped", "interrupted") with no file at all -- one stopped long after
// it was started without ever producing anything -- and its /file/ endpoint then answers 404 "Recording file not
// found" for ever. The row is still listed (hiding it would also hide it from Delete), so opening it used to end
// in the generic dialog with the 404 only in the log (docs/OPEN_ITEMS.md, found 2026-10-03). 401 is not here: the
// caller recovers from it before reporting anything.
inline std::string DescribeRecordingOpenFailure(long httpCode)
{
  switch (httpCode)
  {
  case 404:
    return "Dispatcharr has no file for this recording. It may have been stopped before anything was recorded, or "
           "its file was removed.";
  case 403:
    return "Dispatcharr refused access to this recording.";
  default:
    return std::string();
  }
}

// What a ranged read of a completed recording's file turned out to be, in the order the checks must run
// (ReadRecordingStream()). The order is the content: a 401 is tried once with a refreshed key before anything else; a
// failure that may clear (no response, 502/503/504, 500) is retried inside the read; any other status ends the read; a
// plain 200 past offset 0 means the server dropped Range; a Content-Range total that no longer matches the length
// cached at open means the file was replaced. A mutation that moves any of these past another (the range check before
// the status check, say) changes what a viewer is told.
enum class RecordingReadOutcome
{
  kRefreshApiKeyAndRetry, // a 401 on the first try
  kRetryTransient,
  kFailPermanently,
  kRangeIgnored,
  kFileChanged,
  kOk,
};

inline RecordingReadOutcome ClassifyRecordingReadResponse(bool transferOk, long httpCode, bool apiKeyAlreadyRefreshed,
                                                          int64_t position, int64_t cachedLength,
                                                          int64_t serverReportedTotal)
{
  if (httpCode == 401 && !apiKeyAlreadyRefreshed)
    return RecordingReadOutcome::kRefreshApiKeyAndRetry;
  if (IsTransientReadFailure(transferOk, httpCode))
    return RecordingReadOutcome::kRetryTransient;
  if (httpCode != 200 && httpCode != 206)
    return RecordingReadOutcome::kFailPermanently;
  if (ServerIgnoredRangeRequest(position, httpCode))
    return RecordingReadOutcome::kRangeIgnored;
  if (HasRecordingFileChanged(cachedLength, serverReportedTotal))
    return RecordingReadOutcome::kFileChanged;
  return RecordingReadOutcome::kOk;
}

// The same for one segment body of the live timeshift buffer (ReadLiveTimeshiftStreamOnce()): a transport failure and
// an unexpected status both go to the bounded retry handler; a 404 is a segment the rolling buffer recycled (not an
// error, and it resets that handler's counters); a plain 200 past the segment's start is a proxy that dropped Range,
// and a Content-Range total that differs from the manifest's size is a misaligned address space: both end the stream.
enum class LiveSegmentOutcome
{
  kTransportFailure,
  kSegmentGone,
  kRangeIgnored,
  kUnexpectedStatus,
  kSizeMismatch,
  kOk,
};

inline LiveSegmentOutcome ClassifyLiveSegmentResponse(bool transferOk, long httpCode, int64_t offsetInSegment,
                                                      int64_t serverReportedTotal, int64_t segmentByteSize)
{
  if (!transferOk)
    return LiveSegmentOutcome::kTransportFailure;
  if (httpCode == 404)
    return LiveSegmentOutcome::kSegmentGone;
  if (ServerIgnoredRangeRequest(offsetInSegment, httpCode))
    return LiveSegmentOutcome::kRangeIgnored;
  if (httpCode != 200 && httpCode != 206)
    return LiveSegmentOutcome::kUnexpectedStatus;
  if (serverReportedTotal >= 0 && serverReportedTotal != segmentByteSize)
    return LiveSegmentOutcome::kSizeMismatch;
  return LiveSegmentOutcome::kOk;
}

// What the 0-0 probe that opens a completed recording (OpenRecordingStream()) turned out to be, in the order the checks
// must run. A probe the server cut short for declaring a body bigger than it asked for (libcurl's size limit) is still
// a response with a status and headers, so it counts as a transfer that worked; any other failed transfer ends the open
// before a status is looked at. A first 401 is retried once with the account's current key, whatever the headers say;
// the recovered key is the caller's business, and a refresh that fails is classified again as already refreshed (a
// plain refusal). Any non-2xx status ends the open (with DescribeRecordingOpenFailure()'s message where there is one),
// and only a 2xx is then asked whether it is an in-progress recording's HLS redirect, which this reader cannot play. A
// mutation that checks the redirect before the status, or the size limit after the transfer, changes what a viewer is
// told.
enum class RecordingOpenOutcome
{
  kTransferFailed,
  kRefreshApiKeyAndRetry,
  kHttpFailure,
  kInProgressRecording,
  kOk,
};

inline RecordingOpenOutcome ClassifyRecordingOpenResponse(bool transferOk, bool sizeLimitExceeded, long httpCode,
                                                          bool apiKeyAlreadyRefreshed, const std::string& contentType,
                                                          const std::string& resolvedUrl)
{
  if (!transferOk && !sizeLimitExceeded)
    return RecordingOpenOutcome::kTransferFailed;
  if (httpCode == 401 && !apiKeyAlreadyRefreshed)
    return RecordingOpenOutcome::kRefreshApiKeyAndRetry;
  if (httpCode < 200 || httpCode >= 300)
    return RecordingOpenOutcome::kHttpFailure;
  if (IsInProgressHlsRedirect(contentType, resolvedUrl))
    return RecordingOpenOutcome::kInProgressRecording;
  return RecordingOpenOutcome::kOk;
}

// The same for one segment body of an in-progress recording (ReadInProgressRecordingStream()): a first 401 is retried
// with a refreshed key; a failure that may clear (no response, 500/502/503/504) is retried inside the read's budget; a
// 404 is a segment the playlist listed but the server no longer has, which the caller checks against the recording
// itself (it may be gone) before treating it as one more failure that will not clear; any other status is such a
// failure; only a 200 is a segment (the fetch asks for the whole body, never a range). The 401 comes first even when
// the transfer failed.
enum class InProgressSegmentOutcome
{
  kRefreshApiKeyAndRetry,
  kRetryTransient,
  kSegmentNotFound,
  kFailPermanently,
  kOk,
};

inline InProgressSegmentOutcome ClassifyInProgressSegmentResponse(bool transferOk, long httpCode,
                                                                  bool apiKeyAlreadyRefreshed)
{
  if (httpCode == 401 && !apiKeyAlreadyRefreshed)
    return InProgressSegmentOutcome::kRefreshApiKeyAndRetry;
  if (IsTransientReadFailure(transferOk, httpCode))
    return InProgressSegmentOutcome::kRetryTransient;
  if (httpCode == 404)
    return InProgressSegmentOutcome::kSegmentNotFound;
  if (httpCode != 200)
    return InProgressSegmentOutcome::kFailPermanently;
  return InProgressSegmentOutcome::kOk;
}

} // namespace dispatcharr
