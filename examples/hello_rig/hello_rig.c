/*
 * hello_rig: the OPTIONAL skeleton + IK modules working together
 * (built only with -DPIXELICA_BUILD_SKELETON=ON -DPIXELICA_BUILD_IK=ON).
 *
 *   - A flat ribbon ("limb") made of 11 vertex columns along X is the read-only
 *     BIND mesh. Every column carries up to two bone weights: columns near the
 *     middle blend 75/25, 50/50, 25/75 between the upper and lower bone, so the
 *     bend around the joint is soft (linear blend skinning, positions only).
 *   - A two-bone skeleton (root bone at the model origin, joint bone at
 *     x = UPPER_LENGTH in the bind pose) with inverse bind matrices.
 *   - A target moves along a deterministic looping keyframe track. Each fixed
 *     update pixelica_ik_two_bone() solves the joint position (pole = root + (0,2,0),
 *     so the joint bends upward), atan2f() turns joint/end positions into the
 *     two bone angles about Z, pixelica_skeleton_pose() builds world + palette
 *     matrices and pixelica_skeleton_skin() deforms the ribbon into a state-owned
 *     vertex array that is drawn directly as PIXELICA_Mesh.vertices.
 *   - Bone segments (white boxes), the joint (yellow), the pole (blue) and the
 *     target are drawn on top. The target is green while reachable and red when
 *     the IK distance clamp is active (too far, or closer than |L0 - L1|).
 *
 * All matrices and the ribbon live in MODEL space; the object is placed in the
 * scene only by the model matrix given to pixelica_draw_mesh(). IK input and output
 * use the same model space.
 *
 * Keys: A pauses/resumes (the background tint changes), START restarts at tick 0.
 * Nothing allocates; every status is stored and render returns the first failure
 * instead of drawing stale data.
 */
#include <math.h>

#include "pixelica/animation.h"
#include "pixelica/ik.h"
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#include "pixelica/platform.h"
#include "pixelica/renderer.h"
#include "pixelica/skeleton.h"

#include "../common/pixelica_game.h"

#define BONE_COUNT 2u
#define COLUMN_COUNT 11u
#define RIBBON_VERTS (COLUMN_COUNT * 2u)
#define RIBBON_QUADS (COLUMN_COUNT - 1u)
#define TRACK_TICKS 360u /* one target loop: 6 s at 60 Hz */

#define UPPER_LENGTH 1.6f
#define LOWER_LENGTH 1.4f
#define RIBBON_HALF_HEIGHT 0.22f
#define FACE_COUNT 6u

/* Bone matrices as const float (*)[16]; C11 has no implicit conversion for it. */
#define CONST_MATRICES(p) ((const float (*)[16])(const void *)(p))

/* Model-space IK root and pole: the pole is a POINT above the root. The root is
 * the model origin because the bind mesh below is authored from x = 0. */
static const PIXELICA_Vec3 k_root = {0.0f, 0.0f, 0.0f};
static const PIXELICA_Vec3 k_pole = {0.0f, 2.0f, 0.0f};
/* Placement of the whole limb in the scene (the model matrix). */
static const PIXELICA_Vec3 k_placement = {-1.4f, -0.4f, 0.0f};

/* Bind mesh: two vertices (top, bottom) per column, columns along +X. */
#define COLUMN(x) {(x), RIBBON_HALF_HEIGHT, 0.0f}, {(x), -RIBBON_HALF_HEIGHT, 0.0f}
static const PIXELICA_Vec3 k_bind_vertices[RIBBON_VERTS] = {
    COLUMN(0.0f), COLUMN(0.4f), COLUMN(0.8f), COLUMN(1.2f), COLUMN(1.4f), COLUMN(1.6f),
    COLUMN(1.8f), COLUMN(2.0f), COLUMN(2.4f), COLUMN(2.8f), COLUMN(3.0f),
};

/* Both vertices of a column share their weights. */
#define ONLY(bone) {{(bone), 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}}
#define BLEND(upper, lower) {{0, 1, 0, 0}, {(upper), (lower), 0.0f, 0.0f}}
#define COLUMN_WEIGHTS(init) init, init
static const PIXELICA_SkinWeight k_weights[RIBBON_VERTS] = {
    COLUMN_WEIGHTS(ONLY(0)),
    COLUMN_WEIGHTS(ONLY(0)),
    COLUMN_WEIGHTS(ONLY(0)),
    COLUMN_WEIGHTS(ONLY(0)),               /* x = 1.2 */
    COLUMN_WEIGHTS(BLEND(0.75f, 0.25f)),   /* x = 1.4 */
    COLUMN_WEIGHTS(BLEND(0.5f, 0.5f)),     /* x = 1.6: at the joint */
    COLUMN_WEIGHTS(BLEND(0.25f, 0.75f)),   /* x = 1.8 */
    COLUMN_WEIGHTS(ONLY(1)),
    COLUMN_WEIGHTS(ONLY(1)),
    COLUMN_WEIGHTS(ONLY(1)),
    COLUMN_WEIGHTS(ONLY(1)),
};

/* Quad q joins columns q and q+1. Vertex 2c is the top, 2c + 1 the bottom;
 * counter-clockwise seen from +Z. */
#define QUAD(q) {(2 * (q) + 1), (2 * (q) + 3), (2 * (q) + 2), (2 * (q) + 1), (2 * (q) + 2), (2 * (q))}
static const uint16_t k_ribbon_indices[RIBBON_QUADS][6] = {
    QUAD(0), QUAD(1), QUAD(2), QUAD(3), QUAD(4), QUAD(5), QUAD(6), QUAD(7), QUAD(8), QUAD(9),
};

/*
 * Target track (model space, z = 0). x stays positive so the pole is never on the
 * chain axis. Reach is UPPER + LOWER = 3.0, the minimum distance |L0 - L1| = 0.2:
 *   tick   0 (1.20, 0.00)  reachable        tick 120 (3.50, 0.40)  too far: clamped
 *   tick  60 (2.60, 1.20)  reachable        tick 180 (2.20, 1.80)  reachable
 *   tick 240 (0.12, 0.06)  too close: clamped, chain folded
 *   tick 300 (0.80,-0.90)  reachable
 */
static const PIXELICA_AnimKeyframe k_target_x[7] = {
    {0u, 1.20f, PIXELICA_EASE_SMOOTHSTEP},   {60u, 2.60f, PIXELICA_EASE_SMOOTHSTEP},
    {120u, 3.50f, PIXELICA_EASE_SMOOTHSTEP}, {180u, 2.20f, PIXELICA_EASE_SMOOTHSTEP},
    {240u, 0.12f, PIXELICA_EASE_SMOOTHSTEP}, {300u, 0.80f, PIXELICA_EASE_SMOOTHSTEP},
    {TRACK_TICKS, 1.20f, PIXELICA_EASE_LINEAR},
};
static const PIXELICA_AnimKeyframe k_target_y[7] = {
    {0u, 0.00f, PIXELICA_EASE_SMOOTHSTEP},   {60u, 1.20f, PIXELICA_EASE_SMOOTHSTEP},
    {120u, 0.40f, PIXELICA_EASE_SMOOTHSTEP}, {180u, 1.80f, PIXELICA_EASE_SMOOTHSTEP},
    {240u, 0.06f, PIXELICA_EASE_SMOOTHSTEP}, {300u, -0.90f, PIXELICA_EASE_SMOOTHSTEP},
    {TRACK_TICKS, 0.00f, PIXELICA_EASE_LINEAR},
};

/* Unit box: vertex index bits x = bit 0, y = bit 1, z = bit 2 (set = positive). */
#define HALF 0.5f
static const PIXELICA_Vec3 k_box_vertices[8] = {
    {-HALF, -HALF, -HALF}, {HALF, -HALF, -HALF}, {-HALF, HALF, -HALF}, {HALF, HALF, -HALF},
    {-HALF, -HALF, HALF},  {HALF, -HALF, HALF},  {-HALF, HALF, HALF},  {HALF, HALF, HALF},
};
static const uint16_t k_box_faces[FACE_COUNT][6] = {
    {4, 5, 7, 4, 7, 6}, /* +Z */
    {1, 0, 2, 1, 2, 3}, /* -Z */
    {5, 1, 3, 5, 3, 7}, /* +X */
    {0, 4, 6, 0, 6, 2}, /* -X */
    {6, 7, 3, 6, 3, 2}, /* +Y */
    {0, 1, 5, 0, 5, 4}, /* -Y */
};
static const float k_face_shade[FACE_COUNT] = {1.00f, 0.50f, 0.78f, 0.64f, 0.90f, 0.42f};

typedef struct BoxStyle {
    uint16_t face[FACE_COUNT];
} BoxStyle;

typedef enum StyleId {
    STYLE_BONE,
    STYLE_JOINT,
    STYLE_POLE,
    STYLE_TARGET_REACHABLE,
    STYLE_TARGET_CLAMPED,
    STYLE_COUNT
} StyleId;

typedef struct DemoState {
    PIXELICA_AnimPlayer player;
    /* Skeleton data (caller-owned, never reallocated) and pose workspaces. */
    PIXELICA_Bone bones[BONE_COUNT];
    PIXELICA_Skeleton skeleton;
    float local[BONE_COUNT][16];
    float world[BONE_COUNT][16];
    float palette[BONE_COUNT][16];
    float pose_scratch[2u * BONE_COUNT][16];
    /* Skinned ribbon: drawn directly as PIXELICA_Mesh.vertices; skin workspace. */
    PIXELICA_Vec3 skinned[RIBBON_VERTS];
    PIXELICA_Vec3 skin_scratch[RIBBON_VERTS];
    PIXELICA_Vec3 target;
    PIXELICA_IKTwoBoneResult ik;
    PIXELICA_Status update_status; /* first failure; render reports it */
    uint32_t previous_buttons;
    uint16_t ribbon_color[RIBBON_QUADS];
    BoxStyle styles[STYLE_COUNT];
    float model[16];
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

/* Builds local = T(translation) * Rz(angle). */
static void bone_local(float out[16], PIXELICA_Vec3 translation, float angle)
{
    float shift[16];
    float rot[16];

    pixelica_mat4_translation(shift, translation.x, translation.y, translation.z);
    pixelica_mat4_rotation_z(rot, angle);
    pixelica_mat4_multiply(out, shift, rot);
}

/*
 * IK -> bone angles -> pose -> skin for the player's current tick. The bind pose
 * is local0 = T(root), local1 = T(UPPER_LENGTH, 0, 0) with inverse bind
 * matrices T(-root) and T(-UPPER_LENGTH, 0, 0) (see demo_init), so the ribbon
 * and the palette share the same model space.
 */
static PIXELICA_Status evaluate_pose(DemoState *state)
{
    uint32_t tick = 0u;
    PIXELICA_IKTwoBone chain;
    float upper_angle;
    float lower_angle;
    PIXELICA_Status status = pixelica_anim_player_tick(&state->player, &tick);

    if (status != PIXELICA_OK) {
        return status;
    }
    state->target.z = 0.0f;
    status = pixelica_anim_sample(k_target_x, 7u, tick, &state->target.x);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = pixelica_anim_sample(k_target_y, 7u, tick, &state->target.y);
    if (status != PIXELICA_OK) {
        return status;
    }

    chain.root = k_root;
    chain.target = state->target;
    chain.pole = k_pole;
    chain.upper_length = UPPER_LENGTH;
    chain.lower_length = LOWER_LENGTH;
    status = pixelica_ik_two_bone(&chain, &state->ik);
    if (status != PIXELICA_OK) {
        return status;
    }

    /* Planar chain in XY: the bone angles about Z come straight from the positions. */
    upper_angle = atan2f(state->ik.joint.y - k_root.y, state->ik.joint.x - k_root.x);
    lower_angle = atan2f(state->ik.end.y - state->ik.joint.y, state->ik.end.x - state->ik.joint.x);
    bone_local(state->local[0], k_root, upper_angle);
    /* The lower bone sits UPPER_LENGTH along its parent's X axis and turns relative to it. */
    bone_local(state->local[1], (PIXELICA_Vec3){UPPER_LENGTH, 0.0f, 0.0f}, lower_angle - upper_angle);

    status = pixelica_skeleton_pose(&state->skeleton, CONST_MATRICES(state->local), state->world,
                               state->palette, state->pose_scratch, 2u * BONE_COUNT);
    if (status != PIXELICA_OK) {
        return status;
    }
    return pixelica_skeleton_skin(k_bind_vertices, k_weights, RIBBON_VERTS,
                             CONST_MATRICES(state->palette), BONE_COUNT, state->skinned,
                             state->skin_scratch, RIBBON_VERTS);
}

static bool demo_init(void **out_state, PIXELICA_Context *ctx)
{
    float projection[16];
    float view[16];

    (void)ctx;
    memset(&g_state, 0, sizeof(g_state));

    if (pixelica_mat4_perspective(projection, 0.87266463f, 256.0f / 192.0f, 0.1f, 50.0f) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(view, 0.0f, 0.0f, -7.0f);
    pixelica_mat4_multiply(g_state.view_projection, projection, view);
    pixelica_mat4_translation(g_state.model, k_placement.x, k_placement.y, k_placement.z);

    /* Bind pose: inverse bind = inverse of each bone's bind WORLD matrix. */
    g_state.bones[0].parent = -1;
    pixelica_mat4_translation(g_state.bones[0].inverse_bind, -k_root.x, -k_root.y, -k_root.z);
    g_state.bones[1].parent = 0;
    pixelica_mat4_translation(g_state.bones[1].inverse_bind, -(k_root.x + UPPER_LENGTH), -k_root.y,
                         -k_root.z);
    g_state.skeleton.bones = g_state.bones;
    g_state.skeleton.bone_count = BONE_COUNT;

    for (uint32_t q = 0; q < RIBBON_QUADS; ++q) {
        float t = (float)q / (float)(RIBBON_QUADS - 1u);

        g_state.ribbon_color[q] = PIXELICA_EXAMPLE_RGB565(
            (uint8_t)(60.0f + 180.0f * t), (uint8_t)(200.0f - 40.0f * t),
            (uint8_t)(170.0f - 110.0f * t));
    }
    set_style(&g_state.styles[STYLE_BONE], 235, 235, 245);
    set_style(&g_state.styles[STYLE_JOINT], 255, 225, 80);
    set_style(&g_state.styles[STYLE_POLE], 90, 140, 255);
    set_style(&g_state.styles[STYLE_TARGET_REACHABLE], 90, 220, 120);
    set_style(&g_state.styles[STYLE_TARGET_CLAMPED], 240, 80, 80);

    if (pixelica_anim_player_init(&g_state.player, TRACK_TICKS, PIXELICA_ANIM_LOOP) != PIXELICA_OK ||
        evaluate_pose(&g_state) != PIXELICA_OK) {
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
        /* Re-init restarts at tick 0 (the exact initial pose) and clears the pause. */
        status = pixelica_anim_player_init(&state->player, TRACK_TICKS, PIXELICA_ANIM_LOOP);
        restarted = true;
    } else if ((pressed & PIXELICA_BTN_A) != 0u) {
        state->player.paused = !state->player.paused;
    }
    if (status == PIXELICA_OK && !restarted) {
        status = pixelica_anim_player_advance(&state->player, 1u);
    }
    if (status == PIXELICA_OK) {
        status = evaluate_pose(state);
    }
    if (state->update_status == PIXELICA_OK) {
        state->update_status = status; /* keep the first failure */
    }
}

/* out = model * frame * T(center) * S(size); a NULL frame is the identity. */
static void box_matrix(float out[16], const float model[16], const float *frame,
                       PIXELICA_Vec3 center, PIXELICA_Vec3 size)
{
    float shift[16];
    float scale[16];
    float local[16];
    float base[16];

    pixelica_mat4_translation(shift, center.x, center.y, center.z);
    pixelica_mat4_scale(scale, size.x, size.y, size.z);
    pixelica_mat4_multiply(local, shift, scale);
    if (frame != NULL) {
        pixelica_mat4_multiply(base, model, frame);
        pixelica_mat4_multiply(out, base, local);
    } else {
        pixelica_mat4_multiply(out, model, local);
    }
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

static PIXELICA_Status draw_marker(PIXELICA_Context *ctx, const DemoState *state, PIXELICA_Vec3 center,
                              float size, StyleId style)
{
    float world[16];

    box_matrix(world, state->model, NULL, center, (PIXELICA_Vec3){size, size, size});
    return draw_box(ctx, world, &state->styles[style]);
}

static PIXELICA_Status demo_render(void *userdata, PIXELICA_Context *ctx)
{
    DemoState *state = userdata;
    PIXELICA_Status status = state->update_status;

    if (status != PIXELICA_OK) {
        return status;
    }

    pixelica_begin_frame(ctx, state->player.paused ? PIXELICA_EXAMPLE_RGB565(52, 40, 30)
                                              : PIXELICA_EXAMPLE_RGB565(24, 28, 40));
    pixelica_set_view_projection(ctx, state->view_projection);

    /* The deformed ribbon. A folded chain can flip triangles, so draw both windings. */
    pixelica_set_cull_mode(ctx, PIXELICA_CULL_NONE);
    for (uint32_t q = 0; q < RIBBON_QUADS; ++q) {
        PIXELICA_Mesh mesh = {state->skinned, RIBBON_VERTS, k_ribbon_indices[q], 6u,
                         state->ribbon_color[q]};

        status = pixelica_draw_mesh(ctx, &mesh, state->model);
        if (status != PIXELICA_OK) {
            return status;
        }
    }
    pixelica_set_cull_mode(ctx, PIXELICA_CULL_BACK);

    /* Bone segments: a slim box along each bone's local +X axis. */
    for (uint32_t bone = 0; bone < BONE_COUNT; ++bone) {
        float length = (bone == 0u) ? UPPER_LENGTH : LOWER_LENGTH;
        float world[16];

        box_matrix(world, state->model, state->world[bone], (PIXELICA_Vec3){0.5f * length, 0.0f, 0.0f},
                   (PIXELICA_Vec3){length, 0.08f, 0.14f});
        status = draw_box(ctx, world, &state->styles[STYLE_BONE]);
        if (status != PIXELICA_OK) {
            return status;
        }
    }

    status = draw_marker(ctx, state, state->ik.joint, 0.26f, STYLE_JOINT);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = draw_marker(ctx, state, k_pole, 0.2f, STYLE_POLE);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = draw_marker(ctx, state, state->target, 0.3f,
                         state->ik.clamped ? STYLE_TARGET_CLAMPED : STYLE_TARGET_REACHABLE);
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
    "Pixelica hello_rig",
    {256u, 192u, 64u, 64u, 65536u},
    demo_init,
    demo_update,
    demo_render,
    demo_shutdown,
};
