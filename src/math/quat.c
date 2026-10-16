/*
 * Pixelica - rotation quaternions. See include/pixelica/math.h.
 *
 * Every entry point normalizes its inputs (the direction is the rotation),
 * works in double and writes its output only after the whole result is known
 * to be finite, so a failure leaves the caller's value byte-for-byte intact.
 */
#include "pixelica/math.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Above this |dot| the arc is too short for a stable acos/sin division; a
 * normalized linear blend is then accurate to well within float rounding. */
#define PIXELICA_QUAT_LERP_DOT 0.9995

static int pixelica_quat_finite(PIXELICA_Quat q)
{
    return isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w);
}

/* Unit-length q as doubles; 0 if q is non-finite or has zero length. */
static int pixelica_quat_unit(PIXELICA_Quat q, double out[4])
{
    double len2;
    double len;
    int i;

    if (!pixelica_quat_finite(q)) {
        return 0;
    }
    out[0] = (double)q.x;
    out[1] = (double)q.y;
    out[2] = (double)q.z;
    out[3] = (double)q.w;
    len2 = out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3];
    if (!(len2 > 0.0) || !isfinite(len2)) {
        return 0;
    }
    len = sqrt(len2);
    for (i = 0; i < 4; ++i) {
        out[i] /= len;
    }
    return 1;
}

/* Stores a finite double quadruple into a quaternion; 0 if out of range. */
static int pixelica_quat_store(PIXELICA_Quat *out, const double v[4])
{
    PIXELICA_Quat q;
    int i;

    for (i = 0; i < 4; ++i) {
        if (!(fabs(v[i]) <= (double)FLT_MAX)) {
            return 0;
        }
    }
    q.x = (float)v[0];
    q.y = (float)v[1];
    q.z = (float)v[2];
    q.w = (float)v[3];
    *out = q;
    return 1;
}

void pixelica_quat_identity(PIXELICA_Quat *out)
{
    if (out == NULL) {
        return;
    }
    out->x = 0.0f;
    out->y = 0.0f;
    out->z = 0.0f;
    out->w = 1.0f;
}

PIXELICA_Status pixelica_quat_normalize(PIXELICA_Quat *out, PIXELICA_Quat q)
{
    double u[4];

    if (out == NULL || !pixelica_quat_unit(q, u) ||
        !pixelica_quat_store(out, u)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_quat_from_axis_angle(PIXELICA_Quat *out, PIXELICA_Vec3 axis,
                                              float radians)
{
    double a[3];
    double len2;
    double len;
    double half;
    double s;
    double v[4];
    int i;

    if (out == NULL || !isfinite(axis.x) || !isfinite(axis.y) ||
        !isfinite(axis.z) || !isfinite(radians)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    a[0] = (double)axis.x;
    a[1] = (double)axis.y;
    a[2] = (double)axis.z;
    len2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    if (!(len2 > 0.0) || !isfinite(len2)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    len = sqrt(len2);
    half = 0.5 * (double)radians;
    s = sin(half) / len;
    for (i = 0; i < 3; ++i) {
        v[i] = a[i] * s;
    }
    v[3] = cos(half);
    return pixelica_quat_store(out, v) ? PIXELICA_OK : PIXELICA_ERR_INVALID_ARG;
}

PIXELICA_Status pixelica_quat_slerp(PIXELICA_Quat *out, PIXELICA_Quat a,
                                    PIXELICA_Quat b, float t)
{
    double qa[4];
    double qb[4];
    double r[4];
    double dot = 0.0;
    double ta;
    double wa, wb;
    int i;

    if (out == NULL || !isfinite(t) || !pixelica_quat_unit(a, qa) ||
        !pixelica_quat_unit(b, qb)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    ta = (double)t;
    if (!(ta > 0.0)) {
        ta = 0.0;
    } else if (ta > 1.0) {
        ta = 1.0;
    }
    for (i = 0; i < 4; ++i) {
        dot += qa[i] * qb[i];
    }
    if (dot < 0.0) { /* shortest arc: -q is the same rotation */
        for (i = 0; i < 4; ++i) {
            qb[i] = -qb[i];
        }
        dot = -dot;
    }
    if (dot > 1.0) { /* rounding */
        dot = 1.0;
    }
    if (dot > PIXELICA_QUAT_LERP_DOT) {
        wa = 1.0 - ta;
        wb = ta;
    } else {
        const double theta = acos(dot);
        const double sin_theta = sin(theta);

        wa = sin((1.0 - ta) * theta) / sin_theta;
        wb = sin(ta * theta) / sin_theta;
    }
    for (i = 0; i < 4; ++i) {
        r[i] = qa[i] * wa + qb[i] * wb;
    }
    {
        double len2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
        double len;

        if (!(len2 > 0.0) || !isfinite(len2)) {
            return PIXELICA_ERR_INVALID_ARG; /* unreachable for unit inputs */
        }
        len = sqrt(len2);
        for (i = 0; i < 4; ++i) {
            r[i] /= len;
        }
    }
    return pixelica_quat_store(out, r) ? PIXELICA_OK : PIXELICA_ERR_INVALID_ARG;
}

PIXELICA_Status pixelica_quat_to_mat4(float out[16], PIXELICA_Quat q)
{
    double u[4];
    double x, y, z, w;
    double m[9]; /* row-major 3x3 */
    float r[16];
    int i;

    if (out == NULL || !pixelica_quat_unit(q, u)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    x = u[0];
    y = u[1];
    z = u[2];
    w = u[3];
    m[0] = 1.0 - 2.0 * (y * y + z * z);
    m[1] = 2.0 * (x * y - w * z);
    m[2] = 2.0 * (x * z + w * y);
    m[3] = 2.0 * (x * y + w * z);
    m[4] = 1.0 - 2.0 * (x * x + z * z);
    m[5] = 2.0 * (y * z - w * x);
    m[6] = 2.0 * (x * z - w * y);
    m[7] = 2.0 * (y * z + w * x);
    m[8] = 1.0 - 2.0 * (x * x + y * y);

    memset(r, 0, sizeof r);
    for (i = 0; i < 9; ++i) {
        const int row = i / 3;
        const int col = i % 3;

        /* |m[i]| <= 3 for unit inputs: the narrowing cannot overflow. */
        r[col * 4 + row] = (float)m[i];
    }
    r[15] = 1.0f;
    memcpy(out, r, sizeof r);
    return PIXELICA_OK;
}
