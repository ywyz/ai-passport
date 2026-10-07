// main/suite_scene.c —— 三体场景数学实现(纯逻辑)。
// 每颗太阳:椭圆主轨道(不可公度频率的李萨如轨迹,长期不重复)+ 慢速摆动
// + 半径呼吸。全部量按构造严格有界、确定性、帧间连续,无积分无漂移无 NaN。
#include "suite_scene.h"

#include <math.h>

static const uint32_t k_sun_colors[SUITE_SCENE_SUNS] = {
    0xFF5A3C,  // 焰橙(霓虹)
    0x35E0FF,  // 电青(霓虹)
    0xC94AFF,  // 幻紫(霓虹)
};
static const float k_sun_radius[SUITE_SCENE_SUNS] = { 0.10f, 0.08f, 0.07f };

typedef struct {
    float ax, ay;        // 椭圆半轴(归一化)
    float speed;         // 主轨道角速度(rad/tick)
    float ratio;         // y 相位倍率(不可公度,轨迹长期不重复)
    float phase0;        // 初始相位
    float wobble_w;      // 摆动角速度(rad/tick)
    float wobble_amp;    // 摆动幅度(rad)
    float breath_w;      // 半径呼吸角速度
} sun_orbit_t;

static const sun_orbit_t k_orbits[SUITE_SCENE_SUNS] = {
    { 0.86f, 0.52f, 0.0210f, 1.7f, 0.0f, 0.00317f, 0.35f, 0.00213f },
    { 0.64f, 0.78f, 0.0340f, 1.0f, 2.09f, 0.00431f, 0.45f, 0.00171f },
    { 0.93f, 0.38f, 0.0150f, 4.0f, 4.01f, 0.00259f, 0.40f, 0.00259f },
};

static void evaluate_suns(suite_scene_state_t *s, uint32_t tick)
{
    const float t = (float)tick;
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const sun_orbit_t *o = &k_orbits[i];
        const float phase = o->phase0 + o->speed * t;
        const float wobble = o->wobble_amp * sinf(o->wobble_w * t + o->phase0);
        s->suns[i].x = o->ax * cosf(phase + wobble);
        s->suns[i].y = o->ay * sinf(o->ratio * phase + wobble);
        // 呼吸半径在声明界内:base × [0.85,1.15] ⊂ (0,0.2]。
        s->suns[i].radius = k_sun_radius[i] *
                            (1.0f + 0.15f * sinf(o->breath_w * t + (float)i));
    }
}

void suite_scene_init(suite_scene_state_t *s)
{
    s->tick = 0;
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        s->suns[i].radius = k_sun_radius[i];
        s->suns[i].color = k_sun_colors[i];
        s->suns[i].x = 0.0f;
        s->suns[i].y = 0.0f;
    }
    suite_scene_evaluate(s, 0);
}

void suite_scene_evaluate(suite_scene_state_t *s, uint32_t tick)
{
    s->tick = tick;
    evaluate_suns(s, tick);
}

void suite_scene_advance(suite_scene_state_t *s)
{
    suite_scene_evaluate(s, s->tick + 1u);
}
