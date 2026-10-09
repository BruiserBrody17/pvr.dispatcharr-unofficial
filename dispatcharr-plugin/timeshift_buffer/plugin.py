"""
Timeshift Buffer -- a Dispatcharr plugin.

Records a rolling, per-channel HLS-style buffer to disk so a client (this
plugin was designed alongside pvr.dispatcharr-unofficial, a Kodi PVR addon) can
pause/rewind live TV without needing a local, on-device buffer the way
inputstream.ffmpegdirect's own timeshift mode provides today.

Design notes (see docs/API_NOTES.md in pvr.dispatcharr-unofficial and the
conversation that produced this draft for the full reasoning):

- Reads from Dispatcharr's own live proxy (/proxy/ts/stream/<uuid>) rather
  than re-fetching from the upstream provider directly. Confirmed via
  apps/proxy/live_proxy's source that multiple viewers of one channel
  already share a single upstream connection, so this plugin acting as
  "one more viewer" doesn't cost an extra connection against whatever
  concurrent-stream limit the upstream provider enforces, as long as
  someone (a real viewer, or this buffer itself) is already/also watching.
- Confirmed stream_ts (the view behind that URL) is @permission_classes
  ([AllowAny]), gated only by network_access_allowed(request, "STREAMS")
  -- unrestricted by default. A loopback request from inside this plugin's
  own process needs no API key/token. If you've deliberately narrowed the
  STREAMS network-access setting to exclude localhost, add it back or this
  plugin can't reach the proxy.
- ffmpeg's own hls muxer does almost all the hard work: -hls_flags
  delete_segments (with -hls_delete_threshold) removes old segment files
  instead of growing forever, and -hls_list_size maintains a
  sliding-window HLS playlist natively. No Python-side trimming loop is
  needed for the common case -- only an idle-timeout reaper (below), since
  nothing else would ever stop a buffer once started.
- Segment files must NOT live under Django's MEDIA_ROOT directly (that
  resolves to <app dir>/media, which -- confirmed against the project's
  own docker-compose.yml -- isn't under the one volume (./data:/data) the
  container actually bind-mounts, so it wouldn't survive a container
  recreate and wouldn't benefit from redirecting it to real storage the
  way this project's recordings path already can be). Files live under the
  configurable storage_path (default /data/timeshift) instead.
- Originally tried serving those files by symlinking MEDIA_ROOT/timeshift
  -> storage_path, relying on Django's existing static(MEDIA_URL, ...)
  route. Confirmed live against a real instance that this doesn't work:
  dispatcharr/urls.py's catch-all SPA route
  (path("<path:unused_path>", TemplateView...)) is concatenated BEFORE the
  appended static() patterns, and Django tries patterns in order, so the
  catch-all wins for every /media/... request and returns the React app
  shell instead of the file -- MEDIA_ROOT is effectively unreachable
  directly in this deployment mode, a routing quirk in Dispatcharr itself,
  not something this plugin can fix from the outside. Since plugins can't
  register their own URL routes either (confirmed via apps/plugins/loader.py
  -- no route-registration hook exists), this plugin instead runs its own
  minimal HTTP server (see BufferHTTPServer below), bound to its own port
  (http_port setting) directly on files under storage_path. That port needs
  to be exposed through your container config, the same way 9191 already
  is -- this is the one real infrastructure requirement beyond installing
  the plugin. Every request needs a per-buffer access token (see
  _check_access_token), issued only via the authenticated start_buffer
  action -- reachability alone doesn't grant access, since this server has
  no other auth of its own (Dispatcharr's own session/API-key auth doesn't
  apply to it; nothing here proxies through Dispatcharr's normal web port).
- Idle-timeout liveness comes from the HTTP server itself, not from a
  client explicitly calling the heartbeat action: every successful file
  fetch (playlist or segment) refreshes last_heartbeat. This matters
  because a Kodi PVR addon using plain STREAMURL passthrough for live
  channels (no OpenLiveStream/CloseLiveStream) gets no callback at all for
  "the user stopped watching" -- but inputstream.ffmpegdirect re-fetches a
  live .m3u8 on an interval for as long as playback continues and simply
  stops once it doesn't, so the request stream to this server already *is*
  the liveness signal, with nothing extra required from whatever's playing
  the stream.

Verified live end-to-end against a real Dispatcharr instance and a real
pvr.dispatcharr-unofficial build: buffer capture, this plugin's own HTTP serving,
and a real channel opening and playing cleanly are all confirmed working.

This plugin originally routed live playback through
`inputstream.ffmpegdirect` (a plain `STREAMURL`), with a `snapshot_buffer`
action (copy the buffer's currently-listed segments into a separate,
finite, `ENDLIST`-terminated playlist a client could seek within, since
Kodi gates `canseek` on a known duration a perpetually-growing live
playlist can never have) as a workaround for that route's own seeking,
which turned out to be broken outright, not just imprecise. That whole
approach is **superseded**: pvr.dispatcharr-unofficial now exposes the buffer via
Kodi's own `OpenLiveStream`/`ReadLiveStream`/`SeekLiveStream` API instead,
using Kodi's native internal demuxer directly -- real seeking within the
growing live buffer itself, no snapshot needed at all. That needed two
things from this plugin: Range support in the file server (`do_GET`, see
below -- individual segment files are Range-read directly, no HLS
playlist involved for this path), and the `get_live_manifest` action
(also below), which exposes the buffer's currently-known segments with a
stable, HLS-media-sequence-derived `sequence` number so a client can merge
repeated fetches into one consistent, growing byte-address space as the
rolling window advances. Confirmed live: real pause/rewind/fast-forward/
live-follow from plain Play, including a 95-second rewind spanning
several manifest refreshes. `snapshot_buffer` and the ffmpegdirect route
it existed for have both been removed entirely as a result -- see
pvr.dispatcharr-unofficial's own docs/TIMESHIFT.md for the full investigation this
summary compresses, including the exact `av_seek_frame` failure signature
that motivated moving off ffmpegdirect in the first place.
"""

import contextlib
import io
import ipaddress
import json
import logging
import mimetypes
import os
import re
import secrets
import shutil
import signal
import socket
import subprocess
import threading
import time
import uuid
from datetime import timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

# ---------------------------------------------------------------------------
# Redis-backed state. A plain Python module-level dict would NOT be shared
# across uWSGI worker processes -- the worker that handles start_buffer may
# not be the one that later handles stop_buffer or heartbeat. Redis is the
# same shared-state mechanism apps/proxy/live_proxy itself uses for exactly
# this kind of cross-worker coordination (see RedisKeys/ChannelService in
# that app for the established pattern this mirrors).
# ---------------------------------------------------------------------------

_REDIS_PREFIX = "timeshift_buffer:"
_REDIS_LEADER_KEY = _REDIS_PREFIX + "reaper_leader"
_REDIS_LEADER_TTL = 30  # seconds; the reaper thread renews this while alive
_REDIS_START_BUFFER_LOCK_PREFIX = _REDIS_PREFIX + "start_lock:"
# Generous relative to how long _start_buffer()'s own locked section ever
# actually takes (a handful of Redis round-trips plus one ffmpeg spawn) --
# a safety net for a caller that dies while holding the lock (a worker
# process killed mid-request), not a normal-case wait, so erring long
# here costs nothing in practice but erring short would reopen the exact
# race this lock exists to close if a legitimately-slow _start_ffmpeg()
# call ever outlived a too-short TTL.
_START_BUFFER_LOCK_TTL = 30
# Reserved lock name (not a channel uuid) for the section of start_buffer that counts the running buffers and
# registers the new one, so max_concurrent_buffers holds across channels.
_START_SLOT_LOCK_ID = "__start_slot__"
# Generous headroom past any reasonable idle_timeout_seconds -- a self-healing
# backstop in case the reaper thread itself dies or Redis outlives a container
# restart while the ffmpeg processes it was tracking don't: worst case, a
# buffer state entry (and whatever it points at) disappears on its own
# instead of lingering forever pointing at a dead PID.
_BUFFER_STATE_TTL = 600

_HTTP_THREAD_NAME = "timeshift_buffer_http"
_REAPER_THREAD_NAME = "timeshift_buffer_reaper"

_reaper_thread = None
_reaper_stop_event = None
# Held across _ensure_reaper_running()'s check-then-start, see its comment.
_reaper_lock = threading.Lock()
# Updated on every run() call (see _ensure_reaper_running()'s own comment
# for the real bug this fixes), read fresh on every reaper tick via the
# lambda that closes over this name -- a plain module-level rebind is
# what actually lets a settings change reach the reaper thread after its
# first start, unlike a closure capturing a specific run() call's own
# settings_dict object.
_latest_settings_dict = {}


def _redis():
    # core.utils.RedisClient, the same pattern core code uses inside
    # apps/timeshift/api_views.py (update_catchup_session_position ->
    # _trigger_timeshift_stats_update). Confirmed safe for plugin use by
    # extensive live use this session -- buffer state and reaper
    # leader-election both rely on it working correctly across worker
    # processes, and did throughout testing.
    from core.utils import RedisClient

    return RedisClient.get_client()


def _buffer_key(channel_uuid):
    return f"{_REDIS_PREFIX}buffer:{channel_uuid}"


class _BufferState(dict):
    """A buffer's state dict that remembers the exact JSON text it was read
    from (`raw`), so _cas_buffer_state() can compare against what Redis
    really holds byte for byte instead of against a re-serialization of the
    parsed copy."""

    raw = None


def _get_buffer_state(channel_uuid):
    raw = _redis().get(_buffer_key(channel_uuid))
    if not raw:
        return None
    state = _BufferState(json.loads(raw))
    state.raw = raw
    return state


def _set_buffer_state(channel_uuid, state):
    _redis().set(_buffer_key(channel_uuid), json.dumps(state), ex=_BUFFER_STATE_TTL)


# Sets a key only if it still holds exactly the text the caller read -- the one
# atomic step an optimistic read-modify-write needs.
_CAS_SCRIPT = """
local current = redis.call('GET', KEYS[1])
if current == ARGV[1] then
  redis.call('SET', KEYS[1], ARGV[2], 'EX', tonumber(ARGV[3]))
  return 1
end
return 0
"""

#: How many times _update_buffer_state() re-reads and retries when another
#: writer got in between its read and its write.
_STATE_UPDATE_ATTEMPTS = 10

_cas_fallback_logged = False


def _cas_buffer_state(channel_uuid, expected, new_state) -> bool:
    """Writes `new_state` only if the stored state is still exactly `expected`
    (the JSON text it was read as). Returns whether it was written.

    Falls back to a plain, unguarded write when this Redis refuses scripts
    (a ResponseError) -- the behavior every writer had before this existed, so
    an unusual Redis setup loses the protection but not the plugin. Any other
    error (a dropped connection) propagates to the caller, as it always did."""
    global _cas_fallback_logged
    try:
        return bool(
            _redis().eval(_CAS_SCRIPT, 1, _buffer_key(channel_uuid), expected, json.dumps(new_state), _BUFFER_STATE_TTL)
        )
    except Exception as exc:
        if not _is_redis_response_error(exc):
            raise
        if not _cas_fallback_logged:
            _cas_fallback_logged = True
            logging.getLogger(__name__).warning(
                "timeshift_buffer: this Redis refused the compare-and-set script (%s) -- buffer state updates are "
                "unguarded read-modify-write again",
                exc,
            )
        _set_buffer_state(channel_uuid, new_state)
        return True


def _update_buffer_state(channel_uuid, mutate):
    """Atomic read-modify-write of one buffer's state, for every writer that
    used to do a plain GET then SET.

    `mutate(state)` receives a freshly read copy, changes it in place and
    returns it -- or returns None to decline writing (and `state` is
    discarded). If another writer changes the stored state between the read
    and the write, the write is refused and the whole thing is retried against
    the new state, so `mutate` may run more than once and must be safe to
    (it should only touch the dict it is given).

    Returns (outcome, state): ("written", the state that was stored),
    ("absent", None) when there is no state for this channel -- never
    resurrected by a late write -- ("declined", None) when `mutate` returned
    None, or ("contended", None) after _STATE_UPDATE_ATTEMPTS lost races.

    Closes three consequences of the unguarded version (docs/OPEN_ITEMS.md): a
    heartbeat overwriting another request's viewer registration, a heartbeat
    overwriting the "stopping" marker so a start_buffer could reattach to a
    buffer mid-teardown, and a heartbeat read just before a teardown being
    written back afterwards, resurrecting a deleted state."""
    for _ in range(_STATE_UPDATE_ATTEMPTS):
        state = _get_buffer_state(channel_uuid)
        if state is None:
            return "absent", None
        expected = getattr(state, "raw", None)
        if expected is None:
            expected = json.dumps(state)
        updated = mutate(state)
        if updated is None:
            return "declined", None
        if _cas_buffer_state(channel_uuid, expected, updated):
            return "written", updated
    return "contended", None


def _delete_buffer_state(channel_uuid):
    _redis().delete(_buffer_key(channel_uuid))
    # Single choke point for every "this buffer is gone" path (stop_buffer,
    # stop_all, the reaper, dead-buffer cleanup in start_buffer) -- drops
    # this worker's own cached manifest state (see _get_live_manifest's
    # _manifest_cache) too, so it doesn't outlive the buffer it was for.
    # A future start_buffer for the same channel_uuid always begins with a
    # fresh ffmpeg process and a fresh live.m3u8, whose mtime/size will
    # essentially never coincidentally match a stale cache entry's, but
    # dropping it here is cheap and removes any doubt.
    with _manifest_cache_lock:
        _manifest_cache.pop(channel_uuid, None)


def _list_buffer_keys():
    # SCAN, not KEYS: KEYS walks the whole keyspace in one blocking command, on a Redis that also carries Dispatcharr's
    # proxy and Celery traffic, and this runs on every reaper tick (15 s) and under the start-slot lock of every start.
    keys = [k.decode() if isinstance(k, bytes) else k for k in _redis().scan_iter(match=_buffer_key("*"), count=500)]
    # SCAN promises every key that stays in the keyspace at least once, not exactly once (a keyspace resized between
    # calls repeats some, and this one is shared with Celery and the proxy): a repeat would be counted twice against
    # max_concurrent_buffers and listed twice.
    return list(dict.fromkeys(keys))


_unreadable_state_keys_logged = set()


def _log_unreadable_state_once(key):
    """One warning per key (the reaper ticks every 15 s and the value lives up to 10 minutes)."""
    if key in _unreadable_state_keys_logged or len(_unreadable_state_keys_logged) > 256:
        return
    _unreadable_state_keys_logged.add(key)
    logging.getLogger(__name__).warning("timeshift_buffer: ignoring the unreadable buffer state at %s", key)


def _iter_buffer_states():
    """Yields the parsed state dict for every currently-tracked buffer,
    skipping any key whose value is already gone by the time it's read
    (a real, if narrow, race between _list_buffer_keys() listing it and
    this read -- TTL expiry or a concurrent delete)."""
    client = _redis()
    for key in _list_buffer_keys():
        raw = client.get(key)
        if not raw:
            continue
        # One value that is not valid JSON, or has no channel, must not stop every reaper tick, the orphan scrub and
        # list/stop-all (callers index state["channel_uuid"]); it clears itself when its TTL expires.
        try:
            state = json.loads(raw)
        except ValueError:
            state = None
        if not isinstance(state, dict) or not state.get("channel_uuid"):
            _log_unreadable_state_once(key)
            continue
        yield state


def _start_buffer_lock_key(channel_uuid: str) -> str:
    return _REDIS_START_BUFFER_LOCK_PREFIX + channel_uuid


def _acquire_start_buffer_lock(channel_uuid: str) -> str | None:
    """Per-channel lock around _start_buffer()'s own classify-then-spawn
    sequence (SET NX EX -- the same primitive _reaper_loop's own leader
    election below already uses, just per-channel and short-lived
    instead of singleton and continuously renewed). Fixes a real,
    live-confirmed race (see docs/OPEN_ITEMS.md's own entry, and
    _start_buffer()'s own comment on where this is actually used): two
    near-simultaneous start_buffer calls for the same channel could each
    read no existing buffer, each independently spawn their own ffmpeg
    process, and leave one of them permanently orphaned -- invisible to
    the reaper, stop_buffer, stop_all, AND the orphan-directory scrub
    (confirmed live: both processes write into the same channel-uuid-
    named directory, so it still reads as "tracked" once either one's
    own state write lands).

    Returns a unique token identifying this specific acquisition (pass
    it to _release_start_buffer_lock() -- never a bare "release", so a
    caller can't accidentally release a lock a slower caller of its own
    already lost to TTL expiry and a third caller has since acquired),
    or None when another caller already holds it. Callers should treat
    None the same way _classify_existing_buffer()'s own "stopping" case
    already is -- fail fast with a retryable error rather than block,
    since blocking here risks a request timing out instead of a quick,
    clean "try again shortly", and the lock's own TTL already guarantees
    the wait is always short-lived regardless of which caller loses."""
    token = f"{os.getpid()}:{threading.get_ident()}:{time.time()}"
    if _redis().set(_start_buffer_lock_key(channel_uuid), token, nx=True, ex=_START_BUFFER_LOCK_TTL):
        return token
    return None


# Deletes the lock only while it still holds the caller's own token.
def _is_redis_response_error(exc: BaseException) -> bool:
    """Whether `exc` is redis-py's ResponseError or a subclass of it. The reply a Redis with an ACL that
    withholds scripting gives is "NOPERM", which redis-py raises as NoPermissionError, a SUBCLASS of
    ResponseError: matching the class name exactly re-raised it instead of falling back to the unscripted
    path, so a release escaped the `finally` of _start_buffer and every heartbeat writer failed (found by
    the 2026-10-04 eighth hardening sweep; redis-py is not importable here, so it is matched by name)."""
    return any(cls.__name__ == "ResponseError" for cls in type(exc).__mro__)


_RELEASE_LOCK_SCRIPT = """
if redis.call('GET', KEYS[1]) == ARGV[1] then
  return redis.call('DEL', KEYS[1])
end
return 0
"""


def _release_start_buffer_lock(channel_uuid: str, token: str) -> None:
    """Only deletes the lock if it still holds the exact token this
    caller was given by _acquire_start_buffer_lock() -- guards against
    releasing a lock a *different* caller has since legitimately
    acquired (this caller's own held it past its TTL, e.g. a slow
    ffmpeg spawn or a delayed request), which would let two callers'
    own locked sections overlap after all, defeating the point."""
    client = _redis()
    key = _start_buffer_lock_key(channel_uuid)
    # Atomic where this Redis allows scripts: the GET-then-DEL below left a window in which the lock
    # could expire and be taken by the next caller between the two, and the DEL then removed THEIR lock
    # (found by the 2026-10-04 seventh hardening sweep; it needs the hold to reach the full 30 s TTL).
    eval_fn = getattr(client, "eval", None)
    if eval_fn is not None:
        try:
            eval_fn(_RELEASE_LOCK_SCRIPT, 1, key, token)
            return
        except Exception as exc:
            if not _is_redis_response_error(exc):
                raise
            # This Redis refuses scripts: fall through to the plain compare-then-delete.
    current = client.get(key)
    current_str = current.decode() if isinstance(current, bytes) else current
    if current_str == token:
        client.delete(key)


# ---------------------------------------------------------------------------
# Storage path
# ---------------------------------------------------------------------------


def _shared_now() -> float:
    """Seconds since the epoch on the one clock every Dispatcharr worker shares: Redis's own TIME.

    The heartbeat stamps and the stopping marker are written by one worker and compared by another, possibly on
    another host, and their wall clocks differ by that host's skew; Redis answers for all of them (found by the
    2026-10-04 eighth hardening sweep). This does NOT protect against the host's own clock being stepped: Redis TIME is
    the Redis host's wall clock, and containers on one host share it (the usual deployment, and the all-in-one image
    runs Redis in the same container), so a manual change or a VM resume moves it as far as time.time(). The reaper's
    idle checks therefore do not subtract these stamps any more: they age a heartbeat on the reaper's own monotonic
    clock (`_IdleTracker`, 0.8.14). Falls back to this process's wall clock when Redis cannot say (not reachable, or a
    stand-in without TIME), which is what these timestamps always were.
    Timestamps that are only compared with this process's own filesystem (a directory's mtime, the owner file) stay on
    time.time()."""
    try:
        client = _redis()
        if client is not None:
            seconds, microseconds = client.time()
            return float(seconds) + float(microseconds) / 1_000_000.0
    except Exception:
        pass
    return time.time()


def _channel_dir(storage_path: str, channel_uuid: str) -> Path:
    # Validated as a well-formed UUID before ever being used as a path
    # segment. channel_uuid can reach here from caller-supplied run/
    # action params (_resolve_channel_uuid() rejects a non-UUID value
    # before it gets this far, in the normal case) or from Redis-
    # persisted buffer state (which a pre-fix version of this plugin
    # could have written unvalidated) -- checking here too, at the one
    # place every caller actually builds a filesystem path, closes both.
    # A value like "../recordings" would otherwise let start_buffer's
    # mkdir and stop_buffer's/the reaper's later shutil.rmtree() (see
    # _remove_channel_files()) operate entirely outside storage_path --
    # the same class of traversal _BufferRequestHandler._resolve_path()
    # already guards against for the HTTP file server, just not
    # previously applied here.
    try:
        uuid.UUID(str(channel_uuid))
    except (ValueError, AttributeError, TypeError) as exc:
        raise ValueError(f"invalid channel_uuid: {channel_uuid!r}") from exc
    return Path(storage_path) / str(channel_uuid)


# ---------------------------------------------------------------------------
# Minimal HTTP server for serving buffer files.
#
# Plugins can't register routes on Dispatcharr's own Django app (confirmed
# via apps/plugins/loader.py), and MEDIA_ROOT turned out to be unreachable
# in practice (see the module docstring). So this plugin serves storage_path
# itself, on its own port. Deliberately stdlib-only (http.server) rather
# than pulling in a dependency, matching Plugins.md's "keep dependencies
# minimal" guidance -- this only ever needs to serve small text playlists
# and a handful-of-seconds .ts segments to a single kind of client, nothing
# that needs a real web framework.
# ---------------------------------------------------------------------------

_http_server = None
_http_server_thread = None
_http_server_storage_path = None
# Reentrant: _ensure_http_server_running() calls _stop_http_server() on a config
# change while holding it. See _ensure_http_server_running()'s comment.
_http_server_lock = threading.RLock()


def _resolve_request_path(request_path: str, storage_path: str):
    """Pure logic pulled out of _BufferRequestHandler._resolve_path
    specifically so it's unit-testable without a real HTTP request/server
    (see ../tests/test_timeshift_buffer.py) -- resolves a raw HTTP request
    path (a handler's own self.path) against storage_path, returning
    (channel_uuid, resolved_filesystem_path), or (None, None) if the
    request doesn't map to a real location under storage_path.

    Manual traversal guard even though .resolve() below would also catch
    it -- fail fast and obviously rather than relying solely on path
    resolution semantics for something serving network requests."""
    raw = unquote(urlparse(request_path).path)
    # A percent-encoded null byte (e.g. "%00") decodes to a literal "\x00"
    # here, which Path(...).resolve() below raises an uncaught ValueError
    # on -- found via a project-wide review, confirmed by reproduction
    # (added 2026-09-27, a 52nd-pass audit): this function's own caller
    # (_resolve_and_authorize(), called from do_GET()/do_HEAD() with no
    # exception handling around it, since this is meant to be a pure,
    # already-validated path resolver) has nothing to catch that, so any
    # client able to reach this server's own port -- bound to 0.0.0.0, by
    # design, since it must be reachable from outside the container --
    # could trigger a dropped connection and a traceback logged for every
    # such request, with no token needed. Checked here rather than left to
    # .resolve() to reject, matching this function's own existing
    # "fail fast and obviously" convention for the traversal guard below.
    if "\x00" in raw:
        return None, None
    parts = raw.strip("/").split("/")
    if ".." in parts or len(parts) < 2:
        return None, None
    channel_uuid = parts[0]

    storage_root = Path(storage_path).resolve()
    candidate = (storage_root / raw.lstrip("/")).resolve()
    if storage_root not in candidate.parents and candidate != storage_root:
        return None, None
    return channel_uuid, candidate


# Distinct sentinel for _parse_range()'s "unsatisfiable" result, compared
# with `is` rather than `==` -- a plain (False, False) tuple used to be
# compared with `==`, and Python's `0 == False` meant a perfectly valid,
# satisfiable (0, 0) range (a client asking for just the first byte, e.g.
# via `bytes=0-0`) was indistinguishable from the unsatisfiable sentinel
# and got rejected with a spurious 416. A real bug, not hypothetical --
# found via a project-wide review, not reproduced live.
_RANGE_UNSATISFIABLE = object()

# Matches a token query param's value so it can be redacted out of this
# plugin's own HTTP access logging -- see _BufferRequestHandler.log_message's
# own comment for why. [^&\s]* stops at the next query param (&) or
# whitespace (the HTTP request line itself is space-separated, e.g.
# '"GET /uuid/seg.ts?token=xyz HTTP/1.1" 200 -'), so this only ever
# consumes the token value itself, never anything after it.
_TOKEN_QUERY_PARAM_RE = re.compile(r"token=[^&\s]*")


def _redact_token_query_param(text: str) -> str:
    """Replaces a token=... query param's value with REDACTED wherever it
    appears in `text`. Mirrors pvr.dispatcharr-unofficial's own identical
    redaction in ReadLiveTimeshiftStream()'s logging: this buffer's own
    access_token (see _check_access_token) grants unauthenticated read
    access to its segments for the buffer's whole lifetime, and this
    plugin's own logger may be configured to persist what it's given --
    a real gap found via a project-wide review, not itself independently
    reproduced."""
    return _TOKEN_QUERY_PARAM_RE.sub("token=REDACTED", text)


# Only the playlist and its segments are ever served. The channel directory also holds ffmpeg.log
# (unbounded) and ffmpeg.owner.json, which no client has any use for -- a token holder could read
# the log, and ask for it concurrently, since the token is the only access control here.
_SERVABLE_NAME_RE = re.compile(r"(live\.m3u8|seg_[0-9]+\.ts)")

# Size of each piece of a response body written to the socket.
_SEND_CHUNK_BYTES = 256 * 1024


def _is_servable_name(name: str) -> bool:
    return _SERVABLE_NAME_RE.fullmatch(name) is not None


def _client_key(address):
    """What the per-client connection cap counts a peer as: its IPv4 address (also when it arrives as an IPv4-mapped
    address on the dual-stack socket), or the /64 an IPv6 address belongs to. A host holding a /64 can use a different
    source address for every connection, so counting each address alone made the per-client cap meaningless for IPv6.
    The other side of that: every device of an IPv6 home LAN (one /64) shares one quota of max_connections_per_client,
    where IPv4 devices each have their own; with one request per connection and a handful of viewers it does not
    bind."""
    if not address:
        return None
    try:
        parsed = ipaddress.ip_address(address.split("%", 1)[0])
    except ValueError:
        return address
    if parsed.version == 6:
        if parsed.ipv4_mapped is not None:
            return str(parsed.ipv4_mapped)
        return str(ipaddress.IPv6Network((parsed, 64), strict=False))
    return str(parsed)


class _DeadlineSocketReader(io.RawIOBase):
    """Reads a socket the way StreamRequestHandler does, except that each recv waits at most
    until an absolute deadline (a time.monotonic() value from `deadline()`, or no deadline
    when that returns None) instead of a fresh full timeout every time. Raises socket.timeout
    (the same exception a plain timeout raises, which handle_one_request already treats as a
    request that timed out) once the deadline has passed."""

    def __init__(self, sock, deadline, default_timeout, byte_budget=None):
        self._sock = sock
        self._deadline = deadline
        self._default_timeout = default_timeout
        self._byte_budget = byte_budget
        self._request_bytes = 0

    def readable(self):
        return True

    def readinto(self, buffer):
        deadline = self._deadline()
        if deadline is None:
            self._sock.settimeout(self._default_timeout)
        else:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout("the request took too long to arrive")
            self._sock.settimeout(min(remaining, self._default_timeout))
        received = self._sock.recv_into(buffer)
        # Only while a request is being received (a deadline is set): the deadline bounds how long, this bounds how
        # much. http.server accepts a 64 KiB request line and 100 header lines of 64 KiB each, about 6.5 MB held in
        # memory per connection before the token is looked at; 64 connections of that size measured 418 MB resident.
        if deadline is not None and self._byte_budget is not None:
            self._request_bytes += received
            if self._request_bytes > self._byte_budget:
                raise socket.timeout("the request headers are larger than any real client sends")
        return received


class _BufferRequestHandler(BaseHTTPRequestHandler):
    """Serves GET /<channel_uuid>/<filename> straight from storage_path.

    Requires a valid ?token=... query param matching that buffer's own
    access_token (see _check_access_token) on every request -- this
    server has no other access control, and reachability alone (this port
    is meant to be exposed outside the container, same as Dispatcharr's
    own) previously meant readability. No directory listing, no write
    support. Does support Range requests
    (added for the growing-live-buffer byte-stream path -- see
    get_live_manifest below and pvr.dispatcharr-unofficial's DispatcharrClient,
    which mirrors its already-proven recording-playback Range-read pattern
    against individual segment files here instead of one Dispatcharr-served
    recording file)."""

    server_version = "TimeshiftBufferHTTP/0.1"

    # Fix for a real, confirmed resource-leak risk found via a
    # project-wide review (a 24th-pass audit), not itself independently
    # reproduced: BaseHTTPRequestHandler's own base class,
    # socketserver.StreamRequestHandler, defaults `timeout` to None (no
    # timeout at all), and this server is a ThreadingHTTPServer that
    # spawns one thread per connection with no limit on how many. Since
    # this port is meant to be reachable outside the container (see this
    # class's own docstring) and needs no token just to open a
    # connection (only to actually read a segment), anyone who can reach
    # it could open connections and never send a request line -- or a
    # viewer that vanishes mid-request/mid-response -- pinning a thread
    # and file descriptor here forever, in every SO_REUSEPORT worker
    # process. daemon_threads=True already means a hung thread can't
    # block _stop_http_server() itself, so this is a resource-leak
    # hardening fix, not a hang fix. 60s is generous enough for a slow
    # client still receiving one multi-MB segment body. (sendall()'s own
    # timeout, since Python 3.5, bounds the whole call; do_GET() sends a
    # body in chunks, under its own absolute deadline built on this.)
    timeout = 60

    # An ABSOLUTE budget for receiving the request line and headers. `timeout` above is a
    # socket timeout, which restarts with every recv, so a client sending one byte every few
    # seconds never finishes its request line (up to 64 KiB) or headers and holds a thread and
    # a descriptor here indefinitely, before any token check -- proven against the real
    # plugin 2026-10-04 (the 24th-pass fix above only closed the silent-connection case; a
    # silent connection was dropped at ~60 s, a dripping one was still open after 150 s).
    # Every real client (the Kodi addon, ffmpeg) sends its whole request at once.
    request_deadline_seconds = 10

    # The most a client may send before its request line and headers are complete: a real one (the Kodi addon, ffmpeg)
    # sends under 1 KiB, so 16 KiB is generous, and bounds what one connection can make the server hold in memory
    # (see _DeadlineSocketReader).
    request_header_budget_bytes = 16 * 1024

    # The slowest a client may drain a response body, for the body's own absolute deadline (see
    # do_GET()). A real viewer on even a poor link reads a segment far faster than this.
    body_min_rate_bytes_per_second = 32 * 1024

    def setup(self):
        super().setup()
        self._request_deadline = time.monotonic() + self.request_deadline_seconds
        self.rfile = io.BufferedReader(
            _DeadlineSocketReader(
                self.connection,
                lambda: self._request_deadline,
                self.timeout,
                self.request_header_budget_bytes,
            )
        )

    def parse_request(self):
        # The headers are in (parse_request reads them); whatever follows, a slow body or a slow
        # reader of a large response, is bounded by `timeout` per operation as before.
        try:
            return super().parse_request()
        finally:
            self._request_deadline = None
            # _DeadlineSocketReader shrank the socket's timeout to what was left of the request
            # deadline, and since Python 3.5 that timeout bounds a whole sendall(): without
            # restoring it a large segment sent to a slow reader was cut off after the seconds the
            # headers had left over (a 32 MiB body stopped after about 2.6 MiB, proven against 0.8.1).
            with contextlib.suppress(OSError):
                self.connection.settimeout(self.timeout)

    def log_message(self, fmt, *args):
        # Redacts a ?token=... query param's value before this ever
        # reaches the logger -- see _redact_token_query_param's own
        # comment. Formats eagerly here (rather than passing fmt/args
        # through for the logger's own lazy %-formatting) since the
        # request line this normally logs (self.requestline, via the
        # base class's own log_request()) is exactly where the token
        # lives, and there's no way to redact it after the fact once
        # %-substitution has already happened lazily inside the logger.
        logger = getattr(self.server, "plugin_logger", None)
        if logger:
            logger.debug("timeshift_buffer http: %s", _redact_token_query_param(fmt % args))

    def _resolve_path(self):
        """Returns (channel_uuid, filesystem_path), or (None, None) if the
        request doesn't map to a real file under storage_path."""
        return _resolve_request_path(self.path, self.server.storage_path)

    def _touch_heartbeat(self, channel_uuid):
        # Every successful fetch (playlist or segment) counts as "someone's
        # still watching" -- this is what lets the idle reaper work without
        # the Kodi addon (or any other client) needing to separately call
        # the heartbeat action on some timer it doesn't naturally have.
        # inputstream.ffmpegdirect re-fetches a live .m3u8 on an interval for
        # as long as playback continues and simply stops once it doesn't, so
        # this request stream IS the liveness signal, not just a proxy for
        # one. Best-effort: a Redis hiccup here shouldn't fail the actual
        # file response.
        try:
            _update_buffer_state(channel_uuid, lambda state: _apply_heartbeat(state, _shared_now()))
        except Exception:
            logger = getattr(self.server, "plugin_logger", None)
            if logger:
                logger.exception("timeshift_buffer: heartbeat-on-fetch failed for %s", channel_uuid)

    @staticmethod
    def _parse_range(range_header, file_size):
        """Parses a single-range "bytes=X-Y" / "bytes=X-" header value.
        Returns (start, end) inclusive, or None if absent/unparseable (caller
        falls back to serving the whole file) or _RANGE_UNSATISFIABLE if the
        range is unsatisfiable (caller sends 416) -- a distinct sentinel
        object, not a (False, False) tuple, so a valid (0, 0) result (the
        first byte only) is never mistaken for it."""
        if not range_header or not range_header.startswith("bytes="):
            return None
        spec = range_header[len("bytes=") :].split(",")[0].strip()  # first range only; multi-range unsupported
        if "-" not in spec:
            return None
        start_str, _, end_str = spec.partition("-")
        try:
            if start_str == "":
                # "bytes=-N" -- last N bytes.
                suffix_len = int(end_str)
                if suffix_len <= 0:
                    return None
                start = max(0, file_size - suffix_len)
                end = file_size - 1
            else:
                start = int(start_str)
                end = int(end_str) if end_str != "" else file_size - 1
        except ValueError:
            return None
        if start < 0 or start >= file_size or end < start:
            return _RANGE_UNSATISFIABLE
        return start, min(end, file_size - 1)

    def _check_access_token(self, channel_uuid):
        # Requires the caller to already know this specific buffer's own
        # token, issued only via the authenticated start_buffer/heartbeat
        # plugin actions (see _start_buffer's own comment) -- this file
        # server has no other access control of its own (see the module
        # docstring's "Idle-timeout liveness" bullet for why it's a plain
        # unauthenticated-by-Dispatcharr-standards HTTP server at all).
        # Without this, reaching this port at all (which the plugin's own
        # docs ask users to expose the same way as Dispatcharr's main
        # port -- often the whole LAN, sometimes further) was enough to
        # read any channel's currently-buffered live segments with zero
        # Dispatcharr credentials.
        state = _get_buffer_state(channel_uuid)
        expected = state.get("access_token") if state else None
        if not expected:
            return False
        provided = parse_qs(urlparse(self.path).query).get("token", [None])[0]
        if provided is None:
            return False
        # secrets.compare_digest() only accepts bytes-like objects or
        # ASCII-only strings -- a percent-encoded non-ASCII query value
        # (e.g. "?token=%C3%A9") raised an uncaught TypeError here, which
        # propagates straight through do_GET()/do_HEAD() with no try/except
        # around this call: no response is ever sent (not even a clean
        # 403), just a logged traceback. Not an auth bypass -- comparing
        # as UTF-8 bytes instead sidesteps the ASCII-only restriction
        # entirely while keeping the same timing-safe comparison; the
        # try/except is defensive (str.encode("utf-8") can't actually fail
        # for a well-formed str, but a request path shouldn't be trusted
        # to always produce one).
        try:
            return secrets.compare_digest(provided.encode("utf-8"), expected.encode("utf-8"))
        except UnicodeEncodeError:
            return False

    def _resolve_and_authorize(self):
        """Shared do_GET/do_HEAD preamble: resolves the request path,
        checks the access token, and confirms the target file exists --
        sending the appropriate error response itself on any failure.
        Returns (channel_uuid, target) on success, (None, None) otherwise
        (caller should just return immediately in that case)."""
        channel_uuid, target = self._resolve_path()
        if channel_uuid is None:
            self.send_error(404, "Not found")
            return None, None
        if not self._check_access_token(channel_uuid):
            self.send_error(403, "Forbidden")
            return None, None
        if target is None or not target.is_file() or not _is_servable_name(target.name):
            self.send_error(404, "Not found")
            return None, None
        return channel_uuid, target

    @staticmethod
    def _content_type_for(path) -> str:
        """Pure content-type selection for a served file -- pulled out of
        do_GET() specifically so it's unit-testable standalone; see
        ../tests/test_timeshift_buffer.py. mimetypes.guess_type() doesn't
        know either of these extensions on every platform/Python build, so
        both are pinned explicitly rather than left to guesswork; anything
        else falls back to a generic octet-stream."""
        content_type = mimetypes.guess_type(str(path))[0]
        if path.suffix == ".m3u8":
            content_type = "application/vnd.apple.mpegurl"
        elif path.suffix == ".ts":
            content_type = "video/mp2t"
        return content_type or "application/octet-stream"

    @staticmethod
    def _plan_range_response(range_result, file_size: int):
        """Turns _parse_range()'s result into the response shape do_GET()
        needs: (status, seek_start, read_length, content_range_header).
        Pure -- no file I/O. read_length is None for a plain 200
        (whole-file) response, matching do_GET()'s own unbounded f.read().
        Caller checks range_result is _RANGE_UNSATISFIABLE separately (a
        416 has no body to seek/read at all). Pulled out specifically so
        it's unit-testable standalone; see ../tests/test_timeshift_buffer.py."""
        if range_result is None:
            return 200, 0, None, None
        start, end = range_result
        return 206, start, end - start + 1, f"bytes {start}-{end}/{file_size}"

    def do_GET(self):
        channel_uuid, target = self._resolve_and_authorize()
        if channel_uuid is None:
            return

        content_type = self._content_type_for(target)

        try:
            f = target.open("rb")
        except OSError:
            # Segment was deleted by ffmpeg's -hls_flags delete_segments between the
            # playlist listing it and this request reading it -- a real,
            # expected race for a live-recycling buffer, not a bug. Treat
            # it the same as "not there right now."
            self.send_error(404, "Not found")
            return

        with f:
            # The size comes from the open file, not from a stat of the path beforehand: the muxer
            # rewrites live.m3u8 by writing a temp file and renaming it over, so a stat and a later
            # open could describe two different files and the response carried one's Content-Length
            # with the other's (shorter) body -- a truncated playlist (found by the 2026-10-04 seventh
            # hardening sweep, proven by forcing the interleaving).
            try:
                file_size = os.fstat(f.fileno()).st_size
            except OSError:
                self.send_error(404, "Not found")
                return
            range_result = self._parse_range(self.headers.get("Range"), file_size)
            if range_result is _RANGE_UNSATISFIABLE:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return

            status, start, length, content_range = self._plan_range_response(range_result, file_size)
            self._touch_heartbeat(channel_uuid)

            f.seek(start)
            remaining = file_size - start if length is None else length
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(remaining))
            if content_range:
                self.send_header("Content-Range", content_range)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            # In chunks, never the whole file at once: the body used to be read() into memory in one
            # piece, so a handful of concurrent requests for a large file allocated that much each
            # (found by the 2026-10-04 fourth hardening sweep). A file that shrinks mid-send ends the
            # body short, which the client sees as a truncated response.
            #
            # Each write is a sendall() bounded by the socket timeout, so chunking alone made the bound
            # per chunk: a client taking nearly `timeout` seconds to drain each 256 KiB held the
            # connection (and an open, possibly already-deleted, segment) for size/256 KiB times
            # `timeout` (proven by the fifth sweep). The body gets one absolute deadline instead -- the
            # per-operation timeout plus the time the whole body needs at a floor rate -- and each write's
            # timeout is what is left of it.
            deadline = time.monotonic() + self.timeout + remaining / self.body_min_rate_bytes_per_second
            while remaining > 0:
                left = deadline - time.monotonic()
                if left <= 0:
                    self.close_connection = True
                    break
                chunk = f.read(min(_SEND_CHUNK_BYTES, remaining))
                if not chunk:
                    break
                self.connection.settimeout(min(self.timeout, left))
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def do_HEAD(self):
        channel_uuid, target = self._resolve_and_authorize()
        if channel_uuid is None:
            return

        try:
            file_size = target.stat().st_size
        except OSError:
            # Same real, expected recycled-segment race do_GET's own
            # stat()/open() already guards against -- this one was
            # missing it, an asymmetry found via a project-wide review
            # (the addon itself never sends HEAD to this server, so low
            # impact in practice, but any other HEAD client would
            # otherwise get a dropped connection and a traceback in the
            # log instead of a clean 404).
            self.send_error(404, "Not found")
            return

        self._touch_heartbeat(channel_uuid)
        self.send_response(200)
        self.send_header("Content-Type", self._content_type_for(target))
        self.send_header("Content-Length", str(file_size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()


class _BufferHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer with SO_REUSEPORT set before bind.

    _http_server/_http_server_thread/_http_server_storage_path below are
    plain module-level globals -- fine for state genuinely local to one
    process, but Dispatcharr runs multiple WSGI worker processes (this is
    exactly why buffer state itself lives in Redis instead, see this
    file's own top-of-module comment), so each worker has its own
    separate copy of these globals. Without SO_REUSEPORT, only the first
    worker process ever asked to run a plugin action successfully binds
    this port; every other worker's own first attempt fails with "Address
    already in use" -- confirmed live, not theoretical: this is the exact
    error a real instance logged, repeatedly, during ordinary use. Worse
    than just log spam: if the one worker that *did* bind successfully
    later dies or gets recycled (routine for a WSGI server under normal
    operation), port 9192 goes completely unserved until some other
    worker happens to retry and win the now-open race -- and confirmed
    live that this window lines up with a real pvr.dispatcharr-unofficial
    live-timeshift stall (its HTTP reads against this server fail outright
    for as long as nothing is listening).

    SO_REUSEPORT lets every worker process bind its own socket on the same
    port at once, with the kernel load-balancing incoming connections
    across all of them -- safe specifically because every worker's
    listener serves identical content (the same shared storage_path files
    on disk), unlike the buffer state itself (needs one true, coordinated
    value -- Redis) or the reaper thread (must not run redundantly in
    every worker -- its own Redis leader election below). There's no
    "wrong" worker to answer a GET here, so there's nothing to coordinate:
    any worker's listener dying just means the others keep serving,
    without a gap, and a freshly-spawned replacement worker binds
    successfully on its own first attempt too."""

    # At most this many connections are handled at once per worker; one more is closed at once.
    # A thread and a descriptor per connection with no ceiling is what a flood of idle or
    # slow connections used to pin (see _BufferRequestHandler.request_deadline_seconds). Every
    # real use is a handful of short requests -- a Kodi viewer plus ffmpeg's playlist polls.
    max_connections = 256
    # ...and at most this many from one client address, so a single peer that needs no token to
    # open a connection cannot hold every slot against the real viewers (about 26 silent
    # connections a second keep 256 slots full for the 10 s request deadline). Generous against
    # real use -- a viewer plus ffmpeg's playlist polls is a handful -- because several viewers
    # behind one NAT share an address. Found by the 2026-10-04 third hardening sweep.
    max_connections_per_client = 64
    # A refusal is logged at most once per this many seconds, so a flood cannot also flood the log.
    refusal_log_interval_seconds = 30

    def __init__(self, *args, **kwargs):
        self._connection_slots = threading.BoundedSemaphore(self.max_connections)
        self._client_counts = {}
        self._client_counts_lock = threading.Lock()
        self._last_refusal_log = None
        self._refusals_since_log = 0
        super().__init__(*args, **kwargs)

    def _log_refusal(self, reason):
        now = time.monotonic()
        with self._client_counts_lock:
            self._refusals_since_log += 1
            if self._last_refusal_log is not None and now - self._last_refusal_log < self.refusal_log_interval_seconds:
                return
            count, self._refusals_since_log = self._refusals_since_log, 0
            self._last_refusal_log = now
        logger = getattr(self, "plugin_logger", None)
        if logger:
            logger.warning("timeshift_buffer http: refused %d connection(s) since the last report (%s)", count, reason)

    def _acquire_client_slot(self, client):
        with self._client_counts_lock:
            if self._client_counts.get(client, 0) >= self.max_connections_per_client:
                return False
            self._client_counts[client] = self._client_counts.get(client, 0) + 1
            return True

    def _release_client_slot(self, client):
        with self._client_counts_lock:
            remaining = self._client_counts.get(client, 0) - 1
            if remaining > 0:
                self._client_counts[client] = remaining
            else:
                self._client_counts.pop(client, None)

    def process_request(self, request, client_address):
        client = _client_key(client_address[0]) if client_address else None
        if not self._acquire_client_slot(client):
            self.shutdown_request(request)
            self._log_refusal("this client already has %d open" % self.max_connections_per_client)
            return
        if not self._connection_slots.acquire(blocking=False):
            self._release_client_slot(client)
            self.shutdown_request(request)
            self._log_refusal("%d already open" % self.max_connections)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._connection_slots.release()
            self._release_client_slot(client)
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._connection_slots.release()
            self._release_client_slot(_client_key(client_address[0]) if client_address else None)

    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        super().server_bind()


class _BufferHTTPServerV6(_BufferHTTPServer):
    """The same server on a dual-stack IPv6 socket (`::` with IPV6_V6ONLY
    off), which accepts IPv4 clients too (as IPv4-mapped addresses).

    Bound to `0.0.0.0` only, as it used to be, a client whose host resolves
    to an IPv6 address -- a non-Docker install configured with an IPv6-only
    or IPv6-preferred host, which this addon's own timeshift URLs already
    cater to (FormatHostForUrl()) -- could never reach this port."""

    address_family = socket.AF_INET6

    def server_bind(self):
        self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        super().server_bind()


def _create_http_server(port: int, logger):
    """Binds the file server, preferring a dual-stack IPv6 socket and
    falling back to IPv4 only where that isn't available (a container with
    IPv6 disabled has no `::` to bind -- most Docker setups). Returns
    (server, description of what it bound), or (None, None) if neither
    worked. A port that's simply taken fails both, and is reported once."""
    try:
        return _BufferHTTPServerV6(("::", port), _BufferRequestHandler), f"[::]:{port} (IPv4 and IPv6)"
    except OSError as exc_v6:
        v6_error = exc_v6
    try:
        server = _BufferHTTPServer(("0.0.0.0", port), _BufferRequestHandler)  # noqa: S104 -- must be reachable from outside the container by design
    except OSError as exc:
        logger.error("timeshift_buffer: couldn't bind http server on port %d: %s", port, exc)
        return None, None
    logger.info("timeshift_buffer: no dual-stack IPv6 socket available (%s) -- serving IPv4 only", v6_error)
    return server, f"0.0.0.0:{port} (IPv4 only)"


def _ensure_http_server_running(storage_path: str, port: int, logger):
    # Serialized: every run() calls this first, and several can land at once on
    # the first requests after a worker starts (gevent workers run each request in
    # its own greenlet). Unlocked, two of them both pass the "not running yet"
    # check and both bind -- SO_REUSEPORT lets the second succeed -- and the
    # module keeps only the second server's handle, so the first listener can
    # never be shut down by stop() and answers requests for the life of the worker.
    with _http_server_lock:
        _ensure_http_server_running_locked(storage_path, port, logger)


def _ensure_http_server_running_locked(storage_path: str, port: int, logger):
    global _http_server, _http_server_thread, _http_server_storage_path

    if _http_server is not None:
        if _http_server_storage_path == storage_path and _http_server.server_port == port:
            return  # already running with the same config
        logger.info("timeshift_buffer: http server config changed, restarting")
        _stop_http_server_locked(logger)

    server, bound = _create_http_server(port, logger)
    if server is None:
        return

    server.storage_path = storage_path
    server.plugin_logger = logger
    server.daemon_threads = True

    thread = threading.Thread(target=server.serve_forever, name=_HTTP_THREAD_NAME, daemon=True)
    # So that a later import of this module -- which starts with none of this
    # module's globals -- can still find and stop this listener, see
    # _stop_orphaned_threads().
    thread.tsb_server = server
    thread.start()

    _http_server = server
    _http_server_thread = thread
    _http_server_storage_path = storage_path
    logger.info("timeshift_buffer: serving %s on %s", storage_path, bound)


def _stop_http_server(logger):
    with _http_server_lock:
        _stop_http_server_locked(logger)


def _stop_http_server_locked(logger):
    global _http_server, _http_server_thread, _http_server_storage_path
    if _http_server is None:
        return
    try:
        _http_server.shutdown()
        _http_server.server_close()
    except Exception:
        logger.exception("timeshift_buffer: error stopping http server")
    _http_server = None
    _http_server_thread = None
    _http_server_storage_path = None


# ---------------------------------------------------------------------------
# ffmpeg process management
# ---------------------------------------------------------------------------


def _proxy_url(channel_uuid: str, base_url: str) -> str:
    # Confirmed reachable and working live, extensively, against the
    # deployment this was developed against. Assumes nginx itself listens
    # on this loopback address/port (matching the externally-mapped port in
    # docker-compose.yml); the modular compose config routes the web
    # service through a Unix socket behind nginx rather than necessarily
    # exposing a plain TCP port on its own, so this default may still need
    # adjusting on a differently-shaped deployment -- see the
    # internal_base_url setting's own help text.
    return f"{base_url.rstrip('/')}/proxy/ts/stream/{channel_uuid}"


# How long the access token put on ffmpeg's command line stays valid -- see _stream_attribution_headers().
_STREAM_TOKEN_LIFETIME_SECONDS = 120


def _stream_attribution_headers(params: dict, logger):
    """Returns an ffmpeg `-headers` string (each line `\\r\\n`-terminated)
    that makes the buffer's ffmpeg connection to /proxy/ts/stream/<uuid>
    attribute correctly in Dispatcharr's own Stats screen, or None if
    neither piece of info is available. Two independent problems, both
    confirmed by reading Dispatcharr's own source, not guessed:

    1. stream_ts() (apps/proxy/live_proxy/views.py) is decorated
       @api_view, so it's a DRF view -- request.user is only ever
       populated from DEFAULT_AUTHENTICATION_CLASSES (JWTAuthentication),
       never from a plain unauthenticated GET the way ffmpeg's own -i
       connection makes one. Without an Authorization header the
       connection registers with user=None, and StreamConnectionCard.jsx
       (frontend) shows any uid that's falsy or the string '0' as
       'Anonymous'. `params["username"]` is the Dispatcharr account
       pvr.dispatcharr-unofficial (or whichever client called start_buffer) is
       already configured with -- generating a token for it directly via
       rest_framework_simplejwt (this plugin runs in-process with Django)
       avoids a second login flow.
    2. Because this buffer's ffmpeg runs server-side inside Dispatcharr's
       own container rather than on the viewer's device, its connection's
       real REMOTE_ADDR is the container's own loopback address --
       confirmed live showing as 127.0.0.1 in Stats regardless of which
       device is actually watching. dispatcharr/utils.py's get_client_ip()
       honors X-Real-IP directly once REMOTE_ADDR itself is trusted (which
       127.0.0.1 is, by default). Deliberately X-Real-IP and not
       X-Forwarded-For: get_client_ip()'s XFF handling walks the chain and
       *skips* any hop that's itself in a trusted/private range -- correct
       for its usual reverse-proxy case, but wrong here, since a home-LAN
       viewing device's own IP (e.g. 10.x/192.168.x) is private too and got
       silently skipped as if it were just another internal proxy, leaving
       REMOTE_ADDR (127.0.0.1) as the answer either way -- confirmed live,
       this was exactly why XFF alone didn't work. X-Real-IP has no such
       filtering: it's trusted at face value once REMOTE_ADDR itself is."""
    lines = []

    username = (params.get("username") or "").strip()
    if username:
        try:
            from django.contrib.auth import get_user_model
            from rest_framework_simplejwt.tokens import RefreshToken

            User = get_user_model()
            user = User.objects.get(username=username)
            access = RefreshToken.for_user(user).access_token
            # ffmpeg is handed this on its command line, which /proc/<pid>/cmdline shows to
            # every local user, and it only authenticates once, when it connects (no -reconnect
            # is set, see _build_ffmpeg_command()). So the token lives for a couple of minutes
            # instead of the account's default access lifetime -- found by the 2026-10-04
            # second hardening sweep.
            access.set_exp(lifetime=timedelta(seconds=_STREAM_TOKEN_LIFETIME_SECONDS))
            access_token = str(access)
            lines.append(f"Authorization: Bearer {access_token}\r\n")
        except Exception:
            # Best-effort: a buffer that streams anonymously is still a
            # working buffer. Don't let a bad username block start_buffer.
            logger.exception(
                "timeshift_buffer: couldn't mint a stream-owner token for user %r -- "
                "buffer will stream anonymously (check the caller's Dispatcharr "
                "account still exists)",
                username,
            )

    client_ip = (params.get("client_ip") or "").strip()
    if client_ip:
        try:
            # ipaddress.ip_address() alone isn't enough: its own IPv6
            # zone/scope-id handling (the "%..." suffix, e.g.
            # "fe80::1%eth0") only rejects an empty scope id or one
            # containing another "%" -- anything else, including
            # embedded "\r\n", passes straight through as a "valid"
            # address. A real, confirmed bypass of this exact injection
            # guard, found via a project-wide review (2026-09-26):
            # "fe80::1%x\r\nX-Injected: evil" parses successfully,
            # reintroducing the same header-smuggling class this guard
            # was written to close in the first place (see this
            # function's own docstring/comment below). A zone/scope id
            # is never legitimate for this field anyway -- it only
            # disambiguates which LOCAL interface a link-local address
            # routes through, not something a REMOTE client's own
            # attribution address would ever carry -- so any "%" here is
            # rejected outright, closing the whole class rather than
            # just the one payload shape found. The explicit "\r"/"\n"
            # check is defense in depth on top of that, not reliant on
            # this being the only way ipaddress' own parsing could ever
            # let a control character through.
            if "%" in client_ip or "\r" in client_ip or "\n" in client_ip:
                raise ValueError("not a bare IP address")
            ipaddress.ip_address(client_ip)
        except ValueError:
            # A bare IP is all X-Real-IP is for -- anything else (in
            # particular embedded \r\n, which .strip() above only trims
            # from the ends, not the middle) doesn't belong in an HTTP
            # header value at all. Found via a full-codebase security
            # review (2026-09-10): unvalidated, this let a caller smuggle
            # a second, pipelined request onto ffmpeg's connection to
            # Dispatcharr's own loopback proxy. See docs/TIMESHIFT.md's
            # "client_ip header injection" section for the full writeup.
            logger.warning(
                "timeshift_buffer: rejecting non-IP client_ip value %r "
                "(stream will start without X-Real-IP attribution)",
                client_ip,
            )
        else:
            lines.append(f"X-Real-IP: {client_ip}\r\n")

    return "".join(lines) or None


def _int_setting(
    settings_dict: dict, key: str, default: int, minimum: int | None = None, maximum: int | None = None
) -> int:
    """Parses a numeric plugin setting, falling back to `default` for
    anything `int()` can't handle -- pulled out specifically so it's
    unit-testable standalone; see ../tests/test_timeshift_buffer.py.

    Fixed a real, confirmed bug found via a project-wide review, not
    itself independently reproduced: every one of this plugin's numeric
    settings (segment_seconds, buffer_minutes, http_port,
    idle_timeout_seconds, max_concurrent_buffers) was read with a bare
    `int(settings_dict.get(key, default))` -- Dispatcharr's own
    `_merge_settings_with_defaults()` (apps/plugins/loader.py) only fills
    in a *missing* key, not one present but empty, and its own frontend
    number field can save an emptied field as `""` -- so clearing any one
    of these in Dispatcharr's settings UI raised an uncaught
    `ValueError` at the very top of `run()`, breaking every action for
    that plugin instance, including `stop_buffer`/`stop_all` (exactly the
    actions someone would need to actually recover from a bad setting).

    `minimum` additionally floors an in-range-but-nonsensical value (a
    parsed 0 or negative number is not a parse failure `int()` would
    catch on its own) -- e.g. `segment_seconds=0` divides by zero in
    `_compute_segment_counts()`, and `idle_timeout_seconds=0` makes the
    reaper's own per-tick `_stale_viewers()` treat every viewer as
    stale immediately, tearing down every buffer on every reaper tick.

    `maximum` caps the same way, added 2026-09-26 (a 21st-pass audit,
    fixing a real, confirmed bug in the same class the `minimum` guard
    above already covers, found via a project-wide review, not itself
    independently reproduced): an in-range-looking `http_port` above
    65535 parses fine as a plain `int`, but `socket.bind()` then raises
    `OverflowError` (not `OSError`, the only exception
    `_ensure_http_server_running()` catches) -- and since that call runs
    unconditionally at the very top of `run()`, before action dispatch,
    it broke every action the exact same way an unparseable value did,
    including `stop_buffer`/`stop_all`.
    """
    try:
        value = int(settings_dict.get(key, default))
    except (TypeError, ValueError, OverflowError):  # OverflowError: int(float("inf")) from a JSON Infinity
        return default
    if minimum is not None and value < minimum:
        return minimum
    if maximum is not None and value > maximum:
        return maximum
    return value


def _str_setting(settings_dict: dict, key: str, default: str) -> str:
    """Parses a string plugin setting, falling back to `default` when the
    value is missing, not a string, or empty/whitespace-only after
    stripping -- the same class of bug `_int_setting()` above already
    fixes for numeric settings, added 2026-09-26 (a 23rd-pass audit,
    fixing a real, confirmed bug found via a project-wide review, not
    itself independently reproduced): Dispatcharr's own
    `_merge_settings_with_defaults()` (`apps/plugins/loader.py`) only
    fills in a *missing* key, not one present but emptied, and this
    plugin read `storage_path`/`internal_base_url` raw via a bare
    `settings_dict.get(key, default)` at every one of their own call
    sites.

    A cleared `storage_path` becomes `Path("")` -- the worker process's
    own current working directory -- so every buffer's files, the HTTP
    file server's own served root, and the reaper's orphan scan would
    all silently operate against the wrong directory instead of failing
    loudly. A cleared `internal_base_url` makes ffmpeg's own input URL a
    bare, unresolvable path, failing every single `start_buffer` with an
    unhelpful ffmpeg error instead of a clear "bad setting" one.

    Doesn't reject a relative path -- only an emptied/missing/non-string
    value falls back to `default`; a deliberately-relative
    `storage_path` (unusual, but not itself invalid) is left alone.
    """
    value = settings_dict.get(key, default)
    if not isinstance(value, str) or not value.strip():
        return default
    return value


def _compute_segment_counts(buffer_minutes: int, segment_seconds: int) -> tuple[int, int]:
    """Pure arithmetic core of _start_ffmpeg's segment-count sizing --
    pulled out specifically so it's unit-testable standalone; see
    ../tests/test_timeshift_buffer.py. Returns (visible_segments,
    wrap_segments).

    visible_segments floors at 1 (max(1, ...)) so a segment_seconds
    larger than the whole configured buffer window still produces a
    playable single-segment playlist rather than 0. Note the invariant
    `visible_segments * segment_seconds == buffer_minutes * 60` for any
    input where segment_seconds evenly divides the window -- an actual
    real-world debugging aid: this exact formula was cited in
    docs/TIMESHIFT.md to rule out a suspected ~89s audio-sync-error
    correlation, by showing no realistic buffer_minutes/segment_seconds
    combination could produce that number.

    wrap_segments (how many segment files are kept on disk, listed or not) is deliberately a larger count
    than what the playlist advertises as visible, so a client that just
    requested an old segment has headroom before ffmpeg overwrites that
    same filename in place -- the same class of "don't reveal/rely on
    something about to move under you" caution this project already
    applied to its own gradual-cap fix for in-progress-recording
    playback (see pvr.dispatcharr-unofficial's docs/RECORDINGS.md --
    that mechanism has since been replaced by a native-demuxer approach
    that doesn't need a cap at all, but the same underlying caution
    still applies here), just via a size margin here instead of a
    request-count hold.
    """
    visible_segments = max(1, (buffer_minutes * 60) // segment_seconds)
    wrap_segments = visible_segments * 2
    return visible_segments, wrap_segments


def _build_ffmpeg_command(
    input_url: str,
    attribution_headers: str | None,
    segment_seconds: int,
    wrap_segments: int,
    playlist_path,
    visible_segments: int,
    segment_pattern: str,
) -> list[str]:
    """Pure argv-assembly core of _start_ffmpeg -- pulled out specifically
    so the exact flag order is unit-testable standalone without touching
    subprocess/the filesystem; see ../tests/test_timeshift_buffer.py.

    -headers must precede -i: ffmpeg applies -headers to the input that
    follows it, not globally -- confirmed by this file's own top-of-file
    design notes on attribution headers, so this exact ordering isn't
    incidental. Deliberately NOT -reset_timestamps 1: see _start_ffmpeg's
    own comment on this file for the seeking regression that flag caused.

    The `hls` muxer, not `segment` (0.8.0, docs/TIMESHIFT.md "Packet corrupt"): the `segment` muxer starts every
    file with a fresh MPEG-TS muxer, so every PID's continuity counter restarts at 0 and each splice of two files
    into one byte stream -- which is what the addon does -- is a counter break the demuxer reports as "Packet
    corrupt" (measured: 14 of 14 splices, none after the counters were made continuous). The `hls` muxer keeps one
    muxer across segments and writes the same `seg_%05d.ts` files and `#EXT-X-MEDIA-SEQUENCE` playlist.
    """
    cmd = [
        "ffmpeg",
        "-nostdin",
        "-loglevel",
        "warning",
    ]
    if attribution_headers:
        cmd += ["-headers", attribution_headers]
    cmd += [
        "-i",
        input_url,
        "-c",
        "copy",
        "-f",
        "hls",
        "-hls_time",
        str(segment_seconds),
        "-hls_list_size",
        str(visible_segments),
        # Segment files kept on disk beyond the listed ones: with the playlist's visible_segments this is the
        # same 2x window the old -segment_wrap gave, so a client still reading a segment that has just left the
        # list has headroom before it is deleted.
        "-hls_delete_threshold",
        str(max(1, wrap_segments - visible_segments)),
        "-hls_flags",
        "delete_segments+omit_endlist",
        "-hls_segment_filename",
        segment_pattern,
        str(playlist_path),
    ]
    return cmd


# Names the ffmpeg this plugin started for a channel, inside that channel's own directory, so an
# ffmpeg whose Redis state has been lost can still be found and stopped -- see
# _reap_untracked_ffmpeg(). Found by the 2026-10-04 third hardening sweep: a Redis restart or flush
# while Dispatcharr (and so its ffmpeg children) kept running removed every buffer's state, the
# reaper and the orphan scrub only ever look at Redis-tracked buffers or at directories idle for
# 300 s, and a running hls muxer rewrites its directory every few seconds, so the ffmpeg held its
# provider slot and proxy connection indefinitely -- and the next start_buffer for that channel
# cleared the directory and spawned a second ffmpeg writing into the same one.
_OWNER_FILE_NAME = "ffmpeg.owner.json"


def _write_owner_file(channel_dir: Path, pid, start_ticks, started_at: float, logger):
    try:
        tmp = channel_dir / (_OWNER_FILE_NAME + ".tmp")
        tmp.write_text(json.dumps({"pid": pid, "pid_start_ticks": start_ticks, "started_at": started_at}))
        os.replace(tmp, channel_dir / _OWNER_FILE_NAME)
    except OSError:
        # Best effort: without it this buffer is only as recoverable as before.
        logger.exception("timeshift_buffer: couldn't record the ffmpeg owner file in %s", channel_dir)


def _read_owner_file(channel_dir) -> dict | None:
    """The owner file's contents, or None when it is absent or unusable (never raises)."""
    try:
        data = json.loads((Path(channel_dir) / _OWNER_FILE_NAME).read_text())
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not isinstance(data.get("pid"), int) or isinstance(data.get("pid"), bool):
        return None
    if data["pid"] <= 1:
        return None
    ticks = data.get("pid_start_ticks")
    started_at = data.get("started_at")
    return {
        "pid": data["pid"],
        "pid_start_ticks": ticks if isinstance(ticks, int) and not isinstance(ticks, bool) else None,
        "started_at": float(started_at) if isinstance(started_at, (int, float)) else None,
    }


def _live_owner(channel_dir) -> dict | None:
    """The owner file's contents when it names an ffmpeg that is still running."""
    owner = _read_owner_file(channel_dir)
    # Without a recorded start time (/proc was unreadable when it started) a recycled pid cannot be
    # told from the ffmpeg, so such a file is never acted on.
    if owner is None or owner["pid_start_ticks"] is None:
        return None
    if not _is_process_alive(owner["pid"], owner["pid_start_ticks"]):
        return None
    return owner


def _reap_untracked_ffmpeg(channel_dir: Path, logger) -> bool:
    """Stops the ffmpeg an owner file in `channel_dir` names, if it is still running. Only called
    where the caller has established no Redis-tracked buffer exists for this channel -- on a fresh
    start (holding the channel's start lock) and for a scrubbed orphan -- so a live ffmpeg named
    here is one nothing is tracking. _stop_ffmpeg() refuses a pid whose start time changed, so a
    recycled pid is never signalled."""
    owner = _live_owner(channel_dir)
    if owner is None:
        return False
    logger.warning(
        "timeshift_buffer: found an ffmpeg (pid %s) with no tracked buffer in %s -- stopping it",
        owner["pid"],
        channel_dir,
    )
    _stop_ffmpeg({"pid": owner["pid"], "pid_start_ticks": owner["pid_start_ticks"]}, logger)
    return True


def _clear_channel_dir(channel_dir: Path, logger):
    """Removes whatever a previous buffer instance left in this channel's
    directory before a fresh one starts writing into it.

    Only ever called on a fresh-start path, where the caller holds this
    channel's start lock and has already established there is no tracked
    buffer to reattach to -- so anything in here is a leftover, never a live
    buffer's files. It can exist with no Redis state behind it after a
    container restart (Redis doesn't persist across one, the storage volume
    does), after _BUFFER_STATE_TTL expired, or after a teardown's rmtree
    only half-finished before its state delete still ran.

    Left in place it did real damage (docs/OPEN_ITEMS.md): until the new
    ffmpeg closed its first segment and rewrote live.m3u8,
    _get_live_manifest() served the stale playlist, with sequence numbers far
    above the new instance's; the addon trimmed to its last few segments, and
    once the new playlist restarted at sequence 0 every entry of it was
    filtered out as "not new" -- a viewer stalled at the tail until the new
    numbers caught up with the old ones. It also kept an old directory mtime,
    so a reaper tick could rmtree it as a 300s-old orphan out from under a
    buffer that was just starting, and the missing cwd then surfaced from
    Popen as a misleading "ffmpeg not found". Recreating the directory gives
    it a fresh mtime too."""
    if not channel_dir.exists():
        return
    try:
        shutil.rmtree(channel_dir)
    except FileNotFoundError:
        return
    except OSError:
        # Best effort: a partly cleared directory is no worse than the
        # leftover that was there before, and ffmpeg still starts.
        logger.exception("timeshift_buffer: couldn't fully clear leftover files in %s", channel_dir)
    else:
        logger.info("timeshift_buffer: cleared leftover files from %s before starting a new buffer", channel_dir)


def _start_ffmpeg(channel_uuid: str, params: dict, settings_dict: dict, logger) -> dict:
    storage_path = _str_setting(settings_dict, "storage_path", "/data/timeshift")
    segment_seconds = _int_setting(settings_dict, "segment_seconds", 2, minimum=1)
    buffer_minutes = _int_setting(settings_dict, "buffer_minutes", 60, minimum=1)
    base_url = _str_setting(settings_dict, "internal_base_url", "http://127.0.0.1:9191")
    http_port = _int_setting(settings_dict, "http_port", 9192, minimum=1, maximum=65535)

    visible_segments, wrap_segments = _compute_segment_counts(buffer_minutes, segment_seconds)

    channel_dir = _channel_dir(storage_path, channel_uuid)
    # Before the directory is cleared (which would remove the owner file): an ffmpeg a lost Redis
    # state left running here would otherwise keep writing beside the new one.
    _reap_untracked_ffmpeg(channel_dir, logger)
    _clear_channel_dir(channel_dir, logger)
    channel_dir.mkdir(parents=True, exist_ok=True)
    playlist_path = channel_dir / "live.m3u8"
    # Deliberately a bare relative filename, not channel_dir / "...": ffmpeg
    # writes whatever this template evaluates to verbatim into the segment
    # list. An absolute path here would put absolute filesystem paths in
    # the .m3u8, which an HTTP client can't resolve as a URI against the
    # playlist's own URL. Run with cwd=channel_dir below so the actual
    # files still land in the right place on disk.
    segment_pattern = "seg_%05d.ts"

    # The actual argv assembly lives in _build_ffmpeg_command() above so
    # it's unit-testable standalone -- see that function's own docstring
    # for why -reset_timestamps 1 is deliberately not included.
    attribution_headers = _stream_attribution_headers(params, logger)
    cmd = _build_ffmpeg_command(
        _proxy_url(channel_uuid, base_url),
        attribution_headers,
        segment_seconds,
        wrap_segments,
        playlist_path,
        visible_segments,
        segment_pattern,
    )

    log_path = channel_dir / "ffmpeg.log"
    log_file = open(log_path, "ab")  # noqa: SIM115 -- lifetime tied to the subprocess, closed on stop
    proc = subprocess.Popen(  # noqa: S603 -- fixed argv, no shell, no user-controlled binary
        cmd,
        cwd=str(channel_dir),
        stdout=log_file,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,  # own process group, so SIGTERM below doesn't touch the plugin's own process
    )

    started_at = _shared_now()
    pid_start_ticks = _proc_start_ticks(proc.pid)
    # The local clock, not the shared one: the owner file's age is compared with time.time() by the orphan scrub
    # (_is_untracked_orphan()), and with Redis on another host the two clocks differ by that host's skew.
    _write_owner_file(channel_dir, proc.pid, pid_start_ticks, time.time(), logger)

    logger.info(
        "timeshift_buffer: started ffmpeg pid=%s for channel %s (buffer=%dmin, segment=%ds, visible=%d, wrap=%d)",
        proc.pid,
        channel_uuid,
        buffer_minutes,
        segment_seconds,
        visible_segments,
        wrap_segments,
    )

    return {
        "channel_uuid": str(channel_uuid),
        "pid": proc.pid,
        # The process's own start time (/proc/<pid>/stat field 22, in clock ticks since boot),
        # so a later signal or liveness check can tell this ffmpeg from an unrelated process
        # that has since been handed the same pid -- see _proc_start_ticks(). None when /proc
        # can't be read, in which case checks fall back to trusting the pid as before.
        "pid_start_ticks": pid_start_ticks,
        "started_at": started_at,
        "last_heartbeat": _shared_now(),
        "storage_path": storage_path,
        "playlist_path": str(playlist_path),  # on-disk path, for local debugging (ffmpeg.log lives next to it)
        "http_port": http_port,
        # Path component only -- the plugin can't know its own externally-
        # reachable hostname from inside the container. The caller builds
        # the full URL as http://<whatever host it already uses>:<http_port><playlist_route>.
        "playlist_route": f"/{channel_uuid}/live.m3u8",
        "log_path": str(log_path),
    }


def _stop_ffmpeg(state: dict, logger):
    pid = state.get("pid")
    if not pid:
        return
    start_ticks = state.get("pid_start_ticks")
    if start_ticks is not None:
        actual_ticks = _proc_start_ticks(pid)
        if actual_ticks is not None and actual_ticks != start_ticks:
            # The pid now belongs to a different process -- ours is long gone (a restart reset the
            # process table while Redis kept the state, or the pid wrapped). Signalling it would
            # SIGTERM, then SIGKILL, an unrelated process group.
            logger.warning(
                "timeshift_buffer: pid %s is no longer the ffmpeg this buffer started (its start time changed) "
                "-- not signalling it",
                pid,
            )
            return
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass  # already gone
    except Exception:
        logger.exception("timeshift_buffer: failed to signal ffmpeg pid %s", pid)
        return

    # Give it a moment to exit cleanly (flush the segment list/moov, etc.)
    # before escalating -- mirrors this project's own Plugins.md-documented
    # stop() pattern (track a pid, SIGTERM it, log the outcome). 2s, not the
    # 5s this used to be: confirmed live (pvr.dispatcharr-unofficial's own
    # CloseLiveTimeshiftStream() now waits on this call synchronously, so
    # its own duration is directly what a user feels as "how long does
    # Stop take") that ffmpeg here can take close to the full deadline to
    # exit on SIGTERM alone -- no crash, no error, SIGKILL was never
    # needed, it just isn't prompt about it. Escalating to SIGKILL sooner
    # is safe for this specific pipeline regardless of why: a plain stream
    # copy (-c copy) writing segment files has nothing meaningful to lose
    # from an abrupt kill -- this plugin's own manifest only ever exposes
    # segments ffmpeg has already fully closed (see _get_live_manifest()),
    # so a segment truncated mid-write by SIGKILL was already invisible to
    # every client and gets cleaned up/overwritten normally either way.
    # monotonic, not time.time(): a wall-clock step backwards kept this poll (and the request thread
    # waiting on it) spinning until the clock caught up, and a step forwards SIGKILLed at once (found by
    # the 2026-10-04 eighth hardening sweep).
    start = time.monotonic()
    deadline = start + 2
    while time.monotonic() < deadline:
        # _is_process_alive(), not a bare os.killpg(pid, 0): a zombie
        # (already exited, not yet reaped -- this poll's own worker isn't
        # generally ffmpeg's real parent, see that function's own
        # docstring) still answers signal 0 successfully, so a plain
        # os.killpg() check here used to wait out the *entire* 2s
        # deadline and send a SIGKILL every single time regardless of how
        # quickly ffmpeg actually exited -- a real, confirmed-live-
        # symptom-matching bug (found via a project-wide review): this is
        # very likely the actual cause behind docs/TIMESHIFT.md's own "A
        # plain Stop took ~5s" investigation, which attributed the delay
        # to ffmpeg's own slow SIGTERM response rather than this poll's
        # inability to tell a zombie apart from a still-running process.
        if not _is_process_alive(pid, start_ticks):
            logger.debug(
                "timeshift_buffer: ffmpeg pid %s exited %.1fs after SIGTERM",
                pid,
                time.monotonic() - start,
            )
            return
        time.sleep(0.2)

    logger.warning("timeshift_buffer: ffmpeg pid %s didn't exit after SIGTERM, sending SIGKILL", pid)
    with contextlib.suppress(ProcessLookupError):
        os.killpg(pid, signal.SIGKILL)


def _is_zombie_proc_stat(stat_text: str) -> bool:
    """Parses one line read from /proc/<pid>/stat (the "pid (comm) state
    ..." format) and reports whether its state field is "Z" (zombie --
    exited but not yet reaped by its real parent). comm can itself
    contain spaces or parentheses, so this looks for the LAST ')' rather
    than naively splitting the whole line on whitespace -- the same
    caveat any /proc/stat parser generally has to account for."""
    close_paren = stat_text.rfind(")")
    if close_paren == -1:
        return False
    fields = stat_text[close_paren + 1 :].split()
    if not fields:
        return False
    return fields[0] == "Z"


def _read_proc_pid_stat(pid) -> str | None:
    """Isolates the one filesystem read _is_process_alive's own zombie
    check needs, so tests can monkeypatch this alone rather than needing
    a real /proc filesystem (this plugin always runs inside a real Linux
    container in production, but not necessarily under test)."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read()
    except OSError:
        return None


def _proc_start_ticks_from_stat(stat_text: str) -> int | None:
    """The start time (field 22, clock ticks since boot) from one /proc/<pid>/stat line, or
    None when it can't be read. Like _is_zombie_proc_stat() this works from the LAST ')'
    because comm may contain spaces and parentheses: after it the fields are state (3),
    ppid (4) ... starttime (22), so starttime is index 19 of what follows."""
    close_paren = stat_text.rfind(")")
    if close_paren == -1:
        return None
    fields = stat_text[close_paren + 1 :].split()
    if len(fields) <= 19:
        return None
    try:
        return int(fields[19])
    except ValueError:
        return None


def _proc_start_ticks(pid) -> int | None:
    """When this pid's process started, or None if it is gone or /proc is unreadable. A pid
    number can be handed to a different process once ours has exited (a restart resets the
    process table while Redis, in a multi-container deployment, keeps the buffer's state;
    plain pid wraparound under churn does it too -- docs/CLOSED_ITEMS.md, "timeshift_buffer
    trusts a Redis-stored pid"), but the pair (pid, start time) is never reused."""
    stat_text = _read_proc_pid_stat(pid)
    if stat_text is None:
        return None
    return _proc_start_ticks_from_stat(stat_text)


def _is_process_alive(pid, start_ticks: int | None = None) -> bool:
    """Signal 0 checks a pid's existence without actually signaling it --
    works cross-worker-process (unlike Popen.poll()/os.waitpid(), which only
    work for the process's own parent), same reasoning as _stop_ffmpeg()'s
    own liveness poll above. Only tells you the process is gone, not its
    exit code -- getting that needs being the parent, which this plugin's
    Redis-tracked pid generally isn't (it's whichever WSGI worker process
    happened to handle the original start_buffer call, not necessarily this
    one).

    Signal 0 alone isn't enough, though: a zombie (already exited, but not
    yet reaped by its real parent) still answers signal 0 successfully --
    its process-table entry hasn't actually been freed yet -- so a plain
    kill(pid, 0) check reports it as "alive" indefinitely if nothing ever
    reaps it. That's a real risk here specifically because the Redis-
    tracked pid generally isn't this worker's own child (see above): if
    the worker that's the *actual* parent never calls wait()/waitpid() on
    it (its own Popen object went out of scope once _start_ffmpeg()
    returned, with nothing else ever calling .wait() on it), the zombie
    can persist until that worker process itself exits -- long enough to
    fool both start_buffer's dead-buffer cleanup and the idle-timeout
    reaper into treating an ffmpeg that already exited as still running.
    Reading /proc/<pid>/stat's own state field (rather than
    waitpid/Popen.poll()) works regardless of whether this call happens
    to be running in the real parent process, since any process in the
    same container can read it."""
    if not pid:
        return False
    try:
        os.killpg(pid, 0)
    except ProcessLookupError:
        return False
    except Exception:
        # e.g. PermissionError against a recycled, unrelated pid -- assume
        # alive rather than reap something still running
        return True

    stat_text = _read_proc_pid_stat(pid)
    if stat_text is None:
        # Can't confirm zombie state either way (e.g. this isn't actually
        # Linux, or a permissions/timing gap) -- same conservative
        # "assume alive" default as the exception branch above.
        return True

    if start_ticks is not None:
        actual_ticks = _proc_start_ticks_from_stat(stat_text)
        if actual_ticks is not None and actual_ticks != start_ticks:
            # The pid is alive, but it is somebody else's: the process this buffer started has
            # exited. Not a zombie check or a reap -- that process is not ours to touch.
            return False

    if _is_zombie_proc_stat(stat_text):
        # Opportunistically reaps it if this call does happen to be
        # running in the real parent worker -- best-effort; a
        # ChildProcessError/OSError here just means it isn't (the common
        # case, see this function's own docstring above), not an error
        # worth surfacing.
        with contextlib.suppress(ChildProcessError, OSError):
            os.waitpid(pid, os.WNOHANG)
        return False

    return True


class BufferFailedError(RuntimeError):
    """Distinguishes "ffmpeg already exited, this buffer will never produce
    a segment" from a plain RuntimeError's "not ready yet, keep waiting" --
    see _get_live_manifest()'s own comment on why the distinction matters."""


def _remove_channel_files(state: dict, logger):
    # shutil.rmtree rather than a flat glob+unlink+rmdir: simpler, and
    # robust to whatever this directory happens to contain rather than
    # assuming a flat file list.
    try:
        channel_dir = _channel_dir(state["storage_path"], state["channel_uuid"])
    except ValueError:
        # A corrupted/pre-fix Redis entry, not a real channel -- nothing
        # safe to remove. Caller (_teardown_buffer) still proceeds to
        # delete the Redis state itself either way, so this doesn't get
        # stuck retrying the same bad entry forever.
        logger.error(
            "timeshift_buffer: refusing to remove files for invalid channel_uuid %r", state.get("channel_uuid")
        )
        return
    try:
        shutil.rmtree(channel_dir)
    except FileNotFoundError:
        pass
    except OSError:
        logger.exception("timeshift_buffer: couldn't fully clean up %s", channel_dir)


#: How long _classify_existing_buffer() trusts a "stopping" marker as a
#: still-legitimate, in-progress teardown before treating it as an
#: abandoned one instead -- see that function's own comment. Comfortably
#: covers _stop_ffmpeg()'s own ~2s SIGTERM/poll/SIGKILL deadline plus
#: _remove_channel_files()'s own directory removal (up to a few thousand
#: segment files at the largest configured buffer sizes) and the Redis
#: state delete that follow it.
_TEARDOWN_GRACE_SECONDS = 30


def _classify_existing_buffer(
    existing: dict | None,
    is_alive: bool,
    now: float | None = None,
    expected_http_port: int | None = None,
    expected_storage_path: str | None = None,
) -> str:
    """Classifies a Redis-tracked buffer state for start_buffer's own
    reattach decision -- the pure decision core of its "existing" branch.
    One of five outcomes:

    - "none": nothing tracked for this channel -- a genuinely fresh start.
    - "stopping": tracked, and _teardown_buffer() has marked it
      mid-teardown *within the last _TEARDOWN_GRACE_SECONDS*
      (`stopping_since`, added 2026-09-26 in a 27th-pass audit, fixing a
      real, confirmed race found via a project-wide review, not itself
      independently reproduced) -- checked regardless of `is_alive`, not
      only when it's still True: _stop_ffmpeg() can make the process
      stop testing alive well before _teardown_buffer() actually finishes
      removing its files and deleting its own Redis state (file removal
      for a large buffer can itself take real time), and the old version
      of this function treated that entire remaining window as "dead"
      instead -- a concurrent start_buffer landing there ran its own
      full cleanup-and-restart, spawning a brand-new ffmpeg and writing
      brand-new state, which the *original*, still-in-flight teardown
      then finished by deleting -- an untracked ffmpeg left holding a
      provider stream slot until the container restarts, and a viewer
      handed a now-orphaned buffer. Reattaching to (or restarting
      alongside) a buffer that's committed to going away is unsafe
      either way; the caller is expected to retry shortly once teardown
      genuinely finishes.
    - "dead": tracked, but its ffmpeg process is gone, AND either it was
      never marked "stopping" at all, or its own "stopping" marker is
      older than the grace period above (or has no `stopping_since` at
      all -- a state written by an older version of this same plugin, or
      one this function otherwise can't time-bound) -- clean up and
      start fresh. This still self-heals a crash partway through
      _teardown_buffer() (after its own SIGTERM/SIGKILL sequence
      finished but before it got to remove files/delete state) the same
      way it always has, just correctly bounded by the grace period now
      instead of applying to the entire remaining teardown window
      unconditionally.
    - "stale_config": tracked and alive, but its own stored http_port/
      storage_path no longer matches the caller's current settings (added
      2026-09-26, a 33rd-pass audit, fixing a real, confirmed gap found
      via a project-wide review, not itself independently reproduced):
      _ensure_http_server_running() (called unconditionally at the top of
      every run(), on every worker) restarts that worker's own file
      server on a `storage_path`/`http_port` settings change, but this
      buffer's own already-running ffmpeg keeps writing segments under
      whatever `storage_path` it was actually launched with, and the
      caller who already has this buffer's old `http_port` has no way to
      learn the new one without a fresh start_buffer response telling it.
      Reattaching here would keep handing out the stale port/path pair
      indefinitely (nothing else ever refreshes it), leaving every
      current and future viewer of this channel unable to actually reach
      a single segment -- 404s against the new server root, or a
      connection refused once every worker has moved off the old port --
      while the old ffmpeg keeps holding a real provider stream slot and
      the idle reaper never reaps it, since heartbeats on this same
      "existing" branch keep refreshing `last_heartbeat` regardless.
      Treated by the caller like "dead" in spirit (clean up and start
      fresh with the current config), but needs an actual teardown
      first -- via _teardown_buffer(), not just the "dead" case's own
      state-only cleanup -- since the process is very much still alive.
      `expected_http_port`/`expected_storage_path` are `None` by default
      specifically so every existing caller that doesn't care to check
      config drift keeps working unchanged.
    - "reattach": tracked, alive, not (recently) stopping, and (when
      checked) its own stored config still matches -- the normal case.
    """
    if not existing:
        return "none"
    stopping_since = existing.get("stopping_since")
    if (
        existing.get("stopping")
        and isinstance(stopping_since, (int, float))
        and (now if now is not None else _shared_now()) - stopping_since < _TEARDOWN_GRACE_SECONDS
    ):
        return "stopping"
    if not is_alive:
        return "dead"
    if existing.get("stopping"):
        return "stopping"
    # "in existing", not just a truthy .get() -- an *absent* key (state
    # written by a plugin version old enough to predate recording this
    # field at all) means "unknown", not "confirmed different", the same
    # "don't assume" caution this codebase applies to every other
    # incomplete/legacy-state case. Only a key that's actually present
    # and disagrees counts as a real, confirmed mismatch.
    if expected_http_port is not None and "http_port" in existing and existing["http_port"] != expected_http_port:
        return "stale_config"
    if (
        expected_storage_path is not None
        and "storage_path" in existing
        and existing["storage_path"] != expected_storage_path
    ):
        return "stale_config"
    return "reattach"


def _same_buffer_instance(a: dict, b: dict) -> bool:
    """Whether two copies of a channel's buffer state describe the same
    running buffer. pid and started_at are set once, by _start_ffmpeg(), and
    never change for the life of one buffer instance (everything else --
    heartbeats, viewers, the stopping marker -- is rewritten all the time),
    so a restart is exactly what makes them differ."""
    return a.get("pid") == b.get("pid") and a.get("started_at") == b.get("started_at")


def _teardown_buffer(state: dict, logger, abort_if=None, holds_start_lock: bool = False) -> bool:
    """Stops ffmpeg, removes its segment files, and deletes the tracked
    state for a buffer -- the full "this buffer is done" sequence shared
    by the reaper, stop_buffer, a fatal get_live_manifest failure, and
    stop_all.

    Returns False, doing nothing, when the state Redis holds *now* says this
    teardown shouldn't happen:
      - it is a different instance of this channel's buffer than `state`.
        Callers read `state` earlier -- get_live_manifest before a manifest
        build that can take a few seconds, the reaper at the top of its tick
        -- and a concurrent start_buffer can classify that buffer "dead" and
        start a replacement in between. Tearing down from the stale copy then
        wrote the old state's stopping marker over the new one, rmtree'd the
        directory the new ffmpeg was writing into and deleted the new state
        (docs/OPEN_ITEMS.md);
      - `abort_if(current_state)` is true. stop_buffer and the reaper decide
        from a copy that says "nobody wants this buffer", and a start_buffer
        can reattach a viewer, or a heartbeat arrive, before the marker below
        lands -- the caller passes a predicate that re-checks against the
        freshest state, and it runs atomically with the marker write.
    A state that has disappeared altogether is neither: the old instance still
    has an ffmpeg to stop and files to remove -- but then nothing in Redis says
    who owns the directory any more, so the removal runs under the channel's
    start lock with a re-check (a start_buffer that finished after the read above
    must keep its new directory and state; found by the 2026-10-04 fifth hardening
    sweep, proven: both were gone afterwards). `holds_start_lock` is for the caller
    that is already inside the lock (_start_buffer_locked())."""
    why = {}

    def mark_stopping(current):
        if not _same_buffer_instance(current, state):
            why["replaced"] = current.get("pid")
            return None
        if abort_if is not None and abort_if(current):
            why["wanted"] = True
            return None
        current["stopping"] = True
        current["stopping_since"] = _shared_now()
        return current

    # Marked and persisted before anything else -- see
    # _classify_existing_buffer()'s own "stopping" case for the exact race
    # this closes: _stop_ffmpeg() below can take a couple of real seconds
    # (SIGTERM, poll, SIGKILL), long enough for a concurrent start_buffer
    # to land in between and reattach to a buffer that's about to have its
    # files removed and its state deleted out from under it. stopping_since
    # (added 2026-09-26, a 27th-pass audit) is what lets that same function
    # keep trusting this marker through the *rest* of this sequence too
    # (file removal, state delete), not just up until the process itself
    # stops testing alive -- see its own comment for the real bug this
    # closes. Written through _update_buffer_state() so a concurrent
    # heartbeat can no longer overwrite it with a copy that never had it.
    outcome, _marked = _update_buffer_state(state["channel_uuid"], mark_stopping)
    if outcome == "declined":
        if "replaced" in why:
            logger.warning(
                "timeshift_buffer: not tearing down channel %s -- its buffer was replaced since this teardown was "
                "decided (pid %s -> %s)",
                state["channel_uuid"],
                state.get("pid"),
                why["replaced"],
            )
        else:
            logger.info(
                "timeshift_buffer: not tearing down channel %s -- it is wanted again (a viewer or heartbeat arrived)",
                state["channel_uuid"],
            )
        return False
    state["stopping"] = True
    state["stopping_since"] = _shared_now()
    if outcome == "contended":
        # Heartbeats kept winning the race for ten attempts; the teardown
        # matters more than the tidiness of the write, so mark it the old way.
        logger.warning(
            "timeshift_buffer: couldn't mark %s stopping atomically -- writing the marker directly",
            state["channel_uuid"],
        )
        _set_buffer_state(state["channel_uuid"], state)
    if outcome == "absent" and not holds_start_lock:
        return _teardown_untracked_buffer(state, logger)
    _stop_ffmpeg(state, logger)
    _remove_channel_files(state, logger)
    _delete_buffer_state(state["channel_uuid"])
    return True


def _teardown_untracked_buffer(state: dict, logger) -> bool:
    """_teardown_buffer()'s tail for a buffer whose Redis state is already gone: stop the recorded
    ffmpeg, then remove its files and state only if nothing has been started for the channel since.
    Under the channel's start lock (skipped, as "a start is in progress", when held), with the state
    re-read: a different instance now tracked means a start_buffer finished meanwhile and owns the
    directory."""
    channel_uuid = state["channel_uuid"]
    token = _acquire_start_buffer_lock(channel_uuid)
    if token is None:
        logger.info(
            "timeshift_buffer: not removing the files of channel %s -- a start_buffer is in progress", channel_uuid
        )
        return False
    try:
        current = _get_buffer_state(channel_uuid)
        if current is not None and not _same_buffer_instance(current, state):
            logger.warning(
                "timeshift_buffer: not tearing down channel %s -- a new buffer was started since (pid %s -> %s)",
                channel_uuid,
                state.get("pid"),
                current.get("pid"),
            )
            return False
        _stop_ffmpeg(state, logger)
        _remove_channel_files(state, logger)
        _delete_buffer_state(channel_uuid)
        return True
    finally:
        _release_start_buffer_lock(channel_uuid, token)


def _is_canonical_uuid(name: str) -> bool:
    """True only for the exact canonical, lowercase, hyphenated 36-
    character form (str(uuid.uuid4()) style) -- what every real
    Dispatcharr channel uuid, and everything this plugin itself ever
    writes via _channel_dir(), actually looks like.

    uuid.UUID() alone is deliberately lenient (accepts 32-hex-no-hyphens,
    mixed case, {braced}, urn:uuid: prefixed, ...), which is more
    permissive than _find_orphaned_channel_dirs() below actually needs:
    that scan is deciding whether a directory belongs to *this plugin*,
    not merely whether its name happens to be UUID-shaped in some form.
    A user's own directory in shared storage that happens to satisfy
    uuid.UUID()'s loose parsing (e.g. some other tool's 32-hex identifier
    scheme) has no legitimate reason to be mistaken for one of this
    plugin's own buffer directories -- requiring the round-trip to match
    exactly closes that gap without changing behavior for any directory
    this plugin has ever actually created.
    """
    try:
        return str(uuid.UUID(name)) == name
    except (ValueError, AttributeError, TypeError):
        return False


def _find_orphaned_channel_dirs(storage_path: str, min_age_seconds: int) -> list:
    """Directories directly under storage_path with no matching
    Redis-tracked buffer state, old enough to rule out a buffer that's
    still mid-start.

    Exists because the normal cleanup paths (the reaper's idle-heartbeat
    check, stop_buffer, stop_all) all work by iterating *currently
    Redis-tracked* buffers -- confirmed live that this leaves a real gap:
    a channel directory whose Redis state key is simply gone (expired
    past _BUFFER_STATE_TTL with nothing left to refresh it -- e.g. a
    client killed hard enough that it never sent stop_buffer, and no
    heartbeat arrived again before the TTL ran out -- or a state write
    that never happened at all, e.g. a crash between _start_ffmpeg()
    creating the directory and _set_buffer_state() persisting it) is
    invisible to every one of those, since none of them ever look at
    what's actually sitting in storage_path independent of what Redis
    currently says. This closes that gap by reconciling the filesystem
    against Redis directly, the one place these leaks are actually
    visible from.

    A directory's own mtime changes whenever ffmpeg writes a new segment
    file into it (a new directory entry), so "how long since this
    directory's mtime" is a real idle-since signal for an actively
    written buffer, not just a creation timestamp -- and for a
    freshly-mkdir'd but not-yet-written one, mtime is the creation time
    itself, so min_age_seconds also covers _start_ffmpeg's own narrow
    directory-created-before-state-persisted window.
    """
    root = Path(storage_path)
    if not root.is_dir():
        return []
    now = time.time()
    return [entry for entry in root.iterdir() if _is_untracked_orphan(entry, min_age_seconds, now)]


def _is_untracked_orphan(entry: Path, min_age_seconds: int, now: float) -> bool:
    """Whether `entry` is an orphaned buffer directory right now: a canonical-UUID-named directory with
    no Redis state, idle (or, with a live owner process, started) at least `min_age_seconds` ago.
    Re-evaluated by _scrub_orphaned_dirs() per entry under the channel's start lock, because the list
    built by _find_orphaned_channel_dirs() can be seconds old by the time the scrub reaches an entry."""
    if not entry.is_dir():
        return False
    # Fix for a real, confirmed, high-severity bug: this scan never
    # checked that a directory's own name is actually one of the
    # UUID-named channel directories this plugin creates (the same
    # check _channel_dir() already applies before ever building a
    # path) -- so *any* directory directly under storage_path (e.g.
    # a user pointing storage_path at real, persistent storage they
    # also use for something else, as this project's own README
    # explicitly encourages) that happened to be old enough and
    # untracked in Redis got treated as an orphan and recursively
    # deleted below via shutil.rmtree(), unattended, every ~15s via
    # the reaper. The same class of incident as recording_edl's own
    # 2026-09-05 one, but reachable with no explicit user action at
    # all and via a full recursive delete rather than an
    # empty-directory-only sweep. Reproduced: a temp root containing
    # "recordings/", "db/", and one real UUID-named buffer directory,
    # all backdated past min_age_seconds, had all three removed
    # before this check existed.
    if not _is_canonical_uuid(entry.name):
        return False
    if _get_buffer_state(entry.name) is not None:
        return False  # tracked -- not an orphan
    try:
        age = now - entry.stat().st_mtime
    except OSError:
        return False
    # A running ffmpeg rewrites its directory every few seconds, so the directory's own mtime
    # never looks idle for one whose Redis state was lost; how long the process has been
    # running is the age that matters then (see _reap_untracked_ffmpeg()).
    owner = _live_owner(entry)
    if owner is not None and owner["started_at"] is not None:
        age = now - owner["started_at"]
    return age >= min_age_seconds  # younger: too recent to be sure it isn't just starting up


def _scrub_orphaned_dirs(storage_path: str, min_age_seconds: int, logger) -> list:
    removed = []
    for entry in _find_orphaned_channel_dirs(storage_path, min_age_seconds):
        # Per entry, under the channel's start lock, with everything re-checked: stopping an
        # untracked ffmpeg takes up to two seconds, so by the time the scrub reached a later entry a
        # start_buffer for it could have finished -- new ffmpeg, new owner file, new Redis state --
        # and the stale list would have stopped that new ffmpeg and deleted its directory (found by
        # the 2026-10-04 fourth hardening sweep; the rmtree half is older, the stop made the window
        # seconds wide). A start in progress holds the lock, and the scrub skips that channel.
        token = _acquire_start_buffer_lock(entry.name)
        if token is None:
            continue
        try:
            if not _is_untracked_orphan(entry, min_age_seconds, time.time()):
                continue
            try:
                # An orphan with a live ffmpeg is the lost-Redis-state case: stop the process
                # first, or it recreates the files as fast as they are removed and keeps its
                # provider slot.
                _reap_untracked_ffmpeg(entry, logger)
                shutil.rmtree(entry)
                removed.append(entry.name)
            except OSError:
                logger.exception("timeshift_buffer: couldn't scrub orphaned directory %s", entry)
        finally:
            _release_start_buffer_lock(entry.name, token)
    if removed:
        logger.info(
            "timeshift_buffer: scrubbed %d orphaned buffer director%s: %s",
            len(removed),
            "y" if len(removed) == 1 else "ies",
            ", ".join(removed),
        )
    return removed


_manifest_cache = {}
_manifest_cache_lock = threading.Lock()
# The most channels one worker keeps a cached manifest for. An entry holds a dict of every visible segment
# (about 400 KB at the defaults), and it is only removed in the worker that tears the buffer down
# (_delete_buffer_state()), so in every other worker an entry for a channel nobody watches any more stayed
# for the life of the process: 100 channels surfed was about 40 MB per worker (found by the twelfth
# hardening sweep). A dropped entry only means a cold rebuild on that channel's next call, which is what a
# worker that never served it does anyway. Well above max_concurrent_buffers (default 4), so a busy server
# never evicts a buffer that is in use.
_MANIFEST_CACHE_MAX_ENTRIES = 16


def _remember_manifest(channel_uuid, entry):
    """Stores a channel's cached manifest as the most recently used one and drops the least recently used
    entries beyond _MANIFEST_CACHE_MAX_ENTRIES. The caller holds _manifest_cache_lock."""
    _manifest_cache.pop(channel_uuid, None)
    _manifest_cache[channel_uuid] = entry
    while len(_manifest_cache) > _MANIFEST_CACHE_MAX_ENTRIES:
        _manifest_cache.pop(next(iter(_manifest_cache)))


def _parse_live_playlist_lines(lines):
    """The text half of _get_live_manifest(): the playlist's #EXT-X-MEDIA-SEQUENCE
    (0 when absent or unreadable) and, in list order, one (sequence, filename,
    duration_ms) triple per #EXTINF: line that is followed by a URI line. No
    filesystem access, so it is unit-tested directly.

    The sequence is the media sequence plus the entry's position in the list,
    assigned here before any entry is dropped later for a missing file, so a
    surviving entry's sequence never shifts. A malformed duration ("inf", text)
    becomes 0 rather than failing the whole manifest -- ffmpeg is the only
    realistic writer and never emits one, but a parser reading generated content
    shouldn't assume that; OverflowError is caught beside ValueError because
    float("inf") parses fine and only round() rejects it (the same gap
    recording_edl's _parse_edl had). The duration is read up to the first comma:
    #EXTINF:<duration>,<title> is valid HLS even though ffmpeg writes no title.
    An #EXTINF: followed by a blank line is skipped as a whole: an empty filename
    would otherwise be stat()ed as the channel directory itself and reported as a
    segment of that directory's size."""
    media_sequence = 0
    for line in lines:
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            with contextlib.suppress(ValueError):
                media_sequence = int(line[len("#EXT-X-MEDIA-SEQUENCE:") :].strip())
            break

    parsed = []
    list_index = 0  # position within the m3u8's own segment list, before any drops
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("#EXTINF:") and i + 1 < len(lines) and not lines[i + 1].startswith("#"):
            seg_name = lines[i + 1].strip()
            i += 2
            if not seg_name:
                continue
            sequence = media_sequence + list_index
            list_index += 1
            try:
                duration_ms = int(round(float(line[len("#EXTINF:") :].split(",", 1)[0]) * 1000))
            except (ValueError, OverflowError):
                duration_ms = 0
            parsed.append((sequence, seg_name, duration_ms))
        else:
            i += 1
    return media_sequence, parsed


def _get_live_manifest(state: dict, logger) -> dict:
    """Builds a byte-addressable manifest of the buffer's currently-listed
    (live.m3u8) segments -- filename, byte size, duration, and cumulative
    byte/time offsets -- so a client can treat the rolling live buffer as
    one growing, seekable byte stream (Range-reading individual segment
    files directly, see _BufferRequestHandler's Range support) instead of
    going through inputstream.ffmpegdirect's HLS-seek machinery, which
    pvr.dispatcharr-unofficial's docs/TIMESHIFT.md documents as confirmed broken for
    this kind of buffer.

    Always reflects the current state of live.m3u8 -- never stale -- but
    doesn't necessarily redo the work of getting there: `_manifest_cache`
    (module-global, per-worker-process, keyed by channel_uuid) skips
    re-parsing the playlist and re-stat()-ing every visible segment when
    nothing has actually changed on disk since this same process last read
    it (checked via the playlist file's own mtime/size -- confirmed cheap
    and sufficient, no need for content hashing), and even when it has
    changed, only stat()s segments genuinely new since last time -- a
    sequence number is never reused for the life of *one buffer instance*
    (HLS media sequence is monotonic, and with the 0.7.x -segment_wrap
    muxer filenames were recycled too), so a cache hit by sequence is guaranteed to be the
    exact same bytes, the same invariant pvr.dispatcharr-unofficial's own
    RefreshLiveManifest() already relies on client-side -- with two
    deliberate exceptions, both confirmed live to matter, not just
    theoretical:

    - The newest (last-listed) segment on any given call is always
      re-stat()'d even if it matches a cached entry, since a size sampled
      the very first moment a segment becomes visible could race a
      not-yet-fully-flushed write, and unlike every other cached entry,
      that risk can't yet have been disproven by a later segment having
      since appeared after it (see the inline comment where this is
      checked).
    - The *whole cache entry* for a channel_uuid is discarded outright,
      not partially trusted, if `state["access_token"]` (a fresh random
      value per genuine new buffer instance, see _start_buffer's own
      comment) doesn't match what the cache was built from -- "a sequence
      number is never reused" is only true *within* one continuously-
      running buffer instance; a channel stopped and later restarted
      (e.g. a channel switch away and back) gets a brand-new ffmpeg
      process whose live.m3u8 restarts numbering from scratch, reusing
      the exact same (sequence, filename) pairs for completely different,
      unrelated file content. An earlier version of this same check
      compared the buffer's pid instead -- confirmed live that isn't
      good enough: pids get recycled by the OS, and under heavy
      start_buffer/stop_buffer churn (many hours of repeated test
      cycles), a stale cache entry tagged with a pid the OS has since
      reassigned to a genuinely new instance passed the check it should
      have failed. See the inline comment where `buffer_token` is read
      for the live incidents (a channel switched away from and back,
      then a cold open with heavy same-day buffer churn, both "Packet
      corrupt" within seconds) that found and then fully closed this.

    Matters because
    this is called far more often than the buffer could possibly have
    grown -- the addon's own catch-up-to-tail loop and throttled length
    checks call this repeatedly while waiting, not just once per new
    segment -- and at this plugin's default settings (buffer_minutes=60,
    segment_seconds=2) the visible window is 1800 segments, meaning a
    naive rebuild-from-scratch call was up to 1800 stat() syscalls just to
    answer "did anything change". Per-worker rather than Redis-backed:
    avoids adding a second, differently-shaped piece of Redis-persisted
    state alongside the buffer's own lifecycle state, and a cold worker
    (one that's never seen this channel_uuid, or a round-robin request
    landing on a different worker than last time) just falls back to a
    full stat() sweep once, exactly like this function always did before
    -- never worse than the old behavior, only better when it helps.

    The rolling window means "byte offset 0" in THIS response corresponds
    to whatever's currently oldest -- a later call's "byte offset 0" will
    be different content once the window has advanced. A client that wants
    a stable address space across repeated calls (pvr.dispatcharr-unofficial does,
    to avoid its own position bookkeeping going stale mid-playback) can't
    just concatenate offsets naively; each segment also carries an absolute
    `sequence` number (HLS's own #EXT-X-MEDIA-SEQUENCE plus its position in
    the list), which is stable for the life of the buffer regardless of how
    the visible window slides, and is what a client should key its own
    merged/cumulative table on instead of list position. (Byte/time offsets
    themselves are always recomputed from the resolved segment list on
    every call, cache hit or not -- cheap, in-memory-only arithmetic, but
    not themselves cacheable, precisely because they're window-relative.)"""
    channel_uuid = state["channel_uuid"]
    try:
        channel_dir = _channel_dir(state["storage_path"], channel_uuid)
    except ValueError as exc:
        # Matches this function's own established contract (raise
        # RuntimeError for "can't produce a manifest" conditions, already
        # handled by _get_live_manifest_action()'s caller) rather than
        # letting a corrupted/pre-fix Redis entry's ValueError propagate
        # as an unrelated exception type.
        raise RuntimeError(f"invalid channel_uuid in buffer state: {channel_uuid!r}") from exc
    live_playlist_path = channel_dir / "live.m3u8"
    if not live_playlist_path.is_file():
        # Two very different situations produce the identical symptom here
        # -- no playlist yet -- and a caller retrying blindly on either one
        # can't tell them apart: a buffer that's still cold-starting (ffmpeg
        # running, just hasn't finished its first hls_time interval
        # yet) versus one that will *never* produce a playlist because
        # ffmpeg already exited (confirmed live: an upstream provider's own
        # concurrent-stream limit, already fully used by other channels,
        # makes Dispatcharr's live proxy refuse the connection ffmpeg is
        # reading from -- ffmpeg has no -reconnect flag set here, so it
        # just exits rather than retrying forever). Checking whether the
        # tracked pid is still alive distinguishes them cheaply, so a
        # caller (pvr.dispatcharr-unofficial's own OpenLiveTimeshiftStream() cold
        # -start retry loop) can fail fast on the second case instead of
        # retrying for its full ~15s budget against something that will
        # never succeed.
        if not _is_process_alive(state.get("pid"), state.get("pid_start_ticks")):
            log_tail = ""
            try:
                log_path = channel_dir / "ffmpeg.log"
                lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
                log_tail = " | ".join(lines[-5:])
            except OSError:
                pass
            raise BufferFailedError(
                "ffmpeg exited before producing any segments -- it will not "
                "recover on its own (a provider-side concurrent-stream limit "
                "is the most common cause)" + (f"; last ffmpeg.log lines: {log_tail}" if log_tail else "")
            )
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet")

    try:
        playlist_stat = live_playlist_path.stat()
    except OSError:
        # Translates a low-level race (playlist vanished between the
        # existence check above and this stat()) into the same
        # caller-facing message as that check -- the original OSError
        # adds nothing a caller needs, so deliberately not chained.
        raise RuntimeError("live playlist not found -- the buffer may not have produced any segments yet") from None

    # A playlist already existing doesn't mean the process producing it
    # still is -- ffmpeg can die well after writing its first segment (an
    # upstream drop, a provider-side concurrent-stream limit kicking in
    # mid-stream, no -reconnect flag set here -- the same root causes the
    # "no playlist yet" branch above already handles for a buffer that
    # dies before ever producing one). Nothing else rewrites live.m3u8 or
    # its segments once ffmpeg has exited, so every future call here would
    # otherwise keep succeeding with the exact same frozen manifest
    # forever -- indistinguishable, from a caller's perspective, from a
    # genuinely live buffer that's just momentarily quiet. A real,
    # confirmed-live-symptom-matching gap (found via a project-wide
    # review, not itself independently reproduced): this is the missing
    # half of the addon's own LiveTimeshiftStreamState::fatal short-
    # circuit, which only ever got tripped by the never-started case
    # above, never by a buffer that dies mid-playback -- so the addon's
    # catch-up-to-tail loop burned its full retry budget on every single
    # read, forever, instead of failing fast. A missing/falsy pid (older
    # plugin-version state, or a caller that never recorded one) is left
    # alone here rather than treated as dead -- same conservative
    # "can't confirm either way" bias _is_process_alive() itself already
    # applies to its own inconclusive cases.
    #
    # UPDATE (2026-10-02, 0.7.0, docs/OPEN_ITEMS.md "Dead-buffer detection destroys a paused/
    # rewound viewer's rewind window"): this used to raise BufferFailedError, whose handler
    # tears the whole buffer down on the spot -- so the first poll after ffmpeg died wiped the
    # segments a viewer paused or rewound well behind live was still entitled to play (a
    # paused client keeps polling this action). Dead is not the same as exhausted: the
    # playlist and every listed segment are still on disk and still valid, only growth has
    # stopped. So the frozen manifest is returned, flagged `ended`, and the caller stops
    # waiting at the tail instead of reading on forever; teardown is left to stop_buffer and
    # the idle reaper, which fire as soon as nobody is polling any more.
    ended = bool(state.get("pid")) and not _is_process_alive(state["pid"], state.get("pid_start_ticks"))

    # Ties every cache entry to the specific ffmpeg process (buffer
    # *instance*) it was built from -- confirmed live this matters, not
    # just theoretical: a channel stopped and later restarted (e.g. a
    # channel switch away and back) gets a brand-new ffmpeg process whose
    # live.m3u8 restarts numbering from scratch, so its early segments
    # have the exact same (sequence, filename) pairs as the *previous*
    # buffer instance's -- despite being completely different files with
    # different real content. _delete_buffer_state() clears this same
    # worker process's own cache on a clean stop, but that alone isn't a
    # sufficient guarantee across a multi-worker deployment: the stop_buffer
    # call that tears down the old instance can land on a *different*
    # worker than the one that cached get_live_manifest data for it, in
    # which case that worker's own stale cache never gets cleared at all,
    # and would otherwise be trusted again the moment it next handles a
    # request for the *new* instance of the same channel_uuid -- reusing a
    # completely wrong size for a same-named, same-sequenced, but
    # genuinely different segment (confirmed live: a channel switched away
    # from and back produced exactly this, "Packet corrupt" within
    # seconds of the fresh buffer starting, caught by pvr.dispatcharr-unofficial's
    # own Content-Range cross-check -- see docs/TIMESHIFT.md's "1.0.7
    # follow-up #2").
    #
    # Identified by `access_token`, NOT `pid` -- confirmed live this
    # distinction matters: an earlier version of this fix compared pid,
    # which reproducibly still failed under heavy start_buffer/stop_buffer
    # churn (many hours of repeated test cycles against one channel),
    # because OS pids get recycled -- a stale cache entry tagged with a
    # pid the OS has since reassigned to a genuinely new ffmpeg process
    # passes an equality check it has no business passing. `access_token`
    # (see _start_buffer's own comment -- secrets.token_urlsafe(24),
    # freshly generated for every genuine new buffer instance, never
    # reused for a *different* instance no matter how much churn happens)
    # has none of that risk. A mismatch here is treated exactly like a
    # cold cache, full stop, no partial trust of anything in it.
    buffer_token = state.get("access_token")

    with _manifest_cache_lock:
        cached = _manifest_cache.get(channel_uuid)
        # A missing/empty buffer_token (state predates the access_token
        # feature, or something else went wrong establishing identity)
        # can't be trusted to match anything, itself included -- fails
        # closed to a full cold rebuild rather than risk two different
        # "unknown" instances comparing equal.
        if cached is not None and (not buffer_token or cached.get("instance_token") != buffer_token):
            cached = None
        if (
            cached is not None
            and cached["playlist_mtime_ns"] == playlist_stat.st_mtime_ns
            and cached["playlist_size"] == playlist_stat.st_size
        ):
            # Nothing on disk has changed since our own last read of this
            # exact playlist file -- reuse it outright, no re-parse, not
            # even a re-read of the (small but non-zero) text file. dict
            # insertion order is what supplies list order here (guaranteed
            # since Python 3.7), matching how by_sequence was built below
            # on the call that populated this cache entry.
            media_sequence = cached["media_sequence"]
            ordered = [(seq,) + entry for seq, entry in cached["by_sequence"].items()]

            # Still re-stat()s the newest (last-listed) entry even on this
            # fast, nothing-changed path -- a real gap found via a
            # project-wide review: this function's own docstring/comment
            # below claims the newest segment is "always" re-verified even
            # on a cache match, but that re-stat previously lived only in
            # the "something changed" branch below, which never runs while
            # the playlist itself is unchanged. Under ffmpeg's own normal
            # one-segment-at-a-time behavior, a given segment is "newest"
            # for exactly the one call where it first appears (handled by
            # that branch), then immediately demoted on every call after
            # that -- meaning it would otherwise only ever get the single
            # earliest, highest-risk sample this whole mechanism exists to
            # double-check, with no actual second look ever happening.
            # Only matters if the underlying cross-process stat()
            # visibility lag this guards against is real (e.g. some
            # network filesystems); costs one extra stat() per call, same
            # as the other branch already accepts. Updates the cached
            # entry in place too (same dict object `cached` is), so a
            # corrected size is what the *next* fast-path hit reuses.
            if ordered:
                newest_seq, newest_name, _stale_size, newest_duration_ms = ordered[-1]
                try:
                    newest_size = (channel_dir / newest_name).stat().st_size
                except OSError:
                    # Recycled between being listed and this re-stat --
                    # drop it rather than report a segment that may no
                    # longer exist (same handling the other branch already
                    # gives its own newest-segment stat failure).
                    ordered = ordered[:-1]
                else:
                    ordered[-1] = (newest_seq, newest_name, newest_size, newest_duration_ms)
                    cached["by_sequence"][newest_seq] = (newest_name, newest_size, newest_duration_ms)
        else:
            lines = live_playlist_path.read_text(encoding="utf-8", errors="replace").splitlines()

            # First pass: pure text parsing, no filesystem access yet --
            # just the (sequence, filename, duration_ms) triples in
            # playlist order. Kept separate from size resolution below
            # so that step can tell which entry is the newest one.
            media_sequence, parsed = _parse_live_playlist_lines(lines)

            old_by_sequence = cached["by_sequence"] if cached else {}
            new_by_sequence = {}
            ordered = []
            newest_index = len(parsed) - 1
            for idx, (sequence, seg_name, duration_ms) in enumerate(parsed):
                # A sequence number is never reused for the life of a
                # buffer, so a hit here (same sequence, same filename) is
                # guaranteed to be the exact same bytes -- see this
                # function's own docstring -- with one deliberate
                # exception: the newest (last-listed) segment is never
                # trusted from cache, even on a match. ffmpeg only adds a
                # segment to the playlist once it's done writing it, but
                # confirming that "done" is visible to a stat() from a
                # separate process, on every filesystem, the *instant* the
                # entry first appears, isn't something this function
                # should assume -- a size sampled on that very first call
                # could plausibly race a not-yet-fully-flushed write.
                # Before this cache existed, that risk was harmless: the
                # *next* manifest call (of which pvr.dispatcharr-unofficial's own
                # cold-start retry loop issues several before ever reading
                # a byte) would simply re-stat and self-correct. Caching
                # turned a harmless, self-healing transient into a size
                # that, once wrong, stayed wrong for the rest of that
                # segment's time in the window -- exactly the shape of a
                # real, reproducible corrupt-playback report on a freshly
                # opened live-timeshift stream, which starts right at the
                # live edge (see pvr.dispatcharr-unofficial's own
                # kLiveEdgeMarginSegments) where the newest segment is
                # most likely to still be this fresh. Re-verifying just
                # the one newest entry costs at most one extra stat() per
                # call with something new -- in the common case (exactly
                # one new segment since last call) it costs nothing extra
                # at all, since that segment wasn't cached yet anyway.
                reusable = None if idx == newest_index else old_by_sequence.get(sequence)
                if reusable is not None and reusable[0] == seg_name:
                    size = reusable[1]
                else:
                    try:
                        size = (channel_dir / seg_name).stat().st_size
                    except OSError:
                        # Deleted (by the live buffer's own
                        # delete_segments) between the playlist listing it
                        # and this stat -- drop it rather than fail the
                        # whole manifest over one segment (sequence
                        # numbers for surviving entries are unaffected,
                        # since they were assigned from list position
                        # above, not from what survives here).
                        continue

                entry = (seg_name, size, duration_ms)
                new_by_sequence[sequence] = entry
                ordered.append((sequence,) + entry)

            _remember_manifest(
                channel_uuid,
                {
                    "instance_token": buffer_token,
                    "playlist_mtime_ns": playlist_stat.st_mtime_ns,
                    "playlist_size": playlist_stat.st_size,
                    "media_sequence": media_sequence,
                    "by_sequence": new_by_sequence,
                },
            )

    if not ordered:
        raise RuntimeError("no segments currently available -- the buffer may be too new")

    # Byte/time offsets are always recomputed here, cache hit or not --
    # cheap, in-memory-only arithmetic, but not themselves cacheable, since
    # they're window-relative (see this function's own docstring).
    segments = []
    cumulative_bytes = 0
    cumulative_ms = 0
    for sequence, seg_name, size, duration_ms in ordered:
        segments.append(
            {
                "filename": seg_name,
                "sequence": sequence,
                "byte_offset": cumulative_bytes,
                "byte_size": size,
                "time_offset_ms": cumulative_ms,
                "duration_ms": duration_ms,
            }
        )
        cumulative_bytes += size
        cumulative_ms += duration_ms

    return {
        "media_sequence": media_sequence,
        "segments": segments,
        "total_bytes": cumulative_bytes,
        "total_duration_ms": cumulative_ms,
        # True once ffmpeg has exited: the manifest is frozen, nothing more will ever be
        # appended. A reader behind the tail can carry on; one at the tail should end.
        "ended": ended,
    }


def _buffers_summary(buffers: list) -> str:
    """The list_buffers action's "message": the one part of its result
    Dispatcharr's Plugins page shows (PluginCard.jsx's handlePluginRun() renders
    nothing else). Each buffer by the first eight characters of its channel
    uuid, with its viewer count and age. Pure, so it is unit-tested directly."""
    if not buffers:
        return "No active buffers"
    parts = [f"{b['channel_uuid'][:8]} ({b['viewers']} viewer(s), {b['age_seconds']}s old)" for b in buffers]
    return f"{len(buffers)} active buffer(s): " + ", ".join(parts)


def _prune_stale_viewers(state, idle_timeout, now=None):
    """Drops any viewer_id whose own last-seen heartbeat is older than
    idle_timeout, in place on state["viewers"]/state["viewer_heartbeats"].
    Returns True if anything was actually pruned.

    Used by stop_buffer, which runs in a request worker with no memory between calls, so it can only subtract two
    stamps; the reaper uses _IdleTracker instead (a host clock step misfires THIS comparison, see docs/OPEN_ITEMS.md).

    Needed because last_heartbeat (refreshed by ANY successful file fetch,
    see _touch_heartbeat) is buffer-wide, not per-viewer: if one viewer
    crashes hard enough to never call stop_buffer, its viewer_id otherwise
    stays in state["viewers"] forever, kept "alive" by other viewers'
    ordinary segment fetches. That phantom entry then blocks stop_buffer's
    reference count from ever reaching zero once the real remaining
    viewers actually do stop -- the buffer (and the provider slot it
    holds) never gets torn down. A viewer with no recorded heartbeat yet
    (older plugin-version state from before this field existed, or a
    start_buffer call that raced this exact instant) is treated as fresh
    as of `now`, not as already stale, so an in-progress upgrade doesn't
    mass-prune viewers that just haven't had a chance to report in yet.
    """
    now = now if now is not None else _shared_now()
    viewers = state.get("viewers", [])
    if not viewers:
        return False
    heartbeats = state.get("viewer_heartbeats", {})
    fresh = [v for v in viewers if now - heartbeats.get(v, now) <= idle_timeout]
    if len(fresh) == len(viewers):
        return False
    state["viewers"] = fresh
    state["viewer_heartbeats"] = {v: heartbeats[v] for v in fresh if v in heartbeats}
    return True


class _IdleTracker:
    """Ages heartbeats on this process's monotonic clock instead of subtracting two wall-clock stamps (0.8.14).

    The reaper used to decide "idle" as `shared_now - last_heartbeat`. Both numbers come from the host's wall clock
    (Redis TIME is that clock when Redis shares the host, the usual deployment), so a step of more than
    `idle_timeout_seconds` between a heartbeat and the next tick (a manual change, a VM resume, a clock correction)
    made every watched buffer look idle at once: the viewers were pruned, the buffers stopped and the addon reported
    them gone; a step backwards made a really idle buffer look young until the clock caught up. Here the reaper
    remembers, per key, the heartbeat VALUE it last saw and when (monotonic) that value last changed; the age is the
    time since the change. A value that changes at all counts as a heartbeat, in either direction, so the stamp's
    meaning no longer matters, only that it moved. The first sighting of a key is age 0, so a reaper that has just
    started (or just won the election) gives every buffer one full `idle_timeout_seconds` to prove it is alive before
    reaping it; the cost is that a buffer abandoned while no reaper was watching lives that much longer. The clock
    is injectable for the tests."""

    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self._seen = {}  # key -> (last value seen, monotonic time that value was first seen)

    def age(self, key, value) -> float:
        """Seconds since `value` was first seen for `key` (0.0 for a new key or a changed value)."""
        now = self._clock()
        previous = self._seen.get(key)
        if previous is None or previous[0] != value:
            self._seen[key] = (value, now)
            return 0.0
        return now - previous[1]

    def retain(self, keys) -> None:
        """Forgets every key not in `keys` (a buffer or viewer that is gone must not be remembered)."""
        keep = set(keys)
        for key in [k for k in self._seen if k not in keep]:
            del self._seen[key]

    def clear(self) -> None:
        self._seen.clear()


def _drop_stale_viewers(state, stale):
    """Removes from `state` (in place) each viewer in `stale` ({viewer_id: heartbeat value the reaper saw}) whose
    heartbeat in `state` is STILL that value, and returns True when it removed any. Run against a copy re-read just
    before writing back: a viewer that heartbeated since the reaper looked has a different value and is kept, and one
    that registered in the gap is not in `stale` at all."""
    heartbeats = state.get("viewer_heartbeats", {})
    gone = [v for v, seen in stale.items() if v in state.get("viewers", []) and heartbeats.get(v) == seen]
    if not gone:
        return False
    state["viewers"] = [v for v in state["viewers"] if v not in gone]
    state["viewer_heartbeats"] = {v: hb for v, hb in heartbeats.items() if v not in gone}
    return True


def _stale_viewers(state, uuid, idle_timeout, tracker):
    """{viewer_id: last heartbeat value} for each viewer of `state` whose own heartbeat has not changed for longer
    than `idle_timeout` seconds on the tracker's clock. Needed because last_heartbeat (refreshed by ANY successful file
    fetch, see _touch_heartbeat) is buffer-wide, not per-viewer: a viewer that crashes hard enough never to call
    stop_buffer would otherwise stay in state["viewers"] forever, kept "alive" by other viewers' fetches, and block
    stop_buffer's reference count from reaching zero once the real viewers stop. A viewer with no recorded heartbeat
    (state from before the field existed, or a start_buffer that raced this instant) is treated as fresh and is not
    tracked, so an upgrade does not mass-prune viewers that have not reported in yet. Records every viewer it looked
    at in `tracker` and returns them with the stale ones."""
    stale = {}
    heartbeats = state.get("viewer_heartbeats", {})
    for viewer in state.get("viewers", []):
        value = heartbeats.get(viewer)
        if value is None:
            continue
        if tracker.age(("viewer", uuid, viewer), value) > idle_timeout:
            stale[viewer] = value
    return stale


def _resolve_viewer_id(params: dict):
    """The caller-supplied viewer_id as a non-empty str, or None when it's
    absent or unusable -- every action reads it through here rather than a
    bare params.get("viewer_id"). Pulled out specifically so it's
    unit-testable standalone; see tests/test_timeshift_buffer.py.

    Fix for a real, confirmed gap (added 2026-09-27, a 68th-pass audit,
    found via a project-wide review, confirmed by direct reproduction
    against this module, not reproduced live): viewer_id is caller-
    supplied JSON (Dispatcharr's own PluginRunAPIView passes `params`
    through untouched), and was used unchecked as a dict key and list
    element. Two real consequences, neither reachable from this plugin's
    own addon (which always sends a string) but both from any other
    client of this action:

    - A JSON array/object viewer_id raised TypeError ("unhashable type")
      at start_buffer's own `viewer_heartbeats` write -- on a fresh start,
      *after* _start_ffmpeg() had already spawned ffmpeg but before its
      state was ever saved, leaving an ffmpeg process nothing tracks
      (not stop_buffer, stop_all, nor the reaper), holding a real
      provider stream slot until the container restarts.
    - An integer viewer_id went into `viewers` as an int but came back
      out of Redis as a *string* key in `viewer_heartbeats` (JSON object
      keys are always strings), so _prune_stale_viewers()'s own
      `heartbeats.get(v, now)` never found it and treated that viewer as
      permanently fresh -- a crashed client's phantom viewer_id could then
      keep its buffer alive forever, the exact leak that function exists
      to prevent.

    An int (not a bool) is normalized to its str form rather than
    rejected, so a client that consistently sends an integer id keeps
    working as a reference-counted viewer -- rejecting it would instead
    make its stop_buffer fall through to an unconditional teardown,
    killing every other viewer's buffer. Anything else counts as no
    viewer_id at all, the same degraded-but-safe path an older addon
    version that never sends one already takes."""
    viewer_id = params.get("viewer_id")
    if isinstance(viewer_id, bool):
        return None
    if isinstance(viewer_id, int):
        return str(viewer_id)
    if isinstance(viewer_id, str) and viewer_id:
        return viewer_id
    return None


def _is_stale_access_token(state: dict, supplied) -> bool:
    """Whether a caller-supplied access_token identifies a *different*
    buffer instance than the one currently tracked for this channel --
    i.e. the caller's own buffer died and someone else's start_buffer
    replaced it with a fresh one (new random token, see _start_buffer).
    False when nothing was supplied (an older addon that never sends one)
    or when the tracked state predates access_tokens entirely: only a
    positively-confirmed mismatch counts. Flagged from an 11th-pass audit
    (docs/OPEN_ITEMS.md): get_live_manifest's merge-by-sequence-number
    logic on the addon side otherwise can't tell a replaced buffer apart
    from a merely-quiet one."""
    if not isinstance(supplied, str) or not supplied:
        return False
    expected = state.get("access_token")
    if not isinstance(expected, str) or not expected:
        return False
    return not secrets.compare_digest(supplied.encode(), expected.encode())


def _apply_heartbeat(state: dict, now: float, viewer_id=None) -> dict:
    """Pure state-mutation core of every heartbeat-only write-back path
    (_touch_heartbeat/_heartbeat/_get_live_manifest_action's own
    liveness touch) -- mutates only last_heartbeat, plus, if viewer_id is
    given, that viewer's own membership in `viewers` and its
    viewer_heartbeats entry, rather than touching anything else on
    `state`. Returns `state` (mutated in place) for convenient chaining.

    A viewer_id not currently in `viewers` is re-added, not ignored
    (changed 2026-09-29, `timeshift_buffer` 0.6.5, fixing a real gap
    flagged from a 10th-pass audit, docs/OPEN_ITEMS.md): once
    _prune_stale_viewers() dropped a still-watching viewer (e.g. a >30s
    network stall), its later heartbeat/get_live_manifest calls used to be
    ignored forever, so it stayed uncounted -- and the buffer got torn
    down under it as soon as every other viewer stopped. Cost of the
    fix: a straggler heartbeat arriving after that viewer's own
    stop_buffer resurrects it, bounded by the same idle_timeout prune
    (and moot when it was the last viewer, since the buffer's state is
    gone by then).
    Pulled out specifically so it's unit-testable standalone; see
    tests/test_timeshift_buffer.py.

    Callers matter as much as this function: each call site re-reads
    state from Redis immediately before calling this and writing the
    result back, rather than reusing a copy read earlier and held
    across any real work in between (_get_live_manifest() can mean up
    to ~1,800 stat() calls). Real, confirmed race this fixes: a
    concurrent start_buffer registering a new viewer in that gap had
    its registration silently overwritten once the expensive work
    finished and the stale, viewer-less copy got written back -- the
    "Concurrent viewers" bug (docs/TIMESHIFT.md) coming back through a
    race between two ordinary requests, not the already-fixed
    stop/reopen case that doc's own section covers."""
    state["last_heartbeat"] = now
    if viewer_id:
        viewers = state.setdefault("viewers", [])
        if viewer_id not in viewers:
            viewers.append(viewer_id)
        state.setdefault("viewer_heartbeats", {})[viewer_id] = now
    return state


# ---------------------------------------------------------------------------
# Idle reaper -- the one thing that actually needs a background loop, since
# nothing else ever stops a buffer once started. Leader-elected via Redis
# (SET NX with a TTL, renewed while alive) so only one worker process's
# thread is actually reaping at a time even though every worker loads this
# plugin module independently.
# ---------------------------------------------------------------------------


# How often a repeating reaper-tick failure is logged with its full traceback; the ticks in between
# are only counted. A Redis outage used to log a traceback every 15 s per worker, for as long as it
# lasted (found by the 2026-10-04 fifth hardening sweep).
_REAPER_ERROR_LOG_INTERVAL_SECONDS = 300


def _should_log_reaper_error(last_logged_at, now: float, interval: float = _REAPER_ERROR_LOG_INTERVAL_SECONDS) -> bool:
    """Whether a failed reaper tick at monotonic time `now` gets its full log entry: the first one, and
    then at most one per `interval`."""
    return last_logged_at is None or now - last_logged_at >= interval


# ffmpeg's stdout and stderr go to <channel dir>/ffmpeg.log, opened in append mode and emptied only by a fresh
# start. At -loglevel warning a source that warns for every packet could add hundreds of megabytes a day to the
# Dispatcharr data volume (a suspicion of the 2026-10-04 third hardening sweep), so the reaper trims it.
_FFMPEG_LOG_MAX_BYTES = 16 * 1024 * 1024
_FFMPEG_LOG_KEEP_BYTES = 256 * 1024


def _cap_ffmpeg_log(channel_dir, max_bytes: int = _FFMPEG_LOG_MAX_BYTES, keep_bytes: int = _FFMPEG_LOG_KEEP_BYTES):
    """Trim ffmpeg.log in `channel_dir` to its last `keep_bytes` once it exceeds `max_bytes`. ffmpeg holds the file
    open with O_APPEND, so truncating in place is safe: its next write lands at the new end. A line or two written
    between the read and the truncate is lost, which is why this keeps the tail rather than starting empty (the
    last lines are what a "ffmpeg exited" error quotes). Returns True when it trimmed; never raises."""
    path = Path(channel_dir) / "ffmpeg.log"
    try:
        size = path.stat().st_size
        if size <= max_bytes:
            return False
        with open(path, "rb") as f:
            f.seek(max(0, size - keep_bytes))
            tail = f.read()
        nl = tail.find(b"\n")
        if 0 <= nl < len(tail) - 1:
            tail = tail[nl + 1 :]  # start on a whole line
        os.truncate(path, 0)
        with open(path, "ab") as f:
            f.write(b"[timeshift_buffer: ffmpeg.log exceeded %d bytes and was trimmed to its last lines]\n" % max_bytes)
            f.write(tail)
        return True
    except OSError:
        return False


def _reaper_loop(settings_getter, logger, stop_event: threading.Event, idle_tracker=None):
    my_token = f"{os.getpid()}:{time.time()}"
    last_error_logged_at = None
    suppressed_errors = 0
    # Idle ages are measured here, on this process's monotonic clock, from when each heartbeat value last CHANGED (see
    # _IdleTracker); the stamps are never subtracted from a wall clock. Only the elected reaper keeps it up to date.
    tracker = idle_tracker if idle_tracker is not None else _IdleTracker()

    while not stop_event.is_set():
        try:
            # Fetched every tick, inside the try: a client taken once at thread start (and None, or a
            # connection that later died, for the rest of the worker's life) kept every later tick
            # failing, and since the thread stayed alive nothing ever restarted it -- no reaping or
            # scrubbing from that worker until Dispatcharr restarted.
            client = _redis()
            if client is None:
                raise RuntimeError("no Redis client available")
            got_leadership = client.set(_REDIS_LEADER_KEY, my_token, nx=True, ex=_REDIS_LEADER_TTL)
            if not got_leadership:
                current = client.get(_REDIS_LEADER_KEY)
                current_str = current.decode() if isinstance(current, bytes) else current
                if current_str == my_token:
                    got_leadership = True
                    client.expire(_REDIS_LEADER_KEY, _REDIS_LEADER_TTL)

            if not got_leadership:
                # A reaper that regains leadership later starts from a clean slate rather than from what it saw in an
                # earlier term (a buffer would otherwise be judged by a value remembered long ago).
                tracker.clear()
            if got_leadership:
                settings_dict = settings_getter()
                idle_timeout = _int_setting(settings_dict, "idle_timeout_seconds", 30, minimum=1)
                reaper_storage_path = _str_setting(settings_dict, "storage_path", "/data/timeshift")
                seen_keys = set()
                for state in _iter_buffer_states():
                    uuid = state.get("channel_uuid")
                    if not uuid:
                        continue  # nothing to track or reap by; the scrub deals with such a leftover
                    seen_keys.add(("buffer", uuid))
                    seen_keys.update(
                        ("viewer", uuid, v) for v in state.get("viewers", []) if v in state.get("viewer_heartbeats", {})
                    )
                    # a state whose uuid is not usable as a path is the scrub's business, not this trim's. The buffer's
                    # own storage path, not the current setting: after a change of the setting a running buffer's log
                    # stays where it was started (the state keeps it for exactly that reason, see
                    # _remove_channel_files()).
                    with contextlib.suppress(ValueError, KeyError):
                        _cap_ffmpeg_log(_channel_dir(state.get("storage_path") or reaper_storage_path, uuid))
                    stale = _stale_viewers(state, uuid, idle_timeout, tracker)
                    if stale:
                        logger.info(
                            "timeshift_buffer: pruned stale viewer(s) for channel %s (no heartbeat for %ds)",
                            uuid,
                            idle_timeout,
                        )
                        # Re-run the prune against a copy re-read right
                        # before writing back, rather than the copy
                        # _iter_buffer_states() read moments earlier --
                        # narrows the same race _apply_heartbeat()'s own
                        # comment describes: a concurrent start_buffer
                        # registering a new viewer in this gap would
                        # otherwise have its registration silently
                        # overwritten by this stale write-back. A viewer
                        # whose heartbeat moved since is kept.
                        _update_buffer_state(
                            uuid,
                            lambda fresh_state, gone=stale: (_drop_stale_viewers(fresh_state, gone), fresh_state)[1],
                        )
                    last_heartbeat = state.get("last_heartbeat", 0)
                    buffer_age = tracker.age(("buffer", uuid), last_heartbeat)
                    if buffer_age > idle_timeout:
                        logger.info(
                            "timeshift_buffer: reaping idle buffer for channel %s (no heartbeat for %ds)",
                            uuid,
                            int(buffer_age),
                        )
                        # Re-checked against the freshest state, atomically with the
                        # stopping marker: a heartbeat that landed since this tick read
                        # `state` (its value moved) means someone is watching after all.
                        _teardown_buffer(
                            state,
                            logger,
                            abort_if=lambda fresh, seen=last_heartbeat: fresh.get("last_heartbeat", 0) != seen,
                        )
                tracker.retain(seen_keys)

                # Reconciles storage_path against Redis directly, catching
                # the class of leak the loop above structurally can't (see
                # _find_orphaned_channel_dirs' own comment) -- makes
                # scrub_orphaned_buffers a manual-cleanup convenience
                # rather than the only way this ever gets fixed. Same
                # min-age floor reasoning as that action's own default:
                # at least 5 minutes regardless of a shorter
                # idle_timeout_seconds, since there's no tracked state
                # here to double-check against before deleting.
                _scrub_orphaned_dirs(reaper_storage_path, max(idle_timeout, 300), logger)
            if suppressed_errors:
                logger.info(
                    "timeshift_buffer: the reaper recovered after %d failed tick(s) whose errors were not logged",
                    suppressed_errors,
                )
                suppressed_errors = 0
            last_error_logged_at = None
        except Exception:
            now_mono = time.monotonic()
            if _should_log_reaper_error(last_error_logged_at, now_mono):
                last_error_logged_at = now_mono
                logger.exception(
                    "timeshift_buffer: reaper tick failed (%d similar failure(s) since the last report)",
                    suppressed_errors,
                )
                suppressed_errors = 0
            else:
                suppressed_errors += 1

        stop_event.wait(15)


_orphans_swept = False


def _stop_orphaned_threads(logger):
    """Stops the file-server and reaper threads an EARLIER import of this
    module left running in this process, once per import.

    Dispatcharr reloads plugins by dropping a plugin's modules from
    sys.modules and importing them afresh -- on any plugin install, enable,
    disable or delete, for every enabled plugin -- and does not call the old
    module's stop() first, except in the one worker that handled that request.
    _http_server/_reaper_thread/_latest_settings_dict are plain module globals,
    so the fresh import starts with None/{} and no reference to what the old
    copy started; _ensure_http_server_running()/_ensure_reaper_running() then
    start a second listener (which SO_REUSEPORT lets bind beside the first) and
    a second reaper. The old listener kept answering segment requests from its
    own stale storage_path, split from the new one by the kernel; the old
    reaper kept reading its own frozen settings and, whenever it held Redis
    leadership, reaping and scrubbing by them (docs/OPEN_ITEMS.md).

    The threads themselves survive the reload, so they are found by name in
    threading.enumerate() and stopped through handles attached to them at
    creation (`tsb_server`, `tsb_stop_event`). Threads started by a version of
    this plugin that predates those handles can't be stopped this way -- only a
    Dispatcharr restart clears them -- and are reported once instead.

    Run once per import of this module (every worker imports it separately):
    nothing new can be orphaned afterwards except by another reload, which
    produces a new import of its own."""
    global _orphans_swept
    if _orphans_swept:
        return
    _orphans_swept = True
    unstoppable = 0
    for thread in threading.enumerate():
        if thread.name == _HTTP_THREAD_NAME and thread is not _http_server_thread:
            server = getattr(thread, "tsb_server", None)
            if server is None:
                unstoppable += 1
                continue
            try:
                server.shutdown()
                server.server_close()
                logger.info("timeshift_buffer: stopped a file server left running by an earlier load of this plugin")
            except Exception:
                logger.exception("timeshift_buffer: couldn't stop an orphaned file server")
        elif thread.name == _REAPER_THREAD_NAME and thread is not _reaper_thread:
            stop_event = getattr(thread, "tsb_stop_event", None)
            if stop_event is None:
                unstoppable += 1
                continue
            stop_event.set()
            logger.info("timeshift_buffer: stopped a reaper left running by an earlier load of this plugin")
    if unstoppable:
        logger.warning(
            "timeshift_buffer: %d file server/reaper thread(s) from an older version of this plugin are still "
            "running in this worker and can't be stopped from here -- they go away on the next Dispatcharr restart",
            unstoppable,
        )


def _ensure_reaper_running(settings_dict, logger):
    global _reaper_thread, _reaper_stop_event, _latest_settings_dict
    # Real, confirmed bug this fixes (found via a project-wide review):
    # this function only actually starts the thread on the very first
    # call -- every run() after that still built a fresh
    # `lambda: settings_dict` closure over *that* call's own dict, but it
    # was simply discarded by the early-return below, so the reaper kept
    # reading whichever settings_dict object happened to exist at the
    # moment it first started, forever. A later change to
    # idle_timeout_seconds/storage_path in Dispatcharr's own Plugin
    # Settings UI never reached the reaper thread until the worker
    # process itself restarted. Updating this module-level global on
    # every call, unconditionally (not just the first), and having the
    # reaper thread's own getter read *this* name rather than close over
    # a specific call's dict, is what actually lets a later run() call's
    # settings reach it.
    _latest_settings_dict = settings_dict
    # Check-then-start under a lock: two run() calls landing together on a fresh
    # worker would both pass the check, and the second overwrote
    # _reaper_stop_event, leaving the first thread with no way to be stopped.
    with _reaper_lock:
        if _reaper_thread is not None and _reaper_thread.is_alive():
            return
        _reaper_stop_event = threading.Event()
        _reaper_thread = threading.Thread(
            target=_reaper_loop,
            args=(lambda: _latest_settings_dict, logger, _reaper_stop_event),
            name=_REAPER_THREAD_NAME,
            daemon=True,
        )
        _reaper_thread.tsb_stop_event = _reaper_stop_event  # see _stop_orphaned_threads()
        _reaper_thread.start()


# ---------------------------------------------------------------------------
# Plugin class
# ---------------------------------------------------------------------------


class Plugin:
    name = "Timeshift Buffer"
    version = "0.8.14"
    description = (
        "Server-side rolling live-TV buffer per channel, so clients can "
        "pause/rewind live playback without a local on-device buffer."
    )
    author = "BruiserBrody17"
    help_url = (
        "https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/tree/Omega/dispatcharr-plugin/timeshift_buffer"
    )

    # The single source of truth for fields/actions -- confirmed live that
    # plugin.json's own copies (which Plugins.md's Quick Start example
    # duplicates alongside these, but this project doesn't) are never
    # actually read: PluginImportAPIView hardcodes an empty fields/actions
    # preview for a not-yet-trusted plugin regardless of plugin.json, and
    # once trusted/loaded, the running Plugin class (here) is what's
    # actually introspected. Tested directly: stripping fields/actions out
    # of plugin.json entirely while leaving this class untouched produced
    # an identical plugin listing.
    fields = [
        {
            "id": "about",
            "label": "About",
            "type": "info",
            "description": (
                "Started/stopped per channel by a client (e.g. "
                "pvr.dispatcharr-unofficial's live-timeshift setting) via the plugin "
                "run/ API, not usually by hand. The buttons below are for "
                "manual testing and emergency cleanup."
            ),
        },
        {
            "id": "storage_path",
            "label": "Buffer storage path",
            "type": "string",
            "default": "/data/timeshift",
            "help_text": (
                "Container path where segment files are written. Point this "
                "at a Docker volume mapped to real storage (the same way "
                "you'd map /data/recordings) -- do NOT leave this under "
                "Dispatcharr's own app directory, since continuous rolling "
                "writes don't belong on a small/fast appdata volume."
            ),
        },
        {
            "id": "buffer_minutes",
            "label": "Buffer length (minutes)",
            "type": "number",
            "default": 60,
            "help_text": (
                "How far back a viewer can rewind. Drives both "
                "hls_list_size (what the playlist advertises) and "
                "hls_delete_threshold (when old segment files are deleted)."
            ),
        },
        {
            "id": "segment_seconds",
            "label": "Segment length (seconds)",
            "type": "number",
            "default": 2,
            "help_text": (
                "ffmpeg -hls_time. A client only sees new content once a "
                "segment closes, so shorter segments mean less stalling/"
                "rebuffering during ordinary playback, at the cost of more, "
                "smaller files on disk and more requests to this plugin's own "
                "file server. Confirmed live at the default (2s) with several "
                "channels buffering concurrently -- steady, error-free segment "
                "production throughout, see the addon's own docs/TIMESHIFT.md "
                "for the full account."
            ),
        },
        {
            "id": "idle_timeout_seconds",
            "label": "Idle timeout (seconds)",
            "type": "number",
            "default": 30,
            "help_text": (
                "Stops a channel's buffer if no heartbeat arrives for this "
                "long -- only a backstop for a client that disappears without "
                "cleanly closing (a crash, a network drop), not a normal "
                "Stop, which tears the buffer down immediately regardless. "
                "Kept short so an abandoned buffer doesn't occupy one of a "
                "provider's concurrent-stream slots for long. See "
                "pvr.dispatcharr-unofficial's docs/TIMESHIFT.md for details."
            ),
        },
        {
            "id": "max_concurrent_buffers",
            "label": "Max concurrent channel buffers",
            "type": "number",
            "default": 4,
            "help_text": "Safety cap -- each active buffer is a real ffmpeg process plus continuous disk writes.",
        },
        {
            "id": "internal_base_url",
            "label": "Internal base URL",
            "type": "string",
            "default": "http://127.0.0.1:9191",
            "help_text": (
                "How the plugin reaches Dispatcharr's own live proxy from "
                "inside the container. The default works for a standard "
                "docker-compose setup; if your deployment routes the web "
                "service differently (e.g. through a Unix socket behind "
                "nginx rather than a plain TCP port), adjust this to match."
            ),
        },
        {
            "id": "http_port",
            "label": "Buffer server port",
            "type": "number",
            "default": 9192,
            "help_text": (
                "Port this plugin's own file server listens on (playlists "
                "and segments are served directly by the plugin, not "
                "through Dispatcharr's normal web port -- confirmed live "
                "that Dispatcharr's /media/ static route is unreachable in "
                "this deployment mode, see plugin.py's module docstring). "
                "Every request needs a per-buffer access token, issued only "
                "via the authenticated start_buffer action -- reachable "
                "doesn't mean readable without one. Still must be mapped "
                "through your container config the same way 9191 already "
                "is, or clients outside the container can't reach it."
            ),
        },
        {
            "id": "test_channel_uuid",
            "label": "Test channel UUID",
            "type": "string",
            "default": "",
            "help_text": (
                "Only used by the manual-test buttons below (plugin action "
                "buttons can't take click-time input) -- paste a channel's "
                "UUID here, save, then use Start/Stop Test Buffer. The real "
                "integration (a client calling run/ over the REST API) "
                "passes channel_uuid directly and ignores this field."
            ),
        },
    ]

    actions = [
        {
            "id": "start_buffer",
            "label": "Start Buffer (manual test)",
            "description": "Starts a rolling buffer for a channel. Params: channel_uuid (required).",
            "button_label": "Start Test Buffer",
        },
        {
            "id": "stop_buffer",
            "label": "Stop Buffer",
            "description": "Stops a channel's buffer and removes its segment files. Params: channel_uuid (required).",
            "button_label": "Stop Test Buffer",
            "confirm": {
                "required": True,
                "title": "Stop buffer?",
                "message": "This ends the rolling buffer for the given channel and deletes its segment files.",
            },
        },
        {
            "id": "heartbeat",
            "label": "Heartbeat",
            "description": (
                "Refreshes a channel's idle timeout. Params: channel_uuid (required), viewer_id "
                "(optional). The buffer-wide timeout is refreshed by any file fetch regardless -- "
                "pass viewer_id to also refresh that specific viewer's own last-seen time, which is "
                "what lets stop_buffer tell a still-watching viewer apart from one that crashed "
                "without ever calling stop_buffer (see plugin.py's _prune_stale_viewers). A client "
                "with viewer_id lifecycle (start_buffer/stop_buffer) should call this on an interval "
                "well under idle_timeout_seconds."
            ),
        },
        {
            "id": "get_live_manifest",
            "label": "Get Live Manifest (manual test)",
            "description": (
                "Returns a byte-addressable manifest (segment filenames, byte sizes, durations, "
                "cumulative offsets) of the buffer's currently-listed segments (params: channel_uuid, "
                "required -- start_buffer must already be running). Used by pvr.dispatcharr-unofficial to treat "
                "the rolling live buffer as one growing, seekable byte stream via Range reads against "
                "individual segments, instead of routing through inputstream.ffmpegdirect's HLS-seek "
                "path (confirmed broken for this kind of buffer, see docs/TIMESHIFT.md)."
            ),
            "button_label": "Get Test Manifest",
        },
        {
            "id": "list_buffers",
            "label": "List Active Buffers",
            "description": (
                "Shows every currently-running buffer and its age, as a "
                "one-line summary in the result notification (Dispatcharr's "
                "own Plugins page has no other way to display an action's "
                "result data)."
            ),
            "button_label": "Refresh List",
        },
        {
            "id": "stop_all",
            "label": "Stop All Buffers",
            "description": "Emergency cleanup: stops every active buffer and removes all segment files.",
            "button_label": "Stop Everything",
            "button_variant": "filled",
            "button_color": "red",
            "confirm": {
                "required": True,
                "title": "Stop all buffers?",
                "message": "This ends every active rolling buffer right now, for every channel and "
                "every viewer currently using one.",
            },
        },
        {
            "id": "scrub_orphaned_buffers",
            "label": "Scrub Orphaned Buffer Directories",
            "description": (
                "Removes leftover directories under storage_path that Redis no longer has any "
                "record of (a client killed hard enough that it never sent stop_buffer, and no "
                "heartbeat arrived again before the tracked state's own TTL expired, is the usual "
                "cause) -- the reaper above already does this automatically on every tick, so this "
                "is mainly for cleaning up right now rather than waiting for the next one. Only "
                "touches directories untouched for several minutes; anything that could still be "
                "an actively-starting buffer is left alone."
            ),
            "button_label": "Scrub Now",
            "confirm": {
                "required": True,
                "title": "Scrub orphaned directories?",
                "message": "Permanently deletes any buffer directory under storage_path with no "
                "matching tracked state and no recent activity. Does not touch anything currently "
                "active.",
            },
        },
    ]

    def run(self, action: str, params: dict, context: dict):
        settings_dict = context.get("settings", {})
        logger = context.get("logger")

        storage_path = _str_setting(settings_dict, "storage_path", "/data/timeshift")
        # Best-effort only -- a storage_path that can't be created (permission
        # denied, a read-only mount, or a path whose leaf already exists as a
        # plain file, which raises FileExistsError even with exist_ok=True)
        # used to raise here unguarded, before action dispatch and before
        # _ensure_reaper_running() ever got a chance to start a fresh worker's
        # reaper -- failing every action, including stop_all/stop_buffer/
        # list_buffers, none of which actually need storage_path to exist.
        # Same bug class as the earlier _int_setting()/_str_setting() fixes:
        # a bad setting disabling exactly the actions needed to recover. The
        # HTTP server binding itself doesn't need this directory either (it
        # resolves a path under storage_path per request, not at bind time),
        # and _start_ffmpeg() already creates the full channel-directory tree
        # (parents=True) when a buffer actually starts, so this was always
        # redundant for the one path that genuinely needs it.
        try:
            Path(storage_path).mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            if logger:
                logger.warning("timeshift_buffer: couldn't create storage_path %s: %s", storage_path, exc)
        _stop_orphaned_threads(logger)
        _ensure_http_server_running(
            storage_path, _int_setting(settings_dict, "http_port", 9192, minimum=1, maximum=65535), logger
        )
        _ensure_reaper_running(settings_dict, logger)

        if action == "start_buffer":
            return self._start_buffer(params, settings_dict, logger)
        if action == "stop_buffer":
            return self._stop_buffer(params, settings_dict, logger)
        if action == "heartbeat":
            return self._heartbeat(params, settings_dict, logger)
        if action == "get_live_manifest":
            return self._get_live_manifest_action(params, settings_dict, logger)
        if action == "list_buffers":
            return self._list_buffers()
        if action == "stop_all":
            return self._stop_all(logger)
        if action == "scrub_orphaned_buffers":
            return self._scrub_orphaned_buffers(settings_dict, logger)

        return {"status": "error", "message": f"Unknown action: {action}"}

    def stop(self, context: dict):
        """Called when the plugin is disabled, deleted, or reloaded."""
        logger = context.get("logger")
        if _reaper_stop_event is not None:
            _reaper_stop_event.set()
        # Real, confirmed bug found via a project-wide review (a 41st-pass
        # audit, 2026-09-27), fixed here, not reproduced live: Dispatcharr's
        # own PluginReloadAPIView (apps/plugins/api_views.py) calls
        # stop_all_plugins(reason="reload") -- fired by the Plugins page's
        # own "Reload" button AND its "refresh all repos" button, neither
        # of which implies any intent to touch this plugin's own buffers --
        # before disable() sets reason="disable" and delete() sets
        # reason="delete" (PluginManager.stop_plugin(),
        # apps/plugins/loader.py). Tearing down every live buffer on a
        # plain reload (this addon's own get_live_manifest then reports
        # fatal: true, ending playback server-wide for every viewer with
        # server-side timeshift active) was never actually intended by
        # this stop() -- only disabling or deleting the plugin should ever
        # destroy real, running buffers and their Redis state.
        #
        # Checks for "reload" specifically rather than gating on
        # "disable"/"delete" the other way around: every real production
        # call site always passes an explicit reason (confirmed against
        # Dispatcharr's own real current upstream source -- only its own
        # unit tests ever call stop_plugin() with a reason this plugin
        # doesn't otherwise recognize, e.g. "shutdown"), but stop() may
        # still be invoked with no reason at all by something this plugin
        # doesn't control -- defaulting an unrecognized/missing reason to
        # the full, safe teardown (this function's own prior behavior)
        # rather than silently skipping it is the conservative choice.
        reason = context.get("reason")
        try:
            if reason != "reload":
                self._stop_all(logger)
                # Also scrub anything already-orphaned at the moment of
                # teardown -- _stop_all() above only touches what's still
                # Redis-tracked, so without this a disable/delete would leave
                # existing orphans behind rather than actually cleaning
                # storage_path out.
                self._scrub_orphaned_buffers(context.get("settings", {}), logger)
        finally:
            # Always, also when Redis is unreachable and the teardown above raised: the listener of a plugin that was
            # just disabled or deleted must not stay bound in this worker until Dispatcharr restarts.
            _stop_http_server(logger)

    # -- action implementations --------------------------------------------
    #
    # Dispatcharr's plugin action buttons (per Plugins.md) don't support
    # entering a parameter at click-time -- clicking one just calls
    # run(action, {}, context) with params empty. That's fine for the real
    # integration (a REST caller supplies params directly), but it means a
    # human manually testing via the Plugins page has no way to type in a
    # channel_uuid before pressing "Start Test Buffer". _resolve_channel_uuid
    # falls back to the test_channel_uuid setting field for that case, so
    # manual testing is: paste a UUID into that field, save settings, then
    # use the buttons (which will act on whatever's currently saved there).

    @staticmethod
    def _resolve_channel_uuid(params, settings_dict):
        raw = params.get("channel_uuid") or settings_dict.get("test_channel_uuid")
        if not raw:
            return None
        # Rejects anything that isn't a real UUID before it ever reaches
        # _channel_dir() -- every action below treats a None return here
        # identically to a missing channel_uuid (their existing "channel_uuid
        # is required" check), so a caller-supplied value like
        # "../recordings" is refused with the same ordinary error response
        # rather than being used to build a filesystem path. Dispatcharr's
        # own channel uuid field is a real UUID, so a non-UUID-shaped value
        # has no legitimate reason to reach here at all.
        try:
            # str(uuid.UUID(...)), not the caller-supplied `raw` itself:
            # uuid.UUID() is deliberately lenient (accepts uppercase,
            # {braced}, urn:uuid:-prefixed, and hyphen-less forms, same
            # looseness _is_canonical_uuid() -- plugin.py's own reaper
            # check -- was tightened against). Returning `raw` verbatim
            # meant the same real channel could resolve to a different
            # _channel_dir() path / Redis key depending on which
            # equivalent-but-differently-formatted UUID string happened
            # to be supplied, fragmenting one channel's buffer state
            # across multiple, independently max_concurrent_buffers-
            # counted "channels". The real addon always sends canonical
            # lowercase, so this is hardening (a manual test via the
            # test_channel_uuid setting field is the only realistic way
            # to supply a non-canonical form) rather than a live-
            # triggered bug -- found via a project-wide review.
            #
            # .strip() first: a value pasted into the test_channel_uuid
            # setting field routinely carries a trailing newline or space,
            # which uuid.UUID() rejects, and the caller then reported it as
            # "channel_uuid is required" -- a message that points away from
            # the real problem. A whitespace-only value strips to empty and
            # is refused the same way.
            return str(uuid.UUID(str(raw).strip()))
        except (ValueError, AttributeError, TypeError):
            return None

    @staticmethod
    def _manual_test_hint(params, http_port, playlist_route, access_token):
        """Where to fetch the playlist for the Plugins page's "Start Test Buffer" button, which is
        the only caller with empty params (it falls back to test_channel_uuid): the file server
        answers 403 to any request without the buffer's own token, and the page only shows the
        message, so without this a person following the README's manual check always got a 403 and
        concluded the plugin was broken. An API caller (the addon) supplies channel_uuid itself, gets
        the token as a field already, and its message is logged, so it never carries the token."""
        if params.get("channel_uuid") or not access_token:
            return ""
        return f" -- test with http://<dispatcharr-host>:{http_port}{playlist_route}?token={access_token}"

    def _start_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (pass it as a param, or paste one into "
                "the test_channel_uuid setting for manual testing)",
            }

        # Per-channel lock around the whole classify-then-spawn sequence
        # in _start_buffer_locked() below -- see
        # _acquire_start_buffer_lock()'s own comment for the
        # real, live-confirmed race this closes (two near-simultaneous
        # callers each spawning their own ffmpeg process, one left
        # permanently orphaned). Fails fast with the same `retryable`
        # convention the "stopping" case below already uses, rather than
        # blocking -- the lock's own short TTL already guarantees
        # whichever caller loses only waits briefly either way, and a
        # blocking wait here would risk this request itself timing out
        # instead of a quick, clean "try again shortly".
        lock_token = _acquire_start_buffer_lock(channel_uuid)
        if lock_token is None:
            return {
                "status": "error",
                "retryable": True,
                "message": "Another start_buffer call for this channel is already in progress -- retry in a moment",
            }
        try:
            return self._start_buffer_locked(channel_uuid, params, settings_dict, logger)
        finally:
            _release_start_buffer_lock(channel_uuid, lock_token)

    def _start_buffer_locked(self, channel_uuid, params, settings_dict, logger):
        """The actual classify-then-spawn logic, unchanged from before
        the locking fix above -- split out specifically so it stays
        directly unit-testable (existing tests call it via
        Plugin.run()'s own start_buffer dispatch, same as before; the
        lock itself is exercised by its own dedicated tests instead of
        needing every one of these to also mock Redis lock acquisition).
        Only ever called while _start_buffer() above already holds this
        channel's own lock -- never call this directly outside that."""
        # Registers this caller as one of the buffer's viewers (a plain
        # list, not a set -- state is round-tripped through Redis as JSON,
        # which has no native set type). Optional and best-effort: a caller
        # that doesn't pass one (an older addon version, or a manual click
        # of this plugin's own "Start Test Buffer" button, which calls
        # run() with empty params) just doesn't participate in reference
        # counting -- see _stop_buffer()'s own comment for exactly what
        # that degrades to.
        viewer_id = _resolve_viewer_id(params)

        existing = _get_buffer_state(channel_uuid)
        is_alive = _is_process_alive(existing.get("pid"), existing.get("pid_start_ticks")) if existing else False
        # Read here (not just inside _start_ffmpeg(), which only runs on a
        # genuinely fresh start) so a config-mismatch reattach can be
        # detected before deciding whether to reuse this buffer at all --
        # see _classify_existing_buffer()'s own "stale_config" comment.
        current_http_port = _int_setting(settings_dict, "http_port", 9192, minimum=1, maximum=65535)
        current_storage_path = _str_setting(settings_dict, "storage_path", "/data/timeshift")
        classification = _classify_existing_buffer(
            existing,
            is_alive,
            expected_http_port=current_http_port,
            expected_storage_path=current_storage_path,
        )

        # A teardown is already in flight for this channel (see
        # _classify_existing_buffer()'s own "stopping" case) -- neither
        # reattaching to it nor starting a fresh duplicate is safe while
        # that's happening. Ask the caller to retry shortly instead.
        # `retryable: True` (same structured-signal convention as
        # get_live_manifest's own `fatal`, rather than the addon having to
        # pattern-match this message's own text) is what lets the addon's
        # own OpenLiveTimeshiftStream() retry briefly instead of failing
        # outright -- a real gap found via a project-wide review: this
        # window is normally brief (bounded by _stop_ffmpeg's own ~2s
        # SIGTERM deadline plus file removal), but a caller opening the
        # same channel inside it used to get a hard, non-retried failure.
        if classification == "stopping":
            return {
                "status": "error",
                "retryable": True,
                "message": "A buffer for this channel is currently stopping -- retry in a moment",
            }

        # Confirmed live this check matters, not just theoretical: a
        # buffer whose ffmpeg already died (see _get_live_manifest()'s
        # own comment -- e.g. a provider-side concurrent-stream limit
        # refusing the connection) otherwise stayed "existing" forever.
        # Every future start_buffer for the same channel would keep
        # reattaching to it (refreshing last_heartbeat below), which
        # both kept reporting false success to callers and kept the
        # idle-timeout reaper from ever reaping a buffer that will
        # never produce anything -- a permanently zombied channel until
        # someone noticed and called stop_buffer by hand. Treat a dead
        # process exactly like "no buffer exists" instead: clean up its
        # stale state and fall through to a genuinely fresh start.
        if classification == "dead":
            logger.warning(
                "timeshift_buffer: start_buffer found a dead buffer for %s (pid %s no longer running) -- "
                "cleaning up and starting fresh instead of reattaching",
                channel_uuid,
                existing.get("pid"),
            )
            _remove_channel_files(existing, logger)
            _delete_buffer_state(channel_uuid)
            existing = None

        # This buffer's own http_port/storage_path no longer match current
        # settings (see _classify_existing_buffer()'s own "stale_config"
        # comment) -- unlike "dead" above, the process is still alive and
        # holding a real provider stream slot, so this needs an actual
        # teardown (stop ffmpeg, remove its old files, delete its state),
        # not just a state cleanup, before falling through to a fresh
        # start with the current config.
        if classification == "stale_config":
            logger.warning(
                "timeshift_buffer: start_buffer found a buffer for %s with stale config "
                "(port %s->%s, storage_path %s->%s) -- tearing down and restarting",
                channel_uuid,
                existing.get("http_port"),
                current_http_port,
                existing.get("storage_path"),
                current_storage_path,
            )
            _teardown_buffer(existing, logger, holds_start_lock=True)
            existing = None

        if existing:

            def attach(current):
                # Checked against the freshest state, atomically with the
                # write: a teardown that marked this buffer stopping after
                # the classification above must not gain a viewer (the
                # "stopping" marker used to be overwritable by a concurrent
                # write, reopening exactly the race that marker closed).
                if current.get("stopping"):
                    return None
                current["last_heartbeat"] = _shared_now()
                if viewer_id:
                    viewers = current.setdefault("viewers", [])
                    if viewer_id not in viewers:
                        viewers.append(viewer_id)
                    current.setdefault("viewer_heartbeats", {})[viewer_id] = _shared_now()
                # Retrofits a token onto state left behind by a plugin version
                # older than the access-token requirement (see
                # _check_access_token) -- makes this self-healing across an
                # upgrade instead of leaving a pre-existing buffer permanently
                # unreachable (nobody could ever produce a token matching
                # "none stored").
                if "access_token" not in current:
                    current["access_token"] = secrets.token_urlsafe(24)
                return current

            outcome, existing = _update_buffer_state(channel_uuid, attach)
            if outcome != "written":
                # Gone, stopping, or too contended to attach to since the
                # classification above -- neither reattaching nor starting a
                # duplicate is safe, so ask the caller to try again.
                return {
                    "status": "error",
                    "retryable": True,
                    "message": "The buffer for this channel changed while attaching -- retry in a moment",
                }
            return {
                "status": "ok",
                "message": f"Reattached to already-running buffer ({len(existing.get('viewers', []))} viewer(s))"
                + self._manual_test_hint(
                    params, existing["http_port"], existing["playlist_route"], existing.get("access_token")
                ),
                "http_port": existing["http_port"],
                "playlist_route": existing["playlist_route"],
                "already_running": True,
                "access_token": existing["access_token"],
            }

        # max_concurrent_buffers is a count over ALL channels, but the lock this call holds is per channel:
        # two starts for different channels each counted the same buffers, each saw room, and both spawned
        # (found by the 2026-10-04 sweep). One short global lock from the count to the state write makes the
        # count and the registration one step; the loser answers retryable like every other start-lock miss.
        slot_token = _acquire_start_buffer_lock(_START_SLOT_LOCK_ID)
        if slot_token is None:
            return {
                "status": "error",
                "retryable": True,
                "message": "Another buffer is being started right now -- retry in a moment",
            }
        try:
            max_concurrent = _int_setting(settings_dict, "max_concurrent_buffers", 4, minimum=1)
            if len(_list_buffer_keys()) >= max_concurrent:
                return {
                    "status": "error",
                    "message": f"Already at max_concurrent_buffers ({max_concurrent})",
                }

            try:
                state = _start_ffmpeg(channel_uuid, params, settings_dict, logger)
            except FileNotFoundError:
                return {"status": "error", "message": "ffmpeg not found in this container"}
            except Exception as exc:
                logger.exception("timeshift_buffer: failed to start buffer for %s", channel_uuid)
                return {"status": "error", "message": str(exc)}

            state["viewers"] = [viewer_id] if viewer_id else []
            state["viewer_heartbeats"] = {viewer_id: _shared_now()} if viewer_id else {}
            # Required by every request this buffer's own file server serves
            # from here on -- see _check_access_token's own comment for why.
            # token_urlsafe() output is already safe to place directly in a
            # URL query string (no escaping needed).
            state["access_token"] = secrets.token_urlsafe(24)
            _set_buffer_state(channel_uuid, state)
            return {
                "status": "ok",
                "message": f"Started a new buffer for channel {channel_uuid}"
                + self._manual_test_hint(params, state["http_port"], state["playlist_route"], state["access_token"]),
                "http_port": state["http_port"],
                "playlist_route": state["playlist_route"],
                "already_running": False,
                "access_token": state["access_token"],
            }
        finally:
            _release_start_buffer_lock(_START_SLOT_LOCK_ID, slot_token)

    def _stop_buffer(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        state = _get_buffer_state(channel_uuid)
        if not state:
            return {"status": "ok", "message": "no buffer was running"}

        # Reference-counted stop, not unconditional -- confirmed live this
        # matters: an earlier version of this addon called stop_buffer
        # unconditionally on every Close(), which killed a second viewer's
        # buffer the moment a first viewer also stopped watching (see
        # docs/TIMESHIFT.md's "Concurrent viewers" section in the addon
        # repo). A caller identifies itself via viewer_id (registered by
        # start_buffer above); this only removes *that* viewer from the
        # buffer's own tracked list; the underlying ffmpeg process is only
        # actually stopped once the list is empty. A caller with no
        # viewer_id (an older addon version, or this plugin's own manual
        # "Stop Test Buffer" button) can't be tracked at all, so it always
        # falls through to the unconditional stop below -- the same
        # behavior this action has always had for such callers, not a
        # regression, since there was never a way to reference-count them.
        viewer_id = _resolve_viewer_id(params)
        if viewer_id:
            # Drops any OTHER viewer_id that's gone stale (crashed without
            # ever calling stop_buffer) before deciding whether the buffer
            # is genuinely still in use -- see _prune_stale_viewers' own
            # comment. Without this, a single leftover phantom viewer_id
            # would keep this buffer (and the provider slot it holds)
            # alive forever after the last real viewer cleanly stops.
            idle_timeout = _int_setting(settings_dict, "idle_timeout_seconds", 30, minimum=1)

            def drop_viewer(current):
                current_viewers = current.get("viewers", [])
                if viewer_id in current_viewers:
                    current_viewers.remove(viewer_id)
                    current.get("viewer_heartbeats", {}).pop(viewer_id, None)
                _prune_stale_viewers(current, idle_timeout)
                return current

            outcome, state = _update_buffer_state(channel_uuid, drop_viewer)
            if outcome == "absent":
                return {"status": "ok", "message": "no buffer was running"}
            if outcome != "written":
                return {
                    "status": "error",
                    "retryable": True,
                    "message": "The buffer for this channel is busy -- retry in a moment",
                }
            viewers = state.get("viewers", [])
            if viewers:
                return {
                    "status": "ok",
                    "message": "viewer removed; buffer still active for other viewers",
                    "remaining_viewers": len(viewers),
                }
            # Last viewer gone -- but a start_buffer can attach a new one before
            # the stopping marker lands, so the teardown re-checks atomically.
            if _teardown_buffer(state, logger, abort_if=lambda fresh: bool(fresh.get("viewers"))) is False:
                return {
                    "status": "ok",
                    "message": "viewer removed; buffer still active for other viewers",
                }
            return {"status": "ok", "message": "Buffer stopped"}

        _teardown_buffer(state, logger)
        return {"status": "ok", "message": "Buffer stopped"}

    def _heartbeat(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        # An explicit per-viewer heartbeat (viewer_id passed alongside
        # channel_uuid) is what lets _prune_stale_viewers tell a genuinely
        # still-watching viewer apart from a crashed one -- see that
        # function's own comment. Only recorded for a viewer_id this
        # buffer (re-adding one _prune_stale_viewers dropped) --
        # see _apply_heartbeat()'s own comment for why this write-back
        # only ever touches the heartbeat/viewer fields, never the rest of
        # state (a pruned-but-still-watching viewer is re-added there).
        viewer_id = _resolve_viewer_id(params)
        outcome, _state = _update_buffer_state(
            channel_uuid, lambda current: _apply_heartbeat(current, _shared_now(), viewer_id)
        )
        if outcome == "absent":
            return {"status": "error", "message": "no buffer running for this channel"}
        if outcome != "written":
            return {"status": "error", "message": "could not record the heartbeat (buffer state busy) -- retry"}
        return {"status": "ok", "message": f"Heartbeat refreshed for channel {channel_uuid}"}

    def _get_live_manifest_action(self, params, settings_dict, logger):
        channel_uuid = self._resolve_channel_uuid(params, settings_dict)
        if not channel_uuid:
            return {
                "status": "error",
                "message": "channel_uuid is required (see test_channel_uuid setting for manual testing)",
            }

        # Real, confirmed bug this fixes (found live 2026-09-28, see
        # docs/OPEN_ITEMS.md): this call site used to only refresh the
        # buffer-wide last_heartbeat, never this specific viewer's own
        # viewer_heartbeats entry, even though get_live_manifest is the
        # *only* thing a client still calls while paused (via the
        # addon's own GetStreamTimes() polling -- ReadLiveTimeshiftStream(),
        # the only call site that sends an explicit heartbeat action, is
        # what a pause actually stops). A paused viewer's own per-viewer
        # heartbeat went stale after idle_timeout_seconds regardless, got
        # pruned by the reaper, and if every other viewer then stopped,
        # the buffer was torn down out from under the still-paused
        # viewer -- confirmed live end-to-end against a real Kodi client.
        viewer_id = _resolve_viewer_id(params)

        state = _get_buffer_state(channel_uuid)
        if not state:
            # By the time a caller is polling get_live_manifest at all, it
            # already went through a successful start_buffer, which writes
            # this same state synchronously before returning -- so its
            # absence here means the buffer was deliberately torn down
            # since (stop_buffer, the idle reaper, or this same function's
            # own fatal-error branch below on an earlier call), not a
            # startup race. Nothing will resurrect it on its own, so this
            # gets the same fatal: true treatment as a confirmed-dead
            # buffer below, rather than leaving a caller's retry loop to
            # burn its full budget against a buffer that no longer exists
            # at all.
            return {
                "status": "error",
                "fatal": True,
                "message": "no buffer running for this channel -- call start_buffer first",
            }

        # Deliberately no teardown here, unlike the dead-buffer branch
        # below: the buffer now tracked for this channel is a healthy one
        # belonging to a different viewer, not this caller's.
        if _is_stale_access_token(state, params.get("access_token")):
            return {
                "status": "error",
                "fatal": True,
                "message": (
                    "this caller's buffer was replaced by a newer one for the same channel -- "
                    "call start_buffer again"
                ),
            }

        try:
            manifest = _get_live_manifest(state, logger)
        except BufferFailedError as exc:
            # Caught ahead of the plain RuntimeError branch below (it's a
            # subclass) -- `fatal: true` is what lets a caller (this
            # addon's own OpenLiveTimeshiftStream() cold-start retry loop)
            # stop retrying immediately instead of waiting out its full
            # budget against a buffer that will never produce a segment.
            # Also self-heals here rather than waiting for the next
            # start_buffer call to notice (see that method's own comment):
            # nothing else will proactively clean this up otherwise, since
            # a dead buffer with no further fetches never goes idle either.
            _teardown_buffer(state, logger)
            return {"status": "error", "fatal": True, "message": str(exc)}
        except RuntimeError as exc:
            return {"status": "error", "message": str(exc)}
        except Exception as exc:
            logger.exception("timeshift_buffer: manifest build failed for %s", channel_uuid)
            return {"status": "error", "message": str(exc)}

        # Asking for the manifest is itself a sign this buffer is actively
        # being watched. Re-fetch a fresh copy of state right before
        # writing back, rather than reusing the copy read at the top of
        # this call: _get_live_manifest() above can mean up to ~1,800
        # stat() calls, long enough for a concurrent start_buffer to
        # register a new viewer in the meantime -- writing back the
        # stale, viewer-less copy would otherwise silently overwrite
        # that registration (a real, confirmed race; see
        # _apply_heartbeat()'s own comment). state itself (http_port
        # etc., used in the response below) is stable, set-once metadata
        # unaffected by this, so the original read is still fine to
        # answer the caller from.
        #
        # No `or state` fallback here on purpose -- a real, confirmed
        # gap: this buffer can just as easily have been legitimately torn
        # down (stop_buffer, the idle reaper, or this same function's own
        # fatal-error branch above) in that same window, in which case
        # _get_buffer_state() correctly returns None. Falling back to the
        # stale top-of-function `state` would resurrect that already-
        # deleted buffer's state in Redis with a freshly refreshed
        # heartbeat, undoing the teardown. The manifest itself was
        # already built successfully and is still returned to the caller
        # either way; only the write-back is skipped.
        _update_buffer_state(channel_uuid, lambda fresh_state: _apply_heartbeat(fresh_state, _shared_now(), viewer_id))

        return {
            "status": "ok",
            "message": (
                f"{len(manifest['segments'])} segment(s), {manifest['total_bytes']} bytes, "
                f"{manifest['total_duration_ms'] / 1000:.1f}s buffered"
                + (" (ffmpeg has exited -- no further segments will arrive)" if manifest.get("ended") else "")
            ),
            "http_port": state["http_port"],
            "channel_uuid": channel_uuid,
            "segment_route_prefix": f"/{channel_uuid}/",
            **manifest,
        }

    def _list_buffers(self):
        buffers = []
        now = _shared_now()
        for state in _iter_buffer_states():
            buffers.append(
                {
                    "channel_uuid": state["channel_uuid"],
                    "age_seconds": int(now - state.get("started_at", now)),
                    "idle_seconds": int(now - state.get("last_heartbeat", now)),
                    "http_port": state.get("http_port"),
                    "playlist_route": state.get("playlist_route"),
                    # Reference-counted viewers (see _start_buffer()/_stop_buffer()'s
                    # own comments) -- diagnostic only, not itself load-bearing for
                    # cleanup: a caller with no viewer_id is simply never counted
                    # here even while it's genuinely watching.
                    "viewers": len(state.get("viewers", [])),
                }
            )

        # A human-readable summary, not just the raw buffers list above --
        # Dispatcharr's own Plugins page shows an action's "message" field
        # directly in its result toast (confirmed by reading its frontend,
        # PluginCard.jsx's handlePluginRun()), which is the *only* place any
        # of this action's result is ever actually visible in that UI. The
        # raw "buffers" array above is still returned for a caller that
        # wants the real data (this addon's own diagnostics, or a direct
        # API call), just not something that UI can render on its own.
        return {"status": "ok", "message": _buffers_summary(buffers), "buffers": buffers}

    def _stop_all(self, logger):
        stopped = []
        for state in _iter_buffer_states():
            _teardown_buffer(state, logger)
            stopped.append(state["channel_uuid"])
        message = "No buffers were running" if not stopped else f"Stopped {len(stopped)} buffer(s)"
        return {"status": "ok", "message": message, "stopped": stopped}

    def _scrub_orphaned_buffers(self, settings_dict, logger):
        storage_path = _str_setting(settings_dict, "storage_path", "/data/timeshift")
        idle_timeout = _int_setting(settings_dict, "idle_timeout_seconds", 30, minimum=1)
        # Same floor as the reaper's own automatic pass -- see
        # _find_orphaned_channel_dirs' comment on why an orphan (no
        # tracked state to double-check against) gets more margin than
        # ordinary heartbeat-based reaping.
        removed = _scrub_orphaned_dirs(storage_path, max(idle_timeout, 300), logger)
        message = (
            "No orphaned directories found"
            if not removed
            else f"Removed {len(removed)} orphaned director{'y' if len(removed) == 1 else 'ies'}"
        )
        return {"status": "ok", "message": message, "removed": removed}
