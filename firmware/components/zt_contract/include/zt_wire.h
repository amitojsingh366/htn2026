#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_ids.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Decode/encode explicit little-endian fields, NEVER transmit a C struct.
 * Every decoder rejects trailing bytes, incorrect counts/padding/reserved bits,
 * unsupported enums, and invalid ranges. No allocation; caller owns buffers.
 * Struct padding and sizeof are unrelated to encoded sizes below. */
#define ZT_WIRE_MAGIC UINT16_C(0x5A54)
#define ZT_WIRE_HEADER_BYTES 48
#define ZT_HMAC_KEY_BYTES 32
#define ZT_HMAC_TAG_BYTES 16
#define ZT_MAX_FRAME_BYTES 250
#define ZT_MAX_PAYLOAD 186
#define ZT_INT32_UNKNOWN INT32_MIN
#define ZT_COMMAND_NO_EXPIRY UINT32_MAX
#define ZT_ROSTER_PAGE_ENTRIES 8
#define ZT_ROLE_PAGE_ENTRIES 12
#define ZT_SNAPSHOT_MAX_PAGES 3
#define ZT_WATERMARK_ENTRIES 20
#define ZT_DECISION_PAGE_ENTRIES 20
#define ZT_CACHE_PAGE_ENTRIES 56
#define ZT_WANT_EVENT_ENTRIES 16
#define ZT_COMMAND_ARGS_MAX_BYTES 175
#define ZT_VALID_SLOT_BITMAP UINT32_C(0x000fffff)
#define ZT_HASH_PREFIX_BYTES 8
#define ZT_SHA256_BYTES 32
#define ZT_WIRE_NAME_BYTES 12
#define ZT_WIRE_FLAG_RELAY_CAPABLE (1u << 0)
#define ZT_BEACON_FLAG_HOST (1u << 0)
#define ZT_BEACON_FLAG_PROVISIONAL (1u << 1)
#define ZT_BEACON_FLAG_STORAGE_HEALTHY (1u << 2)
#define ZT_BEACON_FLAG_REGISTERED (1u << 3)
/* Live gateway status is distinct from host/clock reachability. Only the
 * designated host originates this bit; clients expire the authenticated report. */
#define ZT_BEACON_FLAG_SERVER_CONNECTED (1u << 4)
#define ZT_HOST_STATE_FLAG_SERVER_CONNECTED (1u << 0)
#define ZT_SERVER_STATUS_MAX_AGE_MS 10000
#define ZT_ROLE_FLAG_PROVISIONAL (1u << 0)
#define ZT_ROLE_FLAG_READY (1u << 1)
#define ZT_NEED_ROSTER (1u << 0)
#define ZT_NEED_ROLES_PHASE_TIME (1u << 1)
#define ZT_NEED_WATERMARKS (1u << 2)
#define ZT_NEED_DECISIONS (1u << 3)

typedef enum {
    ZT_PKT_BEACON = 0x1,
    ZT_PKT_TAG_REQUEST = 0x2,
    ZT_PKT_TAG_RESULT = 0x3,
    ZT_PKT_JOIN = 0x4,
    ZT_PKT_JOIN_RESULT = 0x5,
    ZT_PKT_EVENT = 0x10,
    ZT_PKT_ROSTER_PAGE = 0x20,
    ZT_PKT_HOST_STATE = 0x21,
    ZT_PKT_WATERMARKS = 0x22,
    ZT_PKT_COMMAND = 0x23,
    ZT_PKT_COMMAND_RECEIPT = 0x24,
    ZT_PKT_SNAPSHOT_REQUEST = 0x25,
    ZT_PKT_ROUND_CLOSED = 0x26,
    ZT_PKT_CLOSE_RECEIPTS = 0x27,
    ZT_PKT_EVENT_DECISIONS = 0x28,
    ZT_PKT_DECISION_RECEIPT = 0x29,
    ZT_PKT_CACHE_PAGE = 0x30,
    ZT_PKT_WANT_EVENTS = 0x31,
    ZT_PKT_EVENT_COPY = 0x32,
    ZT_PKT_TIME_QUERY = 0x33,
    ZT_PKT_TIME_REPLY = 0x34
} zt_pkt_type_t;

typedef enum {
    ZT_ROLE_HUMAN = 0,
    ZT_ROLE_ZOMBIE = 1,
    ZT_ROLE_UNKNOWN = 255
} zt_wire_role_t;

typedef enum {
    ZT_PHASE_LOBBY = 0,
    ZT_PHASE_PREPARED = 1,
    ZT_PHASE_RUNNING = 2,
    ZT_PHASE_EXPIRED_PENDING_SYNC = 3,
    ZT_PHASE_FINAL = 4
} zt_wire_phase_t;

typedef enum {
    ZT_TAG_ACCEPTED = 0,
    ZT_TAG_ALREADY_ZOMBIE = 1,
    ZT_TAG_OUT_OF_RANGE = 2,
    ZT_TAG_ROUND_INACTIVE = 3,
    ZT_TAG_STALE_ACTOR = 4,
    ZT_TAG_BUSY = 5,
    ZT_TAG_NOT_ROSTERED = 6,
    ZT_TAG_PENDING = 7
} zt_tag_result_code_t;

typedef enum {
    ZT_JOIN_REGISTERED = 0,
    ZT_JOIN_REJOINED = 1,
    ZT_JOIN_REGISTRATION_CLOSED = 2,
    ZT_JOIN_ROOM_FULL = 3,
    ZT_JOIN_BAD_CONFIGURATION = 4,
    ZT_JOIN_WAITING_FOR_SERVER = 5
} zt_join_status_t;

typedef enum {
    ZT_CMD_PREPARE_ROUND = 1,
    ZT_CMD_START_ROUND = 2,
    ZT_CMD_ROLE_SET = 3,
    ZT_CMD_ANNOUNCE = 4,
    ZT_CMD_END_ROUND = 5,
    ZT_CMD_FINAL_RESULT = 6,
    ZT_CMD_CANCEL_PREPARE = 7,
    ZT_CMD_RESET_GAME = 8 /* Explicit slot; optional MAC[6] + registration ID LE64 for lobby cleanup. */
} zt_command_kind_t;

typedef enum {
    ZT_RECEIPT_RECEIVED = 0,
    ZT_RECEIPT_APPLIED = 1,
    ZT_RECEIPT_PREPARED_READY = 2,
    ZT_RECEIPT_REJECTED = 3,
    ZT_RECEIPT_REQUIRES_SNAPSHOT = 4
} zt_command_receipt_state_t;

typedef enum {
    ZT_DETAIL_NONE = 0,
    ZT_DETAIL_WRONG_ROUND = 1,
    ZT_DETAIL_INVALID_ARGS = 2,
    ZT_DETAIL_STORAGE_FAILURE = 3,
    ZT_DETAIL_MISSING_PAGES = 4,
    ZT_DETAIL_OLD_EVENTS_PENDING = 5,
    ZT_DETAIL_STALE_REVISION = 6
} zt_command_detail_t;

typedef enum {
    ZT_DECISION_ACCEPTED = 0,
    ZT_DECISION_REJECTED = 1,
    ZT_DECISION_PENDING_DEPENDENCY = 2
} zt_decision_status_t;

typedef enum {
    ZT_REASON_NONE = 0,
    ZT_REASON_WRONG_ROUND = 1,
    ZT_REASON_NOT_ROSTERED = 2,
    ZT_REASON_INVALID_PARENT = 3,
    ZT_REASON_OUTSIDE_ROUND = 4,
    ZT_REASON_DUPLICATE_CONFLICT = 5,
    ZT_REASON_STALE_ROLE = 6,
    ZT_REASON_INVALID_PAYLOAD = 7
} zt_decision_reason_t;

typedef enum {
    ZT_END_TIME_LIMIT = 0,
    ZT_END_ALL_INFECTED = 1,
    ZT_END_OPERATOR_STOP = 2
} zt_end_reason_t;

typedef enum {
    ZT_CANCEL_ABSENT_PLAYERS = 0,
    ZT_CANCEL_OPERATOR_CANCEL = 1,
    ZT_CANCEL_INVALID_CHANNEL = 2
} zt_cancel_reason_t;

typedef enum {
    ZT_TIME_UNKNOWN = 0,
    ZT_TIME_INITIALIZED = 1
} zt_time_quality_t;


/* header: encoded 48 bytes. */
#define ZT_HEADER_MAGIC_OFFSET 0
#define ZT_HEADER_MAGIC_BYTES 2
#define ZT_HEADER_PROTOCOL_VERSION_OFFSET 2
#define ZT_HEADER_PROTOCOL_VERSION_BYTES 1
#define ZT_HEADER_TYPE_OFFSET 3
#define ZT_HEADER_TYPE_BYTES 1
#define ZT_HEADER_PAYLOAD_LEN_OFFSET 4
#define ZT_HEADER_PAYLOAD_LEN_BYTES 2
#define ZT_HEADER_FLAGS_OFFSET 6
#define ZT_HEADER_FLAGS_BYTES 1
#define ZT_HEADER_TTL_REMAINING_OFFSET 7
#define ZT_HEADER_TTL_REMAINING_BYTES 1
#define ZT_HEADER_HOPS_OFFSET 8
#define ZT_HEADER_HOPS_BYTES 1
#define ZT_HEADER_RESERVED_OFFSET 9
#define ZT_HEADER_RESERVED_BYTES 1
#define ZT_HEADER_GAME_ID_OFFSET 10
#define ZT_HEADER_GAME_ID_BYTES 8
#define ZT_HEADER_ROUND_ID_OFFSET 18
#define ZT_HEADER_ROUND_ID_BYTES 8
#define ZT_HEADER_ORIGIN_OFFSET 26
#define ZT_HEADER_ORIGIN_BYTES 6
#define ZT_HEADER_ORIGIN_BOOT_NONCE_OFFSET 32
#define ZT_HEADER_ORIGIN_BOOT_NONCE_BYTES 8
#define ZT_HEADER_PACKET_SEQ_OFFSET 40
#define ZT_HEADER_PACKET_SEQ_BYTES 4
#define ZT_HEADER_AGE_MS_OFFSET 44
#define ZT_HEADER_AGE_MS_BYTES 4
/* Envelope size is ZT_WIRE_HEADER_BYTES, defined once above with the other
 * framing constants. Orchestrator integration edit: a second spelling
 * (ZT_HEADER_BYTES) was defined here with the same value. Two names for one
 * frozen wire constant invite drift across the five modules that consume this
 * header, so the duplicate was removed rather than kept as an alias. */
typedef struct {
    uint16_t magic;
    uint8_t protocol_version;
    uint8_t type;
    uint16_t payload_len;
    uint8_t flags;
    uint8_t ttl_remaining;
    uint8_t hops;
    uint8_t reserved;
    uint64_t game_id;
    uint64_t round_id;
    zt_mac_t origin;
    zt_boot_nonce_t origin_boot_nonce;
    uint32_t packet_seq;
    uint32_t age_ms;
} zt_wire_header_t;

/* event: encoded 30 bytes. */
#define ZT_EVENT_VICTIM_SLOT_OFFSET 0
#define ZT_EVENT_VICTIM_SLOT_BYTES 1
#define ZT_EVENT_EVENT_SEQ_OFFSET 1
#define ZT_EVENT_EVENT_SEQ_BYTES 2
#define ZT_EVENT_ACTOR_SLOT_OFFSET 3
#define ZT_EVENT_ACTOR_SLOT_BYTES 1
#define ZT_EVENT_ACTOR_CAUSE_SEQ_OFFSET 4
#define ZT_EVENT_ACTOR_CAUSE_SEQ_BYTES 2
#define ZT_EVENT_ACTOR_ROLE_REV_OFFSET 6
#define ZT_EVENT_ACTOR_ROLE_REV_BYTES 2
#define ZT_EVENT_VICTIM_PRIOR_ROLE_REV_OFFSET 8
#define ZT_EVENT_VICTIM_PRIOR_ROLE_REV_BYTES 2
#define ZT_EVENT_REQUEST_BOOT_OFFSET 10
#define ZT_EVENT_REQUEST_BOOT_BYTES 8
#define ZT_EVENT_REQUEST_SEQ_OFFSET 18
#define ZT_EVENT_REQUEST_SEQ_BYTES 4
#define ZT_EVENT_OCCURRED_ELAPSED_MS_OFFSET 22
#define ZT_EVENT_OCCURRED_ELAPSED_MS_BYTES 4
#define ZT_EVENT_UNCERTAINTY_MS_OFFSET 26
#define ZT_EVENT_UNCERTAINTY_MS_BYTES 2
#define ZT_EVENT_TAGGER_OBSERVED_VICTIM_RSSI_OFFSET 28
#define ZT_EVENT_TAGGER_OBSERVED_VICTIM_RSSI_BYTES 1
#define ZT_EVENT_VICTIM_OBSERVED_TAGGER_RSSI_OFFSET 29
#define ZT_EVENT_VICTIM_OBSERVED_TAGGER_RSSI_BYTES 1
#define ZT_EVENT_BYTES 30
typedef struct {
    uint8_t victim_slot;
    uint16_t event_seq;
    uint8_t actor_slot;
    uint16_t actor_cause_seq;
    uint16_t actor_role_rev;
    uint16_t victim_prior_role_rev;
    zt_boot_nonce_t request_boot;
    uint32_t request_seq;
    uint32_t occurred_elapsed_ms;
    uint16_t uncertainty_ms;
    int8_t tagger_observed_victim_rssi;
    int8_t victim_observed_tagger_rssi;
} zt_wire_event_t;

/* roster_entry: encoded 20 bytes. */
#define ZT_ROSTER_ENTRY_SLOT_OFFSET 0
#define ZT_ROSTER_ENTRY_SLOT_BYTES 1
#define ZT_ROSTER_ENTRY_MAC_OFFSET 1
#define ZT_ROSTER_ENTRY_MAC_BYTES 6
#define ZT_ROSTER_ENTRY_NAME_LEN_OFFSET 7
#define ZT_ROSTER_ENTRY_NAME_LEN_BYTES 1
#define ZT_ROSTER_ENTRY_NAME_OFFSET 8
#define ZT_ROSTER_ENTRY_NAME_BYTES 12
#define ZT_ROSTER_ENTRY_BYTES 20
typedef struct {
    uint8_t slot;
    zt_mac_t mac;
    uint8_t name_len;
    uint8_t name[12];
} zt_wire_roster_entry_t;

/* role_entry: encoded 10 bytes. */
#define ZT_ROLE_ENTRY_SLOT_OFFSET 0
#define ZT_ROLE_ENTRY_SLOT_BYTES 1
#define ZT_ROLE_ENTRY_ROLE_OFFSET 1
#define ZT_ROLE_ENTRY_ROLE_BYTES 1
#define ZT_ROLE_ENTRY_ROLE_REV_OFFSET 2
#define ZT_ROLE_ENTRY_ROLE_REV_BYTES 2
#define ZT_ROLE_ENTRY_CAUSE_SLOT_OFFSET 4
#define ZT_ROLE_ENTRY_CAUSE_SLOT_BYTES 1
#define ZT_ROLE_ENTRY_CAUSE_SEQ_OFFSET 5
#define ZT_ROLE_ENTRY_CAUSE_SEQ_BYTES 2
#define ZT_ROLE_ENTRY_COVERED_SEQ_OFFSET 7
#define ZT_ROLE_ENTRY_COVERED_SEQ_BYTES 2
#define ZT_ROLE_ENTRY_FLAGS_OFFSET 9
#define ZT_ROLE_ENTRY_FLAGS_BYTES 1
#define ZT_ROLE_ENTRY_BYTES 10
typedef struct {
    uint8_t slot;
    uint8_t role;
    uint16_t role_rev;
    uint8_t cause_slot;
    uint16_t cause_seq;
    uint16_t covered_seq;
    uint8_t flags;
} zt_wire_role_entry_t;

/* watermark_entry: encoded 5 bytes. */
#define ZT_WATERMARK_ENTRY_SLOT_OFFSET 0
#define ZT_WATERMARK_ENTRY_SLOT_BYTES 1
#define ZT_WATERMARK_ENTRY_SERVER_RECEIVED_CONTIGUOUS_OFFSET 1
#define ZT_WATERMARK_ENTRY_SERVER_RECEIVED_CONTIGUOUS_BYTES 2
#define ZT_WATERMARK_ENTRY_SERVER_FINAL_CONTIGUOUS_OFFSET 3
#define ZT_WATERMARK_ENTRY_SERVER_FINAL_CONTIGUOUS_BYTES 2
#define ZT_WATERMARK_ENTRY_BYTES 5
typedef struct {
    uint8_t slot;
    uint16_t server_received_contiguous;
    uint16_t server_final_contiguous;
} zt_wire_watermark_entry_t;

/* decision_entry: encoded 6 bytes. */
#define ZT_DECISION_ENTRY_VICTIM_SLOT_OFFSET 0
#define ZT_DECISION_ENTRY_VICTIM_SLOT_BYTES 1
#define ZT_DECISION_ENTRY_EVENT_SEQ_OFFSET 1
#define ZT_DECISION_ENTRY_EVENT_SEQ_BYTES 2
#define ZT_DECISION_ENTRY_STATUS_OFFSET 3
#define ZT_DECISION_ENTRY_STATUS_BYTES 1
#define ZT_DECISION_ENTRY_REASON_OFFSET 4
#define ZT_DECISION_ENTRY_REASON_BYTES 1
#define ZT_DECISION_ENTRY_ARCHIVED_OFFSET 5
#define ZT_DECISION_ENTRY_ARCHIVED_BYTES 1
#define ZT_DECISION_ENTRY_BYTES 6
typedef struct {
    uint8_t victim_slot;
    uint16_t event_seq;
    uint8_t status;
    uint8_t reason;
    uint8_t archived;
} zt_wire_decision_entry_t;

/* cache_key: encoded 3 bytes. */
#define ZT_CACHE_KEY_ORIGIN_SLOT_OFFSET 0
#define ZT_CACHE_KEY_ORIGIN_SLOT_BYTES 1
#define ZT_CACHE_KEY_EVENT_SEQ_OFFSET 1
#define ZT_CACHE_KEY_EVENT_SEQ_BYTES 2
#define ZT_CACHE_KEY_BYTES 3
typedef struct {
    uint8_t origin_slot;
    uint16_t event_seq;
} zt_wire_cache_key_t;

/* beacon: encoded 50 bytes. */
#define ZT_BEACON_SLOT_OFFSET 0
#define ZT_BEACON_SLOT_BYTES 1
#define ZT_BEACON_ROLE_OFFSET 1
#define ZT_BEACON_ROLE_BYTES 1
#define ZT_BEACON_PHASE_OFFSET 2
#define ZT_BEACON_PHASE_BYTES 1
#define ZT_BEACON_FLAGS_OFFSET 3
#define ZT_BEACON_FLAGS_BYTES 1
#define ZT_BEACON_ROLE_REV_OFFSET 4
#define ZT_BEACON_ROLE_REV_BYTES 2
#define ZT_BEACON_INFECTION_CAUSE_SEQ_OFFSET 6
#define ZT_BEACON_INFECTION_CAUSE_SEQ_BYTES 2
#define ZT_BEACON_OWN_PRODUCED_SEQ_OFFSET 8
#define ZT_BEACON_OWN_PRODUCED_SEQ_BYTES 2
#define ZT_BEACON_OWN_SERVER_RECEIVED_OFFSET 10
#define ZT_BEACON_OWN_SERVER_RECEIVED_BYTES 2
#define ZT_BEACON_OWN_SERVER_FINAL_OFFSET 12
#define ZT_BEACON_OWN_SERVER_FINAL_BYTES 2
#define ZT_BEACON_ELAPSED_MS_OFFSET 14
#define ZT_BEACON_ELAPSED_MS_BYTES 4
#define ZT_BEACON_TIME_QUALITY_OFFSET 18
#define ZT_BEACON_TIME_QUALITY_BYTES 1
#define ZT_BEACON_ROUND_CHANNEL_OFFSET 19
#define ZT_BEACON_ROUND_CHANNEL_BYTES 1
#define ZT_BEACON_SNAPSHOT_REV_OFFSET 20
#define ZT_BEACON_SNAPSHOT_REV_BYTES 4
#define ZT_BEACON_GATEWAY_SERIAL_OFFSET 24
#define ZT_BEACON_GATEWAY_SERIAL_BYTES 4
#define ZT_BEACON_GATEWAY_HOPS_OFFSET 28
#define ZT_BEACON_GATEWAY_HOPS_BYTES 1
#define ZT_BEACON_GATEWAY_AGE_S_OFFSET 29
#define ZT_BEACON_GATEWAY_AGE_S_BYTES 1
#define ZT_BEACON_CACHE_DIGEST_OFFSET 30
#define ZT_BEACON_CACHE_DIGEST_BYTES 8
#define ZT_BEACON_CACHE_COUNT_OFFSET 38
#define ZT_BEACON_CACHE_COUNT_BYTES 2
#define ZT_BEACON_UNCERTAINTY_MS_OFFSET 40
#define ZT_BEACON_UNCERTAINTY_MS_BYTES 2
#define ZT_BEACON_GATEWAY_BOOT_NONCE_OFFSET 42
#define ZT_BEACON_GATEWAY_BOOT_NONCE_BYTES 8
#define ZT_BEACON_BYTES 50
typedef struct {
    uint8_t slot;
    uint8_t role;
    uint8_t phase;
    uint8_t flags;
    uint16_t role_rev;
    uint16_t infection_cause_seq;
    uint16_t own_produced_seq;
    uint16_t own_server_received;
    uint16_t own_server_final;
    int32_t elapsed_ms;
    uint8_t time_quality;
    uint8_t round_channel;
    uint32_t snapshot_rev;
    uint32_t gateway_serial;
    uint8_t gateway_hops;
    uint8_t gateway_age_s;
    uint64_t cache_digest;
    uint16_t cache_count;
    uint16_t uncertainty_ms;
    zt_boot_nonce_t gateway_boot_nonce;
} zt_wire_beacon_t;

/* tag_request: encoded 17 bytes. */
#define ZT_TAG_REQUEST_VICTIM_SLOT_OFFSET 0
#define ZT_TAG_REQUEST_VICTIM_SLOT_BYTES 1
#define ZT_TAG_REQUEST_ACTOR_SLOT_OFFSET 1
#define ZT_TAG_REQUEST_ACTOR_SLOT_BYTES 1
#define ZT_TAG_REQUEST_ACTOR_CAUSE_SEQ_OFFSET 2
#define ZT_TAG_REQUEST_ACTOR_CAUSE_SEQ_BYTES 2
#define ZT_TAG_REQUEST_ACTOR_ROLE_REV_OFFSET 4
#define ZT_TAG_REQUEST_ACTOR_ROLE_REV_BYTES 2
#define ZT_TAG_REQUEST_KNOWN_VICTIM_ROLE_REV_OFFSET 6
#define ZT_TAG_REQUEST_KNOWN_VICTIM_ROLE_REV_BYTES 2
#define ZT_TAG_REQUEST_ATTEMPT_SEQ_OFFSET 8
#define ZT_TAG_REQUEST_ATTEMPT_SEQ_BYTES 4
#define ZT_TAG_REQUEST_ACTOR_ELAPSED_MS_OFFSET 12
#define ZT_TAG_REQUEST_ACTOR_ELAPSED_MS_BYTES 4
#define ZT_TAG_REQUEST_TAGGER_OBSERVED_VICTIM_RSSI_OFFSET 16
#define ZT_TAG_REQUEST_TAGGER_OBSERVED_VICTIM_RSSI_BYTES 1
#define ZT_TAG_REQUEST_BYTES 17
typedef struct {
    uint8_t victim_slot;
    uint8_t actor_slot;
    uint16_t actor_cause_seq;
    uint16_t actor_role_rev;
    uint16_t known_victim_role_rev;
    uint32_t attempt_seq;
    uint32_t actor_elapsed_ms;
    int8_t tagger_observed_victim_rssi;
} zt_wire_tag_request_t;

/* tag_result: encoded 44 bytes. */
#define ZT_TAG_RESULT_ACTOR_SLOT_OFFSET 0
#define ZT_TAG_RESULT_ACTOR_SLOT_BYTES 1
#define ZT_TAG_RESULT_REQUEST_BOOT_OFFSET 1
#define ZT_TAG_RESULT_REQUEST_BOOT_BYTES 8
#define ZT_TAG_RESULT_REQUEST_SEQ_OFFSET 9
#define ZT_TAG_RESULT_REQUEST_SEQ_BYTES 4
#define ZT_TAG_RESULT_RESULT_OFFSET 13
#define ZT_TAG_RESULT_RESULT_BYTES 1
#define ZT_TAG_RESULT_ACCEPTED_EVENT_OFFSET 14
#define ZT_TAG_RESULT_ACCEPTED_EVENT_BYTES 30
#define ZT_TAG_RESULT_BYTES 44
typedef struct {
    uint8_t actor_slot;
    zt_boot_nonce_t request_boot;
    uint32_t request_seq;
    uint8_t result;
    zt_wire_event_t accepted_event;
} zt_wire_tag_result_t;

/* join: encoded 39 bytes. */
#define ZT_JOIN_REQUESTED_BADGE_MAC_OFFSET 0
#define ZT_JOIN_REQUESTED_BADGE_MAC_BYTES 6
#define ZT_JOIN_NAME_LEN_OFFSET 6
#define ZT_JOIN_NAME_LEN_BYTES 1
#define ZT_JOIN_NAME_OFFSET 7
#define ZT_JOIN_NAME_BYTES 12
#define ZT_JOIN_KNOWN_ROUND_ID_OFFSET 19
#define ZT_JOIN_KNOWN_ROUND_ID_BYTES 8
#define ZT_JOIN_REQUEST_NONCE_OFFSET 27
#define ZT_JOIN_REQUEST_NONCE_BYTES 4
#define ZT_JOIN_BUILD_ID_OFFSET 31
#define ZT_JOIN_BUILD_ID_BYTES 8
#define ZT_JOIN_BYTES 39
typedef struct {
    zt_mac_t requested_badge_mac;
    uint8_t name_len;
    uint8_t name[12];
    uint64_t known_round_id;
    uint32_t request_nonce;
    uint8_t build_id[8];
} zt_wire_join_t;

/* join_result: encoded 16 bytes. */
#define ZT_JOIN_RESULT_TARGET_MAC_OFFSET 0
#define ZT_JOIN_RESULT_TARGET_MAC_BYTES 6
#define ZT_JOIN_RESULT_REQUEST_NONCE_OFFSET 6
#define ZT_JOIN_RESULT_REQUEST_NONCE_BYTES 4
#define ZT_JOIN_RESULT_STATUS_OFFSET 10
#define ZT_JOIN_RESULT_STATUS_BYTES 1
#define ZT_JOIN_RESULT_SLOT_OFFSET 11
#define ZT_JOIN_RESULT_SLOT_BYTES 1
#define ZT_JOIN_RESULT_SNAPSHOT_REV_OFFSET 12
#define ZT_JOIN_RESULT_SNAPSHOT_REV_BYTES 4
#define ZT_JOIN_RESULT_BYTES 16
typedef struct {
    zt_mac_t target_mac;
    uint32_t request_nonce;
    uint8_t status;
    uint8_t slot;
    uint32_t snapshot_rev;
} zt_wire_join_result_t;

/* roster_page: encoded maximum 175 bytes. */
#define ZT_ROSTER_PAGE_SNAPSHOT_REV_OFFSET 0
#define ZT_ROSTER_PAGE_SNAPSHOT_REV_BYTES 4
#define ZT_ROSTER_PAGE_ROSTER_HASH_OFFSET 4
#define ZT_ROSTER_PAGE_ROSTER_HASH_BYTES 8
#define ZT_ROSTER_PAGE_PAGE_INDEX_OFFSET 12
#define ZT_ROSTER_PAGE_PAGE_INDEX_BYTES 1
#define ZT_ROSTER_PAGE_PAGE_COUNT_OFFSET 13
#define ZT_ROSTER_PAGE_PAGE_COUNT_BYTES 1
#define ZT_ROSTER_PAGE_ENTRY_COUNT_OFFSET 14
#define ZT_ROSTER_PAGE_ENTRY_COUNT_BYTES 1
#define ZT_ROSTER_PAGE_ENTRIES_OFFSET 15
#define ZT_ROSTER_PAGE_ENTRIES_ENTRY_BYTES 20
#define ZT_ROSTER_PAGE_MAX_BYTES 175
typedef struct {
    uint32_t snapshot_rev;
    uint64_t roster_hash;
    uint8_t page_index;
    uint8_t page_count;
    uint8_t entry_count;
    zt_wire_roster_entry_t entries[8];
} zt_wire_roster_page_t;

/* host_state: encoded maximum 160 bytes. The optional gateway_flags trailer
 * was added for live server status. Legacy 39+10n-byte messages decode with
 * no server-connectivity evidence; all original field offsets stay unchanged. */
#define ZT_HOST_STATE_SNAPSHOT_REV_OFFSET 0
#define ZT_HOST_STATE_SNAPSHOT_REV_BYTES 4
#define ZT_HOST_STATE_ROSTER_HASH_OFFSET 4
#define ZT_HOST_STATE_ROSTER_HASH_BYTES 8
#define ZT_HOST_STATE_PHASE_OFFSET 12
#define ZT_HOST_STATE_PHASE_BYTES 1
#define ZT_HOST_STATE_PAGE_INDEX_OFFSET 13
#define ZT_HOST_STATE_PAGE_INDEX_BYTES 1
#define ZT_HOST_STATE_PAGE_COUNT_OFFSET 14
#define ZT_HOST_STATE_PAGE_COUNT_BYTES 1
#define ZT_HOST_STATE_ENTRY_COUNT_OFFSET 15
#define ZT_HOST_STATE_ENTRY_COUNT_BYTES 1
#define ZT_HOST_STATE_HOST_ELAPSED_MS_OFFSET 16
#define ZT_HOST_STATE_HOST_ELAPSED_MS_BYTES 4
#define ZT_HOST_STATE_RULE_REV_OFFSET 20
#define ZT_HOST_STATE_RULE_REV_BYTES 2
#define ZT_HOST_STATE_ROUND_CHANNEL_OFFSET 22
#define ZT_HOST_STATE_ROUND_CHANNEL_BYTES 1
#define ZT_HOST_STATE_WINNER_OFFSET 23
#define ZT_HOST_STATE_WINNER_BYTES 1
#define ZT_HOST_STATE_DURATION_MS_OFFSET 24
#define ZT_HOST_STATE_DURATION_MS_BYTES 4
#define ZT_HOST_STATE_REMAINING_MS_OFFSET 28
#define ZT_HOST_STATE_REMAINING_MS_BYTES 4
#define ZT_HOST_STATE_PATIENT_ZERO_SLOT_OFFSET 32
#define ZT_HOST_STATE_PATIENT_ZERO_SLOT_BYTES 1
#define ZT_HOST_STATE_GATEWAY_SERIAL_OFFSET 33
#define ZT_HOST_STATE_GATEWAY_SERIAL_BYTES 4
#define ZT_HOST_STATE_ENTRIES_OFFSET 37
#define ZT_HOST_STATE_ENTRIES_ENTRY_BYTES 10
/* uncertainty_ms follows the counted entries; its offset is variable. */
#define ZT_HOST_STATE_UNCERTAINTY_MS_BYTES 2
#define ZT_HOST_STATE_GATEWAY_FLAGS_BYTES 1
#define ZT_HOST_STATE_MAX_BYTES 160
typedef struct {
    uint32_t snapshot_rev;
    uint64_t roster_hash;
    uint8_t phase;
    uint8_t page_index;
    uint8_t page_count;
    uint8_t entry_count;
    int32_t host_elapsed_ms;
    uint16_t rule_rev;
    uint8_t round_channel;
    uint8_t winner;
    uint32_t duration_ms;
    uint32_t remaining_ms;
    uint8_t patient_zero_slot;
    uint32_t gateway_serial;
    zt_wire_role_entry_t entries[12];
    uint16_t uncertainty_ms;
    uint8_t gateway_flags;
} zt_wire_host_state_t;

/* watermarks: encoded maximum 113 bytes. */
#define ZT_WATERMARKS_SNAPSHOT_REV_OFFSET 0
#define ZT_WATERMARKS_SNAPSHOT_REV_BYTES 4
#define ZT_WATERMARKS_ROSTER_HASH_OFFSET 4
#define ZT_WATERMARKS_ROSTER_HASH_BYTES 8
#define ZT_WATERMARKS_COUNT_OFFSET 12
#define ZT_WATERMARKS_COUNT_BYTES 1
#define ZT_WATERMARKS_ENTRIES_OFFSET 13
#define ZT_WATERMARKS_ENTRIES_ENTRY_BYTES 5
#define ZT_WATERMARKS_MAX_BYTES 113
typedef struct {
    uint32_t snapshot_rev;
    uint64_t roster_hash;
    uint8_t count;
    zt_wire_watermark_entry_t entries[20];
} zt_wire_watermarks_t;

/* command: encoded maximum 186 bytes. */
#define ZT_COMMAND_COMMAND_SEQ_OFFSET 0
#define ZT_COMMAND_COMMAND_SEQ_BYTES 4
#define ZT_COMMAND_KIND_OFFSET 4
#define ZT_COMMAND_KIND_BYTES 1
#define ZT_COMMAND_TARGET_SLOT_OFFSET 5
#define ZT_COMMAND_TARGET_SLOT_BYTES 1
#define ZT_COMMAND_VALID_UNTIL_ELAPSED_MS_OFFSET 6
#define ZT_COMMAND_VALID_UNTIL_ELAPSED_MS_BYTES 4
#define ZT_COMMAND_ARGS_LEN_OFFSET 10
#define ZT_COMMAND_ARGS_LEN_BYTES 1
#define ZT_COMMAND_ARGS_OFFSET 11
#define ZT_COMMAND_ARGS_ENTRY_BYTES 1
#define ZT_COMMAND_MAX_BYTES 186
typedef struct {
    uint32_t command_seq;
    uint8_t kind;
    uint8_t target_slot;
    uint32_t valid_until_elapsed_ms;
    uint8_t args_len;
    uint8_t args[175];
} zt_wire_command_t;

/* command_receipt: encoded 12 bytes. */
#define ZT_COMMAND_RECEIPT_SLOT_OFFSET 0
#define ZT_COMMAND_RECEIPT_SLOT_BYTES 1
#define ZT_COMMAND_RECEIPT_COMMAND_SEQ_OFFSET 1
#define ZT_COMMAND_RECEIPT_COMMAND_SEQ_BYTES 4
#define ZT_COMMAND_RECEIPT_STATE_OFFSET 5
#define ZT_COMMAND_RECEIPT_STATE_BYTES 1
#define ZT_COMMAND_RECEIPT_DETAIL_OFFSET 6
#define ZT_COMMAND_RECEIPT_DETAIL_BYTES 2
#define ZT_COMMAND_RECEIPT_APPLIED_SNAPSHOT_REV_OFFSET 8
#define ZT_COMMAND_RECEIPT_APPLIED_SNAPSHOT_REV_BYTES 4
#define ZT_COMMAND_RECEIPT_BYTES 12
typedef struct {
    uint8_t slot;
    uint32_t command_seq;
    uint8_t state;
    uint16_t detail;
    uint32_t applied_snapshot_rev;
} zt_wire_command_receipt_t;

/* snapshot_request: encoded 14 bytes. */
#define ZT_SNAPSHOT_REQUEST_SLOT_OFFSET 0
#define ZT_SNAPSHOT_REQUEST_SLOT_BYTES 1
#define ZT_SNAPSHOT_REQUEST_WANTED_SNAPSHOT_REV_OFFSET 1
#define ZT_SNAPSHOT_REQUEST_WANTED_SNAPSHOT_REV_BYTES 4
#define ZT_SNAPSHOT_REQUEST_ROSTER_HASH_OFFSET 5
#define ZT_SNAPSHOT_REQUEST_ROSTER_HASH_BYTES 8
#define ZT_SNAPSHOT_REQUEST_NEED_FLAGS_OFFSET 13
#define ZT_SNAPSHOT_REQUEST_NEED_FLAGS_BYTES 1
#define ZT_SNAPSHOT_REQUEST_BYTES 14
typedef struct {
    uint8_t slot;
    uint32_t wanted_snapshot_rev;
    uint64_t roster_hash;
    uint8_t need_flags;
} zt_wire_snapshot_request_t;

/* round_closed: encoded 9 bytes. */
#define ZT_ROUND_CLOSED_SLOT_OFFSET 0
#define ZT_ROUND_CLOSED_SLOT_BYTES 1
#define ZT_ROUND_CLOSED_PRODUCED_SEQ_OFFSET 1
#define ZT_ROUND_CLOSED_PRODUCED_SEQ_BYTES 2
#define ZT_ROUND_CLOSED_LAST_ROLE_REV_OFFSET 3
#define ZT_ROUND_CLOSED_LAST_ROLE_REV_BYTES 2
#define ZT_ROUND_CLOSED_CLOSE_ELAPSED_MS_OFFSET 5
#define ZT_ROUND_CLOSED_CLOSE_ELAPSED_MS_BYTES 4
#define ZT_ROUND_CLOSED_BYTES 9
typedef struct {
    uint8_t slot;
    uint16_t produced_seq;
    uint16_t last_role_rev;
    uint32_t close_elapsed_ms;
} zt_wire_round_closed_t;

/* close_receipts: encoded 8 bytes. */
#define ZT_CLOSE_RECEIPTS_CLOSED_BITMAP_OFFSET 0
#define ZT_CLOSE_RECEIPTS_CLOSED_BITMAP_BYTES 4
#define ZT_CLOSE_RECEIPTS_ARCHIVED_BITMAP_OFFSET 4
#define ZT_CLOSE_RECEIPTS_ARCHIVED_BITMAP_BYTES 4
#define ZT_CLOSE_RECEIPTS_BYTES 8
typedef struct {
    uint32_t closed_bitmap;
    uint32_t archived_bitmap;
} zt_wire_close_receipts_t;

/* event_decisions: encoded maximum 125 bytes. */
#define ZT_EVENT_DECISIONS_SNAPSHOT_REV_OFFSET 0
#define ZT_EVENT_DECISIONS_SNAPSHOT_REV_BYTES 4
#define ZT_EVENT_DECISIONS_COUNT_OFFSET 4
#define ZT_EVENT_DECISIONS_COUNT_BYTES 1
#define ZT_EVENT_DECISIONS_ENTRIES_OFFSET 5
#define ZT_EVENT_DECISIONS_ENTRIES_ENTRY_BYTES 6
#define ZT_EVENT_DECISIONS_MAX_BYTES 125
typedef struct {
    uint32_t snapshot_rev;
    uint8_t count;
    zt_wire_decision_entry_t entries[20];
} zt_wire_event_decisions_t;

/* decision_receipt: encoded 3 bytes. */
#define ZT_DECISION_RECEIPT_SLOT_OFFSET 0
#define ZT_DECISION_RECEIPT_SLOT_BYTES 1
#define ZT_DECISION_RECEIPT_OWN_DECIDED_CONTIGUOUS_OFFSET 1
#define ZT_DECISION_RECEIPT_OWN_DECIDED_CONTIGUOUS_BYTES 2
#define ZT_DECISION_RECEIPT_BYTES 3
typedef struct {
    uint8_t slot;
    uint16_t own_decided_contiguous;
} zt_wire_decision_receipt_t;

/* cache_page: encoded maximum 180 bytes. */
#define ZT_CACHE_PAGE_TARGET_SLOT_OFFSET 0
#define ZT_CACHE_PAGE_TARGET_SLOT_BYTES 1
#define ZT_CACHE_PAGE_CACHE_DIGEST_OFFSET 1
#define ZT_CACHE_PAGE_CACHE_DIGEST_BYTES 8
#define ZT_CACHE_PAGE_PAGE_INDEX_OFFSET 9
#define ZT_CACHE_PAGE_PAGE_INDEX_BYTES 1
#define ZT_CACHE_PAGE_PAGE_COUNT_OFFSET 10
#define ZT_CACHE_PAGE_PAGE_COUNT_BYTES 1
#define ZT_CACHE_PAGE_COUNT_OFFSET 11
#define ZT_CACHE_PAGE_COUNT_BYTES 1
#define ZT_CACHE_PAGE_ENTRIES_OFFSET 12
#define ZT_CACHE_PAGE_ENTRIES_ENTRY_BYTES 3
#define ZT_CACHE_PAGE_MAX_BYTES 180
typedef struct {
    uint8_t target_slot;
    uint64_t cache_digest;
    uint8_t page_index;
    uint8_t page_count;
    uint8_t count;
    zt_wire_cache_key_t entries[56];
} zt_wire_cache_page_t;

/* want_events: encoded maximum 52 bytes. */
#define ZT_WANT_EVENTS_TARGET_SLOT_OFFSET 0
#define ZT_WANT_EVENTS_TARGET_SLOT_BYTES 1
#define ZT_WANT_EVENTS_REQUEST_SEQ_OFFSET 1
#define ZT_WANT_EVENTS_REQUEST_SEQ_BYTES 2
#define ZT_WANT_EVENTS_COUNT_OFFSET 3
#define ZT_WANT_EVENTS_COUNT_BYTES 1
#define ZT_WANT_EVENTS_ENTRIES_OFFSET 4
#define ZT_WANT_EVENTS_ENTRIES_ENTRY_BYTES 3
#define ZT_WANT_EVENTS_MAX_BYTES 52
typedef struct {
    uint8_t target_slot;
    uint16_t request_seq;
    uint8_t count;
    zt_wire_cache_key_t entries[16];
} zt_wire_want_events_t;

/* event_copy: encoded 31 bytes. */
#define ZT_EVENT_COPY_TARGET_SLOT_OFFSET 0
#define ZT_EVENT_COPY_TARGET_SLOT_BYTES 1
#define ZT_EVENT_COPY_EVENT_OFFSET 1
#define ZT_EVENT_COPY_EVENT_BYTES 30
#define ZT_EVENT_COPY_BYTES 31
typedef struct {
    uint8_t target_slot;
    zt_wire_event_t event;
} zt_wire_event_copy_t;

/* time_query: encoded 5 bytes. */
#define ZT_TIME_QUERY_TARGET_SLOT_OFFSET 0
#define ZT_TIME_QUERY_TARGET_SLOT_BYTES 1
#define ZT_TIME_QUERY_REQUEST_NONCE_OFFSET 1
#define ZT_TIME_QUERY_REQUEST_NONCE_BYTES 4
#define ZT_TIME_QUERY_BYTES 5
typedef struct {
    uint8_t target_slot;
    uint32_t request_nonce;
} zt_wire_time_query_t;

/* time_reply: encoded 20 bytes. */
#define ZT_TIME_REPLY_TARGET_SLOT_OFFSET 0
#define ZT_TIME_REPLY_TARGET_SLOT_BYTES 1
#define ZT_TIME_REPLY_REQUEST_NONCE_OFFSET 1
#define ZT_TIME_REPLY_REQUEST_NONCE_BYTES 4
#define ZT_TIME_REPLY_SAMPLED_ELAPSED_MS_OFFSET 5
#define ZT_TIME_REPLY_SAMPLED_ELAPSED_MS_BYTES 4
#define ZT_TIME_REPLY_SOURCE_SNAPSHOT_REV_OFFSET 9
#define ZT_TIME_REPLY_SOURCE_SNAPSHOT_REV_BYTES 4
#define ZT_TIME_REPLY_SOURCE_AGE_MS_OFFSET 13
#define ZT_TIME_REPLY_SOURCE_AGE_MS_BYTES 4
#define ZT_TIME_REPLY_UNCERTAINTY_MS_OFFSET 17
#define ZT_TIME_REPLY_UNCERTAINTY_MS_BYTES 2
#define ZT_TIME_REPLY_TIME_QUALITY_OFFSET 19
#define ZT_TIME_REPLY_TIME_QUALITY_BYTES 1
#define ZT_TIME_REPLY_BYTES 20
typedef struct {
    uint8_t target_slot;
    uint32_t request_nonce;
    int32_t sampled_elapsed_ms;
    uint32_t source_snapshot_rev;
    uint32_t source_age_ms;
    uint16_t uncertainty_ms;
    uint8_t time_quality;
} zt_wire_time_reply_t;

/* prepare_round_args: encoded 21 bytes. */
#define ZT_PREPARE_ROUND_ARGS_SNAPSHOT_REV_OFFSET 0
#define ZT_PREPARE_ROUND_ARGS_SNAPSHOT_REV_BYTES 4
#define ZT_PREPARE_ROUND_ARGS_ROSTER_HASH_OFFSET 4
#define ZT_PREPARE_ROUND_ARGS_ROSTER_HASH_BYTES 8
#define ZT_PREPARE_ROUND_ARGS_ROSTER_COUNT_OFFSET 12
#define ZT_PREPARE_ROUND_ARGS_ROSTER_COUNT_BYTES 1
#define ZT_PREPARE_ROUND_ARGS_DURATION_MS_OFFSET 13
#define ZT_PREPARE_ROUND_ARGS_DURATION_MS_BYTES 4
#define ZT_PREPARE_ROUND_ARGS_CHANNEL_OFFSET 17
#define ZT_PREPARE_ROUND_ARGS_CHANNEL_BYTES 1
#define ZT_PREPARE_ROUND_ARGS_TAG_RSSI_OFFSET 18
#define ZT_PREPARE_ROUND_ARGS_TAG_RSSI_BYTES 1
#define ZT_PREPARE_ROUND_ARGS_TAG_COOLDOWN_MS_OFFSET 19
#define ZT_PREPARE_ROUND_ARGS_TAG_COOLDOWN_MS_BYTES 2
#define ZT_PREPARE_ROUND_ARGS_BYTES 21
typedef struct {
    uint32_t snapshot_rev;
    uint64_t roster_hash;
    uint8_t roster_count;
    uint32_t duration_ms;
    uint8_t channel;
    int8_t tag_rssi;
    uint16_t tag_cooldown_ms;
} zt_wire_prepare_round_args_t;

/* start_round_args: encoded 25 bytes. */
#define ZT_START_ROUND_ARGS_SNAPSHOT_REV_OFFSET 0
#define ZT_START_ROUND_ARGS_SNAPSHOT_REV_BYTES 4
#define ZT_START_ROUND_ARGS_ROSTER_HASH_OFFSET 4
#define ZT_START_ROUND_ARGS_ROSTER_HASH_BYTES 8
#define ZT_START_ROUND_ARGS_PATIENT_ZERO_SLOT_OFFSET 12
#define ZT_START_ROUND_ARGS_PATIENT_ZERO_SLOT_BYTES 1
#define ZT_START_ROUND_ARGS_INITIAL_ROLE_REV_OFFSET 13
#define ZT_START_ROUND_ARGS_INITIAL_ROLE_REV_BYTES 2
#define ZT_START_ROUND_ARGS_SAMPLED_ELAPSED_MS_OFFSET 15
#define ZT_START_ROUND_ARGS_SAMPLED_ELAPSED_MS_BYTES 4
#define ZT_START_ROUND_ARGS_DURATION_MS_OFFSET 19
#define ZT_START_ROUND_ARGS_DURATION_MS_BYTES 4
#define ZT_START_ROUND_ARGS_UNCERTAINTY_MS_OFFSET 23
#define ZT_START_ROUND_ARGS_UNCERTAINTY_MS_BYTES 2
#define ZT_START_ROUND_ARGS_BYTES 25
typedef struct {
    uint32_t snapshot_rev;
    uint64_t roster_hash;
    uint8_t patient_zero_slot;
    uint16_t initial_role_rev;
    int32_t sampled_elapsed_ms;
    uint32_t duration_ms;
    uint16_t uncertainty_ms;
} zt_wire_start_round_args_t;

/* role_set_args: encoded 8 bytes. */
#define ZT_ROLE_SET_ARGS_ROLE_OFFSET 0
#define ZT_ROLE_SET_ARGS_ROLE_BYTES 1
#define ZT_ROLE_SET_ARGS_ROLE_REV_OFFSET 1
#define ZT_ROLE_SET_ARGS_ROLE_REV_BYTES 2
#define ZT_ROLE_SET_ARGS_CAUSE_SLOT_OFFSET 3
#define ZT_ROLE_SET_ARGS_CAUSE_SLOT_BYTES 1
#define ZT_ROLE_SET_ARGS_CAUSE_SEQ_OFFSET 4
#define ZT_ROLE_SET_ARGS_CAUSE_SEQ_BYTES 2
#define ZT_ROLE_SET_ARGS_COVERED_SEQ_OFFSET 6
#define ZT_ROLE_SET_ARGS_COVERED_SEQ_BYTES 2
#define ZT_ROLE_SET_ARGS_BYTES 8
typedef struct {
    uint8_t role;
    uint16_t role_rev;
    uint8_t cause_slot;
    uint16_t cause_seq;
    uint16_t covered_seq;
} zt_wire_role_set_args_t;

/* announce_args: encoded maximum 97 bytes. */
#define ZT_ANNOUNCE_ARGS_TEXT_LEN_OFFSET 0
#define ZT_ANNOUNCE_ARGS_TEXT_LEN_BYTES 1
#define ZT_ANNOUNCE_ARGS_TEXT_OFFSET 1
#define ZT_ANNOUNCE_ARGS_TEXT_ENTRY_BYTES 1
#define ZT_ANNOUNCE_ARGS_MAX_BYTES 97
typedef struct {
    uint8_t text_len;
    uint8_t text[96];
} zt_wire_announce_args_t;

/* end_round_args: encoded 7 bytes. */
#define ZT_END_ROUND_ARGS_EFFECTIVE_ELAPSED_MS_OFFSET 0
#define ZT_END_ROUND_ARGS_EFFECTIVE_ELAPSED_MS_BYTES 4
#define ZT_END_ROUND_ARGS_REASON_OFFSET 4
#define ZT_END_ROUND_ARGS_REASON_BYTES 1
#define ZT_END_ROUND_ARGS_WINNER_OFFSET 5
#define ZT_END_ROUND_ARGS_WINNER_BYTES 1
#define ZT_END_ROUND_ARGS_PROVISIONAL_OFFSET 6
#define ZT_END_ROUND_ARGS_PROVISIONAL_BYTES 1
#define ZT_END_ROUND_ARGS_BYTES 7
typedef struct {
    uint32_t effective_elapsed_ms;
    uint8_t reason;
    uint8_t winner;
    uint8_t provisional;
} zt_wire_end_round_args_t;

/* final_result_args: encoded 10 bytes. */
#define ZT_FINAL_RESULT_ARGS_WINNER_OFFSET 0
#define ZT_FINAL_RESULT_ARGS_WINNER_BYTES 1
#define ZT_FINAL_RESULT_ARGS_COMPLETE_OFFSET 1
#define ZT_FINAL_RESULT_ARGS_COMPLETE_BYTES 1
#define ZT_FINAL_RESULT_ARGS_MISSING_SLOTS_BITMAP_OFFSET 2
#define ZT_FINAL_RESULT_ARGS_MISSING_SLOTS_BITMAP_BYTES 4
#define ZT_FINAL_RESULT_ARGS_STATE_REV_OFFSET 6
#define ZT_FINAL_RESULT_ARGS_STATE_REV_BYTES 4
#define ZT_FINAL_RESULT_ARGS_BYTES 10
typedef struct {
    uint8_t winner;
    uint8_t complete;
    uint32_t missing_slots_bitmap;
    uint32_t state_rev;
} zt_wire_final_result_args_t;

/* cancel_prepare_args: encoded 1 bytes. */
#define ZT_CANCEL_PREPARE_ARGS_REASON_OFFSET 0
#define ZT_CANCEL_PREPARE_ARGS_REASON_BYTES 1
#define ZT_CANCEL_PREPARE_ARGS_BYTES 1
typedef struct {
    uint8_t reason;
} zt_wire_cancel_prepare_args_t;

#define ZT_ANNOUNCE_ARGS_MIN_BYTES 2
#define ZT_ROSTER_PAGE_BASE_BYTES 15
#define ZT_HOST_STATE_LEGACY_BASE_BYTES 39
#define ZT_HOST_STATE_BASE_BYTES 40
#define ZT_HOST_STATE_UNCERTAINTY_OFFSET(count) (37u + 10u * (count))
#define ZT_HOST_STATE_GATEWAY_FLAGS_OFFSET(count) (39u + 10u * (count))
#define ZT_WATERMARKS_BASE_BYTES 13
#define ZT_COMMAND_BASE_BYTES 11
#define ZT_EVENT_DECISIONS_BASE_BYTES 5
#define ZT_CACHE_PAGE_BASE_BYTES 12
#define ZT_WANT_EVENTS_BASE_BYTES 4
/* Roster hash: first eight SHA256 bytes of slot-sorted canonical roster entries.
 * Cache digest: same prefix over sorted (slot u8, seq u16 LE), PER ROUND;
 * empty set hashes the empty sequence. Hash prefix interpreted as LE u64.
 * Names: 1..12 printable ASCII, fixed fields padded with zero, no embedded NUL.
 * TAG_RESULT rejected event body is all zero. HOST_STATE zero pages is clock-only.
 * Header flags not named above, reserved byte, and all unused flag bits are zero.
 * Received and final watermarks are contiguous; pending dependency never final.
 * Origin/boot/packet_seq dedupes envelopes; round/victim/event_seq dedupes events.
 * No role/event/transport counter wraps within its scope. Host-only authority is
 * checked against envelope origin, NOT the last relay's SDK source MAC.
 * Group HMAC authenticates group membership, not malicious-member-proof identity. */
typedef union {
    zt_wire_beacon_t beacon;
    zt_wire_tag_request_t tag_request;
    zt_wire_tag_result_t tag_result;
    zt_wire_join_t join;
    zt_wire_join_result_t join_result;
    zt_wire_event_t event;
    zt_wire_roster_page_t roster_page;
    zt_wire_host_state_t host_state;
    zt_wire_watermarks_t watermarks;
    zt_wire_command_t command;
    zt_wire_command_receipt_t command_receipt;
    zt_wire_snapshot_request_t snapshot_request;
    zt_wire_round_closed_t round_closed;
    zt_wire_close_receipts_t close_receipts;
    zt_wire_event_decisions_t event_decisions;
    zt_wire_decision_receipt_t decision_receipt;
    zt_wire_cache_page_t cache_page;
    zt_wire_want_events_t want_events;
    zt_wire_event_copy_t event_copy;
    zt_wire_time_query_t time_query;
    zt_wire_time_reply_t time_reply;
} zt_wire_payload_t;
zt_err_t zt_wire_encode_event(const zt_wire_event_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_event(const uint8_t *buf, size_t len, zt_wire_event_t *out);
zt_err_t zt_wire_encode_roster_entry(const zt_wire_roster_entry_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_roster_entry(const uint8_t *buf, size_t len, zt_wire_roster_entry_t *out);
zt_err_t zt_wire_encode_role_entry(const zt_wire_role_entry_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_role_entry(const uint8_t *buf, size_t len, zt_wire_role_entry_t *out);
zt_err_t zt_wire_encode_watermark_entry(const zt_wire_watermark_entry_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_watermark_entry(const uint8_t *buf, size_t len, zt_wire_watermark_entry_t *out);
zt_err_t zt_wire_encode_decision_entry(const zt_wire_decision_entry_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_decision_entry(const uint8_t *buf, size_t len, zt_wire_decision_entry_t *out);
zt_err_t zt_wire_encode_cache_key(const zt_wire_cache_key_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_cache_key(const uint8_t *buf, size_t len, zt_wire_cache_key_t *out);
zt_err_t zt_wire_encode_beacon(const zt_wire_beacon_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_beacon(const uint8_t *buf, size_t len, zt_wire_beacon_t *out);
zt_err_t zt_wire_encode_tag_request(const zt_wire_tag_request_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_tag_request(const uint8_t *buf, size_t len, zt_wire_tag_request_t *out);
zt_err_t zt_wire_encode_tag_result(const zt_wire_tag_result_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_tag_result(const uint8_t *buf, size_t len, zt_wire_tag_result_t *out);
zt_err_t zt_wire_encode_join(const zt_wire_join_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_join(const uint8_t *buf, size_t len, zt_wire_join_t *out);
zt_err_t zt_wire_encode_join_result(const zt_wire_join_result_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_join_result(const uint8_t *buf, size_t len, zt_wire_join_result_t *out);
zt_err_t zt_wire_encode_roster_page(const zt_wire_roster_page_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_roster_page(const uint8_t *buf, size_t len, zt_wire_roster_page_t *out);
zt_err_t zt_wire_encode_host_state(const zt_wire_host_state_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_host_state(const uint8_t *buf, size_t len, zt_wire_host_state_t *out);
zt_err_t zt_wire_encode_watermarks(const zt_wire_watermarks_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_watermarks(const uint8_t *buf, size_t len, zt_wire_watermarks_t *out);
zt_err_t zt_wire_encode_command(const zt_wire_command_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_command(const uint8_t *buf, size_t len, zt_wire_command_t *out);
zt_err_t zt_wire_encode_command_receipt(const zt_wire_command_receipt_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_command_receipt(const uint8_t *buf, size_t len, zt_wire_command_receipt_t *out);
zt_err_t zt_wire_encode_snapshot_request(const zt_wire_snapshot_request_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_snapshot_request(const uint8_t *buf, size_t len, zt_wire_snapshot_request_t *out);
zt_err_t zt_wire_encode_round_closed(const zt_wire_round_closed_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_round_closed(const uint8_t *buf, size_t len, zt_wire_round_closed_t *out);
zt_err_t zt_wire_encode_close_receipts(const zt_wire_close_receipts_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_close_receipts(const uint8_t *buf, size_t len, zt_wire_close_receipts_t *out);
zt_err_t zt_wire_encode_event_decisions(const zt_wire_event_decisions_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_event_decisions(const uint8_t *buf, size_t len, zt_wire_event_decisions_t *out);
zt_err_t zt_wire_encode_decision_receipt(const zt_wire_decision_receipt_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_decision_receipt(const uint8_t *buf, size_t len, zt_wire_decision_receipt_t *out);
zt_err_t zt_wire_encode_cache_page(const zt_wire_cache_page_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_cache_page(const uint8_t *buf, size_t len, zt_wire_cache_page_t *out);
zt_err_t zt_wire_encode_want_events(const zt_wire_want_events_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_want_events(const uint8_t *buf, size_t len, zt_wire_want_events_t *out);
zt_err_t zt_wire_encode_event_copy(const zt_wire_event_copy_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_event_copy(const uint8_t *buf, size_t len, zt_wire_event_copy_t *out);
zt_err_t zt_wire_encode_time_query(const zt_wire_time_query_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_time_query(const uint8_t *buf, size_t len, zt_wire_time_query_t *out);
zt_err_t zt_wire_encode_time_reply(const zt_wire_time_reply_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_time_reply(const uint8_t *buf, size_t len, zt_wire_time_reply_t *out);
zt_err_t zt_wire_encode_prepare_round_args(const zt_wire_prepare_round_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_prepare_round_args(const uint8_t *buf, size_t len, zt_wire_prepare_round_args_t *out);
zt_err_t zt_wire_encode_start_round_args(const zt_wire_start_round_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_start_round_args(const uint8_t *buf, size_t len, zt_wire_start_round_args_t *out);
zt_err_t zt_wire_encode_role_set_args(const zt_wire_role_set_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_role_set_args(const uint8_t *buf, size_t len, zt_wire_role_set_args_t *out);
zt_err_t zt_wire_encode_announce_args(const zt_wire_announce_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_announce_args(const uint8_t *buf, size_t len, zt_wire_announce_args_t *out);
zt_err_t zt_wire_encode_end_round_args(const zt_wire_end_round_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_end_round_args(const uint8_t *buf, size_t len, zt_wire_end_round_args_t *out);
zt_err_t zt_wire_encode_final_result_args(const zt_wire_final_result_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_final_result_args(const uint8_t *buf, size_t len, zt_wire_final_result_args_t *out);
zt_err_t zt_wire_encode_cancel_prepare_args(const zt_wire_cancel_prepare_args_t *value, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_cancel_prepare_args(const uint8_t *buf, size_t len, zt_wire_cancel_prepare_args_t *out);

/* Full envelope codecs include header, payload and appended tag; exact total
 * 48 + payload_len + 16 <=250. Decode authenticates before publishing payload.
 * Relay must recompute HMAC after changing ttl_remaining, hops or age_ms. */
zt_err_t zt_wire_encode_envelope(const zt_wire_header_t *header, const uint8_t *payload, size_t payload_len, const uint8_t key[ZT_HMAC_KEY_BYTES], uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_wire_decode_envelope(const uint8_t *buf, size_t len, const uint8_t key[ZT_HMAC_KEY_BYTES], zt_wire_header_t *header, uint8_t *payload, size_t capacity, size_t *payload_len);
zt_err_t zt_wire_hmac(const uint8_t key[ZT_HMAC_KEY_BYTES], const uint8_t *header_payload, size_t len, uint8_t tag[ZT_HMAC_TAG_BYTES]);
/* Verify in constant time; mismatch returns ZT_ERR_AUTH. */
zt_err_t zt_wire_hmac_verify(const uint8_t expected[ZT_HMAC_TAG_BYTES], const uint8_t actual[ZT_HMAC_TAG_BYTES]);
zt_err_t zt_wire_roster_hash(const zt_wire_roster_entry_t *entries, size_t count, uint64_t *hash);
zt_err_t zt_wire_cache_digest(const zt_wire_cache_key_t *keys, size_t count, uint64_t *digest);
#ifdef __cplusplus
}
#endif
