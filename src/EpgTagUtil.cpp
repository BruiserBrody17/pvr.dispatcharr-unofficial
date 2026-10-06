#include "EpgTagUtil.h"

#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace dispatcharr
{

namespace
{

// Mirrors kodi/c-api/addon-instance/pvr/pvr_epg.h's EPG_TAG_FLAG_* values --
// see ComputeEpgTagFlags()'s own doc comment in EpgTagUtil.h for why these
// are duplicated as raw bit shifts rather than included directly.
constexpr uint32_t kFlagIsSeries = (1u << 0);
constexpr uint32_t kFlagIsNew = (1u << 1);
constexpr uint32_t kFlagIsPremiere = (1u << 2);
constexpr uint32_t kFlagIsLive = (1u << 4);

// Mirrors kodi/c-api/addon-instance/pvr/pvr_epg.h's EPG_EVENT_CONTENTMASK_*
// values -- the ETSI EN 300 468 DVB-SI content-descriptor top nibble, an
// external broadcast standard Kodi's API just mirrors 1:1, not a
// Kodi-specific value that could drift independently of that standard.
// Kept as raw hex (rather than including Kodi's header directly) so this
// file has zero Kodi SDK dependency; if these ever need rechecking against
// Kodi's own header, see pvr_epg.h's own EPG_EVENT_CONTENTMASK enum.
constexpr int kContentMaskMovieDrama = 0x10;
constexpr int kContentMaskNewsCurrentAffairs = 0x20;
constexpr int kContentMaskShow = 0x30;
constexpr int kContentMaskSports = 0x40;
constexpr int kContentMaskChildrenYouth = 0x50;
constexpr int kContentMaskMusicBalletDance = 0x60;
constexpr int kContentMaskArtsCulture = 0x70;
constexpr int kContentMaskSocialPoliticalEconomics = 0x80;
constexpr int kContentMaskEducationalScience = 0x90;
constexpr int kContentMaskLeisureHobbies = 0xA0;

// See IsWithinCatchupWindow()'s own comment: matches Dispatcharr's own
// migration 0038 backfill default for a missing/unknown archive duration.
constexpr int kDefaultCatchupDaysWhenUnknown = 7;

// True when `keyword` occurs in `lower` starting at a word boundary (the
// start of the string, or right after a non-alphanumeric character) --
// not merely anywhere inside a longer word. Prefix-only on purpose, so
// "sport" still matches "Sports"/"Sportscast" but "arts" no longer
// matches "Darts" (flagged from a 23rd-pass audit, docs/OPEN_ITEMS.md).
bool ContainsAtWordStart(const std::string& lower, const std::string& keyword)
{
  size_t pos = lower.find(keyword);
  while (pos != std::string::npos)
  {
    if (pos == 0 || !IsAsciiAlnum(static_cast<unsigned char>(lower[pos - 1])))
      return true;
    pos = lower.find(keyword, pos + 1);
  }
  return false;
}

} // namespace

bool MapCategoriesToGenreType(const std::vector<std::string>& categories, int& genreType)
{
  static const std::pair<const char*, int> kKeywordToMask[] = {
      {"movie", kContentMaskMovieDrama},
      {"film", kContentMaskMovieDrama},
      {"drama", kContentMaskMovieDrama},
      {"news", kContentMaskNewsCurrentAffairs},
      {"current affairs", kContentMaskNewsCurrentAffairs},
      {"sport", kContentMaskSports},
      // Must precede "arts" below: a first-match-wins table, and a
      // word-start "arts" would otherwise claim "Martial arts".
      {"martial arts", kContentMaskSports},
      {"children", kContentMaskChildrenYouth},
      {"kids", kContentMaskChildrenYouth},
      {"youth", kContentMaskChildrenYouth},
      {"cartoon", kContentMaskChildrenYouth},
      {"music", kContentMaskMusicBalletDance},
      {"ballet", kContentMaskMusicBalletDance},
      {"dance", kContentMaskMusicBalletDance},
      {"concert", kContentMaskMusicBalletDance},
      {"arts", kContentMaskArtsCulture},
      {"culture", kContentMaskArtsCulture},
      {"politic", kContentMaskSocialPoliticalEconomics},
      {"social", kContentMaskSocialPoliticalEconomics},
      {"economic", kContentMaskSocialPoliticalEconomics},
      {"documentary", kContentMaskEducationalScience},
      {"science", kContentMaskEducationalScience},
      {"education", kContentMaskEducationalScience},
      {"nature", kContentMaskEducationalScience},
      {"travel", kContentMaskLeisureHobbies},
      {"cooking", kContentMaskLeisureHobbies},
      {"hobbies", kContentMaskLeisureHobbies},
      {"leisure", kContentMaskLeisureHobbies},
      {"game show", kContentMaskShow},
      {"talk show", kContentMaskShow},
      {"reality", kContentMaskShow},
      {"variety", kContentMaskShow},
      {"comedy", kContentMaskShow},
  };

  for (const std::string& category : categories)
  {
    std::string lower = ToLower(category);
    for (const auto& [keyword, mask] : kKeywordToMask)
    {
      if (ContainsAtWordStart(lower, keyword))
      {
        genreType = mask;
        return true;
      }
    }
  }
  return false;
}

uint32_t ComputeBroadcastId(int channelUid, time_t startTime)
{
  return static_cast<unsigned int>(channelUid) * 2654435761u + static_cast<uint32_t>(startTime);
}

bool ShouldIncludeEpisodeDates(int seasonNumber, int episodeNumber)
{
  return seasonNumber > 0 || episodeNumber > 0;
}

bool CategoriesIndicateSeries(const std::vector<std::string>& categories)
{
  return std::any_of(categories.begin(), categories.end(),
                     [](const std::string& c) { return ToLower(c).find("series") != std::string::npos; });
}

std::string JoinCategories(const std::vector<std::string>& categories, const std::string& separator)
{
  std::string joined;
  for (const std::string& category : categories)
  {
    if (!joined.empty())
      joined += separator;
    joined += category;
  }
  return joined;
}

uint32_t ComputeEpgTagFlags(bool isNew, bool isPremiere, bool isLive, int seasonNumber, int episodeNumber,
                            bool categorySaysSeries)
{
  uint32_t flags = 0;
  if (isNew)
    flags |= kFlagIsNew;
  if (isPremiere)
    flags |= kFlagIsPremiere;
  if (isLive)
    flags |= kFlagIsLive;
  if (seasonNumber > 0 || episodeNumber > 0 || categorySaysSeries)
    flags |= kFlagIsSeries;
  return flags;
}

bool IsWithinCatchupWindow(bool catchupEnabled, int catchupDays, time_t tagStartTime, time_t now)
{
  if (!catchupEnabled)
    return false;
  if (tagStartTime > now)
    return false; // hasn't aired yet
  // A non-positive catchupDays here is Dispatcharr's own "archive depth
  // unknown", not "no catch-up" -- confirmed against Dispatcharr's own
  // real current upstream source (a 29th-pass audit, not itself
  // independently reproduced): a stream gets is_catchup=True together
  // with catchup_days=0 whenever the provider sets tv_archive=1 but
  // leaves out or zeroes tv_archive_duration (apps/m3u/tasks.py), and the
  // channel-level rollup is `max_days or 0` (apps/channels/signals.py).
  // Session creation itself (apps/timeshift/api_views.py's
  // get_channel_catchup_streams()) never checks catchup_days at all, and
  // Dispatcharr's own stream-ordering helper explicitly treats a
  // non-positive value as "stay preferred", never restrictive
  // (apps/timeshift/helpers.py's order_catchup_streams_for_timestamp()).
  // Without this fallback, a channel in exactly this state reported
  // hasarchive=true to Kodi (GetChannels() -> SetHasArchive()) while every
  // guide entry on it was refused here -- catch-up silently unusable for
  // a channel the server would have actually served. kDefaultCatchupDaysWhenUnknown
  // matches Dispatcharr's own migration 0038, which backfills a missing
  // duration as 7 days, not 0.
  int effectiveCatchupDays = catchupDays > 0 ? catchupDays : kDefaultCatchupDaysWhenUnknown;
  time_t oldestAllowed = now - static_cast<time_t>(effectiveCatchupDays) * 24 * 60 * 60;
  if (tagStartTime < oldestAllowed)
    return false; // outside the provider's archive retention window
  return true;
}

bool ShouldOfferCatchup(bool channelCatchupEnabled, bool catchupEnabledGlobally, bool catchupEnabledForCurrentUser)
{
  return channelCatchupEnabled && catchupEnabledGlobally && catchupEnabledForCurrentUser;
}

int ComputeGuidePrevDays(const std::vector<int>& offeredCatchupDays)
{
  constexpr int kServerMaxPrevDays = 30; // Dispatcharr clamps prev_days to this
  int days = 0;
  for (int d : offeredCatchupDays)
  {
    // Every entry is a channel that DOES offer catch-up (the caller filters with
    // ShouldOfferCatchup()). A depth of 0 or less means the server did not say
    // how far back it keeps programmes -- the same "unknown" IsWithinCatchupWindow()
    // above treats as kDefaultCatchupDaysWhenUnknown. Taking it as 0 here meant an
    // install whose catch-up channels report no depth never sent prev_days at all,
    // so the guide never held the already-aired programmes catch-up plays from
    // (found by the 2026-10-04 hardening sweep).
    days = std::max(days, d > 0 ? d : kDefaultCatchupDaysWhenUnknown);
  }
  return std::min(days, kServerMaxPrevDays);
}

} // namespace dispatcharr
