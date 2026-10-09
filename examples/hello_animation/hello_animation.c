/*
 * hello_animation: interactive tour of the animation module.
 *
 *   - A robot built from scaled unit boxes. The whole robot sways (keyframed
 *     yaw on its root); its right arm is a CHILD of the root and waves around
 *     a shoulder pivot (keyframed angle, PIXELICA_Transform.pivot), so the arm world
 *     matrix is parent_world * arm_local.
 *   - A separate cube slides and bounces on keyframe tracks and pulses/turns
 *     with pixelica_anim_tween() driven by the normalized position of a PING_PONG
 *     player.
 *   - Two deterministic players advance exactly one tick per fixed update.
 *
 * Keys: A pauses/resumes both players (the floor changes tint while paused),
 * START restarts everything from tick 0. Pressed edges are detected from the
 * button bitmask, like hello_cube.
 *
 * The animation calls allocate nothing and every status is propagated: update
 * stores the first failure and render returns it instead of drawing stale data.
 */
#include "pixelica/animation.h"
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#include "pixelica/platform.h"
#include "pixelica/renderer.h"

#include "../common/pixelica_game.h"

#define FACE_COUNT 6u

#define BODY_LOOP_TICKS 180u /* arm wave + sway cycle: 3 s at 60 Hz */
#define CUBE_SWEEP_TICKS 120u /* one-way cube trip: 2 s at 60 Hz */

/* Unit box: vertex index bits x = bit 0, y = bit 1, z = bit 2 (set = positive). */
#define HALF 0.5f
static const PIXELICA_Vec3 k_box_vertices[8] = {
    {-HALF, -HALF, -HALF}, {HALF, -HALF, -HALF}, {-HALF, HALF, -HALF}, {HALF, HALF, -HALF},
    {-HALF, -HALF, HALF},  {HALF, -HALF, HALF},  {-HALF, HALF, HALF},  {HALF, HALF, HALF},
};

/* Two triangles per face, counter-clockwise seen from outside the box. */
static const uint16_t k_box_faces[FACE_COUNT][6] = {
    {4, 5, 7, 4, 7, 6}, /* +Z */
    {1, 0, 2, 1, 2, 3}, /* -Z */
    {5, 1, 3, 5, 3, 7}, /* +X */
    {0, 4, 6, 0, 6, 2}, /* -X */
    {6, 7, 3, 6, 3, 2}, /* +Y */
    {0, 1, 5, 0, 5, 4}, /* -Y */
};

/* Brightness per face stands in for lighting (the core has none). */
static const float k_face_shade[FACE_COUNT] = {1.00f, 0.50f, 0.78f, 0.64f, 0.90f, 0.42f};

/* Robot sway (yaw of the root, radians). Last key == first key: seamless loop. */
static const PIXELICA_AnimKeyframe k_sway_keys[3] = {
    {0u, -0.30f, PIXELICA_EASE_SMOOTHSTEP},
    {90u, 0.30f, PIXELICA_EASE_SMOOTHSTEP},
    {BODY_LOOP_TICKS, -0.30f, PIXELICA_EASE_LINEAR},
};

/* Right arm angle about the shoulder (radians, + swings the hand outward/up). */
static const PIXELICA_AnimKeyframe k_arm_keys[7] = {
    {0u, 0.10f, PIXELICA_EASE_QUAD_OUT},
    {50u, 2.20f, PIXELICA_EASE_LINEAR},
    {70u, 1.70f, PIXELICA_EASE_QUAD_IN_OUT},
    {90u, 2.20f, PIXELICA_EASE_QUAD_IN_OUT},
    {110u, 1.70f, PIXELICA_EASE_QUAD_IN_OUT},
    {130u, 2.20f, PIXELICA_EASE_SMOOTHSTEP},
    {BODY_LOOP_TICKS, 0.10f, PIXELICA_EASE_LINEAR},
};

/* Cube position along a PING_PONG sweep. */
static const PIXELICA_AnimKeyframe k_cube_x_keys[2] = {
    {0u, -2.4f, PIXELICA_EASE_QUAD_IN_OUT},
    {CUBE_SWEEP_TICKS, 2.4f, PIXELICA_EASE_LINEAR},
};
static const PIXELICA_AnimKeyframe k_cube_y_keys[5] = {
    {0u, -1.10f, PIXELICA_EASE_QUAD_OUT},
    {30u, -0.45f, PIXELICA_EASE_QUAD_IN},
    {60u, -1.10f, PIXELICA_EASE_QUAD_OUT},
    {90u, -0.45f, PIXELICA_EASE_QUAD_IN},
    {CUBE_SWEEP_TICKS, -1.10f, PIXELICA_EASE_LINEAR},
};

typedef struct BoxStyle {
    uint16_t face[FACE_COUNT];
} BoxStyle;

typedef enum StyleId {
    STYLE_TORSO,
    STYLE_HEAD,
    STYLE_VISOR,
    STYLE_ARM,
    STYLE_LEG,
    STYLE_CUBE,
    STYLE_FLOOR_RUNNING,
    STYLE_FLOOR_PAUSED,
    STYLE_COUNT
} StyleId;

typedef struct Pose {
    float sway;       /* robot root yaw */
    float arm;        /* right arm shoulder angle */
    float cube_x;
    float cube_y;
    float cube_scale; /* from pixelica_anim_tween */
    float cube_yaw;   /* from pixelica_anim_tween */
} Pose;

typedef struct DemoState {
    PIXELICA_AnimPlayer body_player; /* LOOP: sway + arm wave */
    PIXELICA_AnimPlayer cube_player; /* PING_PONG: cube sweep */
    Pose pose;
    PIXELICA_Status update_status;   /* first animation failure; render reports it */
    uint32_t previous_buttons;
    BoxStyle styles[STYLE_COUNT];
    float view_projection[16];
} DemoState;

static DemoState g_state;

static uint16_t shaded(uint8_t r, uint8_t g, uint8_t b, float factor)
{
    return PIXELICA_EXAMPLE_RGB565((uint8_t)((float)r * factor), (uint8_t)((float)g * factor),
                              (uint8_t)((float)b * factor));
}

static void set_style(BoxStyle *style, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t face = 0; face < FACE_COUNT; ++face) {
        style->face[face] = shaded(r, g, b, k_face_shade[face]);
    }
}

/* Evaluates every track for the players' current ticks into state->pose. */
static PIXELICA_Status evaluate_pose(DemoState *state)
{
    uint32_t body_tick = 0u;
    uint32_t cube_tick = 0u;
    float cube_u;
    PIXELICA_Status status;

    status = pixelica_anim_player_tick(&state->body_player, &body_tick);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_anim_sample(k_sway_keys, 3u, body_tick, &state->pose.sway);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_anim_sample(k_arm_keys, 7u, body_tick, &state->pose.arm);
    if (status != PIXELICA_OK) {
        return status;
    }

    status = pixelica_anim_player_tick(&state->cube_player, &cube_tick);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_anim_sample(k_cube_x_keys, 2u, cube_tick, &state->pose.cube_x);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_anim_sample(k_cube_y_keys, 5u, cube_tick, &state->pose.cube_y);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* Scalar tweens driven by the normalized sweep position 0..1. */
    cube_u = (float)cube_tick / (float)CUBE_SWEEP_TICKS;
    status = pixelica_anim_tween(0.45f, 0.95f, cube_u, PIXELICA_EASE_SMOOTHSTEP, &state->pose.cube_scale);
    if (status != PIXELICA_OK) {
        return status;
    }
    return pixelica_anim_tween(-0.9f, 0.9f, cube_u, PIXELICA_EASE_QUAD_IN_OUT, &state->pose.cube_yaw);
}

static PIXELICA_Status restart_players(DemoState *state)
{
    PIXELICA_Status status = pixelica_anim_player_init(&state->body_player, BODY_LOOP_TICKS, PIXELICA_ANIM_LOOP);

    if (status != PIXELICA_OK) {
        return status;
    }
    return pixelica_anim_player_init(&state->cube_player, CUBE_SWEEP_TICKS, PIXELICA_ANIM_PING_PONG);
}

static bool demo_init(void **out_state, PIXELICA_Context *ctx)
{
    float projection[16];
    float view[16];
    float tilt[16];

    (void)ctx;
    memset(&g_state, 0, sizeof(g_state));

    if (pixelica_mat4_perspective(projection, 0.87266463f, 256.0f / 192.0f, 0.1f, 50.0f) != PIXELICA_OK) {
        return false;
    }
    /* Orbit the origin from slightly above: view = T(0, 0, -6.8) * Rx(0.2). */
    pixelica_mat4_rotation_x(tilt, 0.2f);
    pixelica_mat4_translation(view, 0.0f, 0.0f, -6.8f);
    pixelica_mat4_multiply(view, view, tilt);
    pixelica_mat4_multiply(g_state.view_projection, projection, view);

    set_style(&g_state.styles[STYLE_TORSO], 70, 130, 220);
    set_style(&g_state.styles[STYLE_HEAD], 200, 205, 215);
    set_style(&g_state.styles[STYLE_VISOR], 80, 240, 230);
    set_style(&g_state.styles[STYLE_ARM], 235, 150, 60);
    set_style(&g_state.styles[STYLE_LEG], 60, 90, 160);
    set_style(&g_state.styles[STYLE_CUBE], 220, 70, 90);
    set_style(&g_state.styles[STYLE_FLOOR_RUNNING], 70, 80, 100);
    set_style(&g_state.styles[STYLE_FLOOR_PAUSED], 130, 100, 60);

    if (restart_players(&g_state) != PIXELICA_OK || evaluate_pose(&g_state) != PIXELICA_OK) {
        return false;
    }
    g_state.update_status = PIXELICA_OK;

    *out_state = &g_state;
    return true;
}

/* Fixed update: exactly one animation tick per call, dt is intentionally unused. */
static void demo_update(void *userdata, float dt, uint32_t buttons)
{
    DemoState *state = userdata;
    uint32_t pressed = buttons & ~state->previous_buttons;
    PIXELICA_Status status = PIXELICA_OK;
    bool restarted = false;

    (void)dt;
    state->previous_buttons = buttons;

    if ((pressed & PIXELICA_BTN_START) != 0u) {
        /* Re-init restarts from tick 0 and clears the paused flag. START wins over A. */
        status = restart_players(state);
        restarted = true;
    } else if ((pressed & PIXELICA_BTN_A) != 0u) {
        bool paused = !state->body_player.paused;

        state->body_player.paused = paused;
        state->cube_player.paused = paused;
    }

    /* The restart frame shows exactly tick 0, so it does not advance. */
    if (status == PIXELICA_OK && !restarted) {
        /* A paused player accepts the advance and simply does not move. */
        status = pixelica_anim_player_advance(&state->body_player, 1u);
    }
    if (status == PIXELICA_OK && !restarted) {
        status = pixelica_anim_player_advance(&state->cube_player, 1u);
    }
    if (status == PIXELICA_OK) {
        status = evaluate_pose(state);
    }
    if (state->update_status == PIXELICA_OK) {
        state->update_status = status; /* keep the first failure */
    }
}

static PIXELICA_Transform make_part(float tx, float ty, float tz, float sx, float sy, float sz)
{
    PIXELICA_Transform transform;

    pixelica_transform_identity(&transform);
    transform.translation.x = tx;
    transform.translation.y = ty;
    transform.translation.z = tz;
    transform.scale.x = sx;
    transform.scale.y = sy;
    transform.scale.z = sz;
    return transform;
}

static PIXELICA_Status draw_box(PIXELICA_Context *ctx, const float world[16], const BoxStyle *style)
{
    for (uint32_t face = 0; face < FACE_COUNT; ++face) {
        PIXELICA_Mesh mesh = {k_box_vertices, 8u, k_box_faces[face], 6u, style->face[face]};
        PIXELICA_Status status = pixelica_draw_mesh(ctx, &mesh, world);

        if (status != PIXELICA_OK) {
            return status;
        }
    }
    return PIXELICA_OK;
}

/* Draws one box part. A non-NULL parent is the parent's WORLD matrix. */
static PIXELICA_Status draw_part(PIXELICA_Context *ctx, const float *parent_world,
                            const PIXELICA_Transform *transform, const BoxStyle *style)
{
    float local[16];
    float world[16];
    PIXELICA_Status status = pixelica_transform_matrix(local, transform);

    if (status != PIXELICA_OK) {
        return status;
    }
    if (parent_world == NULL) {
        return draw_box(ctx, local, style);
    }
    pixelica_mat4_multiply(world, parent_world, local);
    return draw_box(ctx, world, style);
}

static PIXELICA_Status demo_render(void *userdata, PIXELICA_Context *ctx)
{
    DemoState *state = userdata;
    const Pose *pose = &state->pose;
    PIXELICA_Transform root;
    PIXELICA_Transform part;
    float root_world[16];
    PIXELICA_Status status = state->update_status;

    if (status != PIXELICA_OK) {
        return status;
    }

    /* The root carries no scale, so children inherit only translation + yaw. */
    pixelica_transform_identity(&root);
    root.rotation.y = pose->sway;
    status = pixelica_transform_matrix(root_world, &root);
    if (status != PIXELICA_OK) {
        return status;
    }

    pixelica_begin_frame(ctx, PIXELICA_EXAMPLE_RGB565(24, 28, 40));
    pixelica_set_view_projection(ctx, state->view_projection);

    part = make_part(0.0f, -1.85f, 0.5f, 7.0f, 0.2f, 4.0f); /* floor (top at y = -1.75) */
    status = draw_part(ctx, NULL, &part,
                       &state->styles[state->body_player.paused ? STYLE_FLOOR_PAUSED
                                                                : STYLE_FLOOR_RUNNING]);
    if (status != PIXELICA_OK) {
        return status;
    }

    part = make_part(0.0f, 0.0f, 0.0f, 1.0f, 1.5f, 0.6f); /* torso */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_TORSO]);
    if (status != PIXELICA_OK) {
        return status;
    }
    part = make_part(0.0f, 1.1f, 0.0f, 0.7f, 0.7f, 0.7f); /* head */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_HEAD]);
    if (status != PIXELICA_OK) {
        return status;
    }
    part = make_part(0.0f, 1.15f, 0.38f, 0.5f, 0.15f, 0.06f); /* visor on the head front */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_VISOR]);
    if (status != PIXELICA_OK) {
        return status;
    }
    part = make_part(-0.25f, -1.25f, 0.0f, 0.4f, 1.0f, 0.4f); /* legs */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_LEG]);
    if (status != PIXELICA_OK) {
        return status;
    }
    part = make_part(0.25f, -1.25f, 0.0f, 0.4f, 1.0f, 0.4f);
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_LEG]);
    if (status != PIXELICA_OK) {
        return status;
    }

    /*
     * Arms: a unit box scaled to 0.3 x 1.2 x 0.3 whose mesh-local pivot is the
     * middle of its TOP face. The pivot is applied before scale, so it stays at
     * translation + pivot while the arm hangs 1.2 units below it. For a shoulder
     * at S the translation is therefore S - pivot.
     */
    part = make_part(-0.65f, 0.65f - 0.5f, 0.0f, 0.3f, 1.2f, 0.3f);
    part.pivot.y = 0.5f;
    part.rotation.z = -0.12f; /* idle left arm */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_ARM]);
    if (status != PIXELICA_OK) {
        return status;
    }
    part = make_part(0.65f, 0.65f - 0.5f, 0.0f, 0.3f, 1.2f, 0.3f);
    part.pivot.y = 0.5f;
    part.rotation.z = pose->arm; /* the waving arm */
    status = draw_part(ctx, root_world, &part, &state->styles[STYLE_ARM]);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* Free-standing cube: position from keyframes, scale/yaw from tweens. */
    part = make_part(pose->cube_x, pose->cube_y, 1.5f, pose->cube_scale, pose->cube_scale,
                     pose->cube_scale);
    part.rotation.x = 0.4f;
    part.rotation.y = pose->cube_yaw;
    status = draw_part(ctx, NULL, &part, &state->styles[STYLE_CUBE]);
    if (status != PIXELICA_OK) {
        return status;
    }

    return pixelica_end_frame(ctx);
}

static void demo_shutdown(void *userdata)
{
    (void)userdata;
}

const PIXELICA_Game pixelica_example_game = {
    "Pixelica hello_animation",
    {256u, 192u, 64u, 64u, 65536u},
    demo_init,
    demo_update,
    demo_render,
    demo_shutdown,
};
