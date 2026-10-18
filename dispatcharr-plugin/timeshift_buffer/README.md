# Timeshift Buffer (Dispatcharr plugin)

Server-side rolling live-TV buffer per channel, held by Dispatcharr
itself. `pvr.dispatcharr-unofficial` uses it to give live TV real pause, rewind,
fast-forward, and live-follow -- no extra step, just plain Play.

Set the addon's **Live TV pause/rewind (timeshift)** setting to **Server-side (Dispatcharr plugin)** to use it.
How it is built, and why, is in [docs/TIMESHIFT.md](../../docs/TIMESHIFT.md) in the main repo.

## How it works, briefly

The plugin keeps one rolling buffer per channel on the Dispatcharr server, shared by every device watching that
channel, and deletes it as soon as the last viewer stops. Each device can rewind only as far back as it has itself been
watching since it opened the channel.

## Disk use, and watching live while a recording runs

Each buffered channel writes a rolling window of the stream to the **Buffer storage path**: the **Buffer length (minutes)** setting,
60 by default. At 10 Mbit/s that is up to about 9 GB per channel while someone is watching it (the 60 minutes you can
rewind, about 4.5 GB, plus as much again that `ffmpeg` keeps on disk before deleting old segments, so a viewer still
reading one is not cut off), deleted as soon as the last viewer stops; lower the setting to shrink it (15 minutes is about
a quarter of that, and rewind depth shrinks to match).
The buffer is one `ffmpeg` that copies the stream without re-encoding.

If Dispatcharr is also recording that channel, the recording and the buffer share the channel's one connection to your
provider (no second slot is used), but each keeps its own copy of the data. To watch without a buffer while a
recording of the current programme runs, answer **Play recording** at Kodi's "Play recording / Switch to channel"
prompt: Kodi then opens the recording itself, with pause, seek and rewind, and no buffer is started. It ends when the
recording ends. Kodi shows that prompt only when a recording matches the programme on air, so a recording started
without guide data has none; open it from Kodi's Recordings list instead.

## Installing

1. Download `timeshift_buffer.zip` from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/releases)'s
   Assets and upload it via Dispatcharr's Plugins page **Import** button
   (**the folder name inside the zip must match `timeshift_buffer`
   exactly**, or every call 404s with "Plugin not found" -- already
   correct in the release zip). Alternatively, copy this directory to
   `plugins/timeshift_buffer/` under the directory your compose file
   bind-mounts as `/data` (so `/data/plugins/timeshift_buffer/` inside the
   container -- Dispatcharr's default plugins directory, unless
   `DISPATCHARR_PLUGINS_DIR` overrides it) -- useful if you're working from
   a repo checkout rather than a release.
2. In Dispatcharr's UI, open the Plugins page, click refresh, enable
   "Timeshift Buffer" (accept the trust-warning modal -- this plugin runs
   arbitrary server-side code, same as any other).
3. **Set Buffer storage path (`storage_path`) to real, persistent storage** before using it --
   left on the container's own unmapped filesystem, continuous rolling
   writes will fill up whatever's backing that (often a small cache
   volume) fast. Don't share one storage path between two Dispatcharr
   instances that can see each other's processes (the same container, or
   `--pid=host`): each treats a buffer directory its own Redis doesn't
   know as abandoned and stops the ffmpeg that owns it after five minutes.
4. **Publish the Buffer server port (`http_port`, default `9192`) through your container config**,
   the same way `9191` already is -- without this, the plugin's file
   server is only reachable from inside the container.
   The number in the setting and the published port must be the same: if they differ, the buffer starts but Kodi
   cannot read it (since addon 0.13.0 it says so with a notification). To check from the Kodi device,
   `curl -i http://<host>:<port>/` should get an HTTP answer: `404 Not Found` for that bare address is expected and
   means the server is reachable, while "connection refused" or a hang means it is not.
   The file server listens on IPv6 as well as IPv4 where the host has
   IPv6 (a dual-stack `::` socket), and on IPv4 alone where it doesn't.
5. Check **Internal base URL** matches how this plugin reaches Dispatcharr's
   own web service from inside the container (default
   `http://127.0.0.1:9191`).
6. The account calling these actions must be a Dispatcharr **admin**
   account (`user_level >= 10`).

## Testing manually

Paste a channel's UUID into the **Test channel UUID** setting and save
(action buttons can't take click-time input), then use these actions on the Plugins page:

| Action | Button | What it does |
|---|---|---|
| Start Buffer (manual test) | **Start Test Buffer** | Starts a buffer for the test channel and prints a playlist URL to try |
| Get Live Manifest (manual test) | **Get Test Manifest** | Shows the test channel's current segment list |
| List Active Buffers | **Refresh List** | Lists every running buffer |
| Stop Buffer | **Stop Test Buffer** | Stops the test channel's buffer and deletes its files |
| Stop All Buffers | **Stop Everything** | Stops every buffer, for every viewer; cleanup if something is stuck |
| Scrub Orphaned Buffer Directories | **Scrub Now** | Deletes leftover buffer folders nothing is using; the plugin also does this by itself on a timer |

Segments and a playlist should appear under the storage path. The file server answers 403 to a request for a buffer's files
without that buffer's access token, so use the URL **Start Test Buffer** prints
(`http://<dispatcharr-host>:<port>/<channel-uuid>/live.m3u8?token=...`); a plain URL without `?token=` is refused on
purpose. A buffer started by the Kodi addon gets its token in the response instead and never prints it.

## After updating the plugin

Restart Dispatcharr. A per-plugin disable/enable is not enough for the plugin's background file server to pick up new
code.
