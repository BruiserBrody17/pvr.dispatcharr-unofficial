#include "StringUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
std::string EncodeStr(const std::string& s)
{
  return Base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
} // namespace

// RFC 4648 section 10's own standard test vectors.
TEST_CASE("Base64Encode matches RFC 4648's test vectors", "[StringUtil]")
{
  CHECK(EncodeStr("") == "");
  CHECK(EncodeStr("f") == "Zg==");
  CHECK(EncodeStr("fo") == "Zm8=");
  CHECK(EncodeStr("foo") == "Zm9v");
  CHECK(EncodeStr("foob") == "Zm9vYg==");
  CHECK(EncodeStr("fooba") == "Zm9vYmE=");
  CHECK(EncodeStr("foobar") == "Zm9vYmFy");
}

TEST_CASE("Base64Encode handles arbitrary binary bytes", "[StringUtil]")
{
  const uint8_t data[] = {0x00, 0xFF, 0x10, 0x80, 0x7F};
  CHECK(Base64Encode(data, sizeof(data)) == "AP8QgH8=");
}

TEST_CASE("ToLower lowercases ASCII letters, leaves other characters alone", "[StringUtil]")
{
  CHECK(ToLower("Content-Type") == "content-type");
  CHECK(ToLower("ALREADY-LOWER-ISH-123") == "already-lower-ish-123");
  CHECK(ToLower("") == "");
  CHECK(ToLower("no change") == "no change");
}
