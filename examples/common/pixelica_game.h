/*
 * Example game interface shared by every example and every driver.
 *
 * An example implements one `const PIXELICA_Game pixelica_example_game` using only the
 * public pixelica API. A driver (platforms/sdl3/main_sdl3.c or
 * examples/common/main_headless.c) owns main(), the platform lifecycle and
 * the pixelica context, and calls these callbacks through pixelica_game_run().
 */
#ifndef PIXELICA_EXAMPLE_GAME_H
#define PIXELICA_EXAMPLE_GAME_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pixelica/pixelica.h"
#include "pixelica/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Packs 8-bit channels into RGB565 (low bits are truncated). */
#define PIXELICA_EXAMPLE_RGB565(r, g, b) PIXELICA_RGB565((r), (g), (b))

typedef struct PIXELICA_Game {
    const char *title;
    PIXELICA_Config config;
    /* Returns true on success and stores the game state. No state is kept on failure. */
    bool (*init)(void **out_state, PIXELICA_Context *ctx);
    /* Same signature as PIXELICA_UpdateFn; called by pixelica_update() at the fixed rate. */
    void (*update)(void *state, float dt, uint32_t buttons);
    /* Must call pixelica_begin_frame ... pixelica_end_frame (end_frame presents). */
    PIXELICA_Status (*render)(void *state, PIXELICA_Context *ctx);
    /* Called only after a successful init. */
    void (*shutdown)(void *state);
} PIXELICA_Game;

/* Defined by exactly one example per executable. */
extern const PIXELICA_Game pixelica_example_game;

typedef struct PIXELICA_GameArgs {
    bool hidden;     /* --hidden: SDL dummy video driver, no visible window */
    bool has_frames; /* --frames N given */
    uint32_t frames; /* number of frames to run when has_frames */
} PIXELICA_GameArgs;

/* Returns false when the run loop should stop (quit requested). May be NULL. */
typedef bool (*PIXELICA_PumpFn)(void *userdata);

/*
 * Fixed elapsed time fed to pixelica_update() per rendered frame when --frames is
 * used, so short smoke runs do not depend on the wall clock.
 */
#define PIXELICA_GAME_FRAMES_MODE_STEP_US 16667u

/*
 * Creates the context, runs init/update/render until quit or the frame limit,
 * then shuts everything down. Returns a process exit code (0 on success).
 * `ops` must outlive the call.
 */
int pixelica_game_run(const PIXELICA_Game *game, const PIXELICA_GameArgs *args,
                 const PIXELICA_PlatformOps *ops, PIXELICA_PumpFn pump, void *pump_userdata);

/* 0 = parsed, 1 = help printed, -1 = error (message printed to stderr). */
static inline int pixelica_game_parse_args(int argc, char **argv, PIXELICA_GameArgs *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;
        if (strcmp(arg, "--hidden") == 0) {
            out->hidden = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("usage: %s [--frames N] [--hidden]\n"
                   "  --frames N  run N frames with a fixed per-frame time step, then exit\n"
                   "  --hidden    SDL dummy video driver (no window); ignored by the headless build\n"
                   "keys: arrows/WASD move, Space = A, Enter = START, Escape or window close = quit\n",
                   argc > 0 ? argv[0] : "example");
            return 1;
        } else if (strcmp(arg, "--frames") == 0 || strncmp(arg, "--frames=", 9) == 0) {
            if (arg[8] == '=') {
                value = arg + 9;
            } else if (i + 1 < argc) {
                value = argv[++i];
            }
            if (value == NULL || value[0] < '0' || value[0] > '9') {
                fprintf(stderr, "--frames needs a positive integer\n");
                return -1;
            }
            char *end = NULL;
            errno = 0;
            unsigned long long n = strtoull(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' || n == 0 || n > UINT32_MAX) {
                fprintf(stderr, "--frames value '%s' is not in 1..%lu\n", value,
                        (unsigned long)UINT32_MAX);
                return -1;
            }
            out->has_frames = true;
            out->frames = (uint32_t)n;
        } else {
            fprintf(stderr, "unknown argument '%s' (try --help)\n", arg);
            return -1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_EXAMPLE_GAME_H */
