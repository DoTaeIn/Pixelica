/*
 * hello_water: a raft riding waves on the optional water module.
 *
 * It owns its own context and its own fixed-step loop (no platform, no
 * window), because the point is to show the GAME driving the physics:
 *
 *   1. CALM phase. The surface has no waves at all. The raft is released
 *      above its equilibrium draft and must settle to it, level, and stop.
 *      pixelica_water_equilibrium_draft() says where that is, and the demo
 *      checks it got there.
 *   2. WAVE phase. The same raft, the same rig, the surface switched to the
 *      module's default two-wave sea. The raft heaves and TILTS, because the
 *      four corner probes sit at different depths and r x F therefore makes a
 *      torque - no tilt heuristic anywhere.
 *
 * Every frame, in this order:
 *      pixelica_water_mesh_build()   the drawn surface, at time t
 *      pixelica_water_load()         the forces, at the SAME time t
 *      pixelica_water_body_step()    one explicit 1/60 s step, game-controlled
 *      draw the water, draw the raft
 *
 * Both the mesh and the load go through pixelica_water_sample() with the same
 * double t, so the water that is drawn and the water that pushes the raft are
 * one surface. The demo proves that instead of asserting it: every frame it
 * compares, at each raft corner, the ANALYTIC height the physics used against
 * the DRAWN height pixelica_water_mesh_height() reports for the tessellated
 * mesh, and fails if the gap exceeds what that grid resolution can be held to.
 *
 *   hello_water --frames 420
 *   hello_water --frames 420 --out-dir output/water --scale 2
 *   hello_water --frames 420 --time-offset 1e6   (a long-running session)
 *
 * It prints a deterministic summary and "hello_water: ok=1" only when every
 * check held, which is what the CTest entry matches.
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pixelica/water3d.h"

#define VIEW_WIDTH 256u
#define VIEW_HEIGHT 192u
#define VIEW_PIXELS (VIEW_WIDTH * VIEW_HEIGHT)

/* The drawn patch: 24 m across in 48 cells, so a 0.5 m cell. */
#define PATCH_SIZE 24.0f
#define PATCH_CELLS 48u
#define MESH_VERTS ((PATCH_CELLS + 1u) * (PATCH_CELLS + 1u))
#define MESH_INDICES (PATCH_CELLS * PATCH_CELLS * 6u)

/*
 * The bound on |analytic height - drawn height| inside one cell. Linear
 * interpolation of a function whose second derivative is bounded by C over a
 * span L errs by at most C * L^2 / 8; the default sea has
 * C = sum A_i k_i^2 = 0.095*(2pi/8)^2 + 0.058*(2pi/4.5)^2 = 0.172 1/m and the
 * longest in-cell span is the 0.707 m diagonal, giving 0.011 m. The check
 * allows 0.02 m, and the demo reports the figure it actually measured.
 */
#define RENDER_GAP_LIMIT 0.02f

/*
 * The raft: a 2 x 2 m pontoon, 0.35 m thick, 800 kg (571 kg/m^3).
 *
 * Its draft is mass / (density * waterplane area) = 800 / (1000 * 4) = 0.20 m
 * of the 0.35 m thickness, so each probe has 0.20 m of margin before it can
 * leave the water and 0.15 m before it saturates. The default sea reaches
 * +/-0.153 m and tilts the raft by up to ~0.08 rad, which lifts a corner by a
 * further 0.08 * 0.8 = 0.06 m; 0.20 m of draft covers both with room left, so
 * the raft stays CONVINCINGLY buoyant - all four probes wet at every frame of
 * the wave phase - which is what makes the demo worth looking at.
 *
 * (The same hull at 560 kg drafts 0.14 m and lifts a corner clear on the
 * steepest troughs; at 240 kg it drafts 0.06 m and leaves the water bodily.
 * Both are real behaviour of a light hull in a swell, and the module reports
 * them through submerged_probes - they are just not what this demo shows.)
 *
 * Why these numbers are stable at 60 Hz, using the condition stated in
 * pixelica_water_body_step():
 *   heave  omega_n = sqrt(rho*g*A/m) = sqrt(1000*9.81*4/800) = 7.00 rad/s
 *          wetted area at rest = A * 0.20/0.35 = 2.29 m^2
 *          dt < 2/omega_n = 0.286 s   and   dt < 2m/(c_n*A_wet) = 0.233 s
 *   roll   I_x = m/12*(0.35^2 + 2^2) = 275 kg m^2
 *          stiffness = rho*g*sum(area_i * r_i^2) = 25114 N m/rad
 *          omega_roll = 9.56 rad/s, dt < 2/omega_roll = 0.209 s
 *          dt < 2*I_x/(c_n*sum(A_wet_i*r_i^2)) = 0.125 s
 * The step is 1/60 = 0.0167 s, so the tightest bound has 7x margin. The wave
 * periods (2.67 s and 2.05 s) are far longer than the raft's own heave and
 * roll periods (0.90 s and 0.66 s), which is why it tracks the surface
 * quasi-statically instead of lagging it.
 */
#define RAFT_HALF_X 1.0f
#define RAFT_HALF_Y 0.175f
#define RAFT_HALF_Z 1.0f
#define RAFT_MASS 800.0f
#define RAFT_PROBE_SPAN 0.8f
#define PROBE_COUNT 4u
#define PROBE_AREA 1.0f       /* 4 x 1 m^2 = the 4 m^2 waterplane */
#define PROBE_MAX_DEPTH 0.35f /* the board thickness */
#define PROBE_DRAG_NORMAL 3000.0f
#define PROBE_DRAG_TANGENTIAL 500.0f

#define FIXED_DT (1.0f / 60.0f)
#define CALM_FRAMES 180u
#define RAFT_DROP 0.30f /* released this far above its equilibrium draft */

/*
 * What the wave phase has to produce for the demo to be believable. The
 * default sea's effective slope across the 1.6 m probe span is
 *   roll  2*0.095*sin(0.762*0.8)/1.6 + 2*0.058*sin(0.464*0.8)/1.6 = 0.095 rad
 *   pitch 2*0.095*sin(0.190*0.8)/1.6 + 2*0.058*sin(1.318*0.8)/1.6 = 0.085 rad
 * when the two waves align, and the raft tracks that quasi-statically (its
 * roll period is 0.55 s against the waves' 2.5-3.0 s). The thresholds below
 * are a little under half of those maxima, so they assert a real response
 * without demanding a perfect phase alignment inside the run.
 */
#define TILT_MIN 0.025           /* rad, both roll and pitch must exceed this */
#define TILT_MAX 0.40            /* rad, and neither may approach capsizing */
#define HEAVE_MIN 0.01           /* m, it must actually move up and down */
#define HEAVE_MAX 0.50           /* m */
#define DEPTH_MARGIN_MIN 0.02    /* m, the shallowest probe-depth mean allowed */
#define DEPTH_FRACTION_MAX 0.80  /* of PROBE_MAX_DEPTH: never near saturation */

/* Fixed-point counts to keep the printed summary identical run to run. */
typedef struct Report {
    uint32_t frames, calm_frames, wave_frames;
    uint32_t draw_failures, load_failures, step_failures, probe_failures;
    /* Wave-phase only: the calm phase begins with the raft released in the
     * air, where 0 submerged probes is the correct answer, so counting that
     * frame here would say nothing about whether the raft rides the sea. */
    uint32_t min_submerged, max_submerged;
    uint32_t drop_min_submerged; /* the calm drop, reported for information */
    double draft, probe_depth_eq;
    double calm_draft_error, calm_tilt, calm_depth_error;
    double max_roll, max_pitch, max_heave;
    double min_mean_depth, max_mean_depth, mean_depth_sum;
    double max_render_gap;
    /* Wall-clock cost of each stage, summed over the run (ms). */
    double mesh_ms, physics_ms, draw_ms;
    uint64_t hash_calm, hash_wave_early, hash_wave_late;
    int captured_calm, captured_early, captured_late;
} Report;

static PIXELICA_Vec3 g_mesh_vertices[MESH_VERTS];
static PIXELICA_Vec3 g_mesh_normals[MESH_VERTS];
static uint16_t g_mesh_indices[MESH_INDICES];
static uint16_t g_snapshot[VIEW_PIXELS];
static PIXELICA_WaterProbe g_probes[PROBE_COUNT];

/* ---- small helpers ------------------------------------------------------- */

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static double ms_since(clock_t start)
{
    return (double)(clock() - start) * 1000.0 / (double)CLOCKS_PER_SEC;
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

static bool write_ppm(const char *path, const uint16_t *pixels, uint32_t width,
                      uint32_t height, uint32_t scale)
{
    static uint8_t row[VIEW_WIDTH * 8u * 3u];
    FILE *f = fopen(path, "wb");
    uint32_t x, y, s;
    bool ok = true;

    if (f == NULL) {
        fprintf(stderr, "hello_water: cannot open '%s' for writing\n", path);
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
        fprintf(stderr, "hello_water: failed writing '%s'\n", path);
    }
    return ok;
}

/* ---- the raft ------------------------------------------------------------ */

/* 4 vertices per face so every face keeps its own flat normal. */
static PIXELICA_Vec3 g_raft_vertices[24];
static PIXELICA_Vec3 g_raft_normals[24];
static uint16_t g_raft_indices[36];

static void raft_build(void)
{
    const float x = RAFT_HALF_X, y = RAFT_HALF_Y, z = RAFT_HALF_Z;
    const PIXELICA_Vec3 corners[24] = {
        /* +Z */ {-x, -y, z},  {x, -y, z},   {x, y, z},    {-x, y, z},
        /* -Z */ {x, -y, -z},  {-x, -y, -z}, {-x, y, -z},  {x, y, -z},
        /* +X */ {x, -y, z},   {x, -y, -z},  {x, y, -z},   {x, y, z},
        /* -X */ {-x, -y, -z}, {-x, -y, z},  {-x, y, z},   {-x, y, -z},
        /* +Y */ {-x, y, z},   {x, y, z},    {x, y, -z},   {-x, y, -z},
        /* -Y */ {-x, -y, -z}, {x, -y, -z},  {x, -y, z},   {-x, -y, z},
    };
    const PIXELICA_Vec3 faces[6] = {
        {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
    };
    uint32_t face, i;

    for (i = 0; i < 24u; ++i) {
        g_raft_vertices[i] = corners[i];
        g_raft_normals[i] = faces[i / 4u];
    }
    for (face = 0; face < 6u; ++face) {
        const uint16_t base = (uint16_t)(face * 4u);

        g_raft_indices[face * 6u + 0u] = base;
        g_raft_indices[face * 6u + 1u] = (uint16_t)(base + 1u);
        g_raft_indices[face * 6u + 2u] = (uint16_t)(base + 2u);
        g_raft_indices[face * 6u + 3u] = base;
        g_raft_indices[face * 6u + 4u] = (uint16_t)(base + 2u);
        g_raft_indices[face * 6u + 5u] = (uint16_t)(base + 3u);
    }
}

static void rig_build(PIXELICA_WaterRig *rig)
{
    uint32_t i;

    memset(g_probes, 0, sizeof g_probes);
    for (i = 0; i < PROBE_COUNT; ++i) {
        g_probes[i].offset = vec3((i & 1u) ? RAFT_PROBE_SPAN : -RAFT_PROBE_SPAN,
                                  -RAFT_HALF_Y, /* the underside of the board */
                                  (i & 2u) ? RAFT_PROBE_SPAN : -RAFT_PROBE_SPAN);
        g_probes[i].area = PROBE_AREA;
        g_probes[i].max_depth = PROBE_MAX_DEPTH;
        g_probes[i].drag_normal = PROBE_DRAG_NORMAL;
        g_probes[i].drag_tangential = PROBE_DRAG_TANGENTIAL;
    }
    rig->probes = g_probes;
    rig->probe_count = PROBE_COUNT;
    rig->density = 1000.0f;
    rig->gravity = 9.81f;
}

/* A 2 x 2 x 0.25 m box of uniform density, about its own principal axes. */
static void body_build(PIXELICA_WaterBody *body, float start_y)
{
    const double mx = 2.0 * (double)RAFT_HALF_X, my = 2.0 * (double)RAFT_HALF_Y;
    const double mz = 2.0 * (double)RAFT_HALF_Z;
    const double factor = (double)RAFT_MASS / 12.0;

    memset(body, 0, sizeof *body);
    pixelica_quat_identity(&body->orientation);
    body->position = vec3(0.0f, start_y, 0.0f);
    body->mass = RAFT_MASS;
    body->inertia = vec3((float)(factor * (my * my + mz * mz)),
                         (float)(factor * (mx * mx + mz * mz)),
                         (float)(factor * (mx * mx + my * my)));
}

/* ---- camera -------------------------------------------------------------- */

static void camera_view(float out[16])
{
    float target[16], pitch[16], distance[16], a[16];

    /* High enough to see a stretch of sea rather than a horizon-on sliver,
     * which is the other half of making the swell visible. */
    pixelica_mat4_translation(target, 0.0f, -0.35f, 0.0f);
    pixelica_mat4_rotation_x(pitch, 0.46f);
    pixelica_mat4_translation(distance, 0.0f, 0.0f, -8.5f);
    pixelica_mat4_multiply(a, pitch, target);
    pixelica_mat4_multiply(out, distance, a);
}

/* ---- one frame ----------------------------------------------------------- */

typedef struct Frame {
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterMesh mesh;
    PIXELICA_WaterPatch patch;
    PIXELICA_DirectionalLight lights[2];
    PIXELICA_Lighting water_lighting, raft_lighting;
    float view_projection[16];
} Frame;

static void frame_init(Frame *frame)
{
    float projection[16], view[16];

    memset(&frame->patch, 0, sizeof frame->patch);
    frame->patch.center_x = 0.0f;
    frame->patch.center_z = 0.0f;
    frame->patch.size_x = PATCH_SIZE;
    frame->patch.size_z = PATCH_SIZE;
    frame->patch.cells_x = (uint16_t)PATCH_CELLS;
    frame->patch.cells_z = (uint16_t)PATCH_CELLS;
    frame->patch.color = PIXELICA_RGB565(46, 118, 168);

    memset(&frame->mesh, 0, sizeof frame->mesh);
    frame->mesh.vertices = g_mesh_vertices;
    frame->mesh.normals = g_mesh_normals;
    frame->mesh.indices = g_mesh_indices;
    frame->mesh.vertex_capacity = MESH_VERTS;
    frame->mesh.index_capacity = MESH_INDICES;

    /*
     * A LOW sun. This is the whole trick to making a 3.5 degree swell legible:
     * the Lambert factor is dot(N, L), and for a near-vertical sun the surface
     * normal's tiny tilt barely changes it (cos(3.5 deg) = 0.998 - invisible).
     * With the sun close to the horizon the same tilt swings dot(N, L) by
     * about +/-20% instead, so the wave faces turned toward it brighten and
     * the ones turned away fall to the ambient term.
     */
    frame->lights[0].direction = vec3(-0.80f, -0.35f, -0.49f); /* travels this way */
    frame->lights[0].r = 255u;
    frame->lights[0].g = 236u;
    frame->lights[0].b = 196u;
    frame->lights[1].direction = vec3(0.55f, -0.70f, 0.45f); /* cool sky fill */
    frame->lights[1].r = 58u;
    frame->lights[1].g = 80u;
    frame->lights[1].b = 122u;

    memset(&frame->water_lighting, 0, sizeof frame->water_lighting);
    frame->water_lighting.lights = frame->lights;
    frame->water_lighting.light_count = 2u;
    /* Dark enough that the key light's variation, not the ambient, decides
     * what the surface looks like. */
    frame->water_lighting.ambient_r = 22u;
    frame->water_lighting.ambient_g = 34u;
    frame->water_lighting.ambient_b = 56u;
    /* One-sided on purpose: |dot| would light the away-facing side of every
     * wave exactly like the toward-facing one and flatten the swell back out. */
    frame->water_lighting.two_sided = false;
    frame->raft_lighting = frame->water_lighting;
    frame->raft_lighting.normals = g_raft_normals;
    frame->raft_lighting.ambient_r = 54u;
    frame->raft_lighting.ambient_g = 50u;
    frame->raft_lighting.ambient_b = 46u;

    (void)pixelica_mat4_perspective(projection, 0.85f,
                                    (float)VIEW_WIDTH / (float)VIEW_HEIGHT, 0.4f, 60.0f);
    camera_view(view);
    pixelica_mat4_multiply(frame->view_projection, projection, view);
}

static bool draw_raft(PIXELICA_Context *ctx, const Frame *frame, const PIXELICA_WaterBody *body)
{
    PIXELICA_Mesh mesh;
    float rotation[16], translation[16], model[16];

    if (pixelica_quat_to_mat4(rotation, body->orientation) != PIXELICA_OK) {
        return false;
    }
    pixelica_mat4_translation(translation, body->position.x, body->position.y,
                              body->position.z);
    pixelica_mat4_multiply(model, translation, rotation);
    mesh.vertices = g_raft_vertices;
    mesh.vertex_count = 24u;
    mesh.indices = g_raft_indices;
    mesh.index_count = 36u;
    mesh.color = PIXELICA_RGB565(166, 124, 74);
    return pixelica_draw_mesh_lit(ctx, &mesh, model, NULL, NULL, NULL,
                                  &frame->raft_lighting) == PIXELICA_OK;
}

/*
 * Measures, at every raft corner, how far the DRAWN surface is from the
 * ANALYTIC surface the physics sampled. Returns the largest gap seen, or a
 * negative value when a probe could not be evaluated at all.
 */
static double measure_render_gap(const Frame *frame, const PIXELICA_WaterBody *body,
                                 double time_seconds, uint32_t *out_failures)
{
    float rotation[16];
    double worst = 0.0;
    uint32_t i;

    if (pixelica_quat_to_mat4(rotation, body->orientation) != PIXELICA_OK) {
        ++*out_failures;
        return -1.0;
    }
    for (i = 0; i < PROBE_COUNT; ++i) {
        const PIXELICA_Vec3 o = g_probes[i].offset;
        float world_x = body->position.x + rotation[0] * o.x + rotation[4] * o.y +
                        rotation[8] * o.z;
        float world_z = body->position.z + rotation[2] * o.x + rotation[6] * o.y +
                        rotation[10] * o.z;
        PIXELICA_WaterSample sample;
        float drawn = 0.0f;
        double gap;

        if (pixelica_water_sample(&frame->surface, world_x, world_z, time_seconds, &sample) !=
                PIXELICA_OK ||
            pixelica_water_mesh_height(&frame->mesh, &frame->patch, world_x, world_z,
                                       &drawn) != PIXELICA_OK) {
            ++*out_failures;
            return -1.0;
        }
        gap = fabs((double)sample.height - (double)drawn);
        if (gap > worst) {
            worst = gap;
        }
    }
    return worst;
}

/* The mean analytic depth of the four probes: how deep the raft is sitting. */
static double mean_probe_depth(const Frame *frame, const PIXELICA_WaterBody *body,
                               double time_seconds, uint32_t *out_failures)
{
    float rotation[16];
    double total = 0.0;
    uint32_t i;

    if (pixelica_quat_to_mat4(rotation, body->orientation) != PIXELICA_OK) {
        ++*out_failures;
        return 0.0;
    }
    for (i = 0; i < PROBE_COUNT; ++i) {
        const PIXELICA_Vec3 o = g_probes[i].offset;
        float world_x = body->position.x + rotation[0] * o.x + rotation[4] * o.y +
                        rotation[8] * o.z;
        float world_y = body->position.y + rotation[1] * o.x + rotation[5] * o.y +
                        rotation[9] * o.z;
        float world_z = body->position.z + rotation[2] * o.x + rotation[6] * o.y +
                        rotation[10] * o.z;
        float height = 0.0f;

        if (pixelica_water_height(&frame->surface, world_x, world_z, time_seconds, &height) !=
            PIXELICA_OK) {
            ++*out_failures;
            return 0.0;
        }
        total += (double)height - (double)world_y;
    }
    return total / (double)PROBE_COUNT;
}

/* ---- the run ------------------------------------------------------------- */

typedef struct Options {
    uint32_t frames, scale;
    double time_offset;
    const char *out_dir;
} Options;

static bool capture(const Options *options, PIXELICA_Context *ctx, const char *name,
                    uint64_t *out_hash, int *out_written)
{
    PIXELICA_FrameView live, snapshot;
    char path[512];

    if (pixelica_frame_view(ctx, &live) != PIXELICA_OK ||
        pixelica_frame_copy(&live, g_snapshot, VIEW_PIXELS, NULL, 0u, &snapshot) !=
            PIXELICA_OK) {
        return false;
    }
    *out_hash = hash_pixels(g_snapshot, VIEW_PIXELS);
    if (options->out_dir == NULL) {
        return true;
    }
    if (snprintf(path, sizeof path, "%s/%s.ppm", options->out_dir, name) >= (int)sizeof path) {
        fprintf(stderr, "hello_water: --out-dir path is too long\n");
        return false;
    }
    if (!write_ppm(path, g_snapshot, VIEW_WIDTH, VIEW_HEIGHT, options->scale)) {
        return false;
    }
    printf("hello_water: wrote %s\n", path);
    *out_written = 1;
    return true;
}

/*
 * Runs the whole simulation. When `ctx` is NULL nothing is drawn and no
 * capture is taken: that is the physics-only pass the determinism check uses.
 */
static bool run(const Options *options, PIXELICA_Context *ctx, PIXELICA_WaterBody *body,
                Report *report)
{
    Frame frame;
    PIXELICA_WaterSurface calm, waves;
    float draft = 0.0f;
    uint32_t wave_early, wave_late, n;

    frame_init(&frame);
    rig_build(&frame.rig);
    memset(&calm, 0, sizeof calm); /* wave_count 0: a flat plane at Y = 0 */
    if (pixelica_water_surface_default(&waves) != PIXELICA_OK) {
        return false;
    }
    if (pixelica_water_equilibrium_draft(&frame.rig, RAFT_MASS, &draft) != PIXELICA_OK) {
        fprintf(stderr, "hello_water: the rig cannot float the raft\n");
        return false;
    }
    /* The draft is the depth of the CENTRE OF MASS, which for a flat board
     * floats above the water line and is therefore negative. The submerged
     * depth of a probe is draft - offset.y, and that is the positive number
     * the probe-depth checks below compare against. */
    report->draft = (double)draft;
    report->probe_depth_eq = (double)draft - (double)g_probes[0].offset.y;
    body_build(body, calm.base_height - draft + RAFT_DROP);

    report->calm_frames = CALM_FRAMES < options->frames ? CALM_FRAMES : options->frames;
    report->wave_frames = options->frames - report->calm_frames;
    /* Two captures separated by a third of the remaining run, so they land on
     * genuinely different parts of the swell. */
    wave_early = report->calm_frames + report->wave_frames / 3u;
    wave_late = options->frames - 1u;
    report->min_submerged = PROBE_COUNT + 1u;
    report->drop_min_submerged = PROBE_COUNT + 1u;
    report->min_mean_depth = 1.0e9;
    report->max_mean_depth = -1.0e9;

    for (n = 0; n < options->frames; ++n) {
        /* t is built from the frame index in double, so it neither drifts nor
         * saturates the way a float accumulator would. */
        const double t = options->time_offset + (double)n * (double)FIXED_DT;
        const bool calm_phase = n < report->calm_frames;
        PIXELICA_WaterLoad load;
        double gap, depth;
        clock_t started;

        frame.surface = calm_phase ? calm : waves;

        started = clock();
        if (pixelica_water_mesh_build(&frame.surface, &frame.patch, t, &frame.mesh) !=
            PIXELICA_OK) {
            ++report->draw_failures;
            return false;
        }
        report->mesh_ms += ms_since(started);
        started = clock();
        if (pixelica_water_load(&frame.surface, &frame.rig, body, t, &load) != PIXELICA_OK) {
            ++report->load_failures;
            return false;
        }
        report->physics_ms += ms_since(started);

        /* Measured BEFORE the step, so the mesh, the load and the body state
         * all belong to the same instant t. */
        gap = measure_render_gap(&frame, body, t, &report->probe_failures);
        if (gap < 0.0) {
            return false;
        }
        if (!calm_phase && gap > report->max_render_gap) {
            report->max_render_gap = gap;
        }
        depth = mean_probe_depth(&frame, body, t, &report->probe_failures);
        if (calm_phase) {
            /* The release drop: 0 probes submerged at first is the point. */
            if (load.submerged_probes < report->drop_min_submerged) {
                report->drop_min_submerged = load.submerged_probes;
            }
        } else {
            if (load.submerged_probes < report->min_submerged) {
                report->min_submerged = load.submerged_probes;
            }
            if (load.submerged_probes > report->max_submerged) {
                report->max_submerged = load.submerged_probes;
            }
        }

        started = clock();
        if (pixelica_water_body_step(body, &load, FIXED_DT) != PIXELICA_OK) {
            ++report->step_failures;
            return false;
        }
        report->physics_ms += ms_since(started);

        if (!calm_phase) {
            float roll = 0.0f, pitch = 0.0f;

            if (pixelica_water_body_attitude(body, &roll, &pitch, NULL) != PIXELICA_OK) {
                ++report->probe_failures;
                return false;
            }
            if (fabs((double)roll) > report->max_roll) {
                report->max_roll = fabs((double)roll);
            }
            if (fabs((double)pitch) > report->max_pitch) {
                report->max_pitch = fabs((double)pitch);
            }
            if (fabs((double)body->position.y + (double)draft) > report->max_heave) {
                report->max_heave = fabs((double)body->position.y + (double)draft);
            }
            if (depth < report->min_mean_depth) {
                report->min_mean_depth = depth;
            }
            if (depth > report->max_mean_depth) {
                report->max_mean_depth = depth;
            }
            report->mean_depth_sum += depth;
        }

        if (ctx != NULL) {
            started = clock();
            pixelica_begin_frame(ctx, PIXELICA_RGB565(120, 164, 204)); /* hazy sky */
            pixelica_set_view_projection(ctx, frame.view_projection);
            if (pixelica_water_draw(ctx, &frame.mesh, &frame.patch, &frame.water_lighting,
                                    NULL) != PIXELICA_OK) {
                ++report->draw_failures;
                return false;
            }
            if (!draw_raft(ctx, &frame, body)) {
                ++report->draw_failures;
                return false;
            }
            if (pixelica_end_frame(ctx) != PIXELICA_OK) {
                ++report->draw_failures;
                return false;
            }
            report->draw_ms += ms_since(started);
            /* "Before": the settled, level raft on flat water. */
            if (n + 1u == report->calm_frames &&
                !capture(options, ctx, "calm", &report->hash_calm, &report->captured_calm)) {
                return false;
            }
            if (n == wave_early && report->wave_frames > 0u &&
                !capture(options, ctx, "waves_early", &report->hash_wave_early,
                         &report->captured_early)) {
                return false;
            }
            if (n == wave_late && report->wave_frames > 0u &&
                !capture(options, ctx, "waves_late", &report->hash_wave_late,
                         &report->captured_late)) {
                return false;
            }
        }

        /* The calm phase must have ended level and at the equilibrium draft. */
        if (n + 1u == report->calm_frames) {
            float roll = 0.0f, pitch = 0.0f;

            if (pixelica_water_body_attitude(body, &roll, &pitch, NULL) != PIXELICA_OK) {
                ++report->probe_failures;
                return false;
            }
            report->calm_draft_error = fabs((double)body->position.y + (double)draft);
            report->calm_tilt = fabs((double)roll) > fabs((double)pitch)
                                    ? fabs((double)roll)
                                    : fabs((double)pitch);
            /* The same equilibrium seen from the probes: their mean submerged
             * depth must be the one pixelica_water_equilibrium_draft predicted. */
            report->calm_depth_error = fabs(depth - report->probe_depth_eq);
        }
        ++report->frames;
    }
    return true;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(FILE *out)
{
    fputs("usage: hello_water [--frames N] [--out-dir DIR] [--scale N] [--time-offset S]\n"
          "  --frames N       total fixed 1/60 s steps (default 420; the first 180 are calm)\n"
          "  --out-dir DIR    write calm.ppm, waves_early.ppm and waves_late.ppm there\n"
          "                   (the directory must already exist; nothing else is touched)\n"
          "  --scale N        PPM pixel scale, 1..8 (default 1)\n"
          "  --time-offset S  start the clock at S seconds, to exercise a long session\n",
          out);
}

int main(int argc, char **argv)
{
    Options options;
    Report report;
    PIXELICA_WaterBody body, replay;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Config cfg;
    double average_depth;
    bool ok;
    int i;

    memset(&options, 0, sizeof options);
    options.frames = 420u;
    options.scale = 1u;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "--frames") == 0 && value != NULL) {
            long n = strtol(value, NULL, 10);

            if (n < 2 || n > 100000) {
                fprintf(stderr, "--frames must be 2..100000\n");
                return 2;
            }
            options.frames = (uint32_t)n;
            ++i;
        } else if (strcmp(arg, "--scale") == 0 && value != NULL) {
            long n = strtol(value, NULL, 10);

            if (n < 1 || n > 8) {
                fprintf(stderr, "--scale must be 1..8\n");
                return 2;
            }
            options.scale = (uint32_t)n;
            ++i;
        } else if (strcmp(arg, "--time-offset") == 0 && value != NULL) {
            char *end = NULL;
            double seconds = strtod(value, &end);

            if (end == value || *end != '\0' || !isfinite(seconds)) {
                fprintf(stderr, "--time-offset must be a finite number of seconds\n");
                return 2;
            }
            options.time_offset = seconds;
            ++i;
        } else if (strcmp(arg, "--out-dir") == 0 && value != NULL) {
            options.out_dir = value;
            ++i;
        } else {
            fprintf(stderr, "unknown or incomplete argument '%s' (try --help)\n", arg);
            return 2;
        }
    }
    if (options.frames <= CALM_FRAMES) {
        fprintf(stderr, "hello_water: --frames must exceed the %u calm frames\n",
                (unsigned)CALM_FRAMES);
        return 2;
    }

    raft_build();
    memset(&cfg, 0, sizeof cfg);
    cfg.width = (uint16_t)VIEW_WIDTH;
    cfg.height = (uint16_t)VIEW_HEIGHT;
    cfg.max_draw_commands = 8u;
    cfg.max_vertices_per_mesh = (uint16_t)MESH_VERTS;
    cfg.frame_arena_bytes = MESH_VERTS * PIXELICA_ARENA_BYTES_PER_VERTEX;
    if (pixelica_create(&cfg, NULL, &ctx) != PIXELICA_OK || ctx == NULL) {
        fprintf(stderr, "hello_water: pixelica_create failed\n");
        return 1;
    }

    memset(&report, 0, sizeof report);
    ok = run(&options, ctx, &body, &report);
    pixelica_destroy(ctx);
    if (!ok) {
        fprintf(stderr, "hello_water: the run failed (draw=%" PRIu32 " load=%" PRIu32
                        " step=%" PRIu32 " probe=%" PRIu32 ")\n",
                report.draw_failures, report.load_failures, report.step_failures,
                report.probe_failures);
        printf("hello_water: ok=0\n");
        return 1;
    }

    /* Same inputs, no rendering: the physics must land on the same state. */
    {
        Report again;

        memset(&again, 0, sizeof again);
        if (!run(&options, NULL, &replay, &again)) {
            fprintf(stderr, "hello_water: the physics-only replay failed\n");
            printf("hello_water: ok=0\n");
            return 1;
        }
    }

    average_depth = report.wave_frames > 0u
                        ? report.mean_depth_sum / (double)report.wave_frames
                        : 0.0;
    printf("hello_water: frames=%" PRIu32 " calm=%" PRIu32 " waves=%" PRIu32
           " time_offset=%.6g\n",
           report.frames, report.calm_frames, report.wave_frames, options.time_offset);
    printf("hello_water: draft=%.4f m (probe depth %.4f m of %.3f m)\n", report.draft,
           report.probe_depth_eq, (double)PROBE_MAX_DEPTH);
    printf("hello_water: calm draft_error=%.6f m tilt=%.6f rad depth_error=%.6f m "
           "drop_min_submerged=%" PRIu32 "\n",
           report.calm_draft_error, report.calm_tilt, report.calm_depth_error,
           report.drop_min_submerged);
    printf("hello_water: waves roll_max=%.4f rad pitch_max=%.4f rad heave_max=%.4f m\n",
           report.max_roll, report.max_pitch, report.max_heave);
    printf("hello_water: probe depth mean=%.4f min=%.4f max=%.4f m (equilibrium %.4f, "
           "saturates at %.3f) submerged=%" PRIu32 "..%" PRIu32 "\n",
           average_depth, report.min_mean_depth, report.max_mean_depth,
           report.probe_depth_eq, (double)PROBE_MAX_DEPTH, report.min_submerged,
           report.max_submerged);
    printf("hello_water: render_physics_gap_max=%.5f m (limit %.3f, cell %.3f m)\n",
           report.max_render_gap, (double)RENDER_GAP_LIMIT,
           (double)PATCH_SIZE / (double)PATCH_CELLS);
    printf("hello_water: frame_fnv1a calm=%016" PRIx64 " early=%016" PRIx64
           " late=%016" PRIx64 "\n",
           report.hash_calm, report.hash_wave_early, report.hash_wave_late);
    printf("hello_water: ms/frame mesh=%.3f physics=%.3f draw=%.3f (%ux%u grid, %u probes)\n",
           report.mesh_ms / (double)report.frames, report.physics_ms / (double)report.frames,
           report.draw_ms / (double)report.frames, (unsigned)PATCH_CELLS,
           (unsigned)PATCH_CELLS, (unsigned)PROBE_COUNT);

    /* Every check, stated once, so a failure says which one. */
    ok = report.draw_failures == 0u && report.load_failures == 0u &&
         report.step_failures == 0u && report.probe_failures == 0u;
    if (!ok) {
        fprintf(stderr, "hello_water: a call failed during the run\n");
    }
    if (!(report.calm_draft_error < 0.005)) {
        fprintf(stderr, "hello_water: the calm raft did not settle to its draft\n");
        ok = false;
    }
    if (!(report.calm_tilt < 0.001)) {
        fprintf(stderr, "hello_water: the calm raft is not level\n");
        ok = false;
    }
    if (!(report.calm_depth_error < 0.005)) {
        fprintf(stderr, "hello_water: the settled probe depth is not the predicted draft\n");
        ok = false;
    }
    /* Both axes must respond, or the sea is effectively one-dimensional. */
    if (!(report.max_roll > TILT_MIN) || !(report.max_pitch > TILT_MIN)) {
        fprintf(stderr, "hello_water: the waves did not tilt the raft on both axes "
                        "(roll %.4f, pitch %.4f, need %.3f)\n",
                report.max_roll, report.max_pitch, (double)TILT_MIN);
        ok = false;
    }
    if (!(report.max_roll < TILT_MAX) || !(report.max_pitch < TILT_MAX)) {
        fprintf(stderr, "hello_water: the raft is close to capsizing\n");
        ok = false;
    }
    if (!(report.max_heave > HEAVE_MIN) || !(report.max_heave < HEAVE_MAX)) {
        fprintf(stderr, "hello_water: the heave is not a believable %.2f..%.2f m\n",
                (double)HEAVE_MIN, (double)HEAVE_MAX);
        ok = false;
    }
    /*
     * Convincingly buoyant: every probe stays in the water for the whole wave
     * phase, with real margin at both ends. A raft that briefly emerges is
     * physically fine, but it is not what this demo is built to show, so this
     * is a hard check on the DEMO's chosen parameters rather than a loosened
     * one. See the RAFT_* block above for why 0.20 m of draft under a 0.153 m
     * swell satisfies it.
     */
    if (!(report.min_mean_depth > DEPTH_MARGIN_MIN)) {
        fprintf(stderr, "hello_water: the raft came too close to leaving the water "
                        "(min mean depth %.4f m, need %.3f)\n",
                report.min_mean_depth, (double)DEPTH_MARGIN_MIN);
        ok = false;
    }
    if (!(report.max_mean_depth < DEPTH_FRACTION_MAX * (double)PROBE_MAX_DEPTH)) {
        fprintf(stderr, "hello_water: the raft sank toward probe saturation "
                        "(max mean depth %.4f m)\n",
                report.max_mean_depth);
        ok = false;
    }
    if (report.min_submerged != PROBE_COUNT) {
        fprintf(stderr, "hello_water: only %" PRIu32 " of %u probes stayed in the water\n",
                report.min_submerged, (unsigned)PROBE_COUNT);
        ok = false;
    }
    if (!(report.max_render_gap <= (double)RENDER_GAP_LIMIT)) {
        fprintf(stderr, "hello_water: the drawn water and the physics water disagree\n");
        ok = false;
    }
    if (report.hash_calm == report.hash_wave_early ||
        report.hash_wave_early == report.hash_wave_late ||
        report.hash_calm == report.hash_wave_late) {
        fprintf(stderr, "hello_water: the three captures are not distinct frames\n");
        ok = false;
    }
    if (memcmp(&body, &replay, sizeof body) != 0) {
        fprintf(stderr, "hello_water: the replay diverged from the rendered run\n");
        ok = false;
    }
    if (options.out_dir != NULL &&
        !(report.captured_calm && report.captured_early && report.captured_late)) {
        fprintf(stderr, "hello_water: not every capture was written\n");
        ok = false;
    }
    printf("hello_water: ok=%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
