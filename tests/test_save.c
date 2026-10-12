/*
 * Pixelica - save container tests: layout, roundtrip, strict rejection,
 * unchanged outputs on failure, overlap, and file replace/read.
 *
 * File tests create and remove only their own uniquely named files in the
 * working directory.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen of this test's own files */
#endif

#include <stdlib.h>
#include <time.h>

#include "pixelica/save.h"
#include "test_util.h"

#define T_FILL 0x5Au

/* Independent bitwise CRC-32 (IEEE) to pin the trailer algorithm. */
static uint32_t t_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < n; ++i) {
        crc ^= (uint32_t)p[i];
        for (k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t t_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void t_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

/* Recomputes the trailer so only the targeted field is "wrong". */
static void t_reseal(uint8_t *p, size_t n)
{
    t_wr32(p + n - 4u, t_crc32(p, n - 4u));
}

static int t_all(const uint8_t *p, size_t n, uint8_t v)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

static void t_pattern(uint8_t *p, size_t n, uint32_t seed)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        p[i] = (uint8_t)((i * 31u + seed) & 0xFFu);
    }
}

/* Decode must fail with `want` and leave every output untouched. */
static void t_decode_rejects(const uint8_t *data, size_t size, size_t cap,
                             PIXELICA_Status want)
{
    uint8_t dst[512];
    uint32_t schema = 0xDEADBEEFu;
    size_t out_size = 12345u;

    memset(dst, T_FILL, sizeof dst);
    CHECK_EQ_LONG(pixelica_save_decode(data, size, &schema, dst, cap, &out_size), want);
    CHECK(t_all(dst, sizeof dst, T_FILL));
    CHECK(schema == 0xDEADBEEFu);
    CHECK_EQ_LONG(out_size, 12345);
}

static void test_layout_and_empty(void)
{
    static const uint8_t check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    static const uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    static const uint8_t want[19] = { 'P', 'X', 'S', 'V', 1, 0, 0, 0, 0x04, 0x03,
                                      0x02, 0x01, 3, 0, 0, 0, 0xAA, 0xBB, 0xCC };
    uint8_t buf[64];
    size_t n = 0;
    size_t len = 99;
    uint32_t schema = 0;

    CHECK(t_crc32(check, sizeof check) == 0xCBF43926u);

    CHECK_EQ_LONG(pixelica_save_encode(0x01020304u, payload, 3, buf, sizeof buf, &n),
                  PIXELICA_OK);
    CHECK_EQ_LONG(n, 23);
    CHECK(memcmp(buf, want, sizeof want) == 0);
    CHECK(t_rd32(buf + 19) == t_crc32(buf, 19));

    /* Empty payload: NULL/0 in, NULL/0 out. */
    CHECK_EQ_LONG(pixelica_save_encode(7u, NULL, 0, buf, sizeof buf, &n), PIXELICA_OK);
    CHECK_EQ_LONG(n, PIXELICA_SAVE_OVERHEAD_BYTES);
    CHECK_EQ_LONG(pixelica_save_decode(buf, n, &schema, NULL, 0, &len), PIXELICA_OK);
    CHECK_EQ_LONG(len, 0);
    CHECK_EQ_LONG(schema, 7);
}

static void test_roundtrip_and_version(void)
{
    uint8_t payload[300];
    uint8_t enc[400];
    uint8_t dst[300];
    size_t n = 0;
    size_t len = 0;
    uint32_t schema = 0;

    t_pattern(payload, sizeof payload, 3u);
    CHECK_EQ_LONG(pixelica_save_encode(0xFFFFFFFFu, payload, sizeof payload, enc,
                                       sizeof enc, &n), PIXELICA_OK);
    CHECK_EQ_LONG(n, sizeof payload + PIXELICA_SAVE_OVERHEAD_BYTES);

    CHECK_EQ_LONG(pixelica_save_inspect(enc, n, &schema, &len), PIXELICA_OK);
    CHECK(schema == 0xFFFFFFFFu);
    CHECK_EQ_LONG(len, sizeof payload);
    CHECK_EQ_LONG(pixelica_save_inspect(enc, n, NULL, NULL), PIXELICA_OK);

    schema = 0;
    len = 0;
    CHECK_EQ_LONG(pixelica_save_decode(enc, n, &schema, dst, sizeof dst, &len),
                  PIXELICA_OK);
    CHECK(schema == 0xFFFFFFFFu);
    CHECK_EQ_LONG(len, sizeof payload);
    CHECK(memcmp(dst, payload, sizeof payload) == 0);
    CHECK_EQ_LONG(pixelica_save_decode(enc, n, NULL, dst, sizeof dst, &len), PIXELICA_OK);

    /* Exact capacity works, one less is NO_MEMORY with nothing written. */
    t_decode_rejects(enc, n, sizeof payload - 1u, PIXELICA_ERR_NO_MEMORY);
}

static void test_encode_errors(void)
{
    uint8_t payload[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    uint8_t out[64];
    size_t n = 777u;

    memset(out, T_FILL, sizeof out);
    CHECK_EQ_LONG(pixelica_save_encode(1u, payload, sizeof payload, out, 29, &n),
                  PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(pixelica_save_encode(1u, payload, sizeof payload, NULL, 0, &n),
                  PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(pixelica_save_encode(1u, NULL, 1, out, sizeof out, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_encode(1u, payload, 1, NULL, 5, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_encode(1u, payload, 1, out, sizeof out, NULL),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_encode(1u, payload,
                                       (size_t)PIXELICA_SAVE_MAX_PAYLOAD_BYTES + 1u,
                                       out, sizeof out, &n),
                  PIXELICA_ERR_CAPACITY);
    CHECK(t_all(out, sizeof out, T_FILL));
    CHECK_EQ_LONG(n, 777);

    CHECK_EQ_LONG(pixelica_save_encoded_size(PIXELICA_SAVE_MAX_PAYLOAD_BYTES, &n),
                  PIXELICA_OK);
    CHECK(n == 0xFFFFFFFFu);
    CHECK_EQ_LONG(pixelica_save_encoded_size((size_t)PIXELICA_SAVE_MAX_PAYLOAD_BYTES + 1u,
                                             &n), PIXELICA_ERR_CAPACITY);
    CHECK_EQ_LONG(pixelica_save_encoded_size(0, NULL), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_save_decode(NULL, 20, NULL, out, sizeof out, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_decode(out, 20, NULL, NULL, 4, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_decode(out, 20, NULL, out, sizeof out, NULL),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_inspect(NULL, 20, NULL, NULL), PIXELICA_ERR_INVALID_ARG);
}

static void test_corruption(void)
{
    uint8_t payload[40];
    uint8_t enc[64];
    uint8_t bad[128];
    size_t n = 0;
    size_t i;
    int bit;

    t_pattern(payload, sizeof payload, 11u);
    CHECK_EQ_LONG(pixelica_save_encode(5u, payload, sizeof payload, enc, sizeof enc, &n),
                  PIXELICA_OK);

    /* Every single-bit flip anywhere (header, payload, CRC) is rejected. */
    for (i = 0; i < n; ++i) {
        for (bit = 0; bit < 8; ++bit) {
            memcpy(bad, enc, n);
            bad[i] = (uint8_t)(bad[i] ^ (1u << bit));
            t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);
        }
    }

    /* Every truncation, and trailing bytes. */
    for (i = 0; i < n; ++i) {
        t_decode_rejects(enc, i, sizeof payload, PIXELICA_ERR_BAD_ASSET);
    }
    memcpy(bad, enc, n);
    bad[n] = 0;
    t_decode_rejects(bad, n + 1u, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    /* Header fields that are wrong even with a matching CRC. */
    memcpy(bad, enc, n);
    bad[4] = 2; /* unknown container format */
    t_reseal(bad, n);
    t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    memcpy(bad, enc, n);
    bad[6] = 1; /* reserved */
    t_reseal(bad, n);
    t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    memcpy(bad, enc, n);
    bad[0] = 'Q'; /* magic */
    t_reseal(bad, n);
    t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    memcpy(bad, enc, n);
    t_wr32(bad + 12, 39u); /* length one short of the real payload */
    t_reseal(bad, n);
    t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    memcpy(bad, enc, n);
    t_wr32(bad + 12, 0xFFFFFFFFu); /* oversize claim */
    t_reseal(bad, n);
    t_decode_rejects(bad, n, sizeof payload, PIXELICA_ERR_BAD_ASSET);

    CHECK_EQ_LONG(pixelica_save_inspect(bad, n, NULL, NULL), PIXELICA_ERR_BAD_ASSET);
}

static void test_overlap(void)
{
    uint8_t payload[50];
    uint8_t buf[128];
    size_t offsets[4] = { 0, 5, 16, 40 };
    size_t k;
    size_t n;
    size_t len;
    uint32_t schema;

    t_pattern(payload, sizeof payload, 77u);
    for (k = 0; k < 4; ++k) {
        memset(buf, 0, sizeof buf);
        memcpy(buf + offsets[k], payload, sizeof payload);
        n = 0;
        CHECK_EQ_LONG(pixelica_save_encode(9u, buf + offsets[k], sizeof payload, buf,
                                           sizeof buf, &n), PIXELICA_OK);
        CHECK_EQ_LONG(pixelica_save_inspect(buf, n, NULL, NULL), PIXELICA_OK);

        /* Decode in place: payload moves down to the start of the block. */
        len = 0;
        schema = 0;
        CHECK_EQ_LONG(pixelica_save_decode(buf, n, &schema, buf, sizeof buf, &len),
                      PIXELICA_OK);
        CHECK_EQ_LONG(len, sizeof payload);
        CHECK_EQ_LONG(schema, 9);
        CHECK(memcmp(buf, payload, sizeof payload) == 0);
    }
}

/* ---- files ------------------------------------------------------------- */

static int t_file_put(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL) {
        return 0;
    }
    ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static size_t t_file_get(const char *path, uint8_t *p, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        return 0;
    }
    n = fread(p, 1, cap, f);
    (void)fclose(f);
    return n;
}

static int t_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return 0;
    }
    (void)fclose(f);
    return 1;
}

/* File read must fail with `want` and leave every output untouched. */
static void t_file_rejects(const char *path, size_t cap, PIXELICA_Status want)
{
    uint8_t dst[256];
    uint32_t schema = 0xDEADBEEFu;
    size_t len = 12345u;

    memset(dst, T_FILL, sizeof dst);
    CHECK_EQ_LONG(pixelica_save_file_read(path, &schema, dst, cap, &len), want);
    CHECK(t_all(dst, sizeof dst, T_FILL));
    CHECK(schema == 0xDEADBEEFu);
    CHECK_EQ_LONG(len, 12345);
}

static void test_files(void)
{
    char path[128];
    char tmp[136];
    char nodir[192];
    uint8_t a[100];
    uint8_t b[37];
    uint8_t raw[256];
    uint8_t dst[256];
    size_t n;
    size_t len = 0;
    uint32_t schema = 0;
    int local = 0;

    (void)snprintf(path, sizeof path, "pixelica_test_save_%lu_%lu_%lu.bin",
                   (unsigned long)time(NULL), (unsigned long)clock(),
                   (unsigned long)((uintptr_t)&local & 0xFFFFFFFFu));
    (void)snprintf(tmp, sizeof tmp, "%s.tmp", path);
    (void)snprintf(nodir, sizeof nodir, "%s.missing_dir/save.bin", path);
    t_pattern(a, sizeof a, 1u);
    t_pattern(b, sizeof b, 2u);

    t_file_rejects(path, sizeof dst, PIXELICA_ERR_NOT_FOUND);

    /* Create, then replace an existing file with a different size. */
    CHECK_EQ_LONG(pixelica_save_file_write(path, 1u, a, sizeof a), PIXELICA_OK);
    CHECK(!t_exists(tmp));
    CHECK_EQ_LONG(pixelica_save_file_read(path, &schema, dst, sizeof dst, &len),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 1);
    CHECK_EQ_LONG(len, sizeof a);
    CHECK(memcmp(dst, a, sizeof a) == 0);

    CHECK_EQ_LONG(pixelica_save_file_write(path, 2u, b, sizeof b), PIXELICA_OK);
    CHECK(!t_exists(tmp));
    CHECK_EQ_LONG(pixelica_save_file_read(path, &schema, dst, sizeof dst, &len),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 2);
    CHECK_EQ_LONG(len, sizeof b);
    CHECK(memcmp(dst, b, sizeof b) == 0);
    n = t_file_get(path, raw, sizeof raw);
    CHECK_EQ_LONG(n, sizeof b + PIXELICA_SAVE_OVERHEAD_BYTES);

    /* Failed reads leave the destination untouched. */
    CHECK(t_file_put(tmp, a, sizeof a));
    CHECK_EQ_LONG(pixelica_save_file_write(path, 9u, a, sizeof a), PIXELICA_ERR_PLATFORM);
    CHECK_EQ_LONG(t_file_get(tmp, dst, sizeof dst), sizeof a);
    CHECK(memcmp(dst, a, sizeof a) == 0);
    CHECK_EQ_LONG(pixelica_save_file_read(path, &schema, dst, sizeof dst, &len), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 2);
    CHECK_EQ_LONG(len, sizeof b);
    CHECK(memcmp(dst, b, sizeof b) == 0);
    CHECK(remove(tmp) == 0);
    t_file_rejects(path, sizeof b - 1u, PIXELICA_ERR_NO_MEMORY);

    raw[20] = (uint8_t)(raw[20] ^ 0x10u);
    CHECK(t_file_put(path, raw, n));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_BAD_ASSET);
    raw[20] = (uint8_t)(raw[20] ^ 0x10u);

    CHECK(t_file_put(path, raw, n - 1u));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_BAD_ASSET);
    CHECK(t_file_put(path, raw, 10));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_BAD_ASSET);
    CHECK(t_file_put(path, raw, 0));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_BAD_ASSET);

    raw[n] = 0;
    CHECK(t_file_put(path, raw, n + 1u));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_BAD_ASSET);

    t_wr32(raw + 12, 0xFFFFFFF0u); /* header claims a too-large file */
    CHECK(t_file_put(path, raw, n));
    t_file_rejects(path, sizeof dst, PIXELICA_ERR_CAPACITY);

    /* Empty payload file. */
    CHECK_EQ_LONG(pixelica_save_file_write(path, 3u, NULL, 0), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_save_file_read(path, &schema, NULL, 0, &len), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 3);
    CHECK_EQ_LONG(len, 0);

    /* A write that cannot create its temp file fails and creates nothing. */
    CHECK_EQ_LONG(pixelica_save_file_write(nodir, 1u, a, sizeof a), PIXELICA_ERR_PLATFORM);
    CHECK(!t_exists(nodir));

    CHECK_EQ_LONG(pixelica_save_file_write(NULL, 1u, a, 1), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_file_write("", 1u, a, 1), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_file_write(path, 1u, NULL, 1), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_file_read(path, NULL, NULL, 1, &len),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_save_file_read(path, NULL, dst, sizeof dst, NULL),
                  PIXELICA_ERR_INVALID_ARG);

    (void)remove(path);
    (void)remove(tmp);
    CHECK(!t_exists(path));
}

int main(void)
{
    test_layout_and_empty();
    test_roundtrip_and_version();
    test_encode_errors();
    test_corruption();
    test_overlap();
    test_files();
    return pixelica_t_finish("test_save");
}
