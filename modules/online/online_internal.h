/* Private to the online module and its in-tree tests. Never installed. */
#ifndef PIXELICA_ONLINE_INTERNAL_H
#define PIXELICA_ONLINE_INTERNAL_H

#include "pixelica/online.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_ONLINE_PACKET_MAX 4096u /* request and response bodies */
#define PIXELICA_ONLINE_URL_MAX 512u

/* P3ON v1 opcodes; a response uses opcode | 0x80. */
#define PIXELICA_ONLINE_OP_HOST 1u
#define PIXELICA_ONLINE_OP_JOIN 2u
#define PIXELICA_ONLINE_OP_SYNC 3u
#define PIXELICA_ONLINE_OP_LEAVE 4u
#define PIXELICA_ONLINE_OP_SCORE 5u
#define PIXELICA_ONLINE_OP_BOARD 6u

typedef struct PIXELICA_OnlineEndpoint {
    char url[PIXELICA_ONLINE_URL_MAX + 32];  /* scheme://host[:port]/base/v1/online */
    char host[256];                     /* lowercase; IPv6 keeps its brackets */
    char path[PIXELICA_ONLINE_URL_MAX + 32]; /* /base/v1/online */
    uint16_t port;                      /* explicit or 443/80 */
    bool secure;
} PIXELICA_OnlineEndpoint;

/* HTTP POST of one P3ON packet. post returns OK only for HTTP 200 with a body
 * of at most capacity bytes; 408/429/5xx and network/TLS/timeout failures give
 * TRANSPORT, any other status or an oversized body PROTOCOL. A transport is used
 * by one thread at a time. */
typedef struct PIXELICA_OnlineTransportOps {
    void *(*create)(const PIXELICA_OnlineEndpoint *endpoint, uint32_t timeout_ms,
        PIXELICA_OnlineStatus *status);
    PIXELICA_OnlineStatus (*post)(void *transport, const uint8_t *body, size_t body_size,
        uint8_t *response, size_t capacity, size_t *response_size);
    void (*destroy)(void *transport);
} PIXELICA_OnlineTransportOps;

/* Platform layer: online_win32.c or online_posix.c. */
typedef struct PIXELICA_OnlineSync PIXELICA_OnlineSync;
typedef struct PIXELICA_OnlineThread PIXELICA_OnlineThread;
PIXELICA_OnlineSync *pixelica_online_sys_sync_create(void);
void pixelica_online_sys_sync_destroy(PIXELICA_OnlineSync *sync);
void pixelica_online_sys_lock(PIXELICA_OnlineSync *sync);
void pixelica_online_sys_unlock(PIXELICA_OnlineSync *sync);
/* Lock held; may wake early or spuriously. */
void pixelica_online_sys_wait(PIXELICA_OnlineSync *sync, uint32_t milliseconds);
void pixelica_online_sys_wake(PIXELICA_OnlineSync *sync);
PIXELICA_OnlineThread *pixelica_online_sys_thread_start(void (*entry)(void *), void *arg);
void pixelica_online_sys_thread_join(PIXELICA_OnlineThread *thread); /* also frees */
uint64_t pixelica_online_sys_now_ms(void);                      /* monotonic */
const PIXELICA_OnlineTransportOps *pixelica_online_sys_transport(void);

/* Shared HTTP status mapping for the transports (online.c). */
PIXELICA_OnlineStatus pixelica_online_internal_http_status(long http_code);

/* Test seams. set_transport replaces the platform transport for clients created
 * afterwards (NULL restores it); process-wide, call only while no host/join is
 * running. The parsers take a body without the 8-byte header, require exact
 * length and leave *out untouched unless they return OK. */
void pixelica_online_internal_set_transport(const PIXELICA_OnlineTransportOps *ops);
PIXELICA_OnlineStatus pixelica_online_internal_parse_url(const char *url, bool allow_insecure_lan,
    PIXELICA_OnlineEndpoint *out);
PIXELICA_OnlineStatus pixelica_online_internal_parse_header(const uint8_t *packet, size_t size,
    uint8_t request_opcode, size_t *body_size);
PIXELICA_OnlineStatus pixelica_online_internal_parse_snapshot(const uint8_t *body, size_t size,
    uint8_t self_id, PIXELICA_OnlineSnapshot *out);
PIXELICA_OnlineStatus pixelica_online_internal_parse_board(const uint8_t *body, size_t size,
    const char *expected_board, PIXELICA_OnlineLeaderboard *out);

#ifdef __cplusplus
}
#endif
#endif
