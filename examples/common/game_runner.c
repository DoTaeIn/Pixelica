#include "pixelica_game.h"

int pixelica_game_run(const PIXELICA_Game *game, const PIXELICA_GameArgs *args,
                 const PIXELICA_PlatformOps *ops, PIXELICA_PumpFn pump, void *pump_userdata)
{
    if (game == NULL || args == NULL || ops == NULL || game->init == NULL ||
        game->update == NULL || game->render == NULL || ops->time_us == NULL ||
        ops->read_buttons == NULL) {
        fprintf(stderr, "pixelica_game_run: invalid game or platform description\n");
        return 1;
    }

    PIXELICA_Context *ctx = NULL;
    PIXELICA_Status status = pixelica_create(&game->config, ops, &ctx);
    if (status != PIXELICA_OK) {
        fprintf(stderr, "pixelica_create failed (PIXELICA_Status %d)\n", (int)status);
        return 1;
    }

    void *state = NULL;
    if (!game->init(&state, ctx)) {
        fprintf(stderr, "game init failed\n");
        pixelica_destroy(ctx);
        return 1;
    }

    int exit_code = 0;
    uint32_t frame = 0;
    uint64_t previous_us = ops->time_us(ops->userdata);

    for (;;) {
        if (pump != NULL && !pump(pump_userdata)) {
            break;
        }
        if ((ops->read_buttons(ops->userdata) & PIXELICA_BTN_QUIT) != 0u) {
            break;
        }

        uint64_t elapsed_us;
        if (args->has_frames) {
            elapsed_us = PIXELICA_GAME_FRAMES_MODE_STEP_US;
        } else {
            uint64_t now_us = ops->time_us(ops->userdata);
            elapsed_us = now_us >= previous_us ? now_us - previous_us : 0u;
            previous_us = now_us;
        }

        pixelica_update(ctx, elapsed_us, game->update, state);

        status = game->render(state, ctx);
        if (status != PIXELICA_OK) {
            fprintf(stderr, "render failed on frame %lu (PIXELICA_Status %d)\n",
                    (unsigned long)frame, (int)status);
            exit_code = 1;
            break;
        }

        ++frame;
        if (args->has_frames && frame >= args->frames) {
            break;
        }
    }

    if (game->shutdown != NULL) {
        game->shutdown(state);
    }
    pixelica_destroy(ctx);
    return exit_code;
}
