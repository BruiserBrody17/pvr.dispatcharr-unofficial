#include "SocketWait.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace dispatcharr
{

int WaitForSocketReady(curl_socket_t sockfd, bool forWrite, std::chrono::milliseconds remaining)
{
  if (sockfd == CURL_SOCKET_BAD)
    return -1;
  if (remaining.count() < 0)
    remaining = std::chrono::milliseconds(0);

#ifdef _WIN32
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(sockfd, &fds);
  timeval tv{};
  tv.tv_sec = static_cast<long>(remaining.count() / 1000);
  tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
  return select(0, forWrite ? nullptr : &fds, forWrite ? &fds : nullptr, nullptr, &tv);
#else
  pollfd pfd{};
  pfd.fd = sockfd;
  pfd.events = forWrite ? POLLOUT : POLLIN;
  // poll() takes an int; no wait here is ever long enough to overflow it, but clamp anyway.
  const long long ms = remaining.count() > 0x7fffffffLL ? 0x7fffffffLL : remaining.count();
  const int rc = poll(&pfd, 1, static_cast<int>(ms));
  if (rc > 0 && (pfd.revents & POLLNVAL))
    return -1; // not an open descriptor
  return rc;
#endif
}

} // namespace dispatcharr
