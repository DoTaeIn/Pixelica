/*
 * hello_particles - Pixelica's optional particle module, drawn offscreen with
 * the 2D renderer. No window, no SDL and no asset files.
 *
 * Scene (160x120, 60 simulation ticks per second):
 *   - sparks: untextured colored squares from a moving emitter, plus a burst
 *   - smoke:  a textured particle (a generated 8x8 puff with a color key)
 *   - rain:   four narrow emitters
 * All three use the ready-made presets in the 2D (+Y down) convention. The
 * simulation is advanced `--ticks` times with a fixed dt and the final frame is
 * drawn. The run is done twice and the frame checksums and counts must match, which
 * is the module's same-seed, same-calls determinism promise.
 *
 *   hello_particles                      90 ticks, prints a frame checksum
 *   hello_particles --output frame.ppm   also writes the frame as a binary PPM
 *   hello_particles --ticks N            simulate N ticks (1..3600)
 *
 * The checksum is only comparable between runs of the same build (compiler,
 * flags and platform): float rounding may differ elsewhere.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pixelica/particles.h"
#include "pixelica/particles2d.h"

#define WIDTH 160
#define HEIGHT 120
#define GROUND_Y 104
#define TICK_DT (1.0f / 60.0f)
#define DEFAULT_TICKS 90u
#define MAX_TICKS 3600u

#define SPARK_CAPACITY 128u
#define SMOKE_CAPACITY 64u
#define RAIN_EMITTERS 4u
#define RAIN_CAPACITY 64u
#define BURST_TICK 30u
#define BURST_COUNT 40u

#define PUFF_SIZE 8u
#define PUFF_KEY PIXELICA_RGB565(248, 0, 248)

typedef struct Scene {
    PIXELICA_ParticleEmitter sparks;
    PIXELICA_ParticleEmitter smoke;
    PIXELICA_ParticleEmitter rain[RAIN_EMITTERS];
    PIXELICA_Particle spark_store[SPARK_CAPACITY];
    PIXELICA_Particle smoke_store[SMOKE_CAPACITY];
    PIXELICA_Particle rain_store[RAIN_EMITTERS][RAIN_CAPACITY];
} Scene;

static Scene g_scene;
static uint16_t g_pixels[WIDTH * HEIGHT];
static uint16_t g_puff_pixels[PUFF_SIZE * PUFF_SIZE];
static PIXELICA_Texture g_puff = {g_puff_pixels, PUFF_SIZE, PUFF_SIZE};

/* A soft gray disc on a color-key background: no alpha channel, so edges are hard. */
static void build_puff(void)
{
    for (unsigned y = 0u; y < PUFF_SIZE; ++y) {
        for (unsigned x = 0u; x < PUFF_SIZE; ++x) {
            const int dx = (int)(2u * x) - 7; /* offsets from the center, doubled */
            const int dy = (int)(2u * y) - 7;
            const int d2 = dx * dx + dy * dy;

            if (d2 <= 40) {
                const unsigned shade = 232u - (unsigned)d2 * 2u;
                g_puff_pixels[y * PUFF_SIZE + x] = PIXELICA_RGB565(shade, shade, shade + 8u);
            } else {
                g_puff_pixels[y * PUFF_SIZE + x] = PUFF_KEY;
            }
        }
    }
}

static bool setup_emitter(PIXELICA_ParticleEmitter *emitter, PIXELICA_Particle *storage, uint32_t capacity,
                          PIXELICA_ParticlePreset preset, float x, float y, float rate, uint32_t seed)
{
    PIXELICA_ParticleConfig config;

    if (pixelica_particles_preset(preset, true, &config) != PIXELICA_OK) {
        return false;
    }
    config.position.x = x;
    config.position.y = y;
    if (rate >= 0.0f) {
        config.rate = rate;
    }
    return pixelica_particles_init(emitter, storage, capacity, &config, seed) == PIXELICA_OK;
}

static bool scene_init(Scene *s)
{
    memset(s, 0, sizeof *s);
    if (!setup_emitter(&s->sparks, s->spark_store, SPARK_CAPACITY, PIXELICA_PARTICLES_SPARKS, 25.0f,
                       (float)GROUND_Y, -1.0f, 1u) ||
        !setup_emitter(&s->smoke, s->smoke_store, SMOKE_CAPACITY, PIXELICA_PARTICLES_SMOKE, 85.0f,
                       (float)GROUND_Y, -1.0f, 2u)) {
        return false;
    }
    for (uint32_t i = 0u; i < RAIN_EMITTERS; ++i) {
        /* Each narrow rain column makes 30 drops per second. */
        if (!setup_emitter(&s->rain[i], s->rain_store[i], RAIN_CAPACITY, PIXELICA_PARTICLES_RAIN,
                           105.0f + 15.0f * (float)i, 0.0f, 30.0f, 100u + i)) {
            return false;
        }
    }
    return true;
}

/* One fixed simulation tick; the spark emitter walks back and forth along the ground. */
static bool scene_step(Scene *s, uint32_t tick)
{
    const uint32_t phase = tick % 60u;
    PIXELICA_Vec3 spark_at = {25.0f + (float)(phase < 30u ? phase : 60u - phase), (float)GROUND_Y, 0.0f};

    if (pixelica_particles_move(&s->sparks, spark_at) != PIXELICA_OK) {
        return false;
    }
    if (tick == BURST_TICK && pixelica_particles_burst(&s->sparks, BURST_COUNT, NULL) != PIXELICA_OK) {
        return false;
    }
    if (pixelica_particles_update(&s->sparks, TICK_DT) != PIXELICA_OK ||
        pixelica_particles_update(&s->smoke, TICK_DT) != PIXELICA_OK) {
        return false;
    }
    for (uint32_t i = 0u; i < RAIN_EMITTERS; ++i) {
        if (pixelica_particles_update(&s->rain[i], TICK_DT) != PIXELICA_OK) {
            return false;
        }
    }
    return true;
}

static bool scene_draw(PIXELICA2D_Context *ctx, const Scene *s)
{
    PIXELICA2D_Sprite puff;
    PIXELICA2D_Rect ground = {0, GROUND_Y, WIDTH, HEIGHT - GROUND_Y};

    if (pixelica2d_clear(ctx, PIXELICA_RGB565(8, 12, 40)) != PIXELICA_OK) {
        return false;
    }
    for (int y = 0; y < GROUND_Y; ++y) { /* sky gradient */
        PIXELICA2D_Rect row = {0, y, WIDTH, 1};

        if (pixelica2d_fill_rect(ctx, &row, PIXELICA_RGB565(8 + y / 5, 14 + y / 4, 44 + y / 2), 255u) != PIXELICA_OK) {
            return false;
        }
    }
    if (pixelica2d_fill_rect(ctx, &ground, PIXELICA_RGB565(60, 44, 32), 255u) != PIXELICA_OK) {
        return false;
    }

    if (pixelica2d_sprite_init(&puff, &g_puff) != PIXELICA_OK) {
        return false;
    }
    puff.pivot.x = (float)PUFF_SIZE / 2.0f; /* centered particles */
    puff.pivot.y = (float)PUFF_SIZE / 2.0f;
    puff.color_key_enabled = true;
    puff.color_key = PUFF_KEY;

    /* Back to front: rain, smoke, sparks. Each emitter is drawn in pool order. */
    for (uint32_t i = 0u; i < RAIN_EMITTERS; ++i) {
        if (pixelica2d_particles_draw(ctx, &s->rain[i], NULL) != PIXELICA_OK) {
            return false;
        }
    }
    return pixelica2d_particles_draw(ctx, &s->smoke, &puff) == PIXELICA_OK &&
           pixelica2d_particles_draw(ctx, &s->sparks, NULL) == PIXELICA_OK;
}

static uint64_t fnv1a_pixels(const uint16_t *pixels, size_t count)
{
    uint64_t hash = 14695981039346656037ull;

    for (size_t i = 0u; i < count; ++i) {
        /* Little-endian byte order, independent of the host. */
        hash = (hash ^ (uint64_t)(pixels[i] & 0xFFu)) * 1099511628211ull;
        hash = (hash ^ (uint64_t)(pixels[i] >> 8)) * 1099511628211ull;
    }
    return hash;
}

/* Simulates `ticks` ticks from scratch and leaves the final frame in g_pixels. */
static bool run_scene(uint32_t ticks, uint64_t *checksum, uint32_t *live, uint32_t *dropped)
{
    PIXELICA2D_Surface surface;
    PIXELICA2D_Context ctx;

    memset(&surface, 0, sizeof surface);
    surface.pixels = g_pixels;
    surface.width = WIDTH;
    surface.height = HEIGHT;
    surface.stride = WIDTH;
    if (pixelica2d_init(&ctx, &surface) != PIXELICA_OK || !scene_init(&g_scene)) {
        return false;
    }
    for (uint32_t tick = 0u; tick < ticks; ++tick) {
        if (!scene_step(&g_scene, tick)) {
            return false;
        }
    }
    if (!scene_draw(&ctx, &g_scene)) {
        return false;
    }
    *checksum = fnv1a_pixels(g_pixels, (size_t)WIDTH * HEIGHT);
    *live = g_scene.sparks.count + g_scene.smoke.count;
    *dropped = g_scene.sparks.dropped + g_scene.smoke.dropped;
    for (uint32_t i = 0u; i < RAIN_EMITTERS; ++i) {
        *live += g_scene.rain[i].count;
        *dropped += g_scene.rain[i].dropped;
    }
    return true;
}

static uint8_t expand(uint32_t value, unsigned bits)
{
    return (uint8_t)((value << (8u - bits)) | (value >> (2u * bits - 8u)));
}

static bool write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    bool ok;

    if (file == NULL) {
        return false;
    }
    ok = fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT) > 0;
    for (size_t i = 0u; ok && i < (size_t)WIDTH * HEIGHT; ++i) {
        const uint32_t p = g_pixels[i];
        const uint8_t rgb[3] = {expand(p >> 11, 5u), expand((p >> 5) & 63u, 6u), expand(p & 31u, 5u)};

        ok = fwrite(rgb, 1u, sizeof rgb, file) == sizeof rgb;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    uint32_t ticks = DEFAULT_TICKS;
    uint64_t first = 0u;
    uint64_t second = 0u;
    uint32_t live = 0u;
    uint32_t dropped = 0u;
    uint32_t live_again = 0u;
    uint32_t dropped_again = 0u;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            const long value = strtol(argv[++i], NULL, 10);

            if (value < 1 || value > (long)MAX_TICKS) {
                fprintf(stderr, "hello_particles: --ticks takes 1..%u\n", MAX_TICKS);
                return 2;
            }
            ticks = (uint32_t)value;
        } else {
            fprintf(stderr, "usage: hello_particles [--output frame.ppm] [--ticks N]\n");
            return 2;
        }
    }

    build_puff();
    if (!run_scene(ticks, &first, &live, &dropped) || !run_scene(ticks, &second, &live_again, &dropped_again)) {
        fprintf(stderr, "hello_particles: simulation or drawing failed\n");
        return 1;
    }
    printf("hello_particles: %u ticks, %u live particles, %u dropped, frame checksum 0x%016" PRIx64 "\n",
           (unsigned)ticks, (unsigned)live, (unsigned)dropped, first);
    if (first != second || live != live_again || dropped != dropped_again) {
        fprintf(stderr, "hello_particles: two identical runs differ (not deterministic)\n");
        return 1;
    }
    if (live == 0u) {
        fprintf(stderr, "hello_particles: nothing was emitted\n");
        return 1;
    }
    if (output != NULL) {
        if (!write_ppm(output)) {
            fprintf(stderr, "hello_particles: cannot write %s\n", output);
            return 1;
        }
        printf("hello_particles: wrote %dx%d PPM to %s\n", WIDTH, HEIGHT, output);
    }
    return 0;
}
