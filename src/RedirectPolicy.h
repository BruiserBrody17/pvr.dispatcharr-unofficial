#pragma once

#include <string>

namespace dispatcharr
{

// The scheme, host and port of an absolute http(s) URL. `valid` is false for
// anything else (no scheme, a scheme other than http/https, an empty host, a
// malformed port), so callers can refuse it rather than guess.
struct UrlOrigin
{
  bool valid = false;
  std::string scheme;       // lowercase: "http" or "https"
  std::string host;         // lowercase, IPv6 brackets removed
  int port = 0;             // the URL's own port, or the scheme's default
  bool hasUserInfo = false; // a "user:pass@" before the host
};

UrlOrigin ParseUrlOrigin(const std::string& url);

// Whether a request carrying credentials (an X-API-Key header, an
// Authorization bearer token, or a login POST body) may follow a redirect from
// `fromUrl` to `toUrl`. True only when the target is the same host and not a
// downgrade from https to http.
//
// Why this exists (docs/CLOSED_ITEMS.md, "X-API-Key could follow a cross-host or
// downgrading redirect"): libcurl's own FOLLOWLOCATION only withholds the
// Authorization header on a cross-host redirect, and only since 7.58. A custom
// header like X-API-Key is sent wherever the redirect points, a 307/308 re-sends
// a POST body (a login's username and password) to it, and an http:// target
// sends all of it in the clear. A misconfigured reverse proxy in front of
// Dispatcharr is a real, if narrow, way to get a redirect like that.
//
// Deliberately permissive about what a same-host redirect may do, since those
// are real and wanted: a different port (a reverse proxy, a port mapping), a
// different path, and an http -> https upgrade. Refuses a target carrying
// "user:pass@" credentials of its own: that is how `http://trusted@evil/`
// disguises a different host.
bool IsSafeRedirectTarget(const std::string& fromUrl, const std::string& toUrl);

// Whether two absolute http(s) URLs have exactly the same scheme, host and port
// (default ports compared as their numbers, so "https://h" and "https://h:443"
// match), neither carrying "user:pass@". Used to decide where the X-API-Key may be
// sent for a URL taken from a server-supplied playlist: only to the configured
// server itself. Stricter than IsSafeRedirectTarget() on purpose -- a playlist has
// no business naming another port, let alone another host, and the key must never
// be steered to one (docs/CLOSED_ITEMS.md, "The API key was attached to a segment URL
// on any host"). False when either URL is not a valid absolute http(s) URL.
bool IsSameOrigin(const std::string& urlA, const std::string& urlB);

// What DispatcharrClient::PerformWithSafeRedirects() does with one response
// while following redirects by hand (it does so instead of using
// CURLOPT_FOLLOWLOCATION so each target passes IsSafeRedirectTarget() first):
//   kNotARedirect     -- any status but 301/302/303/307/308: this is the final
//                        response, hand it back.
//   kTooManyRedirects -- a redirect, but `hop` already reached `maxRedirects`.
//   kFollowAsGet      -- a 301/302/303 answer to a POST (`switchPostToGet`):
//                        repeated as a GET, which is what libcurl itself does.
//   kFollow           -- any other redirect: repeated as the same method, so a
//                        307/308 re-sends a POST with its body.
// The caller still checks the Location header and IsSafeRedirectTarget() between
// kNotARedirect and the two follow outcomes; this only pins the status mapping.
enum class RedirectFollowAction
{
  kNotARedirect,
  kTooManyRedirects,
  kFollow,
  kFollowAsGet,
};

inline RedirectFollowAction DecideRedirectFollow(long httpCode, int hop, int maxRedirects, bool switchPostToGet)
{
  const bool switchesToGet = httpCode == 301 || httpCode == 302 || httpCode == 303;
  if (!switchesToGet && httpCode != 307 && httpCode != 308)
    return RedirectFollowAction::kNotARedirect;
  if (hop >= maxRedirects)
    return RedirectFollowAction::kTooManyRedirects;
  return switchPostToGet && switchesToGet ? RedirectFollowAction::kFollowAsGet : RedirectFollowAction::kFollow;
}

// The whole decision PerformWithSafeRedirects() makes for one response, in the order that matters:
//   kReturnResponse -- not a redirect, or a redirect with no Location: hand the response back as it is.
//   kRefuseUnsafe   -- a redirect whose target is not safe (another host, https to http, user info): not followed,
//                      the caller sees the 3xx itself, and it is refused EVEN WHEN the chain is also too long, so
//                      the refusal is never mistaken for a hop-limit failure and is always logged.
//   kFailTooMany    -- a safe redirect, but the hop limit is reached.
//   kFollow / kFollowAsGet -- as DecideRedirectFollow().
// `hasLocation` and `targetIsSafe` are only meaningful for a redirect status; they are ignored otherwise.
enum class RedirectStep
{
  kReturnResponse,
  kRefuseUnsafe,
  kFailTooMany,
  kFollow,
  kFollowAsGet,
};

inline RedirectStep DecideRedirectStep(long httpCode, int hop, int maxRedirects, bool switchPostToGet, bool hasLocation,
                                       bool targetIsSafe)
{
  const RedirectFollowAction action = DecideRedirectFollow(httpCode, hop, maxRedirects, switchPostToGet);
  if (action == RedirectFollowAction::kNotARedirect || !hasLocation)
    return RedirectStep::kReturnResponse;
  if (!targetIsSafe)
    return RedirectStep::kRefuseUnsafe;
  if (action == RedirectFollowAction::kTooManyRedirects)
    return RedirectStep::kFailTooMany;
  return action == RedirectFollowAction::kFollowAsGet ? RedirectStep::kFollowAsGet : RedirectStep::kFollow;
}

} // namespace dispatcharr
