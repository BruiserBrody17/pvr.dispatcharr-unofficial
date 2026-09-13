#include "M3u8SegmentParser.h"

#include <cmath>
#include <stdexcept>

namespace dispatcharr
{

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
      if (!std::isfinite(pendingDurationSec))
        pendingDurationSec = 0.0;
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

} // namespace dispatcharr
