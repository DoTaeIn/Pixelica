/*
 * Pixelica - vector/matrix math.
 *
 * Conventions (fixed for every platform):
 *   - Right-handed coordinates, +X right, +Y up, camera looks down -Z.
 *   - Angles in radians.
 *   - 4x4 matrices are float[16] in COLUMN-MAJOR order and act on COLUMN
 *     vectors: element (row r, column c) is m[c * 4 + r], the translation is
 *     m[12], m[13], m[14], and a point p is transformed as  p' = M * p.
 *   - pixelica_mat4_multiply(out, a, b) computes out = a * b, i.e. b is applied
 *     to the vector first. A typical chain is  clip = P * V * M * p.
 *   - Perspective projection maps view-space depth -near..-far to NDC
 *     z = -1..+1 (OpenGL style) with clip w = -z_view. Orthographic
 *     projection uses the same depth mapping with clip w = 1.
 *
 * This header belongs to the common library (Pixelica::common): it has no
 * dependency on the 3D renderer.
 *
 * All functions accept `out` aliasing an input. A NULL `out` (or NULL input
 * matrix) makes void functions do nothing. Non-finite arguments are not
 * diagnosed by the void builders; they produce non-finite matrices, which
 * the renderer rejects with PIXELICA_ERR_INVALID_ARG.
 */
#ifndef PIXELICA_MATH_H
#define PIXELICA_MATH_H

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PIXELICA_Vec3 {
    float x, y, z;
} PIXELICA_Vec3;

typedef struct PIXELICA_Vec2 {
    float x, y;
} PIXELICA_Vec2;

void pixelica_mat4_identity(float out[16]);
void pixelica_mat4_multiply(float out[16], const float a[16], const float b[16]);
void pixelica_mat4_translation(float out[16], float x, float y, float z);
void pixelica_mat4_scale(float out[16], float x, float y, float z);
/* Counter-clockwise rotation when looking from +axis toward the origin. */
void pixelica_mat4_rotation_x(float out[16], float radians);
void pixelica_mat4_rotation_y(float out[16], float radians);
void pixelica_mat4_rotation_z(float out[16], float radians);

/*
 * Right-handed perspective projection (camera -Z), NDC z in [-1, 1].
 * Requires: out != NULL, all arguments finite, 0 < fov_y_radians < pi,
 * aspect > 0, near_z > 0, far_z > near_z. On failure returns
 * PIXELICA_ERR_INVALID_ARG and leaves `out` unmodified.
 */
PIXELICA_Status pixelica_mat4_perspective(float out[16], float fov_y_radians,
                                float aspect, float near_z, float far_z);

/*
 * Right-handed orthographic projection (camera looks down -Z), NDC z in
 * [-1, 1] (OpenGL style, like pixelica_mat4_perspective). left/right/bottom/top are
 * view-space extents at any depth (+X right, +Y up); near_z and far_z are
 * POSITIVE distances in front of the camera (view-space z = -near_z ..
 * -far_z map to NDC z = -1 .. +1). Clip w stays 1.
 *
 *   m[0]  =  2 / (right - left)       m[12] = -(right + left) / (right - left)
 *   m[5]  =  2 / (top - bottom)       m[13] = -(top + bottom) / (top - bottom)
 *   m[10] = -2 / (far_z - near_z)     m[14] = -(far_z + near_z) / (far_z - near_z)
 *
 * Requires: out != NULL, all arguments finite, left < right, bottom < top,
 * 0 < near_z < far_z. Intermediates are computed in double and every element
 * must round to a finite float. On failure returns PIXELICA_ERR_INVALID_ARG and
 * leaves `out` unmodified.
 */
PIXELICA_Status pixelica_mat4_orthographic(float out[16], float left, float right,
                                 float bottom, float top, float near_z,
                                 float far_z);

/* out_xyzw = m * (p.x, p.y, p.z, 1). Does nothing if a pointer is NULL. */
void pixelica_mat4_transform_point(float out_xyzw[4], const float m[16], PIXELICA_Vec3 p);

/* Returns 1 if all 16 elements are finite, else 0 (also 0 for NULL). */
int pixelica_mat4_is_finite(const float m[16]);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_MATH_H */
