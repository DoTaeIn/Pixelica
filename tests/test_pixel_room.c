/*
 * Pixel Room tests: simulation, collision, investigation edges, fixed-step
 * determinism across 30/60/120 render schedules, independent instances,
 * load-failure cleanup, allocation accounting and rendered pixels.
 *
 * The room is built from assets this test encodes itself (exact P3MS/P3TX v1
 * byte layout from the asset contract), so it does not need the generated
 * assets header. When PIXEL_ROOM_TEST_EMBEDDED is defined (examples/CMake adds
 * it once assets/compiled/pixel_room_assets.h exists) the same checks also run
 * on the real embedded assets.
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "test_util.h"

#include "pixel_room.h"
#ifdef PIXEL_ROOM_TEST_EMBEDDED
#include "room_embedded.h"
#endif

#define PI_D 3.14159265358979323846
#define DT (1.0f / (float)PIXELICA_UPDATE_HZ)

/* ------------------------------------------------------------------------ */
/* Asset encoding (P3MS / P3TX v1)                                           */
/* ------------------------------------------------------------------------ */

#define BLOB_CAP 1024u
#define BIG_BLOB_CAP 70000u
#define SLOT_COUNT 6u /* five props + checker texture */
#define SLOT_CHECKER 5u

#define TEX_A PIXELICA_RGB565(0, 252, 0)
#define TEX_B PIXELICA_RGB565(252, 0, 252)

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_f32(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    put_u32(p, bits);
}

typedef struct Chunk {
    const char *type;
    const uint8_t *data;
    size_t length;
} Chunk;

/* Common header + chunk table + tightly packed payloads. Returns the file size or 0. */
static size_t build_container(uint8_t *out, size_t capacity, const char *magic,
                              const Chunk *chunks, uint32_t count)
{
    size_t header = 16u + 16u * (size_t)count;
    size_t total = header, offset, i;

    for (i = 0; i < count; ++i) {
        total += chunks[i].length;
    }
    if (total > capacity) {
        return 0u;
    }
    memcpy(out, magic, 4);
    put_u16(out + 4, 1u);
    put_u16(out + 6, (uint32_t)header);
    put_u32(out + 8, (uint32_t)total);
    put_u32(out + 12, count);
    offset = header;
    for (i = 0; i < count; ++i) {
        uint8_t *entry = out + 16u + 16u * i;
        memcpy(entry, chunks[i].type, 4);
        put_u32(entry + 4, (uint32_t)offset);
        put_u32(entry + 8, (uint32_t)chunks[i].length);
        put_u32(entry + 12, crc32_ieee(chunks[i].data, chunks[i].length));
        memcpy(out + offset, chunks[i].data, chunks[i].length);
        offset += chunks[i].length;
    }
    return total;
}

/* Triangle list of a box; vertex bit 0 = x, bit 1 = y, bit 2 = z (high = max). */
static const uint16_t k_box_indices[36] = {4, 5, 7, 4, 7, 6, 1, 0, 2, 1, 2, 3, 5, 1, 3, 5, 3, 7,
                                           0, 4, 6, 0, 6, 2, 6, 7, 3, 6, 3, 2, 0, 1, 5, 0, 5, 4};

/*
 * A box occupying [0,sx] x [0,sy] x [0,sz] (deliberately not centred, to
 * exercise the room's measured-bounds placement) plus `extra_vertices` unused
 * vertices to build over-capacity meshes.
 */
static size_t make_mesh_blob(uint8_t *out, size_t capacity, float sx, float sy, float sz,
                             uint32_t extra_vertices)
{
    static uint8_t vtx[(8u + 5000u) * 12u];
    uint8_t idx[36 * 2];
    uint8_t col[12 * 2];
    uint32_t i;
    uint32_t vertex_count = 8u + extra_vertices;
    Chunk chunks[3];
    static const uint16_t face_colors[6] = {PIXELICA_RGB565(220, 80, 80), PIXELICA_RGB565(80, 220, 80),
                                            PIXELICA_RGB565(80, 80, 220), PIXELICA_RGB565(220, 220, 80),
                                            PIXELICA_RGB565(220, 80, 220), PIXELICA_RGB565(80, 220, 220)};

    if (vertex_count > 8u + 5000u) {
        return 0u;
    }
    for (i = 0; i < 8u; ++i) {
        put_f32(vtx + i * 12u, (i & 1u) != 0u ? sx : 0.0f);
        put_f32(vtx + i * 12u + 4u, (i & 2u) != 0u ? sy : 0.0f);
        put_f32(vtx + i * 12u + 8u, (i & 4u) != 0u ? sz : 0.0f);
    }
    for (i = 8u; i < vertex_count; ++i) {
        put_f32(vtx + i * 12u, (float)i * 0.001f);
        put_f32(vtx + i * 12u + 4u, 0.0f);
        put_f32(vtx + i * 12u + 8u, 0.0f);
    }
    for (i = 0; i < 36u; ++i) {
        put_u16(idx + i * 2u, k_box_indices[i]);
    }
    for (i = 0; i < 12u; ++i) {
        put_u16(col + i * 2u, face_colors[i / 2u]);
    }
    chunks[0].type = "VTX0";
    chunks[0].data = vtx;
    chunks[0].length = (size_t)vertex_count * 12u;
    chunks[1].type = "IDX0";
    chunks[1].data = idx;
    chunks[1].length = sizeof(idx);
    chunks[2].type = "COL0";
    chunks[2].data = col;
    chunks[2].length = sizeof(col);
    return build_container(out, capacity, "P3MS", chunks, 3u);
}

static size_t make_texture_blob(uint8_t *out, size_t capacity)
{
    uint8_t info[8];
    uint8_t data[8];
    Chunk chunks[2];

    put_u16(info, 2u);
    put_u16(info + 2, 2u);
    put_u32(info + 4, 0u);
    put_u16(data, TEX_A);
    put_u16(data + 2, TEX_B);
    put_u16(data + 4, TEX_B);
    put_u16(data + 6, TEX_A);
    chunks[0].type = "INFO";
    chunks[0].data = info;
    chunks[0].length = sizeof(info);
    chunks[1].type = "DATA";
    chunks[1].data = data;
    chunks[1].length = sizeof(data);
    return build_container(out, capacity, "P3TX", chunks, 2u);
}

typedef struct TestAssets {
    uint8_t blob[SLOT_COUNT][BLOB_CAP];
    size_t size[SLOT_COUNT];
    PixelRoomAssets view;
} TestAssets;

static void bind_view(TestAssets *a)
{
    uint32_t i;
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        a->view.props[i].data = a->blob[i];
        a->view.props[i].size = a->size[i];
    }
    a->view.checker.data = a->blob[SLOT_CHECKER];
    a->view.checker.size = a->size[SLOT_CHECKER];
}

static void build_test_assets(TestAssets *a)
{
    memset(a, 0, sizeof(*a));
    a->size[PIXEL_ROOM_PROP_DESK] = make_mesh_blob(a->blob[PIXEL_ROOM_PROP_DESK], BLOB_CAP, 4.0f, 2.2f, 2.0f, 0u);
    a->size[PIXEL_ROOM_PROP_CHAIR] = make_mesh_blob(a->blob[PIXEL_ROOM_PROP_CHAIR], BLOB_CAP, 1.0f, 2.0f, 1.0f, 0u);
    a->size[PIXEL_ROOM_PROP_BOOK] = make_mesh_blob(a->blob[PIXEL_ROOM_PROP_BOOK], BLOB_CAP, 2.0f, 0.4f, 1.5f, 0u);
    a->size[PIXEL_ROOM_PROP_CUP] = make_mesh_blob(a->blob[PIXEL_ROOM_PROP_CUP], BLOB_CAP, 0.5f, 0.6f, 0.5f, 0u);
    a->size[PIXEL_ROOM_PROP_PLANT] = make_mesh_blob(a->blob[PIXEL_ROOM_PROP_PLANT], BLOB_CAP, 1.0f, 3.0f, 1.0f, 0u);
    a->size[SLOT_CHECKER] = make_texture_blob(a->blob[SLOT_CHECKER], BLOB_CAP);
    bind_view(a);
}

/* ------------------------------------------------------------------------ */
/* Counting allocator                                                        */
/* ------------------------------------------------------------------------ */

typedef struct CountingAllocator {
    PixelRoomAllocator hooks;
    size_t calls;   /* alloc calls, including failed ones */
    size_t live;    /* currently outstanding allocations */
    size_t frees;
    size_t fail_at; /* 1-based call number that returns NULL, 0 = never */
} CountingAllocator;

static void *counting_alloc(void *user, size_t bytes)
{
    CountingAllocator *c = (CountingAllocator *)user;
    void *p;
    ++c->calls;
    if (c->fail_at != 0u && c->calls == c->fail_at) {
        return NULL;
    }
    p = malloc(bytes);
    if (p != NULL) {
        ++c->live;
    }
    return p;
}

static void counting_release(void *user, void *ptr)
{
    CountingAllocator *c = (CountingAllocator *)user;
    if (ptr != NULL) {
        --c->live;
        ++c->frees;
    }
    free(ptr);
}

static void counting_init(CountingAllocator *c)
{
    memset(c, 0, sizeof(*c));
    c->hooks.user = c;
    c->hooks.alloc = counting_alloc;
    c->hooks.release = counting_release;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void tick(PixelRoom *room, uint32_t buttons)
{
    pixel_room_update(room, DT, buttons);
}

static void ticks(PixelRoom *room, uint32_t buttons, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        tick(room, buttons);
    }
}

static PixelRoom *create_room(const PixelRoomAssets *assets, uint16_t w, uint16_t h,
                              CountingAllocator *counting, PIXELICA_Status *out_status)
{
    PixelRoomParams params;
    PixelRoom *room = NULL;
    PIXELICA_Status status;

    memset(&params, 0, sizeof(params));
    params.width = w;
    params.height = h;
    params.assets = *assets;
    params.allocator = counting != NULL ? &counting->hooks : NULL;
    status = pixel_room_create(&room, &params);
    if (out_status != NULL) {
        *out_status = status;
    }
    return room;
}

static PixelRoom *make_room(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    PIXELICA_Status status = PIXELICA_ERR_INVALID_ARG;
    PixelRoom *room = create_room(assets, w, h, NULL, &status);
    CHECK_EQ_LONG(status, PIXELICA_OK);
    CHECK(room != NULL);
    return room;
}

static PIXELICA_Context *make_context(uint16_t w, uint16_t h)
{
    PIXELICA_Config cfg = pixel_room_context_config(w, h);
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Status status = pixelica_create(&cfg, NULL, &ctx);
    CHECK_EQ_LONG(status, PIXELICA_OK);
    CHECK(ctx != NULL);
    return ctx;
}

static PixelRoomSnapshot snap(const PixelRoom *room)
{
    PixelRoomSnapshot s;
    pixel_room_snapshot(room, &s);
    return s;
}

static bool invariants_hold(const PixelRoom *room)
{
    PIXELICA_AABB box = pixel_room_player_bounds(room);
    const float tol = 1e-4f;
    size_t i, n = pixel_room_obstacle_count(room);
    PixelRoomSnapshot s = snap(room);

    if (!isfinite(box.min.x) || !isfinite(box.max.x) || !isfinite(box.min.z) ||
        !isfinite(box.max.z) || !isfinite(s.facing_x) || !isfinite(s.facing_z)) {
        return false;
    }
    if (box.min.x < -PIXEL_ROOM_HALF_EXTENT - tol || box.max.x > PIXEL_ROOM_HALF_EXTENT + tol ||
        box.min.z < -PIXEL_ROOM_HALF_EXTENT - tol || box.max.z > PIXEL_ROOM_HALF_EXTENT + tol) {
        return false;
    }
    for (i = 0; i < n; ++i) {
        PIXELICA_AABB obstacle;
        if (!pixel_room_obstacle(room, i, &obstacle) || pixelica_aabb_overlaps(&box, &obstacle)) {
            return false;
        }
    }
    if (!(s.camera_yaw >= 0.0f && s.camera_yaw < (float)(2.0 * PI_D))) {
        return false;
    }
    if (s.focus < -1 || s.focus >= (int32_t)PIXEL_ROOM_PROP_COUNT) {
        return false;
    }
    return true;
}

static double circular_difference(double a, double b)
{
    double d = fmod(a - b, 2.0 * PI_D);
    if (d > PI_D) {
        d -= 2.0 * PI_D;
    }
    if (d < -PI_D) {
        d += 2.0 * PI_D;
    }
    return d;
}

static float distance_to_prop(const PixelRoom *room, uint32_t prop)
{
    PixelRoomSnapshot s = snap(room);
    PIXELICA_AABB b;
    float qx, qz;
    if (!pixel_room_prop_bounds(room, prop, &b)) {
        return 1.0e9f;
    }
    qx = s.player_x < b.min.x ? b.min.x : (s.player_x > b.max.x ? b.max.x : s.player_x);
    qz = s.player_z < b.min.z ? b.min.z : (s.player_z > b.max.z ? b.max.z : s.player_z);
    return sqrtf((qx - s.player_x) * (qx - s.player_x) + (qz - s.player_z) * (qz - s.player_z));
}

/* Teleports to every grid cell and faces each direction until `prop` is focused. */
static bool scan_for_focus(PixelRoom *room, uint32_t prop)
{
    static const uint32_t dirs[4] = {PIXELICA_BTN_UP, PIXELICA_BTN_DOWN, PIXELICA_BTN_LEFT, PIXELICA_BTN_RIGHT};
    int xi, zi, d;

    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 0.0f), PIXELICA_OK);
    for (zi = 0; zi <= 28; ++zi) {
        for (xi = 0; xi <= 28; ++xi) {
            float x = -3.6f + 0.25f * (float)xi;
            float z = -3.6f + 0.25f * (float)zi;
            for (d = 0; d < 4; ++d) {
                if (pixel_room_teleport_player(room, x, z) != PIXELICA_OK) {
                    break;
                }
                tick(room, dirs[d]);
                if (snap(room).focus == (int32_t)prop) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Lifecycle, failure cleanup, accounting                                    */
/* ------------------------------------------------------------------------ */

static void test_lifecycle(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    CountingAllocator counting;
    PIXELICA_Status status = PIXELICA_ERR_INVALID_ARG;
    PixelRoom *room;
    PixelRoomSnapshot s;
    size_t calls_after_create;
    uint32_t i;
    PIXELICA_Context *ctx;

    counting_init(&counting);
    room = create_room(assets, w, h, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_OK);
    CHECK(room != NULL);
    if (room == NULL) {
        return;
    }
    /* Exactly one allocation holds the whole room (state, geometry, decoded assets). */
    CHECK_EQ_LONG(counting.calls, 1);
    CHECK_EQ_LONG(counting.live, 1);
    CHECK(pixel_room_allocation_bytes(room) > 0u);

    s = snap(room);
    CHECK_EQ_LONG(s.tick, 0);
    CHECK_EQ_LONG(s.focus, -1);
    CHECK_EQ_LONG(s.investigated_mask, 0);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_NONE);
    CHECK(invariants_hold(room));
    CHECK(pixel_room_scene(room) != NULL);
    CHECK(pixel_room_scene(room)->count >= 12u);
    CHECK(pixel_room_obstacle_count(room) >= 4u + 3u); /* four walls + desk, chair, plant */
    CHECK(pixel_room_prop_is_solid(room, PIXEL_ROOM_PROP_DESK));
    CHECK(pixel_room_prop_is_solid(room, PIXEL_ROOM_PROP_CHAIR));
    CHECK(pixel_room_prop_is_solid(room, PIXEL_ROOM_PROP_PLANT));
    CHECK(!pixel_room_prop_is_solid(room, PIXEL_ROOM_PROP_BOOK));
    CHECK(!pixel_room_prop_is_solid(room, PIXEL_ROOM_PROP_CUP));

    /* Props are inside the room; the book and cup rest on the desk top. */
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        PIXELICA_AABB b;
        CHECK(pixel_room_prop_bounds(room, i, &b));
        CHECK(b.min.x >= -PIXEL_ROOM_HALF_EXTENT && b.max.x <= PIXEL_ROOM_HALF_EXTENT);
        CHECK(b.min.z >= -PIXEL_ROOM_HALF_EXTENT && b.max.z <= PIXEL_ROOM_HALF_EXTENT);
        CHECK(b.max.y > b.min.y);
        CHECK(pixel_room_prop_name(i)[0] != '\0');
    }
    {
        PIXELICA_AABB desk, book, cup, chair;
        CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_DESK, &desk));
        CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_BOOK, &book));
        CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_CUP, &cup));
        CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_CHAIR, &chair));
        CHECK_NEAR(book.min.y, desk.max.y, 1e-4);
        CHECK_NEAR(cup.min.y, desk.max.y, 1e-4);
        CHECK_NEAR(chair.min.y, 0.0, 1e-4);
        CHECK_NEAR(desk.min.y, 0.0, 1e-4);
    }

    /* Neither updates nor renders allocate. */
    ctx = make_context(w, h);
    calls_after_create = counting.calls;
    for (i = 0; i < 300u; ++i) {
        tick(room, (i % 3u) == 0u ? PIXELICA_BTN_UP : PIXELICA_BTN_RIGHT);
        if ((i % 10u) == 0u && ctx != NULL) {
            CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        }
    }
    CHECK_EQ_LONG(counting.calls, calls_after_create);
    CHECK_EQ_LONG(counting.live, 1);
    if (ctx != NULL) {
        CHECK(pixelica_memory_guards_intact(ctx));
        pixelica_destroy(ctx);
    }

    pixel_room_destroy(room);
    CHECK_EQ_LONG(counting.live, 0);
    CHECK_EQ_LONG(counting.frees, 1);
    pixel_room_destroy(NULL); /* no-op */

    /* Reinit after destroy gives an identical fresh state. */
    {
        PixelRoom *a = make_room(assets, w, h);
        PixelRoom *b = make_room(assets, w, h);
        CHECK_EQ_LONG(pixel_room_state_hash(a) == pixel_room_state_hash(b), 1);
        pixel_room_destroy(a);
        pixel_room_destroy(b);
    }
}

static void test_create_argument_errors(const PixelRoomAssets *assets)
{
    CountingAllocator counting;
    PixelRoomParams params;
    PixelRoom *room = (PixelRoom *)(uintptr_t)1u;
    PIXELICA_Status status;
    PixelRoomAllocator bad;

    memset(&params, 0, sizeof(params));
    CHECK_EQ_LONG(pixel_room_create(NULL, &params), PIXELICA_ERR_INVALID_ARG);
    CHECK(pixel_room_create(&room, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);

    counting_init(&counting);
    status = PIXELICA_OK;
    room = create_room(assets, 0u, 120u, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);
    room = create_room(assets, 160u, 15u, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);
    room = create_room(assets, 5000u, 120u, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);

    memset(&params, 0, sizeof(params));
    params.width = 160u;
    params.height = 120u;
    params.assets = *assets;
    memset(&bad, 0, sizeof(bad));
    params.allocator = &bad; /* missing callbacks */
    room = NULL;
    CHECK_EQ_LONG(pixel_room_create(&room, &params), PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);

    /* An empty blob is rejected before anything is allocated. */
    params.allocator = &counting.hooks;
    params.assets.props[PIXEL_ROOM_PROP_BOOK].data = NULL;
    CHECK_EQ_LONG(pixel_room_create(&room, &params), PIXELICA_ERR_INVALID_ARG);
    params.assets = *assets;
    params.assets.checker.size = 0u;
    CHECK_EQ_LONG(pixel_room_create(&room, &params), PIXELICA_ERR_INVALID_ARG);
    CHECK(room == NULL);
    CHECK_EQ_LONG(counting.live, 0);

    /* NULL room queries are safe. */
    {
        PixelRoomSnapshot s;
        PIXELICA_AABB b;
        pixel_room_snapshot(NULL, &s);
        CHECK_EQ_LONG(s.focus, -1);
        CHECK_EQ_LONG(s.tick, 0);
        CHECK(!pixel_room_prop_bounds(NULL, 0u, &b));
        CHECK(!pixel_room_obstacle(NULL, 0u, &b));
        CHECK(pixel_room_scene(NULL) == NULL);
        CHECK_EQ_LONG(pixel_room_obstacle_count(NULL), 0);
        pixel_room_update(NULL, DT, 0u);
        CHECK_EQ_LONG(pixel_room_teleport_player(NULL, 0.0f, 0.0f), PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(NULL, 0.0f), PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(pixel_room_render(NULL, NULL), PIXELICA_ERR_INVALID_ARG);
    }
}

static void test_failed_load_cleanup(const TestAssets *good)
{
    CountingAllocator counting;
    uint32_t slot, mutation;
    PIXELICA_Status status;
    PixelRoom *room;

    counting_init(&counting);

    for (slot = 0; slot < SLOT_COUNT; ++slot) {
        for (mutation = 0; mutation < 4u; ++mutation) {
            TestAssets bad = *good;
            status = PIXELICA_OK;
            if (mutation == 0u) {
                bad.blob[slot][bad.size[slot] - 1u] ^= 0x5Au; /* payload byte: CRC mismatch */
            } else if (mutation == 1u) {
                bad.size[slot] -= 1u; /* truncated file */
            } else if (mutation == 2u) {
                bad.blob[slot][0] = 'X'; /* bad magic */
            } else {
                bad.blob[slot][4] = 2u; /* unsupported version */
            }
            bind_view(&bad);
            room = create_room(&bad.view, 160u, 120u, &counting, &status);
            CHECK(status != PIXELICA_OK);
            CHECK(room == NULL);
            CHECK_EQ_LONG(counting.live, 0);
        }
    }

    /*
     * A valid mesh with more vertices than the renderer limit passes the loader,
     * so this fails AFTER the allocation: it must free the block and report it.
     */
    {
        static uint8_t huge[BIG_BLOB_CAP];
        size_t huge_size = make_mesh_blob(huge, sizeof(huge), 1.0f, 3.0f, 1.0f,
                                          PIXEL_ROOM_MAX_MESH_VERTICES);
        PixelRoomAssets view = good->view;
        size_t calls_before = counting.calls;
        CHECK(huge_size > 0u);
        view.props[PIXEL_ROOM_PROP_PLANT].data = huge;
        view.props[PIXEL_ROOM_PROP_PLANT].size = huge_size;
        status = PIXELICA_OK;
        room = create_room(&view, 160u, 120u, &counting, &status);
        CHECK_EQ_LONG(status, PIXELICA_ERR_CAPACITY);
        CHECK(room == NULL);
        CHECK_EQ_LONG(counting.calls, calls_before + 1u); /* it did allocate ... */
        CHECK_EQ_LONG(counting.live, 0);                  /* ... and freed it */
    }

    /* Allocation failure: nothing leaks, and a retry succeeds. */
    counting.fail_at = counting.calls + 1u;
    status = PIXELICA_OK;
    room = create_room(&good->view, 160u, 120u, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_ERR_NO_MEMORY);
    CHECK(room == NULL);
    CHECK_EQ_LONG(counting.live, 0);

    counting.fail_at = 0u;
    room = create_room(&good->view, 160u, 120u, &counting, &status);
    CHECK_EQ_LONG(status, PIXELICA_OK);
    CHECK(room != NULL);
    CHECK_EQ_LONG(counting.live, 1);
    if (room != NULL) {
        CHECK(invariants_hold(room));
        tick(room, PIXELICA_BTN_UP);
    }
    pixel_room_destroy(room);
    CHECK_EQ_LONG(counting.live, 0);
}

/* ------------------------------------------------------------------------ */
/* Movement, collision, camera                                               */
/* ------------------------------------------------------------------------ */

static void test_movement_and_collision(const PixelRoomAssets *assets)
{
    PixelRoom *room = make_room(assets, 160u, 120u);
    PixelRoomSnapshot s, before;
    PIXELICA_AABB desk, box;
    uint64_t hash_before;

    if (room == NULL) {
        return;
    }
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 0.0f), PIXELICA_OK);

    /* Speed and axis: UP is away from the camera (-Z at yaw 0). */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_UP, 30u);
    s = snap(room);
    CHECK_NEAR(s.player_x, 0.0, 1e-4);
    CHECK_NEAR(s.player_z, 2.0 - (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 1e-3);
    CHECK_NEAR(s.facing_z, -1.0, 1e-5);

    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_DOWN, 30u);
    CHECK_NEAR(snap(room).player_z, 2.0 + (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 1e-3);

    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_RIGHT, 30u);
    CHECK_NEAR(snap(room).player_x, (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 1e-3);
    ticks(room, PIXELICA_BTN_LEFT, 30u);
    CHECK_NEAR(snap(room).player_x, 0.0, 1e-3);

    /* Diagonals are normalised: same speed as a cardinal move. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.5f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_UP | PIXELICA_BTN_RIGHT, 30u);
    s = snap(room);
    CHECK_NEAR(sqrt((double)s.player_x * (double)s.player_x +
                    ((double)s.player_z - 2.5) * ((double)s.player_z - 2.5)),
               (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 2e-3);
    CHECK_NEAR(s.player_x, -((double)s.player_z - 2.5), 1e-3);

    /* Opposite buttons cancel, START freezes movement. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    hash_before = pixel_room_state_hash(room);
    before = snap(room);
    ticks(room, PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT, 20u);
    ticks(room, PIXELICA_BTN_UP | PIXELICA_BTN_DOWN, 20u);
    s = snap(room);
    CHECK_EQ_LONG(s.player_x == before.player_x && s.player_z == before.player_z, 1);
    CHECK_EQ_LONG(pixel_room_state_hash(room) != hash_before, 1); /* tick/held changed */
    ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_UP, 20u);
    s = snap(room);
    CHECK_EQ_LONG(s.player_x == before.player_x && s.player_z == before.player_z, 1);
    CHECK_EQ_LONG(s.camera_yaw == 0.0f, 1);

    /* Walls: the cube stops flush against each side and never enters them. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.5f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_RIGHT, 240u);
    box = pixel_room_player_bounds(room);
    CHECK_NEAR(box.max.x, PIXEL_ROOM_HALF_EXTENT, 1e-4);
    ticks(room, PIXELICA_BTN_DOWN, 240u);
    box = pixel_room_player_bounds(room);
    CHECK_NEAR(box.max.z, PIXEL_ROOM_HALF_EXTENT, 1e-4);
    CHECK_NEAR(box.max.x, PIXEL_ROOM_HALF_EXTENT, 1e-4);
    CHECK_NEAR(box.max.x - box.min.x, 2.0 * PIXEL_ROOM_PLAYER_HALF, 1e-4);
    ticks(room, PIXELICA_BTN_LEFT, 480u);
    box = pixel_room_player_bounds(room);
    CHECK_NEAR(box.min.x, -PIXEL_ROOM_HALF_EXTENT, 1e-4);
    CHECK(invariants_hold(room));

    /* Sliding: pushing diagonally into the east wall keeps x and advances z. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 3.6f, 2.5f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_RIGHT, 20u); /* settle against the wall */
    before = snap(room);
    ticks(room, PIXELICA_BTN_UP | PIXELICA_BTN_RIGHT, 30u);
    s = snap(room);
    CHECK_NEAR(s.player_x, before.player_x, 1e-4);
    CHECK_NEAR(before.player_z - s.player_z, (double)PIXEL_ROOM_MOVE_SPEED * 0.5 * 0.70710678, 3e-3);

    /* No tunnelling with huge steps (clamped to PIXEL_ROOM_MAX_DT, swept per axis). */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 3.0f, 2.0f), PIXELICA_OK);
    pixel_room_update(room, 1000.0f, PIXELICA_BTN_RIGHT);
    box = pixel_room_player_bounds(room);
    CHECK_NEAR(box.max.x, PIXEL_ROOM_HALF_EXTENT, 1e-4);
    CHECK(invariants_hold(room));

    /* A solid prop blocks the cube: walk into the desk from the south. */
    CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_DESK, &desk));
    {
        static const float inset[5] = {0.3f, 0.15f, 0.6f, 0.9f, 1.2f};
        bool placed = false;
        uint32_t k;
        for (k = 0; k < 5u && !placed; ++k) {
            placed = pixel_room_teleport_player(room, desk.max.x - inset[k],
                                                desk.max.z + 0.35f + 0.6f) == PIXELICA_OK;
        }
        CHECK(placed);
    }
    ticks(room, PIXELICA_BTN_UP, 120u);
    box = pixel_room_player_bounds(room);
    CHECK_NEAR(box.min.z, desk.max.z, 1e-4);
    CHECK(invariants_hold(room));

    /* Invalid input leaves the state untouched. */
    hash_before = pixel_room_state_hash(room);
    pixel_room_update(room, 0.0f, PIXELICA_BTN_UP);
    pixel_room_update(room, -1.0f, PIXELICA_BTN_UP);
    pixel_room_update(room, pixelica_t_nan(), PIXELICA_BTN_UP);
    pixel_room_update(room, pixelica_t_inf(), PIXELICA_BTN_UP);
    CHECK_EQ_LONG(pixel_room_state_hash(room) == hash_before, 1);

    /* Teleport validation: walls, solid props, outside, non-finite. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 4.0f, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 10.0f, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, -3.9f, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.5f * (desk.min.x + desk.max.x),
                                              0.5f * (desk.min.z + desk.max.z)),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, pixelica_t_nan(), 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, pixelica_t_inf()), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixel_room_state_hash(room) == hash_before, 1);
    /* Near the wall is legal, sticking out of the room is not. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 3.6f, 1.0f), PIXELICA_OK);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 3.66f, 1.0f), PIXELICA_ERR_INVALID_ARG);

    pixel_room_destroy(room);
}

static void test_camera(const PixelRoomAssets *assets)
{
    PixelRoom *room = make_room(assets, 256u, 192u);
    PixelRoomSnapshot s, before;
    double expected;

    if (room == NULL) {
        return;
    }
    s = snap(room);
    CHECK_NEAR(s.camera_yaw, PIXEL_ROOM_START_CAMERA_YAW, 1e-6);

    /* START+LEFT orbits one way, START+RIGHT the other; the cube does not move. */
    before = snap(room);
    ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_LEFT, 30u);
    s = snap(room);
    expected = (double)before.camera_yaw - (double)PIXEL_ROOM_CAMERA_RATE * 0.5;
    CHECK_NEAR(circular_difference((double)s.camera_yaw, expected), 0.0, 1e-4);
    CHECK_EQ_LONG(s.player_x == before.player_x && s.player_z == before.player_z, 1);
    CHECK(invariants_hold(room));

    before = snap(room);
    ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_RIGHT, 60u);
    s = snap(room);
    expected = (double)before.camera_yaw + (double)PIXEL_ROOM_CAMERA_RATE;
    CHECK_NEAR(circular_difference((double)s.camera_yaw, expected), 0.0, 1e-4);

    /* START with both or neither direction holds the camera. */
    before = snap(room);
    ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT, 10u);
    ticks(room, PIXELICA_BTN_START, 10u);
    CHECK_EQ_LONG(snap(room).camera_yaw == before.camera_yaw, 1);

    /* Arrows without START never rotate the camera. */
    before = snap(room);
    ticks(room, PIXELICA_BTN_LEFT, 5u);
    CHECK_EQ_LONG(snap(room).camera_yaw == before.camera_yaw, 1);

    /* Yaw stays wrapped over many revolutions. */
    {
        uint32_t i;
        for (i = 0; i < 20u; ++i) {
            ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_RIGHT, 100u);
            s = snap(room);
            CHECK(s.camera_yaw >= 0.0f && s.camera_yaw < (float)(2.0 * PI_D));
        }
        for (i = 0; i < 20u; ++i) {
            ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_LEFT, 100u);
            s = snap(room);
            CHECK(s.camera_yaw >= 0.0f && s.camera_yaw < (float)(2.0 * PI_D));
        }
    }

    /* Movement is camera-relative. */
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, (float)(PI_D / 2.0)), PIXELICA_OK);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_UP, 30u); /* forward = -X at yaw pi/2 */
    s = snap(room);
    CHECK_NEAR(s.player_x, -(double)PIXEL_ROOM_MOVE_SPEED * 0.5, 2e-3);
    CHECK_NEAR(s.player_z, 2.0, 2e-3);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.0f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_RIGHT, 30u); /* right = -Z at yaw pi/2 */
    s = snap(room);
    CHECK_NEAR(s.player_x, 0.0, 2e-3);
    CHECK_NEAR(s.player_z, 2.0 - (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 2e-3);
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, (float)PI_D), PIXELICA_OK);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 0.5f), PIXELICA_OK);
    ticks(room, PIXELICA_BTN_UP, 30u); /* forward = +Z at yaw pi */
    CHECK_NEAR(snap(room).player_z, 0.5 + (double)PIXEL_ROOM_MOVE_SPEED * 0.5, 2e-3);

    /* set_camera_yaw wraps and rejects non-finite values. */
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, -0.5f), PIXELICA_OK);
    CHECK_NEAR(snap(room).camera_yaw, 2.0 * PI_D - 0.5, 1e-4);
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 100.0f), PIXELICA_OK);
    CHECK(snap(room).camera_yaw >= 0.0f && snap(room).camera_yaw < (float)(2.0 * PI_D));
    {
        uint64_t hash = pixel_room_state_hash(room);
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, pixelica_t_nan()), PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, pixelica_t_inf()), PIXELICA_ERR_INVALID_ARG);
        CHECK_EQ_LONG(pixel_room_state_hash(room) == hash, 1);
    }
    pixel_room_destroy(room);
}

/* ------------------------------------------------------------------------ */
/* Investigation                                                             */
/* ------------------------------------------------------------------------ */

static void test_every_prop_is_reachable(const PixelRoomAssets *assets)
{
    PixelRoom *room = make_room(assets, 160u, 120u);
    uint32_t prop;

    if (room == NULL) {
        return;
    }
    for (prop = 0; prop < PIXEL_ROOM_PROP_COUNT; ++prop) {
        bool found = scan_for_focus(room, prop);
        if (!found) {
            fprintf(stderr, "no reachable spot focuses prop '%s'\n", pixel_room_prop_name(prop));
        }
        CHECK(found);
        if (found && prop == PIXEL_ROOM_PROP_BOOK) {
            /* Priority is really exercised: the desk is in reach as well, yet the book wins. */
            CHECK(distance_to_prop(room, PIXEL_ROOM_PROP_DESK) <= PIXEL_ROOM_REACH);
        }
    }
    pixel_room_destroy(room);
}

static void test_investigation_edges(const PixelRoomAssets *assets)
{
    PixelRoom *room = make_room(assets, 160u, 120u);
    PixelRoomSnapshot s;
    PIXELICA_AABB plant;
    float x, z_edge;
    uint32_t i;

    if (room == NULL) {
        return;
    }
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 0.0f), PIXELICA_OK);
    CHECK(pixel_room_prop_bounds(room, PIXEL_ROOM_PROP_PLANT, &plant));
    x = 0.5f * (plant.min.x + plant.max.x);
    z_edge = plant.max.z;

    /* Reach is inclusive of the limit and exact on either side of it. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, x, z_edge + PIXEL_ROOM_REACH + 0.02f), PIXELICA_OK);
    tick(room, 0u);
    CHECK_EQ_LONG(snap(room).focus, -1);
    CHECK_EQ_LONG(pixel_room_teleport_player(room, x, z_edge + PIXEL_ROOM_REACH - 0.02f), PIXELICA_OK);
    tick(room, 0u);
    /* The initial facing is -Z and the plant is straight ahead, so it is focused. */
    CHECK_EQ_LONG(snap(room).focus, PIXEL_ROOM_PROP_PLANT);

    /* Facing cone: diagonal to the plant corner, two directions in and two out. */
    {
        static const struct {
            uint32_t button;
            int32_t expected;
        } cases[4] = {{PIXELICA_BTN_UP, PIXEL_ROOM_PROP_PLANT},
                      {PIXELICA_BTN_RIGHT, PIXEL_ROOM_PROP_PLANT},
                      {PIXELICA_BTN_LEFT, -1},
                      {PIXELICA_BTN_DOWN, -1}};
        for (i = 0; i < 4u; ++i) {
            CHECK_EQ_LONG(pixel_room_teleport_player(room, plant.min.x - 0.65f, plant.max.z + 0.55f),
                          PIXELICA_OK);
            tick(room, cases[i].button);
            CHECK_EQ_LONG(snap(room).focus, cases[i].expected);
        }
    }

    /* Edge-triggered A. Stand in front of the plant looking at it. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, x, z_edge + 0.8f), PIXELICA_OK);
    tick(room, PIXELICA_BTN_UP);
    CHECK_EQ_LONG(snap(room).focus, PIXEL_ROOM_PROP_PLANT);

    tick(room, PIXELICA_BTN_A);
    s = snap(room);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 1);
    CHECK_EQ_LONG(s.investigated_mask, 1u << PIXEL_ROOM_PROP_PLANT);
    CHECK_EQ_LONG(s.investigate_count, 1);
    CHECK_EQ_LONG(s.miss_count, 0);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_INVESTIGATED);
    CHECK_EQ_LONG(s.event_prop, PIXEL_ROOM_PROP_PLANT);
    CHECK_EQ_LONG(s.event_ticks_left, PIXEL_ROOM_FLASH_TICKS);

    for (i = 0; i < 20u; ++i) { /* holding A does nothing more */
        tick(room, PIXELICA_BTN_A);
    }
    s = snap(room);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 1);
    CHECK_EQ_LONG(s.investigate_count, 1);
    CHECK_EQ_LONG(s.event_ticks_left, PIXEL_ROOM_FLASH_TICKS - 20u);

    tick(room, 0u); /* release ... */
    tick(room, PIXELICA_BTN_A); /* ... and press again */
    s = snap(room);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 2);
    CHECK_EQ_LONG(s.investigate_count, 2);
    CHECK_EQ_LONG(s.investigated_mask, 1u << PIXEL_ROOM_PROP_PLANT);
    CHECK_EQ_LONG(s.event_ticks_left, PIXEL_ROOM_FLASH_TICKS);

    /* The flash expires exactly FLASH_TICKS steps later. */
    ticks(room, 0u, PIXEL_ROOM_FLASH_TICKS - 1u);
    s = snap(room);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_INVESTIGATED);
    CHECK_EQ_LONG(s.event_ticks_left, 1);
    tick(room, 0u);
    s = snap(room);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_NONE);
    CHECK_EQ_LONG(s.event_prop, -1);
    CHECK_EQ_LONG(s.event_ticks_left, 0);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 2); /* history is kept */

    /* START is a modifier: A with START held is ignored, even after START is released. */
    tick(room, PIXELICA_BTN_START | PIXELICA_BTN_A);
    tick(room, PIXELICA_BTN_A); /* START released, A still held: not a new press */
    s = snap(room);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 2);
    CHECK_EQ_LONG(s.investigate_count, 2);
    CHECK_EQ_LONG(s.miss_count, 0);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_NONE);
    tick(room, 0u);

    /* A walks into range and presses in the same step: movement, then focus, then A. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, x, z_edge + PIXEL_ROOM_REACH + 0.02f), PIXELICA_OK);
    tick(room, 0u);
    CHECK_EQ_LONG(snap(room).focus, -1);
    tick(room, PIXELICA_BTN_UP | PIXELICA_BTN_A);
    s = snap(room);
    CHECK_EQ_LONG(s.focus, PIXEL_ROOM_PROP_PLANT);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_PLANT], 3);

    /* A with nothing in reach is a counted miss and never marks anything. */
    CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.5f), PIXELICA_OK);
    tick(room, 0u);
    tick(room, PIXELICA_BTN_A);
    s = snap(room);
    CHECK_EQ_LONG(s.miss_count, 1);
    CHECK_EQ_LONG(s.investigate_count, 3);
    CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_NOTHING);
    CHECK_EQ_LONG(s.event_prop, -1);
    CHECK_EQ_LONG(s.investigated_mask, 1u << PIXEL_ROOM_PROP_PLANT);

    /* Investigating a second prop sets its own bit only. */
    CHECK(scan_for_focus(room, PIXEL_ROOM_PROP_CHAIR));
    tick(room, 0u);
    tick(room, PIXELICA_BTN_A);
    s = snap(room);
    CHECK_EQ_LONG(s.visits[PIXEL_ROOM_PROP_CHAIR], 1);
    CHECK_EQ_LONG(s.investigated_mask,
                  (1u << PIXEL_ROOM_PROP_PLANT) | (1u << PIXEL_ROOM_PROP_CHAIR));
    CHECK(invariants_hold(room));

    pixel_room_destroy(room);
}

/* ------------------------------------------------------------------------ */
/* Fixed step across render schedules                                        */
/* ------------------------------------------------------------------------ */

/* Segment starts are odd tick numbers, so 30 Hz pairs (2m-1, 2m) never straddle a change. */
static const struct {
    uint32_t start_tick;
    uint32_t buttons;
} k_script[] = {
    {1u, PIXELICA_BTN_UP},
    {61u, PIXELICA_BTN_UP | PIXELICA_BTN_RIGHT},
    {121u, 0u},
    {123u, PIXELICA_BTN_A},
    {125u, 0u},
    {151u, PIXELICA_BTN_START | PIXELICA_BTN_RIGHT},
    {211u, 0u},
    {241u, PIXELICA_BTN_DOWN},
    {301u, PIXELICA_BTN_A},
    {303u, 0u},
    {331u, PIXELICA_BTN_LEFT},
    {391u, PIXELICA_BTN_A},
    {393u, 0u},
    {421u, PIXELICA_BTN_START | PIXELICA_BTN_LEFT},
    {451u, 0u},
};
#define SCRIPT_SECONDS 8u
#define SCRIPT_TICKS (SCRIPT_SECONDS * PIXELICA_UPDATE_HZ)

static uint32_t script_mask(uint64_t tick_index)
{
    uint32_t mask = 0u;
    size_t i;
    for (i = 0; i < sizeof(k_script) / sizeof(k_script[0]); ++i) {
        if (tick_index >= k_script[i].start_tick) {
            mask = k_script[i].buttons;
        }
    }
    return mask;
}

typedef struct ScheduleClock {
    uint64_t now_us;
    uint32_t presents;
} ScheduleClock;

static uint64_t schedule_time_us(void *user)
{
    return ((ScheduleClock *)user)->now_us;
}

static uint32_t schedule_read_buttons(void *user)
{
    /* Input as a function of time: the mask of the latest fixed step reached. */
    return script_mask(((ScheduleClock *)user)->now_us * PIXELICA_UPDATE_HZ / 1000000u);
}

static bool schedule_present(void *user, const uint16_t *pixels, uint16_t width, uint16_t height,
                             uint32_t stride)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    ++((ScheduleClock *)user)->presents;
    return true;
}

typedef struct ScheduleResult {
    PixelRoomSnapshot snapshot;
    uint64_t state_hash;
    uint64_t frame_hash;
    uint32_t presents;
} ScheduleResult;

static void run_schedule(const PixelRoomAssets *assets, uint32_t fps, uint16_t w, uint16_t h,
                         ScheduleResult *result)
{
    ScheduleClock clock;
    PIXELICA_PlatformOps ops;
    PIXELICA_Config cfg = pixel_room_context_config(w, h);
    PIXELICA_Context *ctx = NULL;
    PixelRoom *room = make_room(assets, w, h);
    uint64_t frames = (uint64_t)fps * SCRIPT_SECONDS, k, previous = 0u;

    memset(result, 0, sizeof(*result));
    memset(&clock, 0, sizeof(clock));
    memset(&ops, 0, sizeof(ops));
    ops.userdata = &clock;
    ops.time_us = schedule_time_us;
    ops.read_buttons = schedule_read_buttons;
    ops.present_rgb565 = schedule_present;
    CHECK_EQ_LONG(pixelica_create(&cfg, &ops, &ctx), PIXELICA_OK);
    if (room == NULL || ctx == NULL) {
        pixel_room_destroy(room);
        pixelica_destroy(ctx);
        return;
    }
    for (k = 1; k <= frames; ++k) {
        /* ceil(k * 1e6 / fps): cumulative time never lags the ideal frame time. */
        uint64_t now = (k * 1000000u + fps - 1u) / fps;
        clock.now_us = now;
        pixelica_update(ctx, now - previous, pixel_room_update_cb, room);
        previous = now;
        if (pixel_room_render(room, ctx) != PIXELICA_OK) {
            ++g_failures;
            fprintf(stderr, "render failed at frame %llu\n", (unsigned long long)k);
            break;
        }
    }
    pixel_room_snapshot(room, &result->snapshot);
    result->state_hash = pixel_room_state_hash(room);
    result->frame_hash = pixelica_t_hash16(pixelica_color_buffer(ctx), (size_t)w * h);
    result->presents = clock.presents;
    CHECK(invariants_hold(room));
    pixel_room_destroy(room);
    pixelica_destroy(ctx);
}

static void test_render_schedules(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    static const uint32_t rates[3] = {30u, 60u, 120u};
    ScheduleResult results[3];
    ScheduleResult reference;
    PixelRoom *ref_room = make_room(assets, w, h);
    PIXELICA_Context *ref_ctx = make_context(w, h);
    PixelRoomSnapshot initial;
    uint64_t t;
    uint32_t r;

    /* Reference: exactly one update per fixed tick, no timing involved. */
    memset(&reference, 0, sizeof(reference));
    if (ref_room == NULL || ref_ctx == NULL) {
        pixel_room_destroy(ref_room);
        pixelica_destroy(ref_ctx);
        return;
    }
    initial = snap(ref_room);
    for (t = 1; t <= SCRIPT_TICKS; ++t) {
        tick(ref_room, script_mask(t));
    }
    CHECK_EQ_LONG(pixel_room_render(ref_room, ref_ctx), PIXELICA_OK);
    pixel_room_snapshot(ref_room, &reference.snapshot);
    reference.state_hash = pixel_room_state_hash(ref_room);
    reference.frame_hash = pixelica_t_hash16(pixelica_color_buffer(ref_ctx), (size_t)w * h);

    /* The script really does something (otherwise equality proves nothing). */
    CHECK_EQ_LONG(reference.snapshot.tick, SCRIPT_TICKS);
    CHECK(reference.snapshot.player_x != initial.player_x || reference.snapshot.player_z != initial.player_z);
    CHECK(reference.snapshot.camera_yaw != initial.camera_yaw);
    CHECK(reference.snapshot.investigate_count + reference.snapshot.miss_count >= 3u);

    for (r = 0; r < 3u; ++r) {
        run_schedule(assets, rates[r], w, h, &results[r]);
        CHECK_EQ_LONG(results[r].presents, rates[r] * SCRIPT_SECONDS);
        CHECK_EQ_LONG(results[r].snapshot.tick, SCRIPT_TICKS);
        if (results[r].state_hash != reference.state_hash) {
            fprintf(stderr, "state hash differs at %u Hz: %016llx vs %016llx\n", (unsigned)rates[r],
                    (unsigned long long)results[r].state_hash,
                    (unsigned long long)reference.state_hash);
        }
        CHECK_EQ_LONG(results[r].state_hash == reference.state_hash, 1);
        CHECK_EQ_LONG(results[r].frame_hash == reference.frame_hash, 1);
        CHECK_EQ_LONG(results[r].snapshot.investigate_count, reference.snapshot.investigate_count);
        CHECK_EQ_LONG(results[r].snapshot.miss_count, reference.snapshot.miss_count);
        CHECK_EQ_LONG(results[r].snapshot.investigated_mask, reference.snapshot.investigated_mask);
    }

    pixel_room_destroy(ref_room);
    pixelica_destroy(ref_ctx);
}

/* ------------------------------------------------------------------------ */
/* Independent instances                                                     */
/* ------------------------------------------------------------------------ */

static uint32_t pseudo_buttons(uint32_t *rng)
{
    static const uint32_t table[] = {
        0u,
        PIXELICA_BTN_UP,
        PIXELICA_BTN_DOWN,
        PIXELICA_BTN_LEFT,
        PIXELICA_BTN_RIGHT,
        PIXELICA_BTN_UP | PIXELICA_BTN_RIGHT,
        PIXELICA_BTN_DOWN | PIXELICA_BTN_LEFT,
        PIXELICA_BTN_A,
        PIXELICA_BTN_UP | PIXELICA_BTN_A,
        PIXELICA_BTN_START | PIXELICA_BTN_LEFT,
        PIXELICA_BTN_START | PIXELICA_BTN_RIGHT,
        PIXELICA_BTN_START | PIXELICA_BTN_A,
        PIXELICA_BTN_QUIT | PIXELICA_BTN_UP, /* QUIT is not a game input */
    };
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *rng = x;
    return table[x % (uint32_t)(sizeof(table) / sizeof(table[0]))];
}

static void test_independent_instances(const PixelRoomAssets *assets)
{
    PixelRoom *a = make_room(assets, 256u, 192u);
    PixelRoom *b = make_room(assets, 160u, 120u);
    PixelRoom *solo_a = make_room(assets, 256u, 192u);
    PIXELICA_Context *ctx_a = make_context(256u, 192u);
    PIXELICA_Context *ctx_b = make_context(160u, 120u);
    PIXELICA_Context *ctx_solo = make_context(256u, 192u);
    uint32_t rng_a = 0x1234567u, rng_b = 0x7654321u, rng_solo = 0x1234567u;
    uint32_t i, held_a = 0u, held_b = 0u, held_solo = 0u;
    uint64_t hash_b_before;

    if (a == NULL || b == NULL || solo_a == NULL || ctx_a == NULL || ctx_b == NULL || ctx_solo == NULL) {
        goto cleanup;
    }
    CHECK(pixel_room_scene(a) != pixel_room_scene(b));
    CHECK(pixel_room_scene(a)->nodes != pixel_room_scene(b)->nodes);

    /* Interleaved updates and renders; A must match a run that never saw B. */
    for (i = 0; i < 900u; ++i) {
        if ((i % 5u) == 0u) {
            held_a = pseudo_buttons(&rng_a);
            held_solo = pseudo_buttons(&rng_solo);
        }
        if ((i % 7u) == 0u) {
            held_b = pseudo_buttons(&rng_b);
        }
        tick(a, held_a);
        tick(b, held_b);
        tick(solo_a, held_solo);
        if ((i % 30u) == 0u) {
            CHECK_EQ_LONG(pixel_room_render(a, ctx_a), PIXELICA_OK);
            CHECK_EQ_LONG(pixel_room_render(b, ctx_b), PIXELICA_OK);
            CHECK_EQ_LONG(pixel_room_render(solo_a, ctx_solo), PIXELICA_OK);
        }
        if (pixel_room_state_hash(a) != pixel_room_state_hash(solo_a)) {
            ++g_failures;
            fprintf(stderr, "instance A diverged from its solo run at step %u\n", (unsigned)i);
            break;
        }
        if (!invariants_hold(a) || !invariants_hold(b)) {
            ++g_failures;
            fprintf(stderr, "invariant violated at step %u\n", (unsigned)i);
            break;
        }
    }
    CHECK_EQ_LONG(pixel_room_render(a, ctx_a), PIXELICA_OK);
    CHECK_EQ_LONG(pixel_room_render(solo_a, ctx_solo), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_hash16(pixelica_color_buffer(ctx_a), 256u * 192u) ==
                      pixelica_t_hash16(pixelica_color_buffer(ctx_solo), 256u * 192u),
                  1);
    CHECK_EQ_LONG(pixel_room_state_hash(a) != pixel_room_state_hash(b), 1);

    /* Mutating one instance leaves the other untouched; destroying one keeps the other alive. */
    hash_b_before = pixel_room_state_hash(b);
    CHECK_EQ_LONG(pixel_room_set_camera_yaw(a, 1.0f), PIXELICA_OK);
    ticks(a, PIXELICA_BTN_START | PIXELICA_BTN_LEFT, 40u);
    CHECK_EQ_LONG(pixel_room_state_hash(b) == hash_b_before, 1);
    pixel_room_destroy(a);
    a = NULL;
    ticks(b, PIXELICA_BTN_UP, 30u);
    CHECK(invariants_hold(b));
    CHECK_EQ_LONG(pixel_room_render(b, ctx_b), PIXELICA_OK);

    /* A new instance after destruction starts from the pristine state. */
    a = make_room(assets, 256u, 192u);
    {
        PixelRoom *fresh = make_room(assets, 256u, 192u);
        if (a != NULL && fresh != NULL) {
            CHECK_EQ_LONG(pixel_room_state_hash(a) == pixel_room_state_hash(fresh), 1);
            CHECK_EQ_LONG(snap(a).tick, 0);
        }
        pixel_room_destroy(fresh);
    }

cleanup:
    pixel_room_destroy(a);
    pixel_room_destroy(b);
    pixel_room_destroy(solo_a);
    pixelica_destroy(ctx_a);
    pixelica_destroy(ctx_b);
    pixelica_destroy(ctx_solo);
}

/* ------------------------------------------------------------------------ */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------ */

static void hud_pip_pixel(uint16_t h, uint32_t index, int *x, int *y)
{
    float pip = floorf((float)h / 24.0f);
    float gap, margin;
    if (pip < 3.0f) {
        pip = 3.0f;
    }
    gap = floorf(pip * 0.5f);
    if (gap < 1.0f) {
        gap = 1.0f;
    }
    margin = pip;
    *x = (int)(margin + (float)index * (pip + gap) + floorf(pip * 0.5f));
    *y = (int)(margin + floorf(pip * 0.5f));
}

static void hud_flash_pixel(uint16_t h, int *x, int *y)
{
    float pip = floorf((float)h / 24.0f);
    float gap, margin;
    if (pip < 3.0f) {
        pip = 3.0f;
    }
    gap = floorf(pip * 0.5f);
    if (gap < 1.0f) {
        gap = 1.0f;
    }
    margin = pip;
    *x = (int)(margin + (float)PIXEL_ROOM_PROP_COUNT * (pip + gap) + gap + pip);
    *y = (int)(margin + floorf(pip * 0.5f));
}

static uint32_t count_distinct_colors(const PIXELICA_Context *ctx, uint16_t w, uint16_t h)
{
    static uint8_t seen[65536 / 8];
    const uint16_t *px = pixelica_color_buffer(ctx);
    size_t i, n = (size_t)w * h;
    uint32_t distinct = 0u;
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < n; ++i) {
        uint32_t c = px[i];
        if ((seen[c >> 3] & (1u << (c & 7u))) == 0u) {
            seen[c >> 3] = (uint8_t)(seen[c >> 3] | (1u << (c & 7u)));
            ++distinct;
        }
    }
    return distinct;
}

/*
 * The two most frequent texel colours of the supplied checker asset, decoded
 * with the same loader the room uses. For the synthetic texture these are
 * TEX_A / TEX_B; for the embedded checker (tiles plus grout) they are its two
 * tile colours, so the floor check follows whatever fixture is being tested.
 */
static bool checker_dominant_colors(const PixelRoomBlob *checker, uint16_t *first, uint16_t *second)
{
    static uint8_t storage[64u * 1024u + 64u];
    static uint32_t counts[65536];
    PIXELICA_Texture tex;
    size_t need = 0u, n, i;
    uint32_t best = 0u, next = 0u;

    if (pixelica_texture_asset_memory_required(checker->data, checker->size, &need) != PIXELICA_OK ||
        need > sizeof(storage) ||
        pixelica_texture_asset_load(&tex, checker->data, checker->size, storage, sizeof(storage)) != PIXELICA_OK ||
        tex.pixels == NULL) {
        return false;
    }
    n = (size_t)tex.width * tex.height;
    for (i = 0; i < n; ++i) {
        ++counts[tex.pixels[i]];
    }
    for (i = 0; i < n; ++i) { /* first pass picks the top colour, second the runner-up */
        if (counts[tex.pixels[i]] > best) {
            best = counts[tex.pixels[i]];
            *first = tex.pixels[i];
        }
    }
    for (i = 0; i < n; ++i) {
        if (tex.pixels[i] != *first && counts[tex.pixels[i]] > next) {
            next = counts[tex.pixels[i]];
            *second = tex.pixels[i];
        }
    }
    for (i = 0; i < n; ++i) {
        counts[tex.pixels[i]] = 0u;
    }
    return next > 0u;
}

static void test_rendering(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    uint16_t tex_first = 0u, tex_second = 0u;
    bool have_tex_colors = checker_dominant_colors(&assets->checker, &tex_first, &tex_second);

    PixelRoom *room = make_room(assets, w, h);
    PIXELICA_Context *ctx = make_context(w, h);
    PIXELICA_Context *ctx2 = make_context(w, h);
    PIXELICA_FrameStats stats;
    uint64_t state_before, hash1, hash2;
    uint32_t pixels = (uint32_t)w * h;
    int px, py;

    if (room == NULL || ctx == NULL || ctx2 == NULL) {
        goto cleanup;
    }
    state_before = pixel_room_state_hash(room);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    CHECK_EQ_LONG(pixel_room_state_hash(room) == state_before, 1); /* render is read-only */
    stats = pixelica_frame_stats(ctx);
    CHECK_EQ_LONG(stats.last_error, PIXELICA_OK);
    CHECK(stats.draw_calls >= 12u);
    CHECK(stats.draw_calls <= PIXEL_ROOM_MAX_DRAW_COMMANDS);
    CHECK(stats.triangles_rasterized > 100u);
    CHECK(stats.pixels_written > pixels / 5u);
    CHECK(stats.pixels_covered > 0u && stats.pixels_covered <= pixels);
    CHECK(stats.fragments_covered >= stats.pixels_covered);
    CHECK(count_distinct_colors(ctx, w, h) >= 12u);
    CHECK(pixelica_memory_guards_intact(ctx));

    /*
     * The floor is really textured: both dominant colours of the supplied checker
     * asset show up as exact texels. The expected colours are decoded from the
     * asset under test, not hard-coded, so this covers synthetic and embedded.
     */
    CHECK(have_tex_colors);
    if (have_tex_colors) {
        CHECK(tex_first != tex_second);
        CHECK(pixelica_t_count_color(ctx, w, h, tex_first) > pixels / 100u);
        CHECK(pixelica_t_count_color(ctx, w, h, tex_second) > pixels / 100u);
    }

    /* HUD: an idle pip, then pip/flash colours follow the game state. */
    hud_pip_pixel(h, 0u, &px, &py);
    CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(70, 74, 92));
    hud_flash_pixel(h, &px, &py);
    CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(20, 22, 34));

    /* Deterministic: the same state renders to the same pixels, also on another context. */
    hash1 = pixelica_t_hash16(pixelica_color_buffer(ctx), pixels);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_hash16(pixelica_color_buffer(ctx), pixels) == hash1, 1);
    CHECK_EQ_LONG(pixel_room_render(room, ctx2), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_hash16(pixelica_color_buffer(ctx2), pixels) == hash1, 1);

    /* A miss turns the flash red, then it fades back. */
    tick(room, PIXELICA_BTN_A);
    CHECK_EQ_LONG(snap(room).last_event, PIXEL_ROOM_EVENT_NOTHING);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    hud_flash_pixel(h, &px, &py);
    CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(232, 80, 80));
    ticks(room, 0u, PIXEL_ROOM_FLASH_TICKS);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(20, 22, 34));

    /* Moving the cube, orbiting the camera and investigating each change the picture. */
    hash1 = pixelica_t_hash16(pixelica_color_buffer(ctx), pixels);
    ticks(room, PIXELICA_BTN_UP, 30u);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    hash2 = pixelica_t_hash16(pixelica_color_buffer(ctx), pixels);
    CHECK_EQ_LONG(hash1 != hash2, 1);
    ticks(room, PIXELICA_BTN_START | PIXELICA_BTN_RIGHT, 30u);
    CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_hash16(pixelica_color_buffer(ctx), pixels) != hash2, 1);
    CHECK(count_distinct_colors(ctx, w, h) >= 12u);

    {
        PixelRoomSnapshot s;
        CHECK(scan_for_focus(room, PIXEL_ROOM_PROP_PLANT));
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        hash1 = pixelica_t_hash16(pixelica_color_buffer(ctx), pixels);
        hud_pip_pixel(h, PIXEL_ROOM_PROP_PLANT, &px, &py);
        CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(250, 220, 80)); /* in reach */
        tick(room, PIXELICA_BTN_A);
        s = snap(room);
        CHECK_EQ_LONG(s.last_event, PIXEL_ROOM_EVENT_INVESTIGATED);
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        CHECK_EQ_LONG(pixelica_t_hash16(pixelica_color_buffer(ctx), pixels) != hash1, 1);
        CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(110, 235, 235)); /* in reach + done */
        hud_flash_pixel(h, &px, &py);
        CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(90, 232, 120));
        /* Walk away: the pip stays marked as investigated. */
        CHECK_EQ_LONG(pixel_room_teleport_player(room, 0.0f, 2.5f), PIXELICA_OK);
        tick(room, 0u);
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        hud_pip_pixel(h, PIXEL_ROOM_PROP_PLANT, &px, &py);
        CHECK_EQ_LONG(pixelica_t_color_at(ctx, w, px, py), PIXELICA_RGB565(80, 205, 115));
    }

cleanup:
    pixel_room_destroy(room);
    pixelica_destroy(ctx);
    pixelica_destroy(ctx2);
}

/*
 * Winding audit: a wrongly wound visible face is culled under PIXELICA_CULL_BACK but
 * drawn under PIXELICA_CULL_NONE, so the two pictures of a room whose visible
 * surfaces are all front-facing must match (hidden back faces lose the depth
 * test). A tiny tolerance absorbs depth ties at coplanar contacts.
 */
static void test_winding_and_camera_sides(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    static uint16_t back_pixels[256 * 192];
    static const float yaws[3] = {0.2f, 0.78539816f, 1.3f};
    PixelRoom *room = make_room(assets, w, h);
    PIXELICA_Context *ctx = make_context(w, h);
    uint32_t i;
    size_t p, n = (size_t)w * h;

    if (room == NULL || ctx == NULL) {
        goto cleanup;
    }
    for (i = 0; i < 3u; ++i) {
        size_t different = 0;
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, yaws[i]), PIXELICA_OK);
        CHECK_EQ_LONG(pixel_room_render_with_cull(room, ctx, PIXELICA_CULL_BACK), PIXELICA_OK);
        memcpy(back_pixels, pixelica_color_buffer(ctx), n * sizeof(uint16_t));
        CHECK_EQ_LONG(pixel_room_render_with_cull(room, ctx, PIXELICA_CULL_NONE), PIXELICA_OK);
        for (p = 0; p < n; ++p) {
            if (back_pixels[p] != pixelica_color_buffer(ctx)[p]) {
                ++different;
            }
        }
        if (different * 500u > n) {
            fprintf(stderr, "cull audit: %zu of %zu pixels differ at yaw %.2f\n", different, n,
                    (double)yaws[i]);
        }
        CHECK(different * 500u <= n); /* at most 0.2% */
    }

    /*
     * Viewed from behind, the single-sided walls disappear (back-face culled), so the
     * room stays visible. Compare how much of the screen the far-side walls cover.
     */
    {
        uint32_t front, behind;
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 0.78539816f), PIXELICA_OK);
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        front = pixelica_frame_stats(ctx).triangles_culled;
        CHECK_EQ_LONG(pixel_room_set_camera_yaw(room, 0.78539816f + (float)PI_D), PIXELICA_OK);
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK);
        behind = pixelica_frame_stats(ctx).triangles_culled;
        CHECK_EQ_LONG(pixelica_frame_stats(ctx).last_error, PIXELICA_OK);
        CHECK(behind >= front + 12u); /* the walls' only side now faces away */
        CHECK(count_distinct_colors(ctx, w, h) >= 8u);
    }

    /* A renderer with too few draw commands reports the failure instead of crashing. */
    {
        PIXELICA_Config tight = pixel_room_context_config(w, h);
        PIXELICA_Context *small = NULL;
        tight.max_draw_commands = 2u;
        CHECK_EQ_LONG(pixelica_create(&tight, NULL, &small), PIXELICA_OK);
        if (small != NULL) {
            CHECK(pixel_room_render(room, small) != PIXELICA_OK);
            pixelica_destroy(small);
        }
        CHECK_EQ_LONG(pixel_room_render(room, ctx), PIXELICA_OK); /* the room is unharmed */
    }

cleanup:
    pixel_room_destroy(room);
    pixelica_destroy(ctx);
}

/* ------------------------------------------------------------------------ */
/* Soak                                                                      */
/* ------------------------------------------------------------------------ */

static void test_soak(const PixelRoomAssets *assets, uint16_t w, uint16_t h)
{
    CountingAllocator counting;
    PIXELICA_Status status = PIXELICA_OK;
    PixelRoom *room, *twin;
    PIXELICA_Context *ctx = make_context(w, h);
    uint32_t rng = 0xC0FFEEu, rng2 = 0xC0FFEEu, held = 0u, held2 = 0u, i;
    size_t calls;
    PixelRoomSnapshot s;
    uint32_t visit_sum;

    counting_init(&counting);
    room = create_room(assets, w, h, &counting, &status);
    twin = make_room(assets, w, h);
    CHECK_EQ_LONG(status, PIXELICA_OK);
    if (room == NULL || twin == NULL || ctx == NULL) {
        goto cleanup;
    }
    calls = counting.calls;
    for (i = 0; i < 20000u; ++i) {
        if ((i % 6u) == 0u) {
            held = pseudo_buttons(&rng);
            held2 = pseudo_buttons(&rng2);
        }
        tick(room, held);
        tick(twin, held2);
        if (!invariants_hold(room)) {
            ++g_failures;
            fprintf(stderr, "soak invariant violated at step %u\n", (unsigned)i);
            break;
        }
        if ((i % 97u) == 0u && pixel_room_render(room, ctx) != PIXELICA_OK) {
            ++g_failures;
            fprintf(stderr, "soak render failed at step %u\n", (unsigned)i);
            break;
        }
    }
    CHECK_EQ_LONG(counting.calls, calls); /* no heap traffic in the loop */
    CHECK_EQ_LONG(counting.live, 1);
    CHECK_EQ_LONG(pixel_room_state_hash(room) == pixel_room_state_hash(twin), 1); /* deterministic */
    s = snap(room);
    CHECK_EQ_LONG(s.tick, 20000);
    visit_sum = 0u;
    for (i = 0; i < PIXEL_ROOM_PROP_COUNT; ++i) {
        visit_sum += s.visits[i];
        CHECK_EQ_LONG(((s.investigated_mask >> i) & 1u) != 0u, s.visits[i] > 0u);
    }
    CHECK_EQ_LONG(visit_sum, s.investigate_count);
    CHECK(s.investigate_count + s.miss_count > 0u); /* the random walk did press A */
    CHECK(pixelica_memory_guards_intact(ctx));

cleanup:
    pixel_room_destroy(room);
    pixel_room_destroy(twin);
    CHECK_EQ_LONG(counting.live, 0);
    pixelica_destroy(ctx);
}

/* ------------------------------------------------------------------------ */
/* Embedded (generated) assets                                               */
/* ------------------------------------------------------------------------ */

#ifdef PIXEL_ROOM_TEST_EMBEDDED
static void test_embedded_assets(void)
{
    PixelRoomAssets assets;
    PixelRoomBlob cart_blob;
    PIXELICA_Cart cart;
    const void *data = NULL;
    size_t size = 0u;
    uint32_t id, slot;
    uint32_t matched[SLOT_COUNT];
    const PixelRoomBlob *slots[SLOT_COUNT];

    CHECK(pixel_room_embedded_assets(&assets));
    CHECK(pixel_room_embedded_cart(&cart_blob));

    test_lifecycle(&assets, 160u, 120u);
    test_lifecycle(&assets, 256u, 192u);
    test_movement_and_collision(&assets);
    test_camera(&assets);
    test_every_prop_is_reachable(&assets);
    test_investigation_edges(&assets);
    test_render_schedules(&assets, 160u, 120u);
    test_independent_instances(&assets);
    test_rendering(&assets, 160u, 120u);
    test_rendering(&assets, 256u, 192u);
    test_winding_and_camera_sides(&assets, 160u, 120u);
    test_winding_and_camera_sides(&assets, 256u, 192u);
    test_soak(&assets, 160u, 120u);

    /* The cart bundles exactly the standalone resources used above. */
    memset(matched, 0, sizeof(matched));
    for (slot = 0; slot < PIXEL_ROOM_PROP_COUNT; ++slot) {
        slots[slot] = &assets.props[slot];
    }
    slots[SLOT_CHECKER] = &assets.checker;
    CHECK_EQ_LONG(pixelica_cart_open(&cart, cart_blob.data, cart_blob.size), PIXELICA_OK);
    CHECK(cart.count >= SLOT_COUNT);
    for (id = 0u; id < cart.count; ++id) {
        PIXELICA_CartEntry entry;
        CHECK_EQ_LONG(pixelica_cart_entry(&cart, id, &entry), PIXELICA_OK);
        CHECK_EQ_LONG(pixelica_cart_find(&cart, entry.id, &data, &size), PIXELICA_OK);
        CHECK(data == entry.data && size == entry.size);
        for (slot = 0; slot < SLOT_COUNT; ++slot) {
            if (size == slots[slot]->size && memcmp(data, slots[slot]->data, size) == 0) {
                ++matched[slot];
            }
        }
    }
    for (slot = 0; slot < SLOT_COUNT; ++slot) {
        CHECK_EQ_LONG(matched[slot], 1);
    }
    CHECK_EQ_LONG(pixelica_cart_find(&cart, 0u, &data, &size), PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_cart_find(&cart, 0xFFFFFFFFu, &data, &size), PIXELICA_ERR_NOT_FOUND);
}
#endif

int main(void)
{
    static TestAssets assets;

    build_test_assets(&assets);
    CHECK(assets.size[0] > 0u && assets.size[SLOT_CHECKER] > 0u);
    {
        /* The decoder behind test_rendering must recover the synthetic checker's colours. */
        uint16_t c1 = 0u, c2 = 0u;
        CHECK(checker_dominant_colors(&assets.view.checker, &c1, &c2));
        CHECK((c1 == TEX_A && c2 == TEX_B) || (c1 == TEX_B && c2 == TEX_A));
    }

    test_lifecycle(&assets.view, 160u, 120u);
    test_lifecycle(&assets.view, 256u, 192u);
    test_create_argument_errors(&assets.view);
    test_failed_load_cleanup(&assets);
    test_movement_and_collision(&assets.view);
    test_camera(&assets.view);
    test_every_prop_is_reachable(&assets.view);
    test_investigation_edges(&assets.view);
    test_render_schedules(&assets.view, 160u, 120u);
    test_render_schedules(&assets.view, 256u, 192u);
    test_independent_instances(&assets.view);
    test_rendering(&assets.view, 160u, 120u);
    test_rendering(&assets.view, 256u, 192u);
    test_winding_and_camera_sides(&assets.view, 160u, 120u);
    test_winding_and_camera_sides(&assets.view, 256u, 192u);
    test_soak(&assets.view, 160u, 120u);
    test_soak(&assets.view, 256u, 192u);
#ifdef PIXEL_ROOM_TEST_EMBEDDED
    test_embedded_assets();
#endif
    return pixelica_t_finish("test_pixel_room");
}
