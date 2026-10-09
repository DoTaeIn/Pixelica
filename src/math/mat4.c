/* Pixelica - column-major 4x4 matrix helpers. See include/pixelica/math.h. */
#include "pixelica/math.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define PIXELICA_PI_F 3.14159265358979323846f

static int pixelica_isfinite_f(float v)
{
    return isfinite(v) != 0;
}

void pixelica_mat4_identity(float out[16])
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, 16 * sizeof(float));
    out[0] = 1.0f;
    out[5] = 1.0f;
    out[10] = 1.0f;
    out[15] = 1.0f;
}

void pixelica_mat4_multiply(float out[16], const float a[16], const float b[16])
{
    float r[16];
    int c;
    int row;

    if (out == NULL || a == NULL || b == NULL) {
        return;
    }
    for (c = 0; c < 4; ++c) {
        for (row = 0; row < 4; ++row) {
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
                             a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] +
                             a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof r); /* temp copy makes aliasing safe */
}

void pixelica_mat4_translation(float out[16], float x, float y, float z)
{
    if (out == NULL) {
        return;
    }
    pixelica_mat4_identity(out);
    out[12] = x;
    out[13] = y;
    out[14] = z;
}

void pixelica_mat4_scale(float out[16], float x, float y, float z)
{
    if (out == NULL) {
        return;
    }
    pixelica_mat4_identity(out);
    out[0] = x;
    out[5] = y;
    out[10] = z;
}

void pixelica_mat4_rotation_x(float out[16], float radians)
{
    float c = cosf(radians);
    float s = sinf(radians);

    if (out == NULL) {
        return;
    }
    pixelica_mat4_identity(out);
    out[5] = c;
    out[6] = s;
    out[9] = -s;
    out[10] = c;
}

void pixelica_mat4_rotation_y(float out[16], float radians)
{
    float c = cosf(radians);
    float s = sinf(radians);

    if (out == NULL) {
        return;
    }
    pixelica_mat4_identity(out);
    out[0] = c;
    out[2] = -s;
    out[8] = s;
    out[10] = c;
}

void pixelica_mat4_rotation_z(float out[16], float radians)
{
    float c = cosf(radians);
    float s = sinf(radians);

    if (out == NULL) {
        return;
    }
    pixelica_mat4_identity(out);
    out[0] = c;
    out[1] = s;
    out[4] = -s;
    out[5] = c;
}

PIXELICA_Status pixelica_mat4_perspective(float out[16], float fov_y_radians,
                                float aspect, float near_z, float far_z)
{
    float f;
    float m[16];

    if (out == NULL || !pixelica_isfinite_f(fov_y_radians) ||
        !pixelica_isfinite_f(aspect) || !pixelica_isfinite_f(near_z) ||
        !pixelica_isfinite_f(far_z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!(fov_y_radians > 0.0f) || !(fov_y_radians < PIXELICA_PI_F) ||
        !(aspect > 0.0f) || !(near_z > 0.0f) || !(far_z > near_z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    f = 1.0f / tanf(fov_y_radians * 0.5f);
    memset(m, 0, sizeof m);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_z + near_z) / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    /* Extreme but valid ranges (e.g. far barely above near) can overflow. */
    if (!pixelica_mat4_is_finite(m)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memcpy(out, m, sizeof m);
    return PIXELICA_OK;
}

/* Narrows a double to float only when the value is finite and in float range
 * (a plain cast of an out-of-range double is undefined behavior). */
static int pixelica_narrow_to_float(double v, float *out)
{
    if (!(fabs(v) <= (double)FLT_MAX)) { /* also false for NaN */
        return 0;
    }
    *out = (float)v;
    return 1;
}

PIXELICA_Status pixelica_mat4_orthographic(float out[16], float left, float right,
                                 float bottom, float top, float near_z,
                                 float far_z)
{
    double width;
    double height;
    double depth;
    float m[16];

    if (out == NULL || !pixelica_isfinite_f(left) || !pixelica_isfinite_f(right) ||
        !pixelica_isfinite_f(bottom) || !pixelica_isfinite_f(top) ||
        !pixelica_isfinite_f(near_z) || !pixelica_isfinite_f(far_z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!(left < right) || !(bottom < top) || !(near_z > 0.0f) ||
        !(far_z > near_z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* Differences and sums of two floats are exact enough in double and cannot
     * overflow it; only the final narrowing to float can fail. */
    width = (double)right - (double)left;
    height = (double)top - (double)bottom;
    depth = (double)far_z - (double)near_z;

    memset(m, 0, sizeof m);
    if (!pixelica_narrow_to_float(2.0 / width, &m[0]) ||
        !pixelica_narrow_to_float(2.0 / height, &m[5]) ||
        !pixelica_narrow_to_float(-2.0 / depth, &m[10]) ||
        !pixelica_narrow_to_float(-((double)right + (double)left) / width, &m[12]) ||
        !pixelica_narrow_to_float(-((double)top + (double)bottom) / height, &m[13]) ||
        !pixelica_narrow_to_float(-((double)far_z + (double)near_z) / depth, &m[14])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    m[15] = 1.0f;
    memcpy(out, m, sizeof m);
    return PIXELICA_OK;
}

void pixelica_mat4_transform_point(float out_xyzw[4], const float m[16], PIXELICA_Vec3 p)
{
    float r[4];
    int row;

    if (out_xyzw == NULL || m == NULL) {
        return;
    }
    for (row = 0; row < 4; ++row) {
        r[row] = m[0 * 4 + row] * p.x + m[1 * 4 + row] * p.y +
                 m[2 * 4 + row] * p.z + m[3 * 4 + row];
    }
    memcpy(out_xyzw, r, sizeof r);
}

int pixelica_mat4_is_finite(const float m[16])
{
    int i;

    if (m == NULL) {
        return 0;
    }
    for (i = 0; i < 16; ++i) {
        if (!pixelica_isfinite_f(m[i])) {
            return 0;
        }
    }
    return 1;
}
