#pragma once

#include "lwip/err.h"

typedef void (*tcpip_callback_fn)(void *ctx);

err_t tcpip_try_callback(tcpip_callback_fn function, void *ctx);
