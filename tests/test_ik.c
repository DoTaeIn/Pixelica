/*
 * Pixelica - optional two-bone IK (modules/ik).
 *
 * Expectations come from the written contract (pixelica/ik.h), not from the
 * implementation: hand-solved poses, geometric invariants (bone lengths, end
 * constraint, bend side) checked in an independent double-precision harness,
 * and seeded random properties. CHECK() stays active under NDEBUG. The solver
 * allocates nothing, so nothing here creates a context.
 */
#include "pixelica/ik.h"
#include "test_util.h"

_Static_assert(sizeof(PIXELICA_IKTwoBone) == 11u * sizeof(float), "PIXELICA_IKTwoBone is 11 floats");

typedef struct D3 {
    double x, y, z;
} D3;

static uint32_t g_rng = 0x1357BDF1u;

static double rng_range(double lo, double hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((double)(g_rng >> 8) / 16777216.0);
}

static PIXELICA_Vec3 V(double x, double y, double z)
{
    PIXELICA_Vec3 v;

    v.x = (float)x;
    v.y = (float)y;
    v.z = (float)z;
    return v;
}

static D3 dv(PIXELICA_Vec3 v)
{
    D3 r;

    r.x = (double)v.x;
    r.y = (double)v.y;
    r.z = (double)v.z;
    return r;
}

static D3 dsub(D3 a, D3 b)
{
    D3 r;

    r.x = a.x - b.x;
    r.y = a.y - b.y;
    r.z = a.z - b.z;
    return r;
}

static D3 dmad(D3 a, D3 b, double s)
{
    D3 r;

    r.x = a.x + b.x * s;
    r.y = a.y + b.y * s;
    r.z = a.z + b.z * s;
    return r;
}

static double ddot(D3 a, D3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double dlen(D3 a)
{
    return sqrt(ddot(a, a));
}

static int bits_equal(float a, float b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

static PIXELICA_IKTwoBone chain(PIXELICA_Vec3 root, PIXELICA_Vec3 target, PIXELICA_Vec3 pole, double l0,
                           double l1)
{
    PIXELICA_IKTwoBone c;

    c.root = root;
    c.target = target;
    c.pole = pole;
    c.upper_length = (float)l0;
    c.lower_length = (float)l1;
    return c;
}

static double max_abs(double a, double b)
{
    return fabs(b) > a ? fabs(b) : a;
}

static double chain_scale(const PIXELICA_IKTwoBone *c)
{
    float f[11];
    double s = 0.0;
    int i;

    /* The _Static_assert above guarantees 11 packed floats, so copy into a real array. */
    memcpy(f, c, sizeof f);
    for (i = 0; i < 11; ++i) {
        s = max_abs(s, (double)f[i]);
    }
    return s;
}

/* Contract invariants any successful solve must satisfy. */
static void check_chain(const PIXELICA_IKTwoBone *c, const PIXELICA_IKTwoBoneResult *r)
{
    double tol = 2e-5 * chain_scale(c);
    D3 root = dv(c->root);
    D3 delta = dsub(dv(c->target), root);
    D3 jo = dsub(dv(r->joint), root);
    D3 p = dsub(dv(c->pole), root);
    double l0 = (double)c->upper_length;
    double l1 = (double)c->lower_length;
    double d = dlen(delta);
    double lo = fabs(l0 - l1);
    double hi = l0 + l1;
    double dc = d < lo ? lo : (d > hi ? hi : d);

    CHECK_NEAR(dlen(jo), l0, tol);
    CHECK_NEAR(dlen(dsub(dv(r->end), dv(r->joint))), l1, tol);
    CHECK(r->clamped == (d < lo || d > hi));
    if (!r->clamped) {
        CHECK(bits_equal(r->end.x, c->target.x) && bits_equal(r->end.y, c->target.y) &&
              bits_equal(r->end.z, c->target.z));
    } else {
        D3 eo = dsub(dv(r->end), root);

        CHECK_NEAR(dlen(eo), dc, tol);
        if (d > 0.0) {
            CHECK_NEAR(ddot(eo, delta) / d, dc, tol); /* end lies on the ray */
        }
    }
    if (d > 0.0) {
        /* Bend side: the joint's off-axis part points toward the pole's. */
        D3 f = delta;
        D3 jperp;
        D3 pperp;

        f.x /= d;
        f.y /= d;
        f.z /= d;
        jperp = dmad(jo, f, -ddot(jo, f));
        pperp = dmad(p, f, -ddot(p, f));
        if (dlen(jperp) > 1e-3 * chain_scale(c) && dlen(pperp) > 1e-6 * dlen(p)) {
            CHECK(ddot(jperp, pperp) > 0.99 * dlen(jperp) * dlen(pperp));
        }
    }
}

static PIXELICA_IKTwoBoneResult solve(const PIXELICA_IKTwoBone *c)
{
    PIXELICA_IKTwoBoneResult r;

    memset(&r, 0, sizeof r);
    CHECK_EQ_LONG(pixelica_ik_two_bone(c, &r), PIXELICA_OK);
    check_chain(c, &r);
    return r;
}

static void expect_pose(const PIXELICA_IKTwoBone *c, D3 joint, D3 end, int clamped, double tol)
{
    PIXELICA_IKTwoBoneResult r = solve(c);

    CHECK_NEAR(r.joint.x, joint.x, tol);
    CHECK_NEAR(r.joint.y, joint.y, tol);
    CHECK_NEAR(r.joint.z, joint.z, tol);
    CHECK_NEAR(r.end.x, end.x, tol);
    CHECK_NEAR(r.end.y, end.y, tol);
    CHECK_NEAR(r.end.z, end.z, tol);
    CHECK_EQ_LONG(r.clamped, clamped);
}

static D3 D(double x, double y, double z)
{
    D3 r;

    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

static void test_reachable_poses(void)
{
    PIXELICA_Vec3 o = V(0, 0, 0);
    PIXELICA_IKTwoBone c;

    /* 5+5 chain reaching 8 along X: a = 4, h = 3 (3-4-5 triangle). */
    c = chain(o, V(8, 0, 0), V(0, 10, 0), 5, 5);
    expect_pose(&c, D(4, 3, 0), D(8, 0, 0), 0, 1e-5);
    /* Pole flip in 3D: the knee follows the pole to every side. */
    c.pole = V(0, -10, 0);
    expect_pose(&c, D(4, -3, 0), D(8, 0, 0), 0, 1e-5);
    c.pole = V(0, 0, 10);
    expect_pose(&c, D(4, 0, 3), D(8, 0, 0), 0, 1e-5);
    c.pole = V(0, 0, -10);
    expect_pose(&c, D(4, 0, -3), D(8, 0, 0), 0, 1e-5);
    c.pole = V(30, 1, 0); /* only the off-axis part counts */
    expect_pose(&c, D(4, 3, 0), D(8, 0, 0), 0, 1e-5);

    /* Unequal lengths 3+4 at distance 5: a = 1.8, h = 2.4. */
    c = chain(o, V(5, 0, 0), V(0, 1, 0), 3, 4);
    expect_pose(&c, D(1.8, 2.4, 0), D(5, 0, 0), 0, 1e-5);

    /* Translated root and a diagonal axis: 2-3-6 has length 7, so a 4+4 chain
     * has a = 3.5 along the axis and h = sqrt(16 - 12.25) off it. */
    c = chain(V(1, 2, 3), V(3, 5, 9), V(1, 30, 3), 4, 4);
    {
        PIXELICA_IKTwoBoneResult r = solve(&c);
        D3 jo = dsub(dv(r.joint), D(1, 2, 3));

        CHECK_NEAR(ddot(jo, D(2, 3, 6)) / 7.0, 3.5, 1e-5);
        CHECK_NEAR(r.end.x, 3, 0);
        CHECK_NEAR(r.end.y, 5, 0);
        CHECK_NEAR(r.end.z, 9, 0);
    }

    /* Exactly at both reach limits is reachable (closed range). */
    c = chain(o, V(7, 0, 0), V(0, 1, 0), 3, 4);
    expect_pose(&c, D(3, 0, 0), D(7, 0, 0), 0, 1e-6);
    c = chain(o, V(1, 0, 0), V(0, 1, 0), 3, 4);
    expect_pose(&c, D(-3, 0, 0), D(1, 0, 0), 0, 1e-6);
}

static void test_clamping(void)
{
    PIXELICA_Vec3 o = V(0, 0, 0);
    PIXELICA_IKTwoBone c;

    /* Too far: straight arm toward the target, end at full reach. */
    c = chain(o, V(20, 0, 0), V(0, 1, 0), 3, 4);
    expect_pose(&c, D(3, 0, 0), D(7, 0, 0), 1, 1e-5);
    c = chain(V(1, 1, 1), V(1, -29, 1), V(5, 1, 1), 3, 4);
    expect_pose(&c, D(1, -2, 1), D(1, -6, 1), 1, 1e-5);
    /* Too near (inside |L0-L1|): folded, end at the minimum distance. */
    c = chain(o, V(1, 0, 0), V(0, 5, 0), 4, 1);
    expect_pose(&c, D(4, 0, 0), D(3, 0, 0), 1, 1e-5);
    c = chain(o, V(0, 0.5f, 0), V(5, 0, 0), 1, 4);
    expect_pose(&c, D(0, -1, 0), D(0, 3, 0), 1, 1e-5); /* a = -L0: knee behind the root */
}

static void test_root_targets(void)
{
    PIXELICA_Vec3 o = V(0, 0, 0);
    PIXELICA_IKTwoBone c;

    /* Equal lengths, target on the root: fully folded, joint toward the pole. */
    c = chain(V(1, 2, 3), V(1, 2, 3), V(1, 7, 3), 2, 2);
    expect_pose(&c, D(1, 4, 3), D(1, 2, 3), 0, 1e-5);
    /* Same with no usable pole: forward = +X, least-parallel axis is Y. */
    c = chain(o, o, o, 2, 2);
    expect_pose(&c, D(0, 2, 0), D(0, 0, 0), 0, 1e-5);
    /* Unequal lengths: forward = +X, end at |L0-L1| (clamped), a = L0. */
    c = chain(o, o, V(0, 0, 5), 3, 1);
    expect_pose(&c, D(3, 0, 0), D(2, 0, 0), 1, 1e-5);
    c = chain(o, o, V(0, 0, 5), 1, 3);
    expect_pose(&c, D(-1, 0, 0), D(2, 0, 0), 1, 1e-5);
}

/* root = 0, L0 = L1 = 10; the joint must leave the axis toward coordinate axis
 * `axis` (0..2): coplanar with {forward, axis} and on its positive side. */
static void check_fallback(PIXELICA_Vec3 target, PIXELICA_Vec3 pole, int axis)
{
    PIXELICA_IKTwoBone c = chain(V(0, 0, 0), target, pole, 10, 10);
    PIXELICA_IKTwoBoneResult r = solve(&c);
    D3 f = dv(target);
    D3 j = dv(r.joint);
    D3 u = D(axis == 0, axis == 1, axis == 2);
    double triple = ddot(j, D(f.y * u.z - f.z * u.y, f.z * u.x - f.x * u.z,
                              f.x * u.y - f.y * u.x));

    CHECK_NEAR(triple, 0.0, 1e-4 * dlen(f));
    CHECK(ddot(j, u) > 1.0);
}

static void test_pole_fallback(void)
{
    PIXELICA_Vec3 o = V(0, 0, 0);

    check_fallback(V(4, 0, 0), V(9, 0, 0), 1);   /* collinear, ahead */
    check_fallback(V(4, 0, 0), V(-9, 0, 0), 1);  /* collinear, behind */
    check_fallback(V(4, 0, 0), o, 1);            /* pole == root */
    check_fallback(V(0, 4, 0), V(0, 8, 0), 0);   /* X/Z tie -> X */
    check_fallback(V(0, 0, 4), V(0, 0, 1), 0);   /* X/Y tie -> X */
    check_fallback(V(3, 2, 1), V(6, 4, 2), 2);
    check_fallback(V(1, 3, 2), V(2, 6, 4), 0);
    check_fallback(V(3, 1, 2), V(-3, -1, -2), 1);

    /* A small but genuine off-axis pole is honored, not replaced. */
    {
        PIXELICA_IKTwoBone c = chain(o, V(4, 0, 0), V(9, 0, -1e-3), 3, 3);
        PIXELICA_IKTwoBoneResult r = solve(&c);

        CHECK(r.joint.z < -1.0f && fabsf(r.joint.y) < 1e-3f);
        c.pole = V(9, 0, 1e-6);
        r = solve(&c);
        CHECK(r.joint.z > 1.0f && fabsf(r.joint.y) < 1e-3f);
        /* Below the 1e-12 relative threshold the Y fallback is used. */
        c.pole = V(9e6, 0, 1e-12);
        r = solve(&c);
        CHECK(r.joint.y > 1.0f && fabsf(r.joint.z) < 1e-3f);
    }
}

static void test_magnitudes(void)
{
    static const double k_scales[5] = {1e-30, 1e-10, 1.0, 1e10, 1e38};
    int i;

    for (i = 0; i < 5; ++i) {
        double s = k_scales[i];
        PIXELICA_IKTwoBone c = chain(V(0, 0, 0), V(1.5 * s, 0, 0), V(0, s, 0), s, s);
        double l = (double)c.upper_length;
        double a = 0.5 * (double)c.target.x; /* equal lengths: joint over the midpoint */
        double h = sqrt((l - a) * (l + a));

        expect_pose(&c, D(a, h, 0), dv(c.target), 0, 2e-5 * s);
    }

    /* Whole float range: delta (6e38) exceeds FLT_MAX but the solve is fine. */
    {
        PIXELICA_IKTwoBone c = chain(V(-3e38, 0, 0), V(3e38, 0, 0), V(-3e38, 1, 0), 3.1e38, 3.1e38);
        double l = (double)c.upper_length;
        double a = 0.5 * 2.0 * (double)c.target.x;

        expect_pose(&c, D(0, sqrt((l - a) * (l + a)), 0), D(3e38, 0, 0), 0, 1e33);
    }
    /* A joint beyond FLT_MAX is unrepresentable. */
    {
        PIXELICA_IKTwoBone c = chain(V(3e38, 0, 0), V(3e38, 1e38, 0), V(3.4e38, 0, 0), 1e38, 1e38);
        PIXELICA_IKTwoBoneResult r;
        PIXELICA_IKTwoBoneResult before;

        memset(&r, 0xA5, sizeof r);
        memcpy(&before, &r, sizeof r);
        CHECK_EQ_LONG(pixelica_ik_two_bone(&c, &r), PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&r, &before, sizeof r) == 0);
    }
}

static void check_rejected(const PIXELICA_IKTwoBone *c)
{
    PIXELICA_IKTwoBoneResult r;
    PIXELICA_IKTwoBoneResult before;

    memset(&r, 0xA5, sizeof r);
    memcpy(&before, &r, sizeof r);
    CHECK_EQ_LONG(pixelica_ik_two_bone(c, &r), PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&r, &before, sizeof r) == 0);
}

static void test_invalid_and_alias(void)
{
    PIXELICA_IKTwoBone good = chain(V(0, 0, 0), V(8, 0, 0), V(0, 10, 0), 5, 5);
    PIXELICA_IKTwoBoneResult ref = solve(&good);
    PIXELICA_IKTwoBoneResult r;
    float bad[3];
    float raw[11];
    int i;
    int k;

    bad[0] = pixelica_t_nan();
    bad[1] = pixelica_t_inf();
    bad[2] = -pixelica_t_inf();
    for (i = 0; i < 11; ++i) {
        for (k = 0; k < 3; ++k) {
            PIXELICA_IKTwoBone c;

            memcpy(raw, &good, sizeof raw);
            raw[i] = bad[k];
            memcpy(&c, raw, sizeof c);
            check_rejected(&c);
        }
    }
    for (k = 0; k < 3; ++k) {
        static const float k_bad_len[3] = {0.0f, -1.0f, -0.0f};
        PIXELICA_IKTwoBone c = good;

        c.upper_length = k_bad_len[k];
        check_rejected(&c);
        c = good;
        c.lower_length = k_bad_len[k];
        check_rejected(&c);
    }
    memset(&r, 0xA5, sizeof r);
    CHECK_EQ_LONG(pixelica_ik_two_bone(NULL, &r), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_ik_two_bone(&good, NULL), PIXELICA_ERR_INVALID_ARG);

    /* Output overlapping the input chain gives the same answer, valid or not. */
    {
        union {
            PIXELICA_IKTwoBone c;
            PIXELICA_IKTwoBoneResult r;
        } u;

        u.c = good;
        CHECK_EQ_LONG(pixelica_ik_two_bone(&u.c, &u.r), PIXELICA_OK);
        CHECK(memcmp(&u.r.joint, &ref.joint, sizeof ref.joint) == 0);
        CHECK(memcmp(&u.r.end, &ref.end, sizeof ref.end) == 0);
        CHECK_EQ_LONG(u.r.clamped, ref.clamped);

        u.c = good;
        u.c.pole.y = bad[0];
        memcpy(raw, &u.c, sizeof raw);
        CHECK_EQ_LONG(pixelica_ik_two_bone(&u.c, &u.r), PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(raw, &u.c, sizeof raw) == 0);
    }
}

static D3 random_offset(double lo, double hi)
{
    return D(rng_range(lo, hi), rng_range(lo, hi), rng_range(lo, hi));
}

static void test_random(void)
{
    int clamped_far = 0;
    int clamped_near = 0;
    int reachable = 0;
    int i;

    for (i = 0; i < 4000; ++i) {
        D3 root = random_offset(-100, 100);
        D3 dir = random_offset(-1, 1);
        double l0 = rng_range(0.5, 8.0);
        double l1 = rng_range(0.5, 8.0);
        double dn = dlen(dir);
        double want = rng_range(0.0, 25.0);
        PIXELICA_IKTwoBone c;
        PIXELICA_IKTwoBoneResult r;
        PIXELICA_IKTwoBoneResult again;
        double d;
        double lo;
        double hi;
        D3 pole_off;

        if (dn < 0.1) {
            continue;
        }
        c = chain(V(root.x, root.y, root.z),
                  V(root.x + dir.x / dn * want, root.y + dir.y / dn * want,
                    root.z + dir.z / dn * want),
                  V(0, 0, 0), l0, l1);
        pole_off = random_offset(-50, 50);
        c.pole = V(root.x + pole_off.x, root.y + pole_off.y, root.z + pole_off.z);

        d = dlen(dsub(dv(c.target), dv(c.root)));
        lo = fabs((double)c.upper_length - (double)c.lower_length);
        hi = (double)c.upper_length + (double)c.lower_length;
        if (fabs(d - lo) < 1e-3 || fabs(d - hi) < 1e-3) {
            continue; /* the clamped flag is ambiguous at float resolution */
        }
        if (d < lo) {
            ++clamped_near;
        } else if (d > hi) {
            ++clamped_far;
        } else {
            ++reachable;
        }

        r = solve(&c);
        CHECK_EQ_LONG(pixelica_ik_two_bone(&c, &again), PIXELICA_OK);
        CHECK(memcmp(&r.joint, &again.joint, sizeof r.joint) == 0);

        /* Sliding the pole along the axis or scaling its offset must not
         * change the plane/side, as long as its off-axis part is sizable. */
        {
            D3 f = dsub(dv(c.target), dv(c.root));
            D3 p = dsub(dv(c.pole), dv(c.root));
            D3 pperp = dmad(p, f, -ddot(p, f) / (d * d));

            if (dlen(pperp) > 5.0) {
                PIXELICA_IKTwoBone c2 = c;
                PIXELICA_IKTwoBoneResult r2;

                c2.pole = V(root.x + 3.0 * p.x + 0.7 * f.x, root.y + 3.0 * p.y + 0.7 * f.y,
                            root.z + 3.0 * p.z + 0.7 * f.z);
                r2 = solve(&c2);
                CHECK_NEAR(r2.joint.x, r.joint.x, 3e-4);
                CHECK_NEAR(r2.joint.y, r.joint.y, 3e-4);
                CHECK_NEAR(r2.joint.z, r.joint.z, 3e-4);
            }
        }
    }
    CHECK(clamped_far > 100);
    CHECK(clamped_near > 100);
    CHECK(reachable > 500);
}

int main(void)
{
    test_reachable_poses();
    test_clamping();
    test_root_targets();
    test_pole_fallback();
    test_magnitudes();
    test_invalid_and_alias();
    test_random();
    return pixelica_t_finish("test_ik");
}
