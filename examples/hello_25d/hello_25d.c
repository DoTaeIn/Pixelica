/*
 * hello_25d: 2.5D with the hybrid module - real 3D geometry, sprites that live
 * in the same depth buffer, and a 2D HUD.
 *
 *   pixelica_begin_frame
 *     3D meshes : checkered floor, back wall, a low red box (depth written)
 *     pixelica2d_bind_3d            2D view over the SAME color + depth buffers
 *     pixelica25d_draw_billboard    hero (opaque, color-keyed) and a translucent
 *                              "spirit" orbiting the box: both are depth
 *                              TESTED, so the box hides them when they are
 *                              behind it and they cover it when in front
 *     pixelica2d_fill_rect/_text    HUD, screen space, depth ignored
 *   pixelica_end_frame              the only present
 *
 * Camera: orthographic (pixelica_mat4_orthographic), 16 pixels per world unit,
 * looking down 40 degrees. 3D is +Y UP; the sprites' screen space is +Y DOWN
 * and is produced only by pixelica25d_project inside the billboard call.
 *
 * Billboards are anchored at the sprite's CENTRE (pivot 8,12 at world height
 * 0.75): a vertical sprite has one depth, and a camera that looks down makes
 * higher points nearer, so the middle of the body is the fairest single depth.
 *
 * Keys: arrows move on the floor (UP = away from the camera), A pauses,
 * START resets.
 *
 * Evidence aid: if the environment variable PIXELICA25D_CAPTURE_PPM names a
 * file, every finished frame is also written there as a binary PPM (the file
 * is overwritten, so after --frames N it holds the last frame).
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv/fopen for the optional capture only */
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../common/pixelica_game.h"
#include "../common2d/demo_art.h"
#include "pixelica/hybrid.h"

#define FLOOR_COLS 14u
#define FLOOR_ROWS 12u
#define FLOOR_VERTS ((FLOOR_COLS + 1u) * (FLOOR_ROWS + 1u))
#define FLOOR_TRIS (FLOOR_COLS * FLOOR_ROWS * 2u)
#define WALL_COLS 14u
#define WALL_ROWS 5u
#define WALL_VERTS ((WALL_COLS + 1u) * (WALL_ROWS + 1u))
#define WALL_TRIS (WALL_COLS * WALL_ROWS * 2u)
#define BOX_TRIS 8u
#define MAX_VERTS 256u
#define VIEW_W 256u
#define VIEW_H 192u

#define EYE_DISTANCE 20.0f
#define PITCH_RAD 0.69813170f /* 40 degrees */
#define WALK_SPEED 0.06f      /* world units per fixed step */
#define WALK_CYCLE_TICKS 32u
#define SPRITE_CENTER_Y 0.75f /* world height of the sprite centre */

#define BOX_X0 1.0f
#define BOX_X1 3.0f
#define BOX_Z0 1.0f
#define BOX_Z1 2.0f
#define BOX_H 1.2f

typedef struct Hello25D {
    PIXELICA_Texture hero_tex;
    PIXELICA2D_Font font;
    uint16_t hero_pixels[DEMO_HERO_PIXELS];
    float view_projection[16];

    PIXELICA_Vec3 floor_vertices[FLOOR_VERTS];
    uint16_t floor_indices[FLOOR_TRIS * 3u];
    uint16_t floor_colors[FLOOR_TRIS];
    PIXELICA_Vec3 wall_vertices[WALL_VERTS];
    uint16_t wall_indices[WALL_TRIS * 3u];
    uint16_t wall_colors[WALL_TRIS];
    PIXELICA_Vec3 box_vertices[8];
    uint16_t box_indices[BOX_TRIS * 3u];
    uint16_t box_colors[BOX_TRIS];

    PIXELICA_AnimPlayer walk;
    PIXELICA_Status error;
    const char *capture_path; /* PIXELICA25D_CAPTURE_PPM, or NULL */
    uint32_t previous_buttons;
    uint32_t tick;
    float x, z; /* hero feet on the floor, world units */
    bool moving, paused;
} Hello25D;

static Hello25D g_state;

static const PIXELICA_AnimKeyframe k_walk_keys[4] = {
    {0u, 0.0f, PIXELICA_EASE_STEP},
    {8u, 1.0f, PIXELICA_EASE_STEP},
    {16u, 2.0f, PIXELICA_EASE_STEP},
    {24u, 3.0f, PIXELICA_EASE_STEP},
};

/* Binary PPM (P6) of an RGB565 frame whose stride equals its width. */
static bool write_ppm(const char *path, const uint16_t *pixels, uint16_t width, uint16_t height)
{
    uint8_t row[256u * 3u];
    FILE *file;
    size_t x;
    size_t y;
    bool ok;

    if (path == NULL || pixels == NULL || width == 0u || width > 256u || height == 0u) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    ok = fprintf(file, "P6\n%u %u\n255\n", (unsigned)width, (unsigned)height) > 0;
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
        ok = fwrite(row, 1u, (size_t)width * 3u, file) == (size_t)width * 3u;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

static void set_tri(uint16_t *indices, size_t tri, uint32_t a, uint32_t b, uint32_t c)
{
    indices[tri * 3u + 0u] = (uint16_t)a;
    indices[tri * 3u + 1u] = (uint16_t)b;
    indices[tri * 3u + 2u] = (uint16_t)c;
}

static void build_floor(Hello25D *s)
{
    const uint16_t light = PIXELICA_EXAMPLE_RGB565(176, 146, 104);
    const uint16_t dark = PIXELICA_EXAMPLE_RGB565(140, 112, 80);
    const uint16_t lane = PIXELICA_EXAMPLE_RGB565(112, 144, 168);
    uint32_t ix;
    uint32_t iz;

    for (iz = 0u; iz <= FLOOR_ROWS; ++iz) {
        for (ix = 0u; ix <= FLOOR_COLS; ++ix) {
            PIXELICA_Vec3 *v = &s->floor_vertices[iz * (FLOOR_COLS + 1u) + ix];

            v->x = -7.0f + (float)ix;
            v->y = 0.0f;
            v->z = -6.0f + (float)iz;
        }
    }
    for (iz = 0u; iz < FLOOR_ROWS; ++iz) {
        for (ix = 0u; ix < FLOOR_COLS; ++ix) {
            const uint32_t a = iz * (FLOOR_COLS + 1u) + ix;       /* x0, z0 */
            const uint32_t b = a + 1u;                            /* x1, z0 */
            const uint32_t d = a + (FLOOR_COLS + 1u);             /* x0, z1 */
            const uint32_t c = d + 1u;                            /* x1, z1 */
            const size_t tri = (size_t)(iz * FLOOR_COLS + ix) * 2u;
            const uint16_t color = (ix == 6u || ix == 7u) ? lane : (((ix + iz) & 1u) != 0u ? dark : light);

            set_tri(s->floor_indices, tri, d, c, b); /* counter-clockwise seen from +Y */
            set_tri(s->floor_indices, tri + 1u, d, b, a);
            s->floor_colors[tri] = color;
            s->floor_colors[tri + 1u] = color;
        }
    }
}

static void build_wall(Hello25D *s)
{
    const uint16_t stone_a = PIXELICA_EXAMPLE_RGB565(120, 124, 140);
    const uint16_t stone_b = PIXELICA_EXAMPLE_RGB565(96, 100, 118);
    const uint16_t cap = PIXELICA_EXAMPLE_RGB565(170, 174, 190);
    uint32_t ix;
    uint32_t iy;

    for (iy = 0u; iy <= WALL_ROWS; ++iy) {
        for (ix = 0u; ix <= WALL_COLS; ++ix) {
            PIXELICA_Vec3 *v = &s->wall_vertices[iy * (WALL_COLS + 1u) + ix];

            v->x = -7.0f + (float)ix;
            v->y = 0.5f * (float)iy;
            v->z = -6.0f;
        }
    }
    for (iy = 0u; iy < WALL_ROWS; ++iy) {
        for (ix = 0u; ix < WALL_COLS; ++ix) {
            const uint32_t a = iy * (WALL_COLS + 1u) + ix; /* bottom-left */
            const uint32_t b = a + 1u;
            const uint32_t d = a + (WALL_COLS + 1u);
            const uint32_t c = d + 1u;
            const size_t tri = (size_t)(iy * WALL_COLS + ix) * 2u;
            const uint16_t color =
                (iy == WALL_ROWS - 1u) ? cap : ((((ix + iy) & 1u) != 0u) ? stone_b : stone_a);

            set_tri(s->wall_indices, tri, a, b, c); /* counter-clockwise seen from +Z */
            set_tri(s->wall_indices, tri + 1u, a, c, d);
            s->wall_colors[tri] = color;
            s->wall_colors[tri + 1u] = color;
        }
    }
}

static void build_box(Hello25D *s)
{
    const uint16_t front = PIXELICA_EXAMPLE_RGB565(196, 70, 62);
    const uint16_t top = PIXELICA_EXAMPLE_RGB565(232, 156, 112);
    const uint16_t side = PIXELICA_EXAMPLE_RGB565(150, 52, 48);
    uint32_t i;

    for (i = 0u; i < 8u; ++i) {
        s->box_vertices[i].x = (i & 1u) != 0u ? BOX_X1 : BOX_X0;
        s->box_vertices[i].y = (i & 2u) != 0u ? BOX_H : 0.0f;
        s->box_vertices[i].z = (i & 4u) != 0u ? BOX_Z1 : BOX_Z0;
    }
    set_tri(s->box_indices, 0u, 4u, 5u, 7u); /* +Z front */
    set_tri(s->box_indices, 1u, 4u, 7u, 6u);
    set_tri(s->box_indices, 2u, 6u, 7u, 3u); /* +Y top */
    set_tri(s->box_indices, 3u, 6u, 3u, 2u);
    set_tri(s->box_indices, 4u, 5u, 1u, 3u); /* +X */
    set_tri(s->box_indices, 5u, 5u, 3u, 7u);
    set_tri(s->box_indices, 6u, 0u, 4u, 6u); /* -X */
    set_tri(s->box_indices, 7u, 0u, 6u, 2u);
    for (i = 0u; i < 2u; ++i) {
        s->box_colors[i] = front;
        s->box_colors[2u + i] = top;
        s->box_colors[4u + i] = side;
        s->box_colors[6u + i] = side;
    }
}

static PIXELICA_Status reset_world(Hello25D *s)
{
    s->x = -0.5f;
    s->z = 0.2f; /* just behind the box's row, so the first frames show occlusion */
    s->moving = false;
    s->paused = false;
    s->tick = 0u;
    s->error = PIXELICA_OK;
    return pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP);
}

static bool hello_init(void **out_state, PIXELICA_Context *ctx)
{
    Hello25D *s = &g_state;
    float projection[16];
    float pitch[16];
    float translation[16];
    float view[16];

    if (out_state == NULL || ctx == NULL) {
        return false;
    }
    *out_state = NULL;
    memset(s, 0, sizeof *s);
    if (FLOOR_VERTS > MAX_VERTS || WALL_VERTS > MAX_VERTS ||
        !demo_art_hero_build(s->hero_pixels, DEMO_HERO_PIXELS)) {
        return false;
    }
    s->hero_tex.pixels = s->hero_pixels;
    s->hero_tex.width = (uint16_t)DEMO_HERO_ATLAS_W;
    s->hero_tex.height = (uint16_t)DEMO_HERO_ATLAS_H;
    s->font = demo_art_font();

    /* clip = Ortho * Rx(pitch) * T(-eye): the camera sits above and in front of the origin. */
    if (pixelica_mat4_orthographic(projection, -8.0f, 8.0f, -6.0f, 6.0f, 1.0f, 40.0f) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_rotation_x(pitch, PITCH_RAD);
    pixelica_mat4_translation(translation, 0.0f, -EYE_DISTANCE * sinf(PITCH_RAD),
                         -EYE_DISTANCE * cosf(PITCH_RAD));
    pixelica_mat4_multiply(view, pitch, translation);
    pixelica_mat4_multiply(s->view_projection, projection, view);

    build_floor(s);
    build_wall(s);
    build_box(s);
    pixelica_set_cull_mode(ctx, PIXELICA_CULL_NONE);
    s->capture_path = getenv("PIXELICA25D_CAPTURE_PPM");
    if (s->capture_path != NULL && s->capture_path[0] == '\0') {
        s->capture_path = NULL;
    }
    if (reset_world(s) != PIXELICA_OK) {
        return false;
    }
    *out_state = s;
    return true;
}

static bool hero_blocked(float x, float z)
{
    if (x < -6.5f || x > 6.5f || z < -5.5f || z > 5.5f) {
        return true;
    }
    return x > BOX_X0 - 0.3f && x < BOX_X1 + 0.3f && z > BOX_Z0 - 0.3f && z < BOX_Z1 + 0.3f;
}

static void hello_update(void *state, float dt, uint32_t buttons)
{
    Hello25D *s = state;
    const uint32_t pressed = buttons & ~s->previous_buttons;
    float dx = 0.0f;
    float dz = 0.0f;
    PIXELICA_Status status = PIXELICA_OK;

    (void)dt;
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
        dz -= WALK_SPEED;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        dz += WALK_SPEED;
    }
    if ((buttons & (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT)) != 0u &&
        (buttons & (PIXELICA_BTN_UP | PIXELICA_BTN_DOWN)) != 0u) {
        dx *= 0.70710678f;
        dz *= 0.70710678f;
    }
    if ((buttons & (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT)) != 0u && !hero_blocked(s->x + dx, s->z)) {
        s->x += dx;
    }
    if ((buttons & (PIXELICA_BTN_UP | PIXELICA_BTN_DOWN)) != 0u && !hero_blocked(s->x, s->z + dz)) {
        s->z += dz;
    }
    s->moving = (buttons & (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT | PIXELICA_BTN_UP | PIXELICA_BTN_DOWN)) != 0u;

    if (s->moving) {
        status = pixelica_anim_player_advance(&s->walk, 1u);
    } else {
        status = pixelica_anim_player_init(&s->walk, WALK_CYCLE_TICKS, PIXELICA_ANIM_LOOP);
    }
    if (status != PIXELICA_OK) {
        s->error = status;
    }
}

static PIXELICA_Status draw_colored_mesh(PIXELICA_Context *ctx, const PIXELICA_Vec3 *vertices,
                                    uint32_t vertex_count, const uint16_t *indices,
                                    uint32_t index_count, const uint16_t *triangle_colors)
{
    PIXELICA_Mesh mesh;
    PIXELICA_Material material;
    float model[16];

    mesh.vertices = vertices;
    mesh.vertex_count = vertex_count;
    mesh.indices = indices;
    mesh.index_count = index_count;
    mesh.color = PIXELICA_EXAMPLE_RGB565(255, 0, 255); /* unused: triangle colors win */
    material.uvs = NULL;
    material.triangle_colors = triangle_colors;
    material.texture = NULL;
    pixelica_mat4_identity(model);
    return pixelica_draw_mesh_ex(ctx, &mesh, model, &material);
}

static PIXELICA_Status draw_billboard(Hello25D *s, PIXELICA2D_Context *canvas, uint32_t frame, float x,
                                 float y, float z, uint8_t opacity, bool depth_write)
{
    PIXELICA2D_Sprite sprite;
    PIXELICA_Vec3 position;
    PIXELICA_Vec2 size;

    DEMO_TRY(pixelica2d_sprite_init(&sprite, &s->hero_tex));
    DEMO_TRY(pixelica2d_sprite_frame(&sprite, frame, (uint16_t)DEMO_HERO_CELL_W,
                                (uint16_t)DEMO_HERO_CELL_H));
    sprite.pivot.x = (float)(DEMO_HERO_CELL_W / 2u);
    sprite.pivot.y = (float)(DEMO_HERO_CELL_H / 2u);
    sprite.opacity = opacity;
    sprite.color_key_enabled = true;
    sprite.color_key = DEMO_KEY_COLOR;
    sprite.depth_write = depth_write;
    position.x = x;
    position.y = y;
    position.z = z;
    size.x = 1.0f; /* 16 x 24 pixels at 16 pixels per world unit */
    size.y = 1.5f;
    return pixelica25d_draw_billboard(canvas, s->view_projection, &sprite, position, size);
}

static PIXELICA_Status draw_line(Hello25D *s, PIXELICA2D_Context *canvas, int32_t x, int32_t y,
                            const char *text, uint16_t color)
{
    return pixelica2d_draw_text(canvas, &s->font, text, strlen(text), x, y, color, 255u);
}

static PIXELICA_Status hello_render(void *state, PIXELICA_Context *ctx)
{
    Hello25D *s = state;
    PIXELICA2D_Context canvas;
    PIXELICA2D_Rect top_panel = {0, 0, 256, 24};
    PIXELICA2D_Rect bottom_panel = {0, 180, 256, 12};
    uint32_t tick = 0u;
    uint32_t hero_frame;
    float value = 0.0f;
    float angle;
    char line[64];
    int written;
    bool behind;
    PIXELICA_Status status;

    if (s->error != PIXELICA_OK) {
        return s->error;
    }
    DEMO_TRY(pixelica_anim_player_tick(&s->walk, &tick));
    DEMO_TRY(pixelica_anim_sample(k_walk_keys, 4u, tick, &value));
    hero_frame = (uint32_t)(value + 0.5f);
    angle = (float)(s->tick % 252u) * 0.0249333f; /* one orbit per 252 steps */

    pixelica_begin_frame(ctx, PIXELICA_EXAMPLE_RGB565(24, 28, 44));
    pixelica_set_view_projection(ctx, s->view_projection);
    DEMO_TRY(draw_colored_mesh(ctx, s->floor_vertices, FLOOR_VERTS, s->floor_indices,
                               FLOOR_TRIS * 3u, s->floor_colors));
    DEMO_TRY(draw_colored_mesh(ctx, s->wall_vertices, WALL_VERTS, s->wall_indices, WALL_TRIS * 3u,
                               s->wall_colors));
    DEMO_TRY(draw_colored_mesh(ctx, s->box_vertices, 8u, s->box_indices, BOX_TRIS * 3u,
                               s->box_colors));

    /* From here on the 2D engine draws into the 3D color + depth buffers. */
    DEMO_TRY(pixelica2d_bind_3d(&canvas, ctx));
    DEMO_TRY(draw_billboard(s, &canvas, hero_frame, s->x, SPRITE_CENTER_Y, s->z, 255u, true));
    /* Translucent spirit circling the box: passes behind it and then in front. */
    DEMO_TRY(draw_billboard(s, &canvas, (s->tick / 8u) % DEMO_HERO_FRAMES,
                            2.0f + 2.4f * cosf(angle), 0.9f + 0.2f * sinf(2.0f * angle),
                            1.5f + 2.2f * sinf(angle), 140u, false));

    behind = s->x > BOX_X0 - 0.5f && s->x < BOX_X1 + 0.5f && s->z < BOX_Z0;
    DEMO_TRY(pixelica2d_fill_rect(&canvas, &top_panel, PIXELICA_EXAMPLE_RGB565(8, 10, 24), 150u));
    DEMO_TRY(pixelica2d_fill_rect(&canvas, &bottom_panel, PIXELICA_EXAMPLE_RGB565(8, 10, 24), 150u));
    DEMO_TRY(draw_line(s, &canvas, 4, 4, "PIXELICA 2.5D  MESH + DEPTH SPRITES",
                       PIXELICA_EXAMPLE_RGB565(250, 250, 250)));
    written = snprintf(line, sizeof line, "%s  3D DRAWS %u  2D DRAWS %u",
                       behind ? "HERO BEHIND BOX" : "HERO IN FRONT",
                       (unsigned)pixelica_frame_stats(ctx).draw_calls, (unsigned)canvas.stats.draw_calls);
    if (written < 0 || (size_t)written >= sizeof line) {
        return PIXELICA_ERR_CAPACITY;
    }
    DEMO_TRY(draw_line(s, &canvas, 4, 14, line, PIXELICA_EXAMPLE_RGB565(244, 200, 64)));
    DEMO_TRY(draw_line(s, &canvas, 4, 183, "ARROWS MOVE  A PAUSE  START RESET",
                       PIXELICA_EXAMPLE_RGB565(200, 210, 230)));
    if (s->paused) {
        DEMO_TRY(demo_art_hud_label(&canvas, &s->font, 110, 96, "PAUSED",
                                    PIXELICA_EXAMPLE_RGB565(244, 200, 64)));
    }
    status = pixelica_end_frame(ctx);
    if (status == PIXELICA_OK && s->capture_path != NULL &&
        !write_ppm(s->capture_path, pixelica_color_buffer(ctx), (uint16_t)VIEW_W, (uint16_t)VIEW_H)) {
        return PIXELICA_ERR_PLATFORM;
    }
    return status;
}

static void hello_shutdown(void *state)
{
    (void)state;
}

const PIXELICA_Game pixelica_example_game = {
    "Pixelica hello_25d",
    {VIEW_W, VIEW_H, 8u, MAX_VERTS, 65536u},
    hello_init,
    hello_update,
    hello_render,
    hello_shutdown,
};
