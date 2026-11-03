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
// usable `user_level` (an access level cannot be derived without it), leaving
// `out` untouched; DvrAccessFromAnsweredUserJson() decides what that means.
bool ParseDvrAccessFromUserJson(const nlohmann::json& user, DvrAccess& out);

// The access level to act on once `GET /api/accounts/users/me/` has answered. An answer with no usable `user_level`
// fails open to kManage, as a failed request does, but unlike a failed request it settles the question (levelKnown
// false says which): asking the same server again gets the same answer, and the connection report waits for the level
// to be settled (ConnectionReport.h), so retrying it would keep Kodi showing the server as unreachable for the whole
// session. Every Dispatcharr seen so far sends the field; a proxy or a fork that strips it is the case this covers.
DvrAccess DvrAccessFromAnsweredUserJson(const nlohmann::json& user, bool& levelKnown);

} // namespace dispatcharr
