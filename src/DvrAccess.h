#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace dispatcharr
{

// A Dispatcharr account's DVR access level, mirroring the server's own
// `get_dvr_access()` (apps/channels/dvr_access.py): admins (user_level >= 10)
// always manage, streamers (user_level < 1) have none whatever their custom
// properties say, and a standard user takes `custom_properties.dvr_access`
// ("none" / "view" / "manage"), with anything absent or unrecognized meaning
// "view" -- so a Standard account nobody configured is view-only, not none.
//
// Why the addon needs it: every create / update / delete / stop / extend /
// rename the DVR actions make requires "manage" (IsAdminOrDVRManager), so on a
// view-only account Kodi offered Record, Delete, Rename and Stop that could only
// ever fail with a generic error (docs/CLOSED_ITEMS.md, "View-only account is offered
// Record/Delete/Rename/Stop"). With the level known, GetCapabilities() stops
// advertising them.
enum class DvrAccess
{
  kNone,
  kView,
  kManage,
};

DvrAccess ComputeDvrAccess(int userLevel, const std::string& customDvrAccess);

inline bool CanManageDvr(DvrAccess access)
{
  return access == DvrAccess::kManage;
}

// Reads `user_level` and `custom_properties.dvr_access` from a
// `GET /api/accounts/users/me/` response. False when the response carries no
// usable `user_level` (an access level cannot be derived without it), in which
// case the caller keeps its current value rather than guessing.
bool ParseDvrAccessFromUserJson(const nlohmann::json& user, DvrAccess& out);

} // namespace dispatcharr
