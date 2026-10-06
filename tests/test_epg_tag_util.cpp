#include "EpgTagUtil.h"

#include <catch2/catch_test_macros.hpp>
#include <clocale>
#include <string>

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

// ---------------------------------------------------------------------
// ShouldIncludeEpisodeDates
// ---------------------------------------------------------------------

TEST_CASE("ShouldIncludeEpisodeDates is true with a real season or episode number", "[EpgTagUtil]")
{
  CHECK(ShouldIncludeEpisodeDates(1, 5));
  CHECK(ShouldIncludeEpisodeDates(1, 0));
  CHECK(ShouldIncludeEpisodeDates(0, 5));
}

TEST_CASE("ShouldIncludeEpisodeDates is false with no episode identity at all", "[EpgTagUtil]")
{
  // The exact documented case: a long-running daily show whose guide
  // source stamps an identical placeholder <date> on every airing.
  CHECK_FALSE(ShouldIncludeEpisodeDates(-1, -1));
  CHECK_FALSE(ShouldIncludeEpisodeDates(0, 0));
}

TEST_CASE("MapCategoriesToGenreType doesn't match a keyword buried inside a longer word", "[EpgTagUtil]")
{
  // "Darts" contains "arts" but isn't Arts/Culture (docs/OPEN_ITEMS.md,
  // 23rd-pass audit).
  int genreType = -1;
  CHECK_FALSE(MapCategoriesToGenreType({"Darts"}, genreType));
}

TEST_CASE("MapCategoriesToGenreType still matches a keyword as a word prefix", "[EpgTagUtil]")
{
  int genreType = 0;
  REQUIRE(MapCategoriesToGenreType({"Sports"}, genreType));
  CHECK(genreType == 0x40);
  REQUIRE(MapCategoriesToGenreType({"Sportscast"}, genreType));
  CHECK(genreType == 0x40);
  REQUIRE(MapCategoriesToGenreType({"Live/Sports"}, genreType));
  CHECK(genreType == 0x40);
}

TEST_CASE("MapCategoriesToGenreType maps Martial arts to Sports, not Arts/Culture", "[EpgTagUtil]")
{
  int genreType = 0;
  REQUIRE(MapCategoriesToGenreType({"Martial arts"}, genreType));
  CHECK(genreType == 0x40);
}

// ---------------------------------------------------------------------
// CategoriesIndicateSeries
// ---------------------------------------------------------------------

TEST_CASE("CategoriesIndicateSeries matches a literal Series category", "[EpgTagUtil]")
{
  CHECK(CategoriesIndicateSeries({"Series"}));
  CHECK(CategoriesIndicateSeries({"Comedy", "Series"}));
}

TEST_CASE("CategoriesIndicateSeries is case-insensitive, matching MapCategoriesToGenreType", "[EpgTagUtil]")
{
  CHECK(CategoriesIndicateSeries({"series"}));
  CHECK(CategoriesIndicateSeries({"SERIES"}));
}

TEST_CASE("CategoriesIndicateSeries is false with no matching category", "[EpgTagUtil]")
{
  CHECK_FALSE(CategoriesIndicateSeries({"Comedy", "News"}));
  CHECK_FALSE(CategoriesIndicateSeries({}));
}

// ---------------------------------------------------------------------
// JoinCategories
// ---------------------------------------------------------------------

TEST_CASE("JoinCategories joins multiple entries with the separator", "[EpgTagUtil]")
{
  CHECK(JoinCategories({"Comedy", "Series"}, ",") == "Comedy,Series");
}

TEST_CASE("JoinCategories does not add a leading separator for a single entry", "[EpgTagUtil]")
{
  CHECK(JoinCategories({"Comedy"}, ",") == "Comedy");
}

TEST_CASE("JoinCategories returns empty for no categories", "[EpgTagUtil]")
{
  CHECK(JoinCategories({}, ",") == "");
}

// ---------------------------------------------------------------------
// ComputeEpgTagFlags
// ---------------------------------------------------------------------

TEST_CASE("ComputeEpgTagFlags sets no bits when nothing applies", "[EpgTagUtil]")
{
  CHECK(ComputeEpgTagFlags(false, false, false, 0, 0, false) == 0u);
}

TEST_CASE("ComputeEpgTagFlags sets IS_NEW, IS_PREMIERE, and IS_LIVE independently", "[EpgTagUtil]")
{
  CHECK(ComputeEpgTagFlags(true, false, false, 0, 0, false) == (1u << 1));
  CHECK(ComputeEpgTagFlags(false, true, false, 0, 0, false) == (1u << 2));
  CHECK(ComputeEpgTagFlags(false, false, true, 0, 0, false) == (1u << 4));
}

TEST_CASE("ComputeEpgTagFlags sets IS_SERIES from a season or episode number", "[EpgTagUtil]")
{
  CHECK(ComputeEpgTagFlags(false, false, false, 1, 0, false) == (1u << 0));
  CHECK(ComputeEpgTagFlags(false, false, false, 0, 3, false) == (1u << 0));
}

TEST_CASE("ComputeEpgTagFlags sets IS_SERIES from categorySaysSeries alone", "[EpgTagUtil]")
{
  // The backstop case: a "Series" category but no season/episode number of
  // its own -- see CategoriesIndicateSeries()'s own doc comment.
  CHECK(ComputeEpgTagFlags(false, false, false, 0, 0, true) == (1u << 0));
}

TEST_CASE("ComputeEpgTagFlags combines multiple flags", "[EpgTagUtil]")
{
  CHECK(ComputeEpgTagFlags(true, false, true, 1, 0, false) == ((1u << 1) | (1u << 4) | (1u << 0)));
}

// ---------------------------------------------------------------------
// IsWithinCatchupWindow
// ---------------------------------------------------------------------

TEST_CASE("IsWithinCatchupWindow is false when catch-up is not enabled", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  CHECK_FALSE(IsWithinCatchupWindow(/*catchupEnabled=*/false, /*catchupDays=*/7, now - 3600, now));
}

TEST_CASE("IsWithinCatchupWindow falls back to a default window for a non-positive (unknown) catchupDays",
          "[EpgTagUtil]")
{
  // A non-positive catchupDays is Dispatcharr's own "archive depth
  // unknown", not "no catch-up" -- confirmed against Dispatcharr's own
  // real current upstream source, not itself independently reproduced.
  // The real bug this fixes: before this, a channel in exactly this
  // state (is_catchup=true, catchup_days=0 -- a real, confirmed state a
  // provider that sets tv_archive=1 without tv_archive_duration leaves a
  // channel in) reported hasarchive=true to Kodi while every guide entry
  // on it was refused here, even though the server would have actually
  // served it (session creation server-side never checks catchup_days
  // at all).
  time_t now = 1767268800;
  time_t oneDayAgo = now - 24 * 60 * 60;
  CHECK(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/0, oneDayAgo, now));
  CHECK(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/-1, oneDayAgo, now));
}

TEST_CASE("IsWithinCatchupWindow's unknown-catchupDays fallback still ages out past its own default window",
          "[EpgTagUtil]")
{
  // The fallback is a default retention window, not "always playable" --
  // matches Dispatcharr's own migration 0038 backfill default of 7 days
  // for a missing/unknown archive duration.
  time_t now = 1767268800;
  time_t eightDaysAgo = now - 8 * 24 * 60 * 60;
  CHECK_FALSE(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/0, eightDaysAgo, now));
}

TEST_CASE("IsWithinCatchupWindow is false for a tag that hasn't aired yet", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  CHECK_FALSE(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/7, now + 3600, now));
}

TEST_CASE("IsWithinCatchupWindow is true for a tag that already aired and started exactly now", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  CHECK(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/7, now, now));
}

TEST_CASE("IsWithinCatchupWindow is true well within the retention window", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  time_t oneDayAgo = now - 24 * 60 * 60;
  CHECK(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/7, oneDayAgo, now));
}

TEST_CASE("IsWithinCatchupWindow is false once older than the retention window", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  time_t eightDaysAgo = now - 8 * 24 * 60 * 60;
  CHECK_FALSE(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/7, eightDaysAgo, now));
}

TEST_CASE("IsWithinCatchupWindow is true exactly at the retention boundary", "[EpgTagUtil]")
{
  time_t now = 1767268800;
  time_t exactlySevenDaysAgo = now - 7 * 24 * 60 * 60;
  CHECK(IsWithinCatchupWindow(/*catchupEnabled=*/true, /*catchupDays=*/7, exactlySevenDaysAgo, now));
}

// ---------------------------------------------------------------------
// ShouldOfferCatchup
// ---------------------------------------------------------------------

TEST_CASE("ShouldOfferCatchup is true when the channel, global setting, and current user all allow it", "[EpgTagUtil]")
{
  CHECK(ShouldOfferCatchup(/*channelCatchupEnabled=*/true, /*catchupEnabledGlobally=*/true,
                           /*catchupEnabledForCurrentUser=*/true));
}

TEST_CASE("ShouldOfferCatchup is false when the channel itself doesn't support catch-up", "[EpgTagUtil]")
{
  CHECK_FALSE(ShouldOfferCatchup(/*channelCatchupEnabled=*/false, /*catchupEnabledGlobally=*/true,
                                 /*catchupEnabledForCurrentUser=*/true));
}

TEST_CASE("ShouldOfferCatchup is false when catch-up is disabled globally, even on a catch-up-capable channel",
          "[EpgTagUtil]")
{
  // The exact scenario confirmed live 2026-09-28 (docs/OPEN_ITEMS.md): Kodi
  // used to keep offering "Play" for an already-aired programme even though
  // Dispatcharr's own server-side gate would 403 every attempt.
  CHECK_FALSE(ShouldOfferCatchup(/*channelCatchupEnabled=*/true, /*catchupEnabledGlobally=*/false,
                                 /*catchupEnabledForCurrentUser=*/true));
}

TEST_CASE("ShouldOfferCatchup is false when catch-up is disabled for the current user specifically", "[EpgTagUtil]")
{
  CHECK_FALSE(ShouldOfferCatchup(/*channelCatchupEnabled=*/true, /*catchupEnabledGlobally=*/true,
                                 /*catchupEnabledForCurrentUser=*/false));
}

// ---------------------------------------------------------------------
// ComputeGuidePrevDays
// ---------------------------------------------------------------------

TEST_CASE("ComputeGuidePrevDays asks for nothing when no channel offers catch-up", "[EpgTagUtil]")
{
  CHECK(ComputeGuidePrevDays({}) == 0);
}

TEST_CASE("ComputeGuidePrevDays treats a catch-up channel with no stated depth as the default window", "[EpgTagUtil]")
{
  // Every entry is a channel that does offer catch-up; 0 (or less) is "the server
  // did not say how far back", which IsWithinCatchupWindow() already reads as 7.
  CHECK(ComputeGuidePrevDays({0}) == 7);
  CHECK(ComputeGuidePrevDays({-1}) == 7);
  CHECK(ComputeGuidePrevDays({0, 0}) == 7);
  CHECK(ComputeGuidePrevDays({0, 3}) == 7);   // unknown counts as 7, which beats a stated 3
  CHECK(ComputeGuidePrevDays({0, 10}) == 10); // but not a longer stated window
}

TEST_CASE("ComputeGuidePrevDays asks for the longest window any channel offers", "[EpgTagUtil]")
{
  CHECK(ComputeGuidePrevDays({3}) == 3);
  CHECK(ComputeGuidePrevDays({1, 7, 3}) == 7);
}

TEST_CASE("ComputeGuidePrevDays never asks for more than the server's own cap", "[EpgTagUtil]")
{
  CHECK(ComputeGuidePrevDays({30}) == 30);
  CHECK(ComputeGuidePrevDays({31}) == 30);
  CHECK(ComputeGuidePrevDays({365, 3}) == 30);
}

TEST_CASE("A genre keyword counts only at a word start, and a later word-start occurrence still matches",
          "[EpgTagUtil]")
{
  int genreType = 0;
  // "movie" only inside a word: no match.
  CHECK_FALSE(MapCategoriesToGenreType({"xmovie"}, genreType));
  CHECK_FALSE(MapCategoriesToGenreType({"Darts"}, genreType));
  // The first occurrence is mid-word, the second starts a word: the scan must carry on to it.
  CHECK(MapCategoriesToGenreType({"xmovie movie"}, genreType));
  CHECK(MapCategoriesToGenreType({"Sports"}, genreType));
  CHECK(MapCategoriesToGenreType({"live sport"}, genreType));
}

TEST_CASE("category keyword matching does not depend on the process locale", "[EpgTagUtil]")
{
  // Kodi sets LC_CTYPE to the user's region; under a Turkish locale std::tolower('I') is not 'i', so
  // "SERIES" stopped matching "series" (found by the thirteenth hardening sweep). Skipped where the
  // locale is not installed (the lab host has only C), but the ASCII helpers' own test above pins
  // the behaviour for every byte regardless.
  const char* saved = setlocale(LC_CTYPE, nullptr);
  const std::string savedName = saved ? saved : "C";
  bool switched = false;
  for (const char* name : {"tr_TR.UTF-8", "tr_TR.utf8", "tr_TR", "az_AZ.UTF-8"})
  {
    if (setlocale(LC_CTYPE, name))
    {
      switched = true;
      break;
    }
  }
  const bool series = CategoriesIndicateSeries({"SERIES"});
  const bool sitcom = CategoriesIndicateSeries({"TV SERIES"});
  setlocale(LC_CTYPE, savedName.c_str());
  CHECK(series);
  CHECK(sitcom);
  if (!switched)
    WARN("no Turkish locale installed; only the C locale was exercised");
}
