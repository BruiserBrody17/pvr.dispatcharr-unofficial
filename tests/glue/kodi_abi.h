// The parts of pvr_driver.cpp that depend on the shape of Kodi's PVR C ABI, in one place.
//
// This is the Kodi 22 ("Piers", PVR API 9.x) shape: const char* struct fields, backend strings, properties, EDL entries
// and timer types allocated by the addon's C++ wrapper and released through the Free* callbacks, and a stream id on
// every recorded-stream call. The Kodi 21 ("Omega", PVR API 8.x) shape differs in all of those (fixed char arrays,
// caller-provided buffers, one implicit recorded stream), so the Omega branch has its own copy of this header and the
// driver stays the same. Keep the driver free of anything that names those shapes directly.
#pragma once

#include <kodi/addon-instance/PVR.h>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace abi
{

// A string field of a PVR_* struct, as std::string (a null pointer reads as empty).
inline std::string str(const char* field)
{
  return field ? std::string(field) : std::string();
}

// Storage for strings a driver puts into the structs it hands the addon. A pointer field must outlive the call, and the
// addon must not keep it past the call, so nothing is ever freed; a run makes a few thousand of these at most.
inline const char* Intern(std::string value)
{
  static std::mutex mutex;
  static std::deque<std::string> pool;
  std::lock_guard<std::mutex> lock(mutex);
  pool.push_back(std::move(value));
  return pool.back().c_str();
}

// Assigns a string field of a PVR_* struct. printf-style so call sites keep their format strings.
inline void setf(const char*& field, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
inline void setf(const char*& field, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  const int n = vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  std::string value(n > 0 ? static_cast<size_t>(n) : 0, '\0');
  if (n > 0)
    vsnprintf(value.data(), value.size() + 1, fmt, ap);
  va_end(ap);
  field = Intern(std::move(value));
}

// The capability bits the driver reads.
struct Caps
{
  bool radio = false;
  bool timers = false;
  bool recordings = false;
  bool recordingsDelete = false;
  bool recordingsRename = false;
};

inline Caps ReadCaps(const AddonInstance_PVR* inst)
{
  PVR_ADDON_CAPABILITIES caps{};
  inst->toAddon->GetCapabilities(inst, &caps);
  Caps out;
  out.radio = caps.bSupportsRadio;
  out.timers = caps.bSupportsTimers;
  out.recordings = caps.bSupportsRecordings;
  out.recordingsDelete = caps.bSupportsRecordingsDelete;
  out.recordingsRename = caps.bSupportsRecordingsRename;
  inst->toAddon->FreeCapabilities(inst, &caps);
  return out;
}

namespace detail
{
using BackendString = PVR_ERROR (*)(const AddonInstance_PVR*, char**);

inline PVR_ERROR TakeString(const AddonInstance_PVR* inst, BackendString getter, std::string& out)
{
  char* raw = nullptr;
  const PVR_ERROR err = getter(inst, &raw);
  out = raw ? raw : "";
  if (raw)
    inst->toAddon->FreeString(inst, raw);
  return err;
}
} // namespace detail

inline std::string BackendName(const AddonInstance_PVR* inst)
{
  std::string out;
  detail::TakeString(inst, inst->toAddon->GetBackendName, out);
  return out;
}

inline std::string BackendVersion(const AddonInstance_PVR* inst)
{
  std::string out;
  detail::TakeString(inst, inst->toAddon->GetBackendVersion, out);
  return out;
}

inline std::string ConnectionString(const AddonInstance_PVR* inst)
{
  std::string out;
  detail::TakeString(inst, inst->toAddon->GetConnectionString, out);
  return out;
}

inline PVR_ERROR BackendHostname(const AddonInstance_PVR* inst, std::string& out)
{
  return detail::TakeString(inst, inst->toAddon->GetBackendHostname, out);
}

// How many timer types the addon lists.
inline int TimerTypeCount(const AddonInstance_PVR* inst)
{
  PVR_TIMER_TYPE** types = nullptr;
  unsigned n = 0;
  inst->toAddon->GetTimerTypes(inst, &types, &n);
  if (types)
    inst->toAddon->FreeTimerTypes(inst, types, n);
  return static_cast<int>(n);
}

// How many stream properties the addon hands back for a channel or a recording.
inline unsigned ChannelStreamPropertyCount(const AddonInstance_PVR* inst, const PVR_CHANNEL* channel)
{
  PVR_NAMED_VALUE** props = nullptr;
  unsigned n = 0;
  // PVR_SOURCE's plain-live enumerator is the unprefixed DEFAULT.
  inst->toAddon->GetChannelStreamProperties(inst, channel, DEFAULT, &props, &n);
  if (props)
    inst->toAddon->FreeProperties(inst, props, n);
  return n;
}

inline unsigned RecordingStreamPropertyCount(const AddonInstance_PVR* inst, const PVR_RECORDING* recording)
{
  PVR_NAMED_VALUE** props = nullptr;
  unsigned n = 0;
  inst->toAddon->GetRecordingStreamProperties(inst, recording, &props, &n);
  if (props)
    inst->toAddon->FreeProperties(inst, props, n);
  return n;
}

// The EDL entries the addon returns for a recording.
inline std::vector<PVR_EDL_ENTRY> RecordingEdl(const AddonInstance_PVR* inst, const PVR_RECORDING* recording)
{
  PVR_EDL_ENTRY** entries = nullptr;
  unsigned n = 0;
  inst->toAddon->GetRecordingEdl(inst, recording, &entries, &n);
  std::vector<PVR_EDL_ENTRY> out;
  if (entries)
  {
    for (unsigned i = 0; i < n; i++)
      out.push_back(*entries[i]);
    inst->toAddon->FreeEdlEntries(inst, entries, n);
  }
  return out;
}

// An open recorded stream: the id the addon returned on open, which Kodi 22 passes to every later call.
struct RecordedStream
{
  int64_t id = 0;
};

inline bool OpenRecorded(const AddonInstance_PVR* inst, const PVR_RECORDING* recording, RecordedStream& stream)
{
  return inst->toAddon->OpenRecordedStream(inst, recording, &stream.id);
}

inline void CloseRecorded(const AddonInstance_PVR* inst, RecordedStream& stream)
{
  inst->toAddon->CloseRecordedStream(inst, stream.id);
}

inline int ReadRecorded(const AddonInstance_PVR* inst, RecordedStream& stream, unsigned char* buffer, unsigned size)
{
  return inst->toAddon->ReadRecordedStream(inst, stream.id, buffer, size);
}

inline int64_t SeekRecorded(const AddonInstance_PVR* inst, RecordedStream& stream, int64_t position, int whence)
{
  return inst->toAddon->SeekRecordedStream(inst, stream.id, position, whence);
}

inline int64_t LengthRecorded(const AddonInstance_PVR* inst, RecordedStream& stream)
{
  return inst->toAddon->LengthRecordedStream(inst, stream.id);
}

// Without SetSupportsMultipleRecordedStreams() Kodi asks for a recording's times through the legacy call.
inline PVR_ERROR RecordedStreamTimes(const AddonInstance_PVR* inst, RecordedStream&, PVR_STREAM_TIMES& times)
{
  return inst->toAddon->GetStreamTimes(inst, &times);
}

} // namespace abi
