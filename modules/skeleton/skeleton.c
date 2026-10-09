/*
 * Pixelica - optional skeleton posing and linear blend skinning.
 * See modules/skeleton/include/pixelica/skeleton.h.
 *
 * Both entry points follow the same shape: read every count/pointer into
 * locals, size the caller extents with overflow checks, check the workspace
 * size, reject wrapping or forbidden overlapping address ranges, validate the
 * data, compute every result into the caller workspace, and only then copy
 * the workspace into the outputs. Products of finite floats stay far below
 * DBL_MAX, so double intermediates cannot overflow; each value is checked
 * against the float range before its single conversion to float.
 */
#include <float.h>
#include <math.h>
#include <string.h>

#include "pixelica/skeleton.h"

/* Allowed |sum(weights) - 1| for one vertex, evaluated in double. */
#define PIXELICA_SKIN_WEIGHT_TOLERANCE 1e-4

/* Half-open byte range [begin, end) of one caller array. */
typedef struct PIXELICA_Range {
    uintptr_t begin;
    uintptr_t end;
} PIXELICA_Range;

/* *out = a * b; 0 if the product does not fit size_t. */
static int pixelica_size_mul(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b) {
        return 0;
    }
    *out = a * b;
    return 1;
}

/* 0 if [p, p + bytes) would wrap the address space (an array ending exactly
 * at the top of the address space is conservatively rejected too). */
static int pixelica_range_make(const void *p, size_t bytes, PIXELICA_Range *out)
{
    uintptr_t begin = (uintptr_t)p;

    if ((uintmax_t)bytes > (uintmax_t)(UINTPTR_MAX - begin)) {
        return 0;
    }
    out->begin = begin;
    out->end = begin + (uintptr_t)bytes;
    return 1;
}

/* Both ranges are non-empty and non-wrapping. */
static int pixelica_range_overlap(PIXELICA_Range a, PIXELICA_Range b)
{
    return a.begin < b.end && b.begin < a.end;
}

/* 1 if all 16 elements are finite and the bottom row is (0, 0, 0, 1). */
static int pixelica_affine_valid(const float m[16])
{
    int i;

    for (i = 0; i < 16; ++i) {
        if (!isfinite(m[i])) {
            return 0;
        }
    }
    return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f;
}

/* Copies an affine matrix, canonicalizing the bottom row to +0/+1. */
static void pixelica_affine_copy(float out[16], const float m[16])
{
    memcpy(out, m, 16 * sizeof(float));
    out[3] = 0.0f;
    out[7] = 0.0f;
    out[11] = 0.0f;
    out[15] = 1.0f;
}

/*
 * out = a * b for affine a and b, in double. Returns 0 (out unchanged) if an
 * element does not fit a float. out may alias a or b.
 */
static int pixelica_affine_mul(float out[16], const float a[16], const float b[16])
{
    double d[16];
    float f[16];
    int c;
    int row;

    for (c = 0; c < 4; ++c) {
        for (row = 0; row < 3; ++row) {
            /* b's bottom row is (0, 0, 0, 1): a's translation column only
             * contributes to the translation column of the product. */
            d[c * 4 + row] = (double)a[0 * 4 + row] * (double)b[c * 4 + 0] +
                             (double)a[1 * 4 + row] * (double)b[c * 4 + 1] +
                             (double)a[2 * 4 + row] * (double)b[c * 4 + 2];
        }
    }
    for (row = 0; row < 3; ++row) {
        d[12 + row] += (double)a[12 + row];
    }
    for (c = 0; c < 4; ++c) {
        for (row = 0; row < 3; ++row) {
            double v = d[c * 4 + row];
            if (!(fabs(v) <= (double)FLT_MAX)) {
                return 0;
            }
            f[c * 4 + row] = (float)v;
        }
        f[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
    memcpy(out, f, sizeof f);
    return 1;
}

PIXELICA_Status pixelica_skeleton_pose(const PIXELICA_Skeleton *skeleton,
                            const float (*local)[16],
                            float (*world)[16], float (*palette)[16],
                            float (*scratch)[16], size_t scratch_matrices)
{
    const PIXELICA_Bone *bones;
    uint32_t n;
    uint32_t i;
    size_t needed;
    size_t bone_bytes;
    size_t matrix_bytes;
    size_t scratch_bytes;
    PIXELICA_Range inputs[3];  /* skeleton object, bones, local */
    PIXELICA_Range outputs[3]; /* world, palette, used scratch */
    float (*tmp_world)[16];
    float (*tmp_palette)[16];
    int a;
    int b;

    if (skeleton == NULL || local == NULL || world == NULL ||
        palette == NULL || scratch == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    bones = skeleton->bones;
    n = skeleton->bone_count;
    if (bones == NULL || n == 0 || n > PIXELICA_SKELETON_MAX_BONES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!pixelica_size_mul((size_t)n, 2u, &needed) ||
        !pixelica_size_mul((size_t)n, sizeof(PIXELICA_Bone), &bone_bytes) ||
        !pixelica_size_mul((size_t)n, sizeof local[0], &matrix_bytes) ||
        !pixelica_size_mul(needed, sizeof scratch[0], &scratch_bytes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (scratch_matrices < needed) {
        return PIXELICA_ERR_NO_MEMORY;
    }

    if (!pixelica_range_make(skeleton, sizeof *skeleton, &inputs[0]) ||
        !pixelica_range_make(bones, bone_bytes, &inputs[1]) ||
        !pixelica_range_make(local, matrix_bytes, &inputs[2]) ||
        !pixelica_range_make(world, matrix_bytes, &outputs[0]) ||
        !pixelica_range_make(palette, matrix_bytes, &outputs[1]) ||
        !pixelica_range_make(scratch, scratch_bytes, &outputs[2])) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Every written range is disjoint from every other range; read-only
     * inputs may alias each other. */
    for (a = 0; a < 3; ++a) {
        for (b = 0; b < 3; ++b) {
            if (pixelica_range_overlap(outputs[a], inputs[b]) ||
                (b > a && pixelica_range_overlap(outputs[a], outputs[b]))) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
    }

    /* Whole input validated before scratch is touched. */
    for (i = 0; i < n; ++i) {
        int32_t parent = bones[i].parent;
        if (parent != -1 && !(parent >= 0 && (uint32_t)parent < i)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (!pixelica_affine_valid(bones[i].inverse_bind) ||
            !pixelica_affine_valid(local[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    /* Parent-before-child order means tmp_world[parent] is final here. */
    tmp_world = scratch;
    tmp_palette = scratch + n;
    for (i = 0; i < n; ++i) {
        int32_t parent = bones[i].parent;
        if (parent < 0) {
            pixelica_affine_copy(tmp_world[i], local[i]);
        } else if (!pixelica_affine_mul(tmp_world[i], tmp_world[parent],
                                   local[i])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (!pixelica_affine_mul(tmp_palette[i], tmp_world[i],
                            bones[i].inverse_bind)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    memcpy(world, tmp_world, matrix_bytes);
    memcpy(palette, tmp_palette, matrix_bytes);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_skeleton_skin(const PIXELICA_Vec3 *bind_positions,
                            const PIXELICA_SkinWeight *weights, size_t vertex_count,
                            const float (*palette)[16], uint32_t bone_count,
                            PIXELICA_Vec3 *out_positions,
                            PIXELICA_Vec3 *scratch, size_t scratch_vertices)
{
    size_t vertex_bytes;
    size_t weight_bytes;
    size_t palette_bytes;
    PIXELICA_Range r_bind;
    PIXELICA_Range r_weights;
    PIXELICA_Range r_palette;
    PIXELICA_Range r_out;
    PIXELICA_Range r_scratch;
    size_t v;
    uint32_t j;
    unsigned k;

    if (bind_positions == NULL || weights == NULL || palette == NULL ||
        out_positions == NULL || scratch == NULL || vertex_count == 0 ||
        bone_count == 0 || bone_count > PIXELICA_SKELETON_MAX_BONES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!pixelica_size_mul(vertex_count, sizeof(PIXELICA_Vec3), &vertex_bytes) ||
        !pixelica_size_mul(vertex_count, sizeof(PIXELICA_SkinWeight), &weight_bytes) ||
        !pixelica_size_mul((size_t)bone_count, sizeof palette[0],
                      &palette_bytes)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (scratch_vertices < vertex_count) {
        return PIXELICA_ERR_NO_MEMORY;
    }

    if (!pixelica_range_make(bind_positions, vertex_bytes, &r_bind) ||
        !pixelica_range_make(weights, weight_bytes, &r_weights) ||
        !pixelica_range_make(palette, palette_bytes, &r_palette) ||
        !pixelica_range_make(out_positions, vertex_bytes, &r_out) ||
        !pixelica_range_make(scratch, vertex_bytes, &r_scratch)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (pixelica_range_overlap(r_scratch, r_bind) ||
        pixelica_range_overlap(r_scratch, r_weights) ||
        pixelica_range_overlap(r_scratch, r_palette) ||
        pixelica_range_overlap(r_scratch, r_out) ||
        pixelica_range_overlap(r_out, r_weights) ||
        pixelica_range_overlap(r_out, r_palette)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* Same length, so an equal start means exactly the same array. */
    if (r_out.begin != r_bind.begin && pixelica_range_overlap(r_out, r_bind)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* The whole palette is validated, referenced or not. */
    for (j = 0; j < bone_count; ++j) {
        if (!pixelica_affine_valid(palette[j])) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    /* One pass validates and skins each vertex into scratch; a failure may
     * leave scratch partly written but never reaches out_positions. */
    for (v = 0; v < vertex_count; ++v) {
        const PIXELICA_SkinWeight *sw = &weights[v];
        PIXELICA_Vec3 p = bind_positions[v];
        double sum = 0.0;
        double anchor[3];
        double acc[3];
        int have_anchor = 0;
        int c;

        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        for (k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
            float w = sw->weights[k];
            /* Also rejects NaN and infinities. */
            if (!(w >= 0.0f && w <= 1.0f)) {
                return PIXELICA_ERR_INVALID_ARG;
            }
            if (w > 0.0f) {
                if ((uint32_t)sw->bones[k] >= bone_count) {
                    return PIXELICA_ERR_INVALID_ARG;
                }
                sum += (double)w;
            }
        }
        if (!(sum > 0.0) || fabs(sum - 1.0) > PIXELICA_SKIN_WEIGHT_TOLERANCE) {
            return PIXELICA_ERR_INVALID_ARG;
        }

        /*
         * acc = anchor + sum_k (w_k / sum) * (t_k - anchor), anchored at the
         * first influence's transformed point t_0. Equal t_k give exactly
         * t_0 (every difference is 0), so a point all influences agree on
         * stays fixed regardless of the weight sum's roundoff.
         */
        acc[0] = acc[1] = acc[2] = 0.0;
        anchor[0] = anchor[1] = anchor[2] = 0.0;
        for (k = 0; k < PIXELICA_SKIN_INFLUENCES; ++k) {
            const float *m;
            double t[3];
            double n;
            float w = sw->weights[k];

            if (!(w > 0.0f)) {
                continue;
            }
            m = palette[sw->bones[k]];
            for (c = 0; c < 3; ++c) {
                t[c] = (double)m[0 + c] * (double)p.x +
                       (double)m[4 + c] * (double)p.y +
                       (double)m[8 + c] * (double)p.z + (double)m[12 + c];
            }
            if (!have_anchor) {
                anchor[0] = t[0];
                anchor[1] = t[1];
                anchor[2] = t[2];
                have_anchor = 1;
                continue;
            }
            n = (double)w / sum;
            for (c = 0; c < 3; ++c) {
                acc[c] += n * (t[c] - anchor[c]);
            }
        }
        for (c = 0; c < 3; ++c) {
            acc[c] += anchor[c];
            if (!(fabs(acc[c]) <= (double)FLT_MAX)) {
                return PIXELICA_ERR_INVALID_ARG;
            }
        }
        scratch[v].x = (float)acc[0];
        scratch[v].y = (float)acc[1];
        scratch[v].z = (float)acc[2];
    }

    /* Every read of bind_positions is done, so in-place skinning is safe. */
    memcpy(out_positions, scratch, vertex_bytes);
    return PIXELICA_OK;
}
