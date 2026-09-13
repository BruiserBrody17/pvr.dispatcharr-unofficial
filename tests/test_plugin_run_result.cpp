#include "PluginRunResult.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("UnwrapPluginRunResult succeeds when both envelope and inner status are ok", "[PluginRunResult]")
{
  json response = {{"success", true}, {"result", {{"status", "ok"}, {"playlist_url", "http://example/live.m3u8"}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "timeshift_buffer", result, error);

  REQUIRE(ok);
  CHECK(error.empty());
  CHECK(result["playlist_url"] == "http://example/live.m3u8");
}

TEST_CASE("UnwrapPluginRunResult fails on the outer success=false envelope, using its own error", "[PluginRunResult]")
{
  json response = {{"success", false}, {"error", "plugin not installed"}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "timeshift_buffer", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "plugin not installed");
}

TEST_CASE("UnwrapPluginRunResult falls back to a generic outer error message when the envelope has none",
          "[PluginRunResult]")
{
  json response = {{"success", false}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "recording_edl", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "recording_edl plugin call did not succeed");
}

TEST_CASE("UnwrapPluginRunResult fails when the inner status isn't ok, using its own message", "[PluginRunResult]")
{
  json response = {{"success", true}, {"result", {{"status", "error"}, {"message", "channel not found"}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "timeshift_buffer", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "channel not found");
  // resultOut is still set to the inner result object even on failure, per
  // this function's own documented contract -- callers like
  // RefreshLiveManifest() need a field from it ("fatal") either way.
  CHECK(result["status"] == "error");
}

TEST_CASE("UnwrapPluginRunResult falls back to a generic inner error message when the result has none",
          "[PluginRunResult]")
{
  json response = {{"success", true}, {"result", {{"status", "error"}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "recording_edl", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "recording_edl plugin returned an error");
}

TEST_CASE("UnwrapPluginRunResult leaves resultOut null when result is absent", "[PluginRunResult]")
{
  json response = {{"success", true}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(response, "timeshift_buffer", result, error);

  CHECK_FALSE(ok); // no "status": "ok" -- FieldOr's own default applies to a null resultOut too
  CHECK(result.is_null());
}
