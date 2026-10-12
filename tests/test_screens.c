/* Screen manager: lifecycle order, deferred transitions, capacity, invalid args. */
#include <float.h>
#include <stdbool.h>
#include <string.h>

#include "pixelica/screens.h"
#include "test_util.h"

typedef PIXELICA_ScreenManager Mgr;

enum { REQ_NONE = 0, REQ_CHANGE, REQ_PUSH, REQ_POP };

typedef struct Req {
    int kind;
    const PIXELICA_Screen *to;
    bool repeat;
    PIXELICA_Status st;
} Req;

typedef struct Ctx {
    const char *name;
    Req on_enter, on_leave, on_update, on_draw;
    void *last_target;
    float last_dt;
} Ctx;

static char g_log[1024];
static PIXELICA_Status g_st[8];
static int g_leaves;

static void log_ev(const char *name, const char *ev)
{
    size_t n = strlen(g_log);

    snprintf(g_log + n, sizeof g_log - n, "%s.%s;", name, ev);
}

static bool log_is(const char *expected)
{
    bool ok = strcmp(g_log, expected) == 0;

    if (!ok) {
        fprintf(stderr, "log mismatch:\n  got      %s\n  expected %s\n", g_log, expected);
    }
    g_log[0] = '\0';
    return ok;
}

static void fire(Mgr *m, Ctx *c, Req *r)
{
    if (r->kind == REQ_NONE) {
        return;
    }
    if (r->kind == REQ_CHANGE) {
        r->st = pixelica_screens_change(m, r->to);
    } else if (r->kind == REQ_PUSH) {
        r->st = pixelica_screens_push(m, r->to);
    } else {
        r->st = pixelica_screens_pop(m);
    }
    log_ev(c->name, "req");
    if (!r->repeat) {
        r->kind = REQ_NONE;
    }
}

static void cb_enter(Mgr *m, void *ud)
{
    Ctx *c = ud;

    log_ev(c->name, "enter");
    fire(m, c, &c->on_enter);
}

static void cb_leave(Mgr *m, void *ud)
{
    Ctx *c = ud;

    log_ev(c->name, "leave");
    fire(m, c, &c->on_leave);
}

static void cb_pause(Mgr *m, void *ud)
{
    (void)m;
    log_ev(((Ctx *)ud)->name, "pause");
}

static void cb_resume(Mgr *m, void *ud)
{
    (void)m;
    log_ev(((Ctx *)ud)->name, "resume");
}

static void cb_update(Mgr *m, void *ud, float dt)
{
    Ctx *c = ud;

    c->last_dt = dt;
    log_ev(c->name, "update");
    fire(m, c, &c->on_update);
}

static void cb_draw(Mgr *m, void *ud, void *target)
{
    Ctx *c = ud;

    c->last_target = target;
    log_ev(c->name, "draw");
    fire(m, c, &c->on_draw);
}

static void make(PIXELICA_Screen *s, Ctx *c, const char *name, uint32_t flags)
{
    memset(c, 0, sizeof *c);
    c->name = name;
    s->enter = cb_enter;
    s->leave = cb_leave;
    s->pause = cb_pause;
    s->resume = cb_resume;
    s->update = cb_update;
    s->draw = cb_draw;
    s->userdata = c;
    s->flags = flags;
}

static void test_flow(void)
{
    Mgr m;
    Ctx title, play, menu, result;
    PIXELICA_Screen title_s, play_s, menu_s, result_s;
    int target = 0;

    make(&title_s, &title, "title", 0);
    make(&play_s, &play, "play", 0);
    make(&menu_s, &menu, "menu", PIXELICA_SCREEN_TRANSPARENT);
    make(&result_s, &result, "result", 0);
    memset(&m, 0, sizeof m);

    CHECK_EQ_LONG(pixelica_screens_init(&m, &title_s), PIXELICA_OK);
    CHECK(log_is("title.enter;"));

    /* TITLE -> PLAY: the request is made inside update, applied after it. */
    title.on_update = (Req){REQ_CHANGE, &play_s, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.25f), PIXELICA_OK);
    CHECK_EQ_LONG(title.on_update.st, PIXELICA_OK);
    CHECK_NEAR(title.last_dt, 0.25, 0.0);
    CHECK(log_is("title.update;title.req;title.leave;play.enter;"));

    /* PLAY -> PAUSE overlay: play is paused, not left. */
    play.on_update = (Req){REQ_PUSH, &menu_s, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK(log_is("play.update;play.req;play.pause;menu.enter;"));

    /* Transparent overlay: both drawn, bottom first; target passed through. */
    CHECK_EQ_LONG(pixelica_screens_draw(&m, &target), PIXELICA_OK);
    CHECK(log_is("play.draw;menu.draw;"));
    CHECK(play.last_target == &target && menu.last_target == &target);

    /* Only the overlay updates. */
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.1f), PIXELICA_OK);
    CHECK(log_is("menu.update;"));

    /* RESUME. */
    menu.on_update = (Req){REQ_POP, NULL, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.1f), PIXELICA_OK);
    CHECK(log_is("menu.update;menu.req;menu.leave;play.resume;"));

    /* PLAY -> RESULT; opaque screen draws alone. */
    play.on_update = (Req){REQ_CHANGE, &result_s, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.1f), PIXELICA_OK);
    CHECK(log_is("play.update;play.req;play.leave;result.enter;"));
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_OK);
    CHECK(log_is("result.draw;"));

    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK(log_is("result.leave;"));
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is(""));
}

static void test_opaque_draw_and_order(void)
{
    Mgr m;
    Ctx a, b, c;
    PIXELICA_Screen a_s, b_s, c_s;

    make(&a_s, &a, "a", 0);
    make(&b_s, &b, "b", 0);
    make(&c_s, &c, "c", PIXELICA_SCREEN_TRANSPARENT);
    CHECK_EQ_LONG(pixelica_screens_init(&m, &a_s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_push(&m, &b_s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_push(&m, &c_s), PIXELICA_OK);
    CHECK(log_is("a.enter;a.pause;b.enter;b.pause;c.enter;"));

    /* c is transparent, b is opaque: a is hidden and not drawn. */
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_OK);
    CHECK(log_is("b.draw;c.draw;"));

    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK(log_is("c.leave;b.leave;a.leave;"));
}

static void test_deferred_in_draw_and_enter(void)
{
    Mgr m;
    Ctx base, a, b, play, result;
    PIXELICA_Screen base_s, a_s, b_s, play_s, result_s;

    /* Transition requested inside draw: applied after the draw pass. */
    make(&play_s, &play, "play", 0);
    make(&result_s, &result, "result", 0);
    CHECK_EQ_LONG(pixelica_screens_init(&m, &play_s), PIXELICA_OK);
    log_is("play.enter;");
    play.on_draw = (Req){REQ_CHANGE, &result_s, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_OK);
    CHECK(log_is("play.draw;play.req;play.leave;result.enter;"));
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_OK);
    CHECK(log_is("result.draw;"));
    pixelica_screens_shutdown(&m);
    log_is("result.leave;");

    /* A and B redirect to each other from enter, forever. One batch per
     * boundary: every call returns, and the stack stays one screen deep. */
    make(&base_s, &base, "base", 0);
    make(&a_s, &a, "A", 0);
    make(&b_s, &b, "B", 0);
    a.on_enter = (Req){REQ_CHANGE, &b_s, true, PIXELICA_OK};
    b.on_enter = (Req){REQ_CHANGE, &a_s, true, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_init(&m, &base_s), PIXELICA_OK);
    log_is("base.enter;");
    CHECK_EQ_LONG(pixelica_screens_change(&m, &a_s), PIXELICA_OK);
    CHECK(log_is("base.leave;A.enter;A.req;"));
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK(log_is("A.leave;B.enter;B.req;B.update;B.leave;A.enter;A.req;"));
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK(log_is("A.leave;"));
}

static void spam_update(Mgr *m, void *ud, float dt)
{
    int i;

    (void)dt;
    for (i = 0; i < 6; ++i) {
        g_st[i] = pixelica_screens_change(m, ud);
    }
}

static void pop2_update(Mgr *m, void *ud, float dt)
{
    (void)ud;
    (void)dt;
    g_st[0] = pixelica_screens_pop(m);
    g_st[1] = pixelica_screens_pop(m);
}

static void reentrant_update(Mgr *m, void *ud, float dt)
{
    (void)dt;
    g_st[0] = pixelica_screens_update(m, 0.0f);
    g_st[1] = pixelica_screens_draw(m, NULL);
    g_st[2] = pixelica_screens_shutdown(m);
    g_st[3] = pixelica_screens_init(m, ud);
}

static void count_leave(Mgr *m, void *ud)
{
    (void)m;
    (void)ud;
    ++g_leaves;
}

static void test_capacity(void)
{
    Mgr m;
    Ctx a, b;
    PIXELICA_Screen a_s, b_s, spam_s, pop2_s, counter_s;
    int i;

    make(&a_s, &a, "a", 0);
    make(&b_s, &b, "b", 0);

    /* Queue: 4 accepted, the rest CAPACITY and ignored. */
    memset(&spam_s, 0, sizeof spam_s);
    spam_s.update = spam_update;
    spam_s.userdata = &b_s;
    CHECK_EQ_LONG(pixelica_screens_init(&m, &spam_s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    for (i = 0; i < 4; ++i) {
        CHECK_EQ_LONG(g_st[i], PIXELICA_OK);
    }
    CHECK_EQ_LONG(g_st[4], PIXELICA_ERR_CAPACITY);
    CHECK_EQ_LONG(g_st[5], PIXELICA_ERR_CAPACITY);
    b_s.userdata = &b;
    CHECK(log_is("b.enter;b.leave;b.enter;b.leave;b.enter;b.leave;b.enter;"));
    pixelica_screens_shutdown(&m);
    log_is("b.leave;");

    /* Stack: STACK_MAX screens, the next push is CAPACITY and changes nothing. */
    memset(&counter_s, 0, sizeof counter_s);
    counter_s.leave = count_leave;
    CHECK_EQ_LONG(pixelica_screens_init(&m, &counter_s), PIXELICA_OK);
    for (i = 1; i < (int)PIXELICA_SCREEN_STACK_MAX; ++i) {
        CHECK_EQ_LONG(pixelica_screens_push(&m, &counter_s), PIXELICA_OK);
    }
    CHECK_EQ_LONG(pixelica_screens_push(&m, &counter_s), PIXELICA_ERR_CAPACITY);
    g_leaves = 0;
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK_EQ_LONG(g_leaves, (long long)PIXELICA_SCREEN_STACK_MAX);

    /* Pop-last is rejected, at top level and for queued pops. */
    CHECK_EQ_LONG(pixelica_screens_init(&m, &a_s), PIXELICA_OK);
    log_is("a.enter;");
    CHECK_EQ_LONG(pixelica_screens_pop(&m), PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is(""));
    CHECK_EQ_LONG(pixelica_screens_push(&m, &b_s), PIXELICA_OK);
    log_is("a.pause;b.enter;");
    pop2_s = b_s;
    pop2_s.update = pop2_update;
    pixelica_screens_shutdown(&m);
    log_is("b.leave;a.leave;");
    pixelica_screens_init(&m, &a_s);
    pixelica_screens_push(&m, &pop2_s);
    log_is("a.enter;a.pause;b.enter;");
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(g_st[0], PIXELICA_OK);
    CHECK_EQ_LONG(g_st[1], PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is("b.leave;a.resume;"));
    pixelica_screens_shutdown(&m);
    log_is("a.leave;");
}

static void test_invalid_args(void)
{
    Mgr m, zero;
    Ctx a, t, other;
    PIXELICA_Screen a_s, t_s, other_s, bare, bad;

    make(&a_s, &a, "a", 0);
    make(&other_s, &other, "o", 0);
    memset(&zero, 0, sizeof zero);
    memset(&m, 0, sizeof m);

    /* Failed init leaves the manager untouched. */
    bad = a_s;
    bad.flags = 2u;
    CHECK_EQ_LONG(pixelica_screens_init(NULL, &a_s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_init(&m, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_init(&m, &bad), PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&m, &zero, sizeof m) == 0);

    /* Never-initialised or NULL manager. */
    CHECK_EQ_LONG(pixelica_screens_change(&m, &a_s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_push(&m, &a_s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_pop(&m), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_change(NULL, &a_s), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_pop(NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(NULL, 0.0f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_draw(NULL, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_shutdown(NULL), PIXELICA_ERR_INVALID_ARG);

    CHECK_EQ_LONG(pixelica_screens_init(&m, &a_s), PIXELICA_OK);
    CHECK(log_is("a.enter;"));
    CHECK_EQ_LONG(pixelica_screens_init(&m, &a_s), PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is(""));
    CHECK_EQ_LONG(pixelica_screens_change(&m, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_push(&m, NULL), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_change(&m, &bad), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_push(&m, &bad), PIXELICA_ERR_INVALID_ARG);

    /* Bad dt: INVALID_ARG, no callback. Zero and -0 are fine. */
    CHECK_EQ_LONG(pixelica_screens_update(&m, -0.001f), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, pixelica_t_nan()), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, pixelica_t_inf()), PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, -pixelica_t_inf()), PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is(""));
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_update(&m, -0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_update(&m, FLT_MAX), PIXELICA_OK);
    CHECK(log_is("a.update;a.update;a.update;"));

    /* A screen with every callback NULL is valid. */
    memset(&bare, 0, sizeof bare);
    CHECK_EQ_LONG(pixelica_screens_push(&m, &bare), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_draw(&m, NULL), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_pop(&m), PIXELICA_OK);
    CHECK(log_is("a.pause;a.resume;"));
    pixelica_screens_shutdown(&m);
    log_is("a.leave;");

    /* Lifecycle calls from inside a callback are rejected, manager survives. */
    memset(&t_s, 0, sizeof t_s);
    t_s.update = reentrant_update;
    t_s.userdata = &other_s;
    CHECK_EQ_LONG(pixelica_screens_init(&m, &t_s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(g_st[0], PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(g_st[1], PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(g_st[2], PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(g_st[3], PIXELICA_ERR_INVALID_ARG);
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK(log_is(""));

    /* Requests made from leave during shutdown are rejected and dropped. */
    make(&t_s, &t, "t", 0);
    t.on_leave = (Req){REQ_CHANGE, &a_s, false, PIXELICA_OK};
    CHECK_EQ_LONG(pixelica_screens_init(&m, &t_s), PIXELICA_OK);
    CHECK_EQ_LONG(pixelica_screens_shutdown(&m), PIXELICA_OK);
    CHECK_EQ_LONG(t.on_leave.st, PIXELICA_ERR_INVALID_ARG);
    CHECK(log_is("t.enter;t.leave;t.req;"));
    CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_ERR_INVALID_ARG);

    /* The descriptor is copied: it may die right after the call. */
    {
        PIXELICA_Screen tmp = a_s;

        CHECK_EQ_LONG(pixelica_screens_init(&m, &tmp), PIXELICA_OK);
        memset(&tmp, 0xFF, sizeof tmp);
        CHECK_EQ_LONG(pixelica_screens_update(&m, 0.0f), PIXELICA_OK);
        pixelica_screens_shutdown(&m);
        CHECK(log_is("a.enter;a.update;a.leave;"));
    }
}

int main(void)
{
    test_flow();
    test_opaque_draw_and_order();
    test_deferred_in_draw_and_enter();
    test_capacity();
    test_invalid_args();
    return pixelica_t_finish("test_screens");
}
