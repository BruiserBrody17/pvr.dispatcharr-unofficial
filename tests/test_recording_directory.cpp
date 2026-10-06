#include "RecordingDirectory.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("SanitizeRecordingDirectory passes through an ordinary title unchanged", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("Breaking Bad") == "Breaking Bad");
}

TEST_CASE("SanitizeRecordingDirectory strips a forward slash -- the real bug this fixes", "[RecordingDirectory]")
{
  // Kodi's own CPVRRecordingsPath treats a raw '/' in Directory as a
  // path separator, turning this into a nested folder instead of
  // Dispatcharr's own single flat one for the show.
  CHECK(SanitizeRecordingDirectory("Face/Off") == "FaceOff");
  CHECK(SanitizeRecordingDirectory("20/20") == "2020");
}

TEST_CASE("SanitizeRecordingDirectory strips every forbidden filename character", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("A\\B:C*D?E\"F<G>H|I") == "ABCDEFGHI");
}

TEST_CASE("SanitizeRecordingDirectory trims leading and trailing whitespace", "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("  Show Name  ") == "Show Name");
}

TEST_CASE("SanitizeRecordingDirectory falls back to \"Recording\" when nothing survives sanitizing",
          "[RecordingDirectory]")
{
  CHECK(SanitizeRecordingDirectory("///") == "Recording");
  CHECK(SanitizeRecordingDirectory("") == "Recording");
  CHECK(SanitizeRecordingDirectory("   ") == "Recording");
}
