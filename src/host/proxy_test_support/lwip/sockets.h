#pragma once
#include <sys/socket.h>
#include <unistd.h>
/* The sandbox blocks sendto even on a local socket pair. The adapter's sink
 * needs only stream writes here; use write so tests need no network access. */
static inline ssize_t proxy_test_send(int fd, const void *data, size_t len, int flags)
{ (void)flags; return write(fd, data, len); }
#define send proxy_test_send
