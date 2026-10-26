#pragma once

#include "lwip/err.h"
#include "lwip/ip_addr.h"

typedef void (*dns_found_callback)(const char *name, const ip_addr_t *address,
                                   void *callback_arg);

err_t dns_gethostbyname(const char *hostname, ip_addr_t *address,
                        dns_found_callback found, void *callback_arg);
