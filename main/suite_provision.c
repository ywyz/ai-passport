// main/suite_provision.c —— 手机扫码 BLE 配网页(240×320,深色主题)。
//
// 布局:标题"手机配网" → 150×150 二维码(I1 索引画布,约 3KB)→
//       6 位配对码 → 实时状态行 → "长按 返回"。
//
// 生命周期(AGENTS.md 硬性规定):
//   enter(持 LVGL 锁)生成配对码、启动 BLE 广播、建 UI 与 250ms 轮询 timer;
//   BACK 动作(输入任务上下文,未持锁)先停 BLE(停广播/断连/收任务),
//   再执行页面退栈;exit(持锁)先删 timer、断开任务可触达的 UI 引用,再删屏。
#include "suite_provision.h"

#include "suite_prov_ble.h"
#include "suite_prov_proto.h"

#include "esp_log.h"
#include "esp_random.h"
#include "qrcode.h"

#include "bsp_display.h"
#include "lvgl.h"

#include "fonts/suite_fonts.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "suite_provision_ui";

// ---- 与既有页面一致的深色主题 ----
#define C_BG 0x0E1420
#define C_CARD 0x1A2332
#define C_INK 0xE8EEF7
#define C_DIM 0x8CA0B8
#define C_OK 0x59C98B
#define C_WARN 0xE0B454
#define C_ERR 0xE06C6C

#define QR_SIZE 150
#define QR_Y 30
// I1 每行约 19 字节,按 4 字节对齐后 20;预留到 27,缓冲约 4KB 静态 BSS。
#define QR_ROW_STRIDE_MAX 27

static lv_obj_t *s_scr;
static lv_obj_t *s_qr;
static lv_obj_t *s_pin_digits;
static lv_obj_t *s_status_label;
static lv_timer_t *s_timer;
static uint8_t s_qr_buf[QR_SIZE * QR_ROW_STRIDE_MAX];
static bool s_qr_ready;
static bool s_ble_started;
static char s_pin[SUITE_PROV_PIN_LEN + 1];

// 二维码生成完成回调:把模块矩阵放大后直接写 I1 画布位图
// (黑=索引1,白=索引0),避免逐像素 set_px 触发上万次重绘失效。
static void paint_qr_to_canvas(esp_qrcode_handle_t qr)
{
    if (!s_qr) return;
    int mods = esp_qrcode_get_size(qr);
    // 四周保留至少 4 个模块的静区,再取整数倍像素放大。
    int scale = QR_SIZE / (mods + 8);
    if (scale < 1) scale = 1;
    int drawn = mods * scale;
    int off = (QR_SIZE - drawn) / 2;

    lv_draw_buf_t *db = lv_canvas_get_draw_buf(s_qr);
    if (!db || !db->data || db->header.stride > QR_ROW_STRIDE_MAX) {
        ESP_LOGE(TAG, "二维码画布缓冲异常(stride=%u)",
                 db ? db->header.stride : 0);
        return;
    }
    uint8_t *data = db->data;
    uint32_t stride = db->header.stride;
    memset(data, 0, stride * QR_SIZE);  // 整幅白(索引 0)
    for (int my = 0; my < mods; my++) {
        for (int mx = 0; mx < mods; mx++) {
            if (!esp_qrcode_get_module(qr, mx, my)) continue;
            for (int dy = 0; dy < scale; dy++) {
                int py = off + my * scale + dy;
                for (int dx = 0; dx < scale; dx++) {
                    int px = off + mx * scale + dx;
                    data[py * stride + (px >> 3)] |=
                        (uint8_t)(0x80 >> (px & 0x07));
                }
            }
        }
    }
    lv_obj_invalidate(s_qr);
    s_qr_ready = true;
}

// 把 URL 编码成二维码(espressif/qrcode 0.1 回调式 API)。
static void render_qr(void)
{
    if (s_qr_ready || !s_qr) return;

    char id[5];
    if (!suite_prov_ble_device_id(id)) return;  // host 尚未同步,timer 会重试

    char url[48];
    int n = snprintf(url, sizeof(url), "https://passport.ywyz.tech/?d=%s",
                     id);
    if (n <= 0 || (size_t)n >= sizeof(url)) return;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = paint_qr_to_canvas;
    cfg.max_qrcode_version = 10;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;  // 屏幕小,中等容错更易扫
    esp_err_t err = esp_qrcode_generate(&cfg, url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "二维码生成失败: %s", esp_err_to_name(err));
    }
}

// 断开原因 → 面向用户的简短中文原因(与网页映射保持一致)。
static const char *reason_text(uint16_t reason)
{
    switch (reason) {
    case 15:  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT,密码错误最常见
    case 16:  // WIFI_REASON_HANDSHAKE_TIMEOUT
        return "密码错误";
    case 200:  // WIFI_REASON_NO_AP_FOUND(找不到网络)
    case 201:  // WIFI_REASON_AUTH_FAIL(老芯片常以此上报找不到/拒绝)
        return "找不到网络";
    default:
        return "连接失败";
    }
}

static void refresh_status(void)
{
    suite_prov_snapshot_t snap;
    suite_prov_ble_snapshot(&snap);

    char buf[40];
    uint32_t color = C_DIM;

    if (!s_ble_started) {
        strlcpy(buf, "配网启动失败", sizeof(buf));
        color = C_ERR;
    } else {
        switch (snap.phase) {
        case SUITE_PROV_PHASE_ADVERTISING:
            strlcpy(buf, "等待连接", sizeof(buf));
            break;
        case SUITE_PROV_PHASE_CONNECTED:
            strlcpy(buf, "请输入配对码", sizeof(buf));
            color = C_WARN;
            break;
        case SUITE_PROV_PHASE_AUTHED:
            switch (snap.wifi) {
            case SUITE_CONN_ONLINE:
                strlcpy(buf, "配网成功", sizeof(buf));
                color = C_OK;
                break;
            case SUITE_CONN_CONNECTING:
            case SUITE_CONN_WAIT_IP:
                strlcpy(buf, "网络连接中", sizeof(buf));
                color = C_WARN;
                break;
            case SUITE_CONN_RETRY_WAIT:
                strlcpy(buf, reason_text(snap.reason), sizeof(buf));
                color = C_ERR;
                break;
            default:
                strlcpy(buf, "已连接", sizeof(buf));
                color = C_INK;
                break;
            }
            break;
        default:
            strlcpy(buf, "等待连接", sizeof(buf));
            break;
        }
    }

    if (strcmp(buf, lv_label_get_text(s_status_label)) != 0) {
        lv_label_set_text(s_status_label, buf);
    }
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(color), 0);
}

static void provision_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!bsp_lvgl_lock(200)) return;
    if (!s_scr) {
        bsp_lvgl_unlock();
        return;
    }
    if (!s_qr_ready) render_qr();
    refresh_status();
    bsp_lvgl_unlock();
}

// 生成不含 000000 偏置处理的 6 位数字串(允许前导 0,共一百万种)。
// 配对码只在本页屏幕上显示,不写日志、不入 URL。
static void generate_pin(void)
{
    uint32_t v = esp_random() % 1000000u;
    snprintf(s_pin, sizeof(s_pin), "%06u", (unsigned)v);
}

static void provision_enter(void)
{
    s_qr_ready = false;
    generate_pin();

    // 先建广播再建 UI;失败时状态行提示,页面仍可长按返回。
    esp_err_t err = suite_prov_ble_begin(s_pin);
    s_ble_started = err == ESP_OK;
    if (!s_ble_started) {
        ESP_LOGE(TAG, "BLE 配网启动失败: %s", esp_err_to_name(err));
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &suite_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(title, "手机配网");

    // 1-bit 索引画布:150×150 仅约 3KB(严禁 RGB565,会占 51KB 撑爆 LVGL 池)。
    s_qr = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_qr, s_qr_buf, QR_SIZE, QR_SIZE,
                         LV_COLOR_FORMAT_I1);
    lv_canvas_set_palette(s_qr, 0,
                          (lv_color32_t){.red = 255, .green = 255,
                                         .blue = 255, .alpha = 255});
    lv_canvas_set_palette(s_qr, 1,
                          (lv_color32_t){.red = 0, .green = 0,
                                         .blue = 0, .alpha = 255});
    lv_obj_align(s_qr, LV_ALIGN_TOP_MID, 0, QR_Y);
    // 卡片式描边让白色静区在深底上可见。
    lv_obj_set_style_border_color(s_qr, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_width(s_qr, 2, 0);

    // LVGL 单标签不能混排字体:汉字小标题用 16 号中文字体,
    // 6 位数字单独用内置 Montserrat 20 放大显示。
    lv_obj_t *pin_caption = lv_label_create(s_scr);
    lv_obj_set_style_text_font(pin_caption, &suite_font_16, 0);
    lv_obj_set_style_text_color(pin_caption, lv_color_hex(C_DIM), 0);
    lv_obj_align(pin_caption, LV_ALIGN_TOP_MID, 0, QR_Y + QR_SIZE + 8);
    lv_label_set_text(pin_caption, "配对码");

    s_pin_digits = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_pin_digits, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pin_digits, lv_color_hex(C_INK), 0);
    lv_obj_align(s_pin_digits, LV_ALIGN_TOP_MID, 0, QR_Y + QR_SIZE + 28);
    lv_label_set_text(s_pin_digits, s_pin);  // 仅上屏,绝不写日志

    s_status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status_label, &suite_font_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(C_DIM), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0,
                 QR_Y + QR_SIZE + 60);
    lv_label_set_text(s_status_label, "等待连接");

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_obj_set_style_text_font(hint, &suite_font_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(hint, "长按 返回");

    render_qr();  // host 已同步时立即出码,否则交给 250ms timer
    s_timer = lv_timer_create(provision_tick, 250, NULL);
    lv_screen_load(s_scr);
}

static void provision_exit(void)
{
    // 顺序:先删 timer(producer),再断 UI 引用,最后删屏。
    // BLE 已在 BACK 动作里停止;此处兜底(页面理论上只会经 BACK 退出)。
    if (s_ble_started) {
        suite_prov_ble_end();
        s_ble_started = false;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_qr = NULL;
    s_pin_digits = NULL;
    s_status_label = NULL;
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static void on_provision_action(suite_action_t act)
{
    if (act != SUITE_ACTION_BACK) return;
    // 导航必须在未持 LVGL 锁的输入任务上下文执行(bsp_lvgl_lock 非递归)。
    // 先停广播、断开手机并收走 prov 任务,再退栈触发持锁 exit 删 UI。
    if (s_ble_started) {
        suite_prov_ble_end();
        s_ble_started = false;
    }
    (void)suite_app_back();
}

const suite_page_t SUITE_PAGE_PROVISION = {
    .name = "provision",
    .enter = provision_enter,
    .exit = provision_exit,
    .action = on_provision_action,
};
