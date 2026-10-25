#pragma once

#include <string>
#include <vector>

namespace dispatcharr
{

// Mirrors PVRDispatcharr::kLiveTimeshiftOff/kLiveTimeshiftLocal/
// kLiveTimeshiftServer (PVRDispatcharr.h) -- duplicated as plain
// constants here (rather than included directly) so this stays
// independent of PVRDispatcharr.h's own Kodi-SDK-dependent declarations.
constexpr int kLiveTimeshiftOff = 0;
constexpr int kLiveTimeshiftLocal = 1;
constexpr int kLiveTimeshiftServer = 2;

// The live proxy URL for a channel, as Kodi is handed it in Off and Local mode. `output_format=mpegts` is sent because
// the proxy does not always answer MPEG-TS: `_resolve_output_format()` (apps/proxy/live_proxy/views.py) takes the URL's
// `output_format`, then the requesting user's Output Format Override, then Settings -> Stream Settings -> Default
// Output Format, and since Dispatcharr 0.33.0 that can be HLS, which redirects to a playlist (fMP4, available since an
// earlier release, sends MP4 fragments). The addon tells Kodi `video/mp2t` and reads the warm-up as a byte stream, so
// either breaks playback (confirmed live on 0.33.0 with Kodi 21 and 22: HLS ends at once, fMP4 never shows a picture).
// Dispatcharr's own preview buttons pin `mpegts` the same way.
//
// `|Connection=close` is a Kodi CCurlFile URL option, not part of the URL: see GetLiveStreamUrl()'s comment.
std::string BuildLiveStreamUrl(const std::string& baseUrl, const std::string& channelUuid);

// A single (name, value) stream property pair -- mirrors what
// kodi::addon::PVRStreamProperty holds, without depending on the Kodi
// SDK type itself, so this stays Kodi-SDK-independent and testable
// standalone; see ../tests/test_stream_property_util.cpp.
struct StreamPropertyPair
{
  std::string name;
  std::string value;
};

// The pure field-mapping core of PVRDispatcharr::GetChannelStreamProperties()
// -- decides which properties to return for a given live-timeshift mode
// and (Off/Local only) live stream URL. Mirrors Kodi's
// PVR_STREAM_PROPERTY_*/inputstream.ffmpegdirect string literals as raw
// strings rather than including Kodi's header directly, the same
// convention EpgTagUtil.cpp already uses for EPG_TAG_FLAG_* (confirmed
// against kodi/c-api/addon-instance/pvr/pvr_general.h and .../inputstream/
// stream_constants.h: "streamurl", "isrealtimestream", "mimetype",
// "inputstream").
//
// Server mode deliberately leaves "streamurl" unset (see
// GetChannelStreamProperties()'s own comment: Kodi uses STREAMURL
// directly via its generic CCurlFile when it's set, bypassing this
// addon's own stream callbacks entirely) so Kodi instead demuxes
// through this addon's own OpenLiveStream()/ReadLiveStream()/
// SeekLiveStream(). Off/Local both get a plain streamurl+mimetype;
// Local additionally gets inputstream.ffmpegdirect's own
// stream_mode=timeshift properties, since ffmpegdirect's generic HLS
// seek is confirmed broken for this addon's rolling server-side buffer
// (see docs/TIMESHIFT.md's seek investigation) but its own dedicated
// TimeshiftStream class works for a local, on-device buffer instead.
std::vector<StreamPropertyPair> BuildLiveChannelStreamProperties(int liveTimeshiftMode, const std::string& streamUrl);

// The pure field-mapping core of PVRDispatcharr::GetEPGTagStreamProperties()'s
// own catch-up branch (added 2026-09-26, a 24th-pass audit) -- pulled out
// specifically so it's unit-testable standalone, matching
// BuildLiveChannelStreamProperties()'s own shape above; see
// ../tests/test_stream_property_util.cpp.
//
// Always a plain "streamurl" + "isrealtimestream"="false" +
// "mimetype"="video/mp2t" -- catch-up genuinely isn't a realtime stream,
// unlike live. `enableFfmpegdirectSeek` (the `enable_catchup_ffmpegdirect_seek`
// setting, default off) additionally adds "inputstream"="inputstream.ffmpegdirect"
// plus its own "inputstream.ffmpegdirect.is_realtime_stream"="false" --
// this exact pair, and nothing else, is the *third* attempt at improving
// catch-up seek reliability; two earlier ones (setting
// "inputstream.ffmpegdirect.stream_mode", and separately forcing
// "inputstream.ffmpegdirect.open_mode"="ffmpeg") were tried and reverted
// after live testing showed each made seeking measurably worse, not
// better -- see GetEPGTagStreamProperties()'s own comment (PVRDispatcharr.cpp)
// for the full documented history, so a future change doesn't repeat one
// of those dead ends blind.
std::vector<StreamPropertyPair> BuildCatchupStreamProperties(const std::string& url, bool enableFfmpegdirectSeek);

} // namespace dispatcharr
