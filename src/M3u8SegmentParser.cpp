#include "M3u8SegmentParser.h"

#include <cmath>
#include <stdexcept>

namespace dispatcharr
{

namespace
{
// See the non-finite/huge/negative #EXTINF clamp below for why this exists.
constexpr double kMaxSegmentDurationSec = 86400.0;
} // namespace

std::vector<M3u8SegmentEntry> ParseNewM3u8SegmentEntries(const std::string& playlistText, const std::string& baseDir,
                                                         size_t alreadyKnownCount)
{
  std::vector<M3u8SegmentEntry> pending;
  size_t segmentIndex = 0;
  double pendingDurationSec = 0.0;
  size_t pos = 0;
  while (pos <= playlistText.size())
  {
    size_t newlinePos = playlistText.find('\n', pos);
    std::string line =
        (newlinePos == std::string::npos) ? playlistText.substr(pos) : playlistText.substr(pos, newlinePos - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.compare(0, 8, "#EXTINF:") == 0)
    {
      std::string durStr = line.substr(8);
      size_t comma = durStr.find(',');
      if (comma != std::string::npos)
        durStr = durStr.substr(0, comma);
      try
      {
        pendingDurationSec = std::stod(durStr);
      }
      catch (const std::exception&)
      {
        pendingDurationSec = 0.0;
      }
      // A non-finite value (NaN/inf, see this function's own header
      // comment) isn't the only way this can escape into UB: std::stod
      // also accepts a finite-but-huge or negative value with no error,
      // and the caller's own static_cast<int64_t>(durationSec * 1000) is
      // undefined behavior for any value outside int64_t's range once
      // scaled -- the exact class of bug already fixed in this project's
      // Python-side _parse_edl (a finite value overflowing to inf only
      // after being scaled), just not previously mirrored here. Clamp to
      // a generous but sane per-segment ceiling (1 day) and floor at 0 (a
      // negative #EXTINF is a real M3U idiom -- e.g. "#EXTINF:-1," -- that
      // would otherwise make timeOffsetMs/totalDurationMs non-monotonic).
      if (!std::isfinite(pendingDurationSec) || pendingDurationSec < 0.0)
        pendingDurationSec = 0.0;
      else if (pendingDurationSec > kMaxSegmentDurationSec)
        pendingDurationSec = kMaxSegmentDurationSec;
    }
    else if (!line.empty() && line[0] != '#')
    {
      if (segmentIndex >= alreadyKnownCount)
      {
        std::string segUrl =
            (line.compare(0, 7, "http://") == 0 || line.compare(0, 8, "https://") == 0) ? line : baseDir + line;
        pending.push_back({std::move(segUrl), pendingDurationSec});
      }
      ++segmentIndex;
      pendingDurationSec = 0.0;
    }

    if (newlinePos == std::string::npos)
      break;
    pos = newlinePos + 1;
  }
  return pending;
}

size_t CountLeadingProbedSegments(const std::vector<int64_t>& probedSizes)
{
  size_t count = 0;
  while (count < probedSizes.size() && probedSizes[count] > 0)
    ++count;
  return count;
}

std::string RebaseRecordingSegmentUrl(const std::string& segmentUrl, const std::string& baseUrl,
                                      const std::string& requiredPathPrefix)
{
  size_t schemeEnd = std::string::npos;
  if (segmentUrl.compare(0, 7, "http://") == 0)
    schemeEnd = 7;
  else if (segmentUrl.compare(0, 8, "https://") == 0)
    schemeEnd = 8;
  if (schemeEnd == std::string::npos || baseUrl.empty())
    return segmentUrl;

  const size_t pathStart = segmentUrl.find('/', schemeEnd);
  if (pathStart == std::string::npos)
    return segmentUrl;
  if (segmentUrl.compare(pathStart, requiredPathPrefix.size(), requiredPathPrefix) != 0)
    return segmentUrl;

  std::string base = baseUrl;
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  return base + segmentUrl.substr(pathStart);
}

bool M3u8HasEndList(const std::string& playlistText)
{
  size_t pos = 0;
  while (pos <= playlistText.size())
  {
    const size_t newlinePos = playlistText.find('\n', pos);
    const size_t end = newlinePos == std::string::npos ? playlistText.size() : newlinePos;
    size_t lineEnd = end;
    // Trailing CR (CRLF files) and blanks: the tag is the whole line, but a space or
    // tab after it does not make it a different tag, and treating it as one would
    // hold a finished recording open for the whole no-tag grace period.
    while (lineEnd > pos &&
           (playlistText[lineEnd - 1] == '\r' || playlistText[lineEnd - 1] == ' ' || playlistText[lineEnd - 1] == '\t'))
      --lineEnd;
    if (playlistText.compare(pos, lineEnd - pos, "#EXT-X-ENDLIST") == 0 && lineEnd - pos == 14)
      return true;
    if (newlinePos == std::string::npos)
      break;
    pos = newlinePos + 1;
  }
  return false;
}

} // namespace dispatcharr
