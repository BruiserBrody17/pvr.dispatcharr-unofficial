#pragma once

#include <nlohmann/json.hpp>

namespace dispatcharr
{

// nlohmann::json's item.value(key, default) only substitutes the default
// when the key is *absent* -- if the key is present but explicitly JSON
// null (which Django REST Framework serializers commonly emit for empty
// nullable fields, e.g. a channel with no logo), converting it to a
// non-null-supporting type like int throws an uncaught type_error that
// aborts whatever's parsing the response (confirmed against a real
// instance: ~0.4% of a 9360-channel list had an explicit `"logo_id": null`).
// This wraps every field read so a null or wrong-typed value degrades to
// the default instead of throwing. Header-only (a template) and pulled
// out specifically so it's unit-testable standalone; see
// ../tests/test_json_field_util.cpp.
template <typename T> T FieldOr(const nlohmann::json& item, const char* key, T defaultValue)
{
  if (!item.contains(key) || item[key].is_null())
    return defaultValue;
  try
  {
    return item[key].template get<T>();
  }
  catch (const nlohmann::json::exception&)
  {
    return defaultValue;
  }
}

} // namespace dispatcharr
