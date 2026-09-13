#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dispatcharr
{

// Writes into a fixed-size caller-owned buffer, capping at its capacity --
// used for recording stream reads, where the caller (Kodi's demuxer) owns
// the destination buffer. A Range request should never actually return
// more than requested, so hitting the cap would indicate a confused
// server response rather than a normal condition.
struct FixedBufferSink
{
  uint8_t* buffer;
  unsigned int capacity;
  unsigned int written = 0;
};

// These are plain libcurl callback signatures (CURLOPT_WRITEFUNCTION/
// CURLOPT_HEADERFUNCTION) -- curl calls them during a live transfer, but
// none of them touch a CURL* themselves, so they're callable directly
// with synthetic data too. Pulled out here specifically so that's
// unit-testable standalone; see ../tests/test_curl_callbacks.cpp.

size_t FixedBufferWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

// Captures the total resource size from a "Content-Range: bytes X-Y/TOTAL"
// response header -- the only reliable way to learn a ranged request's
// full size, since Content-Length on a 206 response reflects only the
// requested slice.
size_t RecordingHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata);

// Captures the size from a plain "Content-Length: N" response header --
// used for a HEAD probe rather than RecordingHeaderCallback's ranged-GET
// Content-Range parsing, since Dispatcharr's in-progress-recording HLS
// segment endpoint ignores the Range header entirely and always serves the
// full segment body with a 200 (confirmed live: a "Range: 0-0" GET against
// a growing recording's seg_NNNNN.ts came back 200 with no Content-Range
// header at all, silently downloading the whole multi-MB segment on every
// probe instead of the intended few bytes -- HEAD avoids the body
// entirely, and this reads the size the same server response always
// carries either way).
size_t ContentLengthHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata);

// Plain "append everything to a string" sink, for requests whose response
// body is read in full (not into a fixed caller buffer).
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

} // namespace dispatcharr
