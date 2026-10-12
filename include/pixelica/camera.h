/*
 * Pixelica - small caller-owned camera helpers (common library).
 *
 * No heap, no global state, no RNG, no renderer dependency. Every function
 * is a pure computation over caller-owned values; distinct values share
 * nothing, so it is thread-safe as long as one value is not shared.
 *
 * 2D (same convention as the 2D renderer, see renderer2d.h):
 *   - Screen: (0,0) top-left, +X right, +Y down, units are pixels.
 *   - screen = (world - camera.position) * camera.zoom
 *     so camera.position is the world point at the TOP-LEFT of the viewport
 *     (the viewport origin), NOT the point at the center of the view. The
 *     visible world rectangle is position .. position + viewport / zoom.
 *   - PIXELICA_Camera2D has the same members as PIXELICA2D_Camera; copy
 *     position and zoom field by field into pixelica2d_set_camera().
 *
 * 3D (same convention as math.h): right-handed, +Y up, the camera looks
 * down -Z, matrices are column-major float[16] acting on column vectors.
 *
 * Smoothing: follow helpers move a position toward a goal by exponential
 * decay expressed as a HALF-LIFE in seconds: after `half_life` seconds half
 * of the remaining distance is covered, independent of how the time is
 * split into steps (two steps of dt/2 equal one step of dt up to rounding).
 * half_life == 0 snaps to the goal; dt == 0 (with half_life > 0) does not
 * move. Typical values: 0.05 (tight) .. 0.3 (loose).
 *
 * Errors: every function validates ALL inputs before writing anything. On a
 * non-PIXELICA_OK result every output and in/out argument is left exactly as
 * it was. PIXELICA_ERR_INVALID_ARG is returned for a NULL pointer, a
 * non-finite or out-of-range value, a degenerate configuration, or a result
 * that would not fit in a finite float. Intermediates use double precision.
 */
#ifndef PIXELICA_CAMERA_H
#define PIXELICA_CAMERA_H

#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2D camera. Default: position (0,0), zoom 1. zoom must be finite and > 0. */
typedef struct PIXELICA_Camera2D {
    PIXELICA_Vec2 position; /* world point at the viewport's top-left */
    float zoom;             /* screen pixels per world unit */
} PIXELICA_Camera2D;

/* World rectangle the visible area should stay inside. min <= max per axis. */
typedef struct PIXELICA_CameraBounds2D {
    PIXELICA_Vec2 min;
    PIXELICA_Vec2 max;
} PIXELICA_CameraBounds2D;

/*
 * Screen shake state. Zero-initialise it: an all-zero value is idle and
 * yields a (0,0) offset. Start it with pixelica_camera_shake_start(); its
 * fields may be read but should only be written through the functions.
 * The offset is a pure function of (seed, elapsed), so the same seed and the
 * same elapsed time always give the same offset, however dt was split.
 */
typedef struct PIXELICA_CameraShake {
    float amplitude; /* peak offset in world units at elapsed 0, >= 0 */
    float duration;  /* seconds; the shake is active while elapsed < duration */
    float frequency; /* noise samples per second, > 0 while active */
    float elapsed;   /* seconds since start, 0 .. duration */
    uint32_t seed;
} PIXELICA_CameraShake;

/*
 * Move *position toward `target` (see Smoothing). half_life and dt must be
 * finite and >= 0. The result always lies between the old position and the
 * target, so it cannot overflow.
 */
PIXELICA_Status pixelica_camera_follow_vec3(PIXELICA_Vec3 *position, PIXELICA_Vec3 target,
                                            float half_life, float dt);

/*
 * Smoothly center the camera on `target` (world units):
 *   goal = target - viewport / (2 * zoom)
 * viewport is the screen size in pixels (both > 0). If bounds is not NULL,
 * the goal and the final position are clamped so the visible rectangle
 * (viewport / zoom) stays inside the bounds; on an axis where the bounds are
 * smaller than the visible size the view is centered on the bounds instead.
 * The zoom is not changed. half_life and dt as for follow_vec3.
 */
PIXELICA_Status pixelica_camera2d_follow(PIXELICA_Camera2D *camera, PIXELICA_Vec2 viewport,
                                         PIXELICA_Vec2 target, float half_life, float dt,
                                         const PIXELICA_CameraBounds2D *bounds);

/* screen = (world - position) * zoom. */
PIXELICA_Status pixelica_camera2d_world_to_screen(const PIXELICA_Camera2D *camera,
                                                  PIXELICA_Vec2 world,
                                                  PIXELICA_Vec2 *out_screen);

/* world = screen / zoom + position (exact inverse up to rounding). */
PIXELICA_Status pixelica_camera2d_screen_to_world(const PIXELICA_Camera2D *camera,
                                                  PIXELICA_Vec2 screen,
                                                  PIXELICA_Vec2 *out_world);

/*
 * Right-handed look-at view matrix (gluLookAt equivalent): eye maps to the
 * origin, the direction to `target` maps to -Z and `up` is projected to +Y.
 * Requires finite inputs, eye != target, and `up` non-zero and not parallel
 * to the view direction (angle sine > 1e-6). Every element must round to a
 * finite float (very large eye coordinates can fail).
 */
PIXELICA_Status pixelica_camera_look_at(float out_view[16], PIXELICA_Vec3 eye,
                                        PIXELICA_Vec3 target, PIXELICA_Vec3 up);

/*
 * (Re)start a shake: elapsed = 0. amplitude >= 0, duration >= 0 and
 * frequency > 0, all finite. duration == 0 makes it idle at once.
 * Typical: amplitude 4, duration 0.3, frequency 30.
 */
PIXELICA_Status pixelica_camera_shake_start(PIXELICA_CameraShake *shake, float amplitude,
                                            float duration, float frequency, uint32_t seed);

/*
 * Advance elapsed by dt (finite, >= 0; clamped at duration) and write the
 * offset for the new elapsed time. The offset is smooth noise in
 * [-amplitude, amplitude] per axis scaled by (1 - elapsed/duration)^2, so it
 * fades to exactly (0,0) at the end; an idle shake yields (0,0). Add the
 * offset to a COPY of the camera position used for drawing, not to the
 * camera you follow with. Rejects a corrupted state.
 */
PIXELICA_Status pixelica_camera_shake_update(PIXELICA_CameraShake *shake, float dt,
                                             PIXELICA_Vec2 *out_offset);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_CAMERA_H */
