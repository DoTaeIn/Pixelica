/* Chunk streaming common API: checked floor-division coordinates, inclusive
 * ranges, bounded caller-owned slots, unload-first/z-y-x ordered updates,
 * budgets, failed-load retry, clear and callback reentrancy rejection.
 * Common-library test: links Pixelica::common only and includes no 3D header. */
#include <float.h>
#include <stdbool.h>
#include <stdint.h>

#include "pixelica/chunks.h"
#include "test_util.h"

#define MAX_EVENTS 64
#define SLOTS 8

typedef struct Event {
    char kind; /* 'L' load, 'U' unload */
    PIXELICA_ChunkCoord c;
} Event;

typedef struct Env {
    Event log[MAX_EVENTS];
    size_t events;
    bool fail_enabled;
    PIXELICA_ChunkCoord fail_at;
    PIXELICA_Status fail_status;
    /* Reentrancy probe: when set, callbacks call back into the API. */
    PIXELICA_Chunks *reenter;
    int reentry_checked;
    int reentry_rejected;
} Env;

static PIXELICA_ChunkCoord cc(int32_t x, int32_t y, int32_t z)
{
    PIXELICA_ChunkCoord c;
    c.x = x;
    c.y = y;
    c.z = z;
    return c;
}

static PIXELICA_ChunkRange rng(PIXELICA_ChunkCoord lo, PIXELICA_ChunkCoord hi)
{
    PIXELICA_ChunkRange r;
    r.min = lo;
    r.max = hi;
    return r;
}

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static bool same(PIXELICA_ChunkCoord a, PIXELICA_ChunkCoord b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static PIXELICA_Status on_load(void *user, PIXELICA_ChunkCoord c);
static void on_unload(void *user, PIXELICA_ChunkCoord c);

static void probe_reentry(Env *env)
{
    PIXELICA_ChunkRange r = rng(cc(0, 0, 0), cc(0, 0, 0));
    size_t ops = 0;

    if (env->reenter == NULL) {
        return;
    }
    env->reentry_checked += 4;
    if (pixelica_chunks_init(env->reenter, env->reenter->slots,
            env->reenter->capacity, on_load, on_unload, env) != PIXELICA_OK) {
        ++env->reentry_rejected;
    }
    if (pixelica_chunks_set_range(env->reenter, r) != PIXELICA_OK) {
        ++env->reentry_rejected;
    }
    if (pixelica_chunks_update(env->reenter, 0, &ops) != PIXELICA_OK) {
        ++env->reentry_rejected;
    }
    if (pixelica_chunks_clear(env->reenter) != PIXELICA_OK) {
        ++env->reentry_rejected;
    }
}

static PIXELICA_Status on_load(void *user, PIXELICA_ChunkCoord c)
{
    Env *env = (Env *)user;

    if (env->events < MAX_EVENTS) {
        env->log[env->events].kind = 'L';
        env->log[env->events].c = c;
        ++env->events;
    }
    probe_reentry(env);
    if (env->fail_enabled && same(c, env->fail_at)) {
        return env->fail_status;
    }
    return PIXELICA_OK;
}

static void on_unload(void *user, PIXELICA_ChunkCoord c)
{
    Env *env = (Env *)user;

    if (env->events < MAX_EVENTS) {
        env->log[env->events].kind = 'U';
        env->log[env->events].c = c;
        ++env->events;
    }
    probe_reentry(env);
}

static size_t count_events(const Env *env, char kind, PIXELICA_ChunkCoord c)
{
    size_t i;
    size_t n = 0;

    for (i = 0; i < env->events; ++i) {
        if (env->log[i].kind == kind && same(env->log[i].c, c)) {
            ++n;
        }
    }
    return n;
}

static void reset_log(Env *env)
{
    env->events = 0;
}

static void setup(PIXELICA_Chunks *ch, PIXELICA_ChunkSlot *slots, size_t cap,
                  Env *env)
{
    memset(env, 0, sizeof *env);
    memset(slots, 0, SLOTS * sizeof *slots);
    env->fail_status = PIXELICA_ERR_NOT_FOUND;
    CHECK(pixelica_chunks_init(ch, slots, cap, on_load, on_unload, env) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(ch->count, 0);
}

static void test_coord(void)
{
    PIXELICA_ChunkCoord c = cc(7, 7, 7);
    const PIXELICA_ChunkCoord keep = cc(7, 7, 7);
    PIXELICA_Vec3 size = v3(16.0f, 16.0f, 16.0f);

    CHECK(pixelica_chunk_coord(v3(0.0f, 15.99f, 16.0f), size, &c) == PIXELICA_OK);
    CHECK(c.x == 0 && c.y == 0 && c.z == 1);

    /* Floor, not truncation, for negatives; exact multiples are boundaries. */
    CHECK(pixelica_chunk_coord(v3(-0.1f, -16.0f, -16.5f), size, &c) == PIXELICA_OK);
    CHECK(c.x == -1 && c.y == -1 && c.z == -2);
    CHECK(pixelica_chunk_coord(v3(-32.0f, -31.99f, 32.0f), size, &c) == PIXELICA_OK);
    CHECK(c.x == -2 && c.y == -2 && c.z == 2);

    /* Non-uniform size. */
    CHECK(pixelica_chunk_coord(v3(5.0f, -5.0f, 5.0f), v3(2.0f, 10.0f, 0.5f), &c) ==
          PIXELICA_OK);
    CHECK(c.x == 2 && c.y == -1 && c.z == 10);

    /* int32 limits: exact INT32_MIN fits, 2^31 and beyond do not. */
    size = v3(1.0f, 1.0f, 1.0f);
    CHECK(pixelica_chunk_coord(v3(-2147483648.0f, 2147483520.0f, 0.0f), size, &c) ==
          PIXELICA_OK);
    CHECK(c.x == INT32_MIN && c.y == 2147483520 && c.z == 0);

    c = keep;
    CHECK(pixelica_chunk_coord(v3(2147483648.0f, 0.0f, 0.0f), size, &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(0.0f, -2147483904.0f, 0.0f), size, &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(0.0f, 0.0f, 1e20f), size, &c) != PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1e10f, 0.0f, 0.0f), v3(1e-3f, 1.0f, 1.0f), &c) !=
          PIXELICA_OK);
    CHECK(same(c, keep));

    /* NaN/Inf position and invalid chunk sizes. */
    CHECK(pixelica_chunk_coord(v3(pixelica_t_nan(), 0.0f, 0.0f), size, &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(0.0f, pixelica_t_inf(), 0.0f), size, &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(0.0f, 0.0f, -pixelica_t_inf()), size, &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1.0f, 1.0f, 1.0f), v3(0.0f, 1.0f, 1.0f), &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1.0f, 1.0f, 1.0f), v3(1.0f, -1.0f, 1.0f), &c) !=
          PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1.0f, 1.0f, 1.0f),
                               v3(1.0f, 1.0f, pixelica_t_nan()), &c) != PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1.0f, 1.0f, 1.0f),
                               v3(pixelica_t_inf(), 1.0f, 1.0f), &c) != PIXELICA_OK);
    CHECK(pixelica_chunk_coord(v3(1.0f, 1.0f, 1.0f), size, NULL) != PIXELICA_OK);
    CHECK(same(c, keep));
}

static void test_range(void)
{
    PIXELICA_ChunkRange r;
    PIXELICA_ChunkRange keep = rng(cc(9, 9, 9), cc(8, 8, 8));

    CHECK(pixelica_chunk_range(cc(-2, 0, 5), cc(1, 0, 2), &r) == PIXELICA_OK);
    CHECK(r.min.x == -3 && r.min.y == 0 && r.min.z == 3);
    CHECK(r.max.x == -1 && r.max.y == 0 && r.max.z == 7);

    /* Extremes that still fit. */
    CHECK(pixelica_chunk_range(cc(INT32_MAX, INT32_MIN, 0), cc(0, 0, 0), &r) ==
          PIXELICA_OK);
    CHECK(r.min.x == INT32_MAX && r.max.x == INT32_MAX);
    CHECK(r.min.y == INT32_MIN && r.max.y == INT32_MIN);
    CHECK(pixelica_chunk_range(cc(INT32_MAX - 1, INT32_MIN + 1, 0), cc(1, 1, 0),
                               &r) == PIXELICA_OK);
    CHECK(r.max.x == INT32_MAX && r.min.y == INT32_MIN);

    /* Inclusive overflow and invalid radius preserve the output. */
    r = keep;
    CHECK(pixelica_chunk_range(cc(INT32_MAX, 0, 0), cc(1, 0, 0), &r) != PIXELICA_OK);
    CHECK(pixelica_chunk_range(cc(0, INT32_MIN, 0), cc(0, 1, 0), &r) != PIXELICA_OK);
    CHECK(pixelica_chunk_range(cc(0, 0, 1), cc(0, 0, INT32_MAX), &r) != PIXELICA_OK);
    CHECK(pixelica_chunk_range(cc(0, 0, -2), cc(0, 0, INT32_MAX), &r) != PIXELICA_OK);
    CHECK(pixelica_chunk_range(cc(0, 0, 0), cc(-1, 0, 0), &r) != PIXELICA_OK);
    CHECK(pixelica_chunk_range(cc(0, 0, 0), cc(0, 0, 0), NULL) != PIXELICA_OK);
    CHECK(same(r.min, keep.min) && same(r.max, keep.max));
}

static void test_init_validation(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;

    memset(&env, 0, sizeof env);
    memset(slots, 0, sizeof slots);
    CHECK(pixelica_chunks_init(NULL, slots, SLOTS, on_load, on_unload, &env) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_init(&ch, NULL, SLOTS, on_load, on_unload, &env) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_init(&ch, slots, 0, on_load, on_unload, &env) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_init(&ch, slots, SLOTS, NULL, on_unload, &env) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_init(&ch, slots, SLOTS, on_load, NULL, &env) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_init(&ch, (PIXELICA_ChunkSlot *)(void *)&ch, 1u,
        on_load, on_unload, &env) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ch.state, 0);

    /* NULL userdata is allowed; the struct is caller-owned and reusable. */
    CHECK(pixelica_chunks_init(&ch, slots, SLOTS, on_load, on_unload, NULL) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 0);
    CHECK(!pixelica_chunks_contains(&ch, cc(0, 0, 0)));
    CHECK(!pixelica_chunks_contains(NULL, cc(0, 0, 0)));
    CHECK(pixelica_chunks_clear(NULL) != PIXELICA_OK);
}

static void test_unset_range_and_order(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 99;
    size_t i;
    static const int32_t ex[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    static const int32_t ey[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    static const int32_t ez[8] = {0, 0, 0, 0, 1, 1, 1, 1};

    setup(&ch, slots, SLOTS, &env);

    /* No desired range yet: update is a no-op. */
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 0);
    CHECK_EQ_LONG(env.events, 0);
    CHECK_EQ_LONG(ch.count, 0);
    CHECK(pixelica_chunks_update(&ch, 0, &ch.count) == PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ch.count, 0);

    /* set_range alone does not call back; update loads z outer, x inner. */
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(1, 1, 1))) ==
          PIXELICA_OK);
    CHECK_EQ_LONG(env.events, 0);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 8);
    CHECK_EQ_LONG(ch.count, 8);
    CHECK_EQ_LONG(env.events, 8);
    CHECK(pixelica_chunks_init(&ch, slots, SLOTS, on_load, on_unload, &env) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ch.count, 8);
    for (i = 0; i < 8 && i < env.events; ++i) {
        CHECK(env.log[i].kind == 'L');
        CHECK(same(env.log[i].c, cc(ex[i], ey[i], ez[i])));
        CHECK(pixelica_chunks_contains(&ch, cc(ex[i], ey[i], ez[i])));
    }
    CHECK(!pixelica_chunks_contains(&ch, cc(2, 0, 0)));
    CHECK(!pixelica_chunks_contains(&ch, cc(-1, 0, 0)));
    CHECK(!pixelica_chunks_contains(&ch, cc(0, 0, 2)));
}

static void test_same_range_and_movement(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;
    PIXELICA_ChunkRange r = rng(cc(0, 0, 0), cc(3, 0, 0));
    int32_t x;

    setup(&ch, slots, 4, &env);
    CHECK(pixelica_chunks_set_range(&ch, r) == PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 4);
    CHECK_EQ_LONG(ch.count, 4);

    /* Unchanged range: no callbacks, no duplicate loads. */
    reset_log(&env);
    ops = 99;
    CHECK(pixelica_chunks_set_range(&ch, r) == PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 0);
    CHECK_EQ_LONG(env.events, 0);
    CHECK_EQ_LONG(ch.count, 4);

    /* Overlapping move with full capacity: unloads come first and the
     * retained chunks 2,3 are neither unloaded nor reloaded. */
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(2, 0, 0), cc(5, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 4);
    CHECK_EQ_LONG(env.events, 4);
    CHECK(env.log[0].kind == 'U' && env.log[1].kind == 'U');
    CHECK(env.log[2].kind == 'L' && env.log[3].kind == 'L');
    CHECK(same(env.log[2].c, cc(4, 0, 0)) && same(env.log[3].c, cc(5, 0, 0)));
    CHECK_EQ_LONG(count_events(&env, 'U', cc(0, 0, 0)), 1);
    CHECK_EQ_LONG(count_events(&env, 'U', cc(1, 0, 0)), 1);
    CHECK_EQ_LONG(count_events(&env, 'U', cc(2, 0, 0)), 0);
    CHECK_EQ_LONG(count_events(&env, 'L', cc(3, 0, 0)), 0);
    CHECK_EQ_LONG(ch.count, 4);
    for (x = 0; x < 7; ++x) {
        CHECK(pixelica_chunks_contains(&ch, cc(x, 0, 0)) == (x >= 2 && x <= 5));
    }

    /* Disjoint move (including negative coordinates) reuses all slots. */
    reset_log(&env);
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(-4, 0, 0), cc(-1, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 8);
    CHECK_EQ_LONG(ch.count, 4);
    CHECK(env.log[3].kind == 'U' && env.log[4].kind == 'L');
    CHECK(pixelica_chunks_contains(&ch, cc(-4, 0, 0)));
    CHECK(!pixelica_chunks_contains(&ch, cc(5, 0, 0)));
}

static void test_capacity_and_invalid_range(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;

    setup(&ch, slots, 4, &env);
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(1, 1, 0))) ==
          PIXELICA_OK); /* volume 4 == capacity */
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 4);
    reset_log(&env);

    /* Rejected ranges keep both the desired range and the loaded set. */
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(4, 0, 0))) !=
          PIXELICA_OK); /* volume 5 */
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(2, 0, 0), cc(1, 0, 0))) !=
          PIXELICA_OK); /* min > max */
    CHECK(pixelica_chunks_set_range(
              &ch, rng(cc(INT32_MIN, INT32_MIN, INT32_MIN),
                       cc(INT32_MAX, INT32_MAX, INT32_MAX))) != PIXELICA_OK);
    CHECK(pixelica_chunks_set_range(NULL, rng(cc(0, 0, 0), cc(0, 0, 0))) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 0);
    CHECK_EQ_LONG(env.events, 0);
    CHECK_EQ_LONG(ch.count, 4);
    CHECK(pixelica_chunks_contains(&ch, cc(1, 1, 0)));

    CHECK(pixelica_chunks_update(NULL, 0, &ops) != PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, NULL) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 4);

    /* Rejected range on a fresh set stays unset. */
    CHECK(pixelica_chunks_clear(&ch) == PIXELICA_OK);
    setup(&ch, slots, 2, &env);
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(2, 0, 0))) !=
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(env.events, 0);
}

static void test_budget(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;

    setup(&ch, slots, 4, &env);
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(3, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 3, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 3);
    CHECK_EQ_LONG(ch.count, 3);
    CHECK(pixelica_chunks_update(&ch, 3, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 1);
    CHECK_EQ_LONG(ch.count, 4);
    CHECK(pixelica_chunks_update(&ch, 3, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 0);
    CHECK_EQ_LONG(env.events, 4);

    /* Unloads and loads share one budget; unloads are spent first. */
    reset_log(&env);
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(4, 0, 0), cc(7, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 2, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 2);
    CHECK_EQ_LONG(env.events, 2);
    CHECK(env.log[0].kind == 'U' && env.log[1].kind == 'U');
    CHECK_EQ_LONG(ch.count, 2);
    CHECK(pixelica_chunks_update(&ch, 3, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 3);
    CHECK(env.log[2].kind == 'U' && env.log[3].kind == 'U');
    CHECK(env.log[4].kind == 'L' && same(env.log[4].c, cc(4, 0, 0)));
    CHECK_EQ_LONG(ch.count, 1);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK); /* unlimited */
    CHECK_EQ_LONG(ops, 3);
    CHECK_EQ_LONG(ch.count, 4);
    CHECK(pixelica_chunks_contains(&ch, cc(7, 0, 0)));
}

static void test_failed_load_retry(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;

    setup(&ch, slots, 4, &env);
    env.fail_enabled = true;
    env.fail_at = cc(2, 0, 0);
    env.fail_status = PIXELICA_ERR_NOT_FOUND;
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(3, 0, 0))) ==
          PIXELICA_OK);

    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_chunks_contains(&ch, cc(0, 0, 0)));
    CHECK(pixelica_chunks_contains(&ch, cc(1, 0, 0)));
    CHECK(!pixelica_chunks_contains(&ch, cc(2, 0, 0)));
    CHECK(ch.count == 2 || ch.count == 3);
    CHECK_EQ_LONG(count_events(&env, 'L', cc(2, 0, 0)), 1);

    /* A second failure keeps the same state and does not leak slots. */
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_ERR_NOT_FOUND);
    CHECK(!pixelica_chunks_contains(&ch, cc(2, 0, 0)));
    CHECK(pixelica_chunks_contains(&ch, cc(0, 0, 0)));

    /* Retry succeeds without reloading the preserved chunks. */
    env.fail_enabled = false;
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 4);
    CHECK(pixelica_chunks_contains(&ch, cc(2, 0, 0)));
    CHECK(pixelica_chunks_contains(&ch, cc(3, 0, 0)));
    CHECK_EQ_LONG(count_events(&env, 'L', cc(0, 0, 0)), 1);
    CHECK_EQ_LONG(count_events(&env, 'L', cc(1, 0, 0)), 1);
    CHECK_EQ_LONG(count_events(&env, 'L', cc(3, 0, 0)), 1);
    CHECK(count_events(&env, 'L', cc(2, 0, 0)) >= 2);
    CHECK_EQ_LONG(count_events(&env, 'U', cc(2, 0, 0)), 0);
}

static void test_clear(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;
    int32_t x;

    setup(&ch, slots, 4, &env);
    CHECK(pixelica_chunks_clear(&ch) == PIXELICA_OK); /* empty: no callbacks */
    CHECK_EQ_LONG(env.events, 0);

    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(2, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    reset_log(&env);
    CHECK(pixelica_chunks_clear(&ch) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 0);
    CHECK_EQ_LONG(env.events, 3);
    for (x = 0; x < 3; ++x) {
        CHECK_EQ_LONG(count_events(&env, 'U', cc(x, 0, 0)), 1);
        CHECK(!pixelica_chunks_contains(&ch, cc(x, 0, 0)));
    }
    CHECK(pixelica_chunks_clear(&ch) == PIXELICA_OK);
    CHECK_EQ_LONG(env.events, 3); /* idempotent */
}

static void test_reentrancy(void)
{
    PIXELICA_Chunks ch = {0};
    PIXELICA_ChunkSlot slots[SLOTS];
    Env env;
    size_t ops = 0;

    setup(&ch, slots, 4, &env);
    env.reenter = &ch;
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(1, 0, 0))) ==
          PIXELICA_OK);

    /* Load callbacks may not set_range/update/clear; state is unaffected. */
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ops, 2);
    CHECK_EQ_LONG(ch.count, 2);
    CHECK_EQ_LONG(env.reentry_checked, 8);
    CHECK_EQ_LONG(env.reentry_rejected, 8);
    CHECK_EQ_LONG(count_events(&env, 'L', cc(0, 0, 0)), 1);

    /* Same for unload callbacks during a range move and during clear. */
    env.reentry_checked = 0;
    env.reentry_rejected = 0;
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(5, 0, 0), cc(6, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 2);
    CHECK_EQ_LONG(env.reentry_rejected, env.reentry_checked);
    CHECK(env.reentry_checked >= 12);
    CHECK(pixelica_chunks_contains(&ch, cc(5, 0, 0)));
    CHECK(!pixelica_chunks_contains(&ch, cc(0, 0, 0)));

    env.reentry_checked = 0;
    env.reentry_rejected = 0;
    CHECK(pixelica_chunks_clear(&ch) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 0);
    CHECK_EQ_LONG(env.reentry_checked, 8);
    CHECK_EQ_LONG(env.reentry_rejected, 8);

    /* The API is usable again once the outer call returned. */
    env.reenter = NULL;
    CHECK(pixelica_chunks_set_range(&ch, rng(cc(0, 0, 0), cc(0, 0, 0))) ==
          PIXELICA_OK);
    CHECK(pixelica_chunks_update(&ch, 0, &ops) == PIXELICA_OK);
    CHECK_EQ_LONG(ch.count, 1);
}

int main(void)
{
    test_coord();
    test_range();
    test_init_validation();
    test_unset_range_and_order();
    test_same_range_and_movement();
    test_capacity_and_invalid_range();
    test_budget();
    test_failed_load_retry();
    test_clear();
    test_reentrancy();
    return pixelica_t_finish("test_chunks");
}
