/*
 * Pixelica - pixelica_draw_mesh_effects: legacy bit-identity with NULL/zero
 * effects, bilinear filtering (texel centers, 2x2 mix, edge/repeat neighbors,
 * base alpha, layers), the per-fragment pixel hook (inputs, override, discard,
 * early depth), world-space clip planes (half rect, translated model, several
 * planes, attribute interpolation, perspective) and rejects that leave the
 * frame alone.
 *
 * Same-context mutation from a hook is rejected without changing the frame.
 */
#include <stdbool.h>
#include <stdlib.h>

#include "test_util.h"

#define BG ((uint16_t)0x0000)
#define RED ((uint16_t)0xF800)
#define GREEN ((uint16_t)0x07E0)
#define BLUE ((uint16_t)0x001F)
#define WHITE ((uint16_t)0xFFFF)
#define PI_F 3.14159265358979323846f

typedef struct Target {
    PIXELICA_Context *ctx;
    uint16_t w, h;
} Target;

typedef struct Quad {
    PIXELICA_Vec3 v[4];
    PIXELICA_Vec2 uv[4];
    PIXELICA_Mesh m;
} Quad;

static const float k_id[16] = {
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
};
static const uint16_t k_quad[6] = {0, 1, 2, 0, 2, 3};
static const float k_uv01[4] = {0.0f, 0.0f, 1.0f, 1.0f}; /* u0 v0 u1 v1 */

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
static uint16_t px(const Target *t, int x, int y) { return pixelica_t_color_at(t->ctx, t->w, x, y); }
static uint16_t depth_at(const Target *t, int x, int y) { return pixelica_t_depth_at(t->ctx, t->w, x, y); }
static uint64_t depth_hash(const Target *t) { return pixelica_t_hash16(pixelica_depth_buffer(t->ctx), npix(t)); }
static uint64_t frame_hash(const Target *t)
{
    return pixelica_t_hash16(pixelica_color_buffer(t->ctx), npix(t)) ^ (depth_hash(t) * 31u);
}
static uint32_t count(const Target *t, uint16_t c) { return pixelica_t_count_color(t->ctx, t->w, t->h, c); }

/* ---- reference arithmetic (native 5/6/5 channels) ---------------------- */

static int ch_r(uint16_t c) { return c >> 11; }
static int ch_g(uint16_t c) { return (c >> 5) & 63; }
static int ch_b(uint16_t c) { return c & 31; }
static uint16_t pack(int r, int g, int b) { return (uint16_t)((r << 11) | (g << 5) | b); }
static int near_div(int v) { return (v + 127) / 255; }
static int mixc(int r, int p, int a) { return near_div(r * a + p * (255 - a)); }

static uint16_t ref_blend(uint16_t src, uint16_t dst, int a)
{
    return pack(mixc(ch_r(src), ch_r(dst), a), mixc(ch_g(src), ch_g(dst), a),
                mixc(ch_b(src), ch_b(dst), a));
}

static uint16_t ref_add(uint16_t p, uint16_t t)
{
    int r = ch_r(p) + ch_r(t), g = ch_g(p) + ch_g(t), b = ch_b(p) + ch_b(t);

    return pack(r < 31 ? r : 31, g < 63 ? g : 63, b < 31 ? b : 31);
}

static uint16_t ref_modulate(uint16_t c, PIXELICA_ColorRGBA8 v)
{
    return pack(near_div(ch_r(c) * v.r), near_div(ch_g(c) * v.g), near_div(ch_b(c) * v.b));
}

static int close_color(uint16_t a, uint16_t b, int tol)
{
    return abs(ch_r(a) - ch_r(b)) <= tol && abs(ch_g(a) - ch_g(b)) <= tol &&
           abs(ch_b(a) - ch_b(b)) <= tol;
}

static int wrap_i(int i, int n, PIXELICA_WrapMode m)
{
    if (m == PIXELICA_WRAP_REPEAT) {
        return ((i % n) + n) % n;
    }
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

/* Bilinear reference in double: sample at u*W - 0.5, rounded per channel. */
static uint16_t ref_bilinear(const PIXELICA_Texture *tex, double u, double v,
                             PIXELICA_WrapMode wu, PIXELICA_WrapMode wv)
{
    const double fx = u * tex->width - 0.5, fy = v * tex->height - 0.5;
    const double x0 = floor(fx), y0 = floor(fy), tx = fx - x0, ty = fy - y0;
    double acc[3] = {0, 0, 0};
    int k;

    for (k = 0; k < 4; ++k) {
        const int dx = k & 1, dy = k >> 1;
        const int xi = wrap_i((int)x0 + dx, tex->width, wu);
        const int yi = wrap_i((int)y0 + dy, tex->height, wv);
        const double wgt = (dx ? tx : 1.0 - tx) * (dy ? ty : 1.0 - ty);
        const uint16_t c = tex->pixels[(size_t)yi * tex->width + (size_t)xi];

        acc[0] += wgt * ch_r(c);
        acc[1] += wgt * ch_g(c);
        acc[2] += wgt * ch_b(c);
    }
    return pack((int)floor(acc[0] + 0.5), (int)floor(acc[1] + 0.5), (int)floor(acc[2] + 0.5));
}

/* ---- builders ----------------------------------------------------------- */

static PIXELICA_ColorRGBA8 rgba(int r, int g, int b, int a)
{
    PIXELICA_ColorRGBA8 c;

    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    c.a = (uint8_t)a;
    return c;
}

static void fill_vc(PIXELICA_ColorRGBA8 vc[4], PIXELICA_ColorRGBA8 c)
{
    vc[0] = vc[1] = vc[2] = vc[3] = c;
}

static void set_uv(PIXELICA_Vec2 uv[4], const float uvr[4])
{
    uv[0].x = uvr[0]; uv[0].y = uvr[3]; uv[1].x = uvr[2]; uv[1].y = uvr[3];
    uv[2].x = uvr[2]; uv[2].y = uvr[1]; uv[3].x = uvr[0]; uv[3].y = uvr[1];
}

static void const_uv(PIXELICA_Vec2 uv[4], float u, float v)
{
    int i;

    for (i = 0; i < 4; ++i) {
        uv[i].x = u;
        uv[i].y = v;
    }
}

/* Pixel rect [x0,x1) x [y0,y1) at NDC z, CCW; v0 BL, v1 BR, v2 TR, v3 TL. */
static void quad(Quad *q, const Target *t, float x0, float y0, float x1, float y1,
                 float z, uint16_t color, const float uvr[4])
{
    const float nx0 = x0 / (float)t->w * 2.0f - 1.0f, nx1 = x1 / (float)t->w * 2.0f - 1.0f;
    const float ny0 = 1.0f - y0 / (float)t->h * 2.0f, ny1 = 1.0f - y1 / (float)t->h * 2.0f;

    q->v[0].x = nx0; q->v[0].y = ny1; q->v[1].x = nx1; q->v[1].y = ny1;
    q->v[2].x = nx1; q->v[2].y = ny0; q->v[3].x = nx0; q->v[3].y = ny0;
    q->v[0].z = q->v[1].z = q->v[2].z = q->v[3].z = z;
    set_uv(q->uv, uvr);
    q->m.vertices = q->v; q->m.vertex_count = 4; q->m.indices = k_quad;
    q->m.index_count = 6; q->m.color = color;
}

static PIXELICA_Texture tex_of(const uint16_t *p, uint16_t w, uint16_t h)
{
    PIXELICA_Texture t;
    t.pixels = p;
    t.width = w;
    t.height = h;
    return t;
}

static PIXELICA_TextureLayer layer_of(const PIXELICA_Texture *tex, const PIXELICA_Vec2 *uvs,
                                      PIXELICA_TextureCombine c, uint8_t opacity)
{
    PIXELICA_TextureLayer l;
    l.texture = tex;
    l.uvs = uvs;
    l.wrap_u = l.wrap_v = PIXELICA_WRAP_CLAMP;
    l.combine = c;
    l.opacity = opacity;
    return l;
}

static PIXELICA_MaterialLayers ml_of(const PIXELICA_Vec2 *uvs, const PIXELICA_Texture *tex,
                                     const PIXELICA_ColorRGBA8 *vc,
                                     const PIXELICA_TextureLayer *layers, size_t n)
{
    PIXELICA_MaterialLayers ml;
    memset(&ml, 0, sizeof ml);
    ml.base.uvs = uvs;
    ml.base.texture = tex;
    ml.vertex_colors = vc;
    ml.layers = layers;
    ml.layer_count = n;
    return ml;
}

static PIXELICA_DrawOptions opts(PIXELICA_AlphaMode mode, uint8_t opacity, uint8_t cutoff,
                                 const uint8_t *alpha, size_t bytes)
{
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    o.alpha_mode = mode;
    o.opacity = opacity;
    o.alpha_cutoff = cutoff;
    o.texture_alpha = alpha;
    o.texture_alpha_bytes = bytes;
    return o;
}

static PIXELICA_WorldClipPlane plane_of(float nx, float ny, float nz, float d)
{
    PIXELICA_WorldClipPlane p;

    p.normal.x = nx;
    p.normal.y = ny;
    p.normal.z = nz;
    p.distance = d;
    return p;
}

static PIXELICA_MeshEffects fx_zero(void)
{
    PIXELICA_MeshEffects fx;

    memset(&fx, 0, sizeof fx);
    return fx;
}

static PIXELICA_MeshEffects fx_planes(const PIXELICA_WorldClipPlane *p, size_t n)
{
    PIXELICA_MeshEffects fx = fx_zero();

    fx.clip_planes = p;
    fx.clip_plane_count = n;
    return fx;
}

static PIXELICA_Status draw_fx(const Target *t, const Quad *q, const float *model,
                               const PIXELICA_MaterialLayers *ml, const PIXELICA_DrawOptions *o,
                               const PIXELICA_MeshEffects *fx)
{
    return pixelica_draw_mesh_effects(t->ctx, &q->m, model, ml, o, fx);
}

/* Draw a flat full-target quad under the given planes; model = identity. */
static PIXELICA_Status draw_flat_clipped(const Target *t, uint16_t color,
                                         const PIXELICA_WorldClipPlane *p, size_t n)
{
    Quad q;
    PIXELICA_MeshEffects fx = fx_planes(p, n);

    quad(&q, t, 0, 0, (float)t->w, (float)t->h, 0.0f, color, k_uv01);
    return draw_fx(t, &q, k_id, NULL, NULL, &fx);
}

/* Columns [x_lo, x_hi] hold `c`, all others BG, on every row. Returns bad count. */
static int check_columns(const Target *t, int x_lo, int x_hi, uint16_t c)
{
    int x, y, bad = 0;

    for (y = 0; y < t->h; ++y) {
        for (x = 0; x < t->w; ++x) {
            bad += px(t, x, y) != ((x >= x_lo && x <= x_hi) ? c : BG);
        }
    }
    return bad;
}

/* ---- pixel hook --------------------------------------------------------- */

typedef struct Hook {
    int calls, bad;
    int check_color, check_alpha;
    uint16_t want_color;
    uint8_t want_alpha;
    int discard_x_below;
    int set_color, set_alpha;
    uint16_t new_color;
    uint8_t new_alpha;
    uint16_t w, h;
    uint8_t seen[16 * 16];
} Hook;

static bool hook_fn(void *ud, const PIXELICA_Fragment *f, uint16_t *oc, uint8_t *oa)
{
    Hook *h = (Hook *)ud;

    ++h->calls;
    if (f->x >= h->w || f->y >= h->h || f->depth == PIXELICA_DEPTH_CLEAR) {
        ++h->bad;
    } else {
        ++h->seen[(size_t)f->y * h->w + f->x];
    }
    if (h->check_color && (*oc != h->want_color || f->color != h->want_color)) {
        ++h->bad;
    }
    if (h->check_alpha && (*oa != h->want_alpha || f->alpha != h->want_alpha)) {
        ++h->bad;
    }
    if (f->x < h->discard_x_below) {
        return false;
    }
    if (h->set_color) {
        *oc = h->new_color;
    }
    if (h->set_alpha) {
        *oa = h->new_alpha;
    }
    return true;
}

static void hook_init(Hook *h, const Target *t)
{
    memset(h, 0, sizeof *h);
    h->w = t->w;
    h->h = t->h;
}

static PIXELICA_MeshEffects fx_hook(Hook *h)
{
    PIXELICA_MeshEffects fx = fx_zero();

    fx.pixel = hook_fn;
    fx.userdata = h;
    return fx;
}

/* ---- tests -------------------------------------------------------------- */

/* NULL or zeroed effects (and an explicit NEAREST) are the layers API exactly. */
static void test_legacy_identical(void)
{
    static const uint16_t texels[6] = {0x1000, 0x1001, 0x1002, 0x1003, 0x1004, 0x1005};
    static const uint8_t alpha[6] = {255, 200, 128, 90, 0, 255};
    PIXELICA_Texture tex = tex_of(texels, 3, 2);
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_DrawOptions o[3];
    PIXELICA_FrameStats s_ref = {0};
    Target t;
    int set, mode;

    if (!open_t(&t, 32, 24)) {
        return;
    }
    fill_vc(vc, rgba(220, 180, 255, 200));
    vc[2] = rgba(30, 255, 90, 120);
    o[0] = pixelica_draw_options_default();
    o[1] = opts(PIXELICA_ALPHA_TEST, 200, 100, alpha, 6);
    o[1].wrap_u = o[1].wrap_v = PIXELICA_WRAP_REPEAT;
    o[2] = opts(PIXELICA_ALPHA_BLEND, 160, 0, alpha, 6);
    for (set = 0; set < 3; ++set) {
        uint64_t ref = 0;

        for (mode = 0; mode < 4; ++mode) {
            Quad a, b;
            PIXELICA_MaterialLayers ma, mb;
            PIXELICA_MeshEffects zero = fx_zero(), nearest = fx_zero();
            PIXELICA_DrawOptions flat_options = o[set];
            PIXELICA_Status s1, s2;

            flat_options.texture_alpha = NULL;
            flat_options.texture_alpha_bytes = 0;
            nearest.filter = PIXELICA_FILTER_NEAREST;
            pixelica_begin_frame(t.ctx, 0x1084);
            quad(&a, &t, 1.3f, 2.1f, 30.2f, 22.7f, 0.3f, BLUE, k_uv01);
            quad(&b, &t, 5.0f, 0.0f, 17.0f, 24.0f, -0.2f, GREEN, k_uv01);
            ma = ml_of(a.uv, &tex, vc, NULL, 0);
            mb = ml_of(NULL, NULL, NULL, NULL, 0);
            if (mode == 0) {
                s1 = pixelica_draw_mesh_layers(t.ctx, &a.m, k_id, &ma, &o[set]);
                s2 = pixelica_draw_mesh_layers(t.ctx, &b.m, k_id, &mb, &flat_options);
            } else {
                const PIXELICA_MeshEffects *fx = mode == 1 ? NULL : (mode == 2 ? &zero : &nearest);

                s1 = pixelica_draw_mesh_effects(t.ctx, &a.m, k_id, &ma, &o[set], fx);
                s2 = pixelica_draw_mesh_effects(t.ctx, &b.m, k_id, &mb, &flat_options, fx);
            }
            CHECK(s1 == PIXELICA_OK && s2 == PIXELICA_OK);
            if (mode == 0) {
                ref = frame_hash(&t);
                s_ref = pixelica_frame_stats(t.ctx);
            } else {
                PIXELICA_FrameStats s = pixelica_frame_stats(t.ctx);

                CHECK(frame_hash(&t) == ref);
                CHECK_EQ_LONG(s.pixels_written, s_ref.pixels_written);
                CHECK_EQ_LONG(s.fragments_covered, s_ref.fragments_covered);
                CHECK_EQ_LONG(s.pixels_covered, s_ref.pixels_covered);
                CHECK_EQ_LONG(s.draw_calls, s_ref.draw_calls);
            }
        }
    }
    close_t(&t);
}

/* One full-target quad with a constant UV; returns the (uniform) color. */
static uint16_t sample_at(const Target *t, const PIXELICA_Texture *tex, float u, float v,
                          PIXELICA_WrapMode wu, PIXELICA_WrapMode wv, PIXELICA_FilterMode f)
{
    Quad q;
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions o = pixelica_draw_options_default();
    PIXELICA_MeshEffects fx = fx_zero();
    uint16_t c;

    o.wrap_u = wu;
    o.wrap_v = wv;
    fx.filter = f;
    quad(&q, t, 0, 0, (float)t->w, (float)t->h, 0.0f, BG, k_uv01);
    const_uv(q.uv, u, v);
    ml = ml_of(q.uv, tex, NULL, NULL, 0);
    pixelica_begin_frame(t->ctx, WHITE);
    CHECK(draw_fx(t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
    c = px(t, t->w / 2, t->h / 2);
    CHECK_EQ_LONG(count(t, c), (long long)npix(t));
    return c;
}

/* 2x2 texels A B / C D. Exact expectations: sums divisible by 4, no ties. */
static void test_bilinear_basic(void)
{
    uint16_t texels[4];
    PIXELICA_Texture tex;
    Target t;
    const uint16_t A = pack(20, 40, 12), B = pack(4, 0, 4), C = pack(8, 16, 0), D = pack(0, 8, 8);
    const PIXELICA_WrapMode CL = PIXELICA_WRAP_CLAMP, RP = PIXELICA_WRAP_REPEAT;
    const PIXELICA_FilterMode BI = PIXELICA_FILTER_BILINEAR, NE = PIXELICA_FILTER_NEAREST;

    texels[0] = A; texels[1] = B; texels[2] = C; texels[3] = D;
    tex = tex_of(texels, 2, 2);
    if (!open_t(&t, 8, 8)) {
        return;
    }
    /* exact texel centers: u,v in {0.25, 0.75} */
    CHECK(sample_at(&t, &tex, 0.25f, 0.25f, CL, CL, BI) == A);
    CHECK(sample_at(&t, &tex, 0.75f, 0.25f, CL, CL, BI) == B);
    CHECK(sample_at(&t, &tex, 0.25f, 0.75f, CL, CL, BI) == C);
    CHECK(sample_at(&t, &tex, 0.75f, 0.75f, CL, CL, BI) == D);
    /* the four-texel center is the exact mean (8, 16, 6) */
    CHECK(sample_at(&t, &tex, 0.5f, 0.5f, CL, CL, BI) == pack(8, 16, 6));
    CHECK(sample_at(&t, &tex, 0.5f, 0.5f, CL, CL, NE) == D); /* nearest: floor(1.0) */
    /* 3/4 A + 1/4 B on row 0 */
    CHECK(sample_at(&t, &tex, 0.375f, 0.25f, CL, CL, BI) == pack(16, 30, 10));
    /* clamp: u=0 and u=1 are the edge texels; REPEAT mixes with the far side */
    CHECK(sample_at(&t, &tex, 0.0f, 0.25f, CL, CL, BI) == A);
    CHECK(sample_at(&t, &tex, 1.0f, 0.25f, CL, CL, BI) == B);
    CHECK(sample_at(&t, &tex, 0.0f, 0.25f, RP, CL, BI) == pack(12, 20, 8));
    CHECK(sample_at(&t, &tex, 1.0f, 0.25f, RP, CL, BI) == pack(12, 20, 8));
    CHECK(sample_at(&t, &tex, 0.25f, 0.0f, CL, RP, BI) == pack(14, 28, 6));
    /* far outside the unit square: clamped to a corner, repeat stays in the texture */
    CHECK(sample_at(&t, &tex, -1.0e30f, 0.25f, CL, CL, BI) == A);
    CHECK(sample_at(&t, &tex, 1.0e30f, 0.25f, CL, CL, BI) == B);
    CHECK(sample_at(&t, &tex, -7.0f, -9.0f, CL, CL, BI) == A);
    CHECK(sample_at(&t, &tex, 5.0f, 5.0f, CL, CL, BI) == D);
    {
        uint16_t c = sample_at(&t, &tex, -123456.75f, 987654.5f, RP, RP, BI);
        uint16_t c2 = sample_at(&t, &tex, 1.0e30f, -1.0e30f, RP, RP, BI);
        int lo_r = 0, hi_r = 20;

        CHECK(ch_r(c) >= lo_r && ch_r(c) <= hi_r && ch_g(c) <= 40 && ch_b(c) <= 12);
        CHECK(ch_r(c2) >= lo_r && ch_r(c2) <= hi_r && ch_g(c2) <= 40 && ch_b(c2) <= 12);
    }
    close_t(&t);
}

/* Generic gradient texture vs the double-precision reference (+-1 per channel),
 * with clamp and with repeat over UVs beyond [0, 1]. */
static void test_bilinear_gradient(void)
{
    uint16_t texels[16];
    PIXELICA_Texture tex;
    PIXELICA_MeshEffects fx = fx_zero();
    int x, y, pass, i, bad;
    Target t;

    for (i = 0; i < 16; ++i) {
        const int tx = i % 4, ty = i / 4;
        texels[i] = pack((tx * 9 + ty * 3) % 32, (tx * 17 + ty * 11) % 64, (tx * 5 + ty * 13) % 32);
    }
    tex = tex_of(texels, 4, 4);
    fx.filter = PIXELICA_FILTER_BILINEAR;
    if (!open_t(&t, 16, 16)) {
        return;
    }
    for (pass = 0; pass < 2; ++pass) {
        const float uvr[4] = {pass ? -0.6f : 0.0f, pass ? -0.3f : 0.0f, pass ? 1.9f : 1.0f,
                              pass ? 1.7f : 1.0f};
        const PIXELICA_WrapMode wm = pass ? PIXELICA_WRAP_REPEAT : PIXELICA_WRAP_CLAMP;
        PIXELICA_DrawOptions o = pixelica_draw_options_default();
        PIXELICA_MaterialLayers ml;
        Quad q;

        o.wrap_u = o.wrap_v = wm;
        quad(&q, &t, 0, 0, 16, 16, 0.0f, BG, uvr);
        ml = ml_of(q.uv, &tex, NULL, NULL, 0);
        pixelica_begin_frame(t.ctx, BG);
        CHECK(draw_fx(&t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
        bad = 0;
        for (y = 0; y < 16; ++y) {
            for (x = 0; x < 16; ++x) {
                const double u = uvr[0] + (uvr[2] - uvr[0]) * (x + 0.5) / 16.0;
                const double v = uvr[1] + (uvr[3] - uvr[1]) * (y + 0.5) / 16.0;

                bad += !close_color(px(&t, x, y), ref_bilinear(&tex, u, v, wm, wm), 1);
            }
        }
        CHECK_EQ_LONG(bad, 0);
    }
    close_t(&t);
}

/* Bilinear covers base texel alpha and layer textures; native channels stay
 * 5/6/5 and nearest is still the default. */
static void test_bilinear_alpha_and_layers(void)
{
    uint16_t base_px[4], lay_px[4];
    static const uint8_t plane[4] = {40, 200, 120, 0}; /* mean 90 */
    PIXELICA_Texture base, lay;
    PIXELICA_TextureLayer l;
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions o;
    PIXELICA_MeshEffects fx = fx_zero();
    const uint16_t M = pack(8, 16, 6), L = pack(5, 11, 5);
    Quad q;
    Target t;

    base_px[0] = pack(20, 40, 12); base_px[1] = pack(4, 0, 4);
    base_px[2] = pack(8, 16, 0);   base_px[3] = pack(0, 8, 8);
    lay_px[0] = pack(2, 4, 6);     lay_px[1] = pack(6, 12, 2);
    lay_px[2] = pack(10, 20, 10);  lay_px[3] = pack(2, 8, 2);
    base = tex_of(base_px, 2, 2);
    lay = tex_of(lay_px, 2, 2);
    fx.filter = PIXELICA_FILTER_BILINEAR;
    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    const_uv(q.uv, 0.5f, 0.5f);

    /* base alpha: mean 90 */
    ml = ml_of(q.uv, &base, NULL, NULL, 0);
    o = opts(PIXELICA_ALPHA_BLEND, 255, 0, plane, 4);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, ref_blend(M, BLUE, 90)), 64);
    o = opts(PIXELICA_ALPHA_TEST, 255, 90, plane, 4);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, M), 64);
    o.alpha_cutoff = 91;
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, BLUE), 64);
    /* nearest alpha at this UV is plane[3] = 0: everything dropped by TEST/BLEND */
    fx.filter = PIXELICA_FILTER_NEAREST;
    o = opts(PIXELICA_ALPHA_TEST, 255, 1, plane, 4);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, BLUE), 64);
    fx.filter = PIXELICA_FILTER_BILINEAR;

    /* base + ADD layer, both bilinear */
    l = layer_of(&lay, q.uv, PIXELICA_TEXTURE_ADD, 255);
    ml = ml_of(q.uv, &base, NULL, &l, 1);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, ref_add(M, L)), 64);
    fx.filter = PIXELICA_FILTER_NEAREST;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, ref_add(base_px[3], lay_px[3])), 64);

    /* flat base + LERP layer: layer is filtered on its own */
    fx.filter = PIXELICA_FILTER_BILINEAR;
    l = layer_of(&lay, q.uv, PIXELICA_TEXTURE_LERP, 255);
    ml = ml_of(NULL, NULL, NULL, &l, 1);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, L), 64);
    close_t(&t);
}

/* Hook receives the computed color (texture * vertex RGBA) and alpha (global
 * opacity applied); it can override either; it runs before the alpha test. */
static void test_hook_inputs_and_override(void)
{
    static const uint16_t texel[1] = {0xA5D3};
    static const uint8_t plane[1] = {200};
    PIXELICA_Texture tex = tex_of(texel, 1, 1);
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_MaterialLayers ml;
    PIXELICA_DrawOptions blend, test;
    PIXELICA_MeshEffects fx;
    Hook h;
    Quad q;
    Target t;
    uint16_t want;
    int x, y, bad;
    const int eff = near_div(near_div(200 * 128) * 150); /* 59 */

    if (!open_t(&t, 8, 8)) {
        return;
    }
    fill_vc(vc, rgba(200, 100, 255, 128));
    want = ref_modulate(texel[0], vc[0]);
    quad(&q, &t, 0, 0, 8, 8, 0.0f, BG, k_uv01);
    ml = ml_of(q.uv, &tex, vc, NULL, 0);
    blend = opts(PIXELICA_ALPHA_BLEND, 150, 0, plane, 1);
    test = opts(PIXELICA_ALPHA_TEST, 150, 128, plane, 1);

    /* observe only */
    hook_init(&h, &t);
    h.check_color = h.check_alpha = 1;
    h.want_color = want;
    h.want_alpha = (uint8_t)eff;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &blend, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK_EQ_LONG(h.bad, 0);
    bad = 0;
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            bad += h.seen[y * 8 + x] != 1;
        }
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK_EQ_LONG(count(&t, ref_blend(want, BLUE, eff)), 64);

    /* hook runs before the alpha test: TEST would drop alpha 59 < 128 itself */
    hook_init(&h, &t);
    h.check_color = h.check_alpha = 1;
    h.want_color = want;
    h.want_alpha = (uint8_t)eff;
    h.set_alpha = 1;
    h.new_alpha = 255;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &test, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK_EQ_LONG(h.bad, 0);
    CHECK_EQ_LONG(count(&t, want), 64);
    CHECK(depth_at(&t, 4, 4) < PIXELICA_DEPTH_CLEAR);

    /* ... and the hook can push a passing alpha under the cutoff */
    hook_init(&h, &t);
    h.set_alpha = 1;
    h.new_alpha = 0;
    test.opacity = 255;
    test.alpha_cutoff = 1;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &test, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK_EQ_LONG(count(&t, BLUE), 64);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 0);
    CHECK(depth_at(&t, 4, 4) == PIXELICA_DEPTH_CLEAR);

    /* override color, keep computed alpha */
    hook_init(&h, &t);
    h.set_color = 1;
    h.new_color = GREEN;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &blend, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, ref_blend(GREEN, BLUE, eff)), 64);

    /* override alpha to opaque */
    hook_init(&h, &t);
    h.set_alpha = 1;
    h.new_alpha = 255;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, &ml, &blend, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, want), 64);
    close_t(&t);
}

/* false discards: OPAQUE still honors it (no color, no depth); BLEND never
 * writes depth, with or without the hook. */
static void test_hook_discard_and_depth(void)
{
    Hook h;
    PIXELICA_MeshEffects fx;
    PIXELICA_DrawOptions o;
    Quad q;
    Target t;
    uint64_t d0, f0;
    int x, y, bad;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, RED, k_uv01);

    /* OPAQUE: left half discarded */
    hook_init(&h, &t);
    h.discard_x_below = 4;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    CHECK(draw_fx(&t, &q, k_id, NULL, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK_EQ_LONG(h.bad, 0);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 32);
    bad = 0;
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            const int kept = x >= 4;

            bad += px(&t, x, y) != (kept ? RED : BLUE);
            bad += kept ? depth_at(&t, x, y) == PIXELICA_DEPTH_CLEAR
                        : depth_at(&t, x, y) != PIXELICA_DEPTH_CLEAR;
        }
    }
    CHECK_EQ_LONG(bad, 0);

    /* a discarded fragment leaves the depth free for a later, farther draw */
    {
        Quad far_q;

        quad(&far_q, &t, 0, 0, 8, 8, 0.5f, GREEN, k_uv01);
        CHECK(pixelica_draw_mesh(t.ctx, &far_q.m, k_id) == PIXELICA_OK);
        CHECK(px(&t, 1, 1) == GREEN && px(&t, 6, 1) == RED);
    }

    /* BLEND: no depth writes; discard leaves the frame untouched */
    o = opts(PIXELICA_ALPHA_BLEND, 128, 0, NULL, 0);
    hook_init(&h, &t);
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    d0 = depth_hash(&t);
    CHECK(draw_fx(&t, &q, k_id, NULL, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK(depth_hash(&t) == d0);
    CHECK_EQ_LONG(count(&t, ref_blend(RED, BLUE, 128)), 64);

    hook_init(&h, &t);
    h.discard_x_below = 8;
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BLUE);
    f0 = frame_hash(&t);
    CHECK(draw_fx(&t, &q, k_id, NULL, &o, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 64);
    CHECK(frame_hash(&t) == f0);
    close_t(&t);
}

/* Fragments rejected by the early depth test never reach the hook. */
static void test_hook_early_depth(void)
{
    Hook h;
    PIXELICA_MeshEffects fx;
    Quad near_q, far_q;
    Target t;
    int x, bad;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    /* near covers the left half only: hook sees exactly the right half */
    quad(&near_q, &t, 0, 0, 4, 8, -0.5f, BLUE, k_uv01);
    quad(&far_q, &t, 0, 0, 8, 8, 0.5f, RED, k_uv01);
    hook_init(&h, &t);
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &near_q.m, k_id) == PIXELICA_OK);
    CHECK(draw_fx(&t, &far_q, k_id, NULL, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 32);
    CHECK_EQ_LONG(h.bad, 0);
    bad = 0;
    for (x = 0; x < 8; ++x) {
        bad += h.seen[3 * 8 + x] != (x >= 4 ? 1 : 0);
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK(px(&t, 1, 1) == BLUE && px(&t, 6, 1) == RED);

    /* fully hidden: no calls at all, frame unchanged */
    quad(&near_q, &t, 0, 0, 8, 8, -0.5f, BLUE, k_uv01);
    hook_init(&h, &t);
    fx = fx_hook(&h);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &near_q.m, k_id) == PIXELICA_OK);
    CHECK(draw_fx(&t, &far_q, k_id, NULL, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(h.calls, 0);
    CHECK_EQ_LONG(count(&t, BLUE), 64);
    close_t(&t);
}

/* x >= 0 keeps the right half; sign, scale and offset follow dot(n,w)+d >= 0. */
static void test_clip_half_rect(void)
{
    Target t;
    PIXELICA_WorldClipPlane p;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    p = plane_of(1, 0, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, &p, 1) == PIXELICA_OK);
    CHECK_EQ_LONG(check_columns(&t, 4, 7, RED), 0);
    CHECK(depth_at(&t, 2, 2) == PIXELICA_DEPTH_CLEAR && depth_at(&t, 5, 2) < PIXELICA_DEPTH_CLEAR);

    p = plane_of(-1, 0, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, &p, 1) == PIXELICA_OK);
    CHECK_EQ_LONG(check_columns(&t, 0, 3, RED), 0);

    p = plane_of(2, 0, 0, 1); /* 2x + 1 >= 0  <=>  x >= -0.5 (no normalization) */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, &p, 1) == PIXELICA_OK);
    CHECK_EQ_LONG(check_columns(&t, 2, 7, RED), 0);

    p = plane_of(1, 0, 0, 5.0f); /* keeps everything */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, &p, 1) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 64);

    p = plane_of(1, 0, 0, -5.0f); /* keeps nothing */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, &p, 1) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, BG), 64);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).pixels_written, 0);
    close_t(&t);
}

/* Planes are in world space: the model translation moves the quad before the
 * plane test (an object-space cut would land two columns further right). */
static void test_clip_world_space(void)
{
    PIXELICA_WorldClipPlane p = plane_of(1, 0, 0, 0);
    PIXELICA_MeshEffects fx = fx_planes(&p, 1);
    float model[16];
    Quad q;
    Target t;

    if (!open_t(&t, 16, 8)) {
        return;
    }
    quad(&q, &t, 4, 0, 12, 8, 0.0f, RED, k_uv01); /* object x in [-0.5, 0.5] */
    pixelica_mat4_translation(model, 0.25f, 0.0f, 0.0f); /* +2 pixels */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, model, NULL, NULL, &fx) == PIXELICA_OK);
    CHECK_EQ_LONG(check_columns(&t, 8, 13, RED), 0);
    close_t(&t);
}

static void test_clip_multiple_planes(void)
{
    PIXELICA_WorldClipPlane p[4];
    Target t;
    int x, y, bad;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    p[0] = plane_of(1, 0, 0, 0);
    p[1] = plane_of(0, 1, 0, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, p, 2) == PIXELICA_OK);
    bad = 0;
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            bad += px(&t, x, y) != ((x >= 4 && y < 4) ? RED : BG);
        }
    }
    CHECK_EQ_LONG(bad, 0);

    /* four planes (the maximum); the two extra ones keep the whole quadrant */
    p[2] = plane_of(-1, 0, 0, 0.9f);
    p[3] = plane_of(0, -1, 0, 0.9f);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, p, 4) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 16);

    /* count 0 with a non-NULL array is the plain draw */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_flat_clipped(&t, RED, p, 0) == PIXELICA_OK);
    CHECK_EQ_LONG(count(&t, RED), 64);
    close_t(&t);
}

/* Vertex RGBA and UV are interpolated across the cut, not reset at it: kept
 * pixels equal the unclipped frame. */
static void test_clip_attribute_interpolation(void)
{
    uint16_t texels[8];
    uint16_t ref_row[8], ref_row_bi[8];
    PIXELICA_Texture tex;
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_MaterialLayers ml_v, ml_t;
    PIXELICA_WorldClipPlane p = plane_of(1, 0, 0, 0.3f); /* keeps px >= 3 */
    PIXELICA_MeshEffects fx_c = fx_planes(&p, 1), fx_cb = fx_planes(&p, 1), fx_b = fx_zero();
    Quad q;
    Target t;
    int x, i, bad;

    for (i = 0; i < 8; ++i) {
        texels[i] = pack(i * 4, i * 8, 31 - i * 4);
    }
    tex = tex_of(texels, 8, 1);
    vc[0] = vc[3] = rgba(0, 255, 255, 255);
    vc[1] = vc[2] = rgba(255, 255, 255, 255);
    fx_cb.filter = fx_b.filter = PIXELICA_FILTER_BILINEAR;
    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, WHITE, k_uv01);

    /* vertex colors */
    ml_v = ml_of(NULL, NULL, vc, NULL, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml_v, NULL, NULL) == PIXELICA_OK);
    for (x = 0; x < 8; ++x) {
        ref_row[x] = px(&t, x, 3);
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml_v, NULL, &fx_c) == PIXELICA_OK);
    bad = 0;
    for (x = 0; x < 8; ++x) {
        const uint16_t c = px(&t, x, 3);

        bad += x < 3 ? c != BG : !close_color(c, ref_row[x], 1);
        bad += px(&t, x, 0) != c || px(&t, x, 7) != c;
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK(ch_r(px(&t, 3, 3)) > 0 && ch_r(px(&t, 7, 3)) > ch_r(px(&t, 3, 3)));

    /* UVs, nearest: pixel i shows texel i */
    ml_t = ml_of(q.uv, &tex, NULL, NULL, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml_t, NULL, &fx_c) == PIXELICA_OK);
    bad = 0;
    for (x = 0; x < 8; ++x) {
        bad += px(&t, x, 4) != (x < 3 ? BG : texels[x]);
    }
    CHECK_EQ_LONG(bad, 0);

    /* UVs, bilinear: clipped == unclipped */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml_t, NULL, &fx_b) == PIXELICA_OK);
    for (x = 0; x < 8; ++x) {
        ref_row_bi[x] = px(&t, x, 4);
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, &ml_t, NULL, &fx_cb) == PIXELICA_OK);
    bad = 0;
    for (x = 0; x < 8; ++x) {
        bad += x < 3 ? px(&t, x, 4) != BG : !close_color(px(&t, x, 4), ref_row_bi[x], 1);
    }
    CHECK_EQ_LONG(bad, 0);
    close_t(&t);
}

/* Floor receding from z=-1 to z=-9 (near plane 1.5). The world plane keeps
 * z <= -4; vertex R falls 255 -> 0 along z, so interpolation stays
 * perspective-correct across the cut. */
static void test_clip_perspective(void)
{
    PIXELICA_Vec3 v[4] = {{-4, -1, -1}, {4, -1, -1}, {4, -1, -9}, {-4, -1, -9}};
    PIXELICA_ColorRGBA8 vc[4];
    PIXELICA_WorldClipPlane p = plane_of(0, 0, -1, -4.0f);
    PIXELICA_MeshEffects fx = fx_planes(&p, 1);
    PIXELICA_MaterialLayers ml;
    PIXELICA_Mesh m;
    float persp[16];
    Target t;
    uint32_t kept = 0, bad = 0, dropped = 0;
    int x, y;

    if (!open_t(&t, 64, 64)) {
        return;
    }
    CHECK(pixelica_mat4_perspective(persp, PI_F * 0.5f, 1.0f, 1.5f, 100.0f) == PIXELICA_OK);
    pixelica_set_view_projection(t.ctx, persp);
    m.vertices = v; m.vertex_count = 4; m.indices = k_quad; m.index_count = 6; m.color = WHITE;
    vc[0] = vc[1] = rgba(255, 255, 255, 255);
    vc[2] = vc[3] = rgba(0, 255, 255, 255);
    ml = ml_of(NULL, NULL, vc, NULL, 0);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_effects(t.ctx, &m, k_id, &ml, NULL, &fx) == PIXELICA_OK);
    for (y = 0; y < 64; ++y) {
        const double ndc_y = 1.0 - 2.0 * (y + 0.5) / 64.0;

        if (ndc_y > -1.0 / 9.0 - 0.01 || ndc_y < -1.0 / 1.5 + 0.01) {
            continue;
        }
        for (x = 24; x < 40; ++x) {
            const uint16_t c = px(&t, x, y);

            if (ndc_y >= -0.25 + 0.02) { /* world z <= -4 */
                const double tt = (1.0 / ndc_y + 9.0) / 8.0;

                bad += abs(ch_r(c) - (int)floor(31.0 * tt + 0.5)) > 1;
                ++kept;
            } else if (ndc_y <= -0.25 - 0.02) { /* nearer than the plane */
                bad += c != BG;
                ++dropped;
            }
        }
    }
    CHECK_EQ_LONG(bad, 0);
    CHECK_EQ_LONG(kept, 48); /* three far rows, 16 samples per row */
    CHECK_EQ_LONG(dropped, 192);
    close_t(&t);
}

/* Rejected before any change: frame, depth and draw counter stay put, the hook
 * never runs, last_error reports INVALID_ARG. */
static void test_rejects(void)
{
    Quad q, bg;
    PIXELICA_WorldClipPlane pl[5], one;
    PIXELICA_MeshEffects fx;
    PIXELICA_MaterialLayers ml = ml_of(NULL, NULL, NULL, NULL, 0);
    Hook h;
    float model[16];
    Target t;
    uint64_t before;
    uint32_t calls;
    int i;

    if (!open_t(&t, 8, 8)) {
        return;
    }
    quad(&q, &t, 0, 0, 8, 8, 0.0f, RED, k_uv01);
    quad(&bg, &t, 0, 0, 4, 8, 0.5f, BLUE, k_uv01);
    for (i = 0; i < 5; ++i) {
        pl[i] = plane_of(1, 0, 0, 0);
    }
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &bg.m, k_id) == PIXELICA_OK);
    before = frame_hash(&t);
    calls = pixelica_frame_stats(t.ctx).draw_calls;
    hook_init(&h, &t);

#define REJECT(model_, fxp_)                                                              \
    do {                                                                                  \
        CHECK(pixelica_draw_mesh_effects(t.ctx, &q.m, (model_), &ml, NULL, (fxp_)) ==     \
              PIXELICA_ERR_INVALID_ARG);                                                  \
        CHECK(frame_hash(&t) == before);                                                  \
        CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).draw_calls, calls);                     \
    } while (0)

    fx = fx_planes(pl, 5); /* more than four */
    fx.pixel = hook_fn;
    fx.userdata = &h;
    REJECT(k_id, &fx);
    fx = fx_zero(); /* count without array */
    fx.clip_plane_count = 1;
    REJECT(k_id, &fx);

    for (i = 0; i < 5; ++i) {
        one = plane_of(1, 0, 0, 0);
        switch (i) {
        case 0: one.normal.x = pixelica_t_nan(); break;
        case 1: one.normal.y = pixelica_t_inf(); break;
        case 2: one.normal = (PIXELICA_Vec3){0.0f, 0.0f, 0.0f}; break;
        case 3: one.distance = pixelica_t_nan(); break;
        default: one.distance = -pixelica_t_inf(); break;
        }
        fx = fx_planes(&one, 1);
        fx.pixel = hook_fn;
        fx.userdata = &h;
        REJECT(k_id, &fx);
    }
    /* a second plane being bad poisons the draw even after a good first */
    pl[1].normal = (PIXELICA_Vec3){0.0f, 0.0f, 0.0f};
    fx = fx_planes(pl, 2);
    REJECT(k_id, &fx);

    /* planes require an affine model */
    memcpy(model, k_id, sizeof model);
    model[3] = 0.25f;
    model[12] = 0.25f;
    one = plane_of(1, 0, 0, 0);
    fx = fx_planes(&one, 1);
    REJECT(model, &fx);

    /* bad filter values */
    fx = fx_zero();
    fx.filter = (PIXELICA_FilterMode)2;
    REJECT(k_id, &fx);
    fx.filter = (PIXELICA_FilterMode)-1;
    REJECT(k_id, &fx);

    /* aliasing buffers the context writes */
    {
        const void *owned = pixelica_color_buffer(t.ctx);

        fx = fx_planes((const PIXELICA_WorldClipPlane *)owned, 1);
        REJECT(k_id, &fx);
        CHECK(pixelica_draw_mesh_effects(t.ctx, &q.m, k_id, &ml, NULL,
                                         (const PIXELICA_MeshEffects *)owned) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(frame_hash(&t) == before);
    }
#undef REJECT

    CHECK_EQ_LONG(h.calls, 0);
    CHECK_EQ_LONG(pixelica_frame_stats(t.ctx).last_error, PIXELICA_ERR_INVALID_ARG);

    /* a non-affine model without planes behaves like the layers API */
    {
        PIXELICA_Status want, got;
        uint64_t ref;

        fx = fx_zero();
        pixelica_begin_frame(t.ctx, BG);
        want = pixelica_draw_mesh_layers(t.ctx, &q.m, model, &ml, NULL);
        ref = frame_hash(&t);
        pixelica_begin_frame(t.ctx, BG);
        got = pixelica_draw_mesh_effects(t.ctx, &q.m, model, &ml, NULL, &fx);
        CHECK(got == want);
        CHECK(frame_hash(&t) == ref);
    }

    /* the maximum of four planes plus a hook and bilinear are accepted */
    fx = fx_planes(pl, 4);
    pl[1] = plane_of(1, 0, 0, 0);
    fx.filter = PIXELICA_FILTER_BILINEAR;
    fx.pixel = hook_fn;
    fx.userdata = &h;
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh_effects(t.ctx, &q.m, k_id, &ml, NULL, &fx) == PIXELICA_OK);
    CHECK(h.calls > 0);
    close_t(&t);
}

typedef struct Reentry {
    Target *target, *other;
    Quad *quad;
    int calls, updates;
} Reentry;

static void reentry_update(void *ud, float dt, uint32_t buttons)
{
    Reentry *r = (Reentry *)ud;
    (void)dt;
    (void)buttons;
    ++r->updates;
}

static bool reentry_pixel(void *ud, const PIXELICA_Fragment *f,
                          uint16_t *color, uint8_t *alpha)
{
    Reentry *r = (Reentry *)ud;
    Target *t = r->target;
    (void)f;
    (void)color;
    (void)alpha;
    if (r->calls++ == 0) {
        PIXELICA_FrameStats before = pixelica_frame_stats(t->ctx), after;
        uint64_t hash = frame_hash(t);
        CHECK(pixelica_draw_mesh(t->ctx, &r->quad->m, k_id) == PIXELICA_ERR_INVALID_ARG);
        CHECK(draw_fx(t, r->quad, k_id, NULL, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_end_frame(t->ctx) == PIXELICA_ERR_INVALID_ARG);
        pixelica_begin_frame(t->ctx, WHITE);
        pixelica_set_view_projection(t->ctx, NULL);
        pixelica_set_cull_mode(t->ctx, PIXELICA_CULL_BACK);
        pixelica_update(t->ctx, 1000000u, reentry_update, r);
        pixelica_destroy(t->ctx);
        after = pixelica_frame_stats(t->ctx);
        CHECK(memcmp(&before, &after, sizeof before) == 0);
        CHECK(frame_hash(t) == hash);
        CHECK(r->updates == 0);
        /* An independent target remains usable from the hook. */
        CHECK(pixelica_draw_mesh(r->other->ctx, &r->quad->m, k_id) == PIXELICA_OK);
    }
    return true;
}

static void test_hook_reentry(void)
{
    Target t, other;
    Quad q;
    Reentry r;
    PIXELICA_MeshEffects fx = fx_zero();
    uint64_t result;
    if (!open_t(&t, 8, 8)) { return; }
    if (!open_t(&other, 8, 8)) { close_t(&t); return; }
    quad(&q, &t, 0, 0, 8, 8, 0, RED, k_uv01);
    memset(&r, 0, sizeof r);
    r.target = &t; r.other = &other; r.quad = &q;
    fx.pixel = reentry_pixel; fx.userdata = &r;
    pixelica_set_cull_mode(t.ctx, PIXELICA_CULL_NONE);
    pixelica_begin_frame(other.ctx, BG);
    pixelica_begin_frame(t.ctx, BG);
    CHECK(draw_fx(&t, &q, k_id, NULL, NULL, &fx) == PIXELICA_OK);
    CHECK(r.calls == 64);
    result = frame_hash(&t);
    CHECK(pixelica_end_frame(t.ctx) == PIXELICA_OK);
    /* Guard clears after the draw and a refused camera change had no effect. */
    pixelica_begin_frame(t.ctx, BG);
    CHECK(pixelica_draw_mesh(t.ctx, &q.m, k_id) == PIXELICA_OK);
    CHECK(frame_hash(&t) == result);
    close_t(&other);
    close_t(&t);
}

int main(void)
{
    CHECK_EQ_LONG(PIXELICA_FILTER_NEAREST, 0);
    CHECK_EQ_LONG(PIXELICA_FILTER_BILINEAR, 1);
    test_legacy_identical();
    test_bilinear_basic();
    test_bilinear_gradient();
    test_bilinear_alpha_and_layers();
    test_hook_inputs_and_override();
    test_hook_discard_and_depth();
    test_hook_early_depth();
    test_hook_reentry();
    test_clip_half_rect();
    test_clip_world_space();
    test_clip_multiple_planes();
    test_clip_attribute_interpolation();
    test_clip_perspective();
    test_rejects();
    return pixelica_t_finish("test_render_effects");
}
