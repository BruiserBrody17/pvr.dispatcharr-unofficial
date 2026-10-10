#include "AuthBackoff.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("ComputeLoginBackoffSeconds is zero with no failure yet", "[AuthBackoff]")
{
  CHECK(ComputeLoginBackoffSeconds(0) == 0);
  CHECK(ComputeLoginBackoffSeconds(-1) == 0);
}

TEST_CASE("ComputeLoginBackoffSeconds starts at the initial backoff on the first failure", "[AuthBackoff]")
{
  CHECK(ComputeLoginBackoffSeconds(1) == 30);
}

TEST_CASE("ComputeLoginBackoffSeconds doubles per additional consecutive failure", "[AuthBackoff]")
{
  CHECK(ComputeLoginBackoffSeconds(2) == 60);
  CHECK(ComputeLoginBackoffSeconds(3) == 120);
  CHECK(ComputeLoginBackoffSeconds(4) == 240);
}

TEST_CASE("ComputeLoginBackoffSeconds caps at 30 minutes", "[AuthBackoff]")
{
  CHECK(ComputeLoginBackoffSeconds(7) == 1800); // 30 * 2^6 = 1920, clamped
  CHECK(ComputeLoginBackoffSeconds(8) == 1800);
}

TEST_CASE("ComputeLoginBackoffSeconds never overflows for a very large failure count", "[AuthBackoff]")
{
  CHECK(ComputeLoginBackoffSeconds(1000000) == 1800);
}

// ---------------------------------------------------------------------
// ComputeNextReconnectBackoffSeconds
// ---------------------------------------------------------------------

TEST_CASE("ComputeNextReconnectBackoffSeconds doubles the current backoff on a natural timeout", "[AuthBackoff]")
{
  CHECK(ComputeNextReconnectBackoffSeconds(2, /*wokenByNudge=*/false) == 4);
  CHECK(ComputeNextReconnectBackoffSeconds(4, /*wokenByNudge=*/false) == 8);
  CHECK(ComputeNextReconnectBackoffSeconds(32, /*wokenByNudge=*/false) == 60); // capped, not 64
}

TEST_CASE("ComputeNextReconnectBackoffSeconds stays capped at 60 once already there", "[AuthBackoff]")
{
  CHECK(ComputeNextReconnectBackoffSeconds(60, /*wokenByNudge=*/false) == 60);
}

TEST_CASE("ComputeNextReconnectBackoffSeconds resets to the initial value on a wake nudge", "[AuthBackoff]")
{
  // The exact documented reason: retry right away after waking from
  // sleep instead of continuing to double whatever it had already
  // climbed to before sleep.
  CHECK(ComputeNextReconnectBackoffSeconds(60, /*wokenByNudge=*/true) == kInitialReconnectBackoffSeconds);
  CHECK(ComputeNextReconnectBackoffSeconds(2, /*wokenByNudge=*/true) == kInitialReconnectBackoffSeconds);
}

// ---------------------------------------------------------------------
// WasSessionHealthy
// ---------------------------------------------------------------------

TEST_CASE("WasSessionHealthy is false for a session shorter than the minimum", "[AuthBackoff]")
{
  CHECK_FALSE(WasSessionHealthy(std::chrono::milliseconds(100)));
  CHECK_FALSE(WasSessionHealthy(std::chrono::seconds(29)));
}

TEST_CASE("WasSessionHealthy is true at exactly the minimum duration", "[AuthBackoff]")
{
  CHECK(WasSessionHealthy(std::chrono::seconds(kMinHealthySessionSeconds)));
}

TEST_CASE("WasSessionHealthy is true for a long-lived session", "[AuthBackoff]")
{
  CHECK(WasSessionHealthy(std::chrono::hours(2)));
}

// ---------------------------------------------------------------------
// ShouldCountTowardLoginBackoff
// ---------------------------------------------------------------------

TEST_CASE("ShouldCountTowardLoginBackoff is true for a genuine credential/authorization rejection", "[AuthBackoff]")
{
  CHECK(ShouldCountTowardLoginBackoff(400)); // malformed request / SimpleJWT validation error
  CHECK(ShouldCountTowardLoginBackoff(401)); // wrong credentials
  CHECK(ShouldCountTowardLoginBackoff(403)); // network-policy block or role-incompatible account
  CHECK(ShouldCountTowardLoginBackoff(429)); // Dispatcharr's own LoginRateThrottle
}

TEST_CASE("ShouldCountTowardLoginBackoff is false for a transport failure or a server error -- the real fix",
          "[AuthBackoff]")
{
  // The exact documented bug: without this distinction, an extended
  // Dispatcharr outage escalated the same backoff meant for a
  // credentials lockout.
  CHECK_FALSE(ShouldCountTowardLoginBackoff(0)); // no response received at all
  CHECK_FALSE(ShouldCountTowardLoginBackoff(500));
  CHECK_FALSE(ShouldCountTowardLoginBackoff(502));
  CHECK_FALSE(ShouldCountTowardLoginBackoff(503));
}

TEST_CASE("ShouldCountTowardLoginBackoff is false for an unrelated 2xx/3xx/4xx status", "[AuthBackoff]")
{
  CHECK_FALSE(ShouldCountTowardLoginBackoff(200));
  CHECK_FALSE(ShouldCountTowardLoginBackoff(404));
}

// ---------------------------------------------------------------------
// ClassifyRefreshTokenFailure
// ---------------------------------------------------------------------

TEST_CASE("ClassifyRefreshTokenFailure treats 401/403 as a dead refresh token", "[AuthBackoff]")
{
  CHECK(ClassifyRefreshTokenFailure(401) == RefreshTokenFailure::kRejected);
  CHECK(ClassifyRefreshTokenFailure(403) == RefreshTokenFailure::kRejected); // TokenRefreshView's network policy
}

TEST_CASE("ClassifyRefreshTokenFailure treats no response or a 5xx as transient -- the 47th-pass fix", "[AuthBackoff]")
{
  // Before this, a mid-session outage bypassed both login gates through the
  // refresh attempt; a transient failure now sets the same short cooldown.
  CHECK(ClassifyRefreshTokenFailure(0) == RefreshTokenFailure::kTransient);
  CHECK(ClassifyRefreshTokenFailure(500) == RefreshTokenFailure::kTransient);
  CHECK(ClassifyRefreshTokenFailure(502) == RefreshTokenFailure::kTransient);
  CHECK(ClassifyRefreshTokenFailure(599) == RefreshTokenFailure::kTransient);
}

TEST_CASE("ClassifyRefreshTokenFailure falls through to a login for anything else", "[AuthBackoff]")
{
  CHECK(ClassifyRefreshTokenFailure(400) == RefreshTokenFailure::kOther);
  CHECK(ClassifyRefreshTokenFailure(404) == RefreshTokenFailure::kOther);
  CHECK(ClassifyRefreshTokenFailure(429) == RefreshTokenFailure::kOther);
  CHECK(ClassifyRefreshTokenFailure(200) == RefreshTokenFailure::kOther); // a 2xx with an unusable body
  CHECK(ClassifyRefreshTokenFailure(600) == RefreshTokenFailure::kOther);
}

TEST_CASE("IsTransientCooldownBlocking turns a short-bounded caller away for as long as the cooldown runs",
          "[AuthBackoff]")
{
  using dispatcharr::IsTransientCooldownBlocking;
  // No cooldown, nobody is turned away.
  CHECK_FALSE(IsTransientCooldownBlocking(false, false, false));
  CHECK_FALSE(IsTransientCooldownBlocking(false, true, true));
  // A cooldown armed by a full-length attempt (or the startup deferral) turns everyone away.
  CHECK(IsTransientCooldownBlocking(true, false, false));
  CHECK(IsTransientCooldownBlocking(true, false, true));
  // One armed by a short-bounded attempt turns short-bounded callers away, so a stream's refresh cannot hammer the
  // token endpoint, but lets a caller that may wait the whole connection timeout (an open) try: a server that merely
  // answers slowly gets its chance.
  CHECK(IsTransientCooldownBlocking(true, true, true));
  CHECK_FALSE(IsTransientCooldownBlocking(true, true, false));
}

TEST_CASE("DecideAuthGate lets a valid token through whatever else is armed", "[AuthBackoff]")
{
  for (bool backoff : {false, true})
    for (bool cooldown : {false, true})
      CHECK(DecideAuthGate(true, backoff, cooldown) == AuthGate::kHaveToken);
}

TEST_CASE("DecideAuthGate applies the login backoff before the transient cooldown", "[AuthBackoff]")
{
  CHECK(DecideAuthGate(false, true, true) == AuthGate::kLoginBackoff);
  CHECK(DecideAuthGate(false, true, false) == AuthGate::kLoginBackoff);
  CHECK(DecideAuthGate(false, false, true) == AuthGate::kTransientCooldown);
}

TEST_CASE("DecideAuthGate proceeds to a refresh or login only when no gate applies", "[AuthBackoff]")
{
  // The 47th-pass storm: the cooldown must be a gate BEFORE the refresh attempt, so "proceed" is reached only with
  // no token, no backoff and no blocking cooldown.
  CHECK(DecideAuthGate(false, false, false) == AuthGate::kProceed);
}
