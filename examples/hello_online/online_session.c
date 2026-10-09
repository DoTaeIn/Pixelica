/*
 * hello_online networking glue. See online_session.h.
 *
 * Nothing here blocks after the room exists: every call only copies buffers to
 * or from the online module's background worker.
 */
#include "online_session.h"

#include <string.h>

#define HOL_PUBLISH_EVERY 2u /* frames between published states = 30 Hz */
#define HOL_SUBMIT_RETRY_FRAMES 60u
#define HOL_SMOOTH 0.5f
#define HOL_SNAP_DISTANCE 40.0f

const char *hol_default_board(HolMode mode)
{
    return mode == HOL_MODE_VERSUS ? "versus" : "coop";
}

bool hol_valid_board_name(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (i = 0u; name[i] != '\0'; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';

        if (!ok || i >= 32u) {
            return false;
        }
    }
    return true;
}

static uint32_t fnv1a(const char *text)
{
    uint32_t hash = 2166136261u;

    for (; *text != '\0'; ++text) {
        hash = (hash ^ (uint8_t)*text) * 16777619u;
    }
    return hash;
}

static void set_board(HolSession *s, const char *board, HolMode mode)
{
    const char *name = (board != NULL && board[0] != '\0') ? board : hol_default_board(mode);
    size_t length = strlen(name);

    if (length > HOL_NAME_BYTES - 1u) {
        length = HOL_NAME_BYTES - 1u;
    }
    memcpy(s->board, name, length);
    s->board[length] = '\0';
}

static void begin(HolSession *s, bool is_host)
{
    memset(s, 0, sizeof *s);
    s->is_host = is_host;
}

/* Fills the identity fields that never change once the room exists. */
static void finish_open(HolSession *s, HolMode mode, const char *board)
{
    const char *code = pixelica_online_room_code(s->online);

    if (code != NULL) {
        memcpy(s->room_code, code, sizeof s->room_code - 1u);
        s->room_code[sizeof s->room_code - 1u] = '\0';
    }
    s->seed_base = fnv1a(s->room_code) | 1u;
    set_board(s, board, mode);
    (void)pixelica_online_snapshot(s->online, &s->snap);
    s->self_id = s->snap.self_id;
    s->host_id = s->snap.host_id;
    /* Show the existing top 10 in the lobby; the answer arrives asynchronously. */
    (void)pixelica_online_request_board(s->online, s->board);
}

bool hol_session_host(HolSession *s, const PIXELICA_OnlineConfig *config, HolMode mode,
                      uint8_t max_players, uint32_t round_seconds, const char *board,
                      PIXELICA_OnlineStatus *error)
{
    begin(s, true);
    s->online = pixelica_online_host(config, mode == HOL_MODE_VERSUS ? PIXELICA_ONLINE_VERSUS : PIXELICA_ONLINE_COOP,
                                max_players, error);
    if (s->online == NULL) {
        return false;
    }
    s->max_players = max_players;
    finish_open(s, mode, board);
    hol_world_init(&s->world, mode, round_seconds, s->seed_base);
    s->view = s->world.view;
    s->have_view = true;
    return true;
}

bool hol_session_join(HolSession *s, const PIXELICA_OnlineConfig *config, const char *room_code,
                      const char *board, PIXELICA_OnlineStatus *error)
{
    HolMode mode;

    begin(s, false);
    s->online = pixelica_online_join(config, room_code, error);
    if (s->online == NULL) {
        return false;
    }
    (void)pixelica_online_snapshot(s->online, &s->snap);
    mode = s->snap.mode == PIXELICA_ONLINE_VERSUS ? HOL_MODE_VERSUS : HOL_MODE_COOP;
    s->max_players = s->snap.max_players;
    finish_open(s, mode, board);
    return true;
}

void hol_session_close(HolSession *s)
{
    if (s != NULL && s->online != NULL) {
        pixelica_online_destroy(s->online);
        s->online = NULL;
        s->connected = false;
    }
}

void hol_session_force_score(HolSession *s, int32_t score)
{
    s->has_forced_score = true;
    s->forced_score = score;
}

PIXELICA_OnlineStatus hol_session_submit_now(HolSession *s, int32_t score, const uint8_t *record,
                                        size_t record_size)
{
    return pixelica_online_submit_score(s->online, s->board, score, record, record_size);
}

PIXELICA_OnlineStatus hol_session_request_board(HolSession *s)
{
    return pixelica_online_request_board(s->online, s->board);
}

/* ----------------------------------------------------------------- tick */

static void read_snapshot(HolSession *s)
{
    size_t i;
    size_t count;

    s->last_status = pixelica_online_snapshot(s->online, &s->snap);
    s->connected = s->snap.connected;
    s->self_id = s->snap.self_id;
    s->host_id = s->snap.host_id;
    s->max_players = s->snap.max_players;
    count = s->snap.player_count;
    if (count > HOL_MAX_PLAYERS) {
        count = HOL_MAX_PLAYERS;
    }
    s->player_count = (uint8_t)count;
    memset(s->names, 0, sizeof s->names);
    for (i = 0u; i < count; ++i) {
        const uint8_t id = s->snap.players[i].id;

        if (id < HOL_MAX_PLAYERS) {
            memcpy(s->names[id], s->snap.players[i].name, HOL_NAME_BYTES - 1u);
            s->names[id][HOL_NAME_BYTES - 1u] = '\0';
        }
    }
}

/* ids of the online players, ascending and unique. */
static size_t collect_ids(const HolSession *s, uint8_t ids[HOL_MAX_PLAYERS])
{
    bool present[HOL_MAX_PLAYERS] = {false};
    size_t count = 0u;
    size_t i;

    for (i = 0u; i < s->player_count; ++i) {
        if (s->snap.players[i].id < HOL_MAX_PLAYERS) {
            present[s->snap.players[i].id] = true;
        }
    }
    for (i = 0u; i < HOL_MAX_PLAYERS; ++i) {
        if (present[i]) {
            ids[count++] = (uint8_t)i;
        }
    }
    return count;
}

static void host_tick(HolSession *s, uint8_t buttons, bool start)
{
    uint8_t ids[HOL_MAX_PLAYERS];
    uint8_t inputs[HOL_MAX_PLAYERS] = {0};
    HolView *v = &s->world.view;
    HolPhase before;
    uint8_t wire[HOL_STATE_MAX];
    size_t size;
    size_t i;

    if (!s->connected) {
        return; /* the relay cannot be reached: freeze instead of simulating alone */
    }
    (void)hol_world_set_players(&s->world, ids, collect_ids(s, ids));
    for (i = 0u; i < s->player_count; ++i) {
        const PIXELICA_OnlinePlayer *p = &s->snap.players[i];
        uint8_t remote = 0u;

        if (p->id >= HOL_MAX_PLAYERS) {
            continue;
        }
        if (p->id == s->self_id) {
            inputs[p->id] = (uint8_t)(buttons & HOL_IN_ALL);
        } else if (hol_input_decode(p->input, p->input_size, &remote)) {
            inputs[p->id] = remote;
        }
    }

    before = v->phase;
    if (v->phase != HOL_PHASE_PLAYING) {
        const bool auto_start = s->want_players > 0u && !s->auto_started &&
                                v->phase == HOL_PHASE_LOBBY &&
                                v->player_count >= s->want_players;

        if ((start && v->player_count > 0u) || auto_start) {
            s->auto_started = true;
            hol_world_start_round(&s->world, s->seed_base + (uint32_t)v->round * 2654435761u);
        }
    } else {
        (void)hol_world_step(&s->world, inputs);
    }

    if (v->phase != before || s->frame % HOL_PUBLISH_EVERY == 0u) {
        size = hol_view_encode(v, wire, sizeof wire);
        if (size > 0u) {
            (void)pixelica_online_state(s->online, wire, size);
        }
    }
    s->view = *v;
    s->have_view = true;
}

static void guest_tick(HolSession *s, uint8_t buttons)
{
    uint8_t wire[HOL_INPUT_SIZE];
    HolView decoded;

    if (s->connected && s->snap.state_size > 0u) {
        if (hol_view_decode(s->snap.state, s->snap.state_size, &decoded)) {
            s->view = decoded;
            s->have_view = true;
        } else {
            ++s->bad_states;
        }
    }
    hol_input_encode(buttons, wire);
    (void)pixelica_online_input(s->online, wire, sizeof wire);
}

static void smooth_positions(HolSession *s)
{
    bool seen[HOL_MAX_PLAYERS] = {false};
    size_t i;

    for (i = 0u; i < s->view.player_count; ++i) {
        const HolPlayer *p = &s->view.players[i];
        HolVec *shown = &s->shown[p->id];
        const float dx = p->pos.x - shown->x;
        const float dy = p->pos.y - shown->y;

        seen[p->id] = true;
        if (s->is_host || !s->shown_valid[p->id] || dx * dx + dy * dy > HOL_SNAP_DISTANCE * HOL_SNAP_DISTANCE) {
            *shown = p->pos;
        } else {
            shown->x += dx * HOL_SMOOTH;
            shown->y += dy * HOL_SMOOTH;
        }
        s->shown_valid[p->id] = true;
    }
    for (i = 0u; i < HOL_MAX_PLAYERS; ++i) {
        if (!seen[i]) {
            s->shown_valid[i] = false;
        }
    }
}

/* Queue this machine's score once per finished round, retrying a few times. */
static void submit_round_score(HolSession *s)
{
    const HolView *v = &s->view;
    uint8_t record[HOL_RECORD_SIZE];
    int32_t score = 0;
    PIXELICA_OnlineStatus status;

    if (!s->have_view || v->phase != HOL_PHASE_FINISHED || s->seen_generation == 0u ||
        s->score_pending || (s->submitted && s->submitted_round == v->round) || s->frame < s->next_submit_frame) {
        return;
    }
    if (s->has_forced_score) {
        score = s->forced_score;
    } else if (!hol_view_board_score(v, s->self_id, &score)) {
        /* Not in the round (joined late): nothing to submit. */
        s->submitted = true;
        s->submitted_round = v->round;
        return;
    }
    hol_record_encode(v, record);
    if (s->submitted_round != v->round) s->submit_tries = 0u;
    s->submitted_round = v->round;
    (void)pixelica_online_leaderboard(s->online, &s->leaderboard);
    s->submit_generation = s->leaderboard.generation;
    ++s->submit_tries;
    status = pixelica_online_submit_score(s->online, s->board, score, record, sizeof record);
    if (status == 0) {
        s->submitted = false;
        s->score_pending = true;
        s->submitted_score = score;
        s->submit_error = 0;
        return;
    }
    s->submit_error = status;
    s->next_submit_frame = s->frame + HOL_SUBMIT_RETRY_FRAMES;
    if (s->submit_tries >= HOL_BOARD_MAX_TRIES) {
        s->submitted = true; /* give up for this round; submit_error tells why */
        s->submitted_round = v->round;
    }
}

static void poll_board(HolSession *s)
{
    PIXELICA_OnlineLeaderboard *lb = &s->leaderboard;

    (void)pixelica_online_leaderboard(s->online, lb);
    if (s->score_pending && lb->generation != s->submit_generation) {
        s->score_pending = false;
        s->submit_error = lb->status;
        s->submitted = lb->status == 0 || s->submit_tries >= HOL_BOARD_MAX_TRIES;
        s->next_submit_frame = s->frame + HOL_SUBMIT_RETRY_FRAMES;
    }
    if (lb->generation != s->seen_generation) {
        s->seen_generation = lb->generation;
        s->board_updated = true;
    }
}

void hol_session_tick(HolSession *s, uint8_t buttons, bool start)
{
    if (s->online == NULL) {
        return;
    }
    read_snapshot(s);
    if (s->is_host) {
        host_tick(s, buttons, start);
    } else {
        guest_tick(s, buttons);
    }
    smooth_positions(s);
    if (s->connected) {
        submit_round_score(s);
    }
    poll_board(s);
    ++s->frame;
}
