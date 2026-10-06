#include "DvrAccess.h"

#include "JsonFieldUtil.h"

#include <nlohmann/json.hpp>

namespace dispatcharr
{

DvrAccess ComputeDvrAccess(int userLevel, const std::string& customDvrAccess)
{
  if (userLevel >= 10)
    return DvrAccess::kManage;
  if (userLevel < 1)
    return DvrAccess::kNone;
  if (customDvrAccess == "none")
    return DvrAccess::kNone;
  if (customDvrAccess == "manage")
    return DvrAccess::kManage;
  return DvrAccess::kView;
}

bool ParseDvrAccessFromUserJson(const nlohmann::json& user, DvrAccess& out)
{
  if (!user.is_object() || !user.contains("user_level") || !user["user_level"].is_number())
    return false;
  std::string customAccess;
  if (user.contains("custom_properties") && user["custom_properties"].is_object())
    customAccess = FieldOr<std::string>(user["custom_properties"], "dvr_access", "");
  out = ComputeDvrAccess(FieldOr(user, "user_level", 0), customAccess);
  return true;
}

} // namespace dispatcharr
