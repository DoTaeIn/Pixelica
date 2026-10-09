/*
 * Shared driver for the 2D examples (hello_2d, hello_isometric).
 *
 * Owns main(), the static RGB565 target (<= 256x192), the platform and the
 * frame loop. The platform is chosen at compile time:
 *   - default (HEADLESS): no window; a tiny PAL accepts the frame and the
 *     run is always finite (60 frames unless --frames N is given);
 *   - -DPIXELICA2D_DEMO_SDL: the SDL3 PAL from platforms/sdl3 shows the frame and
 *     reads the keyboard. Without --frames it runs interactively at a fixed
 *     60 Hz simulation step until quit (Escape / window close).
 *
 * Usage:  demo [--frames N] [--hidden] [--capture PATH]
 *   --frames N      positive integer; run exactly N frames, one update and one
 *                   present per frame, scripted input -> deterministic
 *   --hidden        SDL: dummy video driver (no window); ignored when headless
 *   --capture PATH  write the last presented frame as a binary PPM (P6)
 * On success prints one line: frames=N presented=N fnv1a=XXXXXXXX (FNV-1a 32
 * over the little-endian bytes of the last frame). The hash proves
 * determinism, not appearance; use --capture to look at the picture.
 *
 * Per frame: update(s) -> render -> exactly one PAL present_rgb565. Every
 * failure is propagated to a non-zero exit code.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen for --capture; every other warning stays on */
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo2d.h"

#ifdef PIXELICA2D_DEMO_SDL
#include "platform_sdl3.h"
#endif

#define DEFAULT_HEADLESS_FRAMES 60u
#define STEP_UNITS 1000000u /* one 60 Hz step, in 1/60 microsecond units */
#define MAX_ELAPSED_US 1000000u
#define MAX_STEPS_PER_FRAME 4u
#define MAX_CAPTURE_PATH 4095u

typedef bool (*PumpFn)(void *userdata);

typedef struct Args {
    bool hidden;
    bool has_frames;
    uint32_t frames;
    const char *capture;
} Args;

static uint16_t g_target[PIXELICA2D_DEMO_WIDTH * PIXELICA2D_DEMO_HEIGHT];

static bool parse_positive_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0u || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/*
 * "--name value" or "--name=value". NULL: argv[*i] is not this option.
 * An option with no value yields "" (callers reject it).
 */
static const char *option_value(int argc, char **argv, int *i, const char *name)
{
    const char *arg = argv[*i];
    const size_t length = strlen(name);

    if (strncmp(arg, name, length) != 0) {
        return NULL;
    }
    if (arg[length] == '=') {
        return arg + length + 1;
    }
    if (arg[length] != '\0') {
        return NULL;
    }
    if (*i + 1 < argc) {
        *i += 1;
        return argv[*i];
    }
    return "";
}

/* 0 = parsed, 1 = help printed, -1 = error (message on stderr). */
static int parse_args(int argc, char **argv, Args *out)
{
    int i;

    memset(out, 0, sizeof *out);
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "--hidden") == 0) {
            out->hidden = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printf("usage: %s [--frames N] [--hidden] [--capture PATH]\n"
                   "  --frames N      run exactly N frames (positive integer) with scripted input, then exit\n"
                   "  --hidden        SDL dummy video driver (no window); ignored by the headless build\n"
                   "  --capture PATH  write the last frame as a binary PPM (P6)\n"
                   "keys (SDL): arrows/WASD move, Space = A (pause), Enter = START (reset), Escape = quit\n",
                   argc > 0 ? argv[0] : "demo");
            return 1;
        } else if ((value = option_value(argc, argv, &i, "--frames")) != NULL) {
            if (out->has_frames) {
                fprintf(stderr, "--frames given more than once\n");
                return -1;
            }
            if (!parse_positive_u32(value, &out->frames)) {
                fprintf(stderr, "--frames needs an integer in 1..%lu (got '%s')\n",
                        (unsigned long)UINT32_MAX, value);
                return -1;
            }
            out->has_frames = true;
        } else if ((value = option_value(argc, argv, &i, "--capture")) != NULL) {
            if (out->capture != NULL) {
                fprintf(stderr, "--capture given more than once\n");
                return -1;
            }
            if (value[0] == '\0' || strncmp(value, "--", 2) == 0 || strlen(value) > MAX_CAPTURE_PATH) {
                fprintf(stderr, "--capture needs a file path (got '%s')\n", value);
                return -1;
            }
            out->capture = value;
        } else {
            fprintf(stderr, "unknown argument '%s' (try --help)\n", arg);
            return -1;
        }
    }
    return 0;
}

/* Deterministic input for --frames runs (and the headless build). */
static uint32_t scripted_buttons(uint32_t frame)
{
    const uint32_t f = frame % 240u;
    uint32_t buttons = 0u;

    if (f >= 8u && f < 44u) {
        buttons |= PIXELICA_BTN_RIGHT;
    } else if (f >= 44u && f < 68u) {
        buttons |= PIXELICA_BTN_DOWN;
    } else if (f >= 68u && f < 100u) {
        buttons |= PIXELICA_BTN_LEFT;
    } else if (f >= 100u && f < 116u) {
        buttons |= PIXELICA_BTN_UP;
    }
    if (f == 30u || f == 34u) {
        buttons |= PIXELICA_BTN_A; /* pause, then resume */
    }
    if (f == 200u) {
        buttons |= PIXELICA_BTN_START;
    }
    return buttons;
}

static uint32_t hash_frame(const uint16_t *pixels, uint16_t width, uint16_t height)
{
    uint32_t hash = 2166136261u;
    size_t y;
    size_t x;

    for (y = 0u; y < height; ++y) {
        const uint16_t *row = pixels + y * width;

        for (x = 0u; x < width; ++x) {
            hash = (hash ^ (uint32_t)(row[x] & 0xFFu)) * 16777619u;
            hash = (hash ^ (uint32_t)(row[x] >> 8)) * 16777619u;
        }
    }
    return hash;
}

static bool write_ppm(const char *path, const uint16_t *pixels, uint16_t width, uint16_t height)
{
    uint8_t row[PIXELICA2D_DEMO_WIDTH * 3u];
    FILE *file;
    size_t y;
    size_t x;
    bool ok = true;

    if (width == 0u || height == 0u || width > PIXELICA2D_DEMO_WIDTH) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot open '%s' for writing\n", path);
        return false;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height) < 0) {
        ok = false;
    }
    for (y = 0u; ok && y < height; ++y) {
        for (x = 0u; x < width; ++x) {
            const uint32_t p = pixels[y * width + x];
            const uint32_t r = (p >> 11) & 0x1Fu;
            const uint32_t g = (p >> 5) & 0x3Fu;
            const uint32_t b = p & 0x1Fu;

            row[x * 3u + 0u] = (uint8_t)((r << 3) | (r >> 2));
            row[x * 3u + 1u] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3u + 2u] = (uint8_t)((b << 3) | (b >> 2));
        }
        if (fwrite(row, 1u, (size_t)width * 3u, file) != (size_t)width * 3u) {
            ok = false;
        }
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "failed while writing '%s'\n", path);
    }
    return ok;
}

#ifdef PIXELICA2D_DEMO_SDL
static bool pump_sdl3(void *userdata)
{
    return pixelica_sdl3_pump_events(userdata);
}
#else
/* Headless PAL: validates what a real display would receive and drops it. */
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

static bool headless_present(void *userdata, const uint16_t *pixels, uint16_t width,
                             uint16_t height, uint32_t stride_pixels)
{
    (void)userdata;
    return pixels != NULL && width != 0u && height != 0u && stride_pixels >= width;
}
#endif

static bool demo_is_valid(const PIXELICA2D_Demo *demo)
{
    return demo->title != NULL && demo->init != NULL && demo->update != NULL &&
           demo->render != NULL && demo->width >= 1u && demo->height >= 1u &&
           demo->width <= PIXELICA2D_DEMO_WIDTH && demo->height <= PIXELICA2D_DEMO_HEIGHT;
}

/* Runs the demo; on success fills *out_frames / *out_presented. */
static int run_demo(const PIXELICA2D_Demo *demo, const Args *args, const PIXELICA_PlatformOps *ops,
                    PumpFn pump, void *pump_userdata, uint32_t *out_frames,
                    uint32_t *out_presented)
{
    PIXELICA2D_Surface surface;
    void *state = NULL;
    uint32_t frame = 0u;
    uint32_t presented = 0u;
    uint64_t accumulator = 0u;
    uint64_t previous_us;
    int exit_code = 0;

    if (!demo_is_valid(demo) || ops == NULL || ops->time_us == NULL ||
        ops->read_buttons == NULL || ops->present_rgb565 == NULL) {
        fprintf(stderr, "invalid demo or platform description\n");
        return 1;
    }

    memset(g_target, 0, sizeof g_target);
    surface.pixels = g_target;
    surface.width = demo->width;
    surface.height = demo->height;
    surface.stride = demo->width;
    surface.depth = NULL;
    surface.depth_stride = 0u;
    if (!demo->init(&state, &surface)) {
        fprintf(stderr, "demo init failed\n");
        return 1;
    }

    previous_us = ops->time_us(ops->userdata);
    for (;;) {
        uint32_t buttons;
        uint32_t steps;
        uint32_t step;
        PIXELICA_Status status;

        if (pump != NULL && !pump(pump_userdata)) {
            break;
        }
        if (args->has_frames) {
            buttons = scripted_buttons(frame);
            steps = 1u;
        } else {
            const uint64_t now_us = ops->time_us(ops->userdata);
            uint64_t elapsed_us = now_us >= previous_us ? now_us - previous_us : 0u;
            uint64_t whole;

            previous_us = now_us;
            buttons = ops->read_buttons(ops->userdata);
            if (elapsed_us > MAX_ELAPSED_US) {
                elapsed_us = MAX_ELAPSED_US;
            }
            accumulator += elapsed_us * PIXELICA_INPUT_TICK_HZ;
            whole = accumulator / STEP_UNITS;
            if (whole > MAX_STEPS_PER_FRAME) {
                whole = MAX_STEPS_PER_FRAME;
                accumulator %= STEP_UNITS; /* drop whole steps beyond the cap */
            } else {
                accumulator -= whole * STEP_UNITS;
            }
            steps = (uint32_t)whole;
        }
        if ((buttons & PIXELICA_BTN_QUIT) != 0u) {
            break;
        }

        for (step = 0u; step < steps; ++step) {
            demo->update(state, buttons);
        }
        status = demo->render(state);
        if (status != PIXELICA_OK) {
            fprintf(stderr, "render failed on frame %" PRIu32 " (PIXELICA_Status %d)\n", frame,
                    (int)status);
            exit_code = 1;
            break;
        }
        if (!ops->present_rgb565(ops->userdata, g_target, demo->width, demo->height,
                                 demo->width)) {
            fprintf(stderr, "present failed on frame %" PRIu32 "\n", frame);
            exit_code = 1;
            break;
        }
        ++presented;
        ++frame;
        if (args->has_frames && frame >= args->frames) {
            break;
        }
    }

    if (demo->shutdown != NULL) {
        demo->shutdown(state);
    }
    *out_frames = frame;
    *out_presented = presented;
    return exit_code;
}

int main(int argc, char **argv)
{
    const PIXELICA2D_Demo *demo = &pixelica2d_example_demo;
    Args args;
    PIXELICA_PlatformOps ops;
    PumpFn pump = NULL;
    void *pump_userdata = NULL;
    uint32_t frames = 0u;
    uint32_t presented = 0u;
    int rc;
    int parsed = parse_args(argc, argv, &args);

    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }

#ifdef PIXELICA2D_DEMO_SDL
    {
        PIXELICA_Sdl3Config config;
        PIXELICA_Sdl3Platform *platform = NULL;

        config.title = demo->title;
        config.width = demo->width;
        config.height = demo->height;
        config.hidden = args.hidden;
        if (!pixelica_sdl3_create(&config, &platform)) {
            fprintf(stderr, "failed to create the SDL3 platform\n");
            return 1;
        }
        ops = pixelica_sdl3_ops(platform);
        pump = pump_sdl3;
        pump_userdata = platform;
        rc = run_demo(demo, &args, &ops, pump, pump_userdata, &frames, &presented);
        if (rc == 0 && args.capture != NULL && frames > 0u &&
            !write_ppm(args.capture, g_target, demo->width, demo->height)) {
            rc = 1;
        }
        pixelica_sdl3_destroy(platform);
    }
#else
    if (!args.has_frames) {
        /* Nothing can request quit here, so never loop forever. */
        args.has_frames = true;
        args.frames = DEFAULT_HEADLESS_FRAMES;
    }
    memset(&ops, 0, sizeof ops);
    ops.time_us = headless_time_us;
    ops.read_buttons = headless_read_buttons;
    ops.present_rgb565 = headless_present;
    rc = run_demo(demo, &args, &ops, pump, pump_userdata, &frames, &presented);
    if (rc == 0 && args.capture != NULL && frames > 0u &&
        !write_ppm(args.capture, g_target, demo->width, demo->height)) {
        rc = 1;
    }
#endif

    if (rc == 0 && args.capture != NULL && frames == 0u) {
        fprintf(stderr, "no frame was rendered, nothing to capture\n");
        rc = 1;
    }
    if (rc == 0 && args.has_frames && presented != args.frames) {
        fprintf(stderr, "expected %" PRIu32 " presents, got %" PRIu32 "\n", args.frames, presented);
        rc = 1;
    }
    if (rc == 0) {
        printf("pixelica2d-demo: %s frames=%" PRIu32 " presented=%" PRIu32 " fnv1a=%08" PRIx32 "\n",
               demo->title, frames, presented, hash_frame(g_target, demo->width, demo->height));
    }
    return rc;
}
