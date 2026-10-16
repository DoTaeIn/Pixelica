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

/*
 * Rotation quaternion q = (x, y, z, w) with w the SCALAR part, acting on a
 * column vector as v' = q v q* (same handedness and sign convention as
 * pixelica_mat4_rotation_x/y/z: counter-clockwise seen from +axis toward the
 * origin). A rotation of `angle` around a unit axis a is
 *   (sin(angle/2) * a, cos(angle/2)).
 *
 * Policy (every function below follows it):
 *   - Only the DIRECTION of the quaternion matters: inputs need not be unit
 *     length and are normalized internally; a zero-length or non-finite
 *     quaternion is PIXELICA_ERR_INVALID_ARG.
 *   - q and -q are the same rotation. Interpolation therefore takes the
 *     SHORTEST arc: pixelica_quat_slerp negates its second input when the dot
 *     product is negative, so the result never travels the long way round.
 *   - Outputs are unit length (up to float rounding).
 */
typedef struct PIXELICA_Quat {
    float x, y, z, w;
} PIXELICA_Quat;

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

/*
 * Normal matrix of `m`: the INVERSE TRANSPOSE of its upper-left 3x3 block,
 * written into the upper-left 3x3 of `out` with the rest set to identity
 * (out[3] = out[7] = out[11] = out[12..14] = 0, out[15] = 1). The translation
 * and the bottom row of `m` are ignored.
 *
 * This is the transform a DIRECTION (a surface normal) must use so that it
 * stays perpendicular to the surface under a NON-UNIFORM scale or a shear,
 * where transforming it with `m` itself would tilt it. It is not normalized:
 * the caller normalizes the result (lighting does, see renderer.h). A
 * mirroring matrix (negative determinant) flips the normals, matching the
 * winding flip the same matrix applies to the geometry.
 *
 * Computed in double. Returns PIXELICA_ERR_INVALID_ARG, leaving `out`
 * unmodified, for a NULL pointer, a non-finite element, a SINGULAR 3x3 block
 * (zero or non-finite determinant, e.g. a zero scale on one axis) or a result
 * element outside the float range.
 */
PIXELICA_Status pixelica_mat4_normal_matrix(float out[16], const float m[16]);

/* out_xyzw = m * (p.x, p.y, p.z, 1). Does nothing if a pointer is NULL. */
void pixelica_mat4_transform_point(float out_xyzw[4], const float m[16], PIXELICA_Vec3 p);

/* Returns 1 if all 16 elements are finite, else 0 (also 0 for NULL). */
int pixelica_mat4_is_finite(const float m[16]);

/* ---- rotation quaternions (see PIXELICA_Quat above) --------------------- */

/* (0, 0, 0, 1): no rotation. NULL is a no-op. */
void pixelica_quat_identity(PIXELICA_Quat *out);

/* *out = q / |q|. PIXELICA_ERR_INVALID_ARG (out unchanged) for NULL, a
 * non-finite component or |q| == 0. `out` may alias nothing in particular;
 * the value is passed by value, so in-place use is always safe. */
PIXELICA_Status pixelica_quat_normalize(PIXELICA_Quat *out, PIXELICA_Quat q);

/* Rotation of `radians` around `axis` (normalized internally). Requires a
 * finite angle and a finite non-zero axis, else PIXELICA_ERR_INVALID_ARG with
 * `out` unchanged. */
PIXELICA_Status pixelica_quat_from_axis_angle(PIXELICA_Quat *out, PIXELICA_Vec3 axis,
                                              float radians);

/*
 * Shortest-arc spherical interpolation. `t` is clamped to [0, 1]; t == 0
 * gives normalized `a`, t == 1 gives normalized `b` (negated when that is
 * the shorter arc, which is the same rotation). Nearly parallel inputs fall
 * back to a normalized linear blend, which is accurate to within float
 * rounding there. Requires finite non-zero `a` and `b` and a finite `t`,
 * else PIXELICA_ERR_INVALID_ARG with `out` unchanged.
 */
PIXELICA_Status pixelica_quat_slerp(PIXELICA_Quat *out, PIXELICA_Quat a,
                                    PIXELICA_Quat b, float t);

/* Column-major rotation matrix of `q` (normalized internally), bottom row
 * (0,0,0,1) and no translation. PIXELICA_ERR_INVALID_ARG (out unchanged) for
 * NULL, a non-finite component or |q| == 0. */
PIXELICA_Status pixelica_quat_to_mat4(float out[16], PIXELICA_Quat q);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_MATH_H */
