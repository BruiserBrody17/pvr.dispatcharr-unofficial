// What the addon transferred to Kodi through the PVR_HANDLE result callbacks, kept for the drivers to inspect. Shared
// by pvr_stub.cpp (which fills it) and pvr_driver.cpp (which reads it), so both see the same layout.
#pragma once

#include <kodi/addon-instance/PVR.h>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct Collected
{
  std::mutex mu;
  // The strings of the structs below are only valid during the callback that delivered them (as in Kodi, which copies
  // them at once), so the stub copies each one here and points the stored struct at the copy. A deque keeps the
  // addresses stable as it grows; the caller holds mu.
  std::deque<std::string> strings;
  std::vector<PVR_CHANNEL> channels;
  std::vector<PVR_CHANNEL_GROUP> groups;
  std::vector<PVR_CHANNEL_GROUP_MEMBER> members;
  std::vector<EPG_TAG> epg;
  std::vector<std::string> epgTitles;
  std::vector<PVR_RECORDING> recordings;
  std::vector<PVR_TIMER> timers;

  const char* Own(const char* s)
  {
    if (!s)
      return nullptr;
    strings.emplace_back(s);
    return strings.back().c_str();
  }
};
