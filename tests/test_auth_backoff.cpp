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
