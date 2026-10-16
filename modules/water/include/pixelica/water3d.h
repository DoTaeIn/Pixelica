/*
 * Pixelica - 3D renderer adapter for the water module (Pixelica::water3d).
 *
 * This is the ONLY part of the water module that knows a renderer exists. The
 * core (Pixelica::water, pixelica/water.h) stays renderer-independent and is
 * built and shipped on its own; this adapter is a separate library, a separate
 * header, a separate export set and a separate install component, and it is
 * only built when the 3D core is.
 *
 * What it does: turn the analytic surface into a triangle grid in caller-owned
 * buffers and hand it to the ordinary pixelica_draw_mesh_lit() path. Every
 * vertex height and normal comes from pixelica_water_sample() - the SAME call
 * the buoyancy uses, at the same double-precision time - so the drawn water
 * and the floating body are the same surface by construction, not by
 * coincidence.
 *
 * Memory: no heap, no globals. The vertex, normal and index arrays belong to
 * the caller, their capacities are declared, and the builder refuses to write
 * past them (PIXELICA_ERR_CAPACITY). Nothing is retained after a call.
 *
 * The grid is a heightfield, so it has exactly one height per (x, z) and the
 * mesh can never self-intersect or overhang.
 */
#ifndef PIXELICA_WATER3D_H
#define PIXELICA_WATER3D_H

#include <stdint.h>

#include "pixelica/pixelica.h"
#include "pixelica/water.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64 x 64 cells = 4225 vertices, which still fits the renderer's uint16
 * indices with room to spare. */
#define PIXELICA_WATER_MAX_CELLS 64u
#define PIXELICA_WATER_MAX_EXTENT 1.0e5f

/*
 * The piece of water to tessellate: an axis-aligned rectangle in world XZ,
 * centred on (center_x, center_z), `size_x` by `size_z` metres, divided into
 * cells_x by cells_z quads. Vertex (i, j), 0 <= i <= cells_x, is at
 *
 *   x = center_x - size_x / 2 + size_x * i / cells_x
 *   z = center_z - size_z / 2 + size_z * j / cells_z
 *
 * and its Y is the sampled surface height, so the mesh is already in WORLD
 * space: draw it with the identity model matrix (which is what
 * pixelica_water_draw does).
 */
typedef struct PIXELICA_WaterPatch {
    float center_x, center_z;  /* world XZ centre, m */
    float size_x, size_z;      /* full extent, m, > 0 .. PIXELICA_WATER_MAX_EXTENT */
    uint16_t cells_x, cells_z; /* 1 .. PIXELICA_WATER_MAX_CELLS */
    uint16_t color;            /* RGB565 base colour of the surface */
} PIXELICA_WaterPatch;

/*
 * Caller-owned geometry. Set the three pointers and the two capacities; the
 * builder fills the arrays and writes `vertex_count` / `index_count`.
 * `vertices` are world positions, `normals` the matching analytic unit
 * normals, and the three arrays must not overlap each other.
 */
typedef struct PIXELICA_WaterMesh {
    PIXELICA_Vec3 *vertices;  /* >= pixelica_water_mesh_vertex_count(patch) */
    PIXELICA_Vec3 *normals;   /* same count as `vertices` */
    uint16_t *indices;        /* >= pixelica_water_mesh_index_count(patch) */
    uint32_t vertex_capacity; /* in PIXELICA_Vec3 elements */
    uint32_t index_capacity;  /* in uint16_t elements */
    uint32_t vertex_count;    /* written by pixelica_water_mesh_build */
    uint32_t index_count;     /* written by pixelica_water_mesh_build */
} PIXELICA_WaterMesh;

/*
 * Exactly how many elements `patch` needs: (cells_x + 1) * (cells_z + 1)
 * vertices (and normals) and cells_x * cells_z * 6 indices. Both return 0 for
 * a NULL or out-of-range patch, so a caller can size a buffer before building
 * and treat 0 as "that patch is not acceptable".
 */
uint32_t pixelica_water_mesh_vertex_count(const PIXELICA_WaterPatch *patch);
uint32_t pixelica_water_mesh_index_count(const PIXELICA_WaterPatch *patch);

/*
 * Samples the surface on the patch grid at `time_seconds` (double, like the
 * rest of the module) and writes the triangles.
 *
 * Winding: two triangles per cell, emitted counter-clockwise as seen from
 * OUTSIDE the surface, i.e. from above (+Y) - the same right-hand-outward
 * convention the engine's own meshes use, so the renderer's default
 * PIXELICA_CULL_BACK keeps the water visible from above and culls it from
 * below. A scene that must see the surface from underneath should set
 * PIXELICA_CULL_NONE.
 *
 * Everything is validated first and `mesh->vertex_count` / `index_count` are
 * only written on success, so a rejected call leaves the mesh descriptor and
 * the previous contents of the arrays exactly as they were:
 *   PIXELICA_ERR_INVALID_ARG  NULL argument; an invalid surface (see
 *     pixelica_water_surface_validate); a patch with a non-finite or
 *     out-of-range centre/size, or cells outside 1..PIXELICA_WATER_MAX_CELLS;
 *     a non-finite time; a NULL array pointer; two arrays that overlap; or a
 *     sample that is not finite (including a grid corner outside
 *     PIXELICA_WATER_MAX_COORD).
 *   PIXELICA_ERR_CAPACITY     vertex_capacity or index_capacity too small for
 *     the patch (compare with the two count functions above).
 */
PIXELICA_Status pixelica_water_mesh_build(const PIXELICA_WaterSurface *surface,
                                          const PIXELICA_WaterPatch *patch,
                                          double time_seconds, PIXELICA_WaterMesh *mesh);

/*
 * The height of the BUILT (tessellated) surface at world (x, z).
 *
 * This is the DRAWN surface, not an approximation of it: the cell containing
 * (x, z) is found, the one of its two triangles that contains the point is
 * selected, and the point is interpolated on that triangle's plane. The result
 * therefore equals what the rasterizer draws at that pixel up to float
 * rounding - exactly at grid vertices, exactly along cell edges and exactly
 * along the cell diagonal too.
 *
 * It differs from pixelica_water_sample()'s analytic height by the GRID's own
 * discretization error, which shrinks with cells_x / cells_z. That difference
 * is the real render/physics gap, and this function is how to measure it
 * instead of guessing: examples/hello_water reports it per raft corner and
 * fails if it exceeds the bound the grid can be held to.
 *
 * Use it to park an object on the water the player can actually see.
 *
 * `patch` must be the patch the mesh was built with. PIXELICA_ERR_INVALID_ARG
 * (out untouched) for NULL, a mesh that has not been built (vertex_count 0) or
 * whose counts do not match the patch, a non-finite x / z, an (x, z) outside
 * the patch rectangle, or a non-finite result.
 */
PIXELICA_Status pixelica_water_mesh_height(const PIXELICA_WaterMesh *mesh,
                                           const PIXELICA_WaterPatch *patch, float x, float z,
                                           float *out_height);

/*
 * Draws a built mesh as ONE pixelica_draw_mesh_lit() call with the identity
 * model matrix (the vertices are already world space, so the normal matrix is
 * the identity and the analytic normals are used unchanged).
 *
 * `lighting` may be NULL, which draws the flat unlit colour; when it is given,
 * its `normals` field is IGNORED and replaced by the mesh's own normals (that
 * is the one thing this adapter exists to get right), and every other field -
 * lights, ambient, two_sided - is the caller's. `options` may be NULL for the
 * renderer's defaults; pass one to make the water translucent or additive.
 *
 * Returns whatever the renderer returns, and PIXELICA_ERR_INVALID_ARG for a
 * NULL ctx/mesh/patch, a mesh that has not been built, or a mesh whose counts
 * do not match the patch. The renderer's own limits still apply: the context
 * needs max_vertices_per_mesh >= mesh->vertex_count, or the draw comes back
 * PIXELICA_ERR_CAPACITY with the frame untouched.
 */
PIXELICA_Status pixelica_water_draw(PIXELICA_Context *ctx, const PIXELICA_WaterMesh *mesh,
                                    const PIXELICA_WaterPatch *patch,
                                    const PIXELICA_Lighting *lighting,
                                    const PIXELICA_DrawOptions *options);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_WATER3D_H */
