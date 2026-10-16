/*
 * Optional water 3D adapter (pixelica/water3d.h, modules/water/water3d.c).
 *
 * Links Pixelica::water3d, so it needs the 3D core; the core water tests live
 * in tests/test_water.c and need no renderer at all.
 *
 * What matters here is that the DRAWN surface is the SAMPLED surface: every
 * grid vertex must carry exactly what pixelica_water_sample() returned, the
 * triangles must be wound so the renderer's default culling keeps the water
 * visible from above, and pixelica_water_mesh_height() must reproduce the
 * triangle plane the rasterizer uses rather than an approximation of it.
 */
#include "pixelica/water3d.h"
#include "test_util.h"

#define CELLS_X 6u
#define CELLS_Z 4u
#define VERTS ((CELLS_X + 1u) * (CELLS_Z + 1u))
#define INDICES (CELLS_X * CELLS_Z * 6u)

static PIXELICA_Vec3 g_vertices[VERTS];
static PIXELICA_Vec3 g_normals[VERTS];
static uint16_t g_indices[INDICES];

static PIXELICA_WaterPatch patch_of(uint16_t cells_x, uint16_t cells_z)
{
    PIXELICA_WaterPatch patch;

    memset(&patch, 0, sizeof patch);
    patch.center_x = 1.0f;
    patch.center_z = -2.0f;
    patch.size_x = 12.0f;
    patch.size_z = 8.0f;
    patch.cells_x = cells_x;
    patch.cells_z = cells_z;
    patch.color = PIXELICA_RGB565(40, 110, 170);
    return patch;
}

static void mesh_of(PIXELICA_WaterMesh *mesh, uint32_t vertex_capacity,
                    uint32_t index_capacity)
{
    memset(mesh, 0, sizeof *mesh);
    mesh->vertices = g_vertices;
    mesh->normals = g_normals;
    mesh->indices = g_indices;
    mesh->vertex_capacity = vertex_capacity;
    mesh->index_capacity = index_capacity;
}

static void test_counts_and_validation(void)
{
    PIXELICA_WaterPatch patch = patch_of((uint16_t)CELLS_X, (uint16_t)CELLS_Z);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh, keep;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    CHECK(pixelica_water_mesh_vertex_count(&patch) == VERTS);
    CHECK(pixelica_water_mesh_index_count(&patch) == INDICES);
    CHECK(pixelica_water_mesh_vertex_count(NULL) == 0u);
    CHECK(pixelica_water_mesh_index_count(NULL) == 0u);

    /* An unacceptable patch reports 0 instead of a size nobody can use. */
    {
        PIXELICA_WaterPatch bad = patch;

        bad.cells_x = 0u;
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
        bad = patch;
        bad.cells_z = (uint16_t)(PIXELICA_WATER_MAX_CELLS + 1u);
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
        bad = patch;
        bad.size_x = 0.0f;
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
        bad = patch;
        bad.size_z = pixelica_t_nan();
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
        bad = patch;
        bad.center_x = pixelica_t_inf();
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
        bad = patch;
        bad.size_x = 2.0f * PIXELICA_WATER_MAX_EXTENT;
        CHECK(pixelica_water_mesh_vertex_count(&bad) == 0u);
    }

    /* Too small a buffer is PIXELICA_ERR_CAPACITY, and the descriptor keeps
     * its previous counts so a failed build cannot be mistaken for a good one. */
    mesh_of(&mesh, VERTS - 1u, INDICES);
    keep = mesh;
    CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &mesh) == PIXELICA_ERR_CAPACITY);
    CHECK(memcmp(&mesh, &keep, sizeof mesh) == 0);
    mesh_of(&mesh, VERTS, INDICES - 1u);
    keep = mesh;
    CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &mesh) == PIXELICA_ERR_CAPACITY);
    CHECK(memcmp(&mesh, &keep, sizeof mesh) == 0);

    /* Bad arguments, all with the descriptor left alone. */
    mesh_of(&mesh, VERTS, INDICES);
    keep = mesh;
    CHECK(pixelica_water_mesh_build(NULL, &patch, 0.0, &mesh) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_build(&surface, NULL, 0.0, &mesh) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_build(&surface, &patch, (double)pixelica_t_nan(), &mesh) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&mesh, &keep, sizeof mesh) == 0);
    {
        PIXELICA_WaterMesh broken = mesh;

        broken.vertices = NULL;
        CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &broken) ==
              PIXELICA_ERR_INVALID_ARG);
        broken = mesh;
        broken.normals = NULL;
        CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &broken) ==
              PIXELICA_ERR_INVALID_ARG);
        broken = mesh;
        broken.indices = NULL;
        CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &broken) ==
              PIXELICA_ERR_INVALID_ARG);
        /* Overlapping caller arrays are refused before anything is written. */
        broken = mesh;
        broken.normals = g_vertices;
        CHECK(pixelica_water_mesh_build(&surface, &patch, 0.0, &broken) ==
              PIXELICA_ERR_INVALID_ARG);
    }
    /* An invalid surface is refused too. */
    {
        PIXELICA_WaterSurface bad = surface;

        bad.waves[0].wavelength = 0.0f;
        CHECK(pixelica_water_mesh_build(&bad, &patch, 0.0, &mesh) == PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&mesh, &keep, sizeof mesh) == 0);
    }
}

/* Every vertex is the grid point, and carries the sample's own height and
 * normal: that identity is what makes the drawn water the physics water. */
static void test_vertices_are_samples(void)
{
    PIXELICA_WaterPatch patch = patch_of((uint16_t)CELLS_X, (uint16_t)CELLS_Z);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh;
    const double t = 3.125;
    uint32_t i, j;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    mesh_of(&mesh, VERTS, INDICES);
    CHECK(pixelica_water_mesh_build(&surface, &patch, t, &mesh) == PIXELICA_OK);
    CHECK(mesh.vertex_count == VERTS);
    CHECK(mesh.index_count == INDICES);

    for (j = 0; j <= CELLS_Z; ++j) {
        for (i = 0; i <= CELLS_X; ++i) {
            const uint32_t index = j * (CELLS_X + 1u) + i;
            const double expect_x = (double)patch.center_x - 0.5 * (double)patch.size_x +
                                    (double)patch.size_x * (double)i / (double)CELLS_X;
            const double expect_z = (double)patch.center_z - 0.5 * (double)patch.size_z +
                                    (double)patch.size_z * (double)j / (double)CELLS_Z;
            PIXELICA_WaterSample sample;

            CHECK_NEAR(mesh.vertices[index].x, expect_x, 1.0e-5);
            CHECK_NEAR(mesh.vertices[index].z, expect_z, 1.0e-5);
            CHECK(pixelica_water_sample(&surface, mesh.vertices[index].x,
                                        mesh.vertices[index].z, t, &sample) == PIXELICA_OK);
            /* Bit-exact: the builder stores what the sampler returned. */
            CHECK(mesh.vertices[index].y == sample.height);
            CHECK(mesh.normals[index].x == sample.normal.x);
            CHECK(mesh.normals[index].y == sample.normal.y);
            CHECK(mesh.normals[index].z == sample.normal.z);
            CHECK(mesh.normals[index].y > 0.0f); /* a heightfield never overhangs */
        }
    }

    /* Every index is in range and every triangle faces UP, which is what the
     * renderer's default back-face culling needs to keep the water visible. */
    for (i = 0; i < INDICES; i += 3u) {
        const PIXELICA_Vec3 a = mesh.vertices[mesh.indices[i]];
        const PIXELICA_Vec3 b = mesh.vertices[mesh.indices[i + 1u]];
        const PIXELICA_Vec3 c = mesh.vertices[mesh.indices[i + 2u]];
        double ux = (double)b.x - (double)a.x, uz = (double)b.z - (double)a.z;
        double vx = (double)c.x - (double)a.x, vz = (double)c.z - (double)a.z;

        CHECK(mesh.indices[i] < VERTS);
        CHECK(mesh.indices[i + 1u] < VERTS);
        CHECK(mesh.indices[i + 2u] < VERTS);
        /* The Y component of (b - a) x (c - a) is uz*vx - ux*vz. */
        CHECK(uz * vx - ux * vz > 0.0);
    }
}

/* pixelica_water_mesh_height() is the drawn triangulation, not a bilinear
 * stand-in for it. */
static void test_mesh_height_is_the_drawn_surface(void)
{
    PIXELICA_WaterPatch patch = patch_of((uint16_t)CELLS_X, (uint16_t)CELLS_Z);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh;
    const double t = 0.75;
    const double cell_x = 12.0 / (double)CELLS_X, cell_z = 8.0 / (double)CELLS_Z;
    float height = 0.0f, keep;
    uint32_t i, j;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    mesh_of(&mesh, VERTS, INDICES);
    CHECK(pixelica_water_mesh_build(&surface, &patch, t, &mesh) == PIXELICA_OK);

    /* Exact at every grid vertex. */
    for (j = 0; j <= CELLS_Z; ++j) {
        for (i = 0; i <= CELLS_X; ++i) {
            const uint32_t index = j * (CELLS_X + 1u) + i;

            CHECK(pixelica_water_mesh_height(&mesh, &patch, mesh.vertices[index].x,
                                             mesh.vertices[index].z, &height) == PIXELICA_OK);
            CHECK_NEAR(height, mesh.vertices[index].y, 1.0e-6);
        }
    }

    /* On the a->c diagonal of a cell it is the straight line between those two
     * vertices, which is where the two triangles meet. */
    {
        const uint32_t a = 2u * (CELLS_X + 1u) + 2u;
        const uint32_t c = 3u * (CELLS_X + 1u) + 3u;
        const double mid_x = 0.5 * ((double)mesh.vertices[a].x + (double)mesh.vertices[c].x);
        const double mid_z = 0.5 * ((double)mesh.vertices[a].z + (double)mesh.vertices[c].z);
        const double expect = 0.5 * ((double)mesh.vertices[a].y + (double)mesh.vertices[c].y);

        CHECK(pixelica_water_mesh_height(&mesh, &patch, (float)mid_x, (float)mid_z,
                                         &height) == PIXELICA_OK);
        CHECK_NEAR(height, expect, 1.0e-5);
    }

    /* Inside a cell it stays between that cell's lowest and highest corner,
     * and within the grid's discretization error of the analytic surface. */
    for (j = 0; j < CELLS_Z; ++j) {
        for (i = 0; i < CELLS_X; ++i) {
            const uint32_t base = j * (CELLS_X + 1u) + i;
            const double x = (double)mesh.vertices[base].x + 0.37 * cell_x;
            const double z = (double)mesh.vertices[base].z + 0.63 * cell_z;
            float corners[4];
            float analytic = 0.0f;
            double lowest, highest;
            uint32_t k;

            corners[0] = mesh.vertices[base].y;
            corners[1] = mesh.vertices[base + 1u].y;
            corners[2] = mesh.vertices[base + CELLS_X + 1u].y;
            corners[3] = mesh.vertices[base + CELLS_X + 2u].y;
            lowest = corners[0];
            highest = corners[0];
            for (k = 1u; k < 4u; ++k) {
                if ((double)corners[k] < lowest) {
                    lowest = corners[k];
                }
                if ((double)corners[k] > highest) {
                    highest = corners[k];
                }
            }
            CHECK(pixelica_water_mesh_height(&mesh, &patch, (float)x, (float)z, &height) ==
                  PIXELICA_OK);
            CHECK((double)height >= lowest - 1.0e-5);
            CHECK((double)height <= highest + 1.0e-5);
            CHECK(pixelica_water_height(&surface, (float)x, (float)z, t, &analytic) ==
                  PIXELICA_OK);
            /* The default sea has sum(A k^2) = 0.172 1/m and the cells here
             * are 2.0 x 2.0 m, so linear interpolation errs by at most
             * 0.172 * (2*sqrt(2))^2 / 8 = 0.172 m. */
            CHECK(fabs((double)height - (double)analytic) < 0.20);
        }
    }

    /* Outside the patch, and against a mesh that was never built or does not
     * match, it refuses and keeps `out`. */
    keep = height;
    CHECK(pixelica_water_mesh_height(&mesh, &patch, patch.center_x + patch.size_x, 0.0f,
                                     &height) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_height(&mesh, &patch, 0.0f, patch.center_z - patch.size_z,
                                     &height) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_height(&mesh, &patch, pixelica_t_nan(), 0.0f, &height) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_height(NULL, &patch, 0.0f, 0.0f, &height) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_height(&mesh, NULL, 0.0f, 0.0f, &height) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_mesh_height(&mesh, &patch, 0.0f, 0.0f, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(height == keep);
    {
        PIXELICA_WaterMesh unbuilt;
        PIXELICA_WaterPatch other = patch_of((uint16_t)(CELLS_X + 1u), (uint16_t)CELLS_Z);

        mesh_of(&unbuilt, VERTS, INDICES);
        CHECK(pixelica_water_mesh_height(&unbuilt, &patch, 0.0f, 0.0f, &height) ==
              PIXELICA_ERR_INVALID_ARG);
        /* Built for one patch, queried with another: refused, not guessed. */
        CHECK(pixelica_water_mesh_height(&mesh, &other, 0.0f, 0.0f, &height) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(height == keep);
    }
    /* The far edges belong to the last cell rather than falling off the grid. */
    CHECK(pixelica_water_mesh_height(&mesh, &patch,
                                     patch.center_x + 0.5f * patch.size_x,
                                     patch.center_z + 0.5f * patch.size_z,
                                     &height) == PIXELICA_OK);
    CHECK_NEAR(height, mesh.vertices[VERTS - 1u].y, 1.0e-5);
}

/* The time really is a double all the way through the adapter. */
static void test_build_uses_double_time(void)
{
    PIXELICA_WaterPatch patch = patch_of((uint16_t)CELLS_X, (uint16_t)CELLS_Z);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh;
    float first[VERTS];
    uint32_t i;
    int changed = 0;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    mesh_of(&mesh, VERTS, INDICES);
    CHECK(pixelica_water_mesh_build(&surface, &patch, 1.0e6, &mesh) == PIXELICA_OK);
    for (i = 0; i < VERTS; ++i) {
        first[i] = mesh.vertices[i].y;
    }
    /* One 60 Hz step later, a million seconds in: a float clock could not
     * even represent this time, let alone advance by it. */
    CHECK(pixelica_water_mesh_build(&surface, &patch, 1.0e6 + 1.0 / 60.0, &mesh) ==
          PIXELICA_OK);
    for (i = 0; i < VERTS; ++i) {
        if (mesh.vertices[i].y != first[i]) {
            changed = 1;
        }
    }
    CHECK(changed != 0);
}

/* The surface draws, is visible from above under the default culling, and the
 * adapter substitutes the mesh's own normals into the caller's lighting. */
static void test_draw(void)
{
    PIXELICA_WaterPatch patch = patch_of((uint16_t)CELLS_X, (uint16_t)CELLS_Z);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Config cfg = pixelica_t_config(96u, 72u);
    PIXELICA_DirectionalLight light;
    PIXELICA_Lighting lighting;
    float projection[16], view[16], view_projection[16];
    uint32_t unlit_pixels, lit_pixels;

    cfg.max_vertices_per_mesh = (uint16_t)VERTS;
    cfg.frame_arena_bytes = VERTS * PIXELICA_ARENA_BYTES_PER_VERTEX;
    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    mesh_of(&mesh, VERTS, INDICES);
    CHECK(pixelica_water_mesh_build(&surface, &patch, 2.0, &mesh) == PIXELICA_OK);
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    CHECK(pixelica_mat4_perspective(projection, 0.9f, 96.0f / 72.0f, 0.5f, 80.0f) ==
          PIXELICA_OK);
    /* Above the water, looking down at it. */
    pixelica_mat4_translation(view, -1.0f, -4.0f, -14.0f);
    pixelica_mat4_multiply(view_projection, projection, view);

    pixelica_begin_frame(ctx, PIXELICA_RGB565(0, 0, 0));
    pixelica_set_view_projection(ctx, view_projection);
    CHECK(pixelica_water_draw(ctx, &mesh, &patch, NULL, NULL) == PIXELICA_OK);
    unlit_pixels = pixelica_frame_stats(ctx).pixels_written;
    CHECK(unlit_pixels > 0u); /* the default cull mode keeps it visible */
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* With lighting the surface is shaded, so the image changes but the same
     * geometry is covered. */
    memset(&light, 0, sizeof light);
    light.direction.x = 0.3f;
    light.direction.y = -0.9f;
    light.direction.z = 0.3f;
    light.r = 255u;
    light.g = 240u;
    light.b = 210u;
    memset(&lighting, 0, sizeof lighting);
    lighting.lights = &light;
    lighting.light_count = 1u;
    lighting.ambient_r = 40u;
    lighting.ambient_g = 50u;
    lighting.ambient_b = 70u;
    /* Deliberately wrong normals: the adapter must replace them with the
     * mesh's own, so this must not be rejected and must not shade by them. */
    lighting.normals = NULL;
    pixelica_begin_frame(ctx, PIXELICA_RGB565(0, 0, 0));
    pixelica_set_view_projection(ctx, view_projection);
    CHECK(pixelica_water_draw(ctx, &mesh, &patch, &lighting, NULL) == PIXELICA_OK);
    lit_pixels = pixelica_frame_stats(ctx).pixels_written;
    CHECK(lit_pixels == unlit_pixels);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* Rejected draws, none of which may touch the frame. */
    pixelica_begin_frame(ctx, PIXELICA_RGB565(0, 0, 0));
    pixelica_set_view_projection(ctx, view_projection);
    CHECK(pixelica_water_draw(NULL, &mesh, &patch, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_draw(ctx, NULL, &patch, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_draw(ctx, &mesh, NULL, NULL, NULL) == PIXELICA_ERR_INVALID_ARG);
    {
        PIXELICA_WaterMesh unbuilt;

        mesh_of(&unbuilt, VERTS, INDICES);
        CHECK(pixelica_water_draw(ctx, &unbuilt, &patch, NULL, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
    }
    CHECK(pixelica_frame_stats(ctx).pixels_written == 0u);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK);

    /* A context too small for the mesh reports capacity, not corruption. */
    {
        PIXELICA_Context *small = NULL;
        PIXELICA_Config tight = pixelica_t_config(64u, 48u);

        tight.max_vertices_per_mesh = (uint16_t)(VERTS - 1u);
        tight.frame_arena_bytes = VERTS * PIXELICA_ARENA_BYTES_PER_VERTEX;
        CHECK(pixelica_create(&tight, NULL, &small) == PIXELICA_OK);
        if (small != NULL) {
            pixelica_begin_frame(small, PIXELICA_RGB565(0, 0, 0));
            pixelica_set_view_projection(small, view_projection);
            CHECK(pixelica_water_draw(small, &mesh, &patch, NULL, NULL) ==
                  PIXELICA_ERR_CAPACITY);
            CHECK(pixelica_frame_stats(small).pixels_written == 0u);
            CHECK(pixelica_end_frame(small) == PIXELICA_OK);
            pixelica_destroy(small);
        }
    }
    pixelica_destroy(ctx);
}

/* The largest patch the adapter allows still fits uint16 indices. */
static void test_maximum_patch(void)
{
    static PIXELICA_Vec3 vertices[(PIXELICA_WATER_MAX_CELLS + 1u) *
                                  (PIXELICA_WATER_MAX_CELLS + 1u)];
    static PIXELICA_Vec3 normals[(PIXELICA_WATER_MAX_CELLS + 1u) *
                                 (PIXELICA_WATER_MAX_CELLS + 1u)];
    static uint16_t indices[PIXELICA_WATER_MAX_CELLS * PIXELICA_WATER_MAX_CELLS * 6u];
    PIXELICA_WaterPatch patch = patch_of((uint16_t)PIXELICA_WATER_MAX_CELLS,
                                         (uint16_t)PIXELICA_WATER_MAX_CELLS);
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterMesh mesh;
    uint32_t i;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    memset(&mesh, 0, sizeof mesh);
    mesh.vertices = vertices;
    mesh.normals = normals;
    mesh.indices = indices;
    mesh.vertex_capacity = (uint32_t)(sizeof vertices / sizeof vertices[0]);
    mesh.index_capacity = (uint32_t)(sizeof indices / sizeof indices[0]);
    CHECK(pixelica_water_mesh_build(&surface, &patch, 0.5, &mesh) == PIXELICA_OK);
    CHECK(mesh.vertex_count == mesh.vertex_capacity);
    CHECK(mesh.vertex_count <= 65535u); /* uint16 indices stay addressable */
    for (i = 0; i < mesh.index_count; ++i) {
        CHECK(mesh.indices[i] < mesh.vertex_count);
    }
}

int main(void)
{
    test_counts_and_validation();
    test_vertices_are_samples();
    test_mesh_height_is_the_drawn_surface();
    test_build_uses_double_time();
    test_draw();
    test_maximum_patch();
    return pixelica_t_finish("test_water3d");
}
