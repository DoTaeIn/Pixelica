/*
 * SDL3 adapter for the optional input manager (pixelica/input_manager.h).
 *
 * Samples the SDL keyboard and up to three SDL gamepads and submits them to a
 * caller-owned PIXELICA_InputManager as device snapshots. Build-only: it lives in
 * platforms/sdl3, is never installed and is compiled only when both
 * PIXELICA_BUILD_INPUT_MANAGER=ON and PIXELICA_PLATFORM=SDL3. CMake target:
 * pixelica_input_sdl3 (Pixelica::input_sdl3). The header includes SDL, so SDL is a
 * PUBLIC dependency of that target.
 *
 * Sources (manager device indices). The adapter owns exactly these and never
 * touches any other one, so indices above the pad count stay free for custom
 * hardware drivers (GPIO, serial, ...); the adapter never assigns pins or
 * protocols.
 *   0                    keyboard, 64 remappable button slots, no axes
 *   1 .. gamepad_count   gamepad slots (gamepad_count is 0..3)
 *
 * Gamepad snapshot: buttons bit N = SDL_GamepadButton N (SDL_GAMEPAD_BUTTON_*),
 * axes[N] = SDL_GamepadAxis N for N = 0..5 (LEFTX, LEFTY, RIGHTX, RIGHTY,
 * LEFT_TRIGGER, RIGHT_TRIGGER); axes[6] and axes[7] are always 0. Sticks are
 * normalised negative/32768, positive/32767 into [-1,1]; triggers into [0,1].
 * No deadzone is applied: that is the manager's per-binding deadzone.
 *
 * Usage, once per simulation step and from the thread that runs SDL's event
 * loop (the one that created the window):
 *   pixelica_input_sdl3_poll(&adapter, window_has_focus);
 *   pixelica_input_manager_tick(&manager);   // the adapter never ticks
 *
 * Single-threaded. One adapter per manager (two adapters on one manager would
 * overwrite each other's sources). Several adapters on different managers are
 * fine: each init/shutdown pair is balanced against SDL's subsystem refcount.
 */
#ifndef PIXELICA_INPUT_SDL3_H
#define PIXELICA_INPUT_SDL3_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

#include "pixelica/input_manager.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Source 0 is the keyboard, so a manager has room for three gamepads. */
#define PIXELICA_INPUT_SDL3_MAX_GAMEPADS 3u

/* Caller-owned. Zero-initialise before the first pixelica_input_sdl3_init (for
 * example `PIXELICA_InputSdl3 a = {0};`); the fields are implementation storage. */
typedef struct PIXELICA_InputSdl3 {
    PIXELICA_InputManager *manager; /* not owned; must outlive the adapter */
    SDL_Scancode keys[PIXELICA_INPUT_MAX_BUTTONS];
    SDL_Gamepad *pads[PIXELICA_INPUT_SDL3_MAX_GAMEPADS];
    SDL_JoystickID pad_ids[PIXELICA_INPUT_SDL3_MAX_GAMEPADS];
    uint8_t pad_count;
    bool initialized;
} PIXELICA_InputSdl3;

/*
 * Binds the adapter to `manager`, takes one reference on SDL's gamepad
 * subsystem (SDL_INIT_GAMEPAD, which also brings up joystick and events) and
 * installs the default keyboard map:
 *   button 0 LEFT, 1 RIGHT, 2 UP, 3 DOWN, 4 SPACE, 5 RETURN, 6 ESCAPE,
 *   7 A, 8 D, 9 W, 10 S, 11 KP_ENTER, 12..63 SDL_SCANCODE_UNKNOWN (unmapped).
 * The buttons are keyboard-device button indices for PIXELICA_InputBinding; they are
 * not PIXELICA_InputActionId values. Sources 0..gamepad_count are submitted once as
 * neutral (disconnected). Gamepads are opened by pixelica_input_sdl3_poll, not here.
 *
 * PIXELICA_ERR_INVALID_ARG: NULL adapter or manager, gamepad_count > 3, or the
 * adapter is already initialised (shut it down first; nothing is modified).
 * PIXELICA_ERR_PLATFORM: SDL could not initialise the subsystem (SDL_GetError()
 * has the reason). SDL initialization failure leaves the adapter uninitialised;
 * invalid arguments leave its previous state untouched.
 */
PIXELICA_Status pixelica_input_sdl3_init(PIXELICA_InputSdl3 *adapter, PIXELICA_InputManager *manager,
                               uint8_t gamepad_count);

/*
 * Submits neutral (disconnected) snapshots for every source the adapter owns,
 * closes its gamepads, releases its SDL gamepad-subsystem reference and
 * returns the struct to the zeroed, uninitialised state. Safe on NULL and on an
 * adapter that was never initialised or is already shut down. The manager must
 * still be valid here. It does not call SDL_Quit and leaves hints, events and
 * other SDL subsystems alone.
 */
void pixelica_input_sdl3_shutdown(PIXELICA_InputSdl3 *adapter);

/*
 * Maps keyboard button slot `button` (0..63) to `code`. SDL_SCANCODE_UNKNOWN
 * unmaps the slot; any other value must be a scancode SDL can report, 1 ..
 * SDL_SCANCODE_COUNT-1. The same scancode may be mapped to several slots. Takes
 * effect on the next poll.
 * PIXELICA_ERR_INVALID_ARG (map unchanged): NULL or uninitialised adapter, button
 * out of range, or invalid scancode.
 */
PIXELICA_Status pixelica_input_sdl3_set_key(PIXELICA_InputSdl3 *adapter, uint8_t button,
                                  SDL_Scancode code);

/*
 * Samples SDL once and submits one snapshot to each owned source.
 *
 * It calls SDL_PumpEvents() and SDL_UpdateGamepads(). It never calls
 * SDL_PollEvent()/SDL_PeepEvents() and consumes no application event: window,
 * quit and gamepad events stay queued for the application, which should keep
 * draining its queue (SDL drops events once the queue is full).
 *
 * accept_input = false (window without focus, pause menu, ...): hot-plug
 * handling continues, but every owned source is submitted as neutral and
 * disconnected, so nothing stays held across the focus loss. Calling it with
 * true afterwards resumes normal sampling.
 *
 * Gamepad slots are stable: a slot keeps the SDL_Gamepad and instance id it
 * opened until that device disappears. Detected via SDL_GetGamepads(), a lost
 * device closes its slot and submits neutral. A new device takes the lowest free
 * slot, in SDL enumeration order. A slot freed in a poll is NOT refilled in
 * the same poll, so the manager sees at least one neutral snapshot between
 * the old and the new device (no inherited held state). A device that SDL
 * cannot open is skipped and does not fail the poll.
 * As with any snapshot source, call manager_tick between samples if the
 * intermediate neutral state must produce a release edge.
 *
 * SDL_GetGamepads() allocates its id list inside SDL; the adapter frees it
 * before returning. If that allocation fails, every owned source is
 * neutralised first and PIXELICA_ERR_NO_MEMORY is returned; opened gamepads are
 * kept and the next successful poll resumes.
 *
 * PIXELICA_ERR_INVALID_ARG: NULL or uninitialised adapter (nothing happens). Any
 * error from the manager's submit is returned after all sources were tried.
 * This function does not call pixelica_input_manager_tick().
 */
PIXELICA_Status pixelica_input_sdl3_poll(PIXELICA_InputSdl3 *adapter, bool accept_input);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_INPUT_SDL3_H */
