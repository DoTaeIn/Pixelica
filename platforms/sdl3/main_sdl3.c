/* Common SDL3 driver: owns main() and the platform lifecycle for every example. */
#include "../../examples/common/pixelica_game.h"
#include "platform_sdl3.h"

static bool pump_sdl3(void *userdata)
{
    return pixelica_sdl3_pump_events(userdata);
}

int main(int argc, char **argv)
{
    PIXELICA_GameArgs args;
    int parsed = pixelica_game_parse_args(argc, argv, &args);
    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }

    const PIXELICA_Game *game = &pixelica_example_game;
    PIXELICA_Sdl3Config platform_config;
    platform_config.title = game->title;
    platform_config.width = game->config.width;
    platform_config.height = game->config.height;
    platform_config.hidden = args.hidden;

    PIXELICA_Sdl3Platform *platform = NULL;
    if (!pixelica_sdl3_create(&platform_config, &platform)) {
        fprintf(stderr, "failed to create the SDL3 platform\n");
        return 1;
    }

    PIXELICA_PlatformOps ops = pixelica_sdl3_ops(platform);
    int rc = pixelica_game_run(game, &args, &ops, pump_sdl3, platform);

    pixelica_sdl3_destroy(platform);
    return rc;
}
