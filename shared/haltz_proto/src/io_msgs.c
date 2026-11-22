#include "haltz/io_msgs.h"

#include <string.h>

#include "haltz/msgs.h"
#include "wire.h"

/* The element limits must match the sizes in msg_ids.def. */
_Static_assert(HALTZ_MSG_IO_KEYS_MAX_LEN == 1 + HALTZ_IO_KEYS_MAX, "keys");
_Static_assert(HALTZ_MSG_IO_KEYS_MIN_LEN == 1, "keys");
_Static_assert(HALTZ_MSG_IO_NOTES_MAX_LEN == 3 + 2 * HALTZ_IO_NOTES_MAX, "notes");
_Static_assert(HALTZ_MSG_IO_NOTES_MIN_LEN == 3 + 2, "notes");
_Static_assert(HALTZ_IO_TUNE_NOTES <= 255, "tune_free is a u8");

/* ---- helpers ----------------------------------------------------------------------------- */

static haltz_err_t pack_begin(const void *msg, const uint8_t *buf, size_t cap, size_t need) {
    if (msg == NULL || buf == NULL) {
        return HALTZ_ERR_ARG;
    }
    return (cap < need) ? HALTZ_ERR_NO_SPACE : HALTZ_OK;
}

static haltz_err_t pack_done(size_t *out_len, size_t pos) {
    if (out_len != NULL) {
        *out_len = pos;
    }
    return HALTZ_OK;
}

/* Arguments, then the LEN range from msg_ids.def. */
static haltz_err_t unpack_begin(const void *msg, const uint8_t *buf, uint8_t type, size_t len) {
    if (msg == NULL || (buf == NULL && len > 0)) {
        return HALTZ_ERR_ARG;
    }
    return haltz_msg_check(type, len);
}

static int voice_ok(uint8_t v) {
    return v < HALTZ_IO_VOICES;
}

static int voices_ok(uint8_t mask) {
    return mask != 0 && (mask & ~HALTZ_IO_VOICES_ALL) == 0;
}

static int axis_ok(int16_t a) {
    return a >= -HALTZ_IO_AXIS_MAX && a <= HALTZ_IO_AXIS_MAX;
}

/* ---- value checks (shared by pack and unpack) -------------------------------------------- */

static haltz_err_t check_keys(const haltz_io_keys_t *m) {
    if (m->count > HALTZ_IO_KEYS_MAX) {
        return HALTZ_ERR_BAD_VALUE;
    }
    for (int i = 0; i < m->count; i++) {
        uint8_t u = m->usages[i];
        if (u < HALTZ_IO_USAGE_FIRST || u > HALTZ_IO_USAGE_LAST) {
            return HALTZ_ERR_BAD_VALUE;
        }
        for (int j = 0; j < i; j++) {
            if (m->usages[j] == u) {
                return HALTZ_ERR_BAD_VALUE;
            }
        }
    }
    return HALTZ_OK;
}

static haltz_err_t check_stick(const haltz_io_stick_t *m) {
    return (axis_ok(m->x) && axis_ok(m->y) && (m->buttons & ~HALTZ_IO_BTN_ALL) == 0)
               ? HALTZ_OK
               : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_device(const haltz_io_device_t *m) {
    return (m->slot < HALTZ_IO_MAX_DEVICES && m->state <= HALTZ_IO_DEV_CONNECTED &&
            m->kind <= HALTZ_IO_DEV_KIND_LAST)
               ? HALTZ_OK
               : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_led(const haltz_io_led_t *m) {
    if (m->mode > HALTZ_IO_LED_MODE_LAST) {
        return HALTZ_ERR_BAD_VALUE;
    }
    if (m->mode == HALTZ_IO_LED_CODE) {
        if (m->count < 1 || m->count > HALTZ_IO_LED_CODE_MAX) {
            return HALTZ_ERR_BAD_VALUE;
        }
    } else if (m->count != 0) {
        return HALTZ_ERR_BAD_VALUE;
    }
    switch (m->mode) {
    case HALTZ_IO_LED_BLINK:
    case HALTZ_IO_LED_BREATHE:
    case HALTZ_IO_LED_CODE:
        return (m->period_ms >= HALTZ_IO_LED_PERIOD_MIN) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
    case HALTZ_IO_LED_FLASH:
        return (m->period_ms >= 1) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
    default:
        return HALTZ_OK; /* SOLID ignores the period, FADE takes any (0 = at once) */
    }
}

static haltz_err_t check_tone(const haltz_io_tone_t *m) {
    int hz_ok = m->hz == 0 || (m->hz >= HALTZ_IO_TONE_HZ_MIN && m->hz <= HALTZ_IO_TONE_HZ_MAX);
    return (voice_ok(m->voice) && hz_ok) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_notes(const haltz_io_notes_t *m) {
    if (!voice_ok(m->voice) || (m->flags & ~HALTZ_IO_NOTES_FLAGS) || m->tick_ms < 1 ||
        m->count < 1 || m->count > HALTZ_IO_NOTES_MAX) {
        return HALTZ_ERR_BAD_VALUE;
    }
    for (int i = 0; i < m->count; i++) {
        uint8_t n = m->notes[i].note;
        if ((n != HALTZ_IO_NOTE_REST && n < HALTZ_IO_NOTE_LOWEST) || n > HALTZ_IO_NOTE_HIGHEST ||
            m->notes[i].ticks < 1) {
            return HALTZ_ERR_BAD_VALUE;
        }
    }
    return HALTZ_OK;
}

static haltz_err_t check_sfx(const haltz_io_sfx_msg_t *m) {
    return (m->id < HALTZ_IO_SFX_COUNT && voice_ok(m->voice)) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_stop(const haltz_io_stop_t *m) {
    return voices_ok(m->voices) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_volume(const haltz_io_volume_t *m) {
    return voices_ok(m->voices) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

static haltz_err_t check_bt(const haltz_io_bt_t *m) {
    return (m->action <= HALTZ_IO_BT_ACTION_LAST) ? HALTZ_OK : HALTZ_ERR_BAD_VALUE;
}

/* ---- I/O board -> S3 --------------------------------------------------------------------- */

haltz_err_t haltz_io_keys_pack(const haltz_io_keys_t *msg, uint8_t *buf, size_t cap,
                               size_t *out_len) {
    haltz_err_t err = (msg == NULL || buf == NULL) ? HALTZ_ERR_ARG : check_keys(msg);
    if (err == HALTZ_OK) {
        err = pack_begin(msg, buf, cap, 1u + msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->modifiers);
    for (int i = 0; i < msg->count; i++) {
        put_u8(buf, &pos, msg->usages[i]);
    }
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_keys_unpack(haltz_io_keys_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_KEYS, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->modifiers = get_u8(buf, &pos);
    msg->count = (uint8_t)(len - 1);
    for (int i = 0; i < msg->count; i++) {
        msg->usages[i] = get_u8(buf, &pos);
    }
    return check_keys(msg);
}

haltz_err_t haltz_io_stick_pack(const haltz_io_stick_t *msg, uint8_t *buf, size_t cap,
                                size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_STICK_LEN);
    if (err == HALTZ_OK) {
        err = check_stick(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_i16le(buf, &pos, msg->x);
    put_i16le(buf, &pos, msg->y);
    put_u8(buf, &pos, msg->buttons);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_stick_unpack(haltz_io_stick_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_STICK, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->x = get_i16le(buf, &pos);
    msg->y = get_i16le(buf, &pos);
    msg->buttons = get_u8(buf, &pos);
    return check_stick(msg);
}

haltz_err_t haltz_io_knob_pack(const haltz_io_knob_t *msg, uint8_t *buf, size_t cap,
                               size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_KNOB_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->value);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_knob_unpack(haltz_io_knob_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_KNOB, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->value = buf[0];
    return HALTZ_OK;
}

haltz_err_t haltz_io_device_pack(const haltz_io_device_t *msg, uint8_t *buf, size_t cap,
                                 size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_DEVICE_LEN);
    if (err == HALTZ_OK) {
        err = check_device(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->slot);
    put_u8(buf, &pos, msg->state);
    put_u8(buf, &pos, msg->kind);
    put_u8(buf, &pos, msg->battery);
    put_u16le(buf, &pos, msg->vendor_id);
    put_u16le(buf, &pos, msg->product_id);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_device_unpack(haltz_io_device_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_DEVICE, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->slot = get_u8(buf, &pos);
    msg->state = get_u8(buf, &pos);
    msg->kind = get_u8(buf, &pos);
    msg->battery = get_u8(buf, &pos);
    msg->vendor_id = get_u16le(buf, &pos);
    msg->product_id = get_u16le(buf, &pos);
    return check_device(msg);
}

haltz_err_t haltz_io_status_pack(const haltz_io_status_t *msg, uint8_t *buf, size_t cap,
                                 size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_STATUS_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u32le(buf, &pos, msg->uptime_ms);
    put_u16le(buf, &pos, msg->rx_frames);
    put_u16le(buf, &pos, msg->crc_errors);
    put_u16le(buf, &pos, msg->cmd_errors);
    put_u16le(buf, &pos, msg->rx_overruns);
    put_u8(buf, &pos, msg->flags);
    put_u8(buf, &pos, msg->devices);
    put_u8(buf, &pos, msg->tune_free[0]);
    put_u8(buf, &pos, msg->tune_free[1]);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_status_unpack(haltz_io_status_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_STATUS, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->uptime_ms = get_u32le(buf, &pos);
    msg->rx_frames = get_u16le(buf, &pos);
    msg->crc_errors = get_u16le(buf, &pos);
    msg->cmd_errors = get_u16le(buf, &pos);
    msg->rx_overruns = get_u16le(buf, &pos);
    msg->flags = get_u8(buf, &pos);
    msg->devices = get_u8(buf, &pos);
    msg->tune_free[0] = get_u8(buf, &pos);
    msg->tune_free[1] = get_u8(buf, &pos);
    return HALTZ_OK;
}

/* ---- S3 -> I/O board --------------------------------------------------------------------- */

haltz_err_t haltz_io_led_pack(const haltz_io_led_t *msg, uint8_t *buf, size_t cap,
                              size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_LED_LEN);
    if (err == HALTZ_OK) {
        err = check_led(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->mode);
    put_u8(buf, &pos, msg->r);
    put_u8(buf, &pos, msg->g);
    put_u8(buf, &pos, msg->b);
    put_u16le(buf, &pos, msg->period_ms);
    put_u8(buf, &pos, msg->count);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_led_unpack(haltz_io_led_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_LED, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->mode = get_u8(buf, &pos);
    msg->r = get_u8(buf, &pos);
    msg->g = get_u8(buf, &pos);
    msg->b = get_u8(buf, &pos);
    msg->period_ms = get_u16le(buf, &pos);
    msg->count = get_u8(buf, &pos);
    return check_led(msg);
}

haltz_err_t haltz_io_tone_pack(const haltz_io_tone_t *msg, uint8_t *buf, size_t cap,
                               size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_TONE_LEN);
    if (err == HALTZ_OK) {
        err = check_tone(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->voice);
    put_u16le(buf, &pos, msg->hz);
    put_u16le(buf, &pos, msg->ms);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_tone_unpack(haltz_io_tone_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_TONE, len);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->voice = get_u8(buf, &pos);
    msg->hz = get_u16le(buf, &pos);
    msg->ms = get_u16le(buf, &pos);
    return check_tone(msg);
}

haltz_err_t haltz_io_notes_pack(const haltz_io_notes_t *msg, uint8_t *buf, size_t cap,
                                size_t *out_len) {
    haltz_err_t err = (msg == NULL || buf == NULL) ? HALTZ_ERR_ARG : check_notes(msg);
    if (err == HALTZ_OK) {
        err = pack_begin(msg, buf, cap, 3u + 2u * msg->count);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->voice);
    put_u8(buf, &pos, msg->flags);
    put_u8(buf, &pos, msg->tick_ms);
    for (int i = 0; i < msg->count; i++) {
        put_u8(buf, &pos, msg->notes[i].note);
        put_u8(buf, &pos, msg->notes[i].ticks);
    }
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_notes_unpack(haltz_io_notes_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_NOTES, len);
    if (err != HALTZ_OK) {
        return err;
    }
    if ((len - 3) % 2 != 0) {
        return HALTZ_ERR_BAD_LEN;
    }
    size_t pos = 0;
    msg->voice = get_u8(buf, &pos);
    msg->flags = get_u8(buf, &pos);
    msg->tick_ms = get_u8(buf, &pos);
    msg->count = (uint8_t)((len - 3) / 2);
    for (int i = 0; i < msg->count; i++) {
        msg->notes[i].note = get_u8(buf, &pos);
        msg->notes[i].ticks = get_u8(buf, &pos);
    }
    return check_notes(msg);
}

haltz_err_t haltz_io_sfx_pack(const haltz_io_sfx_msg_t *msg, uint8_t *buf, size_t cap,
                              size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_SFX_LEN);
    if (err == HALTZ_OK) {
        err = check_sfx(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->id);
    put_u8(buf, &pos, msg->voice);
    return pack_done(out_len, pos);
}

haltz_err_t haltz_io_sfx_unpack(haltz_io_sfx_msg_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_SFX, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->id = buf[0];
    msg->voice = buf[1];
    return check_sfx(msg);
}

haltz_err_t haltz_io_stop_pack(const haltz_io_stop_t *msg, uint8_t *buf, size_t cap,
                               size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_STOP_LEN);
    if (err == HALTZ_OK) {
        err = check_stop(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    buf[0] = msg->voices;
    return pack_done(out_len, 1);
}

haltz_err_t haltz_io_stop_unpack(haltz_io_stop_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_STOP, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->voices = buf[0];
    return check_stop(msg);
}

haltz_err_t haltz_io_volume_pack(const haltz_io_volume_t *msg, uint8_t *buf, size_t cap,
                                 size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_VOLUME_LEN);
    if (err == HALTZ_OK) {
        err = check_volume(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    buf[0] = msg->voices;
    buf[1] = msg->level;
    return pack_done(out_len, 2);
}

haltz_err_t haltz_io_volume_unpack(haltz_io_volume_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_VOLUME, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->voices = buf[0];
    msg->level = buf[1];
    return check_volume(msg);
}

haltz_err_t haltz_io_bt_pack(const haltz_io_bt_t *msg, uint8_t *buf, size_t cap,
                             size_t *out_len) {
    haltz_err_t err = pack_begin(msg, buf, cap, HALTZ_MSG_IO_BT_LEN);
    if (err == HALTZ_OK) {
        err = check_bt(msg);
    }
    if (err != HALTZ_OK) {
        return err;
    }
    buf[0] = msg->action;
    return pack_done(out_len, 1);
}

haltz_err_t haltz_io_bt_unpack(haltz_io_bt_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = unpack_begin(msg, buf, HALTZ_MSG_IO_BT, len);
    if (err != HALTZ_OK) {
        return err;
    }
    msg->action = buf[0];
    return check_bt(msg);
}

/* ---- any S3 -> I/O command --------------------------------------------------------------- */

haltz_err_t haltz_io_cmd_unpack(haltz_io_cmd_t *cmd, uint8_t type, const uint8_t *buf,
                                size_t len) {
    if (cmd == NULL) {
        return HALTZ_ERR_ARG;
    }
    cmd->type = type;
    switch (type) {
    case HALTZ_MSG_IO_LED:
        return haltz_io_led_unpack(&cmd->u.led, buf, len);
    case HALTZ_MSG_IO_TONE:
        return haltz_io_tone_unpack(&cmd->u.tone, buf, len);
    case HALTZ_MSG_IO_NOTES:
        return haltz_io_notes_unpack(&cmd->u.notes, buf, len);
    case HALTZ_MSG_IO_SFX:
        return haltz_io_sfx_unpack(&cmd->u.sfx, buf, len);
    case HALTZ_MSG_IO_STOP:
        return haltz_io_stop_unpack(&cmd->u.stop, buf, len);
    case HALTZ_MSG_IO_VOLUME:
        return haltz_io_volume_unpack(&cmd->u.volume, buf, len);
    case HALTZ_MSG_IO_STICK_CAL:
        return unpack_begin(cmd, buf, type, len);
    case HALTZ_MSG_IO_BT:
        return haltz_io_bt_unpack(&cmd->u.bt, buf, len);
    default:
        return HALTZ_ERR_UNKNOWN_TYPE;
    }
}

haltz_err_t haltz_io_cmd_pack(const haltz_io_cmd_t *cmd, uint8_t *buf, size_t cap,
                              size_t *out_len) {
    if (cmd == NULL) {
        return HALTZ_ERR_ARG;
    }
    switch (cmd->type) {
    case HALTZ_MSG_IO_LED:
        return haltz_io_led_pack(&cmd->u.led, buf, cap, out_len);
    case HALTZ_MSG_IO_TONE:
        return haltz_io_tone_pack(&cmd->u.tone, buf, cap, out_len);
    case HALTZ_MSG_IO_NOTES:
        return haltz_io_notes_pack(&cmd->u.notes, buf, cap, out_len);
    case HALTZ_MSG_IO_SFX:
        return haltz_io_sfx_pack(&cmd->u.sfx, buf, cap, out_len);
    case HALTZ_MSG_IO_STOP:
        return haltz_io_stop_pack(&cmd->u.stop, buf, cap, out_len);
    case HALTZ_MSG_IO_VOLUME:
        return haltz_io_volume_pack(&cmd->u.volume, buf, cap, out_len);
    case HALTZ_MSG_IO_STICK_CAL:
        return pack_done(out_len, 0);
    case HALTZ_MSG_IO_BT:
        return haltz_io_bt_pack(&cmd->u.bt, buf, cap, out_len);
    default:
        return HALTZ_ERR_UNKNOWN_TYPE;
    }
}
