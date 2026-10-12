/* A complete menu shared by the headless capture and interactive SDL demo. */
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pixelica/image.h"
#include "pixelica/input.h"
#include "pixelica/ui.h"
#ifdef HELLO_UI_SDL
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

#define W 320u
#define H 200u
enum { TITLE = 1, LEFT_PANEL, RIGHT_PANEL, MENU_TITLE, RESUME, RESET, LOCKED,
       GRID, BRIGHTNESS, ENERGY, STATUS, FOOTER, WIDGET_COUNT = 12 };
typedef struct Demo {
    PIXELICA_UI_Widget widgets[WIDGET_COUNT];
    PIXELICA_UI_Context ui;
    unsigned resumed;
} Demo;
static uint16_t pixels[W * H];

static bool widget(Demo *demo, PIXELICA_UI_Kind kind, uint32_t id,
                   int32_t x, int32_t y, int32_t width, int32_t height, const char *text)
{
    PIXELICA_UI_Rect rect = {x, y, width, height};
    return pixelica_ui_widget_default(kind, id, rect, text, strlen(text),
                                     &demo->widgets[id - 1u]) == PIXELICA_OK;
}

static bool init_demo(Demo *demo)
{
    memset(demo, 0, sizeof(*demo));
    if (!widget(demo, PIXELICA_UI_LABEL, TITLE, 24, 17, 272, 10, "PIXELICA / STATION 04") ||
        !widget(demo, PIXELICA_UI_PANEL, LEFT_PANEL, 16, 38, 144, 137, "") ||
        !widget(demo, PIXELICA_UI_PANEL, RIGHT_PANEL, 168, 38, 136, 137, "") ||
        !widget(demo, PIXELICA_UI_LABEL, MENU_TITLE, 28, 50, 120, 10, "PAUSE MENU") ||
        !widget(demo, PIXELICA_UI_BUTTON, RESUME, 28, 69, 120, 24, "RESUME") ||
        !widget(demo, PIXELICA_UI_BUTTON, RESET, 28, 101, 120, 24, "RESET") ||
        !widget(demo, PIXELICA_UI_BUTTON, LOCKED, 28, 133, 120, 24, "LOCKED") ||
        !widget(demo, PIXELICA_UI_CHECKBOX, GRID, 180, 77, 112, 18, "SHOW GRID") ||
        !widget(demo, PIXELICA_UI_SLIDER, BRIGHTNESS, 180, 104, 112, 27, "BRIGHTNESS") ||
        !widget(demo, PIXELICA_UI_PROGRESS, ENERGY, 180, 145, 112, 16, "ENERGY 75%") ||
        !widget(demo, PIXELICA_UI_LABEL, STATUS, 180, 50, 112, 10, "PAUSED") ||
        !widget(demo, PIXELICA_UI_LABEL, FOOTER, 24, 184, 280, 10, "ARROWS: MOVE   SPACE: SELECT")) return false;
    demo->widgets[LOCKED - 1].enabled = false;
    demo->widgets[GRID - 1].checked = true;
    demo->widgets[BRIGHTNESS - 1].min = 0.2f;
    demo->widgets[BRIGHTNESS - 1].max = 1.0f;
    demo->widgets[BRIGHTNESS - 1].step = 0.1f;
    demo->widgets[BRIGHTNESS - 1].value = 0.5f;
    demo->widgets[ENERGY - 1].value = 0.75f;
    return pixelica_ui_init(&demo->ui, demo->widgets, WIDGET_COUNT) == PIXELICA_OK;
}

static bool update_demo(Demo *demo, const PIXELICA_UI_Input *input)
{
    if (pixelica_ui_update(&demo->ui, input) != PIXELICA_OK) return false;
    if (pixelica_ui_event(&demo->ui, RESUME).clicked) {
        ++demo->resumed;
        demo->widgets[STATUS - 1].text = "RUNNING";
        demo->widgets[STATUS - 1].text_bytes = 7u;
    }
    if (pixelica_ui_event(&demo->ui, RESET).clicked) {
        demo->widgets[GRID - 1].checked = true;
        demo->widgets[BRIGHTNESS - 1].value = 0.5f;
        demo->widgets[STATUS - 1].text = "PAUSED";
        demo->widgets[STATUS - 1].text_bytes = 6u;
    }
    return true;
}

static bool draw_demo(const Demo *demo)
{
    const float light = demo->widgets[BRIGHTNESS - 1].value;
    for (uint32_t y = 0u; y < H; ++y) {
        for (uint32_t x = 0u; x < W; ++x) {
            const uint32_t grid = demo->widgets[GRID - 1].checked &&
                (x % 20u == 0u || y % 20u == 0u) ? 16u : 0u;
            uint32_t r = (uint32_t)((float)(12u + y / 16u + grid) * light);
            uint32_t g = (uint32_t)((float)(20u + y / 12u + grid) * light);
            uint32_t b = (uint32_t)((float)(32u + y / 8u + grid) * light);
            pixels[y * W + x] = PIXELICA_RGB565(r, g, b);
        }
    }
    PIXELICA_UI_Surface surface = {pixels, W, H, W};
    return pixelica_ui_render_rgb565(&demo->ui, &surface) == PIXELICA_OK;
}

static uint64_t hash_pixels(void)
{
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0u; i < W * H; ++i) {
        hash = (hash ^ (pixels[i] & 255u)) * 1099511628211ull;
        hash = (hash ^ (pixels[i] >> 8)) * 1099511628211ull;
    }
    return hash;
}

static bool scripted(uint64_t *hash)
{
    Demo demo;
    if (!init_demo(&demo)) return false;
    for (unsigned frame = 0u; frame < 15u; ++frame) {
        uint32_t buttons = 0u;
        if (frame == 1u || frame == 7u) buttons = PIXELICA_BTN_A;
        if (frame == 3u || frame == 5u || frame == 9u) buttons = PIXELICA_BTN_DOWN;
        if (frame == 11u) buttons = PIXELICA_BTN_RIGHT;
        PIXELICA_UI_Input input = pixelica_ui_input_buttons(buttons);
        if (frame >= 13u) {
            input.pointer_valid = true;
            input.pointer_down = frame == 13u;
            input.pointer_x = 266; input.pointer_y = 120;
        }
        if (!update_demo(&demo, &input) || !draw_demo(&demo)) return false;
        if (frame == 11u && (demo.widgets[BRIGHTNESS - 1].value < 0.59f ||
                           demo.widgets[BRIGHTNESS - 1].value > 0.61f)) return false;
    }
    if (demo.resumed != 1u || demo.widgets[GRID - 1].checked ||
        demo.widgets[BRIGHTNESS - 1].value < 0.8f ||
        demo.widgets[BRIGHTNESS - 1].value > 0.83f) return false;
    *hash = hash_pixels();
    return true;
}

static bool write_ppm(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    bool ok = fprintf(file, "P6\n%u %u\n255\n", W, H) > 0;
    for (size_t i = 0u; ok && i < W * H; ++i) {
        const uint32_t p = pixels[i];
        const uint32_t r = p >> 11, g = (p >> 5) & 63u, b = p & 31u;
        const uint8_t rgb[3] = {(uint8_t)((r << 3) | (r >> 2)),
            (uint8_t)((g << 2) | (g >> 4)), (uint8_t)((b << 3) | (b >> 2))};
        ok = fwrite(rgb, 1u, 3u, file) == 3u;
    }
    if (fclose(file) != 0) ok = false;
    return ok;
}

#ifdef HELLO_UI_SDL
static int interactive(unsigned frames, bool hidden)
{
    Demo demo;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    static uint32_t argb[W * H];
    int result = 1;
    SDL_SetMainReady();
    if (hidden) {
        if (!SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy") ||
            !SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software")) return 1;
    }
    if (!init_demo(&demo) || !SDL_Init(SDL_INIT_VIDEO)) return 1;
    if (!SDL_CreateWindowAndRenderer("Pixelica UI - arrows, Space, mouse; Escape exits",
        (int)W * 3, (int)H * 3, hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE,
        &window, &renderer)) goto done;
    if (!SDL_SetRenderLogicalPresentation(renderer, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX)) goto done;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    if (texture == NULL || !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) goto done;
    bool running = true;
    unsigned rendered = 0u;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
        }
        const bool *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_ESCAPE]) break;
        uint32_t buttons = 0u;
        if (SDL_GetKeyboardFocus() == window) {
            if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) buttons |= PIXELICA_BTN_UP;
            if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) buttons |= PIXELICA_BTN_DOWN;
            if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) buttons |= PIXELICA_BTN_LEFT;
            if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) buttons |= PIXELICA_BTN_RIGHT;
            if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN]) buttons |= PIXELICA_BTN_A;
        }
        PIXELICA_UI_Input input = pixelica_ui_input_buttons(buttons);
        float mx = 0.0f, my = 0.0f, x = 0.0f, y = 0.0f;
        SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
        if (SDL_GetMouseFocus() == window && SDL_GetKeyboardFocus() == window &&
            SDL_RenderCoordinatesFromWindow(renderer, mx, my, &x, &y)) {
            if (x > -100000.0f && x < 100000.0f && y > -100000.0f && y < 100000.0f) {
                input.pointer_x = (int32_t)x; input.pointer_y = (int32_t)y;
                input.pointer_valid = true;
                input.pointer_down = (mouse & SDL_BUTTON_LMASK) != 0u;
            }
        }
        if (!update_demo(&demo, &input) || !draw_demo(&demo)) goto done;
        for (size_t i = 0u; i < W * H; ++i) {
            const uint32_t p = pixels[i], r = p >> 11, g = (p >> 5) & 63u, b = p & 31u;
            argb[i] = 0xff000000u | (((r << 3) | (r >> 2)) << 16) |
                (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        if (!SDL_UpdateTexture(texture, NULL, argb, (int)W * (int)sizeof(uint32_t)) ||
            !SDL_RenderClear(renderer) || !SDL_RenderTexture(renderer, texture, NULL, NULL) ||
            !SDL_RenderPresent(renderer)) goto done;
        if (frames != 0u && ++rendered >= frames) break;
        SDL_Delay(16u);
    }
    result = 0;
done:
    if (result != 0) fprintf(stderr, "hello_ui: %s\n", SDL_GetError());
    SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
#endif

int main(int argc, char **argv)
{
    bool smoke = false;
    bool hidden = false;
    unsigned frames = 0u;
    const char *output = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (strcmp(argv[i], "--hidden") == 0) hidden = true;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            char *end = NULL;
            const char *value = argv[++i];
            errno = 0;
            unsigned long parsed = strtoul(value, &end, 10);
            if (errno != 0 || value[0] < '0' || value[0] > '9' || *end != '\0' ||
                parsed == 0u || parsed > 10000u) return 2;
            frames = (unsigned)parsed;
        }
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output = argv[++i];
        else { fprintf(stderr, "usage: hello_ui [--smoke] [--output menu.ppm] [--hidden --frames N]\n"); return 2; }
    }
#ifdef HELLO_UI_SDL
    if (!smoke && output == NULL) return interactive(frames, hidden);
#else
    (void)smoke;
    (void)hidden;
    (void)frames;
#endif
    uint64_t first = 0u, second = 0u;
    if (!scripted(&first) || !scripted(&second) || first != second) return 1;
    if (output != NULL && !write_ppm(output)) return 1;
    printf("hello_ui: keyboard + pointer passed, %ux%u hash 0x%016" PRIx64 "\n", W, H, first);
    return 0;
}
