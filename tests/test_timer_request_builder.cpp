#include "TimerRequestBuilder.h"

#include "DateTimeFormat.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

// ---------------------------------------------------------------------
// BuildSeriesRuleRequestBody
// ---------------------------------------------------------------------

// The trailing five params (titleMode/description/descriptionMode/
// untaggedIsNew/epgSourceId) are all left at their "nothing to echo"
// sentinel in every test above this point, matching AddTimer()'s own
// create-path call (a brand-new rule has nothing cached yet) -- these
// tests are about the pre-existing five params, unaffected by this
// fix's own new ones.

TEST_CASE("BuildSeriesRuleRequestBody always sends title", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(/*channelId=*/0, /*tvgId=*/"", "My Show", /*recordNewOnly=*/false, "", "", "",
                                         false, 0);
  CHECK(body["title"] == "My Show");
}

TEST_CASE("BuildSeriesRuleRequestBody omits channel_id entirely for a non-positive channelId -- the 18th-pass "
          "400 fix",
          "[TimerRequestBuilder]")
{
  // Sending the sentinel literally (e.g. -1) instead of omitting the key
  // tripped a real, confirmed 400 ("channel_id does not exist").
  json bodyZero = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, 0);
  json bodyNegative = BuildSeriesRuleRequestBody(-1, "", "My Show", false, "", "", "", false, 0);
  CHECK_FALSE(bodyZero.contains("channel_id"));
  CHECK_FALSE(bodyNegative.contains("channel_id"));
}

TEST_CASE("BuildSeriesRuleRequestBody includes channel_id when positive", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(42, "", "My Show", false, "", "", "", false, 0);
  CHECK(body["channel_id"] == 42);
}

TEST_CASE("BuildSeriesRuleRequestBody omits tvg_id when empty", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, 0);
  CHECK_FALSE(body.contains("tvg_id"));
}

TEST_CASE("BuildSeriesRuleRequestBody includes tvg_id when non-empty", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "channel.a.tvg", "My Show", false, "", "", "", false, 0);
  CHECK(body["tvg_id"] == "channel.a.tvg");
}

TEST_CASE("BuildSeriesRuleRequestBody omits mode when recordNewOnly is false", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, 0);
  CHECK_FALSE(body.contains("mode"));
}

TEST_CASE("BuildSeriesRuleRequestBody sends mode=new when recordNewOnly is true", "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", true, "", "", "", false, 0);
  CHECK(body["mode"] == "new");
}

TEST_CASE("BuildSeriesRuleRequestBody omits title_mode/description/description_mode/epg_source_id when all left "
          "at their sentinel -- AddTimer()'s own create path, nothing changed from before this fix",
          "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, 0);
  CHECK_FALSE(body.contains("title_mode"));
  CHECK_FALSE(body.contains("description"));
  CHECK_FALSE(body.contains("description_mode"));
  CHECK_FALSE(body.contains("epg_source_id"));
}

TEST_CASE("BuildSeriesRuleRequestBody echoes back title_mode/description/description_mode/epg_source_id when "
          "given real cached values -- UpdateTimer()'s own edit path, the real bug this fix closes",
          "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", false, "contains", "a real filter", "regex", false, 7);
  CHECK(body["title_mode"] == "contains");
  CHECK(body["description"] == "a real filter");
  CHECK(body["description_mode"] == "regex");
  CHECK(body["epg_source_id"] == 7);
}

TEST_CASE("BuildSeriesRuleRequestBody omits epg_source_id for a non-positive value -- the 'unpinned' sentinel",
          "[TimerRequestBuilder]")
{
  json bodyZero = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, 0);
  json bodyNegative = BuildSeriesRuleRequestBody(0, "", "My Show", false, "", "", "", false, -1);
  CHECK_FALSE(bodyZero.contains("epg_source_id"));
  CHECK_FALSE(bodyNegative.contains("epg_source_id"));
}

TEST_CASE("BuildSeriesRuleRequestBody only sends untagged_is_new when recordNewOnly is also true -- mirrors "
          "Dispatcharr's own gating, sending it standalone would be meaningless server-side",
          "[TimerRequestBuilder]")
{
  json bodyWithoutNewMode = BuildSeriesRuleRequestBody(0, "", "My Show", /*recordNewOnly=*/false, "", "", "",
                                                       /*untaggedIsNew=*/true, 0);
  CHECK_FALSE(bodyWithoutNewMode.contains("untagged_is_new"));

  json bodyWithNewMode = BuildSeriesRuleRequestBody(0, "", "My Show", /*recordNewOnly=*/true, "", "", "",
                                                    /*untaggedIsNew=*/true, 0);
  CHECK(bodyWithNewMode["untagged_is_new"] == true);
}

TEST_CASE("BuildSeriesRuleRequestBody omits untagged_is_new when recordNewOnly is true but untaggedIsNew is false",
          "[TimerRequestBuilder]")
{
  json body = BuildSeriesRuleRequestBody(0, "", "My Show", /*recordNewOnly=*/true, "", "", "",
                                         /*untaggedIsNew=*/false, 0);
  CHECK_FALSE(body.contains("untagged_is_new"));
}

// ---------------------------------------------------------------------
// BuildRecurringRuleUpdateBody
// ---------------------------------------------------------------------

namespace
{
RecurringRuleEditPatch FullPatch()
{
  RecurringRuleEditPatch patch;
  patch.channelId = 42;
  patch.name = "My Show";
  patch.daysOfWeek = std::vector<int>{1, 3, 5};
  patch.startTimeOfDaySeconds = 20 * 3600;
  patch.endTimeOfDaySeconds = 21 * 3600;
  patch.startDate = 1767225600;
  patch.enabled = true;
  return patch;
}
} // namespace

TEST_CASE("BuildRecurringRuleUpdateBody leaves end_date out unless the patch carries one -- the invariant the "
          "end_date-preserving fixes depend on",
          "[TimerRequestBuilder]")
{
  CHECK_FALSE(BuildRecurringRuleUpdateBody(FullPatch()).contains("end_date"));

  RecurringRuleEditPatch withEnd = FullPatch();
  withEnd.endDate = 1769904000;
  CHECK(BuildRecurringRuleUpdateBody(withEnd)["end_date"] == "2026-02-01");
}

TEST_CASE("BuildRecurringRuleUpdateBody sends every field the patch carries", "[TimerRequestBuilder]")
{
  json body = BuildRecurringRuleUpdateBody(FullPatch());
  CHECK(body["channel"] == 42);
  CHECK(body["name"] == "My Show");
  CHECK(body["days_of_week"] == json::array({1, 3, 5}));
  CHECK(body["enabled"] == true);
  CHECK(body["start_time"] == "20:00:00");
  CHECK(body["end_time"] == "21:00:00");
  CHECK(body["start_date"] == "2026-01-01");
  CHECK(body.size() == 7);
}

TEST_CASE("BuildRecurringRuleUpdateBody sends only what changed, and nothing for an empty patch",
          "[TimerRequestBuilder]")
{
  RecurringRuleEditPatch toggle;
  toggle.enabled = false;
  json body = BuildRecurringRuleUpdateBody(toggle);
  CHECK(body == json({{"enabled", false}}));

  CHECK(BuildRecurringRuleUpdateBody(RecurringRuleEditPatch()).empty());
}

// ---------------------------------------------------------------------
// BuildOneTimeRecordingPatchBody
// ---------------------------------------------------------------------

TEST_CASE("BuildOneTimeRecordingPatchBody always sends both start_time and end_time -- confirmed live that "
          "omitting either crashes server-side",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingPatchBody(1000, 2000);
  CHECK(body.contains("start_time"));
  CHECK(body.contains("end_time"));
  CHECK(body.size() == 2);
}

TEST_CASE("BuildOneTimeRecordingPatchBody adds the channel next to both times when one is given -- a bare channel "
          "PATCH is a 500 server-side",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingPatchBody(1000, 2000, /*channelId=*/42);
  CHECK(body["channel"] == 42);
  CHECK(body.contains("start_time"));
  CHECK(body.contains("end_time"));
  CHECK(body.size() == 3);
}

TEST_CASE("BuildOneTimeRecordingPatchBody leaves the channel out for zero or a negative id", "[TimerRequestBuilder]")
{
  CHECK_FALSE(BuildOneTimeRecordingPatchBody(1000, 2000, 0).contains("channel"));
  CHECK_FALSE(BuildOneTimeRecordingPatchBody(1000, 2000, -1).contains("channel"));
}

TEST_CASE("ChannelToSendOnOneTimeEdit sends only a real channel that differs from the recording's own",
          "[TimerRequestBuilder]")
{
  CHECK(ChannelToSendOnOneTimeEdit(/*kodi=*/7, /*current=*/5) == 7);
  CHECK(ChannelToSendOnOneTimeEdit(5, 5) == 0);  // unchanged -- never rewritten
  CHECK(ChannelToSendOnOneTimeEdit(0, 5) == 0);  // no channel
  CHECK(ChannelToSendOnOneTimeEdit(-1, 5) == 0); // PVR_CHANNEL_INVALID_UID
  CHECK(ChannelToSendOnOneTimeEdit(7, 0) == 7);  // the recording's own channel unknown
}

// ---------------------------------------------------------------------
// BuildOneTimeRecordingCreateBody
// ---------------------------------------------------------------------

TEST_CASE("BuildOneTimeRecordingCreateBody sends the requested start time unchanged when it's a real, positive "
          "value",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingCreateBody(/*channelId=*/7, /*start=*/1000, /*end=*/2000, /*now=*/500,
                                              /*includeEpgProgramWindow=*/false);
  CHECK(body["channel"] == 7);
  CHECK(body["start_time"] == IsoFromTime(1000));
  CHECK(body["end_time"] == IsoFromTime(2000));
}

TEST_CASE("BuildOneTimeRecordingCreateBody maps a zero start time to now -- Kodi's own instant-recording sentinel, "
          "the real bug this fixes",
          "[TimerRequestBuilder]")
{
  // Confirmed against Kodi's own real current SDK source, not itself
  // independently reproduced: CPVRTimerInfoTag::CreateFromDate() sends
  // exactly 0 for an instant recording, not the real current time.
  json body = BuildOneTimeRecordingCreateBody(7, /*start=*/0, /*end=*/2000, /*now=*/500,
                                              /*includeEpgProgramWindow=*/false);
  CHECK(body["start_time"] == IsoFromTime(500));
}

TEST_CASE("BuildOneTimeRecordingCreateBody maps a negative start time to now too", "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingCreateBody(7, /*start=*/-1, /*end=*/2000, /*now=*/500,
                                              /*includeEpgProgramWindow=*/false);
  CHECK(body["start_time"] == IsoFromTime(500));
}

TEST_CASE("BuildOneTimeRecordingCreateBody omits custom_properties entirely when includeEpgProgramWindow is false "
          "-- the manual-recording case, unchanged from before this fix",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingCreateBody(7, 1000, 2000, 500, /*includeEpgProgramWindow=*/false);
  CHECK_FALSE(body.contains("custom_properties"));
}

TEST_CASE("BuildOneTimeRecordingCreateBody sends a title/id-free custom_properties.program window when "
          "includeEpgProgramWindow is true -- the real bug this fixes, confirmed live",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingCreateBody(7, 1000, 2000, 500, /*includeEpgProgramWindow=*/true);
  REQUIRE(body.contains("custom_properties"));
  const json& program = body["custom_properties"]["program"];
  CHECK(program["start_time"] == IsoFromTime(1000));
  CHECK(program["end_time"] == IsoFromTime(2000));
  CHECK_FALSE(program.contains("title"));
  CHECK_FALSE(program.contains("id"));
}

TEST_CASE("BuildOneTimeRecordingCreateBody's custom_properties.program window uses the same effective (now-mapped) "
          "start time as the top-level start_time, for Kodi's instant-recording sentinel too",
          "[TimerRequestBuilder]")
{
  json body = BuildOneTimeRecordingCreateBody(7, /*start=*/0, 2000, /*now=*/500, /*includeEpgProgramWindow=*/true);
  CHECK(body["custom_properties"]["program"]["start_time"] == IsoFromTime(500));
}

// ---------------------------------------------------------------------
// BuildSeriesRuleDeleteQuery
// ---------------------------------------------------------------------

TEST_CASE("BuildSeriesRuleDeleteQuery always includes title", "[TimerRequestBuilder]")
{
  CHECK(BuildSeriesRuleDeleteQuery("My Show", "", 0) == "?title=My%20Show");
}

TEST_CASE("BuildSeriesRuleDeleteQuery appends tvg_id only when non-empty", "[TimerRequestBuilder]")
{
  CHECK(BuildSeriesRuleDeleteQuery("My Show", "channel.a.tvg", 0) == "?title=My%20Show&tvg_id=channel.a.tvg");
}

TEST_CASE("BuildSeriesRuleDeleteQuery appends epg_source_id only when positive -- the real data-loss bug this "
          "fixes (omitting it matched and removed every EPG source's copy of the rule)",
          "[TimerRequestBuilder]")
{
  CHECK(BuildSeriesRuleDeleteQuery("My Show", "channel.a.tvg", 7) ==
        "?title=My%20Show&tvg_id=channel.a.tvg&epg_source_id=7");
  CHECK(BuildSeriesRuleDeleteQuery("My Show", "", 0) == "?title=My%20Show");
  CHECK(BuildSeriesRuleDeleteQuery("My Show", "", -1) == "?title=My%20Show");
}

TEST_CASE("BuildSeriesRuleDeleteQuery encodes a tvg_id as it does a title", "[TimerRequestBuilder]")
{
  // A tvg_id is a provider's free text: "&", "+", "=", "#", a space or a percent sign would otherwise end or rewrite
  // the parameter, deleting a different rule (or none) than the one meant.
  CHECK(BuildSeriesRuleDeleteQuery("Show", "a&b=c", 0) == "?title=Show&tvg_id=a%26b%3Dc");
  CHECK(BuildSeriesRuleDeleteQuery("Show", "x y+z", 0) == "?title=Show&tvg_id=x%20y%2Bz");
  CHECK(BuildSeriesRuleDeleteQuery("Show", "50%#", 7) == "?title=Show&tvg_id=50%25%23&epg_source_id=7");
}
