// main/suite_provision_qr.h —— 配网页二维码的布局与 I1 绘制(纯逻辑,可主机测试)。
//
// 与 LVGL 解耦:调用方用 lv_draw_buf_goto_xy(db, 0, 0) 取得像素区起点
// (I1 索引画布的调色板位于 draw_buf->data 开头,像素区在其后),本模块
// 只负责整数缩放/居中/静区计算与按位写入,并保留调色板不变。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 二维码四周最少保留的静区模块数(QR 规范要求 4)。
#define SUITE_QR_QUIET_MODULES 4

// I1 索引画布调色板占用字节数:2 项 × sizeof(lv_color32_t)。
// LVGL 的 lv_draw_buf_goto_xy/set_palette 把它放在 draw_buf->data 开头。
#define SUITE_QR_I1_PALETTE_BYTES 8

// 模块矩阵回调:返回 (x, y) 模块是否黑(索引 1)。
typedef bool (*suite_qr_module_fn)(const void *ctx, int x, int y);

typedef struct {
    int modules;  // 每边模块数
    int canvas;   // 画布边长(像素)
    int scale;    // 整数缩放(像素/模块)
    int offset;   // 首个模块左上角的像素偏移(即静区)
    int drawn;    // 内容边长(像素)= modules * scale
} suite_qr_layout_t;

// 计算整数缩放与居中偏移;静区不足 SUITE_QR_QUIET_MODULES 个模块或参数
// 非法时返回 false(不绘制,避免内容溢出画布)。
bool suite_qr_layout_calc(int modules, int canvas, suite_qr_layout_t *out);

// 把模块矩阵放大绘制到 I1 画布像素区(不含调色板):
//   1) 清零整幅像素区(保留 pixels 之前的调色板字节);
//   2) 黑模块按 layout->scale 放大,逐位写入。
// stride 为行字节数,须至少容纳一行像素;cap 为像素区可用字节数。
// 参数非法或容量不足返回 false 且不写任何字节。
bool suite_qr_paint_i1(uint8_t *pixels, size_t cap, size_t stride,
                       const suite_qr_layout_t *layout,
                       suite_qr_module_fn module, const void *ctx);
