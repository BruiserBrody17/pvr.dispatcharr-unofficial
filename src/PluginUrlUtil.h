#pragma once

#include <string>

namespace dispatcharr
{

// DispatcharrClient::CallTimeshiftPluginAction()'s own playlist URL
// assembly: http://<host>:<httpPort><playlistRoute>, with a
// ?token=/&token= query param appended when accessToken is non-empty
// (an empty token, e.g. a pre-upgrade plugin response that doesn't send
// one, appends nothing, matching the original conditional). The `?` vs
// `&` choice depends on whether playlistRoute itself already carries a
// query string, not just assumed absent.
std::string BuildTimeshiftPlaylistUrl(const std::string& host, int httpPort, const std::string& playlistRoute,
                                      const std::string& accessToken);

// DispatcharrClient::RefreshLiveManifest()'s own segment base URL:
// http://<host>:<httpPort><segmentRoutePrefix>. ReadLiveTimeshiftStream()
// appends each segment's own filename plus its own "?token=" query
// param to this directly, so this builder only covers the shared prefix
// both that read path and the playlist URL above assemble the same way.
std::string BuildTimeshiftSegmentBaseUrl(const std::string& host, int httpPort, const std::string& segmentRoutePrefix);

} // namespace dispatcharr
