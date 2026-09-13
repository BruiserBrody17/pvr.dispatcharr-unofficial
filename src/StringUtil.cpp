#include "StringUtil.h"

#include <algorithm>
#include <cctype>

namespace dispatcharr
{

std::string Base64Encode(const uint8_t* data, size_t len)
{
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += table[n & 0x3F];
    i += 3;
  }
  size_t remaining = len - i;
  if (remaining == 1)
  {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += "==";
  }
  else if (remaining == 2)
  {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out += table[(n >> 18) & 0x3F];
    out += table[(n >> 12) & 0x3F];
    out += table[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

} // namespace dispatcharr
