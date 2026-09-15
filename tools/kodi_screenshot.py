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
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEFAULT_TIMEOUT_SECONDS = 10
CAPTURE_SETTLE_SECONDS = 1


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

    jsonrpc_call(url, headers, "Input.ExecuteAction", {"action": "screenshot"})
    time.sleep(CAPTURE_SETTLE_SECONDS)

    ssh = ssh_prefix(args.ssh_user, ssh_host, args.ssh_port)
    # `ls -t` (newest first) rather than trusting a filename pattern --
    # Kodi's own screenshot naming isn't part of any confirmed-live contract
    # here, just observed as screenshotNNNNN.png during development.
    find_newest = subprocess.run(
        [*ssh, f"ls -t {screenshots_dir} | head -1"],
        capture_output=True,
        text=True,
        timeout=DEFAULT_TIMEOUT_SECONDS,
    )
    newest_name = find_newest.stdout.strip()
    if find_newest.returncode != 0 or not newest_name:
        print(f"error: couldn't list {screenshots_dir} over SSH: {find_newest.stderr}", file=sys.stderr)
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
