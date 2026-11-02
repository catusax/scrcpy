#include "immersive.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "util/log.h"

static SDL_Keymod
sc_immersive_modifier(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
        case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
        case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
        case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
        // Normalize MODE too, or input_manager would reintroduce Right-Alt.
        case SDL_SCANCODE_RALT: return SDL_KMOD_RALT | SDL_KMOD_MODE;
        case SDL_SCANCODE_LGUI: return SDL_KMOD_LGUI;
        case SDL_SCANCODE_RGUI: return SDL_KMOD_RGUI;
        default: return SDL_KMOD_NONE;
    }
}

static void
sc_immersive_boundary(struct sc_immersive *imm) {
    for (int i = 0; i < SDL_SCANCODE_COUNT; ++i) {
        // SDL_GetKeyboardState() may include events later in the same pump.
        // Only transitions processed before this boundary are already held.
        imm->quarantine[i] |= imm->held[i];
    }
    if (!imm->disconnected) {
        // Both HID backends clear cached keys and submit a real all-up report.
        imm->kp->ops->release_all(imm->kp);
    }
}

void
sc_immersive_release(struct sc_immersive *imm) {
    if (!imm->enabled) {
        return;
    }
    imm->initial_capture_pending = false;
    sc_immersive_boundary(imm);
    // Queue guest all-up before OS ungrab, then restore owned text input below.
    // OS cleanup must not depend on transport delivery or implicit focus ungrab.
    if (!SDL_SetWindowKeyboardGrab(imm->window, false)) {
        LOGW("Could not release keyboard capture: %s", SDL_GetError());
    }
    if (imm->relative_mouse
            && !SDL_SetWindowRelativeMouseMode(imm->window, false)) {
        LOGW("Could not release relative mouse: %s", SDL_GetError());
    }
    if (imm->restore_text_input) {
        // Consume ownership even on failure: later releases must not restore a
        // stale snapshot over another owner's text-input state.
        imm->restore_text_input = false;
        if (!SDL_StartTextInput(imm->window)) {
            LOGW("Could not restore text input after capture: %s",
                 SDL_GetError());
        }
    }
    imm->active = false;
    SDL_SetWindowTitle(imm->window, imm->released_title);
}

static void
sc_immersive_acquire(struct sc_immersive *imm) {
    if (imm->active) {
        return;
    }
    imm->initial_capture_pending = false;
    if (imm->suspended || imm->disconnected
            || !(SDL_GetWindowFlags(imm->window) & SDL_WINDOW_INPUT_FOCUS)
            || (SDL_GetWindowFlags(imm->window) & SDL_WINDOW_HIDDEN)) {
        return;
    }
    // Treat acquisition as a transaction: failed grabs/text-input suspension
    // use the same release path to unwind OS state before any later recapture.
    sc_immersive_boundary(imm);
    if (!SDL_SetWindowKeyboardGrab(imm->window, true)
            || !SDL_GetWindowKeyboardGrab(imm->window)) {
        LOGW("Could not capture keyboard: %s", SDL_GetError());
        sc_immersive_release(imm);
        return;
    }
    if (imm->relative_mouse
            && !SDL_SetWindowRelativeMouseMode(imm->window, true)) {
        LOGW("Could not capture relative mouse: %s", SDL_GetError());
        sc_immersive_release(imm);
        return;
    }
    // Snapshot each acquisition, not initialization: another owner may change
    // text input while capture is released. HID uses physical keys, not text.
    imm->restore_text_input = SDL_TextInputActive(imm->window);
    if (imm->restore_text_input && !SDL_StopTextInput(imm->window)) {
        LOGW("Could not pause text input for capture: %s", SDL_GetError());
        sc_immersive_release(imm);
        return;
    }
    imm->active = true;
    SDL_SetWindowTitle(imm->window, imm->active_title);
    LOGI("Immersive keyboard capture requested (desktop policy may limit it)");
}

bool
sc_immersive_init(struct sc_immersive *imm, SDL_Window *window,
                  struct sc_key_processor *kp, bool enabled, uint8_t host_key,
                  bool relative_mouse) {
    memset(imm, 0, sizeof(*imm));
    if (!enabled) {
        return true;
    }
    assert(kp && kp->hid && kp->ops->release_all);
    const char *name;
    switch (host_key) {
        case SC_SHORTCUT_MOD_LCTRL:
            imm->host_scancode = SDL_SCANCODE_LCTRL; name = "LCtrl"; break;
        case SC_SHORTCUT_MOD_RCTRL:
            imm->host_scancode = SDL_SCANCODE_RCTRL; name = "RCtrl"; break;
        case SC_SHORTCUT_MOD_LALT:
            imm->host_scancode = SDL_SCANCODE_LALT; name = "LAlt"; break;
        case SC_SHORTCUT_MOD_RALT:
            imm->host_scancode = SDL_SCANCODE_RALT; name = "RAlt"; break;
        case SC_SHORTCUT_MOD_LSUPER:
            imm->host_scancode = SDL_SCANCODE_LGUI; name = "LSuper"; break;
        case SC_SHORTCUT_MOD_RSUPER:
            imm->host_scancode = SDL_SCANCODE_RGUI; name = "RSuper"; break;
        default:
            assert(!"Invalid immersive Host key");
            return false;
    }
    const char *title = SDL_GetWindowTitle(window);
    if (asprintf(&imm->active_title, "%s [immersive: active; %s releases]",
                 title, name) == -1) {
        LOG_OOM();
        return false;
    }
    if (asprintf(&imm->released_title, "%s [immersive: released; Host=%s]",
                 title, name) == -1) {
        LOG_OOM();
        free(imm->active_title);
        return false;
    }
    // Explicit opt-in overrides an environment hint that would let SDL escape
    // fullscreen Alt+Tab before it reaches the guest.
    if (!SDL_SetHintWithPriority(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0",
                                 SDL_HINT_OVERRIDE)) {
        LOGE("Could not configure immersive Alt+Tab handling");
        free(imm->active_title);
        free(imm->released_title);
        return false;
    }
    imm->window = window;
    imm->kp = kp;
    imm->enabled = true;
    imm->initial_capture_pending = true;
    imm->relative_mouse = relative_mouse;
    imm->host_mod = sc_immersive_modifier(imm->host_scancode);
    int count;
    const bool *state = SDL_GetKeyboardState(&count);
    for (int i = 0; i < SDL_SCANCODE_COUNT && i < count; ++i) {
        imm->quarantine[i] = state[i];
    }
    SDL_SetWindowTitle(window, imm->released_title);
    return true;
}

void
sc_immersive_destroy(struct sc_immersive *imm) {
    if (!imm->enabled) {
        return;
    }
    // The caller may already have destroyed the guest transport.
    imm->disconnected = true;
    sc_immersive_release(imm);
    free(imm->active_title);
    free(imm->released_title);
    imm->enabled = false;
}

void
sc_immersive_on_shown(struct sc_immersive *imm) {
    if (imm->enabled && imm->initial_capture_pending
            && (SDL_GetWindowFlags(imm->window) & SDL_WINDOW_INPUT_FOCUS)) {
        sc_immersive_acquire(imm);
    }
}

void
sc_immersive_set_suspended(struct sc_immersive *imm, bool suspended) {
    if (imm->enabled) {
        if (suspended) {
            sc_immersive_release(imm);
        }
        imm->suspended = suspended;
    }
}

void
sc_immersive_disconnect(struct sc_immersive *imm) {
    if (imm->enabled && !imm->disconnected) {
        // Best-effort guest cleanup before recording disconnection. No delivery
        // is promised if the transport has already failed.
        sc_immersive_release(imm);
        imm->disconnected = true;
    }
}

bool
sc_immersive_handle_event(struct sc_immersive *imm, SDL_Event *event) {
    if (!imm->enabled) {
        return false;
    }
    switch (event->type) {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            sc_immersive_on_shown(imm);
            return false;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_QUIT:
            sc_immersive_release(imm);
            return false;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            // Host is the sole unified recapture action in relative mode.
            // Never forward absolute host motion as relative guest motion.
            return imm->relative_mouse
                && (!imm->active
                    || !SDL_GetWindowRelativeMouseMode(imm->window));
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
            return imm->relative_mouse;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            break;
        default:
            return false;
    }
    SDL_Scancode scancode = event->key.scancode;
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
        return true;
    }
    bool down = event->type == SDL_EVENT_KEY_DOWN;
    // Quarantine follows SDL's logical keyboard state, including synthesized
    // focus-loss releases. This avoids discarding the first fresh press after
    // refocus; only explicit focused transitions quarantine still-held keys.
    bool was_down = imm->held[scancode];
    imm->held[scancode] = down;
    if (scancode == imm->host_scancode) {
        if (down && !event->key.repeat && !was_down) {
            if (imm->active) {
                sc_immersive_release(imm);
            } else {
                sc_immersive_acquire(imm);
            }
        }
        if (!down) {
            imm->quarantine[scancode] = false;
        }
        return true;
    }
    bool quarantined = imm->quarantine[scancode];
    if (!down) {
        imm->quarantine[scancode] = false;
    }
    if (quarantined || imm->disconnected || imm->suspended) {
        return true;
    }
    SDL_Keymod mask = imm->host_mod;
    SDL_Keymod held_mods = SDL_KMOD_NONE;
    for (SDL_Scancode i = SDL_SCANCODE_LCTRL; i <= SDL_SCANCODE_RGUI; ++i) {
        SDL_Keymod modifier = sc_immersive_modifier(i);
        if (imm->active) {
            // SDL's modifier snapshot may already reflect a later queued
            // release (notably on X11). HID needs the state at this physical
            // transition, including standalone modifier down/up events.
            mask |= modifier;
            if (imm->held[i] && !imm->quarantine[i]
                    && i != imm->host_scancode) {
                held_mods |= modifier;
            }
        } else if (imm->quarantine[i]) {
            mask |= modifier;
        }
    }
    event->key.mod = (event->key.mod & ~mask) | held_mods;
    return false;
}
