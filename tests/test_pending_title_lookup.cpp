#include "PendingTitleLookup.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace dispatcharr;

namespace
{
// Minimal stand-in for DispatcharrClient::PendingTitle -- both functions
// here are templated purely on recordingId/insertedAt members.
struct FakeEntry
{
  int recordingId;
  std::string title;
  std::chrono::steady_clock::time_point insertedAt;
};
} // namespace

// ---------------------------------------------------------------------
// PruneExpiredPendingTitles
// ---------------------------------------------------------------------

TEST_CASE("PruneExpiredPendingTitles drops entries older than the TTL", "[PendingTitleLookup]")
{
  auto now = std::chrono::steady_clock::now();
  std::vector<FakeEntry> entries = {
      {1, "Old", now - std::chrono::minutes(5)},
      {2, "Fresh", now - std::chrono::minutes(1)},
  };

  PruneExpiredPendingTitles(entries, now, std::chrono::minutes(3));

  REQUIRE(entries.size() == 1);
  CHECK(entries[0].title == "Fresh");
}

TEST_CASE("PruneExpiredPendingTitles keeps an entry exactly at the TTL boundary", "[PendingTitleLookup]")
{
  auto now = std::chrono::steady_clock::now();
  std::vector<FakeEntry> entries = {{1, "Boundary", now - std::chrono::minutes(3)}};

  PruneExpiredPendingTitles(entries, now, std::chrono::minutes(3));

  CHECK(entries.size() == 1); // age == ttl is not > ttl, so it survives
}

TEST_CASE("PruneExpiredPendingTitles is a no-op on an empty list", "[PendingTitleLookup]")
{
  std::vector<FakeEntry> entries;
  PruneExpiredPendingTitles(entries, std::chrono::steady_clock::now(), std::chrono::minutes(3));
  CHECK(entries.empty());
}

// ---------------------------------------------------------------------
// FindPendingTitleForRecording
// ---------------------------------------------------------------------

TEST_CASE("FindPendingTitleForRecording returns nullptr with no matching recording id", "[PendingTitleLookup]")
{
  auto now = std::chrono::steady_clock::now();
  std::vector<FakeEntry> entries = {{1, "Other", now}};
  CHECK(FindPendingTitleForRecording(entries, 2) == nullptr);
}

TEST_CASE("FindPendingTitleForRecording matches only the exact recording id, not another recording on the same "
          "channel -- the real bug this fixes",
          "[PendingTitleLookup]")
{
  // Before this, a lookup keyed by channel alone would have let recording
  // 7's own in-flight title leak into a completely unrelated recording 5
  // on the same channel. Keyed by recording id, each entry only ever
  // matches its own recording.
  auto now = std::chrono::steady_clock::now();
  std::vector<FakeEntry> entries = {
      {5, "Recording 5's own title", now - std::chrono::minutes(2)},
      {7, "Recording 7's own title", now - std::chrono::seconds(10)},
  };

  const FakeEntry* result = FindPendingTitleForRecording(entries, 7);
  REQUIRE(result != nullptr);
  CHECK(result->title == "Recording 7's own title");
}

TEST_CASE("FindPendingTitleForRecording ignores entries for other recording ids", "[PendingTitleLookup]")
{
  auto now = std::chrono::steady_clock::now();
  std::vector<FakeEntry> entries = {{1, "Recording one", now}, {2, "Recording two", now}};

  const FakeEntry* result = FindPendingTitleForRecording(entries, 2);
  REQUIRE(result != nullptr);
  CHECK(result->title == "Recording two");
}

TEST_CASE("FindPendingTitleForRecording returns nullptr for an empty list", "[PendingTitleLookup]")
{
  std::vector<FakeEntry> entries;
  CHECK(FindPendingTitleForRecording(entries, 1) == nullptr);
}
