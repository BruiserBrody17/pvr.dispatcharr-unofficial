#include "Sha1.h"

#include <cstring>
#include <vector>

namespace dispatcharr
{

namespace
{

inline uint32_t RotateLeft(uint32_t value, unsigned bits)
{
  return (value << bits) | (value >> (32 - bits));
}

void ProcessBlock(const uint8_t* block, uint32_t state[5])
{
  uint32_t w[80];
  for (int i = 0; i < 16; ++i)
  {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i)
    w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int i = 0; i < 80; ++i)
  {
    uint32_t f, k;
    if (i < 20)
    {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    }
    else if (i < 40)
    {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    }
    else if (i < 60)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = RotateLeft(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

} // namespace

std::array<uint8_t, 20> Sha1(const uint8_t* data, size_t length)
{
  uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

  // Whole 64-byte blocks straight from the input, then the tail plus padding
  // (0x80, zeros, the 64-bit big-endian bit length) in a scratch buffer.
  size_t offset = 0;
  while (length - offset >= 64)
  {
    ProcessBlock(data + offset, state);
    offset += 64;
  }

  std::vector<uint8_t> tail(data + offset, data + length);
  tail.push_back(0x80);
  while (tail.size() % 64 != 56)
    tail.push_back(0x00);
  const uint64_t bitLength = static_cast<uint64_t>(length) * 8;
  for (int shift = 56; shift >= 0; shift -= 8)
    tail.push_back(static_cast<uint8_t>(bitLength >> shift));
  for (size_t i = 0; i < tail.size(); i += 64)
    ProcessBlock(tail.data() + i, state);

  std::array<uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i)
  {
    digest[i * 4] = static_cast<uint8_t>(state[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i]);
  }
  return digest;
}

std::array<uint8_t, 20> Sha1(const std::string& data)
{
  return Sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace dispatcharr
