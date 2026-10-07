# Release history before the tags were cleared

On 2026-10-06 every release and tag (and the matching local tags) was
removed, so the project starts its tag history afresh with the next release. Nothing in the code
was lost: each of these commits is still on `Omega`. This table records which commit each tag
named, so a version can still be found without digging through the history of `addon.xml.in`.

The commit hashes are the current ones. The history was rewritten on 2026-09-29 (a trailer
cleanup, see `docs/OPEN_ITEMS.md`), and the tags were moved onto the rewritten commits then.
Dates are the commit dates in UTC.

Not preserved: the release assets (each release carried two files; the Windows and plugin
zips can be rebuilt by CI from the commit, the hand-built zips cannot) and the release notes.
The per-version changes themselves are in `CHANGELOG.md`.

The version numbers went `0.2.0`, `0.3.0`, `0.4.0`, then `1.0.0-beta.1` to `1.0.8`, then back to
`0.9.0` to `0.11.0` (see `CLAUDE.md`'s note on pre-1.0 versioning), so these are listed in the
order they were created, not sorted by version.

| Tag | Commit | Date | Commit subject |
| --- | --- | --- | --- |
| `v0.2.0` | `a0dcd5d655` | 2026-09-02 | Bump version to 0.2.0 |
| `0.3.0` | `c0228a0947` | 2026-09-03 | Bump version to 0.3.0 |
| `0.4.0` | `4612b3aa76` | 2026-09-04 | Bump version to 0.4.0 |
| `1.0.0-beta.1` | `2c886ecfa9` | 2026-09-04 | Bump version to 1.0.0-beta.1 |
| `1.0.0-beta.2` | `1c42b10104` | 2026-09-04 | Bump version to 1.0.0-beta.2 |
| `1.0.0-beta.3` | `9e9acc314d` | 2026-09-04 | Bump version to 1.0.0-beta.3 |
| `1.0.0` | `9de73876af` | 2026-09-05 | Rename CHANGELOG's Unreleased section to 1.0.0 ahead of the tag |
| `1.0.1` | `76795d09da` | 2026-09-06 | Bump addon to 1.0.1 and decouple plugin versioning going forward |
| `1.0.2` | `a00315e545` | 2026-09-06 | Bump addon to 1.0.2 |
| `1.0.3` | `229ee45e6a` | 2026-09-06 | Bump addon to 1.0.3 |
| `1.0.4` | `360ef5c2f9` | 2026-09-06 | Bump addon to 1.0.4 |
| `1.0.5` | `5b95f80f63` | 2026-09-07 | Harden and speed up the server-side timeshift buffer; bump to 1.0.5 |
| `1.0.6` | `947d2b5b27` | 2026-09-07 | Fix 1.0.5 regression: unbounded heartbeat call could stall live playback permanently |
| `1.0.7` | `ff8c5b576e` | 2026-09-07 | Fix manifest-cache instance identity for real this time: pid recycles, access_token doesn't |
| `1.0.8` | `10d3844566` | 2026-09-07 | Project-wide C++ review: three defensive fixes, none reproduced live |
| `0.9.0` | `6e8bcde80c` | 2026-09-07 | Step back to 0.x versioning and add code formatting tooling |
| `0.9.1` | `256c741456` | 2026-09-08 | Bump addon to 0.9.1 |
| `0.9.2` | `761298f041` | 2026-09-09 | Bump addon to 0.9.2 |
| `0.9.3` | `34b788149e` | 2026-09-10 | Bump addon to 0.9.3 |
| `0.9.4` | `5f2f250914` | 2026-09-11 | Bump addon to 0.9.4 |
| `0.10.0` | `a1d3033200` | 2026-09-12 | Bump addon to 0.10.0 |
| `0.10.1` | `06f3b9583c` | 2026-09-15 | Bump addon version to 0.10.1 |
| `0.11.0` | `9913d19e4d` | 2026-09-16 | Bump addon version to 0.11.0 |

## The unsquashed hardening-audit history

The 473 commits of the project-wide hardening audit branch were squash-merged into `Omega` as one commit
(`5402c41`), so they are not ancestors of `Omega`. The per-finding record of what those commits
changed is `docs/OPEN_ITEMS.md`'s Fixed entries, and the tag history cleared on 2026-10-06 is the table above.
