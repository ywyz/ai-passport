#define _DEFAULT_SOURCE
#include "../main/suite_translation_http.h"

#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "suite_clock.h"
#include "suite_connection.h"
#include "suite_transport_test_io.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP "0123456789abcdef0123456789abcdef"
#define JOB "fedcba9876543210fedcba9876543210"
#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define AUDIO 9644U
#define TOKEN "0123456789abcdef0123456789abcdef"
#define ORIGIN "https://translate.example"

typedef struct {
    esp_tls_last_error_t error;
    int sock_errno;
} esp_tls_error_storage_test_t;

struct esp_tls {
    esp_tls_conn_state_t state;
    int fd;
    esp_tls_error_storage_test_t storage;
    bool handle_present;
};

static int64_t s_now_us;
static suite_conn_state_t s_connection = SUITE_CONN_ONLINE;
static suite_time_tracker_t s_tracker = {
    .state = SUITE_TIME_SYNCED,
    .last_sync_epoch = 1000,
};
static bool s_trusted = true;
static int s_net_takes;
static int s_net_gives;
static bool s_net_fail;
static int s_tls_init_calls;
static int s_tls_destroy_calls;
static int s_handshake_calls;
static int s_handshake_progress;
static bool s_handshake_fail;
static int s_crt_attach_calls;
static char s_last_common_name[128];
static char s_last_ip[64];
static int s_last_port;
static bool s_last_non_block;
static bool s_last_skip_common_name;
static bool s_tcp_ready;
static int s_select_timeouts;
static int s_select_calls;
static int s_socket_error;
static bool s_select_fail;
static bool s_getsockopt_fail;
static bool s_tls_init_null;
static bool s_tls_handle_present;
static bool s_sockfd_fail;
static bool s_state_get_fail;
static bool s_async1_fail;
static bool s_async2_fail;
static bool s_async_state_fail;
static bool s_async2_bad_state;
static bool s_tcp_stuck;
static bool s_handshake_loop_fail;
static int s_tls_last_error;
static int s_tls_code;
static int s_tls_flags;
static int s_tls_sys_errno;
static int s_heap_free;
static int s_heap_largest;
static int s_heap_largest_limit; /* 0 disables the simulated largest block */
static int s_heap_largest_rejects;
static int s_heap_free_calls;
static int s_heap_largest_calls;
static int s_heap_malloc_calls;
static int s_heap_malloc_free_calls;
static int s_heap_malloc_live;
static bool s_heap_malloc_fail;
static void *s_heap_malloc_last;
static size_t s_heap_malloc_last_size;
static size_t s_heap_malloc_last_cap;
static bool s_heap_malloc_before_tls;
static int s_heap_malloc_at_first_write = -1;
static int s_heap_malloc_nonzero_at_free;
static bool s_heap_cancel_on_alloc;
static bool s_heap_expire_on_alloc;
static int64_t s_heap_advance_on_alloc_us;
static bool s_cancel_on_free;
static bool s_expire_on_free;
static int64_t s_heap_advance_on_free_us;
static bool s_cancel_after_alloc;
static bool s_cancel_on_destroy;
static bool s_errno_cancel_armed;
static bool s_errno_cancel_fire;

static uint8_t s_request[16384];
static size_t s_request_length;
static int s_write_calls;
static int s_write_fail_at = -1;
static int s_write_want_remaining;
static size_t s_short_write;

static uint8_t *s_response;
static size_t s_response_length;
static size_t s_response_capacity;
static size_t s_response_position;
static size_t s_response_served_limit;
static int s_read_calls;
static int s_read_want_remaining;
static int s_read_fail_at = -1;
static size_t s_read_chunk = 7;

static int s_cancel_calls;
static int s_cancel_after;

enum {
    DNS_OK_MODE = 0,
    DNS_PROGRESS_MODE,
    DNS_FAIL_MODE,
    DNS_NEVER_MODE
};
static int s_dns_mode = DNS_OK_MODE;
static int s_dns_delay_ticks;
static int s_dns_delay_left;
static bool s_dns_pending;
static bool s_dns_success = true;
static dns_found_callback s_dns_callback;
static void *s_dns_argument;
static ip_addr_t s_dns_address = {.address = 0xc000020aU, .v6 = false};
static int s_dns_calls;
static bool s_in_tcpip;
static bool s_tcpip_fail;
static bool s_tcpip_defer;
static tcpip_callback_fn s_deferred_callback;
static void *s_deferred_context;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void vTaskDelay(TickType_t ticks)
{
    s_now_us += (int64_t)ticks * 1000;
    if (s_dns_pending) {
        if (s_dns_delay_left > 0) s_dns_delay_left--;
        if (s_dns_delay_left == 0) {
            ip_addr_t address = s_dns_address;
            s_dns_pending = false;
            s_in_tcpip = true;
            s_dns_callback("translate.example",
                           s_dns_success ? &address : NULL, s_dns_argument);
            s_in_tcpip = false;
        }
    }
}

suite_conn_state_t suite_connection_state(void)
{
    return s_connection;
}

int64_t suite_connection_now(void)
{
    return (int64_t)(s_now_us / 1000000);
}

const suite_time_tracker_t *suite_connection_time(void)
{
    return &s_tracker;
}

bool suite_time_is_trusted(const suite_time_tracker_t *tracker,
                           int64_t now_epoch)
{
    (void)tracker;
    (void)now_epoch;
    return s_trusted;
}

bool suite_net_take(unsigned timeout_ms)
{
    (void)timeout_ms;
    s_net_takes++;
    return !s_net_fail;
}

void suite_net_give(void)
{
    s_net_gives++;
}

err_t dns_gethostbyname(const char *hostname, ip_addr_t *address,
                        dns_found_callback found, void *callback_arg)
{
    (void)hostname;
    assert(s_in_tcpip);
    s_dns_calls++;
    if (s_dns_mode == DNS_FAIL_MODE) return ERR_ARG;
    if (s_dns_mode == DNS_OK_MODE) {
        *address = s_dns_address;
        return ERR_OK;
    }
    s_dns_callback = found;
    s_dns_argument = callback_arg;
    s_dns_pending = true;
    s_dns_delay_left = s_dns_mode == DNS_NEVER_MODE ? 1000000 :
        s_dns_delay_ticks;
    return ERR_INPROGRESS;
}

char *ipaddr_ntoa_r(const ip_addr_t *address, char *buffer, int capacity)
{
    const char *text = address->v6 ? "2001:db8::1" : "192.0.2.10";
    if ((int)strlen(text) + 1 > capacity) return NULL;
    strcpy(buffer, text);
    return buffer;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    s_crt_attach_calls++;
    return ESP_OK;
}

esp_tls_t *esp_tls_init(void)
{
    s_tls_init_calls++;
    if (s_tls_init_null) return NULL;
    esp_tls_t *tls = calloc(1, sizeof(*tls));
    assert(tls);
    tls->state = ESP_TLS_INIT;
    tls->fd = 3;
    tls->handle_present = s_tls_handle_present;
    tls->storage.error.last_error = s_tls_last_error;
    tls->storage.error.esp_tls_error_code = s_tls_code;
    tls->storage.error.esp_tls_flags = s_tls_flags;
    tls->storage.sock_errno = s_tls_sys_errno;
    return tls;
}

int esp_tls_conn_new_async(const char *hostname, int hostlen, int port,
                           const esp_tls_cfg_t *cfg, esp_tls_t *tls)
{
    s_handshake_calls++;
    if (s_heap_malloc_calls > 0) s_heap_malloc_before_tls = true;
    if (hostlen >= (int)sizeof(s_last_ip)) return -1;
    memcpy(s_last_ip, hostname, (size_t)hostlen);
    s_last_ip[hostlen] = 0;
    s_last_port = port;
    s_last_non_block = cfg->non_block;
    s_last_skip_common_name = cfg->skip_common_name;
    if (cfg->crt_bundle_attach) cfg->crt_bundle_attach(NULL);
    snprintf(s_last_common_name, sizeof(s_last_common_name), "%s",
             cfg->common_name ? cfg->common_name : "");
    if (s_handshake_fail || (s_async1_fail && cfg->non_block &&
                             tls->state == ESP_TLS_INIT)) {
        tls->state = ESP_TLS_FAIL;
        if (s_errno_cancel_armed) s_errno_cancel_fire = true;
        return -1;
    }
    if (s_async_state_fail && tls->state == ESP_TLS_INIT) {
        tls->state = ESP_TLS_FAIL;
        return 0;
    }
    if (!cfg->non_block) {
        /* Only the transport's one CONNECTING call that skips stale fd_sets. */
        assert(tls->state == ESP_TLS_CONNECTING);
        assert(s_tcp_ready);
        if (s_async2_fail) {
            tls->state = ESP_TLS_FAIL;
            return -1;
        }
        if (s_async2_bad_state) {
            tls->state = ESP_TLS_FAIL;
            return 0;
        }
        if (s_tcp_stuck) {
            tls->state = ESP_TLS_CONNECTING;
            return 0;
        }
        tls->state = ESP_TLS_HANDSHAKE;
        if (s_handshake_progress > 0) {
            s_handshake_progress--;
            return 0;
        }
        tls->state = ESP_TLS_DONE;
        return 1;
    }
    if (tls->state == ESP_TLS_INIT) {
        if (!s_tcp_ready) {
            tls->state = ESP_TLS_CONNECTING;
            return 0;
        }
        tls->state = ESP_TLS_HANDSHAKE;
    }
    if (tls->state == ESP_TLS_CONNECTING) {
        /* A second non-blocking CONNECTING call would reuse empty SDK sets. */
        return 0;
    }
    if (tls->state == ESP_TLS_HANDSHAKE) {
        if (s_handshake_loop_fail && s_handshake_calls > 1) {
            tls->state = ESP_TLS_FAIL;
            return -1;
        }
        if (s_tcp_stuck) {
            tls->state = ESP_TLS_CONNECTING;
            return 0;
        }
        if (s_handshake_progress > 0) {
            s_handshake_progress--;
            return 0;
        }
        tls->state = ESP_TLS_DONE;
        return 1;
    }
    return tls->state == ESP_TLS_DONE ? 1 : -1;
}

esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *sockfd)
{
    if (!tls || !sockfd || s_sockfd_fail) return ESP_ERR_INVALID_ARG;
    *sockfd = tls->fd;
    return ESP_OK;
}

esp_err_t esp_tls_get_conn_state(esp_tls_t *tls,
                                 esp_tls_conn_state_t *conn_state)
{
    if (!tls || !conn_state || s_state_get_fail) return ESP_ERR_INVALID_ARG;
    *conn_state = tls->state;
    return ESP_OK;
}

esp_err_t esp_tls_get_error_handle(esp_tls_t *tls,
                                   esp_tls_error_handle_t *handle)
{
    if (!tls || !handle || !tls->handle_present) return ESP_ERR_INVALID_ARG;
    *handle = &tls->storage.error;
    return ESP_OK;
}

esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t handle,
                                           int *esp_tls_code,
                                           int *esp_tls_flags)
{
    if (!handle) return ESP_ERR_INVALID_STATE;
    int last = handle->last_error;
    if (esp_tls_code) *esp_tls_code = handle->esp_tls_error_code;
    if (esp_tls_flags) *esp_tls_flags = handle->esp_tls_flags;
    memset(handle, 0, sizeof(*handle));
    return last;
}

esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t handle,
                                           esp_tls_error_type_t type,
                                           int *error_code)
{
    if (!handle || !error_code || type >= ESP_TLS_ERR_TYPE_MAX)
        return ESP_ERR_INVALID_ARG;
    esp_tls_error_storage_test_t *storage =
        (esp_tls_error_storage_test_t *)handle;
    int *field = NULL;
    if (type == ESP_TLS_ERR_TYPE_SYSTEM) field = &storage->sock_errno;
    else if (type == ESP_TLS_ERR_TYPE_MBEDTLS)
        field = &handle->esp_tls_error_code;
    else if (type == ESP_TLS_ERR_TYPE_MBEDTLS_CERT_FLAGS)
        field = &handle->esp_tls_flags;
    else if (type == ESP_TLS_ERR_TYPE_ESP) field = &handle->last_error;
    else return ESP_ERR_INVALID_ARG;
    *error_code = *field;
    *field = 0;
    return ESP_OK;
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    s_heap_free_calls++;
    return (size_t)s_heap_free;
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    s_heap_largest_calls++;
    return (size_t)s_heap_largest;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    s_heap_malloc_calls++;
    s_heap_malloc_last_cap = caps;
    s_heap_malloc_last_size = size;
    if (s_heap_cancel_on_alloc) s_cancel_after_alloc = true;
    if (s_heap_expire_on_alloc) s_now_us += s_heap_advance_on_alloc_us;
    if (s_heap_malloc_fail) return NULL;
    if (s_heap_largest_limit > 0 &&
        size > (size_t)s_heap_largest_limit) {
        s_heap_largest_rejects++;
        return NULL;
    }
    void *ptr = calloc(1, size);
    if (!ptr) return NULL;
    s_heap_malloc_last = ptr;
    s_heap_malloc_live++;
    return ptr;
}

void heap_caps_free(void *ptr)
{
    s_heap_malloc_free_calls++;
    if (s_cancel_on_free) s_cancel_after_alloc = true;
    if (s_expire_on_free) s_now_us += s_heap_advance_on_free_us;
    if (ptr && ptr == s_heap_malloc_last) {
        const uint8_t *bytes = ptr;
        for (size_t i = 0; i < s_heap_malloc_last_size; i++) {
            if (bytes[i] != 0) {
                s_heap_malloc_nonzero_at_free++;
                break;
            }
        }
        s_heap_malloc_last = NULL;
    }
    if (s_heap_malloc_live > 0) s_heap_malloc_live--;
    free(ptr);
}

int suite_test_select(int nfds, fd_set *readfds, fd_set *writefds,
                      fd_set *exceptfds, struct timeval *timeout)
{
    s_select_calls++;
    if (writefds) {
        assert(nfds > 0);
        assert(FD_ISSET(nfds - 1, writefds));
    }
    if (s_select_fail) {
        errno = ECONNABORTED;
        return -1;
    }
    if (s_select_timeouts > 0) {
        s_select_timeouts--;
        /* Match lwIP: a timeout returns with the supplied sets cleared. */
        if (readfds) FD_ZERO(readfds);
        if (writefds) FD_ZERO(writefds);
        if (exceptfds) FD_ZERO(exceptfds);
        if (timeout) {
            s_now_us += (int64_t)timeout->tv_sec * 1000000 +
                timeout->tv_usec;
        }
        return 0;
    }
    if (readfds) FD_ZERO(readfds);
    if (exceptfds) FD_ZERO(exceptfds);
    if (writefds) {
        fd_set expected;
        FD_ZERO(&expected);
        FD_SET(nfds - 1, &expected);
        FD_ZERO(writefds);
        FD_SET(nfds - 1, writefds);
        /* Readiness must contain only the watched descriptor. */
        assert(!memcmp(writefds, &expected, sizeof(expected)));
    }
    s_tcp_ready = true;
    return 1;
}

int suite_test_getsockopt(int fd, int level, int optname, void *optval,
                          socklen_t *optlen)
{
    (void)fd;
    if (s_getsockopt_fail) {
        errno = ENOTSOCK;
        return -1;
    }
    if (level != SOL_SOCKET || optname != SO_ERROR || !optval || !optlen)
        return -1;
    if (*optlen < (socklen_t)sizeof(int)) return -1;
    *(int *)optval = s_socket_error;
    *optlen = (socklen_t)sizeof(int);
    return 0;
}

err_t tcpip_try_callback(tcpip_callback_fn function, void *ctx)
{
    if (s_tcpip_fail) return ERR_MEM;
    if (s_tcpip_defer) {
        s_deferred_callback = function;
        s_deferred_context = ctx;
        return ERR_OK;
    }
    s_in_tcpip = true;
    function(ctx);
    s_in_tcpip = false;
    return ERR_OK;
}

static void tcpip_run_deferred(void)
{
    if (!s_deferred_callback) return;
    tcpip_callback_fn function = s_deferred_callback;
    void *context = s_deferred_context;
    s_deferred_callback = NULL;
    s_deferred_context = NULL;
    s_in_tcpip = true;
    function(context);
    s_in_tcpip = false;
}

int esp_tls_conn_destroy(esp_tls_t *tls)
{
    s_tls_destroy_calls++;
    if (s_cancel_on_destroy) s_cancel_after_alloc = true;
    /* Simulate cleanup changing the heap: capture must have happened first. */
    s_heap_free += 4096;
    s_heap_largest += 2048;
    free(tls);
    return 0;
}

ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen)
{
    (void)tls;
    s_write_calls++;
    if (s_heap_malloc_at_first_write < 0)
        s_heap_malloc_at_first_write = s_heap_malloc_calls;
    if (s_write_want_remaining > 0) {
        s_write_want_remaining--;
        return ESP_TLS_ERR_SSL_WANT_WRITE;
    }
    if (s_write_fail_at >= 0 &&
        s_request_length >= (size_t)s_write_fail_at)
        return -1;
    size_t copy = datalen;
    if (s_short_write && copy > s_short_write) copy = s_short_write;
    assert(s_request_length + copy <= sizeof(s_request));
    memcpy(s_request + s_request_length, data, copy);
    s_request_length += copy;
    return (ssize_t)copy;
}

ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data, size_t datalen)
{
    (void)tls;
    s_read_calls++;
    if (s_read_want_remaining > 0) {
        s_read_want_remaining--;
        return ESP_TLS_ERR_SSL_WANT_READ;
    }
    if (s_read_fail_at >= 0 &&
        (int)s_response_position >= s_read_fail_at)
        return -1;
    if (s_response_position >= s_response_served_limit) return 0;
    size_t available = s_response_served_limit - s_response_position;
    size_t take = datalen;
    if (take > available) take = available;
    if (take > s_read_chunk) take = s_read_chunk;
    memcpy(data, s_response + s_response_position, take);
    s_response_position += take;
    return (ssize_t)take;
}

static void dns_finish_pending(bool success)
{
    if (!s_dns_pending) return;
    ip_addr_t address = s_dns_address;
    s_dns_pending = false;
    s_in_tcpip = true;
    s_dns_callback("translate.example", success ? &address : NULL,
                   s_dns_argument);
    s_in_tcpip = false;
}

static void reset(void)
{
    if (s_deferred_callback) {
        s_tcpip_defer = false;
        tcpip_run_deferred();
    }
    dns_finish_pending(false);
    s_now_us = 1000000;
    s_connection = SUITE_CONN_ONLINE;
    s_trusted = true;
    s_net_takes = 0;
    s_net_gives = 0;
    s_net_fail = false;
    s_tls_init_calls = 0;
    s_tls_destroy_calls = 0;
    s_handshake_calls = 0;
    s_handshake_progress = 0;
    s_handshake_fail = false;
    s_crt_attach_calls = 0;
    s_last_common_name[0] = 0;
    s_last_ip[0] = 0;
    s_last_port = 0;
    s_last_non_block = false;
    s_last_skip_common_name = true;
    s_tcp_ready = false;
    s_select_timeouts = 0;
    s_select_calls = 0;
    s_socket_error = 0;
    s_select_fail = false;
    s_getsockopt_fail = false;
    s_tls_init_null = false;
    s_tls_handle_present = true;
    s_sockfd_fail = false;
    s_state_get_fail = false;
    s_async1_fail = false;
    s_async2_fail = false;
    s_async_state_fail = false;
    s_async2_bad_state = false;
    s_tcp_stuck = false;
    s_handshake_loop_fail = false;
    s_tls_last_error = 0;
    s_tls_code = 0;
    s_tls_flags = 0;
    s_tls_sys_errno = 0;
    s_heap_free = 50000;
    s_heap_largest = 4000;
    s_heap_largest_limit = 0;
    s_heap_largest_rejects = 0;
    s_heap_free_calls = 0;
    s_heap_largest_calls = 0;
    s_heap_malloc_calls = 0;
    s_heap_malloc_free_calls = 0;
    s_heap_malloc_live = 0;
    s_heap_malloc_fail = false;
    s_heap_malloc_last = NULL;
    s_heap_malloc_last_size = 0;
    s_heap_malloc_last_cap = 0;
    s_heap_malloc_before_tls = false;
    s_heap_malloc_at_first_write = -1;
    s_heap_malloc_nonzero_at_free = 0;
    s_heap_cancel_on_alloc = false;
    s_heap_expire_on_alloc = false;
    s_heap_advance_on_alloc_us = 0;
    s_cancel_on_free = false;
    s_expire_on_free = false;
    s_heap_advance_on_free_us = 0;
    s_cancel_after_alloc = false;
    s_cancel_on_destroy = false;
    s_errno_cancel_armed = false;
    s_errno_cancel_fire = false;
    memset(s_request, 0, sizeof(s_request));
    s_request_length = 0;
    s_write_calls = 0;
    s_write_fail_at = -1;
    s_write_want_remaining = 0;
    s_short_write = 0;
    free(s_response);
    s_response = NULL;
    s_response_length = 0;
    s_response_capacity = 0;
    s_response_position = 0;
    s_response_served_limit = 0;
    s_read_calls = 0;
    s_read_want_remaining = 0;
    s_read_fail_at = -1;
    s_read_chunk = 7;
    s_cancel_calls = 0;
    s_cancel_after = 0;
    s_dns_mode = DNS_OK_MODE;
    s_dns_delay_ticks = 0;
    s_dns_delay_left = 0;
    s_dns_success = true;
    s_dns_calls = 0;
    s_in_tcpip = false;
    s_tcpip_fail = false;
    s_tcpip_defer = false;
    s_deferred_callback = NULL;
    s_deferred_context = NULL;
}

static void set_response_parts(const char *status_line, const char *headers,
                               const uint8_t *body, size_t body_length)
{
    free(s_response);
    s_response_capacity = strlen(status_line) + strlen(headers) + body_length +
        8;
    s_response = malloc(s_response_capacity);
    assert(s_response);
    int written = snprintf((char *)s_response, s_response_capacity, "%s%s\r\n",
                           status_line, headers);
    assert(written > 0);
    if (body_length) memcpy(s_response + written, body, body_length);
    s_response_length = (size_t)written + body_length;
    s_response_position = 0;
    s_response_served_limit = s_response_length;
}

static void set_json_response(const char *status_line, const char *json)
{
    char headers[128];
    size_t length = strlen(json);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %lu\r\n",
             (unsigned long)length);
    set_response_parts(status_line, headers, (const uint8_t *)json, length);
}

static void set_error_response(const char *status_line, const char *code)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    set_json_response(status_line, body);
}

static bool request_contains(const char *needle)
{
    size_t length = strlen(needle);
    if (length == 0 || length > s_request_length) return false;
    for (size_t i = 0; i + length <= s_request_length; i++) {
        if (!memcmp(s_request + i, needle, length)) return true;
    }
    return false;
}

static bool request_line_is(const char *line)
{
    size_t length = strlen(line);
    return s_request_length > length &&
        !memcmp(s_request, line, length) &&
        !memcmp(s_request + length, "\r\n", 2);
}

static size_t request_body_offset(void)
{
    for (size_t i = 0; i + 4 <= s_request_length; i++) {
        if (!memcmp(s_request + i, "\r\n\r\n", 4)) return i + 4;
    }
    return s_request_length;
}

static bool test_cancel(void *ctx)
{
    (void)ctx;
    s_cancel_calls++;
    if (s_cancel_after_alloc) return true;
    return s_cancel_after > 0 && s_cancel_calls >= s_cancel_after;
}

static uint8_t s_json[SUITE_TR_HTTP_JSON_CAP];
static char s_source[SUITE_TR_PROTOCOL_TEXT_CAP];
static char s_target[SUITE_TR_PROTOCOL_TEXT_CAP];

static suite_translation_http_t s_http;

static void init_http(void)
{
    reset();
    assert(suite_translation_http_init(&s_http, ORIGIN, TOKEN, s_json,
                                       sizeof(s_json)) == SUITE_TR_HTTP_OK);
}

static void init_http_deferred(void)
{
    reset();
    assert(suite_translation_http_init(&s_http, ORIGIN, TOKEN, NULL,
                                       SUITE_TR_HTTP_JSON_CAP) ==
           SUITE_TR_HTTP_OK);
}

static void assert_deferred_storage_cycle(size_t body_length)
{
    assert(s_heap_malloc_calls == 1);
    assert(s_heap_malloc_last_size == body_length + 1U);
    assert(s_heap_malloc_last_size <= SUITE_TR_HTTP_JSON_CAP);
    assert(s_heap_malloc_last_cap ==
           (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    assert(s_heap_malloc_free_calls == 1);
    assert(s_heap_malloc_live == 0);
    assert(!s_heap_malloc_before_tls);
    /* Storage is allocated after the bounded headers were parsed, which is
       after the request bytes were written. */
    assert(s_heap_malloc_at_first_write == 0);
    assert(s_heap_malloc_nonzero_at_free == 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);
}

static void assert_deferred_storage_rejected_before_alloc(void)
{
    assert(s_heap_malloc_calls == 0);
    assert(s_heap_malloc_free_calls == 0);
    assert(s_heap_malloc_live == 0);
    assert(!s_heap_malloc_before_tls);
    /* The request was written, but no response storage was allocated. */
    assert(s_heap_malloc_at_first_write == 0);
    assert(s_request_length > 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);
}

static void assert_deferred_storage_late_alloc_failure(void)
{
    assert(s_heap_malloc_calls == 1);
    assert(s_heap_malloc_last == NULL);
    assert(s_heap_malloc_free_calls == 0);
    assert(s_heap_malloc_live == 0);
    assert(!s_heap_malloc_before_tls);
    assert(s_heap_malloc_at_first_write == 0);
    assert(s_request_length > 0);
    assert(s_write_calls > 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);
}

static void assert_deferred_storage_failed_before_alloc(void)
{
    assert(s_heap_malloc_calls == 0);
    assert(s_heap_malloc_free_calls == 0);
    assert(s_heap_malloc_live == 0);
    assert(!s_heap_malloc_before_tls);
    assert(s_heap_malloc_at_first_write == -1);
}

static suite_translation_http_call_t make_call(int64_t budget_ms)
{
    suite_translation_http_call_t call = {
        .deadline_us = s_now_us + budget_ms * 1000,
        .cancel = test_cancel,
        .cancel_ctx = NULL,
    };
    return call;
}

static suite_translation_protocol_view_t make_view(void)
{
    suite_translation_protocol_view_t view;
    suite_translation_protocol_view_init(&view, s_source, sizeof(s_source),
                                         s_target, sizeof(s_target));
    return view;
}

static suite_translation_protocol_identity_t make_identity(void)
{
    suite_translation_protocol_identity_t identity = {
        .operation_id = OP,
        .job_id = JOB,
        .generation = 7,
        .direction = SUITE_TR_PROTOCOL_DIRECTION_EN_ZH,
        .audio_bytes = AUDIO,
    };
    return identity;
}

static const char *admitted_json =
    "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
    "\"state\":\"ADMITTED\",\"source_lang\":\"en\","
    "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
    "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
    "\"error\":null}";

static const char *done_json =
    "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
    "\"state\":\"DONE\",\"source_lang\":\"en\","
    "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
    "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
    "\"error\":null,\"text_lost\":false,\"source_text\":\"hello\","
    "\"target_text\":\"\\u4f60\\u597d\"}";

static const char *cancelled_json =
    "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
    "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
    "\"target_lang\":\"zh-CN\",\"generation\":8,\"audio_bytes\":9644,"
    "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
    "\"error\":\"SHUTDOWN\"}";

static void test_ready_transport(void)
{
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_dns_mode = DNS_PROGRESS_MODE;
    s_dns_delay_ticks = 2;
    s_handshake_progress = 2;
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_ready_t ready = {0};
    suite_translation_http_info_t info;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(ready.ready);
    assert(!strcmp(ready.provider, "mimo"));
    assert(info.http_status == 200);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);
    assert(s_crt_attach_calls >= 1);
    assert(!strcmp(s_last_common_name, "translate.example"));
    assert(!strcmp(s_last_ip, "192.0.2.10"));
    assert(s_last_port == 443);
    assert(s_last_non_block);
    assert(!s_last_skip_common_name);
    assert(request_line_is("GET /suite/v1/translations/ready HTTP/1.1"));
    assert(request_contains("Authorization: Bearer " TOKEN "\r\n"));
    assert(request_contains("Connection: close\r\n"));
    assert(!request_contains(TOKEN " HTTP/1.1"));
    assert(s_handshake_calls >= 3);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":false,\"provider\":\"mimo\"}");
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(!ready.ready);

    init_http();
    set_error_response("HTTP/1.1 503 Service Unavailable\r\n", "UNAVAILABLE");
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_NOT_READY);
    assert(info.http_status == 503);
    assert(!strcmp(info.remote_error, "UNAVAILABLE"));

    init_http();
    s_connection = SUITE_CONN_OFFLINE;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_OFFLINE);
    assert(s_net_takes == 0 && s_tls_init_calls == 0);

    init_http();
    s_trusted = false;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TIME);
    assert(s_net_takes == 0 && s_tls_init_calls == 0);
}

static void test_admission_transport(void)
{
    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_view_t view = make_view();
    suite_translation_http_info_t info;
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_OK);
    assert(info.http_status == 202);
    assert(!strcmp(view.job_id, JOB));
    assert(view.generation == 7);
    assert(request_line_is("POST /suite/v1/translations HTTP/1.1"));
    assert(request_contains("Content-Type: application/json\r\n"));
    size_t body_offset = request_body_offset();
    assert(s_request_length > body_offset);
    char content_length[64];
    snprintf(content_length, sizeof(content_length),
             "Content-Length: %lu\r\n",
             (unsigned long)(s_request_length - body_offset));
    assert(request_contains(content_length));
    assert(request_contains("\"source_lang\":\"en\""));
    assert(request_contains("\"target_lang\":\"zh-CN\""));
    assert(s_request[body_offset] == '{');

    init_http();
    set_error_response("HTTP/1.1 409 Conflict\r\n", "OPERATION_CONFLICT");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_HTTP_CONFLICT);
    assert(!strcmp(info.remote_error, "OPERATION_CONFLICT"));

    init_http();
    set_error_response("HTTP/1.1 429 Too Many Requests\r\n", "QUOTA");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_HTTP_QUOTA);

    init_http();
    set_error_response("HTTP/1.1 429 Too Many Requests\r\n", "BUSY");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_HTTP_BUSY);

    init_http();
    set_error_response("HTTP/1.1 401 Unauthorized\r\n", "UNAUTHORIZED");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_HTTP_AUTH);

    init_http();
    set_response_parts("HTTP/1.1 302 Found\r\n",
                       "Content-Length: 0\r\nLocation: https://evil.example/\r\n",
                       NULL, 0);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_HTTP_REDIRECT);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Encoding: gzip\r\n"
                       "Content-Length: 10\r\n",
                       (const uint8_t *)"0123456789", 10);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_CONTENT_ENCODING);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Transfer-Encoding: chunked\r\n",
                       (const uint8_t *)"0\r\n\r\n", 5);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_TRANSFER_ENCODING);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: text/plain\r\nContent-Length: 2\r\n",
                       (const uint8_t *)"ok", 2);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_CONTENT_TYPE);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n",
                       (const uint8_t *)admitted_json, strlen(admitted_json));
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_LENGTH);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Length: 9000\r\n",
                       (const uint8_t *)"{}", 2);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_OVERSIZE);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    s_response_served_limit = s_response_length - 5;
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_TRUNCATED);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n",
                      "{\"schema\":1,\"operation_id\":\"" OP "\"}");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_PROTOCOL);
    assert(info.protocol_error == SUITE_TR_PROTOCOL_E_MISSING);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Encoding: identity\r\n"
                       "Content-Length: 1\r\n",
                       (const uint8_t *)"x", 1);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_PROTOCOL);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json; charset=utf-8\r\n"
                       "Content-Length: 2\r\n",
                       (const uint8_t *)"{}", 2);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_CONTENT_TYPE);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Length: 2\r\n"
                       "Content-Length: 2\r\n",
                       (const uint8_t *)"{}", 2);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_IO);

    init_http();
    set_response_parts("HTTP/1.1 202 Accepted\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Length: 0\r\n",
                       NULL, 0);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_LENGTH);

    init_http();
    char long_header[600];
    memset(long_header, 'a', sizeof(long_header));
    long_header[sizeof(long_header) - 1] = 0;
    set_response_parts("HTTP/1.1 202 Accepted\r\n", long_header, NULL, 0);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_IO);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call,
               "11111111111111111111111111111111",
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA, AUDIO, &view,
               &info) == SUITE_TR_HTTP_E_IDENTITY);
}

typedef struct {
    uint32_t length;
    uint32_t next;
    unsigned calls;
    size_t maximum_chunk;
    bool fail_zero;
} upload_source_t;

static size_t upload_read(void *context, uint32_t offset, uint8_t *buffer,
                          size_t capacity)
{
    upload_source_t *source = context;
    source->calls++;
    if (capacity > source->maximum_chunk)
        source->maximum_chunk = capacity;
    assert(offset == source->next);
    if (source->fail_zero) return 0;
    if (offset >= source->length) return 0;
    size_t take = source->length - offset;
    if (take > capacity) take = capacity;
    for (size_t i = 0; i < take; i++)
        buffer[i] = (uint8_t)((offset + i) & 0xff);
    source->next = offset + (uint32_t)take;
    return take;
}

static void test_upload_transport(void)
{
    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_view_t view = make_view();
    suite_translation_protocol_identity_t identity = make_identity();
    suite_translation_http_info_t info;
    upload_source_t source = {.length = AUDIO};
    s_short_write = 17;
    s_write_want_remaining = 3;
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_OK);
    assert(source.next == AUDIO);
    assert(source.calls == (AUDIO + SUITE_TR_HTTP_AUDIO_CHUNK - 1U) /
           SUITE_TR_HTTP_AUDIO_CHUNK);
    assert(source.maximum_chunk == SUITE_TR_HTTP_AUDIO_CHUNK);
    assert(request_line_is("PUT /suite/v1/translations/" JOB
                           "/audio HTTP/1.1"));
    assert(request_contains("Content-Type: audio/wav\r\n"));
    assert(request_contains("Content-Length: 9644\r\n"));
    assert(!request_contains("Transfer-Encoding"));
    size_t offset = request_body_offset();
    assert(s_request_length == offset + AUDIO);
    for (uint32_t i = 0; i < AUDIO; i++)
        assert(s_request[offset + i] == (uint8_t)(i & 0xff));
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    source = (upload_source_t){.length = AUDIO};
    s_write_fail_at = 1500;
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_IO);
    assert(info.ambiguous);
    assert(source.calls <= 32);
    assert(source.next <= AUDIO / 2 + SUITE_TR_HTTP_AUDIO_CHUNK);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    source = (upload_source_t){.length = AUDIO, .fail_zero = true};
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_IO);
    assert(info.ambiguous);
    assert(source.calls == 1);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    call.cancel = test_cancel;
    s_cancel_after = 20;
    view = make_view();
    source = (upload_source_t){.length = AUDIO};
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_CANCELLED);
    assert(info.ambiguous);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);

    init_http();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5);
    view = make_view();
    source = (upload_source_t){.length = AUDIO};
    s_write_want_remaining = 1000000;
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_DEADLINE);
    assert(info.ambiguous);
}

static void test_status_transport(void)
{
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_view_t view = make_view();
    suite_translation_protocol_identity_t identity = make_identity();
    suite_translation_http_info_t info;
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_DONE);
    assert(!strcmp(view.source, "hello"));
    assert(!strcmp(view.target, "\xe4\xbd\xa0\xe5\xa5\xbd"));
    assert(request_line_is("GET /suite/v1/translations/" JOB
                           " HTTP/1.1"));

    const char *failed =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"FAILED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"UNAVAILABLE\",\"source_text\":\"hello\"}";
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", failed);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_FAILED);
    assert(!strcmp(view.source, "hello"));
    assert(!view.has_target);
    assert(!strcmp(view.error, "UNAVAILABLE"));

    const char *lost =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"TEXT_LOST\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":5,\"target_bytes\":6,\"updated_at\":1000,"
        "\"error\":null,\"text_lost\":true}";
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", lost);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_TEXT_LOST);
    assert(view.text_lost);

    const char *expired =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"EXPIRED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":7,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"AUDIO_EXPIRED\"}";
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", expired);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_EXPIRED);
    assert(!strcmp(view.error, "AUDIO_EXPIRED"));

    const char *shutdown =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":8,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":\"SHUTDOWN\"}";
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", shutdown);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_cancel(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_CANCELLED);
    assert(!strcmp(view.error, "SHUTDOWN"));

    init_http();
    set_error_response("HTTP/1.1 404 Not Found\r\n", "NOT_FOUND");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) ==
           SUITE_TR_HTTP_E_HTTP_NOT_FOUND);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    call = make_call(5000);
    view = make_view();
    identity.generation = 8;
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_IDENTITY);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    call = make_call(5000);
    view = make_view();
    identity = make_identity();
    s_read_want_remaining = 1000000;
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_DEADLINE);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);
}

static void test_cancel_transport(void)
{
    const char *cancelled =
        "{\"schema\":1,\"operation_id\":\"" OP "\",\"job_id\":\"" JOB "\","
        "\"state\":\"CANCELLED\",\"source_lang\":\"en\","
        "\"target_lang\":\"zh-CN\",\"generation\":8,\"audio_bytes\":9644,"
        "\"source_bytes\":0,\"target_bytes\":0,\"updated_at\":1000,"
        "\"error\":null}";
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", cancelled);
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_view_t view = make_view();
    suite_translation_protocol_identity_t identity = make_identity();
    suite_translation_http_info_t info;
    assert(suite_translation_http_cancel(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_CANCELLED);
    assert(view.generation == 8);
    assert(request_line_is("DELETE /suite/v1/translations/" JOB
                           " HTTP/1.1"));

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", cancelled);
    call = make_call(5000);
    assert(suite_translation_http_cancel(&s_http, &call, &identity, NULL,
                                         &info) == SUITE_TR_HTTP_OK);

    init_http();
    set_error_response("HTTP/1.1 409 Conflict\r\n", "STATE");
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_cancel(&s_http, &call, &identity, &view,
                                         &info) ==
           SUITE_TR_HTTP_E_HTTP_CONFLICT);
    assert(!strcmp(info.remote_error, "STATE"));
}

static void test_deadlines_and_dns(void)
{
    init_http();
    s_dns_mode = DNS_NEVER_MODE;
    suite_translation_http_call_t call = make_call(40);
    suite_translation_protocol_ready_t ready;
    suite_translation_http_info_t info;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 0);
    dns_finish_pending(false);

    init_http();
    s_dns_mode = DNS_PROGRESS_MODE;
    s_dns_delay_ticks = 1000000;
    call = make_call(5000);
    call.cancel = test_cancel;
    s_cancel_after = 3;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_CANCELLED);
    assert(s_net_gives == 1);
    assert(s_tls_init_calls == 0);
    dns_finish_pending(true);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_handshake_progress = 1000000;
    call = make_call(40);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);

    init_http();
    s_net_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_NET_BUSY);
    assert(s_tls_init_calls == 0);

    init_http();
    s_dns_mode = DNS_FAIL_MODE;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DNS);
    assert(!info.tls_diag.valid);
    assert(s_net_gives == 1);

    init_http();
    s_handshake_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);
}

static void test_tcp_and_dns_workarounds(void)
{
    suite_translation_protocol_ready_t ready;
    suite_translation_http_info_t info;
    suite_translation_http_call_t call;

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_select_timeouts = 1;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(s_select_calls >= 2);
    assert(s_dns_calls > 0);
    assert(!strcmp(s_last_common_name, "translate.example"));
    assert(!s_last_skip_common_name);
    assert(s_request_length > 0);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_socket_error = 111;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert(s_request_length == 0);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);

    init_http();
    s_select_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert(s_request_length == 0);
    assert(s_tls_destroy_calls == 1);

    init_http();
    s_select_timeouts = 1000000;
    call = make_call(5000);
    call.cancel = test_cancel;
    s_cancel_after = 6;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_CANCELLED);
    assert(s_request_length == 0);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);

    init_http();
    s_select_timeouts = 1000000;
    call = make_call(20);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    assert(s_request_length == 0);
    assert(s_tls_destroy_calls == 1);
    assert(s_net_gives == 1);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_tcpip_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DNS);
    assert(s_tls_init_calls == 0);
    assert(s_net_gives == 1);

    /*
     * Dispatch is deferred past the owner deadline. The slot must remain
     * allocated until the later TCPIP callback publishes completion, then be
     * safely reusable by the next request.
     */
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_tcpip_defer = true;
    call = make_call(10);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    assert(s_deferred_callback != NULL);
    assert(s_tls_init_calls == 0);
    assert(s_net_gives == 1);
    s_tcpip_defer = false;
    tcpip_run_deferred();
    assert(s_dns_calls > 0);
    dns_finish_pending(false);

    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
}


static void tls_test_codes(void)
{
    s_tls_last_error = 0x3000;
    s_tls_code = -0x7F00;
    s_tls_flags = 0x8000;
    s_tls_sys_errno = 111;
    s_heap_free = 50000;
    s_heap_largest = 4000;
    errno = 0;
}

static void assert_tls_diag(const suite_translation_http_info_t *info,
                            unsigned stage, int local_errno, int so_error,
                            unsigned sys_source, int sys_error,
                            int esp_err, int tls_code, unsigned flags)
{
    assert(info->tls_diag.valid);
    assert(info->tls_diag.stage == stage);
    assert(info->tls_diag.local_errno == local_errno);
    assert(info->tls_diag.so_error == so_error);
    assert(info->tls_diag.sys_source == sys_source);
    assert(info->tls_diag.sys_error == sys_error);
    assert(info->tls_diag.esp_err_code == esp_err);
    assert(info->tls_diag.tls_code == tls_code);
    assert(info->tls_diag.verify_flags == flags);
    assert(info->tls_diag.free_bytes == 50000U);
    assert(info->tls_diag.largest_bytes == 4000U);
    assert(s_heap_free_calls == 1 && s_heap_largest_calls == 1);
}

static bool change_errno_cancel(void *ctx)
{
    (void)ctx;
    if (s_errno_cancel_fire) errno = EDOM;
    return false;
}

static void test_tls_diagnostic_snapshot(void)
{
    suite_translation_protocol_ready_t ready;
    suite_translation_http_info_t info;
    suite_translation_http_call_t call;

    /* init-null has no handle; no SDK code is invented. */
    init_http();
    s_tls_init_null = true;
    tls_test_codes();
    s_tls_last_error = s_tls_code = s_tls_flags = s_tls_sys_errno = 0;
    errno = 0;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 1, 0, 0, 0, 0, 0, 0, 0);
    assert(s_tls_destroy_calls == 0 && s_net_gives == 1);

    /* First async failure, with real getter read/clear semantics. */
    init_http();
    tls_test_codes();
    s_async1_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 2, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);
    assert(s_tls_destroy_calls == 1 && s_net_gives == 1);

    /* Absent error handle must not crash or fabricate codes. */
    init_http();
    tls_test_codes();
    s_tls_handle_present = false;
    s_async1_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 2, 0, 0, 0, 0, 0, 0, 0);
    assert(s_tls_destroy_calls == 1);

    /* State-getter failure. */
    init_http();
    tls_test_codes();
    s_state_get_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 3, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* ESP_TLS_FAIL observed as a state failure. */
    init_http();
    tls_test_codes();
    s_async_state_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 4, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* TCP sockfd failure. */
    init_http();
    tls_test_codes();
    s_sockfd_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 5, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* select failure keeps the local errno. */
    init_http();
    tls_test_codes();
    s_tls_sys_errno = 0;
    s_select_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 6, ECONNABORTED, 0,
                    SUITE_TR_TLS_SYS_LOCAL_ERRNO, ECONNABORTED,
                    0x3000, -0x7F00, 0x8000);

    /* getsockopt failure keeps its local errno. */
    init_http();
    tls_test_codes();
    s_tls_sys_errno = 0;
    s_getsockopt_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 7, ENOTSOCK, 0,
                    SUITE_TR_TLS_SYS_LOCAL_ERRNO, ENOTSOCK,
                    0x3000, -0x7F00, 0x8000);

    /* Explicit SO_ERROR is retained even when errno is zero. */
    init_http();
    tls_test_codes();
    s_tls_sys_errno = 0;
    s_socket_error = 111;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 8, 0, 111,
                    SUITE_TR_TLS_SYS_SO_ERROR, 111,
                    0x3000, -0x7F00, 0x8000);

    /* Second async failure. */
    init_http();
    tls_test_codes();
    s_async2_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 9, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* Post-second state invalid. */
    init_http();
    tls_test_codes();
    s_async2_bad_state = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 10, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* CONNECTING remains after TCP confirmation. */
    init_http();
    tls_test_codes();
    s_handshake_progress = 1;
    s_tcp_stuck = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 11, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* Later handshake-loop failure. */
    init_http();
    tls_test_codes();
    s_handshake_progress = 1;
    s_handshake_loop_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 12, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* Fast first-call HANDSHAKE then later fatal must map to HANDSHAKE_LOOP. */
    init_http();
    tls_test_codes();
    s_tcp_ready = true;
    s_handshake_progress = 1;
    s_handshake_loop_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 12, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);
    assert(s_handshake_calls == 2);

    /* errno is sampled immediately after the async return, before cancel. */
    init_http();
    tls_test_codes();
    s_async1_fail = true;
    s_errno_cancel_armed = true;
    errno = 0;
    call = make_call(5000);
    call.cancel = change_errno_cancel;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_tls_diag(&info, 2, 0, 0, SUITE_TR_TLS_SYS_SDK, 111,
                    0x3000, -0x7F00, 0x8000);

    /* Non-TLS paths do not fabricate a TLS snapshot. */
    init_http();
    s_connection = SUITE_CONN_OFFLINE;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_OFFLINE);
    assert(!info.tls_diag.valid);
}

static void test_protocol_errors(void)
{
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_view_t view = make_view();
    suite_translation_protocol_identity_t identity = make_identity();
    suite_translation_http_info_t info;
    s_response[0] = 'X';
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_IO);
    assert(!info.tls_diag.valid);

    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"operation_id\":\"" OP "\","
                      "\"job_id\":\"" JOB "\",\"state\":\"DONE\","
                      "\"source_lang\":\"en\",\"target_lang\":\"zh-CN\","
                      "\"generation\":7,\"audio_bytes\":9644,"
                      "\"source_bytes\":5,\"target_bytes\":6,"
                      "\"updated_at\":1000,\"error\":null,"
                      "\"source_text\":\"hello\",\"target_text\":\"\\u0000\"}");
    call = make_call(5000);
    view = make_view();
    memset(s_source, 'S', sizeof(s_source));
    memset(s_target, 'T', sizeof(s_target));
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_PROTOCOL);
    assert(info.protocol_error == SUITE_TR_PROTOCOL_E_NUL);
    assert(!info.tls_diag.valid);
    for (size_t i = 0; i < sizeof(s_source); i++) assert(s_source[i] == 0);
    for (size_t i = 0; i < sizeof(s_target); i++) assert(s_target[i] == 0);
    for (size_t i = 0; i < sizeof(s_json); i++) assert(s_json[i] == 0);

    init_http();
    set_response_parts("HTTP/1.1 200 OK\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Length: 5\r\n",
                       (const uint8_t *)"\xff\xfe\xfd\xfc\xfb", 5);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_PROTOCOL);
    assert(info.protocol_error == SUITE_TR_PROTOCOL_E_UTF8);
}

static void test_deferred_storage_all_operations(void)
{
    static const char ready_json[] =
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}";
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;
    suite_translation_protocol_view_t view;
    suite_translation_protocol_identity_t identity = make_identity();

    /* Each small realistic reply allocates exactly Content-Length + 1. */
    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", ready_json);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(ready.ready);
    assert_deferred_storage_cycle(strlen(ready_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_OK);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    upload_source_t source = {.length = AUDIO};
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_OK);
    assert(source.next == AUDIO);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_DONE);
    assert_deferred_storage_cycle(strlen(done_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", cancelled_json);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_cancel(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_OK);
    assert(view.state == SUITE_TR_PROTOCOL_STATE_CANCELLED);
    assert_deferred_storage_cycle(strlen(cancelled_json));
}

static void test_caller_buffer_not_freed(void)
{
    init_http();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    suite_translation_http_call_t call = make_call(5000);
    suite_translation_protocol_ready_t ready;
    suite_translation_http_info_t info;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(s_heap_malloc_calls == 0);
    assert(s_heap_malloc_free_calls == 0);
    for (size_t i = 0; i < sizeof(s_json); i++) assert(s_json[i] == 0);
}

static void test_deferred_storage_early_failures(void)
{
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;

    init_http_deferred();
    s_connection = SUITE_CONN_OFFLINE;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_OFFLINE);
    assert_deferred_storage_failed_before_alloc();
    assert(s_net_takes == 0 && s_tls_init_calls == 0);

    init_http_deferred();
    s_trusted = false;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TIME);
    assert_deferred_storage_failed_before_alloc();
    assert(s_net_takes == 0 && s_tls_init_calls == 0);

    init_http_deferred();
    s_handshake_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_TLS);
    assert_deferred_storage_failed_before_alloc();
    assert(s_tls_destroy_calls == 1);
    assert(s_net_takes == 1 && s_net_gives == 1);

    init_http_deferred();
    s_dns_mode = DNS_FAIL_MODE;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DNS);
    assert_deferred_storage_failed_before_alloc();
    assert(s_tls_init_calls == 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
}

static void test_deferred_storage_allocation_failures(void)
{
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;
    suite_translation_protocol_view_t view;
    suite_translation_protocol_identity_t identity = make_identity();

    /* All five calls allocate only after the request bytes were written, so
       an allocation failure is a late transport failure. */
    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    s_heap_malloc_fail = true;
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert_deferred_storage_late_alloc_failure();

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    s_heap_malloc_fail = true;
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert(request_line_is("POST /suite/v1/translations HTTP/1.1"));
    assert_deferred_storage_late_alloc_failure();

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    s_heap_malloc_fail = true;
    call = make_call(5000);
    view = make_view();
    upload_source_t source = {.length = AUDIO};
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert(source.next == AUDIO && source.calls > 0);
    assert(request_line_is("PUT /suite/v1/translations/" JOB "/audio HTTP/1.1"));
    assert_deferred_storage_late_alloc_failure();

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", done_json);
    s_heap_malloc_fail = true;
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert(request_line_is("GET /suite/v1/translations/" JOB " HTTP/1.1"));
    assert_deferred_storage_late_alloc_failure();

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", cancelled_json);
    s_heap_malloc_fail = true;
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_cancel(&s_http, &call, &identity, &view,
                                         &info) == SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert(request_line_is("DELETE /suite/v1/translations/" JOB " HTTP/1.1"));
    assert_deferred_storage_late_alloc_failure();
}

static void test_deferred_storage_terminal_release(void)
{
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;
    suite_translation_protocol_view_t view;

    init_http_deferred();
    set_error_response("HTTP/1.1 404 Not Found\r\n", "NOT_FOUND");
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_HTTP_NOT_FOUND);
    assert_deferred_storage_cycle(strlen("{\"error\":\"NOT_FOUND\"}"));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", "{");
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_PROTOCOL);
    assert_deferred_storage_cycle(1U);

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, "11111111111111111111111111111111",
               SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA, AUDIO, &view,
               &info) == SUITE_TR_HTTP_E_IDENTITY);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    s_response_served_limit = s_response_length - 5;
    call = make_call(5000);
    view = make_view();
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_TRUNCATED);
    assert(info.ambiguous);
    assert(!view.job_id[0] && !view.has_source && !view.has_target);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    call = make_call(5000);
    s_heap_expire_on_alloc = true;
    s_heap_advance_on_alloc_us = 6000000;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    /* The GET request was already written before the late allocation. */
    assert(info.ambiguous);
    assert(s_request_length > 0);
    assert_deferred_storage_cycle(strlen(
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}"));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    call = make_call(5000);
    s_heap_cancel_on_alloc = true;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_CANCELLED);
    assert(info.ambiguous);
    assert(s_request_length > 0);
    assert_deferred_storage_cycle(strlen(
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}"));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n",
                      "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}");
    call = make_call(5000);
    s_cancel_on_destroy = true;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_CANCELLED);
    assert(!info.ambiguous);
    assert(s_request_length > 0);
    assert_deferred_storage_cycle(strlen(
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}"));
}

static void test_deferred_storage_capacity_boundary(void)
{
    static const char ready_json[] =
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}";
    char *body = malloc(SUITE_TR_HTTP_JSON_CAP);
    assert(body);
    char headers[128];
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;

    size_t base = strlen(ready_json);
    assert(base < SUITE_TR_PROTOCOL_JSON_MAX);
    memcpy(body, ready_json, base);
    memset(body + base, ' ', SUITE_TR_PROTOCOL_JSON_MAX - base);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %u\r\n",
             (unsigned)SUITE_TR_PROTOCOL_JSON_MAX);
    init_http_deferred();
    set_response_parts("HTTP/1.1 200 OK\r\n", headers, (uint8_t *)body,
                       SUITE_TR_PROTOCOL_JSON_MAX);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(ready.ready);
    assert(info.response_body_bytes == SUITE_TR_PROTOCOL_JSON_MAX);
    assert_deferred_storage_cycle(SUITE_TR_PROTOCOL_JSON_MAX);

    memcpy(body, ready_json, base);
    memset(body + base, ' ', SUITE_TR_HTTP_JSON_CAP - base);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %u\r\n",
             (unsigned)SUITE_TR_HTTP_JSON_CAP);
    init_http_deferred();
    set_response_parts("HTTP/1.1 200 OK\r\n", headers, (uint8_t *)body,
                       SUITE_TR_HTTP_JSON_CAP);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_OVERSIZE);
    assert(ready.ready == false);
    assert_deferred_storage_rejected_before_alloc();

    /* The same boundary applies to a caller-owned 8,193-byte buffer: an
       8,192-byte body fits, while 8,193 is rejected before any read. */
    memcpy(body, ready_json, base);
    memset(body + base, ' ', SUITE_TR_PROTOCOL_JSON_MAX - base);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %u\r\n",
             (unsigned)SUITE_TR_PROTOCOL_JSON_MAX);
    init_http();
    set_response_parts("HTTP/1.1 200 OK\r\n", headers, (uint8_t *)body,
                       SUITE_TR_PROTOCOL_JSON_MAX);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_OK);
    assert(ready.ready);
    assert(s_heap_malloc_calls == 0 && s_heap_malloc_free_calls == 0);
    for (size_t i = 0; i < sizeof(s_json); i++) assert(s_json[i] == 0);

    memcpy(body, ready_json, base);
    memset(body + base, ' ', SUITE_TR_HTTP_JSON_CAP - base);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %u\r\n",
             (unsigned)SUITE_TR_HTTP_JSON_CAP);
    init_http();
    set_response_parts("HTTP/1.1 200 OK\r\n", headers, (uint8_t *)body,
                       SUITE_TR_HTTP_JSON_CAP);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_OVERSIZE);
    assert(s_heap_malloc_calls == 0 && s_heap_malloc_free_calls == 0);
    for (size_t i = 0; i < sizeof(s_json); i++) assert(s_json[i] == 0);
    free(body);
}

static void test_deferred_storage_no_body_skips_allocation(void)
{
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;

    /* A redirect is never body-read, so no response storage is obtained. */
    init_http_deferred();
    set_response_parts("HTTP/1.1 302 Found\r\n",
                       "Content-Type: application/json\r\n"
                       "Content-Length: 5\r\n",
                       (const uint8_t *)"hello", 5);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_HTTP_REDIRECT);
    assert(s_heap_malloc_calls == 0 && s_heap_malloc_free_calls == 0);
    assert(s_heap_malloc_live == 0);
    assert(s_request_length > 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);

    /* A non-JSON error body is not captured, so it is not allocated either. */
    init_http_deferred();
    set_response_parts("HTTP/1.1 503 Service Unavailable\r\n",
                       "Content-Type: text/plain\r\n"
                       "Content-Length: 5\r\n",
                       (const uint8_t *)"hello", 5);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_NOT_READY);
    assert(s_heap_malloc_calls == 0 && s_heap_malloc_free_calls == 0);
    assert(s_heap_malloc_live == 0);
    assert(s_request_length > 0);
}

static void test_deferred_storage_largest_block_limit(void)
{
    static const char ready_json[] =
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}";
    char *body = malloc(SUITE_TR_PROTOCOL_JSON_MAX);
    assert(body);
    char headers[128];
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;
    suite_translation_protocol_view_t view;
    suite_translation_protocol_identity_t identity = make_identity();

    /* Simulate a fragmented internal heap whose largest free block is 1 KiB:
       the old unconditional 8,193-byte reservation fails, while a small
       exact-size response allocation succeeds. */
    size_t base = strlen(ready_json);
    memcpy(body, ready_json, base);
    memset(body + base, ' ', SUITE_TR_PROTOCOL_JSON_MAX - base);
    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nContent-Length: %u\r\n",
             (unsigned)SUITE_TR_PROTOCOL_JSON_MAX);
    init_http_deferred();
    s_heap_largest_limit = 1024;
    set_response_parts("HTTP/1.1 200 OK\r\n", headers, (uint8_t *)body,
                       SUITE_TR_PROTOCOL_JSON_MAX);
    call = make_call(5000);
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_BUFFER);
    assert(info.ambiguous);
    assert(s_heap_malloc_calls == 1 && s_heap_malloc_last == NULL);
    assert(s_heap_malloc_free_calls == 0 && s_heap_malloc_live == 0);
    assert(s_heap_largest_rejects == 1);
    assert(s_write_calls > 0);
    assert(s_net_takes == 1 && s_net_gives == 1);
    assert(s_tls_init_calls == 1 && s_tls_destroy_calls == 1);
    free(body);

    /* Repeated status polls keep allocating only Content-Length + 1. */
    init_http_deferred();
    s_heap_largest_limit = 1024;
    size_t done_length = strlen(done_json);
    assert(done_length + 1U < 1024U);
    for (unsigned poll = 0; poll < 20U; poll++) {
        set_json_response("HTTP/1.1 200 OK\r\n", done_json);
        call = make_call(5000);
        view = make_view();
        assert(suite_translation_http_status(&s_http, &call, &identity, &view,
                                            &info) == SUITE_TR_HTTP_OK);
        assert(view.state == SUITE_TR_PROTOCOL_STATE_DONE);
        assert(s_heap_malloc_calls == (int)poll + 1);
        assert(s_heap_malloc_last_size == done_length + 1U);
        assert(s_heap_malloc_free_calls == (int)poll + 1);
        assert(s_heap_malloc_live == 0);
    }
    assert(s_heap_largest_rejects == 0);
    assert(s_net_takes == 20 && s_net_gives == 20);
    assert(s_tls_init_calls == 20 && s_tls_destroy_calls == 20);
}

static void test_deferred_storage_final_acceptance_after_free(void)
{
    static const char ready_json[] =
        "{\"schema\":1,\"ready\":true,\"provider\":\"mimo\"}";
    suite_translation_http_call_t call;
    suite_translation_http_info_t info;
    suite_translation_protocol_ready_t ready;
    suite_translation_protocol_view_t view;
    suite_translation_protocol_identity_t identity = make_identity();

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", ready_json);
    call = make_call(5000);
    s_cancel_on_free = true;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_CANCELLED);
    assert(!ready.ready);
    assert(!info.ambiguous);
    assert_deferred_storage_cycle(strlen(ready_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 200 OK\r\n", ready_json);
    call = make_call(5000);
    s_expire_on_free = true;
    s_heap_advance_on_free_us = 6000000;
    assert(suite_translation_http_ready(&s_http, &call, &ready, &info) ==
           SUITE_TR_HTTP_E_DEADLINE);
    assert(!ready.ready);
    assert(!info.ambiguous);
    assert_deferred_storage_cycle(strlen(ready_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    s_cancel_on_free = true;
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_CANCELLED);
    assert(info.ambiguous);
    assert(!view.job_id[0] && !view.has_source && !view.has_target);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    s_expire_on_free = true;
    s_heap_advance_on_free_us = 6000000;
    assert(suite_translation_http_admit(
               &s_http, &call, OP, SUITE_TR_PROTOCOL_DIRECTION_EN_ZH, SHA,
               AUDIO, &view, &info) == SUITE_TR_HTTP_E_DEADLINE);
    assert(info.ambiguous);
    assert(!view.job_id[0] && !view.has_source && !view.has_target);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    upload_source_t source = {.length = AUDIO};
    s_cancel_on_free = true;
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source, &view,
               &info) == SUITE_TR_HTTP_E_CANCELLED);
    assert(info.ambiguous);
    assert(source.next == AUDIO);
    assert(!view.job_id[0] && !view.has_source && !view.has_target);
    assert_deferred_storage_cycle(strlen(admitted_json));

    init_http_deferred();
    set_json_response("HTTP/1.1 202 Accepted\r\n", admitted_json);
    call = make_call(5000);
    view = make_view();
    upload_source_t source2 = {.length = AUDIO};
    s_expire_on_free = true;
    s_heap_advance_on_free_us = 6000000;
    assert(suite_translation_http_upload(
               &s_http, &call, &identity, upload_read, &source2, &view,
               &info) == SUITE_TR_HTTP_E_DEADLINE);
    assert(info.ambiguous);
    assert(source2.next == AUDIO);
    assert(!view.job_id[0] && !view.has_source && !view.has_target);
    assert_deferred_storage_cycle(strlen(admitted_json));
}

int main(void)
{
    test_ready_transport();
    test_admission_transport();
    test_upload_transport();
    test_status_transport();
    test_cancel_transport();
    test_deadlines_and_dns();
    test_tcp_and_dns_workarounds();
    test_tls_diagnostic_snapshot();
    test_protocol_errors();
    test_deferred_storage_all_operations();
    test_caller_buffer_not_freed();
    test_deferred_storage_early_failures();
    test_deferred_storage_allocation_failures();
    test_deferred_storage_terminal_release();
    test_deferred_storage_no_body_skips_allocation();
    test_deferred_storage_largest_block_limit();
    test_deferred_storage_final_acceptance_after_free();
    test_deferred_storage_capacity_boundary();
    puts("translation HTTP transport tests: PASS");
    return 0;
}
