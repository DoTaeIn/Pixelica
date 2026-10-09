/*
 * Pixelica - AABB overlap/move semantics (contact clamping, no tunneling,
 * sliding, fixed X-Y-Z order, transactional rejection, float-rounding
 * stress) and the bounded scene list (validation, lookup, solid bounds,
 * rendering equivalent to direct draws, atomic pre-checks).
 */
#include <float.h>

#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define RED ((uint16_t)0xF800)
#define GREEN ((uint16_t)0x07E0)
#define BLUE ((uint16_t)0x001F)

static const float k_identity[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static PIXELICA_AABB box(float x0, float y0, float z0, float x1, float y1, float z1)
{
    PIXELICA_AABB b;
    b.min = v3(x0, y0, z0);
    b.max = v3(x1, y1, z1);
    return b;
}

static int same_box(const PIXELICA_AABB *a, const PIXELICA_AABB *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static int same_vec(PIXELICA_Vec3 a, PIXELICA_Vec3 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* ------------------------------------------------------------------------ */

static void test_overlap_rules(void)
{
    PIXELICA_AABB a = box(0, 0, 0, 1, 1, 1);
    PIXELICA_AABB b;

    b = box(0.5f, 0.5f, 0.5f, 2, 2, 2);
    CHECK(pixelica_aabb_overlaps(&a, &b) && pixelica_aabb_overlaps(&b, &a));
    b = box(0.25f, 0.25f, 0.25f, 0.75f, 0.75f, 0.75f); /* contained */
    CHECK(pixelica_aabb_overlaps(&a, &b) && pixelica_aabb_overlaps(&b, &a));
    /* Touching face, edge and corner: not penetration. */
    b = box(1, 0, 0, 2, 1, 1);
    CHECK(!pixelica_aabb_overlaps(&a, &b) && !pixelica_aabb_overlaps(&b, &a));
    b = box(1, 1, 0, 2, 2, 1);
    CHECK(!pixelica_aabb_overlaps(&a, &b));
    b = box(-1, -1, -1, 0, 0, 0);
    CHECK(!pixelica_aabb_overlaps(&a, &b));
    /* Separated on a single axis is enough. */
    b = box(0, 0, 2, 1, 1, 3);
    CHECK(!pixelica_aabb_overlaps(&a, &b));
    /* A zero-thickness box inside overlaps; on the surface it does not. */
    b = box(0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f);
    CHECK(pixelica_aabb_overlaps(&a, &b));
    b = box(0.5f, 1.0f, 0.5f, 0.5f, 1.0f, 0.5f);
    CHECK(!pixelica_aabb_overlaps(&a, &b));
    /* NULL / NaN never overlap. */
    CHECK(!pixelica_aabb_overlaps(NULL, &a) && !pixelica_aabb_overlaps(&a, NULL));
    b = a;
    b.min.y = pixelica_t_nan();
    CHECK(!pixelica_aabb_overlaps(&a, &b));

    CHECK(pixelica_aabb_is_valid(&a) == 1);
    b = box(0, 0, 0, 0, 0, 0);
    CHECK(pixelica_aabb_is_valid(&b) == 1);
    b = box(0, 2, 0, 1, 1, 1);
    CHECK(pixelica_aabb_is_valid(&b) == 0);
    b = a;
    b.max.z = pixelica_t_inf();
    CHECK(pixelica_aabb_is_valid(&b) == 0);
    CHECK(pixelica_aabb_is_valid(NULL) == 0);
}

static void test_move_basics(void)
{
    PIXELICA_AABB a = box(0, 0, 0, 1, 1, 1);
    PIXELICA_AABB wall = box(3, -5, -5, 4, 5, 5);
    PIXELICA_AABB thin = box(5, -5, -5, 5.001f, 5, 5);
    PIXELICA_Vec3 applied;

    /* Free move: exact translation. */
    CHECK(pixelica_aabb_move(&a, v3(0.5f, -2.0f, 0.25f), NULL, 0, &applied) ==
          PIXELICA_OK);
    CHECK(same_vec(applied, v3(0.5f, -2.0f, 0.25f)));
    CHECK(a.min.x == 0.5f && a.max.y == -1.0f && a.max.z == 1.25f);

    /* Clamped to contact, not beyond. */
    a = box(0, 0, 0, 1, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(10.0f, 0, 0), &wall, 1, &applied) == PIXELICA_OK);
    CHECK(applied.x == 2.0f && a.max.x == 3.0f && a.min.x == 2.0f);
    CHECK(!pixelica_aabb_overlaps(&a, &wall));
    /* Pushing into the touched wall again moves nothing; leaving works. */
    CHECK(pixelica_aabb_move(&a, v3(1.0f, 0, 0), &wall, 1, &applied) == PIXELICA_OK);
    CHECK(applied.x == 0.0f && a.max.x == 3.0f);
    CHECK(pixelica_aabb_move(&a, v3(-0.5f, 0, 0), &wall, 1, &applied) == PIXELICA_OK);
    CHECK(applied.x == -0.5f && a.max.x == 2.5f);
    /* Approach from the other side. */
    a = box(6, 0, 0, 7, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(-100.0f, 0, 0), &wall, 1, &applied) ==
          PIXELICA_OK);
    CHECK(a.min.x == 4.0f && applied.x == -2.0f);

    /* No tunneling through a thin wall with a huge step. */
    a = box(0, 0, 0, 1, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(1.0e30f, 0, 0), &thin, 1, &applied) ==
          PIXELICA_OK);
    CHECK(a.max.x == 5.0f);
    CHECK(!pixelica_aabb_overlaps(&a, &thin));

    /* Nearest of several obstacles wins regardless of array order. */
    {
        PIXELICA_AABB obs[2];
        obs[0] = thin;
        obs[1] = wall;
        a = box(0, 0, 0, 1, 1, 1);
        CHECK(pixelica_aabb_move(&a, v3(50.0f, 0, 0), obs, 2, &applied) == PIXELICA_OK);
        CHECK(a.max.x == 3.0f);
    }

    /* Obstacles beside the path (touching sideways) do not block. */
    {
        PIXELICA_AABB side = box(2, 1, 0, 3, 2, 1); /* top face y = 1 touches */
        a = box(0, 0, 0, 1, 1, 1);
        CHECK(pixelica_aabb_move(&a, v3(5.0f, 0, 0), &side, 1, &applied) ==
              PIXELICA_OK);
        CHECK(applied.x == 5.0f);
    }
    /* out_applied may be NULL. */
    a = box(0, 0, 0, 1, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(1, 1, 1), NULL, 0, NULL) == PIXELICA_OK);
    CHECK(a.min.x == 1.0f && a.min.y == 1.0f && a.min.z == 1.0f);
}

static void test_move_sliding_and_axis_order(void)
{
    PIXELICA_AABB floor_box = box(-10, -1, -10, 10, 0, 10);
    PIXELICA_AABB a = box(0, 0, 0, 1, 1, 1); /* resting on the floor */
    PIXELICA_AABB obs[2];
    PIXELICA_Vec3 applied;

    /* Gravity into the floor plus horizontal motion: slides, stays on it. */
    CHECK(pixelica_aabb_move(&a, v3(2.0f, -0.5f, -1.0f), &floor_box, 1,
                        &applied) == PIXELICA_OK);
    CHECK(same_vec(applied, v3(2.0f, 0.0f, -1.0f)));
    CHECK(a.min.y == 0.0f && a.min.x == 2.0f && a.min.z == -1.0f);

    /* Falling onto the floor from above stops exactly on it. */
    a = box(0, 3.25f, 0, 1, 4.25f, 1);
    CHECK(pixelica_aabb_move(&a, v3(0, -100.0f, 0), &floor_box, 1, &applied) ==
          PIXELICA_OK);
    CHECK(a.min.y == 0.0f && applied.y == -3.25f);

    /* Fixed order X, then Y, then Z: a block at x in [2,3] (current rows
     * only) stops X first; Y then moves freely. The Y-first order would
     * have let the box pass above the block. */
    obs[0] = box(2, 0, 0, 3, 1, 1);
    obs[1] = box(-50, 20, -50, 50, 21, 50); /* ceiling */
    a = box(0, 0, 0, 1, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(5.0f, 5.0f, 0.0f), obs, 2, &applied) ==
          PIXELICA_OK);
    CHECK(same_vec(applied, v3(1.0f, 5.0f, 0.0f)));
    CHECK(a.min.x == 1.0f && a.min.y == 5.0f);
    /* Next step the block is no longer in the way (above it). */
    CHECK(pixelica_aabb_move(&a, v3(5.0f, 100.0f, 0.0f), obs, 2, &applied) ==
          PIXELICA_OK);
    CHECK(same_vec(applied, v3(5.0f, 14.0f, 0.0f)));
    CHECK(a.max.y == 20.0f);
    /* Z sweep sees the updated X/Y position. */
    obs[0] = box(5, 0, 2, 6, 1, 3);
    a = box(0, 0, 0, 1, 1, 1);
    CHECK(pixelica_aabb_move(&a, v3(4.5f, 0.0f, 5.0f), obs, 1, &applied) ==
          PIXELICA_OK);
    CHECK(same_vec(applied, v3(4.5f, 0.0f, 1.0f)));
}

static void test_move_invalid_is_transactional(void)
{
    PIXELICA_AABB a = box(0, 0, 0, 1, 1, 1);
    const PIXELICA_AABB orig = a;
    PIXELICA_AABB obs[2];
    PIXELICA_Vec3 applied;

    obs[0] = box(3, 0, 0, 4, 1, 1);
    obs[1] = box(0.5f, 0.5f, 0.5f, 2, 2, 2); /* penetrating */

#define EXPECT_INVALID(call)                                         \
    do {                                                             \
        applied = v3(9.0f, 9.0f, 9.0f);                              \
        CHECK((call) == PIXELICA_ERR_INVALID_ARG);                        \
        CHECK(same_box(&a, &orig));                                  \
        CHECK(same_vec(applied, v3(0.0f, 0.0f, 0.0f)));              \
    } while (0)

    EXPECT_INVALID(pixelica_aabb_move(&a, v3(1, 0, 0), obs, 2, &applied));
    EXPECT_INVALID(pixelica_aabb_move(&a, v3(pixelica_t_nan(), 0, 0), obs, 1, &applied));
    EXPECT_INVALID(pixelica_aabb_move(&a, v3(0, pixelica_t_inf(), 0), obs, 1, &applied));
    EXPECT_INVALID(pixelica_aabb_move(&a, v3(1, 0, 0), NULL, 1, &applied));
    EXPECT_INVALID(pixelica_aabb_move(NULL, v3(1, 0, 0), obs, 1, &applied));
    obs[1] = box(5, 5, 5, 4, 6, 6); /* inverted obstacle */
    EXPECT_INVALID(pixelica_aabb_move(&a, v3(1, 0, 0), obs, 2, &applied));
    obs[1] = box(5, 5, 5, 6, 6, pixelica_t_nan());
    EXPECT_INVALID(pixelica_aabb_move(&a, v3(1, 0, 0), obs, 2, &applied));
    /* Invalid moving box. */
    a.max.x = -1.0f;
    {
        const PIXELICA_AABB inverted = a;
        applied = v3(9.0f, 9.0f, 9.0f);
        CHECK(pixelica_aabb_move(&a, v3(1, 0, 0), NULL, 0, &applied) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(same_box(&a, &inverted));
    }
    a = orig;
    /* A move that overflows the float range is rejected as a whole, even
     * though X alone would have been fine. */
    {
        PIXELICA_AABB far_box = box(0, FLT_MAX / 2.0f, 0, 1, FLT_MAX, 1);
        const PIXELICA_AABB far_orig = far_box;
        CHECK(pixelica_aabb_move(&far_box, v3(1.0f, FLT_MAX, 0.0f), NULL, 0,
                            &applied) == PIXELICA_ERR_INVALID_ARG);
        CHECK(same_box(&far_box, &far_orig));
        CHECK(same_vec(applied, v3(0.0f, 0.0f, 0.0f)));
    }
#undef EXPECT_INVALID
}

/* Deterministic pseudo-random walk among obstacles with awkward float
 * coordinates: the box never overlaps anything (each next call would also
 * reject a penetrating start), and a replay is bit-identical. */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static float rnd(uint32_t *s, float lo, float hi)
{
    return lo + (hi - lo) * ((float)(lcg(s) & 0xFFFFu) / 65535.0f);
}

static void random_walk(float offset, PIXELICA_AABB *out_final, uint32_t *out_bad,
                        uint32_t *out_contacts)
{
    PIXELICA_AABB obs[6];
    PIXELICA_AABB a;
    uint32_t seed = 12345u;
    uint32_t bad = 0, contacts = 0;
    int i;
    size_t k;

    obs[0] = box(offset + 0.1f, -0.3f, -10.0f, offset + 0.7f, 0.9f, 10.0f);
    obs[1] = box(offset - 3.3f, -0.3f, -10.0f, offset - 2.9f, 3.7f, 10.0f);
    obs[2] = box(offset - 3.3f, -1.3f, -10.0f, offset + 3.3f, -0.3f, 10.0f);
    obs[3] = box(offset + 2.9f, -0.3f, -10.0f, offset + 3.3f, 3.7f, 10.0f);
    obs[4] = box(offset - 3.3f, 3.7f, -10.0f, offset + 3.3f, 4.1f, 10.0f);
    obs[5] = box(offset - 1.7f, 1.3f, -10.0f, offset - 1.1f, 1.9f, 10.0f);
    a = box(offset - 2.5f, 0.0f, -0.5f, offset - 2.2f, 0.7f, 0.5f);
    for (i = 0; i < 20000; ++i) {
        PIXELICA_Vec3 d = v3(rnd(&seed, -0.37f, 0.37f), rnd(&seed, -0.37f, 0.37f),
                        0.0f);
        PIXELICA_Vec3 applied;

        if (pixelica_aabb_move(&a, d, obs, 6, &applied) != PIXELICA_OK) {
            ++bad;
            break;
        }
        if (!same_vec(applied, d)) {
            ++contacts;
        }
        for (k = 0; k < 6; ++k) {
            if (pixelica_aabb_overlaps(&a, &obs[k])) {
                ++bad;
            }
        }
    }
    *out_final = a;
    *out_bad = bad;
    *out_contacts = contacts;
}

static void test_move_rounding_stress(void)
{
    static const float offsets[3] = {0.0f, 1000.1f, 123456.7f};
    int i;

    for (i = 0; i < 3; ++i) {
        PIXELICA_AABB first, second;
        uint32_t bad = 0, contacts = 0, bad2 = 0, contacts2 = 0;

        random_walk(offsets[i], &first, &bad, &contacts);
        random_walk(offsets[i], &second, &bad2, &contacts2);
        CHECK_EQ_LONG(bad, 0);
        CHECK(contacts > 100u); /* the walk really hit walls often */
        CHECK(same_box(&first, &second) && contacts == contacts2);
    }
}

/* ---- scene ------------------------------------------------------------- */

static const PIXELICA_Vec3 k_quad_v[4] = {
    {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f},
    {-0.5f, 0.5f, 0.0f}};
static const uint16_t k_quad_i[6] = {0, 1, 2, 0, 2, 3};
static const PIXELICA_Vec2 k_quad_uv[4] = {
    {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};

static PIXELICA_Mesh quad_mesh(uint16_t color)
{
    PIXELICA_Mesh m;
    m.vertices = k_quad_v;
    m.vertex_count = 4;
    m.indices = k_quad_i;
    m.index_count = 6;
    m.color = color;
    return m;
}

static PIXELICA_SceneNode make_node(uint32_t id, const PIXELICA_Mesh *mesh,
                               const PIXELICA_Material *mat, float tx, float tz,
                               bool solid)
{
    PIXELICA_SceneNode n;
    memset(&n, 0, sizeof n);
    n.mesh = mesh;
    n.material = mat;
    pixelica_mat4_translation(n.model, tx, 0.0f, tz);
    n.bounds = box(tx - 0.5f, -0.5f, tz - 0.1f, tx + 0.5f, 0.5f, tz + 0.1f);
    n.id = id;
    n.solid = solid;
    return n;
}

static uint64_t frame_hash(const PIXELICA_Context *ctx, uint16_t w, uint16_t h)
{
    size_t n = (size_t)w * (size_t)h;
    return pixelica_t_hash16(pixelica_color_buffer(ctx), n) ^
           (pixelica_t_hash16(pixelica_depth_buffer(ctx), n) * 31u);
}

static void test_scene_add_find_bounds(void)
{
    PIXELICA_SceneNode storage[3];
    PIXELICA_Scene scene;
    PIXELICA_SceneNode n;
    PIXELICA_Mesh mesh = quad_mesh(RED);
    PIXELICA_Mesh bad_mesh;
    PIXELICA_Vec3 bad_v[4];
    PIXELICA_Material bad_mat;
    PIXELICA_AABB out[4];
    static const uint16_t tex_px[1] = {GREEN};
    PIXELICA_Texture tex;

    tex.pixels = tex_px;
    tex.width = 1;
    tex.height = 1;

    pixelica_scene_init(NULL, storage, 3); /* no-op */
    pixelica_scene_init(&scene, NULL, 5);
    CHECK(scene.capacity == 0 && scene.count == 0);
    n = make_node(1, &mesh, NULL, 0, 0, true);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_CAPACITY);

    pixelica_scene_init(&scene, storage, 3);
    CHECK(pixelica_scene_add(&scene, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_scene_add(NULL, &n) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    /* Duplicate id. */
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);

    /* Invalid nodes leave the scene unchanged. */
    n = make_node(2, &mesh, NULL, 0, 0, true);
    n.model[5] = pixelica_t_nan();
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    n = make_node(2, &mesh, NULL, 0, 0, true);
    n.bounds.min.x = 5.0f; /* inverted */
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    bad_mesh = mesh;
    bad_mesh.index_count = 5;
    n = make_node(2, &bad_mesh, NULL, 0, 0, true);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    bad_mesh = mesh;
    bad_mesh.vertex_count = 3; /* index 3 now out of range */
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    memcpy(bad_v, k_quad_v, sizeof bad_v);
    bad_v[3].z = pixelica_t_inf();
    bad_mesh = mesh;
    bad_mesh.vertices = bad_v;
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    bad_mat.uvs = NULL;
    bad_mat.triangle_colors = NULL;
    bad_mat.texture = &tex; /* texture without UVs */
    n = make_node(2, &mesh, &bad_mat, 0, 0, true);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_INVALID_ARG);
    CHECK(scene.count == 1);

    /* Collider-only node (no mesh) and a non-solid decoration. */
    n = make_node(7, NULL, NULL, 2.0f, 0, true);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    n = make_node(9, &mesh, NULL, -2.0f, 0, false);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    n = make_node(10, &mesh, NULL, 0, 0, false);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_ERR_CAPACITY);
    CHECK(scene.count == 3);

    CHECK(pixelica_scene_find(&scene, 7) == &storage[1]);
    CHECK(pixelica_scene_find(&scene, 9) == &storage[2]);
    CHECK(pixelica_scene_find(&scene, 10) == NULL);
    CHECK(pixelica_scene_find(NULL, 7) == NULL);

    /* Solid bounds: nodes 1 and 7; skipping node 1 leaves node 7. */
    CHECK(pixelica_scene_solid_bounds(&scene, NULL, out, 4) == 2);
    CHECK(same_box(&out[0], &storage[0].bounds) &&
          same_box(&out[1], &storage[1].bounds));
    CHECK(pixelica_scene_solid_bounds(&scene, &storage[0], out, 4) == 1);
    CHECK(same_box(&out[0], &storage[1].bounds));
    CHECK(pixelica_scene_solid_bounds(&scene, NULL, out, 1) == 1);
    CHECK(pixelica_scene_solid_bounds(&scene, NULL, NULL, 4) == 0);
    CHECK(pixelica_scene_solid_bounds(NULL, NULL, out, 4) == 0);

    /* Player-style use: move node 9's box through the solids. */
    {
        PIXELICA_SceneNode *player = pixelica_scene_find(&scene, 9);
        PIXELICA_AABB solids[4];
        size_t count;
        PIXELICA_Vec3 applied;
        CHECK(player != NULL);
        if (player != NULL) {
            count = pixelica_scene_solid_bounds(&scene, player, solids, 4);
            CHECK(count == 2);
            CHECK(pixelica_aabb_move(&player->bounds, v3(10.0f, 0, 0), solids,
                                count, &applied) == PIXELICA_OK);
            /* Node 1 spans x [-0.5, 0.5]: player (x [-2.5,-1.5]) stops. */
            CHECK(applied.x == 1.0f && player->bounds.max.x == -0.5f);
        }
    }
}

static void test_scene_render(void)
{
    PIXELICA_SceneNode storage[4];
    PIXELICA_Scene scene;
    PIXELICA_SceneNode n;
    PIXELICA_Mesh red = quad_mesh(RED);
    PIXELICA_Mesh blue = quad_mesh(BLUE);
    PIXELICA_Mesh big;
    PIXELICA_Material textured;
    static const uint16_t tex_px[1] = {GREEN};
    PIXELICA_Texture tex;
    PIXELICA_Config cfg = pixelica_t_config(32, 32);
    PIXELICA_Context *ctx = NULL;
    float model[16];
    uint64_t direct, before;
    PIXELICA_FrameStats s;

    tex.pixels = tex_px;
    tex.width = 1;
    tex.height = 1;
    textured.uvs = k_quad_uv;
    textured.triangle_colors = NULL;
    textured.texture = &tex;

    cfg.max_draw_commands = 4;
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    pixelica_scene_init(&scene, storage, 4);
    n = make_node(1, &red, NULL, -0.4f, 0.0f, true);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    n = make_node(2, NULL, NULL, 0.0f, 0.0f, true); /* collider only */
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    n = make_node(3, &blue, NULL, 0.4f, -0.5f, false);
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);
    n = make_node(4, &red, &textured, 0.0f, 0.5f, false);
    n.model[13] = 0.45f;
    CHECK(pixelica_scene_add(&scene, &n) == PIXELICA_OK);

    /* Same frame drawn directly. */
    pixelica_begin_frame(ctx, BG);
    pixelica_mat4_translation(model, -0.4f, 0.0f, 0.0f);
    CHECK(pixelica_draw_mesh(ctx, &red, model) == PIXELICA_OK);
    pixelica_mat4_translation(model, 0.4f, 0.0f, -0.5f);
    CHECK(pixelica_draw_mesh(ctx, &blue, model) == PIXELICA_OK);
    pixelica_mat4_translation(model, 0.0f, 0.45f, 0.5f);
    CHECK(pixelica_draw_mesh_ex(ctx, &red, model, &textured) == PIXELICA_OK);
    direct = frame_hash(ctx, 32, 32);

    pixelica_begin_frame(ctx, BG);
    CHECK(pixelica_scene_render(&scene, ctx) == PIXELICA_OK);
    CHECK(frame_hash(ctx, 32, 32) == direct);
    s = pixelica_frame_stats(ctx);
    CHECK_EQ_LONG(s.draw_calls, 3);
    CHECK(pixelica_t_count_color(ctx, 32, 32, GREEN) > 0u);
    CHECK(pixelica_t_count_color(ctx, 32, 32, BLUE) > 0u);
    CHECK(pixelica_t_count_color(ctx, 32, 32, RED) > 0u);

    /* Pre-checks: nothing drawn when the remaining draw budget (4 - 2) is
     * smaller than the 3 drawable nodes. */
    pixelica_begin_frame(ctx, BG);
    CHECK(pixelica_draw_mesh(ctx, &red, k_identity) == PIXELICA_OK);
    CHECK(pixelica_draw_mesh(ctx, &blue, k_identity) == PIXELICA_OK);
    before = frame_hash(ctx, 32, 32);
    CHECK(pixelica_scene_render(&scene, ctx) == PIXELICA_ERR_CAPACITY);
    CHECK(frame_hash(ctx, 32, 32) == before);
    s = pixelica_frame_stats(ctx);
    CHECK(s.draw_calls == 2 && s.last_error == PIXELICA_ERR_CAPACITY);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Outside a frame, NULL arguments, invalid camera. */
    CHECK(pixelica_scene_render(&scene, ctx) == PIXELICA_ERR_INVALID_ARG);
    pixelica_begin_frame(ctx, BG);
    before = frame_hash(ctx, 32, 32);
    CHECK(pixelica_scene_render(NULL, ctx) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_scene_render(&scene, NULL) == PIXELICA_ERR_INVALID_ARG);
    pixelica_set_view_projection(ctx, NULL);
    CHECK(pixelica_scene_render(&scene, ctx) == PIXELICA_ERR_INVALID_ARG);
    pixelica_set_view_projection(ctx, k_identity);
    CHECK(frame_hash(ctx, 32, 32) == before);

    /* A mesh larger than max_vertices_per_mesh: CAPACITY, nothing drawn. */
    {
        static PIXELICA_Vec3 many[300];
        PIXELICA_SceneNode st2[2];
        PIXELICA_Scene s2;
        big = red;
        big.vertices = many;
        big.vertex_count = 257;
        pixelica_scene_init(&s2, st2, 2);
        n = make_node(1, &red, NULL, 0.0f, 0.0f, false);
        CHECK(pixelica_scene_add(&s2, &n) == PIXELICA_OK);
        n = make_node(2, &big, NULL, 0.0f, 0.0f, false);
        CHECK(pixelica_scene_add(&s2, &n) == PIXELICA_OK);
        CHECK(pixelica_scene_render(&s2, ctx) == PIXELICA_ERR_CAPACITY);
        CHECK(frame_hash(ctx, 32, 32) == before);
        CHECK_EQ_LONG(pixelica_frame_stats(ctx).draw_calls, 0);
    }

    /* Empty scene renders nothing and succeeds. */
    {
        PIXELICA_Scene empty;
        pixelica_scene_init(&empty, NULL, 0);
        CHECK(pixelica_scene_render(&empty, ctx) == PIXELICA_OK);
    }
    CHECK(pixelica_memory_guards_intact(ctx) == 1);
    pixelica_destroy(ctx);
}

int main(void)
{
    test_overlap_rules();
    test_move_basics();
    test_move_sliding_and_axis_order();
    test_move_invalid_is_transactional();
    test_move_rounding_stress();
    test_scene_add_find_bounds();
    test_scene_render();
    return pixelica_t_finish("test_scene");
}
