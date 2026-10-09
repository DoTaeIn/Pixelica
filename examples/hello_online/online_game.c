/*
 * hello_online game logic: wire codec, host simulation, bot. See online_game.h.
 * No networking, no renderer, no platform code, no allocation, no globals.
 */
#include "online_game.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* The codec moves float bits with memcpy; it is only correct for IEEE binary32. */
typedef char hol_assert_float32[(sizeof(float) == 4u && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
                                  FLT_MAX_EXP == 128 && FLT_MIN_EXP == -125)
                                    ? 1
                                    : -1];

#define HOL_VERSION 1u
#define HOL_TOP 16.0f /* top rows belong to the HUD, nothing lives there */
#define HOL_MOVE_SPEED 1.5f   /* pixels per tick = 90 px/s */
#define HOL_DASH_SPEED 3.6f
#define HOL_DASH_TICKS 12u
#define HOL_DASH_COOLDOWN 60u
#define HOL_STUN_TICKS 45u
#define HOL_DIAGONAL 0.70710678f
#define HOL_MAX_SCORE 65535u

typedef char hol_assert_state_fits[(19u + 1u + HOL_MAX_STARS * 8u + 1u + HOL_MAX_HAZARDS * 8u + 1u +
                                     HOL_MAX_PLAYERS * 13u <= HOL_STATE_MAX)
                                       ? 1
                                       : -1];

static const HolVec k_spawn[HOL_MAX_PLAYERS] = {
    {40.0f, 44.0f},  {216.0f, 156.0f}, {216.0f, 44.0f}, {40.0f, 156.0f},
    {128.0f, 34.0f}, {128.0f, 170.0f}, {30.0f, 100.0f}, {226.0f, 100.0f}};

/* ----------------------------------------------------------- byte helpers */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void put_f32(uint8_t *p, float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof bits);
    put_u32(p, bits);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static float get_f32(const uint8_t *p)
{
    const uint32_t bits = get_u32(p);
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

/* NaN fails every comparison, so this also rejects NaN. Infinity is out of range. */
static bool in_range(float v, float lo, float hi)
{
    return v >= lo && v <= hi;
}

static bool valid_point(HolVec p)
{
    return in_range(p.x, 0.0f, HOL_ARENA_W) && in_range(p.y, 0.0f, HOL_ARENA_H);
}

/* ------------------------------------------------------------------ codec */

size_t hol_view_encode(const HolView *view, uint8_t *out, size_t cap)
{
    size_t at = 0u;
    size_t i;
    size_t need;

    if (view == NULL || out == NULL || view->star_count > HOL_MAX_STARS ||
        view->hazard_count > HOL_MAX_HAZARDS || view->player_count > HOL_MAX_PLAYERS) {
        return 0u;
    }
    need = 19u + 1u + (size_t)view->star_count * 8u + 1u + (size_t)view->hazard_count * 8u + 1u +
           (size_t)view->player_count * 13u;
    if (cap < need) {
        return 0u;
    }
    out[at++] = (uint8_t)HOL_VERSION;
    out[at++] = (uint8_t)view->mode;
    out[at++] = (uint8_t)view->phase;
    put_u16(out + at, view->round);
    at += 2u;
    put_u32(out + at, view->seed);
    at += 4u;
    put_u32(out + at, view->tick);
    at += 4u;
    put_u32(out + at, view->round_ticks);
    at += 4u;
    put_u16(out + at, view->team_score);
    at += 2u;
    out[at++] = view->star_count;
    for (i = 0u; i < view->star_count; ++i) {
        put_f32(out + at, view->stars[i].x);
        put_f32(out + at + 4u, view->stars[i].y);
        at += 8u;
    }
    out[at++] = view->hazard_count;
    for (i = 0u; i < view->hazard_count; ++i) {
        put_f32(out + at, view->hazards[i].x);
        put_f32(out + at + 4u, view->hazards[i].y);
        at += 8u;
    }
    out[at++] = view->player_count;
    for (i = 0u; i < view->player_count; ++i) {
        const HolPlayer *p = &view->players[i];

        out[at] = p->id;
        put_f32(out + at + 1u, p->pos.x);
        put_f32(out + at + 5u, p->pos.y);
        put_u16(out + at + 9u, p->score);
        out[at + 11u] = p->stun;
        out[at + 12u] = p->dash;
        at += 13u;
    }
    return at == need ? at : 0u;
}

bool hol_view_decode(const uint8_t *data, size_t size, HolView *out)
{
    HolView v;
    size_t at = 0u;
    size_t i;
    int last_id = -1;

    if (data == NULL || out == NULL || size < 19u + 3u) {
        return false;
    }
    memset(&v, 0, sizeof v);
    if (data[at++] != HOL_VERSION) {
        return false;
    }
    if (data[at] != (uint8_t)HOL_MODE_COOP && data[at] != (uint8_t)HOL_MODE_VERSUS) {
        return false;
    }
    v.mode = (HolMode)data[at++];
    if (data[at] > (uint8_t)HOL_PHASE_FINISHED) {
        return false;
    }
    v.phase = (HolPhase)data[at++];
    v.round = get_u16(data + at);
    at += 2u;
    v.seed = get_u32(data + at);
    at += 4u;
    v.tick = get_u32(data + at);
    at += 4u;
    v.round_ticks = get_u32(data + at);
    at += 4u;
    v.team_score = get_u16(data + at);
    at += 2u;
    if (v.round_ticks == 0u || v.round_ticks > HOL_MAX_ROUND_SECONDS * HOL_TICK_HZ ||
        v.tick > v.round_ticks) {
        return false;
    }

    v.star_count = data[at++];
    if (v.star_count > HOL_MAX_STARS || size - at < (size_t)v.star_count * 8u + 2u) {
        return false;
    }
    for (i = 0u; i < v.star_count; ++i) {
        v.stars[i].x = get_f32(data + at);
        v.stars[i].y = get_f32(data + at + 4u);
        at += 8u;
        if (!valid_point(v.stars[i])) {
            return false;
        }
    }
    v.hazard_count = data[at++];
    if (v.hazard_count > HOL_MAX_HAZARDS || size - at < (size_t)v.hazard_count * 8u + 1u) {
        return false;
    }
    for (i = 0u; i < v.hazard_count; ++i) {
        v.hazards[i].x = get_f32(data + at);
        v.hazards[i].y = get_f32(data + at + 4u);
        at += 8u;
        if (!valid_point(v.hazards[i])) {
            return false;
        }
    }
    v.player_count = data[at++];
    if (v.player_count > HOL_MAX_PLAYERS || size - at != (size_t)v.player_count * 13u) {
        return false; /* exact length: no trailing bytes */
    }
    for (i = 0u; i < v.player_count; ++i) {
        HolPlayer *p = &v.players[i];

        p->id = data[at];
        p->pos.x = get_f32(data + at + 1u);
        p->pos.y = get_f32(data + at + 5u);
        p->score = get_u16(data + at + 9u);
        p->stun = data[at + 11u];
        p->dash = data[at + 12u];
        at += 13u;
        if (p->id >= HOL_MAX_PLAYERS || (int)p->id <= last_id || !valid_point(p->pos)) {
            return false;
        }
        last_id = (int)p->id;
    }
    *out = v;
    return true;
}

void hol_input_encode(uint8_t buttons, uint8_t out[HOL_INPUT_SIZE])
{
    out[0] = (uint8_t)HOL_VERSION;
    out[1] = (uint8_t)(buttons & HOL_IN_ALL);
}

bool hol_input_decode(const uint8_t *data, size_t size, uint8_t *buttons)
{
    if (data == NULL || buttons == NULL || size != HOL_INPUT_SIZE || data[0] != HOL_VERSION ||
        ((uint32_t)data[1] & ~HOL_IN_ALL) != 0u) {
        return false;
    }
    *buttons = data[1];
    return true;
}

void hol_record_encode(const HolView *view, uint8_t out[HOL_RECORD_SIZE])
{
    out[0] = (uint8_t)HOL_VERSION;
    out[1] = (uint8_t)view->mode;
    out[2] = view->player_count;
    put_u16(out + 3, (uint16_t)(view->round_ticks / HOL_TICK_HZ));
    put_u32(out + 5, view->seed);
}

bool hol_record_decode(const uint8_t *data, size_t size, HolRecord *out)
{
    if (data == NULL || out == NULL || size != HOL_RECORD_SIZE || data[0] != HOL_VERSION ||
        (data[1] != (uint8_t)HOL_MODE_COOP && data[1] != (uint8_t)HOL_MODE_VERSUS) ||
        data[2] == 0u || data[2] > HOL_MAX_PLAYERS) {
        return false;
    }
    out->mode = (HolMode)data[1];
    out->players = data[2];
    out->round_seconds = get_u16(data + 3);
    out->seed = get_u32(data + 5);
    return true;
}

/* ------------------------------------------------------------------ world */

static uint32_t rng_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static float rng_range(uint32_t *state, float lo, float hi)
{
    return lo + (float)(rng_next(state) & 0xFFFFu) * ((hi - lo) / 65535.0f);
}

static void place_star(HolWorld *w, size_t index)
{
    w->view.stars[index].x = rng_range(&w->rng, 14.0f, HOL_ARENA_W - 14.0f);
    w->view.stars[index].y = rng_range(&w->rng, HOL_TOP + 12.0f, HOL_ARENA_H - 14.0f);
}

static void reset_round(HolWorld *w, uint32_t seed)
{
    HolView *v = &w->view;
    size_t i;

    v->seed = seed;
    w->rng = seed ^ 0x9E3779B9u;
    v->tick = 0u;
    v->team_score = 0u;
    v->star_count = (uint8_t)HOL_MAX_STARS;
    for (i = 0u; i < HOL_MAX_STARS; ++i) {
        place_star(w, i);
    }
    v->hazard_count = (uint8_t)(v->mode == HOL_MODE_COOP ? HOL_MAX_HAZARDS : 0u);
    for (i = 0u; i < HOL_MAX_HAZARDS; ++i) {
        const float speed = 0.8f + 0.15f * (float)i;

        v->hazards[i].x = rng_range(&w->rng, 40.0f, HOL_ARENA_W - 40.0f);
        v->hazards[i].y = rng_range(&w->rng, HOL_TOP + 30.0f, HOL_ARENA_H - 30.0f);
        w->hazard_velocity[i].x = (rng_next(&w->rng) & 1u) != 0u ? speed : -speed;
        w->hazard_velocity[i].y = (rng_next(&w->rng) & 1u) != 0u ? speed * 0.7f : -speed * 0.7f;
    }
    for (i = 0u; i < v->player_count; ++i) {
        HolPlayer *p = &v->players[i];

        p->pos = k_spawn[p->id];
        p->score = 0u;
        p->stun = 0u;
        p->dash = 0u;
    }
    memset(w->dash_cooldown, 0, sizeof w->dash_cooldown);
    memset(w->facing, 0, sizeof w->facing);
}

void hol_world_init(HolWorld *world, HolMode mode, uint32_t round_seconds, uint32_t seed)
{
    memset(world, 0, sizeof *world);
    if (round_seconds < HOL_MIN_ROUND_SECONDS) {
        round_seconds = HOL_MIN_ROUND_SECONDS;
    }
    if (round_seconds > HOL_MAX_ROUND_SECONDS) {
        round_seconds = HOL_MAX_ROUND_SECONDS;
    }
    world->view.mode = mode == HOL_MODE_VERSUS ? HOL_MODE_VERSUS : HOL_MODE_COOP;
    world->view.phase = HOL_PHASE_LOBBY;
    world->view.round_ticks = round_seconds * HOL_TICK_HZ;
    reset_round(world, seed);
}

int hol_view_find_player(const HolView *view, uint8_t id)
{
    size_t i;

    for (i = 0u; i < view->player_count; ++i) {
        if (view->players[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

bool hol_world_set_players(HolWorld *world, const uint8_t *ids, size_t count)
{
    HolPlayer next[HOL_MAX_PLAYERS];
    size_t i;
    int last = -1;

    if (count > HOL_MAX_PLAYERS || (count > 0u && ids == NULL)) {
        return false;
    }
    for (i = 0u; i < count; ++i) {
        if (ids[i] >= HOL_MAX_PLAYERS || (int)ids[i] <= last) {
            return false;
        }
        last = (int)ids[i];
    }
    memset(next, 0, sizeof next);
    for (i = 0u; i < count; ++i) {
        const int old = hol_view_find_player(&world->view, ids[i]);

        if (old >= 0) {
            next[i] = world->view.players[old];
        } else {
            next[i].id = ids[i];
            next[i].pos = k_spawn[ids[i]];
            world->dash_cooldown[ids[i]] = 0u;
            world->facing[ids[i]].x = 0.0f;
            world->facing[ids[i]].y = 0.0f;
        }
    }
    memset(world->view.players, 0, sizeof world->view.players);
    for (i = 0u; i < count; ++i) {
        world->view.players[i] = next[i];
    }
    world->view.player_count = (uint8_t)count;
    return true;
}

void hol_world_start_round(HolWorld *world, uint32_t seed)
{
    reset_round(world, seed);
    world->view.round = (uint16_t)(world->view.round + 1u);
    world->view.phase = HOL_PHASE_PLAYING;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void clamp_player(HolPlayer *p)
{
    p->pos.x = clampf(p->pos.x, HOL_PLAYER_RADIUS, HOL_ARENA_W - HOL_PLAYER_RADIUS);
    p->pos.y = clampf(p->pos.y, HOL_TOP + HOL_PLAYER_RADIUS, HOL_ARENA_H - HOL_PLAYER_RADIUS);
}

static float dist2(HolVec a, HolVec b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;

    return dx * dx + dy * dy;
}

static void move_player(HolWorld *w, HolPlayer *p, uint8_t buttons)
{
    const int ix = (int)((buttons & HOL_IN_RIGHT) != 0u) - (int)((buttons & HOL_IN_LEFT) != 0u);
    const int iy = (int)((buttons & HOL_IN_DOWN) != 0u) - (int)((buttons & HOL_IN_UP) != 0u);
    HolVec dir;
    HolVec *facing = &w->facing[p->id];
    uint8_t *cooldown = &w->dash_cooldown[p->id];

    dir.x = (float)ix;
    dir.y = (float)iy;
    if (ix != 0 && iy != 0) {
        dir.x *= HOL_DIAGONAL;
        dir.y *= HOL_DIAGONAL;
    }
    if (ix != 0 || iy != 0) {
        *facing = dir;
    }
    if ((buttons & HOL_IN_DASH) != 0u && *cooldown == 0u && p->dash == 0u &&
        (facing->x != 0.0f || facing->y != 0.0f)) {
        p->dash = (uint8_t)HOL_DASH_TICKS;
        *cooldown = (uint8_t)HOL_DASH_COOLDOWN;
    }
    if (p->dash > 0u) {
        p->pos.x += facing->x * HOL_DASH_SPEED;
        p->pos.y += facing->y * HOL_DASH_SPEED;
        --p->dash;
    } else {
        p->pos.x += dir.x * HOL_MOVE_SPEED;
        p->pos.y += dir.y * HOL_MOVE_SPEED;
    }
    clamp_player(p);
}

static void add_score(uint16_t *score)
{
    if (*score < HOL_MAX_SCORE) {
        ++*score;
    }
}

static void step_hazards(HolWorld *w)
{
    HolView *v = &w->view;
    size_t i;
    size_t j;

    for (i = 0u; i < v->hazard_count; ++i) {
        HolVec *pos = &v->hazards[i];
        HolVec *vel = &w->hazard_velocity[i];

        pos->x += vel->x;
        pos->y += vel->y;
        if (pos->x < HOL_HAZARD_RADIUS || pos->x > HOL_ARENA_W - HOL_HAZARD_RADIUS) {
            vel->x = -vel->x;
            pos->x = clampf(pos->x, HOL_HAZARD_RADIUS, HOL_ARENA_W - HOL_HAZARD_RADIUS);
        }
        if (pos->y < HOL_TOP + HOL_HAZARD_RADIUS || pos->y > HOL_ARENA_H - HOL_HAZARD_RADIUS) {
            vel->y = -vel->y;
            pos->y = clampf(pos->y, HOL_TOP + HOL_HAZARD_RADIUS, HOL_ARENA_H - HOL_HAZARD_RADIUS);
        }
        for (j = 0u; j < v->player_count; ++j) {
            HolPlayer *p = &v->players[j];
            const float reach = HOL_PLAYER_RADIUS + HOL_HAZARD_RADIUS;

            if (p->stun == 0u && dist2(p->pos, *pos) < reach * reach) {
                p->stun = (uint8_t)HOL_STUN_TICKS;
                p->dash = 0u;
                p->pos = k_spawn[p->id];
                if (v->team_score > 0u) {
                    --v->team_score;
                }
            }
        }
    }
}

static void step_stars(HolWorld *w)
{
    HolView *v = &w->view;
    size_t i;
    size_t j;
    const float reach = HOL_PLAYER_RADIUS + HOL_STAR_RADIUS;

    for (i = 0u; i < v->star_count; ++i) {
        for (j = 0u; j < v->player_count; ++j) {
            HolPlayer *p = &v->players[j];

            if (p->stun == 0u && dist2(p->pos, v->stars[i]) < reach * reach) {
                add_score(&p->score);
                if (v->mode == HOL_MODE_COOP) {
                    add_score(&v->team_score);
                }
                place_star(w, i);
                break;
            }
        }
    }
}

static void step_bumps(HolWorld *w)
{
    HolView *v = &w->view;
    size_t i;
    size_t j;

    if (v->mode != HOL_MODE_VERSUS) {
        return;
    }
    for (i = 0u; i < v->player_count; ++i) {
        for (j = 0u; j < v->player_count; ++j) {
            HolPlayer *hitter = &v->players[i];
            HolPlayer *victim = &v->players[j];
            const float reach = HOL_PLAYER_RADIUS * 2.0f;
            float dx;
            float dy;
            float len;

            if (i == j || hitter->dash == 0u || victim->dash != 0u || victim->stun != 0u ||
                dist2(hitter->pos, victim->pos) >= reach * reach) {
                continue;
            }
            victim->stun = (uint8_t)HOL_STUN_TICKS;
            if (victim->score > 0u) {
                --victim->score;
                add_score(&hitter->score);
            }
            dx = victim->pos.x - hitter->pos.x;
            dy = victim->pos.y - hitter->pos.y;
            len = sqrtf(dx * dx + dy * dy);
            if (len > 0.001f) {
                victim->pos.x += dx / len * 14.0f;
                victim->pos.y += dy / len * 14.0f;
                clamp_player(victim);
            }
            hitter->dash = 0u;
        }
    }
}

bool hol_world_step(HolWorld *world, const uint8_t buttons[HOL_MAX_PLAYERS])
{
    HolView *v = &world->view;
    size_t i;

    if (v->phase != HOL_PHASE_PLAYING) {
        return false;
    }
    ++v->tick;
    for (i = 0u; i < v->player_count; ++i) {
        HolPlayer *p = &v->players[i];

        if (world->dash_cooldown[p->id] > 0u) {
            --world->dash_cooldown[p->id];
        }
        if (p->stun > 0u) {
            --p->stun;
            p->dash = 0u;
            continue;
        }
        move_player(world, p, (uint8_t)(buttons[p->id] & HOL_IN_ALL));
    }
    step_hazards(world);
    step_stars(world);
    step_bumps(world);
    if (v->tick >= v->round_ticks) {
        v->phase = HOL_PHASE_FINISHED;
        return true;
    }
    return false;
}

int hol_view_leader(const HolView *view)
{
    int best = -1;
    size_t i;

    for (i = 0u; i < view->player_count; ++i) {
        if (best < 0 || view->players[i].score > view->players[best].score) {
            best = (int)i;
        }
    }
    return best < 0 ? -1 : (int)view->players[best].id;
}

bool hol_view_board_score(const HolView *view, uint8_t self_id, int32_t *out)
{
    const int idx = hol_view_find_player(view, self_id);

    if (idx < 0) {
        return false;
    }
    *out = view->mode == HOL_MODE_COOP ? (int32_t)view->team_score
                                       : (int32_t)view->players[idx].score;
    return true;
}

/* -------------------------------------------------------------------- bot */

uint8_t hol_bot_buttons(const HolView *view, uint8_t self_id, uint32_t frame)
{
    const int idx = hol_view_find_player(view, self_id);
    const HolPlayer *me;
    HolVec want;
    float best = FLT_MAX;
    float len;
    size_t i;
    uint8_t buttons = 0u;

    if (view->phase != HOL_PHASE_PLAYING || idx < 0) {
        return 0u;
    }
    me = &view->players[idx];
    if (me->stun != 0u) {
        return 0u;
    }
    want.x = HOL_ARENA_W * 0.5f - me->pos.x;
    want.y = HOL_ARENA_H * 0.5f - me->pos.y;
    for (i = 0u; i < view->star_count; ++i) {
        const float d = dist2(me->pos, view->stars[i]);

        if (d < best) {
            best = d;
            want.x = view->stars[i].x - me->pos.x;
            want.y = view->stars[i].y - me->pos.y;
        }
    }
    len = sqrtf(want.x * want.x + want.y * want.y);
    if (len > 0.001f) {
        want.x /= len;
        want.y /= len;
    }
    for (i = 0u; i < view->hazard_count; ++i) {
        const float d = dist2(me->pos, view->hazards[i]);
        const float near_zone = 42.0f * 42.0f;

        if (d < near_zone && d > 0.01f) {
            const float away = sqrtf(d);
            const float weight = (near_zone - d) / near_zone * 3.0f;

            want.x += (me->pos.x - view->hazards[i].x) / away * weight;
            want.y += (me->pos.y - view->hazards[i].y) / away * weight;
        }
    }
    if (want.x > 0.25f) {
        buttons |= HOL_IN_RIGHT;
    } else if (want.x < -0.25f) {
        buttons |= HOL_IN_LEFT;
    }
    if (want.y > 0.25f) {
        buttons |= HOL_IN_DOWN;
    } else if (want.y < -0.25f) {
        buttons |= HOL_IN_UP;
    }
    if (view->mode == HOL_MODE_VERSUS && best > 70.0f * 70.0f &&
        (frame + (uint32_t)self_id * 17u) % 150u == 0u) {
        buttons |= HOL_IN_DASH;
    }
    return buttons;
}
