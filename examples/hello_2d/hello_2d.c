/*
 * hello_2d: a scrolling top-down tilemap with an animated, mirrored sprite
 * atlas and a translucent HUD drawn with the bitmap font.
 *
 *   - pixelica2d_draw_tilemap: a 40x30 map of 16x16 tiles from a generated 4x2
 *     atlas, scrolled by the 2D camera (the camera follows the hero and is
 *     clamped to the map). Water animates by swapping two tile ids.
 *   - pixelica2d_sprite_frame: the hero is a 4-frame 16x24 atlas; the frame comes
 *     from a PIXELICA_AnimKeyframe track sampled through a PIXELICA_AnimPlayer.
 *   - Negative scale.x mirrors a sprite: the hero faces the way it walks and
 *     the NPC always faces left.
 *   - HUD: pixelica2d_fill_rect with opacity + pixelica2d_draw_text, in screen space.
 *
 * Keys: arrows move, A pauses, START resets. 2D +Y points DOWN.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../common2d/demo2d.h"
#include "../common2d/demo_art.h"
#include "pixelica/animation.h"

#define MAP_COLS 40u
#define MAP_ROWS 30u
#define WORLD_W (MAP_COLS * DEMO_TILE_SIZE)
#define WORLD_H (MAP_ROWS * DEMO_TILE_SIZE)
#define WALK_SPEED 1.5f /* world pixels per fixed step */
#define WALK_CYCLE_TICKS 32u
#define WATER_CYCLE_TICKS 60u
#define START_TILE_X 12u
#define START_TILE_Y 15u
#define NPC_X 392.0f
#define NPC_Y 248.0f

typedef struct Hello2D {
    PIXELICA2D_Context ctx;
    PIXELICA_Texture tile_tex, hero_tex, shadow_tex;
    PIXELICA2D_Font font;
    uint16_t tile_pixels[DEMO_TILE_PIXELS];
    uint16_t hero_pixels[DEMO_HERO_PIXELS];
    uint16_t shadow_pixels[DEMO_SHADOW_PIXELS];
    uint16_t map[MAP_COLS * MAP_ROWS];
    PIXELICA_AnimPlayer walk, npc_walk, water;
    PIXELICA_Status error; /* first failure seen by update(), reported by render() */
    uint32_t previous_buttons;
    uint32_t tick;
    uint32_t water_phase;
    float x, y; /* hero feet, world pixels */
    bool facing_left, moving, paused;
} Hello2D;

static Hello2D g_state;

/* Walk cycle: stand, step, stand, step (frames of the hero atlas). */
static const PIXELICA_AnimKeyframe k_walk_keys[4] = {
    {0u, 0.0f, PIXELICA_EASE_STEP},
    {8u, 1.0f, PIXELICA_EASE_STEP},
    {16u, 2.0f, PIXELICA_EASE_STEP},
    {24u, 3.0f, PIXELICA_EASE_STEP},
};
static const PIXELICA_AnimKeyframe k_water_keys[2] = {
    {0u, 0.0f, PIXELICA_EASE_STEP},
    {30u, 1.0f, PIXELICA_EASE_STEP},
};

static uint16_t map_tile(uint32_t cx, uint32_t cy)
{
    const int32_t dx = (int32_t)cx - 28;
    const int32_t dy = (int32_t)cy - 10;
    const int32_t lake = dx * dx * 16 + dy * dy * 36; /* ellipse 6 x 4 tiles */
    const uint32_t h = demo_art_hash(cx, cy, 1u);
    const bool near_start = demo_art_abs((int32_t)cx - (int32_t)START_TILE_X) <= 2 &&
                            demo_art_abs((int32_t)cy - (int32_t)START_TILE_Y) <= 2;

    if (cx == 0u || cy == 0u || cx == MAP_COLS - 1u || cy == MAP_ROWS - 1u) {
        return DEMO_TILE_STONE;
    }
    if (lake <= 576) {
        return DEMO_TILE_WATER_A;
    }
    if (lake <= 576 + 280) {
        return DEMO_TILE_SAND;
    }
    if (cy == START_TILE_Y || cx == START_TILE_X) {
        return DEMO_TILE_PATH;
    }
    if (!near_start && h % 17u == 0u) {
        return DEMO_TILE_BUSH;
    }
    return (uint16_t)(((h >> 8) % 7u == 1u) ? DEMO_TILE_FLOWER : DEMO_TILE_GRASS);
}

static bool tile_is_solid(const Hello2D *s, float wx, float wy)
{
    const int32_t px = (int32_t)floorf(wx);
    const int32_t py = (int32_t)floorf(wy);
    uint16_t id;

    if (px < 0 || py < 0 || px >= (int32_t)WORLD_W || py >= (int32_t)WORLD_H) {
        return true;
    }
    id = s->map[(uint32_t)py / DEMO_TILE_SIZE * MAP_COLS + (uint32_t)px / DEMO_TILE_SIZE];
    return id == DEMO_TILE_STONE || id == DEMO_TILE_BUSH || id == DEMO_TILE_WATER_A ||
           id == DEMO_TILE_WATER_B;
}

/* The hero's footprint is a 8x3 pixel box around the feet point. */
static bool footprint_blocked(const Hello2D *s, float x, float y)
{
    return tile_is_solid(s, x - 4.0f, y) || tile_is_solid(s, x + 4.0f, y) ||
           tile_is_solid(s, x - 4.0f, y - 3.0f) || tile_is_solid(s, x + 4.0f, y - 3.0f);
}

static PIXELICA_Status reset_world(Hello2D *s)
{
    s->x = (float)(START_TILE_X * DEMO_TILE_SIZE + DEMO_TILE_SIZE / 2u);
    s->y = (float)(START_TILE_Y * DEMO_TILE_SIZE + DEMO_TILE_SIZE / 2u);
    s->facing_left = false;
    s->moving = false;
    s->paused = false;
    s->tick = 0u;
    s->water_phase = 0u;
    s->error = PIXELICA_OK;
    DEMO_TRY(pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP));
    DEMO_TRY(pixelica_anim_player_init(&s->npc_walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP));
    DEMO_TRY(pixelica_anim_player_init(&s->water, WATER_CYCLE_TICKS, PIXELICA_ANIM_LOOP));
    return PIXELICA_OK;
}

/* Frame index (0..3) of a walk player. */
static PIXELICA_Status walk_frame(const PIXELICA_AnimPlayer *player, uint32_t *out_frame)
{
    uint32_t tick = 0u;
    float value = 0.0f;

    DEMO_TRY(pixelica_anim_player_tick(player, &tick));
    DEMO_TRY(pixelica_anim_sample(k_walk_keys, 4u, tick, &value));
    *out_frame = (uint32_t)(value + 0.5f);
    if (*out_frame >= DEMO_HERO_FRAMES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

static bool demo_init(void **out_state, const PIXELICA2D_Surface *surface)
{
    Hello2D *s = &g_state;
    uint32_t cx;
    uint32_t cy;

    if (out_state == NULL || surface == NULL) {
        return false;
    }
    *out_state = NULL;
    memset(s, 0, sizeof *s);
    if (surface->width != PIXELICA2D_DEMO_WIDTH || surface->height != PIXELICA2D_DEMO_HEIGHT ||
        pixelica2d_init(&s->ctx, surface) != PIXELICA_OK) {
        return false;
    }
    if (!demo_art_tiles_build(s->tile_pixels, DEMO_TILE_PIXELS) ||
        !demo_art_hero_build(s->hero_pixels, DEMO_HERO_PIXELS) ||
        !demo_art_shadow_build(s->shadow_pixels, DEMO_SHADOW_PIXELS)) {
        return false;
    }
    s->tile_tex.pixels = s->tile_pixels;
    s->tile_tex.width = (uint16_t)DEMO_TILE_ATLAS_W;
    s->tile_tex.height = (uint16_t)DEMO_TILE_ATLAS_H;
    s->hero_tex.pixels = s->hero_pixels;
    s->hero_tex.width = (uint16_t)DEMO_HERO_ATLAS_W;
    s->hero_tex.height = (uint16_t)DEMO_HERO_ATLAS_H;
    s->shadow_tex.pixels = s->shadow_pixels;
    s->shadow_tex.width = (uint16_t)DEMO_SHADOW_W;
    s->shadow_tex.height = (uint16_t)DEMO_SHADOW_H;
    s->font = demo_art_font();

    for (cy = 0u; cy < MAP_ROWS; ++cy) {
        for (cx = 0u; cx < MAP_COLS; ++cx) {
            s->map[cy * MAP_COLS + cx] = map_tile(cx, cy);
        }
    }
    if (reset_world(s) != PIXELICA_OK) {
        return false;
    }
    *out_state = s;
    return true;
}

static void demo_update(void *state, uint32_t buttons)
{
    Hello2D *s = state;
    const uint32_t pressed = buttons & ~s->previous_buttons;
    float dx = 0.0f;
    float dy = 0.0f;
    PIXELICA_Status status;

    s->previous_buttons = buttons;
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        status = reset_world(s);
        if (status != PIXELICA_OK) {
            s->error = status;
        }
        return;
    }
    if ((pressed & PIXELICA_BTN_A) != 0u) {
        s->paused = !s->paused;
    }
    if (s->paused) {
        return;
    }
    ++s->tick;

    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        dx -= WALK_SPEED;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        dx += WALK_SPEED;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        dy -= WALK_SPEED;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        dy += WALK_SPEED;
    }
    if ((dx < 0.0f || dx > 0.0f) && (dy < 0.0f || dy > 0.0f)) {
        dx *= 0.70710678f;
        dy *= 0.70710678f;
    }
    /* Axes are tried separately so the hero slides along walls and water. */
    if ((dx < 0.0f || dx > 0.0f) && !footprint_blocked(s, s->x + dx, s->y)) {
        s->x += dx;
    }
    if ((dy < 0.0f || dy > 0.0f) && !footprint_blocked(s, s->x, s->y + dy)) {
        s->y += dy;
    }
    s->moving = (buttons & (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT | PIXELICA_BTN_UP | PIXELICA_BTN_DOWN)) != 0u;
    if ((buttons & PIXELICA_BTN_LEFT) != 0u && (buttons & PIXELICA_BTN_RIGHT) == 0u) {
        s->facing_left = true;
    } else if ((buttons & PIXELICA_BTN_RIGHT) != 0u && (buttons & PIXELICA_BTN_LEFT) == 0u) {
        s->facing_left = false;
    }

    if (s->moving) {
        status = pixelica_anim_player_advance(&s->walk, 1u);
    } else {
        status = pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP); /* back to stand */
    }
    if (status == PIXELICA_OK) {
        status = pixelica_anim_player_advance(&s->npc_walk, 1u);
    }
    if (status == PIXELICA_OK) {
        status = pixelica_anim_player_advance(&s->water, 1u);
    }
    if (status == PIXELICA_OK) {
        uint32_t tick = 0u;
        float value = 0.0f;

        status = pixelica_anim_player_tick(&s->water, &tick);
        if (status == PIXELICA_OK) {
            status = pixelica_anim_sample(k_water_keys, 2u, tick, &value);
        }
        if (status == PIXELICA_OK && (uint32_t)(value + 0.5f) != s->water_phase) {
            uint32_t i;

            s->water_phase = (uint32_t)(value + 0.5f);
            for (i = 0u; i < MAP_COLS * MAP_ROWS; ++i) {
                if (s->map[i] == DEMO_TILE_WATER_A || s->map[i] == DEMO_TILE_WATER_B) {
                    s->map[i] = (uint16_t)(s->water_phase != 0u ? DEMO_TILE_WATER_B
                                                                : DEMO_TILE_WATER_A);
                }
            }
        }
    }
    if (status != PIXELICA_OK) {
        s->error = status;
    }
}

static PIXELICA_Status draw_character(Hello2D *s, float x, float y, uint32_t frame, bool mirror)
{
    PIXELICA2D_Sprite shadow;
    PIXELICA2D_Sprite body;
    const float px = floorf(x + 0.5f);
    const float py = floorf(y + 0.5f);

    DEMO_TRY(pixelica2d_sprite_init(&shadow, &s->shadow_tex));
    shadow.position.x = px;
    shadow.position.y = py;
    shadow.pivot.x = 8.0f;
    shadow.pivot.y = 3.0f;
    shadow.opacity = 96u;
    shadow.color_key_enabled = true;
    shadow.color_key = DEMO_KEY_COLOR;
    DEMO_TRY(pixelica2d_draw_sprite(&s->ctx, &shadow));

    DEMO_TRY(pixelica2d_sprite_init(&body, &s->hero_tex));
    DEMO_TRY(pixelica2d_sprite_frame(&body, frame, (uint16_t)DEMO_HERO_CELL_W,
                                (uint16_t)DEMO_HERO_CELL_H));
    body.position.x = px;
    body.position.y = py;
    body.pivot.x = (float)(DEMO_HERO_CELL_W / 2u);
    body.pivot.y = (float)DEMO_HERO_CELL_H;
    body.scale.x = mirror ? -1.0f : 1.0f; /* negative scale flips about the pivot */
    body.color_key_enabled = true;
    body.color_key = DEMO_KEY_COLOR;
    return pixelica2d_draw_sprite(&s->ctx, &body);
}

static PIXELICA_Status draw_line(Hello2D *s, int32_t x, int32_t y, const char *text, uint16_t color)
{
    return pixelica2d_draw_text(&s->ctx, &s->font, text, strlen(text), x, y, color, 255u);
}

static PIXELICA_Status demo_render(void *state)
{
    Hello2D *s = state;
    PIXELICA2D_Camera camera;
    PIXELICA2D_Tilemap map;
    PIXELICA2D_Rect top_panel = {0, 0, 256, 24};
    PIXELICA2D_Rect bottom_panel = {0, 180, 256, 12};
    uint32_t hero_frame = 0u;
    uint32_t npc_frame = 0u;
    char line[48];
    int written;

    if (s->error != PIXELICA_OK) {
        return s->error;
    }
    DEMO_TRY(walk_frame(&s->walk, &hero_frame));
    DEMO_TRY(walk_frame(&s->npc_walk, &npc_frame));

    camera.position.x = floorf(s->x) - (float)(PIXELICA2D_DEMO_WIDTH / 2u);
    camera.position.y = floorf(s->y) - (float)(PIXELICA2D_DEMO_HEIGHT / 2u);
    if (camera.position.x < 0.0f) {
        camera.position.x = 0.0f;
    }
    if (camera.position.y < 0.0f) {
        camera.position.y = 0.0f;
    }
    if (camera.position.x > (float)(WORLD_W - PIXELICA2D_DEMO_WIDTH)) {
        camera.position.x = (float)(WORLD_W - PIXELICA2D_DEMO_WIDTH);
    }
    if (camera.position.y > (float)(WORLD_H - PIXELICA2D_DEMO_HEIGHT)) {
        camera.position.y = (float)(WORLD_H - PIXELICA2D_DEMO_HEIGHT);
    }
    camera.zoom = 1.0f;

    DEMO_TRY(pixelica2d_clear(&s->ctx, PIXELICA_RGB565(20, 24, 40)));
    DEMO_TRY(pixelica2d_set_camera(&s->ctx, &camera));

    memset(&map, 0, sizeof map);
    map.atlas = &s->tile_tex;
    map.tiles = s->map;
    map.cols = (uint16_t)MAP_COLS;
    map.rows = (uint16_t)MAP_ROWS;
    map.tile_width = (uint16_t)DEMO_TILE_SIZE;
    map.tile_height = (uint16_t)DEMO_TILE_SIZE;
    map.stride_tiles = MAP_COLS;
    map.scale = 1.0f;
    map.opacity = 255u;
    DEMO_TRY(pixelica2d_draw_tilemap(&s->ctx, &map));

    /* Painter order by feet y: whoever stands lower on the screen is in front. */
    if (NPC_Y <= s->y) {
        DEMO_TRY(draw_character(s, NPC_X, NPC_Y, npc_frame, true));
        DEMO_TRY(draw_character(s, s->x, s->y, hero_frame, s->facing_left));
    } else {
        DEMO_TRY(draw_character(s, s->x, s->y, hero_frame, s->facing_left));
        DEMO_TRY(draw_character(s, NPC_X, NPC_Y, npc_frame, true));
    }

    /* HUD: screen space, translucent plates + bitmap text. */
    DEMO_TRY(pixelica2d_fill_rect(&s->ctx, &top_panel, PIXELICA_RGB565(8, 10, 24), 150u));
    DEMO_TRY(pixelica2d_fill_rect(&s->ctx, &bottom_panel, PIXELICA_RGB565(8, 10, 24), 150u));
    DEMO_TRY(draw_line(s, 4, 4, "PIXELICA2D HELLO 2D  TILEMAP+SPRITES", PIXELICA_RGB565(250, 250, 250)));
    written = snprintf(line, sizeof line, "X %d  Y %d  TICK %u", (int)s->x, (int)s->y,
                       (unsigned)s->tick);
    if (written < 0 || (size_t)written >= sizeof line) {
        return PIXELICA_ERR_CAPACITY;
    }
    DEMO_TRY(draw_line(s, 4, 14, line, PIXELICA_RGB565(244, 200, 64)));
    DEMO_TRY(draw_line(s, 4, 183, "ARROWS MOVE  A PAUSE  START RESET", PIXELICA_RGB565(200, 210, 230)));
    if (s->paused) {
        DEMO_TRY(demo_art_hud_label(&s->ctx, &s->font, 110, 90, "PAUSED", PIXELICA_RGB565(244, 200, 64)));
    }
    return PIXELICA_OK;
}

static void demo_shutdown(void *state)
{
    (void)state;
}

const PIXELICA2D_Demo pixelica2d_example_demo = {
    "Pixelica2D hello_2d",
    (uint16_t)PIXELICA2D_DEMO_WIDTH,
    (uint16_t)PIXELICA2D_DEMO_HEIGHT,
    demo_init,
    demo_update,
    demo_render,
    demo_shutdown,
};
