#include "UrlEncode.h"

#include <catch2/catch_test_macros.hpp>

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
