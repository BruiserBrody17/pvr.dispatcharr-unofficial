#include "RecordingDirectory.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("SanitizeRecordingDirectory passes through an ordinary title unchanged", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("Alpha Cookery") == "Alpha Cookery");
}

TEST_CASE("SanitizeRecordingDirectory strips a forward slash -- the real bug this fixes", "[RecordingDirectory]")
{
  // Kodi's own CPVRRecordingsPath treats a raw '/' in Directory as a
  // path separator, turning this into a nested folder instead of
  // Dispatcharr's own single flat one for the show.
  CHECK(SanitizeRecordingDirectory("Alpha/Beta") == "AlphaBeta");
  CHECK(SanitizeRecordingDirectory("10/12") == "1012");
}

TEST_CASE("SanitizeRecordingDirectory strips every forbidden filename character", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("A\\B:C*D?E\"F<G>H|I") == "ABCDEFGHI");
}

TEST_CASE("SanitizeRecordingDirectory trims leading and trailing whitespace", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("  Show Name  ") == "Show Name");
}

// Dispatcharr's _safe_name() trims with Python's str.strip(), which removes Unicode whitespace; std::isspace() trimmed
// ASCII only, and under a Windows code page that counts 0xA0 as a space it cut the second byte off a trailing "à".
TEST_CASE("SanitizeRecordingDirectory trims Unicode whitespace the way Python's strip() does", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("\xE3\x80\x80Show Name\xE3\x80\x80") == "Show Name"); // ideographic space
  CHECK(SanitizeRecordingDirectory("Show Name\xC2\xA0") == "Show Name");                 // no-break space
  CHECK(SanitizeRecordingDirectory("\xE2\x80\x83Show\xE2\x80\x89Name\xE2\x80\x83") ==
        "Show\xE2\x80\x89Name"); // inner whitespace stays
}

TEST_CASE("SanitizeRecordingDirectory keeps a title ending in a letter whose UTF-8 ends in 0xA0 whole",
          "[RecordingDirectory]")
{
  // "à" is C3 A0: only whole whitespace code points are trimmed, never a byte out of one.
  CHECK(SanitizeRecordingDirectory("Voil\xC3\xA0") == "Voil\xC3\xA0");
  CHECK(SanitizeRecordingDirectory("Voil\xC3\xA0 ") == "Voil\xC3\xA0");
  CHECK(SanitizeRecordingDirectory("\xE3\x80\x80") == "Recording");
}

TEST_CASE("SanitizeRecordingDirectory falls back to \"Recording\" when nothing survives sanitizing",
          "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("///") == "Recording");
  CHECK(SanitizeRecordingDirectory("") == "Recording");
  CHECK(SanitizeRecordingDirectory("   ") == "Recording");
}
