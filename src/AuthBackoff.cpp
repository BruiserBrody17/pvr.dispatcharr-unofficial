#include "AuthBackoff.h"

#include <algorithm>

namespace dispatcharr
{

int ComputeLoginBackoffSeconds(int consecutiveFailures)
{
  constexpr int kInitialSeconds = 30;
  constexpr int kMaxSeconds = 1800; // 30 minutes
  if (consecutiveFailures <= 0)
    return 0;

  // Cap the shift, not just the final value -- consecutiveFailures could in
  // principle climb arbitrarily high (a long-running install left with bad
  // credentials), and `1 << 60`-ish would overflow long before the
  // kMaxSeconds clamp below gets a chance to apply.
  int shift = std::min(consecutiveFailures - 1, 10);
  long long backoff = static_cast<long long>(kInitialSeconds) << shift;
  return static_cast<int>(std::min<long long>(backoff, kMaxSeconds));
}

} // namespace dispatcharr
