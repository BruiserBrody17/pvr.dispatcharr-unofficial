// Drives PVRDispatcharr through Kodi's real C function table (the KodiToAddonFuncTable_PVR the
// dev-kit's CInstancePVRClient fills in), against fake_dispatcharr.py, from several threads.
#include "PVRDispatcharr.h"
#include "kodi_abi.h"
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
extern std::mutex g_connMu;
extern std::mutex g_notifyMu;
extern std::vector<std::string> g_notifyMessages;
extern std::vector<int> g_connCalls;
extern std::vector<std::string> g_connMessages;
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
extern std::atomic<const char*> g_logGate, g_logWatch;
extern std::atomic<bool> g_logGateHit, g_logGateOpen;
extern std::atomic<int> g_logWatchCount;

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
  abi::ReadCaps(I());
  abi::BackendName(I());
  abi::BackendVersion(I());
  abi::ConnectionString(I());
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
  abi::TimerTypeCount(I());
  A()->GetTimersAmount(I(), &n);
  A()->GetTimers(I(), (PVR_HANDLE)&c);
  A()->GetRecordingsAmount(I(), false, &n);
  A()->GetRecordings(I(), (PVR_HANDLE)&c, false);
  return e;
}

// The states the addon told Kodi through ConnectionStateChange, as the names the connection scenarios use.
static std::string ConnName(int state)
{
  switch (state)
  {
  case PVR_CONNECTION_STATE_CONNECTED:
    return "connected";
  case PVR_CONNECTION_STATE_ACCESS_DENIED:
    return "denied";
  case PVR_CONNECTION_STATE_SERVER_UNREACHABLE:
    return "unreachable";
  case PVR_CONNECTION_STATE_DISCONNECTED:
    return "lost";
  case PVR_CONNECTION_STATE_SERVER_MISMATCH:
    return "mismatch";
  default:
    return "state" + std::to_string(state);
  }
}

static std::vector<std::string> ConnSnapshot()
{
  std::lock_guard<std::mutex> lock(g_connMu);
  std::vector<std::string> out;
  for (int state : g_connCalls)
    out.push_back(ConnName(state));
  return out;
}

static std::string Joined(const std::vector<std::string>& v)
{
  std::string out;
  for (const auto& e : v)
    out += (out.empty() ? "" : ",") + e;
  return out;
}

// One manual timer request, the steady traffic the mid-session connection scenarios need (a failed one is a failed
// request).
static PVR_ERROR ConnTraffic(const std::vector<PVR_CHANNEL>& chans)
{
  PVR_TIMER t{};
  t.iTimerType = 1;
  t.iClientChannelUid = (int)chans[0].iUniqueId;
  t.state = PVR_TIMER_STATE_SCHEDULED;
  t.startTime = time(nullptr) + 7200;
  t.endTime = t.startTime + 1800;
  t.iEpgUid = PVR_TIMER_NO_EPG_UID;
  abi::setf(t.strTitle, "loss probe");
  return A()->AddTimer(I(), &t);
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
  abi::setf(t.strTitle, "T%u", (unsigned)rng());
  if (A()->AddTimer(I(), &t) != PVR_ERROR_NO_ERROR)
    fails++;
  // series
  PVR_TIMER s{};
  s.iTimerType = 2;
  s.iClientChannelUid = (int)ch.iUniqueId;
  abi::setf(s.strTitle, "Show %u-1", ch.iChannelNumber);
  abi::setf(s.strEpgSearchString, "Show %u-1", ch.iChannelNumber);
  s.state = PVR_TIMER_STATE_SCHEDULED;
  s.bStartAnyTime = true;
  s.bEndAnyTime = true;
  if (A()->AddTimer(I(), &s) != PVR_ERROR_NO_ERROR)
    fails++;
  // recurring
  PVR_TIMER r{};
  r.iTimerType = 4;
  r.iClientChannelUid = (int)ch.iUniqueId;
  abi::setf(r.strTitle, "Rec %u", (unsigned)rng());
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
    abi::setf(u.strTitle, "%s!", abi::str(x.strTitle).c_str());
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
    abi::ChannelStreamPropertyCount(I(), &ch);
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
    abi::RecordingStreamPropertyCount(I(), &r);
    abi::RecordedStream stream;
    if (abi::OpenRecorded(I(), &r, stream))
    {
      std::atomic<bool> stop{false};
      std::thread times(
          [&]
          {
            while (!stop)
            {
              PVR_STREAM_TIMES st{};
              abi::RecordedStreamTimes(I(), stream, st);
              abi::LengthRecorded(I(), stream);
              sleep_ms(50);
            }
          });
      for (int i = 0; i < 15; i++)
      {
        abi::ReadRecorded(I(), stream, buf.data(), (unsigned)buf.size());
        if (i == 7)
          abi::SeekRecorded(I(), stream, 0, SEEK_SET);
      }
      stop = true;
      times.join();
      abi::CloseRecorded(I(), stream);
    }
    abi::RecordingEdl(I(), &r);
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
    // The backend hostname is the configured host alone: no scheme, port or brackets (the connection string has those).
    {
      std::string host;
      PVR_ERROR he = abi::BackendHostname(I(), host);
      if (he != PVR_ERROR_NO_ERROR || host != "127.0.0.1")
        FAIL("the backend hostname was '%s' (err %d), expected the bare configured host", host.c_str(), (int)he);
    }
    // Radio: the fake's one radio channel (id 7, group 2, which also holds TV channels) is in the radio list only,
    // and the group is listed under both kinds with the members split the same way.
    {
      if (!abi::ReadCaps(I()).radio)
        FAIL("radio support was not declared");
      Collected r;
      A()->GetChannels(I(), (PVR_HANDLE)&r, true);
      if (r.channels.size() != 1 || r.channels[0].iUniqueId != 7 || !r.channels[0].bIsRadio)
        FAIL("the radio list holds %zu channels, expected only channel 7", r.channels.size());
      for (auto& ch : c.channels)
        if (ch.iUniqueId == 7 || ch.bIsRadio)
          FAIL("the TV list holds radio channel %u", ch.iUniqueId);
      for (bool radio : {false, true})
      {
        Collected gs;
        A()->GetChannelGroups(I(), (PVR_HANDLE)&gs, radio);
        bool found = false;
        for (auto& gr : gs.groups)
        {
          if (gr.bIsRadio != radio)
            FAIL("a group listed with radio=%d is flagged radio=%d", (int)radio, (int)gr.bIsRadio);
          Collected mem;
          A()->GetChannelGroupMembers(I(), (PVR_HANDLE)&mem, &gr);
          for (auto& m : mem.members)
            if ((m.iChannelUniqueId == 7) != radio)
              FAIL("group member %u is in the wrong kind of group (radio=%d)", m.iChannelUniqueId, (int)radio);
          if (!mem.members.empty())
            found = true;
        }
        if (!found)
          FAIL("no group with members was listed for radio=%d", (int)radio);
      }
    }
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
    // The guide fetch names the channel-id source and the catch-up depth of the fake's channels (3), and no forward
    // window.
    {
      std::string q = http("/__epgquery");
      if (q.find("\"tvg_id_source\": \"channel_number\"") == std::string::npos ||
          q.find("\"prev_days\": \"3\"") == std::string::npos || q.find("\"days\": ") != std::string::npos)
        FAIL("the guide was fetched with the query %s", q.c_str());
    }
    // Recording artwork and numbering: a recording whose programme had a poster gets it (the cached copy on the server,
    // by logo id) and its season and episode; one that only carries the channel's own logo as poster_logo_id gets no
    // artwork and no numbers.
    {
      Collected artRecs;
      A()->GetRecordings(I(), (PVR_HANDLE)&artRecs, false);
      const PVR_RECORDING* enriched = nullptr;
      const PVR_RECORDING* plain = nullptr;
      for (const auto& rec : artRecs.recordings)
      {
        if (abi::str(rec.strRecordingId) == "908")
          enriched = &rec;
        else if (abi::str(rec.strRecordingId) == "909")
          plain = &rec;
      }
      if (!enriched || !plain)
        FAIL("the enriched or the plain recording is missing from the list");
      else
      {
        const std::string thumb = abi::str(enriched->strThumbnailPath);
        if (thumb.find("/api/channels/logos/31/cache/") == std::string::npos)
          FAIL("a recording with a poster has the thumbnail '%s'", thumb.c_str());
        if (enriched->iSeriesNumber != 4 || enriched->iEpisodeNumber != 44)
          FAIL("a recording with season 4 episode 44 shows %d / %d", enriched->iSeriesNumber, enriched->iEpisodeNumber);
        if (!abi::str(plain->strThumbnailPath).empty())
          FAIL("a recording with only the channel's logo has the thumbnail '%s'", plain->strThumbnailPath);
        if (plain->iSeriesNumber != PVR_RECORDING_INVALID_SERIES_EPISODE ||
            plain->iEpisodeNumber != PVR_RECORDING_INVALID_SERIES_EPISODE)
          FAIL("a recording with no numbers shows %d / %d", plain->iSeriesNumber, plain->iEpisodeNumber);
      }
    }
    // A recording nothing titled shows its channel's name, not "Recording <id>"; one with a title keeps it.
    {
      Collected titleRecs;
      A()->GetRecordings(I(), (PVR_HANDLE)&titleRecs, false);
      std::string untitled, titled;
      for (const auto& rec : titleRecs.recordings)
      {
        if (abi::str(rec.strRecordingId) == "910")
          untitled = abi::str(rec.strTitle);
        else if (abi::str(rec.strRecordingId) == "908")
          titled = abi::str(rec.strTitle);
      }
      if (untitled != "Channel 1")
        FAIL("a recording with no title shows '%s', expected its channel's name", untitled.c_str());
      if (titled != "Enriched show")
        FAIL("a recording with a title shows '%s'", titled.c_str());
    }
    // The EDL: what comskip left decides whether the plugin is asked at all, and a break over the whole recording is
    // dropped. Three hour-long recordings in the fake: no commercials found (905), an ordinary break (906), one break
    // over the whole hour (907). The fake's plugin answers with a break for each whatever the metadata says.
    {
      Collected edlRecs;
      A()->GetRecordings(I(), (PVR_HANDLE)&edlRecs, false);
      auto edlCalls = [&](int rid) -> bool
      { return http("/__stats").find("\"edl " + std::to_string(rid) + "\"") != std::string::npos; };
      auto edlFor = [&](int rid, int& n) -> PVR_EDL_ENTRY
      {
        PVR_RECORDING rec{};
        abi::setf(rec.strRecordingId, "%d", rid);
        const std::vector<PVR_EDL_ENTRY> entries = abi::RecordingEdl(I(), &rec);
        n = (int)entries.size();
        return n > 0 ? entries[0] : PVR_EDL_ENTRY{};
      };
      int n905 = -1, n906 = -1, n907 = -1;
      edlFor(905, n905);
      PVR_EDL_ENTRY e906 = edlFor(906, n906);
      edlFor(907, n907);
      if (n905 != 0 || edlCalls(905))
        FAIL("a recording comskip found no commercials in gave %d EDL entries and the plugin %s asked", n905,
             edlCalls(905) ? "was" : "was not");
      if (n906 != 1 || e906.start != 300000 || !edlCalls(906))
        FAIL("an ordinary break was not passed on (%d entries, start %lld)", n906, (long long)e906.start);
      if (n907 != 0 || !edlCalls(907))
        FAIL("a break over the whole recording gave %d entries (plugin asked: %d), expected none after asking", n907,
             (int)edlCalls(907));
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
        abi::setf(t.strTitle, "%s", c.epgTitles[i].c_str());
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
    // The recording it created carries the guide programme the search found (its id and title, the way Dispatcharr's
    // own guide posts it), so the server's 80% time-slot match has nothing left to fail at. Without the search endpoint
    // (an older Dispatcharr) the timer still gets created, with only the window, as before.
    {
      std::string db = http("/__db");
      if (db.find("\"title\": \"Guide programme\"") == std::string::npos ||
          db.find("\"id\": 4242") == std::string::npos || db.find("From the programme search") == std::string::npos)
        FAIL("the EPG-based recording did not carry the looked-up programme: %s", db.c_str());
      http("/__ctl?program_search=off");
      PVR_TIMER later{};
      later.iTimerType = 3;
      later.iClientChannelUid = 6;
      later.state = PVR_TIMER_STATE_SCHEDULED;
      for (size_t i = 0; i < c.epg.size(); i++)
        if (c.epg[i].iUniqueChannelId == 6 && c.epg[i].startTime > t.startTime)
        {
          later.startTime = c.epg[i].startTime;
          later.endTime = c.epg[i].endTime;
          later.iEpgUid = c.epg[i].iUniqueBroadcastId;
          abi::setf(later.strTitle, "%s", c.epgTitles[i].c_str());
          break;
        }
      if (!later.startTime)
        FAIL("no second upcoming guide entry on channel 6");
      mustAdd("epg-based, no programme search", &later);
      http("/__ctl?program_search=");
      std::string db2 = http("/__db");
      size_t guide = 0, n = 0;
      while ((guide = db2.find("Guide programme", guide)) != std::string::npos)
      {
        n++;
        guide++;
      }
      if (n != 1)
        FAIL("a timer made without the programme search still carried a looked-up programme (%zu)", n);
    }
    // instant recording: start 0, and named after the channel, which is what Kodi sends for one
    PVR_TIMER inst{};
    inst.iTimerType = 1;
    inst.iClientChannelUid = 2;
    inst.startTime = 0;
    inst.endTime = now + 1800;
    inst.iEpgUid = PVR_TIMER_NO_EPG_UID;
    abi::setf(inst.strTitle, "Channel 2");
    mustAdd("instant", &inst);
    PVR_TIMER r{};
    r.iTimerType = 4;
    r.iClientChannelUid = 1;
    abi::setf(r.strTitle, "Daily");
    r.startTime = now + 3600;
    r.endTime = now + 5400;
    r.firstDay = now;
    r.iWeekdays = 0x1F;
    mustAdd("recurring", &r);
    PVR_TIMER s{};
    s.iTimerType = 2;
    s.iClientChannelUid = 3;
    abi::setf(s.strTitle, "Show 3-1");
    abi::setf(s.strEpgSearchString, "Show 3-1");
    s.bStartAnyTime = s.bEndAnyTime = true;
    mustAdd("series", &s);
    // series_rule_title_mode: a rule created under the default (Exact) sends no title_mode at all, which is exactly
    // what it sent before the setting existed; Contains and Whole words send that mode; a value outside the list sends
    // none. The setting is read when a rule is created, so it is changed between the adds, as the settings dialog does.
    {
      auto ruleBody = [&](const std::string& db, const char* title) -> std::string
      {
        const size_t at = db.find(std::string("\"title\": \"") + title + "\"");
        if (at == std::string::npos)
          return "";
        return db.substr(at, db.find('}', at) - at);
      };
      auto addSeriesUnder = [&](const char* mode, const char* title)
      {
        set("series_rule_title_mode", mode);
        pvr->OnAddonSettingChanged("series_rule_title_mode", kodi::addon::CSettingValue(mode));
        PVR_TIMER x{};
        x.iTimerType = 2;
        x.iClientChannelUid = 3;
        abi::setf(x.strTitle, "%s", title);
        abi::setf(x.strEpgSearchString, "%s", title);
        x.bStartAnyTime = x.bEndAnyTime = true;
        mustAdd(title, &x);
      };
      addSeriesUnder("1", "Show 3-2");
      addSeriesUnder("2", "Show 3-3");
      addSeriesUnder("7", "Show 3-4");
      addSeriesUnder("0", "Show 3-5");
      const std::string dbRules = http("/__db");
      auto check = [&](const char* title, const char* wanted)
      {
        const std::string body = ruleBody(dbRules, title);
        if (body.empty())
          FAIL("the series rule '%s' never reached the server: %s", title, dbRules.c_str());
        else if (wanted[0] == 0 && body.find("title_mode") != std::string::npos)
          FAIL("series rule '%s' sent a title_mode it should not have: %s", title, body.c_str());
        else if (wanted[0] != 0 && body.find(std::string("\"title_mode\": \"") + wanted + "\"") == std::string::npos)
          FAIL("series rule '%s' should carry title_mode '%s': %s", title, wanted, body.c_str());
      };
      check("Show 3-1", "");
      check("Show 3-2", "contains");
      check("Show 3-3", "search");
      check("Show 3-4", "");
      check("Show 3-5", "");
    }
    // A series rule with no title (a description-only rule) must never be deleted from Kodi, nor re-keyed by an edit
    // that would have to delete it afterwards: Dispatcharr's delete of such a rule also removes every upcoming
    // recording on its channel (every one, for a channel-less rule), confirmed live. The rule is seeded straight into
    // the fake, as Dispatcharr's web interface can make one. A titled rule still deletes, and an edit that keeps the
    // rule's identity still goes through.
    {
      auto stat = [&](const char* key) -> long
      {
        const std::string s = http("/__stats");
        const std::string k = std::string("\"") + key + "\": ";
        const size_t at = s.find(k);
        return at == std::string::npos ? 0 : atol(s.c_str() + at + k.size());
      };
      ctl("seed_empty_title_rule=1");
      Collected got;
      A()->GetTimers(I(), (PVR_HANDLE)&got);
      const PVR_TIMER* bare = nullptr;
      const PVR_TIMER* titled = nullptr;
      for (const auto& x : got.timers)
      {
        if (x.iTimerType != 2)
          continue;
        if (abi::str(x.strTitle).empty())
          bare = &x;
        else if (abi::str(x.strTitle) == "Show 3-5")
          titled = &x;
      }
      if (!bare)
        FAIL("the seeded series rule with no title was not listed as a timer");
      else
      {
        const long deletes = stat("DELETE /api/channels/series-rules/");
        const long posts = stat("POST /api/channels/series-rules/");
        PVR_TIMER rekey = *bare;
        abi::setf(rekey.strTitle, "Re-keyed");
        abi::setf(rekey.strEpgSearchString, "Re-keyed");
        if (A()->UpdateTimer(I(), &rekey) == PVR_ERROR_NO_ERROR)
          FAIL("an edit that re-keys a series rule with no title was accepted");
        PVR_TIMER del = *bare;
        if (A()->DeleteTimer(I(), &del, false) == PVR_ERROR_NO_ERROR)
          FAIL("a series rule with no title was deleted from Kodi");
        if (stat("DELETE /api/channels/series-rules/") != deletes)
          FAIL("a DELETE for the series rule with no title reached the server (it purges the channel's recordings)");
        if (stat("POST /api/channels/series-rules/") != posts)
          FAIL("the refused re-keying still posted a series rule");
        const std::string db = http("/__db");
        if (db.find("zzz description filter") == std::string::npos)
          FAIL("the series rule with no title is gone from the server: %s", db.c_str());
        if (db.find("Re-keyed") != std::string::npos)
          FAIL("the refused re-keying left a rule behind: %s", db.c_str());
        // An edit that keeps the rule's identity (same empty title, same channel) is an in-place upsert.
        PVR_TIMER same = *bare;
        same.iPreventDuplicateEpisodes = 1;
        if (A()->UpdateTimer(I(), &same) != PVR_ERROR_NO_ERROR)
          FAIL("an edit that keeps a series rule's identity was refused");
        if (stat("POST /api/channels/series-rules/") != posts + 1)
          FAIL("the identity-keeping edit of a series rule with no title was not posted");
      }
      if (!titled)
        FAIL("the control series rule 'Show 3-5' was not listed as a timer");
      else
      {
        const long deletes = stat("DELETE /api/channels/series-rules/");
        PVR_TIMER del = *titled;
        if (A()->DeleteTimer(I(), &del, false) != PVR_ERROR_NO_ERROR)
          FAIL("a titled series rule could not be deleted");
        if (stat("DELETE /api/channels/series-rules/") != deletes + 1)
          FAIL("deleting a titled series rule did not reach the server");
      }
    }
    // A manual timer's typed name is written to the server (it would otherwise give way to the channel's name once the
    // pending-title cache expires); a manual timer that carries the channel's name, which is what Kodi's dialog sends
    // for an empty or default name, and a guide timer are not (writing marks the recording user_edited, which stops
    // Dispatcharr's own enrichment).
    {
      auto addManual = [&](const char* title)
      {
        PVR_TIMER m{};
        m.iTimerType = 1;
        m.iClientChannelUid = 4;
        m.state = PVR_TIMER_STATE_SCHEDULED;
        m.startTime = now + 10800;
        m.endTime = m.startTime + 1800;
        m.iEpgUid = PVR_TIMER_NO_EPG_UID;
        abi::setf(m.strTitle, "%s", title);
        mustAdd(title, &m);
      };
      addManual("Typed manual name");
      addManual("Channel 4");
      const std::string dbTitles = http("/__db");
      const size_t typedAt = dbTitles.find("\"title\": \"Typed manual name\"");
      if (typedAt == std::string::npos)
        FAIL("a manual timer's typed name was not stored on the server: %s", dbTitles.c_str());
      else if (dbTitles.find("\"user_edited\": true", typedAt > 200 ? typedAt - 200 : 0) == std::string::npos)
        FAIL("the stored typed name is not marked user_edited: %s", dbTitles.c_str());
      // The one with the channel's name stored nothing, and the guide timer added earlier stored no rename either: the
      // only user_edited recording is the typed one.
      size_t edited = 0, at = 0;
      while ((at = dbTitles.find("\"user_edited\"", at)) != std::string::npos)
      {
        edited++;
        at++;
      }
      if (edited != 1)
        FAIL("expected exactly one recording renamed by AddTimer, found %d: %s", (int)edited, dbTitles.c_str());
    }
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
        if (x.iTimerType == 4 && abi::str(x.strTitle) == "ZZZ offset rule")
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
  else if (what == "connection_state")
  {
    // What the addon tells Kodi about the link (ConnectionStateChange), in order. argv[4] is the expected sequence, a
    // comma-separated list of connected, denied, unreachable, mismatch (empty: none). The fake's faults come from CTL
    // (login_status=401, api_down=1), set before the constructor ran. With CONN_RECOVER=1 the scenario clears api_down
    // after the first report and waits (up to `dur`) for the rest; otherwise it waits the whole `dur` so a spurious
    // extra report is caught too.
    const std::string expectArg = argc > 4 ? argv[4] : "";
    std::vector<std::string> expected;
    for (size_t p = 0; !expectArg.empty() && p <= expectArg.size();)
    {
      size_t comma = expectArg.find(',', p);
      expected.push_back(expectArg.substr(p, comma == std::string::npos ? std::string::npos : comma - p));
      if (comma == std::string::npos)
        break;
      p = comma + 1;
    }
    const bool recover = getenv("CONN_RECOVER") != nullptr;
    bool cleared = false;
    const long began = ms();
    while (ms() - began < dur)
    {
      const auto now = ConnSnapshot();
      if (recover && !cleared && !now.empty())
      {
        INFO("first report %s; clearing the fault", now.front().c_str());
        ctl("api_down=0&login_status=");
        cleared = true;
      }
      if (recover && now.size() >= expected.size())
        break;
      sleep_ms(100);
    }
    const auto got = ConnSnapshot();
    const std::string gotText = Joined(got), wantText = Joined(expected);
    INFO("reported to Kodi after %ld ms: [%s], expected [%s]", ms() - began, gotText.c_str(), wantText.c_str());
    if (got != expected)
      FAIL("the addon reported [%s] to Kodi, expected [%s]", gotText.c_str(), wantText.c_str());
    {
      std::lock_guard<std::mutex> lock(g_connMu);
      for (const auto& m : g_connMessages)
        if (!m.empty())
          FAIL("a report carried the message '%s'; an empty one makes Kodi use its own localized text", m.c_str());
    }
  }
  else if (what == "dvr_access")
  {
    // A view-only account (the fake's user_level=1, set through CTL before the constructor ran) must lose the Record,
    // delete and rename actions, and the addon must not tell the user to restart Kodi. argv[4] "late" starts the fake
    // with api_down=1 (CTL) and lifts it after the first capabilities read, which is a server that is unreachable when
    // Kodi starts: Kodi's first read sees the fail-open answer, and the level is learned on the background thread's
    // retry (about a minute later). The addon reports CONNECTED only once the level is known, and Kodi re-reads the
    // capabilities on that report (CPVRClient::SetConnectionState), which this scenario does by hand; the second read
    // must be the restricted one.
    const bool late = argc > 4 && std::string(argv[4]) == "late";
    auto readCaps = [] { return abi::ReadCaps(I()); };
    const abi::Caps first = readCaps();
    if (late && !first.timers)
      FAIL("the first capabilities read, taken before the level was known, already hid the timers; the late path was "
           "not exercised");
    if (!first.recordings)
      FAIL("the first capabilities read did not offer recordings");
    if (late)
      ctl("api_down=0");
    const long began = ms();
    bool connected = false;
    while (ms() - began < dur && !connected)
    {
      for (const auto& state : ConnSnapshot())
        connected = connected || state == "connected";
      if (!connected)
        sleep_ms(100);
    }
    INFO("connected=%d after %ld ms", connected ? 1 : 0, ms() - began);
    if (!connected)
      FAIL("the addon never reported CONNECTED, so Kodi would never re-read the capabilities");
    const abi::Caps second = readCaps();
    if (second.timers || second.recordingsDelete || second.recordingsRename)
      FAIL("a view-only account is still offered timers=%d delete=%d rename=%d after CONNECTED", second.timers,
           second.recordingsDelete, second.recordingsRename);
    if (!second.recordings)
      FAIL("a view-only account lost the recordings list");
    {
      // Other notifications are legitimate here (the harness runs Server-side live mode, which a non-admin account
      // cannot use); only the old "restart Kodi" advice is wrong.
      std::lock_guard<std::mutex> lock(g_notifyMu);
      for (const auto& m : g_notifyMessages)
        if (m.find("Restart Kodi") != std::string::npos || m.find("view-only") != std::string::npos)
          FAIL("the addon told the user '%s'; Kodi re-reads the capabilities on CONNECTED, so no restart is needed",
               m.c_str());
    }
  }
  else if (what == "connection_loss")
  {
    // A server that goes away mid-session, with the token still valid: no login happens, so only the run of failed
    // requests (LinkHealth) can say so. Expects connected, then lost once the failures span about ten seconds, then
    // connected again when a request succeeds after the server is back. AddTimer is the traffic, one call a second.
    auto waitFor = [](size_t count, long limitMs)
    {
      const long began = ms();
      while (ms() - began < limitMs && ConnSnapshot().size() < count)
        sleep_ms(100);
      return ConnSnapshot();
    };
    waitFor(1, 6000);
    Collected c0;
    all_lists(c0, false);
    if (c0.channels.empty())
      FAIL("the connection_loss scenario loaded no channels");
    else
    {
      if (ConnSnapshot() != std::vector<std::string>{"connected"})
        FAIL("expected [connected] before the outage, got [%s]", Joined(ConnSnapshot()).c_str());
      ctl("api_down=1");
      const long outageAt = ms();
      std::vector<std::string> now;
      while (ms() - outageAt < 40000 && (now = ConnSnapshot()).size() < 2)
      {
        ConnTraffic(c0.channels);
        sleep_ms(1000);
      }
      INFO("%ld ms after the server went away: [%s]", ms() - outageAt, Joined(now).c_str());
      const long lostAfter = ms() - outageAt;
      if (now != std::vector<std::string>{"connected", "lost"})
        FAIL("expected [connected,lost] after the outage, got [%s]", Joined(now).c_str());
      else if (lostAfter < 9000)
        FAIL("the loss was reported after %ld ms, before the failures could span ten seconds", lostAfter);
      // More failures while it is still down must not say anything else (never "unreachable" after "lost").
      for (int i = 0; i < 4; i++)
      {
        ConnTraffic(c0.channels);
        sleep_ms(1000);
      }
      if (ConnSnapshot().size() != 2)
        FAIL("more was reported while the server was still down: [%s]", Joined(ConnSnapshot()).c_str());
      ctl("api_down=0");
      const long backAt = ms();
      while (ms() - backAt < 20000 && ConnSnapshot().size() < 3)
      {
        ConnTraffic(c0.channels);
        sleep_ms(500);
      }
      INFO("%ld ms after the server came back: [%s]", ms() - backAt, Joined(ConnSnapshot()).c_str());
      if (ConnSnapshot() != std::vector<std::string>{"connected", "lost", "connected"})
        FAIL("expected [connected,lost,connected], got [%s]", Joined(ConnSnapshot()).c_str());
    }
  }
  else if (what == "connection_blip")
  {
    // A short outage (five seconds of failed requests) is not a lost connection: nothing may be reported beyond the
    // first connected, neither during the outage nor when the server returns.
    const long began = ms();
    while (ms() - began < 6000 && ConnSnapshot().empty())
      sleep_ms(100);
    Collected c0;
    all_lists(c0, false);
    if (c0.channels.empty())
      FAIL("the connection_blip scenario loaded no channels");
    else
    {
      ctl("api_down=1");
      for (int i = 0; i < 5; i++)
      {
        ConnTraffic(c0.channels);
        sleep_ms(1000);
      }
      ctl("api_down=0");
      for (int i = 0; i < 6; i++)
      {
        ConnTraffic(c0.channels);
        sleep_ms(500);
      }
      if (ConnSnapshot() != std::vector<std::string>{"connected"})
        FAIL("a five-second outage reported [%s], expected only [connected]", Joined(ConnSnapshot()).c_str());
    }
  }
  else if (what == "finished_hls_gone")
  {
    // A recording the server ended long ago whose `_hls_dir` key was never cleared (Dispatcharr clears it only after a
    // successful remux; found live 2026-10-18 on an interrupted recording) is opened on the HLS path, whose playlist is
    // gone. The open falls back to the finished file.
    ctl("ip_reset=20&ip_state=interrupted&ip_hls_key=1&ip_dir_gone=1&ip_file=1");
    PVR_RECORDING r{};
    abi::setf(r.strRecordingId, "%d", 7);
    r.recordingTime = time(nullptr) - 600;
    abi::RecordedStream st;
    if (!abi::OpenRecorded(I(), &r, st))
      FAIL("a finished recording whose playlist redirects to its file did not open");
    else
    {
      std::vector<unsigned char> buf(4096);
      const int n = abi::ReadRecorded(I(), st, buf.data(), (unsigned)buf.size());
      const uint32_t first =
          n >= 4 ? (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8) | buf[3] : 0;
      if (first != 0xC0FFEE00u)
        FAIL("the fallback read returned %d byte(s) starting %08x, not the finished file's", n, first);
      if (A()->IsRealTimeStream(I()))
        FAIL("the fallback stream is reported as real-time; it is a finished file");
      abi::CloseRecorded(I(), st);
    }
    // The shape seen live: the HLS directory is there but never got a playlist (404) and there is no file. The open
    // still fails, but without a notification: Kodi 22's thumbnail loader makes opens nobody asked to watch.
    ctl("ip_dir_gone=0&ip_nodir=1&ip_file=0");
    const long notifiesBefore = g_notifyCount;
    abi::RecordedStream empty;
    if (abi::OpenRecorded(I(), &r, empty))
    {
      FAIL("a finished recording with no playlist and no file opened");
      abi::CloseRecorded(I(), empty);
    }
    if (g_notifyCount != notifiesBefore)
      FAIL("the failed fallback showed %ld notification(s)", g_notifyCount - notifiesBefore);
    ctl("ip_state=&ip_hls_key=0&ip_nodir=0");
  }
  else if (what == "dtor_late_report")
  {
    // The connection report the channel/EPG thread schedules when it learns the account's DVR access level late
    // (RetryDeferredServerSyncs()) must not be started once the destructor has drained the detached-thread list: that
    // thread is stopped only after the drain, and a task tracked after it was a joinable std::thread destroyed with the
    // list, which is std::terminate. The fake fails /users/me/ (CTL me_status=500) until the first background cycle has
    // gone past the retry (the guide is fetched only by that thread, after it); then the level becomes readable as
    // view-only, which logs a line, a wake makes the thread retry at once, and the log gate parks the thread on that
    // line, between learning the level and scheduling the report. The destroy below starts while it is parked and
    // opens the gate once the drain is done.
    long began = ms();
    auto epgFetches = []
    {
      const std::string stats = http("/__stats");
      const size_t at = stats.find("\"GET /output/epg\": ");
      return at == std::string::npos ? 0 : atoi(stats.c_str() + at + 19);
    };
    while (ms() - began < 20000 && epgFetches() < 1)
      sleep_ms(100);
    if (epgFetches() < 1)
      FAIL("the background thread never fetched the guide, so its first retry of the access level was not seen");
    g_logGate = "DVR access is view-only";
    g_logWatch = "a background task was not started";
    ctl("me_status=0&user_level=1");
    A()->OnSystemWake(I());
    Collected c;
    A()->GetEPGForChannel(I(), (PVR_HANDLE)&c, 1, time(nullptr) - 3600, time(nullptr) + 3600);
    began = ms();
    while (ms() - began < 10000 && !g_logGateHit)
      sleep_ms(50);
    INFO("access level learned on the background thread: %d (after %ld ms)", g_logGateHit ? 1 : 0, ms() - began);
    if (!g_logGateHit)
      FAIL("the background thread did not retry the access level after the wake");
  }
  else if (what == "epg_redirect")
  {
    // The guide download is the one request curl follows redirects for itself (it carries no credentials). Run with
    // CTL="epg_redirect=loop" (a redirect to itself, forever) or "epg_redirect=ftp" (a redirect to an FTP URL): the
    // first must stop after five redirects, the second must never connect (before the limits, curl's default of 30
    // and its default protocols, FTP among them). Waits for the background thread's first guide fetch, which the
    // failure then holds off for a minute, so every request counted is that one fetch's.
    auto stat = [](const char* key)
    {
      const std::string stats = http("/__stats");
      const std::string needle = std::string("\"") + key + "\": ";
      const size_t at = stats.find(needle);
      return at == std::string::npos ? 0 : atoi(stats.c_str() + at + needle.size());
    };
    long began = ms();
    while (ms() - began < 20000 && stat("GET /output/epg") < 1)
      sleep_ms(100);
    sleep_ms(3000);
    const int requests = stat("GET /output/epg");
    const int ftpConnects = stat("ftp_trap_connects");
    INFO("guide requests %d, connections to the FTP target %d", requests, ftpConnects);
    if (requests < 1)
      FAIL("the background thread never fetched the guide");
    if (requests > 6)
      FAIL("%d guide requests for one fetch: the redirects were not limited to five", requests);
    if (ftpConnects != 0)
      FAIL("the guide fetch followed a redirect to FTP (%d connection(s))", ftpConnects);
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
  std::thread gateOpener;
  if (what == "dtor_late_report")
    gateOpener = std::thread(
        []
        {
          sleep_ms(500);
          g_logGateOpen = true;
        });
  delete pvr;
  INFO("destroyed in %ld ms", ms() - d0);
  if (gateOpener.joinable())
    gateOpener.join();
  if (what == "dtor_late_report" && g_logGateHit && g_logWatchCount < 1)
    FAIL("the connection report scheduled during the destroy was not refused (no 'not started' line), so the late "
         "schedule did not happen inside the shutdown");
  printf("stats=%s\n", http("/__stats").c_str());
  printf("RESULT fails=%d\n", (int)g_fail);
  curl_global_cleanup();
  return g_fail ? 1 : 0;
}
