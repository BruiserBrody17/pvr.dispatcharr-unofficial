#include "UrlEncode.h"

#include <catch2/catch_test_macros.hpp>
#include <curl/curl.h>

#include <string>

using namespace dispatcharr;

TEST_CASE("UrlEncode leaves unreserved characters alone", "[UrlEncode]")
{
  CHECK(UrlEncode("abcXYZ123-_.~") == "abcXYZ123-_.~");
}

TEST_CASE("UrlEncode escapes spaces and reserved characters", "[UrlEncode]")
{
  CHECK(UrlEncode("Channel A News") == "Channel%20A%20News");
  CHECK(UrlEncode("a&b") == "a%26b");
  CHECK(UrlEncode("a=b") == "a%3Db");
}

TEST_CASE("UrlEncode handles an empty string", "[UrlEncode]")
{
  CHECK(UrlEncode("") == "");
}

TEST_CASE("UrlEncode is idempotent for a string with nothing to escape twice", "[UrlEncode]")
{
  std::string once = UrlEncode("tvg-id.US");
  CHECK(UrlEncode(once) == once);
}

TEST_CASE("UrlEncode escapes multi-byte UTF-8 and an embedded NUL byte by byte", "[UrlEncode]")
{
  CHECK(UrlEncode("caf\xC3\xA9") == "caf%C3%A9");
  CHECK(UrlEncode(std::string("a\0b", 3)) == "a%00b");
  CHECK(UrlEncode("/path?x=1#y") == "%2Fpath%3Fx%3D1%23y");
}

// UrlEncode no longer goes through curl (it returned the value unescaped when curl's handle or escape failed, found by
// the 2026-10-18 read-only audit); this pins that the self-contained encoder gives exactly what curl_easy_escape did,
// for every byte value.
TEST_CASE("UrlEncode matches curl_easy_escape for every byte value", "[UrlEncode]")
{
  CURL* curl = curl_easy_init();
  REQUIRE(curl != nullptr);
  std::string all;
  for (int b = 0; b < 256; ++b)
  {
    const std::string one(1, static_cast<char>(b));
    char* escaped = curl_easy_escape(curl, one.data(), 1);
    REQUIRE(escaped != nullptr);
    CHECK(UrlEncode(one) == escaped);
    curl_free(escaped);
    all += one;
  }
  char* escapedAll = curl_easy_escape(curl, all.data(), static_cast<int>(all.size()));
  REQUIRE(escapedAll != nullptr);
  CHECK(UrlEncode(all) == escapedAll);
  curl_free(escapedAll);
  curl_easy_cleanup(curl);
}
