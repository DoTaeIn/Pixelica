/*
 * hello_isometric: 2D-only fake depth. An 8x8 isometric floor built from
 * color-keyed diamond sprites, stacked blocks and a hopping character, all
 * submitted as ONE pixelica2d_draw_list. There is no depth buffer: occlusion comes
 * purely from the painter order the list guarantees (layer ascending, then
 * sort_y ascending, then insertion order).
 *
 *   layer 0  floor diamonds   sort_y = ground y of the tile centre
 *   layer 1  the hero's shadow (always on the ground)
 *   layer 2  blocks and hero  sort_y = ground y of their footprint centre
 *
 * "Ground y" is the screen y of the point at height 0, so the hop (z > 0)
 * lifts the sprite but never changes its sort key - a jumping hero does not
 * pop in front of or behind a block it is standing next to.
 *
 * Projection: pixelica2d_iso_project, 32x16 tiles, 1 pixel per height unit. World
 * x/y are in tile units (+x goes down-right on screen, +y down-left), z is
 * height in pixels. The caller-owned `order` workspace has room for every
 * item. Keys: arrows move in SCREEN directions, A pauses, START resets.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../common2d/demo2d.h"
#include "../common2d/demo_art.h"
#include "pixelica/animation.h"

#define MAP_N 8u
#define ITEM_CAPACITY 96u
#define HOP_CYCLE_TICKS 48u
#define WALK_CYCLE_TICKS 32u
#define WALK_SPEED 0.04f /* tiles per fixed step */
#define BLOCK_HEIGHT 16.0f
#define START_X 3.5f
#define START_Y 3.5f

typedef struct Block {
    uint8_t cx, cy, level, variant;
} Block;

typedef struct HelloIso {
    PIXELICA2D_Context ctx;
    PIXELICA2D_Iso iso;
    PIXELICA_Texture floor_tex, block_tex, hero_tex, shadow_tex;
    PIXELICA2D_Font font;
    uint16_t floor_pixels[DEMO_ISO_TILES_PIXELS];
    uint16_t block_pixels[DEMO_BLOCKS_PIXELS];
    uint16_t hero_pixels[DEMO_HERO_PIXELS];
    uint16_t shadow_pixels[DEMO_SHADOW_PIXELS];
    PIXELICA2D_Item items[ITEM_CAPACITY];
    size_t order[ITEM_CAPACITY]; /* caller-owned sort workspace */
    PIXELICA_AnimPlayer hop, walk;
    PIXELICA_Status error;
    uint32_t previous_buttons;
    uint32_t tick;
    float x, y; /* hero ground position, tile units */
    bool facing_left, moving, paused;
} HelloIso;

static HelloIso g_state;

static const Block k_blocks[] = {
    {2u, 2u, 0u, 0u}, {5u, 2u, 0u, 1u}, {2u, 5u, 0u, 0u}, {5u, 5u, 0u, 0u}, {5u, 5u, 1u, 1u},
};
#define BLOCK_COUNT (sizeof k_blocks / sizeof k_blocks[0])

/* Hop height in pixels over one 48-tick cycle: rise, fall, rest. */
static const PIXELICA_AnimKeyframe k_hop_keys[4] = {
    {0u, 0.0f, PIXELICA_EASE_QUAD_OUT},
    {12u, 14.0f, PIXELICA_EASE_QUAD_IN},
    {24u, 0.0f, PIXELICA_EASE_STEP},
    {48u, 0.0f, PIXELICA_EASE_STEP},
};
static const PIXELICA_AnimKeyframe k_walk_keys[4] = {
    {0u, 0.0f, PIXELICA_EASE_STEP},
    {8u, 1.0f, PIXELICA_EASE_STEP},
    {16u, 2.0f, PIXELICA_EASE_STEP},
    {24u, 3.0f, PIXELICA_EASE_STEP},
};

static PIXELICA_Status reset_world(HelloIso *s)
{
    s->x = START_X;
    s->y = START_Y;
    s->facing_left = false;
    s->moving = false;
    s->paused = false;
    s->tick = 0u;
    s->error = PIXELICA_OK;
    DEMO_TRY(pixelica_anim_player_init(&s->hop, HOP_CYCLE_TICKS, PIXELICA_ANIM_LOOP));
    return pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP);
}

static bool demo_init(void **out_state, const PIXELICA2D_Surface *surface)
{
    HelloIso *s = &g_state;

    if (out_state == NULL || surface == NULL) {
        return false;
    }
    *out_state = NULL;
    memset(s, 0, sizeof *s);
    if (surface->width != PIXELICA2D_DEMO_WIDTH || surface->height != PIXELICA2D_DEMO_HEIGHT ||
        pixelica2d_init(&s->ctx, surface) != PIXELICA_OK) {
        return false;
    }
    if (!demo_art_iso_tiles_build(s->floor_pixels, DEMO_ISO_TILES_PIXELS) ||
        !demo_art_iso_blocks_build(s->block_pixels, DEMO_BLOCKS_PIXELS) ||
        !demo_art_hero_build(s->hero_pixels, DEMO_HERO_PIXELS) ||
        !demo_art_shadow_build(s->shadow_pixels, DEMO_SHADOW_PIXELS)) {
        return false;
    }
    s->floor_tex.pixels = s->floor_pixels;
    s->floor_tex.width = (uint16_t)DEMO_ISO_TILES_ATLAS_W;
    s->floor_tex.height = (uint16_t)DEMO_ISO_TILE_H;
    s->block_tex.pixels = s->block_pixels;
    s->block_tex.width = (uint16_t)DEMO_BLOCKS_ATLAS_W;
    s->block_tex.height = (uint16_t)DEMO_BLOCK_SIZE;
    s->hero_tex.pixels = s->hero_pixels;
    s->hero_tex.width = (uint16_t)DEMO_HERO_ATLAS_W;
    s->hero_tex.height = (uint16_t)DEMO_HERO_ATLAS_H;
    s->shadow_tex.pixels = s->shadow_pixels;
    s->shadow_tex.width = (uint16_t)DEMO_SHADOW_W;
    s->shadow_tex.height = (uint16_t)DEMO_SHADOW_H;
    s->font = demo_art_font();

    s->iso.tile_width = (float)DEMO_ISO_TILE_W;
    s->iso.tile_height = (float)DEMO_ISO_TILE_H;
    s->iso.height_scale = 1.0f;
    s->iso.origin.x = 128.0f;
    s->iso.origin.y = 36.0f;
    if (reset_world(s) != PIXELICA_OK) {
        return false;
    }
    *out_state = s;
    return true;
}

static bool hero_blocked(float x, float y)
{
    size_t i;

    if (x < 0.25f || y < 0.25f || x > (float)MAP_N - 0.25f || y > (float)MAP_N - 0.25f) {
        return true;
    }
    for (i = 0u; i < BLOCK_COUNT; ++i) {
        const float cx = (float)k_blocks[i].cx;
        const float cy = (float)k_blocks[i].cy;

        if (x > cx - 0.2f && x < cx + 1.2f && y > cy - 0.2f && y < cy + 1.2f) {
            return true;
        }
    }
    return false;
}

static void demo_update(void *state, uint32_t buttons)
{
    HelloIso *s = state;
    const uint32_t pressed = buttons & ~s->previous_buttons;
    float vx = 0.0f;
    float vy = 0.0f;
    float length;
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

    /* Arrow keys are SCREEN directions; map them onto the iso axes. */
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        vx += 1.0f;
        vy -= 1.0f;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        vx -= 1.0f;
        vy += 1.0f;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        vx -= 1.0f;
        vy -= 1.0f;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        vx += 1.0f;
        vy += 1.0f;
    }
    length = sqrtf(vx * vx + vy * vy);
    s->moving = length > 0.0f;
    if (s->moving) {
        float dx = vx / length * WALK_SPEED;
        float dy = vy / length * WALK_SPEED;

        if (!hero_blocked(s->x + dx, s->y)) {
            s->x += dx;
        }
        if (!hero_blocked(s->x, s->y + dy)) {
            s->y += dy;
        }
        if (vx - vy < 0.0f) {
            s->facing_left = true;
        } else if (vx - vy > 0.0f) {
            s->facing_left = false;
        }
    }

    status = pixelica_anim_player_advance(&s->hop, 1u);
    if (status == PIXELICA_OK) {
        status = s->moving ? pixelica_anim_player_advance(&s->walk, 1u)
                           : pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP);
    }
    if (status != PIXELICA_OK) {
        s->error = status;
    }
}

static PIXELICA_Status project(const HelloIso *s, float x, float y, float z, PIXELICA_Vec2 *out)
{
    PIXELICA_Vec3 map;

    map.x = x;
    map.y = y;
    map.z = z;
    return pixelica2d_iso_project(&s->iso, map, out);
}

/* Keyed sprite of one atlas frame; position is rounded to whole pixels. */
static PIXELICA_Status make_sprite(PIXELICA2D_Sprite *out, const PIXELICA_Texture *texture, uint32_t frame,
                              uint16_t cell_w, uint16_t cell_h, PIXELICA_Vec2 at, float pivot_x,
                              float pivot_y)
{
    DEMO_TRY(pixelica2d_sprite_init(out, texture));
    DEMO_TRY(pixelica2d_sprite_frame(out, frame, cell_w, cell_h));
    out->position.x = floorf(at.x + 0.5f);
    out->position.y = floorf(at.y + 0.5f);
    out->pivot.x = pivot_x;
    out->pivot.y = pivot_y;
    out->color_key_enabled = true;
    out->color_key = DEMO_KEY_COLOR;
    return PIXELICA_OK;
}

static PIXELICA_Status push_item(HelloIso *s, size_t *count, const PIXELICA2D_Sprite *sprite, int32_t layer,
                            float sort_y)
{
    if (*count >= ITEM_CAPACITY) {
        return PIXELICA_ERR_CAPACITY;
    }
    s->items[*count].sprite = *sprite;
    s->items[*count].layer = layer;
    s->items[*count].sort_y = sort_y;
    *count += 1u;
    return PIXELICA_OK;
}

static PIXELICA_Status build_items(HelloIso *s, size_t *out_count, float *out_hero_sort, float *out_hop)
{
    size_t count = 0u;
    uint32_t i;
    uint32_t j;
    size_t b;
    uint32_t tick = 0u;
    uint32_t frame = 0u;
    float hop = 0.0f;
    float walk = 0.0f;
    PIXELICA_Vec2 at;
    PIXELICA_Vec2 ground;
    PIXELICA2D_Sprite sprite;

    DEMO_TRY(pixelica_anim_player_tick(&s->hop, &tick));
    DEMO_TRY(pixelica_anim_sample(k_hop_keys, 4u, tick, &hop));
    DEMO_TRY(pixelica_anim_player_tick(&s->walk, &tick));
    DEMO_TRY(pixelica_anim_sample(k_walk_keys, 4u, tick, &walk));
    frame = (uint32_t)(walk + 0.5f);

    /* Layer 0: floor diamonds. Pivot (16, 0) = the diamond's top corner. */
    for (j = 0u; j < MAP_N; ++j) {
        for (i = 0u; i < MAP_N; ++i) {
            const uint32_t variant = (i == 4u) ? 2u : ((i + j) % 2u);

            DEMO_TRY(project(s, (float)i, (float)j, 0.0f, &at));
            DEMO_TRY(project(s, (float)i + 0.5f, (float)j + 0.5f, 0.0f, &ground));
            DEMO_TRY(make_sprite(&sprite, &s->floor_tex, variant, (uint16_t)DEMO_ISO_TILE_W,
                                 (uint16_t)DEMO_ISO_TILE_H, at, 16.0f, 0.0f));
            DEMO_TRY(push_item(s, &count, &sprite, 0, ground.y));
        }
    }

    /* Layer 1: shadow on the ground under the hero (does not follow the hop). */
    DEMO_TRY(project(s, s->x, s->y, 0.0f, &ground));
    DEMO_TRY(make_sprite(&sprite, &s->shadow_tex, 0u, (uint16_t)DEMO_SHADOW_W,
                         (uint16_t)DEMO_SHADOW_H, ground, 8.0f, 3.0f));
    sprite.opacity = 110u;
    DEMO_TRY(push_item(s, &count, &sprite, 1, ground.y));

    /* Layer 2: blocks, then the hero; the list sorts them by ground y. */
    for (b = 0u; b < BLOCK_COUNT; ++b) {
        const float cx = (float)k_blocks[b].cx;
        const float cy = (float)k_blocks[b].cy;
        const float z = (float)k_blocks[b].level * BLOCK_HEIGHT;

        DEMO_TRY(project(s, cx, cy, z, &at));
        DEMO_TRY(project(s, cx + 0.5f, cy + 0.5f, 0.0f, &ground));
        DEMO_TRY(make_sprite(&sprite, &s->block_tex, k_blocks[b].variant,
                             (uint16_t)DEMO_BLOCK_SIZE, (uint16_t)DEMO_BLOCK_SIZE, at, 16.0f,
                             16.0f));
        /* A stacked block shares its footprint's sort_y; insertion order breaks the tie. */
        DEMO_TRY(push_item(s, &count, &sprite, 2, ground.y));
    }

    DEMO_TRY(project(s, s->x, s->y, hop, &at));
    DEMO_TRY(project(s, s->x, s->y, 0.0f, &ground));
    DEMO_TRY(make_sprite(&sprite, &s->hero_tex, frame, (uint16_t)DEMO_HERO_CELL_W,
                         (uint16_t)DEMO_HERO_CELL_H, at, (float)(DEMO_HERO_CELL_W / 2u),
                         (float)DEMO_HERO_CELL_H));
    sprite.scale.x = s->facing_left ? -1.0f : 1.0f;
    DEMO_TRY(push_item(s, &count, &sprite, 2, ground.y));

    *out_count = count;
    *out_hero_sort = ground.y;
    *out_hop = hop;
    return PIXELICA_OK;
}

static PIXELICA_Status draw_line(HelloIso *s, int32_t x, int32_t y, const char *text, uint16_t color)
{
    return pixelica2d_draw_text(&s->ctx, &s->font, text, strlen(text), x, y, color, 255u);
}

static PIXELICA_Status demo_render(void *state)
{
    HelloIso *s = state;
    PIXELICA2D_Rect top_panel = {0, 0, 256, 24};
    PIXELICA2D_Rect bottom_panel = {0, 180, 256, 12};
    size_t count = 0u;
    float hero_sort = 0.0f;
    float hop = 0.0f;
    char line[56];
    int written;

    if (s->error != PIXELICA_OK) {
        return s->error;
    }
    DEMO_TRY(pixelica2d_clear(&s->ctx, PIXELICA_RGB565(22, 26, 46)));
    DEMO_TRY(build_items(s, &count, &hero_sort, &hop));
    DEMO_TRY(pixelica2d_draw_list(&s->ctx, s->items, count, s->order, ITEM_CAPACITY));

    DEMO_TRY(pixelica2d_fill_rect(&s->ctx, &top_panel, PIXELICA_RGB565(8, 10, 24), 150u));
    DEMO_TRY(pixelica2d_fill_rect(&s->ctx, &bottom_panel, PIXELICA_RGB565(8, 10, 24), 150u));
    DEMO_TRY(draw_line(s, 4, 4, "PIXELICA2D ISOMETRIC  PAINTER ORDER", PIXELICA_RGB565(250, 250, 250)));
    written = snprintf(line, sizeof line, "GROUND Y %d  HEIGHT %d  ITEMS %u", (int)hero_sort,
                       (int)hop, (unsigned)count);
    if (written < 0 || (size_t)written >= sizeof line) {
        return PIXELICA_ERR_CAPACITY;
    }
    DEMO_TRY(draw_line(s, 4, 14, line, PIXELICA_RGB565(244, 200, 64)));
    DEMO_TRY(draw_line(s, 4, 183, "ARROWS MOVE  A PAUSE  START RESET", PIXELICA_RGB565(200, 210, 230)));
    if (s->paused) {
        DEMO_TRY(demo_art_hud_label(&s->ctx, &s->font, 110, 96, "PAUSED", PIXELICA_RGB565(244, 200, 64)));
    }
    return PIXELICA_OK;
}

static void demo_shutdown(void *state)
{
    (void)state;
}

const PIXELICA2D_Demo pixelica2d_example_demo = {
    "Pixelica2D hello_isometric",
    (uint16_t)PIXELICA2D_DEMO_WIDTH,
    (uint16_t)PIXELICA2D_DEMO_HEIGHT,
    demo_init,
    demo_update,
    demo_render,
    demo_shutdown,
};
