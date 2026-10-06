#include "JsonResponse.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace dispatcharr;

TEST_CASE("ParseJsonResponseBody parses an ordinary document", "[JsonResponse]")
{
  nlohmann::json out;
  std::string error;
  REQUIRE(ParseJsonResponseBody(R"({"results":[{"id":3,"name":"x"}],"next":null})", out, error));
  CHECK(out["results"][0]["id"] == 3);
  CHECK(error.empty());
}

TEST_CASE("ParseJsonResponseBody fails instead of throwing on malformed JSON", "[JsonResponse]")
{
  nlohmann::json out;
  std::string error;
  CHECK_FALSE(ParseJsonResponseBody("{\"a\":", out, error));
  CHECK(error.find("Failed to parse JSON response") == 0);
}

TEST_CASE("ParseJsonResponseBody fails instead of throwing on an integer beyond double range", "[JsonResponse]")
{
  // json::parse() throws out_of_range (406), not parse_error, for these; Request() used to catch only
  // parse_error, so a stored 10**400 in a JSON field reached std::terminate on a worker thread.
  const std::string huge(400, '9');
  const std::vector<std::string> bodies = {R"({"results":[{"custom_properties":{"x":)" + huge + R"(}}]})",
                                           "[-" + huge + "]", R"({"x":1e400})", R"({"x":-1e400})", huge};
  for (const std::string& body : bodies)
  {
    nlohmann::json out;
    std::string error;
    CHECK_NOTHROW(ParseJsonResponseBody(body, out, error));
    CHECK_FALSE(ParseJsonResponseBody(body, out, error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("ParseJsonResponseBody accepts the largest values a double can hold", "[JsonResponse]")
{
  nlohmann::json out;
  std::string error;
  CHECK(ParseJsonResponseBody(R"({"x":1e308,"y":18446744073709551615})", out, error));
}

TEST_CASE("SanitizeServerErrorBody keeps a short plain body as it is", "[JsonResponse]")
{
  CHECK(SanitizeServerErrorBody("{\"detail\":\"Not found.\"}") == "{\"detail\":\"Not found.\"}");
  CHECK(SanitizeServerErrorBody("") == "");
}

TEST_CASE("SanitizeServerErrorBody turns control characters into spaces", "[JsonResponse]")
{
  CHECK(SanitizeServerErrorBody("line1\nline2\r\n\ttab\x1b[31mred\x01\x7f") == "line1 line2   tab [31mred  ");
  std::string withNul = "a";
  withNul.push_back('\0');
  withNul += "b";
  CHECK(SanitizeServerErrorBody(withNul) == "a b");
}

TEST_CASE("SanitizeServerErrorBody cuts a long body at the limit and says how much was dropped", "[JsonResponse]")
{
  const std::string big(10000, 'x');
  const std::string out = SanitizeServerErrorBody(big);
  CHECK(out.substr(0, 512) == std::string(512, 'x'));
  CHECK(out.find("9488 more bytes not shown") != std::string::npos);
  CHECK(out.size() < 600);
  // Exactly at the limit: nothing is dropped, no note.
  CHECK(SanitizeServerErrorBody(std::string(512, 'y')) == std::string(512, 'y'));
  CHECK(SanitizeServerErrorBody(std::string(513, 'y')).find("1 more bytes") != std::string::npos);
}

TEST_CASE("SanitizeServerErrorBody never cuts a UTF-8 sequence in half", "[JsonResponse]")
{
  // 511 ASCII bytes then a two-byte character straddling the 512 limit: the whole character is dropped.
  std::string body(511, 'a');
  body += "\xC3\xA9"; // bytes 511 and 512
  body += "tail";
  const std::string out = SanitizeServerErrorBody(body);
  CHECK(out.substr(0, 511) == std::string(511, 'a'));
  CHECK(static_cast<unsigned char>(out[511]) != 0xC3); // no dangling lead byte
  // A four-byte character cut after its first byte, the same way.
  std::string wide(510, 'a');
  wide += "\xF0\x9F\x93\xBA";
  wide += "tail";
  const std::string wideOut = SanitizeServerErrorBody(wide);
  CHECK(wideOut.substr(0, 510) == std::string(510, 'a'));
  CHECK(wideOut.find('\xF0') == std::string::npos);
}

TEST_CASE("SanitizeServerErrorBody turns every control character, including 0x1E and 0x1F, into a space",
          "[JsonResponse]")
{
  CHECK(SanitizeServerErrorBody(std::string("a\x1f"
                                            "b\x1e"
                                            "c\x01"
                                            "d\x7f"
                                            "e")) == "a b c d e");
}
