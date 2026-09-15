#!/usr/bin/env bash
# One-time SSH-driven provisioning for a native Windows Kodi test VM: deploys
# this addon's built zip, and enables the two things
# tools/kodi_smoke_test.py fundamentally cannot enable itself:
#   - the JSON-RPC webserver (the smoke-test script IS a JSON-RPC client, so
#     it can't be the thing that turns JSON-RPC on -- a bootstrap paradox)
#   - the addon's own debug_logging setting (Kodi's JSON-RPC API has no
#     generic "write an addon's setting" method, and docs/TIMESHIFT.md
#     already documents driving the AddonSettings dialog via JSON-RPC
#     synthetic input as confirmed broken live -- blank content under
#     screencapture, no response to Input.Down/Input.Select -- so this
#     doesn't even attempt GUI automation for it)
# Also enables Kodi's own core debug logging (debug.showloginfo) and
# registers a Scheduled Task that can launch Kodi into the real interactive
# desktop session afterward (see the Session-0 note below) -- useful for the
# same smoke-test pass.
#
# The Linux/Flatpak counterpart to this script is
# tools/kodi_provision_linux_flatpak.sh -- same purpose, same SSH-driven
# shape, different remote OS underneath. tools/kodi_smoke_test.py itself is
# unchanged either way: it only speaks Kodi's own JSON-RPC API.
#
# Deliberately NOT part of CI, same manual/live-hardware territory as
# everything else in docs/OPEN_ITEMS.md's "No automated test suite exists"
# entry -- this provisions a real VM you're about to smoke-test against.
#
# Requires OpenSSH Server enabled on the Windows VM (an optional Windows
# feature, not installed by default) -- see this project's own notes on
# setting that up. Every remote command here runs via `powershell.exe`
# explicitly (never relying on cmd.exe being replaced as the SSH default
# shell), so it works regardless of whatever the account's own default
# shell is set to.
#
# Kodi must be fully stopped while this runs: Kodi flushes its in-memory
# settings back to guisettings.xml on clean exit (core Kodi behavior, not
# platform-specific), which would silently clobber any edit made here if
# Kodi were still running when it later closes. This script checks for and
# refuses to run against a live Kodi process rather than risk that.
#
# Deliberately does NOT touch the addon's own Dispatcharr connection
# settings (server URL, credentials) -- those are personal/sensitive and
# left for you to enter once through Kodi's own GUI on the VM, not passed
# through this script or SSH.
#
# KNOWN LIMITATION, same root cause already documented in
# tools/kodi_provision_linux_flatpak.sh's own header for the Linux/Flatpak
# case: editing guisettings.xml before Kodi has EVER been launched on this
# profile doesn't reliably stick -- Kodi's own first-ever-run
# initialization appears to fully regenerate the file rather than merge a
# sparse pre-existing one. If %APPDATA%\Kodi\userdata\guisettings.xml
# doesn't exist yet, launch Kodi once first (see the Scheduled Task note
# below), quit it, THEN run this script.
#
# Confirmed live (2026-09-15): a GUI process started directly from an SSH
# session lands in Windows' non-interactive Session 0 (the same class of
# problem Linux's Flatpak Kodi hits without WAYLAND_DISPLAY set -- see that
# script's own header) -- it runs, but is invisible and can't be screenshotted
# from the real desktop. The fix isn't an env var here, it's a Scheduled
# Task: this script registers one named "LaunchKodiInteractive" (via
# New-ScheduledTaskPrincipal -LogonType Interactive), which runs in
# whichever session is actually logged into the console. Once registered
# (a one-time setup step, safe to re-run), launch or relaunch Kodi with:
#   ssh <user>@<host> schtasks /run /tn LaunchKodiInteractive
# This requires an actual interactive console session already logged in
# on the VM (e.g. via Proxmox's own console viewer) -- confirmed live this
# does NOT work against a session sitting at the lock/login screen with no
# one signed in.
#
# KNOWN LIMITATION, confirmed live (2026-09-15) and genuinely confusing to
# diagnose the first time: an idle Windows session can lock itself (its own
# screensaver/inactivity-lock policy) purely from a long stretch of
# SSH-only work with no real keyboard/mouse activity -- and a locked
# session blocks Kodi's DirectX exclusive-fullscreen rendering path from
# ever finishing initialization, so Kodi's whole startup silently stalls
# indefinitely (no crash, no error -- it just never reaches the point of
# opening the JSON-RPC webserver port). `Get-Process -Name LogonUI` having
# any result is the definitive live signal the session is locked (query
# user's own STATE field doesn't reliably show this). Pass
# --enable-autologon to have this script configure Windows Autologon (so a
# reboot logs back in on its own) plus disable the screensaver and
# display/system sleep (so it doesn't re-lock from inactivity either) --
# genuinely necessary for a VM you intend to drive purely over SSH for any
# length of time. This is a real, deliberate security tradeoff (the
# account's password is stored in plaintext in the registry, and the
# console stays unlocked indefinitely for anyone with access to it) --
# hence opt-in, not automatic, same reasoning already applied to the
# equivalent Linux/GDM autologin fix. One piece confirmed NOT to work
# without a genuinely UAC-elevated process even for an account in the
# Administrators group (the account's own admin-group membership isn't
# the same as an elevated token) -- the
# HKLM:\Software\Microsoft\Windows\CurrentVersion\Policies\System
# `InactivityTimeoutSecs` value -- so this relies on the screensaver +
# power-sleep settings instead, both of which a non-elevated admin
# account's own HKCU/powercfg calls can set directly.
#
# KNOWN LIMITATION, confirmed live (2026-09-15), genuinely easy to
# conflate with the lock-screen stall above since both present as "Kodi
# never opens its webserver port": a Proxmox-emulated HDA sound device can
# be unstable specifically under Kodi's default DirectSound audio backend
# on Windows -- observed as repeated "buffer underrun" warnings and
# `CAESinkDirectSound::Deinitialize: Cleaning up` cycling, with Kodi
# actively burning CPU (confirmed via Get-Process CPU sampled twice a few
# seconds apart, not just idle/hung) stuck retrying for several minutes
# before either recovering on its own or (observed once) never recovering
# at all within a reasonable wait. Switching the audio backend to WASAPI
# (`audiooutput.audiodevice`/`audiooutput.passthroughdevice` set to
# `WASAPI:default`) made the exact same VM/device start up cleanly and
# immediately every time afterward -- this script sets this unconditionally
# (not opt-in, since it's a reliability fix with no real security/privacy
# tradeoff, unlike the autologon fix above). Worth knowing if you ever see
# this stall again despite the fix: it means something changed about the
# VM's audio hardware, not that WASAPI itself stopped working.
#
# Usage (key-based auth, the default):
#   tools/kodi_provision_windows.sh --host 192.168.1.50 --user alice --zip /path/to/addon-pvr.dispatcharr-unofficial-*.zip
#   tools/kodi_provision_windows.sh --host 192.168.1.50 --user alice --zip ... --port 2222
#
# Usage (password auth): export SSHPASS first rather than passing it as an
# argument -- a command-line arg is visible to other users on the same
# machine via `ps`, an env var isn't. Requires `sshpass` installed.
#   SSHPASS='...' tools/kodi_provision_windows.sh --host 192.168.1.50 --user alice --zip ...

set -euo pipefail

ADDON_ID="pvr.dispatcharr-unofficial"
SSH_PORT=22

usage() {
  echo "Usage: $0 --host HOST --user USER --zip PATH_TO_ADDON_ZIP [--port PORT] [--enable-autologon]" >&2
  exit 1
}

HOST=""
SSH_USER=""
ZIP_PATH=""
ENABLE_AUTOLOGON=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --user) SSH_USER="$2"; shift 2 ;;
    --zip) ZIP_PATH="$2"; shift 2 ;;
    --port) SSH_PORT="$2"; shift 2 ;;
    --enable-autologon) ENABLE_AUTOLOGON=1; shift ;;
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

# Every PowerShell fragment below is written to a local temp file and
# scp'd over rather than passed inline as a `-Command "..."` string --
# confirmed live (2026-09-15) that anything beyond a short one-liner run
# this way hits unreliable, silently-swallowed quoting failures once
# PowerShell's own `$`-prefixed variable syntax collides with bash's own
# heredoc/quoting rules across the ssh hop. A real script file with a
# `param(...)` block, invoked via `-File`, sidesteps that entirely --
# dynamic values become plain, safely-quoted command-line arguments
# instead of text embedded inside an already-fragile string.
TMPDIR_LOCAL="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

echo "==> Checking connectivity to ${SSH_USER}@${HOST}:${SSH_PORT}..."
ssh_run "powershell -NoProfile -Command exit" || {
  echo "error: SSH connection failed." >&2
  exit 1
}

echo "==> Checking Kodi isn't currently running (it must be fully stopped for these edits to stick)..."
if ssh_run "powershell -NoProfile -Command \"if (Get-Process -Name kodi -ErrorAction SilentlyContinue) { exit 1 } else { exit 0 }\""; then
  :
else
  echo "error: Kodi is still running on ${HOST}." >&2
  echo "       Stop it first (ssh ${SSH_USER}@${HOST} powershell -Command \"Stop-Process -Name kodi -Force\")," >&2
  echo "       then re-run this script -- Kodi flushes guisettings.xml on exit and would" >&2
  echo "       overwrite these edits otherwise." >&2
  exit 1
fi

REMOTE_TMP_DIR="C:\\Users\\${SSH_USER}\\AppData\\Local\\Temp\\kodi-provision"
ssh_run "powershell -NoProfile -Command \"New-Item -ItemType Directory -Force -Path '${REMOTE_TMP_DIR}' | Out-Null\""

REMOTE_ZIP_NAME="$(basename "$ZIP_PATH")"
REMOTE_ZIP_PATH="${REMOTE_TMP_DIR}\\${REMOTE_ZIP_NAME}"
echo "==> Copying ${REMOTE_ZIP_NAME} to ${HOST}..."
scp_run "$ZIP_PATH" "${SSH_USER}@${HOST}:${REMOTE_ZIP_PATH}"

cat >"${TMPDIR_LOCAL}/deploy.ps1" <<'EOF'
param(
    [Parameter(Mandatory)][string]$AddonId,
    [Parameter(Mandatory)][string]$ZipPath
)
$ErrorActionPreference = 'Stop'
$addonsDir = Join-Path $env:APPDATA 'Kodi\addons'
$addonDir = Join-Path $addonsDir $AddonId
if (Test-Path $addonDir) {
    Remove-Item -Path $addonDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $addonsDir | Out-Null
Expand-Archive -Path $ZipPath -DestinationPath $addonsDir -Force
Remove-Item -Path $ZipPath -Force
Write-Output "addon deployed to $addonDir"
EOF

printf '%s\n' "==> Deploying the addon into Kodi's user-profile addon directory (%APPDATA%\\Kodi\\addons -- confirmed live 2026-09-15 this is where Kodi already keeps installed, non-bundled addons, not the admin-only C:\\Program Files\\Kodi\\addons that only holds what shipped with the installer)..."
scp_run "${TMPDIR_LOCAL}/deploy.ps1" "${SSH_USER}@${HOST}:${REMOTE_TMP_DIR}\\deploy.ps1"
ssh_run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${REMOTE_TMP_DIR}\\deploy.ps1\" -AddonId \"${ADDON_ID}\" -ZipPath \"${REMOTE_ZIP_PATH}\""

cat >"${TMPDIR_LOCAL}/guisettings.ps1" <<'EOF'
param()
$ErrorActionPreference = 'Stop'
$path = Join-Path $env:APPDATA 'Kodi\userdata\guisettings.xml'
# Confirmed live (2026-09-15): Windows Kodi's special://temp/ maps to
# %APPDATA%\Kodi\cache, not a literal "temp" folder -- mirroring that
# real mapping here rather than inventing a new location.
$screenshotsDir = Join-Path $env:APPDATA 'Kodi\cache\screenshots'
New-Item -ItemType Directory -Force -Path $screenshotsDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null

if (Test-Path $path) {
    [xml]$xml = Get-Content -Path $path -Raw
} else {
    [xml]$xml = New-Object System.Xml.XmlDocument
    $decl = $xml.CreateXmlDeclaration('1.0', 'UTF-8', $null)
    $xml.AppendChild($decl) | Out-Null
    $root = $xml.CreateElement('settings')
    $root.SetAttribute('version', '2')
    $xml.AppendChild($root) | Out-Null
}
$root = $xml.DocumentElement

$desired = @{
    'services.webserver'          = 'true'
    'debug.showloginfo'           = 'true'
    'debug.screenshotpath'        = $screenshotsDir
    # Confirmed live (2026-09-15, see this script's own header) -- a
    # Proxmox-emulated HDA device can be unstable under Kodi's default
    # DirectSound backend (a several-minute startup stall, CPU actively
    # spinning). WASAPI avoided it entirely on the same VM/device.
    'audiooutput.audiodevice'     = 'WASAPI:default'
    'audiooutput.passthroughdevice' = 'WASAPI:default'
}
foreach ($id in $desired.Keys) {
    $node = $root.SelectSingleNode("setting[@id='$id']")
    if ($null -eq $node) {
        $node = $xml.CreateElement('setting')
        $node.SetAttribute('id', $id)
        $root.AppendChild($node) | Out-Null
    }
    $node.InnerText = $desired[$id]
    # Same "default" attribute quirk already documented live for the
    # Linux/Flatpak build (tools/kodi_provision_linux_flatpak.sh's own
    # header) -- a node still carrying default="true" can have its
    # stored text silently ignored in favor of Kodi's own built-in
    # default. Clearing it here matches what Kodi itself does once a
    # setting is genuinely changed via its own GUI.
    if ($node.HasAttribute('default')) {
        $node.RemoveAttribute('default')
    }
}
$xml.Save($path)
Write-Output "guisettings.xml updated: $($desired | ConvertTo-Json -Compress)"
EOF

echo "==> Ensuring the JSON-RPC webserver, Kodi's core debug logging, and a screenshot destination are enabled in guisettings.xml..."
scp_run "${TMPDIR_LOCAL}/guisettings.ps1" "${SSH_USER}@${HOST}:${REMOTE_TMP_DIR}\\guisettings.ps1"
ssh_run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${REMOTE_TMP_DIR}\\guisettings.ps1\""

cat >"${TMPDIR_LOCAL}/addonsettings.ps1" <<'EOF'
param(
    [Parameter(Mandatory)][string]$AddonId
)
$ErrorActionPreference = 'Stop'
$path = Join-Path $env:APPDATA "Kodi\userdata\addon_data\$AddonId\settings.xml"
New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null

if (Test-Path $path) {
    [xml]$xml = Get-Content -Path $path -Raw
} else {
    [xml]$xml = New-Object System.Xml.XmlDocument
    $decl = $xml.CreateXmlDeclaration('1.0', 'UTF-8', $null)
    $xml.AppendChild($decl) | Out-Null
    $root = $xml.CreateElement('settings')
    $root.SetAttribute('version', '2')
    $xml.AppendChild($root) | Out-Null
}
$root = $xml.DocumentElement

$node = $root.SelectSingleNode("setting[@id='debug_logging']")
if ($null -eq $node) {
    $node = $xml.CreateElement('setting')
    $node.SetAttribute('id', 'debug_logging')
    $root.AppendChild($node) | Out-Null
}
$node.InnerText = 'true'
if ($node.HasAttribute('default')) {
    $node.RemoveAttribute('default')
}
$xml.Save($path)
Write-Output "addon settings.xml updated: debug_logging=true"
EOF

echo "==> Ensuring the addon's own debug_logging setting is enabled..."
scp_run "${TMPDIR_LOCAL}/addonsettings.ps1" "${SSH_USER}@${HOST}:${REMOTE_TMP_DIR}\\addonsettings.ps1"
ssh_run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${REMOTE_TMP_DIR}\\addonsettings.ps1\" -AddonId \"${ADDON_ID}\""

cat >"${TMPDIR_LOCAL}/register_task.ps1" <<'EOF'
param()
$ErrorActionPreference = 'Stop'
$kodiExe = 'C:\Program Files\Kodi\kodi.exe'
$action = New-ScheduledTaskAction -Execute $kodiExe
# LogonType Interactive: runs in whichever session is actually logged
# into the console, not Session 0 -- confirmed live (2026-09-15) that a
# GUI process started directly from an SSH command lands in Session 0
# instead and is invisible/unscreenshottable, the Windows analog of
# Linux Flatpak Kodi needing WAYLAND_DISPLAY explicitly set.
$principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$env:USERNAME" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'LaunchKodiInteractive' -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Write-Output "registered scheduled task 'LaunchKodiInteractive' -> $kodiExe"
EOF

echo "==> Registering a Scheduled Task to launch Kodi into the real interactive session (see this script's own header for why a bare SSH launch doesn't work)..."
scp_run "${TMPDIR_LOCAL}/register_task.ps1" "${SSH_USER}@${HOST}:${REMOTE_TMP_DIR}\\register_task.ps1"
ssh_run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${REMOTE_TMP_DIR}\\register_task.ps1\""

if [[ "$ENABLE_AUTOLOGON" -eq 1 ]]; then
  cat >"${TMPDIR_LOCAL}/autologon.ps1" <<'EOF'
param(
    [Parameter(Mandatory)][string]$AutologonUser,
    [Parameter(Mandatory)][string]$AutologonPassword
)
$ErrorActionPreference = 'Stop'

# See this script's own header ("idle Windows session can lock itself")
# for why this is needed at all, and why it's opt-in rather than
# automatic -- a real security tradeoff, not a pure reliability fix.
$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Set-ItemProperty -Path $winlogon -Name AutoAdminLogon -Value '1' -Type String
Set-ItemProperty -Path $winlogon -Name DefaultUserName -Value $AutologonUser -Type String
Set-ItemProperty -Path $winlogon -Name DefaultPassword -Value $AutologonPassword -Type String
Set-ItemProperty -Path $winlogon -Name DefaultDomainName -Value $env:COMPUTERNAME -Type String

# Disable the screensaver (per-user setting, HKCU of whichever account
# this script is running as -- normally the same account being
# autologon'd).
Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name ScreenSaveActive -Value '0' -Type String
Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name ScreenSaverIsSecure -Value '0' -Type String

# Never sleep/turn off the display on AC power -- a sleeping/display-off
# session is a separate path back to a locked session on resume.
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0
powercfg /change hibernate-timeout-ac 0

Write-Output "autologon + idle-lock prevention configured for $AutologonUser"
EOF

  if [[ -z "${SSHPASS:-}" ]]; then
    echo "error: --enable-autologon needs the account's own password to configure (it's what Windows will" >&2
    echo "       use to log back in automatically) -- re-run with SSHPASS set, even if you're using key-based" >&2
    echo "       SSH auth otherwise; this script never logs or displays it." >&2
    exit 1
  fi
  echo "==> Configuring Windows Autologon and disabling idle-lock (--enable-autologon was passed)..."
  scp_run "${TMPDIR_LOCAL}/autologon.ps1" "${SSH_USER}@${HOST}:${REMOTE_TMP_DIR}\\autologon.ps1"
  ssh_run "powershell -NoProfile -ExecutionPolicy Bypass -File \"${REMOTE_TMP_DIR}\\autologon.ps1\" -AutologonUser \"${SSH_USER}\" -AutologonPassword \"${SSHPASS}\""
fi

echo "==> Cleaning up remote temp files..."
ssh_run "powershell -NoProfile -Command \"Remove-Item -Path '${REMOTE_TMP_DIR}' -Recurse -Force -ErrorAction SilentlyContinue\""

if [[ "$ENABLE_AUTOLOGON" -eq 1 ]]; then
  LOGIN_NOTE=" -- Autologon should handle this on the VM's next reboot, but this first time still needs a real login"
  AUTOLOGON_NOTE="Autologon and idle-lock prevention are configured -- this VM should now
survive both a reboot and a long SSH-only session without needing anyone
at the console again."
else
  LOGIN_NOTE=""
  AUTOLOGON_NOTE="--enable-autologon wasn't passed -- if you're planning to drive this VM
purely over SSH for any length of time, its session can lock itself from
inactivity and silently stall Kodi's startup (see this script's own
header). Re-run with --enable-autologon (and SSHPASS set) if that happens."
fi

cat <<EOF

==> Done. Remaining manual steps:
    1. Make sure an interactive console session is actually logged in on
       ${HOST} (e.g. via Proxmox's own console viewer)${LOGIN_NOTE} -- the
       Scheduled Task launches into that session, not a fresh one of its own.
    2. Launch (or relaunch) Kodi:
           ssh ${SSH_USER}@${HOST} schtasks /run /tn LaunchKodiInteractive
    3. Open this addon's own settings and enter your real Dispatcharr
       server URL/credentials -- deliberately not handled by this script.
    4. Confirm Settings -> Services -> Control shows "Allow remote control
       via HTTP" already checked (this script enabled it on disk; Kodi
       will pick it up on this fresh start).

If this is a brand new Kodi profile that has never been launched before
and step 4 above turns out NOT checked, see this script's own
"KNOWN LIMITATION" header comment -- launch Kodi once first (step 2),
quit it, then re-run this script.

${AUTOLOGON_NOTE}

Once Kodi is up and connected, run:
    python3 tools/kodi_smoke_test.py --host ${HOST}
    python3 tools/kodi_screenshot.py --host ${HOST} --ssh-user ${SSH_USER}
EOF
