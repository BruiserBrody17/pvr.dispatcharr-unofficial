#include "UrlEncode.h"

namespace dispatcharr
{

// Encoded here rather than through curl_easy_escape: that needed a curl handle, and when the handle or the escape
// could not be had, the old code returned the value unescaped, so a title with '&' or '/' changed the request it was
// put into instead of failing (found by the 2026-10-18 read-only audit). Done by hand there is no failure path left to
// get wrong.
std::string UrlEncode(const std::string& value)
{
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const char ch : value)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '.' || c == '_' || c == '~';
    if (unreserved)
    {
      out += static_cast<char>(c);
    }
    else
    {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

} // namespace dispatcharr
