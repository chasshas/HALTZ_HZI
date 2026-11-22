/*
 * HALTZ I/O: Bluepad32 platform. See haltz_platform.h.
 *
 * Everything in the uni_platform callbacks runs on the BTstack thread (core 0). BTstack and
 * Bluepad32 aren't thread safe, so other tasks only use the *_safe() calls, which BTstack runs
 * on its own thread.
 */
#include "haltz_platform.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "bt/uni_bt.h"
#include "controller/uni_controller.h"
#include "platform/uni_platform.h"
#include "uni_hid_device.h"
#include "uni_log.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Must use BLUEPAD32_PLATFORM_CUSTOM"
#endif

#define QUEUE_LEN 32

_Static_assert(UNI_KEYBOARD_PRESSED_KEYS_MAX == HALTZ_IO_KEYS_MAX, "key list sizes differ");

static QueueHandle_t s_queue;
static volatile uint32_t s_dropped;
static volatile bool s_scanning;

/* Written on the BTstack thread only */
static volatile uint8_t s_ready;                 /* bit n = slot n is ready */
static uint8_t s_battery[HALTZ_IO_MAX_DEVICES]; /* last level reported */

static void post(const bt_event_t *ev) {
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        s_dropped++;
    }
}

static int slot_of(uni_hid_device_t *d) {
    int32_t idx = uni_hid_device_get_idx_for_instance(d);
    return (idx >= 0 && idx < HALTZ_IO_MAX_DEVICES) ? (int)idx : -1;
}

static uint8_t kind_of(uni_hid_device_t *d) {
    if (uni_hid_device_is_keyboard(d)) {
        return HALTZ_IO_DEV_KEYBOARD;
    }
    if (uni_hid_device_is_mouse(d)) {
        return HALTZ_IO_DEV_MOUSE;
    }
    if (uni_hid_device_is_gamepad(d)) {
        return HALTZ_IO_DEV_GAMEPAD;
    }
    return HALTZ_IO_DEV_OTHER;
}

static void device_event(uni_hid_device_t *d, int slot, uint8_t type) {
    bt_event_t ev = {.type = type, .slot = (uint8_t)slot, .kind = kind_of(d),
                     .battery = s_battery[slot], .vendor_id = d->vendor_id,
                     .product_id = d->product_id};
    post(&ev);
}

/* ---- uni_platform callbacks (BTstack thread) ---------------------------------------------- */

static void p_init(int argc, const char **argv) {
    (void)argc;
    (void)argv;
}

static void p_on_init_complete(void) {
    logi("HALTZ: Bluetooth ready, scanning for devices (paired ones reconnect)\n");
    uni_bt_start_scanning_and_autoconnect_unsafe();
}

static void p_on_device_connected(uni_hid_device_t *d) {
    (void)d; /* not usable yet: wait for on_device_ready */
}

static void p_on_device_disconnected(uni_hid_device_t *d) {
    int slot = slot_of(d);
    if (slot < 0 || !(s_ready & (1u << slot))) {
        return;
    }
    s_ready &= (uint8_t) ~(1u << slot);
    logi("HALTZ: device %d disconnected\n", slot);
    device_event(d, slot, BT_EV_DISCONNECT);
    s_battery[slot] = HALTZ_IO_BATTERY_UNKNOWN;
}

static uni_error_t p_on_device_ready(uni_hid_device_t *d) {
    int slot = slot_of(d);
    if (slot < 0) {
        return UNI_ERROR_NO_SLOTS;
    }
    s_ready |= (uint8_t)(1u << slot);
    s_battery[slot] = HALTZ_IO_BATTERY_UNKNOWN;
    logi("HALTZ: device %d ready: %s, VID %04x PID %04x\n", slot, d->name, d->vendor_id,
         d->product_id);
    device_event(d, slot, BT_EV_CONNECT);
    return UNI_ERROR_SUCCESS;
}

static void p_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl) {
    int slot = slot_of(d);
    if (slot < 0 || !(s_ready & (1u << slot))) {
        return;
    }
    if (ctl->battery != s_battery[slot]) {
        s_battery[slot] = ctl->battery;
        device_event(d, slot, BT_EV_BATTERY);
    }
    if (ctl->klass != UNI_CONTROLLER_CLASS_KEYBOARD) {
        return; /* mouse and gamepad data aren't forwarded in v1 */
    }
    bt_event_t ev = {.type = BT_EV_KEYS, .slot = (uint8_t)slot, .kind = HALTZ_IO_DEV_KEYBOARD,
                     .modifiers = ctl->keyboard.modifiers};
    memcpy(ev.keys, ctl->keyboard.pressed_keys, sizeof(ev.keys));
    post(&ev);
}

static void p_on_oob_event(uni_platform_oob_event_t event, void *data) {
    if (event == UNI_PLATFORM_OOB_BLUETOOTH_ENABLED) {
        s_scanning = (bool)(uintptr_t)data;
    }
}

static const uni_property_t *p_get_property(uni_property_idx_t idx) {
    (void)idx;
    return NULL;
}

void io_console_register(void); /* io_console.c */

struct uni_platform *haltz_platform_get(void) {
    static struct uni_platform plat = {
        .name = "HALTZ",
        .init = p_init,
        .on_init_complete = p_on_init_complete,
        .on_device_connected = p_on_device_connected,
        .on_device_disconnected = p_on_device_disconnected,
        .on_device_ready = p_on_device_ready,
        .on_controller_data = p_on_controller_data,
        .on_oob_event = p_on_oob_event,
        .get_property = p_get_property,
        .register_console_cmds = io_console_register,
    };
    if (s_queue == NULL) {
        s_queue = xQueueCreate(QUEUE_LEN, sizeof(bt_event_t));
        assert(s_queue != NULL);
    }
    return &plat;
}

/* ---- other tasks ---------------------------------------------------------------------- */

bool haltz_bt_next(bt_event_t *ev) {
    return s_queue != NULL && xQueueReceive(s_queue, ev, 0) == pdTRUE;
}

uint32_t haltz_bt_dropped(void) {
    return s_dropped;
}

bool haltz_bt_scanning(void) {
    return s_scanning;
}

void haltz_bt_action(uint8_t action) {
    switch (action) {
    case HALTZ_IO_BT_SCAN_OFF:
        uni_bt_stop_scanning_safe();
        break;
    case HALTZ_IO_BT_SCAN_ON:
        uni_bt_start_scanning_and_autoconnect_safe();
        break;
    case HALTZ_IO_BT_FORGET:
        /* Bluepad32 has a ring of 8 pending calls: at most 4 disconnects + 1 here. */
        for (int i = 0; i < HALTZ_IO_MAX_DEVICES; i++) {
            if (s_ready & (1u << i)) {
                uni_bt_disconnect_device_safe(i);
            }
        }
        uni_bt_del_keys_safe();
        break;
    default:
        break;
    }
}
