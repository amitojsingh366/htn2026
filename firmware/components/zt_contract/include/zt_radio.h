#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_wire.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZT_RADIO_COUNTRY "CA"
#define ZT_CHANNEL_MIN 1
#define ZT_CHANNEL_MAX 11
#define ZT_RADIO_PHY_RATE_MBPS 1
#define ZT_CHANNEL_WIDTH_MHZ 20
#define ZT_DISCOVERY_LAST_CHANNEL_MS 2000
#define ZT_DISCOVERY_DWELL_MS 700
#define ZT_DISCOVERY_BACKOFF_MS 1000
#define ZT_STA_RECONNECT_BACKOFF_MS {10000, 20000, 30000, 30000}
#define ZT_STA_SCAN_MIN_MS 40
#define ZT_STA_SCAN_MAX_MS 120
#define ZT_STA_ASSOC_TIMEOUT_MS 3000
#define ZT_STA_IP_TIMEOUT_MS 8000
#define ZT_CHANNEL_CHECK_MS 100
#define ZT_RADIO_CANCEL_TIMEOUT_MS 500
#define ZT_TX_CALLBACK_TIMEOUT_MS 500
#define ZT_TX_IN_FLIGHT_MAX 1
#define ZT_TX_CRITICAL_RATE 8
#define ZT_TX_CRITICAL_BURST 8
#define ZT_TX_CONTROL_RATE 6
#define ZT_TX_CONTROL_BURST 8
#define ZT_TX_REPAIR_RATE 2
#define ZT_TX_REPAIR_BURST 2
#define ZT_TX_BEACON_RATE 2
#define ZT_TX_BEACON_BURST 2
#define ZT_TX_TOTAL_RATE 20
#define ZT_TX_TOTAL_BURST 20
#define ZT_LINK_ALLOWLIST_MAX ZT_MAX_PLAYERS
/* SDK RX pointers never survive the callback. Copy all these fields and bytes;
 * queue-full drops/counts. Generation tags discard work from stopped drivers. */
typedef struct {
    zt_mac_t src_mac;
    int8_t rssi;
    uint8_t channel;
    uint64_t rx_us;
    uint16_t len;
    uint8_t data[ZT_MAX_FRAME_BYTES];
    uint32_t driver_generation;
} zt_rx_frame_t;
typedef enum {
    ZT_TX_PRIO_DIRECT_TAG=0, ZT_TX_PRIO_EVENT_CONTROL=1,
    ZT_TX_PRIO_CLOCK_REPAIR=2, ZT_TX_PRIO_REPLAY=3, ZT_TX_PRIO_COSMETIC=4
} zt_tx_priority_t;
typedef enum { ZT_BUCKET_CRITICAL=0, ZT_BUCKET_CONTROL=1, ZT_BUCKET_REPAIR=2, ZT_BUCKET_BEACON=3 } zt_tx_bucket_t;
typedef struct {
    uint16_t len;
    uint8_t data[ZT_MAX_FRAME_BYTES];
    zt_tx_priority_t priority;
    zt_tx_bucket_t bucket;
    uint64_t not_before_us, expires_us;
    uint32_t request_id;
} zt_tx_frame_t;
typedef struct { uint32_t request_id, driver_generation; zt_err_t result; } zt_tx_completion_t;
typedef zt_err_t (*zt_rx_sink_t)(const zt_rx_frame_t *frame, void *context);
typedef void (*zt_tx_sink_t)(const zt_tx_completion_t *completion, void *context);
typedef struct {
    zt_game_id_t game_id;
    zt_mac_t self_mac, host_mac;
    uint8_t group_key[ZT_HMAC_KEY_BYTES];
    uint8_t last_channel, is_host;
} zt_radio_config_t;
/* One owner controls Wi-Fi/ESP-NOW, STA RAM configuration, HT20/WIFI_PS_NONE,
 * one unencrypted broadcast peer, channel=0. No default NVS or PHY persistence.
 * TX buffer stays owned until callback or COMPLETED driver teardown; watchdog
 * records failure and initiates controlled recovery, retaining logical retries. */
zt_err_t zt_radio_init(const zt_radio_config_t *config, zt_rx_sink_t rx_sink, zt_tx_sink_t tx_sink, void *context);
zt_err_t zt_radio_submit(const zt_tx_frame_t *frame);
zt_err_t zt_radio_service(uint64_t now_us);
typedef enum { ZT_CHANNEL_DISCOVERY, ZT_CHANNEL_LOBBY, ZT_CHANNEL_LOCKED, ZT_CHANNEL_RECOVERING } zt_channel_state_t;
typedef struct { zt_channel_state_t state; uint8_t channel, associated, has_ip; uint32_t driver_generation; } zt_channel_status_t;
/* Query configured CA country, intersect allowed channels with 1..11. No blind
 * sweep; no scan in PREPARED/RUNNING due to host silence. Socket loss alone
 * NEVER scans. At most one restricted host recovery. No set_channel concurrent
 * with scan/connect; cancel, then restart driver if unsettled after 500 ms. */
zt_err_t zt_channel_start_discovery(uint8_t last_channel);
zt_err_t zt_channel_lock(zt_round_id_t round_id, uint8_t channel);
zt_err_t zt_channel_unlock_after_expiry(zt_round_id_t round_id);
zt_err_t zt_channel_recover(uint64_t now_us);
zt_err_t zt_channel_get_status(zt_channel_status_t *out);
typedef enum { ZT_STA_DISCONNECTED, ZT_STA_GOT_IP, ZT_STA_LOST_IP, ZT_STA_HOME_CHANNEL_CHANGED, ZT_STA_SCAN_DONE } zt_sta_event_kind_t;
typedef struct { zt_sta_event_kind_t kind; uint8_t channel; uint32_t driver_generation; } zt_sta_event_t;
zt_err_t zt_channel_post_sta_event(const zt_sta_event_t *event);
/* Diagnostic allowlist filters RX BEFORE protocol processing; no fabricated
 * RSSI, never persisted. Empty restores normal reception; UI shows DIAGNOSTIC. */
zt_err_t zt_radio_link_allowlist(const zt_mac_t *macs, size_t count);
/* Validated-domain publication in mesh task. Sink must copy accepted fields
 * before returning; message pointer is borrowed only for the call. Game queues
 * compact semantic records (<=96 bytes), never copies this entire union to its
 * 32-record event queue. Snapshot assembly has separately bounded storage. */
typedef struct {
    zt_wire_header_t header;
    zt_wire_payload_t payload;
    zt_mac_t direct_source;
    int8_t rssi;
    uint8_t channel;
    uint64_t rx_us;
    uint32_t driver_generation;
} zt_domain_message_t;
typedef zt_err_t (*zt_domain_sink_t)(const zt_domain_message_t *message, void *context);
zt_err_t zt_mesh_init(zt_domain_sink_t sink, void *context);
zt_err_t zt_mesh_receive(const zt_rx_frame_t *frame);
zt_err_t zt_mesh_publish(const zt_domain_message_t *message);
zt_err_t zt_mesh_submit(zt_round_id_t round_id, zt_pkt_type_t type, const zt_wire_payload_t *payload, zt_tx_priority_t priority);
zt_err_t zt_mesh_service(uint64_t now_us);
/* Contract amendment 1, 2026-09-19, raised by packet G01 and integrated by the
 * orchestrator. The radio module owns the transport boot nonce: plan 4.1 defers
 * creating it until Wi-Fi has started and the radio entropy source is available,
 * and plan 12 requires it to change every reboot. zt_mesh_submit() stamps it into
 * the envelope internally, so callers never pass it.
 *
 * The game task nevertheless needs to READ it, because the tag attempt identity is
 * (tagger boot nonce, attempt sequence) and that nonce travels only in the envelope
 * header, while TAG_RESULT and EVENT carry it back in their payload 'request_boot'
 * field. Without a getter the game cannot match an incoming TAG_RESULT to its own
 * outstanding attempt, nor populate request_boot when committing an infection.
 *
 * Returns ZT_ERR_INVALID_STATE until radio entropy is available. After that it returns
 * the same value for the rest of this boot. Never persisted; never reused across
 * reboots. */
zt_err_t zt_mesh_get_boot_nonce(zt_boot_nonce_t *out);
/* Contract amendment 3, 2026-09-19, raised by packet M01 and adjudicated by the
 * orchestrator. Both requests are accepted; (b) is accepted with its ownership rules
 * written down, because the direction of the call matters.
 *
 * (a) Radio-owned diagnostics. plan.md 8 requires "drops, queue highwater, replay
 * backlog, gateway age, channel and RSSI in a diagnostics screen/console", and
 * zt_diagnostics_t in zt_game.h already reserves fields (rx_drops, tx_drops,
 * invalid_frames, auth_failures, dedupe_hits, rx_high_water, tx_high_water,
 * tx_watchdogs, radio_restarts, replay_backlog) that only zt_radio can observe. The
 * frozen header exposed no way to read them, so the diagnostics screen and the console
 * `status` operation could never be populated. zt_radio_link_allowlist() likewise had
 * no readback, so the DIAGNOSTIC banner that plan.md 13 makes mandatory whenever the
 * allowlist is engaged could not be raised.
 *
 * Counters are monotonic u32 owned by the radio and mesh tasks and published by
 * word-sized stores. This getter is read-only, never blocks, never allocates, may be
 * called from any task, and returns a best-effort sample: each field is individually
 * self-consistent, the set is not an atomic instant. It never exposes keys, tokens,
 * credentials, or a MAC. */
typedef struct {
    uint32_t rx_drops, tx_drops, invalid_frames, auth_failures, dedupe_hits;
    uint32_t rx_high_water, tx_high_water, tx_watchdogs, radio_restarts;
    uint32_t replay_backlog, gateway_age_ms;
    uint8_t channel, diagnostic_mode, gateway_hops;
    int8_t last_rssi;
} zt_radio_diagnostics_t;
zt_err_t zt_radio_get_diagnostics(zt_radio_diagnostics_t *out);
/* (b) Explicit mesh restoration after reboot, distinguishing active and previous
 * rounds. plan.md 12 makes cache sets, digests and counts per round, and requires a
 * badge retaining the immediately previous round to offer old-round inventories once
 * per 30 s while its ordinary beacon stays current-round. After a reboot the mesh RAM
 * caches are empty, so with no restoration entry point the badge advertises an empty
 * digest, cannot answer WANT_EVENTS for events it still durably holds, and cannot
 * re-advertise the previous round at all.
 *
 * Ownership: the GAME task calls this, once per retained round, only after that
 * round's checkpoint is durably restored, and calls it for the ACTIVE round LAST,
 * because the most recent successful call selects the round the ordinary beacon
 * carries. The mesh may read the checkpoint and the durable journal only through the
 * existing read-only zt_store_load_checkpoint() and zt_game_event_feed() accessors,
 * must seed its per-round cache incrementally from zt_mesh_service() rather than
 * blocking the caller, and must never write durable storage, never mutate gameplay
 * state, and never persist a competing roster. A roster hash conflicting with a round
 * already held returns ZT_ERR_CONFLICT and changes nothing. */
zt_err_t zt_radio_mesh_restore(zt_round_id_t round_id);
/* Game owner calls only after explicit RESET_GAME has durably deleted this
 * round. Evict its volatile cache/templates; unrelated retained evidence stays. */
zt_err_t zt_radio_mesh_reset(zt_round_id_t round_id);
#ifdef __cplusplus
}
#endif
