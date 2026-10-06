#pragma once

// This targets the kodi-dev-kit C++ PVR API as it stands for Kodi 20
// (Nexus) through 22 (in development at the time of writing).

#include <kodi/addon-instance/PVR.h>

#include "ServerOffsetCrossCheck.h"
#include "ManagedRecurringRule.h"
#include "PaddingPush.h"
#include "RefreshEvents.h"
#include "RecordingEpgLinks.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DispatcharrClient.h"
#include "XmlTvParser.h"

class PVRDispatcharr : public kodi::addon::CInstancePVRClient
{
public:
  explicit PVRDispatcharr(const kodi::addon::IInstanceInfo& instance);
  ~PVRDispatcharr() override;

  // Called from CAddonDispatcharr::SetSetting() (addon.cpp) -- the
  // addon-base-level callback Kodi invokes once per changed setting when
  // the user edits addon settings via the GUI, without restarting Kodi.
  // Not a CInstancePVRClient override; there's no per-instance equivalent
  // wired into the PVR C++ API, only the addon-base one, so
  // CAddonDispatcharr forwards to whichever instance it created. Updates
  // the specific atomic member(s) named in settings.xml that this addon
  // can safely apply live; returns ADDON_STATUS_NEED_RESTART for anything
  // it can't (the Dispatcharr connection settings, which are baked into
  // DispatcharrClient's Config at construction, and
  // enable_realtime_updates, which would need to dynamically start/stop a
  // background thread -- deliberately left out of scope here).
  ADDON_STATUS OnAddonSettingChanged(const std::string& settingName, const kodi::addon::CSettingValue& settingValue);

  // --- General ---
  PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;
  PVR_ERROR GetBackendName(std::string& name) override;
  PVR_ERROR GetBackendVersion(std::string& version) override;
  PVR_ERROR GetConnectionString(std::string& connection) override;

  // Only override worth having of the two system-power-state hooks Kodi's
  // PVR API offers (OnSystemSleep()/OnSystemWake()) -- this addon's actual
  // live/recording read paths are stateless HTTP polls (a fresh request
  // each time, byte position kept in this addon's own memory), which
  // recover from a suspend gap on their own with no explicit handling
  // needed, unlike a persistent subscription-based protocol. The one place
  // sleep silently breaks something is the real-time-updates WebSocket
  // (see m_wakeRealtimeUpdateThread's own comment) -- nudging it here
  // rather than leaving OnSystemSleep() as a no-op override that does
  // nothing useful.
  PVR_ERROR OnSystemWake() override;

  // --- Channel groups ---
  PVR_ERROR GetChannelGroupsAmount(int& amount) override;
  PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
  PVR_ERROR GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                   kodi::addon::PVRChannelGroupMembersResultSet& results) override;

  // --- Channels ---
  PVR_ERROR GetChannelsAmount(int& amount) override;
  PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;
  PVR_ERROR GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                       std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  // Server-side timeshift's actual playback path: GetChannelStreamProperties()
  // leaves STREAMURL unset for that mode specifically so Kodi calls these
  // instead of routing through inputstream.ffmpegdirect -- see its own
  // comment and docs/TIMESHIFT.md. Mirrors OpenRecordedStream()/
  // ReadRecordedStream()/SeekRecordedStream()/LengthRecordedStream() below
  // almost exactly, just against DispatcharrClient's live-timeshift-stream
  // methods instead of its recording-stream ones.
  bool OpenLiveStream(const kodi::addon::PVRChannel& channel) override;
  void CloseLiveStream() override;
  int ReadLiveStream(unsigned char* buffer, unsigned int size) override;
  int64_t SeekLiveStream(int64_t position, int whence) override;
  int64_t LengthLiveStream() override;
  bool CanPauseStream() override;
  bool CanSeekStream() override;
  bool IsRealTimeStream() override;
  PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override;
  // Kodi's ffmpeg demuxer defaults its AVIO read buffer to a hardcoded 4096
  // bytes (CDVDDemuxFFmpeg::CreateDemuxer, DVDDemuxFFmpeg.cpp) unless the
  // PVR client overrides this -- and every read against our live-timeshift
  // or recording streams costs one full HTTP round trip to the timeshift
  // plugin's file server, so 4KB reads meant needing hundreds of requests
  // per second to sustain a high-bitrate channel, causing periodic
  // stall/rebuffer cycles on higher-bitrate channels. Applies to both live
  // (server-side timeshift mode) and recording playback, both of which go
  // through this same CInputStreamPVRBase-backed path.
  PVR_ERROR GetStreamReadChunkSize(int& chunksize) override;

  // --- EPG ---
  PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end,
                             kodi::addon::PVREPGTagsResultSet& results) override;
  // "Play from guide" for a past/currently-airing programme, backed by
  // Dispatcharr's catch-up/archive feature (see docs/API_NOTES.md) --
  // per-channel and dependent on the upstream provider's own archive, not
  // a continuous rolling live-timeshift buffer for every channel.
  PVR_ERROR IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable) override;
  PVR_ERROR GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag,
                                      std::vector<kodi::addon::PVRStreamProperty>& properties) override;

  // --- Recordings ---
  PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override;
  PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override;
  PVR_ERROR GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording,
                                         std::vector<kodi::addon::PVRStreamProperty>& properties) override;
  PVR_ERROR DeleteRecording(const kodi::addon::PVRRecording& recording) override;
  PVR_ERROR RenameRecording(const kodi::addon::PVRRecording& recording) override;
  PVR_ERROR GetRecordingEdl(const kodi::addon::PVRRecording& recording,
                            std::vector<kodi::addon::PVREDLEntry>& edl) override;
  // Kodi always demuxes pvr://recordings/... via CInputStreamPVRRecording,
  // which serves the generic FFmpeg demuxer through these -- confirmed
  // against Kodi's own source that it never resolves
  // PVR_STREAM_PROPERTY_STREAMURL from GetRecordingStreamProperties() for
  // this path the way live channels and catch-up work. See
  // DispatcharrClient::OpenRecordingStream().
  bool OpenRecordedStream(const kodi::addon::PVRRecording& recording) override;
  void CloseRecordedStream() override;
  int ReadRecordedStream(unsigned char* buffer, unsigned int size) override;
  int64_t SeekRecordedStream(int64_t position, int whence) override;
  int64_t LengthRecordedStream() override;

  // --- Timers ---
  PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override;
  PVR_ERROR GetTimersAmount(int& amount) override;
  PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override;
  PVR_ERROR AddTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR UpdateTimer(const kodi::addon::PVRTimer& timer) override;
  PVR_ERROR DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete) override;

private:
  static constexpr int kTimerTypeOneTime = 1;
  static constexpr int kTimerTypeSeries = 2;
  static constexpr int kTimerTypeOneTimeEpgBased = 3;
  static constexpr int kTimerTypeRecurring = 4;
  static constexpr int kLiveTimeshiftOff = 0;
  // Removed once server-side timeshift proved stable (one less choice, one
  // less separate-addon dependency for live playback), then reintroduced:
  // server-side turned out to have a hard requirement -- a real Dispatcharr
  // admin account -- that's a blanket restriction in Dispatcharr's own
  // plugin run/ API (see docs/TIMESHIFT.md's "permission requirement"
  // section), not something this addon or its companion plugin can loosen.
  // Local fills that gap for anyone who doesn't want to grant admin access:
  // real pause/rewind, entirely client-side via inputstream.ffmpegdirect's
  // own on-device buffer, no Dispatcharr-side cooperation at all. Value 1
  // was deliberately left unreused during the removal rather than
  // renumbering kLiveTimeshiftServer down to 1, specifically so it could be
  // safely reused here without an existing install's persisted `2` ever
  // meaning something different.
  static constexpr int kLiveTimeshiftLocal = 1;
  static constexpr int kLiveTimeshiftServer = 2;
  // ClientIndex namespace bits: series rules already use 0x40000000 (see
  // GetTimers()); recurring rules use a separate bit so a real Dispatcharr
  // id (unlike series rules, which have none and must be hashed) can be
  // used directly without colliding with either namespace.
  static constexpr unsigned int kRecurringRuleIndexFlag = 0x20000000u;
  // How many days ahead a recurring rule's Dispatcharr-side end_date is
  // kept, both at creation and via the periodic renewal below. Kodi's own
  // repeating-timer UI has no "last day" field to expose per-timer
  // (confirmed against kodi-dev-kit's PVR_TIMER_TYPE_SUPPORTS_* flags --
  // only SUPPORTS_FIRST_DAY exists, no equivalent for an end), but
  // Dispatcharr's serializer requires a real end_date on every create --
  // see CreateRecurringRule(). This used to be a flat 3-year end_date on
  // the theory that Dispatcharr only lazily materializes ~14 days ahead
  // regardless of how far out end_date sits, making a far-future value
  // "free". Confirmed live that theory was wrong: Dispatcharr eagerly
  // materializes *every* occurrence between start_date and end_date
  // synchronously, right when the rule is created (or its end_date is
  // changed) -- a single weekly rule with a 3-year end_date produced 157
  // real Recording rows immediately, which would show as 157 child timers
  // in Kodi's own timer list. A much shorter rolling window, kept topped
  // up by RenewRecurringRules() below, keeps that cost bounded while still
  // behaving like a "create once, forget about it" repeating timer from
  // the user's side -- deleting the timer in Kodi still ends it for good
  // at any point; there's no separate "stop repeating" concept to expose
  // beyond that.
  static constexpr int kRecurringRuleWindowDays = 30;
  // RenewRecurringRules() extends a rule's end_date once less than half
  // this window remains, rather than waiting until it's about to actually
  // run out -- keeps the periodic check (which runs on the existing
  // recording-refresh cadence, not a dedicated timer) cheap: most cycles
  // see a rule comfortably inside its window and do nothing at all.
  // Skips a rule with an occurrence currently recording or starting within
  // this many seconds, even though live testing confirmed Dispatcharr's
  // own regeneration on this kind of update already leaves an in-progress
  // or completed occurrence alone (see ExtendRecurringRuleEndDate()'s own
  // comment and docs/RECURRING_RULES.md) -- defense in depth rather than
  // relying solely on that server-side scoping.
  static constexpr int kRecurringRuleRenewalSafetyMarginSeconds = 3600;
  // ResolveRecordingBroadcastId()'s own tolerance when matching a
  // recording's programStartTime against this addon's own cached XMLTV
  // guide entries -- generous enough to absorb any second/minute-level
  // drift between Dispatcharr's own internal EPG database (where
  // programStartTime comes from) and this addon's independently-parsed
  // XMLTV guide of the same underlying provider data, without being so
  // wide it could ever span into a neighboring programme.
  static constexpr int kEpgStartTimeMatchToleranceSeconds = 120;

  dispatcharr::Config LoadConfigFromSettings() const;

  // Returns the offset (minutes) to actually use for recurring-rule
  // timezone conversion right now. When recurring_rule_timezone is set to
  // one of the zones DispatcharrClient::ComputeKnownZoneOffsetMinutes()
  // recognizes, this is computed fresh on every call (not cached), so it
  // can never go stale across a DST transition the way a value computed
  // once at startup would -- the whole reason this exists instead of just
  // reading m_recurringRuleUtcOffsetMinutes directly. Falls back to that
  // plain manual setting when the zone is "manual" (the default) or
  // unrecognized. Deliberately not cached in a member: both call sites
  // (GetTimers(), AddTimer()/UpdateTimer()'s recurring-rule paths) are
  // synchronous PVR callbacks on Kodi's own thread, not a background loop,
  // so there's no thread-safety reason to cache this the way
  // m_recordingRefreshMinutes needs to for its own polling thread.
  //
  // `at` is the date DST should be resolved against -- defaults to "now"
  // only for a caller with no more specific date available. Both real
  // call sites pass one explicitly: GetTimers()'s own display direction
  // passes the rule's own (potentially much older) startDate, and
  // AddTimer()/UpdateTimer()'s create/edit direction (via
  // ComputeRecurringRuleFields()'s own wrapper overload) passes
  // timer.GetStartTime().
  //
  // Both must resolve DST at a date matching what the *other* direction
  // used, not independently at whatever's individually most convenient --
  // a real, confirmed bug found via a project-wide review, in two parts:
  // (1) resolving DST at "now" while *displaying* a time for a rule whose
  // startDate is on the other side of a DST transition renders the wrong
  // wall-clock hour in Kodi's Timers list for roughly half the year
  // (display-only, harmless to the actual schedule) -- fixed by
  // GetTimers() passing startDate explicitly. (2) UpdateTimer() receives
  // back exactly the startTime GetTimers() just displayed (unchanged, on
  // an edit that doesn't touch the time fields at all -- e.g. toggling
  // enabled/disabled), so if it re-resolved DST at "now" instead of
  // matching that same reference date, the two different offsets applied
  // on each leg of the round trip wouldn't cancel out, silently shifting
  // the rule's actual recording time by the DST delta on every such edit
  // -- a regression introduced by the first fix and caught within the
  // same review pass, not shipped separately. Fixed by
  // ComputeRecurringRuleFields()'s wrapper passing timer.GetStartTime()
  // instead of relying on this function's own "now" default -- correct
  // for UpdateTimer() (matches what GetTimers() used) and at least as
  // correct as "now" for AddTimer() (a genuinely new rule's own start
  // time is normally today or the near future anyway).
  int EffectiveRecurringRuleUtcOffsetMinutes(time_t at = time(nullptr)) const;
  // Compares the zone table's offset with what Dispatcharr actually applied to the recurring-rule occurrences it has
  // materialized (ServerOffsetCrossCheck.h), and while they disagree makes EffectiveRecurringRuleUtcOffsetMinutes()
  // answer with the server's offset (two evaluations in a row must agree before it does): a server whose tz data
  // differs from the table's would otherwise have every rule made or edited from Kodi record an hour off. Called from
  // GetTimers(), which already holds both lists.
  void UpdateServerOffsetCrossCheck(const std::vector<dispatcharr::Recording>& recordings,
                                    const std::vector<dispatcharr::RecurringRule>& recurringRules);
  // Syncs recurring_rule_timezone/dispatcharr_timezone_info from
  // Dispatcharr's own configured system timezone -- extracted from the
  // constructor (added 2026-09-27, a 47th-pass audit) so
  // StartChannelEpgRefreshThread()'s own background loop can retry it
  // once, on its own next cycle, if the constructor's own one-shot
  // attempt failed (see m_timezoneSynced's own comment for the real,
  // confirmed startup-timing gap this closes). Returns whether the sync
  // itself succeeded (a real answer was read from Dispatcharr), not
  // which zone it resolved to.
  //
  // zoneChangedOut (added 2026-09-27, a 48th-pass audit, fixing a real,
  // confirmed scheduling bug found via a project-wide review, not itself
  // independently reproduced): true when this call actually changed
  // `recurring_rule_timezone` from whatever it was before. Before this
  // pass's own fix used it, a mid-session zone change (now reachable at
  // all only because the retry this same 47th-pass fix added can now
  // succeed *after* Kodi has already loaded some timers, not just before
  // any exist yet) silently corrupted every recurring rule's own
  // scheduled time on its next edit: `GetTimers()`'s own
  // `ComputeRecurringRuleDisplayTimes()` (`RecurringRuleUtil.cpp`)
  // subtracts the offset in effect *at display time* to build the time
  // Kodi caches and shows; `UpdateTimer()`'s own `ComputeRecurringRuleFields()`
  // adds back whatever offset is in effect *at edit time* to convert
  // that same cached, possibly-stale-offset time back to Dispatcharr's
  // own representation. If the offset changed in between -- even from an
  // edit as small as a plain enable/disable toggle, which still resends
  // `start_time`/`end_time`/`start_date` (`BuildRecurringRuleUpdateBody()`,
  // `TimerRequestBuilder.cpp`) -- the round trip shifts the rule by the
  // full difference between the old and new offset, not by nothing.
  // `StartChannelEpgRefreshThread()`'s own retry uses this to trigger a
  // timer refresh whenever the zone genuinely changes, so Kodi's own
  // cached display times catch up to the new offset before any edit can
  // read a stale one back. The constructor's own first call ignores
  // this -- nothing has been loaded into Kodi yet for a trigger to
  // matter at that point, and Trigger*() calls are unverified as safe
  // from inside CreateInstance() itself, before Kodi has ever registered
  // this instance.
  bool SyncTimezoneFromDispatcharr(bool* zoneChangedOut = nullptr);
  // Whether SyncTimezoneFromDispatcharr() has ever succeeded this
  // session -- added 2026-09-27, a 47th-pass audit, fixing a real,
  // confirmed gap found via a project-wide review, not itself
  // independently reproduced: the timezone sync used to run only once,
  // at construction, with no retry on failure -- the same startup-timing
  // class of gap already fixed elsewhere (channels/EPG, the API key). If
  // Dispatcharr isn't reachable yet at Kodi startup, recurring_rule_timezone
  // stayed at its "manual" default with a 0 offset for the rest of the
  // session, silently scheduling every recurring rule created or edited
  // at the wrong real-world time. StartChannelEpgRefreshThread()'s own
  // background loop now retries the sync once per cycle for as long as
  // this stays false.
  bool m_timezoneSynced = false;
  // Cached at construction from DispatcharrClient::IsCatchupEnabledGlobally()/
  // IsCatchupEnabledForCurrentUser() -- see dispatcharr::ShouldOfferCatchup()'s
  // own comment (EpgTagUtil.h) for the real, live-confirmed bug this exists
  // to fix (Kodi kept offering catch-up playback that Dispatcharr's own
  // server-side gate would 403 every time). Both default `true` (fail
  // open, same reasoning as the dispatcharr_is_admin sync just above --
  // Dispatcharr's own 403 is still the authoritative enforcement either
  // way) so a fetch failure at startup just reproduces today's
  // pre-this-fix behavior rather than spuriously hiding catch-up
  // entirely. Not refreshed again after construction -- unlike
  // channels/EPG/recordings, neither of these has its own periodic
  // background refresh, so a change made in Dispatcharr's own UI mid-
  // session (the global flag, or another admin editing this account)
  // won't be picked up until the next restart. Acceptable for the same
  // reason the admin-check sync above already treats itself as startup-
  // only: this is a UI-affordance/early-failure improvement over
  // Dispatcharr's own always-authoritative server-side check, not a
  // security boundary that needs to react instantly.
  // Atomic because SyncCatchupFlagsFromDispatcharr() can now write them from
  // the background thread, after Kodi's own threads have started reading.
  std::atomic<bool> m_catchupEnabledGlobally{true};
  std::atomic<bool> m_catchupEnabledForCurrentUser{true};
  // Whether each of the startup-time syncs below has answered once; until it
  // has, RetryDeferredServerSyncs() tries again every background cycle. See
  // each Sync*() method's own comment. (A later change made on the server is
  // still only picked up at the next restart -- these are retry-until-first-
  // success, not periodic.)
  std::atomic<bool> m_dvrPaddingSynced{false};
  std::atomic<bool> m_adminStatusChecked{false};
  std::atomic<bool> m_catchupFlagsChecked{false};
  // Whether the account may manage the DVR (create/delete/rename/stop). Fails
  // open until the account's access level has been read, and is only consulted by
  // GetCapabilities(), which Kodi reads once per instance -- see
  // SyncDvrAccessFromDispatcharr().
  std::atomic<bool> m_dvrManageAllowed{true};
  std::atomic<bool> m_dvrAccessChecked{false};
  // Set by GetCapabilities(): from then on a changed access level can no longer
  // reach what Kodi offers.
  std::atomic<bool> m_capabilitiesRead{false};
  // A DVR padding edit that couldn't be pushed to Dispatcharr yet, per side.
  // While either is set, RetryDeferredServerSyncs() resends it instead of
  // syncing the padding FROM Dispatcharr, which would revert the edit. The
  // bookkeeping rules live in PaddingPush.h (tested); guarded by
  // m_paddingPushMutex.
  std::mutex m_paddingPushMutex;
  dispatcharr::PendingPaddingPush m_paddingPush;
  // After this many failed background retries the pending edit is dropped and
  // the user told -- a non-admin account's push is refused every time.
  static constexpr int kMaxPaddingPushRetries = 3;
  bool SyncDvrPaddingFromDispatcharr();
  bool SyncAdminStatusFromDispatcharr();
  bool SyncCatchupFlagsFromDispatcharr();
  bool SyncDvrAccessFromDispatcharr();
  void RetryDeferredServerSyncs();
  // Snapshot of LoadConfigFromSettings()'s result, taken once at
  // construction and updated by OnAddonSettingChanged() itself below for
  // host/port/username/password/verify_ssl/timeout -- exists purely so
  // that method can tell a *genuine* change to a connection setting apart
  // from a spurious re-notification of the same value. That second case is
  // real, not hypothetical: Kodi has a documented quirk where a settings-
  // dialog save's terminal SetSetting() call can arrive mislabeled with the
  // name of the *last* setting defined in settings.xml (api_key, here)
  // even when nothing about that setting actually changed -- confirmed
  // live against a real CoreELEC install: saving `debug_logging` alone,
  // with nothing else touched, reliably produced a spurious "api_key
  // changed" notification and restarted the PVR client instance every
  // time, defeating live-apply for every setting, not just the connection
  // ones. Comparing against this snapshot turns that spurious case into a
  // no-op instead of an unwanted restart.
  //
  // `apiKey` specifically has a *second* writer: OpenRecordedStream()/
  // ReadRecordedStream()'s own self-heal persistence (see their own
  // comments) also update it, guarded by m_lastAppliedApiKeyMutex below --
  // confirmed live as a real bug otherwise: a self-generated key
  // regeneration mid-open (RefreshInProgressRecordingManifest()'s
  // proactive self-heal check) persists the new key via SetSettingString()
  // so a *future* restart doesn't lose it, but that persistence call is
  // itself what Kodi's SetSetting() delivers back here as a "genuine"
  // change (correctly, by the letter of this comparison, since the string
  // value did change) -- restarting the instance and tearing down the
  // in-progress-recording stream that had just successfully opened,
  // despite the new key already being live in DispatcharrClient's own
  // m_config.apiKey the moment ObtainApiKey() returned it, with no
  // restart actually needed. Updating this snapshot at the same two
  // self-heal sites, before their own SetSettingString() call, makes the
  // ensuing notification correctly see no *genuine* change and return
  // ADDON_STATUS_OK instead.
  dispatcharr::Config m_lastAppliedConfig;
  // Guards m_lastAppliedConfig.apiKey specifically, the one field with two
  // writers on potentially different threads (Kodi's own SetSetting()
  // dispatch thread via OnAddonSettingChanged(), and whichever thread calls
  // OpenRecordedStream()/ReadRecordedStream()) -- see m_lastAppliedConfig's
  // own comment. Every other field only has the one writer -- but see
  // m_apiKeyOwnerHost/m_apiKeyOwnerUsername just below for why
  // PersistApiKeyIfChanged() deliberately does NOT read
  // m_lastAppliedConfig.host/.username despite that, even though those two
  // specifically also have no lock of their own.
  std::mutex m_lastAppliedApiKeyMutex;
  // Set when PersistApiKeyIfChanged()'s SetSettingString("api_key") did not land (Kodi swallows an addon-initiated
  // write while this addon's own settings dialog is open): what the read-back found Kodi still storing. Empty
  // when no write is outstanding. Guarded by m_lastAppliedApiKeyMutex. See dispatcharr::ClassifyApiKeyDelivery().
  std::string m_apiKeyStoredAfterDroppedWrite;
  // The host/username m_client was actually constructed with -- a const
  // snapshot taken once at construction, deliberately NOT the same as
  // m_lastAppliedConfig.host/.username above (added 2026-09-27, a 46th-pass
  // audit, fixing two real, confirmed bugs in the 45th-pass fix that
  // introduced api_key_host/api_key_username, found via a project-wide
  // review, confirmed against Kodi's own real current source, not itself
  // independently reproduced): (1) a genuine data race --
  // m_lastAppliedConfig.host/.username are written with no lock by
  // OnAddonSettingChanged() (the settings/GUI thread) and were being read
  // with no lock by PersistApiKeyIfChanged() (any playback/read thread);
  // (2) even ignoring the race, m_lastAppliedConfig.host/.username reflect
  // the *latest delivered* setting, not necessarily what m_client is
  // actually still using -- CAddonStatusHandler::Process() (AddonStatusHandler.cpp)
  // shows a *blocking* OK dialog before actually restarting the addon on
  // ADDON_STATUS_NEED_RESTART, and this (old, not-yet-destroyed) instance
  // keeps running (and can still self-heal/regenerate the key mid-playback)
  // for however long the user takes to dismiss it. Stamping the *new*,
  // not-yet-applied host/username against a key regeneration that actually
  // happened against the *old* connection made the next instance's own
  // owner-check wrongly treat a foreign-account key as already matching,
  // keeping it permanently. `const`, so safe to read from any thread with
  // no synchronization at all -- never written after construction, unlike
  // m_lastAppliedConfig's own fields.
  //
  // Despite its name, m_apiKeyOwnerHost holds the *full* server identity
  // (scheme://host:port, ComputeApiKeyOwnerServer(), PVRDispatcharr.cpp's
  // anonymous namespace), not just the bare host (fixed 2026-09-27, a
  // 48th-pass audit, fixing a real, confirmed gap found via a project-wide
  // review, not itself independently reproduced): comparing only host and
  // username let switching to a *different* Dispatcharr instance
  // reachable on the same host but a different port or scheme (two
  // containers on 127.0.0.1 at different ports, a staging instance) with
  // the same username keep treating the old instance's key as already
  // belonging to the new one -- worse than the host/username case this
  // whole mechanism already guards, since an API key never expires on its
  // own, so OpenRecordedStream() skipped regeneration entirely and
  // silently sent the *previous* server's key to the *new* one until it
  // self-healed via a 401. Kept the setting id `api_key_host` unchanged
  // (only its stored *value* changed) to avoid an unnecessary settings.xml
  // churn for what's still, semantically, the same "which server/account
  // owns this key" identity.
  const std::string m_apiKeyOwnerHost;
  const std::string m_apiKeyOwnerUsername;
  // Whether the currently-stored API key is known to belong to this
  // instance's own account (added 2026-09-27, a 46th-pass audit, fixing a
  // real, confirmed gap in the 45th-pass account-mismatch fix, found via
  // a project-wide review, not itself independently reproduced): the
  // constructor's own api_key_host/api_key_username comparison (moved to
  // run unconditionally as of a 47th-pass fix -- see its own comment)
  // only actually resolves a detected mismatch if ObtainApiKey() also
  // succeeds -- if the initial login fails (the same startup-timing
  // class of gap channels/EPG already had) or that regeneration attempt
  // itself fails, this stays false and OpenRecordedStream() retries the
  // check (not just the plain-empty-key case) on every open until it
  // actually succeeds, since keyBefore.empty() alone can't detect a
  // *non-empty*, wrong-account key. Once true, stays true for the rest
  // of this instance's life -- never re-checked needlessly on every open
  // once resolved. Same plain-bool convention as the rest of this
  // single-active-stream-at-a-time code area (Kodi never opens more than
  // one recording stream concurrently per instance), no extra locking.
  bool m_apiKeyOwnershipVerified = false;
  // Same spurious-renotification guard as m_lastAppliedConfig above,
  // applied to the DVR padding settings -- a real, confirmed bug fixed
  // by this pair: OnAddonSettingChanged()'s own recording_pre/post_offset_minutes
  // branch used to push both values to Dispatcharr on *every* notification
  // with no "did this actually change" guard at all, unlike every other
  // branch in that function. Since Kodi's own dialog-save re-notification
  // quirk (see m_lastAppliedConfig's own comment) delivers this branch a
  // same-named, same-value notification whenever *any* setting is saved,
  // that meant every single settings-dialog save -- even saving an
  // unrelated setting like debug_logging -- spawned two detached network
  // round trips regardless of whether the padding actually changed, and
  // could silently revert a padding change made elsewhere (Dispatcharr's
  // own web UI, a different Kodi install sharing the account) back to
  // Kodi's own possibly-stale cached value on the very next unrelated
  // save. Seeded by the constructor's own sync-from-Dispatcharr, the same
  // way m_lastAppliedConfig is seeded from Kodi's settings at construction.
  // Atomic: written by the background thread too (SyncDvrPaddingFromDispatcharr()).
  std::atomic<int> m_lastAppliedPreOffsetMinutes{0};
  std::atomic<int> m_lastAppliedPostOffsetMinutes{0};
  // Tracks the detached, self-contained worker threads OnAddonSettingChanged()'s
  // DVR-padding push and AddTimer()'s delayed recording-refresh each spawn
  // (see each call site's own comment) -- both capture `this`/`m_client`,
  // and neither has a natural join point of its own the way the three named
  // worker threads below do (this addon has no long-lived "settings push" or
  // "delayed refresh" thread to fold them into). Real use-after-free risk
  // without this, confirmed by code reading rather than live reproduction:
  // if this instance is destroyed (e.g. an ADDON_STATUS_NEED_RESTART from an
  // unrelated setting change tears it down) while either thread is still
  // running, its captured `this` becomes dangling the instant the destructor
  // returns -- these used to just .detach() with no join at all.
  // TrackDetachedThread() records each one so the destructor can join every
  // one of them before returning, rather than outliving the instance they
  // reference.
  //
  // Takes the work itself (not an already-constructed std::thread) so it
  // can wrap it to flag completion via `done` below -- a real, confirmed
  // leak found via code reading in an earlier version of this fix: with
  // no way to tell a finished thread apart from a still-running one
  // short of join()ing it, entries only ever accumulated here between
  // pushes, each holding its stack/TCB mapped until this instance was
  // eventually destroyed. TrackDetachedThread() now opportunistically
  // joins and drops every already-finished entry first, so the list
  // only ever grows across genuinely-concurrent in-flight work, not
  // across this instance's entire lifetime.
  // When no thread can be created the task is dropped, or run on the calling thread when
  // `runInlineIfThreadFails` (for work that must not be lost and does not sleep).
  void TrackDetachedThread(std::function<void()> work, bool runInlineIfThreadFails = false);
  struct DetachedThreadEntry
  {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  // Set first thing in the destructor so a tracked thread that is only waiting (AddTimer's delayed
  // refresh sleeps five seconds) ends at once rather than holding the join -- see
  // SleepUnlessShuttingDown(). Found by the 2026-10-04 second hardening sweep: a Kodi exit within five
  // seconds of adding a timer waited out the rest of that sleep, and then refreshed an instance being
  // torn down.
  std::mutex m_shutdownMutex;
  std::condition_variable m_shutdownCv;
  std::atomic<bool> m_shuttingDown{false};
  // Waits up to `duration`; returns false when the instance began shutting down first.
  bool SleepUnlessShuttingDown(std::chrono::seconds duration);
  std::mutex m_detachedThreadsMutex;
  std::vector<DetachedThreadEntry> m_detachedThreads;
  // Returns true if this call actually performed a fetch (cache was stale
  // or never loaded), false if it was a no-op (still fresh). The
  // background thread below uses this to know when to call
  // TriggerEpgUpdate() -- Kodi's own calling threads (GetChannels(),
  // GetEPGForChannel(), ...) ignore it, since they're already inside the
  // callback that answers Kodi's question either way. (Corrected
  // 2026-09-26, a 26th-pass audit: EnsureChannelsLoaded() itself now
  // fires TriggerChannelUpdate()/TriggerChannelGroupsUpdate() on every
  // successful commit regardless of caller -- see its own comment -- so
  // this return value is no longer needed for those two specifically,
  // only for TriggerEpgUpdate() still.)
  //
  // `forceStale` (EnsureChannelsLoaded() only, default false): bypasses
  // the channels-side staleness check (not the failure backoff) -- see
  // dispatcharr::ShouldFetchChannels()'s own comment (Staleness.h) for
  // why EnsureEpgLoaded() needs this immediately before its own XMLTV
  // fetch.
  bool EnsureChannelsLoaded(bool forceStale = false);
  bool EnsureEpgLoaded();
  // Same staleness-cache shape as the pair above, but a much shorter TTL
  // (kRecordingsAndTimersCacheTtlSeconds, not channel_refresh_hours-scale)
  // -- recordings/timers can change the instant the user acts, unlike
  // channels/EPG. Exists so GetRecordingsAmount()+GetRecordings() (and
  // GetTimersAmount()+GetTimers()) don't each independently re-fetch:
  // Kodi calls the Amount() half and the List() half back-to-back on
  // every refresh, and this TTL is enough to collapse that pair into one
  // real fetch without meaningfully risking staleness for anything else.
  // The addon's own writes (AddTimer()/UpdateTimer()/DeleteTimer()/...)
  // don't wait out this TTL at all -- see
  // InvalidateAndTriggerRecordingUpdate()/InvalidateAndTriggerTimerUpdate().
  bool EnsureRecordingsLoaded();
  bool EnsureTimerRulesLoaded();
  const dispatcharr::Channel* FindChannelByUid(int uid) const;
  // Shared by GetTimers() (SetEPGUid) and GetRecordings() (SetEPGEventId)
  // -- the broadcast id (see dispatcharr::ComputeBroadcastId(), EpgTagUtil.h)
  // of the EPG entry a recording was actually recorded from, or
  // EPG_TAG_INVALID_UID (0) when none can be found. Added 2026-09-26 (a
  // 34th-pass audit, fixing a real, confirmed gap found via a
  // project-wide review, confirmed against both Kodi's own real current
  // SDK source and Dispatcharr's own real current upstream source, not
  // itself independently reproduced) -- extracted once both call sites
  // needed the exact same channel/EPG-cache lookup and
  // exact-start-time-then-overlap-ratio matching strategy; see
  // dispatcharr::FindEpgEntryIndexByStartTime()/
  // FindEpgEntryIndexCoveringRecording()'s own comments (EpgProgramMatch.h)
  // for why both are tried, in that order. Takes m_dataMutex itself --
  // callers must not already hold it.
  unsigned int ResolveRecordingBroadcastId(const dispatcharr::Recording& rec);
  // Which programme each recording was matched to, kept across guide refreshes and
  // restarts so a finished recording's guide link outlives the EPG cache entry it
  // was found in -- see RecordingEpgLinks.h. Guarded by m_dataMutex, like the guide
  // cache it is the durable counterpart of.
  // The recurring rules exactly as GetTimers() last handed them to Kodi, by id.
  // Kodi's timer dialog is filled from that copy, so it -- not the cache, which
  // may have been refreshed since -- is what an edit must be compared against to
  // tell what the user changed. Diffing against the refreshed cache instead treated
  // a change made elsewhere in between as the user's own edit and sent it back
  // (caught live). Guarded by m_dataMutex.
  std::map<int, dispatcharr::RecurringRule> m_reportedRecurringRules;
  // The start and end instants GetTimers() handed Kodi for each of those rules, so UpdateTimer() can tell
  // an edit that left them alone (see dispatcharr::ResolveRecurringRuleTimesOnEdit()). Guarded by m_dataMutex.
  std::map<int, std::pair<time_t, time_t>> m_reportedRecurringRuleTimes;
  dispatcharr::RecordingEpgLinkMap m_recordingEpgLinks;
  bool m_recordingEpgLinksDirty = false;
  // Held across serialize, write and rename in SaveRecordingEpgLinksIfDirty(): GetTimers() and
  // GetRecordings() both save, and two writers sharing one ".tmp" path could interleave their bytes
  // or rename the older text in last. A damaged file is discarded whole on load, losing links the
  // guide can no longer rebuild. Taken before m_dataMutex, never inside it.
  std::mutex m_recordingEpgLinksSaveMutex;
  // Whether the one-time tagging of pre-existing recurring rules is finished, and
  // which rules are still to do -- see ManagedRecurringRule.h. Only touched from the
  // recording-refresh thread (and the constructor, before it starts).
  dispatcharr::RecurringRuleAdoptionState m_recurringAdoption;
  // Consecutive failed tagging attempts per rule this session (AdoptExistingRecurringRulesOnce()).
  std::map<int, int> m_adoptionFailures;
  void AdoptExistingRecurringRulesOnce(std::vector<dispatcharr::RecurringRule>& rules,
                                       const std::vector<dispatcharr::Recording>& recordings, bool haveRecordings,
                                       time_t now);
  void LoadRecurringAdoptionState();
  void SaveRecurringAdoptionState();
  void LoadRecordingEpgLinks();
  // Writes the file when something changed since the last write; the serialization
  // happens under the lock, the file I/O after it.
  void SaveRecordingEpgLinksIfDirty();
  // Looks up one recording by id -- shared by
  // GetRecordingStreamProperties()/OpenRecordedStream() (both need a
  // recording's isInProgress/hlsDirStillPresent flags right before
  // playback) and UpdateTimer()'s extend-recording branch (needs
  // currentEndTime). A direct single-item REST call
  // (DispatcharrClient::GetRecordingById()), not a scan of
  // m_cachedRecordings -- these callers want this recording's truly
  // current state right before acting on it, not a copy that could be up
  // to kRecordingsAndTimersCacheTtlSeconds stale. Returns false
  // (recordingOut left untouched) if the fetch failed or no recording
  // with that id was found.
  bool FindRecordingById(int id, dispatcharr::Recording& recordingOut, long* httpStatusOut = nullptr);
  // Wrap the base class's own TriggerRecordingUpdate()/TriggerTimerUpdate()
  // so every call site that reports "something changed" also invalidates
  // the relevant cache above -- otherwise a timer/recording the user just
  // added or deleted could still read back the pre-change state for up to
  // kRecordingsAndTimersCacheTtlSeconds on the very next refresh.
  void InvalidateAndTriggerRecordingUpdate();
  void InvalidateAndTriggerTimerUpdate();
  // If m_client's current API key differs from keyBefore (captured by the
  // caller right before whatever DispatcharrClient call may have
  // self-healed it -- see OpenRecordedStream()'s own comment for why this
  // can happen and why m_lastAppliedConfig.apiKey is updated before the
  // SetSettingString() below), persists the new key so a later restart of
  // this install doesn't immediately invalidate it again.
  void PersistApiKeyIfChanged(const std::string& keyBefore);
  // Writes the key in use to the stored setting again while a dropped write is outstanding. Background thread only,
  // never from OnAddonSettingChanged() (see its comment on why no SetSetting*() may be called there).
  void ReassertApiKeySetting();
  // Shared by AddTimer()/UpdateTimer() for kTimerTypeRecurring -- converts
  // Kodi's UTC start-end-time-of-day/first-day and its own weekday
  // bitmask (NOT itself timezone-shifted -- see RecurringRuleUtil.h's own
  // comment) into Dispatcharr's own representation (0-6 day list, its
  // configured-system-timezone-local time-of-day via
  // recurring_rule_utc_offset_minutes, a UTC-midnight start date). Returns
  // false (with error set) only when no weekday is selected at all --
  // everything else here is pure, infallible conversion.
  bool ComputeRecurringRuleFields(const kodi::addon::PVRTimer& timer, std::vector<int>& daysOfWeekOut,
                                  int& startSecondsOut, int& endSecondsOut, time_t& startDateOut, std::string& error);

  dispatcharr::DispatcharrClient m_client;

  std::mutex m_dataMutex;
  // Serialize the fetch-and-commit part of EnsureChannelsLoaded()/
  // EnsureEpgLoaded(), so two callers that both see a stale cache fetch
  // once, not twice -- the second waits, then re-checks the gate and finds
  // the first one's result. Held across the network call on purpose, never
  // together with m_dataMutex taken first (always this one, then
  // m_dataMutex), and released before any Kodi Trigger*Update() call.
  // EnsureEpgLoaded() calls EnsureChannelsLoaded() with its own mutex held,
  // never the other way round, so the two can't deadlock.
  //
  // Since 2026-10-03 only the background thread calls EnsureEpgLoaded() at
  // all -- a Kodi callback that finds the guide stale wakes that thread
  // instead (RequestGuideFetchIfWanted()) and serves what is cached. Kodi's
  // own PVR manager thread used to wait here on a guide fetch in progress,
  // and since Kodi stops that thread before it destroys this instance, a
  // guide download stalled on an unresponsive server held a Kodi exit for
  // the whole guide timeout (4x `timeout`, 120 s by default): the abort flag
  // the destructor sets was never reached (reproduced live, docs/OPEN_ITEMS.md,
  // "Kodi's own threads waited on the guide fetch, and two transfers
  // could not be aborted at shutdown"). The channels fetch is still made on Kodi's threads
  // (GetChannels() has to answer synchronously); it is bounded by `timeout`.
  std::mutex m_channelsFetchMutex;
  std::mutex m_epgFetchMutex;
  std::vector<dispatcharr::Channel> m_channels;
  std::vector<dispatcharr::ChannelGroup> m_groups;
  // Keyed by the XMLTV <channel id="..."> value, which is the channel's
  // channel_number (not tvg_id -- see XmlTvParser.h), as the exact text
  // dispatcharr::ChannelGuideKey() produces for it ("5", "5.1", or the
  // channel's id when it has no number at all).
  std::unordered_map<std::string, std::vector<dispatcharr::EpgEntry>> m_epgByChannelNumber;
  // The channel-number keys (same ChannelGuideKey() text as
  // m_epgByChannelNumber's own keys) currently shared by two or more
  // channels in m_channels -- recomputed alongside m_channels itself in
  // EnsureChannelsLoaded() via dispatcharr::FindAmbiguousChannelNumbers()
  // (ChannelRenumbering.h). See that function's own comment for the
  // real, live-confirmed bug this exists to fix (2026-09-28,
  // docs/OPEN_ITEMS.md): GetEPGForChannel()/ResolveRecordingBroadcastId()
  // both check membership here before trusting m_epgByChannelNumber for a
  // channel, since Dispatcharr's own unauthenticated XMLTV export can't
  // tell two colliding channels' programme lists apart at all.
  std::unordered_set<std::string> m_ambiguousChannelNumbers;

  std::chrono::steady_clock::time_point m_channelsLoadedAt{};
  // A GROUPS-specific counterpart to m_channelsLoadedAt above, set only when
  // GetChannelGroups() itself actually succeeds, not merely whenever
  // channels do (added 2026-09-27, a 40th-pass audit, fixing a real,
  // confirmed bug found via a project-wide review, confirmed against
  // Kodi's own real current SDK source, not itself independently
  // reproduced -- see dispatcharr::HasNeverLoadedSuccessfully()'s own
  // comment, Staleness.h, for the full Kodi-side mechanism this guards
  // against). Deliberately separate from m_channelsLoadedAt: groups are
  // best-effort (see EnsureChannelsLoaded()'s own comment on
  // m_groupsLastFailedAt), so channels can succeed -- advancing
  // m_channelsLoadedAt -- on a cycle where the groups fetch itself still
  // fails, especially the very first one, when there's no previously-known-
  // good groups list to fall back to either. Checking m_channelsLoadedAt
  // alone for GetChannelGroups()/GetChannelGroupsAmount()/
  // GetChannelGroupMembers() would miss exactly that case.
  std::chrono::steady_clock::time_point m_groupsLoadedAt{};
  // When EnsureChannelsLoaded() last actually poked Kodi
  // (TriggerChannelUpdate()/TriggerChannelGroupsUpdate()) -- see
  // ShouldTriggerKodiChannelSync() (ChannelLineupChange.h). Guarded by
  // m_dataMutex, same as the other cache timestamps.
  std::chrono::steady_clock::time_point m_lastChannelSyncTriggerAt{};
  std::chrono::steady_clock::time_point m_epgLoadedAt{};
  // Bumped whenever the matching cache is deliberately invalidated, so a fetch that was
  // already in flight when that happened does not commit its (pre-change) result as fresh
  // over the invalidation -- see dispatcharr::IsFetchStillCurrent(). Guarded by m_dataMutex.
  uint64_t m_recordingsGeneration = 0;
  uint64_t m_seriesRulesGeneration = 0;
  uint64_t m_recurringRulesGeneration = 0;
  uint64_t m_epgGeneration = 0;
  // Bumped when something says the channel list in hand is already out of date (an M3U refresh finished, the device
  // woke): a fetch that was already running then commits what it read but does not call it fresh.
  uint64_t m_channelsGeneration = 0;
  // When the guide must be fetched once more after a channel renumbering, or the zero time_point
  // when no such fetch is pending -- see dispatcharr::kGuideRefetchAfterRenumber. Guarded by
  // m_dataMutex.
  std::chrono::steady_clock::time_point m_epgRefetchDueAt{};
  // Set on a failed fetch, cleared on a successful one -- backs
  // dispatcharr::IsRetryDue() (Staleness.h) so a failure doesn't get
  // retried immediately on every single call. Real, confirmed bug this
  // fixes (found via a project-wide review): without this, a failed
  // fetch never updates m_channelsLoadedAt/m_epgLoadedAt at all, so
  // IsStaleSince() keeps reporting "stale" on every subsequent call --
  // and GetEPGForChannel() calls EnsureChannelsLoaded()/EnsureEpgLoaded()
  // once per channel, back-to-back, while Kodi populates its EPG grid.
  // A single failure (Dispatcharr unreachable, a malformed guide) meant
  // every one of potentially hundreds of channels independently retried
  // the full fetch -- up to `timeout`*4 (120s default) each for a
  // genuine timeout -- rather than one failure being noticed once and
  // left alone for a short while. Not applied to
  // m_recordingsCachedAt/m_seriesRulesCachedAt/m_recurringRulesCachedAt's
  // own much shorter (2-second) TTL -- those are refreshed far less
  // densely (not in a per-channel loop), and that cache's whole point is
  // favoring freshness, so a failure backoff there would be a bigger,
  // more deliberate design tradeoff than this fix intends to make.
  std::chrono::steady_clock::time_point m_channelsLastFailedAt{};
  std::chrono::steady_clock::time_point m_epgLastFailedAt{};
  // Consecutive /output/epg failures that were a durable rejection (a 4xx,
  // chiefly the Network Access 403) rather than an outage -- see
  // dispatcharr::ShouldCountTowardEpgFailureBackoff() (Staleness.h). Sets
  // how long m_epgLastFailedAt holds the retry gate shut. Reset by a
  // success, a transient failure, and a wake from sleep. Guarded by
  // m_dataMutex.
  int m_epgDurableFailureCount = 0;
  // A GROUPS-specific failure (channels themselves still loaded fine)
  // doesn't fail EnsureChannelsLoaded() overall, so m_channelsLoadedAt
  // still advances and m_channelsLastFailedAt above still gets cleared
  // -- meaning the channels-vs-EPG failure-backoff pair above can't see
  // a groups-only failure at all. Without this second, independent
  // timestamp (found via a project-wide review, not reproduced live), a
  // transient groups failure -- especially on the very first load, when
  // there's no previously-known-good groups list to fall back to either
  // -- would otherwise stay unretried for the full channel_refresh_hours
  // (12h default), even though channels/EPG are refreshing normally the
  // whole time. Non-zero (a real previous failure, not "never tried")
  // AND dispatcharr::IsRetryDue() true is what actually triggers a
  // groups-only retry attempt inside EnsureChannelsLoaded() even while
  // channels themselves are still fresh -- see its own comment.
  std::chrono::steady_clock::time_point m_groupsLastFailedAt{};
  // How long to leave a failed channels/EPG fetch alone before retrying
  // -- short enough that a genuine transient blip still recovers
  // promptly (the background channel/EPG refresh thread's own periodic
  // cadence is a separate, longer-term retry path regardless), long
  // enough to absorb an entire GetEPGForChannel() per-channel burst
  // without repeating the same failure hundreds of times over.
  // Longest an unchanged channel lineup goes without EnsureChannelsLoaded()
  // re-poking Kodi anyway -- twice channel_refresh_hours' own default, so
  // a poke Kodi's manager missed (both triggers are fire-and-forget) is
  // retried at most a day later instead of never. See
  // ShouldTriggerKodiChannelSync() (ChannelLineupChange.h).
  static constexpr int kChannelSyncMaxSilenceHours = 24;
  static constexpr int kChannelEpgFailureRetryMinutes = 1;

  // How recently channels must have already been loaded for
  // EnsureEpgLoaded() to skip its own forced pre-XMLTV-fetch channel
  // refresh (see that call site's own comment for why the force exists
  // at all) -- added 2026-09-26, a 27th-pass audit, fixing a real,
  // confirmed regression risk in that same fix found via a project-wide
  // review, not itself independently reproduced: without this gate,
  // EVERY EnsureEpgLoaded() attempt forced a full GetChannels()+
  // GetChannelGroups() pair (plus both Trigger*Update() calls), even
  // when channels were already known-fresh -- certainly once at every
  // single startup (the background thread's own first
  // EnsureChannelsLoaded() call, then EnsureEpgLoaded() moments later in
  // the same loop iteration), and, worse, on every single retry of a
  // *persistent* XMLTV failure (e.g. the already-logged Network Access
  // 403 case) -- as often as once a minute from the background thread
  // alone, or a tight burst of Kodi's own GetEPGForChannel() calls
  // within that same minute.
  //
  // Raised from 2 to 10 (2026-09-27, a 45th-pass audit, fixing a real,
  // confirmed regression this value's own original 2-minute margin left
  // open once `ShouldUseShortChannelEpgRefreshWait()` (Staleness.h)
  // shipped one pass later, found via a project-wide review, not itself
  // independently reproduced): a 2-minute margin comfortably absorbed a
  // background-thread loop that only iterated once every 10 minutes
  // (its own cadence at the time this constant was first chosen), but
  // once that loop's own wait shrank to
  // kChannelEpgFailureRetryMinutes (1 minute) for as long as channels/EPG
  // have never successfully loaded even once -- not just briefly at
  // startup, but indefinitely for a channels/EPG fetch that has been
  // failing since the very first attempt (e.g. a Network Access 403
  // present from initial install) -- a 2-minute margin no longer
  // absorbed that loop's own new cadence at all: the force fired again
  // roughly every 2-3 minutes, indefinitely, not the "couple of minutes"
  // one-time startup burst this constant was written to tolerate. 10
  // minutes restores the same amplification rate a persistent failure
  // had before that fix shipped (the background thread's own un-shortened
  // interval), while still being short enough not to meaningfully reopen
  // the renumbering race this fix exists to close in the *normal*
  // (non-failing) case -- real channel/EPG refreshes there are hours
  // apart regardless of this constant's own exact value.
  static constexpr int kForceChannelRefreshMinAgeMinutes = 10;

  // See EnsureRecordingsLoaded()/EnsureTimerRulesLoaded()'s own comment
  // for why these need a much shorter TTL than the channels/EPG pair
  // above. Two separate timestamps: recordings and timer-rules are
  // fetched (and go stale) independently of each other.
  static constexpr int kRecordingsAndTimersCacheTtlSeconds = 2;
  std::vector<dispatcharr::Recording> m_cachedRecordings;
  std::chrono::steady_clock::time_point m_recordingsCachedAt{};
  // Whether recordings / series rules have ever been committed, which invalidation does NOT reset. The cached-at
  // timestamps above double as "never loaded" when zero, but invalidation (and a fetch discarded as out of date)
  // also leave them zero while the data is still held, so a GetTimers() in that window reported a server error to
  // Kodi for a cache that was fine (seen on the real device and the Linux client around every timer change).
  // Guarded by m_dataMutex.
  bool m_recordingsEverLoaded = false;
  bool m_seriesRulesEverLoaded = false;
  std::vector<dispatcharr::TimerRule> m_cachedTimerRules;
  std::vector<dispatcharr::RecurringRule> m_cachedRecurringRules;
  // Two independent timestamps, not one shared m_timerRulesCachedAt (split
  // out 2026-09-27, a 41st-pass audit, fixing a real, confirmed regression
  // in the very fix just committed before this one -- see
  // dispatcharr::HasNeverLoadedSuccessfully()'s own comment, Staleness.h --
  // found via a project-wide review, confirmed against Dispatcharr's own
  // real current upstream source, not itself independently reproduced):
  // series rules and recurring rules are gated by two DIFFERENT permission
  // classes server-side (`SeriesRulesAPIView.get_permissions()`'s GET only
  // needs `IsDVRViewer` -- view or manage -- but
  // `RecurringRecordingRuleViewSet.get_permissions()` needs
  // `IsAdminOrDVRManager` for every method, including GET). A Standard
  // account with no `custom_properties.dvr_access` set at all defaults to
  // `"view"` (`apps/channels/dvr_access.py`'s own `get_dvr_access()`
  // docstring: "Absent or unrecognized: view (opt-out via explicit
  // none)") -- meaning such an account can list series rules fine but
  // always gets a 403 on recurring rules, permanently, not a transient
  // failure.
  // A single shared m_timerRulesCachedAt required BOTH fetches to succeed
  // before ever advancing -- harmless before the pass-40 fix (recurring
  // rules just stayed an empty list forever, series rules/one-time
  // recordings still displayed fine via GetRecordings()), but once
  // GetTimersAmount()/GetTimers() started returning PVR_ERROR_SERVER_ERROR
  // whenever this timestamp had never advanced, such an account's entire
  // Timers list -- one-time recordings and series rules included, not just
  // recurring ones -- went permanently empty instead.
  std::chrono::steady_clock::time_point m_seriesRulesCachedAt{};
  std::chrono::steady_clock::time_point m_recurringRulesCachedAt{};
  // Every setting below is atomic rather than plain, and updated live by
  // OnAddonSettingChanged() (called via CAddonDispatcharr::SetSetting() in
  // addon.cpp, Kodi's own per-setting change notification) rather than
  // only read once at construction. Confirmed as a real bug otherwise, not
  // just a theoretical one: live_timeshift_mode changed via Kodi's addon
  // settings GUI had no effect on an already-running instance until Kodi
  // was fully restarted -- silently defeating the whole point of
  // switching to Off for a non-admin account, since the addon kept trying
  // the now-stale server-side/admin-only path in the meantime. Each of
  // these is read from more than one thread already (Kodi's own
  // PVR-calling threads plus this addon's background refresh threads), so
  // now that a write can also arrive at any time from whichever thread
  // Kodi delivers SetSetting() on, atomic is the correct minimum fix --
  // simpler and less invasive than threading a mutex through every read
  // site, and sufficient since none of these values have a cross-field
  // invariant that needs a single consistent snapshot.
  std::atomic<int> m_channelRefreshHours{12};
  std::atomic<int> m_epgRefreshHours{4};
  // 0 = off, 1 = local (inputstream.ffmpegdirect's own on-device buffer,
  // no Dispatcharr-side cooperation needed), 2 = server-side (this addon's
  // companion Dispatcharr plugin -- see dispatcharr-plugin/timeshift_buffer/
  // in this repo, see kLiveTimeshiftLocal's own comment for the history of
  // 1 being removed and then reintroduced). Off and Local both exist for
  // the account this addon is configured with NOT being (or the owner not
  // wanting it to be) a Dispatcharr admin -- the timeshift_buffer plugin's
  // run/ API requires that role; a plain stream or an on-device ffmpegdirect
  // buffer both need nothing beyond ordinary channel-browsing/streaming
  // permission. Defaults to Off, not server-side: OpenLiveStream() hard-fails
  // every live channel (see its own comment) when the account isn't admin or
  // the plugin isn't installed, which is exactly the state of a fresh
  // install before anyone's done that extra setup -- Off "just works" out
  // of the box, Local and server-side are both explicit opt-ins.
  std::atomic<int> m_liveTimeshiftMode{kLiveTimeshiftOff};
  std::atomic<bool> m_enableCatchupFfmpegdirectSeek{false};
  std::atomic<bool> m_debugLogging{false};
  // Fetched once at startup (see the constructor) via
  // DispatcharrClient::GetServerVersion() and never written again after
  // that -- safe as a plain string despite GetBackendVersion() reading it
  // from Kodi's own calling thread, since the write happens-before this
  // object is ever handed back to Kodi (no background thread touches it).
  // "unknown" if the fetch failed (e.g. addon started while Dispatcharr
  // itself was unreachable).
  std::string m_backendVersion{"unknown"};
  // See recurring_rule_utc_offset_minutes in settings.xml/strings.po --
  // bridges Kodi's UTC-based timer times against Dispatcharr's own
  // recurring-rule scheduler, which interprets a rule's start/end
  // time-of-day using its configured (non-UTC-by-default) system
  // timezone with no server-side conversion available. Only actually used
  // as a fallback now, when recurring_rule_timezone is "manual" or an
  // unrecognized zone -- see EffectiveRecurringRuleUtcOffsetMinutes().
  std::atomic<int> m_recurringRuleUtcOffsetMinutes{0};
  // What UpdateServerOffsetCrossCheck() has learnt about how Dispatcharr's clock differs from the zone table's
  // (ServerOffsetCrossCheck.h); EffectiveRecurringRuleUtcOffsetMinutes() applies the override in force to every instant
  // it is asked about. Written by the timer thread, read by any thread, so under its own mutex.
  mutable std::mutex m_serverOffsetMutex;
  dispatcharr::ServerOffsetTracker m_serverOffsetTracker;
  // Guards OnAddonSettingChanged()'s own recurring_rule_timezone branch
  // against Kodi's re-notify-every-setting-on-every-save quirk (added
  // 2026-09-27, a 49th-pass audit, fixing a real, confirmed gap found
  // via a project-wide review, not itself independently reproduced) --
  // this setting has no other dedicated C++ member, since
  // EffectiveRecurringRuleUtcOffsetMinutes() always reads it live rather
  // than caching it (see that function's own comment on why). Only ever
  // read/written from OnAddonSettingChanged(), plus one seeding read in
  // the constructor (before this instance is reachable from any other
  // thread) -- corrected 2026-09-27, a 51st-pass audit, fixing a real
  // inaccuracy in this comment's own original wording, found via a
  // project-wide review, confirmed against Kodi's own real current
  // source, not itself independently reproduced: that's NOT necessarily
  // "Kodi's own settings/GUI dispatch thread" as originally claimed here
  // -- any addon-initiated SetSettingString() call (SyncTimezoneFromDispatcharr()'s
  // own background-thread retry; PersistApiKeyIfChanged(), reachable from
  // a playback thread) re-delivers every setting synchronously on the
  // CALLING thread (Interface_Base::set_setting_string() ->
  // CAddonDll::SaveSettings() -> TransferSettings(), AddonBase.cpp/
  // AddonDll.cpp), so OnAddonSettingChanged() itself can run on any of
  // them -- with one already-logged exception (verified 2026-09-27, a
  // 52nd-pass audit, correcting this comment's own prior "no thread
  // marshaling anywhere in that chain" overstatement, then itself
  // corrected the very next, 53rd, pass for overcorrecting: this GUI
  // path doesn't skip OnAddonSettingChanged() forever, just defers and
  // relocates it): when this addon's own settings dialog happens to be
  // open, Interface_Base::set_setting_string() instead routes through
  // UpdateSettingInActiveDialog() (AddonBase.cpp), posting a
  // GUI_MSG_SETTING_UPDATED message the open dialog stores as its own
  // pending value (GUIDialogAddonSettings.cpp) rather than calling
  // OnAddonSettingChanged() right then -- but if the user saves that
  // dialog (corrected 2026-09-27, a 54th-pass audit, fixing a real,
  // confirmed wrong-function citation in this comment's own prior
  // wording, found via a project-wide review, confirmed against Kodi's
  // own real current source, not itself independently reproduced): the
  // real save path is CGUIDialogAddonSettings::ShowForSingleInstance()'s
  // own post-Open() check, not SaveAndClose() (that one's only caller is
  // an action-button setting with close-dialog set, AddonSettings.cpp --
  // this addon's own settings.xml has no such button, so it's simply
  // never reached here). `dialog->Open()` blocks until the dialog closes;
  // once it returns, `!dialog->IsConfirmed()` (Back/Cancel) discards via
  // `ReloadSettings()`, otherwise `SaveSettings()` -> `TransferSettings()`
  // runs, reaching OnAddonSettingChanged() after all, later and on
  // whichever thread actually opened the dialog (normally Kodi's own GUI/
  // app thread for a binary PVR addon's settings, reached through its
  // own Settings > Add-ons configuration UI -- not necessarily true in
  // general, e.g. a Python addon's own `xbmcaddon.Addon().openSettings()`
  // calls the equivalent `ShowForAddon()` directly on its own script
  // thread, though that specific path doesn't apply to this addon).
  // Only a *cancelled* dialog discards the value entirely -- see
  // docs/OPEN_ITEMS.md's own 22nd-pass entry on that separate,
  // already-tracked gap. Still safe
  // with no extra locking of its own, because CAddonDispatcharr::SetSetting()
  // (addon.cpp) takes m_instancesMutex around every call into
  // OnAddonSettingChanged(), on every thread, serializing all of them
  // against each other -- the same reasoning `m_lastAppliedPreOffsetMinutes`'s
  // own comment already gives for that pair of members.
  std::string m_lastAppliedRecurringRuleTimezone;

  // Recordings/timers only ever get re-fetched by Kodi when this addon
  // calls TriggerRecordingUpdate()/TriggerTimerUpdate() -- unlike channels/
  // EPG above, there's no lazy "check staleness next time Kodi asks"
  // option, since Kodi only asks again once told to. Every existing call
  // site is reactive (right after this addon's own AddTimer()/
  // DeleteTimer()/etc.), so a change that happens with no local Kodi
  // action to react to -- another Kodi install's action, a change made
  // directly against Dispatcharr's API, a scheduled recording finishing on
  // its own -- had no way to ever surface short of restarting Kodi.
  // Confirmed: a recording deleted directly via Dispatcharr's API (not
  // through this addon) kept showing in Kodi indefinitely until restarted.
  // This background thread closes that gap by triggering periodically
  // regardless of local activity. Started in the constructor, joined in
  // the destructor.
  void StartRecordingRefreshThread();
  // Keeps every enabled recurring rule's Dispatcharr-side end_date topped
  // up (see kRecurringRuleWindowDays's own comment for why this exists at
  // all) -- called from the recording-refresh thread's own cadence rather
  // than a dedicated thread/setting, since it needs to run periodically
  // regardless of local activity the same way that thread's existing
  // TriggerRecordingUpdate()/TriggerTimerUpdate() calls do. Best-effort:
  // a failed GetRecurringRules()/GetRecordings()/ExtendRecurringRuleEndDate()
  // call just tries again next cycle, same as the rest of this thread.
  void RenewRecurringRules();
  std::thread m_recordingRefreshThread;
  std::mutex m_recordingRefreshMutex;
  std::condition_variable m_recordingRefreshCv;
  std::atomic<bool> m_stopRecordingRefreshThread{false};
  // Atomic and re-read fresh every wait_for() cycle (see
  // StartRecordingRefreshThread()) -- OnAddonSettingChanged() updating
  // this takes effect on the thread's very next wake, no restart needed.
  std::atomic<int> m_recordingRefreshMinutes{5};
  // std::condition_variable::wait_for()'s own deadline is computed once,
  // at the moment it's entered, from whatever m_recordingRefreshMinutes
  // was at that instant -- a bare notify_all() after changing the
  // setting wakes the thread, but its predicate only checks the stop
  // flag, so it just goes back to sleep until that SAME original
  // (stale) deadline. A real, confirmed bug this fixes (found via code
  // reading): the existing notify_all() in OnAddonSettingChanged()'s own
  // recording_refresh_minutes branch, and this member's own previous
  // comment, both claimed the change "takes effect on the thread's very
  // next wake" -- it didn't. Same pattern as m_wakeRealtimeUpdateThread
  // below: set alongside the notify, checked by the wait predicate, so
  // StartRecordingRefreshThread() can tell "asked to re-wait with a new
  // interval" apart from "asked to stop" apart from a genuine timeout.
  std::atomic<bool> m_recordingRefreshIntervalChanged{false};

  // Channels/EPG were previously only ever loaded synchronously, on
  // whichever Kodi-owned thread first called GetChannels()/
  // GetEPGForChannel() after the cache went stale (EnsureChannelsLoaded()/
  // EnsureEpgLoaded() above did the actual blocking fetch inline). Fine
  // for a home server with one Dispatcharr instance and a moderate
  // channel count, but a real full XMLTV fetch+parse is genuinely slow
  // enough to be worth moving off Kodi's calling thread. This thread
  // proactively calls the same EnsureChannelsLoaded()/EnsureEpgLoaded()
  // (unchanged -- still the correctness fallback if this thread hasn't
  // caught up yet, e.g. the first moment after construction) on a much
  // shorter cadence than channel_refresh_hours/epg_refresh_hours actually
  // requires, so in steady state the cache is essentially always already
  // warm by the time Kodi asks and those calls become instant, cache-only
  // reads. When a check here does find the cache stale and refreshes it,
  // this thread also calls TriggerChannelUpdate()/
  // TriggerChannelGroupsUpdate()/TriggerEpgUpdate() itself, the same way
  // the recording refresh thread above does for recordings/timers --
  // Kodi's own periodic EPG re-poll would eventually pick this up anyway,
  // but there's no reason to wait for that when this addon already knows
  // the moment new data landed. Started in the constructor, joined in the
  // destructor.
  void StartChannelEpgRefreshThread();
  std::thread m_channelEpgRefreshThread;
  std::mutex m_channelEpgRefreshMutex;
  std::condition_variable m_channelEpgRefreshCv;
  std::atomic<bool> m_stopChannelEpgRefreshThread{false};
  // Set (under m_channelEpgRefreshMutex) by RequestGuideFetchIfWanted() so the
  // thread's wait ends at once; cleared by the thread when it starts a cycle.
  std::atomic<bool> m_channelEpgRefreshRequested{false};
  // Wakes the background thread for a guide fetch when one would be attempted
  // now (dispatcharr::ShouldAttemptGuideFetch()); a no-op otherwise. What a Kodi
  // callback does instead of fetching the guide itself -- see m_epgFetchMutex.
  void RequestGuideFetchIfWanted();
  // Acts on a finished EPG or M3U refresh seen on the realtime socket: schedules the guide fetch, or ages the channel
  // list so the background thread fetches it, and wakes that thread. Called from the realtime thread.
  void HandleRefreshEvent(dispatcharr::RefreshEventKind kind);
  static constexpr int kChannelEpgRefreshCheckMinutes = 10;

  // Real-time alternative/complement to the polling thread above: connects
  // to Dispatcharr's own WebSocket push (ws(s)://host:port/ws/?token=<JWT>,
  // the exact channel its own frontend uses -- no plugin or server-side
  // change needed, confirmed by reading Dispatcharr's own source) and
  // triggers a Kodi refresh the moment a relevant recording/timer event
  // actually happens, instead of waiting out the polling interval. Kept
  // as an addition to, not a replacement for, the polling thread: if the
  // connection can't be established or drops and stays down (a firewall,
  // an older Dispatcharr version without this channel, a network blip
  // outlasting the reconnect backoff), the poll above still gets there
  // eventually. Opt-in (enable_realtime_updates, off by default) since
  // this is genuinely new, non-trivial networking code (a hand-rolled
  // RFC 6455 client -- see WebSocketClient.h for why it isn't just
  // curl's own WebSocket support) that hasn't seen the real-world use
  // everything else in this addon has.
  void StartRealtimeUpdateThread();
  void HandleRealtimeUpdateMessage(const std::string& message);
  std::thread m_realtimeUpdateThread;
  std::mutex m_realtimeUpdateMutex;
  std::condition_variable m_realtimeUpdateCv;
  std::atomic<bool> m_stopRealtimeUpdateThread{false};
  // Set by OnSystemWake() -- checked in two different places
  // (StartRealtimeUpdateThread()'s own inner read loop, and the outer
  // reconnect wait, see each one's own comment), since a real OS/device
  // suspend can catch this thread in either state. If the thread is
  // currently disconnected and waiting out a reconnect backoff, this
  // cuts that wait short instead of leaving it for up to 60s. If the
  // thread is currently connected, this instead forces an immediate
  // reconnect rather than trusting a session that was already open
  // going into suspend -- a real gap found via a project-wide review
  // (2026-09-26) in an earlier version of this fix, which only handled
  // the disconnected case; a plain read timeout can't itself prove a
  // suspended-then-resumed session is still alive (that's what TCP
  // keepalive is for, but it can take up to ~195s on Linux -- see
  // WebSocketClient::Connect()'s own comment -- far slower than an
  // explicit wake nudge should have to wait). Found via a comparative-
  // architecture review against pvr.hts/Tvheadend, which explicitly
  // hooks OS sleep/wake for its own (more stateful) HTSP subscription --
  // this addon's read/recording paths are stateless HTTP polls that
  // don't need the same handling, so this is the one place a sleep/wake
  // hook is actually worth something here.
  std::atomic<bool> m_wakeRealtimeUpdateThread{false};
  // Atomic because OnAddonSettingChanged() now reads-then-writes this (to
  // detect a genuine change vs. the spurious-renotification quirk
  // documented on m_lastAppliedConfig above) on whatever thread Kodi
  // delivers SetSetting() on, not just the constructor's own thread.
  std::atomic<bool> m_enableRealtimeUpdates{false};
};
