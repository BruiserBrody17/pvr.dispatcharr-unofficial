#include "EpgTagUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

TEST_CASE("Returns false and leaves genreType untouched when nothing matches", "[EpgTagUtil]")
{
  int genreType = 999;
  REQUIRE_FALSE(MapCategoriesToGenreType({"Unrelated", "Whatever"}, genreType));
  CHECK(genreType == 999);
}

TEST_CASE("Matches a representative keyword for each genre family", "[EpgTagUtil]")
{
  int genreType = 0;

  CHECK(MapCategoriesToGenreType({"Movie"}, genreType));
  CHECK(genreType == 0x10);
  CHECK(MapCategoriesToGenreType({"News"}, genreType));
  CHECK(genreType == 0x20);
  CHECK(MapCategoriesToGenreType({"Comedy"}, genreType));
  CHECK(genreType == 0x30);
  CHECK(MapCategoriesToGenreType({"Sport"}, genreType));
  CHECK(genreType == 0x40);
  CHECK(MapCategoriesToGenreType({"Kids"}, genreType));
  CHECK(genreType == 0x50);
  CHECK(MapCategoriesToGenreType({"Music"}, genreType));
  CHECK(genreType == 0x60);
  CHECK(MapCategoriesToGenreType({"Arts"}, genreType));
  CHECK(genreType == 0x70);
  CHECK(MapCategoriesToGenreType({"Politics"}, genreType));
  CHECK(genreType == 0x80);
  CHECK(MapCategoriesToGenreType({"Documentary"}, genreType));
  CHECK(genreType == 0x90);
  CHECK(MapCategoriesToGenreType({"Travel"}, genreType));
  CHECK(genreType == 0xA0);
}

TEST_CASE("Matching is case-insensitive", "[EpgTagUtil]")
{
  int genreType = 0;
  REQUIRE(MapCategoriesToGenreType({"MOVIE NIGHT"}, genreType));
  CHECK(genreType == 0x10);
}

TEST_CASE("Matches a keyword as a substring, not just a whole category", "[EpgTagUtil]")
{
  int genreType = 0;
  REQUIRE(MapCategoriesToGenreType({"Feature Film"}, genreType));
  CHECK(genreType == 0x10);
}

TEST_CASE("Scans categories in order, not just the first one", "[EpgTagUtil]")
{
  int genreType = 0;
  // "Series" itself matches nothing; the real genre-bearing category comes
  // second, matching this project's own documented reason for scanning all
  // categories rather than stopping at the first.
  REQUIRE(MapCategoriesToGenreType({"Series", "Comedy"}, genreType));
  CHECK(genreType == 0x30);
}

TEST_CASE("ComputeBroadcastId is deterministic for the same inputs", "[EpgTagUtil]")
{
  CHECK(ComputeBroadcastId(42, 1767268800) == ComputeBroadcastId(42, 1767268800));
}

TEST_CASE("ComputeBroadcastId matches a concrete hand-computed value", "[EpgTagUtil]")
{
  // channelUid * 2654435761 + startTime, wrapped mod 2^32 -- computed
  // independently (Python) rather than by re-deriving the formula here, so
  // this locks the exact algorithm in place against an accidental change.
  CHECK(ComputeBroadcastId(42, 1767268800) == 1584421066u);
}

TEST_CASE("ComputeBroadcastId does not collide across a 65536-second gap on the same channel", "[EpgTagUtil]")
{
  // The exact bug this hash was designed to fix (see its own doc comment in
  // EpgTagUtil.h): an earlier version XORed in only startTime & 0xFFFF, so
  // two entries on the same channel exactly 65536s (~18.2h) apart collided.
  time_t t1 = 1767268800;
  time_t t2 = t1 + 65536;
  CHECK(ComputeBroadcastId(7, t1) != ComputeBroadcastId(7, t2));
}

TEST_CASE("ComputeBroadcastId differs across channels for the same start time", "[EpgTagUtil]")
{
  CHECK(ComputeBroadcastId(1, 1767268800) != ComputeBroadcastId(2, 1767268800));
}
