#include "zt_ids.h"

static int lower_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Callers supply fixed widths no greater than sixteen nibbles. Parsing into a
 * temporary keeps the destination unchanged on malformed input. */
static zt_err_t parse_hex(const char *text, size_t width, uint64_t *out)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        int digit = lower_hex(text[i]);
        if (digit < 0) return ZT_ERR_PROTOCOL;
        value = (value << 4) | (unsigned)digit;
    }
    *out = value;
    return ZT_OK;
}

static void format_hex(uint64_t value, char *out, size_t width)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = width; i > 0; --i) {
        out[i - 1] = digits[value & 15u];
        value >>= 4;
    }
}

zt_err_t zt_mac_format(const zt_mac_t *value, char *out, size_t capacity)
{
    if (!value || !out) return ZT_ERR_INVALID_ARG;
    if (capacity < ZT_MAC_TEXT_BYTES) return ZT_ERR_NO_SPACE;
    zt_mac_t copy = *value;
    for (size_t i = 0; i < ZT_MAC_BYTES; ++i)
        format_hex(copy.bytes[i], out + 2 * i, 2);
    out[ZT_MAC_TEXT_LEN] = '\0';
    return ZT_OK;
}

zt_err_t zt_mac_parse(const char *text, size_t len, zt_mac_t *out)
{
    if (!text || !out) return ZT_ERR_INVALID_ARG;
    if (len != ZT_MAC_TEXT_LEN) return ZT_ERR_INVALID_LENGTH;
    zt_mac_t value = {0};
    for (size_t i = 0; i < ZT_MAC_BYTES; ++i) {
        uint64_t byte;
        zt_err_t result = parse_hex(text + 2 * i, 2, &byte);
        if (result != ZT_OK) return result;
        value.bytes[i] = (uint8_t)byte;
    }
    *out = value;
    return ZT_OK;
}

zt_err_t zt_id_format(uint64_t value, char *out, size_t capacity)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (capacity < ZT_ID_TEXT_BYTES) return ZT_ERR_NO_SPACE;
    format_hex(value, out, ZT_ID_TEXT_LEN);
    out[ZT_ID_TEXT_LEN] = '\0';
    return ZT_OK;
}

zt_err_t zt_id_parse(const char *text, size_t len, uint64_t *out)
{
    if (!text || !out) return ZT_ERR_INVALID_ARG;
    if (len != ZT_ID_TEXT_LEN) return ZT_ERR_INVALID_LENGTH;
    return parse_hex(text, len, out);
}

zt_err_t zt_event_id_format(const zt_event_id_t *value, char *out, size_t capacity)
{
    if (!value || !out) return ZT_ERR_INVALID_ARG;
    if (!value->round_id || value->victim_slot >= ZT_MAX_PLAYERS ||
        value->event_seq < ZT_EVENT_SEQ_FIRST) return ZT_ERR_INVALID_ARG;
    if (capacity < ZT_EVENT_ID_TEXT_BYTES) return ZT_ERR_NO_SPACE;
    zt_event_id_t copy = *value;
    format_hex(copy.round_id, out, ZT_ID_TEXT_LEN);
    out[16] = '/';
    format_hex(copy.victim_slot, out + 17, 2);
    out[19] = '/';
    format_hex(copy.event_seq, out + 20, 4);
    out[ZT_EVENT_ID_TEXT_LEN] = '\0';
    return ZT_OK;
}

zt_err_t zt_event_id_parse(const char *text, size_t len, zt_event_id_t *out)
{
    if (!text || !out) return ZT_ERR_INVALID_ARG;
    if (len != ZT_EVENT_ID_TEXT_LEN) return ZT_ERR_INVALID_LENGTH;
    if (text[16] != '/' || text[19] != '/') return ZT_ERR_PROTOCOL;
    uint64_t round, slot, sequence;
    zt_err_t result = parse_hex(text, ZT_ID_TEXT_LEN, &round);
    if (result != ZT_OK) return result;
    result = parse_hex(text + 17, 2, &slot);
    if (result != ZT_OK) return result;
    result = parse_hex(text + 20, 4, &sequence);
    if (result != ZT_OK) return result;
    if (!round || slot >= ZT_MAX_PLAYERS || sequence < ZT_EVENT_SEQ_FIRST)
        return ZT_ERR_PROTOCOL;
    *out = (zt_event_id_t){.round_id = round, .victim_slot = (zt_slot_t)slot,
                           .event_seq = (uint16_t)sequence};
    return ZT_OK;
}
