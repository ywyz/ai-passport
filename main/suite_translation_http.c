#include "suite_translation_http.h"

#include <suite_clock.h>
#include <suite_connection.h>
#include <suite_net.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"

#include <sys/socket.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TR_HTTP_PORT 443
#define TR_HTTP_PATH "/suite/v1/translations"
#define TR_HTTP_READY_PATH TR_HTTP_PATH "/ready"
#define TR_HTTP_MAX_ORIGIN 192U
#define TR_HTTP_MAX_HOST 193U
#define TR_HTTP_MAX_TOKEN 128U
#define TR_HTTP_MAX_PATH 96U
#define TR_HTTP_MAX_HEADER 640U
#define TR_HTTP_READ_BUFFER 512U
#define TR_HTTP_LINE_MAX 512U
#define TR_HTTP_TCP_POLL_MS 50

typedef struct {
    bool content_length_seen;
    uint32_t content_length;
    bool content_type_seen;
    bool content_type_json;
    bool content_encoding_seen;
    bool content_encoding_identity;
    bool transfer_encoding;
} response_headers_t;

typedef struct {
    esp_tls_t *tls;
    const suite_translation_http_call_t *call;
    uint8_t buffer[TR_HTTP_READ_BUFFER];
    size_t position;
    size_t length;
    bool eof;
} response_reader_t;

typedef struct {
    uint8_t *json;
    size_t cap;
    bool owned;
} response_storage_t;

typedef struct {
    ip_addr_t address;
    char host[TR_HTTP_MAX_HOST];
    atomic_bool finished;
    atomic_bool success;
    atomic_uint generation;
    atomic_uint dns_generation;
    bool active;
} dns_slot_t;

/*
 * Two static slots bound callback state for one worker. The worker never calls
 * lwIP DNS directly: tcpip_try_callback dispatches the resolver call into the
 * TCPIP thread. A slot owns its hostname and result until every callback has
 * finished; a timed-out worker leaves the slot active and the later callback
 * publishes completion, which the next call reaps. lwIP invokes its resolver
 * callback on success, failure and resolver timeout, so slots recover even
 * after a caller deadline. Enqueue failure releases the slot immediately.
 * Generation counters reject a stale callback if a slot is ever reused.
 */
static dns_slot_t s_dns[SUITE_TR_HTTP_DNS_SLOTS];

static void secure_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = data;
    while (length--) *bytes++ = 0;
}

static void release_response(response_storage_t *storage)
{
    if (!storage || !storage->json) return;
    secure_zero(storage->json, storage->cap);
    if (storage->owned) heap_caps_free(storage->json);
    storage->json = NULL;
    storage->cap = 0;
    storage->owned = false;
}

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static bool origin_valid(const char *origin)
{
    if (!origin || strncmp(origin, "https://", 8) != 0)
        return false;
    size_t length = strlen(origin);
    if (length <= 8 || length > TR_HTTP_MAX_ORIGIN || origin[length - 1] == '/')
        return false;
    size_t label = 8;
    for (size_t i = 8; i <= length; i++) {
        char c = origin[i];
        if (c == '.' || c == 0) {
            if (i == label || i - label > 63 || origin[label] == '-' ||
                origin[i - 1] == '-')
                return false;
            label = i + 1;
            if (c == 0) break;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static bool token_valid(const char *token)
{
    if (!token) return false;
    size_t length = strlen(token);
    if (length < 32 || length > TR_HTTP_MAX_TOKEN) return false;
    for (size_t i = 0; i < length; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool http_valid(const suite_translation_http_t *http)
{
    return http && origin_valid(http->origin) && token_valid(http->token) &&
        http->json_cap >= SUITE_TR_HTTP_JSON_CAP;
}

static bool cancelled(const suite_translation_http_call_t *call)
{
    return call->cancel && call->cancel(call->cancel_ctx);
}

static bool expired(const suite_translation_http_call_t *call)
{
    return esp_timer_get_time() >= call->deadline_us;
}

/* Cancel has priority over the deadline. Every successful I/O boundary and
 * every complete-response acceptance uses this check. */
static suite_translation_http_result_t io_complete(
    const suite_translation_http_call_t *call)
{
    if (cancelled(call)) return SUITE_TR_HTTP_E_CANCELLED;
    if (expired(call)) return SUITE_TR_HTTP_E_DEADLINE;
    return SUITE_TR_HTTP_OK;
}

static suite_translation_http_result_t connection_gate(void)
{
    if (suite_connection_state() != SUITE_CONN_ONLINE)
        return SUITE_TR_HTTP_E_OFFLINE;
    if (!suite_time_is_trusted(suite_connection_time(),
                               suite_connection_now()))
        return SUITE_TR_HTTP_E_TIME;
    return SUITE_TR_HTTP_OK;
}

static void dns_done(const char *name, const ip_addr_t *address, void *arg)
{
    (void)name;
    dns_slot_t *slot = arg;
    unsigned generation =
        atomic_load_explicit(&slot->generation, memory_order_acquire);
    unsigned dispatch_generation =
        atomic_load_explicit(&slot->dns_generation, memory_order_acquire);
    if (generation != dispatch_generation) return;
    bool success = address != NULL;
    if (success) slot->address = *address;
    atomic_store_explicit(&slot->success, success, memory_order_relaxed);
    atomic_store_explicit(&slot->finished, true, memory_order_release);
}

/* Runs in the lwIP TCPIP thread, never in the worker. */
static void dns_dispatch(void *arg)
{
    dns_slot_t *slot = arg;
    unsigned generation =
        atomic_load_explicit(&slot->generation, memory_order_acquire);
    atomic_store_explicit(&slot->dns_generation, generation,
                          memory_order_release);
    err_t error = dns_gethostbyname(slot->host, &slot->address, dns_done,
                                    slot);
    if (error == ERR_OK) {
        atomic_store_explicit(&slot->success, true, memory_order_relaxed);
        atomic_store_explicit(&slot->finished, true,
                              memory_order_release);
    } else if (error != ERR_INPROGRESS) {
        atomic_store_explicit(&slot->success, false, memory_order_relaxed);
        atomic_store_explicit(&slot->finished, true,
                              memory_order_release);
    }
}

static void dns_reap(void)
{
    for (unsigned i = 0; i < SUITE_TR_HTTP_DNS_SLOTS; i++) {
        if (s_dns[i].active &&
            atomic_load_explicit(&s_dns[i].finished, memory_order_acquire))
            s_dns[i].active = false;
    }
}

static suite_translation_http_result_t dns_resolve(
    const char *host, const suite_translation_http_call_t *call,
    ip_addr_t *address)
{
    dns_reap();
    dns_slot_t *slot = NULL;
    for (unsigned i = 0; i < SUITE_TR_HTTP_DNS_SLOTS; i++) {
        if (!s_dns[i].active) {
            slot = &s_dns[i];
            break;
        }
    }
    if (!slot) return SUITE_TR_HTTP_E_DNS;
    size_t host_length = strlen(host);
    if (host_length == 0 || host_length >= sizeof(slot->host))
        return SUITE_TR_HTTP_E_DNS;
    memcpy(slot->host, host, host_length + 1U);
    slot->active = true;
    atomic_fetch_add_explicit(&slot->generation, 1U,
                              memory_order_relaxed);
    atomic_store_explicit(&slot->dns_generation, 0U, memory_order_relaxed);
    atomic_store_explicit(&slot->finished, false, memory_order_relaxed);
    atomic_store_explicit(&slot->success, false, memory_order_relaxed);

    err_t queued = tcpip_try_callback(dns_dispatch, slot);
    if (queued != ERR_OK) {
        slot->active = false;
        return SUITE_TR_HTTP_E_DNS;
    }
    for (;;) {
        if (cancelled(call)) return SUITE_TR_HTTP_E_CANCELLED;
        if (expired(call)) return SUITE_TR_HTTP_E_DEADLINE;
        if (atomic_load_explicit(&slot->finished, memory_order_acquire)) {
            bool success =
                atomic_load_explicit(&slot->success, memory_order_relaxed);
            if (!success) {
                slot->active = false;
                return SUITE_TR_HTTP_E_DNS;
            }
            *address = slot->address;
            slot->active = false;
            return SUITE_TR_HTTP_OK;
        }
        vTaskDelay(SUITE_TR_HTTP_POLL_TICKS);
    }
}

static int remaining_timeout_ms(const suite_translation_http_call_t *call,
                                int maximum)
{
    int64_t remaining = (call->deadline_us - esp_timer_get_time()) / 1000;
    if (remaining <= 0) return 0;
    if (remaining > maximum) remaining = maximum;
    if (remaining < 1) remaining = 1;
    return (int)remaining;
}

/*
 * The pinned SDK sets its stored fd_sets once in INIT and lwIP clears them on
 * a select timeout. Re-entering CONNECTING after that can keep timing out on
 * empty sets even when TCP is ready. While the worker owns the TLS handle it
 * polls fresh sets and SO_ERROR itself. Only after TCP is confirmed does it
 * make exactly one esp_tls call with non_block=false, which skips the SDK's
 * obsolete CONNECTING select and proceeds to certificate/handshake setup.
 */

/*
 * Copy the first transport-level TLS failure into a fixed-size numeric
 * snapshot before esp_tls_conn_destroy() frees the SDK error storage. The SDK
 * getters only read/clear fields and allocate nothing. local_errno is saved at
 * the failure site; explicit SO_ERROR takes priority for the displayed system
 * value because errno can be zero or stale. No library code is invented for an
 * init-null failure (there is no handle).
 */
static void tls_capture(esp_tls_t *tls, suite_translation_tls_diag_t *diag,
                        unsigned stage, int local_errno, int so_error)
{
    if (!diag || diag->valid) return;
    memset(diag, 0, sizeof(*diag));
    diag->valid = true;
    diag->stage = (uint8_t)stage;
    diag->local_errno = local_errno;
    diag->so_error = so_error;

    if (tls) {
        esp_tls_error_handle_t handle = NULL;
        if (esp_tls_get_error_handle(tls, &handle) == ESP_OK && handle) {
            int tls_code = 0;
            int flags = 0;
            int sys = 0;
            diag->esp_err_code = esp_tls_get_and_clear_last_error(
                handle, &tls_code, &flags);
            diag->tls_code = tls_code;
            diag->verify_flags = (uint32_t)flags;
            if (esp_tls_get_and_clear_error_type(
                    handle, ESP_TLS_ERR_TYPE_SYSTEM, &sys) == ESP_OK)
                diag->sdk_sys_errno = sys;
        }
    }

    if (so_error != 0) {
        diag->sys_error = so_error;
        diag->sys_source = SUITE_TR_TLS_SYS_SO_ERROR;
    } else if (diag->sdk_sys_errno != 0) {
        diag->sys_error = diag->sdk_sys_errno;
        diag->sys_source = SUITE_TR_TLS_SYS_SDK;
    } else if (local_errno != 0) {
        diag->sys_error = local_errno;
        diag->sys_source = SUITE_TR_TLS_SYS_LOCAL_ERRNO;
    }

    diag->free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT);
    diag->largest_bytes = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static suite_translation_http_result_t tcp_wait_connected(
    esp_tls_t *tls, const suite_translation_http_call_t *call,
    suite_translation_tls_diag_t *diag)
{
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        int local_errno = errno;
        tls_capture(tls, diag, SUITE_TR_TLS_STAGE_TCP_SOCKFD, local_errno, 0);
        return SUITE_TR_HTTP_E_TLS;
    }
    for (;;) {
        if (cancelled(call)) return SUITE_TR_HTTP_E_CANCELLED;
        if (expired(call)) return SUITE_TR_HTTP_E_DEADLINE;
        int64_t remaining_us = call->deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) return SUITE_TR_HTTP_E_DEADLINE;
        int64_t wait_ms = remaining_us / 1000;
        if (wait_ms > TR_HTTP_TCP_POLL_MS) wait_ms = TR_HTTP_TCP_POLL_MS;
        if (wait_ms < 1) wait_ms = 1;
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval timeout = {
            .tv_sec = (long)(wait_ms / 1000),
            .tv_usec = (long)((wait_ms % 1000) * 1000),
        };
        int ready = select(fd + 1, NULL, &write_set, NULL, &timeout);
        if (ready < 0) {
            int local_errno = errno;
            tls_capture(tls, diag, SUITE_TR_TLS_STAGE_TCP_SELECT,
                        local_errno, 0);
            return SUITE_TR_HTTP_E_TLS;
        }
        if (ready == 0) continue;
        if (!FD_ISSET(fd, &write_set)) continue;
        if (cancelled(call)) return SUITE_TR_HTTP_E_CANCELLED;
        if (expired(call)) return SUITE_TR_HTTP_E_DEADLINE;
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
            int local_errno = errno;
            tls_capture(tls, diag, SUITE_TR_TLS_STAGE_TCP_GETSOCKOPT,
                        local_errno, 0);
            return SUITE_TR_HTTP_E_TLS;
        }
        if (socket_error != 0) {
            int local_errno = errno;
            tls_capture(tls, diag, SUITE_TR_TLS_STAGE_TCP_SO_ERROR,
                        local_errno, socket_error);
            return SUITE_TR_HTTP_E_TLS;
        }
        return SUITE_TR_HTTP_OK;
    }
}

static suite_translation_http_result_t tls_connect(
    const char *host, const ip_addr_t *address,
    const suite_translation_http_call_t *call, esp_tls_t **out,
    suite_translation_tls_diag_t *diag)
{
    char ip[64];
    if (!ipaddr_ntoa_r(address, ip, sizeof(ip)))
        return SUITE_TR_HTTP_E_DNS;
    /*
     * lwIP resolves the hostname asynchronously. esp_tls is then pointed at
     * the numeric address while cfg.common_name keeps the original hostname
     * for SNI and certificate verification; the resolved IP alone never
     * replaces TLS identity. esp_tls still performs a local numeric
     * getaddrinfo internally, which is not network DNS but has no caller
     * deadline. The first async call's TCP select is capped at 100 ms and the
     * caller checks cancellation/deadline immediately after it returns.
     * mbedTLS crypto and worker-owned cleanup are not preemptible.
     */
    esp_tls_cfg_t config = {0};
    config.non_block = true;
    config.timeout_ms = SUITE_TR_HTTP_CONNECT_TIMEOUT_MS;
    config.common_name = host;
    config.skip_common_name = false;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.addr_family =
        IP_IS_V6(address) ? ESP_TLS_AF_INET6 : ESP_TLS_AF_INET;

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        int local_errno = errno;
        tls_capture(NULL, diag, SUITE_TR_TLS_STAGE_INIT_NULL, local_errno, 0);
        return SUITE_TR_HTTP_E_TLS;
    }
    bool tcp_confirmed = false;
    unsigned async_calls = 0;
    for (;;) {
        if (cancelled(call)) {
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_CANCELLED;
        }
        if (expired(call)) {
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_DEADLINE;
        }
        int timeout = remaining_timeout_ms(
            call, SUITE_TR_HTTP_CONNECT_TIMEOUT_MS);
        if (!timeout) {
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_DEADLINE;
        }
        config.non_block = true;
        config.timeout_ms = timeout;
        int result = esp_tls_conn_new_async(ip, (int)strlen(ip), TR_HTTP_PORT,
                                            &config, tls);
        async_calls++;
        int async_errno = errno;
        if (cancelled(call)) {
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_CANCELLED;
        }
        if (expired(call)) {
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_DEADLINE;
        }
        if (result == 1) {
            *out = tls;
            return SUITE_TR_HTTP_OK;
        }
        if (result < 0) {
            tls_capture(tls, diag,
                        async_calls == 1 ? SUITE_TR_TLS_STAGE_ASYNC1
                                         : SUITE_TR_TLS_STAGE_HANDSHAKE_LOOP,
                        async_errno, 0);
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_TLS;
        }

        esp_tls_conn_state_t state;
        if (esp_tls_get_conn_state(tls, &state) != ESP_OK) {
            tls_capture(tls, diag,
                        async_calls == 1 ? SUITE_TR_TLS_STAGE_STATE1
                                         : SUITE_TR_TLS_STAGE_HANDSHAKE_LOOP,
                        async_errno, 0);
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_TLS;
        }
        if (state == ESP_TLS_DONE) {
            *out = tls;
            return SUITE_TR_HTTP_OK;
        }
        if (state == ESP_TLS_FAIL) {
            tls_capture(tls, diag,
                        async_calls == 1 ? SUITE_TR_TLS_STAGE_STATE_FAIL
                                         : SUITE_TR_TLS_STAGE_HANDSHAKE_LOOP,
                        async_errno, 0);
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_TLS;
        }
        if (state == ESP_TLS_CONNECTING && !tcp_confirmed) {
            suite_translation_http_result_t wait =
                tcp_wait_connected(tls, call, diag);
            if (wait != SUITE_TR_HTTP_OK) {
                esp_tls_conn_destroy(tls);
                return wait;
            }
            tcp_confirmed = true;
            if (cancelled(call)) {
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_CANCELLED;
            }
            if (expired(call)) {
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_DEADLINE;
            }
            timeout = remaining_timeout_ms(
                call, SUITE_TR_HTTP_CONNECT_TIMEOUT_MS);
            if (!timeout) {
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_DEADLINE;
            }
            /*
             * Exactly one non-blocking=false call while already CONNECTING.
             * tcp_connect is not re-entered, so the established nonblocking
             * socket is not switched back to blocking; this call only skips
             * the SDK's stale CONNECTING select, then creates TLS state and
             * performs a nonblocking handshake.
             */
            config.non_block = false;
            config.timeout_ms = timeout;
            result = esp_tls_conn_new_async(ip, (int)strlen(ip),
                                            TR_HTTP_PORT, &config, tls);
            config.non_block = true;
            async_calls++;
            int async2_errno = errno;
            if (cancelled(call)) {
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_CANCELLED;
            }
            if (expired(call)) {
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_DEADLINE;
            }
            if (result == 1) {
                *out = tls;
                return SUITE_TR_HTTP_OK;
            }
            if (result < 0) {
                tls_capture(tls, diag, SUITE_TR_TLS_STAGE_ASYNC2,
                            async2_errno, 0);
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_TLS;
            }
            bool state_ok = esp_tls_get_conn_state(tls, &state) == ESP_OK;
            if (!state_ok ||
                (state != ESP_TLS_HANDSHAKE && state != ESP_TLS_DONE)) {
                unsigned stage = state_ok && state == ESP_TLS_CONNECTING
                                     ? SUITE_TR_TLS_STAGE_TCP_STUCK
                                     : SUITE_TR_TLS_STAGE_POST_STATE;
                tls_capture(tls, diag, stage, async2_errno, 0);
                esp_tls_conn_destroy(tls);
                return SUITE_TR_HTTP_E_TLS;
            }
            if (state == ESP_TLS_DONE) {
                *out = tls;
                return SUITE_TR_HTTP_OK;
            }
        } else if (state == ESP_TLS_CONNECTING) {
            /* tcp_confirmed but the SDK did not advance: fail closed. */
            int local_errno = errno;
            tls_capture(tls, diag, SUITE_TR_TLS_STAGE_TCP_STUCK,
                        local_errno, 0);
            esp_tls_conn_destroy(tls);
            return SUITE_TR_HTTP_E_TLS;
        }
        vTaskDelay(SUITE_TR_HTTP_POLL_TICKS);
    }
}

static suite_translation_http_result_t tls_write_all(
    esp_tls_t *tls, const uint8_t *data, size_t length,
    const suite_translation_http_call_t *call)
{
    size_t offset = 0;
    while (offset < length) {
        if (cancelled(call)) return SUITE_TR_HTTP_E_CANCELLED;
        if (expired(call)) return SUITE_TR_HTTP_E_DEADLINE;
        ssize_t written = esp_tls_conn_write(tls, data + offset,
                                             length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written == ESP_TLS_ERR_SSL_WANT_READ ||
            written == ESP_TLS_ERR_SSL_WANT_WRITE || written == 0) {
            vTaskDelay(SUITE_TR_HTTP_POLL_TICKS);
            continue;
        }
        return SUITE_TR_HTTP_E_IO;
    }
    return io_complete(call);
}

static suite_translation_http_result_t reader_fill(response_reader_t *reader)
{
    if (reader->position < reader->length) return SUITE_TR_HTTP_OK;
    reader->position = reader->length = 0;
    if (reader->eof) return SUITE_TR_HTTP_E_TRUNCATED;
    for (;;) {
        if (cancelled(reader->call)) return SUITE_TR_HTTP_E_CANCELLED;
        if (expired(reader->call)) return SUITE_TR_HTTP_E_DEADLINE;
        ssize_t read = esp_tls_conn_read(reader->tls, reader->buffer,
                                         sizeof(reader->buffer));
        if (read > 0) {
            suite_translation_http_result_t complete = io_complete(reader->call);
            if (complete != SUITE_TR_HTTP_OK) return complete;
            reader->length = (size_t)read;
            return SUITE_TR_HTTP_OK;
        }
        if (read == 0) {
            reader->eof = true;
            return SUITE_TR_HTTP_E_TRUNCATED;
        }
        if (read == ESP_TLS_ERR_SSL_WANT_READ ||
            read == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(SUITE_TR_HTTP_POLL_TICKS);
            continue;
        }
        return SUITE_TR_HTTP_E_IO;
    }
}

static suite_translation_http_result_t reader_read(response_reader_t *reader,
                                                   uint8_t *data,
                                                   size_t length)
{
    while (length) {
        if (reader->position == reader->length) {
            suite_translation_http_result_t result = reader_fill(reader);
            if (result != SUITE_TR_HTTP_OK) return result;
        }
        size_t available = reader->length - reader->position;
        size_t take = available < length ? available : length;
        memcpy(data, reader->buffer + reader->position, take);
        reader->position += take;
        data += take;
        length -= take;
    }
    return io_complete(reader->call);
}

static suite_translation_http_result_t reader_line(response_reader_t *reader,
                                                   char *line, size_t capacity,
                                                   size_t *line_length)
{
    size_t length = 0;
    bool carriage_return = false;
    for (;;) {
        if (reader->position == reader->length) {
            suite_translation_http_result_t result = reader_fill(reader);
            if (result != SUITE_TR_HTTP_OK) return result;
        }
        uint8_t byte = reader->buffer[reader->position++];
        if (byte == '\n') {
            if (!carriage_return) return SUITE_TR_HTTP_E_IO;
            line[length] = 0;
            *line_length = length;
            return SUITE_TR_HTTP_OK;
        }
        if (byte == '\r') {
            if (carriage_return) return SUITE_TR_HTTP_E_IO;
            carriage_return = true;
            continue;
        }
        if (carriage_return) return SUITE_TR_HTTP_E_IO;
        if (byte == 0 || (byte < 0x20 && byte != '\t'))
            return SUITE_TR_HTTP_E_IO;
        if (length + 1U >= capacity) return SUITE_TR_HTTP_E_IO;
        line[length++] = (char)byte;
    }
}

static bool ascii_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

static char *trim_ows(char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    size_t length = strlen(value);
    while (length && (value[length - 1] == ' ' ||
                      value[length - 1] == '\t'))
        value[--length] = 0;
    return value;
}

static bool header_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' ||
        c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
        c == '-' || c == '.' || c == '^' || c == '_' || c == '`' ||
        c == '|' || c == '~';
}

static bool parse_decimal(const char *value, uint32_t *out)
{
    if (!*value) return false;
    uint64_t result = 0;
    for (const char *p = value; *p; p++) {
        if (!is_digit_char(*p)) return false;
        uint8_t digit = (uint8_t)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    if (result > UINT32_MAX) return false;
    *out = (uint32_t)result;
    return true;
}

static bool parse_status_line(const char *line, int *status)
{
    size_t length = strlen(line);
    if (length < 12) return false;
    if (memcmp(line, "HTTP/1.", 7) != 0) return false;
    if (line[7] != '0' && line[7] != '1') return false;
    if (line[8] != ' ') return false;
    if (!is_digit_char(line[9]) || !is_digit_char(line[10]) ||
        !is_digit_char(line[11]))
        return false;
    int code = (line[9] - '0') * 100 + (line[10] - '0') * 10 +
        (line[11] - '0');
    if (code < 100 || code > 599) return false;
    if (length != 12 && line[12] != ' ') return false;
    *status = code;
    return true;
}

static bool parse_header_line(char *line, response_headers_t *headers)
{
    char *colon = strchr(line, ':');
    if (!colon || colon == line) return false;
    for (char *p = line; p < colon; p++) {
        if (!header_name_char(*p)) return false;
    }
    *colon = 0;
    char *value = trim_ows(colon + 1);
    if (ascii_equal(line, "Content-Length")) {
        if (headers->content_length_seen) return false;
        if (!parse_decimal(value, &headers->content_length)) return false;
        headers->content_length_seen = true;
    } else if (ascii_equal(line, "Content-Type")) {
        if (headers->content_type_seen) return false;
        headers->content_type_json = ascii_equal(value, "application/json");
        headers->content_type_seen = true;
    } else if (ascii_equal(line, "Content-Encoding")) {
        if (headers->content_encoding_seen) return false;
        headers->content_encoding_identity = ascii_equal(value, "identity");
        headers->content_encoding_seen = true;
    } else if (ascii_equal(line, "Transfer-Encoding")) {
        headers->transfer_encoding = true;
    }
    return true;
}

static suite_translation_http_result_t read_headers(
    response_reader_t *reader, int *status, response_headers_t *headers)
{
    char line[TR_HTTP_LINE_MAX] = {0};
    size_t line_length;
    suite_translation_http_result_t result =
        reader_line(reader, line, sizeof(line), &line_length);
    if (result != SUITE_TR_HTTP_OK) goto done;
    if (!parse_status_line(line, status)) {
        result = SUITE_TR_HTTP_E_IO;
        goto done;
    }
    size_t total = line_length + 2U;
    unsigned count = 0;
    for (;;) {
        result = reader_line(reader, line, sizeof(line), &line_length);
        if (result != SUITE_TR_HTTP_OK) goto done;
        total += line_length + 2U;
        if (total > SUITE_TR_HTTP_HEADER_BYTES_MAX) {
            result = SUITE_TR_HTTP_E_LIMIT;
            goto done;
        }
        if (line_length == 0) break;
        if (line[0] == ' ' || line[0] == '\t') {
            result = SUITE_TR_HTTP_E_IO;
            goto done;
        }
        if (++count > SUITE_TR_HTTP_HEADERS_MAX) {
            result = SUITE_TR_HTTP_E_LIMIT;
            goto done;
        }
        if (!parse_header_line(line, headers)) {
            result = SUITE_TR_HTTP_E_IO;
            goto done;
        }
    }
    if (headers->transfer_encoding) {
        result = SUITE_TR_HTTP_E_TRANSFER_ENCODING;
        goto done;
    }
    if (headers->content_encoding_seen &&
        !headers->content_encoding_identity) {
        result = SUITE_TR_HTTP_E_CONTENT_ENCODING;
        goto done;
    }
    if (!headers->content_length_seen) {
        result = SUITE_TR_HTTP_E_LENGTH;
        goto done;
    }
    result = SUITE_TR_HTTP_OK;

done:
    secure_zero(line, sizeof(line));
    return result;
}

static bool make_job_path(char *path, size_t capacity, const char *job_id,
                          const char *suffix)
{
    if (!suite_translation_protocol_id_valid(job_id)) return false;
    int written = snprintf(path, capacity, "%s/%s%s", TR_HTTP_PATH, job_id,
                           suffix);
    return written > 0 && (size_t)written < capacity;
}

static suite_translation_http_result_t send_request(
    esp_tls_t *tls, const suite_translation_http_t *http,
    const suite_translation_http_call_t *call, const char *method,
    const char *path, const char *host, const char *content_type,
    const uint8_t *body, uint32_t body_length,
    suite_translation_http_audio_read_fn read, void *read_ctx,
    bool *request_started, uint32_t *sent)
{
    char header[TR_HTTP_MAX_HEADER] = {0};
    uint8_t chunk[SUITE_TR_HTTP_AUDIO_CHUNK] = {0};
    suite_translation_http_result_t result = SUITE_TR_HTTP_E_ARGUMENT;

    int written = snprintf(header, sizeof(header),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n",
        method, path, host, http->token);
    if (written < 0 || (size_t)written >= sizeof(header)) goto done;
    size_t used = (size_t)written;
    if (content_type) {
        written = snprintf(header + used, sizeof(header) - used,
                           "Content-Type: %s\r\nContent-Length: %lu\r\n",
                           content_type, (unsigned long)body_length);
        if (written < 0 || (size_t)written >= sizeof(header) - used)
            goto done;
        used += (size_t)written;
    }
    written = snprintf(header + used, sizeof(header) - used, "\r\n");
    if (written < 0 || (size_t)written >= sizeof(header) - used)
        goto done;
    used += (size_t)written;

    *request_started = true;
    result = tls_write_all(tls, (const uint8_t *)header, used, call);
    if (result != SUITE_TR_HTTP_OK) goto done;

    if (read) {
        if (!content_type || body_length == 0) {
            result = SUITE_TR_HTTP_E_ARGUMENT;
            goto done;
        }
        uint32_t offset = 0;
        while (offset < body_length) {
            if (cancelled(call)) {
                result = SUITE_TR_HTTP_E_CANCELLED;
                goto done;
            }
            if (expired(call)) {
                result = SUITE_TR_HTTP_E_DEADLINE;
                goto done;
            }
            size_t wanted = body_length - offset;
            if (wanted > sizeof(chunk)) wanted = sizeof(chunk);
            size_t got = read(read_ctx, offset, chunk, wanted);
            if (got == 0 || got > wanted) {
                result = SUITE_TR_HTTP_E_IO;
                goto done;
            }
            result = tls_write_all(tls, chunk, got, call);
            if (result != SUITE_TR_HTTP_OK) goto done;
            offset += (uint32_t)got;
            *sent = offset;
        }
    } else if (body_length) {
        if (!body) {
            result = SUITE_TR_HTTP_E_ARGUMENT;
            goto done;
        }
        result = tls_write_all(tls, body, body_length, call);
        if (result != SUITE_TR_HTTP_OK) goto done;
        *sent = body_length;
    }
    result = io_complete(call);

done:
    secure_zero(chunk, sizeof(chunk));
    secure_zero(header, sizeof(header));
    return result;
}

/*
 * Parse and validate the response headers first, then obtain response storage
 * only when a body is actually needed. Deferred storage is sized exactly to
 * Content-Length + 1 (at most 8,193 bytes), so a small reply no longer reserves
 * the full JSON cap after TLS has connected. All length/type/encoding/framing
 * checks run before the allocation; an oversize reply is rejected without
 * allocating. A non-NULL caller buffer is never allocated, freed or resized.
 *
 * On success `storage` owns the parsed bytes (or borrows the caller buffer) and
 * the public wrappers release it exactly once. On failure the exchange loop
 * releases it, including after cancellation or a deadline reached while the
 * body was being read.
 */
static suite_translation_http_result_t read_response(
    response_reader_t *reader, const suite_translation_http_t *http,
    bool allow_200, bool allow_202, suite_translation_http_info_t *info,
    bool *body_available, response_storage_t *storage)
{
    int status = 0;
    response_headers_t headers = {0};
    suite_translation_http_result_t result =
        read_headers(reader, &status, &headers);
    if (result != SUITE_TR_HTTP_OK) return result;
    result = io_complete(reader->call);
    if (result != SUITE_TR_HTTP_OK) return result;
    info->http_status = status;
    if (status < 200 || status > 599) return SUITE_TR_HTTP_E_HTTP_STATUS;
    if (headers.content_length > SUITE_TR_PROTOCOL_JSON_MAX)
        return SUITE_TR_HTTP_E_OVERSIZE;
    size_t capacity = http->json ? http->json_cap : SUITE_TR_HTTP_JSON_CAP;
    if (headers.content_length >= capacity) return SUITE_TR_HTTP_E_OVERSIZE;

    bool expected = (status == 200 && allow_200) ||
        (status == 202 && allow_202);
    *body_available = false;
    if (expected) {
        if (!headers.content_type_seen || !headers.content_type_json)
            return SUITE_TR_HTTP_E_CONTENT_TYPE;
        if (headers.content_length == 0) return SUITE_TR_HTTP_E_LENGTH;
    } else if (status >= 300 && status < 400) {
        return io_complete(reader->call);
    } else if (!headers.content_type_seen || !headers.content_type_json ||
               headers.content_length == 0) {
        return io_complete(reader->call);
    }

    uint8_t *json = NULL;
    size_t json_cap = 0;
    bool owned = false;
    if (http->json) {
        json = http->json;
        json_cap = http->json_cap;
    } else {
        json_cap = (size_t)headers.content_length + 1U;
        json = heap_caps_malloc(json_cap,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!json) return SUITE_TR_HTTP_E_BUFFER;
        owned = true;
        secure_zero(json, json_cap);
    }
    storage->json = json;
    storage->cap = json_cap;
    storage->owned = owned;

    result = reader_read(reader, json, headers.content_length);
    if (result != SUITE_TR_HTTP_OK) return result;
    result = io_complete(reader->call);
    if (result != SUITE_TR_HTTP_OK) return result;
    json[headers.content_length] = 0;
    info->response_body_bytes = headers.content_length;
    *body_available = true;
    return SUITE_TR_HTTP_OK;
}

static suite_translation_http_result_t map_status(
    int status, const suite_translation_http_info_t *info)
{
    if (status >= 300 && status < 400) return SUITE_TR_HTTP_E_HTTP_REDIRECT;
    if (status == 401 || status == 403) return SUITE_TR_HTTP_E_HTTP_AUTH;
    if (status == 404) return SUITE_TR_HTTP_E_HTTP_NOT_FOUND;
    if (status == 409) return SUITE_TR_HTTP_E_HTTP_CONFLICT;
    if (status == 413) return SUITE_TR_HTTP_E_HTTP_LIMIT;
    if (status == 429) {
        if (!strcmp(info->remote_error, "QUOTA"))
            return SUITE_TR_HTTP_E_HTTP_QUOTA;
        return SUITE_TR_HTTP_E_HTTP_BUSY;
    }
    if (status == 400) return SUITE_TR_HTTP_E_HTTP_BAD_REQUEST;
    if (status >= 500 && status <= 599)
        return SUITE_TR_HTTP_E_HTTP_UNAVAILABLE;
    return SUITE_TR_HTTP_E_HTTP_STATUS;
}

static void capture_error(const uint8_t *json,
                          bool body_available,
                          suite_translation_http_info_t *info)
{
    if (!body_available || info->response_body_bytes == 0) return;
    suite_translation_protocol_error_t error =
        suite_translation_protocol_parse_error(
            json, info->response_body_bytes, info->remote_error,
            sizeof(info->remote_error));
    if (error != SUITE_TR_PROTOCOL_OK) info->remote_error[0] = 0;
}

static suite_translation_http_result_t exchange(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call, const char *method,
    const char *path, const char *content_type, const uint8_t *body,
    uint32_t body_length, suite_translation_http_audio_read_fn read,
    void *read_ctx, bool allow_200, bool allow_202,
    suite_translation_http_info_t *info, bool *body_available,
    response_storage_t *storage)
{
    if (storage) {
        storage->json = NULL;
        storage->cap = 0;
        storage->owned = false;
    }
    if (!http_valid(http) || !call || !info || !storage)
        return SUITE_TR_HTTP_E_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->protocol_error = SUITE_TR_PROTOCOL_OK;
    if (http->json) secure_zero(http->json, http->json_cap);
    *body_available = false;
    if (call->deadline_us <= esp_timer_get_time())
        return SUITE_TR_HTTP_E_DEADLINE;
    suite_translation_http_result_t result = connection_gate();
    if (result != SUITE_TR_HTTP_OK) return result;

    int64_t remaining_ms = (call->deadline_us - esp_timer_get_time()) / 1000;
    unsigned net_wait = 250;
    if (remaining_ms <= 0) return SUITE_TR_HTTP_E_DEADLINE;
    if (remaining_ms < (int64_t)net_wait) net_wait = (unsigned)remaining_ms;
    if (!suite_net_take(net_wait)) return SUITE_TR_HTTP_E_NET_BUSY;

    esp_tls_t *tls = NULL;
    response_reader_t reader = {0};
    bool request_started = false;
    bool net_held = true;
    char host[TR_HTTP_MAX_HOST];
    size_t host_length = strlen(http->origin) - 8U;
    if (host_length == 0 || host_length >= sizeof(host)) {
        result = SUITE_TR_HTTP_E_ARGUMENT;
        goto done;
    }
    memcpy(host, http->origin + 8, host_length);
    host[host_length] = 0;

    result = connection_gate();
    if (result != SUITE_TR_HTTP_OK) goto done;

    ip_addr_t address;
    result = dns_resolve(host, call, &address);
    if (result != SUITE_TR_HTTP_OK) goto done;

    result = tls_connect(host, &address, call, &tls, &info->tls_diag);
    if (result != SUITE_TR_HTTP_OK) goto done;

    if (cancelled(call)) {
        result = SUITE_TR_HTTP_E_CANCELLED;
        goto done;
    }
    if (expired(call)) {
        result = SUITE_TR_HTTP_E_DEADLINE;
        goto done;
    }

    uint32_t sent = 0;
    result = send_request(tls, http, call, method, path, host, content_type,
                          body, body_length, read, read_ctx,
                          &request_started, &sent);
    info->request_body_bytes = sent;
    if (result != SUITE_TR_HTTP_OK) {
        if (request_started &&
            (result == SUITE_TR_HTTP_E_IO ||
             result == SUITE_TR_HTTP_E_TLS ||
             result == SUITE_TR_HTTP_E_CANCELLED ||
             result == SUITE_TR_HTTP_E_DEADLINE))
            info->ambiguous = true;
        goto done;
    }

    reader.tls = tls;
    reader.call = call;
    result = read_response(&reader, http, allow_200, allow_202, info,
                           body_available, storage);
    if (result == SUITE_TR_HTTP_OK) result = io_complete(call);
    if (result != SUITE_TR_HTTP_OK && request_started &&
        (result == SUITE_TR_HTTP_E_IO ||
         result == SUITE_TR_HTTP_E_TLS ||
         result == SUITE_TR_HTTP_E_CANCELLED ||
         result == SUITE_TR_HTTP_E_DEADLINE ||
         result == SUITE_TR_HTTP_E_TRUNCATED ||
         result == SUITE_TR_HTTP_E_BUFFER))
        info->ambiguous = true;

done:
    if (tls) {
        esp_tls_conn_destroy(tls);
        tls = NULL;
    }
    if (net_held) {
        suite_net_give();
        net_held = false;
    }
    /* Cleanup may itself cross the deadline or observe cancellation. It must
       finish in the owning worker, but a late completion must never be
       published as success. */
    if (result == SUITE_TR_HTTP_OK) result = io_complete(call);
    if (result != SUITE_TR_HTTP_OK) {
        if (request_started &&
            (!strcmp(method, "POST") || !strcmp(method, "PUT")) &&
            (result == SUITE_TR_HTTP_E_CANCELLED ||
             result == SUITE_TR_HTTP_E_DEADLINE))
            info->ambiguous = true;
        *body_available = false;
        /* Exactly-once teardown: zero then free transport-owned storage, or
           zero and keep the caller-owned buffer. This also covers a late
           cancellation/deadline observed during cleanup. */
        release_response(storage);
    }
    secure_zero(&reader, sizeof(reader));
    secure_zero(host, sizeof(host));
    return result;
}

static void view_clear(suite_translation_protocol_view_t *view)
{
    if (!view) return;
    char *source = view->source;
    size_t source_cap = view->source_cap;
    char *target = view->target;
    size_t target_cap = view->target_cap;
    if (source && source_cap) secure_zero(source, source_cap);
    if (target && target_cap) secure_zero(target, target_cap);
    memset(view, 0, sizeof(*view));
    view->source = source;
    view->source_cap = source_cap;
    view->target = target;
    view->target_cap = target_cap;
    view->state = SUITE_TR_PROTOCOL_STATE_UNKNOWN;
}

static suite_translation_http_result_t finalize_view(
    const suite_translation_http_call_t *call,
    suite_translation_http_info_t *info,
    suite_translation_protocol_view_t *view, bool paid_request)
{
    suite_translation_http_result_t final = io_complete(call);
    if (final != SUITE_TR_HTTP_OK) {
        if (paid_request &&
            (final == SUITE_TR_HTTP_E_CANCELLED ||
             final == SUITE_TR_HTTP_E_DEADLINE))
            info->ambiguous = true;
        view_clear(view);
    }
    return final;
}

static suite_translation_http_result_t parse_expected_view(
    const uint8_t *json,
    suite_translation_http_info_t *info, bool body_available,
    int expected_status, suite_translation_protocol_view_t *view,
    const suite_translation_protocol_identity_t *expected, bool admission,
    bool cancel)
{
    if (!body_available || info->http_status != expected_status) {
        view_clear(view);
        return map_status(info->http_status, info);
    }
    suite_translation_protocol_error_t error;
    if (admission) {
        error = suite_translation_protocol_parse_admission(
            json, info->response_body_bytes, expected->operation_id,
            expected->direction, expected->audio_bytes, view);
    } else if (cancel) {
        error = suite_translation_protocol_parse_cancel_view(
            json, info->response_body_bytes, expected, view);
    } else {
        error = suite_translation_protocol_parse_view(
            json, info->response_body_bytes, expected, view);
    }
    info->protocol_error = error;
    if (error == SUITE_TR_PROTOCOL_OK) return SUITE_TR_HTTP_OK;
    view_clear(view);
    if (error == SUITE_TR_PROTOCOL_E_IDENTITY)
        return SUITE_TR_HTTP_E_IDENTITY;
    return SUITE_TR_HTTP_E_PROTOCOL;
}

suite_translation_http_result_t suite_translation_http_init(
    suite_translation_http_t *http, const char *origin, const char *token,
    uint8_t *json, size_t json_cap)
{
    if (!http) return SUITE_TR_HTTP_E_ARGUMENT;
    http->origin = origin;
    http->token = token;
    http->json = json;
    http->json_cap = json_cap;
    if (!http_valid(http)) return SUITE_TR_HTTP_E_ARGUMENT;
    return SUITE_TR_HTTP_OK;
}

suite_translation_http_result_t suite_translation_http_ready(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    suite_translation_protocol_ready_t *ready,
    suite_translation_http_info_t *info)
{
    if (!ready) return SUITE_TR_HTTP_E_ARGUMENT;
    memset(ready, 0, sizeof(*ready));
    if (info) memset(info, 0, sizeof(*info));
    bool body = false;
    response_storage_t storage;
    suite_translation_http_result_t result = exchange(
        http, call, "GET", TR_HTTP_READY_PATH, NULL, NULL, 0, NULL, NULL,
        true, false, info, &body, &storage);
    if (result != SUITE_TR_HTTP_OK) return result;
    if (info->http_status != 200) {
        capture_error(storage.json, body, info);
        result = map_status(info->http_status, info);
        release_response(&storage);
        if (info->http_status == 503 || info->http_status == 429)
            return SUITE_TR_HTTP_E_NOT_READY;
        return result;
    }
    if (!body) {
        release_response(&storage);
        return SUITE_TR_HTTP_E_LENGTH;
    }
    suite_translation_protocol_error_t error =
        suite_translation_protocol_parse_ready(storage.json,
                                               info->response_body_bytes,
                                               ready);
    info->protocol_error = error;
    if (error != SUITE_TR_PROTOCOL_OK) {
        release_response(&storage);
        memset(ready, 0, sizeof(*ready));
        return SUITE_TR_HTTP_E_PROTOCOL;
    }
    release_response(&storage);
    suite_translation_http_result_t final = io_complete(call);
    if (final != SUITE_TR_HTTP_OK) {
        memset(ready, 0, sizeof(*ready));
        return final;
    }
    return SUITE_TR_HTTP_OK;
}

suite_translation_http_result_t suite_translation_http_admit(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call, const char *operation_id,
    suite_translation_protocol_direction_t direction,
    const char *audio_sha256, uint32_t audio_bytes,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info)
{
    if (!view) return SUITE_TR_HTTP_E_ARGUMENT;
    view_clear(view);
    if (info) memset(info, 0, sizeof(*info));
    if (!operation_id || !audio_sha256)
        return SUITE_TR_HTTP_E_ARGUMENT;
    char request[320];
    size_t request_length = 0;
    suite_translation_protocol_error_t error =
        suite_translation_protocol_build_admission(
            request, sizeof(request), operation_id, direction, audio_sha256,
            audio_bytes, &request_length);
    if (error != SUITE_TR_PROTOCOL_OK) {
        secure_zero(request, sizeof(request));
        return SUITE_TR_HTTP_E_ARGUMENT;
    }

    bool body = false;
    response_storage_t storage;
    suite_translation_http_result_t result = exchange(
        http, call, "POST", TR_HTTP_PATH, "application/json",
        (const uint8_t *)request, (uint32_t)request_length, NULL, NULL,
        false, true, info, &body, &storage);
    secure_zero(request, sizeof(request));
    if (result != SUITE_TR_HTTP_OK) return result;
    if (info->http_status != 202) {
        capture_error(storage.json, body, info);
        view_clear(view);
        result = map_status(info->http_status, info);
        release_response(&storage);
        return result;
    }
    suite_translation_protocol_identity_t expected = {
        .operation_id = operation_id,
        .job_id = NULL,
        .generation = 0,
        .direction = direction,
        .audio_bytes = audio_bytes,
    };
    result = parse_expected_view(storage.json, info, body, 202, view,
                                 &expected, true, false);
    release_response(&storage);
    if (result == SUITE_TR_HTTP_OK)
        result = finalize_view(call, info, view, true);
    return result;
}

suite_translation_http_result_t suite_translation_http_upload(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_http_audio_read_fn read, void *read_ctx,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info)
{
    if (!view) return SUITE_TR_HTTP_E_ARGUMENT;
    view_clear(view);
    if (info) memset(info, 0, sizeof(*info));
    if (!expected || !read ||
        !suite_translation_protocol_id_valid(expected->operation_id) ||
        !suite_translation_protocol_id_valid(expected->job_id) ||
        expected->generation == 0 ||
        expected->audio_bytes < SUITE_TR_HTTP_AUDIO_MIN ||
        expected->audio_bytes > SUITE_TR_HTTP_AUDIO_MAX)
        return SUITE_TR_HTTP_E_ARGUMENT;
    char path[TR_HTTP_MAX_PATH];
    if (!make_job_path(path, sizeof(path), expected->job_id, "/audio"))
        return SUITE_TR_HTTP_E_ARGUMENT;

    bool body = false;
    response_storage_t storage;
    suite_translation_http_result_t result = exchange(
        http, call, "PUT", path, "audio/wav", NULL, expected->audio_bytes,
        read, read_ctx, true, true, info, &body, &storage);
    if (result != SUITE_TR_HTTP_OK) return result;
    if (info->http_status != 200 && info->http_status != 202) {
        capture_error(storage.json, body, info);
        view_clear(view);
        result = map_status(info->http_status, info);
        release_response(&storage);
        return result;
    }
    result = parse_expected_view(storage.json, info, body,
                                 info->http_status, view, expected, false,
                                 false);
    release_response(&storage);
    if (result == SUITE_TR_HTTP_OK)
        result = finalize_view(call, info, view, true);
    return result;
}

suite_translation_http_result_t suite_translation_http_status(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info)
{
    if (!view) return SUITE_TR_HTTP_E_ARGUMENT;
    view_clear(view);
    if (info) memset(info, 0, sizeof(*info));
    if (!expected ||
        !suite_translation_protocol_id_valid(expected->operation_id) ||
        !suite_translation_protocol_id_valid(expected->job_id) ||
        expected->generation == 0 ||
        expected->audio_bytes < SUITE_TR_HTTP_AUDIO_MIN ||
        expected->audio_bytes > SUITE_TR_HTTP_AUDIO_MAX)
        return SUITE_TR_HTTP_E_ARGUMENT;
    char path[TR_HTTP_MAX_PATH];
    if (!make_job_path(path, sizeof(path), expected->job_id, ""))
        return SUITE_TR_HTTP_E_ARGUMENT;

    bool body = false;
    response_storage_t storage;
    suite_translation_http_result_t result = exchange(
        http, call, "GET", path, NULL, NULL, 0, NULL, NULL, true, false,
        info, &body, &storage);
    if (result != SUITE_TR_HTTP_OK) return result;
    if (info->http_status != 200) {
        capture_error(storage.json, body, info);
        view_clear(view);
        result = map_status(info->http_status, info);
        release_response(&storage);
        return result;
    }
    result = parse_expected_view(storage.json, info, body, 200, view,
                                 expected, false, false);
    release_response(&storage);
    if (result == SUITE_TR_HTTP_OK)
        result = finalize_view(call, info, view, false);
    return result;
}

suite_translation_http_result_t suite_translation_http_cancel(
    const suite_translation_http_t *http,
    const suite_translation_http_call_t *call,
    const suite_translation_protocol_identity_t *expected,
    suite_translation_protocol_view_t *view,
    suite_translation_http_info_t *info)
{
    view_clear(view);
    if (info) memset(info, 0, sizeof(*info));
    if (!expected ||
        !suite_translation_protocol_id_valid(expected->operation_id) ||
        !suite_translation_protocol_id_valid(expected->job_id) ||
        expected->generation == 0 ||
        expected->audio_bytes < SUITE_TR_HTTP_AUDIO_MIN ||
        expected->audio_bytes > SUITE_TR_HTTP_AUDIO_MAX)
        return SUITE_TR_HTTP_E_ARGUMENT;
    char path[TR_HTTP_MAX_PATH];
    if (!make_job_path(path, sizeof(path), expected->job_id, ""))
        return SUITE_TR_HTTP_E_ARGUMENT;

    suite_translation_protocol_view_t discarded;
    if (!view) {
        suite_translation_protocol_view_init(&discarded, NULL, 0, NULL, 0);
        view = &discarded;
    }
    bool body = false;
    response_storage_t storage;
    suite_translation_http_result_t result = exchange(
        http, call, "DELETE", path, NULL, NULL, 0, NULL, NULL, true, false,
        info, &body, &storage);
    if (result != SUITE_TR_HTTP_OK) return result;
    if (info->http_status != 200) {
        capture_error(storage.json, body, info);
        view_clear(view);
        result = map_status(info->http_status, info);
        release_response(&storage);
        return result;
    }
    result = parse_expected_view(storage.json, info, body, 200, view,
                                 expected, false, true);
    release_response(&storage);
    if (result == SUITE_TR_HTTP_OK)
        result = finalize_view(call, info, view, false);
    return result;
}

const char *suite_translation_http_result_name(
    suite_translation_http_result_t result)
{
    switch (result) {
    case SUITE_TR_HTTP_OK: return "OK";
    case SUITE_TR_HTTP_E_ARGUMENT: return "ARGUMENT";
    case SUITE_TR_HTTP_E_BUFFER: return "BUFFER";
    case SUITE_TR_HTTP_E_OFFLINE: return "OFFLINE";
    case SUITE_TR_HTTP_E_TIME: return "TIME";
    case SUITE_TR_HTTP_E_NET_BUSY: return "NET_BUSY";
    case SUITE_TR_HTTP_E_DNS: return "DNS";
    case SUITE_TR_HTTP_E_TLS: return "TLS";
    case SUITE_TR_HTTP_E_IO: return "IO";
    case SUITE_TR_HTTP_E_CANCELLED: return "CANCELLED";
    case SUITE_TR_HTTP_E_DEADLINE: return "DEADLINE";
    case SUITE_TR_HTTP_E_HTTP_REDIRECT: return "HTTP_REDIRECT";
    case SUITE_TR_HTTP_E_HTTP_AUTH: return "HTTP_AUTH";
    case SUITE_TR_HTTP_E_HTTP_NOT_FOUND: return "HTTP_NOT_FOUND";
    case SUITE_TR_HTTP_E_HTTP_CONFLICT: return "HTTP_CONFLICT";
    case SUITE_TR_HTTP_E_HTTP_BUSY: return "HTTP_BUSY";
    case SUITE_TR_HTTP_E_HTTP_QUOTA: return "HTTP_QUOTA";
    case SUITE_TR_HTTP_E_HTTP_LIMIT: return "HTTP_LIMIT";
    case SUITE_TR_HTTP_E_HTTP_BAD_REQUEST: return "HTTP_BAD_REQUEST";
    case SUITE_TR_HTTP_E_HTTP_UNAVAILABLE: return "HTTP_UNAVAILABLE";
    case SUITE_TR_HTTP_E_HTTP_STATUS: return "HTTP_STATUS";
    case SUITE_TR_HTTP_E_CONTENT_TYPE: return "CONTENT_TYPE";
    case SUITE_TR_HTTP_E_CONTENT_ENCODING: return "CONTENT_ENCODING";
    case SUITE_TR_HTTP_E_TRANSFER_ENCODING: return "TRANSFER_ENCODING";
    case SUITE_TR_HTTP_E_LENGTH: return "LENGTH";
    case SUITE_TR_HTTP_E_LIMIT: return "LIMIT";
    case SUITE_TR_HTTP_E_OVERSIZE: return "OVERSIZE";
    case SUITE_TR_HTTP_E_TRUNCATED: return "TRUNCATED";
    case SUITE_TR_HTTP_E_PROTOCOL: return "PROTOCOL";
    case SUITE_TR_HTTP_E_IDENTITY: return "IDENTITY";
    case SUITE_TR_HTTP_E_NOT_READY: return "NOT_READY";
    default: return "UNKNOWN";
    }
}
