// 三体示意场景。互引力改变速度,扫掠圆碰撞阻止高速穿透;画布边界回弹。
// 不是天文预测:速度、坐标、接触求解工作量均有界,异常时确定性重置。
#include "suite_scene.h"

#include <math.h>
#include <stdbool.h>

#define GRAVITY 0.85f
#define SOFTENING_SQ 0.0144f
#define CONTACT_PASSES 16
#define MAX_EVENTS_PER_STEP 12
#define DIST_EPS 0.000001f

_Static_assert(SUITE_SCENE_STEP_MS <= 20, "SC-07 integration step budget");
_Static_assert(SUITE_SCENE_STEPS_PER_FRAME <= 4, "SC-07 frame work budget");

void suite_scene_init(suite_scene_state_t *s)
{
    *s = (suite_scene_state_t) {
        .suns = {
            { .x = -0.62f, .y = -0.25f, .vx = 1.35f, .vy = -0.50f,
              .mass = 1.0f, .radius = 0.10f, .color = 0xFF5A3C },
            { .x = 0.58f, .y = -0.18f, .vx = -1.15f, .vy = 1.40f,
              .mass = 0.8f, .radius = 0.08f, .color = 0x35E0FF },
            { .x = 0.05f, .y = 0.36f, .vx = -0.65f, .vy = -0.95f,
              .mass = 0.65f, .radius = 0.07f, .color = 0xC94AFF },
        },
    };
}

static bool state_finite(const suite_scene_state_t *s)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const suite_scene_sun_t *a = &s->suns[i];
        if (!isfinite(a->x) || !isfinite(a->y) || !isfinite(a->vx) ||
            !isfinite(a->vy) || !isfinite(a->mass) || !isfinite(a->radius) ||
            fabsf(a->x) > 2.0f || fabsf(a->y) > 2.0f ||
            fabsf(a->vx) > 100.0f || fabsf(a->vy) > 100.0f ||
            a->mass < 0.05f || a->mass > 4.0f ||
            a->radius < 0.05f || a->radius > 0.12f) return false;
    }
    return true;
}

static void cap_speed(suite_scene_sun_t *a)
{
    float v2 = a->vx * a->vx + a->vy * a->vy;
    if (v2 > SUITE_SCENE_MAX_SPEED * SUITE_SCENE_MAX_SPEED) {
        float scale = SUITE_SCENE_MAX_SPEED / sqrtf(v2);
        a->vx *= scale;
        a->vy *= scale;
    }
}

static void confine(suite_scene_sun_t *a)
{
    float xlim = SUITE_SCENE_HALF_WIDTH - a->radius;
    float ylim = SUITE_SCENE_HALF_HEIGHT - a->radius;
    if (a->x < -xlim) { a->x = -xlim; a->vx = fabsf(a->vx); }
    if (a->x > xlim) { a->x = xlim; a->vx = -fabsf(a->vx); }
    if (a->y < -ylim) { a->y = -ylim; a->vy = fabsf(a->vy); }
    if (a->y > ylim) { a->y = ylim; a->vy = -fabsf(a->vy); }
}

static float contact_radius(const suite_scene_sun_t *a,
                            const suite_scene_sun_t *b)
{
    return a->radius + b->radius + SUITE_SCENE_CONTACT_GAP;
}

static void contact(suite_scene_sun_t *a, suite_scene_sun_t *b)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    float distance = sqrtf(dx * dx + dy * dy);
    // 完全重合也有确定性法线,不除以零。
    float nx = distance > DIST_EPS ? dx / distance : 1.0f;
    float ny = distance > DIST_EPS ? dy / distance : 0.0f;
    float ia = 1.0f / a->mass, ib = 1.0f / b->mass;
    float penetration = contact_radius(a, b) - distance;
    if (penetration > 0.0f) {
        float correction = (penetration + DIST_EPS) / (ia + ib);
        a->x -= nx * correction * ia;
        a->y -= ny * correction * ia;
        b->x += nx * correction * ib;
        b->y += ny * correction * ib;
    }
    float approach = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;
    // 已经分离的接触不能再次反向而粘在一起。
    if (approach < 0.0f) {
        float impulse = -2.0f * approach / (ia + ib);
        a->vx -= impulse * ia * nx;
        a->vy -= impulse * ia * ny;
        b->vx += impulse * ib * nx;
        b->vy += impulse * ib * ny;
        cap_speed(a);
        cap_speed(b);
    }
}

static bool separated(const suite_scene_state_t *s)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        for (int j = i + 1; j < SUITE_SCENE_SUNS; j++) {
            const suite_scene_sun_t *a = &s->suns[i], *b = &s->suns[j];
            float dx = b->x - a->x, dy = b->y - a->y;
            float r = contact_radius(a, b) - 0.0001f;
            if (dx * dx + dy * dy < r * r) return false;
        }
    }
    return true;
}

// 投影只修正数值误差/退化输入;正常运动用连续碰撞时间求解。
static bool settle_contacts(suite_scene_state_t *s)
{
    for (int pass = 0; pass < CONTACT_PASSES; pass++) {
        for (int i = 0; i < SUITE_SCENE_SUNS; i++) confine(&s->suns[i]);
        if (separated(s)) return true;
        for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
            for (int j = i + 1; j < SUITE_SCENE_SUNS; j++) {
                suite_scene_sun_t *a = &s->suns[i], *b = &s->suns[j];
                float dx = b->x - a->x, dy = b->y - a->y;
                float r = contact_radius(a, b);
                if (dx * dx + dy * dy < r * r) contact(a, b);
            }
        }
    }
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) confine(&s->suns[i]);
    return separated(s);
}

static void gravity_kick(suite_scene_state_t *s, float dt)
{
    float ax[SUITE_SCENE_SUNS] = {0}, ay[SUITE_SCENE_SUNS] = {0};
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        for (int j = i + 1; j < SUITE_SCENE_SUNS; j++) {
            const suite_scene_sun_t *a = &s->suns[i], *b = &s->suns[j];
            float dx = b->x - a->x, dy = b->y - a->y;
            float r2 = dx * dx + dy * dy + SOFTENING_SQ;
            float force = GRAVITY / (r2 * sqrtf(r2));
            ax[i] += force * b->mass * dx;
            ay[i] += force * b->mass * dy;
            ax[j] -= force * a->mass * dx;
            ay[j] -= force * a->mass * dy;
        }
    }
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        s->suns[i].vx += ax[i] * dt;
        s->suns[i].vy += ay[i] * dt;
        cap_speed(&s->suns[i]);
    }
}

typedef struct {
    float time;
    int a, b; // b=-1:画布边界;否则为两太阳接触
    int axis;
} hit_t;

static hit_t next_hit(const suite_scene_state_t *s, float remaining)
{
    hit_t hit = { .time = remaining, .a = -1, .b = -1, .axis = 0 };
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const suite_scene_sun_t *a = &s->suns[i];
        float pos[2] = {a->x, a->y}, vel[2] = {a->vx, a->vy};
        float lim[2] = {SUITE_SCENE_HALF_WIDTH - a->radius,
                        SUITE_SCENE_HALF_HEIGHT - a->radius};
        for (int axis = 0; axis < 2; axis++) {
            if (fabsf(vel[axis]) < DIST_EPS) continue;
            float edge = vel[axis] > 0.0f ? lim[axis] : -lim[axis];
            float t = fmaxf(0.0f, (edge - pos[axis]) / vel[axis]);
            if (t <= hit.time) hit = (hit_t){t, i, -1, axis};
        }
        for (int j = i + 1; j < SUITE_SCENE_SUNS; j++) {
            const suite_scene_sun_t *b = &s->suns[j];
            float dx = b->x - a->x, dy = b->y - a->y;
            float vx = b->vx - a->vx, vy = b->vy - a->vy;
            float approach = dx * vx + dy * vy;
            if (approach >= 0.0f) continue;
            float v2 = vx * vx + vy * vy;
            float r = contact_radius(a, b);
            float c = dx * dx + dy * dy - r * r;
            float discriminant = approach * approach - v2 * c;
            if (discriminant < 0.0f) continue;
            // 稳定二次方程根:避免接近接触时两大数相减。
            float t = c <= 0.0f ? 0.0f :
                      c / (-approach + sqrtf(discriminant));
            if (t <= hit.time) hit = (hit_t){t, i, j, 0};
        }
    }
    return hit;
}

static void drift(suite_scene_state_t *s, float dt)
{
    float remaining = dt;
    for (int event = 0; event < MAX_EVENTS_PER_STEP && remaining > 0.0f; event++) {
        hit_t hit = next_hit(s, remaining);
        for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
            s->suns[i].x += s->suns[i].vx * hit.time;
            s->suns[i].y += s->suns[i].vy * hit.time;
        }
        remaining -= hit.time;
        if (hit.a < 0) break;
        if (hit.b >= 0) {
            contact(&s->suns[hit.a], &s->suns[hit.b]);
        } else if (hit.axis == 0) {
            s->suns[hit.a].vx = -s->suns[hit.a].vx;
        } else {
            s->suns[hit.a].vy = -s->suns[hit.a].vy;
        }
    }
    // 达到事件上限时丢弃剩余模拟时间,绝不未经检测地移动而穿透。
}

static void recover(suite_scene_state_t *s, uint32_t next_tick)
{
    uint32_t count = s->reset_count + 1u;
    suite_scene_init(s);
    s->tick = next_tick;
    s->reset_count = count;
}

void suite_scene_advance(suite_scene_state_t *s)
{
    uint32_t next_tick = s->tick + 1u;
    if (!state_finite(s)) {
        recover(s, next_tick);
        return;
    }
    for (int step = 0; step < SUITE_SCENE_STEPS_PER_FRAME; step++) {
        if (!settle_contacts(s)) {
            recover(s, next_tick);
            return;
        }
        // 半步 kick / 扫掠 drift / 半步 kick,避免大时间步显式积分的发散。
        float dt = SUITE_SCENE_STEP_MS / 1000.0f;
        gravity_kick(s, dt * 0.5f);
        drift(s, dt);
        if (!settle_contacts(s)) {
            recover(s, next_tick);
            return;
        }
        gravity_kick(s, dt * 0.5f);
    }
    s->tick = next_tick;
    if (!state_finite(s)) recover(s, next_tick);
}
