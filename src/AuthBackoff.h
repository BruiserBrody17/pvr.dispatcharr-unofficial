#pragma once

namespace dispatcharr
{

// How long, in seconds, EnsureAuthenticated() should refuse to retry
// Login() after `consecutiveFailures` in a row -- pulled out as a free
// function so the growth/cap behavior is unit-testable without
// DispatcharrClient's own auth-mutex/HTTP machinery. See
// ../tests/test_auth_backoff.cpp.
//
// Exists because EnsureAuthenticated() previously retried Login()
// unconditionally on every call with no memory of a prior failure --
// with wrong or role-incompatible credentials (confirmed live: a
// Dispatcharr "Streamer"-role account, see docs/API_NOTES.md), every
// periodic channel/EPG/recording refresh and every realtime-update
// WebSocket reconnect cycle re-POSTed to /api/accounts/token/
// indefinitely, a real reported way to trip Dispatcharr's own login
// rate-limiter (docs/OPEN_ITEMS.md's "Auth-retry loop" entry).
//
// consecutiveFailures <= 0 (no failure yet, or the most recent attempt
// succeeded) returns 0 -- retry immediately. Otherwise doubles from
// kInitialSeconds, capped at kMaxSeconds, so a transient blip still
// recovers reasonably quickly while a persistent hard failure backs off
// to a long, low-frequency retry instead of a tight loop. Deliberately
// resets only on a successful Login() (DispatcharrClient's own
// responsibility, not this function's) -- fixing the underlying
// credentials/role still needs a Kodi restart per README.md's
// "Configuration" section anyway, which re-zeroes this from a fresh
// process.
int ComputeLoginBackoffSeconds(int consecutiveFailures);

} // namespace dispatcharr
