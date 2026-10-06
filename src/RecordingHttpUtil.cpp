#include "RecordingHttpUtil.h"

#include "StringUtil.h"

namespace dispatcharr
{

bool IsInProgressHlsRedirect(const std::string& contentType, const std::string& resolvedUrl)
{
  return ToLower(contentType).find("mpegurl") != std::string::npos || resolvedUrl.find("/hls/") != std::string::npos;
}

} // namespace dispatcharr
