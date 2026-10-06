# Manual testing checklist

What a person has to do before a release, because neither the unit suites
(pure logic only) nor `tools/kodi_smoke_test.py` (JSON-RPC only: it can confirm
a playback position advanced, not that the picture is right) can reach it.
Each item says how to do it, what a pass looks like, and what automation
already covers so you do not repeat that. Dated results are from the lab run
of 2026-10-02 (Linux, Flatpak Kodi 21, a real Dispatcharr instance); anything
marked **lab-checked** was driven from there and needs a person only on
platforms the lab does not have.

Run `python3 tools/kodi_smoke_test.py --host <kodi> ...` first (its docstring has the
options; add `--allow-mutations` only against an instance where creating and
deleting a real timer is fine). A pass there is the precondition for everything
below, and it takes the API-level surface off this list. Turn **Enable verbose
debug logging** on in the addon's settings before any of it, and keep
`kodi.log` -- `Packet corrupt`, `large audio sync error` and the addon's own
`giving up` / `marking stream fatal` lines are what to look for.

Platforms: the lab runs of 2026-10-02 were on Linux; macOS got the smoke harness, a
timeshift session with seeks and an in-progress recording playback on the same day
(`docs/OPEN_ITEMS.md`, "macOS build and smoke test of the hardening branch is stale").
To run Kodi on a machine whose own profile must not be touched, start it with a different
`HOME` (on macOS: `open -n --env HOME=<dir> -a Kodi.app`), enable the JSON-RPC webserver in
that profile's `guisettings.xml`, put the built addon in its `addons` folder and enable it
with `Addons.SetAddonEnabled`; that profile's log is `<dir>/Library/Logs/kodi.log`.

## 1. Things only a person can judge

- **Picture and sound while playing.** Live (timeshift off, local, and
  server-side), a finished recording, a recording still in progress, and a
  catch-up programme, each for a few minutes with a seek in the middle. Pass:
  no repeated visible block/tear, no audio drift that does not settle within a
  few seconds of a seek. A short audio-sync warning right after a seek is
  normal; a sustained one is not. `Packet corrupt` lines should not appear at
  all on `timeshift_buffer` 0.8.0 or later (the earlier plugin produced about
  one per segment).
- **The settings dialog itself.** Open it from Add-ons -> PVR clients ->
  Configure (opening it any other way leaves it unbound to the addon and tests
  the wrong thing). Enter host and credentials from nothing, change a value,
  press OK and Cancel, reopen. Pass: values stick or revert as expected, no
  restart prompt for a change that needs none (the API key is the case that used to
  restart for nothing). JSON-RPC cannot type into it.
- **The skin's own views.** Channel list with logos, the guide grid, the
  Recordings and Timers lists, the timer dialog (one-time, series, recurring),
  and the "Play recording" / scheduling-conflict dialogs. The smoke test avoids
  triggering the dialogs on purpose.

## 2. Real hardware

Not substituted for by a virtual GPU or a laptop.

- Hardware decode on the real device (VAAPI on Intel/AMD/NVIDIA Linux,
  DXVA2/D3D11VA on Windows, the platform decoder on Android, and CoreELEC on an
  ODROID N2+). Pass: live and recorded H.264/HEVC play without falling back to
  software decode noise in the log.
- Audio passthrough (AC3/DTS bitstreaming to a real receiver).
- CoreELEC specifically: install the package, confirm the addon loads at all
  (`docs/OPEN_ITEMS.md`, "CoreELEC-built addon.xml has an empty <platform> tag").
  For that item, with a zip built from an untagged branch (below): copy it to the N2+,
  Settings -> Add-ons -> Install from zip file, then Add-ons -> My add-ons -> PVR clients.
  Pass: the addon installs without an "incompatible"/"unsupported platform" message, can be
  enabled, shows its channels, and `kodi.log` (`/storage/.kodi/temp/kodi.log`) has no
  `CAddonInfoBuilder::Generate: No platform for add-on` line. A blank tag is expected in its
  `addon.xml`. Then play a live channel for a minute so the library itself is exercised.
  Building that zip with no tag (needs the CoreELEC tree, `docs/BUILDING.md`): `git archive` the
  commit with `--prefix=pvr.dispatcharr-unofficial-<PKG_VERSION>/` into
  `sources/pvr.dispatcharr-unofficial/pvr.dispatcharr-unofficial-<PKG_VERSION>.tar.gz`, write its
  SHA256 into that file's `.sha256` sidecar and into the tree's copy of `package.mk` (never the
  repo's), delete the old `build/` and `install_pkg/` entries for the addon, and run
  `PROJECT=Amlogic-ce ARCH=arm DEVICE=Amlogic-ng ./scripts/create_addon pvr.dispatcharr-unofficial`.
  If `target/addons/...` still holds an old zip, zip the installed `addon.xml`, `resources/` and
  the versioned `.so` yourself.
  Lab result (2026-10-02, a real N2+, CoreELEC Omega): installs, enables and creates the PVR
  client with the blank tag; the smoke harness passes (14 of 14 attempted); live 1080p H.264 through
  server-side timeshift uses the Amlogic hardware decoder (`CAMLCodec::OpenDecoder` 1920x1080 in the
  log) with no `Packet corrupt` or audio-sync errors over seeks and a pause. Still for a person on
  that hardware: audio passthrough, a long soak, and HEVC.

## 3. Long-running conditions

- **Soak.** Play a live channel for at least 30 minutes, with a big step back
  every few minutes and a pause now and then, sampling the Kodi process's resident
  memory each minute. Pass: playback never stops by itself, memory settles rather than
  climbing steadily. Lab-checked (2026-10-02, plugin 0.8.0, server-side
  timeshift): 30 minutes, a step back every 5 minutes and a 10 s pause every 7,
  resident memory essentially flat between the first and last sample (29 samples), playback never stopped, zero `Packet corrupt`, zero fatal-stream
  lines, zero addon errors. Hours-long runs are still a person's job.
- **The rolling window.** Set the plugin's `buffer_minutes` to 1, play, pause for
  longer than two minutes, resume. Pass: playback continues from the oldest
  segment still held (the log says it moved forward), no fatal error.
  Lab-checked on `timeshift_buffer` 0.8.0.
- **A refresh on the server reaches Kodi within minutes.** With "Enable real-time recording/timer updates" on, refresh the EPG source in
  Dispatcharr (the Sources page, or `POST /api/epg/import/` with the source's id) and watch Kodi's log with debug
  logging on: when the source finishes parsing, a line says the guide will be fetched again shortly, and about
  330 seconds later (past Dispatcharr's own cache of the exported guide) the guide is downloaded again and the
  per-channel EPG updates are triggered. Refresh an M3U account that changes channels (or just trigger its refresh,
  `POST /api/m3u/refresh/<id>/`: one that changes none is correctly ignored) and the channel list is fetched again
  within a minute. Pass: both happen without waiting for the polling interval. See `docs/EPG.md`'s section on refresh events.
- **Rule renewal.** With a recurring rule the addon created, leave Kodi running
  across a renewal; the rule's end date should move forward without anyone touching it.
  Needs real elapsed time (the renewal fires on its own schedule).

## 4. When the server is unreachable or misbehaving

Lab-checked with a TCP forwarder in front of Dispatcharr that could refuse,
drop or hang connections on demand, and Kodi's addon host pointed at it
(the plugin's segment port needs forwarding too). Do the same on any new
platform, or by pulling the network cable.

| Cut | Expected | Lab result (2026-10-02) |
|---|---|---|
| Live timeshift, connections refused for 8 s and for 20 s | Playback stalls, then resumes | Both survive, measured at the live edge where Kodi has nothing buffered. **Before the fix it did not:** four refused fetches in a row, about two seconds, marked the stream dead for good; and a first version of the fix, which returned zero-byte reads to Kodi, still lost the stream after about five seconds there |
| Live timeshift, refused for 45 s | Playback ends cleanly, no hang, UI stays responsive | Ends (Kodi gives up at about 40 s, as does the plugin's 30 s idle timeout); UI answered every ping in 0.3 s |
| Live timeshift, connections hang for 20 s | UI never blocks; playback resumes | UI fine, playback resumed |
| Completed recording, refused for 12 s | Playback resumes | Survives. **Before the fix it ended** after about 8 s |
| In-progress recording, refused for 12 s | Playback resumes | Survives |
| Server down when Kodi starts | Channels and guide kept; recovers within about a minute of the server returning | Confirmed 2026-09-30, `docs/OPEN_ITEMS.md` |

Not done: a provider-side concurrent-stream limit hit on purpose, and a full
disk during a recording (both need the real conditions).

## 5. Clean install and upgrade

- **Fresh first run.** Move the addon's `addon_data` folder aside, leave only the
  host and credentials, start Kodi. Pass: it adopts the account's existing API key
  rather than generating a new one (generating one logs out every other client of
  that account), fills in the timezone, loads channels and guide, plays live.
  Lab-checked on the addon side (channels, a channel's guide, recordings, timers
  and live playback all came up, no errors). A brand-new Kodi
  profile (empty PVR database) was not tried.
- **Upgrade in place.** Install the new zip over an older version with the old
  settings in place. Pass: the addon starts, settings survive, nothing re-prompts.
  Not lab-checked: it needs an older released build to install over.
- **Plugin pairings.** The addon with the previous `timeshift_buffer`, and the new
  plugin with the previous addon, both play (the lab checked 0.8.0 with the
  current addon, and 0.7.0 with it).

## 6. More than one client

- Two Kodi installs playing the same channel at once with server-side timeshift:
  both play, stopping one leaves the other playing, stopping the last tears the
  buffer down. Lab-checked at the plugin level with a second viewer added and
  removed through the plugin's own API while Kodi played (2 viewers, "buffer still
  active for other viewers", then none left once Kodi stopped); two real Kodi
  installs at once is the part to repeat by hand.
- Two installs sharing one account: neither should end up with a revoked key
  (`docs/OPEN_ITEMS.md`, the two API-key entries).
