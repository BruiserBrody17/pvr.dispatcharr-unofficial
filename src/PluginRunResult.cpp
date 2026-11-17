#include "PluginRunResult.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace dispatcharr
{

bool UnwrapPluginRunResult(nlohmann::json&& response, const char* pluginLabel, nlohmann::json& resultOut,
                           std::string& error)
{
  const bool success = FieldOr(response, "success", false);
  if (!success)
    error = FieldOr<std::string>(response, "error", std::string(pluginLabel) + " plugin call did not succeed");
  const auto it = response.is_object() ? response.find("result") : response.end();
  if (it != response.end() && (success || it->is_object()))
    resultOut = std::move(*it);
  else
    resultOut = nlohmann::json();
  if (!success)
    return false;
  if (FieldOr<std::string>(resultOut, "status", "") != "ok")
  {
    error = FieldOr<std::string>(resultOut, "message", std::string(pluginLabel) + " plugin returned an error");
    return false;
  }
  return true;
}

bool IsPluginRunErrorEnvelope(const nlohmann::json& body)
{
  if (!body.is_object())
    return false;
  const auto success = body.find("success");
  const auto result = body.find("result");
  return success != body.end() && success->is_boolean() && !success->get<bool>() && result != body.end() &&
         result->is_object();
}

} // namespace dispatcharr
