/*
 * SDL3 window front end. Only built in source-tree mode with
 * -DPIXELICA_GAME_USE_SOURCE_TREE=ON -DPIXELICA_GAME_WITH_SDL3=ON (the installed package
 * is headless only). Uses Pixelica's SDL3 platform target, which keeps SDL
 * confined to showing pixels, reading the keyboard and the clock.
 *
 *   pixelica_game_sdl3 [--frames N] [--hidden]
 *
 * With --frames the clock advances a fixed 16667 us per frame so short runs
 * are repeatable; --hidden uses SDL's dummy video driver (no window).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "platform_sdl3.h"

#define FIXED_STEP_US 16667u

int main(int argc, char **argv)
{
    bool hidden = false;
    bool has_frames = false;
    unsigned long frames = 0ul;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--hidden") == 0) {
            hidden = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            frames = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || frames == 0ul) {
                fprintf(stderr, "--frames needs a positive integer\n");
                return 2;
            }
            has_frames = true;
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--hidden]\n", argv[0]);
            return 2;
        }
    }

    const PIXELICA_Config config = template_game_config();
    PIXELICA_Sdl3Config sdl_config;
    memset(&sdl_config, 0, sizeof(sdl_config));
    sdl_config.title = "Pixelica starter game";
    sdl_config.width = config.width;
    sdl_config.height = config.height;
    sdl_config.hidden = hidden;

    PIXELICA_Sdl3Platform *platform = NULL;
    if (!pixelica_sdl3_create(&sdl_config, &platform)) {
        fprintf(stderr, "SDL3 platform creation failed\n");
        return 1;
    }
    PIXELICA_PlatformOps ops = pixelica_sdl3_ops(platform);

    PIXELICA_Context *ctx = NULL;
    if (pixelica_create(&config, &ops, &ctx) != PIXELICA_OK) {
        fprintf(stderr, "pixelica_create failed\n");
        pixelica_sdl3_destroy(platform);
        return 1;
    }

    TemplateGame game;
    if (!template_game_init(&game)) {
        fprintf(stderr, "game init failed\n");
        pixelica_destroy(ctx);
        pixelica_sdl3_destroy(platform);
        return 1;
    }

    int exit_code = 0;
    unsigned long frame = 0ul;
    uint64_t previous_us = ops.time_us(ops.userdata);
    while (pixelica_sdl3_pump_events(platform)) {
        uint64_t elapsed_us = FIXED_STEP_US;
        if (!has_frames) {
            uint64_t now_us = ops.time_us(ops.userdata);
            elapsed_us = now_us >= previous_us ? now_us - previous_us : 0u;
            previous_us = now_us;
        }
        pixelica_update(ctx, elapsed_us, template_game_update, &game);
        if (template_game_render(&game, ctx) != PIXELICA_OK) {
            fprintf(stderr, "render failed\n");
            exit_code = 1;
            break;
        }
        ++frame;
        if (has_frames && frame >= frames) {
            break;
        }
    }

    pixelica_destroy(ctx);
    pixelica_sdl3_destroy(platform);
    return exit_code;
}
