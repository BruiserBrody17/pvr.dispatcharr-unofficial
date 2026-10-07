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
| `v0.2.0` | `dac4bb1f46` | 2026-09-02 | Bump version to 0.2.0 |
| `0.3.0` | `234c5f943d` | 2026-09-03 | Bump version to 0.3.0 |
| `0.4.0` | `daf70c31c0` | 2026-09-04 | Bump version to 0.4.0 |
| `1.0.0-beta.1` | `23acbd5a67` | 2026-09-04 | Bump version to 1.0.0-beta.1 |
| `1.0.0-beta.2` | `498121c3b5` | 2026-09-04 | Bump version to 1.0.0-beta.2 |
| `1.0.0-beta.3` | `a78bd4c60e` | 2026-09-04 | Bump version to 1.0.0-beta.3 |
| `1.0.0` | `2bf4300708` | 2026-09-05 | Rename CHANGELOG's Unreleased section to 1.0.0 ahead of the tag |
| `1.0.1` | `28b0463bdf` | 2026-09-06 | Bump addon to 1.0.1 and decouple plugin versioning going forward |
| `1.0.2` | `d3148dafb0` | 2026-09-06 | Bump addon to 1.0.2 |
| `1.0.3` | `045b6d6b16` | 2026-09-06 | Bump addon to 1.0.3 |
| `1.0.4` | `b87b2de32e` | 2026-09-06 | Bump addon to 1.0.4 |
| `1.0.5` | `5e058d93f3` | 2026-09-07 | Harden and speed up the server-side timeshift buffer; bump to 1.0.5 |
| `1.0.6` | `5525801fd7` | 2026-09-07 | Fix 1.0.5 regression: unbounded heartbeat call could stall live playback permanently |
| `1.0.7` | `216f97ff49` | 2026-09-07 | Fix manifest-cache instance identity for real this time: pid recycles, access_token doesn't |
| `1.0.8` | `1fbe030f9f` | 2026-09-07 | Project-wide C++ review: three defensive fixes, none reproduced live |
| `0.9.0` | `fdd932bd77` | 2026-09-07 | Step back to 0.x versioning and add code formatting tooling |
| `0.9.1` | `b474135b0c` | 2026-09-08 | Bump addon to 0.9.1 |
| `0.9.2` | `8804409b38` | 2026-09-09 | Bump addon to 0.9.2 |
| `0.9.3` | `8606068a30` | 2026-09-10 | Bump addon to 0.9.3 |
| `0.9.4` | `898d9069c5` | 2026-09-11 | Bump addon to 0.9.4 |
| `0.10.0` | `a620f4ce08` | 2026-09-12 | Bump addon to 0.10.0 |
| `0.10.1` | `c15ce2ead9` | 2026-09-15 | Bump addon version to 0.10.1 |
| `0.11.0` | `b6a9a63043` | 2026-09-16 | Bump addon version to 0.11.0 |

## The unsquashed hardening-audit history

The 473 commits of the project-wide hardening audit branch were squash-merged into `Omega` as one commit
(`b191b01`), so they are not ancestors of `Omega`. The per-finding record of what those commits
changed is `docs/OPEN_ITEMS.md`'s Fixed entries, and the tag history cleared on 2026-10-06 is the table above.
