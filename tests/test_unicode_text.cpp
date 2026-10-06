#include "UnicodeText.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

// Expected values below were taken from glibc's own towupper()/iswalnum(),
// the functions the tables were generated from, and the whole mapping was
// separately verified exhaustively against them for every code point --
// these tests pin the behavior a rule's title comparison depends on.

TEST_CASE("FoldCaseForDatabaseMatch folds ASCII case", "[UnicodeText]")
{
  CHECK(FoldCaseForDatabaseMatch("Hello, World 123") == "HELLO, WORLD 123");
  CHECK(FoldCaseForDatabaseMatch("") == "");
}

TEST_CASE("FoldCaseForDatabaseMatch folds non-ASCII case -- the gap a real rule hit live", "[UnicodeText]")
{
  // A rule "ZZZ ÜNDER TEST" stayed unlinked from a recording "ZZZ ünder
  // test" because this addon only folded ASCII, while the real database's
  // UPPER() folds both.
  CHECK(FoldCaseForDatabaseMatch("ünder") == FoldCaseForDatabaseMatch("ÜNDER"));
  CHECK(FoldCaseForDatabaseMatch("ünder") == "\xC3\x9CNDER");
  CHECK(FoldCaseForDatabaseMatch("é") == "\xC3\x89");
  CHECK(FoldCaseForDatabaseMatch("\xC3\xBF") == "\xC5\xB8"); // ÿ -> Ÿ, which lives in a different block
}

TEST_CASE("FoldCaseForDatabaseMatch covers Latin, Greek, Cyrillic and supplementary-plane letters", "[UnicodeText]")
{
  // Alternating-case blocks (every other code point maps): Latin Extended-A.
  CHECK(FoldCaseForDatabaseMatch("\xC4\x81") == "\xC4\x80"); // ā -> Ā
  CHECK(FoldCaseForDatabaseMatch("\xC4\x80") == "\xC4\x80"); // Ā stays
  // Greek, including the final sigma folding onto the capital just as
  // the medial one does.
  CHECK(FoldCaseForDatabaseMatch("\xCF\x83") == "\xCE\xA3"); // σ -> Σ
  CHECK(FoldCaseForDatabaseMatch("\xCF\x82") == "\xCE\xA3"); // ς -> Σ
  // The accent is kept: ή folds to Ή (U+0389), not to a plain Η, as in PostgreSQL.
  CHECK(FoldCaseForDatabaseMatch("\xCE\xB1\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1") ==
        "\xCE\x91\xCE\x98\xCE\x89\xCE\x9D\xCE\x91"); // αθήνα -> ΑΘΉΝΑ
  CHECK(FoldCaseForDatabaseMatch("\xCE\xB1\xCE\xB8\xCE\xAE\xCE\xBD\xCE\xB1") !=
        FoldCaseForDatabaseMatch("\xCE\x91\xCE\x98\xCE\x97\xCE\x9D\xCE\x91"));
  // Cyrillic.
  CHECK(FoldCaseForDatabaseMatch("\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0") ==
        FoldCaseForDatabaseMatch("\xD0\x9C\xD0\x9E\xD0\xA1\xD0\x9A\xD0\x92\xD0\x90")); // москва vs МОСКВА
  // The micro sign maps to the Greek capital mu, a mapping that crosses scripts.
  CHECK(FoldCaseForDatabaseMatch("\xC2\xB5") == "\xCE\x9C");
  // Dotless i and long s fold onto ASCII letters.
  CHECK(FoldCaseForDatabaseMatch("\xC4\xB1") == "I");
  CHECK(FoldCaseForDatabaseMatch("\xC5\xBF") == "S");
  // Supplementary plane: Deseret small letter long i -> capital.
  CHECK(FoldCaseForDatabaseMatch("\xF0\x90\x90\xA8") == "\xF0\x90\x90\x80");
}

TEST_CASE("FoldCaseForDatabaseMatch uses the simple mapping, one code point in and one out", "[UnicodeText]")
{
  // PostgreSQL's UPPER() maps a code point at a time, so the German sharp s
  // stays as it is (Python's str.upper() would make it "SS"), and the
  // titlecase digraphs map to the digraph capital rather than expanding.
  CHECK(FoldCaseForDatabaseMatch("\xC3\x9F") == "\xC3\x9F");
  CHECK(FoldCaseForDatabaseMatch("\xC7\x85") == "\xC7\x84"); // ǅ -> Ǆ
  CHECK(FoldCaseForDatabaseMatch("\xC7\x86") == "\xC7\x84"); // ǆ -> Ǆ
  // The Greek letters with prosgegrammeni have a simple mapping to their
  // titlecase form (U+1F80 -> U+1F88) even though Python's full mapping
  // expands them to two code points -- the gap a Python-generated table
  // would have had.
  CHECK(FoldCaseForDatabaseMatch("\xE1\xBE\x80") == "\xE1\xBE\x88");
}

TEST_CASE("FoldCaseForDatabaseMatch leaves letters without a case alone", "[UnicodeText]")
{
  CHECK(FoldCaseForDatabaseMatch("\xE4\xB8\xAD\xE6\x96\x87") == "\xE4\xB8\xAD\xE6\x96\x87"); // 中文
  CHECK(FoldCaseForDatabaseMatch("\xF0\x9F\x93\xBA") == "\xF0\x9F\x93\xBA");                 // an emoji
}

TEST_CASE("FoldCaseForDatabaseMatch is idempotent", "[UnicodeText]")
{
  for (const char* text : {"Hello", "ünder", "\xCF\x82\xCF\x83", "\xC7\x85", "\xE1\xBE\x80", "mixed \xC3\x9F Case"})
  {
    const std::string once = FoldCaseForDatabaseMatch(text);
    CHECK(FoldCaseForDatabaseMatch(once) == once);
  }
}

TEST_CASE("FoldCaseForDatabaseMatch passes invalid UTF-8 through unchanged", "[UnicodeText]")
{
  // Never an error: an invalid byte is an opaque character of its own, so two
  // strings carrying the same invalid bytes still compare equal.
  CHECK(FoldCaseForDatabaseMatch("\xFF\xFE"
                                 "abc") == "\xFF\xFE"
                                           "ABC");
  CHECK(FoldCaseForDatabaseMatch("a\x80z") == "A\x80Z");                     // a stray continuation byte
  CHECK(FoldCaseForDatabaseMatch("ab\xC3") == "AB\xC3");                     // a truncated sequence
  CHECK(FoldCaseForDatabaseMatch("\xC0\x80") == "\xC0\x80");                 // an overlong encoding
  CHECK(FoldCaseForDatabaseMatch("\xED\xA0\x80") == "\xED\xA0\x80");         // a UTF-16 surrogate
  CHECK(FoldCaseForDatabaseMatch("\xF4\x90\x80\x80") == "\xF4\x90\x80\x80"); // above U+10FFFF
}

TEST_CASE("StripUnicodeWhitespace strips what Python's str.strip() strips", "[UnicodeText]")
{
  CHECK(StripUnicodeWhitespace("  hello  ") == "hello");
  CHECK(StripUnicodeWhitespace("\t\r\n hello \x0B\x0C") == "hello");
  // The separators Python also counts as whitespace.
  CHECK(StripUnicodeWhitespace("\x1C\x1D\x1E\x1F"
                               "x"
                               "\x1F\x1E\x1D\x1C") == "x");
}

TEST_CASE("StripUnicodeWhitespace strips a trailing no-break space -- the rule the API stores unstripped",
          "[UnicodeText]")
{
  // Dispatcharr stores a rule's title as sent and strips it only when it
  // evaluates the rule, so a trailing no-break space survives in the API
  // response and has to come off here to line up with what was matched.
  CHECK(StripUnicodeWhitespace("ZZZ_TEST Alpha\xC2\xA0") == "ZZZ_TEST Alpha");
  CHECK(StripUnicodeWhitespace("\xC2\xA0ZZZ_TEST Alpha") == "ZZZ_TEST Alpha");
}

TEST_CASE("StripUnicodeWhitespace strips the full Unicode whitespace set", "[UnicodeText]")
{
  for (const char* space : {"\xC2\x85",      // U+0085 next line
                            "\xC2\xA0",      // U+00A0 no-break space
                            "\xE1\x9A\x80",  // U+1680 ogham space mark
                            "\xE2\x80\x80",  // U+2000 en quad
                            "\xE2\x80\x8A",  // U+200A hair space
                            "\xE2\x80\xA8",  // U+2028 line separator
                            "\xE2\x80\xA9",  // U+2029 paragraph separator
                            "\xE2\x80\xAF",  // U+202F narrow no-break space
                            "\xE2\x81\x9F",  // U+205F medium mathematical space
                            "\xE3\x80\x80"}) // U+3000 ideographic space
  {
    CHECK(StripUnicodeWhitespace(std::string(space) + "x" + space) == "x");
  }
}

TEST_CASE("StripUnicodeWhitespace keeps characters Python does not count as whitespace", "[UnicodeText]")
{
  // The zero-width space and the byte order mark look blank but are not
  // whitespace to str.strip().
  CHECK(StripUnicodeWhitespace("\xE2\x80\x8B"
                               "x") == "\xE2\x80\x8B"
                                       "x");
  CHECK(StripUnicodeWhitespace("x\xEF\xBB\xBF") == "x\xEF\xBB\xBF");
  CHECK(StripUnicodeWhitespace("\xE2\x80\x8B") == "\xE2\x80\x8B");
}

TEST_CASE("StripUnicodeWhitespace leaves interior whitespace and handles all-whitespace input", "[UnicodeText]")
{
  CHECK(StripUnicodeWhitespace(" a  b\xC2\xA0"
                               "c ") == "a  b\xC2\xA0"
                                        "c");
  CHECK(StripUnicodeWhitespace("   ") == "");
  CHECK(StripUnicodeWhitespace("\xC2\xA0\xE3\x80\x80") == "");
  CHECK(StripUnicodeWhitespace("") == "");
}

TEST_CASE("the one-sided strips only touch their own end", "[UnicodeText]")
{
  CHECK(StripLeadingUnicodeWhitespace("\xC2\xA0 x \xC2\xA0") == "x \xC2\xA0");
  CHECK(StripTrailingUnicodeWhitespace("\xC2\xA0 x \xC2\xA0") == "\xC2\xA0 x");
}

TEST_CASE("StripUnicodeWhitespace handles invalid UTF-8 without reading past the ends", "[UnicodeText]")
{
  CHECK(StripUnicodeWhitespace(" \xFF ") == "\xFF");
  CHECK(StripUnicodeWhitespace("\xC2") == "\xC2");   // a lone lead byte is not a no-break space
  CHECK(StripUnicodeWhitespace("x\xA0") == "x\xA0"); // a lone continuation byte is not one either
}

TEST_CASE("IsDatabaseWordCodePoint matches PostgreSQL's word characters", "[UnicodeText]")
{
  for (char c : {'a', 'Z', '0', '9', '_'})
    CHECK(IsDatabaseWordCodePoint(static_cast<char32_t>(c)));
  for (char c : {' ', '-', '.', '+', '"', '(', ','})
    CHECK_FALSE(IsDatabaseWordCodePoint(static_cast<char32_t>(c)));
  CHECK(IsDatabaseWordCodePoint(0xE9));           // é
  CHECK(IsDatabaseWordCodePoint(0x3A3));          // Σ
  CHECK(IsDatabaseWordCodePoint(0x4E2D));         // 中
  CHECK(IsDatabaseWordCodePoint(0x10428));        // a supplementary-plane letter
  CHECK_FALSE(IsDatabaseWordCodePoint(0xA0));     // no-break space
  CHECK_FALSE(IsDatabaseWordCodePoint(0xAB));     // «
  CHECK_FALSE(IsDatabaseWordCodePoint(0x2014));   // em dash
  CHECK_FALSE(IsDatabaseWordCodePoint(0x1F4FA));  // an emoji
  CHECK_FALSE(IsDatabaseWordCodePoint(0x110000)); // not a code point
}

TEST_CASE("DecodeUtf8At and DecodeUtf8Before round-trip every length", "[UnicodeText]")
{
  const std::string s = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x93\xBA"; // a é 中 📺
  std::size_t length = 0;
  CHECK(DecodeUtf8At(s, 0, length) == U'a');
  CHECK(length == 1);
  CHECK(DecodeUtf8At(s, 1, length) == 0xE9);
  CHECK(length == 2);
  CHECK(DecodeUtf8At(s, 3, length) == 0x4E2D);
  CHECK(length == 3);
  CHECK(DecodeUtf8At(s, 6, length) == 0x1F4FA);
  CHECK(length == 4);

  CHECK(DecodeUtf8Before(s, s.size(), length) == 0x1F4FA);
  CHECK(length == 4);
  CHECK(DecodeUtf8Before(s, 6, length) == 0x4E2D);
  CHECK(length == 3);
  CHECK(DecodeUtf8Before(s, 3, length) == 0xE9);
  CHECK(length == 2);
  CHECK(DecodeUtf8Before(s, 1, length) == U'a');
  CHECK(length == 1);
}

TEST_CASE("the decoders treat a broken sequence as one opaque byte above the code-point range", "[UnicodeText]")
{
  std::size_t length = 0;
  CHECK(DecodeUtf8At("\xC3", 0, length) > 0x10FFFF); // truncated
  CHECK(length == 1);
  CHECK(DecodeUtf8At("\x80", 0, length) > 0x10FFFF); // stray continuation
  CHECK(length == 1);
  CHECK(DecodeUtf8At("\xE0\x80\x80", 0, length) > 0x10FFFF); // overlong
  CHECK(length == 1);
  CHECK(DecodeUtf8Before("a\xC3", 2, length) > 0x10FFFF); // the last byte is a lead with nothing after it
  CHECK(length == 1);
  CHECK(DecodeUtf8Before("\x80\x80\x80\x80\x80", 5, length) > 0x10FFFF); // a run of continuation bytes, no lead
  CHECK(length == 1);
}

TEST_CASE("FoldCaseForDatabaseMatch handles the alternating upper/lower ranges the tables stride over", "[UnicodeText]")
{
  // Latin Extended-A alternates upper (even) and lower (odd) code points: U+0100 stays, U+0101 folds up.
  CHECK(FoldCaseForDatabaseMatch("\xC4\x80") == "\xC4\x80"); // U+0100 (A with macron)
  CHECK(FoldCaseForDatabaseMatch("\xC4\x81") == "\xC4\x80"); // U+0101 -> U+0100
  CHECK(FoldCaseForDatabaseMatch("\xC4\x82") == "\xC4\x82"); // U+0102
  CHECK(FoldCaseForDatabaseMatch("\xC4\x83") == "\xC4\x82"); // U+0103 -> U+0102
  // Greek and Cyrillic: a lower-case letter folds to its capital.
  CHECK(FoldCaseForDatabaseMatch("\xCE\xB1") == "\xCE\x91"); // alpha -> ALPHA
  CHECK(FoldCaseForDatabaseMatch("\xD0\xB4") == "\xD0\x94"); // Cyrillic de -> DE
}

TEST_CASE("FoldCaseForDatabaseMatch passes an overlong UTF-8 encoding through, never decoding it to ASCII",
          "[UnicodeText]")
{
  // F0 80 81 A1 is an overlong encoding of 'a' (U+0061): invalid, so its bytes are left as they are
  // rather than folded to "A".
  const std::string overlong = "\xF0\x80\x81\xA1";
  CHECK(FoldCaseForDatabaseMatch(overlong) == overlong);
  CHECK(FoldCaseForDatabaseMatch(overlong) != "A");
}
