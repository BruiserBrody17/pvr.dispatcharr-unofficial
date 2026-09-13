#pragma once

#include <string>

namespace dispatcharr
{

// URL-encodes a query parameter value (titles/tvg_ids can contain
// spaces, '&', etc.), via curl_easy_escape -- needs a handle but doesn't
// use it for anything beyond the escape itself, so a scratch one is fine
// here. Pulled out into its own file specifically so it's unit-testable
// standalone (against real libcurl, no Kodi dependency); see
// ../tests/test_url_encode.cpp.
std::string UrlEncode(const std::string& value);

} // namespace dispatcharr
