#pragma once

#include <curl/curl.h>

#include <chrono>

namespace dispatcharr
{

// Waits up to `remaining` for `sockfd` to become ready for writing (forWrite) or
// reading. >0: ready, or a spurious wake-up (callers loop and re-check, as before);
// 0: timed out; <0: the wait itself failed -- including an invalid socket.
//
// Shared by WebSocketClient::SendAll()/FillBuffer(). It used select(), whose fd_set
// cannot hold a descriptor numbered FD_SETSIZE (1024) or more -- FD_SET() on one is
// undefined behaviour (a glibc _FORTIFY_SOURCE abort on some builds), reachable in a
// long-running Kodi process that already holds that many descriptors -- and which
// FD_SET(-1) hit just the same when curl reported no active socket
// (CURL_SOCKET_BAD). POSIX now uses poll(), which has no such limit, and an invalid
// socket is refused up front (docs/OPEN_ITEMS.md, "Lower-severity WebSocketClient.cpp
// gaps"). Windows keeps select(): its fd_set counts sockets rather than indexing by
// value, so a single socket always fits.
int WaitForSocketReady(curl_socket_t sockfd, bool forWrite, std::chrono::milliseconds remaining);

} // namespace dispatcharr
