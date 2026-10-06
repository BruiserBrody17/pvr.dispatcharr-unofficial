#include "RefreshEvents.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
// The shapes below are the real events of a live refresh (an XMLTV source and an M3U account,
// refreshed on demand), with the numbers and names replaced.
const std::string kEpgDownloading =
    R"({"type": "update", "data": {"progress": 95, "type": "epg_refresh", "source": 5, "action": "downloading", "speed": 1000.5, "elapsed_time": 1.0, "time_remaining": 0, "downloaded": "1.00 MB"}})";
const std::string kEpgChannelsParsing =
    R"({"type": "update", "data": {"progress": 57, "type": "epg_refresh", "source": 5, "action": "parsing_channels", "processed": 4, "total": 9}})";
const std::string kEpgChannelsDone =
    R"({"type": "update", "data": {"progress": 100, "type": "epg_refresh", "source": 5, "action": "parsing_channels", "status": "success", "channels_count": 9}})";
const std::string kEpgProgramsStart =
    R"({"type": "update", "data": {"progress": 0, "type": "epg_refresh", "source": 5, "action": "parsing_programs"}})";
const std::string kEpgProgramsStaging =
    R"({"type": "update", "data": {"progress": 73, "type": "epg_refresh", "source": 5, "action": "parsing_programs", "processed": 55, "channels": 5, "message": "Staging programs... 55"}})";
const std::string kEpgProgramsDone =
    R"({"type": "update", "data": {"progress": 100, "type": "epg_refresh", "source": 5, "action": "parsing_programs", "status": "success", "message": "Parsed 100 programs for 4 channels - skipped 300 programs for 3 unmapped channels",
    "updated_at" : "2000-01-01T00:00:00.000000+00:00"
}
})";

const std::string kM3uGroups =
    R"({"type": "update", "data": {"progress": 0, "type": "m3u_refresh", "account": 5, "action": "processing_groups", "status": "fetching", "message": "Refresh in progress..."}})";
const std::string kM3uParsing =
    R"({"type": "update", "data": {"progress": 100, "type": "m3u_refresh", "account": 5, "action": "parsing", "status": "parsing", "message": "Refresh in progress...", "streams_processed": 9}})";
const std::string kM3uDoneNothingChanged =
    R"({"type": "update", "data": {"progress": 100, "type": "m3u_refresh", "account": 5, "action": "parsing", "status": "success", "elapsed_time": 2.5, "time_remaining": 0, "streams_processed": 9, "streams_created": 0, "streams_updated": 0, "streams_stale": 0, "streams_deleted": 0, "channels_created": 0, "channels_updated": 0, "channels_deleted": 0, "channels_failed": 0, "failed_stream_details": [], "message": "Processing completed in 2.5 seconds."}})";
const std::string kM3uVod =
    R"({"type": "update", "data": {"progress": 0, "type": "m3u_refresh", "account": 5, "action": "vod_refresh", "message": "Processing completed in 2.5 seconds.", "status": "processing"}})";

std::string M3uDone(const std::string& channelFields)
{
  return R"({"type": "update", "data": {"progress": 100, "type": "m3u_refresh", "account": 5, "action": "parsing", "status": "success", )" +
         channelFields + R"("message": "done"}})";
}
} // namespace

TEST_CASE("ClassifyRefreshEvent recognizes the end of an EPG refresh and nothing before it", "[RefreshEvents]")
{
  CHECK(ClassifyRefreshEvent(kEpgProgramsDone) == RefreshEventKind::kGuideParsed);
  // every step of the real sequence that is not its last
  CHECK(ClassifyRefreshEvent(kEpgDownloading) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(kEpgChannelsParsing) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(kEpgProgramsStart) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(kEpgProgramsStaging) == RefreshEventKind::kNone);
}

TEST_CASE("ClassifyRefreshEvent does not mistake the channel step's success for the end", "[RefreshEvents]")
{
  // parsing_channels also ends with progress 100 and status "success", and programmes are still to come.
  CHECK(ClassifyRefreshEvent(kEpgChannelsDone) == RefreshEventKind::kNone);
}

TEST_CASE("ClassifyRefreshEvent needs both the success status and a finished progress for an EPG refresh",
          "[RefreshEvents]")
{
  const auto epg = [](const std::string& fields)
  {
    return R"({"type": "update", "data": {"type": "epg_refresh", "source": 5, "action": "parsing_programs", )" +
           fields + "}}";
  };
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 100, "status": "success")")) == RefreshEventKind::kGuideParsed);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 100.0, "status": "success")")) == RefreshEventKind::kGuideParsed);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 99, "status": "success")")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 100, "status": "error")")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 100)")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg(R"("status": "success")")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": "100", "status": "success")")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg(R"("progress": 100, "status": ["success"])")) == RefreshEventKind::kNone);
}

TEST_CASE("ClassifyRefreshEvent reads the end of an M3U refresh by what its channel sync did", "[RefreshEvents]")
{
  // nothing about the real sequence before the last "parsing" event, or after it, is a signal
  CHECK(ClassifyRefreshEvent(kM3uGroups) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(kM3uParsing) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(kM3uVod) == RefreshEventKind::kNone);
  // a refresh that changed no channel (the usual scheduled one) does not cost a channel fetch
  CHECK(ClassifyRefreshEvent(kM3uDoneNothingChanged) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 3, "channels_updated": 0, "channels_deleted": 0, )")) ==
        RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 0, "channels_updated": 2, "channels_deleted": 0, )")) ==
        RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 0, "channels_updated": 0, "channels_deleted": 1, )")) ==
        RefreshEventKind::kChannelsSynced);
  // channels_failed alone is not a change to the list
  CHECK(ClassifyRefreshEvent(M3uDone(
            R"("channels_created": 0, "channels_updated": 0, "channels_deleted": 0, "channels_failed": 4, )")) ==
        RefreshEventKind::kNone);
}

TEST_CASE("ClassifyRefreshEvent treats an M3U result that does not say what changed as a change", "[RefreshEvents]")
{
  CHECK(ClassifyRefreshEvent(M3uDone("")) == RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("streams_processed": 10, )")) == RefreshEventKind::kChannelsSynced);
  // one count present and zero, the others missing: it said nothing changed
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 0, )")) == RefreshEventKind::kNone);
  // a count that is not a number says nothing
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": "many", )")) == RefreshEventKind::kChannelsSynced);
}

TEST_CASE("ClassifyRefreshEvent ignores a failed or unrelated refresh and anything malformed", "[RefreshEvents]")
{
  CHECK(
      ClassifyRefreshEvent(
          R"({"type": "update", "data": {"type": "m3u_refresh", "account": 5, "action": "parsing", "status": "error", "channels_created": 9}})") ==
      RefreshEventKind::kNone);
  CHECK(
      ClassifyRefreshEvent(
          R"({"type": "update", "data": {"type": "recording_started", "status": "success", "progress": 100, "action": "parsing_programs"}})") ==
      RefreshEventKind::kNone);
  CHECK(
      ClassifyRefreshEvent(
          R"({"type": "update", "data": {"type": "epg_refresh", "status": "success", "progress": 100, "action": "downloading"}})") ==
      RefreshEventKind::kNone);
  CHECK(
      ClassifyRefreshEvent(
          R"({"type": "update", "data": {"type": "m3u_refresh", "status": "success", "action": "vod_refresh", "channels_created": 3}})") ==
      RefreshEventKind::kNone);
  for (
      const char* bad :
      {"", "not json", "[]", "null", "{}", R"({"data": []})", R"({"data": null})", R"({"data": {"type": 5}})",
       R"({"data": {"type": "epg_refresh", "action": 7, "status": "success", "progress": 100}})",
       R"({"type": "update", "data": {"type": "epg_refresh", "action": "parsing_programs", "status": "success", "progress": 1e999}})"})
    CHECK(ClassifyRefreshEvent(bad) == RefreshEventKind::kNone);
}

TEST_CASE("ScheduleGuideRefetchForEvent schedules past the exported guide's own cache", "[RefreshEvents]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(10);
  CHECK(ScheduleGuideRefetchForEvent({}, now) == now + std::chrono::seconds(330));
  CHECK(kGuideRefetchAfterRefreshEvent > std::chrono::seconds(300)); // Dispatcharr's own cache of the exported guide
}

TEST_CASE("ScheduleGuideRefetchForEvent keeps a fetch that is already scheduled past this event's window",
          "[RefreshEvents]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(10);
  // A renumbering's follow-up, due well after this event's own window: it picks this refresh up too.
  const auto later = now + std::chrono::seconds(331);
  CHECK(ScheduleGuideRefetchForEvent(later, now) == later);
  CHECK(ScheduleGuideRefetchForEvent(now + std::chrono::seconds(330), now) == now + std::chrono::seconds(330));
  // Once the fetch has run (and cleared the schedule) the next event schedules afresh.
  CHECK(ScheduleGuideRefetchForEvent({}, now + std::chrono::seconds(400)) == now + std::chrono::seconds(730));
}

TEST_CASE("ScheduleGuideRefetchForEvent moves a fetch that would land inside this event's cache window",
          "[RefreshEvents]")
{
  const auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(10);
  const auto due = now + std::chrono::seconds(330);
  // A fetch due in 100 s, 30 s, now, or already overdue (running) would be served the old guide: it moves out.
  CHECK(ScheduleGuideRefetchForEvent(now + std::chrono::seconds(100), now) == due);
  CHECK(ScheduleGuideRefetchForEvent(now + std::chrono::seconds(329), now) == due);
  CHECK(ScheduleGuideRefetchForEvent(now, now) == due);
  CHECK(ScheduleGuideRefetchForEvent(now - std::chrono::seconds(20), now) == due);
  // A second source finishing a few seconds after the first moves it by those few seconds, not by a whole window.
  const auto first = now + kGuideRefetchAfterRefreshEvent;
  CHECK(ScheduleGuideRefetchForEvent(first, now + std::chrono::seconds(5)) == first + std::chrono::seconds(5));
}

TEST_CASE("ClassifyRefreshEvent needs a finished progress of 100 to call a guide parsed", "[RefreshEvents]")
{
  const auto epg = [](const char* progress)
  {
    return std::string(
               R"({"type": "update", "data": {"type": "epg_refresh", "action": "parsing_programs", "status": "success", "progress": )") +
           progress + "}}";
  };
  CHECK(ClassifyRefreshEvent(epg("100")) == RefreshEventKind::kGuideParsed);
  CHECK(ClassifyRefreshEvent(epg("100.0")) == RefreshEventKind::kGuideParsed);
  CHECK(ClassifyRefreshEvent(epg("99.9")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg("99.5")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg("99")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(epg("0")) == RefreshEventKind::kNone);
}

TEST_CASE("ClassifyRefreshEvent reads a count that is missing in part, and a count that is not an integer",
          "[RefreshEvents]")
{
  // Each count alone, the others missing: a zero says nothing changed, a positive one says something did.
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_deleted": 0, )")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_updated": 0, )")) == RefreshEventKind::kNone);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_deleted": 2, )")) == RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_updated": 2, )")) == RefreshEventKind::kChannelsSynced);
  // A float count: 1e300 is far beyond any integer (reading it as one was undefined behaviour), and still a change.
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 1e300, "channels_updated": 0, "channels_deleted": 0, )")) ==
        RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 3.0, "channels_updated": 0, "channels_deleted": 0, )")) ==
        RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": 0.0, "channels_updated": 0, "channels_deleted": 0, )")) ==
        RefreshEventKind::kNone);
  CHECK(
      ClassifyRefreshEvent(M3uDone(R"("channels_created": -1e300, "channels_updated": 0, "channels_deleted": 0, )")) ==
      RefreshEventKind::kNone);
  // An unsigned integer beyond the signed range is still a count.
  CHECK(ClassifyRefreshEvent(
            M3uDone(R"("channels_created": 18446744073709551615, "channels_updated": 0, "channels_deleted": 0, )")) ==
        RefreshEventKind::kChannelsSynced);
}

TEST_CASE("ClassifyRefreshEvent reads counts that are all negative floats as not said, which is a change",
          "[RefreshEvents]")
{
  // A negative count is not a count: all three of them read as missing, and a result that does not say what changed is
  // treated as a change (refetching is the safe error), exactly like all three being absent.
  CHECK(ClassifyRefreshEvent(
            M3uDone(R"("channels_created": -5.5, "channels_updated": -1.5, "channels_deleted": -0.5, )")) ==
        RefreshEventKind::kChannelsSynced);
  CHECK(ClassifyRefreshEvent(M3uDone(R"("channels_created": -5, "channels_updated": -1, "channels_deleted": -2, )")) ==
        RefreshEventKind::kChannelsSynced);
  // One real zero among them says nothing changed.
  CHECK(
      ClassifyRefreshEvent(M3uDone(R"("channels_created": -5.5, "channels_updated": 0, "channels_deleted": -0.5, )")) ==
      RefreshEventKind::kNone);
}
