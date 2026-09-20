#pragma once

#include <stddef.h>
#include "zt_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZT_MAC_BYTES 6
#define ZT_MAC_TEXT_LEN 12
#define ZT_MAC_TEXT_BYTES (ZT_MAC_TEXT_LEN + 1)
#define ZT_ID_TEXT_LEN 16
#define ZT_ID_TEXT_BYTES (ZT_ID_TEXT_LEN + 1)
#define ZT_EVENT_ID_TEXT_LEN 24
#define ZT_EVENT_ID_TEXT_BYTES (ZT_EVENT_ID_TEXT_LEN + 1)
#define ZT_BOOT_NONCE_BYTES 8
#define ZT_EVENT_SEQ_FIRST 1
#define ZT_PATIENT_ZERO_CAUSE_SEQ 0
#define ZT_ROUND_LOBBY UINT64_C(0)
typedef struct { uint8_t bytes[ZT_MAC_BYTES]; } zt_mac_t;
typedef uint64_t zt_game_id_t;
typedef uint64_t zt_round_id_t;
typedef uint64_t zt_boot_nonce_t;
typedef uint8_t zt_slot_t;
#define ZT_SLOT_INVALID UINT8_C(255) /* unassigned slot / human cause */
#define ZT_SLOT_ALL UINT8_C(255) /* command/cache target broadcast only */
typedef struct {
    zt_round_id_t round_id;
    zt_slot_t victim_slot;
    uint16_t event_seq;
} zt_event_id_t;
typedef struct { zt_slot_t slot; uint16_t seq; } zt_cause_t;
/* Full factory STA MAC is the durable identity, never truncated. Text codecs
 * require lowercase hex, exact length, no trailing bytes, and no wrap.
 * Event format: <round16>/<slot02>/<seq04>, all lowercase hexadecimal.
 * Output capacities include a separate NUL byte. No allocation. */
zt_err_t zt_mac_format(const zt_mac_t *value, char *out, size_t capacity);
zt_err_t zt_mac_parse(const char *text, size_t len, zt_mac_t *out);
zt_err_t zt_id_format(uint64_t value, char *out, size_t capacity);
zt_err_t zt_id_parse(const char *text, size_t len, uint64_t *out);
zt_err_t zt_event_id_format(const zt_event_id_t *value, char *out, size_t capacity);
zt_err_t zt_event_id_parse(const char *text, size_t len, zt_event_id_t *out);

#ifdef __cplusplus
}
#endif
