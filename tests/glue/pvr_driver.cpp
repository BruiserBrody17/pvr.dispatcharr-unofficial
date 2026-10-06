// Drives PVRDispatcharr through Kodi's real C function table (the KodiToAddonFuncTable_PVR the
// dev-kit's CInstancePVRClient fills in), against fake_dispatcharr.py, from several threads.
#include "PVRDispatcharr.h"
#include <kodi/addon-instance/PVR.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

void InitKodiStub();
KODI_ADDON_INSTANCE_STRUCT* MakePvrInstanceStruct();
extern std::mutex g_setMu;
extern std::map<std::string, std::string> g_settings;
extern std::atomic<bool> g_dropSetSetting;
extern std::atomic<long> g_setCount, g_notifyCount, g_triggerCount;
extern std::string g_userPath;
extern AddonInstance_PVR g_pvr;
struct Collected
{
  std::mutex mu;
  std::vector<PVR_CHANNEL> channels;
  std::vector<PVR_CHANNEL_GROUP> groups;
  std::vector<PVR_CHANNEL_GROUP_MEMBER> members;
  std::vector<EPG_TAG> epg;
  std::vector<std::string> epgTitles;
  std::vector<PVR_RECORDING> recordings;
  std::vector<PVR_TIMER> timers;
};

static int g_port;
static std::atomic<int> g_fail{0};
static auto T0 = std::chrono::steady_clock::now();
static long ms()
{
  return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count();
}
#define FAIL(...)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    g_fail++;                                                                                                          \
    printf("FAIL t=%ld: ", ms());                                                                                      \
    printf(__VA_ARGS__);                                                                                               \
    printf("\n");                                                                                                      \
    fflush(stdout);                                                                                                    \
  } while (0)
#define INFO(...)                                                                                                      \
  do                                                                                                                   \
  {                                                                                                                    \
    printf("t=%ld: ", ms());                                                                                           \
    printf(__VA_ARGS__);                                                                                               \
    printf("\n");                                                                                                      \
    fflush(stdout);                                                                                                    \
  } while (0)
static void sleep_ms(int m)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(m));
}
static size_t sinkcb(char* p, size_t s, size_t n, void* u)
{
  ((std::string*)u)->append(p, s * n);
  return s * n;
}
static std::string http(const std::string& path)
{
  CURL* c = curl_easy_init();
  std::string out;
  std::string url = "http://127.0.0.1:" + std::to_string(g_port) + path;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sinkcb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
  curl_easy_perform(c);
  curl_easy_cleanup(c);
  return out;
}
static void ctl(const std::string& kv)
{
  http("/__ctl?" + kv);
}
static void set(const std::string& k, const std::string& v)
{
  std::lock_guard<std::mutex> g(g_setMu);
  g_settings[k] = v;
}

static const AddonInstance_PVR* I()
{
  return &g_pvr;
}
static KodiToAddonFuncTable_PVR* A()
{
  return g_pvr.toAddon;
}

static PVR_ERROR all_lists(Collected& c, bool withEpg)
{
  int n = 0;
  PVR_ERROR e;
  PVR_ADDON_CAPABILITIES caps{};
  A()->GetCapabilities(I(), &caps);
  char buf[256];
  A()->GetBackendName(I(), buf, sizeof buf);
  A()->GetBackendVersion(I(), buf, sizeof buf);
  A()->GetConnectionString(I(), buf, sizeof buf);
  A()->GetChannelGroupsAmount(I(), &n);
  e = A()->GetChannelGroups(I(), (PVR_HANDLE)&c, false);
  std::vector<PVR_CHANNEL_GROUP> groups;
  {
    std::lock_guard<std::mutex> g(c.mu);
    groups = c.groups;
  }
  for (auto& gr : groups)
    A()->GetChannelGroupMembers(I(), (PVR_HANDLE)&c, &gr);
  A()->GetChannelsAmount(I(), &n);
  e = A()->GetChannels(I(), (PVR_HANDLE)&c, false);
  std::vector<PVR_CHANNEL> chans;
  {
    std::lock_guard<std::mutex> g(c.mu);
    chans = c.channels;
  }
  if (withEpg)
    for (auto& ch : chans)
      A()->GetEPGForChannel(I(), (PVR_HANDLE)&c, (int)ch.iUniqueId, time(nullptr) - 86400, time(nullptr) + 2 * 86400);
  static thread_local std::vector<PVR_TIMER_TYPE> types(32);
  int nt = 32;
  A()->GetTimerTypes(I(), types.data(), &nt);
  A()->GetTimersAmount(I(), &n);
  A()->GetTimers(I(), (PVR_HANDLE)&c);
  A()->GetRecordingsAmount(I(), false, &n);
  A()->GetRecordings(I(), (PVR_HANDLE)&c, false);
  return e;
}

static int timer_cycle(std::mt19937& rng, const std::vector<PVR_CHANNEL>& chans, const std::vector<EPG_TAG>& epg)
{
  int fails = 0;
  time_t now = time(nullptr);
  const PVR_CHANNEL& ch = chans[rng() % chans.size()];
  // one-time EPG-based on a future programme of that channel
  PVR_TIMER t{};
  t.iTimerType = 3;
  t.iClientChannelUid = (int)ch.iUniqueId;
  t.state = PVR_TIMER_STATE_SCHEDULED;
  for (auto& e : epg)
    if (e.iUniqueChannelId == ch.iUniqueId && e.startTime > now + 3600)
    {
      t.startTime = e.startTime;
      t.endTime = e.endTime;
      t.iEpgUid = e.iUniqueBroadcastId;
      break;
    }
  if (!t.startTime)
  {
    t.startTime = now + 7200;
    t.endTime = now + 9000;
    t.iTimerType = 1;
    t.iEpgUid = PVR_TIMER_NO_EPG_UID;
  }
  snprintf(t.strTitle, sizeof t.strTitle, "T%u", (unsigned)rng());
  if (A()->AddTimer(I(), &t) != PVR_ERROR_NO_ERROR)
    fails++;
  // series
  PVR_TIMER s{};
  s.iTimerType = 2;
  s.iClientChannelUid = (int)ch.iUniqueId;
  snprintf(s.strTitle, sizeof s.strTitle, "Show %u-1", ch.iChannelNumber);
  snprintf(s.strEpgSearchString, sizeof s.strEpgSearchString, "Show %u-1", ch.iChannelNumber);
  s.state = PVR_TIMER_STATE_SCHEDULED;
  s.bStartAnyTime = true;
  s.bEndAnyTime = true;
  if (A()->AddTimer(I(), &s) != PVR_ERROR_NO_ERROR)
    fails++;
  // recurring
  PVR_TIMER r{};
  r.iTimerType = 4;
  r.iClientChannelUid = (int)ch.iUniqueId;
  snprintf(r.strTitle, sizeof r.strTitle, "Rec %u", (unsigned)rng());
  r.startTime = now + 3600;
  r.endTime = now + 5400;
  r.firstDay = now;
  r.iWeekdays = 0x7F;
  r.state = PVR_TIMER_STATE_SCHEDULED;
  if (A()->AddTimer(I(), &r) != PVR_ERROR_NO_ERROR)
    fails++;
  // read back, update, delete everything this cycle created
  Collected c;
  A()->GetTimers(I(), (PVR_HANDLE)&c);
  for (auto& x : c.timers)
  {
    if (x.state != PVR_TIMER_STATE_SCHEDULED)
      continue;
    PVR_TIMER u = x;
    snprintf(u.strTitle, sizeof u.strTitle, "%s!", x.strTitle);
    if (u.iTimerType == 1 || u.iTimerType == 3)
    {
      u.startTime += 60;
      u.endTime += 120;
    }
    A()->UpdateTimer(I(), &u);
    A()->DeleteTimer(I(), &u, false);
  }
  return fails;
}

static void stream_cycle(std::mt19937& rng, const std::vector<PVR_CHANNEL>& chans,
                         const std::vector<PVR_RECORDING>& recs)
{
  std::vector<unsigned char> buf(256 * 1024);
  if (rng() % 2 && !chans.empty())
  {
    PVR_CHANNEL ch = chans[rng() % chans.size()];
    PVR_NAMED_VALUE props[40];
    unsigned int np = 0;
    A()->GetChannelStreamProperties(I(), &ch, props, &np);
    if (A()->OpenLiveStream(I(), &ch))
    {
      std::atomic<bool> stop{false};
      std::thread times(
          [&]
          {
            while (!stop)
            {
              PVR_STREAM_TIMES st{};
              A()->GetStreamTimes(I(), &st);
              A()->LengthLiveStream(I());
              A()->CanPauseStream(I());
              A()->IsRealTimeStream(I());
              sleep_ms(50);
            }
          });
      for (int i = 0; i < 20; i++)
      {
        A()->ReadLiveStream(I(), buf.data(), (unsigned)buf.size());
        if (i == 10)
          A()->SeekLiveStream(I(), 0, SEEK_SET);
      }
      stop = true;
      times.join();
      A()->CloseLiveStream(I());
    }
  }
  else if (!recs.empty())
  {
    PVR_RECORDING r = recs[rng() % recs.size()];
    PVR_NAMED_VALUE props[40];
    unsigned int np = 0;
    A()->GetRecordingStreamProperties(I(), &r, props, &np);
    if (A()->OpenRecordedStream(I(), &r))
    {
      std::atomic<bool> stop{false};
      std::thread times(
          [&]
          {
            while (!stop)
            {
              PVR_STREAM_TIMES st{};
              A()->GetStreamTimes(I(), &st);
              A()->LengthRecordedStream(I());
              sleep_ms(50);
            }
          });
      for (int i = 0; i < 15; i++)
      {
        A()->ReadRecordedStream(I(), buf.data(), (unsigned)buf.size());
        if (i == 7)
          A()->SeekRecordedStream(I(), 0, SEEK_SET);
      }
      stop = true;
      times.join();
      A()->CloseRecordedStream(I());
    }
    PVR_EDL_ENTRY edl[PVR_ADDON_EDL_LENGTH];
    int ne = PVR_ADDON_EDL_LENGTH;
    A()->GetRecordingEdl(I(), &r, edl, &ne);
  }
}

static const char* kSettingsCycle[][2] = {
    {"recording_pre_offset_minutes", "3"},
    {"recording_post_offset_minutes", "4"},
    {"recording_refresh_minutes", "1"},
    {"channel_refresh_hours", "6"},
    {"epg_refresh_hours", "2"},
    {"live_timeshift_mode", "0"},
    {"live_timeshift_mode", "2"},
    {"recurring_rule_timezone", "Europe/London"},
    {"recurring_rule_utc_offset_minutes", "60"},
    {"recurring_rule_timezone", "manual"},
    {"debug_logging", "true"},
    {"debug_logging", "false"},
    {"enable_catchup_ffmpegdirect_seek", "true"},
    {"enable_realtime_updates", "true"},
    {"enable_realtime_updates", "false"},
    {"timeout", "7"},
};

int main(int argc, char** argv)
{
  g_port = atoi(argv[1]);
  std::string what = argv[2];
  int dur = argc > 3 ? atoi(argv[3]) : 10000;
  char tmpl[] = "/tmp/pvr_glue_userdata_XXXXXX";
  g_userPath = mkdtemp(tmpl);
  InitKodiStub();
  curl_global_init(CURL_GLOBAL_ALL);
  set("host", "127.0.0.1");
  set("port", std::to_string(g_port));
  set("username", "u");
  set("password", "p");
  set("timeout", getenv("TMO") ? getenv("TMO") : "5");
  set("live_timeshift_mode", "2");
  set("api_key", "");
  set("enable_realtime_updates", getenv("RT") || what == "refresh_events" ? "true" : "false");
  if (what == "offset_cross_check")
  {
    // The fake server's zone (a fixed-offset one, so the scenario does not depend on the time of year) and the rule and
    // occurrence it materialized; the constructor's zone sync then selects the same zone in the setting.
    ctl("seed_offset_check=1");
  }
  KODI_ADDON_INSTANCE_STRUCT* inst = MakePvrInstanceStruct();
  long c0 = ms();
  PVRDispatcharr* pvr;
  {
    kodi::addon::IInstanceInfo info(inst);
    pvr = new PVRDispatcharr(info);
  }
  INFO("constructed in %ld ms; api_key now '%s'", ms() - c0, g_settings["api_key"].c_str());
  std::thread faultT;
  if (getenv("PVR_FAULTS"))
    faultT = std::thread(
        []
        {
          std::string spec = getenv("PVR_FAULTS");
          long base = ms();
          size_t p = 0;
          while (p < spec.size())
          {
            size_t semi = spec.find(';', p);
            std::string item = spec.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
            size_t colon = item.find(':');
            long at = atol(item.substr(0, colon).c_str());
            while (ms() - base < at)
              sleep_ms(10);
            INFO("fault %s", item.substr(colon + 1).c_str());
            ctl(item.substr(colon + 1));
            if (semi == std::string::npos)
              break;
            p = semi + 1;
          }
        });
  if (what == "ctor_dtor")
  {
    sleep_ms(dur);
  }
  else if (what == "surface")
  {
    // wait for the background thread's first channel/EPG load the way Kodi would poll
    for (int i = 0; i < 100; i++)
    {
      int n = 0;
      if (A()->GetChannelsAmount(I(), &n) == PVR_ERROR_NO_ERROR && n > 0)
        break;
      sleep_ms(100);
    }
    Collected c0c;
    all_lists(c0c, true);
    INFO("channels=%zu groups=%zu members=%zu epg=%zu timers=%zu recs=%zu", c0c.channels.size(), c0c.groups.size(),
         c0c.members.size(), c0c.epg.size(), c0c.timers.size(), c0c.recordings.size());
    // The scenario's own premise: without channels every cycle below runs on nothing and passes.
    if (c0c.channels.empty())
      FAIL("the surface scenario loaded no channels");
    for (auto& ch : c0c.channels)
      INFO(" ch uid=%u num=%u.%u name=%s archive=%d", ch.iUniqueId, ch.iChannelNumber, ch.iSubChannelNumber,
           ch.strChannelName, ch.bHasArchive);
    for (auto& r : c0c.recordings)
      INFO(" rec id=%s title=%s dir=%s", r.strRecordingId, r.strTitle, r.strDirectory);
    std::atomic<bool> stop{false};
    std::atomic<long> cycles{0}, tfails{0};
    std::vector<std::thread> th;
    int nThreads = getenv("NTH") ? atoi(getenv("NTH")) : 4;
    for (int k = 0; k < nThreads; k++)
      th.emplace_back(
          [&, k]
          {
            std::mt19937 rng(k);
            while (!stop)
            {
              Collected c;
              all_lists(c, k % 2 == 0);
              if (!c.channels.empty() && k == 0)
                tfails += timer_cycle(rng, c.channels, c0c.epg);
              cycles++;
            }
          });
    th.emplace_back(
        [&]
        {
          std::mt19937 rng(77);
          while (!stop)
          {
            stream_cycle(rng, c0c.channels, c0c.recordings);
          }
        });
    th.emplace_back(
        [&]
        {
          std::mt19937 rng(5);
          size_t i = 0;
          while (!stop)
          {
            auto& kv = kSettingsCycle[i++ % (sizeof kSettingsCycle / sizeof kSettingsCycle[0])];
            set(kv[0], kv[1]);
            if (rng() % 7 == 0)
              g_dropSetSetting = !g_dropSetSetting;
            pvr->OnAddonSettingChanged(kv[0], kodi::addon::CSettingValue(kv[1]));
            if (rng() % 5 == 0)
              A()->OnSystemWake(I());
            sleep_ms(150);
          }
        });
    sleep_ms(dur);
    stop = true;
    for (auto& t : th)
      t.join();
    g_dropSetSetting = false;
    INFO("surface: cycles=%ld timer-add-fails=%ld sets=%ld notifies=%ld triggers=%ld", (long)cycles, (long)tfails,
         (long)g_setCount, (long)g_notifyCount, (long)g_triggerCount);
    if (cycles == 0)
      FAIL("no list cycle completed");
  }
  else if (what == "functional")
  {
    for (int i = 0; i < 100; i++)
    {
      int n = 0;
      if (A()->GetChannelsAmount(I(), &n) == PVR_ERROR_NO_ERROR && n > 0)
        break;
      sleep_ms(100);
    }
    Collected c;
    A()->GetChannels(I(), (PVR_HANDLE)&c, false);
    if (c.channels.empty())
      FAIL("no channels were listed");
    // EPG: first call may answer "not loaded yet" -- retry like Kodi
    for (auto& ch : c.channels)
    {
      Collected e;
      PVR_ERROR err = PVR_ERROR_SERVER_ERROR;
      for (int i = 0; i < 50 && err != PVR_ERROR_NO_ERROR; i++)
      {
        e.epg.clear();
        e.epgTitles.clear();
        err = A()->GetEPGForChannel(I(), (PVR_HANDLE)&e, (int)ch.iUniqueId, time(nullptr) - 86400,
                                    time(nullptr) + 2 * 86400);
        if (err != PVR_ERROR_NO_ERROR)
          sleep_ms(200);
      }
      std::map<unsigned, int> ids;
      int dup = 0;
      for (auto& t : e.epg)
        if (ids[t.iUniqueBroadcastId]++)
          dup++;
      INFO("epg ch %u (%u.%u): err=%d tags=%zu dupIds=%d first='%s' last='%s'", ch.iUniqueId, ch.iChannelNumber,
           ch.iSubChannelNumber, err, e.epg.size(), dup, e.epgTitles.empty() ? "" : e.epgTitles.front().c_str(),
           e.epgTitles.empty() ? "" : e.epgTitles.back().c_str());
      if (err != PVR_ERROR_NO_ERROR)
        FAIL("the guide of channel %u never loaded (err=%d)", ch.iUniqueId, err);
      else if (e.epg.empty())
        FAIL("channel %u has no guide entries", ch.iUniqueId);
      if (dup)
        FAIL("channel %u lists %d guide entries with a duplicate id", ch.iUniqueId, dup);
      {
        std::lock_guard<std::mutex> g(c.mu);
        for (size_t i = 0; i < e.epg.size(); i++)
        {
          c.epg.push_back(e.epg[i]);
          c.epgTitles.push_back(e.epgTitles[i]);
        }
      }
    }
    time_t now = time(nullptr);
    // EPG-based one-time on channel 6 (5.1), next programme
    PVR_TIMER t{};
    t.iTimerType = 3;
    t.iClientChannelUid = 6;
    t.state = PVR_TIMER_STATE_SCHEDULED;
    for (size_t i = 0; i < c.epg.size(); i++)
      if (c.epg[i].iUniqueChannelId == 6 && c.epg[i].startTime > now)
      {
        t.startTime = c.epg[i].startTime;
        t.endTime = c.epg[i].endTime;
        t.iEpgUid = c.epg[i].iUniqueBroadcastId;
        snprintf(t.strTitle, sizeof t.strTitle, "%s", c.epgTitles[i].c_str());
        break;
      }
    auto mustAdd = [&](const char* what2, PVR_TIMER* timer)
    {
      PVR_ERROR rc = A()->AddTimer(I(), timer);
      INFO("AddTimer %s: %d (epguid %u title %s)", what2, rc, timer->iEpgUid, timer->strTitle);
      if (rc != PVR_ERROR_NO_ERROR)
        FAIL("AddTimer %s failed (%d)", what2, rc);
    };
    if (!t.startTime)
      FAIL("no upcoming guide entry on channel 6 to record");
    mustAdd("epg-based", &t);
    // instant recording: start 0
    PVR_TIMER inst{};
    inst.iTimerType = 1;
    inst.iClientChannelUid = 2;
    inst.startTime = 0;
    inst.endTime = now + 1800;
    inst.iEpgUid = PVR_TIMER_NO_EPG_UID;
    snprintf(inst.strTitle, sizeof inst.strTitle, "Instant");
    mustAdd("instant", &inst);
    PVR_TIMER r{};
    r.iTimerType = 4;
    r.iClientChannelUid = 1;
    snprintf(r.strTitle, sizeof r.strTitle, "Daily");
    r.startTime = now + 3600;
    r.endTime = now + 5400;
    r.firstDay = now;
    r.iWeekdays = 0x1F;
    mustAdd("recurring", &r);
    PVR_TIMER s{};
    s.iTimerType = 2;
    s.iClientChannelUid = 3;
    snprintf(s.strTitle, sizeof s.strTitle, "Show 3-1");
    snprintf(s.strEpgSearchString, sizeof s.strEpgSearchString, "Show 3-1");
    s.bStartAnyTime = s.bEndAnyTime = true;
    mustAdd("series", &s);
    sleep_ms(1500);
    Collected tt;
    PVR_ERROR timersRc = A()->GetTimers(I(), (PVR_HANDLE)&tt);
    INFO("GetTimers: %d", timersRc);
    if (timersRc != PVR_ERROR_NO_ERROR)
      FAIL("GetTimers failed (%d)", timersRc);
    // The four timers just added (a recurring rule is listed with its occurrences, so at least those four).
    if (tt.timers.size() < 4)
      FAIL("GetTimers listed %zu timers after four were added", tt.timers.size());
    for (auto& x : tt.timers)
      INFO(" timer idx=%u parent=%u type=%u ch=%d state=%d start=%ld end=%ld epg=%u title='%s' days=%u first=%ld",
           x.iClientIndex, x.iParentClientIndex, x.iTimerType, x.iClientChannelUid, x.state, (long)x.startTime - now,
           (long)x.endTime - now, x.iEpgUid, x.strTitle, x.iWeekdays, (long)x.firstDay);
    Collected rr;
    A()->GetRecordings(I(), (PVR_HANDLE)&rr, false);
    for (auto& x : rr.recordings)
      INFO(" rec id=%s title='%s' ch=%d dur=%d epg=%u", x.strRecordingId, x.strTitle, x.iChannelUid, x.iDuration,
           x.iEpgEventId);
    printf("serverdb=%s\n", http("/__db").c_str());
  }
  else if (what == "refresh_events")
  {
    // The realtime socket tells the addon an M3U refresh finished: one that changed channels makes it fetch the channel
    // list again within moments; one that changed none (the usual scheduled refresh) costs nothing. (The EPG case
    // schedules a fetch 330 s out, which is unit-tested but too slow for this harness.)
    auto stat = [&](const char* key) -> long
    {
      const std::string all = http("/__stats");
      const size_t at = all.find(std::string("\"") + key + "\": ");
      return at == std::string::npos ? 0 : atol(all.c_str() + at + strlen(key) + 4);
    };
    for (int i = 0; i < 100 && stat("GET /ws/") < 1; i++)
      sleep_ms(100);
    if (stat("GET /ws/") < 1)
      FAIL("the realtime socket never connected");
    // let the startup fetches settle before counting
    sleep_ms(3000);
    const char* kChannels = "GET /api/channels/channels/";
    const std::string kUnchanged =
        "%7B%22type%22%3A%22update%22%2C%22data%22%3A%7B%22type%22%3A%22m3u_refresh%22%2C%22account%22%3A5%2C%22action%"
        "22%3A%22parsing%22%2C%22status%22%3A%22success%22%2C%22progress%22%3A100%2C%22channels_created%22%3A0%2C%"
        "22channels_updated%22%3A0%2C%22channels_deleted%22%3A0%7D%7D";
    const std::string kChanged =
        "%7B%22type%22%3A%22update%22%2C%22data%22%3A%7B%22type%22%3A%22m3u_refresh%22%2C%22account%22%3A5%2C%22action%"
        "22%3A%22parsing%22%2C%22status%22%3A%22success%22%2C%22progress%22%3A100%2C%22channels_created%22%3A2%2C%"
        "22channels_updated%22%3A0%2C%22channels_deleted%22%3A0%7D%7D";
    const long before = stat(kChannels);
    ctl("ws_push=" + kUnchanged);
    sleep_ms(4000);
    INFO("channel fetches: %ld before, %ld after an M3U refresh that changed nothing", before, stat(kChannels));
    if (stat(kChannels) != before)
      FAIL("an M3U refresh that changed no channel made the addon fetch the channel list");
    ctl("ws_push=" + kChanged);
    long after = before;
    for (int i = 0; i < 100 && after == before; i++)
    {
      sleep_ms(200);
      after = stat(kChannels);
    }
    INFO("channel fetches: %ld before, %ld after an M3U refresh that created channels", before, after);
    if (after == before)
      FAIL("an M3U refresh that created channels did not make the addon fetch the channel list again");
  }
  else if (what == "offset_cross_check")
  {
    // The server's zone is a fixed UTC+9 in the table; the occurrence the fake server materialized for a 19:30 rule
    // starts at 11:30 UTC, which is UTC+8. The rule's displayed start must follow the server, not the table
    // (ServerOffsetCrossCheck.h), once two evaluations in a row have said so.
    // (the zone, the rule and its occurrence are set up before the instance is constructed, see above)
    auto ruleStart = [&]() -> long
    {
      Collected tt;
      A()->GetTimers(I(), (PVR_HANDLE)&tt);
      for (auto& x : tt.timers)
        // the rule itself (a repeating timer), not the occurrence listed under it, which has the same title
        if (x.iTimerType == 4 && std::string(x.strTitle) == "ZZZ offset rule")
          return (long)x.startTime;
      return -1;
    };
    // Only the time of day is compared: the date is today's, whenever the scenario runs.
    constexpr long kServerTimeOfDay = 11 * 3600 + 1800; // 19:30 at UTC+8
    constexpr long kTableTimeOfDay = 10 * 3600 + 1800;  // 19:30 at UTC+9
    long start = -1, first = -1;
    for (int i = 0; i < 60; i++)
    {
      start = ruleStart();
      if (first < 0 && start >= 0)
        first = start;
      if (start >= 0 && ((start % 86400) + 86400) % 86400 == kServerTimeOfDay)
        break;
      sleep_ms(250);
    }
    INFO("zone setting now '%s', manual offset '%s'", g_settings["recurring_rule_timezone"].c_str(),
         g_settings["recurring_rule_utc_offset_minutes"].c_str());
    INFO("rule start %ld (first seen %ld), time of day %ld; the server's is %ld, the table's %ld", start, first,
         ((start % 86400) + 86400) % 86400, kServerTimeOfDay, kTableTimeOfDay);
    if (g_settings["recurring_rule_timezone"] != "Asia/Tokyo")
      FAIL("the zone setting was not selected from the server's zone, so the check did not run against the table");
    if (((start % 86400) + 86400) % 86400 != kServerTimeOfDay)
      FAIL("the rule's start does not follow the offset the server applied");
    // One evaluation is not enough to move every rule: the first start seen is the table's, whatever it settles on.
    if (first >= 0 && ((first % 86400) + 86400) % 86400 != kTableTimeOfDay)
      FAIL("the offset was adopted on a single evaluation (the first start seen was already the server's)");
  }
  else if (what == "dtor_busy")
  {
    // destroy while Kodi-facing calls are idle but the addon's own threads are mid-fetch (slow server)
    ctl("epg_slow_ms=8000&channels_slow_ms=8000");
    sleep_ms(dur);
  }
  else
    FAIL("unknown scenario %s", what.c_str());
  if (faultT.joinable())
    faultT.join();
  INFO("final api_key setting '%s'", g_settings["api_key"].c_str());
  long d0 = ms();
  delete pvr;
  INFO("destroyed in %ld ms", ms() - d0);
  printf("stats=%s\n", http("/__stats").c_str());
  printf("RESULT fails=%d\n", (int)g_fail);
  curl_global_cleanup();
  return g_fail ? 1 : 0;
}
