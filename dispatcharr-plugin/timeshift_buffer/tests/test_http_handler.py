"""Tests for timeshift_buffer's file server (`_BufferRequestHandler`) over a real loopback socket.

This is the one network-facing surface in either plugin that needs no Dispatcharr
authentication -- the port is meant to be reachable from outside the container and
the per-buffer `?token=` is its only access control -- and `do_GET`/`do_HEAD`/
`_resolve_and_authorize` had no coverage at all (the 2026-10-04 hardening sweep's
missing-tests list). The Redis-touching functions the handler calls
(`_get_buffer_state`, `_update_buffer_state`) are monkeypatched; everything else is
the real code, driven with `http.client` against a real `ThreadingHTTPServer`.

Loaded by explicit file path under its own module name for the same reason
test_timeshift_buffer.py is (recording_edl's plugin.py shares the filename).
"""

import http.client
import importlib.util
import threading
from pathlib import Path

import pytest

_PLUGIN_PATH = Path(__file__).parent.parent / "plugin.py"
_spec = importlib.util.spec_from_file_location("timeshift_buffer_plugin_http", _PLUGIN_PATH)
plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(plugin)

_UUID = "11111111-2222-3333-4444-555555555555"
_TOKEN = "s3cret-token"
_SEGMENT = bytes(range(256)) * 4  # 1024 bytes, every byte value distinct per 256


class _Logger:
    def __init__(self):
        self.lines = []

    def _rec(self, fmt, args):
        self.lines.append(fmt % args if args else fmt)

    debug = info = warning = error = exception = lambda self, fmt, *args: self._rec(fmt, args)


@pytest.fixture()
def served(tmp_path, monkeypatch):
    """A running file server over tmp_path with one buffer's files in it, plus the recorded heartbeats."""
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    (channel_dir / "seg_00001.ts").write_bytes(_SEGMENT)
    (channel_dir / "live.m3u8").write_text("#EXTM3U\n")
    (tmp_path / "outside.txt").write_text("not under a channel directory")

    heartbeats = []
    monkeypatch.setattr(plugin, "_get_buffer_state", lambda uuid: {"access_token": _TOKEN} if uuid == _UUID else None)
    monkeypatch.setattr(plugin, "_update_buffer_state", lambda uuid, fn: heartbeats.append(uuid))

    logger = _Logger()
    server = plugin._BufferHTTPServer(("127.0.0.1", 0), plugin._BufferRequestHandler)
    server.storage_path = str(tmp_path)
    server.plugin_logger = logger
    server.daemon_threads = True
    thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.02), daemon=True)
    thread.start()
    try:
        yield types.SimpleNamespace(
            port=server.server_port, heartbeats=heartbeats, logger=logger, channel_dir=channel_dir, root=tmp_path
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


import types  # noqa: E402  (kept next to its only use in the fixture)


def _request(served, method, path, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", served.port, timeout=10)
    try:
        conn.request(method, path, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, dict(resp.getheaders()), resp.read()
    finally:
        conn.close()


def _url(name="seg_00001.ts", token=_TOKEN, uuid=_UUID):
    return f"/{uuid}/{name}" + (f"?token={token}" if token is not None else "")


def test_get_serves_the_whole_file_with_the_right_headers(served):
    status, headers, body = _request(served, "GET", _url())
    assert status == 200
    assert body == _SEGMENT
    assert headers["Content-Type"] == "video/mp2t"
    assert headers["Content-Length"] == str(len(_SEGMENT))
    assert headers["Accept-Ranges"] == "bytes"
    assert headers["Cache-Control"] == "no-store"
    assert served.heartbeats == [_UUID]


def test_get_serves_a_playlist_with_the_hls_content_type(served):
    status, headers, body = _request(served, "GET", _url("live.m3u8"))
    assert status == 200
    assert headers["Content-Type"] == "application/vnd.apple.mpegurl"
    assert body == b"#EXTM3U\n"


def test_a_missing_token_a_wrong_token_and_a_non_ascii_token_are_all_403(served):
    for path in (_url(token=None), _url(token="wrong"), _url(token="%C3%A9"), _url(token="")):
        status, _, _ = _request(served, "GET", path)
        assert status == 403, path
        status, _, _ = _request(served, "HEAD", path)
        assert status == 403, path
    # A refused request never counts as a viewer.
    assert served.heartbeats == []


def test_a_buffer_with_no_state_is_403_even_with_a_token(served):
    other = "99999999-2222-3333-4444-555555555555"
    (served.root / other).mkdir()
    (served.root / other / "seg_00001.ts").write_bytes(b"x")
    status, _, _ = _request(served, "GET", _url(uuid=other))
    assert status == 403


@pytest.mark.parametrize(
    "path",
    [
        f"/{_UUID}/../outside.txt?token={_TOKEN}",
        f"/{_UUID}/%2e%2e/outside.txt?token={_TOKEN}",
        f"/{_UUID}/seg_00001.ts%00?token={_TOKEN}",
        f"/{_UUID}/?token={_TOKEN}",
        f"/?token={_TOKEN}",
        f"/{_UUID}?token={_TOKEN}",
    ],
)
def test_paths_that_escape_or_do_not_name_a_file_are_404(served, path):
    status, _, body = _request(served, "GET", path)
    assert status == 404
    assert b"not under a channel directory" not in body
    status, _, _ = _request(served, "HEAD", path)
    assert status == 404


def test_a_file_that_does_not_exist_is_404_and_does_not_count_as_a_viewer(served):
    status, _, _ = _request(served, "GET", _url("seg_99999.ts"))
    assert status == 404
    assert served.heartbeats == []


def test_a_closed_range_returns_206_with_exactly_those_bytes(served):
    status, headers, body = _request(served, "GET", _url(), {"Range": "bytes=0-9"})
    assert status == 206
    assert body == _SEGMENT[0:10]
    assert headers["Content-Range"] == f"bytes 0-9/{len(_SEGMENT)}"
    assert headers["Content-Length"] == "10"


def test_a_single_byte_range_is_not_mistaken_for_unsatisfiable(served):
    # A (0, 0) result is falsy-looking; _parse_range used a sentinel object for this.
    status, headers, body = _request(served, "GET", _url(), {"Range": "bytes=0-0"})
    assert status == 206
    assert body == _SEGMENT[0:1]
    assert headers["Content-Range"] == f"bytes 0-0/{len(_SEGMENT)}"


def test_open_ended_and_suffix_ranges(served):
    status, headers, body = _request(served, "GET", _url(), {"Range": "bytes=1000-"})
    assert status == 206
    assert body == _SEGMENT[1000:]
    assert headers["Content-Range"] == f"bytes 1000-1023/{len(_SEGMENT)}"

    status, _, body = _request(served, "GET", _url(), {"Range": "bytes=-4"})
    assert status == 206
    assert body == _SEGMENT[-4:]

    # An end past the file is clamped, not refused.
    status, headers, body = _request(served, "GET", _url(), {"Range": "bytes=1020-99999"})
    assert status == 206
    assert body == _SEGMENT[1020:]
    assert headers["Content-Range"] == f"bytes 1020-1023/{len(_SEGMENT)}"


def test_a_range_starting_at_or_past_the_end_is_416_with_the_size(served):
    status, headers, _ = _request(served, "GET", _url(), {"Range": f"bytes={len(_SEGMENT)}-"})
    assert status == 416
    assert headers["Content-Range"] == f"bytes */{len(_SEGMENT)}"
    status, headers, _ = _request(served, "GET", _url(), {"Range": "bytes=500-100"})
    assert status == 416


def test_an_unparseable_range_falls_back_to_the_whole_file(served):
    for value in ("bytes=abc-def", "items=0-9", "bytes=-0", "bytes=5"):
        status, _, body = _request(served, "GET", _url(), {"Range": value})
        assert status == 200, value
        assert body == _SEGMENT, value


def test_head_reports_the_size_without_a_body(served):
    status, headers, body = _request(served, "HEAD", _url())
    assert status == 200
    assert body == b""
    assert headers["Content-Length"] == str(len(_SEGMENT))
    assert headers["Accept-Ranges"] == "bytes"
    assert served.heartbeats == [_UUID]


def test_a_file_removed_between_the_check_and_the_read_is_a_clean_404(served, monkeypatch):
    # ffmpeg's delete_segments removes a file the playlist still lists: the handler
    # saw it with is_file() and then open() (GET) / stat() (HEAD) lost the race. is_file() is
    # patched to still say yes while open()/stat() already say no (Path.resolve() also
    # stats, and tolerates a missing file).
    real_stat = Path.stat
    real_open = Path.open
    real_is_file = Path.is_file

    def vanishing_stat(self, *args, **kwargs):
        if self.name == "seg_00001.ts":
            raise FileNotFoundError(self)
        return real_stat(self, *args, **kwargs)

    def vanishing_open(self, *args, **kwargs):
        if self.name == "seg_00001.ts":
            raise FileNotFoundError(self)
        return real_open(self, *args, **kwargs)

    def still_a_file(self):
        return True if self.name == "seg_00001.ts" else real_is_file(self)

    monkeypatch.setattr(Path, "stat", vanishing_stat)
    monkeypatch.setattr(Path, "open", vanishing_open)
    monkeypatch.setattr(Path, "is_file", still_a_file)
    status, _, _ = _request(served, "GET", _url())
    assert status == 404
    status, _, _ = _request(served, "HEAD", _url())
    assert status == 404
    assert served.heartbeats == []


def test_the_content_length_describes_the_file_that_was_opened_not_an_earlier_stat(served, monkeypatch):
    # The muxer replaces live.m3u8 by renaming a temp file over it. A stat of the path before the open
    # could describe the OLD file while the open read the NEW one: Content-Length of one, body of the
    # other (a truncated playlist). The size now comes from the open file, so a stale stat is ignored.
    playlist = served.channel_dir / "live.m3u8"
    playlist.write_text("#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:2.0,\nseg_00001.ts\n#EXTINF:2.0,\nseg_00002.ts\n")
    real_stat = Path.stat

    def stale_stat(self, *args, **kwargs):
        if self.name == "live.m3u8":
            result = real_stat(self, *args, **kwargs)
            return type("S", (), {"st_size": 10, "st_mode": result.st_mode, "st_mtime": result.st_mtime})()
        return real_stat(self, *args, **kwargs)

    monkeypatch.setattr(Path, "stat", stale_stat)
    status, headers, body = _request(served, "GET", _url("live.m3u8"))
    assert status == 200
    assert body == playlist.read_bytes()
    assert int(headers["Content-Length"]) == len(body)


def test_the_token_never_reaches_the_log(served):
    _request(served, "GET", _url())
    _request(served, "GET", _url(token="wrong"))
    logged = "\n".join(served.logger.lines)
    assert "timeshift_buffer http:" in logged
    assert _TOKEN not in logged
    assert "wrong" not in logged
    assert "token=REDACTED" in logged


def test_a_heartbeat_failure_does_not_fail_the_response(served, monkeypatch):
    def boom(uuid, fn):
        raise RuntimeError("redis is down")

    monkeypatch.setattr(plugin, "_update_buffer_state", boom)
    status, _, body = _request(served, "GET", _url())
    assert status == 200
    assert body == _SEGMENT
    assert _wait_until(lambda: any("heartbeat-on-fetch failed" in line for line in served.logger.lines))


# ---------------------------------------------------------------------
# Slow and idle connections: the absolute request deadline and the connection cap
# (timeshift_buffer 0.8.1). Found and reproduced against the real plugin on 2026-10-04: a
# silent connection was dropped after the 60 s socket timeout, but one dripping a byte every
# few seconds was still open after 150 s and holds a thread and a descriptor for as long as
# it likes, before any token check.
# ---------------------------------------------------------------------

import socket  # noqa: E402
import time  # noqa: E402


def _wait_until(predicate, seconds=5):
    """True once `predicate()` holds. The server closes a refused connection before it logs the refusal (and its
    handler threads finish after the client has its response), so a test that reads the log or a counter right after
    its own socket call is racing that thread: this failed once on CI (2026-10-07, the first run of the release tag)
    though the same commit had passed a run earlier."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


def _closed_within(sock, seconds):
    """True once the server has closed `sock` (recv returns b'' or the connection resets)."""
    deadline = time.monotonic() + seconds
    sock.settimeout(0.05)
    while time.monotonic() < deadline:
        try:
            if sock.recv(1) == b"":
                return True
        except socket.timeout:
            pass
        except OSError:
            return True
    return False


def test_a_client_dripping_the_request_a_byte_at_a_time_is_cut_off_at_the_deadline(served, monkeypatch):
    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 0.5)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        sent = 0
        start = time.monotonic()
        request = b"GET /" + b"a" * 200
        closed = False
        while time.monotonic() - start < 4 and not closed:
            try:
                sock.send(request[sent : sent + 1])
                sent += 1
            except OSError:
                closed = True
                break
            time.sleep(0.1)
            closed = _closed_within(sock, 0.01)
        elapsed = time.monotonic() - start
        assert closed, "the dripping connection was still open after 4 s"
        # Cut off near the 0.5 s deadline -- not after the 60 s socket timeout, and not only
        # once the whole request line has arrived (200+ bytes at 10 bytes a second would take 20 s).
        assert elapsed < 3
        assert sent < 40
    finally:
        sock.close()


def test_a_connection_that_sends_nothing_is_cut_off_at_the_deadline_too(served, monkeypatch):
    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 0.5)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        assert _closed_within(sock, 3)
    finally:
        sock.close()


def test_a_request_that_arrives_in_pieces_within_the_deadline_is_still_served(served, monkeypatch):
    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 3)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        request = f"GET {_url()} HTTP/1.0\r\nHost: x\r\n\r\n".encode()
        for i in range(0, len(request), 12):
            sock.sendall(request[i : i + 12])
            time.sleep(0.05)
        data = b""
        sock.settimeout(5)
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        assert data.startswith(b"HTTP/1.0 200")
        assert data.endswith(_SEGMENT)
    finally:
        sock.close()


def test_the_deadline_applies_to_the_headers_only_not_to_a_long_response(served, monkeypatch):
    # A viewer that reads a response slowly is bounded by the per-operation socket timeout, not by
    # what was left of the request deadline: sendall()'s timeout covers the whole call, and the
    # reader had shrunk the socket's timeout to that remainder (0.8.1 cut a 32 MiB body off after
    # about 2.6 MiB). The body must be larger than the loopback socket buffers for the send to
    # block at all, so a 1 KB file proves nothing.
    big = bytes(range(256)) * (16 * 4096)  # 16 MiB
    (served.channel_dir / "seg_99999.ts").write_bytes(big)
    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 0.3)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        sock.sendall(f"GET {_url('seg_99999.ts')} HTTP/1.0\r\n\r\n".encode())
        time.sleep(1.5)  # well past the deadline before reading anything
        received = bytearray()
        sock.settimeout(10)
        while True:
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            received += chunk
        header_end = received.index(b"\r\n\r\n") + 4
        assert bytes(received[:12]) == b"HTTP/1.0 200"
        assert len(received) - header_end == len(big)
    finally:
        sock.close()


def test_connections_beyond_the_cap_are_closed_at_once_and_slots_come_back(served, monkeypatch):
    class _Small(plugin._BufferHTTPServer):
        max_connections = 2

    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 30)
    server = _Small(("127.0.0.1", 0), plugin._BufferRequestHandler)
    server.storage_path = str(served.root)
    server.plugin_logger = served.logger
    server.daemon_threads = True
    thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.02), daemon=True)
    thread.start()
    held = []
    try:
        for _ in range(2):
            held.append(socket.create_connection(("127.0.0.1", server.server_port), timeout=5))
        time.sleep(0.2)  # let the server take both slots
        third = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
        try:
            assert _closed_within(third, 2), "a connection past the cap was not closed"
        finally:
            third.close()
        assert _wait_until(lambda: any("refused 1 connection(s)" in line for line in served.logger.lines))

        # Closing a held connection frees its slot (the handler thread sees EOF and finishes).
        held.pop().close()
        deadline = time.monotonic() + 5
        served_ok = False
        while time.monotonic() < deadline and not served_ok:
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
            try:
                conn.request("GET", _url())
                served_ok = conn.getresponse().status == 200
            except OSError:
                pass
            finally:
                conn.close()
            if not served_ok:
                time.sleep(0.1)
        assert served_ok, "no slot came back after a held connection closed"
    finally:
        for s in held:
            s.close()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def _serve(served, server_class, monkeypatch=None):
    server = server_class(("127.0.0.1", 0), plugin._BufferRequestHandler)
    server.storage_path = str(served.root)
    server.plugin_logger = served.logger
    server.daemon_threads = True
    thread = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.02), daemon=True)
    thread.start()
    return server, thread


def _stop(server, thread):
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)


def test_one_client_cannot_hold_more_than_its_share_of_the_slots(served, monkeypatch):
    class _Small(plugin._BufferHTTPServer):
        max_connections = 10
        max_connections_per_client = 3

    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 30)
    server, thread = _serve(served, _Small)
    held = []
    try:
        for _ in range(3):
            held.append(socket.create_connection(("127.0.0.1", server.server_port), timeout=5))
        time.sleep(0.2)
        extra = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
        try:
            assert _closed_within(extra, 2), "a connection past the per-client share was not closed"
        finally:
            extra.close()
        assert _wait_until(lambda: any("this client already has 3 open" in line for line in served.logger.lines))
        # The refusal gave back the total slot it never used: 7 remain, so closing one held
        # connection lets this client in again.
        assert server._connection_slots._value == 10 - 3
        held.pop().close()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and server._client_counts.get("127.0.0.1", 0) >= 3:
            time.sleep(0.05)
        again = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
        try:
            assert not _closed_within(again, 0.5)
        finally:
            again.close()
    finally:
        for sock in held:
            sock.close()
        _stop(server, thread)


def test_refusals_are_logged_at_most_once_per_interval(served, monkeypatch):
    class _One(plugin._BufferHTTPServer):
        max_connections = 1
        refusal_log_interval_seconds = 60

    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 30)
    server, thread = _serve(served, _One)
    first = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
    try:
        time.sleep(0.2)
        for _ in range(8):
            extra = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
            _closed_within(extra, 1)
            extra.close()
        assert _wait_until(lambda: server._refusals_since_log == 7)
        lines = [line for line in served.logger.lines if "refused" in line]
        assert len(lines) == 1
    finally:
        first.close()
        _stop(server, thread)


def test_a_failed_thread_start_gives_both_slots_back(served, monkeypatch):
    # ThreadingMixIn.process_request() starts a thread per connection; a start that fails (the
    # process is out of threads) must not leak the slot it was handed, or a few failures would
    # lock every client out for good.
    server = plugin._BufferHTTPServer(("127.0.0.1", 0), plugin._BufferRequestHandler)
    try:
        real_start = threading.Thread.start
        calls = {"n": 0}

        def failing_start(self):
            calls["n"] += 1
            if calls["n"] == 1:
                raise RuntimeError("can't start new thread")
            return real_start(self)

        monkeypatch.setattr(threading.Thread, "start", failing_start)
        a, b = socket.socketpair()
        try:
            with pytest.raises(RuntimeError):
                server.process_request(a, ("client-a", 5555))
        finally:
            a.close()
            b.close()
        assert server._connection_slots._value == server.max_connections
        assert server._client_counts == {}
    finally:
        server.server_close()


def test_only_the_playlist_and_segments_are_served_not_the_other_files_in_the_directory(served):
    # ffmpeg.log (unbounded) and ffmpeg.owner.json sit in the channel directory; a token holder had
    # no business reading them, nor asking for a large log many times at once.
    (served.channel_dir / "ffmpeg.log").write_bytes(b"log")
    (served.channel_dir / "ffmpeg.owner.json").write_text("{}")
    (served.channel_dir / "notes.txt").write_text("x")
    for name in ("ffmpeg.log", "ffmpeg.owner.json", "notes.txt", "seg_00001.ts.bak", "seg_.ts", "xlive.m3u8"):
        status, _, _ = _request(served, "GET", _url(name))
        assert status == 404, name
        status, _, _ = _request(served, "HEAD", _url(name))
        assert status == 404, name
    for name in ("live.m3u8", "seg_00001.ts"):
        assert _request(served, "GET", _url(name))[0] == 200


def test_a_range_request_is_streamed_from_the_right_offset(served):
    status, headers, body = _request(served, "GET", _url(), headers={"Range": "bytes=300-699"})
    assert status == 206
    assert body == _SEGMENT[300:700]
    assert headers["Content-Range"] == f"bytes 300-699/{len(_SEGMENT)}"
    assert headers["Content-Length"] == "400"


def test_a_global_refusal_does_not_leak_the_per_client_slot(served, monkeypatch):
    # With the global cap hit, the refusal must give back the per-client slot it just took, or each
    # refusal would leak one and a NAT or proxy address would end up locked out for good.
    class _Tiny(plugin._BufferHTTPServer):
        max_connections = 1
        max_connections_per_client = 2

    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 30)
    server, thread = _serve(served, _Tiny)
    held = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
    try:
        time.sleep(0.2)
        for _ in range(2):
            extra = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
            assert _closed_within(extra, 2)
            extra.close()
        assert server._client_counts == {"127.0.0.1": 1}
        held.close()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and server._client_counts:
            time.sleep(0.05)
        again = socket.create_connection(("127.0.0.1", server.server_port), timeout=5)
        try:
            assert not _closed_within(again, 0.5)
        finally:
            again.close()
    finally:
        held.close()
        _stop(server, thread)


def test_a_client_draining_the_body_slowly_is_cut_off_at_the_body_deadline(served, monkeypatch):
    # Chunked sends made the 60 s bound per 256 KiB chunk: a client taking nearly that long per chunk
    # held the connection for size/256 KiB times the timeout. The body now has one absolute deadline:
    # the per-operation timeout plus the time the whole body needs at the floor rate.
    big = bytes(range(256)) * (16 * 4096)  # 16 MiB
    (served.channel_dir / "seg_88888.ts").write_bytes(big)
    monkeypatch.setattr(plugin._BufferRequestHandler, "timeout", 1)
    monkeypatch.setattr(plugin._BufferRequestHandler, "body_min_rate_bytes_per_second", 8 * 1024 * 1024)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=10)
    try:
        sock.sendall(f"GET {_url('seg_88888.ts')} HTTP/1.0\r\n\r\n".encode())
        received = 0
        start = time.monotonic()
        sock.settimeout(10)
        while time.monotonic() - start < 20:
            time.sleep(0.3)  # about 0.8 MB/s: far under the 8 MiB/s floor
            try:
                chunk = sock.recv(256 * 1024)
            except OSError:
                break
            if not chunk:
                break
            received += len(chunk)
        elapsed = time.monotonic() - start
        assert received < len(big), "the slow drain was served to the end"
        assert elapsed < 15, "the body deadline did not cut the connection off"
    finally:
        sock.close()


def test_a_client_reading_steadily_but_under_the_floor_rate_is_cut_off_by_the_absolute_deadline(served, monkeypatch):
    # The two tests around this one are ended by the per-write socket timeout (the client stalls), so removing the
    # absolute deadline altogether left them green. This client never stalls: every write succeeds well inside the
    # timeout, and only the deadline (timeout + size / floor rate = 2 + 16 MiB / 32 MiB/s = 2.5 s) ends it, while
    # the body at its own pace (about 2.5 MiB/s) would take over six seconds.
    big = bytes(range(256)) * (16 * 4096)  # 16 MiB
    (served.channel_dir / "seg_66666.ts").write_bytes(big)
    monkeypatch.setattr(plugin._BufferRequestHandler, "timeout", 2)
    monkeypatch.setattr(plugin._BufferRequestHandler, "body_min_rate_bytes_per_second", 32 * 1024 * 1024)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=10)
    try:
        sock.sendall(f"GET {_url('seg_66666.ts')} HTTP/1.0\r\n\r\n".encode())
        received = 0
        start = time.monotonic()
        sock.settimeout(10)
        while time.monotonic() - start < 12:
            try:
                chunk = sock.recv(256 * 1024)
            except OSError:
                break
            if not chunk:
                break
            received += len(chunk)
            time.sleep(0.1)
        elapsed = time.monotonic() - start
        assert received < len(big), "the body was served to the end past its deadline"
        assert elapsed < 5.5, f"the connection lasted {elapsed:.1f} s; the deadline is 2.5 s"
    finally:
        sock.close()


def test_one_stalled_write_cannot_overrun_the_body_deadline_by_a_whole_timeout(served, monkeypatch):
    # Each chunk's socket timeout is what remains of the body deadline, not the full per-operation
    # timeout: a client that stops reading just before the deadline must cost the server its
    # thread at the deadline, not up to `timeout` seconds later. Observed through the per-client
    # slot, which the handler thread gives back when it ends.
    big = bytes(range(256)) * (16 * 4096)  # 16 MiB
    (served.channel_dir / "seg_77777.ts").write_bytes(big)
    monkeypatch.setattr(plugin._BufferRequestHandler, "timeout", 3)
    monkeypatch.setattr(plugin._BufferRequestHandler, "body_min_rate_bytes_per_second", 16 * 1024 * 1024)
    server, thread = _serve(served, plugin._BufferHTTPServer)
    # deadline = 3 + 16 MiB / 16 MiB/s = 4 s after the headers
    sock = socket.create_connection(("127.0.0.1", server.server_port), timeout=10)
    try:
        sock.sendall(f"GET {_url('seg_77777.ts')} HTTP/1.0\r\n\r\n".encode())
        start = time.monotonic()
        sock.settimeout(10)
        while time.monotonic() - start < 3.5:  # drain slowly, keeping the server's writes succeeding
            time.sleep(0.3)
            sock.recv(256 * 1024)
        # Stop reading. The server is now blocked in a write; with the per-chunk clamp it gives up at
        # the 4 s deadline (about 0.5 s from now), without it only after the full 3 s timeout (6.5 s).
        released_at = None
        while time.monotonic() - start < 8:
            if not server._client_counts:
                released_at = time.monotonic() - start
                break
            time.sleep(0.1)
        assert released_at is not None, "the handler thread never ended"
        assert released_at < 5.5, f"the stalled write ran {released_at - 4:.1f} s past the body deadline"
    finally:
        sock.close()
        _stop(server, thread)


def test_a_fetch_stamps_the_buffer_heartbeat_with_the_shared_clock(served, monkeypatch):
    """Every served file counts as a viewer still watching, and that stamp is read by the reaper in another worker:
    it is the shared (Redis) clock, whatever this host's own clock says."""
    stamped = {}

    class _Redis:
        def time(self):
            return (7_000, 250_000)

    monkeypatch.setattr(plugin, "_redis", lambda: _Redis())
    monkeypatch.setattr(plugin.time, "time", lambda: 1.0)
    monkeypatch.setattr(plugin, "_update_buffer_state", lambda uuid, fn: stamped.update(fn({"viewers": []})))

    status, _headers, _body = _request(served, "GET", _url())

    assert status == 200
    assert stamped["last_heartbeat"] == 7000.25


# ---------------------------------------------------------------------
# The request header budget and the per-client key (the 16th hardening sweep). The deadline bounds how long a
# request may take to arrive, not how much: http.server accepts a 64 KiB request line and 100 header lines of 64 KiB
# each, about 6.5 MB held in memory per connection before any token check (64 connections measured 418 MB).
# ---------------------------------------------------------------------


def _request_of_size(total_bytes):
    """A GET for the buffer's segment whose request line and headers are exactly `total_bytes` long."""
    head = f"GET {_url()} HTTP/1.0\r\nX-Pad: ".encode()
    tail = b"\r\n\r\n"
    padding = total_bytes - len(head) - len(tail)
    assert padding > 0
    return head + b"a" * padding + tail


def _raw_exchange(served, payload):
    """Sends `payload`, returns (status line or None when the server just closed the connection)."""
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        sock.sendall(payload)
        data = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\r\n" in data:
                break
        return data.split(b"\r\n", 1)[0].decode() if data else None
    except (ConnectionResetError, BrokenPipeError):
        return None
    finally:
        sock.close()


def test_a_request_within_the_header_budget_is_served(served):
    budget = plugin._BufferRequestHandler.request_header_budget_bytes
    assert budget == 16 * 1024
    assert _raw_exchange(served, _request_of_size(budget)) == "HTTP/1.0 200 OK"
    assert _raw_exchange(served, _request_of_size(2048)) == "HTTP/1.0 200 OK"


def test_a_request_past_the_header_budget_is_dropped_without_a_response(served):
    budget = plugin._BufferRequestHandler.request_header_budget_bytes
    assert _raw_exchange(served, _request_of_size(budget + 1)) is None
    assert _raw_exchange(served, _request_of_size(100 * 1024)) is None


def test_headers_sent_endlessly_without_the_closing_blank_line_are_cut_off_by_size_not_time(served, monkeypatch):
    # A long deadline, so what ends this is the byte budget and not the clock.
    monkeypatch.setattr(plugin._BufferRequestHandler, "request_deadline_seconds", 60)
    sock = socket.create_connection(("127.0.0.1", served.port), timeout=5)
    try:
        sock.sendall(f"GET {_url()} HTTP/1.1\r\n".encode())
        line = b"X-Pad: " + b"a" * 4000 + b"\r\n"
        started = time.monotonic()
        closed = False
        for _ in range(100):
            try:
                sock.sendall(line)
            except (BrokenPipeError, ConnectionResetError):
                closed = True
                break
            if _closed_within(sock, 0.01):
                closed = True
                break
        assert closed, "the server kept reading headers past any real client's size"
        assert time.monotonic() - started < 10
    finally:
        sock.close()


def test_the_budget_does_not_limit_the_response(served):
    # The budget covers what the client sends while its request line and headers are pending: a response larger than
    # the budget is not a request, and is served whole.
    big = bytes(range(256)) * (4 * 256)  # 256 KiB, sixteen times the budget
    (served.channel_dir / "seg_55555.ts").write_bytes(big)
    status, _headers, body = _request(served, "GET", _url("seg_55555.ts"))
    assert status == 200 and body == big


@pytest.mark.parametrize(
    ("address", "expected"),
    [
        ("192.0.2.7", "192.0.2.7"),
        ("::ffff:192.0.2.7", "192.0.2.7"),  # an IPv4 client on the dual-stack socket
        ("2001:db8:1:2:aaaa:bbbb:cccc:dddd", "2001:db8:1:2::/64"),
        ("2001:db8:1:2::1", "2001:db8:1:2::/64"),
        ("fe80::1%eth0", "fe80::/64"),  # a zone id is not part of the address
        ("::1", "::/64"),
        ("not-an-address", "not-an-address"),
        ("", None),
        (None, None),
    ],
)
def test_the_per_client_key_groups_an_ipv6_host_by_its_64(address, expected):
    assert plugin._client_key(address) == expected


def test_two_ipv6_addresses_of_one_64_are_one_client_and_two_64s_are_two():
    assert plugin._client_key("2001:db8:1:2::1") == plugin._client_key("2001:db8:1:2:ffff:ffff:ffff:ffff")
    assert plugin._client_key("2001:db8:1:2::1") != plugin._client_key("2001:db8:1:3::1")
    assert plugin._client_key("192.0.2.7") != plugin._client_key("192.0.2.8")


class _FakeConnection:
    def __init__(self):
        self.closed = False

    def shutdown(self, how):
        self.closed = True

    def close(self):
        self.closed = True


def test_the_connection_cap_counts_an_ipv6_host_by_its_64(monkeypatch):
    release = threading.Event()

    class _Blocking:
        def __init__(self, request, client_address, server):
            release.wait(10)

    class _OnePerClient(plugin._BufferHTTPServer):
        max_connections_per_client = 1

    server = _OnePerClient(("127.0.0.1", 0), _Blocking)
    server.daemon_threads = True
    try:
        first, second, other_net = _FakeConnection(), _FakeConnection(), _FakeConnection()
        server.process_request(first, ("2001:db8:1:2::1", 40000))
        # A different address in the same /64 is the same client, so it is turned away...
        server.process_request(second, ("2001:db8:1:2::2", 40001))
        assert second.closed and not first.closed
        # ...and another /64 is not.
        server.process_request(other_net, ("2001:db8:1:3::1", 40002))
        assert not other_net.closed
        assert len(server._client_counts) == 2
    finally:
        release.set()
        server.server_close()
    deadline = time.monotonic() + 5
    while server._client_counts and time.monotonic() < deadline:
        time.sleep(0.05)  # the handler threads give their slots back as they finish
    assert server._client_counts == {}  # every slot came back under the same key it was taken with
