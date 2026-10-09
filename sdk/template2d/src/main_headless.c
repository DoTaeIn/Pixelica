/*
 * Headless driver: no window, no SDL, no 3D. Runs the game for a fixed number
 * of frames with a deterministic scripted input, hands every finished frame to
 * a PAL present callback exactly once, and prints a one-line summary with the
 * FNV-1a hash of the last frame. The hash proves the run is deterministic, not
 * what it looks like: use --capture to write the last frame as a PPM image.
 *
 *   pixelica2d_game [--frames N] [--capture PATH]     N in 1..1000000, default 60
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen for --capture only */
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "pixelica/platform.h"

#define DEFAULT_FRAMES 60u
#define MAX_FRAMES 1000000u

typedef struct HeadlessPlatform {
    uint32_t presented;
} HeadlessPlatform;

static uint16_t g_pixels[TEMPLATE2D_WIDTH * TEMPLATE2D_HEIGHT];

static uint64_t headless_time_us(void *userdata)
{
    (void)userdata;
    return 0u;
}

static uint32_t headless_read_buttons(void *userdata)
{
    (void)userdata;
    return 0u;
}

/* The PAL contract: called once per finished frame, must not keep `pixels`. */
static bool headless_present(void *userdata, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    HeadlessPlatform *platform = userdata;

    if (pixels == NULL || width == 0u || height == 0u || stride_pixels < width) {
        return false;
    }
    platform->presented += 1u;
    return true;
}

/* Scripted input so the update path sees real button changes. */
static uint32_t scripted_buttons(uint32_t frame)
{
    uint32_t buttons = 0u;

    if (frame >= 5u && frame < 30u) {
        buttons |= PIXELICA_BTN_RIGHT;
    }
    if (frame >= 20u && frame < 40u) {
        buttons |= PIXELICA_BTN_DOWN;
    }
    if (frame >= 45u && frame < 70u) {
        buttons |= PIXELICA_BTN_LEFT | PIXELICA_BTN_UP;
    }
    return buttons;
}

static bool parse_frames(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u || value > MAX_FRAMES) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static uint32_t hash_frame(const uint16_t *pixels, size_t count)
{
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0u; i < count; ++i) {
        hash = (hash ^ (uint32_t)(pixels[i] & 0xFFu)) * 16777619u;
        hash = (hash ^ (uint32_t)(pixels[i] >> 8)) * 16777619u;
    }
    return hash;
}

static bool write_ppm(const char *path, const uint16_t *pixels)
{
    FILE *file = fopen(path, "wb");
    uint8_t row[TEMPLATE2D_WIDTH * 3u];
    uint32_t x;
    uint32_t y;
    bool ok = true;

    if (file == NULL) {
        fprintf(stderr, "cannot open '%s' for writing\n", path);
        return false;
    }
    ok = fprintf(file, "P6\n%u %u\n255\n", (unsigned)TEMPLATE2D_WIDTH,
                 (unsigned)TEMPLATE2D_HEIGHT) > 0;
    for (y = 0u; ok && y < TEMPLATE2D_HEIGHT; ++y) {
        for (x = 0u; x < TEMPLATE2D_WIDTH; ++x) {
            const uint32_t p = pixels[y * TEMPLATE2D_WIDTH + x];
            const uint32_t r = (p >> 11) & 0x1Fu;
            const uint32_t g = (p >> 5) & 0x3Fu;
            const uint32_t b = p & 0x1Fu;

            row[x * 3u + 0u] = (uint8_t)((r << 3) | (r >> 2));
            row[x * 3u + 1u] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3u + 2u] = (uint8_t)((b << 3) | (b >> 2));
        }
        ok = fwrite(row, 1u, sizeof row, file) == sizeof row;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "failed while writing '%s'\n", path);
    }
    return ok;
}

int main(int argc, char **argv)
{
    uint32_t frames = DEFAULT_FRAMES;
    const char *capture = NULL;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [--frames N] [--capture PATH]\n", argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_frames(argv[++i], &frames)) {
                fprintf(stderr, "--frames needs an integer in 1..%u\n", MAX_FRAMES);
                return 2;
            }
        } else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc && argv[i + 1][0] != '\0' &&
                   strncmp(argv[i + 1], "--", 2) != 0) {
            capture = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--capture PATH]\n", argv[0]);
            return 2;
        }
    }

    HeadlessPlatform platform = {0u};
    PIXELICA_PlatformOps ops;
    memset(&ops, 0, sizeof ops);
    ops.userdata = &platform;
    ops.time_us = headless_time_us;
    ops.read_buttons = headless_read_buttons;
    ops.present_rgb565 = headless_present;

    PIXELICA2D_Surface surface;
    surface.pixels = g_pixels;
    surface.width = (uint16_t)TEMPLATE2D_WIDTH;
    surface.height = (uint16_t)TEMPLATE2D_HEIGHT;
    surface.stride = TEMPLATE2D_WIDTH;
    surface.depth = NULL;
    surface.depth_stride = 0u;

    static Template2DGame game;
    if (!template2d_game_init(&game, &surface)) {
        fprintf(stderr, "game init failed\n");
        return 1;
    }

    int exit_code = 0;
    for (uint32_t frame = 0u; frame < frames; ++frame) {
        template2d_game_update(&game, scripted_buttons(frame));
        PIXELICA_Status status = template2d_game_render(&game);
        if (status != PIXELICA_OK) {
            fprintf(stderr, "render failed on frame %" PRIu32 " (PIXELICA_Status %d)\n", frame,
                    (int)status);
            exit_code = 1;
            break;
        }
        if (!ops.present_rgb565(ops.userdata, g_pixels, surface.width, surface.height,
                                surface.stride)) {
            fprintf(stderr, "present failed on frame %" PRIu32 "\n", frame);
            exit_code = 1;
            break;
        }
    }

    if (exit_code == 0 && platform.presented != frames) {
        fprintf(stderr, "expected %" PRIu32 " presents, got %" PRIu32 "\n", frames,
                platform.presented);
        exit_code = 1;
    }
    if (exit_code == 0 && capture != NULL && !write_ppm(capture, g_pixels)) {
        exit_code = 1;
    }
    if (exit_code == 0) {
        printf("pixelica2d-template: frames=%" PRIu32 " presented=%" PRIu32 " fnv1a=%08" PRIx32 "\n",
               frames, platform.presented,
               hash_frame(g_pixels, (size_t)TEMPLATE2D_WIDTH * TEMPLATE2D_HEIGHT));
    }
    return exit_code;
}
