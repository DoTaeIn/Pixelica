#include "pixelica/camera.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

/* Minimum sine of the angle between `up` and the view direction. */
#define LOOK_AT_MIN_SIN 1e-6

static bool fin(float v)
{
    return isfinite(v) != 0;
}

static bool fin2(PIXELICA_Vec2 v)
{
    return fin(v.x) && fin(v.y);
}

static bool fin3(PIXELICA_Vec3 v)
{
    return fin(v.x) && fin(v.y) && fin(v.z);
}

/* True if v rounds to a finite float (also false for NaN). */
static bool fits(double v)
{
    return fabs(v) <= (double)FLT_MAX;
}

static bool camera_valid(const PIXELICA_Camera2D *c)
{
    return c != NULL && fin2(c->position) && fin(c->zoom) && c->zoom > 0.0f;
}

/* Fraction of the remaining distance to cover this step, in [0, 1]. */
static bool smoothing_alpha(float half_life, float dt, double *alpha)
{
    if (!fin(half_life) || !fin(dt) || half_life < 0.0f || dt < 0.0f) {
        return false;
    }
    if (half_life == 0.0f) {
        *alpha = 1.0;
    } else {
        *alpha = 1.0 - exp2(-(double)dt / (double)half_life);
    }
    return true;
}

/* Exponential approach, kept between cur and goal despite rounding. */
static double approach(double cur, double goal, double alpha)
{
    double v;
    double lo = cur < goal ? cur : goal;
    double hi = cur < goal ? goal : cur;

    if (alpha >= 1.0) {
        return goal;
    }
    v = cur + (goal - cur) * alpha;
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Keep [pos, pos + vis] inside [mn, mx]; center when the bounds are smaller. */
static double clamp_axis(double pos, double vis, double mn, double mx)
{
    double hi = mx - vis;

    if (hi < mn) {
        return (mn + mx) * 0.5 - vis * 0.5;
    }
    return pos < mn ? mn : (pos > hi ? hi : pos);
}

PIXELICA_Status pixelica_camera_follow_vec3(PIXELICA_Vec3 *position, PIXELICA_Vec3 target,
                                            float half_life, float dt)
{
    double alpha;
    PIXELICA_Vec3 r;

    if (position == NULL || !fin3(*position) || !fin3(target) ||
        !smoothing_alpha(half_life, dt, &alpha)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    r.x = (float)approach((double)position->x, (double)target.x, alpha);
    r.y = (float)approach((double)position->y, (double)target.y, alpha);
    r.z = (float)approach((double)position->z, (double)target.z, alpha);
    *position = r;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_camera2d_follow(PIXELICA_Camera2D *camera, PIXELICA_Vec2 viewport,
                                         PIXELICA_Vec2 target, float half_life, float dt,
                                         const PIXELICA_CameraBounds2D *bounds)
{
    double alpha, vis_x, vis_y, goal_x, goal_y, x, y;

    if (!camera_valid(camera) || !fin2(viewport) || viewport.x <= 0.0f ||
        viewport.y <= 0.0f || !fin2(target) || !smoothing_alpha(half_life, dt, &alpha)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (bounds != NULL && (!fin2(bounds->min) || !fin2(bounds->max) ||
                           bounds->min.x > bounds->max.x || bounds->min.y > bounds->max.y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    vis_x = (double)viewport.x / (double)camera->zoom;
    vis_y = (double)viewport.y / (double)camera->zoom;
    goal_x = (double)target.x - vis_x * 0.5;
    goal_y = (double)target.y - vis_y * 0.5;
    if (bounds != NULL) {
        goal_x = clamp_axis(goal_x, vis_x, (double)bounds->min.x, (double)bounds->max.x);
        goal_y = clamp_axis(goal_y, vis_y, (double)bounds->min.y, (double)bounds->max.y);
    }
    x = approach((double)camera->position.x, goal_x, alpha);
    y = approach((double)camera->position.y, goal_y, alpha);
    if (bounds != NULL) { /* e.g. after a zoom change the old position may be outside */
        x = clamp_axis(x, vis_x, (double)bounds->min.x, (double)bounds->max.x);
        y = clamp_axis(y, vis_y, (double)bounds->min.y, (double)bounds->max.y);
    }
    if (!fits(x) || !fits(y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    camera->position.x = (float)x;
    camera->position.y = (float)y;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_camera2d_world_to_screen(const PIXELICA_Camera2D *camera,
                                                  PIXELICA_Vec2 world,
                                                  PIXELICA_Vec2 *out_screen)
{
    double x, y;

    if (!camera_valid(camera) || !fin2(world) || out_screen == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    x = ((double)world.x - (double)camera->position.x) * (double)camera->zoom;
    y = ((double)world.y - (double)camera->position.y) * (double)camera->zoom;
    if (!fits(x) || !fits(y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out_screen->x = (float)x;
    out_screen->y = (float)y;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_camera2d_screen_to_world(const PIXELICA_Camera2D *camera,
                                                  PIXELICA_Vec2 screen,
                                                  PIXELICA_Vec2 *out_world)
{
    double x, y;

    if (!camera_valid(camera) || !fin2(screen) || out_world == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    x = (double)screen.x / (double)camera->zoom + (double)camera->position.x;
    y = (double)screen.y / (double)camera->zoom + (double)camera->position.y;
    if (!fits(x) || !fits(y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out_world->x = (float)x;
    out_world->y = (float)y;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_camera_look_at(float out_view[16], PIXELICA_Vec3 eye,
                                        PIXELICA_Vec3 target, PIXELICA_Vec3 up)
{
    double f[3], u[3], s[3], v[3], m[16], len;
    int i;

    if (out_view == NULL || !fin3(eye) || !fin3(target) || !fin3(up)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    f[0] = (double)target.x - (double)eye.x;
    f[1] = (double)target.y - (double)eye.y;
    f[2] = (double)target.z - (double)eye.z;
    len = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(len > 0.0)) {
        return PIXELICA_ERR_INVALID_ARG; /* eye == target */
    }
    for (i = 0; i < 3; ++i) {
        f[i] /= len;
    }
    u[0] = (double)up.x;
    u[1] = (double)up.y;
    u[2] = (double)up.z;
    len = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(len > 0.0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < 3; ++i) {
        u[i] /= len;
    }
    /* s = f x up (right), v = s x f (true up). */
    s[0] = f[1] * u[2] - f[2] * u[1];
    s[1] = f[2] * u[0] - f[0] * u[2];
    s[2] = f[0] * u[1] - f[1] * u[0];
    len = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(len > LOOK_AT_MIN_SIN)) {
        return PIXELICA_ERR_INVALID_ARG; /* up parallel to the view direction */
    }
    for (i = 0; i < 3; ++i) {
        s[i] /= len;
    }
    v[0] = s[1] * f[2] - s[2] * f[1];
    v[1] = s[2] * f[0] - s[0] * f[2];
    v[2] = s[0] * f[1] - s[1] * f[0];

    /* Rows s, v, -f; column-major element (r, c) is m[c * 4 + r]. */
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = v[0];
    m[5] = v[1];
    m[9] = v[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[3] = 0.0;
    m[7] = 0.0;
    m[11] = 0.0;
    m[12] = -(s[0] * (double)eye.x + s[1] * (double)eye.y + s[2] * (double)eye.z);
    m[13] = -(v[0] * (double)eye.x + v[1] * (double)eye.y + v[2] * (double)eye.z);
    m[14] = f[0] * (double)eye.x + f[1] * (double)eye.y + f[2] * (double)eye.z;
    m[15] = 1.0;
    for (i = 12; i < 15; ++i) {
        if (!fits(m[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    for (i = 0; i < 16; ++i) {
        out_view[i] = (float)m[i];
    }
    return PIXELICA_OK;
}

/* ---- screen shake --------------------------------------------------------- */

/* "lowbias32" integer hash (public domain, C. Wellons). */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* Deterministic value in [-1, 1) for lattice point k on one axis. */
static double lattice(uint32_t seed, uint32_t k, uint32_t axis)
{
    uint32_t h = hash32(hash32(seed + 0x9E3779B9u * axis) ^ k);

    return (double)(h >> 8) / 8388608.0 - 1.0;
}

/* Smooth value noise at t >= 0: smoothstep between neighbouring lattice values. */
static double noise(uint32_t seed, double t, uint32_t axis)
{
    double fl = floor(t);
    double fr = t - fl;
    uint32_t k = (uint32_t)fmod(fl, 4294967296.0); /* wraps, never overflows */
    double a = lattice(seed, k, axis);
    double b = lattice(seed, k + 1u, axis);
    double w = fr * fr * (3.0 - 2.0 * fr);

    return a + (b - a) * w;
}

static bool shake_valid(const PIXELICA_CameraShake *sh)
{
    return fin(sh->amplitude) && fin(sh->duration) && fin(sh->frequency) &&
           fin(sh->elapsed) && sh->amplitude >= 0.0f && sh->duration >= 0.0f &&
           sh->elapsed >= 0.0f && sh->elapsed <= sh->duration &&
           (sh->duration == 0.0f || sh->frequency > 0.0f);
}

PIXELICA_Status pixelica_camera_shake_start(PIXELICA_CameraShake *shake, float amplitude,
                                            float duration, float frequency, uint32_t seed)
{
    if (shake == NULL || !fin(amplitude) || !fin(duration) || !fin(frequency) ||
        amplitude < 0.0f || duration < 0.0f || frequency <= 0.0f) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    shake->amplitude = amplitude;
    shake->duration = duration;
    shake->frequency = frequency;
    shake->elapsed = 0.0f;
    shake->seed = seed;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_camera_shake_update(PIXELICA_CameraShake *shake, float dt,
                                             PIXELICA_Vec2 *out_offset)
{
    double e, t, k;
    float elapsed;
    PIXELICA_Vec2 off = {0.0f, 0.0f};

    if (shake == NULL || out_offset == NULL || !fin(dt) || dt < 0.0f || !shake_valid(shake)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    e = (double)shake->elapsed + (double)dt;
    elapsed = e >= (double)shake->duration ? shake->duration : (float)e;
    if (elapsed > shake->duration) {
        elapsed = shake->duration;
    }
    if (elapsed < shake->duration) {
        t = (double)elapsed * (double)shake->frequency;
        k = 1.0 - (double)elapsed / (double)shake->duration;
        k = (double)shake->amplitude * k * k;
        off.x = (float)(k * noise(shake->seed, t, 0u));
        off.y = (float)(k * noise(shake->seed, t, 1u));
    }
    shake->elapsed = elapsed;
    *out_offset = off;
    return PIXELICA_OK;
}
