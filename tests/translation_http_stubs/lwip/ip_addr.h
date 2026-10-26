#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct ip_addr {
    uint32_t address;
    bool v6;
} ip_addr_t;

#define IP_IS_V6(addr) ((addr)->v6)

char *ipaddr_ntoa_r(const ip_addr_t *address, char *buffer, int capacity);
