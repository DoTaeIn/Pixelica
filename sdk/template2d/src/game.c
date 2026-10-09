#include "game.h"

#include <string.h>

#define KEY_COLOR ((uint16_t)0xF81Fu) /* PIXELICA_RGB565(255, 0, 255): transparent */
#define PLAYER_SPEED 1.5f
#define PICKUP_REACH 10.0f

/* Deterministic LCG so every run places gems identically. */
static uint32_t next_random(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

static void place_gem(Template2DGame *game, uint32_t index)
{
    const uint32_t span_x = TEMPLATE2D_WIDTH - 2u * TEMPLATE2D_GEM_SIZE;
    const uint32_t span_y = TEMPLATE2D_HEIGHT - 3u * TEMPLATE2D_GEM_SIZE;

    game->gem_x[index] = (float)(TEMPLATE2D_GEM_SIZE + next_random(&game->seed) % span_x);
    game->gem_y[index] = (float)(2u * TEMPLATE2D_GEM_SIZE + next_random(&game->seed) % span_y);
}

static void reset(Template2DGame *game)
{
    uint32_t i;

    game->player_x = (float)((TEMPLATE2D_WIDTH - TEMPLATE2D_PLAYER_SIZE) / 2u);
    game->player_y = (float)((TEMPLATE2D_HEIGHT - TEMPLATE2D_PLAYER_SIZE) / 2u);
    game->score = 0u;
    game->tick = 0u;
    game->seed = 12345u;
    game->paused = false;
    for (i = 0u; i < TEMPLATE2D_GEM_COUNT; ++i) {
        place_gem(game, i);
    }
}

static void build_tiles(Template2DGame *game)
{
    uint32_t tile;
    uint32_t x;
    uint32_t y;

    for (tile = 0u; tile < 2u; ++tile) {
        for (y = 0u; y < TEMPLATE2D_TILE_SIZE; ++y) {
            for (x = 0u; x < TEMPLATE2D_TILE_SIZE; ++x) {
                const bool edge = x == 0u || y == 0u;
                uint16_t color = tile == 0u ? PIXELICA_RGB565(30, 36, 58) : PIXELICA_RGB565(38, 46, 72);

                if (edge) {
                    color = PIXELICA_RGB565(24, 28, 46);
                }
                /* Two tiles side by side: a 32x16 sheet, tile ids 0 and 1. */
                game->tile_pixels[y * (2u * TEMPLATE2D_TILE_SIZE) + tile * TEMPLATE2D_TILE_SIZE + x] =
                    color;
            }
        }
    }
}

/* Four 8x8 frames of a diamond that "spins" by narrowing and widening. */
static void build_gems(Template2DGame *game)
{
    static const int32_t half_width[TEMPLATE2D_GEM_FRAMES] = {3, 2, 1, 2};
    uint32_t frame;
    uint32_t x;
    uint32_t y;

    for (frame = 0u; frame < TEMPLATE2D_GEM_FRAMES; ++frame) {
        for (y = 0u; y < TEMPLATE2D_GEM_SIZE; ++y) {
            for (x = 0u; x < TEMPLATE2D_GEM_SIZE; ++x) {
                const int32_t dy = (int32_t)y < 4 ? 3 - (int32_t)y : (int32_t)y - 4;
                const int32_t reach = half_width[frame] * (4 - dy) / 4;
                const int32_t dx = (int32_t)x < 4 ? 3 - (int32_t)x : (int32_t)x - 4;
                uint16_t color = KEY_COLOR;

                if (dx <= reach) {
                    color = (dx == reach || dy == 3) ? PIXELICA_RGB565(40, 120, 150)
                                                     : PIXELICA_RGB565(96, 232, 240);
                    if (x < 4u && y < 4u && dx + dy <= 2) {
                        color = PIXELICA_RGB565(220, 250, 255); /* highlight */
                    }
                }
                game->gem_pixels[y * (TEMPLATE2D_GEM_FRAMES * TEMPLATE2D_GEM_SIZE) +
                                 frame * TEMPLATE2D_GEM_SIZE + x] = color;
            }
        }
    }
}

static void build_player(Template2DGame *game)
{
    uint32_t x;
    uint32_t y;

    for (y = 0u; y < TEMPLATE2D_PLAYER_SIZE; ++y) {
        for (x = 0u; x < TEMPLATE2D_PLAYER_SIZE; ++x) {
            const bool corner = (x < 2u || x > 9u) && (y < 2u || y > 9u);
            const bool rim = x == 0u || y == 0u || x == 11u || y == 11u;
            uint16_t color = PIXELICA_RGB565(244, 200, 64);

            if (corner) {
                color = KEY_COLOR;
            } else if (rim) {
                color = PIXELICA_RGB565(28, 22, 40);
            } else if ((x == 4u || x == 7u) && y == 4u) {
                color = PIXELICA_RGB565(28, 22, 40); /* eyes */
            }
            game->player_pixels[y * TEMPLATE2D_PLAYER_SIZE + x] = color;
        }
    }
}

bool template2d_game_init(Template2DGame *game, const PIXELICA2D_Surface *surface)
{
    uint32_t x;
    uint32_t y;

    if (game == NULL || surface == NULL || surface->width != TEMPLATE2D_WIDTH ||
        surface->height != TEMPLATE2D_HEIGHT) {
        return false;
    }
    memset(game, 0, sizeof *game);
    if (pixelica2d_init(&game->ctx, surface) != PIXELICA_OK) {
        return false;
    }
    build_tiles(game);
    build_gems(game);
    build_player(game);
    game->tiles.pixels = game->tile_pixels;
    game->tiles.width = (uint16_t)(2u * TEMPLATE2D_TILE_SIZE);
    game->tiles.height = (uint16_t)TEMPLATE2D_TILE_SIZE;
    game->gems.pixels = game->gem_pixels;
    game->gems.width = (uint16_t)(TEMPLATE2D_GEM_FRAMES * TEMPLATE2D_GEM_SIZE);
    game->gems.height = (uint16_t)TEMPLATE2D_GEM_SIZE;
    game->player.pixels = game->player_pixels;
    game->player.width = (uint16_t)TEMPLATE2D_PLAYER_SIZE;
    game->player.height = (uint16_t)TEMPLATE2D_PLAYER_SIZE;
    for (y = 0u; y < TEMPLATE2D_MAP_ROWS; ++y) {
        for (x = 0u; x < TEMPLATE2D_MAP_COLS; ++x) {
            game->map[y * TEMPLATE2D_MAP_COLS + x] = (uint16_t)((x + y) % 2u);
        }
    }
    reset(game);
    return true;
}

void template2d_game_update(Template2DGame *game, uint32_t buttons)
{
    const uint32_t pressed = buttons & ~game->previous_buttons;
    const float max_x = (float)(TEMPLATE2D_WIDTH - TEMPLATE2D_PLAYER_SIZE);
    const float max_y = (float)(TEMPLATE2D_HEIGHT - TEMPLATE2D_PLAYER_SIZE);
    uint32_t i;

    game->previous_buttons = buttons;
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        reset(game);
        return;
    }
    if ((pressed & PIXELICA_BTN_A) != 0u) {
        game->paused = !game->paused;
    }
    if (game->paused) {
        return;
    }
    ++game->tick;

    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        game->player_x -= PLAYER_SPEED;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        game->player_x += PLAYER_SPEED;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        game->player_y -= PLAYER_SPEED;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        game->player_y += PLAYER_SPEED;
    }
    if (game->player_x < 0.0f) {
        game->player_x = 0.0f;
    }
    if (game->player_y < 0.0f) {
        game->player_y = 0.0f;
    }
    if (game->player_x > max_x) {
        game->player_x = max_x;
    }
    if (game->player_y > max_y) {
        game->player_y = max_y;
    }

    for (i = 0u; i < TEMPLATE2D_GEM_COUNT; ++i) {
        const float dx = game->gem_x[i] + 4.0f - (game->player_x + 6.0f);
        const float dy = game->gem_y[i] + 4.0f - (game->player_y + 6.0f);

        if (dx > -PICKUP_REACH && dx < PICKUP_REACH && dy > -PICKUP_REACH && dy < PICKUP_REACH) {
            ++game->score;
            place_gem(game, i);
        }
    }
}

static PIXELICA_Status draw_keyed(Template2DGame *game, const PIXELICA_Texture *texture, uint32_t frame,
                             uint16_t cell, float x, float y)
{
    PIXELICA2D_Sprite sprite;
    PIXELICA_Status status = pixelica2d_sprite_init(&sprite, texture);

    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica2d_sprite_frame(&sprite, frame, cell, cell);
    if (status != PIXELICA_OK) {
        return status;
    }
    sprite.position.x = x;
    sprite.position.y = y;
    sprite.color_key_enabled = true;
    sprite.color_key = KEY_COLOR;
    return pixelica2d_draw_sprite(&game->ctx, &sprite);
}

PIXELICA_Status template2d_game_render(Template2DGame *game)
{
    PIXELICA2D_Tilemap map;
    PIXELICA2D_Rect bar = {4, 4, 84, 8};
    PIXELICA_Status status;
    uint32_t i;
    uint32_t filled = game->score;

    status = pixelica2d_clear(&game->ctx, PIXELICA_RGB565(20, 24, 40));
    if (status != PIXELICA_OK) {
        return status;
    }

    memset(&map, 0, sizeof map);
    map.atlas = &game->tiles;
    map.tiles = game->map;
    map.cols = (uint16_t)TEMPLATE2D_MAP_COLS;
    map.rows = (uint16_t)TEMPLATE2D_MAP_ROWS;
    map.tile_width = (uint16_t)TEMPLATE2D_TILE_SIZE;
    map.tile_height = (uint16_t)TEMPLATE2D_TILE_SIZE;
    map.stride_tiles = TEMPLATE2D_MAP_COLS;
    map.scale = 1.0f;
    map.opacity = 255u;
    status = pixelica2d_draw_tilemap(&game->ctx, &map);
    if (status != PIXELICA_OK) {
        return status;
    }

    for (i = 0u; i < TEMPLATE2D_GEM_COUNT; ++i) {
        status = draw_keyed(game, &game->gems, (game->tick / 6u + i) % TEMPLATE2D_GEM_FRAMES,
                            (uint16_t)TEMPLATE2D_GEM_SIZE, game->gem_x[i], game->gem_y[i]);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    status = draw_keyed(game, &game->player, 0u, (uint16_t)TEMPLATE2D_PLAYER_SIZE,
                        game->player_x, game->player_y);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* HUD: translucent bar, one segment per collected gem (wraps after 10). */
    status = pixelica2d_fill_rect(&game->ctx, &bar, PIXELICA_RGB565(8, 10, 24), 170u);
    if (status != PIXELICA_OK) {
        return status;
    }
    if (filled > TEMPLATE2D_SCORE_SEGMENTS) {
        filled = (filled - 1u) % TEMPLATE2D_SCORE_SEGMENTS + 1u;
    }
    for (i = 0u; i < filled; ++i) {
        PIXELICA2D_Rect segment;

        segment.x = 6 + (int32_t)(i * 8u);
        segment.y = 6;
        segment.width = 6;
        segment.height = 4;
        status = pixelica2d_fill_rect(&game->ctx, &segment, PIXELICA_RGB565(96, 232, 240), 255u);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    if (game->paused) {
        PIXELICA2D_Rect pause_bar = {TEMPLATE2D_WIDTH / 2 - 8, TEMPLATE2D_HEIGHT / 2 - 8, 16, 16};

        status = pixelica2d_fill_rect(&game->ctx, &pause_bar, PIXELICA_RGB565(244, 200, 64), 200u);
    }
    return status;
}
