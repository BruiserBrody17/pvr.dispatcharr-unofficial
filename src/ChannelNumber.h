#pragma once

#include <string>

namespace dispatcharr
{

// What Dispatcharr's `channel_number` -- a FloatField server-side
// (apps/channels/models.py), so a channel can be numbered 5.1 as easily as
// 5 -- turns into on this addon's side. Fix for a real, live-confirmed bug
// (docs/OPEN_ITEMS.md, confirmed 2026-09-30): Channel::channelNumber used
// to be a plain int, and nlohmann::json's get<int>() silently truncates a
// floating-point value rather than throwing, so a 5.1 subchannel became
// channel 5 -- colliding with a real channel 5, and losing its own guide,
// since GetEPGForChannel() keys the XMLTV cache by this number's text.
struct ChannelNumberParts
{
  // Kodi's channel number: the integer part.
  int whole = 0;
  // Kodi's sub-channel number: the digits after the decimal point read as
  // an integer ("5.1" -> 1, "5.25" -> 25). Lossy for a leading-zero
  // fraction ("5.05" and "5.5" both give 5), and capped at the first nine
  // digits -- neither matters for the ATSC-style 5.1/5.2 numbering this
  // exists for, and Kodi's own display keeps working either way. Nothing
  // that must tell channels apart (the guide cache key, collision
  // detection) uses this; they use `key`.
  int sub = 0;
  // The exact text Dispatcharr's own XMLTV export puts in `<channel id>`
  // for this number (apps/output/epg.py's format_channel_number(): an int
  // for a whole value, "5.1" for a fractional one) -- the key
  // PVRDispatcharr::GetEPGForChannel() looks the guide up by. Empty when
  // the channel has no usable number (see SplitChannelNumber()).
  std::string key;
};

// Splits a parsed channel number into what Kodi and the guide lookup each
// need. A value Kodi can't display as a channel number -- below 1,
// non-finite, or too large for its own int-sized channel numbers -- gets
// whole and sub 0. The key is separate from that: it is whatever text
// Dispatcharr's export puts in `<channel id>` for the value, so a channel
// numbered 0, -3 or 0.5 still finds its guide even though Kodi shows it as
// channel 0. Only a value that isn't a finite, sanely sized number leaves the
// key empty, meaning "no usable number" (GetEPGForChannel() declines to look
// such a channel up at all). A channel with NO number is a different case
// again -- see FormatChannelGuideKey().
//
// `key` reproduces Python's own str(float) for the fractional case: the
// shortest decimal string that reads back as exactly the same double
// (Python's repr() guarantee). An earlier draft used a fixed precision,
// which would disagree with Dispatcharr's own text for a value that
// genuinely needs 16-17 digits to round-trip. Formatted through a
// classic-locale stream, never printf/to_string, so a Kodi running under
// a comma-decimal locale can't turn "5.1" into "5,1".
ChannelNumberParts SplitChannelNumber(double value);

// The guide-lookup key for a whole channel, which is what call sites should
// use: a channel whose channel_number is null is exported under its own id
// (apps/output/epg.py), not under a number, so it needs the channel's id as
// well as its number -- and, since Dispatcharr writes both into the same
// `<channel id>` space, a null-numbered channel 77 and a channel numbered 77
// share a key and are caught as ambiguous like any other collision.
// `hasChannelNumber` is false only for a null/absent number; a literal 0 is a
// number.
std::string FormatChannelGuideKey(bool hasChannelNumber, double value, int channelId);

// Convenience accessors over SplitChannelNumber() for call sites that
// only need one part.
std::string FormatChannelNumberKey(double value);
int WholeChannelNumber(double value);
int SubChannelNumber(double value);

} // namespace dispatcharr
