#include "ChannelNumber.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <limits>

using namespace dispatcharr;

TEST_CASE("SplitChannelNumber keeps a whole channel number exactly as before", "[ChannelNumber]")
{
  ChannelNumberParts p = SplitChannelNumber(101.0);
  CHECK(p.whole == 101);
  CHECK(p.sub == 0);
  CHECK(p.key == "101");
}

TEST_CASE("SplitChannelNumber reads a whole-valued float the way Dispatcharr's own export does", "[ChannelNumber]")
{
  // The live API returns 5.0 for channel 5; format_channel_number() renders
  // a whole-valued float as an int, so the XMLTV id is "5", never "5.0".
  ChannelNumberParts p = SplitChannelNumber(5.0);
  CHECK(p.whole == 5);
  CHECK(p.sub == 0);
  CHECK(p.key == "5");
}

TEST_CASE("SplitChannelNumber splits a subchannel into channel and sub-channel numbers", "[ChannelNumber]")
{
  // The case that used to be silently truncated to plain channel 5.
  ChannelNumberParts p = SplitChannelNumber(5.1);
  CHECK(p.whole == 5);
  CHECK(p.sub == 1);
  CHECK(p.key == "5.1");
}

TEST_CASE("SplitChannelNumber matches the keys the live lab export produced", "[ChannelNumber]")
{
  // The three disposable channels used for the 2026-09-30 live check:
  // Dispatcharr exported <channel id="88881">, "88881.1" and "88882.5".
  CHECK(SplitChannelNumber(88881.0).key == "88881");
  CHECK(SplitChannelNumber(88881.1).key == "88881.1");
  CHECK(SplitChannelNumber(88882.5).key == "88882.5");
  CHECK(SplitChannelNumber(88881.1).whole == 88881);
  CHECK(SplitChannelNumber(88881.1).sub == 1);
  CHECK(SplitChannelNumber(88882.5).whole == 88882);
  CHECK(SplitChannelNumber(88882.5).sub == 5);
}

TEST_CASE("SplitChannelNumber keeps a two-digit sub-channel", "[ChannelNumber]")
{
  ChannelNumberParts p = SplitChannelNumber(5.25);
  CHECK(p.whole == 5);
  CHECK(p.sub == 25);
  CHECK(p.key == "5.25");
}

TEST_CASE("SplitChannelNumber gives distinct keys to numbers the sub-channel integer cannot tell apart",
          "[ChannelNumber]")
{
  // 5.05 and 5.5 both read as sub-channel 5 (the documented lossy case),
  // but the key -- what the guide lookup and collision detection use --
  // still tells them apart.
  ChannelNumberParts a = SplitChannelNumber(5.05);
  ChannelNumberParts b = SplitChannelNumber(5.5);
  CHECK(a.sub == 5);
  CHECK(b.sub == 5);
  CHECK(a.key == "5.05");
  CHECK(b.key == "5.5");
  CHECK(a.key != b.key);
}

TEST_CASE("SplitChannelNumber caps the sub-channel at nine digits but keeps the full key", "[ChannelNumber]")
{
  ChannelNumberParts p = SplitChannelNumber(5.123456789012);
  CHECK(p.whole == 5);
  CHECK(p.sub == 123456789);
  CHECK(p.key == "5.123456789012");
}

TEST_CASE("SplitChannelNumber reproduces Python's shortest round-trip text for a value needing 17 digits",
          "[ChannelNumber]")
{
  // 1.1 + 2.2 is the classic non-representable sum; Python's
  // str(1.1 + 2.2) is '3.3000000000000003', and a fixed 15-digit format
  // would have produced "3.3" -- a different XMLTV id from the one
  // Dispatcharr wrote.
  double sum = 1.1 + 2.2;
  REQUIRE(sum != 3.3);
  CHECK(SplitChannelNumber(sum).key == "3.3000000000000003");
  CHECK(SplitChannelNumber(3.3).key == "3.3");
}

TEST_CASE("SplitChannelNumber's key always reads back as exactly the same double", "[ChannelNumber]")
{
  for (double v : {1.5, 2.25, 5.1, 9.99, 12.34, 100.001, 88881.1, 88882.5, 1234567.5, 2147483647.5, 3.14159265358979})
  {
    ChannelNumberParts p = SplitChannelNumber(v);
    REQUIRE_FALSE(p.key.empty());
    CHECK(std::stod(p.key) == v);
    CHECK(p.key.find('e') == std::string::npos);
  }
}

TEST_CASE("SplitChannelNumber gives Kodi no number for a value it can't display", "[ChannelNumber]")
{
  for (double v :
       {0.0, -1.0, -5.1, 0.5, 0.99999, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), 2147483648.0, 1e15})
  {
    ChannelNumberParts p = SplitChannelNumber(v);
    CHECK(p.whole == 0);
    CHECK(p.sub == 0);
  }
}

TEST_CASE("SplitChannelNumber still keys a number Kodi can't display by Dispatcharr's own export text",
          "[ChannelNumber]")
{
  // Dispatcharr accepts a channel numbered 0 (ChannelSerializer sets no
  // minimum) and exports it as <channel id="0">; a negative or sub-1 number
  // is exported under its own text the same way. The guide has to be found
  // there even though Kodi shows the channel as 0.
  CHECK(SplitChannelNumber(0.0).key == "0");
  CHECK(SplitChannelNumber(-0.0).key == "0");
  CHECK(SplitChannelNumber(-1.0).key == "-1");
  CHECK(SplitChannelNumber(-5.1).key == "-5.1");
  CHECK(SplitChannelNumber(0.5).key == "0.5");
  CHECK(SplitChannelNumber(0.99999).key == "0.99999");
  CHECK(SplitChannelNumber(2147483648.0).key == "2147483648");
}

TEST_CASE("SplitChannelNumber leaves the key empty only for something that isn't a usable number", "[ChannelNumber]")
{
  for (double v : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity(), 1e15, -1e15, 1e300})
    CHECK(SplitChannelNumber(v).key.empty());
}

TEST_CASE("FormatChannelGuideKey uses the number's text for a numbered channel", "[ChannelNumber]")
{
  CHECK(FormatChannelGuideKey(true, 5.0, 900) == "5");
  CHECK(FormatChannelGuideKey(true, 5.1, 900) == "5.1");
  // A literal 0 is a number, not a missing one: it is exported as "0".
  CHECK(FormatChannelGuideKey(true, 0.0, 900) == "0");
}

TEST_CASE("FormatChannelGuideKey falls back to the channel id when there is no number", "[ChannelNumber]")
{
  // apps/output/epg.py: `str(number) if number != "" else str(channel.id)`.
  CHECK(FormatChannelGuideKey(false, 0.0, 900) == "900");
  CHECK(FormatChannelGuideKey(false, 7.0, 12) == "12"); // the number is ignored when flagged absent
}

TEST_CASE("FormatChannelGuideKey keeps an unusable number unmatchable", "[ChannelNumber]")
{
  CHECK(FormatChannelGuideKey(true, std::numeric_limits<double>::quiet_NaN(), 900).empty());
}

TEST_CASE("SplitChannelNumber accepts the exact boundaries of the usable range", "[ChannelNumber]")
{
  CHECK(SplitChannelNumber(1.0).key == "1");
  CHECK(SplitChannelNumber(1.0).whole == 1);
  CHECK(SplitChannelNumber(1.5).key == "1.5");
  ChannelNumberParts top = SplitChannelNumber(2147483647.0);
  CHECK(top.whole == 2147483647);
  CHECK(top.key == "2147483647");
}

TEST_CASE("the ChannelNumber accessors agree with SplitChannelNumber", "[ChannelNumber]")
{
  CHECK(FormatChannelNumberKey(5.1) == "5.1");
  CHECK(WholeChannelNumber(5.1) == 5);
  CHECK(SubChannelNumber(5.1) == 1);
  CHECK(FormatChannelNumberKey(7.0) == "7");
  CHECK(WholeChannelNumber(7.0) == 7);
  CHECK(SubChannelNumber(7.0) == 0);
  CHECK(FormatChannelNumberKey(0.0) == "0");
}

namespace
{
// A numpunct with a comma decimal point and dot grouping -- what a de_DE or fr_FR stream would write -- so
// the test needs no installed locale.
struct CommaDecimal : std::numpunct<char>
{
  char do_decimal_point() const override
  {
    return ',';
  }
  char do_thousands_sep() const override
  {
    return '.';
  }
  std::string do_grouping() const override
  {
    return "\3";
  }
};
} // namespace

TEST_CASE("FormatChannelNumberKey is locale-independent: a comma-decimal global locale cannot turn 5.1 into 5,1",
          "[ChannelNumber]")
{
  // The guide key must be exactly what Dispatcharr writes ("5.1"); a stream that honours the process locale
  // would write "5,1" under de_DE and the channel would never find its guide.
  const std::locale previous = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
  {
    // Sanity: a plain stream built now really does write the comma, so the test can fail.
    std::ostringstream plain;
    plain << 5.1;
    REQUIRE(plain.str() == "5,1");
  }
  const std::string key = FormatChannelNumberKey(5.1);
  const std::string whole = FormatChannelNumberKey(12.0);
  const std::string trailing = FormatChannelNumberKey(1234.5);
  std::locale::global(previous);
  CHECK(key == "5.1");
  CHECK(whole == "12");
  CHECK(trailing == "1234.5"); // no thousands grouping either
}
