#include "StreamSeek.h"

#include <cstdio>
#include <limits>

namespace dispatcharr
{

int64_t ResolveSeekPosition(int64_t position, int whence, int64_t currentPosition, int64_t referenceLength)
{
  int64_t base;
  switch (whence)
  {
  case SEEK_SET:
    base = 0;
    break;
  case SEEK_CUR:
    base = currentPosition;
    break;
  case SEEK_END:
    if (referenceLength < 0)
      return -1;
    base = referenceLength;
    break;
  default:
    return -1;
  }
  // Checked before adding, not after -- signed overflow is undefined
  // behavior in C++, so an after-the-fact "result < 0" check can't be
  // relied on to catch it. See this function's own header comment.
  if ((position > 0 && base > std::numeric_limits<int64_t>::max() - position) ||
      (position < 0 && base < std::numeric_limits<int64_t>::min() - position))
    return -1;
  int64_t newPos = base + position;
  if (newPos < 0)
    return -1;
  return newPos;
}

bool ComputeReadRangeEnd(int64_t position, unsigned int size, int64_t& rangeEndOut)
{
  if (position < 0 || size == 0)
    return false;
  if (position > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(size))
    return false;
  rangeEndOut = position + static_cast<int64_t>(size) - 1;
  return true;
}

} // namespace dispatcharr
