#pragma once

#include <cstddef>
#include <string>

namespace dispatcharr
{

// The Unicode-aware text primitives Dispatcharr's own series-rule matching
// needs, and this addon's ASCII-only ToLower()/StripAsciiWhitespaceLikePython()
// (StringUtil.h, SeriesRuleMatching.cpp) don't cover -- see
// docs/OPEN_ITEMS.md's two series-rule title-matching entries. Everything
// here is UTF-8 in, UTF-8 out, with no locale dependency at all: the case
// table is a fixed, generated snapshot (tools/gen_unicode_tables.py), not a
// call into whatever `towupper()` this process's C library happens to have,
// which differs across Kodi's platforms (and is wrong on most of them
// unless setlocale() was called).
//
// A byte that isn't part of a valid UTF-8 sequence is never an error: every
// function treats it as an opaque character of its own and passes it through
// unchanged, so two strings containing the same invalid bytes still compare
// equal after folding.

// The simple uppercase mapping of every code point, applied to a whole
// string -- for comparing two strings case-insensitively the way a
// PostgreSQL `UPPER(a) = UPPER(b)` does (Django's `iexact`; `icontains` is
// `UPPER(a) LIKE UPPER(...)`). Confirmed live that the real instance's
// database folds non-ASCII case (an `icontains` for "ünder" matched a
// channel named "ÜNDER"), so a rule titled "ZZZ ÜNDER TEST" must link to a
// recording titled "ZZZ ünder test" even though the server -- not this
// addon -- is what scheduled it. Simple, not full, case mapping (PostgreSQL
// maps one code point at a time): U+00DF stays as it is rather than
// becoming "SS", and the Greek letters with prosgegrammeni map to their
// single-code-point titlecase forms.
std::string FoldCaseForDatabaseMatch(const std::string& utf8);

// Python's str.strip() with no arguments: removes the leading and trailing
// characters str.isspace() accepts -- the ASCII whitespace and \x1c-\x1f
// separators, but also U+0085, U+00A0 (no-break space), U+1680,
// U+2000-U+200A, U+2028/2029, U+202F, U+205F and U+3000.
// Dispatcharr's own series-rule evaluation strips a rule's title and
// description with exactly that (`(rule.get("title") or "").strip()`), while
// the rule is *stored* unstripped, so a trailing no-break space survives in
// the API response and must be removed here to line up with what the server
// actually matched.
std::string StripUnicodeWhitespace(const std::string& utf8);
std::string StripLeadingUnicodeWhitespace(const std::string& utf8);
std::string StripTrailingUnicodeWhitespace(const std::string& utf8);

// Whether a code point counts as a "word character" for PostgreSQL's
// regex word-boundary assertion (`\y`): alphanumeric or an underscore. The
// alphanumeric set is glibc's `iswalnum()`, the function PostgreSQL uses on
// a libc-collation database -- not Python's `isalnum()`, which disagrees on
// a couple of thousand code points.
bool IsDatabaseWordCodePoint(char32_t codePoint);

// Decodes the code point starting at byte `pos` of `s`, setting `length` to
// the number of bytes it occupies. An invalid or truncated sequence decodes
// as a single byte -- returned as a value above U+10FFFF, so it never
// collides with a real code point -- with `length` 1.
char32_t DecodeUtf8At(const std::string& s, std::size_t pos, std::size_t& length);

// Decodes the code point that ends just before byte `end` of `s` (`end`
// must be greater than zero), with the same invalid-sequence rule.
char32_t DecodeUtf8Before(const std::string& s, std::size_t end, std::size_t& length);

} // namespace dispatcharr
