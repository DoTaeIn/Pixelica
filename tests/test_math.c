/* Pixelica - math conventions: column-major, column vectors, right-handed,
 * OpenGL-style perspective and orthographic projection with NDC z in [-1, 1].
 * Common-library test: links Pixelica::common only and includes no 3D header. */
#include <float.h>

#include "test_util.h"

#define PI_F 3.14159265358979323846f
#define EPS 1e-5

static void check_mat_near(const float *a, const float *b, double tol)
{
    int i;
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(a[i], b[i], tol);
    }
}

static void xform(const float m[16], float x, float y, float z, float out[4])
{
    PIXELICA_Vec3 p;
    p.x = x;
    p.y = y;
    p.z = z;
    pixelica_mat4_transform_point(out, m, p);
}

static void test_identity_and_layout(void)
{
    float m[16];
    float t[16];
    float r[4];
    int i;

    pixelica_mat4_identity(m);
    for (i = 0; i < 16; ++i) {
        CHECK(m[i] == ((i % 5) == 0 ? 1.0f : 0.0f));
    }

    /* Column-major: translation lives in elements 12..14. */
    pixelica_mat4_translation(t, 10.0f, 20.0f, 30.0f);
    CHECK(t[12] == 10.0f && t[13] == 20.0f && t[14] == 30.0f);
    CHECK(t[3] == 0.0f && t[7] == 0.0f && t[11] == 0.0f && t[15] == 1.0f);

    xform(t, 1.0f, 2.0f, 3.0f, r);
    CHECK_NEAR(r[0], 11.0, EPS);
    CHECK_NEAR(r[1], 22.0, EPS);
    CHECK_NEAR(r[2], 33.0, EPS);
    CHECK_NEAR(r[3], 1.0, EPS);

    pixelica_mat4_scale(m, 2.0f, 3.0f, 4.0f);
    xform(m, 1.0f, 1.0f, 1.0f, r);
    CHECK_NEAR(r[0], 2.0, EPS);
    CHECK_NEAR(r[1], 3.0, EPS);
    CHECK_NEAR(r[2], 4.0, EPS);
}

static void test_multiply(void)
{
    float a[16];
    float b[16];
    float c[16];
    float id[16];
    float expect[16];
    float rz[16];
    float r[4];

    pixelica_mat4_translation(a, 1.0f, 2.0f, 3.0f);
    pixelica_mat4_translation(b, 4.0f, 5.0f, 6.0f);
    pixelica_mat4_multiply(c, a, b);
    pixelica_mat4_translation(expect, 5.0f, 7.0f, 9.0f);
    check_mat_near(c, expect, EPS);

    pixelica_mat4_identity(id);
    pixelica_mat4_rotation_y(a, 0.7f);
    pixelica_mat4_multiply(c, id, a);
    check_mat_near(c, a, EPS);
    pixelica_mat4_multiply(c, a, id);
    check_mat_near(c, a, EPS);

    /* out = a * b applies b first: T * Rz(90) maps (1,0,0) -> (0,1,0) -> (5,1,0). */
    pixelica_mat4_translation(a, 5.0f, 0.0f, 0.0f);
    pixelica_mat4_rotation_z(rz, PI_F * 0.5f);
    pixelica_mat4_multiply(c, a, rz);
    xform(c, 1.0f, 0.0f, 0.0f, r);
    CHECK_NEAR(r[0], 5.0, EPS);
    CHECK_NEAR(r[1], 1.0, EPS);
    CHECK_NEAR(r[2], 0.0, EPS);

    /* Aliasing: out may be the same array as an input. */
    pixelica_mat4_translation(a, 1.0f, 0.0f, 0.0f);
    pixelica_mat4_translation(b, 0.0f, 2.0f, 0.0f);
    pixelica_mat4_multiply(a, a, b);
    pixelica_mat4_translation(expect, 1.0f, 2.0f, 0.0f);
    check_mat_near(a, expect, EPS);
    pixelica_mat4_multiply(b, a, b);
    pixelica_mat4_translation(expect, 1.0f, 4.0f, 0.0f);
    check_mat_near(b, expect, EPS);
}

static void test_rotations(void)
{
    float m[16];
    float inv[16];
    float prod[16];
    float id[16];
    float r[4];

    /* Right-handed, counter-clockwise looking down the +axis. */
    pixelica_mat4_rotation_x(m, PI_F * 0.5f);
    xform(m, 0.0f, 1.0f, 0.0f, r);
    CHECK_NEAR(r[0], 0.0, EPS);
    CHECK_NEAR(r[1], 0.0, EPS);
    CHECK_NEAR(r[2], 1.0, EPS);

    pixelica_mat4_rotation_y(m, PI_F * 0.5f);
    xform(m, 0.0f, 0.0f, 1.0f, r);
    CHECK_NEAR(r[0], 1.0, EPS);
    CHECK_NEAR(r[1], 0.0, EPS);
    CHECK_NEAR(r[2], 0.0, EPS);

    pixelica_mat4_rotation_z(m, PI_F * 0.5f);
    xform(m, 1.0f, 0.0f, 0.0f, r);
    CHECK_NEAR(r[0], 0.0, EPS);
    CHECK_NEAR(r[1], 1.0, EPS);
    CHECK_NEAR(r[2], 0.0, EPS);

    /* R(a) * R(-a) == I for every axis. */
    pixelica_mat4_identity(id);
    pixelica_mat4_rotation_x(m, 1.234f);
    pixelica_mat4_rotation_x(inv, -1.234f);
    pixelica_mat4_multiply(prod, m, inv);
    check_mat_near(prod, id, EPS);
    pixelica_mat4_rotation_y(m, -2.5f);
    pixelica_mat4_rotation_y(inv, 2.5f);
    pixelica_mat4_multiply(prod, m, inv);
    check_mat_near(prod, id, EPS);
    pixelica_mat4_rotation_z(m, 0.3f);
    pixelica_mat4_rotation_z(inv, -0.3f);
    pixelica_mat4_multiply(prod, m, inv);
    check_mat_near(prod, id, EPS);
}

static void test_perspective(void)
{
    const float fov = PI_F * 0.5f; /* tan(fov/2) == 1 */
    const float aspect = 2.0f;
    const float n = 0.5f;
    const float f = 100.0f;
    float p[16];
    float r[4];

    CHECK(pixelica_mat4_perspective(p, fov, aspect, n, f) == PIXELICA_OK);

    /* Near plane -> NDC z = -1, far plane -> +1, w = -z_view. */
    xform(p, 0.0f, 0.0f, -n, r);
    CHECK_NEAR(r[3], n, EPS);
    CHECK_NEAR(r[2] / r[3], -1.0, 1e-4);
    xform(p, 0.0f, 0.0f, -f, r);
    CHECK_NEAR(r[3], f, 1e-3);
    CHECK_NEAR(r[2] / r[3], 1.0, 1e-4);

    /* Monotonic: farther points get larger NDC z. */
    {
        float r2[4];
        xform(p, 0.0f, 0.0f, -2.0f, r);
        xform(p, 0.0f, 0.0f, -3.0f, r2);
        CHECK(r[2] / r[3] < r2[2] / r2[3]);
    }

    /* Frustum edges: at distance d, y = d*tan(fov/2) and x = aspect*y hit
     * NDC +1; +Y is up and +X is right. */
    xform(p, 0.0f, 4.0f, -4.0f, r);
    CHECK_NEAR(r[1] / r[3], 1.0, 1e-5);
    xform(p, 8.0f, 0.0f, -4.0f, r);
    CHECK_NEAR(r[0] / r[3], 1.0, 1e-5);
    xform(p, -8.0f, -4.0f, -4.0f, r);
    CHECK_NEAR(r[0] / r[3], -1.0, 1e-5);
    CHECK_NEAR(r[1] / r[3], -1.0, 1e-5);

    /* Behind the camera w is negative. */
    xform(p, 0.0f, 0.0f, 1.0f, r);
    CHECK(r[3] < 0.0f);
}

static void test_perspective_invalid(void)
{
    const float pos_inf = pixelica_t_inf();
    const float qnan = pixelica_t_nan();
    float m[16];
    float sentinel[16];
    int i;
    struct {
        float fov, aspect, n, f;
    } bad[] = {
        {1.0f, 1.0f, 0.0f, 10.0f},   /* near == 0 */
        {1.0f, 1.0f, -1.0f, 10.0f},  /* near < 0 */
        {1.0f, 1.0f, 1.0f, 1.0f},    /* far == near */
        {1.0f, 1.0f, 2.0f, 1.0f},    /* far < near */
        {0.0f, 1.0f, 0.1f, 10.0f},   /* fov == 0 */
        {-1.0f, 1.0f, 0.1f, 10.0f},  /* fov < 0 */
        {PI_F, 1.0f, 0.1f, 10.0f},   /* fov == pi */
        {4.0f, 1.0f, 0.1f, 10.0f},   /* fov > pi */
        {1.0f, 0.0f, 0.1f, 10.0f},   /* aspect == 0 */
        {1.0f, -1.0f, 0.1f, 10.0f},  /* aspect < 0 */
        {0.0f, 1.0f, 0.1f, 10.0f},   /* replaced below with NaN/inf */
        {0.0f, 1.0f, 0.1f, 10.0f},
        {0.0f, 1.0f, 0.1f, 10.0f},
        {0.0f, 1.0f, 0.1f, 10.0f},
        {0.0f, 1.0f, 0.1f, 10.0f},
    };
    const int count = (int)(sizeof bad / sizeof bad[0]);

    bad[10].fov = qnan;
    bad[11].fov = 1.0f;
    bad[11].aspect = pos_inf;
    bad[12].fov = 1.0f;
    bad[12].n = qnan;
    bad[13].fov = 1.0f;
    bad[13].f = pos_inf;
    bad[14].fov = 1.0f;
    bad[14].aspect = qnan;

    for (i = 0; i < 16; ++i) {
        sentinel[i] = 42.0f + (float)i;
    }
    for (i = 0; i < count; ++i) {
        memcpy(m, sentinel, sizeof m);
        CHECK(pixelica_mat4_perspective(m, bad[i].fov, bad[i].aspect, bad[i].n,
                                   bad[i].f) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(m, sentinel, sizeof m) == 0); /* untouched on failure */
    }
    CHECK(pixelica_mat4_perspective(NULL, 1.0f, 1.0f, 0.1f, 10.0f) ==
          PIXELICA_ERR_INVALID_ARG);
}

/* Right-handed orthographic projection: camera looks down -Z, near/far are
 * positive view distances, NDC z in [-1, 1], clip w stays 1. */
static void test_orthographic(void)
{
    float m[16];
    float r[4];
    float view[16];
    float vp[16];
    float expect[16];
    int i;

    /* Symmetric 8 x 6 box, near 1, far 11. */
    CHECK(pixelica_mat4_orthographic(m, -4.0f, 4.0f, -3.0f, 3.0f, 1.0f, 11.0f) ==
          PIXELICA_OK);
    for (i = 0; i < 16; ++i) {
        expect[i] = 0.0f;
    }
    expect[0] = 0.25f;           /* 2 / (r - l) */
    expect[5] = 2.0f / 6.0f;     /* 2 / (t - b) */
    expect[10] = -0.2f;          /* -2 / (f - n) */
    expect[12] = 0.0f;           /* -(r + l) / (r - l) */
    expect[13] = 0.0f;           /* -(t + b) / (t - b) */
    expect[14] = -1.2f;          /* -(f + n) / (f - n) */
    expect[15] = 1.0f;
    check_mat_near(m, expect, 1e-6);

    /* Corners of the box map to the NDC cube corners; w is always 1. */
    xform(m, -4.0f, -3.0f, -1.0f, r); /* left, bottom, near */
    CHECK_NEAR(r[0], -1.0, 1e-6);
    CHECK_NEAR(r[1], -1.0, 1e-6);
    CHECK_NEAR(r[2], -1.0, 1e-6);
    CHECK_NEAR(r[3], 1.0, 0.0);
    xform(m, 4.0f, 3.0f, -11.0f, r); /* right, top, far */
    CHECK_NEAR(r[0], 1.0, 1e-6);
    CHECK_NEAR(r[1], 1.0, 1e-6);
    CHECK_NEAR(r[2], 1.0, 1e-6);
    CHECK_NEAR(r[3], 1.0, 0.0);
    xform(m, 0.0f, 0.0f, -6.0f, r); /* box center in depth */
    CHECK_NEAR(r[0], 0.0, 1e-6);
    CHECK_NEAR(r[1], 0.0, 1e-6);
    CHECK_NEAR(r[2], 0.0, 1e-6);

    /* No perspective: x/y do not depend on depth, +Y is up, +X is right,
     * z grows with distance, and behind the camera is outside [-1, 1]. */
    xform(m, 2.0f, 1.5f, -2.0f, r);
    CHECK_NEAR(r[0], 0.5, 1e-6);
    CHECK_NEAR(r[1], 0.5, 1e-6);
    xform(m, 2.0f, 1.5f, -9.0f, r);
    CHECK_NEAR(r[0], 0.5, 1e-6);
    CHECK_NEAR(r[1], 0.5, 1e-6);
    {
        float near_pt[4];
        float far_pt[4];
        xform(m, 0.0f, 0.0f, -2.0f, near_pt);
        xform(m, 0.0f, 0.0f, -3.0f, far_pt);
        CHECK(near_pt[2] < far_pt[2]);
    }
    xform(m, 0.0f, 0.0f, 1.0f, r); /* behind the camera: z_ndc < -1 */
    CHECK(r[2] < -1.0f);
    CHECK_NEAR(r[3], 1.0, 0.0);

    /* Off-center box: its center maps to the NDC origin. */
    CHECK(pixelica_mat4_orthographic(m, 2.0f, 6.0f, -1.0f, 1.0f, 0.5f, 2.5f) ==
          PIXELICA_OK);
    xform(m, 4.0f, 0.0f, -1.5f, r);
    CHECK_NEAR(r[0], 0.0, 1e-6);
    CHECK_NEAR(r[1], 0.0, 1e-6);
    CHECK_NEAR(r[2], 0.0, 1e-6);
    xform(m, 2.0f, -1.0f, -0.5f, r);
    CHECK_NEAR(r[0], -1.0, 1e-6);
    CHECK_NEAR(r[1], -1.0, 1e-6);
    CHECK_NEAR(r[2], -1.0, 1e-6);

    /* In a view * projection chain: a camera 5 units back along +Z sees the
     * origin at view distance 5, i.e. NDC z = -1 + 2 * (5 - 1) / (11 - 1). */
    CHECK(pixelica_mat4_orthographic(m, -4.0f, 4.0f, -3.0f, 3.0f, 1.0f, 11.0f) ==
          PIXELICA_OK);
    pixelica_mat4_translation(view, 0.0f, 0.0f, -5.0f);
    pixelica_mat4_multiply(vp, m, view);
    xform(vp, 0.0f, 0.0f, 0.0f, r);
    CHECK_NEAR(r[2], -0.2, 1e-6);

    /* The depth range matches pixelica_mat4_perspective's: near -> -1, far -> +1. */
    {
        float p[16];
        float o[16];
        float rp[4];
        float ro[4];

        CHECK(pixelica_mat4_perspective(p, 1.0f, 1.0f, 0.5f, 100.0f) == PIXELICA_OK);
        CHECK(pixelica_mat4_orthographic(o, -1.0f, 1.0f, -1.0f, 1.0f, 0.5f,
                                    100.0f) == PIXELICA_OK);
        xform(p, 0.0f, 0.0f, -0.5f, rp);
        xform(o, 0.0f, 0.0f, -0.5f, ro);
        CHECK_NEAR(rp[2] / rp[3], ro[2] / ro[3], 1e-4);
        xform(p, 0.0f, 0.0f, -100.0f, rp);
        xform(o, 0.0f, 0.0f, -100.0f, ro);
        CHECK_NEAR(rp[2] / rp[3], ro[2] / ro[3], 1e-4);
    }

    /* Extreme but representable ranges are accepted and finite. */
    CHECK(pixelica_mat4_orthographic(m, -FLT_MAX, FLT_MAX, -1.0f, 1.0f, 1.0f,
                                FLT_MAX) == PIXELICA_OK);
    CHECK(pixelica_mat4_is_finite(m) == 1);
}

static void test_orthographic_invalid(void)
{
    const float pos_inf = pixelica_t_inf();
    const float qnan = pixelica_t_nan();
    const float denorm = nextafterf(0.0f, 1.0f); /* smallest positive float */
    float m[16];
    float sentinel[16];
    int i;
    struct {
        float l, r, b, t, n, f;
    } bad[] = {
        {1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 2.0f},   /* left == right */
        {2.0f, 1.0f, -1.0f, 1.0f, 1.0f, 2.0f},   /* left > right */
        {-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f},   /* bottom == top */
        {-1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 2.0f},   /* bottom > top */
        {-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 2.0f},  /* near == 0 */
        {-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 2.0f}, /* near < 0 */
        {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 1.0f},  /* far == near */
        {-1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 1.0f},  /* far < near */
        {0.0f, denorm, -1.0f, 1.0f, 1.0f, 2.0f}, /* 2/width overflows float */
        {-1.0f, 1.0f, 0.0f, denorm, 1.0f, 2.0f}, /* 2/height overflows float */
        {-1.0f, 1.0f, -1.0f, 1.0f, denorm, 2.0f * denorm}, /* 2/depth overflows */
        {qnan, 1.0f, -1.0f, 1.0f, 1.0f, 2.0f},
        {-1.0f, qnan, -1.0f, 1.0f, 1.0f, 2.0f},
        {-1.0f, 1.0f, qnan, 1.0f, 1.0f, 2.0f},
        {-1.0f, 1.0f, -1.0f, qnan, 1.0f, 2.0f},
        {-1.0f, 1.0f, -1.0f, 1.0f, qnan, 2.0f},
        {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, qnan},
        {-pos_inf, 1.0f, -1.0f, 1.0f, 1.0f, 2.0f},
        {-1.0f, pos_inf, -1.0f, 1.0f, 1.0f, 2.0f},
        {-1.0f, 1.0f, -pos_inf, 1.0f, 1.0f, 2.0f},
        {-1.0f, 1.0f, -1.0f, pos_inf, 1.0f, 2.0f},
        {-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, pos_inf},
    };
    const int count = (int)(sizeof bad / sizeof bad[0]);

    for (i = 0; i < 16; ++i) {
        sentinel[i] = 42.0f + (float)i;
    }
    for (i = 0; i < count; ++i) {
        memcpy(m, sentinel, sizeof m);
        CHECK(pixelica_mat4_orthographic(m, bad[i].l, bad[i].r, bad[i].b, bad[i].t,
                                    bad[i].n, bad[i].f) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(m, sentinel, sizeof m) == 0); /* untouched on failure */
    }
    CHECK(pixelica_mat4_orthographic(NULL, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 2.0f) ==
          PIXELICA_ERR_INVALID_ARG);
}

static void test_null_and_finite(void)
{
    float m[16];

    /* NULL outputs/inputs are ignored rather than dereferenced. */
    pixelica_mat4_identity(NULL);
    pixelica_mat4_identity(m);
    pixelica_mat4_multiply(NULL, m, m);
    pixelica_mat4_multiply(m, NULL, m);
    CHECK(m[0] == 1.0f && m[5] == 1.0f && m[1] == 0.0f);
    pixelica_mat4_translation(NULL, 1.0f, 2.0f, 3.0f);
    pixelica_mat4_rotation_x(NULL, 1.0f);
    pixelica_mat4_rotation_y(NULL, 1.0f);
    pixelica_mat4_rotation_z(NULL, 1.0f);
    pixelica_mat4_scale(NULL, 1.0f, 1.0f, 1.0f);
    {
        PIXELICA_Vec3 v = {1.0f, 2.0f, 3.0f};
        pixelica_mat4_transform_point(NULL, m, v);
    }

    pixelica_mat4_identity(m);
    CHECK(pixelica_mat4_is_finite(m) == 1);
    m[7] = pixelica_t_nan();
    CHECK(pixelica_mat4_is_finite(m) == 0);
    m[7] = -pixelica_t_inf();
    CHECK(pixelica_mat4_is_finite(m) == 0);
    CHECK(pixelica_mat4_is_finite(NULL) == 0);
}

/* ---- normal matrix (inverse transpose of the upper-left 3x3) ---------- */

/* out = a * b restricted to the 3x3 block, row-major result. */
static void mul3(double out[3][3], const float a[16], const float b[16])
{
    int r, c, k;

    for (r = 0; r < 3; ++r) {
        for (c = 0; c < 3; ++c) {
            double s = 0.0;
            for (k = 0; k < 3; ++k) {
                s += (double)a[k * 4 + r] * (double)b[c * 4 + k];
            }
            out[r][c] = s;
        }
    }
}

static void test_normal_matrix(void)
{
    float m[16];
    float n[16];
    float keep[16];
    double p[3][3];
    int i;

    /* A rotation is orthonormal: its inverse transpose is itself. */
    pixelica_mat4_rotation_y(m, 0.7f);
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_OK);
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(n[i], m[i], 1e-5);
    }

    /* Translation is ignored: only the 3x3 block matters. */
    m[12] = 9.0f;
    m[13] = -3.0f;
    m[14] = 4.0f;
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_OK);
    CHECK_NEAR(n[12], 0.0, 0.0);
    CHECK_NEAR(n[13], 0.0, 0.0);
    CHECK_NEAR(n[14], 0.0, 0.0);
    CHECK(n[15] == 1.0f && n[3] == 0.0f && n[7] == 0.0f && n[11] == 0.0f);

    /* Non-uniform scale: the normal matrix holds the RECIPROCAL scales, and
     * M^T * N is the identity by construction. */
    pixelica_mat4_scale(m, 2.0f, 4.0f, 0.5f);
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_OK);
    CHECK_NEAR(n[0], 0.5, 1e-6);
    CHECK_NEAR(n[5], 0.25, 1e-6);
    CHECK_NEAR(n[10], 2.0, 1e-6);

    /* A sheared, scaled, rotated matrix: transpose(M) * N == I exactly
     * characterizes the inverse transpose. */
    pixelica_mat4_identity(m);
    m[0] = 2.0f;
    m[4] = 0.5f;  /* shear x by y */
    m[5] = 3.0f;
    m[9] = -0.25f;
    m[10] = 1.5f;
    m[2] = 0.75f;
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_OK);
    {
        float mt[16];
        int r, c;

        pixelica_mat4_identity(mt);
        for (r = 0; r < 3; ++r) {
            for (c = 0; c < 3; ++c) {
                mt[c * 4 + r] = m[r * 4 + c]; /* transpose of the 3x3 */
            }
        }
        mul3(p, mt, n);
        for (r = 0; r < 3; ++r) {
            for (c = 0; c < 3; ++c) {
                CHECK_NEAR(p[r][c], r == c ? 1.0 : 0.0, 1e-6);
            }
        }
    }

    /* A normal stays perpendicular: for the surface tangent t and normal n0
     * with dot(n0, t) == 0, dot(N*n0, M*t) == 0 too. */
    {
        const PIXELICA_Vec3 t0 = {1.0f, 0.0f, 0.0f};
        const PIXELICA_Vec3 n0 = {0.0f, 1.0f, 0.0f};
        float mt[4], nn[4];
        double d;

        pixelica_mat4_transform_point(mt, m, t0);
        pixelica_mat4_transform_point(nn, n, n0);
        /* Directions: subtract the translation columns (both are zero here
         * for n, and m has none either). */
        d = (double)mt[0] * nn[0] + (double)mt[1] * nn[1] + (double)mt[2] * nn[2];
        CHECK_NEAR(d, 0.0, 1e-5);
        /* The naive "transform the normal with M" would NOT be perpendicular. */
        pixelica_mat4_transform_point(nn, m, n0);
        d = (double)mt[0] * nn[0] + (double)mt[1] * nn[1] + (double)mt[2] * nn[2];
        CHECK(fabs(d) > 0.1);
    }

    /* Mirroring (negative determinant) flips the normals. */
    pixelica_mat4_scale(m, -1.0f, 1.0f, 1.0f);
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_OK);
    CHECK_NEAR(n[0], -1.0, 0.0);
    CHECK_NEAR(n[5], 1.0, 0.0);

    /* Invalid input leaves `out` untouched. */
    for (i = 0; i < 16; ++i) {
        n[i] = 7.5f;
        keep[i] = 7.5f;
    }
    pixelica_mat4_scale(m, 1.0f, 0.0f, 1.0f); /* singular */
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_ERR_INVALID_ARG);
    pixelica_mat4_identity(m);
    m[5] = pixelica_t_nan();
    CHECK(pixelica_mat4_normal_matrix(n, m) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mat4_normal_matrix(n, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mat4_normal_matrix(NULL, m) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(n, keep, sizeof n) == 0);
}

/* ---- quaternions ------------------------------------------------------- */

static double quat_dot(PIXELICA_Quat a, PIXELICA_Quat b)
{
    return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z +
           (double)a.w * b.w;
}

static void test_quat_basics(void)
{
    const PIXELICA_Vec3 zaxis = {0.0f, 0.0f, 1.0f};
    const PIXELICA_Vec3 zero = {0.0f, 0.0f, 0.0f};
    PIXELICA_Quat q, keep;
    float m[16], r[16];
    int i;

    pixelica_quat_identity(&q);
    CHECK(q.x == 0.0f && q.y == 0.0f && q.z == 0.0f && q.w == 1.0f);
    pixelica_quat_identity(NULL); /* no crash */

    /* An identity quaternion is the identity matrix. */
    CHECK(pixelica_quat_to_mat4(m, q) == PIXELICA_OK);
    pixelica_mat4_identity(r);
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(m[i], r[i], 0.0);
    }

    /* A quaternion rotation matches pixelica_mat4_rotation_z elementwise,
     * which fixes the sign/handedness convention. */
    CHECK(pixelica_quat_from_axis_angle(&q, zaxis, 0.9f) == PIXELICA_OK);
    CHECK(pixelica_quat_to_mat4(m, q) == PIXELICA_OK);
    pixelica_mat4_rotation_z(r, 0.9f);
    for (i = 0; i < 16; ++i) {
        CHECK_NEAR(m[i], r[i], 1e-5);
    }
    {
        const PIXELICA_Vec3 xaxis = {1.0f, 0.0f, 0.0f};
        const PIXELICA_Vec3 yaxis = {0.0f, 1.0f, 0.0f};

        CHECK(pixelica_quat_from_axis_angle(&q, xaxis, -0.4f) == PIXELICA_OK);
        CHECK(pixelica_quat_to_mat4(m, q) == PIXELICA_OK);
        pixelica_mat4_rotation_x(r, -0.4f);
        for (i = 0; i < 16; ++i) {
            CHECK_NEAR(m[i], r[i], 1e-5);
        }
        CHECK(pixelica_quat_from_axis_angle(&q, yaxis, 2.3f) == PIXELICA_OK);
        CHECK(pixelica_quat_to_mat4(m, q) == PIXELICA_OK);
        pixelica_mat4_rotation_y(r, 2.3f);
        for (i = 0; i < 16; ++i) {
            CHECK_NEAR(m[i], r[i], 1e-5);
        }
    }

    /* A non-unit axis/quaternion is normalized, not rejected. */
    {
        const PIXELICA_Vec3 longz = {0.0f, 0.0f, 12.5f};
        PIXELICA_Quat big;

        CHECK(pixelica_quat_from_axis_angle(&big, longz, 0.9f) == PIXELICA_OK);
        CHECK(pixelica_quat_from_axis_angle(&q, zaxis, 0.9f) == PIXELICA_OK);
        CHECK_NEAR(quat_dot(big, q), 1.0, 1e-6);
        big.x *= 7.0f;
        big.y *= 7.0f;
        big.z *= 7.0f;
        big.w *= 7.0f;
        CHECK(pixelica_quat_normalize(&big, big) == PIXELICA_OK);
        CHECK_NEAR(quat_dot(big, q), 1.0, 1e-6);
        CHECK(pixelica_quat_to_mat4(m, big) == PIXELICA_OK);
    }

    /* Invalid input leaves the output untouched. */
    keep.x = keep.y = keep.z = keep.w = 3.5f;
    q = keep;
    CHECK(pixelica_quat_normalize(&q, keep) == PIXELICA_OK); /* finite non-zero */
    q = keep;
    {
        PIXELICA_Quat bad = {0.0f, 0.0f, 0.0f, 0.0f};

        CHECK(pixelica_quat_normalize(&q, bad) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_quat_to_mat4(m, bad) == PIXELICA_ERR_INVALID_ARG);
        bad.w = pixelica_t_nan();
        CHECK(pixelica_quat_normalize(&q, bad) == PIXELICA_ERR_INVALID_ARG);
        CHECK(q.x == keep.x && q.w == keep.w);
    }
    CHECK(pixelica_quat_from_axis_angle(&q, zero, 1.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_quat_from_axis_angle(&q, zaxis, pixelica_t_inf()) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_quat_from_axis_angle(NULL, zaxis, 1.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(q.x == keep.x && q.w == keep.w);
}

static void test_quat_slerp(void)
{
    const PIXELICA_Vec3 zaxis = {0.0f, 0.0f, 1.0f};
    PIXELICA_Quat a, b, mid, expect, got;

    CHECK(pixelica_quat_from_axis_angle(&a, zaxis, 0.0f) == PIXELICA_OK);
    CHECK(pixelica_quat_from_axis_angle(&b, zaxis, 1.2f) == PIXELICA_OK);
    CHECK(pixelica_quat_from_axis_angle(&expect, zaxis, 0.6f) == PIXELICA_OK);

    /* Endpoints are exact (up to normalization). */
    CHECK(pixelica_quat_slerp(&got, a, b, 0.0f) == PIXELICA_OK);
    CHECK_NEAR(fabs(quat_dot(got, a)), 1.0, 1e-6);
    CHECK(pixelica_quat_slerp(&got, a, b, 1.0f) == PIXELICA_OK);
    CHECK_NEAR(fabs(quat_dot(got, b)), 1.0, 1e-6);

    /* The halfway point is the half angle, and t is clamped. */
    CHECK(pixelica_quat_slerp(&mid, a, b, 0.5f) == PIXELICA_OK);
    CHECK_NEAR(fabs(quat_dot(mid, expect)), 1.0, 1e-6);
    CHECK(pixelica_quat_slerp(&got, a, b, -5.0f) == PIXELICA_OK);
    CHECK_NEAR(fabs(quat_dot(got, a)), 1.0, 1e-6);
    CHECK(pixelica_quat_slerp(&got, a, b, 7.0f) == PIXELICA_OK);
    CHECK_NEAR(fabs(quat_dot(got, b)), 1.0, 1e-6);

    /* Results are unit length. */
    CHECK_NEAR(quat_dot(mid, mid), 1.0, 1e-6);

    /* Shortest arc: -b is the same rotation, so slerping to it must give the
     * same halfway rotation, not the long way round. */
    {
        PIXELICA_Quat nb = b;
        PIXELICA_Quat mid2;

        nb.x = -nb.x;
        nb.y = -nb.y;
        nb.z = -nb.z;
        nb.w = -nb.w;
        CHECK(pixelica_quat_slerp(&mid2, a, nb, 0.5f) == PIXELICA_OK);
        CHECK_NEAR(fabs(quat_dot(mid2, mid)), 1.0, 1e-6);
    }

    /* Nearly identical inputs take the linear fallback and stay unit. */
    {
        PIXELICA_Quat c;

        CHECK(pixelica_quat_from_axis_angle(&c, zaxis, 1.2000001f) == PIXELICA_OK);
        CHECK(pixelica_quat_slerp(&got, b, c, 0.37f) == PIXELICA_OK);
        CHECK_NEAR(quat_dot(got, got), 1.0, 1e-6);
    }

    /* A 180 degree turn still produces a unit result. */
    {
        PIXELICA_Quat half;

        CHECK(pixelica_quat_from_axis_angle(&half, zaxis, 3.14159265f) == PIXELICA_OK);
        CHECK(pixelica_quat_slerp(&got, a, half, 0.5f) == PIXELICA_OK);
        CHECK_NEAR(quat_dot(got, got), 1.0, 1e-5);
    }

    /* Invalid inputs change nothing. */
    {
        PIXELICA_Quat zero = {0.0f, 0.0f, 0.0f, 0.0f};

        got = a;
        CHECK(pixelica_quat_slerp(&got, zero, b, 0.5f) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_quat_slerp(&got, a, zero, 0.5f) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_quat_slerp(&got, a, b, pixelica_t_nan()) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_quat_slerp(NULL, a, b, 0.5f) == PIXELICA_ERR_INVALID_ARG);
        CHECK(got.x == a.x && got.w == a.w);
    }
}

int main(void)
{
    test_identity_and_layout();
    test_multiply();
    test_rotations();
    test_perspective();
    test_perspective_invalid();
    test_orthographic();
    test_orthographic_invalid();
    test_null_and_finite();
    test_normal_matrix();
    test_quat_basics();
    test_quat_slerp();
    return pixelica_t_finish("test_math");
}
