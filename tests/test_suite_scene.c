// Real scene model: mutual forces, continuous contact, bounds and recovery.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "suite_scene.h"

static void check_scene(const suite_scene_state_t *s)
{
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        const suite_scene_sun_t *a = &s->suns[i];
        assert(isfinite(a->x) && isfinite(a->y));
        assert(isfinite(a->vx) && isfinite(a->vy));
        assert(a->radius > 0.0f && a->mass > 0.0f && a->color != 0);
        assert(fabsf(a->x) + a->radius <= SUITE_SCENE_HALF_WIDTH + 0.00001f);
        assert(fabsf(a->y) + a->radius <= SUITE_SCENE_HALF_HEIGHT + 0.00001f);
        assert(hypotf(a->vx, a->vy) <= SUITE_SCENE_MAX_SPEED + 0.00001f);
        for (int j = i + 1; j < SUITE_SCENE_SUNS; j++) {
            const suite_scene_sun_t *b = &s->suns[j];
            float distance = hypotf(a->x - b->x, a->y - b->y);
            assert(distance >= a->radius + b->radius +
                               SUITE_SCENE_CONTACT_GAP - 0.00011f);
            // Actual LVGL core geometry, including integer coordinate/size rounding.
            int ad = (int)(2.0f * a->radius * 88.0f);
            int bd = (int)(2.0f * b->radius * 88.0f);
            float ax = (int)(120.0f + a->x * 88.0f) + (ad % 2) * 0.5f;
            float ay = (int)(128.0f + a->y * 88.0f) + (ad % 2) * 0.5f;
            float bx = (int)(120.0f + b->x * 88.0f) + (bd % 2) * 0.5f;
            float by = (int)(128.0f + b->y * 88.0f) + (bd % 2) * 0.5f;
            assert(hypotf(ax - bx, ay - by) >= (ad + bd) * 0.5f);
        }
    }
}

static void same_state(const suite_scene_state_t *a, const suite_scene_state_t *b)
{
    assert(a->tick == b->tick && a->reset_count == b->reset_count);
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        assert(a->suns[i].x == b->suns[i].x && a->suns[i].y == b->suns[i].y);
        assert(a->suns[i].vx == b->suns[i].vx && a->suns[i].vy == b->suns[i].vy);
        assert(a->suns[i].mass == b->suns[i].mass);
        assert(a->suns[i].radius == b->suns[i].radius);
        assert(a->suns[i].color == b->suns[i].color);
    }
}

static suite_scene_state_t quiet_scene(void)
{
    suite_scene_state_t s;
    suite_scene_init(&s);
    s.suns[0].x = -0.4f; s.suns[0].y = 0.0f;
    s.suns[1].x = 0.4f; s.suns[1].y = 0.0f;
    s.suns[2].x = 0.0f; s.suns[2].y = 0.5f;
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        s.suns[i].vx = 0.0f;
        s.suns[i].vy = 0.0f;
        s.suns[i].mass = i == 2 ? 0.05f : 1.0f;
        s.suns[i].radius = 0.07f;
    }
    return s;
}

static void test_mutual_force(void)
{
    suite_scene_state_t s = quiet_scene();
    suite_scene_advance(&s);
    check_scene(&s);
    assert(s.suns[0].vx > 0.08f && s.suns[1].vx < -0.08f);
    assert(s.suns[2].vy < 0.0f);
    float px = 0.0f, py = 0.0f;
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        px += s.suns[i].mass * s.suns[i].vx;
        py += s.suns[i].mass * s.suns[i].vy;
    }
    assert(fabsf(px) < 0.00001f && fabsf(py) < 0.00001f);
    suite_scene_state_t heavier = quiet_scene();
    heavier.suns[1].mass = 2.0f;
    suite_scene_advance(&heavier);
    assert(heavier.suns[0].vx > s.suns[0].vx * 1.8f);
}

static void test_contacts(void)
{
    suite_scene_state_t s = quiet_scene();
    s.suns[0].x = -0.13f; s.suns[1].x = 0.13f;
    s.suns[0].vx = 3.0f; s.suns[1].vx = -3.0f;
    suite_scene_advance(&s);
    check_scene(&s);
    assert(s.reset_count == 0);
    assert(s.suns[0].vx < -2.0f && s.suns[1].vx > 2.0f);
    assert(s.suns[0].x < s.suns[1].x);

    // Grazing paths enter and leave the contact disc within ONE 20 ms step.
    // Checking only endpoints misses this collision. The vertical deflection
    // must come from contact, not the much weaker gravitational acceleration.
    s = quiet_scene();
    s.suns[0].x = -0.0395f; s.suns[1].x = 0.0395f;
    s.suns[0].y = -0.0495f; s.suns[1].y = 0.0495f;
    for (int i = 0; i < 2; i++) {
        s.suns[i].radius = 0.05f;
        s.suns[i].mass = 0.05f;
    }
    s.suns[0].vx = 4.0f; s.suns[1].vx = -4.0f;
    suite_scene_advance(&s);
    check_scene(&s);
    assert(s.reset_count == 0);
    assert(s.suns[1].vy - s.suns[0].vy > 2.0f);
    assert(s.suns[1].y - s.suns[0].y > 0.20f);

    // Separating contacts must not be reflected back into each other.
    s = quiet_scene();
    s.suns[0].x = -0.08f; s.suns[1].x = 0.08f;
    s.suns[0].vx = -2.0f; s.suns[1].vx = 2.0f;
    suite_scene_advance(&s);
    check_scene(&s);
    assert(s.suns[0].vx < 0.0f && s.suns[1].vx > 0.0f);

    // All centers coincide: finite, bounded separation instead of divide by zero.
    s = quiet_scene();
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) s.suns[i].x = s.suns[i].y = 0.0f;
    suite_scene_advance(&s);
    check_scene(&s);
}

static void test_close_approach_deflection(void)
{
    suite_scene_state_t s = quiet_scene();
    s.suns[0].x = -0.30f; s.suns[0].y = -0.12f;
    s.suns[1].x = 0.30f; s.suns[1].y = 0.12f;
    s.suns[0].vx = 1.5f; s.suns[1].vx = -1.5f;
    suite_scene_advance(&s);
    assert(s.suns[0].vy > 0.05f && s.suns[1].vy < -0.05f);
    check_scene(&s);
}

static void test_long_run_and_snapshot(void)
{
    suite_scene_state_t s, twin;
    suite_scene_init(&s);
    suite_scene_init(&twin);
    double path[SUITE_SCENE_SUNS] = {0};
    for (uint32_t frame = 1; frame <= 100000u; frame++) {
        suite_scene_state_t before = s;
        suite_scene_advance(&s);
        suite_scene_advance(&twin);
        check_scene(&s);
        same_state(&s, &twin);
        assert(s.tick == frame && s.reset_count == 0);
        if (frame <= 1000u) {
            for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
                path[i] += hypotf(s.suns[i].x - before.suns[i].x,
                                  s.suns[i].y - before.suns[i].y);
            }
        }
    }
    for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
        // Every body moves at least 53 px/s on average at nominal 10 FPS.
        // This catches a regression to the former slow decorative tracks.
        assert(path[i] / 1000.0 > 0.06);
        printf("scene body %d: %.1f px/s mean sampled motion at nominal 10 FPS\n",
               i, path[i] * 88.0 / 100.0);
    }
    // A paused page/saved state needs no unbounded replay from tick zero.
    twin = s;
    for (int i = 0; i < 200; i++) {
        suite_scene_advance(&s);
        suite_scene_advance(&twin);
        same_state(&s, &twin);
    }
    s.tick = UINT32_MAX;
    suite_scene_advance(&s);
    assert(s.tick == 0 && s.reset_count == 0);
    check_scene(&s);
}

static void test_recovery_and_edges(void)
{
    suite_scene_state_t s, initial;
    suite_scene_init(&initial);
    for (int field = 0; field < 5; field++) {
        s = initial;
        if (field == 0) s.suns[0].x = NAN;
        if (field == 1) s.suns[1].vy = INFINITY;
        if (field == 2) s.suns[2].mass = 0.0f;
        if (field == 3) s.suns[0].radius = -0.1f;
        if (field == 4) s.suns[1].x = 1000.0f;
        suite_scene_advance(&s);
        assert(s.tick == 1 && s.reset_count == 1);
        suite_scene_state_t expected = initial;
        expected.tick = 1; expected.reset_count = 1;
        same_state(&s, &expected);
        check_scene(&s);
    }
    // Simultaneous wall/corner contacts, unequal masses and many initial velocities.
    for (int seed = 0; seed < 100; seed++) {
        s = initial;
        for (int i = 0; i < SUITE_SCENE_SUNS; i++) {
            s.suns[i].vx = ((seed * 13 + i * 7) % 81 - 40) * 0.1f;
            s.suns[i].vy = ((seed * 17 + i * 11) % 81 - 40) * 0.1f;
        }
        s.suns[0].x = SUITE_SCENE_HALF_WIDTH - s.suns[0].radius;
        s.suns[0].y = SUITE_SCENE_HALF_HEIGHT - s.suns[0].radius;
        for (int frame = 0; frame < 1000; frame++) {
            suite_scene_advance(&s);
            check_scene(&s);
        }
    }
}

int main(void)
{
    test_mutual_force();
    test_contacts();
    test_close_approach_deflection();
    test_long_run_and_snapshot();
    test_recovery_and_edges();
    puts("three-body force/contact/sweep/stability tests: PASS");
    return 0;
}
