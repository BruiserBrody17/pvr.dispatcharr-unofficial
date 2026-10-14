// The parts of pvr_driver.cpp that depend on the shape of Kodi's PVR C ABI, in one place.
//
// This is the Kodi 21 ("Omega", PVR API 8.x) shape: fixed char arrays in the PVR_* structs, caller-provided buffers for
// backend strings, properties, EDL entries and timer types, and one implicit recorded stream per instance. The Kodi 22
// ("Piers", PVR API 9.x) shape differs in all of those (const char* struct fields, heap-allocated out-parameters
// released through the Free* callbacks, a stream id on every recorded-stream call), so the Piers branch replaces this
// header alone and the driver stays the same. Keep the driver free of anything that names those shapes directly.
#pragma once

#include <kodi/addon-instance/PVR.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace abi
{

// A string field of a PVR_* struct, as std::string (a null pointer, possible in the Piers shape, reads as empty).
inline std::string str(const char* field)
{
  return field ? std::string(field) : std::string();
}

// Assigns a string field of a PVR_* struct. printf-style so call sites keep their format strings.
template <size_t N> void setf(char (&field)[N], const char* fmt, ...) __attribute__((format(printf, 2, 3)));
template <size_t N> void setf(char (&field)[N], const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(field, N, fmt, ap);
  va_end(ap);
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
  return out;
}

inline std::string BackendName(const AddonInstance_PVR* inst)
{
  char buf[256] = {};
  inst->toAddon->GetBackendName(inst, buf, sizeof buf);
  return buf;
}

inline std::string BackendVersion(const AddonInstance_PVR* inst)
{
  char buf[256] = {};
  inst->toAddon->GetBackendVersion(inst, buf, sizeof buf);
  return buf;
}

inline std::string ConnectionString(const AddonInstance_PVR* inst)
{
  char buf[256] = {};
  inst->toAddon->GetConnectionString(inst, buf, sizeof buf);
  return buf;
}

inline PVR_ERROR BackendHostname(const AddonInstance_PVR* inst, std::string& out)
{
  char buf[256] = {};
  const PVR_ERROR err = inst->toAddon->GetBackendHostname(inst, buf, sizeof buf);
  out = buf;
  return err;
}

// How many timer types the addon lists.
inline int TimerTypeCount(const AddonInstance_PVR* inst)
{
  std::vector<PVR_TIMER_TYPE> types(PVR_ADDON_TIMERTYPE_ARRAY_SIZE);
  int n = (int)types.size();
  inst->toAddon->GetTimerTypes(inst, types.data(), &n);
  return n;
}

// How many stream properties the addon hands back for a channel or a recording. The buffer is small on purpose: a
// PVR_NAMED_VALUE is about 2 KB in this shape, and the addon returns a handful.
constexpr unsigned kMaxProperties = 40;

inline unsigned ChannelStreamPropertyCount(const AddonInstance_PVR* inst, const PVR_CHANNEL* channel)
{
  PVR_NAMED_VALUE props[kMaxProperties];
  unsigned n = kMaxProperties;
  inst->toAddon->GetChannelStreamProperties(inst, channel, props, &n);
  return n;
}

inline unsigned RecordingStreamPropertyCount(const AddonInstance_PVR* inst, const PVR_RECORDING* recording)
{
  PVR_NAMED_VALUE props[kMaxProperties];
  unsigned n = kMaxProperties;
  inst->toAddon->GetRecordingStreamProperties(inst, recording, props, &n);
  return n;
}

// The EDL entries the addon returns for a recording.
inline std::vector<PVR_EDL_ENTRY> RecordingEdl(const AddonInstance_PVR* inst, const PVR_RECORDING* recording)
{
  PVR_EDL_ENTRY entries[PVR_ADDON_EDL_LENGTH];
  int n = PVR_ADDON_EDL_LENGTH;
  inst->toAddon->GetRecordingEdl(inst, recording, entries, &n);
  return std::vector<PVR_EDL_ENTRY>(entries, entries + (n > 0 ? n : 0));
}

// An open recorded stream. Kodi 21 has one implicit stream per instance, so this carries nothing; Kodi 22 hands out an
// id on open and takes it on every later call.
struct RecordedStream
{
};

inline bool OpenRecorded(const AddonInstance_PVR* inst, const PVR_RECORDING* recording, RecordedStream&)
{
  return inst->toAddon->OpenRecordedStream(inst, recording);
}

inline void CloseRecorded(const AddonInstance_PVR* inst, RecordedStream&)
{
  inst->toAddon->CloseRecordedStream(inst);
}

inline int ReadRecorded(const AddonInstance_PVR* inst, RecordedStream&, unsigned char* buffer, unsigned size)
{
  return inst->toAddon->ReadRecordedStream(inst, buffer, size);
}

inline int64_t SeekRecorded(const AddonInstance_PVR* inst, RecordedStream&, int64_t position, int whence)
{
  return inst->toAddon->SeekRecordedStream(inst, position, whence);
}

inline int64_t LengthRecorded(const AddonInstance_PVR* inst, RecordedStream&)
{
  return inst->toAddon->LengthRecordedStream(inst);
}

inline PVR_ERROR RecordedStreamTimes(const AddonInstance_PVR* inst, RecordedStream&, PVR_STREAM_TIMES& times)
{
  return inst->toAddon->GetStreamTimes(inst, &times);
}

} // namespace abi
