/*
 * hello_online - the game itself (no networking, no renderer, no platform).
 *
 * This file and online_game.c depend only on <stdint.h>/<string.h>/<math.h>,
 * so the SAME simulation, wire codec and bot run in the SDL3 build, in the
 * headless console build and in the unit test.
 *
 * The game ("Star Catch", up to 8 players in a 256x192 arena):
 *   - COOP    everybody collects stars for ONE shared team score; drifting
 *             spikes stun a player (and cost the team 1 point).
 *   - VERSUS  every player has an own score; a dashing player bumps others,
 *             stuns them and steals 1 point from them.
 *
 * Authority: the HOST runs HolWorld at a fixed 60 Hz from the latest input of
 * every player and publishes the encoded HolView (about 200 bytes, big-endian,
 * hand-written codec, no raw C structs on the wire). Guests send two bytes of
 * input and only draw what the host published. A guest that does not answer
 * simply stands still; nothing here waits for the network.
 *
 * Wire formats (all integers big-endian, floats are IEEE binary32 bits):
 *   input  : u8 version(=1), u8 buttons (HOL_IN_* bits)                 2 bytes
 *   state  : u8 version(=1), u8 mode, u8 phase, u16 round, u32 seed,
 *            u32 tick, u32 round_ticks, u16 team_score,
 *            u8 star_count,   star_count   * (f32 x, f32 y),
 *            u8 hazard_count, hazard_count * (f32 x, f32 y),
 *            u8 player_count, player_count * (u8 id, f32 x, f32 y,
 *                                             u16 score, u8 stun, u8 dash)
 *   record : u8 version(=1), u8 mode, u8 players, u16 round_seconds,
 *            u32 seed                                                   9 bytes
 */
#ifndef HELLO_ONLINE_GAME_H
#define HELLO_ONLINE_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOL_MAX_PLAYERS 8u
#define HOL_MAX_STARS 5u
#define HOL_MAX_HAZARDS 3u
#define HOL_TICK_HZ 60u

#define HOL_ARENA_W 256.0f
#define HOL_ARENA_H 192.0f
#define HOL_PLAYER_RADIUS 6.0f
#define HOL_STAR_RADIUS 5.0f
#define HOL_HAZARD_RADIUS 7.0f

#define HOL_INPUT_SIZE 2u
#define HOL_RECORD_SIZE 9u
#define HOL_STATE_MAX 256u /* upper bound of hol_view_encode output */

#define HOL_DEFAULT_ROUND_SECONDS 45u
#define HOL_MIN_ROUND_SECONDS 3u
#define HOL_MAX_ROUND_SECONDS 300u

/* Input button bits (the byte after the version byte). */
#define HOL_IN_LEFT 1u
#define HOL_IN_RIGHT 2u
#define HOL_IN_UP 4u
#define HOL_IN_DOWN 8u
#define HOL_IN_DASH 16u
#define HOL_IN_ALL 31u

typedef enum HolMode {
    HOL_MODE_COOP = 1,
    HOL_MODE_VERSUS = 2
} HolMode;

typedef enum HolPhase {
    HOL_PHASE_LOBBY = 0,
    HOL_PHASE_PLAYING = 1,
    HOL_PHASE_FINISHED = 2
} HolPhase;

typedef struct HolVec {
    float x, y;
} HolVec;

typedef struct HolPlayer {
    uint8_t id;    /* the online player id (0..7) */
    HolVec pos;
    uint16_t score;
    uint8_t stun;  /* ticks left, 0 = free */
    uint8_t dash;  /* ticks of dash left, 0 = not dashing */
} HolPlayer;

/* Everything that is published to the guests (and drawn by everybody). */
typedef struct HolView {
    HolMode mode;
    HolPhase phase;
    uint16_t round;       /* 0 before the first round, +1 for every started round */
    uint32_t seed;
    uint32_t tick;        /* ticks played in this round */
    uint32_t round_ticks; /* the round ends when tick reaches this */
    uint16_t team_score;  /* COOP only; always 0 in VERSUS */
    uint8_t star_count;
    HolVec stars[HOL_MAX_STARS];
    uint8_t hazard_count;
    HolVec hazards[HOL_MAX_HAZARDS];
    uint8_t player_count;
    HolPlayer players[HOL_MAX_PLAYERS]; /* compact list, ascending id */
} HolView;

/* ------------------------------------------------------------------ codec */

/* Returns the byte count written (> 0) or 0 when `cap` is too small. */
size_t hol_view_encode(const HolView *view, uint8_t *out, size_t cap);
/*
 * Validates EVERYTHING (exact length, version, enums, finite coordinates inside
 * the arena, unique ascending ids, counts) before touching *out. false = the
 * bytes were not a valid state and *out is unchanged.
 */
bool hol_view_decode(const uint8_t *data, size_t size, HolView *out);

void hol_input_encode(uint8_t buttons, uint8_t out[HOL_INPUT_SIZE]);
/* false (and *buttons unchanged) unless exactly a valid 2-byte input. */
bool hol_input_decode(const uint8_t *data, size_t size, uint8_t *buttons);

/* The "record" stored next to a score: how the score was made. */
void hol_record_encode(const HolView *view, uint8_t out[HOL_RECORD_SIZE]);
typedef struct HolRecord {
    HolMode mode;
    uint8_t players;
    uint16_t round_seconds;
    uint32_t seed;
} HolRecord;
bool hol_record_decode(const uint8_t *data, size_t size, HolRecord *out);

/* ------------------------------------------------------- host simulation */

typedef struct HolWorld {
    HolView view;
    uint32_t rng;
    HolVec hazard_velocity[HOL_MAX_HAZARDS];
    uint8_t dash_cooldown[HOL_MAX_PLAYERS]; /* indexed by player id */
    HolVec facing[HOL_MAX_PLAYERS];         /* last move direction by player id */
} HolWorld;

/* A lobby with nobody in it. round_ticks = seconds * HOL_TICK_HZ. */
void hol_world_init(HolWorld *world, HolMode mode, uint32_t round_seconds, uint32_t seed);

/*
 * Makes the player set equal to `ids` (ascending, unique, each < 8, at most 8).
 * Known players keep their state, new ones appear at their spawn point with
 * score 0, missing ones disappear. Returns false for an invalid list.
 */
bool hol_world_set_players(HolWorld *world, const uint8_t *ids, size_t count);

/* Lobby/finished -> playing: new round number, scores and stars reset. */
void hol_world_start_round(HolWorld *world, uint32_t seed);

/*
 * One 60 Hz tick while PLAYING (no-op otherwise). `buttons[id]` is the input of
 * player `id`. Returns true when this tick finished the round.
 */
bool hol_world_step(HolWorld *world, const uint8_t buttons[HOL_MAX_PLAYERS]);

/* Index in view->players of `id`, or -1. */
int hol_view_find_player(const HolView *view, uint8_t id);
/* Id of the best player (highest score, lower id wins ties); -1 if none. */
int hol_view_leader(const HolView *view);
/* Score this player contributes to the board: team score in COOP, own in VERSUS. */
bool hol_view_board_score(const HolView *view, uint8_t self_id, int32_t *out);

/* ------------------------------------------------------------------- bot */

/*
 * Deterministic autopilot: chases the nearest star and sidesteps spikes using
 * only the published view (so a guest bot needs no extra network data).
 * Returns HOL_IN_* bits.
 */
uint8_t hol_bot_buttons(const HolView *view, uint8_t self_id, uint32_t frame);

#ifdef __cplusplus
}
#endif

#endif /* HELLO_ONLINE_GAME_H */
