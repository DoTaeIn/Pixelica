/*
 * hello_online - the networking glue between the game (online_game.h) and the
 * Pixelica online module (pixelica/online.h).
 *
 * One HolSession is driven by ONE thread (the game thread) with exactly one
 * hol_session_tick() per 60 Hz frame. Every pixelica_online_* call below is
 * non-blocking after the room was created/joined; hol_session_host/join are the
 * only calls that wait (see the platform timeout rules in online.h).
 *
 * The SAME three features are used on every machine, there is no second
 * "score API":
 *   1. room code + co-op / versus   pixelica_online_host / join, mode in the room
 *   2. realtime exchange            pixelica_online_input (guest -> host, 2 bytes) and
 *                                   pixelica_online_state (host -> guests, ~190 bytes)
 *   3. scores and records           pixelica_online_submit_score / request_board /
 *                                   leaderboard, with a 9-byte record per score
 *
 * Honest limits (also in docs/online.md): casual realtime over HTTP relay, no
 * rollback, no prediction, scores are self-reported and not cheat-proof.
 */
#ifndef HELLO_ONLINE_SESSION_H
#define HELLO_ONLINE_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "online_game.h"
#include "pixelica/online.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOL_NAME_BYTES 33u /* 32 UTF-8 bytes + NUL, like the online module */
#define HOL_BOARD_MAX_TRIES 5u

typedef struct HolSession {
    PIXELICA_Online *online;
    bool is_host;
    bool connected;            /* latest snapshot says the relay is reachable */
    PIXELICA_OnlineStatus last_status;
    uint8_t self_id;
    uint8_t host_id;
    uint8_t player_count;
    uint8_t max_players;
    char room_code[7];
    char names[HOL_MAX_PLAYERS][HOL_NAME_BYTES]; /* by player id, "" = absent */

    HolView view;              /* what is drawn: host world or last valid guest state */
    bool have_view;
    uint32_t bad_states;       /* guest: published states that failed validation */
    HolVec shown[HOL_MAX_PLAYERS]; /* smoothed draw position by player id */
    bool shown_valid[HOL_MAX_PLAYERS];

    /* host only */
    HolWorld world;
    uint32_t want_players;     /* > 0: start the first round automatically at this count */
    bool auto_started;
    uint32_t seed_base;

    /* scores */
    char board[HOL_NAME_BYTES];
    bool has_forced_score;
    int32_t forced_score;
    bool submitted;            /* the score of `submitted_round` was queued (or given up) */
    uint16_t submitted_round;
    int32_t submitted_score;
    uint8_t submit_tries;
    bool score_pending;
    uint32_t submit_generation;
    PIXELICA_OnlineStatus submit_error; /* 0 unless every try failed */
    uint32_t next_submit_frame;
    uint32_t seen_generation;
    bool board_updated;        /* set when a board result arrived; the caller clears it */
    PIXELICA_OnlineLeaderboard leaderboard;

    uint32_t frame;
    PIXELICA_OnlineSnapshot snap;   /* last snapshot (names, input, connection) */
} HolSession;

/* NULL / "" board = "coop" or "versus" by room mode. Both calls connect synchronously. */
bool hol_session_host(HolSession *s, const PIXELICA_OnlineConfig *config, HolMode mode,
                      uint8_t max_players, uint32_t round_seconds, const char *board,
                      PIXELICA_OnlineStatus *error);
bool hol_session_join(HolSession *s, const PIXELICA_OnlineConfig *config, const char *room_code,
                      const char *board, PIXELICA_OnlineStatus *error);

/* Best-effort leave + free. Safe on a zeroed or already closed session. */
void hol_session_close(HolSession *s);

/* Score submitted at round end instead of the played score (demo: --score N). */
void hol_session_force_score(HolSession *s, int32_t score);

/* Board-only use: submit now / fetch now, no round needed. */
PIXELICA_OnlineStatus hol_session_submit_now(HolSession *s, int32_t score, const uint8_t *record,
                                        size_t record_size);
PIXELICA_OnlineStatus hol_session_request_board(HolSession *s);

/*
 * One 60 Hz frame. `buttons` are HOL_IN_* bits of THIS machine; `start` is a
 * one-frame START press (the host starts/restarts the round with it).
 */
void hol_session_tick(HolSession *s, uint8_t buttons, bool start);

/* The default board for a mode: "coop" or "versus". */
const char *hol_default_board(HolMode mode);
/* [A-Za-z0-9_.-]{1,32} */
bool hol_valid_board_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HELLO_ONLINE_SESSION_H */
