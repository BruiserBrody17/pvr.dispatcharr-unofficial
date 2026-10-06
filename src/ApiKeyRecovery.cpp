#include "ApiKeyRecovery.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace dispatcharr
{

ServerApiKeyLookup ParseApiKeyListResponse(const nlohmann::json& response)
{
  ServerApiKeyLookup lookup;
  lookup.state = ServerApiKeyState::kUnsupported;
  if (!response.is_object() || !response.contains("key"))
    return lookup;

  const nlohmann::json& key = response.at("key");
  if (key.is_null())
  {
    lookup.state = ServerApiKeyState::kNone;
    return lookup;
  }
  if (!key.is_string())
    return lookup;

  std::string value = key.get<std::string>();
  bool blank = std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
  if (blank)
  {
    lookup.state = ServerApiKeyState::kNone;
    return lookup;
  }
  lookup.state = ServerApiKeyState::kPresent;
  lookup.key = std::move(value);
  return lookup;
}

ServerApiKeyLookup ClassifyApiKeyLookupFailure(long httpStatus)
{
  ServerApiKeyLookup lookup;
  bool clientError = httpStatus >= 400 && httpStatus < 500;
  bool transientClientError = httpStatus == 408 || httpStatus == 429;
  lookup.state =
      (clientError && !transientClientError) ? ServerApiKeyState::kUnsupported : ServerApiKeyState::kTransientError;
  return lookup;
}

ApiKeyRecoveryAction DecideApiKeyRecovery(const ServerApiKeyLookup& lookup)
{
  switch (lookup.state)
  {
  case ServerApiKeyState::kPresent:
    // A kPresent lookup with an empty key can't come out of
    // ParseApiKeyListResponse(), but adopting an empty string would leave
    // this client with no key at all -- treat it as "none" instead.
    return lookup.key.empty() ? ApiKeyRecoveryAction::kGenerate : ApiKeyRecoveryAction::kUseServerKey;
  case ServerApiKeyState::kNone:
  case ServerApiKeyState::kUnsupported:
    return ApiKeyRecoveryAction::kGenerate;
  case ServerApiKeyState::kTransientError:
    return ApiKeyRecoveryAction::kFail;
  }
  return ApiKeyRecoveryAction::kFail;
}

GeneratedKeyReconcileAction ReconcileGeneratedApiKey(const std::string& generatedKey,
                                                     const ServerApiKeyLookup& verification)
{
  if (verification.state == ServerApiKeyState::kPresent && !verification.key.empty() &&
      verification.key != generatedKey)
    return GeneratedKeyReconcileAction::kAdoptServerKey;
  return GeneratedKeyReconcileAction::kKeepGenerated;
}

ApiKeyDelivery ClassifyApiKeyDelivery(const std::string& delivered, const std::string& currentKey,
                                      const std::string& storedAfterDroppedWrite)
{
  if (delivered == currentKey)
    return ApiKeyDelivery::kUnchanged;
  if (!storedAfterDroppedWrite.empty() && delivered == storedAfterDroppedWrite)
    return ApiKeyDelivery::kStaleRedelivery;
  return ApiKeyDelivery::kChanged;
}

ApiKeyStoreAction DecideApiKeyStoreAction(const std::string& storedNow, const std::string& currentKey,
                                          const std::string& storedAfterDroppedWrite)
{
  if (storedNow == currentKey)
    return ApiKeyStoreAction::kNothingToDo;
  if (!storedAfterDroppedWrite.empty() && storedNow == storedAfterDroppedWrite)
    return ApiKeyStoreAction::kRewrite;
  return ApiKeyStoreAction::kStop;
}

} // namespace dispatcharr
