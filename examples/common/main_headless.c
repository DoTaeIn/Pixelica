/*
 * Driver for -DPIXELICA_PLATFORM=HEADLESS: no SDL, no window. The platform is a
 * tiny in-process PAL that discards pixels after hashing the last frame. This
 * is only a smoke run; it shows the examples execute, not what they look like.
 */
#include <inttypes.h>

#include "pixelica_game.h"

typedef struct HeadlessPlatform {
    uint64_t clock_us;
    uint32_t presented;
    uint32_t last_hash; /* FNV-1a over the little-endian bytes of the last frame */
} HeadlessPlatform;

static uint64_t headless_time_us(void *userdata)
{
    HeadlessPlatform *platform = userdata;
    platform->clock_us += PIXELICA_GAME_FRAMES_MODE_STEP_US;
    return platform->clock_us;
}

static uint32_t headless_read_buttons(void *userdata)
{
    (void)userdata;
    return 0u;
}

static bool headless_present(void *userdata, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    HeadlessPlatform *platform = userdata;
    if (pixels == NULL || width == 0u || height == 0u || stride_pixels < width) {
        return false;
    }
    uint32_t hash = 2166136261u;
    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t *row = pixels + (size_t)y * stride_pixels;
        for (uint32_t x = 0; x < width; ++x) {
            hash = (hash ^ (uint32_t)(row[x] & 0xFFu)) * 16777619u;
            hash = (hash ^ (uint32_t)(row[x] >> 8)) * 16777619u;
        }
    }
    platform->last_hash = hash;
    platform->presented += 1u;
    return true;
}

static void headless_log(void *userdata, const char *message)
{
    (void)userdata;
    fprintf(stderr, "%s\n", message);
}

int main(int argc, char **argv)
{
    PIXELICA_GameArgs args;
    int parsed = pixelica_game_parse_args(argc, argv, &args);
    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }
    if (!args.has_frames) {
        /* Nothing can request quit here, so never loop forever. */
        args.has_frames = true;
        args.frames = 120u;
    }

    HeadlessPlatform platform = {0u, 0u, 0u};
    PIXELICA_PlatformOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.userdata = &platform;
    ops.time_us = headless_time_us;
    ops.read_buttons = headless_read_buttons;
    ops.present_rgb565 = headless_present;
    ops.log = headless_log;

    int rc = pixelica_game_run(&pixelica_example_game, &args, &ops, NULL, NULL);
    if (rc == 0) {
        printf("headless: presented=%" PRIu32 " last_frame_fnv1a=%08" PRIx32 "\n",
               platform.presented, platform.last_hash);
        if (platform.presented != args.frames) {
            fprintf(stderr, "expected %" PRIu32 " presents\n", args.frames);
            rc = 1;
        }
    }
    return rc;
}
