#include "CurlCallbacks.h"

#include <algorithm>
#include <cctype>
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
  return totalBytes;
}

size_t RecordingHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
  auto* totalOut = static_cast<int64_t*>(userdata);
  size_t len = size * nitems;
  std::string line(buffer, len);
  std::string prefix = line.size() >= 14 ? line.substr(0, 14) : std::string();
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

} // namespace dispatcharr
