#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>

namespace dispatcharr
{

// nlohmann::json's item.value(key, default) only substitutes the default
// when the key is *absent* -- if the key is present but explicitly JSON
// null (which Django REST Framework serializers commonly emit for empty
// nullable fields, e.g. a channel with no logo), converting it to a
// non-null-supporting type like int throws an uncaught type_error that
// aborts whatever's parsing the response (confirmed against a real
// instance: a small fraction of a real channel list had an explicit `"logo_id": null`).
// This wraps every field read so a null or wrong-typed value degrades to
// the default instead of throwing. Header-only (a template) and pulled
// out specifically so it's unit-testable standalone; see
// ../tests/test_json_field_util.cpp.
//
// A JSON number too large for int64_t/uint64_t (or one written with a
// decimal point/exponent) is stored by nlohmann's own lexer as a
// `number_float`, not a parse error -- and converting a `number_float`
// to an integral T is a plain `static_cast` with no range check inside
// nlohmann itself (from_json.hpp's `get_arithmetic_value()`), so it
// neither throws (nothing here would catch it) nor clamps: casting an
// out-of-range double to an integral type is undefined behavior in C++.
// Found via a project-wide review (a 29th-pass audit, confirmed with
// UBSan against the vendored header, not reproduced live): both
// companion plugins can hand this addon a finite-but-huge or negative
// value that reaches here unclamped (`recording_edl`'s `_parse_edl`,
// `timeshift_buffer`'s `_get_live_manifest`), the same UB class the
// 28th pass fixed on the C++-only in-progress-recording playlist path
// (`M3u8SegmentParser`) but that fix couldn't reach this shared,
// lower-level helper every other JSON field read in this codebase goes
// through. Only relevant for an integral T -- `is_number_float()` on a
// non-integral T (double, bool, std::string, ...) always falls through
// to the plain `get<T>()` below unchanged.
template <typename T> T FieldOr(const nlohmann::json& item, const char* key, T defaultValue)
{
  if (!item.contains(key) || item[key].is_null())
    return defaultValue;
  try
  {
    const nlohmann::json& value = item[key];
    if constexpr (std::is_integral<T>::value)
    {
      if (value.is_number_float())
      {
        double d = value.template get<double>();
        // The upper bound is deliberately an exclusive "one past max"
        // value, not numeric_limits<T>::max() itself compared with `>`
        // (fixed 2026-09-27, a 52nd-pass audit, fixing a real, confirmed
        // UB gap the 29th-pass fix above didn't cover, found via a
        // project-wide review, confirmed with UBSan against the vendored
        // header, not reproduced live): for a 64-bit T, max() (e.g.
        // INT64_MAX, 2^63-1) isn't exactly representable as a double --
        // it rounds UP to exactly 2^63, the same value a genuine
        // out-of-range input like 9223372036854775807.0 also parses to.
        // The old `d > static_cast<double>(max)` compared d against that
        // *same, already-rounded-up* value, so an input landing exactly
        // on it read as `d > lim` == false and reached the unchecked
        // static_cast below -- undefined behavior. What actually fixes
        // the 64-bit case is switching the comparison from `>` to `>=`
        // (corrected 2026-09-27, a 53rd-pass audit, fixing a real,
        // confirmed inversion in this comment's own original wording,
        // found via a project-wide review, confirmed by direct UBSan
        // testing, not itself independently reproduced): for a 64-bit T,
        // `max + 1.0` numerically rounds right back to the same value as
        // `max` itself (the gap between representable doubles that large
        // is already far bigger than 1), so the `+ 1.0` genuinely is a
        // no-op there -- but it's essential, not a no-op, for a narrower
        // type where `max` IS exactly representable (`int`, `uint32_t`,
        // ...): switching bare `>` straight to `>=` with no adjustment
        // would there wrongly reject the true max value itself (e.g.
        // `d == 2147483647.0` satisfying `d >= 2147483647.0`). Both
        // together give the correct "one past the true max" exclusive
        // boundary for every width -- 2^(bits-1) for a signed type,
        // 2^bits for unsigned -- regardless of whether double's own
        // rounding already put max() there or `+ 1.0` needed to.
        double upperBoundExclusive = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
        if (!std::isfinite(d) || d < static_cast<double>(std::numeric_limits<T>::lowest()) || d >= upperBoundExclusive)
          return defaultValue;
      }
    }
    if constexpr (std::is_integral<T>::value && !std::is_same<T, bool>::value)
    {
      // An integer outside T's range used to wrap silently: nlohmann's integral get<>() is a plain
      // cast, so {"id": 4294967301} read as the int 5, an unsigned 2^64-1 as the int64 -1, and an
      // EDL "type": 4294967297 as 1, past the 0-3 coercion (found by the 2026-10-04 sixth hardening
      // sweep, proven under UBSan). Treated the same as the floating-point case above: the default.
      if (value.is_number_unsigned())
      {
        if (value.template get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
          return defaultValue;
      }
      else if (value.is_number_integer())
      {
        const std::int64_t v = value.template get<std::int64_t>();
        if (v < 0)
        {
          if (std::is_unsigned<T>::value || v < static_cast<std::int64_t>(std::numeric_limits<T>::lowest()))
            return defaultValue;
        }
        else if (static_cast<std::uint64_t>(v) > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
        {
          return defaultValue;
        }
      }
    }
    return value.template get<T>();
  }
  catch (const nlohmann::json::exception&)
  {
    return defaultValue;
  }
}

// DispatcharrClient::SetDvrOffsetMinutes()'s own merge-preserving PATCH
// body: sets only the offset key(s) actually being changed on top of
// whatever this dvr_settings row's value blob already held (comskip
// settings, path templates, ... and, as of 2026-09-26, the *other*
// offset too when it's not being changed this call), returning
// everything else exactly as read rather than reconstructing it --
// Dispatcharr's own PATCH replaces the whole `value` blob rather than
// merging it server-side, so a body built from just these two keys
// would silently wipe every other one instead of leaving it alone (a
// real incident, not hypothetical: a partial PATCH wiped a real
// instance's own dvr_settings this exact way). Takes `value` by value
// (a small, already-parsed JSON object, cheap to copy) so the caller's
// own copy read from the row is left untouched.
//
// `preMinutes`/`postMinutes` are nullable: nullptr leaves that key
// exactly as `value` already had it -- fix for a real, confirmed bug
// found via a project-wide review (a 20th-pass audit), not itself
// independently reproduced: `PVRDispatcharr::OnAddonSettingChanged()`'s
// own "Dispatcharr's own DVR padding is global-only, always push both
// together" convention used to read whichever offset *didn't* just
// change from Kodi's own local copy of it -- synced from Dispatcharr
// only once, at addon construction (see the constructor's own comment) --
// so a genuine server-side change to the untouched offset (Dispatcharr's
// own web UI, a second Kodi install pointed at the same instance) made
// after that sync got silently reverted the next time the *other*
// offset was edited from this Kodi, even though `SetDvrOffsetMinutes()`
// had just re-fetched this row's real, current value one call earlier --
// discarded here instead of used. The real caller (SetDvrOffsetMinutes())
// now always passes exactly one non-null and one nullptr.
inline nlohmann::json MergeDvrOffsetMinutes(nlohmann::json value, const int* preMinutes, const int* postMinutes)
{
  if (preMinutes)
    value["pre_offset_minutes"] = *preMinutes;
  if (postMinutes)
    value["post_offset_minutes"] = *postMinutes;
  return value;
}

// The results/bare-array response-shape tolerance duplicated across
// GetChannels()/GetChannelGroups()/GetRecordings()/GetRecurringRules()/
// FindCoreSettingsRow() (and, with its own extra "rules" key first,
// GetTimerRules()): Dispatcharr's list endpoints are usually a bare JSON
// array, confirmed live, but some wrap it in Django REST Framework's own
// {"results": [...]} pagination envelope -- this tolerates either shape
// rather than assuming one. Checks `keys` in order and returns the first
// one present; returns `response` itself unchanged if none match (the
// plain-array case). Returns a reference into `response`, so it must
// outlive the result, same as every call site already requires.
inline const nlohmann::json& UnwrapListResponse(const nlohmann::json& response,
                                                std::initializer_list<const char*> keys = {"results"})
{
  for (const char* key : keys)
  {
    if (response.contains(key))
      return response[key];
  }
  return response;
}

// True when `response` is DRF's own {"results": [...], "next": ...}
// pagination envelope AND `next` is genuinely non-null -- i.e. there's
// a second page UnwrapListResponse() above would otherwise silently
// discard, keeping only the first page as if it were the complete
// list. A real, confirmed risk found via a project-wide review, not
// reproduced live: DispatcharrClient.h's own top-of-file API summary
// already documents the channel list specifically as "paginated", and
// every UnwrapListResponse() call site's own comment already knew DRF's
// pagination envelope carries "next" -- but nothing ever checked it,
// so a genuinely multi-page response was cached as a complete,
// successful fetch missing everything past page one. `next` being
// absent or null (the normal, confirmed-live case for every endpoint
// tested so far) means either pagination is disabled for that endpoint
// or the whole list genuinely fit on one page -- both fine to proceed
// with; only a genuinely truncated response is a problem. Deliberately
// fails loudly rather than following `next` and fetching more pages:
// a smaller, safer fix that turns a silent wrong answer into a clear,
// diagnosable error without touching any call site's own HTTP-request
// flow -- see docs/OPEN_ITEMS.md for the fuller pagination-following
// fix this doesn't attempt.
inline bool IsTruncatedPaginatedResponse(const nlohmann::json& response)
{
  return response.contains("next") && !response["next"].is_null();
}

// DispatcharrClient::FindCoreSettingsRow()'s own per-row match result.
// valueWasObject is false whenever the matched row's own "value" field
// isn't a real JSON object (missing, null, or a different type) -- see
// FindSettingsRowByKey()'s own comment for why this matters beyond just
// GetDvrOffsetMinutes()/GetSystemTimeZone()'s read-only use of `value`.
struct SettingsRowMatch
{
  bool found = false;
  int id = 0;
  nlohmann::json value = nlohmann::json::object();
  bool valueWasObject = false;
};

// The pure per-row matching core of DispatcharrClient::FindCoreSettingsRow()
// -- scans an already-fetched, already-unwrapped (see UnwrapListResponse()
// above) /api/core/settings/ list for the first row whose own "key"
// field matches `key`.
//
// valueWasObject exists specifically so DispatcharrClient::SetDvrOffsetMinutes()
// can refuse a destructive PATCH rather than silently proceeding: fixing
// a real, confirmed gap where a row whose real "value" wasn't an object
// (however that happened server-side) got silently replaced with an
// empty {} here, which SetDvrOffsetMinutes() would then merge the two
// offset keys into and PATCH straight back -- wiping every other key the
// real value actually held, the same incident MergeDvrOffsetMinutes()
// above exists to prevent, just reached via a different path
// (FindCoreSettingsRow() discarding the real value before the merge
// function ever saw it, rather than the merge itself dropping keys).
// GetDvrOffsetMinutes()/GetSystemTimeZone() only ever read `value`, so
// substituting {} for either of them is harmless and they can ignore
// this field.
inline SettingsRowMatch FindSettingsRowByKey(const nlohmann::json& list, const std::string& key)
{
  for (const auto& row : list)
  {
    if (FieldOr<std::string>(row, "key", "") == key)
    {
      SettingsRowMatch match;
      match.found = true;
      match.id = FieldOr(row, "id", 0);
      match.valueWasObject = row.contains("value") && row["value"].is_object();
      if (match.valueWasObject)
        match.value = row["value"];
      return match;
    }
  }
  return SettingsRowMatch{};
}

// Serializes an outgoing request body, failing (false, with `error` set)
// instead of throwing when `body` holds a string that isn't valid UTF-8 --
// the serialization core of DispatcharrClient::Request(). Fix for a real,
// confirmed crash path (added 2026-09-27, a 66th-pass audit, found via a
// project-wide review, confirmed by direct compilation against this
// project's own vendored nlohmann/json v3.11.3 and against Kodi's own real
// current SDK source, not reproduced live): a plain `body.dump()` uses
// nlohmann's default strict error handler, which throws
// `type_error.316` ("incomplete UTF-8 string") for invalid UTF-8, and
// nothing between Request() and Kodi's own C callback wrappers
// (`ADDON_AddTimer`/`ADDON_UpdateTimer`/..., kodi/addon-instance/PVR.h,
// plain passthroughs with no try/catch) catches it -- std::terminate,
// taking the whole Kodi process down. Reachable without anything
// adversarial: PVR_TIMER/PVR_RECORDING's `strTitle` is a fixed
// `char[PVR_ADDON_NAME_STRING_LENGTH]` (1024) that both Kodi's own
// `CPVRTimerInfoTag::FillAddonData()` and this addon's own
// `kodi::addon::PVRTimer::SetTitle()` fill via a byte-level
// `strncpy(..., 1023)`, which can split a multi-byte UTF-8 sequence --
// and a Dispatcharr recording's title can exceed 1023 bytes, since its
// own `update-metadata` action (apps/channels/api_views.py, confirmed
// against Dispatcharr's own real current upstream source) stores a
// user-supplied title into `custom_properties.program.title` with no
// length limit. Failing the one request is deliberately preferred over
// nlohmann's `error_handler_t::replace`: the only way a non-UTF-8 string
// reaches here is truncation or corruption, and silently writing a
// U+FFFD-mangled value back to Dispatcharr (a rename, a rule name) would
// be a quiet server-side data change rather than a visible failure. See
// ../tests/test_json_field_util.cpp.
inline bool TrySerializeJsonBody(const nlohmann::json& body, std::string& out, std::string& error)
{
  try
  {
    out = body.dump();
    return true;
  }
  catch (const nlohmann::json::type_error& e)
  {
    error = std::string("Couldn't serialize request body: ") + e.what();
    return false;
  }
}

} // namespace dispatcharr
