/*
 * Pixelica - optional CPU postprocessing shaders for RGB565 framebuffers.
 * See modules/shaders/include/pixelica/shaders.h.
 *
 * Every entry point copies the caller structs into locals, sizes every
 * pixel range with overflow and address-wrap checks, validates the fields the
 * effect uses and rejects forbidden overlaps before the first write. Spatial
 * effects (BLUR, OUTLINE, CUSTOM) read only a tight snapshot taken into the
 * caller scratch, so the in-place pass never observes its own output.
 */
#include <string.h>

#include "pixelica/shaders.h"

/* Half-open byte range [lo, hi) of one caller object or array. */
typedef struct PIXELICA_ShRange {
    uintptr_t lo, hi;
} PIXELICA_ShRange;

typedef struct PIXELICA_ShRgb {
    int r, g, b;
} PIXELICA_ShRgb;

/* 0 if count * elem overflows size_t or [p, p + bytes) wraps the address
 * space (an array ending exactly at the top is conservatively rejected). */
static int sh_range(const void *p, size_t count, size_t elem, PIXELICA_ShRange *out)
{
    uintptr_t lo = (uintptr_t)p;
    size_t bytes;

    if (count != 0 && elem > SIZE_MAX / count) {
        return 0;
    }
    bytes = count * elem;
    if ((uintmax_t)bytes > (uintmax_t)(UINTPTR_MAX - lo)) {
        return 0;
    }
    out->lo = lo;
    out->hi = lo + (uintptr_t)bytes;
    return 1;
}

/* Both ranges are non-empty and non-wrapping. */
static int sh_overlap(PIXELICA_ShRange a, PIXELICA_ShRange b)
{
    return a.lo < b.hi && b.lo < a.hi;
}

/* Validates a pixel layout and returns its byte range: pixels != NULL,
 * 1 .. MAX_DIMENSION width/height, stride >= width, no size overflow/wrap. */
static int sh_layout(const uint16_t *pixels, uint16_t width, uint16_t height, uint32_t stride,
    PIXELICA_ShRange *out)
{
    size_t rows;
    size_t span;

    if (pixels == NULL || width == 0u || height == 0u ||
        (unsigned)width > PIXELICA_SHADER_MAX_DIMENSION ||
        (unsigned)height > PIXELICA_SHADER_MAX_DIMENSION || stride < (uint32_t)width) {
        return 0;
    }
    rows = (size_t)height - 1u;
    if (rows != 0u && (size_t)stride > (SIZE_MAX - (size_t)width) / rows) {
        return 0;
    }
    span = rows * (size_t)stride + (size_t)width;
    return sh_range(pixels, span, sizeof(uint16_t), out);
}

static int sh_kind_valid(PIXELICA_ShaderKind kind)
{
    int k = (int)kind;

    return k >= (int)PIXELICA_SHADER_TINT && k <= (int)PIXELICA_SHADER_CUSTOM;
}

static int sh_needs_scratch(PIXELICA_ShaderKind kind)
{
    return kind == PIXELICA_SHADER_BLUR || kind == PIXELICA_SHADER_OUTLINE ||
           kind == PIXELICA_SHADER_CUSTOM;
}

/* Finite and in [0, 1]; NaN fails both comparisons, infinities the range. */
static int sh_amount_ok(float amount)
{
    return amount >= 0.0f && amount <= 1.0f;
}

/* Validates the fields `s->kind` uses. For PALETTE also returns the palette
 * byte range in *pal and sets *has_pal. */
static int sh_shader_ok(const PIXELICA_Shader *s, PIXELICA_ShRange *pal, int *has_pal)
{
    *has_pal = 0;
    if (!sh_kind_valid(s->kind)) {
        return 0;
    }
    switch (s->kind) {
    case PIXELICA_SHADER_TINT:
    case PIXELICA_SHADER_GRAYSCALE:
    case PIXELICA_SHADER_INVERT:
    case PIXELICA_SHADER_SCANLINES:
    case PIXELICA_SHADER_VIGNETTE:
    case PIXELICA_SHADER_OUTLINE:
        return sh_amount_ok(s->amount);
    case PIXELICA_SHADER_POSTERIZE:
        return (unsigned)s->levels >= PIXELICA_SHADER_MIN_LEVELS &&
               (unsigned)s->levels <= PIXELICA_SHADER_MAX_LEVELS;
    case PIXELICA_SHADER_PALETTE:
        if (s->palette == NULL || s->palette_count == 0u ||
            (unsigned)s->palette_count > PIXELICA_SHADER_MAX_PALETTE ||
            !sh_range(s->palette, s->palette_count, sizeof(uint16_t), pal)) {
            return 0;
        }
        *has_pal = 1;
        return 1;
    case PIXELICA_SHADER_BLUR:
        return (unsigned)s->radius >= PIXELICA_SHADER_MIN_RADIUS &&
               (unsigned)s->radius <= PIXELICA_SHADER_MAX_RADIUS;
    case PIXELICA_SHADER_CUSTOM:
        return s->callback != NULL;
    default:
        return 0;
    }
}

/*
 * Full non-writing validation of one apply call. On success *need is the
 * scratch pixel count the effect uses (0 when none).
 */
static PIXELICA_Status sh_validate(const PIXELICA_ShaderSurface *surface,
    const PIXELICA_Shader *shader, const uint16_t *scratch, size_t scratch_count, size_t *need)
{
    PIXELICA_ShRange r_dst, r_surf, r_sh, r_scr;
    PIXELICA_ShRange r_pal = { 0u, 0u };
    int has_pal;
    size_t n;

    if (surface == NULL || shader == NULL ||
        !sh_layout(surface->pixels, surface->width, surface->height, surface->stride, &r_dst) ||
        !sh_shader_ok(shader, &r_pal, &has_pal) ||
        !sh_range(surface, 1u, sizeof(*surface), &r_surf) ||
        !sh_range(shader, 1u, sizeof(*shader), &r_sh)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (sh_overlap(r_dst, r_surf) || sh_overlap(r_dst, r_sh) ||
        (has_pal && sh_overlap(r_dst, r_pal))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    n = 0;
    if (sh_needs_scratch(shader->kind)) {
        n = (size_t)surface->width * (size_t)surface->height;
        if (scratch_count < n) {
            return PIXELICA_ERR_CAPACITY;
        }
        if (scratch == NULL || !sh_range(scratch, n, sizeof(uint16_t), &r_scr) ||
            sh_overlap(r_scr, r_dst) || sh_overlap(r_scr, r_surf) || sh_overlap(r_scr, r_sh) ||
            (has_pal && sh_overlap(r_scr, r_pal))) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    *need = n;
    return PIXELICA_OK;
}

/* ---- color math ---------------------------------------------------------- */

static PIXELICA_ShRgb sh_expand(uint16_t c)
{
    PIXELICA_ShRgb o;
    int r5 = (c >> 11) & 31;
    int g6 = (c >> 5) & 63;
    int b5 = c & 31;

    o.r = (r5 << 3) | (r5 >> 2);
    o.g = (g6 << 2) | (g6 >> 4);
    o.b = (b5 << 3) | (b5 >> 2);
    return o;
}

static uint16_t sh_pack(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int sh_luma(PIXELICA_ShRgb c)
{
    return (77 * c.r + 150 * c.g + 29 * c.b + 128) / 256;
}

/* a + (b - a) * t rounded to nearest, t in [0, 1]. */
static int sh_lerp8(int a, int b, float t)
{
    int i = (int)((float)a + (float)(b - a) * t + 0.5f);

    return i < 0 ? 0 : (i > 255 ? 255 : i);
}

/* Nearest of `levels` evenly spaced values in 0 .. 255 (n = levels - 1). */
static int sh_posterize(int c, int n)
{
    int q = (2 * c * n + 255) / 510;

    return (q * 510 + n) / (2 * n);
}

static uint16_t sh_nearest(PIXELICA_ShRgb s, const uint16_t *palette, uint16_t count)
{
    uint16_t best = palette[0];
    int best_d = -1;
    uint16_t i;

    for (i = 0; i < count; ++i) {
        PIXELICA_ShRgb p = sh_expand(palette[i]);
        int dr = p.r - s.r, dg = p.g - s.g, db = p.b - s.b;
        int d = dr * dr + dg * dg + db * db;

        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = palette[i];
        }
    }
    return best;
}

/* clamp(nx*nx + ny*ny, 0, 1); a single-pixel axis contributes 0. */
static float sh_vignette(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    float nx = 0.0f, ny = 0.0f, d;

    if (w > 1u) {
        nx = (float)((int32_t)(2u * x) - (int32_t)(w - 1u)) / (float)(w - 1u);
    }
    if (h > 1u) {
        ny = (float)((int32_t)(2u * y) - (int32_t)(h - 1u)) / (float)(h - 1u);
    }
    d = nx * nx + ny * ny;
    return d > 1.0f ? 1.0f : d;
}

/* Per-pixel (non-spatial) effects. */
static uint16_t sh_point(const PIXELICA_Shader *sh, uint16_t c, uint32_t x, uint32_t y, uint32_t w,
    uint32_t h)
{
    PIXELICA_ShRgb s = sh_expand(c);
    PIXELICA_ShRgb t = { 0, 0, 0 };
    float amount = sh->amount;

    switch (sh->kind) {
    case PIXELICA_SHADER_TINT:
        t = sh_expand(sh->color);
        break;
    case PIXELICA_SHADER_GRAYSCALE:
        t.r = t.g = t.b = sh_luma(s);
        break;
    case PIXELICA_SHADER_INVERT:
        t.r = 255 - s.r;
        t.g = 255 - s.g;
        t.b = 255 - s.b;
        break;
    case PIXELICA_SHADER_POSTERIZE: {
        int n = (int)sh->levels - 1;
        return sh_pack(sh_posterize(s.r, n), sh_posterize(s.g, n), sh_posterize(s.b, n));
    }
    case PIXELICA_SHADER_PALETTE:
        return sh_nearest(s, sh->palette, sh->palette_count);
    case PIXELICA_SHADER_SCANLINES:
        if ((y & 1u) == 0u) {
            return c;
        }
        break;
    case PIXELICA_SHADER_VIGNETTE:
        amount *= sh_vignette(x, y, w, h);
        break;
    default:
        return c;
    }
    return sh_pack(sh_lerp8(s.r, t.r, amount), sh_lerp8(s.g, t.g, amount),
        sh_lerp8(s.b, t.b, amount));
}

static uint32_t sh_clamp(int32_t v, uint32_t n)
{
    if (v < 0) {
        return 0u;
    }
    return (uint32_t)v >= n ? n - 1u : (uint32_t)v;
}

static uint16_t sh_blur(const uint16_t *src, uint32_t w, uint32_t h, uint32_t x, uint32_t y, int r)
{
    int sr = 0, sg = 0, sb = 0, dx, dy;
    int n = (2 * r + 1) * (2 * r + 1);

    for (dy = -r; dy <= r; ++dy) {
        uint32_t yy = sh_clamp((int32_t)y + dy, h);
        for (dx = -r; dx <= r; ++dx) {
            uint32_t xx = sh_clamp((int32_t)x + dx, w);
            PIXELICA_ShRgb c = sh_expand(src[(size_t)yy * w + xx]);
            sr += c.r;
            sg += c.g;
            sb += c.b;
        }
    }
    return sh_pack((sr + n / 2) / n, (sg + n / 2) / n, (sb + n / 2) / n);
}

static int sh_edge(const uint16_t *src, uint32_t w, uint32_t h, uint32_t x, uint32_t y, int thr)
{
    int l = sh_luma(sh_expand(src[(size_t)y * w + x]));
    uint32_t xs[4], ys[4];
    int i;

    xs[0] = x > 0u ? x - 1u : 0u;
    ys[0] = y;
    xs[1] = x + 1u < w ? x + 1u : x;
    ys[1] = y;
    xs[2] = x;
    ys[2] = y > 0u ? y - 1u : 0u;
    xs[3] = x;
    ys[3] = y + 1u < h ? y + 1u : y;
    for (i = 0; i < 4; ++i) {
        int d = sh_luma(sh_expand(src[(size_t)ys[i] * w + xs[i]])) - l;
        if (d < 0) {
            d = -d;
        }
        if (d > thr) {
            return 1;
        }
    }
    return 0;
}

/* Runs a validated effect. `s` and `sh` are private copies. */
static void sh_run(const PIXELICA_ShaderSurface *s, const PIXELICA_Shader *sh, uint16_t *scratch)
{
    uint32_t w = s->width, h = s->height, x, y;
    size_t stride = (size_t)s->stride;
    const uint16_t *src = scratch;
    int thr = 0; /* OUTLINE luma threshold; amount is unvalidated for other kinds */

    if (sh->kind == PIXELICA_SHADER_OUTLINE) {
        thr = (int)(sh->amount * 255.0f + 0.5f);
    }

    if (sh_needs_scratch(sh->kind)) {
        for (y = 0; y < h; ++y) {
            memcpy(scratch + (size_t)y * w, s->pixels + (size_t)y * stride,
                (size_t)w * sizeof(uint16_t));
        }
    }
    for (y = 0; y < h; ++y) {
        uint16_t *row = s->pixels + (size_t)y * stride;
        for (x = 0; x < w; ++x) {
            switch (sh->kind) {
            case PIXELICA_SHADER_BLUR:
                row[x] = sh_blur(src, w, h, x, y, (int)sh->radius);
                break;
            case PIXELICA_SHADER_OUTLINE:
                if (sh_edge(src, w, h, x, y, thr)) {
                    row[x] = sh->color;
                }
                break;
            case PIXELICA_SHADER_CUSTOM: {
                PIXELICA_ShaderImage image;
                PIXELICA_ShaderPixel px;

                image.pixels = src;
                image.width = s->width;
                image.height = s->height;
                image.stride = w;
                px.source = &image;
                px.x = (uint16_t)x;
                px.y = (uint16_t)y;
                px.color = src[(size_t)y * w + x];
                row[x] = sh->callback(&px, sh->userdata);
                break;
            }
            default:
                row[x] = sh_point(sh, row[x], x, y, w, h);
                break;
            }
        }
    }
}

/* ---- public API ---------------------------------------------------------- */

PIXELICA_Status pixelica_shader_default(PIXELICA_ShaderKind kind, PIXELICA_Shader *out)
{
    PIXELICA_Shader s;

    if (out == NULL || !sh_kind_valid(kind)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.amount = kind == PIXELICA_SHADER_OUTLINE ? 0.2f : 1.0f;
    s.color = kind == PIXELICA_SHADER_OUTLINE ? (uint16_t)0x0000u : (uint16_t)0xFFFFu;
    s.levels = 4u;
    s.radius = 1u;
    s.palette = NULL;
    s.palette_count = 0u;
    s.callback = NULL;
    s.userdata = NULL;
    *out = s;
    return PIXELICA_OK;
}

size_t pixelica_shader_scratch_pixels(const PIXELICA_ShaderSurface *surface,
    const PIXELICA_Shader *shader)
{
    PIXELICA_ShRange r;
    int has_pal;

    if (surface == NULL || shader == NULL ||
        !sh_layout(surface->pixels, surface->width, surface->height, surface->stride, &r) ||
        !sh_shader_ok(shader, &r, &has_pal) || !sh_needs_scratch(shader->kind)) {
        return 0u;
    }
    return (size_t)surface->width * (size_t)surface->height;
}

PIXELICA_Status pixelica_shader_apply(PIXELICA_ShaderSurface *surface, const PIXELICA_Shader *shader,
    uint16_t *scratch, size_t scratch_count)
{
    PIXELICA_ShaderSurface s;
    PIXELICA_Shader sh;
    size_t need;
    PIXELICA_Status st = sh_validate(surface, shader, scratch, scratch_count, &need);

    if (st != PIXELICA_OK) {
        return st;
    }
    s = *surface;
    sh = *shader;
    sh_run(&s, &sh, scratch);
    return PIXELICA_OK;
}

uint16_t pixelica_shader_sample(const PIXELICA_ShaderImage *image, int32_t x, int32_t y)
{
    PIXELICA_ShRange r;

    if (image == NULL || !sh_layout(image->pixels, image->width, image->height, image->stride, &r)) {
        return 0u;
    }
    return image->pixels[(size_t)sh_clamp(y, image->height) * (size_t)image->stride +
                         (size_t)sh_clamp(x, image->width)];
}

/* ---- presenter ----------------------------------------------------------- */

static uint64_t sh_time_us(void *userdata)
{
    const PIXELICA_ShaderPresenter *p = (const PIXELICA_ShaderPresenter *)userdata;

    if (p == NULL || p->upstream.time_us == NULL) {
        return 0u;
    }
    return p->upstream.time_us(p->upstream.userdata);
}

static uint32_t sh_read_buttons(void *userdata)
{
    const PIXELICA_ShaderPresenter *p = (const PIXELICA_ShaderPresenter *)userdata;

    if (p == NULL || p->upstream.read_buttons == NULL) {
        return 0u;
    }
    return p->upstream.read_buttons(p->upstream.userdata);
}

static void sh_log(void *userdata, const char *message)
{
    const PIXELICA_ShaderPresenter *p = (const PIXELICA_ShaderPresenter *)userdata;

    if (p != NULL && p->upstream.log != NULL) {
        p->upstream.log(p->upstream.userdata, message);
    }
}

static bool sh_present(void *userdata, const uint16_t *pixels, uint16_t width, uint16_t height,
    uint32_t stride)
{
    const PIXELICA_ShaderPresenter *pp = (const PIXELICA_ShaderPresenter *)userdata;
    PIXELICA_ShaderPresenter p;
    PIXELICA_ShaderSurface surface;
    PIXELICA_ShRange r_frame, r_self, r_output, r_scr, r_pal;
    int has_pal;
    size_t n, need;
    uint32_t y;

    if (pp == NULL) {
        return false;
    }
    p = *pp; /* the shader callback cannot retarget this frame */
    if (p.upstream.present_rgb565 == NULL || !sh_layout(pixels, width, height, stride, &r_frame) ||
        !sh_range(pp, 1u, sizeof(*pp), &r_self)) {
        return false;
    }
    n = (size_t)width * (size_t)height;
    if (p.output == NULL || p.output_capacity < n ||
        !sh_range(p.output, p.output_capacity, sizeof(uint16_t), &r_output) ||
        sh_overlap(r_frame, r_self) || sh_overlap(r_frame, r_output) ||
        sh_overlap(r_output, r_self) ||
        !sh_shader_ok(&p.shader, &r_pal, &has_pal) ||
        (has_pal && (sh_overlap(r_pal, r_self) || sh_overlap(r_pal, r_output))) ||
        (p.scratch == NULL && p.scratch_capacity != 0u)) {
        return false;
    }
    surface.pixels = p.output;
    surface.width = width;
    surface.height = height;
    surface.stride = width;
    if (sh_validate(&surface, &p.shader, p.scratch, p.scratch_capacity, &need) != PIXELICA_OK) {
        return false;
    }
    /* All supplied buffers stay disjoint even for a point effect: the shader
     * may change to a spatial effect between frames. Check whole capacities. */
    if (p.scratch_capacity != 0u &&
        (!sh_range(p.scratch, p.scratch_capacity, sizeof(uint16_t), &r_scr) ||
         sh_overlap(r_scr, r_frame) || sh_overlap(r_scr, r_self) ||
         sh_overlap(r_scr, r_output) || (has_pal && sh_overlap(r_scr, r_pal)))) {
        return false;
    }
    for (y = 0; y < height; ++y) {
        memcpy(p.output + (size_t)y * width, pixels + (size_t)y * (size_t)stride,
            (size_t)width * sizeof(uint16_t));
    }
    sh_run(&surface, &p.shader, p.scratch);
    return p.upstream.present_rgb565(p.upstream.userdata, p.output, width, height, width);
}

PIXELICA_Status pixelica_shader_presenter_init(PIXELICA_ShaderPresenter *out,
    const PIXELICA_PlatformOps *upstream, const PIXELICA_Shader *shader, uint16_t *output,
    size_t output_capacity, uint16_t *scratch, size_t scratch_capacity)
{
    PIXELICA_ShaderPresenter p;
    PIXELICA_ShRange r_out, r_up, r_sh, r_output;
    PIXELICA_ShRange r_scr = { 0u, 0u }, r_pal = { 0u, 0u };
    int has_pal, needs;

    if (out == NULL || upstream == NULL || shader == NULL || output == NULL ||
        output_capacity == 0u || upstream->time_us == NULL || upstream->read_buttons == NULL ||
        upstream->present_rgb565 == NULL || !sh_shader_ok(shader, &r_pal, &has_pal) ||
        (scratch == NULL && scratch_capacity != 0u)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    needs = sh_needs_scratch(shader->kind);
    if ((needs && (scratch == NULL || scratch_capacity == 0u)) ||
        !sh_range(out, 1u, sizeof(*out), &r_out) ||
        !sh_range(upstream, 1u, sizeof(*upstream), &r_up) ||
        !sh_range(shader, 1u, sizeof(*shader), &r_sh) ||
        !sh_range(output, output_capacity, sizeof(uint16_t), &r_output) ||
        (scratch != NULL && !sh_range(scratch, scratch_capacity, sizeof(uint16_t), &r_scr))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (sh_overlap(r_out, r_up) || sh_overlap(r_out, r_sh) || sh_overlap(r_out, r_output) ||
        (has_pal && sh_overlap(r_out, r_pal)) || sh_overlap(r_output, r_up) ||
        sh_overlap(r_output, r_sh) || (has_pal && sh_overlap(r_output, r_pal))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (scratch_capacity != 0u &&
        (sh_overlap(r_scr, r_out) || sh_overlap(r_scr, r_up) ||
         sh_overlap(r_scr, r_sh) || sh_overlap(r_scr, r_output) ||
         (has_pal && sh_overlap(r_scr, r_pal)))) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    p.upstream = *upstream;
    p.shader = *shader;
    p.ops.userdata = out;
    p.ops.time_us = sh_time_us;
    p.ops.read_buttons = sh_read_buttons;
    p.ops.present_rgb565 = sh_present;
    p.ops.log = upstream->log != NULL ? sh_log : NULL;
    p.output = output;
    p.output_capacity = output_capacity;
    p.scratch = scratch;
    p.scratch_capacity = scratch_capacity;
    *out = p;
    return PIXELICA_OK;
}
