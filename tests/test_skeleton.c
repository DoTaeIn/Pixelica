/*
 * Optional skeleton module (modules/skeleton): pose, palette and positions-only
 * linear blend skinning. Built and registered only with PIXELICA_BUILD_SKELETON=ON;
 * it does not use the PIXELICA_Context. CHECK never uses assert(), so the test stays
 * active in Release.
 *
 * Matrices are column-major float[16] acting on column vectors (math.h).
 * "unchanged" means byte-for-byte: outputs are filled with 0xA5 first and
 * compared with memcmp after the call over the WHOLE array, so overruns show up
 * too. Zero compares use CHECK_NEAR with tolerance 0, which treats -0 == +0.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test_util.h"

#include "pixelica/skeleton.h"

#define MAXB 16u
#define MAXV 48u
#define HALF_PI 1.57079632679f

/* const float (*)[16] cannot be formed implicitly from float (*)[16] in C11. */
#define CM(p) ((const float (*)[16])(const void *)(p))
#define BAD PIXELICA_ERR_INVALID_ARG
#define NOMEM PIXELICA_ERR_NO_MEMORY

typedef struct PoseBuf {
    float world[MAXB][16];
    float palette[MAXB][16];
    float scratch[2u * MAXB + 4u][16];
} PoseBuf;

typedef struct Rig {
    PIXELICA_Bone bones[MAXB];
    float local[MAXB][16];
    PIXELICA_Skeleton sk;
} Rig;

static void rig_init(Rig *r)
{
    memset(r, 0, sizeof(*r));
    r->sk.bones = r->bones;
}

static void rig_add(Rig *r, int32_t parent, const float local[16], const float inverse_bind[16])
{
    uint32_t i = r->sk.bone_count++;

    r->bones[i].parent = parent;
    memcpy(r->bones[i].inverse_bind, inverse_bind, sizeof(r->bones[i].inverse_bind));
    memcpy(r->local[i], local, sizeof(r->local[i]));
}

static PIXELICA_Status pose(const Rig *r, PoseBuf *b)
{
    return pixelica_skeleton_pose(&r->sk, CM(r->local), b->world, b->palette, b->scratch,
                             2u * (size_t)r->sk.bone_count);
}

/* pose() must return `expected` and leave world/palette byte-for-byte unchanged. */
static void check_pose_fails(const Rig *r, PIXELICA_Status expected)
{
    PoseBuf b;
    PoseBuf before;

    memset(&b, 0xA5, sizeof(b));
    before = b;
    CHECK_EQ_LONG(pose(r, &b), expected);
    CHECK(memcmp(b.world, before.world, sizeof(b.world)) == 0);
    CHECK(memcmp(b.palette, before.palette, sizeof(b.palette)) == 0);
}

static void rigid(float m[16], float rx, float ry, float rz, float tx, float ty, float tz)
{
    float a[16];
    float b[16];
    float c[16];

    pixelica_mat4_rotation_x(a, rx);
    pixelica_mat4_rotation_y(b, ry);
    pixelica_mat4_multiply(c, b, a);
    pixelica_mat4_rotation_z(a, rz);
    pixelica_mat4_multiply(b, a, c);
    pixelica_mat4_translation(a, tx, ty, tz);
    pixelica_mat4_multiply(m, a, b);
}

/* Inverse of a rotation + translation matrix. */
static void rigid_inverse(float out[16], const float m[16])
{
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) {
            out[c * 4 + r] = m[r * 4 + c];
        }
        out[c * 4 + 3] = 0.0f;
    }
    for (int i = 0; i < 3; ++i) {
        out[12 + i] = -(out[i] * m[12] + out[4 + i] * m[13] + out[8 + i] * m[14]);
    }
    out[15] = 1.0f;
}

static void check_translation(const float m[16], float x, float y, float z, double tol)
{
    CHECK_NEAR(m[12], x, tol);
    CHECK_NEAR(m[13], y, tol);
    CHECK_NEAR(m[14], z, tol);
}

static void check_matrix_near(const float a[16], const float b[16], double tol)
{
    for (int i = 0; i < 16; ++i) {
        CHECK_NEAR(a[i], b[i], tol);
    }
}

static PIXELICA_SkinWeight skin_weight(uint16_t b0, float w0, uint16_t b1, float w1, uint16_t b2,
                                  float w2, uint16_t b3, float w3)
{
    PIXELICA_SkinWeight w;

    w.bones[0] = b0;
    w.bones[1] = b1;
    w.bones[2] = b2;
    w.bones[3] = b3;
    w.weights[0] = w0;
    w.weights[1] = w1;
    w.weights[2] = w2;
    w.weights[3] = w3;
    return w;
}

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* Skins with a poisoned MAXV-vertex workspace. */
static PIXELICA_Status skin_into(const PIXELICA_Vec3 *bind, const PIXELICA_SkinWeight *w, size_t n,
                            const float (*palette)[16], uint32_t bones, PIXELICA_Vec3 *out)
{
    PIXELICA_Vec3 scratch[MAXV];

    memset(scratch, 0xA5, sizeof(scratch));
    return pixelica_skeleton_skin(bind, w, n, palette, bones, out, scratch, MAXV);
}

/* skin_into must return `expected` and leave `out` byte-for-byte unchanged. */
static void check_skin_fails(const PIXELICA_Vec3 *bind, const PIXELICA_SkinWeight *w, size_t n,
                             const float (*palette)[16], uint32_t bones, PIXELICA_Status expected)
{
    PIXELICA_Vec3 out[MAXV];
    PIXELICA_Vec3 before[MAXV];

    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(out));
    CHECK_EQ_LONG(skin_into(bind, w, n, palette, bones, out), expected);
    CHECK(memcmp(out, before, sizeof(out)) == 0);
}

/* ------------------------------------------------------------------------ */
/* Pose                                                                      */
/* ------------------------------------------------------------------------ */

/* A skeleton posed exactly in its bind pose has identity palettes. */
static void test_pose_bind_is_identity(void)
{
    Rig r;
    PoseBuf b;
    PoseBuf again;
    float step[16];
    float inverse[16];
    float identity[16];

    rig_init(&r);
    pixelica_mat4_identity(identity);
    pixelica_mat4_translation(step, 1.0f, 0.0f, 0.0f);
    rig_add(&r, -1, identity, identity);
    pixelica_mat4_translation(inverse, -1.0f, 0.0f, 0.0f);
    rig_add(&r, 0, step, inverse);
    pixelica_mat4_translation(inverse, -2.0f, 0.0f, 0.0f);
    rig_add(&r, 1, step, inverse);

    memset(&b, 0xA5, sizeof(b));
    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
    check_translation(b.world[1], 1.0f, 0.0f, 0.0f, 0.0);
    check_translation(b.world[2], 2.0f, 0.0f, 0.0f, 0.0);
    for (unsigned i = 0; i < 3u; ++i) {
        check_matrix_near(b.palette[i], identity, 0.0);
    }

    /* Identical input gives bitwise identical output, whatever the old contents. */
    memset(&again, 0x11, sizeof(again));
    CHECK_EQ_LONG(pose(&r, &again), PIXELICA_OK);
    CHECK(memcmp(again.world, b.world, 3u * sizeof(b.world[0])) == 0);
    CHECK(memcmp(again.palette, b.palette, 3u * sizeof(b.palette[0])) == 0);

    /* A single bone is the smallest valid skeleton: world = local. */
    r.sk.bone_count = 1u;
    pixelica_mat4_translation(r.local[0], 3.0f, -4.0f, 5.0f);
    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
    check_matrix_near(b.world[0], r.local[0], 0.0);
}

/* world = parent_world * local, with rotations, translations and several roots. */
static void test_pose_hierarchy(void)
{
    Rig r;
    PoseBuf b;
    float m[16];
    float identity[16];
    float expect[16];

    rig_init(&r);
    pixelica_mat4_identity(identity);
    pixelica_mat4_rotation_z(m, HALF_PI);
    rig_add(&r, -1, m, identity); /* 0: root turned 90 degrees about Z */
    pixelica_mat4_translation(m, 2.0f, 0.0f, 0.0f);
    rig_add(&r, 0, m, identity); /* 1: 2 along the parent's X, i.e. world +Y */
    pixelica_mat4_translation(m, 5.0f, 0.0f, 0.0f);
    rig_add(&r, -1, m, identity); /* 2: second root, ignores bone 0 */
    pixelica_mat4_translation(m, 0.0f, 0.0f, 3.0f);
    rig_add(&r, 2, m, identity); /* 3: child of the second root */
    pixelica_mat4_rotation_x(m, HALF_PI);
    rig_add(&r, 1, m, identity); /* 4: grandchild, turned about X */

    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
    check_matrix_near(b.world[0], r.local[0], 0.0);
    check_translation(b.world[1], 0.0f, 2.0f, 0.0f, 1e-6);
    check_translation(b.world[2], 5.0f, 0.0f, 0.0f, 0.0);
    check_translation(b.world[3], 5.0f, 0.0f, 3.0f, 0.0);
    pixelica_mat4_multiply(expect, b.world[1], r.local[4]);
    check_matrix_near(b.world[4], expect, 1e-6);
    /* With identity inverse_bind the palette equals the world matrices. */
    for (unsigned i = 0; i < 5u; ++i) {
        check_matrix_near(b.palette[i], b.world[i], 0.0);
    }
    /* Bottom rows are exactly (0,0,0,1); a -0 in the input is accepted, written as +0. */
    CHECK(b.world[4][3] == 0.0f && b.world[4][7] == 0.0f && b.world[4][11] == 0.0f &&
          b.world[4][15] == 1.0f);
    r.local[3][3] = -0.0f;
    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
    CHECK(!signbit(b.world[3][3]));
}

/* palette = world * inverse_bind carries a bind-pose point with its bone. */
static void test_pose_palette_and_skin_rigid(void)
{
    Rig r;
    PoseBuf b;
    float rot[16];
    float shift[16];
    float local[16];
    float inverse[16];
    float identity[16];
    PIXELICA_Vec3 bind = vec3(2.0f, 0.0f, 0.0f);
    PIXELICA_SkinWeight w = skin_weight(1, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
    PIXELICA_Vec3 out = vec3(0.0f, 0.0f, 0.0f);

    rig_init(&r);
    pixelica_mat4_identity(identity);
    rig_add(&r, -1, identity, identity);
    /* Bone 1 sits at (1,0,0) in the bind pose; posed, it also turns 90 degrees about Z. */
    pixelica_mat4_rotation_z(rot, HALF_PI);
    pixelica_mat4_translation(shift, 1.0f, 0.0f, 0.0f);
    pixelica_mat4_multiply(local, rot, shift);
    pixelica_mat4_translation(inverse, -1.0f, 0.0f, 0.0f);
    rig_add(&r, 0, local, inverse);

    /* palette[1] = Rz * T(1) * T(-1) = Rz, so the bind point (2,0,0) goes to (0,2,0). */
    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
    check_matrix_near(b.palette[1], rot, 1e-7);
    CHECK_EQ_LONG(skin_into(&bind, &w, 1u, CM(b.palette), 2u, &out), PIXELICA_OK);
    CHECK_NEAR(out.x, 0.0, 1e-6);
    CHECK_NEAR(out.y, 2.0, 1e-6);
    CHECK_NEAR(out.z, 0.0, 1e-6);
}

/* ------------------------------------------------------------------------ */
/* Skinning                                                                  */
/* ------------------------------------------------------------------------ */

static void test_skin_blend(void)
{
    float pal[4][16];
    PIXELICA_Vec3 bind[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}};
    PIXELICA_SkinWeight w[3];
    PIXELICA_Vec3 out[3];

    pixelica_mat4_identity(pal[0]);
    pixelica_mat4_rotation_z(pal[1], HALF_PI);
    pixelica_mat4_translation(pal[2], 0.0f, 4.0f, 0.0f);
    pixelica_mat4_translation(pal[3], 0.0f, 0.0f, 4.0f);

    /* Two bones: halfway between (1,0,0) and Rz(90)(1,0,0) = (0,1,0). */
    w[0] = skin_weight(0, 0.5f, 1, 0.5f, 0, 0.0f, 0, 0.0f);
    /* Four influences on the origin: bones 0 and 1 keep it, 2 and 3 move it. */
    w[1] = skin_weight(0, 0.1f, 1, 0.2f, 2, 0.3f, 3, 0.4f);
    /* Weight 1 on one bone is exactly that bone; zero-weight slots may name any bone. */
    w[2] = skin_weight(2, 1.0f, 65535, 0.0f, 0, 0.0f, 9, 0.0f);
    CHECK_EQ_LONG(skin_into(bind, w, 3u, CM(pal), 4u, out), PIXELICA_OK);
    CHECK_NEAR(out[0].x, 0.5, 1e-6);
    CHECK_NEAR(out[0].y, 0.5, 1e-6);
    CHECK_NEAR(out[0].z, 0.0, 1e-6);
    CHECK_NEAR(out[1].x, 0.0, 1e-6);
    CHECK_NEAR(out[1].y, 0.3 * 4.0, 1e-6);
    CHECK_NEAR(out[1].z, 0.4 * 4.0, 1e-6);
    CHECK_NEAR(out[2].x, 1.0, 0.0);
    CHECK_NEAR(out[2].y, 6.0, 0.0);
    CHECK_NEAR(out[2].z, 3.0, 0.0);

    /* A vertex whose influences all agree stays where that bone puts it. */
    w[0] = skin_weight(2, 0.25f, 2, 0.25f, 2, 0.25f, 2, 0.25f);
    CHECK_EQ_LONG(skin_into(bind, w, 1u, CM(pal), 4u, out), PIXELICA_OK);
    CHECK_NEAR(out[0].x, 1.0, 1e-7);
    CHECK_NEAR(out[0].y, 4.0, 1e-7);
    CHECK_NEAR(out[0].z, 0.0, 1e-7);
}

/* Weights within 1e-4 of 1 are renormalized by their actual sum. */
static void test_skin_normalization(void)
{
    float pal[2][16];
    PIXELICA_Vec3 bind = vec3(1.0f, 1.0f, 1.0f);
    PIXELICA_SkinWeight w = skin_weight(0, 0.50003f, 1, 0.50002f, 0, 0.0f, 0, 0.0f);
    PIXELICA_Vec3 out;

    /* Both bones are the same big translation: without normalization the result
     * would be off by (sum - 1) * 1000 = 0.05 or more. */
    pixelica_mat4_translation(pal[0], 1000.0f, 2000.0f, 3000.0f);
    pixelica_mat4_translation(pal[1], 1000.0f, 2000.0f, 3000.0f);
    CHECK_EQ_LONG(skin_into(&bind, &w, 1u, CM(pal), 2u, &out), PIXELICA_OK);
    CHECK_NEAR(out.x, 1001.0, 1e-3);
    CHECK_NEAR(out.y, 2001.0, 1e-3);
    CHECK_NEAR(out.z, 3001.0, 1e-3);

    /* Outside the tolerance, or without any positive weight, it is rejected. */
    w = skin_weight(0, 0.5005f, 1, 0.5f, 0, 0.0f, 0, 0.0f);
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(0, 0.4995f, 1, 0.5f, 0, 0.0f, 0, 0.0f);
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(0, 0.0f, 1, 0.0f, 0, 0.0f, 0, 0.0f);
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
}

/* Every slot is validated, zero-weight slots included; bone indices only when used. */
static void test_skin_weight_validation(void)
{
    float pal[2][16];
    PIXELICA_Vec3 bind = vec3(0.0f, 0.0f, 0.0f);
    PIXELICA_SkinWeight w;
    PIXELICA_Vec3 out;
    float not_a_number = pixelica_t_nan();
    float infinity = pixelica_t_inf();

    pixelica_mat4_identity(pal[0]);
    pixelica_mat4_identity(pal[1]);

    w = skin_weight(0, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
    CHECK_EQ_LONG(skin_into(&bind, &w, 1u, CM(pal), 2u, &out), PIXELICA_OK);
    w = skin_weight(0, 1.5f, 1, -0.5f, 0, 0.0f, 0, 0.0f); /* sums to 1 but out of range */
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(0, 1.0f, 0, not_a_number, 0, 0.0f, 0, 0.0f);
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(0, 1.0f, 0, 0.0f, 0, 0.0f, 0, infinity);
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(2, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f); /* bone 2 >= bone_count */
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
    w = skin_weight(0, 0.5f, 0, 0.0f, 0, 0.0f, 7, 0.5f); /* positive weight, bad bone */
    check_skin_fails(&bind, &w, 1u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
}

/* In-place skinning (out == bind) equals out-of-place; partial overlap is rejected. */
static void test_skin_inplace_and_overlap(void)
{
    float pal[2][16];
    PIXELICA_Vec3 bind[4] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    PIXELICA_Vec3 copy[4];
    PIXELICA_Vec3 expect[4];
    PIXELICA_Vec3 scratch[4];
    PIXELICA_SkinWeight w[4];
    PIXELICA_Vec3 big[8];
    PIXELICA_Vec3 before[8];

    pixelica_mat4_rotation_y(pal[0], 0.7f);
    pixelica_mat4_translation(pal[1], 3.0f, -1.0f, 2.0f);
    for (int i = 0; i < 4; ++i) {
        w[i] = skin_weight(0, 0.25f * (float)i, 1, 1.0f - 0.25f * (float)i, 0, 0.0f, 0, 0.0f);
    }
    memcpy(copy, bind, sizeof(bind));
    CHECK_EQ_LONG(skin_into(bind, w, 4u, CM(pal), 2u, expect), PIXELICA_OK);
    CHECK(memcmp(expect, bind, sizeof(bind)) != 0);

    CHECK_EQ_LONG(pixelica_skeleton_skin(copy, w, 4u, CM(pal), 2u, copy, scratch, 4u), PIXELICA_OK);
    CHECK(memcmp(copy, expect, sizeof(copy)) == 0);

    /* The output shifted by one vertex over the bind array, and the reverse. */
    memcpy(big, bind, sizeof(bind));
    memset(&big[4], 0xA5, sizeof(big) - sizeof(bind));
    memcpy(before, big, sizeof(big));
    CHECK_EQ_LONG(pixelica_skeleton_skin(big, w, 4u, CM(pal), 2u, big + 1, scratch, 4u),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(big, before, sizeof(big)) == 0);
    CHECK_EQ_LONG(pixelica_skeleton_skin(big + 1, w, 4u, CM(pal), 2u, big, scratch, 4u),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(big, before, sizeof(big)) == 0);
}

/* ------------------------------------------------------------------------ */
/* Validation                                                                */
/* ------------------------------------------------------------------------ */

static void test_pose_parent_validation(void)
{
    Rig r;
    PoseBuf b;
    float identity[16];
    /* Parents of bone 2 that are not -1, 0 or 1: unknown, self, forward, huge. */
    const int32_t bad[] = {-2, 2, 3, 100, INT32_MIN, INT32_MAX};

    pixelica_mat4_identity(identity);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        rig_init(&r);
        rig_add(&r, -1, identity, identity);
        rig_add(&r, 0, identity, identity);
        rig_add(&r, bad[i], identity, identity);
        check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);
    }

    /* A forward reference that closes a two-bone cycle (0 -> 1 -> 0). */
    rig_init(&r);
    rig_add(&r, 1, identity, identity);
    rig_add(&r, 0, identity, identity);
    check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);
    /* A self parent on the very first bone. */
    rig_init(&r);
    rig_add(&r, 0, identity, identity);
    check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);

    /* A valid forest: two roots, each with a child, children listed out of root order. */
    rig_init(&r);
    rig_add(&r, -1, identity, identity);
    rig_add(&r, -1, identity, identity);
    rig_add(&r, 1, identity, identity);
    rig_add(&r, 0, identity, identity);
    CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
}

/* Non-finite / non-affine data in the LAST bone leaves world and palette intact. */
static void test_matrix_validation(void)
{
    const int elem[8] = {12, 5, 0, 3, 7, 11, 15, 15};
    float value[8];
    float identity[16];
    Rig r;

    value[0] = pixelica_t_nan();
    value[1] = pixelica_t_inf();
    value[2] = -pixelica_t_inf();
    value[3] = 0.25f; /* non-zero bottom row entries */
    value[4] = 0.25f;
    value[5] = 0.25f;
    value[6] = 0.5f; /* element 15 must be exactly 1 */
    value[7] = 0.0f;
    pixelica_mat4_identity(identity);

    for (int field = 0; field < 2; ++field) { /* 0 = local matrix, 1 = inverse_bind */
        for (int c = 0; c < 8; ++c) {
            rig_init(&r);
            for (int i = 0; i < 4; ++i) {
                rig_add(&r, i - 1, identity, identity);
            }
            if (field == 0) {
                r.local[3][elem[c]] = value[c];
            } else {
                r.bones[3].inverse_bind[elem[c]] = value[c];
            }
            check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);
        }
    }

    /* An invalid matrix in an UNUSED palette entry still rejects the whole skin call. */
    {
        float pal[3][16];
        PIXELICA_Vec3 bind = vec3(1.0f, 2.0f, 3.0f);
        PIXELICA_SkinWeight w = skin_weight(0, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
        PIXELICA_Vec3 binds[3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        PIXELICA_SkinWeight ws[3];
        PIXELICA_Vec3 out[3];

        pixelica_mat4_identity(pal[0]);
        pixelica_mat4_identity(pal[1]);
        pixelica_mat4_identity(pal[2]);
        pal[2][15] = 2.0f;
        check_skin_fails(&bind, &w, 1u, CM(pal), 3u, PIXELICA_ERR_INVALID_ARG);
        pal[2][15] = 1.0f;
        pal[2][12] = pixelica_t_nan();
        check_skin_fails(&bind, &w, 1u, CM(pal), 3u, PIXELICA_ERR_INVALID_ARG);
        pal[2][12] = 0.0f;
        CHECK_EQ_LONG(skin_into(&bind, &w, 1u, CM(pal), 3u, out), PIXELICA_OK);

        /* A non-finite bind position in the LAST vertex only. */
        ws[0] = w;
        ws[1] = w;
        ws[2] = w;
        binds[2].y = pixelica_t_inf();
        check_skin_fails(binds, ws, 3u, CM(pal), 3u, PIXELICA_ERR_INVALID_ARG);
        binds[2].y = 1.0f;
        CHECK_EQ_LONG(skin_into(binds, ws, 3u, CM(pal), 3u, out), PIXELICA_OK);
    }
}

/* Results that do not fit in float are rejected and nothing is committed. */
static void test_overflow(void)
{
    Rig r;
    float identity[16];
    float huge[16];
    float pal[2][16];
    PIXELICA_Vec3 bind[3] = {{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1e30f, 0.0f, 0.0f}};
    PIXELICA_SkinWeight w[3];

    pixelica_mat4_identity(identity);
    pixelica_mat4_scale(huge, 1e30f, 1e30f, 1e30f);

    /* World chain: 1e30 * 1e30 overflows in the LAST bone. */
    rig_init(&r);
    rig_add(&r, -1, huge, identity);
    rig_add(&r, 0, identity, identity);
    rig_add(&r, 1, huge, identity);
    check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);

    /* Palette: a finite world matrix times a huge inverse_bind overflows. */
    rig_init(&r);
    rig_add(&r, -1, identity, identity);
    rig_add(&r, 0, huge, huge);
    check_pose_fails(&r, PIXELICA_ERR_INVALID_ARG);

    /* Skin: the third vertex maps 1e30 through a 1e30 scale. Earlier ones are fine. */
    memcpy(pal[0], huge, sizeof(huge));
    pixelica_mat4_identity(pal[1]);
    w[0] = skin_weight(1, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
    w[1] = w[0];
    w[2] = skin_weight(0, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
    check_skin_fails(bind, w, 3u, CM(pal), 2u, PIXELICA_ERR_INVALID_ARG);
}

static void test_arguments_and_scratch(void)
{
    Rig r;
    PoseBuf b;
    PoseBuf before;
    float identity[16];
    float pal[1][16];
    PIXELICA_Vec3 bind = vec3(0.0f, 0.0f, 0.0f);
    PIXELICA_SkinWeight w = skin_weight(0, 1.0f, 0, 0.0f, 0, 0.0f, 0, 0.0f);
    PIXELICA_Vec3 out[2];
    PIXELICA_Vec3 scratch[2];
    PIXELICA_Skeleton no_bones = {NULL, 2u};
    PIXELICA_Skeleton zero;
    PIXELICA_Skeleton too_many;
    /* Short names keep the many one-line argument variations readable. */
    const float (*L)[16] = CM(r.local);
    const float (*K)[16] = CM(pal);
    float (*W)[16] = b.world;
    float (*P)[16] = b.palette;
    float (*S)[16] = b.scratch;

    pixelica_mat4_identity(identity);
    pixelica_mat4_identity(pal[0]);
    rig_init(&r);
    rig_add(&r, -1, identity, identity);
    rig_add(&r, 0, identity, identity);
    zero = r.sk;
    zero.bone_count = 0u;
    too_many = r.sk;
    too_many.bone_count = PIXELICA_SKELETON_MAX_BONES + 1u;
    memset(&b, 0xA5, sizeof(b));
    before = b;

    CHECK_EQ_LONG(pixelica_skeleton_pose(NULL, L, W, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&no_bones, L, W, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&zero, L, W, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&too_many, L, W, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, NULL, W, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, NULL, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, NULL, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, P, NULL, 4u), BAD);

    /* The workspace needs 2 * bone_count matrices: one short is NO_MEMORY. */
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, P, S, 3u), NOMEM);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, P, S, 0u), NOMEM);
    CHECK(memcmp(b.world, before.world, sizeof(b.world)) == 0);
    CHECK(memcmp(b.palette, before.palette, sizeof(b.palette)) == 0);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, P, S, 4u), PIXELICA_OK);
    /* More workspace than needed is fine. */
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, W, P, S, sizeof(b.scratch) / sizeof(b.scratch[0])),
                  PIXELICA_OK);

    /* Skin: NULLs, zero counts, too many bones, workspace one vertex short. */
    CHECK_EQ_LONG(pixelica_skeleton_skin(NULL, &w, 1u, K, 1u, out, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, NULL, 1u, K, 1u, out, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 1u, NULL, 1u, out, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 1u, K, 1u, NULL, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 1u, K, 1u, out, NULL, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 0u, K, 1u, out, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 1u, K, 0u, out, scratch, 1u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(&bind, &w, 1u, K, PIXELICA_SKELETON_MAX_BONES + 1u, out, scratch,
                                    1u),
                  BAD);
    {
        PIXELICA_Vec3 binds[2] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
        PIXELICA_SkinWeight ws[2];
        PIXELICA_Vec3 out_before[2];

        ws[0] = w;
        ws[1] = w;
        memset(out, 0xA5, sizeof(out));
        memcpy(out_before, out, sizeof(out));
        CHECK_EQ_LONG(pixelica_skeleton_skin(binds, ws, 2u, K, 1u, out, scratch, 1u), NOMEM);
        CHECK_EQ_LONG(pixelica_skeleton_skin(binds, ws, 2u, K, 1u, out, scratch, 0u), NOMEM);
        CHECK(memcmp(out, out_before, sizeof(out)) == 0);
        CHECK_EQ_LONG(pixelica_skeleton_skin(binds, ws, 2u, K, 1u, out, scratch, 2u), PIXELICA_OK);
    }
}

/* ------------------------------------------------------------------------ */
/* Aliasing                                                                  */
/* ------------------------------------------------------------------------ */

static void test_pose_aliasing(void)
{
    Rig r;
    PoseBuf b;
    PoseBuf c;
    float identity[16];
    float pool[8][16];
    unsigned char before[sizeof(pool)];
    struct {
        PIXELICA_Skeleton sk;
        float tail[64];
    } arena;
    const float (*L)[16] = CM(r.local);
    float (*P)[16] = b.palette;
    float (*S)[16] = b.scratch;

    pixelica_mat4_identity(identity);
    rig_init(&r);
    rig_add(&r, -1, identity, identity);
    rig_add(&r, 0, identity, identity);
    memset(&b, 0xA5, sizeof(b));
    memset(pool, 0xA5, sizeof(pool));
    memcpy(before, pool, sizeof(pool));

    /* Exact overlaps: in-place pose, world == palette, scratch over an output. */
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, r.local, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool, pool, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool, pool + 2, pool, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool, pool + 2, pool + 2, 4u), BAD);
    /* Partial overlaps: shifted by one matrix. */
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool, pool + 1, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool + 1, pool, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, pool, P, pool + 1, 4u), BAD);
    /* Outputs over the read-only bone records or over the skeleton object. */
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, (float(*)[16])(void *)r.bones, P, S, 4u), BAD);
    memset(&arena, 0, sizeof(arena));
    arena.sk = r.sk;
    CHECK_EQ_LONG(pixelica_skeleton_pose(&arena.sk, L, (float(*)[16])(void *)&arena, P, S, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_pose(&arena.sk, L, b.world, P, (float(*)[16])(void *)&arena, 4u),
                  BAD);
    CHECK(memcmp(pool, before, sizeof(pool)) == 0);

    /* Adjacent but disjoint ranges are fine: world | palette | scratch in one array. */
    memset(&c, 0x5A, sizeof(c));
    CHECK_EQ_LONG(pixelica_skeleton_pose(&r.sk, L, c.world, c.world + 2, c.world + 4, 4u), PIXELICA_OK);
    check_matrix_near(c.world[1], identity, 0.0);
    check_matrix_near(c.world[3], identity, 0.0); /* palette[1] */
}

static void test_skin_aliasing(void)
{
    float pal[2][16];
    PIXELICA_Vec3 bind[4] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    PIXELICA_SkinWeight w[4];
    PIXELICA_Vec3 pool[16];
    PIXELICA_Vec3 scratch[4];
    unsigned char before[sizeof(pool)];
    const float (*K)[16] = CM(pal);

    pixelica_mat4_identity(pal[0]);
    pixelica_mat4_translation(pal[1], 1.0f, 2.0f, 3.0f);
    for (int i = 0; i < 4; ++i) {
        w[i] = skin_weight(0, 0.5f, 1, 0.5f, 0, 0.0f, 0, 0.0f);
    }
    memset(pool, 0xA5, sizeof(pool));
    memcpy(before, pool, sizeof(pool));

    /* out over weights or palette, or equal to its own workspace. */
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, (PIXELICA_Vec3 *)(void *)w, scratch, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, (PIXELICA_Vec3 *)(void *)pal, scratch, 4u),
                  BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, pool, 4u), BAD);
    /* Workspace over the bind positions, weights, palette, or partly over out. */
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, bind, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, bind + 2, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, (PIXELICA_Vec3 *)(void *)w, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, (PIXELICA_Vec3 *)(void *)pal, 4u), BAD);
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, pool + 3, 4u), BAD);
    CHECK(memcmp(pool, before, sizeof(pool)) == 0);

    /* Adjacent output | workspace is fine. */
    CHECK_EQ_LONG(pixelica_skeleton_skin(bind, w, 4u, K, 2u, pool, pool + 4, 4u), PIXELICA_OK);
    CHECK_NEAR(pool[0].x, 1.5, 1e-6);
    CHECK_NEAR(pool[0].y, 1.0, 1e-6);
    CHECK_NEAR(pool[0].z, 1.5, 1e-6);
}

/* ------------------------------------------------------------------------ */
/* Deterministic reference properties                                        */
/* ------------------------------------------------------------------------ */

static uint32_t g_rng = 0x2545F491u;

static float frand(float lo, float hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((float)(g_rng >> 8) / 16777216.0f);
}

static uint32_t urand(uint32_t n)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (g_rng >> 8) % n;
}

static void mul_d(double out[16], const double a[16], const double b[16])
{
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double s = 0.0;

            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = s;
        }
    }
}

static void to_d(double out[16], const float in[16])
{
    for (int i = 0; i < 16; ++i) {
        out[i] = (double)in[i];
    }
}

/* Random forest of rigid bones; inverse_bind inverts the bind-pose world matrix. */
static void random_rig(Rig *r, uint32_t count)
{
    double bind_world[MAXB][16];

    rig_init(r);
    for (uint32_t i = 0; i < count; ++i) {
        float local[16];
        float world_f[16];
        float inverse[16];
        double local_d[16];
        int32_t parent = (i == 0u || urand(5u) == 0u) ? -1 : (int32_t)urand(i);

        rigid(local, frand(-3.0f, 3.0f), frand(-3.0f, 3.0f), frand(-3.0f, 3.0f),
              frand(-3.0f, 3.0f), frand(-3.0f, 3.0f), frand(-3.0f, 3.0f));
        to_d(local_d, local);
        if (parent < 0) {
            memcpy(bind_world[i], local_d, sizeof(local_d));
        } else {
            mul_d(bind_world[i], bind_world[parent], local_d);
        }
        for (int k = 0; k < 16; ++k) {
            world_f[k] = (float)bind_world[i][k];
        }
        rigid_inverse(inverse, world_f);
        rig_add(r, parent, local, inverse);
    }
}

static void test_random_reference(void)
{
    for (int trial = 0; trial < 40; ++trial) {
        Rig r;
        PoseBuf b;
        PoseBuf again;
        double world[MAXB][16];
        uint32_t n = 1u + urand(MAXB);
        PIXELICA_Vec3 bind[MAXV];
        PIXELICA_Vec3 out[MAXV];
        PIXELICA_Vec3 out_again[MAXV];
        PIXELICA_SkinWeight w[MAXV];
        float identity[16];

        random_rig(&r, n);
        pixelica_mat4_identity(identity);

        /* Bind pose: every palette matrix is the identity (roundoff only). */
        memset(&b, 0xA5, sizeof(b));
        CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
        for (uint32_t i = 0; i < n; ++i) {
            check_matrix_near(b.palette[i], identity, 2e-3);
        }

        /* Animate every bone and compare with an independent double-precision chain. */
        for (uint32_t i = 0; i < n; ++i) {
            double local_d[16];

            rigid(r.local[i], frand(-3.0f, 3.0f), frand(-3.0f, 3.0f), frand(-3.0f, 3.0f),
                  frand(-2.0f, 2.0f), frand(-2.0f, 2.0f), frand(-2.0f, 2.0f));
            to_d(local_d, r.local[i]);
            if (r.bones[i].parent < 0) {
                memcpy(world[i], local_d, sizeof(local_d));
            } else {
                mul_d(world[i], world[r.bones[i].parent], local_d);
            }
        }
        CHECK_EQ_LONG(pose(&r, &b), PIXELICA_OK);
        for (uint32_t i = 0; i < n; ++i) {
            double inverse_d[16];
            double palette_d[16];

            to_d(inverse_d, r.bones[i].inverse_bind);
            mul_d(palette_d, world[i], inverse_d);
            for (int k = 0; k < 16; ++k) {
                CHECK_NEAR(b.world[i][k], world[i][k], 2e-3);
                CHECK_NEAR(b.palette[i][k], palette_d[k], 2e-3);
            }
        }
        memset(&again, 0x3C, sizeof(again));
        CHECK_EQ_LONG(pose(&r, &again), PIXELICA_OK);
        CHECK(memcmp(again.world, b.world, (size_t)n * sizeof(b.world[0])) == 0);
        CHECK(memcmp(again.palette, b.palette, (size_t)n * sizeof(b.palette[0])) == 0);

        /* Skinning against the double formula; two runs are bitwise equal. */
        for (uint32_t v = 0; v < MAXV; ++v) {
            float sum = 0.0f;

            bind[v] = vec3(frand(-4.0f, 4.0f), frand(-4.0f, 4.0f), frand(-4.0f, 4.0f));
            for (uint32_t k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
                w[v].bones[k] = (uint16_t)urand(n);
                w[v].weights[k] = (urand(4u) == 0u) ? 0.0f : frand(0.05f, 1.0f);
                sum += w[v].weights[k];
            }
            if (sum == 0.0f) {
                w[v].weights[0] = 1.0f;
                sum = 1.0f;
            }
            for (uint32_t k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
                w[v].weights[k] /= sum;
                if (w[v].weights[k] == 0.0f && urand(2u) == 0u) {
                    w[v].bones[k] = 65535u; /* ignored while the weight is zero */
                }
            }
        }
        CHECK_EQ_LONG(skin_into(bind, w, MAXV, CM(b.palette), n, out), PIXELICA_OK);
        CHECK_EQ_LONG(skin_into(bind, w, MAXV, CM(b.palette), n, out_again), PIXELICA_OK);
        CHECK(memcmp(out, out_again, sizeof(out)) == 0);
        for (uint32_t v = 0; v < MAXV; ++v) {
            double acc[3] = {0.0, 0.0, 0.0};
            double wsum = 0.0;

            for (uint32_t k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
                wsum += (double)w[v].weights[k];
            }
            for (uint32_t k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
                double wk = (double)w[v].weights[k] / wsum;
                const float *p = b.palette[w[v].bones[k] < n ? w[v].bones[k] : 0u];

                if (w[v].weights[k] > 0.0f) {
                    acc[0] += wk * (p[0] * (double)bind[v].x + p[4] * (double)bind[v].y +
                                    p[8] * (double)bind[v].z + p[12]);
                    acc[1] += wk * (p[1] * (double)bind[v].x + p[5] * (double)bind[v].y +
                                    p[9] * (double)bind[v].z + p[13]);
                    acc[2] += wk * (p[2] * (double)bind[v].x + p[6] * (double)bind[v].y +
                                    p[10] * (double)bind[v].z + p[14]);
                }
            }
            CHECK_NEAR(out[v].x, acc[0], 1e-3);
            CHECK_NEAR(out[v].y, acc[1], 1e-3);
            CHECK_NEAR(out[v].z, acc[2], 1e-3);
        }
    }
}

int main(void)
{
    test_pose_bind_is_identity();
    test_pose_hierarchy();
    test_pose_palette_and_skin_rigid();
    test_skin_blend();
    test_skin_normalization();
    test_skin_weight_validation();
    test_skin_inplace_and_overlap();
    test_pose_parent_validation();
    test_matrix_validation();
    test_overflow();
    test_arguments_and_scratch();
    test_pose_aliasing();
    test_skin_aliasing();
    test_random_reference();
    return pixelica_t_finish("test_skeleton");
}
