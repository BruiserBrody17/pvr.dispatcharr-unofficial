#include "Sha1.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace dispatcharr;

namespace
{
std::string Hex(const std::array<uint8_t, 20>& digest)
{
  std::string out;
  char buf[3];
  for (uint8_t byte : digest)
  {
    std::snprintf(buf, sizeof(buf), "%02x", byte);
    out += buf;
  }
  return out;
}
} // namespace

// The published vectors (FIPS 180 / RFC 3174).
TEST_CASE("Sha1 of the empty string", "[Sha1]")
{
  CHECK(Hex(Sha1(std::string())) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST_CASE("Sha1 of abc", "[Sha1]")
{
  CHECK(Hex(Sha1(std::string("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("Sha1 of the two-block FIPS vector", "[Sha1]")
{
  CHECK(Hex(Sha1(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("Sha1 of a million a's", "[Sha1]")
{
  CHECK(Hex(Sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// Lengths around the 55/56/63/64-byte padding boundaries are where hand-rolled SHA-1 goes wrong (the
// length field spilling into an extra block); expected digests are hashlib's, over bytes (i*7+3) % 256.
TEST_CASE("Sha1 agrees with a reference implementation across the padding boundaries", "[Sha1]")
{
  struct Case
  {
    size_t length;
    const char* digest;
  };
  const Case cases[] = {
      {1, "9842926af7ca0a8cca12604f945414f07b01e13d"},    {55, "ddf57317ef34bfee3b6df83d359098930eb278bc"},
      {56, "a0d492bb0fc889d0eca3bc137066ab6f4f74f369"},   {57, "11a02dcf95859677a62e75024067c22b165d890f"},
      {63, "c55856749bef509bdfe6bfebfc7bf4e793e82132"},   {64, "bede92be29c3874e1b54ddc77988d606fc857a8e"},
      {65, "b05a80522b053d6dc7e0a517d0e70212c7dad11f"},   {119, "504e27376a6e0f0dba8295b85cb25dc4dfa17d23"},
      {120, "82134b02fb3f702491be9bed581eeab59334acb2"},  {128, "a09133e6730ffe899efb70204cb5646cd5dc24ee"},
      {1000, "4231a8a50a10fa9758db8ec71fdef855b751048a"},
  };
  for (const Case& c : cases)
  {
    std::vector<uint8_t> data(c.length);
    for (size_t i = 0; i < c.length; ++i)
      data[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    INFO("length " << c.length);
    CHECK(Hex(Sha1(data.data(), data.size())) == c.digest);
  }
}
