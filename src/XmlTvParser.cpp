#include "XmlTvParser.h"

#include "TimeUtil.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace dispatcharr
{

namespace
{

// Parses a run of decimal digits starting at `start`, advancing `end` past
// them and writing the parsed value to `out`. Returns false (leaving `out`
// untouched) when there are no digits at `start` at all, or when the run
// doesn't fit in an int (std::stoi throws std::out_of_range rather than
// clamping -- confirmed, see ParseEpisodeNum's own comment on this same
// point) -- either way, the caller treats it as "not a form this
// recognizes" rather than guessing. Shared by ParseOnscreenEpisodeNum's
// two digit runs below.
bool ParseDigitsAt(const std::string& s, size_t start, size_t& end, int& out)
{
  end = start;
  while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end])))
    ++end;
  if (end == start)
    return false;
  try
  {
    out = std::stoi(s.substr(start, end - start));
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

} // namespace

std::string NormalizeXmlTvDateToW3C(const std::string& dateStr)
{
  // Bare year-only, or already hyphenated -- see this function's own
  // header comment for why both are left as-is.
  if (dateStr.size() <= 4 || dateStr[4] == '-')
    return dateStr;

  if (dateStr.size() != 6 && dateStr.size() != 8)
    return dateStr;
  if (!std::all_of(dateStr.begin(), dateStr.end(), [](unsigned char c) { return std::isdigit(c); }))
    return dateStr;

  std::string result = dateStr.substr(0, 4) + "-" + dateStr.substr(4, 2);
  if (dateStr.size() == 8)
    result += "-" + dateStr.substr(6, 2);
  return result;
}

void ParseEpisodeNum(const std::string& value, int& season, int& episode)
{
  season = -1;
  episode = -1;

  std::stringstream ss(value);
  std::string field;
  int index = 0;
  while (std::getline(ss, field, '.') && index < 2)
  {
    // Per-field "N/total" form (this field's own position out of a
    // known total, e.g. "3/10" meaning the 4th of 10, 0-indexed) --
    // drop everything from the first '/' onward within THIS field
    // only. Fix for a real, confirmed bug (found via a project-wide
    // review): the previous version dropped everything after the
    // FIRST '/' in the WHOLE value up front, assuming a '/' could only
    // ever appear on the trailing part-number field -- but per the
    // XMLTV DTD, the season and episode fields can each independently
    // carry their own "/total" too. "1/3.4/10." (season 1 of 3,
    // episode 4 of 10) lost its episode entirely under the old logic:
    // truncated to just "1" at the very first slash, before the "."
    // split even ran.
    size_t slashPos = field.find('/');
    if (slashPos != std::string::npos)
      field = field.substr(0, slashPos);

    if (!field.empty())
    {
      try
      {
        int parsed = std::stoi(field);
        // Corrected 2026-09-27, a 51st-pass audit, fixing a real,
        // confirmed factual error in this comment's own original wording,
        // found via a project-wide review, confirmed by direct
        // compilation (g++ -std=c++17): std::stoi does NOT clamp an
        // overlong digit string to INT_MAX -- it throws std::out_of_range
        // for any value outside int's range (confirmed:
        // "99999999999999999999" and "2147483648" both throw;
        // "2147483647" itself parses to INT_MAX cleanly), so an overlong
        // string is already caught by the catch block below, not reaching
        // this check at all. The only way parsed == INT_MAX is reachable
        // is a field std::stoi actually parses to exactly that value (the
        // literal "2147483647" itself, but also e.g. a leading-zero-padded
        // or leading-"+"-signed variant of it, or one with trailing
        // non-digit garbage stoi simply stops at -- a 52nd-pass audit
        // precision correction) -- still
        // be signed-integer overflow (undefined behavior in C++) for that
        // exact value, found via a project-wide UB review, not reproduced
        // live. No real season/episode number is ever legitimately this
        // large, so treat it the same as any other unparseable value:
        // leave as unknown rather than risk the overflow.
        if (parsed < std::numeric_limits<int>::max())
        {
          if (index == 0)
            season = parsed + 1; // xmltv_ns is 0-indexed
          else
            episode = parsed + 1;
        }
      }
      catch (const std::exception&)
      {
        // leave as unknown
      }
    }
    ++index;
  }
}

void ParseOnscreenEpisodeNum(const std::string& value, int& season, int& episode)
{
  season = -1;
  episode = -1;

  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return;
  size_t last = value.find_last_not_of(" \t\r\n");
  std::string trimmed = value.substr(begin, last - begin + 1);

  size_t pos = 0;
  int parsedSeason = -1;
  if (trimmed[0] == 'S' || trimmed[0] == 's')
  {
    size_t digitsEnd = 0;
    if (!ParseDigitsAt(trimmed, 1, digitsEnd, parsedSeason))
      return; // "S" with nothing (parseable) after it -- not a recognized form
    pos = digitsEnd;
  }

  if (pos >= trimmed.size() || (trimmed[pos] != 'E' && trimmed[pos] != 'e'))
    return; // no episode marker at this position -- not a recognized form

  size_t episodeDigitsEnd = 0;
  int parsedEpisode = -1;
  if (!ParseDigitsAt(trimmed, pos + 1, episodeDigitsEnd, parsedEpisode) || episodeDigitsEnd != trimmed.size())
    return; // no digits after "E", or trailing garbage after them -- don't guess

  season = parsedSeason;
  episode = parsedEpisode;
}

time_t ParseXmlTvTime(const std::string& timeStr)
{
  if (timeStr.size() < 14)
    return 0;

  tm tmVal{};
  try
  {
    tmVal.tm_year = std::stoi(timeStr.substr(0, 4)) - 1900;
    tmVal.tm_mon = std::stoi(timeStr.substr(4, 2)) - 1;
    tmVal.tm_mday = std::stoi(timeStr.substr(6, 2));
    tmVal.tm_hour = std::stoi(timeStr.substr(8, 2));
    tmVal.tm_min = std::stoi(timeStr.substr(10, 2));
    tmVal.tm_sec = std::stoi(timeStr.substr(12, 2));
  }
  catch (const std::exception&)
  {
    return 0;
  }

  // Same bounds TimeFromIso() applies, for the same reason (found by the 2026-10-04 second hardening
  // sweep, proven: "20261345256161 +0000" came back as a time in February of the next year, because
  // PortableTimeGm() normalizes a field out of range the way timegm() does). A leap second is tolerated.
  if (tmVal.tm_mon < 0 || tmVal.tm_mon > 11 || tmVal.tm_mday < 1 || tmVal.tm_mday > 31 || tmVal.tm_hour < 0 ||
      tmVal.tm_hour > 23 || tmVal.tm_min < 0 || tmVal.tm_min > 59 || tmVal.tm_sec < 0 || tmVal.tm_sec > 60 ||
      !IsValidCivilDate(tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday))
    return 0;

  time_t utcTime = PortableTimeGmSaturating(&tmVal);

  // The offset follows the 14 digits, after any spaces: XMLTV allows it directly attached too
  // ("20370828230612+0530"), which reading only after a space treated as UTC.
  size_t offsetPos = 14;
  while (offsetPos < timeStr.size() && timeStr[offsetPos] == ' ')
    ++offsetPos;
  if (offsetPos < timeStr.size())
  {
    std::string offsetStr = timeStr.substr(offsetPos);
    if (offsetStr.size() >= 5 && (offsetStr[0] == '+' || offsetStr[0] == '-'))
    {
      try
      {
        int sign = (offsetStr[0] == '-') ? -1 : 1;
        int offsetHours = std::stoi(offsetStr.substr(1, 2));
        int offsetMinutes = std::stoi(offsetStr.substr(3, 2));
        // A real offset is within +-23:59; "+9999" used to shift the result by over four days.
        if (offsetHours >= 0 && offsetHours <= 23 && offsetMinutes >= 0 && offsetMinutes <= 59)
          utcTime -= sign * (offsetHours * 3600 + offsetMinutes * 60);
      }
      catch (const std::exception&)
      {
        // Ignore a malformed offset; fall back to the unadjusted UTC time.
      }
    }
  }
  return utcTime;
}

namespace
{

// Appends a name to a comma-joined credits string (EPG_STRING_TOKEN_SEPARATOR),
// skipping empty names.
void AppendCredit(std::string& joined, const std::string& name)
{
  if (name.empty())
    return;
  if (!joined.empty())
    joined += ",";
  joined += name;
}

} // namespace

bool XmlTvParser::Parse(const std::string& xmlContent, std::unordered_map<std::string, std::vector<EpgEntry>>& out,
                        std::string& error)
{
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(xmlContent.data(), xmlContent.size());
  if (!result)
  {
    error = std::string("Failed to parse XMLTV document: ") + result.description();
    return false;
  }

  pugi::xml_node tv = doc.child("tv");
  if (!tv)
  {
    error = "XMLTV document has no <tv> root element";
    return false;
  }

  out.clear();
  for (pugi::xml_node programme : tv.children("programme"))
  {
    EpgEntry entry;
    entry.channelXmltvId = programme.attribute("channel").as_string();
    if (entry.channelXmltvId.empty())
      continue;

    entry.startTime = ParseXmlTvTime(programme.attribute("start").as_string());
    entry.endTime = ParseXmlTvTime(programme.attribute("stop").as_string());
    if (entry.startTime == 0 || entry.endTime == 0)
      continue;
    // A programme that ends at or before it starts has no airtime to show: a feed
    // with a swapped or copied stop (found by the 2026-10-04 hardening sweep to be
    // kept as-is) would hand Kodi a tag of zero or negative length, and the
    // overlap arithmetic that links recordings to guide entries (EpgProgramMatch.h)
    // treats a reversed window as matching nothing.
    if (entry.endTime <= entry.startTime)
      continue;

    entry.title = programme.child("title").text().as_string();
    entry.subtitle = programme.child("sub-title").text().as_string();
    entry.description = programme.child("desc").text().as_string();

    for (pugi::xml_node category : programme.children("category"))
    {
      std::string value = category.text().as_string();
      if (!value.empty())
        entry.categories.push_back(std::move(value));
    }

    entry.iconPath = programme.child("icon").attribute("src").as_string();

    pugi::xml_node credits = programme.child("credits");
    if (credits)
    {
      for (pugi::xml_node director : credits.children("director"))
        AppendCredit(entry.director, director.text().as_string());
      for (pugi::xml_node writer : credits.children("writer"))
        AppendCredit(entry.writer, writer.text().as_string());
      for (pugi::xml_node adapter : credits.children("adapter"))
        AppendCredit(entry.writer, adapter.text().as_string());
      // Everyone else visible on screen (as opposed to behind the camera)
      // goes into "cast", matching how most PVR clients bucket xmltv credits.
      for (const char* role : {"actor", "presenter", "guest", "producer", "commentator", "composer", "editor"})
      {
        for (pugi::xml_node person : credits.children(role))
          AppendCredit(entry.cast, person.text().as_string());
      }
    }

    std::string dateStr = programme.child("date").text().as_string();
    if (dateStr.size() >= 4)
    {
      entry.firstAired = NormalizeXmlTvDateToW3C(dateStr);
      try
      {
        entry.year = std::stoi(dateStr.substr(0, 4));
      }
      catch (const std::exception&)
      {
        entry.year = 0;
      }
    }

    entry.isNew = static_cast<bool>(programme.child("new"));
    entry.isPremiere = static_cast<bool>(programme.child("premiere"));
    entry.isLive = static_cast<bool>(programme.child("live"));

    bool matchedXmltvNs = false;
    for (pugi::xml_node episodeNum : programme.children("episode-num"))
    {
      if (std::string(episodeNum.attribute("system").as_string()) == "xmltv_ns")
      {
        ParseEpisodeNum(episodeNum.text().as_string(), entry.seasonNumber, entry.episodeNumber);
        matchedXmltvNs = true;
        break;
      }
    }
    // Fallback for an episode-only programme with no season -- see
    // ParseOnscreenEpisodeNum's own comment in XmlTvParser.h. Only tried
    // when xmltv_ns is absent entirely, matching Dispatcharr's own export
    // behavior (it emits xmltv_ns whenever both season and episode are
    // known, onscreen only as a substitute when season isn't).
    if (!matchedXmltvNs)
    {
      for (pugi::xml_node episodeNum : programme.children("episode-num"))
      {
        if (std::string(episodeNum.attribute("system").as_string()) == "onscreen")
        {
          ParseOnscreenEpisodeNum(episodeNum.text().as_string(), entry.seasonNumber, entry.episodeNumber);
          break;
        }
      }
    }

    out[entry.channelXmltvId].push_back(std::move(entry));
  }

  return true;
}

} // namespace dispatcharr
