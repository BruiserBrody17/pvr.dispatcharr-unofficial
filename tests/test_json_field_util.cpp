#include "JsonFieldUtil.h"

#include <catch2/catch_test_macros.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("FieldOr returns the real value when present and correctly typed", "[JsonFieldUtil]")
{
  json item = {{"name", "Channel A"}, {"channel_number", 42}};
  CHECK(FieldOr<std::string>(item, "name", "") == "Channel A");
  CHECK(FieldOr<int>(item, "channel_number", 0) == 42);
}

TEST_CASE("FieldOr returns the default when the key is absent", "[JsonFieldUtil]")
{
  json item = {{"name", "Channel A"}};
  CHECK(FieldOr<std::string>(item, "logo_id", "fallback") == "fallback");
}

TEST_CASE("FieldOr returns the default for an explicit JSON null, not throw", "[JsonFieldUtil]")
{
  // The exact documented case this exists for: Django REST Framework
  // serializers commonly emit an explicit null for an empty nullable
  // field, and item.value(key, default) alone doesn't handle that --
  // only an *absent* key falls back to the default.
  json item = {{"logo_id", nullptr}};
  CHECK(FieldOr<int>(item, "logo_id", -1) == -1);
}

TEST_CASE("FieldOr returns the default on a type mismatch instead of throwing", "[JsonFieldUtil]")
{
  json item = {{"channel_number", "not-a-number"}};
  CHECK(FieldOr<int>(item, "channel_number", -1) == -1);
}

TEST_CASE("FieldOr works with bool and double types too", "[JsonFieldUtil]")
{
  json item = {{"enabled", true}, {"duration", 12.5}};
  CHECK(FieldOr<bool>(item, "enabled", false) == true);
  CHECK(FieldOr<double>(item, "duration", 0.0) == 12.5);
  CHECK(FieldOr<bool>(item, "missing_flag", false) == false);
}
