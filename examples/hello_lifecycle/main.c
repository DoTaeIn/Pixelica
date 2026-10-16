/*
 * hello_lifecycle - the portable game lifecycle and save storage end to end,
 * with no renderer, no window and no platform #ifdef.
 *
 * The program is one run of a game: it initializes, optionally restores a
 * checkpoint written by an EARLIER PROCESS, simulates a few fixed steps
 * (skipping the ones where the app is paused), then leaves the way the
 * command line asked it to:
 *
 *   --exit shutdown   an orderly quit        (checkpoint, then on_shutdown)
 *   --exit suspend    the host backgrounds   (checkpoint, then on_suspend) and
 *                     the process is never resumed
 *   --exit kill       the process disappears without any lifecycle call, so
 *                     everything since the last checkpoint is lost
 *
 * Run it twice against the same --dir and the second run continues where the
 * first one stopped. That is what tests/../cmake/PixelicaLifecycleCheck.cmake
 * checks, and it is the whole contract: the engine persists nothing by
 * itself, the game serializes its own fields, and pixelica/storage.h wraps
 * them in the versioned CRC-32 envelope.
 *
 * Usage:
 *   hello_lifecycle --dir <root> [--game-id <id>] [--slot <0..15>]
 *                   [--steps <n>] [--pause-at <n>] [--exit shutdown|suspend|kill]
 *                   [--fresh]
 *
 * It prints exactly one line:
 *   hello-lifecycle: restored=<0|1> restore=<status> level=<n> score=<n>
 *                    steps=<n> skipped=<n> exit=<mode> state=<name>
 *                    checkpoint=<status>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pixelica/app.h"
#include "pixelica/storage.h"

/* v1 stored level+score; v2 added the run counter. The game owns this. */
#define GAME_SCHEMA_V1 1u
#define GAME_SCHEMA 2u
#define GAME_PAYLOAD_V1 8u
#define GAME_PAYLOAD 12u
/* Buffers are sized for the biggest save this game will ever write, not for
 * today's payload: a slot that has grown past the scratch buffer reports
 * NO_MEMORY instead of being validated, and headroom keeps that from turning
 * a detectable corruption into an unhelpful "too big". */
#define GAME_MAX_PAYLOAD 32u

typedef struct Game {
    uint32_t level;
    uint32_t score;
    uint32_t runs; /* how many processes have saved this game */
} Game;

/* ---- fixed-order, fixed-width serialization ----------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static PIXELICA_Status game_save(void *userdata, void *out, size_t capacity,
                                 size_t *out_size, uint32_t *out_schema_version)
{
    const Game *g = (const Game *)userdata;
    uint8_t *p = (uint8_t *)out;

    if (capacity < GAME_PAYLOAD) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    put_u32(p + 0, g->level);
    put_u32(p + 4, g->score);
    put_u32(p + 8, g->runs);
    *out_size = GAME_PAYLOAD;
    *out_schema_version = GAME_SCHEMA;
    return PIXELICA_OK;
}

static PIXELICA_Status game_load(void *userdata, uint32_t schema, const void *data,
                                 size_t size)
{
    Game *g = (Game *)userdata;
    const uint8_t *p = (const uint8_t *)data;
    Game loaded;

    /* Branch on the version the save was written with; never trust the size. */
    if (schema == GAME_SCHEMA_V1 && size == GAME_PAYLOAD_V1) {
        loaded.runs = 0u; /* the field did not exist yet */
    } else if (schema == GAME_SCHEMA && size == GAME_PAYLOAD) {
        loaded.runs = get_u32(p + 8);
    } else {
        return PIXELICA_ERR_BAD_ASSET; /* a save from a newer build */
    }
    loaded.level = get_u32(p + 0);
    loaded.score = get_u32(p + 4);
    if (loaded.level > 1000u) {
        return PIXELICA_ERR_BAD_ASSET; /* valid bytes, impossible value */
    }
    *g = loaded; /* only now is the game state touched */
    return PIXELICA_OK;
}

/* ---- lifecycle notifications ------------------------------------------- */

static PIXELICA_Status game_on_init(void *userdata)
{
    (void)userdata;
    return PIXELICA_OK;
}

static PIXELICA_Status game_on_suspend(void *userdata)
{
    Game *g = (Game *)userdata;

    /* Nothing may be written here: the checkpoint has already been taken. */
    (void)g;
    return PIXELICA_OK;
}

/* ---- command line ------------------------------------------------------- */

typedef struct Options {
    const char *dir;
    const char *game_id;
    uint32_t slot;
    uint32_t steps;
    uint32_t pause_at; /* UINT32_MAX = never */
    const char *exit_mode;
    bool fresh; /* erase the slot before starting */
} Options;

static int parse_u32(const char *text, uint32_t limit, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0' || value > limit) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: hello_lifecycle --dir <root> [--game-id <id>] [--slot <0..%u>]\n"
            "                       [--steps <n>] [--pause-at <n>]\n"
            "                       [--exit shutdown|suspend|kill] [--fresh]\n",
            (unsigned)PIXELICA_STORAGE_SLOT_MAX - 1u);
}

static int parse_args(int argc, char **argv, Options *opt)
{
    int i;

    opt->dir = NULL;
    opt->game_id = "hello-lifecycle";
    opt->slot = 0u;
    opt->steps = 10u;
    opt->pause_at = 0xFFFFFFFFu;
    opt->exit_mode = "shutdown";
    opt->fresh = false;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--fresh") == 0) {
            opt->fresh = true;
            continue;
        }
        if (value == NULL) {
            fprintf(stderr, "hello_lifecycle: %s needs a value\n", arg);
            return 0;
        }
        ++i;
        if (strcmp(arg, "--dir") == 0) {
            opt->dir = value;
        } else if (strcmp(arg, "--game-id") == 0) {
            opt->game_id = value;
        } else if (strcmp(arg, "--slot") == 0) {
            if (!parse_u32(value, PIXELICA_STORAGE_SLOT_MAX - 1u, &opt->slot)) {
                fprintf(stderr, "hello_lifecycle: bad --slot '%s'\n", value);
                return 0;
            }
        } else if (strcmp(arg, "--steps") == 0) {
            if (!parse_u32(value, 100000u, &opt->steps)) {
                fprintf(stderr, "hello_lifecycle: bad --steps '%s'\n", value);
                return 0;
            }
        } else if (strcmp(arg, "--pause-at") == 0) {
            if (!parse_u32(value, 100000u, &opt->pause_at)) {
                fprintf(stderr, "hello_lifecycle: bad --pause-at '%s'\n", value);
                return 0;
            }
        } else if (strcmp(arg, "--exit") == 0) {
            if (strcmp(value, "shutdown") != 0 && strcmp(value, "suspend") != 0 &&
                strcmp(value, "kill") != 0) {
                fprintf(stderr, "hello_lifecycle: bad --exit '%s'\n", value);
                return 0;
            }
            opt->exit_mode = value;
        } else {
            fprintf(stderr, "hello_lifecycle: unknown option '%s'\n", arg);
            return 0;
        }
    }
    if (opt->dir == NULL) {
        fprintf(stderr, "hello_lifecycle: --dir is required\n");
        return 0;
    }
    return 1;
}

static const char *status_name(PIXELICA_Status status)
{
    switch (status) {
    case PIXELICA_OK:
        return "ok";
    case PIXELICA_ERR_INVALID_ARG:
        return "invalid_arg";
    case PIXELICA_ERR_NO_MEMORY:
        return "no_memory";
    case PIXELICA_ERR_CAPACITY:
        return "capacity";
    case PIXELICA_ERR_BAD_ASSET:
        return "bad_asset";
    case PIXELICA_ERR_PLATFORM:
        return "platform";
    case PIXELICA_ERR_NOT_FOUND:
        return "not_found";
    default:
        break;
    }
    return "unknown";
}

int main(int argc, char **argv)
{
    Options opt;
    Game game;
    PIXELICA_StorageFs fs;
    PIXELICA_StorageOps ops;
    PIXELICA_Storage storage;
    PIXELICA_App app;
    PIXELICA_AppConfig config;
    uint8_t game_scratch[GAME_MAX_PAYLOAD];
    uint8_t store_scratch[GAME_MAX_PAYLOAD + PIXELICA_SAVE_OVERHEAD_BYTES];
    PIXELICA_Status st;
    PIXELICA_Status exit_status = PIXELICA_OK;
    uint32_t step;
    uint32_t skipped = 0u;

    if (!parse_args(argc, argv, &opt)) {
        usage();
        return 2;
    }
    if (!pixelica_storage_game_id_valid(opt.game_id)) {
        fprintf(stderr, "hello_lifecycle: '%s' is not a valid game id\n", opt.game_id);
        return 2;
    }

    memset(&game, 0, sizeof game);
    memset(&storage, 0, sizeof storage);
    memset(&app, 0, sizeof app);

    st = pixelica_storage_fs_init(&fs, opt.dir, &ops);
    if (st != PIXELICA_OK) {
        fprintf(stderr, "hello_lifecycle: storage root '%s' refused (%s)\n", opt.dir,
                status_name(st));
        return 3;
    }
    st = pixelica_storage_init(&storage, opt.game_id, &ops, store_scratch,
                               sizeof store_scratch);
    if (st != PIXELICA_OK) {
        fprintf(stderr, "hello_lifecycle: storage init failed (%s)\n", status_name(st));
        return 3;
    }
    if (opt.fresh) {
        st = pixelica_storage_erase(&storage, opt.slot);
        if (st != PIXELICA_OK && st != PIXELICA_ERR_NOT_FOUND) {
            fprintf(stderr, "hello_lifecycle: --fresh could not erase slot %u (%s)\n",
                    (unsigned)opt.slot, status_name(st));
            return 3;
        }
    }

    memset(&config, 0, sizeof config);
    config.callbacks.on_init = game_on_init;
    config.callbacks.on_suspend = game_on_suspend;
    config.callbacks.save_checkpoint = game_save;
    config.callbacks.load_checkpoint = game_load;
    config.callbacks.userdata = &game;
    config.storage = &storage;
    config.checkpoint_slot = opt.slot;
    config.scratch = game_scratch;
    config.scratch_size = sizeof game_scratch;
    config.restore_on_init = true;
    config.checkpoint_on_suspend = true;
    config.checkpoint_on_shutdown = true;

    st = pixelica_app_init(&app, &config);
    if (st != PIXELICA_OK) {
        fprintf(stderr, "hello_lifecycle: app init failed (%s)\n", status_name(st));
        return 3;
    }

    /* One fixed-step simulation. A paused step advances nothing, which is the
     * only difference the game sees between RUNNING and PAUSED. */
    for (step = 0; step < opt.steps; ++step) {
        if (step == opt.pause_at) {
            (void)pixelica_app_pause(&app);
        } else if (opt.pause_at != 0xFFFFFFFFu && step == opt.pause_at + 2u) {
            (void)pixelica_app_resume(&app);
        }
        if (!pixelica_app_should_update(&app)) {
            ++skipped;
            continue;
        }
        game.score += 1u;
        game.level = game.score / 10u;
    }
    (void)pixelica_app_resume(&app); /* a no-op unless the loop ended paused */
    game.runs += 1u;

    if (strcmp(opt.exit_mode, "suspend") == 0) {
        exit_status = pixelica_app_suspend(&app);
    } else if (strcmp(opt.exit_mode, "shutdown") == 0) {
        exit_status = pixelica_app_shutdown(&app);
    }
    /* "kill": return without any lifecycle call, exactly like a process that
     * is terminated. Nothing since the last checkpoint reaches the disk. */

    printf("hello-lifecycle: restored=%d restore=%s level=%lu score=%lu runs=%lu "
           "steps=%lu skipped=%lu exit=%s state=%s checkpoint=%s\n",
           pixelica_app_restored(&app) ? 1 : 0,
           status_name(pixelica_app_restore_status(&app)), (unsigned long)game.level,
           (unsigned long)game.score, (unsigned long)game.runs,
           (unsigned long)opt.steps, (unsigned long)skipped, opt.exit_mode,
           pixelica_app_state_name(pixelica_app_state(&app)),
           status_name(pixelica_app_checkpoint_status(&app)));
    fflush(stdout);

    return exit_status == PIXELICA_OK ? 0 : 4;
}
