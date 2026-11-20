#include "haltz/msgs.h"

/* ---- tables generated from msg_ids.def ------------------------------------------------- */

#define X(name, id, len)                                                                       \
    _Static_assert((id) >= 0x01 && (id) <= 0xFF, "msg " #name ": ID must be 0x01..0xFF");     \
    _Static_assert((len) >= 0 && (len) <= HALTZ_MAX_PAYLOAD, "msg " #name ": bad PAYLOAD_LEN");
#include "haltz/msg_ids.def"
#undef X

const uint8_t haltz_msg_ids[HALTZ_MSG_COUNT] = {
#define X(name, id, len) (id),
#include "haltz/msg_ids.def"
#undef X
};

const char *const haltz_msg_names[HALTZ_MSG_COUNT] = {
#define X(name, id, len) #name,
#include "haltz/msg_ids.def"
#undef X
};

const uint8_t haltz_msg_payload_lens[HALTZ_MSG_COUNT] = {
#define X(name, id, len) (len),
#include "haltz/msg_ids.def"
#undef X
};

/* A duplicate ID in msg_ids.def becomes a duplicate case label: a compile error. */
static int msg_index(uint8_t id) {
    switch (id) {
#define X(name, id, len)                                                                       \
    case (id):                                                                                 \
        return HALTZ_MSG_IDX_##name;
#include "haltz/msg_ids.def"
#undef X
    default:
        return -1;
    }
}

const char *haltz_msg_name(uint8_t id) {
    int i = msg_index(id);
    return (i < 0) ? "?" : haltz_msg_names[i];
}

int haltz_msg_payload_len(uint8_t id) {
    int i = msg_index(id);
    return (i < 0) ? -1 : haltz_msg_payload_lens[i];
}

haltz_err_t haltz_msg_check(uint8_t id, size_t len) {
    int expected = haltz_msg_payload_len(id);
    if (expected < 0) {
        return HALTZ_ERR_UNKNOWN_TYPE;
    }
    return ((size_t)expected == len) ? HALTZ_OK : HALTZ_ERR_BAD_LEN;
}

/* ---- little-endian byte helpers -------------------------------------------------------- */

static void put_u8(uint8_t *buf, size_t *pos, uint8_t v) {
    buf[(*pos)++] = v;
}

static void put_u16le(uint8_t *buf, size_t *pos, uint16_t v) {
    buf[(*pos)++] = (uint8_t)(v & 0xFFu);
    buf[(*pos)++] = (uint8_t)(v >> 8);
}

static uint8_t get_u8(const uint8_t *buf, size_t *pos) {
    return buf[(*pos)++];
}

static uint16_t get_u16le(const uint8_t *buf, size_t *pos) {
    uint16_t v = (uint16_t)buf[*pos] | (uint16_t)((uint16_t)buf[*pos + 1] << 8);
    *pos += 2;
    return v;
}

static haltz_err_t check_pack(const void *msg, const uint8_t *buf, size_t cap, size_t need) {
    if (msg == NULL || buf == NULL) {
        return HALTZ_ERR_ARG;
    }
    return (cap < need) ? HALTZ_ERR_NO_SPACE : HALTZ_OK;
}

static haltz_err_t check_unpack(const void *msg, const uint8_t *buf, size_t len, size_t need) {
    if (msg == NULL || buf == NULL) {
        return HALTZ_ERR_ARG;
    }
    return (len != need) ? HALTZ_ERR_BAD_LEN : HALTZ_OK;
}

/* ---- HELLO ---------------------------------------------------------------------------- */

haltz_err_t haltz_hello_pack(const haltz_hello_t *msg, uint8_t *buf, size_t cap) {
    haltz_err_t err = check_pack(msg, buf, cap, HALTZ_MSG_HELLO_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->board_id);
    put_u16le(buf, &pos, msg->proto_version);
    for (int i = 0; i < 4; i++) {
        put_u8(buf, &pos, msg->git_hash[i]);
    }
    return HALTZ_OK;
}

haltz_err_t haltz_hello_unpack(haltz_hello_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = check_unpack(msg, buf, len, HALTZ_MSG_HELLO_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->board_id = get_u8(buf, &pos);
    msg->proto_version = get_u16le(buf, &pos);
    for (int i = 0; i < 4; i++) {
        msg->git_hash[i] = get_u8(buf, &pos);
    }
    return HALTZ_OK;
}

/* ---- ACK / NAK ------------------------------------------------------------------------- */

haltz_err_t haltz_ack_pack(const haltz_ack_t *msg, uint8_t *buf, size_t cap) {
    haltz_err_t err = check_pack(msg, buf, cap, HALTZ_MSG_ACK_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->type);
    return HALTZ_OK;
}

haltz_err_t haltz_ack_unpack(haltz_ack_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = check_unpack(msg, buf, len, HALTZ_MSG_ACK_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->type = get_u8(buf, &pos);
    return HALTZ_OK;
}

haltz_err_t haltz_nak_pack(const haltz_nak_t *msg, uint8_t *buf, size_t cap) {
    haltz_err_t err = check_pack(msg, buf, cap, HALTZ_MSG_NAK_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    put_u8(buf, &pos, msg->type);
    put_u8(buf, &pos, msg->error);
    return HALTZ_OK;
}

haltz_err_t haltz_nak_unpack(haltz_nak_t *msg, const uint8_t *buf, size_t len) {
    haltz_err_t err = check_unpack(msg, buf, len, HALTZ_MSG_NAK_LEN);
    if (err != HALTZ_OK) {
        return err;
    }
    size_t pos = 0;
    msg->type = get_u8(buf, &pos);
    msg->error = get_u8(buf, &pos);
    return HALTZ_OK;
}

/* ---- git hash helpers ------------------------------------------------------------------ */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

haltz_err_t haltz_git_hash_parse(const char *hex, uint8_t out[4]) {
    if (hex == NULL || out == NULL) {
        return HALTZ_ERR_ARG;
    }
    for (int i = 0; i < 4; i++) {
        int hi = hex_value(hex[2 * i]);
        int lo = (hi < 0) ? -1 : hex_value(hex[2 * i + 1]); /* stop at NUL, never read past */
        if (hi < 0 || lo < 0) {
            out[0] = out[1] = out[2] = out[3] = 0;
            return HALTZ_ERR_BAD_VALUE;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return HALTZ_OK;
}

void haltz_git_hash_format(const uint8_t hash[4], char out[9]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[2 * i] = digits[hash[i] >> 4];
        out[2 * i + 1] = digits[hash[i] & 0x0Fu];
    }
    out[8] = '\0';
}
