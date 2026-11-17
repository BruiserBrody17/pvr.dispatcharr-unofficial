#include "PluginRunResult.h"

#include "small_stack.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace dispatcharr;
using json = nlohmann::json;

TEST_CASE("UnwrapPluginRunResult succeeds when both envelope and inner status are ok", "[PluginRunResult]")
{
  json response = {{"success", true}, {"result", {{"status", "ok"}, {"playlist_url", "http://example/live.m3u8"}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

  REQUIRE(ok);
  CHECK(error.empty());
  CHECK(result["playlist_url"] == "http://example/live.m3u8");
}

TEST_CASE("UnwrapPluginRunResult fails on the outer success=false envelope, using its own error", "[PluginRunResult]")
{
  json response = {{"success", false}, {"error", "plugin not installed"}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "plugin not installed");
}

TEST_CASE("UnwrapPluginRunResult falls back to a generic outer error message when the envelope has none",
          "[PluginRunResult]")
{
  json response = {{"success", false}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "recording_edl", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "recording_edl plugin call did not succeed");
}

TEST_CASE("UnwrapPluginRunResult fails when the inner status isn't ok, using its own message", "[PluginRunResult]")
{
  json response = {{"success", true}, {"result", {{"status", "error"}, {"message", "channel not found"}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

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
  bool ok = UnwrapPluginRunResult(std::move(response), "recording_edl", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "recording_edl plugin returned an error");
}

TEST_CASE("UnwrapPluginRunResult leaves resultOut null when result is absent", "[PluginRunResult]")
{
  json response = {{"success", true}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

  CHECK_FALSE(ok); // no "status": "ok" -- FieldOr's own default applies to a null resultOut too
  CHECK(result.is_null());
}

TEST_CASE("UnwrapPluginRunResult hands back a deeply nested result on a 1 MiB thread stack", "[PluginRunResult]")
{
  // The inner result is moved out of the response, not copied: nlohmann's copy constructor recurses per nesting level
  // (docs/CLOSED_ITEMS.md, "Deeply nested custom_properties could overflow the stack on Android").
  // The deep value is moved into place: an initializer list would copy it, recursively, on this thread.
  json response = {{"success", true}, {"result", {{"status", "ok"}}}};
  response["result"]["extra"] = dispatcharr_test::DeeplyNestedArray(dispatcharr_test::kDeepNestingLevels);

  json result;
  std::string error;
  bool ok = false;
  REQUIRE(dispatcharr_test::RunOnSmallStack(
      [&] { ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error); }));

  CHECK(ok);
  CHECK(result["status"] == "ok");
}

// Dispatcharr 0.34.0 sends a plugin's own {"status": "error"} result as an HTTP 500 with the result alongside the
// failed envelope (src/PluginRunResult.h, IsPluginRunErrorEnvelope()); the start retry, the cold-start wait and the
// replaced-buffer check all read a field of that result.
TEST_CASE("UnwrapPluginRunResult hands back the result a 0.34 error envelope carries", "[PluginRunResult]")
{
  json response = {
      {"success", false},
      {"error", "another start of this channel is running"},
      {"result", {{"status", "error"}, {"message", "another start of this channel is running"}, {"retryable", true}}}};

  json result;
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "another start of this channel is running");
  CHECK(result["status"] == "error");
  CHECK(result["retryable"] == true);
}

TEST_CASE("UnwrapPluginRunResult leaves resultOut null on a failed envelope whose result is not an object",
          "[PluginRunResult]")
{
  json response = {{"success", false}, {"error", "boom"}, {"result", "a string"}};

  json result = {{"stale", true}};
  std::string error;
  bool ok = UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error);

  CHECK_FALSE(ok);
  CHECK(error == "boom");
  CHECK(result.is_null());
}

TEST_CASE("UnwrapPluginRunResult sets resultOut to null on a failed envelope with no result", "[PluginRunResult]")
{
  json response = {{"success", false}, {"error", "Plugin is disabled"}};

  json result = {{"stale", true}};
  std::string error;
  CHECK_FALSE(UnwrapPluginRunResult(std::move(response), "timeshift_buffer", result, error));
  CHECK(result.is_null());
}

TEST_CASE("IsPluginRunErrorEnvelope recognises only Dispatcharr 0.34's error-with-result shape", "[PluginRunResult]")
{
  // The 0.34 shape: PluginRunAPIView's answer to a PluginActionResultError.
  CHECK(IsPluginRunErrorEnvelope(
      {{"success", false}, {"error", "dead"}, {"result", {{"status", "error"}, {"fatal", true}}}}));

  // The other non-2xx answers carry no result: a disabled plugin, a timed-out wait, an exception in run().
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"success", false}, {"error", "Plugin is disabled"}}));
  CHECK_FALSE(IsPluginRunErrorEnvelope(
      {{"success", false}, {"error", "Plugin action timed out waiting for a response"}, {"task_id", "t"}}));
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"success", false}, {"error", "x"}, {"result", nullptr}}));
  // A proxy's or Django's own error page, and anything that is not an object.
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"detail", "x"}}));
  CHECK_FALSE(IsPluginRunErrorEnvelope(json()));
  CHECK_FALSE(IsPluginRunErrorEnvelope(json::array({1})));
  // "success" must be a real false, not missing or another type.
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"result", {{"status", "error"}}}}));
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"success", 0}, {"result", {{"status", "error"}}}}));
  CHECK_FALSE(IsPluginRunErrorEnvelope({{"success", true}, {"result", {{"status", "ok"}}}}));
}
