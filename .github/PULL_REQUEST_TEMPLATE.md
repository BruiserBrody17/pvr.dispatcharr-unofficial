<!--
Thanks for the PR. CONTRIBUTING.md explains each point below. A security fix should be reported privately first
(SECURITY.md), not opened here.
-->

## What this changes and why

## How it was tested

<!--
What you ran (the unit tests, the glue harness) and what you tried against a real Dispatcharr and Kodi, with the
result: the Kodi version and platform, the Dispatcharr version, what you did and what you saw. If you could not test
something end to end, say so.
-->

## Checklist

- [ ] Opened against the branch for its Kodi version: `Omega` for Kodi 21, `Piers` for Kodi 22.
- [ ] Formatted: `clang-format` for C++, `ruff format` and `ruff check` for Python.
- [ ] A bug fix or new logic on the tested side comes with a test in this PR.
- [ ] Docs updated where behaviour changed (`README.md` for users, `docs/` for the why).
- [ ] No real hostname, IP address, account name, time zone or channel name anywhere in the change, its commits or this
      description.
