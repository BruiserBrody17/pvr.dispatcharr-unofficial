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

// Whether a probe of the timeshift_buffer plugin's file server (a request to its segment base URL, sent with no token)
// proved the port reachable. Any HTTP answer does, including the 403 the file server gives a request without the
// buffer's access token: the probe asks "is something listening where the plugin said it is", not "may I read this". A
// connect failure or a timeout does not: the port is not published by the container, is published under another number
// than the plugin's `http_port` setting, or is blocked, which a user met as a spinner that ended the channel after
// about 30 s with nothing in Kodi saying why.
bool IsTimeshiftFileServerReachable(bool transferOk, long httpCode);

// What Kodi shows for an unreachable file server. Short on purpose: it is a toast.
std::string BuildTimeshiftFileServerUnreachableMessage(int httpPort);

} // namespace dispatcharr
