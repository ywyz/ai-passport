// main/suite_scene.h —— 三体首页场景数学(纯逻辑,可主机测试)。
// 三颗"太阳"以确定性参数轨道运行,叠加 logistic 混沌调制制造三体不适感。
// 所有量按构造有界(归一化 [-1,1]),不依赖物理积分,长期运行无漂移/无 NaN,
// 满足 ST-03"程序场景有界、帧率上限 10 FPS"要求;渲染层按 tick 取位置。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SUITE_SCENE_SUNS 3
#define SUITE_SCENE_FPS_MS 100  // 10 FPS 上限

typedef struct {
    float x;        // 归一化横坐标 [-1,1]
    float y;        // 归一化纵坐标 [-1,1]
    float radius;   // 归一化半径 (0,0.2]
    uint32_t color; // 0xRRGGBB
} suite_scene_sun_t;

typedef struct {
    uint32_t tick;  // 帧序号,单调递增
    suite_scene_sun_t suns[SUITE_SCENE_SUNS];
} suite_scene_state_t;

void suite_scene_init(suite_scene_state_t *s);

// 前进一帧:tick+1 并重算三体位置。
void suite_scene_advance(suite_scene_state_t *s);

// 直接按给定 tick 重算位置(渲染层恢复/跳帧用,不推进 tick)。
void suite_scene_evaluate(suite_scene_state_t *s, uint32_t tick);
