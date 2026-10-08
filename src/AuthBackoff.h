#pragma once

#include <chrono>

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
// rate-limiter (docs/CLOSED_ITEMS.md's "Auth retry loop never gives up or backs off" entry).
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

// Sibling of ComputeLoginBackoffSeconds() above, for
// PVRDispatcharr::StartRealtimeUpdateThread()'s own reconnect wait
// between connection attempts -- a separate, much shorter backoff
// scheme (2s doubling to a 60s cap, vs. Login()'s own 30s-1800s) since a
// dropped WebSocket connection is a routine, expected event (a network
// blip, Dispatcharr restarting) rather than the credentials/role
// problem ComputeLoginBackoffSeconds() guards against.
//
// wokenByNudge is true when the wait was cut short by a deliberate
// OnSystemWake() nudge rather than a natural timeout (see
// m_wakeRealtimeUpdateThread's own comment in PVRDispatcharr.h) --
// resets to kInitialReconnectBackoffSeconds in that case, so a
// wake-from-sleep retries right away instead of continuing to double
// whatever the backoff had already climbed to before sleep. Otherwise
// doubles, capped at kMaxReconnectBackoffSeconds. Pulled out as a free
// function (rather than left inline in the reconnect loop) so this
// decision is unit-testable without a real thread/condition
// variable/socket; see ../tests/test_auth_backoff.cpp.
constexpr int kInitialReconnectBackoffSeconds = 2;
constexpr int kMaxReconnectBackoffSeconds = 60;

int ComputeNextReconnectBackoffSeconds(int currentSeconds, bool wokenByNudge);

// Whether a realtime-update session that just ended (successfully
// connected, then later disconnected for any reason) lasted long enough
// to trust the connection itself as genuinely healthy, resetting
// backoff back to kInitialReconnectBackoffSeconds -- as opposed to
// treating it the same as a failed connect attempt (letting backoff
// keep growing via ComputeNextReconnectBackoffSeconds() instead). Fix
// for a real, confirmed-by-code-reading reconnect-storm risk (not yet
// reproduced live): the reconnect loop used to reset backoff to the
// floor the instant Connect() itself succeeded, before the session
// even had a chance to actually last. A handshake that succeeds but
// drops again almost immediately (a misbehaving reverse proxy, an
// overloaded server dropping connections right after accepting them)
// meant every single reconnect attempt reset backoff back to the floor
// again, so it never actually grew -- reconnecting every
// kInitialReconnectBackoffSeconds forever instead of backing off.
// kMinHealthySessionSeconds (30s) is comfortably longer than a
// handshake-then-immediate-drop, but far shorter than how long a
// genuinely working session actually lasts in practice (hours/days).
constexpr int kMinHealthySessionSeconds = 30;

bool WasSessionHealthy(std::chrono::steady_clock::duration sessionDuration);

// Whether a Login() failure should count toward EnsureAuthenticated()'s
// own consecutive-failure backoff above (ComputeLoginBackoffSeconds()) --
// added 2026-09-27, a 43rd-pass audit, fixing a real, confirmed gap
// found via a project-wide review, confirmed against Dispatcharr's own
// real current upstream source, not itself independently reproduced.
// Before this, EnsureAuthenticated() counted every Login() failure the
// same way regardless of *why* it failed -- but ComputeLoginBackoffSeconds()'s
// own comment is explicit that the backoff exists for a durable,
// retrying-sooner-won't-help failure (wrong or role-incompatible
// credentials), not a transient one. A curl transport failure (no
// response received at all, httpCode == 0 -- Dispatcharr unreachable,
// still starting up, or a network path not up yet) or a 5xx (Dispatcharr
// itself erroring) are exactly the transient case: without this
// distinction, an extended outage escalated the same backoff meant for
// a credentials lockout, so EnsureAuthenticated() could still refuse to
// even attempt Login() again for up to kMaxSeconds (30 minutes) after
// Dispatcharr had already recovered -- nothing about a network/server
// outage justifies that wait once connectivity is actually restored.
// 400/401/403/429 are genuine, durable rejections confirmed against
// TokenObtainPairView.post() (apps/accounts/api_views.py): 403 for its
// own network_access_allowed() policy block or a re-raised SimpleJWT
// validation error class DRF maps to 400, 401 for SimpleJWT's own wrong-
// credentials AuthenticationFailed, and 429 from its own
// LoginRateThrottle -- the exact rate-limiter this whole backoff exists
// to avoid tripping (docs/CLOSED_ITEMS.md's "Auth retry loop never gives up or backs off" entry), so a
// 429 is if anything a stronger signal to back off, not a reason to
// treat it as transient.
inline bool ShouldCountTowardLoginBackoff(long httpCode)
{
  return httpCode == 400 || httpCode == 401 || httpCode == 403 || httpCode == 429;
}

// What EnsureAuthenticated() does after RefreshAccessToken() fails, by HTTP
// status (0 == no response at all). Three outcomes, each with its own
// history in this codebase (the 46th/47th-pass audits, 2026-09-27):
//   kRejected  -- 401/403: the refresh token itself is dead (expired, revoked,
//                 or Dispatcharr's own TokenRefreshView answering 403 for its
//                 network policy). Forget it and fall through to a full Login()
//                 for a fresh pair, with no transient cooldown -- this is not an
//                 outage.
//   kTransient -- no response or a 5xx: the same class
//                 ShouldCountTowardLoginBackoff() carves out for Login(). Apply
//                 the short transient cooldown and stop, rather than making a
//                 second full blocking call against a host that just failed to
//                 answer the first one.
//   kOther     -- anything else (a malformed body, an unexpected 4xx): fall
//                 through to Login() as always, keeping the refresh token.
// Pulled out of EnsureAuthenticated() so the mapping is pinned by a test
// instead of resting on three inline conditions staying in agreement.
enum class RefreshTokenFailure
{
  kRejected,
  kTransient,
  kOther,
};

inline RefreshTokenFailure ClassifyRefreshTokenFailure(long httpCode)
{
  if (httpCode == 401 || httpCode == 403)
    return RefreshTokenFailure::kRejected;
  if (httpCode == 0 || (httpCode >= 500 && httpCode < 600))
    return RefreshTokenFailure::kTransient;
  return RefreshTokenFailure::kOther;
}

// Whether the short transient-failure cooldown turns an authentication attempt away. A cooldown armed by a call
// that only waited the short bound (ShortRequestTimeoutMs()) proves little about a server that is merely slow: a
// caller that is allowed the full connection timeout (an open, the guide fetch) goes ahead, where one armed by a
// full-length attempt or by the startup check's deferral (which exists to keep Kodi's own threads from waiting on a
// dead server) turns everyone away. A short-bounded caller is always turned away while a cooldown runs, so a stream's
// refresh, twice a second, cannot hammer a token endpoint slower than its bound or starve the others.
inline bool IsTransientCooldownBlocking(bool cooldownActive, bool armedByShortAttempt, bool callerHasShortBound)
{
  return cooldownActive && (!armedByShortAttempt || callerHasShortBound);
}

} // namespace dispatcharr
