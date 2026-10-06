#include "ChannelRenumbering.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;

namespace
{
Channel MakeChannel(int id, double channelNumber)
{
  Channel ch;
  ch.id = id;
  ch.channelNumber = channelNumber;
  return ch;
}
Channel MakeUnnumbered(int id)
{
  Channel ch;
  ch.id = id;
  ch.hasChannelNumber = false;
  return ch;
}
} // namespace

TEST_CASE("HaveChannelNumbersChanged is false on a first load (empty oldChannels)", "[ChannelRenumbering]")
{
  std::vector<Channel> newChannels = {MakeChannel(1, 5)};
  CHECK_FALSE(HaveChannelNumbersChanged({}, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is false when nothing changed", "[ChannelRenumbering]")
{
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 6)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5), MakeChannel(2, 6)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is true when a shared channel's number shifted -- the real bug this fixes",
          "[ChannelRenumbering]")
{
  // The exact scenario this guards against: hiding channel 1 in a
  // compact-numbered group shifts channel 2 down to fill the gap.
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 6)};
  std::vector<Channel> newChannels = {MakeChannel(2, 5)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged ignores a channel that only appears in one list", "[ChannelRenumbering]")
{
  // Also the "genuinely new channel number never seen before" case --
  // deliberately not flagged, since there's nothing to reuse: "no guide
  // yet", not "wrong guide".
  std::vector<Channel> oldChannels = {MakeChannel(1, 5)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5), MakeChannel(2, 6)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is false when a channel disappears without any remaining one renumbering",
          "[ChannelRenumbering]")
{
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 6)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is true when a new channel id reuses a deleted channel's number -- the real "
          "bug this fixes",
          "[ChannelRenumbering]")
{
  // Deleting channel id 1 (number 5) and creating a brand-new channel
  // id 2 that gets handed the same now-free number 5
  // (get_next_available_channel_number() hands out the lowest free
  // one) shares no id between the two snapshots at all -- an id-only
  // comparison misses this, even though it's the identical
  // wrong-guide-shown symptom the id-based check already catches.
  std::vector<Channel> oldChannels = {MakeChannel(1, 5)};
  std::vector<Channel> newChannels = {MakeChannel(2, 5)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is false when two channels stably share the same number -- the real "
          "regression this fixes",
          "[ChannelRenumbering]")
{
  // Dispatcharr genuinely permits two channels sharing a channel_number.
  // An earlier version of this function stored only the *last* id seen
  // for a given number, so an unchanged channel among a stable pair
  // sharing a number could spuriously read as "reused by a different
  // id" against the other one's own id -- forcing a full EPG re-fetch
  // on every single successful channel refresh, not just around an
  // actual renumbering.
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 5)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5), MakeChannel(2, 5)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged still catches one of two same-numbered channels genuinely being reassigned",
          "[ChannelRenumbering]")
{
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 5)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5), MakeChannel(2, 7)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged ignores several channels stably sharing a literal 0 or no number",
          "[ChannelRenumbering]")
{
  // Two channels numbered 0 share a key (and so are ambiguous), but nothing
  // changed between the two snapshots, so it must not spuriously read as a
  // renumbering -- the same regression the duplicate-number test above guards.
  std::vector<Channel> zeros = {MakeChannel(1, 0), MakeChannel(2, 0)};
  CHECK_FALSE(HaveChannelNumbersChanged(zeros, zeros));
  std::vector<Channel> unnumbered = {MakeUnnumbered(1), MakeUnnumbered(2)};
  CHECK_FALSE(HaveChannelNumbersChanged(unnumbered, unnumbered));
}

TEST_CASE("HaveChannelNumbersChanged sees a channel gain or lose its number", "[ChannelRenumbering]")
{
  // An unnumbered channel's guide is exported under its id, a numbered one's
  // under the number, so either direction moves the key.
  CHECK(HaveChannelNumbersChanged({MakeUnnumbered(7)}, {MakeChannel(7, 12)}));
  CHECK(HaveChannelNumbersChanged({MakeChannel(7, 12)}, {MakeUnnumbered(7)}));
}

// ---------------------------------------------------------------------
// FindAmbiguousChannelNumbers
// ---------------------------------------------------------------------

TEST_CASE("FindAmbiguousChannelNumbers is empty when every channel number is unique", "[ChannelRenumbering]")
{
  std::vector<Channel> channels = {MakeChannel(1, 5), MakeChannel(2, 6), MakeChannel(3, 7)};
  CHECK(FindAmbiguousChannelNumbers(channels).empty());
}

TEST_CASE("FindAmbiguousChannelNumbers flags a number shared by two channels -- the real collision confirmed live",
          "[ChannelRenumbering]")
{
  // The exact scenario confirmed live 2026-09-28 (docs/OPEN_ITEMS.md): two
  // channels from different provider groups both auto-numbered onto 9901.
  std::vector<Channel> channels = {MakeChannel(1, 9901), MakeChannel(2, 9901), MakeChannel(3, 42)};
  auto ambiguous = FindAmbiguousChannelNumbers(channels);
  CHECK(ambiguous.count("9901") == 1);
  CHECK(ambiguous.count("42") == 0);
}

TEST_CASE("FindAmbiguousChannelNumbers flags a number shared by three or more channels", "[ChannelRenumbering]")
{
  std::vector<Channel> channels = {MakeChannel(1, 10), MakeChannel(2, 10), MakeChannel(3, 10)};
  auto ambiguous = FindAmbiguousChannelNumbers(channels);
  CHECK(ambiguous.count("10") == 1);
}

TEST_CASE("FindAmbiguousChannelNumbers flags several channels numbered 0", "[ChannelRenumbering]")
{
  // A literal 0 is a number Dispatcharr exports as <channel id="0"> for every
  // channel holding it, so they collide like any other shared number.
  std::vector<Channel> channels = {MakeChannel(1, 0), MakeChannel(2, 0)};
  CHECK(FindAmbiguousChannelNumbers(channels).count("0") == 1);
}

TEST_CASE("FindAmbiguousChannelNumbers doesn't flag channels with no number: each is exported under its own id",
          "[ChannelRenumbering]")
{
  std::vector<Channel> channels = {MakeUnnumbered(1), MakeUnnumbered(2), MakeChannel(3, 40)};
  CHECK(FindAmbiguousChannelNumbers(channels).empty());
}

TEST_CASE("FindAmbiguousChannelNumbers catches an unnumbered channel whose id is another channel's number",
          "[ChannelRenumbering]")
{
  // Both are written into the same <channel id> space, as "77".
  std::vector<Channel> channels = {MakeUnnumbered(77), MakeChannel(2, 77)};
  CHECK(FindAmbiguousChannelNumbers(channels).count("77") == 1);
}

TEST_CASE("FindAmbiguousChannelNumbers is empty for an empty channel list", "[ChannelRenumbering]")
{
  CHECK(FindAmbiguousChannelNumbers({}).empty());
}

// ---------------------------------------------------------------------
// Fractional (sub-channel) numbers -- live-confirmed 2026-09-30
// (docs/OPEN_ITEMS.md): Channel::channelNumber used to be an int, so 5.1
// silently became 5 and shared 5's identity.
// ---------------------------------------------------------------------

TEST_CASE("HaveChannelNumbersChanged sees a channel move from 5 to the sub-channel 5.1", "[ChannelRenumbering]")
{
  // Both used to truncate to 5, so this read as "nothing changed" and the
  // cached guide stayed keyed by the wrong text.
  std::vector<Channel> oldChannels = {MakeChannel(1, 5)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5.1)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged is false when a sub-channel stays put next to its parent", "[ChannelRenumbering]")
{
  std::vector<Channel> oldChannels = {MakeChannel(1, 5), MakeChannel(2, 5.1)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5), MakeChannel(2, 5.1)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged catches a new channel id reusing a deleted sub-channel's number",
          "[ChannelRenumbering]")
{
  std::vector<Channel> oldChannels = {MakeChannel(1, 5.1)};
  std::vector<Channel> newChannels = {MakeChannel(2, 5.1)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged does not treat a new whole channel as reusing an existing sub-channel's number",
          "[ChannelRenumbering]")
{
  // 5 is a number never seen before (5.1 is a different one) -- "no guide
  // yet", not "wrong guide".
  std::vector<Channel> oldChannels = {MakeChannel(1, 5.1)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5.1), MakeChannel(2, 5)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("HaveChannelNumbersChanged reads a whole-valued float the same as the whole number", "[ChannelRenumbering]")
{
  // The live API returns 5.0 for channel 5 -- and Dispatcharr's own export
  // keys it "5" either way.
  std::vector<Channel> oldChannels = {MakeChannel(1, 5)};
  std::vector<Channel> newChannels = {MakeChannel(1, 5.0)};
  CHECK_FALSE(HaveChannelNumbersChanged(oldChannels, newChannels));
}

TEST_CASE("FindAmbiguousChannelNumbers does not confuse a sub-channel with its parent channel", "[ChannelRenumbering]")
{
  // The live check's shape: 88881 and 88881.1 are two different XMLTV ids
  // (Dispatcharr exported <channel id="88881"> and "88881.1"), so neither
  // is ambiguous. Before the fix both read as 88881 and *both* lost their
  // guide.
  std::vector<Channel> channels = {MakeChannel(1, 88881), MakeChannel(2, 88881.1), MakeChannel(3, 88882.5)};
  CHECK(FindAmbiguousChannelNumbers(channels).empty());
}

TEST_CASE("FindAmbiguousChannelNumbers flags two channels sharing the same sub-channel number", "[ChannelRenumbering]")
{
  std::vector<Channel> channels = {MakeChannel(1, 5.1), MakeChannel(2, 5.1), MakeChannel(3, 5)};
  auto ambiguous = FindAmbiguousChannelNumbers(channels);
  CHECK(ambiguous.count("5.1") == 1);
  CHECK(ambiguous.count("5") == 0);
  CHECK(ambiguous.size() == 1);
}

TEST_CASE("FindAmbiguousChannelNumbers treats 5 and a whole-valued 5.0 as the same number", "[ChannelRenumbering]")
{
  std::vector<Channel> channels = {MakeChannel(1, 5), MakeChannel(2, 5.0)};
  CHECK(FindAmbiguousChannelNumbers(channels).count("5") == 1);
}

TEST_CASE("FindAmbiguousChannelNumbers keys match FormatChannelNumberKey() so the guide lookup agrees",
          "[ChannelRenumbering]")
{
  // GetEPGForChannel() checks membership using FormatChannelNumberKey() of
  // the channel's own number; an ambiguous set built any other way would
  // silently never match.
  std::vector<Channel> channels = {MakeChannel(1, 88882.5), MakeChannel(2, 88882.5)};
  auto ambiguous = FindAmbiguousChannelNumbers(channels);
  REQUIRE(ambiguous.size() == 1);
  CHECK(*ambiguous.begin() == FormatChannelNumberKey(88882.5));
}

TEST_CASE("FindAmbiguousChannelNumbers flags numbers below 1 that several channels share", "[ChannelRenumbering]")
{
  // Exported as "0.5" for each, so they collide even though Kodi shows both as 0.
  std::vector<Channel> channels = {MakeChannel(1, 0.5), MakeChannel(2, 0.5)};
  CHECK(FindAmbiguousChannelNumbers(channels).count("0.5") == 1);
}

TEST_CASE("ChannelGuideKey is the exported <channel id> for a numbered channel and the id for an unnumbered one",
          "[ChannelRenumbering]")
{
  Channel numbered = MakeChannel(10, 5.1);
  numbered.hasChannelNumber = true;
  CHECK(ChannelGuideKey(numbered) == "5.1");
  Channel whole = MakeChannel(10, 12.0);
  whole.hasChannelNumber = true;
  CHECK(ChannelGuideKey(whole) == "12");
  Channel unnumbered = MakeChannel(77, 0.0);
  unnumbered.hasChannelNumber = false;
  CHECK(ChannelGuideKey(unnumbered) == "77");
}

TEST_CASE("HaveChannelNumbersChanged keeps looking after a channel that has no guide key", "[ChannelRenumbering]")
{
  // A number at or above 1e15 has no key and is skipped; the loop must go on to the channel after it.
  std::vector<Channel> oldChannels = {MakeChannel(1, 1e16), MakeChannel(2, 6)};
  std::vector<Channel> newChannels = {MakeChannel(1, 1e16), MakeChannel(2, 7)};
  CHECK(HaveChannelNumbersChanged(oldChannels, newChannels));
}
