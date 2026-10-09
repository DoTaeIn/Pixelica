/*
 * Generated pixel art + tiny helpers shared by the 2D / isometric / 2.5D demos.
 *
 * Header-only on purpose: every function is `static inline`, so a demo that
 * does not use one of them compiles warning-free and no extra CMake target or
 * dependency is needed. Nothing here allocates: builders fill caller-owned
 * RGB565 arrays after checking their capacity, and validate the whole ASCII
 * art (row lengths, palette letters) before writing the first pixel.
 *
 * Asset format: plain RGB565 sprite sheets with the color key 0xF81F
 * (magenta) as transparency - the same layout pixelica_pack's texture loader
 * produces, so the generated sheets could be replaced by packed textures
 * without touching the demos. Color-key sprites need no alpha channel.
 *
 * Contents
 *   demo_art_font()               5x7 bitmap font, ASCII 32..90 (' '..'Z', 59 glyphs, upper case)
 *   demo_art_hero_build()         64x24 sheet, four 16x24 walk frames (front view)
 *   demo_art_tiles_build()        64x32 sheet, eight 16x16 top-down tiles
 *   demo_art_iso_tiles_build()    96x16 sheet, three 32x16 diamond floor tiles (keyed)
 *   demo_art_iso_blocks_build()   64x32 sheet, two 32x32 isometric blocks (keyed)
 *   demo_art_shadow_build()       16x6 ellipse shadow (keyed)
 *   demo_art_hud_label()          translucent plate + text, screen space
 */
#ifndef PIXELICA2D_EXAMPLE_DEMO_ART_H
#define PIXELICA2D_EXAMPLE_DEMO_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/renderer2d.h"

/* Return early with the status of any call that did not succeed. */
#define DEMO_TRY(expr)                                                    \
    do {                                                                  \
        PIXELICA_Status demo_try_status_ = (expr);                             \
        if (demo_try_status_ != PIXELICA_OK) {                                 \
            return demo_try_status_;                                      \
        }                                                                 \
    } while (0)

#define DEMO_KEY_COLOR ((uint16_t)0xF81Fu) /* == PIXELICA_RGB565(255, 0, 255) */

#define DEMO_HERO_CELL_W 16u
#define DEMO_HERO_CELL_H 24u
#define DEMO_HERO_FRAMES 4u
#define DEMO_HERO_ATLAS_W (DEMO_HERO_CELL_W * DEMO_HERO_FRAMES)
#define DEMO_HERO_ATLAS_H DEMO_HERO_CELL_H
#define DEMO_HERO_PIXELS (DEMO_HERO_ATLAS_W * DEMO_HERO_ATLAS_H)

#define DEMO_TILE_SIZE 16u
#define DEMO_TILE_ATLAS_W 64u
#define DEMO_TILE_ATLAS_H 32u
#define DEMO_TILE_PIXELS (DEMO_TILE_ATLAS_W * DEMO_TILE_ATLAS_H)

enum {
    DEMO_TILE_GRASS = 0,
    DEMO_TILE_FLOWER,
    DEMO_TILE_PATH,
    DEMO_TILE_WATER_A,
    DEMO_TILE_WATER_B,
    DEMO_TILE_STONE,
    DEMO_TILE_BUSH,
    DEMO_TILE_SAND,
    DEMO_TILE_COUNT
};

#define DEMO_ISO_TILE_W 32u
#define DEMO_ISO_TILE_H 16u
#define DEMO_ISO_TILE_VARIANTS 3u
#define DEMO_ISO_TILES_ATLAS_W (DEMO_ISO_TILE_W * DEMO_ISO_TILE_VARIANTS)
#define DEMO_ISO_TILES_PIXELS (DEMO_ISO_TILES_ATLAS_W * DEMO_ISO_TILE_H)

#define DEMO_BLOCK_SIZE 32u
#define DEMO_BLOCK_VARIANTS 2u
#define DEMO_BLOCKS_ATLAS_W (DEMO_BLOCK_SIZE * DEMO_BLOCK_VARIANTS)
#define DEMO_BLOCKS_PIXELS (DEMO_BLOCKS_ATLAS_W * DEMO_BLOCK_SIZE)

#define DEMO_SHADOW_W 16u
#define DEMO_SHADOW_H 6u
#define DEMO_SHADOW_PIXELS (DEMO_SHADOW_W * DEMO_SHADOW_H)

#define DEMO_FONT_FIRST_CHAR 32u
#define DEMO_FONT_GLYPHS 59u
#define DEMO_FONT_ADVANCE 6 /* glyph width 5 + 1 */

static inline int32_t demo_art_abs(int32_t v)
{
    return v < 0 ? -v : v;
}

/* Small integer hash for deterministic noise. */
static inline uint32_t demo_art_hash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = (a * 0x9E3779B1u) ^ ((b + 0x7F4A7C15u) * 0x85EBCA6Bu) ^
                 ((c + 0x165667B1u) * 0xC2B2AE35u);

    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

/* ------------------------------------------------------------------ font */

/* Classic 5x7 rows: five bits per row, stored left-aligned (bit 7 = left). */
#define DEMO_G(a, b, c, d, e, f, g)                                        \
    (uint8_t)((a) << 3), (uint8_t)((b) << 3), (uint8_t)((c) << 3),        \
        (uint8_t)((d) << 3), (uint8_t)((e) << 3), (uint8_t)((f) << 3),    \
        (uint8_t)((g) << 3)

static inline PIXELICA2D_Font demo_art_font(void)
{
    static const uint8_t rows[DEMO_FONT_GLYPHS * 7u] = {
        DEMO_G(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00), /* ' ' */
        DEMO_G(0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04), /* ! */
        DEMO_G(0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00), /* " */
        DEMO_G(0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A), /* # */
        DEMO_G(0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04), /* $ */
        DEMO_G(0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03), /* % */
        DEMO_G(0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D), /* & */
        DEMO_G(0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00), /* ' */
        DEMO_G(0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02), /* ( */
        DEMO_G(0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08), /* ) */
        DEMO_G(0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00), /* * */
        DEMO_G(0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00), /* + */
        DEMO_G(0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08), /* , */
        DEMO_G(0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00), /* - */
        DEMO_G(0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C), /* . */
        DEMO_G(0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00), /* / */
        DEMO_G(0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E), /* 0 */
        DEMO_G(0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E), /* 1 */
        DEMO_G(0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F), /* 2 */
        DEMO_G(0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E), /* 3 */
        DEMO_G(0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02), /* 4 */
        DEMO_G(0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E), /* 5 */
        DEMO_G(0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E), /* 6 */
        DEMO_G(0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08), /* 7 */
        DEMO_G(0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E), /* 8 */
        DEMO_G(0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C), /* 9 */
        DEMO_G(0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00), /* : */
        DEMO_G(0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08), /* ; */
        DEMO_G(0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02), /* < */
        DEMO_G(0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00), /* = */
        DEMO_G(0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08), /* > */
        DEMO_G(0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04), /* ? */
        DEMO_G(0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E), /* @ */
        DEMO_G(0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11), /* A */
        DEMO_G(0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E), /* B */
        DEMO_G(0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E), /* C */
        DEMO_G(0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C), /* D */
        DEMO_G(0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F), /* E */
        DEMO_G(0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10), /* F */
        DEMO_G(0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F), /* G */
        DEMO_G(0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11), /* H */
        DEMO_G(0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E), /* I */
        DEMO_G(0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C), /* J */
        DEMO_G(0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11), /* K */
        DEMO_G(0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F), /* L */
        DEMO_G(0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11), /* M */
        DEMO_G(0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11), /* N */
        DEMO_G(0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E), /* O */
        DEMO_G(0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10), /* P */
        DEMO_G(0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D), /* Q */
        DEMO_G(0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11), /* R */
        DEMO_G(0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E), /* S */
        DEMO_G(0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04), /* T */
        DEMO_G(0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E), /* U */
        DEMO_G(0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04), /* V */
        DEMO_G(0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A), /* W */
        DEMO_G(0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11), /* X */
        DEMO_G(0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04), /* Y */
        DEMO_G(0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F), /* Z */
    };
    PIXELICA2D_Font font;

    font.rows = rows;
    font.first_char = (uint16_t)DEMO_FONT_FIRST_CHAR;
    font.glyph_count = (uint16_t)DEMO_FONT_GLYPHS;
    font.width = 5u;
    font.height = 7u;
    return font;
}

/*
 * Translucent plate with text on top, in screen pixels (no camera, no depth).
 * `text` must be NUL-terminated and at most 80 characters, upper case only
 * (the font stops at 'Z'; other bytes advance without ink).
 */
static inline PIXELICA_Status demo_art_hud_label(PIXELICA2D_Context *ctx, const PIXELICA2D_Font *font,
                                            int32_t x, int32_t y, const char *text,
                                            uint16_t color)
{
    PIXELICA2D_Rect plate;
    size_t length;

    if (ctx == NULL || font == NULL || text == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    length = strlen(text);
    if (length == 0u || length > 80u) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    plate.x = x - 2;
    plate.y = y - 2;
    plate.width = (int32_t)length * DEMO_FONT_ADVANCE + 3;
    plate.height = (int32_t)font->height + 4;
    DEMO_TRY(pixelica2d_fill_rect(ctx, &plate, PIXELICA_RGB565(8, 10, 24), 170u));
    return pixelica2d_draw_text(ctx, font, text, length, x, y, color, 255u);
}

/* ----------------------------------------------------------- ASCII art */

static inline bool demo_art_palette(char c, uint16_t *out)
{
    switch (c) {
    case '.': *out = DEMO_KEY_COLOR; return true;
    case 'k': *out = PIXELICA_RGB565(28, 22, 40); return true;    /* outline */
    case 'h': *out = PIXELICA_RGB565(122, 70, 36); return true;   /* hair */
    case 's': *out = PIXELICA_RGB565(244, 196, 150); return true; /* skin */
    case 'c': *out = PIXELICA_RGB565(232, 140, 128); return true; /* cheek */
    case 'r': *out = PIXELICA_RGB565(210, 52, 60); return true;   /* tunic */
    case 'R': *out = PIXELICA_RGB565(140, 30, 48); return true;   /* tunic shade */
    case 'y': *out = PIXELICA_RGB565(244, 200, 64); return true;  /* trim / staff */
    case 'b': *out = PIXELICA_RGB565(112, 72, 40); return true;   /* belt */
    case 'p': *out = PIXELICA_RGB565(52, 90, 190); return true;   /* pants */
    case 'o': *out = PIXELICA_RGB565(96, 56, 36); return true;    /* boots */
    case 'm': *out = PIXELICA_RGB565(96, 232, 240); return true;  /* staff orb */
    case 'w': *out = PIXELICA_RGB565(250, 250, 250); return true; /* white */
    default: return false;
    }
}

/*
 * Copies `row_count` ASCII rows of exactly `width` characters into a
 * stride-`stride` RGB565 array at (x0, y0). Checks the target capacity, every
 * row length and every palette letter first; nothing is written on failure.
 */
static inline bool demo_art_blit_ascii(uint16_t *dst, size_t capacity, uint32_t stride,
                                       uint32_t x0, uint32_t y0, const char *const *rows,
                                       uint32_t row_count, uint32_t width)
{
    uint32_t x;
    uint32_t y;
    uint16_t color;

    if (dst == NULL || rows == NULL || width == 0u || row_count == 0u || x0 > stride ||
        width > stride - x0 || y0 > 65535u || row_count > 65535u) {
        return false;
    }
    if (((size_t)y0 + row_count - 1u) * stride + x0 + width > capacity) {
        return false;
    }
    for (y = 0u; y < row_count; ++y) {
        if (rows[y] == NULL || strlen(rows[y]) != width) {
            return false;
        }
        for (x = 0u; x < width; ++x) {
            if (!demo_art_palette(rows[y][x], &color)) {
                return false;
            }
        }
    }
    for (y = 0u; y < row_count; ++y) {
        for (x = 0u; x < width; ++x) {
            (void)demo_art_palette(rows[y][x], &color);
            dst[(size_t)(y0 + y) * stride + x0 + x] = color;
        }
    }
    return true;
}

/*
 * Hero: 4 frames of 16x24 in one row (64x24). Order = walk cycle
 * stand, step-left, stand, step-right. The staff on the right makes the
 * sprite asymmetric so a mirrored (negative scale) draw is visible.
 */
static inline bool demo_art_hero_build(uint16_t *pixels, size_t capacity)
{
    static const char *const top[16] = {
        "...." "...." "...." "....",
        "...." "kkkk" "kkkk" "....",
        "...k" "hhhh" "hhhh" "k...",
        "..kh" "hhhh" "hhhh" "hk..",
        "..kh" "hhhh" "hhhh" "hk..",
        "..kh" "ssss" "ssss" "hk..",
        "..ks" "skss" "ssks" "sk..",
        "..ks" "ssss" "ssss" "sk..",
        "..ks" "cssk" "kssc" "sk..",
        "...k" "ssss" "ssss" "k.m.",
        "...." "kkss" "sskk" "..y.",
        "...k" "RRrr" "rrRR" "k.y.",
        "..kR" "rrrr" "rrrr" "Rky.",
        "..kR" "rryy" "yyrr" "Rky.",
        "..ks" "rrrr" "rrrr" "sky.",
        "..ks" "bbby" "ybbb" "sky.",
    };
    static const char *const legs_stand[8] = {
        "...k" "pppp" "pppp" "k.y.",
        "...k" "pppk" "kppp" "k.y.",
        "...k" "pppk" "kppp" "k...",
        "...k" "pppk" "kppp" "k...",
        "...k" "pppk" "kppp" "k...",
        "..ko" "oook" "kooo" "ok..",
        "..ko" "oook" "kooo" "ok..",
        "..kk" "kkkk" "kkkk" "kk..",
    };
    static const char *const legs_left[8] = {
        "...k" "pppp" "pppp" "k.y.",
        "...k" "pppk" "kppp" "k.y.",
        "...k" "pppk" "kppp" "k...",
        "..ko" "oook" "kppp" "k...",
        "..ko" "oook" "kppp" "k...",
        "..kk" "kkkk" "kppp" "k...",
        "...." "...." "kooo" "ok..",
        "...." "...." "kkkk" "kk..",
    };
    static const char *const legs_right[8] = {
        "...k" "pppp" "pppp" "k.y.",
        "...k" "pppk" "kppp" "k.y.",
        "...k" "pppk" "kppp" "k...",
        "...k" "pppk" "kooo" "ok..",
        "...k" "pppk" "kooo" "ok..",
        "...k" "pppk" "kkkk" "kk..",
        "..ko" "oook" "...." "....",
        "..kk" "kkkk" "...." "....",
    };
    const char *const *const legs[DEMO_HERO_FRAMES] = {legs_stand, legs_left, legs_stand,
                                                       legs_right};
    uint32_t frame;

    if (pixels == NULL || capacity < DEMO_HERO_PIXELS) {
        return false;
    }
    for (frame = 0u; frame < DEMO_HERO_FRAMES; ++frame) {
        if (!demo_art_blit_ascii(pixels, capacity, DEMO_HERO_ATLAS_W, frame * DEMO_HERO_CELL_W, 0u,
                                 top, 16u, DEMO_HERO_CELL_W) ||
            !demo_art_blit_ascii(pixels, capacity, DEMO_HERO_ATLAS_W, frame * DEMO_HERO_CELL_W, 16u,
                                 legs[frame], 8u, DEMO_HERO_CELL_W)) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------- top-down tiles */

static inline uint16_t demo_art_tile_pixel(uint32_t tile, uint32_t x, uint32_t y)
{
    const uint32_t n = demo_art_hash(x, y, tile);

    switch (tile) {
    case DEMO_TILE_GRASS:
    case DEMO_TILE_FLOWER: {
        uint16_t color = PIXELICA_RGB565(66, 156, 64);

        if (n % 8u == 0u) {
            color = PIXELICA_RGB565(44, 124, 52);
        } else if (n % 8u == 1u) {
            color = PIXELICA_RGB565(98, 190, 84);
        }
        if (tile == DEMO_TILE_FLOWER) {
            static const int32_t flowers[3][2] = {{3, 4}, {11, 3}, {7, 11}};
            size_t i;

            for (i = 0u; i < 3u; ++i) {
                const int32_t dx = (int32_t)x - flowers[i][0];
                const int32_t dy = (int32_t)y - flowers[i][1];

                if (dx == 0 && dy == 0) {
                    color = PIXELICA_RGB565(248, 214, 64);
                } else if (demo_art_abs(dx) + demo_art_abs(dy) == 1) {
                    color = PIXELICA_RGB565(250, 250, 250);
                }
            }
        }
        return color;
    }
    case DEMO_TILE_PATH:
        if (n % 10u == 0u) {
            return PIXELICA_RGB565(150, 108, 66);
        }
        if (n % 10u == 1u) {
            return PIXELICA_RGB565(224, 186, 134);
        }
        return PIXELICA_RGB565(196, 152, 98);
    case DEMO_TILE_WATER_A:
    case DEMO_TILE_WATER_B: {
        const uint32_t shift = tile == DEMO_TILE_WATER_B ? 4u : 0u;

        if (y % 4u == 1u && (x + shift + 2u * (y / 4u)) % 8u < 2u) {
            return PIXELICA_RGB565(132, 190, 250);
        }
        return n % 7u == 0u ? PIXELICA_RGB565(40, 100, 196) : PIXELICA_RGB565(52, 116, 210);
    }
    case DEMO_TILE_STONE: {
        const uint32_t row = y / 8u;

        if (y % 8u == 7u || (x + (row % 2u) * 4u) % 8u == 7u) {
            return PIXELICA_RGB565(80, 84, 98); /* mortar */
        }
        if (y % 8u == 0u) {
            return PIXELICA_RGB565(170, 174, 188); /* lit top edge of a brick */
        }
        return n % 6u == 0u ? PIXELICA_RGB565(116, 120, 134) : PIXELICA_RGB565(134, 138, 152);
    }
    case DEMO_TILE_BUSH: {
        const int32_t dx = 2 * (int32_t)x - 15;
        const int32_t dy = 2 * (int32_t)y - 16;
        const int32_t d2 = dx * dx + dy * dy;

        if (d2 > 169) {
            return demo_art_tile_pixel(DEMO_TILE_GRASS, x, y);
        }
        if (d2 >= 121) {
            return PIXELICA_RGB565(22, 80, 40); /* outline */
        }
        if (dx < 0 && dy < 0 && d2 <= 60) {
            return PIXELICA_RGB565(104, 190, 90); /* highlight */
        }
        return n % 5u == 0u ? PIXELICA_RGB565(32, 108, 48) : PIXELICA_RGB565(42, 130, 58);
    }
    case DEMO_TILE_SAND:
        return n % 9u == 0u ? PIXELICA_RGB565(200, 172, 104) : PIXELICA_RGB565(232, 208, 140);
    default:
        return DEMO_KEY_COLOR;
    }
}

static inline bool demo_art_tiles_build(uint16_t *pixels, size_t capacity)
{
    uint32_t tile;
    uint32_t x;
    uint32_t y;

    if (pixels == NULL || capacity < DEMO_TILE_PIXELS) {
        return false;
    }
    for (tile = 0u; tile < DEMO_TILE_COUNT; ++tile) {
        const uint32_t ox = (tile % 4u) * DEMO_TILE_SIZE;
        const uint32_t oy = (tile / 4u) * DEMO_TILE_SIZE;

        for (y = 0u; y < DEMO_TILE_SIZE; ++y) {
            for (x = 0u; x < DEMO_TILE_SIZE; ++x) {
                pixels[(size_t)(oy + y) * DEMO_TILE_ATLAS_W + ox + x] =
                    demo_art_tile_pixel(tile, x, y);
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------- isometric */

/* Diamond metric: <= 32 inside the 32x16 diamond, > 28 on its rim. */
static inline int32_t demo_art_diamond(uint32_t x, uint32_t y)
{
    return demo_art_abs(2 * (int32_t)x + 1 - 32) + 2 * demo_art_abs(2 * (int32_t)y + 1 - 16);
}

/* Three keyed 32x16 floor diamonds: light grass, dark grass, stone path. */
static inline bool demo_art_iso_tiles_build(uint16_t *pixels, size_t capacity)
{
    uint32_t variant;
    uint32_t x;
    uint32_t y;

    if (pixels == NULL || capacity < DEMO_ISO_TILES_PIXELS) {
        return false;
    }
    for (variant = 0u; variant < DEMO_ISO_TILE_VARIANTS; ++variant) {
        for (y = 0u; y < DEMO_ISO_TILE_H; ++y) {
            for (x = 0u; x < DEMO_ISO_TILE_W; ++x) {
                const int32_t metric = demo_art_diamond(x, y);
                const uint32_t n = demo_art_hash(x, y, variant + 100u);
                uint16_t color = DEMO_KEY_COLOR;

                if (metric <= 32) {
                    if (variant == 0u) {
                        color = metric > 28 ? PIXELICA_RGB565(50, 120, 56)
                                            : (n % 6u == 0u ? PIXELICA_RGB565(112, 198, 98)
                                                            : PIXELICA_RGB565(88, 176, 80));
                    } else if (variant == 1u) {
                        color = metric > 28 ? PIXELICA_RGB565(42, 100, 50)
                                            : (n % 6u == 0u ? PIXELICA_RGB565(90, 172, 86)
                                                            : PIXELICA_RGB565(70, 150, 68));
                    } else {
                        color = metric > 28 ? PIXELICA_RGB565(120, 90, 60)
                                            : (n % 5u == 0u ? PIXELICA_RGB565(212, 178, 130)
                                                            : PIXELICA_RGB565(186, 150, 104));
                    }
                }
                pixels[(size_t)y * DEMO_ISO_TILES_ATLAS_W + variant * DEMO_ISO_TILE_W + x] = color;
            }
        }
    }
    return true;
}

/* Face of an isometric block at sprite pixel (x, y): 0 none, 1 top, 2 left, 3 right. */
static inline int demo_art_block_face(int32_t x, int32_t y)
{
    int32_t top;

    if (x < 0 || x > 31 || y < 0 || y > 31) {
        return 0;
    }
    if (demo_art_diamond((uint32_t)x, (uint32_t)y) <= 32 && y < 16) {
        return 1;
    }
    top = 8 + (x < 16 ? x : 31 - x) / 2;
    if (y >= top && y < top + 17) {
        return x < 16 ? 2 : 3;
    }
    return 0;
}

/*
 * Two keyed 32x32 blocks (crate, stone). The top diamond's apex is at
 * (16, 0); the 1x1 footprint's apex is at (16, 16): draw with pivot (16, 16)
 * at the footprint's ground point.
 */
static inline bool demo_art_iso_blocks_build(uint16_t *pixels, size_t capacity)
{
    static const uint16_t palette[DEMO_BLOCK_VARIANTS][4] = {
        {PIXELICA_RGB565(60, 36, 20), PIXELICA_RGB565(216, 166, 98), PIXELICA_RGB565(170, 120, 66),
         PIXELICA_RGB565(126, 84, 46)},
        {PIXELICA_RGB565(40, 42, 56), PIXELICA_RGB565(178, 182, 194), PIXELICA_RGB565(134, 138, 152),
         PIXELICA_RGB565(96, 100, 116)},
    };
    uint32_t variant;
    int32_t x;
    int32_t y;

    if (pixels == NULL || capacity < DEMO_BLOCKS_PIXELS) {
        return false;
    }
    for (variant = 0u; variant < DEMO_BLOCK_VARIANTS; ++variant) {
        for (y = 0; y < (int32_t)DEMO_BLOCK_SIZE; ++y) {
            for (x = 0; x < (int32_t)DEMO_BLOCK_SIZE; ++x) {
                const int face = demo_art_block_face(x, y);
                uint16_t color = DEMO_KEY_COLOR;

                if (face != 0) {
                    const bool rim = demo_art_block_face(x - 1, y) == 0 ||
                                     demo_art_block_face(x + 1, y) == 0 ||
                                     demo_art_block_face(x, y - 1) == 0 ||
                                     demo_art_block_face(x, y + 1) == 0;

                    color = palette[variant][rim ? 0 : face];
                    if (!rim && face != 1) {
                        const int32_t top = 8 + (x < 16 ? x : 31 - x) / 2;

                        if (variant == 0u && (y - top) % 8 == 7) {
                            color = palette[variant][0]; /* plank seam */
                        } else if (variant == 1u &&
                                   demo_art_hash((uint32_t)x, (uint32_t)y, 7u) % 7u == 0u) {
                            color = palette[variant][face == 2 ? 3 : 0];
                        }
                    }
                }
                pixels[(size_t)y * DEMO_BLOCKS_ATLAS_W + variant * DEMO_BLOCK_SIZE + (uint32_t)x] = color;
            }
        }
    }
    return true;
}

/* 16x6 keyed black ellipse; draw it translucent under a character. */
static inline bool demo_art_shadow_build(uint16_t *pixels, size_t capacity)
{
    uint32_t x;
    uint32_t y;

    if (pixels == NULL || capacity < DEMO_SHADOW_PIXELS) {
        return false;
    }
    for (y = 0u; y < DEMO_SHADOW_H; ++y) {
        for (x = 0u; x < DEMO_SHADOW_W; ++x) {
            const int32_t dx = 2 * (int32_t)x + 1 - (int32_t)DEMO_SHADOW_W;
            const int32_t dy = 2 * (int32_t)y + 1 - (int32_t)DEMO_SHADOW_H;
            const bool inside = dx * dx * 36 + dy * dy * 256 <= 9216;

            pixels[(size_t)y * DEMO_SHADOW_W + x] = inside ? (uint16_t)PIXELICA_RGB565(0, 0, 0)
                                                           : DEMO_KEY_COLOR;
        }
    }
    return true;
}

#endif /* PIXELICA2D_EXAMPLE_DEMO_ART_H */
