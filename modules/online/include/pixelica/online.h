/* Optional online client: room-code co-op, casual real-time versus and
 * persistent best-score record boards, shared by the 2D, 2.5D and 3D profiles.
 *
 * Honest scope: a background worker exchanges the latest input of every player
 * and the host's world state with the Pixelica relay (tools/online_server) over
 * HTTPS (plain HTTP only for loopback, or private LAN when explicitly allowed)
 * at sync_hz, default 30 Hz, with latest-state-wins semantics. This suits casual
 * co-op and versus play. It is NOT rollback or UDP netcode and does not hide
 * latency. Scores are self-reported by clients: boards are not anti-cheat and
 * not a ranked service.
 *
 * Threading: call every function from the thread that created the client (the
 * game thread). Network I/O runs on an internal worker. Only pixelica_online_host and
 * pixelica_online_join (one request) and pixelica_online_destroy
 * (one in-flight request plus one leave request) wait for the network; every
 * other call only copies memory under a short lock. Linux applies timeout_ms
 * to the whole request; Windows applies it per WinHTTP phase and checks elapsed
 * time between body reads, so a synchronous operation can take several phases.
 *
 * No automatic reconnect: when the snapshot reports a fatal status (room closed,
 * session expired or revoked), destroy the client and join again. A room dies
 * when its host leaves or expires. */
#ifndef PIXELICA_ONLINE_H
#define PIXELICA_ONLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_ONLINE_MAX_PLAYERS 8u
#define PIXELICA_ONLINE_MAX_INPUT 64u
#define PIXELICA_ONLINE_MAX_STATE 2048u
#define PIXELICA_ONLINE_MAX_RECORD 128u
#define PIXELICA_ONLINE_MAX_SCORES 10u
#define PIXELICA_ONLINE_MAX_NAME 32u /* bytes of UTF-8, also game id / board name */
#define PIXELICA_ONLINE_ROOM_CODE_LENGTH 6u

typedef struct PIXELICA_Online PIXELICA_Online;

typedef enum PIXELICA_OnlineMode {
    PIXELICA_ONLINE_COOP = 1,
    PIXELICA_ONLINE_VERSUS = 2
} PIXELICA_OnlineMode;

typedef enum PIXELICA_OnlineStatus {
    PIXELICA_ONLINE_OK = 0,
    /* Reported by the relay. */
    PIXELICA_ONLINE_BAD_REQUEST = 1,
    PIXELICA_ONLINE_NOT_FOUND = 2,
    PIXELICA_ONLINE_FULL = 3,
    PIXELICA_ONLINE_UNAUTHORIZED = 4,
    PIXELICA_ONLINE_NOT_HOST = 5,
    PIXELICA_ONLINE_CAPACITY = 6,
    PIXELICA_ONLINE_RATE_LIMIT = 7,
    PIXELICA_ONLINE_ROOM_CLOSED = 8,
    PIXELICA_ONLINE_INTERNAL = 9,
    /* Local. */
    PIXELICA_ONLINE_TRANSPORT = 100,  /* network, TLS, timeout, HTTP 408/429/5xx */
    PIXELICA_ONLINE_PROTOCOL = 101,   /* malformed/oversized reply, other HTTP status */
    PIXELICA_ONLINE_NO_MEMORY = 102,
    PIXELICA_ONLINE_INVALID_ARG = 103,
    PIXELICA_ONLINE_UNSUPPORTED = 104
} PIXELICA_OnlineStatus;

typedef struct PIXELICA_OnlineConfig {
    /* "https://host[:port][/base]" or, for loopback only, "http://localhost",
     * "http://127.0.0.1" or "http://[::1]" (optional port/base). At most 512
     * bytes; no credentials, query or fragment. Requests go to <base>/v1/online,
     * HTTPS verifies the certificate and host name, redirects are refused. */
    const char *server_url;
    const char *game_id;     /* [A-Za-z0-9_.-]{1,32}; partitions rooms and boards */
    const char *player_name; /* non-empty UTF-8, <= 32 bytes, no control chars */
    /* Optional 64 hex chars from a previous pixelica_online_player_key() to keep the
     * same board identity; NULL asks the relay for a new random identity. */
    const char *player_key;
    uint32_t timeout_ms;     /* request budget (WinHTTP: per phase), 100..10000, default 1500 */
    uint32_t sync_hz;        /* 1..60, default 30 */
    /* Also permit http:// to private IPv4 literals (10/8, 172.16/12,
     * 192.168/16, 169.254/16, 127/8) and *.local names. Unencrypted. */
    bool allow_insecure_lan;
} PIXELICA_OnlineConfig;

typedef struct PIXELICA_OnlinePlayer {
    uint8_t id;                  /* 0..7, stable for the session */
    char name[PIXELICA_ONLINE_MAX_NAME + 1];
    uint32_t input_sequence;     /* 0 = never published */
    size_t input_size;
    uint8_t input[PIXELICA_ONLINE_MAX_INPUT];
} PIXELICA_OnlinePlayer;

typedef struct PIXELICA_OnlineSnapshot {
    char room_code[PIXELICA_ONLINE_ROOM_CODE_LENGTH + 1];
    uint8_t self_id;
    uint8_t host_id;
    uint8_t player_count;
    uint8_t max_players;
    PIXELICA_OnlineMode mode;
    /* false while the latest exchange failed or after a fatal status; the rest
     * is then the last good snapshot, not new authoritative data. */
    bool connected;
    uint32_t revision;           /* informational */
    uint32_t state_sequence;     /* host world state, 0 = never published */
    size_t state_size;
    uint8_t state[PIXELICA_ONLINE_MAX_STATE];
    PIXELICA_OnlinePlayer players[PIXELICA_ONLINE_MAX_PLAYERS]; /* first player_count used */
} PIXELICA_OnlineSnapshot;

typedef struct PIXELICA_OnlineScore {
    char name[PIXELICA_ONLINE_MAX_NAME + 1]; /* name at the time of the best score */
    int32_t score;
    size_t record_size;
    uint8_t record[PIXELICA_ONLINE_MAX_RECORD]; /* opaque bytes stored with the score */
} PIXELICA_OnlineScore;

typedef struct PIXELICA_OnlineLeaderboard {
    uint32_t generation;         /* +1 per completed board operation, 0 = none yet */
    PIXELICA_OnlineStatus status;     /* result of the latest completed operation */
    char board[PIXELICA_ONLINE_MAX_NAME + 1];
    size_t count;
    PIXELICA_OnlineScore entries[PIXELICA_ONLINE_MAX_SCORES]; /* best first */
} PIXELICA_OnlineLeaderboard;

PIXELICA_OnlineConfig pixelica_online_config_default(void);

/* Create a room and join it as host (player 0 is not guaranteed; use
 * snapshot.host_id/self_id). Connects synchronously; see platform timeout above.
 * max_players 2..8. Config strings are copied. Returns NULL on failure and
 * stores the reason in *error (error may be NULL). */
PIXELICA_Online *pixelica_online_host(const PIXELICA_OnlineConfig *config, PIXELICA_OnlineMode mode,
    uint8_t max_players, PIXELICA_OnlineStatus *error);
/* Join an existing room by its 6-character code (lowercase accepted). */
PIXELICA_Online *pixelica_online_join(const PIXELICA_OnlineConfig *config, const char *room_code,
    PIXELICA_OnlineStatus *error);
/* Stops the worker, sends a best-effort leave and frees. NULL is safe. A host
 * leaving closes the room for everyone. */
void pixelica_online_destroy(PIXELICA_Online *client);

/* Valid until destroy. The key is a secret persistent identity for the record
 * boards: store it privately if wanted, never print or share it. "" on NULL. */
const char *pixelica_online_room_code(const PIXELICA_Online *client);
const char *pixelica_online_player_key(const PIXELICA_Online *client);

/* Publish this player's latest input (<= 64 bytes). Copied immediately; latest
 * wins; size 0 clears. The sequence advances only when the bytes change. Encode
 * fields explicitly (e.g. big-endian), never send raw C structs. Returns the
 * fatal status once the session is over. */
PIXELICA_OnlineStatus pixelica_online_input(PIXELICA_Online *client, const void *data, size_t size);
/* Host only (else PIXELICA_ONLINE_NOT_HOST): publish the authoritative world state
 * (<= 2048 bytes), same rules as pixelica_online_input. */
PIXELICA_OnlineStatus pixelica_online_state(PIXELICA_Online *client, const void *data, size_t size);
/* Copy the latest verified room snapshot. Returns the connection status: OK,
 * a transient error being retried with capped backoff (TRANSPORT, PROTOCOL,
 * RATE_LIMIT, CAPACITY, INTERNAL), or a fatal status after which the worker no
 * longer exchanges data. */
PIXELICA_OnlineStatus pixelica_online_snapshot(PIXELICA_Online *client, PIXELICA_OnlineSnapshot *out);

/* Record boards, partitioned by game_id + board ([A-Za-z0-9_.-]{1,32}). The
 * relay keeps one best score per player key. Non-blocking: there is one pending
 * score slot and one pending fetch slot, each latest-wins (a newer call
 * replaces a request the worker has not started). A completed submit also
 * returns the refreshed top 10. record may be NULL when record_size is 0. */
PIXELICA_OnlineStatus pixelica_online_submit_score(PIXELICA_Online *client, const char *board,
    int32_t score, const void *record, size_t record_size);
PIXELICA_OnlineStatus pixelica_online_request_board(PIXELICA_Online *client, const char *board);
/* Copy the last completed board result. On a failed operation generation still
 * advances and status holds the error, while board/count/entries keep the last
 * successful result. */
PIXELICA_OnlineStatus pixelica_online_leaderboard(PIXELICA_Online *client, PIXELICA_OnlineLeaderboard *out);

const char *pixelica_online_status_string(PIXELICA_OnlineStatus status);

#ifdef __cplusplus
}
#endif
#endif
