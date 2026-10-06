#include "SeriesRuleTextMatch.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace dispatcharr;

namespace
{

struct TextMatchCase
{
  SeriesTextMode mode;
  const char* query;
  const char* text;
  SeriesTextMatch expected;
};

// Every expected value below was produced by running the query through
// Dispatcharr's own parse_text_query() (apps/epg/query_utils.py, 0.31.0) and
// evaluating the resulting condition against the text with PostgreSQL's
// semantics -- not by reading the code and predicting. The same port was also
// checked against that parser on ~108,000 randomly generated query/text pairs
// (contains, whole-word and exact modes, with no mismatch), so these rows are
// a readable, permanent sample of it rather than its only evidence.
const TextMatchCase kCases[] = {
    // a bare term is a case-insensitive substring
    {SeriesTextMode::kContains, "news", "Evening NEWS at six", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "news", "Newsroom", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "bulletin", "Evening news", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "Evening News", "evening news tonight", SeriesTextMatch::kMatches},
    // AND needs both terms, in any order
    {SeriesTextMode::kContains, "news AND sport", "Sport and news tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "news AND sport", "Evening news", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "news and sport", "NEWS and SPORT", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a AND b AND c", "c b a", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a AND b AND c", "a b", SeriesTextMatch::kNoMatch},
    // OR needs either
    {SeriesTextMode::kContains, "news OR sport", "Evening news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "news OR sport", "Cooking", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "news or sport", "SPORT tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a OR b OR c", "only c here", SeriesTextMatch::kMatches},
    // operators apply strictly left to right -- there is no precedence
    {SeriesTextMode::kContains, "a OR b AND c", "a", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "a OR b AND c", "b c", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a OR b AND c", "c", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "a AND b OR c", "c", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a AND b OR c", "a", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "a AND b OR c", "a b", SeriesTextMatch::kMatches},
    // a quoted phrase is atomic, including an operator inside it
    {SeriesTextMode::kContains, "\"news AND sport\"", "the news AND sport hour", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\"news AND sport\"", "news sport", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "\"late news\"", "The Late News", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\"late news\"", "late breaking news", SeriesTextMatch::kNoMatch},
    // a quote only makes a phrase when it is a whole operand
    {SeriesTextMode::kContains, "breaking \"late news\"", "breaking late news", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "\"late news\" AND tonight", "Late News tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\"late news\" AND tonight", "Late News", SeriesTextMatch::kNoMatch},
    // an unmatched quote is ordinary text
    {SeriesTextMode::kContains, "\"news", "he said \"news tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\"news", "news tonight", SeriesTextMatch::kNoMatch},
    // parentheses group
    {SeriesTextMode::kContains, "(a OR b) AND c", "b c", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "(a OR b) AND c", "b", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "c AND (a OR b)", "c a", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "x AND (a OR b)", "x", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "(a AND b) OR c", "c", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "(a AND b) OR c", "a", SeriesTextMatch::kNoMatch},
    // an unclosed parenthesis empties the whole filter, which matches everything
    {SeriesTextMode::kContains, "a AND (b", "nothing relevant", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "(", "anything", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "x (", "x", SeriesTextMatch::kMatches},
    // a nested group leaves a stray ')' that then has to match as text
    {SeriesTextMode::kContains, "a AND (b OR (c AND d))", "a b c d", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "a AND (b OR (c AND d))", "a b c d )", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "x )", "x )", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, ")", "x", SeriesTextMatch::kNoMatch},
    // an empty operand is neutral, not match-everything
    {SeriesTextMode::kContains, "a AND \"\"", "a", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "a AND \"\"", "b", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "\"\"", "anything", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\"\" OR a", "b", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "()", "anything", SeriesTextMatch::kMatches},
    // surrounding and Unicode whitespace is stripped from the query and each operand
    {SeriesTextMode::kContains, "  news  ", "news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\xC2\xA0news\xC2\xA0", "news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains,
     "news\xC2\xA0"
     "AND sport",
     "news sport", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "news AND \xE3\x80\x80sport", "sport news", SeriesTextMatch::kMatches},
    // a term matches across Unicode case and accents are kept
    {SeriesTextMode::kContains, "\xC3\xBCnder", "\xC3\x9CNDER TEST", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\xC3\x9CNDER", "a \xC3\xBCnder test", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "cafe", "caf\xC3\xA9", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "caf\xC3\xA9", "CAF\xC3\x89", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\xD0\x9C\xD0\x9E\xD0\xA1\xD0\x9A\xD0\x92\xD0\x90",
     "\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0 \xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xBD\xD1\x8F",
     SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\xC7\x85", "\xC7\x86", SeriesTextMatch::kMatches},
    // whole-word mode anchors each term on a word boundary
    {SeriesTextMode::kSearch, "news", "Evening news tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "news", "Newsroom", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "new", "Newsroom", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "news", "Breaking-news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "news", "news_today", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "news", "news2", SeriesTextMatch::kNoMatch},
    // whole-word mode keeps the same AND/OR/phrase grammar
    {SeriesTextMode::kSearch, "news AND sport", "sport and news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "news AND sport", "newsroom sport", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "news OR sport", "sports", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "\"late news\"", "The Late News tonight", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "\"late news\"", "the late newsroom", SeriesTextMatch::kNoMatch},
    // a term that starts or ends in a non-word character needs a word character beside it
    {SeriesTextMode::kSearch, "C++", "C++", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "C++", "C++x", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "C++", "learn C++ now", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "-a", "-a", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "-a", "x-a", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "(c)", "(c)", SeriesTextMatch::kMatches},
    // whole-word mode treats accented letters as word characters
    {SeriesTextMode::kSearch, "caf\xC3\xA9", "caf\xC3\xA9", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "caf", "caf\xC3\xA9", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kSearch, "caf\xC3\xA9", "un caf\xC3\xA9 noir", SeriesTextMatch::kMatches},
    {SeriesTextMode::kSearch, "\xC3\xBCnder", "\xC3\x9CNDER", SeriesTextMatch::kMatches},
    // exact mode compares the whole string
    {SeriesTextMode::kExact, "Evening News", "evening news", SeriesTextMatch::kMatches},
    {SeriesTextMode::kExact, "Evening News", "Evening News Extra", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kExact, "Evening", "Evening News", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kExact, "\xC3\x9CNDER", "\xC3\xBCnder", SeriesTextMatch::kMatches},
    {SeriesTextMode::kExact, "  Evening News  ", "Evening News", SeriesTextMatch::kMatches},
    {SeriesTextMode::kExact, "Evening News\xC2\xA0", "Evening News", SeriesTextMatch::kMatches},
    {SeriesTextMode::kExact,
     "stra\xC3\x9F"
     "e",
     "STRA\xC3\x9F"
     "E",
     SeriesTextMatch::kMatches},
    {SeriesTextMode::kExact,
     "stra\xC3\x9F"
     "e",
     "STRASSE", SeriesTextMatch::kNoMatch},
    // an empty filter matches everything
    {SeriesTextMode::kContains, "", "anything", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "   ", "anything", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "\xC2\xA0", "anything", SeriesTextMatch::kMatches},
    // a regex filter is not evaluated
    {SeriesTextMode::kRegex, "^news", "news tonight", SeriesTextMatch::kUnsupported},
    {SeriesTextMode::kRegex, "x", "y", SeriesTextMatch::kUnsupported},
    // an OR directly before a group applies to the group (found by the ninth sweep's mutation pass)
    {SeriesTextMode::kContains, "x OR (y)", "y", SeriesTextMatch::kMatches},
    {SeriesTextMode::kContains, "x OR (y)", "z", SeriesTextMatch::kNoMatch},
    // the term before an AND-group keeps its first character
    {SeriesTextMode::kContains, "news AND (sport)", "ews sport", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "news AND (sport)", "news sport", SeriesTextMatch::kMatches},
    // an operator the text ends in the middle of stays literal text
    {SeriesTextMode::kContains, "rock and", "rock music", SeriesTextMatch::kNoMatch},
    {SeriesTextMode::kContains, "rock and", "rock and roll", SeriesTextMatch::kMatches},
};

} // namespace

TEST_CASE("MatchSeriesText agrees with Dispatcharr's own query parser and database on the curated cases",
          "[SeriesRuleTextMatch]")
{
  for (const TextMatchCase& c : kCases)
  {
    INFO("mode=" << static_cast<int>(c.mode) << " query=[" << c.query << "] text=[" << c.text << "]");
    CHECK(MatchSeriesText(c.mode, c.query, c.text) == c.expected);
  }
}

TEST_CASE("ParseSeriesTextMode defaults a missing title mode to exact and a missing description mode to contains",
          "[SeriesRuleTextMatch]")
{
  CHECK(ParseSeriesTextMode("", /*isTitle=*/true) == SeriesTextMode::kExact);
  CHECK(ParseSeriesTextMode("", /*isTitle=*/false) == SeriesTextMode::kContains);
}

TEST_CASE("ParseSeriesTextMode recognizes the modes the server does, case-insensitively", "[SeriesRuleTextMatch]")
{
  CHECK(ParseSeriesTextMode("exact", true) == SeriesTextMode::kExact);
  CHECK(ParseSeriesTextMode("EXACT", true) == SeriesTextMode::kExact);
  CHECK(ParseSeriesTextMode("contains", true) == SeriesTextMode::kContains);
  CHECK(ParseSeriesTextMode("Search", true) == SeriesTextMode::kSearch);
  CHECK(ParseSeriesTextMode("search", false) == SeriesTextMode::kSearch);
  CHECK(ParseSeriesTextMode("REGEX", true) == SeriesTextMode::kRegex);
  CHECK(ParseSeriesTextMode("regex", false) == SeriesTextMode::kRegex);
}

TEST_CASE("ParseSeriesTextMode treats any other mode name as a substring match, as the server does",
          "[SeriesRuleTextMatch]")
{
  // The server only special-cases "exact" (title only), "regex" and "search";
  // everything else -- a typo, a mode a newer Dispatcharr adds -- falls
  // through to parse_text_query()'s default.
  CHECK(ParseSeriesTextMode("fuzzy", true) == SeriesTextMode::kContains);
  CHECK(ParseSeriesTextMode("fuzzy", false) == SeriesTextMode::kContains);
  // A description has no "exact" of its own: it behaves as "contains".
  CHECK(ParseSeriesTextMode("exact", /*isTitle=*/false) == SeriesTextMode::kContains);
}

TEST_CASE("MatchSeriesText treats an empty or all-whitespace query as no filter at all", "[SeriesRuleTextMatch]")
{
  // The caller is responsible for rejecting a rule with neither a title nor
  // a description; a single empty filter just doesn't filter.
  for (SeriesTextMode mode :
       {SeriesTextMode::kExact, SeriesTextMode::kContains, SeriesTextMode::kSearch, SeriesTextMode::kRegex})
  {
    CHECK(MatchSeriesText(mode, "", "anything") == SeriesTextMatch::kMatches);
    CHECK(MatchSeriesText(mode, " \t\xC2\xA0", "anything") == SeriesTextMatch::kMatches);
  }
}

TEST_CASE("MatchSeriesText reports only a regex filter as unsupported", "[SeriesRuleTextMatch]")
{
  CHECK(MatchSeriesText(SeriesTextMode::kRegex, "^news", "news") == SeriesTextMatch::kUnsupported);
  CHECK(MatchSeriesText(SeriesTextMode::kExact, "news", "news") != SeriesTextMatch::kUnsupported);
  CHECK(MatchSeriesText(SeriesTextMode::kContains, "news", "x") != SeriesTextMatch::kUnsupported);
  CHECK(MatchSeriesText(SeriesTextMode::kSearch, "news", "x") != SeriesTextMatch::kUnsupported);
}

TEST_CASE("MatchSeriesText matches a text with invalid UTF-8 the same way it matches any other",
          "[SeriesRuleTextMatch]")
{
  CHECK(MatchSeriesText(SeriesTextMode::kContains, "news", "\xFF news \xFE") == SeriesTextMatch::kMatches);
  CHECK(MatchSeriesText(SeriesTextMode::kSearch, "news", "\xFFnews\xFE") == SeriesTextMatch::kMatches);
  CHECK(MatchSeriesText(SeriesTextMode::kExact, "a\xFFz", "A\xFFZ") == SeriesTextMatch::kMatches);
}

TEST_CASE("MatchSeriesText declines a query nested past the group bound instead of recursing without limit",
          "[SeriesRuleTextMatch]")
{
  // Unbounded, ~25,000 "(a)" groups overran an 8 MB stack and ~3,000 a 1 MB one.
  std::string deep;
  for (int i = 0; i < 100000; ++i)
    deep += "(a)";
  for (SeriesTextMode mode : {SeriesTextMode::kContains, SeriesTextMode::kSearch})
    CHECK(MatchSeriesText(mode, deep, "a show") == SeriesTextMatch::kUnsupported);

  std::string nested;
  for (std::size_t i = 0; i < kMaxSeriesQueryGroups + 1; ++i)
    nested += "(";
  nested += "a";
  for (std::size_t i = 0; i < kMaxSeriesQueryGroups + 1; ++i)
    nested += ")";
  CHECK(MatchSeriesText(SeriesTextMode::kContains, nested, "a show") == SeriesTextMatch::kUnsupported);
}

TEST_CASE("MatchSeriesText still evaluates a query exactly at the group bound", "[SeriesRuleTextMatch]")
{
  std::string query;
  for (std::size_t i = 0; i < kMaxSeriesQueryGroups; ++i)
    query += (i ? " OR (a)" : "(a)");
  CHECK(MatchSeriesText(SeriesTextMode::kContains, query, "a show") == SeriesTextMatch::kMatches);
  CHECK(MatchSeriesText(SeriesTextMode::kContains, query, "zzz") == SeriesTextMatch::kNoMatch);
}

TEST_CASE("MatchSeriesText does not count parentheses inside a quoted phrase toward the group bound",
          "[SeriesRuleTextMatch]")
{
  std::string phrase = "\"";
  for (std::size_t i = 0; i < kMaxSeriesQueryGroups * 3; ++i)
    phrase += "(";
  phrase += "\"";
  CHECK(MatchSeriesText(SeriesTextMode::kContains, phrase, "x") == SeriesTextMatch::kNoMatch);
}

TEST_CASE("MatchSeriesText declines an over-long query and finishes a long one near the bound quickly",
          "[SeriesRuleTextMatch]")
{
  // The operator loop re-sliced its remainder per operator: 48 s at 600 KB.
  std::string huge = "a";
  while (huge.size() <= kMaxSeriesQueryBytes)
    huge += " AND a";
  CHECK(MatchSeriesText(SeriesTextMode::kContains, huge, "a show") == SeriesTextMatch::kUnsupported);

  std::string nearBound = "a";
  while (nearBound.size() + 6 <= kMaxSeriesQueryBytes)
    nearBound += " AND a";
  const auto start = std::chrono::steady_clock::now();
  CHECK(MatchSeriesText(SeriesTextMode::kContains, nearBound, "a show") == SeriesTextMatch::kMatches);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

TEST_CASE("MatchSeriesText accepts a query of exactly the byte bound and declines one byte more",
          "[SeriesRuleTextMatch]")
{
  // "a" padded with trailing spaces would be stripped, so build the length from real terms.
  std::string exact = "a";
  while (exact.size() + 6 <= kMaxSeriesQueryBytes)
    exact += " AND a";
  while (exact.size() < kMaxSeriesQueryBytes)
    exact += "a"; // extend the last term
  REQUIRE(exact.size() == kMaxSeriesQueryBytes);
  CHECK(MatchSeriesText(SeriesTextMode::kContains, exact, "aaaa") != SeriesTextMatch::kUnsupported);
  CHECK(MatchSeriesText(SeriesTextMode::kContains, exact + "a", "aaaa") == SeriesTextMatch::kUnsupported);
}
