"""Unit tests for dispatcharr-plugin/timeshift_buffer/plugin.py.

Loaded by explicit file path with a unique synthetic module name (not a
bare `import plugin`) since recording_edl's own plugin.py shares the same
filename -- importing both by name in one pytest session would otherwise
collide via sys.modules.

Covers only logic with no Redis/Django/real-HTTP-socket dependency --
Redis-backed state (_redis/_get_buffer_state/etc.), the actual HTTP
server, and ffmpeg subprocess management stay untested here, same
boundary this project draws elsewhere (see recording_edl's own tests and
docs/OPEN_ITEMS.md's "No automated test suite exists" entry). Functions
that only had a Redis/Django dependency in *part* of their logic
(_find_orphaned_channel_dirs/_scrub_orphaned_dirs, _stream_attribution_headers)
are still tested by monkeypatching just that one call, or by only
exercising the code path that never touches it.
"""

import importlib.util
import os
import time
from pathlib import Path

import pytest

_PLUGIN_PATH = Path(__file__).parent.parent / "plugin.py"
_spec = importlib.util.spec_from_file_location("timeshift_buffer_plugin", _PLUGIN_PATH)
timeshift_buffer_plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(timeshift_buffer_plugin)


class _FakeLogger:
    """Minimal stand-in for the plugin `context["logger"]` -- records
    calls rather than actually logging anything."""

    def __init__(self):
        self.calls = []

    def _record(self, level, fmt, args):
        self.calls.append((level, fmt % args if args else fmt))

    def debug(self, fmt, *args):
        self._record("debug", fmt, args)

    def info(self, fmt, *args):
        self._record("info", fmt, args)

    def warning(self, fmt, *args):
        self._record("warning", fmt, args)

    def error(self, fmt, *args):
        self._record("error", fmt, args)

    def exception(self, fmt, *args):
        self._record("exception", fmt, args)


@pytest.fixture(autouse=True)
def _clear_manifest_cache():
    """_manifest_cache is real module-global mutable state -- clear it
    around every test so one test's cache entries can't leak into
    another's."""
    timeshift_buffer_plugin._manifest_cache.clear()
    yield
    timeshift_buffer_plugin._manifest_cache.clear()


_UUID = "11111111-1111-1111-1111-111111111111"


# ---------------------------------------------------------------------
# _channel_dir
# ---------------------------------------------------------------------


def test_channel_dir_valid_uuid():
    d = timeshift_buffer_plugin._channel_dir("/data/timeshift", _UUID)
    assert d == Path("/data/timeshift") / _UUID


def test_channel_dir_rejects_traversal_attempt():
    """The exact security-relevant case this validation exists for: a
    non-UUID channel_uuid (e.g. path traversal) must be rejected before
    it's ever used to build a filesystem path a later mkdir/rmtree would
    act on."""
    with pytest.raises(ValueError):
        timeshift_buffer_plugin._channel_dir("/data/timeshift", "../../etc")


def test_channel_dir_rejects_non_uuid_string():
    with pytest.raises(ValueError):
        timeshift_buffer_plugin._channel_dir("/data/timeshift", "not-a-uuid")


# ---------------------------------------------------------------------
# _resolve_request_path
# ---------------------------------------------------------------------


def test_resolve_request_path_valid():
    channel_uuid, path = timeshift_buffer_plugin._resolve_request_path(
        f"/{_UUID}/live.m3u8?token=abc", "/data/timeshift"
    )
    assert channel_uuid == _UUID
    assert path == Path("/data/timeshift") / _UUID / "live.m3u8"


def test_resolve_request_path_rejects_dotdot_component():
    channel_uuid, path = timeshift_buffer_plugin._resolve_request_path(f"/{_UUID}/../../etc/passwd", "/data/timeshift")
    assert (channel_uuid, path) == (None, None)


def test_resolve_request_path_rejects_missing_filename():
    channel_uuid, path = timeshift_buffer_plugin._resolve_request_path(f"/{_UUID}", "/data/timeshift")
    assert (channel_uuid, path) == (None, None)


def test_resolve_request_path_rejects_symlink_escape(tmp_path):
    """A symlink inside storage_path that resolves outside it must still
    be rejected even though the raw request string has no literal ".."
    component -- this is exactly why the function re-checks via
    Path.resolve() rather than trusting the string-level ".." guard
    alone."""
    storage = tmp_path / "storage"
    storage.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / "secret.txt").write_text("secret")
    (storage / "escape").symlink_to(outside)

    channel_uuid, path = timeshift_buffer_plugin._resolve_request_path("/escape/secret.txt", str(storage))
    assert (channel_uuid, path) == (None, None)


# ---------------------------------------------------------------------
# _BufferRequestHandler._parse_range (static method, no instance needed)
# ---------------------------------------------------------------------

_parse_range = timeshift_buffer_plugin._BufferRequestHandler._parse_range


def test_parse_range_absent_header():
    assert _parse_range(None, 1000) is None
    assert _parse_range("", 1000) is None


def test_parse_range_not_bytes_unit():
    assert _parse_range("items=0-10", 1000) is None


def test_parse_range_explicit_start_end():
    assert _parse_range("bytes=10-20", 1000) == (10, 20)


def test_parse_range_open_ended():
    assert _parse_range("bytes=990-", 1000) == (990, 999)


def test_parse_range_suffix_range():
    # "last 100 bytes" of a 1000-byte file.
    assert _parse_range("bytes=-100", 1000) == (900, 999)


def test_parse_range_suffix_range_larger_than_file():
    assert _parse_range("bytes=-5000", 1000) == (0, 999)


def test_parse_range_end_clamped_to_file_size():
    assert _parse_range("bytes=0-99999", 1000) == (0, 999)


def test_parse_range_unsatisfiable_start_past_eof():
    assert _parse_range("bytes=1000-2000", 1000) == (False, False)


def test_parse_range_unsatisfiable_end_before_start():
    assert _parse_range("bytes=50-10", 1000) == (False, False)


def test_parse_range_unsatisfiable_zero_suffix():
    assert _parse_range("bytes=-0", 1000) is None


def test_parse_range_garbage_values():
    assert _parse_range("bytes=abc-def", 1000) is None


def test_parse_range_only_first_of_multirange():
    # Multi-range unsupported by design -- only the first range is honored.
    assert _parse_range("bytes=10-20,30-40", 1000) == (10, 20)


# ---------------------------------------------------------------------
# _proxy_url
# ---------------------------------------------------------------------


def test_proxy_url_strips_trailing_slash():
    assert (
        timeshift_buffer_plugin._proxy_url(_UUID, "http://127.0.0.1:9191/")
        == f"http://127.0.0.1:9191/proxy/ts/stream/{_UUID}"
    )


def test_proxy_url_no_trailing_slash():
    assert (
        timeshift_buffer_plugin._proxy_url(_UUID, "http://127.0.0.1:9191")
        == f"http://127.0.0.1:9191/proxy/ts/stream/{_UUID}"
    )


# ---------------------------------------------------------------------
# _compute_segment_counts
# ---------------------------------------------------------------------


def test_compute_segment_counts_typical_values():
    visible, wrap = timeshift_buffer_plugin._compute_segment_counts(buffer_minutes=60, segment_seconds=2)

    assert visible == 1800
    assert wrap == 3600


def test_compute_segment_counts_invariant_matches_buffer_window():
    """The exact invariant docs/TIMESHIFT.md cites to rule out a
    suspected ~89s audio-sync-error correlation: visible_segments *
    segment_seconds always reduces to the full buffer window in
    seconds, for any segment_seconds that evenly divides it."""
    for buffer_minutes, segment_seconds in [(60, 2), (5, 1), (300, 6), (1, 1)]:
        visible, _ = timeshift_buffer_plugin._compute_segment_counts(buffer_minutes, segment_seconds)
        assert visible * segment_seconds == buffer_minutes * 60


def test_compute_segment_counts_wrap_is_double_visible():
    visible, wrap = timeshift_buffer_plugin._compute_segment_counts(buffer_minutes=10, segment_seconds=5)

    assert wrap == visible * 2


def test_compute_segment_counts_floors_at_one_when_segment_longer_than_buffer():
    """A segment_seconds larger than the whole configured buffer window
    must still produce a playable single-segment playlist, not 0."""
    visible, wrap = timeshift_buffer_plugin._compute_segment_counts(buffer_minutes=1, segment_seconds=120)

    assert visible == 1
    assert wrap == 2


# ---------------------------------------------------------------------
# _stream_attribution_headers -- client_ip path only. The username/JWT
# branch imports Django (deferred inside `if username:`) -- deliberately
# never exercised here, so no test in this file passes a non-empty
# "username" param.
# ---------------------------------------------------------------------


def test_stream_attribution_headers_no_params_returns_none():
    logger = _FakeLogger()
    assert timeshift_buffer_plugin._stream_attribution_headers({}, logger) is None


def test_stream_attribution_headers_valid_client_ip():
    logger = _FakeLogger()
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": "192.168.1.42"}, logger)
    assert result == "X-Real-IP: 192.168.1.42\r\n"


def test_stream_attribution_headers_rejects_invalid_ip():
    logger = _FakeLogger()
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": "not-an-ip"}, logger)
    assert result is None
    assert any(level == "warning" for level, _msg in logger.calls)


def test_stream_attribution_headers_rejects_header_injection_attempt():
    """The exact documented security fix (docs/TIMESHIFT.md's "client_ip
    header injection" section): an unvalidated client_ip let a caller
    smuggle a second, pipelined request onto ffmpeg's own connection.
    ipaddress.ip_address() rejects anything that isn't a bare IP,
    including embedded CRLF-style injection attempts."""
    logger = _FakeLogger()
    malicious = "1.2.3.4\r\nX-Injected: evil"
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": malicious}, logger)
    assert result is None


# ---------------------------------------------------------------------
# _prune_stale_viewers
# ---------------------------------------------------------------------


def test_prune_stale_viewers_no_viewers():
    state = {}
    assert timeshift_buffer_plugin._prune_stale_viewers(state, idle_timeout=60, now=1000) is False


def test_prune_stale_viewers_all_fresh():
    state = {"viewers": ["a", "b"], "viewer_heartbeats": {"a": 990, "b": 995}}
    changed = timeshift_buffer_plugin._prune_stale_viewers(state, idle_timeout=60, now=1000)
    assert changed is False
    assert state["viewers"] == ["a", "b"]


def test_prune_stale_viewers_drops_stale_one():
    state = {"viewers": ["a", "b"], "viewer_heartbeats": {"a": 990, "b": 500}}
    changed = timeshift_buffer_plugin._prune_stale_viewers(state, idle_timeout=60, now=1000)
    assert changed is True
    assert state["viewers"] == ["a"]
    assert state["viewer_heartbeats"] == {"a": 990}


def test_prune_stale_viewers_no_heartbeat_yet_treated_as_fresh():
    """A viewer with no recorded heartbeat (older plugin-version state,
    or a start_buffer call that raced this exact instant) must not be
    mass-pruned just because it hasn't reported in yet."""
    state = {"viewers": ["a"], "viewer_heartbeats": {}}
    changed = timeshift_buffer_plugin._prune_stale_viewers(state, idle_timeout=60, now=1000)
    assert changed is False
    assert state["viewers"] == ["a"]


# ---------------------------------------------------------------------
# _is_process_alive -- monkeypatch just os.killpg (its one external
# dependency), same technique used elsewhere for a single Redis/Django
# call.
# ---------------------------------------------------------------------


def test_is_process_alive_false_for_falsy_pid():
    assert timeshift_buffer_plugin._is_process_alive(None) is False
    assert timeshift_buffer_plugin._is_process_alive(0) is False


def test_is_process_alive_true_when_killpg_succeeds(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    assert timeshift_buffer_plugin._is_process_alive(1234) is True


def test_is_process_alive_false_when_process_lookup_error(monkeypatch):
    def raise_lookup_error(pid, sig):
        raise ProcessLookupError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", raise_lookup_error)
    assert timeshift_buffer_plugin._is_process_alive(1234) is False


def test_is_process_alive_true_on_other_errors_conservatively(monkeypatch):
    """Documented deliberate choice (see the function's own docstring):
    e.g. a PermissionError against a recycled, unrelated pid should
    assume the process is still alive rather than risk reaping something
    still running."""

    def raise_permission_error(pid, sig):
        raise PermissionError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", raise_permission_error)
    assert timeshift_buffer_plugin._is_process_alive(1234) is True


# ---------------------------------------------------------------------
# _find_orphaned_channel_dirs / _scrub_orphaned_dirs -- monkeypatch just
# _get_buffer_state (the one Redis-dependent call), exercise the real
# filesystem scan/removal logic against tmp_path.
# ---------------------------------------------------------------------


def test_find_orphaned_channel_dirs_skips_tracked_buffers(tmp_path, monkeypatch):
    tracked = tmp_path / _UUID
    tracked.mkdir()
    old_mtime = time.time() - 3600
    os.utime(tracked, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"channel_uuid": uuid})

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert orphans == []


def test_find_orphaned_channel_dirs_finds_untracked_old_dir(tmp_path, monkeypatch):
    orphan = tmp_path / "22222222-2222-2222-2222-222222222222"
    orphan.mkdir()
    old_mtime = time.time() - 3600
    os.utime(orphan, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert [p.name for p in orphans] == [orphan.name]


def test_find_orphaned_channel_dirs_skips_recently_created_dir(tmp_path, monkeypatch):
    """A directory too recent to be sure it isn't just starting up (the
    directory-created-before-state-persisted race in _start_ffmpeg) must
    not be treated as orphaned yet."""
    fresh = tmp_path / "33333333-3333-3333-3333-333333333333"
    fresh.mkdir()  # mtime is "now"

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert orphans == []


def test_scrub_orphaned_dirs_removes_and_reports(tmp_path, monkeypatch):
    orphan = tmp_path / "44444444-4444-4444-4444-444444444444"
    orphan.mkdir()
    (orphan / "segment0.ts").write_bytes(b"data")
    old_mtime = time.time() - 3600
    os.utime(orphan, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    logger = _FakeLogger()
    removed = timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), min_age_seconds=60, logger=logger)
    assert removed == [orphan.name]
    assert not orphan.exists()
    assert any(level == "info" for level, _msg in logger.calls)


# ---------------------------------------------------------------------
# _get_live_manifest -- real filesystem (tmp_path), no Redis/Django
# dependency (state is passed in directly, not fetched from Redis).
# ---------------------------------------------------------------------


def _write_playlist(channel_dir: Path, media_sequence: int, segments):
    """segments: list of (filename, content_bytes, duration_str)."""
    channel_dir.mkdir(parents=True, exist_ok=True)
    lines = [f"#EXT-X-MEDIA-SEQUENCE:{media_sequence}"]
    for filename, content, duration in segments:
        (channel_dir / filename).write_bytes(content)
        lines.append(f"#EXTINF:{duration},")
        lines.append(filename)
    (channel_dir / "live.m3u8").write_text("\n".join(lines) + "\n")


def _make_state(tmp_path, channel_uuid=_UUID, access_token="tok1", pid=None):
    return {
        "channel_uuid": channel_uuid,
        "storage_path": str(tmp_path),
        "access_token": access_token,
        "pid": pid,
    }


def test_get_live_manifest_no_playlist_process_dead(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid: False)
    state = _make_state(tmp_path, pid=99999)
    with pytest.raises(timeshift_buffer_plugin.BufferFailedError):
        timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())


def test_get_live_manifest_no_playlist_process_alive(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid: True)
    state = _make_state(tmp_path, pid=os.getpid())
    with pytest.raises(RuntimeError) as exc_info:
        timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert not isinstance(exc_info.value, timeshift_buffer_plugin.BufferFailedError)


def test_get_live_manifest_basic(tmp_path):
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=5,
        segments=[("seg0.ts", b"a" * 100, "2.0"), ("seg1.ts", b"b" * 200, "2.5")],
    )
    state = _make_state(tmp_path)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())

    assert manifest["media_sequence"] == 5
    assert manifest["total_bytes"] == 300
    assert manifest["total_duration_ms"] == 4500
    assert manifest["segments"] == [
        {
            "filename": "seg0.ts",
            "sequence": 5,
            "byte_offset": 0,
            "byte_size": 100,
            "time_offset_ms": 0,
            "duration_ms": 2000,
        },
        {
            "filename": "seg1.ts",
            "sequence": 6,
            "byte_offset": 100,
            "byte_size": 200,
            "time_offset_ms": 2000,
            "duration_ms": 2500,
        },
    ]


def test_get_live_manifest_malformed_duration_defaults_to_zero(tmp_path):
    """Mirrors recording_edl's own nan/inf regression: a single malformed
    #EXTINF: line (ffmpeg is the only realistic writer and isn't expected
    to emit this, but a parser shouldn't assume that) must not fail the
    whole manifest fetch."""
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, media_sequence=0, segments=[("seg0.ts", b"x" * 10, "inf")])
    state = _make_state(tmp_path)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert manifest["segments"][0]["duration_ms"] == 0


def test_get_live_manifest_drops_segment_missing_from_disk(tmp_path):
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=0,
        segments=[("seg0.ts", b"x" * 10, "2.0"), ("seg1.ts", b"y" * 10, "2.0")],
    )
    (channel_dir / "seg1.ts").unlink()  # recycled by ffmpeg's -segment_wrap
    state = _make_state(tmp_path)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["filename"] for s in manifest["segments"]] == ["seg0.ts"]


def test_get_live_manifest_no_segments_raises(tmp_path):
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, media_sequence=0, segments=[])
    state = _make_state(tmp_path)

    with pytest.raises(RuntimeError, match="no segments"):
        timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())


def test_get_live_manifest_cache_reuse_when_playlist_unchanged(tmp_path):
    """If the playlist file's own mtime/size haven't changed at all, the
    cached entry is trusted outright -- no re-stat, not even for what was
    the newest segment. Confirmed by changing a segment's real size on
    disk without touching the playlist: the second call must still
    report the *original* cached size."""
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, media_sequence=0, segments=[("seg0.ts", b"a" * 50, "2.0")])
    state = _make_state(tmp_path)

    first = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert first["segments"][0]["byte_size"] == 50

    (channel_dir / "seg0.ts").write_bytes(b"a" * 999)  # file changed, playlist untouched

    second = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert second["segments"][0]["byte_size"] == 50  # still the stale cached value


def test_get_live_manifest_newest_segment_always_restatted(tmp_path):
    """The exact documented race this cache guards against: even when a
    full reparse happens (playlist changed) and the newest segment
    happens to match a cache entry by (sequence, filename), its size is
    always freshly stat()'d, never trusted from cache."""
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=0,
        segments=[("seg0.ts", b"a" * 50, "2.0"), ("seg1.ts", b"b" * 77, "2.0")],
    )
    state = _make_state(tmp_path, access_token="tok1")

    # Pre-seed a cache entry with a playlist_mtime_ns/size that won't match
    # the real file (forcing a full reparse), a matching instance_token, and
    # a deliberately-wrong cached size for what will be the newest segment.
    timeshift_buffer_plugin._manifest_cache[_UUID] = {
        "instance_token": "tok1",
        "playlist_mtime_ns": 1,
        "playlist_size": 1,
        "media_sequence": 0,
        "by_sequence": {0: ("seg0.ts", 50, 2000), 1: ("seg1.ts", 999999, 2000)},
    }

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    sizes = {s["filename"]: s["byte_size"] for s in manifest["segments"]}
    assert sizes["seg0.ts"] == 50  # non-newest, correctly reused from cache
    assert sizes["seg1.ts"] == 77  # newest -- real size, not the stale 999999


def test_get_live_manifest_instance_token_mismatch_forces_full_reparse(tmp_path):
    """The exact "Packet corrupt" incident this project's docs describe:
    a channel stopped and restarted gets a brand-new ffmpeg instance
    whose live.m3u8 restarts numbering from scratch, reusing the same
    (sequence, filename) pairs for genuinely different file content. A
    mismatched access_token must invalidate the *whole* cache entry, even
    when the playlist's own mtime/size happen to match exactly."""
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=0,
        segments=[("seg0.ts", b"a" * 50, "2.0"), ("seg1.ts", b"b" * 77, "2.0")],
    )
    real_stat = (channel_dir / "live.m3u8").stat()
    state = _make_state(tmp_path, access_token="new_instance_token")

    # Cache entry from a *previous* buffer instance -- matches the real
    # playlist's own current stat exactly (as it could plausibly do after
    # a restart that happens to produce a same-sized playlist), but was
    # built from different, unrelated file content.
    timeshift_buffer_plugin._manifest_cache[_UUID] = {
        "instance_token": "old_instance_token",
        "playlist_mtime_ns": real_stat.st_mtime_ns,
        "playlist_size": real_stat.st_size,
        "media_sequence": 0,
        "by_sequence": {0: ("seg0.ts", 12345, 2000), 1: ("seg1.ts", 67890, 2000)},
    }

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    sizes = {s["filename"]: s["byte_size"] for s in manifest["segments"]}
    assert sizes["seg0.ts"] == 50  # real size, not the stale other-instance value
    assert sizes["seg1.ts"] == 77


# ---------------------------------------------------------------------
# Plugin._resolve_channel_uuid -- a static method, zero Redis dependency.
# ---------------------------------------------------------------------


def test_resolve_channel_uuid_from_params():
    result = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": _UUID}, {})
    assert result == _UUID


def test_resolve_channel_uuid_falls_back_to_test_setting():
    result = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({}, {"test_channel_uuid": _UUID})
    assert result == _UUID


def test_resolve_channel_uuid_params_take_priority_over_setting():
    other_uuid = "22222222-2222-2222-2222-222222222222"
    result = timeshift_buffer_plugin.Plugin._resolve_channel_uuid(
        {"channel_uuid": _UUID}, {"test_channel_uuid": other_uuid}
    )
    assert result == _UUID


def test_resolve_channel_uuid_neither_present():
    assert timeshift_buffer_plugin.Plugin._resolve_channel_uuid({}, {}) is None


def test_resolve_channel_uuid_rejects_non_uuid_value():
    """The exact security-relevant case this validation exists for: a
    caller-supplied value like "../recordings" must be refused before it
    ever reaches a filesystem path -- every action treats this the same
    as a missing channel_uuid."""
    result = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": "../recordings"}, {})
    assert result is None


def test_resolve_channel_uuid_empty_string_falls_back_to_setting():
    result = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": ""}, {"test_channel_uuid": _UUID})
    assert result == _UUID
