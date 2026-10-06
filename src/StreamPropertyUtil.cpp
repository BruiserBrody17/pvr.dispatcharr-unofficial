#include "StreamPropertyUtil.h"

namespace dispatcharr
{

std::vector<StreamPropertyPair> BuildLiveChannelStreamProperties(int liveTimeshiftMode, const std::string& streamUrl)
{
  std::vector<StreamPropertyPair> properties;
  if (liveTimeshiftMode == kLiveTimeshiftServer)
  {
    properties.push_back({"isrealtimestream", "true"});
    return properties;
  }

  properties.push_back({"streamurl", streamUrl});
  properties.push_back({"isrealtimestream", "true"});
  properties.push_back({"mimetype", "video/mp2t"});

  if (liveTimeshiftMode == kLiveTimeshiftLocal)
  {
    properties.push_back({"inputstream", "inputstream.ffmpegdirect"});
    properties.push_back({"inputstream.ffmpegdirect.stream_mode", "timeshift"});
    properties.push_back({"inputstream.ffmpegdirect.is_realtime_stream", "true"});
  }

  return properties;
}

std::vector<StreamPropertyPair> BuildCatchupStreamProperties(const std::string& url, bool enableFfmpegdirectSeek)
{
  std::vector<StreamPropertyPair> properties;
  properties.push_back({"streamurl", url});
  properties.push_back({"isrealtimestream", "false"});
  properties.push_back({"mimetype", "video/mp2t"});

  if (enableFfmpegdirectSeek)
  {
    properties.push_back({"inputstream", "inputstream.ffmpegdirect"});
    properties.push_back({"inputstream.ffmpegdirect.is_realtime_stream", "false"});
  }

  return properties;
}

} // namespace dispatcharr
