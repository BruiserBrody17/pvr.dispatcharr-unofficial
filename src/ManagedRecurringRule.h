#pragma once

#include "DispatcharrClient.h"

#include <ctime>
#include <string>
#include <vector>

namespace dispatcharr
{

// Which recurring rules this addon owns, and so may renew.
//
// Dispatcharr requires an end date on every recurring rule, so the addon gives
// the rules it creates a rolling window and pushes the end date forward in the
// background. It used to do that to every enabled rule it could see -- including
// one made in Dispatcharr's own web UI with a deliberate end date, which was
// silently extended again and again -- and, unable to tell them apart, refused to
// extend any rule that had already expired, so a rule of its own that lapsed
// because no Kodi ran for a few weeks stayed dead (docs/OPEN_ITEMS.md, "Can't tell
// addon-managed rolling rules from user-created ones" and "Rolling recurring rules
// die if no Kodi runs for ~15-30 days").
//
// A rule's stored fields have no free-form slot, so ownership is a visible tag at
// the end of its name: a rule created from Kodi is stored as `Show name [Kodi]`.
// The addon shows Kodi the name without it and puts it back on any rename. A tag
// the user removes just means the rule stops being renewed, which fails safe, and
// one added by hand makes the addon renew it, which is the user's decision. Only
// the addon and the user can add the tag, so another Kodi install on the same
// account recognises the rules the first one made.
inline constexpr const char* kManagedRuleMarker = "[Kodi]";

// True when `serverName` carries the tag: it is exactly the tag, or ends with a
// space and the tag.
bool IsManagedRuleName(const std::string& serverName);

// The name Kodi should show for a stored name: the tag removed (with the space
// before it). Names without the tag pass through untouched.
std::string StripManagedRuleMarker(const std::string& serverName);

// The name to store for a name as Kodi knows it. Idempotent; an empty name becomes
// the bare tag.
std::string AddManagedRuleMarker(const std::string& displayName);

// The longest recurring-rule name Dispatcharr accepts (RecurringRecordingRule.name max_length=255,
// characters, not bytes).
constexpr std::size_t kMaxRuleNameCharacters = 255;

// AddManagedRuleMarker() for a name typed in Kodi: the display name is cut to the characters that
// leave room for the " [Kodi]" tag, on a UTF-8 code-point boundary (never mid-sequence), so adding the
// tag cannot push a name the server would have accepted past its limit and fail the create or rename
// with a 400 (found by the 2026-10-04 seventh hardening sweep). A name that already carries the tag is
// returned as it is; adoption of an existing rule keeps using AddManagedRuleMarker() and skips one that
// would not fit instead of renaming the user's rule.
std::string AddManagedRuleMarkerBounded(const std::string& displayName);

// Whether `rule` has an occurrence recording now or about to start within
// `safetyMarginSeconds`, which a renewal (or the one-time marking below) must not
// disturb -- a rule PATCH makes Dispatcharr drop and regenerate its future
// occurrences. `!haveRecordings` (the list could not be read) counts as "yes":
// err toward leaving it alone. A missed occurrence stuck at `scheduled` with its
// whole window already past does not count (see ShouldRenewRecurringRule()).
bool HasActiveOrImminentOccurrence(const RecurringRule& rule, const std::vector<Recording>& recordings,
                                   bool haveRecordings, time_t now, int safetyMarginSeconds);

// The one-time marking of rules that existed before ownership was tracked.
//
// Until then every rule was renewed, so the first run of a build that knows about
// ownership treats the rules already there as the addon's and tags them -- once.
// `done` ends the process for good; `pending` are the rule ids still to tag (a rule
// with a recording starting is left until a later cycle, so the work can span
// several and a restart in the middle must not lose track of it).
struct RecurringRuleAdoptionState
{
  bool known = false; // false: never evaluated (no state file)
  bool done = false;
  std::vector<int> pending;
};

std::string SerializeAdoptionState(const RecurringRuleAdoptionState& state);

// Unreadable text gives an unknown state (and false), which re-evaluates -- safe,
// because evaluation sees a tagged rule and concludes the work was already done.
bool ParseAdoptionState(const std::string& text, RecurringRuleAdoptionState& out);

// The state after looking at the account's rules for the first time. Done at once
// when there are no rules (so one made in the web UI later is never swept up) or any
// rule is already tagged (another install, or this one before the state file was
// lost, got there first). Otherwise every rule that is not tagged and has not already
// run its course -- an enabled one past its end date is exactly the "deliberately
// finished" case ownership exists to leave alone -- is pending.
RecurringRuleAdoptionState EvaluateInitialAdoption(const std::vector<RecurringRule>& rules, time_t now);

// One cycle of the marking above: which of the `pending` rule ids to tag now
// (`toTag`), and which to leave for a later cycle (`deferred`, a rule with a
// recording running or about to start -- a rule PATCH makes Dispatcharr drop and
// regenerate the rule's future occurrences). A pending id whose rule is gone, or
// already carries the tag, is in neither list: nothing left to do for it. The
// caller tags `toTag` and keeps `deferred` plus whatever failed to tag as the
// next cycle's `pending`; the marking is done once that is empty. Pulled out of
// PVRDispatcharr::AdoptExistingRecurringRulesOnce() so the resumable part of a
// one-time, destructive-if-wrong migration has a test.
struct RecurringRuleAdoptionPlan
{
  std::vector<int> toTag;
  std::vector<int> deferred;
};

RecurringRuleAdoptionPlan PlanRecurringRuleAdoption(const std::vector<int>& pending,
                                                    const std::vector<RecurringRule>& rules,
                                                    const std::vector<Recording>& recordings, bool haveRecordings,
                                                    time_t now, int safetyMarginSeconds);

// What a failed attempt to tag one rule means for that rule (found by the 2026-10-04 hardening
// sweep: a failure was retried every refresh cycle forever, an ERROR line each time, and the
// one-time marking never finished). A 4xx other than "try again" (a name that, with the
// " [Kodi]" tag, fails the server's validation, a rule the account may not edit, a rule
// deleted meanwhile) will fail identically next time: the rule is skipped for good. Anything
// else -- no response, a 5xx, 401 (an authentication problem, not the rule's), 408, 429 -- is
// retried, but only a bounded number of times a
// session so a server that answers every PATCH with an error is not hit every cycle.
enum class AdoptionFailureKind
{
  kPermanent,  // skip this rule; it will never be tagged by this addon
  kRetryLater, // keep it pending and try again next cycle
};

AdoptionFailureKind ClassifyAdoptionPatchFailure(long httpStatus);

// Consecutive failed attempts for one rule after which this session stops trying it (it stays
// pending in the persisted state, so the next start tries again).
constexpr int kMaxAdoptionRetriesPerSession = 5;

} // namespace dispatcharr
