#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace dispatcharr
{

// What Dispatcharr told us about the calling account's own API key, from
// GET /api/accounts/api-keys/ (APIKeyViewSet.list(), apps/accounts/api_views.py,
// confirmed against Dispatcharr's own real current upstream source and live
// against a real instance: `{"key": user.api_key}` for the authenticated
// caller, permission class `Authenticated` only -- any role, not just
// admin).
//
// Exists because Dispatcharr keeps exactly ONE key per account and
// `POST /api/accounts/api-keys/generate/` overwrites it unconditionally
// (`target_user.api_key = secrets.token_urlsafe(40)`), so every call to it
// silently revokes the key for every other client of that account -- other
// Kodi installs, scripts, MCP/automation tools. This addon used to call it
// on first run and on every 401; a report against 0.11.0 showed simply
// enabling the addon with an empty `api_key` setting broke every other
// client on the account (docs/OPEN_ITEMS.md). The fix is to read the
// account's existing key first and only generate when there genuinely
// isn't one.
enum class ServerApiKeyState
{
  kPresent,        // the account has a key; ServerApiKeyLookup::key holds it
  kNone,           // the account definitively has no key yet ({"key": null} or "")
  kUnsupported,    // this server can't tell us: an unrecognizable response shape, or the
                   // endpoint itself refused with a non-transient 4xx (an older Dispatcharr)
  kTransientError, // couldn't reach or read the server right now (no response, 5xx, 408,
                   // 429, or a 2xx/3xx whose body wasn't usable)
};

struct ServerApiKeyLookup
{
  ServerApiKeyState state = ServerApiKeyState::kTransientError;
  std::string key; // only meaningful when state == kPresent
};

// Classifies a *successful* response body from GET /api/accounts/api-keys/.
// `{"key": "<non-empty>"}` is kPresent; `{"key": null}`, `""` and a
// whitespace-only string are kNone (the model column is
// `CharField(null=True, blank=True)`, so both null and "" mean "never
// generated one"); anything else -- not an object, no "key" member, or a
// "key" that isn't a string or null -- is kUnsupported rather than a guess.
ServerApiKeyLookup ParseApiKeyListResponse(const nlohmann::json& response);

// Classifies a *failed* fetch by its HTTP status (0 == no response at all,
// per DispatcharrClient::Request()'s own httpStatusOut convention). A
// transport failure, 5xx, 408, 429, or a 2xx/3xx whose body couldn't be
// used is kTransientError -- the outcome is genuinely unknown, and
// generating a key blind on such a failure could revoke a perfectly good
// one for every other client. Any other 4xx (404/405 from an older
// Dispatcharr without the endpoint, 401/403 from a role that can't use
// it) is kUnsupported: retrying won't change it, so the pre-existing
// generate fallback is all that's left.
ServerApiKeyLookup ClassifyApiKeyLookupFailure(long httpStatus);

enum class ApiKeyRecoveryAction
{
  kUseServerKey, // adopt the account's existing key; no rotation, nobody else is affected
  kGenerate,     // the account has no key (or this server can't say) -- generate one
  kFail,         // couldn't find out right now -- don't rotate blind, let the caller retry later
};

// The single decision every "I need a working API key" path
// (DispatcharrClient::ObtainApiKey()) makes from a lookup. Note kPresent
// is always adopted even when it equals the key that was just rejected:
// Dispatcharr resolves a key with a plain `User.objects.get(api_key=...)`
// per request (no caching), so an equal key means the 401 wasn't the
// key's fault (an inactive account also answers 401) and rotating it
// would only break other clients for nothing -- the caller's own
// retry-once-then-give-up loop bounds the cost of trying it again.
ApiKeyRecoveryAction DecideApiKeyRecovery(const ServerApiKeyLookup& lookup);

enum class GeneratedKeyReconcileAction
{
  kKeepGenerated, // the key we just generated is still the account's current one (or we can't tell)
  kAdoptServerKey // another client generated a key after ours -- theirs is the account's key now
};

// What to do right after DispatcharrClient::ObtainApiKey() generated a key,
// once it has re-read the account's key to check it stuck. Two clients that
// both find the account keyless at the same moment each generate one, and
// Dispatcharr keeps only the last: the first client's freshly minted key is
// already dead by the time it retries the request that needed it, so that
// open failed once (seen live with a Linux and a Windows client opening at the
// same instant: a single "Playback failed", recovered on the next attempt).
// Re-reading and adopting the account's actual key when it differs from the
// one just generated resolves any collision where the other client's generate
// landed before the re-read -- the near-simultaneous case that actually
// occurs. It moves the remaining failure window rather than closing it: a
// generate landing between the re-read and the retry, roughly one round trip
// later, still costs that one open (the mid-stream 401 recovery paths absorb
// the same thing during playback).
//
// Anything short of a positively-read, different key keeps the generated one:
// a lookup that says "none" (the key was revoked again in between -- generating
// again would only invite a loop), can't say, or failed transiently gives no
// better key to switch to.
GeneratedKeyReconcileAction ReconcileGeneratedApiKey(const std::string& generatedKey,
                                                     const ServerApiKeyLookup& verification);

// The two decisions behind keeping Kodi's stored `api_key` setting in step with the key this addon is really
// using, when Kodi's settings dialog swallowed the addon's own write (docs/OPEN_ITEMS.md, "SetSetting*()
// swallowed while the settings dialog is open"). While the addon's own settings dialog is open, Kodi routes
// SetSetting*() into the dialog's pending value instead of storing it, so a cancelled dialog leaves the OLD key
// stored while this addon already runs on the new one -- and the next settings save re-delivers that old key,
// which looked like a user edit and restarted the instance (tearing down playback) for no real change.
// `storedAfterDroppedWrite` is what a read-back found Kodi still holding right after the write that did not
// land; empty when no write has been dropped.

enum class ApiKeyDelivery
{
  kUnchanged,       // the delivered key is the one already in use
  kStaleRedelivery, // Kodi handing back the key it still stored after our write was dropped: not an edit
  kChanged          // anything else: the user entered a different key
};

// Classifies a key arriving through OnAddonSettingChanged(). A stale re-delivery must neither restart the
// instance nor replace the key in use (that key is revoked server-side: Dispatcharr keeps one per account).
ApiKeyDelivery ClassifyApiKeyDelivery(const std::string& delivered, const std::string& currentKey,
                                      const std::string& storedAfterDroppedWrite);

enum class ApiKeyStoreAction
{
  kNothingToDo, // the stored setting already is the key in use
  kRewrite,     // still holding the stale key: write the current one again
  kStop         // holds something else (the user entered a key): leave it alone, the restart path handles it
};

// What the retry that re-asserts the key should do, given what the setting holds now.
ApiKeyStoreAction DecideApiKeyStoreAction(const std::string& storedNow, const std::string& currentKey,
                                          const std::string& storedAfterDroppedWrite);

} // namespace dispatcharr
