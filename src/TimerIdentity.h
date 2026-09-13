#pragma once

#include <string>

namespace dispatcharr
{

// Series rules have no numeric id at all in Dispatcharr's API (confirmed
// against a real rule: {mode, title, tvg_id, channel_id, title_mode,
// description, description_mode} -- nothing else), so PVRDispatcharr::
// GetTimers() can't use one for a Kodi PVR_TIMER's ClientIndex the way
// every other timer kind does. Hashes the (title, tvgId) pair instead --
// the same identity DispatcharrClient::DeleteSeriesRule() uses -- masked
// into the lower 30 bits so the high "series rule" flag bit (0x40000000,
// applied by the caller alongside this) is never disturbed.
//
// Deliberately does NOT return a value stable across process restarts or
// platforms/compilers: std::hash<std::string> is only guaranteed
// consistent within one running process, which is exactly what a
// transient, in-memory Kodi ClientIndex needs -- it's never persisted or
// compared across a restart. Pulled out here specifically so the masking
// behavior is unit-testable standalone; see
// ../tests/test_timer_identity.cpp.
inline unsigned int ComputeSeriesRuleClientIndex(const std::string& title, const std::string& tvgId)
{
  std::size_t h = std::hash<std::string>()(title + '\x1f' + tvgId);
  return (static_cast<unsigned int>(h) & 0x3FFFFFFFu) | 0x40000000u;
}

} // namespace dispatcharr
