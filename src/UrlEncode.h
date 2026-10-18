#pragma once

#include <string>

namespace dispatcharr
{

// URL-encodes a query parameter value (titles/tvg_ids can contain
// spaces, '&', etc.): every byte except RFC 3986's unreserved characters
// (A-Z a-z 0-9 - . _ ~) becomes %XX with upper-case hex, byte for byte the
// output of curl_easy_escape. Pulled out into its own file specifically so
// it's unit-testable standalone; see ../tests/test_url_encode.cpp.
std::string UrlEncode(const std::string& value);

} // namespace dispatcharr
