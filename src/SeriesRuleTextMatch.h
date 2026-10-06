#pragma once

#include <cstddef>
#include <string>

namespace dispatcharr
{

// How a series rule's `title`/`description` is compared with a programme's,
// mirroring Dispatcharr's own evaluation (`_evaluate_series_rules_locked()`,
// apps/channels/tasks.py, confirmed against its real 0.31.0 source):
//
//   title_mode == "exact"  -> `title__iexact=<title>`
//   anything else          -> `parse_text_query("title", <title>,
//                                use_regex=(mode == "regex"),
//                                whole_words=(mode == "search"))`
//
// `description` is always run through `parse_text_query` (there is no
// "exact" for it, so `description_mode == "exact"` behaves as "contains"),
// and a rule's two filters are ANDed. The mode string is lowercased, and a
// missing or empty one means "exact" for the title and "contains" for the
// description. The default -- and so "contains", and any mode name the
// server doesn't recognise -- is a case-insensitive substring match: the
// *only* two modes that aren't are "regex" and "search".
enum class SeriesTextMode
{
  kExact,    // the whole string, compared case-insensitively
  kContains, // a case-insensitive substring, with the AND/OR/quote/parenthesis grammar below
  kSearch,   // the same grammar, but each term must sit on word boundaries
  kRegex,    // a PostgreSQL regular expression -- not evaluated here, see kUnsupported
};

// `isTitle` picks the default for a missing/empty mode (see above).
SeriesTextMode ParseSeriesTextMode(const std::string& rawMode, bool isTitle);

// The most a contains/search query may hold before MatchSeriesText() declines it: bytes (after the
// strip) and "(" groups (outside quotes). See MatchSeriesText()'s own comment for what these guard.
constexpr std::size_t kMaxSeriesQueryBytes = 4096;
constexpr std::size_t kMaxSeriesQueryGroups = 64;

enum class SeriesTextMatch
{
  kMatches,
  kNoMatch,
  // The filter uses something this addon can't evaluate faithfully -- a regex,
  // whose PostgreSQL syntax isn't ECMAScript's, or a query past the size and
  // nesting bounds below. A rule that
  // depends on one is left unlinked, exactly as every non-exact rule used to
  // be, rather than guessed at.
  kUnsupported,
};

// Whether `text` satisfies the rule filter `query` under `mode`, the way the
// server's database would. `query` is stripped of Python-style whitespace
// first (the server does `.strip()` on both the title and description before
// using them); an empty query is no filter at all and matches everything --
// it is the caller's job to reject a rule whose title *and* description are
// both empty, which the server treats as invalid and never evaluates.
//
// kContains / kSearch port `parse_text_query()` (apps/epg/query_utils.py)
// faithfully, quirks included, because the point is to agree with what the
// server actually scheduled, not with what its documentation intends:
//   - AND and OR are matched case-insensitively, but only as a whole
//     " AND " / " OR " with a space on each side, and are applied strictly
//     left to right -- there is no precedence, so `a OR b AND c` is
//     `(a OR b) AND c`.
//   - A double-quoted phrase is atomic, but only when it makes up an entire
//     operand (`"a b" AND c`); a quote inside a longer operand is ordinary
//     text. An unmatched quote is ordinary text too.
//   - Parentheses group, but the parser takes the *last* "(" and the first
//     ")" after it, so a nested group leaves a stray ")" behind that then
//     has to match as text -- which a real title essentially never does. An
//     unclosed "(" makes the whole filter empty (matches everything).
//   - An empty operand is neutral, not "match everything": it drops out of
//     the AND/OR it sits in, and only an expression that is empty all the
//     way down matches everything.
//   - kSearch anchors each term with `\y`, which is a true word boundary
//     (a word character on exactly one side), not "surrounded by spaces": a
//     term that starts or ends in a non-word character, such as "C++", can
//     only match where a word character follows it.
// One divergence it knows about and does not try to reproduce: the server
// finds the operators in `text.upper()` and then slices the *original* text
// at those positions, so a query containing a character whose uppercase form
// is longer (the German sharp s) ahead of an operator splits in the wrong
// place there. A rule like that matches nothing useful on the server either.
SeriesTextMatch MatchSeriesText(SeriesTextMode mode, const std::string& query, const std::string& text);

} // namespace dispatcharr
