#include "travel_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "esp_netif.h"

#include "travel_cfg.h"
#include "travel_state.h"

static const char *TAG = "prov";

#define STA_STOP_NONE 0
#define AP_PASS_BYTES 16

static int ap_netif_id = -1;
static httpd_handle_t h_server = NULL;
static volatile int s_state = PROV_IDLE;
static uint64_t s_expires_ms;
static char s_ap_pass[AP_PASS_BYTES + 1];
static char s_ssid[33];

/* Page content (no secrets echoed back). The password is shown only by the
 * device UI application (P03 page), assembled by strlen-safe locking. */

static const char PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Passport provisioning</title></head><body>"
"<h1>Passport provisioning (5 min)</h1>"
"<p>This device hotspot serves the local form only. The remote server is"
" contacted by the device itself over HTTPS.</p>"
"<form method=POST action=/apply>"
"<p>Wi-Fi SSID<br><input name=ssid maxlength=32 required></p>"
"<p>Wi-Fi password<br><input name=password type=password maxlength=64></p>"
"<p>Server HTTPS base URL<br><input name=url required "
"placeholder='https://passport.example'></p>"
"<p>Binding code from the website<br><input name=code maxlength=6 "
"pattern='[A-Z26-9]{6}' required></p>"
"<p>Device CA certificate (PEM, base64 + lines)<br>"
"<textarea name=ca rows=6 required></textarea></p>"
"<p>Current date/time local epoch seconds<br>"
"<input name=time type=number min=1700000000 required></p>"
"<p><label><input type=checkbox required> I confirm this network and time"
" are correct and bind this device.</label></p>"
"<button>Apply</button></form>"
"<p><a href=/cancel>Cancel provisioning</a></p>"
"</body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t cancel_get(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "<html><body>cancelled; you may close this page"
                            "</body></html>");
    s_state = PROV_IDLE;
    travel_prov_stop();
    return ESP_OK;
}

static bool post_field(const char *body, const char *name, char *out, size_t cap)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    const char *found = strstr(body, pattern);
    if (!found)
        return false;
    if (found != body && found[-1] != '&' && found[-1] != '\n')
        return false; /* avoid "xxxpassword=" collisions */
    found += strlen(pattern);
    size_t i = 0;
    while (found[i] && found[i] != '&' && i + 1 < cap) {
        char out_ch = found[i] == '+' ? ' ' : found[i];
        if (out_ch == '%' && found[i + 1] && found[i + 2]) {
            char hex[3] = {found[i + 1], found[i + 2], '\0'};
            long value = strtol(hex, NULL, 16);
            if (value > 0) {
                out_ch = (char)value;
                i += 2;
            }
        }
        out[i] = out_ch;
        ++i;
    }
    out[i] = '\0';
    return true;
}

static esp_err_t apply_post(httpd_req_t *req)
{
    char body[4096];
    int recv = httpd_req_recv(req, body,
                              sizeof(body) - sizeof(body) / 4);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    body[recv < 0 ? 0 : recv] = '\0';
    char ssid[TRAVEL_CFG_SSID_MAX];
    char pass[TRAVEL_CFG_PASS_MAX];
    char url[TRAVEL_CFG_URL_MAX];
    char code[8];
    char ca[TRAVEL_CFG_CA_MAX];
    char timestr[16];
    if (!post_field(body, "ssid", ssid, sizeof(ssid)) ||
            !post_field(body, "password", pass, sizeof(pass)) ||
            !post_field(body, "url", url, sizeof(url)) ||
            !post_field(body, "code", code, sizeof(code)) ||
            !post_field(body, "ca", ca, sizeof(ca)) ||
            !post_field(body, "time", timestr, sizeof(timestr))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        return ESP_OK;
    }
    int64_t user_epoch = atoll(timestr);
    /* URL must be https; never log any field of the body */
    if (strncmp(url, "https://", 8) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "server URL must be https://");
        return ESP_OK;
    }
    unsigned long rt = travel_prov_remaining_ms();
    (void)rt;
    /* Persist settings durably BEFORE switch; keep old settings until the
     * replacement connection verifies (PRD §9). */
    if (travel_cfg_set_wifi(ssid, pass) != 0 ||
            travel_cfg_set_server(url, ca) != 0) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    /* stash the binding code + user time for the worker via NVS temp keys
     * (records namespace "provtmp", erased after use; never in logs). */
    nvs_handle_t h;
    nvs_flash_init_partition("records");
    if (nvs_open_from_partition("records", "provtmp", NVS_READWRITE, &h)
            != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    nvs_set_str(h, "bindcode", code);
    nvs_set_i64(h, "user_time", user_epoch);
    nvs_set_u64(h, "applied_ms", (uint64_t)esp_log_timestamp());
    nvs_commit(h);
    nvs_close(h);
    s_state = PROV_APPLYING;
    httpd_resp_sendstr(req, "<html><body>saved; the device will connect."
                            "</body></html>");
    return ESP_OK;
}

int travel_prov_start(void)
{
    if (s_state == PROV_HOTSPOT_RUNNING || s_state == PROV_APPLYING)
        return -1;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap)
        return -1;
    wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wi));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t cfg = {0};
    /* per-session random SSID/password; password shown only on screen */
    snprintf(s_ssid, sizeof(s_ssid), "Passport-D1-%04X",
             (unsigned)(esp_random() & 0xFFFF));
    for (unsigned i = 0; i < AP_PASS_BYTES; ++i) {
        static const char alphabet[] =
            "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
        s_ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_ap_pass[AP_PASS_BYTES] = '\0';
    strlcpy((char *)cfg.ap.ssid, s_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password));
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 1;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    h_server = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    if (httpd_start(&h_server, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "httpd failed");
        travel_prov_stop();
        return -1;
    }
    httpd_uri_t get = {"/", HTTP_GET, page_get, NULL};
    httpd_uri_t form = {"/apply", HTTP_POST, apply_post, NULL};
    httpd_uri_t cncl = {"/cancel", HTTP_GET, cancel_get, NULL};
    httpd_register_uri_handler(h_server, &get);
    httpd_register_uri_handler(h_server, &form);
    httpd_register_uri_handler(h_server, &cncl);
    s_state = PROV_HOTSPOT_RUNNING;
    s_expires_ms = (uint64_t)esp_log_timestamp() + PASSPORT_PROV_TTL_MS;
    return 0;
}

void travel_prov_stop(void)
{
    if (h_server) {
        httpd_stop(h_server);
        h_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (ap_netif_id >= 0) {
    }
    memset(s_ap_pass, 0, sizeof(s_ap_pass)); /* hotspot secret erased */
    s_state = PROV_IDLE;
}

enum travel_prov_state travel_prov_state(void)
{
    return s_state;
}

unsigned long travel_prov_remaining_ms(void)
{
    if (s_state != PROV_HOTSPOT_RUNNING)
        return 0;
    uint64_t now = (uint64_t)esp_log_timestamp();
    if (now >= s_expires_ms)
        return 0;
    return (unsigned long)(s_expires_ms - now);
}
