#include "PluginUrlUtil.h"

#include "StringUtil.h"

namespace dispatcharr
{

bool IsValidTimeshiftRoute(const std::string& route)
{
  if (route.empty() || route[0] != '/' || (route.size() > 1 && route[1] == '/'))
    return false;
  for (const char ch : route)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c <= 0x20 || c == 0x7F || c == '@' || c == '\\' || c == '#')
      return false;
  }
  return true;
}

bool IsValidTimeshiftPort(int port)
{
  return port >= 1 && port <= 65535;
}

std::string BuildTimeshiftPlaylistUrl(const std::string& host, int httpPort, const std::string& playlistRoute,
                                      const std::string& accessToken)
{
  std::string url = "http://" + FormatHostForUrl(host) + ":" + std::to_string(httpPort) + playlistRoute;
  if (!accessToken.empty())
    url += (url.find('?') == std::string::npos ? "?token=" : "&token=") + accessToken;
  return url;
}

std::string BuildTimeshiftSegmentBaseUrl(const std::string& host, int httpPort, const std::string& segmentRoutePrefix)
{
  return "http://" + FormatHostForUrl(host) + ":" + std::to_string(httpPort) + segmentRoutePrefix;
}

bool IsTimeshiftFileServerReachable(bool transferOk, long httpCode)
{
  return transferOk && httpCode > 0;
}

std::string BuildTimeshiftFileServerUnreachableMessage(int httpPort)
{
  return "Can't reach Dispatcharr's timeshift file server on port " + std::to_string(httpPort) +
         ". Publish that port and make it match the plugin's http_port setting.";
}

} // namespace dispatcharr
