"""Unit tests for the pure/isolable helpers in tools/kodi_smoke_test.py.

Loaded by explicit file path (not a bare `import kodi_smoke_test`), the
same pattern this project's own plugin tests and
tools/tests/test_check_doc_refs.py already use. kodi_smoke_test.py's own
module docstring already establishes it's deliberately NOT part of CI as
an end-to-end script (it needs a live Kodi/Dispatcharr instance) -- this
suite covers only the pure selection/lookup/arithmetic logic extracted
out of it, the same "new pure-logic code gets a test alongside it"
convention CLAUDE.md documents for the addon's own C++/plugin code,
following a 26th-pass audit that found several real bugs in this exact
logic with no test of any kind guarding against a recurrence
(docs/OPEN_ITEMS.md).

Covers, one section per real bug fixed alongside the extraction:
  - _match_recording_by_title / _find_in_progress_recording_id's own
    state=="recording" matching (replacing an endtime > now heuristic
    that also matched an already-finished, early-stopped recording).
  - _windows_overlap / _pick_conflict_free_broadcast (replacing a
    fallback that could pick a broadcast confirmed live to hang Kodi or
    fail with -32100).
  - _select_new_timers (the recurring-rule-plus-auto-spawned-occurrence
    matching _add_and_verify_timer relies on for cleanup).
  - _compute_forward_seek_target (replacing a seek-target formula that
    could go negative near a recording's own end).
  - _find_matching_recording (check_realtime_update_push's own
    POST-failure orphan-recovery lookup).
  - SmokeTestRun.record()'s broad exception catch (an unanticipated
    exception from one check must not abort every check after it).
  - DispatcharrApiClient's ConnectionError/JSONDecodeError handling
    (matching JsonRpcClient's own, already-tested-live equivalent).
"""

import importlib.util
import json
import sys
from pathlib import Path

import pytest

_MODULE_PATH = Path(__file__).parent.parent / "kodi_smoke_test.py"
_spec = importlib.util.spec_from_file_location("kodi_smoke_test", _MODULE_PATH)
kodi_smoke_test = importlib.util.module_from_spec(_spec)
# dataclass's own field-type resolution (SmokeTestRun/CheckResult below)
# looks itself up via sys.modules[cls.__module__] -- a module built via
# module_from_spec is never registered there on its own, so this needs
# doing explicitly before exec_module, unlike check_doc_refs.py's own
# otherwise-identical load-by-path pattern, which has no dataclass to hit
# this.
sys.modules[_spec.name] = kodi_smoke_test
_spec.loader.exec_module(kodi_smoke_test)


# ---------------------------------------------------------------------
# _match_recording_by_title
# ---------------------------------------------------------------------


def test_match_recording_by_title_finds_a_recording_whose_title_is_in_the_set():
    recordings = [{"recordingid": 1, "title": "Show A"}, {"recordingid": 2, "title": "Show B"}]
    match = kodi_smoke_test._match_recording_by_title(recordings, {"Show B"})
    assert match["recordingid"] == 2


def test_match_recording_by_title_returns_none_when_nothing_matches():
    recordings = [{"recordingid": 1, "title": "Show A"}]
    assert kodi_smoke_test._match_recording_by_title(recordings, {"Show B"}) is None


def test_match_recording_by_title_ignores_a_recording_missing_a_title():
    recordings = [{"recordingid": 1}, {"recordingid": 2, "title": "Show B"}]
    match = kodi_smoke_test._match_recording_by_title(recordings, {"Show B"})
    assert match["recordingid"] == 2


# ---------------------------------------------------------------------
# _windows_overlap / _pick_conflict_free_broadcast
# ---------------------------------------------------------------------


def test_windows_overlap_true_for_a_genuine_overlap():
    existing = [("2026-01-01 10:00:00", "2026-01-01 11:00:00")]
    assert kodi_smoke_test._windows_overlap("2026-01-01 10:30:00", "2026-01-01 11:30:00", existing)


def test_windows_overlap_false_for_adjacent_non_overlapping_windows():
    existing = [("2026-01-01 10:00:00", "2026-01-01 11:00:00")]
    assert not kodi_smoke_test._windows_overlap("2026-01-01 11:00:00", "2026-01-01 12:00:00", existing)


def test_windows_overlap_false_with_no_existing_windows_at_all():
    assert not kodi_smoke_test._windows_overlap("2026-01-01 10:00:00", "2026-01-01 11:00:00", [])


def test_pick_conflict_free_broadcast_returns_the_first_non_overlapping_candidate():
    existing = [("2026-01-01 10:00:00", "2026-01-01 11:00:00")]
    future = [
        {"broadcastid": 1, "starttime": "2026-01-01 10:30:00", "endtime": "2026-01-01 11:30:00"},
        {"broadcastid": 2, "starttime": "2026-01-01 12:00:00", "endtime": "2026-01-01 13:00:00"},
    ]
    picked = kodi_smoke_test._pick_conflict_free_broadcast(future, existing)
    assert picked["broadcastid"] == 2


def test_pick_conflict_free_broadcast_returns_none_rather_than_a_conflicting_fallback():
    # Regression test: the original version fell back to future[0] here,
    # a broadcast this project confirmed live can hang Kodi with a
    # blocking dialog or fail outright with -32100 -- silently picking
    # one anyway defeated the point of checking for conflicts at all.
    existing = [("2026-01-01 10:00:00", "2026-01-01 11:00:00")]
    future = [{"broadcastid": 1, "starttime": "2026-01-01 10:30:00", "endtime": "2026-01-01 11:30:00"}]
    assert kodi_smoke_test._pick_conflict_free_broadcast(future, existing) is None


def test_pick_conflict_free_broadcast_with_no_candidates_at_all():
    assert kodi_smoke_test._pick_conflict_free_broadcast([], []) is None


# ---------------------------------------------------------------------
# _select_new_timers
# ---------------------------------------------------------------------


def test_select_new_timers_one_off_matches_only_the_new_timer_with_the_right_broadcastid():
    timers = [
        {"timerid": 1, "broadcastid": 100},  # pre-existing
        {"timerid": 2, "broadcastid": 200},  # new, wrong broadcast
        {"timerid": 3, "broadcastid": 100},  # new, right broadcast
    ]
    result = kodi_smoke_test._select_new_timers(
        timers, existing_timerids={1}, timerrule=False, broadcast_id=100, channel_id=5
    )
    assert [t["timerid"] for t in result] == [3]


def test_select_new_timers_recurring_rule_with_no_new_rule_entry_returns_empty():
    timers = [{"timerid": 1, "istimerrule": False}]
    result = kodi_smoke_test._select_new_timers(
        timers, existing_timerids=set(), timerrule=True, broadcast_id=100, channel_id=5
    )
    assert result == []


def test_select_new_timers_recurring_rule_includes_its_auto_spawned_occurrence():
    # Regression test: an earlier version of this logic missed the
    # auto-spawned occurrence entirely, leaving it orphaned server-side
    # after the rule itself was cleaned up (confirmed live, 2026-09-16).
    timers = [
        {"timerid": 1, "istimerrule": True, "title": "Show A", "channelid": 5},
        {"timerid": 2, "istimerrule": False, "title": "Show A", "channelid": 5},  # the auto-spawned occurrence
        {"timerid": 3, "istimerrule": False, "title": "Unrelated Show", "channelid": 5},
        {"timerid": 4, "istimerrule": False, "title": "Show A", "channelid": 6},  # right title, wrong channel
    ]
    result = kodi_smoke_test._select_new_timers(
        timers, existing_timerids=set(), timerrule=True, broadcast_id=100, channel_id=5
    )
    assert [t["timerid"] for t in result] == [1, 2]


def test_select_new_timers_ignores_timers_already_present_before():
    timers = [
        {"timerid": 1, "broadcastid": 100},
        {"timerid": 2, "broadcastid": 100},
    ]
    result = kodi_smoke_test._select_new_timers(
        timers, existing_timerids={1, 2}, timerrule=False, broadcast_id=100, channel_id=5
    )
    assert result == []


# ---------------------------------------------------------------------
# _compute_forward_seek_target
# ---------------------------------------------------------------------


def test_compute_forward_seek_target_caps_at_the_configured_forward_amount():
    target = kodi_smoke_test._compute_forward_seek_target(total_seconds=10_000, before=0)
    assert target == kodi_smoke_test.RECORDING_SEEK_FORWARD_SECONDS


def test_compute_forward_seek_target_respects_the_end_margin():
    forward = kodi_smoke_test.RECORDING_SEEK_FORWARD_SECONDS
    margin = kodi_smoke_test.RECORDING_SEEK_END_MARGIN_SECONDS
    total = forward + margin  # exactly forward+margin left after "before"
    target = kodi_smoke_test._compute_forward_seek_target(total_seconds=total, before=0)
    assert target == forward


def test_compute_forward_seek_target_can_go_non_positive_near_the_end():
    # Regression test: playback already resumed close enough to the end
    # that there's no room left to seek forward within the end margin --
    # the caller must skip rather than silently seek backward/nowhere
    # while still claiming to test a forward seek.
    target = kodi_smoke_test._compute_forward_seek_target(total_seconds=100, before=95)
    assert target <= 0


# ---------------------------------------------------------------------
# _find_matching_recording
# ---------------------------------------------------------------------


def test_find_matching_recording_matches_on_all_three_fields():
    candidates = [
        {"id": 1, "channel": 5, "start_time": "2026-01-01T10:00:00Z", "end_time": "2026-01-01T11:00:00Z"},
        {"id": 2, "channel": 5, "start_time": "2026-01-01T12:00:00Z", "end_time": "2026-01-01T13:00:00Z"},
    ]
    match = kodi_smoke_test._find_matching_recording(candidates, 5, "2026-01-01T12:00:00Z", "2026-01-01T13:00:00Z")
    assert match["id"] == 2


def test_find_matching_recording_returns_none_when_no_candidate_matches():
    candidates = [{"id": 1, "channel": 5, "start_time": "2026-01-01T10:00:00Z", "end_time": "2026-01-01T11:00:00Z"}]
    assert (
        kodi_smoke_test._find_matching_recording(candidates, 5, "2026-01-01T12:00:00Z", "2026-01-01T13:00:00Z") is None
    )


def test_find_matching_recording_requires_the_right_channel_too():
    candidates = [{"id": 1, "channel": 6, "start_time": "2026-01-01T10:00:00Z", "end_time": "2026-01-01T11:00:00Z"}]
    assert (
        kodi_smoke_test._find_matching_recording(candidates, 5, "2026-01-01T10:00:00Z", "2026-01-01T11:00:00Z") is None
    )


# ---------------------------------------------------------------------
# SmokeTestRun.record()
# ---------------------------------------------------------------------


def test_record_reports_pass_for_a_successful_check():
    run = kodi_smoke_test.SmokeTestRun()
    run.record("ok", lambda: "all good")
    assert run.results[0].status == "pass"
    assert run.results[0].detail == "all good"


def test_record_reports_skip_for_a_skipcheck():
    run = kodi_smoke_test.SmokeTestRun()

    def _fn():
        raise kodi_smoke_test.SkipCheck("nothing to test")

    run.record("skipped", _fn)
    assert run.results[0].status == "skip"
    assert run.results[0].detail == "nothing to test"


@pytest.mark.parametrize(
    "exc",
    [
        kodi_smoke_test.JsonRpcError("boom"),
        kodi_smoke_test.DispatcharrApiError("boom"),
        AssertionError("boom"),
    ],
)
def test_record_reports_fail_for_the_already_expected_exception_types(exc):
    run = kodi_smoke_test.SmokeTestRun()

    def _fn():
        raise exc

    run.record("failed", _fn)
    assert run.results[0].status == "fail"


def test_record_reports_fail_rather_than_crashing_for_an_unanticipated_exception():
    # Regression test: a check's own KeyError/TypeError/OSError used to
    # kill the whole run with a raw traceback instead of a clean per-check
    # failure (docs/OPEN_ITEMS.md's 26th-pass audit entry) -- every check
    # after this one in a real run still deserves to execute.
    run = kodi_smoke_test.SmokeTestRun()

    def _fn():
        raise KeyError("missing")

    run.record("buggy_check", _fn)
    assert run.results[0].status == "fail"
    assert "KeyError" in run.results[0].detail


def test_record_still_lets_a_later_check_run_after_an_earlier_ones_unanticipated_exception():
    run = kodi_smoke_test.SmokeTestRun()
    run.record("buggy_check", lambda: (_ for _ in ()).throw(TypeError("bad")))
    run.record("later_check", lambda: "fine")
    assert [r.status for r in run.results] == ["fail", "pass"]


# ---------------------------------------------------------------------
# DispatcharrApiClient error handling
# ---------------------------------------------------------------------


class _FakeResponse:
    def __init__(self, body: bytes):
        self._body = body

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self._body


def test_authenticate_wraps_a_connection_error(monkeypatch):
    def _raise(*_args, **_kwargs):
        raise ConnectionResetError("reset by peer")

    monkeypatch.setattr(kodi_smoke_test.urllib.request, "urlopen", _raise)
    client = kodi_smoke_test.DispatcharrApiClient("example.invalid", 80, "user", "pass")
    with pytest.raises(kodi_smoke_test.DispatcharrApiError, match="connection reset"):
        client._authenticate()


def test_authenticate_wraps_a_json_decode_error(monkeypatch):
    monkeypatch.setattr(kodi_smoke_test.urllib.request, "urlopen", lambda *a, **k: _FakeResponse(b"not json"))
    client = kodi_smoke_test.DispatcharrApiClient("example.invalid", 80, "user", "pass")
    with pytest.raises(kodi_smoke_test.DispatcharrApiError, match="invalid JSON response"):
        client._authenticate()


def test_call_wraps_a_connection_error(monkeypatch):
    def _raise(*_args, **_kwargs):
        raise ConnectionResetError("reset by peer")

    monkeypatch.setattr(kodi_smoke_test.urllib.request, "urlopen", _raise)
    client = kodi_smoke_test.DispatcharrApiClient("example.invalid", 80, "user", "pass")
    client._token = "already-authenticated"
    with pytest.raises(kodi_smoke_test.DispatcharrApiError, match="connection reset"):
        client.call("GET", "/api/channels/recordings/")


def test_call_wraps_a_json_decode_error(monkeypatch):
    monkeypatch.setattr(kodi_smoke_test.urllib.request, "urlopen", lambda *a, **k: _FakeResponse(b"not json"))
    client = kodi_smoke_test.DispatcharrApiClient("example.invalid", 80, "user", "pass")
    client._token = "already-authenticated"
    with pytest.raises(kodi_smoke_test.DispatcharrApiError, match="invalid JSON response"):
        client.call("GET", "/api/channels/recordings/")


def test_call_returns_parsed_json_on_success(monkeypatch):
    monkeypatch.setattr(
        kodi_smoke_test.urllib.request, "urlopen", lambda *a, **k: _FakeResponse(json.dumps({"id": 1}).encode())
    )
    client = kodi_smoke_test.DispatcharrApiClient("example.invalid", 80, "user", "pass")
    client._token = "already-authenticated"
    assert client.call("GET", "/api/channels/recordings/1/") == {"id": 1}


# ---------------------------------------------------------------------
# _time_to_seconds
# ---------------------------------------------------------------------


def test_time_to_seconds_combines_kodis_player_time_fields():
    t = {"hours": 1, "minutes": 2, "seconds": 3, "milliseconds": 250}
    assert kodi_smoke_test._time_to_seconds(t) == 3600 + 120 + 3 + 0.25


def test_time_to_seconds_is_zero_for_a_stopped_player_time():
    assert kodi_smoke_test._time_to_seconds({"hours": 0, "minutes": 0, "seconds": 0, "milliseconds": 0}) == 0


class _ScriptedRpc:
    """A JsonRpcClient stand-in: `answers` maps a method to a value, an exception to raise, or a list of them
    consumed in order (the last one repeats)."""

    def __init__(self, answers):
        self.answers = answers
        self.calls = []

    def call(self, method, params=None, timeout=None):
        self.calls.append((method, params))
        answer = self.answers[method]
        if isinstance(answer, list):
            answer = answer.pop(0) if len(answer) > 1 else answer[0]
        if isinstance(answer, Exception):
            raise answer
        return answer


def test_has_modal_dialog_reads_the_info_boolean_and_never_sends_input():
    rpc = _ScriptedRpc({"XBMC.GetInfoBooleans": {"System.HasActiveModalDialog": True}})
    assert kodi_smoke_test._has_modal_dialog(rpc) is True
    rpc = _ScriptedRpc({"XBMC.GetInfoBooleans": {"System.HasActiveModalDialog": False}})
    assert kodi_smoke_test._has_modal_dialog(rpc) is False
    assert all(not method.startswith("Input.") for method, _ in rpc.calls)


def test_has_modal_dialog_is_false_when_kodi_cannot_answer():
    rpc = _ScriptedRpc({"XBMC.GetInfoBooleans": kodi_smoke_test.JsonRpcError("timed out")})
    assert kodi_smoke_test._has_modal_dialog(rpc) is False


def test_open_live_channel_succeeds_without_checking_for_a_prompt():
    rpc = _ScriptedRpc({"Player.Open": "OK"})
    kodi_smoke_test._open_live_channel(rpc, 7)
    assert [m for m, _ in rpc.calls] == ["Player.Open"]


def test_open_live_channel_skips_when_a_modal_prompt_is_why_it_timed_out():
    rpc = _ScriptedRpc(
        {
            "Player.Open": kodi_smoke_test.JsonRpcError("Player.Open: timed out waiting for a response"),
            "XBMC.GetInfoBooleans": {"System.HasActiveModalDialog": True},
            "GUI.GetProperties": {"currentwindow": {"label": "Yes / No dialog", "id": 10100}},
        }
    )
    with pytest.raises(kodi_smoke_test.SkipCheck) as info:
        kodi_smoke_test._open_live_channel(rpc, 7)
    message = str(info.value)
    assert "Yes / No dialog" in message and "channel 7" in message and "--channel-id" in message
    assert all(not method.startswith("Input.") for method, _ in rpc.calls)


def test_open_live_channel_still_fails_when_it_timed_out_with_no_prompt():
    rpc = _ScriptedRpc(
        {
            "Player.Open": kodi_smoke_test.JsonRpcError("Player.Open: timed out waiting for a response"),
            "XBMC.GetInfoBooleans": {"System.HasActiveModalDialog": False},
        }
    )
    with pytest.raises(kodi_smoke_test.JsonRpcError):
        kodi_smoke_test._open_live_channel(rpc, 7)


class _Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def test_wait_for_canseek_returns_as_soon_as_it_turns_true():
    clock = _Clock()
    rpc = _ScriptedRpc({"Player.GetProperties": [{"canseek": False}, {"canseek": False}, {"canseek": True}]})
    assert kodi_smoke_test._wait_for_canseek(rpc, 1, timeout=30, interval=1, sleep=clock.sleep, clock=clock) is True
    assert len(rpc.calls) == 3 and clock.now == 2


def test_wait_for_canseek_gives_up_after_the_timeout_and_polls_only_that_long():
    clock = _Clock()
    rpc = _ScriptedRpc({"Player.GetProperties": {"canseek": False}})
    assert kodi_smoke_test._wait_for_canseek(rpc, 1, timeout=10, interval=2, sleep=clock.sleep, clock=clock) is False
    assert clock.now == 10 and len(rpc.calls) == 6


def test_wait_for_canseek_asks_for_the_right_player():
    clock = _Clock()
    rpc = _ScriptedRpc({"Player.GetProperties": {"canseek": True}})
    kodi_smoke_test._wait_for_canseek(rpc, 3, sleep=clock.sleep, clock=clock)
    assert rpc.calls[0] == ("Player.GetProperties", {"playerid": 3, "properties": ["canseek"]})
