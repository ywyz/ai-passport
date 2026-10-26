#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef long ssize_t;

typedef struct esp_tls esp_tls_t;

typedef enum {
    ESP_TLS_INIT = 0,
    ESP_TLS_CONNECTING,
    ESP_TLS_HANDSHAKE,
    ESP_TLS_FAIL,
    ESP_TLS_DONE
} esp_tls_conn_state_t;

typedef enum {
    ESP_TLS_AF_UNSPEC = 0,
    ESP_TLS_AF_INET,
    ESP_TLS_AF_INET6
} esp_tls_addr_family_t;

typedef enum {
    ESP_TLS_ERR_TYPE_UNKNOWN = 0,
    ESP_TLS_ERR_TYPE_SYSTEM,
    ESP_TLS_ERR_TYPE_MBEDTLS,
    ESP_TLS_ERR_TYPE_MBEDTLS_CERT_FLAGS,
    ESP_TLS_ERR_TYPE_ESP,
    ESP_TLS_ERR_TYPE_MAX
} esp_tls_error_type_t;

typedef struct esp_tls_last_error {
    int last_error;
    int esp_tls_error_code;
    int esp_tls_flags;
} esp_tls_last_error_t;

typedef struct esp_tls_last_error *esp_tls_error_handle_t;

typedef struct esp_tls_cfg {
    bool non_block;
    int timeout_ms;
    bool use_global_ca_store;
    const char *common_name;
    bool skip_common_name;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool is_plain_tcp;
    esp_tls_addr_family_t addr_family;
} esp_tls_cfg_t;

esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_async(const char *hostname, int hostlen, int port,
                           const esp_tls_cfg_t *cfg, esp_tls_t *tls);
int esp_tls_conn_destroy(esp_tls_t *tls);
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t datalen);
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *data, size_t datalen);
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *tls, int *sockfd);
esp_err_t esp_tls_get_conn_state(esp_tls_t *tls,
                                 esp_tls_conn_state_t *conn_state);
esp_err_t esp_tls_get_error_handle(esp_tls_t *tls,
                                   esp_tls_error_handle_t *handle);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t handle,
                                           int *esp_tls_code,
                                           int *esp_tls_flags);
esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t handle,
                                           esp_tls_error_type_t type,
                                           int *error_code);

#define ESP_TLS_ERR_SSL_WANT_READ (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-0x6880)
