/*
 * Pixelica - PC filesystem adapter for pixelica/storage.h.
 *
 * Maps (game id, slot) to "<root>/<game id>/save<NN>.pxsv" and hands the bytes
 * to the shared atomic file helpers in save.c, so a storage save has exactly
 * the same durability behavior as pixelica_save_file_write().
 *
 * Only this file needs anything outside C11: mkdir (POSIX) / _mkdir (Windows)
 * to create the two directory levels it owns. Everything else is stdio.
 *
 * Every callback re-validates the game id and the slot. The adapter is a
 * public table, so a caller that bypasses PIXELICA_Storage still cannot make
 * a path component out of "..", an absolute path or a drive letter.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* mkdir under -std=c11 */
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define STORAGE_FS_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define STORAGE_FS_MKDIR(path) mkdir((path), 0777)
#endif

#include "pixelica/storage.h"

#include "save_internal.h"

#define STORAGE_FS_PREFIX "save"
#define STORAGE_FS_SUFFIX ".pxsv"
/* "save" + two digits + ".pxsv" + NUL. */
#define STORAGE_FS_NAME_BYTES (sizeof STORAGE_FS_PREFIX - 1u + 2u + sizeof STORAGE_FS_SUFFIX)

/* Longest tail appended to the root: "/<game id>/save00.pxsv" + NUL. */
#define STORAGE_FS_TAIL_BYTES (1u + PIXELICA_STORAGE_GAME_ID_MAX + 1u + STORAGE_FS_NAME_BYTES)

typedef char storage_fs_root_fits[(PIXELICA_STORAGE_PATH_MAX > STORAGE_FS_TAIL_BYTES) ? 1 : -1];

/* Builds "<root>/<game id>" into fs->path. The caller has validated both.
 * A root that already ends in a separator (only "/" survives init) does not
 * get a second one. */
static void storage_fs_dir(PIXELICA_StorageFs *fs, const char *game_id)
{
    size_t id_len = strlen(game_id);
    size_t at = fs->root_len;

    memcpy(fs->path, fs->root, fs->root_len);
    if (fs->root[fs->root_len - 1u] != '/' && fs->root[fs->root_len - 1u] != '\\') {
        fs->path[at++] = '/';
    }
    memcpy(fs->path + at, game_id, id_len);
    fs->path[at + id_len] = '\0';
}

/* Builds "<root>/<game id>/save<NN>.pxsv" into fs->path. */
static void storage_fs_file(PIXELICA_StorageFs *fs, const char *game_id, uint32_t slot)
{
    size_t at;

    storage_fs_dir(fs, game_id);
    at = strlen(fs->path);
    fs->path[at++] = '/';
    memcpy(fs->path + at, STORAGE_FS_PREFIX, sizeof STORAGE_FS_PREFIX - 1u);
    at += sizeof STORAGE_FS_PREFIX - 1u;
    fs->path[at++] = (char)('0' + (slot / 10u) % 10u);
    fs->path[at++] = (char)('0' + slot % 10u);
    memcpy(fs->path + at, STORAGE_FS_SUFFIX, sizeof STORAGE_FS_SUFFIX);
}

/* The arguments every callback gets from a hand-written caller too. */
static PIXELICA_Status storage_fs_check(const PIXELICA_StorageFs *fs, const char *game_id,
                                        uint32_t slot)
{
    if (fs == NULL || fs->root_len == 0u || slot >= PIXELICA_STORAGE_SLOT_MAX ||
        !pixelica_storage_game_id_valid(game_id)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

/* Creates one directory. Succeeds when it already exists. */
static bool storage_fs_mkdir(const char *path)
{
    if (STORAGE_FS_MKDIR(path) == 0) {
        return true;
    }
#ifdef EEXIST
    return errno == EEXIST;
#else
    return false;
#endif
}

static PIXELICA_Status storage_fs_write(void *userdata, const char *game_id, uint32_t slot,
                                        const void *data, size_t size)
{
    PIXELICA_StorageFs *fs = (PIXELICA_StorageFs *)userdata;
    PIXELICA_Status st;

    st = storage_fs_check(fs, game_id, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    /* The two levels this adapter owns; the parents of root must exist. */
    memcpy(fs->path, fs->root, fs->root_len + 1u);
    errno = 0;
    if (!storage_fs_mkdir(fs->path)) {
        return PIXELICA_ERR_PLATFORM;
    }
    storage_fs_dir(fs, game_id);
    errno = 0;
    if (!storage_fs_mkdir(fs->path)) {
        return PIXELICA_ERR_PLATFORM;
    }
    storage_fs_file(fs, game_id, slot);
    return pixelica_save_raw_write(fs->path, data, size);
}

static PIXELICA_Status storage_fs_read(void *userdata, const char *game_id, uint32_t slot,
                                       void *out, size_t capacity, size_t *out_size)
{
    PIXELICA_StorageFs *fs = (PIXELICA_StorageFs *)userdata;
    PIXELICA_Status st;

    st = storage_fs_check(fs, game_id, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_size == NULL || (out == NULL && capacity != 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    storage_fs_file(fs, game_id, slot);
    return pixelica_save_raw_read(fs->path, out, capacity, out_size);
}

static PIXELICA_Status storage_fs_erase(void *userdata, const char *game_id, uint32_t slot)
{
    PIXELICA_StorageFs *fs = (PIXELICA_StorageFs *)userdata;
    PIXELICA_Status st;

    st = storage_fs_check(fs, game_id, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    storage_fs_file(fs, game_id, slot);
    errno = 0;
    if (remove(fs->path) == 0) {
        return PIXELICA_OK;
    }
#ifdef ENOENT
    if (errno == ENOENT) {
        return PIXELICA_ERR_NOT_FOUND;
    }
#endif
    return PIXELICA_ERR_PLATFORM;
}

static PIXELICA_Status storage_fs_size(void *userdata, const char *game_id, uint32_t slot,
                                       size_t *out_size)
{
    PIXELICA_StorageFs *fs = (PIXELICA_StorageFs *)userdata;
    PIXELICA_Status st;

    st = storage_fs_check(fs, game_id, slot);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_size == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    storage_fs_file(fs, game_id, slot);
    st = pixelica_save_raw_read(fs->path, NULL, 0u, out_size);
    /* A zero-capacity read reports the size and then NO_MEMORY; only an empty
     * file (which is not a save) returns OK. */
    return st == PIXELICA_ERR_NO_MEMORY ? PIXELICA_OK : st;
}

PIXELICA_Status pixelica_storage_fs_init(PIXELICA_StorageFs *fs, const char *root,
                                         PIXELICA_StorageOps *out_ops)
{
    size_t len;

    if (fs == NULL || out_ops == NULL || root == NULL || root[0] == '\0') {
        return PIXELICA_ERR_INVALID_ARG;
    }
    len = strlen(root);
    /* Keep room for the longest "/<game id>/save<NN>.pxsv" plus the ".tmp"
     * the atomic write appends. */
    if (len + STORAGE_FS_TAIL_BYTES + 4u > PIXELICA_STORAGE_PATH_MAX) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* A trailing separator would produce "<root>//<game id>"; drop it. */
    while (len > 1u && (root[len - 1u] == '/' || root[len - 1u] == '\\')) {
        --len;
    }
    memset(fs, 0, sizeof *fs);
    memcpy(fs->root, root, len);
    fs->root[len] = '\0';
    fs->root_len = len;

    memset(out_ops, 0, sizeof *out_ops);
    out_ops->userdata = fs;
    out_ops->write = storage_fs_write;
    out_ops->read = storage_fs_read;
    out_ops->erase = storage_fs_erase;
    out_ops->size = storage_fs_size;
    return PIXELICA_OK;
}
