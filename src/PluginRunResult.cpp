#include "PluginRunResult.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

bool UnwrapPluginRunResult(const nlohmann::json& response, const char* pluginLabel, nlohmann::json& resultOut,
                           std::string& error)
{
  if (!FieldOr(response, "success", false))
  {
    error = FieldOr<std::string>(response, "error", std::string(pluginLabel) + " plugin call did not succeed");
    return false;
  }
  resultOut = response.contains("result") ? response["result"] : nlohmann::json();
  if (FieldOr<std::string>(resultOut, "status", "") != "ok")
  {
    error = FieldOr<std::string>(resultOut, "message", std::string(pluginLabel) + " plugin returned an error");
    return false;
  }
  return true;
}

} // namespace dispatcharr
