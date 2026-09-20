#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_game.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host ONLY: esp_http_client for HTTPS bootstrap/registration; exactly one
 * esp_websocket_client 1.8.0 WSS connection for ALL live traffic. No HTTP polling
 * fallback exists. Ordinary players allocate neither client, TLS nor JSON pools.
 * Certificate-bundle verification AND common-name/time checking are mandatory.
 * Authorization: Bearer header on HTTPS and WSS upgrade. Token MUST NEVER appear
 * in URL, query string, subprotocol, logs, UI, diagnostics, or redirects.
 * Configured HTTPS base defines origin: reject advertised socket_path if scheme,
 * host or port differ; select WSS only after validating the HTTPS-origin path.
 * Server MUST echo zt.v1. No listening socket, server, or backend on firmware. */
#define ZT_GATEWAY_API_PREFIX "/api/v1"
#define ZT_GATEWAY_BOOTSTRAP_PATH "/api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}"
#define ZT_GATEWAY_REGISTRATIONS_PATH "/api/v1/games/{game_id}/registrations"
#define ZT_GATEWAY_SOCKET_PATH "/api/v1/games/{game_id}/gateway/socket"
#define ZT_GATEWAY_AUTH_HEADER "Authorization"
#define ZT_GATEWAY_AUTH_SCHEME "Bearer"
#define ZT_GATEWAY_IDEMPOTENCY_HEADER "Idempotency-Key"
#define ZT_GATEWAY_SUBPROTOCOL "zt.v1"
#define ZT_GATEWAY_SCHEMA_VERSION 1
#define ZT_GATEWAY_MESSAGE_MAX_BYTES 4096
#define ZT_GATEWAY_TX_BUFFER_BYTES 4097
#define ZT_GATEWAY_RX_BUFFER_BYTES 4097
#define ZT_GATEWAY_EVENTS_MAX 8
#define ZT_GATEWAY_RECEIPTS_MAX 8
#define ZT_GATEWAY_DECISIONS_MAX 8
#define ZT_GATEWAY_COMMANDS_MAX 4
#define ZT_GATEWAY_SNAPSHOT_ENTRIES 8
#define ZT_GATEWAY_NEED_EVENTS_MAX 8
#define ZT_GATEWAY_ACK_ARRAY_MAX 8
#define ZT_GATEWAY_EVENTS_IN_FLIGHT 1
#define ZT_GATEWAY_CLOCK_IN_FLIGHT 1
#define ZT_GATEWAY_PING_INTERVAL_MS 10000
#define ZT_GATEWAY_PONG_TIMEOUT_MS 20000
#define ZT_GATEWAY_STALE_LINK_MS 25000
#define ZT_GATEWAY_CLOCK_INTERVAL_MS 30000
#define ZT_GATEWAY_NETWORK_TIMEOUT_MS 5000
#define ZT_GATEWAY_SEND_TIMEOUT_MS 200
#define ZT_GATEWAY_BACKOFF_COUNT 6
#define ZT_GATEWAY_BACKOFF_MS {1000, 2000, 4000, 8000, 16000, 30000}
#define ZT_GATEWAY_BACKOFF_CAP_MS 30000
#define ZT_GATEWAY_BACKOFF_JITTER_PERCENT 20
#define ZT_GATEWAY_RETRY_AFTER_MIN_S 1
#define ZT_GATEWAY_RETRY_AFTER_MAX_S 60
#define ZT_GATEWAY_DISABLE_AUTO_RECONNECT 1
#define ZT_GATEWAY_ENABLE_CLOSE_RECONNECT 0
#define ZT_GATEWAY_JSON_TOKEN_CAPACITY 256
#define ZT_GATEWAY_FIELD_VERSION "v"
#define ZT_GATEWAY_FIELD_TYPE "t"
#define ZT_GATEWAY_FIELD_ID "id"
#define ZT_GATEWAY_FIELD_TIMESTAMP "ts"
typedef enum {
    ZT_GATEWAY_HELLO, ZT_GATEWAY_EVENTS, ZT_GATEWAY_ACK, ZT_GATEWAY_NEED,
    ZT_GATEWAY_TIME_SYNC, ZT_GATEWAY_WELCOME, ZT_GATEWAY_RECEIPTS,
    ZT_GATEWAY_DECISIONS, ZT_GATEWAY_COMMANDS, ZT_GATEWAY_SNAPSHOT,
    ZT_GATEWAY_NEED_EVENTS, ZT_GATEWAY_TIME_SYNC_REPLY, ZT_GATEWAY_ERROR
} zt_gateway_message_type_t;
/* Enum names map exactly to lower-case t strings, e.g. TIME_SYNC_REPLY =>
 * "time_sync_reply". These enum ordinals are local, not wire numeric codes. */
typedef enum {
    ZT_GATEWAY_ERROR_INVALID_PAYLOAD, ZT_GATEWAY_ERROR_WRONG_ROUND,
    ZT_GATEWAY_ERROR_REGISTRATION_CLOSED, ZT_GATEWAY_ERROR_RATE_LIMITED,
    ZT_GATEWAY_ERROR_TOO_LARGE, ZT_GATEWAY_ERROR_CURSOR_EXPIRED,
    ZT_GATEWAY_ERROR_INTERNAL
} zt_gateway_error_code_t;
typedef enum { ZT_RESUME_OK, ZT_RESUME_RESET } zt_resume_t;
#define ZT_RESUME_OK_TEXT "ok"
#define ZT_RESUME_RESET_TEXT "reset"
typedef enum { ZT_ACK_SERVER_RECEIPT, ZT_ACK_SERVER_DECISION, ZT_ACK_BADGE_APPLICATION } zt_ack_stage_t;
typedef struct {
    uint8_t v;
    zt_gateway_message_type_t t;
    uint32_t id;
    uint64_t ts;
} zt_gateway_envelope_t;
/* Client id increases per connection. Durable server outbox id increases over
 * reconnects for round life. ts is NOT a clock anchor. IDs/MACs use zt_ids text
 * forms. Event IDs include their round; no numeric floating-point IDs. */
typedef struct { zt_slot_t slot; uint16_t seq; } zt_gateway_frontier_t;
typedef struct {
    zt_mac_t host_id;
    zt_boot_nonce_t host_boot;
    zt_game_id_t game_id;
    zt_round_id_t round_id; /* zero => JSON null in lobby */
    uint64_t registration_id; /* current registration request key; omit when zero */
    uint8_t proto, channel;
    char fw[ZT_BUILD_ID_LEN + 1];
    uint32_t last_server_id; /* last DURABLY APPLIED, never merely received */
    uint32_t state_rev;
    uint16_t pending_events;
    uint8_t decided_count;
    zt_gateway_frontier_t decided_through[ZT_MAX_PLAYERS];
} zt_gateway_hello_t;
typedef struct {
    uint64_t server_time_ms;
    zt_resume_t resume;
    zt_phase_t phase;
    zt_round_id_t round_id;
    uint32_t state_rev, resume_from, snapshot_id;
    uint8_t snapshot_pages, resetting;
} zt_gateway_welcome_t;
/* Exactly one hello first after CONNECTED. Send nothing else until welcome.
 * reset: re-fetch snapshot while RETAINING EVERY unsent/undecided local event.
 * NEVER clear journal, restart round, change role, or rerun patient zero for reset.
 * Successful esp_websocket_client_send_text acknowledges NOTHING. Retain queued
 * data until matching application acknowledgment; keep event causal evidence
 * through final decisions and round clearance. Server must persist before receipt
 * or decision; host persists commands/decisions before badge application ack. */
typedef struct { zt_round_id_t round_id; uint8_t count; zt_gateway_event_t events[ZT_GATEWAY_EVENTS_MAX]; } zt_gateway_events_t;
typedef struct { uint8_t count; zt_event_id_t ids[ZT_GATEWAY_RECEIPTS_MAX]; } zt_gateway_receipts_t;
typedef struct { zt_event_id_t id; zt_decision_status_t status; zt_decision_reason_t reason; uint8_t archived; } zt_gateway_decision_t;
typedef struct { uint8_t count; zt_gateway_decision_t entries[ZT_GATEWAY_DECISIONS_MAX]; } zt_gateway_decisions_t;
/* JSON START keeps the UTC schedule until bridge constructs a fresh radio time
 * sample; a decoded command must not lose that schedule or reuse a stale sample. */
typedef struct {
    uint32_t seq;
    zt_round_id_t round_id;
    zt_command_kind_t type;
    zt_slot_t target;
    uint32_t valid_until_elapsed_ms;
    union {
        zt_wire_prepare_round_args_t prepare;
        struct {
            uint32_t snapshot_id;
            uint64_t roster_hash;
            zt_slot_t patient_zero_slot;
            uint16_t initial_role_rev;
            uint64_t start_time_ms;
            uint32_t duration_ms;
        } start;
        zt_wire_role_set_args_t role_set;
        zt_wire_announce_args_t announce;
        zt_wire_end_round_args_t end;
        zt_wire_final_result_args_t final_result;
        zt_wire_cancel_prepare_args_t cancel;
        struct {
            zt_mac_t target_mac;
            uint64_t registration_id; /* zero => legacy round-scoped RESET_GAME */
        } reset;
    } args;
} zt_gateway_command_t;
typedef struct { uint8_t count; zt_gateway_command_t entries[ZT_GATEWAY_COMMANDS_MAX]; uint32_t state_rev; } zt_gateway_commands_t;
typedef struct { uint32_t seq; zt_slot_t slot; zt_command_receipt_state_t result; uint32_t state_rev; } zt_gateway_applied_t;
typedef struct { zt_slot_t slot; uint16_t through_seq; } zt_gateway_decision_applied_t;
typedef struct { zt_slot_t slot; uint32_t snapshot_id; zt_round_id_t round_id; } zt_gateway_ready_t;
typedef struct { zt_slot_t slot; uint16_t produced_seq; } zt_gateway_closed_t;
typedef struct {
    zt_slot_t slot;
    zt_role_t role;
    zt_cause_t cause;
    uint16_t role_rev, produced_seq, decided_seq;
    uint32_t age_ms;
    uint8_t via_hops;
} zt_gateway_presence_t;
typedef struct {
    zt_round_id_t round_id;
    uint8_t applied_count, decision_applied_count, ready_count, round_closed_count, presence_count;
    zt_gateway_applied_t applied[ZT_GATEWAY_ACK_ARRAY_MAX];
    zt_gateway_decision_applied_t decision_applied[ZT_GATEWAY_ACK_ARRAY_MAX];
    zt_gateway_ready_t ready[ZT_GATEWAY_ACK_ARRAY_MAX];
    zt_gateway_closed_t round_closed[ZT_GATEWAY_ACK_ARRAY_MAX];
    zt_gateway_presence_t presence[ZT_GATEWAY_ACK_ARRAY_MAX];
} zt_gateway_ack_t;
typedef struct {
    zt_round_id_t round_id;
    uint8_t wants_snapshot, page_index, event_count;
    uint32_t snapshot_id;
    zt_event_id_t events[ZT_GATEWAY_NEED_EVENTS_MAX];
} zt_gateway_need_t;
typedef struct { uint8_t count; zt_event_id_t events[ZT_GATEWAY_NEED_EVENTS_MAX]; } zt_gateway_need_events_t;
typedef struct { uint32_t nonce; } zt_gateway_time_sync_t;
typedef struct { uint32_t nonce; uint64_t server_time_ms; } zt_gateway_time_sync_reply_t;
/* Parsed text spans index the caller-owned JSON buffer, which must remain
 * unchanged until the message is consumed. No server-supplied text in logs
 * unless sanitized and known non-secret. */
typedef struct { uint16_t offset, len; } zt_json_span_t;
typedef struct { zt_gateway_error_code_t code; zt_json_span_t detail; uint8_t fatal; } zt_gateway_error_t;
typedef struct {
    zt_gateway_envelope_t envelope;
    union {
        zt_gateway_hello_t hello;
        zt_gateway_welcome_t welcome;
        zt_gateway_events_t events;
        zt_gateway_ack_t ack;
        zt_gateway_need_t need;
        zt_gateway_time_sync_t time_sync;
        zt_gateway_receipts_t receipts;
        zt_gateway_decisions_t decisions;
        zt_gateway_commands_t commands;
        zt_server_snapshot_t snapshot;
        zt_gateway_need_events_t need_events;
        zt_gateway_time_sync_reply_t time_sync_reply;
        zt_gateway_error_t error;
    } body;
} zt_gateway_message_t;
typedef struct { uint16_t start, end, parent, child_count; uint8_t type; } zt_json_token_t;
typedef struct { zt_json_token_t tokens[ZT_GATEWAY_JSON_TOKEN_CAPACITY]; uint16_t count; } zt_json_workspace_t;
typedef struct { char bytes[ZT_GATEWAY_TX_BUFFER_BYTES]; size_t len; } zt_gateway_tx_buffer_t;
typedef enum { ZT_WS_CONTINUATION=0, ZT_WS_TEXT=1, ZT_WS_BINARY=2, ZT_WS_CLOSE=8, ZT_WS_PING=9, ZT_WS_PONG=10 } zt_ws_opcode_t;
/* SDK event metadata describes one chunk of one FRAME: payload_len is that
 * frame's total length, payload_offset is the offset within that frame, and
 * fin/op_code describe that frame. data_ptr is borrowed only during this call. */
typedef struct {
    int32_t payload_len, payload_offset, data_len;
    uint8_t fin, op_code;
    const uint8_t *data_ptr;
} zt_ws_chunk_t;
typedef struct {
    char bytes[ZT_GATEWAY_RX_BUFFER_BYTES];
    uint32_t frame_payload_len; /* current data frame's expected payload length */
    uint32_t frame_copied_len;  /* progress within that frame only */
    uint32_t message_len;       /* aggregate copied bytes across all data frames */
    uint8_t frame_in_progress, frame_fin, frame_op_code;
    uint8_t message_in_progress, ready;
    uint32_t dropped_messages;
} zt_ws_reassembly_t;
/* One JSON object per logical TEXT MESSAGE: opening text frame (op_code 0x1),
 * then zero or more continuation frames (0x0), ending at the frame with fin set.
 * A frame is fully received when payload_offset + data_len >= payload_len;
 * the message completes ONLY after that final frame has been fully copied.
 * A frame may itself span several SDK events. Only its first chunk opens the
 * frame; later chunks of the same text frame are NOT new opening text frames.
 * Reset per-frame progress for each data frame; retain aggregate message_len
 * across continuations. The 4096-byte limit bounds the AGGREGATE across all
 * frames of one message; bytes[] stays 4097 bytes including the separate NUL.
 * Control frames (close 0x8, ping 0x9, pong 0xA) may interleave between fragments:
 * the client component handles them, they NEVER reach the JSON parser, and they
 * MUST NOT disturb/reset data-frame progress or the message assembly. A close
 * frame itself does not discard assembly; a subsequent disconnect does.
 * Discard the whole assembly, count the drop and advance NO cursor if aggregate
 * payload would exceed 4096, a new opening text frame arrives while a message is
 * in progress, a binary frame arrives, or a disconnect occurs mid-assembly.
 * Handler ONLY copies bounded bytes and signals gateway task: never JSON parse,
 * gameplay mutation, esp_websocket_client_stop/_close/_destroy. Those lifecycle
 * functions are forbidden from handler context; gateway task exclusively owns
 * them. Completed RX buffer must remain immutable until task releases it. */
zt_err_t zt_gateway_ws_copy_chunk(zt_ws_reassembly_t *assembly, const zt_ws_chunk_t *chunk);
void zt_gateway_ws_discard(zt_ws_reassembly_t *assembly);
/* Ping/pong NEVER establishes server time. Clock exchange every 30s and once
 * after welcome. Anchor ONLY server_time_ms in bootstrap/welcome/time_sync_reply:
 * RTT<=2000ms, initial ceil(RTT/2)+50ms, 200ppm drift, sane round/server match.
 * Stale link after 25s without ANY inbound frame. Gateway owns jittered ladder;
 * disable_auto_reconnect=true, no socket-loss-induced channel scan. */
typedef enum {
    ZT_CLOSE_NORMAL=1000, ZT_CLOSE_GOING_AWAY=1001, ZT_CLOSE_POLICY=1008,
    ZT_CLOSE_TOO_BIG=1009, ZT_CLOSE_SERVER_ERROR=1011,
    ZT_CLOSE_UNKNOWN_GAME=4001, ZT_CLOSE_STALE_ROUND=4003, ZT_CLOSE_CURSOR_EXPIRED=4010
} zt_gateway_close_code_t;
typedef enum {
    ZT_GATEWAY_RETRY_BACKOFF, ZT_GATEWAY_STOP_AUTH, ZT_GATEWAY_OPERATOR_SETUP,
    ZT_GATEWAY_SHRINK_BATCH, ZT_GATEWAY_RECONCILE_SNAPSHOT,
    ZT_GATEWAY_RESET_RETAIN_EVENTS, ZT_GATEWAY_REPORT_TLS,
    ZT_GATEWAY_DROP_NO_CURSOR, ZT_GATEWAY_STOP_FATAL,
    ZT_GATEWAY_STOP_SCHEMA, ZT_GATEWAY_RETRY_AFTER
} zt_gateway_reaction_t;
/* Close reactions: 1000/1001/1011 => RETRY_BACKOFF; 1008 => STOP_AUTH;
 * 1009 => count, SHRINK_BATCH, never resend oversized object; 4001 => SETUP;
 * 4003 => reconnect + RECONCILE_SNAPSHOT; 4010 => RESET_RETAIN_EVENTS.
 * Upgrade 401/403 => STOP_AUTH; 404 => SETUP; TLS => REPORT_TLS, never weaken
 * verification. Malformed/oversize inbound => DROP_NO_CURSOR; repeated failures
 * close with 1009 from gateway task. fatal error => STOP_FATAL/actionable UI.
 * HTTPS 400/422 => STOP_SCHEMA; 401/403 => STOP_AUTH; 404 => SETUP;
 * 409 => reconcile round/registration; 413 => split never truncate;
 * 429 => bounded Retry-After 1..60s; network/5xx => RETRY_BACKOFF;
 * malformed/incomplete 200 => failure, no deletion of pending events. */
typedef struct {
    zt_mac_t badge_id;
    char name[ZT_NAME_BUFFER_BYTES];
    zt_round_id_t known_round_id;
    char fw[ZT_BUILD_ID_LEN + 1];
    uint64_t stable_request_id; /* formatted once, reused as Idempotency-Key */
} zt_registration_request_t;
typedef struct { zt_join_status_t status; zt_slot_t slot; zt_round_id_t round_id; uint32_t state_rev; } zt_registration_response_t;
typedef struct {
    zt_host_control_t action;
    uint64_t request_id, registration_id;
    zt_round_id_t round_id;
} zt_gateway_control_request_t;
typedef struct {
    zt_host_control_t action;
    uint64_t request_id;
    zt_round_id_t round_id;
    uint32_t state_rev;
} zt_gateway_control_response_t;
typedef struct {
    zt_game_id_t game_id;
    zt_mac_t host_id;
    uint64_t server_time_ms;
    uint8_t max_players, has_next_page, next_page;
    zt_server_snapshot_t first_page;
    char socket_path[ZT_HTTPS_URL_MAX_LEN + 1];
} zt_bootstrap_t;
typedef enum {
    ZT_GATEWAY_ACTIVITY_IDLE, ZT_GATEWAY_ACTIVITY_BOOTSTRAP, ZT_GATEWAY_ACTIVITY_REGISTER,
    ZT_GATEWAY_ACTIVITY_START, ZT_GATEWAY_ACTIVITY_RESET
} zt_gateway_activity_t;
typedef struct {
    uint8_t connected, welcomed, auth_error;
    zt_gateway_activity_t activity;
    uint32_t last_server_applied_id, dropped_messages, reconnect_count;
    uint32_t gateway_stack_free_min, websocket_stack_free_min;
    uint64_t last_inbound_us;
    /* A validated HTTPS reply is separate from a fresh, welcomed WSS link. */
    uint64_t last_http_success_us;
    zt_err_t last_error;
    int http_status;
} zt_gateway_status_t;
typedef zt_err_t (*zt_gateway_decision_sink_t)(const zt_server_decision_t *decision, void *context);
typedef zt_err_t (*zt_gateway_command_sink_t)(const zt_server_command_t *command, void *context);
typedef zt_err_t (*zt_gateway_snapshot_sink_t)(const zt_server_snapshot_t *snapshot, void *context);
typedef zt_err_t (*zt_gateway_receipt_sink_t)(const zt_server_receipt_t *receipt, void *context);
typedef struct {
    zt_gateway_decision_sink_t decision;
    zt_gateway_command_sink_t command;
    zt_gateway_snapshot_sink_t snapshot;
    zt_gateway_receipt_sink_t receipt;
    void *context;
} zt_gateway_sinks_t;
zt_err_t zt_gateway_init(const zt_config_t *host_config, const zt_gateway_sinks_t *sinks);
zt_err_t zt_gateway_bootstrap(zt_bootstrap_t *out);
zt_err_t zt_gateway_register(const zt_registration_request_t *request, zt_registration_response_t *out);
zt_err_t zt_gateway_control(const zt_gateway_control_request_t *request, zt_gateway_control_response_t *out);
zt_err_t zt_gateway_ws_open(const zt_gateway_hello_t *hello);
zt_err_t zt_gateway_ws_send(const uint8_t *text, size_t len);
zt_err_t zt_gateway_ws_close(uint16_t code);
zt_err_t zt_gateway_service(uint64_t now_us);
zt_err_t zt_gateway_status(zt_gateway_status_t *out);
zt_err_t zt_gateway_submit_feed(const zt_game_feed_item_t *item);
/* Fixed parser tokens, exact schema/types/bounds, no unbounded JSON trees.
 * Decode must reject trailing content, truncated objects and over-bound arrays. */
zt_err_t zt_gateway_encode(const zt_gateway_message_t *message, char *out, size_t capacity, size_t *written);
zt_err_t zt_gateway_decode(const char *json, size_t len, zt_json_workspace_t *workspace, zt_gateway_message_t *out);
zt_err_t zt_gateway_decode_bootstrap(const char *json, size_t len, zt_json_workspace_t *workspace, zt_bootstrap_t *out);
zt_err_t zt_gateway_encode_registration(const zt_registration_request_t *request, char *out, size_t capacity, size_t *written);
zt_err_t zt_gateway_decode_registration(const char *json, size_t len, zt_json_workspace_t *workspace, zt_registration_response_t *out);
zt_err_t zt_gateway_encode_control(const zt_gateway_control_request_t *request, char *out, size_t capacity, size_t *written);
zt_err_t zt_gateway_decode_control(const char *json, size_t len, zt_json_workspace_t *workspace, zt_gateway_control_response_t *out);
/* Semantic errors are trusted only when the rejection echoes this exact action
 * and request identity. Unknown codes remain a generic conflict. */
zt_err_t zt_gateway_decode_control_rejection(const char *json, size_t len, zt_json_workspace_t *workspace,
    const zt_gateway_control_request_t *request, zt_err_t *reason);
zt_err_t zt_gateway_bridge_publish(const zt_gateway_message_t *verified);
/* Persist cursor only when ALL applicable outbox content is durably applied;
 * targeted command gaps mean largest-seen sequence is never sufficient. */
zt_err_t zt_gateway_mark_applied(zt_round_id_t round_id, uint32_t server_id);
#ifdef __cplusplus
}
#endif
