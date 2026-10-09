/*
 * Pixelica - bounded input recording, replay and portable serialization.
 * See include/pixelica/input.h for the canonical span form and the "P3IN" v1
 * byte layout.
 */
#include <string.h>

#include "pixelica/assets_common.h"
#include "pixelica/input.h"

/* Part of the common library: no 3D header here. Replay ticks exactly like
 * pixelica_update() because pixelica.h defines PIXELICA_UPDATE_HZ as PIXELICA_INPUT_TICK_HZ. */
typedef char pixelica_input_tick_rate_check[(PIXELICA_INPUT_TICK_HZ > 0u) ? 1 : -1];

#define PIXELICA_INPUT_MAGIC PIXELICA_FOURCC('P', '3', 'I', 'N')
#define PIXELICA_INPUT_VERSION 1u

void pixelica_input_recorder_init(PIXELICA_InputRecorder *rec, PIXELICA_InputSpan *storage,
                             size_t capacity)
{
    if (rec == NULL) {
        return;
    }
    rec->spans = storage;
    rec->capacity = storage != NULL ? capacity : 0u;
    rec->count = 0;
    rec->total_ticks = 0;
}

PIXELICA_Status pixelica_input_record(PIXELICA_InputRecorder *rec, uint32_t buttons)
{
    PIXELICA_InputSpan *last;

    if (rec == NULL || rec->count > rec->capacity ||
        (rec->spans == NULL && rec->capacity > 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    buttons &= PIXELICA_BTN_ALL;
    last = rec->count > 0 ? &rec->spans[rec->count - 1u] : NULL;
    if (last != NULL && last->buttons == buttons && last->ticks < UINT32_MAX) {
        last->ticks += 1u;
    } else {
        if (rec->count >= rec->capacity) {
            return PIXELICA_ERR_CAPACITY;
        }
        rec->spans[rec->count].buttons = buttons;
        rec->spans[rec->count].ticks = 1u;
        rec->count += 1u;
    }
    rec->total_ticks += 1u;
    return PIXELICA_OK;
}

void pixelica_input_player_init(PIXELICA_InputPlayer *player, const PIXELICA_InputSpan *spans,
                           size_t count)
{
    if (player == NULL) {
        return;
    }
    player->spans = spans;
    player->count = spans != NULL ? count : 0u;
    player->span = 0;
    player->used = 0;
    player->tick = 0;
}

bool pixelica_input_player_next(PIXELICA_InputPlayer *player, uint32_t *out_buttons)
{
    if (out_buttons != NULL) {
        *out_buttons = 0;
    }
    if (player == NULL || player->spans == NULL) {
        return false;
    }
    while (player->span < player->count &&
           player->used >= player->spans[player->span].ticks) {
        player->span += 1u;
        player->used = 0;
    }
    if (player->span >= player->count) {
        return false;
    }
    if (out_buttons != NULL) {
        *out_buttons = player->spans[player->span].buttons;
    }
    player->used += 1u;
    player->tick += 1u;
    return true;
}

uint64_t pixelica_input_replay(const PIXELICA_InputSpan *spans, size_t count,
                          PIXELICA_InputTickFn tick, void *userdata)
{
    const float dt = 1.0f / (float)PIXELICA_INPUT_TICK_HZ;
    PIXELICA_InputPlayer player;
    uint32_t buttons;

    if (spans == NULL || tick == NULL) {
        return 0;
    }
    pixelica_input_player_init(&player, spans, count);
    while (pixelica_input_player_next(&player, &buttons)) {
        tick(userdata, dt, buttons);
    }
    return player.tick;
}

static void pixelica_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void pixelica_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t pixelica_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t pixelica_rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* 1 if span i is canonical given its predecessor (NULL for the first). */
static int pixelica_span_canonical(const PIXELICA_InputSpan *prev, uint32_t buttons,
                              uint32_t ticks)
{
    if (ticks == 0 || (buttons & ~PIXELICA_BTN_ALL) != 0) {
        return 0;
    }
    if (prev != NULL && prev->buttons == buttons && prev->ticks != UINT32_MAX) {
        return 0; /* should have been merged into the previous span */
    }
    return 1;
}

PIXELICA_Status pixelica_input_encoded_size(size_t span_count, size_t *out_bytes)
{
    if (out_bytes == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_bytes = 0;
#if SIZE_MAX > UINT32_MAX
    /* The file stores span_count as u32. With a 32-bit size_t the byte-size
     * bound below is already stricter. */
    if (span_count > (size_t)UINT32_MAX) {
        return PIXELICA_ERR_CAPACITY;
    }
#endif
    if (span_count > (SIZE_MAX - PIXELICA_INPUT_HEADER_BYTES) / PIXELICA_INPUT_SPAN_BYTES) {
        return PIXELICA_ERR_CAPACITY;
    }
    *out_bytes = PIXELICA_INPUT_HEADER_BYTES + span_count * PIXELICA_INPUT_SPAN_BYTES;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_encode(const PIXELICA_InputSpan *spans, size_t count,
                            void *out, size_t out_size, size_t *out_written)
{
    uint8_t *p = (uint8_t *)out;
    size_t need;
    size_t i;
    PIXELICA_Status st;
    uint32_t crc;

    if (out_written == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_written = 0;
    if (out == NULL || (spans == NULL && count > 0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; ++i) {
        if (!pixelica_span_canonical(i > 0 ? &spans[i - 1u] : NULL,
                                spans[i].buttons, spans[i].ticks)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    st = pixelica_input_encoded_size(count, &need);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (out_size < need) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    for (i = 0; i < count; ++i) {
        uint8_t *s = p + PIXELICA_INPUT_HEADER_BYTES + i * PIXELICA_INPUT_SPAN_BYTES;
        pixelica_wr_u32(s, spans[i].buttons);
        pixelica_wr_u32(s + 4, spans[i].ticks);
    }
    crc = pixelica_crc32(p + PIXELICA_INPUT_HEADER_BYTES, need - PIXELICA_INPUT_HEADER_BYTES);
    pixelica_wr_u32(p, PIXELICA_INPUT_MAGIC);
    pixelica_wr_u16(p + 4, (uint16_t)PIXELICA_INPUT_VERSION);
    pixelica_wr_u16(p + 6, 0u);
    pixelica_wr_u32(p + 8, (uint32_t)count);
    pixelica_wr_u32(p + 12, crc);
    *out_written = need;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_decode_count(const void *data, size_t size,
                                  size_t *out_count)
{
    const uint8_t *p = (const uint8_t *)data;
    const PIXELICA_InputSpan *prev = NULL;
    PIXELICA_InputSpan prev_span;
    uint32_t count;
    size_t i;

    if (out_count == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (p == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (size < PIXELICA_INPUT_HEADER_BYTES || pixelica_rd32(p) != PIXELICA_INPUT_MAGIC ||
        (uint32_t)pixelica_rd16(p + 4) != PIXELICA_INPUT_VERSION ||
        (uint32_t)pixelica_rd16(p + 6) != 0u) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    count = pixelica_rd32(p + 8);
    /* Exact size, checked by division so nothing can overflow. */
    if ((size - PIXELICA_INPUT_HEADER_BYTES) % PIXELICA_INPUT_SPAN_BYTES != 0 ||
        (size - PIXELICA_INPUT_HEADER_BYTES) / PIXELICA_INPUT_SPAN_BYTES != (size_t)count) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    if (pixelica_crc32(p + PIXELICA_INPUT_HEADER_BYTES, size - PIXELICA_INPUT_HEADER_BYTES) !=
        pixelica_rd32(p + 12)) {
        return PIXELICA_ERR_BAD_ASSET;
    }
    for (i = 0; i < (size_t)count; ++i) {
        const uint8_t *s = p + PIXELICA_INPUT_HEADER_BYTES + i * PIXELICA_INPUT_SPAN_BYTES;
        const uint32_t buttons = pixelica_rd32(s);
        const uint32_t ticks = pixelica_rd32(s + 4);

        if (!pixelica_span_canonical(prev, buttons, ticks)) {
            return PIXELICA_ERR_BAD_ASSET;
        }
        prev_span.buttons = buttons;
        prev_span.ticks = ticks;
        prev = &prev_span;
    }
    *out_count = (size_t)count;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_decode(const void *data, size_t size, PIXELICA_InputSpan *out,
                            size_t capacity, size_t *out_count)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t count = 0;
    size_t i;
    PIXELICA_Status st;

    if (out_count == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (out == NULL && capacity > 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    st = pixelica_input_decode_count(data, size, &count);
    if (st != PIXELICA_OK) {
        return st;
    }
    if (count > capacity) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    for (i = 0; i < count; ++i) {
        const uint8_t *s = p + PIXELICA_INPUT_HEADER_BYTES + i * PIXELICA_INPUT_SPAN_BYTES;
        out[i].buttons = pixelica_rd32(s);
        out[i].ticks = pixelica_rd32(s + 4);
    }
    *out_count = count;
    return PIXELICA_OK;
}
