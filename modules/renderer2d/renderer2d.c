/*
 * Pixelica 2D - software sprite, tilemap, text and isometric helpers.
 * See modules/renderer2d/include/pixelica/renderer2d.h.
 *
 * Every public function copies and validates all of its inputs into locals
 * before the first write, so failures leave outputs, the context and the
 * target untouched. Multi-draw calls (list, tilemap, text) validate the
 * whole input before rasterizing anything.
 *
 * Coordinates run in double. Finite float inputs are at most ~3.4e38, so
 * every product used here (at most four such factors, ~1e154) stays finite
 * in double. A double becomes an integer only after it has been clamped to
 * the clip rectangle or tested to lie inside a source extent, so no
 * conversion can be out of range.
 *
 * Unrotated sprites and tilemaps map a pixel index p to a source coordinate
 * t(p) = ((p + 0.5) * iz + offset) * inv + base. Each step is monotonic in
 * p, so the pixels with 0 <= t < limit form one run, found exactly by binary
 * search (no offscreen looping however far the transform reaches). Rotated
 * sprites loop over a forward-mapped bounding box clamped to the clip. The
 * inner loops still re-check every coordinate before indexing (cheap, and
 * immune to compilers contracting the same expression differently).
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "pixelica/renderer2d.h"
#include "pixelica/draw_options.h"

/* Half-open byte range [lo, hi); an empty range overlaps nothing. */
typedef struct PIXELICA2D_Mem {
    uintptr_t lo, hi;
} PIXELICA2D_Mem;

/* Validated snapshot of a context. */
typedef struct PIXELICA2D_Target {
    uint16_t *color;
    uint16_t *depth; /* NULL when the surface has no depth buffer */
    size_t stride, depth_stride;
    int32_t width, height;
    PIXELICA2D_Mem color_mem, depth_mem;
    PIXELICA2D_Camera camera;
    PIXELICA2D_Rect clip;
} PIXELICA2D_Target;

/* Per-draw pixel state; alpha 0 means "draw nothing". */
typedef struct PIXELICA2D_Shade {
    uint32_t alpha;
    uint16_t depth, key;
    bool keyed, test, write;
} PIXELICA2D_Shade;

/* t(p) = ((p + 0.5) * iz + offset) * inv + base, valid for 0 <= t < limit. */
typedef struct PIXELICA2D_Axis {
    double iz, offset, inv, base, limit;
} PIXELICA2D_Axis;

typedef struct PIXELICA2D_SpriteJob {
    const uint16_t *texels; /* top-left texel of the source rectangle */
    size_t tex_stride;
    double src_w, src_h;
    double zoom, iz, dx, dy; /* dx, dy = camera - position */
    double sx, sy, isx, isy, pivx, pivy, c, s;
    bool rotated;
    PIXELICA2D_Shade shade;
    /* Opt-in draw options (pixelica2d_draw_sprite_ex with non-NULL options). */
    bool ex, repeat_u, repeat_v;
    const uint16_t *tex;
    const uint8_t *amap; /* NULL = alpha 255 */
    int32_t tex_w, tex_h, src_x, src_y;
    int alpha_mode;
    uint8_t opacity, cutoff;
} PIXELICA2D_SpriteJob;

/* ---- memory ranges ------------------------------------------------------ */

static int pixelica2d_mem(const void *ptr, uint64_t count, size_t size, PIXELICA2D_Mem *out)
{
    uintptr_t base = (uintptr_t)ptr;
    uint64_t bytes;

    if (count > UINT64_MAX / (uint64_t)size) {
        return 0;
    }
    bytes = count * (uint64_t)size;
#if SIZE_MAX < UINT64_MAX
    if (bytes > (uint64_t)SIZE_MAX) {
        return 0;
    }
#endif
    if (bytes > (uint64_t)(UINTPTR_MAX - base)) {
        return 0;
    }
    out->lo = base;
    out->hi = base + (uintptr_t)bytes;
    return 1;
}

static int pixelica2d_overlap(PIXELICA2D_Mem a, PIXELICA2D_Mem b)
{
    return a.lo < a.hi && b.lo < b.hi && a.lo < b.hi && b.lo < a.hi;
}

/* Does `m` overlap the color or depth buffer, or the optional scratch? */
static int pixelica2d_hits(const PIXELICA2D_Target *t, PIXELICA2D_Mem m, const PIXELICA2D_Mem *scratch)
{
    return pixelica2d_overlap(m, t->color_mem) || pixelica2d_overlap(m, t->depth_mem) ||
           (scratch != NULL && pixelica2d_overlap(m, *scratch));
}

/* ---- context validation ------------------------------------------------- */

/* Validate a surface for a context stored at `ctx`; fills the buffer fields. */
static int pixelica2d_surface_check(const PIXELICA2D_Surface *surface, const void *ctx, PIXELICA2D_Target *t)
{
    PIXELICA2D_Surface s = *surface;
    PIXELICA2D_Mem ctx_mem;
    uint32_t w = s.width, h = s.height;

    if (s.pixels == NULL || w == 0u || w > PIXELICA2D_MAX_DIMENSION || h == 0u || h > PIXELICA2D_MAX_DIMENSION ||
        s.stride < w ||
        !pixelica2d_mem(s.pixels, (uint64_t)(h - 1u) * s.stride + w, sizeof(uint16_t), &t->color_mem)) {
        return 0;
    }
    t->depth_mem.lo = t->depth_mem.hi = 0u;
    if (s.depth != NULL &&
        (s.depth_stride < w ||
         !pixelica2d_mem(s.depth, (uint64_t)(h - 1u) * s.depth_stride + w, sizeof(uint16_t), &t->depth_mem) ||
         pixelica2d_overlap(t->color_mem, t->depth_mem))) {
        return 0;
    }
    if (!pixelica2d_mem(ctx, 1u, sizeof(PIXELICA2D_Context), &ctx_mem) || pixelica2d_hits(t, ctx_mem, NULL)) {
        return 0;
    }
    t->color = s.pixels;
    t->depth = s.depth;
    t->stride = s.stride;
    t->depth_stride = s.depth != NULL ? s.depth_stride : 0u;
    t->width = (int32_t)w;
    t->height = (int32_t)h;
    return 1;
}

static int pixelica2d_finite2(PIXELICA_Vec2 v)
{
    return isfinite(v.x) && isfinite(v.y);
}

static int pixelica2d_camera_ok(const PIXELICA2D_Camera *c)
{
    return pixelica2d_finite2(c->position) && isfinite(c->zoom) && c->zoom > 0.0f;
}

static int pixelica2d_target(const PIXELICA2D_Context *ctx, PIXELICA2D_Target *t)
{
    PIXELICA2D_Rect r;

    if (ctx == NULL || !pixelica2d_surface_check(&ctx->surface, ctx, t) || !pixelica2d_camera_ok(&ctx->camera)) {
        return 0;
    }
    r = ctx->clip;
    if (r.x < 0 || r.y < 0 || r.width < 0 || r.height < 0 || (int64_t)r.x + r.width > t->width ||
        (int64_t)r.y + r.height > t->height) {
        return 0;
    }
    t->camera = ctx->camera;
    t->clip = r;
    return 1;
}

static uint32_t pixelica2d_sat(uint32_t a, uint64_t b)
{
    return b >= (uint64_t)(UINT32_MAX - a) ? UINT32_MAX : (uint32_t)(a + b);
}

static void pixelica2d_account(PIXELICA2D_Context *ctx, uint64_t draws, uint64_t pixels)
{
    ctx->stats.draw_calls = pixelica2d_sat(ctx->stats.draw_calls, draws);
    ctx->stats.pixels_written = pixelica2d_sat(ctx->stats.pixels_written, pixels);
}

/* ---- pixels ------------------------------------------------------------- */

static uint16_t *pixelica2d_color_row(const PIXELICA2D_Target *t, int32_t y)
{
    return t->color + (size_t)y * t->stride;
}

static uint16_t *pixelica2d_depth_row(const PIXELICA2D_Target *t, int32_t y)
{
    return t->depth != NULL ? t->depth + (size_t)y * t->depth_stride : NULL;
}

/* Per 5/6-bit channel: (src * a + dst * (255 - a) + 127) / 255. */
static uint16_t pixelica2d_blend(uint16_t src, uint16_t dst, uint32_t a)
{
    uint32_t s = src, d = dst, ia = 255u - a;
    uint32_t r = ((s >> 11) * a + (d >> 11) * ia + 127u) / 255u;
    uint32_t g = (((s >> 5) & 0x3Fu) * a + ((d >> 5) & 0x3Fu) * ia + 127u) / 255u;
    uint32_t b = ((s & 0x1Fu) * a + (d & 0x1Fu) * ia + 127u) / 255u;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Framebuffer additive blend, per 5/6/5 channel: dst + (src*a + 127)/255,
 * saturated at the channel maximum. */
static uint16_t pixelica2d_add(uint16_t src, uint16_t dst, uint32_t a)
{
    uint32_t s = src, d = dst;
    uint32_t r = (d >> 11) + (((s >> 11) * a + 127u) / 255u);
    uint32_t g = ((d >> 5) & 0x3Fu) + ((((s >> 5) & 0x3Fu) * a + 127u) / 255u);
    uint32_t b = (d & 0x1Fu) + (((s & 0x1Fu) * a + 127u) / 255u);

    return (uint16_t)(((r < 31u ? r : 31u) << 11) | ((g < 63u ? g : 63u) << 5) |
                      (b < 31u ? b : 31u));
}

/* Key, depth test, blend, depth write. `d` may be NULL when unused. */
static void pixelica2d_plot(const PIXELICA2D_Shade *sh, uint16_t *c, uint16_t *d, uint16_t src, uint64_t *written)
{
    if ((sh->keyed && src == sh->key) || (sh->test && !(sh->depth < *d))) {
        return;
    }
    if (sh->alpha == 255u) {
        *c = src;
    } else {
        *c = pixelica2d_blend(src, *c, sh->alpha);
    }
    if (sh->write) {
        *d = sh->depth;
    }
    ++*written;
}

static int pixelica2d_shade(const PIXELICA2D_Target *t, uint16_t depth, uint8_t opacity, bool keyed, uint16_t key,
                       bool test, bool write, PIXELICA2D_Shade *out)
{
    if ((test || write) && (t->depth == NULL || (uint32_t)depth > (uint32_t)PIXELICA2D_DEPTH_MAX)) {
        return 0;
    }
    out->alpha = (uint32_t)opacity;
    out->depth = depth;
    out->key = key;
    out->keyed = keyed;
    out->test = test;
    out->write = write;
    return 1;
}

static int pixelica2d_texture_ok(const PIXELICA_Texture *tex)
{
    return tex->pixels != NULL && tex->width != 0u && (uint32_t)tex->width <= PIXELICA_MAX_TEXTURE_DIMENSION &&
           tex->height != 0u && (uint32_t)tex->height <= PIXELICA_MAX_TEXTURE_DIMENSION;
}

/* Copy and validate a texture; the struct and its texels must not overlap
 * the target (or the scratch range). */
static int pixelica2d_texture(const PIXELICA2D_Target *t, const PIXELICA_Texture *tex, const PIXELICA2D_Mem *scratch,
                         PIXELICA_Texture *out)
{
    PIXELICA2D_Mem mem;
    PIXELICA_Texture copy;

    if (tex == NULL || !pixelica2d_mem(tex, 1u, sizeof *tex, &mem) || pixelica2d_hits(t, mem, scratch)) {
        return 0;
    }
    copy = *tex;
    if (!pixelica2d_texture_ok(&copy) ||
        !pixelica2d_mem(copy.pixels, (uint64_t)copy.width * (uint64_t)copy.height, sizeof(uint16_t), &mem) ||
        pixelica2d_hits(t, mem, scratch)) {
        return 0;
    }
    *out = copy;
    return 1;
}

/* ---- spans -------------------------------------------------------------- */

static PIXELICA2D_Axis pixelica2d_axis(double iz, double offset, double inv, double base, double limit)
{
    PIXELICA2D_Axis a;

    a.iz = iz;
    a.offset = offset;
    a.inv = inv;
    a.base = base;
    a.limit = limit;
    return a;
}

static double pixelica2d_axis_at(const PIXELICA2D_Axis *a, int32_t p)
{
    return (((double)p + 0.5) * a->iz + a->offset) * a->inv + a->base;
}

/* First p in [lo, hi) past `bound` in the axis direction (t >= bound when
 * t rises with p, t < bound when it falls), or hi. Monotonic, so bisect. */
static int32_t pixelica2d_axis_find(const PIXELICA2D_Axis *a, int32_t lo, int32_t hi, double bound, int rising)
{
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        double v = pixelica2d_axis_at(a, mid);

        if (rising ? v >= bound : v < bound) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

/* Narrow [*lo, *hi) to the run of pixels with 0 <= t(p) < limit. */
static void pixelica2d_axis_span(const PIXELICA2D_Axis *a, int32_t *lo, int32_t *hi)
{
    int rising = a->inv > 0.0;
    int32_t first = pixelica2d_axis_find(a, *lo, *hi, rising ? 0.0 : a->limit, rising);

    *hi = pixelica2d_axis_find(a, first, *hi, rising ? a->limit : 0.0, rising);
    *lo = first;
}

/* Pixels whose centers can fall in [lo, hi] (2-pixel margin), clamped to
 * [clip_lo, clip_hi) in double before converting. */
static void pixelica2d_box(double lo, double hi, int32_t clip_lo, int32_t clip_hi, int32_t *out_lo, int32_t *out_hi)
{
    double a = floor(lo) - 2.0;
    double b = ceil(hi) + 2.0;

    if (a < (double)clip_lo) {
        a = (double)clip_lo;
    }
    if (b > (double)clip_hi) {
        b = (double)clip_hi;
    }
    if (!(a < b)) {
        *out_lo = *out_hi = clip_lo;
        return;
    }
    *out_lo = (int32_t)a;
    *out_hi = (int32_t)b;
}

/* ---- sprites ------------------------------------------------------------ */

/* `relaxed` (draw_sprite_ex with options): only width/height > 0 are required of
 * the source rectangle; the sampler wraps or clamps absolute texel coordinates. */
static int pixelica2d_sprite_prepare(const PIXELICA2D_Target *t, const PIXELICA2D_Sprite *sprite, const PIXELICA2D_Mem *scratch,
                                PIXELICA2D_SpriteJob *job, int relaxed)
{
    PIXELICA2D_Sprite sp;
    PIXELICA_Texture tex;
    PIXELICA2D_Rect r;

    if (sprite == NULL) {
        return 0;
    }
    sp = *sprite;
    if (!pixelica2d_texture(t, sp.texture, scratch, &tex)) {
        return 0;
    }
    r = sp.source;
    if (r.width <= 0 || r.height <= 0 ||
        (!relaxed && (r.x < 0 || r.y < 0 || (int64_t)r.x + r.width > tex.width ||
                      (int64_t)r.y + r.height > tex.height)) ||
        !pixelica2d_finite2(sp.position) || !pixelica2d_finite2(sp.scale) ||
        !pixelica2d_finite2(sp.pivot) || !isfinite(sp.rotation) || sp.scale.x == 0.0f || sp.scale.y == 0.0f ||
        !pixelica2d_shade(t, sp.depth, sp.opacity, sp.color_key_enabled, sp.color_key, sp.depth_test,
                     sp.depth_write, &job->shade)) {
        return 0;
    }
    job->tex_stride = (size_t)tex.width;
    job->texels = relaxed ? tex.pixels : tex.pixels + (size_t)r.y * job->tex_stride + (size_t)r.x;
    job->ex = false;
    job->tex = tex.pixels;
    job->tex_w = (int32_t)tex.width;
    job->tex_h = (int32_t)tex.height;
    job->src_x = r.x;
    job->src_y = r.y;
    job->src_w = (double)r.width;
    job->src_h = (double)r.height;
    job->zoom = (double)t->camera.zoom;
    job->iz = 1.0 / job->zoom;
    job->dx = (double)t->camera.position.x - (double)sp.position.x;
    job->dy = (double)t->camera.position.y - (double)sp.position.y;
    job->sx = (double)sp.scale.x;
    job->sy = (double)sp.scale.y;
    job->isx = 1.0 / job->sx;
    job->isy = 1.0 / job->sy;
    job->pivx = (double)sp.pivot.x;
    job->pivy = (double)sp.pivot.y;
    job->rotated = sp.rotation != 0.0f;
    job->c = job->rotated ? cos((double)sp.rotation) : 1.0;
    job->s = job->rotated ? sin((double)sp.rotation) : 0.0;
    return 1;
}

static uint32_t pixelica2d_mul255(uint32_t a, uint32_t b)
{
    return (a * b + 127u) / 255u;
}

/* Repeat (floor modulo, no overflow) or clamp an absolute texel coordinate. */
static size_t pixelica2d_wrap(int64_t a, int32_t n, bool repeat)
{
    if (repeat) {
        int64_t m = a % (int64_t)n;

        return (size_t)(m < 0 ? m + n : m);
    }
    return (size_t)(a < 0 ? 0 : (a >= n ? (int64_t)n - 1 : a));
}

/* Plot the source texel at local source coordinate (u, v), 0 <= u < src_w. */
static void pixelica2d_sprite_plot(const PIXELICA2D_SpriteJob *j, uint16_t *c, uint16_t *d, double u, double v,
                                   uint64_t *written)
{
    const PIXELICA2D_Shade *sh = &j->shade;
    size_t idx;
    uint16_t src;
    uint32_t a;

    if (!j->ex) {
        pixelica2d_plot(sh, c, d, j->texels[(size_t)v * j->tex_stride + (size_t)u], written);
        return;
    }
    idx = pixelica2d_wrap((int64_t)j->src_y + (int64_t)v, j->tex_h, j->repeat_v) * (size_t)j->tex_w +
          pixelica2d_wrap((int64_t)j->src_x + (int64_t)u, j->tex_w, j->repeat_u);
    src = j->tex[idx];
    if (j->alpha_mode == PIXELICA_ALPHA_OPAQUE) {
        pixelica2d_plot(sh, c, d, src, written);
        return;
    }
    if (sh->keyed && src == sh->key) {
        return;
    }
    a = j->amap != NULL ? j->amap[idx] : 255u;
    a = pixelica2d_mul255(pixelica2d_mul255(a, j->opacity), (uint32_t)sh->alpha);
    if (j->alpha_mode == PIXELICA_ALPHA_TEST ? a < j->cutoff : a == 0u) {
        return;
    }
    if (sh->test && !(sh->depth < *d)) {
        return;
    }
    if (j->alpha_mode == PIXELICA_ALPHA_TEST) {
        *c = src;
        if (sh->write) {
            *d = sh->depth;
        }
    } else if (j->alpha_mode == PIXELICA_ALPHA_ADD) {
        *c = pixelica2d_add(src, *c, a);
    } else {
        *c = a == 255u ? src : pixelica2d_blend(src, *c, a);
    }
    ++*written;
}

/*
 * screen = (R * S * (local - pivot) + position - camera) * zoom, R turning
 * clockwise on a y-down screen. Inverse per pixel center:
 *   q = screen / zoom + camera - position,  local = S^-1 * R^T * q + pivot.
 */
static void pixelica2d_sprite_draw(const PIXELICA2D_Target *t, const PIXELICA2D_SpriteJob *j, uint64_t *written)
{
    int32_t x0 = t->clip.x, x1 = t->clip.x + t->clip.width;
    int32_t y0 = t->clip.y, y1 = t->clip.y + t->clip.height;
    int32_t x, y;

    if (j->shade.alpha == 0u && !(j->ex && j->alpha_mode == PIXELICA_ALPHA_TEST)) {
        return;
    }
    if (!j->rotated) {
        const PIXELICA2D_Axis ax = pixelica2d_axis(j->iz, j->dx, j->isx, j->pivx, j->src_w);
        const PIXELICA2D_Axis ay = pixelica2d_axis(j->iz, j->dy, j->isy, j->pivy, j->src_h);

        pixelica2d_axis_span(&ax, &x0, &x1);
        pixelica2d_axis_span(&ay, &y0, &y1);
        for (y = y0; y < y1; ++y) {
            double v = pixelica2d_axis_at(&ay, y);
            uint16_t *crow = pixelica2d_color_row(t, y);
            uint16_t *drow = pixelica2d_depth_row(t, y);

            if (!(v >= 0.0 && v < j->src_h)) {
                continue;
            }
            for (x = x0; x < x1; ++x) {
                double u = pixelica2d_axis_at(&ax, x);

                if (u >= 0.0 && u < j->src_w) {
                    pixelica2d_sprite_plot(j, crow + x, drow != NULL ? drow + x : NULL, u, v, written);
                }
            }
        }
    } else {
        double lo_x = DBL_MAX, hi_x = -DBL_MAX, lo_y = DBL_MAX, hi_y = -DBL_MAX;
        int i;

        for (i = 0; i < 4; ++i) {
            double lx = (((i & 1) != 0 ? j->src_w : 0.0) - j->pivx) * j->sx;
            double ly = (((i & 2) != 0 ? j->src_h : 0.0) - j->pivy) * j->sy;
            double px = (j->c * lx - j->s * ly - j->dx) * j->zoom;
            double py = (j->s * lx + j->c * ly - j->dy) * j->zoom;

            lo_x = px < lo_x ? px : lo_x;
            hi_x = px > hi_x ? px : hi_x;
            lo_y = py < lo_y ? py : lo_y;
            hi_y = py > hi_y ? py : hi_y;
        }
        pixelica2d_box(lo_x, hi_x, x0, x1, &x0, &x1);
        pixelica2d_box(lo_y, hi_y, y0, y1, &y0, &y1);
        for (y = y0; y < y1; ++y) {
            double qy = ((double)y + 0.5) * j->iz + j->dy;
            uint16_t *crow = pixelica2d_color_row(t, y);
            uint16_t *drow = pixelica2d_depth_row(t, y);

            for (x = x0; x < x1; ++x) {
                double qx = ((double)x + 0.5) * j->iz + j->dx;
                double u = (j->c * qx + j->s * qy) * j->isx + j->pivx;
                double v = (j->c * qy - j->s * qx) * j->isy + j->pivy;

                if (u >= 0.0 && u < j->src_w && v >= 0.0 && v < j->src_h) {
                    pixelica2d_sprite_plot(j, crow + x, drow != NULL ? drow + x : NULL, u, v, written);
                }
            }
        }
    }
}

/* ---- public API --------------------------------------------------------- */

PIXELICA_Status pixelica2d_init(PIXELICA2D_Context *out, const PIXELICA2D_Surface *surface)
{
    PIXELICA2D_Context c;
    PIXELICA2D_Target t;

    if (out == NULL || surface == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&c, 0, sizeof c);
    c.surface = *surface;
    if (!pixelica2d_surface_check(&c.surface, out, &t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    c.camera.position.x = 0.0f;
    c.camera.position.y = 0.0f;
    c.camera.zoom = 1.0f;
    c.clip.width = t.width;
    c.clip.height = t.height;
    *out = c;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_clear(PIXELICA2D_Context *ctx, uint16_t color)
{
    PIXELICA2D_Target t;
    int32_t x, y;

    if (!pixelica2d_target(ctx, &t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (y = 0; y < t.height; ++y) {
        uint16_t *crow = pixelica2d_color_row(&t, y);
        uint16_t *drow = pixelica2d_depth_row(&t, y);

        for (x = 0; x < t.width; ++x) {
            crow[x] = color;
        }
        for (x = 0; drow != NULL && x < t.width; ++x) {
            drow[x] = PIXELICA2D_DEPTH_CLEAR;
        }
    }
    ctx->stats.draw_calls = 0u;
    ctx->stats.pixels_written = 0u;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_set_camera(PIXELICA2D_Context *ctx, const PIXELICA2D_Camera *camera)
{
    PIXELICA2D_Target t;
    PIXELICA2D_Camera c;

    if (ctx == NULL || !pixelica2d_surface_check(&ctx->surface, ctx, &t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (camera == NULL) {
        c.position.x = 0.0f;
        c.position.y = 0.0f;
        c.zoom = 1.0f;
    } else {
        c = *camera;
        if (!pixelica2d_camera_ok(&c)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    ctx->camera = c;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_set_clip(PIXELICA2D_Context *ctx, const PIXELICA2D_Rect *clip)
{
    PIXELICA2D_Target t;
    PIXELICA2D_Rect r;
    int64_t x0, y0, x1, y1;

    if (ctx == NULL || !pixelica2d_surface_check(&ctx->surface, ctx, &t)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (clip == NULL) {
        x0 = y0 = 0;
        x1 = t.width;
        y1 = t.height;
    } else {
        r = *clip;
        if (r.width < 0 || r.height < 0) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        x0 = r.x < 0 ? 0 : (r.x > t.width ? t.width : r.x);
        y0 = r.y < 0 ? 0 : (r.y > t.height ? t.height : r.y);
        x1 = (int64_t)r.x + r.width;
        y1 = (int64_t)r.y + r.height;
        x1 = x1 > t.width ? t.width : (x1 < x0 ? x0 : x1);
        y1 = y1 > t.height ? t.height : (y1 < y0 ? y0 : y1);
    }
    ctx->clip.x = (int32_t)x0;
    ctx->clip.y = (int32_t)y0;
    ctx->clip.width = (int32_t)(x1 - x0);
    ctx->clip.height = (int32_t)(y1 - y0);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_sprite_init(PIXELICA2D_Sprite *out, const PIXELICA_Texture *texture)
{
    PIXELICA2D_Sprite s;
    PIXELICA_Texture tex;

    if (out == NULL || texture == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    tex = *texture;
    if (!pixelica2d_texture_ok(&tex)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof s);
    s.texture = texture;
    s.source.width = tex.width;
    s.source.height = tex.height;
    s.position.x = s.position.y = 0.0f;
    s.scale.x = s.scale.y = 1.0f;
    s.pivot.x = s.pivot.y = 0.0f;
    s.rotation = 0.0f;
    s.opacity = 255u;
    s.color_key_enabled = false;
    s.depth_test = s.depth_write = false;
    *out = s;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_sprite_frame(PIXELICA2D_Sprite *sprite, uint32_t frame, uint16_t width, uint16_t height)
{
    PIXELICA_Texture tex;
    uint32_t w = width, h = height, cols, rows;

    if (sprite == NULL || sprite->texture == NULL || w == 0u || h == 0u) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    tex = *sprite->texture;
    if (!pixelica2d_texture_ok(&tex) || (uint32_t)tex.width % w != 0u || (uint32_t)tex.height % h != 0u) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    cols = (uint32_t)tex.width / w;
    rows = (uint32_t)tex.height / h;
    if (frame >= cols * rows) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    sprite->source.x = (int32_t)((frame % cols) * w);
    sprite->source.y = (int32_t)((frame / cols) * h);
    sprite->source.width = (int32_t)w;
    sprite->source.height = (int32_t)h;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_draw_sprite(PIXELICA2D_Context *ctx, const PIXELICA2D_Sprite *sprite)
{
    PIXELICA2D_Target t;
    PIXELICA2D_SpriteJob job;
    uint64_t written = 0u;

    if (!pixelica2d_target(ctx, &t) || !pixelica2d_sprite_prepare(&t, sprite, NULL, &job, 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixelica2d_sprite_draw(&t, &job, &written);
    pixelica2d_account(ctx, 1u, written);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_draw_sprite_ex(PIXELICA2D_Context *ctx, const PIXELICA2D_Sprite *sprite,
                                          const PIXELICA_DrawOptions *options)
{
    PIXELICA2D_Target t;
    PIXELICA2D_SpriteJob job;
    PIXELICA_DrawOptions o;
    PIXELICA2D_Sprite sp;
    PIXELICA2D_Mem mem;
    uint64_t written = 0u;

    if (options == NULL) {
        return pixelica2d_draw_sprite(ctx, sprite);
    }
    if (sprite == NULL || !pixelica2d_target(ctx, &t) ||
        !pixelica2d_mem(options, 1u, sizeof *options, &mem) || pixelica2d_hits(&t, mem, NULL)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    o = *options;
    if ((int)o.wrap_u != (int)PIXELICA_WRAP_CLAMP && (int)o.wrap_u != (int)PIXELICA_WRAP_REPEAT) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if ((int)o.wrap_v != (int)PIXELICA_WRAP_CLAMP && (int)o.wrap_v != (int)PIXELICA_WRAP_REPEAT) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if ((int)o.alpha_mode < (int)PIXELICA_ALPHA_OPAQUE || (int)o.alpha_mode > (int)PIXELICA_ALPHA_ADD) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    sp = *sprite;
    if (o.alpha_mode == PIXELICA_ALPHA_BLEND || o.alpha_mode == PIXELICA_ALPHA_ADD) {
        sp.depth_write = false;
    }
    if (!pixelica2d_sprite_prepare(&t, &sp, NULL, &job, 1)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (o.texture_alpha == NULL) {
        if (o.texture_alpha_bytes != 0u) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    } else {
        uint64_t need = (uint64_t)job.tex_w * (uint64_t)job.tex_h;

        if ((uint64_t)o.texture_alpha_bytes < need || !pixelica2d_mem(o.texture_alpha, need, 1u, &mem) ||
            pixelica2d_hits(&t, mem, NULL)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    job.ex = true;
    job.repeat_u = o.wrap_u == PIXELICA_WRAP_REPEAT;
    job.repeat_v = o.wrap_v == PIXELICA_WRAP_REPEAT;
    job.amap = o.texture_alpha;
    job.alpha_mode = (int)o.alpha_mode;
    job.opacity = o.opacity;
    job.cutoff = o.alpha_cutoff;
    pixelica2d_sprite_draw(&t, &job, &written);
    pixelica2d_account(ctx, 1u, written);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_fill_rect(PIXELICA2D_Context *ctx, const PIXELICA2D_Rect *rect, uint16_t color, uint8_t opacity)
{
    PIXELICA2D_Target t;
    PIXELICA2D_Rect r;
    int64_t x0, y0, x1, y1;
    uint32_t alpha = opacity;
    uint64_t written = 0u;

    if (!pixelica2d_target(ctx, &t) || rect == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    r = *rect;
    if (r.width < 0 || r.height < 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    x0 = r.x > t.clip.x ? r.x : t.clip.x;
    y0 = r.y > t.clip.y ? r.y : t.clip.y;
    x1 = (int64_t)r.x + r.width;
    y1 = (int64_t)r.y + r.height;
    x1 = x1 < (int64_t)t.clip.x + t.clip.width ? x1 : (int64_t)t.clip.x + t.clip.width;
    y1 = y1 < (int64_t)t.clip.y + t.clip.height ? y1 : (int64_t)t.clip.y + t.clip.height;
    if (alpha != 0u && x0 < x1 && y0 < y1) {
        int32_t x, y;

        for (y = (int32_t)y0; y < (int32_t)y1; ++y) {
            uint16_t *crow = pixelica2d_color_row(&t, y);

            for (x = (int32_t)x0; x < (int32_t)x1; ++x) {
                if (alpha == 255u) {
                    crow[x] = color;
                } else {
                    crow[x] = pixelica2d_blend(color, crow[x], alpha);
                }
            }
        }
        written = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
    }
    pixelica2d_account(ctx, 1u, written);
    return PIXELICA_OK;
}

/* Painter order: does item a belong strictly after item b? */
static int pixelica2d_item_after(const PIXELICA2D_Item *a, const PIXELICA2D_Item *b)
{
    if (a->layer != b->layer) {
        return a->layer > b->layer;
    }
    return a->sort_y > b->sort_y;
}

PIXELICA_Status pixelica2d_draw_list(PIXELICA2D_Context *ctx, const PIXELICA2D_Item *items, size_t count, size_t *order,
                           size_t order_count)
{
    PIXELICA2D_Target t;
    PIXELICA2D_SpriteJob job;
    PIXELICA2D_Mem items_mem, order_mem, ctx_mem;
    uint64_t written = 0u;
    size_t i;

    if (!pixelica2d_target(ctx, &t) || count > PIXELICA2D_MAX_ITEMS) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (count == 0u) {
        return PIXELICA_OK;
    }
    if (items == NULL || order == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (order_count < count) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    if (!pixelica2d_mem(items, count, sizeof *items, &items_mem) || !pixelica2d_mem(order, count, sizeof *order, &order_mem) ||
        !pixelica2d_mem(ctx, 1u, sizeof *ctx, &ctx_mem) || pixelica2d_hits(&t, items_mem, NULL) ||
        pixelica2d_hits(&t, order_mem, &items_mem) || pixelica2d_overlap(order_mem, ctx_mem)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; ++i) {
        if (!isfinite(items[i].sort_y) || !pixelica2d_sprite_prepare(&t, &items[i].sprite, &order_mem, &job, 0)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    /* Stable insertion sort: O(count) when nearly sorted, O(count^2) worst. */
    for (i = 0; i < count; ++i) {
        size_t j = i;

        while (j > 0u && pixelica2d_item_after(&items[order[j - 1u]], &items[i])) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = i;
    }

    /* Nothing read here was written since validation, so this re-prepare
     * cannot fail; checking it keeps the draw path self-contained. */
    for (i = 0; i < count; ++i) {
        if (pixelica2d_sprite_prepare(&t, &items[order[i]].sprite, NULL, &job, 0)) {
            pixelica2d_sprite_draw(&t, &job, &written);
        }
    }
    pixelica2d_account(ctx, count, written);
    return PIXELICA_OK;
}

/* Clamp t(p) into [0, limit) and return the integer texel coordinate. */
static uint32_t pixelica2d_axis_texel(const PIXELICA2D_Axis *a, int32_t p)
{
    double v = pixelica2d_axis_at(a, p);

    if (!(v >= 0.0)) {
        v = 0.0;
    } else if (v >= a->limit) {
        v = a->limit - 1.0;
    }
    return (uint32_t)v;
}

PIXELICA_Status pixelica2d_draw_tilemap(PIXELICA2D_Context *ctx, const PIXELICA2D_Tilemap *map)
{
    PIXELICA2D_Target t;
    PIXELICA2D_Tilemap m;
    PIXELICA_Texture atlas;
    PIXELICA2D_Shade sh;
    PIXELICA2D_Mem tiles_mem;
    uint32_t cols, rows, acols, ids, r, c, tw, th;
    int32_t x0, x1, y0, y1, x, y;
    uint64_t draws = 0u, written = 0u;

    if (!pixelica2d_target(ctx, &t) || map == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    m = *map;
    cols = m.cols;
    rows = m.rows;
    tw = m.tile_width;
    th = m.tile_height;
    if (!pixelica2d_texture(&t, m.atlas, NULL, &atlas) || m.tiles == NULL || tw == 0u || th == 0u ||
        (uint32_t)atlas.width % tw != 0u || (uint32_t)atlas.height % th != 0u || cols == 0u || rows == 0u ||
        m.stride_tiles < cols || cols * rows > PIXELICA2D_MAX_TILES || !pixelica2d_finite2(m.position) ||
        !isfinite(m.scale) || !(m.scale > 0.0f) ||
        !pixelica2d_shade(&t, m.depth, m.opacity, m.color_key_enabled, m.color_key, m.depth_test, m.depth_write,
                     &sh) ||
        !pixelica2d_mem(m.tiles, (uint64_t)(rows - 1u) * m.stride_tiles + cols, sizeof(uint16_t), &tiles_mem) ||
        pixelica2d_hits(&t, tiles_mem, NULL)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    acols = (uint32_t)atlas.width / tw;
    ids = acols * ((uint32_t)atlas.height / th);
    for (r = 0; r < rows; ++r) {
        const uint16_t *cells = m.tiles + (size_t)r * m.stride_tiles;

        for (c = 0; c < cols; ++c) {
            if (cells[c] != PIXELICA2D_EMPTY_TILE && (uint32_t)cells[c] >= ids) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }

    {
        const double iz = 1.0 / (double)t.camera.zoom;
        const double inv = 1.0 / (double)m.scale;
        const PIXELICA2D_Axis ax = pixelica2d_axis(iz, (double)t.camera.position.x - (double)m.position.x, inv, 0.0,
                                         (double)cols * (double)tw);
        const PIXELICA2D_Axis ay = pixelica2d_axis(iz, (double)t.camera.position.y - (double)m.position.y, inv, 0.0,
                                         (double)rows * (double)th);

        x0 = t.clip.x;
        x1 = t.clip.x + t.clip.width;
        y0 = t.clip.y;
        y1 = t.clip.y + t.clip.height;
        pixelica2d_axis_span(&ax, &x0, &x1);
        pixelica2d_axis_span(&ay, &y0, &y1);
        if (x0 < x1 && y0 < y1) {
            /* Visible cell rectangle: one draw per non-empty cell in it. */
            uint32_t c0 = pixelica2d_axis_texel(&ax, x0) / tw, c1 = pixelica2d_axis_texel(&ax, x1 - 1) / tw;
            uint32_t r0 = pixelica2d_axis_texel(&ay, y0) / th, r1 = pixelica2d_axis_texel(&ay, y1 - 1) / th;

            for (r = r0; r <= r1; ++r) {
                for (c = c0; c <= c1; ++c) {
                    if (m.tiles[(size_t)r * m.stride_tiles + c] != PIXELICA2D_EMPTY_TILE) {
                        ++draws;
                    }
                }
            }
            for (y = y0; y < y1 && sh.alpha != 0u; ++y) {
                double ty = pixelica2d_axis_at(&ay, y);
                const uint16_t *cells, *src = NULL;
                uint16_t *crow = pixelica2d_color_row(&t, y);
                uint16_t *drow = pixelica2d_depth_row(&t, y);
                uint32_t texel_y, row, v, last = UINT32_MAX;

                if (!(ty >= 0.0 && ty < ay.limit)) {
                    continue;
                }
                texel_y = (uint32_t)ty;
                row = texel_y / th;
                v = texel_y - row * th;
                cells = m.tiles + (size_t)row * m.stride_tiles;
                for (x = x0; x < x1; ++x) {
                    double tx = pixelica2d_axis_at(&ax, x);
                    uint32_t texel_x, col;

                    if (!(tx >= 0.0 && tx < ax.limit)) {
                        continue;
                    }
                    texel_x = (uint32_t)tx;
                    col = texel_x / tw;
                    if (col != last) {
                        uint32_t id = cells[col];

                        last = col;
                        src = NULL;
                        if (id != PIXELICA2D_EMPTY_TILE) {
                            src = atlas.pixels + ((size_t)(id / acols) * th + v) * (size_t)atlas.width +
                                  (size_t)(id % acols) * tw;
                        }
                    }
                    if (src != NULL) {
                        pixelica2d_plot(&sh, crow + x, drow != NULL ? drow + x : NULL, src[texel_x - col * tw],
                                   &written);
                    }
                }
            }
        }
    }
    pixelica2d_account(ctx, draws, written);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_draw_text(PIXELICA2D_Context *ctx, const PIXELICA2D_Font *font, const char *text, size_t length,
                           int32_t x, int32_t y, uint16_t color, uint8_t opacity)
{
    PIXELICA2D_Target t;
    PIXELICA2D_Font f;
    PIXELICA2D_Mem mem;
    PIXELICA2D_Shade sh;
    uint32_t gw, gh, first, glyphs;
    int64_t pen_x = x, pen_y = y, cx0, cx1, cy0, cy1;
    uint64_t written = 0u;
    size_t i;

    if (!pixelica2d_target(ctx, &t) || font == NULL || length > PIXELICA2D_MAX_TEXT_BYTES || (length > 0u && text == NULL)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    f = *font;
    gw = f.width;
    gh = f.height;
    first = f.first_char;
    glyphs = f.glyph_count;
    if (f.rows == NULL || gw < 1u || gw > 8u || gh < 1u || gh > 32u || glyphs < 1u || glyphs > 256u ||
        first + glyphs > 256u || !pixelica2d_mem(f.rows, (uint64_t)glyphs * gh, 1u, &mem) ||
        pixelica2d_hits(&t, mem, NULL) ||
        (length > 0u && (!pixelica2d_mem(text, length, 1u, &mem) || pixelica2d_hits(&t, mem, NULL)))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&sh, 0, sizeof sh);
    sh.alpha = opacity;
    cx0 = t.clip.x;
    cx1 = cx0 + t.clip.width;
    cy0 = t.clip.y;
    cy1 = cy0 + t.clip.height;
    for (i = 0; i < length && sh.alpha != 0u; ++i) {
        uint32_t ch = (unsigned char)text[i];
        uint32_t row, col;

        if (ch == (uint32_t)'\n') {
            pen_x = x;
            pen_y += (int64_t)gh + 1;
            continue;
        }
        if (ch >= first && ch - first < glyphs && pen_x < cx1 && pen_x + (int64_t)gw > cx0 && pen_y < cy1 &&
            pen_y + (int64_t)gh > cy0) {
            const uint8_t *glyph = f.rows + (size_t)(ch - first) * gh;

            for (row = 0; row < gh; ++row) {
                int64_t py = pen_y + (int64_t)row;
                uint32_t bits = glyph[row];

                for (col = 0; col < gw && py >= cy0 && py < cy1; ++col) {
                    int64_t px = pen_x + (int64_t)col;

                    if ((bits & (0x80u >> col)) != 0u && px >= cx0 && px < cx1) {
                        pixelica2d_plot(&sh, pixelica2d_color_row(&t, (int32_t)py) + px, NULL, color, &written);
                    }
                }
            }
        }
        pen_x += (int64_t)gw + 1;
    }
    pixelica2d_account(ctx, 1u, written);
    return PIXELICA_OK;
}

static int pixelica2d_iso_ok(const PIXELICA2D_Iso *iso)
{
    return iso != NULL && isfinite(iso->tile_width) && iso->tile_width > 0.0f && isfinite(iso->tile_height) &&
           iso->tile_height > 0.0f && isfinite(iso->height_scale) && iso->height_scale >= 0.0f &&
           pixelica2d_finite2(iso->origin);
}

static PIXELICA_Status pixelica2d_store_vec2(double x, double y, PIXELICA_Vec2 *out)
{
    if (!(fabs(x) <= FLT_MAX && fabs(y) <= FLT_MAX)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    out->x = (float)x;
    out->y = (float)y;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica2d_iso_project(const PIXELICA2D_Iso *iso, PIXELICA_Vec3 map, PIXELICA_Vec2 *out)
{
    double hw, hh;

    if (!pixelica2d_iso_ok(iso) || out == NULL || !isfinite(map.x) || !isfinite(map.y) || !isfinite(map.z)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    hw = (double)iso->tile_width * 0.5;
    hh = (double)iso->tile_height * 0.5;
    return pixelica2d_store_vec2((double)iso->origin.x + ((double)map.x - (double)map.y) * hw,
                            (double)iso->origin.y + ((double)map.x + (double)map.y) * hh -
                                (double)map.z * (double)iso->height_scale,
                            out);
}

PIXELICA_Status pixelica2d_iso_unproject(const PIXELICA2D_Iso *iso, PIXELICA_Vec2 screen, float height, PIXELICA_Vec2 *out)
{
    double a, b;

    if (!pixelica2d_iso_ok(iso) || out == NULL || !pixelica2d_finite2(screen) || !isfinite(height)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* a = x - y, b = x + y after removing the origin and the height lift. */
    a = ((double)screen.x - (double)iso->origin.x) / ((double)iso->tile_width * 0.5);
    b = ((double)screen.y - (double)iso->origin.y + (double)height * (double)iso->height_scale) /
        ((double)iso->tile_height * 0.5);
    return pixelica2d_store_vec2((a + b) * 0.5, (b - a) * 0.5, out);
}
