/* Optional online client (see pixelica/online.h). Portable part: argument and URL
 * validation, the frozen P3ON v1 binary codec, the public API and the worker
 * loop. Locks, threads, clocks and HTTP live in online_win32.c (WinHTTP) and
 * online_posix.c (pthreads + libcurl).
 *
 * Every reply is validated completely into worker-private scratch storage
 * before it replaces the shared snapshot or board under the lock, so a
 * malformed reply never changes what the game sees. */
#include "online_internal.h"

#include <stdlib.h>
#include <string.h>

#define OL_TEXT_MAX PIXELICA_ONLINE_MAX_NAME
#define OL_SECRET 32u
#define OL_CODE_LEN PIXELICA_ONLINE_ROOM_CODE_LENGTH
#define OL_HEADER 8u
#define OL_BACKOFF_MAX_MS 2000u
#define OL_IDLE_WAIT_MS 1000u

static const char k_code_alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static const uint8_t k_empty[1] = { 0 };

struct PIXELICA_Online {
    /* Set by host/join, then immutable. */
    const PIXELICA_OnlineTransportOps *ops;
    void *transport;
    PIXELICA_OnlineSync *sync;
    PIXELICA_OnlineThread *thread;
    PIXELICA_OnlineEndpoint endpoint;
    uint32_t timeout_ms;
    uint32_t period_ms;
    bool is_host;
    uint8_t self_id;
    uint8_t host_id;
    uint8_t max_players;
    PIXELICA_OnlineMode mode;
    uint8_t token[OL_SECRET];
    char key_hex[2u * OL_SECRET + 1u];
    char code[OL_CODE_LEN + 1u];

    /* Guarded by sync. */
    bool stop;
    bool fatal;
    PIXELICA_OnlineStatus link_status;
    PIXELICA_OnlineSnapshot snapshot;
    uint32_t input_sequence;
    size_t input_size;
    uint8_t input[PIXELICA_ONLINE_MAX_INPUT];
    uint32_t state_sequence;
    size_t state_size;
    uint8_t state[PIXELICA_ONLINE_MAX_STATE];
    bool score_pending;
    char score_board[OL_TEXT_MAX + 1u];
    int32_t score_value;
    size_t score_record_size;
    uint8_t score_record[PIXELICA_ONLINE_MAX_RECORD];
    bool fetch_pending;
    char fetch_board[OL_TEXT_MAX + 1u];
    PIXELICA_OnlineLeaderboard board;

    /* Owned by the single thread doing I/O: the creator until the worker
     * starts and after it is joined, the worker in between. */
    uint8_t request[PIXELICA_ONLINE_PACKET_MAX];
    uint8_t response[PIXELICA_ONLINE_PACKET_MAX];
    PIXELICA_OnlineSnapshot scratch_snapshot;
    PIXELICA_OnlineLeaderboard scratch_board;
};

static const PIXELICA_OnlineTransportOps *g_transport_override;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void secure_zero(void *data, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size--) *p++ = 0;
}

static bool is_digit(char ch) { return ch >= '0' && ch <= '9'; }

static bool is_alnum(char ch)
{
    return is_digit(ch) || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool is_hex(char ch)
{
    return is_digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static char to_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int hex_value(char ch)
{
    if (is_digit(ch)) return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static size_t bounded_strlen(const char *s, size_t limit)
{
    size_t n = 0;
    while (n < limit && s[n] != '\0') ++n;
    return n;
}

static bool is_zero(const uint8_t *data, size_t size)
{
    uint8_t acc = 0;
    size_t i;
    for (i = 0; i < size; ++i) acc = (uint8_t)(acc | data[i]);
    return acc == 0;
}

/* Game ids and board names: [A-Za-z0-9_.-]{1,32}. */
static bool valid_ident(const char *s, size_t *length)
{
    size_t n = 0;
    if (s == NULL) return false;
    for (; s[n] != '\0'; ++n) {
        if (n >= OL_TEXT_MAX) return false;
        if (!is_alnum(s[n]) && s[n] != '_' && s[n] != '.' && s[n] != '-') return false;
    }
    if (n == 0) return false;
    *length = n;
    return true;
}

/* Player names: 1..32 bytes of strict UTF-8 without C0/DEL/C1 controls. */
static bool valid_name_bytes(const uint8_t *s, size_t n)
{
    size_t i = 0;
    if (n == 0 || n > OL_TEXT_MAX) return false;
    while (i < n) {
        uint32_t cp = s[i];
        uint32_t min;
        size_t extra, k;
        if (cp < 0x80u) {
            if (cp < 0x20u || cp == 0x7Fu) return false;
            ++i;
            continue;
        }
        if ((cp & 0xE0u) == 0xC0u) { extra = 1; min = 0x80u; cp &= 0x1Fu; }
        else if ((cp & 0xF0u) == 0xE0u) { extra = 2; min = 0x800u; cp &= 0x0Fu; }
        else if ((cp & 0xF8u) == 0xF0u) { extra = 3; min = 0x10000u; cp &= 0x07u; }
        else return false;
        if (n - i - 1u < extra) return false;
        for (k = 1; k <= extra; ++k) {
            uint32_t b = s[i + k];
            if ((b & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) ||
            (cp >= 0x80u && cp <= 0x9Fu))
            return false;
        i += extra + 1u;
    }
    return true;
}

static bool valid_code_bytes(const uint8_t *code)
{
    size_t i;
    for (i = 0; i < OL_CODE_LEN; ++i)
        if (memchr(k_code_alphabet, code[i], sizeof k_code_alphabet - 1u) == NULL) return false;
    return true;
}

static bool normalize_code(const char *in, char out[OL_CODE_LEN + 1u])
{
    size_t i;
    if (in == NULL) return false;
    for (i = 0; i < OL_CODE_LEN; ++i) {
        int ch = (unsigned char)in[i];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if (memchr(k_code_alphabet, ch, sizeof k_code_alphabet - 1u) == NULL) return false;
        out[i] = (char)ch;
    }
    if (in[OL_CODE_LEN] != '\0') return false;
    out[OL_CODE_LEN] = '\0';
    return true;
}

static bool parse_key(const char *hex, uint8_t key[OL_SECRET])
{
    size_t i;
    for (i = 0; i < OL_SECRET; ++i) {
        int hi = hex_value(hex[2u * i]);
        int lo;
        if (hi < 0) return false;
        lo = hex_value(hex[2u * i + 1u]);
        if (lo < 0) return false;
        key[i] = (uint8_t)(hi * 16 + lo);
    }
    return hex[2u * OL_SECRET] == '\0';
}

static void format_key(const uint8_t key[OL_SECRET], char out[2u * OL_SECRET + 1u])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < OL_SECRET; ++i) {
        out[2u * i] = digits[key[i] >> 4];
        out[2u * i + 1u] = digits[key[i] & 15u];
    }
    out[2u * OL_SECRET] = '\0';
}

static int32_t i32_from_u32(uint32_t v)
{
    if (v <= 0x7FFFFFFFu) return (int32_t)v;
    return (int32_t)(v - 0x80000000u) - INT32_MAX - 1;
}

/* ------------------------------------------------------------------------- */
/* Codec                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct Writer {
    uint8_t *data;
    size_t capacity;
    size_t size;
    bool ok;
} Writer;

typedef struct Reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    bool ok;
} Reader;

static void writer_init(Writer *w, uint8_t *data, size_t capacity)
{
    w->data = data;
    w->capacity = capacity;
    w->size = 0;
    w->ok = true;
}

static void wr_bytes(Writer *w, const void *src, size_t n)
{
    if (!w->ok || w->capacity - w->size < n) {
        w->ok = false;
        return;
    }
    if (n != 0) memcpy(w->data + w->size, src, n);
    w->size += n;
}

static void wr_u8(Writer *w, uint8_t v) { wr_bytes(w, &v, 1); }

static void wr_u16(Writer *w, uint16_t v)
{
    uint8_t b[2];
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
    wr_bytes(w, b, sizeof b);
}

static void wr_u32(Writer *w, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
    wr_bytes(w, b, sizeof b);
}

static void wr_text(Writer *w, const char *s, size_t n)
{
    wr_u8(w, (uint8_t)n);
    wr_bytes(w, s, n);
}

static void wr_header(Writer *w, uint8_t opcode)
{
    wr_bytes(w, "P3ON", 4);
    wr_u8(w, 1);
    wr_u8(w, opcode);
    wr_u16(w, 0);
}

static void reader_init(Reader *r, const uint8_t *data, size_t size)
{
    r->data = data;
    r->size = size;
    r->pos = 0;
    r->ok = true;
}

/* Returns NULL (and latches !ok) when fewer than n bytes remain. */
static const uint8_t *rd_bytes(Reader *r, size_t n)
{
    const uint8_t *p;
    if (!r->ok || r->size - r->pos < n) {
        r->ok = false;
        return NULL;
    }
    p = r->data + r->pos;
    r->pos += n;
    return p;
}

static uint8_t rd_u8(Reader *r)
{
    const uint8_t *p = rd_bytes(r, 1);
    return p ? p[0] : 0;
}

static uint16_t rd_u16(Reader *r)
{
    const uint8_t *p = rd_bytes(r, 2);
    return p ? (uint16_t)(((unsigned)p[0] << 8) | (unsigned)p[1]) : 0;
}

static uint32_t rd_u32(Reader *r)
{
    const uint8_t *p = rd_bytes(r, 4);
    if (p == NULL) return 0;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/* SNAPSHOT body. Validates everything; *out is only meaningful on true. */
static bool read_snapshot(Reader *r, uint8_t self_id, PIXELICA_OnlineSnapshot *out)
{
    uint32_t revision = rd_u32(r);
    uint8_t host = rd_u8(r);
    uint8_t count = rd_u8(r);
    uint8_t mode = rd_u8(r);
    uint8_t max_players = rd_u8(r);
    uint32_t state_sequence = rd_u32(r);
    uint16_t state_size = rd_u16(r);
    const uint8_t *state;
    unsigned seen = 0;
    uint8_t i;

    if (!r->ok || (unsigned)self_id >= PIXELICA_ONLINE_MAX_PLAYERS) return false;
    if ((unsigned)mode != (unsigned)PIXELICA_ONLINE_COOP && (unsigned)mode != (unsigned)PIXELICA_ONLINE_VERSUS)
        return false;
    if ((unsigned)max_players < 2u || (unsigned)max_players > PIXELICA_ONLINE_MAX_PLAYERS) return false;
    if ((unsigned)count < 1u || count > max_players || (unsigned)host >= PIXELICA_ONLINE_MAX_PLAYERS)
        return false;
    if ((unsigned)state_size > PIXELICA_ONLINE_MAX_STATE || (state_sequence == 0 && state_size != 0))
        return false;
    state = rd_bytes(r, state_size);
    if (state == NULL) return false;

    memset(out, 0, sizeof *out);
    out->self_id = self_id;
    out->host_id = host;
    out->player_count = count;
    out->max_players = max_players;
    out->mode = (PIXELICA_OnlineMode)mode;
    out->revision = revision;
    out->state_sequence = state_sequence;
    out->state_size = state_size;
    if (state_size != 0) memcpy(out->state, state, state_size);

    for (i = 0; i < count; ++i) {
        PIXELICA_OnlinePlayer *player = &out->players[i];
        uint8_t id = rd_u8(r);
        uint8_t name_size = rd_u8(r);
        const uint8_t *name = rd_bytes(r, name_size);
        uint32_t input_sequence = rd_u32(r);
        uint8_t input_size = rd_u8(r);
        const uint8_t *input = rd_bytes(r, input_size);
        if (!r->ok || name == NULL || input == NULL) return false;
        if ((unsigned)id >= PIXELICA_ONLINE_MAX_PLAYERS || (seen & (1u << id)) != 0) return false;
        if (!valid_name_bytes(name, name_size)) return false;
        if ((unsigned)input_size > PIXELICA_ONLINE_MAX_INPUT || (input_sequence == 0 && input_size != 0))
            return false;
        seen |= 1u << id;
        player->id = id;
        memcpy(player->name, name, name_size);
        player->name[name_size] = '\0';
        player->input_sequence = input_sequence;
        player->input_size = input_size;
        if (input_size != 0) memcpy(player->input, input, input_size);
    }
    return (seen & (1u << host)) != 0 && (seen & (1u << self_id)) != 0;
}

/* BOARDRESULT body for the board that was asked for. */
static bool read_board(Reader *r, const char *expected, size_t expected_size,
    PIXELICA_OnlineLeaderboard *out)
{
    uint8_t board_size = rd_u8(r);
    const uint8_t *board = rd_bytes(r, board_size);
    uint8_t count = rd_u8(r);
    uint8_t i;

    if (!r->ok || board == NULL || (size_t)board_size != expected_size ||
        memcmp(board, expected, board_size) != 0 || (unsigned)count > PIXELICA_ONLINE_MAX_SCORES)
        return false;
    memset(out, 0, sizeof *out);
    memcpy(out->board, expected, expected_size);
    out->board[expected_size] = '\0';
    out->count = count;
    for (i = 0; i < count; ++i) {
        PIXELICA_OnlineScore *entry = &out->entries[i];
        uint8_t name_size = rd_u8(r);
        const uint8_t *name = rd_bytes(r, name_size);
        uint32_t score = rd_u32(r);
        uint8_t record_size = rd_u8(r);
        const uint8_t *record = rd_bytes(r, record_size);
        if (!r->ok || name == NULL || record == NULL) return false;
        if (!valid_name_bytes(name, name_size) || (unsigned)record_size > PIXELICA_ONLINE_MAX_RECORD)
            return false;
        memcpy(entry->name, name, name_size);
        entry->name[name_size] = '\0';
        entry->score = i32_from_u32(score);
        entry->record_size = record_size;
        if (record_size != 0) memcpy(entry->record, record, record_size);
    }
    return true;
}

PIXELICA_OnlineStatus pixelica_online_internal_parse_header(const uint8_t *packet, size_t size,
    uint8_t request_opcode, size_t *body_size)
{
    unsigned status;
    if (body_size != NULL) *body_size = 0;
    if (packet == NULL || body_size == NULL) return PIXELICA_ONLINE_INVALID_ARG;
    if (size < OL_HEADER || memcmp(packet, "P3ON", 4) != 0 || (unsigned)packet[4] != 1u ||
        (unsigned)packet[5] != ((unsigned)request_opcode | 0x80u))
        return PIXELICA_ONLINE_PROTOCOL;
    status = ((unsigned)packet[6] << 8) | (unsigned)packet[7];
    if (status == 0) {
        *body_size = size - OL_HEADER;
        return PIXELICA_ONLINE_OK;
    }
    /* Errors carry no body and use the frozen server range 1..9. */
    if (size != OL_HEADER || status > (unsigned)PIXELICA_ONLINE_INTERNAL) return PIXELICA_ONLINE_PROTOCOL;
    return (PIXELICA_OnlineStatus)status;
}

PIXELICA_OnlineStatus pixelica_online_internal_parse_snapshot(const uint8_t *body, size_t size,
    uint8_t self_id, PIXELICA_OnlineSnapshot *out)
{
    PIXELICA_OnlineSnapshot tmp;
    Reader r;
    if (out == NULL || (body == NULL && size != 0)) return PIXELICA_ONLINE_INVALID_ARG;
    reader_init(&r, body ? body : k_empty, size);
    if (!read_snapshot(&r, self_id, &tmp) || r.pos != r.size) return PIXELICA_ONLINE_PROTOCOL;
    *out = tmp;
    return PIXELICA_ONLINE_OK;
}

PIXELICA_OnlineStatus pixelica_online_internal_parse_board(const uint8_t *body, size_t size,
    const char *expected_board, PIXELICA_OnlineLeaderboard *out)
{
    PIXELICA_OnlineLeaderboard tmp;
    size_t expected_size = 0;
    Reader r;
    if (out == NULL || (body == NULL && size != 0) || !valid_ident(expected_board, &expected_size))
        return PIXELICA_ONLINE_INVALID_ARG;
    reader_init(&r, body ? body : k_empty, size);
    if (!read_board(&r, expected_board, expected_size, &tmp) || r.pos != r.size)
        return PIXELICA_ONLINE_PROTOCOL;
    *out = tmp;
    return PIXELICA_ONLINE_OK;
}

PIXELICA_OnlineStatus pixelica_online_internal_http_status(long http_code)
{
    if (http_code == 200) return PIXELICA_ONLINE_OK;
    if (http_code == 408 || http_code == 429 || (http_code >= 500 && http_code <= 599))
        return PIXELICA_ONLINE_TRANSPORT;
    return PIXELICA_ONLINE_PROTOCOL;
}

/* ------------------------------------------------------------------------- */
/* URL                                                                        */
/* ------------------------------------------------------------------------- */

static bool prefix_ci(const char *s, const char *prefix)
{
    size_t i;
    for (i = 0; prefix[i] != '\0'; ++i)
        if (to_lower(s[i]) != prefix[i]) return false;
    return true;
}

static bool valid_hostname(const char *s, size_t n)
{
    size_t i, label = 0;
    if (n == 0 || n > 253u) return false;
    for (i = 0; i <= n; ++i) {
        if (i == n || s[i] == '.') {
            if (label == 0 || label > 63u || s[i - label] == '-' || s[i - 1u] == '-') return false;
            label = 0;
        } else {
            ++label;
        }
    }
    return true;
}

/* Strict dotted quad, no leading zeros (avoids octal/hex resolver forms). */
static bool parse_ipv4(const char *s, uint8_t ip[4])
{
    size_t i = 0;
    int part;
    for (part = 0; part < 4; ++part) {
        unsigned value = 0;
        size_t digits = 0;
        while (is_digit(s[i])) {
            if (digits == 3u || (digits == 1u && value == 0u)) return false;
            value = value * 10u + (unsigned)(s[i] - '0');
            ++digits;
            ++i;
        }
        if (digits == 0 || value > 255u) return false;
        ip[part] = (uint8_t)value;
        if (part < 3) {
            if (s[i] != '.') return false;
            ++i;
        }
    }
    return s[i] == '\0';
}

static bool is_private_lan(const char *host)
{
    uint8_t ip[4];
    size_t n = strlen(host);
    if (parse_ipv4(host, ip))
        return ip[0] == 10u || ip[0] == 127u || (ip[0] == 172u && ip[1] >= 16u && ip[1] <= 31u) ||
               (ip[0] == 192u && ip[1] == 168u) || (ip[0] == 169u && ip[1] == 254u);
    return n > 6u && strcmp(host + n - 6u, ".local") == 0;
}

static bool append(char *dst, size_t capacity, size_t *size, const char *src, size_t n)
{
    if (capacity - *size <= n) return false;
    memcpy(dst + *size, src, n);
    *size += n;
    dst[*size] = '\0';
    return true;
}

PIXELICA_OnlineStatus pixelica_online_internal_parse_url(const char *url, bool allow_insecure_lan,
    PIXELICA_OnlineEndpoint *out)
{
    static const char k_route[] = "/v1/online";
    PIXELICA_OnlineEndpoint ep;
    size_t n, pos = 0, host_begin, host_end, port_begin, path_begin, path_end, i, seg;
    size_t url_size = 0, path_size = 0, host_size;
    unsigned long port = 0;
    bool ipv6 = false, loopback;

    if (url == NULL || out == NULL) return PIXELICA_ONLINE_INVALID_ARG;
    n = bounded_strlen(url, PIXELICA_ONLINE_URL_MAX + 1u);
    if (n > PIXELICA_ONLINE_URL_MAX) return PIXELICA_ONLINE_INVALID_ARG;
    memset(&ep, 0, sizeof ep);
    if (prefix_ci(url, "https://")) {
        ep.secure = true;
        pos = 8;
    } else if (prefix_ci(url, "http://")) {
        pos = 7;
    } else {
        return PIXELICA_ONLINE_INVALID_ARG;
    }

    host_begin = pos;
    if (url[pos] == '[') {
        ipv6 = true;
        ++pos;
        while (pos < n && (is_hex(url[pos]) || url[pos] == ':' || url[pos] == '.')) ++pos;
        if (pos >= n || url[pos] != ']' || pos - host_begin < 3u ||
            memchr(url + host_begin, ':', pos - host_begin) == NULL)
            return PIXELICA_ONLINE_INVALID_ARG;
        ++pos;
    } else {
        while (pos < n && (is_alnum(url[pos]) || url[pos] == '-' || url[pos] == '.')) ++pos;
        if (!valid_hostname(url + host_begin, pos - host_begin)) return PIXELICA_ONLINE_INVALID_ARG;
    }
    host_end = pos;
    host_size = host_end - host_begin;
    if (host_size >= sizeof ep.host) return PIXELICA_ONLINE_INVALID_ARG;

    port_begin = pos;
    if (pos < n && url[pos] == ':') {
        size_t digits = 0;
        ++pos;
        while (pos < n && is_digit(url[pos])) {
            if (++digits > 5u) return PIXELICA_ONLINE_INVALID_ARG;
            port = port * 10u + (unsigned long)(url[pos] - '0');
            ++pos;
        }
        if (digits == 0 || port == 0 || port > 65535u) return PIXELICA_ONLINE_INVALID_ARG;
    } else {
        port = ep.secure ? 443u : 80u;
    }
    ep.port = (uint16_t)port;

    /* Anything else in the authority (userinfo '@', '?', '#', '\\', spaces...)
     * is rejected here; the path allows only [A-Za-z0-9._~-/]. */
    path_begin = pos;
    if (pos < n && url[pos] != '/') return PIXELICA_ONLINE_INVALID_ARG;
    path_end = n;
    while (path_end > path_begin && url[path_end - 1u] == '/') --path_end;
    seg = path_begin;
    for (i = path_begin; i <= path_end; ++i) {
        if (i == path_end || url[i] == '/') {
            size_t len = i - seg;
            if ((len == 1u && url[seg] == '.') || (len == 2u && url[seg] == '.' && url[seg + 1u] == '.'))
                return PIXELICA_ONLINE_INVALID_ARG;
            seg = i + 1u;
        } else if (!is_alnum(url[i]) && url[i] != '.' && url[i] != '_' && url[i] != '~' &&
                   url[i] != '-') {
            return PIXELICA_ONLINE_INVALID_ARG;
        }
    }

    for (i = 0; i < host_size; ++i) ep.host[i] = to_lower(url[host_begin + i]);
    ep.host[host_size] = '\0';

    if (!ep.secure) {
        loopback = ipv6 ? strcmp(ep.host, "[::1]") == 0
                        : (strcmp(ep.host, "localhost") == 0 || strcmp(ep.host, "127.0.0.1") == 0);
        if (!loopback && !(allow_insecure_lan && !ipv6 && is_private_lan(ep.host)))
            return PIXELICA_ONLINE_INVALID_ARG;
    }

    if (!append(ep.url, sizeof ep.url, &url_size, ep.secure ? "https://" : "http://",
                ep.secure ? 8u : 7u) ||
        !append(ep.url, sizeof ep.url, &url_size, ep.host, host_size) ||
        !append(ep.url, sizeof ep.url, &url_size, url + port_begin, path_begin - port_begin) ||
        !append(ep.url, sizeof ep.url, &url_size, url + path_begin, path_end - path_begin) ||
        !append(ep.url, sizeof ep.url, &url_size, k_route, sizeof k_route - 1u) ||
        !append(ep.path, sizeof ep.path, &path_size, url + path_begin, path_end - path_begin) ||
        !append(ep.path, sizeof ep.path, &path_size, k_route, sizeof k_route - 1u))
        return PIXELICA_ONLINE_INVALID_ARG;

    *out = ep;
    return PIXELICA_ONLINE_OK;
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                   */
/* ------------------------------------------------------------------------- */

void pixelica_online_internal_set_transport(const PIXELICA_OnlineTransportOps *ops)
{
    g_transport_override = ops;
}

/* POST c->request[0..request_size) and validate the reply header. */
static PIXELICA_OnlineStatus exchange(PIXELICA_Online *c, size_t request_size, uint8_t opcode,
    size_t *body_size)
{
    size_t received = 0;
    PIXELICA_OnlineStatus st;
    *body_size = 0;
    st = c->ops->post(c->transport, c->request, request_size, c->response,
                      sizeof c->response, &received);
    if (st != PIXELICA_ONLINE_OK) return st;
    if (received > sizeof c->response) return PIXELICA_ONLINE_PROTOCOL;
    return pixelica_online_internal_parse_header(c->response, received, opcode, body_size);
}

static void send_leave(PIXELICA_Online *c)
{
    Writer w;
    size_t body = 0;
    writer_init(&w, c->request, sizeof c->request);
    wr_header(&w, (uint8_t)PIXELICA_ONLINE_OP_LEAVE);
    wr_bytes(&w, c->token, OL_SECRET);
    if (w.ok) (void)exchange(c, w.size, (uint8_t)PIXELICA_ONLINE_OP_LEAVE, &body);
}

/* Lock held. */
static size_t build_sync(PIXELICA_Online *c)
{
    Writer w;
    writer_init(&w, c->request, sizeof c->request);
    wr_header(&w, (uint8_t)PIXELICA_ONLINE_OP_SYNC);
    wr_bytes(&w, c->token, OL_SECRET);
    wr_u32(&w, c->input_sequence);
    wr_u8(&w, (uint8_t)c->input_size);
    wr_bytes(&w, c->input, c->input_size);
    if (c->is_host) {
        wr_u32(&w, c->state_sequence);
        wr_u16(&w, (uint16_t)c->state_size);
        wr_bytes(&w, c->state, c->state_size);
    } else {
        wr_u32(&w, 0);
        wr_u16(&w, 0);
    }
    return w.ok ? w.size : 0;
}

/* Worker-private: validate a SYNC reply into scratch_snapshot. */
static PIXELICA_OnlineStatus accept_sync(PIXELICA_Online *c, size_t body_size)
{
    PIXELICA_OnlineSnapshot *s = &c->scratch_snapshot;
    Reader r;
    reader_init(&r, c->response + OL_HEADER, body_size);
    if (!read_snapshot(&r, c->self_id, s) || r.pos != r.size || s->host_id != c->host_id ||
        s->mode != c->mode || s->max_players != c->max_players)
        return PIXELICA_ONLINE_PROTOCOL;
    memcpy(s->room_code, c->code, sizeof s->room_code);
    s->connected = true;
    return PIXELICA_ONLINE_OK;
}

static bool sync_transient(PIXELICA_OnlineStatus st)
{
    return st == PIXELICA_ONLINE_TRANSPORT || st == PIXELICA_ONLINE_PROTOCOL ||
           st == PIXELICA_ONLINE_RATE_LIMIT || st == PIXELICA_ONLINE_CAPACITY ||
           st == PIXELICA_ONLINE_INTERNAL;
}

static bool session_gone(PIXELICA_OnlineStatus st)
{
    return st == PIXELICA_ONLINE_UNAUTHORIZED || st == PIXELICA_ONLINE_NOT_FOUND ||
           st == PIXELICA_ONLINE_ROOM_CLOSED;
}

/* Lock held. */
static void mark_fatal(PIXELICA_Online *c, PIXELICA_OnlineStatus st)
{
    c->fatal = true;
    c->link_status = st;
    c->snapshot.connected = false;
}

/* Lock held. A failed operation keeps the last good board/entries. */
static void board_complete(PIXELICA_Online *c, PIXELICA_OnlineStatus st)
{
    uint32_t generation = c->board.generation + 1u;
    if (generation == 0) generation = 1;
    if (st == PIXELICA_ONLINE_OK) c->board = c->scratch_board;
    c->board.generation = generation;
    c->board.status = st;
}

static uint64_t backoff_ms(uint32_t period_ms, uint32_t failures)
{
    uint64_t delay = period_ms;
    uint32_t i;
    for (i = 1; i < failures && delay < OL_BACKOFF_MAX_MS; ++i) delay *= 2u;
    return delay < OL_BACKOFF_MAX_MS ? delay : OL_BACKOFF_MAX_MS;
}

/* Lock held on entry and exit; released during I/O. */
static void worker_sync(PIXELICA_Online *c, uint64_t *next_sync, uint32_t *failures)
{
    uint64_t started = pixelica_online_sys_now_ms();
    size_t request_size = build_sync(c);
    size_t body = 0;
    PIXELICA_OnlineStatus st;

    pixelica_online_sys_unlock(c->sync);
    st = exchange(c, request_size, (uint8_t)PIXELICA_ONLINE_OP_SYNC, &body);
    if (st == PIXELICA_ONLINE_OK) st = accept_sync(c, body);
    pixelica_online_sys_lock(c->sync);

    if (st == PIXELICA_ONLINE_OK) {
        c->snapshot = c->scratch_snapshot;
        c->link_status = PIXELICA_ONLINE_OK;
        *failures = 0;
        *next_sync = started + c->period_ms;
    } else if (sync_transient(st)) {
        c->snapshot.connected = false;
        c->link_status = st;
        if (*failures < 32u) ++*failures;
        *next_sync = pixelica_online_sys_now_ms() + backoff_ms(c->period_ms, *failures);
    } else {
        mark_fatal(c, st);
    }
}

/* Lock held on entry and exit; released during I/O. */
static void worker_board(PIXELICA_Online *c)
{
    char board[OL_TEXT_MAX + 1u];
    size_t board_size = 0, body = 0;
    uint8_t opcode;
    PIXELICA_OnlineStatus st;
    Writer w;
    Reader r;

    writer_init(&w, c->request, sizeof c->request);
    if (c->score_pending) {
        opcode = (uint8_t)PIXELICA_ONLINE_OP_SCORE;
        memcpy(board, c->score_board, sizeof board);
        board_size = strlen(board);
        wr_header(&w, opcode);
        wr_bytes(&w, c->token, OL_SECRET);
        wr_text(&w, board, board_size);
        wr_u32(&w, (uint32_t)c->score_value);
        wr_u8(&w, (uint8_t)c->score_record_size);
        wr_bytes(&w, c->score_record, c->score_record_size);
        c->score_pending = false;
    } else {
        opcode = (uint8_t)PIXELICA_ONLINE_OP_BOARD;
        memcpy(board, c->fetch_board, sizeof board);
        board_size = strlen(board);
        wr_header(&w, opcode);
        wr_bytes(&w, c->token, OL_SECRET);
        wr_text(&w, board, board_size);
        c->fetch_pending = false;
    }

    pixelica_online_sys_unlock(c->sync);
    st = w.ok ? exchange(c, w.size, opcode, &body) : PIXELICA_ONLINE_INVALID_ARG;
    if (st == PIXELICA_ONLINE_OK) {
        reader_init(&r, c->response + OL_HEADER, body);
        if (!read_board(&r, board, board_size, &c->scratch_board) || r.pos != r.size)
            st = PIXELICA_ONLINE_PROTOCOL;
    }
    pixelica_online_sys_lock(c->sync);

    board_complete(c, st);
    if (session_gone(st) && !c->fatal) mark_fatal(c, st);
}

static void worker_main(void *arg)
{
    PIXELICA_Online *c = (PIXELICA_Online *)arg;
    uint64_t next_sync = 0;
    uint32_t failures = 0;
    bool last_was_sync = false;

    pixelica_online_sys_lock(c->sync);
    while (!c->stop) {
        bool board_pending = c->score_pending || c->fetch_pending;
        uint64_t now;
        if (c->fatal) {
            /* The session is over: fail queued board work, then idle. */
            if (c->score_pending) { c->score_pending = false; board_complete(c, c->link_status); }
            if (c->fetch_pending) { c->fetch_pending = false; board_complete(c, c->link_status); }
            pixelica_online_sys_wait(c->sync, OL_IDLE_WAIT_MS);
            continue;
        }
        now = pixelica_online_sys_now_ms();
        /* Alternate with syncs so neither board traffic nor a slow relay can
         * starve the other. */
        if (board_pending && (now < next_sync || last_was_sync)) {
            worker_board(c);
            last_was_sync = false;
        } else if (now >= next_sync) {
            worker_sync(c, &next_sync, &failures);
            last_was_sync = true;
        } else {
            uint64_t delay = next_sync - now;
            pixelica_online_sys_wait(c->sync,
                (uint32_t)(delay < OL_IDLE_WAIT_MS ? delay : OL_IDLE_WAIT_MS));
        }
    }
    pixelica_online_sys_unlock(c->sync);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

const char *pixelica_online_status_string(PIXELICA_OnlineStatus status)
{
    switch (status) {
    case PIXELICA_ONLINE_OK: return "ok";
    case PIXELICA_ONLINE_BAD_REQUEST: return "bad request";
    case PIXELICA_ONLINE_NOT_FOUND: return "room or session not found";
    case PIXELICA_ONLINE_FULL: return "room is full";
    case PIXELICA_ONLINE_UNAUTHORIZED: return "session expired or not authorized";
    case PIXELICA_ONLINE_NOT_HOST: return "only the host may do this";
    case PIXELICA_ONLINE_CAPACITY: return "relay at capacity";
    case PIXELICA_ONLINE_RATE_LIMIT: return "rate limited";
    case PIXELICA_ONLINE_ROOM_CLOSED: return "room closed";
    case PIXELICA_ONLINE_INTERNAL: return "relay internal error";
    case PIXELICA_ONLINE_TRANSPORT: return "network or TLS error";
    case PIXELICA_ONLINE_PROTOCOL: return "malformed or unexpected reply";
    case PIXELICA_ONLINE_NO_MEMORY: return "out of memory";
    case PIXELICA_ONLINE_INVALID_ARG: return "invalid argument";
    case PIXELICA_ONLINE_UNSUPPORTED: return "unsupported";
    }
    return "unknown status";
}

PIXELICA_OnlineConfig pixelica_online_config_default(void)
{
    PIXELICA_OnlineConfig config;
    memset(&config, 0, sizeof config);
    config.timeout_ms = 1500u;
    config.sync_hz = 30u;
    config.allow_insecure_lan = false;
    return config;
}

static PIXELICA_OnlineStatus check_config(const PIXELICA_OnlineConfig *config, size_t *game_size,
    size_t *name_size, uint8_t key[OL_SECRET])
{
    memset(key, 0, OL_SECRET);
    if (config == NULL || config->server_url == NULL || config->player_name == NULL ||
        !valid_ident(config->game_id, game_size))
        return PIXELICA_ONLINE_INVALID_ARG;
    *name_size = bounded_strlen(config->player_name, OL_TEXT_MAX + 1u);
    if (!valid_name_bytes((const uint8_t *)config->player_name, *name_size))
        return PIXELICA_ONLINE_INVALID_ARG;
    if (config->timeout_ms < 100u || config->timeout_ms > 10000u || config->sync_hz < 1u ||
        config->sync_hz > 60u)
        return PIXELICA_ONLINE_INVALID_ARG;
    if (config->player_key != NULL && !parse_key(config->player_key, key))
        return PIXELICA_ONLINE_INVALID_ARG;
    return PIXELICA_ONLINE_OK;
}

/* Validate a HOST/JOIN reply and adopt it. join_code is NULL for HOST. */
static PIXELICA_OnlineStatus accept_welcome(PIXELICA_Online *c, size_t body_size,
    const uint8_t sent_key[OL_SECRET], const char *join_code, PIXELICA_OnlineMode mode,
    uint8_t max_players)
{
    PIXELICA_OnlineSnapshot *s = &c->scratch_snapshot;
    const uint8_t *token, *key, *code;
    uint8_t self;
    Reader r;

    reader_init(&r, c->response + OL_HEADER, body_size);
    token = rd_bytes(&r, OL_SECRET);
    key = rd_bytes(&r, OL_SECRET);
    code = rd_bytes(&r, OL_CODE_LEN);
    self = rd_u8(&r);
    if (!r.ok || token == NULL || key == NULL || code == NULL || !read_snapshot(&r, self, s) ||
        r.pos != r.size || !valid_code_bytes(code))
        return PIXELICA_ONLINE_PROTOCOL;
    if (is_zero(sent_key, OL_SECRET) ? is_zero(key, OL_SECRET)
                                     : memcmp(key, sent_key, OL_SECRET) != 0)
        return PIXELICA_ONLINE_PROTOCOL;
    if (join_code == NULL) {
        if (s->host_id != self || s->mode != mode || s->max_players != max_players)
            return PIXELICA_ONLINE_PROTOCOL;
    } else if (memcmp(code, join_code, OL_CODE_LEN) != 0 || s->host_id == self) {
        return PIXELICA_ONLINE_PROTOCOL;
    }

    memcpy(c->token, token, OL_SECRET);
    format_key(key, c->key_hex);
    memcpy(c->code, code, OL_CODE_LEN);
    c->code[OL_CODE_LEN] = '\0';
    c->self_id = self;
    c->host_id = s->host_id;
    c->max_players = s->max_players;
    c->mode = s->mode;
    c->is_host = join_code == NULL;
    memcpy(s->room_code, c->code, sizeof s->room_code);
    s->connected = true;
    c->snapshot = *s;
    c->link_status = PIXELICA_ONLINE_OK;
    return PIXELICA_ONLINE_OK;
}

static void free_client(PIXELICA_Online *c)
{
    if (c == NULL) return;
    if (c->transport != NULL) c->ops->destroy(c->transport);
    if (c->sync != NULL) pixelica_online_sys_sync_destroy(c->sync);
    secure_zero(c, sizeof *c); /* token, key, inputs */
    free(c);
}

static PIXELICA_Online *connect_room(const PIXELICA_OnlineConfig *config, uint8_t opcode,
    PIXELICA_OnlineMode mode, uint8_t max_players, const char *join_code, PIXELICA_OnlineStatus *error)
{
    PIXELICA_Online *c = NULL;
    size_t game_size = 0, name_size = 0, body = 0;
    uint8_t key[OL_SECRET];
    PIXELICA_OnlineStatus st = check_config(config, &game_size, &name_size, key);
    Writer w;

    if (st == PIXELICA_ONLINE_OK) {
        c = (PIXELICA_Online *)calloc(1, sizeof *c);
        if (c == NULL) st = PIXELICA_ONLINE_NO_MEMORY;
    }
    if (st == PIXELICA_ONLINE_OK)
        st = pixelica_online_internal_parse_url(config->server_url, config->allow_insecure_lan,
                                           &c->endpoint);
    if (st == PIXELICA_ONLINE_OK) {
        c->sync = pixelica_online_sys_sync_create();
        if (c->sync == NULL) st = PIXELICA_ONLINE_NO_MEMORY;
    }
    if (st == PIXELICA_ONLINE_OK) {
        c->ops = g_transport_override ? g_transport_override : pixelica_online_sys_transport();
        c->timeout_ms = config->timeout_ms;
        c->period_ms = 1000u / config->sync_hz;
        c->transport = c->ops->create(&c->endpoint, c->timeout_ms, &st);
        if (c->transport == NULL)
            st = st == PIXELICA_ONLINE_OK ? PIXELICA_ONLINE_TRANSPORT : st;
        else
            st = PIXELICA_ONLINE_OK;
    }
    if (st == PIXELICA_ONLINE_OK) {
        writer_init(&w, c->request, sizeof c->request);
        wr_header(&w, opcode);
        wr_text(&w, config->game_id, game_size);
        wr_text(&w, config->player_name, name_size);
        wr_bytes(&w, key, OL_SECRET);
        if (join_code == NULL) {
            wr_u8(&w, (uint8_t)mode);
            wr_u8(&w, max_players);
        } else {
            wr_bytes(&w, join_code, OL_CODE_LEN);
        }
        st = w.ok ? exchange(c, w.size, opcode, &body) : PIXELICA_ONLINE_INVALID_ARG;
    }
    if (st == PIXELICA_ONLINE_OK) st = accept_welcome(c, body, key, join_code, mode, max_players);
    if (st == PIXELICA_ONLINE_OK) {
        c->thread = pixelica_online_sys_thread_start(worker_main, c);
        if (c->thread == NULL) {
            send_leave(c);
            st = PIXELICA_ONLINE_NO_MEMORY;
        }
    }
    secure_zero(key, sizeof key);
    if (st != PIXELICA_ONLINE_OK) {
        free_client(c);
        c = NULL;
    }
    if (error != NULL) *error = st;
    return c;
}

PIXELICA_Online *pixelica_online_host(const PIXELICA_OnlineConfig *config, PIXELICA_OnlineMode mode,
    uint8_t max_players, PIXELICA_OnlineStatus *error)
{
    if ((mode != PIXELICA_ONLINE_COOP && mode != PIXELICA_ONLINE_VERSUS) || (unsigned)max_players < 2u ||
        (unsigned)max_players > PIXELICA_ONLINE_MAX_PLAYERS) {
        if (error != NULL) *error = PIXELICA_ONLINE_INVALID_ARG;
        return NULL;
    }
    return connect_room(config, (uint8_t)PIXELICA_ONLINE_OP_HOST, mode, max_players, NULL, error);
}

PIXELICA_Online *pixelica_online_join(const PIXELICA_OnlineConfig *config, const char *room_code,
    PIXELICA_OnlineStatus *error)
{
    char code[OL_CODE_LEN + 1u];
    if (!normalize_code(room_code, code)) {
        if (error != NULL) *error = PIXELICA_ONLINE_INVALID_ARG;
        return NULL;
    }
    return connect_room(config, (uint8_t)PIXELICA_ONLINE_OP_JOIN, PIXELICA_ONLINE_COOP, 0, code, error);
}

void pixelica_online_destroy(PIXELICA_Online *client)
{
    if (client == NULL) return;
    pixelica_online_sys_lock(client->sync);
    client->stop = true;
    pixelica_online_sys_wake(client->sync);
    pixelica_online_sys_unlock(client->sync);
    pixelica_online_sys_thread_join(client->thread); /* bounded by one in-flight request */
    client->thread = NULL;
    if (!client->fatal) send_leave(client); /* worker joined: request buffers are ours */
    free_client(client);
}

const char *pixelica_online_room_code(const PIXELICA_Online *client)
{
    return client != NULL ? client->code : "";
}

const char *pixelica_online_player_key(const PIXELICA_Online *client)
{
    return client != NULL ? client->key_hex : "";
}

/* Lock held. Same bytes keep the same sequence (an idempotent resend). */
static void publish_latest(uint32_t *sequence, size_t *size, uint8_t *dst, const void *src,
    size_t n)
{
    if (*size == n && (n == 0 || memcmp(dst, src, n) == 0)) return;
    if (n != 0) memcpy(dst, src, n);
    *size = n;
    *sequence += 1u;
    if (*sequence == 0) *sequence = 1u;
}

PIXELICA_OnlineStatus pixelica_online_input(PIXELICA_Online *client, const void *data, size_t size)
{
    PIXELICA_OnlineStatus st = PIXELICA_ONLINE_OK;
    if (client == NULL || size > PIXELICA_ONLINE_MAX_INPUT || (size != 0 && data == NULL))
        return PIXELICA_ONLINE_INVALID_ARG;
    pixelica_online_sys_lock(client->sync);
    if (client->fatal)
        st = client->link_status;
    else
        publish_latest(&client->input_sequence, &client->input_size, client->input, data, size);
    pixelica_online_sys_unlock(client->sync);
    return st;
}

PIXELICA_OnlineStatus pixelica_online_state(PIXELICA_Online *client, const void *data, size_t size)
{
    PIXELICA_OnlineStatus st = PIXELICA_ONLINE_OK;
    if (client == NULL || size > PIXELICA_ONLINE_MAX_STATE || (size != 0 && data == NULL))
        return PIXELICA_ONLINE_INVALID_ARG;
    if (!client->is_host) return PIXELICA_ONLINE_NOT_HOST;
    pixelica_online_sys_lock(client->sync);
    if (client->fatal)
        st = client->link_status;
    else
        publish_latest(&client->state_sequence, &client->state_size, client->state, data, size);
    pixelica_online_sys_unlock(client->sync);
    return st;
}

PIXELICA_OnlineStatus pixelica_online_snapshot(PIXELICA_Online *client, PIXELICA_OnlineSnapshot *out)
{
    PIXELICA_OnlineStatus st;
    if (client == NULL || out == NULL) return PIXELICA_ONLINE_INVALID_ARG;
    pixelica_online_sys_lock(client->sync);
    *out = client->snapshot;
    st = client->link_status;
    pixelica_online_sys_unlock(client->sync);
    return st;
}

PIXELICA_OnlineStatus pixelica_online_submit_score(PIXELICA_Online *client, const char *board,
    int32_t score, const void *record, size_t record_size)
{
    PIXELICA_OnlineStatus st = PIXELICA_ONLINE_OK;
    size_t board_size = 0;
    if (client == NULL || !valid_ident(board, &board_size) ||
        record_size > PIXELICA_ONLINE_MAX_RECORD || (record_size != 0 && record == NULL))
        return PIXELICA_ONLINE_INVALID_ARG;
    pixelica_online_sys_lock(client->sync);
    if (client->fatal) {
        st = client->link_status;
    } else {
        memcpy(client->score_board, board, board_size);
        client->score_board[board_size] = '\0';
        client->score_value = score;
        client->score_record_size = record_size;
        if (record_size != 0) memcpy(client->score_record, record, record_size);
        client->score_pending = true;
        pixelica_online_sys_wake(client->sync);
    }
    pixelica_online_sys_unlock(client->sync);
    return st;
}

PIXELICA_OnlineStatus pixelica_online_request_board(PIXELICA_Online *client, const char *board)
{
    PIXELICA_OnlineStatus st = PIXELICA_ONLINE_OK;
    size_t board_size = 0;
    if (client == NULL || !valid_ident(board, &board_size)) return PIXELICA_ONLINE_INVALID_ARG;
    pixelica_online_sys_lock(client->sync);
    if (client->fatal) {
        st = client->link_status;
    } else {
        memcpy(client->fetch_board, board, board_size);
        client->fetch_board[board_size] = '\0';
        client->fetch_pending = true;
        pixelica_online_sys_wake(client->sync);
    }
    pixelica_online_sys_unlock(client->sync);
    return st;
}

PIXELICA_OnlineStatus pixelica_online_leaderboard(PIXELICA_Online *client, PIXELICA_OnlineLeaderboard *out)
{
    if (client == NULL || out == NULL) return PIXELICA_ONLINE_INVALID_ARG;
    pixelica_online_sys_lock(client->sync);
    *out = client->board;
    pixelica_online_sys_unlock(client->sync);
    return PIXELICA_ONLINE_OK;
}
