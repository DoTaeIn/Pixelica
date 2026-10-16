/*
 * Pixelica - tests for pixelica/storage.h: game id rules (including path
 * traversal), slot range, the save envelope reuse, platform callback failures
 * and the PC filesystem adapter.
 *
 * The mock platform table can be told to fail any callback, so the error
 * paths are covered without needing an unwritable filesystem.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen/remove of this test's own files */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pixelica/storage.h"
#include "test_util.h"

/* ---- mock blob store ---------------------------------------------------- */

#define MOCK_SLOTS PIXELICA_STORAGE_SLOT_MAX
#define MOCK_BYTES 256u

typedef struct Mock {
    uint8_t data[MOCK_SLOTS][MOCK_BYTES];
    size_t size[MOCK_SLOTS]; /* 0 = empty slot */
    char last_id[PIXELICA_STORAGE_GAME_ID_MAX + 1u];
    uint32_t last_slot;
    uint32_t writes;
    uint32_t reads;
    PIXELICA_Status write_fail; /* OK = behave normally */
    PIXELICA_Status read_fail;
    PIXELICA_Status erase_fail;
    bool lie_about_size; /* report more bytes than were copied */
} Mock;

static void mock_note(Mock *m, const char *game_id, uint32_t slot)
{
    size_t n = strlen(game_id);

    memset(m->last_id, 0, sizeof m->last_id);
    if (n <= PIXELICA_STORAGE_GAME_ID_MAX) {
        memcpy(m->last_id, game_id, n);
    }
    m->last_slot = slot;
}

static PIXELICA_Status mock_write(void *userdata, const char *game_id, uint32_t slot,
                                  const void *data, size_t size)
{
    Mock *m = (Mock *)userdata;

    mock_note(m, game_id, slot);
    ++m->writes;
    if (m->write_fail != PIXELICA_OK) {
        return m->write_fail; /* the previous content stays untouched */
    }
    if (size > MOCK_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    memcpy(m->data[slot], data, size);
    m->size[slot] = size;
    return PIXELICA_OK;
}

static PIXELICA_Status mock_read(void *userdata, const char *game_id, uint32_t slot,
                                 void *out, size_t capacity, size_t *out_size)
{
    Mock *m = (Mock *)userdata;

    mock_note(m, game_id, slot);
    ++m->reads;
    if (m->read_fail != PIXELICA_OK) {
        return m->read_fail;
    }
    if (m->size[slot] == 0u) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    *out_size = m->lie_about_size ? capacity + 1u : m->size[slot];
    if (m->size[slot] > capacity) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    memcpy(out, m->data[slot], m->size[slot]);
    return PIXELICA_OK;
}

static PIXELICA_Status mock_erase(void *userdata, const char *game_id, uint32_t slot)
{
    Mock *m = (Mock *)userdata;

    mock_note(m, game_id, slot);
    if (m->erase_fail != PIXELICA_OK) {
        return m->erase_fail;
    }
    if (m->size[slot] == 0u) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    m->size[slot] = 0u;
    return PIXELICA_OK;
}

static PIXELICA_Status mock_size(void *userdata, const char *game_id, uint32_t slot,
                                 size_t *out_size)
{
    Mock *m = (Mock *)userdata;

    mock_note(m, game_id, slot);
    if (m->size[slot] == 0u) {
        return PIXELICA_ERR_NOT_FOUND;
    }
    *out_size = m->size[slot];
    return PIXELICA_OK;
}

static void mock_init(Mock *m, PIXELICA_StorageOps *ops)
{
    memset(m, 0, sizeof *m);
    memset(ops, 0, sizeof *ops);
    ops->userdata = m;
    ops->write = mock_write;
    ops->read = mock_read;
    ops->erase = mock_erase;
    ops->size = mock_size;
}

/* ---- game id ------------------------------------------------------------ */

static void test_game_id(void)
{
    static const char *const good[] = { "a",
                                        "pixel-room",
                                        "hello_world",
                                        "game2",
                                        "0",
                                        "abcdefghijklmnopqrstuvwxyz012345" /* 32 */ };
    static const char *const bad[] = {
        "",                                  /* empty */
        "..",                                /* parent directory */
        "../../etc/passwd",                  /* classic traversal */
        "..\\..\\windows",                   /* the Windows spelling */
        "a/b",             "a\\b",   "/abs",     "C:",  "c:game",
        ".hidden",         "game.sav",                /* '.' is not allowed at all */
        "Game",            "GAME",                    /* upper case */
        "my game",         "tab\there",               /* whitespace */
        "-lead",           "_lead",  "trail-",   "trail_",
        "con",             "nul",    "com1",     "lpt9", /* Windows devices */
        "abcdefghijklmnopqrstuvwxyz0123456"             /* 33 bytes */
    };
    size_t i;

    for (i = 0; i < sizeof good / sizeof good[0]; ++i) {
        CHECK(pixelica_storage_game_id_valid(good[i]));
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        CHECK(!pixelica_storage_game_id_valid(bad[i]));
    }
    CHECK(!pixelica_storage_game_id_valid(NULL));
    /* "com" and "lpt" without a digit are ordinary names. */
    CHECK(pixelica_storage_game_id_valid("com"));
    CHECK(pixelica_storage_game_id_valid("lpt"));
    CHECK(pixelica_storage_game_id_valid("console"));
}

/* ---- init / shutdown ---------------------------------------------------- */

static void test_init(void)
{
    PIXELICA_Storage s;
    PIXELICA_StorageOps ops;
    PIXELICA_StorageOps broken;
    Mock mock;
    uint8_t scratch[64];
    size_t bytes = 123u;

    mock_init(&mock, &ops);
    memset(&s, 0, sizeof s);

    CHECK_EQ_LONG(pixelica_storage_init(NULL, "game", &ops, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_init(&s, NULL, &ops, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", NULL, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "../evil", &ops, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &ops, NULL, 8u),
                  PIXELICA_ERR_INVALID_ARG);
    /* write and read are required. */
    broken = ops;
    broken.write = NULL;
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &broken, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    broken = ops;
    broken.read = NULL;
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &broken, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    /* Nothing above changed the storage: it is still unusable. */
    CHECK(pixelica_storage_game_id(&s) == NULL);
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);
    CHECK(pixelica_storage_game_id(&s) != NULL &&
          strcmp(pixelica_storage_game_id(&s), "game") == 0);
    /* A live storage is not re-initialised by accident. */
    CHECK_EQ_LONG(pixelica_storage_init(&s, "other", &ops, scratch, sizeof scratch),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK(strcmp(pixelica_storage_game_id(&s), "game") == 0);

    CHECK_EQ_LONG(pixelica_storage_max_payload(&s, &bytes), PIXELICA_OK);
    CHECK_EQ_LONG(bytes, (long long)(sizeof scratch - PIXELICA_SAVE_OVERHEAD_BYTES));
    CHECK_EQ_LONG(pixelica_storage_required_scratch(10u, &bytes), PIXELICA_OK);
    CHECK_EQ_LONG(bytes, 10 + PIXELICA_SAVE_OVERHEAD_BYTES);

    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_storage_game_id(&s) == NULL);
    CHECK_EQ_LONG(pixelica_storage_max_payload(&s, &bytes), PIXELICA_ERR_INVALID_ARG);
    /* It can be init-ed again. */
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);

    /* A scratch-less storage still answers the metadata questions. */
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "game", &ops, NULL, 0u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_max_payload(&s, &bytes), PIXELICA_OK);
    CHECK_EQ_LONG(bytes, 0);
}

/* ---- save / load round trip --------------------------------------------- */

static void test_round_trip(void)
{
    PIXELICA_Storage s;
    PIXELICA_StorageOps ops;
    Mock mock;
    uint8_t scratch[MOCK_BYTES];
    const uint8_t payload[6] = { 1u, 2u, 3u, 250u, 251u, 252u };
    uint8_t out[16];
    size_t n = 0;
    uint32_t schema = 0;
    bool exists = true;

    mock_init(&mock, &ops);
    memset(&s, 0, sizeof s);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "round-trip", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);

    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, &exists), PIXELICA_OK);
    CHECK(!exists);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 0u, NULL, NULL), PIXELICA_ERR_NOT_FOUND);

    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 7u, payload, sizeof payload),
                  PIXELICA_OK);
    /* The stored blob is the save envelope: magic, size and CRC included. */
    CHECK_EQ_LONG(mock.size[0], (long long)(sizeof payload + PIXELICA_SAVE_OVERHEAD_BYTES));
    CHECK(memcmp(mock.data[0], "PXSV", 4u) == 0);
    CHECK(strcmp(mock.last_id, "round-trip") == 0);

    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, &exists), PIXELICA_OK);
    CHECK(exists);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 0u, &schema, &n), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 7);
    CHECK_EQ_LONG(n, (long long)sizeof payload);

    memset(out, 0xAAu, sizeof out);
    schema = 0;
    n = 0;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 7);
    CHECK_EQ_LONG(n, (long long)sizeof payload);
    CHECK(memcmp(out, payload, sizeof payload) == 0);
    CHECK_EQ_LONG(out[sizeof payload], 0xAA); /* nothing beyond the payload */

    /* Slots are independent. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 3u, 99u, payload, 2u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 3u, &schema, &n), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 99);
    CHECK_EQ_LONG(n, 2);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 0u, &schema, &n), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 7);

    /* An empty payload is a valid save. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 1u, 0u, NULL, 0u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 1u, &schema, NULL, 0u, &n), PIXELICA_OK);
    CHECK_EQ_LONG(n, 0);

    /* Overwrite keeps only the new content. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 8u, payload, 3u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 8);
    CHECK_EQ_LONG(n, 3);

    CHECK_EQ_LONG(pixelica_storage_erase(&s, 0u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_erase(&s, 0u), PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, &exists), PIXELICA_OK);
    CHECK(!exists);
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
}

/* ---- invalid arguments and capacity ------------------------------------- */

static void test_limits(void)
{
    PIXELICA_Storage s;
    PIXELICA_StorageOps ops;
    PIXELICA_StorageOps no_erase;
    PIXELICA_Storage small;
    Mock mock;
    uint8_t scratch[32];
    uint8_t tiny[PIXELICA_SAVE_OVERHEAD_BYTES + 4u];
    uint8_t payload[32];
    uint8_t out[32];
    size_t n = 0;
    bool exists = false;

    mock_init(&mock, &ops);
    memset(payload, 0x5Au, sizeof payload);
    memset(&s, 0, sizeof s);
    memset(&small, 0, sizeof small);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "limits", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);

    /* Slot range. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, PIXELICA_STORAGE_SLOT_MAX, 1u, payload, 1u),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_load(&s, PIXELICA_STORAGE_SLOT_MAX, NULL, out,
                                        sizeof out, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_erase(&s, 9999u), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 9999u, &exists), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(mock.writes, 0);

    /* NULL outputs. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 1u, NULL, 4u), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, NULL, out, sizeof out, NULL),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, NULL, NULL, 4u, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, NULL), PIXELICA_ERR_INVALID_ARG);

    /* A payload that does not fit the scratch buffer never reaches the
     * platform, and nothing stored changes. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 1u, payload, sizeof payload),
                  PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(mock.writes, 0);

    /* A payload larger than the caller's buffer on the way back. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 1u, payload, 8u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, NULL, out, 4u, &n),
                  PIXELICA_ERR_NO_MEMORY);
    /* ... and one larger than the scratch buffer of another storage. */
    CHECK_EQ_LONG(pixelica_storage_init(&small, "limits", &ops, tiny, sizeof tiny),
                  PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_load(&small, 0u, NULL, out, sizeof out, &n),
                  PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(pixelica_storage_shutdown(&small), PIXELICA_OK);

    /* An optional callback that is not there. */
    no_erase = ops;
    no_erase.erase = NULL;
    no_erase.size = NULL;
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "limits", &no_erase, scratch, sizeof scratch),
                  PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_erase(&s, 0u), PIXELICA_ERR_INVALID_ARG);
    /* exists falls back to a zero-capacity read. */
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, &exists), PIXELICA_OK);
    CHECK(exists);
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 2u, &exists), PIXELICA_OK);
    CHECK(!exists);
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
}

/* ---- platform failures and corruption ----------------------------------- */

static void test_io_failures(void)
{
    PIXELICA_Storage s;
    PIXELICA_StorageOps ops;
    Mock mock;
    uint8_t scratch[MOCK_BYTES];
    const uint8_t payload[4] = { 9u, 8u, 7u, 6u };
    uint8_t out[16];
    size_t n = 0;
    uint32_t schema = 0;
    bool exists = false;

    mock_init(&mock, &ops);
    memset(&s, 0, sizeof s);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "io", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 5u, payload, sizeof payload),
                  PIXELICA_OK);

    /* A failing write is reported and leaves the stored slot alone. */
    mock.write_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_storage_save(&s, 0u, 6u, payload, 2u), PIXELICA_ERR_PLATFORM);
    mock.write_fail = PIXELICA_OK;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 5);
    CHECK_EQ_LONG(n, (long long)sizeof payload);

    /* A failing read is passed through untranslated. */
    mock.read_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_PLATFORM);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 0u, NULL, NULL), PIXELICA_ERR_PLATFORM);
    mock.read_fail = PIXELICA_OK;

    mock.erase_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_storage_erase(&s, 0u), PIXELICA_ERR_PLATFORM);
    mock.erase_fail = PIXELICA_OK;

    /* ops.size answers exists without reading, so a failing read does not
     * affect it; without ops.size the failure is reported, not turned into
     * "absent". */
    mock.read_fail = PIXELICA_ERR_PLATFORM;
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 0u, &exists), PIXELICA_OK);
    CHECK(exists);
    {
        PIXELICA_Storage probe;
        PIXELICA_StorageOps no_size = ops;

        no_size.size = NULL;
        memset(&probe, 0, sizeof probe);
        CHECK_EQ_LONG(pixelica_storage_init(&probe, "io", &no_size, scratch,
                                            sizeof scratch),
                      PIXELICA_OK);
        CHECK_EQ_LONG(pixelica_storage_exists(&probe, 0u, &exists),
                      PIXELICA_ERR_PLATFORM);
        CHECK_EQ_LONG(pixelica_storage_shutdown(&probe), PIXELICA_OK);
    }
    mock.read_fail = PIXELICA_OK;

    /* An adapter that claims to have copied more than it was given. */
    mock.lie_about_size = true;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_PLATFORM);
    mock.lie_about_size = false;

    /* Corruption: every byte of the envelope is covered by the CRC. */
    mock.data[0][PIXELICA_SAVE_HEADER_BYTES] ^= 0x01u;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_BAD_ASSET);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 0u, NULL, NULL), PIXELICA_ERR_BAD_ASSET);
    mock.data[0][PIXELICA_SAVE_HEADER_BYTES] ^= 0x01u;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_OK);

    /* Truncation and a wrong magic are rejected too. */
    mock.size[0] -= 1u;
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_BAD_ASSET);
    mock.size[0] += 1u;
    mock.data[0][0] = 'X';
    CHECK_EQ_LONG(pixelica_storage_load(&s, 0u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_BAD_ASSET);

    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);
}

/* ---- PC filesystem adapter ---------------------------------------------- */

static int fs_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return 0;
    }
    (void)fclose(f);
    return 1;
}

static void test_fs_adapter(void)
{
    PIXELICA_StorageFs fs;
    PIXELICA_StorageOps ops;
    PIXELICA_Storage s;
    char root[96];
    char file[256];
    char game_dir[160];
    char long_root[PIXELICA_STORAGE_PATH_MAX + 8u];
    const uint8_t payload[5] = { 11u, 22u, 33u, 44u, 55u };
    uint8_t scratch[128];
    uint8_t out[32];
    size_t n = 0;
    uint32_t schema = 0;
    bool exists = true;
    int local = 0;

    (void)snprintf(root, sizeof root, "pixelica_test_storage_%lu_%lu",
                   (unsigned long)time(NULL),
                   (unsigned long)((uintptr_t)&local & 0xFFFFFFFFu));

    CHECK_EQ_LONG(pixelica_storage_fs_init(NULL, root, &ops), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_fs_init(&fs, NULL, &ops), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_fs_init(&fs, "", &ops), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_storage_fs_init(&fs, root, NULL), PIXELICA_ERR_INVALID_ARG);
    memset(long_root, 'x', sizeof long_root - 1u);
    long_root[sizeof long_root - 1u] = '\0';
    CHECK_EQ_LONG(pixelica_storage_fs_init(&fs, long_root, &ops),
                  PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_storage_fs_init(&fs, root, &ops), PIXELICA_OK);
    CHECK(ops.userdata == &fs);
    memset(&s, 0, sizeof s);
    CHECK_EQ_LONG(pixelica_storage_init(&s, "fs-game", &ops, scratch, sizeof scratch),
                  PIXELICA_OK);

    (void)snprintf(game_dir, sizeof game_dir, "%s/fs-game", root);
    (void)snprintf(file, sizeof file, "%s/save02.pxsv", game_dir);

    /* Nothing exists yet; the directories are created by the first write. */
    CHECK_EQ_LONG(pixelica_storage_load(&s, 2u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_NOT_FOUND);
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 2u, &exists), PIXELICA_OK);
    CHECK(!exists);
    CHECK_EQ_LONG(pixelica_storage_erase(&s, 2u), PIXELICA_ERR_NOT_FOUND);

    CHECK_EQ_LONG(pixelica_storage_save(&s, 2u, 42u, payload, sizeof payload),
                  PIXELICA_OK);
    CHECK(fs_file_exists(file));
    CHECK_EQ_LONG(pixelica_storage_exists(&s, 2u, &exists), PIXELICA_OK);
    CHECK(exists);

    memset(out, 0, sizeof out);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 2u, &schema, out, sizeof out, &n),
                  PIXELICA_OK);
    CHECK_EQ_LONG(schema, 42);
    CHECK_EQ_LONG(n, (long long)sizeof payload);
    CHECK(memcmp(out, payload, sizeof payload) == 0);

    /* Too small a destination reports NO_MEMORY and writes nothing. */
    memset(out, 0xC3u, sizeof out);
    CHECK_EQ_LONG(pixelica_storage_load(&s, 2u, &schema, out, 2u, &n),
                  PIXELICA_ERR_NO_MEMORY);
    CHECK_EQ_LONG(out[0], 0xC3);

    /* Rewriting the same slot replaces it atomically; no .tmp is left over. */
    CHECK_EQ_LONG(pixelica_storage_save(&s, 2u, 43u, payload, 2u), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_storage_inspect(&s, 2u, &schema, &n), PIXELICA_OK);
    CHECK_EQ_LONG(schema, 43);
    CHECK_EQ_LONG(n, 2);
    {
        char tmp_path[264];

        (void)snprintf(tmp_path, sizeof tmp_path, "%s.tmp", file);
        CHECK(!fs_file_exists(tmp_path));
    }

    /* A corrupt file on disk is caught by the envelope's CRC, not by trust. */
    {
        FILE *f = fopen(file, "ab");

        CHECK(f != NULL);
        if (f != NULL) {
            CHECK(fputc('!', f) != EOF);
            CHECK(fclose(f) == 0);
        }
    }
    CHECK_EQ_LONG(pixelica_storage_load(&s, 2u, &schema, out, sizeof out, &n),
                  PIXELICA_ERR_BAD_ASSET);

    /* The callbacks refuse a hostile id even when called directly. */
    CHECK_EQ_LONG(ops.read(ops.userdata, "../../etc/passwd", 0u, out, sizeof out, &n),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ops.write(ops.userdata, "..", 0u, payload, sizeof payload),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ops.write(ops.userdata, "fs-game", PIXELICA_STORAGE_SLOT_MAX, payload,
                            sizeof payload),
                  PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ops.erase(ops.userdata, "/etc", 0u), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(ops.size(ops.userdata, "a/b", 0u, &n), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_storage_erase(&s, 2u), PIXELICA_OK);
    CHECK(!fs_file_exists(file));
    CHECK_EQ_LONG(pixelica_storage_shutdown(&s), PIXELICA_OK);

    /* A root that is an ordinary file cannot hold a game directory. */
    {
        PIXELICA_StorageFs bad_fs;
        PIXELICA_StorageOps bad_ops;
        PIXELICA_Storage bad;
        char blocker[128];
        FILE *f;

        (void)snprintf(blocker, sizeof blocker, "%s_file", root);
        f = fopen(blocker, "wb");
        CHECK(f != NULL);
        if (f != NULL) {
            CHECK(fclose(f) == 0);
            CHECK_EQ_LONG(pixelica_storage_fs_init(&bad_fs, blocker, &bad_ops),
                          PIXELICA_OK);
            memset(&bad, 0, sizeof bad);
            CHECK_EQ_LONG(
                pixelica_storage_init(&bad, "fs-game", &bad_ops, scratch, sizeof scratch),
                PIXELICA_OK);
            CHECK_EQ_LONG(pixelica_storage_save(&bad, 0u, 1u, payload, sizeof payload),
                          PIXELICA_ERR_PLATFORM);
            CHECK_EQ_LONG(pixelica_storage_shutdown(&bad), PIXELICA_OK);
            (void)remove(blocker);
        }
    }

    /* Leave no test artifacts behind (remove() removes a directory on POSIX;
     * on Windows it does not, which is harmless inside the build tree). */
    (void)remove(file);
    (void)remove(game_dir);
    (void)remove(root);
}

int main(void)
{
    test_game_id();
    test_init();
    test_round_trip();
    test_limits();
    test_io_failures();
    test_fs_adapter();
    return pixelica_t_finish("test_storage");
}
