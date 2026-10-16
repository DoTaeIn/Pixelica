/* External consumer of the installed water3d ADAPTER component: it links the
 * installed 3D core too, creates a context and draws one water patch. */
#include <math.h>
#include <string.h>

#include "pixelica/water3d.h"

#define CELLS 8u
#define VERTS ((CELLS + 1u) * (CELLS + 1u))
#define INDICES (CELLS * CELLS * 6u)

static PIXELICA_Vec3 g_vertices[VERTS];
static PIXELICA_Vec3 g_normals[VERTS];
static uint16_t g_indices[INDICES];

int main(void)
{
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterPatch patch;
    PIXELICA_WaterMesh mesh;
    PIXELICA_WaterSample sample;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Config cfg;
    float projection[16], view[16], view_projection[16];
    float drawn = 0.0f;
    const double time_seconds = 1.25;

    if (pixelica_water_surface_default(&surface) != PIXELICA_OK) {
        return 1;
    }
    memset(&patch, 0, sizeof patch);
    patch.size_x = 8.0f;
    patch.size_z = 8.0f;
    patch.cells_x = (uint16_t)CELLS;
    patch.cells_z = (uint16_t)CELLS;
    patch.color = PIXELICA_RGB565(40, 110, 170);
    if (pixelica_water_mesh_vertex_count(&patch) != VERTS ||
        pixelica_water_mesh_index_count(&patch) != INDICES) {
        return 2;
    }

    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = g_vertices;
    mesh.normals = g_normals;
    mesh.indices = g_indices;
    mesh.vertex_capacity = VERTS;
    mesh.index_capacity = INDICES;
    if (pixelica_water_mesh_build(&surface, &patch, time_seconds, &mesh) != PIXELICA_OK) {
        return 3;
    }

    /* At a grid vertex the drawn surface and the analytic surface agree. */
    if (pixelica_water_mesh_height(&mesh, &patch, g_vertices[0].x, g_vertices[0].z, &drawn) !=
        PIXELICA_OK) {
        return 4;
    }
    if (pixelica_water_sample(&surface, g_vertices[0].x, g_vertices[0].z, time_seconds,
                              &sample) != PIXELICA_OK) {
        return 5;
    }
    if (fabs((double)drawn - (double)sample.height) > 1.0e-6) {
        return 6;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 64u;
    cfg.height = 48u;
    cfg.max_draw_commands = 4u;
    cfg.max_vertices_per_mesh = (uint16_t)VERTS;
    cfg.frame_arena_bytes = VERTS * PIXELICA_ARENA_BYTES_PER_VERTEX;
    if (pixelica_create(&cfg, NULL, &ctx) != PIXELICA_OK || ctx == NULL) {
        return 7;
    }
    if (pixelica_mat4_perspective(projection, 0.9f, 64.0f / 48.0f, 0.5f, 60.0f) !=
        PIXELICA_OK) {
        pixelica_destroy(ctx);
        return 8;
    }
    /* Look down at the water from above and in front of it. */
    pixelica_mat4_translation(view, 0.0f, -2.0f, -9.0f);
    pixelica_mat4_multiply(view_projection, projection, view);
    pixelica_begin_frame(ctx, PIXELICA_RGB565(0, 0, 0));
    pixelica_set_view_projection(ctx, view_projection);
    if (pixelica_water_draw(ctx, &mesh, &patch, NULL, NULL) != PIXELICA_OK) {
        pixelica_destroy(ctx);
        return 9;
    }
    if (pixelica_frame_stats(ctx).pixels_written == 0u) {
        pixelica_destroy(ctx); /* the surface must be visible from above */
        return 10;
    }
    if (pixelica_end_frame(ctx) != PIXELICA_OK) {
        pixelica_destroy(ctx);
        return 11;
    }
    pixelica_destroy(ctx);
    return 0;
}
