#pragma once

/*
 * Host-test I/O seam. Production uses lwIP's select/getsockopt through
 * <sys/socket.h> and <sys/select.h>. The focused transport test replaces only
 * those two OS calls so the production transport and HTTP framing code run
 * unchanged against deterministic TCP readiness/error results.
 *
 * Compile the transport with:
 *   -include tests/translation_http_stubs/suite_transport_test_io.h
 */

#include <sys/select.h>
#include <sys/socket.h>

#define select suite_test_select
#define getsockopt suite_test_getsockopt

int suite_test_select(int nfds, fd_set *readfds, fd_set *writefds,
                      fd_set *exceptfds, struct timeval *timeout);
int suite_test_getsockopt(int fd, int level, int optname, void *optval,
                          socklen_t *optlen);
