#include "common.h"

#include <assert.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "hid/hid_keyboard.h"
#include "immersive.h"
#include "options.h"
#include "shortcut_mod.h"

static void
release_all(struct sc_key_processor *kp) {
    (void) kp;
}

static SDL_Event
key(SDL_Scancode scancode, bool down, SDL_Keymod mod, bool repeat) {
    SDL_Event event = {0};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.mod = mod;
    event.key.repeat = repeat;
    return event;
}

static void
assert_report(struct sc_immersive *imm, struct sc_hid_keyboard *hid,
              SDL_Scancode scancode, bool down, SDL_Keymod snapshot,
              uint8_t modifiers, uint8_t keycode) {
    SDL_Event event = key(scancode, down, snapshot, false);
    assert(!sc_immersive_handle_event(imm, &event));
    struct sc_key_event evt = {
        .action = down ? SC_ACTION_DOWN : SC_ACTION_UP,
        .scancode = sc_scancode_from_sdl(event.key.scancode),
        .mods_state = sc_mods_state_from_sdl(
            sc_sdl_mod_normalize(event.key.mod)),
    };
    struct sc_hid_input report;
    assert(sc_hid_keyboard_generate_input_from_key(hid, &report, &evt));
    const uint8_t expected[] = {modifiers, 0, keycode, 0, 0, 0, 0, 0};
    assert(report.size == sizeof(expected));
    assert(!memcmp(report.data, expected, sizeof(expected)));
}

static void
test_physical_modifiers(SDL_Window *window, struct sc_key_processor *kp) {
    struct sc_immersive imm;
    assert(sc_immersive_init(&imm, window, kp, true,
                             SC_SHORTCUT_MOD_RCTRL, false));
    imm.active = true;
    struct sc_hid_keyboard hid;
    sc_hid_keyboard_init(&hid);

    // X11 may deliver a queued down/up pair with mod=0 on both events.
    // A standalone modifier must still produce a real down then all-up.
    assert_report(&imm, &hid, SDL_SCANCODE_LGUI, true, 0, 0x08, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_LGUI, false, 0, 0, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_RGUI, true, 0, 0x80, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_RGUI, false, 0, 0, 0);

    // Follow physical ordering for combos too, despite missing/stale masks.
    assert_report(&imm, &hid, SDL_SCANCODE_LGUI, true, 0, 0x08, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_RGUI, true, 0, 0x88, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_A, true, 0, 0x88, 0x04);
    assert_report(&imm, &hid, SDL_SCANCODE_LGUI, false, SDL_KMOD_GUI,
                  0x80, 0x04);
    assert_report(&imm, &hid, SDL_SCANCODE_A, false, 0, 0x80, 0);
    assert_report(&imm, &hid, SDL_SCANCODE_RGUI, false, SDL_KMOD_GUI, 0, 0);

    // Preserve lock snapshots, without resurrecting momentary modifiers.
    SDL_Event event = key(SDL_SCANCODE_B, true,
                         SDL_KMOD_CAPS | SDL_KMOD_NUM | SDL_KMOD_GUI, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(event.key.mod == (SDL_KMOD_CAPS | SDL_KMOD_NUM));

    assert_report(&imm, &hid, SDL_SCANCODE_LGUI, true, 0, 0x08, 0);
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(!sc_immersive_handle_event(&imm, &event));
    event = key(SDL_SCANCODE_LGUI, false, 0, false);
    assert(sc_immersive_handle_event(&imm, &event));
    imm.active = true; // runtime tests cover actual reacquisition
    assert_report(&imm, &hid, SDL_SCANCODE_A, true, SDL_KMOD_GUI, 0, 0x04);
    assert_report(&imm, &hid, SDL_SCANCODE_A, false, 0, 0, 0);
    sc_immersive_destroy(&imm);

    // Either Super remains exclusively reserved when explicitly selected Host.
    const uint8_t hosts[] = {SC_SHORTCUT_MOD_LSUPER, SC_SHORTCUT_MOD_RSUPER};
    for (unsigned i = 0; i < ARRAY_LEN(hosts); ++i) {
        assert(sc_immersive_init(&imm, window, kp, true, hosts[i], false));
        imm.active = true;
        SDL_Scancode host = i ? SDL_SCANCODE_RGUI : SDL_SCANCODE_LGUI;
        event = key(host, true, 0, false);
        assert(sc_immersive_handle_event(&imm, &event));
        assert(!imm.active);
        // Even a stale both-GUI snapshot cannot leak the reserved side, while
        // the opposite physical Super and an ordinary key still reach HID.
        imm.active = true;
        SDL_Scancode other = i ? SDL_SCANCODE_LGUI : SDL_SCANCODE_RGUI;
        uint8_t other_mod = i ? 0x08 : 0x80;
        assert_report(&imm, &hid, other, true, SDL_KMOD_GUI, other_mod, 0);
        assert_report(&imm, &hid, SDL_SCANCODE_A, true, SDL_KMOD_GUI,
                      other_mod, 0x04);
        event = key(host, false, SDL_KMOD_GUI, false);
        assert(sc_immersive_handle_event(&imm, &event));
        assert_report(&imm, &hid, other, false, SDL_KMOD_GUI, 0, 0x04);
        assert_report(&imm, &hid, SDL_SCANCODE_A, false, 0, 0, 0);
        sc_immersive_destroy(&imm);
    }
}

static void
test_text_input_restoration(SDL_Window *window, struct sc_key_processor *kp) {
    struct sc_immersive imm;
    const SDL_EventType releases[] = {
        SDL_EVENT_WINDOW_FOCUS_LOST,
        SDL_EVENT_WINDOW_HIDDEN,
        SDL_EVENT_WINDOW_MINIMIZED,
        SDL_EVENT_WINDOW_CLOSE_REQUESTED,
        SDL_EVENT_QUIT,
    };
    for (unsigned i = 0; i < ARRAY_LEN(releases); ++i) {
        assert(sc_immersive_init(&imm, window, kp, true,
                                 SC_SHORTCUT_MOD_RCTRL, false));
        // As in the key tests, seed captured state for the dummy video driver.
        // X11 runtime tests exercise the real acquisition and its snapshot.
        assert(SDL_StartTextInput(window));
        imm.restore_text_input = SDL_TextInputActive(window);
        assert(SDL_StopTextInput(window));
        imm.active = true;
        SDL_Event event = {.type = releases[i]};
        assert(!sc_immersive_handle_event(&imm, &event));
        assert(!imm.active && !imm.restore_text_input);
        assert(SDL_TextInputActive(window));

        // A later owner's disabled state must survive every repeated release,
        // including disconnect and destroy after an earlier successful release.
        assert(SDL_StopTextInput(window));
        sc_immersive_release(&imm);
        sc_immersive_disconnect(&imm);
        sc_immersive_destroy(&imm);
        assert(!SDL_TextInputActive(window));
    }

    // A capture which did not pause text input must never enable it on release.
    assert(sc_immersive_init(&imm, window, kp, true,
                             SC_SHORTCUT_MOD_RCTRL, false));
    imm.active = true;
    sc_immersive_set_suspended(&imm, true);
    assert(!SDL_TextInputActive(window));
    sc_immersive_destroy(&imm);

    // Destroy must restore an owned state even after the transport is gone.
    assert(sc_immersive_init(&imm, window, kp, true,
                             SC_SHORTCUT_MOD_RCTRL, false));
    imm.restore_text_input = true;
    imm.active = true;
    sc_immersive_destroy(&imm);
    assert(SDL_TextInputActive(window));
    assert(SDL_StopTextInput(window));
}

static void
test_host_quarantine(SDL_Window *window, struct sc_key_processor *kp) {
    struct sc_immersive imm;
    assert(sc_immersive_init(&imm, window, kp, true,
                             SC_SHORTCUT_MOD_RCTRL, true));

    // Seed an already-captured state, independent of dummy driver's grab/focus
    // support. Actual acquire and X11 delivery are exercised by runtime smoke.
    imm.active = true;
    SDL_Event event = key(SDL_SCANCODE_A, true, SDL_KMOD_NONE, false);
    assert(!sc_immersive_handle_event(&imm, &event));

    event = key(SDL_SCANCODE_LALT, true, SDL_KMOD_LALT, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    event = key(SDL_SCANCODE_RCTRL, true,
                SDL_KMOD_RCTRL | SDL_KMOD_LALT, false);
    assert(sc_immersive_handle_event(&imm, &event));
    assert(!imm.active);

    // Repeats and duplicate downs are reserved like the initial Host press.
    event = key(SDL_SCANCODE_RCTRL, true, SDL_KMOD_RCTRL, true);
    assert(sc_immersive_handle_event(&imm, &event));
    event.key.repeat = false;
    assert(sc_immersive_handle_event(&imm, &event));

    // A held key cannot reappear through repeat, nor leak a stale key-up.
    event = key(SDL_SCANCODE_A, true, SDL_KMOD_RCTRL, true);
    assert(sc_immersive_handle_event(&imm, &event));
    event = key(SDL_SCANCODE_A, false, SDL_KMOD_RCTRL, false);
    assert(sc_immersive_handle_event(&imm, &event));
    event = key(SDL_SCANCODE_A, true, SDL_KMOD_RCTRL | SDL_KMOD_LALT, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(!(event.key.mod & (SDL_KMOD_RCTRL | SDL_KMOD_LALT)));

    event = key(SDL_SCANCODE_LALT, false, SDL_KMOD_RCTRL, false);
    assert(sc_immersive_handle_event(&imm, &event));
    event = key(SDL_SCANCODE_B, true, SDL_KMOD_RCTRL | SDL_KMOD_LALT, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(event.key.mod == SDL_KMOD_LALT); // Host always masked
    event = key(SDL_SCANCODE_RCTRL, false, SDL_KMOD_NONE, false);
    assert(sc_immersive_handle_event(&imm, &event));

    event.type = SDL_EVENT_MOUSE_MOTION;
    assert(sc_immersive_handle_event(&imm, &event));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    assert(sc_immersive_handle_event(&imm, &event));
    assert(!imm.active);

    sc_immersive_set_suspended(&imm, true);
    event = key(SDL_SCANCODE_C, true, SDL_KMOD_NONE, false);
    assert(sc_immersive_handle_event(&imm, &event));
    sc_immersive_set_suspended(&imm, false);
    assert(!imm.active); // resuming is never an automatic recapture

    imm.active = true;
    event = key(SDL_SCANCODE_E, true, SDL_KMOD_NONE, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(!imm.active);
    event = key(SDL_SCANCODE_E, false, SDL_KMOD_NONE, false);
    assert(sc_immersive_handle_event(&imm, &event)); // SDL synthetic release
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(!imm.active && !imm.initial_capture_pending);
    event = key(SDL_SCANCODE_E, true, SDL_KMOD_NONE, false);
    assert(!sc_immersive_handle_event(&imm, &event)); // first fresh press
    sc_immersive_disconnect(&imm);
    sc_immersive_release(&imm);
    event = key(SDL_SCANCODE_D, true, SDL_KMOD_NONE, false);
    assert(sc_immersive_handle_event(&imm, &event));
    sc_immersive_destroy(&imm);
}

static void
test_altgr_host(SDL_Window *window, struct sc_key_processor *kp) {
    struct sc_immersive imm;
    // Right-Alt can arrive as MODE/AltGr; downstream normalization must not
    // restore the reserved Host modifier after the early filter masks it.
    assert(sc_immersive_init(&imm, window, kp, true,
                             SC_SHORTCUT_MOD_RALT, false));
    SDL_Event event = key(SDL_SCANCODE_B, true,
                          SDL_KMOD_RALT | SDL_KMOD_MODE, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(!(sc_sdl_mod_normalize(event.key.mod)
             & (SDL_KMOD_RALT | SDL_KMOD_MODE)));
    sc_immersive_destroy(&imm);
}

static void
test_disabled(SDL_Window *window) {
    struct sc_immersive imm;

    // Opt-out preserves incoming key events and does not take text ownership.
    assert(SDL_StartTextInput(window));
    assert(sc_immersive_init(&imm, window, NULL, false, 0, false));
    SDL_Event event = key(SDL_SCANCODE_RCTRL, true, SDL_KMOD_RCTRL, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(event.key.mod == SDL_KMOD_RCTRL);
    event = key(SDL_SCANCODE_LGUI, true, SDL_KMOD_NONE, false);
    assert(!sc_immersive_handle_event(&imm, &event));
    assert(event.key.mod == SDL_KMOD_NONE); // no opt-in, no reconstruction
    sc_immersive_release(&imm);
    sc_immersive_destroy(&imm);
    assert(SDL_TextInputActive(window));
}

int
main(void) {
    assert(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"));
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window = SDL_CreateWindow("custom title", 64, 64, 0);
    assert(window);
    static const struct sc_key_processor_ops ops = {.release_all = release_all};
    struct sc_key_processor kp = {.hid = true, .ops = &ops};
    test_host_quarantine(window, &kp);
    test_physical_modifiers(window, &kp);
    test_text_input_restoration(window, &kp);
    test_altgr_host(window, &kp);
    test_disabled(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
