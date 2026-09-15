#!/usr/bin/env python3
"""Drives a running Kodi instance's JSON-RPC webserver to smoke-test
pvr.dispatcharr-unofficial end to end against a real Dispatcharr backend.

This is deliberately NOT part of CI and never will be (see
docs/OPEN_ITEMS.md's "No automated test suite exists" entry): it needs a
live Kodi instance actually talking to a live Dispatcharr server, the same
manual/live-hardware territory as every other real bug this project has
found. What it replaces is ad hoc one-off JSON-RPC calls typed by hand
during a pre-release smoke-test pass, with a repeatable script that covers
the addon's real PVR-API surface -- exactly the surface CLAUDE.md's
"Building and testing" section documents as deliberately outside the
Catch2/pytest suites, since mocking Kodi's addon ABI or Dispatcharr's own
REST API would test the mock, not the addon.

Every check here is read-only against Dispatcharr by default. A handful of
checks that necessarily mutate real state on the real backend (creating/
deleting a real timer, starting/stopping a real instant recording) are
gated behind --allow-mutations and print a loud warning first -- run
those only against a Dispatcharr instance you're comfortable creating and
immediately deleting a real timer/recording on.

Passing --dispatcharr-host/--dispatcharr-username/--dispatcharr-password
(alongside --allow-mutations) additionally enables a realtime-update-push
check: it talks directly to Dispatcharr's own REST API -- the one
deliberate exception to this script's usual Kodi-JSON-RPC-only boundary,
see DispatcharrApiClient -- to create a one-off recording completely
outside Kodi/the addon's own request/response cycle, then confirms via
PVR.GetTimers that the addon's WebSocket-driven push reflects it into
Kodi with no restart. Requires the addon's own enable_realtime_updates
setting to be turned on; see check_realtime_update_push()'s docstring.

Known Kodi-core landmines this script works around, both confirmed live
across multiple platforms (see docs/RECORDINGS.md, docs/TIMESHIFT.md, and
this project's own operator notes):
  - Chaining several JSON-RPC calls over one persistent connection has
    hung indefinitely in a prior live session even though the webserver
    itself stayed responsive to fresh connections; this script opens one
    plain HTTP request per call, never reusing a connection across calls.
  - Player.Open on a recording with a stale resume bookmark pops a modal
    "Resume from X / Play from beginning" dialog that silently stalls
    every further JSON-RPC call ({"resume": false} does NOT suppress this
    for a PVR recording item) -- see _dismiss_resume_dialog_if_stuck()
    below.

Usage:
    python3 tools/kodi_smoke_test.py --host 192.0.2.10 [--port 8080]
        [--username kodi] [--password ...] [--channel-id N]
        [--recording-id N] [--allow-mutations]
        [--dispatcharr-host 192.0.2.20 [--dispatcharr-port 80]
         --dispatcharr-username ... --dispatcharr-password ...]

Exit code is 0 only if every attempted check passed; a skipped check
(e.g. no recordings exist to test against) does not fail the run.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone

DEFAULT_TIMEOUT_SECONDS = 10
PLAYBACK_POLL_INTERVAL_SECONDS = 1
PLAYBACK_START_TIMEOUT_SECONDS = 15
RESUME_DIALOG_DISMISS_ATTEMPTS = 3
MUTATION_WARNING_DELAY_SECONDS = 5
PLAYBACK_PROGRESS_CHECK_SECONDS = 2
ADDTIMER_CLEANUP_RETRY_ATTEMPTS = 3
ADDTIMER_CLEANUP_RETRY_INTERVAL_SECONDS = 1
PLAYBACK_TRANSITION_SETTLE_SECONDS = 2
PLAYBACK_OPEN_SETTLE_SECONDS = 3
# Confirmed live (2026-09-15, real CoreELEC/ODROID N2+ hardware, real
# production Dispatcharr account): 5 was too low against a real account's
# actual content distribution -- of 140 real hasarchive=true channels,
# none of the first 5 (in PVR.GetChannels' own return order) had a
# finished broadcast at test time, but 7 of the first 30 did, and
# catch-up playback itself worked cleanly once a real one was found.
CATCHUP_CHANNEL_PROBE_LIMIT = 30
PLAYER_OPEN_TIMEOUT_SECONDS = 30
LIVE_TIMESHIFT_BUFFER_WARMUP_SECONDS = 5
LIVE_TIMESHIFT_SEEK_BACK_SECONDS = 20
LIVE_TIMESHIFT_SEEK_TIMEOUT_SECONDS = 30
REALTIME_UPDATE_TEST_LEAD_MINUTES = 2
REALTIME_UPDATE_TEST_DURATION_MINUTES = 3
REALTIME_UPDATE_POLL_TIMEOUT_SECONDS = 30
REALTIME_UPDATE_POLL_INTERVAL_SECONDS = 2
RECORDING_SEEK_FORWARD_SECONDS = 30
RECORDING_SEEK_TOLERANCE_SECONDS = 5
RECORDING_SEEK_TIMEOUT_SECONDS = 15
RECORDING_SEEK_END_MARGIN_SECONDS = 15
RECORDING_SEEK_MIN_DURATION_SECONDS = RECORDING_SEEK_FORWARD_SECONDS + RECORDING_SEEK_END_MARGIN_SECONDS + 15


class JsonRpcError(RuntimeError):
    pass


class JsonRpcClient:
    """One plain HTTP POST per call -- see the module docstring's note on
    why chained/reused connections aren't used here."""

    def __init__(self, host: str, port: int, username: str | None, password: str | None):
        self._url = f"http://{host}:{port}/jsonrpc"
        self._headers = {"Content-Type": "application/json"}
        if username:
            token = base64.b64encode(f"{username}:{password or ''}".encode()).decode()
            self._headers["Authorization"] = f"Basic {token}"
        self._next_id = 1

    def call(self, method: str, params: dict | None = None, timeout: float | None = None):
        request_id = self._next_id
        self._next_id += 1
        body = json.dumps({"jsonrpc": "2.0", "method": method, "params": params or {}, "id": request_id}).encode()
        req = urllib.request.Request(self._url, data=body, headers=self._headers, method="POST")
        try:
            with urllib.request.urlopen(
                req, timeout=timeout if timeout is not None else DEFAULT_TIMEOUT_SECONDS
            ) as resp:
                payload = json.loads(resp.read())
        except urllib.error.URLError as exc:
            raise JsonRpcError(f"{method}: connection failed ({exc})") from exc
        except TimeoutError as exc:
            # Confirmed live (2026-09-14): a *read* timeout (connection
            # established, response never arrives -- e.g. Kodi's PVR manager
            # blocked on an unanswered confirmation dialog) surfaces as a bare
            # TimeoutError, not wrapped in URLError like a connect-time
            # timeout is. Both need to fail the same way here.
            raise JsonRpcError(f"{method}: timed out waiting for a response") from exc
        except ConnectionError as exc:
            # Confirmed live (2026-09-15): Kodi's webserver dying mid-request
            # (e.g. the whole process/VM crashing) surfaces as a raw
            # ConnectionResetError, a plain OSError subclass urllib does NOT
            # wrap in URLError -- an uncaught crash here previously took down
            # the whole script with an unhandled traceback instead of a clean
            # [FAIL] line. ConnectionError covers this and its siblings
            # (BrokenPipeError, ConnectionAbortedError) the same way.
            raise JsonRpcError(f"{method}: connection reset ({exc})") from exc
        except json.JSONDecodeError as exc:
            # A dying webserver can also return a truncated/empty body
            # instead of severing the connection outright -- same "don't
            # crash the whole script" reasoning as the ConnectionError case
            # above.
            raise JsonRpcError(f"{method}: invalid JSON response ({exc})") from exc
        if "error" in payload:
            raise JsonRpcError(f"{method}: {payload['error']}")
        return payload.get("result")


class DispatcharrApiError(RuntimeError):
    pass


class DispatcharrApiClient:
    """Thin direct client for Dispatcharr's own REST API -- used only by
    check_realtime_update_push, the one deliberate exception to this
    script's usual Kodi-JSON-RPC-only boundary (see the module docstring).
    The whole point of that check is proving the addon's WebSocket-driven
    realtime-update push reflects a change that originates *outside* the
    addon's own request/response cycle -- there's no way to trigger that
    from inside Kodi at all, so this has to talk to Dispatcharr directly.

    Auth and the recordings endpoint shape are both confirmed live
    (2026-09-14) directly against DispatcharrClient.cpp's own Login()/
    CreateOneTimeRecording(): POST /api/accounts/token/ with username/
    password returns a SimpleJWT {"access": ...}; POST
    /api/channels/recordings/ takes {"channel", "start_time", "end_time"}
    (both times "%Y-%m-%dT%H:%M:%SZ", matching DateTimeFormat.cpp's own
    IsoFromTime())."""

    def __init__(self, host: str, port: int, username: str, password: str):
        self._base = f"http://{host}:{port}"
        self._username = username
        self._password = password
        self._token: str | None = None

    def _authenticate(self):
        body = json.dumps({"username": self._username, "password": self._password}).encode()
        req = urllib.request.Request(
            f"{self._base}/api/accounts/token/",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=DEFAULT_TIMEOUT_SECONDS) as resp:
                self._token = json.loads(resp.read())["access"]
        except (urllib.error.URLError, TimeoutError) as exc:
            raise DispatcharrApiError(f"Dispatcharr login failed: {exc}") from exc

    def call(self, method: str, path: str, body: dict | None = None):
        if self._token is None:
            self._authenticate()
        headers = {"Content-Type": "application/json", "Authorization": f"Bearer {self._token}"}
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"{self._base}{path}", data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=DEFAULT_TIMEOUT_SECONDS) as resp:
                raw = resp.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as exc:
            raise DispatcharrApiError(
                f"Dispatcharr {method} {path}: HTTP {exc.code} {exc.read().decode(errors='replace')}"
            ) from exc
        except (urllib.error.URLError, TimeoutError) as exc:
            raise DispatcharrApiError(f"Dispatcharr {method} {path}: {exc}") from exc


@dataclass
class CheckResult:
    name: str
    status: str  # "pass", "fail", "skip"
    detail: str = ""


@dataclass
class SmokeTestRun:
    results: list[CheckResult] = field(default_factory=list)

    def record(self, name: str, fn):
        try:
            detail = fn() or ""
            self.results.append(CheckResult(name, "pass", detail))
        except SkipCheck as exc:
            self.results.append(CheckResult(name, "skip", str(exc)))
        except (JsonRpcError, DispatcharrApiError, AssertionError) as exc:
            self.results.append(CheckResult(name, "fail", str(exc)))

    def print_summary(self) -> int:
        width = max(len(r.name) for r in self.results)
        for r in self.results:
            marker = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP"}[r.status]
            print(f"[{marker}] {r.name.ljust(width)}  {r.detail}")
        failed = [r for r in self.results if r.status == "fail"]
        skipped = [r for r in self.results if r.status == "skip"]
        print(
            f"\n{len(self.results) - len(failed) - len(skipped)} passed, "
            f"{len(failed)} failed, {len(skipped)} skipped"
        )
        return 1 if failed else 0


class SkipCheck(Exception):
    """Raised by a check that has nothing to test against (e.g. no
    recordings exist yet) -- not a failure of the addon."""


# ---------------------------------------------------------------------
# Playback helpers
# ---------------------------------------------------------------------


def _wait_for_active_player(rpc: JsonRpcClient, timeout: float = PLAYBACK_START_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        players = rpc.call("Player.GetActivePlayers")
        if players:
            return players[0]
        time.sleep(PLAYBACK_POLL_INTERVAL_SECONDS)
    return None


def _describe_current_window(rpc: JsonRpcClient) -> str:
    """Read-only diagnostic for a playback attempt that never produced an
    active player -- reports GUI.GetProperties' own currentwindow so a
    failure message can say what Kodi's focus actually looks like (e.g.
    the known resume-bookmark prompt, or something else entirely) without
    ever guessing at or acting on it."""
    try:
        window = rpc.call("GUI.GetProperties", {"properties": ["currentwindow"]})["currentwindow"]
        return f"{window.get('label', '?')} (id {window.get('id', '?')})"
    except JsonRpcError as exc:
        return f"<could not check: {exc}>"


def _dismiss_resume_dialog_if_stuck(rpc: JsonRpcClient):
    """Player.Open on a recording with a stale resume bookmark pops a
    modal dialog that silently stalls Player.GetActivePlayers (returns
    [] with no error) until dismissed -- {"resume": false} does not
    suppress it. Confirmed live on Windows/macOS/CoreELEC/Rocky Linux.

    Confirmed live (2026-09-15) this used to blindly send Input.Down +
    Input.Select here to auto-pick "Play from beginning" -- and confirmed
    live, the same day, that this is genuinely unsafe: Kodi's focus can
    be on a *completely different* dialog when a playback attempt stalls
    for some other reason, and those same blind keystrokes landed on
    Kodi's own power menu (DialogButtonMenu.xml) and selected "Power Off"
    -- shutting down the real Windows machine this was running on mid-test
    (root-caused via kodi.log's own "Window Init (DialogButtonMenu.xml)"
    immediately followed by "kodi.exe...has initiated the power off" in
    Windows' Event Viewer). A test script that can accidentally power off
    the machine it's testing against is not an acceptable risk on any
    platform -- this deliberately no longer sends ANY synthetic input.
    Just waits (same total budget as before) and returns None on
    timeout for the caller to report as a clean failure; see
    _describe_current_window() for a safe, read-only diagnostic instead."""
    return _wait_for_active_player(rpc, timeout=PLAYBACK_POLL_INTERVAL_SECONDS * 2 * RESUME_DIALOG_DISMISS_ATTEMPTS)


def _time_to_seconds(t: dict) -> float:
    return t["hours"] * 3600 + t["minutes"] * 60 + t["seconds"] + t["milliseconds"] / 1000


def _confirm_playback_progressing(rpc: JsonRpcClient, playerid: int):
    """An active Player.GetActivePlayers entry with nonzero speed is not
    sufficient proof anything is actually playing -- confirmed live
    (2026-09-14): Kodi reported exactly that right after its own log showed
    "OpenDemuxStream - Error creating demuxer", a real failed-open with no
    playback happening at all. Poll Player.GetProperties' "time" twice and
    require it to have actually advanced, which a stalled/failed-to-open
    player can't fake.

    Settles before the first reading rather than sampling immediately:
    confirmed live (2026-09-14) that a Range-seekable stream (a recording
    or catch-up broadcast, both HTTP Range-based per docs/CATCHUP.md) can
    report an implausible "time" value right as Player.GetActivePlayers
    first goes active, then a small, sane one moments later -- matching
    kodi.log showing an internal duration-probe seek ("cache completely
    reset for seek to position <huge byte offset>") as part of Kodi's own
    stream-open sequence, before real forward playback actually begins.
    Sampling too early catches that transient, not a real position; an
    earlier fix here (a settle delay only *between* different playback
    checks, reasoning this was live-channel EPG-relative-time bleed-over
    per docs/TIMESHIFT.md) turned out incomplete -- confirmed by this
    exact symptom recurring on check_catchup_playback with no live
    channel involved at all in that same run."""
    time.sleep(PLAYBACK_OPEN_SETTLE_SECONDS)
    before = _time_to_seconds(rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["time"]})["time"])
    time.sleep(PLAYBACK_PROGRESS_CHECK_SECONDS)
    after = _time_to_seconds(rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["time"]})["time"])
    assert after > before, (
        f"playback position didn't advance ({before}s -> {after}s over "
        f"{PLAYBACK_PROGRESS_CHECK_SECONDS}s) -- Player.GetActivePlayers reporting a player "
        "isn't proof anything is actually playing, e.g. a demuxer-open failure right after "
        "Player.Open can still leave a phantom active player"
    )


def _stop_all_active_players(rpc: JsonRpcClient):
    """Confirmed live (2026-09-14): Player.Open itself can time out
    client-side while still succeeding server-side (the same class of
    issue this project already found for PVR.AddTimer) -- a playback
    check's own Player.Open call sitting outside its try/finally meant a
    stream could be left genuinely open with no cleanup at all if that
    first call raised. Checking for and stopping ANY currently-active
    player here, rather than only one this check already knows the
    playerid of, closes that gap and avoids contaminating whatever
    playback check runs next (matches the EPG-relative-time bleed-over
    symptom documented on check_recorded_playback's own docstring)."""
    try:
        active = rpc.call("Player.GetActivePlayers")
    except JsonRpcError:
        return
    for p in active:
        with contextlib.suppress(JsonRpcError):
            rpc.call("Player.Stop", {"playerid": p["playerid"]})


# ---------------------------------------------------------------------
# Read-only checks
# ---------------------------------------------------------------------


def check_connectivity(rpc: JsonRpcClient):
    result = rpc.call("JSONRPC.Ping")
    assert result == "pong", f"expected 'pong', got {result!r}"
    return "webserver reachable"


def check_addon_enabled(rpc: JsonRpcClient, addon_id: str):
    details = rpc.call(
        "Addons.GetAddonDetails",
        {"addonid": addon_id, "properties": ["enabled", "version", "name"]},
    )["addon"]
    assert details["enabled"], f"{addon_id} is installed but disabled"
    return f"{details['name']} {details['version']}, enabled"


def check_channel_groups(rpc: JsonRpcClient):
    groups = rpc.call("PVR.GetChannelGroups", {"channeltype": "tv"})["channelgroups"]
    assert groups, "no TV channel groups returned"
    return f"{len(groups)} group(s)"


def check_channel_group_membership(rpc: JsonRpcClient):
    """Regression check for ChannelGroupFilter (see CLAUDE.md's "Building
    and testing" section): Dispatcharr's own /api/channels/groups/
    returns every group that has ever existed, including ones no longer
    enabled for any M3U account, and "enabled" isn't itself a property of
    the group to check directly -- GetChannelGroups() is supposed to drop
    a group with no member channels left in the just-fetched channel
    list before this addon ever hands it to Kodi. Asserts every group
    PVR.GetChannelGroups actually returns has at least one real member
    channel via PVR.GetChannelGroupDetails, which would fail if that
    filtering regressed.

    Confirmed live (2026-09-14) there's no "PVR.GetChannelGroupMembers"
    method (returns "Method not found." per JSONRPC.Introspect) --
    PVR.GetChannelGroupDetails' own "channels" sub-object is the real way
    to list a group's member channels. Also confirmed live that its
    "channels.limits" sub-param is NOT honored by Kodi-core for this
    method (a real Kodi quirk, not this addon's own doing) -- passing one
    made no difference to a large real group's response size, so this
    doesn't bother trying to limit the result, just checks it's
    non-empty."""
    groups = rpc.call("PVR.GetChannelGroups", {"channeltype": "tv"})["channelgroups"]
    assert groups, "no TV channel groups returned"
    empty = []
    for group in groups:
        details = rpc.call(
            "PVR.GetChannelGroupDetails",
            {"channelgroupid": group["channelgroupid"], "channels": {"properties": []}},
        )["channelgroupdetails"]
        if not details.get("channels"):
            empty.append(group.get("label", group["channelgroupid"]))
    assert not empty, (
        f"{len(empty)} channel group(s) with zero member channels were returned by "
        f"PVR.GetChannelGroups: {empty[:5]}{'...' if len(empty) > 5 else ''} -- "
        "regression of ChannelGroupFilter, which exists specifically to drop these"
    )
    return f"all {len(groups)} channel group(s) have at least one member channel"


def check_channels(rpc: JsonRpcClient) -> int:
    """Returns the first channel id found, for later checks to reuse."""
    channels = rpc.call(
        "PVR.GetChannels",
        {"channelgroupid": "alltv", "properties": ["channeltype", "channel"]},
    )["channels"]
    assert channels, "no channels returned for the 'alltv' group"
    return channels[0]["channelid"]


def check_epg(rpc: JsonRpcClient, channel_id: int):
    broadcasts = rpc.call(
        "PVR.GetBroadcasts",
        {"channelid": channel_id, "properties": ["title", "starttime", "endtime"]},
    )["broadcasts"]
    assert broadcasts, f"no EPG entries for channel {channel_id}"
    return f"{len(broadcasts)} broadcast(s)"


def check_timers(rpc: JsonRpcClient):
    timers = rpc.call("PVR.GetTimers", {"properties": ["title", "istimerrule", "state"]})["timers"]
    return f"{len(timers)} timer(s)/rule(s) (0 is fine if none are scheduled)"


def check_recordings(rpc: JsonRpcClient) -> int | None:
    """Returns the first recording id found, or None if there aren't any
    (not a failure -- a fresh install may have no recordings yet)."""
    recordings = rpc.call("PVR.GetRecordings", {"properties": ["title", "runtime"]})["recordings"]
    if not recordings:
        raise SkipCheck("no recordings exist to check against")
    return recordings[0]["recordingid"]


def check_live_playback(rpc: JsonRpcClient, channel_id: int):
    """Confirmed live (2026-09-14): Player.Open on a channel that
    currently has one of its own timers actively recording pops a real,
    legitimate Kodi DialogConfirm.xml ("Play recording" / "Switch to
    channel") -- the same class of blocking-dialog issue already found
    for PVR.AddTimer, just triggered by ordinary live playback this time.
    Checking the channel's own isrecording flag first and skipping
    cleanly avoids colliding with a real, currently-in-progress recording
    rather than gambling on which button is focused."""
    details = rpc.call("PVR.GetChannelDetails", {"channelid": channel_id, "properties": ["isrecording"]})[
        "channeldetails"
    ]
    if details.get("isrecording"):
        raise SkipCheck(f"channel {channel_id} has an active recording in progress right now -- retry later")

    try:
        rpc.call("Player.Open", {"item": {"channelid": channel_id}}, timeout=PLAYER_OPEN_TIMEOUT_SECONDS)
        player = _wait_for_active_player(rpc)
        assert player is not None, "playback never started (Player.GetActivePlayers stayed empty)"
        props = rpc.call("Player.GetProperties", {"playerid": player["playerid"], "properties": ["speed", "canseek"]})
        assert props["speed"] != 0, "player reports speed 0 (paused/stalled) right after opening"
        _confirm_playback_progressing(rpc, player["playerid"])
        return f"playing, canseek={props['canseek']}"
    finally:
        _stop_all_active_players(rpc)


def check_live_timeshift_seek(rpc: JsonRpcClient, channel_id: int):
    """Server-side live timeshift (live_timeshift_mode: "Server-side
    (Dispatcharr plugin)", see docs/TIMESHIFT.md) is this addon's own
    rolling live-TV pause/rewind buffer, layered on the companion
    timeshift_buffer plugin -- unrelated to catch-up (see
    check_catchup_playback's own docstring: catch-up is per-programme
    and archive-provider-driven, this is a continuous rolling buffer this
    addon builds itself). check_live_playback only ever confirms forward
    playback; this is the only check that actually seeks a live stream,
    a real, distinct, historically bug-prone surface (audio-sync and
    packet-corruption incidents documented at length in docs/TIMESHIFT.md).

    Uses a relative seconds seek deliberately, matching this project's
    own confirmed-live testing convention (docs/TIMESHIFT.md: an absolute
    Player.Seek {"time": ...} computed from Player.GetProperties' own
    "time" silently targets the wrong domain, since that reading is
    EPG-relative, not buffer-relative, for a live channel -- relative
    Player.Seek {"seconds": N} is unaffected and is what all of this
    project's own real seek testing already uses).

    Skips (same isrecording guard as check_live_playback) rather than
    colliding with an active recording, and also skips outright if
    canseek comes back false -- docs/TIMESHIFT.md documents a real case
    of exactly that (server-side buffer not yet ready) where Player.Seek
    then fails outright with -32100, not just a UI-level no-op.

    Deliberately does NOT compare Player.GetProperties' "time" before vs.
    after the seek to confirm it moved -- confirmed live (2026-09-14)
    that "time" for a live channel is EPG-relative (wall-clock time since
    the current programme started), not buffer-relative, exactly as
    docs/TIMESHIFT.md's own "Practical takeaway" section already warns:
    it kept climbing by real elapsed seconds across a real seek that
    otherwise succeeded with no error, never reflecting the seek at all.
    The two signals this project's own docs establish as actually
    reliable are used instead: the seek call itself not erroring (a
    genuinely broken/not-ready buffer fails outright with -32100, not a
    silent no-op), and playback continuing to progress normally
    afterward (catches a post-seek freeze/corruption, the exact shape of
    this project's own real incidents from that class of bug)."""
    details = rpc.call("PVR.GetChannelDetails", {"channelid": channel_id, "properties": ["isrecording"]})[
        "channeldetails"
    ]
    if details.get("isrecording"):
        raise SkipCheck(f"channel {channel_id} has an active recording in progress right now -- retry later")

    try:
        rpc.call("Player.Open", {"item": {"channelid": channel_id}}, timeout=PLAYER_OPEN_TIMEOUT_SECONDS)
        player = _wait_for_active_player(rpc)
        assert player is not None, "playback never started (Player.GetActivePlayers stayed empty)"
        playerid = player["playerid"]
        # Let the rolling buffer actually accumulate some real history
        # before attempting to seek backward into it at all.
        time.sleep(LIVE_TIMESHIFT_BUFFER_WARMUP_SECONDS)
        props = rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["canseek"]})
        if not props["canseek"]:
            raise SkipCheck("live stream reports canseek=false -- server-side timeshift isn't ready/available")

        rpc.call(
            "Player.Seek",
            {"playerid": playerid, "value": {"seconds": -LIVE_TIMESHIFT_SEEK_BACK_SECONDS}},
            timeout=LIVE_TIMESHIFT_SEEK_TIMEOUT_SECONDS,
        )
        # Confirm the seek didn't leave playback frozen/corrupted -- the
        # exact shape of this project's own real post-seek freeze incidents.
        _confirm_playback_progressing(rpc, playerid)
        return f"seek call succeeded (backward {LIVE_TIMESHIFT_SEEK_BACK_SECONDS}s), playback continued afterward"
    finally:
        _stop_all_active_players(rpc)


def _play_recording_and_verify(rpc: JsonRpcClient, recording_id: int) -> str:
    """Shared open+verify+cleanup core for check_recorded_playback and
    check_in_progress_recording_playback -- see either's own docstring
    for what distinguishes the two and why both are worth testing
    separately rather than just one or the other.

    Note on a failure shaped like "position went from a large,
    implausible number back to 0" (as opposed to "stuck at 0"): confirmed
    live (2026-09-14) this can happen when this runs immediately after
    check_live_playback with no settle gap -- docs/TIMESHIFT.md already
    documents Player.GetProperties' "time" as EPG-relative (not
    buffer-relative) for a *live* channel specifically, and a "before"
    reading matching neither 0 nor anything sane for the recording is
    consistent with a brief bleed-over from the just-closed live player's
    own EPG-relative clock rather than a real playback failure here.
    main() adds a short settle delay before check_recorded_playback when
    it follows check_live_playback for exactly this reason.

    A resume bookmark near the recording's own end (from this project's
    own separately-confirmed finding that {"resume": false} does not
    actually suppress resume for a PVR recording item -- a real Kodi-core
    quirk, not fixable from this client) is a second, real possible cause
    of a similar-looking failure if it recurs despite the settle delay;
    pass --recording-id explicitly with a fresh/short recording to rule
    that out."""
    try:
        rpc.call("Player.Open", {"item": {"recordingid": recording_id}}, timeout=PLAYER_OPEN_TIMEOUT_SECONDS)
        player = _dismiss_resume_dialog_if_stuck(rpc)
        assert player is not None, (
            "playback never started -- if this is a resume-bookmark stall, see this "
            "function's own docstring; a fresh recording with no prior playback "
            f"avoids it entirely. Kodi's current window: {_describe_current_window(rpc)} "
            "-- this script no longer sends synthetic input to guess a dismissal (see "
            "_dismiss_resume_dialog_if_stuck's own docstring for why), so check/dismiss "
            "manually if this is a real stuck dialog"
        )
        props = rpc.call("Player.GetProperties", {"playerid": player["playerid"], "properties": ["speed", "canseek"]})
        assert props["speed"] != 0, "player reports speed 0 (paused/stalled) right after opening"
        _confirm_playback_progressing(rpc, player["playerid"])
        return f"playing, canseek={props['canseek']}"
    finally:
        _stop_all_active_players(rpc)


def check_recorded_playback(rpc: JsonRpcClient, recording_id: int):
    return _play_recording_and_verify(rpc, recording_id)


def _find_in_progress_recording_id(rpc: JsonRpcClient) -> int:
    """Kodi's own JSON-RPC schema has no direct "is this in progress" flag
    on a recording (confirmed via JSONRPC.Introspect's
    PVR.Details.Recording) -- inferring it from endtime > now is the same
    signal Kodi's own PVR core effectively relies on. Raises SkipCheck
    when nothing is currently recording, since that's real, time-dependent
    environment state, not a bug."""
    recordings = rpc.call("PVR.GetRecordings", {"properties": ["endtime"]})["recordings"]
    now = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
    in_progress = [r for r in recordings if r["endtime"] > now]
    if not in_progress:
        raise SkipCheck("no recording is currently in progress -- nothing to test against")
    return in_progress[0]["recordingid"]


def check_in_progress_recording_playback(rpc: JsonRpcClient):
    """A recording the addon is still actively writing (its own endtime
    still in the future) exercises a genuinely different code path than
    check_recorded_playback -- the addon's own isInProgress/HLS-manifest-
    merging logic (see CLAUDE.md's "Building and testing" section on
    RecordingParser) has real documented incidents specific to this
    state, and absent an explicit --recording-id, check_recorded_playback
    tends to land on whatever recording happens to hold the lowest id,
    which is not reliably an in-progress one."""
    return _play_recording_and_verify(rpc, _find_in_progress_recording_id(rpc))


def _seek_within_recording_and_verify(rpc: JsonRpcClient, recording_id: int) -> str:
    """Shared open+seek+verify+cleanup core for check_recorded_playback_seek
    and check_in_progress_recording_seek -- the recording-playback
    counterpart to check_live_timeshift_seek, covering a genuinely
    different code path (SegmentLookup/M3u8SegmentParser's byte-position
    lookup, see CLAUDE.md's "Building and testing" section) than either
    _play_recording_and_verify (start+progress only, never seeks) or
    check_live_timeshift_seek (a live channel, not a recording).

    Unlike a live channel, a recording's Player.GetProperties "time" is
    genuine buffer/file-relative position, not EPG-relative (docs/
    TIMESHIFT.md's EPG-relative-time finding is specific to a *live*
    channel) -- so this check can assert the seek actually landed near
    its target, a strictly stronger signal than check_live_timeshift_seek
    can rely on for a live channel.

    Seeks forward, not backward: an in-progress recording (the other
    caller) has only ever had its past written so far, so a backward seek
    would be far less interesting than confirming a forward seek lands
    correctly and respects the addon's own LiveEdgeMargin backoff (see
    CLAUDE.md) rather than overrunning what's actually been buffered.
    Skips outright on a recording too short to safely seek this far
    into, and on canseek=false, matching check_live_timeshift_seek's own
    skip conventions."""
    try:
        rpc.call("Player.Open", {"item": {"recordingid": recording_id}}, timeout=PLAYER_OPEN_TIMEOUT_SECONDS)
        player = _dismiss_resume_dialog_if_stuck(rpc)
        assert player is not None, (
            "playback never started -- if this is a resume-bookmark stall, see "
            "_play_recording_and_verify's own docstring; a fresh recording with no "
            "prior playback avoids it entirely"
        )
        playerid = player["playerid"]
        props = rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["canseek", "totaltime"]})
        if not props["canseek"]:
            raise SkipCheck("recording reports canseek=false")
        total_seconds = _time_to_seconds(props["totaltime"])
        if total_seconds < RECORDING_SEEK_MIN_DURATION_SECONDS:
            raise SkipCheck(
                f"recording is only {total_seconds:.0f}s long -- too short to safely seek "
                f"{RECORDING_SEEK_FORWARD_SECONDS}s forward into without risking landing past its end"
            )

        # Avoid the internal duration-probe-seek transient right after
        # Player.Open on a Range-seekable stream (see
        # _confirm_playback_progressing's own docstring) before reading a
        # "before" position to seek relative to.
        time.sleep(PLAYBACK_OPEN_SETTLE_SECONDS)
        before = _time_to_seconds(
            rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["time"]})["time"]
        )
        target = min(RECORDING_SEEK_FORWARD_SECONDS, total_seconds - RECORDING_SEEK_END_MARGIN_SECONDS - before)
        rpc.call(
            "Player.Seek",
            {"playerid": playerid, "value": {"seconds": target}},
            timeout=RECORDING_SEEK_TIMEOUT_SECONDS,
        )
        after = _time_to_seconds(
            rpc.call("Player.GetProperties", {"playerid": playerid, "properties": ["time"]})["time"]
        )
        expected = before + target
        assert abs(after - expected) <= RECORDING_SEEK_TOLERANCE_SECONDS, (
            f"seek landed at {after:.0f}s, expected roughly {expected:.0f}s "
            f"(started at {before:.0f}s, sought forward {target:.0f}s, "
            f"tolerance {RECORDING_SEEK_TOLERANCE_SECONDS}s)"
        )
        _confirm_playback_progressing(rpc, playerid)
        return f"seek from {before:.0f}s landed at {after:.0f}s (target {expected:.0f}s), playback continued afterward"
    finally:
        _stop_all_active_players(rpc)


def check_recorded_playback_seek(rpc: JsonRpcClient, recording_id: int):
    return _seek_within_recording_and_verify(rpc, recording_id)


def check_in_progress_recording_seek(rpc: JsonRpcClient):
    return _seek_within_recording_and_verify(rpc, _find_in_progress_recording_id(rpc))


def check_catchup_playback(rpc: JsonRpcClient):
    """Catch-up ("play from EPG") playback -- confirmed live 2026-09-14
    via Player.Open's real broadcastid item variant, routing through the
    addon's GetEPGTagStreamProperties()/IsEPGTagPlayable() path rather
    than OpenLiveStream() or a recording (see docs/CATCHUP.md). Confirmed
    via kodi.log: opens pvr://guide/<id>/<time>.epg, a real Dispatcharr
    catchup session URL, and ffmpeg successfully demuxing it.

    Requires a channel with hasarchive=true (Kodi's own flag for "this
    channel's upstream IPTV provider offers catch-up/archive" -- entirely
    provider-driven, not something every Dispatcharr instance or channel
    has) and a broadcast that has genuinely *ended* (not merely started)
    and is still within that channel's retention window. Skips cleanly
    rather than failing when neither exists, since that's a real
    environment/content capability gap, not a bug -- not gated behind
    --allow-mutations either, since a catchup session is an ephemeral
    stream, not a persistent Dispatcharr entity like a timer.

    Confirmed live (2026-09-14): preferring the most recently *started*
    broadcast (rather than the most recently *ended* one) can land on a
    still-airing programme -- PVR.GetBroadcastIsPlayable returns true for
    those too (docs/CATCHUP.md: "past (or currently-airing)"), but its
    archive file may not be complete yet, and really did fail with a real
    "OpenDemuxStream - Error creating demuxer" here, unlike a genuinely
    finished programme tested the same way. Filtering on endtime rather
    than starttime avoids this."""
    channels = rpc.call("PVR.GetChannels", {"channelgroupid": "alltv", "properties": ["hasarchive"]})["channels"]
    archive_channel_ids = [c["channelid"] for c in channels if c.get("hasarchive")]
    if not archive_channel_ids:
        raise SkipCheck("no channel reports hasarchive=true -- nothing to catch-up-test")

    now = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
    broadcast_id = None
    channel_id = None
    for candidate_channel_id in archive_channel_ids[:CATCHUP_CHANNEL_PROBE_LIMIT]:
        broadcasts = rpc.call(
            "PVR.GetBroadcasts", {"channelid": candidate_channel_id, "properties": ["starttime", "endtime"]}
        )["broadcasts"]
        finished = sorted((b for b in broadcasts if b["endtime"] < now), key=lambda b: b["endtime"])
        if not finished:
            continue
        if rpc.call("PVR.GetBroadcastIsPlayable", {"broadcastid": finished[-1]["broadcastid"]}):
            broadcast_id, channel_id = finished[-1]["broadcastid"], candidate_channel_id
            break
    if broadcast_id is None:
        raise SkipCheck(
            f"no finished, playable broadcast found on the first {CATCHUP_CHANNEL_PROBE_LIMIT} "
            "hasarchive=true channel(s) tried"
        )

    try:
        rpc.call("Player.Open", {"item": {"broadcastid": broadcast_id}}, timeout=PLAYER_OPEN_TIMEOUT_SECONDS)
        player = _wait_for_active_player(rpc)
        assert player is not None, "catch-up playback never started (Player.GetActivePlayers stayed empty)"
        props = rpc.call("Player.GetProperties", {"playerid": player["playerid"], "properties": ["speed", "canseek"]})
        assert props["speed"] != 0, "player reports speed 0 (paused/stalled) right after opening"
        _confirm_playback_progressing(rpc, player["playerid"])
        return f"playing (channel {channel_id}, broadcast {broadcast_id}), canseek={props['canseek']}"
    finally:
        _stop_all_active_players(rpc)


# ---------------------------------------------------------------------
# Mutating checks -- opt-in only, see --allow-mutations
# ---------------------------------------------------------------------


def check_realtime_update_push(rpc: JsonRpcClient, dispatcharr: DispatcharrApiClient, channel_id: int):
    """Confirmed live (2026-09-14): the addon's WebSocket-driven realtime-
    update push genuinely updates Kodi's own PVR.GetTimers *without any
    addon/Kodi restart* -- creating a one-off recording directly against
    Dispatcharr's own REST API (the same POST /api/channels/recordings/
    call CreateOneTimeRecording() itself makes) and polling PVR.GetTimers
    showed the new entry appear within a few seconds, and kodi.log showed
    "realtime update received: recording_updated" immediately followed by
    a cache refresh. Deleting it the same way removed it from
    PVR.GetTimers live too, confirming both directions.

    Requires enable_realtime_updates to actually be turned on in the
    addon's own settings -- confirmed live that with it off there's no
    WebSocket log activity at all (it simply never connects, no error
    either). There's no way to check this via Kodi's JSON-RPC first (no
    generic addon-settings read/write path exists at all), so a timeout
    here is ambiguous between "the setting is off" and "the push is
    genuinely broken" -- the failure message says so rather than
    guessing.

    Deliberately talks to Dispatcharr's own REST API directly rather than
    through Kodi -- proving the push works when a change originates
    *outside* the addon's own request/response cycle is the whole point,
    and there's no way to trigger that from inside Kodi at all (the one
    deliberate exception to this script's usual Kodi-JSON-RPC-only
    boundary, see the module docstring and DispatcharrApiClient's own).

    Kodi's JSON-RPC "channelid" is Kodi's own internal PVR database row
    id, NOT the addon's own uniqueid (confirmed live: they were 729 vs.
    69110 for the same real channel) -- PVR.GetChannelDetails' "uniqueid"
    property is what actually matches Dispatcharr's own real channel id,
    needed here since we're talking to Dispatcharr directly."""
    details = rpc.call("PVR.GetChannelDetails", {"channelid": channel_id, "properties": ["uniqueid"]})["channeldetails"]
    dispatcharr_channel_id = details["uniqueid"]

    before_ids = {t["timerid"] for t in rpc.call("PVR.GetTimers", {"properties": ["title"]})["timers"]}

    start = datetime.now(timezone.utc) + timedelta(minutes=REALTIME_UPDATE_TEST_LEAD_MINUTES)
    end = start + timedelta(minutes=REALTIME_UPDATE_TEST_DURATION_MINUTES)
    fmt = "%Y-%m-%dT%H:%M:%SZ"
    recording = dispatcharr.call(
        "POST",
        "/api/channels/recordings/",
        {"channel": dispatcharr_channel_id, "start_time": start.strftime(fmt), "end_time": end.strftime(fmt)},
    )
    recording_id = recording["id"]

    try:
        deadline = time.monotonic() + REALTIME_UPDATE_POLL_TIMEOUT_SECONDS
        new_timer = None
        while time.monotonic() < deadline:
            timers = rpc.call("PVR.GetTimers", {"properties": ["title", "channelid"]})["timers"]
            new_timer = next(
                (t for t in timers if t["timerid"] not in before_ids and t.get("channelid") == channel_id), None
            )
            if new_timer is not None:
                break
            time.sleep(REALTIME_UPDATE_POLL_INTERVAL_SECONDS)
        assert new_timer is not None, (
            f"a recording created directly via Dispatcharr's API didn't appear in PVR.GetTimers within "
            f"{REALTIME_UPDATE_POLL_TIMEOUT_SECONDS}s -- either enable_realtime_updates isn't turned on/"
            "connected, or the realtime push itself didn't propagate; check kodi.log for "
            '"realtime update" activity either way'
        )
        return f"realtime push confirmed: timer {new_timer['timerid']} ({new_timer['title']}) appeared with no restart"
    finally:
        dispatcharr.call("DELETE", f"/api/channels/recordings/{recording_id}/")


def _add_and_verify_timer(rpc: JsonRpcClient, channel_id: int, timerrule: bool) -> int:
    """Shared create+verify+cleanup core for check_add_delete_timer and
    check_add_delete_recurring_rule -- PVR.AddTimer's real timerrule
    boolean (confirmed via JSONRPC.Introspect) toggles between the two;
    docs/RECURRING_RULES.md's own "only supports the record from EPG
    broadcast flow" note is about not being able to customize the
    resulting rule's own days/times pattern, not about this flag not
    existing. Returns the created timer/rule's own timerid.

    Confirmed live (2026-09-14): PVR.AddTimer against a broadcast that
    already has an overlapping timer (e.g. one generated by an existing
    recurring rule) can pop a blocking DialogConfirm.xml on the Kodi
    device that stalls the JSON-RPC response well past any reasonable
    timeout -- with no clean way to auto-dismiss it remotely, unlike the
    resume-bookmark dialog (see _dismiss_resume_dialog_if_stuck's own
    comment). Picking a broadcast with no pre-existing timer on this
    channel avoids the most common trigger, though not necessarily every
    possible one.

    A second, initially-confusing real trigger found the same day: with
    timerrule=False specifically, this hung identically on a broadcast
    that had simply already finished airing (PVR.GetBroadcasts' own
    first/oldest entry for the channel, which the original version of
    this function picked without checking) -- scheduling a one-off
    recording for something already over is nonsensical, so Kodi hangs
    the same way rather than erroring cleanly. timerrule=True didn't hit
    this, since it creates a rule matching the show's *future* showings
    regardless of which instance seeded it. Requiring a genuinely future
    broadcast (starttime > now) avoids this for both cases.

    A third real trigger, confirmed live (2026-09-15) on Windows against
    a real, actively-scheduled account: picking a broadcast whose time
    *window* overlaps an existing timer's on the same channel -- even by
    just a couple minutes at the boundary, not a full duplicate -- fails
    too, but differently: an immediate PVR.AddTimer error response
    (-32100 "Failed to execute method."), not the blocking-dialog hang
    documented above. Confirmed by direct JSON-RPC reproduction: the
    exact same broadcastid failed every time picked, and a neighboring
    broadcast one hour later (no overlap) succeeded immediately. The
    original version of this function's own conflict-avoidance only
    checked for an existing timer's starttime matching a broadcast's own
    starttime *exactly* -- too narrow a check against a real account
    with its own already-scheduled recordings potentially running right
    up against whatever broadcast gets picked. Checking for genuine
    time-range overlap against every existing timer on the channel (not
    just an exact-starttime match) avoids both known immediate-failure
    shapes, not just the exact-duplicate one.

    If AddTimer times out, a short bounded retry checks whether Kodi
    created the timer/rule anyway shortly after -- but this is a
    best-effort catch for a narrowly-slow response, not a guarantee: the
    one dialog actually observed live stayed open for ~43 seconds before
    auto-resolving, far longer than this check waits. A timeout here
    always needs a manual PVR.GetTimers/device check afterward, not just
    trust in this retry."""
    existing = rpc.call("PVR.GetTimers", {"properties": ["channelid", "starttime", "endtime"]})["timers"]
    existing_windows = [
        (t["starttime"], t["endtime"]) for t in existing if t.get("channelid") == channel_id and t.get("endtime")
    ]
    existing_timerids = {t["timerid"] for t in existing}

    def overlaps_existing(candidate_start: str, candidate_end: str) -> bool:
        return any(candidate_start < end and start < candidate_end for start, end in existing_windows)

    now = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
    broadcasts = rpc.call(
        "PVR.GetBroadcasts", {"channelid": channel_id, "properties": ["title", "starttime", "endtime"]}
    )["broadcasts"]
    future = sorted((b for b in broadcasts if b["starttime"] > now), key=lambda b: b["starttime"])
    if not future:
        raise SkipCheck(f"no future EPG entries on channel {channel_id} to create a test timer from")
    broadcast = next((b for b in future if not overlaps_existing(b["starttime"], b["endtime"])), future[0])
    broadcast_id = broadcast["broadcastid"]

    add_error = None
    try:
        rpc.call("PVR.AddTimer", {"broadcastid": broadcast_id, "timerrule": timerrule})
    except JsonRpcError as exc:
        add_error = exc

    def find_new(timers):
        if timerrule:
            # A rule's own timer entry doesn't reliably carry the
            # originating broadcastid the way a one-off timer does
            # (PVR.Details.Timer's broadcastid defaults to -1) -- match
            # on "a new istimerrule=true entry not present before" instead.
            return [t for t in timers if t.get("istimerrule") and t["timerid"] not in existing_timerids]
        return [t for t in timers if t.get("broadcastid") == broadcast_id]

    attempts = ADDTIMER_CLEANUP_RETRY_ATTEMPTS if add_error else 1
    matching = []
    for attempt in range(attempts):
        timers = rpc.call("PVR.GetTimers", {"properties": ["title", "broadcastid", "istimerrule", "starttime"]})[
            "timers"
        ]
        matching = find_new(timers)
        if matching or attempt == attempts - 1:
            break
        time.sleep(ADDTIMER_CLEANUP_RETRY_INTERVAL_SECONDS)

    # Regression check for SeriesRuleMatching (see CLAUDE.md's "Building
    # and testing" section): a series rule has no fixed time of its own,
    # so without matching it to its earliest upcoming/in-progress
    # recording, Kodi displayed it as the Unix epoch ("12/31/1969") -- a
    # real confirmed-live display bug, not cosmetic. Checked before
    # cleanup deletes the rule below.
    epoch_like = [t for t in matching if timerrule and t.get("starttime", "").startswith("1970")]

    for t in matching:
        rpc.call("PVR.DeleteTimer", {"timerid": t["timerid"]})

    assert not epoch_like, (
        "the new recurring rule was reported with an epoch-like starttime "
        f"({epoch_like[0].get('starttime')!r}) -- regression of SeriesRuleMatching, "
        "which exists specifically to give a rule with no fixed time of its own a "
        "sane earliest-match starttime instead"
    )

    kind = "timer rule" if timerrule else "timer"
    if add_error is not None:
        cleanup_note = (
            f"cleaned up {len(matching)} {kind}(s) created despite the error"
            if matching
            else f"no {kind} appeared within this check's short retry window -- that does NOT "
            "rule out one appearing later if a dialog is still unresolved; verify manually"
        )
        raise AssertionError(
            f"PVR.AddTimer failed or timed out ({add_error}) -- if Kodi is showing an "
            f"unanswered confirmation dialog (e.g. a scheduling-conflict warning), it may take "
            f"far longer to resolve than this check waits for; {cleanup_note}"
        )
    assert matching, f"PVR.AddTimer succeeded but the new {kind} isn't in PVR.GetTimers"
    return matching[0]["timerid"]


def check_add_delete_timer(rpc: JsonRpcClient, channel_id: int):
    timerid = _add_and_verify_timer(rpc, channel_id, timerrule=False)
    return f"created and found timer id {timerid}"


def check_add_delete_recurring_rule(rpc: JsonRpcClient, channel_id: int):
    timerid = _add_and_verify_timer(rpc, channel_id, timerrule=True)
    return f"created and found timer rule id {timerid}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--addon-id", default="pvr.dispatcharr-unofficial")
    parser.add_argument("--channel-id", type=int, help="skip auto-discovery, use this channel for playback checks")
    parser.add_argument("--recording-id", type=int, help="skip auto-discovery, use this recording for playback checks")
    parser.add_argument(
        "--allow-mutations",
        action="store_true",
        help="also run checks that create/delete a real timer on the real backend",
    )
    parser.add_argument("--skip-playback", action="store_true", help="skip Player.Open checks entirely")
    parser.add_argument(
        "--dispatcharr-host",
        help="Dispatcharr server host/IP -- enables the realtime-update-push check "
        "(also requires --allow-mutations and --dispatcharr-username/--dispatcharr-password)",
    )
    parser.add_argument("--dispatcharr-port", type=int, default=80)
    parser.add_argument("--dispatcharr-username")
    parser.add_argument("--dispatcharr-password")
    args = parser.parse_args()

    if args.allow_mutations:
        mutation_warning = (
            "WARNING: --allow-mutations will create and delete a real one-off timer AND a "
            "real recurring rule on your real Dispatcharr backend"
        )
        if args.dispatcharr_host:
            mutation_warning += ", AND a real one-off recording (created directly via Dispatcharr's own API)"
        print(
            f"{mutation_warning}. Continuing in " f"{MUTATION_WARNING_DELAY_SECONDS} seconds (Ctrl-C to abort)...",
            file=sys.stderr,
        )
        time.sleep(MUTATION_WARNING_DELAY_SECONDS)

    rpc = JsonRpcClient(args.host, args.port, args.username, args.password)
    run = SmokeTestRun()

    run.record("connectivity", lambda: check_connectivity(rpc))
    run.record("addon_enabled", lambda: check_addon_enabled(rpc, args.addon_id))
    run.record("channel_groups", lambda: check_channel_groups(rpc))
    run.record("channel_group_membership", lambda: check_channel_group_membership(rpc))

    channel_id = args.channel_id
    if channel_id is None:
        holder = {}

        def _discover_channel():
            holder["id"] = check_channels(rpc)
            return f"channel id {holder['id']}"

        run.record("channels", _discover_channel)
        channel_id = holder.get("id")
    else:
        run.record("channels", lambda: f"using supplied channel id {channel_id}")

    if channel_id is not None:
        run.record("epg", lambda: check_epg(rpc, channel_id))
    run.record("timers", lambda: check_timers(rpc))

    recording_id = args.recording_id
    if recording_id is None:
        holder = {}

        def _discover_recording():
            holder["id"] = check_recordings(rpc)
            return f"recording id {holder['id']}"

        run.record("recordings", _discover_recording)
        recording_id = holder.get("id")
    else:
        run.record("recordings", lambda: f"using supplied recording id {recording_id}")

    if not args.skip_playback:
        # Confirmed live (2026-09-14): running one playback check
        # immediately after another can catch a transitional reading --
        # docs/TIMESHIFT.md already documents Player.GetProperties' "time"
        # as EPG-relative (not buffer-relative) for a live channel
        # specifically, and a "before" position matching neither 0 nor
        # anything sane for the next item is consistent with a brief
        # bleed-over from the just-closed player rather than a real
        # playback failure. Settle between every playback check, not just
        # some of them, for the same reason.
        played_something = False

        def _settle_if_needed():
            nonlocal played_something
            if played_something:
                time.sleep(PLAYBACK_TRANSITION_SETTLE_SECONDS)
            played_something = True

        if channel_id is not None:
            _settle_if_needed()
            run.record("live_playback", lambda: check_live_playback(rpc, channel_id))
            _settle_if_needed()
            run.record("live_timeshift_seek", lambda: check_live_timeshift_seek(rpc, channel_id))
        if recording_id is not None:
            _settle_if_needed()
            run.record("recorded_playback", lambda: check_recorded_playback(rpc, recording_id))
            _settle_if_needed()
            run.record("recorded_playback_seek", lambda: check_recorded_playback_seek(rpc, recording_id))
        _settle_if_needed()
        run.record("in_progress_recording_playback", lambda: check_in_progress_recording_playback(rpc))
        _settle_if_needed()
        run.record("in_progress_recording_seek", lambda: check_in_progress_recording_seek(rpc))
        _settle_if_needed()
        run.record("catchup_playback", lambda: check_catchup_playback(rpc))

    if args.allow_mutations and channel_id is not None:
        run.record("add_delete_timer", lambda: check_add_delete_timer(rpc, channel_id))
        run.record("add_delete_recurring_rule", lambda: check_add_delete_recurring_rule(rpc, channel_id))

        if args.dispatcharr_host and args.dispatcharr_username and args.dispatcharr_password:
            dispatcharr = DispatcharrApiClient(
                args.dispatcharr_host, args.dispatcharr_port, args.dispatcharr_username, args.dispatcharr_password
            )
            run.record("realtime_update_push", lambda: check_realtime_update_push(rpc, dispatcharr, channel_id))
        else:
            print(
                "\nNOTE: WebSocket realtime-update push (a recording/timer change reflected "
                "in Kodi without an addon restart) wasn't exercised -- pass --dispatcharr-host/"
                "--dispatcharr-username/--dispatcharr-password to enable it.\n",
                file=sys.stderr,
            )

    return run.print_summary()


if __name__ == "__main__":
    sys.exit(main())
