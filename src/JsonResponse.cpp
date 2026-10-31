#include "JsonResponse.h"

#include "UnicodeText.h"

#include <algorithm>
#include <exception>

namespace dispatcharr
{

bool ParseJsonResponseBody(const std::string& body, nlohmann::json& out, std::string& error)
{
  try
  {
    out = nlohmann::json::parse(body);
    return true;
  }
  catch (const std::exception& e)
  {
    error = std::string("Failed to parse JSON response: ") + e.what();
    return false;
  }
}

std::string SanitizeServerErrorBody(const std::string& body, std::size_t maxBytes)
{
  std::size_t end = std::min(body.size(), maxBytes);
  // Never cut inside a multi-byte sequence: back up over continuation bytes (10xxxxxx), and over the lead
  // byte they belong to if the sequence would be incomplete.
  if (end < body.size())
  {
    while (end > 0 && (static_cast<unsigned char>(body[end]) & 0xC0) == 0x80)
      --end;
  }
  const std::string kept = body.substr(0, end);
  std::string out;
  out.reserve(kept.size());
  // C1 controls too (U+0080-U+009F, and a stray raw 0x80-0x9F byte, which a terminal not in UTF-8 reads the same):
  // U+009B is CSI on its own. A raw 0x80-0x9F byte inside a valid sequence is a continuation byte, so decode first.
  for (std::size_t pos = 0; pos < kept.size();)
  {
    std::size_t length = 1;
    const char32_t cp = DecodeUtf8At(kept, pos, length);
    const unsigned char lead = static_cast<unsigned char>(kept[pos]);
    const bool stray = length == 1 && lead >= 0x80 && lead <= 0x9F;
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || stray)
      out += ' ';
    else
      out.append(kept, pos, length);
    pos += length;
  }
  if (end < body.size())
    out += "... [" + std::to_string(body.size() - end) + " more bytes not shown]";
  return out;
}

} // namespace dispatcharr
