/*
 * Pixelica - portable game lifecycle state machine (pixelica/app.h).
 *
 * A validated transition table plus the checkpoint plumbing. No heap, no
 * clock, no global state: the PIXELICA_App is caller memory, and a checkpoint
 * is only ever the bytes the game's own save_checkpoint callback produced.
 *
 * `busy` is set around every callback so that a callback which calls back
 * into the app is rejected instead of corrupting the state machine.
 */
#include <string.h>

#include "pixelica/app.h"

static bool app_live(const PIXELICA_App *app)
{
    return app != NULL &&
           (app->state == PIXELICA_APP_RUNNING || app->state == PIXELICA_APP_PAUSED ||
            app->state == PIXELICA_APP_SUSPENDED);
}

/* Runs one optional notification with the re-entrancy guard held. */
static PIXELICA_Status app_notify(PIXELICA_App *app, PIXELICA_AppFn fn)
{
    PIXELICA_Status st;

    if (fn == NULL) {
        return PIXELICA_OK;
    }
    app->busy = true;
    st = fn(app->config.callbacks.userdata);
    app->busy = false;
    return st;
}

/* Serializes through the game callback and stores the bytes. Updates
 * checkpoint_status and the counters. */
static PIXELICA_Status app_write_checkpoint(PIXELICA_App *app)
{
    size_t size = 0;
    uint32_t schema = 0;
    PIXELICA_Status st;

    if (app->config.storage == NULL || app->config.callbacks.save_checkpoint == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    app->busy = true;
    st = app->config.callbacks.save_checkpoint(app->config.callbacks.userdata,
                                               app->config.scratch,
                                               app->config.scratch_size, &size, &schema);
    app->busy = false;
    if (st == PIXELICA_OK && size > app->config.scratch_size) {
        st = PIXELICA_ERR_NO_MEMORY; /* the game reported more than it was given */
    }
    if (st == PIXELICA_OK) {
        st = pixelica_storage_save(app->config.storage, app->config.checkpoint_slot,
                                   schema, app->config.scratch, size);
    }
    app->checkpoint_status = st;
    if (st == PIXELICA_OK) {
        ++app->stats.checkpoints;
    } else {
        ++app->stats.checkpoint_failures;
    }
    return st;
}

/* Reads the slot and hands it to the game. Updates restored/restore_status. */
static PIXELICA_Status app_read_checkpoint(PIXELICA_App *app)
{
    size_t size = 0;
    uint32_t schema = 0;
    PIXELICA_Status st;

    if (app->config.storage == NULL || app->config.callbacks.load_checkpoint == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    app->restored = false;
    st = pixelica_storage_load(app->config.storage, app->config.checkpoint_slot, &schema,
                               app->config.scratch, app->config.scratch_size, &size);
    if (st == PIXELICA_OK) {
        app->busy = true;
        st = app->config.callbacks.load_checkpoint(app->config.callbacks.userdata, schema,
                                                   app->config.scratch, size);
        app->busy = false;
    }
    app->restore_status = st;
    if (st == PIXELICA_OK) {
        app->restored = true;
        ++app->stats.restores;
    }
    return st;
}

static PIXELICA_Status app_config_valid(const PIXELICA_AppConfig *config)
{
    bool wants_save;
    bool wants_load;

    if (config->checkpoint_slot >= PIXELICA_STORAGE_SLOT_MAX) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (config->scratch == NULL && config->scratch_size != 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    wants_save = config->checkpoint_on_suspend || config->checkpoint_on_shutdown;
    wants_load = config->restore_on_init;
    if ((wants_save || wants_load) && config->storage == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (wants_save && config->callbacks.save_checkpoint == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (wants_load && config->callbacks.load_checkpoint == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_app_init(PIXELICA_App *app, const PIXELICA_AppConfig *config)
{
    PIXELICA_App fresh;
    PIXELICA_Status st;

    if (app == NULL || config == NULL || app->busy || app_live(app)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = app_config_valid(config);
    if (st != PIXELICA_OK) {
        return st;
    }
    memset(&fresh, 0, sizeof fresh);
    fresh.config = *config;
    fresh.state = PIXELICA_APP_RUNNING;
    fresh.resume_state = PIXELICA_APP_RUNNING;
    fresh.restore_status = PIXELICA_OK;
    fresh.checkpoint_status = PIXELICA_OK;
    *app = fresh;

    if (config->restore_on_init) {
        /* An empty, corrupt or rejected checkpoint is a normal cold start:
         * the game keeps its defaults and can read the reason afterwards. */
        (void)app_read_checkpoint(app);
    }
    st = app_notify(app, config->callbacks.on_init);
    if (st != PIXELICA_OK) {
        memset(app, 0, sizeof *app); /* back to UNINITIALIZED */
        return st;
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_app_pause(PIXELICA_App *app)
{
    if (!app_live(app) || app->busy || app->state == PIXELICA_APP_SUSPENDED) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (app->state == PIXELICA_APP_PAUSED) {
        return PIXELICA_OK; /* platforms repeat focus events */
    }
    app->state = PIXELICA_APP_PAUSED;
    ++app->stats.pauses;
    return app_notify(app, app->config.callbacks.on_pause);
}

PIXELICA_Status pixelica_app_resume(PIXELICA_App *app)
{
    if (!app_live(app) || app->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (app->state == PIXELICA_APP_RUNNING) {
        return PIXELICA_OK;
    }
    if (app->state == PIXELICA_APP_SUSPENDED) {
        /* Back to whatever the app was doing before the host backgrounded it:
         * a game paused before suspend must not start updating again. */
        app->state = app->resume_state;
    } else {
        app->state = PIXELICA_APP_RUNNING;
    }
    ++app->stats.resumes;
    return app_notify(app, app->config.callbacks.on_resume);
}

PIXELICA_Status pixelica_app_suspend(PIXELICA_App *app)
{
    PIXELICA_Status checkpoint = PIXELICA_OK;
    PIXELICA_Status st;

    if (!app_live(app) || app->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (app->state == PIXELICA_APP_SUSPENDED) {
        return PIXELICA_OK;
    }
    if (app->config.checkpoint_on_suspend) {
        checkpoint = app_write_checkpoint(app);
    }
    /* The host is not offering a choice: suspend even if the save failed. */
    app->resume_state = app->state;
    app->state = PIXELICA_APP_SUSPENDED;
    ++app->stats.suspends;
    st = app_notify(app, app->config.callbacks.on_suspend);
    return checkpoint != PIXELICA_OK ? checkpoint : st;
}

PIXELICA_Status pixelica_app_shutdown(PIXELICA_App *app)
{
    PIXELICA_Status checkpoint = PIXELICA_OK;
    PIXELICA_Status st;

    if (!app_live(app) || app->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (app->config.checkpoint_on_shutdown) {
        checkpoint = app_write_checkpoint(app);
    }
    app->state = PIXELICA_APP_STOPPED;
    st = app_notify(app, app->config.callbacks.on_shutdown);
    /* Drop the borrowed pointers (userdata may be freed by on_shutdown) but
     * keep the post-mortem fields readable for a last log line. A second
     * shutdown still fails, and init may be called again. */
    memset(&app->config, 0, sizeof app->config);
    app->resume_state = (uint32_t)PIXELICA_APP_STOPPED;
    return checkpoint != PIXELICA_OK ? checkpoint : st;
}

PIXELICA_Status pixelica_app_checkpoint(PIXELICA_App *app)
{
    if (!app_live(app) || app->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return app_write_checkpoint(app);
}

PIXELICA_Status pixelica_app_restore(PIXELICA_App *app)
{
    if (!app_live(app) || app->busy || app->state == PIXELICA_APP_SUSPENDED) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return app_read_checkpoint(app);
}

PIXELICA_AppState pixelica_app_state(const PIXELICA_App *app)
{
    if (app == NULL) {
        return PIXELICA_APP_UNINITIALIZED;
    }
    return (PIXELICA_AppState)app->state;
}

bool pixelica_app_should_update(const PIXELICA_App *app)
{
    return app != NULL && app->state == PIXELICA_APP_RUNNING;
}

bool pixelica_app_restored(const PIXELICA_App *app)
{
    return app != NULL && app->restored;
}

PIXELICA_Status pixelica_app_restore_status(const PIXELICA_App *app)
{
    return app == NULL ? PIXELICA_ERR_INVALID_ARG : app->restore_status;
}

PIXELICA_Status pixelica_app_checkpoint_status(const PIXELICA_App *app)
{
    return app == NULL ? PIXELICA_ERR_INVALID_ARG : app->checkpoint_status;
}

PIXELICA_Status pixelica_app_stats(const PIXELICA_App *app, PIXELICA_AppStats *out_stats)
{
    if (app == NULL || out_stats == NULL ||
        app->state == (uint32_t)PIXELICA_APP_UNINITIALIZED) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_stats = app->stats;
    return PIXELICA_OK;
}

const char *pixelica_app_state_name(PIXELICA_AppState state)
{
    switch (state) {
    case PIXELICA_APP_UNINITIALIZED:
        return "uninitialized";
    case PIXELICA_APP_RUNNING:
        return "running";
    case PIXELICA_APP_PAUSED:
        return "paused";
    case PIXELICA_APP_SUSPENDED:
        return "suspended";
    case PIXELICA_APP_STOPPED:
        return "stopped";
    default:
        break;
    }
    return "unknown";
}
