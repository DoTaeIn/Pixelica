#include "pixelica/screens.h"

#include <float.h>
#include <string.h>

enum { STATE_LIVE = 0x53435231u, STATE_CLOSING = 0x53435232u };
enum { OP_CHANGE = 1, OP_PUSH, OP_POP };

static bool is_live(const PIXELICA_ScreenManager *m)
{
    return m != NULL && m->state == STATE_LIVE;
}

static bool screen_valid(const PIXELICA_Screen *s)
{
    return s != NULL && (s->flags & ~PIXELICA_SCREEN_TRANSPARENT) == 0u;
}

static void call(PIXELICA_ScreenManager *m, const PIXELICA_Screen *s, PIXELICA_ScreenFn fn)
{
    if (fn != NULL) {
        fn(m, s->userdata);
    }
}

/* Runs one validated op. Callbacks only queue, so the stack stays stable. */
static void apply(PIXELICA_ScreenManager *m, const PIXELICA_ScreenOp *op)
{
    PIXELICA_Screen old;

    switch (op->kind) {
    case OP_CHANGE:
        old = m->stack[m->depth - 1u];
        call(m, &old, old.leave);
        m->stack[m->depth - 1u] = op->screen;
        call(m, &op->screen, op->screen.enter);
        break;
    case OP_PUSH:
        old = m->stack[m->depth - 1u];
        call(m, &old, old.pause);
        m->stack[m->depth++] = op->screen;
        call(m, &op->screen, op->screen.enter);
        break;
    case OP_POP:
        old = m->stack[--m->depth];
        call(m, &old, old.leave);
        old = m->stack[m->depth - 1u];
        call(m, &old, old.resume);
        break;
    default:
        break;
    }
}

/* One batch: only the entries queued on entry; later ones wait. */
static void flush(PIXELICA_ScreenManager *m)
{
    uint32_t n = m->queue_count;

    m->busy = true;
    while (n-- > 0u) {
        PIXELICA_ScreenOp op = m->queue[m->queue_head];

        m->queue_head = (m->queue_head + 1u) % PIXELICA_SCREEN_QUEUE_MAX;
        --m->queue_count;
        apply(m, &op);
    }
    m->busy = false;
}

static PIXELICA_Status request(PIXELICA_ScreenManager *m, uint32_t kind,
                               const PIXELICA_Screen *screen)
{
    PIXELICA_ScreenOp *slot;
    uint32_t projected;

    if (!is_live(m)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (kind == OP_POP) {
        if (m->projected < 2u) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        projected = m->projected - 1u;
    } else {
        if (!screen_valid(screen)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (kind == OP_PUSH && m->projected >= PIXELICA_SCREEN_STACK_MAX) {
            return PIXELICA_ERR_CAPACITY;
        }
        projected = m->projected + (kind == OP_PUSH ? 1u : 0u);
    }
    if (m->queue_count >= PIXELICA_SCREEN_QUEUE_MAX) {
        return PIXELICA_ERR_CAPACITY;
    }

    slot = &m->queue[(m->queue_head + m->queue_count) % PIXELICA_SCREEN_QUEUE_MAX];
    slot->kind = kind;
    if (kind != OP_POP) {
        slot->screen = *screen;
    }
    ++m->queue_count;
    m->projected = projected;

    if (!m->busy) {
        flush(m);
    }
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_screens_init(PIXELICA_ScreenManager *screens,
                                      const PIXELICA_Screen *first)
{
    if (screens == NULL || !screen_valid(first)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (screens->state == STATE_LIVE || screens->state == STATE_CLOSING) {
        return PIXELICA_ERR_INVALID_ARG; /* preserve live screens and callback state */
    }
    memset(screens, 0, sizeof *screens);
    screens->state = STATE_LIVE;
    screens->stack[0] = *first;
    screens->depth = 1u;
    screens->projected = 1u;

    screens->busy = true;
    call(screens, first, first->enter);
    screens->busy = false;
    flush(screens);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_screens_change(PIXELICA_ScreenManager *screens,
                                        const PIXELICA_Screen *next)
{
    return request(screens, OP_CHANGE, next);
}

PIXELICA_Status pixelica_screens_push(PIXELICA_ScreenManager *screens,
                                      const PIXELICA_Screen *next)
{
    return request(screens, OP_PUSH, next);
}

PIXELICA_Status pixelica_screens_pop(PIXELICA_ScreenManager *screens)
{
    return request(screens, OP_POP, NULL);
}

PIXELICA_Status pixelica_screens_update(PIXELICA_ScreenManager *screens, float dt)
{
    PIXELICA_Screen top;

    if (!is_live(screens) || screens->busy || !(dt >= 0.0f && dt <= FLT_MAX)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    flush(screens);

    top = screens->stack[screens->depth - 1u];
    screens->busy = true;
    if (top.update != NULL) {
        top.update(screens, top.userdata, dt);
    }
    screens->busy = false;
    flush(screens);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_screens_draw(PIXELICA_ScreenManager *screens, void *target)
{
    uint32_t first;
    uint32_t i;

    if (!is_live(screens) || screens->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    first = screens->depth - 1u;
    while (first > 0u && (screens->stack[first].flags & PIXELICA_SCREEN_TRANSPARENT) != 0u) {
        --first;
    }
    screens->busy = true;
    for (i = first; i < screens->depth; ++i) {
        PIXELICA_Screen s = screens->stack[i];

        if (s.draw != NULL) {
            s.draw(screens, s.userdata, target);
        }
    }
    screens->busy = false;
    flush(screens);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_screens_shutdown(PIXELICA_ScreenManager *screens)
{
    uint32_t i;

    if (!is_live(screens) || screens->busy) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    screens->state = STATE_CLOSING;
    screens->busy = true;
    for (i = screens->depth; i-- > 0u;) {
        PIXELICA_Screen s = screens->stack[i];

        call(screens, &s, s.leave);
    }
    memset(screens, 0, sizeof *screens);
    return PIXELICA_OK;
}
