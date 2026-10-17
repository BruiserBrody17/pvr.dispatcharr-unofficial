# pvr.dispatcharr-unofficial

**This is an unofficial, community-maintained addon.** It is not
affiliated with, endorsed by, or supported by the
[Dispatcharr](https://github.com/Dispatcharr/Dispatcharr) project, nor
by Kodi/Team Kodi -- it's a third-party client built against
Dispatcharr's public API, using Kodi's public addon interface. Please
don't file addon bugs on Dispatcharr's or Kodi's own issue trackers,
and don't expect either project's maintainers to support this addon.

A Kodi PVR (Live TV / DVR) addon for [Dispatcharr](https://github.com/Dispatcharr/Dispatcharr),
built against Dispatcharr's native REST API rather than its Xtream Codes
compatibility layer, so channels, EPG, live TV, and recordings in Kodi map
directly onto Dispatcharr's own backend and settings.

**Platforms**: Windows, macOS, Linux, CoreELEC (tested on an ODROID N2+),
and Android (arm/arm64, tested on real 32-bit and 64-bit ARM devices).

**Kodi version**: this branch (`Piers`) is built against Kodi 22 ("Piers"), currently its 22.0 RC1. It has been run on
Kodi 22.0 RC1 on Windows, macOS, Linux, 64-bit and 32-bit Android and CoreELEC (an ODROID N2+). Audio passthrough, a
long soak and HEVC have not been checked on Kodi 22. While Kodi 22 is a release candidate, `0.14.0` is marked a
pre-release on GitHub, so it is not shown as the "latest" release. **Kodi 21 ("Omega") users want the `0.13.x` releases** from the `Omega`
branch: Kodi checks the API version a binary addon was built against, and a build for one major version is refused by the
other. A different Kodi major version than the one a release names has not been tested.

**Status**: pre-1.0 and actively developed by a single maintainer --
expect occasional rough edges. Verified against Dispatcharr `0.32.0` (and `0.31.0` before it);
Dispatcharr's own REST API has changed shape across releases before,
so a real incompatibility against a different version is possible and
not guaranteed to be caught yet. Bug reports (with the detail requested
in the issue template) are genuinely useful for exactly this reason.

## Features

- Channel and channel-group listing, with EPG (posters, New/Premiere/Live
  badges, genre, cast, episode names where the EPG source provides them)
- Live TV playback, with optional pause/rewind/seek -- either server-side
  via a companion Dispatcharr plugin, or entirely on-device with no
  Dispatcharr admin account needed (see below)
- Recording playback, including watching a recording while it's still
  being written
- Recording pre/post padding, synced with Dispatcharr's own global
  setting (needs an admin account to change -- see below)
- Commercial-break markers on a recording's seekbar, for recordings
  Dispatcharr's comskip integration has marked (needs the
  `recording_edl` companion plugin)
- One-time, series, and recurring (day-of-week) timers, all editable in
  place from Kodi's own timer list. A recurring rule created from Kodi is
  tagged `[Kodi]` in Dispatcharr's own web UI (Kodi never shows the tag):
  that tag is how the addon knows the rule is its own, so it keeps
  extending the rule's end date for you, and picks it back up if no Kodi
  ran for a while. Rules you make in Dispatcharr yourself are never
  touched. Remove the tag to make the addon stop managing a rule
- Series rules made from Kodi can match the programme title loosely: the setting "Series rules made here: how the title is
  matched" chooses Exact (default), Contains, or Whole words, for rules created from then on. Existing rules, and rules made
  in Dispatcharr's web interface (which also offers regular expressions and a description filter), are never changed by it
- Catch-up/archive playback ("play from guide") for channels whose
  provider supports it
- Optional real-time push updates for recordings/timers, so most changes
  made elsewhere (another Kodi install, Dispatcharr's web UI) show up
  immediately rather than waiting for the next periodic refresh (anything
  the feed doesn't cover still arrives within the Recordings/timers refresh interval, 5 minutes by default); the same feed also tells the
  addon when Dispatcharr has finished refreshing an EPG source or an M3U account, so a new guide or
  channel list arrives within minutes rather than hours

## Companion Dispatcharr plugins

Two optional server-side plugins live in `dispatcharr-plugin/` in this
repo and install separately on your Dispatcharr instance (not through
Kodi):

- **`timeshift_buffer`** -- enables server-side live TV pause/rewind/seek
  (**Live TV pause/rewind (timeshift)** set to **Server-side (Dispatcharr plugin)**): one buffer per channel,
  shared across every device currently watching, torn down as soon as the
  last viewer stops. Requires an admin-level Dispatcharr account (see the
  plugin's own README for the full picture). Without one, use **Local (this device)**
  instead: pause/rewind buffered on the Kodi device itself via
  `inputstream.ffmpegdirect`, no admin account or server-side plugin
  needed, but local-only and gone on restart. **Off** (the default) is a
  plain live stream, no pause/rewind, no extra dependency.
- **`recording_edl`** -- exposes comskip commercial-break markers to Kodi.
  Also requires an admin-level Dispatcharr account. Optional; without it
  (or with a non-admin account), recordings just show no markers --
  playback itself is unaffected.

Each has its own README with install steps.

## Installing

1. Download the zip for your platform from the
   [latest release](https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/releases).
2. In Kodi: **Add-ons -> Install from zip file**, and select the
   downloaded zip.
3. Enable and configure the addon under **Settings -> PVR & Live TV ->
   General -> PVR client add-ons** (or **Add-ons -> My add-ons -> PVR
   clients**, the same list either way): select **Dispatcharr PVR
   Client (Unofficial)**, enable it, then **Configure** (see below).

## Configuration

Set in Kodi's addon settings: Dispatcharr host, port, HTTPS toggle,
username, and password. A standard account with `dvr_access` set to
`manage` covers everything native to Dispatcharr -- browsing, playback,
and timer management. An admin account is needed for both companion
plugins and for pushing a recording-padding change back to Dispatcharr
(reading the current value doesn't need one; with a non-admin account the
padding settings are greyed out).

This addon only speaks Dispatcharr's native REST API, not its Xtream
Codes compatibility layer -- there's no separate "XC" username/password
to enter anywhere. A Dispatcharr account with the restricted "Streamer"
role can't log in through this addon at all (Dispatcharr itself rejects
that role's login attempt with "No active account found," regardless of
whether the username/password are correct); use a regular or admin
account instead. Repeated login attempts against wrong or
incompatible credentials can also trigger Dispatcharr's own request
rate-limiting -- if you see "too many requests," wait a few minutes
before retrying with corrected credentials.

Two settings look alike but are not: this addon's "EPG refresh interval" is how often it downloads the whole
guide from Dispatcharr (the costly step, 4 hours by default), while Kodi's own Settings > PVR & Live TV > Guide >
"Update interval" only sets how often Kodi re-reads the guide from the addon, with no server traffic. Neither
overrides the other, and the addon tells Kodi to re-read right after each download, so Kodi's interval does not delay
new data.

Most settings take effect immediately after saving. Connection settings
(host, port, HTTPS, username, password, SSL certificate verification and
connection timeout) and real-time updates need a Kodi restart -- Kodi will
tell you when one does.

If something doesn't work as expected -- a channel that is slow to start, an empty guide, a recording that doesn't
show up on another device -- see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Building

See [docs/BUILDING.md](docs/BUILDING.md). This addon builds through
Kodi's own binary-addon build harness, which needs a matching Kodi source
checkout -- it can't be compiled standalone.
`.github/workflows/build.yml` is the workflow that builds the Windows and Linux
zips; the release zips are published by hand (see [docs/BUILDING.md](docs/BUILDING.md)). macOS, Android and the
CoreELEC package are built by hand for each release too, and every release zip passes one privacy check before
it is published.

## More detail

See [CHANGELOG.md](CHANGELOG.md) for what changed release to release.
The `docs/` directory holds engineering notes -- how each feature was
built, root causes for bugs that were found and fixed, and decisions that
were tried and reverted -- kept as project history rather than user
documentation; the exception is [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md),
written for users. Start at [docs/API_NOTES.md](docs/API_NOTES.md) if you
want it.

## License

GPL-2.0-or-later, matching Kodi's own binary addon convention.
