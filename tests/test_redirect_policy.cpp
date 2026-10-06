#include "RedirectPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ParseUrlOrigin reads scheme, host and port", "[RedirectPolicy]")
{
  UrlOrigin o = ParseUrlOrigin("http://dispatcharr.example:9191/api/x?y=1#z");
  REQUIRE(o.valid);
  CHECK(o.scheme == "http");
  CHECK(o.host == "dispatcharr.example");
  CHECK(o.port == 9191);
  CHECK_FALSE(o.hasUserInfo);
}

TEST_CASE("ParseUrlOrigin applies the default port and lowercases", "[RedirectPolicy]")
{
  UrlOrigin https = ParseUrlOrigin("HTTPS://Media.Example/path");
  REQUIRE(https.valid);
  CHECK(https.scheme == "https");
  CHECK(https.host == "media.example");
  CHECK(https.port == 443);
  CHECK(ParseUrlOrigin("http://h").port == 80);
}

TEST_CASE("ParseUrlOrigin handles IPv6 literals", "[RedirectPolicy]")
{
  UrlOrigin o = ParseUrlOrigin("http://[::1]:9191/a");
  REQUIRE(o.valid);
  CHECK(o.host == "::1");
  CHECK(o.port == 9191);
  UrlOrigin bare = ParseUrlOrigin("https://[2001:db8::5]/a");
  REQUIRE(bare.valid);
  CHECK(bare.host == "2001:db8::5");
  CHECK(bare.port == 443);
}

TEST_CASE("ParseUrlOrigin ends the authority at the first path, query or fragment character", "[RedirectPolicy]")
{
  CHECK(ParseUrlOrigin("http://a.example?x=http://b.example/").host == "a.example");
  CHECK(ParseUrlOrigin("http://a.example#frag").host == "a.example");
  // A second "://" later in the URL isn't the authority.
  CHECK(ParseUrlOrigin("http://a.example/redirect?to=http://b.example/").host == "a.example");
}

TEST_CASE("ParseUrlOrigin reports user info instead of treating it as the host", "[RedirectPolicy]")
{
  UrlOrigin o = ParseUrlOrigin("http://trusted.example@evil.example/path");
  REQUIRE(o.valid);
  CHECK(o.host == "evil.example"); // what a client would actually connect to
  CHECK(o.hasUserInfo);
  CHECK(ParseUrlOrigin("http://user:pw@h.example:81/").hasUserInfo);
}

TEST_CASE("ParseUrlOrigin rejects what isn't an absolute http(s) URL", "[RedirectPolicy]")
{
  for (const char* bad :
       {"", "/relative/path", "//host/path", "host/path", "ftp://h/x", "file:///etc/passwd", "javascript://h",
        "http://", "http:///path", "http://h:abc/", "http://h:/x", "http://h:0/", "http://h:70000/", "http://h:-1/",
        "http://h:+80/", "http://[::1/", "http://[::1]x/", "http://h:123456/"})
  {
    INFO(bad);
    // "http://h:/x" has an empty port, which is legal; the others must not validate.
    if (std::string(bad) == "http://h:/x")
      continue;
    CHECK_FALSE(ParseUrlOrigin(bad).valid);
  }
}

TEST_CASE("IsSafeRedirectTarget allows a same-host redirect, including the ones real setups use", "[RedirectPolicy]")
{
  const std::string from = "http://dispatcharr.example:9191/api/channels/recordings/5/file/";
  CHECK(IsSafeRedirectTarget(from, "http://dispatcharr.example:9191/api/channels/recordings/5/hls/index.m3u8"));
  // A reverse proxy moving to another port, and an upgrade to https.
  CHECK(IsSafeRedirectTarget(from, "http://dispatcharr.example:8080/x"));
  CHECK(IsSafeRedirectTarget(from, "https://dispatcharr.example/x"));
  CHECK(IsSafeRedirectTarget("https://dispatcharr.example/a", "https://dispatcharr.example/b"));
  CHECK(IsSafeRedirectTarget("https://dispatcharr.example/a", "https://DISPATCHARR.example/b"));
  CHECK(IsSafeRedirectTarget("http://[::1]:9191/a", "http://[::1]:9191/b"));
}

TEST_CASE("IsSafeRedirectTarget refuses a different host", "[RedirectPolicy]")
{
  const std::string from = "http://dispatcharr.example:9191/a";
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://evil.example:9191/a"));
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://dispatcharr.example.evil.example/a"));
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://evil-dispatcharr.example/a"));
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://127.0.0.1:9191/a"));
  CHECK_FALSE(IsSafeRedirectTarget("http://192.168.1.5:9191/a", "http://192.168.1.6:9191/a"));
  CHECK_FALSE(IsSafeRedirectTarget("http://[::1]:9191/a", "http://[::2]:9191/a"));
}

TEST_CASE("IsSafeRedirectTarget refuses a downgrade from https to http", "[RedirectPolicy]")
{
  CHECK_FALSE(IsSafeRedirectTarget("https://dispatcharr.example/a", "http://dispatcharr.example/a"));
  CHECK_FALSE(IsSafeRedirectTarget("https://dispatcharr.example:9191/a", "http://dispatcharr.example:9191/a"));
}

TEST_CASE("IsSafeRedirectTarget refuses a target that smuggles in another host through user info", "[RedirectPolicy]")
{
  const std::string from = "http://dispatcharr.example:9191/a";
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://dispatcharr.example@evil.example/a"));
  // Even when the host after the "@" does match, user info of its own is not something to follow.
  CHECK_FALSE(IsSafeRedirectTarget(from, "http://user:pw@dispatcharr.example:9191/a"));
}

TEST_CASE("IsSafeRedirectTarget refuses anything that isn't an absolute http(s) URL", "[RedirectPolicy]")
{
  const std::string from = "http://dispatcharr.example:9191/a";
  CHECK_FALSE(IsSafeRedirectTarget(from, ""));
  CHECK_FALSE(IsSafeRedirectTarget(from, "/relative"));
  CHECK_FALSE(IsSafeRedirectTarget(from, "ftp://dispatcharr.example/a"));
  CHECK_FALSE(IsSafeRedirectTarget(from, "file:///etc/passwd"));
  CHECK_FALSE(IsSafeRedirectTarget("", "http://dispatcharr.example/a"));
  CHECK_FALSE(IsSafeRedirectTarget("not a url", "http://dispatcharr.example/a"));
}

// ---------------------------------------------------------------------
// DecideRedirectFollow
// ---------------------------------------------------------------------

TEST_CASE("DecideRedirectFollow hands back any non-redirect status as the final response", "[RedirectPolicy]")
{
  for (long code : {200L, 204L, 206L, 304L, 400L, 401L, 404L, 500L})
    CHECK(DecideRedirectFollow(code, 0, 5, true) == RedirectFollowAction::kNotARedirect);
}

TEST_CASE("DecideRedirectFollow repeats a POST as a GET only for 301/302/303, like libcurl", "[RedirectPolicy]")
{
  for (long code : {301L, 302L, 303L})
  {
    CHECK(DecideRedirectFollow(code, 0, 5, /*switchPostToGet=*/true) == RedirectFollowAction::kFollowAsGet);
    CHECK(DecideRedirectFollow(code, 0, 5, /*switchPostToGet=*/false) == RedirectFollowAction::kFollow);
  }
  // 307/308 keep the method and body regardless.
  for (long code : {307L, 308L})
  {
    CHECK(DecideRedirectFollow(code, 0, 5, true) == RedirectFollowAction::kFollow);
    CHECK(DecideRedirectFollow(code, 0, 5, false) == RedirectFollowAction::kFollow);
  }
}

TEST_CASE("DecideRedirectFollow stops a chain at the hop limit", "[RedirectPolicy]")
{
  CHECK(DecideRedirectFollow(302, 4, 5, true) == RedirectFollowAction::kFollowAsGet);
  CHECK(DecideRedirectFollow(302, 5, 5, true) == RedirectFollowAction::kTooManyRedirects);
  CHECK(DecideRedirectFollow(308, 9, 5, false) == RedirectFollowAction::kTooManyRedirects);
  // The limit never turns a final response into an error.
  CHECK(DecideRedirectFollow(200, 99, 5, true) == RedirectFollowAction::kNotARedirect);
}

// ---------------------------------------------------------------------
// IsSameOrigin
// ---------------------------------------------------------------------

TEST_CASE("IsSameOrigin is true for the same scheme, host and port whatever the path", "[RedirectPolicy]")
{
  const std::string base = "http://dispatcharr.example:9191";
  CHECK(IsSameOrigin("http://dispatcharr.example:9191/api/channels/recordings/5/hls/seg_00001.ts", base));
  CHECK(IsSameOrigin("http://dispatcharr.example:9191", base));
  CHECK(IsSameOrigin("HTTP://Dispatcharr.Example:9191/x?y=1", base)); // scheme and host compare case-insensitively
}

TEST_CASE("IsSameOrigin compares a default port as its number", "[RedirectPolicy]")
{
  CHECK(IsSameOrigin("https://dispatcharr.example/x", "https://dispatcharr.example:443"));
  CHECK(IsSameOrigin("http://dispatcharr.example:80/x", "http://dispatcharr.example"));
  CHECK_FALSE(IsSameOrigin("https://dispatcharr.example:8443/x", "https://dispatcharr.example:443"));
}

TEST_CASE("IsSameOrigin is false for another host, port or scheme", "[RedirectPolicy]")
{
  const std::string base = "http://dispatcharr.example:9191";
  CHECK_FALSE(IsSameOrigin("http://other.example:9191/x", base));
  CHECK_FALSE(IsSameOrigin("http://dispatcharr.example:9192/x", base)); // same host, another service
  CHECK_FALSE(IsSameOrigin("https://dispatcharr.example:9191/x", base));
  // The host is compared whole: a lookalike that merely starts with it is another host.
  CHECK_FALSE(IsSameOrigin("http://dispatcharr.example.evil.test:9191/x", base));
  CHECK_FALSE(IsSameOrigin("http://127.0.0.1:19192/foreign/5/seg_00001.ts", "http://127.0.0.1:19191"));
}

TEST_CASE("IsSameOrigin refuses userinfo tricks and anything that is not an absolute http(s) URL", "[RedirectPolicy]")
{
  const std::string base = "http://dispatcharr.example:9191";
  // `trusted@evil` is the host `evil`, not `trusted`: the userinfo makes either side unusable.
  CHECK_FALSE(IsSameOrigin("http://dispatcharr.example:9191@evil.test/x", base));
  CHECK_FALSE(IsSameOrigin("http://user:pw@dispatcharr.example:9191/x", base));
  CHECK_FALSE(IsSameOrigin("seg_00001.ts", base));
  CHECK_FALSE(IsSameOrigin("//dispatcharr.example:9191/x", base));
  CHECK_FALSE(IsSameOrigin("ftp://dispatcharr.example:9191/x", base));
  CHECK_FALSE(IsSameOrigin("", base));
  CHECK_FALSE(IsSameOrigin("http://dispatcharr.example:9191/x", ""));
  CHECK_FALSE(IsSameOrigin("", ""));
}

TEST_CASE("IsSameOrigin handles IPv6 literals", "[RedirectPolicy]")
{
  CHECK(IsSameOrigin("http://[2001:db8::1]:9191/x", "http://[2001:DB8::1]:9191"));
  CHECK_FALSE(IsSameOrigin("http://[2001:db8::2]:9191/x", "http://[2001:db8::1]:9191"));
}

TEST_CASE("ParseUrlOrigin refuses a port long enough to wrap an int", "[RedirectPolicy]")
{
  // atoi("4294967376") wraps to 80: without the length check this origin would read as the default
  // port and pass IsSameOrigin() against a plain http URL.
  CHECK_FALSE(ParseUrlOrigin("http://h:4294967376/").valid);
  CHECK_FALSE(ParseUrlOrigin("http://h:65536/").valid);
  CHECK_FALSE(ParseUrlOrigin("http://h:099999/").valid);
  CHECK_FALSE(IsSameOrigin("http://h:4294967376/x", "http://h/x"));
  CHECK(ParseUrlOrigin("http://h:65535/").valid);
}

TEST_CASE("IsSameOrigin is false when either side carries user info, in both directions", "[RedirectPolicy]")
{
  CHECK_FALSE(IsSameOrigin("http://h:1/x", "http://u:p@h:1/"));
  CHECK_FALSE(IsSameOrigin("http://u:p@h:1/", "http://h:1/x"));
  CHECK_FALSE(IsSameOrigin("http://u:p@h:1/", "http://u:p@h:1/"));
  CHECK(IsSameOrigin("http://h:1/a", "http://h:1/b"));
}

TEST_CASE("an authority with two @ signs carries user info and is never the same origin", "[RedirectPolicy]")
{
  const UrlOrigin twice = ParseUrlOrigin("http://a@b@host/");
  CHECK((!twice.valid || twice.hasUserInfo));
  CHECK_FALSE(IsSameOrigin("http://a@b@host/", "http://host/"));
  CHECK_FALSE(IsSameOrigin("http://host/", "http://a@b@host/"));
}
