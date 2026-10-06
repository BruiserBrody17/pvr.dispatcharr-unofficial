#include "JsonResponse.h"

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
  std::string out = body.substr(0, end);
  for (char& c : out)
  {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F)
      c = ' ';
  }
  if (end < body.size())
    out += "... [" + std::to_string(body.size() - end) + " more bytes not shown]";
  return out;
}

} // namespace dispatcharr
