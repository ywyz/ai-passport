// tests/test_suite_provision_qr.c —— 配网页二维码布局与 I1 绘制主机测试。
//
// 覆盖:整数缩放/居中/静区计算、调色板保留、像素区清零范围、位序、
// 容量与 stride 防护、各真实二维码版本在 150px 画布内的绘制边界。
// 屏幕可见性与手机扫描性属于真机验收,不在本测试范围内。
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "suite_provision_qr.h"

#define CANVAS 150
#define STRIDE 19  // 150 bit = 19 字节(LV_DRAW_BUF_STRIDE_ALIGN=1)

typedef struct {
    int mods;
    const char *const *rows;
} matrix_t;

static bool matrix_module(const void *ctx, int x, int y)
{
    const matrix_t *m = ctx;
    return m->rows[y][x] == '#';
}

static bool black_module(const void *ctx, int x, int y)
{
    (void)ctx;
    (void)x;
    (void)y;
    return true;
}

// 读取 I1 像素:字节最高位是同一字节内最左侧像素(与 LVGL 一致)。
static int px_get(const uint8_t *pixels, size_t stride, int x, int y)
{
    return (pixels[(size_t)y * stride + (size_t)(x >> 3)] >>
            (7 - (x & 7))) & 1;
}

static const char *const k_ring_rows[] = {
    "#######",
    "#.....#",
    "#.###.#",
    "#.#.#.#",
    "#.###.#",
    "#.....#",
    "#######",
};

static void test_layout_math(void)
{
    static const int sizes[] = {21, 25, 29, 33, 37, 41, 45, 49, 53, 57};
    suite_qr_layout_t l;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        assert(suite_qr_layout_calc(sizes[i], CANVAS, &l));
        assert(l.modules == sizes[i] && l.canvas == CANVAS);
        assert(l.scale >= 1 && l.drawn == sizes[i] * l.scale);
        assert(l.drawn <= CANVAS);
        // 居中:左右留白差不超过 1 像素;静区至少 4 个模块。
        assert(CANVAS - l.drawn - 2 * l.offset <= 1);
        assert(l.offset >= SUITE_QR_QUIET_MODULES * l.scale);
        assert(l.offset + l.drawn <= CANVAS);
    }
    // 具体版本的缩放/静区(必须是整数缩放,不留半个模块)。
    assert(suite_qr_layout_calc(21, CANVAS, &l));
    assert(l.scale == 5 && l.offset == 22 && l.drawn == 105);
    assert(suite_qr_layout_calc(45, CANVAS, &l));
    assert(l.scale == 2 && l.offset == 30 && l.drawn == 90);
    // 非法参数与放不下的模块数。
    assert(!suite_qr_layout_calc(0, CANVAS, &l));
    assert(!suite_qr_layout_calc(21, 0, &l));
    assert(!suite_qr_layout_calc(21, CANVAS, NULL));
    assert(!suite_qr_layout_calc(151, CANVAS, &l));
    assert(!suite_qr_layout_calc(25, 24, &l));  // 单模块缩放仍放不下
}

static void test_paint_matrix_and_palette(void)
{
    matrix_t m = { .mods = 7, .rows = k_ring_rows };
    suite_qr_layout_t l;
    assert(suite_qr_layout_calc(m.mods, 33, &l));
    assert(l.scale == 2 && l.offset == 9 && l.drawn == 14);

    enum { PAL = SUITE_QR_I1_PALETTE_BYTES, CAP = 33 * 5 + 16 };
    uint8_t buf[PAL + CAP];
    memset(buf, 0x5A, sizeof(buf));  // 调色板与哨兵填充可识别字节
    uint8_t *pixels = buf + PAL;

    assert(suite_qr_paint_i1(pixels, CAP, 5, &l, matrix_module, &m));

    // 调色板(2 项 × 4 字节)与像素区之后的哨兵必须原样保留。
    for (size_t i = 0; i < PAL; i++) assert(buf[i] == 0x5A);
    for (size_t i = PAL + 33u * 5u; i < sizeof(buf); i++)
        assert(buf[i] == 0x5A);

    // 静区全白;模块块按矩阵放大,位序逐像素正确。
    for (int y = 0; y < 33; y++) {
        for (int x = 0; x < 33; x++) {
            bool inside = x >= l.offset && x < l.offset + l.drawn &&
                          y >= l.offset && y < l.offset + l.drawn;
            int expect = 0;
            if (inside) {
                int mx = (x - l.offset) / l.scale;
                int my = (y - l.offset) / l.scale;
                expect = m.rows[my][mx] == '#';
            }
            assert(px_get(pixels, 5, x, y) == expect);
        }
    }
}

static void test_paint_guards(void)
{
    enum { PAL = SUITE_QR_I1_PALETTE_BYTES, CAP = 4 * 28 + 32 };
    uint8_t buf[PAL + CAP];
    memset(buf, 0xA5, sizeof(buf));
    uint8_t *pixels = buf + PAL;

    suite_qr_layout_t l;
    assert(suite_qr_layout_calc(6, 28, &l));  // scale=2, drawn=12, offset=8
    assert(l.scale == 2 && l.offset == 8 && l.drawn == 12);

    assert(!suite_qr_paint_i1(NULL, CAP, 4, &l, black_module, NULL));
    assert(!suite_qr_paint_i1(pixels, CAP, 4, NULL, black_module, NULL));
    assert(!suite_qr_paint_i1(pixels, CAP, 4, &l, NULL, NULL));
    // 28px 一行需 4 字节;stride=3 必须拒绝。
    assert(!suite_qr_paint_i1(pixels, CAP, 3, &l, black_module, NULL));
    // 像素区容量少 1 字节必须拒绝。
    assert(!suite_qr_paint_i1(pixels, 4 * 28 - 1, 4, &l, black_module, NULL));
    // 布局自检:drawn 与 modules*scale 不一致必须拒绝。
    suite_qr_layout_t bad = l;
    bad.drawn += 1;
    assert(!suite_qr_paint_i1(pixels, CAP, 4, &bad, black_module, NULL));

    // 所有失败调用都不得写缓冲(含调色板区)。
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0xA5);
}

static void test_full_paint_bounds(void)
{
    // 全部真实二维码版本(21..57 模块):全黑绘制后逐像素核对内容与静区,
    // 且不触碰调色板和像素区尾部哨兵。
    static const int sizes[] = {21, 25, 29, 33, 37, 41, 45, 49, 53, 57};
    enum { PAL = SUITE_QR_I1_PALETTE_BYTES, PIXELS = STRIDE * CANVAS,
           SENTINEL = 16 };
    uint8_t buf[PAL + PIXELS + SENTINEL];
    suite_qr_layout_t l;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(buf, 0xC3, sizeof(buf));
        assert(suite_qr_layout_calc(sizes[i], CANVAS, &l));
        uint8_t *pixels = buf + PAL;
        assert(suite_qr_paint_i1(pixels, PIXELS, STRIDE, &l, black_module,
                                 NULL));
        for (size_t j = 0; j < PAL; j++) assert(buf[j] == 0xC3);
        for (size_t j = PAL + PIXELS; j < sizeof(buf); j++)
            assert(buf[j] == 0xC3);
        for (int y = 0; y < CANVAS; y++) {
            for (int x = 0; x < CANVAS; x++) {
                bool content = x >= l.offset &&
                               x < l.offset + l.drawn &&
                               y >= l.offset &&
                               y < l.offset + l.drawn;
                assert(px_get(pixels, STRIDE, x, y) == (content ? 1 : 0));
            }
        }
    }
}

int main(void)
{
    test_layout_math();
    test_paint_matrix_and_palette();
    test_paint_guards();
    test_full_paint_bounds();
    puts("provision QR layout/paint tests: PASS");
    return 0;
}
