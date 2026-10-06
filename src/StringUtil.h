#pragma once

#include <cstdint>
#include <string>

namespace dispatcharr
{

// Standard Base64 (RFC 4648), no line wrapping -- used for
// WebSocketClient's Sec-WebSocket-Key handshake header, pulled out here
// (and kept dependency-free) specifically so it's unit-testable
// standalone; see ../tests/test_string_util.cpp.
std::string Base64Encode(const uint8_t* data, size_t len);

// Locale-independent ASCII character helpers. std::tolower()/std::isalnum() follow the process's
// LC_CTYPE, which Kodi sets to the user's region: under a Turkish locale tolower('I') is not 'i', and in
// a single-byte locale the bytes of a UTF-8 sequence can be changed (found by the thirteenth hardening
// sweep, plausible, not reproduced because the lab host has only the C locale). Everything here that
// compares header names, host names or EPG category keywords wants plain ASCII.
inline char AsciiToLower(unsigned char c)
{
  return static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
}
inline bool IsAsciiAlnum(unsigned char c)
{
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// ASCII lowercase, used for case-insensitive HTTP header comparison.
std::string ToLower(std::string s);

// Wraps a bare IPv6 literal (e.g. "::1" or "2001:db8::1") in brackets
// before it's used as the host portion of a "host:port" URL/Host header
// -- per RFC 3986, "http://::1:8080" is ambiguous (the parser can't tell
// where the address ends and the port begins) and must instead be
// "http://[::1]:8080". A hostname or IPv4 literal never contains a
// colon, so detecting one by the presence of ':' is sufficient to tell
// the two apart; already-bracketed input (starts with '[') is returned
// unchanged rather than double-wrapped. Shared by every "host:port" URL
// this addon builds: DispatcharrClient::BaseUrl(), BuildTimeshiftPlaylistUrl()/
// BuildTimeshiftSegmentBaseUrl() (PluginUrlUtil.h), and
// BuildWebSocketHandshakeRequest()'s own Host: header (WebSocketHandshake.h).
std::string FormatHostForUrl(const std::string& host);

} // namespace dispatcharr
