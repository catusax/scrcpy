#include "common.h"

#include <assert.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "controller.h"
#include "uhid/keyboard_uhid.h"
#include "util/thread.h"
#ifdef HAVE_USB
# include "usb/keyboard_aoa.h"
#endif

static void
on_ended(struct sc_controller *controller, bool error, void *userdata) {
    (void) controller;
    (void) error;
    (void) userdata;
    assert(!"No transport thread should be started by this queue test");
}

static void
assert_all_up(const uint8_t *data, size_t size) {
    static const uint8_t zero[8];
    assert(size == sizeof(zero));
    assert(!memcmp(data, zero, sizeof(zero)));
}

static void
test_uhid(void) {
    static const struct sc_controller_callbacks cbs = {.on_ended = on_ended};
    struct sc_controller controller;
    assert(sc_controller_init(&controller, SC_SOCKET_NONE, &cbs, NULL));
    struct sc_keyboard_uhid kb;
    assert(sc_keyboard_uhid_init(&kb, &controller));
    struct sc_key_event event = {.action = SC_ACTION_DOWN,
                                .scancode = SC_SCANCODE_A,
                                .mods_state = SC_MOD_LALT};
    kb.key_processor.ops->process_key(&kb.key_processor, &event,
                                      SC_SEQUENCE_INVALID);
    assert(kb.hid.keys[SC_SCANCODE_A]);
    // Saturate the real controller queue; normal reports still drop at limit.
    struct sc_control_msg msg = {.type = SC_CONTROL_MSG_TYPE_UHID_INPUT};
    msg.uhid_input.id = SC_HID_ID_KEYBOARD;
    msg.uhid_input.size = 8;
    msg.uhid_input.data[2] = SC_SCANCODE_A;
    unsigned accepted = 0;
    while (sc_controller_push_msg(&controller, &msg)) {
        ++accepted;
        assert(accepted < 1000);
    }
    size_t before = sc_vecdeque_size(&controller.queue);
    assert(before > 0 && accepted > 0);
    kb.key_processor.ops->release_all(&kb.key_processor);
    assert(sc_vecdeque_size(&controller.queue) == before + 1);
    assert(!kb.hid.keys[SC_SCANCODE_A]);
    struct sc_control_msg last;
    while (!sc_vecdeque_is_empty(&controller.queue)) {
        last = sc_vecdeque_pop(&controller.queue);
    }
    assert(last.type == SC_CONTROL_MSG_TYPE_UHID_INPUT);
    assert_all_up(last.uhid_input.data, last.uhid_input.size);
    event.scancode = SC_SCANCODE_B;
    event.mods_state = 0;
    kb.key_processor.ops->process_key(&kb.key_processor, &event,
                                      SC_SEQUENCE_INVALID);
    last = sc_vecdeque_pop(&controller.queue);
    assert(last.uhid_input.data[0] == 0);
    assert(last.uhid_input.data[2] == SC_SCANCODE_B);
    assert(last.uhid_input.data[3] == 0);
    sc_controller_destroy(&controller);
}

#ifdef HAVE_USB
static void
test_aoa(void) {
    struct sc_aoa aoa;
    assert(sc_aoa_init(&aoa, NULL, NULL));
    struct sc_keyboard_aoa kb;
    assert(sc_keyboard_aoa_init(&kb, &aoa));
    struct sc_key_event event = {.action = SC_ACTION_DOWN,
                                .scancode = SC_SCANCODE_A,
                                .mods_state = SC_MOD_LALT};
    kb.key_processor.ops->process_key(&kb.key_processor, &event,
                                      SC_SEQUENCE_INVALID);
    assert(kb.hid.keys[SC_SCANCODE_A]);
    struct sc_hid_input input = {.hid_id = SC_HID_ID_KEYBOARD, .size = 8};
    input.data[2] = SC_SCANCODE_A;
    unsigned accepted = 0;
    while (sc_aoa_push_input(&aoa, &input)) {
        ++accepted;
        assert(accepted < 1000);
    }
    size_t before = sc_vecdeque_size(&aoa.queue);
    assert(before > 0 && accepted > 0);
    kb.key_processor.ops->release_all(&kb.key_processor);
    assert(sc_vecdeque_size(&aoa.queue) == before + 1);
    assert(!kb.hid.keys[SC_SCANCODE_A]);
    struct sc_aoa_event last;
    while (!sc_vecdeque_is_empty(&aoa.queue)) {
        last = sc_vecdeque_pop(&aoa.queue);
    }
    assert(last.type == SC_AOA_EVENT_TYPE_INPUT);
    assert(last.input.ack_to_wait == SC_SEQUENCE_INVALID);
    assert_all_up(last.input.hid.data, last.input.hid.size);
    event.scancode = SC_SCANCODE_B;
    event.mods_state = 0;
    kb.key_processor.ops->process_key(&kb.key_processor, &event,
                                      SC_SEQUENCE_INVALID);
    last = sc_vecdeque_pop(&aoa.queue);
    assert(last.input.hid.data[0] == 0);
    assert(last.input.hid.data[2] == SC_SCANCODE_B);
    assert(last.input.hid.data[3] == 0);
    sc_aoa_destroy(&aoa);
}
#endif

int
main(void) {
    assert(SDL_Init(0));
    test_uhid();
#ifdef HAVE_USB
    test_aoa();
#endif
    SDL_Quit();
    return 0;
}
