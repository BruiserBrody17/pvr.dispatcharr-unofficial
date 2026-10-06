#include "BoundedParallel.h"

#include <algorithm>
#include <vector>

namespace dispatcharr
{

namespace
{

void RunGuarded(const std::function<void(size_t)>& task, size_t index)
{
  try
  {
    task(index);
  }
  catch (...)
  {
  }
}

} // namespace

void RunInBoundedBatches(size_t count, size_t batchSize, const std::function<void(size_t)>& task,
                         const ThreadSpawner& spawn)
{
  if (batchSize == 0)
    batchSize = 1;
  for (size_t batchStart = 0; batchStart < count; batchStart += batchSize)
  {
    const size_t batchEnd = std::min(batchStart + batchSize, count);
    std::vector<std::thread> batch;
    batch.reserve(batchEnd - batchStart);
    size_t next = batchStart;
    try
    {
      for (; next < batchEnd; ++next)
      {
        const size_t index = next;
        std::function<void()> body = [&task, index]() { RunGuarded(task, index); };
        batch.push_back(spawn ? spawn(std::move(body)) : std::thread(std::move(body)));
      }
    }
    catch (...)
    {
      // `next` is the index whose thread could not be created; everything before it is running.
    }
    for (; next < batchEnd; ++next)
      RunGuarded(task, next);
    for (auto& t : batch)
      t.join();
  }
}

} // namespace dispatcharr
