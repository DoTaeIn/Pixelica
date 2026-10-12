/* Camera helpers: dt-independent follow, 2D centering/bounds/zoom, world<->screen,
 * checked look-at, deterministic shake, transactional failures.
 * Common-library test: links Pixelica::common only and includes no 3D header. */
#include <float.h>

#include "pixelica/camera.h"
#include "test_util.h"

#define EPS 1e-4

static PIXELICA_Vec2 v2(float x, float y)
{
    PIXELICA_Vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_Camera2D cam(float x, float y, float zoom)
{
    PIXELICA_Camera2D c;
    c.position = v2(x, y);
    c.zoom = zoom;
    return c;
}

static PIXELICA_CameraBounds2D rect(float x0, float y0, float x1, float y1)
{
    PIXELICA_CameraBounds2D b;
    b.min = v2(x0, y0);
    b.max = v2(x1, y1);
    return b;
}

static void test_follow_vec3(void)
{
    PIXELICA_Vec3 p = v3(-10.0f, 4.0f, 0.0f);
    PIXELICA_Vec3 q;
    PIXELICA_Vec3 before;
    PIXELICA_Vec3 target = v3(10.0f, -4.0f, -8.0f);
    int i;

    /* One half-life covers half of the distance. */
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.5f, 0.5f) == PIXELICA_OK);
    CHECK_NEAR(p.x, 0.0, EPS);
    CHECK_NEAR(p.y, 0.0, EPS);
    CHECK_NEAR(p.z, -4.0, EPS);

    /* dt independence: 8 x 0.125 s == 1 x 1 s. */
    p = v3(-10.0f, 4.0f, 0.0f);
    q = p;
    for (i = 0; i < 8; ++i) {
        CHECK(pixelica_camera_follow_vec3(&p, target, 0.3f, 0.125f) == PIXELICA_OK);
    }
    CHECK(pixelica_camera_follow_vec3(&q, target, 0.3f, 1.0f) == PIXELICA_OK);
    CHECK_NEAR(p.x, q.x, EPS);
    CHECK_NEAR(p.y, q.y, EPS);
    CHECK_NEAR(p.z, q.z, EPS);
    CHECK(p.x < target.x && p.x > -10.0f); /* never overshoots */

    /* dt 0 keeps, half_life 0 snaps, huge dt arrives exactly. */
    before = p;
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.3f, 0.0f) == PIXELICA_OK);
    CHECK(memcmp(&p, &before, sizeof p) == 0);
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.0f, 0.0f) == PIXELICA_OK);
    CHECK(p.x == target.x && p.y == target.y && p.z == target.z);
    p = v3(-FLT_MAX, FLT_MAX, 0.0f);
    target = v3(FLT_MAX, -FLT_MAX, 1.0f);
    CHECK(pixelica_camera_follow_vec3(&p, target, 1e-30f, FLT_MAX) == PIXELICA_OK);
    CHECK(p.x == FLT_MAX && p.y == -FLT_MAX && p.z == 1.0f);

    /* Opposite extremes do not overflow mid-way. */
    p = v3(-FLT_MAX, 0.0f, 0.0f);
    CHECK(pixelica_camera_follow_vec3(&p, v3(FLT_MAX, 0.0f, 0.0f), 1.0f, 1.0f) ==
          PIXELICA_OK);
    CHECK_NEAR(p.x, 0.0, 1e30);

    /* Failures leave the position untouched. */
    before = p;
    CHECK(pixelica_camera_follow_vec3(NULL, target, 0.1f, 0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_follow_vec3(&p, v3(pixelica_t_nan(), 0.0f, 0.0f), 0.1f, 0.1f) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_follow_vec3(&p, target, -0.1f, 0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.1f, -0.1f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_follow_vec3(&p, target, pixelica_t_inf(), 0.1f) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.1f, pixelica_t_inf()) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&p, &before, sizeof p) == 0);
    p.y = pixelica_t_inf();
    CHECK(pixelica_camera_follow_vec3(&p, target, 0.1f, 0.1f) == PIXELICA_ERR_INVALID_ARG);
}

static void test_follow2d_centering(void)
{
    PIXELICA_Camera2D c = cam(0.0f, 0.0f, 2.0f);
    PIXELICA_Camera2D d;
    PIXELICA_Vec2 vp = v2(160.0f, 120.0f);
    PIXELICA_Vec2 s;
    int i;

    /* Snap: position is the viewport origin, target lands at the screen center. */
    CHECK(pixelica_camera2d_follow(&c, vp, v2(-100.0f, -50.0f), 0.0f, 0.016f, NULL) ==
          PIXELICA_OK);
    CHECK_NEAR(c.position.x, -140.0, EPS);
    CHECK_NEAR(c.position.y, -80.0, EPS);
    CHECK(c.zoom == 2.0f);
    CHECK(pixelica_camera2d_world_to_screen(&c, v2(-100.0f, -50.0f), &s) == PIXELICA_OK);
    CHECK_NEAR(s.x, 80.0, EPS);
    CHECK_NEAR(s.y, 60.0, EPS);

    /* Smoothed, split dt == whole dt. */
    c = cam(0.0f, 0.0f, 1.0f);
    d = c;
    for (i = 0; i < 4; ++i) {
        CHECK(pixelica_camera2d_follow(&c, vp, v2(200.0f, -300.0f), 0.2f, 0.05f, NULL) ==
              PIXELICA_OK);
    }
    CHECK(pixelica_camera2d_follow(&d, vp, v2(200.0f, -300.0f), 0.2f, 0.2f, NULL) ==
          PIXELICA_OK);
    CHECK_NEAR(c.position.x, d.position.x, EPS);
    CHECK_NEAR(c.position.y, d.position.y, EPS);
    /* One half-life: halfway from 0 to the goal (120, -360). */
    CHECK_NEAR(d.position.x, 60.0, EPS);
    CHECK_NEAR(d.position.y, -180.0, EPS);
}

static void test_follow2d_bounds(void)
{
    PIXELICA_CameraBounds2D map = rect(0.0f, 0.0f, 320.0f, 240.0f);
    PIXELICA_CameraBounds2D neg = rect(-500.0f, -400.0f, -100.0f, -100.0f);
    PIXELICA_Vec2 vp = v2(160.0f, 120.0f);
    PIXELICA_Camera2D c = cam(50.0f, 50.0f, 1.0f);
    int i;

    CHECK(pixelica_camera2d_follow(&c, vp, v2(10.0f, 10.0f), 0.0f, 0.0f, &map) == PIXELICA_OK);
    CHECK(c.position.x == 0.0f && c.position.y == 0.0f);
    CHECK(pixelica_camera2d_follow(&c, vp, v2(310.0f, 230.0f), 0.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == 160.0f && c.position.y == 120.0f);
    CHECK(pixelica_camera2d_follow(&c, vp, v2(160.0f, 100.0f), 0.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == 80.0f && c.position.y == 40.0f); /* interior: plain centering */

    /* Zoom aware: zoom 2 halves the visible size (80 x 60). */
    c.zoom = 2.0f;
    CHECK(pixelica_camera2d_follow(&c, vp, v2(1000.0f, -1000.0f), 0.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == 240.0f && c.position.y == 0.0f);

    /* Visible area equal to / larger than the bounds: centered on the bounds. */
    c.zoom = 0.5f;
    CHECK(pixelica_camera2d_follow(&c, vp, v2(-77.0f, 999.0f), 0.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == 0.0f && c.position.y == 0.0f);
    c.zoom = 0.25f;
    CHECK(pixelica_camera2d_follow(&c, vp, v2(-77.0f, 999.0f), 0.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == -160.0f && c.position.y == -120.0f);

    /* Negative-coordinate bounds while smoothing: always inside, converges. */
    c = cam(-300.0f, -250.0f, 1.0f);
    for (i = 0; i < 200; ++i) {
        CHECK(pixelica_camera2d_follow(&c, vp, v2(-1000.0f, 1000.0f), 0.1f, 0.016f, &neg) ==
              PIXELICA_OK);
        CHECK(c.position.x >= -500.0f && c.position.x <= -260.0f);
        CHECK(c.position.y >= -400.0f && c.position.y <= -220.0f);
    }
    CHECK_NEAR(c.position.x, -500.0, 1e-2);
    CHECK_NEAR(c.position.y, -220.0, 1e-2);

    /* A position outside the bounds (e.g. after zooming out) is clamped even with dt 0. */
    c = cam(300.0f, 300.0f, 1.0f);
    CHECK(pixelica_camera2d_follow(&c, vp, v2(300.0f, 300.0f), 1.0f, 0.0f, &map) ==
          PIXELICA_OK);
    CHECK(c.position.x == 160.0f && c.position.y == 120.0f);

    /* Degenerate (zero-area) bounds are allowed: the view centers on them. */
    map = rect(10.0f, 20.0f, 10.0f, 20.0f);
    CHECK(pixelica_camera2d_follow(&c, vp, v2(0.0f, 0.0f), 0.0f, 0.0f, &map) == PIXELICA_OK);
    CHECK(c.position.x == -70.0f && c.position.y == -40.0f);
}

static void test_follow2d_invalid(void)
{
    PIXELICA_CameraBounds2D map = rect(0.0f, 0.0f, 320.0f, 240.0f);
    PIXELICA_CameraBounds2D bad = rect(10.0f, 0.0f, 0.0f, 240.0f);
    PIXELICA_Vec2 vp = v2(160.0f, 120.0f);
    PIXELICA_Vec2 t = v2(1.0f, 2.0f);
    PIXELICA_Camera2D c = cam(5.0f, 6.0f, 1.0f);
    PIXELICA_Camera2D before = c;
    PIXELICA_Camera2D z;

    CHECK(pixelica_camera2d_follow(NULL, vp, t, 0.1f, 0.1f, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_follow(&c, v2(0.0f, 120.0f), t, 0.1f, 0.1f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_follow(&c, v2(160.0f, -1.0f), t, 0.1f, 0.1f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_follow(&c, vp, v2(pixelica_t_nan(), 0.0f), 0.1f, 0.1f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_follow(&c, vp, t, 0.1f, -1.0f, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_follow(&c, vp, t, 0.1f, 0.1f, &bad) == PIXELICA_ERR_INVALID_ARG);
    bad = map;
    bad.max.y = pixelica_t_inf();
    CHECK(pixelica_camera2d_follow(&c, vp, t, 0.1f, 0.1f, &bad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&c, &before, sizeof c) == 0);

    z = c;
    z.zoom = 0.0f;
    CHECK(pixelica_camera2d_follow(&z, vp, t, 0.1f, 0.1f, NULL) == PIXELICA_ERR_INVALID_ARG);
    z.zoom = -1.0f;
    CHECK(pixelica_camera2d_follow(&z, vp, t, 0.1f, 0.1f, NULL) == PIXELICA_ERR_INVALID_ARG);
    z.zoom = pixelica_t_nan();
    CHECK(pixelica_camera2d_follow(&z, vp, t, 0.1f, 0.1f, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* Tiny zoom: the visible size overflows float -> rejected, untouched. */
    z = cam(0.0f, 0.0f, 1e-38f);
    before = z;
    CHECK(pixelica_camera2d_follow(&z, v2(FLT_MAX, FLT_MAX), t, 0.0f, 0.0f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&z, &before, sizeof z) == 0);
}

static void test_conversions(void)
{
    PIXELICA_Camera2D c = cam(-37.5f, 12.25f, 3.0f);
    PIXELICA_Camera2D z;
    PIXELICA_Vec2 s, w;
    PIXELICA_Vec2 sentinel = v2(123.0f, 456.0f);

    CHECK(pixelica_camera2d_world_to_screen(&c, v2(-40.0f, 10.0f), &s) == PIXELICA_OK);
    CHECK_NEAR(s.x, -7.5, EPS); /* left of the screen: negative is fine */
    CHECK_NEAR(s.y, -6.75, EPS);
    CHECK(pixelica_camera2d_screen_to_world(&c, s, &w) == PIXELICA_OK);
    CHECK_NEAR(w.x, -40.0, EPS);
    CHECK_NEAR(w.y, 10.0, EPS);

    /* Screen (0,0) is camera.position; screen center is position + viewport/(2 zoom). */
    CHECK(pixelica_camera2d_screen_to_world(&c, v2(0.0f, 0.0f), &w) == PIXELICA_OK);
    CHECK(w.x == c.position.x && w.y == c.position.y);
    CHECK(pixelica_camera2d_screen_to_world(&c, v2(150.0f, 90.0f), &w) == PIXELICA_OK);
    CHECK_NEAR(w.x, -37.5 + 50.0, EPS);
    CHECK_NEAR(w.y, 12.25 + 30.0, EPS);

    /* Overflow and invalid input: rejected, output untouched. */
    s = sentinel;
    c = cam(-FLT_MAX, 0.0f, 2.0f);
    CHECK(pixelica_camera2d_world_to_screen(&c, v2(FLT_MAX, 0.0f), &s) ==
          PIXELICA_ERR_INVALID_ARG);
    z = cam(FLT_MAX, 0.0f, 0.5f);
    CHECK(pixelica_camera2d_screen_to_world(&z, v2(FLT_MAX, 0.0f), &s) ==
          PIXELICA_ERR_INVALID_ARG);
    z.zoom = 0.0f;
    CHECK(pixelica_camera2d_world_to_screen(&z, v2(0.0f, 0.0f), &s) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_screen_to_world(NULL, v2(0.0f, 0.0f), &s) ==
          PIXELICA_ERR_INVALID_ARG);
    c = cam(0.0f, 0.0f, 1.0f);
    CHECK(pixelica_camera2d_world_to_screen(&c, v2(0.0f, pixelica_t_inf()), &s) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera2d_world_to_screen(&c, v2(0.0f, 0.0f), NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&s, &sentinel, sizeof s) == 0);
}

static void xform(const float m[16], PIXELICA_Vec3 p, float out[4])
{
    pixelica_mat4_transform_point(out, m, p);
}

static void test_look_at(void)
{
    float m[16];
    float sentinel[16];
    float r[4];
    PIXELICA_Vec3 eye = v3(3.0f, 4.0f, -2.0f);
    PIXELICA_Vec3 target = v3(-1.0f, 2.0f, 6.0f);
    PIXELICA_Vec3 up = v3(0.0f, 1.0f, 0.0f);
    double dist = sqrt(16.0 + 4.0 + 64.0);
    int i;

    /* Canonical: eye on +Z looking at the origin is a pure -5 z translation. */
    CHECK(pixelica_camera_look_at(m, v3(0.0f, 0.0f, 5.0f), v3(0.0f, 0.0f, 0.0f), up) ==
          PIXELICA_OK);
    for (i = 0; i < 16; ++i) {
        double expect = (i % 5 == 0) ? 1.0 : 0.0;
        if (i == 14) {
            expect = -5.0;
        }
        CHECK_NEAR(m[i], expect, 1e-6);
    }

    /* General: eye -> origin, target -> (0,0,-dist), up stays in +Y (x = 0). */
    CHECK(pixelica_camera_look_at(m, eye, target, up) == PIXELICA_OK);
    CHECK(pixelica_mat4_is_finite(m) == 1);
    xform(m, eye, r);
    CHECK_NEAR(r[0], 0.0, EPS);
    CHECK_NEAR(r[1], 0.0, EPS);
    CHECK_NEAR(r[2], 0.0, EPS);
    CHECK_NEAR(r[3], 1.0, 0.0);
    xform(m, target, r);
    CHECK_NEAR(r[0], 0.0, EPS);
    CHECK_NEAR(r[1], 0.0, EPS);
    CHECK_NEAR(r[2], -dist, EPS);
    xform(m, v3(eye.x, eye.y + 1.0f, eye.z), r);
    CHECK_NEAR(r[0], 0.0, EPS);
    CHECK(r[1] > 0.0f);
    /* Rotation part is orthonormal: column lengths 1, w row (0,0,0,1). */
    for (i = 0; i < 3; ++i) {
        double len = (double)m[i * 4] * m[i * 4] + (double)m[i * 4 + 1] * m[i * 4 + 1] +
                     (double)m[i * 4 + 2] * m[i * 4 + 2];
        CHECK_NEAR(len, 1.0, EPS);
        CHECK(m[i * 4 + 3] == 0.0f);
    }
    CHECK(m[15] == 1.0f);
    /* A non-unit, tilted up gives the same matrix as its normalized form... */
    {
        float m2[16];
        CHECK(pixelica_camera_look_at(m2, eye, target, v3(0.0f, 7.0f, 0.0f)) == PIXELICA_OK);
        for (i = 0; i < 16; ++i) {
            CHECK_NEAR(m2[i], m[i], 1e-6);
        }
    }

    /* Degenerate / invalid: rejected, matrix untouched. */
    for (i = 0; i < 16; ++i) {
        sentinel[i] = 42.0f + (float)i;
    }
    memcpy(m, sentinel, sizeof m);
    CHECK(pixelica_camera_look_at(m, eye, eye, up) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_look_at(m, eye, target, v3(0.0f, 0.0f, 0.0f)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_look_at(m, v3(0.0f, 5.0f, 0.0f), v3(0.0f, 0.0f, 0.0f), up) ==
          PIXELICA_ERR_INVALID_ARG); /* looking straight down along up */
    CHECK(pixelica_camera_look_at(m, v3(0.0f, -5.0f, 0.0f), v3(0.0f, 0.0f, 0.0f), up) ==
          PIXELICA_ERR_INVALID_ARG); /* anti-parallel */
    CHECK(pixelica_camera_look_at(m, eye, v3(target.x, pixelica_t_nan(), target.z), up) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_look_at(m, eye, target, v3(0.0f, pixelica_t_inf(), 0.0f)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_look_at(m, v3(FLT_MAX, FLT_MAX, FLT_MAX), v3(0.0f, 0.0f, 0.0f),
                                  v3(0.0f, 0.0f, 1.0f)) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_look_at(NULL, eye, target, up) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(m, sentinel, sizeof m) == 0);

    /* Tiny but nonzero separation is still a valid direction. */
    CHECK(pixelica_camera_look_at(m, v3(1.0f, 0.0f, 0.0f), v3(1.0f, 0.0f, -1e-30f), up) ==
          PIXELICA_OK);
    CHECK_NEAR(m[10], 1.0, 1e-6);
}

static void run_shake(uint32_t seed, float dt, int steps, PIXELICA_Vec2 *out)
{
    PIXELICA_CameraShake sh;
    int i;

    memset(&sh, 0, sizeof sh);
    CHECK(pixelica_camera_shake_start(&sh, 6.0f, 2.0f, 25.0f, seed) == PIXELICA_OK);
    for (i = 0; i < steps; ++i) {
        CHECK(pixelica_camera_shake_update(&sh, dt, &out[i]) == PIXELICA_OK);
    }
}

static void test_shake(void)
{
    PIXELICA_CameraShake sh;
    PIXELICA_CameraShake before;
    PIXELICA_Vec2 a[8], b[8], c[8], off;
    PIXELICA_Vec2 sentinel = v2(9.0f, 9.0f);
    int i, nonzero = 0, differs = 0, varies = 0;

    /* Zero-initialised: idle. */
    memset(&sh, 0, sizeof sh);
    off = sentinel;
    CHECK(pixelica_camera_shake_update(&sh, 0.1f, &off) == PIXELICA_OK);
    CHECK(off.x == 0.0f && off.y == 0.0f && sh.elapsed == 0.0f);

    /* Same seed -> identical sequence; different seed -> different. */
    run_shake(1234u, 0.125f, 8, a);
    run_shake(1234u, 0.125f, 8, b);
    run_shake(99u, 0.125f, 8, c);
    CHECK(memcmp(a, b, sizeof a) == 0);
    for (i = 0; i < 8; ++i) {
        double decay = 1.0 - 0.125 * (i + 1) / 2.0;
        nonzero += (a[i].x != 0.0f || a[i].y != 0.0f);
        differs += (a[i].x != c[i].x || a[i].y != c[i].y);
        varies += (i > 0 && a[i].x != a[i - 1].x);
        CHECK(fabs(a[i].x) <= 6.0 * decay * decay + 1e-5);
        CHECK(fabs(a[i].y) <= 6.0 * decay * decay + 1e-5);
    }
    CHECK(nonzero == 8);
    CHECK(differs >= 6);
    CHECK(varies >= 6);

    /* Step-size independence: 2 x 0.5 s lands on the same offset as 8 x 0.125 s. */
    run_shake(1234u, 0.5f, 2, b);
    CHECK(memcmp(&b[1], &a[7], sizeof b[1]) == 0);

    /* Fades to exactly zero at the end; elapsed clamps; stays idle. */
    memset(&sh, 0, sizeof sh);
    CHECK(pixelica_camera_shake_start(&sh, 4.0f, 0.3f, 30.0f, 7u) == PIXELICA_OK);
    CHECK(pixelica_camera_shake_update(&sh, FLT_MAX, &off) == PIXELICA_OK);
    CHECK(off.x == 0.0f && off.y == 0.0f && sh.elapsed == sh.duration);
    CHECK(pixelica_camera_shake_update(&sh, 0.1f, &off) == PIXELICA_OK);
    CHECK(off.x == 0.0f && off.y == 0.0f);
    /* Restart rewinds. */
    CHECK(pixelica_camera_shake_start(&sh, 4.0f, 0.3f, 30.0f, 7u) == PIXELICA_OK);
    CHECK(sh.elapsed == 0.0f);
    /* Zero duration is accepted and idle. */
    CHECK(pixelica_camera_shake_start(&sh, 4.0f, 0.0f, 30.0f, 7u) == PIXELICA_OK);
    CHECK(pixelica_camera_shake_update(&sh, 0.0f, &off) == PIXELICA_OK);
    CHECK(off.x == 0.0f && off.y == 0.0f);

    /* Huge frequency * time stays well-defined. */
    CHECK(pixelica_camera_shake_start(&sh, 1.0f, FLT_MAX, FLT_MAX, 3u) == PIXELICA_OK);
    CHECK(pixelica_camera_shake_update(&sh, 1e30f, &off) == PIXELICA_OK);
    CHECK(fabs(off.x) <= 1.0 && fabs(off.y) <= 1.0);

    /* Invalid start/update: state and output untouched. */
    CHECK(pixelica_camera_shake_start(&sh, 2.0f, 1.0f, 10.0f, 5u) == PIXELICA_OK);
    before = sh;
    off = sentinel;
    CHECK(pixelica_camera_shake_start(NULL, 1.0f, 1.0f, 1.0f, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_start(&sh, -1.0f, 1.0f, 1.0f, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_start(&sh, 1.0f, -1.0f, 1.0f, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_start(&sh, 1.0f, 1.0f, 0.0f, 0u) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_start(&sh, pixelica_t_nan(), 1.0f, 1.0f, 0u) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_update(&sh, -0.1f, &off) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_update(&sh, pixelica_t_inf(), &off) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_update(&sh, 0.1f, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_camera_shake_update(NULL, 0.1f, &off) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&sh, &before, sizeof sh) == 0);
    sh.elapsed = 5.0f; /* corrupted: past duration */
    CHECK(pixelica_camera_shake_update(&sh, 0.1f, &off) == PIXELICA_ERR_INVALID_ARG);
    sh = before;
    sh.frequency = 0.0f; /* corrupted: active with no frequency */
    CHECK(pixelica_camera_shake_update(&sh, 0.1f, &off) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&off, &sentinel, sizeof off) == 0);
}

int main(void)
{
    test_follow_vec3();
    test_follow2d_centering();
    test_follow2d_bounds();
    test_follow2d_invalid();
    test_conversions();
    test_look_at();
    test_shake();
    return pixelica_t_finish("test_camera");
}
