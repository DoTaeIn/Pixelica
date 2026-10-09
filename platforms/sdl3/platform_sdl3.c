/* SDL3 PAL: pixel display, keyboard and clock. No 3D work happens here. */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "letterbox.h"
#include "platform_sdl3.h"

#define PIXELICA_SDL3_INITIAL_SCALE 3

struct PIXELICA_Sdl3Platform {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    /* Persistent RGB565 -> ARGB8888 conversion buffer; only set when the
     * renderer cannot create or accept an RGB565 streaming texture. */
    uint32_t *argb_buffer;
    uint16_t width;
    uint16_t height;
    bool video_initialized;
    bool quit_requested;
};

static void sdl3_report(const char *what)
{
    fprintf(stderr, "sdl3: %s: %s\n", what, SDL_GetError());
}

static uint32_t rgb565_to_argb8888(uint16_t pixel)
{
    uint32_t r = (uint32_t)(pixel >> 11) & 0x1Fu;
    uint32_t g = (uint32_t)(pixel >> 5) & 0x3Fu;
    uint32_t b = (uint32_t)pixel & 0x1Fu;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool pixelica_sdl3_create(const PIXELICA_Sdl3Config *config, PIXELICA_Sdl3Platform **out_platform)
{
    if (config == NULL || out_platform == NULL || config->width == 0u || config->height == 0u) {
        return false;
    }
    *out_platform = NULL;

    PIXELICA_Sdl3Platform *platform = calloc(1u, sizeof(*platform));
    if (platform == NULL) {
        return false;
    }
    platform->width = config->width;
    platform->height = config->height;

    if (config->hidden) {
        /* Override any SDL_VIDEODRIVER in the environment: hidden means dummy. */
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_OVERRIDE);
    }

    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        sdl3_report("SDL_InitSubSystem(VIDEO)");
        pixelica_sdl3_destroy(platform);
        return false;
    }
    platform->video_initialized = true;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (config->hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    platform->window = SDL_CreateWindow(config->title != NULL ? config->title : "Pixelica",
                                        (int)config->width * PIXELICA_SDL3_INITIAL_SCALE,
                                        (int)config->height * PIXELICA_SDL3_INITIAL_SCALE, flags);
    if (platform->window == NULL) {
        sdl3_report("SDL_CreateWindow");
        pixelica_sdl3_destroy(platform);
        return false;
    }

    platform->renderer = SDL_CreateRenderer(platform->window, NULL);
    if (platform->renderer == NULL) {
        sdl3_report("SDL_CreateRenderer (default driver), trying software");
        platform->renderer = SDL_CreateRenderer(platform->window, "software");
    }
    if (platform->renderer == NULL) {
        sdl3_report("SDL_CreateRenderer");
        pixelica_sdl3_destroy(platform);
        return false;
    }
    if (!config->hidden) {
        /* Best effort; presenting still works without vsync. */
        SDL_SetRenderVSync(platform->renderer, 1);
    }

    platform->texture = SDL_CreateTexture(platform->renderer, SDL_PIXELFORMAT_RGB565,
                                          SDL_TEXTUREACCESS_STREAMING, (int)config->width,
                                          (int)config->height);
    if (platform->texture == NULL) {
        /* Fall back to a 32-bit texture fed from a persistent conversion buffer. */
        size_t count = (size_t)config->width * (size_t)config->height;
        platform->argb_buffer = malloc(count * sizeof(uint32_t));
        if (platform->argb_buffer == NULL) {
            pixelica_sdl3_destroy(platform);
            return false;
        }
        platform->texture = SDL_CreateTexture(platform->renderer, SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING, (int)config->width,
                                              (int)config->height);
    }
    if (platform->texture == NULL) {
        sdl3_report("SDL_CreateTexture");
        pixelica_sdl3_destroy(platform);
        return false;
    }
    if (!SDL_SetTextureScaleMode(platform->texture, SDL_SCALEMODE_NEAREST)) {
        sdl3_report("SDL_SetTextureScaleMode");
        pixelica_sdl3_destroy(platform);
        return false;
    }

    *out_platform = platform;
    return true;
}

void pixelica_sdl3_destroy(PIXELICA_Sdl3Platform *platform)
{
    if (platform == NULL) {
        return;
    }
    if (platform->texture != NULL) {
        SDL_DestroyTexture(platform->texture);
    }
    if (platform->renderer != NULL) {
        SDL_DestroyRenderer(platform->renderer);
    }
    if (platform->window != NULL) {
        SDL_DestroyWindow(platform->window);
    }
    if (platform->video_initialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    free(platform->argb_buffer);
    free(platform);
}

bool pixelica_sdl3_pump_events(PIXELICA_Sdl3Platform *platform)
{
    if (platform == NULL) {
        return false;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            platform->quit_requested = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                platform->quit_requested = true;
            }
            break;
        default:
            break;
        }
    }
    return !platform->quit_requested;
}

static uint64_t sdl3_time_us(void *userdata)
{
    (void)userdata;
    return SDL_GetTicksNS() / 1000u;
}

static uint32_t sdl3_read_buttons(void *userdata)
{
    const PIXELICA_Sdl3Platform *platform = userdata;
    const bool *keys = SDL_GetKeyboardState(NULL);
    uint32_t buttons = 0u;

    if (platform != NULL && platform->quit_requested) {
        buttons |= PIXELICA_BTN_QUIT;
    }
    if (keys == NULL) {
        return buttons;
    }
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) {
        buttons |= PIXELICA_BTN_LEFT;
    }
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) {
        buttons |= PIXELICA_BTN_RIGHT;
    }
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) {
        buttons |= PIXELICA_BTN_UP;
    }
    if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) {
        buttons |= PIXELICA_BTN_DOWN;
    }
    if (keys[SDL_SCANCODE_SPACE]) {
        buttons |= PIXELICA_BTN_A;
    }
    if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]) {
        buttons |= PIXELICA_BTN_START;
    }
    if (keys[SDL_SCANCODE_ESCAPE]) {
        buttons |= PIXELICA_BTN_QUIT;
    }
    return buttons;
}

/* Synchronous: SDL_UpdateTexture copies the pixels before returning, so the
 * caller may reuse its buffer immediately. */
static bool sdl3_present_rgb565(void *userdata, const uint16_t *pixels, uint16_t width,
                                uint16_t height, uint32_t stride_pixels)
{
    PIXELICA_Sdl3Platform *platform = userdata;
    if (platform == NULL || pixels == NULL || width != platform->width ||
        height != platform->height || stride_pixels < width ||
        stride_pixels > (uint32_t)(INT_MAX / 2)) {
        return false;
    }

    if (platform->argb_buffer != NULL) {
        uint32_t *dst = platform->argb_buffer;
        for (uint32_t y = 0; y < height; ++y) {
            const uint16_t *src = pixels + (size_t)y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                *dst++ = rgb565_to_argb8888(src[x]);
            }
        }
        if (!SDL_UpdateTexture(platform->texture, NULL, platform->argb_buffer,
                               (int)width * (int)sizeof(uint32_t))) {
            sdl3_report("SDL_UpdateTexture (ARGB8888 fallback)");
            return false;
        }
    } else if (!SDL_UpdateTexture(platform->texture, NULL, pixels,
                                  (int)(stride_pixels * 2u))) {
        sdl3_report("SDL_UpdateTexture (RGB565)");
        return false;
    }

    int out_w = 0;
    int out_h = 0;
    if (!SDL_GetCurrentRenderOutputSize(platform->renderer, &out_w, &out_h)) {
        sdl3_report("SDL_GetCurrentRenderOutputSize");
        return false;
    }

    SDL_SetRenderDrawColor(platform->renderer, 0, 0, 0, 255);
    if (!SDL_RenderClear(platform->renderer)) {
        sdl3_report("SDL_RenderClear");
        return false;
    }

    PIXELICA_Sdl3Rect rect;
    if (pixelica_sdl3_compute_letterbox(out_w, out_h, width, height, &rect)) {
        SDL_FRect dst = {(float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h};
        if (!SDL_RenderTexture(platform->renderer, platform->texture, NULL, &dst)) {
            sdl3_report("SDL_RenderTexture");
            return false;
        }
    }
    /* else: zero-sized output (minimized); present the cleared frame. */

    if (!SDL_RenderPresent(platform->renderer)) {
        sdl3_report("SDL_RenderPresent");
        return false;
    }
    return true;
}

static void sdl3_log(void *userdata, const char *message)
{
    (void)userdata;
    fprintf(stderr, "%s\n", message != NULL ? message : "(null)");
}

PIXELICA_PlatformOps pixelica_sdl3_ops(PIXELICA_Sdl3Platform *platform)
{
    PIXELICA_PlatformOps ops;
    ops.userdata = platform;
    ops.time_us = sdl3_time_us;
    ops.read_buttons = sdl3_read_buttons;
    ops.present_rgb565 = sdl3_present_rgb565;
    ops.log = sdl3_log;
    return ops;
}
