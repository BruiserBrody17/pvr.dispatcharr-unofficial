#include "CurlCallbacks.h"

#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <cstring>

namespace dispatcharr
{

size_t FixedBufferWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* sink = static_cast<FixedBufferSink*>(userdata);
  size_t totalBytes = size * nmemb;
  size_t remaining = sink->capacity - sink->written;
  size_t toCopy = std::min(totalBytes, remaining);
  if (toCopy > 0)
  {
    std::memcpy(sink->buffer + sink->written, ptr, toCopy);
    sink->written += static_cast<unsigned int>(toCopy);
  }
  if (toCopy < totalBytes)
  {
    sink->truncated = true;
    if (sink->abortWhenFull)
      return 0;
  }
  return totalBytes;
}

size_t RecordingHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* totalOut = static_cast<int64_t*>(userdata);
  size_t len = size * nitems;
  std::string line(buffer, len);
  std::string prefix = line.size() >= 14 ? line.substr(0, 14) : std::string();
  std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return AsciiToLower(c); });
  if (prefix == "content-range:")
  {
    size_t slash = line.rfind('/');
    if (slash != std::string::npos)
    {
      try
      {
        *totalOut = std::stoll(line.substr(slash + 1));
      }
      catch (const std::exception&)
      {
      }
    }
  }
  return len;
}

size_t ContentLengthHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* totalOut = static_cast<int64_t*>(userdata);
  size_t len = size * nitems;
  std::string line(buffer, len);
  std::string prefix = line.size() >= 15 ? line.substr(0, 15) : std::string();
  std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return AsciiToLower(c); });
  if (prefix == "content-length:")
  {
    try
    {
      *totalOut = std::stoll(line.substr(15));
    }
    catch (const std::exception&)
    {
    }
  }
  return len;
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* out = static_cast<std::string*>(userdata);
  // An exception must never leave a callback libcurl calls through C: returning
  // fewer bytes than were offered makes the transfer fail with CURLE_WRITE_ERROR.
  try
  {
    out->append(ptr, size * nmemb);
  }
  catch (const std::exception&)
  {
    return 0;
  }
  return size * nmemb;
}

size_t BoundedWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  auto* sink = static_cast<BoundedStringSink*>(userdata);
  // libcurl never hands over a size * nmemb that overflows, but this is the one
  // callback whose whole job is to be safe against a hostile peer: treat a
  // product that wraps as over the limit rather than as the wrapped value.
  if (nmemb != 0 && size > SIZE_MAX / nmemb)
  {
    sink->exceeded = true;
    return 0;
  }
  const size_t totalBytes = size * nmemb;
  // Compared as "does it fit in what's left" rather than size() + totalBytes
  // > limit, which a huge totalBytes could wrap past.
  const size_t used = sink->out->size();
  if (used > sink->limit || totalBytes > sink->limit - used)
  {
    sink->exceeded = true;
    return 0;
  }
  try
  {
    sink->out->append(ptr, totalBytes);
  }
  catch (const std::exception&) // std::bad_alloc or std::length_error
  {
    sink->exceeded = true;
    sink->allocationFailed = true;
    return 0;
  }
  return totalBytes;
}

} // namespace dispatcharr
