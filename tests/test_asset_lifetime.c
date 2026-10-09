/*
 * Pixelica - .p3tex loader lifetime regression. The loader must not keep
 * pointers into the temporary container view of its inspector (ASan
 * stack-use-after-return). Each case loads an independently encoded texture
 * (own writer and bitwise CRC-32, none of the library's code) from unaligned
 * input into unaligned caller storage, then checks every texel, the storage
 * guard bytes, independence from the input and repeated loads.
 */
#include "test_util.h"

#include "pixelica/assets.h"

#define GUARD 32u
#define MAX_TEXELS (64u * 37u)
#define MAX_FILE (48u + 8u + 2u * MAX_TEXELS)
#define GUARD_BYTE 0xA5u
#define FILL_BYTE 0x5Au

/* Independent bitwise CRC-32 (IEEE 802.3, reflected). */
static uint32_t ref_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;

    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

static uint16_t texel_value(uint32_t seed, uint32_t i)
{
    return (uint16_t)((i * 40503u + seed * 977u + 0x1234u) ^ (i >> 3));
}

/* Writes a complete file; returns its size. */
static size_t encode_texture(uint8_t *f, uint32_t w, uint32_t h, uint32_t seed)
{
    const uint32_t n = w * h;
    const uint32_t data_len = 2u * n;
    const uint32_t size = 48u + 8u + data_len;
    uint32_t i;

    memset(f, 0, size);
    memcpy(f, "P3TX", 4);
    put16(f + 4, 1u);
    put16(f + 6, 48u);
    put32(f + 8, size);
    put32(f + 12, 2u);
    memcpy(f + 16, "INFO", 4);
    put32(f + 20, 48u);
    put32(f + 24, 8u);
    memcpy(f + 32, "DATA", 4);
    put32(f + 36, 56u);
    put32(f + 40, data_len);
    put16(f + 48, w);
    put16(f + 50, h);
    put32(f + 52, 0u);
    for (i = 0; i < n; ++i) {
        put16(f + 56u + 2u * i, texel_value(seed, i));
    }
    put32(f + 28, ref_crc32(f + 48, 8u));
    put32(f + 44, ref_crc32(f + 56, data_len));
    return size;
}

static int all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

static int texels_match(const PIXELICA_Texture *t, uint32_t w, uint32_t h,
                        uint32_t seed)
{
    uint32_t i;

    if (t->pixels == NULL || t->width != w || t->height != h) {
        return 0;
    }
    for (i = 0; i < w * h; ++i) {
        if (t->pixels[i] != texel_value(seed, i)) {
            return 0;
        }
    }
    return 1;
}

static int tex_is_zero(const PIXELICA_Texture *t)
{
    return t->pixels == NULL && t->width == 0 && t->height == 0;
}

static uint8_t g_file[MAX_FILE];
static uint8_t g_in[MAX_FILE + 16u];
static uint8_t g_store[GUARD + MAX_TEXELS * 2u + 16u + 16u + GUARD];

static void run_case(uint32_t w, uint32_t h, uint32_t seed, size_t in_off,
                     size_t st_off)
{
    const size_t fsize = encode_texture(g_file, w, h, seed);
    const size_t texel_bytes = (size_t)w * h * 2u;
    const size_t region_len = texel_bytes + 15u;
    const size_t total = GUARD + st_off + region_len + GUARD;
    uint8_t *const in = g_in + in_off;
    uint8_t *const region = g_store + GUARD + st_off;
    const size_t pad = (size_t)((16u - ((uintptr_t)region % 16u)) % 16u);
    PIXELICA_Texture tex;
    size_t req = 0;
    int rep;

    memcpy(in, g_file, fsize);
    CHECK(pixelica_texture_asset_memory_required(in, fsize, &req) == PIXELICA_OK);
    CHECK_EQ_LONG(req, region_len);

    /* Repeated loads into exactly-sized, freshly guarded storage. */
    for (rep = 0; rep < 3; ++rep) {
        memset(g_store, GUARD_BYTE, total);
        memset(region, FILL_BYTE, region_len);
        memcpy(in, g_file, fsize);
        memset(&tex, 0xFF, sizeof tex);

        CHECK(pixelica_texture_asset_load(&tex, in, fsize, region, region_len) ==
              PIXELICA_OK);
        CHECK((const uint8_t *)tex.pixels == region + pad);
        CHECK(((uintptr_t)tex.pixels % 16u) == 0u);
        CHECK(texels_match(&tex, w, h, seed));
        CHECK(all_bytes(g_store, GUARD + st_off, GUARD_BYTE));
        CHECK(all_bytes(region + region_len, GUARD, GUARD_BYTE));
        CHECK(all_bytes(region, pad, FILL_BYTE));
        CHECK(all_bytes(region + pad + texel_bytes, region_len - pad -
                                                        texel_bytes,
                        FILL_BYTE));

        /* The texture must not reference the input file. */
        memset(in, 0xEE, fsize);
        CHECK(texels_match(&tex, w, h, seed));
    }

    /* Rejections never touch storage and zero the output. */
    memcpy(in, g_file, fsize);
    memset(g_store, GUARD_BYTE, total);
    memset(region, FILL_BYTE, region_len);

    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, in, fsize, region, region_len - 1u) ==
          PIXELICA_ERR_NO_MEMORY);
    CHECK(tex_is_zero(&tex));
    CHECK(all_bytes(region, region_len, FILL_BYTE));

    in[fsize - 1u] ^= 0x01u; /* DATA CRC mismatch */
    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, in, fsize, region, region_len) ==
          PIXELICA_ERR_BAD_ASSET);
    CHECK(tex_is_zero(&tex));
    CHECK(all_bytes(region, region_len, FILL_BYTE));
    in[fsize - 1u] ^= 0x01u;

    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, in, fsize - 1u, region, region_len) ==
          PIXELICA_ERR_BAD_ASSET);
    CHECK(tex_is_zero(&tex));
    CHECK(all_bytes(region, region_len, FILL_BYTE));

    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, in, fsize, in + 1, region_len) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(tex_is_zero(&tex));

    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, NULL, fsize, region, region_len) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(tex_is_zero(&tex));

    memset(&tex, 0xFF, sizeof tex);
    CHECK(pixelica_texture_asset_load(&tex, in, fsize, NULL, region_len) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(tex_is_zero(&tex));

    CHECK(pixelica_texture_asset_load(NULL, in, fsize, region, region_len) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(all_bytes(region, region_len, FILL_BYTE));
    CHECK(all_bytes(g_store, GUARD + st_off, GUARD_BYTE));
    CHECK(all_bytes(region + region_len, GUARD, GUARD_BYTE));
}

int main(void)
{
    size_t in_off, st_off;

    /* Every input/storage misalignment mod 16 (storage) and 4 (input). */
    for (st_off = 0; st_off < 16u; ++st_off) {
        run_case(5u, 3u, 1u, st_off & 3u, st_off);
    }
    for (in_off = 0; in_off < 4u; ++in_off) {
        run_case(1u, 1u, 2u, in_off, in_off * 5u);
        run_case(7u, 1u, 3u, in_off, 3u + in_off);
        run_case(1u, 9u, 4u, in_off, 11u + in_off);
        run_case(64u, 37u, 5u + (uint32_t)in_off, in_off, 15u - in_off);
    }
    return pixelica_t_finish("test_asset_lifetime");
}
