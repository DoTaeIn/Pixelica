/*
 * Pixelica - portable game lifecycle: initialization, pause/resume,
 * suspend/resume, shutdown and game-owned checkpoints.
 *
 * One small state machine that every platform driver can map its own events
 * onto, so the game's own code contains no platform #ifdef. No heap, no global
 * state, no renderer, no clock, no threads: a PIXELICA_App is caller-owned
 * memory used by one thread, and the engine never calls a lifecycle function
 * by itself. The driver (SDL3 main loop, ESP-IDF task, a test) decides when a
 * transition happens; this module enforces which ones are legal, in which
 * order the callbacks run, and when a checkpoint is written.
 *
 * States (pixelica_app_state):
 *
 *            pixelica_app_init
 *                   |
 *                   v
 *        +----> RUNNING <----- resume ----- PAUSED <----+
 *        |       |   |                        ^   |     |
 *        +-resume+   +-------- pause ---------+   |     |
 *                |                                |     |
 *             suspend                          suspend  |
 *                v                                v     |
 *              SUSPENDED --- resume --> (back to RUNNING or PAUSED)
 *
 *   any live state --- pixelica_app_shutdown ---> STOPPED
 *
 * PAUSE / RESUME is cheap and in-process: the window lost focus, a pause menu
 * opened, the player pressed Start. The process keeps running and keeps all of
 * its memory, so nothing is written to storage. Only the driver's own update
 * loop stops (pixelica_app_should_update() goes false). It is the frequent,
 * free transition.
 *
 * SUSPEND / RESUME is the expensive one: the host is putting the game in the
 * background and MAY terminate the process afterwards without another
 * callback. Suspending therefore writes a checkpoint (below) and the app
 * remembers which state it came from. If the process survives, resume returns
 * to exactly that state (a game paused before suspend stays paused). If the
 * process is killed, the next launch is an ordinary pixelica_app_init() which
 * restores from the checkpoint.
 *
 * The engine does NOT synthesize transitions. A platform that sends "will
 * resign active" and then "did enter background" should forward both (pause,
 * then suspend); a platform that only has a background event just calls
 * suspend from RUNNING.
 *
 * CHECKPOINTS are game-owned serialization, never a process snapshot. The app
 * calls save_checkpoint, which writes the game's own bytes (fixed order,
 * fixed width, see docs/save.md) into the caller-supplied scratch buffer; the
 * bytes go through pixelica/storage.h, which wraps them in the versioned
 * CRC-32 envelope. On the way back, load_checkpoint gets the schema version
 * the game stored plus those bytes and rebuilds the game state itself. The
 * engine never inspects, diffs or migrates a payload, and no engine object
 * (context, screen stack, textures, pointers) is ever persisted.
 *
 * Callback order:
 *   init      [restore: storage read + load_checkpoint] then on_init  -> RUNNING
 *   pause     on_pause                                                -> PAUSED
 *   resume    on_resume                     -> RUNNING / PAUSED / unchanged
 *   suspend   [checkpoint] then on_suspend                          -> SUSPENDED
 *   shutdown  [checkpoint] then on_shutdown                          -> STOPPED
 * A lifecycle callback that returns a failure status aborts init (the app
 * stays uninitialised) but cannot abort a pause, resume, suspend or shutdown:
 * those statuses are reported to the caller after the transition has happened.
 *
 * Idempotence: pause in PAUSED, resume in RUNNING and suspend in SUSPENDED
 * return PIXELICA_OK and run no callback, because platforms do repeat their
 * focus and background events.
 *
 * ONE DOCUMENTED EXCEPTION to the engine's "a failed call changes nothing"
 * rule: pixelica_app_suspend() and pixelica_app_shutdown() transition even
 * when the checkpoint fails, because the host is not offering the game a
 * choice. They return the checkpoint's status (also available from
 * pixelica_app_checkpoint_status()), so check pixelica_app_state() rather
 * than the return value to learn where the app is.
 *
 * Re-entrancy: calling init/pause/resume/suspend/shutdown/checkpoint from
 * inside any callback returns PIXELICA_ERR_INVALID_ARG and does nothing. The
 * getters are always allowed.
 *
 * Lifetimes (all borrowed, nothing is owned or freed by the app):
 *   - PIXELICA_App: caller-owned, stable address from init to shutdown. Zero-
 *     initialise it before the first init (PIXELICA_App app = {0}).
 *   - PIXELICA_AppConfig is copied by value and need not outlive the call.
 *   - callbacks.userdata must stay valid until on_shutdown has returned.
 *   - config.storage (may be NULL) must stay initialised and untouched by
 *     anyone else until shutdown.
 *   - config.scratch is the game's serialization buffer, exclusively the app's
 *     from init to shutdown; its contents between calls are unspecified.
 */
#ifndef PIXELICA_APP_H
#define PIXELICA_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"
#include "pixelica/storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PIXELICA_AppState {
    PIXELICA_APP_UNINITIALIZED = 0, /* before init, and after a failed init */
    PIXELICA_APP_RUNNING,           /* updating */
    PIXELICA_APP_PAUSED,            /* in process, not updating */
    PIXELICA_APP_SUSPENDED,         /* backgrounded, checkpoint written */
    PIXELICA_APP_STOPPED            /* shut down; init may be called again */
} PIXELICA_AppState;

/* Lifecycle notification. Return PIXELICA_OK on success; see the header
 * comment for what a failure does at each point. */
typedef PIXELICA_Status (*PIXELICA_AppFn)(void *userdata);

/*
 * Writes the game's current progress into out[0..capacity) and reports the
 * number of bytes plus the schema version those bytes are in. Serialize field
 * by field in a fixed byte order; never fwrite a struct. Return
 * PIXELICA_ERR_NO_MEMORY when `capacity` is too small (grow
 * PIXELICA_AppConfig.scratch_size). `out` may be NULL when capacity is 0.
 * A zero-byte checkpoint is allowed.
 */
typedef PIXELICA_Status (*PIXELICA_AppSaveFn)(void *userdata, void *out, size_t capacity,
                                              size_t *out_size,
                                              uint32_t *out_schema_version);

/*
 * Rebuilds the game state from bytes a previous save_checkpoint produced.
 * `schema_version` is what that call reported: branch on it and migrate in the
 * game. Return any failure status for data the game does not accept (an
 * unknown version, an impossible value); the app then reports "not restored"
 * and the game must start from its defaults. `data` may be NULL when size
 * is 0.
 */
typedef PIXELICA_Status (*PIXELICA_AppLoadFn)(void *userdata, uint32_t schema_version,
                                              const void *data, size_t size);

typedef struct PIXELICA_AppCallbacks {
    PIXELICA_AppFn on_init;     /* after a restore attempt, before RUNNING */
    PIXELICA_AppFn on_pause;
    PIXELICA_AppFn on_resume;
    PIXELICA_AppFn on_suspend;  /* after the checkpoint was attempted */
    PIXELICA_AppFn on_shutdown; /* last call; userdata may be freed after it */
    PIXELICA_AppSaveFn save_checkpoint;
    PIXELICA_AppLoadFn load_checkpoint;
    void *userdata;
} PIXELICA_AppCallbacks;

typedef struct PIXELICA_AppConfig {
    PIXELICA_AppCallbacks callbacks;
    /* Where checkpoints go. NULL (or a storage without the matching callback)
     * disables them: suspend and shutdown then only run their notification and
     * pixelica_app_checkpoint() returns INVALID_ARG. */
    PIXELICA_Storage *storage;
    uint32_t checkpoint_slot;     /* < PIXELICA_STORAGE_SLOT_MAX */
    void *scratch;                /* game serialization buffer, >= the biggest payload */
    size_t scratch_size;          /* 0 is allowed only for a zero-byte checkpoint */
    bool restore_on_init;         /* read checkpoint_slot during init */
    bool checkpoint_on_suspend;   /* write a checkpoint while suspending */
    bool checkpoint_on_shutdown;  /* write a checkpoint while shutting down */
} PIXELICA_AppConfig;

/* Monotonic counters, for diagnostics and tests. */
typedef struct PIXELICA_AppStats {
    uint32_t pauses;
    uint32_t resumes;
    uint32_t suspends;
    uint32_t checkpoints;         /* successful writes */
    uint32_t checkpoint_failures; /* attempts that returned non-OK */
    uint32_t restores;            /* successful restores (0 or 1 per init) */
} PIXELICA_AppStats;

/* Caller-owned; treat every field as internal. */
typedef struct PIXELICA_App {
    uint32_t state;
    uint32_t resume_state; /* state to return to from SUSPENDED */
    bool busy;             /* a callback is running */
    bool restored;
    PIXELICA_Status restore_status;
    PIXELICA_Status checkpoint_status;
    PIXELICA_AppConfig config;
    PIXELICA_AppStats stats;
} PIXELICA_App;

/*
 * Validates the config, copies it, optionally restores a checkpoint, runs
 * on_init and enters RUNNING.
 *
 * INVALID_ARG (and the app is left untouched) for a NULL app or config, an
 * app that is already live, checkpoint_slot >= PIXELICA_STORAGE_SLOT_MAX,
 * scratch == NULL with scratch_size != 0, a checkpoint option without a
 * storage, restore_on_init without load_checkpoint, or a checkpoint option
 * without save_checkpoint.
 *
 * A restore that finds nothing (NOT_FOUND), finds corrupt bytes (BAD_ASSET) or
 * is rejected by load_checkpoint is NOT an init failure: the app starts
 * RUNNING with pixelica_app_restored() == false and the reason in
 * pixelica_app_restore_status(), and the game uses its defaults. Only a
 * failing on_init aborts init; the app then stays UNINITIALIZED and that
 * status is returned.
 */
PIXELICA_Status pixelica_app_init(PIXELICA_App *app, const PIXELICA_AppConfig *config);

/* RUNNING -> PAUSED (on_pause). No-op OK in PAUSED. INVALID_ARG in
 * SUSPENDED / STOPPED / UNINITIALIZED. */
PIXELICA_Status pixelica_app_pause(PIXELICA_App *app);

/* PAUSED -> RUNNING, or SUSPENDED -> the state it was suspended from, running
 * on_resume either way. No-op OK in RUNNING. INVALID_ARG in STOPPED /
 * UNINITIALIZED. */
PIXELICA_Status pixelica_app_resume(PIXELICA_App *app);

/* RUNNING or PAUSED -> SUSPENDED: writes a checkpoint when
 * checkpoint_on_suspend is set, then runs on_suspend. No-op OK in SUSPENDED.
 * INVALID_ARG in STOPPED / UNINITIALIZED. Otherwise returns the checkpoint
 * status, having transitioned regardless (see the header comment). */
PIXELICA_Status pixelica_app_suspend(PIXELICA_App *app);

/* Any live state -> STOPPED: writes a checkpoint when checkpoint_on_shutdown
 * is set, then runs on_shutdown, then drops its copy of the config (the
 * borrowed storage, scratch and userdata pointers may die with the call). The
 * app may be init-ed again, and the counters and the restore/checkpoint
 * statuses stay readable for a final log line. INVALID_ARG in STOPPED /
 * UNINITIALIZED, so a second shutdown fails. Otherwise returns the checkpoint
 * status, having transitioned regardless. */
PIXELICA_Status pixelica_app_shutdown(PIXELICA_App *app);

/* Writes a checkpoint now (autosave, level change, settings screen). Allowed
 * in RUNNING, PAUSED and SUSPENDED. INVALID_ARG without a storage and a
 * save_checkpoint callback. Returns the storage error on failure, in which
 * case the stored slot is unchanged. */
PIXELICA_Status pixelica_app_checkpoint(PIXELICA_App *app);

/*
 * Reads checkpoint_slot and hands it to load_checkpoint again, outside init
 * ("load game" from a menu). Allowed in RUNNING and PAUSED. Returns NOT_FOUND
 * for an empty slot, BAD_ASSET for corrupt bytes, or whatever load_checkpoint
 * returned; pixelica_app_restored() and pixelica_app_restore_status() are
 * updated to match. The game's own state is only changed by its
 * load_checkpoint.
 */
PIXELICA_Status pixelica_app_restore(PIXELICA_App *app);

/* UNINITIALIZED for NULL. */
PIXELICA_AppState pixelica_app_state(const PIXELICA_App *app);

/* true only in RUNNING: the driver should call its update/draw this frame. */
bool pixelica_app_should_update(const PIXELICA_App *app);

/* Whether the last restore attempt (init or pixelica_app_restore) rebuilt the
 * game state, and why it did not. The status is PIXELICA_OK when a restore
 * succeeded and when none was requested. */
bool pixelica_app_restored(const PIXELICA_App *app);
PIXELICA_Status pixelica_app_restore_status(const PIXELICA_App *app);

/* Status of the most recent checkpoint attempt (PIXELICA_OK when none ran). */
PIXELICA_Status pixelica_app_checkpoint_status(const PIXELICA_App *app);

/* Copies the counters. They survive shutdown and are reset by the next init.
 * INVALID_ARG for a NULL argument or an app that was never init-ed. */
PIXELICA_Status pixelica_app_stats(const PIXELICA_App *app, PIXELICA_AppStats *out_stats);

/* Stable ASCII name of a state ("running", ...) for logs; "unknown" for a
 * value outside the enum. Never NULL. */
const char *pixelica_app_state_name(PIXELICA_AppState state);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_APP_H */
