#include "PluginUrlUtil.h"

#include "StringUtil.h"

namespace dispatcharr
{

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

} // namespace dispatcharr
