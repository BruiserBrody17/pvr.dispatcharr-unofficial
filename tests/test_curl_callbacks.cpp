#include "CurlCallbacks.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace dispatcharr;

// ---------------------------------------------------------------------
// WriteCallback
// ---------------------------------------------------------------------

TEST_CASE("WriteCallback appends to the target string", "[CurlCallbacks]")
{
  std::string out;
  const char* chunk1 = "hello ";
  const char* chunk2 = "world";

  size_t r1 = WriteCallback(const_cast<char*>(chunk1), 1, std::strlen(chunk1), &out);
  size_t r2 = WriteCallback(const_cast<char*>(chunk2), 1, std::strlen(chunk2), &out);

  CHECK(r1 == std::strlen(chunk1));
  CHECK(r2 == std::strlen(chunk2));
  CHECK(out == "hello world");
}

// ---------------------------------------------------------------------
// FixedBufferWriteCallback / FixedBufferSink
// ---------------------------------------------------------------------

TEST_CASE("FixedBufferWriteCallback writes within capacity", "[CurlCallbacks]")
{
  uint8_t buffer[16] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0};
  const char* data = "hello world";

  size_t r = FixedBufferWriteCallback(const_cast<char*>(data), 1, std::strlen(data), &sink);

  CHECK(r == std::strlen(data));
  CHECK(sink.written == std::strlen(data));
  CHECK(std::memcmp(buffer, data, std::strlen(data)) == 0);
}

TEST_CASE("FixedBufferWriteCallback accumulates across multiple calls", "[CurlCallbacks]")
{
  uint8_t buffer[16] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0};

  FixedBufferWriteCallback(const_cast<char*>("abc"), 1, 3, &sink);
  FixedBufferWriteCallback(const_cast<char*>("def"), 1, 3, &sink);

  CHECK(sink.written == 6);
  CHECK(std::memcmp(buffer, "abcdef", 6) == 0);
}

TEST_CASE("FixedBufferWriteCallback clamps at capacity rather than overflowing", "[CurlCallbacks]")
{
  // A Range request should never actually return more than requested --
  // this is the confused-server-response safety net, not a normal path.
  uint8_t buffer[4] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0};
  const char* data = "0123456789"; // 10 bytes into a 4-byte buffer

  size_t r = FixedBufferWriteCallback(const_cast<char*>(data), 1, std::strlen(data), &sink);

  // curl's own contract: the return value is what the transfer believes
  // was consumed, still reported as the full totalBytes even though only
  // `capacity` bytes actually landed in the buffer -- documenting the
  // real current behavior, not asserting it's ideal.
  CHECK(r == std::strlen(data));
  CHECK(sink.written == 4);
  CHECK(std::memcmp(buffer, "0123", 4) == 0);
}

TEST_CASE("FixedBufferWriteCallback reports truncation without aborting by default", "[CurlCallbacks]")
{
  uint8_t buffer[4] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0};
  CHECK_FALSE(sink.truncated);
  CHECK_FALSE(sink.abortWhenFull);
  FixedBufferWriteCallback(const_cast<char*>("abcd"), 1, 4, &sink); // exactly full: not truncated
  CHECK_FALSE(sink.truncated);
  CHECK(FixedBufferWriteCallback(const_cast<char*>("e"), 1, 1, &sink) == 1); // keeps consuming, as before
  CHECK(sink.truncated);
  CHECK(sink.written == 4);
}

TEST_CASE("FixedBufferWriteCallback with abortWhenFull ends the transfer once the buffer is full", "[CurlCallbacks]")
{
  uint8_t buffer[4] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0, false, true};

  // Within capacity, including exactly filling it: consumed in full, no abort.
  CHECK(FixedBufferWriteCallback(const_cast<char*>("ab"), 1, 2, &sink) == 2);
  CHECK(FixedBufferWriteCallback(const_cast<char*>("cd"), 1, 2, &sink) == 2);
  CHECK_FALSE(sink.truncated);

  // The first byte past the end: returning anything but the full count is how libcurl is told to stop.
  CHECK(FixedBufferWriteCallback(const_cast<char*>("e"), 1, 1, &sink) == 0);
  CHECK(sink.truncated);
  CHECK(sink.written == 4);
  CHECK(std::memcmp(buffer, "abcd", 4) == 0);
}

TEST_CASE("FixedBufferWriteCallback with abortWhenFull keeps what fit when one chunk straddles the end",
          "[CurlCallbacks]")
{
  uint8_t buffer[4] = {};
  FixedBufferSink sink{buffer, sizeof(buffer), 0, false, true};
  const char* data = "0123456789";

  CHECK(FixedBufferWriteCallback(const_cast<char*>(data), 1, std::strlen(data), &sink) == 0);
  CHECK(sink.truncated);
  CHECK(sink.written == 4);
  CHECK(std::memcmp(buffer, "0123", 4) == 0);
}

TEST_CASE("FixedBufferWriteCallback no-ops once the buffer is already full", "[CurlCallbacks]")
{
  uint8_t buffer[4] = {'a', 'a', 'a', 'a'};
  FixedBufferSink sink{buffer, sizeof(buffer), 4}; // already full

  size_t r = FixedBufferWriteCallback(const_cast<char*>("zzzz"), 1, 4, &sink);

  CHECK(r == 4);
  CHECK(sink.written == 4);
  CHECK(std::memcmp(buffer, "aaaa", 4) == 0); // untouched
}

// ---------------------------------------------------------------------
// RecordingHeaderCallback
// ---------------------------------------------------------------------

namespace
{
size_t CallHeader(size_t (*fn)(char*, size_t, size_t, void*), const std::string& line, void* userdata)
{
  return fn(const_cast<char*>(line.data()), 1, line.size(), userdata);
}
} // namespace

TEST_CASE("RecordingHeaderCallback extracts the total from Content-Range", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Range: bytes 0-99/123456\r\n";

  size_t r = CallHeader(RecordingHeaderCallback, line, &total);

  CHECK(r == line.size());
  CHECK(total == 123456);
}

TEST_CASE("RecordingHeaderCallback matches the header name case-insensitively", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "CONTENT-RANGE: bytes 0-9/500\r\n";

  CallHeader(RecordingHeaderCallback, line, &total);

  CHECK(total == 500);
}

TEST_CASE("RecordingHeaderCallback ignores an unrelated header", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Type: video/mp2t\r\n";

  CallHeader(RecordingHeaderCallback, line, &total);

  CHECK(total == -1); // untouched
}

TEST_CASE("RecordingHeaderCallback leaves the total untouched when there's no slash", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Range: bytes 0-99\r\n";

  CallHeader(RecordingHeaderCallback, line, &total);

  CHECK(total == -1);
}

TEST_CASE("RecordingHeaderCallback leaves the total untouched on a non-numeric size", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Range: bytes 0-99/not-a-number\r\n";

  CallHeader(RecordingHeaderCallback, line, &total);

  CHECK(total == -1);
}

// ---------------------------------------------------------------------
// ContentLengthHeaderCallback
// ---------------------------------------------------------------------

TEST_CASE("ContentLengthHeaderCallback extracts the size from Content-Length", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Length: 98765\r\n";

  size_t r = CallHeader(ContentLengthHeaderCallback, line, &total);

  CHECK(r == line.size());
  CHECK(total == 98765);
}

TEST_CASE("ContentLengthHeaderCallback matches the header name case-insensitively", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "CONTENT-LENGTH: 42\r\n";

  CallHeader(ContentLengthHeaderCallback, line, &total);

  CHECK(total == 42);
}

TEST_CASE("ContentLengthHeaderCallback ignores an unrelated header", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Type: video/mp2t\r\n";

  CallHeader(ContentLengthHeaderCallback, line, &total);

  CHECK(total == -1);
}

TEST_CASE("ContentLengthHeaderCallback leaves the total untouched on a non-numeric value", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Length: not-a-number\r\n";

  CallHeader(ContentLengthHeaderCallback, line, &total);

  CHECK(total == -1);
}

// ---------------------------------------------------------------------
// BoundedWriteCallback
// ---------------------------------------------------------------------

TEST_CASE("BoundedWriteCallback appends like WriteCallback while under the limit", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 100};
  char a[] = "hello ";
  char b[] = "world";

  CHECK(BoundedWriteCallback(a, 1, 6, &sink) == 6);
  CHECK(BoundedWriteCallback(b, 1, 5, &sink) == 5);

  CHECK(out == "hello world");
  CHECK_FALSE(sink.exceeded);
}

TEST_CASE("BoundedWriteCallback accepts a body that exactly fills the limit", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 10};
  char data[] = "0123456789";

  CHECK(BoundedWriteCallback(data, 1, 10, &sink) == 10);

  CHECK(out == "0123456789");
  CHECK_FALSE(sink.exceeded);
}

TEST_CASE("BoundedWriteCallback refuses a chunk that would pass the limit, keeping what it already has",
          "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 10};
  char first[] = "0123456";
  char second[] = "789AB"; // 7 + 5 = 12 > 10

  CHECK(BoundedWriteCallback(first, 1, 7, &sink) == 7);
  // 0 tells libcurl to fail the transfer (CURLE_WRITE_ERROR); a partial append would hide the cut.
  CHECK(BoundedWriteCallback(second, 1, 5, &sink) == 0);

  CHECK(out == "0123456");
  CHECK(sink.exceeded);
}

TEST_CASE("BoundedWriteCallback keeps refusing once it has refused", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 4};
  char big[] = "toolong";
  char small[] = "ab";

  CHECK(BoundedWriteCallback(big, 1, 7, &sink) == 0);
  // A later chunk that would fit is still taken -- libcurl stops calling after the first 0 anyway; this
  // pins that the sink itself holds no hidden "stop" state beyond the flag.
  CHECK(BoundedWriteCallback(small, 1, 2, &sink) == 2);
  CHECK(sink.exceeded);
  CHECK(out == "ab");
}

TEST_CASE("BoundedWriteCallback can't be tricked by a size that would wrap", "[CurlCallbacks]")
{
  std::string out = "x";
  BoundedStringSink sink{&out, 1000};
  char data[] = "y";

  // size * nmemb that wraps around size_t would pass a naive "size() + n > limit" check.
  CHECK(BoundedWriteCallback(data, SIZE_MAX / 2 + 1, 2, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(out == "x");
}

TEST_CASE("BoundedWriteCallback with a limit of zero accepts nothing", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 0};
  char data[] = "a";

  CHECK(BoundedWriteCallback(data, 1, 1, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(out.empty());
}

TEST_CASE("BoundedWriteCallback fails the transfer instead of throwing when the buffer cannot grow", "[CurlCallbacks]")
{
  // The 2026-10-04 hardening sweep reproduced a std::bad_alloc escaping this C
  // callback (RLIMIT_AS) near the ceiling on a constrained address space. A chunk
  // larger than std::string::max_size() takes the same catch path without needing
  // real memory exhaustion: append() throws std::length_error before it reads a
  // byte, so the pointer is never dereferenced.
  std::string out = "kept";
  BoundedStringSink sink{&out, SIZE_MAX};
  char byte = 'x';
  const size_t tooBig = out.max_size() + 1;
  REQUIRE(tooBig > out.max_size());
  CHECK(BoundedWriteCallback(&byte, 1, tooBig, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(sink.allocationFailed);
  CHECK(out == "kept");
}

TEST_CASE("WriteCallback returns 0 instead of throwing when the buffer cannot grow", "[CurlCallbacks]")
{
  std::string out = "kept";
  char byte = 'x';
  CHECK(WriteCallback(&byte, 1, out.max_size() + 1, &out) == 0);
  CHECK(out == "kept");
}

TEST_CASE("The ordinary refusal at the ceiling is not reported as an allocation failure", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 4};
  char data[] = "abcdef";
  CHECK(BoundedWriteCallback(data, 1, 6, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK_FALSE(sink.allocationFailed);
}

TEST_CASE("The per-call-site ceilings leave real payloads plenty of room", "[CurlCallbacks]")
{
  // Measured against a live instance: every real response sat far below its ceiling.
  CHECK(kMaxJsonResponseBytes >= 64u * 1024u * 1024u);
  CHECK(kMaxXmlTvResponseBytes >= 256u * 1024u * 1024u);
  CHECK(kMaxPlaylistResponseBytes >= 32u * 1024u * 1024u);
}

// ---------------------------------------------------------------------
// TransferAbortResult
// ---------------------------------------------------------------------

TEST_CASE("TransferAbortResult asks libcurl to stop only once the flag is set", "[CurlCallbacks]")
{
  std::atomic<bool> flag{false};
  CHECK(TransferAbortResult(&flag) == 0);
  flag = true;
  CHECK(TransferAbortResult(&flag) != 0);
  flag = false;
  CHECK(TransferAbortResult(&flag) == 0);
}

TEST_CASE("TransferAbortResult never aborts without a flag", "[CurlCallbacks]")
{
  CHECK(TransferAbortResult(nullptr) == 0);
}

TEST_CASE("RecordingHeaderCallback reads the total from an unsatisfied-range Content-Range", "[CurlCallbacks]")
{
  // "bytes */1234" is what a 416 answers with; the total is after the slash all the same.
  int64_t total = -1;
  std::string line = "Content-Range: bytes */1234\r\n";
  CHECK(CallHeader(RecordingHeaderCallback, line, &total) == line.size());
  CHECK(total == 1234);
}

TEST_CASE("RecordingHeaderCallback leaves the total unknown when the server does not know it either", "[CurlCallbacks]")
{
  int64_t total = -1;
  std::string line = "Content-Range: bytes 0-0/*\r\n";
  CHECK(CallHeader(RecordingHeaderCallback, line, &total) == line.size());
  CHECK(total == -1);
}

TEST_CASE("RecordingHeaderCallback copes with a header line shorter than the name it looks for", "[CurlCallbacks]")
{
  int64_t total = -1;
  for (const std::string line : {std::string("\r\n"), std::string("HTTP/1.1 206\r\n"), std::string("X\r\n"),
                                 std::string("content-range"), std::string("")})
  {
    CHECK(CallHeader(RecordingHeaderCallback, line, &total) == line.size());
  }
  CHECK(total == -1);
}

TEST_CASE("BoundedWriteCallback refuses a size times count that would wrap", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 1024};
  char byte = 'x';
  CHECK(BoundedWriteCallback(&byte, SIZE_MAX, 2, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK_FALSE(sink.allocationFailed);
  CHECK(out.empty());
}

TEST_CASE("BoundedWriteCallback refuses any append once the string already holds more than the limit",
          "[CurlCallbacks]")
{
  // limit - used would wrap to a huge number and the append would be accepted.
  std::string out(10, 'x');
  BoundedStringSink sink{&out, 5};
  char byte = 'y';
  CHECK(BoundedWriteCallback(&byte, 1, 1, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(out.size() == 10);
}

// ---------------------------------------------------------------------
// The sixteenth hardening sweep's mutation survivors: single-byte writes, a header with no space after the colon, and
// the two overflow guards of BoundedWriteCallback (each told apart by `allocationFailed`: a guard that fires returns
// before any allocation is attempted, one that does not reaches append(), which throws for a size this large).
// ---------------------------------------------------------------------

TEST_CASE("FixedBufferWriteCallback copies a single byte, and the single byte that is left", "[CurlCallbacks]")
{
  uint8_t buf[4] = {0, 0, 0, 0};
  FixedBufferSink sink{buf, 4};
  char one = 'x';
  CHECK(FixedBufferWriteCallback(&one, 1, 1, &sink) == 1);
  CHECK(sink.written == 1);
  CHECK(buf[0] == 'x');
  char three[3] = {'a', 'b', 'c'};
  CHECK(FixedBufferWriteCallback(three, 1, 3, &sink) == 3);
  CHECK(sink.written == 4);
  CHECK(std::memcmp(buf, "xabc", 4) == 0);
  CHECK_FALSE(sink.truncated);

  // One byte of room left, two offered: the one that fits is kept.
  uint8_t small[2] = {0, 0};
  FixedBufferSink tight{small, 2};
  char two[2] = {'p', 'q'};
  FixedBufferWriteCallback(two, 1, 1, &tight); // fills one
  CHECK(FixedBufferWriteCallback(two, 1, 2, &tight) == 2);
  CHECK(tight.written == 2);
  CHECK(small[1] == 'p');
  CHECK(tight.truncated);
}

TEST_CASE("ContentLengthHeaderCallback reads a value with no space after the colon", "[CurlCallbacks]")
{
  // Optional whitespace after the colon is legal (RFC 9110): "Content-Length:1234" is 1234, not 234.
  int64_t total = -1;
  const char* header = "Content-Length:1234\r\n";
  CHECK(ContentLengthHeaderCallback(const_cast<char*>(header), 1, std::strlen(header), &total) == std::strlen(header));
  CHECK(total == 1234);
  total = -1;
  const char* spaced = "content-length: 99\r\n";
  ContentLengthHeaderCallback(const_cast<char*>(spaced), 1, std::strlen(spaced), &total);
  CHECK(total == 99);
}

TEST_CASE("BoundedWriteCallback lets a product of exactly SIZE_MAX - 1 past the overflow guard", "[CurlCallbacks]")
{
  // size * nmemb is SIZE_MAX - 1: no overflow, so it is not the overflow guard that must refuse it but the limit check
  // (with a limit this large it fits, and the append is what fails). Only the one-past case wraps.
  std::string out;
  BoundedStringSink sink{&out, SIZE_MAX};
  char c = 0;
  CHECK(BoundedWriteCallback(&c, SIZE_MAX / 2, 2, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(sink.allocationFailed); // it got as far as trying
}

TEST_CASE("BoundedWriteCallback treats a wrapping size product as over the limit without allocating", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, SIZE_MAX};
  char c = 0;
  CHECK(BoundedWriteCallback(&c, SIZE_MAX / 2 + 1, 2, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK_FALSE(sink.allocationFailed); // refused by the overflow guard, before any allocation
}

TEST_CASE("BoundedWriteCallback compares what is left with the chunk, so a huge chunk cannot wrap past the limit",
          "[CurlCallbacks]")
{
  // used + totalBytes would wrap to 4 and look like it fits in a limit of 100.
  std::string out(10, 'a');
  BoundedStringSink sink{&out, 100};
  char c = 0;
  CHECK(BoundedWriteCallback(&c, 1, SIZE_MAX - 5, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK_FALSE(sink.allocationFailed);
  CHECK(out.size() == 10);
}

TEST_CASE("BoundedWriteCallback fills the limit exactly and refuses the next byte", "[CurlCallbacks]")
{
  std::string out;
  BoundedStringSink sink{&out, 5};
  char data[6] = {'1', '2', '3', '4', '5', '6'};
  CHECK(BoundedWriteCallback(data, 1, 5, &sink) == 5);
  CHECK_FALSE(sink.exceeded);
  CHECK(BoundedWriteCallback(data, 1, 1, &sink) == 0);
  CHECK(sink.exceeded);
  CHECK(out == "12345");
  // A string already past the limit (the limit was lowered, or grew elsewhere) refuses even an empty chunk's neighbour.
  std::string big(10, 'b');
  BoundedStringSink over{&big, 4};
  CHECK(BoundedWriteCallback(data, 1, 1, &over) == 0);
  CHECK(over.exceeded);
}
