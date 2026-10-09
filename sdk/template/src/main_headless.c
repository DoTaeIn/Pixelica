/*
 * Headless driver: no window, no SDL. Runs the game for a fixed number of
 * frames with a deterministic clock and a scripted button sequence, hashes the
 * last presented frame and prints a one-line summary. It proves the program
 * and the Pixelica package work; it does not show what the frame looks like.
 *
 *   pixelica_game [--frames N]      N in 1..1000000, default 60
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

#define STEP_US 16667u
#define DEFAULT_FRAMES 60u
#define MAX_FRAMES 1000000u

typedef struct HeadlessPlatform {
    uint64_t clock_us;
    uint32_t frame;
    uint32_t presented;
    uint32_t last_hash; /* FNV-1a over the little-endian bytes of the last frame */
} HeadlessPlatform;

static uint64_t headless_time_us(void *userdata)
{
    HeadlessPlatform *platform = userdata;
    return platform->clock_us;
}

/* Scripted input so the update path sees real button changes. */
static uint32_t headless_read_buttons(void *userdata)
{
    const HeadlessPlatform *platform = userdata;
    uint32_t buttons = 0u;
    if (platform->frame >= 10u && platform->frame < 20u) {
        buttons |= PIXELICA_BTN_RIGHT;
    }
    if (platform->frame == 25u) {
        buttons |= PIXELICA_BTN_A;
    }
    return buttons;
}

static bool headless_present(void *userdata, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    HeadlessPlatform *platform = userdata;
    if (pixels == NULL || width == 0u || height == 0u || stride_pixels < width) {
        return false;
    }
    uint32_t hash = 2166136261u;
    for (uint32_t y = 0u; y < height; ++y) {
        const uint16_t *row = pixels + (size_t)y * stride_pixels;
        for (uint32_t x = 0u; x < width; ++x) {
            hash = (hash ^ (uint32_t)(row[x] & 0xFFu)) * 16777619u;
            hash = (hash ^ (uint32_t)(row[x] >> 8)) * 16777619u;
        }
    }
    platform->last_hash = hash;
    platform->presented += 1u;
    return true;
}

static bool parse_frames(const char *text, uint32_t *out)
{
    char *end = NULL;
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u || value > MAX_FRAMES) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

int main(int argc, char **argv)
{
    uint32_t frames = DEFAULT_FRAMES;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_frames(argv[++i], &frames)) {
                fprintf(stderr, "--frames needs an integer in 1..%u\n", MAX_FRAMES);
                return 2;
            }
        } else {
            fprintf(stderr, "usage: %s [--frames N]\n", argc > 0 ? argv[0] : "pixelica_game");
            return (strcmp(argv[i], "--help") == 0) ? 0 : 2;
        }
    }

    HeadlessPlatform platform;
    memset(&platform, 0, sizeof(platform));
    PIXELICA_PlatformOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.userdata = &platform;
    ops.time_us = headless_time_us;
    ops.read_buttons = headless_read_buttons;
    ops.present_rgb565 = headless_present;

    const PIXELICA_Config config = template_game_config();
    PIXELICA_Context *ctx = NULL;
    if (pixelica_create(&config, &ops, &ctx) != PIXELICA_OK) {
        fprintf(stderr, "pixelica_create failed\n");
        return 1;
    }

    TemplateGame game;
    if (!template_game_init(&game)) {
        fprintf(stderr, "game init failed\n");
        pixelica_destroy(ctx);
        return 1;
    }

    int exit_code = 0;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        platform.frame = frame;
        platform.clock_us += STEP_US;
        pixelica_update(ctx, STEP_US, template_game_update, &game);
        PIXELICA_Status status = template_game_render(&game, ctx);
        if (status != PIXELICA_OK) {
            fprintf(stderr, "render failed on frame %" PRIu32 " (PIXELICA_Status %d)\n", frame, (int)status);
            exit_code = 1;
            break;
        }
    }

    if (exit_code == 0 && platform.presented != frames) {
        fprintf(stderr, "expected %" PRIu32 " presents, got %" PRIu32 "\n", frames, platform.presented);
        exit_code = 1;
    }
    if (exit_code == 0) {
        printf("pixelica-template: frames=%" PRIu32 " presented=%" PRIu32 " fnv1a=%08" PRIx32 "\n",
               frames, platform.presented, platform.last_hash);
    }
    pixelica_destroy(ctx);
    return exit_code;
}
