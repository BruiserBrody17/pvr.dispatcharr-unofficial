#include "SeriesRuleTextMatch.h"

#include "UnicodeText.h"

#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dispatcharr
{

namespace
{

// Django's Q objects as parse_text_query() builds them: a value, or -- for an
// empty operand -- an *empty* Q, which drops out of any AND/OR it is combined
// into rather than meaning "everything" (Q._combine(): `if not self: return
// other`). nullopt is that empty Q.
using Condition = std::optional<bool>;

Condition Combine(const Condition& left, const Condition& right, char op)
{
  if (!left)
    return right;
  if (!right)
    return left;
  return op == '&' ? (*left && *right) : (*left || *right);
}

char AsciiUpper(char c)
{
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

// Whether `needle` (upper-case ASCII) sits at `position` in `haystack`, comparing ASCII letters
// case-insensitively.
bool MatchesAsciiCaseInsensitiveAt(const std::string& haystack, std::size_t position, const std::string& needle)
{
  if (position + needle.size() > haystack.size())
    return false;
  for (std::size_t k = 0; k < needle.size(); ++k)
  {
    if (AsciiUpper(haystack[position + k]) != needle[k])
      return false;
  }
  return true;
}

bool EndsWithAsciiCaseInsensitive(const std::string& s, const std::string& suffix)
{
  if (suffix.size() > s.size())
    return false;
  const std::size_t offset = s.size() - suffix.size();
  for (std::size_t k = 0; k < suffix.size(); ++k)
  {
    if (AsciiUpper(s[offset + k]) != suffix[k])
      return false;
  }
  return true;
}

bool StartsWithAsciiCaseInsensitive(const std::string& s, const std::string& prefix)
{
  if (prefix.size() > s.size())
    return false;
  for (std::size_t k = 0; k < prefix.size(); ++k)
  {
    if (AsciiUpper(s[k]) != prefix[k])
      return false;
  }
  return true;
}

// `re.sub(r'"([^"]*)"', replacer, raw)`: each complete pair of quotes becomes
// a placeholder key `\x00P<n>\x00`, left to right; an unmatched quote stays
// as it is.
std::string ExtractQuotedPhrases(const std::string& raw, std::map<std::string, std::string>& phrases)
{
  std::string out;
  std::size_t pos = 0;
  while (true)
  {
    const std::size_t open = raw.find('"', pos);
    const std::size_t close = open == std::string::npos ? std::string::npos : raw.find('"', open + 1);
    if (close == std::string::npos)
    {
      out.append(raw, pos, std::string::npos);
      break;
    }
    out.append(raw, pos, open - pos);
    std::string key;
    key.push_back('\0');
    key.push_back('P');
    key += std::to_string(phrases.size());
    key.push_back('\0');
    phrases[key] = raw.substr(open + 1, close - open - 1);
    out += key;
    pos = close + 1;
  }
  return out;
}

bool HasWordBoundaryAt(const std::string& s, std::size_t position)
{
  bool previousIsWord = false;
  bool nextIsWord = false;
  std::size_t length = 0;
  if (position > 0)
    previousIsWord = IsDatabaseWordCodePoint(DecodeUtf8Before(s, position, length));
  if (position < s.size())
    nextIsWord = IsDatabaseWordCodePoint(DecodeUtf8At(s, position, length));
  return previousIsWord != nextIsWord;
}

// One parse_text_query() call: the phrases pulled out of the raw value, the
// programme text already case-folded, and whether each term must be a whole
// word.
class TextQuery
{
public:
  TextQuery(const std::map<std::string, std::string>& phrases, const std::string& foldedText, bool wholeWord)
      : m_phrases(phrases), m_foldedText(foldedText), m_wholeWord(wholeWord)
  {
  }

  bool OverBudget() const
  {
    return m_overBudget;
  }

  Condition ParseExpression(const std::string& rawExpression)
  {
    const std::string expression = StripUnicodeWhitespace(rawExpression);

    if (expression.find('(') != std::string::npos)
      return ParseGrouped(expression);

    // One forward pass over `expression`, not a re-slice-and-rescan of a shrinking remainder per
    // operator: that was quadratic (a 4 KB "a AND a AND ..." query cost about 3 ms per call, 718 ms
    // for one rule against 300 recordings -- found by the 2026-10-04 fourth hardening sweep). The
    // earliest operator wins, and an operator is only recognised wholly at or after `start`, exactly
    // as when the remainder was sliced off ("a AND OR b" keeps "OR b" whole: its leading space went
    // with the AND).
    std::vector<std::string> tokens;
    std::vector<char> operators;
    std::size_t start = 0;
    while (start < expression.size())
    {
      std::size_t position = std::string::npos;
      char op = '&';
      std::size_t operatorLength = 5;
      for (std::size_t i = start; i < expression.size(); ++i)
      {
        if (expression[i] != ' ')
          continue;
        if (MatchesAsciiCaseInsensitiveAt(expression, i, " AND "))
        {
          position = i;
          break;
        }
        if (MatchesAsciiCaseInsensitiveAt(expression, i, " OR "))
        {
          position = i;
          op = '|';
          operatorLength = 4;
          break;
        }
      }
      if (position == std::string::npos)
      {
        tokens.push_back(StripUnicodeWhitespace(expression.substr(start)));
        break;
      }
      const std::string token = StripUnicodeWhitespace(expression.substr(start, position - start));
      if (!token.empty())
      {
        tokens.push_back(token);
        operators.push_back(op);
      }
      start = position + operatorLength;
    }
    if (tokens.empty())
      return std::nullopt;

    // Left to right with no precedence, so once the result so far is true before an OR or false before an
    // AND, the next operand cannot change it (an empty one drops out either way) and is not evaluated.
    Condition result = Leaf(tokens[0]);
    for (std::size_t i = 0; i < operators.size() && i + 1 < tokens.size(); ++i)
    {
      if (result && *result == (operators[i] == '|'))
        continue;
      result = Combine(result, Leaf(tokens[i + 1]), operators[i]);
    }
    return result;
  }

private:
  // `(` ... `)`: the *last* "(" and the first ")" after it, recursing into
  // what is inside and on either side. See MatchSeriesText()'s comment on
  // why that leaves a stray ")" behind for a nested group.
  Condition ParseGrouped(const std::string& expression)
  {
    const std::size_t open = expression.rfind('(');
    const std::size_t close = expression.find(')', open);
    if (close == std::string::npos)
      return std::nullopt;

    const Condition group = ParseExpression(expression.substr(open + 1, close - open - 1));

    std::string before = StripTrailingUnicodeWhitespace(expression.substr(0, open));
    std::string after = StripLeadingUnicodeWhitespace(expression.substr(close + 1));

    char beforeOp = '&';
    if (EndsWithAsciiCaseInsensitive(before, " AND"))
    {
      before = StripTrailingUnicodeWhitespace(before.substr(0, before.size() - 4));
    }
    else if (EndsWithAsciiCaseInsensitive(before, " OR"))
    {
      before = StripTrailingUnicodeWhitespace(before.substr(0, before.size() - 3));
      beforeOp = '|';
    }

    char afterOp = '&';
    if (StartsWithAsciiCaseInsensitive(after, "AND "))
    {
      after = StripLeadingUnicodeWhitespace(after.substr(4));
    }
    else if (StartsWithAsciiCaseInsensitive(after, "OR "))
    {
      after = StripLeadingUnicodeWhitespace(after.substr(3));
      afterOp = '|';
    }

    Condition result = group;
    if (!before.empty())
      result = Combine(ParseExpression(before), result, beforeOp);
    if (!after.empty())
      result = Combine(result, ParseExpression(after), afterOp);
    return result;
  }

  // build_q(): an operand that is exactly a phrase placeholder stands for the
  // phrase's text; anything else is the operand itself. A term's answer depends
  // only on its folded form, so each distinct one is searched for once per call.
  Condition Leaf(const std::string& token)
  {
    if (m_overBudget)
      return std::nullopt;
    const auto phrase = m_phrases.find(token);
    const std::string term = StripUnicodeWhitespace(phrase != m_phrases.end() ? phrase->second : token);
    if (term.empty())
      return std::nullopt;
    std::string foldedTerm = FoldCaseForDatabaseMatch(term);
    const auto known = m_answers.find(foldedTerm);
    if (known != m_answers.end())
      return known->second;
    const bool found = Find(foldedTerm);
    if (m_overBudget)
      return std::nullopt;
    m_answers.emplace(std::move(foldedTerm), found);
    return found;
  }

  // `text.find(term)` (and, for a whole word, every later occurrence until one sits on word boundaries)
  // written out, so that the work is charged where it is done: `find` stops at every position holding the
  // term's first byte and compares from there, which costs far more than the scan between them when that
  // byte is common, and more again for a long term whose prefix the text repeats.
  bool Find(const std::string& term)
  {
    const std::string& text = m_foldedText;
    const std::size_t candidateCost = kSeriesMatchCandidateCost + term.size() / 16;
    std::size_t from = 0;
    while (from < text.size())
    {
      const std::size_t hit = text.find(term[0], from);
      if (!Spend((hit == std::string::npos ? text.size() : hit + 1) - from))
        return false;
      if (hit == std::string::npos || term.size() > text.size() - hit)
        return false;
      if (!Spend(candidateCost))
        return false;
      if (text.compare(hit, term.size(), term) == 0 &&
          (!m_wholeWord || (HasWordBoundaryAt(text, hit) && HasWordBoundaryAt(text, hit + term.size()))))
        return true;
      from = hit + 1;
    }
    return false;
  }

  bool Spend(std::size_t units)
  {
    if (units > kMaxSeriesMatchWork - m_work)
    {
      m_overBudget = true;
      return false;
    }
    m_work += units;
    return true;
  }

  const std::map<std::string, std::string>& m_phrases;
  const std::string& m_foldedText;
  bool m_wholeWord;
  std::unordered_map<std::string, bool> m_answers;
  std::size_t m_work = 0;
  bool m_overBudget = false;
};

std::string LowerAscii(const std::string& s)
{
  std::string out = s;
  for (char& c : out)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  }
  return out;
}

} // namespace

SeriesTextMode ParseSeriesTextMode(const std::string& rawMode, bool isTitle)
{
  const std::string mode = LowerAscii(rawMode);
  if (mode.empty())
    return isTitle ? SeriesTextMode::kExact : SeriesTextMode::kContains;
  if (mode == "regex")
    return SeriesTextMode::kRegex;
  if (mode == "search")
    return SeriesTextMode::kSearch;
  if (isTitle && mode == "exact")
    return SeriesTextMode::kExact;
  return SeriesTextMode::kContains;
}

SeriesTextMatch MatchSeriesText(SeriesTextMode mode, const std::string& query, const std::string& text)
{
  const std::string stripped = StripUnicodeWhitespace(query);
  if (stripped.empty())
    return SeriesTextMatch::kMatches;

  switch (mode)
  {
  case SeriesTextMode::kRegex:
    return SeriesTextMatch::kUnsupported;
  case SeriesTextMode::kExact:
    return FoldCaseForDatabaseMatch(text) == FoldCaseForDatabaseMatch(stripped) ? SeriesTextMatch::kMatches
                                                                                : SeriesTextMatch::kNoMatch;
  case SeriesTextMode::kContains:
  case SeriesTextMode::kSearch:
    break;
  }

  std::map<std::string, std::string> phrases;
  // Bounds found by the 2026-10-04 second hardening sweep, proven with the real sources: ParseExpression()
  // and ParseGrouped() recurse once per parenthesised group, so about 25,000 "(a)" groups (75 KB) overran
  // an 8 MB stack and about 3,000 (9 KB) a 1 MB one, and the AND/OR loop re-slices its remainder at every
  // operator, 48 s for a 600 KB query. A rule can be made in Dispatcharr's own web UI or API with no length
  // limit and this runs on Kodi's thread for each of its recordings, so one hostile or runaway title
  // crashed or hung every client. The upstream parser fails at a few hundred nested groups anyway
  // (RecursionError), so nothing a real rule needs is lost; a query past either bound is left unlinked,
  // like a regex.
  if (stripped.size() > kMaxSeriesQueryBytes)
    return SeriesTextMatch::kUnsupported;
  const std::string processed = ExtractQuotedPhrases(stripped, phrases);
  std::size_t groupCount = 0;
  for (char c : processed)
    groupCount += (c == '(');
  if (groupCount > kMaxSeriesQueryGroups)
    return SeriesTextMatch::kUnsupported;
  const std::string foldedText = FoldCaseForDatabaseMatch(text);
  TextQuery textQuery(phrases, foldedText, mode == SeriesTextMode::kSearch);
  const Condition condition = textQuery.ParseExpression(processed);
  if (textQuery.OverBudget())
    return SeriesTextMatch::kUnsupported;
  // An expression that is empty all the way down filters nothing.
  return (!condition || *condition) ? SeriesTextMatch::kMatches : SeriesTextMatch::kNoMatch;
}

} // namespace dispatcharr
