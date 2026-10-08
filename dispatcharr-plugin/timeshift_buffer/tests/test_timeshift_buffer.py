"""Unit tests for dispatcharr-plugin/timeshift_buffer/plugin.py.

Loaded by explicit file path with a unique synthetic module name (not a
bare `import plugin`) since recording_edl's own plugin.py shares the same
filename -- importing both by name in one pytest session would otherwise
collide via sys.modules.

Covers only logic with no Redis/Django/real-HTTP-socket dependency --
the real Redis client (_redis()), the actual HTTP server, and real
ffmpeg subprocess management stay untested here, same boundary this
project draws elsewhere (see recording_edl's own tests and
docs/CLOSED_ITEMS.md's "No automated test suite exists" entry). Functions
that only had a Redis/Django dependency in *part* of their logic
(_find_orphaned_channel_dirs/_scrub_orphaned_dirs, _stream_attribution_headers)
are still tested by monkeypatching just that one call, or by only
exercising the code path that never touches it.

Plugin.run()'s own dispatch and every action handler's message
formatting *is* covered too, the same way as recording_edl's own
Plugin.run() tests: by monkeypatching the module-level Redis-touching
functions (_get_buffer_state/_set_buffer_state/_delete_buffer_state/
_list_buffer_keys/_iter_buffer_states) and process-management functions
(_start_ffmpeg/_remove_channel_files/_teardown_buffer/_is_process_alive)
each handler calls, plus the two lifecycle calls run() itself makes
unconditionally before ever dispatching (_ensure_http_server_running/
_ensure_reaper_running, stubbed to no-ops via the _run() helper below --
a real HTTP server/reaper thread has no place in a unit test).
"""

import importlib.util
import contextlib
import json
import os
import shutil
import subprocess
import threading
import time
import types
from pathlib import Path

import pytest

_PLUGIN_PATH = Path(__file__).parent.parent / "plugin.py"
_spec = importlib.util.spec_from_file_location("timeshift_buffer_plugin", _PLUGIN_PATH)
timeshift_buffer_plugin = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(timeshift_buffer_plugin)
# The real compare-and-set, kept before the autouse fixture below swaps in a
# stand-in for every other test.
_REAL_CAS = timeshift_buffer_plugin._cas_buffer_state


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
def _cas_over_patched_state_functions(monkeypatch):
    """Most tests replace _get_buffer_state/_set_buffer_state with in-memory
    stand-ins. _update_buffer_state() (every state writer now goes through it)
    ends in _cas_buffer_state(), which talks to Redis directly; this default
    reimplements it over whatever those two currently are, so those tests
    keep exercising the same logic. The tests of the compare-and-set itself
    install their own fake Redis client instead (see _FakeStateRedis)."""

    def cas(channel_uuid, expected, new_state):
        current = timeshift_buffer_plugin._get_buffer_state(channel_uuid)
        if current is None:
            return False
        text = expected.decode() if isinstance(expected, bytes) else expected
        if json.dumps(current) != text:
            return False
        timeshift_buffer_plugin._set_buffer_state(channel_uuid, new_state)
        return True

    monkeypatch.setattr(timeshift_buffer_plugin, "_cas_buffer_state", cas)


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
# _remove_channel_files
# ---------------------------------------------------------------------


def test_remove_channel_files_removes_an_existing_directory(tmp_path):
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    (channel_dir / "seg_00000.ts").write_bytes(b"data")
    logger = _FakeLogger()

    timeshift_buffer_plugin._remove_channel_files({"storage_path": str(tmp_path), "channel_uuid": _UUID}, logger)

    assert not channel_dir.exists()


def test_remove_channel_files_is_a_noop_when_the_directory_is_already_gone(tmp_path):
    """_teardown_buffer calls this unconditionally -- a buffer that was
    never started, or whose files were already cleaned up, must not raise."""
    logger = _FakeLogger()

    timeshift_buffer_plugin._remove_channel_files({"storage_path": str(tmp_path), "channel_uuid": _UUID}, logger)

    assert not any(level == "exception" for level, _msg in logger.calls)


def test_remove_channel_files_refuses_an_invalid_channel_uuid_without_touching_disk(tmp_path):
    """A corrupted/pre-fix Redis entry: nothing safe to remove, so this
    must log and return rather than ever calling shutil.rmtree with a
    path built from the bad value."""
    logger = _FakeLogger()

    timeshift_buffer_plugin._remove_channel_files({"storage_path": str(tmp_path), "channel_uuid": "../../etc"}, logger)

    assert any(level == "error" for level, _msg in logger.calls)
    assert list(tmp_path.iterdir()) == []


def test_remove_channel_files_logs_but_does_not_raise_on_other_os_errors(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    logger = _FakeLogger()

    def _raise(_path):
        raise PermissionError("denied")

    monkeypatch.setattr(timeshift_buffer_plugin.shutil, "rmtree", _raise)

    timeshift_buffer_plugin._remove_channel_files({"storage_path": str(tmp_path), "channel_uuid": _UUID}, logger)

    assert any(level == "exception" for level, _msg in logger.calls)


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


def test_resolve_request_path_rejects_embedded_null_byte():
    """A 52nd-pass audit regression case: a percent-encoded null byte
    (%00) decodes to a literal "\\x00", which Path.resolve() raises an
    uncaught ValueError on if not caught here first -- this function's
    own caller has nothing to catch that."""
    channel_uuid, path = timeshift_buffer_plugin._resolve_request_path(f"/{_UUID}/%00evil.ts", "/data/timeshift")
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
# _redact_token_query_param -- used by _BufferRequestHandler.log_message
# to keep the buffer's own access_token out of whatever this plugin's
# logger is configured to persist.
# ---------------------------------------------------------------------


def test_redact_token_query_param_redacts_a_bare_token():
    text = '"GET /uuid/seg0.ts?token=abc123 HTTP/1.1" 200 -'
    redacted = timeshift_buffer_plugin._redact_token_query_param(text)
    assert "abc123" not in redacted
    assert "token=REDACTED" in redacted


def test_redact_token_query_param_stops_at_the_next_query_param():
    text = "GET /uuid/seg0.ts?viewer_id=v1&token=abc123&other=1 HTTP/1.1"
    redacted = timeshift_buffer_plugin._redact_token_query_param(text)
    assert redacted == "GET /uuid/seg0.ts?viewer_id=v1&token=REDACTED&other=1 HTTP/1.1"


def test_redact_token_query_param_leaves_text_with_no_token_unchanged():
    text = '"GET /uuid/live.m3u8 HTTP/1.1" 200 -'
    assert timeshift_buffer_plugin._redact_token_query_param(text) == text


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


def test_parse_range_first_byte_only_is_satisfiable_not_the_sentinel():
    """Regression test for a real bug: (0, 0) is a perfectly valid,
    satisfiable range (just the first byte), but comparing the old
    (False, False) sentinel with `==` treated it as unsatisfiable, since
    Python's `0 == False`. Must be a genuine (0, 0) tuple, not the
    sentinel object, and specifically not `is` the sentinel."""
    result = _parse_range("bytes=0-0", 1000)
    assert result == (0, 0)
    assert result is not timeshift_buffer_plugin._RANGE_UNSATISFIABLE


def test_parse_range_unsatisfiable_start_past_eof():
    assert _parse_range("bytes=1000-2000", 1000) is timeshift_buffer_plugin._RANGE_UNSATISFIABLE


def test_parse_range_unsatisfiable_end_before_start():
    assert _parse_range("bytes=50-10", 1000) is timeshift_buffer_plugin._RANGE_UNSATISFIABLE


def test_parse_range_unsatisfiable_zero_suffix():
    assert _parse_range("bytes=-0", 1000) is None


def test_parse_range_garbage_values():
    assert _parse_range("bytes=abc-def", 1000) is None


def test_parse_range_only_first_of_multirange():
    # Multi-range unsupported by design -- only the first range is honored.
    assert _parse_range("bytes=10-20,30-40", 1000) == (10, 20)


# ---------------------------------------------------------------------
# _BufferRequestHandler._content_type_for
# ---------------------------------------------------------------------

_content_type_for = timeshift_buffer_plugin._BufferRequestHandler._content_type_for


def test_content_type_for_m3u8():
    assert _content_type_for(Path("/data/live.m3u8")) == "application/vnd.apple.mpegurl"


def test_content_type_for_ts():
    assert _content_type_for(Path("/data/seg_00001.ts")) == "video/mp2t"


def test_content_type_for_unknown_extension_falls_back_to_octet_stream():
    assert _content_type_for(Path("/data/ffmpeg.log")) == "application/octet-stream"


# ---------------------------------------------------------------------
# _BufferRequestHandler._plan_range_response
# ---------------------------------------------------------------------

_plan_range_response = timeshift_buffer_plugin._BufferRequestHandler._plan_range_response


def test_plan_range_response_whole_file_when_no_range():
    status, start, length, content_range = _plan_range_response(None, 1000)
    assert status == 200
    assert start == 0
    assert length is None  # do_GET() reads to EOF unbounded, not a fixed length
    assert content_range is None


def test_plan_range_response_partial_content_for_a_real_range():
    status, start, length, content_range = _plan_range_response((10, 20), 1000)
    assert status == 206
    assert start == 10
    assert length == 11  # inclusive 10-20
    assert content_range == "bytes 10-20/1000"


def test_plan_range_response_first_byte_only():
    # The exact regression this pairs with: (0, 0) is satisfiable, not
    # the sentinel -- see test_parse_range_first_byte_only_is_satisfiable_not_the_sentinel.
    status, start, length, content_range = _plan_range_response((0, 0), 1000)
    assert status == 206
    assert start == 0
    assert length == 1
    assert content_range == "bytes 0-0/1000"


# ---------------------------------------------------------------------
# _BufferRequestHandler._check_access_token -- the only access control on
# this file server (see its own comment). Not a static method (needs
# self.path), and BaseHTTPRequestHandler.__init__ does real socket I/O, so
# a plain object with just a .path attribute stands in for self here --
# the method only ever touches that one attribute plus the module-level
# _get_buffer_state call, which is monkeypatched the same way elsewhere
# in this file.
# ---------------------------------------------------------------------

_check_access_token = timeshift_buffer_plugin._BufferRequestHandler._check_access_token


def _fake_handler(path):
    return types.SimpleNamespace(path=path)


def test_check_access_token_accepts_matching_token(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"access_token": "secret123"})

    handler = _fake_handler("/segment0.ts?token=secret123")

    assert _check_access_token(handler, "chan-uuid") is True


def test_check_access_token_rejects_wrong_token(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"access_token": "secret123"})

    handler = _fake_handler("/segment0.ts?token=wrong")

    assert _check_access_token(handler, "chan-uuid") is False


def test_check_access_token_rejects_missing_token_query_param(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"access_token": "secret123"})

    handler = _fake_handler("/segment0.ts")  # no ?token= at all

    assert _check_access_token(handler, "chan-uuid") is False


def test_check_access_token_rejects_when_no_buffer_state_exists(monkeypatch):
    """No buffer for this channel at all (e.g. never started, or already
    torn down) -- must deny, not raise, and never treat a missing expected
    token as satisfied by a missing/empty provided one."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    handler = _fake_handler("/segment0.ts?token=anything")

    assert _check_access_token(handler, "chan-uuid") is False


def test_check_access_token_rejects_when_expected_token_is_empty_string(monkeypatch):
    """A falsy expected token (empty string, not just missing) must still
    deny -- an empty provided token must never accidentally satisfy it."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"access_token": ""})

    handler = _fake_handler("/segment0.ts?token=")

    assert _check_access_token(handler, "chan-uuid") is False


def test_check_access_token_rejects_a_non_ascii_token_instead_of_raising(monkeypatch):
    """Regression test for a real bug: secrets.compare_digest() only
    accepts bytes-like objects or ASCII-only strings -- a percent-encoded
    non-ASCII query value used to raise an uncaught TypeError here rather
    than a clean False, propagating straight through do_GET()/do_HEAD()
    with no response ever sent."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"access_token": "secret123"})

    handler = _fake_handler("/segment0.ts?token=%C3%A9")  # decodes to "é"

    assert _check_access_token(handler, "chan-uuid") is False


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
# _int_setting
# ---------------------------------------------------------------------


def test_int_setting_parses_a_valid_value():
    assert timeshift_buffer_plugin._int_setting({"segment_seconds": "5"}, "segment_seconds", 2) == 5


def test_int_setting_falls_back_to_default_when_the_key_is_missing():
    assert timeshift_buffer_plugin._int_setting({}, "segment_seconds", 2) == 2


def test_int_setting_falls_back_to_default_for_an_empty_string_the_real_bug_this_fixes():
    """A cleared number field in Dispatcharr's own settings UI can save
    as "" -- a bare int("") raised an uncaught ValueError at the very
    top of run(), breaking every action for that plugin instance."""
    assert timeshift_buffer_plugin._int_setting({"http_port": ""}, "http_port", 9192) == 9192


def test_int_setting_falls_back_to_default_for_a_non_numeric_string():
    assert timeshift_buffer_plugin._int_setting({"http_port": "not-a-number"}, "http_port", 9192) == 9192


def test_int_setting_floors_a_zero_value_at_the_given_minimum_the_real_bug_this_fixes():
    """segment_seconds=0 would otherwise divide by zero in
    _compute_segment_counts(); idle_timeout_seconds=0 would make the
    reaper treat every viewer as stale on every tick."""
    assert timeshift_buffer_plugin._int_setting({"segment_seconds": "0"}, "segment_seconds", 2, minimum=1) == 1


def test_int_setting_floors_a_negative_value_at_the_given_minimum():
    assert timeshift_buffer_plugin._int_setting({"buffer_minutes": "-5"}, "buffer_minutes", 60, minimum=1) == 1


def test_int_setting_with_no_minimum_allows_zero_and_negative_values():
    assert timeshift_buffer_plugin._int_setting({"x": "0"}, "x", 1) == 0
    assert timeshift_buffer_plugin._int_setting({"x": "-5"}, "x", 1) == -5


def test_int_setting_passes_through_a_valid_value_already_above_the_minimum():
    assert timeshift_buffer_plugin._int_setting({"segment_seconds": "5"}, "segment_seconds", 2, minimum=1) == 5


def test_int_setting_caps_a_value_above_the_given_maximum_the_real_bug_this_fixes():
    """http_port=70000 parses fine as a plain int, but socket.bind()
    then raises OverflowError -- not the OSError _ensure_http_server_
    running() actually catches -- breaking every action the same way an
    unparseable value did, since this runs before action dispatch."""
    assert (
        timeshift_buffer_plugin._int_setting({"http_port": "70000"}, "http_port", 9192, minimum=1, maximum=65535)
        == 65535
    )


def test_int_setting_passes_through_a_valid_value_already_below_the_maximum():
    assert (
        timeshift_buffer_plugin._int_setting({"http_port": "9192"}, "http_port", 9192, minimum=1, maximum=65535) == 9192
    )


def test_int_setting_with_no_maximum_allows_an_arbitrarily_large_value():
    assert timeshift_buffer_plugin._int_setting({"x": "999999"}, "x", 1) == 999999


# ---------------------------------------------------------------------
# _str_setting
# ---------------------------------------------------------------------


def test_str_setting_passes_through_a_valid_value():
    assert (
        timeshift_buffer_plugin._str_setting({"storage_path": "/mnt/data"}, "storage_path", "/data/timeshift")
        == "/mnt/data"
    )


def test_str_setting_falls_back_to_default_when_the_key_is_missing():
    assert timeshift_buffer_plugin._str_setting({}, "storage_path", "/data/timeshift") == "/data/timeshift"


def test_str_setting_falls_back_to_default_for_an_empty_string_the_real_bug_this_fixes():
    """A cleared text field in Dispatcharr's own settings UI can save as
    "" -- Path("") is the worker process's own current working
    directory, so this would otherwise silently redirect every buffer's
    files (and the HTTP server's own served root) there instead of
    failing loudly."""
    assert (
        timeshift_buffer_plugin._str_setting({"storage_path": ""}, "storage_path", "/data/timeshift")
        == "/data/timeshift"
    )


def test_str_setting_falls_back_to_default_for_a_whitespace_only_string():
    assert (
        timeshift_buffer_plugin._str_setting({"storage_path": "   "}, "storage_path", "/data/timeshift")
        == "/data/timeshift"
    )


def test_str_setting_falls_back_to_default_for_a_non_string_value():
    assert (
        timeshift_buffer_plugin._str_setting({"storage_path": 123}, "storage_path", "/data/timeshift")
        == "/data/timeshift"
    )


def test_str_setting_does_not_reject_a_relative_path():
    # Unusual, but not itself invalid -- only an emptied/missing/non-string
    # value falls back to the default.
    assert (
        timeshift_buffer_plugin._str_setting({"storage_path": "relative/dir"}, "storage_path", "/data/timeshift")
        == "relative/dir"
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
# _build_ffmpeg_command
# ---------------------------------------------------------------------


def test_build_ffmpeg_command_omits_headers_flag_when_none():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    assert "-headers" not in cmd


def test_build_ffmpeg_command_places_headers_flag_immediately_before_input():
    """Confirmed live: ffmpeg applies -headers to the input that follows
    it, not globally -- must appear immediately before -i, not just
    somewhere in the argv."""
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", "X-Real-IP: 1.2.3.4\r\n", 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    i_index = cmd.index("-i")
    assert cmd[i_index - 2 : i_index] == ["-headers", "X-Real-IP: 1.2.3.4\r\n"]
    assert cmd[i_index + 1] == "http://example/proxy"


def test_build_ffmpeg_command_never_includes_reset_timestamps():
    """The exact flag this project deliberately removed -- see this
    function's own docstring for the seeking regression it caused."""
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    assert "-reset_timestamps" not in cmd


def test_build_ffmpeg_command_uses_the_hls_muxer_with_a_visible_window_and_a_delete_margin():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    assert cmd[cmd.index("-f") + 1] == "hls"
    assert cmd[cmd.index("-hls_time") + 1] == "2"
    assert cmd[cmd.index("-hls_list_size") + 1] == "1800"
    # the same 2x window the old -segment_wrap gave: listed + this many kept beyond the list
    assert cmd[cmd.index("-hls_delete_threshold") + 1] == "1800"
    assert cmd[cmd.index("-hls_flags") + 1] == "delete_segments+omit_endlist"
    assert cmd[cmd.index("-hls_segment_filename") + 1] == "seg_%05d.ts"
    assert cmd[-1] == "/data/live.m3u8"


def test_build_ffmpeg_command_never_uses_the_segment_muxer():
    """The segment muxer restarts every PID's continuity counter at 0 in each file, so every splice the addon
    makes is a break the demuxer reports as "Packet corrupt" -- see _build_ffmpeg_command's docstring."""
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    assert "segment" not in cmd
    assert not any(a.startswith("-segment") for a in cmd)


def test_build_ffmpeg_command_delete_margin_is_at_least_one():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 1, "/data/live.m3u8", 1, "seg_%05d.ts"
    )

    assert cmd[cmd.index("-hls_delete_threshold") + 1] == "1"


def test_build_ffmpeg_command_uses_stream_copy_not_re_encode():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://example/proxy", None, 2, 3600, "/data/live.m3u8", 1800, "seg_%05d.ts"
    )

    assert cmd[cmd.index("-c") + 1] == "copy"


def _ts_continuity_breaks(segment_paths):
    """Counts, over every PID in every consecutive pair of segment files, the first packet whose continuity counter
    is not what the previous file's last packet of that PID leads to (a payload packet increments it, an
    adaptation-only one repeats it)."""
    breaks = 0
    last = {}
    for path in segment_paths:
        data = path.read_bytes()
        seen = set()
        for i in range(0, len(data) - 187, 188):
            pid = ((data[i + 1] & 0x1F) << 8) | data[i + 2]
            if pid == 0x1FFF:
                continue
            afc = (data[i + 3] >> 4) & 3
            cc = data[i + 3] & 15
            if pid not in seen:
                seen.add(pid)
                if pid in last:
                    prev_cc, _ = last[pid]
                    expected = (prev_cc + 1) % 16 if afc & 1 else prev_cc
                    if cc != expected:
                        breaks += 1
            last[pid] = (cc, afc)
    return breaks


# CI sets REQUIRE_FFMPEG so a missing ffmpeg is a failure there, not a silent skip of the only guard
# on the hls-muxer continuity-counter fix.
@pytest.mark.skipif(
    shutil.which("ffmpeg") is None and not os.environ.get("REQUIRE_FFMPEG"), reason="ffmpeg not installed"
)
def test_buffer_segments_keep_one_continuity_counter_across_files(tmp_path):
    """The reason for the hls muxer (see _build_ffmpeg_command's docstring): the files the addon splices into one
    byte stream must not each restart the MPEG-TS continuity counters. Runs the real ffmpeg with the plugin's own
    argv over a generated stream."""
    source = tmp_path / "in.ts"
    gen = subprocess.run(
        [
            "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=duration=14:size=160x120:rate=25",
            "-f", "lavfi", "-i", "sine=duration=14", "-c:v", "libx264", "-g", "25", "-c:a", "aac", "-f", "mpegts",
            str(source),
        ],
        capture_output=True,
    )  # fmt: skip
    if gen.returncode != 0:
        if os.environ.get("REQUIRE_FFMPEG"):
            # CI installs a full ffmpeg and sets this: a skip here would again let the only continuity-counter guard
            # silently not run (the first 0.8.0 sweep found exactly that for a missing binary).
            pytest.fail("this ffmpeg cannot generate the test stream (needs libx264/aac)")
        pytest.skip("this ffmpeg cannot generate the test stream (needs libx264/aac)")

    out = tmp_path / "chan"
    out.mkdir()
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        str(source), None, 2, 20, str(out / "live.m3u8"), 10, "seg_%05d.ts"
    )
    cmd[cmd.index("-i") + 1] = str(source)
    run = subprocess.run(cmd, cwd=out, capture_output=True, timeout=60)
    assert run.returncode == 0, run.stderr.decode(errors="replace")

    segments = sorted(out.glob("seg_*.ts"))
    assert len(segments) >= 4
    assert _ts_continuity_breaks(segments) == 0


# ---------------------------------------------------------------------
# _stream_attribution_headers -- the client_ip path against the real code, and the
# username/JWT branch (Django is imported inside `if username:`) against fake
# `django.contrib.auth` / `rest_framework_simplejwt.tokens` modules in sys.modules,
# the same way recording_edl's test_django_boundary.py reaches its Django glue.
# ---------------------------------------------------------------------


def _install_fake_jwt(monkeypatch, *, users=("alice",), raise_on_mint=False):
    """Fake the two modules the username branch imports. Returns the list of
    `set_exp` lifetimes the fake access token was given."""
    import sys
    import types

    lifetimes = []

    class _Access:
        def __init__(self, username):
            self.username = username
            self.exp = "default-lifetime"

        def set_exp(self, claim="exp", from_time=None, lifetime=None):
            lifetimes.append(lifetime)
            self.exp = lifetime

        def __str__(self):
            return f"jwt-for-{self.username}-exp-{self.exp}"

    class _Refresh:
        def __init__(self, username):
            self.access_token = _Access(username)

        @classmethod
        def for_user(cls, user):
            if raise_on_mint:
                raise RuntimeError("signing key unavailable")
            return cls(user.username)

    class _DoesNotExist(Exception):
        pass

    class _User:
        DoesNotExist = _DoesNotExist

        def __init__(self, username):
            self.username = username

        class objects:  # noqa: N801 -- mimics the Django manager attribute
            @staticmethod
            def get(username):
                if username not in users:
                    raise _DoesNotExist(username)
                return _User(username)

    auth = types.ModuleType("django.contrib.auth")
    auth.get_user_model = lambda: _User
    tokens = types.ModuleType("rest_framework_simplejwt.tokens")
    tokens.RefreshToken = _Refresh
    for name, mod in (
        ("django", types.ModuleType("django")),
        ("django.contrib", types.ModuleType("django.contrib")),
        ("django.contrib.auth", auth),
        ("rest_framework_simplejwt", types.ModuleType("rest_framework_simplejwt")),
        ("rest_framework_simplejwt.tokens", tokens),
    ):
        monkeypatch.setitem(sys.modules, name, mod)
    return lifetimes


def test_stream_attribution_headers_username_gives_one_short_lived_bearer_line(monkeypatch):
    lifetimes = _install_fake_jwt(monkeypatch)
    result = timeshift_buffer_plugin._stream_attribution_headers({"username": "alice"}, _FakeLogger())
    assert result.count("Authorization: Bearer ") == 1
    assert result.endswith("\r\n")
    assert "jwt-for-alice" in result
    # The token is on ffmpeg's command line, readable by every local user through /proc:
    # it must not carry the account's default access lifetime.
    assert len(lifetimes) == 1
    assert lifetimes[0].total_seconds() == timeshift_buffer_plugin._STREAM_TOKEN_LIFETIME_SECONDS
    assert timeshift_buffer_plugin._STREAM_TOKEN_LIFETIME_SECONDS <= 300


def test_stream_attribution_headers_username_and_client_ip_both_appear_in_order(monkeypatch):
    _install_fake_jwt(monkeypatch)
    result = timeshift_buffer_plugin._stream_attribution_headers(
        {"username": "alice", "client_ip": "192.168.1.42"}, _FakeLogger()
    )
    lines = result.split("\r\n")
    assert lines[0].startswith("Authorization: Bearer ")
    assert lines[1] == "X-Real-IP: 192.168.1.42"
    assert lines[2:] == [""]


@pytest.mark.parametrize("failure", ["unknown-user", "mint-error"])
def test_stream_attribution_headers_username_failure_streams_anonymously_and_logs(monkeypatch, failure):
    _install_fake_jwt(monkeypatch, users=("alice",), raise_on_mint=(failure == "mint-error"))
    logger = _FakeLogger()
    username = "nobody" if failure == "unknown-user" else "alice"
    assert timeshift_buffer_plugin._stream_attribution_headers({"username": username}, logger) is None
    assert any(level == "exception" for level, _msg in logger.calls)


def test_stream_attribution_headers_username_failure_still_keeps_a_valid_client_ip(monkeypatch):
    _install_fake_jwt(monkeypatch, users=())
    result = timeshift_buffer_plugin._stream_attribution_headers(
        {"username": "alice", "client_ip": "10.0.0.5"}, _FakeLogger()
    )
    assert result == "X-Real-IP: 10.0.0.5\r\n"


def test_stream_attribution_headers_no_params_returns_none():
    logger = _FakeLogger()
    assert timeshift_buffer_plugin._stream_attribution_headers({}, logger) is None


def test_stream_attribution_headers_valid_client_ip():
    logger = _FakeLogger()
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": "192.168.1.42"}, logger)
    assert result == "X-Real-IP: 192.168.1.42\r\n"


def test_stream_attribution_headers_valid_ipv6_client_ip():
    """A bare (no zone/scope id) IPv6 address is still legitimate and
    must not be caught by the scope-id-bypass fix below."""
    logger = _FakeLogger()
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": "2001:db8::1"}, logger)
    assert result == "X-Real-IP: 2001:db8::1\r\n"


def test_stream_attribution_headers_rejects_invalid_ip():
    logger = _FakeLogger()
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": "not-an-ip"}, logger)
    assert result is None
    assert any(level == "warning" for level, _msg in logger.calls)


def test_stream_attribution_headers_rejects_header_injection_attempt():
    """The exact documented security fix (docs/TIMESHIFT.md's "client_ip
    header injection" section): an unvalidated client_ip let a caller
    smuggle a second, pipelined request onto ffmpeg's own connection.
    ipaddress.ip_address() alone rejects this specific (IPv4) payload,
    including embedded CRLF-style injection attempts -- see the IPv6
    scope-id regression test below for a payload it does NOT reject on
    its own, which is why this function also checks for "%"/"\\r"/"\\n"
    explicitly rather than relying on ipaddress.ip_address() alone."""
    logger = _FakeLogger()
    malicious = "1.2.3.4\r\nX-Injected: evil"
    result = timeshift_buffer_plugin._stream_attribution_headers({"client_ip": malicious}, logger)
    assert result is None


def test_stream_attribution_headers_rejects_ipv6_scope_id_injection_bypass():
    """Regression test for a real, confirmed bypass of the fix above:
    ipaddress.ip_address()'s own IPv6 zone/scope-id handling (the
    "%..." suffix, e.g. "fe80::1%eth0") only rejects an empty scope id
    or one containing another "%" -- embedded "\\r\\n" passed straight
    through as a "valid" address, reintroducing the exact header-
    smuggling class the fix above exists to close. Confirmed directly:
    ipaddress.ip_address("fe80::1%x\\r\\nX-Injected: evil") does not
    raise ValueError on its own."""
    logger = _FakeLogger()
    malicious = "fe80::1%x\r\nX-Injected: evil"
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
# _apply_heartbeat
# ---------------------------------------------------------------------


# ---------------------------------------------------------------------
# _is_stale_access_token
# ---------------------------------------------------------------------


def test_is_stale_access_token_true_only_on_a_confirmed_mismatch():
    assert timeshift_buffer_plugin._is_stale_access_token({"access_token": "new"}, "old")


def test_is_stale_access_token_false_on_a_match():
    assert not timeshift_buffer_plugin._is_stale_access_token({"access_token": "same"}, "same")


@pytest.mark.parametrize("supplied", [None, "", 123, ["x"]])
def test_is_stale_access_token_false_when_nothing_usable_was_supplied(supplied):
    assert not timeshift_buffer_plugin._is_stale_access_token({"access_token": "tok"}, supplied)


@pytest.mark.parametrize("state", [{}, {"access_token": ""}, {"access_token": None}])
def test_is_stale_access_token_false_when_the_state_predates_access_tokens(state):
    assert not timeshift_buffer_plugin._is_stale_access_token(state, "whatever")


def test_apply_heartbeat_sets_last_heartbeat():
    state = {"last_heartbeat": 0}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000)
    assert state["last_heartbeat"] == 1000


def test_apply_heartbeat_does_not_touch_viewers_when_no_viewer_id_given():
    state = {"viewers": ["a"], "viewer_heartbeats": {"a": 0}}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000)
    assert state["viewers"] == ["a"]
    assert state["viewer_heartbeats"] == {"a": 0}


def test_apply_heartbeat_updates_a_known_viewers_own_heartbeat():
    state = {"viewers": ["a", "b"], "viewer_heartbeats": {"a": 0, "b": 0}}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000, viewer_id="a")
    assert state["viewer_heartbeats"] == {"a": 1000, "b": 0}


def test_apply_heartbeat_re_adds_a_viewer_no_longer_in_the_list():
    # Regression test: a still-watching viewer pruned by
    # _prune_stale_viewers used to be ignored forever (docs/OPEN_ITEMS.md,
    # 10th-pass audit).
    state = {"viewers": ["a"], "viewer_heartbeats": {"a": 0}}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000, viewer_id="pruned")
    assert state["viewers"] == ["a", "pruned"]
    assert state["viewer_heartbeats"] == {"a": 0, "pruned": 1000}


def test_apply_heartbeat_re_adds_into_a_state_with_no_viewer_fields_at_all():
    state = {}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000, viewer_id="v")
    assert state["viewers"] == ["v"]
    assert state["viewer_heartbeats"] == {"v": 1000}


def test_apply_heartbeat_does_not_duplicate_a_viewer_already_present():
    state = {"viewers": ["a"], "viewer_heartbeats": {"a": 0}}
    timeshift_buffer_plugin._apply_heartbeat(state, now=1000, viewer_id="a")
    assert state["viewers"] == ["a"]


def test_apply_heartbeat_returns_the_same_mutated_state():
    state = {}
    assert timeshift_buffer_plugin._apply_heartbeat(state, now=1000) is state


def test_apply_heartbeat_never_overwrites_a_viewer_registered_since_state_was_read():
    """The exact real race this function's own callers are designed to
    avoid: a concurrent start_buffer registers viewer "b" in Redis after
    this handler's own initial read but before its write-back. As long
    as the caller re-reads state immediately before calling this (the
    real call sites do; see _apply_heartbeat's own docstring), "b"
    survives -- this test locks in that _apply_heartbeat itself never
    clobbers a viewers list it wasn't told to touch."""
    fresh_state_after_concurrent_registration = {
        "viewers": ["a", "b"],
        "viewer_heartbeats": {"a": 500, "b": 900},
    }
    timeshift_buffer_plugin._apply_heartbeat(fresh_state_after_concurrent_registration, now=1000)
    assert fresh_state_after_concurrent_registration["viewers"] == ["a", "b"]


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
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: "1234 (ffmpeg) R 1 ...")
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


def test_is_process_alive_false_for_a_zombie(monkeypatch):
    """The real bug this fixes: signal 0 alone can't tell a zombie
    (already exited, not yet reaped by its real parent) apart from a
    genuinely running process -- both answer kill(pid, 0) successfully.
    Reading /proc/<pid>/stat's own state field catches this even when
    this call isn't running in the process's actual parent."""
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: "1234 (ffmpeg) Z 1 ...")
    waitpid_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin.os, "waitpid", lambda pid, opts: waitpid_calls.append((pid, opts)) or (0, 0)
    )

    assert timeshift_buffer_plugin._is_process_alive(1234) is False
    assert waitpid_calls == [(1234, timeshift_buffer_plugin.os.WNOHANG)]


def test_is_process_alive_ignores_waitpid_failure_when_not_the_real_parent(monkeypatch):
    """The common case (see this function's own docstring): the
    Redis-tracked pid usually isn't this worker process's own child, so
    waitpid raising ChildProcessError here must not be treated as an
    error -- the zombie verdict from /proc/<pid>/stat still stands."""
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: "1234 (ffmpeg) Z 1 ...")

    def raise_child_process_error(pid, opts):
        raise ChildProcessError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "waitpid", raise_child_process_error)

    assert timeshift_buffer_plugin._is_process_alive(1234) is False


def test_is_process_alive_true_when_proc_stat_unreadable(monkeypatch):
    """Can't confirm zombie state either way (e.g. not actually Linux, or
    a permissions/timing gap) -- same conservative "assume alive" default
    the other-errors case above already uses."""
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: None)

    assert timeshift_buffer_plugin._is_process_alive(1234) is True


# ---------------------------------------------------------------------
# _is_zombie_proc_stat -- pure parsing of /proc/<pid>/stat's own state
# field.
# ---------------------------------------------------------------------


def test_is_zombie_proc_stat_true_for_zombie_state():
    assert timeshift_buffer_plugin._is_zombie_proc_stat("1234 (ffmpeg) Z 1 1234 ...") is True


def test_is_zombie_proc_stat_false_for_running_state():
    assert timeshift_buffer_plugin._is_zombie_proc_stat("1234 (ffmpeg) R 1 1234 ...") is False


def test_is_zombie_proc_stat_handles_comm_containing_spaces_and_parens():
    """comm (the process name in parens) can itself contain spaces or
    parentheses -- this must find the LAST ')' to locate the state field,
    not the first."""
    assert timeshift_buffer_plugin._is_zombie_proc_stat("1234 (ffmpeg (child) worker) Z 1 ...") is True
    assert timeshift_buffer_plugin._is_zombie_proc_stat("1234 (ffmpeg (child) worker) R 1 ...") is False


def test_is_zombie_proc_stat_false_for_malformed_text():
    assert timeshift_buffer_plugin._is_zombie_proc_stat("") is False
    assert timeshift_buffer_plugin._is_zombie_proc_stat("no parens here") is False


# ---------------------------------------------------------------------
# _classify_existing_buffer -- the pure decision core of start_buffer's
# reattach/dead-cleanup/refuse-while-stopping branches.
# ---------------------------------------------------------------------


def test_classify_existing_buffer_none_when_nothing_tracked():
    assert timeshift_buffer_plugin._classify_existing_buffer(None, is_alive=False) == "none"


def test_classify_existing_buffer_reattach_when_alive_and_not_stopping():
    assert timeshift_buffer_plugin._classify_existing_buffer({"pid": 1}, is_alive=True) == "reattach"


def test_classify_existing_buffer_dead_when_not_alive():
    assert timeshift_buffer_plugin._classify_existing_buffer({"pid": 1}, is_alive=False) == "dead"


def test_classify_existing_buffer_stopping_when_alive_and_marked_stopping():
    """The real race this fixes: a concurrent start_buffer landing while
    _teardown_buffer() is still mid-SIGTERM/SIGKILL must not reattach to
    (or start a duplicate alongside) a buffer that's already committed to
    going away."""
    assert timeshift_buffer_plugin._classify_existing_buffer({"pid": 1, "stopping": True}, is_alive=True) == "stopping"


def test_classify_existing_buffer_dead_takes_priority_over_stopping_when_not_alive():
    """A crash partway through _teardown_buffer() (after its own
    SIGTERM/SIGKILL sequence finished but before file removal/state
    delete) must still self-heal via the ordinary dead-buffer cleanup
    path, not get stuck permanently reporting "stopping" with no process
    left to ever revive it. No stopping_since here -- an old-format state
    (or one this function otherwise can't time-bound) falls back to this
    same pre-existing behavior."""
    assert timeshift_buffer_plugin._classify_existing_buffer({"pid": 1, "stopping": True}, is_alive=False) == "dead"


def test_classify_existing_buffer_stopping_wins_over_dead_within_the_grace_period_the_real_bug_this_fixes():
    """The actual race this fixes: _stop_ffmpeg() can make the process
    stop testing alive well before _teardown_buffer() actually finishes
    removing its files and deleting its own Redis state. Without
    stopping_since, this used to read as "dead" the instant the process
    exited, letting a concurrent start_buffer spawn a brand-new,
    untracked ffmpeg that the still-in-flight original teardown then
    orphaned by deleting its state out from under it."""
    now = 1000.0
    existing = {"pid": 1, "stopping": True, "stopping_since": now - 5}  # 5s into a 30s grace period
    assert timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=False, now=now) == "stopping"


def test_classify_existing_buffer_stopping_wins_over_dead_within_the_grace_period_even_while_alive():
    now = 1000.0
    existing = {"pid": 1, "stopping": True, "stopping_since": now - 5}
    assert timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=True, now=now) == "stopping"


def test_classify_existing_buffer_falls_back_to_dead_once_the_grace_period_elapses():
    """An abandoned teardown (the plugin process itself crashed, not
    just ffmpeg) still needs to self-heal eventually -- once the grace
    period passes, a not-alive buffer goes back to "dead" instead of
    staying stuck reporting "stopping" forever."""
    now = 1000.0
    existing = {"pid": 1, "stopping": True, "stopping_since": now - 31}  # just past the 30s grace period
    assert timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=False, now=now) == "dead"


def test_classify_existing_buffer_stays_stopping_past_the_grace_period_while_still_alive():
    """An unusually slow teardown (still alive well past the grace
    period) must not be treated as safe to restart alongside -- staying
    "stopping" avoids ever running two ffmpeg processes for the same
    channel at once, the same risk this whole mechanism exists to
    avoid."""
    now = 1000.0
    existing = {"pid": 1, "stopping": True, "stopping_since": now - 31}
    assert timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=True, now=now) == "stopping"


def test_classify_existing_buffer_reattach_when_config_matches_and_checked():
    existing = {"pid": 1, "http_port": 9192, "storage_path": "/data/timeshift"}
    assert (
        timeshift_buffer_plugin._classify_existing_buffer(
            existing, is_alive=True, expected_http_port=9192, expected_storage_path="/data/timeshift"
        )
        == "reattach"
    )


def test_classify_existing_buffer_stale_config_when_http_port_changed():
    # The real bug this fixes: _ensure_http_server_running() restarts
    # every worker's own file server on a settings change, but the
    # already-running ffmpeg keeps writing under whatever it was
    # actually launched with -- reattaching here would keep handing out
    # a port nothing is listening on for much longer.
    existing = {"pid": 1, "http_port": 9192, "storage_path": "/data/timeshift"}
    assert (
        timeshift_buffer_plugin._classify_existing_buffer(
            existing, is_alive=True, expected_http_port=9999, expected_storage_path="/data/timeshift"
        )
        == "stale_config"
    )


def test_classify_existing_buffer_stale_config_when_storage_path_changed():
    existing = {"pid": 1, "http_port": 9192, "storage_path": "/data/timeshift"}
    assert (
        timeshift_buffer_plugin._classify_existing_buffer(
            existing, is_alive=True, expected_http_port=9192, expected_storage_path="/mnt/new-location"
        )
        == "stale_config"
    )


def test_classify_existing_buffer_treats_a_missing_config_key_as_unknown_not_a_mismatch():
    # Old-format state (written before this field was ever recorded)
    # shouldn't get needlessly torn down just because we can't confirm a
    # match -- only a key that's actually present and disagrees counts.
    existing = {"pid": 1}  # no http_port/storage_path keys at all
    assert (
        timeshift_buffer_plugin._classify_existing_buffer(
            existing, is_alive=True, expected_http_port=9192, expected_storage_path="/data/timeshift"
        )
        == "reattach"
    )


def test_classify_existing_buffer_skips_config_check_when_not_asked_for():
    # expected_http_port/expected_storage_path default to None -- every
    # existing caller that doesn't pass them keeps working unchanged,
    # even against a state dict with no http_port/storage_path keys at
    # all (an old-format state).
    assert timeshift_buffer_plugin._classify_existing_buffer({"pid": 1}, is_alive=True) == "reattach"


def test_classify_existing_buffer_stopping_wins_over_stale_config():
    existing = {"pid": 1, "http_port": 9192, "stopping": True, "stopping_since": 995.0}
    assert (
        timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=True, now=1000.0, expected_http_port=9999)
        == "stopping"
    )


def test_teardown_buffer_marks_stopping_before_stopping_ffmpeg(monkeypatch):
    """The "stopping" marker must land in Redis before the slow part
    (_stop_ffmpeg's own SIGTERM/poll/SIGKILL sequence) starts -- that's
    the whole point, see _classify_existing_buffer()'s own "stopping"
    case."""
    order = []
    state = {"channel_uuid": _UUID, "pid": 1}

    def fake_set_buffer_state(uuid, s):
        order.append(("set_buffer_state", dict(s)))

    def fake_stop_ffmpeg(s, logger):
        order.append(("stop_ffmpeg", None))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", fake_set_buffer_state)
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", fake_stop_ffmpeg)
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: order.append(("remove", None))
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: order.append(("delete", uuid)))

    assert timeshift_buffer_plugin._teardown_buffer(state, _FakeLogger()) is True

    assert [step for step, _ in order] == ["set_buffer_state", "stop_ffmpeg", "remove", "delete"]
    assert order[0][1]["stopping"] is True
    assert isinstance(order[0][1]["stopping_since"], (int, float))


def _teardown_probe(monkeypatch, current_state):
    """Runs _teardown_buffer() with Redis stubbed to answer `current_state`
    and every side effect recorded; returns the ordered list of steps."""
    steps = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: current_state)
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: steps.append("set"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: steps.append("stop"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: steps.append("remove"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: steps.append("delete"))
    return steps


def test_teardown_buffer_proceeds_when_redis_holds_the_same_instance(monkeypatch):
    stale = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0, "last_heartbeat": 1.0}
    # Heartbeats and viewers change all the time; they don't make it a different buffer.
    current = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0, "last_heartbeat": 99.0, "viewers": ["a"]}
    steps = _teardown_probe(monkeypatch, current)

    assert timeshift_buffer_plugin._teardown_buffer(stale, _FakeLogger()) is True
    assert steps == ["set", "stop", "remove", "delete"]


def test_teardown_buffer_leaves_a_buffer_a_concurrent_start_replaced(monkeypatch):
    """The race from docs/OPEN_ITEMS.md: get_live_manifest read `stale`, took
    a few seconds building a manifest for a dead ffmpeg, and meanwhile
    start_buffer classified it dead and started a replacement. Tearing down
    from the stale copy used to kill the replacement's directory and state."""
    stale = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0}
    replacement = {"channel_uuid": _UUID, "pid": 200, "started_at": 60.0}
    steps = _teardown_probe(monkeypatch, replacement)
    logger = _FakeLogger()

    assert timeshift_buffer_plugin._teardown_buffer(stale, logger) is False
    assert steps == []  # nothing written, nothing stopped, nothing removed or deleted
    assert any(level == "warning" and "replaced" in msg for level, msg in logger.calls)


def test_teardown_buffer_treats_a_reused_pid_with_a_new_start_time_as_a_different_instance(monkeypatch):
    stale = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0}
    steps = _teardown_probe(monkeypatch, {"channel_uuid": _UUID, "pid": 100, "started_at": 60.0})

    assert timeshift_buffer_plugin._teardown_buffer(stale, _FakeLogger()) is False
    assert steps == []


def test_teardown_buffer_still_cleans_up_when_the_state_is_already_gone(monkeypatch, start_locks):
    """No state at all is not a replacement: the old instance still has an
    ffmpeg to stop and files to remove -- under the channel's start lock."""
    steps = _teardown_probe(monkeypatch, None)

    assert timeshift_buffer_plugin._teardown_buffer({"channel_uuid": _UUID, "pid": 100}, _FakeLogger()) is True
    # Nothing to mark (and nothing resurrected by writing one), but the old instance is still cleaned up.
    assert steps == ["stop", "remove", "delete"]
    assert [kind for kind, _ in start_locks.log] == ["acquire", "release"]
    assert start_locks.held == set()


def test_teardown_of_an_absent_state_leaves_a_buffer_started_in_the_meantime(monkeypatch, start_locks):
    """The fifth sweep's reproducer: Redis held nothing for the channel when the teardown read it, and a
    start_buffer finished before the teardown reached its removal -- new directory, new state. Both used
    to be removed along with the old buffer's."""
    steps = _teardown_probe(monkeypatch, None)
    new_state = {"channel_uuid": _UUID, "pid": 300, "started_at": 99.0}
    current = {"state": None}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: current["state"])
    real_acquire = start_locks.acquire

    def acquire_after_start_finished(channel_uuid):
        current["state"] = new_state  # the start_buffer that held the lock finished first
        return real_acquire(channel_uuid)

    monkeypatch.setattr(timeshift_buffer_plugin, "_acquire_start_buffer_lock", acquire_after_start_finished)
    logger = _FakeLogger()

    old = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0}
    assert timeshift_buffer_plugin._teardown_buffer(old, logger) is False
    assert steps == []  # nothing stopped, removed or deleted
    assert any(level == "warning" and "a new buffer was started" in msg for level, msg in logger.calls)
    assert start_locks.held == set()


def test_teardown_of_an_absent_state_waits_out_a_start_in_progress(monkeypatch, start_locks):
    steps = _teardown_probe(monkeypatch, None)
    start_locks.held.add(_UUID)  # a start_buffer for this channel is mid-flight

    assert timeshift_buffer_plugin._teardown_buffer({"channel_uuid": _UUID, "pid": 100}, _FakeLogger()) is False
    assert steps == []


def test_teardown_from_inside_the_start_lock_does_not_take_it_again(monkeypatch, start_locks):
    # _start_buffer_locked() tears down a stale-config buffer while it already holds the lock.
    steps = _teardown_probe(monkeypatch, None)
    start_locks.held.add(_UUID)

    assert (
        timeshift_buffer_plugin._teardown_buffer(
            {"channel_uuid": _UUID, "pid": 100}, _FakeLogger(), holds_start_lock=True
        )
        is True
    )
    assert steps == ["stop", "remove", "delete"]


def test_teardown_buffer_can_be_repeated_on_a_state_already_marked_stopping(monkeypatch):
    """A second teardown of the same instance (stop_buffer racing the reaper) is allowed through."""
    already = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0, "stopping": True, "stopping_since": 1.0}
    steps = _teardown_probe(monkeypatch, dict(already))

    assert timeshift_buffer_plugin._teardown_buffer(dict(already), _FakeLogger()) is True
    assert steps == ["set", "stop", "remove", "delete"]


# ---------------------------------------------------------------------
# _clear_channel_dir / _start_ffmpeg -- a fresh start must not inherit
# a previous instance's files
# ---------------------------------------------------------------------


def test_clear_channel_dir_removes_a_previous_instances_files(tmp_path):
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    (channel_dir / "live.m3u8").write_text("#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:5000\n")
    (channel_dir / "seg_04999.ts").write_bytes(b"old")
    (channel_dir / "ffmpeg.log").write_bytes(b"old log")

    timeshift_buffer_plugin._clear_channel_dir(channel_dir, _FakeLogger())

    assert not channel_dir.exists()


def test_clear_channel_dir_is_a_noop_when_there_is_nothing_to_clear(tmp_path):
    logger = _FakeLogger()
    timeshift_buffer_plugin._clear_channel_dir(tmp_path / _UUID, logger)

    assert not (tmp_path / _UUID).exists()
    assert logger.calls == []


def test_clear_channel_dir_survives_a_failed_removal(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    logger = _FakeLogger()

    def _boom(path):
        raise OSError("permission denied")

    monkeypatch.setattr(timeshift_buffer_plugin.shutil, "rmtree", _boom)

    timeshift_buffer_plugin._clear_channel_dir(channel_dir, logger)  # must not raise

    assert any(level == "exception" for level, _ in logger.calls)


class _FakeProc:
    pid = 4242


def test_start_ffmpeg_does_not_inherit_a_leftover_playlist_or_segments(tmp_path, monkeypatch):
    """The bug from docs/OPEN_ITEMS.md, end to end: a directory left behind
    (container restart wiped Redis, storage volume survived) held a stale
    playlist whose sequence numbers were far above the new instance's."""
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    (channel_dir / "live.m3u8").write_text("#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:5000\n")
    (channel_dir / "seg_04999.ts").write_bytes(b"stale")
    (channel_dir / "ffmpeg.log").write_bytes(b"stale log\n")
    seen = {}

    def fake_popen(cmd, **kwargs):
        # What ffmpeg would find when it starts: the directory as it is at spawn time.
        seen["cwd"] = kwargs["cwd"]
        seen["files"] = sorted(p.name for p in Path(kwargs["cwd"]).iterdir())
        return _FakeProc()

    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", fake_popen)

    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert seen["cwd"] == str(channel_dir)
    assert seen["files"] == ["ffmpeg.log"]  # only the fresh log opened for the new process
    assert not (channel_dir / "live.m3u8").exists()
    assert not (channel_dir / "seg_04999.ts").exists()
    assert (channel_dir / "ffmpeg.log").read_bytes() == b""
    assert state["pid"] == 4242


def test_start_ffmpeg_gives_a_leftover_directory_a_fresh_mtime(tmp_path, monkeypatch):
    """A leftover directory kept its old mtime until the first segment closed, so a
    reaper tick could rmtree it as a 300s-old orphan out from under a buffer that was
    only just starting."""
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir()
    (channel_dir / "seg_00001.ts").write_bytes(b"stale")
    old = time.time() - 3600
    os.utime(channel_dir, (old, old))
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())

    timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert time.time() - channel_dir.stat().st_mtime < 60
    # ...so the reaper's orphan scan (min age 300s) leaves it alone.
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    assert timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), 300) == []


def test_start_ffmpeg_works_when_there_is_no_leftover(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())

    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert (tmp_path / _UUID).is_dir()
    assert state["channel_uuid"] == _UUID


# ---------------------------------------------------------------------
# _create_http_server -- dual-stack where the host has IPv6
# ---------------------------------------------------------------------


def _has_ipv6_loopback():
    import socket

    if not socket.has_ipv6:
        return False
    try:
        with socket.socket(socket.AF_INET6, socket.SOCK_STREAM) as probe:
            probe.bind(("::1", 0))
        return True
    except OSError:
        return False


def _serve_once(server):
    import threading

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return thread


def _http_status(host, port, family):
    import socket

    with socket.socket(family, socket.SOCK_STREAM) as conn:
        conn.settimeout(5)
        conn.connect((host, port))
        conn.sendall(b"GET /nothing HTTP/1.0\r\nHost: x\r\n\r\n")
        return conn.recv(64).split(b"\r\n", 1)[0]


@pytest.mark.skipif(not _has_ipv6_loopback(), reason="no IPv6 loopback on this machine")
def test_create_http_server_answers_on_both_ipv4_and_ipv6():
    import socket

    server, bound = timeshift_buffer_plugin._create_http_server(0, _FakeLogger())
    try:
        assert server is not None
        assert "IPv4 and IPv6" in bound
        port = server.server_address[1]
        server.storage_path = "/nonexistent"
        server.plugin_logger = _FakeLogger()
        _serve_once(server)

        assert _http_status("::1", port, socket.AF_INET6).startswith(b"HTTP/1.")
        assert _http_status("127.0.0.1", port, socket.AF_INET).startswith(b"HTTP/1.")
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()


def test_create_http_server_falls_back_to_ipv4_when_there_is_no_ipv6(monkeypatch):
    """A container with IPv6 disabled has no `::` to bind -- the common Docker case."""

    def no_v6(*args, **kwargs):
        raise OSError(97, "Address family not supported by protocol")

    monkeypatch.setattr(timeshift_buffer_plugin, "_BufferHTTPServerV6", no_v6)
    logger = _FakeLogger()

    server, bound = timeshift_buffer_plugin._create_http_server(0, logger)
    try:
        assert server is not None
        assert bound.startswith("0.0.0.0:") and "IPv4 only" in bound
        assert any(level == "info" and "IPv4 only" in msg for level, msg in logger.calls)
    finally:
        if server is not None:
            server.server_close()


def test_create_http_server_reports_a_port_that_is_taken_once(monkeypatch):
    import socket

    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    holder.bind(("0.0.0.0", 0))
    holder.listen(1)
    port = holder.getsockname()[1]
    logger = _FakeLogger()
    try:
        # No SO_REUSEPORT on the holder, so neither the v6 attempt nor the v4 one can share the port.
        server, bound = timeshift_buffer_plugin._create_http_server(port, logger)
        assert server is None and bound is None
        assert sum(1 for level, msg in logger.calls if level == "error" and "couldn't bind" in msg) == 1
    finally:
        holder.close()


# ---------------------------------------------------------------------
# _stop_ffmpeg -- monkeypatch os.killpg/time.time/time.sleep, same
# technique as _is_process_alive above, so the real SIGTERM/poll/SIGKILL
# sequence runs without any actual signaling or wall-clock delay.
# ---------------------------------------------------------------------


def test_stop_ffmpeg_does_nothing_without_a_pid(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))
    timeshift_buffer_plugin._stop_ffmpeg({}, _FakeLogger())
    assert calls == []


def test_stop_ffmpeg_returns_without_polling_when_sigterm_raises_unexpectedly(monkeypatch):
    """A non-ProcessLookupError from the initial SIGTERM itself (e.g. a
    permissions problem) logs and bails out immediately -- never enters
    the poll loop at all."""

    def raise_permission_error(pid, sig):
        raise PermissionError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", raise_permission_error)
    monkeypatch.setattr(
        timeshift_buffer_plugin.time, "time", lambda: (_ for _ in ()).throw(AssertionError("should not poll"))
    )

    logger = _FakeLogger()
    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, logger)
    assert any(level == "exception" for level, _ in logger.calls)


def test_stop_ffmpeg_returns_promptly_when_process_exits_after_sigterm(monkeypatch):
    """The common case: ffmpeg exits cleanly on SIGTERM well within the
    poll deadline -- must return as soon as the poll sees it gone,
    without ever escalating to SIGKILL."""
    calls = []

    def fake_killpg(pid, sig):
        calls.append((pid, sig))
        if sig == 0:
            raise ProcessLookupError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", fake_killpg)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)  # never crosses the deadline itself

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, _FakeLogger())

    assert calls == [(1234, timeshift_buffer_plugin.signal.SIGTERM), (1234, 0)]


def test_stop_ffmpeg_treats_a_zombie_as_exited_instead_of_waiting_out_the_full_deadline(monkeypatch):
    """The real bug this fixes: a bare os.killpg(pid, 0) check can't tell
    a zombie (already exited, not yet reaped -- this poll's own worker
    isn't generally ffmpeg's real parent) from a genuinely still-running
    process, since signal 0 succeeds against either. The poll used to
    wait out the *entire* 2s deadline and send an unnecessary SIGKILL
    every single time a zombie was involved, regardless of how quickly
    ffmpeg actually exited -- very likely the actual cause behind
    docs/TIMESHIFT.md's own "A plain Stop took ~5s" investigation, which
    attributed the delay to ffmpeg's own slow SIGTERM response instead.
    _is_process_alive() (already zombie-aware, see its own tests) is
    what the poll must go through instead of a bare os.killpg()."""
    killpg_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: killpg_calls.append((pid, sig)))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: False)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)  # never crosses the deadline itself

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, _FakeLogger())

    # Only the initial SIGTERM -- the poll's own liveness check went
    # through _is_process_alive() (stubbed above to report "not alive"
    # immediately), never a second bare os.killpg() call, and SIGKILL was
    # never reached.
    assert killpg_calls == [(1234, timeshift_buffer_plugin.signal.SIGTERM)]


def test_stop_ffmpeg_escalates_to_sigkill_once_the_deadline_passes(monkeypatch):
    """Confirmed live (see _stop_ffmpeg's own comment): ffmpeg can take
    close to the full deadline to exit on SIGTERM alone with no error --
    escalating to SIGKILL once the deadline passes is safe for this
    stream-copy pipeline specifically."""
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))

    # First call (the `start` timestamp) returns 0; every call afterward
    # (the while-loop condition check) returns far past the 2s deadline,
    # so the poll loop body never runs at all -- this exercises the
    # escalation path without simulating real elapsed time.
    times = iter([0.0, 100.0])
    monkeypatch.setattr(timeshift_buffer_plugin.time, "monotonic", lambda: next(times, 100.0))
    monkeypatch.setattr(timeshift_buffer_plugin.time, "sleep", lambda s: (_ for _ in ()).throw(AssertionError))

    logger = _FakeLogger()
    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, logger)

    assert calls == [(1234, timeshift_buffer_plugin.signal.SIGTERM), (1234, timeshift_buffer_plugin.signal.SIGKILL)]
    assert any(level == "warning" for level, _ in logger.calls)


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


def test_find_orphaned_channel_dirs_ignores_a_non_uuid_directory(tmp_path, monkeypatch):
    """Regression test for a real, confirmed, high-severity bug: this
    scan used to treat *any* old, untracked directory under storage_path
    as an orphan, with no check that its name is actually one of the
    UUID-named channel directories this plugin creates -- a user
    pointing storage_path at real, shared, persistent storage (this
    project's own README explicitly encourages persistent storage) could
    have an unrelated directory recursively deleted by the reaper, fully
    unattended. A non-UUID directory name must never be treated as an
    orphan candidate at all, regardless of age or Redis state."""
    unrelated = tmp_path / "recordings"
    unrelated.mkdir()
    old_mtime = time.time() - 3600
    os.utime(unrelated, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert orphans == []


def test_find_orphaned_channel_dirs_still_finds_a_real_uuid_orphan_alongside_a_non_uuid_dir(tmp_path, monkeypatch):
    """The fix above must not over-broadly reject every directory -- a
    genuine orphaned buffer directory sitting next to an unrelated,
    non-UUID one must still be found."""
    unrelated = tmp_path / "recordings"
    unrelated.mkdir()
    orphan = tmp_path / "55555555-5555-5555-5555-555555555555"
    orphan.mkdir()
    old_mtime = time.time() - 3600
    os.utime(unrelated, (old_mtime, old_mtime))
    os.utime(orphan, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert [p.name for p in orphans] == [orphan.name]


def test_find_orphaned_channel_dirs_ignores_a_non_canonical_uuid_directory(tmp_path, monkeypatch):
    """Tightened beyond plain uuid.UUID() parsing (which is lenient --
    accepts 32-hex-no-hyphens, mixed case, etc.): only the exact
    canonical, lowercase, hyphenated form this plugin itself ever
    creates via _channel_dir() is treated as one of its own directories.
    A 32-hex-no-hyphens name (uuid.UUID()-parseable, but not what this
    plugin ever writes) must not be mistaken for one."""
    non_canonical = tmp_path / "55555555555555555555555555555555"[:32]  # 32 hex chars, no hyphens
    non_canonical.mkdir()
    old_mtime = time.time() - 3600
    os.utime(non_canonical, (old_mtime, old_mtime))

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=60)
    assert orphans == []


# ---------------------------------------------------------------------
# _is_canonical_uuid
# ---------------------------------------------------------------------


def test_is_canonical_uuid_true_for_canonical_form():
    assert timeshift_buffer_plugin._is_canonical_uuid("55555555-5555-5555-5555-555555555555") is True


def test_is_canonical_uuid_false_for_hyphen_less_hex():
    assert timeshift_buffer_plugin._is_canonical_uuid("55555555555555555555555555555555") is False


def test_is_canonical_uuid_false_for_uppercase():
    assert timeshift_buffer_plugin._is_canonical_uuid("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa".upper()) is False


def test_is_canonical_uuid_false_for_non_uuid_text():
    assert timeshift_buffer_plugin._is_canonical_uuid("recordings") is False
    assert timeshift_buffer_plugin._is_canonical_uuid("") is False


class _StartLocks:
    """Stands in for the per-channel start lock (Redis SET NX): `held` is the set of channels whose
    lock someone else holds; `log` records acquire/release in order."""

    def __init__(self):
        self.held = set()
        self.log = []

    def acquire(self, channel_uuid):
        if channel_uuid in self.held:
            self.log.append(("busy", channel_uuid))
            return None
        self.held.add(channel_uuid)
        self.log.append(("acquire", channel_uuid))
        return "token-" + channel_uuid

    def release(self, channel_uuid, token):
        assert token == "token-" + channel_uuid
        self.held.discard(channel_uuid)
        self.log.append(("release", channel_uuid))


@pytest.fixture()
def start_locks(monkeypatch):
    locks = _StartLocks()
    monkeypatch.setattr(timeshift_buffer_plugin, "_acquire_start_buffer_lock", locks.acquire)
    monkeypatch.setattr(timeshift_buffer_plugin, "_release_start_buffer_lock", locks.release)
    return locks


def test_scrub_orphaned_dirs_removes_and_reports(tmp_path, monkeypatch, start_locks):
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
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: False)
    state = _make_state(tmp_path, pid=99999)
    with pytest.raises(timeshift_buffer_plugin.BufferFailedError):
        timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())


def test_get_live_manifest_no_playlist_process_alive(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    state = _make_state(tmp_path, pid=os.getpid())
    with pytest.raises(RuntimeError) as exc_info:
        timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert not isinstance(exc_info.value, timeshift_buffer_plugin.BufferFailedError)


def test_get_live_manifest_returns_the_frozen_manifest_flagged_ended_when_ffmpeg_died_after_producing_segments(
    tmp_path, monkeypatch
):
    """ffmpeg can die well *after* writing its first segment (an upstream drop, a provider-side
    concurrent-stream limit), leaving a valid playlist and every segment on disk that will just
    never gain another. That used to raise BufferFailedError, whose handler tears the whole
    buffer down -- wiping the rewind window of a viewer paused or rewound behind live, who is
    still entitled to play it. Now the frozen manifest comes back, flagged `ended`."""
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir, media_sequence=0, segments=[("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 20, "2.0")]
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: False)
    state = _make_state(tmp_path, pid=99999)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())

    assert manifest["ended"] is True
    assert [seg["filename"] for seg in manifest["segments"]] == ["seg0.ts", "seg1.ts"]
    assert manifest["total_bytes"] == 30


def test_get_live_manifest_is_not_ended_while_ffmpeg_is_running(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, media_sequence=0, segments=[("seg0.ts", b"a" * 10, "2.0")])
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)

    manifest = timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path, pid=99999), _FakeLogger())

    assert manifest["ended"] is False


def test_get_live_manifest_still_fails_fast_when_ffmpeg_died_before_producing_anything(tmp_path, monkeypatch):
    """Unchanged: with no playlist at all there is nothing to serve, and waiting is pointless."""
    (tmp_path / _UUID).mkdir()
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: False)

    with pytest.raises(timeshift_buffer_plugin.BufferFailedError):
        timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path, pid=99999), _FakeLogger())


def test_get_live_manifest_does_not_check_liveness_without_a_tracked_pid(tmp_path):
    """A missing/falsy pid (older plugin-version state, or a caller that
    never recorded one) can't be confirmed dead -- left alone rather than
    treated as fatal, the same conservative bias _is_process_alive()
    itself already applies to its own inconclusive cases."""
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, media_sequence=0, segments=[("seg0.ts", b"a" * 10, "2.0")])
    state = _make_state(tmp_path, pid=None)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert manifest["segments"][0]["filename"] == "seg0.ts"


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


def test_get_live_manifest_extinf_with_a_title_after_the_comma_still_parses_duration(tmp_path):
    """#EXTINF:<duration>,<title> is valid HLS syntax even though
    ffmpeg's own segment muxer (this file's only realistic producer)
    never actually writes a title here -- a parser reading generated
    content shouldn't assume the comma is always trailing with nothing
    after it. Regression test for a real, if narrow, gap: an earlier
    rstrip(",")-only parse left a title attached, silently defaulting
    to duration 0 instead of reading the duration before the comma."""
    channel_dir = tmp_path / _UUID
    channel_dir.mkdir(parents=True)
    (channel_dir / "seg0.ts").write_bytes(b"x" * 10)
    (channel_dir / "live.m3u8").write_text("#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:6.0,some title\nseg0.ts\n")
    state = _make_state(tmp_path)

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert manifest["segments"][0]["duration_ms"] == 6000


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
    """If the playlist file's own mtime/size haven't changed at all, a
    non-newest cached entry is trusted outright -- no re-stat. The
    newest (last-listed) entry is still always freshly re-stat'd even on
    this fast, nothing-changed path -- a real bug fixed alongside this
    test (found via a project-wide review): under ffmpeg's own normal
    one-segment-at-a-time behavior, a segment is "newest" for exactly
    the one call where it first appears, then immediately demoted on
    every call after that -- without this, it would only ever get the
    single earliest, highest-risk sample this whole mechanism exists to
    double-check, with no actual second look ever happening."""
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=0,
        segments=[("seg0.ts", b"a" * 50, "2.0"), ("seg1.ts", b"b" * 60, "2.0")],
    )
    state = _make_state(tmp_path)

    first = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert first["segments"][0]["byte_size"] == 50
    assert first["segments"][1]["byte_size"] == 60

    (channel_dir / "seg0.ts").write_bytes(b"a" * 999)  # non-newest, playlist untouched
    (channel_dir / "seg1.ts").write_bytes(b"b" * 888)  # newest, playlist untouched

    second = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert second["segments"][0]["byte_size"] == 50  # non-newest: still the stale cached value
    assert second["segments"][1]["byte_size"] == 888  # newest: freshly re-stat'd despite the fast path


def test_get_live_manifest_drops_newest_segment_recycled_between_listing_and_the_fast_path_restat(tmp_path):
    """The newest-segment re-stat this fast path now also performs can
    itself race the segment being recycled (ffmpeg's own -segment_wrap)
    -- dropped rather than failing the whole manifest, same handling the
    "playlist changed" branch already gives its own newest-segment stat
    failure."""
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=0,
        segments=[("seg0.ts", b"a" * 50, "2.0"), ("seg1.ts", b"b" * 60, "2.0")],
    )
    state = _make_state(tmp_path)

    first = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["filename"] for s in first["segments"]] == ["seg0.ts", "seg1.ts"]

    (channel_dir / "seg1.ts").unlink()  # newest recycled, playlist untouched

    second = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["filename"] for s in second["segments"]] == ["seg0.ts"]


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


def test_resolve_channel_uuid_ignores_surrounding_whitespace():
    # A value pasted into the test_channel_uuid setting often has a trailing
    # newline or space; it used to be refused as if no channel was given.
    params = {"channel_uuid": f"  {_UUID}\n"}
    assert timeshift_buffer_plugin.Plugin._resolve_channel_uuid(params, {}) == _UUID
    settings = {"test_channel_uuid": f"{_UUID} "}
    assert timeshift_buffer_plugin.Plugin._resolve_channel_uuid({}, settings) == _UUID


def test_resolve_channel_uuid_refuses_a_whitespace_only_value():
    assert timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": "  \n"}, {}) is None


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


def test_resolve_channel_uuid_canonicalizes_a_non_canonical_but_equivalent_value():
    """Regression test for a real gap: uuid.UUID() is deliberately
    lenient (uppercase, {braced}, hyphen-less forms all parse), but
    returning the caller-supplied value verbatim meant the same real
    channel could resolve to a different _channel_dir() path / Redis key
    depending on which equivalent-but-differently-formatted UUID string
    happened to be supplied -- fragmenting one channel's buffer state
    across what this plugin would otherwise treat as separate channels."""
    canonical = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa"

    uppercase = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": canonical.upper()}, {})
    assert uppercase == canonical

    hyphen_less = timeshift_buffer_plugin.Plugin._resolve_channel_uuid({"channel_uuid": canonical.replace("-", "")}, {})
    assert hyphen_less == canonical


# ---------------------------------------------------------------------
# Plugin.run() dispatch -- every action handler, via monkeypatching the
# module-level Redis-touching and process-management functions each one
# calls, the same technique recording_edl's own Plugin.run() tests use.
# run() itself unconditionally calls _ensure_http_server_running()/
# _ensure_reaper_running() before ever dispatching, so _run() below
# always stubs those to no-ops first.
# ---------------------------------------------------------------------

_OTHER_UUID = "22222222-2222-2222-2222-222222222222"


def _run(monkeypatch, action, params, settings_overrides, tmp_path, *, acquire_lock=None, release_lock=None):
    monkeypatch.setattr(timeshift_buffer_plugin, "_ensure_http_server_running", lambda *a, **k: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_ensure_reaper_running", lambda *a, **k: None)
    # start_buffer's own per-channel lock (see _acquire_start_buffer_lock()'s
    # own comment) -- stubbed to always-succeeds/no-op by default so every
    # existing start_buffer test doesn't also need to mock Redis lock
    # acquisition just to reach the logic it's actually testing.
    # acquire_lock/release_lock (keyword-only params, not a plain
    # monkeypatch.setattr the caller does itself before calling _run())
    # are how the lock's own dedicated tests below override this default
    # -- setattr-ing *after* this function's own calls would just get
    # clobbered right back by them, since whichever setattr for a given
    # attribute runs last always wins regardless of which caller's own
    # source line it's written on.
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_acquire_start_buffer_lock", acquire_lock or (lambda channel_uuid: "test-token")
    )
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_release_start_buffer_lock", release_lock or (lambda channel_uuid, token: None)
    )
    settings_dict = {"storage_path": str(tmp_path), **settings_overrides}
    logger = _FakeLogger()
    result = timeshift_buffer_plugin.Plugin().run(action, params, {"logger": logger, "settings": settings_dict})
    return result, logger


def test_run_unknown_action_returns_error(monkeypatch, tmp_path):
    result, _logger = _run(monkeypatch, "not_a_real_action", {}, {}, tmp_path)
    assert result == {"status": "error", "message": "Unknown action: not_a_real_action"}


def test_run_survives_a_storage_path_that_cannot_be_created(monkeypatch, tmp_path):
    # A plain file at the exact storage_path location: Path.mkdir(exist_ok=True)
    # still raises FileExistsError, since the existing entry isn't a
    # directory -- the same class of "one bad setting blocks every action,
    # including the recovery ones" bug the earlier _int_setting()/
    # _str_setting() fixes address. stop_all doesn't need storage_path to
    # exist at all, so it must still succeed.
    bad_path = tmp_path / "not_a_directory"
    bad_path.write_text("i am a file, not a directory")
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter([]))

    result, logger = _run(monkeypatch, "stop_all", {}, {"storage_path": str(bad_path)}, tmp_path)

    assert result == {"status": "ok", "message": "No buffers were running", "stopped": []}
    assert any(level == "warning" and "couldn't create storage_path" in msg for level, msg in logger.calls)


# -- start_buffer --------------------------------------------------------


def test_run_start_buffer_requires_channel_uuid(monkeypatch, tmp_path):
    result, _logger = _run(monkeypatch, "start_buffer", {}, {}, tmp_path)
    assert result["status"] == "error"
    assert "channel_uuid is required" in result["message"]


def test_run_start_buffer_reattaches_to_running_buffer(monkeypatch, tmp_path):
    existing_state = {
        "channel_uuid": _UUID,
        "pid": 12345,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "access_token": "tok123",
        "viewers": [],
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["already_running"] is True
    assert result["access_token"] == "tok123"
    assert result["http_port"] == 9192
    assert "Reattached" in result["message"]
    assert saved  # state was persisted (heartbeat refresh)


def test_run_start_buffer_registers_new_viewer_on_reattach(monkeypatch, tmp_path):
    existing_state = {
        "channel_uuid": _UUID,
        "pid": 1,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "access_token": "tok",
        "viewers": ["viewer-a"],
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": "viewer-b"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert set(saved["viewers"]) == {"viewer-a", "viewer-b"}
    assert "viewer-b" in saved["viewer_heartbeats"]
    assert "(2 viewer(s))" in result["message"]


def test_run_start_buffer_retrofits_missing_access_token(monkeypatch, tmp_path):
    """State written by a plugin version older than the access-token
    requirement (no access_token key at all) self-heals on the next
    start_buffer rather than staying permanently unreachable."""
    existing_state = {
        "channel_uuid": _UUID,
        "pid": 1,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "viewers": [],
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["access_token"]
    assert saved["access_token"] == result["access_token"]


def test_run_start_buffer_refuses_to_reattach_while_stopping(monkeypatch, tmp_path):
    """The real race this fixes: a concurrent start_buffer landing while
    _teardown_buffer() is still mid-SIGTERM/SIGKILL (state marked
    "stopping", process still testing alive) must refuse rather than
    reattach to -- or start a duplicate alongside -- a buffer that's
    about to have its files removed and state deleted out from under
    it."""
    stopping_state = {
        "channel_uuid": _UUID,
        "pid": 12345,
        "http_port": 9192,
        "stopping": True,
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(stopping_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    removed = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda state, logger: removed.append(state))
    started = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: started.append(1) or {})

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "error"
    assert result["retryable"] is True
    assert "stopping" in result["message"]
    assert removed == []
    assert started == []


def test_run_start_buffer_cleans_up_dead_buffer_and_starts_fresh(monkeypatch, tmp_path):
    """Confirmed live this matters: a buffer whose ffmpeg already died
    must not be treated as "existing" forever -- start_buffer cleans up
    its stale state and starts genuinely fresh instead of reattaching."""
    dead_state = {"channel_uuid": _UUID, "pid": 999, "http_port": 9192}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(dead_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: False)
    removed = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda state, logger: removed.append(state))
    deleted = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: deleted.append(uuid))
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    fake_new_state = {
        "channel_uuid": _UUID,
        "pid": 5555,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(fake_new_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["already_running"] is False
    assert len(removed) == 1
    assert deleted == [_UUID]


def test_run_start_buffer_tears_down_and_restarts_a_buffer_with_a_stale_http_port(monkeypatch, tmp_path):
    """The real bug this fixes: a settings change to http_port left an
    already-running buffer's own stored port stale -- reattaching would
    keep handing callers a port nothing (eventually) listens on anymore.
    Unlike a dead buffer, the process is still alive, so this must go
    through a real teardown (stop ffmpeg, remove its old files, delete
    its state), not just a state cleanup, before starting fresh."""
    stale_state = {"channel_uuid": _UUID, "pid": 12345, "http_port": 9192, "storage_path": str(tmp_path)}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(stale_state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    torn_down = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda state, logger, **kw: torn_down.append(state)
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    fake_new_state = {
        "channel_uuid": _UUID,
        "pid": 5555,
        "http_port": 9999,
        "storage_path": str(tmp_path),
        "playlist_route": f"/{_UUID}/live.m3u8",
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(fake_new_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {"http_port": 9999}, tmp_path)

    assert result["status"] == "ok"
    assert result["already_running"] is False
    assert result["http_port"] == 9999
    assert len(torn_down) == 1
    assert torn_down[0]["channel_uuid"] == _UUID


def test_run_start_buffer_rejects_when_at_max_concurrent(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: ["k1", "k2"])

    result, _logger = _run(
        monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {"max_concurrent_buffers": 2}, tmp_path
    )

    assert result["status"] == "error"
    assert "max_concurrent_buffers" in result["message"]


def test_run_start_buffer_reports_missing_ffmpeg(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])

    def _raise_not_found(*a, **k):
        raise FileNotFoundError()

    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", _raise_not_found)

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "message": "ffmpeg not found in this container"}


def test_run_start_buffer_reports_generic_start_failure(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])

    def _raise_runtime(*a, **k):
        raise RuntimeError("disk full")

    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", _raise_runtime)

    result, logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "message": "disk full"}
    assert any(level == "exception" for level, _msg in logger.calls)


def test_run_start_buffer_starts_fresh_buffer(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    fake_state = {
        "channel_uuid": _UUID,
        "pid": 123,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(fake_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["already_running"] is False
    assert result["access_token"]
    assert saved["viewers"] == ["v1"]
    assert "v1" in saved["viewer_heartbeats"]


def test_run_start_buffer_refuses_when_another_call_already_holds_the_lock(monkeypatch, tmp_path):
    """Fixes a real, live-confirmed race (see docs/OPEN_ITEMS.md's own
    entry): without the lock this test overrides _run()'s own default
    stub to simulate contention for, two near-simultaneous start_buffer
    calls for the same channel could each spawn their own ffmpeg
    process, leaving one permanently orphaned."""
    get_buffer_state_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: get_buffer_state_calls.append(uuid))

    result, _logger = _run(
        monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path, acquire_lock=lambda channel_uuid: None
    )

    assert result == {
        "status": "error",
        "retryable": True,
        "message": "Another start_buffer call for this channel is already in progress -- retry in a moment",
    }
    # Never even reached the classify-then-spawn logic -- refused before
    # the first Redis read, not just before the eventual ffmpeg spawn.
    assert get_buffer_state_calls == []


def test_run_start_buffer_releases_the_lock_even_when_the_locked_call_errors(monkeypatch, tmp_path):
    """The finally block around _start_buffer_locked() must release the
    lock on every path, not just a successful one -- otherwise a single
    failed call (ffmpeg missing, a generic exception, ...) would leave
    this channel permanently unable to start a buffer until the lock's
    own TTL expired."""
    release_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])

    def _raise(*_a, **_k):
        raise RuntimeError("simulated failure")

    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", _raise)

    result, _logger = _run(
        monkeypatch,
        "start_buffer",
        {"channel_uuid": _UUID},
        {},
        tmp_path,
        acquire_lock=lambda channel_uuid: "tok",
        release_lock=lambda channel_uuid, token: release_calls.append((channel_uuid, token)),
    )

    assert result["status"] == "error"
    # the count-and-register slot lock (released first, it is the inner one) and the channel lock
    assert release_calls == [(timeshift_buffer_plugin._START_SLOT_LOCK_ID, "tok"), (_UUID, "tok")]


# -- stop_buffer -----------------------------------------------------------


def test_run_stop_buffer_requires_channel_uuid(monkeypatch, tmp_path):
    result, _logger = _run(monkeypatch, "stop_buffer", {}, {}, tmp_path)
    assert result["status"] == "error"
    assert "channel_uuid is required" in result["message"]


def test_run_stop_buffer_no_buffer_running(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "ok", "message": "no buffer was running"}


def test_run_stop_buffer_removes_one_of_several_viewers(monkeypatch, tmp_path):
    """Reference-counted stop: confirmed live this matters -- an
    unconditional stop on every Close() used to kill a second viewer's
    still-active buffer the moment a first viewer also stopped watching."""
    now = time.time()
    state = {"channel_uuid": _UUID, "viewers": ["v1", "v2"], "viewer_heartbeats": {"v1": now, "v2": now}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update(s))

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert "still active for other viewers" in result["message"]
    assert result["remaining_viewers"] == 1
    assert saved["viewers"] == ["v2"]


def test_run_stop_buffer_tears_down_when_last_viewer_leaves(monkeypatch, tmp_path):
    state = {"channel_uuid": _UUID, "viewers": ["v1"], "viewer_heartbeats": {"v1": 100}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: None)
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)

    assert result == {"status": "ok", "message": "Buffer stopped"}
    assert teardown_calls == [_UUID]


def test_run_stop_buffer_unconditional_stop_without_viewer_id(monkeypatch, tmp_path):
    """A caller with no viewer_id (an older client, or a manual test
    button) can't be reference-counted at all, so it always falls
    through to an unconditional stop."""
    state = {"channel_uuid": _UUID, "viewers": ["v1", "v2"]}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "ok", "message": "Buffer stopped"}
    assert teardown_calls == [_UUID]


# -- heartbeat ---------------------------------------------------------


def test_run_heartbeat_requires_channel_uuid(monkeypatch, tmp_path):
    result, _logger = _run(monkeypatch, "heartbeat", {}, {}, tmp_path)
    assert result["status"] == "error"
    assert "channel_uuid is required" in result["message"]


def test_run_heartbeat_no_buffer_running(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    result, _logger = _run(monkeypatch, "heartbeat", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "message": "no buffer running for this channel"}


def test_run_heartbeat_refreshes_buffer_and_known_viewer(monkeypatch, tmp_path):
    state = {"channel_uuid": _UUID, "viewers": ["v1"], "last_heartbeat": 0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update(s))

    result, _logger = _run(monkeypatch, "heartbeat", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)

    assert result == {"status": "ok", "message": f"Heartbeat refreshed for channel {_UUID}"}
    assert saved["last_heartbeat"] > 0
    assert "v1" in saved.get("viewer_heartbeats", {})


def test_run_heartbeat_re_adds_an_unregistered_viewer_id(monkeypatch, tmp_path):
    """A viewer_id not in the buffer's viewers (e.g. one the reaper
    pruned while still watching) is re-registered by its own heartbeat."""
    state = {"channel_uuid": _UUID, "viewers": ["v1"], "last_heartbeat": 0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update(s))

    result, _logger = _run(monkeypatch, "heartbeat", {"channel_uuid": _UUID, "viewer_id": "unknown"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert saved["viewers"] == ["v1", "unknown"]
    assert "unknown" in saved["viewer_heartbeats"]


# -- get_live_manifest -------------------------------------------------


def test_run_get_live_manifest_requires_channel_uuid(monkeypatch, tmp_path):
    result, _logger = _run(monkeypatch, "get_live_manifest", {}, {}, tmp_path)
    assert result["status"] == "error"
    assert "channel_uuid is required" in result["message"]


def test_run_get_live_manifest_no_buffer_running(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "error"
    assert "call start_buffer first" in result["message"]
    # fatal: true here too, not just the BufferFailedError case below --
    # by the time a caller is polling get_live_manifest, start_buffer
    # already wrote this state synchronously, so its absence means the
    # buffer was deliberately torn down, not a startup race; nothing will
    # resurrect it on its own.
    assert result["fatal"] is True


def test_run_get_live_manifest_fatal_buffer_failure_tears_down(monkeypatch, tmp_path):
    """fatal: true is what lets a caller (pvr.dispatcharr-unofficial's own
    cold-start retry loop) stop retrying immediately instead of waiting
    out its full budget against a buffer that will never recover --
    also self-heals here rather than waiting for a future start_buffer."""
    state = {"channel_uuid": _UUID}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))

    def _raise_fatal(*a, **k):
        raise timeshift_buffer_plugin.BufferFailedError("ffmpeg exited")

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", _raise_fatal)
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "fatal": True, "message": "ffmpeg exited"}
    assert teardown_calls == [_UUID]


def test_run_get_live_manifest_for_an_ended_buffer_serves_the_manifest_and_does_not_tear_it_down(monkeypatch, tmp_path):
    """The whole point of the change: a dead ffmpeg with content already on disk is reported
    (`ended`), not destroyed, so a viewer paused or rewound behind live keeps their buffer."""
    state = {"channel_uuid": _UUID, "http_port": 9192, "viewers": [], "viewer_heartbeats": {}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    manifest = {
        "media_sequence": 0,
        "segments": [
            {
                "filename": "seg0.ts",
                "sequence": 0,
                "byte_offset": 0,
                "byte_size": 10,
                "time_offset_ms": 0,
                "duration_ms": 2000,
            }
        ],
        "total_bytes": 10,
        "total_duration_ms": 2000,
        "ended": True,
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", lambda s, logger: manifest)
    monkeypatch.setattr(timeshift_buffer_plugin, "_update_buffer_state", lambda uuid, mutate: ("updated", state))
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["ended"] is True
    assert "no further segments" in result["message"]
    assert teardown_calls == []


def test_run_get_live_manifest_plain_runtime_error_does_not_tear_down(monkeypatch, tmp_path):
    """Distinguished from BufferFailedError above: a buffer that's just
    still cold-starting shouldn't be torn down on a retryable error."""
    state = {"channel_uuid": _UUID}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))

    def _raise_runtime(*a, **k):
        raise RuntimeError("live playlist not found")

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", _raise_runtime)
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "message": "live playlist not found"}
    assert teardown_calls == []


def test_run_get_live_manifest_success_message_and_heartbeat(monkeypatch, tmp_path):
    state = {"channel_uuid": _UUID, "http_port": 9192, "last_heartbeat": 0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    fake_manifest = {
        "media_sequence": 0,
        "segments": [{"filename": "seg0.ts"}, {"filename": "seg1.ts"}],
        "total_bytes": 12345,
        "total_duration_ms": 4000,
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", lambda s, logger: fake_manifest)
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update(s))

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["http_port"] == 9192
    assert result["segment_route_prefix"] == f"/{_UUID}/"
    assert result["total_bytes"] == 12345
    assert "2 segment(s)" in result["message"]
    assert "12345 bytes" in result["message"]
    assert "4.0s buffered" in result["message"]
    assert saved["last_heartbeat"] > 0


def test_run_get_live_manifest_does_not_clobber_a_viewer_registered_during_the_build(monkeypatch, tmp_path):
    """Regression test for a real, confirmed race (see _apply_heartbeat's
    own docstring): _get_live_manifest() can take a while (up to
    ~1,800 stat() calls on a cold worker), long enough for a concurrent
    start_buffer to register a new viewer in Redis before this handler's
    own write-back. Simulates that interleaving by mutating the shared,
    in-memory "Redis" store from inside the mocked _get_live_manifest()
    call itself -- the same technique a real concurrent request would
    produce, just made deterministic."""
    store = {_UUID: {"channel_uuid": _UUID, "http_port": 9192, "last_heartbeat": 0, "viewers": ["a"]}}
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))

    def racing_manifest_build(_state, _logger):
        # A concurrent start_buffer registers viewer "b" while this
        # (slow) call is still in progress.
        store[_UUID]["viewers"] = ["a", "b"]
        return {"media_sequence": 0, "segments": [], "total_bytes": 0, "total_duration_ms": 0}

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", racing_manifest_build)

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert store[_UUID]["viewers"] == ["a", "b"]  # viewer "b" survives this handler's own write-back
    assert store[_UUID]["last_heartbeat"] > 0


def test_run_get_live_manifest_refreshes_the_calling_viewers_own_heartbeat(monkeypatch, tmp_path):
    """Real, confirmed bug this fixes (found live 2026-09-28, see
    docs/OPEN_ITEMS.md): get_live_manifest used to only ever refresh the
    buffer-wide last_heartbeat, never the specific viewer's own
    viewer_heartbeats entry -- even though this is the *only* action a
    paused client still calls (via the addon's own GetStreamTimes()
    polling), since the addon's per-viewer heartbeat action only ever
    fires from ReadLiveTimeshiftStream(), which a pause stops calling
    entirely. A paused viewer's own per-viewer heartbeat went stale and
    got pruned by the reaper regardless of how often get_live_manifest
    itself was still being called."""
    store = {
        _UUID: {
            "channel_uuid": _UUID,
            "http_port": 9192,
            "last_heartbeat": 0,
            "viewers": ["a", "b"],
            "viewer_heartbeats": {"a": 0, "b": 0},
        }
    }
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_get_live_manifest",
        lambda s, logger: {"media_sequence": 0, "segments": [], "total_bytes": 0, "total_duration_ms": 0},
    )

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID, "viewer_id": "a"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert store[_UUID]["viewer_heartbeats"]["a"] > 0  # the calling viewer's own heartbeat is refreshed
    assert store[_UUID]["viewer_heartbeats"]["b"] == 0  # a different viewer's own heartbeat is untouched


def test_run_get_live_manifest_re_adds_a_viewer_id_this_buffer_does_not_know_about(monkeypatch, tmp_path):
    """Same re-add behavior as heartbeat (see _apply_heartbeat's own
    docstring) -- a paused viewer only ever calls get_live_manifest."""
    store = {_UUID: {"channel_uuid": _UUID, "http_port": 9192, "last_heartbeat": 0, "viewers": ["a"]}}
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_get_live_manifest",
        lambda s, logger: {"media_sequence": 0, "segments": [], "total_bytes": 0, "total_duration_ms": 0},
    )

    result, _logger = _run(
        monkeypatch, "get_live_manifest", {"channel_uuid": _UUID, "viewer_id": "unknown"}, {}, tmp_path
    )

    assert result["status"] == "ok"
    assert store[_UUID]["viewers"] == ["a", "unknown"]
    assert "unknown" in store[_UUID]["viewer_heartbeats"]


def test_run_get_live_manifest_does_not_resurrect_a_buffer_torn_down_during_the_build(monkeypatch, tmp_path):
    """Regression test for a real, confirmed gap: a concurrent teardown
    (stop_buffer, the idle reaper, or this same action's own fatal-error
    branch) can delete this buffer's Redis state while _get_live_manifest()
    is still building the response. The old `_get_buffer_state(...) or
    state` fallback would then write the stale top-of-function state back
    to Redis with a freshly refreshed heartbeat, resurrecting an
    already-deleted buffer. The manifest itself is still returned to the
    caller either way -- only the write-back must be skipped."""
    store = {_UUID: {"channel_uuid": _UUID, "http_port": 9192, "last_heartbeat": 0}}
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))

    def racing_manifest_build(_state, _logger):
        # A concurrent teardown deletes this buffer's state while this
        # (slow) call is still in progress.
        del store[_UUID]
        return {"media_sequence": 0, "segments": [], "total_bytes": 0, "total_duration_ms": 0}

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_live_manifest", racing_manifest_build)

    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["status"] == "ok"
    assert _UUID not in store  # stays torn down -- not resurrected by this handler's own write-back


# -- list_buffers --------------------------------------------------------


def test_run_list_buffers_none_active(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter([]))

    result, _logger = _run(monkeypatch, "list_buffers", {}, {}, tmp_path)

    assert result == {"status": "ok", "message": "No active buffers", "buffers": []}


def test_run_list_buffers_summarizes_active_buffers(monkeypatch, tmp_path):
    fake_states = [
        {
            "channel_uuid": _UUID,
            "started_at": 0,
            "last_heartbeat": 0,
            "http_port": 9192,
            "playlist_route": f"/{_UUID}/live.m3u8",
            "viewers": ["v1", "v2"],
        }
    ]
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter(fake_states))

    result, _logger = _run(monkeypatch, "list_buffers", {}, {}, tmp_path)

    assert result["status"] == "ok"
    assert "1 active buffer(s)" in result["message"]
    assert _UUID[:8] in result["message"]
    assert "2 viewer(s)" in result["message"]
    assert result["buffers"][0]["viewers"] == 2


# -- stop_all --------------------------------------------------------------


def test_run_stop_all_no_buffers(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter([]))

    result, _logger = _run(monkeypatch, "stop_all", {}, {}, tmp_path)

    assert result == {"status": "ok", "message": "No buffers were running", "stopped": []}


def test_run_stop_all_tears_down_every_buffer(monkeypatch, tmp_path):
    fake_states = [{"channel_uuid": _UUID}, {"channel_uuid": _OTHER_UUID}]
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter(fake_states))
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["channel_uuid"])
    )

    result, _logger = _run(monkeypatch, "stop_all", {}, {}, tmp_path)

    assert result["status"] == "ok"
    assert "Stopped 2 buffer(s)" in result["message"]
    assert set(result["stopped"]) == {_UUID, _OTHER_UUID}
    assert len(teardown_calls) == 2


# -- scrub_orphaned_buffers --------------------------------------------


def test_run_scrub_orphaned_buffers_none_found(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])

    result, _logger = _run(monkeypatch, "scrub_orphaned_buffers", {}, {}, tmp_path)

    assert result == {"status": "ok", "message": "No orphaned directories found", "removed": []}


def test_run_scrub_orphaned_buffers_reports_removed_count(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: ["a", "b"])

    result, _logger = _run(monkeypatch, "scrub_orphaned_buffers", {}, {}, tmp_path)

    assert result["status"] == "ok"
    assert result["message"] == "Removed 2 orphaned directories"


# ---------------------------------------------------------------------
# _reaper_loop -- monkeypatch _redis() to a fake in-memory client and every
# other Redis-touching function it calls (_iter_buffer_states/
# _set_buffer_state/_scrub_orphaned_dirs), same technique as Plugin.run()'s
# own tests above. A one-shot fake stop_event runs exactly one real tick
# synchronously (its .wait(15) sets itself rather than actually sleeping),
# so this exercises the real per-tick leadership/prune/reap/scrub logic
# without a background thread or real Redis/time.sleep.
#
# _FakeReaperRedisClient itself is also reused below by the
# _acquire_start_buffer_lock()/_release_start_buffer_lock() tests --
# same underlying SET NX EX primitive as the reaper's own leader
# election, just per-channel/short-lived instead of singleton/
# continuously-renewed, so one fake client naturally covers both.
# ---------------------------------------------------------------------


class _OneShotStopEvent:
    def __init__(self):
        self._done = False

    def is_set(self):
        return self._done

    def wait(self, _timeout):
        self._done = True


class _FakeReaperRedisClient:
    def __init__(self, leader_value=None):
        self.store = {}
        if leader_value is not None:
            self.store[timeshift_buffer_plugin._REDIS_LEADER_KEY] = leader_value
        self.expire_calls = []

    def set(self, key, value, nx=False, ex=None):  # noqa: ARG002 -- ex unused, matches real redis-py signature
        if nx and key in self.store:
            return False
        self.store[key] = value
        return True

    def get(self, key):
        return self.store.get(key)

    def expire(self, key, ttl):
        self.expire_calls.append((key, ttl))

    def delete(self, key):
        self.store.pop(key, None)


# ---------------------------------------------------------------------
# _acquire_start_buffer_lock / _release_start_buffer_lock -- reuse the
# same fake in-memory client as the reaper tests below, since this is
# the same SET NX EX primitive, just per-channel/short-lived instead of
# singleton/continuously-renewed.
# ---------------------------------------------------------------------


def test_acquire_start_buffer_lock_succeeds_when_free(monkeypatch):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    token = timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID)

    assert token is not None
    assert client.store[timeshift_buffer_plugin._start_buffer_lock_key(_UUID)] == token


def test_acquire_start_buffer_lock_fails_when_already_held(monkeypatch):
    client = _FakeReaperRedisClient()
    client.store[timeshift_buffer_plugin._start_buffer_lock_key(_UUID)] = "someone-else"
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    assert timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID) is None


def test_acquire_start_buffer_lock_is_independent_per_channel(monkeypatch):
    """The real bug this whole mechanism fixes is scoped to ONE channel
    at a time -- two different channels starting simultaneously must
    never contend with each other's own lock."""
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    token_a = timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID)
    token_b = timeshift_buffer_plugin._acquire_start_buffer_lock(_OTHER_UUID)

    assert token_a is not None
    assert token_b is not None
    assert token_a != token_b


def test_release_start_buffer_lock_removes_it_when_token_matches(monkeypatch):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    token = timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID)

    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, token)

    assert timeshift_buffer_plugin._start_buffer_lock_key(_UUID) not in client.store


def test_release_start_buffer_lock_does_not_remove_a_lock_a_different_caller_now_holds(monkeypatch):
    """Fixes a real, confirmed gap this fix's own comment already
    documents: a caller whose own hold outlived the lock's TTL (a slow
    ffmpeg spawn, a delayed request) must not blow away a *different*
    caller's own legitimately-acquired lock on release -- that would let
    two callers' own locked sections overlap after all."""
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    stale_token = timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID)
    # Simulate the stale token's own TTL expiring, then a different
    # caller legitimately acquiring the now-free lock.
    del client.store[timeshift_buffer_plugin._start_buffer_lock_key(_UUID)]
    fresh_token = timeshift_buffer_plugin._acquire_start_buffer_lock(_UUID)
    assert fresh_token != stale_token

    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, stale_token)

    # The fresh, still-valid lock must survive the stale caller's own
    # (too-late) release attempt.
    assert client.store[timeshift_buffer_plugin._start_buffer_lock_key(_UUID)] == fresh_token


def test_reaper_loop_skips_work_when_not_leader(monkeypatch):
    """Another process already holds the leader key under a different
    token -- this tick must do nothing at all, not just skip reaping."""
    client = _FakeReaperRedisClient(leader_value="someone-else")
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    iter_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter_calls.append(1) or [])
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: (_ for _ in ()).throw(AssertionError)
    )

    timeshift_buffer_plugin._reaper_loop(lambda: {}, _FakeLogger(), _OneShotStopEvent())

    assert iter_calls == []


def test_reaper_loop_reaps_an_idle_buffer_on_fresh_leadership(monkeypatch):
    client = _FakeReaperRedisClient()  # no existing leader -- this tick acquires it fresh
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)

    state = {"channel_uuid": "abc", "last_heartbeat": 0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    teardown_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s))
    scrub_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: scrub_calls.append(a) or [])

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": "/data/timeshift"}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    assert teardown_calls == [state]
    assert scrub_calls  # the storage-path reconciliation still runs this tick


def test_reaper_loop_prune_write_back_uses_a_freshly_read_copy(monkeypatch):
    """Regression test for the same class of race _apply_heartbeat's own
    docstring describes, applied to the reaper's own prune write-back: a
    concurrent start_buffer registering a new viewer between
    _iter_buffer_states()'s own read and this write-back must survive,
    not get silently dropped by writing back the loop's own now-stale
    pre-registration copy."""
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)

    stale_state = {
        "channel_uuid": "abc",
        "last_heartbeat": 1000.0,
        "viewers": ["a"],
        "viewer_heartbeats": {"a": 900.0},  # stale enough to be pruned (idle_timeout=30)
    }
    # Simulates a concurrent start_buffer that registered viewer "b" in
    # Redis after _iter_buffer_states() already handed the reaper its
    # own (now-stale) copy above.
    fresh_store = {
        "abc": {
            "channel_uuid": "abc",
            "last_heartbeat": 1000.0,
            "viewers": ["a", "b"],
            "viewer_heartbeats": {"a": 900.0, "b": 1000.0},
        }
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [stale_state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(fresh_store[uuid]))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update({uuid: s}))
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": "/data/timeshift"}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    # "a" is stale (900s old, > 30s timeout) and gets pruned; "b" (fresh,
    # registered concurrently) must survive.
    assert saved["abc"]["viewers"] == ["b"]


def test_reaper_loop_renews_ttl_when_already_leader(monkeypatch):
    """A leader key that already holds *this* process's own token (set by
    an earlier tick) must be renewed, not treated as someone else's lock."""
    my_token = f"{timeshift_buffer_plugin.os.getpid()}:1000.0"
    client = _FakeReaperRedisClient(leader_value=my_token)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])

    timeshift_buffer_plugin._reaper_loop(lambda: {"idle_timeout_seconds": 30}, _FakeLogger(), _OneShotStopEvent())

    assert client.expire_calls == [
        (timeshift_buffer_plugin._REDIS_LEADER_KEY, timeshift_buffer_plugin._REDIS_LEADER_TTL)
    ]


def test_reaper_loop_logs_and_continues_on_unexpected_error(monkeypatch):
    """An exception mid-tick (e.g. a transient Redis error) must not
    crash the loop -- it's caught, logged, and the loop still reaches
    its between-tick wait() so a real thread would try again."""
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)

    def raise_error():
        raise RuntimeError("boom")

    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", raise_error)

    logger = _FakeLogger()
    stop_event = _OneShotStopEvent()
    timeshift_buffer_plugin._reaper_loop(lambda: {"idle_timeout_seconds": 30}, logger, stop_event)

    assert any(level == "exception" for level, _ in logger.calls)
    assert stop_event.is_set()  # reached wait(15) despite the exception


def test_ensure_reaper_running_updates_settings_even_when_already_running(monkeypatch):
    """Real bug this fixes (found via a project-wide review): a settings
    change made after the reaper thread's first start must still reach
    it. _ensure_reaper_running() used to only ever wire up a fresh
    settings_getter closure on the very first call -- every later call's
    own settings_dict was simply discarded once the thread was already
    alive, so a change to idle_timeout_seconds/storage_path in
    Dispatcharr's own Plugin Settings UI never reached the reaper thread
    until the worker process itself restarted."""

    class _FakeAliveThread:
        def is_alive(self):
            return True

    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", _FakeAliveThread())
    monkeypatch.setattr(timeshift_buffer_plugin, "_latest_settings_dict", {"idle_timeout_seconds": 30})

    timeshift_buffer_plugin._ensure_reaper_running({"idle_timeout_seconds": 99}, _FakeLogger())

    assert timeshift_buffer_plugin._latest_settings_dict == {"idle_timeout_seconds": 99}


def test_ensure_reaper_running_starts_a_thread_whose_getter_reads_the_live_global(monkeypatch):
    """The reaper thread's own getter (built the first time the thread
    actually starts) must read _latest_settings_dict fresh each call, not
    close over the settings_dict object passed to *this* particular
    call -- otherwise the fix above would have nothing to update."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_latest_settings_dict", {})

    captured_getter = {}

    class _FakeThread:
        def __init__(self, target, args, name, daemon):
            captured_getter["fn"] = args[0]

        def start(self):
            pass

        def is_alive(self):
            return True

    monkeypatch.setattr(timeshift_buffer_plugin.threading, "Thread", _FakeThread)

    timeshift_buffer_plugin._ensure_reaper_running({"idle_timeout_seconds": 1}, _FakeLogger())
    assert captured_getter["fn"]() == {"idle_timeout_seconds": 1}

    # A later call with a different dict, even without the thread ever
    # being "alive" again in this test, is still visible through the
    # same getter -- confirming it reads the live global, not a snapshot.
    timeshift_buffer_plugin._ensure_reaper_running({"idle_timeout_seconds": 2}, _FakeLogger())
    assert captured_getter["fn"]() == {"idle_timeout_seconds": 2}


# ---------------------------------------------------------------------
# Plugin.stop() -- monkeypatch _iter_buffer_states/_teardown_buffer/
# _scrub_orphaned_dirs/_stop_http_server (the module-level calls
# _stop_all()/_scrub_orphaned_buffers()/stop() itself make) to confirm
# the real teardown order: reaper stop event first, then _stop_all(),
# then a scrub of anything already-orphaned, then the HTTP server last.
# ---------------------------------------------------------------------


class _FakeStopEvent:
    def __init__(self, calls):
        self._calls = calls

    def set(self):
        self._calls.append("reaper_stop_event.set")


def test_plugin_stop_runs_in_the_documented_order(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", _FakeStopEvent(calls))
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [])
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: calls.append("scrub_orphaned_dirs") or []
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: calls.append("stop_http_server"))

    timeshift_buffer_plugin.Plugin().stop({"logger": _FakeLogger(), "settings": {}})

    assert calls == ["reaper_stop_event.set", "scrub_orphaned_dirs", "stop_http_server"]


def test_plugin_stop_tears_down_every_tracked_buffer_before_scrubbing(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", None)
    state = {"channel_uuid": _UUID}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_teardown_buffer",
        lambda s, logger, **kw: calls.append(("teardown", s["channel_uuid"])),
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: calls.append("scrub") or [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: calls.append("http_server"))

    timeshift_buffer_plugin.Plugin().stop({"logger": _FakeLogger(), "settings": {}})

    assert calls == [("teardown", _UUID), "scrub", "http_server"]


def test_plugin_stop_tolerates_a_reaper_that_never_started(monkeypatch):
    """_reaper_stop_event stays None until _ensure_reaper_running() has
    actually been called at least once (e.g. run() was never invoked) --
    stop() must not crash trying to .set() it."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: None)

    timeshift_buffer_plugin.Plugin().stop({"logger": _FakeLogger(), "settings": {}})  # must not raise


def test_plugin_stop_does_not_tear_down_buffers_on_a_plain_reload(monkeypatch):
    """The real bug this fixes: Dispatcharr's own PluginReloadAPIView calls
    stop_all_plugins(reason="reload") on ANY plugin install/enable/disable/
    delete, not just this one -- tearing down every live buffer on that
    alone used to end playback server-wide for every viewer with
    server-side timeshift active, even though nothing about this plugin
    itself was being disabled or deleted."""
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", _FakeStopEvent(calls))
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: calls.append("teardown"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [{"channel_uuid": _UUID}])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: calls.append("scrub") or [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: calls.append("stop_http_server"))

    timeshift_buffer_plugin.Plugin().stop({"logger": _FakeLogger(), "settings": {}, "reason": "reload"})

    # The reaper still stops and the HTTP server still shuts down (this
    # module instance's own in-process objects), but no buffer teardown
    # or orphan scrub -- those touch state shared across every worker.
    assert calls == ["reaper_stop_event.set", "stop_http_server"]


@pytest.mark.parametrize("reason", ["disable", "delete", "shutdown", None])
def test_plugin_stop_still_tears_down_buffers_for_every_other_reason(monkeypatch, reason):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", _FakeStopEvent(calls))
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: calls.append("teardown"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [{"channel_uuid": _UUID}])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: calls.append("scrub") or [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: calls.append("stop_http_server"))

    context = {"logger": _FakeLogger(), "settings": {}}
    if reason is not None:
        context["reason"] = reason
    timeshift_buffer_plugin.Plugin().stop(context)

    assert calls == ["reaper_stop_event.set", "teardown", "scrub", "stop_http_server"]


# -- _resolve_viewer_id ----------------------------------------------------


def test_resolve_viewer_id_passes_through_a_non_empty_string():
    assert timeshift_buffer_plugin._resolve_viewer_id({"viewer_id": "viewer-a"}) == "viewer-a"


def test_resolve_viewer_id_normalizes_an_integer_to_its_string_form():
    assert timeshift_buffer_plugin._resolve_viewer_id({"viewer_id": 123}) == "123"


def test_resolve_viewer_id_rejects_missing_empty_bool_and_non_scalar_values():
    for params in (
        {},
        {"viewer_id": None},
        {"viewer_id": ""},
        {"viewer_id": True},
        {"viewer_id": ["a"]},
        {"viewer_id": {"a": 1}},
        {"viewer_id": 1.5},
    ):
        assert timeshift_buffer_plugin._resolve_viewer_id(params) is None, params


def test_run_start_buffer_with_an_unhashable_viewer_id_still_saves_its_state(monkeypatch, tmp_path):
    """Regression: a JSON-array viewer_id raised TypeError at the
    viewer_heartbeats write -- after _start_ffmpeg() had already spawned
    ffmpeg, but before its state was saved, leaving an ffmpeg process
    nothing could ever stop."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    fake_state = {"channel_uuid": _UUID, "pid": 123, "http_port": 9192, "playlist_route": f"/{_UUID}/live.m3u8"}
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(fake_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": ["a"]}, {}, tmp_path)

    assert result["status"] == "ok"
    assert saved["pid"] == 123
    assert saved["viewers"] == []


def test_integer_viewer_id_is_still_prunable_after_a_redis_json_round_trip(monkeypatch, tmp_path):
    """Regression: an int viewer_id stayed an int in `viewers` but came
    back from Redis as a str key in `viewer_heartbeats` (JSON object keys
    are always strings), so _prune_stale_viewers() never found its
    heartbeat and treated it as permanently fresh."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    fake_state = {"channel_uuid": _UUID, "pid": 123, "http_port": 9192, "playlist_route": f"/{_UUID}/live.m3u8"}
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(fake_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": 123}, {}, tmp_path)

    round_tripped = json.loads(json.dumps(saved))
    last_seen = round_tripped["viewer_heartbeats"]["123"]
    assert timeshift_buffer_plugin._prune_stale_viewers(round_tripped, 30, now=last_seen + 31) is True
    assert round_tripped["viewers"] == []


def test_run_get_live_manifest_reports_fatal_without_tearing_down_when_the_buffer_was_replaced(monkeypatch, tmp_path):
    """Regression test (docs/OPEN_ITEMS.md, 11th-pass audit): viewer A's
    buffer died and viewer B's start_buffer replaced it with a fresh one
    (new access_token) before A's next poll. A must get fatal: true -- and
    B's healthy buffer must NOT be torn down on A's behalf."""
    store = {
        _UUID: {"channel_uuid": _UUID, "http_port": 9192, "last_heartbeat": 0, "viewers": ["b"], "access_token": "B"}
    }
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))
    torn_down = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: torn_down.append(s))
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_get_live_manifest",
        lambda s, logger: pytest.fail("must not build a manifest for a replaced buffer"),
    )

    result, _logger = _run(
        monkeypatch,
        "get_live_manifest",
        {"channel_uuid": _UUID, "viewer_id": "a", "access_token": "A"},
        {},
        tmp_path,
    )

    assert result["status"] == "error"
    assert result["fatal"] is True
    assert torn_down == []
    assert store[_UUID]["viewers"] == ["b"]


# ---------------------------------------------------------------------
# _update_buffer_state / _cas_buffer_state -- atomic read-modify-write of
# a buffer's state (docs/CLOSED_ITEMS.md: "Redis viewer/heartbeat writes are
# unguarded read-modify-write"), against a fake Redis whose eval() has the
# real script's semantics and which can be made to lose races on demand.
# ---------------------------------------------------------------------


class _FakeStateRedis:
    """get/set/delete/eval over a dict. `before_eval(key)` runs just before
    each eval(), standing in for another request's write landing between this
    caller's read and its write."""

    def __init__(self):
        self.store = {}
        self.before_eval = None
        self.eval_calls = 0
        self.eval_error = None

    def get(self, key):
        return self.store.get(key)

    def set(self, key, value, ex=None, nx=False):  # noqa: ARG002
        self.store[key] = value
        return True

    def delete(self, key):
        self.store.pop(key, None)

    def eval(self, script, numkeys, key, expected, new, ttl):  # noqa: ARG002
        self.eval_calls += 1
        if self.eval_error is not None:
            raise self.eval_error
        if self.before_eval is not None:
            self.before_eval(key)
        if self.store.get(key) == expected:
            self.store[key] = new
            return 1
        return 0


@pytest.fixture
def state_redis(monkeypatch):
    client = _FakeStateRedis()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin, "_cas_buffer_state", _REAL_CAS)
    return client


def _stored(client, uuid=_UUID):
    return json.loads(client.store[timeshift_buffer_plugin._buffer_key(uuid)])


def _seed(client, state, uuid=_UUID):
    client.store[timeshift_buffer_plugin._buffer_key(uuid)] = json.dumps(state)


def test_update_buffer_state_writes_the_mutated_state(state_redis):
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1, "last_heartbeat": 1})

    outcome, state = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: {**s, "last_heartbeat": 99})

    assert outcome == "written"
    assert state["last_heartbeat"] == 99
    assert _stored(state_redis) == {"channel_uuid": _UUID, "pid": 1, "last_heartbeat": 99}


def test_update_buffer_state_never_resurrects_a_deleted_state(state_redis):
    """Consequence (c) in docs/OPEN_ITEMS.md: a heartbeat read just before a teardown
    deleted the state was written back afterwards with a fresh TTL."""
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1})

    def teardown_lands_first(key):
        state_redis.store.pop(key, None)

    state_redis.before_eval = teardown_lands_first

    outcome, state = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: {**s, "last_heartbeat": 5})

    assert (outcome, state) == ("absent", None)
    assert timeshift_buffer_plugin._buffer_key(_UUID) not in state_redis.store


def test_update_buffer_state_reports_absent_without_calling_the_mutator(state_redis):
    called = []

    outcome, _ = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: called.append(1) or s)

    assert outcome == "absent"
    assert called == []
    assert state_redis.eval_calls == 0


def test_update_buffer_state_declines_without_writing(state_redis):
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1})
    before = dict(state_redis.store)

    outcome, state = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: None)

    assert (outcome, state) == ("declined", None)
    assert state_redis.store == before
    assert state_redis.eval_calls == 0


def test_update_buffer_state_retries_against_the_state_another_writer_stored(state_redis):
    """Consequence (a): a heartbeat's read-modify-write used to overwrite a viewer registration that
    landed between its read and its write. Now the heartbeat loses the race, re-reads, and keeps both."""
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1, "viewers": ["a"], "last_heartbeat": 1})
    raced = []

    def viewer_registers_first(key):
        if raced:
            return
        raced.append(1)
        other = json.loads(state_redis.store[key])
        other["viewers"].append("b")
        state_redis.store[key] = json.dumps(other)

    state_redis.before_eval = viewer_registers_first
    mutator_runs = []

    def heartbeat(s):
        mutator_runs.append(list(s["viewers"]))
        s["last_heartbeat"] = 50
        return s

    outcome, _ = timeshift_buffer_plugin._update_buffer_state(_UUID, heartbeat)

    assert outcome == "written"
    assert mutator_runs == [["a"], ["a", "b"]]  # the second run saw the other writer's change
    assert _stored(state_redis)["viewers"] == ["a", "b"]
    assert _stored(state_redis)["last_heartbeat"] == 50


def test_update_buffer_state_keeps_a_stopping_marker_a_heartbeat_would_have_overwritten(state_redis):
    """Consequence (b): a heartbeat overwriting the "stopping" marker let a concurrent start_buffer
    reattach to a buffer that was mid-teardown."""
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1})
    raced = []

    def teardown_marks_first(key):
        if raced:
            return
        raced.append(1)
        other = json.loads(state_redis.store[key])
        other["stopping"] = True
        state_redis.store[key] = json.dumps(other)

    state_redis.before_eval = teardown_marks_first

    timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: timeshift_buffer_plugin._apply_heartbeat(s, 7))

    assert _stored(state_redis)["stopping"] is True
    assert _stored(state_redis)["last_heartbeat"] == 7


def test_update_buffer_state_gives_up_after_repeated_lost_races(state_redis):
    _seed(state_redis, {"channel_uuid": _UUID, "n": 0})

    def someone_always_writes_first(key):
        other = json.loads(state_redis.store[key])
        other["n"] += 1
        state_redis.store[key] = json.dumps(other)

    state_redis.before_eval = someone_always_writes_first
    runs = []

    outcome, state = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: runs.append(1) or s)

    assert (outcome, state) == ("contended", None)
    assert len(runs) == timeshift_buffer_plugin._STATE_UPDATE_ATTEMPTS


def test_update_buffer_state_compares_against_the_exact_stored_text(state_redis):
    """The compare is against the raw text read, not a re-serialization -- a stored value formatted
    differently from what json.dumps would produce (older writer, manual edit) must still update."""
    state_redis.store[timeshift_buffer_plugin._buffer_key(_UUID)] = '{"channel_uuid":  "%s",   "pid": 1}' % _UUID

    outcome, _ = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: {**s, "pid": 2})

    assert outcome == "written"
    assert _stored(state_redis)["pid"] == 2


def test_get_buffer_state_remembers_its_raw_text(state_redis):
    state_redis.store[timeshift_buffer_plugin._buffer_key(_UUID)] = b'{"pid": 3}'

    state = timeshift_buffer_plugin._get_buffer_state(_UUID)

    assert state == {"pid": 3}
    assert state.raw == b'{"pid": 3}'


def test_cas_buffer_state_falls_back_to_a_plain_write_when_scripts_are_refused(state_redis, monkeypatch):
    class ResponseError(Exception):
        pass

    state_redis.eval_error = ResponseError("unknown command 'eval'")
    monkeypatch.setattr(timeshift_buffer_plugin, "_cas_fallback_logged", False)
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1})

    outcome, _ = timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: {**s, "pid": 2})

    assert outcome == "written"
    assert _stored(state_redis)["pid"] == 2


def test_cas_buffer_state_propagates_a_connection_error(state_redis):
    class ConnectionError_(Exception):
        pass

    state_redis.eval_error = ConnectionError_("redis went away")
    _seed(state_redis, {"channel_uuid": _UUID, "pid": 1})

    with pytest.raises(ConnectionError_):
        timeshift_buffer_plugin._update_buffer_state(_UUID, lambda s: {**s, "pid": 2})


# ---- the callers that used to GET-then-SET


def test_teardown_buffer_marks_stopping_atomically_and_keeps_a_concurrent_viewer_out(state_redis, monkeypatch):
    """The teardown's own marker goes through the same compare-and-set, so a heartbeat can't overwrite it."""
    state = {"channel_uuid": _UUID, "pid": 1, "started_at": 5.0, "storage_path": "/x"}
    _seed(state_redis, state)
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: None)
    marked = []

    def record_marker(key):
        marked.append(json.loads(state_redis.store[key]).get("stopping"))

    state_redis.before_eval = record_marker

    assert timeshift_buffer_plugin._teardown_buffer(dict(state), _FakeLogger()) is True
    assert marked == [None]  # the marker was written by the eval, after the read, over an unmarked state
    assert timeshift_buffer_plugin._buffer_key(_UUID) not in state_redis.store  # and the state was then deleted


def test_teardown_buffer_abort_if_stops_it_before_anything_is_touched(state_redis, monkeypatch):
    """stop_buffer/the reaper decide from an earlier copy; a viewer or heartbeat that arrived since
    must win. The predicate runs against the freshest state, atomically with the marker."""
    state = {"channel_uuid": _UUID, "pid": 1, "started_at": 5.0, "viewers": []}
    fresh = {**state, "viewers": ["late"]}
    _seed(state_redis, fresh)
    steps = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: steps.append("stop"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: steps.append("remove"))
    logger = _FakeLogger()

    result = timeshift_buffer_plugin._teardown_buffer(state, logger, abort_if=lambda cur: bool(cur.get("viewers")))

    assert result is False
    assert steps == []
    assert "stopping" not in _stored(state_redis)  # no marker either
    assert any("wanted again" in msg for _level, msg in logger.calls)


def test_teardown_buffer_abort_if_lets_a_genuinely_unwanted_buffer_through(state_redis, monkeypatch):
    state = {"channel_uuid": _UUID, "pid": 1, "started_at": 5.0, "viewers": []}
    _seed(state_redis, state)
    steps = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: steps.append("stop"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: steps.append("remove"))

    result = timeshift_buffer_plugin._teardown_buffer(
        dict(state), _FakeLogger(), abort_if=lambda cur: bool(cur.get("viewers"))
    )

    assert result is True
    assert steps == ["stop", "remove"]


def test_run_stop_buffer_leaves_the_buffer_when_a_viewer_attached_before_the_teardown(monkeypatch, tmp_path):
    state = {"channel_uuid": _UUID, "viewers": ["v1"], "viewer_heartbeats": {"v1": 100}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: None)
    captured = {}

    def refused(s, logger, abort_if=None):
        captured["abort_if"] = abort_if
        return False

    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", refused)

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert "still active" in result["message"]
    # What it asked the teardown to re-check: viewers on the freshest state.
    assert captured["abort_if"]({"viewers": ["new"]}) is True
    assert captured["abort_if"]({"viewers": []}) is False


def test_run_start_buffer_refuses_to_reattach_to_a_buffer_marked_stopping(monkeypatch, tmp_path):
    """The classification read says alive and unmarked; the teardown marked it a moment later. The attach
    re-checks the freshest state, so a stale classification can't hand out a buffer about to be removed."""
    reads = iter(
        [
            {
                "channel_uuid": _UUID,
                "pid": 7,
                "http_port": 9192,
                "storage_path": str(tmp_path),
                "playlist_route": "/x",
                "access_token": "t",
                "viewers": [],
            },
            {
                "channel_uuid": _UUID,
                "pid": 7,
                "http_port": 9192,
                "storage_path": str(tmp_path),
                "playlist_route": "/x",
                "access_token": "t",
                "viewers": [],
                "stopping": True,
                "stopping_since": time.time(),
            },
        ]
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: next(reads))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: pytest.fail("must not write"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)

    result, _logger = _run(
        monkeypatch,
        "start_buffer",
        {"channel_uuid": _UUID, "viewer_id": "v1"},
        {"storage_path": str(tmp_path)},
        tmp_path,
    )

    assert result["status"] == "error"
    assert result["retryable"] is True


def test_run_heartbeat_reports_a_missing_buffer_without_writing(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: pytest.fail("must not write"))

    result, _logger = _run(monkeypatch, "heartbeat", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result == {"status": "error", "message": "no buffer running for this channel"}


def test_reaper_asks_the_teardown_to_recheck_the_heartbeat(monkeypatch):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
    state = {"channel_uuid": "abc", "last_heartbeat": 0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    captured = {}
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_teardown_buffer",
        lambda s, logger, abort_if=None: captured.setdefault("abort_if", abort_if),
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])

    timeshift_buffer_plugin._reaper_loop(
        lambda: {"idle_timeout_seconds": 30, "storage_path": "/x"}, _FakeLogger(), _OneShotStopEvent()
    )

    # A heartbeat 10s ago (now=1000) is inside the 30s idle window: keep the buffer.
    assert captured["abort_if"]({"last_heartbeat": 990}) is True
    assert captured["abort_if"]({"last_heartbeat": 100}) is False


# ---------------------------------------------------------------------
# _stop_orphaned_threads -- listener/reaper threads an earlier import of
# the module left running (docs/CLOSED_ITEMS.md: "Plugin reload leaks
# timeshift_buffer's listener and reaper per worker")
# ---------------------------------------------------------------------


class _FakeServer:
    def __init__(self, shutdown_error=None):
        self.calls = []
        self.shutdown_error = shutdown_error

    def shutdown(self):
        self.calls.append("shutdown")
        if self.shutdown_error:
            raise self.shutdown_error

    def server_close(self):
        self.calls.append("server_close")


@pytest.fixture
def parked_threads():
    """Starts real threads under given names that sit until released, and
    guarantees they are released and joined afterwards."""
    import threading

    release = threading.Event()
    started = []

    def start(name, **attrs):
        thread = threading.Thread(target=release.wait, name=name, daemon=True)
        for key, value in attrs.items():
            setattr(thread, key, value)
        thread.start()
        started.append(thread)
        return thread

    yield start
    release.set()
    for thread in started:
        thread.join(timeout=5)


@pytest.fixture
def fresh_sweep(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_orphans_swept", False)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", None)


def test_stop_orphaned_threads_shuts_down_a_listener_an_earlier_import_left(parked_threads, fresh_sweep):
    server = _FakeServer()
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=server)
    logger = _FakeLogger()

    timeshift_buffer_plugin._stop_orphaned_threads(logger)

    assert server.calls == ["shutdown", "server_close"]
    assert any("file server left running" in msg for _level, msg in logger.calls)


def test_stop_orphaned_threads_stops_a_reaper_an_earlier_import_left(parked_threads, fresh_sweep):
    import threading

    stop_event = threading.Event()
    parked_threads(timeshift_buffer_plugin._REAPER_THREAD_NAME, tsb_stop_event=stop_event)

    timeshift_buffer_plugin._stop_orphaned_threads(_FakeLogger())

    assert stop_event.is_set()


def test_stop_orphaned_threads_leaves_this_imports_own_threads_alone(parked_threads, monkeypatch, fresh_sweep):
    import threading

    server = _FakeServer()
    stop_event = threading.Event()
    own_http = parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=server)
    own_reaper = parked_threads(timeshift_buffer_plugin._REAPER_THREAD_NAME, tsb_stop_event=stop_event)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_thread", own_http)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", own_reaper)

    timeshift_buffer_plugin._stop_orphaned_threads(_FakeLogger())

    assert server.calls == []
    assert not stop_event.is_set()


def test_stop_orphaned_threads_ignores_unrelated_threads(parked_threads, fresh_sweep):
    server = _FakeServer()
    parked_threads("some_other_plugins_http", tsb_server=server)

    timeshift_buffer_plugin._stop_orphaned_threads(_FakeLogger())

    assert server.calls == []


def test_stop_orphaned_threads_reports_threads_from_an_older_plugin_version_once(parked_threads, fresh_sweep):
    """Threads started before the handles existed can't be stopped from here."""
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME)
    parked_threads(timeshift_buffer_plugin._REAPER_THREAD_NAME)
    logger = _FakeLogger()

    timeshift_buffer_plugin._stop_orphaned_threads(logger)

    warnings = [msg for level, msg in logger.calls if level == "warning"]
    assert len(warnings) == 1
    assert "2 file server/reaper thread(s)" in warnings[0]
    assert "restart" in warnings[0]


def test_stop_orphaned_threads_keeps_going_when_one_listener_wont_stop(parked_threads, fresh_sweep):
    bad = _FakeServer(shutdown_error=OSError("already closed"))
    good = _FakeServer()
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=bad)
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=good)
    logger = _FakeLogger()

    timeshift_buffer_plugin._stop_orphaned_threads(logger)

    assert good.calls == ["shutdown", "server_close"]
    assert any(level == "exception" for level, _msg in logger.calls)


def test_stop_orphaned_threads_runs_once_per_import(parked_threads, fresh_sweep):
    first = _FakeServer()
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=first)
    timeshift_buffer_plugin._stop_orphaned_threads(_FakeLogger())

    later = _FakeServer()
    parked_threads(timeshift_buffer_plugin._HTTP_THREAD_NAME, tsb_server=later)
    timeshift_buffer_plugin._stop_orphaned_threads(_FakeLogger())

    assert first.calls == ["shutdown", "server_close"]
    assert later.calls == []


def test_run_sweeps_orphans_before_starting_its_own_listener_and_reaper(monkeypatch, tmp_path):
    order = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_orphaned_threads", lambda logger: order.append("sweep"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_ensure_http_server_running", lambda *a, **k: order.append("http"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_ensure_reaper_running", lambda *a, **k: order.append("reaper"))
    monkeypatch.setattr(timeshift_buffer_plugin.Plugin, "_list_buffers", lambda self: {"status": "ok"})

    timeshift_buffer_plugin.Plugin().run(
        "list_buffers", {}, {"logger": _FakeLogger(), "settings": {"storage_path": str(tmp_path)}}
    )

    assert order == ["sweep", "http", "reaper"]


def test_started_threads_carry_the_handles_a_later_import_needs(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_storage_path", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_loop", lambda *a, **k: None)
    logger = _FakeLogger()

    timeshift_buffer_plugin._ensure_http_server_running(str(tmp_path), 0, logger)
    timeshift_buffer_plugin._ensure_reaper_running({}, logger)
    try:
        http_thread = timeshift_buffer_plugin._http_server_thread
        assert http_thread.name == timeshift_buffer_plugin._HTTP_THREAD_NAME
        assert http_thread.tsb_server is timeshift_buffer_plugin._http_server
        reaper_thread = timeshift_buffer_plugin._reaper_thread
        assert reaper_thread.name == timeshift_buffer_plugin._REAPER_THREAD_NAME
        assert reaper_thread.tsb_stop_event is timeshift_buffer_plugin._reaper_stop_event
    finally:
        timeshift_buffer_plugin._stop_http_server(logger)


def test_a_reloaded_copy_of_the_module_stops_the_previous_copys_real_listener(tmp_path):
    """The whole mechanism with real objects: two separate imports of the plugin file (what
    Dispatcharr's reload does), the first having started a listener; the second one's sweep must
    close it even though it shares none of the first's module globals."""
    import socket

    def load(name):
        spec = importlib.util.spec_from_file_location(name, _PLUGIN_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    old_copy = load("timeshift_buffer_plugin_old_copy")
    new_copy = load("timeshift_buffer_plugin_new_copy")
    logger = _FakeLogger()
    try:
        old_copy._ensure_http_server_running(str(tmp_path), 0, logger)
        port = old_copy._http_server.server_address[1]
        with socket.create_connection(("127.0.0.1", port), timeout=5):
            pass  # reachable before the "reload"

        assert new_copy._http_server is None  # the fresh import knows nothing of it
        new_copy._stop_orphaned_threads(logger)

        with pytest.raises(OSError):
            socket.create_connection(("127.0.0.1", port), timeout=2)
    finally:
        old_copy._stop_http_server(logger)


# ---------------------------------------------------------------------------
# Startup checks are serialized (docs/CLOSED_ITEMS.md, "HTTP-server and reaper startup checks are unlocked")
# ---------------------------------------------------------------------------


def _free_port():
    import socket

    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def _run_concurrently(target, count=8):
    errors = []
    barrier = threading.Barrier(count)

    def worker():
        try:
            barrier.wait(timeout=5)
            target()
        except Exception as exc:  # pragma: no cover - surfaced below
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(count)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=10)
    assert not errors, errors


def test_concurrent_first_calls_start_exactly_one_http_server(monkeypatch, tmp_path):
    created = []
    real_create = timeshift_buffer_plugin._create_http_server

    def slow_create(port, logger):
        created.append(1)
        time.sleep(0.05)  # widens the window an unlocked check-then-create loses in
        return real_create(port, logger)

    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_storage_path", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_create_http_server", slow_create)
    logger = _FakeLogger()
    port = _free_port()  # a real port: with 0 the "config unchanged" check never matches the bound one
    try:
        _run_concurrently(lambda: timeshift_buffer_plugin._ensure_http_server_running(str(tmp_path), port, logger))
        assert len(created) == 1
        assert timeshift_buffer_plugin._http_server is not None
    finally:
        timeshift_buffer_plugin._stop_http_server(logger)


def test_a_config_change_restarts_the_http_server_without_deadlocking(monkeypatch, tmp_path):
    """_ensure_http_server_running() holds the lock while calling the stop path on a config
    change, so the lock has to be reentrant."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_http_server_storage_path", None)
    logger = _FakeLogger()
    other = tmp_path / "other"
    other.mkdir()
    try:
        port = _free_port()
        timeshift_buffer_plugin._ensure_http_server_running(str(tmp_path), port, logger)
        first = timeshift_buffer_plugin._http_server
        done = threading.Event()

        def restart():
            timeshift_buffer_plugin._ensure_http_server_running(str(other), port, logger)
            done.set()

        t = threading.Thread(target=restart, daemon=True)
        t.start()
        assert done.wait(timeout=10), "restart on a storage_path change deadlocked"
        assert timeshift_buffer_plugin._http_server is not first
        assert timeshift_buffer_plugin._http_server_storage_path == str(other)
    finally:
        timeshift_buffer_plugin._stop_http_server(logger)


def test_concurrent_first_calls_start_exactly_one_reaper(monkeypatch):
    started = []

    class SlowEvent(threading.Event):
        def __init__(self):
            time.sleep(0.05)  # widens the window an unlocked check-then-start loses in
            super().__init__()

    def fake_loop(getter, logger, stop_event):
        started.append(stop_event)
        stop_event.wait(timeout=10)

    fake_threading = types.SimpleNamespace(Event=SlowEvent, Thread=threading.Thread)
    monkeypatch.setattr(timeshift_buffer_plugin, "threading", fake_threading)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_thread", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_loop", fake_loop)
    logger = _FakeLogger()
    try:
        _run_concurrently(lambda: timeshift_buffer_plugin._ensure_reaper_running({}, logger))
        time.sleep(0.2)
        assert len(started) == 1
        assert timeshift_buffer_plugin._reaper_thread.tsb_stop_event is started[0]
    finally:
        if timeshift_buffer_plugin._reaper_stop_event is not None:
            timeshift_buffer_plugin._reaper_stop_event.set()
        for e in started:
            e.set()


# ---------------------------------------------------------------------------
# A recycled pid is not our ffmpeg (docs/CLOSED_ITEMS.md, "timeshift_buffer trusts a Redis-stored pid")
# ---------------------------------------------------------------------------

# Field 22 is the start time: after ") " come state (3), ppid (4) ... so it is the 20th token.
_STAT_FIELDS_AFTER_COMM = "S 1 4242 4242 0 -1 4194560 100 0 0 0 5 3 0 0 20 0 1 0 {ticks} 1000 200 18446744073709551615"


def _stat(pid, comm, ticks):
    return f"{pid} ({comm}) " + _STAT_FIELDS_AFTER_COMM.format(ticks=ticks)


def test_proc_start_ticks_reads_field_22():
    assert timeshift_buffer_plugin._proc_start_ticks_from_stat(_stat(4242, "ffmpeg", 987654)) == 987654


def test_proc_start_ticks_survives_a_comm_with_spaces_and_parentheses():
    assert timeshift_buffer_plugin._proc_start_ticks_from_stat(_stat(7, "weird (name) x", 555)) == 555


def test_proc_start_ticks_is_none_for_text_it_cannot_read():
    for text in ("", "garbage", "1234 (ffmpeg) Z 1 ...", "1 (x) S 1 2", _stat(1, "x", "notanumber")):
        assert timeshift_buffer_plugin._proc_start_ticks_from_stat(text) is None


def test_start_ffmpeg_records_the_processes_start_time(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "ffmpeg", 31337))

    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert state["pid"] == 4242
    assert state["pid_start_ticks"] == 31337


def test_start_ffmpeg_records_none_when_proc_is_unreadable(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: None)

    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert state["pid_start_ticks"] is None


def test_is_process_alive_false_when_the_pid_belongs_to_a_different_process(monkeypatch):
    """The pid answers signal 0 and is not a zombie -- but its start time is not the one recorded,
    so it is somebody else's, and ours is gone."""
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "gunicorn", 222))
    waitpid_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "waitpid", lambda *a: waitpid_calls.append(a) or (0, 0))

    assert timeshift_buffer_plugin._is_process_alive(4242, 111) is False
    assert waitpid_calls == []  # never reaps a process that is not ours


def test_is_process_alive_true_when_the_start_time_matches(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "ffmpeg", 111))

    assert timeshift_buffer_plugin._is_process_alive(4242, 111) is True


def test_is_process_alive_without_a_recorded_start_time_behaves_as_before(monkeypatch):
    """State written by an older plugin version, or on a host with no readable /proc."""
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "anything", 999))

    assert timeshift_buffer_plugin._is_process_alive(4242) is True
    assert timeshift_buffer_plugin._is_process_alive(4242, None) is True


def test_is_process_alive_still_treats_a_matching_zombie_as_dead(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: None)
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "ffmpeg", 111).replace(" S ", " Z ", 1)
    )
    monkeypatch.setattr(timeshift_buffer_plugin.os, "waitpid", lambda *a: (0, 0))

    assert timeshift_buffer_plugin._is_process_alive(4242, 111) is False


def test_stop_ffmpeg_does_not_signal_a_pid_that_now_belongs_to_another_process(monkeypatch):
    """The failure this exists for: Redis still holds the state, the pid has been reused by an
    unrelated process, and SIGTERM then SIGKILL would take that process group down."""
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "gunicorn", 222))
    logger = _FakeLogger()

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 4242, "pid_start_ticks": 111}, logger)

    assert calls == []


def test_stop_ffmpeg_signals_the_pid_when_the_start_time_matches(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: _stat(pid, "ffmpeg", 111))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *a: False)

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 4242, "pid_start_ticks": 111}, _FakeLogger())

    assert calls == [(4242, timeshift_buffer_plugin.signal.SIGTERM)]


def test_stop_ffmpeg_without_a_recorded_start_time_signals_as_before(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *a: False)

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 4242}, _FakeLogger())

    assert calls == [(4242, timeshift_buffer_plugin.signal.SIGTERM)]


def test_stop_ffmpeg_still_signals_when_the_process_is_already_gone(monkeypatch):
    """No /proc entry at all is not a mismatch -- the existing "already gone" handling applies."""
    calls = []

    def killpg(pid, sig):
        calls.append((pid, sig))
        raise ProcessLookupError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", killpg)
    monkeypatch.setattr(timeshift_buffer_plugin, "_read_proc_pid_stat", lambda pid: None)

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 4242, "pid_start_ticks": 111}, _FakeLogger())

    assert calls[0] == (4242, timeshift_buffer_plugin.signal.SIGTERM)  # then the liveness poll's own signal 0


# ---------------------------------------------------------------------
# _same_buffer_instance
# ---------------------------------------------------------------------


def test_same_buffer_instance_matches_on_pid_and_started_at_only():
    same = timeshift_buffer_plugin._same_buffer_instance
    a = {"pid": 100, "started_at": 1700000000.5, "viewers": {"v1": 1.0}, "stopping": True}
    b = {"pid": 100, "started_at": 1700000000.5, "viewers": {}, "last_heartbeat": 1700000900.0}
    # Everything but pid/started_at is rewritten all the time and must not matter.
    assert same(a, b)
    assert not same(a, {**b, "pid": 101})
    assert not same(a, {**b, "started_at": 1700000001.0})


def test_same_buffer_instance_is_false_when_either_side_lacks_the_identity():
    same = timeshift_buffer_plugin._same_buffer_instance
    full = {"pid": 100, "started_at": 1700000000.5}
    assert not same({}, full)
    assert not same(full, {"started_at": 1700000000.5})
    # Two states that both carry no identity compare equal: a teardown of such
    # a broken state is allowed to proceed, which is what a caller wants.
    assert same({}, {"viewers": {}})


# ---------------------------------------------------------------------
# _parse_live_playlist_lines
# ---------------------------------------------------------------------


def test_parse_live_playlist_lines_reads_the_media_sequence_and_numbers_entries_from_it():
    lines = ["#EXTM3U", "#EXT-X-MEDIA-SEQUENCE:40", "#EXTINF:2.0,", "seg_00040.ts", "#EXTINF:1.5,", "seg_00041.ts"]
    media_sequence, parsed = timeshift_buffer_plugin._parse_live_playlist_lines(lines)
    assert media_sequence == 40
    assert parsed == [(40, "seg_00040.ts", 2000), (41, "seg_00041.ts", 1500)]


def test_parse_live_playlist_lines_defaults_a_missing_or_unreadable_media_sequence_to_zero():
    assert timeshift_buffer_plugin._parse_live_playlist_lines(["#EXTINF:2.0,", "a.ts"]) == (0, [(0, "a.ts", 2000)])
    assert timeshift_buffer_plugin._parse_live_playlist_lines(["#EXT-X-MEDIA-SEQUENCE:x", "#EXTINF:2.0,", "a.ts"]) == (
        0,
        [(0, "a.ts", 2000)],
    )


def test_parse_live_playlist_lines_tolerates_a_title_and_a_malformed_duration():
    lines = ["#EXTINF:6.0,some title", "a.ts", "#EXTINF:inf,", "b.ts", "#EXTINF:abc,", "c.ts"]
    _, parsed = timeshift_buffer_plugin._parse_live_playlist_lines(lines)
    assert parsed == [(0, "a.ts", 6000), (1, "b.ts", 0), (2, "c.ts", 0)]


def test_parse_live_playlist_lines_skips_an_extinf_with_no_uri_and_keeps_the_numbering():
    # An #EXTINF: followed by a tag, or by a blank line, names no file: skipped
    # whole, and (unlike a file dropped later for being missing) not counted.
    lines = ["#EXTINF:2.0,", "#EXT-X-ENDLIST", "#EXTINF:2.0,", "", "#EXTINF:2.0,", "z.ts"]
    assert timeshift_buffer_plugin._parse_live_playlist_lines(lines) == (0, [(0, "z.ts", 2000)])


def test_parse_live_playlist_lines_handles_an_empty_or_tag_only_playlist():
    assert timeshift_buffer_plugin._parse_live_playlist_lines([]) == (0, [])
    assert timeshift_buffer_plugin._parse_live_playlist_lines(["#EXTM3U", "#EXT-X-MEDIA-SEQUENCE:3"]) == (3, [])


# ---------------------------------------------------------------------
# _buffers_summary
# ---------------------------------------------------------------------


def test_buffers_summary_for_no_buffers():
    assert timeshift_buffer_plugin._buffers_summary([]) == "No active buffers"


def test_buffers_summary_names_each_buffer_by_a_uuid_prefix_with_viewers_and_age():
    buffers = [
        {"channel_uuid": "0123456789abcdef-x", "viewers": 2, "age_seconds": 61},
        {"channel_uuid": "fedcba9876543210-y", "viewers": 0, "age_seconds": 5},
    ]
    assert timeshift_buffer_plugin._buffers_summary(buffers) == (
        "2 active buffer(s): 01234567 (2 viewer(s), 61s old), fedcba98 (0 viewer(s), 5s old)"
    )


# ---------------------------------------------------------------------
# The ffmpeg owner file and the buffer whose Redis state was lost (the 2026-10-04 third
# hardening sweep): the reaper and the orphan scrub only looked at Redis-tracked buffers or at
# directories idle for 300 s, and a running hls muxer rewrites its directory every few seconds,
# so an ffmpeg left running by a Redis restart held its provider slot indefinitely and the next
# start_buffer put a second ffmpeg into the same directory.
# ---------------------------------------------------------------------

_ORPHAN_UUID = "55555555-5555-5555-5555-555555555555"


def _write_owner(channel_dir, pid=4242, ticks=777, started_at=None):
    channel_dir.mkdir(parents=True, exist_ok=True)
    (channel_dir / timeshift_buffer_plugin._OWNER_FILE_NAME).write_text(
        json.dumps({"pid": pid, "pid_start_ticks": ticks, "started_at": started_at if started_at else time.time()})
    )


def test_start_ffmpeg_records_an_owner_file_naming_the_new_process(tmp_path, monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())
    monkeypatch.setattr(timeshift_buffer_plugin, "_proc_start_ticks", lambda pid: 987654)

    monkeypatch.setattr(timeshift_buffer_plugin, "_shared_now", lambda: 5000.0)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 222.0)

    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    owner = timeshift_buffer_plugin._read_owner_file(tmp_path / _UUID)
    # The state's start time is the shared clock's, the owner file's is this host's: the orphan scrub compares the
    # owner file's age with time.time(), and with Redis on another host the two clocks differ by that host's skew.
    assert state["started_at"] == 5000.0
    assert owner == {"pid": 4242, "pid_start_ticks": 987654, "started_at": 222.0}
    assert not list((tmp_path / _UUID).glob("*.tmp"))


@pytest.mark.parametrize(
    "content",
    [
        "",
        "not json",
        "[]",
        '{"pid": "4242"}',
        '{"pid": true}',
        '{"pid": 1}',
        '{"pid": 0}',
        '{"pid": -5}',
        '{"pid_start_ticks": 5}',
    ],
)
def test_read_owner_file_ignores_anything_unusable(tmp_path, content):
    (tmp_path / timeshift_buffer_plugin._OWNER_FILE_NAME).write_text(content)
    assert timeshift_buffer_plugin._read_owner_file(tmp_path) is None


def test_read_owner_file_is_none_when_absent(tmp_path):
    assert timeshift_buffer_plugin._read_owner_file(tmp_path) is None


def test_read_owner_file_tolerates_missing_or_mistyped_optional_fields(tmp_path):
    (tmp_path / timeshift_buffer_plugin._OWNER_FILE_NAME).write_text(
        '{"pid": 4242, "pid_start_ticks": "x", "started_at": "yesterday"}'
    )
    assert timeshift_buffer_plugin._read_owner_file(tmp_path) == {
        "pid": 4242,
        "pid_start_ticks": None,
        "started_at": None,
    }


def test_an_untracked_directory_with_a_long_running_ffmpeg_is_an_orphan_despite_a_fresh_mtime(tmp_path, monkeypatch):
    orphan = tmp_path / _ORPHAN_UUID
    _write_owner(orphan, started_at=time.time() - 3600)  # ffmpeg has been running for an hour...
    (orphan / "seg_00001.ts").write_bytes(b"x")  # ...and just closed a segment: the mtime is fresh
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)

    orphans = timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=300)
    assert [p.name for p in orphans] == [_ORPHAN_UUID]


def test_an_untracked_directory_whose_ffmpeg_only_just_started_is_left_alone(tmp_path, monkeypatch):
    # The window between spawning ffmpeg and persisting its state in Redis.
    young = tmp_path / _ORPHAN_UUID
    _write_owner(young, started_at=time.time() - 5)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)

    assert timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=300) == []


def test_a_tracked_buffer_is_never_an_orphan_however_old_its_ffmpeg(tmp_path, monkeypatch):
    tracked = tmp_path / _ORPHAN_UUID
    _write_owner(tracked, started_at=time.time() - 36000)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"channel_uuid": uuid})
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)

    assert timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=300) == []


def test_a_dead_owner_leaves_the_old_mtime_rule_in_charge(tmp_path, monkeypatch):
    orphan = tmp_path / _ORPHAN_UUID
    _write_owner(orphan, started_at=time.time() - 3600)
    (orphan / "seg_00001.ts").write_bytes(b"x")  # fresh mtime, but nothing is writing any more
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: False)

    assert timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=300) == []


def test_an_owner_file_without_a_start_time_is_never_acted_on(tmp_path, monkeypatch):
    # With no recorded start time a recycled pid cannot be told from the ffmpeg.
    orphan = tmp_path / _ORPHAN_UUID
    _write_owner(orphan, ticks=None, started_at=time.time() - 3600)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    stopped = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda state, logger: stopped.append(state))

    assert timeshift_buffer_plugin._reap_untracked_ffmpeg(orphan, _FakeLogger()) is False
    assert stopped == []


def test_scrubbing_an_orphan_stops_its_ffmpeg_before_removing_the_directory(tmp_path, monkeypatch, start_locks):
    orphan = tmp_path / _ORPHAN_UUID
    _write_owner(orphan, pid=4321, ticks=555, started_at=time.time() - 3600)
    (orphan / "seg_00001.ts").write_bytes(b"x")
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    events = []

    def fake_stop(state, logger):
        events.append(("stop", state["pid"], state["pid_start_ticks"], orphan.exists()))

    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", fake_stop)

    removed = timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), min_age_seconds=300, logger=_FakeLogger())

    assert removed == [_ORPHAN_UUID]
    assert events == [("stop", 4321, 555, True)]  # stopped while the directory was still there
    assert not orphan.exists()


def test_a_fresh_start_stops_an_untracked_ffmpeg_before_clearing_its_directory(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    _write_owner(channel_dir, pid=9999, ticks=111)
    (channel_dir / "seg_00001.ts").write_bytes(b"stale")
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: pid == 9999)
    order = []

    def fake_stop(state, logger):
        order.append(("stop", state["pid"], (channel_dir / "seg_00001.ts").exists()))

    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", fake_stop)

    def fake_popen(cmd, **kwargs):
        order.append(("spawn", sorted(p.name for p in Path(kwargs["cwd"]).iterdir())))
        return _FakeProc()

    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", fake_popen)

    timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    # Stopped first, while the old files were still there; the new process started in a cleared directory.
    assert order == [("stop", 9999, True), ("spawn", ["ffmpeg.log"])]


def test_a_fresh_start_with_no_owner_file_or_a_dead_one_stops_nothing(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    _write_owner(channel_dir, pid=9999, ticks=111)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: False)
    stopped = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda state, logger: stopped.append(state))
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())

    timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())

    assert stopped == []


_ORPHAN_UUID_B = "66666666-6666-6666-6666-666666666666"


def test_the_scrub_takes_each_channels_start_lock_and_releases_it(tmp_path, monkeypatch, start_locks):
    for uuid_ in (_ORPHAN_UUID, _ORPHAN_UUID_B):
        _write_owner(tmp_path / uuid_, pid=5000, ticks=1, started_at=time.time() - 3600)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda state, logger: None)

    removed = timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), 300, _FakeLogger())

    assert sorted(removed) == sorted([_ORPHAN_UUID, _ORPHAN_UUID_B])
    kinds = [kind for kind, _ in start_locks.log]
    assert kinds == ["acquire", "release", "acquire", "release"]
    assert start_locks.held == set()


def test_the_scrub_skips_a_channel_whose_start_is_in_progress(tmp_path, monkeypatch, start_locks):
    _write_owner(tmp_path / _ORPHAN_UUID, pid=5000, ticks=1, started_at=time.time() - 3600)
    start_locks.held.add(_ORPHAN_UUID)  # a start_buffer for this channel holds the lock
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    stopped = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda state, logger: stopped.append(state["pid"]))

    assert timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), 300, _FakeLogger()) == []
    assert stopped == []
    assert (tmp_path / _ORPHAN_UUID).is_dir()


def test_the_scrub_releases_the_lock_when_removing_fails(tmp_path, monkeypatch, start_locks):
    _write_owner(tmp_path / _ORPHAN_UUID, pid=5000, ticks=1, started_at=time.time() - 3600)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda state, logger: None)

    def failing_rmtree(path):
        raise OSError("busy")

    monkeypatch.setattr(timeshift_buffer_plugin.shutil, "rmtree", failing_rmtree)
    assert timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), 300, _FakeLogger()) == []
    assert start_locks.held == set()


def test_a_start_that_finishes_while_the_scrub_stops_an_earlier_orphan_is_left_alone(
    tmp_path, monkeypatch, start_locks
):
    """The reproducer from the 2026-10-04 fourth sweep: two orphans (every buffer is one after a Redis
    loss); stopping the first takes seconds, and a start_buffer for the second finishes meanwhile --
    new ffmpeg, new owner file, new Redis state. The stale list used to stop that new ffmpeg and
    delete its directory."""
    pids = {_ORPHAN_UUID: 1001, _ORPHAN_UUID_B: 2000}
    for uuid_, pid in pids.items():
        _write_owner(tmp_path / uuid_, pid=pid, ticks=1, started_at=time.time() - 3600)
    uuid_of_pid = {pid: uuid_ for uuid_, pid in pids.items()}
    tracked = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: tracked.get(uuid))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    stopped = []
    started = {}

    def fake_stop(state, logger):
        stopped.append(state["pid"])
        if len(stopped) == 1:
            # ...meanwhile start_buffer for the OTHER channel completes (the scrub's directory order
            # is the filesystem's, so which one is first is not fixed).
            other = _ORPHAN_UUID_B if uuid_of_pid[state["pid"]] == _ORPHAN_UUID else _ORPHAN_UUID
            started["uuid"] = other
            _write_owner(tmp_path / other, pid=3000, ticks=2, started_at=time.time())
            tracked[other] = {"channel_uuid": other, "pid": 3000}

    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", fake_stop)

    removed = timeshift_buffer_plugin._scrub_orphaned_dirs(str(tmp_path), 300, _FakeLogger())

    second = started["uuid"]
    first = _ORPHAN_UUID if second == _ORPHAN_UUID_B else _ORPHAN_UUID_B
    assert removed == [first]
    assert stopped == [pids[first]]  # never the new pid 3000, nor the second's old one
    assert (tmp_path / second).is_dir()
    assert timeshift_buffer_plugin._read_owner_file(tmp_path / second)["pid"] == 3000


def test_live_owner_checks_liveness_against_the_recorded_start_ticks(tmp_path, monkeypatch):
    _write_owner(tmp_path, pid=4242, ticks=777, started_at=time.time() - 3600)
    seen = []

    def alive(pid, ticks=None):
        seen.append((pid, ticks))
        return ticks == 777  # alive only as the process that started at tick 777

    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", alive)
    assert timeshift_buffer_plugin._live_owner(tmp_path)["pid"] == 4242
    assert seen == [(4242, 777)]
    # A pid reused by another process (a different start time) is not the owner, and so does not
    # override the directory's own age either.
    _write_owner(tmp_path, pid=4242, ticks=888, started_at=time.time() - 3600)
    assert timeshift_buffer_plugin._live_owner(tmp_path) is None


# ---------------------------------------------------------------------
# The reaper's Redis client and error log (the 2026-10-04 fifth hardening sweep)
# ---------------------------------------------------------------------


class _NStopEvent:
    """Lets the reaper loop run `ticks` ticks, then ends it."""

    def __init__(self, ticks):
        self.ticks_left = ticks

    def is_set(self):
        return self.ticks_left <= 0

    def wait(self, _timeout):
        self.ticks_left -= 1


def test_reaper_loop_survives_a_tick_with_no_redis_client_and_recovers(monkeypatch):
    # The client used to be fetched once at thread start, outside the try: a None (or a connection that
    # later died) then failed every tick for the life of the worker, and the thread never restarted.
    client = _FakeReaperRedisClient()
    answers = iter([None, RuntimeError("redis is down"), client])

    def redis_now():
        answer = next(answers)
        if isinstance(answer, Exception):
            raise answer
        return answer

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", redis_now)
    iter_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: iter_calls.append(1) or [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])

    timeshift_buffer_plugin._reaper_loop(lambda: {"idle_timeout_seconds": 30}, _FakeLogger(), _NStopEvent(3))

    assert iter_calls == [1]  # the third tick got a client and did the real work


def test_reaper_loop_logs_a_repeating_failure_once_and_reports_recovery(monkeypatch):
    client = _FakeReaperRedisClient()
    outcomes = iter([RuntimeError("down")] * 5 + [client])

    def redis_now():
        outcome = next(outcomes)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", redis_now)
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    logger = _FakeLogger()

    timeshift_buffer_plugin._reaper_loop(lambda: {"idle_timeout_seconds": 30}, logger, _NStopEvent(6))

    failures = [msg for level, msg in logger.calls if level == "exception"]
    assert len(failures) == 1  # five failing ticks, one traceback
    recovered = [msg for level, msg in logger.calls if level == "info" and "recovered after 4" in msg]
    assert len(recovered) == 1


def test_should_log_reaper_error_is_the_first_then_once_per_interval():
    f = timeshift_buffer_plugin._should_log_reaper_error
    assert f(None, 0.0, 300)
    assert not f(100.0, 399.9, 300)
    assert f(100.0, 400.0, 300)
    assert f(100.0, 10_000.0, 300)
    assert not f(100.0, 100.0, 300)


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-04 fifth hardening sweep
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "name,servable",
    [
        ("live.m3u8", True),
        ("seg_00001.ts", True),
        ("seg_0.ts", True),
        ("live.m3u8.tmp", False),  # ffmpeg's real temp name while it rewrites the playlist
        ("seg_.ts", False),
        ("seg_1.ts.bak", False),
        ("xseg_1.ts", False),
        ("seg_1.tsx", False),
        ("seg_a.ts", False),
        ("ffmpeg.log", False),
        ("ffmpeg.owner.json", False),
        ("", False),
    ],
)
def test_is_servable_name(name, servable):
    assert timeshift_buffer_plugin._is_servable_name(name) is servable


def test_proc_start_ticks_needs_the_full_set_of_fields():
    ticks = timeshift_buffer_plugin._proc_start_ticks_from_stat
    # After the last ")" the fields are state (3) .. starttime (22): 20 fields end exactly at starttime.
    twenty = "1 (ffmpeg) S " + " ".join(str(n) for n in range(4, 22))
    assert len(twenty.rsplit(")", 1)[1].split()) == 19  # one short: no starttime
    assert ticks(twenty) is None
    full = "1 (ffmpeg) S " + " ".join(str(n) for n in range(4, 23))
    assert len(full.rsplit(")", 1)[1].split()) == 20
    assert ticks(full) == 22  # field 22 of the stat line, index 19 after the comm
    assert ticks("garbage with no paren") is None
    assert ticks("1 (x) S " + " ".join(["a"] * 25)) is None  # non-numeric starttime


def test_classify_existing_buffer_stopping_grace_boundary():
    now = 1000.0
    grace = timeshift_buffer_plugin._TEARDOWN_GRACE_SECONDS
    classify = timeshift_buffer_plugin._classify_existing_buffer

    def state(age):
        return {"stopping": True, "stopping_since": now - age}

    # Strictly inside the grace period: still "stopping"; at exactly the period it has lapsed.
    assert classify(state(grace - 0.001), True, now=now) == "stopping"
    assert classify(state(grace), True, now=now) == "stopping"  # alive: a stale marker on a live ffmpeg
    assert classify(state(grace), False, now=now) == "dead"
    assert classify(state(grace - 0.001), False, now=now) == "stopping"


def test_prune_stale_viewers_keeps_a_viewer_exactly_at_the_idle_timeout():
    prune = timeshift_buffer_plugin._prune_stale_viewers
    state = {"viewers": ["a", "b"], "viewer_heartbeats": {"a": 970.0, "b": 969.9}}
    assert prune(state, 30, now=1000.0) is True
    assert state["viewers"] == ["a"]  # "a" is exactly 30 s old: kept; "b" is just past it


def test_an_orphan_exactly_at_the_minimum_age_is_one(tmp_path, monkeypatch):
    orphan = tmp_path / _ORPHAN_UUID
    orphan.mkdir()
    now = 10_000.0
    os.utime(orphan, (now - 300, now - 300))
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    assert timeshift_buffer_plugin._is_untracked_orphan(orphan, 300, now) is True
    assert timeshift_buffer_plugin._is_untracked_orphan(orphan, 300, now - 0.5) is False


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-04 sixth hardening sweep
# ---------------------------------------------------------------------


def test_run_stop_buffer_prunes_a_phantom_viewer_so_the_last_real_one_tears_it_down(monkeypatch, tmp_path):
    """The documented phantom-viewer leak: viewer B crashed without stopping and was kept "alive" by
    A's fetches. When A stops, B's stale heartbeat is pruned in the same update, so the buffer ends."""
    now = time.time()
    state = {"channel_uuid": _UUID, "viewers": ["A", "B"], "viewer_heartbeats": {"A": now, "B": now - 1000}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: None)
    teardown_calls = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s["viewers"])
    )

    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID, "viewer_id": "A"}, {}, tmp_path)

    assert result == {"status": "ok", "message": "Buffer stopped"}
    assert teardown_calls == [[]]  # torn down with no viewers left, the stale one pruned too


def test_a_stale_config_teardown_does_not_retake_the_start_lock_it_already_holds(monkeypatch, start_locks):
    """_start_buffer_locked() runs inside the channel's start lock. If the state has vanished by the time it
    tears a stale-config buffer down, the teardown must still stop that ffmpeg and remove its (old
    storage_path) files -- taking the lock again would find it held and skip both, leaking the process."""
    existing = {"channel_uuid": _UUID, "pid": 100, "started_at": 5.0, "http_port": 1111, "storage_path": "/old"}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: existing)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_update_buffer_state", lambda uuid, fn: ("absent", None))
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    steps = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: steps.append("stop"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: steps.append("remove"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: steps.append("delete"))

    def stop_here(*a, **k):
        raise RuntimeError("fresh start reached")

    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", stop_here)
    start_locks.held.add(_UUID)  # the lock _start_buffer() holds around this call

    with contextlib.suppress(RuntimeError):
        timeshift_buffer_plugin.Plugin()._start_buffer_locked(
            _UUID, {}, {"http_port": 9192, "storage_path": "/new"}, _FakeLogger()
        )

    assert steps == ["stop", "remove", "delete"]
    # never tried to take the channel's lock again (the global start-slot lock is a different lock, and is taken
    # for the fresh start that follows)
    slot = timeshift_buffer_plugin._START_SLOT_LOCK_ID
    assert [kind for kind, uuid in start_locks.log if kind != "busy" and uuid != slot] == []


def test_reaper_loop_reports_a_new_failure_after_a_recovery(monkeypatch):
    # fail, recover, fail again within the log interval: the second failure is a new incident and
    # gets its own traceback rather than being swallowed by the first one's suppression window.
    client = _FakeReaperRedisClient()
    outcomes = iter([RuntimeError("down"), client, RuntimeError("down again")])

    def redis_now():
        outcome = next(outcomes)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", redis_now)
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    logger = _FakeLogger()

    timeshift_buffer_plugin._reaper_loop(lambda: {"idle_timeout_seconds": 30}, logger, _NStopEvent(3))

    assert len([m for level, m in logger.calls if level == "exception"]) == 2


def test_build_ffmpeg_command_never_reads_stdin():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://127.0.0.1:9191/proxy/ts/stream/x", None, 2, 60, Path("/tmp/p/live.m3u8"), 30, "seg_%05d.ts"
    )
    # A backgrounded ffmpeg that reads the terminal can be suspended by SIGTTIN.
    assert "-nostdin" in cmd
    assert cmd.index("-nostdin") < cmd.index("-i")


# ---------------------------------------------------------------------
# The atomic start-lock release (the 2026-10-04 seventh hardening sweep)
# ---------------------------------------------------------------------


class _ScriptingLockClient(_FakeReaperRedisClient):
    """A fake Redis whose eval() runs the compare-and-delete the release script does, and which counts
    plain get()/delete() calls so a test can tell the atomic path from the two-step one."""

    def __init__(self):
        super().__init__()
        self.get_calls = 0
        self.delete_calls = 0
        self.scripts = []

    def get(self, key):
        self.get_calls += 1
        return super().get(key)

    def delete(self, key):
        self.delete_calls += 1
        super().delete(key)

    def eval(self, script, numkeys, *args):
        self.scripts.append(script)
        key, token = args
        if self.store.get(key) == token:
            self.store.pop(key)
            return 1
        return 0


def test_release_start_buffer_lock_is_one_atomic_script_when_redis_allows_it(monkeypatch):
    client = _ScriptingLockClient()
    key = timeshift_buffer_plugin._start_buffer_lock_key(_UUID)
    client.store[key] = "mine"
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")

    assert key not in client.store
    assert len(client.scripts) == 1
    assert (client.get_calls, client.delete_calls) == (0, 0)  # no GET-then-DEL window


def test_release_start_buffer_lock_never_removes_someone_elses_lock_by_script(monkeypatch):
    client = _ScriptingLockClient()
    key = timeshift_buffer_plugin._start_buffer_lock_key(_UUID)
    client.store[key] = "the-next-holder"
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")

    assert client.store[key] == "the-next-holder"


def test_release_start_buffer_lock_falls_back_when_redis_refuses_scripts(monkeypatch):
    class ResponseError(Exception):
        pass

    class _NoScripts(_FakeReaperRedisClient):
        def eval(self, *args):
            raise ResponseError("unknown command 'eval'")

    client = _NoScripts()
    key = timeshift_buffer_plugin._start_buffer_lock_key(_UUID)
    client.store[key] = "mine"
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)

    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")
    assert key not in client.store

    client.store[key] = "other"
    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")
    assert client.store[key] == "other"


def test_release_start_buffer_lock_propagates_a_connection_error(monkeypatch):
    class _Down(_FakeReaperRedisClient):
        def eval(self, *args):
            raise ConnectionError("redis is down")

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _Down())
    with pytest.raises(ConnectionError):
        timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")


# ---------------------------------------------------------------------
# _get_live_manifest / _parse_live_playlist_lines mutation survivors (the 2026-10-04 seventh sweep)
# ---------------------------------------------------------------------


def test_the_newest_segments_size_is_remembered_when_it_is_re_sampled_on_the_fast_path(tmp_path):
    """The documented "Packet corrupt" incident: the newest segment first sampled before it was fully
    flushed. The fast path (playlist unchanged) re-stats it and must UPDATE the cache, or the next
    segment's arrival makes it a non-newest entry reused at its stale, too-small size."""
    channel_dir = tmp_path / _UUID
    state = _make_state(tmp_path)
    _write_playlist(channel_dir, 0, [("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 5, "2.0")])
    first = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["byte_size"] for s in first["segments"]] == [10, 5]

    (channel_dir / "seg1.ts").write_bytes(b"b" * 50)  # finished flushing; the playlist is untouched
    second = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())  # the fast path
    assert [s["byte_size"] for s in second["segments"]] == [10, 50]

    _write_playlist(
        channel_dir, 0, [("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 50, "2.0"), ("seg2.ts", b"c", "2.0")]
    )
    third = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["byte_size"] for s in third["segments"]] == [10, 50, 1]  # seg1 reused at 50, not 5


def test_a_state_with_no_access_token_never_trusts_the_manifest_cache(tmp_path):
    channel_dir = tmp_path / _UUID
    state = _make_state(tmp_path, access_token=None)
    _write_playlist(channel_dir, 0, [("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 5, "2.0")])
    playlist = channel_dir / "live.m3u8"
    stat = playlist.stat()
    timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())

    (channel_dir / "seg0.ts").write_bytes(b"a" * 99)  # a different instance's file under the same names
    os.utime(playlist, ns=(stat.st_atime_ns, stat.st_mtime_ns))  # playlist looks unchanged

    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert manifest["segments"][0]["byte_size"] == 99  # re-stat'd, not served from the cache


def test_a_reused_sequence_under_a_different_filename_is_re_stated(tmp_path):
    channel_dir = tmp_path / _UUID
    state = _make_state(tmp_path)
    _write_playlist(channel_dir, 0, [("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 5, "2.0")])
    timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())

    _write_playlist(channel_dir, 0, [("other0.ts", b"x" * 77, "2.0"), ("seg1.ts", b"b" * 5, "2.0")])
    manifest = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())

    assert [(s["filename"], s["byte_size"]) for s in manifest["segments"]] == [("other0.ts", 77), ("seg1.ts", 5)]


def test_ended_is_false_for_a_state_with_no_pid_and_liveness_uses_the_start_ticks(tmp_path, monkeypatch):
    channel_dir = tmp_path / _UUID
    _write_playlist(channel_dir, 0, [("seg0.ts", b"a" * 10, "2.0")])
    seen = []

    def alive(pid, ticks=None):
        seen.append((pid, ticks))
        return ticks == 111  # only the ffmpeg that started at tick 111 is "alive"

    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", alive)
    no_pid = _make_state(tmp_path, pid=None)
    assert timeshift_buffer_plugin._get_live_manifest(no_pid, _FakeLogger())["ended"] is False
    assert seen == []  # nothing to check without a pid

    live_state = dict(_make_state(tmp_path, pid=4242), pid_start_ticks=111)
    assert timeshift_buffer_plugin._get_live_manifest(live_state, _FakeLogger())["ended"] is False
    recycled = dict(_make_state(tmp_path, pid=4242), pid_start_ticks=222)  # the pid now belongs to another process
    assert timeshift_buffer_plugin._get_live_manifest(recycled, _FakeLogger())["ended"] is True
    assert (4242, 111) in seen and (4242, 222) in seen


def test_a_recycled_pid_with_no_playlist_is_a_failed_buffer(tmp_path, monkeypatch):
    (tmp_path / _UUID).mkdir()
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: ticks == 111)
    recycled = dict(_make_state(tmp_path, pid=4242), pid_start_ticks=222)
    with pytest.raises(timeshift_buffer_plugin.BufferFailedError):
        timeshift_buffer_plugin._get_live_manifest(recycled, _FakeLogger())
    still_ours = dict(_make_state(tmp_path, pid=4242), pid_start_ticks=111)
    with pytest.raises(RuntimeError) as exc_info:
        timeshift_buffer_plugin._get_live_manifest(still_ours, _FakeLogger())
    assert not isinstance(exc_info.value, timeshift_buffer_plugin.BufferFailedError)


def test_parse_live_playlist_lines_takes_the_first_media_sequence_and_strips_filenames():
    lines = [
        "#EXTM3U",
        "#EXT-X-MEDIA-SEQUENCE:7",
        "#EXT-X-MEDIA-SEQUENCE:900",
        "#EXTINF:2.0,",
        "seg7.ts   ",
        "#EXTINF:2.0,",
        "seg8.ts",
    ]
    media_sequence, parsed = timeshift_buffer_plugin._parse_live_playlist_lines(lines)
    assert media_sequence == 7
    assert parsed == [(7, "seg7.ts", 2000), (8, "seg8.ts", 2000)]


# ---------------------------------------------------------------------
# The eighth hardening sweep's Python findings and surviving mutations
# ---------------------------------------------------------------------


def test_a_wall_clock_step_does_not_disturb_the_sigterm_deadline(monkeypatch):
    """The 2 s SIGTERM grace runs on the monotonic clock: an NTP step of the wall clock (here a jump of a
    day forward, which used to SIGKILL at once) must not matter to a process that exits promptly."""
    calls = []

    def fake_killpg(pid, sig):
        calls.append((pid, sig))
        if sig == 0:
            raise ProcessLookupError

    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", fake_killpg)
    wall = iter([0.0, 86400.0, 86400.0, 86400.0])
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: next(wall, 86400.0))

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, _FakeLogger())

    assert calls == [(1234, timeshift_buffer_plugin.signal.SIGTERM), (1234, 0)]  # no SIGKILL


def test_stop_ffmpeg_gives_a_slow_process_the_full_two_second_grace(monkeypatch):
    """A process that is still alive 1.5 s after SIGTERM and gone by 1.7 s is never SIGKILLed: the
    escalation deadline is 2 s, not shorter."""
    calls = []
    clock = {"now": 0.0}
    monkeypatch.setattr(timeshift_buffer_plugin.time, "monotonic", lambda: clock["now"])
    monkeypatch.setattr(timeshift_buffer_plugin.time, "sleep", lambda s: clock.__setitem__("now", clock["now"] + s))
    monkeypatch.setattr(timeshift_buffer_plugin.os, "killpg", lambda pid, sig: calls.append((pid, sig)))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: clock["now"] < 1.7)

    timeshift_buffer_plugin._stop_ffmpeg({"pid": 1234}, _FakeLogger())

    assert calls == [(1234, timeshift_buffer_plugin.signal.SIGTERM)]


def test_a_subclass_of_response_error_still_falls_back_for_the_lock_release_and_the_cas(monkeypatch):
    # An ACL without @scripting answers NOPERM, which redis-py raises as NoPermissionError, a SUBCLASS of
    # ResponseError: matching the class name exactly re-raised it.
    class ResponseError(Exception):
        pass

    class NoPermissionError(ResponseError):
        pass

    class _Denied(_FakeReaperRedisClient):
        def eval(self, *args):
            raise NoPermissionError("NOPERM this user has no permissions to run the 'eval' command")

    assert timeshift_buffer_plugin._is_redis_response_error(NoPermissionError("x"))
    assert timeshift_buffer_plugin._is_redis_response_error(ResponseError("x"))
    assert not timeshift_buffer_plugin._is_redis_response_error(ConnectionError("x"))
    assert not timeshift_buffer_plugin._is_redis_response_error(ValueError("x"))

    client = _Denied()
    key = timeshift_buffer_plugin._start_buffer_lock_key(_UUID)
    client.store[key] = "mine"
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    timeshift_buffer_plugin._release_start_buffer_lock(_UUID, "mine")  # must not raise
    assert key not in client.store


def test_int_setting_tolerates_non_numeric_values_and_an_infinite_float():
    f = timeshift_buffer_plugin._int_setting
    assert f({"k": None}, "k", 5) == 5
    assert f({"k": []}, "k", 5) == 5
    assert f({"k": {}}, "k", 5) == 5
    assert f({"k": ""}, "k", 5) == 5
    assert f({"k": float("inf")}, "k", 5) == 5  # int(inf) raises OverflowError
    assert f({"k": float("nan")}, "k", 5) == 5
    assert f({"k": "7"}, "k", 5) == 7


def test_a_scope_id_in_client_ip_is_rejected_on_its_own():
    # "%" alone, with no CRLF, must be refused by the "%" guard itself (an IPv6 zone is never a remote
    # client's attribution address).
    for value in ("fe80::1%eth0", "fe80::1%1", "::1%"):
        assert timeshift_buffer_plugin._stream_attribution_headers({"client_ip": value}, _FakeLogger()) is None


def test_build_ffmpeg_command_pins_the_log_level():
    cmd = timeshift_buffer_plugin._build_ffmpeg_command(
        "http://127.0.0.1:9191/proxy/ts/stream/x", None, 2, 60, Path("/tmp/p/live.m3u8"), 30, "seg_%05d.ts"
    )
    i = cmd.index("-loglevel")
    assert cmd[i + 1] == "warning"


@pytest.mark.parametrize(
    "state,zombie",
    [("Z", True), ("S", False), ("R", False), ("D", False), ("X", False), ("T", False)],
)
def test_is_zombie_proc_stat_only_for_state_z(state, zombie):
    assert timeshift_buffer_plugin._is_zombie_proc_stat(f"123 (ffmpeg) {state} 1 2 3") is zombie


# ---------------------------------------------------------------------
# Mutation survivors from the 2026-10-04 ninth hardening sweep
# ---------------------------------------------------------------------


def test_reaper_loop_keeps_a_buffer_whose_heartbeat_is_exactly_the_idle_timeout_old(monkeypatch):
    def reaped(heartbeat_age):
        client = _FakeReaperRedisClient()
        monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
        monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
        state = {"channel_uuid": "abc", "last_heartbeat": 1000.0 - heartbeat_age}
        monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
        monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
        calls = []
        monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: calls.append(s))
        monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
        settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": "/data/timeshift"}  # noqa: E731
        timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())
        return bool(calls)

    assert reaped(30.0) is False  # exactly the timeout: still within it
    assert reaped(30.5) is True


def test_an_untracked_teardown_leaves_a_buffer_restarted_with_the_same_pid_alone(monkeypatch):
    # A restarted buffer can be handed a recycled pid: only pid AND started_at together identify the
    # instance, so the same pid with a different start time is a NEW buffer that owns the directory.
    old = {"channel_uuid": "abc", "pid": 4242, "started_at": 100.0}
    restarted = {"channel_uuid": "abc", "pid": 4242, "started_at": 200.0}
    monkeypatch.setattr(timeshift_buffer_plugin, "_acquire_start_buffer_lock", lambda uuid: "token")
    monkeypatch.setattr(timeshift_buffer_plugin, "_release_start_buffer_lock", lambda uuid, token: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: restarted)
    stopped = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: stopped.append(s))
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: stopped.append("files"))
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: stopped.append("state"))

    assert timeshift_buffer_plugin._teardown_untracked_buffer(old, _FakeLogger()) is False
    assert stopped == []

    # The very same instance is torn down.
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(old))
    assert timeshift_buffer_plugin._teardown_untracked_buffer(old, _FakeLogger()) is True
    assert stopped == [old, "files", "state"]


def test_a_directory_with_an_old_mtime_but_a_freshly_started_owner_is_not_an_orphan(tmp_path, monkeypatch):
    # The owner's start time, not the directory's age, decides while its ffmpeg is alive: taking the
    # older of the two would reap a buffer in the window between spawning ffmpeg and saving its state.
    directory = tmp_path / _ORPHAN_UUID
    _write_owner(directory, started_at=time.time() - 5)
    long_ago = time.time() - 10_000
    os.utime(directory, (long_ago, long_ago))
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, ticks=None: True)

    assert timeshift_buffer_plugin._is_untracked_orphan(directory, 300, time.time()) is False
    assert timeshift_buffer_plugin._find_orphaned_channel_dirs(str(tmp_path), min_age_seconds=300) == []


# ---------------------------------------------------------------------
# The 2026-10-05 tenth hardening sweep
# ---------------------------------------------------------------------


def test_the_manual_test_button_prints_the_playlist_url_with_the_buffers_token(monkeypatch, tmp_path):
    """The file server answers 403 without ?token=, and the Plugins page shows only the message, so a
    manual check following the README always failed. The button calls with empty params."""
    existing = {
        "channel_uuid": _UUID,
        "pid": 1,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "access_token": "tok123",
        "viewers": [],
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: None)

    result, _logger = _run(monkeypatch, "start_buffer", {}, {"test_channel_uuid": _UUID}, tmp_path)

    assert result["status"] == "ok"
    assert f":9192/{_UUID}/live.m3u8?token=tok123" in result["message"]


def test_an_api_caller_never_gets_the_token_in_the_message(monkeypatch, tmp_path):
    # The addon gets the token as a field and its message is logged.
    existing = {
        "channel_uuid": _UUID,
        "pid": 1,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "access_token": "tok123",
        "viewers": [],
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: None)

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path)

    assert result["access_token"] == "tok123"
    assert "tok123" not in result["message"]


def test_a_freshly_started_manual_buffer_also_prints_the_url(monkeypatch, tmp_path):
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    new_state = {"channel_uuid": _UUID, "pid": 5555, "http_port": 9192, "playlist_route": f"/{_UUID}/live.m3u8"}
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: dict(new_state))
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: saved.update(state))

    result, _logger = _run(monkeypatch, "start_buffer", {}, {"test_channel_uuid": _UUID}, tmp_path)

    assert result["already_running"] is False
    assert f"?token={saved['access_token']}" in result["message"]
    assert result["access_token"] == saved["access_token"]


def test_is_zombie_proc_stat_with_nothing_after_the_command_name():
    assert timeshift_buffer_plugin._is_zombie_proc_stat("123 (ffmpeg)") is False
    assert timeshift_buffer_plugin._is_zombie_proc_stat("123 (ffmpeg) Z 1") is True


def test_a_regular_file_named_like_a_channel_is_not_an_orphan_directory(tmp_path, monkeypatch):
    path = tmp_path / _ORPHAN_UUID
    path.write_text("not a directory")
    long_ago = time.time() - 10_000
    os.utime(path, (long_ago, long_ago))
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    assert timeshift_buffer_plugin._is_untracked_orphan(path, 300, time.time()) is False


def test_a_candidate_whose_stat_fails_is_not_an_orphan(monkeypatch):
    class _Vanished:
        name = _ORPHAN_UUID

        def is_dir(self):
            return True

        def stat(self):
            raise OSError("gone")

    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    assert timeshift_buffer_plugin._is_untracked_orphan(_Vanished(), 300, time.time()) is False


# ---------------------------------------------------------------------
# The 2026-10-05 twelfth hardening sweep
# ---------------------------------------------------------------------


def test_the_manifest_cache_is_bounded_and_keeps_the_most_recently_used_channels(tmp_path, monkeypatch):
    limit = timeshift_buffer_plugin._MANIFEST_CACHE_MAX_ENTRIES
    uuids = [f"00000000-0000-4000-8000-{i:012d}" for i in range(limit + 3)]
    for uuid in uuids:
        _write_playlist(tmp_path / uuid, media_sequence=0, segments=[("seg0.ts", b"x" * 10, "2.0")])
        timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path, channel_uuid=uuid), _FakeLogger())
    cache = timeshift_buffer_plugin._manifest_cache
    assert len(cache) == limit
    assert list(cache) == uuids[-limit:]  # the three least recently used were dropped

    # An evicted channel rebuilds correctly on its next call, and becomes the most recently used.
    manifest = timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path, channel_uuid=uuids[0]), _FakeLogger())
    assert manifest["segments"][0]["byte_size"] == 10
    assert list(cache)[-1] == uuids[0]
    assert len(cache) == limit
    # Touching an entry that is already cached moves it to the end instead of evicting another.
    timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path, channel_uuid=uuids[-2]), _FakeLogger())
    assert len(cache) == limit


def test_remembering_a_cached_channel_again_makes_it_the_most_recently_used(monkeypatch):
    # Least recently used, not first in: without the pop before the insert a channel in active use kept its
    # original slot and was the first to go (nothing failed -- it only rebuilt its manifest every time).
    monkeypatch.setattr(timeshift_buffer_plugin, "_manifest_cache", {})
    monkeypatch.setattr(timeshift_buffer_plugin, "_MANIFEST_CACHE_MAX_ENTRIES", 3)
    remember = timeshift_buffer_plugin._remember_manifest
    for key in ("a", "b", "c"):
        remember(key, {"n": 0})
    remember("a", {"n": 1})
    assert list(timeshift_buffer_plugin._manifest_cache) == ["b", "c", "a"]
    assert timeshift_buffer_plugin._manifest_cache["a"] == {"n": 1}  # the new entry replaced the old
    remember("d", {"n": 0})
    assert list(timeshift_buffer_plugin._manifest_cache) == ["c", "a", "d"]  # "b", not the one just reused, went


def test_a_playlist_rewritten_to_the_same_size_with_a_new_mtime_is_reparsed(tmp_path):
    # ffmpeg's sliding window makes a same-size rewrite the normal case, so the mtime is what has to
    # tell the cache the list changed.
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir, media_sequence=0, segments=[("seg0.ts", b"a" * 10, "2.0"), ("seg1.ts", b"b" * 20, "2.0")]
    )
    state = _make_state(tmp_path)
    first = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["filename"] for s in first["segments"]] == ["seg0.ts", "seg1.ts"]

    playlist = channel_dir / "live.m3u8"
    old_size = playlist.stat().st_size
    (channel_dir / "seg2.ts").write_bytes(b"c" * 30)
    playlist.write_text("#EXT-X-MEDIA-SEQUENCE:1\n#EXTINF:2.0,\nseg1.ts\n#EXTINF:2.0,\nseg2.ts\n")
    assert playlist.stat().st_size == old_size
    later = playlist.stat().st_mtime_ns + 5_000_000_000
    os.utime(playlist, ns=(later, later))

    second = timeshift_buffer_plugin._get_live_manifest(state, _FakeLogger())
    assert [s["filename"] for s in second["segments"]] == ["seg1.ts", "seg2.ts"]
    assert second["media_sequence"] == 1


def test_a_listed_segment_that_is_gone_does_not_drop_the_ones_after_it(tmp_path):
    channel_dir = tmp_path / _UUID
    _write_playlist(
        channel_dir,
        media_sequence=10,
        segments=[("seg1.ts", b"a" * 10, "2.0"), ("seg2.ts", b"b" * 10, "2.0"), ("seg3.ts", b"c" * 10, "2.0")],
    )
    (channel_dir / "seg2.ts").unlink()
    manifest = timeshift_buffer_plugin._get_live_manifest(_make_state(tmp_path), _FakeLogger())
    assert [(s["filename"], s["sequence"]) for s in manifest["segments"]] == [("seg1.ts", 10), ("seg3.ts", 12)]


def test_a_trailing_extinf_with_no_uri_after_it_yields_no_entry_and_no_error():
    media_sequence, entries = timeshift_buffer_plugin._parse_live_playlist_lines(
        ["#EXT-X-MEDIA-SEQUENCE:3", "#EXTINF:2.0,", "seg3.ts", "#EXTINF:2.0,"]
    )
    assert media_sequence == 3
    assert [e[1] for e in entries] == ["seg3.ts"]


def test_stop_buffer_reports_a_contended_state_write_as_retryable(monkeypatch, tmp_path):
    state = {"channel_uuid": _UUID, "viewers": ["v1", "v2"], "viewer_heartbeats": {}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(state))
    monkeypatch.setattr(timeshift_buffer_plugin, "_update_buffer_state", lambda uuid, mutate: ("contended", None))
    result, _logger = _run(monkeypatch, "stop_buffer", {"channel_uuid": _UUID, "viewer_id": "v1"}, {}, tmp_path)
    assert result["status"] == "error"
    assert result["retryable"] is True


# ---------------------------------------------------------------------
# max_concurrent_buffers across channels (the global start-slot lock)
# ---------------------------------------------------------------------


def test_run_start_buffer_refuses_when_another_start_holds_the_slot_lock(monkeypatch, tmp_path):
    """The count of running buffers and the registration of a new one are one step: a start for a different
    channel that finds the slot lock taken answers retryable, before counting or spawning anything."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    spawned = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", lambda *a, **k: spawned.append(1))
    counted = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: counted.append(1) or [])

    def acquire(channel_uuid):
        return None if channel_uuid == timeshift_buffer_plugin._START_SLOT_LOCK_ID else "tok"

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID}, {}, tmp_path, acquire_lock=acquire)

    assert result["status"] == "error"
    assert result["retryable"] is True
    assert spawned == [] and counted == []


def test_run_start_buffer_releases_the_slot_lock_on_every_path(monkeypatch, tmp_path):
    for outcome in ("at_max", "no_ffmpeg", "boom"):
        released = []
        monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
        monkeypatch.setattr(
            timeshift_buffer_plugin,
            "_list_buffer_keys",
            lambda outcome=outcome: ["k"] * (9 if outcome == "at_max" else 0),
        )

        def start(*a, outcome=outcome, **k):
            raise FileNotFoundError() if outcome == "no_ffmpeg" else RuntimeError("boom")

        monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", start)
        _run(
            monkeypatch,
            "start_buffer",
            {"channel_uuid": _UUID},
            {},
            tmp_path,
            acquire_lock=lambda channel_uuid: "t",
            release_lock=lambda channel_uuid, token, released=released: released.append(channel_uuid),
        )
        assert timeshift_buffer_plugin._START_SLOT_LOCK_ID in released, outcome


def test_start_slot_lock_id_cannot_be_a_channel_uuid():
    # _channel_dir() validates uuids, so the reserved name can never collide with a real channel's own lock key.
    with pytest.raises(ValueError):
        timeshift_buffer_plugin._channel_dir("/data/timeshift", timeshift_buffer_plugin._START_SLOT_LOCK_ID)


# ---------------------------------------------------------------------
# _cap_ffmpeg_log
# ---------------------------------------------------------------------


def test_cap_ffmpeg_log_leaves_a_small_log_alone(tmp_path):
    log = tmp_path / "ffmpeg.log"
    log.write_bytes(b"line one\nline two\n")
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path, max_bytes=1000, keep_bytes=100) is False
    assert log.read_bytes() == b"line one\nline two\n"


def test_cap_ffmpeg_log_exactly_at_the_cap_is_not_trimmed(tmp_path):
    (tmp_path / "ffmpeg.log").write_bytes(b"x" * 1000)
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path, max_bytes=1000, keep_bytes=100) is False
    (tmp_path / "ffmpeg.log").write_bytes(b"x" * 1001)
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path, max_bytes=1000, keep_bytes=100) is True


def test_cap_ffmpeg_log_keeps_the_last_whole_lines_and_a_marker(tmp_path):
    log = tmp_path / "ffmpeg.log"
    lines = [b"warning number %05d\n" % i for i in range(200)]
    log.write_bytes(b"".join(lines))
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path, max_bytes=1000, keep_bytes=100) is True
    data = log.read_bytes()
    assert data.startswith(b"[timeshift_buffer: ffmpeg.log exceeded 1000 bytes")
    body = data.split(b"\n", 1)[1]
    assert body.endswith(lines[-1])
    assert all(line in lines for line in body.splitlines(keepends=True))  # only whole original lines
    assert len(body) <= 100


def test_cap_ffmpeg_log_keeps_writing_at_the_end_for_a_writer_that_holds_the_file_open(tmp_path):
    log = tmp_path / "ffmpeg.log"
    with open(log, "ab") as writer:  # what ffmpeg's stdout is: O_APPEND
        writer.write(b"y" * 5000 + b"\n")
        writer.flush()
        assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path, max_bytes=1000, keep_bytes=100) is True
        writer.write(b"after the trim\n")
        writer.flush()
    data = log.read_bytes()
    assert data.endswith(b"after the trim\n")
    assert b"\x00" not in data  # no sparse hole where the old offset would have landed


def test_cap_ffmpeg_log_ignores_a_missing_log_and_a_missing_directory(tmp_path):
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path) is False
    assert timeshift_buffer_plugin._cap_ffmpeg_log(tmp_path / "nope") is False


def test_reaper_loop_trims_the_ffmpeg_log_of_each_tracked_buffer(monkeypatch, tmp_path):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
    state = {"channel_uuid": _UUID, "last_heartbeat": 999}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    capped = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_cap_ffmpeg_log", lambda channel_dir: capped.append(channel_dir))

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": str(tmp_path)}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    assert capped == [timeshift_buffer_plugin._channel_dir(str(tmp_path), _UUID)]


def test_reaper_loop_survives_a_state_with_an_unusable_uuid_when_trimming(monkeypatch, tmp_path):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_iter_buffer_states", lambda: [{"channel_uuid": "../x", "last_heartbeat": 999}]
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    scrubbed = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: scrubbed.append(1) or [])

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": str(tmp_path)}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    assert scrubbed == [1]  # the tick went on to its scrub


def test_a_start_for_another_channel_during_a_start_cannot_pass_the_count(monkeypatch, tmp_path, start_locks):
    """max_concurrent_buffers = 1 with two different channels starting at once: while the first is between its
    count and its state write, the second must be told to retry rather than count the same (empty) set and
    spawn too. Before the global slot lock both saw room."""
    registered = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: list(registered))
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: registered.append(uuid))
    inner = []

    def start_ffmpeg(channel_uuid, params, settings, logger):
        if channel_uuid == _UUID:  # the first start is mid-spawn: a second channel's start arrives now
            inner.append(
                _run(
                    monkeypatch,
                    "start_buffer",
                    {"channel_uuid": _OTHER_UUID},
                    {"max_concurrent_buffers": 1},
                    tmp_path,
                    acquire_lock=start_locks.acquire,
                    release_lock=start_locks.release,
                )[0]
            )
        return {"http_port": 1, "playlist_route": "/x", "pid": 1}

    monkeypatch.setattr(timeshift_buffer_plugin, "_start_ffmpeg", start_ffmpeg)

    first, _logger = _run(
        monkeypatch,
        "start_buffer",
        {"channel_uuid": _UUID},
        {"max_concurrent_buffers": 1},
        tmp_path,
        acquire_lock=start_locks.acquire,
        release_lock=start_locks.release,
    )

    assert first["status"] == "ok"
    assert inner[0]["status"] == "error" and inner[0]["retryable"] is True
    assert registered == [_UUID]
    assert start_locks.held == set()  # everything released

    # once the first is registered, the same call is refused on the count itself, not as a lock miss
    again, _logger = _run(
        monkeypatch,
        "start_buffer",
        {"channel_uuid": _OTHER_UUID},
        {"max_concurrent_buffers": 1},
        tmp_path,
        acquire_lock=start_locks.acquire,
        release_lock=start_locks.release,
    )
    assert again["status"] == "error" and "retryable" not in again and "max_concurrent_buffers" in again["message"]


# ---------------------------------------------------------------------
# _shared_now: cross-worker timestamps are Redis's clock, not each host's wall clock
# ---------------------------------------------------------------------


class _TimedFakeRedis(_FakeReaperRedisClient):
    def __init__(self, now, **kwargs):
        super().__init__(**kwargs)
        self.now = now

    def time(self):
        return (int(self.now), int((self.now % 1) * 1_000_000))


def test_shared_now_reads_redis_time(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _TimedFakeRedis(1234.5))
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 9999.0)
    assert timeshift_buffer_plugin._shared_now() == pytest.approx(1234.5)


def test_shared_now_falls_back_to_the_wall_clock_when_redis_cannot_say(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 777.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: None)
    assert timeshift_buffer_plugin._shared_now() == 777.0
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _FakeReaperRedisClient())  # no TIME command
    assert timeshift_buffer_plugin._shared_now() == 777.0

    def broken():
        raise RuntimeError("redis is down")

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", broken)
    assert timeshift_buffer_plugin._shared_now() == 777.0

    class _TimeFails(_FakeReaperRedisClient):
        def time(self):
            raise ConnectionError("gone")

    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _TimeFails())
    assert timeshift_buffer_plugin._shared_now() == 777.0


def test_a_stepped_host_wall_clock_does_not_make_the_reaper_tear_down_a_watched_buffer(monkeypatch):
    """The buffer's last heartbeat was written 10 s ago on the shared clock. This worker's own wall clock has been
    stepped an hour forward (a VM resume, a manual change): with time.time() the reaper read the heartbeat as an
    hour old and tore the buffer down under its viewer."""
    client = _TimedFakeRedis(10_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 10_000.0 + 3600)  # the stepped host clock
    state = {"channel_uuid": _UUID, "last_heartbeat": 9_990.0, "viewers": ["a"], "viewer_heartbeats": {"a": 9_990.0}}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_cap_ffmpeg_log", lambda channel_dir: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    teardown_calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: teardown_calls.append(s))

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": "/data/timeshift"}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    assert teardown_calls == []
    assert state["viewers"] == ["a"]  # and the viewer was not pruned as stale either


def test_a_heartbeat_is_stamped_with_the_shared_clock(monkeypatch):
    client = _TimedFakeRedis(5_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1.0)  # a host clock that disagrees entirely
    stamped = {}
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_update_buffer_state",
        lambda uuid, fn: stamped.update(fn({"viewers": ["v"], "viewer_heartbeats": {}, "last_heartbeat": 0}))
        or ("ok", stamped),
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: {"channel_uuid": uuid})
    result, _logger = _run(monkeypatch, "heartbeat", {"channel_uuid": _UUID, "viewer_id": "v"}, {}, Path("/tmp"))
    assert stamped["last_heartbeat"] == pytest.approx(5_000.0)
    assert stamped["viewer_heartbeats"]["v"] == pytest.approx(5_000.0)


def test_a_reattach_stamps_the_viewer_and_the_buffer_with_the_shared_clock(monkeypatch, tmp_path):
    client = _TimedFakeRedis(20_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 3.0)  # a host clock that disagrees entirely
    existing = {
        "channel_uuid": _UUID,
        "pid": 1,
        "http_port": 9192,
        "playlist_route": f"/{_UUID}/live.m3u8",
        "access_token": "tok",
        "viewers": [],
        "viewer_heartbeats": {},
    }
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(existing))
    monkeypatch.setattr(timeshift_buffer_plugin, "_is_process_alive", lambda pid, *_a: True)
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, state: None)
    saved = {}

    def update(uuid, fn):
        written = fn(dict(existing))
        saved.update(written)
        return ("written", written)

    monkeypatch.setattr(timeshift_buffer_plugin, "_update_buffer_state", update)

    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": "v"}, {}, tmp_path)

    assert result["status"] == "ok"
    assert saved["last_heartbeat"] == pytest.approx(20_000.0)
    assert saved["viewer_heartbeats"]["v"] == pytest.approx(20_000.0)


def test_prune_stale_viewers_without_an_explicit_now_uses_the_shared_clock(monkeypatch):
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _TimedFakeRedis(1_000.0))
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1_000.0 + 3600)  # a stepped host clock
    state = {"viewers": ["a"], "viewer_heartbeats": {"a": 990.0}, "last_heartbeat": 990.0}
    assert timeshift_buffer_plugin._prune_stale_viewers(state, 30) is False
    assert state["viewers"] == ["a"]


def test_the_reapers_last_look_before_a_teardown_reads_the_shared_clock(monkeypatch):
    """The abort_if the reaper hands _teardown_buffer re-checks the freshest heartbeat just before tearing down: a
    stepped host clock must not make a heartbeat that landed seconds ago on the shared clock look an hour old."""
    client = _TimedFakeRedis(10_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    state = {"channel_uuid": _UUID, "last_heartbeat": 0}  # idle on the shared clock, so the reaper goes to tear it down
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    monkeypatch.setattr(timeshift_buffer_plugin, "_cap_ffmpeg_log", lambda channel_dir: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    captured = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_teardown_buffer", lambda s, logger, **kw: captured.update(kw))
    timeshift_buffer_plugin._reaper_loop(
        lambda: {"idle_timeout_seconds": 30, "storage_path": "/data/timeshift"}, _FakeLogger(), _OneShotStopEvent()
    )
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 10_000.0 + 3600)  # the host clock steps now
    assert captured["abort_if"]({"last_heartbeat": 9_995.0}) is True  # a heartbeat 5 s ago: someone is watching
    assert captured["abort_if"]({"last_heartbeat": 9_000.0}) is False  # 1000 s ago: still idle


def _use_shared_clock(monkeypatch, shared_now, host_now=3.0):
    """Redis says `shared_now`; this process's own wall clock says something else entirely."""
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: _TimedFakeRedis(shared_now))
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: host_now)


def test_a_new_buffer_is_stamped_with_the_shared_clock(monkeypatch, tmp_path):
    _use_shared_clock(monkeypatch, 40_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin.subprocess, "Popen", lambda cmd, **kw: _FakeProc())
    state = timeshift_buffer_plugin._start_ffmpeg(_UUID, {}, {"storage_path": str(tmp_path)}, _FakeLogger())
    assert state["last_heartbeat"] == pytest.approx(40_000.0)
    assert state["started_at"] == pytest.approx(40_000.0)


def test_the_first_viewer_of_a_new_buffer_is_stamped_with_the_shared_clock(monkeypatch, tmp_path):
    _use_shared_clock(monkeypatch, 41_000.0)
    monkeypatch.setattr(timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_list_buffer_keys", lambda: [])
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_start_ffmpeg",
        lambda *a, **k: {"http_port": 1, "playlist_route": "/x", "pid": 1, "last_heartbeat": 41_000.0},
    )
    saved = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: saved.update(s))
    result, _logger = _run(monkeypatch, "start_buffer", {"channel_uuid": _UUID, "viewer_id": "v"}, {}, tmp_path)
    assert result["status"] == "ok"
    assert saved["viewer_heartbeats"]["v"] == pytest.approx(41_000.0)


def test_get_live_manifest_stamps_the_calling_viewer_with_the_shared_clock(monkeypatch, tmp_path):
    _use_shared_clock(monkeypatch, 42_000.0)
    store = {
        _UUID: {
            "channel_uuid": _UUID,
            "http_port": 9192,
            "last_heartbeat": 0,
            "viewers": ["a"],
            "viewer_heartbeats": {"a": 0},
        }
    }
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_get_buffer_state", lambda uuid: dict(store[uuid]) if uuid in store else None
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: store.__setitem__(uuid, s))
    monkeypatch.setattr(
        timeshift_buffer_plugin,
        "_get_live_manifest",
        lambda s, logger: {"media_sequence": 0, "segments": [], "total_bytes": 0, "total_duration_ms": 0},
    )
    result, _logger = _run(monkeypatch, "get_live_manifest", {"channel_uuid": _UUID, "viewer_id": "a"}, {}, tmp_path)
    assert result["status"] == "ok"
    assert store[_UUID]["last_heartbeat"] == pytest.approx(42_000.0)
    assert store[_UUID]["viewer_heartbeats"]["a"] == pytest.approx(42_000.0)


def test_a_teardown_marks_stopping_with_the_shared_clock(monkeypatch):
    _use_shared_clock(monkeypatch, 43_000.0)
    state = {"channel_uuid": _UUID, "pid": 1, "started_at": 1.0}
    marked = {}
    outcomes = iter(["written", "contended"])

    def update(uuid, fn):
        marked.update(fn(dict(state)) or {})
        return (next(outcomes), None)

    monkeypatch.setattr(timeshift_buffer_plugin, "_update_buffer_state", update)
    written = {}
    monkeypatch.setattr(timeshift_buffer_plugin, "_set_buffer_state", lambda uuid, s: written.update(s))
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_ffmpeg", lambda s, logger: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_remove_channel_files", lambda s, logger: None)
    monkeypatch.setattr(timeshift_buffer_plugin, "_delete_buffer_state", lambda uuid: None)

    # the atomic marker the update function writes...
    timeshift_buffer_plugin._teardown_buffer(dict(state), _FakeLogger(), holds_start_lock=True)
    assert marked["stopping_since"] == pytest.approx(43_000.0)

    # ...and the direct write it falls back to when the atomic one lost the race ten times
    timeshift_buffer_plugin._teardown_buffer(dict(state), _FakeLogger(), holds_start_lock=True)
    assert written["stopping_since"] == pytest.approx(43_000.0)


def test_the_stopping_grace_is_measured_on_the_shared_clock(monkeypatch):
    _use_shared_clock(monkeypatch, 44_000.0, host_now=44_000.0 + 3600)  # the host clock has been stepped an hour
    existing = {"stopping": True, "stopping_since": 43_995.0}  # five seconds ago, shared clock
    assert timeshift_buffer_plugin._classify_existing_buffer(existing, is_alive=False) == "stopping"


def test_list_buffers_ages_are_on_the_shared_clock(monkeypatch, tmp_path):
    _use_shared_clock(monkeypatch, 45_000.0, host_now=45_000.0 + 3600)
    state = {"channel_uuid": _UUID, "started_at": 44_900.0, "last_heartbeat": 44_990.0, "viewers": ["a"]}
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: [state])
    result, _logger = _run(monkeypatch, "list_buffers", {}, {}, tmp_path)
    assert result["buffers"][0]["age_seconds"] == 100
    assert result["buffers"][0]["idle_seconds"] == 10


# ---------------------------------------------------------------------
# The 16th hardening sweep (0.8.12): stop() always stops the file server, an unreadable state does not stop the reaper,
# listing uses SCAN, and the log trim follows the buffer's own storage path.
# ---------------------------------------------------------------------


def test_plugin_stop_still_stops_the_http_server_when_redis_is_down(monkeypatch):
    calls = []
    monkeypatch.setattr(timeshift_buffer_plugin, "_reaper_stop_event", None)

    def redis_down():
        raise ConnectionError("redis is unreachable")

    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", redis_down)
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    monkeypatch.setattr(timeshift_buffer_plugin, "_stop_http_server", lambda logger: calls.append("stop_http_server"))

    with pytest.raises(ConnectionError):
        timeshift_buffer_plugin.Plugin().stop({"logger": _FakeLogger(), "settings": {}, "reason": "disable"})

    assert calls == ["stop_http_server"]  # the listener of a disabled plugin does not outlive it


class _FakeScanRedis:
    """A client that answers SCAN (scan_iter) and GET, and refuses KEYS."""

    def __init__(self, values):
        self.values = values
        self.scans = []

    def scan_iter(self, match=None, count=None):
        self.scans.append((match, count))
        text = lambda k: k.decode() if isinstance(k, bytes) else k  # noqa: E731
        return iter([k for k in self.values if text(k).startswith(match.rstrip("*"))])

    def keys(self, pattern):
        raise AssertionError("KEYS blocks the whole Redis; use SCAN")

    def get(self, key):
        return self.values.get(key)


def test_listing_buffer_keys_uses_scan_not_keys(monkeypatch):
    client = _FakeScanRedis(
        {"timeshift_buffer:buffer:a": b"{}", b"timeshift_buffer:buffer:b": b"{}", "other:key": b"{}"}
    )
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    keys = timeshift_buffer_plugin._list_buffer_keys()
    assert keys == ["timeshift_buffer:buffer:a", "timeshift_buffer:buffer:b"]  # bytes keys are decoded
    assert client.scans and client.scans[0][0] == timeshift_buffer_plugin._buffer_key("*")


def test_a_key_that_scan_returns_twice_is_listed_once(monkeypatch):
    # SCAN guarantees at least once, and a repeat would count one buffer twice against max_concurrent_buffers.
    class _Repeating(_FakeScanRedis):
        def scan_iter(self, match=None, count=None):
            keys = list(super().scan_iter(match=match, count=count))
            return iter(keys + keys[:1] + [k.decode() if isinstance(k, bytes) else k for k in keys])

    client = _Repeating({"timeshift_buffer:buffer:a": b"{}", b"timeshift_buffer:buffer:b": b"{}"})
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    assert timeshift_buffer_plugin._list_buffer_keys() == ["timeshift_buffer:buffer:a", "timeshift_buffer:buffer:b"]


def test_one_unreadable_state_does_not_stop_the_others_from_being_read(monkeypatch):
    prefix = timeshift_buffer_plugin._buffer_key("")
    values = {
        prefix + "good1": b'{"channel_uuid": "good1", "pid": 1}',
        prefix + "badjson": b"{not json",
        prefix + "notadict": b"[1, 2]",
        prefix + "nouuid": b'{"pid": 5}',
        prefix + "emptyuuid": b'{"channel_uuid": ""}',
        prefix + "gone": b"",
        prefix + "good2": b'{"channel_uuid": "good2"}',
    }
    client = _FakeScanRedis(values)
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin, "_unreadable_state_keys_logged", set())

    states = list(timeshift_buffer_plugin._iter_buffer_states())

    assert [s["channel_uuid"] for s in states] == ["good1", "good2"]


def test_an_unreadable_state_is_logged_once_not_on_every_tick(monkeypatch, caplog):
    prefix = timeshift_buffer_plugin._buffer_key("")
    client = _FakeScanRedis({prefix + "bad": b"{not json"})
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin, "_unreadable_state_keys_logged", set())
    with caplog.at_level("WARNING"):
        for _ in range(3):
            assert list(timeshift_buffer_plugin._iter_buffer_states()) == []
    assert sum("unreadable buffer state" in r.getMessage() for r in caplog.records) == 1


def test_the_reaper_trims_the_log_under_the_buffers_own_storage_path(monkeypatch):
    client = _FakeReaperRedisClient()
    monkeypatch.setattr(timeshift_buffer_plugin, "_redis", lambda: client)
    monkeypatch.setattr(timeshift_buffer_plugin.time, "time", lambda: 1000.0)
    states = [
        {"channel_uuid": _UUID, "last_heartbeat": 1000.0, "storage_path": "/old/place"},
        {"channel_uuid": _ORPHAN_UUID, "last_heartbeat": 1000.0},  # a state from before the field existed
    ]
    monkeypatch.setattr(timeshift_buffer_plugin, "_iter_buffer_states", lambda: states)
    monkeypatch.setattr(timeshift_buffer_plugin, "_prune_stale_viewers", lambda *a, **k: False)
    monkeypatch.setattr(timeshift_buffer_plugin, "_scrub_orphaned_dirs", lambda *a, **k: [])
    trimmed = []
    monkeypatch.setattr(
        timeshift_buffer_plugin, "_cap_ffmpeg_log", lambda channel_dir: trimmed.append(str(channel_dir))
    )

    settings_getter = lambda: {"idle_timeout_seconds": 30, "storage_path": "/new/place"}  # noqa: E731
    timeshift_buffer_plugin._reaper_loop(settings_getter, _FakeLogger(), _OneShotStopEvent())

    assert trimmed == [f"/old/place/{_UUID}", f"/new/place/{_ORPHAN_UUID}"]
