#include "game.h"

#include <string.h>

#define TWO_PI 6.28318530717958647692f
#define AUTO_SPIN_RAD_PER_S 0.9f
#define MANUAL_SPIN_RAD_PER_S 2.5f
#define MAX_VERTICES_PER_MESH 64u
#define MAX_DRAWS_PER_FRAME 16u

/*
 * One quad per cube face, vertices counter-clockwise seen from outside, so
 * the default back-face culling keeps the visible sides. There is no lighting
 * yet: a distinct flat color per face is what makes the edges visible.
 */
static const PIXELICA_Vec3 k_face_vertices[6][4] = {
    {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},     /* +Z */
    {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}}, /* -Z */
    {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},     /* +X */
    {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}}, /* -X */
    {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},     /* +Y */
    {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}}, /* -Y */
};

static const uint16_t k_face_indices[6] = {0u, 1u, 2u, 0u, 2u, 3u};

static const uint16_t k_face_colors[6] = {
    PIXELICA_RGB565(220, 70, 60),  /* +Z red */
    PIXELICA_RGB565(70, 160, 220), /* -Z blue */
    PIXELICA_RGB565(90, 200, 100), /* +X green */
    PIXELICA_RGB565(240, 200, 60), /* -X yellow */
    PIXELICA_RGB565(200, 200, 210),/* +Y light grey */
    PIXELICA_RGB565(150, 90, 200), /* -Y purple */
};

/* Floor quad, counter-clockwise seen from above (+Y). (u, v) = (0, 0) is the
 * top-left texel; the far edge (-Z) maps to the top row. */
#define FLOOR_Y (-1.0f)
#define FLOOR_HALF 2.5f
static const PIXELICA_Vec3 k_floor_vertices[4] = {
    {-FLOOR_HALF, FLOOR_Y, FLOOR_HALF},
    {FLOOR_HALF, FLOOR_Y, FLOOR_HALF},
    {FLOOR_HALF, FLOOR_Y, -FLOOR_HALF},
    {-FLOOR_HALF, FLOOR_Y, -FLOOR_HALF},
};
static const PIXELICA_Vec2 k_floor_uvs[4] = {
    {0.0f, 1.0f},
    {1.0f, 1.0f},
    {1.0f, 0.0f},
    {0.0f, 0.0f},
};

PIXELICA_Config template_game_config(void)
{
    PIXELICA_Config config = {
        .width = (uint16_t)TEMPLATE_RENDER_WIDTH,
        .height = (uint16_t)TEMPLATE_RENDER_HEIGHT,
        .max_draw_commands = (uint16_t)MAX_DRAWS_PER_FRAME,
        .max_vertices_per_mesh = (uint16_t)MAX_VERTICES_PER_MESH,
        .frame_arena_bytes = 65536u,
    };
    /* The engine states the minimum; keep the generous default above unless it is smaller. */
    uint32_t minimum = (uint32_t)MAX_VERTICES_PER_MESH * (uint32_t)PIXELICA_ARENA_BYTES_PER_VERTEX;
    if (config.frame_arena_bytes < minimum) {
        config.frame_arena_bytes = minimum;
    }
    return config;
}

bool template_game_init(TemplateGame *game)
{
    float projection[16];
    float view[16];

    if (game == NULL) {
        return false;
    }
    memset(game, 0, sizeof(*game));
    if (pixelica_mat4_perspective(projection, 1.0471976f,
                             (float)TEMPLATE_RENDER_WIDTH / (float)TEMPLATE_RENDER_HEIGHT,
                             0.1f, 50.0f) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(view, 0.0f, -0.6f, -3.6f);
    pixelica_mat4_multiply(game->view_projection, projection, view);

    /* Procedural 8x8 checker floor texture: no asset files needed for the starter. */
    const uint16_t light = PIXELICA_RGB565(200, 170, 120);
    const uint16_t dark = PIXELICA_RGB565(120, 90, 60);
    for (unsigned y = 0u; y < TEMPLATE_CHECKER_SIZE; ++y) {
        for (unsigned x = 0u; x < TEMPLATE_CHECKER_SIZE; ++x) {
            uint16_t texel = light;
            if ((((x >> 1) + (y >> 1)) & 1u) != 0u) {
                texel = dark;
            }
            game->checker_texels[y * TEMPLATE_CHECKER_SIZE + x] = texel;
        }
    }
    game->floor_texture.pixels = game->checker_texels;
    game->floor_texture.width = (uint16_t)TEMPLATE_CHECKER_SIZE;
    game->floor_texture.height = (uint16_t)TEMPLATE_CHECKER_SIZE;
    game->floor_material.uvs = k_floor_uvs;
    game->floor_material.triangle_colors = NULL;
    game->floor_material.texture = &game->floor_texture;
    return true;
}

static float wrap_angle(float angle)
{
    while (angle >= TWO_PI) {
        angle -= TWO_PI;
    }
    while (angle < 0.0f) {
        angle += TWO_PI;
    }
    return angle;
}

void template_game_update(void *userdata, float dt, uint32_t buttons)
{
    TemplateGame *game = userdata;
    uint32_t pressed = buttons & ~game->previous_buttons;
    game->previous_buttons = buttons;

    if ((pressed & PIXELICA_BTN_A) != 0u) {
        game->paused = !game->paused;
    }
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        game->yaw = 0.0f;
        game->pitch = 0.0f;
        game->paused = false;
    }
    if (!game->paused) {
        game->yaw += AUTO_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        game->yaw += MANUAL_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        game->yaw -= MANUAL_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        game->pitch += MANUAL_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        game->pitch -= MANUAL_SPIN_RAD_PER_S * dt;
    }
    game->yaw = wrap_angle(game->yaw);
    game->pitch = wrap_angle(game->pitch);
}

PIXELICA_Status template_game_render(const TemplateGame *game, PIXELICA_Context *ctx)
{
    float rotation_y[16];
    float rotation_x[16];
    float model[16];

    pixelica_mat4_rotation_y(rotation_y, game->yaw);
    pixelica_mat4_rotation_x(rotation_x, game->pitch);
    pixelica_mat4_multiply(model, rotation_y, rotation_x);

    pixelica_begin_frame(ctx, PIXELICA_RGB565(24, 28, 40));
    pixelica_set_view_projection(ctx, game->view_projection);

    float identity[16];
    pixelica_mat4_identity(identity);
    PIXELICA_Mesh floor_mesh;
    memset(&floor_mesh, 0, sizeof(floor_mesh));
    floor_mesh.vertices = k_floor_vertices;
    floor_mesh.vertex_count = 4u;
    floor_mesh.indices = k_face_indices;
    floor_mesh.index_count = 6u;
    floor_mesh.color = PIXELICA_RGB565(255, 0, 255); /* unused: the texture wins */
    PIXELICA_Status floor_status = pixelica_draw_mesh_ex(ctx, &floor_mesh, identity, &game->floor_material);
    if (floor_status != PIXELICA_OK) {
        return floor_status;
    }

    for (unsigned face = 0u; face < 6u; ++face) {
        PIXELICA_Mesh mesh;
        memset(&mesh, 0, sizeof(mesh));
        mesh.vertices = k_face_vertices[face];
        mesh.vertex_count = 4u;
        mesh.indices = k_face_indices;
        mesh.index_count = 6u;
        mesh.color = k_face_colors[face];
        PIXELICA_Status status = pixelica_draw_mesh(ctx, &mesh, model);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return pixelica_end_frame(ctx);
}
