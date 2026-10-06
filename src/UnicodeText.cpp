#include "UnicodeText.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace dispatcharr
{

namespace
{

struct UpperRange
{
  char32_t first;
  char32_t last;
  std::int32_t delta;
  unsigned stride;
};

struct CodePointRange
{
  char32_t first;
  char32_t last;
};

// kUpperRanges / kAlnumRanges -- generated, see tools/gen_unicode_tables.py.
#include "UnicodeTables.inc"

// Above every real code point, so a byte that isn't part of a valid UTF-8
// sequence can be carried around as a code point of its own without ever
// colliding with a real one.
constexpr char32_t kInvalidByteBase = 0x110000;

char32_t SimpleUpper(char32_t cp)
{
  if (cp < 0x80)
    return (cp >= 'a' && cp <= 'z') ? cp - ('a' - 'A') : cp;
  if (cp > 0x10FFFF)
    return cp;
  // The last range that starts at or before cp. The generator only builds a
  // range out of consecutive mapped code points, so no other range's entries
  // can fall inside one range's span.
  const UpperRange* const begin = std::begin(kUpperRanges);
  const UpperRange* it =
      std::upper_bound(begin, std::end(kUpperRanges), cp, [](char32_t v, const UpperRange& r) { return v < r.first; });
  if (it == begin)
    return cp;
  --it;
  if (cp > it->last || (cp - it->first) % it->stride != 0)
    return cp;
  return static_cast<char32_t>(static_cast<std::int64_t>(cp) + it->delta);
}

void AppendUtf8(std::string& out, char32_t cp)
{
  if (cp < 0x80)
  {
    out.push_back(static_cast<char>(cp));
  }
  else if (cp < 0x800)
  {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000)
  {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  else
  {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// str.isspace() -- confirmed against Python directly (every code point for
// which it answers True).
bool IsPythonWhitespace(char32_t cp)
{
  if (cp >= 0x09 && cp <= 0x0D)
    return true;
  if (cp >= 0x1C && cp <= 0x1F)
    return true;
  if (cp >= 0x2000 && cp <= 0x200A)
    return true;
  switch (cp)
  {
  case 0x20:
  case 0x85:
  case 0xA0:
  case 0x1680:
  case 0x2028:
  case 0x2029:
  case 0x202F:
  case 0x205F:
  case 0x3000:
    return true;
  default:
    return false;
  }
}

} // namespace

char32_t DecodeUtf8At(const std::string& s, std::size_t pos, std::size_t& length)
{
  const unsigned char c0 = static_cast<unsigned char>(s[pos]);
  length = 1;
  if (c0 < 0x80)
    return c0;

  // The continuation byte's six payload bits, or -1 if there isn't one there.
  auto continuation = [&](std::size_t offset) -> int
  {
    if (pos + offset >= s.size())
      return -1;
    const unsigned char c = static_cast<unsigned char>(s[pos + offset]);
    return (c & 0xC0) == 0x80 ? (c & 0x3F) : -1;
  };
  const char32_t invalid = kInvalidByteBase + c0;

  if (c0 >= 0xC2 && c0 <= 0xDF)
  {
    const int b1 = continuation(1);
    if (b1 < 0)
      return invalid;
    length = 2;
    return (static_cast<char32_t>(c0 & 0x1F) << 6) | static_cast<char32_t>(b1);
  }
  if (c0 >= 0xE0 && c0 <= 0xEF)
  {
    const int b1 = continuation(1);
    const int b2 = continuation(2);
    if (b1 < 0 || b2 < 0)
      return invalid;
    const char32_t cp =
        (static_cast<char32_t>(c0 & 0x0F) << 12) | (static_cast<char32_t>(b1) << 6) | static_cast<char32_t>(b2);
    // Overlong, or a UTF-16 surrogate half -- neither is a scalar value.
    if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))
      return invalid;
    length = 3;
    return cp;
  }
  if (c0 >= 0xF0 && c0 <= 0xF4)
  {
    const int b1 = continuation(1);
    const int b2 = continuation(2);
    const int b3 = continuation(3);
    if (b1 < 0 || b2 < 0 || b3 < 0)
      return invalid;
    const char32_t cp = (static_cast<char32_t>(c0 & 0x07) << 18) | (static_cast<char32_t>(b1) << 12) |
                        (static_cast<char32_t>(b2) << 6) | static_cast<char32_t>(b3);
    if (cp < 0x10000 || cp > 0x10FFFF)
      return invalid;
    length = 4;
    return cp;
  }
  return invalid;
}

char32_t DecodeUtf8Before(const std::string& s, std::size_t end, std::size_t& length)
{
  const unsigned char last = static_cast<unsigned char>(s[end - 1]);
  length = 1;
  if (last < 0x80)
    return last;
  std::size_t start = end - 1;
  while (start > 0 && (end - start) < 4 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80)
    --start;
  std::size_t decodedLength = 0;
  const char32_t cp = DecodeUtf8At(s, start, decodedLength);
  if (start + decodedLength == end && cp < kInvalidByteBase)
  {
    length = decodedLength;
    return cp;
  }
  return kInvalidByteBase + last;
}

std::string FoldCaseForDatabaseMatch(const std::string& utf8)
{
  std::string out;
  out.reserve(utf8.size());
  std::size_t pos = 0;
  while (pos < utf8.size())
  {
    std::size_t length = 0;
    const char32_t cp = DecodeUtf8At(utf8, pos, length);
    if (cp >= kInvalidByteBase)
      out.push_back(utf8[pos]);
    else
      AppendUtf8(out, SimpleUpper(cp));
    pos += length;
  }
  return out;
}

std::string StripLeadingUnicodeWhitespace(const std::string& utf8)
{
  std::size_t pos = 0;
  while (pos < utf8.size())
  {
    std::size_t length = 0;
    if (!IsPythonWhitespace(DecodeUtf8At(utf8, pos, length)))
      break;
    pos += length;
  }
  return utf8.substr(pos);
}

std::string StripTrailingUnicodeWhitespace(const std::string& utf8)
{
  std::size_t end = utf8.size();
  while (end > 0)
  {
    std::size_t length = 0;
    if (!IsPythonWhitespace(DecodeUtf8Before(utf8, end, length)))
      break;
    end -= length;
  }
  return utf8.substr(0, end);
}

std::string StripUnicodeWhitespace(const std::string& utf8)
{
  return StripTrailingUnicodeWhitespace(StripLeadingUnicodeWhitespace(utf8));
}

bool IsDatabaseWordCodePoint(char32_t codePoint)
{
  if (codePoint < 0x80)
    return codePoint == '_' || (codePoint >= '0' && codePoint <= '9') || (codePoint >= 'A' && codePoint <= 'Z') ||
           (codePoint >= 'a' && codePoint <= 'z');
  if (codePoint > 0x10FFFF)
    return false;
  const CodePointRange* const begin = std::begin(kAlnumRanges);
  const CodePointRange* it = std::upper_bound(begin, std::end(kAlnumRanges), codePoint,
                                              [](char32_t v, const CodePointRange& r) { return v < r.first; });
  if (it == begin)
    return false;
  --it;
  return codePoint <= it->last;
}

} // namespace dispatcharr
