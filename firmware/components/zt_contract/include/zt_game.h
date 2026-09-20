#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "zt_radio.h"
#include "zt_store.h"
#include "zt_hal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The game task is the ONLY writer of gameplay state. Radio, HTTP/WebSocket,
 * LCD-DMA and USB callbacks NEVER mutate it. Post APIs copy bounded inputs;
 * BUSY means caller retains/retries. No borrowed SDK pointers or hot-path heap. */
typedef zt_wire_role_t zt_role_t;
typedef zt_wire_phase_t zt_phase_t;
typedef enum {
    ZT_ADMISSION_BOOT, ZT_ADMISSION_WAIT_INSTALL, ZT_ADMISSION_NEEDS_CONFIG,
    ZT_ADMISSION_RECOVERING, ZT_ADMISSION_REJOINING, ZT_ADMISSION_LOBBY,
    ZT_ADMISSION_REGISTERING, ZT_ADMISSION_WAITING_FOR_ROUND,
    ZT_ADMISSION_NEXT_ROUND, ZT_ADMISSION_PREPARED, ZT_ADMISSION_RUNNING,
    ZT_ADMISSION_EXPIRED_PENDING_SYNC, ZT_ADMISSION_FINAL
} zt_admission_t;
typedef enum { ZT_ERROR_NONE, ZT_ERROR_STORAGE, ZT_ERROR_RADIO, ZT_ERROR_HOST_AUTH } zt_error_overlay_t;
#define ZT_STORAGE_ERROR ZT_ERROR_STORAGE
#define ZT_RADIO_ERROR ZT_ERROR_RADIO
#define ZT_HOST_AUTH_ERROR ZT_ERROR_HOST_AUTH
#define ZT_UI_MESSAGE_MAX_LEN ZT_ANNOUNCE_MAX_LEN
#define ZT_GET_CLOSER_RATE_LIMIT_MS 500
#define ZT_LED_TAG_CONFIRMED_MS 600
#define ZT_LED_INFECTED_MS 1200
#define ZT_LED_UNCONFIRMED_MS 300
#define ZT_RADAR_WIDTH 160
#define ZT_RADAR_HEIGHT 180
#define ZT_RADAR_LABEL "PROXIMITY · NOT DIRECTION"
typedef enum { ZT_RANGE_UNKNOWN, ZT_RANGE_FAR, ZT_RANGE_NEARBY, ZT_RANGE_CLOSE, ZT_RANGE_IN_RANGE } zt_range_tier_t;
typedef enum { ZT_TRANSITION_IDLE, ZT_TRANSITION_PERSISTING } zt_transition_t;
typedef enum { ZT_FEEDBACK_NONE, ZT_FEEDBACK_GET_CLOSER, ZT_FEEDBACK_STAY_CLEAR, ZT_FEEDBACK_TAG_CONFIRMED, ZT_FEEDBACK_INFECTED, ZT_FEEDBACK_UNCONFIRMED, ZT_FEEDBACK_SYNC_REQUIRED } zt_feedback_t;
typedef enum { ZT_HOST_CONTROL_NONE, ZT_HOST_CONTROL_START, ZT_HOST_CONTROL_RESET } zt_host_control_t;
typedef struct {
    zt_mac_t mac;
    zt_slot_t slot;
    char name[ZT_NAME_BUFFER_BYTES];
    zt_role_t role;
    uint16_t role_rev;
    zt_cause_t cause;
    zt_range_tier_t tier;
    int16_t filtered_rssi_q8;
    int8_t latest_rssi;
    uint8_t recent_sample_count, eligible, provisional;
    uint32_t age_ms;
} zt_peer_entry_t;
typedef struct {
    uint32_t rx_drops, tx_drops, invalid_frames, auth_failures, dedupe_hits;
    uint32_t rx_high_water, tx_high_water, event_high_water, input_drops;
    uint32_t tx_watchdogs, radio_restarts, gateway_drops, gateway_reconnects;
    uint32_t replay_backlog, free_heap, minimum_heap, largest_free_block;
    uint32_t clock_uncertainty_ms;
    uint32_t gateway_error, gateway_http_status, gateway_http_age_ms;
    uint8_t gateway_initialized, wifi_has_ip, gateway_activity;
} zt_diagnostics_t;
/* Published immutable value: consumer owns its copy. No pointers into game state,
 * credentials, keys, invented battery percentages, or relay-based proximity. */
typedef struct {
    uint32_t generation;
    uint64_t sampled_us;
    zt_mac_t self_mac;
    char build_id[ZT_BUILD_ID_LEN + 1], name[ZT_NAME_BUFFER_BYTES];
    zt_round_id_t round_id;
    zt_slot_t self_slot;
    zt_admission_t admission;
    zt_error_overlay_t error;
    zt_err_t last_error;
    zt_phase_t phase;
    zt_role_t role;
    uint16_t role_rev;
    uint32_t remaining_ms;
    int32_t countdown_ms;
    uint8_t roster_count, ready_count, registered, role_provisional;
    zt_slot_t selected_target;
    char selected_name[ZT_NAME_BUFFER_BYTES];
    zt_range_tier_t selected_tier;
    uint8_t direct_contact_count;
    zt_peer_entry_t contacts[ZT_MAX_PLAYERS];
    uint8_t channel, host_selected, host_configured, aux1_changed_live;
    uint8_t host_connected, server_connected, diagnostic_mode;
    uint32_t host_age_ms, server_age_ms;
    uint16_t pending_event_count;
    uint8_t result_present, result_final, result_complete;
    zt_role_t winner;
    uint32_t missing_slots_bitmap;
    zt_host_control_t host_control;
    zt_err_t host_control_error;
    uint8_t host_control_pending, host_can_start, host_can_reset;
    zt_feedback_t feedback;
    uint64_t feedback_expires_us;
    char announcement[ZT_ANNOUNCE_MAX_LEN + 1];
    uint64_t announcement_expires_us;
    uint8_t status_overlay, brightness_cap;
    zt_diagnostics_t diagnostics;
} zt_ui_snapshot_t;
/* Verified gateway feeds preserve server receipt / decision / badge application.
 * A cursor can advance durably only after the full message has been applied. */
typedef struct { zt_round_id_t round_id; uint32_t server_id; zt_wire_decision_entry_t decision; } zt_server_decision_t;
typedef struct { zt_round_id_t round_id; uint32_t server_id; zt_wire_command_t command; } zt_server_command_t;
typedef struct { zt_round_id_t round_id; uint32_t server_id; zt_event_id_t event_id; } zt_server_receipt_t;
typedef struct {
    zt_round_id_t round_id;
    uint32_t server_id, snapshot_id, state_rev;
    uint64_t roster_hash;
    uint8_t page_index, page_count, entry_count;
    zt_wire_roster_entry_t roster[ZT_ROSTER_PAGE_ENTRIES];
    zt_wire_role_entry_t roles[ZT_ROSTER_PAGE_ENTRIES];
    zt_phase_t phase;
    uint64_t start_time_ms, end_time_ms;
    uint8_t patient_zero_slot, round_channel;
    uint8_t result_present, result_final, result_complete, winner;
    uint32_t missing_slots_bitmap, effective_elapsed_ms;
    zt_wire_prepare_round_args_t rules;
} zt_server_snapshot_t;
typedef struct {
    zt_event_id_t id;
    zt_wire_event_t body;
} zt_gateway_event_t;
typedef enum { ZT_GAME_FEED_EVENT, ZT_GAME_FEED_COMMAND_RECEIPT, ZT_GAME_FEED_DECISION_RECEIPT, ZT_GAME_FEED_ROUND_CLOSED, ZT_GAME_FEED_JOIN, ZT_GAME_FEED_CONTROL } zt_game_feed_kind_t;
typedef struct {
    zt_game_feed_kind_t kind;
    zt_round_id_t round_id;
    union {
        zt_wire_event_t event;
        zt_wire_command_receipt_t command_receipt;
        zt_wire_decision_receipt_t decision_receipt;
        zt_wire_round_closed_t round_closed;
        /* Registration is explicit (A), authenticated on the mesh, and retried
         * with the same badge/boot/request nonce until the backend responds. */
        struct { zt_wire_join_t request; zt_boot_nonce_t boot_nonce; } join;
        /* Explicit host B action only. Gateway derives an idempotency key from
         * this immutable request and performs network work in its own task.
         * Outer round_id is the expected current round (zero for lobby START). */
        struct {
            zt_host_control_t action;
            uint32_t request_seq;
            zt_boot_nonce_t boot_nonce;
            uint64_t registration_id;
        } control;
    } body;
} zt_game_feed_item_t;
typedef zt_err_t (*zt_game_feed_sink_t)(const zt_game_feed_item_t *item, void *context);
typedef zt_err_t (*zt_snapshot_sink_t)(const zt_ui_snapshot_t *snapshot, void *context);
zt_err_t zt_game_init(const zt_config_t *config, zt_snapshot_sink_t ui_sink, zt_game_feed_sink_t gateway_sink, void *context);
zt_err_t zt_game_post_button(const zt_button_edge_t *edge);
zt_err_t zt_game_post_domain(const zt_domain_message_t *message);
zt_err_t zt_game_post_persist(const zt_persist_completion_t *completion);
zt_err_t zt_game_post_decision(const zt_server_decision_t *decision);
zt_err_t zt_game_post_command(const zt_server_command_t *command);
zt_err_t zt_game_post_receipt(const zt_server_receipt_t *receipt);
zt_err_t zt_game_post_snapshot(const zt_server_snapshot_t *snapshot);
/* Gateway queues a verified HTTPS registration result; only the game owner
 * applies its own admission or originates a remote badge's JOIN_RESULT. */
zt_err_t zt_game_post_registration(zt_round_id_t round_id, const zt_wire_join_result_t *result);
/* Gateway reports server acceptance or a terminal rejection. Transient network
 * failures retain the same control request there; BUSY retains this result. */
zt_err_t zt_game_post_control_result(zt_host_control_t action, uint32_t request_seq, zt_err_t result);
zt_err_t zt_game_service(uint64_t now_us);
zt_err_t zt_game_snapshot(zt_ui_snapshot_t *out);
/* Round admission requires a successful explicit A registration in this boot. */
bool zt_game_admission_enabled(void);
uint64_t zt_game_registration_id(void);
/* Non-destructive bounded feed; one round per batch, alternate active/previous.
 * Transport send success NEVER removes entries. Receipt stops upload retries;
 * durable final decisions and clearance govern eventual journal reclamation. */
zt_err_t zt_game_event_feed(zt_round_id_t round_id, uint16_t cursor, zt_gateway_event_t *out, size_t capacity, size_t *count, uint16_t *next_cursor);
/* Clock is owned by game. Reboot clears time validity and local round state.
 * New samples may shorten but NEVER extend an established local deadline. */
typedef struct { int32_t elapsed_ms; uint32_t uncertainty_ms; uint64_t sampled_us; zt_time_quality_t quality; } zt_clock_sample_t;
zt_err_t zt_clock_apply(zt_round_id_t round_id, const zt_clock_sample_t *sample);
zt_err_t zt_clock_read(uint64_t now_us, zt_clock_sample_t *out);
zt_err_t zt_peers_observe(const zt_domain_message_t *direct_beacon);
zt_err_t zt_peers_invalidate(uint32_t driver_generation);
zt_err_t zt_peers_list(uint64_t now_us, zt_peer_entry_t *out, size_t capacity, size_t *count);
#ifdef __cplusplus
}
#endif
