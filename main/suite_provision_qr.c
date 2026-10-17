// main/suite_provision_qr.c —— 配网页二维码布局与 I1 绘制实现(纯逻辑)。
#include "suite_provision_qr.h"

#include <string.h>

bool suite_qr_layout_calc(int modules, int canvas, suite_qr_layout_t *out)
{
    if (!out || modules <= 0 || canvas <= 0) return false;

    // 先按"内容 + 两侧各 4 模块静区"取整数缩放;取整只会让静区更大。
    int scale = canvas / (modules + 2 * SUITE_QR_QUIET_MODULES);
    if (scale < 1) scale = 1;
    int drawn = modules * scale;
    if (drawn > canvas) return false;  // 模块数超过画布,无法绘制

    int offset = (canvas - drawn) / 2;  // 居中(差值奇数时右侧多 1 像素)
    if (offset < SUITE_QR_QUIET_MODULES * scale) return false;  // 静区不足

    out->modules = modules;
    out->canvas = canvas;
    out->scale = scale;
    out->offset = offset;
    out->drawn = drawn;
    return true;
}

bool suite_qr_paint_i1(uint8_t *pixels, size_t cap, size_t stride,
                       const suite_qr_layout_t *layout,
                       suite_qr_module_fn module, const void *ctx)
{
    if (!pixels || !layout || !module) return false;

    int canvas = layout->canvas;
    if (canvas <= 0 || layout->modules <= 0 || layout->scale < 1) return false;
    // 行字节数必须容纳一行像素;像素区必须容纳整幅画布。
    if (stride < (size_t)((canvas + 7) / 8)) return false;
    if (stride * (size_t)canvas > cap) return false;
    // 布局边界自检:绘制区必须完整落在画布内,且与模块数一致。
    if (layout->offset < 0 || layout->drawn < 0 ||
        layout->offset + layout->drawn > canvas ||
        layout->modules * layout->scale != layout->drawn) {
        return false;
    }

    // 整幅清零(索引 0 = 白)。pixels 指向调色板之后的像素区,不触碰调色板。
    memset(pixels, 0, stride * (size_t)canvas);

    for (int my = 0; my < layout->modules; my++) {
        for (int mx = 0; mx < layout->modules; mx++) {
            if (!module(ctx, mx, my)) continue;
            for (int dy = 0; dy < layout->scale; dy++) {
                int py = layout->offset + my * layout->scale + dy;
                uint8_t *row = pixels + (size_t)py * stride;
                for (int dx = 0; dx < layout->scale; dx++) {
                    int px = layout->offset + mx * layout->scale + dx;
                    // LVGL I1 位序:字节最高位是同一字节内最左侧像素。
                    row[px >> 3] |= (uint8_t)(0x80 >> (px & 0x07));
                }
            }
        }
    }
    return true;
}
