#!/usr/bin/env bash
# One-time SSH-driven provisioning for a Kodi Flatpak test VM: deploys this
# addon's built zip, and enables the two things tools/kodi_smoke_test.py
# fundamentally cannot enable itself:
#   - the JSON-RPC webserver (the smoke-test script IS a JSON-RPC client, so
#     it can't be the thing that turns JSON-RPC on -- a bootstrap paradox)
#   - the addon's own debug_logging setting (Kodi's JSON-RPC API has no
#     generic "write an addon's setting" method, and docs/TIMESHIFT.md
#     already documents driving the AddonSettings dialog via JSON-RPC
#     synthetic input as confirmed broken live -- blank content under
#     screencapture, no response to Input.Down/Input.Select -- so this
#     doesn't even attempt GUI automation for it)
# Also enables Kodi's own core debug logging (debug.showloginfo) while it's
# in here, since that's a plain file edit too and useful for the same
# smoke-test pass.
#
# Deliberately NOT part of CI, same manual/live-hardware territory as
# everything else in docs/OPEN_ITEMS.md's "No automated test suite exists"
# entry -- this provisions a real VM you're about to smoke-test against.
#
# Kodi must be fully stopped while this runs: Kodi flushes its in-memory
# settings back to guisettings.xml on clean exit, which would silently
# clobber any edit made here if Kodi were still running when it later
# closes. This script checks for and refuses to run against a live Kodi
# session rather than risk that.
#
# Deliberately does NOT touch the addon's own Dispatcharr connection
# settings (server URL, credentials) -- those are personal/sensitive and
# left for you to enter once through Kodi's own GUI on the VM, not passed
# through this script or SSH.
#
# Deliberately does NOT attempt to restart Kodi's GUI remotely either --
# that's tied to whatever display/session manager the VM happens to be
# running, fragile to script reliably in general, and this is a one-time
# setup step where a manual restart from the VM's own desktop is a low
# enough cost not to chase.
#
# KNOWN LIMITATION, confirmed live (2026-09-14): editing guisettings.xml
# before Kodi has EVER been launched on this profile doesn't reliably
# stick -- Kodi's own first-ever-run initialization appears to fully
# regenerate the file rather than merge a sparse pre-existing one, even
# with no `default` attribute present. Confirmed by watching
# services.webserver revert to `default="true">false` despite this
# script having written a plain `true` beforehand. If this is a brand
# new Kodi profile (never launched before), start Kodi once manually
# first, quit it, THEN run this script -- editing an already-real
# settings file (the normal path once a VM has been used before) does
# stick, confirmed by re-applying the same edit afterward and having it
# survive a relaunch. Not yet automated here (would mean baking in the
# WAYLAND_DISPLAY launch fix below plus a way to detect "first run
# finished", both real complexity for what's still a one-time setup
# step per VM).
#
# Also see the separate WAYLAND_DISPLAY requirement for actually
# *launching* Kodi remotely over SSH afterward, which this script
# doesn't attempt (see its own header note above) -- a bare
# `flatpak run tv.kodi.Kodi &` over SSH with no display env vars set
# falls back to Kodi's standalone kodi-gbm path, invisible and unstable
# (EGL fence-sync errors) rather than attaching to the real desktop
# session. Needs, once Kodi's own session is confirmed active:
#   XDG_RUNTIME_DIR=/run/user/<uid> WAYLAND_DISPLAY=wayland-0 \
#     DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/<uid>/bus \
#     flatpak run tv.kodi.Kodi &
#
# Usage (key-based auth, the default):
#   tools/kodi_provision_linux_flatpak.sh --host 192.168.1.50 --user alice --zip /path/to/addon-pvr.dispatcharr-unofficial-*.zip
#   tools/kodi_provision_linux_flatpak.sh --host 192.168.1.50 --user alice --zip ... --port 2222
#
# Usage (password auth): export SSHPASS first rather than passing it as an
# argument -- a command-line arg is visible to other users on the same
# machine via `ps`, an env var isn't. Requires `sshpass` installed.
#   SSHPASS='...' tools/kodi_provision_linux_flatpak.sh --host 192.168.1.50 --user alice --zip ...

set -euo pipefail

ADDON_ID="pvr.dispatcharr-unofficial"
FLATPAK_APP_ID="tv.kodi.Kodi"
SSH_PORT=22

usage() {
  echo "Usage: $0 --host HOST --user USER --zip PATH_TO_ADDON_ZIP [--port PORT]" >&2
  exit 1
}

HOST=""
SSH_USER=""
ZIP_PATH=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --user) SSH_USER="$2"; shift 2 ;;
    --zip) ZIP_PATH="$2"; shift 2 ;;
    --port) SSH_PORT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

[[ -n "$HOST" && -n "$SSH_USER" && -n "$ZIP_PATH" ]] || usage
[[ -f "$ZIP_PATH" ]] || { echo "error: zip not found at $ZIP_PATH" >&2; exit 1; }

# StrictHostKeyChecking=accept-new unconditionally: a first-time interactive
# host-key prompt would otherwise hang forever under sshpass, which only
# ever answers a password prompt, not a host-key one.
SSH_OPTS=(-p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)

# shellcheck disable=SC2029 # every "$@" passed to ssh_run below is already
# a fully client-resolved string (paths/ids built from our own variables) --
# none of them reference a remote-side variable, so client-side expansion
# here is intentional, not a quoting bug.
if [[ -n "${SSHPASS:-}" ]]; then
  command -v sshpass >/dev/null || { echo "error: SSHPASS is set but sshpass isn't installed." >&2; exit 1; }
  ssh_run() { sshpass -e ssh "${SSH_OPTS[@]}" "${SSH_USER}@${HOST}" "$@"; }
  scp_run() { sshpass -e scp "${SCP_OPTS[@]}" "$@"; }
else
  # BatchMode=yes only makes sense for key-based auth -- it disables any
  # interactive password prompt entirely, which is exactly what we want
  # here (fail fast instead of hanging) when a key is what's expected.
  SSH_OPTS+=(-o BatchMode=yes)
  ssh_run() { ssh "${SSH_OPTS[@]}" "${SSH_USER}@${HOST}" "$@"; }
  scp_run() { scp "${SCP_OPTS[@]}" "$@"; }
fi

FLATPAK_DATA="\$HOME/.var/app/${FLATPAK_APP_ID}/data"
ADDON_DIR="${FLATPAK_DATA}/addons/${ADDON_ID}"
GUISETTINGS="${FLATPAK_DATA}/userdata/guisettings.xml"
ADDON_SETTINGS_DIR="${FLATPAK_DATA}/userdata/addon_data/${ADDON_ID}"
ADDON_SETTINGS="${ADDON_SETTINGS_DIR}/settings.xml"
SCREENSHOTS_DIR="${FLATPAK_DATA}/temp/screenshots"

echo "==> Checking connectivity to ${SSH_USER}@${HOST}:${SSH_PORT}..."
ssh_run true 2>/dev/null || {
  echo "error: SSH connection failed." >&2
  exit 1
}

echo "==> Checking Kodi isn't currently running (it must be fully stopped for these edits to stick)..."
if ssh_run "flatpak ps --columns=application 2>/dev/null | grep -qx '${FLATPAK_APP_ID}'"; then
  echo "error: Kodi (${FLATPAK_APP_ID}) is still running on ${HOST}." >&2
  echo "       Quit it from the VM's desktop first, then re-run this script -- Kodi" >&2
  echo "       flushes guisettings.xml on exit and would overwrite these edits otherwise." >&2
  exit 1
fi

echo "==> Checking unzip is available on ${HOST}..."
ssh_run "command -v unzip >/dev/null" || {
  echo "error: unzip not found on ${HOST} -- install it there first (apt install unzip)." >&2
  exit 1
}

REMOTE_TMP_ZIP="/tmp/$(basename "$ZIP_PATH")"
echo "==> Copying $(basename "$ZIP_PATH") to ${HOST}:${REMOTE_TMP_ZIP}..."
scp_run "$ZIP_PATH" "${SSH_USER}@${HOST}:${REMOTE_TMP_ZIP}"

echo "==> Deploying the addon into Flatpak Kodi's addon directory..."
ssh_run bash -s -- "$ADDON_DIR" "$REMOTE_TMP_ZIP" <<'REMOTE_DEPLOY'
set -euo pipefail
addon_dir="$1"
zip_path="$2"
mkdir -p "$(dirname "$addon_dir")"
rm -rf "$addon_dir"
unzip -q "$zip_path" -d "$(dirname "$addon_dir")"
rm -f "$zip_path"
REMOTE_DEPLOY

echo "==> Creating the screenshots directory (Input.ExecuteAction screenshot needs this to already exist -- confirmed live 2026-09-14: setting debug.screenshotpath to a not-yet-existing directory pops Kodi's own folder-browser dialog as a validation prompt instead of just saving there)..."
ssh_run mkdir -p "$SCREENSHOTS_DIR"

echo "==> Ensuring the JSON-RPC webserver, Kodi's core debug logging, and a screenshot destination are enabled in guisettings.xml..."
ssh_run python3 - "$GUISETTINGS" "$SCREENSHOTS_DIR" <<'REMOTE_GUISETTINGS'
import os
import sys
import xml.etree.ElementTree as ET

path = os.path.expandvars(sys.argv[1])
screenshots_dir = os.path.expandvars(sys.argv[2])
os.makedirs(os.path.dirname(path), exist_ok=True)

if os.path.exists(path):
    tree = ET.parse(path)
    root = tree.getroot()
else:
    root = ET.Element("settings", version="2")
    tree = ET.ElementTree(root)

desired = {
    "services.webserver": "true",
    "debug.showloginfo": "true",
    "debug.screenshotpath": screenshots_dir,
}
for setting_id, value in desired.items():
    node = root.find(f"./setting[@id='{setting_id}']")
    if node is None:
        node = ET.SubElement(root, "setting", id=setting_id)
    node.text = value
    # Confirmed live (2026-09-14): a node still carrying default="true"
    # can have its stored text silently ignored by Kodi in favor of the
    # real built-in default (observed: services.webserver came back
    # default="true">false on next launch despite this script having
    # written a plain "true" with no default attribute originally
    # present -- but re-applying the same edit against an already-real,
    # post-first-run settings file with the attribute explicitly cleared
    # did stick). Clearing it here matches what Kodi itself does once a
    # setting is genuinely changed via its own GUI.
    if "default" in node.attrib:
        del node.attrib["default"]

tree.write(path, encoding="utf-8", xml_declaration=True)
print(f"guisettings.xml updated: {desired}")
REMOTE_GUISETTINGS

echo "==> Ensuring the addon's own debug_logging setting is enabled..."
ssh_run python3 - "$ADDON_SETTINGS" <<'REMOTE_ADDON_SETTINGS'
import os
import sys
import xml.etree.ElementTree as ET

path = os.path.expandvars(sys.argv[1])
os.makedirs(os.path.dirname(path), exist_ok=True)

if os.path.exists(path):
    tree = ET.parse(path)
    root = tree.getroot()
else:
    root = ET.Element("settings", version="2")
    tree = ET.ElementTree(root)

node = root.find("./setting[@id='debug_logging']")
if node is None:
    node = ET.SubElement(root, "setting", id="debug_logging")
node.text = "true"

tree.write(path, encoding="utf-8", xml_declaration=True)
print("addon settings.xml updated: {'debug_logging': 'true'}")
REMOTE_ADDON_SETTINGS

cat <<EOF

==> Done. Remaining manual steps on ${HOST}'s own desktop:
    1. Start Kodi (${FLATPAK_APP_ID}) from the VM's GUI (or, once its own
       session is confirmed active, remotely with the WAYLAND_DISPLAY
       env vars noted in this script's own header comment).
    2. Open this addon's own settings and enter your real Dispatcharr
       server URL/credentials -- deliberately not handled by this script.
    3. Confirm Settings -> Services -> Control shows "Allow remote control
       via HTTP" already checked (this script enabled it on disk; Kodi
       will pick it up on this fresh start).

If this is a brand new Kodi profile that has never been launched before
and step 3 above turns out NOT checked, see this script's own
"KNOWN LIMITATION" header comment -- re-run this script once after
Kodi's first real launch.

Once Kodi is up and connected, run:
    python3 tools/kodi_smoke_test.py --host ${HOST}
    python3 tools/kodi_screenshot.py --host ${HOST} --ssh-user ${SSH_USER}
EOF
