/*
 * pixel_room_capture: headless screenshot + statistics + wall-clock benchmark
 * for the Pixel Room, with its own main() and no SDL.
 *
 *   pixel_room_capture [--profile tiny|standard] [--script SPEC] [--yaw RAD]
 *                      [--teleport X,Z] [--ppm FILE] [--scale N]
 *                      [--json FILE|-] [--bench FRAMES] [--warmup FRAMES]
 *
 * It builds the room from the embedded assets, optionally moves the cube and
 * camera, plays a scripted input at the fixed 60 Hz step, renders one frame
 * and reports what is actually in it: the PPM is the real RGB565 buffer
 * expanded to 8 bits, and the JSON carries the renderer's own frame counters.
 *
 * Script SPEC: comma separated NAME[+NAME...]:TICKS, NAME being one of LEFT,
 * RIGHT, UP, DOWN, A, START, NONE. Example:
 *   "UP:90,A:2,NONE:30,START+RIGHT:60"
 *
 * --bench N renders N more frames after --warmup (default 10), each preceded
 * by one update (the script, or a built-in tour when none is given, loops).
 * Times come from a monotonic clock (QueryPerformanceCounter on Windows,
 * clock_gettime(CLOCK_MONOTONIC) elsewhere; the JSON names the one used), a
 * wall-time measurement on whatever machine runs the tool, not CPU time. The
 * benchmark fails if no monotonic clock exists or it steps backwards. Nothing
 * here is a hardware claim; the JSON states what was measured and how.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "pixel_room.h"
#include "room_embedded.h"

#define MAX_SEGMENTS 4096u
#define MAX_SCRIPT_TICKS 100000000ull
#define MAX_BENCH_FRAMES 5000000ull
#define MAX_WARMUP_FRAMES 1000000ull
#define MAX_SCALE 16u

#define DEFAULT_TOUR \
    "UP:60,RIGHT:60,A:2,NONE:20,START+RIGHT:90,DOWN:60,LEFT:60,A:2,NONE:20,START+LEFT:90"

typedef struct Segment {
    uint32_t buttons;
    uint32_t ticks;
} Segment;

typedef struct Options {
    bool tiny;
    const char *script;
    bool has_yaw;
    float yaw;
    bool has_teleport;
    float tele_x, tele_z;
    const char *ppm_path;
    uint32_t scale;
    const char *json_path;
    bool has_bench;
    uint64_t bench_frames;
    uint64_t warmup_frames;
} Options;

static void usage(FILE *out)
{
    fputs("usage: pixel_room_capture [--profile tiny|standard] [--script SPEC] [--yaw RAD]\n"
          "                          [--teleport X,Z] [--ppm FILE] [--scale N]\n"
          "                          [--json FILE|-] [--bench FRAMES] [--warmup FRAMES]\n"
          "  SPEC: NAME[+NAME...]:TICKS,... with NAME in LEFT RIGHT UP DOWN A START NONE\n"
          "  The JSON report goes to stdout unless --json FILE is given or only --ppm is used\n"
          "  (--bench always reports).\n",
          out);
}

/* ------------------------------------------------------------------------ */
/* Argument parsing                                                          */
/* ------------------------------------------------------------------------ */

static bool parse_u64(const char *text, uint64_t max, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > max) {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_float(const char *text, float *out)
{
    char *end = NULL;
    double value;
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) || fabs(value) > 1.0e6) {
        return false;
    }
    *out = (float)value;
    return true;
}

static bool name_equals(const char *s, size_t len, const char *name)
{
    size_t i;
    if (strlen(name) != len) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        if (toupper((unsigned char)s[i]) != name[i]) {
            return false;
        }
    }
    return true;
}

static bool parse_buttons(const char *s, size_t len, uint32_t *out)
{
    static const struct {
        const char *name;
        uint32_t bit;
    } names[] = {{"LEFT", PIXELICA_BTN_LEFT}, {"RIGHT", PIXELICA_BTN_RIGHT}, {"UP", PIXELICA_BTN_UP},
                 {"DOWN", PIXELICA_BTN_DOWN}, {"A", PIXELICA_BTN_A},         {"START", PIXELICA_BTN_START},
                 {"NONE", 0u},           {"IDLE", 0u}};
    uint32_t mask = 0u;
    size_t i = 0;

    while (i <= len) {
        size_t j = i, k;
        bool found = false;
        while (j < len && s[j] != '+') {
            ++j;
        }
        for (k = 0; k < sizeof(names) / sizeof(names[0]); ++k) {
            if (name_equals(s + i, j - i, names[k].name)) {
                mask |= names[k].bit;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        i = j + 1u;
    }
    *out = mask;
    return true;
}

/* Fills a malloc'd segment array; the caller frees it. */
static bool parse_script(const char *text, Segment **out_segments, size_t *out_count,
                         uint64_t *out_total)
{
    Segment *segments = (Segment *)malloc(MAX_SEGMENTS * sizeof(Segment));
    size_t count = 0;
    uint64_t total = 0;
    const char *p = text;

    if (segments == NULL) {
        return false;
    }
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        size_t token_len = end != NULL ? (size_t)(end - p) : strlen(p);
        const char *colon = (const char *)memchr(p, ':', token_len);
        uint64_t ticks;
        char number[16];
        size_t number_len;
        uint32_t buttons;

        if (colon == NULL || count >= MAX_SEGMENTS) {
            free(segments);
            return false;
        }
        number_len = token_len - (size_t)(colon - p) - 1u;
        if (number_len == 0u || number_len >= sizeof(number)) {
            free(segments);
            return false;
        }
        memcpy(number, colon + 1, number_len);
        number[number_len] = '\0';
        if (!parse_buttons(p, (size_t)(colon - p), &buttons) ||
            !parse_u64(number, 1000000ull, &ticks) || ticks == 0u) {
            free(segments);
            return false;
        }
        segments[count].buttons = buttons;
        segments[count].ticks = (uint32_t)ticks;
        ++count;
        total += ticks;
        if (total > MAX_SCRIPT_TICKS) {
            free(segments);
            return false;
        }
        p += token_len;
        if (*p == ',') {
            ++p;
            if (*p == '\0') {
                free(segments);
                return false; /* trailing comma */
            }
        }
    }
    if (count == 0u) {
        free(segments);
        return false;
    }
    *out_segments = segments;
    *out_count = count;
    *out_total = total;
    return true;
}

static int parse_options(int argc, char **argv, Options *opt)
{
    int i;
    memset(opt, 0, sizeof(*opt));
    opt->scale = 1u;
    opt->warmup_frames = 10u;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        uint64_t n;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 1;
        }
        if (value == NULL) {
            fprintf(stderr, "option '%s' needs a value (try --help)\n", arg);
            return -1;
        }
        ++i;
        if (strcmp(arg, "--profile") == 0) {
            if (strcmp(value, "tiny") == 0) {
                opt->tiny = true;
            } else if (strcmp(value, "standard") == 0) {
                opt->tiny = false;
            } else {
                fprintf(stderr, "--profile must be tiny or standard\n");
                return -1;
            }
        } else if (strcmp(arg, "--script") == 0) {
            opt->script = value;
        } else if (strcmp(arg, "--yaw") == 0) {
            if (!parse_float(value, &opt->yaw)) {
                fprintf(stderr, "--yaw needs a finite number of radians\n");
                return -1;
            }
            opt->has_yaw = true;
        } else if (strcmp(arg, "--teleport") == 0) {
            const char *comma = strchr(value, ',');
            char first[48];
            size_t first_len = comma != NULL ? (size_t)(comma - value) : 0u;
            if (comma == NULL || first_len == 0u || first_len >= sizeof(first)) {
                fprintf(stderr, "--teleport needs X,Z\n");
                return -1;
            }
            memcpy(first, value, first_len);
            first[first_len] = '\0';
            if (!parse_float(first, &opt->tele_x) || !parse_float(comma + 1, &opt->tele_z)) {
                fprintf(stderr, "--teleport needs two finite numbers: X,Z\n");
                return -1;
            }
            opt->has_teleport = true;
        } else if (strcmp(arg, "--ppm") == 0) {
            opt->ppm_path = value;
        } else if (strcmp(arg, "--scale") == 0) {
            if (!parse_u64(value, MAX_SCALE, &n) || n == 0u) {
                fprintf(stderr, "--scale must be 1..%u\n", MAX_SCALE);
                return -1;
            }
            opt->scale = (uint32_t)n;
        } else if (strcmp(arg, "--json") == 0) {
            opt->json_path = value;
        } else if (strcmp(arg, "--bench") == 0) {
            if (!parse_u64(value, MAX_BENCH_FRAMES, &n) || n == 0u) {
                fprintf(stderr, "--bench must be 1..%llu\n", (unsigned long long)MAX_BENCH_FRAMES);
                return -1;
            }
            opt->has_bench = true;
            opt->bench_frames = n;
        } else if (strcmp(arg, "--warmup") == 0) {
            if (!parse_u64(value, MAX_WARMUP_FRAMES, &opt->warmup_frames)) {
                fprintf(stderr, "--warmup must be 0..%llu\n", (unsigned long long)MAX_WARMUP_FRAMES);
                return -1;
            }
        } else {
            fprintf(stderr, "unknown option '%s' (try --help)\n", arg);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

static uint64_t hash_pixels(const uint16_t *pixels, size_t count)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < count; ++i) {
        h ^= (uint64_t)(pixels[i] & 0xFFu);
        h *= 1099511628211ull;
        h ^= (uint64_t)(pixels[i] >> 8);
        h *= 1099511628211ull;
    }
    return h;
}

static bool write_ppm(const char *path, const uint16_t *pixels, uint32_t width, uint32_t height,
                      uint32_t scale)
{
    FILE *f = fopen(path, "wb");
    uint8_t *row;
    uint32_t x, y, s;
    bool ok = true;

    if (f == NULL) {
        fprintf(stderr, "cannot open '%s' for writing\n", path);
        return false;
    }
    row = (uint8_t *)malloc((size_t)width * scale * 3u);
    if (row == NULL) {
        fclose(f);
        return false;
    }
    fprintf(f, "P6\n%" PRIu32 " %" PRIu32 "\n255\n", width * scale, height * scale);
    for (y = 0; y < height && ok; ++y) {
        uint8_t *out = row;
        for (x = 0; x < width; ++x) {
            uint32_t c = pixels[(size_t)y * width + x];
            uint32_t r5 = (c >> 11) & 0x1Fu, g6 = (c >> 5) & 0x3Fu, b5 = c & 0x1Fu;
            uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
            uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
            uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));
            for (s = 0; s < scale; ++s) {
                *out++ = r;
                *out++ = g;
                *out++ = b;
            }
        }
        for (s = 0; s < scale && ok; ++s) {
            ok = fwrite(row, (size_t)width * scale * 3u, 1, f) == 1u;
        }
    }
    free(row);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        fprintf(stderr, "failed writing '%s'\n", path);
    }
    return ok;
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c < 0x20u) {
            fprintf(f, "\\u%04x", (unsigned)c);
        } else {
            fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static void compiler_string(char *buf, size_t size)
{
#if defined(__clang__)
    snprintf(buf, size, "clang %s", __clang_version__);
#elif defined(__GNUC__)
    snprintf(buf, size, "gcc %s", __VERSION__);
#elif defined(_MSC_VER)
    snprintf(buf, size, "msvc %d", (int)_MSC_VER);
#else
    snprintf(buf, size, "unknown");
#endif
}

typedef struct TimeSummary {
    double min, mean, p50, p95, p99, max;
} TimeSummary;

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static double percentile(const float *sorted, size_t n, double p)
{
    size_t rank = (size_t)ceil(p * (double)n);
    if (rank < 1u) {
        rank = 1u;
    }
    if (rank > n) {
        rank = n;
    }
    return (double)sorted[rank - 1u];
}

/* Sorts `values` in place. */
static TimeSummary summarize(float *values, size_t n)
{
    TimeSummary s;
    double sum = 0.0;
    size_t i;
    memset(&s, 0, sizeof(s));
    if (n == 0u) {
        return s;
    }
    for (i = 0; i < n; ++i) {
        sum += (double)values[i];
    }
    qsort(values, n, sizeof(float), compare_float);
    s.min = (double)values[0];
    s.max = (double)values[n - 1u];
    s.mean = sum / (double)n;
    s.p50 = percentile(values, n, 0.50);
    s.p95 = percentile(values, n, 0.95);
    s.p99 = percentile(values, n, 0.99);
    return s;
}

static void json_summary(FILE *f, const char *name, const TimeSummary *s, bool last)
{
    fprintf(f,
            "    \"%s\": {\"min\": %.6f, \"mean\": %.6f, \"p50\": %.6f, \"p95\": %.6f, "
            "\"p99\": %.6f, \"max\": %.6f}%s\n",
            name, s->min, s->mean, s->p50, s->p95, s->p99, s->max, last ? "" : ",");
}

/*
 * Monotonic clock. Only this tool (never the portable game logic) touches a
 * platform clock. The wall clock (timespec_get/TIME_UTC) is deliberately not
 * used: it can be stepped backwards, which would corrupt unsigned elapsed
 * times. Where no monotonic clock is known the benchmark refuses to run rather
 * than silently measuring something else.
 */
#if defined(_WIN32)
static uint64_t g_qpc_frequency;

static bool clock_init(void)
{
    LARGE_INTEGER f, probe;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0 || !QueryPerformanceCounter(&probe)) {
        return false;
    }
    g_qpc_frequency = (uint64_t)f.QuadPart;
    return true;
}

static bool now_ns(uint64_t *out)
{
    LARGE_INTEGER c;
    uint64_t ticks, seconds, rest;
    if (g_qpc_frequency == 0u || !QueryPerformanceCounter(&c) || c.QuadPart < 0) {
        return false;
    }
    ticks = (uint64_t)c.QuadPart;
    seconds = ticks / g_qpc_frequency;
    rest = ticks % g_qpc_frequency;
    /* rest < frequency, so rest * 1e9 cannot overflow for any realistic frequency (< 1.8e10 Hz). */
    *out = seconds * 1000000000ull + rest * 1000000000ull / g_qpc_frequency;
    return true;
}

static void clock_describe(char *buf, size_t size)
{
    snprintf(buf, size, "QueryPerformanceCounter (monotonic), %" PRIu64 " Hz; host machine only",
             g_qpc_frequency);
}
#elif defined(CLOCK_MONOTONIC)
static bool clock_init(void)
{
    struct timespec ts;
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0;
}

static bool now_ns(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L) {
        return false;
    }
    *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return true;
}

static void clock_describe(char *buf, size_t size)
{
    struct timespec res;
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0) {
        snprintf(buf, size,
                 "clock_gettime(CLOCK_MONOTONIC), reported resolution %lld ns; host machine only",
                 (long long)res.tv_sec * 1000000000ll + (long long)res.tv_nsec);
    } else {
        snprintf(buf, size, "clock_gettime(CLOCK_MONOTONIC); host machine only");
    }
}
#else
static bool clock_init(void)
{
    return false;
}

static bool now_ns(uint64_t *out)
{
    (void)out;
    return false;
}

static void clock_describe(char *buf, size_t size)
{
    snprintf(buf, size, "none (no monotonic clock known for this platform)");
}
#endif

/* end - start, rejecting a clock that went backwards instead of wrapping. */
static bool elapsed_ns(uint64_t start, uint64_t end, uint64_t *out)
{
    if (end < start) {
        return false;
    }
    *out = end - start;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Script playback                                                           */
/* ------------------------------------------------------------------------ */

typedef struct Player {
    const Segment *segments;
    size_t count;
    size_t index;
    uint32_t remaining;
} Player;

static void player_init(Player *p, const Segment *segments, size_t count)
{
    p->segments = segments;
    p->count = count;
    p->index = 0u;
    p->remaining = count > 0u ? segments[0].ticks : 0u;
}

/* Buttons for the next tick; `loop` restarts the script at the end, else returns 0. */
static uint32_t player_next(Player *p, bool loop)
{
    uint32_t buttons;
    if (p->count == 0u) {
        return 0u;
    }
    if (p->remaining == 0u) {
        if (p->index + 1u >= p->count) {
            if (!loop) {
                return 0u;
            }
            p->index = 0u;
        } else {
            ++p->index;
        }
        p->remaining = p->segments[p->index].ticks;
    }
    buttons = p->segments[p->index].buttons;
    --p->remaining;
    return buttons;
}

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    Options opt;
    int parsed = parse_options(argc, argv, &opt);
    PixelRoomParams params;
    PixelRoom *room = NULL;
    PIXELICA_Context *ctx = NULL;
    PIXELICA_Config cfg;
    PIXELICA_Status status;
    Segment *segments = NULL;
    size_t segment_count = 0;
    uint64_t script_ticks = 0;
    const char *script_text;
    Player player;
    uint64_t t;
    int exit_code = 1;
    float *render_ms = NULL, *update_ms = NULL, *total_ms = NULL;
    TimeSummary render_sum, update_sum, total_sum;
    uint64_t bench_total_ns = 0;
    PixelRoomSnapshot snapshot;
    PIXELICA_FrameStats stats;
    uint32_t width, height;
    const uint16_t *pixels;
    uint64_t frame_hash;
    FILE *json = NULL;
    bool json_is_stdout = false;
    char compiler[256];
    char clock_text[160] = "";
    uint64_t i;

    if (parsed != 0) {
        return parsed > 0 ? 0 : 2;
    }
    width = opt.tiny ? PIXEL_ROOM_TINY_WIDTH : PIXEL_ROOM_STANDARD_WIDTH;
    height = opt.tiny ? PIXEL_ROOM_TINY_HEIGHT : PIXEL_ROOM_STANDARD_HEIGHT;
    script_text = opt.script != NULL ? opt.script : (opt.has_bench ? DEFAULT_TOUR : NULL);
    memset(&render_sum, 0, sizeof(render_sum));
    update_sum = render_sum;
    total_sum = render_sum;

    if (script_text != NULL && !parse_script(script_text, &segments, &segment_count, &script_ticks)) {
        fprintf(stderr, "invalid --script (try --help)\n");
        return 2;
    }
    if (opt.json_path == NULL && (opt.ppm_path == NULL || opt.has_bench)) {
        json = stdout;
        json_is_stdout = true;
    } else if (opt.json_path != NULL) {
        if (strcmp(opt.json_path, "-") == 0) {
            json = stdout;
            json_is_stdout = true;
        } else {
            json = fopen(opt.json_path, "wb");
            if (json == NULL) {
                fprintf(stderr, "cannot open '%s' for writing\n", opt.json_path);
                free(segments);
                return 1;
            }
        }
    }

    memset(&params, 0, sizeof(params));
    params.width = (uint16_t)width;
    params.height = (uint16_t)height;
    if (!pixel_room_embedded_assets(&params.assets)) {
        fprintf(stderr, "embedded assets are unavailable\n");
        goto done;
    }
    status = pixel_room_create(&room, &params);
    if (status != PIXELICA_OK) {
        fprintf(stderr, "pixel_room_create failed (PIXELICA_Status %d)\n", (int)status);
        goto done;
    }
    cfg = pixel_room_context_config(params.width, params.height);
    status = pixelica_create(&cfg, NULL, &ctx);
    if (status != PIXELICA_OK) {
        fprintf(stderr, "pixelica_create failed (PIXELICA_Status %d)\n", (int)status);
        goto done;
    }

    if (opt.has_yaw && pixel_room_set_camera_yaw(room, opt.yaw) != PIXELICA_OK) {
        fprintf(stderr, "camera yaw rejected\n");
        goto done;
    }
    if (opt.has_teleport && pixel_room_teleport_player(room, opt.tele_x, opt.tele_z) != PIXELICA_OK) {
        fprintf(stderr, "--teleport position is blocked or outside the room\n");
        goto done;
    }

    /* Play the script once (no loop) at the fixed step, then render the result. */
    player_init(&player, segments, segment_count);
    for (t = 0; t < script_ticks; ++t) {
        pixel_room_update(room, 1.0f / (float)PIXELICA_UPDATE_HZ, player_next(&player, false));
    }
    status = pixel_room_render(room, ctx);
    if (status != PIXELICA_OK) {
        fprintf(stderr, "render failed (PIXELICA_Status %d)\n", (int)status);
        goto done;
    }

    if (opt.has_bench) {
        size_t n = (size_t)opt.bench_frames;
        uint64_t bench_start = 0u, bench_end = 0u;
        render_ms = (float *)malloc(n * sizeof(float));
        update_ms = (float *)malloc(n * sizeof(float));
        total_ms = (float *)malloc(n * sizeof(float));
        if (render_ms == NULL || update_ms == NULL || total_ms == NULL) {
            fprintf(stderr, "cannot allocate %zu benchmark samples\n", n);
            goto done;
        }
        player_init(&player, segments, segment_count);
        for (i = 0; i < opt.warmup_frames; ++i) {
            pixel_room_update(room, 1.0f / (float)PIXELICA_UPDATE_HZ, player_next(&player, true));
            if (pixel_room_render(room, ctx) != PIXELICA_OK) {
                fprintf(stderr, "render failed during warmup\n");
                goto done;
            }
        }
        if (!clock_init() || !now_ns(&bench_start)) {
            fprintf(stderr, "no usable monotonic clock on this platform; benchmark not run\n");
            goto done;
        }
        clock_describe(clock_text, sizeof(clock_text));
        /* Nothing below allocates: samples were allocated before the loop. */
        for (i = 0; i < opt.bench_frames; ++i) {
            uint64_t a = 0u, b = 0u, c = 0u, d_update = 0u, d_render = 0u, d_total = 0u;
            bool ok = now_ns(&a);
            if (ok) {
                pixel_room_update(room, 1.0f / (float)PIXELICA_UPDATE_HZ, player_next(&player, true));
                ok = now_ns(&b);
            }
            if (ok && pixel_room_render(room, ctx) != PIXELICA_OK) {
                fprintf(stderr, "render failed on benchmark frame %" PRIu64 "\n", i);
                goto done;
            }
            ok = ok && now_ns(&c) && elapsed_ns(a, b, &d_update) && elapsed_ns(b, c, &d_render) &&
                 elapsed_ns(a, c, &d_total);
            if (!ok) {
                fprintf(stderr, "monotonic clock failed or went backwards on frame %" PRIu64 "\n", i);
                goto done;
            }
            update_ms[i] = (float)((double)d_update / 1.0e6);
            render_ms[i] = (float)((double)d_render / 1.0e6);
            total_ms[i] = (float)((double)d_total / 1.0e6);
        }
        if (!now_ns(&bench_end) || !elapsed_ns(bench_start, bench_end, &bench_total_ns)) {
            fprintf(stderr, "monotonic clock failed or went backwards after the benchmark\n");
            goto done;
        }
        render_sum = summarize(render_ms, (size_t)opt.bench_frames);
        update_sum = summarize(update_ms, (size_t)opt.bench_frames);
        total_sum = summarize(total_ms, (size_t)opt.bench_frames);
    }

    pixel_room_snapshot(room, &snapshot);
    stats = pixelica_frame_stats(ctx);
    pixels = pixelica_color_buffer(ctx);
    frame_hash = hash_pixels(pixels, (size_t)width * height);

    if (opt.ppm_path != NULL && !write_ppm(opt.ppm_path, pixels, width, height, opt.scale)) {
        goto done;
    }

    if (json != NULL) {
        double total_px = (double)width * (double)height;
        double coverage = total_px > 0.0 ? (double)stats.pixels_covered / total_px : 0.0;
        double overdraw = stats.pixels_covered > 0u
                              ? (double)stats.fragments_covered / (double)stats.pixels_covered
                              : 0.0;
        uint32_t k;

        compiler_string(compiler, sizeof(compiler));
        fprintf(json, "{\n  \"tool\": \"pixel_room_capture\",\n");
        fprintf(json, "  \"profile\": \"%s\",\n  \"width\": %" PRIu32 ",\n  \"height\": %" PRIu32 ",\n",
                opt.tiny ? "TINY" : "STANDARD", width, height);
        fprintf(json, "  \"build\": {\"compiler\": ");
        json_string(json, compiler);
#ifdef NDEBUG
        fprintf(json, ", \"ndebug\": true");
#else
        fprintf(json, ", \"ndebug\": false");
#endif
        fprintf(json, ", \"sizeof_pointer\": %zu},\n", sizeof(void *));
        fprintf(json, "  \"script\": ");
        if (script_text != NULL) {
            json_string(json, script_text);
        } else {
            fputs("null", json);
        }
        fprintf(json, ",\n  \"script_ticks\": %" PRIu64 ",\n", script_ticks);
        fprintf(json, "  \"room_allocation_bytes\": %zu,\n", pixel_room_allocation_bytes(room));
        fprintf(json, "  \"state\": {\n");
        fprintf(json, "    \"tick\": %" PRIu32 ", \"hash\": \"0x%016" PRIx64 "\",\n", snapshot.tick,
                pixel_room_snapshot_hash(&snapshot));
        fprintf(json, "    \"player\": [%.6f, %.6f], \"facing\": [%.6f, %.6f], \"camera_yaw\": %.6f,\n",
                (double)snapshot.player_x, (double)snapshot.player_z, (double)snapshot.facing_x,
                (double)snapshot.facing_z, (double)snapshot.camera_yaw);
        fprintf(json, "    \"focus\": %" PRId32 ", \"investigated_mask\": %" PRIu32
                      ", \"investigate_count\": %" PRIu32 ", \"miss_count\": %" PRIu32 ",\n",
                snapshot.focus, snapshot.investigated_mask, snapshot.investigate_count,
                snapshot.miss_count);
        fprintf(json, "    \"visits\": {");
        for (k = 0; k < PIXEL_ROOM_PROP_COUNT; ++k) {
            fprintf(json, "%s\"%s\": %" PRIu32, k == 0u ? "" : ", ", pixel_room_prop_name(k),
                    snapshot.visits[k]);
        }
        fprintf(json, "}\n  },\n");
        fprintf(json, "  \"frame\": {\n");
        fprintf(json, "    \"hash_fnv1a64\": \"0x%016" PRIx64 "\",\n", frame_hash);
        fprintf(json, "    \"draw_calls\": %" PRIu32 ", \"triangles_submitted\": %" PRIu32
                      ", \"triangles_culled\": %" PRIu32 ", \"triangles_rejected\": %" PRIu32 ",\n",
                stats.draw_calls, stats.triangles_submitted, stats.triangles_culled,
                stats.triangles_rejected);
        fprintf(json, "    \"triangles_clipped\": %" PRIu32 ", \"triangles_rasterized\": %" PRIu32
                      ", \"arena_high_water\": %" PRIu32 ",\n",
                stats.triangles_clipped, stats.triangles_rasterized, stats.arena_high_water);
        fprintf(json, "    \"pixels_written\": %" PRIu32 ", \"fragments_covered\": %" PRIu32
                      ", \"pixels_covered\": %" PRIu32 ",\n",
                stats.pixels_written, stats.fragments_covered, stats.pixels_covered);
        fprintf(json, "    \"coverage\": %.6f, \"geometric_overdraw\": %.6f, \"last_error\": %d\n",
                coverage, overdraw, (int)stats.last_error);
        fprintf(json, "  },\n");
        if (opt.has_bench) {
            fprintf(json, "  \"bench\": {\n");
            fprintf(json, "    \"measured\": true,\n");
            fputs("    \"clock\": ", json);
            json_string(json, clock_text);
            fputs(",\n", json);
            fprintf(json, "    \"frames\": %" PRIu64 ", \"warmup_frames\": %" PRIu64 ",\n",
                    opt.bench_frames, opt.warmup_frames);
            fprintf(json, "    \"wall_seconds\": %.6f,\n", (double)bench_total_ns / 1.0e9);
            fprintf(json, "    \"mean_frames_per_second\": %.3f,\n",
                    bench_total_ns > 0u ? (double)opt.bench_frames / ((double)bench_total_ns / 1.0e9)
                                        : 0.0);
            fprintf(json, "    \"unit\": \"milliseconds per frame\",\n");
            json_summary(json, "update_ms", &update_sum, false);
            json_summary(json, "render_ms", &render_sum, false);
            json_summary(json, "update_plus_render_ms", &total_sum, true);
            fprintf(json, "  }\n");
        } else {
            fprintf(json, "  \"bench\": null\n");
        }
        fprintf(json, "}\n");
        if (fflush(json) != 0) {
            fprintf(stderr, "failed writing the JSON report\n");
            goto done;
        }
    }
    exit_code = 0;

done:
    if (json != NULL && !json_is_stdout) {
        if (fclose(json) != 0) {
            exit_code = 1;
        }
    }
    free(render_ms);
    free(update_ms);
    free(total_ms);
    free(segments);
    if (ctx != NULL) {
        pixelica_destroy(ctx);
    }
    pixel_room_destroy(room);
    return exit_code;
}
