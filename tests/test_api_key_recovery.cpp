#include "ApiKeyRecovery.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

using namespace dispatcharr;
using nlohmann::json;

// ---------------------------------------------------------------------
// ParseApiKeyListResponse
// ---------------------------------------------------------------------

TEST_CASE("ParseApiKeyListResponse reads an existing key", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup = ParseApiKeyListResponse(json{{"key", "abc123"}});
  CHECK(lookup.state == ServerApiKeyState::kPresent);
  CHECK(lookup.key == "abc123");
}

TEST_CASE("ParseApiKeyListResponse ignores unrelated members alongside the key", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup = ParseApiKeyListResponse(json{{"key", "abc123"}, {"user", {{"id", 1}}}});
  CHECK(lookup.state == ServerApiKeyState::kPresent);
  CHECK(lookup.key == "abc123");
}

TEST_CASE("ParseApiKeyListResponse treats a null key as none -- the account never generated one", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup = ParseApiKeyListResponse(json{{"key", nullptr}});
  CHECK(lookup.state == ServerApiKeyState::kNone);
  CHECK(lookup.key.empty());
}

TEST_CASE("ParseApiKeyListResponse treats an empty or whitespace-only key as none", "[ApiKeyRecovery]")
{
  // The model column is CharField(null=True, blank=True): "" is as much
  // "no key" as null is.
  CHECK(ParseApiKeyListResponse(json{{"key", ""}}).state == ServerApiKeyState::kNone);
  CHECK(ParseApiKeyListResponse(json{{"key", "   "}}).state == ServerApiKeyState::kNone);
  CHECK(ParseApiKeyListResponse(json{{"key", "\n\t"}}).state == ServerApiKeyState::kNone);
}

TEST_CASE("ParseApiKeyListResponse keeps a key's own characters untouched", "[ApiKeyRecovery]")
{
  // secrets.token_urlsafe() output includes '-' and '_'.
  ServerApiKeyLookup lookup = ParseApiKeyListResponse(json{{"key", "Ab-_9xZ"}});
  CHECK(lookup.state == ServerApiKeyState::kPresent);
  CHECK(lookup.key == "Ab-_9xZ");
}

TEST_CASE("ParseApiKeyListResponse is unsupported when there is no key member at all", "[ApiKeyRecovery]")
{
  CHECK(ParseApiKeyListResponse(json::object()).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json{{"api_key", "abc"}}).state == ServerApiKeyState::kUnsupported);
}

TEST_CASE("ParseApiKeyListResponse is unsupported for a non-object response", "[ApiKeyRecovery]")
{
  CHECK(ParseApiKeyListResponse(json()).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json::array()).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json("abc")).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json(42)).state == ServerApiKeyState::kUnsupported);
}

TEST_CASE("ParseApiKeyListResponse is unsupported when key has an unexpected type", "[ApiKeyRecovery]")
{
  CHECK(ParseApiKeyListResponse(json{{"key", 123}}).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json{{"key", true}}).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json{{"key", json::array({"a"})}}).state == ServerApiKeyState::kUnsupported);
  CHECK(ParseApiKeyListResponse(json{{"key", json::object()}}).state == ServerApiKeyState::kUnsupported);
}

// ---------------------------------------------------------------------
// ClassifyApiKeyLookupFailure
// ---------------------------------------------------------------------

TEST_CASE("ClassifyApiKeyLookupFailure treats no response at all as transient", "[ApiKeyRecovery]")
{
  CHECK(ClassifyApiKeyLookupFailure(0).state == ServerApiKeyState::kTransientError);
}

TEST_CASE("ClassifyApiKeyLookupFailure treats server errors and throttling as transient", "[ApiKeyRecovery]")
{
  for (long status : {408L, 429L, 500L, 502L, 503L, 504L, 599L})
    CHECK(ClassifyApiKeyLookupFailure(status).state == ServerApiKeyState::kTransientError);
}

TEST_CASE("ClassifyApiKeyLookupFailure treats a 2xx/3xx failure as transient", "[ApiKeyRecovery]")
{
  // A 200 whose body wasn't valid JSON (a misbehaving proxy) reaches here
  // too -- the outcome is unknown, not "this server has no key".
  CHECK(ClassifyApiKeyLookupFailure(200).state == ServerApiKeyState::kTransientError);
  CHECK(ClassifyApiKeyLookupFailure(302).state == ServerApiKeyState::kTransientError);
}

TEST_CASE("ClassifyApiKeyLookupFailure treats other client errors as unsupported", "[ApiKeyRecovery]")
{
  for (long status : {400L, 401L, 403L, 404L, 405L, 410L, 422L})
    CHECK(ClassifyApiKeyLookupFailure(status).state == ServerApiKeyState::kUnsupported);
}

// ---------------------------------------------------------------------
// DecideApiKeyRecovery
// ---------------------------------------------------------------------

TEST_CASE("DecideApiKeyRecovery adopts an existing key instead of generating one -- the real bug this fixes",
          "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = "existing";
  CHECK(DecideApiKeyRecovery(lookup) == ApiKeyRecoveryAction::kUseServerKey);
}

TEST_CASE("DecideApiKeyRecovery generates only when the account has no key", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kNone;
  CHECK(DecideApiKeyRecovery(lookup) == ApiKeyRecoveryAction::kGenerate);
}

TEST_CASE("DecideApiKeyRecovery falls back to generating when the server can't report a key", "[ApiKeyRecovery]")
{
  // An older Dispatcharr without the endpoint or field: the pre-existing
  // behavior is all that's left.
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kUnsupported;
  CHECK(DecideApiKeyRecovery(lookup) == ApiKeyRecoveryAction::kGenerate);
}

TEST_CASE("DecideApiKeyRecovery refuses to generate blind on a transient failure", "[ApiKeyRecovery]")
{
  // Rotating an unread key could revoke a perfectly good one for every
  // other client of the account.
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kTransientError;
  CHECK(DecideApiKeyRecovery(lookup) == ApiKeyRecoveryAction::kFail);
}

TEST_CASE("DecideApiKeyRecovery never adopts an empty key", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = "";
  CHECK(DecideApiKeyRecovery(lookup) == ApiKeyRecoveryAction::kGenerate);
}

TEST_CASE("A default-constructed lookup is treated as a transient failure, never as permission to generate",
          "[ApiKeyRecovery]")
{
  CHECK(DecideApiKeyRecovery(ServerApiKeyLookup{}) == ApiKeyRecoveryAction::kFail);
}

TEST_CASE("End to end: parse then decide, for every shape the server can answer with", "[ApiKeyRecovery]")
{
  CHECK(DecideApiKeyRecovery(ParseApiKeyListResponse(json{{"key", "k"}})) == ApiKeyRecoveryAction::kUseServerKey);
  CHECK(DecideApiKeyRecovery(ParseApiKeyListResponse(json{{"key", nullptr}})) == ApiKeyRecoveryAction::kGenerate);
  CHECK(DecideApiKeyRecovery(ParseApiKeyListResponse(json::object())) == ApiKeyRecoveryAction::kGenerate);
  CHECK(DecideApiKeyRecovery(ClassifyApiKeyLookupFailure(503)) == ApiKeyRecoveryAction::kFail);
  CHECK(DecideApiKeyRecovery(ClassifyApiKeyLookupFailure(404)) == ApiKeyRecoveryAction::kGenerate);
}

// ---------------------------------------------------------------------
// ReconcileGeneratedApiKey
// ---------------------------------------------------------------------

TEST_CASE("ReconcileGeneratedApiKey keeps the generated key when the server still reports it", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = "mine";
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("ReconcileGeneratedApiKey adopts a different key another client generated afterward -- the real race",
          "[ApiKeyRecovery]")
{
  // Two clients both found the account keyless and both generated; the
  // server kept the later one, so the earlier client's key is already dead.
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = "theirs";
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kAdoptServerKey);
}

TEST_CASE("ReconcileGeneratedApiKey keeps the generated key when the server reports no key", "[ApiKeyRecovery]")
{
  // Revoked again in between: generating once more could only start a loop.
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kNone;
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("ReconcileGeneratedApiKey keeps the generated key when the check itself couldn't say", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kUnsupported;
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kKeepGenerated);
  lookup.state = ServerApiKeyState::kTransientError;
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("ReconcileGeneratedApiKey never adopts an empty key", "[ApiKeyRecovery]")
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = "";
  CHECK(ReconcileGeneratedApiKey("mine", lookup) == GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("A default-constructed verification lookup never overrides the generated key", "[ApiKeyRecovery]")
{
  CHECK(ReconcileGeneratedApiKey("mine", ServerApiKeyLookup{}) == GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("End to end: parse the re-read response, then reconcile", "[ApiKeyRecovery]")
{
  CHECK(ReconcileGeneratedApiKey("mine", ParseApiKeyListResponse(json{{"key", "mine"}})) ==
        GeneratedKeyReconcileAction::kKeepGenerated);
  CHECK(ReconcileGeneratedApiKey("mine", ParseApiKeyListResponse(json{{"key", "theirs"}})) ==
        GeneratedKeyReconcileAction::kAdoptServerKey);
  CHECK(ReconcileGeneratedApiKey("mine", ParseApiKeyListResponse(json{{"key", nullptr}})) ==
        GeneratedKeyReconcileAction::kKeepGenerated);
  CHECK(ReconcileGeneratedApiKey("mine", ParseApiKeyListResponse(json::object())) ==
        GeneratedKeyReconcileAction::kKeepGenerated);
  CHECK(ReconcileGeneratedApiKey("mine", ClassifyApiKeyLookupFailure(503)) ==
        GeneratedKeyReconcileAction::kKeepGenerated);
}

TEST_CASE("A key equal to the one in use is no change", "[ApiKeyRecovery]")
{
  CHECK(ClassifyApiKeyDelivery("new", "new", "") == ApiKeyDelivery::kUnchanged);
  CHECK(ClassifyApiKeyDelivery("new", "new", "old") == ApiKeyDelivery::kUnchanged);
}

TEST_CASE("The stored key handed back after a dropped write is a stale redelivery, not an edit", "[ApiKeyRecovery]")
{
  CHECK(ClassifyApiKeyDelivery("old", "new", "old") == ApiKeyDelivery::kStaleRedelivery);
}

TEST_CASE("Without a dropped write any different key is a user change", "[ApiKeyRecovery]")
{
  CHECK(ClassifyApiKeyDelivery("old", "new", "") == ApiKeyDelivery::kChanged);
  CHECK(ClassifyApiKeyDelivery("typed", "new", "old") == ApiKeyDelivery::kChanged);
  CHECK(ClassifyApiKeyDelivery("", "new", "old") == ApiKeyDelivery::kChanged);
}

TEST_CASE("The re-assert retry rewrites only while the stale key is still stored", "[ApiKeyRecovery]")
{
  CHECK(DecideApiKeyStoreAction("new", "new", "old") == ApiKeyStoreAction::kNothingToDo);
  CHECK(DecideApiKeyStoreAction("old", "new", "old") == ApiKeyStoreAction::kRewrite);
  CHECK(DecideApiKeyStoreAction("typed", "new", "old") == ApiKeyStoreAction::kStop);
  CHECK(DecideApiKeyStoreAction("old", "new", "") == ApiKeyStoreAction::kStop);
}

TEST_CASE("an empty stored value after a dropped write is not mistaken for a stale re-delivery", "[ApiKeyRecovery]")
{
  // The user cleared the key (delivered ""), and nothing stale was ever remembered (""): that is a real change,
  // not "Kodi handed back the key it still had".
  CHECK(ClassifyApiKeyDelivery("", "abc", "") == ApiKeyDelivery::kChanged);
  CHECK(DecideApiKeyStoreAction("", "abc", "") == ApiKeyStoreAction::kStop);
  // With a remembered stale key, the same shapes behave as documented.
  CHECK(ClassifyApiKeyDelivery("old", "abc", "old") == ApiKeyDelivery::kStaleRedelivery);
  CHECK(DecideApiKeyStoreAction("old", "abc", "old") == ApiKeyStoreAction::kRewrite);
  CHECK(ClassifyApiKeyDelivery("abc", "abc", "") == ApiKeyDelivery::kUnchanged);
}
