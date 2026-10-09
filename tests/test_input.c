/*
 * Pixelica - input edge helpers, bounded run-length recording, playback,
 * the portable "P3IN" encoding (golden bytes, strict validation, every
 * truncation and bit flip rejected) and deterministic replay: identical
 * tick input gives bit-identical game state at 30/60/120 Hz render
 * schedules, and a recording reproduces a live session exactly.
 *
 * The edge helpers, recorder, player and "P3IN" encoding tests use only the
 * common headers and run in every build. The two replay tests drive a real
 * PIXELICA_Context and are compiled only when PIXELICA_TEST_WITH_3D is defined.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pixelica/assets_common.h"
#include "pixelica/input.h"
#include "pixelica/platform.h"
#include "pixelica/status.h"
#ifdef PIXELICA_TEST_WITH_3D
#include "pixelica/math.h"
#include "pixelica/pixelica.h"
#endif

#include "test_util.h"

/* ---- tiny deterministic game ------------------------------------------ */

typedef struct Game {
    int32_t x, y;
    uint32_t ticks;
    uint32_t interactions;
    uint32_t starts;
    uint32_t prev;
    float fx;
    uint64_t trace;
    PIXELICA_InputRecorder *rec; /* optional: records every tick's mask */
    int record_failures;
} Game;

#ifdef PIXELICA_TEST_WITH_3D
static void game_reset(Game *g, PIXELICA_InputRecorder *rec)
{
    memset(g, 0, sizeof *g);
    g->trace = 1469598103934665603ull;
    g->rec = rec;
}
#endif

static void game_tick(void *ud, float dt, uint32_t buttons)
{
    Game *g = (Game *)ud;
    uint32_t pressed;

    buttons &= PIXELICA_BTN_ALL; /* games only interpret defined buttons */
    pressed = pixelica_buttons_pressed(g->prev, buttons);
    if (g->rec != NULL && pixelica_input_record(g->rec, buttons) != PIXELICA_OK) {
        g->record_failures++;
    }
    if ((buttons & PIXELICA_BTN_RIGHT) != 0u) {
        g->x += 3;
    }
    if ((buttons & PIXELICA_BTN_LEFT) != 0u) {
        g->x -= 2;
    }
    if ((buttons & PIXELICA_BTN_UP) != 0u) {
        g->y += 1;
    }
    if ((pressed & PIXELICA_BTN_A) != 0u) {
        g->interactions++;
        g->x *= -1;
    }
    if ((pressed & PIXELICA_BTN_START) != 0u) {
        g->starts++;
    }
    g->fx += dt * (float)g->x * 0.37f;
    g->trace = (g->trace ^ (uint64_t)buttons ^ ((uint64_t)(uint32_t)g->x << 8)) *
               1099511628211ull;
    g->prev = buttons;
    g->ticks++;
}

#ifdef PIXELICA_TEST_WITH_3D
static int game_equal(const Game *a, const Game *b)
{
    return a->x == b->x && a->y == b->y && a->ticks == b->ticks &&
           a->interactions == b->interactions && a->starts == b->starts &&
           a->prev == b->prev &&
           memcmp(&a->fx, &b->fx, sizeof a->fx) == 0 && a->trace == b->trace;
}
#endif

static PIXELICA_InputSpan span(uint32_t buttons, uint32_t ticks)
{
    PIXELICA_InputSpan s;
    s.buttons = buttons;
    s.ticks = ticks;
    return s;
}

static int spans_equal(const PIXELICA_InputSpan *a, size_t na,
                       const PIXELICA_InputSpan *b, size_t nb)
{
    size_t i;
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        if (a[i].buttons != b[i].buttons || a[i].ticks != b[i].ticks) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------ */

/* PIXELICA_BTN_ALL is exactly the defined buttons (compile time, no constant
 * runtime condition). */
typedef char pixelica_t_btn_all_check
    [(PIXELICA_BTN_ALL == (PIXELICA_BTN_LEFT | PIXELICA_BTN_RIGHT | PIXELICA_BTN_UP |
                      PIXELICA_BTN_DOWN | PIXELICA_BTN_A | PIXELICA_BTN_START |
                      PIXELICA_BTN_QUIT)) ? 1 : -1];

static void test_edges(void)
{
    CHECK(pixelica_buttons_pressed(0u, PIXELICA_BTN_A) == PIXELICA_BTN_A);
    CHECK(pixelica_buttons_pressed(PIXELICA_BTN_A, PIXELICA_BTN_A) == 0u);
    CHECK(pixelica_buttons_pressed(PIXELICA_BTN_A, PIXELICA_BTN_A | PIXELICA_BTN_UP) ==
          PIXELICA_BTN_UP);
    CHECK(pixelica_buttons_released(PIXELICA_BTN_A | PIXELICA_BTN_UP, PIXELICA_BTN_UP) ==
          PIXELICA_BTN_A);
    CHECK(pixelica_buttons_released(0u, PIXELICA_BTN_ALL) == 0u);
}

static void test_recorder(void)
{
    PIXELICA_InputSpan storage[3];
    PIXELICA_InputRecorder rec;
    PIXELICA_InputPlayer player;
    uint32_t b = 99;
    int i;

    pixelica_input_recorder_init(&rec, storage, 3);
    for (i = 0; i < 4; ++i) {
        CHECK(pixelica_input_record(&rec, PIXELICA_BTN_A) == PIXELICA_OK);
    }
    /* Unknown bits are dropped, so this extends the A span. */
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_A | 0x80u | 0x80000000u) == PIXELICA_OK);
    CHECK(pixelica_input_record(&rec, 0u) == PIXELICA_OK);
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_LEFT) == PIXELICA_OK);
    CHECK(rec.count == 3 && rec.total_ticks == 7u);
    CHECK(storage[0].buttons == PIXELICA_BTN_A && storage[0].ticks == 5u);
    CHECK(storage[1].buttons == 0u && storage[1].ticks == 1u);
    CHECK(storage[2].buttons == PIXELICA_BTN_LEFT && storage[2].ticks == 1u);

    /* Full: a new mask is refused without changing anything; the current
     * mask still extends. */
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_UP) == PIXELICA_ERR_CAPACITY);
    CHECK(rec.count == 3 && rec.total_ticks == 7u && storage[2].ticks == 1u);
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_LEFT) == PIXELICA_OK);
    CHECK(storage[2].ticks == 2u && rec.total_ticks == 8u);

    /* Playback yields exactly the recorded ticks. */
    pixelica_input_player_init(&player, storage, rec.count);
    for (i = 0; i < 5; ++i) {
        CHECK(pixelica_input_player_next(&player, &b) && b == PIXELICA_BTN_A);
    }
    CHECK(pixelica_input_player_next(&player, &b) && b == 0u);
    CHECK(pixelica_input_player_next(&player, &b) && b == PIXELICA_BTN_LEFT);
    CHECK(pixelica_input_player_next(&player, NULL));
    CHECK(!pixelica_input_player_next(&player, &b) && b == 0u);
    CHECK(!pixelica_input_player_next(&player, &b));
    CHECK(player.tick == 8u);

    /* Zero-tick spans are skipped by the player. */
    {
        PIXELICA_InputSpan odd[3];
        odd[0] = span(PIXELICA_BTN_A, 0);
        odd[1] = span(PIXELICA_BTN_UP, 2);
        odd[2] = span(PIXELICA_BTN_A, 0);
        pixelica_input_player_init(&player, odd, 3);
        CHECK(pixelica_input_player_next(&player, &b) && b == PIXELICA_BTN_UP);
        CHECK(pixelica_input_player_next(&player, &b) && b == PIXELICA_BTN_UP);
        CHECK(!pixelica_input_player_next(&player, &b));
    }

    /* A span that reaches UINT32_MAX ticks continues in a new span. */
    pixelica_input_recorder_init(&rec, storage, 3);
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_DOWN) == PIXELICA_OK);
    storage[0].ticks = UINT32_MAX - 1u;
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_DOWN) == PIXELICA_OK);
    CHECK(rec.count == 1 && storage[0].ticks == UINT32_MAX);
    CHECK(pixelica_input_record(&rec, PIXELICA_BTN_DOWN) == PIXELICA_OK);
    CHECK(rec.count == 2 && storage[1].ticks == 1u);

    /* NULL / empty storage. */
    CHECK(pixelica_input_record(NULL, 0u) == PIXELICA_ERR_INVALID_ARG);
    pixelica_input_recorder_init(&rec, NULL, 10);
    CHECK(rec.capacity == 0);
    CHECK(pixelica_input_record(&rec, 0u) == PIXELICA_ERR_CAPACITY);
    pixelica_input_recorder_init(NULL, storage, 3);
    pixelica_input_player_init(NULL, storage, 3);
    CHECK(!pixelica_input_player_next(NULL, &b) && b == 0u);
    CHECK(pixelica_input_replay(NULL, 3, game_tick, NULL) == 0u);
    CHECK(pixelica_input_replay(storage, 1, NULL, NULL) == 0u);
}

/* Exact little-endian layout. */
static void test_encoding_golden(void)
{
    PIXELICA_InputSpan spans[2];
    uint8_t buf[64];
    PIXELICA_InputSpan back[2];
    size_t written = 0, count = 0, need = 0;
    static const uint8_t expected_payload[16] = {
        0x10, 0, 0, 0, 3, 0, 0, 0,     /* A for 3 ticks */
        0x00, 0, 0, 0, 0x02, 0x01, 0, 0 /* none for 258 ticks */
    };
    uint32_t crc;

    spans[0] = span(PIXELICA_BTN_A, 3);
    spans[1] = span(0u, 258);
    CHECK(pixelica_input_encoded_size(2, &need) == PIXELICA_OK && need == 32u);
    CHECK(pixelica_input_encode(spans, 2, buf, sizeof buf, &written) == PIXELICA_OK);
    CHECK_EQ_LONG(written, 32);
    CHECK(memcmp(buf, "P3IN", 4) == 0);
    CHECK(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
    CHECK(buf[8] == 2 && buf[9] == 0 && buf[10] == 0 && buf[11] == 0);
    crc = pixelica_crc32(expected_payload, sizeof expected_payload);
    CHECK(buf[12] == (uint8_t)(crc & 0xFFu) &&
          buf[13] == (uint8_t)((crc >> 8) & 0xFFu) &&
          buf[14] == (uint8_t)((crc >> 16) & 0xFFu) &&
          buf[15] == (uint8_t)(crc >> 24));
    CHECK(memcmp(buf + 16, expected_payload, sizeof expected_payload) == 0);

    CHECK(pixelica_input_decode_count(buf, written, &count) == PIXELICA_OK &&
          count == 2u);
    CHECK(pixelica_input_decode(buf, written, back, 2, &count) == PIXELICA_OK);
    CHECK(spans_equal(spans, 2, back, count));

    /* Empty recording: header only. */
    CHECK(pixelica_input_encode(NULL, 0, buf, sizeof buf, &written) == PIXELICA_OK);
    CHECK_EQ_LONG(written, 16);
    CHECK(pixelica_input_decode(buf, written, NULL, 0, &count) == PIXELICA_OK &&
          count == 0u);
}

static void test_encoding_errors(void)
{
    PIXELICA_InputSpan spans[4];
    PIXELICA_InputSpan back[4];
    static uint8_t buf[256];
    static uint8_t copy[256 + 4];
    static uint8_t forged[64];
    size_t written = 0, count = 7, i, size;
    int bit;
    uint32_t bad = 0;

    /* Non-canonical spans are refused by the encoder (nothing written). */
    memset(buf, 0xEE, sizeof buf);
    spans[0] = span(PIXELICA_BTN_A, 0);
    CHECK(pixelica_input_encode(spans, 1, buf, sizeof buf, &written) ==
          PIXELICA_ERR_INVALID_ARG);
    spans[0] = span(0x80u, 1);
    CHECK(pixelica_input_encode(spans, 1, buf, sizeof buf, &written) ==
          PIXELICA_ERR_INVALID_ARG);
    spans[0] = span(PIXELICA_BTN_A, 2);
    spans[1] = span(PIXELICA_BTN_A, 1);
    CHECK(pixelica_input_encode(spans, 2, buf, sizeof buf, &written) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(written == 0 && buf[0] == 0xEE);
    spans[0] = span(PIXELICA_BTN_A, UINT32_MAX); /* full span may repeat */
    CHECK(pixelica_input_encode(spans, 2, buf, sizeof buf, &written) == PIXELICA_OK);
    CHECK(pixelica_input_decode(buf, written, back, 4, &count) == PIXELICA_OK &&
          count == 2u && back[0].ticks == UINT32_MAX);

    /* Output too small / NULL arguments. */
    spans[0] = span(PIXELICA_BTN_LEFT, 5);
    spans[1] = span(PIXELICA_BTN_LEFT | PIXELICA_BTN_A, 1);
    spans[2] = span(PIXELICA_BTN_LEFT, 7);
    spans[3] = span(PIXELICA_BTN_QUIT, 1);
    memset(buf, 0xEE, sizeof buf);
    CHECK(pixelica_input_encode(spans, 4, buf, 47, &written) == PIXELICA_ERR_NO_MEMORY);
    CHECK(written == 0 && buf[0] == 0xEE);
    CHECK(pixelica_input_encode(spans, 4, NULL, 48, &written) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_encode(NULL, 4, buf, 48, &written) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_encode(spans, 4, buf, 48, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_encoded_size(4, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_encoded_size(SIZE_MAX, &size) == PIXELICA_ERR_CAPACITY);
    /* Largest encodable count: bounded by the u32 file field with a 64-bit
     * size_t, by the byte size itself with a 32-bit size_t. */
    {
#if SIZE_MAX > UINT32_MAX
        const size_t max_spans = (size_t)UINT32_MAX;
#else
        const size_t max_spans =
            (SIZE_MAX - PIXELICA_INPUT_HEADER_BYTES) / PIXELICA_INPUT_SPAN_BYTES;
#endif
        CHECK(pixelica_input_encoded_size(max_spans, &size) == PIXELICA_OK &&
              size == PIXELICA_INPUT_HEADER_BYTES +
                          max_spans * PIXELICA_INPUT_SPAN_BYTES);
        CHECK(pixelica_input_encoded_size(max_spans + 1u, &size) ==
                  PIXELICA_ERR_CAPACITY &&
              size == 0u);
    }
    CHECK(pixelica_input_encode(spans, 4, buf, 48, &written) == PIXELICA_OK);
    size = written;

    /* Decoder: capacity too small leaves `out` untouched. */
    memset(back, 0x5A, sizeof back);
    CHECK(pixelica_input_decode(buf, size, back, 3, &count) == PIXELICA_ERR_NO_MEMORY);
    CHECK(count == 0 && back[0].buttons == 0x5A5A5A5Au);
    CHECK(pixelica_input_decode(buf, size, NULL, 4, &count) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_decode(NULL, size, back, 4, &count) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_input_decode(buf, size, back, 4, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* Unaligned input decodes identically. */
    memcpy(copy + 1, buf, size);
    CHECK(pixelica_input_decode(copy + 1, size, back, 4, &count) == PIXELICA_OK);
    CHECK(spans_equal(spans, 4, back, count));

    /* Every truncation, extension and single-bit flip is rejected. */
    for (i = 0; i < size; ++i) {
        size_t c = 9;
        memcpy(copy, buf, i);
        if (pixelica_input_decode_count(copy, i, &c) != PIXELICA_ERR_BAD_ASSET ||
            c != 0) {
            ++bad;
        }
        for (bit = 0; bit < 8; ++bit) {
            memcpy(copy, buf, size);
            copy[i] = (uint8_t)(copy[i] ^ (1u << bit));
            if (pixelica_input_decode(copy, size, back, 4, &c) == PIXELICA_OK) {
                ++bad;
            }
        }
    }
    memcpy(copy, buf, size);
    copy[size] = 0;
    if (pixelica_input_decode_count(copy, size + 1u, &count) == PIXELICA_OK) {
        ++bad;
    }
    CHECK_EQ_LONG(bad, 0);

    /* Forged files with a correct CRC but invalid content. */
    {
        static const uint32_t cases[4][2] = {
            {PIXELICA_BTN_A, 0u},           /* zero ticks */
            {0x100u, 1u},              /* undefined button bit */
            {PIXELICA_BTN_A, 1u},           /* same mask as previous span */
            {PIXELICA_BTN_UP, 1u},          /* valid control case */
        };
        int k;
        for (k = 0; k < 4; ++k) {
            uint8_t *p = forged + 16;
            uint32_t crc;
            memset(forged, 0, sizeof forged);
            memcpy(forged, "P3IN", 4);
            forged[4] = 1;
            forged[8] = 2;
            p[0] = (uint8_t)PIXELICA_BTN_A;
            p[4] = 1;
            p[8] = (uint8_t)(cases[k][0] & 0xFFu);
            p[9] = (uint8_t)((cases[k][0] >> 8) & 0xFFu);
            p[12] = (uint8_t)(cases[k][1] & 0xFFu);
            crc = pixelica_crc32(p, 16);
            forged[12] = (uint8_t)(crc & 0xFFu);
            forged[13] = (uint8_t)((crc >> 8) & 0xFFu);
            forged[14] = (uint8_t)((crc >> 16) & 0xFFu);
            forged[15] = (uint8_t)(crc >> 24);
            CHECK(pixelica_input_decode(forged, 32, back, 4, &count) ==
                  (k == 3 ? PIXELICA_OK : PIXELICA_ERR_BAD_ASSET));
        }
        /* Header fields: version, reserved, count. */
        forged[4] = 2;
        CHECK(pixelica_input_decode_count(forged, 32, &count) == PIXELICA_ERR_BAD_ASSET);
        forged[4] = 1;
        forged[6] = 1;
        CHECK(pixelica_input_decode_count(forged, 32, &count) == PIXELICA_ERR_BAD_ASSET);
        forged[6] = 0;
        forged[8] = 3;
        CHECK(pixelica_input_decode_count(forged, 32, &count) == PIXELICA_ERR_BAD_ASSET);
        forged[8] = 0xFF;
        forged[11] = 0xFF;
        CHECK(pixelica_input_decode_count(forged, 32, &count) == PIXELICA_ERR_BAD_ASSET);
        forged[8] = 2;
        forged[11] = 0;
        CHECK(pixelica_input_decode_count(forged, 32, &count) == PIXELICA_OK);
    }
}

#ifdef PIXELICA_TEST_WITH_3D
/* ---- deterministic replay (needs the 3D core) -------------------------- */

/* Scripted per-tick input; all spans have even lengths so that a 30 Hz
 * schedule (two ticks per pixelica_update call sharing one sample) still sees
 * exactly the same mask on every tick. */
static const PIXELICA_InputSpan k_script[9] = {
    {0u, 20u},
    {PIXELICA_BTN_RIGHT, 40u},
    {PIXELICA_BTN_RIGHT | PIXELICA_BTN_A, 2u},
    {PIXELICA_BTN_RIGHT, 30u},
    {PIXELICA_BTN_UP | PIXELICA_BTN_LEFT, 16u},
    {PIXELICA_BTN_A, 2u},
    {0u, 4u},
    {PIXELICA_BTN_A | PIXELICA_BTN_START, 6u},
    {PIXELICA_BTN_DOWN, 10u},
};
#define SCRIPT_TICKS 130u

/* Mock PAL whose buttons are the script's mask for the game's next tick. */
typedef struct ScriptPal {
    const Game *game;
    uint32_t reads;
} ScriptPal;

static uint32_t script_mask(uint32_t tick)
{
    uint32_t start = 0;
    size_t i;
    for (i = 0; i < sizeof k_script / sizeof k_script[0]; ++i) {
        if (tick < start + k_script[i].ticks) {
            return k_script[i].buttons;
        }
        start += k_script[i].ticks;
    }
    return 0u;
}

static uint64_t pal_time(void *ud)
{
    (void)ud;
    return 0;
}

static uint32_t pal_buttons(void *ud)
{
    ScriptPal *p = (ScriptPal *)ud;
    p->reads++;
    return script_mask(p->game->ticks);
}

static bool pal_present(void *ud, const uint16_t *px, uint16_t w, uint16_t h,
                        uint32_t stride)
{
    (void)ud;
    (void)px;
    (void)w;
    (void)h;
    (void)stride;
    return true;
}

/* Runs the live session at a render schedule of `frame_us` per frame
 * (rendering a frame between updates), recording every tick. */
static int run_live(uint64_t frame_us, Game *g, PIXELICA_InputRecorder *rec)
{
    static const uint16_t idx[3] = {0, 1, 2};
    static const PIXELICA_Vec3 v[3] = {
        {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
    PIXELICA_Config cfg = pixelica_t_config(16, 12);
    PIXELICA_PlatformOps ops;
    ScriptPal pal;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Mesh mesh;
    float id[16];
    int frames = 0;

    game_reset(g, rec);
    pal.game = g;
    pal.reads = 0;
    ops.userdata = &pal;
    ops.time_us = pal_time;
    ops.read_buttons = pal_buttons;
    ops.present_rgb565 = pal_present;
    ops.log = NULL;
    if (pixelica_create(&cfg, &ops, &ctx) != PIXELICA_OK) {
        return 0;
    }
    pixelica_mat4_identity(id);
    mesh.vertices = v;
    mesh.vertex_count = 3;
    mesh.indices = idx;
    mesh.index_count = 3;
    while (g->ticks < SCRIPT_TICKS && frames < 100000) {
        pixelica_update(ctx, frame_us, game_tick, g);
        mesh.color = (uint16_t)(g->x & 0xFFFF);
        pixelica_begin_frame(ctx, 0);
        (void)pixelica_draw_mesh(ctx, &mesh, id);
        (void)pixelica_end_frame(ctx);
        ++frames;
    }
    pixelica_destroy(ctx);
    return (int)pal.reads;
}

static void test_schedule_independence(void)
{
    /* 30, 60 and 120 Hz rendering (frame times rounded up to whole us). */
    static const uint64_t schedules[3] = {33334u, 16667u, 8334u};
    PIXELICA_InputSpan storage[3][16];
    PIXELICA_InputRecorder rec[3];
    Game live[3], replayed;
    static uint8_t encoded[3][256];
    size_t enc_len[3];
    int reads[3];
    int s;

    for (s = 0; s < 3; ++s) {
        pixelica_input_recorder_init(&rec[s], storage[s], 16);
        reads[s] = run_live(schedules[s], &live[s], &rec[s]);
        CHECK(reads[s] > 0);
        CHECK_EQ_LONG(live[s].ticks, SCRIPT_TICKS);
        CHECK_EQ_LONG(live[s].record_failures, 0);
        /* The recording is exactly the script, whatever the schedule. */
        CHECK(spans_equal(storage[s], rec[s].count, k_script, 9));
        CHECK(rec[s].total_ticks == SCRIPT_TICKS);
        CHECK(pixelica_input_encode(storage[s], rec[s].count, encoded[s],
                               sizeof encoded[s], &enc_len[s]) == PIXELICA_OK);
    }
    /* Different numbers of rendered frames / input samples ... */
    CHECK(reads[0] < reads[1] && reads[1] < reads[2]);
    /* ... but bit-identical game state and recordings. */
    CHECK(game_equal(&live[0], &live[1]));
    CHECK(game_equal(&live[0], &live[2]));
    CHECK(enc_len[0] == enc_len[1] && enc_len[1] == enc_len[2]);
    CHECK(memcmp(encoded[0], encoded[1], enc_len[0]) == 0 &&
          memcmp(encoded[0], encoded[2], enc_len[0]) == 0);
    /* Sanity: the script really exercised the logic. */
    CHECK(live[0].interactions == 3u && live[0].starts == 1u);
    CHECK(live[0].x != 0 && live[0].y == 16);

    /* Replaying the decoded recording without any render loop gives the
     * same state again. */
    {
        PIXELICA_InputSpan decoded[16];
        size_t count = 0;
        CHECK(pixelica_input_decode(encoded[2], enc_len[2], decoded, 16, &count) ==
              PIXELICA_OK);
        game_reset(&replayed, NULL);
        CHECK(pixelica_input_replay(decoded, count, game_tick, &replayed) ==
              SCRIPT_TICKS);
        CHECK(game_equal(&replayed, &live[0]));
    }
}

/* Free-form live input (changing every few frames, not aligned to ticks)
 * at a jittery frame rate: the recording replays to the identical state. */
static uint32_t g_noise_seed;
static uint32_t noise_buttons(void *ud)
{
    (void)ud;
    g_noise_seed = g_noise_seed * 1103515245u + 12345u;
    return (g_noise_seed >> 16) & (PIXELICA_BTN_ALL | 0x300u);
}

static void test_record_replay_fidelity(void)
{
    static const uint64_t jitter[7] = {7000u, 16667u, 40000u, 1000u,
                                       25000u, 16666u, 90000u};
    PIXELICA_InputSpan storage[2048];
    PIXELICA_InputRecorder rec;
    PIXELICA_PlatformOps ops;
    PIXELICA_Config cfg = pixelica_t_config(8, 8);
    PIXELICA_Context *ctx = NULL;
    Game live, replayed;
    int i;

    g_noise_seed = 42u;
    ops.userdata = NULL;
    ops.time_us = pal_time;
    ops.read_buttons = noise_buttons;
    ops.present_rgb565 = pal_present;
    ops.log = NULL;
    CHECK(pixelica_create(&cfg, &ops, &ctx) == PIXELICA_OK);
    if (ctx == NULL) {
        return;
    }
    pixelica_input_recorder_init(&rec, storage, 2048);
    game_reset(&live, &rec);
    for (i = 0; i < 700; ++i) {
        pixelica_update(ctx, jitter[i % 7], game_tick, &live);
    }
    pixelica_destroy(ctx);
    CHECK(live.ticks > 800u);
    CHECK(rec.count > 100u); /* input really changed often */
    CHECK_EQ_LONG(live.record_failures, 0);
    CHECK(rec.total_ticks == live.ticks);

    game_reset(&replayed, NULL);
    CHECK(pixelica_input_replay(storage, rec.count, game_tick, &replayed) ==
          live.ticks);
    CHECK(game_equal(&replayed, &live));

    /* Changing a single recorded tick changes the outcome. */
    if (rec.count > 10) {
        storage[5].buttons ^= PIXELICA_BTN_A;
        game_reset(&replayed, NULL);
        (void)pixelica_input_replay(storage, rec.count, game_tick, &replayed);
        CHECK(!game_equal(&replayed, &live));
    }
}
#endif /* PIXELICA_TEST_WITH_3D */

int main(void)
{
    test_edges();
    test_recorder();
    test_encoding_golden();
    test_encoding_errors();
#ifdef PIXELICA_TEST_WITH_3D
    test_schedule_independence();
    test_record_replay_fidelity();
#endif
    return pixelica_t_finish("test_input");
}
