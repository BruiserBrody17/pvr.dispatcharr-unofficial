#include "RecordedStreamRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace dispatcharr;

namespace
{
struct Item
{
  int value = 0;
};
} // namespace

TEST_CASE("RecordedStreamRegistry issues ids from 1 and never reuses one", "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  const int64_t a = registry.Add(std::make_shared<Item>());
  const int64_t b = registry.Add(std::make_shared<Item>());
  CHECK(a == 1);
  CHECK(b == 2);

  registry.Remove(b);
  // A stale id must never reach a later stream, so the freed number is not handed out again.
  const int64_t c = registry.Add(std::make_shared<Item>());
  CHECK(c == 3);
}

TEST_CASE("RecordedStreamRegistry finds what was added and nothing else", "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  auto item = std::make_shared<Item>();
  item->value = 7;
  const int64_t id = registry.Add(item);

  CHECK(registry.Find(id) == item);
  CHECK(registry.Find(id + 1) == nullptr);
  // 0 and negatives are what Kodi uses for "no stream".
  CHECK(registry.Find(0) == nullptr);
  CHECK(registry.Find(-1) == nullptr);
}

TEST_CASE("RecordedStreamRegistry Remove is idempotent", "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  const int64_t id = registry.Add(std::make_shared<Item>());

  CHECK(registry.Remove(id) != nullptr);
  CHECK(registry.Remove(id) == nullptr);
  CHECK(registry.Find(id) == nullptr);
  CHECK(registry.size() == 0);
  CHECK(registry.Remove(12345) == nullptr);
}

TEST_CASE("RecordedStreamRegistry keeps a removed stream alive for a holder that already found it",
          "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  std::weak_ptr<Item> weak;
  std::shared_ptr<Item> held;
  {
    auto item = std::make_shared<Item>();
    weak = item;
    const int64_t id = registry.Add(item);
    held = registry.Find(id);
    registry.Remove(id);
  }
  // A read still running on the stream that Close just removed must not see it freed under it.
  CHECK_FALSE(weak.expired());
  held.reset();
  CHECK(weak.expired());
}

TEST_CASE("RecordedStreamRegistry TakeAll empties the registry and returns every stream", "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  registry.Add(std::make_shared<Item>());
  registry.Add(std::make_shared<Item>());
  registry.Add(std::make_shared<Item>());
  CHECK(registry.size() == 3);

  auto all = registry.TakeAll();
  CHECK(all.size() == 3);
  CHECK(registry.size() == 0);
  CHECK(registry.TakeAll().empty());
}

TEST_CASE("RecordedStreamRegistry survives concurrent adds, finds and removes", "[RecordedStreamRegistry]")
{
  RecordedStreamRegistry<Item> registry;
  std::mutex idsMutex;
  std::set<int64_t> issued;
  std::atomic<bool> duplicate{false};

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
  {
    threads.emplace_back(
        [&]()
        {
          for (int i = 0; i < 500; ++i)
          {
            const int64_t id = registry.Add(std::make_shared<Item>());
            {
              std::lock_guard<std::mutex> lock(idsMutex);
              if (!issued.insert(id).second)
                duplicate = true;
            }
            registry.Find(id);
            registry.Remove(id);
            registry.Remove(id);
          }
        });
  }
  for (auto& thread : threads)
    thread.join();

  CHECK_FALSE(duplicate.load());
  CHECK(registry.size() == 0);
  CHECK(issued.size() == 2000);
}
