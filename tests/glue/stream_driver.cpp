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
// Every scenario that closes its recorded streams ends with none registered: a leak here is a handle and a cached
// segment kept for the life of the client.
static void expect_all_closed(DispatcharrClient& cl)
{
  if (cl.OpenRecordedStreamCount() != 0)
    FAIL("%zu recorded stream(s) still registered after every Close", cl.OpenRecordedStreamCount());
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

// Completed recording: byte o is pack('>II', tag, o/8)[o%8]. Each fake recording has its own tag (REC_TAGS in
// fake_dispatcharr.py), so a stream served another recording's bytes shows up as a mismatch.
static const int64_t REC_SIZE = 3 * 1024 * 1024 + 5;
static uint32_t rec_tag(int id)
{
  return id == 6 ? 0xDEADBE00u : 0xC0FFEE00u;
}
static uint8_t rec_byte(int64_t o, uint32_t a = 0xC0FFEE00u)
{
  uint8_t r[8];
  uint32_t k = (uint32_t)(o / 8);
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
// In-progress: concatenation of seg_bytes(tag, s, seg_size(s)) from s=0 (IP_TAGS in fake_dispatcharr.py)
// Built once, on first use: several reader threads call ip_byte() at the same time.
static const std::vector<int64_t>& ip_prefix()
{
  static const std::vector<int64_t> prefix = []
  {
    std::vector<int64_t> p{0};
    for (int s = 0; s < 20000; s++)
      p.push_back(p.back() + seg_size(s));
    return p;
  }();
  return prefix;
}
static uint32_t ip_tag(int id)
{
  return id == 8 ? 0x2C000000u : 0x1B000000u;
}
static uint8_t ip_byte(int64_t o, uint32_t tag = 0x1B000000u)
{
  const std::vector<int64_t>& ipPrefix = ip_prefix();
  int64_t s = std::upper_bound(ipPrefix.begin(), ipPrefix.end(), o) - ipPrefix.begin() - 1;
  int64_t off = o - ipPrefix[s];
  uint32_t a = tag | (uint32_t)(s & 0xFFFF), k = (uint32_t)(off / 8);
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
// which Kodi's demuxer retries (a 0 would end playback), and 0 comes only once the whole budget has been used. The
// budget on a plugin that keeps answering is about 70 s (kLiveStallWaitBudgetMs: Dispatcharr switches a stalled channel
// to its next stream at about 58 s), so this freezes the live buffer for 20 s, times each read at the tail and requires
// no 0, then lets it grow again and requires the stream to resume. The end of the budget is live_tail_giveup (by
// hand, it runs 70 s) and the unit tests; an ffmpeg that exits ends the wait at once, live_tail_ended.
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
  if (sawZero)
    FAIL(
        "the stream gave up (0) after %ld ms at the tail; a quiet buffer on an answering plugin is waited on for about "
        "a minute (a silent stall that Dispatcharr heals by switching streams)",
        waitBeforeFirstZero);
  if (maxTailReadMs > 1800)
    FAIL("one read at the live tail blocked for %ld ms; Kodi cannot act on a close for that long", maxTailReadMs);
  if (minusOnes < 10)
    FAIL("only %ld -1 returns in 20 s at the tail, so the wait was not handed back in slices", minusOnes);
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

// The ffmpeg behind a stalled buffer exits (Dispatcharr gave up on the channel, or the buffer died): the plugin's
// manifest then says `ended`, and the wait must stop at once instead of running on to the stall budget (about 70 s).
// The buffer is frozen so the reader parks at the tail, `live_ended` is set after a few seconds, and the next read at
// the tail must return 0 within a couple of slices.
static void live_tail_ended(DispatcharrClient& cl)
{
  std::string err;
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("live_freeze=1");
  std::vector<uint8_t> buf(1 << 20);
  long tailStart = -1, endedSetAt = -1, zeroAt = -1;
  const long phaseStart = ms();
  while (ms() - phaseStart < 30000 && zeroAt < 0)
  {
    const int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
    if (n > 0)
      continue;
    if (tailStart < 0)
      tailStart = ms();
    if (n == 0)
      zeroAt = ms();
    else if (endedSetAt < 0 && ms() - tailStart >= 3000)
    {
      ctl("live_ended=1");
      endedSetAt = ms();
    }
  }
  ctl("live_ended=0");
  ctl("live_freeze=0");
  INFO("tail ended: tail reached at +%ld ms, ended set at +%ld ms, first 0 at +%ld ms", tailStart - phaseStart,
       endedSetAt - phaseStart, zeroAt - phaseStart);
  if (tailStart < 0)
    FAIL("the reader never reached the live tail");
  else if (zeroAt < 0)
    FAIL("the wait did not end within 30 s of an ended buffer");
  else if (endedSetAt < 0)
    FAIL("the stream returned 0 (after %ld ms at the tail) before the buffer was marked ended", zeroAt - tailStart);
  else if (zeroAt - endedSetAt > 4000)
    FAIL("the wait ran %ld ms past the buffer being marked ended; it must stop at once", zeroAt - endedSetAt);
}

// The end of the stall budget itself, by hand because it runs about 70 s: a quiet buffer on an answering plugin is
// waited on, returning -1 slices, until about kLiveStallWaitBudgetMs of waiting has gone by, then the read returns 0.
static void live_tail_giveup(DispatcharrClient& cl)
{
  std::string err;
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("live_freeze=1");
  std::vector<uint8_t> buf(1 << 20);
  long tailStart = -1, zeroAt = -1;
  const long phaseStart = ms();
  while (ms() - phaseStart < 120000 && zeroAt < 0)
  {
    const int n = cl.ReadLiveTimeshiftStream(buf.data(), (unsigned)buf.size());
    if (n > 0)
      continue;
    if (tailStart < 0)
      tailStart = ms();
    if (n == 0)
      zeroAt = ms();
  }
  ctl("live_freeze=0");
  const long waited = zeroAt - tailStart;
  INFO("tail give-up: waited %ld ms at the tail before the first 0", waited);
  if (tailStart < 0 || zeroAt < 0)
    FAIL("no 0 within 120 s: the stall budget never ended");
  else if (waited < 60000 || waited > 90000)
    FAIL("gave up after %ld ms at the tail; the stall budget is about 70000 ms", waited);
}

// ---------------------------------------------------------------- completed recording
static void rec_play(DispatcharrClient& cl, const char* faults)
{
  std::string err;
  int64_t sid = 0;
  if (!cl.OpenRecordingStream(5, sid, err))
  {
    FAIL("rec open: %s", err.c_str());
    return;
  }
  int64_t len = cl.GetRecordedStreamLength(sid);
  if (len != REC_SIZE)
    FAIL("rec length %lld", (long long)len);
  std::atomic<bool> stop{false};
  std::thread poller(
      [&]
      {
        while (!stop)
        {
          cl.GetRecordedStreamLength(sid);
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
      int64_t np = cl.SeekRecordedStream(sid, t, SEEK_SET);
      if (np != t)
        FAIL("seek to %lld got %lld", (long long)t, (long long)np);
      pos = np;
    }
    if (iter % 37 == 36)
    {
      int64_t np = cl.SeekRecordedStream(sid, -7, SEEK_END);
      pos = np;
    }
    unsigned want = 1 + rng() % buf.size();
    long r0 = ms();
    int n = cl.ReadRecordedStream(sid, buf.data(), want);
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
      pos = cl.SeekRecordedStream(sid, 0, SEEK_SET);
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
  cl.CloseRecordedStream(sid);
  expect_all_closed(cl);
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
  int64_t sid = 0;
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
  bool ok = cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, sid, err);
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
              np = cl.SeekRecordedStream(sid, 0, SEEK_SET);
            else if (k == 1)
              np = cl.SeekRecordedStream(sid, -(int64_t)(rng() % 500000), SEEK_CUR);
            else
              np = cl.SeekRecordedStream(sid, 0, SEEK_END);
            if (np >= 0)
              pos = np;
          }
          long r0 = ms();
          int n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
          long dt = ms() - r0;
          if (dt > maxRead)
            maxRead = dt;
          reads++;
          if (n < 0)
          {
            neg++;
            pos = cl.SeekRecordedStream(sid, 0, SEEK_CUR);
            if (!(cl.OpenRecordedStreamCount() > 0))
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
            pos = cl.SeekRecordedStream(sid, 0, SEEK_CUR) - n;
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
          cl.GetRecordedStreamLength(sid);
          DispatcharrClient::RecordedStreamTimes times;
          cl.GetRecordedStreamTimes(sid, times);
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
            cl.SeekRecordedStream(sid, -(int64_t)(r() % 300000), SEEK_CUR);
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
      if (!(cl.OpenRecordedStreamCount() > 0))
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
  cl.CloseRecordedStream(sid);
  expect_all_closed(cl);
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
  int64_t sid = 0;
  long t = ms();
  bool ok = cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, sid, err);
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
    len = cl.GetRecordedStreamLength(sid);
    if (len >= expect)
      break;
    sleep_ms(100);
  }
  INFO("cascade: %lld of %lld bytes after %ld ms", (long long)len, (long long)expect, ms() - t1);
  if (len < expect)
    FAIL("only %lld of %lld bytes merged after %ld ms", (long long)len, (long long)expect, ms() - t1);
  cl.CloseRecordedStream(sid);
  expect_all_closed(cl);
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

// A server that answers the start request and then goes silent: the live cold-start loop used to try thirty times, each
// as long as the request timeout, which held an open for about fifteen minutes at the default 30 s. The loop now ends
// once its own healthy wait plus one short bound has passed (ColdStartWallClockBudgetMs()): with the harness's 5 s
// timeout, 20 s plus the try in flight; at the default 30 s timeout the first timed-out try is the last. The
// in-progress variant (by hand, about a minute) is a recording whose playlist does not exist yet, which retries ninety
// times, against an API that takes 3 s to say so: 315 s before, 50 s plus a try now. (A playlist request that never
// answers is not retried there: any failure but a 404 ends that open at once.)
static void silent_open(DispatcharrClient& cl, bool inProgress)
{
  const long timeoutMs = configuredTimeout() * 1000L;
  const long budgetMs = inProgress ? dispatcharr::ColdStartWallClockBudgetMs(90, 500, configuredTimeout())
                                   : dispatcharr::ColdStartWallClockBudgetMs(30, 500, configuredTimeout());
  std::string err;
  int64_t sid = 0;
  if (inProgress)
  {
    ctl("ip_reset=3");
    ctl("ip_nodir=1");
    ctl("api_delay_ms=3000");
  }
  else
    ctl("manifest_hang=1");
  const long t0 = ms();
  const bool ok = inProgress ? cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, sid, err)
                             : cl.OpenLiveTimeshiftStream("uuid", err);
  const long took = ms() - t0;
  INFO("silent %s open: ok=%d in %ld ms (budget %ld ms, timeout %ld ms) err=%s", inProgress ? "in-progress" : "live",
       ok, took, budgetMs, timeoutMs, err.c_str());
  ctl("ip_nodir=0");
  ctl("api_delay_ms=0");
  ctl("manifest_hang=0");
  if (ok)
    FAIL("the open succeeded against a server that never answered the playlist");
  if (took < budgetMs)
    FAIL("the open gave up after %ld ms, before its %ld ms budget", took, budgetMs);
  if (took > budgetMs + 2 * timeoutMs)
    FAIL("the open took %ld ms against a silent server (budget %ld ms): the loop is not bounded by it", took, budgetMs);
  // At the default 30 s timeout a try that already used the whole timeout is the last one, so the open ends right after
  // it. Kodi holds its UI for the open's length when a viewer presses Stop, which is why a second timeout-long try is
  // not worth waiting for. Stated against a fixed 20 s, not the budget function's own value, so that a budget that grew
  // back cannot excuse itself.
  if (!inProgress && timeoutMs > 20000 && took > timeoutMs + 3000)
    FAIL("the open took %ld ms against a silent server: a second try started after the first used the whole %ld ms "
         "timeout (budget %ld ms)",
         took, timeoutMs, budgetMs);
}

// A channel whose provider is slow to deliver its first segment: the plugin keeps answering "live playlist not found"
// (ffmpeg alive, no `fatal`) for 25 s, longer than the 15 s the cold-start loop used to wait. The open must wait it out
// (ColdStartTryLimit()) instead of failing with a playback error and succeeding on the viewer's second try.
static void live_late_start(DispatcharrClient& cl)
{
  ctl("manifest_not_ready_ms=25000");
  std::string err;
  const long t0 = ms();
  const bool ok = cl.OpenLiveTimeshiftStream("uuid", err);
  const long took = ms() - t0;
  INFO("late start ok=%d in %ld ms err=%s", ok, took, err.c_str());
  ctl("manifest_not_ready_ms=0");
  if (!ok)
    FAIL("a channel that took 25 s to produce its first segment could not be opened: %s", err.c_str());
  else
  {
    if (took < 24000)
      FAIL("the open returned after %ld ms, before the fake's first segment could exist", took);
    cl.CloseLiveTimeshiftStream();
  }
}

// The buffer starts and its manifest comes back through the API port, but the plugin's file server (a different port
// the container has to publish) cannot be reached: the open used to succeed and every segment read then failed until
// Kodi gave the stream up, a spinner for about 30 s and a channel that exited with nothing saying why. The open now
// probes the file server and fails at once, naming the port (the stub prints the notification as a "[notify]" line).
static void live_fileserver_unreachable(DispatcharrClient& cl)
{
  ctl("fs_port=1"); // nothing listens there: the connection is refused
  std::string err;
  const long t0 = ms();
  const bool ok = cl.OpenLiveTimeshiftStream("uuid", err);
  const long took = ms() - t0;
  INFO("open with the file server's port unreachable: ok=%d in %ld ms err=%s", ok, took, err.c_str());
  if (ok)
    FAIL("the open succeeded although the file server cannot be reached");
  if (err.find("port 1") == std::string::npos)
    FAIL("the error does not name the port: %s", err.c_str());
  if (took > 15000)
    FAIL("the open took %ld ms to notice an unreachable file server", took);
  if (cl.IsLiveTimeshiftStreamOpen())
    FAIL("the stream was left open after the failed open");
  // And the same server, reachable again: the open must work (the probe is not sticky).
  ctl("fs_port=0");
  err.clear();
  if (!cl.OpenLiveTimeshiftStream("uuid", err))
    FAIL("the open failed once the file server was reachable: %s", err.c_str());
  else
    cl.CloseLiveTimeshiftStream();
}

// A proxy that starts dropping Range mid-playback and sends the whole file slowly: the read at a non-zero offset
// must end the stream at once, not after downloading (and discarding) the file or waiting out the timeout (the
// fifteenth sweep's follow-up to the open-time Range probe fix).
static void rec_range_dropped(DispatcharrClient& cl)
{
  std::string err;
  int64_t sid = 0;
  if (!cl.OpenRecordingStream(5, sid, err))
  {
    FAIL("rec open: %s", err.c_str());
    return;
  }
  std::vector<uint8_t> buf(65536);
  int n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
  if (n <= 0)
    FAIL("first read returned %d", n);
  ctl("rec_norange=1");
  ctl("rec_slow_ms=250");
  const long t0 = ms();
  n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
  const long took = ms() - t0;
  INFO("read at a non-zero offset against a Range-dropping server: ret=%d in %ld ms", n, took);
  if (n != 0)
    FAIL("expected the stream to end (0), got %d", n);
  if (took > 3000)
    FAIL("the read took %ld ms: it kept receiving a body it was going to throw away", took);
  ctl("rec_norange=0");
  cl.CloseRecordedStream(sid);
  expect_all_closed(cl);
}

// Stop while a reader waits at the tail of an in-progress recording and the server stops answering altogether: Kodi
// closes a stream only once its read thread has left Read(), so the longest a single Read() can block after that is
// how long a Stop takes. Every request of the read's refresh used to take the full timeout (140 s at the 30 s
// default, found live; the fifteenth sweep's follow-up).
static void ip_blackhole_close(DispatcharrClient& cl, long limitMs)
{
  ctl("ip_reset=3");
  std::string err;
  int64_t sid = 0;
  if (!cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, sid, err))
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
          int n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
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
  cl.CloseRecordedStream(sid);
  expect_all_closed(cl);
  reader.join();
  INFO("ip blackhole: worst single Read() after the hang %ld ms; Close and the reader's exit took %ld ms",
       (long)worstAfterHang, ms() - c0);
  if (worstAfterHang > limitMs)
    FAIL("a Read() blocked %ld ms against a hung server (limit %ld): that is how long Stop waits", (long)worstAfterHang,
         limitMs);
  ctl("api_hang=0");
  ctl("ip_freeze=0");
}

// The in-progress recording's tail wait in slices, as live_tail_bound does for the live buffer: a reader parked at the
// tail of a recording that is still being written (a stalled recorder, a viewer who caught up) has to hand control back
// to Kodi about once a second, because Kodi acts on a stop only between reads. A slice that finds nothing returns -1,
// 0 comes only once the whole budget is used, and the same stream resumes when the recording grows again.
static void ip_tail_bound(DispatcharrClient& cl)
{
  ctl("ip_reset=3");
  std::string err;
  int64_t sid = 0;
  if (!cl.OpenInProgressRecordingStream(7, time(nullptr) - 600, sid, err))
  {
    FAIL("open: %s", err.c_str());
    return;
  }
  ctl("ip_freeze=1");
  std::vector<uint8_t> buf(1 << 18);
  long maxTailReadMs = 0, tailWaitMs = 0, waitBeforeFirstZero = -1, minusOnes = 0, bytes = 0;
  bool sawZero = false;
  const long phaseStart = ms();
  while (ms() - phaseStart < 30000 && !sawZero)
  {
    const long t0 = ms();
    const int n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
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
  INFO("ip tail bound: max read at the tail %ld ms, %ld retries (-1) before the first 0, waited %ld ms in all, "
       "bytes before the tail %ld",
       maxTailReadMs, minusOnes, waitBeforeFirstZero, bytes);
  if (!sawZero)
    FAIL("no read ever returned 0 in 30 s: the budget never ended");
  if (maxTailReadMs > 1800)
    FAIL("one read at the tail blocked for %ld ms; Kodi cannot act on a stop for that long", maxTailReadMs);
  if (waitBeforeFirstZero >= 0 && waitBeforeFirstZero < 2000)
    FAIL("the stream gave up (0) after only %ld ms at the tail; the budget must not shrink", waitBeforeFirstZero);
  if (minusOnes < 1)
    FAIL("no -1 was returned while waiting, so the wait was not handed back in slices");
  if (!(cl.OpenRecordedStreamCount() > 0))
    FAIL("the stream closed itself while waiting at the tail");

  ctl("ip_freeze=0");
  long resumed = 0;
  const long phase2 = ms();
  while (ms() - phase2 < 20000 && resumed == 0)
  {
    const int n = cl.ReadRecordedStream(sid, buf.data(), (unsigned)buf.size());
    if (n > 0)
      resumed += n;
  }
  INFO("ip tail bound: after the recording grew again %ld bytes arrived within %ld ms", resumed, ms() - phase2);
  if (resumed == 0)
    FAIL("the stream did not resume once new segments arrived");
}

// A recording row whose file does not exist: opening it must fail and tell the viewer why (the stub prints each
// notification as a "[notify]" line).
static void rec_open_missing(DispatcharrClient& cl)
{
  ctl("rec_status=404");
  std::string err;
  int64_t sid = 0;
  const bool ok = cl.OpenRecordingStream(5, sid, err);
  INFO("open of a recording with no file: ok=%d err=%s", ok, err.c_str());
  if (ok)
    FAIL("the open should have failed");
  ctl("rec_status=0");
}

// ---------------------------------------------------------------- several recorded streams at once
// Kodi 22 keeps the previous recorded stream open until it closes it itself, so a second open arrives while the first
// is playing and a Close for the old id can arrive after the next stream's open.
struct OpenStream
{
  int recId = 0;
  bool inProgress = false;
  int64_t sid = 0;
  int64_t pos = 0;
  uint8_t byteAt(int64_t o) const
  {
    return inProgress ? ip_byte(o, ip_tag(recId)) : rec_byte(o, rec_tag(recId));
  }
};

static bool open_stream(DispatcharrClient& cl, int recId, OpenStream& st)
{
  std::string err;
  st.recId = recId;
  st.inProgress = recId >= 7;
  st.pos = 0;
  const bool ok = st.inProgress ? cl.OpenInProgressRecordingStream(recId, time(nullptr) - 600, st.sid, err)
                                : cl.OpenRecordingStream(recId, st.sid, err);
  if (!ok)
    FAIL("open of recording %d: %s", recId, err.c_str());
  return ok;
}

// Reads `want` bytes from the stream's own position and checks each against its recording's pattern. A -1 is
// retried (a wait at the tail of a growing recording is handed back in slices), a 0 before `want` is a failure.
static int64_t read_verified(DispatcharrClient& cl, OpenStream& st, int64_t want)
{
  std::vector<uint8_t> buf(32768 + 3);
  int64_t got = 0;
  int retries = 0;
  while (got < want)
  {
    const unsigned ask = (unsigned)std::min<int64_t>((int64_t)buf.size(), want - got);
    const int n = cl.ReadRecordedStream(st.sid, buf.data(), ask);
    if (n < 0)
    {
      if (++retries > 400)
      {
        FAIL("recording %d (stream %lld): reads kept failing at %lld", st.recId, (long long)st.sid, (long long)st.pos);
        return got;
      }
      sleep_ms(20);
      continue;
    }
    if (n == 0)
    {
      FAIL("recording %d (stream %lld): unexpected end at %lld after %lld of %lld bytes", st.recId, (long long)st.sid,
           (long long)st.pos, (long long)got, (long long)want);
      return got;
    }
    for (int i = 0; i < n; i++)
      if (buf[i] != st.byteAt(st.pos + i))
      {
        FAIL("recording %d (stream %lld): byte mismatch at %lld (another stream's data, or a corrupted read)", st.recId,
             (long long)st.sid, (long long)(st.pos + i));
        return got;
      }
    st.pos += n;
    got += n;
  }
  return got;
}

// Opens the streams in order without closing any, reads them side by side, then closes them: `closeFirst` first.
static void multi_play(DispatcharrClient& cl, const std::vector<int>& recIds, size_t closeFirst)
{
  ctl("ip_reset=20");
  std::vector<OpenStream> streams(recIds.size());
  for (size_t i = 0; i < recIds.size(); i++)
    if (!open_stream(cl, recIds[i], streams[i]))
      return;
  for (size_t i = 0; i < streams.size(); i++)
    for (size_t j = i + 1; j < streams.size(); j++)
      if (streams[i].sid == streams[j].sid)
        FAIL("two open streams were given the same id %lld", (long long)streams[i].sid);
  if (cl.OpenRecordedStreamCount() != streams.size())
    FAIL("%zu streams open, the client counts %zu", streams.size(), cl.OpenRecordedStreamCount());

  for (auto& st : streams)
  {
    bool realTime = false;
    if (!cl.IsRecordedStreamRealTime(st.sid, realTime))
      FAIL("stream %lld is open but IsRecordedStreamRealTime does not know it", (long long)st.sid);
    else if (realTime != st.inProgress)
      FAIL("recording %d: real-time is %d, expected %d", st.recId, (int)realTime, (int)st.inProgress);
    DispatcharrClient::RecordedStreamTimes times;
    const auto lookup = cl.GetRecordedStreamTimes(st.sid, times);
    const auto want = st.inProgress ? DispatcharrClient::RecordedStreamLookup::kOk
                                    : DispatcharrClient::RecordedStreamLookup::kNoTimes;
    if (lookup != want)
      FAIL("recording %d: times lookup %d, expected %d", st.recId, (int)lookup, (int)want);
  }

  // Phase 1: all streams read at once, each from its own thread, with a poller on every stream's times and length as
  // Kodi does (it also keeps a paused stream's server-side key alive).
  std::atomic<bool> stop{false};
  std::thread poller(
      [&]
      {
        while (!stop)
        {
          for (auto& st : streams)
          {
            DispatcharrClient::RecordedStreamTimes times;
            cl.GetRecordedStreamTimes(st.sid, times);
            cl.GetRecordedStreamLength(st.sid);
          }
          sleep_ms(30);
        }
      });
  std::vector<std::thread> readers;
  for (auto& st : streams)
    readers.emplace_back([&cl, &st] { read_verified(cl, st, 200000); });
  for (auto& t : readers)
    t.join();

  // Phase 2: one stream moves while the other is left where it was, then it is the other's turn. Positions are per
  // stream: a seek or read on one must not move the other.
  OpenStream& a = streams[0];
  OpenStream& b = streams[streams.size() - 1];
  const int64_t bPosBefore = b.pos;
  const int64_t aSeek = cl.SeekRecordedStream(a.sid, 0, SEEK_SET);
  if (aSeek != 0)
    FAIL("seek on stream %lld returned %lld", (long long)a.sid, (long long)aSeek);
  a.pos = 0;
  read_verified(cl, a, 100000);
  if (&a != &b)
  {
    if (cl.SeekRecordedStream(b.sid, 0, SEEK_CUR) != bPosBefore)
      FAIL("a seek and reads on stream %lld moved stream %lld", (long long)a.sid, (long long)b.sid);
    read_verified(cl, b, 100000);
  }
  stop = true;
  poller.join();

  // Phase 3: Close in the chosen order. The other stream keeps working, and the closed id answers like an unknown one.
  OpenStream& first = streams[closeFirst % streams.size()];
  cl.CloseRecordedStream(first.sid);
  if (cl.OpenRecordedStreamCount() != streams.size() - 1)
    FAIL("%zu streams registered after one Close, expected %zu", cl.OpenRecordedStreamCount(), streams.size() - 1);
  std::vector<uint8_t> buf(1000);
  if (cl.ReadRecordedStream(first.sid, buf.data(), 1000) != 0)
    FAIL("a read on a closed id returned data or an error (Kodi retries -1 at once)");
  if (cl.SeekRecordedStream(first.sid, 0, SEEK_SET) != -1 || cl.GetRecordedStreamLength(first.sid) != -1)
    FAIL("seek or length on a closed id did not return -1");
  cl.CloseRecordedStream(first.sid); // a second Close of the same id is harmless
  bool unused = false;
  if (cl.IsRecordedStreamRealTime(first.sid, unused))
    FAIL("a closed id is still known");
  for (auto& st : streams)
  {
    if (&st == &first)
      continue;
    read_verified(cl, st, 50000);
    cl.CloseRecordedStream(st.sid);
  }
  expect_all_closed(cl);
  INFO("multi: %zu streams played and closed", streams.size());
}

// A stream already playing must not wait for another stream's open: the second open's cold start (here a slow first
// playlist) holds no lock the first stream's reads need.
static void ip_open_while_playing(DispatcharrClient& cl)
{
  ctl("ip_reset=20");
  OpenStream a;
  if (!open_stream(cl, 7, a))
    return;
  std::atomic<bool> stop{false};
  std::atomic<long> longestReadMs{0};
  std::atomic<int64_t> bytes{0};
  std::thread reader(
      [&]
      {
        std::vector<uint8_t> buf(16384);
        while (!stop)
        {
          const long t0 = ms();
          const int n = cl.ReadRecordedStream(a.sid, buf.data(), (unsigned)buf.size());
          const long dt = ms() - t0;
          if (dt > longestReadMs)
            longestReadMs = dt;
          if (n > 0)
          {
            for (int i = 0; i < n; i++)
              if (buf[i] != a.byteAt(a.pos + i))
              {
                FAIL("playing stream: byte mismatch at %lld while another stream opened", (long long)(a.pos + i));
                return;
              }
            a.pos += n;
            bytes += n;
          }
          else
            sleep_ms(20);
        }
      });
  sleep_ms(300);
  ctl("ip_manifest_delay_ms_8=2500");
  OpenStream b;
  const long t0 = ms();
  const bool ok = open_stream(cl, 8, b);
  const long openMs = ms() - t0;
  ctl("ip_manifest_delay_ms_8=0");
  sleep_ms(300);
  stop = true;
  reader.join();
  INFO("second open took %ld ms; the playing stream's longest read was %ld ms, %lld bytes read", openMs,
       longestReadMs.load(), (long long)bytes.load());
  if (openMs < 2000)
    FAIL("the slow open finished in %ld ms; the delay on its playlist never applied, so this proves nothing", openMs);
  if (longestReadMs > 1500)
    FAIL("a read on the playing stream blocked for %ld ms while another stream opened", longestReadMs.load());
  if (bytes < 100000)
    FAIL("the playing stream read only %lld bytes while another stream opened", (long long)bytes.load());
  if (ok)
  {
    read_verified(cl, b, 50000);
    cl.CloseRecordedStream(b.sid);
  }
  cl.CloseRecordedStream(a.sid);
  expect_all_closed(cl);
}

// Close on streams that readers are in the middle of, from another thread, in an arbitrary order.
static void multi_close_race(DispatcharrClient& cl)
{
  ctl("ip_reset=20");
  for (int round = 0; round < 4; round++)
  {
    std::vector<OpenStream> streams(4);
    const int ids[4] = {5, 5, 7, 7};
    for (int i = 0; i < 4; i++)
      if (!open_stream(cl, ids[i], streams[i]))
        return;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (auto& st : streams)
      readers.emplace_back(
          [&cl, &st, &stop]
          {
            std::vector<uint8_t> buf(8192);
            while (!stop)
            {
              const int n = cl.ReadRecordedStream(st.sid, buf.data(), (unsigned)buf.size());
              if (n < 0)
              {
                sleep_ms(10);
                continue;
              }
              if (n == 0)
              {
                // End of a completed recording, or the id was closed under this reader: a seek tells which.
                const int64_t np = cl.SeekRecordedStream(st.sid, 0, SEEK_SET);
                if (np < 0)
                  return;
                st.pos = np;
                continue;
              }
              for (int i = 0; i < n; i++)
                if (buf[i] != st.byteAt(st.pos + i))
                {
                  FAIL("recording %d: byte mismatch at %lld during the close race", st.recId, (long long)(st.pos + i));
                  return;
                }
              st.pos += n;
            }
          });
    sleep_ms(200 + 50 * round);
    const int order[4][4] = {{0, 1, 2, 3}, {3, 2, 1, 0}, {1, 3, 0, 2}, {2, 0, 3, 1}};
    for (int k = 0; k < 4; k++)
    {
      cl.CloseRecordedStream(streams[order[round][k]].sid);
      sleep_ms(30);
    }
    stop = true;
    for (auto& t : readers)
      t.join();
    expect_all_closed(cl);
  }
}

// Destroying the client with recorded streams still open: its destructor closes them all (no leaked handle, and the
// curl share is released afterwards). The leak check stays on for this one, unlike dtor_inflight.
static void multi_dtor(DispatcharrClient& cl)
{
  ctl("ip_reset=20");
  std::vector<OpenStream> streams(4);
  const int ids[4] = {5, 6, 7, 8};
  for (int i = 0; i < 4; i++)
    if (open_stream(cl, ids[i], streams[i]))
      read_verified(cl, streams[i], 20000);
  INFO("destroying the client with %zu recorded streams open", cl.OpenRecordedStreamCount());
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
    else if (what == "live_tail_ended")
      live_tail_ended(*cl);
    else if (what == "live_tail_giveup")
      live_tail_giveup(*cl);
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
    else if (what == "ip_tail_bound")
      ip_tail_bound(*cl);
    else if (what == "live_silent_open")
      silent_open(*cl, false);
    else if (what == "ip_silent_open")
      silent_open(*cl, true);
    else if (what == "live_fileserver_unreachable")
      live_fileserver_unreachable(*cl);
    else if (what == "live_slow_open")
      live_slow_open(*cl);
    else if (what == "live_late_start")
      live_late_start(*cl);
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
    else if (what == "rec_two_same")
      multi_play(*cl, {5, 5}, 0);
    else if (what == "rec_two_distinct")
      multi_play(*cl, {5, 6}, 1);
    else if (what == "ip_two_same")
      multi_play(*cl, {7, 7}, 0);
    else if (what == "ip_two_distinct")
      multi_play(*cl, {7, 8}, 1);
    else if (what == "mixed")
    {
      // Both close orders: the completed stream first, then the in-progress one first.
      multi_play(*cl, {5, 7}, 0);
      multi_play(*cl, {5, 7}, 1);
      multi_play(*cl, {8, 6}, 0);
    }
    else if (what == "ip_open_while_playing")
      ip_open_while_playing(*cl);
    else if (what == "multi_close_race")
      multi_close_race(*cl);
    else if (what == "multi_dtor")
      multi_dtor(*cl);
    else if (what == "dtor_inflight")
    {
      // a stream left open at destruction: does anything leak or fault?
      std::string err;
      int64_t sid = 0;
      cl->OpenLiveTimeshiftStream("uuid", err);
      std::vector<uint8_t> b(65536);
      cl->ReadLiveTimeshiftStream(b.data(), 65536);
      cl->OpenRecordingStream(5, sid, err);
      cl->ReadRecordedStream(sid, b.data(), 1000);
      int64_t sid2 = 0;
      cl->OpenInProgressRecordingStream(7, time(nullptr), sid2, err);
      cl->ReadRecordedStream(sid2, b.data(), 1000);
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
