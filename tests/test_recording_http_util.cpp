#include "RecordingHttpUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("IsInProgressHlsRedirect detects the mpegurl content type", "[RecordingHttpUtil]")
{
  CHECK(IsInProgressHlsRedirect("application/x-mpegurl", "http://example/recordings/1/file/"));
}

TEST_CASE("IsInProgressHlsRedirect is case-insensitive on content type -- the real gap this fixes",
          "[RecordingHttpUtil]")
{
  // A real-world casing HTTP doesn't guarantee against: the original
  // inline check was case-sensitive and only caught this via the URL
  // fallback, not the content-type check itself.
  CHECK(IsInProgressHlsRedirect("application/x-mpegURL", "http://example/recordings/1/file/"));
  CHECK(IsInProgressHlsRedirect("APPLICATION/VND.APPLE.MPEGURL", "http://example/recordings/1/file/"));
}

TEST_CASE("IsInProgressHlsRedirect detects the /hls/ URL segment even with an unrelated content type",
          "[RecordingHttpUtil]")
{
  CHECK(IsInProgressHlsRedirect("text/plain", "http://example/recordings/1/hls/index.m3u8"));
}

TEST_CASE("IsInProgressHlsRedirect is false for a completed recording's plain file response", "[RecordingHttpUtil]")
{
  CHECK_FALSE(IsInProgressHlsRedirect("video/mp2t", "http://example/recordings/1/file/recording.ts"));
}

TEST_CASE("IsInProgressHlsRedirect is false for empty content type and URL", "[RecordingHttpUtil]")
{
  CHECK_FALSE(IsInProgressHlsRedirect("", ""));
}

// ---------------------------------------------------------------------
// HasRecordingFileChanged
// ---------------------------------------------------------------------

TEST_CASE("HasRecordingFileChanged is false when the sizes agree", "[RecordingHttpUtil]")
{
  CHECK_FALSE(HasRecordingFileChanged(/*cachedLength=*/1000, /*serverReportedTotal=*/1000));
}

TEST_CASE("HasRecordingFileChanged is true when the file got shorter -- the real bug this fixes", "[RecordingHttpUtil]")
{
  // Dispatcharr's own comskip "cut" mode replaces a just-finished
  // recording's file with a shorter, commercial-trimmed one -- possibly
  // while a client is already reading the original at these exact
  // offsets.
  CHECK(HasRecordingFileChanged(/*cachedLength=*/1000, /*serverReportedTotal=*/700));
}

TEST_CASE("HasRecordingFileChanged is true when the file got longer too", "[RecordingHttpUtil]")
{
  CHECK(HasRecordingFileChanged(/*cachedLength=*/1000, /*serverReportedTotal=*/1500));
}

TEST_CASE("HasRecordingFileChanged is false when the cached length was never known (< 0)", "[RecordingHttpUtil]")
{
  CHECK_FALSE(HasRecordingFileChanged(/*cachedLength=*/-1, /*serverReportedTotal=*/700));
}

TEST_CASE("HasRecordingFileChanged is false when this read's header callback never found a Content-Range line",
          "[RecordingHttpUtil]")
{
  CHECK_FALSE(HasRecordingFileChanged(/*cachedLength=*/1000, /*serverReportedTotal=*/-1));
}

// ---------------------------------------------------------------------
// ServerIgnoredRangeRequest
// ---------------------------------------------------------------------

TEST_CASE("ServerIgnoredRangeRequest flags a plain 200 to a ranged read past offset 0", "[RecordingHttpUtil]")
{
  CHECK(ServerIgnoredRangeRequest(1, 200));
  CHECK(ServerIgnoredRangeRequest(4096, 200));
  CHECK(ServerIgnoredRangeRequest(int64_t{1} << 40, 200));
}

TEST_CASE("ServerIgnoredRangeRequest accepts a 206 at any offset, and a 200 at offset 0", "[RecordingHttpUtil]")
{
  CHECK_FALSE(ServerIgnoredRangeRequest(0, 200)); // the bytes really do start at the reader's position
  CHECK_FALSE(ServerIgnoredRangeRequest(0, 206));
  CHECK_FALSE(ServerIgnoredRangeRequest(4096, 206));
}

TEST_CASE("ServerIgnoredRangeRequest leaves every other status to the callers' own handling", "[RecordingHttpUtil]")
{
  for (long code : {0L, 301L, 401L, 404L, 416L, 500L})
    CHECK_FALSE(ServerIgnoredRangeRequest(4096, code));
}

TEST_CASE("HasRecordingFileChanged treats a known zero length as a known length", "[RecordingHttpUtil]")
{
  // A cached length of 0 is a real length (an empty file at open time), not the unknown sentinel -1.
  CHECK(HasRecordingFileChanged(0, 100));
  CHECK_FALSE(HasRecordingFileChanged(0, 0));
  CHECK_FALSE(HasRecordingFileChanged(-1, 100));
}

TEST_CASE("DescribeRecordingOpenFailure explains a recording whose file is missing", "[RecordingHttpUtil]")
{
  const std::string notFound = DescribeRecordingOpenFailure(404);
  CHECK_FALSE(notFound.empty());
  CHECK(notFound.find("no file") != std::string::npos);
  CHECK_FALSE(DescribeRecordingOpenFailure(403).empty());
  CHECK(DescribeRecordingOpenFailure(403) != notFound);
}

TEST_CASE("DescribeRecordingOpenFailure leaves every other status to Kodi's own dialog", "[RecordingHttpUtil]")
{
  // 401 is recovered from before anything is reported; the rest have no better explanation than the generic one.
  for (long code : {0L, 200L, 206L, 301L, 400L, 401L, 405L, 416L, 500L, 502L, 503L})
    CHECK(DescribeRecordingOpenFailure(code).empty());
}

TEST_CASE("ClassifyRecordingReadResponse answers a first 401 with a refreshed key, once", "[RecordingHttpUtil]")
{
  CHECK(ClassifyRecordingReadResponse(true, 401, false, 0, 100, -1) == RecordingReadOutcome::kRefreshApiKeyAndRetry);
  // Already refreshed: the 401 is just a refusal.
  CHECK(ClassifyRecordingReadResponse(true, 401, true, 0, 100, -1) == RecordingReadOutcome::kFailPermanently);
}

TEST_CASE("ClassifyRecordingReadResponse retries what may clear and fails the rest", "[RecordingHttpUtil]")
{
  // No response at all (the transport failed), whatever the status field holds.
  CHECK(ClassifyRecordingReadResponse(false, 0, false, 0, 100, -1) == RecordingReadOutcome::kRetryTransient);
  CHECK(ClassifyRecordingReadResponse(false, 200, true, 0, 100, -1) == RecordingReadOutcome::kRetryTransient);
  for (long status : {500L, 502L, 503L, 504L})
    CHECK(ClassifyRecordingReadResponse(true, status, false, 0, 100, -1) == RecordingReadOutcome::kRetryTransient);
  for (long status : {301L, 400L, 403L, 404L, 416L, 501L, 505L})
    CHECK(ClassifyRecordingReadResponse(true, status, false, 0, 100, -1) == RecordingReadOutcome::kFailPermanently);
}

TEST_CASE("ClassifyRecordingReadResponse checks the status before the range and the range before the file size",
          "[RecordingHttpUtil]")
{
  // A plain 200 past offset 0 is a server that dropped Range; at offset 0 it is fine.
  CHECK(ClassifyRecordingReadResponse(true, 200, false, 1, 100, -1) == RecordingReadOutcome::kRangeIgnored);
  CHECK(ClassifyRecordingReadResponse(true, 200, false, 0, 100, -1) == RecordingReadOutcome::kOk);
  CHECK(ClassifyRecordingReadResponse(true, 206, false, 50, 100, 100) == RecordingReadOutcome::kOk);
  // A file whose size no longer matches the cached length was replaced under the stream.
  CHECK(ClassifyRecordingReadResponse(true, 206, false, 50, 100, 99) == RecordingReadOutcome::kFileChanged);
  CHECK(ClassifyRecordingReadResponse(true, 206, false, 50, -1, 99) == RecordingReadOutcome::kOk);
  CHECK(ClassifyRecordingReadResponse(true, 206, false, 50, 100, -1) == RecordingReadOutcome::kOk);
  // Both at once: the range failure wins (the bytes are the wrong ones whatever the size says).
  CHECK(ClassifyRecordingReadResponse(true, 200, false, 50, 100, 99) == RecordingReadOutcome::kRangeIgnored);
  // A bad status never reaches the later checks.
  CHECK(ClassifyRecordingReadResponse(true, 403, false, 50, 100, 99) == RecordingReadOutcome::kFailPermanently);
}

TEST_CASE("ClassifyLiveSegmentResponse puts a transport failure first, then a recycled segment", "[RecordingHttpUtil]")
{
  CHECK(ClassifyLiveSegmentResponse(false, 0, 0, -1, 1000) == LiveSegmentOutcome::kTransportFailure);
  CHECK(ClassifyLiveSegmentResponse(false, 404, 0, -1, 1000) == LiveSegmentOutcome::kTransportFailure);
  // A 404 is the rolling buffer recycling a segment, not a failure to count.
  CHECK(ClassifyLiveSegmentResponse(true, 404, 0, -1, 1000) == LiveSegmentOutcome::kSegmentGone);
  CHECK(ClassifyLiveSegmentResponse(true, 404, 500, -1, 1000) == LiveSegmentOutcome::kSegmentGone);
}

TEST_CASE("ClassifyLiveSegmentResponse tells a dropped Range, a bad status and a changed size apart",
          "[RecordingHttpUtil]")
{
  CHECK(ClassifyLiveSegmentResponse(true, 200, 100, -1, 1000) == LiveSegmentOutcome::kRangeIgnored);
  CHECK(ClassifyLiveSegmentResponse(true, 200, 0, -1, 1000) == LiveSegmentOutcome::kOk);
  CHECK(ClassifyLiveSegmentResponse(true, 206, 100, 1000, 1000) == LiveSegmentOutcome::kOk);
  for (long status : {301L, 401L, 403L, 416L, 500L, 503L})
    CHECK(ClassifyLiveSegmentResponse(true, status, 0, -1, 1000) == LiveSegmentOutcome::kUnexpectedStatus);
  CHECK(ClassifyLiveSegmentResponse(true, 206, 100, 999, 1000) == LiveSegmentOutcome::kSizeMismatch);
  CHECK(ClassifyLiveSegmentResponse(true, 206, 100, 1001, 1000) == LiveSegmentOutcome::kSizeMismatch);
  CHECK(ClassifyLiveSegmentResponse(true, 206, 100, -1, 1000) == LiveSegmentOutcome::kOk);
  CHECK(ClassifyLiveSegmentResponse(true, 206, 100, 0, 1000) == LiveSegmentOutcome::kSizeMismatch);
  // The range check comes before the status check (a 200 at an offset is never "just an unexpected status"), and the
  // status check before the size check.
  CHECK(ClassifyLiveSegmentResponse(true, 200, 100, 999, 1000) == LiveSegmentOutcome::kRangeIgnored);
  CHECK(ClassifyLiveSegmentResponse(true, 403, 100, 999, 1000) == LiveSegmentOutcome::kUnexpectedStatus);
}

TEST_CASE("HasRecordingFileChanged treats a file swapped for an empty one as a change", "[RecordingHttpUtil]")
{
  // The Content-Range total of an empty file is 0, which is a real answer (>= 0), not "not reported".
  CHECK(HasRecordingFileChanged(100, 0));
  CHECK_FALSE(HasRecordingFileChanged(0, 0));
  CHECK(HasRecordingFileChanged(0, 100));
  CHECK_FALSE(HasRecordingFileChanged(100, -1));
  CHECK_FALSE(HasRecordingFileChanged(-1, 100));
}

TEST_CASE("ClassifyRecordingReadResponse looks at a 401 before whether the transfer worked", "[RecordingHttpUtil]")
{
  // The order the code had before it was extracted: a 401 first (a key to refresh), only then a failed transfer.
  CHECK(ClassifyRecordingReadResponse(false, 401, false, 0, 100, -1) == RecordingReadOutcome::kRefreshApiKeyAndRetry);
  CHECK(ClassifyRecordingReadResponse(false, 401, true, 0, 100, -1) == RecordingReadOutcome::kRetryTransient);
}
