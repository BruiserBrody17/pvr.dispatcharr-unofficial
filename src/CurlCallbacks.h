#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dispatcharr
{

// Writes into a fixed-size caller-owned buffer, capping at its capacity --
// originally written for recording stream reads, where the caller
// (Kodi's demuxer) owns the destination buffer and a Range request
// should never actually return more than requested, so hitting the cap
// there would indicate a confused server response rather than a normal
// condition. Broadened in scope since (57th/58th-pass audits, updated
// 2026-09-27, fixing this comment's own now-stale, too-narrow original
// scope, found via a project-wide review, not itself independently
// reproduced) to several tiny discard-the-body probes (a "0-0" Range GET
// only checking the response's own status/headers) and one
// bounded-to-a-known-size segment fetch, where hitting the cap is the
// deliberately-desired safety behavior instead, not a confused-response
// signal -- see each call site's own comment for which case applies.
struct FixedBufferSink
{
  uint8_t* buffer;
  unsigned int capacity;
  unsigned int written = 0;
  // Set by the callback whenever the response had more bytes than the buffer holds.
  bool truncated = false;
  // End the transfer (the callback returns 0, which libcurl reports as CURLE_WRITE_ERROR) as soon as the buffer is
  // full instead of reading and discarding the rest. For a ranged read of a recording: a server (or proxy) that
  // ignores Range answers with the whole file, which used to be downloaded to the last byte or the request
  // timeout just to be thrown away. The caller treats CURLE_WRITE_ERROR with `truncated` set as "the buffer
  // filled" and looks at the HTTP status for what it means (a 200 at offset 0 is fine, one past it is not).
  bool abortWhenFull = false;
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

// WriteCallback with a ceiling. WriteCallback itself appends without limit,
// so a server (or a proxy in front of one) that answered with a response far
// larger than anything real -- or never stopped -- ran this addon out of
// memory, and the std::bad_alloc is thrown inside a plain C callback libcurl
// is calling, which is very likely to end in std::terminate() rather than a
// catchable error (docs/CLOSED_ITEMS.md, "WriteCallback has no upper bound").
// Past `limit` this stops accepting data and returns 0, which libcurl turns
// into a failed transfer (CURLE_WRITE_ERROR); `exceeded` tells the caller
// *why* it failed so the error can say so.
struct BoundedStringSink
{
  std::string* out;
  size_t limit;
  bool exceeded = false;
  // The buffer could not grow (std::bad_alloc / length_error): the process ran out
  // of address space or memory before the ceiling was reached. Set together with
  // `exceeded` so a caller that only checks that one still fails the transfer.
  bool allocationFailed = false;
};

size_t BoundedWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

// Per-call-site ceilings for BoundedStringSink, each a generous multiple of the
// largest real payload measured against a live instance (2026-09-30) rather
// than one number for everything -- a legitimately large response must never
// be truncated:
//  - JSON API responses: the biggest is the channel list, far below the 128 MiB limit.
//  - the XMLTV guide: measured well below the 512 MiB limit for the default window.
//  - an in-progress recording's HLS playlist: about 100 bytes per segment,
//    ~2 MB for a recording a day long; 64 MiB covers a month of it.
//
// Lower on a 32-bit build (the CoreELEC userland on the ODROID N2+ is armhf): a
// std::string grows by doubling, so a body close to a ceiling needs one
// contiguous allocation of up to twice its size, and a 32-bit process has about
// 3 GiB of address space to find it in. Found by the 2026-10-04 hardening sweep
// with RLIMIT_AS: the 512 MiB ceiling was only ever reached by a std::bad_alloc
// thrown out of this C callback. BoundedWriteCallback() now catches that too, so
// the ceiling is a limit and not the only thing standing between a hostile or
// runaway response and std::terminate().
constexpr bool kIs64BitAddressSpace = sizeof(void*) >= 8;
constexpr size_t kMaxJsonResponseBytes = (kIs64BitAddressSpace ? 128u : 64u) * 1024u * 1024u;
constexpr size_t kMaxXmlTvResponseBytes = (kIs64BitAddressSpace ? 512u : 256u) * 1024u * 1024u;
constexpr size_t kMaxPlaylistResponseBytes = (kIs64BitAddressSpace ? 64u : 32u) * 1024u * 1024u;

// What libcurl's transfer-progress callback (CURLOPT_XFERINFOFUNCTION) should
// return given an abort flag: non-zero ends the transfer at once (with
// CURLE_ABORTED_BY_CALLBACK), zero lets it carry on. libcurl calls it about once
// a second even when no data is moving -- during a connect or while waiting for a
// response -- which is what makes a request stuck on an unresponsive server
// abortable at all (CURLOPT_TIMEOUT is the only other way out, and is 30 s by
// default, more when the user raises it). A null flag never aborts.
inline int TransferAbortResult(const std::atomic<bool>* abortFlag)
{
  return (abortFlag != nullptr && abortFlag->load()) ? 1 : 0;
}

} // namespace dispatcharr
