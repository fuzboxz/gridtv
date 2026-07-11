// MinGW-w64 compat shim, force-included ONLY when building the VLC plugin with
// MinGW. The CI runner's MinGW ships no <poll.h>, but VLC's vlc_threads.h
// references poll() and struct pollfd. vlc_threads.h only uses a `struct pollfd*`
// and defines a static inline vlc_poll() that this plugin never calls, so bare
// declarations are enough — no struct body, no definition, nothing to link.
#pragma once

struct pollfd;

#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *fds, unsigned nfds, int timeout);
#ifdef __cplusplus
}
#endif
