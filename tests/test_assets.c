/*
 * Pixelica - asset format v1 loaders: exact decoding, unaligned input and
 * storage, transactional failure (output zeroed, storage untouched), every
 * container/geometry/cart rule, configured limits, exhaustive truncation
 * and single-bit-flip rejection, and zero-copy cart lookup.
 *
 * Test files are assembled here byte by byte (little-endian) with a tiny
 * writer, independent of the loaders; CRCs use pixelica_crc32, which is checked
 * against published reference values first.
 */
#include <stdlib.h>

#include "test_util.h"

#define FOURCC(s) PIXELICA_FOURCC((s)[0], (s)[1], (s)[2], (s)[3])

typedef struct Chunk {
    uint32_t type;
    const uint8_t *data;
    size_t length;
} Chunk;

/* ---- little-endian writer ---------------------------------------------- */

static void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wrf(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    wr32(p, bits);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static Chunk chunk(const char *type, const uint8_t *data, size_t length)
{
    Chunk c;
    c.type = FOURCC(type);
    c.data = data;
    c.length = length;
    return c;
}

/* Header + table + tightly packed payloads with correct CRCs. */
static size_t build_file(uint8_t *out, size_t cap, uint32_t magic,
                         const Chunk *chunks, uint32_t n)
{
    size_t header = 16u + 16u * (size_t)n;
    size_t total = header;
    size_t off;
    uint32_t i;

    for (i = 0; i < n; ++i) {
        total += chunks[i].length;
    }
    if (total > cap) {
        fprintf(stderr, "build_file: buffer too small\n");
        return 0;
    }
    wr32(out, magic);
    wr16(out + 4, 1u);
    wr16(out + 6, (uint32_t)header);
    wr32(out + 8, (uint32_t)total);
    wr32(out + 12, n);
    off = header;
    for (i = 0; i < n; ++i) {
        uint8_t *e = out + 16u + 16u * (size_t)i;
        if (chunks[i].length > 0) {
            memcpy(out + off, chunks[i].data, chunks[i].length);
        }
        wr32(e, chunks[i].type);
        wr32(e + 4, (uint32_t)off);
        wr32(e + 8, (uint32_t)chunks[i].length);
        wr32(e + 12, pixelica_crc32(chunks[i].data, chunks[i].length));
        off += chunks[i].length;
    }
    return total;
}

static int all_bytes(const void *p, size_t n, uint8_t value)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; ++i) {
        if (b[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* ---- sample mesh ------------------------------------------------------- */

static const float s_pos[12] = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f,
                                1.0f,  1.0f,  0.0f, -1.0f, 1.0f, 0.25f};
static const uint16_t s_idx[6] = {0, 1, 2, 0, 2, 3};
static const uint16_t s_col[2] = {0xF800, 0x07E0};
static const float s_uv[8] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};

static uint8_t e_vtx[48], e_idx[12], e_col[4], e_uv[32];

static void encode_f32s(uint8_t *dst, const float *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        wrf(dst + 4u * i, src[i]);
    }
}

static void encode_u16s(uint8_t *dst, const uint16_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        wr16(dst + 2u * i, src[i]);
    }
}

static void encode_sample(void)
{
    encode_f32s(e_vtx, s_pos, 12);
    encode_u16s(e_idx, s_idx, 6);
    encode_u16s(e_col, s_col, 2);
    encode_f32s(e_uv, s_uv, 8);
}

static uint32_t sample_chunks(Chunk *c, int with_uv)
{
    c[0] = chunk("VTX0", e_vtx, sizeof e_vtx);
    c[1] = chunk("IDX0", e_idx, sizeof e_idx);
    c[2] = chunk("COL0", e_col, sizeof e_col);
    if (with_uv) {
        c[3] = chunk("UV00", e_uv, sizeof e_uv);
        return 4;
    }
    return 3;
}

static size_t sample_mesh(uint8_t *out, size_t cap, int with_uv)
{
    Chunk c[4];
    uint32_t n = sample_chunks(c, with_uv);
    return build_file(out, cap, PIXELICA_ASSET_KIND_MESH, c, n);
}

/* Both mesh entry points reject the file with `want` (or with any error if
 * want == PIXELICA_OK), zero the output and leave the storage untouched. */
static int mesh_rejects(const uint8_t *file, size_t size, PIXELICA_Status want)
{
    static unsigned char storage[2048];
    PIXELICA_MeshAsset out;
    size_t need = 77;
    PIXELICA_Status a, b;

    memset(storage, 0xCD, sizeof storage);
    memset(&out, 0xAB, sizeof out);
    a = pixelica_mesh_asset_memory_required(file, size, &need);
    b = pixelica_mesh_asset_load(&out, file, size, storage, sizeof storage);
    if (want == PIXELICA_OK) {
        if (a == PIXELICA_OK || b == PIXELICA_OK) {
            return 0;
        }
    } else if (a != want || b != want) {
        fprintf(stderr, "  mesh_rejects: got %d/%d, want %d\n", (int)a,
                (int)b, (int)want);
        return 0;
    }
    return need == 0 && all_bytes(&out, sizeof out, 0) &&
           all_bytes(storage, sizeof storage, 0xCD);
}

static int texture_rejects(const uint8_t *file, size_t size, PIXELICA_Status want)
{
    static unsigned char storage[2048];
    PIXELICA_Texture out;
    size_t need = 77;
    PIXELICA_Status a, b;

    memset(storage, 0xCD, sizeof storage);
    memset(&out, 0xAB, sizeof out);
    a = pixelica_texture_asset_memory_required(file, size, &need);
    b = pixelica_texture_asset_load(&out, file, size, storage, sizeof storage);
    if (want == PIXELICA_OK) {
        if (a == PIXELICA_OK || b == PIXELICA_OK) {
            return 0;
        }
    } else if (a != want || b != want) {
        fprintf(stderr, "  texture_rejects: got %d/%d, want %d\n", (int)a,
                (int)b, (int)want);
        return 0;
    }
    return need == 0 && all_bytes(&out, sizeof out, 0) &&
           all_bytes(storage, sizeof storage, 0xCD);
}

/* Both mesh entry points accept the file. */
static int mesh_accepts(const uint8_t *file, size_t size)
{
    static unsigned char storage[2048];
    PIXELICA_MeshAsset out;
    size_t need = 0;

    return pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK &&
           need <= sizeof storage &&
           pixelica_mesh_asset_load(&out, file, size, storage, sizeof storage) ==
               PIXELICA_OK;
}

static int cart_accepts(const uint8_t *file, size_t size)
{
    PIXELICA_Cart cart;
    return pixelica_cart_open(&cart, file, size) == PIXELICA_OK && cart.data == file;
}

static int cart_rejects(const uint8_t *file, size_t size, PIXELICA_Status want)
{
    PIXELICA_Cart cart;
    PIXELICA_Status st;

    memset(&cart, 0xAB, sizeof cart);
    st = pixelica_cart_open(&cart, file, size);
    if (want == PIXELICA_OK ? st == PIXELICA_OK : st != want) {
        fprintf(stderr, "  cart_rejects: got %d, want %d\n", (int)st,
                (int)want);
        return 0;
    }
    return all_bytes(&cart, sizeof cart, 0);
}

/* ------------------------------------------------------------------------ */

static void test_crc32_reference(void)
{
    static const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK(pixelica_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(pixelica_crc32(fox, strlen(fox)) == 0x414FA339u);
    CHECK(pixelica_crc32("", 0) == 0u);
    CHECK(pixelica_crc32(NULL, 5) == 0u);
}

static void check_sample_decoded(const PIXELICA_MeshAsset *a, int with_uv)
{
    uint32_t i;

    CHECK_EQ_LONG(a->mesh.vertex_count, 4);
    CHECK_EQ_LONG(a->mesh.index_count, 6);
    CHECK_EQ_LONG(a->mesh.color, 0xFFFF);
    CHECK(a->material.texture == NULL);
    CHECK(a->mesh.vertices != NULL && a->mesh.indices != NULL &&
          a->material.triangle_colors != NULL);
    if (a->mesh.vertices == NULL || a->mesh.indices == NULL ||
        a->material.triangle_colors == NULL) {
        return;
    }
    for (i = 0; i < 4; ++i) {
        CHECK(a->mesh.vertices[i].x == s_pos[i * 3u]);
        CHECK(a->mesh.vertices[i].y == s_pos[i * 3u + 1u]);
        CHECK(a->mesh.vertices[i].z == s_pos[i * 3u + 2u]);
    }
    CHECK(memcmp(a->mesh.indices, s_idx, sizeof s_idx) == 0);
    CHECK(memcmp(a->material.triangle_colors, s_col, sizeof s_col) == 0);
    if (with_uv) {
        CHECK(a->material.uvs != NULL);
        if (a->material.uvs != NULL) {
            for (i = 0; i < 4; ++i) {
                CHECK(a->material.uvs[i].x == s_uv[i * 2u]);
                CHECK(a->material.uvs[i].y == s_uv[i * 2u + 1u]);
            }
        }
    } else {
        CHECK(a->material.uvs == NULL);
    }
}

static void test_mesh_load_valid(void)
{
    static uint8_t file[512];
    static unsigned char in_buf[512 + 8];
    static unsigned char storage[256 + 32];
    PIXELICA_MeshAsset a;
    size_t size, need = 0;
    int with_uv;
    size_t ia, is;

    for (with_uv = 0; with_uv < 2; ++with_uv) {
        size = sample_mesh(file, sizeof file, with_uv);
        CHECK(pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK);
        CHECK_EQ_LONG(need, 48 + (with_uv ? 32 : 0) + 12 + 4 + 15);
        CHECK(pixelica_mesh_asset_load(&a, file, size, storage, need) == PIXELICA_OK);
        check_sample_decoded(&a, with_uv);
        /* One byte less than required: NO_MEMORY, output zeroed. */
        memset(storage, 0xCD, sizeof storage);
        CHECK(pixelica_mesh_asset_load(&a, file, size, storage, need - 1u) ==
              PIXELICA_ERR_NO_MEMORY);
        CHECK(all_bytes(&a, sizeof a, 0));
        CHECK(all_bytes(storage, sizeof storage, 0xCD));
    }

    /* Every input and storage misalignment decodes identically, into
     * 16-byte aligned arrays inside the storage. */
    size = sample_mesh(file, sizeof file, 1);
    CHECK(pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK);
    for (ia = 0; ia < 8; ++ia) {
        memcpy(in_buf + ia, file, size);
        for (is = 0; is < 16; ++is) {
            CHECK(pixelica_mesh_asset_load(&a, in_buf + ia, size, storage + is,
                                      need) == PIXELICA_OK);
            CHECK(((uintptr_t)(const void *)a.mesh.vertices % 16u) == 0);
            CHECK((const unsigned char *)(const void *)a.mesh.vertices >=
                      storage + is &&
                  (const unsigned char *)(const void *)
                          (a.material.triangle_colors + 2) <=
                      storage + is + need);
            check_sample_decoded(&a, 1);
        }
    }
    /* The asset references storage only: wiping the input keeps it valid. */
    memset(in_buf, 0, sizeof in_buf);
    check_sample_decoded(&a, 1);
}

static void test_mesh_args_and_overlap(void)
{
    static unsigned char big[1024];
    static uint8_t file[512];
    static unsigned char storage[256];
    PIXELICA_MeshAsset a;
    size_t size = sample_mesh(file, sizeof file, 1);
    size_t need = 0;

    CHECK(pixelica_mesh_asset_memory_required(file, size, NULL) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mesh_asset_memory_required(NULL, size, &need) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mesh_asset_load(NULL, file, size, storage, sizeof storage) ==
          PIXELICA_ERR_INVALID_ARG);
    memset(&a, 0xAB, sizeof a);
    CHECK(pixelica_mesh_asset_load(&a, NULL, size, storage, sizeof storage) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(all_bytes(&a, sizeof a, 0));
    CHECK(pixelica_mesh_asset_load(&a, file, size, NULL, sizeof storage) ==
          PIXELICA_ERR_INVALID_ARG);

    /* Overlap by one byte (either side) is rejected; adjacency is fine. */
    CHECK(pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK);
    memcpy(big + 256, file, size);
    CHECK(pixelica_mesh_asset_load(&a, big + 256, size, big + 256 + size - 1u,
                              need) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mesh_asset_load(&a, big + 256, size, big + 257 - need, need) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mesh_asset_load(&a, big + 256, size, big + 256 + 4, need) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_mesh_asset_load(&a, big + 256, size, big + 256 + size, need) ==
          PIXELICA_OK);
    check_sample_decoded(&a, 1);
    CHECK(pixelica_mesh_asset_load(&a, big + 256, size, big + 256 - need, need) ==
          PIXELICA_OK);
    check_sample_decoded(&a, 1);

    /* Oversized input is refused before any byte is read. */
    CHECK(pixelica_mesh_asset_memory_required(
              file, (size_t)PIXELICA_ASSET_MAX_FILE_BYTES + 1u, &need) ==
          PIXELICA_ERR_CAPACITY);
}

static void test_mesh_container_errors(void)
{
    static uint8_t file[1024];
    static uint8_t copy[1024];
    static uint8_t junk[16];
    Chunk c[6];
    uint32_t n;
    size_t size;

    /* Header fields. */
    size = sample_mesh(file, sizeof file, 1);
    CHECK(mesh_accepts(file, size)); /* sanity: it is valid */
    memcpy(copy, file, size);
    copy[0] = 'X';
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    memcpy(copy, file, size);
    wr16(copy + 4, 2u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    wr16(copy + 4, 0u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    memcpy(copy, file, size);
    wr16(copy + 6, 16u + 16u * 4u + 16u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    memcpy(copy, file, size);
    wr32(copy + 8, (uint32_t)size + 1u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    wr32(copy + 8, (uint32_t)size - 1u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    memcpy(copy, file, size);
    wr32(copy + 12, 0u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    wr32(copy + 12, 17u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    wr32(copy + 12, 0xFFFFFFFFu);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    /* A texture file is not a mesh. */
    memcpy(copy, file, size);
    wr32(copy, PIXELICA_ASSET_KIND_TEXTURE);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));

    /* Trailing byte (file_size consistent, but after the last payload). */
    memcpy(copy, file, size);
    copy[size] = 0;
    wr32(copy + 8, (uint32_t)size + 1u);
    CHECK(mesh_rejects(copy, size + 1u, PIXELICA_ERR_BAD_ASSET));

    /* Gap: second payload starts one byte late (and the file is consistent
     * in size). */
    memcpy(copy, file, size);
    memmove(copy + rd32(copy + 16u + 16u + 4u) + 1u,
            copy + rd32(copy + 16u + 16u + 4u),
            size - rd32(copy + 16u + 16u + 4u));
    wr32(copy + 8, (uint32_t)size + 1u);
    wr32(copy + 16u + 16u + 4u, rd32(copy + 16u + 16u + 4u) + 1u);
    wr32(copy + 16u + 32u + 4u, rd32(copy + 16u + 32u + 4u) + 1u);
    wr32(copy + 16u + 48u + 4u, rd32(copy + 16u + 48u + 4u) + 1u);
    CHECK(mesh_rejects(copy, size + 1u, PIXELICA_ERR_BAD_ASSET));

    /* Overlap: second payload offset moved one byte back. */
    memcpy(copy, file, size);
    wr32(copy + 16u + 16u + 4u, rd32(copy + 16u + 16u + 4u) - 1u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    /* Length running past the end of the file. */
    memcpy(copy, file, size);
    wr32(copy + 16u + 48u + 8u, 0xFFFFFFF0u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    wr32(copy + 16u + 48u + 4u, 0xFFFFFFF0u);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));

    /* CRC mismatch in one payload byte. */
    memcpy(copy, file, size);
    copy[size - 1u] ^= 0x01u;
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    /* ... and in the stored CRC itself. */
    memcpy(copy, file, size);
    copy[16u + 12u] ^= 0x80u;
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));

    /* Duplicate, unknown and missing chunks (all CRCs valid). */
    n = sample_chunks(c, 1);
    c[4] = chunk("VTX0", e_vtx, sizeof e_vtx);
    size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, c, 5);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    c[4] = chunk("UV00", e_uv, sizeof e_uv);
    size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, c, 5);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    c[4] = chunk("XTRA", junk, sizeof junk);
    size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, c, 5);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    c[4] = chunk("vtx0", e_vtx, sizeof e_vtx); /* case matters */
    size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, c, 5);
    CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    CHECK_EQ_LONG(n, 4);
    for (n = 0; n < 3; ++n) {
        Chunk d[3];
        uint32_t k, m = 0;
        for (k = 0; k < 3; ++k) {
            if (k != n) {
                d[m++] = c[k];
            }
        }
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, d, m);
        CHECK(mesh_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    }

    /* Chunk order in the table is free: UV00 first still loads. */
    sample_chunks(c, 1);
    {
        Chunk d[4];
        PIXELICA_MeshAsset a;
        static unsigned char storage[256];
        d[0] = c[3];
        d[1] = c[2];
        d[2] = c[0];
        d[3] = c[1];
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_MESH, d, 4);
        CHECK(pixelica_mesh_asset_load(&a, copy, size, storage, sizeof storage) ==
              PIXELICA_OK);
        check_sample_decoded(&a, 1);
    }
}

static void test_mesh_geometry_errors(void)
{
    static uint8_t file[1024];
    uint8_t vtx[48], idx[12], col[4], uv[32];
    Chunk c[4];
    size_t size;

#define RESET()                                   \
    do {                                          \
        memcpy(vtx, e_vtx, sizeof vtx);           \
        memcpy(idx, e_idx, sizeof idx);           \
        memcpy(col, e_col, sizeof col);           \
        memcpy(uv, e_uv, sizeof uv);              \
        c[0] = chunk("VTX0", vtx, sizeof vtx);    \
        c[1] = chunk("IDX0", idx, sizeof idx);    \
        c[2] = chunk("COL0", col, sizeof col);    \
        c[3] = chunk("UV00", uv, sizeof uv);      \
    } while (0)
#define BUILD() build_file(file, sizeof file, PIXELICA_ASSET_KIND_MESH, c, 4)

    RESET();
    size = BUILD();
    CHECK(mesh_accepts(file, size));

    /* Index == vertex_count (last index). */
    RESET();
    wr16(idx + 10, 4u);
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wr16(idx + 0, 0xFFFFu);
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));

    /* Non-finite positions (last vertex). */
    RESET();
    wrf(vtx + 44, pixelica_t_nan());
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wrf(vtx + 36, -pixelica_t_inf());
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));

    /* UVs outside [0, 1] or NaN; exact bounds are fine. */
    RESET();
    wrf(uv + 28, -0.001f);
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wrf(uv + 0, 1.0001f);
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wrf(uv + 4, pixelica_t_nan());
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wrf(uv + 4, pixelica_t_inf());
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    wrf(uv + 0, 0.0f);
    wrf(uv + 4, 1.0f);
    wrf(uv + 8, -0.0f);
    size = BUILD();
    CHECK(mesh_accepts(file, size));

    /* Length rules. */
    RESET();
    c[0].length = 47; /* not a multiple of 12 */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[0].length = 36; /* 3 vertices: UV00 count mismatch (and index 3) */
    c[3].length = 24;
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[3].length = 24; /* UV00 for 3 of 4 vertices */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[3].length = 31;
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[1].length = 11; /* odd index bytes */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[1].length = 10; /* 5 indices: not a triangle list */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[1].length = 6; /* 1 triangle but COL0 has 2 colors */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[2].length = 2; /* 1 color for 2 triangles */
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[2].length = 3;
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    /* Empty vertices / indices (with matching empty colors). */
    RESET();
    c[0].length = 0;
    c[3].length = 0;
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));
    RESET();
    c[1].length = 0;
    c[2].length = 0;
    size = BUILD();
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));

    /* One triangle and matching color is valid. */
    RESET();
    c[1].length = 6;
    c[2].length = 2;
    size = BUILD();
    CHECK(mesh_accepts(file, size));
#undef RESET
#undef BUILD
}

/* Every strict prefix and every single-bit flip of a valid file fails
 * cleanly (CRC-32 detects all single-bit payload errors; every header and
 * table field is checked exactly). */
static void test_mesh_truncation_and_bitflips(void)
{
    static uint8_t file[512];
    static uint8_t copy[512];
    size_t size = sample_mesh(file, sizeof file, 1);
    size_t i;
    uint32_t bad_prefix = 0, bad_flip = 0;
    int bit;

    for (i = 0; i < size; ++i) {
        memcpy(copy, file, i);
        if (!mesh_rejects(copy, i, PIXELICA_OK)) {
            ++bad_prefix;
        }
    }
    for (i = 0; i < size; ++i) {
        for (bit = 0; bit < 8; ++bit) {
            memcpy(copy, file, size);
            copy[i] = (uint8_t)(copy[i] ^ (1u << bit));
            if (!mesh_rejects(copy, size, PIXELICA_OK)) {
                ++bad_flip;
            }
        }
    }
    CHECK_EQ_LONG(bad_prefix, 0);
    CHECK_EQ_LONG(bad_flip, 0);
}

/* Configured limits: PIXELICA_ASSET_MAX_VERTICES / _INDICES -> CAPACITY, the
 * exact limit is accepted. */
static void test_mesh_limits(void)
{
    static uint8_t vtx[(PIXELICA_ASSET_MAX_VERTICES + 1u) * 12u];
    static uint8_t idx[(PIXELICA_ASSET_MAX_INDICES + 3u) * 2u];
    static uint8_t col[(PIXELICA_ASSET_MAX_INDICES / 3u + 1u) * 2u];
    static uint8_t file[16u + 48u + sizeof vtx + sizeof idx + sizeof col];
    Chunk c[3];
    size_t size, need = 0;
    uint32_t i;

    memset(vtx, 0, sizeof vtx); /* all-zero floats: finite */
    memset(idx, 0, sizeof idx); /* index 0 everywhere */
    memset(col, 0, sizeof col);

    c[0] = chunk("VTX0", vtx, (size_t)PIXELICA_ASSET_MAX_VERTICES * 12u);
    c[1] = chunk("IDX0", idx, 6);
    c[2] = chunk("COL0", col, 2);
    size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_MESH, c, 3);
    CHECK(pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK);
    CHECK(need >= (size_t)PIXELICA_ASSET_MAX_VERTICES * sizeof(PIXELICA_Vec3));

    c[0].length = ((size_t)PIXELICA_ASSET_MAX_VERTICES + 1u) * 12u;
    size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_MESH, c, 3);
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_CAPACITY));

    c[0].length = 36;
    c[1].length = (size_t)PIXELICA_ASSET_MAX_INDICES * 2u;
    c[2].length = (size_t)(PIXELICA_ASSET_MAX_INDICES / 3u) * 2u;
    for (i = 0; i < 3; ++i) {
        wr16(idx + 2u * i, i);
    }
    size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_MESH, c, 3);
    CHECK(pixelica_mesh_asset_memory_required(file, size, &need) == PIXELICA_OK);
    c[1].length = ((size_t)PIXELICA_ASSET_MAX_INDICES + 3u) * 2u;
    c[2].length = ((size_t)PIXELICA_ASSET_MAX_INDICES / 3u + 1u) * 2u;
    size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_MESH, c, 3);
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_CAPACITY));
}

/* Loaded meshes render through pixelica_draw_mesh_ex with a bound texture and
 * with their per-triangle colors. */
static void test_mesh_draw_integration(void)
{
    static uint8_t file[512];
    static unsigned char storage[256];
    static const uint16_t texels[1] = {0x1234};
    PIXELICA_Texture tex;
    PIXELICA_MeshAsset a;
    PIXELICA_Config cfg = pixelica_t_config(16, 16);
    PIXELICA_Context *ctx = NULL;
    float id[16];
    size_t size = sample_mesh(file, sizeof file, 1);

    tex.pixels = texels;
    tex.width = 1;
    tex.height = 1;
    pixelica_mat4_identity(id);
    CHECK(pixelica_mesh_asset_load(&a, file, size, storage, sizeof storage) ==
          PIXELICA_OK);
    CHECK(pixelica_create(&cfg, NULL, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    pixelica_begin_frame(ctx, 0);
    CHECK(pixelica_draw_mesh_ex(ctx, &a.mesh, id, &a.material) == PIXELICA_OK);
    CHECK(pixelica_t_count_color(ctx, 16, 16, 0xF800) > 0u);
    CHECK(pixelica_t_count_color(ctx, 16, 16, 0x07E0) > 0u);
    a.material.texture = &tex;
    pixelica_begin_frame(ctx, 0);
    CHECK(pixelica_draw_mesh_ex(ctx, &a.mesh, id, &a.material) == PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_t_count_color(ctx, 16, 16, 0x1234), 16 * 16);
    pixelica_destroy(ctx);
}

/* ---- textures ---------------------------------------------------------- */

static size_t build_texture(uint8_t *out, size_t cap, uint32_t w, uint32_t h,
                            uint32_t reserved, const uint16_t *texels,
                            size_t texel_count)
{
    static uint8_t data[4096 * 2 + 64];
    uint8_t info[8];
    Chunk c[2];

    wr16(info, w);
    wr16(info + 2, h);
    wr32(info + 4, reserved);
    encode_u16s(data, texels, texel_count);
    c[0] = chunk("INFO", info, sizeof info);
    c[1] = chunk("DATA", data, texel_count * 2u);
    return build_file(out, cap, PIXELICA_ASSET_KIND_TEXTURE, c, 2);
}

static void test_texture_assets(void)
{
    static const uint16_t texels[6] = {1, 2, 3, 0xF800, 0x07E0, 0xFFFF};
    static uint16_t many[4097];
    static uint8_t file[16384];
    static uint8_t copy[16384];
    static unsigned char storage[64 + 16];
    PIXELICA_Texture tex;
    size_t size, need = 0, i;
    int bit;
    uint32_t bad = 0;

    size = build_texture(file, sizeof file, 3, 2, 0, texels, 6);
    CHECK(pixelica_texture_asset_memory_required(file, size, &need) == PIXELICA_OK);
    CHECK_EQ_LONG(need, 12 + 15);
    for (i = 0; i < 16; ++i) {
        CHECK(pixelica_texture_asset_load(&tex, file, size, storage + i, need) ==
              PIXELICA_OK);
        CHECK(tex.width == 3 && tex.height == 2);
        CHECK(tex.pixels != NULL &&
              memcmp(tex.pixels, texels, sizeof texels) == 0);
        CHECK(((uintptr_t)(const void *)tex.pixels % 16u) == 0);
    }
    memset(storage, 0xCD, sizeof storage);
    CHECK(pixelica_texture_asset_load(&tex, file, size, storage, need - 1u) ==
          PIXELICA_ERR_NO_MEMORY);
    CHECK(all_bytes(&tex, sizeof tex, 0));
    CHECK(all_bytes(storage, sizeof storage, 0xCD));
    CHECK(pixelica_texture_asset_load(&tex, file, size, file + 1, need) ==
          PIXELICA_ERR_INVALID_ARG); /* overlapping storage */
    CHECK(pixelica_texture_asset_load(NULL, file, size, storage, need) ==
          PIXELICA_ERR_INVALID_ARG);

    /* INFO/DATA rules. */
    size = build_texture(copy, sizeof copy, 3, 2, 1, texels, 6);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET)); /* reserved */
    size = build_texture(copy, sizeof copy, 0, 2, 0, texels, 0);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    size = build_texture(copy, sizeof copy, 3, 0, 0, texels, 0);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    size = build_texture(copy, sizeof copy, 3, 2, 0, texels, 5);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    size = build_texture(copy, sizeof copy, 2, 2, 0, texels, 6);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    {
        uint8_t info[9] = {3, 0, 2, 0, 0, 0, 0, 0, 0};
        uint8_t data[12];
        Chunk c[3];
        encode_u16s(data, texels, 6);
        c[0] = chunk("INFO", info, 9);
        c[1] = chunk("DATA", data, 12);
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_TEXTURE, c, 2);
        CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
        c[0].length = 8;
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_TEXTURE, c, 1);
        CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET)); /* no DATA */
        c[2] = chunk("INFO", info, 8);
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_TEXTURE, c, 3);
        CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET)); /* dup */
        c[2] = chunk("UV00", data, 8);
        size = build_file(copy, sizeof copy, PIXELICA_ASSET_KIND_TEXTURE, c, 3);
        CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET)); /* foreign */
    }
    /* Dimension limit: 4097 x 1 is well formed but over the limit. */
    for (i = 0; i < 4097; ++i) {
        many[i] = (uint16_t)i;
    }
    size = build_texture(copy, sizeof copy, PIXELICA_ASSET_MAX_TEXTURE_DIMENSION + 1u,
                         1, 0, many, PIXELICA_ASSET_MAX_TEXTURE_DIMENSION + 1u);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_CAPACITY));
    size = build_texture(copy, sizeof copy, PIXELICA_ASSET_MAX_TEXTURE_DIMENSION, 1,
                         0, many, PIXELICA_ASSET_MAX_TEXTURE_DIMENSION);
    CHECK(pixelica_texture_asset_memory_required(copy, size, &need) == PIXELICA_OK);

    /* Mesh and texture loaders refuse each other's files. */
    size = sample_mesh(copy, sizeof copy, 1);
    CHECK(texture_rejects(copy, size, PIXELICA_ERR_BAD_ASSET));
    size = build_texture(file, sizeof file, 3, 2, 0, texels, 6);
    CHECK(mesh_rejects(file, size, PIXELICA_ERR_BAD_ASSET));

    /* Truncation and bit flips. */
    for (i = 0; i < size; ++i) {
        memcpy(copy, file, i);
        if (!texture_rejects(copy, i, PIXELICA_OK)) {
            ++bad;
        }
        for (bit = 0; bit < 8; ++bit) {
            memcpy(copy, file, size);
            copy[i] = (uint8_t)(copy[i] ^ (1u << bit));
            if (!texture_rejects(copy, size, PIXELICA_OK)) {
                ++bad;
            }
        }
    }
    CHECK_EQ_LONG(bad, 0);
}

/* ---- carts ------------------------------------------------------------- */

typedef struct Entry {
    uint32_t id;
    uint32_t kind;
    const char *name;
    const uint8_t *file;
    size_t size;
} Entry;

static uint8_t g_entr[4 + 1100 * 48];
static size_t g_entr_len;
static uint8_t g_cdata[1100 * 64];
static size_t g_cdata_len;

static uint8_t *record(uint32_t i)
{
    return g_entr + 4u + 48u * (size_t)i;
}

/* ENTR and DATA payloads for `n` entries, tightly packed. */
static void cart_payloads(const Entry *e, uint32_t n)
{
    uint32_t i;
    size_t off = 0;

    memset(g_entr, 0, sizeof g_entr);
    wr32(g_entr, n);
    for (i = 0; i < n; ++i) {
        uint8_t *r = record(i);
        size_t len = strlen(e[i].name);
        wr32(r, e[i].id);
        wr32(r + 4, e[i].kind);
        memcpy(r + 8, e[i].name, len > 32u ? 32u : len);
        wr32(r + 40, (uint32_t)off);
        wr32(r + 44, (uint32_t)e[i].size);
        memcpy(g_cdata + off, e[i].file, e[i].size);
        off += e[i].size;
    }
    g_entr_len = 4u + 48u * (size_t)n;
    g_cdata_len = off;
}

static size_t cart_file(uint8_t *out, size_t cap)
{
    Chunk c[2];
    c[0] = chunk("ENTR", g_entr, g_entr_len);
    c[1] = chunk("DATA", g_cdata, g_cdata_len);
    return build_file(out, cap, PIXELICA_ASSET_KIND_CART, c, 2);
}

static uint8_t g_mesh_a[512], g_mesh_b[512], g_tex[256];
static size_t g_mesh_a_len, g_mesh_b_len, g_tex_len;

static void make_resources(void)
{
    static const uint16_t texels[4] = {10, 20, 30, 40};
    g_mesh_a_len = sample_mesh(g_mesh_a, sizeof g_mesh_a, 1);
    g_mesh_b_len = sample_mesh(g_mesh_b, sizeof g_mesh_b, 0);
    g_tex_len = build_texture(g_tex, sizeof g_tex, 2, 2, 0, texels, 4);
}

static uint32_t standard_entries(Entry *e)
{
    e[0].id = 1;
    e[0].kind = PIXELICA_ASSET_KIND_MESH;
    e[0].name = "desk";
    e[0].file = g_mesh_a;
    e[0].size = g_mesh_a_len;
    e[1].id = 2;
    e[1].kind = PIXELICA_ASSET_KIND_TEXTURE;
    e[1].name = "checker";
    e[1].file = g_tex;
    e[1].size = g_tex_len;
    e[2].id = 7;
    e[2].kind = PIXELICA_ASSET_KIND_MESH;
    e[2].name = "cup.v2-B_x";
    e[2].file = g_mesh_b;
    e[2].size = g_mesh_b_len;
    return 3;
}

static void test_cart_valid(void)
{
    static uint8_t file[4096];
    static uint8_t in_buf[4096 + 8];
    static unsigned char storage[512];
    Entry e[3];
    PIXELICA_Cart cart;
    PIXELICA_CartEntry ce;
    const void *data = NULL;
    size_t size, rsize = 0;
    uint32_t n = standard_entries(e);
    PIXELICA_MeshAsset mesh;
    PIXELICA_Texture tex;
    uint32_t i;

    cart_payloads(e, n);
    size = cart_file(file, sizeof file);
    memcpy(in_buf + 3, file, size); /* unaligned zero-copy view */
    CHECK(pixelica_cart_open(&cart, in_buf + 3, size) == PIXELICA_OK);
    CHECK_EQ_LONG(cart.count, 3);
    CHECK(cart.data == in_buf + 3 && cart.size == size);

    for (i = 0; i < n; ++i) {
        CHECK(pixelica_cart_find(&cart, e[i].id, &data, &rsize) == PIXELICA_OK);
        CHECK(rsize == e[i].size);
        CHECK(data != NULL && memcmp(data, e[i].file, e[i].size) == 0);
        CHECK((const uint8_t *)data > cart.data &&
              (const uint8_t *)data + rsize <= cart.data + cart.size);
        CHECK(pixelica_cart_entry(&cart, i, &ce) == PIXELICA_OK);
        CHECK(ce.id == e[i].id && ce.kind == e[i].kind);
        CHECK(strcmp(ce.name, e[i].name) == 0);
        CHECK(ce.data == data && ce.size == rsize);
    }
    CHECK(pixelica_cart_find(&cart, 1, &data, &rsize) == PIXELICA_OK);
    CHECK(pixelica_mesh_asset_load(&mesh, data, rsize, storage, sizeof storage) ==
          PIXELICA_OK);
    check_sample_decoded(&mesh, 1);
    CHECK(pixelica_cart_find(&cart, 2, &data, &rsize) == PIXELICA_OK);
    CHECK(pixelica_texture_asset_load(&tex, data, rsize, storage, sizeof storage) ==
          PIXELICA_OK);
    CHECK(tex.width == 2 && tex.height == 2 && tex.pixels != NULL &&
          tex.pixels[3] == 40);

    /* Lookups that find nothing. */
    data = file;
    rsize = 99;
    CHECK(pixelica_cart_find(&cart, 3, &data, &rsize) == PIXELICA_ERR_NOT_FOUND);
    CHECK(data == NULL && rsize == 0);
    CHECK(pixelica_cart_find(&cart, 0, &data, &rsize) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_cart_find(&cart, 8, &data, &rsize) == PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_cart_find(&cart, 0xFFFFFFFFu, &data, &rsize) ==
          PIXELICA_ERR_NOT_FOUND);
    CHECK(pixelica_cart_entry(&cart, 3, &ce) == PIXELICA_ERR_NOT_FOUND);
    CHECK(all_bytes(&ce, sizeof ce, 0));

    /* Invalid arguments. */
    CHECK(pixelica_cart_find(NULL, 1, &data, &rsize) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_cart_find(&cart, 1, NULL, &rsize) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_cart_find(&cart, 1, &data, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(data == NULL);
    CHECK(pixelica_cart_open(NULL, file, size) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_cart_open(&cart, NULL, size) == PIXELICA_ERR_INVALID_ARG);
    CHECK(all_bytes(&cart, sizeof cart, 0));
    CHECK(pixelica_cart_find(&cart, 1, &data, &rsize) == PIXELICA_ERR_INVALID_ARG);

    /* A cart view whose bytes were corrupted after open stays memory-safe:
     * the record's offset now points outside DATA. */
    memcpy(in_buf, file, size);
    CHECK(pixelica_cart_open(&cart, in_buf, size) == PIXELICA_OK);
    {
        size_t entr_off = rd32(in_buf + 16u + 4u);
        wr32(in_buf + entr_off + 4u + 48u + 40u, 0xFFFFFF00u);
    }
    CHECK(pixelica_cart_find(&cart, 2, &data, &rsize) == PIXELICA_ERR_BAD_ASSET);
    CHECK(data == NULL && rsize == 0);
    CHECK(pixelica_cart_entry(&cart, 1, &ce) == PIXELICA_ERR_BAD_ASSET);
}

static void test_cart_errors(void)
{
    static uint8_t file[16384];
    static uint8_t copy[16384];
    static uint8_t bad_mesh[512];
    static uint8_t big_tex[16384];
    static uint16_t big_texels[4097];
    Entry e[4];
    uint32_t n;
    size_t size, i;
    int bit;
    uint32_t bad = 0;

#define EXPECT_CART(want)                                  \
    do {                                                   \
        size = cart_file(file, sizeof file);               \
        CHECK(cart_rejects(file, size, (want)));           \
    } while (0)

    /* Sanity. */
    n = standard_entries(e);
    cart_payloads(e, n);
    size = cart_file(file, sizeof file);
    CHECK(cart_accepts(file, size));

    /* Ids: zero, unsorted, duplicate. */
    n = standard_entries(e);
    e[0].id = 0;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].id = 9;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].id = 1;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);

    /* Names: duplicate, empty, bad characters, 32 chars (no NUL),
     * non-zero padding after the NUL. 31 chars is the maximum. */
    n = standard_entries(e);
    e[2].name = "desk";
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].name = "";
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].name = "check er";
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].name = "assets/checker";
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].name = "0123456789abcdef0123456789ABCDEF";
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].name = "0123456789abcdef0123456789ABCDE";
    cart_payloads(e, n);
    size = cart_file(file, sizeof file);
    CHECK(cart_accepts(file, size));
    n = standard_entries(e);
    cart_payloads(e, n);
    record(1)[8 + 31] = 'x'; /* "checker\0...x" */
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    record(0)[8] = 0xC3; /* non-ASCII */
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);

    /* Kinds: unknown, cart-in-cart, mismatch with the nested magic. */
    n = standard_entries(e);
    e[0].kind = FOURCC("P3XX");
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[0].kind = PIXELICA_ASSET_KIND_CART;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    e[1].kind = PIXELICA_ASSET_KIND_MESH;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);

    /* Nested resource invalid (its own CRC), although the cart CRCs are
     * all correct. */
    memcpy(bad_mesh, g_mesh_a, g_mesh_a_len);
    bad_mesh[g_mesh_a_len - 1u] ^= 0x10u;
    n = standard_entries(e);
    e[0].file = bad_mesh;
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    /* Nested resource over a configured limit -> CAPACITY. */
    for (i = 0; i < 4097; ++i) {
        big_texels[i] = 1;
    }
    n = standard_entries(e);
    e[1].file = big_tex;
    e[1].size = build_texture(big_tex, sizeof big_tex,
                              PIXELICA_ASSET_MAX_TEXTURE_DIMENSION + 1u, 1, 0,
                              big_texels, PIXELICA_ASSET_MAX_TEXTURE_DIMENSION + 1u);
    cart_payloads(e, n);
    EXPECT_CART(PIXELICA_ERR_CAPACITY);

    /* Packing: gap, overlap, zero length, length past DATA, trailing DATA
     * bytes. */
    n = standard_entries(e);
    cart_payloads(e, n);
    wr32(record(1) + 40, rd32(record(1) + 40) + 1u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    wr32(record(1) + 40, rd32(record(1) + 40) - 1u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    wr32(record(0) + 44, 0u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    wr32(record(2) + 44, rd32(record(2) + 44) + 1u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    g_cdata[g_cdata_len++] = 0;
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);

    /* Directory shape: count 0, count/length mismatch, huge count. */
    n = standard_entries(e);
    cart_payloads(e, n);
    wr32(g_entr, 0u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    wr32(g_entr, 2u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    wr32(g_entr, 4u);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    wr32(g_entr, 0xFFFFFFFFu);
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    n = standard_entries(e);
    cart_payloads(e, n);
    g_entr_len -= 1u;
    EXPECT_CART(PIXELICA_ERR_BAD_ASSET);
    {
        Chunk c[3];
        n = standard_entries(e);
        cart_payloads(e, n);
        c[0] = chunk("ENTR", g_entr, g_entr_len);
        c[1] = chunk("DATA", g_cdata, g_cdata_len);
        size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_CART, c, 1);
        CHECK(cart_rejects(file, size, PIXELICA_ERR_BAD_ASSET)); /* no DATA */
        c[2] = chunk("UV00", g_cdata, 8);
        size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_CART, c, 3);
        CHECK(cart_rejects(file, size, PIXELICA_ERR_BAD_ASSET)); /* foreign */
        c[0] = chunk("DATA", g_cdata, g_cdata_len);
        c[1] = chunk("ENTR", g_entr, g_entr_len);
        size = build_file(file, sizeof file, PIXELICA_ASSET_KIND_CART, c, 2);
        CHECK(cart_accepts(file, size)); /* order is free */
        /* A mesh file is not a cart. */
        CHECK(cart_rejects(g_mesh_a, g_mesh_a_len, PIXELICA_ERR_BAD_ASSET));
    }

    /* Exhaustive truncation and single-bit flips of a valid cart. */
    n = standard_entries(e);
    cart_payloads(e, n);
    size = cart_file(file, sizeof file);
    for (i = 0; i < size; ++i) {
        memcpy(copy, file, i);
        if (!cart_rejects(copy, i, PIXELICA_OK)) {
            ++bad;
        }
        for (bit = 0; bit < 8; ++bit) {
            memcpy(copy, file, size);
            copy[i] = (uint8_t)(copy[i] ^ (1u << bit));
            if (!cart_rejects(copy, size, PIXELICA_OK)) {
                ++bad;
            }
        }
    }
    CHECK_EQ_LONG(bad, 0);
#undef EXPECT_CART
}

/* PIXELICA_CART_MAX_ENTRIES entries are accepted, one more is CAPACITY. */
static void test_cart_entry_limit(void)
{
    static uint8_t tiny[64];
    static char names[PIXELICA_CART_MAX_ENTRIES + 1u][8];
    static Entry e[PIXELICA_CART_MAX_ENTRIES + 1u];
    static uint8_t file[16 + 32 + sizeof g_entr + sizeof g_cdata];
    static const uint16_t texel[1] = {7};
    PIXELICA_Cart cart;
    const void *data = NULL;
    size_t tiny_len = build_texture(tiny, sizeof tiny, 1, 1, 0, texel, 1);
    size_t size, rsize = 0;
    uint32_t i;

    for (i = 0; i < PIXELICA_CART_MAX_ENTRIES + 1u; ++i) {
        names[i][0] = 't';
        names[i][1] = (char)('0' + (i / 1000u) % 10u);
        names[i][2] = (char)('0' + (i / 100u) % 10u);
        names[i][3] = (char)('0' + (i / 10u) % 10u);
        names[i][4] = (char)('0' + i % 10u);
        names[i][5] = '\0';
        e[i].id = i + 1u;
        e[i].kind = PIXELICA_ASSET_KIND_TEXTURE;
        e[i].name = names[i];
        e[i].file = tiny;
        e[i].size = tiny_len;
    }
    cart_payloads(e, PIXELICA_CART_MAX_ENTRIES);
    size = cart_file(file, sizeof file);
    CHECK(pixelica_cart_open(&cart, file, size) == PIXELICA_OK);
    CHECK_EQ_LONG(cart.count, PIXELICA_CART_MAX_ENTRIES);
    CHECK(pixelica_cart_find(&cart, PIXELICA_CART_MAX_ENTRIES, &data, &rsize) == PIXELICA_OK);
    CHECK(rsize == tiny_len && memcmp(data, tiny, tiny_len) == 0);
    CHECK(pixelica_cart_find(&cart, PIXELICA_CART_MAX_ENTRIES / 2u, &data, &rsize) ==
          PIXELICA_OK);
    cart_payloads(e, PIXELICA_CART_MAX_ENTRIES + 1u);
    size = cart_file(file, sizeof file);
    CHECK(cart_rejects(file, size, PIXELICA_ERR_CAPACITY));
}

int main(void)
{
    encode_sample();
    make_resources();
    test_crc32_reference();
    test_mesh_load_valid();
    test_mesh_args_and_overlap();
    test_mesh_container_errors();
    test_mesh_geometry_errors();
    test_mesh_truncation_and_bitflips();
    test_mesh_limits();
    test_mesh_draw_integration();
    test_texture_assets();
    test_cart_valid();
    test_cart_errors();
    test_cart_entry_limit();
    return pixelica_t_finish("test_assets");
}
