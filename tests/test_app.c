/*
 * Pixelica - tests for pixelica/app.h: every legal and illegal transition,
 * the callback order, checkpoint write/restore through pixelica/storage.h,
 * I/O failures during suspend and shutdown, and re-entrancy.
 *
 * The "game" here is two counters plus an explicit little-endian
 * serialization, exactly what a real game is expected to provide: the engine
 * never snapshots the process.
 */
#include <string.h>

#include "pixelica/app.h"
#include "test_util.h"

#define GAME_SCHEMA 2u
#define GAME_BYTES 8u

/* ---- the test game ------------------------------------------------------ */

typedef struct Game {
    uint32_t level;
    uint32_t score;
    char log[128]; /* callback trace, one letter per call */
    size_t log_len;
    PIXELICA_Status fail_init;
    PIXELICA_Status fail_save;
    PIXELICA_Status fail_load;
    size_t save_size_override; /* non-zero: report this many bytes */
    PIXELICA_App *reenter;     /* non-NULL: call back into the app from on_pause */
    PIXELICA_Status reenter_status;
} Game;

static void game_log(Game *g, char c)
{
    if (g->log_len + 1u < sizeof g->log) {
        g->log[g->log_len++] = c;
        g->log[g->log_len] = '\0';
    }
}

static PIXELICA_Status game_on_init(void *ud)
{
    Game *g = (Game *)ud;

    game_log(g, 'i');
    return g->fail_init;
}

static PIXELICA_Status game_on_pause(void *ud)
{
    Game *g = (Game *)ud;

    game_log(g, 'p');
    if (g->reenter != NULL) {
        /* Every transition is refused while a callback is running. */
        g->reenter_status = pixelica_app_resume(g->reenter);
    }
    return PIXELICA_OK;
}

static PIXELICA_Status game_on_resume(void *ud)
{
    game_log((Game *)ud, 'r');
    return PIXELICA_OK;
}

static PIXELICA_Status game_on_suspend(void *ud)
{
    game_log((Game *)ud, 'z');
    return PIXELICA_OK;
}

static PIXELICA_Status game_on_shutdown(void *ud)
{
    game_log((Game *)ud, 'x');
    return PIXELICA_OK;
}

static PIXELICA_Status game_save(void *ud, void *out, size_t capacity, size_t *out_size,
                                 uint32_t *out_schema_version)
{
    Game *g = (Game *)ud;
    uint8_t *p = (uint8_t *)out;

    game_log(g, 's');
    if (g->fail_save != PIXELICA_OK) {
        return g->fail_save;
    }
    if (g->save_size_override != 0u) {
        *out_size = g->save_size_override; /* a game that lies about its size */
        *out_schema_version = GAME_SCHEMA;
        return PIXELICA_OK;
    }
    if (capacity < GAME_BYTES) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    p[0] = (uint8_t)(g->level & 0xFFu);
    p[1] = (uint8_t)((g->level >> 8) & 0xFFu);
    p[2] = (uint8_t)((g->level >> 16) & 0xFFu);
    p[3] = (uint8_t)(g->level >> 24);
    p[4] = (uint8_t)(g->score & 0xFFu);
    p[5] = (uint8_t)((g->score >> 8) & 0xFFu);
    p[6] = (uint8_t)((g->score >> 16) & 0xFFu);
    p[7] = (uint8_t)(g->score >> 24);
    *out_size = GAME_BYTES;
    *out_schema_version = GAME_SCHEMA;
    return PIXELICA_OK;
}

static PIXELICA_Status game_load(void *ud, uint32_t schema, const void *data, size_t size)
{
    Game *g = (Game *)ud;
    const uint8_t *p = (const uint8_t *)data;

    game_log(g, 'l');
    if (g->fail_load != PIXELICA_OK) {
        return g->fail_load;
    }
    if (schema != GAME_SCHEMA || size != GAME_BYTES) {
        return PIXELICA_ERR_BAD_ASSET; /* the game owns version handling */
    }
    g->level = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    g->score = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) |
               ((uint32_t)p[7] << 24);
    return PIXELICA_OK;
}

/* ---- mock blob store ---------------------------------------------------- */

#define STORE_BYTES 64u

typedef struct Store {
    uint8_t data[PIXELICA_STORAGE_SLOT_MAX][STORE_BYTES];
    size_t size[PIXELICA_STORAGE_SLOT_MAX];
    PIXELICA_Status write_fail;
    PIXELICA_Status read_fail;
} Store;

static PIXELICA_Status store_write(void *ud, const char *id, uint32_t slot,
                                   const void *data, size_t size)
{
    Store *s = (Store *)ud;

    (void)id;
    if (s->write_fail != PIXELICA_OK) {
        return s->write_fail;
    }
    if (size > STORE_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    memcpy(s->data[slot], data, size);
    s->size[slot] = size;
    return PIXELICA_OK;
}

static PIXELICA_Status store_read(void *ud, const char *id, uint32_t slot, void *out,
                                  size_t capacity, size_t *out_size)
{
    Store *s = (Store *)ud;

    (void)id;
    if (s->read_fail != PIXELICA_OK) {
        return s->read_fail;
    }
    if (s->size[slot] == 0u) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    *out_size = s->size[slot];
    if (s->size[slot] > capacity) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    memcpy(out, s->data[slot], s->size[slot]);
    return PIXELICA_OK;
}

/* ---- fixture ------------------------------------------------------------ */

typedef struct Fixture {
    Game game;
    Store store;
    PIXELICA_Storage storage;
    PIXELICA_StorageOps ops;
    PIXELICA_App app;
    uint8_t scratch[GAME_BYTES];
    PIXELICA_AppConfig config;
} Fixture;

/* Resets the game and app but keeps whatever the store holds (that is what a
 * new process would see). */
static void fixture_reset_game(Fixture *f)
{
    memset(&f->game, 0, sizeof f->game);
    memset(&f->app, 0, sizeof f->app);
}

static void fixture_init(Fixture *f)
{
    memset(f, 0, sizeof *f);
    f->ops.userdata = &f->store;
    f->ops.write = store_write;
    f->ops.read = store_read;
}

/* The storage scratch must hold the envelope around the game payload. */
static uint8_t g_storage_scratch[GAME_BYTES + PIXELICA_SAVE_OVERHEAD_BYTES];

static void fixture_bind(Fixture *f, bool restore, bool on_suspend, bool on_shutdown)
{
    memset(&f->config, 0, sizeof f->config);
    f->config.callbacks.on_init = game_on_init;
    f->config.callbacks.on_pause = game_on_pause;
    f->config.callbacks.on_resume = game_on_resume;
    f->config.callbacks.on_suspend = game_on_suspend;
    f->config.callbacks.on_shutdown = game_on_shutdown;
    f->config.callbacks.save_checkpoint = game_save;
    f->config.callbacks.load_checkpoint = game_load;
    f->config.callbacks.userdata = &f->game;
    f->config.storage = &f->storage;
    f->config.checkpoint_slot = 0u;
    f->config.scratch = f->scratch;
    f->config.scratch_size = sizeof f->scratch;
    f->config.restore_on_init = restore;
    f->config.checkpoint_on_suspend = on_suspend;
    f->config.checkpoint_on_shutdown = on_shutdown;
}

/* ---- tests -------------------------------------------------------------- */

static void test_config_validation(void)
{
    Fixture f;
    PIXELICA_AppConfig cfg;

    fixture_init(&f);
    CHECK_EQ_LONG(pixelica_storage_init(&f.storage, "test-app", &f.ops, g_storage_scratch,
                                        sizeof g_storage_scratch),
                  PIXELICA_OK);
    fixture_bind(&f, false, false, false);

    CHECK_EQ_LONG(pixelica_app_init(NULL, &f.config), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, NULL), PIXELICA_ERR_INVALID_ARG);

    cfg = f.config;
    cfg.checkpoint_slot = PIXELICA_STORAGE_SLOT_MAX;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_ERR_INVALID_ARG);

    cfg = f.config;
    cfg.scratch = NULL; /* size != 0 */
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_ERR_INVALID_ARG);

    cfg = f.config;
    cfg.storage = NULL;
    cfg.checkpoint_on_suspend = true;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_ERR_INVALID_ARG);

    cfg = f.config;
    cfg.restore_on_init = true;
    cfg.callbacks.load_checkpoint = NULL;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_ERR_INVALID_ARG);

    cfg = f.config;
    cfg.checkpoint_on_shutdown = true;
    cfg.callbacks.save_checkpoint = NULL;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_ERR_INVALID_ARG);

    /* None of the rejected configs started anything. */
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_UNINITIALIZED);
    CHECK_EQ_LONG(f.game.log_len, 0);
    CHECK(!pixelica_app_should_update(&f.app));
    CHECK_EQ_LONG(pixelica_app_state(NULL), PIXELICA_APP_UNINITIALIZED);

    /* A failing on_init leaves the app uninitialised. */
    f.game.fail_init = PIXELICA_ERR_NO_MEMORY;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_UNINITIALIZED);
    f.game.fail_init = PIXELICA_OK;

    /* A storage-less app is legal as long as it asks for no checkpoints. */
    cfg = f.config;
    cfg.storage = NULL;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &cfg), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_restore(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_OK); /* notification only */
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_SUSPENDED);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
}

static void test_transitions(void)
{
    Fixture f;
    PIXELICA_AppStats stats;

    fixture_init(&f);
    CHECK_EQ_LONG(pixelica_storage_init(&f.storage, "test-app", &f.ops, g_storage_scratch,
                                        sizeof g_storage_scratch),
                  PIXELICA_OK);
    fixture_bind(&f, false, false, false);

    /* Nothing is allowed before init. */
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_stats(&f.app, &stats), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_RUNNING);
    CHECK(pixelica_app_should_update(&f.app));
    CHECK(strcmp(f.game.log, "i") == 0);
    /* init on a live app is refused and changes nothing. */
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_ERR_INVALID_ARG);
    CHECK(strcmp(f.game.log, "i") == 0);

    /* running -> paused -> running */
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_PAUSED);
    CHECK(!pixelica_app_should_update(&f.app));
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_OK); /* repeated event */
    CHECK(strcmp(f.game.log, "ip") == 0);                   /* only one callback */
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_RUNNING);
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_OK); /* repeated event */
    CHECK(strcmp(f.game.log, "ipr") == 0);

    /* running -> suspended -> running */
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_SUSPENDED);
    CHECK(!pixelica_app_should_update(&f.app));
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_OK); /* repeated event */
    CHECK(strcmp(f.game.log, "iprz") == 0);
    /* pause and restore are not legal while suspended. */
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_RUNNING);

    /* paused -> suspended -> PAUSED again (not running) */
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_SUSPENDED);
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_PAUSED);
    CHECK(!pixelica_app_should_update(&f.app));

    CHECK_EQ_LONG(pixelica_app_stats(&f.app, &stats), PIXELICA_OK);
    CHECK_EQ_LONG(stats.pauses, 2);
    CHECK_EQ_LONG(stats.resumes, 3);
    CHECK_EQ_LONG(stats.suspends, 2);
    CHECK_EQ_LONG(stats.checkpoints, 0); /* no checkpoint option was set */

    /* shutdown from PAUSED, then nothing works any more. */
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_STOPPED);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_ERR_INVALID_ARG);
    CHECK(!pixelica_app_should_update(&f.app));
    CHECK(strcmp(f.game.log, "iprzrpzrx") == 0);

    /* A stopped app can be started again. */
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_RUNNING);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
}

static void test_reentrancy(void)
{
    Fixture f;

    fixture_init(&f);
    CHECK_EQ_LONG(pixelica_storage_init(&f.storage, "test-app", &f.ops, g_storage_scratch,
                                        sizeof g_storage_scratch),
                  PIXELICA_OK);
    fixture_bind(&f, false, false, false);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);

    f.game.reenter = &f.app;
    f.game.reenter_status = PIXELICA_OK;
    CHECK_EQ_LONG(pixelica_app_pause(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(f.game.reenter_status, PIXELICA_ERR_INVALID_ARG);
    /* The nested call changed nothing: the app really is paused. */
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_PAUSED);
    f.game.reenter = NULL;
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
}

/* The whole point: suspend writes a checkpoint, a brand-new app (= a fresh
 * process, the store standing in for the filesystem) restores it. */
static void test_checkpoint_round_trip(void)
{
    Fixture f;
    PIXELICA_AppStats stats;

    fixture_init(&f);
    CHECK_EQ_LONG(pixelica_storage_init(&f.storage, "test-app", &f.ops, g_storage_scratch,
                                        sizeof g_storage_scratch),
                  PIXELICA_OK);

    /* --- first run: nothing stored yet --- */
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK(!pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(pixelica_app_restore_status(&f.app), PIXELICA_ERR_NOT_FOUND);
    CHECK(strcmp(f.game.log, "i") == 0); /* no load callback for an empty slot */

    f.game.level = 3u;
    f.game.score = 1234u;
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_checkpoint_status(&f.app), PIXELICA_OK);
    CHECK(strcmp(f.game.log, "isz") == 0); /* save BEFORE the notification */
    CHECK_EQ_LONG(f.store.size[0],
                  (long long)(GAME_BYTES + PIXELICA_SAVE_OVERHEAD_BYTES));
    CHECK(memcmp(f.store.data[0], "PXSV", 4u) == 0);
    /* The process ends here without a shutdown: the host killed it. */

    /* --- second run: a fresh app and a fresh game --- */
    fixture_reset_game(&f);
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK(pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(pixelica_app_restore_status(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(f.game.level, 3);
    CHECK_EQ_LONG(f.game.score, 1234);
    CHECK(strcmp(f.game.log, "li") == 0); /* restore runs before on_init */
    CHECK_EQ_LONG(pixelica_app_stats(&f.app, &stats), PIXELICA_OK);
    CHECK_EQ_LONG(stats.restores, 1);

    /* An explicit checkpoint in the middle of play. */
    f.game.score = 2000u;
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_OK);
    /* Shutdown writes one more. */
    f.game.score = 3000u;
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);

    /* --- third run: it sees the shutdown checkpoint --- */
    fixture_reset_game(&f);
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK(pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(f.game.score, 3000);
    CHECK_EQ_LONG(f.game.level, 3);

    /* pixelica_app_restore re-reads the same slot mid-game. */
    f.game.score = 1u;
    CHECK_EQ_LONG(pixelica_app_restore(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(f.game.score, 3000);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
}

static void test_checkpoint_failures(void)
{
    Fixture f;
    PIXELICA_AppStats stats;

    fixture_init(&f);
    CHECK_EQ_LONG(pixelica_storage_init(&f.storage, "test-app", &f.ops, g_storage_scratch,
                                        sizeof g_storage_scratch),
                  PIXELICA_OK);
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    f.game.level = 1u;
    f.game.score = 10u;

    /* The platform cannot write: suspend still suspends, and says why. */
    f.store.write_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_app_suspend(&f.app), PIXELICA_ERR_PLATFORM);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_SUSPENDED);
    CHECK_EQ_LONG(pixelica_app_checkpoint_status(&f.app), PIXELICA_ERR_PLATFORM);
    CHECK(strcmp(f.game.log, "isz") == 0); /* on_suspend still ran */
    CHECK_EQ_LONG(f.store.size[0], 0);     /* nothing was stored */
    f.store.write_fail = PIXELICA_OK;
    CHECK_EQ_LONG(pixelica_app_resume(&f.app), PIXELICA_OK);

    /* The game itself refuses to serialize. */
    f.game.fail_save = PIXELICA_ERR_CAPACITY;
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_ERR_CAPACITY);
    CHECK_EQ_LONG(f.store.size[0], 0);
    f.game.fail_save = PIXELICA_OK;

    /* A game that reports more bytes than it was given cannot corrupt a save. */
    f.game.save_size_override = sizeof f.scratch + 1u;
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(f.store.size[0], 0);
    f.game.save_size_override = 0u;

    /* Shutdown behaves the same way: it stops, then reports. */
    f.store.write_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_ERR_PLATFORM);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_STOPPED);
    f.store.write_fail = PIXELICA_OK;

    /* A good save, then a corrupt store: the next run starts fresh. */
    fixture_reset_game(&f);
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    f.game.level = 5u;
    f.game.score = 500u;
    CHECK_EQ_LONG(pixelica_app_checkpoint(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_stats(&f.app, &stats), PIXELICA_OK);
    CHECK_EQ_LONG(stats.checkpoints, 1);
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);

    f.store.data[0][PIXELICA_SAVE_HEADER_BYTES + 1u] ^= 0xFFu;
    fixture_reset_game(&f);
    fixture_bind(&f, true, true, true);
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_app_state(&f.app), PIXELICA_APP_RUNNING);
    CHECK(!pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(pixelica_app_restore_status(&f.app), PIXELICA_ERR_BAD_ASSET);
    CHECK_EQ_LONG(f.game.score, 0); /* defaults */
    /* Shutting down overwrites the corrupt slot with a valid one. */
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_save_inspect(f.store.data[0], f.store.size[0], NULL, NULL),
                  PIXELICA_OK);

    /* The storage read itself fails. */
    fixture_reset_game(&f);
    fixture_bind(&f, true, false, false);
    f.store.read_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_app_init(&f.app, &f.config), PIXELICA_OK);
    CHECK(!pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(pixelica_app_restore_status(&f.app), PIXELICA_ERR_PLATFORM);
    f.store.read_fail = PIXELICA_OK;

    /* The game rejects the payload (for instance an unknown schema). */
    f.game.fail_load = PIXELICA_ERR_BAD_ASSET;
    CHECK_EQ_LONG(pixelica_app_restore(&f.app), PIXELICA_ERR_BAD_ASSET);
    CHECK(!pixelica_app_restored(&f.app));
    f.game.fail_load = PIXELICA_OK;
    CHECK_EQ_LONG(pixelica_app_restore(&f.app), PIXELICA_OK);
    CHECK(pixelica_app_restored(&f.app));
    CHECK_EQ_LONG(pixelica_app_shutdown(&f.app), PIXELICA_OK);
}

int main(void)
{
    test_config_validation();
    test_transitions();
    test_reentrancy();
    test_checkpoint_round_trip();
    test_checkpoint_failures();
    return pixelica_t_finish("test_app");
}
