/*
 * Pixelica - 3D adapter for the water module (Pixelica::water3d).
 *
 * See include/pixelica/water3d.h. Implementation notes:
 *
 *   - Grid vertex (i, j) is stored at index j * (cells_x + 1) + i, and its
 *     height AND normal come from pixelica_water_sample() at the caller's
 *     double time. No separate surface evaluation exists in this file, which
 *     is what keeps the drawn water and the physics identical.
 *   - Cell (i, j) emits (a, c, b) and (a, d, c) with
 *     a = (i, j), b = (i+1, j), c = (i+1, j+1), d = (i, j+1).
 *     The right-hand normal of both is +Y, i.e. counter-clockwise seen from
 *     outside, the same convention as the engine's own meshes.
 *   - pixelica_water_mesh_height() picks the triangle by comparing the two
 *     in-cell fractions against the a->c diagonal, so it returns the plane the
 *     rasterizer actually draws rather than a bilinear blend of the corners.
 *   - No heap, no globals, no VLA. Caller arrays are checked for overlap by
 *     byte range before anything is written.
 */
#include "pixelica/water3d.h"

#include <math.h>
#include <stdint.h>

/* Half-open byte range of one caller array. */
typedef struct WaterRange {
    uintptr_t lo, hi;
} WaterRange;

static WaterRange water_range(const void *base, size_t bytes)
{
    WaterRange range;

    range.lo = (uintptr_t)base;
    range.hi = range.lo + (uintptr_t)bytes;
    return range;
}

static int water_overlap(WaterRange a, WaterRange b)
{
    return a.lo < b.hi && b.lo < a.hi;
}

static int water_patch_ok(const PIXELICA_WaterPatch *patch)
{
    if (patch == NULL) {
        return 0;
    }
    if (patch->cells_x == 0u || patch->cells_z == 0u ||
        (uint32_t)patch->cells_x > PIXELICA_WATER_MAX_CELLS ||
        (uint32_t)patch->cells_z > PIXELICA_WATER_MAX_CELLS) {
        return 0;
    }
    if (isfinite((double)patch->center_x) == 0 || isfinite((double)patch->center_z) == 0 ||
        isfinite((double)patch->size_x) == 0 || isfinite((double)patch->size_z) == 0) {
        return 0;
    }
    if (!(patch->size_x > 0.0f) || !(patch->size_z > 0.0f) ||
        patch->size_x > PIXELICA_WATER_MAX_EXTENT || patch->size_z > PIXELICA_WATER_MAX_EXTENT) {
        return 0;
    }
    if (patch->center_x < -PIXELICA_WATER_MAX_COORD ||
        patch->center_x > PIXELICA_WATER_MAX_COORD ||
        patch->center_z < -PIXELICA_WATER_MAX_COORD ||
        patch->center_z > PIXELICA_WATER_MAX_COORD) {
        return 0;
    }
    return 1;
}

uint32_t pixelica_water_mesh_vertex_count(const PIXELICA_WaterPatch *patch)
{
    if (!water_patch_ok(patch)) {
        return 0u;
    }
    return ((uint32_t)patch->cells_x + 1u) * ((uint32_t)patch->cells_z + 1u);
}

uint32_t pixelica_water_mesh_index_count(const PIXELICA_WaterPatch *patch)
{
    if (!water_patch_ok(patch)) {
        return 0u;
    }
    return (uint32_t)patch->cells_x * (uint32_t)patch->cells_z * 6u;
}

/* World X of grid column i, World Z of grid row j. Computed in double from the
 * index so the spacing cannot drift along the row. */
static double water_grid_x(const PIXELICA_WaterPatch *patch, uint32_t i)
{
    return (double)patch->center_x - 0.5 * (double)patch->size_x +
           (double)patch->size_x * (double)i / (double)patch->cells_x;
}

static double water_grid_z(const PIXELICA_WaterPatch *patch, uint32_t j)
{
    return (double)patch->center_z - 0.5 * (double)patch->size_z +
           (double)patch->size_z * (double)j / (double)patch->cells_z;
}

PIXELICA_Status pixelica_water_mesh_build(const PIXELICA_WaterSurface *surface,
                                          const PIXELICA_WaterPatch *patch,
                                          double time_seconds, PIXELICA_WaterMesh *mesh)
{
    uint32_t columns, rows, vertex_count, index_count, i, j, cursor;

    if (mesh == NULL || !water_patch_ok(patch) ||
        pixelica_water_surface_validate(surface) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (isfinite(time_seconds) == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (mesh->vertices == NULL || mesh->normals == NULL || mesh->indices == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    columns = (uint32_t)patch->cells_x + 1u;
    rows = (uint32_t)patch->cells_z + 1u;
    vertex_count = columns * rows;
    index_count = (uint32_t)patch->cells_x * (uint32_t)patch->cells_z * 6u;
    if (mesh->vertex_capacity < vertex_count || mesh->index_capacity < index_count) {
        return PIXELICA_ERR_CAPACITY;
    }
    {
        WaterRange v = water_range(mesh->vertices, (size_t)vertex_count * sizeof mesh->vertices[0]);
        WaterRange n = water_range(mesh->normals, (size_t)vertex_count * sizeof mesh->normals[0]);
        WaterRange k = water_range(mesh->indices, (size_t)index_count * sizeof mesh->indices[0]);

        if (water_overlap(v, n) || water_overlap(v, k) || water_overlap(n, k)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }

    /*
     * Sample first. A grid corner outside PIXELICA_WATER_MAX_COORD (or any
     * non-finite sample) is reported by pixelica_water_sample itself, and
     * because vertex_count / index_count are only written at the very end a
     * failure here leaves the descriptor untouched.
     */
    for (j = 0; j < rows; ++j) {
        double z = water_grid_z(patch, j);

        for (i = 0; i < columns; ++i) {
            double x = water_grid_x(patch, i);
            PIXELICA_WaterSample sample;
            uint32_t index = j * columns + i;
            PIXELICA_Vec3 position;

            if (isfinite(x) == 0 || isfinite(z) == 0 ||
                x < -(double)PIXELICA_WATER_MAX_COORD ||
                x > (double)PIXELICA_WATER_MAX_COORD ||
                z < -(double)PIXELICA_WATER_MAX_COORD ||
                z > (double)PIXELICA_WATER_MAX_COORD) {
                return PIXELICA_ERR_INVALID_ARG;
            }
            if (pixelica_water_sample(surface, (float)x, (float)z, time_seconds, &sample) !=
                PIXELICA_OK) {
                return PIXELICA_ERR_INVALID_ARG;
            }
            position.x = (float)x;
            position.y = sample.height;
            position.z = (float)z;
            mesh->vertices[index] = position;
            mesh->normals[index] = sample.normal;
        }
    }

    cursor = 0u;
    for (j = 0; j < (uint32_t)patch->cells_z; ++j) {
        for (i = 0; i < (uint32_t)patch->cells_x; ++i) {
            uint16_t a = (uint16_t)(j * columns + i);
            uint16_t b = (uint16_t)(j * columns + i + 1u);
            uint16_t c = (uint16_t)((j + 1u) * columns + i + 1u);
            uint16_t d = (uint16_t)((j + 1u) * columns + i);

            /* (a, c, b) and (a, d, c): right-hand normal +Y, i.e. CCW from
             * outside, which is what PIXELICA_CULL_BACK expects. */
            mesh->indices[cursor++] = a;
            mesh->indices[cursor++] = c;
            mesh->indices[cursor++] = b;
            mesh->indices[cursor++] = a;
            mesh->indices[cursor++] = d;
            mesh->indices[cursor++] = c;
        }
    }

    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;
    return PIXELICA_OK;
}

/* TRUE when `mesh` holds a build of exactly `patch`. */
static int water_mesh_matches(const PIXELICA_WaterMesh *mesh, const PIXELICA_WaterPatch *patch)
{
    if (mesh == NULL || mesh->vertices == NULL || mesh->normals == NULL ||
        mesh->indices == NULL) {
        return 0;
    }
    if (mesh->vertex_count == 0u || mesh->index_count == 0u) {
        return 0;
    }
    return mesh->vertex_count == pixelica_water_mesh_vertex_count(patch) &&
           mesh->index_count == pixelica_water_mesh_index_count(patch);
}

PIXELICA_Status pixelica_water_mesh_height(const PIXELICA_WaterMesh *mesh,
                                           const PIXELICA_WaterPatch *patch, float x, float z,
                                           float *out_height)
{
    double min_x, min_z, u, v, fu, fv, height;
    uint32_t columns, cell_i, cell_j, a, b, c, d;

    if (out_height == NULL || !water_patch_ok(patch) || !water_mesh_matches(mesh, patch)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (isfinite((double)x) == 0 || isfinite((double)z) == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    min_x = (double)patch->center_x - 0.5 * (double)patch->size_x;
    min_z = (double)patch->center_z - 0.5 * (double)patch->size_z;
    u = ((double)x - min_x) / (double)patch->size_x * (double)patch->cells_x;
    v = ((double)z - min_z) / (double)patch->size_z * (double)patch->cells_z;
    if (!(u >= 0.0) || !(v >= 0.0) || u > (double)patch->cells_x || v > (double)patch->cells_z) {
        return PIXELICA_ERR_INVALID_ARG; /* outside the patch rectangle */
    }

    cell_i = (uint32_t)floor(u);
    cell_j = (uint32_t)floor(v);
    if (cell_i >= (uint32_t)patch->cells_x) {
        cell_i = (uint32_t)patch->cells_x - 1u; /* the far edge belongs to the last cell */
    }
    if (cell_j >= (uint32_t)patch->cells_z) {
        cell_j = (uint32_t)patch->cells_z - 1u;
    }
    fu = u - (double)cell_i;
    fv = v - (double)cell_j;

    columns = (uint32_t)patch->cells_x + 1u;
    a = cell_j * columns + cell_i;
    b = a + 1u;
    d = (cell_j + 1u) * columns + cell_i;
    c = d + 1u;

    /*
     * The two triangles the builder emitted are (a, c, b) and (a, d, c); their
     * shared edge is the a->c diagonal, i.e. fv == fu. Interpolate on the
     * plane of whichever triangle holds the point, so this is the drawn
     * surface and not a bilinear stand-in for it.
     */
    if (fv <= fu) {
        height = (double)mesh->vertices[a].y +
                 fu * ((double)mesh->vertices[b].y - (double)mesh->vertices[a].y) +
                 fv * ((double)mesh->vertices[c].y - (double)mesh->vertices[b].y);
    } else {
        height = (double)mesh->vertices[a].y +
                 fv * ((double)mesh->vertices[d].y - (double)mesh->vertices[a].y) +
                 fu * ((double)mesh->vertices[c].y - (double)mesh->vertices[d].y);
    }
    if (isfinite(height) == 0 || isfinite((double)(float)height) == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_height = (float)height;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_water_draw(PIXELICA_Context *ctx, const PIXELICA_WaterMesh *mesh,
                                    const PIXELICA_WaterPatch *patch,
                                    const PIXELICA_Lighting *lighting,
                                    const PIXELICA_DrawOptions *options)
{
    PIXELICA_Mesh drawn;
    PIXELICA_Lighting lit;
    float model[16];

    if (ctx == NULL || !water_patch_ok(patch) || !water_mesh_matches(mesh, patch)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    pixelica_mat4_identity(model); /* the vertices are already world space */
    drawn.vertices = mesh->vertices;
    drawn.vertex_count = mesh->vertex_count;
    drawn.indices = mesh->indices;
    drawn.index_count = mesh->index_count;
    drawn.color = patch->color;
    if (lighting == NULL) {
        return pixelica_draw_mesh_lit(ctx, &drawn, model, NULL, options, NULL, NULL);
    }
    /* The caller owns every lighting choice except the normals, which must be
     * the mesh's own or the surface would be shaded as something else. */
    lit = *lighting;
    lit.normals = mesh->normals;
    return pixelica_draw_mesh_lit(ctx, &drawn, model, NULL, options, NULL, &lit);
}
