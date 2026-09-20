#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_wire.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZT_NVS_PARTITION "zt_nvs"
#define ZT_NVS_NAMESPACE "zt"
#define ZT_NVS_OFFSET UINT32_C(0x3F1000)
#define ZT_NVS_LENGTH UINT32_C(0xF000)
#define ZT_INSTALL_PARTITION "zt_install"
#define ZT_INSTALL_SUBTYPE 0x40
#define ZT_INSTALL_OFFSET UINT32_C(0x3F0000)
#define ZT_INSTALL_SECTOR_BYTES 4096
#define ZT_INSTALL_BYTES 80
#define ZT_INSTALL_MAGIC "ZTIN"
#define ZT_INSTALL_MAGIC_OFFSET 0
#define ZT_INSTALL_MAGIC_BYTES 4
#define ZT_INSTALL_SCHEMA_OFFSET 4
#define ZT_INSTALL_SCHEMA_BYTES 2
#define ZT_INSTALL_LENGTH_OFFSET 6
#define ZT_INSTALL_LENGTH_BYTES 2
#define ZT_INSTALL_MAC_OFFSET 8
#define ZT_INSTALL_MAC_BYTES 6
#define ZT_INSTALL_RESERVED_OFFSET 14
#define ZT_INSTALL_RESERVED_BYTES 2
#define ZT_INSTALL_UUID_OFFSET 16
#define ZT_INSTALL_UUID_BYTES 16
#define ZT_INSTALL_BASELINE_HASH_OFFSET 32
#define ZT_INSTALL_BASELINE_HASH_BYTES 32
#define ZT_INSTALL_NVS_OFFSET_OFFSET 64
#define ZT_INSTALL_NVS_OFFSET_BYTES 4
#define ZT_INSTALL_NVS_LENGTH_OFFSET 68
#define ZT_INSTALL_NVS_LENGTH_BYTES 4
#define ZT_INSTALL_FLAGS_OFFSET 72
#define ZT_INSTALL_FLAGS_BYTES 4
#define ZT_INSTALL_CRC_OFFSET 76
#define ZT_INSTALL_CRC_BYTES 4
#define ZT_INSTALL_CRC_COVERAGE_BYTES 76
#define ZT_INSTALL_FLAG_INITIALIZED 1
#define ZT_ERASED_BYTE 0xff
#define ZT_NVS_KEY_MAX_LEN 15
#define ZT_STORE_KEY_CONFIG "cfg"
#define ZT_STORE_KEY_ROUND_A "round_a"
#define ZT_STORE_KEY_ROUND_B "round_b"
#define ZT_STORE_KEY_DECISION_A "dec_a"
#define ZT_STORE_KEY_DECISION_B "dec_b"
#define ZT_STORE_EVENT_KEY_FORMAT "evt%03u"
#define ZT_STORE_EVENT_KEY_FIRST "evt000"
#define ZT_STORE_EVENT_KEY_LAST "evt127"
#define ZT_DURABLE_EVENT_BYTES 64
#define ZT_DURABLE_EVENT_MAGIC_OFFSET 0
#define ZT_DURABLE_EVENT_MAGIC_BYTES 4
#define ZT_DURABLE_EVENT_SCHEMA_OFFSET 4
#define ZT_DURABLE_EVENT_SCHEMA_BYTES 2
#define ZT_DURABLE_EVENT_LENGTH_OFFSET 6
#define ZT_DURABLE_EVENT_LENGTH_BYTES 2
#define ZT_DURABLE_EVENT_ROUND_OFFSET 8
#define ZT_DURABLE_EVENT_ROUND_BYTES 8
#define ZT_DURABLE_EVENT_BODY_OFFSET 16
#define ZT_DURABLE_EVENT_BODY_BYTES 30
#define ZT_DURABLE_EVENT_RESERVED_OFFSET 46
#define ZT_DURABLE_EVENT_RESERVED_BYTES 14
#define ZT_DURABLE_EVENT_CRC_OFFSET 60
#define ZT_DURABLE_EVENT_CRC_BYTES 4
#define ZT_DURABLE_EVENT_CRC_COVERAGE_BYTES 60
#define ZT_CONFIG_MAX_BYTES 1024
#define ZT_CHECKPOINT_MAX_BYTES 2048
#define ZT_STORE_GC_RESERVE_BYTES 16384
#define ZT_STORE_LIVE_VALUES_LIMIT_BYTES 20480
#define ZT_STORE_ALLOCATED_LIMIT_BYTES 40960
#define ZT_HTTPS_URL_MAX_LEN 192
#define ZT_SSID_MAX_LEN 32
#define ZT_PASSWORD_MAX_LEN 63
#define ZT_HOST_TOKEN_MAX_LEN 256
#define ZT_FLASH_BYTES UINT32_C(0x400000)
#define ZT_FACTORY_OFFSET UINT32_C(0x10000)
#define ZT_FACTORY_BYTES UINT32_C(0x2A0000)
/* Serialized integers LE, IEEE CRC32, reserved bytes zero; raw header sector
 * remainder 0xff. These decoded structs are never directly written to flash. */
typedef struct {
    uint8_t magic[ZT_INSTALL_MAGIC_BYTES];
    uint16_t schema, length;
    zt_mac_t mac;
    uint16_t reserved;
    uint8_t installation_uuid[ZT_INSTALL_UUID_BYTES];
    uint8_t baseline_sha256[ZT_INSTALL_BASELINE_HASH_BYTES];
    uint32_t nvs_offset, nvs_length, flags, crc32;
} zt_install_header_t;
typedef struct {
    uint8_t magic[ZT_DURABLE_EVENT_MAGIC_BYTES];
    uint16_t schema, length;
    zt_round_id_t round_id;
    zt_wire_event_t event;
    uint8_t reserved[ZT_DURABLE_EVENT_RESERVED_BYTES];
    uint32_t crc32;
} zt_durable_event_t;
typedef struct {
    zt_mac_t expected_mac;
    char name[ZT_NAME_BUFFER_BYTES];
    zt_game_id_t game_id;
    uint8_t group_key[ZT_HMAC_KEY_BYTES];
    zt_mac_t host_mac;
    uint8_t last_channel, host_credentials_present;
    char https_base[ZT_HTTPS_URL_MAX_LEN + 1];
    char ssid[ZT_SSID_MAX_LEN + 1];
    char password[ZT_PASSWORD_MAX_LEN + 1];
    char token[ZT_HOST_TOKEN_MAX_LEN + 1];
    uint64_t provisioned_time_ms;
} zt_config_t;
typedef struct {
    zt_round_id_t round_id;
    uint32_t snapshot_rev, state_rev;
    uint64_t roster_hash;
    uint8_t roster_count, patient_zero_slot, round_channel, phase;
    zt_wire_roster_entry_t roster[ZT_MAX_PLAYERS];
    zt_wire_role_entry_t roles[ZT_MAX_PLAYERS];
    zt_wire_prepare_round_args_t rules;
    uint16_t local_event_seq;
    uint16_t produced[ZT_MAX_PLAYERS], received[ZT_MAX_PLAYERS], decided[ZT_MAX_PLAYERS];
    uint32_t applied_command_seq[ZT_PENDING_COMMAND_CAPACITY];
    uint32_t last_server_applied_id;
    uint8_t journal_slots[ZT_JOURNAL_CAPACITY];
    uint16_t journal_slot_count;
    zt_wire_role_entry_t pending_roles[ZT_MAX_PLAYERS];
    uint32_t closed_bitmap, cleared_bitmap;
} zt_checkpoint_t;
typedef struct {
    zt_round_id_t round_id;
    uint16_t produced[ZT_MAX_PLAYERS];
    uint8_t export_sha256[ZT_SHA256_BYTES];
} zt_archive_clearance_t;
/* Explicit dashboard RESET_GAME authority, distinct from an export receipt.
 * Persist before deleting the named round; replay APPLIED within this boot.
 * LIVEG005 deliberately forgets round data and this receipt at the next boot. */
typedef struct {
    zt_round_id_t round_id;
    uint32_t command_seq;
    zt_slot_t slot;
    uint8_t channel;
} zt_reset_receipt_t;
typedef uint32_t zt_persist_request_id_t;
typedef enum {
    ZT_PERSIST_CONFIG, ZT_PERSIST_CHECKPOINT, ZT_PERSIST_EVENT,
    ZT_PERSIST_DECISIONS, ZT_PERSIST_COMMAND_RECEIPT, ZT_PERSIST_CLOSE,
    ZT_PERSIST_SERVER_CURSOR, ZT_PERSIST_ARCHIVE_CLEAR, ZT_PERSIST_RESET_ROUND
} zt_persist_kind_t;
typedef struct {
    zt_persist_request_id_t request_id;
    zt_persist_kind_t kind;
    zt_round_id_t round_id;
    union {
        zt_config_t config;
        zt_checkpoint_t checkpoint;
        zt_wire_event_t event;
        zt_wire_event_decisions_t decisions;
        zt_wire_command_receipt_t command_receipt;
        zt_wire_round_closed_t close;
        uint32_t server_applied_id;
        zt_archive_clearance_t clearance;
        zt_reset_receipt_t reset;
    } value;
} zt_persist_request_t;
typedef struct { zt_persist_request_id_t request_id; zt_persist_kind_t kind; zt_err_t result; } zt_persist_completion_t;
typedef zt_err_t (*zt_persist_sink_t)(const zt_persist_completion_t *completion, void *context);
typedef enum { ZT_INSTALL_WAIT, ZT_INSTALL_VALID, ZT_INSTALL_ERROR } zt_install_state_t;
typedef struct {
    zt_install_state_t install_state;
    uint32_t live_bytes, allocated_bytes, free_bytes;
    uint16_t event_count;
    zt_err_t last_error;
} zt_store_status_t;
/* Read-only inspection precedes named NVS initialization: exact stock four-entry
 * layout, chip size, valid raw header/schema/MAC. Blank tail => WAIT_INSTALL.
 * The default NVS erase API is NEVER called. There is NO automatic format-on-error
 * path. Invalid/nonblank unrecognized tails remain intact in STORAGE_ERROR.
 * Register runtime partition descriptors only; never change on-flash table. */
zt_err_t zt_store_inspect(const zt_mac_t *mac, zt_store_status_t *out);
zt_err_t zt_store_open(const zt_mac_t *mac, zt_persist_sink_t sink, void *context);
/* Guarded WAIT_INSTALL only: whole tail verified blank, expected MAC/schema/UUID
 * validated; initialize named NVS, write installation header LAST. */
zt_err_t zt_store_install_init(const zt_install_header_t *header);
/* Copy request into bounded owned work; BUSY transfers nothing. Accepted request
 * yields one definitive durable completion, retained until sink accepts it.
 * Victim stays locked PERSISTING; at 500ms send PENDING but DO NOT release lock.
 * Late success applies infection. Only definitive failure allows BUSY/unlock.
 * Immutable event commits before derived checkpoint; reconstruct sequence from
 * committed records. No assumption that two NVS keys are an atomic transaction.
 * Never persist beacons/RSSI/timer ticks. All writes enforce capacity reserves:
 * live values <20KiB, allocations <40KiB, free >=16KiB. */
zt_err_t zt_store_submit(const zt_persist_request_t *request);
zt_err_t zt_store_service(void);
zt_err_t zt_store_load_config(zt_config_t *out);
zt_err_t zt_store_load_checkpoint(zt_round_id_t round_id, zt_checkpoint_t *out);
/* Round zero requests the latest completed explicit reset. */
zt_err_t zt_store_load_reset(zt_round_id_t round_id, zt_reset_receipt_t *out);
zt_err_t zt_store_read_event(zt_round_id_t round_id, zt_slot_t victim_slot, uint16_t seq, zt_wire_event_t *out);
/* Read committed decision records from either retained round, including pending
 * decisions. Cursor indexes stable stored records; next_cursor==journal capacity
 * ends one scan. Restart at zero periodically to observe pending->final changes.
 * At most ZT_DECISION_PAGE_ENTRIES per call. BUSY means retry without advancing. */
zt_err_t zt_store_read_decisions(zt_round_id_t round_id, uint16_t cursor,
    zt_wire_decision_entry_t *out, size_t capacity, size_t *count, uint16_t *next_cursor);
/* Export bounded immutable evidence pages, current/previous round only; no stock
 * reads. Clearance must cover exported/decided frontiers; config/header survive. */
zt_err_t zt_store_export(zt_round_id_t round_id, uint16_t cursor, uint8_t *out, size_t capacity, size_t *written, uint16_t *next_cursor);
zt_err_t zt_store_status(zt_store_status_t *out);
zt_err_t zt_store_encode_install(const zt_install_header_t *header, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_store_decode_install(const uint8_t *buf, size_t len, zt_install_header_t *out);
zt_err_t zt_store_encode_event(const zt_durable_event_t *event, uint8_t *out, size_t capacity, size_t *written);
zt_err_t zt_store_decode_event(const uint8_t *buf, size_t len, zt_durable_event_t *out);
#ifdef __cplusplus
}
#endif
