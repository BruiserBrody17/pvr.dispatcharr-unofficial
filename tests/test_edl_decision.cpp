#include "EdlDecision.h"

#include "RecordingParser.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

namespace
{
ComskipInfo Parse(const char* customProperties)
{
  return ParseComskipInfo(json::parse(customProperties));
}

RecordingEdlEntry Entry(int64_t startMs, int64_t endMs, int type = kEdlTypeCommercialBreak)
{
  RecordingEdlEntry e;
  e.startMs = startMs;
  e.endMs = endMs;
  e.type = type;
  return e;
}
} // namespace

// ---------------------------------------------------------------------
// ParseComskipInfo: the shapes comskip_process_recording writes
// ---------------------------------------------------------------------

TEST_CASE("ParseComskipInfo reports an absent, null or malformed key as not present", "[EdlDecision]")
{
  CHECK_FALSE(Parse(R"({})").present);
  CHECK_FALSE(Parse(R"({"comskip": null})").present);
  CHECK_FALSE(Parse(R"({"comskip": "completed"})").present);
  CHECK_FALSE(ParseComskipInfo(json()).present);
}

TEST_CASE("ParseComskipInfo reads mark mode: markers, no skipped flag, not cut", "[EdlDecision]")
{
  ComskipInfo c = Parse(R"({"comskip": {"status": "completed", "mode": "mark", "edl": "a.edl", "commercials": 4}})");
  CHECK(c.present);
  CHECK(c.status == "completed");
  CHECK_FALSE(c.noCommercials);
  CHECK_FALSE(c.cutMode);
}

TEST_CASE("ParseComskipInfo reads the no-commercials, cut and not-installed shapes", "[EdlDecision]")
{
  // comskip's exit code 1: no commercials detected.
  ComskipInfo none = Parse(R"({"comskip": {"status": "completed", "skipped": true, "ini_path": "x.ini"}})");
  CHECK(none.noCommercials);
  CHECK_FALSE(none.cutMode);
  // Cut mode: no `mode` key, a count of kept segments.
  ComskipInfo cut =
      Parse(R"({"comskip": {"status": "completed", "edl": "a.edl", "segments_kept": 3, "commercials": 2}})");
  CHECK(cut.cutMode);
  CHECK_FALSE(cut.noCommercials);
  ComskipInfo missing = Parse(R"({"comskip": {"status": "skipped", "reason": "comskip_not_installed"}})");
  CHECK(missing.status == "skipped");
  CHECK_FALSE(missing.noCommercials);
}

// ---------------------------------------------------------------------
// ShouldAskPluginForEdl
// ---------------------------------------------------------------------

TEST_CASE("ShouldAskPluginForEdl asks when comskip left nothing on record, so older servers behave as before",
          "[EdlDecision]")
{
  CHECK(ShouldAskPluginForEdl(ComskipInfo{}));
}

TEST_CASE("ShouldAskPluginForEdl asks for a recording with markers", "[EdlDecision]")
{
  CHECK(ShouldAskPluginForEdl(
      Parse(R"({"comskip": {"status": "completed", "mode": "mark", "edl": "a.edl", "commercials": 4}})")));
}

TEST_CASE("ShouldAskPluginForEdl does not ask where comskip left no EDL file", "[EdlDecision]")
{
  CHECK_FALSE(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "completed", "skipped": true}})")));
  CHECK_FALSE(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "completed", "skipped": true, "edl": "a.edl"}})")));
  CHECK_FALSE(
      ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "completed", "edl": "a.edl", "segments_kept": 3}})")));
  CHECK_FALSE(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "skipped", "reason": "comskip_not_installed"}})")));
}

TEST_CASE("ShouldAskPluginForEdl still asks after an error or in a state it does not know", "[EdlDecision]")
{
  // An error can come after the EDL file was written (duration_unknown), and the plugin reads the file.
  CHECK(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "error", "reason": "duration_unknown"}})")));
  CHECK(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "running"}})")));
  CHECK(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "completed"}})")));
}

TEST_CASE("ShouldAskPluginForEdl never denies markers to an older block that has no skipped or segments_kept key",
          "[EdlDecision]")
{
  // The one that would be lost if "completed without mode: mark" were read as cut mode.
  CHECK(ShouldAskPluginForEdl(Parse(R"({"comskip": {"status": "completed", "edl": "a.edl", "commercials": 5}})")));
}

// ---------------------------------------------------------------------
// WithoutBreaksCoveringRecording
// ---------------------------------------------------------------------

TEST_CASE("WithoutBreaksCoveringRecording drops the break the live recording produced", "[EdlDecision]")
{
  // 35.27 s of EDL over a 33 s recording: comskip ran past the end by a second or two.
  auto kept = WithoutBreaksCoveringRecording({Entry(0, 35270)}, 33);
  CHECK(kept.empty());
}

TEST_CASE("WithoutBreaksCoveringRecording keeps ordinary breaks", "[EdlDecision]")
{
  // A half-hour recording with two breaks.
  std::vector<RecordingEdlEntry> in = {Entry(300000, 480000), Entry(900000, 1080000)};
  auto kept = WithoutBreaksCoveringRecording(in, 1800);
  REQUIRE(kept.size() == 2);
  CHECK(kept[0].startMs == 300000);
  CHECK(kept[1].endMs == 1080000);
}

TEST_CASE("WithoutBreaksCoveringRecording drops a cut as well, and keeps a mute and a scene marker", "[EdlDecision]")
{
  std::vector<RecordingEdlEntry> in = {Entry(0, 60000, kEdlTypeCut), Entry(0, 60000, 1), Entry(0, 60000, 2)};
  auto kept = WithoutBreaksCoveringRecording(in, 60);
  REQUIRE(kept.size() == 2);
  CHECK(kept[0].type == 1);
  CHECK(kept[1].type == 2);
}

TEST_CASE("WithoutBreaksCoveringRecording draws the line at nine tenths of the recording", "[EdlDecision]")
{
  CHECK(WithoutBreaksCoveringRecording({Entry(0, 90000)}, 100).empty());
  CHECK(WithoutBreaksCoveringRecording({Entry(0, 89999)}, 100).size() == 1);
  // Not starting at zero: the covered share is what counts.
  CHECK(WithoutBreaksCoveringRecording({Entry(5000, 95000)}, 100).empty());
  CHECK(WithoutBreaksCoveringRecording({Entry(5000, 94000)}, 100).size() == 1);
}

TEST_CASE("WithoutBreaksCoveringRecording clamps an entry that runs outside the recording", "[EdlDecision]")
{
  CHECK(WithoutBreaksCoveringRecording({Entry(-2000, 120000)}, 100).empty());
  // Starts after the end: covers nothing.
  CHECK(WithoutBreaksCoveringRecording({Entry(200000, 300000)}, 100).size() == 1);
}

TEST_CASE("WithoutBreaksCoveringRecording drops nothing when the duration is unknown", "[EdlDecision]")
{
  CHECK(WithoutBreaksCoveringRecording({Entry(0, 35270)}, 0).size() == 1);
  CHECK(WithoutBreaksCoveringRecording({Entry(0, 35270)}, -1).size() == 1);
  CHECK(WithoutBreaksCoveringRecording({}, 33).empty());
}
