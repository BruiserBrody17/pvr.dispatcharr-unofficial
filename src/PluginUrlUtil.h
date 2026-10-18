#pragma once

#include <string>

namespace dispatcharr
{

// Whether a route and port the timeshift_buffer plugin reported are fit to build a URL from. The two builders below
// put the route straight after "http://<host>:<port>", and the playlist one appends the buffer's access token, so a
// route that does not start the path changes where that token goes: "@otherhost/x" makes the URL's authority
// "<host>:<port>@otherhost" (found by the 2026-10-18 read-only audit). Only a buggy or compromised plugin would report
// one, but refusing it costs nothing. A route must start with a single '/' and hold no '@', backslash, '#', space or
// control character; the plugin's real routes are "/<channel-uuid>/live.m3u8" and "/<channel-uuid>/". A port must be
// 1-65535.
bool IsValidTimeshiftRoute(const std::string& route);
bool IsValidTimeshiftPort(int port);

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
// proved the port reachable. Any HTTP answer does, including the 404 the file server gives the bare channel prefix the
// probe asks for (confirmed live; it refuses a path shorter than "/<channel>/<file>" before it looks for a token): the
// probe asks "is something listening where the plugin said it is", not "may I read this". A
// connect failure or a timeout does not: the port is not published by the container, is published under another number
// than the plugin's `http_port` setting, or is blocked, which a user met as a spinner that ended the channel after
// about 30 s with nothing in Kodi saying why.
bool IsTimeshiftFileServerReachable(bool transferOk, long httpCode);

// What Kodi shows for an unreachable file server. Short on purpose: it is a toast.
std::string BuildTimeshiftFileServerUnreachableMessage(int httpPort);

} // namespace dispatcharr
