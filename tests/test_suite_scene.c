// tests/test_suite_scene.c —— 三体场景数学主机测试。
// 验收要点(ST-03):有界、确定性、无 NaN、帧间连续(不跳变)、可恢复。
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "suite_scene.h"

static bool in_bounds(const suite_scene_sun_t *s) {
    return s->x >= -1.0f && s->x <= 1.0f &&
           s->y >= -1.0f && s->y <= 1.0f &&
           s->radius > 0.0f && s->radius <= 0.2f &&
           isfinite(s->x) && isfinite(s->y) && isfinite(s->radius);
}

int main(void) {
    suite_scene_state_t s;
    suite_scene_init(&s);
    assert(s.tick == 0);
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        assert(in_bounds(&s.suns[i]));
        assert(s.suns[i].color != 0);  // 每颗太阳有颜色
    }

    // 推进 100,000 帧(约 2.8 小时 @10FPS):始终有界、有限、帧间连续。
    suite_scene_state_t prev = s;
    for (uint32_t f = 1; f <= 100000u; f++) {
        suite_scene_advance(&s);
        assert(s.tick == f);
        for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
            const suite_scene_sun_t *sun = &s.suns[i];
            assert(in_bounds(sun));
            const suite_scene_sun_t *p = &prev.suns[i];
            // 帧间位移上限:保证视觉连续,不允许"瞬移"。
            float dx = sun->x - p->x, dy = sun->y - p->y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            assert(dx < 0.05f && dy < 0.05f);
            float dr = sun->radius - p->radius;
            if (dr < 0) dr = -dr;
            assert(dr < 0.01f);
        }
        prev = s;
    }

    // 确定性:同一 tick 两次求值结果逐位一致(先 init 固定 color 字段)。
    suite_scene_state_t a, b;
    suite_scene_init(&a);
    suite_scene_init(&b);
    suite_scene_evaluate(&a, 77777u);
    suite_scene_evaluate(&b, 77777u);
    assert(memcmp(&a, &b, sizeof(a)) == 0);

    // 可恢复:渲染层按 tick 跳帧后与连续推进结果一致。
    suite_scene_state_t run;
    suite_scene_init(&run);
    for (uint32_t f = 0; f < 5000u; f++) suite_scene_advance(&run);
    suite_scene_state_t jump;
    suite_scene_init(&jump);
    suite_scene_evaluate(&jump, run.tick);
    assert(memcmp(&run, &jump, sizeof(run)) == 0);

    // 轨迹长期不重合:100k 帧内三颗太阳从未同时几乎重叠(三体无共线稳定解)。
    suite_scene_state_t t;
    suite_scene_init(&t);
    for (uint32_t f = 0; f < 100000u; f++) {
        suite_scene_advance(&t);
        float d01x = t.suns[0].x - t.suns[1].x, d01y = t.suns[0].y - t.suns[1].y;
        float d02x = t.suns[0].x - t.suns[2].x, d02y = t.suns[0].y - t.suns[2].y;
        float d12x = t.suns[1].x - t.suns[2].x, d12y = t.suns[1].y - t.suns[2].y;
        bool c01 = fabsf(d01x) < 0.01f && fabsf(d01y) < 0.01f;
        bool c02 = fabsf(d02x) < 0.01f && fabsf(d02y) < 0.01f;
        bool c12 = fabsf(d12x) < 0.01f && fabsf(d12y) < 0.01f;
        assert(!(c01 && c02) && !(c01 && c12) && !(c02 && c12));
    }

    return 0;
}
