/*
 * hello_cube: a rotating cube with six flat-colored faces. The core has no
 * lighting in G0, so each face is its own mesh with its own color; all six
 * share one vertex array and have outward-facing counter-clockwise winding.
 *
 * Rotation advances in the fixed update. Keys: arrows rotate, A pauses the
 * automatic spin, START resets.
 */
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#include "pixelica/platform.h"
#include "pixelica/renderer.h"

#include "../common/pixelica_game.h"

#define TWO_PI 6.28318530717958647692f
#define AUTO_YAW_RAD_PER_S 0.9f
#define AUTO_PITCH_RAD_PER_S 0.5f
#define MANUAL_RAD_PER_S 2.0f
#define HALF 0.8f

#define FACE_COUNT 6u

/* Vertex index bits: x = bit 0, y = bit 1, z = bit 2 (set means positive). */
static const PIXELICA_Vec3 k_vertices[8] = {
    {-HALF, -HALF, -HALF}, {HALF, -HALF, -HALF}, {-HALF, HALF, -HALF}, {HALF, HALF, -HALF},
    {-HALF, -HALF, HALF},  {HALF, -HALF, HALF},  {-HALF, HALF, HALF},  {HALF, HALF, HALF},
};

/* Two triangles per face, counter-clockwise seen from outside the cube. */
static const uint16_t k_face_indices[FACE_COUNT][6] = {
    {4, 5, 7, 4, 7, 6}, /* +Z */
    {1, 0, 2, 1, 2, 3}, /* -Z */
    {5, 1, 3, 5, 3, 7}, /* +X */
    {0, 4, 6, 0, 6, 2}, /* -X */
    {6, 7, 3, 6, 3, 2}, /* +Y */
    {0, 1, 5, 0, 5, 4}, /* -Y */
};

typedef struct CubeState {
    float yaw;
    float pitch;
    bool paused;
    uint32_t previous_buttons;
    uint16_t face_colors[FACE_COUNT];
    float view_projection[16];
} CubeState;

static CubeState g_state;

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

static bool cube_init(void **out_state, PIXELICA_Context *ctx)
{
    (void)ctx;
    float projection[16];
    float view[16];

    memset(&g_state, 0, sizeof(g_state));
    if (pixelica_mat4_perspective(projection, 0.87266463f, 256.0f / 192.0f, 0.1f, 50.0f) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(view, 0.0f, 0.0f, -4.0f);
    pixelica_mat4_multiply(g_state.view_projection, projection, view);

    g_state.face_colors[0] = PIXELICA_EXAMPLE_RGB565(220, 60, 60);   /* +Z red */
    g_state.face_colors[1] = PIXELICA_EXAMPLE_RGB565(60, 200, 210);  /* -Z cyan */
    g_state.face_colors[2] = PIXELICA_EXAMPLE_RGB565(70, 200, 90);   /* +X green */
    g_state.face_colors[3] = PIXELICA_EXAMPLE_RGB565(200, 80, 200);  /* -X magenta */
    g_state.face_colors[4] = PIXELICA_EXAMPLE_RGB565(70, 100, 230);  /* +Y blue */
    g_state.face_colors[5] = PIXELICA_EXAMPLE_RGB565(235, 210, 70);  /* -Y yellow */

    *out_state = &g_state;
    return true;
}

static void cube_update(void *userdata, float dt, uint32_t buttons)
{
    CubeState *state = userdata;
    uint32_t pressed = buttons & ~state->previous_buttons;
    state->previous_buttons = buttons;

    if ((pressed & PIXELICA_BTN_A) != 0u) {
        state->paused = !state->paused;
    }
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        state->yaw = 0.0f;
        state->pitch = 0.0f;
        state->paused = false;
    }
    if (!state->paused) {
        state->yaw += AUTO_YAW_RAD_PER_S * dt;
        state->pitch += AUTO_PITCH_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        state->yaw += MANUAL_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        state->yaw -= MANUAL_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        state->pitch += MANUAL_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_DOWN) != 0u) {
        state->pitch -= MANUAL_RAD_PER_S * dt;
    }
    state->yaw = wrap_angle(state->yaw);
    state->pitch = wrap_angle(state->pitch);
}

static PIXELICA_Status cube_render(void *userdata, PIXELICA_Context *ctx)
{
    CubeState *state = userdata;
    float rotation_x[16];
    float rotation_y[16];
    float model[16];

    pixelica_mat4_rotation_x(rotation_x, state->pitch);
    pixelica_mat4_rotation_y(rotation_y, state->yaw);
    pixelica_mat4_multiply(model, rotation_y, rotation_x);

    pixelica_begin_frame(ctx, PIXELICA_EXAMPLE_RGB565(24, 28, 40));
    pixelica_set_view_projection(ctx, state->view_projection);

    for (uint32_t face = 0; face < FACE_COUNT; ++face) {
        PIXELICA_Mesh mesh = {k_vertices, 8u, k_face_indices[face], 6u, state->face_colors[face]};
        PIXELICA_Status status = pixelica_draw_mesh(ctx, &mesh, model);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return pixelica_end_frame(ctx);
}

static void cube_shutdown(void *userdata)
{
    (void)userdata;
}

const PIXELICA_Game pixelica_example_game = {
    "Pixelica hello_cube",
    {256u, 192u, 16u, 64u, 65536u},
    cube_init,
    cube_update,
    cube_render,
    cube_shutdown,
};
