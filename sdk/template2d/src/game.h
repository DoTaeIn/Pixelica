/*
 * Pixelica2D starter game - portable logic, no platform code, no 3D.
 *
 * Uses only Pixelica2D::core headers (pixelica/renderer2d.h and the common headers it
 * includes). The caller owns the RGB565 target; the game keeps one
 * PIXELICA2D_Context over it and allocates nothing.
 */
#ifndef PIXELICA2D_TEMPLATE_GAME_H
#define PIXELICA2D_TEMPLATE_GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/input.h"
#include "pixelica/renderer2d.h"

#define TEMPLATE2D_WIDTH 160u
#define TEMPLATE2D_HEIGHT 120u

#define TEMPLATE2D_GEM_COUNT 4u
#define TEMPLATE2D_GEM_FRAMES 4u
#define TEMPLATE2D_GEM_SIZE 8u
#define TEMPLATE2D_PLAYER_SIZE 12u
#define TEMPLATE2D_TILE_SIZE 16u
#define TEMPLATE2D_MAP_COLS (TEMPLATE2D_WIDTH / TEMPLATE2D_TILE_SIZE)
#define TEMPLATE2D_MAP_ROWS 8u /* 128 rows of pixels; the bottom 8 are clipped */
#define TEMPLATE2D_SCORE_SEGMENTS 10u

typedef struct Template2DGame {
    PIXELICA2D_Context ctx;
    PIXELICA_Texture tiles, gems, player;
    uint16_t tile_pixels[2u * TEMPLATE2D_TILE_SIZE * TEMPLATE2D_TILE_SIZE];
    uint16_t gem_pixels[TEMPLATE2D_GEM_FRAMES * TEMPLATE2D_GEM_SIZE * TEMPLATE2D_GEM_SIZE];
    uint16_t player_pixels[TEMPLATE2D_PLAYER_SIZE * TEMPLATE2D_PLAYER_SIZE];
    uint16_t map[TEMPLATE2D_MAP_COLS * TEMPLATE2D_MAP_ROWS];
    float player_x, player_y; /* top-left corner, screen pixels (+Y down) */
    float gem_x[TEMPLATE2D_GEM_COUNT], gem_y[TEMPLATE2D_GEM_COUNT];
    uint32_t score;
    uint32_t tick;
    uint32_t seed;
    uint32_t previous_buttons;
    bool paused;
} Template2DGame;

/*
 * Builds the art and the map, binds the game to `surface` (which must be
 * TEMPLATE2D_WIDTH x TEMPLATE2D_HEIGHT). Returns false and keeps nothing on
 * failure. `surface->pixels` must stay valid while the game is used.
 */
bool template2d_game_init(Template2DGame *game, const PIXELICA2D_Surface *surface);

/* Fixed 60 Hz step; `buttons` is a PIXELICA_BTN_* mask. A pauses, START resets. */
void template2d_game_update(Template2DGame *game, uint32_t buttons);

/* Draws one complete frame into the surface; the caller presents it. */
PIXELICA_Status template2d_game_render(Template2DGame *game);

#endif /* PIXELICA2D_TEMPLATE_GAME_H */
