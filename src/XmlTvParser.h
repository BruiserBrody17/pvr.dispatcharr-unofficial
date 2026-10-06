#pragma once

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace dispatcharr
{

struct EpgEntry
{
  std::string channelXmltvId;
  std::string title;
  std::string subtitle; // xmltv <sub-title> -- episode title, not a plot summary
  std::string description;
  std::vector<std::string> categories; // xmltv <category>, 0+ (can repeat)
  std::string iconPath;                // xmltv <icon src="..."> -- per-programme
                                       // poster/artwork, distinct from the
                                       // channel's own logo
  std::string cast;                    // joined actor/presenter/guest/producer/commentator/
                                       // composer/editor credits, comma-separated
                                       // (EPG_STRING_TOKEN_SEPARATOR)
  std::string director;                // joined <credits><director>, comma-separated
  std::string writer;                  // joined <credits><writer>/<adapter>, comma-separated
  int year = 0;                        // parsed from <date>'s leading YYYY; 0 = unknown
  std::string firstAired;              // <date> normalized via NormalizeXmlTvDateToW3C()
                                       // below (YYYY-MM-DD or just YYYY), empty = unknown
  bool isNew = false;                  // <new/> present
  bool isPremiere = false;             // <premiere/> present
  bool isLive = false;                 // <live/> present
  time_t startTime = 0;
  time_t endTime = 0;
  int seasonNumber = -1;  // -1 = unknown; xmltv_ns is 0-indexed, we store as
  int episodeNumber = -1; // human-readable (1-indexed) once parsed
};

// Normalizes an XMLTV <date> value into the "YYYY-MM-DD"/"YYYY-MM" shape
// Kodi's own CDateTime::SetFromW3CDate() needs to read a month/day out of
// it. The XMLTV DTD's own <date> format is a plain digit-string prefix of
// "YYYYMMDDhhmmss" (see this file's own top-of-file comment) -- no
// hyphens at all -- but SetFromW3CDate() only reads month/day when the
// string is at least 10 characters long AND has hyphens at exactly
// positions 4 and 7 ("YYYY-MM-DD"); a raw 8-digit "YYYYMMDD" is only 8
// characters, so it silently defaults month/day to January 1st despite
// carrying a real one -- a real display bug (confirmed against Kodi's
// own XBDateTime.cpp source, not yet reproduced against a live provider
// emitting this fully spec-compliant, unhyphenated form). A bare 4-digit
// "YYYY" (year-only) and an already-hyphenated value are both left
// unchanged -- the former already round-trips correctly through
// SetFromW3CDate() as-is (year known, month/day intentionally default to
// January 1st), matching EpgEntry::firstAired's own documented "YYYY-MM-DD
// or just YYYY" contract. Only a plain 6-digit ("YYYYMM") or 8-digit
// ("YYYYMMDD") all-digit value gets hyphens inserted; anything else
// (malformed, or an XMLTV form this addon has no real coverage for) is
// returned unchanged rather than guessed at.
std::string NormalizeXmlTvDateToW3C(const std::string& dateStr);

// Parses an xmltv_ns <episode-num system="xmltv_ns"> value
// ("season.episode.part", each component 0-indexed and optional, e.g.
// "2.4." or ".4." or "2..") into 1-indexed season/episode numbers, -1
// for a component that's absent or unparseable. Each of the three
// dot-separated fields (only the first two are used here) can
// independently carry its own "/total" suffix per the XMLTV DTD (e.g.
// "1/3.4/10." means season 1 of 3, episode 4 of 10) -- stripped per
// field before parsing the leading number, not once up front against
// the whole value (a real, confirmed bug this fixes: doing it once
// against the whole value assumed a '/' could only ever appear on the
// trailing part-number field, silently truncating and losing the
// episode number whenever the season field itself carried its own
// "/total" first).
void ParseEpisodeNum(const std::string& value, int& season, int& episode);

// Parses a plain <episode-num system="onscreen"> value ("as it would
// appear on screen", per the XMLTV DTD -- unlike xmltv_ns's 0-indexed
// dot-separated triple, these are freeform, already-1-indexed human-
// readable numbers) into season/episode, as a fallback for when no
// xmltv_ns entry is present at all. Flagged from a 23rd-pass audit
// (docs/OPEN_ITEMS.md), confirmed against Dispatcharr's own real current
// upstream source: its XMLTV export (apps/output/epg.py) only emits
// xmltv_ns when BOTH season and episode are known -- an episode-only
// programme (no season) is instead exported solely as an onscreen value
// like "E12", which this addon never read at all before this, silently
// losing that episode's own number, its EPG_TAG_FLAG_IS_SERIES flag, and
// (per ShouldIncludeEpisodeDates()) its Year/FirstAired display.
// Recognizes "S<n>E<m>" (season+episode) and a bare "E<m>" (episode
// only, season left -1/unknown) case-insensitively, with leading/
// trailing whitespace ignored; anything else -- a free-text form this
// fallback doesn't recognize, or digits followed by trailing garbage --
// leaves both as -1 rather than guessing at it, the same "don't guess"
// convention ParseEpisodeNum() above already uses. Neither field gets
// xmltv_ns's own +1 adjustment: an onscreen number is already exactly
// what it says.
void ParseOnscreenEpisodeNum(const std::string& value, int& season, int& episode);

// Parses an XMLTV programme start/stop attribute, "YYYYMMDDHHMMSS" optionally
// followed by a space and a "+HHMM"/"-HHMM" UTC offset, into a UTC time_t. The
// offset is applied (a "-0700" time is seven hours later in UTC); a malformed
// or absent offset leaves the time read as UTC. Returns 0 for anything shorter
// than the 14 digits or with a non-numeric field -- the parser skips such a
// programme. Dispatcharr's own export always writes " +0000", but an EPG
// source relayed through it may not. Exposed (2026-10-03) so the offset
// handling is tested directly, not only through whole documents.
time_t ParseXmlTvTime(const std::string& timeStr);

// Parses a Dispatcharr XMLTV guide document (as returned by
// GET {base}/output/epg) into programme entries keyed by the XMLTV
// <channel id="..."> value. Confirmed against a live instance: Dispatcharr
// uses the channel's channel_number here, NOT its tvg_id (e.g. Channel G
// with channel_number 1 and tvg_id "ChannelG.example" appears in the XMLTV
// as <channel id="1">) -- match on Channel::channelNumber, not tvgId.
class XmlTvParser
{
public:
  static bool Parse(const std::string& xmlContent, std::unordered_map<std::string, std::vector<EpgEntry>>& out,
                    std::string& error);
};

} // namespace dispatcharr
