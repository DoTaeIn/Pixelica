/*
 * hello_online drawing. See online_view.h. Screen-space fill_rect and draw_text
 * only: discs are stacks of one-pixel-high rectangles, so the example needs no
 * texture or atlas data. The bitmap font comes from the shared 2D demo art
 * (examples/common2d/demo_art.h, upper case only).
 */
#include "online_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "demo_art.h"

#define TEXT_MAX 64u

static const uint16_t k_player_colors[HOL_MAX_PLAYERS] = {
    PIXELICA_RGB565(235, 80, 80),  PIXELICA_RGB565(80, 150, 245), PIXELICA_RGB565(90, 215, 110),
    PIXELICA_RGB565(245, 210, 70), PIXELICA_RGB565(215, 100, 235), PIXELICA_RGB565(80, 220, 225),
    PIXELICA_RGB565(245, 150, 60), PIXELICA_RGB565(225, 225, 235)};

bool hol_draw_init(HolDraw *draw, uint16_t *pixels)
{
    PIXELICA2D_Surface surface;

    if (draw == NULL || pixels == NULL) {
        return false;
    }
    surface.pixels = pixels;
    surface.width = (uint16_t)HOL_VIEW_WIDTH;
    surface.height = (uint16_t)HOL_VIEW_HEIGHT;
    surface.stride = HOL_VIEW_WIDTH;
    surface.depth = NULL;
    surface.depth_stride = 0u;
    draw->font = demo_art_font();
    return pixelica2d_init(&draw->ctx, &surface) == PIXELICA_OK;
}

static PIXELICA_Status rect(HolDraw *d, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color,
                       uint8_t opacity)
{
    PIXELICA2D_Rect r;

    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return pixelica2d_fill_rect(&d->ctx, &r, color, opacity);
}

static PIXELICA_Status disc(HolDraw *d, float cx, float cy, int32_t radius, uint16_t color,
                       uint8_t opacity)
{
    const int32_t x = (int32_t)cx;
    const int32_t y = (int32_t)cy;
    int32_t dy;

    for (dy = -radius; dy <= radius; ++dy) {
        const int32_t half = (int32_t)sqrtf((float)(radius * radius - dy * dy) + 0.5f);

        DEMO_TRY(rect(d, x - half, y + dy, half * 2 + 1, 1, color, opacity));
    }
    return PIXELICA_OK;
}

/* Upper-cases ASCII, replaces everything the font cannot show with '?'. */
static size_t to_font_text(char *dst, size_t cap, const char *src)
{
    size_t n = 0u;

    for (; *src != '\0' && n + 1u < cap; ++src) {
        char c = *src;

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        dst[n++] = (c < ' ' || c > 'Z') ? '?' : c;
    }
    dst[n] = '\0';
    return n;
}

static PIXELICA_Status text(HolDraw *d, int32_t x, int32_t y, const char *s, uint16_t color)
{
    char buf[TEXT_MAX];
    const size_t n = to_font_text(buf, sizeof buf, s);

    return n == 0u ? PIXELICA_OK : pixelica2d_draw_text(&d->ctx, &d->font, buf, n, x, y, color, 255u);
}

static PIXELICA_Status text_centered(HolDraw *d, int32_t y, const char *s, uint16_t color)
{
    char buf[TEXT_MAX];
    const size_t n = to_font_text(buf, sizeof buf, s);

    return text(d, ((int32_t)HOL_VIEW_WIDTH - (int32_t)n * DEMO_FONT_ADVANCE) / 2, y, buf, color);
}

static PIXELICA_Status draw_arena(HolDraw *d)
{
    int32_t cx;
    int32_t cy;

    DEMO_TRY(pixelica2d_clear(&d->ctx, PIXELICA_RGB565(22, 30, 52)));
    for (cy = 1; cy < 12; ++cy) {
        for (cx = 0; cx < 16; ++cx) {
            if (((cx + cy) & 1) == 0) {
                DEMO_TRY(rect(d, cx * 16, cy * 16, 16, 16, PIXELICA_RGB565(28, 38, 64), 255u));
            }
        }
    }
    return PIXELICA_OK;
}

static PIXELICA_Status draw_entities(HolDraw *d, const HolSession *s)
{
    const HolView *v = &s->view;
    const uint16_t star_color = PIXELICA_RGB565(255, 226, 90);
    size_t i;

    for (i = 0u; i < v->star_count; ++i) {
        DEMO_TRY(disc(d, v->stars[i].x, v->stars[i].y, 5, PIXELICA_RGB565(200, 150, 30), 255u));
        DEMO_TRY(disc(d, v->stars[i].x, v->stars[i].y, 3, star_color, 255u));
    }
    for (i = 0u; i < v->hazard_count; ++i) {
        DEMO_TRY(disc(d, v->hazards[i].x, v->hazards[i].y, 7, PIXELICA_RGB565(120, 20, 40), 255u));
        DEMO_TRY(disc(d, v->hazards[i].x, v->hazards[i].y, 4, PIXELICA_RGB565(240, 70, 80), 255u));
    }
    for (i = 0u; i < v->player_count; ++i) {
        const HolPlayer *p = &v->players[i];
        const HolVec at = s->shown_valid[p->id] ? s->shown[p->id] : p->pos;
        uint16_t color = k_player_colors[p->id];

        if (p->stun != 0u && ((s->frame >> 2) & 1u) != 0u) {
            continue; /* blink while stunned */
        }
        if (p->stun != 0u) {
            color = PIXELICA_RGB565(120, 120, 130);
        }
        if (p->dash != 0u) {
            DEMO_TRY(disc(d, at.x, at.y, 10, color, 90u));
        }
        if (p->id == s->self_id) {
            DEMO_TRY(disc(d, at.x, at.y, 8, PIXELICA_RGB565(255, 255, 255), 255u));
        }
        DEMO_TRY(disc(d, at.x, at.y, 6, color, 255u));
        if (s->names[p->id][0] != '\0') {
            char tag[8];
            const size_t n = to_font_text(tag, sizeof tag, s->names[p->id]);
            int32_t y = (int32_t)at.y - 18;

            if (y < 17) {
                y = 17;
            }
            DEMO_TRY(text(d, (int32_t)at.x - (int32_t)n * 3, y, tag, PIXELICA_RGB565(235, 235, 245)));
        }
    }
    return PIXELICA_OK;
}

static PIXELICA_Status draw_hud(HolDraw *d, const HolSession *s)
{
    const HolView *v = &s->view;
    char line[TEXT_MAX];
    int32_t seconds_left = 0;
    int32_t score = 0;
    size_t i;

    DEMO_TRY(rect(d, 0, 0, (int32_t)HOL_VIEW_WIDTH, 16, PIXELICA_RGB565(8, 10, 24), 255u));
    if (v->phase == HOL_PHASE_PLAYING) {
        seconds_left = (int32_t)((v->round_ticks - v->tick + HOL_TICK_HZ - 1u) / HOL_TICK_HZ);
    }
    (void)hol_view_board_score(v, s->self_id, &score);
    (void)snprintf(line, sizeof line, "%s %d", v->mode == HOL_MODE_COOP ? "COOP TEAM" : "VERSUS",
                   (int)score);
    DEMO_TRY(text(d, 4, 4, line, PIXELICA_RGB565(255, 255, 255)));
    (void)snprintf(line, sizeof line, "T %d", (int)seconds_left);
    DEMO_TRY(text(d, 110, 4, line, PIXELICA_RGB565(255, 226, 90)));
    (void)snprintf(line, sizeof line, "ROOM %s", s->room_code);
    DEMO_TRY(text(d, 256 - 4 - (int32_t)strlen(line) * DEMO_FONT_ADVANCE, 4, line,
                  PIXELICA_RGB565(120, 220, 255)));

    /* scoreboard strip: one colored square + score per player */
    for (i = 0u; i < v->player_count; ++i) {
        const int32_t x = 4 + (int32_t)i * 31;

        DEMO_TRY(rect(d, x, 181, 7, 7, k_player_colors[v->players[i].id], 255u));
        (void)snprintf(line, sizeof line, "%u", (unsigned)v->players[i].score);
        DEMO_TRY(text(d, x + 10, 181, line, PIXELICA_RGB565(235, 235, 245)));
    }
    return PIXELICA_OK;
}

static PIXELICA_Status draw_board(HolDraw *d, const HolSession *s, int32_t y)
{
    const PIXELICA_OnlineLeaderboard *lb = &s->leaderboard;
    char line[TEXT_MAX];
    size_t i;
    size_t rows;

    (void)snprintf(line, sizeof line, "BOARD %s", s->board);
    DEMO_TRY(text_centered(d, y, line, PIXELICA_RGB565(120, 220, 255)));
    if (lb->generation == 0u) {
        return text_centered(d, y + 10, "LOADING", PIXELICA_RGB565(170, 170, 190));
    }
    if (lb->status != 0) {
        return text_centered(d, y + 10, pixelica_online_status_string(lb->status),
                             PIXELICA_RGB565(240, 90, 90));
    }
    if (lb->count == 0u) {
        return text_centered(d, y + 10, "EMPTY", PIXELICA_RGB565(170, 170, 190));
    }
    rows = lb->count < 5u ? lb->count : 5u;
    for (i = 0u; i < rows; ++i) {
        char name[12];

        (void)to_font_text(name, sizeof name, lb->entries[i].name);
        (void)snprintf(line, sizeof line, "%u %-10s %d", (unsigned)(i + 1u), name,
                       (int)lb->entries[i].score);
        DEMO_TRY(text(d, 60, y + 10 + (int32_t)i * 9, line, PIXELICA_RGB565(235, 235, 245)));
    }
    return PIXELICA_OK;
}

static PIXELICA_Status draw_overlay(HolDraw *d, const HolSession *s, bool autopilot)
{
    const HolView *v = &s->view;
    char line[TEXT_MAX];
    size_t i;
    int32_t y;

    if (!s->have_view) {
        DEMO_TRY(rect(d, 40, 70, 176, 36, PIXELICA_RGB565(8, 10, 24), 210u));
        return text_centered(d, 84, "CONNECTING", PIXELICA_RGB565(255, 226, 90));
    }
    if (v->phase == HOL_PHASE_LOBBY) {
        DEMO_TRY(rect(d, 24, 22, 208, 156, PIXELICA_RGB565(8, 10, 24), 215u));
        DEMO_TRY(text_centered(d, 28, "HELLO ONLINE", PIXELICA_RGB565(255, 226, 90)));
        (void)snprintf(line, sizeof line, "ROOM CODE %s", s->room_code);
        DEMO_TRY(text_centered(d, 42, line, PIXELICA_RGB565(120, 220, 255)));
        (void)snprintf(line, sizeof line, "%s  %u/%u PLAYERS",
                       v->mode == HOL_MODE_COOP ? "COOP" : "VERSUS", (unsigned)s->player_count,
                       (unsigned)s->max_players);
        DEMO_TRY(text_centered(d, 54, line, PIXELICA_RGB565(235, 235, 245)));
        y = 70;
        for (i = 0u; i < HOL_MAX_PLAYERS; ++i) {
            if (s->names[i][0] == '\0') {
                continue;
            }
            DEMO_TRY(rect(d, 70, y, 6, 6, k_player_colors[i], 255u));
            (void)snprintf(line, sizeof line, "%s%s", s->names[i], i == s->self_id ? " (YOU)" : "");
            DEMO_TRY(text(d, 82, y, line, PIXELICA_RGB565(235, 235, 245)));
            y += 9;
        }
        DEMO_TRY(text_centered(d, 150, s->is_host ? "ENTER = START ROUND" : "WAITING FOR HOST",
                               PIXELICA_RGB565(90, 215, 110)));
        DEMO_TRY(text_centered(d, 162, autopilot ? "AUTOPILOT ON" : "ARROWS/WASD MOVE  SPACE DASH",
                               PIXELICA_RGB565(170, 170, 190)));
        return PIXELICA_OK;
    }
    if (v->phase == HOL_PHASE_FINISHED) {
        const int leader = hol_view_leader(v);

        DEMO_TRY(rect(d, 24, 22, 208, 156, PIXELICA_RGB565(8, 10, 24), 215u));
        DEMO_TRY(text_centered(d, 28, "ROUND OVER", PIXELICA_RGB565(255, 226, 90)));
        if (v->mode == HOL_MODE_COOP) {
            (void)snprintf(line, sizeof line, "TEAM SCORE %u", (unsigned)v->team_score);
        } else if (leader >= 0 && s->names[leader][0] != '\0') {
            (void)snprintf(line, sizeof line, "WINNER %s", s->names[leader]);
        } else {
            (void)snprintf(line, sizeof line, "NO WINNER");
        }
        DEMO_TRY(text_centered(d, 42, line, PIXELICA_RGB565(235, 235, 245)));
        if (s->submitted && s->submit_error != 0) {
            (void)snprintf(line, sizeof line, "SCORE NOT SENT %s", pixelica_online_status_string(s->submit_error));
            DEMO_TRY(text_centered(d, 54, line, PIXELICA_RGB565(240, 90, 90)));
        } else if (s->submitted) {
            (void)snprintf(line, sizeof line, "YOUR SCORE %d SENT", (int)s->submitted_score);
            DEMO_TRY(text_centered(d, 54, line, PIXELICA_RGB565(90, 215, 110)));
        } else {
            DEMO_TRY(text_centered(d, 54, "SENDING SCORE", PIXELICA_RGB565(170, 170, 190)));
        }
        DEMO_TRY(draw_board(d, s, 72));
        if (s->is_host) {
            DEMO_TRY(text_centered(d, 160, "ENTER = PLAY AGAIN", PIXELICA_RGB565(90, 215, 110)));
        }
        return PIXELICA_OK;
    }
    return PIXELICA_OK;
}

PIXELICA_Status hol_draw_frame(HolDraw *draw, const HolSession *session, bool autopilot)
{
    DEMO_TRY(draw_arena(draw));
    if (session->have_view) {
        DEMO_TRY(draw_entities(draw, session));
        DEMO_TRY(draw_hud(draw, session));
    }
    DEMO_TRY(draw_overlay(draw, session, autopilot));
    if (!session->connected) {
        DEMO_TRY(rect(draw, 0, 90, (int32_t)HOL_VIEW_WIDTH, 22, PIXELICA_RGB565(120, 20, 40), 235u));
        DEMO_TRY(text_centered(draw, 97, "DISCONNECTED - RESTART TO REJOIN", PIXELICA_RGB565(255, 255, 255)));
    }
    return PIXELICA_OK;
}
