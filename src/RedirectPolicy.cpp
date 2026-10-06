#include "RedirectPolicy.h"

#include "StringUtil.h"

#include <cctype>
#include <cstdlib>

namespace dispatcharr
{

UrlOrigin ParseUrlOrigin(const std::string& url)
{
  UrlOrigin origin;
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos)
    return origin;
  const std::string scheme = ToLower(url.substr(0, schemeEnd));
  if (scheme != "http" && scheme != "https")
    return origin;

  const size_t authorityStart = schemeEnd + 3;
  size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  if (authorityEnd == std::string::npos)
    authorityEnd = url.size();
  std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);

  const size_t at = authority.rfind('@');
  if (at != std::string::npos)
  {
    origin.hasUserInfo = true;
    authority = authority.substr(at + 1);
  }

  std::string host;
  std::string portText;
  if (!authority.empty() && authority.front() == '[')
  {
    const size_t close = authority.find(']');
    if (close == std::string::npos)
      return origin;
    host = authority.substr(1, close - 1);
    const std::string rest = authority.substr(close + 1);
    if (!rest.empty())
    {
      if (rest.front() != ':')
        return origin;
      portText = rest.substr(1);
    }
  }
  else
  {
    const size_t colon = authority.find(':');
    host = authority.substr(0, colon);
    if (colon != std::string::npos)
      portText = authority.substr(colon + 1);
  }
  if (host.empty())
    return origin;

  int port = scheme == "https" ? 443 : 80;
  if (!portText.empty())
  {
    // Digits only: strtol would accept a sign or trailing text.
    for (char c : portText)
    {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return origin;
    }
    if (portText.size() > 5)
      return origin;
    port = std::atoi(portText.c_str());
    if (port < 1 || port > 65535)
      return origin;
  }

  origin.valid = true;
  origin.scheme = scheme;
  origin.host = ToLower(host);
  origin.port = port;
  return origin;
}

bool IsSafeRedirectTarget(const std::string& fromUrl, const std::string& toUrl)
{
  const UrlOrigin from = ParseUrlOrigin(fromUrl);
  const UrlOrigin to = ParseUrlOrigin(toUrl);
  if (!from.valid || !to.valid || to.hasUserInfo)
    return false;
  if (from.host != to.host)
    return false;
  if (from.scheme == "https" && to.scheme == "http")
    return false;
  return true;
}

bool IsSameOrigin(const std::string& urlA, const std::string& urlB)
{
  const UrlOrigin a = ParseUrlOrigin(urlA);
  const UrlOrigin b = ParseUrlOrigin(urlB);
  if (!a.valid || !b.valid || a.hasUserInfo || b.hasUserInfo)
    return false;
  return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

} // namespace dispatcharr
