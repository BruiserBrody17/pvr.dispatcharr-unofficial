#include "JsonFieldUtil.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

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

TEST_CASE("FieldOr returns the default for a finite-but-huge float read as an integral type, not UB", "[JsonFieldUtil]")
{
  // nlohmann's own lexer stores an integer literal too large for
  // int64_t/uint64_t as a number_float rather than failing to parse --
  // and converting a number_float to an integral type is a plain
  // static_cast inside nlohmann itself, with no range check and no
  // throw. Casting an out-of-range double to an integral type is
  // undefined behavior in C++; this must never reach that cast.
  json huge = json::parse("{\"type\":1e300}");
  CHECK(FieldOr<int>(huge, "type", -1) == -1);
  CHECK(FieldOr<int64_t>(huge, "type", -1) == -1);
}

TEST_CASE("FieldOr returns the default for a large negative float read as an integral type, not UB", "[JsonFieldUtil]")
{
  json hugeNegative = json::parse("{\"type\":-1e300}");
  CHECK(FieldOr<int>(hugeNegative, "type", -1) == -1);
  CHECK(FieldOr<int64_t>(hugeNegative, "type", -1) == -1);
}

TEST_CASE("FieldOr still returns an in-range float value read as an integral type", "[JsonFieldUtil]")
{
  json item = {{"count", 42.0}};
  CHECK(FieldOr<int>(item, "count", -1) == 42);
}

TEST_CASE("FieldOr returns the default for a non-finite float read as an integral type", "[JsonFieldUtil]")
{
  json item = {{"count", std::numeric_limits<double>::infinity()}};
  CHECK(FieldOr<int>(item, "count", -1) == -1);
}

TEST_CASE("FieldOr is unaffected by the float clamp for a non-integral target type", "[JsonFieldUtil]")
{
  json item = {{"duration", 1e300}};
  CHECK(FieldOr<double>(item, "duration", 0.0) == 1e300);
}

TEST_CASE("FieldOr rejects a float that rounds to exactly INT64_MAX + 1 as a double, not UB", "[JsonFieldUtil]")
{
  // A 52nd-pass audit regression case: int64_t's real max (2^63-1) isn't
  // exactly representable as a double -- it rounds UP to exactly 2^63,
  // the same value this literal parses to. The old `d > max_as_double`
  // check compared d against that same rounded-up value, so this exact
  // input passed the check and reached an unchecked static_cast to
  // int64_t -- undefined behavior, confirmed with UBSan before this fix.
  json item = json::parse("{\"count\":9223372036854775807.0}");
  CHECK(FieldOr<int64_t>(item, "count", -1) == -1);
}

TEST_CASE("FieldOr rejects a float that rounds to exactly UINT64_MAX + 1 as a double, not UB", "[JsonFieldUtil]")
{
  json item = json::parse("{\"count\":18446744073709551615.0}");
  CHECK(FieldOr<uint64_t>(item, "count", 0) == 0);
}

TEST_CASE("FieldOr still accepts a float safely just below the int64_t/uint64_t boundary", "[JsonFieldUtil]")
{
  // The fix above must not overcorrect: a value safely representable and
  // castable, even one very close to the rounded boundary, must still
  // come through rather than being needlessly rejected. Renamed from an
  // earlier, overstated title (a 53rd-pass audit, found via a
  // project-wide review): INT64_MAX itself isn't representable as a
  // double at all (it rounds up to 2^63, the boundary this exists to
  // reject), so this can only ever test a value safely *below* it, not
  // the true max value itself -- true for every integral width except
  // one narrow enough that max() is exactly representable, already
  // covered by the int/uint32_t case below instead.
  json int64Item = json::parse("{\"count\":9223372036854774784.0}"); // 2^63 - 1024, exact as a double
  CHECK(FieldOr<int64_t>(int64Item, "count", -1) == 9223372036854774784LL);
  json uint64Item = json::parse("{\"count\":18446744073709549568.0}"); // 2^64 - 2048, exact as a double
  CHECK(FieldOr<uint64_t>(uint64Item, "count", 0) == 18446744073709549568ULL);
}

TEST_CASE("FieldOr still accepts the true int32_t/uint32_t max value itself", "[JsonFieldUtil]")
{
  // For a narrower type, max() IS exactly representable as a double, so
  // the +1.0 upper-bound adjustment must not reject the real max value.
  json intItem = {{"count", 2147483647.0}};
  CHECK(FieldOr<int>(intItem, "count", -1) == 2147483647);
  json uintItem = {{"count", 4294967295.0}};
  CHECK(FieldOr<uint32_t>(uintItem, "count", 0) == 4294967295u);
}

// ---------------------------------------------------------------------
// MergeDvrOffsetMinutes
// ---------------------------------------------------------------------

TEST_CASE("MergeDvrOffsetMinutes sets both offset keys when both are non-null", "[JsonFieldUtil]")
{
  json value = json::object();
  int pre = 15, post = 30;
  json merged = MergeDvrOffsetMinutes(value, &pre, &post);
  CHECK(merged["pre_offset_minutes"] == 15);
  CHECK(merged["post_offset_minutes"] == 30);
}

TEST_CASE("MergeDvrOffsetMinutes preserves every other key already in the value blob", "[JsonFieldUtil]")
{
  // The exact regression this guards against: Dispatcharr's own PATCH
  // replaces the whole value blob rather than merging it server-side, so
  // a body built from just the two offset keys would silently wipe
  // everything else (comskip settings, path templates, ...) -- a real
  // incident, not hypothetical.
  json value = {{"pre_offset_minutes", 0},
                {"post_offset_minutes", 0},
                {"comskip_enabled", true},
                {"path_template", "{title}/{season}"}};
  int pre = 15, post = 30;
  json merged = MergeDvrOffsetMinutes(value, &pre, &post);
  CHECK(merged["comskip_enabled"] == true);
  CHECK(merged["path_template"] == "{title}/{season}");
  CHECK(merged["pre_offset_minutes"] == 15);
  CHECK(merged["post_offset_minutes"] == 30);
}

TEST_CASE("MergeDvrOffsetMinutes overwrites pre-existing offset values", "[JsonFieldUtil]")
{
  json value = {{"pre_offset_minutes", 5}, {"post_offset_minutes", 10}};
  int pre = 15, post = 30;
  json merged = MergeDvrOffsetMinutes(value, &pre, &post);
  CHECK(merged["pre_offset_minutes"] == 15);
  CHECK(merged["post_offset_minutes"] == 30);
}

TEST_CASE("MergeDvrOffsetMinutes does not mutate the caller's own copy", "[JsonFieldUtil]")
{
  json value = json::object();
  int pre = 15, post = 30;
  MergeDvrOffsetMinutes(value, &pre, &post);
  CHECK_FALSE(value.contains("pre_offset_minutes"));
}

TEST_CASE("MergeDvrOffsetMinutes leaves the post offset untouched when only pre is given -- the real bug this fixes",
          "[JsonFieldUtil]")
{
  // Before this, both offsets were always overwritten together, even
  // when only one was the caller's actual, intended change -- silently
  // reverting a genuine server-side change to the other one made since
  // this addon last synced it into Kodi's own local settings copy.
  json value = {{"pre_offset_minutes", 5}, {"post_offset_minutes", 10}};
  int pre = 15;
  json merged = MergeDvrOffsetMinutes(value, &pre, nullptr);
  CHECK(merged["pre_offset_minutes"] == 15);
  CHECK(merged["post_offset_minutes"] == 10); // untouched, not overwritten with a stale value
}

TEST_CASE("MergeDvrOffsetMinutes leaves the pre offset untouched when only post is given", "[JsonFieldUtil]")
{
  json value = {{"pre_offset_minutes", 5}, {"post_offset_minutes", 10}};
  int post = 30;
  json merged = MergeDvrOffsetMinutes(value, nullptr, &post);
  CHECK(merged["pre_offset_minutes"] == 5); // untouched
  CHECK(merged["post_offset_minutes"] == 30);
}

TEST_CASE("MergeDvrOffsetMinutes with both null changes nothing", "[JsonFieldUtil]")
{
  json value = {{"pre_offset_minutes", 5}, {"post_offset_minutes", 10}, {"comskip_enabled", true}};
  json merged = MergeDvrOffsetMinutes(value, nullptr, nullptr);
  CHECK(merged == value);
}

// ---------------------------------------------------------------------
// UnwrapListResponse
// ---------------------------------------------------------------------

TEST_CASE("UnwrapListResponse returns the response itself for a bare array", "[JsonFieldUtil]")
{
  json response = json::array({1, 2, 3});
  CHECK(UnwrapListResponse(response) == response);
}

TEST_CASE("UnwrapListResponse unwraps the default \"results\" key", "[JsonFieldUtil]")
{
  json response = {{"results", json::array({1, 2, 3})}, {"count", 3}};
  CHECK(UnwrapListResponse(response) == json::array({1, 2, 3}));
}

TEST_CASE("UnwrapListResponse checks multiple keys in order", "[JsonFieldUtil]")
{
  // The exact real case: GetTimerRules() checks "rules" first, "results"
  // second -- confirmed live that the series-rules endpoint uses neither
  // a bare array nor the usual DRF "results" wrapper.
  json withRules = {{"rules", json::array({1, 2})}};
  CHECK(UnwrapListResponse(withRules, {"rules", "results"}) == json::array({1, 2}));

  json withResultsOnly = {{"results", json::array({3, 4})}};
  CHECK(UnwrapListResponse(withResultsOnly, {"rules", "results"}) == json::array({3, 4}));

  json neither = json::array({5, 6});
  CHECK(UnwrapListResponse(neither, {"rules", "results"}) == json::array({5, 6}));
}

TEST_CASE("UnwrapListResponse prefers the earlier key when both are present", "[JsonFieldUtil]")
{
  json response = {{"rules", json::array({1})}, {"results", json::array({2})}};
  CHECK(UnwrapListResponse(response, {"rules", "results"}) == json::array({1}));
}

// ---------------------------------------------------------------------
// IsTruncatedPaginatedResponse
// ---------------------------------------------------------------------

TEST_CASE("IsTruncatedPaginatedResponse is false for a bare array", "[JsonFieldUtil]")
{
  json response = json::array({1, 2, 3});
  CHECK_FALSE(IsTruncatedPaginatedResponse(response));
}

TEST_CASE("IsTruncatedPaginatedResponse is false when \"next\" is absent", "[JsonFieldUtil]")
{
  json response = {{"results", json::array({1, 2})}, {"count", 2}};
  CHECK_FALSE(IsTruncatedPaginatedResponse(response));
}

TEST_CASE("IsTruncatedPaginatedResponse is false when \"next\" is explicitly null (the last/only page)",
          "[JsonFieldUtil]")
{
  json response = {{"results", json::array({1, 2})}, {"next", nullptr}};
  CHECK_FALSE(IsTruncatedPaginatedResponse(response));
}

TEST_CASE("IsTruncatedPaginatedResponse is true when \"next\" carries a real URL", "[JsonFieldUtil]")
{
  json response = {{"results", json::array({1, 2})}, {"next", "http://example.invalid/api/channels/channels/?page=2"}};
  CHECK(IsTruncatedPaginatedResponse(response));
}

// ---------------------------------------------------------------------
// FindSettingsRowByKey
// ---------------------------------------------------------------------

TEST_CASE("FindSettingsRowByKey finds a matching row with a real object value", "[JsonFieldUtil]")
{
  json list = json::array({{{"key", "dvr_settings"}, {"id", 7}, {"value", {{"pre_offset_minutes", 5}}}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK(match.found);
  CHECK(match.id == 7);
  CHECK(match.valueWasObject);
  CHECK(match.value["pre_offset_minutes"] == 5);
}

TEST_CASE("FindSettingsRowByKey reports not found for a missing key", "[JsonFieldUtil]")
{
  json list = json::array({{{"key", "other_settings"}, {"id", 1}, {"value", json::object()}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK_FALSE(match.found);
}

TEST_CASE("FindSettingsRowByKey flags a non-object value rather than silently substituting {}", "[JsonFieldUtil]")
{
  // The exact real gap this exists to catch: a row whose real "value"
  // isn't an object at all (here, a bare string) used to be silently
  // treated as an empty object, which SetDvrOffsetMinutes() would then
  // merge into and PATCH back -- wiping whatever the real value held.
  json list = json::array({{{"key", "dvr_settings"}, {"id", 7}, {"value", "not-an-object"}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK(match.found);
  CHECK_FALSE(match.valueWasObject);
  CHECK(match.value == json::object());
}

TEST_CASE("FindSettingsRowByKey flags a missing value field the same way", "[JsonFieldUtil]")
{
  json list = json::array({{{"key", "dvr_settings"}, {"id", 7}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK(match.found);
  CHECK_FALSE(match.valueWasObject);
}

TEST_CASE("FindSettingsRowByKey flags an explicit JSON null value the same way", "[JsonFieldUtil]")
{
  json list = json::array({{{"key", "dvr_settings"}, {"id", 7}, {"value", nullptr}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK(match.found);
  CHECK_FALSE(match.valueWasObject);
}

TEST_CASE("FindSettingsRowByKey reports a matched row's id even when it's 0", "[JsonFieldUtil]")
{
  json list = json::array({{{"key", "dvr_settings"}, {"id", 0}, {"value", json::object()}}});
  SettingsRowMatch match = FindSettingsRowByKey(list, "dvr_settings");
  CHECK(match.found);
  CHECK(match.id == 0);
}

TEST_CASE("TrySerializeJsonBody serializes an ordinary body unchanged", "[JsonFieldUtil]")
{
  json body = {{"title", "Channel A news"}, {"extra_minutes", 5}};
  std::string out, error;
  REQUIRE(TrySerializeJsonBody(body, out, error));
  CHECK(out == body.dump());
  CHECK(error.empty());
}

TEST_CASE("TrySerializeJsonBody fails instead of throwing on a Kodi-truncated UTF-8 title", "[JsonFieldUtil]")
{
  // Regression: the exact shape Kodi's own byte-level strncpy() into a
  // PVR_TIMER/PVR_RECORDING strTitle (char[1024]) produces for a long
  // title of 2-byte UTF-8 characters -- 1023 bytes, ending on the first
  // byte of a split sequence. A plain dump() throws type_error.316 here,
  // which escaped all the way to Kodi's own C callback wrappers.
  std::string full;
  for (int i = 0; i < 600; ++i)
    full += "\xD0\x96";
  std::string kodiTitle = full.substr(0, 1023);
  json body = {{"title", kodiTitle}};
  std::string out, error;
  bool ok = true;
  REQUIRE_NOTHROW(ok = TrySerializeJsonBody(body, out, error));
  CHECK_FALSE(ok);
  CHECK_FALSE(error.empty());
}

TEST_CASE("TrySerializeJsonBody fails on invalid UTF-8 nested inside the body too", "[JsonFieldUtil]")
{
  json body = {{"action", "heartbeat"}, {"params", {{"viewer_id", std::string("abc\xFF")}}}};
  std::string out, error;
  CHECK_FALSE(TrySerializeJsonBody(body, out, error));
}

TEST_CASE("TrySerializeJsonBody accepts valid multi-byte UTF-8", "[JsonFieldUtil]")
{
  json body = {{"title", "\xD0\x96\xE3\x81\x82\xF0\x9F\x93\xBA"}};
  std::string out, error;
  CHECK(TrySerializeJsonBody(body, out, error));
}

TEST_CASE("FieldOr gives the default for an integer outside the target type's range instead of wrapping",
          "[JsonFieldUtil]")
{
  // nlohmann's integral get<>() is a plain cast: these used to read as 5, -1 and 1.
  auto item = nlohmann::json::parse(
      R"({"big":4294967301,"huge":18446744073709551615,"edl":4294967297,"neg":-4294967301,"ok":2147483647,
          "min":-2147483648,"over":2147483648,"under":-2147483649,"u32":4294967295,"u32over":4294967296,
          "negu":-1,"i64":9223372036854775807,"i64min":-9223372036854775808})");
  CHECK(FieldOr<int>(item, "big", -7) == -7);
  CHECK(FieldOr<int>(item, "huge", -7) == -7);
  CHECK(FieldOr<int>(item, "edl", -7) == -7);
  CHECK(FieldOr<int>(item, "neg", -7) == -7);
  CHECK(FieldOr<int>(item, "over", -7) == -7);
  CHECK(FieldOr<int>(item, "under", -7) == -7);
  CHECK(FieldOr<int64_t>(item, "huge", -7) == -7);
  CHECK(FieldOr<uint32_t>(item, "u32over", 9) == 9);
  CHECK(FieldOr<uint32_t>(item, "negu", 9) == 9);
  CHECK(FieldOr<uint64_t>(item, "negu", 9) == 9);
}

TEST_CASE("FieldOr still reads every in-range integer, including both ends of the range", "[JsonFieldUtil]")
{
  auto item = nlohmann::json::parse(
      R"({"ok":2147483647,"min":-2147483648,"u32":4294967295,"i64":9223372036854775807,
          "i64min":-9223372036854775808,"u64":18446744073709551615,"zero":0,"flag":true})");
  CHECK(FieldOr<int>(item, "ok", -7) == 2147483647);
  CHECK(FieldOr<int>(item, "min", -7) == -2147483647 - 1);
  CHECK(FieldOr<uint32_t>(item, "u32", 9) == 4294967295u);
  CHECK(FieldOr<int64_t>(item, "i64", -7) == 9223372036854775807LL);
  CHECK(FieldOr<int64_t>(item, "i64min", -7) == (-9223372036854775807LL - 1));
  CHECK(FieldOr<uint64_t>(item, "u64", 9) == 18446744073709551615ULL);
  CHECK(FieldOr<int>(item, "zero", -7) == 0);
  CHECK(FieldOr<bool>(item, "flag", false) == true);
}

TEST_CASE("FieldOr rejects a positive signed integer beyond the target type's range", "[JsonFieldUtil]")
{
  // A value built in code is stored signed; one parsed from text is stored unsigned. Both must be
  // range-checked.
  nlohmann::json built;
  built["id"] = static_cast<int64_t>(1) << 40;
  CHECK(FieldOr<int>(built, "id", -7) == -7);
  CHECK(FieldOr<int64_t>(built, "id", -7) == (static_cast<int64_t>(1) << 40));
  nlohmann::json parsed = nlohmann::json::parse(R"({"id":1099511627776})");
  CHECK(FieldOr<int>(parsed, "id", -7) == -7);
  nlohmann::json negative;
  negative["id"] = -(static_cast<int64_t>(1) << 40);
  CHECK(FieldOr<int>(negative, "id", -7) == -7);
  CHECK(FieldOr<uint32_t>(negative, "id", 9) == 9);
}

TEST_CASE("FieldOr<int> reads a JSON float that is exactly INT_MIN or INT_MAX, and refuses one past either",
          "[JsonFieldUtil]")
{
  // The integer literals the other range tests use never reach the floating-point branch.
  json item = {{"low", -2147483648.0}, {"high", 2147483647.0}, {"below", -2147483649.0}, {"above", 2147483648.0}};
  CHECK(FieldOr<int>(item, "low", 7) == std::numeric_limits<int>::min());
  CHECK(FieldOr<int>(item, "high", 7) == std::numeric_limits<int>::max());
  CHECK(FieldOr<int>(item, "below", 7) == 7);
  CHECK(FieldOr<int>(item, "above", 7) == 7);
}
