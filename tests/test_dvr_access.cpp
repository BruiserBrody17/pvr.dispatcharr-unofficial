#include "DvrAccess.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using nlohmann::json;

TEST_CASE("an admin always manages the DVR, whatever custom_properties says", "[DvrAccess]")
{
  CHECK(ComputeDvrAccess(10, "") == DvrAccess::kManage);
  CHECK(ComputeDvrAccess(10, "none") == DvrAccess::kManage);
  CHECK(ComputeDvrAccess(10, "view") == DvrAccess::kManage);
}

TEST_CASE("a streamer has no DVR access, even when custom_properties grants it", "[DvrAccess]")
{
  CHECK(ComputeDvrAccess(0, "") == DvrAccess::kNone);
  CHECK(ComputeDvrAccess(0, "manage") == DvrAccess::kNone);
}

TEST_CASE("a standard user takes custom_properties.dvr_access, and an unset one is view-only", "[DvrAccess]")
{
  CHECK(ComputeDvrAccess(1, "manage") == DvrAccess::kManage);
  CHECK(ComputeDvrAccess(1, "view") == DvrAccess::kView);
  CHECK(ComputeDvrAccess(1, "none") == DvrAccess::kNone);
  // The case this exists for: nobody configured it, so the server says "view".
  CHECK(ComputeDvrAccess(1, "") == DvrAccess::kView);
  CHECK(ComputeDvrAccess(1, "bogus") == DvrAccess::kView);
  CHECK(ComputeDvrAccess(5, "") == DvrAccess::kView);
}

TEST_CASE("only manage can manage the DVR", "[DvrAccess]")
{
  CHECK(CanManageDvr(DvrAccess::kManage));
  CHECK_FALSE(CanManageDvr(DvrAccess::kView));
  CHECK_FALSE(CanManageDvr(DvrAccess::kNone));
}

TEST_CASE("ParseDvrAccessFromUserJson reads user_level and custom_properties.dvr_access", "[DvrAccess]")
{
  DvrAccess out = DvrAccess::kManage;
  REQUIRE(ParseDvrAccessFromUserJson(json{{"user_level", 1}, {"custom_properties", {{"dvr_access", "manage"}}}}, out));
  CHECK(out == DvrAccess::kManage);

  REQUIRE(ParseDvrAccessFromUserJson(json{{"user_level", 1}, {"custom_properties", json::object()}}, out));
  CHECK(out == DvrAccess::kView);

  REQUIRE(ParseDvrAccessFromUserJson(json{{"user_level", 1}}, out));
  CHECK(out == DvrAccess::kView);

  REQUIRE(ParseDvrAccessFromUserJson(json{{"user_level", 10}, {"custom_properties", nullptr}}, out));
  CHECK(out == DvrAccess::kManage);
}

TEST_CASE("ParseDvrAccessFromUserJson refuses a response it cannot derive a level from, leaving the output alone",
          "[DvrAccess]")
{
  DvrAccess out = DvrAccess::kView;
  CHECK_FALSE(ParseDvrAccessFromUserJson(json::object(), out));
  CHECK_FALSE(ParseDvrAccessFromUserJson(json{{"user_level", "ten"}}, out));
  CHECK_FALSE(ParseDvrAccessFromUserJson(json::array(), out));
  CHECK_FALSE(ParseDvrAccessFromUserJson(json(), out));
  CHECK(out == DvrAccess::kView);
}
