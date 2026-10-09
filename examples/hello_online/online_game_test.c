/*
 * hello_online game test: wire codec, host simulation and bot. Pure logic, no
 * network, no window, no server - links online_game.c only (plus libm).
 * Exit status 0 = all checks passed.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "online_game.h"

static int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static void clear_stars(HolWorld *w)
{
    size_t i;

    for (i = 0u; i < HOL_MAX_STARS; ++i) {
        w->view.stars[i].x = 250.0f;
        w->view.stars[i].y = 180.0f;
    }
}

static HolWorld make_world(HolMode mode, uint32_t seconds, size_t players)
{
    HolWorld w;
    uint8_t ids[HOL_MAX_PLAYERS];
    size_t i;

    hol_world_init(&w, mode, seconds, 1234u);
    for (i = 0u; i < players; ++i) {
        ids[i] = (uint8_t)i;
    }
    CHECK(hol_world_set_players(&w, ids, players));
    hol_world_start_round(&w, 99u);
    clear_stars(&w);
    return w;
}

static void test_codec_roundtrip(void)
{
    HolWorld w = make_world(HOL_MODE_COOP, 10u, 3u);
    HolView decoded;
    uint8_t wire[HOL_STATE_MAX];
    size_t size;

    w.view.team_score = 7;
    w.view.players[1].score = 4;
    w.view.players[1].stun = 12;
    w.view.players[2].dash = 3;
    size = hol_view_encode(&w.view, wire, sizeof wire);
    CHECK(size > 0u && size <= HOL_STATE_MAX);
    /* 20 header/count bytes + 5 stars * 8 + 3 hazards * 8 + 2 count bytes + 3 players * 13 */
    CHECK(size == 19u + 1u + 40u + 1u + 24u + 1u + 39u);
    CHECK(hol_view_decode(wire, size, &decoded));
    CHECK(memcmp(&decoded.stars, &w.view.stars, sizeof decoded.stars) == 0);
    CHECK(decoded.player_count == 3u && decoded.players[1].score == 4u &&
          decoded.players[1].stun == 12u && decoded.players[2].dash == 3u);
    CHECK(decoded.team_score == 7u && decoded.round == w.view.round && decoded.seed == 99u);
    CHECK(decoded.players[2].pos.x == w.view.players[2].pos.x);
    CHECK(hol_view_encode(&w.view, wire, size - 1u) == 0u); /* too small */
}

static void test_codec_is_big_endian(void)
{
    HolWorld w = make_world(HOL_MODE_VERSUS, 10u, 1u);
    uint8_t wire[HOL_STATE_MAX];

    w.view.seed = 0x01020304u;
    w.view.players[0].pos.x = 1.0f; /* IEEE binary32 0x3F800000 */
    CHECK(hol_view_encode(&w.view, wire, sizeof wire) > 0u);
    CHECK(wire[0] == 1u && wire[1] == (uint8_t)HOL_MODE_VERSUS && wire[2] == (uint8_t)HOL_PHASE_PLAYING);
    CHECK(wire[3] == 0u && wire[4] == 1u); /* round = 1 */
    CHECK(wire[5] == 1u && wire[6] == 2u && wire[7] == 3u && wire[8] == 4u);
    /* the only player entry follows 19 + (1 + 40) + (1 + 0) + 1 bytes */
    CHECK(wire[62] == 0u);
    CHECK(wire[63] == 0x3Fu && wire[64] == 0x80u && wire[65] == 0u && wire[66] == 0u);
}

static void test_decode_rejects_bad_input(void)
{
    HolWorld w = make_world(HOL_MODE_COOP, 10u, 2u);
    HolView probe;
    HolView before;
    uint8_t wire[HOL_STATE_MAX];
    uint8_t copy[HOL_STATE_MAX + 1u];
    size_t size = hol_view_encode(&w.view, wire, sizeof wire);
    size_t i;

    memset(&probe, 0x5A, sizeof probe);
    before = probe;
    CHECK(size > 0u);
    for (i = 0u; i < size; ++i) { /* every truncation is rejected */
        CHECK(!hol_view_decode(wire, i, &probe));
    }
    memcpy(copy, wire, size);
    copy[size] = 0u; /* trailing byte */
    CHECK(!hol_view_decode(copy, size + 1u, &probe));
    CHECK(memcmp(&probe, &before, sizeof probe) == 0); /* output untouched on failure */

    memcpy(copy, wire, size);
    copy[0] = 2u; /* version */
    CHECK(!hol_view_decode(copy, size, &probe));
    memcpy(copy, wire, size);
    copy[1] = 3u; /* mode */
    CHECK(!hol_view_decode(copy, size, &probe));
    memcpy(copy, wire, size);
    copy[2] = 3u; /* phase */
    CHECK(!hol_view_decode(copy, size, &probe));
    memcpy(copy, wire, size);
    copy[19] = 6u; /* star count */
    CHECK(!hol_view_decode(copy, size, &probe));

    {   /* NaN x of the first star */
        const float nan_value = NAN;
        uint32_t bits;

        memcpy(&bits, &nan_value, sizeof bits);
        memcpy(copy, wire, size);
        copy[20] = (uint8_t)(bits >> 24);
        copy[21] = (uint8_t)(bits >> 16);
        copy[22] = (uint8_t)(bits >> 8);
        copy[23] = (uint8_t)bits;
        CHECK(!hol_view_decode(copy, size, &probe));
    }
    {   /* star outside the arena */
        HolView bad = w.view;

        bad.stars[0].x = 300.0f;
        CHECK(hol_view_encode(&bad, wire, sizeof wire) > 0u);
        CHECK(!hol_view_decode(wire, size, &probe));
        bad = w.view;
        bad.players[1].id = 0u; /* duplicate / not ascending id */
        CHECK(hol_view_encode(&bad, wire, sizeof wire) > 0u);
        CHECK(!hol_view_decode(wire, size, &probe));
        bad = w.view;
        bad.players[1].id = 8u;
        CHECK(hol_view_encode(&bad, wire, sizeof wire) > 0u);
        CHECK(!hol_view_decode(wire, size, &probe));
        bad = w.view;
        bad.tick = bad.round_ticks + 1u;
        CHECK(hol_view_encode(&bad, wire, sizeof wire) > 0u);
        CHECK(!hol_view_decode(wire, size, &probe));
    }
    CHECK(!hol_view_decode(NULL, 0u, &probe));
}

static void test_input_and_record(void)
{
    uint8_t wire[HOL_INPUT_SIZE];
    uint8_t buttons = 0xAAu;
    uint8_t record[HOL_RECORD_SIZE];
    HolRecord rec;
    HolWorld w = make_world(HOL_MODE_VERSUS, 20u, 4u);

    hol_input_encode(HOL_IN_LEFT | HOL_IN_DASH, wire);
    CHECK(wire[0] == 1u && wire[1] == (HOL_IN_LEFT | HOL_IN_DASH));
    CHECK(hol_input_decode(wire, sizeof wire, &buttons) && buttons == (HOL_IN_LEFT | HOL_IN_DASH));
    buttons = 0xAAu;
    CHECK(!hol_input_decode(wire, 1u, &buttons) && buttons == 0xAAu);
    wire[1] = 0x20u; /* undefined bit */
    CHECK(!hol_input_decode(wire, sizeof wire, &buttons));
    wire[1] = 1u;
    wire[0] = 9u;
    CHECK(!hol_input_decode(wire, sizeof wire, &buttons));

    hol_record_encode(&w.view, record);
    CHECK(hol_record_decode(record, sizeof record, &rec));
    CHECK(rec.mode == HOL_MODE_VERSUS && rec.players == 4u && rec.round_seconds == 20u &&
          rec.seed == 99u);
    CHECK(!hol_record_decode(record, sizeof record - 1u, &rec));
    record[2] = 0u;
    CHECK(!hol_record_decode(record, sizeof record, &rec));
}

static void test_set_players(void)
{
    HolWorld w;
    const uint8_t first[3] = {0u, 2u, 5u};
    const uint8_t second[2] = {2u, 3u};
    const uint8_t bad_order[2] = {3u, 1u};
    const uint8_t bad_id[1] = {8u};

    hol_world_init(&w, HOL_MODE_COOP, 10u, 1u);
    CHECK(hol_world_set_players(&w, first, 3u));
    w.view.players[1].score = 9u;
    CHECK(hol_world_set_players(&w, second, 2u));
    CHECK(w.view.player_count == 2u && w.view.players[0].id == 2u && w.view.players[0].score == 9u);
    CHECK(w.view.players[1].id == 3u && w.view.players[1].score == 0u);
    CHECK(!hol_world_set_players(&w, bad_order, 2u));
    CHECK(!hol_world_set_players(&w, bad_id, 1u));
    CHECK(w.view.player_count == 2u); /* rejected lists change nothing */
    CHECK(hol_view_find_player(&w.view, 3u) == 1 && hol_view_find_player(&w.view, 0u) == -1);
}

static void test_collect_star_coop(void)
{
    HolWorld w = make_world(HOL_MODE_COOP, 10u, 2u);
    const uint8_t idle[HOL_MAX_PLAYERS] = {0};
    HolVec old_star;

    w.view.hazard_count = 0u;
    w.view.players[0].pos.x = 100.0f;
    w.view.players[0].pos.y = 100.0f;
    w.view.stars[0].x = 103.0f;
    w.view.stars[0].y = 100.0f;
    old_star = w.view.stars[0];
    CHECK(!hol_world_step(&w, idle));
    CHECK(w.view.players[0].score == 1u && w.view.team_score == 1u && w.view.players[1].score == 0u);
    CHECK(w.view.stars[0].x != old_star.x || w.view.stars[0].y != old_star.y); /* respawned */
    CHECK(w.view.tick == 1u);
}

static void test_hazard_stuns_coop(void)
{
    HolWorld w = make_world(HOL_MODE_COOP, 10u, 1u);
    const uint8_t idle[HOL_MAX_PLAYERS] = {0};

    w.view.team_score = 3u;
    w.view.players[0].pos.x = 120.0f;
    w.view.players[0].pos.y = 90.0f;
    w.view.hazards[0].x = 122.0f;
    w.view.hazards[0].y = 90.0f;
    (void)hol_world_step(&w, idle);
    CHECK(w.view.players[0].stun == 45u && w.view.team_score == 2u);
    CHECK(w.view.players[0].pos.x == 40.0f && w.view.players[0].pos.y == 44.0f); /* spawn of id 0 */
}

static void test_versus_bump(void)
{
    HolWorld w = make_world(HOL_MODE_VERSUS, 10u, 2u);
    const uint8_t idle[HOL_MAX_PLAYERS] = {0};

    CHECK(w.view.hazard_count == 0u && w.view.team_score == 0u);
    w.view.players[0].pos.x = 100.0f;
    w.view.players[0].pos.y = 100.0f;
    w.view.players[0].dash = 5u;
    w.view.players[1].pos.x = 108.0f;
    w.view.players[1].pos.y = 100.0f;
    w.view.players[1].score = 3u;
    (void)hol_world_step(&w, idle);
    CHECK(w.view.players[1].stun == 45u && w.view.players[1].score == 2u);
    CHECK(w.view.players[0].score == 1u && w.view.players[0].dash == 0u);
    CHECK(w.view.team_score == 0u);
    CHECK(hol_view_leader(&w.view) == 1);
}

static void test_dash_and_walls(void)
{
    HolWorld w = make_world(HOL_MODE_VERSUS, 10u, 1u);
    uint8_t buttons[HOL_MAX_PLAYERS] = {0};
    const float start_x = w.view.players[0].pos.x;
    int i;

    buttons[0] = HOL_IN_RIGHT;
    (void)hol_world_step(&w, buttons);
    CHECK(fabsf(w.view.players[0].pos.x - (start_x + 1.5f)) < 0.001f);
    buttons[0] = HOL_IN_RIGHT | HOL_IN_DASH;
    (void)hol_world_step(&w, buttons);
    CHECK(w.view.players[0].dash > 0u); /* dash started */
    for (i = 0; i < 200; ++i) { /* keep walking into the wall */
        buttons[0] = HOL_IN_RIGHT;
        (void)hol_world_step(&w, buttons);
    }
    CHECK(w.view.players[0].pos.x <= HOL_ARENA_W - HOL_PLAYER_RADIUS + 0.001f);
    buttons[0] = HOL_IN_UP;
    for (i = 0; i < 300; ++i) {
        (void)hol_world_step(&w, buttons);
    }
    CHECK(w.view.players[0].pos.y >= 16.0f + HOL_PLAYER_RADIUS - 0.001f);
}

static void test_round_ends_and_restarts(void)
{
    HolWorld w = make_world(HOL_MODE_COOP, 3u, 2u);
    const uint8_t idle[HOL_MAX_PLAYERS] = {0};
    uint32_t steps = 0u;
    bool finished = false;

    while (!finished && steps < 1000u) {
        finished = hol_world_step(&w, idle);
        ++steps;
    }
    CHECK(finished && steps == 3u * HOL_TICK_HZ);
    CHECK(w.view.phase == HOL_PHASE_FINISHED);
    CHECK(!hol_world_step(&w, idle)); /* finished worlds do not advance */
    CHECK(w.view.tick == 3u * HOL_TICK_HZ);
    hol_world_start_round(&w, 7u);
    CHECK(w.view.phase == HOL_PHASE_PLAYING && w.view.round == 2u && w.view.tick == 0u &&
          w.view.team_score == 0u && w.view.seed == 7u);
}

static void test_board_score(void)
{
    HolWorld coop = make_world(HOL_MODE_COOP, 10u, 2u);
    HolWorld versus = make_world(HOL_MODE_VERSUS, 10u, 2u);
    int32_t score = -1;

    coop.view.team_score = 11u;
    coop.view.players[0].score = 4u;
    versus.view.players[0].score = 4u;
    CHECK(hol_view_board_score(&coop.view, 0u, &score) && score == 11);
    CHECK(hol_view_board_score(&versus.view, 0u, &score) && score == 4);
    CHECK(!hol_view_board_score(&versus.view, 6u, &score));
}

/* Two autopilots on one host world must actually play the game. */
static void test_bots_play(HolMode mode)
{
    HolWorld w = make_world(mode, 30u, 2u);
    uint8_t buttons[HOL_MAX_PLAYERS] = {0};
    uint32_t frame = 0u;
    uint32_t total;

    hol_world_start_round(&w, 4242u); /* real stars again */
    while (w.view.phase == HOL_PHASE_PLAYING) {
        buttons[0] = hol_bot_buttons(&w.view, 0u, frame);
        buttons[1] = hol_bot_buttons(&w.view, 1u, frame);
        (void)hol_world_step(&w, buttons);
        ++frame;
    }
    total = (uint32_t)w.view.players[0].score + w.view.players[1].score;
    CHECK(frame == 30u * HOL_TICK_HZ);
    CHECK(total >= 3u);
    if (mode == HOL_MODE_COOP) {
        CHECK(w.view.team_score <= total);
    } else {
        CHECK(w.view.team_score == 0u);
    }
    CHECK(hol_bot_buttons(&w.view, 0u, frame) == 0u); /* finished: idle */
}

static void test_determinism(void)
{
    HolWorld a = make_world(HOL_MODE_COOP, 5u, 2u);
    HolWorld b = make_world(HOL_MODE_COOP, 5u, 2u);
    uint8_t wa[HOL_STATE_MAX];
    uint8_t wb[HOL_STATE_MAX];
    uint8_t buttons[HOL_MAX_PLAYERS] = {0};
    uint32_t frame;
    size_t sa;
    size_t sb;

    hol_world_start_round(&a, 5u);
    hol_world_start_round(&b, 5u);
    for (frame = 0u; frame < 200u; ++frame) {
        buttons[0] = hol_bot_buttons(&a.view, 0u, frame);
        buttons[1] = hol_bot_buttons(&a.view, 1u, frame);
        (void)hol_world_step(&a, buttons);
        (void)hol_world_step(&b, buttons);
    }
    sa = hol_view_encode(&a.view, wa, sizeof wa);
    sb = hol_view_encode(&b.view, wb, sizeof wb);
    CHECK(sa > 0u && sa == sb && memcmp(wa, wb, sa) == 0);
}

int main(void)
{
    test_codec_roundtrip();
    test_codec_is_big_endian();
    test_decode_rejects_bad_input();
    test_input_and_record();
    test_set_players();
    test_collect_star_coop();
    test_hazard_stuns_coop();
    test_versus_bump();
    test_dash_and_walls();
    test_round_ends_and_restarts();
    test_board_score();
    test_bots_play(HOL_MODE_COOP);
    test_bots_play(HOL_MODE_VERSUS);
    test_determinism();
    if (g_failures != 0) {
        fprintf(stderr, "hello_online game test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("hello_online game test: ok\n");
    return 0;
}
