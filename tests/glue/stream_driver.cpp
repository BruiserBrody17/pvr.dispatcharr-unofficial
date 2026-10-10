// Stream-path glue harness: drives DispatcharrClient's live-timeshift, completed-recording and
// in-progress-recording paths against fake_dispatcharr.py, from several threads, verifying every byte.
#include "DispatcharrClient.h"
#include "RequestTimeout.h"
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
using namespace dispatcharr;
void InitKodiStub();
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
static void sleep_ms(int m)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(m));
}

static int64_t seg_size(int64_t seq)
{
  return 8 * (1500 + (seq * 37) % 700);
}
static uint32_t be32(const uint8_t* p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Live verifier: each record is (0x5E00|seq16, k). base[seq] must stay consistent within a session,
// and consecutive seqs must be exactly seg_size apart.
struct LiveVerifier
{
  std::map<int64_t, int64_t> base;
  long records = 0;
  void reset()
  {
    base.clear();
  }
  // dry: returns false instead of failing when the very first record disagrees (a forward clamp inside Read)
  bool firstRecordConsistent(const uint8_t* b, int n, int64_t pos)
  {
    if (n < 8)
      return true;
    uint32_t tag = be32(b), k = be32(b + 4);
    int64_t seq = tag & 0xFFFF;
    int64_t bs = pos - (int64_t)k * 8;
    auto it = base.find(seq);
    if (it != base.end())
      return it->second == bs;
    auto prev = base.find(seq - 1);
    if (prev != base.end())
      return prev->second + seg_size(seq - 1) == bs;
    return true;
  }
  void check(const uint8_t* b, int n, int64_t pos, const char* who)
  {
    if (pos % 8 || n % 8)
    {
      FAIL("%s: unaligned read pos=%lld n=%d", who, (long long)pos, n);
      return;
    }
    for (int i = 0; i < n; i += 8)
    {
      uint32_t tag = be32(b + i), k = be32(b + i + 4);
      if ((tag & 0xFFFF0000u) != 0x5E000000u)
      {
        FAIL("%s: bad tag %08x at pos %lld", who, tag, (long long)(pos + i));
        return;
      }
      int64_t seq = tag & 0xFFFF;
      if ((int64_t)k * 8 >= seg_size(seq))
      {
        FAIL("%s: k %u beyond seg %lld size", who, k, (long long)seq);
        return;
      }
      int64_t bs = pos + i - (int64_t)k * 8;
      auto it = base.find(seq);
      if (it == base.end())
      {
        base[seq] = bs;
        auto prev = base.find(seq - 1);
        if (prev != base.end() && prev->second + seg_size(seq - 1) != bs)
        {
          FAIL("%s: seq %lld base %lld != prev %lld + %lld", who, (long long)seq, (long long)bs,
               (long long)prev->second, (long long)seg_size(seq - 1));
          return;
        }
        auto nx = base.find(seq + 1);
        if (nx != base.end() && bs + seg_size(seq) != nx->second)
        {
          FAIL("%s: seq %lld base mismatch with next", who, (long long)seq);
          return;
        }
      }
      else if (it->second != bs)
      {
        FAIL("%s: seq %lld base %lld != earlier %lld (pos %lld)", who, (long long)seq, (long long)bs,
             (long long)it->second, (long long)(pos + i));
        return;
      }
      records++;
    }
  }
};

// Completed recording: byte o is pack('>II', 0xC0FFEE00, o/8)[o%8]
static const int64_t REC_SIZE = 3 * 1024 * 1024 + 5;
static uint8_t rec_byte(int64_t o)
{
  uint8_t r[8];
  uint32_t a = 0xC0FFEE00u, k = (uint32_t)(o / 8);
  r[0] = a >> 24;
  r[1] = a >> 16;
  r[2] = a >> 8;
  r[3] = a;
  r[4] = k >> 24;
  r[5] = k >> 16;
  r[6] = k >> 8;
  r[7] = k;
  return r[o % 8];
}
// In-progress: concatenation of seg_bytes(0x1B, s, seg_size(s)) from s=0
static std::vector<int64_t> ipPrefix;
static uint8_t ip_byte(int64_t o)
{
  if (ipPrefix.empty())
  {
    ipPrefix.push_back(0);
    for (int s = 0; s < 20000; s++)
      ipPrefix.push_back(ipPrefix.back() + seg_size(s));
  }
  int64_t s = std::upper_bound(ipPrefix.begin(), ipPrefix.end(), o) - ipPrefix.begin() - 1;
  int64_t off = o - ipPrefix[s];
  uint32_t a = 0x1B000000u | (uint32_t)(s & 0xFFFF), k = (uint32_t)(off / 8);
  uint8_t r[8] = {uint8_t(a >> 24), uint8_t(a >> 16), uint8_t(a >> 8), uint8_t(a),
                  uint8_t(k >> 24), uint8_t(k >> 16), uint8_t(k >> 8), uint8_t(k)};
  return r[off % 8];
}

// The client's request timeout in seconds (the TMO environment variable, as run.sh passes it).
static int configuredTimeout()
{
  return getenv("TMO") ? atoi(getenv("TMO")) : 5;
}

static Config cfg()
{
  Config c;
  c.host = "127.0.0.1";
  c.port = g_port;
  c.username = "u";
  c.password = "p";
  c.timeoutSeconds = configuredTimeout();
  c.apiKey = "K1";
  return c;
}

// ---------------------------------------------------------------- live
static void live_play(DispatcharrClient& cl, int durMs, bool chaosSeekThread, const char* faultAtMs = nullptr)
{
  std::string err;
  long t = ms();
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  INFO("live open took %ld ms", ms() - t);
  std::atomic<bool> stop{false};
  LiveVerifier v;
  std::mutex vm;
  std::atomic<long> bytes{0}, reads{0}, neg{0}, zeros{0}, maxReadMs{0}, resyncs{0};
  std::thread reader(
      [&]
      {
        std::vector<uint8_t> buf(65536);
        std::mt19937 rng(1);
        int64_t pos = -1; // unknown until first seek
        pos = getenv("NOSEEK") ? 0 : cl.SeekLiveTimeshiftStream(0, SEEK_CUR);
        long lastSeek = ms();
        long pauseAt = getenv("PAUSE_AT") ? atol(getenv("PAUSE_AT")) : -1;
        long pauseMs = getenv("PAUSE_MS") ? atol(getenv("PAUSE_MS")) : 0;
        long startT = ms();
        bool noSeek = getenv("NOSEEK") != nullptr;
        while (!stop)
        {
          if (pauseAt >= 0 && ms() - startT >= pauseAt)
          {
            INFO("reader pausing %ld ms", pauseMs);
            sleep_ms(pauseMs);
            pauseAt = -1;
            INFO("reader resumes");
          }
          if (!chaosSeekThread && !noSeek && ms() - lastSeek > 1300)
          {
            lastSeek = ms();
            int kind = rng() % 4;
            int64_t np = -1;
            int64_t first = cl.FirstAvailableLiveByteOffset();
            int64_t len = cl.GetLiveTimeshiftStreamLength();
            if (kind == 0)
              np = cl.SeekLiveTimeshiftStream(0, SEEK_END);
            else if (kind == 1)
              np = cl.SeekLiveTimeshiftStream(first, SEEK_SET);
            else if (kind == 2)
              np = cl.SeekLiveTimeshiftStream(-8 * (int64_t)(rng() % 20000), SEEK_CUR);
            else
              np = cl.SeekLiveTimeshiftStream(first + ((len - first) / 16) * 8, SEEK_SET);
            if (np >= 0)
              pos = np;
            if (np >= 0 && np % 8)
              FAIL("seek kind %d landed unaligned %lld", kind, (long long)np);
          }
          long r0 = ms();
          int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
          long dt = ms() - r0;
          if (dt > maxReadMs)
            maxReadMs = dt;
          reads++;
          if (n < 0)
          {
            neg++;
            if (!cl.IsLiveTimeshiftStreamOpen())
              break;
            sleep_ms(5);
            continue;
          }
          if (n == 0)
          {
            zeros++;
            if (getenv("STOP_ON_EOF"))
            {
              INFO("reader EOF");
              break;
            }
            sleep_ms(20);
            continue;
          }
          if (chaosSeekThread)
          {
            std::lock_guard<std::mutex> g(vm); /* position unknown under chaos: check alignment only */
            if (n % 8)
              FAIL("chaos unaligned n");
          }
          else
          {
            // the stream may have skipped forward inside Read (position behind the oldest segment)
            if (!v.firstRecordConsistent(buf.data(), n, pos) && be32(buf.data() + 4) == 0)
            {
              int64_t after = cl.SeekLiveTimeshiftStream(0, SEEK_CUR);
              INFO("resync: tracked %lld, stream at %lld after a %d-byte read", (long long)pos, (long long)after, n);
              pos = after - n;
              resyncs++;
            }
            v.check(buf.data(), n, pos, "live");
            pos += n;
          }
          bytes += n;
        }
      });
  std::thread poller(
      [&]
      {
        while (!stop)
        {
          cl.GetLiveTimeshiftStreamLength();
          cl.GetLiveTimeshiftStreamDurationMs();
          cl.GetLiveTimeshiftStreamBeginMs();
          cl.GetLiveTimeshiftStreamWallClockAnchor();
          sleep_ms(getenv("POLL_MS") ? atoi(getenv("POLL_MS")) : 30);
        }
      });
  std::thread chaos;
  if (chaosSeekThread)
    chaos = std::thread(
        [&]
        {
          std::mt19937 r(2);
          while (!stop)
          {
            cl.SeekLiveTimeshiftStream(8 * (int64_t)(r() % 4000) - 16000, SEEK_CUR);
            if (r() % 5 == 0)
              cl.SeekLiveTimeshiftStream(0, SEEK_END);
            sleep_ms(r() % 40);
          }
        });
  if (faultAtMs)
  {
    // "delay:k=v;delay:k=v"
    std::string spec = faultAtMs;
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
  }
  long until = ms() + durMs;
  while (ms() < until)
    sleep_ms(20);
  // Close racing the reader (Kodi closes from its own thread while the demuxer may be inside Read)
  long c0 = ms();
  stop = true;
  cl.CloseLiveTimeshiftStream();
  INFO("close took %ld ms", ms() - c0);
  reader.join();
  poller.join();
  if (chaos.joinable())
    chaos.join();
  INFO("live: resyncs=%ld reads=%ld bytes=%ld neg=%ld zeros=%ld records=%ld segs_seen=%zu maxRead=%ldms "
       "open_after_close=%d",
       (long)resyncs, (long)reads, (long)bytes, (long)neg, (long)zeros, v.records, v.base.size(), (long)maxReadMs,
       cl.IsLiveTimeshiftStreamOpen());
  if ((long)bytes == 0)
    FAIL("the live stream delivered no bytes");
}

static void live_tail_close(DispatcharrClient& cl)
{
  std::string err;
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("live_freeze=1");
  std::vector<uint8_t> buf(1 << 20);
  std::atomic<long> lastRet{-99}, retAt{0};
  std::thread reader(
      [&]
      {
        for (int i = 0; i < 200; i++)
        {
          int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
          if (n <= 0)
          {
            lastRet = n;
            retAt = ms();
            if (n < 0 || !cl.IsLiveTimeshiftStreamOpen())
              break;
          }
        }
      });
  sleep_ms(1500);
  long c0 = ms();
  cl.CloseLiveTimeshiftStream();
  long c1 = ms();
  reader.join();
  INFO("tail close: Close took %ld ms; reader's last ret=%ld at +%ld ms after close start", c1 - c0, (long)lastRet,
       (long)retAt - c0);
  ctl("live_freeze=0");
  // Close must not wait for a reader that is blocked at the tail (the stop request is bounded, the read ends with it).
  if (c1 - c0 > 8000)
    FAIL("Close took %ld ms with a reader blocked at the live tail", c1 - c0);
  if (cl.IsLiveTimeshiftStreamOpen())
    FAIL("the stream was still open after Close");
}

// A reader parked at the live tail has to hand control back to Kodi often: Kodi acts on a close request only between
// reads, so one read that blocks for the whole catch-up budget (4 to 6 s measured on three devices) delays every
// close and channel change by that long. The wait is therefore taken in slices; a slice that finds nothing returns -1,
// which Kodi's demuxer retries (a 0 would end playback), and 0 comes only once the whole budget has been used. This
// freezes the live buffer and times each read at the tail, then lets it grow again and requires the stream to resume.
static void live_tail_bound(DispatcharrClient& cl)
{
  std::string err;
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("live_freeze=1");
  std::vector<uint8_t> buf(1 << 20);
  long maxTailReadMs = 0, tailWaitMs = 0, waitBeforeFirstZero = -1, minusOnes = 0, bytes = 0;
  bool sawZero = false;
  const long phaseStart = ms();
  // Phase 1: the buffer is frozen, so after the segments already there are consumed every read waits at the tail.
  while (ms() - phaseStart < 20000 && !sawZero)
  {
    const long t0 = ms();
    const int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
    const long took = ms() - t0;
    if (n > 0)
    {
      bytes += n;
      continue;
    }
    maxTailReadMs = std::max(maxTailReadMs, took);
    tailWaitMs += took;
    if (n < 0)
      minusOnes++;
    else
    {
      sawZero = true;
      waitBeforeFirstZero = tailWaitMs;
    }
  }
  INFO("tail bound: max read at the tail %ld ms, %ld retries (-1) before the first 0, waited %ld ms in all, "
       "bytes before the tail %ld",
       maxTailReadMs, minusOnes, waitBeforeFirstZero, bytes);
  if (!sawZero)
    FAIL("no read ever returned 0 in 20 s: the budget never ended");
  if (maxTailReadMs > 1800)
    FAIL("one read at the live tail blocked for %ld ms; Kodi cannot act on a close for that long", maxTailReadMs);
  if (waitBeforeFirstZero >= 0 && waitBeforeFirstZero < 3000)
    FAIL("the stream gave up (0) after only %ld ms at the tail; the budget must not shrink", waitBeforeFirstZero);
  if (minusOnes < 1)
    FAIL("no -1 was returned while waiting, so the wait was not handed back in slices");
  if (!cl.IsLiveTimeshiftStreamOpen())
    FAIL("the stream closed itself while waiting at the tail");

  // Phase 2: the buffer grows again; the same stream must deliver bytes again (a slice's -1 must never have ended it).
  ctl("live_freeze=0");
  long resumed = 0;
  const long phase2 = ms();
  while (ms() - phase2 < 20000 && resumed == 0)
  {
    const int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
    if (n > 0)
      resumed += n;
  }
  INFO("tail bound: after the buffer grew again %ld bytes arrived within %ld ms", resumed, ms() - phase2);
  if (resumed == 0)
    FAIL("the stream did not resume once new segments arrived");
}

// ---------------------------------------------------------------- completed recording
static void rec_play(DispatcharrClient& cl, const char* faults)
{
  std::string err;
  if (!cl.OpenRecordingStream(5, err))
  {
    FAIL("rec open: %s", err.c_str());
    return;
  }
  int64_t len = cl.GetRecordingStreamLength();
  if (len != REC_SIZE)
    FAIL("rec length %lld", (long long)len);
  std::atomic<bool> stop{false};
  std::thread poller(
      [&]
      {
        while (!stop)
        {
          cl.GetRecordingStreamLength();
          sleep_ms(10);
        }
      });
  std::thread faulter;
  if (faults)
    faulter = std::thread(
        [&]
        {
          std::string s = faults;
          size_t c = s.find(':');
          size_t c2 = s.find('|');
          std::string first = s.substr(c + 1, c2 == std::string::npos ? std::string::npos : c2 - c - 1);
          sleep_ms(atoi(s.substr(0, c).c_str()));
          INFO("fault %s", first.c_str());
          ctl(first);
          if (c2 != std::string::npos)
          {
            sleep_ms(3000);
            ctl(s.substr(c2 + 1));
            INFO("fault %s", s.substr(c2 + 1).c_str());
          }
        });
  std::vector<uint8_t> buf(131072 + 3);
  std::mt19937 rng(3);
  int64_t pos = 0;
  long n_reads = 0, bad = 0, neg = 0;
  int64_t got = 0;
  for (int iter = 0; iter < 400; iter++)
  {
    if (iter % 25 == 24)
    {
      int64_t t = rng() % REC_SIZE;
      int64_t np = cl.SeekRecordingStream(t, SEEK_SET);
      if (np != t)
        FAIL("seek to %lld got %lld", (long long)t, (long long)np);
      pos = np;
    }
    if (iter % 37 == 36)
    {
      int64_t np = cl.SeekRecordingStream(-7, SEEK_END);
      pos = np;
    }
    unsigned want = 1 + rng() % buf.size();
    long r0 = ms();
    int n = cl.ReadRecordingStream(buf.data(), want);
    long dt = ms() - r0;
    if (dt > 1500)
      INFO("rec read took %ld ms -> %d", dt, n);
    n_reads++;
    if (n < 0)
    {
      neg++;
      continue;
    }
    if (n == 0)
    {
      if (pos < REC_SIZE && !faults)
        FAIL("rec EOF at %lld", (long long)pos);
      pos = cl.SeekRecordingStream(0, SEEK_SET);
      continue;
    }
    for (int i = 0; i < n; i++)
      if (buf[i] != rec_byte(pos + i))
      {
        bad++;
        FAIL("rec byte mismatch at %lld", (long long)(pos + i));
        break;
      }
    pos += n;
    got += n;
  }
  stop = true;
  poller.join();
  if (faulter.joinable())
    faulter.join();
  cl.CloseRecordingStream();
  INFO("rec: reads=%ld bytes=%lld neg=%ld bad=%ld", n_reads, (long long)got, neg, bad);
  if (got == 0)
    FAIL("the recording delivered no bytes");
  if (neg > 0 && !faults)
    FAIL("%ld reads of a completed recording failed with no fault injected", neg);
}

// ---------------------------------------------------------------- in-progress recording
static void ip_play(DispatcharrClient& cl, int recordMs, const char* faults, bool chaos)
{
  ctl(std::string("ip_reset=") + (getenv("IP_AGE") ? getenv("IP_AGE") : "1"));
  if (getenv("IP_AGE"))
  {
    int64_t skip = 0;
    for (int s2 = 0; s2 < atoi(getenv("IP_AGE")) * 1000 / 300 - 2; s2++)
      skip += seg_size(s2);
    (void)skip;
  }
  std::string err;
  long t = ms();
  std::thread faultT;
  if (faults)
    faultT = std::thread(
        [faults]
        {
          std::string spec = faults;
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
  bool ok = cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, err);
  INFO("ip open ok=%d in %ld ms err=%s", ok, ms() - t, err.c_str());
  if (!ok)
  {
    // Returning quietly here let "in-progress recording" report ok with its open hard-wired to fail.
    FAIL("the in-progress stream did not open: %s", err.c_str());
    if (faultT.joinable())
      faultT.join();
    return;
  }
  std::atomic<bool> stop{false}, finishedSet{false};
  std::atomic<long> reads{0}, neg{0}, zeros{0}, bytes{0}, bad{0}, maxRead{0};
  std::atomic<int64_t> highest{0};
  std::thread reader(
      [&]
      {
        std::vector<uint8_t> buf(262144);
        std::mt19937 rng(4);
        int64_t pos = 0;
        long lastSeek = ms();
        int consecutiveZero = 0;
        long pauseAt = getenv("PAUSE_AT") ? atol(getenv("PAUSE_AT")) : -1;
        long pauseMs = getenv("PAUSE_MS") ? atol(getenv("PAUSE_MS")) : 0;
        long startT = ms();
        bool noSeek = getenv("NOSEEK") != nullptr;
        while (!stop)
        {
          if (pauseAt >= 0 && ms() - startT >= pauseAt)
          {
            INFO("reader pausing %ld ms", pauseMs);
            sleep_ms(pauseMs);
            pauseAt = -1;
            INFO("reader resumes");
          }
          if (!chaos && !noSeek && ms() - lastSeek > 2000)
          {
            lastSeek = ms();
            int k = rng() % 3;
            int64_t np;
            if (k == 0)
              np = cl.SeekInProgressRecordingStream(0, SEEK_SET);
            else if (k == 1)
              np = cl.SeekInProgressRecordingStream(-(int64_t)(rng() % 500000), SEEK_CUR);
            else
              np = cl.SeekInProgressRecordingStream(0, SEEK_END);
            if (np >= 0)
              pos = np;
          }
          long r0 = ms();
          int n = cl.ReadInProgressRecordingStream(buf.data(), (unsigned)buf.size());
          long dt = ms() - r0;
          if (dt > maxRead)
            maxRead = dt;
          reads++;
          if (n < 0)
          {
            neg++;
            pos = cl.SeekInProgressRecordingStream(0, SEEK_CUR);
            if (!cl.IsInProgressRecordingStreamOpen())
              break;
            continue;
          }
          if (n == 0)
          {
            zeros++;
            if (finishedSet && ++consecutiveZero > 3)
              break;
            sleep_ms(10);
            continue;
          }
          consecutiveZero = 0;
          if (chaos)
          {
            pos = cl.SeekInProgressRecordingStream(0, SEEK_CUR) - n;
          }
          if (!getenv("IP_NOVERIFY"))
            for (int i = 0; i < n; i++)
              if (buf[i] != ip_byte(pos + i))
              {
                bad++;
                FAIL("ip byte mismatch at %lld (n=%d)", (long long)(pos + i), n);
                break;
              }
          pos += n;
          bytes += n;
          if (pos > highest)
            highest = pos;
        }
      });
  std::thread poller(
      [&]
      {
        while (!stop)
        {
          cl.GetInProgressRecordingStreamLength();
          cl.GetInProgressRecordingStreamDurationMs();
          cl.GetInProgressRecordingStreamStartTime();
          sleep_ms(getenv("POLL_MS") ? atoi(getenv("POLL_MS")) : 30);
        }
      });
  std::thread chaosT;
  if (chaos)
    chaosT = std::thread(
        [&]
        {
          std::mt19937 r(9);
          while (!stop)
          {
            cl.SeekInProgressRecordingStream(-(int64_t)(r() % 300000), SEEK_CUR);
            sleep_ms(r() % 50);
          }
        });
  long until = t + recordMs;
  while (ms() < until)
    sleep_ms(20);
  if (getenv("IP_EXPECT_RESUME"))
  {
    // For a scenario whose fault has cleared by now: playback must have continued, not ended for good (a transient
    // 500 used to end a recording read permanently; the other drivers only check bytes and miss an early end).
    const long b1 = (long)bytes;
    sleep_ms(1500);
    if ((long)bytes == b1)
      FAIL("the in-progress stream did not resume after the fault cleared (%ld bytes)", b1);
  }
  if (!getenv("IP_NOFINISH"))
  {
    ctl("ip_freeze=1");
    ctl("ip_state=completed");
    ctl("ip_endlist=1");
    finishedSet = true;
    INFO("recording finished server-side");
    long end = ms() + 15000;
    while (ms() < end && reader.joinable())
    {
      if (!cl.IsInProgressRecordingStreamOpen())
        break;
      sleep_ms(50);
      if (zeros > 0 && finishedSet)
      {
      }
    }
  }
  INFO("stats before close=%s", http("/__stats").c_str());
  long c0 = ms();
  stop = true;
  cl.CloseInProgressRecordingStream();
  INFO("ip close took %ld ms", ms() - c0);
  reader.join();
  poller.join();
  if (chaosT.joinable())
    chaosT.join();
  if (faultT.joinable())
    faultT.join();
  int64_t expectTotal = 0;
  std::string pl = http("/__stats");
  INFO("ip: reads=%ld bytes=%ld neg=%ld zeros=%ld bad=%ld highest=%lld maxRead=%ldms", (long)reads, (long)bytes,
       (long)neg, (long)zeros, (long)bad, (long long)highest, (long)maxRead);
  (void)expectTotal;
  if ((long)bytes == 0)
    FAIL("the in-progress stream delivered no bytes");
}

// A hung server with the cached token's freshness hint already lapsed: Close and a tail-wait read must still be
// bounded by the short per-request timeout, authentication step included (fifteenth hardening sweep: 30 s, not 5).
static void live_blackhole(DispatcharrClient& cl, bool tailRead)
{
  std::string err;
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  std::vector<uint8_t> buf(1 << 20);
  cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
  ctl("live_freeze=1");
  ctl("api_hang=1");
  cl.InvalidateAccessToken();
  const long shortMs = dispatcharr::ShortRequestTimeoutMs(configuredTimeout());
  const long limitMs = 4 * shortMs;
  if (tailRead)
  {
    long worst = 0;
    long t0 = ms();
    while (ms() - t0 < 40000)
    {
      long r0 = ms();
      int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
      worst = std::max(worst, ms() - r0);
      if (n < 0 || !cl.IsLiveTimeshiftStreamOpen())
        break;
    }
    INFO("blackhole tail read: worst single Read() %ld ms", worst);
    if (worst > limitMs)
      FAIL("a tail-wait read blocked %ld ms against a hung server", worst);
  }
  long c0 = ms();
  cl.CloseLiveTimeshiftStream();
  long took = ms() - c0;
  INFO("blackhole close took %ld ms", took);
  // The bound follows the connection timeout (RequestTimeout.h): about a sixth of it, so this is 5 s at the default of
  // 30 and 10 s at 60. A Close well under that means a fixed bound is back; well over means none applied.
  if (took > shortMs + 3000 || took < shortMs - 1500)
    FAIL("Close took %ld ms against a hung server with a lapsed token hint (the bound is %ld ms)", took, shortMs);
  ctl("api_hang=0");
  ctl("live_freeze=0");
}

// A cold open of a long in-progress backlog where one segment answers 500 to the open's probe and then recovers:
// the whole backlog must merge within a refresh or two, not one segment per refresh (fifteenth hardening sweep).
static void ip_cascade(DispatcharrClient& cl)
{
  ctl("ip_reset=120");
  ctl("ip_freeze=1");
  ctl("ip_head_seq=200");
  ctl("ip_head_status=500");
  std::string err;
  long t = ms();
  bool ok = cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, err);
  INFO("cascade open ok=%d in %ld ms err=%s", ok, ms() - t, err.c_str());
  ctl("ip_head_status=0");
  ctl("ip_head_seq=");
  if (!ok)
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  int64_t expect = 0;
  for (int s2 = 0; s2 < 400; s2++)
    expect += seg_size(s2);
  long t1 = ms();
  int64_t len = 0;
  while (ms() - t1 < 10000)
  {
    len = cl.GetInProgressRecordingStreamLength();
    if (len >= expect)
      break;
    sleep_ms(100);
  }
  INFO("cascade: %lld of %lld bytes after %ld ms", (long long)len, (long long)expect, ms() - t1);
  if (len < expect)
    FAIL("only %lld of %lld bytes merged after %ld ms", (long long)len, (long long)expect, ms() - t1);
  cl.CloseInProgressRecordingStream();
}

// An API that answers every request slowly (above the 5 s bound steady-state refreshes get) must still be able to
// start a stream: Open's cold-start wait uses the configured timeout (found by the live check of the fifteenth
// hardening sweep: with 6 s of latency every cold-start refresh timed out and Open failed after about 165 s).
static void live_slow_open(DispatcharrClient& cl)
{
  ctl("api_delay_ms=6000");
  std::string err;
  const long t0 = ms();
  const bool ok = cl.OpenLiveTimeshiftStream("uuid", err);
  INFO("slow open ok=%d in %ld ms err=%s", ok, ms() - t0, err.c_str());
  if (!ok)
    FAIL("a stream could not be opened against an API with 6 s of latency: %s", err.c_str());
  ctl("api_delay_ms=0");
  if (ok)
    cl.CloseLiveTimeshiftStream();
}

// A proxy that starts dropping Range mid-playback and sends the whole file slowly: the read at a non-zero offset
// must end the stream at once, not after downloading (and discarding) the file or waiting out the timeout (the
// fifteenth sweep's follow-up to the open-time Range probe fix).
static void rec_range_dropped(DispatcharrClient& cl)
{
  std::string err;
  if (!cl.OpenRecordingStream(5, err))
  {
    FAIL("rec open: %s", err.c_str());
    return;
  }
  std::vector<uint8_t> buf(65536);
  int n = cl.ReadRecordingStream(buf.data(), (unsigned)buf.size());
  if (n <= 0)
    FAIL("first read returned %d", n);
  ctl("rec_norange=1");
  ctl("rec_slow_ms=250");
  const long t0 = ms();
  n = cl.ReadRecordingStream(buf.data(), (unsigned)buf.size());
  const long took = ms() - t0;
  INFO("read at a non-zero offset against a Range-dropping server: ret=%d in %ld ms", n, took);
  if (n != 0)
    FAIL("expected the stream to end (0), got %d", n);
  if (took > 3000)
    FAIL("the read took %ld ms: it kept receiving a body it was going to throw away", took);
  ctl("rec_norange=0");
  cl.CloseRecordingStream();
}

// Stop while a reader waits at the tail of an in-progress recording and the server stops answering altogether: Kodi
// closes a stream only once its read thread has left Read(), so the longest a single Read() can block after that is
// how long a Stop takes. Every request of the read's refresh used to take the full timeout (140 s at the 30 s
// default, found live; the fifteenth sweep's follow-up).
static void ip_blackhole_close(DispatcharrClient& cl, long limitMs)
{
  ctl("ip_reset=3");
  std::string err;
  if (!cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("ip_freeze=1"); // nothing new is ever listed: the reader ends up waiting at the tail
  std::atomic<bool> stop{false}, hung{false};
  std::atomic<long> worstAfterHang{0};
  std::thread reader(
      [&]
      {
        std::vector<uint8_t> buf(1 << 18);
        while (!stop)
        {
          const long r0 = ms();
          const bool during = hung;
          int n = cl.ReadInProgressRecordingStream(buf.data(), (unsigned)buf.size());
          const long took = ms() - r0;
          if ((during || hung) && took > worstAfterHang)
            worstAfterHang = took;
          if (n < 0)
            sleep_ms(20);
        }
      });
  sleep_ms(3000);
  ctl("api_hang=1");
  hung = true;
  sleep_ms(45000);
  stop = true;
  const long c0 = ms();
  cl.CloseInProgressRecordingStream();
  reader.join();
  INFO("ip blackhole: worst single Read() after the hang %ld ms; Close and the reader's exit took %ld ms",
       (long)worstAfterHang, ms() - c0);
  if (worstAfterHang > limitMs)
    FAIL("a Read() blocked %ld ms against a hung server (limit %ld): that is how long Stop waits", (long)worstAfterHang,
         limitMs);
  ctl("api_hang=0");
  ctl("ip_freeze=0");
}

// A recording row whose file does not exist: opening it must fail and tell the viewer why (the stub prints each
// notification as a "[notify]" line).
static void rec_open_missing(DispatcharrClient& cl)
{
  ctl("rec_status=404");
  std::string err;
  const bool ok = cl.OpenRecordingStream(5, err);
  INFO("open of a recording with no file: ok=%d err=%s", ok, err.c_str());
  if (ok)
    FAIL("the open should have failed");
  ctl("rec_status=0");
}

int main(int argc, char** argv)
{
  InitKodiStub();
  curl_global_init(CURL_GLOBAL_ALL);
  g_port = atoi(argv[1]);
  std::string what = argv[2];
  const char* arg = argc > 3 ? argv[3] : nullptr;
  {
    auto* cl = new DispatcharrClient(cfg());
    if (what == "live")
      live_play(*cl, arg ? atoi(arg) : 8000, false);
    else if (what == "live_chaos")
      live_play(*cl, 8000, true);
    else if (what == "live_fault")
      live_play(*cl, atoi(argv[3]), false, argv[4]);
    else if (what == "live_tail_close")
      live_tail_close(*cl);
    else if (what == "live_tail_bound")
      live_tail_bound(*cl);
    else if (what == "live_blackhole_close")
      live_blackhole(*cl, false);
    else if (what == "live_blackhole_read")
      live_blackhole(*cl, true);
    else if (what == "rec_open_missing")
      rec_open_missing(*cl);
    else if (what == "rec_range_dropped")
      rec_range_dropped(*cl);
    else if (what == "ip_blackhole_close")
      ip_blackhole_close(*cl, arg ? atol(arg) : 4 * dispatcharr::ShortRequestTimeoutMs(configuredTimeout()));
    else if (what == "live_slow_open")
      live_slow_open(*cl);
    else if (what == "ip_cascade")
      ip_cascade(*cl);
    else if (what == "live_reopen")
    {
      for (int i = 0; i < 5; i++)
        live_play(*cl, 1500, false);
    }
    else if (what == "rec")
      rec_play(*cl, arg);
    else if (what == "ip")
      ip_play(*cl, arg ? atoi(arg) : 6000, argc > 4 ? argv[4] : nullptr, false);
    else if (what == "live_fault")
    {
    }
    else if (what == "ip_chaos")
      ip_play(*cl, 6000, nullptr, true);
    else if (what == "dtor_inflight")
    {
      // a stream left open at destruction: does anything leak or fault?
      std::string err;
      cl->OpenLiveTimeshiftStream("uuid", err);
      std::vector<uint8_t> b(65536);
      cl->ReadLiveTimeshiftStream(b.data(), 65536);
      cl->OpenRecordingStream(5, err);
      cl->ReadRecordingStream(b.data(), 1000);
      cl->OpenInProgressRecordingStream(7, time(nullptr), err);
      cl->ReadInProgressRecordingStream(b.data(), 1000);
      cl->AbortInFlightRequests();
    }
    else
      FAIL("unknown scenario %s", what.c_str());
    long d0 = ms();
    delete cl;
    INFO("delete took %ld ms", ms() - d0);
  }
  printf("stats=%s\n", http("/__stats").c_str());
  printf("RESULT fails=%d\n", (int)g_fail);
  curl_global_cleanup();
  return g_fail ? 1 : 0;
}
