#include "ChannelNumber.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace dispatcharr
{

namespace
{

// The largest whole part Kodi's int-sized channel numbers can carry.
// Compared against the value itself (not its whole part), so 2147483647.9
// is still in range and 2147483648.0 is not.
constexpr double kMaxChannelNumberExclusive = 2147483648.0;

// Longest fractional digit run read into the sub-channel number: nine
// digits always fit an int.
constexpr size_t kMaxSubChannelDigits = 9;

// Larger magnitudes aren't channel numbers, and Python would print an integer
// this big (or a float this large) as something this code doesn't reproduce.
constexpr double kMaxGuideKeyMagnitude = 1e15;

std::string ShortestRoundTripText(double value)
{
  for (int precision = 1; precision <= 17; ++precision)
  {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(precision) << value;
    std::string text = out.str();

    std::istringstream in(text);
    in.imbue(std::locale::classic());
    double readBack = 0.0;
    in >> readBack;
    if (!in.fail() && readBack == value)
      return text;
  }
  return std::string();
}

// The text Python's own `str()` gives a float after Dispatcharr's
// format_channel_number() (apps/channels/utils.py) has turned a whole-valued
// one into an int: "5", "0", "-3", "5.1", "0.5". Empty when the value isn't
// finite or is too large to be a channel number at all.
std::string GuideKeyText(double value)
{
  if (!std::isfinite(value) || std::fabs(value) >= kMaxGuideKeyMagnitude)
    return std::string();
  if (value == std::floor(value))
    return std::to_string(static_cast<long long>(value));
  return ShortestRoundTripText(value);
}

} // namespace

ChannelNumberParts SplitChannelNumber(double value)
{
  ChannelNumberParts parts;
  // The key is independent of what Kodi can display: Dispatcharr exports a
  // channel numbered 0, -3 or 0.5 under exactly that text too (a literal 0 is
  // accepted by ChannelSerializer, which sets no minimum), and the guide
  // lookup has to find it there even though Kodi's channel number for it is
  // 0. Only a value that can't be displayed, or isn't a number at all, leaves
  // the key empty.
  parts.key = GuideKeyText(value);
  if (!std::isfinite(value) || value < 1.0 || value >= kMaxChannelNumberExclusive)
    return parts;

  const double wholePart = std::floor(value);
  parts.whole = static_cast<int>(wholePart);
  if (value == wholePart)
    return parts;

  // Every value that reaches here has a non-zero fraction and a whole part
  // of at least 1, so the shortest round-trip text always has a decimal
  // point and never an exponent; anything else means the round trip itself
  // failed, which is treated as "no usable number" rather than guessed at.
  const size_t dot = parts.key.find('.');
  if (dot == std::string::npos || parts.key.find_first_of("eE") != std::string::npos)
  {
    parts.whole = 0;
    parts.key.clear();
    return parts;
  }

  int sub = 0;
  size_t digits = 0;
  for (size_t i = dot + 1; i < parts.key.size() && digits < kMaxSubChannelDigits; ++i, ++digits)
    sub = sub * 10 + (parts.key[i] - '0');
  parts.sub = sub;
  return parts;
}

std::string FormatChannelGuideKey(bool hasChannelNumber, double value, int channelId)
{
  // Dispatcharr's export uses the channel's own id when it has no number at
  // all (apps/output/epg.py: `str(formatted_channel_number) if
  // formatted_channel_number != "" else str(channel.id)`).
  if (!hasChannelNumber)
    return std::to_string(channelId);
  return GuideKeyText(value);
}

std::string FormatChannelNumberKey(double value)
{
  return SplitChannelNumber(value).key;
}

int WholeChannelNumber(double value)
{
  return SplitChannelNumber(value).whole;
}

int SubChannelNumber(double value)
{
  return SplitChannelNumber(value).sub;
}

} // namespace dispatcharr
