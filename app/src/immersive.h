#ifndef SC_IMMERSIVE_H
#define SC_IMMERSIVE_H

#include "common.h"

#include <SDL3/SDL.h>

#include "trait/key_processor.h"

// Main-thread-only window capture state. SDL is the only platform boundary.
struct sc_immersive {
    SDL_Window *window;
    struct sc_key_processor *kp;
    SDL_Scancode host_scancode;
    SDL_Keymod host_mod;
    bool enabled;
    bool active;
    bool initial_capture_pending;
    bool suspended;
    bool disconnected;
    bool relative_mouse;
    // This capture paused previously active text input; restore it once.
    bool restore_text_input;
    // One processed-event timeline, not SDL's potentially newer pump snapshot.
    // Boundaries quarantine held keys until their matching release arrives.
    bool held[SDL_SCANCODE_COUNT];
    bool quarantine[SDL_SCANCODE_COUNT];
    char *active_title;
    char *released_title;
};

bool
sc_immersive_init(struct sc_immersive *imm, SDL_Window *window,
                  struct sc_key_processor *kp, bool enabled, uint8_t host_key,
                  bool relative_mouse);

// Release host capture immediately, regardless of guest transport health.
// Guest reports are queued only while connected; shutdown does not drain them.
void
sc_immersive_release(struct sc_immersive *imm);

void
sc_immersive_destroy(struct sc_immersive *imm);

void
sc_immersive_on_shown(struct sc_immersive *imm);

void
sc_immersive_set_suspended(struct sc_immersive *imm, bool suspended);

void
sc_immersive_disconnect(struct sc_immersive *imm);

// Consumes Host/quarantined events; active modifiers follow processed key order,
// not SDL's potentially newer snapshot. Host is always masked from forwarded keys.
bool
sc_immersive_handle_event(struct sc_immersive *imm, SDL_Event *event);

#endif
