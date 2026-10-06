#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace dispatcharr
{

// Parses an API response body without ever throwing. Found by the 2026-10-04 second hardening
// sweep, proven against the vendored nlohmann: json::parse() throws json::out_of_range (not
// json::parse_error) for an integer literal of about 1.8e308 or more, and Request() caught only
// parse_error, so a response carrying one (Python's json.dumps(10**400) emits it, and a recording's
// custom_properties is a JSON field a DVR-manage account can store anything in) escaped every caller
// and, on the recording-refresh thread, ended in std::terminate for every Kodi client of the server on
// each start while that row existed. std::bad_alloc on a body too big for a constrained address space
// takes the same path, so any std::exception is a failed parse here, not just the JSON library's own.
bool ParseJsonResponseBody(const std::string& body, nlohmann::json& out, std::string& error);

// A server's error response body made fit to go into an error string: control characters (which
// include the ESC of a terminal escape sequence and the NULs and newlines of a multi-line page)
// become spaces and the length is cut to `maxBytes` on a UTF-8 code-point boundary, with a note of
// how much was dropped. A non-2xx body went into the error text verbatim, and those strings reach
// kodi.log, which users paste into public issues: a proxy's HTML error page, or a Django debug 500
// page (which lists the request headers, Authorization included), could be up to the 128 MiB response
// ceiling (found by the 2026-10-04 eighth hardening sweep).
std::string SanitizeServerErrorBody(const std::string& body, std::size_t maxBytes = 512);

} // namespace dispatcharr
