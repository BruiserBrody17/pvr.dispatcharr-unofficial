# Glue harness: the real client and PVR code, outside Kodi

`DispatcharrClient.cpp` and `PVRDispatcharr.cpp` are where this project's real bugs have lived, and the Catch2
suite cannot reach them: they need Kodi's SDK and a Dispatcharr to talk to. This harness compiles the **real
`src/*.cpp`** against Kodi's real dev-kit headers, replaces only the Kodi runtime with small stubs, and drives
the result against a **fake Dispatcharr** (one Python process that plays the API, the `timeshift_buffer`
plugin's `run/` endpoint and file server, completed recordings with `Range`, and a growing in-progress HLS
recording). Every byte the fake serves encodes its own position, so the drivers verify each byte they read,
across seeks, from several threads, under AddressSanitizer + UndefinedBehaviorSanitizer (or ThreadSanitizer).

The scenarios in `ci.sh` (every stream path and the PVR surface, about three minutes under ASan + UBSan) run in CI as
the `glue-harness` job, which fetches only the dev-kit headers of the pinned Kodi commit. The rest stays by hand: the
TSan variant, `ip_chaos`, the plugin integration run and the longer fault matrices. The harness is not part of the
Catch2 build. It is also for running after touching the stream paths, the auth code or the shared curl handles, and
by hardening sweeps. It found the shared-connection-cache libcurl crash (`docs/OPEN_ITEMS.md`, "Concurrent requests
could crash libcurl...") and the blackhole, probe-storm and retry-storm findings of the fourteenth sweep.

## Build and run

```bash
cmake -S tests -B build-tests          # once: fetches nlohmann-json and pugixml into build-tests/_deps
tests/glue/build.sh asan               # or tsan, or plain; output in build-glue/<variant>/
tests/glue/run.sh live 1500            # the live-timeshift stream path, 1.5 s of playback
BIN=pvr_harness tests/glue/run.sh functional
tests/glue/run_tsan.sh ip 5000         # after build.sh tsan; retries while TSan's runtime refuses the layout
```

`KODI_INCLUDE` points at `kodi-dev-kit/include` of a Kodi source checkout (default
`~/kodi-build/kodi-source/xbmc/addons/kodi-dev-kit/include`, the tree `docs/BUILDING.md` sets up).
`run.sh` takes `BIN` (`stream_harness` or `pvr_harness`), `VARIANT`, `T` (seconds before it is killed), `TMO` (the
client's request timeout, seconds) and `CTL="knob=value&knob=value"` (faults set on the fake before the run).
A run prints `RESULT fails=N` and `rc=0` when it passed.

## Scenarios

`stream_harness` (`stream_driver.cpp`'s `main()`):

| Scenario | What it does |
|---|---|
| `live [ms]`, `live_chaos`, `live_reopen`, `live_tail_close` | live timeshift: play, seek, a `GetStreamTimes` poller, a chaos seeker, five reopens, Close racing a Read at the tail |
| `live_fault <ms> "<at_ms>:<knob>=<v>[\|<knob>=<v>]"` | the same with a fault injected mid-playback (outage, hung API, dropped Range, bad size, ended, fatal) |
| `rec "<at_ms>:<knob>=<v>"` | a completed recording: random seeks, outages, a file that shrinks, a 404 |
| `ip [ms] "<at_ms>:<knob>=<v>"`, `ip_chaos` | an in-progress recording: cold open (`IP_AGE` segments of backlog), natural finish, user stop, deleted mid-play, an unsizeable segment, 403 segments, key rotation, foreign host |
| `live_blackhole_close`, `live_blackhole_read` | a hung API with the token hint lapsed: Close and a tail-wait read must stay bounded (run with `TMO=30`) |
| `ip_blackhole_close` | Stop while a reader waits at the tail of an in-progress recording and the API hangs: the longest blocked `Read()` is how long Stop waits |
| `rec_range_dropped` | a proxy starts dropping `Range` and sends the whole file slowly: the read must end the stream at once |
| `live_slow_open` | an API answering every request in 6 s: Open must still succeed (its cold-start wait uses the configured timeout) |
| `ip_cascade` | a 400-segment backlog with one transient probe failure: the whole backlog must merge within about a second |
| `dtor_inflight` | destroying the client with a request in flight (run.sh turns leak detection off for it: the leak is the documented limit below) |

`IP_EXPECT_RESUME=1` makes `ip` fail when playback has not resumed after its faults clear (the other drivers only check bytes and miss an early end). `ip_chaos` verifies bytes from a position it reads back while its own seeker thread moves it, so it reports
false mismatches (identical on builds before and after any change); run it with `IP_NOVERIFY=1` as a crash and
hang stress. Fault knobs and counters (`api_down`, `api_hang`, `api_delay_ms`, ...) are documented at the top of `fake_dispatcharr.py` (`/__ctl`, `/__stats`).

`pvr_harness` (`pvr_driver.cpp`, `pvr_stub.cpp`) builds a real `CInstancePVRClient` from a hand-made instance
struct and drives it as Kodi's PVR manager does: `ctor_dtor`, `dtor_busy`, `functional` (EPG on six channels
including a 5.1, all four timer types), `surface <ms>` (four threads on lists and EPG, timer add/update/delete
cycles, stream cycles with a `GetStreamTimes` thread, settings changes including the dialog-open dropped-write
case, `OnSystemWake`).

## Plugin integration (`plugin_integration/`)

`python3 tests/glue/plugin_integration/driver.py race|reload|crash|workerdeath|viewers|many` runs two simulated
Dispatcharr workers (separate processes importing the real `timeshift_buffer/plugin.py`) against a fake Redis
(`redis_server.py`, a small RESP server that runs the plugin's two Lua scripts), a **real ffmpeg** reading a
real-time lavfi source (so `ffmpeg` with libx264 must be installed), and the plugin's own file server: racing
`start_buffer`s, a plugin reload in one worker, a killed ffmpeg, a dead worker, `Range` reads checked against
the file. It exercises the same code the unit tests mock around.

## Limits

* Run it on Linux with GNU `g++` and a Kodi checkout; ThreadSanitizer needs a no-PIE build and aborts with
  "unexpected memory mapping" on roughly half of all starts on kernels with high mmap randomisation
  (`run_tsan.sh` retries), `setarch -R` was not permitted where it was tried.
* The fake is faithful to what this project has measured of Dispatcharr, not to Dispatcharr itself; a scenario
  passing here is not a substitute for the live checks in `docs/MANUAL_TESTING.md`.
* Destroying the client with a stream still open leaks three curl handles and the share (LSan); Kodi's
  `StopClient()` stops playback first, so it is not reachable.
