#!/usr/bin/env python3
# Fake Dispatcharr + timeshift_buffer plugin + recording endpoints, for the stream-path glue harness.
# Content is self-describing: every 8-byte record is struct('>II', tag, index) so the harness can
# verify every byte it reads.  Runtime knobs via GET /__ctl?k=v ; counters via GET /__stats.
import base64
import contextlib
import datetime
import hashlib
import select
import functools
import json
import sys
import time
import threading
import collections
import struct
import socket
import urllib.parse
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

PORT = int(sys.argv[1])
counts = collections.Counter()
lock = threading.Lock()
CTL = collections.defaultdict(str)
CTL.update({"seg_ms": "300", "window": "12", "ip_seg_ms": "300"})
T0 = {"live": None, "ip": time.time()}
LIVE_FROZEN_AT = {"n": None}
IP_FROZEN_AT = {"n": None}
TOKEN = {"v": "tok1"}
WS_PUSHED = []  # realtime messages pushed through /__ctl?ws_push= (every connected client gets all of them)
REC_SIZE = 3 * 1024 * 1024 + 5


def ctl_int(k, d=0):
    try:
        return int(CTL[k]) if CTL[k] != "" else d
    except ValueError:
        return d


def seg_size(seq):  # multiple of 8
    return 8 * (1500 + (seq * 37) % 700)


@functools.lru_cache(maxsize=4096)
def seg_bytes(tag, seq, size):
    n = size // 8
    return b"".join(struct.pack(">II", tag | (seq & 0xFFFF), k) for k in range(n))


def rec_bytes(start, end):  # [start, end) of the completed recording
    out = bytearray()
    first = start // 8
    last = (end + 7) // 8
    for k in range(first, last):
        out += struct.pack(">II", 0xC0FFEE00, k)
    off = start - first * 8
    return bytes(out[off : off + (end - start)])


def live_count():
    if T0["live"] is None:
        return 0
    if LIVE_FROZEN_AT["n"] is not None:
        return LIVE_FROZEN_AT["n"]
    return 4 + int((time.time() - T0["live"]) * 1000 / ctl_int("seg_ms", 300))


def ip_count():
    if IP_FROZEN_AT["n"] is not None:
        return IP_FROZEN_AT["n"]
    return int((time.time() - T0["ip"]) * 1000 / ctl_int("ip_seg_ms", 300))


# ---------------- PVR model (for the PVRDispatcharr harness)
DB = {
    "recs": {},
    "next_id": 100,
    "series": [],
    "recurring": {},
    "next_rule": 1,
    "settings": {
        1: {
            "id": 1,
            "key": "dvr_settings",
            "value": {"pre_offset_minutes": 1, "post_offset_minutes": 2, "comskip": {"x": 1}},
        },
        2: {"id": 2, "key": "system_settings", "value": {"time_zone": "UTC", "catchup_enabled": True}},
    },
}
DBL = threading.Lock()
NCH = 6


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def channels():
    out = []
    for i in range(1, NCH + 1):
        out.append(
            {
                "id": i,
                "uuid": "uuid%d" % i,
                "name": "Channel %d" % i,
                "channel_number": i if i != 6 else 5.1,
                "channel_group_id": 1 + (i % 2),
                "tvg_id": "t%d" % i,
                "logo_id": None if i % 3 else i,
                "is_catchup": i % 2 == 0,
                "catchup_days": 3,
                "epg_data_id": i,
            }
        )
    return out


def xmltv():
    now = int(time.time()) // 1800 * 1800
    parts = ['<?xml version="1.0" encoding="UTF-8"?><tv>']
    for c in channels():
        num = c["channel_number"]
        key = str(int(num)) if float(num).is_integer() else str(num)
        parts.append('<channel id="%s"><display-name>%s</display-name></channel>' % (key, c["name"]))
    for c in channels():
        num = c["channel_number"]
        key = str(int(num)) if float(num).is_integer() else str(num)
        for k in range(-12, 48):
            st = now + k * 1800

            def f(t):
                return time.strftime("%Y%m%d%H%M%S +0000", time.gmtime(t))

            parts.append(
                '<programme start="%s" stop="%s" channel="%s"><title>Show %s-%d</title><desc>d</desc>'
                '<episode-num system="xmltv_ns">0.%d.</episode-num><category>News</category></programme>'
                % (f(st), f(st + 1800), key, key, k % 5, k % 7)
            )
    parts.append("</tv>")
    return "".join(parts).encode()


def pvr_route(h, method, p, q, body):
    if CTL["pvr_api_500"] == "1":
        return h.J(500, {"detail": "boom"})
    if p == "/api/core/version/":
        return h.J(200, {"version": "0.31.0"})
    if p == "/api/accounts/users/me/":
        return h.J(200, {"id": 1, "username": "u", "user_level": 10, "custom_properties": {"catchup_enabled": True}})
    if p == "/api/core/timezones/":
        return h.J(200, {"timezones": ["UTC", "Europe/London", "Asia/Tokyo"]})
    if p == "/api/core/settings/":
        with DBL:
            return h.J(200, list(DB["settings"].values()))
    if p.startswith("/api/core/settings/") and method == "PATCH":
        sid = int(p.split("/")[4])
        with DBL:
            DB["settings"][sid]["value"] = body.get("value", {})
            return h.J(200, DB["settings"][sid])
    if p == "/api/channels/channels/":
        if CTL["channels_slow_ms"]:
            time.sleep(ctl_int("channels_slow_ms") / 1000.0)
        return h.J(200, channels())
    if p == "/api/channels/groups/":
        return h.J(200, [{"id": 1, "name": "G1"}, {"id": 2, "name": "G2"}, {"id": 3, "name": "Empty"}])
    if p == "/output/epg":
        if CTL["epg_slow_ms"]:
            time.sleep(ctl_int("epg_slow_ms") / 1000.0)
        return h.send(200, xmltv(), "application/xml")
    if p.startswith("/api/epg/epgdata/"):
        eid = int(p.split("/")[4])
        return h.J(200, {"id": eid, "tvg_id": "t%d" % eid, "epg_source": 1})
    if p == "/api/channels/recordings/":
        if method == "POST":
            with DBL:
                rid = DB["next_id"]
                DB["next_id"] += 1
                rec = {
                    "id": rid,
                    "channel": body.get("channel"),
                    "start_time": body.get("start_time"),
                    "end_time": body.get("end_time"),
                    "custom_properties": dict(body.get("custom_properties") or {}, status="scheduled"),
                }
                if body.get("title"):
                    rec["custom_properties"]["program"] = {"title": body.get("title")}
                DB["recs"][rid] = rec
                return h.J(201, rec)
        with DBL:
            lst = list(DB["recs"].values())
        lst.append(
            {
                "id": 5,
                "channel": 1,
                "start_time": "2026-10-01T10:00:00Z",
                "end_time": "2026-10-01T11:00:00Z",
                "custom_properties": {"status": "completed", "program": {"title": "Done show"}},
            }
        )
        lst.append(
            {
                "id": 7,
                "channel": 2,
                "start_time": iso(time.time() - 600),
                "end_time": iso(time.time() + 3600),
                "custom_properties": {
                    "status": CTL["ip_state"] or "recording",
                    "_hls_dir": "/x",
                    "program": {"title": "Live rec"},
                },
            }
        )
        return h.J(200, lst)
    if p.startswith("/api/channels/recordings/"):
        parts = [x for x in p.split("/") if x]
        rid = int(parts[3])
        with DBL:
            rec = DB["recs"].get(rid)
            if rec is None:
                return h.J(404, {"detail": "nf"})
            if method == "DELETE":
                del DB["recs"][rid]
                return h.send(204, b"")
            if method == "PATCH":
                rec.update({k: v for k, v in body.items() if k in ("start_time", "end_time", "channel")})
                return h.J(200, rec)
            if len(parts) > 4 and parts[4] == "stop":
                rec["custom_properties"]["status"] = "stopped"
                return h.J(200, rec)
            if len(parts) > 4:
                return h.J(200, rec)
            return h.J(200, rec)
    if p.startswith("/api/channels/series-rules/"):
        with DBL:
            if method == "GET":
                return h.J(200, {"rules": DB["series"]})
            if method == "POST" and p.endswith("evaluate/"):
                return h.J(200, {"ok": True})
            if method == "POST":
                DB["series"] = [
                    r
                    for r in DB["series"]
                    if not (r.get("title") == body.get("title") and r.get("tvg_id") == body.get("tvg_id"))
                ]
                DB["series"].append(body)
                return h.J(200, {"rules": DB["series"]})
            if method == "DELETE":
                t = q.get("title", [""])[0]
                DB["series"] = [r for r in DB["series"] if r.get("title") != t]
                return h.J(200, {"rules": DB["series"]})
    if p.startswith("/api/channels/recurring-rules/"):
        parts = [x for x in p.split("/") if x]
        with DBL:
            if len(parts) == 3:
                if method == "POST":
                    rid = DB["next_rule"]
                    DB["next_rule"] += 1
                    r = dict(body, id=rid)
                    DB["recurring"][rid] = r
                    return h.J(201, r)
                return h.J(200, list(DB["recurring"].values()))
            rid = int(parts[3])
            r = DB["recurring"].get(rid)
            if r is None:
                return h.J(404, {"detail": "nf"})
            if method == "DELETE":
                del DB["recurring"][rid]
                return h.send(204, b"")
            if method == "PATCH":
                r.update(body)
                return h.J(200, r)
            return h.J(200, r)
    if p.startswith("/api/channels/logos/"):
        return h.send(200, b"PNG", "image/png")
    return None


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body=b"", ctype="application/json", extra=None, head=False):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if not head:
            self.wfile.write(body)
        return True

    def J(self, code, obj):
        return self.send(code, json.dumps(obj).encode())

    def ranged(self, data, head=False, ignore_range=False, total_override=None):
        rng = self.headers.get("Range")
        total = len(data) if total_override is None else total_override
        if rng and not ignore_range and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            a = int(a)
            b = int(b) if b else len(data) - 1
            b = min(b, len(data) - 1)
            if a >= len(data):
                return self.send(416, b"", "video/mp2t", {"Content-Range": "bytes */%d" % total}, head)
            return self.send(
                206, data[a : b + 1], "video/mp2t", {"Content-Range": "bytes %d-%d/%d" % (a, b, total)}, head
            )
        return self.send(200, data, "video/mp2t", None, head)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def websocket(self):
        """Dispatcharr's realtime socket: a connection_established message, then whatever /__ctl?ws_push= queues, as
        text frames, until the client goes away (or ~2 minutes pass). Control frames from the client are not read."""
        accept = base64.b64encode(
            hashlib.sha1((self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.close_connection = True

        def frame(text):
            data = text.encode()
            n = len(data)
            head = bytes([0x81, n]) if n < 126 else bytes([0x81, 126]) + struct.pack(">H", n)
            return head + data

        try:
            self.wfile.write(frame(json.dumps({"type": "connection_established", "data": {"success": True}})))
            self.wfile.flush()
            deadline = time.time() + 120
            sent = 0
            while time.time() < deadline:
                with lock:
                    pending, sent = WS_PUSHED[sent:], len(WS_PUSHED)
                for text in pending:
                    self.wfile.write(frame(text))
                self.wfile.flush()
                readable, _, _ = select.select([self.connection], [], [], 0.1)
                if readable and not self.connection.recv(4096, socket.MSG_PEEK):
                    break
        except OSError:
            pass
        return True

    def handle_any(self, method):
        b = self.body()
        u = urllib.parse.urlsplit(self.path)
        p = u.path
        q = urllib.parse.parse_qs(u.query)
        key = method + " " + p
        if p.startswith("/tsbuf/"):
            key = method + " /tsbuf/seg"
        if "/hls/seg_" in p:
            key = method + " /hls/seg"
        with lock:
            counts[key] += 1
        if p == "/ws/" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        if p == "/__stats":
            return self.J(200, dict(counts))
        if p == "/__ctl":
            for k, v in q.items():
                CTL[k] = v[0]
                if k == "live_freeze":
                    LIVE_FROZEN_AT["n"] = live_count() if v[0] == "1" else None
                    if v[0] != "1" and T0["live"] is not None:
                        pass
                if k == "ip_freeze":
                    IP_FROZEN_AT["n"] = ip_count() if v[0] == "1" else None
                if k == "ip_reset":
                    T0["ip"] = time.time() - float(v[0] if v[0] not in ("", "1") else 0)
                    IP_FROZEN_AT["n"] = None
                if k == "token":
                    TOKEN["v"] = v[0]
                if k == "ws_push":
                    with lock:
                        WS_PUSHED.append(v[0])
                if k == "seed_offset_check":
                    # A recurring rule at 19:30 local and the occurrence the server materialized for it tomorrow at
                    # 11:30 UTC: UTC+8, while the table says UTC+9 for the server's zone. The dates follow the clock
                    # (the check only reads occurrences that have not started), and the zone has no daylight saving so
                    # the answer does not depend on the time of year the scenario runs.
                    today = datetime.datetime.now(datetime.timezone.utc).date()
                    tomorrow = today + datetime.timedelta(days=1)
                    with DBL:
                        DB["settings"][2]["value"]["time_zone"] = "Asia/Tokyo"
                        DB["recurring"][900] = {
                            "id": 900,
                            "name": "ZZZ offset rule [Kodi]",
                            "channel": 1,
                            "enabled": True,
                            "days_of_week": [0, 1, 2, 3, 4, 5, 6],
                            "start_time": "19:30:00",
                            "end_time": "20:30:00",
                            "start_date": today.isoformat(),
                            "end_date": (today + datetime.timedelta(days=30)).isoformat(),
                        }
                        DB["recs"][901] = {
                            "id": 901,
                            "channel": 1,
                            "start_time": tomorrow.isoformat() + "T11:30:00Z",
                            "end_time": tomorrow.isoformat() + "T12:30:00Z",
                            "custom_properties": {
                                "status": "scheduled",
                                "rule": {"type": "recurring", "id": 900},
                                "program": {"title": "ZZZ offset rule"},
                            },
                        }
            return self.J(200, {"ok": True})
        if p == "/__db":
            with DBL:
                return self.J(
                    200,
                    {
                        "recs": DB["recs"],
                        "series": DB["series"],
                        "recurring": DB["recurring"],
                        "settings": DB["settings"],
                    },
                )
        if p == "/__reset":
            with lock:
                counts.clear()
            return self.J(200, {})
        if CTL["api_down"] == "1" and not p.startswith("/tsbuf/"):
            self.close_connection = True
            with contextlib.suppress(OSError):
                self.connection.shutdown(socket.SHUT_RDWR)
            return
        if ctl_int("api_delay_ms") and not p.startswith("/tsbuf/"):
            time.sleep(ctl_int("api_delay_ms") / 1000.0)
        if CTL["api_hang"] == "1" and not p.startswith("/tsbuf/"):
            time.sleep(40)
            return self.J(503, {})
        if p == "/api/accounts/token/":
            return self.J(200, {"access": "A", "refresh": "R"})
        if p == "/api/accounts/token/refresh/":
            return self.J(200, {"access": "AR"})
        if p == "/api/accounts/api-keys/":
            return self.J(200, {"key": CTL["require_key"] or "K1"})
        if p == "/api/plugins/plugins/timeshift_buffer/run/":
            req = json.loads(b or b"{}")
            act = req.get("action")
            req.get("params", {})
            with lock:
                counts["action " + str(act)] += 1
            if act == "start_buffer":
                if CTL["start_fail"] == "1":
                    return self.J(200, {"success": True, "result": {"status": "error", "message": "limit"}})
                if T0["live"] is None:
                    T0["live"] = time.time()
                return self.J(
                    200,
                    {
                        "success": True,
                        "result": {
                            "status": "ok",
                            "http_port": PORT,
                            "playlist_route": "/tsbuf/u/live.m3u8",
                            "access_token": TOKEN["v"],
                        },
                    },
                )
            if act == "stop_buffer":
                return self.J(200, {"success": True, "result": {"status": "ok"}})
            if act == "heartbeat":
                return self.J(200, {"success": True, "result": {"status": "ok"}})
            if act == "get_live_manifest":
                if CTL["live_fatal"] == "1":
                    return self.J(
                        200, {"success": True, "result": {"status": "error", "fatal": True, "message": "dead"}}
                    )
                if CTL["manifest_500"] == "1":
                    return self.J(500, {"detail": "x"})
                n = live_count()
                w = ctl_int("window", 12)
                segs = []
                for s in range(max(0, n - w), n):
                    sz = seg_size(s)
                    if CTL["bad_size_from"] != "" and s >= ctl_int("bad_size_from"):
                        sz += 8
                    segs.append(
                        {
                            "filename": "seg_%05d.ts" % (s % 1000),
                            "sequence": s,
                            "byte_size": sz,
                            "duration_ms": ctl_int("seg_ms", 300),
                        }
                    )
                if CTL["live_empty"] == "1":
                    segs = []
                return self.J(
                    200,
                    {
                        "success": True,
                        "result": {
                            "status": "ok",
                            "http_port": PORT,
                            "segment_route_prefix": "/tsbuf/u/",
                            "segments": segs,
                            "ended": CTL["live_ended"] == "1",
                        },
                    },
                )
            return self.J(200, {"success": True, "result": {"status": "ok"}})
        if p.startswith("/tsbuf/"):
            if q.get("token", [""])[0] != TOKEN["v"]:
                return self.send(403, b"bad token")
            st = ctl_int("seg_status", 0)
            if st:
                return self.send(st, b"x")
            if CTL["seg_drop"] == "1":
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                return
            if CTL["seg_slow_ms"]:
                time.sleep(ctl_int("seg_slow_ms") / 1000.0)
            name = p.rsplit("/", 1)[1]
            idx = int(name[4:9])
            n = live_count()
            w = ctl_int("window", 12)
            # filenames recycle mod 1000; find the newest seq with that name still on disk (2x window kept)
            cands = [s for s in range(max(0, n - 2 * w), n) if s % 1000 == idx]
            if not cands:
                return self.send(404, b"nf")
            s = cands[-1]
            return self.ranged(seg_bytes(0x5E000000, s, seg_size(s)), method == "HEAD", CTL["seg_norange"] == "1")
        if (
            p != "/api/channels/recordings/"
            and p.startswith("/api/channels/recordings/")
            and p.split("/")[4:5] not in (["5"], ["7"])
        ):
            r = pvr_route(self, method, p, q, json.loads(b) if b else {})
            if r is None:
                self.J(404, {"detail": "nf"})
            return
        if p.startswith("/api/channels/recordings/") and p != "/api/channels/recordings/":
            parts = [x for x in p.split("/") if x]
            rid = int(parts[3]) if len(parts) > 3 and parts[3].isdigit() else -1
            rest = parts[4:]
            if CTL["require_key"] and rest and self.headers.get("X-API-Key") != CTL["require_key"]:
                with lock:
                    counts["401 " + key] += 1
                return self.send(401, b'{"detail":"bad key"}')
            if rid == 5:  # completed recording
                if rest == ["file"]:
                    st = ctl_int("rec_status", 0)
                    if st:
                        return self.send(st, b"x")
                    size = REC_SIZE if CTL["rec_shrunk"] != "1" else REC_SIZE - 4096
                    rng = self.headers.get("Range")
                    data_len = size
                    ign = CTL["rec_norange"] == "1"
                    if rng and not ign and rng.startswith("bytes="):
                        a, _, bb = rng[6:].partition("-")
                        a = int(a)
                        bb = int(bb) if bb else data_len - 1
                        bb = min(bb, data_len - 1)
                        if a >= data_len:
                            return self.send(
                                416, b"", "video/mp2t", {"Content-Range": "bytes */%d" % data_len}, method == "HEAD"
                            )
                        return self.send(
                            206,
                            rec_bytes(a, bb + 1),
                            "video/mp2t",
                            {"Content-Range": "bytes %d-%d/%d" % (a, bb, data_len)},
                            method == "HEAD",
                        )
                    if ign and ctl_int("rec_slow_ms") and method != "HEAD":
                        # a body that takes a while to arrive, as a multi-gigabyte file over a real link does
                        body = rec_bytes(0, data_len)
                        self.send_response(200)
                        self.send_header("Content-Type", "video/mp2t")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        try:
                            for off in range(0, len(body), 65536):
                                self.wfile.write(body[off : off + 65536])
                                self.wfile.flush()
                                time.sleep(ctl_int("rec_slow_ms") / 1000.0)
                        except OSError:
                            pass  # the client ended the transfer
                        return True
                    return self.send(200, rec_bytes(0, data_len), "video/mp2t", None, method == "HEAD")
                return self.J(
                    200,
                    {
                        "id": 5,
                        "start_time": "2026-10-01T10:00:00Z",
                        "end_time": "2026-10-01T11:00:00Z",
                        "custom_properties": {"status": "completed"},
                    },
                )
            if rid == 7:  # in-progress recording
                state = CTL["ip_state"] or "recording"
                if rest == []:
                    if state == "deleted":
                        return self.J(404, {"detail": "nf"})
                    now = time.time()
                    st = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now - 600))
                    en = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now + 3600))
                    return self.J(
                        200,
                        {
                            "id": 7,
                            "channel": 1,
                            "start_time": st,
                            "end_time": en,
                            "custom_properties": {"status": state},
                        },
                    )
                if rest == ["file"]:
                    return self.J(404, {"detail": "nf"})
                if rest == ["hls", "index.m3u8"]:
                    if state == "deleted" or CTL["ip_nodir"] == "1":
                        return self.J(404, {"detail": "nf"})
                    if CTL["ip_dir_gone"] == "1":
                        return self.send(302, b"", "text/html", {"Location": "/api/channels/recordings/7/file/"})
                    n = ip_count()
                    host = CTL["ip_host"] or ("127.0.0.1:%d" % PORT)
                    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:1", "#EXT-X-MEDIA-SEQUENCE:0"]
                    for s in range(n):
                        lines.append("#EXTINF:%.3f," % (ctl_int("ip_seg_ms", 300) / 1000.0))
                        if CTL["ip_relative"] == "1":
                            lines.append("seg_%05d.ts" % s)
                        else:
                            lines.append("http://%s/api/channels/recordings/7/hls/seg_%05d.ts" % (host, s))
                    if CTL["ip_endlist"] == "1":
                        lines.append("#EXT-X-ENDLIST")
                    return self.send(200, ("\n".join(lines) + "\n").encode(), "application/vnd.apple.mpegurl")
                if len(rest) == 2 and rest[0] == "hls" and rest[1].startswith("seg_"):
                    if state == "deleted" or CTL["ip_dir_gone"] == "1":
                        return self.send(404, b"nf")
                    s = int(rest[1][4:9])
                    if s >= ip_count():
                        return self.send(404, b"nf")
                    if method == "HEAD":
                        hs = ctl_int("ip_head_status", 0)
                        if hs and (CTL["ip_head_seq"] == "" or ctl_int("ip_head_seq") == s):
                            return self.send(hs, b"", head=True)
                    else:
                        gs = ctl_int("ip_seg_status", 0)
                        if gs:
                            return self.send(gs, b"x")
                    # Dispatcharr's hls() ignores Range: always the whole file
                    if method == "HEAD":
                        self.send_response(200)
                        self.send_header("Content-Type", "video/mp2t")
                        self.send_header("Content-Length", str(seg_size(s)))
                        self.end_headers()
                        return True
                    return self.send(200, seg_bytes(0x1B000000, s, seg_size(s)), "video/mp2t", None, method == "HEAD")
            return self.J(404, {"detail": "nf"})
        try:
            jb = json.loads(b) if b else {}
        except ValueError:
            jb = {}
        if pvr_route(self, method, p, q, jb) is None:
            return self.J(404, {"detail": "nf"})

    def do_GET(self):
        self.handle_any("GET")

    def do_HEAD(self):
        self.handle_any("HEAD")

    def do_POST(self):
        self.handle_any("POST")

    def do_PATCH(self):
        self.handle_any("PATCH")

    def do_DELETE(self):
        self.handle_any("DELETE")


class Srv(ThreadingHTTPServer):
    request_queue_size = 512
    daemon_threads = True


srv = Srv(("127.0.0.1", PORT), H)
srv.serve_forever()
