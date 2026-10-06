#!/usr/bin/env python3
# Two simulated Dispatcharr workers running the real timeshift_buffer plugin against a shared fake Redis,
# a real ffmpeg reading a real-time lavfi MPEG-TS source, and the plugin's own HTTP file server.
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import urllib.error
import contextlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
PLUGIN = os.path.join(REPO, "dispatcharr-plugin", "timeshift_buffer", "plugin.py")
SCEN = sys.argv[1] if len(sys.argv) > 1 else "race"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


RP, LP, HP = free_port(), free_port(), free_port()
STORE = tempfile.mkdtemp(prefix="tsbuf_")
SETTINGS = {
    "storage_path": STORE,
    "http_port": HP,
    "internal_base_url": "http://127.0.0.1:%d" % LP,
    "buffer_minutes": 1,
    "segment_seconds": 1,
    "idle_timeout_seconds": 8,
    "max_concurrent_buffers": 3,
}
procs = []


def spawn(args, **kw):
    p = subprocess.Popen(args, **kw)
    procs.append(p)
    return p


spawn([sys.executable, os.path.join(HERE, "redis_server.py"), str(RP)])
spawn([sys.executable, os.path.join(HERE, "live_source.py"), str(LP)])
time.sleep(0.6)


class Worker:
    def __init__(self, wid):
        env = dict(
            os.environ, PLUGIN_PY=PLUGIN, PLUGIN_SETTINGS=json.dumps(SETTINGS), FAKE_REDIS_PORT=str(RP), WID=str(wid)
        )
        self.p = spawn(
            [sys.executable, os.path.join(HERE, "worker.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            env=env,
            text=True,
        )
        self.lock = threading.Lock()
        self.n = 0
        self.wid = wid

    def call(self, action=None, params=None, stop=None):
        with self.lock:
            self.n += 1
            req = {"id": self.n, "action": action, "params": params or {}}
            if stop:
                req["stop"] = stop
            self.p.stdin.write(json.dumps(req) + "\n")
            self.p.stdin.flush()
            line = self.p.stdout.readline()
            if not line:
                return {"dead": True}
            return json.loads(line)


def ffmpegs():
    out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True).stdout
    return [ln for ln in out.splitlines() if STORE in ln and "ffmpeg" in ln and "worker.py" not in ln]


def http_get(path, rng=None, timeout=5):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (HP, path))
    if rng:
        req.add_header("Range", rng)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
    except Exception as e:
        return -1, {}, repr(e).encode()


problems = []


def problem(msg):
    problems.append(msg)
    print("PROBLEM:", msg, flush=True)


def maxff_watch(stop, out):
    while not stop.is_set():
        n = len(ffmpegs())
        out[0] = max(out[0], n)
        time.sleep(0.1)


def range_check(manifest_res, token, samples=6):
    """Fetch listed segments whole and in ranges; the ranges must equal slices of the whole file."""
    segs = manifest_res.get("segments") or []
    route = manifest_res.get("segment_route_prefix")
    bad = 0
    for seg in segs[-samples:]:
        path = "%s%s?token=%s" % (route, seg["filename"], token)
        st, h, whole = http_get(path)
        if st != 200:
            continue  # rolled off meanwhile
        if len(whole) != seg["byte_size"]:
            problem(
                "segment %s whole size %d != manifest byte_size %d" % (seg["filename"], len(whole), seg["byte_size"])
            )
        for a, b in [(0, 0), (1, 187), (len(whole) - 10, len(whole) - 1), (len(whole) // 2, None)]:
            rng = "bytes=%d-%s" % (a, "" if b is None else b)
            st2, h2, part = http_get(path, rng)
            exp = whole[a : (len(whole) if b is None else b + 1)]
            if st2 != 206 or part != exp:
                bad += 1
                problem(
                    "range %s on %s -> %d len %d (expected 206 len %d) CR=%s"
                    % (rng, seg["filename"], st2, len(part), len(exp), h2.get("Content-Range"))
                )
        st3, h3, part = http_get(path, "bytes=%d-" % (len(whole) + 5))
        if st3 != 416:
            problem("range past end -> %d" % st3)
    return bad


def main():
    w = [Worker(1), Worker(2)]
    stop = threading.Event()
    mx = [0]
    threading.Thread(target=maxff_watch, args=(stop, mx), daemon=True).start()
    ch = "11111111-2222-3333-4444-555555555555"
    if SCEN == "race":
        for rnd in range(int(os.environ.get("ROUNDS", "4"))):
            res = [None, None]
            ts = [
                threading.Thread(
                    target=lambda i=i, res=res, rnd=rnd: res.__setitem__(
                        i, w[i].call("start_buffer", {"channel_uuid": ch, "viewer_id": "v%d_%d" % (i, rnd)})
                    )
                )
                for i in range(2)
            ]
            [t.start() for t in ts]
            [t.join() for t in ts]
            print(
                "round",
                rnd,
                [(r["res"].get("status"), r["res"].get("message", "")[:60], r["res"].get("retryable")) for r in res],
                "ffmpegs",
                len(ffmpegs()),
                flush=True,
            )
            tokens = {r["res"].get("access_token") for r in res if r["res"].get("status") == "ok"}
            if len(tokens) > 1:
                problem("two different access tokens handed out for one channel: %s" % tokens)
            t0 = time.time()
            while True:
                m = w[rnd % 2].call("get_live_manifest", {"channel_uuid": ch, "viewer_id": "v0_%d" % rnd})["res"]
                if m.get("status") == "ok" and len(m.get("segments", [])) >= 4 or time.time() - t0 > 20:
                    break
                time.sleep(0.5)
            if m.get("status") == "ok":
                tok = next(iter(tokens)) if tokens else ""
                range_check(m, tok)
                print("  manifest segs", len(m.get("segments", [])), "ended", m.get("ended"), flush=True)
            else:
                print("  manifest", m, flush=True)
            # interleaved stops from both workers, plus a concurrent heartbeat
            ts = [
                threading.Thread(
                    target=lambda i=i, rnd=rnd: w[i].call(
                        "stop_buffer", {"channel_uuid": ch, "viewer_id": "v%d_%d" % (i, rnd)}
                    )
                )
                for i in range(2)
            ]
            ts.append(
                threading.Thread(
                    target=lambda rnd=rnd: w[0].call("heartbeat", {"channel_uuid": ch, "viewer_id": "v0_%d" % rnd})
                )
            )
            [t.start() for t in ts]
            [t.join() for t in ts]
            time.sleep(0.5)
            n = len(ffmpegs())
            print("  after both stops: ffmpegs", n, flush=True)
            if n:
                time.sleep(3)
                if ffmpegs():
                    problem("round %d: ffmpeg still running 3.5 s after the last viewer stopped: %s" % (rnd, ffmpegs()))
    elif SCEN == "reload":
        r = w[0].call("start_buffer", {"channel_uuid": ch, "viewer_id": "a"})
        print(r["res"].get("status"), flush=True)
        time.sleep(10)
        w[1].call(stop="reload")  # Plugins page "Reload" in the OTHER worker
        time.sleep(1)
        m = w[0].call("get_live_manifest", {"channel_uuid": ch, "viewer_id": "a"})["res"]
        print(
            "after reload in worker 2: manifest",
            m.get("status"),
            len(m.get("segments", [])),
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        if m.get("status") != "ok":
            problem("buffer did not survive a reload in another worker")
        # the worker that reloaded is used again (new module state after reload is a fresh import in reality)
        r2 = w[1].call("start_buffer", {"channel_uuid": ch, "viewer_id": "b"})["res"]
        print(
            "worker2 start after reload:",
            r2.get("status"),
            r2.get("message", "")[:80],
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        w[0].call(stop="disable")
        time.sleep(1)
        print("after disable: ffmpegs", len(ffmpegs()), flush=True)
    elif SCEN == "crash":
        r = w[0].call("start_buffer", {"channel_uuid": ch, "viewer_id": "a"})["res"]
        tok = r.get("access_token")
        time.sleep(4)
        pids = [int(ln.split()[0]) for ln in ffmpegs()]
        print("killing ffmpeg", pids, flush=True)
        for p in pids:
            os.kill(p, 9)
        time.sleep(1)
        for _i in range(3):
            m = w[1].call("get_live_manifest", {"channel_uuid": ch, "viewer_id": "a", "access_token": tok})["res"]
            print(
                " manifest after kill:",
                m.get("status"),
                "ended",
                m.get("ended"),
                "fatal",
                m.get("fatal"),
                len(m.get("segments", []) or []),
                (m.get("message") or "")[:80],
                flush=True,
            )
            time.sleep(1)
        r2 = w[1].call("start_buffer", {"channel_uuid": ch, "viewer_id": "b"})["res"]
        print(
            " restart:",
            r2.get("status"),
            (r2.get("message") or "")[:90],
            "new token",
            r2.get("access_token") != tok,
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        time.sleep(3)
        m = w[0].call("get_live_manifest", {"channel_uuid": ch, "viewer_id": "a", "access_token": tok})["res"]
        print(
            " old viewer with old token:",
            m.get("status"),
            "fatal",
            m.get("fatal"),
            (m.get("message") or "")[:80],
            flush=True,
        )
    elif SCEN == "workerdeath":
        r = w[0].call("start_buffer", {"channel_uuid": ch, "viewer_id": "a"})["res"]
        w[1].call("list_buffers")
        time.sleep(10)
        w[0].p.kill()
        w[0].p.wait()
        print("worker 1 killed; ffmpegs", len(ffmpegs()), flush=True)
        time.sleep(1)
        m = w[1].call("get_live_manifest", {"channel_uuid": ch, "viewer_id": "a"})["res"]
        print(
            " manifest via worker 2:",
            m.get("status"),
            len(m.get("segments", []) or []),
            (m.get("message") or "")[:80],
            flush=True,
        )
        st, h, body = http_get("/%s/live.m3u8?token=%s" % (ch, r.get("access_token")))
        print(" file server after worker 1 death:", st, flush=True)
        print(" waiting for idle reaper (idle 8 s)...", flush=True)
        t0 = time.time()
        while ffmpegs() and time.time() - t0 < 120:
            time.sleep(2)
            w[1].call("list_buffers")
        print(" ffmpeg gone after %.0f s more (idle timeout 8 s)" % (time.time() - t0), len(ffmpegs()), flush=True)
    elif SCEN == "viewers":
        r1 = w[0].call("start_buffer", {"channel_uuid": ch, "viewer_id": "A"})["res"]
        time.sleep(1)
        r2 = w[1].call("start_buffer", {"channel_uuid": ch, "viewer_id": "B"})["res"]
        print(
            "A:",
            r1.get("status"),
            "B:",
            r2.get("status"),
            (r2.get("message") or "")[:60],
            "same token",
            r1.get("access_token") == r2.get("access_token"),
            flush=True,
        )
        s1 = w[1].call("stop_buffer", {"channel_uuid": ch, "viewer_id": "ZZZ"})["res"]
        print(
            "stop unknown viewer:",
            s1.get("status"),
            (s1.get("message") or "")[:80],
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        s2 = w[1].call("stop_buffer", {"channel_uuid": ch})["res"]
        print(
            "stop without viewer_id:",
            s2.get("status"),
            (s2.get("message") or "")[:80],
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        if not ffmpegs():
            problem("a stop for an unknown/absent viewer tore down a buffer two real viewers still hold")
        # concurrent stop of A and B from different workers, then a start racing them
        res = {}
        ts = [
            threading.Thread(
                target=lambda: res.__setitem__(
                    "sa", w[0].call("stop_buffer", {"channel_uuid": ch, "viewer_id": "A"})["res"]
                )
            ),
            threading.Thread(
                target=lambda: res.__setitem__(
                    "sb", w[1].call("stop_buffer", {"channel_uuid": ch, "viewer_id": "B"})["res"]
                )
            ),
        ]
        [t.start() for t in ts]
        [t.join() for t in ts]
        time.sleep(0.3)
        r3 = w[0].call("start_buffer", {"channel_uuid": ch, "viewer_id": "C"})["res"]
        print(
            "stops:",
            {k: (v.get("message") or "")[:50] for k, v in res.items()},
            "start C:",
            r3.get("status"),
            (r3.get("message") or "")[:60],
            "retry",
            r3.get("retryable"),
            "ffmpegs",
            len(ffmpegs()),
            flush=True,
        )
        time.sleep(2)
        print("ffmpegs now", len(ffmpegs()), w[1].call("list_buffers")["res"].get("message"), flush=True)
    elif SCEN == "many":
        # max_concurrent_buffers=3 across two workers, 6 channels started concurrently
        chans = ["%08d-0000-0000-0000-000000000000" % i for i in range(6)]
        res = {}
        ts = [
            threading.Thread(
                target=lambda c=c, i=i: res.__setitem__(
                    c, w[i % 2].call("start_buffer", {"channel_uuid": c, "viewer_id": "v"})["res"]
                )
            )
            for i, c in enumerate(chans)
        ]
        [t.start() for t in ts]
        [t.join() for t in ts]
        ok = [c for c in chans if res[c].get("status") == "ok"]
        print(
            "started ok:",
            len(ok),
            "of 6; ffmpegs",
            len(ffmpegs()),
            [res[c].get("message", "")[:50] for c in chans],
            flush=True,
        )
        time.sleep(2)
        print("ffmpegs after 2s", len(ffmpegs()), flush=True)
    # teardown and leak checks
    r = w[1].call("stop_all")
    time.sleep(3)
    stop.set()
    left = ffmpegs()
    print("max concurrent ffmpegs seen:", mx[0], " left after stop_all:", len(left), flush=True)
    if left:
        problem("ffmpeg left running after stop_all: %s" % left)
    dirs = [d for d in os.listdir(STORE)]
    print("storage entries after stop_all:", dirs, flush=True)
    for x in w:
        with contextlib.suppress(Exception):
            x.p.stdin.close()


try:
    main()
finally:
    for p in procs:
        with contextlib.suppress(Exception):
            p.kill()
    for ln in ffmpegs():
        with contextlib.suppress(Exception):
            os.kill(int(ln.split()[0]), 9)
    print("RESULT problems=%d" % len(problems))
