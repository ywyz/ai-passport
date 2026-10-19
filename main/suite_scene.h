// 三体示意场景:软化引力 + 有限半径弹性碰撞,纯逻辑、无动态分配。
// 显示最多 10 FPS;每帧固定四个 20 ms 模拟步,不做无界补帧。
#pragma once

#include <stdint.h>

#define SUITE_SCENE_SUNS 3
#define SUITE_SCENE_FPS_MS 100
#define SUITE_SCENE_STEP_MS 20
#define SUITE_SCENE_STEPS_PER_FRAME 4
#define SUITE_SCENE_HALF_WIDTH 1.0f
#define SUITE_SCENE_HALF_HEIGHT 0.62f
#define SUITE_SCENE_MAX_SPEED 4.0f
// 约 2.2 px 核心间隙,包含渲染坐标取整误差(88 px/单位)。
#define SUITE_SCENE_CONTACT_GAP 0.025f

typedef struct {
    float x, y;
    float vx, vy;   // 归一化单位 / 模拟秒
    float mass;
    float radius;   // 本体半径;光晕不参加碰撞
    uint32_t color;
} suite_scene_sun_t;

typedef struct {
    uint32_t tick;
    uint32_t reset_count; // 非有限/不可恢复状态的确定性重置次数
    suite_scene_sun_t suns[SUITE_SCENE_SUNS];
} suite_scene_state_t;

void suite_scene_init(suite_scene_state_t *s);

// 前进一个有界帧。隐藏时不调用;恢复用完整 state 快照,不按 tick 重放历史。
void suite_scene_advance(suite_scene_state_t *s);
