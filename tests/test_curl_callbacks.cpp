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
