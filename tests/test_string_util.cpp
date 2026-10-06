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

TEST_CASE("FormatHostForUrl leaves a hostname unchanged", "[StringUtil]")
{
  CHECK(FormatHostForUrl("dispatcharr.example") == "dispatcharr.example");
}

TEST_CASE("FormatHostForUrl leaves an IPv4 literal unchanged", "[StringUtil]")
{
  CHECK(FormatHostForUrl("192.168.1.10") == "192.168.1.10");
}

TEST_CASE("FormatHostForUrl brackets a bare IPv6 literal", "[StringUtil]")
{
  CHECK(FormatHostForUrl("::1") == "[::1]");
  CHECK(FormatHostForUrl("2001:db8::1") == "[2001:db8::1]");
}

TEST_CASE("FormatHostForUrl does not double-bracket an already-bracketed literal", "[StringUtil]")
{
  CHECK(FormatHostForUrl("[::1]") == "[::1]");
}

TEST_CASE("FormatHostForUrl leaves an empty host unchanged", "[StringUtil]")
{
  CHECK(FormatHostForUrl("") == "");
}

TEST_CASE("AsciiToLower and IsAsciiAlnum change and accept only ASCII letters and digits, in any locale",
          "[StringUtil]")
{
  for (int c = 0; c < 256; ++c)
  {
    const char expected = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
    CHECK(AsciiToLower(static_cast<unsigned char>(c)) == expected);
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    CHECK(IsAsciiAlnum(static_cast<unsigned char>(c)) == alnum);
  }
  // Bytes of a UTF-8 sequence are never altered or taken for letters.
  CHECK(ToLower("\xC3\x89"
                "I") == "\xC3\x89"
                        "i");
  CHECK_FALSE(IsAsciiAlnum(0xC3));
}
