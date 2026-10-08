#include "PVRDispatcharr.h"

#include "AuthBackoff.h"
#include "ChannelGroupFilter.h"
#include "ChannelLineupChange.h"
#include "ChannelNumber.h"
#include "ChannelRenumbering.h"
#include "EpgProgramMatch.h"
#include "EpgTagUtil.h"
#include "RealtimeUpdateParser.h"
#include "RecordingDirectory.h"
#include "RecordingVisibility.h"
#include "RecurringRuleRenewal.h"
#include "RecurringRuleUtil.h"
#include "RecurringRuleWeekdays.h"
#include "RefreshEvents.h"
#include "SeriesRuleMatching.h"
#include "ServerOffsetCrossCheck.h"
#include "Staleness.h"
#include "StreamPropertyUtil.h"
#include "StringUtil.h"
#include "TimeUtil.h"
#include "TimeZoneUtil.h"
#include "TimerRequestBuilder.h"
#include "TimerIdentity.h"
#include "WebSocketClient.h"

#include <kodi/AddonBase.h>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <new>
#include <ctime>
#include <functional>
#include <thread>

using namespace dispatcharr;

namespace
{

// Shared by the destructor for each of the three background threads below:
// set the stop flag under its mutex, wake it, then join.
void StopWorkerThread(std::mutex& mutex, std::atomic<bool>& stopFlag, std::condition_variable& cv, std::thread& thread)
{
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopFlag = true;
  }
  cv.notify_all();
  if (thread.joinable())
    thread.join();
}

// Runs one of the long-lived worker loops so that an exception cannot leave it: an exception out of a
// std::thread body is std::terminate, which takes the whole Kodi process down (found by the 2026-10-04
// second hardening sweep -- an unparseable API response used to be one; the body parse is guarded now,
// but each of these loops makes dozens of calls into code that can throw, bad_alloc included). On one
// the loop is logged, left, and entered again after a pause that watches the stop flag, so a persistent
// fault retries every 30 s instead of spinning and shutdown is never held up by the pause.
void RunGuardedWorker(const char* name, const std::atomic<bool>& stopFlag, const std::function<void()>& body)
{
  while (true)
  {
    try
    {
      body();
      return;
    }
    catch (const std::exception& e)
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: the %s thread hit an exception and restarts: %s", name,
                e.what());
    }
    catch (...)
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: the %s thread hit an unknown exception and restarts",
                name);
    }
    for (int i = 0; i < 30 && !stopFlag.load(); ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
    if (stopFlag.load())
      return;
  }
}

// What m_apiKeyOwnerHost actually stores (added 2026-09-27, a 48th-pass
// audit, fixing a real, confirmed gap found via a project-wide review,
// not itself independently reproduced): the API-key owner comparison used
// to compare only host and username, so switching to a *different*
// Dispatcharr instance reachable on the same host but a different port
// or scheme (two containers on 127.0.0.1 at different ports, a staging
// instance) with the same username kept treating the old instance's key
// as already belonging to the new one -- an API key never expires on its
// own, unlike a host/username mismatch caught by the existing check, so
// this gap was worse in that respect: OpenRecordedStream() skipped
// regeneration entirely (ownership already marked verified), silently
// sending the *previous* server's key to the *new* one in `X-API-Key`
// until it self-healed via a 401. Folding port/scheme into the same
// stored identity closes this the same minimal way the host/username
// check itself already works, without the bigger "read the server's own
// current key" redesign OPEN_ITEMS.md's own related entry already
// defers.
std::string ComputeApiKeyOwnerServer(const Config& config)
{
  return (config.useHttps ? "https://" : "http://") + config.host + ":" + std::to_string(config.port);
}

} // namespace

dispatcharr::Config PVRDispatcharr::LoadConfigFromSettings() const
{
  Config config;
  config.host = kodi::addon::GetSettingString("host", "127.0.0.1");
  config.port = kodi::addon::GetSettingInt("port", 9191);
  config.useHttps = kodi::addon::GetSettingBoolean("use_https", false);
  config.username = kodi::addon::GetSettingString("username", "");
  config.password = kodi::addon::GetSettingString("password", "");
  config.verifySsl = kodi::addon::GetSettingBoolean("verify_ssl", true);
  config.timeoutSeconds = kodi::addon::GetSettingInt("timeout", 30);
  config.apiKey = kodi::addon::GetSettingString("api_key", "");
  return config;
}

int PVRDispatcharr::EffectiveRecurringRuleUtcOffsetMinutes(time_t at) const
{
  std::string zone = kodi::addon::GetSettingString("recurring_rule_timezone", "manual");
  if (zone != "manual")
  {
    int computed = 0;
    if (DispatcharrClient::ComputeKnownZoneOffsetMinutes(zone, at, computed))
    {
      dispatcharr::ServerOffsetOverride serverOverride;
      {
        std::lock_guard<std::mutex> lock(m_serverOffsetMutex);
        // An override learnt for another zone is not this one's (UpdateServerOffsetCrossCheck() has not run for the
        // new setting yet).
        if (m_serverOffsetTracker.zone == zone)
          serverOverride = m_serverOffsetTracker.active;
      }
      int legacy = computed;
      if (serverOverride.mode == dispatcharr::ServerOffsetOverride::Mode::kLegacyZoneData)
        DispatcharrClient::ComputeKnownZoneOffsetMinutes(zone, at, legacy, /*applyZoneRuleChanges=*/false);
      return dispatcharr::ApplyServerOffsetOverride(serverOverride, computed, legacy);
    }
  }
  return m_recurringRuleUtcOffsetMinutes;
}

void PVRDispatcharr::UpdateServerOffsetCrossCheck(const std::vector<dispatcharr::Recording>& recordings,
                                                  const std::vector<RecurringRule>& recurringRules)
{
  const std::string zone = kodi::addon::GetSettingString("recurring_rule_timezone", "manual");
  dispatcharr::ServerOffsetJudgement judgement;
  if (zone != "manual")
  {
    std::map<int, int> ruleStartTimeOfDay;
    for (const RecurringRule& rule : recurringRules)
      ruleStartTimeOfDay[rule.id] = rule.startTimeOfDaySeconds;
    std::vector<dispatcharr::OccurrenceRef> occurrences;
    occurrences.reserve(recordings.size());
    for (const Recording& rec : recordings)
      occurrences.push_back({rec.recurringRuleId, rec.startTime});
    constexpr size_t kMaxSamples = 6;
    const auto samples = dispatcharr::SelectOffsetSamples(occurrences, ruleStartTimeOfDay, time(nullptr), kMaxSamples);
    judgement = dispatcharr::JudgeServerOffset(
        [&zone](time_t at, int& out) { return DispatcharrClient::ComputeKnownZoneOffsetMinutes(zone, at, out); },
        [&zone](time_t at, int& out)
        { return DispatcharrClient::ComputeKnownZoneOffsetMinutes(zone, at, out, /*applyZoneRuleChanges=*/false); },
        samples);
  }
  dispatcharr::ServerOffsetChange change;
  dispatcharr::ServerOffsetOverride active;
  {
    std::lock_guard<std::mutex> lock(m_serverOffsetMutex);
    change = dispatcharr::UpdateServerOffsetTracker(m_serverOffsetTracker, zone, judgement);
    active = m_serverOffsetTracker.active;
  }
  using Change = dispatcharr::ServerOffsetChange;
  if (change == Change::kCleared)
    kodi::Log(ADDON_LOG_INFO,
              "pvr.dispatcharr-unofficial: no longer adjusting recurring-rule times for a difference between "
              "Dispatcharr's clock and the %s zone table",
              zone.c_str());
  else if (change == Change::kActivated || change == Change::kChanged)
  {
    using Mode = dispatcharr::ServerOffsetOverride::Mode;
    const std::string how =
        active.mode == Mode::kLegacyZoneData
            ? "the zone's rules as they were before 2026 (its time zone data is older than the table)"
            : "a " + dispatcharr::FormatUtcOffsetMinutes(active.tableOffsetMinutes + active.deltaMinutes) +
                  " offset where the table gives " + dispatcharr::FormatUtcOffsetMinutes(active.tableOffsetMinutes);
    kodi::Log(ADDON_LOG_WARNING,
              "pvr.dispatcharr-unofficial: Dispatcharr schedules its recurring rules with %s, not what this addon's %s "
              "zone table gives; using what Dispatcharr applies",
              how.c_str(), zone.c_str());
    kodi::QueueNotification(QUEUE_WARNING, "",
                            "Dispatcharr's clock rules differ from this addon's table for its time zone; recurring "
                            "timers use the offset Dispatcharr actually applies.");
  }
}

bool PVRDispatcharr::SyncTimezoneFromDispatcharr(bool* zoneChangedOut)
{
  if (zoneChangedOut)
    *zoneChangedOut = false;
  // Same self-heal-on-every-startup reasoning as the constructor's own
  // recording_pre/post_offset_minutes padding sync (PVRDispatcharr.cpp's
  // own comment there). Dispatcharr's own configured IANA timezone name
  // is always surfaced as a read-only reference. When that zone is one
  // of the short list
  // DispatcharrClient::ComputeKnownZoneOffsetMinutes() knows the DST rules
  // for, recurring_rule_timezone is auto-selected to match it, the same
  // authoritative "Dispatcharr's real value wins" way the padding settings
  // already work -- not just a suggestion, since a stale manual
  // offset silently makes recurring timers fire at the wrong time. Note
  // this only picks the *zone*, not a numeric offset: the actual offset is
  // computed live wherever it's actually needed (see
  // EffectiveRecurringRuleUtcOffsetMinutes()), so it can never go stale
  // across a DST transition the way pre-computing it once here would --
  // that was a real gap in an earlier version of this sync. Any zone
  // outside the known list leaves recurring_rule_timezone at "manual" (the
  // default), falling back to the existing plain manual offset entry --
  // this addon still can't derive an arbitrary zone's current offset
  // without bundling a real timezone database (see docs/RECURRING_RULES.md
  // for why that was deliberately ruled out).
  //
  // Returns whether the sync itself succeeded (a real answer was read
  // from Dispatcharr), regardless of which zone it resolved to -- callers
  // use this to decide whether it's still worth retrying later, not to
  // judge the zone choice itself.
  std::string timeZone, tzError;
  if (!m_client.GetSystemTimeZone(timeZone, tzError))
  {
    if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not read Dispatcharr's system timezone: %s",
                tzError.c_str());
    }
    return false;
  }

  // Both writes below are checked by reading the setting back (see the last
  // comment in this function): Kodi discards an addon-initiated SetSetting*()
  // while this addon's settings dialog is open and then cancelled, and the
  // call returns void either way.
  if (kodi::addon::GetSettingString("dispatcharr_timezone_info", "") != timeZone)
    kodi::addon::SetSettingString("dispatcharr_timezone_info", timeZone);

  // The dropdown only knows a zone by its modern name, while Dispatcharr's web
  // UI can store an older spelling (Asia/Calcutta): select the canonical one,
  // or "manual" for a zone the table has no rules for --
  // dispatcharr::DesiredRecurringRuleTimezoneSetting() (TimeZoneUtil.h, tested).
  bool known = false;
  std::string desiredZoneSetting = dispatcharr::DesiredRecurringRuleTimezoneSetting(timeZone, time(nullptr), &known);
  if (kodi::addon::GetSettingString("recurring_rule_timezone", "manual") != desiredZoneSetting)
  {
    // Only worth the extra authenticated request (GetSupportedTimezones()
    // -- see its own comment) when there's actually something to debug
    // and someone's turned debug logging on to see it: distinguishes "a
    // real IANA zone, this addon just has no DST rule for it" from "not
    // a recognized zone at all", which matters for telling a genuinely
    // unusual Dispatcharr misconfiguration apart from this addon's own,
    // deliberately narrow zone coverage (see kKnownTimeZones's comment).
    std::string zoneKindNote = known ? "known zone" : "unrecognized zone, falling back to manual offset entry";
    if (!known && m_debugLogging)
    {
      std::vector<std::string> supported;
      std::string tzListError;
      if (m_client.GetSupportedTimezones(supported, tzListError))
      {
        bool realZone = std::find(supported.begin(), supported.end(), timeZone) != supported.end();
        zoneKindNote = realZone ? "a real IANA zone, but this addon has no DST rule for it yet -- falling "
                                  "back to manual offset entry"
                                : "not a recognized IANA zone at all (per Dispatcharr's own timezone list) "
                                  "-- falling back to manual offset entry";
      }
    }
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: setting recurring_rule_timezone=%s (%s)",
              desiredZoneSetting.c_str(), zoneKindNote.c_str());
    kodi::addon::SetSettingString("recurring_rule_timezone", desiredZoneSetting);
    // Read back rather than assumed. Kodi's UpdateSettingInActiveDialog()
    // routes this write into the open settings dialog's own pending value
    // when that dialog is showing this addon, and drops it if the dialog is
    // then cancelled -- and since this returns true once Dispatcharr's answer
    // was read, the caller would latch m_timezoneSynced and never retry,
    // leaving every recurring rule scheduled with the wrong zone for the
    // session. Not synced yet, then: the background thread tries again, and
    // the write lands once no dialog is in the way.
    if (kodi::addon::GetSettingString("recurring_rule_timezone", "manual") != desiredZoneSetting)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr-unofficial: recurring_rule_timezone write didn't land (settings dialog open?) -- "
                "will retry");
      return false;
    }
    if (zoneChangedOut)
      *zoneChangedOut = true;
  }
  return true;
}

bool PVRDispatcharr::SyncDvrPaddingFromDispatcharr()
{
  // Dispatcharr's recording pre/post padding is genuinely global-only --
  // no per-timer override exists server-side (confirmed against its own
  // source) -- so rather than a Kodi per-timer margin UI, which would
  // misleadingly imply a per-timer effect Dispatcharr doesn't have, this
  // is surfaced as a plain settings-screen value that mirrors
  // Dispatcharr's real global setting directly. Synced FROM Dispatcharr
  // on startup -- and retried by the background thread until it has
  // worked once (RetryDeferredServerSyncs(); it used to be tried exactly
  // once, so a Dispatcharr that wasn't reachable yet at Kodi startup left
  // the displayed padding wrong for the whole session) -- so Kodi's
  // display doesn't stay stale relative to a change made another way
  // (Dispatcharr's own web UI, a different Kodi install sharing the
  // account). Same self-heal reasoning as the API key, just reading
  // instead of generating. Only actually rewrites Kodi's own persisted
  // setting if the value is genuinely different, so a normal restart with
  // nothing changed doesn't churn OnAddonSettingChanged() for no reason.
  //
  // Returns true once Kodi's own storage holds what Dispatcharr reports.
  // False when Dispatcharr couldn't be asked, or when a write didn't land:
  // a real server value above settings.xml's 1440-minute cap is rejected
  // by Kodi's own CSettingInt::CheckValidity() with nothing returned to
  // say so, and an addon-initiated SetSetting*() made while this addon's
  // settings dialog is open is discarded if that dialog is cancelled.
  int pre = 0, post = 0;
  std::string offsetError;
  uint64_t editSeqAtStart = 0;
  {
    std::lock_guard<std::mutex> lock(m_paddingPushMutex);
    editSeqAtStart = m_paddingPush.nextSeq;
  }
  if (!m_client.GetDvrOffsetMinutes(pre, post, offsetError))
  {
    if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not read Dispatcharr's DVR padding settings: %s",
                offsetError.c_str());
    }
    return false;
  }
  {
    // A padding edit made meanwhile (or one whose push is still queued or in flight) is newer than
    // what was just read; writing the server's value over Kodi's setting would revert it, and seed
    // the spurious-change guard below with the old value. Tried again next cycle.
    std::lock_guard<std::mutex> lock(m_paddingPushMutex);
    if (!dispatcharr::IsSafeToSyncPaddingFromServer(m_paddingPush, editSeqAtStart))
    {
      if (m_debugLogging)
      {
        kodi::Log(ADDON_LOG_DEBUG,
                  "pvr.dispatcharr-unofficial: DVR padding sync deferred, an edit is pending or was just made");
      }
      return false;
    }
  }

  // m_lastApplied* is set to the value BEFORE each write: Kodi re-delivers
  // settings to OnAddonSettingChanged() after one, and a delivery that finds
  // a value differing from m_lastApplied* pushes it to Dispatcharr -- here,
  // that would push Dispatcharr's own value straight back at it.
  if (kodi::addon::GetSettingInt("recording_pre_offset_minutes", -1) != pre)
  {
    m_lastAppliedPreOffsetMinutes = pre;
    kodi::addon::SetSettingInt("recording_pre_offset_minutes", pre);
  }
  if (kodi::addon::GetSettingInt("recording_post_offset_minutes", -1) != post)
  {
    m_lastAppliedPostOffsetMinutes = post;
    kodi::addon::SetSettingInt("recording_post_offset_minutes", post);
  }
  // Seeds OnAddonSettingChanged()'s own spurious-renotification guard
  // (see m_lastAppliedPreOffsetMinutes's own comment) with what Kodi's
  // own setting storage actually now holds -- read back *after* the
  // write attempt above, not Dispatcharr's own server value directly.
  // Fix for a real, confirmed bug found via a project-wide review (a
  // 27th-pass audit), confirmed against Kodi's own real source: a real
  // server-side value above the settings.xml cap (Dispatcharr's own DVR
  // settings form sets no maximum at all) makes `SetSettingInt()` above a
  // silent no-op, and seeding m_lastApplied* from the server's own
  // out-of-range value left it disagreeing with what Kodi's storage
  // *actually* has -- so the very next re-delivery of every setting read
  // Kodi's real value as "changed" from the wrongly-seeded baseline and
  // PATCHed it back to Dispatcharr, silently destroying the user's real,
  // intentional server-side padding with no user action needed.
  const int storedPre = kodi::addon::GetSettingInt("recording_pre_offset_minutes", pre);
  const int storedPost = kodi::addon::GetSettingInt("recording_post_offset_minutes", post);
  m_lastAppliedPreOffsetMinutes = storedPre;
  m_lastAppliedPostOffsetMinutes = storedPost;
  return storedPre == pre && storedPost == post;
}

bool PVRDispatcharr::SyncAdminStatusFromDispatcharr()
{
  // Gates recording_pre_offset_minutes/recording_post_offset_minutes (see
  // settings.xml's own comment there) via the read-only-reference
  // dispatcharr_is_admin setting, since Dispatcharr itself rejects a padding
  // write from a non-admin account (see IsCurrentUserAdmin()'s own comment
  // for how this was confirmed to be the exact same permission check).
  // Deliberately fails OPEN (leaves dispatcharr_is_admin at its default
  // `true`, i.e. not greyed out) when the check itself fails -- e.g.
  // Dispatcharr unreachable at startup -- rather than failing closed: a
  // false negative here just reproduces today's pre-this-feature behavior
  // (the save still fails server-side for a genuine non-admin), while a
  // false positive greying it out would strand a real admin looking at a
  // disabled field with no explanation. Retried by the background thread
  // until it has answered once (RetryDeferredServerSyncs()).
  bool isAdmin = true;
  std::string adminError;
  if (!m_client.IsCurrentUserAdmin(isAdmin, adminError))
  {
    if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not check Dispatcharr admin status: %s",
                adminError.c_str());
    }
    return false;
  }

  if (kodi::addon::GetSettingBoolean("dispatcharr_is_admin", true) != isAdmin)
  {
    kodi::addon::SetSettingBoolean("dispatcharr_is_admin", isAdmin);
    // Read back, like SyncTimezoneFromDispatcharr()'s own write: Kodi drops an
    // addon-initiated SetSetting*() while this addon's settings dialog is open
    // and then cancelled, and a flag left at the wrong value would never be
    // written again once this reported success.
    if (kodi::addon::GetSettingBoolean("dispatcharr_is_admin", true) != isAdmin)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr-unofficial: dispatcharr_is_admin write didn't land (settings dialog open?) -- will "
                "retry");
      return false;
    }
  }

  // live_timeshift_mode's own dropdown can't be restricted the way the
  // padding settings above are -- confirmed against Kodi's own source
  // (xbmc/settings/lib/SettingDefinitions.h's IntegerSettingOption/
  // TranslatableIntegerSettingOption) that a single list/option control
  // has no per-option enable/disable concept at all, only the
  // whole-setting <dependencies> mechanism used elsewhere in this file
  // -- and disabling the *whole* dropdown would incorrectly block Off/
  // Local too, which need no admin account. So this is a one-time
  // warning instead of a UI restriction: without it, a non-admin account
  // with Server-side configured would just silently hard-fail every live
  // channel via OpenLiveStream() (see its own comment), with nothing but a
  // kodi.log line explaining why.
  if (m_liveTimeshiftMode == kLiveTimeshiftServer && !isAdmin)
  {
    kodi::QueueNotification(QUEUE_WARNING, "",
                            "Live TV pause/rewind is set to Server-side, but this Dispatcharr account "
                            "isn't an admin -- live channels will fail to play. Switch to Off or Local, "
                            "or use an admin account.");
  }
  return true;
}

bool PVRDispatcharr::SyncDvrAccessFromDispatcharr()
{
  // A view-only account (Dispatcharr's own default for a Standard user nobody
  // configured) is refused every create/update/delete/stop/rename the DVR actions
  // make, so GetCapabilities() stops advertising them -- Kodi then hides the Record
  // button, the Timers window and the delete/rename actions instead of offering
  // things that can only fail. Fails OPEN when the level can't be read, for the
  // same reason SyncAdminStatusFromDispatcharr() does: Dispatcharr's own check
  // stays authoritative either way, and a false "view-only" would strand a real
  // admin with no explanation. Retried by the background thread until it answers.
  //
  // Kodi reads capabilities once per instance, so a level learned only on a later
  // retry (Dispatcharr unreachable at startup) cannot change what is offered until
  // the next restart; the user is told so rather than left with actions that fail.
  DvrAccess access = DvrAccess::kManage;
  std::string accessError;
  if (!m_client.GetCurrentUserDvrAccess(access, accessError))
  {
    if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not read the account's DVR access level: %s",
                accessError.c_str());
    }
    return false;
  }
  const bool canManage = dispatcharr::CanManageDvr(access);
  const bool wasAllowed = m_dvrManageAllowed.exchange(canManage);
  if (!canManage)
  {
    kodi::Log(ADDON_LOG_INFO,
              "pvr.dispatcharr-unofficial: this account's DVR access is %s -- recording, deleting and renaming are "
              "not offered",
              access == DvrAccess::kNone ? "none" : "view-only");
    if (wasAllowed && m_capabilitiesRead)
    {
      kodi::QueueNotification(QUEUE_WARNING, "",
                              "This Dispatcharr account can't manage recordings (view-only). Restart Kodi to "
                              "hide the record, delete and rename actions.");
    }
  }
  return true;
}

bool PVRDispatcharr::SyncCatchupFlagsFromDispatcharr()
{
  // See m_catchupEnabledGlobally's own comment: both fail open, and both are
  // retried by the background thread until each has answered once.
  std::string catchupError;
  bool global = m_catchupEnabledGlobally;
  bool user = m_catchupEnabledForCurrentUser;
  const bool globalOk = m_client.IsCatchupEnabledGlobally(global, catchupError);
  if (!globalOk && m_debugLogging)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not check global catch-up setting: %s",
              catchupError.c_str());
  }
  const bool userOk = m_client.IsCatchupEnabledForCurrentUser(user, catchupError);
  if (!userOk && m_debugLogging)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not check per-user catch-up setting: %s",
              catchupError.c_str());
  }
  const bool changed = global != m_catchupEnabledGlobally || user != m_catchupEnabledForCurrentUser;
  m_catchupEnabledGlobally = global;
  m_catchupEnabledForCurrentUser = user;
  // A late answer of "off" must reach Kodi's own channel list: GetChannels()
  // is what sets each channel's has-archive flag from these.
  if (changed)
    TriggerChannelUpdate();
  return globalOk && userOk;
}

void PVRDispatcharr::RetryDeferredServerSyncs()
{
  // Called once per background-thread cycle. Each of these used to be tried
  // exactly once, at construction, so a Dispatcharr that wasn't reachable yet
  // at Kodi startup (the same startup-timing class of gap already fixed for
  // channels/EPG, the API key and the timezone) left it undone for the whole
  // session. Cheap once done: nothing is called after the first success.

  // A padding change that couldn't be pushed is retried, and takes priority
  // over syncing FROM the server: that sync reads Dispatcharr's own value and
  // overwrites Kodi's, so running it first would quietly revert the very edit
  // the user made while Dispatcharr was unreachable.
  bool anyPending = false;
  {
    std::lock_guard<std::mutex> lock(m_paddingPushMutex);
    anyPending = m_paddingPush.HasPending();
  }
  if (anyPending)
  {
    std::string error;
    int retriesBefore = 0;
    dispatcharr::PaddingRetryOutcome outcome = dispatcharr::PaddingRetryOutcome::kSent;
    std::optional<dispatcharr::PaddingEdit> sentPre, sentPost;
    // What to send is read again once the client's settings mutex is held: a direct push that
    // succeeded while this waited for it has cleared what was parked, and sending the value read
    // before would put an older one back on the server (see PendingPaddingEdits()).
    auto choose = [&](std::optional<int>& pre, std::optional<int>& post)
    {
      std::lock_guard<std::mutex> lock(m_paddingPushMutex);
      const auto pending = dispatcharr::PendingPaddingEdits(m_paddingPush);
      sentPre = pending.first;
      sentPost = pending.second;
      if (sentPre)
        pre = sentPre->value;
      if (sentPost)
        post = sentPost->value;
    };
    auto apply = [&](bool pushOk)
    {
      std::lock_guard<std::mutex> lock(m_paddingPushMutex);
      retriesBefore = m_paddingPush.retries;
      outcome = dispatcharr::ApplyPaddingRetryResult(m_paddingPush, sentPre, sentPost, pushOk, kMaxPaddingPushRetries);
    };
    m_client.SetDvrOffsetMinutesChosen(choose, error, apply);
    if (outcome == dispatcharr::PaddingRetryOutcome::kGaveUp)
    {
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr-unofficial: giving up on updating Dispatcharr's DVR padding after %d retries: %s",
                retriesBefore + 1, error.c_str());
      kodi::QueueNotification(QUEUE_ERROR, "",
                              "Couldn't save the recording padding to Dispatcharr -- it will show Dispatcharr's own "
                              "value again after a restart.");
    }
  }
  else if (!m_dvrPaddingSynced)
  {
    m_dvrPaddingSynced = SyncDvrPaddingFromDispatcharr();
  }

  if (!m_adminStatusChecked)
    m_adminStatusChecked = SyncAdminStatusFromDispatcharr();
  if (!m_catchupFlagsChecked)
    m_catchupFlagsChecked = SyncCatchupFlagsFromDispatcharr();
  if (!m_dvrAccessChecked)
    m_dvrAccessChecked = SyncDvrAccessFromDispatcharr();

  ReassertApiKeySetting();
}

PVRDispatcharr::PVRDispatcharr(const kodi::addon::IInstanceInfo& instance)
    : CInstancePVRClient(instance), m_lastAppliedConfig(LoadConfigFromSettings()),
      m_apiKeyOwnerHost(ComputeApiKeyOwnerServer(m_lastAppliedConfig)),
      m_apiKeyOwnerUsername(m_lastAppliedConfig.username), m_client(LoadConfigFromSettings())
{
  m_channelRefreshHours = kodi::addon::GetSettingInt("channel_refresh_hours", 12);
  m_epgRefreshHours = kodi::addon::GetSettingInt("epg_refresh_hours", 4);
  m_liveTimeshiftMode = kodi::addon::GetSettingInt("live_timeshift_mode", kLiveTimeshiftOff);
  m_enableCatchupFfmpegdirectSeek = kodi::addon::GetSettingBoolean("enable_catchup_ffmpegdirect_seek", false);
  m_recordingRefreshMinutes = kodi::addon::GetSettingInt("recording_refresh_minutes", 5);
  m_recurringRuleUtcOffsetMinutes = kodi::addon::GetSettingInt("recurring_rule_utc_offset_minutes", 0);
  m_enableRealtimeUpdates = kodi::addon::GetSettingBoolean("enable_realtime_updates", false);
  m_debugLogging = kodi::addon::GetSettingBoolean("debug_logging", false);
  LoadRecordingEpgLinks();
  LoadRecurringAdoptionState();

  // Public, no-auth endpoint (see GetServerVersion()'s own comment) --
  // fetched before login so GetBackendVersion() still has a real answer
  // even if authentication below fails outright. m_backendVersion keeps
  // its "unknown" default if this fails too (e.g. Dispatcharr unreachable
  // at startup).
  {
    std::string version, versionError;
    long versionStatus = -1;
    if (m_client.GetServerVersion(version, versionError, &versionStatus))
      m_backendVersion = version;
    else
    {
      if (m_debugLogging)
      {
        kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: could not read Dispatcharr's server version: %s",
                  versionError.c_str());
      }
      // No response at all (a refusal, or a server that accepts connections and never answers): the login below
      // would only wait out another full timeout before failing the same way, and every startup sync after it
      // is gated on that failure. Start the same cooldown a failed login starts, so the instance comes up at
      // once and the background thread's deferred syncs reconnect when the server answers (found by the
      // fourteenth hardening sweep: 10 s at a 5 s timeout, about 60 s at the default, against a hung server).
      if (versionStatus == 0)
        m_client.DeferAuthenticationAfterUnresponsiveServer("Dispatcharr did not answer at startup: " + versionError);
    }
  }

  std::string error;
  // api_key_host/api_key_username (added 2026-09-27, a 45th-pass audit,
  // correcting a real, confirmed GUI-thread deadlock the previous,
  // 44th-pass version of this same fix introduced -- see
  // OnAddonSettingChanged()'s own comment on the host/username branch
  // for the full mechanism, confirmed against Kodi's own real current
  // source, not itself independently reproduced): which host/username
  // the currently-stored api_key was actually generated for. A stored
  // key surviving a host/username change untouched would otherwise let
  // recording playback keep authorizing as the *previous* account --
  // Dispatcharr's own ApiKeyAuthentication resolves a request's user
  // purely from the key itself (`User.objects.get(api_key=raw_key)`),
  // independent of whichever account this addon's own JWT login now
  // separately authenticates as for every other call -- but clearing
  // the stored key reactively, from inside the settings-change callback
  // itself, is exactly what caused the deadlock. Comparing here, at
  // construction (an ordinary, non-reentrant call path), and treating a
  // mismatch the same as "no key yet" achieves the same result safely:
  // the fresh instance a host/username change's own ADDON_STATUS_NEED_RESTART
  // already creates generates a new key scoped to whichever account is
  // now configured, the first time it constructs.
  //
  // Deliberately computed and stored *before* the EnsureAuthenticated()
  // check below, not inside its success branch (fixed 2026-09-27, a
  // 47th-pass audit, fixing a real, confirmed regression the 46th-pass
  // introduction of m_apiKeyOwnershipVerified itself had, found via a
  // project-wide review, not itself independently reproduced): this
  // comparison only reads local settings, no network call, so it
  // doesn't need login to have already succeeded -- but m_apiKeyOwnershipVerified
  // defaults to false, and if it were only ever set inside the success
  // branch, a transient initial-login failure (the same startup-timing
  // class of gap passes 43/44 already fixed elsewhere) left it false
  // even when the stored key genuinely already belonged to this account,
  // making OpenRecordedStream()'s own retry logic below needlessly
  // rotate a perfectly valid key on the very first recording open --
  // silently invalidating it for every other install/tool/script already
  // using that same account's key, exactly the disruption this whole
  // owner-tracking mechanism exists to avoid causing unnecessarily.
  std::string apiKeyHost = kodi::addon::GetSettingString("api_key_host", "");
  std::string apiKeyUsername = kodi::addon::GetSettingString("api_key_username", "");
  bool apiKeyBelongsToThisAccount = apiKeyHost == m_apiKeyOwnerHost && apiKeyUsername == m_apiKeyOwnerUsername;
  if (!apiKeyBelongsToThisAccount && apiKeyHost.empty() && apiKeyUsername.empty() && m_client.HasApiKey())
  {
    // One-time migration (added 2026-09-27, a 49th-pass audit, fixing a
    // real, confirmed bug found via a project-wide review, not itself
    // independently reproduced): api_key_host/api_key_username didn't
    // exist at all before this same pass added them, so every install
    // upgrading from a released version that predates this owner-tracking
    // mechanism (0.11.0 or earlier, confirmed against master's own current
    // addon.xml.in) already has a perfectly valid api_key but both stamps
    // reading as their empty GetSettingString() default -- indistinguishable
    // from a genuine mismatch by the plain comparison above. Without this,
    // every existing user's key would be silently rotated once on their
    // very first post-upgrade start, invalidating it for any other
    // install/tool/script already using that same account's key -- exactly
    // the disruption this whole mechanism exists to avoid causing
    // unnecessarily (see m_apiKeyOwnerHost's own comment, PVRDispatcharr.h).
    // Treat this specific combination as "already ours" instead, and stamp
    // the real values now (safe here, still inside the constructor, the
    // same reasoning PersistApiKeyIfChanged()'s own calls below rely on)
    // so a *later* genuine host/username change is still correctly caught
    // as a mismatch by the next instance's own constructor, rather than
    // this bypass silently applying forever. The one gap this leaves: a
    // user who already changed host/username on the pre-migration version
    // before upgrading won't get the wrong-account protection for that
    // change -- but that protection didn't exist there either, so nothing
    // regresses.
    apiKeyBelongsToThisAccount = true;
    kodi::addon::SetSettingString("api_key_host", m_apiKeyOwnerHost);
    kodi::addon::SetSettingString("api_key_username", m_apiKeyOwnerUsername);
  }
  m_apiKeyOwnershipVerified = apiKeyBelongsToThisAccount;
  if (!m_client.EnsureAuthenticated(error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: initial login failed: %s", error.c_str());
  }
  else if (!m_client.HasApiKey() || !apiKeyBelongsToThisAccount)
  {
    // Recording playback needs an API key (see OpenRecordingStream()/
    // ReadRecordingStream()) -- a JWT would work too, but expires after
    // 30 minutes, which is shorter than most recordings. Obtain one once
    // and persist it so it isn't looked up again on every addon restart.
    // ObtainApiKey() adopts the account's EXISTING key and only generates
    // a new one when the account has none: Dispatcharr keeps one key per
    // account and generating overwrites it, so generating unconditionally
    // here (what this used to do) silently revoked the key for every other
    // client of the account -- other Kodi installs, scripts, MCP/
    // automation tools -- just by enabling this addon with an empty
    // api_key setting (a real report against 0.11.0, docs/OPEN_ITEMS.md).
    // Routed through PersistApiKeyIfChanged() (same as the two other
    // self-heal call sites, see its own comment) rather than a plain
    // SetSettingString() here too -- a real, confirmed bug otherwise: an
    // empty m_lastAppliedConfig.apiKey (the !HasApiKey() case, i.e.
    // keyBefore is "") never gets updated to
    // match the freshly generated key, so the very next settings-dialog
    // save re-delivers api_key's own spurious-renotification (see
    // m_lastAppliedConfig's own comment) as a genuine change and returns
    // ADDON_STATUS_NEED_RESTART for an unrelated setting save -- an
    // unneeded instance restart that can tear down playback. (The
    // owner-mismatch case has a real, non-empty keyBefore instead --
    // PersistApiKeyIfChanged()'s own comparison against keyAfter still
    // persists and updates m_lastAppliedConfig.apiKey correctly either
    // way, since the two will differ regardless of whether keyBefore
    // started empty or stale.)
    std::string keyBefore = m_client.GetApiKey();
    std::string key;
    if (m_client.ObtainApiKey(key, error))
    {
      PersistApiKeyIfChanged(keyBefore);
      m_apiKeyOwnershipVerified = true;
    }
    else
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to obtain an API key: %s", error.c_str());
    }
  }

  // See SyncDvrPaddingFromDispatcharr()'s own comment. When Dispatcharr can't
  // be asked yet, fall back to whatever Kodi's own settings storage currently
  // holds, so OnAddonSettingChanged()'s guard still has *some* baseline to
  // compare a later notification against instead of spuriously treating
  // every value as "changed" until the background thread's retry succeeds.
  m_dvrPaddingSynced = SyncDvrPaddingFromDispatcharr();
  if (!m_dvrPaddingSynced)
  {
    m_lastAppliedPreOffsetMinutes = kodi::addon::GetSettingInt("recording_pre_offset_minutes", 0);
    m_lastAppliedPostOffsetMinutes = kodi::addon::GetSettingInt("recording_post_offset_minutes", 0);
  }

  // Extracted into its own method, SyncTimezoneFromDispatcharr() (fixed
  // 2026-09-27, a 47th-pass audit, fixing a real, confirmed gap found via
  // a project-wide review, not itself independently reproduced): this
  // used to run only here, once, at construction, with no retry on
  // failure -- the same startup-timing class of gap already fixed
  // elsewhere in this codebase (channels/EPG, the API key). If
  // Dispatcharr isn't reachable yet at Kodi startup, m_timezoneSynced
  // stays false and recurring_rule_timezone stays at its "manual"
  // default with a 0 offset, silently scheduling every recurring rule
  // created or edited in that whole session at the wrong real-world time
  // until a later restart happens to reach the server. The background
  // channel/EPG refresh thread now retries this once it succeeds, so a
  // transient startup failure only costs one refresh cycle, not the rest
  // of the session.
  m_timezoneSynced = SyncTimezoneFromDispatcharr();
  // Seeded *after* the sync above, from whatever it actually left the
  // setting at (added 2026-09-27, a 49th-pass audit, alongside
  // OnAddonSettingChanged()'s own new recurring_rule_timezone branch --
  // see m_lastAppliedRecurringRuleTimezone's own comment, PVRDispatcharr.h):
  // seeding from the pre-sync value instead would make the very first
  // unrelated settings-dialog save after startup look like a genuine
  // zone change against that stale baseline, spuriously triggering one
  // harmless-but-unneeded extra timer refresh -- the same
  // "PersistApiKeyIfChanged() updates its own tracker at the same time
  // it changes the setting" convention the API key self-heal above
  // already follows, just read back afterward here instead of updated
  // inline, since SyncTimezoneFromDispatcharr() itself has no reason to
  // know about this tracker.
  m_lastAppliedRecurringRuleTimezone = kodi::addon::GetSettingString("recurring_rule_timezone", "manual");

  // See SyncAdminStatusFromDispatcharr()'s own comment.
  m_adminStatusChecked = SyncAdminStatusFromDispatcharr();

  // See SyncCatchupFlagsFromDispatcharr()'s own comment.
  m_catchupFlagsChecked = SyncCatchupFlagsFromDispatcharr();

  // See SyncDvrAccessFromDispatcharr()'s own comment. Before Kodi's first
  // GetCapabilities() on the normal path, which is the point.
  m_dvrAccessChecked = SyncDvrAccessFromDispatcharr();

  StartRecordingRefreshThread();
  StartChannelEpgRefreshThread();
  if (m_enableRealtimeUpdates)
    StartRealtimeUpdateThread();
}

bool PVRDispatcharr::SleepUnlessShuttingDown(std::chrono::seconds duration)
{
  std::unique_lock<std::mutex> lock(m_shutdownMutex);
  m_shutdownCv.wait_for(lock, duration, [this]() { return m_shuttingDown.load(); });
  return !m_shuttingDown.load();
}

PVRDispatcharr::~PVRDispatcharr()
{
  {
    std::lock_guard<std::mutex> lock(m_shutdownMutex);
    m_shuttingDown = true;
  }
  m_shutdownCv.notify_all();

  // First: end every request in flight (and any later one) at once. Everything below
  // waits for threads that may be inside one, and the longest of those is a full request
  // timeout to an unresponsive Dispatcharr -- 30 s by default, user-configurable far
  // higher -- which a Kodi exit or an addon restart would otherwise sit through.
  m_client.AbortInFlightRequests();

  // Join every tracked detached thread (see m_detachedThreads's own comment)
  // before anything else -- each one captured `this`, so it must finish
  // before the rest of this destructor starts tearing that state down.
  // Moved out of the member under lock first rather than joining while
  // still holding m_detachedThreadsMutex, since join() can block (a padding push
  // is a network call; AddTimer's delayed refresh wakes at once, see
  // SleepUnlessShuttingDown()) and nothing else needs
  // that mutex once the instance is being destroyed anyway.
  std::vector<DetachedThreadEntry> detachedThreads;
  {
    std::lock_guard<std::mutex> lock(m_detachedThreadsMutex);
    detachedThreads = std::move(m_detachedThreads);
  }
  for (auto& entry : detachedThreads)
  {
    if (entry.thread.joinable())
      entry.thread.join();
  }

  StopWorkerThread(m_recordingRefreshMutex, m_stopRecordingRefreshThread, m_recordingRefreshCv,
                   m_recordingRefreshThread);
  StopWorkerThread(m_channelEpgRefreshMutex, m_stopChannelEpgRefreshThread, m_channelEpgRefreshCv,
                   m_channelEpgRefreshThread);
  StopWorkerThread(m_realtimeUpdateMutex, m_stopRealtimeUpdateThread, m_realtimeUpdateCv, m_realtimeUpdateThread);
}

namespace
{
// Runs a background task, keeping an exception out of whatever thread it runs on.
void RunBackgroundTask(const std::function<void()>& work)
{
  try
  {
    work();
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: a background task hit an exception: %s", e.what());
  }
  catch (...)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: a background task hit an unknown exception");
  }
}
} // namespace

void PVRDispatcharr::TrackDetachedThread(std::function<void()> work, bool runInlineIfThreadFails)
{
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto shared = std::make_shared<std::function<void()>>(std::move(work));
  bool started = false;
  {
    std::lock_guard<std::mutex> lock(m_detachedThreadsMutex);
    // Opportunistic prune: join and drop every earlier entry that's
    // already finished, so this list doesn't grow unbounded over a long
    // Kodi uptime with many timers added / many settings saves -- see this
    // member's own comment (PVRDispatcharr.h) for the leak this fixes.
    m_detachedThreads.erase(std::remove_if(m_detachedThreads.begin(), m_detachedThreads.end(),
                                           [](DetachedThreadEntry& entry)
                                           {
                                             if (!entry.done->load())
                                               return false;
                                             entry.thread.join();
                                             return true;
                                           }),
                            m_detachedThreads.end());
    // A thread that cannot be created throws std::system_error, and a push_back that ran out of
    // memory after the thread existed would destroy a joinable std::thread, which is
    // std::terminate -- found by the 2026-10-04 third hardening sweep, the same failure
    // RunInBoundedBatches() guards. So the room is reserved first, and the push_back after the
    // creation cannot throw.
    try
    {
      m_detachedThreads.reserve(m_detachedThreads.size() + 1);
      std::thread thread(
          [shared, done]()
          {
            RunBackgroundTask(*shared);
            done->store(true);
          });
      m_detachedThreads.push_back({std::move(thread), done});
      started = true;
    }
    catch (const std::exception& e)
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: could not start a background task: %s", e.what());
    }
  }
  // Outside the lock: the task may take a while, and nothing else needs it held.
  if (!started && runInlineIfThreadFails)
    RunBackgroundTask(*shared);
}

ADDON_STATUS PVRDispatcharr::OnAddonSettingChanged(const std::string& settingName,
                                                   const kodi::addon::CSettingValue& settingValue)
{
  if (settingName == "live_timeshift_mode")
  {
    m_liveTimeshiftMode = settingValue.GetInt();
  }
  else if (settingName == "channel_refresh_hours")
  {
    m_channelRefreshHours = settingValue.GetInt();
  }
  else if (settingName == "epg_refresh_hours")
  {
    m_epgRefreshHours = settingValue.GetInt();
  }
  else if (settingName == "enable_catchup_ffmpegdirect_seek")
  {
    m_enableCatchupFfmpegdirectSeek = settingValue.GetBoolean();
  }
  else if (settingName == "recording_refresh_minutes")
  {
    int value = settingValue.GetInt();
    // Guarded the same way every other branch here is (see
    // m_lastAppliedPreOffsetMinutes's own comment) -- fix for a real,
    // confirmed bug found via a project-wide review (a 22nd-pass
    // audit), not itself independently reproduced: this was the one
    // settings branch left unguarded against Kodi's own re-notify-every-
    // setting-on-every-save behavior (confirmed against Kodi's real
    // source: CAddonDll::SaveSettings()'s own TransferSettings()
    // re-delivers every setting, not just the one the user actually
    // touched in the dialog). Without this, ANY settings-dialog save --
    // even of an unrelated setting -- or this addon's own
    // PersistApiKeyIfChanged() self-heal (itself a SetSetting* call,
    // which triggers the identical re-delivery) restarted the refresh
    // thread's own wait_for() with the *full* interval, postponing the
    // next recording/timer refresh and RenewRecurringRules() pass by a
    // full cycle -- frequent saves could delay it indefinitely.
    if (value == m_recordingRefreshMinutes.exchange(value))
      return ADDON_STATUS_OK;
    // Wake the thread immediately rather than leaving it asleep for up to
    // the *old* interval before it notices the new one -- a bare
    // notify_all() alone doesn't do this (wait_for()'s own deadline is
    // fixed at entry; its predicate never re-reads m_recordingRefreshMinutes
    // on a plain wake), so this flag is what StartRecordingRefreshThread()'s
    // own predicate actually checks -- see its own comment.
    {
      // Set under the mutex the waiter's predicate is checked under: a flag set and a
      // notify sent with no lock held can land between the waiter checking the predicate
      // and actually going to sleep, and the wake-up is lost. Safe to take here now that
      // the refresh thread no longer holds this mutex across its network calls.
      std::lock_guard<std::mutex> flagLock(m_recordingRefreshMutex);
      m_recordingRefreshIntervalChanged = true;
    }
    m_recordingRefreshCv.notify_all();
  }
  else if (settingName == "recurring_rule_utc_offset_minutes")
  {
    // Guarded the same way as m_recordingRefreshMinutes just above, and
    // for the same reason (fixed 2026-09-27, a 49th-pass audit, fixing a
    // real, confirmed gap found via a project-wide review, not itself
    // independently reproduced): without this, every settings-dialog
    // save re-delivers this value even when it's actually unchanged
    // (Kodi's own re-notify-every-setting-on-every-save behavior), which
    // would otherwise spuriously re-trigger the timer refresh below on
    // every unrelated save. A *genuine* change here needs the same
    // refresh SyncTimezoneFromDispatcharr()'s own mid-session zone change
    // gets (see its own comment, PVRDispatcharr.h): GetTimers()'s own
    // ComputeRecurringRuleDisplayTimes() and UpdateTimer()'s own
    // ComputeRecurringRuleFields() each apply whatever offset is in
    // effect at that specific moment, so a changed offset with no
    // refresh in between leaves Kodi's cached display times stale
    // relative to it -- the next edit (even a plain enable/disable
    // toggle) would round-trip through the stale cached time and shift
    // the rule by the full old/new offset difference, exactly the bug
    // already fixed for the auto-sync path.
    int value = settingValue.GetInt();
    if (value != m_recurringRuleUtcOffsetMinutes.exchange(value))
      InvalidateAndTriggerTimerUpdate();
  }
  else if (settingName == "recurring_rule_timezone")
  {
    // Same reasoning and same fix as recurring_rule_utc_offset_minutes
    // just above -- this setting has no dedicated C++ member of its own
    // (EffectiveRecurringRuleUtcOffsetMinutes() always reads it live), so
    // m_lastAppliedRecurringRuleTimezone exists purely to guard this one
    // branch against Kodi's own re-notify-every-setting quirk, the same
    // role m_lastAppliedConfig's own fields play for their branches.
    std::string value = settingValue.GetString();
    bool changed = value != m_lastAppliedRecurringRuleTimezone;
    m_lastAppliedRecurringRuleTimezone = std::move(value);
    if (changed)
      InvalidateAndTriggerTimerUpdate();
  }
  else if (settingName == "recording_pre_offset_minutes" || settingName == "recording_post_offset_minutes")
  {
    // Global-only on Dispatcharr's side (see DispatcharrClient::
    // SetDvrOffsetMinutes()'s own comment) -- but only the offset that
    // actually changed is pushed; the other is left for
    // SetDvrOffsetMinutes()'s own fresh fetch to preserve as-is (see
    // dispatcharr::MergeDvrOffsetMinutes()'s own comment for the real
    // bug this fixes, found via a project-wide review, a 20th-pass
    // audit: reading the *other* setting from Kodi's own local copy
    // here -- synced from Dispatcharr only once, at addon construction --
    // could silently revert a genuine server-side change to it made
    // since (Dispatcharr's own web UI, a second Kodi install), even
    // though SetDvrOffsetMinutes() had just re-fetched the real,
    // current value one call earlier).
    bool isPre = settingName == "recording_pre_offset_minutes";
    int value = settingValue.GetInt();
    std::atomic<int>& lastApplied = isPre ? m_lastAppliedPreOffsetMinutes : m_lastAppliedPostOffsetMinutes;
    // Guarded the same way as the connection settings/enable_realtime_updates
    // below (see m_lastAppliedPreOffsetMinutes's own comment for the real
    // bug this fixes: without it, every settings-dialog save re-pushed
    // this regardless of whether the padding actually changed).
    bool changed = value != lastApplied.exchange(value);
    if (changed)
    {
      // Numbered here, on the thread the edit arrives on, so the order is the
      // order the user made them in, not the order the pushes happen to finish
      // (see PendingPaddingPush).
      uint64_t editSeq = 0;
      {
        std::lock_guard<std::mutex> lock(m_paddingPushMutex);
        editSeq = dispatcharr::BeginPaddingEdit(m_paddingPush, isPre, value);
      }
      // Not blocking whatever thread Kodi delivers SetSetting() on for this
      // real network round-trip (unlike every other branch here, a plain
      // in-memory write). Tracked via TrackDetachedThread() rather than
      // .detach()'d outright so the destructor can still join it -- see
      // m_detachedThreads's own comment.
      //
      // A push that fails (a network blip, Dispatcharr unreachable at the
      // exact moment of the save) is kept as pending and retried by the
      // background thread (RetryDeferredServerSyncs()). Corrected
      // 2026-09-26 (a 21st-pass audit) that the next restart is NOT a
      // natural retry: the constructor's sync-from-Dispatcharr reads the
      // server's own real value and overwrites Kodi's local setting to match
      // it, so a push that never arrived was silently reverted in Kodi's UI.
      TrackDetachedThread(
          [this, isPre, value, editSeq]()
          {
            std::string offsetError;
            // The result is applied while the client still holds its DVR-settings mutex, so results
            // are applied in the order the requests reached the server -- see SetDvrOffsetMinutes().
            bool resultApplied = false;
            auto apply = [this, isPre, value, editSeq, &resultApplied](bool pushOk)
            {
              std::lock_guard<std::mutex> lock(m_paddingPushMutex);
              dispatcharr::ApplyDirectPaddingPushResult(m_paddingPush, isPre, value, editSeq, pushOk);
              resultApplied = true;
            };
            bool ok = false;
            try
            {
              ok = isPre ? m_client.SetDvrOffsetMinutes(&value, nullptr, offsetError, apply)
                         : m_client.SetDvrOffsetMinutes(nullptr, &value, offsetError, apply);
            }
            catch (...)
            {
              // An exception out of the request (an allocation failure, a system_error) skips the
              // result callback, which would leave this edit counted as in flight for good and defer
              // the startup padding sync forever (see PendingPaddingPush::inFlightEdits). Report it
              // as a failed push, so the edit is parked for the background retry, then let it go on.
              if (!resultApplied)
                apply(false);
              throw;
            }
            if (!ok)
              kodi::Log(ADDON_LOG_ERROR,
                        "pvr.dispatcharr-unofficial: failed to update Dispatcharr's DVR padding (will retry): %s",
                        offsetError.c_str());
          },
          /*runInlineIfThreadFails=*/true);
    }
  }
  else if (settingName == "debug_logging")
  {
    m_debugLogging = settingValue.GetBoolean();
  }
  else if (settingName == "enable_realtime_updates")
  {
    // Deliberately not applied live -- would mean dynamically starting or
    // stopping m_realtimeUpdateThread outside its normal
    // constructor/destructor lifecycle, real added complexity for a
    // setting that's already documented experimental. Restart picks it up
    // the same way every setting used to work before this method existed.
    //
    // Guarded against Kodi's spurious-renotification quirk the same way
    // as the connection settings just below -- see m_lastAppliedConfig's
    // own comment for why this guard exists at all (confirmed live, not
    // theoretical: without it, saving *any* setting restarted the
    // instance every time).
    bool value = settingValue.GetBoolean();
    bool changed = value != m_enableRealtimeUpdates;
    m_enableRealtimeUpdates = value;
    return changed ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
  }
  else if (settingName == "host" || settingName == "port" || settingName == "use_https" || settingName == "username" ||
           settingName == "password" || settingName == "verify_ssl" || settingName == "timeout" ||
           settingName == "api_key")
  {
    // Baked into DispatcharrClient's Config at construction (see
    // LoadConfigFromSettings()) -- changing the connection this addon
    // talks to, or re-authenticating against it, isn't something to
    // attempt on a live instance.
    //
    // See m_lastAppliedConfig's own comment: compared against that cached
    // snapshot rather than unconditionally restarting on every
    // notification, since Kodi can (and, confirmed live, reliably does)
    // deliver a same-named, same-value notification here that has nothing
    // to do with this setting actually changing.
    bool changed = false;
    if (settingName == "host")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.host;
      m_lastAppliedConfig.host = std::move(value);
    }
    else if (settingName == "port")
    {
      int value = settingValue.GetInt();
      changed = value != m_lastAppliedConfig.port;
      m_lastAppliedConfig.port = value;
    }
    else if (settingName == "use_https")
    {
      bool value = settingValue.GetBoolean();
      changed = value != m_lastAppliedConfig.useHttps;
      m_lastAppliedConfig.useHttps = value;
    }
    else if (settingName == "username")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.username;
      m_lastAppliedConfig.username = std::move(value);
    }
    else if (settingName == "password")
    {
      std::string value = settingValue.GetString();
      changed = value != m_lastAppliedConfig.password;
      m_lastAppliedConfig.password = std::move(value);
    }
    else if (settingName == "verify_ssl")
    {
      bool value = settingValue.GetBoolean();
      changed = value != m_lastAppliedConfig.verifySsl;
      m_lastAppliedConfig.verifySsl = value;
    }
    else if (settingName == "timeout")
    {
      int value = settingValue.GetInt();
      changed = value != m_lastAppliedConfig.timeoutSeconds;
      m_lastAppliedConfig.timeoutSeconds = value;
    }
    else if (settingName == "api_key")
    {
      std::string value = settingValue.GetString();
      std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
      // Kodi re-delivering the key it still stored after our own write was swallowed by the settings dialog is
      // not an edit: restarting for it tore down playback for no real change, and taking it over would put back
      // a key the server has already replaced. The retry (ReassertApiKeySetting()) fixes the stored value.
      switch (dispatcharr::ClassifyApiKeyDelivery(value, m_lastAppliedConfig.apiKey, m_apiKeyStoredAfterDroppedWrite))
      {
      case dispatcharr::ApiKeyDelivery::kUnchanged:
        changed = false;
        break;
      case dispatcharr::ApiKeyDelivery::kStaleRedelivery:
        changed = false;
        kodi::Log(ADDON_LOG_INFO,
                  "pvr.dispatcharr-unofficial: Kodi re-delivered the previous API key (its stored copy was not "
                  "updated while the settings dialog was open); keeping the current key");
        break;
      case dispatcharr::ApiKeyDelivery::kChanged:
        changed = true;
        m_lastAppliedConfig.apiKey = std::move(value);
        m_apiKeyStoredAfterDroppedWrite.clear(); // an entered key wins over any outstanding write
        break;
      }
    }
    // A stored api_key surviving a host/username change untouched (so the
    // fresh instance this restart creates would otherwise keep using the
    // *previous* account/server's key for recording playback) is real and
    // still fixed -- see PVRDispatcharr()'s own constructor comment on
    // api_key_host/api_key_username -- but NOT handled here anymore.
    // Real, confirmed GUI-thread deadlock this used to cause (found via a
    // project-wide review, a 45th-pass audit, confirmed against Kodi's
    // own real current source, not itself independently reproduced): an
    // earlier version of this fix called `kodi::addon::SetSettingString(
    // "api_key", "")` directly in this branch. `CAddonDispatcharr::SetSetting()`
    // (addon.cpp) holds `m_instancesMutex` for its *entire* call into this
    // function -- and `SetSettingString()` (`Interface_Base::set_setting_string`)
    // unconditionally calls `CAddonDll::SaveSettings()`, which -- once the
    // settings dialog that triggered this change has already closed (it's
    // a modal `dialog->Open()` that returns only after Close(),
    // `CGUIDialogAddonSettings::ShowForSingleInstance()`) -- re-enters
    // `TransferSettings()` and re-delivers *every* setting again,
    // including `host`/`username` themselves, back into
    // `CAddonDispatcharr::SetSetting()` on the very same call stack. That
    // tries to re-lock the still-held `m_instancesMutex` on the same
    // thread -- undefined behavior for a plain (non-recursive)
    // `std::mutex`, and a real, permanent hang in practice on every
    // mainstream `std::mutex` implementation (glibc's default
    // `PTHREAD_MUTEX_NORMAL` explicitly documents self-relock as
    // deadlocking, not erroring). `OnAddonSettingChanged()` must never
    // call any `SetSetting*()` variant, directly or indirectly, since
    // Kodi's own settings-transfer machinery can invoke it from exactly
    // this non-reentrant context.
    return changed ? ADDON_STATUS_NEED_RESTART : ADDON_STATUS_OK;
  }
  return ADDON_STATUS_OK;
}

void PVRDispatcharr::StartRecordingRefreshThread()
{
  try
  {
    m_recordingRefreshThread = std::thread(
        [this]()
        {
          RunGuardedWorker("recording refresh", m_stopRecordingRefreshThread,
                           [this]()
                           {
                             // Held only while waiting, never across the refresh work below: that work makes
                             // network calls that can take a full request timeout each, and the destructor
                             // and the settings thread both need this mutex briefly to set a flag and wake
                             // this thread (docs/CLOSED_ITEMS.md, "Kodi shutdown / NEED_RESTART teardown can block
                             // for minutes" and "notify_all() without holding the condition variable's mutex").
                             std::unique_lock<std::mutex> lock(m_recordingRefreshMutex);
                             // Real, confirmed bug found via a project-wide review (a 44th-pass
                             // audit), not itself independently reproduced: this loop used to
                             // wait_for() *before* ever doing any real work, so the very first
                             // recurring-rule renewal/timer/recording refresh didn't happen
                             // until a full recording_refresh_minutes (default 5, up to 60 per
                             // settings.xml) had already elapsed since this thread started --
                             // unlike StartChannelEpgRefreshThread()'s own loop, which always
                             // did its first channels/EPG attempt immediately. firstIteration
                             // skips the wait exactly once, so the first pass through the loop
                             // runs the refresh work right away; every later iteration is
                             // completely unchanged from before, including the
                             // m_recordingRefreshIntervalChanged skip-without-refreshing
                             // semantics its own comment below documents.
                             bool firstIteration = true;
                             while (!m_stopRecordingRefreshThread)
                             {
                               if (!firstIteration)
                               {
                                 // The predicate also checks m_recordingRefreshIntervalChanged
                                 // (see its own comment) -- wait_for()'s own deadline is fixed
                                 // the moment it's entered, so a bare notify_all() alone would
                                 // just wake the thread and have it go right back to sleep
                                 // until that same stale deadline; this flag is what actually
                                 // lets a changed recording_refresh_minutes take effect on the
                                 // very next wake, not just at the end of whatever interval was
                                 // already in progress when it changed.
                                 m_recordingRefreshCv.wait_for(lock, std::chrono::minutes(m_recordingRefreshMinutes),
                                                               [this]() {
                                                                 return m_stopRecordingRefreshThread.load() ||
                                                                        m_recordingRefreshIntervalChanged.load();
                                                               });
                                 if (m_stopRecordingRefreshThread)
                                   break;
                                 if (m_recordingRefreshIntervalChanged.exchange(false))
                                   continue; // re-enter wait_for() immediately with the now-current interval
                               }
                               firstIteration = false;
                               lock.unlock();
                               RenewRecurringRules();
                               // Timers before recordings -- see HandleRealtimeUpdateMessage()'s
                               // own comment on why the order matters here, not just at
                               // AddTimer()'s already-established site.
                               InvalidateAndTriggerTimerUpdate();
                               InvalidateAndTriggerRecordingUpdate();
                               lock.lock();
                             }
                           });
        });
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: could not start the RecordingRefresh thread: %s", e.what());
  }
}

void PVRDispatcharr::RenewRecurringRules()
{
  std::vector<RecurringRule> rules;
  std::string error;
  if (!m_client.GetRecurringRules(rules, error))
    return;

  std::vector<Recording> recordings;
  bool haveRecordings = m_client.GetRecordings(recordings, error);

  time_t now = time(nullptr);
  // The one-time tagging of rules that predate ownership -- see ManagedRecurringRule.h.
  AdoptExistingRecurringRulesOnce(rules, recordings, haveRecordings, now);

  // The per-rule renewal decision itself lives in
  // dispatcharr::ShouldRenewRecurringRule() (RecurringRuleRenewal.{h,cpp})
  // so it's unit-testable standalone -- see that function's own comment. It renews
  // only rules the addon owns, and revives one of them that has run past its end date.
  for (const auto& rule : rules)
  {
    if (!ShouldRenewRecurringRule(rule, recordings, haveRecordings, now, kRecurringRuleWindowDays,
                                  kRecurringRuleRenewalSafetyMarginSeconds))
      continue; // not ours, disabled, still inside its window, or an active/imminent occurrence -- try again next cycle

    const bool revival = rule.endDate <= now;
    time_t newEndDate = now + static_cast<time_t>(kRecurringRuleWindowDays) * 86400;
    std::string extendError;
    if (!m_client.ExtendRecurringRuleEndDate(rule.id, newEndDate, extendError))
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to renew recurring rule %d: %s", rule.id,
                extendError.c_str());
    }
    else if (revival)
    {
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: recurring rule %d had run past its end date; it is one of this addon's "
                "own, so it was revived with a new %d-day window",
                rule.id, kRecurringRuleWindowDays);
    }
    else if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: renewed recurring rule %d end_date forward", rule.id);
    }
  }
}

void PVRDispatcharr::AdoptExistingRecurringRulesOnce(std::vector<RecurringRule>& rules,
                                                     const std::vector<Recording>& recordings, bool haveRecordings,
                                                     time_t now)
{
  if (m_recurringAdoption.known && m_recurringAdoption.done)
    return;

  if (!m_recurringAdoption.known)
  {
    m_recurringAdoption = dispatcharr::EvaluateInitialAdoption(rules, now);
    SaveRecurringAdoptionState();
    if (!m_recurringAdoption.done)
    {
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: marking %zu existing recurring rule(s) as managed by this addon (they "
                "get a \"[Kodi]\" tag and are renewed automatically from now on)",
                m_recurringAdoption.pending.size());
    }
  }
  if (m_recurringAdoption.done)
    return;

  // Which pending rules to tag this cycle and which to leave alone (a recording
  // running or about to start) -- dispatcharr::PlanRecurringRuleAdoption(), tested.
  const dispatcharr::RecurringRuleAdoptionPlan plan = dispatcharr::PlanRecurringRuleAdoption(
      m_recurringAdoption.pending, rules, recordings, haveRecordings, now, kRecurringRuleRenewalSafetyMarginSeconds);
  std::vector<int> stillPending = plan.deferred;
  size_t tagged = 0;
  for (int id : plan.toTag)
  {
    // A rule this session has already failed to tag too many times in a row is left alone until
    // the next start (see kMaxAdoptionRetriesPerSession).
    const auto failed = m_adoptionFailures.find(id);
    if (failed != m_adoptionFailures.end() && failed->second >= dispatcharr::kMaxAdoptionRetriesPerSession)
    {
      stillPending.push_back(id);
      continue;
    }
    auto it = std::find_if(rules.begin(), rules.end(), [id](const RecurringRule& r) { return r.id == id; });
    RecurringRuleEditPatch patch;
    patch.name = AddManagedRuleMarker(it->name);
    // A rule with no end date cannot be saved without one -- see RecurringRuleEdit.h.
    time_t fallbackEnd = ComputeEndDateForOpenEndedRuleEdit(it->endDate, it->startDate, now, kRecurringRuleWindowDays);
    if (fallbackEnd > 0)
      patch.endDate = fallbackEnd;
    std::string patchError;
    long patchStatus = 0;
    if (m_client.UpdateRecurringRule(id, patch, patchError, &patchStatus))
    {
      it->name = *patch.name;
      ++tagged;
      m_adoptionFailures.erase(id);
    }
    else if (dispatcharr::ClassifyAdoptionPatchFailure(patchStatus) == dispatcharr::AdoptionFailureKind::kPermanent)
    {
      // Will fail the same way next time (a validation error, no permission, the rule gone):
      // skipped for good rather than retried every cycle with an ERROR line each time.
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr-unofficial: not marking recurring rule %d as managed, the server refused it (HTTP "
                "%ld): %s -- this rule will not be renewed automatically",
                id, patchStatus, patchError.c_str());
      m_adoptionFailures.erase(id);
    }
    else
    {
      const int failures = ++m_adoptionFailures[id];
      if (failures >= dispatcharr::kMaxAdoptionRetriesPerSession)
      {
        // Stays pending in the saved state (the next start tries again) but is no longer
        // retried this session, so a server that errors on every PATCH is not hit each cycle.
        kodi::Log(
            ADDON_LOG_ERROR,
            "pvr.dispatcharr-unofficial: giving up marking recurring rule %d as managed for this session after %d "
            "failed attempts: %s",
            id, failures, patchError.c_str());
      }
      else
      {
        kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: could not mark recurring rule %d as managed: %s", id,
                  patchError.c_str());
      }
      stillPending.push_back(id);
    }
  }

  m_recurringAdoption.pending = stillPending;
  m_recurringAdoption.done = stillPending.empty();
  SaveRecurringAdoptionState();
  if (tagged > 0)
  {
    kodi::QueueNotification(QUEUE_INFO, "",
                            "Existing recurring rules were marked \"[Kodi]\" and will now be renewed automatically by "
                            "this addon.");
    InvalidateAndTriggerTimerUpdate();
  }
}

// Writes `text` to `path` so a reader never sees a half-written file: it goes to
// "<path>.tmp" first and is renamed over the target. Both state files this is used
// for (recurring_rule_adoption.json, recording_epg_links.json) were written by
// truncating the real file and then writing it, so a power cut or a killed Kodi
// in between left a truncated file the loader then discards whole -- for the
// guide links that loses programmes the guide has already dropped, which cannot
// be rebuilt (found by the 2026-10-04 hardening sweep). Where the rename over an
// existing file is refused, the old file is removed first and the rename retried;
// if that also fails the text is written straight to the target, as before, so a
// save is never lost for want of a rename.
static bool WriteStateFileAtomically(const std::string& path, const std::string& text)
{
  const std::string tmpPath = path + ".tmp";
  bool written = false;
  {
    kodi::vfs::CFile file;
    if (file.OpenFileForWrite(tmpPath, true))
    {
      written = file.Write(text.data(), text.size()) == static_cast<ssize_t>(text.size());
      file.Close();
    }
  }
  if (written)
  {
    if (kodi::vfs::RenameFile(tmpPath, path))
      return true;
    kodi::vfs::DeleteFile(path);
    if (kodi::vfs::RenameFile(tmpPath, path))
      return true;
  }
  kodi::vfs::DeleteFile(tmpPath);
  kodi::vfs::CFile file;
  if (!file.OpenFileForWrite(path, true))
    return false;
  const bool direct = file.Write(text.data(), text.size()) == static_cast<ssize_t>(text.size());
  file.Close();
  return direct;
}

void PVRDispatcharr::LoadRecurringAdoptionState()
{
  const std::string path = kodi::addon::GetUserPath("recurring_rule_adoption.json");
  if (!kodi::vfs::FileExists(path, false))
    return;
  kodi::vfs::CFile file;
  if (!file.OpenFile(path, 0))
    return;
  std::string text;
  char buffer[4096];
  ssize_t got;
  while ((got = file.Read(buffer, sizeof(buffer))) > 0)
    text.append(buffer, static_cast<std::size_t>(got));
  file.Close();
  dispatcharr::RecurringRuleAdoptionState loaded;
  if (dispatcharr::ParseAdoptionState(text, loaded))
    m_recurringAdoption = loaded;
}

void PVRDispatcharr::SaveRecurringAdoptionState()
{
  const std::string dir = kodi::addon::GetUserPath();
  if (!kodi::vfs::DirectoryExists(dir))
    kodi::vfs::CreateDirectory(dir);
  const std::string text = dispatcharr::SerializeAdoptionState(m_recurringAdoption);
  if (!WriteStateFileAtomically(kodi::addon::GetUserPath("recurring_rule_adoption.json"), text))
    kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr-unofficial: could not save the recurring-rule marking state");
}

void PVRDispatcharr::StartChannelEpgRefreshThread()
{
  try
  {
    m_channelEpgRefreshThread = std::thread(
        [this]()
        {
          RunGuardedWorker(
              "channel and guide refresh", m_stopChannelEpgRefreshThread,
              [this]()
              {
                while (true)
                {
                  // Retried here once per cycle for as long as it hasn't
                  // succeeded yet (added 2026-09-27, a 47th-pass audit) -- see
                  // m_timezoneSynced's own comment (PVRDispatcharr.h) for the
                  // real, confirmed startup-timing gap this closes. Cheap once
                  // synced: SyncTimezoneFromDispatcharr() itself isn't called at
                  // all once this is true.
                  //
                  // A change detected *here* (as opposed to the constructor's own
                  // first call, which ignores this) now also triggers a timer
                  // refresh (added 2026-09-27, a 48th-pass audit, fixing a real,
                  // confirmed scheduling bug this retry itself made reachable,
                  // found via a project-wide review, not itself independently
                  // reproduced -- see SyncTimezoneFromDispatcharr()'s own
                  // zoneChangedOut comment, PVRDispatcharr.h, for the full
                  // mechanism): unlike the constructor's call, this one can run
                  // well after Kodi has already fetched and cached timers using
                  // the *old* offset, and nothing previously told Kodi to re-fetch
                  // them with the new one before an edit could read a stale
                  // cached time back and silently shift the rule.
                  if (!m_timezoneSynced)
                  {
                    bool zoneChanged = false;
                    m_timezoneSynced = SyncTimezoneFromDispatcharr(&zoneChanged);
                    if (zoneChanged)
                      InvalidateAndTriggerTimerUpdate();
                  }

                  RetryDeferredServerSyncs();

                  // Checked (and, if stale, fetched) immediately on every wake,
                  // starting with the very first one -- this is what actually
                  // pre-warms the cache ahead of Kodi's own first GetChannels() call,
                  // rather than only reacting after channel_refresh_hours/
                  // epg_refresh_hours has already elapsed once.
                  if (EnsureChannelsLoaded())
                  {
                    // EnsureChannelsLoaded() itself now fires TriggerChannelGroupsUpdate()/
                    // TriggerChannelUpdate() on every successful commit, regardless
                    // of which thread caused it -- see its own comment.
                    if (m_debugLogging)
                      kodi::Log(ADDON_LOG_DEBUG,
                                "pvr.dispatcharr-unofficial: background thread refreshed channels/groups");
                  }
                  if (EnsureEpgLoaded())
                  {
                    if (m_debugLogging)
                      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: background thread refreshed EPG");
                    // No bulk/whole-guide equivalent exists in Kodi's PVR API --
                    // TriggerEpgUpdate() is per-channel only (confirmed in
                    // kodi-dev-kit's PVR.h). Channel/EPG refreshes are already coarse
                    // (hours, not minutes), so iterating every known channel here
                    // isn't a hot path.
                    std::vector<int> channelUids;
                    {
                      std::lock_guard<std::mutex> lock(m_dataMutex);
                      channelUids.reserve(m_channels.size());
                      for (const auto& ch : m_channels)
                        channelUids.push_back(ch.id);
                    }
                    for (int uid : channelUids)
                      TriggerEpgUpdate(static_cast<unsigned int>(uid));
                  }

                  // See dispatcharr::ShouldUseShortChannelEpgRefreshWait()'s own
                  // comment (Staleness.h): a fixed 10-minute wait regardless of
                  // whether channels/EPG have EVER successfully loaded left a
                  // first-ever startup failure unretried for the full interval,
                  // even though the failure-retry gate inside
                  // EnsureChannelsLoaded()/EnsureEpgLoaded() themselves would
                  // otherwise allow a retry within a minute. Read under
                  // m_dataMutex -- same data-race reasoning as
                  // EnsureChannelsLoaded()'s own top-of-function comment.
                  int waitMinutes = kChannelEpgRefreshCheckMinutes;
                  {
                    std::lock_guard<std::mutex> dataLock(m_dataMutex);
                    // A guide fetch scheduled by a channel renumbering is due in minutes, not hours.
                    if (dispatcharr::ShouldUseShortChannelEpgRefreshWait(m_channelsLoadedAt, m_epgLoadedAt) ||
                        dispatcharr::IsGuideRefetchPending(m_epgRefetchDueAt))
                      waitMinutes = kChannelEpgFailureRetryMinutes;
                  }
                  std::unique_lock<std::mutex> lock(m_channelEpgRefreshMutex);
                  // Also woken by RequestGuideFetchIfWanted(): a Kodi callback that found the
                  // guide stale, which this thread fetches on its behalf.
                  m_channelEpgRefreshCv.wait_for(
                      lock, std::chrono::minutes(waitMinutes),
                      [this]() { return m_stopChannelEpgRefreshThread.load() || m_channelEpgRefreshRequested.load(); });
                  if (m_stopChannelEpgRefreshThread)
                    break;
                  // Cleared before the next cycle's work, so a request made during that work
                  // still earns one more pass (a cheap one when nothing is left to do).
                  m_channelEpgRefreshRequested = false;
                }
              });
        });
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: could not start the ChannelEpgRefresh thread: %s",
              e.what());
  }
}

void PVRDispatcharr::RequestGuideFetchIfWanted()
{
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!dispatcharr::ShouldAttemptGuideFetch(m_epgLoadedAt, m_epgRefetchDueAt, m_epgLastFailedAt,
                                              m_epgDurableFailureCount, std::chrono::steady_clock::now(),
                                              std::chrono::hours(m_epgRefreshHours),
                                              std::chrono::minutes(kChannelEpgFailureRetryMinutes)))
      return;
  }
  {
    // Under the mutex the thread's wait predicate is checked under, so the wake-up
    // cannot be lost between that check and the thread going to sleep.
    std::lock_guard<std::mutex> lock(m_channelEpgRefreshMutex);
    m_channelEpgRefreshRequested = true;
  }
  m_channelEpgRefreshCv.notify_all();
}

void PVRDispatcharr::HandleRefreshEvent(dispatcharr::RefreshEventKind kind)
{
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (kind == dispatcharr::RefreshEventKind::kGuideParsed)
    {
      // Not at once: Dispatcharr's own cache of the exported guide outlives the refresh, and an earlier fetch would
      // download the old guide again (the same follow-up a channel renumbering schedules). The background thread
      // runs it when due and, while one is pending, checks every minute.
      m_epgRefetchDueAt = dispatcharr::ScheduleGuideRefetchForEvent(m_epgRefetchDueAt, now);
    }
    else
    {
      // Aged to just-stale, never to the zero "never loaded" sentinel (see dispatcharr::MarkStaleForWake()): the list
      // in hand is still good to show while the background thread fetches the new one.
      m_channelsLoadedAt =
          dispatcharr::MarkStaleForWake(m_channelsLoadedAt, now, std::chrono::hours(m_channelRefreshHours));
      ++m_channelsGeneration;
    }
  }
  if (m_debugLogging)
    kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: realtime update: a Dispatcharr %s refresh finished; %s",
              kind == dispatcharr::RefreshEventKind::kGuideParsed ? "guide (EPG)" : "channel (M3U)",
              kind == dispatcharr::RefreshEventKind::kGuideParsed ? "fetching the guide again shortly"
                                                                  : "fetching the channel list again");
  {
    // Under the mutex the thread's wait predicate is checked under (see RequestGuideFetchIfWanted()).
    std::lock_guard<std::mutex> lock(m_channelEpgRefreshMutex);
    m_channelEpgRefreshRequested = true;
  }
  m_channelEpgRefreshCv.notify_all();
}

void PVRDispatcharr::HandleRealtimeUpdateMessage(const std::string& message)
{
  // The wire-shape/relevant-event-type parsing itself lives in
  // dispatcharr::ParseRelevantRealtimeUpdateEventType() (RealtimeUpdateParser.{h,cpp})
  // so it's unit-testable standalone -- see that function's own comment.
  // An EPG or M3U refresh that has just finished on the server is its own kind of event: it says Dispatcharr's guide or
  // channel list differs from the copy this addon holds, which would otherwise be noticed at the next polling window
  // (hours). See dispatcharr::ClassifyRefreshEvent() for what the real sequences look like.
  const dispatcharr::RefreshEventKind refreshKind = dispatcharr::ClassifyRefreshEvent(message);
  if (refreshKind != dispatcharr::RefreshEventKind::kNone)
  {
    HandleRefreshEvent(refreshKind);
    return;
  }
  std::string eventType = ParseRelevantRealtimeUpdateEventType(message);
  if (eventType.empty())
    return;

  if (m_debugLogging)
    kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: realtime update received: %s", eventType.c_str());
  // Timers *before* recordings, matching AddTimer()'s own already-
  // established order (fixed 2026-09-27, a 41st-pass audit, fixing a
  // real latch bug found via a project-wide review, confirmed against
  // Kodi's own real current SDK source, not itself independently
  // reproduced): CPVRRecording::IsInProgress() (PVRRecording.cpp) only
  // ever re-checks GetRecordingTimer() while m_bInProgress is still true
  // -- once it finds no matching timer, it latches to false permanently,
  // never re-checking again no matter what changes afterward. A brand
  // new CPVRRecording starts with m_bInProgress = true (Reset()), so a
  // newly-started recording whose corresponding RECORDING-state timer
  // hasn't reached Kodi's own Timers list *yet* latches to "not in
  // progress" the moment anything calls IsInProgress() on it in that
  // gap -- and something does, ambiently, about once a second:
  // CPVRManager::Process()'s own main loop calls
  // TriggerRecordingsSizeInProgressUpdate() (which calls IsInProgress()
  // on every recording) on every iteration once any client declares
  // SetSupportsRecordingSize(true), which this addon does. Firing the
  // recordings update first (the order this code used before this fix)
  // left exactly that gap open between the two HTTP round-trips this
  // pair of Trigger*Update() calls schedules; firing timers first closes
  // it, at the (already-accepted-elsewhere -- see AddTimer()'s own
  // comment) small cost of the two updates not landing atomically.
  InvalidateAndTriggerTimerUpdate();
  InvalidateAndTriggerRecordingUpdate();
}

void PVRDispatcharr::StartRealtimeUpdateThread()
{
  try
  {
    m_realtimeUpdateThread = std::thread(
        [this]()
        {
          RunGuardedWorker(
              "realtime update", m_stopRealtimeUpdateThread,
              [this]()
              {
                constexpr int kMessageReadTimeoutSeconds = 5; // bounds how quickly a stop request is noticed
                int backoffSeconds = dispatcharr::kInitialReconnectBackoffSeconds;
                // True from the moment a session ends (or a connect attempt fails)
                // until the next successful connect: a recording finishing or a
                // timer edited elsewhere while nothing was listening is never
                // pushed again, so without a refresh on reconnect it waited for the
                // next periodic one (recording_refresh_minutes) to show up. Not
                // armed before the very first attempt -- Kodi has just loaded
                // timers and recordings itself then.
                bool resyncOnConnect = false;

                auto shouldStop = [this]()
                {
                  std::lock_guard<std::mutex> lock(m_realtimeUpdateMutex);
                  return m_stopRealtimeUpdateThread.load();
                };

                while (!shouldStop())
                {
                  // m_client's own (construction-time) connection settings, not a
                  // fresh LoadConfigFromSettings() read (fixed 2026-09-27, a
                  // 47th-pass audit -- see DispatcharrClient::GetConnectionSettings()'s
                  // own comment for the real, confirmed bug this fixes): a live
                  // settings read here could reconnect to a *different* host
                  // using the *previous* server's own JWT below, for however long
                  // Kodi's blocking NEED_RESTART dialog leaves this old instance
                  // still running after a host/username change.
                  std::string host;
                  int port = 0;
                  bool useHttps = false;
                  bool verifySsl = true;
                  int timeoutSeconds = 30;
                  m_client.GetConnectionSettings(host, port, useHttps, verifySsl, timeoutSeconds);
                  std::string token, error;
                  if (m_client.GetAccessToken(token, error))
                  {
                    WebSocketClient ws;
                    ws.SetStopCheck([&shouldStop]() { return shouldStop(); });
                    std::string pathAndQuery = "/ws/?token=" + token;
                    if (ws.Connect(host, port, useHttps, pathAndQuery, verifySsl, timeoutSeconds, error))
                    {
                      // Fix for a real, confirmed bug found via a project-wide
                      // review (a 26th-pass audit), not itself independently
                      // reproduced: a stale wake nudge left over from *before*
                      // this connection was established could otherwise tear
                      // down this brand-new, healthy session for no reason.
                      // Concretely: system suspend kills the previous session
                      // outright, so the outer loop's own reconnect-on-failure
                      // path (not OnSystemWake()'s nudge mechanism at all) can
                      // already have re-established a fresh connection here by
                      // the time Kodi actually delivers OnSystemWake() for that
                      // same resume -- the inner loop's own wake branch below has
                      // no way to tell "this connection predates the wake, still
                      // unproven" apart from "this connection postdates it, the
                      // flag is just stale," and without this reset it treated
                      // both the same, tearing down an already-fine session and
                      // paying a full reconnect (plus, since the resulting
                      // session then fails WasSessionHealthy()'s own 30s minimum,
                      // a doubled backoff wait) for nothing.
                      m_wakeRealtimeUpdateThread = false;
                      if (m_debugLogging)
                        kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: realtime updates: connected");
                      auto connectedAt = std::chrono::steady_clock::now();
                      if (resyncOnConnect)
                      {
                        resyncOnConnect = false;
                        kodi::Log(ADDON_LOG_DEBUG,
                                  "pvr.dispatcharr-unofficial: realtime updates reconnected -- refreshing timers and "
                                  "recordings for anything missed while disconnected");
                        // Timers before recordings -- see HandleRealtimeUpdateMessage()'s own comment.
                        InvalidateAndTriggerTimerUpdate();
                        InvalidateAndTriggerRecordingUpdate();
                      }

                      while (!shouldStop())
                      {
                        std::string message;
                        int result = ws.ReceiveTextMessage(message, kMessageReadTimeoutSeconds, error);
                        if (result == 1)
                        {
                          HandleRealtimeUpdateMessage(message);
                          // A live message just proved this session survived
                          // whatever prompted a wake nudge -- consume it without
                          // acting on it (see the result==0 branch below for why
                          // it matters when nothing was actually proven).
                          m_wakeRealtimeUpdateThread = false;
                        }
                        else if (result < 0)
                        {
                          if (m_debugLogging)
                            kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: realtime updates: %s",
                                      error.c_str());
                          break; // reconnect
                        }
                        else if (m_wakeRealtimeUpdateThread.exchange(false))
                        {
                          // A system wake happened while this session was
                          // already connected, and nothing has arrived since to
                          // prove it's still actually alive (a plain read
                          // timeout tells us nothing either way). A real gap
                          // found via a project-wide review, not reproduced
                          // live: OnSystemWake() only ever cut short the OUTER
                          // reconnect wait below, never anything here -- so a
                          // session that looked "connected" going into suspend
                          // sat unchecked until TCP keepalive eventually noticed
                          // (added the prior pass specifically for a dead
                          // session, up to ~195s on Linux), completely bypassing
                          // the whole point of an immediate wake nudge for
                          // exactly the case a suspend/resume is most likely to
                          // produce. Force a reconnect right away instead of
                          // waiting that out.
                          if (m_debugLogging)
                            kodi::Log(
                                ADDON_LOG_INFO,
                                "pvr.dispatcharr-unofficial: realtime updates: forcing reconnect after system wake");
                          break; // reconnect
                        }
                        // result == 0 with no pending wake nudge: just a read
                        // timeout with nothing new -- loop and re-check
                        // shouldStop().
                      }
                      ws.Close();
                      resyncOnConnect = true;

                      // Only reset backoff to the floor once this session has
                      // proven itself healthy for a real minimum stretch, rather
                      // than the instant Connect() itself succeeded -- see
                      // dispatcharr::WasSessionHealthy()'s own comment
                      // (AuthBackoff.h) for the reconnect-storm this avoids.
                      if (dispatcharr::WasSessionHealthy(std::chrono::steady_clock::now() - connectedAt))
                        backoffSeconds = dispatcharr::kInitialReconnectBackoffSeconds;
                    }
                    else
                    {
                      resyncOnConnect = true;
                      if (m_debugLogging)
                      {
                        kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: realtime updates: connect failed: %s",
                                  error.c_str());
                      }
                    }
                  }
                  else
                  {
                    resyncOnConnect = true;
                    if (m_debugLogging)
                    {
                      kodi::Log(ADDON_LOG_INFO,
                                "pvr.dispatcharr-unofficial: realtime updates: could not get an access token: %s",
                                error.c_str());
                    }
                  }

                  if (shouldStop())
                    break;

                  std::unique_lock<std::mutex> lock(m_realtimeUpdateMutex);
                  // Also wakes on m_wakeRealtimeUpdateThread (see OnSystemWake()) --
                  // wait_for()'s return tells the two apart from a natural timeout:
                  // true means the predicate was satisfied early (stop or wake), false
                  // means the full backoffSeconds actually elapsed.
                  bool predicateSatisfied = m_realtimeUpdateCv.wait_for(
                      lock, std::chrono::seconds(backoffSeconds),
                      [this]() { return m_stopRealtimeUpdateThread.load() || m_wakeRealtimeUpdateThread.load(); });
                  if (m_stopRealtimeUpdateThread)
                    break;
                  // The decision itself lives in
                  // dispatcharr::ComputeNextReconnectBackoffSeconds() (AuthBackoff.h,
                  // alongside ComputeLoginBackoffSeconds()) so it's unit-testable
                  // standalone -- see that function's own comment.
                  bool wokenByNudge = predicateSatisfied && m_wakeRealtimeUpdateThread.exchange(false);
                  backoffSeconds = dispatcharr::ComputeNextReconnectBackoffSeconds(backoffSeconds, wokenByNudge);
                }
              });
        });
  }
  catch (const std::exception& e)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: could not start the RealtimeUpdate thread: %s", e.what());
  }
}

// ---------------------------------------------------------------------
// General
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
  capabilities.SetSupportsTV(true);
  capabilities.SetSupportsRadio(false);
  capabilities.SetSupportsEPG(true);
  capabilities.SetSupportsChannelGroups(true);
  m_capabilitiesRead = true;
  // Recording, deleting and renaming need DVR "manage" access; a view-only account
  // would be offered all of them and fail every time -- see
  // SyncDvrAccessFromDispatcharr(). Playback and the recordings list stay.
  const bool canManageDvr = m_dvrManageAllowed;
  capabilities.SetSupportsRecordings(true);
  capabilities.SetSupportsRecordingsDelete(canManageDvr);
  // Backed by Dispatcharr's real POST .../recordings/{id}/update-metadata/
  // (confirmed against its source, not its OpenAPI schema -- see
  // DispatcharrClient::RenameRecording()'s own comment). Works for both
  // completed and in-progress recordings alike, since it's a plain
  // custom_properties write independent of either playback path.
  capabilities.SetSupportsRecordingsRename(canManageDvr);
  // Sourced from custom_properties.bytes_written, already present in the
  // same GetRecordings() payload -- see Recording::bytesWritten's own
  // comment for why it reads 0 while a recording is still in progress.
  capabilities.SetSupportsRecordingSize(true);
  capabilities.SetSupportsTimers(canManageDvr);
  capabilities.SetSupportsRecordingPlayCount(false);
  // Backed by this addon's companion recording_edl Dispatcharr plugin (see
  // GetRecordingEdl() below) -- safe to declare unconditionally the same
  // way SetHandlesInputStream() is: a recording with no comskip markers
  // (the common case) or with the plugin not installed just gets an empty
  // EDL back, not an error, so there's no reason to gate this on a
  // setting the way, say, enable_catchup_ffmpegdirect_seek gates a
  // genuinely optional dependency.
  capabilities.SetSupportsRecordingEdl(true);
  capabilities.SetSupportsDescrambleInfo(false);
  // For server-side timeshift's OpenLiveStream()/ReadLiveStream()/
  // SeekLiveStream() (see GetChannelStreamProperties()) and in-progress
  // recording playback's equivalents -- Kodi only actually calls these
  // when GetChannelStreamProperties()/GetRecordingStreamProperties() left
  // STREAMURL unset. Safe to declare unconditionally regardless of
  // live_timeshift_mode: with it set to Off, GetChannelStreamProperties()
  // sets STREAMURL instead, and Kodi simply never calls these for a live
  // channel in that case.
  capabilities.SetHandlesInputStream(true);
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetBackendName(std::string& name)
{
  name = "Dispatcharr";
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetBackendVersion(std::string& version)
{
  // Real Dispatcharr server version, fetched once at startup -- see
  // m_backendVersion's own comment and GetServerVersion().
  version = m_backendVersion;
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetConnectionString(std::string& connection)
{
  // FormatHostForUrl() brackets an IPv6 literal, so "[::1]:9191" reads as
  // host and port instead of "::1:9191".
  connection = dispatcharr::FormatHostForUrl(kodi::addon::GetSettingString("host", "127.0.0.1")) + ":" +
               std::to_string(kodi::addon::GetSettingInt("port", 9191));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::OnSystemWake()
{
  // See m_wakeRealtimeUpdateThread's own comment. A no-op if the thread
  // isn't running (enable_realtime_updates off) or isn't currently in its
  // reconnect wait (e.g. already mid-connect) -- the flag just gets
  // checked and cleared on that thread's own next wait_for() regardless.
  {
    // Under the mutex the realtime thread's wait predicate is checked under, so the wake-up
    // cannot be lost between that check and the thread going to sleep.
    std::lock_guard<std::mutex> flagLock(m_realtimeUpdateMutex);
    m_wakeRealtimeUpdateThread = true;
  }
  m_realtimeUpdateCv.notify_all();
  // Added 2026-09-27, a 42nd-pass audit, fixing a real, confirmed gap
  // found via a project-wide review, confirmed against Kodi's own real
  // current SDK source, not itself independently reproduced: this is
  // the one place in this whole ordering problem (see DeleteRecording()'s
  // own comment) an addon can actually get *ahead* of Kodi's own
  // resume-from-suspend ordering rather than just avoiding making it
  // worse. CPVRManager::OnWake() (PVRManager.cpp) calls
  // CPVRClients::OnSystemWake() -- which is what invokes this very
  // callback, synchronously, for every client, before OnWake() itself
  // goes on to call TriggerRecordingsUpdate() *before*
  // TriggerTimersUpdate() a few lines later. Kodi's own job queue
  // (CPVRManagerJobQueue::AppendJob()) is FIFO and only dedupes a job
  // against another already-pending job of the exact same type -- so a
  // pvr-update-timers-<id> job queued here, while this callback is still
  // running, is guaranteed to execute before whatever
  // TriggerRecordingsUpdate() queues moments later in the very same
  // OnWake() call, closing the same latch-bug window a suspend/resume
  // cycle would otherwise open (a recording that started, or had its
  // recording-vs-scheduled state otherwise change, while the device was
  // asleep).
  InvalidateAndTriggerTimerUpdate();
  // The channel list, the guide and the access token are all timed on
  // steady_clock, which doesn't advance while the device sleeps (see
  // dispatcharr::MarkStaleForWake()). Kodi's own CPVRManager::OnWake() asks
  // for channels and EPG again right after this returns, and without this the
  // addon would answer from whatever it fetched before the sleep -- for up to
  // channel_refresh_hours/epg_refresh_hours of awake time, and the guide's
  // dummy window is only ~3 days, so a long sleep could leave it entirely in
  // the past. The token has the same blind spot over a much shorter window,
  // and a WebSocket reconnect after wake has no reactive 401 to fix it.
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const auto now = std::chrono::steady_clock::now();
    m_channelsLoadedAt =
        dispatcharr::MarkStaleForWake(m_channelsLoadedAt, now, std::chrono::hours(m_channelRefreshHours));
    ++m_channelsGeneration;
    // The guide too: a fetch already running across the suspend would otherwise commit as fresh and undo the ageing
    // below.
    ++m_epgGeneration;
    m_epgLoadedAt = dispatcharr::MarkStaleForWake(m_epgLoadedAt, now, std::chrono::hours(m_epgRefreshHours));
    // A device waking up may be on a different network than the one that got
    // the rejection, so a grown retry interval shouldn't carry across.
    m_epgLastFailedAt = {};
    m_epgDurableFailureCount = 0;
  }
  m_client.InvalidateAccessToken();
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Data loading / caching
// ---------------------------------------------------------------------

bool PVRDispatcharr::EnsureChannelsLoaded(bool forceStale)
{
  auto now = std::chrono::steady_clock::now();
  {
    // Reading m_channelsLoadedAt under the same mutex that writes it
    // below (a real, confirmed data race otherwise -- this addon's own
    // background channel/EPG refresh thread and one of Kodi's own
    // calling threads for GetChannels()/GetChannelGroups()/etc. can both
    // call this concurrently; a plain std::chrono::time_point has no
    // synchronization of its own). The full gating decision (channels/EPG
    // staleness, a pending channel-groups-only retry, and the
    // channels-failure backoff) lives in dispatcharr::ShouldFetchChannels()
    // (Staleness.h) so it's unit-testable standalone -- see that
    // function's own comment, including the real bug fixed there (found
    // via a project-wide review): gating the channels-failure backoff
    // only when channels/EPG were stale left a groups-only retry free to
    // call GetChannels() completely unthrottled, since the function body
    // below always calls it first regardless of which trigger fired.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!dispatcharr::ShouldFetchChannels(m_channelsLoadedAt, m_channelsLastFailedAt, m_groupsLastFailedAt, now,
                                          std::chrono::hours(m_channelRefreshHours),
                                          std::chrono::minutes(kChannelEpgFailureRetryMinutes), forceStale))
      return false;
  }

  // See m_channelsFetchMutex's own comment. A caller that had to wait re-runs
  // the same gate afterwards -- with its own `forceStale` too, but the
  // first caller's commit just made the cache fresh, and `forceStale` only
  // bypasses the staleness check, not a retry backoff, so a forced caller
  // still refetches once after another caller's *forced* fetch; only the
  // unforced pair (the startup race) collapses into one fetch. `now` is
  // compared against the load time so "someone else just loaded it" can be
  // told from "nothing was due".
  std::unique_lock<std::mutex> fetchLock(m_channelsFetchMutex);
  uint64_t channelsGenerationAtStart = 0;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    channelsGenerationAtStart = m_channelsGeneration;
    if (m_channelsLoadedAt > now)
      return true;
    if (!dispatcharr::ShouldFetchChannels(m_channelsLoadedAt, m_channelsLastFailedAt, m_groupsLastFailedAt, now,
                                          std::chrono::hours(m_channelRefreshHours),
                                          std::chrono::minutes(kChannelEpgFailureRetryMinutes), forceStale))
      return false;
  }

  std::vector<Channel> channels;
  std::vector<ChannelGroup> groups;
  std::string error;
  bool ok = m_client.GetChannels(channels, error);
  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to load channels: %s", error.c_str());
    // A fresh timestamp here, not the entry-time `now` above (fixed
    // 2026-09-27, a 46th-pass audit, fixing a real, confirmed bug found
    // via a project-wide review, not itself independently reproduced):
    // GetChannels()'s own CURL timeout is `timeoutSeconds` (30s default),
    // comfortably able to exceed kChannelEpgFailureRetryMinutes (60s) on
    // its own for a slow/hanging failure (a genuine timeout, not just a
    // fast refused/RST) if `timeoutSeconds` is configured higher than
    // default -- and EnsureEpgLoaded()'s own XMLTV fetch (same failure-
    // timestamp pattern, its own comment) always can, at 4x this value.
    // Stamping the failure with the stale, pre-call `now` meant
    // IsRetryDue() saw the gate as already open the instant the failure
    // was even recorded, defeating the whole point of throttling a
    // repeated slow failure -- exactly the case that gate exists for,
    // not the fast-fail case it accidentally still worked for.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_channelsLastFailedAt = std::chrono::steady_clock::now();
    return false;
  }

  // Groups are best-effort: a channel list is still useful without them.
  // A *failed* fetch must not wipe out the previously-known-good groups
  // list though -- a real, confirmed cache-poisoning bug found via a
  // project-wide review, the same class already fixed for
  // EnsureTimerRulesLoaded() (see its own comment): committing an empty
  // groups list on a transient failure, cached fresh for the next
  // channel_refresh_hours (12h by default), makes Kodi (which syncs
  // channel groups with the backend) remove every group until the next
  // successful refresh. Falls back to the previous m_groups, re-filtered
  // against the just-fetched channels (which did succeed) below, rather
  // than an empty list.
  std::string groupsError;
  bool groupsOk = m_client.GetChannelGroups(groups, groupsError);
  if (!groupsOk)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to load channel groups: %s", groupsError.c_str());
    std::lock_guard<std::mutex> lock(m_dataMutex);
    groups = m_groups;
  }

  // The filtering itself lives in dispatcharr::FilterChannelGroupsWithChannels()
  // (ChannelGroupFilter.h) so it's unit-testable standalone -- see that
  // function's own comment.
  groups = FilterChannelGroupsWithChannels(std::move(groups), channels);

  bool shouldTriggerKodiSync = false;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // Fix for a real, confirmed bug found via a project-wide review (a
    // 23rd-pass audit), confirmed against Dispatcharr's own real current
    // upstream source, not itself independently reproduced: this addon's
    // own channel and EPG refreshes run on completely independent timers,
    // with nothing telling the EPG side that a channel renumbering (e.g.
    // Dispatcharr's own "compact numbering" scheme closing a gap after a
    // channel is hidden) just happened -- see
    // dispatcharr::HaveChannelNumbersChanged()'s own comment
    // (ChannelRenumbering.h) for the full account. Comparing against
    // m_channels *before* it's overwritten below, naturally false on a
    // first load (m_channels starts empty, so there's no common id to
    // disagree on yet). Only resets m_epgLoadedAt, not m_epgLastFailedAt --
    // the failure backoff still applies if the forced re-fetch this
    // triggers happens to fail.
    //
    // Also clears m_epgByChannelNumber outright, not just the loaded-at
    // timestamp (added 2026-09-26, a 26th-pass audit, fixing a real,
    // confirmed residual gap in the fix above found via a project-wide
    // review, not itself independently reproduced): resetting
    // m_epgLoadedAt alone still leaves the *old*, now-mismatched-keyed
    // EPG data in place to keep serving until the forced re-fetch this
    // triggers actually succeeds -- if that re-fetch is itself delayed
    // or fails, a renumbered channel keeps showing its former
    // neighbor's guide in the meantime instead of just no guide.
    // Clearing outright is safe: confirmed against Kodi's own real
    // source that its own EPG update merges entries and keeps a
    // channel's existing (already-cached-by-Kodi) tags when this addon
    // returns none for it, rather than blanking the display -- so this
    // trades "definitely wrong" for "briefly nothing new", not for a
    // visibly empty guide.
    bool channelNumbersChanged = dispatcharr::HaveChannelNumbersChanged(m_channels, channels);
    // Decided here, against m_channels/m_groups before they're overwritten
    // below -- see HasChannelLineupChanged()'s own comment.
    bool lineupChanged = dispatcharr::HasChannelLineupChanged(m_channels, m_groups, channels, groups);
    shouldTriggerKodiSync = dispatcharr::ShouldTriggerKodiChannelSync(lineupChanged, m_lastChannelSyncTriggerAt,
                                                                      std::chrono::steady_clock::now(),
                                                                      std::chrono::hours(kChannelSyncMaxSilenceHours));
    if (shouldTriggerKodiSync)
      m_lastChannelSyncTriggerAt = std::chrono::steady_clock::now();
    // Recomputed fresh from this fetch's own channel list every time, not
    // accumulated -- see m_ambiguousChannelNumbers's own comment
    // (PVRDispatcharr.h) for the real, live-confirmed bug this exists to
    // fix.
    m_ambiguousChannelNumbers = dispatcharr::FindAmbiguousChannelNumbers(channels);
    m_channels = std::move(channels);
    m_groups = std::move(groups);
    // The commit time, not the entry-time `now`: a caller that was waiting on
    // m_channelsFetchMutex tells "someone loaded this after I started" from
    // "nothing was due" by comparing against its own, earlier, entry time.
    m_channelsLoadedAt = std::chrono::steady_clock::now();
    // A fetch that began before something said the list was already out of date (an M3U refresh finished while it
    // ran: it may have read the lineup from before) keeps what it read but is not called fresh, so the refetch the
    // event asked for still happens.
    if (!dispatcharr::IsFetchStillCurrent(channelsGenerationAtStart, m_channelsGeneration))
      m_channelsLoadedAt = dispatcharr::MarkStaleForWake(m_channelsLoadedAt, m_channelsLoadedAt,
                                                         std::chrono::hours(m_channelRefreshHours));
    m_channelsLastFailedAt = {};
    // A fresh timestamp here, not the entry-time `now` above (fixed
    // 2026-09-27, a 47th-pass audit, fixing a real gap the 46th-pass
    // fix of this same stale-timestamp bug class missed in this exact
    // function, found via a project-wide review, not itself
    // independently reproduced): GetChannelGroups() runs its own
    // primary-then-fallback HTTP attempts in sequence, each with its own
    // `timeoutSeconds` CURL timeout, so a slow groups failure alone can
    // already exceed kChannelEpgFailureRetryMinutes (60s at default
    // settings) before the channels fetch's own time is even added --
    // the same "stamped failure defeats its own retry gate" bug
    // EnsureChannelsLoaded()'s/EnsureEpgLoaded()'s other failure sites
    // already had fixed.
    m_groupsLastFailedAt = groupsOk ? std::chrono::steady_clock::time_point{} : std::chrono::steady_clock::now();
    if (groupsOk)
      m_groupsLoadedAt = now;
    if (channelNumbersChanged)
    {
      m_epgLoadedAt = {};
      m_epgByChannelNumber.clear();
      ++m_epgGeneration;
      // Dispatcharr's own guide cache may still hold the pre-renumber numbers for a few minutes
      // (dispatcharr::kGuideRefetchAfterRenumber), so whatever guide is fetched next could be
      // keyed wrongly; fetch once more after that window has passed.
      m_epgRefetchDueAt = std::chrono::steady_clock::now() + dispatcharr::kGuideRefetchAfterRenumber;
    }
  }
  // Fix for a real, confirmed bug found via a project-wide review (a
  // 25th-pass audit), not itself independently reproduced: these two
  // triggers used to be called only from the background channel/EPG
  // refresh thread's own loop (StartChannelEpgRefreshThread()), never
  // from here -- but this function is also reachable directly on one of
  // Kodi's own calling threads (e.g. GetEPGForChannel()'s own
  // EnsureChannelsLoaded() call), whenever that thread happens to notice
  // stale channel data before the background thread's own next check
  // (up to kChannelEpgRefreshCheckMinutes later). A successful commit
  // reached that way updated this addon's own cache with no way for
  // Kodi to ever find out -- the stale channel lineup (added/removed/
  // renamed/renumbered channels) stayed visible in Kodi's own UI until a
  // later refresh the background thread happened to win instead,
  // routinely a full channel_refresh_hours (12h by default) away. Both
  // are fire-and-forget, safe to call from any thread (confirmed against
  // Kodi's real source: CPVRClient::cb_trigger_channel_update just
  // schedules a Kodi-manager-side update for its own next main-loop
  // iteration, no addon-side callback or lock involved) -- moved here so
  // every successful commit triggers regardless of which thread caused
  // it, rather than relying on the background thread's own duplicate
  // calls (removed from there as redundant).
  //
  // Now gated on an actual lineup change (or a 24h fallback), not every
  // commit -- see HasChannelLineupChanged()/ShouldTriggerKodiChannelSync()
  // (ChannelLineupChange.h, flagged from a 26th-pass audit): an unchanged
  // periodic refresh used to cost Kodi a full, pointless channel/group
  // resync each time.
  fetchLock.unlock();
  if (shouldTriggerKodiSync)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: channel lineup changed (or fallback due) -- triggering Kodi resync");
    TriggerChannelGroupsUpdate();
    TriggerChannelUpdate();
  }
  else
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: channel lineup unchanged -- skipping Kodi resync");
  }
  return true;
}

bool PVRDispatcharr::EnsureEpgLoaded()
{
  auto now = std::chrono::steady_clock::now();
  // The gate (stale or a refetch due, and the failure backoff run out -- see
  // m_channelsLastFailedAt's own comment for the per-channel-hammering fix the
  // backoff is) is dispatcharr::ShouldAttemptGuideFetch(), shared with
  // RequestGuideFetchIfWanted() so a Kodi callback wakes this fetch only when it
  // would actually run.
  auto fetchWanted = [this](std::chrono::steady_clock::time_point at)
  {
    return dispatcharr::ShouldAttemptGuideFetch(m_epgLoadedAt, m_epgRefetchDueAt, m_epgLastFailedAt,
                                                m_epgDurableFailureCount, at, std::chrono::hours(m_epgRefreshHours),
                                                std::chrono::minutes(kChannelEpgFailureRetryMinutes));
  };
  {
    // Same data-race fix as EnsureChannelsLoaded() above.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!fetchWanted(now))
      return false;
  }

  // See m_epgFetchMutex's own comment: a second caller that found the guide
  // stale at the same moment waits here, then finds the first one's result,
  // and returns true (a load newer than this call's own `now` exists) so its
  // per-channel TriggerEpgUpdate() pass still runs. Only the background thread
  // calls this since 2026-10-03, so in practice nobody waits here; kept as the
  // guard it is, should a second caller ever be added.
  std::unique_lock<std::mutex> fetchLock(m_epgFetchMutex);
  std::chrono::steady_clock::time_point refetchDueAtSeen{};
  bool refetchServing = false;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (m_epgLoadedAt > now)
      return true;
    if (!fetchWanted(now))
      return false;
    // Remembered so the commit below clears exactly the follow-up this fetch is serving, and
    // not one a renumbering scheduled while the fetch was running.
    refetchDueAtSeen = m_epgRefetchDueAt;
    refetchServing = dispatcharr::IsGuideRefetchDue(refetchDueAtSeen, now);
  }

  // Fix for a real, confirmed bug found via a project-wide review (a
  // 26th-pass audit), confirmed against Dispatcharr's own real current
  // upstream source, not itself independently reproduced: this addon's
  // own channel and EPG refreshes run on completely independent timers
  // (channel_refresh_hours, default 12h, vs. epg_refresh_hours, default
  // 4h) -- at these defaults, the very next XMLTV refresh after a
  // server-side channel renumbering is far more likely to land *before*
  // the next channel refresh, not after. `HaveChannelNumbersChanged()`'s
  // own detection (EnsureChannelsLoaded()'s own comment,
  // ChannelRenumbering.h) only helps once the *channel* side notices a
  // renumbering first -- it can't help if the EPG side gets there first,
  // since m_channels (and this fetch's own m_epgByChannelNumber key
  // space) would still reflect the *old* numbers when the *new* XMLTV
  // (already keyed by Dispatcharr's own new numbers) gets parsed and
  // committed, silently pairing each renumbered channel with its former
  // neighbor's guide for however long the channel side stays stale
  // (routinely most of a 12h window). Forcing a channel refresh here,
  // immediately before the XMLTV fetch below (not the other way around --
  // committing the XMLTV first, then discovering a renumbering, would
  // just throw the freshly-committed EPG away and force a second,
  // redundant XMLTV fetch), keeps the two in lockstep: bypasses only the
  // channels-side staleness check (`forceStale`), not its own
  // failure backoff (`dispatcharr::ShouldFetchChannels()`'s own comment,
  // Staleness.h), and a failed forced fetch here doesn't block the XMLTV
  // fetch below at all -- its return value is deliberately ignored. Side
  // effect, worth knowing: channels now effectively refresh at least as
  // often as the EPG does (every epg_refresh_hours, not just
  // channel_refresh_hours) -- a real increase in background API calls
  // at the default 4h/12h split, judged worth it against silently wrong
  // per-channel guide data for hours at a time.
  //
  // Skipped when channels were already refreshed very recently (added
  // 2026-09-26, a 27th-pass audit, fixing a real, confirmed regression
  // risk in the fix above found via a project-wide review, not itself
  // independently reproduced) -- see kForceChannelRefreshMinAgeMinutes's
  // own comment (PVRDispatcharr.h) for why: without this, the force
  // above turned every already-due EPG retry into a redundant full
  // channel+groups fetch, certainly once at every startup and,
  // considerably worse, on every single retry of a persistent XMLTV
  // failure (e.g. the already-logged Network Access 403 case).
  bool channelsRecentlyLoaded;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    channelsRecentlyLoaded =
        !dispatcharr::IsStaleSince(m_channelsLoadedAt, now, std::chrono::minutes(kForceChannelRefreshMinAgeMinutes));
  }
  if (!channelsRecentlyLoaded && !EnsureChannelsLoaded(/*forceStale=*/true))
  {
    // The forced refresh did not happen -- it failed, or its own failure backoff
    // is still running. Fetching the guide anyway would commit XMLTV keyed by
    // Dispatcharr's *current* channel numbers against a channel list that may
    // still hold the old ones: the very mismatch this force exists to prevent,
    // reached a different way (docs/CLOSED_ITEMS.md, "Residual gaps in the forced
    // channel refresh before the EPG fetch", (a)). So treat this guide attempt as
    // failed too, and let both retry together on the ordinary retry interval. The
    // cost, accepted: while channels keep failing, the guide stays as stale as
    // they are. Left out of the durable-failure count on purpose -- that tracks
    // the guide endpoint rejecting this addon, which did not happen here.
    kodi::Log(ADDON_LOG_WARNING,
              "pvr.dispatcharr-unofficial: deferring the guide fetch: the channel refresh it must follow did not "
              "complete");
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_epgLastFailedAt = std::chrono::steady_clock::now();
    return false;
  }

  // Taken only now, after the forced channel refresh above: a renumbering that refresh
  // detects bumps this generation and must not discard the guide fetched *after* it.
  uint64_t epgGenerationAtStart;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    epgGenerationAtStart = m_epgGeneration;
  }

  std::string xml, error;
  long httpStatus = 0;
  // Already-aired programmes too, as far back as the longest catch-up window of any
  // channel that offers it -- see dispatcharr::ComputeGuidePrevDays().
  int guidePrevDays = 0;
  {
    std::vector<int> offeredCatchupDays;
    std::lock_guard<std::mutex> lock(m_dataMutex);
    for (const auto& ch : m_channels)
    {
      if (dispatcharr::ShouldOfferCatchup(ch.catchupEnabled, m_catchupEnabledGlobally, m_catchupEnabledForCurrentUser))
        offeredCatchupDays.push_back(ch.catchupDays);
    }
    guidePrevDays = dispatcharr::ComputeGuidePrevDays(offeredCatchupDays);
  }
  bool guideUnusable = false;
  if (!m_client.GetXmlTvGuide(xml, error, &httpStatus, guidePrevDays, &guideUnusable))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to fetch XMLTV guide: %s", error.c_str());
    // A fresh timestamp here, not the entry-time `now` above -- see
    // EnsureChannelsLoaded()'s own comment on m_channelsLastFailedAt for
    // the full account (fixed 2026-09-27, a 46th-pass audit). Matters
    // even more here: GetXmlTvGuide()'s own CURL timeout is
    // `timeoutSeconds * 4` (120s default), double
    // kChannelEpgFailureRetryMinutes (60s) even at default settings, so a
    // genuine XMLTV fetch timeout always immediately reopened the retry
    // gate the instant the failure was recorded.
    //
    // A durable rejection (the Network Access 403) also grows the retry
    // interval -- see dispatcharr::ShouldCountTowardEpgFailureBackoff(). An
    // outage (no response, 5xx) resets the count so a recovered server is
    // retried on the flat interval again.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_epgLastFailedAt = std::chrono::steady_clock::now();
    m_epgDurableFailureCount =
        dispatcharr::ShouldCountTowardEpgFailureBackoff(httpStatus, guideUnusable) ? m_epgDurableFailureCount + 1 : 0;
    if (m_epgDurableFailureCount > 1)
      kodi::Log(ADDON_LOG_ERROR,
                "pvr.dispatcharr-unofficial: guide fetch rejected %d times in a row -- next retry in "
                "%lld minute(s)",
                m_epgDurableFailureCount,
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::minutes>(
                        dispatcharr::ComputeEpgFailureRetryInterval(
                            m_epgDurableFailureCount, std::chrono::minutes(kChannelEpgFailureRetryMinutes)))
                        .count()));
    return false;
  }

  std::unordered_map<std::string, std::vector<EpgEntry>> parsed;
  // Parsing copies the whole document, builds a DOM and then the per-channel map;
  // on a constrained or 32-bit address space that is where a large guide runs out
  // of memory. This runs on the background thread, where an uncaught bad_alloc is
  // std::terminate() for the whole Kodi process, so it is a failed refresh instead.
  bool parsedOk = false;
  try
  {
    parsedOk = XmlTvParser::Parse(xml, parsed, error);
  }
  catch (const std::bad_alloc&)
  {
    parsed.clear();
    error = "out of memory parsing the XMLTV guide";
  }
  if (!parsedOk)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to parse XMLTV guide: %s", error.c_str());
    // Same fresh-timestamp fix as the fetch failure above -- a CPU-bound
    // parse is unlikely to itself take anywhere near
    // kChannelEpgFailureRetryMinutes, but there's no reason to leave this
    // site inconsistent with the one right above it.
    //
    // A guide that downloaded but cannot be parsed (or parsed only by running out of memory) is
    // a durable failure: the same bytes come back a minute later -- see
    // dispatcharr::ShouldCountTowardEpgFailureBackoff()'s responseUnusable.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_epgLastFailedAt = std::chrono::steady_clock::now();
    ++m_epgDurableFailureCount;
    return false;
  }

  // Real, confirmed bug found via a project-wide review (a 44th-pass
  // audit), confirmed against Kodi's own real current SDK source, not
  // itself independently reproduced: PVRDispatcharr::GetTimers()/
  // GetRecordings() compute their own EPG-tag link (SetEPGUid()/
  // SetEPGEventId(), see EpgProgramMatch.h) purely from whatever
  // m_epgByChannelNumber already holds at the moment they're called --
  // but at Kodi startup, CPVRManager::UpdateComponents() loads timers
  // and recordings *before* the EPG container starts, and this addon's
  // own XMLTV fetch/parse (a full-guide parse, seconds on a large one)
  // routinely finishes well after that. Nothing previously refreshed
  // timers/recordings again once the EPG cache actually did finish
  // loading, so a recording created shortly before its own EPG entry
  // was available (or one whose start got clamped, the pass-32 case)
  // kept showing "Record" instead of a recording indicator, and pressing
  // it again created a duplicate recording server-side -- until the next
  // unrelated timer/recording refresh happened to occur, or Kodi
  // restarted. The same gap re-opens after a channel renumbering clears
  // m_epgByChannelNumber outright (see EnsureChannelsLoaded()'s own
  // comment) -- epgNeverLoadedBefore is naturally true again in that
  // case too, since that same renumbering branch also resets
  // m_epgLoadedAt, so this closes both without special-casing the
  // renumbering path separately.
  bool epgNeverLoadedBefore;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // A channel renumbering detected while this guide was being fetched has already cleared
    // the cache and its loaded-at stamp; committing this (pre-renumber, keyed by the old
    // numbers) parse as fresh would undo that for a full epg_refresh_hours. Discard it; the
    // next cycle fetches again straight away (nothing is stamped as failed).
    if (!dispatcharr::IsFetchStillCurrent(epgGenerationAtStart, m_epgGeneration))
    {
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: discarding a guide fetched across a channel renumbering; it will be "
                "fetched again");
      return false;
    }
    epgNeverLoadedBefore = dispatcharr::HasNeverLoadedSuccessfully(m_epgLoadedAt);
    m_epgByChannelNumber = std::move(parsed);
    m_epgLoadedAt = std::chrono::steady_clock::now();
    m_epgLastFailedAt = {};
    m_epgDurableFailureCount = 0;
    if (refetchServing && m_epgRefetchDueAt == refetchDueAtSeen)
    {
      m_epgRefetchDueAt = {};
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: guide fetched again after a channel renumbering, past the server's "
                "guide cache window");
    }
  }
  fetchLock.unlock();
  if (epgNeverLoadedBefore)
  {
    // Timers before recordings -- see HandleRealtimeUpdateMessage()'s own
    // comment on why the order matters.
    InvalidateAndTriggerTimerUpdate();
    InvalidateAndTriggerRecordingUpdate();
  }
  return true;
}

bool PVRDispatcharr::EnsureRecordingsLoaded()
{
  auto now = std::chrono::steady_clock::now();
  uint64_t generationAtStart;
  {
    // Same data-race fix as EnsureChannelsLoaded() above.
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!dispatcharr::IsStaleSince(m_recordingsCachedAt, now,
                                   std::chrono::seconds(kRecordingsAndTimersCacheTtlSeconds)))
      return false;
    generationAtStart = m_recordingsGeneration;
  }

  std::vector<Recording> recordings;
  std::string error;
  if (!m_client.GetRecordings(recordings, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to load recordings: %s", error.c_str());
    return false;
  }

  if (m_debugLogging)
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: recordings cache refreshed (%zu recording(s))",
              recordings.size());
  std::lock_guard<std::mutex> lock(m_dataMutex);
  m_cachedRecordings = std::move(recordings);
  m_recordingsEverLoaded = true;
  // Not marked fresh when an invalidation landed while this fetch was running: its result
  // predates whatever that invalidation is about, and the next call must fetch again.
  if (dispatcharr::IsFetchStillCurrent(generationAtStart, m_recordingsGeneration))
    m_recordingsCachedAt = now;
  return true;
}

bool PVRDispatcharr::EnsureTimerRulesLoaded()
{
  auto now = std::chrono::steady_clock::now();
  uint64_t seriesGenerationAtStart, recurringGenerationAtStart;
  {
    // Same data-race fix as EnsureChannelsLoaded() above. Gated on
    // m_seriesRulesCachedAt alone (not also m_recurringRulesCachedAt) --
    // series rules need only view-level DVR access and so should always
    // be fetchable, the same role m_channelsLoadedAt plays for the
    // channels+groups combined fetch attempt above (see
    // m_seriesRulesCachedAt's own comment, PVRDispatcharr.h).
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!dispatcharr::IsStaleSince(m_seriesRulesCachedAt, now,
                                   std::chrono::seconds(kRecordingsAndTimersCacheTtlSeconds)))
      return false;
    seriesGenerationAtStart = m_seriesRulesGeneration;
    recurringGenerationAtStart = m_recurringRulesGeneration;
  }

  // An empty rules list is itself a legitimate response (no series/
  // recurring timers exist), but a *failed* fetch must not be cached as
  // that same empty list -- both return values used to be discarded
  // entirely, so a transient failure here got cached as "zero rules"
  // for the next kRecordingsAndTimersCacheTtlSeconds regardless. A
  // concurrent UpdateTimer()/DeleteTimer() series-rule lookup landing in
  // that window would silently miss this cache and fall back to a
  // re-derived tvgId instead -- exactly the drifted-tvgId bug
  // TimerIdentity.h's own cache-based lookup was added to fix (see
  // MatchRecordingsToSeriesRules()'s own comment). If either fetch
  // fails, leave the existing cache (whatever it already was) in place
  // rather than overwrite it with a possibly-incomplete result.
  //
  // The two fetches are committed independently (added 2026-09-27, a
  // 41st-pass audit -- see m_seriesRulesCachedAt's own comment,
  // PVRDispatcharr.h, for the real regression this fixes): a permission-
  // denied recurring-rules fetch must not also block series rules (and,
  // downstream, the whole Timers list) from ever being considered loaded.
  std::vector<TimerRule> rules;
  std::vector<RecurringRule> recurringRules;
  std::string rulesError, recurringRulesError;
  bool rulesOk = m_client.GetTimerRules(rules, rulesError);
  bool recurringOk = m_client.GetRecurringRules(recurringRules, recurringRulesError);
  if (!rulesOk)
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to load series timer rules: %s", rulesError.c_str());
  if (!recurringOk)
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to load recurring timer rules: %s",
              recurringRulesError.c_str());

  if (m_debugLogging)
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: timer-rules cache refreshed (series ok=%d, recurring ok=%d)", rulesOk,
              recurringOk);
  std::lock_guard<std::mutex> lock(m_dataMutex);
  if (rulesOk)
  {
    m_cachedTimerRules = std::move(rules);
    m_seriesRulesEverLoaded = true;
    if (dispatcharr::IsFetchStillCurrent(seriesGenerationAtStart, m_seriesRulesGeneration))
      m_seriesRulesCachedAt = now;
  }
  if (recurringOk)
  {
    m_cachedRecurringRules = std::move(recurringRules);
    if (dispatcharr::IsFetchStillCurrent(recurringGenerationAtStart, m_recurringRulesGeneration))
      m_recurringRulesCachedAt = now;
  }
  return rulesOk && recurringOk;
}

const Channel* PVRDispatcharr::FindChannelByUid(int uid) const
{
  for (const auto& ch : m_channels)
  {
    if (ch.id == uid)
      return &ch;
  }
  return nullptr;
}

unsigned int PVRDispatcharr::ResolveRecordingBroadcastId(const dispatcharr::Recording& rec)
{
  if (rec.channelId <= 0)
    return EPG_TAG_INVALID_UID;

  std::lock_guard<std::mutex> lock(m_dataMutex);

  // The start time of the EPG programme this recording belongs to, from the cached
  // guide -- 0 when it cannot be found there (no channel, nothing usable keyed,
  // ambiguous, or the programme already dropped out of the guide).
  time_t programStart = 0;
  const Channel* ch = FindChannelByUid(rec.channelId);
  if (ch)
  {
    // The exact text Dispatcharr's own XMLTV export keys this channel by --
    // see dispatcharr::FormatChannelGuideKey() (ChannelNumber.h): the number's
    // text, or the channel's id when it has no number. Empty means nothing
    // usable, which is never looked up.
    std::string channelNumberKey = dispatcharr::ChannelGuideKey(*ch);
    // See m_ambiguousChannelNumbers's own comment -- a shared channel
    // number means m_epgByChannelNumber's own entry for it is the union of
    // every colliding channel's own programmes, with no way to tell which
    // one actually aired this recording. Fail toward "no linked EPG tag"
    // rather than risk matching against the wrong channel's programme.
    auto epgIt = channelNumberKey.empty() || m_ambiguousChannelNumbers.count(channelNumberKey)
                     ? m_epgByChannelNumber.end()
                     : m_epgByChannelNumber.find(channelNumberKey);
    if (epgIt != m_epgByChannelNumber.end())
    {
      // Prefer an exact match against the recording's own true, never-padded
      // programme start time when available -- see
      // dispatcharr::FindEpgEntryIndexByStartTime()'s own comment
      // (EpgProgramMatch.h) for why the overlap-ratio fallback below isn't
      // always enough (a padded and/or past-start-clamped recording).
      int entryIdx = -1;
      if (rec.programStartTime > 0)
        entryIdx = dispatcharr::FindEpgEntryIndexByStartTime(epgIt->second, rec.programStartTime,
                                                             kEpgStartTimeMatchToleranceSeconds);
      if (entryIdx < 0)
      {
        // dispatcharr::ResolveEpgOverlapWindow() (EpgProgramMatch.h) picks
        // the recording's own true, never-padded programme window over its
        // possibly-padded/clamped one when a valid one is known -- see that
        // function's own comment for the real, confirmed gap this closes (a
        // 36th-pass audit, not itself independently reproduced): the exact
        // match above can legitimately fail even with a known
        // programStartTime (this addon's own EPG cache drops an
        // already-ended entry at its next XMLTV refresh), and falling back
        // to the padded window in that case risked a wrong match, not just
        // a missing one.
        dispatcharr::EpgOverlapWindow window =
            dispatcharr::ResolveEpgOverlapWindow(rec.startTime, rec.endTime, rec.programStartTime, rec.programEndTime);
        entryIdx = dispatcharr::FindEpgEntryIndexCoveringRecording(epgIt->second, window.start, window.end);
      }
      if (entryIdx >= 0)
        programStart = epgIt->second[static_cast<std::size_t>(entryIdx)].startTime;
    }
  }

  if (programStart > 0)
  {
    // Found in the guide: remember it, so the link outlives this guide entry.
    dispatcharr::RecordingEpgLink link;
    link.channelId = rec.channelId;
    link.recordingStartTime = rec.startTime;
    link.programStartTime = programStart;
    if (dispatcharr::RememberRecordingEpgLink(m_recordingEpgLinks, rec.id, link))
      m_recordingEpgLinksDirty = true;
  }
  else
  {
    // Not in the guide (any more): fall back to what was remembered while it was.
    // Trusted even for a channel that is ambiguous now, since a link is only ever
    // remembered from an unambiguous match.
    programStart = dispatcharr::LookupRememberedProgramStart(m_recordingEpgLinks, rec.id, rec.channelId, rec.startTime);
  }
  if (programStart <= 0)
    return EPG_TAG_INVALID_UID;

  return dispatcharr::ComputeBroadcastId(rec.channelId, programStart);
}

void PVRDispatcharr::LoadRecordingEpgLinks()
{
  const std::string path = kodi::addon::GetUserPath("recording_epg_links.json");
  if (!kodi::vfs::FileExists(path, false))
    return;
  kodi::vfs::CFile file;
  if (!file.OpenFile(path, 0))
    return;
  std::string text;
  char buffer[8192];
  ssize_t got;
  while ((got = file.Read(buffer, sizeof(buffer))) > 0)
    text.append(buffer, static_cast<std::size_t>(got));
  file.Close();

  dispatcharr::RecordingEpgLinkMap loaded;
  if (!dispatcharr::ParseRecordingEpgLinks(text, loaded))
  {
    kodi::Log(ADDON_LOG_WARNING,
              "pvr.dispatcharr-unofficial: ignoring an unreadable recording guide-link file (it is rebuilt as "
              "recordings are matched)");
    return;
  }
  std::lock_guard<std::mutex> lock(m_dataMutex);
  m_recordingEpgLinks = std::move(loaded);
  m_recordingEpgLinksDirty = false;
}

void PVRDispatcharr::SaveRecordingEpgLinksIfDirty()
{
  std::lock_guard<std::mutex> saveLock(m_recordingEpgLinksSaveMutex);
  std::string text;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    if (!m_recordingEpgLinksDirty)
      return;
    text = dispatcharr::SerializeRecordingEpgLinks(m_recordingEpgLinks);
    m_recordingEpgLinksDirty = false;
  }
  const std::string dir = kodi::addon::GetUserPath();
  if (!kodi::vfs::DirectoryExists(dir))
    kodi::vfs::CreateDirectory(dir);
  if (!WriteStateFileAtomically(kodi::addon::GetUserPath("recording_epg_links.json"), text))
  {
    kodi::Log(ADDON_LOG_WARNING, "pvr.dispatcharr-unofficial: could not save the recording guide-link file");
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_recordingEpgLinksDirty = true; // try again next time
  }
}

// ---------------------------------------------------------------------
// Channel groups
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetChannelGroupsAmount(int& amount)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // Real, confirmed bug found via a project-wide review (a 40th-pass
  // audit): reporting PVR_ERROR_NO_ERROR here when groups have never
  // successfully loaded even once tells Kodi this (empty) result is
  // authoritative -- see dispatcharr::HasNeverLoadedSuccessfully()'s own
  // comment (Staleness.h) for the full Kodi-side deletion mechanism this
  // avoids triggering.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_groupsLoadedAt))
    return PVR_ERROR_SERVER_ERROR;
  amount = static_cast<int>(m_groups.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR; // no radio support

  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // See GetChannelGroupsAmount()'s own comment above.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_groupsLoadedAt))
    return PVR_ERROR_SERVER_ERROR;
  for (const auto& group : m_groups)
  {
    kodi::addon::PVRChannelGroup g;
    g.SetGroupName(group.name);
    g.SetIsRadio(false);
    results.Add(g);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelGroupMembers(const kodi::addon::PVRChannelGroup& group,
                                                 kodi::addon::PVRChannelGroupMembersResultSet& results)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // See GetChannelGroupsAmount()'s own comment above -- m_groupsLoadedAt
  // only ever advances once channels have also loaded successfully in
  // that same cycle (EnsureChannelsLoaded() returns before ever calling
  // GetChannelGroups() if the channels fetch itself fails), so this one
  // check alone covers both caches this function reads from.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_groupsLoadedAt))
    return PVR_ERROR_SERVER_ERROR;

  // Through dispatcharr::FindChannelGroupIdByKodiName() (ChannelGroupFilter.h),
  // not a plain name comparison -- see that function's own comment for
  // the real truncated-name gap this closes.
  int groupId = dispatcharr::FindChannelGroupIdByKodiName(m_groups, group.GetGroupName());
  if (groupId == -1)
    return PVR_ERROR_NO_ERROR;

  for (const auto& ch : m_channels)
  {
    if (ch.groupId != groupId)
      continue;
    kodi::addon::PVRChannelGroupMember member;
    member.SetGroupName(group.GetGroupName());
    member.SetChannelUniqueId(ch.id);
    member.SetChannelNumber(static_cast<unsigned int>(dispatcharr::WholeChannelNumber(ch.channelNumber)));
    member.SetSubChannelNumber(static_cast<unsigned int>(dispatcharr::SubChannelNumber(ch.channelNumber)));
    results.Add(member);
  }
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetChannelsAmount(int& amount)
{
  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // See GetChannelGroupsAmount()'s own comment above -- the channels-side
  // counterpart, keyed off m_channelsLoadedAt rather than m_groupsLoadedAt.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_channelsLoadedAt))
    return PVR_ERROR_SERVER_ERROR;
  amount = static_cast<int>(m_channels.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
  if (radio)
    return PVR_ERROR_NO_ERROR;

  EnsureChannelsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // See GetChannelsAmount()'s own comment above.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_channelsLoadedAt))
    return PVR_ERROR_SERVER_ERROR;
  for (const auto& ch : m_channels)
  {
    kodi::addon::PVRChannel channel;
    channel.SetUniqueId(static_cast<unsigned int>(ch.id));
    channel.SetIsRadio(false);
    // Both halves, not just the whole number: a subchannel (5.1) used to
    // reach Kodi as plain channel 5 -- see dispatcharr::SplitChannelNumber()
    // (ChannelNumber.h).
    channel.SetChannelNumber(static_cast<unsigned int>(dispatcharr::WholeChannelNumber(ch.channelNumber)));
    channel.SetSubChannelNumber(static_cast<unsigned int>(dispatcharr::SubChannelNumber(ch.channelNumber)));
    channel.SetChannelName(ch.name);
    if (ch.logoId >= 0)
      channel.SetIconPath(m_client.GetChannelLogoUrl(ch.logoId));
    channel.SetIsHidden(false);
    // Confirmed live (2026-09-14): this was never set, so Kodi's own
    // "hasarchive" (JSON-RPC PVR.GetChannels/PVR.GetChannelDetails, and
    // whatever GUI affordance the skin drives from it) always reported
    // false even for real catch-up-enabled channels -- despite
    // catchupEnabled/catchupDays already being parsed and correctly used
    // later in GetEPGTagStreamProperties()'s own catch-up logic below.
    // Also folds in the global/per-user catch-up gates (added 2026-09-28,
    // see dispatcharr::ShouldOfferCatchup()'s own comment, EpgTagUtil.h,
    // for the real, live-confirmed bug this fixes) -- a channel that
    // itself supports catch-up but whose account has it disabled used to
    // still report hasarchive=true here.
    channel.SetHasArchive(
        dispatcharr::ShouldOfferCatchup(ch.catchupEnabled, m_catchupEnabledGlobally, m_catchupEnabledForCurrentUser));
    results.Add(channel);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetChannelStreamProperties(const kodi::addon::PVRChannel& channel,
                                                     std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  // Read once into a local, not twice from the atomic member (fixed
  // 2026-09-27, a 61st-pass audit, fixing a real, confirmed gap found
  // via a project-wide review, not itself independently reproduced,
  // corrected 2026-09-27, a 62nd-pass audit, for this comment's own
  // inaccurate claim that BuildLiveChannelStreamProperties() itself
  // reads the member -- it only ever reads whatever value its own
  // caller passes in as a parameter): a settings-dialog save can change
  // m_liveTimeshiftMode (OnAddonSettingChanged()) at any time, including
  // between this read and what USED to be this same function's own
  // second read further below, at the BuildLiveChannelStreamProperties()
  // call-site's own argument expression (fixed away by this same local
  // variable, reused there instead of re-reading the member) -- before
  // that fix, if the mode read here (deciding whether to populate
  // streamUrl) differed from the mode read there (deciding which
  // properties to actually build), the two could disagree: streamUrl
  // stayed empty because this read saw Server mode, but properties got
  // built for a non-Server mode that expects streamUrl populated,
  // failing that one channel-open attempt with an empty STREAMURL.
  int liveTimeshiftMode = m_liveTimeshiftMode;
  std::string streamUrl;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(channel.GetUniqueId()));
    if (!ch)
      return PVR_ERROR_INVALID_PARAMETERS;
    if (liveTimeshiftMode != kLiveTimeshiftServer)
      streamUrl = m_client.GetLiveStreamUrl(*ch);
  }

  // Live pause/rewind ("timeshift") is opt-in via live_timeshift_mode --
  // see docs/TIMESHIFT.md for the full history: an earlier local
  // (inputstream.ffmpegdirect on-device buffer) mode was removed once
  // server-side proved stable, then reintroduced once a real need for a
  // non-admin-account path came up (see kLiveTimeshiftLocal's own comment
  // and that setting's help text) -- Off and Local are both handled in
  // the branch below, alongside Server-side here. Server-side
  // deliberately leaves STREAMURL unset (confirmed elsewhere in this
  // addon, see GetRecordingStreamProperties()'s comment, that Kodi uses
  // STREAMURL directly via its generic CCurlFile when it's set,
  // bypassing addon stream callbacks entirely) so Kodi falls through to
  // this addon's own OpenLiveStream()/ReadLiveStream()/SeekLiveStream()
  // (PVRCapabilities::SetHandlesInputStream(), set in GetCapabilities())
  // instead of routing through inputstream.ffmpegdirect via a plain URL
  // -- ffmpegdirect's generic HLS seek is confirmed broken for this
  // addon's rolling server-side buffer (see docs/TIMESHIFT.md's seek
  // investigation). Local mode is delegated entirely to the separate
  // inputstream.ffmpegdirect addon instead: its own dedicated
  // TimeshiftStream class (stream_mode: timeshift) is exactly built for
  // a genuinely live, continuously arriving source with no native
  // pause/rewind of its own -- works independent of any Dispatcharr-side
  // support at all, since the buffer lives as a local on-device
  // recording managed entirely by ffmpegdirect's own settings. The
  // actual buffer-start call happens in OpenLiveStream(), not here.
  //
  // The field-mapping itself lives in
  // dispatcharr::BuildLiveChannelStreamProperties() (StreamPropertyUtil.h)
  // so it's unit-testable standalone -- see that function's own comment.
  for (const auto& p : dispatcharr::BuildLiveChannelStreamProperties(liveTimeshiftMode, streamUrl))
    properties.emplace_back(p.name, p.value);
  if (m_debugLogging)
  {
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: GetChannelStreamProperties: returning %zu properties",
              properties.size());
    for (const auto& p : properties)
      kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial:   prop %s = %s", p.GetName().c_str(),
                p.GetValue().c_str());
  }
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::OpenLiveStream(const kodi::addon::PVRChannel& channel)
{
  // Only ever actually called for a server-side-timeshift channel -- see
  // GetChannelStreamProperties(), which is the only mode that leaves STREAMURL
  // unset, so Kodi opening this callback is itself the signal that it chose Server
  // mode. There is deliberately no re-check of m_liveTimeshiftMode here: it was a
  // second, independent read of an atomic that a settings save can flip between
  // Kodi's two separate callbacks, and when it did, an open that Kodi had already
  // committed to failed because the two reads disagreed, for no benefit -- the
  // plugin call below is the real gate and fails clearly on its own (docs/CLOSED_ITEMS.md,
  // "OpenLiveStream() torn atomic read across Kodi callbacks").

  std::string channelUuid;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(channel.GetUniqueId()));
    if (!ch)
      return false;
    channelUuid = ch->uuid;
  }

  std::string error;
  if (!m_client.OpenLiveTimeshiftStream(channelUuid, error))
  {
    kodi::Log(ADDON_LOG_ERROR,
              "pvr.dispatcharr-unofficial: failed to open server-side timeshift stream for channel %s: "
              "%s (confirm the timeshift_buffer Dispatcharr plugin is installed and enabled, and "
              "that this addon's configured account is a Dispatcharr admin)",
              channelUuid.c_str(), error.c_str());
    return false;
  }
  return true;
}

void PVRDispatcharr::CloseLiveStream()
{
  m_client.CloseLiveTimeshiftStream();
}

int PVRDispatcharr::ReadLiveStream(unsigned char* buffer, unsigned int size)
{
  return m_client.ReadLiveTimeshiftStream(buffer, size);
}

int64_t PVRDispatcharr::SeekLiveStream(int64_t position, int whence)
{
  return m_client.SeekLiveTimeshiftStream(position, whence);
}

int64_t PVRDispatcharr::LengthLiveStream()
{
  return m_client.GetLiveTimeshiftStreamLength();
}

bool PVRDispatcharr::CanPauseStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

bool PVRDispatcharr::CanSeekStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

bool PVRDispatcharr::IsRealTimeStream()
{
  return m_client.IsLiveTimeshiftStreamOpen() || m_client.IsInProgressRecordingStreamOpen();
}

PVR_ERROR PVRDispatcharr::GetStreamTimes(kodi::addon::PVRStreamTimes& times)
{
  // Same self-heal persistence gap as ReadRecordedStream()/
  // LengthRecordedStream() (see their own comment): both branches below
  // can reach RefreshInProgressRecordingManifest()'s own proactive
  // key-regeneration self-heal via GetInProgressRecordingStreamDurationMs()
  // -- once via the debug-log line just below, unconditionally via the
  // in-progress-recording branch further down. GetStreamTimes() is
  // polled far more often than either of those two (Kodi calls it
  // repeatedly during active playback), so it's plausibly the *first*
  // place a stale key actually gets detected and regenerated in
  // practice -- without this, that regeneration was silently never
  // saved, only ever costing an extra 401-and-regenerate round trip on
  // this install's next restart (self-healing, not data loss, hence
  // low severity -- found via a project-wide review, not reproduced
  // live).
  std::string keyBefore = m_client.GetApiKey();
  // Guarded by m_debugLogging, unlike a plain kodi::Log(ADDON_LOG_DEBUG,
  // ...) call elsewhere -- a real, confirmed inefficiency found via a
  // project-wide review, not reproduced live: kodi::Log()'s own
  // arguments are always evaluated regardless of whether debug logging
  // is enabled or the message ends up discarded, and
  // GetInProgressRecordingStreamDurationMs() isn't a cheap read -- it
  // triggers a (throttled, but still real) manifest refresh attempt as
  // a side effect. That meant every single GetStreamTimes() call (Kodi
  // calls this frequently during active playback to update seek-bar/
  // stream-time displays) paid that cost purely to build a log line
  // nobody would ever see with debug logging off.
  if (m_debugLogging)
  {
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: GetStreamTimes called: liveTimeshiftOpen=%d inProgressOpen=%d "
              "durationMs=%lld",
              m_client.IsLiveTimeshiftStreamOpen() ? 1 : 0, m_client.IsInProgressRecordingStreamOpen() ? 1 : 0,
              static_cast<long long>(m_client.GetInProgressRecordingStreamDurationMs()));
  }
  // startTime/ptsStart both zero: no meaningful wall-clock "show start" for
  // a growing buffer/recording the way a scheduled EPG programme would
  // have, so pts values here are purely self-relative rather than
  // UTC-anchored -- see kodi-dev-kit's own PVRStreamTimes doc comments.
  // ptsEnd is in microseconds and grows on every call as the growing
  // source's own manifest gets refreshed -- that growth, reported live, is
  // what gives real pause/rewind/live-follow instead of the
  // fixed-duration-or-nothing ffmpegdirect route this replaced (see
  // docs/TIMESHIFT.md and docs/RECORDINGS.md).
  //
  // CInputStreamPVRRecording extends the same CInputStreamPVRBase as
  // CInputStreamPVRChannel (confirmed in Kodi-core source), so this same
  // callback drives both a live-timeshift channel and an in-progress
  // recording -- only one of the two is ever open at once, so checking
  // both here is safe and simplest. Checking the *setting*
  // (m_liveTimeshiftMode == kLiveTimeshiftServer) here instead of actual
  // open state was a real bug, not just imprecision: that setting doesn't
  // change once a stream closes, so it stayed true while an in-progress
  // recording was playing with server-side timeshift also enabled,
  // permanently shadowing the recording branch below and reporting
  // GetLiveTimeshiftStreamDurationMs()'s 0 (no live stream open) as ptsEnd
  // instead -- confirmed live: canseek/totaltime stayed false/0 despite
  // GetInProgressRecordingStreamDurationMs() correctly growing every call.
  if (m_client.IsLiveTimeshiftStreamOpen())
  {
    // A real, non-zero startTime here is load-bearing, not cosmetic --
    // see GetLiveTimeshiftStreamWallClockAnchor()'s own comment. A zero/
    // falsy startTime makes Kodi-core's CPVRGUITimesInfo::UpdateTimeshiftData()
    // substitute the current playback position for both its internal min
    // and max time, which collapses "is timeshifting supported" to false
    // and makes the on-screen seek bar's position silently fall back to
    // raw wall-clock time regardless of where a seek actually landed --
    // confirmed live via screenshots before this fix. See
    // docs/TIMESHIFT.md's "PVR.TimeshiftProgress*"/seek bar section.
    times.SetStartTime(m_client.GetLiveTimeshiftStreamWallClockAnchor());
    times.SetPTSStart(0);
    // Moves forward once the plugin's rolling buffer starts dropping
    // segments, so the seek bar stops offering history that is gone.
    times.SetPTSBegin(m_client.GetLiveTimeshiftStreamBeginMs() * 1000);
    times.SetPTSEnd(m_client.GetLiveTimeshiftStreamDurationMs() * 1000);
    return PVR_ERROR_NO_ERROR;
  }
  if (m_client.IsInProgressRecordingStreamOpen())
  {
    // Same root cause/mechanism as the live-timeshift branch above (see its
    // comment and GetInProgressRecordingStreamStartTime()'s), fixed the same
    // way -- a real, non-zero startTime here. Simpler than the live case:
    // the recording's own actual start time, not something computed from a
    // cold-start trim, since a recording always plays from true byte 0.
    times.SetStartTime(m_client.GetInProgressRecordingStreamStartTime());
    times.SetPTSStart(0);
    times.SetPTSBegin(0);
    times.SetPTSEnd(m_client.GetInProgressRecordingStreamDurationMs() * 1000);
    PersistApiKeyIfChanged(keyBefore);
    return PVR_ERROR_NO_ERROR;
  }
  return PVR_ERROR_NOT_IMPLEMENTED;
}

PVR_ERROR PVRDispatcharr::GetStreamReadChunkSize(int& chunksize)
{
  // See the declaration comment in PVRDispatcharr.h -- without this, ffmpeg
  // reads 4KB at a time from our HTTP-backed live-timeshift/recording
  // streams, which measurably stalls higher-bitrate channels. 256KB cuts
  // that to a handful of requests per second even for a ~14 Mbps stream,
  // while staying well under a single timeshift segment's typical size so a
  // read still resolves in one HTTP request in the common case.
  chunksize = 256 * 1024;
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// EPG
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetEPGForChannel(int channelUid, time_t start, time_t end,
                                           kodi::addon::PVREPGTagsResultSet& results)
{
  EnsureChannelsLoaded();
  // Never EnsureEpgLoaded() here: the guide is fetched only by the background
  // thread, and this call -- made on Kodi's PVR manager and EPG threads -- must
  // not wait on that fetch or make it (see m_epgFetchMutex's comment for the
  // live-reproduced Kodi exit that hung on it). Wake the thread if a fetch is
  // wanted and answer from the cache; the thread triggers a per-channel EPG
  // update once the guide has landed.
  RequestGuideFetchIfWanted();

  std::lock_guard<std::mutex> lock(m_dataMutex);
  // Real, confirmed bug found via a project-wide review (a 43rd-pass
  // audit), confirmed against Kodi's own real current SDK source, not
  // itself independently reproduced: returning PVR_ERROR_NO_ERROR here
  // when channels/EPG have never loaded even once told Kodi this
  // (empty) result was a genuinely completed scan, not a transient
  // startup gap. CPVREpg::Update() (Epg.cpp) only calls its own
  // UpdateEntries() (which sets/persists m_lastScanTime) when
  // UpdateFromScraper()'s own `client->GetEPGForChannel(...) ==
  // PVR_ERROR_NO_ERROR` check passes -- so a success return with zero
  // tags made Kodi wait its full advanced-settings
  // m_iEpgUpdateEmptyTagsInterval (2 hours by default) before asking
  // again for that channel, rather than its much shorter
  // m_iEpgUpdateCheckInterval (5 minutes) an error return actually
  // triggers. CPVREpg::IsValid() never looks at scan success/failure
  // (only the channel's own client id), so returning an error here
  // doesn't risk the EPG table itself being torn down. This mattered
  // most right at Kodi startup with Dispatcharr still unreachable (or
  // briefly so): a device that boots faster than its Dispatcharr
  // server, or one whose VPN/network comes up after Kodi does.
  //
  // Deliberately checked before the per-channel/per-EPG-entry lookups
  // below, not folded into their own `!ch`/cache-miss cases: a genuinely
  // unknown channelUid, or a real channel with no guide data at all
  // once channels/EPG have actually loaded at least once, are legitimate
  // "nothing to report" results, not this same startup gap -- returning
  // an error for those instead would make Kodi retry every 5 minutes,
  // forever, for a channel that will never have guide data regardless.
  if (dispatcharr::HasNeverLoadedSuccessfully(m_channelsLoadedAt) ||
      dispatcharr::HasNeverLoadedSuccessfully(m_epgLoadedAt))
    return PVR_ERROR_SERVER_ERROR;
  const Channel* ch = FindChannelByUid(channelUid);
  if (!ch)
    return PVR_ERROR_NO_ERROR;

  // The exact text Dispatcharr's own XMLTV export keys this channel by --
  // see dispatcharr::FormatChannelGuideKey() (ChannelNumber.h): the number's
  // text, or the channel's id when it has no number. Empty means nothing
  // usable, which is never looked up.
  std::string channelNumberKey = dispatcharr::ChannelGuideKey(*ch);
  if (channelNumberKey.empty())
    return PVR_ERROR_NO_ERROR;
  // See m_ambiguousChannelNumbers's own comment (PVRDispatcharr.h) for
  // the real, live-confirmed bug this guards against (2026-09-28,
  // docs/OPEN_ITEMS.md): a channel number Dispatcharr's own real
  // instance had shared by two channels, both exported under the
  // identical XMLTV <channel id>, with no way to tell which programmes
  // actually belong to which. A plain "no guide data" here (matching
  // the null-channelNumber case just above) is safer than showing a
  // possibly-wrong, possibly-merged guide.
  if (m_ambiguousChannelNumbers.count(channelNumberKey))
  {
    if (m_debugLogging)
    {
      kodi::Log(ADDON_LOG_DEBUG,
                "pvr.dispatcharr-unofficial: GetEPGForChannel: channel %d's own number (%s) is shared by another "
                "channel on this Dispatcharr instance -- refusing to guess which channel's guide data is real, "
                "reporting no guide data for this channel instead",
                channelUid, channelNumberKey.c_str());
    }
    return PVR_ERROR_NO_ERROR;
  }

  // Confirmed against a live instance: Dispatcharr's XMLTV export keys
  // <channel id="..."> by channel_number, not tvg_id (see XmlTvParser.h).
  auto it = m_epgByChannelNumber.find(channelNumberKey);
  if (it == m_epgByChannelNumber.end())
    return PVR_ERROR_NO_ERROR;

  for (const auto& entry : it->second)
  {
    if (entry.endTime < start || entry.startTime > end)
      continue;

    kodi::addon::PVREPGTag tag;
    // See ComputeBroadcastId()'s own doc comment in EpgTagUtil.h for why
    // this specific hash (full start time, multiplicative channel-id
    // mixing) rather than something simpler -- pulled out there so it's
    // unit-testable standalone.
    tag.SetUniqueBroadcastId(dispatcharr::ComputeBroadcastId(channelUid, entry.startTime));
    tag.SetUniqueChannelId(static_cast<unsigned int>(channelUid));
    tag.SetTitle(entry.title);
    tag.SetPlotOutline(entry.subtitle);
    tag.SetEpisodeName(entry.subtitle);
    tag.SetPlot(entry.description);
    tag.SetStartTime(entry.startTime);
    tag.SetEndTime(entry.endTime);
    if (!entry.iconPath.empty())
      tag.SetIconPath(entry.iconPath);
    if (!entry.cast.empty())
      tag.SetCast(entry.cast);
    if (!entry.director.empty())
      tag.SetDirector(entry.director);
    if (!entry.writer.empty())
      tag.SetWriter(entry.writer);
    // See ShouldIncludeEpisodeDates()'s own doc comment in EpgTagUtil.h for
    // why Year/FirstAired are only passed through with a real episode
    // identity -- both derive from the same XMLTV <date> element, which a
    // real instance confirmed is unreliable placeholder data without one.
    bool includeEpisodeDates = dispatcharr::ShouldIncludeEpisodeDates(entry.seasonNumber, entry.episodeNumber);
    if (entry.year > 0 && includeEpisodeDates)
      tag.SetYear(entry.year);
    if (!entry.firstAired.empty() && includeEpisodeDates)
      tag.SetFirstAired(entry.firstAired);

    if (!entry.categories.empty())
    {
      tag.SetGenreDescription(dispatcharr::JoinCategories(entry.categories, EPG_STRING_TOKEN_SEPARATOR));

      int genreType = EPG_GENRE_USE_STRING;
      dispatcharr::MapCategoriesToGenreType(entry.categories, genreType);
      tag.SetGenreType(genreType);
    }

    // >= 0, not > 0: season 0 (a "specials" episode) is a real,
    // legitimate value, not a sentinel -- XmlTvParser's own -1 default
    // is what actually means "unknown" here (matching Kodi's own
    // EPG_TAG_INVALID_SERIES_EPISODE == -1, confirmed against its real
    // SDK header; 0 is accepted as a normal season by Kodi). Fix for a
    // real, confirmed bug found via a project-wide review (a 25th-pass
    // audit), confirmed against Dispatcharr's own real current upstream
    // source, not itself independently reproduced: Dispatcharr's own
    // XMLTV export emits xmltv_ns as "-1.<ep>." for a stored season of
    // 0 (its own 0-indexed xmltv_ns convention, `season - 1`), which
    // ParseEpisodeNum() already converts back to 0 correctly -- but this
    // `> 0` check then discarded that real season-0 value, silently
    // dropping the episode's own series/episode grouping, its
    // EPG_TAG_FLAG_IS_SERIES flag, and (per ShouldIncludeEpisodeDates(),
    // unaffected here since episodeNumber alone already satisfies its
    // own `> 0` OR-check) nothing else changes there.
    if (entry.seasonNumber >= 0)
      tag.SetSeriesNumber(entry.seasonNumber);
    if (entry.episodeNumber > 0)
      tag.SetEpisodeNumber(entry.episodeNumber);

    bool categorySaysSeries = dispatcharr::CategoriesIndicateSeries(entry.categories);
    tag.SetFlags(dispatcharr::ComputeEpgTagFlags(entry.isNew, entry.isPremiere, entry.isLive, entry.seasonNumber,
                                                 entry.episodeNumber, categorySaysSeries));

    results.Add(tag);
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::IsEPGTagPlayable(const kodi::addon::PVREPGTag& tag, bool& isPlayable)
{
  isPlayable = false;
  std::lock_guard<std::mutex> lock(m_dataMutex);
  const Channel* ch = FindChannelByUid(static_cast<int>(tag.GetUniqueChannelId()));
  if (!ch)
    return PVR_ERROR_NO_ERROR;

  // The window check itself lives in dispatcharr::IsWithinCatchupWindow()
  // (EpgTagUtil.h) so it's unit-testable standalone -- see that
  // function's own comment. catchupEnabled folds in the global/per-user
  // catch-up gates first (added 2026-09-28, see
  // dispatcharr::ShouldOfferCatchup()'s own comment, EpgTagUtil.h) --
  // without this, Kodi kept offering "Play" for an already-aired
  // programme on a catch-up-capable channel even when the account itself
  // had catch-up disabled, only to have the actual playback attempt fail
  // with nothing but a debug log line explaining why (confirmed live).
  bool catchupEnabled =
      dispatcharr::ShouldOfferCatchup(ch->catchupEnabled, m_catchupEnabledGlobally, m_catchupEnabledForCurrentUser);
  isPlayable = dispatcharr::IsWithinCatchupWindow(catchupEnabled, ch->catchupDays, tag.GetStartTime(), time(nullptr));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetEPGTagStreamProperties(const kodi::addon::PVREPGTag& tag,
                                                    std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  std::string channelUuid;
  bool channelCatchupEnabled = false;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    const Channel* ch = FindChannelByUid(static_cast<int>(tag.GetUniqueChannelId()));
    if (!ch)
      return PVR_ERROR_INVALID_PARAMETERS;
    channelUuid = ch->uuid;
    channelCatchupEnabled = ch->catchupEnabled;
  }

  // Defense in depth alongside IsEPGTagPlayable()'s own gate above --
  // reachable if Kodi still calls through here on a stale "is playable"
  // decision it cached before either flag changed this session. Fails
  // with a clear, user-visible reason instead of attempting a network
  // call already known doomed to a 403 (added 2026-09-28, see
  // dispatcharr::ShouldOfferCatchup()'s own comment for the real,
  // live-confirmed bug: previously this always attempted the call
  // regardless, and the only trace of the eventual failure was a debug
  // log line).
  if (!dispatcharr::ShouldOfferCatchup(channelCatchupEnabled, m_catchupEnabledGlobally, m_catchupEnabledForCurrentUser))
  {
    kodi::QueueNotification(QUEUE_ERROR, "",
                            "Catch-up is disabled for this Dispatcharr account or instance -- ask an admin to "
                            "enable it, or turn it off in Dispatcharr's own Users settings.");
    return PVR_ERROR_REJECTED;
  }

  int durationMinutes = static_cast<int>((tag.GetEndTime() - tag.GetStartTime()) / 60);
  std::string playbackUrl, error;
  if (!m_client.CreateCatchupSession(channelUuid, tag.GetStartTime(), durationMinutes, playbackUrl, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to create catch-up session: %s", error.c_str());
    return PVR_ERROR_FAILED;
  }

  // The field-mapping itself lives in
  // dispatcharr::BuildCatchupStreamProperties() (StreamPropertyUtil.h,
  // extracted 2026-09-26 in a 24th-pass audit) so it's unit-testable
  // standalone -- see that function's own comment. Always a plain
  // streamurl+isrealtimestream=false+mimetype; the
  // enable_catchup_ffmpegdirect_seek pair below is added only when that
  // setting is on.
  //
  // Third attempt at improving catch-up seek reliability, after the two
  // documented further down (still kept here for history) were tried and
  // reverted -- this one deliberately sets neither ffmpegdirect property
  // either of those set. Confirmed via ffmpegdirect's own source
  // (StreamManager.cpp's Open()): leaving
  // "inputstream.ffmpegdirect.stream_mode" unset at all (neither "catchup"
  // nor "timeshift") makes it instantiate the plain FFmpegStream class
  // instead of FFmpegCatchupStream or TimeshiftStream -- the same base
  // class this addon already routes in-progress-recording playback
  // through. Its SeekTime() calls libavformat's own av_seek_frame() against
  // the mpegts demuxer directly, rather than the generic
  // CCurlFile-plus-bitrate-estimate seek Kodi-core falls back to on its own
  // (byte offset computed first from duration/filesize outside any
  // format-specific logic, then handed to FFmpeg to resync) when no
  // PVR_STREAM_PROPERTY_INPUTSTREAM is set at all -- the plain STREAMURL
  // path set above, still what plays when this setting is off.
  //
  // Requires the separate inputstream.ffmpegdirect addon to actually be
  // installed; if it isn't, this would fail to open the stream at all, so
  // it's opt-in (enable_catchup_ffmpegdirect_seek, default off) rather than
  // silently changed for everyone.
  //
  // Verified live against a real instance, both directions, several times:
  // seeks land precisely (within ~10-15s of the requested target, e.g. a
  // seek to 18:20 landing at 18:09, one to 5:00 landing at 5:15) and
  // playback resumes and continues normally afterward -- a real
  // improvement over the plain-STREAMURL path's known-imprecise byte-
  // estimation seeking.
  //
  // open_mode is deliberately left unset, deferring to ffmpegdirect's own
  // DEFAULT-mode detection, which lands on OpenMode::CURL for a plain
  // http:// URL with this mimetype. A companion session's macOS testing
  // found a credible explanation for this path's intermittent
  // multi-second-to-85+-second seek latency: OpenMode::CURL means
  // ffmpegdirect's I/O still goes through Kodi-core's own CCurlFile/
  // CFileCache rather than an independent connection, and a real macOS log
  // showed libavformat's mpegts demuxer's normal PCR-probe seek algorithm
  // (~15-30 probe-and-adjust reads, expected for a format with no real
  // index) paying CFileCache's own "cache completely reset for seek to
  // position X" cost on every single probe before the seek finally landed.
  // Forcing open_mode to "ffmpeg" was tried as the fix (matching the
  // in-progress-recording HLS path, for the same reasoning: bypass
  // Kodi-core's cache layer by having FFmpeg's own native http:// protocol
  // handler own the I/O instead) -- and made things measurably worse in
  // direct, patient live testing on Windows, not better. A forward seek
  // that would typically land within ~10-20s under CURL mode (worst case
  // observed: 85+s) instead sat completely unmoved for nearly 5 minutes
  // (280s) under forced "ffmpeg" mode before finally landing 68 seconds off
  // target (21:08 for a 20:00 request) -- both slower to resolve and less
  // precise once it did, confirmed via patient polling specifically
  // designed not to repeat the mistake of giving up too early (a first,
  // shorter attempt at this same test was called "stuck" after only 60s,
  // which in hindsight wasn't long enough to tell the difference between
  // "slow" and "actually stuck" -- a real lesson from this investigation:
  // this path's seeks need patience on the order of minutes, not seconds,
  // before concluding anything). Reverted for that reason and left here as
  // a documented dead end -- the *diagnosis* of why CURL mode is
  // occasionally slow is still credible, but this particular fix for it
  // isn't, so a future attempt shouldn't retry forcing "ffmpeg" mode
  // without knowing it was already tried and made things worse.
  for (const auto& p : dispatcharr::BuildCatchupStreamProperties(playbackUrl, m_enableCatchupFfmpegdirectSeek))
    properties.emplace_back(p.name, p.value);

  // PVR_STREAM_PROPERTY_EPGPLAYBACKASLIVE was tried here too (a plain
  // Kodi-core flag, unrelated to ffmpegdirect) to make the OSD feel more
  // like live TV. Reverted: setting it makes Kodi re-route playback through
  // GetChannelStreamProperties() -- the *live-channel* path -- instead of
  // just using the catch-up URL returned here, which isn't what a static,
  // already-complete archived file needs and broke seeking further.
  //
  // inputstream.ffmpegdirect was tried here (both "timeshift" and "catchup"
  // stream_mode) to address unreliable seeking, then reverted after
  // confirming via its actual source (src/stream/TimeshiftBuffer.cpp,
  // src/stream/FFmpegCatchupStream.cpp) that neither mode's seek model
  // matches how Dispatcharr's catch-up API actually works:
  //   - "timeshift" mode locally records and segments what it assumes is a
  //     *live*, continuously-arriving source, then seeks only within what
  //     it has already recorded itself -- our catch-up URL is instead a
  //     single, already-complete archived file.
  //   - "catchup" mode seeks by reconstructing a *new* URL for the exact
  //     wall-clock time being sought to (FFmpegCatchupStream::
  //     SeekCatchupStream -> GetUpdatedCatchupUrl()), which requires the
  //     backend to support starting playback from an arbitrary in-programme
  //     timestamp. Dispatcharr's own docs are explicit that its catch-up
  //     `start` parameter only selects *which programme* to fetch, not a
  //     time within it -- in-programme seeking is meant to happen via plain
  //     HTTP Range on the byte stream, which is exactly what Kodi's default
  //     player already does (see docs/API_NOTES.md for why that's still
  //     imprecise for raw MPEG-TS, and why this was worth investigating).
  // Confirmed live with a real install: "timeshift" mode didn't just fail
  // to improve seeking, it broke it entirely (no seeking at all), which
  // fits -- it isn't merely suboptimal for this URL shape, it's the wrong
  // mechanism for it.
  return PVR_ERROR_NO_ERROR;
}

// ---------------------------------------------------------------------
// Recordings
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetRecordingsAmount(bool deleted, int& amount)
{
  if (deleted)
  {
    amount = 0; // Dispatcharr recording trash/undelete not implemented here
    return PVR_ERROR_NO_ERROR;
  }
  // Deliberately NOT given the same PVR_ERROR_SERVER_ERROR-on-never-loaded
  // guard as GetChannels()/GetTimers() (a 40th-pass audit; see
  // GetTimersAmount()'s own comment on the general mechanism, and
  // dispatcharr::HasNeverLoadedSuccessfully()'s, Staleness.h): confirmed
  // against Kodi's own real current SDK source that it would have no
  // effect here even if added.
  //
  // Corrected 2026-09-27, a 50th-pass audit, fixing a real, confirmed
  // inaccuracy in this same comment's own original (9th-pass) reasoning,
  // found via a project-wide review, confirmed against Kodi's own real
  // current SDK source, not itself independently reproduced: the
  // original version of this comment said the `deleted` branch above
  // "always returns PVR_ERROR_NO_ERROR unconditionally", implying that's
  // what lets a second CPVRRecordings::UpdateFromClients() call
  // (PVRRecordings.cpp) wipe out a real failure the first call recorded.
  // That branch's own return value is irrelevant, because it never runs
  // at all: CPVRClient::GetRecordingsAmount()/GetRecordings() (PVRClient.cpp)
  // gate the `deleted=true` call on m_clientCapabilities.SupportsRecordingsUndelete()
  // -- never declared true anywhere in this addon (only
  // SetSupportsRecordings(true) is) -- via DoAddonCall()'s own
  // `bIsImplemented` parameter, which short-circuits to
  // PVR_ERROR_NOT_IMPLEMENTED *before* ever calling into this addon's own
  // GetRecordingsAmount()/GetRecordings() for that call. And
  // CPVRClients::ForClients() (PVRClients.cpp) explicitly excludes
  // PVR_ERROR_NOT_IMPLEMENTED from counting as a failure in the first
  // place (`currentError != PVR_ERROR_NO_ERROR && currentError !=
  // PVR_ERROR_NOT_IMPLEMENTED`) -- so even setting aside that this
  // addon's own code for that branch is unreachable, returning anything
  // else from it wouldn't change what counts as a failure either. The
  // conclusion this comment reaches (no effect from adding the guard
  // here) still holds, just for this simpler, more fundamental reason:
  // ForClients() unconditionally clears its own failedClients vector on
  // entry to each call regardless of what either call actually returns,
  // so the second call always wins -- unlike CPVRTimers::Update(), which
  // also makes two such calls (UpdateTimerTypes() then GetTimers()) but
  // where the LAST one is what survives, and this addon's own
  // GetTimers() is that last call.
  EnsureRecordingsLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // In-progress recordings belong here too, not just upcoming/scheduled
  // ones excluded below -- Kodi's own CPVRRecording::IsInProgress() cross-
  // references GetRecordings() against the active timer list by
  // channel+time overlap to decide whether a *listed recording* is still
  // being written, and that's also what makes it clickable/playable while
  // recording. Omitting in-progress ones here (as an earlier version of
  // this code did) made them show up only as an uneditable timer entry,
  // with nothing to actually click and play.
  amount = static_cast<int>(
      std::count_if(m_cachedRecordings.begin(), m_cachedRecordings.end(), dispatcharr::IsListedAsRecording));
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
  if (deleted)
    return PVR_ERROR_NO_ERROR;

  EnsureRecordingsLoaded();
  // Copied out under the lock, then processed lock-free below -- matches
  // GetTimers()'s own established pattern (see its own comment), needed
  // here now that this loop also calls ResolveRecordingBroadcastId(),
  // which takes m_dataMutex itself and would otherwise deadlock against
  // this same, non-recursive mutex still held for the whole loop.
  std::vector<Recording> recordings;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    recordings = m_cachedRecordings;
    // Forget links for recordings that no longer exist -- but only against a list
    // that really loaded, so a failed refresh never wipes the lot.
    if (m_recordingsEverLoaded)
    {
      std::unordered_set<int> liveIds;
      for (const auto& rec : recordings)
        liveIds.insert(rec.id);
      if (dispatcharr::PruneRecordingEpgLinks(m_recordingEpgLinks, liveIds) > 0)
        m_recordingEpgLinksDirty = true;
    }
  }
  for (const auto& rec : recordings)
  {
    // Only a not-yet-started recording has nothing to play at all; skip
    // that case. In-progress ones belong here too (see
    // GetRecordingsAmount() above for why) -- must match its filter, via
    // the same dispatcharr::IsListedAsRecording() (RecordingVisibility.h).
    if (!dispatcharr::IsListedAsRecording(rec))
      continue;
    kodi::addon::PVRRecording recording;
    recording.SetRecordingId(std::to_string(rec.id));
    recording.SetTitle(rec.title);
    // Groups recordings into a per-show folder in Kodi's own recordings UI.
    // rec.title is already the show name, not an episode-specific one --
    // confirmed against Dispatcharr's own source: the exact same
    // custom_properties.program.title read that populates this field is
    // also, verbatim, what Dispatcharr itself uses as the show-folder
    // path segment when it writes the file to disk (apps/channels/
    // tasks.py's _build_output_paths) -- but only after that function's
    // own _safe_name() sanitization, which dispatcharr::
    // SanitizeRecordingDirectory() (RecordingDirectory.h) now mirrors
    // (added 2026-09-26, a 21st-pass audit, fixing a real, confirmed
    // bug: a raw title containing '/' -- e.g. "Face/Off", "20/20" --
    // turned SetDirectory() into a *nested* Kodi folder instead of
    // Dispatcharr's own single flat one, confirmed against Kodi's real
    // source, CPVRRecordingsPath treats a raw '/' in Directory as a path
    // separator). Never empty -- rec.title already falls back to
    // "Recording <id>" server-side when nothing else is available, and
    // SanitizeRecordingDirectory() has its own "Recording" fallback for
    // the rare case sanitizing consumes the whole title -- so a
    // one-off/unmatched recording still gets its own single-item folder
    // rather than an empty Directory, matching normal Kodi PVR/
    // video-library grouping conventions.
    recording.SetDirectory(dispatcharr::SanitizeRecordingDirectory(rec.title));
    recording.SetEpisodeName(rec.subtitle);
    recording.SetPlot(rec.description);
    recording.SetChannelUid(rec.channelId > 0 ? rec.channelId : PVR_CHANNEL_INVALID_UID);
    recording.SetRecordingTime(rec.startTime);
    recording.SetDuration(rec.durationSeconds);
    recording.SetSizeInBytes(rec.bytesWritten);
    recording.SetIsDeleted(false);
    // Links this recording back to the actual EPG tag it was recorded
    // from -- fix for a real, confirmed bug found via a project-wide
    // review (a 34th-pass audit, confirmed against Kodi's own real
    // current SDK source, not itself independently reproduced): without
    // SetEPGEventId(), Kodi's own CPVRRecordings::GetRecordingForEpgTag()
    // (a real consumer, not a lookalike -- it backs LISTITEM_HASRECORDING,
    // the "Play recording" context menu entry, the guide's past-event
    // click behavior, and JSON-RPC's FileItemHandler) falls back to
    // requiring this recording's own RecordingTimeAsUTC()/EndTimeAsUTC()
    // to fully cover the EPG tag's own window -- which the pass-32
    // start-time clamp, and this addon's own stopped-early/interrupted
    // duration shortening (ParseRecordingFields()'s own stopped_at/
    // ended_at handling), routinely leave it unable to do once the
    // recording has actually finished. Uses the exact same matching
    // strategy GetTimers() already applies to a still-recording timer
    // (ResolveRecordingBroadcastId(), shared by both) -- see its own
    // comment, and docs/OPEN_ITEMS.md, for the one durability gap this
    // doesn't close: a match here depends on the EPG entry still being
    // present in this addon's own cached guide, which drops an
    // already-ended entry at its own next XMLTV refresh.
    unsigned int broadcastId = ResolveRecordingBroadcastId(rec);
    if (broadcastId != EPG_TAG_INVALID_UID)
      recording.SetEPGEventId(broadcastId);
    results.Add(recording);
  }
  SaveRecordingEpgLinksIfDirty();
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::FindRecordingById(int id, dispatcharr::Recording& recordingOut, long* httpStatusOut)
{
  std::string error;
  return m_client.GetRecordingById(id, recordingOut, error, httpStatusOut);
}

void PVRDispatcharr::InvalidateAndTriggerRecordingUpdate()
{
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_recordingsCachedAt = {};
    ++m_recordingsGeneration;
  }
  TriggerRecordingUpdate();
}

void PVRDispatcharr::InvalidateAndTriggerTimerUpdate()
{
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_recordingsCachedAt = {};
    m_seriesRulesCachedAt = {};
    m_recurringRulesCachedAt = {};
    ++m_recordingsGeneration;
    ++m_seriesRulesGeneration;
    ++m_recurringRulesGeneration;
  }
  TriggerTimerUpdate();
}

void PVRDispatcharr::PersistApiKeyIfChanged(const std::string& keyBefore)
{
  std::string keyAfter = m_client.GetApiKey();
  if (keyAfter == keyBefore)
    return;
  {
    std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
    m_lastAppliedConfig.apiKey = keyAfter;
  }
  kodi::addon::SetSettingString("api_key", keyAfter);
  // Read back: while this addon's settings dialog is open Kodi keeps the write in the dialog's pending value
  // instead of storing it (docs/CLOSED_ITEMS.md, "SetSetting*() swallowed while the settings dialog is open").
  // Remembering what is still stored lets the key Kodi re-delivers after a cancelled dialog be told from a
  // user's edit, and lets ReassertApiKeySetting() write it again once the dialog is gone.
  {
    const std::string stored = kodi::addon::GetSettingString("api_key");
    std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
    if (stored == keyAfter)
    {
      m_apiKeyStoredAfterDroppedWrite.clear();
    }
    else
    {
      m_apiKeyStoredAfterDroppedWrite = stored;
      kodi::Log(ADDON_LOG_INFO,
                "pvr.dispatcharr-unofficial: the new API key was not stored (the settings dialog is probably open) "
                "-- will write it again");
    }
  }
  // api_key_host/api_key_username (added 2026-09-27, a 45th-pass audit):
  // recorded alongside every real key change, not just the constructor's
  // own first-generation case, so a *later* host/username change is
  // still correctly detected as an owner mismatch by the next instance's
  // own constructor -- see its own comment.
  //
  // Stamped from m_apiKeyOwnerHost/m_apiKeyOwnerUsername, deliberately
  // NOT m_lastAppliedConfig.host/.username (fixed 2026-09-27, a 46th-pass
  // audit, correcting a real, confirmed bug in the fix above found via a
  // project-wide review, confirmed against Kodi's own real current
  // source, not itself independently reproduced -- see
  // m_apiKeyOwnerHost's own comment, PVRDispatcharr.h, for the full
  // account): m_lastAppliedConfig.host/.username are written with no lock
  // by OnAddonSettingChanged() on the settings/GUI thread -- a genuine
  // data race against this read from whatever thread calls
  // OpenRecordedStream()/ReadRecordedStream()/etc. -- and, even ignoring
  // the race, can already reflect a *newer*, not-yet-applied
  // host/username change while this (old, not-yet-destroyed) instance's
  // own m_client is still actually using the previous one, for however
  // long Kodi's blocking NEED_RESTART OK dialog stays up.
  // m_apiKeyOwnerHost/m_apiKeyOwnerUsername are `const`, snapshotting
  // what m_client was actually constructed with -- always correct for
  // this instance's entire lifetime, safe to read from any thread with
  // no lock at all.
  kodi::addon::SetSettingString("api_key_host", m_apiKeyOwnerHost);
  kodi::addon::SetSettingString("api_key_username", m_apiKeyOwnerUsername);
}

void PVRDispatcharr::ReassertApiKeySetting()
{
  std::string stale;
  {
    std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
    stale = m_apiKeyStoredAfterDroppedWrite;
  }
  if (stale.empty())
    return;

  const std::string current = m_client.GetApiKey();
  switch (dispatcharr::DecideApiKeyStoreAction(kodi::addon::GetSettingString("api_key"), current, stale))
  {
  case dispatcharr::ApiKeyStoreAction::kNothingToDo:
  case dispatcharr::ApiKeyStoreAction::kStop:
  {
    // Landed meanwhile, or the user entered a key of their own (its restart path handles that).
    std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
    if (m_apiKeyStoredAfterDroppedWrite == stale)
      m_apiKeyStoredAfterDroppedWrite.clear();
    return;
  }
  case dispatcharr::ApiKeyStoreAction::kRewrite:
    break;
  }
  kodi::addon::SetSettingString("api_key", current);
  if (kodi::addon::GetSettingString("api_key") == current)
  {
    std::lock_guard<std::mutex> apiKeyLock(m_lastAppliedApiKeyMutex);
    if (m_apiKeyStoredAfterDroppedWrite == stale)
      m_apiKeyStoredAfterDroppedWrite.clear();
    kodi::Log(ADDON_LOG_INFO, "pvr.dispatcharr-unofficial: the API key setting is stored now");
  }
}

PVR_ERROR PVRDispatcharr::GetRecordingStreamProperties(const kodi::addon::PVRRecording& recording,
                                                       std::vector<kodi::addon::PVRStreamProperty>& properties)
{
  // Confirmed against a real failed playback (a live kodi.log showed
  // Kodi's generic CCurlFile opening a populated STREAMURL directly,
  // bypassing this addon's own OpenRecordedStream() entirely, including
  // its 401-retry/API-key-regen logic) that leaving STREAMURL unset is
  // what actually forces Kodi through CInputStreamPVRRecording's
  // OpenRecordedStream()/ReadRecordedStream()/etc. below -- true for a
  // completed recording, and, since the growing-buffer approach proven
  // for live-timeshift replaced the old ffmpegdirect-routed in-progress
  // mechanism, now equally true for an in-progress one: see
  // DispatcharrClient::OpenInProgressRecordingStream()'s comment for why
  // this no longer needs its own STREAMURL/inputstream.ffmpegdirect
  // properties at all.
  bool isRealTime = false;
  {
    int id = std::atoi(recording.GetRecordingId().c_str());
    Recording rec;
    if (FindRecordingById(id, rec))
    {
      // Same predicate OpenRecordedStream() uses to choose the
      // growing-buffer read path -- dispatcharr::ShouldUseGrowingBufferPlayback()
      // (RecordingVisibility.h) -- since a recording still on that path is
      // still "real-time" in the sense Kodi cares about here. See that
      // function's own comment for hlsDirStillPresent alongside
      // isInProgress: a just-stopped recording still needs it for the
      // whole window until Dispatcharr's own HLS-to-MKV concat finishes.
      isRealTime = dispatcharr::ShouldUseGrowingBufferPlayback(rec);
    }
  }
  properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, isRealTime ? "true" : "false");
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  if (!m_client.DeleteRecording(id, error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to delete recording %d: %s", id, error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  // Timers before recordings -- see HandleRealtimeUpdateMessage()'s own
  // comment on why the order matters (fixed here 2026-09-27, a 42nd-pass
  // audit, fixing a real, confirmed gap the 41st-pass fix left open,
  // found via a project-wide review, confirmed against Kodi's own real
  // current SDK source, not itself independently reproduced): the latch
  // bug that fix closed can strike *any* recording whose corresponding
  // Kodi timer hasn't caught up yet, not just the one this call itself
  // acted on -- a recordings-only refresh triggered here is exactly as
  // capable of exposing a *different*, freshly-started recording to
  // Kodi's own ambient IsInProgress() check before its own timer update
  // has separately caught up.
  InvalidateAndTriggerTimerUpdate();
  InvalidateAndTriggerRecordingUpdate();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::RenameRecording(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  if (!m_client.RenameRecording(id, recording.GetTitle(), error))
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to rename recording %d: %s", id, error.c_str());
    // Real, confirmed bug found via a project-wide review (a 48th-pass
    // audit), confirmed against Kodi's own real current SDK source, not
    // itself independently reproduced: CPVRRecording::Rename()
    // (PVRRecording.cpp) sets its own `m_strTitle` to the new name
    // *before* ever calling into this addon, unconditionally -- so a
    // failed rename here still leaves Kodi displaying the new, never-
    // actually-applied title, with nothing correcting it: the caller
    // (AsyncRecordingAction::Run(), PVRGUIActionsRecordings.cpp) only
    // triggers a recordings refresh on *success*. A recordings refresh
    // here corrects the wrongly-optimistic in-memory title back to the
    // real server-side value promptly instead of leaving it wrong until
    // some unrelated refresh happens to occur.
    //
    // Timers before recordings here too (added 2026-09-27, a 50th-pass
    // audit, fixing a real, confirmed gap in this same 48th-pass fix,
    // found via a project-wide review, confirmed against Kodi's own real
    // current SDK source, not itself independently reproduced): this
    // failure path was added after the "timers before recordings" rule
    // below (`DeleteRecording()`'s own comment) already existed, and
    // missed it -- a recordings-only refresh here is exactly as capable
    // of exposing a *different*, freshly-started recording to Kodi's own
    // ambient `IsInProgress()` latch check (`CPVRRecording::IsInProgress()`,
    // `PVRRecording.cpp`) before that recording's own timer update has
    // separately caught up, same as `DeleteRecording()`'s own comment
    // already documents.
    InvalidateAndTriggerTimerUpdate();
    InvalidateAndTriggerRecordingUpdate();
    return PVR_ERROR_SERVER_ERROR;
  }
  // See DeleteRecording()'s own comment above.
  InvalidateAndTriggerTimerUpdate();
  InvalidateAndTriggerRecordingUpdate();
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetRecordingEdl(const kodi::addon::PVRRecording& recording,
                                          std::vector<kodi::addon::PVREDLEntry>& edl)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::vector<RecordingEdlEntry> entries;
  std::string error;
  if (!m_client.GetRecordingEdl(id, entries, error))
  {
    // Not installing the companion recording_edl plugin is an entirely
    // normal, expected configuration (unlike the timeshift plugin, this
    // one has no setting gating it, so most installs simply won't have
    // it) -- log at DEBUG rather than ERROR so declining to install an
    // optional plugin doesn't read as a real problem in the log.
    kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: no EDL for recording %d: %s", id, error.c_str());
    return PVR_ERROR_NO_ERROR; // empty edl -- not a failure, just nothing to show
  }
  for (const auto& entry : entries)
  {
    kodi::addon::PVREDLEntry e;
    e.SetStart(entry.startMs);
    e.SetEnd(entry.endMs);
    e.SetType(static_cast<PVR_EDL_TYPE>(entry.type));
    edl.emplace_back(std::move(e));
  }
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::OpenRecordedStream(const kodi::addon::PVRRecording& recording)
{
  int id = std::atoi(recording.GetRecordingId().c_str());
  std::string error;
  std::string keyBefore = m_client.GetApiKey();

  // Real, confirmed bug found via a project-wide review (a 44th-pass
  // audit), confirmed against Dispatcharr's own real current upstream
  // source, not itself independently reproduced: an empty API key never
  // self-heals via any of the 401-triggered regenerate-and-retry paths
  // elsewhere in DispatcharrClient.cpp (OpenRecordingStream()/
  // ReadRecordingStream()/FetchRawInProgressPlaylist()), because
  // Dispatcharr's own RecordingViewSet `file`/`hls` actions use
  // `AllowAny` at the DRF permission-class level specifically so an
  // unauthenticated request reaches `_user_can_play_recording()` at all --
  // which then returns **403** for a request with no credentials, not
  // 401 (401 is only what a *bad*, non-empty key gets from
  // ApiKeyAuthentication's own AuthenticationFailed). The key can end up
  // permanently empty on a fresh install if the constructor's own
  // initial Login() attempt (see its own comment) happened to fail --
  // e.g. Dispatcharr not yet reachable at Kodi startup, the same
  // scenario the 43rd-pass channels/EPG fix addressed -- since nothing
  // else in this addon ever revisits "no key yet" afterward. Without
  // this, every recording (completed and in-progress alike) failed to
  // open for the entire lifetime of that addon instance.
  //
  // !m_apiKeyOwnershipVerified (added 2026-09-27, a 46th-pass audit,
  // fixing a real, confirmed gap in the 45th-pass account-mismatch fix,
  // found via a project-wide review, not itself independently
  // reproduced): the constructor's own api_key_host/api_key_username
  // comparison (moved to run unconditionally as of a 47th-pass fix --
  // see its own comment in the constructor) only actually resolves a
  // detected mismatch if ObtainApiKey() itself also succeeds -- if the
  // initial login fails (this same startup-timing class of gap again) or
  // that recovery attempt itself fails, m_apiKeyOwnershipVerified
  // stays false and a stale, wrong-account key never gets re-checked for
  // the rest of the session otherwise, since keyBefore.empty() alone
  // can't detect a *non-empty*, merely wrong-account key. Retried here
  // on every OpenRecordedStream() call until it actually succeeds, the
  // same self-correcting convention as the plain-empty-key case just
  // above.
  if (keyBefore.empty() || !m_apiKeyOwnershipVerified)
  {
    std::string genKey, genError;
    if (m_client.ObtainApiKey(genKey, genError))
      m_apiKeyOwnershipVerified = true;
    else
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to obtain an API key before opening recording: %s",
                genError.c_str());
  }

  // Check current in-progress status directly rather than trusting
  // GetRecordingStreamProperties()'s own check from moments earlier: a
  // recording that finishes in the gap between that call and this one
  // should still open correctly either way (both paths handle a
  // recording that finishes mid-session -- OpenRecordingStream() simply
  // isn't the right one to have started with if it was in progress right
  // now).
  Recording rec; // stays default-constructed (isInProgress/hlsDirStillPresent both false) if not found
  FindRecordingById(id, rec);

  // The decision itself lives in dispatcharr::ShouldUseGrowingBufferPlayback()
  // (RecordingVisibility.h, shared with GetRecordingStreamProperties()'s
  // own check) so it's unit-testable standalone -- see that function's
  // own comment, including the real reported bug this guards against.
  bool useGrowingBuffer = dispatcharr::ShouldUseGrowingBufferPlayback(rec);

  kodi::Log(ADDON_LOG_DEBUG,
            "pvr.dispatcharr-unofficial: OpenRecordedStream: rawId=%s parsedId=%d inProgress=%d "
            "hlsDirStillPresent=%d",
            recording.GetRecordingId().c_str(), id, rec.isInProgress ? 1 : 0, rec.hlsDirStillPresent ? 1 : 0);
  bool opened = useGrowingBuffer ? m_client.OpenInProgressRecordingStream(id, recording.GetRecordingTime(), error)
                                 : m_client.OpenRecordingStream(id, error);
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: OpenRecordedStream: opened=%d isInProgressStreamOpen=%d",
            opened ? 1 : 0, m_client.IsInProgressRecordingStreamOpen() ? 1 : 0);
  if (!opened)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to open recording %d: %s", id, error.c_str());
    // Persist here too (added 2026-09-27, a 46th-pass audit, fixing a
    // real, confirmed gap found via a project-wide review, not itself
    // independently reproduced): a key freshly (re)generated just above,
    // before this open attempt, is already live server-side and in
    // m_client's own in-memory config regardless of whether the open
    // itself then succeeds -- if the open fails for an unrelated reason
    // (the recording genuinely doesn't exist, a transient network issue)
    // and this returns early without persisting, the next restart would
    // still see the *old*, already-invalidated key on disk and
    // needlessly regenerate yet again.
    PersistApiKeyIfChanged(keyBefore);
    return false;
  }
  // OpenRecordingStream()/OpenInProgressRecordingStream() may have silently
  // regenerated the API key (see OpenRecordingStream()'s comment) if
  // another Kodi install using this same Dispatcharr account had
  // invalidated the one persisted here. Save the new one so a restart of
  // this install doesn't immediately invalidate it again -- but update
  // m_lastAppliedConfig.apiKey first (see PersistApiKeyIfChanged()'s own
  // comment): the stream above already opened successfully with the new
  // key live in DispatcharrClient's own m_config.apiKey, so persisting it
  // is purely for durability, not something this already-open stream
  // needs a restart to pick up. Confirmed live as a real bug without
  // this: it tore down the very stream that had just opened.
  PersistApiKeyIfChanged(keyBefore);
  return true;
}

void PVRDispatcharr::CloseRecordedStream()
{
  kodi::Log(ADDON_LOG_DEBUG, "pvr.dispatcharr-unofficial: CloseRecordedStream: isInProgressStreamOpen=%d",
            m_client.IsInProgressRecordingStreamOpen() ? 1 : 0);
  if (m_client.IsInProgressRecordingStreamOpen())
    m_client.CloseInProgressRecordingStream();
  else
    m_client.CloseRecordingStream();
}

int PVRDispatcharr::ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
  // Same self-heal persistence as the completed-recording branch below --
  // including updating m_lastAppliedConfig.apiKey first, for the same
  // reason: the key can also be invalidated mid-playback by another
  // install, not just between opens, and this already-open stream
  // doesn't need a restart to keep using the new one. A real, confirmed
  // gap this fixes (found via a project-wide review): this branch used
  // to return early with no key-persist wrapper at all, even though
  // FetchRawInProgressPlaylist()'s own 401 handling and
  // ReadInProgressRecordingStream()'s own segment-fetch 401 handling
  // both independently regenerate the key from deep inside this same
  // call -- so a regeneration triggered during in-progress playback
  // specifically was silently never saved, unlike the identical
  // scenario during completed-recording playback.
  std::string keyBefore = m_client.GetApiKey();
  int result = m_client.IsInProgressRecordingStreamOpen() ? m_client.ReadInProgressRecordingStream(buffer, size)
                                                          : m_client.ReadRecordingStream(buffer, size);
  PersistApiKeyIfChanged(keyBefore);
  return result;
}

int64_t PVRDispatcharr::SeekRecordedStream(int64_t position, int whence)
{
  // Same self-heal persistence gap as ReadRecordedStream()/
  // LengthRecordedStream() above (see their own comment) --
  // SeekInProgressRecordingStream()'s own SEEK_END branch calls
  // RefreshInProgressRecordingManifest() directly, which can trigger the
  // same proactive key-regeneration self-heal. Found via a project-wide
  // review, not reproduced live; low severity, same as GetStreamTimes()'s
  // own identical gap -- self-healing, just an extra round trip on this
  // install's next restart if left unpersisted.
  if (m_client.IsInProgressRecordingStreamOpen())
  {
    std::string keyBefore = m_client.GetApiKey();
    int64_t result = m_client.SeekInProgressRecordingStream(position, whence);
    PersistApiKeyIfChanged(keyBefore);
    return result;
  }
  return m_client.SeekRecordingStream(position, whence);
}

int64_t PVRDispatcharr::LengthRecordedStream()
{
  // Same self-heal persistence gap as ReadRecordedStream() above --
  // GetInProgressRecordingStreamLength() also calls
  // RefreshInProgressRecordingManifest() internally, which can trigger
  // the same proactive key-regeneration self-heal.
  std::string keyBefore = m_client.GetApiKey();
  int64_t result = m_client.IsInProgressRecordingStreamOpen() ? m_client.GetInProgressRecordingStreamLength()
                                                              : m_client.GetRecordingStreamLength();
  PersistApiKeyIfChanged(keyBefore);
  return result;
}

// ---------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------

PVR_ERROR PVRDispatcharr::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
  kodi::addon::PVRTimerType oneTime;
  oneTime.SetId(kTimerTypeOneTime);
  // No PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH on either one-time type: only
  // the series type reads GetEPGSearchString() (AddTimer()/UpdateTimer()), so
  // on these two it only made Kodi's "Search guide for" field show up as a
  // dead field. Confirmed against Kodi's own source that nothing else reads
  // the flag for a one-time type: CPVRTimerInfoTag::CreateFromEpg() consults
  // it only on its repeating-rule branch (`bCreateRule`), so the guide's
  // "Record" button, which takes the one-time branch, is unaffected; and
  // CPVRTimerRuleMatcher uses it only to match a repeating rule's own search
  // text against the guide.
  oneTime.SetAttributes(PVR_TIMER_TYPE_IS_MANUAL | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                        PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME);
  oneTime.SetDescription("One-time recording (manual)");
  types.push_back(oneTime);

  // Separate from the manual type above: Kodi's own "Record" button on an
  // EPG guide entry (CGUIDialogPVRGuideInfo::OnClickButtonRecord /
  // PVRContextMenus.cpp's StartRecording) creates a timer via
  // CPVRTimerInfoTag::CreateFromEpg(), which explicitly searches for a
  // timer type WITHOUT PVR_TIMER_TYPE_IS_MANUAL and WITHOUT
  // PVR_TIMER_TYPE_IS_REPEATING (confirmed in Kodi's own source,
  // xbmc/pvr/timers/PVRTimerInfoTag.cpp). The one-time type above has
  // IS_MANUAL set (needed for Kodi's separate "add a manual timer with no
  // EPG event" flow), and the series type below has IS_REPEATING set, so
  // neither one qualifies -- without this type, CreateFromEpg() always
  // returned null and "Record" from the guide could never create a real
  // timer. AddTimer()/DeleteTimer() already branch on kTimerTypeSeries and
  // kTimerTypeRecurring separately, falling through to one-time handling
  // for anything else -- this type isn't either of those, so no other
  // code needed to change for it to work.
  kodi::addon::PVRTimerType oneTimeEpg;
  oneTimeEpg.SetId(kTimerTypeOneTimeEpgBased);
  oneTimeEpg.SetAttributes(PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_TIME |
                           PVR_TIMER_TYPE_SUPPORTS_END_TIME);
  oneTimeEpg.SetDescription("One-time recording (from guide)");
  types.push_back(oneTimeEpg);

  kodi::addon::PVRTimerType series;
  series.SetId(kTimerTypeSeries);
  // PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL added 2026-09-26 (a 30th-pass
  // audit, fixing a real, confirmed bug found via a project-wide review,
  // confirmed against Kodi's own real current SDK source, not itself
  // independently reproduced): a channel-less series rule is real and
  // reachable in practice (Dispatcharr's own "Record series" Guide
  // button creates one -- see GetTimers()'s own comment), sent to Kodi
  // with SetClientChannelUid(PVR_CHANNEL_INVALID_UID). Without this
  // flag, Kodi's own GUIDialogPVRTimerSettings -- confirmed in its real
  // source -- never initializes m_channel for an *existing* timer whose
  // uid is already PVR_CHANNEL_INVALID_UID unless the type declares this
  // (its "select first regular channel entry" fallback only applies to a
  // brand-new timer, not an edit): m_channel stayed at its own
  // default-constructed ChannelDescriptor(), clientId -1, which
  // CPVRTimerInfoTag::UpdateOnClient() then failed to resolve to any
  // real PVR client at all -- silently failing the whole edit
  // ("Could not update the timer") before this addon's own UpdateTimer()
  // was ever called, for every field, not just the channel. See
  // UpdateTimer()'s own comment for the second bug this exposes once
  // fixed.
  series.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                       PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL | PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH |
                       PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES);
  series.SetDescription("Record series (via Dispatcharr series rule)");
  // Maps to Dispatcharr's SeriesRuleRequest.mode ("all" vs "new") --
  // confirmed against the live schema. Kodi shows this as a normal
  // per-timer setting when creating/editing a series rule.
  series.SetPreventDuplicateEpisodes({{0, "Record all episodes"}, {1, "Record only new episodes"}}, 0);
  types.push_back(series);

  // Backed by Dispatcharr's own RecurringRecordingRule model/scheduler
  // (see DispatcharrClient::CreateRecurringRule()) -- a fixed weekly
  // time-of-day pattern, not EPG-title matching, so no
  // SUPPORTS_TITLE_EPG_MATCH here (unlike the series type above).
  // SUPPORTS_ENABLE_DISABLE is now wired up via UpdateTimer() ->
  // DispatcharrClient::UpdateRecurringRule() (a pre-existing gap when
  // this addon didn't implement UpdateTimer() at all -- see
  // docs/RECORDINGS.md's UpdateTimer entry for how that was closed);
  // deleting the timer (DeleteTimer()) remains the way to stop one for
  // good, rather than just disabling it.
  kodi::addon::PVRTimerType recurring;
  recurring.SetId(kTimerTypeRecurring);
  recurring.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
                          PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME |
                          PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS | PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY |
                          PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE);
  recurring.SetDescription("Recurring recording (day-of-week)");
  types.push_back(recurring);

  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetTimersAmount(int& amount)
{
  EnsureRecordingsLoaded();
  EnsureTimerRulesLoaded();
  std::lock_guard<std::mutex> lock(m_dataMutex);
  // Same class of real, confirmed bug as GetChannels()/GetChannelGroups()
  // (see dispatcharr::HasNeverLoadedSuccessfully()'s own comment,
  // Staleness.h), confirmed independently against Kodi's own real current
  // SDK source rather than assumed to be equivalent: CPVRTimers::UpdateEntries()
  // (PVRTimers.cpp) deletes any existing timer missing from this addon's
  // own just-returned list unless this client is in Kodi's own
  // failedClients -- which, same as the channels case, only ever gets
  // populated from a non-PVR_ERROR_NO_ERROR/PVR_ERROR_NOT_IMPLEMENTED
  // return. Unlike GetRecordings() (deliberately left unfixed -- see its
  // own comment below on why an equivalent fix there wouldn't actually
  // help), there's no second ForClients() call after this one to clear
  // Kodi's own failedClients list back out: CPVRTimers::Update() calls
  // UpdateTimerTypes() first, then GetTimers() last, and each ForClients()
  // call clears failedClients on entry -- so whichever call runs last is
  // the only one that actually reaches UpdateEntries(), and that's this
  // one. If recordings/series rules have never successfully loaded even
  // once, every real pending one-time recording and series rule would
  // otherwise vanish from Kodi's own Timers list instead of merely being
  // unavailable until the next successful refresh.
  //
  // Deliberately checks m_seriesRulesCachedAt only, not
  // m_recurringRulesCachedAt (fixed 2026-09-27, a 41st-pass audit,
  // correcting a real, confirmed regression this same guard introduced
  // one pass earlier -- see m_seriesRulesCachedAt's own comment,
  // PVRDispatcharr.h): a Standard Dispatcharr account with default
  // (view-level) DVR access can never successfully fetch recurring
  // rules at all -- confirmed against Dispatcharr's own real current
  // upstream source, a permanent 403, not a transient failure -- so
  // requiring m_recurringRulesCachedAt to have ever advanced made this
  // guard itself permanently block that account's entire Timers list.
  if (!m_recordingsEverLoaded || !m_seriesRulesEverLoaded)
    return PVR_ERROR_SERVER_ERROR;
  time_t now = time(nullptr);
  int scheduled =
      static_cast<int>(std::count_if(m_cachedRecordings.begin(), m_cachedRecordings.end(),
                                     [now](const Recording& rec) { return dispatcharr::IsListedAsTimer(rec, now); }));
  amount = scheduled + static_cast<int>(m_cachedTimerRules.size()) + static_cast<int>(m_cachedRecurringRules.size());
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
  EnsureRecordingsLoaded();
  EnsureTimerRulesLoaded();
  // Copied out under the lock, then processed lock-free below -- this
  // function's own processing (hashing, multi-pass matching) never
  // touches any other shared state, so there's no need to hold
  // m_dataMutex for all of it, only for this snapshot.
  std::vector<Recording> recordings;
  std::vector<TimerRule> rules;
  std::vector<RecurringRule> recurringRules;
  {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // See GetTimersAmount()'s own comment above.
    if (!m_recordingsEverLoaded || !m_seriesRulesEverLoaded)
      return PVR_ERROR_SERVER_ERROR;
    recordings = m_cachedRecordings;
    rules = m_cachedTimerRules;
    recurringRules = m_cachedRecurringRules;
    // What Kodi is about to be shown for each recurring rule, remembered as the
    // baseline UpdateTimer() diffs an edit against -- see m_reportedRecurringRules.
    m_reportedRecurringRules.clear();
    m_reportedRecurringRuleTimes.clear();
    for (const auto& rule : recurringRules)
      m_reportedRecurringRules[rule.id] = rule;
  }
  // Before any rule below is given its display times, which read the effective offset.
  UpdateServerOffsetCrossCheck(recordings, recurringRules);

  // Series rules have no numeric id at all in Dispatcharr's API (confirmed
  // against a real rule: {mode, title, tvg_id, channel_id, title_mode,
  // description, description_mode} -- nothing else), so rule.id is always
  // 0 and can't be used for a ClientIndex -- every series rule would
  // collide on the same one. Hash the (title, tvgId) pair instead, the
  // same identity DeleteSeriesRule() uses, masked into the lower 30 bits
  // so the series-rule flag bit above it is never disturbed.
  //
  // The matching itself -- which recording belongs to which series rule,
  // and each rule's own earliest upcoming/in-progress match -- lives in
  // dispatcharr::MatchRecordingsToSeriesRules() (SeriesRuleMatching.{h,cpp})
  // so it's unit-testable standalone; see that function's own comment
  // for why a series rule needs this at all (a real "12/31/1969" display
  // bug, not a cosmetic nitpick).
  std::vector<unsigned int> ruleClientIndex(rules.size());
  for (std::size_t i = 0; i < rules.size(); ++i)
    ruleClientIndex[i] = ComputeSeriesRuleClientIndex(rules[i].title, rules[i].tvgId, rules[i].epgSourceId);
  SeriesRuleMatchResult matches = MatchRecordingsToSeriesRules(recordings, rules);

  time_t now = time(nullptr);
  for (std::size_t recIdx = 0; recIdx < recordings.size(); ++recIdx)
  {
    const Recording& rec = recordings[recIdx];
    // Completed recordings are surfaced via GetRecordings(), not as
    // timers -- must match GetTimersAmount()'s own filter, via the same
    // dispatcharr::IsListedAsTimer() (RecordingVisibility.h).
    if (!dispatcharr::IsListedAsTimer(rec, now))
      continue;
    kodi::addon::PVRTimer timer;
    timer.SetClientIndex(static_cast<unsigned int>(rec.id));
    timer.SetTimerType(kTimerTypeOneTime);
    timer.SetTitle(rec.title);
    // Same PVR_CHANNEL_INVALID_UID convention GetRecordings() already
    // applies to this same field -- a real, confirmed inconsistency
    // found via a project-wide review: this raw channelId could pass 0
    // straight through (Dispatcharr's own channel ids are always >= 1,
    // so 0 only ever means "no known channel", the same case
    // GetRecordings() already guards), letting Kodi treat "channel 0" as
    // a real, resolvable UID for the timer instead of "no channel".
    timer.SetClientChannelUid(rec.channelId > 0 ? rec.channelId : PVR_CHANNEL_INVALID_UID);
    timer.SetStartTime(rec.startTime);
    timer.SetEndTime(rec.endTime);
    timer.SetState(rec.isInProgress ? PVR_TIMER_STATE_RECORDING : PVR_TIMER_STATE_SCHEDULED);
    // Links this timer back to the actual EPG tag it was recorded from,
    // so Kodi's guide shows a recording indicator on it and its context
    // menu offers "Stop recording" instead of "Record" -- fix for a
    // real, confirmed bug found via a project-wide review (a 32nd-pass
    // audit, confirmed against Kodi's own real current SDK source, not
    // itself independently reproduced): without SetEPGUid(), Kodi falls
    // back to CPVRTimerInfoTag::GetEpgInfoTag()'s own
    // GetTagBetween(timerStart-2min, timerEnd+2min) window probe
    // (PVRTimerInfoTag.cpp), which requires the EPG tag's own start time
    // to fall at or after timerStart-2min -- but rec.startTime here can
    // already be later than the programme's own real start time,
    // confirmed against Dispatcharr's own real current upstream source:
    // RecordingSerializer.validate() silently clamps a past start_time
    // to now on create/update. Pressing "Record" on a programme more
    // than 2 minutes into its own run therefore left Kodi with no way to
    // link the resulting timer back to its EPG tag at all -- the guide
    // kept offering "Record" instead of showing it as already recording,
    // and a second press created a second, duplicate Recording
    // server-side (Dispatcharr has no overlap guard of its own).
    //
    // ResolveRecordingBroadcastId() finds the right EPG entry anyway --
    // preferring an exact match against the recording's own true,
    // never-padded programme start time when available
    // (Recording::programStartTime), falling back to the same >=80%-of-
    // recording-duration overlap-ratio rule Dispatcharr's own server-side
    // auto-enrichment already uses (apps/channels/tasks.py's
    // _match_epg_program_by_timeslot(), confirmed against its own real
    // current upstream source) otherwise -- see
    // dispatcharr::FindEpgEntryIndexByStartTime()/
    // FindEpgEntryIndexCoveringRecording()'s own comments (EpgProgramMatch.h)
    // for why both exist: the overlap-ratio fallback alone still fails a
    // *padded* recording (a real, confirmed gap found via a project-wide
    // review, a 34th-pass audit, not itself independently reproduced --
    // Dispatcharr's own web UI Guide "Record" button and series-rule-
    // materialized occurrences both apply global pre/post-padding
    // directly to a recording's own start_time/end_time, which can push
    // the overlap ratio below 80% well before the programme is even
    // over, especially combined with the same past-start clamp).
    unsigned int broadcastId = ResolveRecordingBroadcastId(rec);
    if (broadcastId != EPG_TAG_INVALID_UID)
      timer.SetEPGUid(broadcastId);
    // Links this one occurrence back to its parent rule (recurring or
    // series) as a Kodi PVR_TIMER child -- the standard Kodi PVR
    // convention for a repeating timer's individual materialized
    // instances (kodi-dev-kit's PVR_TIMER_NO_PARENT is 0,
    // SetParentClientIndex()'s own default, so this is a plain
    // stand-alone one-time timer when neither matches).
    if (rec.recurringRuleId != 0)
    {
      timer.SetParentClientIndex(static_cast<unsigned int>(rec.recurringRuleId) | kRecurringRuleIndexFlag);
    }
    else
    {
      int ruleIdx = matches.recordingRuleIndex[recIdx];
      if (ruleIdx >= 0)
        timer.SetParentClientIndex(ruleClientIndex[static_cast<std::size_t>(ruleIdx)]);
    }
    results.Add(timer);
  }

  for (std::size_t i = 0; i < rules.size(); ++i)
  {
    const TimerRule& rule = rules[i];
    kodi::addon::PVRTimer timer;
    timer.SetClientIndex(ruleClientIndex[i]);
    timer.SetTimerType(kTimerTypeSeries);
    timer.SetTitle(rule.title);
    // rule.title IS Dispatcharr's own actual EPG-title match pattern --
    // there's no separate cosmetic-name field server-side, so this is
    // shown as both Kodi's "Name" and "Search guide for" fields, the
    // same convention Kodi's own CPVRTimerInfoTag::CreateFromEpg() uses
    // for a brand-new EPG-created series timer. Fix for a real,
    // confirmed bug found via a project-wide review, not itself
    // independently reproduced -- see
    // dispatcharr::ResolveSeriesRuleMatchTitle()'s own comment
    // (TimerIdentity.h) for the full story: without this, "Search guide
    // for" always displayed empty, and AddTimer()/UpdateTimer() never
    // read it at all.
    timer.SetEPGSearchString(rule.title);
    // Same PVR_CHANNEL_INVALID_UID convention as the one-time-recording
    // branch above -- see its own comment.
    timer.SetClientChannelUid(rule.channelId > 0 ? rule.channelId : PVR_CHANNEL_INVALID_UID);
    timer.SetState(PVR_TIMER_STATE_SCHEDULED);
    timer.SetPreventDuplicateEpisodes(rule.recordNewOnly ? 1 : 0);
    int earliestIdx = matches.ruleEarliestRecordingIndex[i];
    if (earliestIdx >= 0)
    {
      const Recording& earliest = recordings[static_cast<std::size_t>(earliestIdx)];
      timer.SetStartTime(earliest.startTime);
      timer.SetEndTime(earliest.endTime);
    }
    else
    {
      // Fix for a real, confirmed bug found live (see docs/OPEN_ITEMS.md's
      // 2026-09-29 live check): a correctly-linkable rule with genuinely
      // no upcoming/in-progress occurrence right now (a show between
      // seasons, a "new episodes only" rule while only reruns currently
      // air, a show outside the fetched EPG window) otherwise leaves
      // SetStartTime()/SetEndTime() at Kodi's own zero-initialized
      // default, displaying as the Unix epoch in the Timers list --
      // confirmed live, a real test rule with no possible match rendered
      // as exactly "1970-01-01 00:00:00" for both fields.
      //
      // SetStartAnyTime()/SetEndAnyTime()/SetWeekdays(PVR_WEEKDAY_ALLDAYS)
      // render as "Any day at any time" instead -- confirmed against
      // Kodi's own real current source, CPVRTimerInfoTag::UpdateSummary()
      // (PVRTimerInfoTag.cpp): the list-view summary text substitutes
      // these unconditionally, entirely independent of whether the timer
      // TYPE itself declares PVR_TIMER_TYPE_SUPPORTS_START_ANYTIME/
      // END_ANYTIME (that pair only gates a user-facing toggle in Kodi's
      // own *edit* dialog, GUIDialogPVRTimerSettings.cpp, for letting a
      // person manually pick "any time" on a new/edited timer -- a
      // separate concern from this read-only display fix). Deliberately
      // does NOT add either flag to kTimerTypeSeries's own
      // GetTimerTypes() declaration: this addon never offers "any time"
      // as an editable choice, it only ever computes it here for display,
      // and the series type already has no SUPPORTS_START_TIME/END_TIME
      // of its own for the edit dialog to react to either way -- so
      // there's no dialog-behavior/CreateFromEpg() regression risk to
      // this fix's actual scope, unlike this open item's own original,
      // overcautious "fix direction" note assumed.
      timer.SetStartAnyTime(true);
      timer.SetEndAnyTime(true);
      timer.SetWeekdays(PVR_WEEKDAY_ALLDAYS);
    }
    results.Add(timer);
  }

  {
    for (const auto& rule : recurringRules)
    {
      kodi::addon::PVRTimer timer;
      // Recurring rules have a real numeric id (unlike series rules
      // above), so it's used directly rather than hashed -- just needs
      // its own namespace bit so it can never collide with a plain
      // one-time recording's own id (kTimerTypeOneTime above) or a
      // hashed series-rule index.
      timer.SetClientIndex(static_cast<unsigned int>(rule.id) | kRecurringRuleIndexFlag);
      timer.SetTimerType(kTimerTypeRecurring);
      // Kodi is shown the name without the ownership tag (ManagedRecurringRule.h).
      const std::string ruleDisplayName = StripManagedRuleMarker(rule.name);
      timer.SetTitle(ruleDisplayName.empty() ? ("Recurring recording " + std::to_string(rule.id)) : ruleDisplayName);
      // Same PVR_CHANNEL_INVALID_UID convention as the one-time-recording
      // branch above -- see its own comment.
      timer.SetClientChannelUid(rule.channelId > 0 ? rule.channelId : PVR_CHANNEL_INVALID_UID);
      // The computation itself lives in
      // dispatcharr::ComputeRecurringRuleDisplayTimes() (RecurringRuleUtil.h,
      // the inverse of ComputeRecurringRuleFields() below) so it's
      // unit-testable standalone -- see that function's own comment,
      // including the open question on overnight rules tracked in
      // docs/OPEN_ITEMS.md, and firstDayOut's own comment for the real
      // bug fixed by shifting it here rather than passing rule.startDate
      // straight through to SetFirstDay() unshifted.
      //
      // Passes a resolver callback, not a single pre-resolved offset --
      // see that function's own comment for the real, live-confirmed DST
      // bug this fixes (a single offset resolved at rule.startDate's own
      // UTC midnight was wrong by the DST delta whenever startDate and
      // the rule's own real start instant landed on opposite sides of a
      // transition). AddTimer()/UpdateTimer()'s own create/edit path
      // (ComputeRecurringRuleFields() below) doesn't need this same
      // iteration -- it already resolves at timer.GetStartTime(), an
      // exact absolute instant handed in directly, not a startDate+
      // time-of-day combination that needs resolving before it's even
      // known which side of a transition it lands on.
      time_t startTime, endTime, firstDay;
      int dayShift = 0;
      ComputeRecurringRuleDisplayTimes(
          rule.startDate, rule.startTimeOfDaySeconds, rule.endTimeOfDaySeconds,
          [this](time_t at) { return EffectiveRecurringRuleUtcOffsetMinutes(at); }, dispatcharr::LocalUtcOffsetMinutes,
          startTime, endTime, firstDay, dayShift);
      timer.SetFirstDay(firstDay);
      timer.SetStartTime(startTime);
      timer.SetEndTime(endTime);
      {
        std::lock_guard<std::mutex> lock(m_dataMutex);
        m_reportedRecurringRuleTimes[rule.id] = {startTime, endTime};
      }
      // Dispatcharr's days_of_week is in its own zone's calendar and Kodi
      // reads the weekday bitmask in its own (Kodi's offset for an instant
      // is libc's localtime() of it, dispatcharr::LocalUtcOffsetMinutes())
      // -- see dispatcharr::ComputeRecurringRuleDayShift()
      // (RecurringRuleUtil.h) for why, and for the real, live-confirmed
      // display mismatch this fixes (a rule read from a Dispatcharr in a
      // different zone than Kodi's listed the wrong weekdays, or none that
      // matched its real occurrences). Always zero when the two zones agree,
      // at every time of year. The base bitmask conversion itself lives in
      // dispatcharr::ComputeRecurringRuleWeekdaysBitmask()
      // (RecurringRuleWeekdays.h) so it's unit-testable standalone -- see
      // that function's own comment and RecurringRule's own comment for
      // why Dispatcharr's days_of_week needs no reordering to become a
      // Kodi PVR_WEEKDAY bitmask.
      timer.SetWeekdays(
          dispatcharr::RotateWeekdaysBitmask(ComputeRecurringRuleWeekdaysBitmask(rule.daysOfWeek), -dayShift));
      if (m_debugLogging && dayShift != 0)
      {
        kodi::Log(ADDON_LOG_DEBUG,
                  "pvr.dispatcharr-unofficial: recurring rule %d: Kodi is at UTC%+d min, Dispatcharr at UTC%+d min -- "
                  "weekdays shown %+d day(s) from Dispatcharr's",
                  rule.id, dispatcharr::LocalUtcOffsetMinutes(startTime),
                  EffectiveRecurringRuleUtcOffsetMinutes(startTime), -dayShift);
      }
      // dispatcharr::HasRecurringRuleEndDatePassed() (RecurringRuleRenewal.h)
      // -- a rule this addon's own ShouldRenewRecurringRule() has already
      // (deliberately) refused to push forward again once its end_date
      // passed still displayed as a perfectly healthy SCHEDULED repeating
      // timer here, with nothing telling the user it's silently recording
      // nothing at all -- see that function's own comment. Same offset
      // already resolved for rule.startDate just above -- end_date's own
      // inclusive-day/UTC-vs-Dispatcharr-local nuance (that function's
      // own comment) doesn't need a fresh DST-sensitive lookup at
      // rule.endDate itself; this is already a deliberately conservative
      // check, not a precise one.
      timer.SetState(!rule.enabled ? PVR_TIMER_STATE_DISABLED
                     : dispatcharr::HasRecurringRuleEndDatePassed(
                           rule.enabled, rule.endDate, now, EffectiveRecurringRuleUtcOffsetMinutes(rule.startDate))
                         ? PVR_TIMER_STATE_ERROR
                         : PVR_TIMER_STATE_SCHEDULED);
      results.Add(timer);
    }
  }

  SaveRecordingEpgLinksIfDirty();
  return PVR_ERROR_NO_ERROR;
}

bool PVRDispatcharr::ComputeRecurringRuleFields(const kodi::addon::PVRTimer& timer, std::vector<int>& daysOfWeekOut,
                                                int& startSecondsOut, int& endSecondsOut, time_t& startDateOut,
                                                std::string& error)
{
  // Delegates to the free function in RecurringRuleUtil.{h,cpp} -- pulled
  // out specifically so this pure UTC day/time-of-day math is
  // unit-testable standalone; see tests/test_recurring_rule_util.cpp.
  // This method stays as the public entry point since AddTimer()/
  // UpdateTimer() already call it via this exact name.
  //
  // The *only* place a real timezone enters is the explicit
  // EffectiveRecurringRuleUtcOffsetMinutes() shift below, bridging to
  // Dispatcharr's own (non-UTC-by-default) system timezone -- computed
  // live for a known zone (see recurring_rule_timezone), or falling back
  // to the plain manual recurring_rule_utc_offset_minutes setting
  // otherwise; see that method's own comment and RecurringRule's comment
  // in DispatcharrClient.h for why an arbitrary zone can't be handled
  // automatically.
  //
  // Resolved at timer.GetStartTime(), not "now" -- a real, confirmed
  // regression this fixes, found via a project-wide review (introduced
  // by, and not caught within, this same day's own earlier DST display
  // fix): GetTimers() now computes the displayed startTime/endTime using
  // the offset resolved at the rule's own startDate, but UpdateTimer()
  // (which receives that exact displayed value back from Kodi on ANY
  // edit, including one that doesn't touch the time fields at all, e.g.
  // toggling enabled/disabled) still resolved its own offset at "now" --
  // whenever startDate and "now" fall on opposite sides of a DST
  // transition, the two different offsets applied on each leg of the
  // round trip don't cancel out, silently shifting the rule's actual
  // recording time by the DST delta on every such edit. Using
  // timer.GetStartTime() here instead recovers a matching offset in the
  // ordinary case (its calendar date is always very close to
  // startDate's own), and is also at least as correct as "now" for a
  // genuinely new rule (AddTimer()'s own start time is normally today or
  // the near future anyway).
  //
  // Kodi's own offset is asked about an instant, not read once: Kodi turns
  // each timer time into local time (and back) with libc's localtime() of
  // that very instant (CPVRTimerInfoTag::ConvertUTCToLocalTime()/
  // ConvertLocalTimeToUTC()), so it follows daylight saving per date rather
  // than applying one bias to all of them. It's what turns the weekday
  // bitmask and first day, both read in Kodi's local calendar, into
  // Dispatcharr's own -- see dispatcharr::ComputeRecurringRuleDayShift()
  // (RecurringRuleUtil.h).
  const int dispatcharrOffsetMinutes = EffectiveRecurringRuleUtcOffsetMinutes(timer.GetStartTime());
  const bool ok = dispatcharr::ComputeRecurringRuleFields(
      timer.GetStartTime(), timer.GetEndTime(), timer.GetFirstDay(), timer.GetWeekdays(), dispatcharrOffsetMinutes,
      dispatcharr::LocalUtcOffsetMinutes, time(nullptr), daysOfWeekOut, startSecondsOut, endSecondsOut, startDateOut,
      error);
  if (m_debugLogging)
  {
    const time_t startTime = timer.GetStartTime();
    const int kodiOffsetMinutes = dispatcharr::LocalUtcOffsetMinutes(startTime);
    const int utcSecondsOfDay = static_cast<int>(((startTime % 86400) + 86400) % 86400);
    std::string dispatcharrDays;
    for (int day : daysOfWeekOut)
      dispatcharrDays += (dispatcharrDays.empty() ? "" : ",") + std::to_string(day);
    kodi::Log(ADDON_LOG_DEBUG,
              "pvr.dispatcharr-unofficial: recurring rule from Kodi: weekdays 0x%02x at UTC %02d:%02d, Kodi at UTC%+d "
              "min, Dispatcharr at UTC%+d min -> day shift %+d, Dispatcharr days [%s]",
              timer.GetWeekdays() & 0x7Fu, utcSecondsOfDay / 3600, (utcSecondsOfDay / 60) % 60, kodiOffsetMinutes,
              dispatcharrOffsetMinutes,
              dispatcharr::ComputeRecurringRuleDayShift(utcSecondsOfDay, kodiOffsetMinutes, dispatcharrOffsetMinutes),
              dispatcharrDays.c_str());
  }
  return ok;
}

PVR_ERROR PVRDispatcharr::AddTimer(const kodi::addon::PVRTimer& timer)
{
  std::string error;
  bool ok;
  if (timer.GetTimerType() == kTimerTypeSeries)
  {
    // Only the channel lookup itself needs m_dataMutex -- copy out the
    // two plain fields ResolveSeriesRuleTvgId() needs and release the
    // lock before it (a real HTTP GET) and CreateSeriesRule() (a POST)
    // below run. A real, confirmed bug otherwise (found via code
    // reading, not reproduced live): holding m_dataMutex across up to
    // two full HTTP round trips (each up to `timeout` seconds, longer
    // still with a Login() retry) blocked every other caller of that
    // same mutex -- GetChannels()/GetEPGForChannel()/GetRecordings()/
    // GetTimers() and the background refresh thread's own commits --
    // for the whole duration, a real Kodi-UI-visible stall over what's
    // meant to be a normal, responsive PVR API call.
    bool channelFound = false;
    int epgDataId = 0;
    std::string channelTvgId;
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      const Channel* ch = FindChannelByUid(static_cast<int>(timer.GetClientChannelUid()));
      channelFound = ch != nullptr;
      if (ch)
      {
        epgDataId = ch->epgDataId;
        channelTvgId = ch->tvgId;
      }
    }
    std::string tvgId = channelFound ? m_client.ResolveSeriesRuleTvgId(epgDataId, channelTvgId) : "";
    // dispatcharr::ResolveSeriesRuleMatchTitle() (TimerIdentity.h) is the
    // actual EPG-title match pattern Kodi's own "Search guide for" field
    // represents -- not GetTitle() ("Name", a separate, cosmetic label)
    // -- see that function's own comment for the real bug this fixes.
    // titleMode/description/descriptionMode/untaggedIsNew/epgSourceId
    // are left at their "nothing to echo" sentinel here -- a brand-new
    // rule has nothing cached yet to preserve, and none of these five
    // have any Kodi-side UI of their own anyway (see
    // dispatcharr::BuildSeriesRuleRequestBody()'s own comment,
    // TimerRequestBuilder.h).
    ok = m_client.CreateSeriesRule(
        static_cast<int>(timer.GetClientChannelUid()), tvgId,
        dispatcharr::ResolveSeriesRuleMatchTitle(timer.GetEPGSearchString(), timer.GetTitle()),
        timer.GetPreventDuplicateEpisodes() != 0, /*titleMode=*/"", /*description=*/"", /*descriptionMode=*/"",
        /*untaggedIsNew=*/false, /*epgSourceId=*/0, error);
  }
  else if (timer.GetTimerType() == kTimerTypeRecurring)
  {
    std::vector<int> daysOfWeek;
    int startSeconds = 0, endSeconds = 0;
    time_t startDate = 0;
    if (!ComputeRecurringRuleFields(timer, daysOfWeek, startSeconds, endSeconds, startDate, error))
    {
      ok = false;
    }
    else
    {
      // Create only -- see TruncateRuleTimesToWholeMinutes()'s own comment
      // for why an edit must keep whatever seconds Kodi echoes back.
      dispatcharr::TruncateRuleTimesToWholeMinutes(startSeconds, endSeconds);
      time_t endDate = startDate + static_cast<time_t>(kRecurringRuleWindowDays) * 86400;
      // Fix for a real, confirmed bug found via a project-wide review,
      // not itself independently reproduced: see
      // CreateRecurringRule()'s own comment on `enabled` -- this used to
      // be hardcoded true regardless of Kodi's own timer-settings
      // dialog Enabled toggle, silently ignoring a user creating a new
      // recurring timer already unchecked.
      bool enabled = timer.GetState() != PVR_TIMER_STATE_DISABLED;
      // A rule created from Kodi is the addon's own: it carries the ownership tag, which
      // is what lets the addon renew (and revive) it and nothing else -- see
      // ManagedRecurringRule.h.
      ok = m_client.CreateRecurringRule(static_cast<int>(timer.GetClientChannelUid()),
                                        AddManagedRuleMarkerBounded(timer.GetTitle()), daysOfWeek, startSeconds,
                                        endSeconds, startDate, endDate, enabled, error);
    }
  }
  else
  {
    // isEpgBased (added 2026-09-29, fixing a real, confirmed,
    // live-verified bug -- see dispatcharr::BuildOneTimeRecordingCreateBody()'s
    // own comment, TimerRequestBuilder.h) is what actually lets
    // Dispatcharr's own configured pre/post padding apply to a recording
    // created by pressing "Record" in Kodi's own EPG guide -- a manual,
    // non-EPG timer (GetEPGUid() == PVR_TIMER_NO_EPG_UID) has no real
    // programme window to associate with that mechanism in the first
    // place, so it's deliberately excluded.
    ok = m_client.CreateOneTimeRecording(static_cast<int>(timer.GetClientChannelUid()), timer.GetStartTime(),
                                         timer.GetEndTime(), timer.GetTitle(),
                                         timer.GetEPGUid() != PVR_TIMER_NO_EPG_UID, error);
  }

  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to create timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  InvalidateAndTriggerTimerUpdate();
  // A one-time recording for a start time at or near "now" (or an already
  // in-progress EPG event, e.g. Kodi's "Record" button on a live guide
  // entry) may already be actively recording by the time this returns.
  // Without this, Kodi has no reason to re-poll GetRecordings() until its
  // own next periodic refresh -- confirmed: a freshly-created recording
  // did not appear under Recordings for several minutes without it, and a
  // full Kodi restart was what actually surfaced it. Harmless no-op for a
  // genuinely future recording or a series rule.
  InvalidateAndTriggerRecordingUpdate();
  // Dispatcharr fills in the recording's real title (custom_properties.
  // program.title, see GetRecordings()) asynchronously, a moment after it
  // actually starts -- confirmed: right at creation, custom_properties is
  // still `{}`. The immediate InvalidateAndTriggerRecordingUpdate() above
  // fires before that happens, so Kodi's first (and, confirmed, often
  // *only* -- nothing else prompts it to ask again) fetch gets our
  // "Recording <id>" fallback and keeps showing it indefinitely, even
  // after the recording finishes. A second, delayed trigger (also
  // invalidating the cache, not just re-triggering against a still-fresh
  // one) gives Dispatcharr time to enrich it first. AddTimer() shouldn't
  // block Kodi's calling thread for this, so it's not run inline -- tracked
  // via TrackDetachedThread() rather than .detach()'d outright so the
  // destructor can still join it -- see m_detachedThreads's own comment.
  // Recurring rules are excluded from this delayed, title-enrichment-
  // specific retrigger, but not for the reason originally stated here
  // (corrected 2026-09-27, a 52nd-pass audit, fixing a real, confirmed
  // factual error found via a project-wide review, confirmed against
  // Dispatcharr's own real current upstream source, not itself
  // independently reproduced): `RecurringRecordingRuleViewSet.perform_create()`
  // (apps/channels/api_views.py) calls `sync_recurring_rule_impl(rule.id,
  // drop_existing=True)` synchronously, inside the same POST that creates
  // the rule -- not deferred to an hourly scheduler task at all -- and
  // that function creates every real `Recording` row for the rule's own
  // scheduling window (`horizon_days`, or up to `end_date` when set)
  // immediately, each already carrying its own real title in
  // `custom_properties.program.title` (corrected 2026-09-27, a 53rd-pass
  // audit, fixing this comment's own imprecise original field-name
  // citation, confirmed against Dispatcharr's own real current upstream
  // source: `RecordingParser.cpp`'s own `r.title` read comes from this
  // exact field, populated from `rule.name or rule.channel.name` --
  // `custom_properties.rule.name` alone, a *different* field this addon
  // never actually reads for the title, is simply `rule.name or ""`,
  // empty for an unnamed rule), not an empty `{}` needing async
  // enrichment the way a plain one-time recording does.
  // The immediate InvalidateAndTriggerRecordingUpdate() above already
  // covers surfacing those without delay; there's simply nothing here for
  // a second, delayed retrigger to catch up on.
  if (timer.GetTimerType() != kTimerTypeSeries && timer.GetTimerType() != kTimerTypeRecurring)
  {
    TrackDetachedThread(
        [this]()
        {
          if (!SleepUnlessShuttingDown(std::chrono::seconds(5)))
            return;
          // Timers before recordings here too (added 2026-09-27, a
          // 42nd-pass audit -- see DeleteRecording()'s own comment for
          // the full account): also refreshes the timer itself, so its
          // own placeholder "Recording <id>" title doesn't linger in
          // Kodi's Timers list once Dispatcharr's async enrichment has
          // actually landed -- the same reason this whole delayed
          // refresh exists for GetRecordings(), just not previously
          // applied to GetTimers() as well.
          InvalidateAndTriggerTimerUpdate();
          InvalidateAndTriggerRecordingUpdate();
        });
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::UpdateTimer(const kodi::addon::PVRTimer& timer)
{
  // The kind/id decoding itself lives in dispatcharr::DecodeTimerClientIndex()
  // (TimerIdentity.h) so it's unit-testable standalone -- see that
  // function's own comment.
  dispatcharr::DecodedTimerIdentity identity =
      dispatcharr::DecodeTimerClientIndex(timer.GetClientIndex(), kRecurringRuleIndexFlag);
  bool isSeries = identity.kind == dispatcharr::TimerIdentityKind::SeriesRule;
  bool isRecurring = identity.kind == dispatcharr::TimerIdentityKind::RecurringRule;
  std::string error;
  bool ok;
  if (isSeries)
  {
    EnsureTimerRulesLoaded();
    // Prefer the rule's own originally-stored tvgId (looked up in the
    // cache by this exact ClientIndex, via
    // dispatcharr::FindSeriesRuleIndexByClientIndex() -- TimerIdentity.h)
    // over re-deriving it from the channel's own *current* state, but
    // only when the channel hasn't actually changed from what this rule
    // was created against (still re-derives for a genuine channel
    // change, which needs a fresh tvgId for the new channel anyway --
    // see the upsert-semantics comment below). A real, confirmed bug
    // this fixes: re-deriving on every edit meant a channel's EPG
    // mapping drifting after the rule was created (or a rule predating
    // ResolveSeriesRuleTvgId() itself, see docs/RECORDINGS.md) silently
    // upserted a *separate* rule under the new, drifted identity
    // instead of editing the existing one -- the same "identity
    // changed" behavior documented below, just triggered by drift the
    // user never asked for.
    //
    // Only the two lookups below (cache scan, channel scan) need
    // m_dataMutex -- copy out plain fields and release the lock before
    // any network call (ResolveSeriesRuleTvgId()'s GET, CreateSeriesRule()'s
    // POST) runs. See AddTimer()'s own comment for the real,
    // Kodi-UI-visible stall this avoids: this same mutex also guards
    // GetChannels()/GetEPGForChannel()/GetRecordings()/GetTimers() and
    // the background refresh thread's own commits.
    std::string tvgId;
    // Echoed straight back to CreateSeriesRule() below when this rule is
    // found in the cache, unchanged -- see
    // dispatcharr::BuildSeriesRuleRequestBody()'s own comment
    // (TimerRequestBuilder.h) for the real, live-confirmed bug this
    // fixes: none of these five have any Kodi-side UI of their own, so
    // the only correct value to send on an edit is whatever this rule's
    // own last-known cached value was, never a sentinel that would
    // silently reset it server-side.
    std::string titleMode, description, descriptionMode;
    bool untaggedIsNew = false;
    int epgSourceId = 0;
    bool foundInCache = false;
    bool channelFound = false;
    int epgDataId = 0;
    std::string channelTvgId;
    // The rule as it was before this edit, found by ClientIndex whether or not the
    // channel changed (unlike foundInCache above, which is also false for a channel
    // change) -- what a replaced rule is deleted by, see below.
    bool foundOldRule = false;
    std::string oldTitle, oldTvgId;
    int oldEpgSourceId = 0;
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      int ruleIdx = dispatcharr::FindSeriesRuleIndexByClientIndex(m_cachedTimerRules, timer.GetClientIndex());
      if (ruleIdx >= 0)
      {
        const TimerRule& old = m_cachedTimerRules[static_cast<std::size_t>(ruleIdx)];
        foundOldRule = true;
        oldTitle = old.title;
        oldTvgId = old.tvgId;
        oldEpgSourceId = old.epgSourceId;
        // Echoed whatever the channel does: none of these five has any Kodi-side UI, so a
        // channel edit must not reset them either. Only tvg_id depends on the channel and is
        // re-derived below when it changed. Before this, a channel change (or "any channel" to a
        // specific one) took the else branch and sent all five as empty, resetting a `contains`
        // rule to `exact` and leaving an EPG-source-pinned rule's replacement as a separate,
        // unpinned rule (found by the 2026-10-04 sixth hardening sweep).
        titleMode = old.titleMode;
        description = old.description;
        descriptionMode = old.descriptionMode;
        untaggedIsNew = old.untaggedIsNew;
        epgSourceId = old.epgSourceId;
      }
      // dispatcharr::IsSameSeriesRuleChannel() (TimerIdentity.h) rather
      // than a plain == -- see that function's own comment for the real
      // bug a plain comparison has for a channel-less rule (cached
      // channelId 0, PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL's own
      // clientChannelUid -1 -- neither is the other, but both mean "no
      // channel").
      if (ruleIdx >= 0 &&
          dispatcharr::IsSameSeriesRuleChannel(m_cachedTimerRules[static_cast<std::size_t>(ruleIdx)].channelId,
                                               static_cast<int>(timer.GetClientChannelUid())))
      {
        tvgId = m_cachedTimerRules[static_cast<std::size_t>(ruleIdx)].tvgId;
        foundInCache = true;
      }
      else
      {
        const Channel* ch = FindChannelByUid(static_cast<int>(timer.GetClientChannelUid()));
        channelFound = ch != nullptr;
        if (ch)
        {
          epgDataId = ch->epgDataId;
          channelTvgId = ch->tvgId;
        }
      }
    }
    if (!foundInCache)
      tvgId = channelFound ? m_client.ResolveSeriesRuleTvgId(epgDataId, channelTvgId) : "";
    // The pinned source is only meaningful for the tvg_id it was pinned with -- see
    // dispatcharr::ResolveSeriesRuleSourceOnEdit().
    if (foundOldRule)
      epgSourceId = dispatcharr::ResolveSeriesRuleSourceOnEdit(oldTvgId, tvgId, oldEpgSourceId);
    // Upsert semantics, same call AddTimer() uses to create one --
    // confirmed against Dispatcharr's own source that re-POSTing with
    // the same identity (title + tvg_id) edits mode/title_mode/
    // description/etc. of the existing rule in place rather than
    // creating a duplicate; there is no PATCH-by-id route since series
    // rules have no id at all. If the title (or the channel, which
    // changes tvgId) genuinely changed from what this rule was
    // originally created with, this instead creates a *separate* rule
    // under the new identity -- the old one is left behind untouched,
    // not renamed. Not worked around here: Dispatcharr's own identity
    // key is title+tvg_id+epg_source_id by design, and Kodi's own series
    // timer dialog doesn't meaningfully support "rename this rule" as a
    // normal workflow to begin with.
    //
    // dispatcharr::ResolveSeriesRuleMatchTitle() (TimerIdentity.h) is the
    // actual EPG-title match pattern Kodi's own "Search guide for" field
    // represents, not GetTitle() ("Name", a separate, cosmetic label) --
    // see that function's own comment for the real bug this fixes:
    // editing "Search guide for" alone used to be silently dropped, and
    // editing "Name" alone silently changed the match rule instead.
    const std::string newTitle = dispatcharr::ResolveSeriesRuleMatchTitle(timer.GetEPGSearchString(), timer.GetTitle());
    ok = m_client.CreateSeriesRule(static_cast<int>(timer.GetClientChannelUid()), tvgId, newTitle,
                                   timer.GetPreventDuplicateEpisodes() != 0, titleMode, description, descriptionMode,
                                   untaggedIsNew, epgSourceId, error);
    // A new search pattern or channel is a new identity, which Dispatcharr stores as
    // a second rule beside the original. Replace rather than duplicate: create
    // first, delete the old one only once that succeeded, so a failed create never
    // costs the user their rule. The delete purges the old rule's future recordings,
    // which is what replacing it means; the new rule's own (a different tvg_id or
    // title, so not matched by that purge) are untouched, and Dispatcharr does not
    // evaluate a rule at create time, so there are none yet to lose.
    const bool replaceOld =
        ok && foundOldRule &&
        dispatcharr::ShouldReplaceSeriesRuleOnEdit(oldTitle, oldTvgId, oldEpgSourceId, newTitle, tvgId, epgSourceId);
    bool oldHasPinnedSibling = false;
    if (replaceOld && oldEpgSourceId <= 0)
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      oldHasPinnedSibling = dispatcharr::HasPinnedSiblingSeriesRule(m_cachedTimerRules, oldTitle, oldTvgId);
    }
    if (replaceOld && oldHasPinnedSibling)
    {
      // An unscoped delete of the old rule would remove the pinned rule with the same title and tvg_id
      // as well, and purge its recordings (found by the 2026-10-04 eighth hardening sweep): leave the
      // old rule and say so.
      kodi::Log(ADDON_LOG_WARNING,
                "pvr.dispatcharr-unofficial: not deleting the old series rule: another rule with the same "
                "title and tvg_id is pinned to an EPG source and the delete would remove it too");
      kodi::QueueNotification(QUEUE_WARNING, "",
                              "The edited series rule was saved, but the old one was left in place (another rule "
                              "shares its title) -- delete it in Dispatcharr if you no longer want it.");
    }
    else if (replaceOld)
    {
      std::string deleteError;
      if (m_client.DeleteSeriesRule(oldTitle, oldTvgId, oldEpgSourceId, deleteError))
      {
        kodi::Log(ADDON_LOG_INFO,
                  "pvr.dispatcharr-unofficial: series rule edit changed its identity; replaced the old rule");
      }
      else
      {
        // The edit itself went through (the new rule exists), so this is not a
        // failure of the user's action -- but they now have two rules.
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharr-unofficial: created the edited series rule but could not delete the old one: %s",
                  deleteError.c_str());
        kodi::QueueNotification(QUEUE_WARNING, "",
                                "The edited series rule was saved, but the old one could not be removed -- delete it "
                                "manually.");
      }
    }
  }
  else if (isRecurring)
  {
    int ruleId = identity.id;
    // Needed below to look up this rule's own cached name/end_date -- see
    // ResolveRecurringRuleNameForUpdate()'s and
    // ShouldExtendRecurringRuleEndDateOnUpdate()'s own call sites further
    // down for why.
    EnsureTimerRulesLoaded();
    std::string cachedName;
    time_t cachedEndDate = 0;
    bool cachedWasEnabled = false;
    bool foundCached = false;
    // The whole cached rule too: it is the baseline an edit is diffed against, see
    // dispatcharr::ComputeRecurringRuleEditPatch().
    RecurringRule cachedRule;
    RecurringRule baselineRule;
    bool haveBaselineRule = false;
    // What GetTimers() showed Kodi as this rule's start and end -- see ResolveRecurringRuleTimesOnEdit().
    std::optional<time_t> reportedStart, reportedEnd;
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      const auto reportedTimes = m_reportedRecurringRuleTimes.find(ruleId);
      if (reportedTimes != m_reportedRecurringRuleTimes.end())
      {
        reportedStart = reportedTimes->second.first;
        reportedEnd = reportedTimes->second.second;
      }
      for (const auto& cached : m_cachedRecurringRules)
      {
        if (cached.id == ruleId)
        {
          cachedName = StripManagedRuleMarker(cached.name);
          cachedEndDate = cached.endDate;
          cachedWasEnabled = cached.enabled;
          cachedRule = cached;
          foundCached = true;
          break;
        }
      }
      // The baseline is what Kodi was last shown, not the cache -- see
      // m_reportedRecurringRules. Falls back to the cache only for a rule GetTimers()
      // never reported (it cannot be editable then, but be safe).
      auto reported = m_reportedRecurringRules.find(ruleId);
      if (reported != m_reportedRecurringRules.end())
      {
        baselineRule = reported->second;
        haveBaselineRule = true;
      }
      else if (foundCached)
      {
        baselineRule = cachedRule;
        haveBaselineRule = true;
      }
    }
    // Re-read from the server rather than trusting the cache for the end date: the
    // open-ended-rule and First Day extension decisions below both turn on it, and
    // the cache can be minutes old. A failed read leaves the cached value in use;
    // a 404 means the rule is gone, which no edit can fix.
    long freshStatus = 0;
    RecurringRule freshRule;
    std::string freshError;
    bool haveFresh = m_client.GetRecurringRuleById(ruleId, freshRule, freshError, &freshStatus);
    const bool ruleGone = !haveFresh && freshStatus == 404;
    if (ruleGone)
    {
      error = "this recurring rule no longer exists on Dispatcharr";
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: UpdateTimer: recurring rule %d is gone: %s", ruleId,
                freshError.c_str());
    }
    if (haveFresh)
    {
      cachedEndDate = freshRule.endDate;
      cachedWasEnabled = freshRule.enabled;
    }
    // The two end-date decisions below need a known end date; the fresh read
    // supplies one just as well as the cache (added 2026-10-03: they used to be
    // skipped on a cache miss even when the fresh read had succeeded, so a First
    // Day moved past the end date 400'd, and a re-enable left a stale end date).
    const bool haveRuleEndDate = foundCached || haveFresh;
    std::vector<int> daysOfWeek;
    int startSeconds = 0, endSeconds = 0;
    time_t startDate = 0;
    if (ruleGone)
    {
      ok = false;
    }
    else if (!ComputeRecurringRuleFields(timer, daysOfWeek, startSeconds, endSeconds, startDate, error))
    {
      ok = false;
    }
    else
    {
      // This is also how Kodi's "enable/disable" timer action reaches a
      // recurring rule -- GetTimerTypes() declares
      // PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE for this type specifically
      // so that action is offered at all, and Kodi implements it by
      // calling UpdateTimer() with everything else unchanged and just
      // GetState() flipped, not a separate dedicated call.
      bool enabled = timer.GetState() != PVR_TIMER_STATE_DISABLED;
      // Fix for a real, confirmed bug found via a project-wide review,
      // not itself independently reproduced: timer.GetTitle() can itself
      // be the synthesized "Recurring recording <id>" placeholder
      // GetTimers() falls back to when this rule's own `name` is empty
      // -- UpdateRecurringRule()'s single PATCH always sends whatever
      // `name` it's given (no separate "don't touch name" option the way
      // one-time recordings have via RenameRecording()), so an edit that
      // doesn't actually rename anything (e.g. a bare enable/disable
      // toggle) would otherwise send that placeholder straight back as
      // the rule's own new, permanent name. See
      // ResolveRecurringRuleNameForUpdate()'s own comment (TimerIdentity.h)
      // -- the same fix ShouldRenameOnTimerEdit() already applies to a
      // one-time recording's own analogous placeholder, just reached
      // here as the value to send rather than a separate action to skip.
      // Applied unconditionally, not just when foundCached -- a rare
      // cache miss (m_cachedRecurringRules stale/empty) still shouldn't
      // bypass this guard entirely; falling back to an empty cachedName
      // in that case is the same "preserve an empty name" outcome the
      // common (empty-name) case already produces, at the cost of
      // clearing a real name that happened to coincidentally equal this
      // exact placeholder text -- the same narrow tradeoff
      // ShouldRenameOnTimerEdit() already accepts for its own analogous
      // placeholder.
      std::string nameToSend =
          ResolveRecurringRuleNameForUpdate(timer.GetTitle(), foundCached ? cachedName : "", ruleId);

      // Fix for a real, confirmed bug found via a project-wide review
      // (a 19th-pass audit), not itself independently reproduced: moving
      // this rule's own First Day forward to reach or pass its own
      // currently-cached end_date -- entirely possible via Kodi's own
      // First Day picker, which offers dates up to a year out, against a
      // renewed rule's own end_date sitting at most
      // kRecurringRuleWindowDays out -- fails the PATCH below outright,
      // since an ordinary edit omits end_date and relies on the server falling back to
      // the existing one. See ComputeRecurringRuleEndDateForStartDateChange()'s
      // own comment (RecurringRuleRenewal.h) for why. Extending first,
      // before the PATCH that would otherwise 400, not after (unlike
      // ShouldExtendRecurringRuleEndDateOnUpdate() below, which restores
      // an already-stale end_date once the main edit has already
      // succeeded) -- best-effort: if this fails, UpdateRecurringRule()
      // below is left to fail on its own and report that instead.
      if (haveRuleEndDate)
      {
        time_t requiredEndDate =
            ComputeRecurringRuleEndDateForStartDateChange(startDate, cachedEndDate, kRecurringRuleWindowDays);
        if (requiredEndDate > 0)
        {
          std::string extendError;
          if (m_client.ExtendRecurringRuleEndDate(ruleId, requiredEndDate, extendError))
            cachedEndDate = requiredEndDate;
          else
            kodi::Log(ADDON_LOG_ERROR,
                      "pvr.dispatcharr-unofficial: failed to extend recurring rule %d's end_date ahead of a First "
                      "Day edit: %s",
                      ruleId, extendError.c_str());
        }
      }

      // Only what the user changed: diffed against the cached rule, which is what
      // Kodi's dialog was filled from. A save that changes nothing sends nothing --
      // and so doesn't make the server drop and regenerate every future occurrence.
      // See RecurringRuleEdit.h.
      RecurringRuleFields editedFields;
      editedFields.channelId = static_cast<int>(timer.GetClientChannelUid());
      editedFields.name = nameToSend;
      editedFields.daysOfWeek = daysOfWeek;
      editedFields.startTimeOfDaySeconds = startSeconds;
      editedFields.endTimeOfDaySeconds = endSeconds;
      editedFields.startDate = startDate;
      editedFields.enabled = enabled;
      std::optional<RecurringRuleFields> baselineFields;
      if (haveBaselineRule)
      {
        RecurringRuleFields b;
        b.channelId = baselineRule.channelId;
        b.name = StripManagedRuleMarker(baselineRule.name);
        b.daysOfWeek = baselineRule.daysOfWeek;
        b.startTimeOfDaySeconds = baselineRule.startTimeOfDaySeconds;
        b.endTimeOfDaySeconds = baselineRule.endTimeOfDaySeconds;
        b.startDate = baselineRule.startDate;
        b.enabled = baselineRule.enabled;
        baselineFields = b;
      }
      // Times Kodi handed back exactly as shown are not an edit, whatever re-deriving them would give
      // (the skipped spring-forward hour) -- see ResolveRecurringRuleTimesOnEdit().
      const RuleTimesOfDay times =
          ResolveRecurringRuleTimesOnEdit(baselineFields, timer.GetStartTime(), timer.GetEndTime(), reportedStart,
                                          reportedEnd, startSeconds, endSeconds);
      editedFields.startTimeOfDaySeconds = times.startSeconds;
      editedFields.endTimeOfDaySeconds = times.endSeconds;
      RecurringRuleEditPatch patch = ComputeRecurringRuleEditPatch(baselineFields, editedFields);
      // Names are compared and sent as Kodi knows them; a rule the addon owns keeps its
      // ownership tag through a rename (ManagedRecurringRule.h). A rule that is not owned
      // is not made so by being edited.
      if (patch.name)
      {
        const bool ownedRule =
            IsManagedRuleName(haveFresh ? freshRule.name : (haveBaselineRule ? baselineRule.name : ""));
        if (ownedRule)
          patch.name = AddManagedRuleMarkerBounded(*patch.name);
      }
      // A rule with no end date at all cannot be saved without being given one.
      if (haveRuleEndDate)
      {
        time_t openEndedFallback =
            ComputeEndDateForOpenEndedRuleEdit(cachedEndDate, startDate, time(nullptr), kRecurringRuleWindowDays);
        if (openEndedFallback > 0)
        {
          patch.endDate = openEndedFallback;
          kodi::Log(ADDON_LOG_INFO,
                    "pvr.dispatcharr-unofficial: recurring rule %d has no end_date; giving it a %d-day rolling window "
                    "so it can be edited",
                    ruleId, kRecurringRuleWindowDays);
        }
      }
      if (patch.IsEmpty())
      {
        kodi::Log(ADDON_LOG_DEBUG,
                  "pvr.dispatcharr-unofficial: UpdateTimer: recurring rule %d unchanged, nothing to send", ruleId);
        ok = true;
      }
      else
      {
        ok = m_client.UpdateRecurringRule(ruleId, patch, error);
        // Once an open-ended rule has an end date, the checks below see it.
        if (ok && patch.endDate)
          cachedEndDate = *patch.endDate;
      }

      // Fix for a real, confirmed gap found via a project-wide review,
      // not itself independently reproduced: UpdateRecurringRule()'s own
      // PATCH deliberately omits end_date (see its own comment), so a
      // rule left disabled long enough for its existing end_date to
      // fall into the past, then re-enabled from here, would otherwise
      // stay enabled with an end_date already in the past forever --
      // ShouldRenewRecurringRule()'s own periodic renewal loop now
      // correctly refuses to touch an already-expired end_date (a
      // separate, deliberate fix), since it can't tell that case apart
      // from a rule a user created directly with a genuine, intentionally
      // -finite end_date. This explicit edit is exactly the moment a
      // stale end_date can safely be restored -- the user's own action
      // is the signal, not a background guess. Requires a genuine
      // disabled -> enabled transition (`cachedWasEnabled`), not just the
      // post-edit `enabled` value alone -- see
      // ShouldExtendRecurringRuleEndDateOnUpdate()'s own comment for the
      // real, confirmed gap this closes (found via a project-wide review
      // in a later pass than the one that added this call): checking
      // only the post-edit state fired on any edit of an already-enabled
      // rule, not just an explicit re-enable, silently resurrecting a
      // deliberately-finite, already-expired, still-enabled rule the
      // "don't resurrect an expired rule" fix elsewhere in this file
      // exists to leave alone. Best-effort: a failure here doesn't fail
      // the whole edit, since the rule's core fields (days/times/
      // enabled/etc.) already updated successfully above -- just logged,
      // since the periodic renewal loop can't retry this one on its own
      // (that's the entire reason this exists).
      if (ok && haveRuleEndDate &&
          ShouldExtendRecurringRuleEndDateOnUpdate(cachedWasEnabled, enabled, cachedEndDate, time(nullptr),
                                                   kRecurringRuleWindowDays))
      {
        time_t newEndDate = time(nullptr) + static_cast<time_t>(kRecurringRuleWindowDays) * 86400;
        std::string extendError;
        if (!m_client.ExtendRecurringRuleEndDate(ruleId, newEndDate, extendError))
        {
          kodi::Log(ADDON_LOG_ERROR,
                    "pvr.dispatcharr-unofficial: failed to restore recurring rule %d's end_date on edit: %s", ruleId,
                    extendError.c_str());
        }
      }
    }
  }
  else
  {
    int id = identity.id;
    // Fix for a real, confirmed bug: this used to branch on
    // timer.GetState() == PVR_TIMER_STATE_RECORDING -- Kodi's own
    // *cached* copy of this timer, which only changes when this addon's
    // own GetTimers() runs (the recording-refresh thread, every
    // recording_refresh_minutes, up to 5 minutes by default -- Kodi
    // itself never flips a timer to RECORDING on its own). A scheduled
    // recording that had genuinely already started, but within that
    // stale window, still read as SCHEDULED to Kodi -- so an edit fell
    // through to the plain-PATCH branch below instead of the dedicated
    // extend endpoint, and per ExtendRecording()'s own comment, that
    // bare PATCH revokes the running Celery task instead of extending
    // it: exactly the failure this dedicated-endpoint routing exists to
    // avoid, just reached via stale Kodi state instead of never routing
    // there at all. Fetching the recording's own live isInProgress
    // fresh (rather than trusting timer.GetState()) fixes this the same
    // way the extend branch already had to, one step earlier: it needed
    // rec.endTime anyway (Kodi doesn't send the pre-edit value), so this
    // adds no new network round trip to the common case, and a failed
    // lookup now refuses rather than risking a PATCH against unknown
    // state.
    Recording rec;
    if (!FindRecordingById(id, rec))
    {
      kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to update timer: recording %d not found", id);
      // Triggered here too (added 2026-09-27, a 51st-pass audit, fixing a
      // real gap the 49th-pass "trigger unconditionally" fix below left
      // open, found via a project-wide review, not itself independently
      // reproduced): this return happens before ever reaching that
      // trigger. FindRecordingById() is a live GET (GetRecordingById()),
      // not a cache lookup -- corrected 2026-09-27, a 52nd-pass audit,
      // fixing a real inaccuracy in this comment's own original wording,
      // found via a project-wide review, confirmed against
      // DispatcharrClient::GetRecordingById()'s own real current source,
      // not itself independently reproduced -- so this can fail for any
      // reason (a transient network error, a 5xx, an auth failure), not
      // only a confirmed-gone 404. Triggering here is harmless for a
      // transient failure too (corrected 2026-09-27, a 53rd-pass audit,
      // fixing a real, confirmed false claim in this comment's own prior
      // wording, found via a project-wide review, confirmed by tracing
      // this function to its own end: this early return -- like the
      // no-op-edit one right after it -- never reaches the shared tail
      // below at all, that comment's own "neither can reach this shared
      // tail" already says so) -- a plain wasted refresh, nothing more --
      // but most usefully closes the case where the recording is
      // genuinely gone server-side (deleted by another install, or
      // Dispatcharr's own
      // cleanup, since the timer was last listed), which otherwise left
      // the stale timer sitting in Kodi's Timers list until some
      // unrelated refresh happened to occur.
      InvalidateAndTriggerTimerUpdate();
      InvalidateAndTriggerRecordingUpdate();
      return PVR_ERROR_SERVER_ERROR;
    }
    // Pure classification core in dispatcharr::ClassifyOneTimeTimerEdit()
    // (RecordingVisibility.h) -- an inline, order-sensitive 4-way
    // decision that two separate earlier passes (20 and 21) each added a
    // new case to, each time inserting it at a specific point in what
    // used to be an if/else-if chain here to get the ordering right
    // relative to the others. Extracted (a 22nd-pass audit) so that
    // ordering is locked in by a test instead of resting on each future
    // change getting it right again by inspection.
    switch (dispatcharr::ClassifyOneTimeTimerEdit(rec.isInProgress, rec.isUpcoming, rec.endTime, time(nullptr)))
    {
    case dispatcharr::OneTimeTimerEditAction::Extend:
    {
      // Editing an already-recording timer's end time is Kodi's native
      // "extend this recording" UX (its player OSD's "Record for longer"
      // action opens exactly this same timer-edit dialog) -- routed to
      // the dedicated extend endpoint, NOT UpdateOneTimeRecording()'s
      // generic PATCH: see ExtendRecording()'s own comment for why a bare
      // PATCH here would actually revoke the running Celery task instead
      // of extending it. Dispatcharr's endpoint takes a relative
      // extra_minutes, not the absolute end time Kodi hands back here, so
      // the current end time has to be fetched fresh first -- Kodi
      // doesn't send the pre-edit value, and this addon's own last-polled
      // copy could be stale.
      //
      // The delta-to-minutes conversion and non-positive-delta rejection
      // itself lives in dispatcharr::ComputeRecordingExtendMinutes()
      // (TimerIdentity.h) so it's unit-testable standalone -- see that
      // function's own comment.
      int extraMinutes = 0;
      bool wantsExtend = dispatcharr::ComputeRecordingExtendMinutes(rec.endTime, timer.GetEndTime(), extraMinutes);
      bool titleChanged = dispatcharr::ShouldRenameOnTimerEdit(timer.GetTitle(), rec.title, id);
      if (!wantsExtend && !titleChanged)
      {
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharr-unofficial: failed to update timer: new end time is not later than the current one");
        // Deliberately returns with no trigger (noted 2026-09-27, a
        // 51st-pass audit): unlike the "recording not found" return just
        // above, this is a pure client-side validation rejection -- no
        // PATCH or any other server call was ever attempted, so there's
        // nothing server-side a refresh could possibly pick up here.
        return PVR_ERROR_INVALID_PARAMETERS;
      }
      ok = true;
      if (wantsExtend)
        ok = m_client.ExtendRecording(id, extraMinutes, error);
      // RenameRecording() only ever writes custom_properties.program.title
      // via a dedicated metadata endpoint (see its own comment) -- unrelated
      // to the recording's actual schedule/status, so it's just as safe to
      // call on a still-in-progress recording as it already was in the
      // not-yet-started case below. Fix for two real, confirmed bugs (the
      // same class as an earlier fix's own "editing a one-time timer's
      // title silently did nothing" note, just reached from this case
      // instead): a title-only edit on an in-progress recording used to
      // always fail outright (no genuine end-time change meant wantsExtend
      // was false, treated as an error regardless of whether the title
      // itself changed) -- and even a title+time edit together silently
      // dropped the title, since this case never called RenameRecording()
      // at all before.
      if (ok && titleChanged)
        ok = m_client.RenameRecording(id, timer.GetTitle(), error);
      break;
    }
    case dispatcharr::OneTimeTimerEditAction::RenameOnly:
      // A missed occurrence (still `isUpcoming` to this addon forever,
      // see IsMissedOccurrence()'s own comment) or an already-finished
      // recording (a stale-cache window IsAlreadyFinishedRecording()
      // already documents, RecordingVisibility.h) -- either way,
      // there's nothing left to reschedule: a start/end-time PATCH via
      // UpdateOneTimeRecording() would resend an end_time Dispatcharr's
      // own validation rejects outright (`end_time < now`, confirmed
      // live -- see UpdateOneTimeRecording()'s own comment), which used
      // to fail even a harmless title-only edit, since RenameRecording()
      // was never reached once that PATCH failed first. Only a rename
      // applies; any other field Kodi's dialog might have changed
      // (start/end time) is silently ignored rather than attempting a
      // PATCH that would just be rejected anyway.
      ok = true;
      if (dispatcharr::ShouldRenameOnTimerEdit(timer.GetTitle(), rec.title, id))
        ok = m_client.RenameRecording(id, timer.GetTitle(), error);
      break;
    case dispatcharr::OneTimeTimerEditAction::Reschedule:
      // A recurring rule's own occurrence can't be moved from here -- see
      // IsRecurringOccurrenceReschedule() (RecordingVisibility.h): Dispatcharr
      // recreates the original slot within the hour and reverts the edit on
      // the next rule-level change. Refused before any server call, so nothing
      // to refresh.
      if (dispatcharr::IsRecurringOccurrenceReschedule(rec.recurringRuleId, rec.startTime, rec.endTime,
                                                       timer.GetStartTime(), timer.GetEndTime()))
      {
        kodi::Log(ADDON_LOG_ERROR,
                  "pvr.dispatcharr-unofficial: refusing to move recurring-rule occurrence %d: Dispatcharr would "
                  "recreate its original slot and revert the change -- edit the recurring rule instead",
                  id);
        kodi::QueueNotification(QUEUE_ERROR, "",
                                "This recording belongs to a recurring rule, so its time can't be changed on its "
                                "own -- edit the recurring rule instead.");
        return PVR_ERROR_REJECTED;
      }
      // One-time (manual or EPG-based) recording, not yet started.
      // UpdateOneTimeRecording() itself deliberately doesn't touch title/
      // custom_properties -- see its own comment for why (a real crash risk
      // on a bare partial PATCH, and this mirrors CreateOneTimeRecording()'s
      // own choice not to stomp Dispatcharr's auto-enrichment). A title
      // change is instead sent separately, via the same dedicated
      // update-metadata/ endpoint RenameRecording() (the Extend case
      // above) already uses for a completed/in-progress recording -- and
      // only when the title actually changed, so an edit that only touches
      // start/end time doesn't send a stale title and prematurely mark
      // user_edited, which would block Dispatcharr's own EPG-based
      // auto-enrichment from ever filling the title in. Real reported bug:
      // editing a one-time timer's title from Kodi's own Timers-list edit
      // dialog silently did nothing before this.
      ok = m_client.UpdateOneTimeRecording(
          id, timer.GetStartTime(), timer.GetEndTime(),
          dispatcharr::ChannelToSendOnOneTimeEdit(static_cast<int>(timer.GetClientChannelUid()), rec.channelId), error);
      if (ok && dispatcharr::ShouldRenameOnTimerEdit(timer.GetTitle(), rec.title, id))
        ok = m_client.RenameRecording(id, timer.GetTitle(), error);
      break;
    }
  }

  // Triggered unconditionally past this point, even on failure below
  // (fixed 2026-09-27, a 49th-pass audit, fixing a real, confirmed bug
  // found via a project-wide review, confirmed against Kodi's own real
  // current SDK source, not itself independently reproduced) -- two
  // early returns above this point (recording-not-found, its own
  // trigger added a pass later; and the no-op-edit rejection, which
  // deliberately has none -- see each one's own comment) are the only
  // exceptions, since neither can reach this shared tail at all: Kodi's
  // own
  // CPVRGUIActionsTimers::ToggleTimerState() (the enable/disable toggle
  // this addon's own GetTimerTypes() PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE
  // flag offers) calls `timer->SetState(...)` on its own shared, cached
  // timer object *before* calling into this addon at all -- and
  // CPVRTimers::UpdateTimer() -> CPVRTimerInfoTag::UpdateOnClient() has no
  // rollback of any kind on a failed return, only an error dialog. A
  // failed enable/disable (a network blip, a non-manager account getting
  // a 403) left Kodi displaying the *opposite* of the rule's real
  // server-side state until some unrelated refresh happened to correct
  // it -- the same class of stale-optimistic-UI-state bug already fixed
  // for a failed RenameRecording()/DeleteRecording() one pass earlier.
  // This also covers a *partial* success reported as an overall failure
  // (e.g. Extend succeeds but the accompanying Rename fails, or the
  // pre-PATCH ExtendRecurringRuleEndDate succeeds but UpdateRecurringRule()
  // itself then fails) -- triggering the refresh regardless of `ok`
  // correctly picks up whatever *did* actually change server-side rather
  // than leaving Kodi's cache silently wrong about that too. Harmless
  // when nothing changed at all: Kodi just re-fetches and sees the same
  // state again.
  InvalidateAndTriggerTimerUpdate();
  // A rescheduled one-time recording (or a recurring rule's own edit,
  // which can add/remove materialized occurrences) changes what
  // GetRecordings() would return too, same reasoning as AddTimer()'s own
  // trigger -- series rules have no recording-list-visible effect from
  // an edit alone (evaluation is a separate, explicit step).
  if (!isSeries)
    InvalidateAndTriggerRecordingUpdate();
  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to update timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  return PVR_ERROR_NO_ERROR;
}

PVR_ERROR PVRDispatcharr::DeleteTimer(const kodi::addon::PVRTimer& timer, bool forceDelete)
{
  // See UpdateTimer()'s own comment on DecodeTimerClientIndex(). The
  // recurring-rule flag is only ever set on a recurring rule's own parent
  // timer (see GetTimers()) -- one of its individual materialized child
  // instances keeps a plain, unflagged ClientIndex (just the underlying
  // Recording's own id), so it falls through to the ordinary one-time
  // delete path below like any other Recording.
  dispatcharr::DecodedTimerIdentity identity =
      dispatcharr::DecodeTimerClientIndex(timer.GetClientIndex(), kRecurringRuleIndexFlag);
  bool isSeries = identity.kind == dispatcharr::TimerIdentityKind::SeriesRule;
  bool isRecurring = identity.kind == dispatcharr::TimerIdentityKind::RecurringRule;
  std::string error;
  bool ok;
  if (isSeries)
  {
    // Series rules have no numeric id in Dispatcharr's API -- they're
    // deleted by title + tvg_id, the same identity AddTimer() used to
    // create them (see CreateSeriesRule() above). Prefer the rule's own
    // originally-stored identity (looked up in the cache by this exact
    // ClientIndex, via dispatcharr::FindSeriesRuleIndexByClientIndex() --
    // TimerIdentity.h) over re-deriving tvgId from the channel's own
    // *current* state: a real, confirmed bug this fixes -- a channel's
    // EPG mapping drifting after the rule was created (or a rule
    // predating ResolveSeriesRuleTvgId() itself, see docs/RECORDINGS.md)
    // meant the re-derived tvgId no longer matched what Dispatcharr
    // actually stored, so the DELETE silently matched nothing and the
    // rule was never removed. If the rule isn't found in the cache at
    // all (e.g. already deleted server-side since GetTimers() last cached
    // it) nothing is deleted: the old channel-derived fallback sent no EPG
    // source and so removed every source's copy of the title and tvg_id.
    EnsureTimerRulesLoaded();
    // Only the two lookups below (cache scan, channel scan) need
    // m_dataMutex -- copy out plain fields and release the lock before
    // any network call (ResolveSeriesRuleTvgId()'s GET, DeleteSeriesRule()'s
    // DELETE) runs. See AddTimer()'s own comment for the real,
    // Kodi-UI-visible stall this avoids.
    std::string title, tvgId;
    // See UpdateTimer()'s own identical field for why this is echoed
    // from the cache rather than left at its "unpinned" sentinel: a
    // real, confirmed data-loss bug (docs/OPEN_ITEMS.md) -- Dispatcharr's
    // own delete only restricts by epg_source_id when the request itself
    // supplies one, so omitting it here matched and removed *every* EPG
    // source's own copy of a rule sharing title+tvgId, not just the one
    // pinned copy the user meant to delete.
    int epgSourceId = 0;
    bool foundInCache = false;
    bool siblingPinned = false;
    {
      std::lock_guard<std::mutex> lock(m_dataMutex);
      int ruleIdx = dispatcharr::FindSeriesRuleIndexByClientIndex(m_cachedTimerRules, timer.GetClientIndex());
      if (ruleIdx >= 0)
      {
        const TimerRule& cached = m_cachedTimerRules[static_cast<std::size_t>(ruleIdx)];
        title = cached.title;
        tvgId = cached.tvgId;
        epgSourceId = cached.epgSourceId;
        foundInCache = true;
        if (epgSourceId <= 0)
          siblingPinned = dispatcharr::HasPinnedSiblingSeriesRule(m_cachedTimerRules, title, tvgId);
      }
    }
    if (!foundInCache)
    {
      // Not deleted by guessing: a rule that is not in the freshly loaded cache is either already
      // gone or the load failed, and the old fallback (re-deriving title and tvg_id from Kodi's
      // own copy and sending no EPG source) deleted every source's copy of a rule sharing that
      // title and tvg_id -- the data-loss path the cached branch above closed (found by the
      // 2026-10-04 sixth hardening sweep). Kodi drops a rule the server no longer lists at its
      // next timer refresh.
      kodi::Log(ADDON_LOG_WARNING,
                "pvr.dispatcharr-unofficial: not deleting a series rule that is not in the loaded rule list "
                "(already deleted, or the list could not be loaded)");
      ok = false;
      error = "the series rule was not found on the server";
    }
    else if (epgSourceId <= 0 && siblingPinned)
    {
      // See HasPinnedSiblingSeriesRule(): this delete names no EPG source, so Dispatcharr would
      // remove the pinned rule with the same title and tvg_id too.
      kodi::Log(ADDON_LOG_WARNING,
                "pvr.dispatcharr-unofficial: not deleting a series rule whose title and tvg_id are shared with a "
                "rule pinned to an EPG source: the delete would remove both");
      ok = false;
      error = "another series rule with the same title is pinned to an EPG source; delete it in Dispatcharr";
    }
    else
    {
      ok = m_client.DeleteSeriesRule(title, tvgId, epgSourceId, error);
    }
  }
  else if (isRecurring)
  {
    // Deleting the rule also purges its future materialized recordings
    // server-side (confirmed against Dispatcharr's source:
    // RecurringRecordingRuleViewSet.perform_destroy calls
    // purge_recurring_rule_impl), so the
    // InvalidateAndTriggerRecordingUpdate() below (fires for anything
    // other than a series rule) correctly reflects those disappearing
    // too, not just the rule itself.
    int ruleId = identity.id;
    ok = m_client.DeleteRecurringRule(ruleId, error);
  }
  else
  {
    int id = identity.id;
    // Confirmed against Kodi's own source (xbmc/pvr/timers/PVRTimers.cpp):
    // forceDelete is specifically how Kodi tells the addon "this timer is
    // still actively recording" -- both its dedicated "Stop Recording"
    // action and "Delete" on a timer it already knows is recording pass
    // it as true. That must route to Dispatcharr's dedicated stop
    // endpoint ("stop a recording early while retaining the partial
    // content for playback"), NOT the delete endpoint (removes the file
    // entirely) -- confirmed by testing: routing it to delete wiped out
    // an actively-recording file the user only meant to stop.
    //
    // forceDelete itself comes from Kodi's own *cached* copy of this
    // timer's state (see UpdateTimer()'s own comment on the identical
    // staleness this fixes there) -- only refreshed by this addon's own
    // GetTimers(), up to recording_refresh_minutes stale. A recording
    // that genuinely started within that window still reads as
    // "scheduled" to Kodi, so it can pass forceDelete=false here even
    // though the server-side recording is actually in progress. The
    // decision itself lives in dispatcharr::ShouldStopInsteadOfDelete()
    // (RecordingVisibility.h) so it's unit-testable standalone -- see
    // that function's own comment, including the real data-loss risk
    // this fixes.
    Recording rec;
    long lookupHttpStatus = 0;
    bool lookupOk = FindRecordingById(id, rec, &lookupHttpStatus);
    // A second real, confirmed data-loss risk found via a project-wide
    // review, checked before ShouldStopInsteadOfDelete() below -- see
    // dispatcharr::IsAlreadyFinishedRecording()'s own comment
    // (RecordingVisibility.h): a recording short enough to both start
    // and finish inside one recording_refresh_minutes interval can still
    // show as a SCHEDULED timer to Kodi's own stale cache even though
    // it already completed normally server-side. Neither stopping
    // (nothing left recording) nor deleting (real, wanted content, not
    // an empty timer) is correct there -- Kodi's own next refresh
    // naturally drops the now-stale Timers entry once this addon's own
    // cache catches up.
    //
    // The whole decision, including a lookup that failed, is
    // dispatcharr::DecideDeleteTimerAction() -- see its comment for why an
    // unverifiable recording is refused rather than guessed at.
    switch (
        dispatcharr::DecideDeleteTimerAction(forceDelete, lookupOk, lookupHttpStatus, rec.isInProgress, rec.isUpcoming))
    {
    case dispatcharr::DeleteTimerAction::kAlreadyDone:
      ok = true;
      break;
    case dispatcharr::DeleteTimerAction::kStop:
      ok = m_client.StopRecording(id, error);
      break;
    case dispatcharr::DeleteTimerAction::kDelete:
      ok = m_client.DeleteRecording(id, error);
      break;
    case dispatcharr::DeleteTimerAction::kRefuseUnverified:
      ok = false;
      error = "couldn't check whether this recording is in progress";
      kodi::QueueNotification(QUEUE_WARNING, "",
                              "Couldn't check this recording's state on Dispatcharr, so it was not deleted. Try "
                              "again in a moment.");
      break;
    }
  }

  // Triggered unconditionally, even on failure below -- see UpdateTimer()'s
  // own comment (added the same, 49th, pass) for the full reasoning: a
  // failed delete/stop can still have partially changed server-side
  // state (or Kodi's own cached copy may already assume the delete/stop
  // succeeded in some UI path), and this refresh corrects either case;
  // it's a harmless no-op when nothing actually changed.
  InvalidateAndTriggerTimerUpdate();
  // Stopping a recording (forceDelete=true, see above) turns it into a
  // normal completed recording immediately, not just a future timer-list
  // change -- make sure Kodi's Recordings view picks that up too, same
  // reasoning as AddTimer()'s trigger.
  if (!isSeries)
    InvalidateAndTriggerRecordingUpdate();
  if (!ok)
  {
    kodi::Log(ADDON_LOG_ERROR, "pvr.dispatcharr-unofficial: failed to delete timer: %s", error.c_str());
    return PVR_ERROR_SERVER_ERROR;
  }
  return PVR_ERROR_NO_ERROR;
}
