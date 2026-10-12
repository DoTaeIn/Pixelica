/*
 * Pixelica - minimal game screen/state manager (title / play / pause / result).
 *
 * A caller-owned stack of at most PIXELICA_SCREEN_STACK_MAX screens, each a
 * set of optional callbacks plus a userdata pointer. No heap, no SDL, no
 * renderer, no clock, no global state. Single-threaded; distinct managers
 * share nothing.
 *
 * Callbacks (all optional, NULL = no-op, all return void):
 *   enter   the screen was installed (init/change/push)
 *   leave   the screen is being removed (change/pop/shutdown)
 *   pause   another screen was pushed on top of it
 *   resume  the screen above it was popped; it is the top again
 *   update  once per update() call, TOP screen only
 *   draw    once per draw() call (see draw policy)
 *
 * Lifecycle order:
 *   init(s)       s.enter
 *   change(n)     top.leave, n.enter                  (no pause/resume)
 *   push(n)       top.pause, n.enter                  (top stays on the stack)
 *   pop()         top.leave, below.resume
 *   shutdown()    leave for every screen, top first
 *
 * Update policy: only the top screen is updated. A paused screen below an
 * overlay is never updated until it is resumed.
 *
 * Draw policy: draw() calls draw on the top screen and on every screen below
 * it that is reachable through a chain of PIXELICA_SCREEN_TRANSPARENT screens,
 * bottom to top. I.e. start at the top; while the current screen is
 * TRANSPARENT and has a screen below, step down; then draw from there up.
 * A screen without the flag is opaque and hides everything beneath it. The
 * bottom screen is always the last one considered.
 *
 * Transitions (change/push/pop) are the only way to modify the stack, and
 * they are QUEUED, never applied recursively:
 *   - Called from inside ANY callback (enter/leave/pause/resume/update/draw),
 *     the request is validated, stored in a bounded FIFO queue of
 *     PIXELICA_SCREEN_QUEUE_MAX entries, and returns at once. The stack is
 *     untouched, so the running callback and the loop over screens in
 *     update()/draw() see a stable stack.
 *   - Called outside callbacks, the request is queued and applied before the
 *     call returns.
 *   - Safe boundaries: the start and end of update(), the end of draw(), the
 *     end of init() and of an outside change/push/pop. At each boundary ONE
 *     batch is applied: exactly the entries queued when the boundary began.
 *     Requests made by callbacks during that batch (e.g. an enter that asks
 *     for another change) stay queued for the NEXT boundary. So a cycle of
 *     screens that keep redirecting each other advances a bounded number of
 *     steps per call and can never loop forever or invalidate a running
 *     callback.
 *   - Requests are validated against the stack as it will be after all
 *     queued entries ran, so an accepted request cannot fail later:
 *       NULL/invalid screen, unknown flag bit   -> PIXELICA_ERR_INVALID_ARG
 *       pop that would remove the last screen   -> PIXELICA_ERR_INVALID_ARG
 *       push past STACK_MAX screens             -> PIXELICA_ERR_CAPACITY
 *       queue already holds QUEUE_MAX entries   -> PIXELICA_ERR_CAPACITY
 *     A rejected request changes nothing.
 *
 * Re-entrancy: init/update/draw/shutdown called from inside a callback return
 * PIXELICA_ERR_INVALID_ARG. During shutdown (leave callbacks) transition
 * requests also return INVALID_ARG and pending ones are discarded.
 *
 * Lifetimes (all borrowed, nothing is owned or freed by the manager):
 *   - the manager: caller-owned, at a stable address from init to shutdown.
 *     Zero-initialise it (or shut it down) before init; its fields are
 *     internal, do not edit them.
 *   - the PIXELICA_Screen descriptor passed to init/change/push is COPIED, it
 *     need not outlive the call.
 *   - `userdata` stays caller-owned and must stay valid until that screen's
 *     leave callback has returned (or it has no leave callback and the screen
 *     was removed/shut down).
 *   - the `target` of draw() is passed through opaquely (may be NULL) and is
 *     only used during that call.
 *
 * Failure behavior: every function returns PIXELICA_ERR_INVALID_ARG for a NULL
 * or uninitialised/shut-down manager and then changes nothing (a second
 * shutdown therefore returns INVALID_ARG). Callbacks cannot fail; report game
 * errors through your own userdata.
 */
#ifndef PIXELICA_SCREENS_H
#define PIXELICA_SCREENS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_SCREEN_STACK_MAX 8u
#define PIXELICA_SCREEN_QUEUE_MAX 4u

/* PIXELICA_Screen.flags: draw the screen(s) below this one too (overlay). */
#define PIXELICA_SCREEN_TRANSPARENT ((uint32_t)1u << 0)

typedef struct PIXELICA_ScreenManager PIXELICA_ScreenManager;

/* enter/leave/pause/resume. May call change/push/pop on `screens` (queued). */
typedef void (*PIXELICA_ScreenFn)(PIXELICA_ScreenManager *screens, void *userdata);
/* dt: seconds, finite and >= 0 (validated by pixelica_screens_update). */
typedef void (*PIXELICA_ScreenUpdateFn)(PIXELICA_ScreenManager *screens, void *userdata,
                                        float dt);
/* target: whatever the caller gave pixelica_screens_draw (framebuffer, context...). */
typedef void (*PIXELICA_ScreenDrawFn)(PIXELICA_ScreenManager *screens, void *userdata,
                                      void *target);

typedef struct PIXELICA_Screen {
    PIXELICA_ScreenFn enter;
    PIXELICA_ScreenFn leave;
    PIXELICA_ScreenFn pause;
    PIXELICA_ScreenFn resume;
    PIXELICA_ScreenUpdateFn update;
    PIXELICA_ScreenDrawFn draw;
    void *userdata;
    uint32_t flags; /* 0 or PIXELICA_SCREEN_TRANSPARENT; other bits are rejected */
} PIXELICA_Screen;

/* Storage only: treat every field as internal. */
typedef struct PIXELICA_ScreenOp {
    uint32_t kind;
    PIXELICA_Screen screen;
} PIXELICA_ScreenOp;

struct PIXELICA_ScreenManager {
    uint32_t state;
    bool busy;
    uint32_t depth;     /* screens on the stack right now */
    uint32_t projected; /* depth after every queued op has run */
    uint32_t queue_head;
    uint32_t queue_count;
    PIXELICA_Screen stack[PIXELICA_SCREEN_STACK_MAX];
    PIXELICA_ScreenOp queue[PIXELICA_SCREEN_QUEUE_MAX];
};

/* Installs `first` as the only screen and runs its enter. On error the
 * manager is untouched. Do not call on a live manager (shut it down first). */
PIXELICA_Status pixelica_screens_init(PIXELICA_ScreenManager *screens,
                                      const PIXELICA_Screen *first);

/* Replace the top screen: top.leave, next.enter. */
PIXELICA_Status pixelica_screens_change(PIXELICA_ScreenManager *screens,
                                        const PIXELICA_Screen *next);

/* Pause the top screen and put `next` above it: top.pause, next.enter. */
PIXELICA_Status pixelica_screens_push(PIXELICA_ScreenManager *screens,
                                      const PIXELICA_Screen *next);

/* Remove the top screen and resume the one below it. The last screen cannot
 * be popped (INVALID_ARG); use change or shutdown. */
PIXELICA_Status pixelica_screens_pop(PIXELICA_ScreenManager *screens);

/* Applies pending transitions, updates the top screen, applies the
 * transitions it requested. dt must be finite and >= 0, else INVALID_ARG with
 * no callback run. */
PIXELICA_Status pixelica_screens_update(PIXELICA_ScreenManager *screens, float dt);

/* Draws per the draw policy, then applies transitions requested while drawing. */
PIXELICA_Status pixelica_screens_draw(PIXELICA_ScreenManager *screens, void *target);

/* Calls leave on every screen (top first), then clears the manager (it may be
 * init-ed again). Pending transitions are discarded. */
PIXELICA_Status pixelica_screens_shutdown(PIXELICA_ScreenManager *screens);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_SCREENS_H */
