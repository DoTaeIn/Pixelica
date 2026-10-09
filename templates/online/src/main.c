/*
 * Pixelica online starter - the smallest complete multiplayer game.
 *
 * It uses ONLY <pixelica/online.h> (common + online components; no renderer, no
 * window). The game is "Charge": every player holds a button (a bot does it
 * for you); the HOST counts how many 60 Hz ticks each player held it and gives
 * 1 point per POINT_TICKS held ticks. When the round ends every machine sends
 * its score to the same relay with the same API and prints the top 10:
 *   co-op   the board score is the TEAM total      (everybody shares it)
 *   versus  the board score is the player's own points
 *
 * The three online features, in the order they appear below:
 *   1. room code + mode        pixelica_online_host / pixelica_online_join
 *   2. realtime exchange       pixelica_online_input (guest -> host, level of the
 *                              button) and pixelica_online_state (host -> guests)
 *   3. scores and records      pixelica_online_submit_score / pixelica_online_leaderboard
 *
 * IMPORTANT semantics: input and state are "latest value wins" LEVELS sent at
 * ~30 Hz, not an ordered event stream. A one-tick button press can be lost, so
 * the game uses a held level that the host integrates per tick. If you need
 * events, send a cumulative counter in the input bytes instead.
 *
 * Output protocol (kept identical to examples/hello_online, so launch_local.py
 * works unchanged): the first stdout line is "room=CODE"; the secret player key
 * is never printed.
 *
 *   online_game --host coop|versus [--server URL] [--name N] [--players N]
 *   online_game --join CODE        [--server URL] [--name N]
 * Exit status: 0 ok, 1 runtime failure, 2 usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "pixelica/online.h"

#define MODE_COOP ((uint32_t)PIXELICA_ONLINE_COOP) /* 1 */
#define MODE_VERSUS ((uint32_t)PIXELICA_ONLINE_VERSUS) /* 2 */
#define MAX_PLAYERS 8u
#define TICK_HZ 60u
#define POINT_TICKS 10u
#define PUBLISH_EVERY 2u /* frames, 60 Hz / 2 = 30 Hz */
#define RECORD_SIZE 5u
#define KEY_HEX 64u

/* ---------------------------------------------------------- byte helpers */
/* Big-endian by hand: never put a C struct on the wire. */

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

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------- the game */

typedef struct Game {
    uint8_t mode;          /* MODE_COOP / MODE_VERSUS */
    uint8_t phase;         /* 0 lobby, 1 playing, 2 finished */
    uint16_t round;        /* 1 once the round has started */
    uint32_t tick;
    uint32_t total_ticks;
    uint8_t count;
    uint8_t ids[MAX_PLAYERS];
    uint16_t points[MAX_PLAYERS];
    uint32_t held[MAX_PLAYERS]; /* host only: held ticks so far */
} Game;

/* state: u8 version=1, u8 mode, u8 phase, u16 round, u32 tick, u32 total,
 *        u8 count, count * (u8 id, u16 points)                         */
static size_t encode_state(const Game *g, uint8_t *out, size_t cap)
{
    const size_t need = 14u + (size_t)g->count * 3u;
    size_t i;

    if (cap < need) {
        return 0u;
    }
    out[0] = 1u;
    out[1] = g->mode;
    out[2] = g->phase;
    put_u16(out + 3, g->round);
    put_u32(out + 5, g->tick);
    put_u32(out + 9, g->total_ticks);
    out[13] = g->count;
    for (i = 0u; i < g->count; ++i) {
        out[14u + i * 3u] = g->ids[i];
        put_u16(out + 15u + i * 3u, g->points[i]);
    }
    return need;
}

/* Validates everything first; *g is untouched when this returns false. */
static bool decode_state(const uint8_t *data, size_t size, Game *g)
{
    Game next;
    size_t i;

    if (size < 14u || data[0] != 1u || (data[1] != MODE_COOP && data[1] != MODE_VERSUS) ||
        data[2] > 2u || data[13] > MAX_PLAYERS || size != 14u + (size_t)data[13] * 3u) {
        return false;
    }
    memset(&next, 0, sizeof next);
    next.mode = data[1];
    next.phase = data[2];
    next.round = get_u16(data + 3);
    next.tick = get_u32(data + 5);
    next.total_ticks = get_u32(data + 9);
    next.count = data[13];
    if (next.total_ticks == 0u || next.tick > next.total_ticks) {
        return false;
    }
    for (i = 0u; i < next.count; ++i) {
        next.ids[i] = data[14u + i * 3u];
        next.points[i] = get_u16(data + 15u + i * 3u);
        if (next.ids[i] >= MAX_PLAYERS) {
            return false;
        }
    }
    *g = next;
    return true;
}

static int find_player(const Game *g, uint8_t id)
{
    size_t i;

    for (i = 0u; i < g->count; ++i) {
        if (g->ids[i] == id) {
            return (int)i;
        }
    }
    return -1;
}

/* The bot "holds" for 30 ticks, releases for 30 ticks, shifted per player. */
static uint8_t bot_hold(uint32_t frame, uint8_t self_id)
{
    return (uint8_t)((((frame / 30u) + self_id) & 1u) == 0u ? 1u : 0u);
}

/* Host: make the player list equal to the online snapshot, then count the held ticks. */
static void host_step(Game *g, const PIXELICA_OnlineSnapshot *snap, uint8_t self_id, uint8_t own_hold)
{
    Game next = *g;
    size_t i;

    next.count = 0u;
    for (i = 0u; i < snap->player_count && next.count < MAX_PLAYERS; ++i) {
        const uint8_t id = snap->players[i].id;
        const int old = id < MAX_PLAYERS ? find_player(g, id) : -1;

        if (id >= MAX_PLAYERS) {
            continue;
        }
        next.ids[next.count] = id;
        next.points[next.count] = old >= 0 ? g->points[old] : 0u;
        next.held[next.count] = old >= 0 ? g->held[old] : 0u;
        ++next.count;
    }
    *g = next;
    if (g->phase != 1u) {
        return;
    }
    ++g->tick;
    for (i = 0u; i < snap->player_count; ++i) {
        const PIXELICA_OnlinePlayer *p = &snap->players[i];
        uint8_t hold = 0u;
        int idx;

        if (p->id >= MAX_PLAYERS) {
            continue;
        }
        if (p->id == self_id) {
            hold = own_hold;
        } else if (p->input_size == 2u && p->input[0] == 1u && p->input[1] <= 1u) {
            hold = p->input[1];
        }
        idx = find_player(g, p->id);
        if (idx >= 0 && hold != 0u && ++g->held[idx] % POINT_TICKS == 0u && g->points[idx] < UINT16_MAX) {
            ++g->points[idx];
        }
    }
    if (g->tick >= g->total_ticks) {
        g->phase = 2u;
    }
}

/* The score THIS machine submits: team total in co-op, own points in versus. */
static bool board_score(const Game *g, uint8_t self_id, int32_t *out)
{
    const int me = find_player(g, self_id);
    int32_t total = 0;
    size_t i;

    if (me < 0) {
        return false;
    }
    if (g->mode == MODE_VERSUS) {
        *out = g->points[me];
        return true;
    }
    for (i = 0u; i < g->count; ++i) {
        total += g->points[i];
    }
    *out = total;
    return true;
}

/* ------------------------------------------------------------ utilities */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void say(const char *format, ...)
{
    va_list args;

    fputs("hello_online: ", stdout);
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
}

static uint64_t mono_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter.QuadPart * 1000000u / (uint64_t)frequency.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void sleep_us(uint64_t us)
{
#ifdef _WIN32
    Sleep((DWORD)(us / 1000u));
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
#endif
}

static bool parse_u32(const char *text, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < lo || value > hi) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* ------------------------------------------------- persistent identity */
/*
 * The player key is the SECRET that owns your place on the boards. --key-file
 * reads it when the file exists and creates it (once, never overwritten) after
 * the first connection. It is never printed.
 */

static bool load_key(const char *path, char key_hex[KEY_HEX + 1u], bool *exists)
{
    char buffer[KEY_HEX + 8u];
    FILE *file = fopen(path, "rb");
    size_t length;
    size_t i;

    *exists = false;
    if (file == NULL) {
        return errno == ENOENT; /* missing is fine: it will be created */
    }
    length = fread(buffer, 1u, sizeof buffer, file);
    fclose(file);
    while (length > 0u && (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r')) {
        --length;
    }
    if (length != KEY_HEX) {
        return false;
    }
    for (i = 0u; i < length; ++i) {
        const char c = buffer[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    memcpy(key_hex, buffer, length);
    key_hex[length] = '\0';
    *exists = true;
    return true;
}

static bool save_key(const char *path, const char *key_hex)
{
    FILE *file;
    bool ok;

#ifdef _WIN32
    file = fopen(path, "wbx"); /* exclusive: never overwrite */
#else
    {
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600); /* owner only */

        file = fd >= 0 ? fdopen(fd, "wb") : NULL;
        if (file == NULL && fd >= 0) {
            close(fd);
        }
    }
#endif
    if (file == NULL) {
        return false;
    }
    ok = fprintf(file, "%s\n", key_hex) > 0;
    return fclose(file) == 0 && ok;
}

/* ------------------------------------------------------------- the CLI */

typedef struct Args {
    const char *server, *game, *name, *join, *board, *key_file;
    uint32_t mode;         /* 0 = not hosting */
    uint32_t players, max_players, round_seconds, frames, linger_seconds, timeout_ms;
    bool exit_after_round, has_linger;
} Args;

static const char *value_of(int argc, char **argv, int *i, const char *name)
{
    const size_t length = strlen(name);

    if (strncmp(argv[*i], name, length) != 0) {
        return NULL;
    }
    if (argv[*i][length] == '=') {
        return argv[*i] + length + 1;
    }
    if (argv[*i][length] != '\0') {
        return NULL;
    }
    if (*i + 1 < argc) {
        *i += 1;
        return argv[*i];
    }
    return "";
}

static void usage(const char *program)
{
    printf("usage: %s (--host coop|versus | --join CODE) [--server URL] [--name NAME] [--game ID]\n"
           "  [--players N] [--max-players N] [--round-seconds N] [--board NAME] [--key-file PATH]\n"
           "  [--frames N] [--exit-after-round] [--linger-seconds N] [--timeout-ms N]\n"
           "  --bot and --hidden are accepted for launch_local.py (this starter always plays itself)\n",
           program);
}

/* 0 ok, 1 help, -1 error. */
static int parse_args(int argc, char **argv, Args *a)
{
    int i;

    memset(a, 0, sizeof *a);
    a->server = "http://127.0.0.1:8787"; /* tools/online_server default port */
    a->game = "online_starter";
    a->max_players = 4u;
    a->round_seconds = 10u;
    a->frames = TICK_HZ * 120u;
    a->timeout_ms = 1500u;
    for (i = 1; i < argc; ++i) {
        const char *v;

        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "--bot") == 0 || strcmp(argv[i], "--hidden") == 0) {
            continue;
        } else if (strcmp(argv[i], "--exit-after-round") == 0) {
            a->exit_after_round = true;
        } else if ((v = value_of(argc, argv, &i, "--host")) != NULL) {
            a->mode = strcmp(v, "coop") == 0 ? MODE_COOP : (strcmp(v, "versus") == 0 ? MODE_VERSUS : 0u);
            if (a->mode == 0u) {
                fprintf(stderr, "--host needs coop or versus\n");
                return -1;
            }
        } else if ((v = value_of(argc, argv, &i, "--join")) != NULL) {
            a->join = v;
        } else if ((v = value_of(argc, argv, &i, "--server")) != NULL) {
            a->server = v;
        } else if ((v = value_of(argc, argv, &i, "--game")) != NULL) {
            a->game = v;
        } else if ((v = value_of(argc, argv, &i, "--name")) != NULL) {
            a->name = v;
        } else if ((v = value_of(argc, argv, &i, "--board")) != NULL) {
            a->board = v;
        } else if ((v = value_of(argc, argv, &i, "--key-file")) != NULL) {
            a->key_file = v;
        } else if ((v = value_of(argc, argv, &i, "--players")) != NULL) {
            if (!parse_u32(v, 1u, MAX_PLAYERS, &a->players)) {
                fprintf(stderr, "--players needs 1..8\n");
                return -1;
            }
        } else if ((v = value_of(argc, argv, &i, "--max-players")) != NULL) {
            if (!parse_u32(v, 2u, MAX_PLAYERS, &a->max_players)) {
                fprintf(stderr, "--max-players needs 2..8\n");
                return -1;
            }
        } else if ((v = value_of(argc, argv, &i, "--round-seconds")) != NULL) {
            if (!parse_u32(v, 3u, 300u, &a->round_seconds)) {
                fprintf(stderr, "--round-seconds needs 3..300\n");
                return -1;
            }
        } else if ((v = value_of(argc, argv, &i, "--frames")) != NULL) {
            if (!parse_u32(v, 1u, UINT32_MAX, &a->frames)) {
                fprintf(stderr, "--frames needs a positive integer\n");
                return -1;
            }
        } else if ((v = value_of(argc, argv, &i, "--linger-seconds")) != NULL) {
            if (!parse_u32(v, 0u, 60u, &a->linger_seconds)) {
                fprintf(stderr, "--linger-seconds needs 0..60\n");
                return -1;
            }
            a->has_linger = true;
        } else if ((v = value_of(argc, argv, &i, "--timeout-ms")) != NULL) {
            if (!parse_u32(v, 100u, 10000u, &a->timeout_ms)) {
                fprintf(stderr, "--timeout-ms needs 100..10000\n");
                return -1;
            }
        } else {
            fprintf(stderr, "unknown argument '%s' (try --help)\n", argv[i]);
            return -1;
        }
    }
    if ((a->mode == 0u) == (a->join == NULL)) {
        fprintf(stderr, "give exactly one of --host coop|versus and --join CODE\n");
        return -1;
    }
    return 0;
}

static void print_board(const PIXELICA_OnlineLeaderboard *lb)
{
    size_t count = lb->count;
    size_t i;

    if (count > sizeof lb->entries / sizeof lb->entries[0]) {
        count = sizeof lb->entries / sizeof lb->entries[0];
    }
    say("board name=%s generation=%u status=%d status_text=%s count=%u", lb->board,
        (unsigned)lb->generation, (int)lb->status, pixelica_online_status_string(lb->status),
        (unsigned)count);
    for (i = 0u; i < count; ++i) {
        printf("board %u %s %d\n", (unsigned)(i + 1u), lb->entries[i].name, (int)lb->entries[i].score);
    }
    fflush(stdout);
}

/* -------------------------------------------------------------- main loop */

int main(int argc, char **argv)
{
    static PIXELICA_OnlineSnapshot snap; /* about 3 KB: keep it off the stack */
    static PIXELICA_OnlineLeaderboard lb;
    Args a;
    PIXELICA_OnlineConfig config;
    PIXELICA_OnlineStatus error = 0;
    PIXELICA_Online *online;
    Game game;
    char key_hex[KEY_HEX + 1u] = {0};
    bool key_exists = false;
    bool is_host;
    const char *board;
    uint64_t t0;
    uint32_t frame;
    uint32_t seen_generation = 0u;
    uint32_t generation_at_submit = 0u;
    uint32_t done_frame = 0u;
    uint32_t linger_frames;
    bool submitted = false;
    bool score_acked = false;
    int32_t submitted_score = 0;
    bool final_fetch_sent = false;
    uint32_t final_generation = 0u;
    uint32_t final_deadline = 0u;
    bool done = false;
    bool announced_finish = false;
    int rc = 0;
    const int parsed = parse_args(argc, argv, &a);

    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }
    if (a.key_file != NULL && !load_key(a.key_file, key_hex, &key_exists)) {
        fprintf(stderr, "key file '%s' is not usable\n", a.key_file);
        return 2;
    }
    is_host = a.mode != 0u;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. room code + mode ------------------------------------------------ */
    config = pixelica_online_config_default();
    config.server_url = a.server;
    config.game_id = a.game;
    config.player_name = a.name != NULL ? a.name : (is_host ? "Host" : "Guest");
    config.player_key = key_exists ? key_hex : NULL; /* NULL = new identity */
    config.timeout_ms = a.timeout_ms;
    online = is_host ? pixelica_online_host(&config, (PIXELICA_OnlineMode)a.mode, (uint8_t)a.max_players, &error)
                     : pixelica_online_join(&config, a.join, &error);
    if (online == NULL) {
        fprintf(stderr, "cannot %s: %s\n", is_host ? "create the room" : "join the room",
                pixelica_online_status_string(error));
        return 1;
    }
    if (a.key_file != NULL && !key_exists) {
        if (save_key(a.key_file, pixelica_online_player_key(online))) {
            say("identity saved to %s", a.key_file);
        } else {
            fprintf(stderr, "warning: could not save the player key to '%s'\n", a.key_file);
        }
    }
    printf("room=%s\n", pixelica_online_room_code(online));
    fflush(stdout);

    (void)pixelica_online_snapshot(online, &snap);
    memset(&game, 0, sizeof game);
    game.mode = (uint8_t)snap.mode;
    game.total_ticks = a.round_seconds * TICK_HZ;
    board = a.board != NULL ? a.board : (game.mode == MODE_VERSUS ? "versus" : "coop");
    say("role=%s mode=%s self=%u max_players=%u board=%s game=%s", is_host ? "host" : "guest",
        game.mode == MODE_VERSUS ? "versus" : "coop", (unsigned)snap.self_id,
        (unsigned)snap.max_players, board, a.game);
    (void)pixelica_online_request_board(online, board); /* show the existing top 10 */
    linger_frames = (a.has_linger ? a.linger_seconds : (is_host ? 3u : 0u)) * TICK_HZ;

    /* 2. + 3. one 60 Hz frame per iteration; nothing here blocks ---------- */
    t0 = mono_us();
    for (frame = 0u; frame < a.frames && g_stop == 0; ++frame) {
        const uint64_t due = t0 + (uint64_t)frame * 1000000u / TICK_HZ;
        const uint64_t now = mono_us();
        const uint8_t hold = bot_hold(frame, snap.self_id);
        uint8_t wire[2];
        int32_t score = 0;

        if (now < due) {
            sleep_us(due - now);
        }
        (void)pixelica_online_snapshot(online, &snap);

        if (is_host) {
            const uint8_t before = game.phase;
            uint8_t state[64];
            size_t size;

            if (snap.connected && game.phase == 0u && snap.player_count >= (a.players != 0u ? a.players : 2u)) {
                game.phase = 1u;
                game.round = 1u;
            }
            if (snap.connected) host_step(&game, &snap, snap.self_id, hold);
            if (game.phase != before || frame % PUBLISH_EVERY == 0u) {
                size = encode_state(&game, state, sizeof state);
                if (size > 0u) {
                    (void)pixelica_online_state(online, state, size);
                }
            }
        } else {
            wire[0] = 1u; /* input: u8 version, u8 held level */
            wire[1] = hold;
            (void)pixelica_online_input(online, wire, sizeof wire);
            if (snap.connected && snap.state_size > 0u) {
                (void)decode_state(snap.state, snap.state_size, &game);
            }
        }

        if (game.phase == 2u && !announced_finish) {
            announced_finish = true;
            (void)board_score(&game, snap.self_id, &score);
            say("phase=finished round=%u", (unsigned)game.round);
            say("result round=%u mode=%s your_score=%d players=%u", (unsigned)game.round,
                game.mode == MODE_VERSUS ? "versus" : "coop", (int)score, (unsigned)game.count);
        }

        /* 3. scores: one submit per round; the answer is the refreshed top 10 */
        if (game.phase == 2u && !submitted && seen_generation > 0u && board_score(&game, snap.self_id, &score)) {
            uint8_t record[RECORD_SIZE]; /* u8 version, u8 mode, u8 players, u16 seconds */
            PIXELICA_OnlineStatus status;

            record[0] = 1u;
            record[1] = game.mode;
            record[2] = game.count;
            put_u16(record + 3, (uint16_t)(game.total_ticks / TICK_HZ));
            (void)pixelica_online_leaderboard(online, &lb);
            generation_at_submit = lb.generation;
            status = pixelica_online_submit_score(online, board, score, record, sizeof record);
            if (status != 0) {
                fprintf(stderr, "score not accepted: %s\n", pixelica_online_status_string(status));
                rc = 1;
                break;
            }
            submitted = true;
            submitted_score = score;
        }
        (void)pixelica_online_leaderboard(online, &lb);
        if (lb.generation != seen_generation) {
            seen_generation = lb.generation;
            print_board(&lb);
        }

        if (submitted && !score_acked && seen_generation > generation_at_submit) {
            if (lb.status != PIXELICA_ONLINE_OK) { rc = 1; break; }
            score_acked = true;
            say("score submitted board=%s score=%d", board, (int)submitted_score);
        }
        if (a.exit_after_round && !done && score_acked) {
            done = true;
            done_frame = frame;
        }
        if (done && frame >= done_frame + linger_frames) {
            if (!is_host) break;
            if (!final_fetch_sent) {
                final_fetch_sent = true;
                final_generation = seen_generation;
                final_deadline = frame + TICK_HZ * 3u;
                (void)pixelica_online_request_board(online, board);
            } else if (seen_generation > final_generation) {
                if (lb.status != PIXELICA_ONLINE_OK) rc = 1;
                break;
            } else if (frame >= final_deadline) { rc = 1; break; }
        }
    }

    if (rc == 0 && a.exit_after_round && !done) {
        fprintf(stderr, "the round did not finish (or the board did not arrive) in time\n");
        rc = 1;
    }
    pixelica_online_destroy(online); /* best-effort leave, then free */
    if (rc == 0) {
        say("done frames=%" PRIu32 " round_done=%d", frame, done ? 1 : 0);
    }
    return rc;
}
