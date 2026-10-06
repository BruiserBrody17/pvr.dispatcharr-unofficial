#!/usr/bin/env python3
"""Captures a screenshot from a running Kodi instance for visual debugging --
e.g. reading the actual text of a DialogConfirm.xml/AddonSettings-style modal
that's blocking JSON-RPC, rather than inferring it from kodi.log alone (see
tools/kodi_smoke_test.py's own module docstring for the class of Kodi-core
dialog this is meant to diagnose).

Deliberately NOT part of CI, same manual/live-hardware territory as
tools/kodi_smoke_test.py and tools/kodi_provision_linux_flatpak.sh.

Requires SSH access to the same host, separate from Kodi's own JSON-RPC
credentials -- Kodi's JSON-RPC API has no way to transfer the resulting file
back itself (Files.PrepareDownload's /vfs/ HTTP download endpoint returned a
plain 401 against a real device on 2026-09-14 regardless of path encoding
tried; not investigated further given SSH access is already required for
tools/kodi_provision_linux_flatpak.sh in this same workflow, so it isn't a new dependency).

Requires tools/kodi_provision_linux_flatpak.sh (or an equivalent manual setup) to have
already set debug.screenshotpath to an existing directory -- this script
errors out clearly rather than guessing a path if that setting is empty.

Usage (key-based SSH, the default):
    python3 tools/kodi_screenshot.py --host 192.168.1.50 --ssh-user alice
        [--username kodi] [--password ...] [--out screenshot.png]

Usage (password-based SSH): export SSHPASS first, same convention as
tools/kodi_provision_linux_flatpak.sh.
    SSHPASS='...' python3 tools/kodi_screenshot.py --host 192.168.1.50 --ssh-user alice
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEFAULT_TIMEOUT_SECONDS = 10
# Input.ExecuteAction's own RPC call returns as soon as Kodi's main thread
# has *queued* the action (CInputOperations::SendAction()'s own
# waitResult=false default, confirmed against Kodi's real current SDK
# source, PostMsg() not SendMsg()) -- not once the screenshot has actually
# been taken, let alone written to disk. CScreenShot::TakeScreenshot(file,
# false) (xbmc/utils/Screenshot.cpp) then creates a zero-byte placeholder
# file immediately (synchronously, "to avoid concurrency issues" per its
# own comment) and writes the real PNG content afterward, asynchronously,
# via a background CThumbnailWriter job. A single fixed sleep before
# copying the "newest" file (found via a 38th-pass audit, not itself
# independently reproduced) could therefore copy back either a stale,
# previous screenshot or this cycle's own not-yet-written placeholder --
# and still report success either way. Polling for a *new* filename whose
# size has stopped changing (POLL_INTERVAL_SECONDS apart, twice in a row)
# is what actually confirms the PNG is both new and fully written.
POLL_INTERVAL_SECONDS = 1
POLL_TIMEOUT_SECONDS = 15


class JsonRpcError(RuntimeError):
    pass


def jsonrpc_call(url: str, headers: dict, method: str, params: dict | None = None):
    body = json.dumps({"jsonrpc": "2.0", "method": method, "params": params or {}, "id": 1}).encode()
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=DEFAULT_TIMEOUT_SECONDS) as resp:
            payload = json.loads(resp.read())
    except (urllib.error.URLError, TimeoutError) as exc:
        raise JsonRpcError(f"{method}: {exc}") from exc
    if "error" in payload:
        raise JsonRpcError(f"{method}: {payload['error']}")
    return payload.get("result")


def ssh_prefix(ssh_user: str, ssh_host: str, ssh_port: int) -> list:
    opts = ["-p", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    if os.environ.get("SSHPASS"):
        return ["sshpass", "-e", "ssh", *opts, f"{ssh_user}@{ssh_host}"]
    return ["ssh", *opts, "-o", "BatchMode=yes", f"{ssh_user}@{ssh_host}"]


def scp_prefix(ssh_port: int) -> list:
    opts = ["-P", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    if os.environ.get("SSHPASS"):
        return ["sshpass", "-e", "scp", *opts]
    return ["scp", *opts, "-o", "BatchMode=yes"]


def list_remote_files(ssh: list, screenshots_dir: str) -> dict[str, int] | None:
    """Returns {filename: size in bytes} for every regular file directly
    under screenshots_dir, or None if the SSH command itself failed.

    Uses `find -printf` (GNU find, already assumed by this whole
    SSH-based-Linux-host workflow -- see tools/kodi_provision_linux_flatpak.sh)
    rather than parsing `ls` output, which has no reliable way to also
    report each file's size in one round trip without ambiguity for a
    filename containing spaces. screenshots_dir is shell-quoted -- the
    original `ls -t {screenshots_dir}` (unquoted) broke on a path
    containing a space, found via a 38th-pass audit, not itself
    independently reproduced.
    """
    result = subprocess.run(
        [*ssh, f"find {shlex.quote(screenshots_dir)} -maxdepth 1 -type f -printf '%s %f\\n'"],
        capture_output=True,
        text=True,
        timeout=DEFAULT_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        return None
    files: dict[str, int] = {}
    for line in result.stdout.splitlines():
        size_str, _, name = line.partition(" ")
        try:
            files[name] = int(size_str)
        except ValueError:
            continue
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--host", required=True, help="Kodi's JSON-RPC host (also the SSH host unless --ssh-host is given)"
    )
    parser.add_argument("--port", type=int, default=8080, help="Kodi's JSON-RPC webserver port")
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--ssh-user", required=True)
    parser.add_argument("--ssh-host", help="defaults to --host")
    parser.add_argument("--ssh-port", type=int, default=22)
    parser.add_argument("--out", default=f"kodi_screenshot_{int(time.time())}.png")
    args = parser.parse_args()

    ssh_host = args.ssh_host or args.host
    url = f"http://{args.host}:{args.port}/jsonrpc"
    headers = {"Content-Type": "application/json"}
    if args.username:
        token = base64.b64encode(f"{args.username}:{args.password or ''}".encode()).decode()
        headers["Authorization"] = f"Basic {token}"

    result = jsonrpc_call(url, headers, "Settings.GetSettingValue", {"setting": "debug.screenshotpath"})
    screenshots_dir = result.get("value") if result else ""
    if not screenshots_dir:
        print(
            "error: Kodi's debug.screenshotpath setting is empty -- run tools/kodi_provision_linux_flatpak.sh "
            "first (it sets this up), or set it manually via Kodi's own settings.",
            file=sys.stderr,
        )
        return 1

    ssh = ssh_prefix(args.ssh_user, ssh_host, args.ssh_port)
    before = list_remote_files(ssh, screenshots_dir)
    if before is None:
        print(f"error: couldn't list {screenshots_dir} over SSH", file=sys.stderr)
        return 1

    jsonrpc_call(url, headers, "Input.ExecuteAction", {"action": "screenshot"})

    # Poll for a *new* filename (not present in `before`) whose size has
    # stopped changing across two consecutive polls -- see
    # POLL_INTERVAL_SECONDS's own comment for why a single fixed sleep
    # isn't enough. `candidate`/`candidate_size` track the newest
    # not-previously-seen name across iterations; it's only accepted once
    # its size is unchanged (and non-zero) from the previous poll.
    deadline = time.time() + POLL_TIMEOUT_SECONDS
    candidate: str | None = None
    candidate_size = -1
    newest_name: str | None = None
    while time.time() < deadline:
        time.sleep(POLL_INTERVAL_SECONDS)
        after = list_remote_files(ssh, screenshots_dir)
        if after is None:
            continue
        new_names = [name for name in after if name not in before]
        if not new_names:
            continue
        # If several new files appeared (something else is also writing
        # to this directory), the largest by size is the best guess at
        # "the screenshot", since a placeholder still sitting at 0 bytes
        # loses to anything actually written.
        this_round = max(new_names, key=lambda name: after[name])
        this_size = after[this_round]
        if this_round == candidate and this_size == candidate_size and this_size > 0:
            newest_name = this_round
            break
        candidate, candidate_size = this_round, this_size

    if newest_name is None:
        print(
            f"error: no new, fully-written screenshot appeared in {screenshots_dir} within "
            f"{POLL_TIMEOUT_SECONDS}s (screensaver swallowing the action, or a slow write?)",
            file=sys.stderr,
        )
        return 1

    scp = scp_prefix(args.ssh_port)
    remote_path = f"{screenshots_dir.rstrip('/')}/{newest_name}"
    copy = subprocess.run(
        [*scp, f"{args.ssh_user}@{ssh_host}:{remote_path}", args.out],
        capture_output=True,
        text=True,
        timeout=DEFAULT_TIMEOUT_SECONDS,
    )
    if copy.returncode != 0:
        print(f"error: scp failed: {copy.stderr}", file=sys.stderr)
        return 1

    print(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
