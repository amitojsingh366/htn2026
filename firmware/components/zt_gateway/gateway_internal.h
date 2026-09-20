#pragma once
#include "zt_gateway.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_websocket_client.h"
#include "esp_transport.h"
#include <stdbool.h>

#define GW_JOINS ZT_MAX_PLAYERS
#define GW_ACKS (ZT_MAX_PLAYERS * 2)
#define GW_COMMANDS 4
typedef struct {
    zt_game_feed_item_t item;
    zt_registration_response_t response;
    uint64_t retry_us;
    uint8_t occupied, answered, delivered, backoff;
} gateway_join_t;
typedef struct {
    zt_game_feed_item_t item;
    zt_gateway_control_request_t request;
    zt_err_t result;
    uint64_t retry_us;
    uint8_t occupied, answered, delivered, backoff;
} gateway_control_t;
typedef struct { zt_game_feed_item_t item; uint8_t occupied; uint64_t sent_us; } gateway_ack_t;
typedef struct { zt_round_id_t round; uint16_t through[ZT_MAX_PLAYERS]; uint32_t dirty; } gateway_decision_ack_t;
typedef struct {
    zt_game_id_t game;
    zt_mac_t host;
    char origin[ZT_HTTPS_URL_MAX_LEN+1], token[ZT_HOST_TOKEN_MAX_LEN+1];
    char socket_path[ZT_HTTPS_URL_MAX_LEN+1];
    zt_gateway_sinks_t sinks;
    zt_gateway_status_t status;
    TaskHandle_t owner;
    esp_websocket_client_handle_t ws;
    esp_transport_handle_t ssl, guard_transport, ws_transport;
    portMUX_TYPE guard;
    zt_ws_reassembly_t rx;
    /* Codec token storage and outgoing/HTTPS scratch are never used together. */
    union { zt_json_workspace_t tokens; char tx[ZT_GATEWAY_TX_BUFFER_BYTES]; } scratch;
    zt_gateway_message_t message;
    gateway_join_t joins[GW_JOINS];
    gateway_control_t control;
    gateway_ack_t acks[GW_ACKS];
    zt_gateway_command_t commands[GW_COMMANDS];
    zt_gateway_event_t events[ZT_GATEWAY_EVENTS_MAX];
    zt_event_id_t requested_events[ZT_GATEWAY_NEED_EVENTS_MAX];
    gateway_decision_ack_t decision_acks[ZT_RETAINED_ROUND_CAPACITY];
    uint16_t event_cursor[ZT_RETAINED_ROUND_CAPACITY];
    zt_round_id_t event_rounds[ZT_RETAINED_ROUND_CAPACITY];
    uint64_t event_due, event_sent_us, decision_ack_due;
    uint8_t event_count, event_turn, requested_count, inbound_index, resetting;
    uint8_t command_count;
    uint8_t connected_event, disconnected_event, overflow_event, bootstrapped, hello_sent;
    uint8_t rx_dropping_message, ack_next, join_next, join_burst, events_first;
    uint8_t response_pending;
    uint8_t backoff, stopped, need_page, snapshot_pages, snapshot_mask, snapshot_request_pending;
    uint16_t close_code;
    uint32_t client_id, sync_nonce, snapshot_id;
    zt_round_id_t round;
    uint64_t retry_us, hello_us, sync_us, sync_due, anchor_us, anchor_ms, start_ms, page_due;
    uint64_t http_retry_us, response_due, send_due, join_resume_us;
    uint32_t anchor_uncertainty_ms;
    uint8_t header_done, header_first, header_protocol, header_overflow;
    uint16_t header_len;
    char header_line[128];
} gateway_t;
extern gateway_t *zt_gw;
zt_err_t gw_send(zt_gateway_message_t *message);
void gw_backoff(uint64_t now);
void gw_anchor(uint64_t server_ms, uint64_t sent_us, uint64_t received_us);
void gw_ws_release_rx(void);
void gw_ws_destroy(void);
