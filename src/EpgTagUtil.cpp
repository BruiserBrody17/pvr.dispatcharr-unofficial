#include "EpgTagUtil.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace dispatcharr
{

namespace
{

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
    std::string lower = category;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto& [keyword, mask] : kKeywordToMask)
    {
      if (lower.find(keyword) != std::string::npos)
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

} // namespace dispatcharr
