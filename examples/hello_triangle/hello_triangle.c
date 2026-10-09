/*
 * hello_triangle: a spinning flat-colored triangle in front of a static,
 * larger triangle placed farther away. The far triangle is submitted second,
 * so it only stays hidden behind the spinner because of the depth test.
 *
 * Keys: Left/Right nudge the spin, A pauses, START resets.
 */
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#include "pixelica/platform.h"
#include "pixelica/renderer.h"

#include "../common/pixelica_game.h"

#define TWO_PI 6.28318530717958647692f
#define AUTO_SPIN_RAD_PER_S 1.2f
#define MANUAL_SPIN_RAD_PER_S 2.5f

static const PIXELICA_Vec3 k_front_vertices[3] = {
    {0.0f, 0.9f, 0.0f},
    {-0.8f, -0.6f, 0.0f},
    {0.8f, -0.6f, 0.0f},
};

static const PIXELICA_Vec3 k_back_vertices[3] = {
    {0.0f, 1.6f, 0.0f},
    {-1.5f, -1.0f, 0.0f},
    {1.5f, -1.0f, 0.0f},
};

/* Counter-clockwise when viewed from +Z (the camera side). */
static const uint16_t k_indices[3] = {0u, 1u, 2u};

typedef struct TriangleState {
    float angle;
    bool paused;
    uint32_t previous_buttons;
    float view_projection[16];
} TriangleState;

static TriangleState g_state;

static bool triangle_init(void **out_state, PIXELICA_Context *ctx)
{
    (void)ctx;
    float projection[16];
    float view[16];

    memset(&g_state, 0, sizeof(g_state));
    if (pixelica_mat4_perspective(projection, 1.0471976f, 256.0f / 192.0f, 0.1f, 50.0f) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(view, 0.0f, 0.0f, -3.0f);
    pixelica_mat4_multiply(g_state.view_projection, projection, view);
    *out_state = &g_state;
    return true;
}

static void triangle_update(void *userdata, float dt, uint32_t buttons)
{
    TriangleState *state = userdata;
    uint32_t pressed = buttons & ~state->previous_buttons;
    state->previous_buttons = buttons;

    if ((pressed & PIXELICA_BTN_A) != 0u) {
        state->paused = !state->paused;
    }
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        state->angle = 0.0f;
        state->paused = false;
    }
    if (!state->paused) {
        state->angle += AUTO_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        state->angle += MANUAL_SPIN_RAD_PER_S * dt;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        state->angle -= MANUAL_SPIN_RAD_PER_S * dt;
    }
    while (state->angle >= TWO_PI) {
        state->angle -= TWO_PI;
    }
    while (state->angle < 0.0f) {
        state->angle += TWO_PI;
    }
}

static PIXELICA_Status triangle_render(void *userdata, PIXELICA_Context *ctx)
{
    TriangleState *state = userdata;
    float spin[16];
    float back_model[16];
    PIXELICA_Status status;

    PIXELICA_Mesh front = {k_front_vertices, 3u, k_indices, 3u, PIXELICA_EXAMPLE_RGB565(255, 190, 40)};
    PIXELICA_Mesh back = {k_back_vertices, 3u, k_indices, 3u, PIXELICA_EXAMPLE_RGB565(60, 110, 220)};

    pixelica_mat4_rotation_z(spin, state->angle);
    pixelica_mat4_translation(back_model, 0.0f, 0.0f, -1.5f);

    pixelica_begin_frame(ctx, PIXELICA_EXAMPLE_RGB565(24, 28, 40));
    pixelica_set_view_projection(ctx, state->view_projection);

    status = pixelica_draw_mesh(ctx, &front, spin);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_draw_mesh(ctx, &back, back_model);
    if (status != PIXELICA_OK) {
        return status;
    }
    return pixelica_end_frame(ctx);
}

static void triangle_shutdown(void *userdata)
{
    (void)userdata;
}

const PIXELICA_Game pixelica_example_game = {
    "Pixelica hello_triangle",
    {256u, 192u, 16u, 64u, 65536u},
    triangle_init,
    triangle_update,
    triangle_render,
    triangle_shutdown,
};
