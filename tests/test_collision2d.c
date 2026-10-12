/* 2D collision: validity/overlap, swept X-then-Y move, wall slide, contacts,
 * no tunneling, platformer bodies (grounding, jump, edges, walls, ceilings),
 * transactional failures and alias rejection.
 * Common-library test: links Pixelica::common only and includes no 3D header. */
#include <float.h>

#include "pixelica/collision2d.h"
#include "test_util.h"

/* dt and gravity chosen so every step is exact in float. */
#define DT 0.125f
#define G 600.0f
#define JUMP 220.0f
#define FALL 600.0f

static PIXELICA_Vec2 v2(float x, float y)
{
    PIXELICA_Vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

static PIXELICA_AABB2D box(float x0, float y0, float x1, float y1)
{
    PIXELICA_AABB2D b;
    b.min = v2(x0, y0);
    b.max = v2(x1, y1);
    return b;
}

static PIXELICA_Body2D body_at(float x0, float y0, float w, float h)
{
    PIXELICA_Body2D b;
    memset(&b, 0, sizeof b);
    b.bounds = box(x0, y0, x0 + w, y0 + h);
    return b;
}

static int same_box(const PIXELICA_AABB2D *a, const PIXELICA_AABB2D *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

static void test_validity_and_overlap(void)
{
    PIXELICA_AABB2D a = box(0, 0, 10, 10);
    PIXELICA_AABB2D b;
    CHECK(pixelica_aabb2d_is_valid(&a));
    CHECK(!pixelica_aabb2d_is_valid(NULL));
    b = box(0, 0, 0, 10);
    CHECK(!pixelica_aabb2d_is_valid(&b)); /* zero extent */
    b = box(0, 5, 10, 4);
    CHECK(!pixelica_aabb2d_is_valid(&b));
    b = box(pixelica_t_nan(), 0, 10, 10);
    CHECK(!pixelica_aabb2d_is_valid(&b));
    b = box(0, 0, pixelica_t_inf(), 10);
    CHECK(!pixelica_aabb2d_is_valid(&b));

    b = box(5, 5, 15, 15);
    CHECK(pixelica_aabb2d_overlaps(&a, &b) && pixelica_aabb2d_overlaps(&b, &a));
    b = box(2, 2, 3, 3);
    CHECK(pixelica_aabb2d_overlaps(&a, &b)); /* containment */
    b = box(10, 0, 20, 10);
    CHECK(!pixelica_aabb2d_overlaps(&a, &b)); /* shared edge */
    b = box(10, 10, 20, 20);
    CHECK(!pixelica_aabb2d_overlaps(&a, &b)); /* shared corner */
    b = box(5, 5, 5, 15);
    CHECK(!pixelica_aabb2d_overlaps(&a, &b)); /* invalid */
    CHECK(!pixelica_aabb2d_overlaps(&a, NULL) && !pixelica_aabb2d_overlaps(NULL, &a));
}

static void test_move_free_and_walls(void)
{
    const PIXELICA_AABB2D walls[2] = {box(20, -100, 30, 100), box(-30, -100, -20, 100)};
    PIXELICA_AABB2D b = box(0, 0, 10, 10);
    PIXELICA_Vec2 applied;
    uint32_t contacts = 99u;

    /* No obstacles: NULL array with count 0, NULL outputs. */
    CHECK(pixelica_aabb2d_move(&b, v2(3, -4), NULL, 0, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == 3.0f && applied.y == -4.0f && contacts == 0u);
    CHECK(b.min.x == 3.0f && b.min.y == -4.0f && b.max.x == 13.0f && b.max.y == 6.0f);
    CHECK(pixelica_aabb2d_move(&b, v2(-3, 4), NULL, 0, NULL, NULL) == PIXELICA_OK);
    CHECK(b.min.x == 0.0f && b.min.y == 0.0f);

    /* Blocked on the right, with wall slide on Y. */
    CHECK(pixelica_aabb2d_move(&b, v2(50, 5), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == 10.0f && applied.y == 5.0f);
    CHECK(b.max.x == 20.0f && b.min.y == 5.0f);
    CHECK(contacts == PIXELICA_CONTACT2D_RIGHT);
    /* Pushing into the touching wall: no motion, still a contact. */
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == 0.0f && b.max.x == 20.0f && contacts == PIXELICA_CONTACT2D_RIGHT);
    /* Moving away from it: no contact. */
    CHECK(pixelica_aabb2d_move(&b, v2(-5, 0), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == -5.0f && contacts == 0u);
    /* Exact move onto the wall face is recognized. */
    CHECK(pixelica_aabb2d_move(&b, v2(5, 0), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == 5.0f && b.max.x == 20.0f && contacts == PIXELICA_CONTACT2D_RIGHT);
    /* Left wall. */
    CHECK(pixelica_aabb2d_move(&b, v2(-100, 0), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(b.min.x == -20.0f && applied.x == -30.0f && contacts == PIXELICA_CONTACT2D_LEFT);
    /* Zero delta reports nothing even while touching. */
    CHECK(pixelica_aabb2d_move(&b, v2(0, 0), walls, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(contacts == 0u);
}

static void test_move_vertical_and_seams(void)
{
    /* Two floor tiles sharing an edge plus a ceiling. */
    const PIXELICA_AABB2D obs[3] = {box(0, 10, 10, 20), box(10, 10, 20, 20), box(-50, -40, 50, -30)};
    PIXELICA_AABB2D b = box(2, 0, 6, 10); /* resting on the first tile */
    PIXELICA_Vec2 applied;
    uint32_t contacts;

    /* Sliding across the seam is not blocked by the neighbour tile. */
    CHECK(pixelica_aabb2d_move(&b, v2(12, 3), obs, 3, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.x == 12.0f && applied.y == 0.0f && contacts == PIXELICA_CONTACT2D_BOTTOM);
    CHECK(b.min.x == 14.0f && b.max.y == 10.0f);
    /* Up into the ceiling. */
    CHECK(pixelica_aabb2d_move(&b, v2(0, -100), obs, 3, &applied, &contacts) == PIXELICA_OK);
    CHECK(applied.y == -30.0f && b.min.y == -30.0f && contacts == PIXELICA_CONTACT2D_TOP);
    /* Diagonal into a corner: both sides. */
    {
        const PIXELICA_AABB2D corner[2] = {box(10, -100, 20, 100), box(-100, 10, 10, 20)};
        PIXELICA_AABB2D c = box(0, 0, 5, 5);
        CHECK(pixelica_aabb2d_move(&c, v2(30, 30), corner, 2, &applied, &contacts) == PIXELICA_OK);
        CHECK(c.max.x == 10.0f && c.max.y == 10.0f);
        CHECK(contacts == (PIXELICA_CONTACT2D_RIGHT | PIXELICA_CONTACT2D_BOTTOM));
    }
}

static void test_move_no_tunneling(void)
{
    const PIXELICA_AABB2D thin[2] = {box(100, 0, 100.5f, 10), box(-1000, 5000, 1000, 5000.25f)};
    PIXELICA_AABB2D b = box(0, 0, 10, 10);
    PIXELICA_Vec2 applied;
    uint32_t contacts;

    CHECK(pixelica_aabb2d_move(&b, v2(1.0e6f, 0), thin, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(b.max.x == 100.0f && applied.x == 90.0f && contacts == PIXELICA_CONTACT2D_RIGHT);
    b = box(0, 20, 10, 30);
    CHECK(pixelica_aabb2d_move(&b, v2(0, 1.0e7f), thin, 2, &applied, &contacts) == PIXELICA_OK);
    CHECK(b.max.y == 5000.0f && contacts == PIXELICA_CONTACT2D_BOTTOM);
    /* Non-representable gap: never passes the face. */
    b = box(0.1f, 0.3f, 10.1f, 10.3f);
    CHECK(pixelica_aabb2d_move(&b, v2(1000.0f, 0), thin, 2, NULL, &contacts) == PIXELICA_OK);
    CHECK(b.max.x <= 100.0f && b.max.x > 99.99f && contacts == PIXELICA_CONTACT2D_RIGHT);
}

static void test_move_invalid(void)
{
    PIXELICA_AABB2D obs[2] = {box(20, 0, 30, 10), box(100, 100, 110, 110)};
    PIXELICA_AABB2D b = box(0, 0, 10, 10);
    const PIXELICA_AABB2D saved = b;
    PIXELICA_Vec2 applied = v2(7, 7);
    uint32_t contacts = 77u;

#define UNCHANGED() CHECK(same_box(&b, &saved) && applied.x == 7.0f && applied.y == 7.0f && contacts == 77u)
    /* Starting penetration is rejected, not resolved. */
    {
        PIXELICA_AABB2D pen = box(5, 5, 15, 15);
        CHECK(pixelica_aabb2d_move(&b, v2(1, 1), &pen, 1, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
        UNCHANGED();
    }
    CHECK(pixelica_aabb2d_move(&b, v2(pixelica_t_nan(), 0), obs, 2, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(0, pixelica_t_inf()), obs, 2, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(NULL, v2(1, 0), obs, 2, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), NULL, 1, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, (size_t)PIXELICA_COLLISION2D_MAX_OBSTACLES + 1u, &applied,
                               &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, SIZE_MAX, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    obs[1].max.x = 100.0f; /* zero-extent obstacle */
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, 2, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
    obs[1].max.x = 110.0f;
    UNCHANGED();

    /* Aliasing outputs. */
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, 2, &b.max, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, 2, &obs[1].min, &contacts) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, 2, &applied, (uint32_t *)(void *)&applied) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&b, v2(1, 0), obs, 2, NULL, (uint32_t *)(void *)&b.min.x) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_aabb2d_move(&obs[1], v2(1, 0), obs, 2, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(obs[1].min.x == 100.0f && obs[1].max.y == 110.0f);
    UNCHANGED();

    /* Float overflow of the result. */
    {
        PIXELICA_AABB2D big = box(3.0e38f, 0, 3.2e38f, 1);
        const PIXELICA_AABB2D big_saved = big;
        CHECK(pixelica_aabb2d_move(&big, v2(3.0e38f, 0), NULL, 0, &applied, &contacts) == PIXELICA_ERR_INVALID_ARG);
        CHECK(same_box(&big, &big_saved));
    }
    UNCHANGED();
#undef UNCHANGED
}

static const PIXELICA_AABB2D k_floor[1] = {{{-1000.0f, 100.0f}, {1000.0f, 200.0f}}};

static void test_body_grounding(void)
{
    PIXELICA_Body2D b = body_at(0, 68, 32, 32); /* exactly on the floor */
    int i;

    /* First tick, with gravity. */
    b.contacts = 123u;
    b.grounded = 7u;
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
    CHECK(b.grounded == 1u && b.contacts == PIXELICA_CONTACT2D_BOTTOM);
    CHECK(b.bounds.max.y == 100.0f && b.velocity.y == 0.0f);
    /* Stays put without velocity accumulation. */
    for (i = 0; i < 20; ++i) {
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
    }
    CHECK(b.grounded == 1u && b.bounds.max.y == 100.0f && b.velocity.y == 0.0f);

    /* gravity 0 still detects the floor. */
    b = body_at(0, 68, 32, 32);
    CHECK(pixelica_body2d_step(&b, DT, 0.0f, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
    CHECK(b.grounded == 1u && b.bounds.max.y == 100.0f);
    /* No floor: not grounded. */
    b = body_at(0, 68, 32, 32);
    CHECK(pixelica_body2d_step(&b, DT, 0.0f, JUMP, FALL, false, NULL, 0) == PIXELICA_OK);
    CHECK(b.grounded == 0u && b.contacts == 0u);

    /* Falling: semi-implicit gravity, lands exactly on the floor. */
    b = body_at(0, 0, 32, 32);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
    CHECK(b.velocity.y == 75.0f && b.bounds.max.y == 41.375f && b.grounded == 0u);
    for (i = 0; i < 3; ++i) {
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
    }
    CHECK(b.bounds.max.y == 100.0f && b.velocity.y == 0.0f);
    CHECK(b.grounded == 1u && b.contacts == PIXELICA_CONTACT2D_BOTTOM);

    /* Fall speed cap. */
    b = body_at(0, -5000, 32, 32);
    for (i = 0; i < 10; ++i) {
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, 100.0f, false, k_floor, 1) == PIXELICA_OK);
    }
    CHECK(b.velocity.y == 100.0f);
}

static void test_body_jump(void)
{
    PIXELICA_Body2D b = body_at(0, 68, 32, 32);
    int i;
    int landed = 0;

    /* First-tick jump from an exact floor contact. */
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, k_floor, 1) == PIXELICA_OK);
    CHECK(b.velocity.y == -220.0f && b.bounds.max.y == 72.5f && b.grounded == 0u && b.contacts == 0u);
    /* Held jump in the air: no air jump, gravity applies. */
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, k_floor, 1) == PIXELICA_OK);
    CHECK(b.velocity.y == -145.0f && b.bounds.max.y == 54.375f);
    for (i = 0; i < 20 && !landed; ++i) {
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_OK);
        CHECK(b.bounds.max.y <= 100.0f);
        landed = b.grounded != 0u;
    }
    CHECK(landed && b.bounds.max.y == 100.0f && b.velocity.y == 0.0f);

    /* In the air with zero velocity: jump is ignored. */
    b = body_at(0, 0, 32, 32);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, k_floor, 1) == PIXELICA_OK);
    CHECK(b.velocity.y == 75.0f);
    /* jump_speed 0 never leaves the floor. */
    b = body_at(0, 68, 32, 32);
    CHECK(pixelica_body2d_step(&b, DT, G, 0.0f, FALL, true, k_floor, 1) == PIXELICA_OK);
    CHECK(b.grounded == 1u && b.bounds.max.y == 100.0f);

    /* Ceiling: jump blocked, vertical speed zeroed, TOP contact. */
    {
        const PIXELICA_AABB2D obs[2] = {k_floor[0], box(-100, 50, 100, 60)};
        b = body_at(0, 68, 32, 32);
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, obs, 2) == PIXELICA_OK);
        CHECK(b.bounds.min.y == 60.0f && b.velocity.y == 0.0f);
        CHECK((b.contacts & PIXELICA_CONTACT2D_TOP) != 0u && b.grounded == 0u);
    }
}

static void test_body_walls_and_edges(void)
{
    const PIXELICA_AABB2D obs[2] = {k_floor[0], box(100, 0, 110, 100)};
    const PIXELICA_AABB2D ledge[1] = {box(-1000, 100, 40, 200)};
    PIXELICA_Body2D b = body_at(0, 68, 32, 32);
    int i;

    /* Run into a wall while grounded: slide stops, vx zeroed. */
    b.velocity.x = 1000.0f;
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, obs, 2) == PIXELICA_OK);
    CHECK(b.bounds.max.x == 100.0f && b.velocity.x == 0.0f && b.grounded == 1u);
    CHECK(b.contacts == (PIXELICA_CONTACT2D_RIGHT | PIXELICA_CONTACT2D_BOTTOM));

    /* Walk off a ledge: support is recomputed, no hovering. */
    b = body_at(0, 68, 32, 32);
    for (i = 0; i < 10 && b.bounds.min.x < 40.0f; ++i) {
        b.velocity.x = 100.0f;
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, ledge, 1) == PIXELICA_OK);
        if (b.bounds.min.x < 40.0f) {
            CHECK(b.grounded == 1u && b.bounds.max.y == 100.0f);
        }
    }
    CHECK(b.bounds.min.x >= 40.0f && b.grounded == 0u && b.bounds.max.y > 100.0f);
    /* No coyote/air jump after leaving the ledge. */
    {
        const float vy = b.velocity.y;
        CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, ledge, 1) == PIXELICA_OK);
        CHECK(b.velocity.y == vy + 75.0f);
    }

    /* High speed onto a thin floor: never tunnels. */
    {
        const PIXELICA_AABB2D thin[1] = {box(-100, 1000, 100, 1000.5f)};
        b = body_at(0, 0, 10, 10);
        for (i = 0; i < 40; ++i) {
            CHECK(pixelica_body2d_step(&b, 0.25f, 10000.0f, 0.0f, 10000.0f, false, thin, 1) == PIXELICA_OK);
            CHECK(b.bounds.max.y <= 1000.0f);
        }
        CHECK(b.bounds.max.y == 1000.0f && b.grounded == 1u);
    }
}

static void test_body_invalid(void)
{
    PIXELICA_Body2D b = body_at(0, 68, 32, 32);
    PIXELICA_Body2D saved;
    PIXELICA_AABB2D pen = box(10, 90, 20, 110);
    const float big = 1000.0f;

    b.velocity = v2(5, 6);
    saved = b;
#define BODY_UNCHANGED() CHECK(memcmp(&b, &saved, sizeof b) == 0)
    CHECK(pixelica_body2d_step(NULL, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, 0.0f, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, -DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, 0.26f, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, pixelica_t_nan(), G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, -1.0f, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, 10001.0f, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, pixelica_t_inf(), JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, -1.0f, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, pixelica_t_nan(), FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, 0.0f, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, 10001.0f, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, NULL, 1) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, true, &pen, 1) == PIXELICA_ERR_INVALID_ARG);
    BODY_UNCHANGED();

    b.velocity.x = pixelica_t_nan();
    saved = b;
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    BODY_UNCHANGED();
    b.velocity = v2(5, 6);
    b.bounds.max.x = b.bounds.min.x; /* zero width */
    saved = b;
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false, k_floor, 1) == PIXELICA_ERR_INVALID_ARG);
    BODY_UNCHANGED();

    /* Obstacle array aliasing the body: velocity/contacts/grounded read as a
     * valid, non-penetrating box (500,600)-(1000,1000). */
    b = body_at(0, 68, 32, 32);
    b.velocity = v2(500, 600);
    memcpy(&b.contacts, &big, sizeof big);
    memcpy(&b.grounded, &big, sizeof big);
    saved = b;
    CHECK(pixelica_aabb2d_is_valid((const PIXELICA_AABB2D *)(const void *)&b.velocity));
    CHECK(pixelica_body2d_step(&b, DT, G, JUMP, FALL, false,
                               (const PIXELICA_AABB2D *)(const void *)&b.velocity, 1) == PIXELICA_ERR_INVALID_ARG);
    BODY_UNCHANGED();
#undef BODY_UNCHANGED
}

int main(void)
{
    test_validity_and_overlap();
    test_move_free_and_walls();
    test_move_vertical_and_seams();
    test_move_no_tunneling();
    test_move_invalid();
    test_body_grounding();
    test_body_jump();
    test_body_walls_and_edges();
    test_body_invalid();
    return pixelica_t_finish("test_collision2d");
}
