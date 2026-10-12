/*
 * hello_shaders - Pixelica's optional CPU shader module, run offscreen. No
 * window, no SDL, no renderer and no asset files: only the common library and
 * pixelica/shaders.h.
 *
 * A procedural 160x120 RGB565 scene (sky, sun, checkerboard floor, color bars)
 * is copied once per effect, each of the ten built-in effects is applied to its
 * own copy, and every result is reduced with 2x2 RGB8 box averaging into a panel of
 * a 5x2 comparison sheet:
 *
 *   tint      grayscale  invert    posterize  palette
 *   scanlines vignette   blur      outline    custom
 *
 * The sheet is built twice from scratch and the two hashes must match. Every
 * panel must also differ from the unprocessed scene and from every other panel.
 *
 *   hello_shaders                     prints the sheet hash
 *   hello_shaders --output sheet.ppm  also writes the sheet as a binary PPM
 *
 * The hash is only comparable between runs of the same build.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pixelica/image.h"
#include "pixelica/shaders.h"
#include "pixelica/status.h"

#define WIDTH 160u
#define HEIGHT 120u
#define COLUMNS 5u
#define ROWS 2u
#define PANEL_W (WIDTH / 2u)
#define PANEL_H (HEIGHT / 2u)
#define SHEET_W (COLUMNS * PANEL_W)
#define SHEET_H (ROWS * PANEL_H)
#define EFFECTS (COLUMNS * ROWS)

static uint16_t g_base[WIDTH * HEIGHT];
static uint16_t g_work[WIDTH * HEIGHT];
static uint16_t g_scratch[WIDTH * HEIGHT];
static uint16_t g_sheet[SHEET_W * SHEET_H];

static const uint16_t g_palette[4] = {
    PIXELICA_RGB565(16, 16, 48), PIXELICA_RGB565(208, 64, 48), PIXELICA_RGB565(240, 208, 96),
    PIXELICA_RGB565(240, 240, 240)};

static const char *const g_names[EFFECTS] = {"tint",      "grayscale", "invert", "posterize", "palette",
                                             "scanlines", "vignette",  "blur",   "outline",   "custom"};

static void build_scene(void)
{
    for (unsigned y = 0u; y < HEIGHT; ++y) {
        for (unsigned x = 0u; x < WIDTH; ++x) {
            uint16_t color;

            if (y < 76u) { /* sky gradient with a sun */
                const int dx = (int)x - 118;
                const int dy = (int)y - 30;

                if (dx * dx + dy * dy <= 14 * 14) {
                    color = PIXELICA_RGB565(255, 224, 96);
                } else {
                    color = PIXELICA_RGB565(24 + y, 48 + y * 2u, 160 + y / 2u);
                }
            } else if (y < 96u) { /* six solid color bars */
                static const uint16_t bars[6] = {
                    PIXELICA_RGB565(220, 40, 40),  PIXELICA_RGB565(230, 150, 30), PIXELICA_RGB565(230, 220, 40),
                    PIXELICA_RGB565(40, 180, 70),  PIXELICA_RGB565(50, 90, 220),  PIXELICA_RGB565(150, 60, 190)};

                color = bars[x * 6u / WIDTH];
            } else { /* checkerboard floor */
                color = (((x / 8u) + (y / 8u)) & 1u) != 0u ? PIXELICA_RGB565(200, 200, 200)
                                                           : PIXELICA_RGB565(60, 50, 40);
            }
            g_base[y * WIDTH + x] = color;
        }
    }
}

typedef struct SmearState {
    uint32_t calls;
} SmearState;

/* Custom effect: green stays, red and blue come from the pixel 3 columns to the left. */
static uint16_t smear(const PIXELICA_ShaderPixel *pixel, void *userdata)
{
    SmearState *state = (SmearState *)userdata;
    const uint16_t left = pixelica_shader_sample(pixel->source, (int32_t)pixel->x - 3, (int32_t)pixel->y);

    ++state->calls;
    return (uint16_t)((pixel->color & 0x07E0u) | (left & 0xF81Fu));
}

static bool make_shader(unsigned index, PIXELICA_Shader *shader, SmearState *state)
{
    static const PIXELICA_ShaderKind kinds[EFFECTS] = {
        PIXELICA_SHADER_TINT,      PIXELICA_SHADER_GRAYSCALE, PIXELICA_SHADER_INVERT,
        PIXELICA_SHADER_POSTERIZE, PIXELICA_SHADER_PALETTE,   PIXELICA_SHADER_SCANLINES,
        PIXELICA_SHADER_VIGNETTE,  PIXELICA_SHADER_BLUR,      PIXELICA_SHADER_OUTLINE,
        PIXELICA_SHADER_CUSTOM};

    if (pixelica_shader_default(kinds[index], shader) != PIXELICA_OK) {
        return false;
    }
    switch (kinds[index]) {
    case PIXELICA_SHADER_TINT:
        shader->color = PIXELICA_RGB565(255, 160, 64);
        shader->amount = 0.5f;
        break;
    case PIXELICA_SHADER_POSTERIZE:
        shader->levels = 3u;
        break;
    case PIXELICA_SHADER_PALETTE:
        shader->palette = g_palette;
        shader->palette_count = 4u;
        break;
    case PIXELICA_SHADER_SCANLINES:
        shader->amount = 0.5f;
        break;
    case PIXELICA_SHADER_BLUR:
        shader->radius = 2u;
        break;
    case PIXELICA_SHADER_CUSTOM:
        shader->callback = smear;
        shader->userdata = state;
        break;
    default:
        break;
    }
    return true;
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

/* Average both row parities so scanlines survive the comparison thumbnail. */
static uint16_t thumbnail_pixel(const uint16_t *pixels, unsigned x, unsigned y)
{
    uint32_t r = 0u, g = 0u, b = 0u;
    for (unsigned dy = 0u; dy < 2u; ++dy) {
        for (unsigned dx = 0u; dx < 2u; ++dx) {
            const uint32_t p = pixels[(2u * y + dy) * WIDTH + 2u * x + dx];
            const uint32_t r5 = p >> 11, g6 = (p >> 5) & 63u, b5 = p & 31u;
            r += (r5 << 3) | (r5 >> 2);
            g += (g6 << 2) | (g6 >> 4);
            b += (b5 << 3) | (b5 >> 2);
        }
    }
    return PIXELICA_RGB565((r + 2u) / 4u, (g + 2u) / 4u, (b + 2u) / 4u);
}

/* Applies effect `index` to a fresh copy of the scene and stores it as one panel. */
static bool render_panel(unsigned index, uint64_t *panel_hash, uint32_t *callbacks)
{
    PIXELICA_ShaderSurface surface = {g_work, (uint16_t)WIDTH, (uint16_t)HEIGHT, WIDTH};
    PIXELICA_Shader shader;
    SmearState state = {0u};
    size_t need;
    uint16_t panel[PANEL_W * PANEL_H];

    memcpy(g_work, g_base, sizeof g_work);
    if (!make_shader(index, &shader, &state)) {
        return false;
    }
    need = pixelica_shader_scratch_pixels(&surface, &shader);
    if (need > sizeof g_scratch / sizeof g_scratch[0]) {
        return false;
    }
    if (pixelica_shader_apply(&surface, &shader, need != 0u ? g_scratch : NULL, need) != PIXELICA_OK) {
        return false;
    }
    for (unsigned y = 0u; y < PANEL_H; ++y) {
        for (unsigned x = 0u; x < PANEL_W; ++x) {
            const uint16_t color = thumbnail_pixel(g_work, x, y);
            const unsigned px = (index % COLUMNS) * PANEL_W + x;
            const unsigned py = (index / COLUMNS) * PANEL_H + y;

            panel[y * PANEL_W + x] = color;
            g_sheet[py * SHEET_W + px] = color;
        }
    }
    *panel_hash = fnv1a_pixels(panel, PANEL_W * PANEL_H);
    *callbacks = state.calls;
    return true;
}

static bool build_sheet(uint64_t *sheet_hash, uint64_t panel_hashes[EFFECTS])
{
    memset(g_sheet, 0, sizeof g_sheet);
    for (unsigned i = 0u; i < EFFECTS; ++i) {
        uint32_t callbacks = 0u;

        if (!render_panel(i, &panel_hashes[i], &callbacks)) {
            fprintf(stderr, "hello_shaders: effect '%s' failed\n", g_names[i]);
            return false;
        }
        /* The custom shader must have been called exactly once per pixel. */
        if (i == EFFECTS - 1u && callbacks != WIDTH * HEIGHT) {
            fprintf(stderr, "hello_shaders: custom callback ran %u times, expected %u\n", (unsigned)callbacks,
                    (unsigned)(WIDTH * HEIGHT));
            return false;
        }
    }
    *sheet_hash = fnv1a_pixels(g_sheet, (size_t)SHEET_W * SHEET_H);
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
    ok = fprintf(file, "P6\n%u %u\n255\n", SHEET_W, SHEET_H) > 0;
    for (size_t i = 0u; ok && i < (size_t)SHEET_W * SHEET_H; ++i) {
        const uint32_t p = g_sheet[i];
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
    uint64_t first = 0u;
    uint64_t second = 0u;
    uint64_t panels[EFFECTS];
    uint64_t panels_again[EFFECTS];

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else {
            fprintf(stderr, "usage: hello_shaders [--output sheet.ppm]\n");
            return 2;
        }
    }

    build_scene();
    if (!build_sheet(&first, panels) || !build_sheet(&second, panels_again)) {
        return 1;
    }
    printf("hello_shaders: %ux%u sheet of %u effects, hash 0x%016" PRIx64 "\n", SHEET_W, SHEET_H, EFFECTS,
           first);
    if (first != second || memcmp(panels, panels_again, sizeof panels) != 0) {
        fprintf(stderr, "hello_shaders: two identical runs differ (not deterministic)\n");
        return 1;
    }
    for (unsigned i = 0u; i < EFFECTS; ++i) {
        /* Compare the same 2x2 averaging for the reference and effect. */
        uint16_t reference[PANEL_W * PANEL_H];

        for (unsigned y = 0u; y < PANEL_H; ++y) {
            for (unsigned x = 0u; x < PANEL_W; ++x) {
                reference[y * PANEL_W + x] = thumbnail_pixel(g_base, x, y);
            }
        }
        if (panels[i] == fnv1a_pixels(reference, PANEL_W * PANEL_H)) {
            fprintf(stderr, "hello_shaders: effect '%s' left the scene unchanged\n", g_names[i]);
            return 1;
        }
        for (unsigned j = i + 1u; j < EFFECTS; ++j) {
            if (panels[i] == panels[j]) {
                fprintf(stderr, "hello_shaders: effects '%s' and '%s' look identical\n", g_names[i], g_names[j]);
                return 1;
            }
        }
    }
    if (output != NULL) {
        if (!write_ppm(output)) {
            fprintf(stderr, "hello_shaders: cannot write %s\n", output);
            return 1;
        }
        printf("hello_shaders: wrote %ux%u PPM to %s\n", SHEET_W, SHEET_H, output);
    }
    return 0;
}
