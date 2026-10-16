/*
 * Pixelica - opt-in normal-based lighting (pixelica_draw_mesh_lit):
 * unlit bit-identity when lighting is off, ambient/directional Lambert
 * arithmetic, inverse-transpose normals under non-uniform and mirrored
 * transforms, normalization, two-sided shading, composition with vertex
 * colors, and rejects that leave the frame and the stats alone.
 */
#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define WHITE ((uint16_t)0xFFFF)
#define GREY ((uint16_t)0x8410) /* r=16 g=32 b=16 */
#define BAD PIXELICA_ERR_INVALID_ARG

typedef struct Target {
    PIXELICA_Context *ctx;
    uint16_t w, h;
} Target;

typedef struct Quad {
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec3 n[4];
    PIXELICA_Mesh m;
} Quad;

static const float k_id[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};
static const uint16_t k_quad[6] = {0, 1, 2, 0, 2, 3};

static int open_t(Target *t, uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg = pixelica_t_config(w, h);

    t->w = w;
    t->h = h;
    t->ctx = NULL;
    CHECK(pixelica_create(&cfg, NULL, &t->ctx) == PIXELICA_OK);
    return t->ctx != NULL;
}

static void close_t(Target *t)
{
    CHECK(pixelica_memory_guards_intact(t->ctx) == 1);
    pixelica_destroy(t->ctx);
}

static size_t npix(const Target *t) { return (size_t)t->w * t->h; }
static uint16_t px(const Target *t, int x, int y)
{
    return pixelica_t_color_at(t->ctx, t->w, x, y);
}
static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^
           (pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)) * 31u);
}

static int ch_r(uint16_t c) { return c >> 11; }
static int ch_g(uint16_t c) { return (c >> 5) & 63; }
static int ch_b(uint16_t c) { return c & 31; }
static uint16_t pack(int r, int g, int b)
{
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* floor(x * f / 255 + 0.5), the renderer's modulation. */
static int modulate(int x, double f)
{
    return (int)((double)x * f / 255.0 + 0.5);
}

static uint16_t ref_lit(uint16_t c, double fr, double fg, double fb)
{
    return pack(modulate(ch_r(c), fr), modulate(ch_g(c), fg),
                modulate(ch_b(c), fb));
}

static PIXELICA_Vec3 v3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

/* Screen-space rect at NDC z, CCW, every vertex sharing one normal. */
static void quad(Quad *q, const Target *t, float x0, float y0, float x1,
                 float y1, float z, uint16_t color, PIXELICA_Vec3 normal)
{
    const float nx0 = x0 / (float)t->w * 2.0f - 1.0f;
    const float nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    const float ny0 = 1.0f - y0 / (float)t->h * 2.0f;
    const float ny1 = 1.0f - y1 / (float)t->h * 2.0f;
    int i;

    q->v[0].x = nx0; q->v[0].y = ny1;
    q->v[1].x = nx1; q->v[1].y = ny1;
    q->v[2].x = nx1; q->v[2].y = ny0;
    q->v[3].x = nx0; q->v[3].y = ny0;
    for (i = 0; i < 4; ++i) {
        q->v[i].z = z;
        q->n[i] = normal;
    }
    q->m.vertices = q->v;
    q->m.vertex_count = 4;
    q->m.indices = k_quad;
    q->m.index_count = 6;
    q->m.color = color;
}

static PIXELICA_DirectionalLight light_of(PIXELICA_Vec3 dir, int r, int g, int b)
{
    PIXELICA_DirectionalLight l;

    l.direction = dir;
    l.r = (uint8_t)r;
    l.g = (uint8_t)g;
    l.b = (uint8_t)b;
    return l;
}

static PIXELICA_Lighting lighting_of(const PIXELICA_Vec3 *normals,
                                     const PIXELICA_DirectionalLight *lights,
                                     size_t count, int ar, int ag, int ab)
{
    PIXELICA_Lighting lt;

    lt.normals = normals;
    lt.lights = lights;
    lt.light_count = count;
    lt.ambient_r = (uint8_t)ar;
    lt.ambient_g = (uint8_t)ag;
    lt.ambient_b = (uint8_t)ab;
    lt.two_sided = false;
    return lt;
}

/* ---- old behavior is untouched ----------------------------------------- */

static void test_unlit_identity(void)
{
    Target t;
    Quad q;
    uint64_t legacy;

    if (!open_t(&t, 24, 16)) {
        return;
    }
    quad(&q, &t, 2.0f, 2.0f, 20.0f, 14.0f, 0.0f, GREY, v3(0.0f, 0.0f, 1.0f));

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &q.m, k_id) == PIXELICA_OK);
    legacy = frame_hash(&t);

    /* NULL lighting is exactly the old call, bit for bit. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, NULL) ==
          PIXELICA_OK);
    CHECK(frame_hash(&t) == legacy);

    /* White ambient with no lights reproduces the unlit image exactly. */
    pixelica_begin_frame(t.ctx, BG);
    {
        PIXELICA_Lighting lt = lighting_of(q.n, NULL, 0, 255, 255, 255);

        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
              PIXELICA_OK);
    }
    CHECK(frame_hash(&t) == legacy);

    /* Black ambient with no lights is black (the depth writes still match). */
    pixelica_begin_frame(t.ctx, BG);
    {
        PIXELICA_Lighting lt = lighting_of(q.n, NULL, 0, 0, 0, 0);

        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
              PIXELICA_OK);
    }
    CHECK(px(&t, 10, 8) == 0u);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 18u * 12u);
    close_t(&t);
}

/* ---- Lambert arithmetic ------------------------------------------------ */

static void test_directional_lambert(void)
{
    Target t;
    Quad q;
    PIXELICA_DirectionalLight lights[2];
    PIXELICA_Lighting lt;

    if (!open_t(&t, 16, 16)) {
        return;
    }
    /* A quad facing +Z, lit by a light travelling along -Z (straight at it). */
    quad(&q, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE, v3(0.0f, 0.0f, 1.0f));
    lights[0] = light_of(v3(0.0f, 0.0f, -1.0f), 200, 100, 50);
    lt = lighting_of(q.n, lights, 1, 10, 20, 30);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    /* dot == 1, so f = ambient + full light colour. */
    CHECK(px(&t, 8, 8) == ref_lit(WHITE, 210.0, 120.0, 80.0));

    /* Facing away: max(0, dot) clamps the diffuse term to zero. */
    lights[0] = light_of(v3(0.0f, 0.0f, 1.0f), 200, 100, 50);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    CHECK(px(&t, 8, 8) == ref_lit(WHITE, 10.0, 20.0, 30.0));

    /* two_sided uses |dot| instead, so the same light shines through. */
    lt.two_sided = true;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    CHECK(px(&t, 8, 8) == ref_lit(WHITE, 210.0, 120.0, 80.0));
    lt.two_sided = false;

    /* 45 degrees: dot = cos(45) = 0.70710678, rounded per light channel. */
    lights[0] = light_of(v3(0.0f, -1.0f, -1.0f), 200, 100, 50);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    {
        const double d = 0.70710678118654752;
        const double fr = 10.0 + (double)(int)(200.0 * d + 0.5);
        const double fg = 20.0 + (double)(int)(100.0 * d + 0.5);
        const double fb = 30.0 + (double)(int)(50.0 * d + 0.5);

        CHECK(px(&t, 8, 8) == ref_lit(WHITE, fr, fg, fb));
    }

    /* Several lights add, and the total saturates at 255 per channel. */
    lights[0] = light_of(v3(0.0f, 0.0f, -1.0f), 200, 200, 200);
    lights[1] = light_of(v3(0.0f, 0.0f, -1.0f), 200, 200, 200);
    lt = lighting_of(q.n, lights, 2, 0, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    CHECK(px(&t, 8, 8) == WHITE); /* clamped to 255 = identity, not wrapped */

    /* The light direction need not be unit length. */
    lights[0] = light_of(v3(0.0f, 0.0f, -37.5f), 128, 128, 128);
    lt = lighting_of(q.n, lights, 1, 0, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    CHECK(px(&t, 8, 8) == ref_lit(WHITE, 128.0, 128.0, 128.0));

    /* Neither do the normals: scaling them all changes nothing. */
    {
        int i;

        for (i = 0; i < 4; ++i) {
            q.n[i] = v3(0.0f, 0.0f, 9.0f);
        }
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
              PIXELICA_OK);
        CHECK(px(&t, 8, 8) == ref_lit(WHITE, 128.0, 128.0, 128.0));
    }
    close_t(&t);
}

/* ---- inverse-transpose normals ----------------------------------------- */

static void test_nonuniform_scale(void)
{
    Target t;
    Quad q;
    PIXELICA_DirectionalLight light;
    PIXELICA_Lighting lt;
    float model[16];

    if (!open_t(&t, 16, 16)) {
        return;
    }
    /* A surface tilted 45 degrees in the XZ plane: normal (1, 0, 1). The
     * model squashes X by 1/4, which tilts the SURFACE toward +Z and must
     * therefore tilt the normal toward +X - the opposite way to what
     * transforming the normal with the model itself would do. */
    quad(&q, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE, v3(1.0f, 0.0f, 1.0f));
    pixelica_mat4_identity(model);
    model[0] = 0.25f;

    light = light_of(v3(-1.0f, 0.0f, 0.0f), 255, 255, 255); /* toward +X */
    lt = lighting_of(q.n, &light, 1, 0, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, model, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    {
        /* N = inverse-transpose(diag(1/4,1,1)) * (1,0,1) = (4, 0, 1),
         * normalized (0.970142..., 0, 0.242535...); dot with (1,0,0). */
        const double d = 4.0 / sqrt(17.0);
        const double f = (double)(int)(255.0 * d + 0.5);

        CHECK(px(&t, 8, 8) == ref_lit(WHITE, f, f, f));
        /* The naive model-transformed normal would have been
         * (0.25, 0, 1) normalized = 0.2425..., i.e. much darker. */
        CHECK(ch_r(px(&t, 8, 8)) > 25);
    }

    /* Scaling the whole model uniformly cannot change the shading. */
    {
        uint16_t before = px(&t, 8, 8);

        pixelica_mat4_identity(model);
        model[0] = model[5] = model[10] = 3.0f;
        quad(&q, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE,
             v3(0.0f, 0.0f, 1.0f));
        light = light_of(v3(0.0f, 0.0f, -1.0f), 255, 255, 255);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
              PIXELICA_OK);
        before = px(&t, 8, 8);
        /* The same mesh under a uniform scale: same NDC quad (the vertices
         * are already in NDC, so scale the quad back down), same shading. */
        {
            Quad small;
            int i;

            quad(&small, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE,
                 v3(0.0f, 0.0f, 1.0f));
            for (i = 0; i < 4; ++i) {
                small.v[i].x /= 3.0f;
                small.v[i].y /= 3.0f;
                small.v[i].z /= 3.0f;
            }
            lt.normals = small.n;
            pixelica_begin_frame(t.ctx, BG);
            CHECK(pixelica_draw_mesh_lit(t.ctx, &small.m, model, NULL, NULL,
                                         NULL, &lt) == PIXELICA_OK);
            CHECK(px(&t, 8, 8) == before);
        }
    }

    /* A mirroring model flips the normals, so a face lit head-on becomes
     * unlit (and vice versa). */
    {
        PIXELICA_Lighting m_lt;

        quad(&q, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE,
             v3(0.0f, 0.0f, 1.0f));
        pixelica_mat4_identity(model);
        model[10] = -1.0f; /* mirror in Z */
        light = light_of(v3(0.0f, 0.0f, -1.0f), 255, 255, 255);
        m_lt = lighting_of(q.n, &light, 1, 8, 8, 8);
        pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, model, NULL, NULL, NULL,
                                     &m_lt) == PIXELICA_OK);
        CHECK(px(&t, 8, 8) == ref_lit(WHITE, 8.0, 8.0, 8.0));
        pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_BACK);
    }
    close_t(&t);
}

/* ---- interpolation and composition ------------------------------------- */

static void test_gouraud_and_vertex_colors(void)
{
    Target t;
    Quad q;
    PIXELICA_DirectionalLight light;
    PIXELICA_Lighting lt;
    PIXELICA_MaterialLayers ml;
    PIXELICA_ColorRGBA8 vc[4];

    if (!open_t(&t, 32, 8)) {
        return;
    }
    /* Opposite normals on the left and right edges: the factor is
     * interpolated across the quad, so the shading forms a gradient. */
    quad(&q, &t, 0.0f, 0.0f, 32.0f, 8.0f, 0.0f, WHITE, v3(0.0f, 0.0f, 1.0f));
    q.n[0] = v3(-1.0f, 0.0f, 0.0f); /* bottom-left */
    q.n[3] = v3(-1.0f, 0.0f, 0.0f); /* top-left */
    q.n[1] = v3(1.0f, 0.0f, 0.0f);  /* bottom-right */
    q.n[2] = v3(1.0f, 0.0f, 0.0f);  /* top-right */
    light = light_of(v3(-1.0f, 0.0f, 0.0f), 255, 255, 255); /* toward +X */
    lt = lighting_of(q.n, &light, 1, 0, 0, 0);

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) ==
          PIXELICA_OK);
    /* Left edge faces away (black), right edge faces the light (full). */
    CHECK(ch_r(px(&t, 0, 4)) == 0);
    CHECK(ch_r(px(&t, 31, 4)) > 28);
    CHECK(ch_r(px(&t, 8, 4)) < ch_r(px(&t, 24, 4))); /* monotone gradient */

    /* Lighting multiplies with a vertex colour instead of replacing it. */
    {
        int i;

        for (i = 0; i < 4; ++i) {
            q.n[i] = v3(0.0f, 0.0f, 1.0f);
            vc[i].r = 128;
            vc[i].g = 255;
            vc[i].b = 255;
            vc[i].a = 255;
        }
        light = light_of(v3(0.0f, 0.0f, -1.0f), 128, 128, 128);
        memset(&ml, 0, sizeof ml);
        ml.vertex_colors = vc;
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, &ml, NULL, NULL, &lt) ==
              PIXELICA_OK);
        /* factor = 128 * 128 / 255 per red channel, 128 for g/b. */
        CHECK(px(&t, 16, 4) ==
              ref_lit(WHITE, 128.0 * 128.0 / 255.0, 128.0, 128.0));

        /* Without lighting the same vertex colour alone is unchanged. */
        pixelica_begin_frame(t.ctx, BG);
        CHECK(pixelica_draw_mesh_layers(t.ctx, &q.m, k_id, &ml, NULL) ==
              PIXELICA_OK);
        CHECK(px(&t, 16, 4) == ref_lit(WHITE, 128.0, 255.0, 255.0));
    }
    close_t(&t);
}

/* ---- validation -------------------------------------------------------- */

static void test_lighting_rejects(void)
{
    Target t;
    Quad q;
    PIXELICA_DirectionalLight lights[PIXELICA_MAX_DIRECTIONAL_LIGHTS + 1u];
    PIXELICA_Lighting lt;
    PIXELICA_Vec3 normals[4];
    uint64_t before;
    uint32_t calls;
    float model[16];
    size_t i;

    if (!open_t(&t, 16, 16)) {
        return;
    }
    quad(&q, &t, 1.0f, 1.0f, 15.0f, 15.0f, 0.0f, WHITE, v3(0.0f, 0.0f, 1.0f));
    for (i = 0; i < 4; ++i) {
        normals[i] = v3(0.0f, 0.0f, 1.0f);
    }
    for (i = 0; i < PIXELICA_MAX_DIRECTIONAL_LIGHTS + 1u; ++i) {
        lights[i] = light_of(v3(0.0f, 0.0f, -1.0f), 255, 255, 255);
    }

    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &q.m, k_id) == PIXELICA_OK);
    before = frame_hash(&t);
    calls = pixelica_frame_stats(t.ctx).draw_calls;

    /* Missing or malformed normals. */
    lt = lighting_of(NULL, lights, 1, 0, 0, 0);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    lt = lighting_of(normals, lights, 1, 0, 0, 0);
    normals[2] = v3(0.0f, 0.0f, 0.0f); /* zero-length normal */
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    normals[2] = v3(0.0f, pixelica_t_nan(), 1.0f);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    normals[2] = v3(0.0f, 0.0f, 1.0f);

    /* Malformed light lists. */
    lt = lighting_of(normals, NULL, 1, 0, 0, 0);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    lt = lighting_of(normals, lights, PIXELICA_MAX_DIRECTIONAL_LIGHTS + 1u, 0, 0, 0);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    lights[0] = light_of(v3(0.0f, 0.0f, 0.0f), 255, 255, 255);
    lt = lighting_of(normals, lights, 1, 0, 0, 0);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    lights[0] = light_of(v3(pixelica_t_inf(), 0.0f, -1.0f), 255, 255, 255);
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL, &lt) == BAD);
    lights[0] = light_of(v3(0.0f, 0.0f, -1.0f), 255, 255, 255);

    /* A model with no normal matrix (singular 3x3). */
    pixelica_mat4_identity(model);
    model[5] = 0.0f;
    CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, model, NULL, NULL, NULL, &lt) == BAD);

    /* Lighting data that aliases context-owned memory. */
    {
        const PIXELICA_Vec3 *ctx_normals =
            (const PIXELICA_Vec3 *)(const void *)pixelica_color_buffer(t.ctx);
        PIXELICA_Lighting alias = lighting_of(ctx_normals, lights, 1, 0, 0, 0);

        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL,
                                     &alias) == BAD);
        alias = lighting_of(normals,
                            (const PIXELICA_DirectionalLight *)(const void *)
                                pixelica_depth_buffer(t.ctx),
                            1, 0, 0, 0);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &q.m, k_id, NULL, NULL, NULL,
                                     &alias) == BAD);
        CHECK(pixelica_draw_mesh_lit(
                  t.ctx, &q.m, k_id, NULL, NULL, NULL,
                  (const PIXELICA_Lighting *)(const void *)
                      pixelica_color_buffer(t.ctx)) == BAD);
    }

    /* Not one of those touched a pixel, a depth value or a counter. */
    CHECK(frame_hash(&t) == before);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, calls);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, BAD);

    /* An empty mesh with lighting is still accepted and draws nothing. */
    {
        PIXELICA_Mesh empty = q.m;

        empty.index_count = 0;
        empty.vertex_count = 0;
        lt = lighting_of(NULL, NULL, 0, 255, 255, 255);
        CHECK(pixelica_draw_mesh_lit(t.ctx, &empty, k_id, NULL, NULL, NULL,
                                     &lt) == PIXELICA_OK);
    }
    CHECK(pixelica_end_frame(t.ctx) == PIXELICA_OK);
    close_t(&t);
}

int main(void)
{
    test_unlit_identity();
    test_directional_lambert();
    test_nonuniform_scale();
    test_gouraud_and_vertex_colors();
    test_lighting_rejects();
    return pixelica_t_finish("test_lighting");
}
