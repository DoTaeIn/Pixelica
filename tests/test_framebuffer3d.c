/* Offscreen 3D render -> frame texture -> main render, plus snapshots.
 * Needs the 3D core (PIXELICA_TEST_WITH_3D). Uses only platform-less contexts. */
#include "pixelica/framebuffer.h"
#include "test_util.h"

#define RED 0xF800u
#define BLUE 0x001Fu
#define GREEN 0x07E0u

static PIXELICA_Vec3 v3(float x, float y)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = 0.0f;
    return v;
}

static PIXELICA_Context *make_ctx(uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg = pixelica_t_config(w, h);
    PIXELICA_Context *ctx = NULL;

    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    CHECK(ctx != NULL);
    return ctx;
}

/* Draws the NDC rectangle [x0,x1] x [-1,1] in `color` (no material). */
static void render_band(PIXELICA_Context *ctx, float x0, float x1, uint16_t color,
                        uint16_t clear)
{
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Vec3 verts[4];
    PIXELICA_Mesh mesh;

    verts[0] = v3(x0, -1.0f);
    verts[1] = v3(x1, -1.0f);
    verts[2] = v3(x1, 1.0f);
    verts[3] = v3(x0, 1.0f);
    mesh.vertices = verts;
    mesh.vertex_count = 4;
    mesh.indices = idx;
    mesh.index_count = 6;
    mesh.color = color;

    pixelica_set_cull_mode(ctx, PIXELICA_CULL_NONE);
    pixelica_begin_frame(ctx, clear);
    CHECK(pixelica_draw_mesh(ctx, &mesh, (const float[16]){1, 0, 0, 0, 0, 1, 0, 0,
                                                           0, 0, 1, 0, 0, 0, 0, 1}) ==
          PIXELICA_OK);
    CHECK(pixelica_end_frame(ctx) == PIXELICA_OK); /* no PAL: no present */
}

static void test_adapters(void)
{
    PIXELICA_Context *ctx = make_ctx(16, 8);
    PIXELICA_FrameView view;
    PIXELICA_Texture tex;
    PIXELICA_FrameView bad;

    memset(&view, 0xAB, sizeof view);
    CHECK(pixelica_frame_view(ctx, &view) == PIXELICA_OK);
    CHECK(view.color == pixelica_color_buffer(ctx));
    CHECK(view.depth == pixelica_depth_buffer(ctx));
    CHECK(view.width == 16 && view.height == 8 && view.stride == 16);

    CHECK(pixelica_frame_texture(ctx, &tex) == PIXELICA_OK);
    CHECK(tex.pixels == pixelica_color_buffer(ctx));
    CHECK(tex.width == 16 && tex.height == 8);

    /* Rejections leave outputs alone. */
    bad = view;
    CHECK(pixelica_frame_view(NULL, &bad) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_view(ctx, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_texture(NULL, &tex) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_texture(ctx, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_view(ctx, (PIXELICA_FrameView *)(void *)(UINTPTR_MAX - 1u)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_frame_texture(ctx, (PIXELICA_Texture *)(void *)(UINTPTR_MAX - 1u)) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&bad, &view, sizeof bad) == 0);
    /* Output inside context-owned memory. */
    {
        uint16_t *color = (uint16_t *)(void *)(uintptr_t)pixelica_color_buffer(ctx);
        uint16_t *depth = (uint16_t *)(void *)(uintptr_t)pixelica_depth_buffer(ctx);
        uint16_t c0 = color[0], d0 = depth[0];

        CHECK(pixelica_frame_view(ctx, (PIXELICA_FrameView *)(void *)color) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_frame_texture(ctx, (PIXELICA_Texture *)(void *)depth) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(pixelica_frame_view(ctx, (PIXELICA_FrameView *)(void *)ctx) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(color[0] == c0 && depth[0] == d0);
    }
    pixelica_destroy(ctx);
}

static void test_offscreen_to_texture(void)
{
    /* Offscreen 16x8: left half red, right half blue. Main 32x32 samples it. */
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    PIXELICA_Context *off = make_ctx(16, 8);
    PIXELICA_Context *main_ctx = make_ctx(32, 32);
    PIXELICA_Texture tex;
    PIXELICA_Vec3 verts[4];
    PIXELICA_Vec2 uvs[4];
    PIXELICA_Mesh mesh;
    PIXELICA_Material mat;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    PIXELICA_FrameView view;
    uint16_t snap_color[16 * 8], snap_depth[16 * 8];
    PIXELICA_FrameView snap;
    uint16_t px = 0;
    int i;

    render_band(off, -1.0f, 0.0f, RED, BLUE);
    CHECK(pixelica_frame_texture(off, &tex) == PIXELICA_OK);

    verts[0] = v3(-1.0f, -1.0f);
    verts[1] = v3(1.0f, -1.0f);
    verts[2] = v3(1.0f, 1.0f);
    verts[3] = v3(-1.0f, 1.0f);
    uvs[0].x = 0.0f; uvs[0].y = 1.0f;
    uvs[1].x = 1.0f; uvs[1].y = 1.0f;
    uvs[2].x = 1.0f; uvs[2].y = 0.0f;
    uvs[3].x = 0.0f; uvs[3].y = 0.0f;
    mesh.vertices = verts;
    mesh.vertex_count = 4;
    mesh.indices = idx;
    mesh.index_count = 6;
    mesh.color = GREEN;
    mat.uvs = uvs;
    mat.triangle_colors = NULL;
    mat.texture = &tex;

    pixelica_set_cull_mode(main_ctx, PIXELICA_CULL_NONE);
    pixelica_begin_frame(main_ctx, 0);
    CHECK(pixelica_draw_mesh_ex(main_ctx, &mesh, identity, &mat) == PIXELICA_OK);
    CHECK(pixelica_end_frame(main_ctx) == PIXELICA_OK);
    CHECK(pixelica_t_color_at(main_ctx, 32, 4, 16) == RED);
    CHECK(pixelica_t_color_at(main_ctx, 32, 28, 16) == BLUE);

    /* Snapshot, then a new offscreen frame must not alter the snapshot. */
    CHECK(pixelica_frame_view(off, &view) == PIXELICA_OK);
    CHECK(pixelica_frame_copy(&view, snap_color, 16 * 8, snap_depth, 16 * 8, &snap) ==
          PIXELICA_OK);
    CHECK(snap.stride == 16 && snap.color == snap_color);

    render_band(off, -1.0f, 1.0f, GREEN, GREEN);
    CHECK(pixelica_t_color_at(off, 16, 2, 2) == GREEN);
    CHECK(pixelica_t_color_at(off, 16, 14, 2) == GREEN);
    CHECK(snap_color[2 * 16 + 2] == RED);
    CHECK(snap_color[2 * 16 + 14] == BLUE);

    /* Raw depth: covered texels < clear, uncovered == PIXELICA_DEPTH_CLEAR. */
    CHECK(pixelica_frame_sample_depth(&snap, 0.25f, 0.5f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &px) == PIXELICA_OK);
    CHECK(px < PIXELICA_DEPTH_CLEAR);
    CHECK(pixelica_frame_sample_depth(&snap, 0.75f, 0.5f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, &px) == PIXELICA_OK);
    CHECK(px == PIXELICA_DEPTH_CLEAR);
    CHECK(pixelica_frame_sample_color(&snap, 0.25f, 0.5f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, PIXELICA_FILTER_NEAREST,
                                      &px) == PIXELICA_OK);
    CHECK(px == RED);

    /* Live view tracks the context; the old texture borrows live memory. */
    CHECK(pixelica_frame_sample_color(&view, 0.25f, 0.5f, PIXELICA_WRAP_CLAMP,
                                      PIXELICA_WRAP_CLAMP, PIXELICA_FILTER_NEAREST,
                                      &px) == PIXELICA_OK);
    CHECK(px == GREEN);
    CHECK(tex.pixels[2 * 16 + 2] == GREEN);
    for (i = 0; i < 16 * 8; ++i) {
        if (snap_color[i] != RED && snap_color[i] != BLUE) {
            CHECK(0);
            break;
        }
    }

    pixelica_destroy(main_ctx);
    pixelica_destroy(off);
}

int main(void)
{
    test_adapters();
    test_offscreen_to_texture();
    return pixelica_t_finish("test_framebuffer3d");
}
