# Glue harness: the real client and PVR code, outside Kodi

`DispatcharrClient.cpp` and `PVRDispatcharr.cpp` are where this project's real bugs have lived, and the Catch2
suite cannot reach them: they need Kodi's SDK and a Dispatcharr to talk to. This harness compiles the **real
`src/*.cpp`** against Kodi's real dev-kit headers, replaces only the Kodi runtime with small stubs, and drives
the result against a **fake Dispatcharr** (one Python process that plays the API, the `timeshift_buffer`
plugin's `run/` endpoint and file server, completed recordings with `Range`, and a growing in-progress HLS
recording). Every byte the fake serves encodes its own position, so the drivers verify each byte they read,
across seeks, from several threads, under AddressSanitizer + UndefinedBehaviorSanitizer (or ThreadSanitizer).

The scenarios in `ci.sh` (every stream path and the PVR surface, about five minutes under ASan + UBSan) run in CI as
the `glue-harness` job, which fetches only the dev-kit headers of the pinned Kodi commit. The rest stays by hand: the
TSan variant, `ip_chaos`, the plugin integration run and the longer fault matrices. The harness is not part of the
Catch2 build. It is also for running after touching the stream paths, the auth code or the shared curl handles, and
by hardening sweeps. It found the shared-connection-cache libcurl crash (`docs/CLOSED_ITEMS.md`, "Concurrent requests
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
| `live [ms]`, `live_chaos`, `live_reopen`, `live_tail_close`, `live_tail_bound`, `live_tail_ended`, `live_tail_giveup` | live timeshift: play, seek, a `GetStreamTimes` poller, a chaos seeker, five reopens, Close racing a Read at the tail; `live_tail_bound` freezes the buffer for 20 s (no 0, every read under 1.8 s, a clean resume), `live_tail_ended` marks the ffmpeg ended mid-wait (the wait must stop within four seconds), `live_tail_giveup` (by hand, about 70 s) the first 0 between 60 and 90 s; `live_fatal_ends tail|behind` turns the buffer fatal with the reader at the live edge, or behind it with its next segment answering 403 (the stream must end with a 0 within 5 s, every later read must answer 0 at once, and the viewer is told once) |
| `live_fault <ms> "<at_ms>:<knob>=<v>[\|<knob>=<v>]"` | the same with a fault injected mid-playback (outage, hung API, dropped Range, bad size, ended, fatal) |
| `rec "<at_ms>:<knob>=<v>"` | a completed recording: random seeks, outages, a file that shrinks, a 404 |
| `ip [ms] "<at_ms>:<knob>=<v>"`, `ip_chaos` | an in-progress recording: cold open (`IP_AGE` segments of backlog), natural finish, user stop, deleted mid-play, an unsizeable segment, 403 segments, key rotation, foreign host |
| `live_blackhole_close`, `live_blackhole_read` | a hung API with the token hint lapsed: Close and a tail-wait read must stay bounded (run with `TMO=30`) |
| `ip_tail_bound` | an in-progress recording whose tail stops growing: the longest read at the tail (about a second, not the whole budget), the `-1` retries before the first `0`, and the stream resuming when segments arrive |
| `ip_blackhole_close` | Stop while a reader waits at the tail of an in-progress recording and the API hangs: the longest blocked `Read()` is how long Stop waits |
| `rec_range_dropped` | a proxy starts dropping `Range` and sends the whole file slowly: the read must end the stream at once |
| `live_silent_open`, `ip_silent_open` | a server that answers the start request and then never answers the first manifest (live), or answers a missing playlist slowly (in-progress, by hand, about a minute): the open must end once its wall-clock budget has passed, not after every try has taken the full timeout (run with `TMO=5`) |
| `live_fileserver_unreachable` | the buffer starts but the plugin's file server port is not reachable: the open must fail at once with an error naming the port (and queue a notification), and open fine once the port is reachable again |
| `live_late_start` | a channel whose plugin answers "live playlist not found" (no `fatal`) for 25 s after the buffer starts: the open must wait it out, not give up at 15 s |
| `live_refresh_throttle` | polls the stream's length and duration every 20 ms, as Kodi's `GetStreamTimes()` does, for 3 s healthy and 3 s with the manifest answering 429 (`manifest_status=429`): each phase may cost at most seven `get_live_manifest` calls (280 under 429 before a failed refresh armed the throttle) |
| `live_slow_open` | an API answering every request in 6 s: Open must still succeed (its cold-start wait uses the configured timeout) |
| `ip_cascade` | a 400-segment backlog with one transient probe failure: the whole backlog must merge within about a second |
| `dtor_inflight` | destroying the client with a request in flight (run.sh turns leak detection off for it: the leak is the documented limit below) |

`IP_EXPECT_RESUME=1` makes `ip` fail when playback has not resumed after its faults clear (the other drivers only check bytes and miss an early end). `ip_chaos` verifies bytes from a position it reads back while its own seeker thread moves it, so it reports
false mismatches (identical on builds before and after any change); run it with `IP_NOVERIFY=1` as a crash and
hang stress. Fault knobs and counters (`api_down`, `api_hang`, `api_delay_ms`, ...) are documented at the top of `fake_dispatcharr.py` (`/__ctl`, `/__stats`).

`pvr_harness` (`pvr_driver.cpp`, `pvr_stub.cpp`) builds a real `CInstancePVRClient` from a hand-made instance
struct and drives it as Kodi's PVR manager does (everything in the driver that depends on the shape of Kodi's PVR C ABI -- struct string fields, backend strings, properties, EDL and timer-type out-parameters, the recorded-stream calls -- lives in `kodi_abi.h`, so a branch built against another Kodi release swaps that one header): `ctor_dtor`, `dtor_busy`, `functional` (the radio lists, EPG on six channels
including a 5.1, all four timer types; an EPG-based timer's recording must carry the programme the fake's search returned, and still be created when the search answers 404; series rules created under each `series_rule_title_mode` must reach the server with the matching `title_mode`, or none for Exact and for a value outside the list; a manual timer's typed name must be stored on the server, and one carrying the channel's name must not), `surface <ms>` (four threads on lists and EPG, timer add/update/delete
cycles, stream cycles with a `GetStreamTimes` thread, settings changes including the dialog-open dropped-write
case, `OnSystemWake`), and `connection_state <ms> <states>`: what the addon tells Kodi through `ConnectionStateChange`, in order, as a
comma-separated list of `connected`, `denied`, `unreachable`, `mismatch` (empty for none), against a fault set with `CTL` before the constructor
runs (`login_status=401` a rejected login, `429` a throttled one, `502` a proxy error, `404` or `405` no such route, `200` an answer with no token; `api_down=1` a server down at start). In CI for
healthy, 401, 429, down at start, 502, and 404, 405 or a 200 without a token (`mismatch`). With `CONN_RECOVER=1` it clears the fault after the first report and waits for the rest
(`CTL="api_down=1" CONN_RECOVER=1 ... connection_state 170000 unreachable,connected`, about a minute: the login cooldown plus the
background thread's one-minute retry).
`connection_loss` is a server that goes away mid-session with the token still valid (so only `LinkHealth`'s run of failed requests can notice): connected, then `lost` about eleven seconds after the fake starts dropping connections, nothing more while it stays down, `connected` again when a request succeeds. `connection_blip` is five seconds of failures, which must report nothing. Both use `AddTimer` once a second as their traffic and are in CI.
`finished_hls_gone` is a recording the server ended long ago whose `_hls_dir` key was never cleared (`CTL` `ip_state=interrupted`, `ip_hls_key=1`), so the addon opens it on the HLS path: with the playlist redirecting to the file (`ip_dir_gone=1`, `ip_file=1`) the open must fall back to the finished file (its bytes, not real-time); with no playlist and no file (`ip_nodir=1`) it must fail without a notification. In CI.
`epg_redirect` is the guide download behind a redirect, run with `CTL="epg_redirect=loop"` (to itself, forever: at most six requests for the first fetch, 31 before the limit) and `CTL="epg_redirect=ftp"` (to an `ftp://` URL on a listener the fake counts connections to: none). Both are in CI.
`owner_stamps` starts the client with the fake's own key but `api_key_host` naming the server as `localhost` while the addon is set to `127.0.0.1`: the constructor must look the key up once, rewrite `api_key_host`, and a second start must not look it up again. With `late` (`CTL="api_down=1"`, about 30 s) the server is unreachable while the constructor runs and the first recording open confirms the key instead. Both fail on the code before the fix (the stamp kept, a lookup at every start) and both are in CI.
`dvr_access` is a view-only account (`CTL="user_level=1"`): after the connection report, the capabilities Kodi re-reads must hide timers, recording delete and rename and keep the recordings list. With `late` (`CTL="user_level=1&api_down=1"`, about a minute) the fake is unreachable until after Kodi's first read, so the level is learned on the background thread's retry; the addon must not tell the user to restart Kodi. Both are in CI.
`dtor_late_report` destroys the client while the channel/EPG thread is about to schedule a connection report: the fake fails `/api/accounts/users/me/` (`CTL="me_status=500"`) until the thread's first cycle is past its retry, then answers view-only; a wake makes the thread retry, and the stub's log gate (`g_logGate` in `pvr_stub.cpp`, which parks the first thread that logs a matching line) holds it between learning the level and scheduling the report until the destroy has drained the detached-thread list. Before the fix the report's thread was left joinable in that list (`std::terminate`); the scenario also requires the refused task's log line. In CI, under a second.

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
