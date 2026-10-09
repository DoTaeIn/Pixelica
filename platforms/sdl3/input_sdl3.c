/* SDL3 adapter for the optional input manager. See input_sdl3.h for the contract. */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <string.h>

#include "input_sdl3.h"

#define PIXELICA_SDL3_KEYBOARD_SOURCE 0u

SDL_COMPILE_TIME_ASSERT(pixelica_input_sdl3_pads_fit_manager,
                        PIXELICA_INPUT_SDL3_MAX_GAMEPADS + 1u <= PIXELICA_INPUT_MAX_DEVICES);
SDL_COMPILE_TIME_ASSERT(pixelica_input_sdl3_buttons_fit_mask, SDL_GAMEPAD_BUTTON_COUNT <= 64);

static void set_default_keys(SDL_Scancode keys[PIXELICA_INPUT_MAX_BUTTONS])
{
    for (size_t i = 0u; i < PIXELICA_INPUT_MAX_BUTTONS; ++i) {
        keys[i] = SDL_SCANCODE_UNKNOWN;
    }
    keys[0] = SDL_SCANCODE_LEFT;
    keys[1] = SDL_SCANCODE_RIGHT;
    keys[2] = SDL_SCANCODE_UP;
    keys[3] = SDL_SCANCODE_DOWN;
    keys[4] = SDL_SCANCODE_SPACE;
    keys[5] = SDL_SCANCODE_RETURN;
    keys[6] = SDL_SCANCODE_ESCAPE;
    keys[7] = SDL_SCANCODE_A;
    keys[8] = SDL_SCANCODE_D;
    keys[9] = SDL_SCANCODE_W;
    keys[10] = SDL_SCANCODE_S;
    keys[11] = SDL_SCANCODE_KP_ENTER;
}

/* Neutral = disconnected: the manager ignores the payload of such a snapshot. */
static PIXELICA_Status submit_neutral(const PIXELICA_InputSdl3 *adapter, unsigned source)
{
    PIXELICA_InputDeviceState state;
    memset(&state, 0, sizeof(state));
    return pixelica_input_manager_submit(adapter->manager, (uint8_t)source, &state);
}

/* Tries every owned source; returns the first error. */
static PIXELICA_Status neutralize_all(const PIXELICA_InputSdl3 *adapter)
{
    PIXELICA_Status result = PIXELICA_OK;
    for (unsigned source = 0u; source <= adapter->pad_count; ++source) {
        PIXELICA_Status status = submit_neutral(adapter, source);
        if (result == PIXELICA_OK) {
            result = status;
        }
    }
    return result;
}

/* Sticks: negative/32768, positive/32767. */
static float normalize_axis(Sint16 value)
{
    return value < 0 ? (float)value / 32768.0f : (float)value / 32767.0f;
}

static void read_gamepad(SDL_Gamepad *pad, PIXELICA_InputDeviceState *state)
{
    memset(state, 0, sizeof(*state));
    state->connected = true;
    for (int button = 0; button < (int)SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)button)) {
            state->buttons |= UINT64_C(1) << button;
        }
    }
    for (int axis = 0; axis < (int)SDL_GAMEPAD_AXIS_COUNT && axis < (int)PIXELICA_INPUT_MAX_AXES; ++axis) {
        float value = normalize_axis(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis));
        if ((axis == (int)SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == (int)SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
            value < 0.0f) {
            value = 0.0f;
        }
        state->axes[axis] = value;
    }
}

static void read_keyboard(const PIXELICA_InputSdl3 *adapter, PIXELICA_InputDeviceState *state)
{
    int key_count = 0;
    const bool *keys = SDL_GetKeyboardState(&key_count);

    memset(state, 0, sizeof(*state));
    state->connected = true;
    if (keys == NULL) {
        return;
    }
    for (size_t button = 0u; button < PIXELICA_INPUT_MAX_BUTTONS; ++button) {
        int code = (int)adapter->keys[button];
        if (code > 0 && code < key_count && keys[code]) {
            state->buttons |= UINT64_C(1) << button;
        }
    }
}

static bool id_listed(const SDL_JoystickID *ids, int count, SDL_JoystickID id)
{
    for (int i = 0; i < count; ++i) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

static bool id_owned(const PIXELICA_InputSdl3 *adapter, SDL_JoystickID id)
{
    for (unsigned slot = 0u; slot < adapter->pad_count; ++slot) {
        if (adapter->pads[slot] != NULL && adapter->pad_ids[slot] == id) {
            return true;
        }
    }
    return false;
}

static void release_slot(PIXELICA_InputSdl3 *adapter, unsigned slot)
{
    if (adapter->pads[slot] != NULL) {
        SDL_CloseGamepad(adapter->pads[slot]);
    }
    adapter->pads[slot] = NULL;
    adapter->pad_ids[slot] = 0;
}

/* Drops slots whose device is gone, then fills free slots (except those freed
 * just now) with unowned gamepads in enumeration order. *opened tells whether a
 * new gamepad was opened. */
static PIXELICA_Status manage_gamepads(PIXELICA_InputSdl3 *adapter, bool *opened)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count); /* allocated by SDL */
    bool freed_now[PIXELICA_INPUT_SDL3_MAX_GAMEPADS] = {false};
    int next = 0;

    if (ids == NULL) {
        return PIXELICA_ERR_NO_MEMORY;
    }
    for (unsigned slot = 0u; slot < adapter->pad_count; ++slot) {
        if (adapter->pads[slot] != NULL &&
            (!id_listed(ids, count, adapter->pad_ids[slot]) || !SDL_GamepadConnected(adapter->pads[slot]))) {
            release_slot(adapter, slot);
            freed_now[slot] = true;
        }
    }
    for (unsigned slot = 0u; slot < adapter->pad_count; ++slot) {
        if (adapter->pads[slot] != NULL || freed_now[slot]) {
            continue;
        }
        while (next < count && adapter->pads[slot] == NULL) {
            SDL_JoystickID id = ids[next++];
            if (!id_owned(adapter, id)) {
                SDL_Gamepad *pad = SDL_OpenGamepad(id);
                if (pad != NULL) {
                    adapter->pads[slot] = pad;
                    adapter->pad_ids[slot] = id;
                    *opened = true;
                }
            }
        }
    }
    SDL_free(ids);
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_sdl3_init(PIXELICA_InputSdl3 *adapter, PIXELICA_InputManager *manager,
                               uint8_t gamepad_count)
{
    if (adapter == NULL || manager == NULL || gamepad_count > PIXELICA_INPUT_SDL3_MAX_GAMEPADS ||
        adapter->initialized) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    SDL_SetMainReady();
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        return PIXELICA_ERR_PLATFORM;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->manager = manager;
    adapter->pad_count = gamepad_count;
    set_default_keys(adapter->keys);
    adapter->initialized = true;

    PIXELICA_Status status = neutralize_all(adapter);
    if (status != PIXELICA_OK) {
        pixelica_input_sdl3_shutdown(adapter);
        return status;
    }
    return PIXELICA_OK;
}

void pixelica_input_sdl3_shutdown(PIXELICA_InputSdl3 *adapter)
{
    if (adapter == NULL || !adapter->initialized) {
        return;
    }
    (void)neutralize_all(adapter);
    for (unsigned slot = 0u; slot < PIXELICA_INPUT_SDL3_MAX_GAMEPADS; ++slot) {
        release_slot(adapter, slot);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    memset(adapter, 0, sizeof(*adapter));
}

PIXELICA_Status pixelica_input_sdl3_set_key(PIXELICA_InputSdl3 *adapter, uint8_t button, SDL_Scancode code)
{
    if (adapter == NULL || !adapter->initialized || button >= PIXELICA_INPUT_MAX_BUTTONS ||
        (int)code < 0 || (int)code >= (int)SDL_SCANCODE_COUNT) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    adapter->keys[button] = code;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_input_sdl3_poll(PIXELICA_InputSdl3 *adapter, bool accept_input)
{
    PIXELICA_Status result = PIXELICA_OK;
    PIXELICA_InputDeviceState state;

    if (adapter == NULL || !adapter->initialized) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    SDL_PumpEvents();
    if (adapter->pad_count > 0u) {
        bool opened = false;
        SDL_UpdateGamepads();
        PIXELICA_Status managed = manage_gamepads(adapter, &opened);
        if (managed != PIXELICA_OK) {
            (void)neutralize_all(adapter);
            return managed;
        }
        if (opened) {
            SDL_UpdateGamepads(); /* refresh after the new mapping is initialized */
        }
    }
    if (!accept_input) {
        return neutralize_all(adapter);
    }

    read_keyboard(adapter, &state);
    result = pixelica_input_manager_submit(adapter->manager, PIXELICA_SDL3_KEYBOARD_SOURCE, &state);
    for (unsigned slot = 0u; slot < adapter->pad_count; ++slot) {
        PIXELICA_Status status;
        if (adapter->pads[slot] != NULL) {
            read_gamepad(adapter->pads[slot], &state);
            status = pixelica_input_manager_submit(adapter->manager, (uint8_t)(slot + 1u), &state);
        } else {
            status = submit_neutral(adapter, slot + 1u);
        }
        if (result == PIXELICA_OK) {
            result = status;
        }
    }
    return result;
}
