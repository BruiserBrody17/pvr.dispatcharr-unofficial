#include "BoundedParallel.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

using namespace dispatcharr;

TEST_CASE("RunInBoundedBatches runs every index exactly once", "[BoundedParallel]")
{
  std::vector<std::atomic<int>> hits(50);
  RunInBoundedBatches(hits.size(), 16, [&hits](size_t i) { ++hits[i]; });
  for (const auto& h : hits)
    CHECK(h.load() == 1);
}

TEST_CASE("RunInBoundedBatches handles an empty range and a zero batch size", "[BoundedParallel]")
{
  std::atomic<int> calls{0};
  RunInBoundedBatches(0, 16, [&calls](size_t) { ++calls; });
  CHECK(calls == 0);
  RunInBoundedBatches(3, 0, [&calls](size_t) { ++calls; });
  CHECK(calls == 3);
}

TEST_CASE("RunInBoundedBatches never has more than batchSize tasks in flight", "[BoundedParallel]")
{
  std::atomic<int> inFlight{0};
  std::atomic<int> peak{0};
  RunInBoundedBatches(40, 4,
                      [&](size_t)
                      {
                        int now = ++inFlight;
                        int seen = peak.load();
                        while (now > seen && !peak.compare_exchange_weak(seen, now))
                        {
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        --inFlight;
                      });
  CHECK(peak.load() <= 4);
}

TEST_CASE("RunInBoundedBatches falls back to the calling thread when a thread cannot be created", "[BoundedParallel]")
{
  // The inline version let std::system_error out of emplace_back while earlier threads in the batch
  // were still joinable, which is std::terminate.
  for (size_t failAt : {0u, 1u, 5u, 15u})
  {
    size_t created = 0;
    ThreadSpawner spawn = [&created, failAt](std::function<void()> body) -> std::thread
    {
      if (created++ == failAt)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
      return std::thread(std::move(body));
    };
    std::vector<std::atomic<int>> hits(40);
    CHECK_NOTHROW(RunInBoundedBatches(hits.size(), 16, [&hits](size_t i) { ++hits[i]; }, spawn));
    for (size_t i = 0; i < hits.size(); ++i)
      CHECK(hits[i].load() == 1);
  }
}

TEST_CASE("RunInBoundedBatches runs everything serially when no thread can be created", "[BoundedParallel]")
{
  ThreadSpawner spawn = [](std::function<void()>) -> std::thread { throw std::bad_alloc(); };
  std::vector<std::atomic<int>> hits(20);
  CHECK_NOTHROW(RunInBoundedBatches(hits.size(), 16, [&hits](size_t i) { ++hits[i]; }, spawn));
  for (const auto& h : hits)
    CHECK(h.load() == 1);
}

TEST_CASE("RunInBoundedBatches swallows an exception out of a task and carries on", "[BoundedParallel]")
{
  std::vector<std::atomic<int>> hits(10);
  CHECK_NOTHROW(RunInBoundedBatches(hits.size(), 4,
                                    [&hits](size_t i)
                                    {
                                      ++hits[i];
                                      if (i % 3 == 0)
                                        throw std::runtime_error("boom");
                                    }));
  for (const auto& h : hits)
    CHECK(h.load() == 1);
}
