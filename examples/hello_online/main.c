/*
 * hello_online - standalone driver (main + command line + frame loop).
 *
 * One executable, one game (online_game.c) and one networking layer
 * (online_session.c) for every build flavour:
 *   - HELLO_ONLINE_SDL defined      SDL3 window, keyboard, renderer2d drawing
 *                                   (needs HELLO_ONLINE_WITH_RENDERER2D);
 *   - HELLO_ONLINE_WITH_RENDERER2D  renders every frame into a static RGB565
 *                                   target (no window); --capture writes a PPM;
 *   - neither (common + online only) pure console: no renderer at all.
 * Without SDL there is no input device, so the client is always an autopilot bot.
 *
 * Usage:
 *   hello_online --host coop|versus [--server URL] [--name N] [--players N] ...
 *   hello_online --join CODE        [--server URL] [--name N] ...
 *   hello_online --score N --board NAME   (board only: submit/fetch, no game)
 *
 * The room code is printed first, alone on a line ("room=ABCDEF"), and stdout is
 * flushed, so a launcher or test can read it. The player key (the secret that
 * owns a ranking identity) is NEVER printed; --key-file keeps it in a file.
 *
 * Exit status: 0 success, 1 runtime failure (network, round not finished, score
 * rejected), 2 usage error.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen for the key file / --capture */
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime, nanosleep, open, fdopen */
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "online_game.h"
#include "online_session.h"

#if defined(HELLO_ONLINE_SDL) && !defined(HELLO_ONLINE_WITH_RENDERER2D)
#error "HELLO_ONLINE_SDL needs HELLO_ONLINE_WITH_RENDERER2D"
#endif
#ifdef HELLO_ONLINE_WITH_RENDERER2D
#include "online_view.h"
#endif
#ifdef HELLO_ONLINE_SDL
#include "pixelica/input.h"
#include "platform_sdl3.h"
#endif

#define DEFAULT_SERVER "http://127.0.0.1:8787" /* tools/online_server default port */
#define DEFAULT_GAME "hello_online"
#define HEADLESS_DEFAULT_FRAMES (HOL_TICK_HZ * 180u)
#define BOARD_WAIT_FRAMES (HOL_TICK_HZ * 6u)
#define DISCONNECT_FAIL_FRAMES (HOL_TICK_HZ * 10u)
#define MAX_PATH_BYTES 4095u
#define KEY_HEX_LENGTH 64u

typedef struct Args {
    const char *server;
    const char *game;
    const char *name;
    const char *board;
    const char *join;
    const char *key_file;
    const char *capture;
    bool host;
    HolMode mode;
    bool has_score;
    int32_t score;
    bool hidden;
    bool bot;
    bool exit_after_round;
    bool allow_insecure_lan;
    bool has_frames;
    uint32_t frames;
    bool has_players;
    uint32_t players;
    uint32_t max_players;
    uint32_t round_seconds;
    bool has_linger;
    uint32_t linger_seconds;
    uint32_t timeout_ms;
    uint32_t sync_hz;
} Args;

static volatile sig_atomic_t g_stop = 0;

#ifdef HELLO_ONLINE_WITH_RENDERER2D
static uint16_t g_target[HOL_VIEW_WIDTH * HOL_VIEW_HEIGHT];
#endif

/* ------------------------------------------------------------ utilities */

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
    if (errno != 0 || end == text || *end != '\0' || value < lo || value > hi) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_i32(const char *text, int32_t *out)
{
    char *end = NULL;
    long long value;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

/* "--name value" or "--name=value"; NULL if argv[*i] is another option. */
static const char *option_value(int argc, char **argv, int *i, const char *name)
{
    const char *arg = argv[*i];
    const size_t length = strlen(name);

    if (strncmp(arg, name, length) != 0) {
        return NULL;
    }
    if (arg[length] == '=') {
        return arg + length + 1;
    }
    if (arg[length] != '\0') {
        return NULL;
    }
    if (*i + 1 < argc) {
        *i += 1;
        return argv[*i];
    }
    return "";
}

static void print_usage(const char *program)
{
    printf("usage: %s (--host coop|versus | --join CODE | --score N [--board NAME]) [options]\n"
           "  --host coop|versus   create a room (co-op: shared team score, versus: own score)\n"
           "  --join CODE          join a room by its 6 character code\n"
           "  --server URL         relay, default %s (https://... for public servers)\n"
           "  --game ID            game id, partitions the boards, default %s\n"
           "  --name NAME          player name (1..32 bytes), default Host/Guest/Player\n"
           "  --players N          host: start the first round automatically at N players\n"
           "  --max-players N      host: room capacity 2..8, default 4\n"
           "  --round-seconds N    host: round length %u..%u, default %u\n"
           "  --board NAME         score board [A-Za-z0-9_.-]{1,32}, default coop/versus\n"
           "  --score N            submit N instead of the played score (board-only without a room)\n"
           "  --bot                autopilot instead of the keyboard (always on without SDL)\n"
           "  --frames N           run at most N 60 Hz frames (default: unlimited with SDL)\n"
           "  --exit-after-round   exit 0 once the round is over and the board arrived\n"
           "  --linger-seconds N   with --exit-after-round: stay N more seconds (host default 3)\n"
           "  --key-file PATH      load/save the secret player key (never printed) for a stable identity\n"
           "  --allow-insecure-lan allow plain http:// to a private LAN address (not loopback)\n"
           "  --timeout-ms N       request timeout 100..10000, default 1500\n"
           "  --sync-hz N          network exchange rate 1..60, default 30\n"
           "  --hidden             SDL: no window (dummy video driver)\n"
           "  --capture PATH       write the last frame as a binary PPM (needs a renderer build)\n"
           "keys (SDL): arrows/WASD move, Space dash, Enter start/play again, Escape quit\n"
           "stdout: first line 'room=CODE'; the secret player key is never printed.\n",
           program != NULL ? program : "hello_online", DEFAULT_SERVER, DEFAULT_GAME,
           HOL_MIN_ROUND_SECONDS, HOL_MAX_ROUND_SECONDS, HOL_DEFAULT_ROUND_SECONDS);
}

/* 0 = parsed, 1 = help printed, -1 = error. */
static int parse_args(int argc, char **argv, Args *out)
{
    int i;

    memset(out, 0, sizeof *out);
    out->server = DEFAULT_SERVER;
    out->game = DEFAULT_GAME;
    out->max_players = 4u;
    out->round_seconds = HOL_DEFAULT_ROUND_SECONDS;
    out->timeout_ms = 1500u;
    out->sync_hz = 30u;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        } else if (strcmp(arg, "--hidden") == 0) {
            out->hidden = true;
        } else if (strcmp(arg, "--bot") == 0) {
            out->bot = true;
        } else if (strcmp(arg, "--exit-after-round") == 0) {
            out->exit_after_round = true;
        } else if (strcmp(arg, "--allow-insecure-lan") == 0) {
            out->allow_insecure_lan = true;
        } else if ((value = option_value(argc, argv, &i, "--host")) != NULL) {
            if (strcmp(value, "coop") == 0) {
                out->mode = HOL_MODE_COOP;
            } else if (strcmp(value, "versus") == 0) {
                out->mode = HOL_MODE_VERSUS;
            } else {
                fprintf(stderr, "--host needs coop or versus (got '%s')\n", value);
                return -1;
            }
            out->host = true;
        } else if ((value = option_value(argc, argv, &i, "--join")) != NULL) {
            out->join = value;
        } else if ((value = option_value(argc, argv, &i, "--server")) != NULL) {
            out->server = value;
        } else if ((value = option_value(argc, argv, &i, "--game")) != NULL) {
            out->game = value;
        } else if ((value = option_value(argc, argv, &i, "--name")) != NULL) {
            out->name = value;
        } else if ((value = option_value(argc, argv, &i, "--board")) != NULL) {
            out->board = value;
        } else if ((value = option_value(argc, argv, &i, "--key-file")) != NULL) {
            out->key_file = value;
        } else if ((value = option_value(argc, argv, &i, "--capture")) != NULL) {
            out->capture = value;
        } else if ((value = option_value(argc, argv, &i, "--score")) != NULL) {
            if (!parse_i32(value, &out->score)) {
                fprintf(stderr, "--score needs a 32-bit integer (got '%s')\n", value);
                return -1;
            }
            out->has_score = true;
        } else if ((value = option_value(argc, argv, &i, "--frames")) != NULL) {
            if (!parse_u32(value, 1u, UINT32_MAX, &out->frames)) {
                fprintf(stderr, "--frames needs a positive integer (got '%s')\n", value);
                return -1;
            }
            out->has_frames = true;
        } else if ((value = option_value(argc, argv, &i, "--players")) != NULL) {
            if (!parse_u32(value, 1u, HOL_MAX_PLAYERS, &out->players)) {
                fprintf(stderr, "--players needs an integer in 1..8 (got '%s')\n", value);
                return -1;
            }
            out->has_players = true;
        } else if ((value = option_value(argc, argv, &i, "--max-players")) != NULL) {
            if (!parse_u32(value, 2u, HOL_MAX_PLAYERS, &out->max_players)) {
                fprintf(stderr, "--max-players needs an integer in 2..8 (got '%s')\n", value);
                return -1;
            }
        } else if ((value = option_value(argc, argv, &i, "--round-seconds")) != NULL) {
            if (!parse_u32(value, HOL_MIN_ROUND_SECONDS, HOL_MAX_ROUND_SECONDS,
                           &out->round_seconds)) {
                fprintf(stderr, "--round-seconds needs an integer in %u..%u (got '%s')\n",
                        HOL_MIN_ROUND_SECONDS, HOL_MAX_ROUND_SECONDS, value);
                return -1;
            }
        } else if ((value = option_value(argc, argv, &i, "--linger-seconds")) != NULL) {
            if (!parse_u32(value, 0u, 60u, &out->linger_seconds)) {
                fprintf(stderr, "--linger-seconds needs an integer in 0..60 (got '%s')\n", value);
                return -1;
            }
            out->has_linger = true;
        } else if ((value = option_value(argc, argv, &i, "--timeout-ms")) != NULL) {
            if (!parse_u32(value, 100u, 10000u, &out->timeout_ms)) {
                fprintf(stderr, "--timeout-ms needs an integer in 100..10000 (got '%s')\n", value);
                return -1;
            }
        } else if ((value = option_value(argc, argv, &i, "--sync-hz")) != NULL) {
            if (!parse_u32(value, 1u, 60u, &out->sync_hz)) {
                fprintf(stderr, "--sync-hz needs an integer in 1..60 (got '%s')\n", value);
                return -1;
            }
        } else {
            fprintf(stderr, "unknown argument '%s' (try --help)\n", arg);
            return -1;
        }
    }
    return 0;
}

static bool valid_text_option(const char *option, const char *value, size_t max_bytes)
{
    size_t i;

    if (value == NULL || value[0] == '\0' || strlen(value) > max_bytes) {
        fprintf(stderr, "%s needs 1..%u bytes\n", option, (unsigned)max_bytes);
        return false;
    }
    for (i = 0u; value[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)value[i];

        if (c < 0x20u || c == 0x7Fu) {
            fprintf(stderr, "%s must not contain control characters\n", option);
            return false;
        }
    }
    return true;
}

static bool valid_game_id(const char *game)
{
    return hol_valid_board_name(game); /* same alphabet and length rule */
}

/* Validates the combination of options; -1 = error message printed. */
static int validate_args(Args *args, char room_code[7])
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    size_t i;

    if (args->host && args->join != NULL) {
        fprintf(stderr, "--host and --join cannot be combined\n");
        return -1;
    }
    if (!args->host && args->join == NULL && !args->has_score && args->board == NULL) {
        fprintf(stderr, "give --host coop|versus, --join CODE, or --score N / --board NAME\n");
        return -1;
    }
    if (!valid_text_option("--server", args->server, 512u) ||
        !valid_text_option("--name", args->name != NULL ? args->name : "x", 32u) ||
        !valid_text_option("--game", args->game, 32u) ||
        (args->key_file != NULL && !valid_text_option("--key-file", args->key_file, MAX_PATH_BYTES)) ||
        (args->capture != NULL && !valid_text_option("--capture", args->capture, MAX_PATH_BYTES))) {
        return -1;
    }
    if (!valid_game_id(args->game)) {
        fprintf(stderr, "--game must match [A-Za-z0-9_.-]{1,32}\n");
        return -1;
    }
    if (args->board != NULL && !hol_valid_board_name(args->board)) {
        fprintf(stderr, "--board must match [A-Za-z0-9_.-]{1,32}\n");
        return -1;
    }
    if (args->join != NULL) {
        if (strlen(args->join) != 6u) {
            fprintf(stderr, "--join needs a 6 character room code\n");
            return -1;
        }
        for (i = 0u; i < 6u; ++i) {
            char c = args->join[i];

            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            if (strchr(alphabet, c) == NULL || c == '\0') {
                fprintf(stderr, "--join: '%c' is not a room code character (A-Z without I,O; 2-9)\n",
                        args->join[i]);
                return -1;
            }
            room_code[i] = c;
        }
        room_code[6] = '\0';
    }
    if (args->host && args->has_players && args->players > args->max_players) {
        fprintf(stderr, "--players %u is larger than --max-players %u\n",
                (unsigned)args->players, (unsigned)args->max_players);
        return -1;
    }
#ifndef HELLO_ONLINE_WITH_RENDERER2D
    if (args->capture != NULL) {
        fprintf(stderr, "--capture needs a build with the renderer2d component\n");
        return -1;
    }
#endif
    return 0;
}

/* -------------------------------------------------------------- key file */

typedef struct KeyFile {
    char hex[KEY_HEX_LENGTH + 1u];
    bool loaded;
    bool create_after_open;
} KeyFile;

/* 0 = ok (loaded or to be created), -1 = unusable file. Never prints the key. */
static int key_file_load(const char *path, KeyFile *out)
{
    FILE *file;
    char buffer[KEY_HEX_LENGTH + 8u];
    size_t length;
    size_t i;

    memset(out, 0, sizeof *out);
    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            out->create_after_open = true;
            return 0;
        }
        fprintf(stderr, "cannot read key file '%s'\n", path);
        return -1;
    }
    length = fread(buffer, 1u, sizeof buffer, file);
    fclose(file);
    while (length > 0u && (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r' ||
                           buffer[length - 1u] == ' ')) {
        --length;
    }
    if (length != KEY_HEX_LENGTH) {
        fprintf(stderr, "key file '%s' must hold exactly %u hex characters\n", path,
                (unsigned)KEY_HEX_LENGTH);
        return -1;
    }
    for (i = 0u; i < length; ++i) {
        const char c = buffer[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            fprintf(stderr, "key file '%s' must hold exactly %u hex characters\n", path,
                    (unsigned)KEY_HEX_LENGTH);
            return -1;
        }
    }
    memcpy(out->hex, buffer, length);
    out->hex[length] = '\0';
    out->loaded = true;
    return 0;
}

/* Creates the file exclusively (owner-only on POSIX). Never overwrites. */
static bool key_file_save(const char *path, const char *key_hex)
{
    FILE *file;
    bool ok;

#ifdef _WIN32
    file = fopen(path, "wbx");
#else
    {
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

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
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

/* ------------------------------------------------------------ SDL / draw */

typedef struct Frontend {
    bool bot;
    uint8_t held_start; /* previous START state for edge detection */
#ifdef HELLO_ONLINE_WITH_RENDERER2D
    HolDraw draw;
#endif
#ifdef HELLO_ONLINE_SDL
    PIXELICA_Sdl3Platform *platform;
    PIXELICA_PlatformOps ops;
#endif
} Frontend;

#ifdef HELLO_ONLINE_WITH_RENDERER2D
static bool write_ppm(const char *path, const uint16_t *pixels)
{
    uint8_t row[HOL_VIEW_WIDTH * 3u];
    FILE *file = fopen(path, "wb");
    size_t y;
    size_t x;
    bool ok = true;

    if (file == NULL) {
        fprintf(stderr, "cannot open '%s' for writing\n", path);
        return false;
    }
    if (fprintf(file, "P6\n%u %u\n255\n", (unsigned)HOL_VIEW_WIDTH, (unsigned)HOL_VIEW_HEIGHT) < 0) {
        ok = false;
    }
    for (y = 0u; ok && y < HOL_VIEW_HEIGHT; ++y) {
        for (x = 0u; x < HOL_VIEW_WIDTH; ++x) {
            const uint32_t p = pixels[y * HOL_VIEW_WIDTH + x];
            const uint32_t r = (p >> 11) & 0x1Fu;
            const uint32_t g = (p >> 5) & 0x3Fu;
            const uint32_t b = p & 0x1Fu;

            row[x * 3u + 0u] = (uint8_t)((r << 3) | (r >> 2));
            row[x * 3u + 1u] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3u + 2u] = (uint8_t)((b << 3) | (b >> 2));
        }
        if (fwrite(row, 1u, sizeof row, file) != sizeof row) {
            ok = false;
        }
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "failed while writing '%s'\n", path);
    }
    return ok;
}
#endif

static bool frontend_open(Frontend *f, const Args *args)
{
    memset(f, 0, sizeof *f);
#ifdef HELLO_ONLINE_SDL
    {
        PIXELICA_Sdl3Config config;

        config.title = "Pixelica Hello Online";
        config.width = (uint16_t)HOL_VIEW_WIDTH;
        config.height = (uint16_t)HOL_VIEW_HEIGHT;
        config.hidden = args->hidden;
        if (!pixelica_sdl3_create(&config, &f->platform)) {
            fprintf(stderr, "failed to create the SDL3 platform\n");
            return false;
        }
        f->ops = pixelica_sdl3_ops(f->platform);
    }
    f->bot = args->bot;
#else
    (void)args;
    f->bot = true; /* no input device in this build */
#endif
#ifdef HELLO_ONLINE_WITH_RENDERER2D
    if (!hol_draw_init(&f->draw, g_target)) {
        fprintf(stderr, "renderer2d init failed\n");
        return false;
    }
#endif
    return true;
}

static void frontend_close(Frontend *f)
{
#ifdef HELLO_ONLINE_SDL
    pixelica_sdl3_destroy(f->platform);
    f->platform = NULL;
#else
    (void)f;
#endif
}

/* false = quit requested (window closed / Escape / signal). */
static bool frontend_input(Frontend *f, const HolSession *s, uint32_t frame, uint8_t *buttons,
                           bool *start)
{
    *buttons = 0u;
    *start = false;
    if (g_stop != 0) {
        return false;
    }
#ifdef HELLO_ONLINE_SDL
    if (!pixelica_sdl3_pump_events(f->platform)) {
        return false;
    }
    {
        const uint32_t keys = f->ops.read_buttons(f->ops.userdata);

        if ((keys & PIXELICA_BTN_QUIT) != 0u) {
            return false;
        }
        if (!f->bot) {
            if ((keys & PIXELICA_BTN_LEFT) != 0u) {
                *buttons |= HOL_IN_LEFT;
            }
            if ((keys & PIXELICA_BTN_RIGHT) != 0u) {
                *buttons |= HOL_IN_RIGHT;
            }
            if ((keys & PIXELICA_BTN_UP) != 0u) {
                *buttons |= HOL_IN_UP;
            }
            if ((keys & PIXELICA_BTN_DOWN) != 0u) {
                *buttons |= HOL_IN_DOWN;
            }
            if ((keys & PIXELICA_BTN_A) != 0u) {
                *buttons |= HOL_IN_DASH;
            }
        }
        {
            const uint8_t now = (keys & PIXELICA_BTN_START) != 0u ? 1u : 0u;

            *start = now != 0u && f->held_start == 0u;
            f->held_start = now;
        }
    }
#endif
    if (f->bot && s->have_view) {
        *buttons = hol_bot_buttons(&s->view, s->self_id, frame);
    }
    return true;
}

static bool frontend_present(Frontend *f, const HolSession *s)
{
#ifdef HELLO_ONLINE_WITH_RENDERER2D
    const PIXELICA_Status status = hol_draw_frame(&f->draw, s, f->bot);

    if (status != PIXELICA_OK) {
        fprintf(stderr, "render failed (PIXELICA_Status %d)\n", (int)status);
        return false;
    }
#ifdef HELLO_ONLINE_SDL
    if (!f->ops.present_rgb565(f->ops.userdata, g_target, (uint16_t)HOL_VIEW_WIDTH,
                               (uint16_t)HOL_VIEW_HEIGHT, HOL_VIEW_WIDTH)) {
        fprintf(stderr, "present failed\n");
        return false;
    }
#endif
#else
    (void)f;
    (void)s;
#endif
    return true;
}

/* -------------------------------------------------------------- reporting */

static void print_board(const HolSession *s)
{
    const PIXELICA_OnlineLeaderboard *lb = &s->leaderboard;
    size_t count = lb->count;
    size_t i;

    if (count > sizeof lb->entries / sizeof lb->entries[0]) {
        count = sizeof lb->entries / sizeof lb->entries[0];
    }
    say("board name=%s generation=%u status=%d status_text=%s count=%u", s->board,
        (unsigned)lb->generation, (int)lb->status, pixelica_online_status_string(lb->status),
        (unsigned)count);
    for (i = 0u; i < count; ++i) {
        const PIXELICA_OnlineScore *e = &lb->entries[i];
        HolRecord rec;

        if (hol_record_decode(e->record, e->record_size, &rec)) {
            printf("board %u %s %d mode=%s players=%u seconds=%u\n", (unsigned)(i + 1u), e->name,
                   (int)e->score, rec.mode == HOL_MODE_COOP ? "coop" : "versus",
                   (unsigned)rec.players, (unsigned)rec.round_seconds);
        } else {
            printf("board %u %s %d\n", (unsigned)(i + 1u), e->name, (int)e->score);
        }
    }
    fflush(stdout);
}

static void fill_config(PIXELICA_OnlineConfig *config, const Args *args, const KeyFile *key,
                        const char *default_name)
{
    *config = pixelica_online_config_default();
    config->server_url = args->server;
    config->game_id = args->game;
    config->player_name = args->name != NULL ? args->name : default_name;
    config->player_key = key->loaded ? key->hex : NULL;
    config->timeout_ms = args->timeout_ms;
    config->sync_hz = args->sync_hz;
    config->allow_insecure_lan = args->allow_insecure_lan;
}

/* ---------------------------------------------------------------- run loop */

typedef struct LoopResult {
    uint32_t frames;
    bool round_done;
    int rc;
} LoopResult;

static LoopResult run_game_loop(const Args *args, Frontend *front, HolSession *s)
{
    LoopResult result = {0u, false, 0};
    const uint32_t linger_seconds =
        args->has_linger ? args->linger_seconds : (s->is_host ? 3u : 0u);
    const uint32_t frame_limit =
        args->has_frames ? args->frames
                         : (front->bot || args->hidden ? HEADLESS_DEFAULT_FRAMES : 0u);
    uint64_t t0 = mono_us();
    uint32_t frame = 0u;
    HolPhase last_phase = HOL_PHASE_LOBBY;
    uint16_t last_round = 0u;
    uint8_t last_players = 0u;
    bool last_connected = true;
    bool was_submitted = false;
    uint16_t reported_round = 0u;
    uint32_t generation_at_submit = 0u;
    uint32_t done_frame = 0u;
    bool done = false;
    bool final_fetch_sent = false;
    uint32_t final_generation = 0u;
    uint32_t final_deadline = 0u;
    uint32_t disconnected_frames = 0u;

    for (;; ++frame) {
        const uint64_t due = t0 + (uint64_t)frame * 1000000u / HOL_TICK_HZ;
        const uint64_t now = mono_us();
        uint8_t buttons;
        bool start;

        if (now < due) {
            sleep_us(due - now);
        } else if (now - due > 250000u) {
            t0 += now - due; /* far behind (suspended?): do not fast-forward */
        }
        if (!frontend_input(front, s, frame, &buttons, &start)) {
            break;
        }
        hol_session_tick(s, buttons, start);

        if (s->have_view) {
            if (s->view.phase != last_phase || s->view.round != last_round) {
                static const char *const names[] = {"lobby", "playing", "finished"};

                say("phase=%s round=%u", names[s->view.phase], (unsigned)s->view.round);
                if (s->view.phase == HOL_PHASE_FINISHED) {
                    int32_t score = 0;

                    (void)hol_view_board_score(&s->view, s->self_id, &score);
                    say("result round=%u mode=%s team_score=%u your_score=%d players=%u",
                        (unsigned)s->view.round, s->view.mode == HOL_MODE_COOP ? "coop" : "versus",
                        (unsigned)s->view.team_score, (int)score, (unsigned)s->view.player_count);
                }
                last_phase = s->view.phase;
                last_round = s->view.round;
            }
        }
        if (s->player_count != last_players) {
            say("players=%u/%u", (unsigned)s->player_count, (unsigned)s->max_players);
            last_players = s->player_count;
        }
        if (s->connected != last_connected) {
            if (s->connected) {
                say("connected again");
            } else {
                say("disconnected (status %s)", pixelica_online_status_string(s->last_status));
            }
            last_connected = s->connected;
        }
        disconnected_frames = s->connected ? 0u : disconnected_frames + 1u;
        if (s->submitted && (!was_submitted || s->submitted_round != reported_round)) {
            was_submitted = true;
            reported_round = s->submitted_round;
            generation_at_submit = s->submit_generation;
            if (s->submit_error != 0) {
                say("score not accepted: %s", pixelica_online_status_string(s->submit_error));
            } else {
                say("score submitted board=%s score=%d", s->board, (int)s->submitted_score);
            }
        }
        if (s->board_updated) {
            s->board_updated = false;
            print_board(s);
        }

        if (!frontend_present(front, s)) {
            result.rc = 1;
            break;
        }
        result.frames = frame + 1u;

        if (args->exit_after_round) {
            if (s->submitted && s->submit_error != 0) {
                result.rc = 1;
                break;
            }
            if (!done && s->have_view && s->view.phase == HOL_PHASE_FINISHED && s->submitted &&
                s->submitted_round == s->view.round && s->seen_generation > generation_at_submit) {
                done = true;
                done_frame = frame;
                result.round_done = true;
            }
            if (done && frame >= done_frame + linger_seconds * HOL_TICK_HZ) {
                if (linger_seconds == 0u) {
                    break;
                }
                /* After lingering, read the board once more so it includes late guests. */
                if (!final_fetch_sent) {
                    final_fetch_sent = true;
                    final_generation = s->seen_generation;
                    final_deadline = frame + HOL_TICK_HZ * 3u;
                    (void)hol_session_request_board(s);
                } else if (s->seen_generation > final_generation || frame >= final_deadline) {
                    break;
                }
            }
        }
        if (disconnected_frames >= DISCONNECT_FAIL_FRAMES && (front->bot || args->exit_after_round)) {
            fprintf(stderr, "the relay stayed unreachable for %u seconds\n",
                    (unsigned)(DISCONNECT_FAIL_FRAMES / HOL_TICK_HZ));
            result.rc = 1;
            break;
        }
        if (frame_limit != 0u && frame + 1u >= frame_limit) {
            result.frames = frame + 1u;
            break;
        }
    }

    if (result.rc == 0 && args->exit_after_round && !result.round_done) {
        fprintf(stderr, "the round did not finish (and the board did not arrive) within %u frames\n",
                (unsigned)result.frames);
        result.rc = 1;
    }
    return result;
}

/* Board only: a throwaway 2-player room is just the way to get a session token. */
static int run_board_only(const Args *args, HolSession *s)
{
    uint32_t waited = 0u;

    /* 1. the board request queued while opening completes */
    while (s->seen_generation < 1u && waited < BOARD_WAIT_FRAMES) {
        sleep_us(1000000u / HOL_TICK_HZ);
        hol_session_tick(s, 0u, false);
        ++waited;
    }
    if (s->seen_generation < 1u) {
        fprintf(stderr, "the board request did not complete\n");
        return 1;
    }
    if (args->has_score) {
        HolView view;
        uint8_t record[HOL_RECORD_SIZE];
        PIXELICA_OnlineStatus status;

        memset(&view, 0, sizeof view);
        view.mode = HOL_MODE_COOP;
        view.player_count = 1u;
        view.round_ticks = HOL_DEFAULT_ROUND_SECONDS * HOL_TICK_HZ;
        hol_record_encode(&view, record);
        status = hol_session_submit_now(s, args->score, record, sizeof record);
        if (status != 0) {
            fprintf(stderr, "score not accepted: %s\n", pixelica_online_status_string(status));
            return 1;
        }
        say("score submitted board=%s score=%d", s->board, (int)args->score);
        /* 2. the submit completion returns the refreshed top 10 */
        waited = 0u;
        while (s->seen_generation < 2u && waited < BOARD_WAIT_FRAMES) {
            sleep_us(1000000u / HOL_TICK_HZ);
            hol_session_tick(s, 0u, false);
            ++waited;
        }
        if (s->seen_generation < 2u) {
            fprintf(stderr, "the score result did not arrive\n");
            return 1;
        }
    }
    print_board(s);
    if (s->leaderboard.status != 0) {
        fprintf(stderr, "board error: %s\n", pixelica_online_status_string(s->leaderboard.status));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static HolSession session; /* large (snapshot + leaderboard): not on the stack */
    Args args;
    KeyFile key = {{0}, false, false};
    Frontend front;
    PIXELICA_OnlineConfig config;
    PIXELICA_OnlineStatus error = 0;
    char room_code[7] = {0};
    LoopResult result;
    bool board_only;
    bool opened;
    int rc;
    const int parsed = parse_args(argc, argv, &args);

    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }
    if (validate_args(&args, room_code) != 0 || key_file_load(args.key_file, &key) != 0) {
        return 2;
    }
    board_only = !args.host && args.join == NULL;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fill_config(&config, &args, &key,
                args.host ? "Host" : (args.join != NULL ? "Guest" : "Player"));
    if (args.join != NULL) {
        opened = hol_session_join(&session, &config, room_code, args.board, &error);
    } else {
        const HolMode mode = args.host ? args.mode : HOL_MODE_COOP;
        const uint8_t max_players = board_only ? 2u : (uint8_t)args.max_players;

        opened = hol_session_host(&session, &config, mode, max_players, args.round_seconds,
                                  args.board, &error);
    }
    if (!opened) {
        fprintf(stderr, "cannot %s: %s\n", args.join != NULL ? "join the room" : "create the room",
                pixelica_online_status_string(error));
        return 1;
    }

    /* Persist the (secret) identity only if asked; never print it. */
    if (key.create_after_open) {
        const char *secret = pixelica_online_player_key(session.online);

        if (secret != NULL && key_file_save(args.key_file, secret)) {
            say("identity saved to %s", args.key_file);
        } else {
            fprintf(stderr, "warning: could not save the player key to '%s'\n", args.key_file);
        }
    } else if (key.loaded) {
        say("identity loaded from %s", args.key_file);
    }

    printf("room=%s\n", session.room_code);
    fflush(stdout);
    say("role=%s mode=%s self=%u max_players=%u board=%s game=%s",
        session.is_host ? "host" : "guest",
        session.snap.mode == PIXELICA_ONLINE_VERSUS ? "versus" : "coop",
        (unsigned)session.self_id, (unsigned)session.max_players, session.board, args.game);

    if (board_only) {
        rc = run_board_only(&args, &session);
        hol_session_close(&session);
        return rc;
    }

    if (!frontend_open(&front, &args)) {
        hol_session_close(&session);
        return 1;
    }
    if (session.is_host) {
        const bool autopilot = front.bot;

        session.want_players = args.has_players ? args.players : (autopilot ? 2u : 0u);
    }
    if (args.has_score) {
        hol_session_force_score(&session, args.score);
    }

    result = run_game_loop(&args, &front, &session);
    rc = result.rc;
#ifdef HELLO_ONLINE_WITH_RENDERER2D
    if (rc == 0 && args.capture != NULL && !write_ppm(args.capture, g_target)) {
        rc = 1;
    }
#endif
    hol_session_close(&session);
    frontend_close(&front);
    if (rc == 0) {
        say("done frames=%" PRIu32 " round_done=%d", result.frames, result.round_done ? 1 : 0);
    }
    return rc;
}
