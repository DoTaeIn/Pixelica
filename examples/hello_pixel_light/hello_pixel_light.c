/*
 * hello_pixel_light: the Pixel Room scene lit with the reusable pixel
 * lighting tools (pixelica/light.h and the two 3D draw helpers).
 *
 *   warm window light   the window OPENING is projected along the sun
 *                       direction onto the floor (pixelica_light_quad_project),
 *                       drawn additively with a window-pane mask, plus a
 *                       softer wash on the wall around the window.
 *   contact shadows     one soft blob per prop and under the player cube,
 *                       each on the surface the object rests on (the floor,
 *                       or the desk top for the book and the cup).
 *   controlled glow     an additive quad over the glass, recorded into an
 *                       EXPLICIT emissive mask by pixelica_emissive_mask_pixel
 *                       and resolved against the finished depth buffer, then
 *                       bloomed with pixelica_bloom_apply.
 *
 * It owns its own context (no platform, no window), renders the room's scene
 * itself so the light draws land inside the frame, and reports a deterministic
 * one-line summary plus per-stage timings. Nothing here changes Pixel Room.
 *
 *   hello_pixel_light --frames 30 --effects both
 *   hello_pixel_light --ppm lit.ppm --ppm-base base.ppm --scale 2
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pixelica/light.h"
#include "pixelica/pixelica.h"

#include "pixel_room.h"
#include "room_embedded.h"

#define MAX_WIDTH 256u
#define MAX_HEIGHT 192u
#define MAX_PIXELS (MAX_WIDTH * MAX_HEIGHT)
/* The emitter (the sun in the glass) is only a dozen pixels across, so the
 * glow is sampled at half resolution instead of a quarter: a 1/4 downsample
 * averages a source this small away (docs/pixel_light.md, "measured limits"). */
#define BLOOM_DOWNSAMPLE 2u
#define BLOOM_SCRATCH (6u * ((MAX_WIDTH + BLOOM_DOWNSAMPLE - 1u) / BLOOM_DOWNSAMPLE) * \
                       ((MAX_HEIGHT + BLOOM_DOWNSAMPLE - 1u) / BLOOM_DOWNSAMPLE))

/*
 * The north-wall window, as built by examples/pixel_room/pixel_room_world.c
 * (k_window_opening = {4.5, 6.5, 1.0, 2.3} with s = x + 4, glass node z in
 * [-4.1, -3.8]). Kept here as named constants instead of reaching into the
 * room's private geometry.
 */
#define WINDOW_X0 0.5f
#define WINDOW_X1 2.5f
#define WINDOW_Y0 1.0f
#define WINDOW_Y1 2.3f
#define WINDOW_Z (-3.9f)
/* The painted sun inside the glass: x 1.7..2.1, y 1.9..2.2 at z = -4.07,
 * behind the mullions (-3.97). The glow sits just in front of it. */
#define SUN_X 1.9f
#define SUN_Y 2.05f
#define SUN_Z (-4.05f)

#define WINDOW_MASK_W 48u
#define WINDOW_MASK_H 32u
#define WASH_MASK_W 32u
#define WASH_MASK_H 24u
#define BLOB_MASK_W 24u
#define BLOB_MASK_H 24u

/* Prebuilt shapes and tints: built once, reused by every patch and shadow. */
typedef struct LightKit {
    uint8_t window_alpha[WINDOW_MASK_W * WINDOW_MASK_H];
    uint16_t window_tint[WINDOW_MASK_W * WINDOW_MASK_H];
    uint16_t glow_tint[BLOB_MASK_W * BLOB_MASK_H]; /* shares the ellipse mask */
    uint8_t wash_alpha[WASH_MASK_W * WASH_MASK_H];
    uint16_t wash_tint[WASH_MASK_W * WASH_MASK_H];
    uint8_t blob_alpha[BLOB_MASK_W * BLOB_MASK_H];
    uint16_t blob_tint[BLOB_MASK_W * BLOB_MASK_H];
    PIXELICA_LightMask window, wash, blob;
    PIXELICA_Texture window_tex, glow_tex, wash_tex, blob_tex;
} LightKit;

typedef struct Frame {
    uint16_t snapshot[MAX_PIXELS];
    uint8_t emissive[MAX_PIXELS];
    uint16_t emissive_depth[MAX_PIXELS];
    uint16_t scratch[BLOOM_SCRATCH];
} Frame;

typedef struct Report {
    uint32_t patches, shadows, glows;
    uint32_t shadow_px, patch_px, glow_px; /* fragments each stage wrote */
    uint32_t emissive_pixels, occluded_emitters, bloom_pixels;
    uint64_t hash;
    double scene_ms, light_ms, bloom_ms;
    bool draws_ok, bloom_only_adds;
} Report;

static LightKit g_kit;
static Frame g_frame;
static uint16_t g_pre_bloom[MAX_PIXELS];
static uint16_t g_base_image[MAX_PIXELS];

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static uint64_t hash_pixels(const uint16_t *pixels, size_t count)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;

    for (i = 0; i < count; ++i) {
        h ^= (uint64_t)(pixels[i] & 0xFFu);
        h *= 1099511628211ull;
        h ^= (uint64_t)(pixels[i] >> 8);
        h *= 1099511628211ull;
    }
    return h;
}

static double ms_since(clock_t start)
{
    return (double)(clock() - start) * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* ---- the light kit ------------------------------------------------------- */

static bool kit_init(LightKit *kit)
{
    PIXELICA_LightMaskDesc desc;

    /* The window: panes with bars, fading with distance from the opening. */
    if (pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_RECT, &desc) != PIXELICA_OK) {
        return false;
    }
    desc.width = (uint16_t)WINDOW_MASK_W;
    desc.height = (uint16_t)WINDOW_MASK_H;
    desc.softness = 0.26f;
    desc.fade_v = 0.35f;
    desc.peak = 215u;
    desc.panes_x = 2u;
    desc.panes_y = 3u;
    desc.bar = 0.13f;
    if (pixelica_light_mask_build(&desc, kit->window_alpha, sizeof kit->window_alpha,
                                  &kit->window) != PIXELICA_OK) {
        return false;
    }
    /* A broad soft rectangle for the wall around the window. */
    desc.width = (uint16_t)WASH_MASK_W;
    desc.height = (uint16_t)WASH_MASK_H;
    desc.softness = 0.95f;
    desc.fade_v = 0.25f;
    desc.peak = 110u;
    desc.panes_x = 1u;
    desc.panes_y = 1u;
    desc.bar = 0.0f;
    if (pixelica_light_mask_build(&desc, kit->wash_alpha, sizeof kit->wash_alpha,
                                  &kit->wash) != PIXELICA_OK) {
        return false;
    }
    /* One blob for every contact shadow: softness and darkness live here. */
    if (pixelica_light_mask_default(PIXELICA_LIGHT_SHAPE_ELLIPSE, &desc) != PIXELICA_OK) {
        return false;
    }
    desc.width = (uint16_t)BLOB_MASK_W;
    desc.height = (uint16_t)BLOB_MASK_H;
    desc.softness = 0.80f;
    desc.peak = 205u;
    if (pixelica_light_mask_build(&desc, kit->blob_alpha, sizeof kit->blob_alpha,
                                  &kit->blob) != PIXELICA_OK) {
        return false;
    }

    return pixelica_light_tint_fill(PIXELICA_RGB565(255, 196, 120), (uint16_t)WINDOW_MASK_W,
                                    (uint16_t)WINDOW_MASK_H, kit->window_tint,
                                    WINDOW_MASK_W * WINDOW_MASK_H, &kit->window_tex) ==
               PIXELICA_OK &&
           pixelica_light_tint_fill(PIXELICA_RGB565(255, 232, 176), (uint16_t)BLOB_MASK_W,
                                    (uint16_t)BLOB_MASK_H, kit->glow_tint,
                                    BLOB_MASK_W * BLOB_MASK_H, &kit->glow_tex) ==
               PIXELICA_OK &&
           pixelica_light_tint_fill(PIXELICA_RGB565(255, 180, 110), (uint16_t)WASH_MASK_W,
                                    (uint16_t)WASH_MASK_H, kit->wash_tint,
                                    WASH_MASK_W * WASH_MASK_H, &kit->wash_tex) ==
               PIXELICA_OK &&
           pixelica_light_tint_fill(PIXELICA_RGB565(18, 16, 40), (uint16_t)BLOB_MASK_W,
                                    (uint16_t)BLOB_MASK_H, kit->blob_tint,
                                    BLOB_MASK_W * BLOB_MASK_H, &kit->blob_tex) ==
               PIXELICA_OK;
}

/* ---- the lit frame ------------------------------------------------------- */

/* Orbit camera of the room (same framing as pixel_room's own view). */
static void camera_view(float out[16], float yaw)
{
    float t_target[16], ry[16], rx[16], t_dist[16], a[16], b[16];

    pixelica_mat4_translation(t_target, 0.0f, -1.0f, 0.0f);
    pixelica_mat4_rotation_y(ry, -yaw);
    pixelica_mat4_rotation_x(rx, 0.62f);
    pixelica_mat4_translation(t_dist, 0.0f, 0.0f, -13.5f);
    pixelica_mat4_multiply(a, ry, t_target);
    pixelica_mat4_multiply(b, rx, a);
    pixelica_mat4_multiply(out, t_dist, b);
}

/*
 * One soft blob on the surface the box rests on (its own min.y), a little
 * wider than the footprint and leaning away from the light.
 *
 * This is a CONTACT shadow, not a cast shadow: it hugs the base of the object
 * so the object stops floating. The lean is deliberately a fraction of the
 * height (not the full 1/tan(elevation) a cast shadow would use), because a
 * blob slid that far reads as a stain on the floor rather than as the
 * object's own shadow - and nothing here knows the object's silhouette.
 */
#define SHADOW_LEAN 0.30f
#define SHADOW_SPREAD 1.55f

static bool draw_box_shadow(PIXELICA_Context *ctx, const LightKit *kit,
                            const PIXELICA_AABB *box, uint8_t opacity,
                            PIXELICA_Vec3 sun_ground)
{
    PIXELICA_ContactShadow shadow;
    float half_x = 0.5f * (box->max.x - box->min.x);
    float half_z = 0.5f * (box->max.z - box->min.z);
    float height = box->max.y - box->min.y;
    float radius = 0.5f * (half_x + half_z);
    float slide;

    if (!(half_x > 0.0f) || !(half_z > 0.0f) || !(height > 0.0f)) {
        return true; /* a degenerate prop simply gets no shadow */
    }
    slide = height * SHADOW_LEAN;
    memset(&shadow, 0, sizeof shadow);
    shadow.center = vec3(0.5f * (box->min.x + box->max.x) + sun_ground.x * slide,
                         box->min.y,
                         0.5f * (box->min.z + box->max.z) + sun_ground.z * slide);
    shadow.normal = vec3(0.0f, 1.0f, 0.0f);
    shadow.u_axis = sun_ground; /* stretch along the light */
    shadow.radius_u = radius * SHADOW_SPREAD + slide;
    shadow.radius_v = radius * SHADOW_SPREAD;
    /* The depth buffer is nonlinear: at this camera distance one depth16 step
     * is about 0.003 world units, so a 0.03 lift is ~10 steps - enough for
     * the blob to win the strict depth test everywhere, and invisible. */
    shadow.bias = 0.03f;
    shadow.tint = kit->blob_tex;
    shadow.mask = kit->blob;
    shadow.opacity = opacity;
    return pixelica_draw_contact_shadow(ctx, &shadow, NULL) == PIXELICA_OK;
}

/*
 * Draws the room and, when `effects` is set, the light patches, the contact
 * shadows and the emissive glow. The emissive mask is recorded by the glow
 * draw itself.
 */
static bool render_frame(PIXELICA_Context *ctx, const PixelRoom *room, const LightKit *kit,
                         bool effects, PIXELICA_EmissiveMask *emissive, Report *report)
{
    float projection[16], view[16], view_projection[16];
    PIXELICA_MeshEffects smooth, glow_effects;
    PIXELICA_LightPlacement opening;
    PIXELICA_LightDraw patch;
    /* Low afternoon sun through the north window: it travels down, slightly
     * to -X and well into the room (+Z), so the pool reaches the middle of
     * the floor instead of hugging the wall. */
    const PIXELICA_Vec3 sun = {-0.30f, -0.48f, 0.82f};
    clock_t start;
    uint16_t width = 0u, height = 0u;
    uint32_t i, written = 0u;
    bool ok = true;

    {
        PIXELICA_FrameView probe;

        if (pixelica_frame_view(ctx, &probe) != PIXELICA_OK) {
            return false;
        }
        width = probe.width;
        height = probe.height;
    }
    if (pixelica_mat4_perspective(projection, 0.8f, (float)width / (float)height, 1.0f,
                                  40.0f) != PIXELICA_OK) {
        return false;
    }
    camera_view(view, PIXEL_ROOM_START_CAMERA_YAW);
    pixelica_mat4_multiply(view_projection, projection, view);

    memset(&smooth, 0, sizeof smooth);
    smooth.filter = PIXELICA_FILTER_BILINEAR; /* soft patch edges */
    glow_effects = smooth;
    glow_effects.pixel = pixelica_emissive_mask_pixel;
    glow_effects.userdata = emissive;

    start = clock();
    pixelica_begin_frame(ctx, PIXELICA_RGB565(22, 26, 40));
    pixelica_set_view_projection(ctx, view_projection);
    if (pixelica_scene_render(pixel_room_scene(room), ctx) != PIXELICA_OK) {
        (void)pixelica_end_frame(ctx);
        return false;
    }
    report->scene_ms += ms_since(start);
    if (!effects) {
        return pixelica_end_frame(ctx) == PIXELICA_OK;
    }

    start = clock();
    written = pixelica_frame_stats(ctx).pixels_written;
    /* 1. Contact shadows first: they darken the receiver, the light below
     *    then adds on top. Neither writes depth, so the order is ours. */
    {
        /* The light's direction along the ground, which is the way every
         * blob leans. */
        float ground_len = sqrtf(sun.x * sun.x + sun.z * sun.z);
        PIXELICA_Vec3 sun_ground = vec3(sun.x / ground_len, 0.0f, sun.z / ground_len);

        for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
            PIXELICA_AABB bounds;

            if (pixel_room_prop_bounds(room, i, &bounds)) {
                ok = draw_box_shadow(ctx, kit, &bounds, 190u, sun_ground) && ok;
                ++report->shadows;
            }
        }
        {
            PIXELICA_AABB player = pixel_room_player_bounds(room);

            ok = draw_box_shadow(ctx, kit, &player, 185u, sun_ground) && ok;
            ++report->shadows;
        }
    }
    report->shadow_px += pixelica_frame_stats(ctx).pixels_written - written;
    written = pixelica_frame_stats(ctx).pixels_written;

    /* 2. The window opening projected onto the floor along the sun. */
    memset(&opening, 0, sizeof opening);
    opening.origin = vec3(WINDOW_X0, WINDOW_Y0, WINDOW_Z);
    opening.u_axis = vec3(WINDOW_X1 - WINDOW_X0, 0.0f, 0.0f);
    opening.v_axis = vec3(0.0f, WINDOW_Y1 - WINDOW_Y0, 0.0f);
    opening.normal = vec3(0.0f, 0.0f, 1.0f);
    memset(&patch, 0, sizeof patch);
    patch.tint = kit->window_tex;
    patch.mask = kit->window;
    patch.mode = PIXELICA_ALPHA_ADD;
    patch.opacity = 235u;
    if (pixelica_light_quad_project(&opening, sun, vec3(0.0f, 0.0f, 0.0f),
                                    vec3(0.0f, 1.0f, 0.0f), 0.03f,
                                    &patch.quad) == PIXELICA_OK) {
        ok = pixelica_draw_light(ctx, &patch, &smooth) == PIXELICA_OK && ok;
        ++report->patches;
    } else {
        ok = false;
    }

    /* 3. A wash on the wall around the window (receiver: the north wall). */
    patch.tint = kit->wash_tex;
    patch.mask = kit->wash;
    patch.opacity = 120u;
    if (pixelica_light_quad_centered(vec3(1.5f, 1.65f, -3.96f), vec3(0.0f, 0.0f, 1.0f),
                                     vec3(1.0f, 0.0f, 0.0f), 2.1f, 1.35f, 0.0f,
                                     &patch.quad) == PIXELICA_OK) {
        ok = pixelica_draw_light(ctx, &patch, &smooth) == PIXELICA_OK && ok;
        ++report->patches;
    } else {
        ok = false;
    }

    report->patch_px += pixelica_frame_stats(ctx).pixels_written - written;
    written = pixelica_frame_stats(ctx).pixels_written;

    /* 4. ONLY the painted sun glows, not the whole pane. That is the point of
     *    an explicit emissive mask: the sky, the cloud and the mullions keep
     *    their own pixels while a small, genuinely bright spot blooms. The
     *    glow sits behind the mullions, so the frame still cuts across it.
     *    It is drawn LAST, so nothing opaque can cover it afterwards; the
     *    resolve pass enforces that instead of assuming it. */
    patch.tint = kit->glow_tex;
    patch.mask = kit->blob;
    patch.opacity = 130u;
    if (pixelica_light_quad_centered(vec3(SUN_X, SUN_Y, SUN_Z), vec3(0.0f, 0.0f, 1.0f),
                                     vec3(1.0f, 0.0f, 0.0f), 0.30f, 0.25f, 0.0f,
                                     &patch.quad) == PIXELICA_OK) {
        ok = pixelica_draw_light(ctx, &patch, &glow_effects) == PIXELICA_OK && ok;
        ++report->glows;
    } else {
        ok = false;
    }
    report->glow_px += pixelica_frame_stats(ctx).pixels_written - written;
    report->light_ms += ms_since(start);

    return pixelica_end_frame(ctx) == PIXELICA_OK && ok;
}

/* ---- PPM ----------------------------------------------------------------- */

static bool write_ppm(const char *path, const uint16_t *pixels, uint32_t width,
                      uint32_t height, uint32_t scale)
{
    static uint8_t row[MAX_WIDTH * 8u * 3u];
    FILE *f = fopen(path, "wb");
    uint32_t x, y, s;
    bool ok = true;

    if (f == NULL) {
        fprintf(stderr, "cannot open '%s' for writing\n", path);
        return false;
    }
    fprintf(f, "P6\n%" PRIu32 " %" PRIu32 "\n255\n", width * scale, height * scale);
    for (y = 0; y < height && ok; ++y) {
        uint8_t *out = row;

        for (x = 0; x < width; ++x) {
            uint32_t c = pixels[(size_t)y * width + x];
            uint32_t r5 = (c >> 11) & 0x1Fu, g6 = (c >> 5) & 0x3Fu, b5 = c & 0x1Fu;
            uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

            for (s = 0; s < scale; ++s) {
                *out++ = r;
                *out++ = g;
                *out++ = b;
            }
        }
        for (s = 0; s < scale && ok; ++s) {
            ok = fwrite(row, (size_t)width * scale * 3u, 1, f) == 1u;
        }
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "failed writing '%s'\n", path);
    }
    return ok;
}

/* ---- one pass ------------------------------------------------------------ */

/*
 * Renders `frames` frames and leaves the finished image in g_frame.snapshot.
 * With `effects` every frame is additionally snapshotted and bloomed through
 * the emissive mask that frame recorded, so the reported cost is the real
 * per-frame cost of the effect.
 */
static bool run_pass(PixelRoom *room, PIXELICA_Context *ctx, uint16_t width, uint16_t height,
                     uint32_t frames, bool effects, Report *report)
{
    PIXELICA_EmissiveMask emissive;
    PIXELICA_FrameView live, snapshot;
    PIXELICA_LightSurface surface;
    PIXELICA_LightMask selection;
    PIXELICA_BloomDesc bloom;
    const size_t pixels = (size_t)width * (size_t)height;
    uint32_t frame, i;
    clock_t start;

    memset(&emissive, 0, sizeof emissive);
    emissive.alpha = g_frame.emissive;
    emissive.depth = g_frame.emissive_depth;
    emissive.width = width;
    emissive.height = height;
    emissive.stride = width;

    surface.pixels = g_frame.snapshot;
    surface.width = width;
    surface.height = height;
    surface.stride = width;
    selection.alpha = g_frame.emissive;
    selection.width = width;
    selection.height = height;
    selection.stride = width;
    if (pixelica_bloom_default(&bloom) != PIXELICA_OK) {
        return false;
    }
    bloom.mask = &selection;
    bloom.downsample = (uint8_t)BLOOM_DOWNSAMPLE;
    bloom.radius = 2u;
    bloom.passes = 2u;
    bloom.intensity = 255u;

    report->draws_ok = true;
    report->bloom_only_adds = true;
    for (frame = 0; frame < frames; ++frame) {
        /* A still camera and a still room: every frame is the same work. */
        if (pixelica_emissive_mask_clear(&emissive) != PIXELICA_OK) {
            return false;
        }
        if (!render_frame(ctx, room, &g_kit, effects, &emissive, report)) {
            report->draws_ok = false;
            return false;
        }
        if (pixelica_frame_view(ctx, &live) != PIXELICA_OK ||
            pixelica_frame_copy(&live, g_frame.snapshot, MAX_PIXELS, NULL, 0u, &snapshot) !=
                PIXELICA_OK) {
            return false;
        }
        if (!effects) {
            continue;
        }
        /* Kept only so the check below can prove bloom never darkens a
         * pixel; not part of the measured cost. */
        memcpy(g_pre_bloom, g_frame.snapshot, pixels * sizeof g_pre_bloom[0]);
        start = clock();
        /* Emitters the finished depth buffer shows to be covered do not glow. */
        if (pixelica_emissive_mask_resolve(&emissive, &live, &report->occluded_emitters) !=
            PIXELICA_OK) {
            return false;
        }
        if (pixelica_bloom_apply(&surface, &bloom, g_frame.scratch, BLOOM_SCRATCH) !=
            PIXELICA_OK) {
            return false;
        }
        report->bloom_ms += ms_since(start);
    }

    report->hash = hash_pixels(g_frame.snapshot, pixels);
    if (!effects) {
        return true;
    }
    for (i = 0; i < (uint32_t)pixels; ++i) {
        uint16_t before = g_pre_bloom[i], after = g_frame.snapshot[i];

        if (g_frame.emissive[i] != 0u) {
            ++report->emissive_pixels;
        }
        /* Bloom must only ever ADD: a pixel it does not reach keeps its
         * exact value, which is what keeps the pixel art sharp. */
        if (((after >> 11) & 31u) < ((before >> 11) & 31u) ||
            ((after >> 5) & 63u) < ((before >> 5) & 63u) ||
            (after & 31u) < (before & 31u)) {
            report->bloom_only_adds = false;
        }
        if (after != before) {
            ++report->bloom_pixels;
        }
    }
    return true;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(FILE *out)
{
    fputs("usage: hello_pixel_light [--frames N] [--effects on|off|both] [--tiny]\n"
          "                         [--ppm FILE] [--ppm-base FILE] [--scale N]\n"
          "  --effects both (default) renders the scene twice and compares\n"
          "  --ppm / --ppm-base write the lit / unlit image as binary PPM\n",
          out);
}

int main(int argc, char **argv)
{
    PixelRoomParams params;
    PixelRoom *room = NULL;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Config cfg;
    Report lit, base;
    const char *ppm = NULL, *ppm_base = NULL;
    uint16_t width = (uint16_t)PIXEL_ROOM_STANDARD_WIDTH;
    uint16_t height = (uint16_t)PIXEL_ROOM_STANDARD_HEIGHT;
    uint32_t frames = 30u, scale = 1u;
    bool want_lit = true, want_base = true;
    int rc = 0, i;
    uint32_t changed = 0u, p;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--tiny") == 0) {
            width = (uint16_t)PIXEL_ROOM_TINY_WIDTH;
            height = (uint16_t)PIXEL_ROOM_TINY_HEIGHT;
        } else if (strcmp(arg, "--frames") == 0 && value != NULL) {
            long n = strtol(value, NULL, 10);

            if (n < 1 || n > 100000) {
                fprintf(stderr, "--frames must be 1..100000\n");
                return 2;
            }
            frames = (uint32_t)n;
            ++i;
        } else if (strcmp(arg, "--scale") == 0 && value != NULL) {
            long n = strtol(value, NULL, 10);

            if (n < 1 || n > 8) {
                fprintf(stderr, "--scale must be 1..8\n");
                return 2;
            }
            scale = (uint32_t)n;
            ++i;
        } else if (strcmp(arg, "--effects") == 0 && value != NULL) {
            want_lit = strcmp(value, "off") != 0;
            want_base = strcmp(value, "on") != 0;
            if (!want_lit && !want_base) {
                fprintf(stderr, "--effects must be on, off or both\n");
                return 2;
            }
            ++i;
        } else if (strcmp(arg, "--ppm") == 0 && value != NULL) {
            ppm = value;
            ++i;
        } else if (strcmp(arg, "--ppm-base") == 0 && value != NULL) {
            ppm_base = value;
            ++i;
        } else {
            fprintf(stderr, "unknown or incomplete argument '%s' (try --help)\n", arg);
            return 2;
        }
    }

    memset(&params, 0, sizeof params);
    if (!pixel_room_embedded_assets(&params.assets)) {
        fprintf(stderr, "embedded Pixel Room assets are missing\n");
        return 1;
    }
    params.width = width;
    params.height = height;
    if (pixel_room_create(&room, &params) != PIXELICA_OK || room == NULL) {
        fprintf(stderr, "pixel_room_create failed\n");
        return 1;
    }
    /* Stand the cube in the pool of window light. */
    (void)pixel_room_teleport_player(room, 0.6f, -1.3f);

    cfg = pixel_room_context_config(width, height);
    if (pixelica_create(&cfg, NULL, &ctx) != PIXELICA_OK) {
        fprintf(stderr, "pixelica_create failed\n");
        pixel_room_destroy(room);
        return 1;
    }
    if (!kit_init(&g_kit)) {
        fprintf(stderr, "building the light masks failed\n");
        pixelica_destroy(ctx);
        pixel_room_destroy(room);
        return 1;
    }

    memset(&lit, 0, sizeof lit);
    memset(&base, 0, sizeof base);
    if (want_base) {
        if (!run_pass(room, ctx, width, height, frames, false, &base)) {
            fprintf(stderr, "the unlit pass failed\n");
            rc = 1;
        }
        memcpy(g_base_image, g_frame.snapshot, (size_t)width * height * sizeof g_base_image[0]);
    }
    if (rc == 0 && want_lit) {
        if (!run_pass(room, ctx, width, height, frames, true, &lit)) {
            fprintf(stderr, "the lit pass failed\n");
            rc = 1;
        }
    }

    if (rc == 0) {
        bool ok = true;

        printf("hello_pixel_light: %ux%u frames=%" PRIu32 " patches=%" PRIu32
               " shadows=%" PRIu32 " glow=%" PRIu32 "\n",
               (unsigned)width, (unsigned)height, frames,
               lit.patches / (want_lit ? frames : 1u),
               lit.shadows / (want_lit ? frames : 1u), lit.glows / (want_lit ? frames : 1u));
        if (want_lit && want_base) {
            for (p = 0; p < (uint32_t)((size_t)width * height); ++p) {
                if (g_base_image[p] != g_frame.snapshot[p]) {
                    ++changed;
                }
            }
            printf("hello_pixel_light: base_fnv1a=%016" PRIx64 " lit_fnv1a=%016" PRIx64
                   " changed_px=%" PRIu32 "\n",
                   base.hash, lit.hash, changed);
            ok = ok && changed > 0u && base.hash != lit.hash;
        }
        if (want_lit) {
            printf("hello_pixel_light: shadow_px=%" PRIu32 " patch_px=%" PRIu32
                   " glow_px=%" PRIu32 " emissive_px=%" PRIu32 " occluded_emitters=%" PRIu32
                   " bloom_px=%" PRIu32 "\n",
                   lit.shadow_px / frames, lit.patch_px / frames, lit.glow_px / frames,
                   lit.emissive_pixels, lit.occluded_emitters, lit.bloom_pixels);
            printf("hello_pixel_light: ms/frame scene=%.3f light=%.3f bloom=%.3f\n",
                   lit.scene_ms / (double)frames, lit.light_ms / (double)frames,
                   lit.bloom_ms / (double)frames);
            ok = ok && lit.draws_ok && lit.bloom_only_adds && lit.emissive_pixels > 0u &&
                 lit.bloom_pixels > 0u && lit.patches == 2u * frames &&
                 lit.shadows == (PIXEL_ROOM_PROP_COUNT + 1u) * frames &&
                 lit.glows == frames && lit.shadow_px > 0u && lit.patch_px > 0u &&
                 lit.glow_px > 0u;
        }
        if (want_base) {
            printf("hello_pixel_light: ms/frame unlit scene=%.3f\n",
                   base.scene_ms / (double)frames);
        }
        if (ppm != NULL && want_lit) {
            ok = write_ppm(ppm, g_frame.snapshot, width, height, scale) && ok;
        }
        if (ppm_base != NULL && want_base) {
            ok = write_ppm(ppm_base, g_base_image, width, height, scale) && ok;
        }
        printf("hello_pixel_light: ok=%d\n", ok ? 1 : 0);
        rc = ok ? 0 : 1;
    }

    pixelica_destroy(ctx);
    pixel_room_destroy(room);
    return rc;
}
