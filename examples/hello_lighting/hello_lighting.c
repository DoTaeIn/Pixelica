/*
 * hello_lighting: one runnable demonstration of the three opt-in features
 * added on top of the unlit renderer.
 *
 *   1. NORMAL-BASED LIGHTING (pixelica/renderer.h, pixelica_draw_mesh_lit).
 *      A cube with one outward normal per face, lit by an ambient term and
 *      two directional lights. Its model matrix carries a NON-UNIFORM
 *      scale, so the renderer's inverse-transpose normal matrix is what
 *      keeps the shading correct; transforming the normals with the model
 *      itself would tilt them the wrong way.
 *   2. FRAMEBUFFER ADDITIVE BLENDING (PIXELICA_ALPHA_ADD). Two overlapping
 *      glow quads are added on top of the cube. They are depth-TESTED
 *      against it but never WRITE depth, and the channels saturate where
 *      they overlap instead of wrapping round.
 *   3. SKELETAL ANIMATION CLIPS (pixelica/anim_clip.h). A one-bone clip
 *      with translation and rotation tracks drives the cube, looping, with
 *      timed events reported through a callback. A crossfade to a second,
 *      faster clip is started partway through.
 *
 * Everything runs on the ordinary example driver, so it works with SDL3 and
 * headless alike:
 *     hello_lighting --frames 120 --hidden
 * and prints a one-line summary on shutdown, which is what the smoke test
 * checks. Keys: A toggles the lights, START restarts the clip.
 */
#include "pixelica/anim_clip.h"
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#include "pixelica/platform.h"
#include "pixelica/renderer.h"

#include "../common/pixelica_game.h"

#define HALF 0.75f
#define FACE_COUNT 6u
#define VERT_COUNT 24u
#define INDEX_COUNT 36u

/* Clip timing, in fixed 60 Hz ticks. */
#define WALK_TICKS 120u
#define RUN_TICKS 48u
#define CROSSFADE_AT 180u
#define CROSSFADE_TICKS 30u

#define EVENT_STEP_LEFT 1u
#define EVENT_STEP_RIGHT 2u

/* Four vertices per face so every face carries its own flat normal. */
static const PIXELICA_Vec3 k_vertices[VERT_COUNT] = {
    /* +Z */
    {-HALF, -HALF, HALF}, {HALF, -HALF, HALF}, {HALF, HALF, HALF}, {-HALF, HALF, HALF},
    /* -Z */
    {HALF, -HALF, -HALF}, {-HALF, -HALF, -HALF}, {-HALF, HALF, -HALF}, {HALF, HALF, -HALF},
    /* +X */
    {HALF, -HALF, HALF}, {HALF, -HALF, -HALF}, {HALF, HALF, -HALF}, {HALF, HALF, HALF},
    /* -X */
    {-HALF, -HALF, -HALF}, {-HALF, -HALF, HALF}, {-HALF, HALF, HALF}, {-HALF, HALF, -HALF},
    /* +Y */
    {-HALF, HALF, HALF}, {HALF, HALF, HALF}, {HALF, HALF, -HALF}, {-HALF, HALF, -HALF},
    /* -Y */
    {-HALF, -HALF, -HALF}, {HALF, -HALF, -HALF}, {HALF, -HALF, HALF}, {-HALF, -HALF, HALF},
};

static const PIXELICA_Vec3 k_normals[VERT_COUNT] = {
    {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, -1.0f},
    {1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},
    {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},  {0.0f, 1.0f, 0.0f},  {0.0f, 1.0f, 0.0f},  {0.0f, 1.0f, 0.0f},
    {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
};

static uint16_t k_indices[INDEX_COUNT];

/* A screen-facing quad for the additive glows, in world space. */
static const PIXELICA_Vec3 k_glow[4] = {
    {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f},
};
static const uint16_t k_glow_indices[6] = {0, 1, 2, 0, 2, 3};

typedef struct Clip1 {
    PIXELICA_ClipVec3Key translation[3];
    PIXELICA_ClipQuatKey rotation[3];
    PIXELICA_ClipBoneTrack track;
    PIXELICA_ClipEvent events[2];
    PIXELICA_AnimClip clip;
} Clip1;

typedef struct LitState {
    PIXELICA_ClipPlayer player;
    Clip1 walk;
    Clip1 run;
    PIXELICA_TRS pose[1];
    PIXELICA_TRS scratch[1];
    PIXELICA_DirectionalLight lights[2];
    float view_projection[16];
    uint32_t ticks;
    uint32_t steps_left;
    uint32_t steps_right;
    uint32_t loops;
    uint32_t crossfades;
    uint32_t lit_pixels;
    uint32_t glow_pixels;
    uint32_t previous_buttons;
    bool lights_on;
    bool faded;
} LitState;

static LitState g_state;

/* ---- clip construction -------------------------------------------------- */

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* A one-bone clip: a bob in Y and a full turn about Y, with a footstep
 * event at each half. `lift` and `duration` give the two clips their
 * different character. */
static bool clip1_init(Clip1 *c, uint32_t duration, float lift)
{
    const uint32_t half = duration / 2u;

    memset(c, 0, sizeof *c);
    c->translation[0].tick = 0u;
    c->translation[0].value = vec3(0.0f, 0.0f, 0.0f);
    c->translation[0].ease = PIXELICA_EASE_SMOOTHSTEP;
    c->translation[1].tick = half;
    c->translation[1].value = vec3(0.0f, lift, 0.0f);
    c->translation[1].ease = PIXELICA_EASE_SMOOTHSTEP;
    c->translation[2].tick = duration;
    c->translation[2].value = vec3(0.0f, 0.0f, 0.0f);
    c->translation[2].ease = PIXELICA_EASE_SMOOTHSTEP;

    c->rotation[0].tick = 0u;
    c->rotation[1].tick = half;
    c->rotation[2].tick = duration;
    if (pixelica_quat_from_axis_angle(&c->rotation[0].value, vec3(0.2f, 1.0f, 0.0f),
                                      0.0f) != PIXELICA_OK ||
        pixelica_quat_from_axis_angle(&c->rotation[1].value, vec3(0.2f, 1.0f, 0.0f),
                                      3.14159265f) != PIXELICA_OK ||
        pixelica_quat_from_axis_angle(&c->rotation[2].value, vec3(0.2f, 1.0f, 0.0f),
                                      6.28318531f) != PIXELICA_OK) {
        return false;
    }
    c->rotation[0].ease = PIXELICA_EASE_LINEAR;
    c->rotation[1].ease = PIXELICA_EASE_LINEAR;
    c->rotation[2].ease = PIXELICA_EASE_LINEAR;

    c->track.translation = c->translation;
    c->track.translation_count = 3u;
    c->track.rotation = c->rotation;
    c->track.rotation_count = 3u;

    /* Tick 0 fires on the first advance and on every loop wrap; the
     * mid-clip one fires on the step that reaches it. */
    c->events[0].tick = 0u;
    c->events[0].id = EVENT_STEP_LEFT;
    c->events[1].tick = half;
    c->events[1].id = EVENT_STEP_RIGHT;
    c->clip.tracks = &c->track;
    c->clip.bone_count = 1u;
    c->clip.duration_ticks = duration;
    c->clip.events = c->events;
    c->clip.event_count = 2u;
    return pixelica_clip_validate(&c->clip) == PIXELICA_OK;
}

static void on_clip_event(void *userdata, const PIXELICA_ClipEventInfo *info)
{
    LitState *s = (LitState *)userdata;

    switch (info->kind) {
    case PIXELICA_CLIP_EVENT_USER:
        if (info->id == EVENT_STEP_LEFT) {
            ++s->steps_left;
        } else {
            ++s->steps_right;
        }
        break;
    case PIXELICA_CLIP_EVENT_LOOP:
        ++s->loops;
        break;
    case PIXELICA_CLIP_EVENT_END:
    default:
        break;
    }
}

/* ---- game callbacks ------------------------------------------------------ */

static bool lit_init(void **out_state, PIXELICA_Context *ctx)
{
    LitState *s = &g_state;
    float projection[16];
    float view[16];
    uint32_t face;

    (void)ctx;
    memset(s, 0, sizeof *s); /* also the clip player's zero-init contract */
    for (face = 0; face < FACE_COUNT; ++face) {
        const uint16_t base = (uint16_t)(face * 4u);

        k_indices[face * 6u + 0u] = base;
        k_indices[face * 6u + 1u] = (uint16_t)(base + 1u);
        k_indices[face * 6u + 2u] = (uint16_t)(base + 2u);
        k_indices[face * 6u + 3u] = base;
        k_indices[face * 6u + 4u] = (uint16_t)(base + 2u);
        k_indices[face * 6u + 5u] = (uint16_t)(base + 3u);
    }

    if (!clip1_init(&s->walk, WALK_TICKS, 0.45f) ||
        !clip1_init(&s->run, RUN_TICKS, 0.9f)) {
        return false;
    }
    if (pixelica_clip_player_init(&s->player, &s->walk.clip, true) != PIXELICA_OK) {
        return false;
    }

    /* Key light from the upper front left, cool fill from the right. */
    s->lights[0].direction = vec3(0.6f, -0.8f, -0.5f);
    s->lights[0].r = 255u;
    s->lights[0].g = 230u;
    s->lights[0].b = 170u;
    s->lights[1].direction = vec3(-0.9f, 0.1f, -0.3f);
    s->lights[1].r = 60u;
    s->lights[1].g = 80u;
    s->lights[1].b = 130u;
    s->lights_on = true;

    if (pixelica_mat4_perspective(projection, 1.0f, 4.0f / 3.0f, 0.1f, 50.0f) !=
        PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(view, 0.0f, -0.2f, -4.0f);
    pixelica_mat4_multiply(s->view_projection, projection, view);

    *out_state = s;
    return true;
}

static void lit_update(void *userdata, float dt, uint32_t buttons)
{
    LitState *s = (LitState *)userdata;
    const uint32_t pressed = buttons & ~s->previous_buttons;

    (void)dt;
    s->previous_buttons = buttons;
    if ((pressed & PIXELICA_BTN_A) != 0u) {
        s->lights_on = !s->lights_on;
    }
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        (void)pixelica_clip_player_init(&s->player, &s->walk.clip, true);
        s->faded = false;
    }

    ++s->ticks;
    /* Blend into the faster clip once, partway through the run. */
    if (!s->faded && s->ticks == CROSSFADE_AT) {
        if (pixelica_clip_player_crossfade(&s->player, &s->run.clip, true,
                                           CROSSFADE_TICKS,
                                           PIXELICA_EASE_SMOOTHSTEP) ==
            PIXELICA_OK) {
            s->faded = true;
            ++s->crossfades;
        }
    }
    /* One tick per fixed step; the events of this step are reported now. */
    (void)pixelica_clip_player_advance(&s->player, 1u, on_clip_event, s);
    (void)pixelica_clip_player_sample(&s->player, s->pose, 1u, s->scratch, 1u);
}

/* The glow quad, drawn additively in front of the cube. */
static PIXELICA_Status draw_glow(PIXELICA_Context *ctx, float x, uint16_t color,
                                 uint8_t opacity)
{
    PIXELICA_Mesh mesh;
    PIXELICA_DrawOptions options = pixelica_draw_options_default();
    float model[16];

    pixelica_mat4_translation(model, x, 0.0f, 1.2f);
    mesh.vertices = k_glow;
    mesh.vertex_count = 4u;
    mesh.indices = k_glow_indices;
    mesh.index_count = 6u;
    mesh.color = color;
    options.alpha_mode = PIXELICA_ALPHA_ADD;
    options.opacity = opacity;
    return pixelica_draw_mesh_options(ctx, &mesh, model, NULL, &options);
}

static PIXELICA_Status lit_render(void *userdata, PIXELICA_Context *ctx)
{
    LitState *s = (LitState *)userdata;
    PIXELICA_Mesh mesh;
    PIXELICA_Lighting lighting;
    PIXELICA_Status status;
    float local[16];
    float squash[16];
    float model[16];
    uint32_t written_after_cube;

    pixelica_begin_frame(ctx, PIXELICA_EXAMPLE_RGB565(8, 10, 24));
    pixelica_set_view_projection(ctx, s->view_projection);

    /* The clip supplies the local transform; the non-uniform squash is what
     * makes the inverse-transpose normal matrix matter. */
    if (pixelica_trs_matrix(local, &s->pose[0]) != PIXELICA_OK) {
        return pixelica_end_frame(ctx);
    }
    pixelica_mat4_scale(squash, 1.0f, 0.55f, 1.0f);
    pixelica_mat4_multiply(model, local, squash);

    mesh.vertices = k_vertices;
    mesh.vertex_count = VERT_COUNT;
    mesh.indices = k_indices;
    mesh.index_count = INDEX_COUNT;
    mesh.color = PIXELICA_EXAMPLE_RGB565(220, 210, 200);

    lighting.normals = k_normals;
    lighting.lights = s->lights;
    lighting.light_count = 2u;
    lighting.ambient_r = 30u;
    lighting.ambient_g = 34u;
    lighting.ambient_b = 48u;
    lighting.two_sided = false;

    if (s->lights_on) {
        status = pixelica_draw_mesh_lit(ctx, &mesh, model, NULL, NULL, NULL,
                                        &lighting);
    } else {
        /* Exactly the old unlit path, unchanged. */
        status = pixelica_draw_mesh(ctx, &mesh, model);
    }
    if (status != PIXELICA_OK) {
        return status;
    }
    written_after_cube = pixelica_frame_stats(ctx).pixels_written;
    s->lit_pixels = written_after_cube;

    /* Two overlapping additive glows: order does not matter and the
     * overlap saturates instead of wrapping. Neither writes depth. */
    status = draw_glow(ctx, -0.6f, PIXELICA_EXAMPLE_RGB565(255, 120, 40), 90u);
    if (status != PIXELICA_OK) {
        return status;
    }
    status = draw_glow(ctx, 0.6f, PIXELICA_EXAMPLE_RGB565(40, 160, 255), 90u);
    if (status != PIXELICA_OK) {
        return status;
    }
    s->glow_pixels = pixelica_frame_stats(ctx).pixels_written - written_after_cube;
    return pixelica_end_frame(ctx);
}

static void lit_shutdown(void *userdata)
{
    const LitState *s = (const LitState *)userdata;
    float weight = 1.0f;

    (void)pixelica_clip_player_weight(&s->player, &weight);
    printf("hello_lighting: ticks=%lu clip=%s fade_weight=%.2f "
           "steps=%lu/%lu loops=%lu crossfades=%lu skipped=%lu "
           "lit_pixels=%lu glow_pixels=%lu\n",
           (unsigned long)s->ticks, s->faded ? "run" : "walk", (double)weight,
           (unsigned long)s->steps_left, (unsigned long)s->steps_right,
           (unsigned long)s->loops, (unsigned long)s->crossfades,
           (unsigned long)s->player.skipped_repetitions,
           (unsigned long)s->lit_pixels, (unsigned long)s->glow_pixels);
}

const PIXELICA_Game pixelica_example_game = {
    "Pixelica hello_lighting",
    {256u, 192u, 16u, 64u, 65536u},
    lit_init,
    lit_update,
    lit_render,
    lit_shutdown,
};
