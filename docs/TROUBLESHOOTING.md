# Troubleshooting

What to check when something doesn't work. If your problem isn't here, switch on **Enable verbose debug logging** in the
addon's settings, reproduce it, and look in `kodi.log` for lines that start with `pvr.dispatcharr-unofficial`.

Each entry says what you see, why, and what to do. The long investigations behind them (how each cause was confirmed,
what was tried) are in [TROUBLESHOOTING_DETAIL.md](TROUBLESHOOTING_DETAIL.md).

| You see | Go to |
|---|---|
| A channel fails to start, then plays when you try again | [Slow channel starts](#a-channel-fails-to-start-then-plays-when-you-try-again) |
| A spinner for about 30 seconds, then the channel closes (Server-side mode) | [Server-side spinner](#server-side-live-tv-a-spinner-for-about-30-seconds-then-the-channel-closes) |
| Live TV freezes, then recovers or ends | [Stalls](#live-tv-freezes-for-a-while-then-recovers-or-ends) |
| The guide is empty but channels play | [Empty guide](#the-guide-is-empty-but-channels-and-recordings-work) |
| A recording made on another device isn't listed, or recording playback fails with 401 | [More than one Kodi client](#known-limitations-with-more-than-one-kodi-client) |
| No Record button or Timers window, or "You do not have permission" when recording | [Permissions](#recording-is-refused-or-record-and-delete-are-missing) |
| Nothing works after install or reboot, every PVR call fails | [Kodi startup](#kodi-does-not-start-the-addon) |
| Something odd in Kodi's own screens (guide grid, timers list, a recording's length) | [Kodi quirks](#known-kodi-core-quirks-confirmed-not-this-addons-bug) |

## A channel fails to start, then plays when you try again

**You see:** a playback error after about 20 seconds; pressing OK and opening the channel again plays it at once.
**Applies to:** **Live TV pause/rewind (timeshift)** set to Off or Local. (Server-side mode waits up to about 30 seconds for a slow channel itself.)

**Why:** Dispatcharr's proxy sends nothing until it has buffered its first data. Kodi opens the stream itself and gives up
after 20 seconds without a byte. The second try works because the channel is already running by then.

**What to do:**

1. **Check whether the channel's source is an HLS playlist with several quality levels** (its stream URL in Dispatcharr
   ends in `.m3u8`). Dispatcharr's default **FFmpeg** stream profile opens every quality level before it sends the first
   byte, so the delay grows with their number; the built-in **Streamlink** profile sends one quality (the highest)
   and starts much sooner (measurements in [TROUBLESHOOTING_DETAIL.md](TROUBLESHOOTING_DETAIL.md#stalls-or-slow-channel-starts-that-come-from-the-stream-itself)). To try it, change the channel's (or its stream's) stream profile from FFmpeg to Streamlink in
   Dispatcharr.
2. **Otherwise, or as well, give Kodi more patience.** In Kodi's `userdata` folder create or edit `advancedsettings.xml`:

   ```xml
   <advancedsettings>
     <network>
       <curllowspeedtime>60</curllowspeedtime>
     </network>
   </advancedsettings>
   ```

   then restart Kodi. This is a Kodi-wide setting (any stalled download now takes 60 seconds to give up), and the addon
   cannot set it for you.

## Server-side live TV: a spinner for about 30 seconds, then the channel closes

**Why:** the timeshift buffer plugin serves its video from its own port (the plugin's **Buffer server port** setting, default
**9192**), not from Dispatcharr's port. If Kodi can't reach that port, the buffer still starts on the server, but nothing
can be played. Since 0.13.0 the addon shows a notification naming the port.

**What to do:** publish that port from the Dispatcharr container (for Docker, `-p 9192:9192`, the same number on both
sides) and make sure the Kodi device can reach it. To check by hand from the Kodi device:
`curl -i http://<dispatcharr-host>:9192/` should get an HTTP answer (`404 Not Found` for that bare address is normal);
"connection refused" or a hang means it can't be reached.

## Live TV freezes for a while, then recovers or ends

- **Server-side mode:** if a channel's stream stops sending data without closing, Dispatcharr switches to the channel's
  next stream after about a minute. The addon waits up to 70 seconds for that, so the picture freezes and then resumes by
  itself. If the channel has no other stream, playback ends after about a minute. Stop works throughout.
- **Off and Local mode:** Kodi's own player decides how long to wait.
- **Stalls that come from the provider** (a CDN that pauses, a timestamp break) can't be repaired by a client. If the
  addon's log shows nothing wrong and another player stalls the same way, look at the server. Dispatcharr's plugin hub
  has plugins aimed at this (Reservoarr, Profilarr and Segmentarr for gaps and timestamp breaks; Audio Buffer Tuner for
  the proxy's start-up prebuffer). They are named here from their descriptions only: none has been tried with this
  addon, so this is a pointer, not a recommendation.

## The guide is empty, but channels and recordings work

**You see:** `Dispatcharr returned HTTP 403 for /output/epg` in `kodi.log`, with login, channels, playback and recordings
all fine.

**Why:** the guide endpoint is the one Dispatcharr endpoint that, by default, only accepts local-network addresses
(127.0.0.0/8, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 and their IPv6 equivalents). A Kodi that reaches the server from
elsewhere is refused: a Tailscale or other CGNAT address, a remote Kodi, or a LAN where the configured host resolves to a
global IPv6 address.

**What to do:** in Dispatcharr, open Settings, then Network Access, then "M3U / EPG Endpoints", and add the Kodi device's
address or range.

## Known limitations with more than one Kodi client

These are not bugs; they come from running several separate Kodi installs against one Dispatcharr server.

- **A recording made on one device doesn't appear on another straight away.** Each install polls on its own schedule.
  The wait is at most the **Recordings/timers refresh interval** (5 minutes by default). Switching on **Enable real-time
  recording/timer updates (experimental)** makes most changes show up at once.
- **Resume position doesn't carry between devices.** Kodi keeps it in each install's own database. Kodi has a hook for
  server-side resume positions; this addon doesn't use it yet.
- **Recording playback failing with `HTTP 401` after another install started.** Dispatcharr keeps one API key per
  account. The addon adopts the account's existing key instead of replacing it, and retries once with a fresh one after a
  401, so this normally heals itself. If it still happens, check that the installs use the same account and are up to
  date.

## Kodi does not start the addon

- **Two PVR clients enabled at once.** If an older copy of this addon (`pvr.dispatcharr` or `pvr.dispatcharrai`) is still
  enabled next to `pvr.dispatcharr-unofficial`, Kodi's PVR manager can get stuck: every `PVR.*` call fails and the new
  addon logs nothing. Disable the old addon; the new one starts within seconds.
- **After a cold Kodi reboot, nothing works and the addon logs nothing.** Seen on CoreELEC hardware. In Kodi, disable the
  addon and enable it again a few seconds later; a plain restart doesn't reliably help.

## Recording is refused, or Record and Delete are missing

**You see:** no Record button, no Timers window and no Delete or Rename on recordings, from an account that can log in,
browse and play. (Before 0.12.0 the addon offered them anyway, and each failed with "You do not have permission to
perform this action".)

**Why:** Dispatcharr lets only an admin, or a standard account whose DVR access is `manage`, create or change recordings
and timers; a standard account is view-only unless an admin sets it. The addon reads the account's DVR access and hides
what it can't do. This is a Dispatcharr permission, not an addon bug.

**What to do:** an admin sets the account's DVR access to `manage` in Dispatcharr's user settings.

## Known Kodi-core quirks (confirmed not this addon's bug)

None of these can be fixed from the addon; each was traced to Kodi's own code.

- **A completed recording shows a wrong, too-short length in the list.** Kodi remembers the length it measured if the
  recording was played while it was still being written, and prefers it over the addon's correct value, including after a
  restart. Playback itself is unaffected.
- **The guide grid jumps to an odd time of day after switching channel groups a few times.** Kodi tries to keep your
  selection across the switch and sometimes lands on a stale position. Paging a day forward and back, or reopening the
  Guide window, re-centres it.
- **A timer you edit keeps its old title in the open Timers window.** The change is saved; leave the window and re-enter
  it to see it.
- **Recording a programme that overlaps an existing timer on the same channel fails** with "The PVR backend does not
  allow to record this event." Kodi refuses it before the addon is called, even for a couple of minutes of overlap.
- **A channel group that has lost all its channels stays in Kodi as an empty group,** even after a restart. Kodi never
  removes a group a client stops reporting. Delete it in Kodi's group manager.
