#include "zt_game.h"
#include "zt_gateway.h"
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void zt_game_clock_reset(void);
zt_err_t zt_store_find_attempt(zt_round_id_t round, zt_slot_t victim, zt_slot_t actor,
                             zt_boot_nonce_t boot, uint32_t seq, zt_wire_event_t *out);
zt_err_t zt_store_inventory(zt_round_id_t round, zt_wire_cache_key_t *out, size_t cap, size_t *count);
zt_round_id_t zt_store_other_round(zt_round_id_t active);
bool zt_store_round_retained(zt_round_id_t round);
zt_err_t zt_store_submit_final(const zt_persist_request_t *request,const zt_wire_final_result_args_t *final);
zt_err_t zt_store_get_final(zt_round_id_t round,zt_wire_final_result_args_t *out);
zt_err_t zt_store_decision_frontier(zt_round_id_t round, zt_slot_t *slot, uint16_t *frontier);
void zt_game_peers_configure(zt_game_id_t id, const zt_checkpoint_t *cp);

/* All ingress paths copy only. init() and service() run in the same game
 * task; that task is the sole gameplay owner for the lifetime of this boot. */
typedef struct {
    uint64_t rx_us, round, boot;
    zt_mac_t origin;
    uint8_t type, hops;
    uint32_t generation;
    int8_t rssi;
    uint8_t channel, len, flags;
    uint8_t bytes[56];
} input_t;
_Static_assert(sizeof(input_t)<=ZT_EVENT_QUEUE_RECORD_BYTES,"game queue budget");
enum { IN_PERSIST=0xe0, IN_DECISION, IN_RECEIPT, IN_COMMAND, IN_PAGE, IN_CLOCK, IN_INVALIDATE, IN_REGISTRATION, IN_CONTROL_RESULT };
typedef struct { zt_host_control_t action; uint32_t request_seq; zt_err_t result; } control_result_t;
/* The same eight slots cover ingress, persistence and host delivery. */
typedef struct {
    zt_server_command_t value;
    uint64_t rx_us, deadline_us, retry_us[ZT_MAX_PLAYERS];
    uint32_t age_ms, targets;
    uint8_t hops, occupied, server, first_broadcast, attempts[ZT_MAX_PLAYERS];
} command_slot_t;
enum { COMMAND_DELIVERING=3, COMMAND_PERSISTING=4, COMMAND_CACHED=5, HOST_COMMAND_ATTEMPTS=30 };
typedef struct {
    uint8_t occupied, server;
    union { zt_domain_message_t radio; zt_server_snapshot_t snapshot; } value;
} page_slot_t;
static input_t queue[ZT_EVENT_QUEUE_CAPACITY];
static uint8_t qhead,qcount;
static input_t deferred;
static bool deferred_present;
static zt_button_edge_t buttons[ZT_INPUT_QUEUE_CAPACITY];
static uint8_t bhead,bcount;
static command_slot_t commands[ZT_PENDING_COMMAND_CAPACITY];
static page_slot_t pages[2];
static portMUX_TYPE ingress_guard=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t owner;
static bool initialized;
static uint32_t ingress_drops, ingress_high_water;
static zt_snapshot_sink_t ui_sink;
static zt_game_feed_sink_t feed_sink;
static void *sink_context;
static zt_config_t config;
static zt_ui_snapshot_t view, published;
static uint64_t published_registration_id;
static portMUX_TYPE view_guard=portMUX_INITIALIZER_UNLOCKED;
static zt_checkpoint_t current, staged;
static struct { zt_round_id_t round; zt_mac_t macs[20]; uint32_t slots; zt_slot_t self; } previous_round;
static uint16_t decision_received[ZT_MAX_PLAYERS], previous_decision_received[ZT_MAX_PLAYERS];
static struct { zt_round_id_t round; uint16_t cursor; uint64_t due; } decision_relay[2];
static unsigned decision_relay_turn;
static zt_wire_payload_t decision_payload;
static uint64_t inventory_due, inventory_next, previous_inventory_due;
static zt_round_id_t inventory_round;
static uint64_t inventory_digest;
static uint8_t inventory_page, inventory_pages;
static zt_slot_t inventory_target;
static uint8_t replay_previous;
static uint16_t previous_replay_cursor;
static uint64_t event_retry_us;
static uint8_t event_retry_count;
static zt_wire_event_t last_local_event;
enum { ASSEMBLY_SERVER=1, ASSEMBLY_MESH };
static struct {
    zt_round_id_t round;
    uint32_t rev, state_rev, server_id;
    uint64_t hash;
    uint8_t roster_pages, role_pages, roster_count, role_count, roster_mask, role_mask;
    uint32_t roster_slots, role_slots;
    zt_wire_roster_entry_t roster[20];
    zt_wire_role_entry_t roles[20];
    zt_wire_prepare_round_args_t rules;
    uint64_t start_time_ms, end_time_ms;
    uint8_t phase, patient_zero, channel, complete, source, invalid;
    uint8_t result_present, result_final, result_complete, winner;
    uint32_t missing_slots_bitmap, effective_elapsed_ms;
} assembly;
static zt_persist_request_id_t next_request;
static struct {
    uint32_t id;
    zt_round_id_t round;
    zt_persist_kind_t kind;
    uint64_t submitted_us;
    uint8_t pending_sent, action;
    zt_wire_event_t event;
    zt_wire_command_receipt_t receipt;
    zt_server_decision_t decision;
    zt_wire_final_result_args_t final_result;
    uint8_t final_present;
    zt_role_t end_winner;
    uint32_t end_elapsed_ms;
    uint32_t result_command_seq;
    uint8_t result_authoritative;
    uint8_t end_present;
    uint8_t host_command; /* Slot + 1, held until checkpoint completion. */
    uint8_t admission; /* Publish snapshot admission only after durable completion. */
    zt_reset_receipt_t reset;
} pending;
enum { APPLY_EVENT=1, APPLY_CHECKPOINT, APPLY_DECISION, APPLY_CLOSE, APPLY_RECEIPT, APPLY_RELAY_EVENT, APPLY_RESET };
static struct {
    bool active, responded, confirmed;
    zt_boot_nonce_t boot;
    zt_wire_tag_request_t request;
    uint64_t started_us, next_us;
    uint8_t sent;
} outbound;
static struct {
    zt_round_id_t round;
    zt_wire_tag_result_t result;
    uint64_t expires_us, repeat_us;
} outcomes[ZT_TAG_OUTCOME_CAPACITY];
static uint8_t outcome_next;
static uint32_t attempt_seq, join_nonce, join_generation, time_nonce;
static zt_boot_nonce_t boot_nonce;
static zt_mac_t time_peer;
static uint64_t time_sent_us, cooldown_until, beacon_due, join_due, snapshot_due, close_due, replay_due, time_due, publish_due;
static uint64_t config_poll_due;
static uint32_t driver_generation;
static uint16_t replay_cursor;
static bool config_present, close_committed, checkpoint_dirty, host_latched, rejoining;
static uint32_t round_end_elapsed_ms=ZT_ROUND_DURATION_MS;
static uint32_t result_command_seq;
static bool result_authoritative;
/* Cosmetic dedupe is independent of durable gameplay command checkpoints. */
static zt_round_id_t announcement_round;
static uint32_t announcement_seq;
static uint64_t announcement_next_us;
static uint32_t gateway_serial;
static zt_boot_nonce_t gateway_boot;
static uint64_t gateway_seen_us, host_seen_us;
static struct { zt_boot_nonce_t boot; uint64_t seen_us; uint32_t packet_seq; bool online; } server_report;
static uint8_t gateway_hops;
static void peers_refresh(uint64_t now);
static bool is_host(void) { return host_latched && view.host_selected && view.host_configured; }
static void host_arm_command(unsigned ci,uint64_t now);
static void host_schedule_snapshot(bool roster);
static uint64_t host_request_due[ZT_MAX_PLAYERS];
static struct { zt_mac_t mac; uint64_t due; uint32_t request_nonce; } host_join_limits[ZT_MAX_PLAYERS];
static uint8_t host_join_next;
static struct {
    zt_checkpoint_t cp;
    uint64_t due, deadline, periodic_due;
    uint8_t requested, roster_requested, active, roster_page, role_page;
} host_snapshot;
static uint64_t host_metadata_due;
static zt_reset_receipt_t reset_record;
static bool reset_mesh_pending, reset_channel_pending;
static uint64_t reset_receipt_due;
static uint32_t host_control_seq;
static struct { zt_game_feed_item_t item; uint64_t retry_us; bool submitted; } host_control_request;
static struct {
    zt_round_id_t round;
    zt_mac_t macs[ZT_MAX_PLAYERS];
    uint32_t seq[ZT_MAX_PLAYERS], slots;
} reset_delivery;
static void reset_service(uint64_t now);

bool zt_game_is_owner(void);
zt_err_t zt_game_queue_clock(zt_round_id_t round,const zt_clock_sample_t *sample);
zt_err_t zt_game_queue_invalidation(uint32_t generation);
bool zt_game_is_owner(void) { return owner && owner==xTaskGetCurrentTaskHandle(); }
static bool mac_equal(const zt_mac_t *a,const zt_mac_t *b) { return !memcmp(a->bytes,b->bytes,6); }
static uint64_t registration_id(void)
{
    if (!view.registered || !join_nonce || !boot_nonce) return 0;
    uint64_t h=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<6;++i) { h^=view.self_mac.bytes[i]; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<8;++i) { h^=(boot_nonce>>(8*i))&255; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<4;++i) { h^=(join_nonce>>(8*i))&255; h*=UINT64_C(1099511628211); }
    return h ? h : 1;
}
static void clear_host_control(void)
{
    memset(&host_control_request,0,sizeof(host_control_request));
    view.host_control=ZT_HOST_CONTROL_NONE; view.host_control_pending=0; view.host_control_error=ZT_OK;
}
static bool host_start_allowed(void)
{
    return is_host() && view.error==ZT_ERROR_NONE && view.registered && join_nonce && boot_nonce &&
        !current.round_id && current.phase==ZT_PHASE_LOBBY;
}
static bool host_reset_allowed(void)
{
    return is_host() && view.error==ZT_ERROR_NONE && view.registered && join_nonce && boot_nonce &&
        current.round_id && current.phase>=ZT_PHASE_EXPIRED_PENDING_SYNC && result_authoritative &&
        view.result_present && view.winner<=ZT_ROLE_ZOMBIE;
}
static int roster_index(zt_slot_t slot)
{
    for (unsigned i=0;i<current.roster_count;++i) if (current.roster[i].slot==slot) return i;
    return -1;
}
static int origin_index(const zt_mac_t *mac)
{
    for (unsigned i=0;i<current.roster_count;++i) if (mac_equal(mac,&current.roster[i].mac)) return i;
    return -1;
}
static void remember_previous(const zt_checkpoint_t *cp)
{
    memcpy(previous_decision_received,decision_received,sizeof(decision_received));
    memset(&previous_round,0,sizeof(previous_round)); previous_round.round=cp->round_id; previous_round.self=ZT_SLOT_INVALID;
    for (unsigned i=0;i<cp->roster_count;++i) {
        unsigned slot=cp->roster[i].slot; previous_round.slots|=1u<<slot; previous_round.macs[slot]=cp->roster[i].mac;
        if (mac_equal(&view.self_mac,&cp->roster[i].mac)) previous_round.self=slot;
    }
}
static bool event_origin(zt_round_id_t round,unsigned slot,const zt_mac_t *origin)
{
    if (slot>=20) return false;
    if (round==current.round_id) { int i=roster_index(slot); return i>=0 && mac_equal(origin,&current.roster[i].mac); }
    return round==previous_round.round && (previous_round.slots&(1u<<slot)) && mac_equal(origin,&previous_round.macs[slot]);
}
static zt_wire_role_entry_t *self_role(void)
{
    int i=roster_index(view.self_slot); return i<0 ? NULL : &current.roles[i];
}
static void error_overlay(zt_err_t error)
{
    view.last_error=error;
    if (error==ZT_ERR_STORAGE) view.error=ZT_ERROR_STORAGE;
    else if (error==ZT_ERR_AUTH) view.error=ZT_ERROR_HOST_AUTH;
    else if (error==ZT_ERR_RADIO) view.error=ZT_ERROR_RADIO;
}
static void feedback(zt_feedback_t f,uint64_t now)
{
    if (f==ZT_FEEDBACK_GET_CLOSER && view.feedback==f && now<view.feedback_expires_us) return;
    view.feedback=f;
    unsigned duration=f==ZT_FEEDBACK_INFECTED ? ZT_LED_INFECTED_MS : f==ZT_FEEDBACK_TAG_CONFIRMED ? ZT_LED_TAG_CONFIRMED_MS : ZT_GET_CLOSER_RATE_LIMIT_MS;
    view.feedback_expires_us=now+duration*1000ULL;
}
static void server_status_observe(zt_boot_nonce_t boot,uint32_t seq,uint64_t rx,uint32_t age,bool online,uint64_t now)
{
    if (is_host() || !boot || !seq || rx>now || age>ZT_CLOCK_AGE_MAX_MS || rx<(uint64_t)age*1000ULL ||
        now-rx+(uint64_t)age*1000ULL>=ZT_SERVER_STATUS_MAX_AGE_MS*1000ULL) return;
    if (server_report.boot==boot && seq<=server_report.packet_seq) return;
    server_report.boot=boot; server_report.packet_seq=seq;
    server_report.seen_us=rx-(uint64_t)age*1000ULL; server_report.online=online;
}
static zt_err_t send_payload(zt_round_id_t round,zt_pkt_type_t type,const zt_wire_payload_t *p,zt_tx_priority_t priority)
{
    zt_err_t r=zt_mesh_submit(round,type,p,priority);
    if (r==ZT_ERR_RADIO) error_overlay(r);
    return r;
}
static zt_err_t feed(const zt_game_feed_item_t *item)
{
    zt_err_t r=ZT_ERR_INVALID_STATE;
    if (feed_sink) {
        r=feed_sink(item,sink_context);
        /* An absent gateway refuses custody; it has not dropped a delivery. */
        if (r!=ZT_OK && r!=ZT_ERR_INVALID_STATE) ++view.diagnostics.gateway_drops;
    }
    /* Every durable item is replayable from checkpoint/journal or by command
     * retry. A sink accepting a copy never causes evidence deletion. */
    return r;
}
static zt_err_t enqueue_locked(const input_t *in)
{
    if (qcount==ZT_EVENT_QUEUE_CAPACITY) { ++ingress_drops; return ZT_ERR_BUSY; }
    queue[(qhead+qcount)%ZT_EVENT_QUEUE_CAPACITY]=*in;
    if (++qcount>ingress_high_water) ingress_high_water=qcount;
    return ZT_OK;
}
static zt_err_t enqueue(const input_t *in)
{
    if (!initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&ingress_guard); zt_err_t r=enqueue_locked(in); portEXIT_CRITICAL(&ingress_guard);
    if (r==ZT_OK && owner) xTaskNotifyGive(owner);
    return r;
}
zt_err_t zt_game_queue_clock(zt_round_id_t round,const zt_clock_sample_t *sample)
{
    if (!sample) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_CLOCK,.round=round}; memcpy(in.bytes,sample,sizeof(*sample)); return enqueue(&in);
}
zt_err_t zt_game_queue_invalidation(uint32_t generation)
{
    input_t in={.type=IN_INVALIDATE,.generation=generation}; return enqueue(&in);
}
zt_err_t zt_game_post_button(const zt_button_edge_t *edge)
{
    if (!edge || edge->button>ZT_BUTTON_START || edge->kind>ZT_EDGE_REPEAT) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    if (edge->button==ZT_BUTTON_A && edge->kind==ZT_EDGE_REPEAT) return ZT_OK;
    portENTER_CRITICAL(&ingress_guard);
    if (edge->kind==ZT_EDGE_REPEAT) {
        for (unsigned i=0;i<bcount;++i) {
            zt_button_edge_t *old=&buttons[(bhead+i)%16];
            if (old->kind==ZT_EDGE_REPEAT && old->button==edge->button) { *old=*edge; portEXIT_CRITICAL(&ingress_guard); return ZT_OK; }
        }
        if (bcount>=12) { portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY; }
    }
    if (bcount==16) { ++ingress_drops; portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY; }
    buttons[(bhead+bcount++)%16]=*edge;
    portEXIT_CRITICAL(&ingress_guard);
    if (owner) xTaskNotifyGive(owner);
    return ZT_OK;
}
zt_err_t zt_game_post_persist(const zt_persist_completion_t *v)
{
    if (!v) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_PERSIST}; memcpy(in.bytes,v,sizeof(*v)); return enqueue(&in);
}
zt_err_t zt_game_post_decision(const zt_server_decision_t *v)
{
    if (!v || v->decision.victim_slot>=20 || !v->decision.event_seq || v->decision.status>ZT_DECISION_PENDING_DEPENDENCY) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_DECISION}; memcpy(in.bytes,v,sizeof(*v)); return enqueue(&in);
}
zt_err_t zt_game_post_receipt(const zt_server_receipt_t *v)
{
    if (!v || v->event_id.victim_slot>=20 || v->event_id.round_id!=v->round_id) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_RECEIPT}; memcpy(in.bytes,v,sizeof(*v)); return enqueue(&in);
}
zt_err_t zt_game_post_registration(zt_round_id_t round,const zt_wire_join_result_t *result)
{
    if (!result || !result->request_nonce) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_REGISTRATION,.round=round}; size_t n;
    zt_err_t r=zt_wire_encode_join_result(result,in.bytes,sizeof(in.bytes),&n);
    if (r!=ZT_OK) return r;
    in.len=(uint8_t)n;
    return enqueue(&in);
}
zt_err_t zt_game_post_control_result(zt_host_control_t action,uint32_t request_seq,zt_err_t result)
{
    if ((action!=ZT_HOST_CONTROL_START && action!=ZT_HOST_CONTROL_RESET) || !request_seq) return ZT_ERR_INVALID_ARG;
    input_t in={.type=IN_CONTROL_RESULT};
    control_result_t value={action,request_seq,result}; memcpy(in.bytes,&value,sizeof(value));
    return enqueue(&in);
}
static zt_err_t post_command(const zt_server_command_t *v,uint64_t rx,uint32_t age,uint8_t hops,bool server)
{
    if (!v || v->command.args_len>ZT_COMMAND_ARGS_MAX_BYTES) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&ingress_guard);
    int available=-1;
    for (unsigned i=0;i<8;++i) if (commands[i].occupied && commands[i].server==(uint8_t)server &&
        commands[i].value.round_id==v->round_id && commands[i].value.command.command_seq==v->command.command_seq) {
        const zt_wire_command_t *old=&commands[i].value.command, *cmd=&v->command;
        bool args_same=old->args_len==cmd->args_len && !memcmp(old->args,cmd->args,cmd->args_len);
        if (old->kind==ZT_CMD_START_ROUND && cmd->kind==ZT_CMD_START_ROUND &&
            old->args_len==ZT_START_ROUND_ARGS_BYTES && cmd->args_len==ZT_START_ROUND_ARGS_BYTES)
            args_same=!memcmp(old->args,cmd->args,15) && !memcmp(old->args+19,cmd->args+19,4);
        bool same_target=old->target_slot==cmd->target_slot || (!server &&
            (old->target_slot==ZT_SLOT_ALL || old->target_slot==view.self_slot) &&
            (cmd->target_slot==ZT_SLOT_ALL || cmd->target_slot==view.self_slot));
        bool same=old->kind==cmd->kind && same_target && old->args_len==cmd->args_len &&
            old->valid_until_elapsed_ms==cmd->valid_until_elapsed_ms && args_same;
        if (!same || commands[i].occupied!=COMMAND_CACHED) {
            portEXIT_CRITICAL(&ingress_guard); return same ? ZT_OK : ZT_ERR_CONFLICT;
        }
        available=i; break;
    }
    if (v->command.kind==ZT_CMD_ANNOUNCE) {
        unsigned count=0;
        for (unsigned i=0;i<ZT_PENDING_COMMAND_CAPACITY;++i)
            if (commands[i].occupied && commands[i].occupied!=COMMAND_CACHED &&
                commands[i].value.command.kind==ZT_CMD_ANNOUNCE) ++count;
        if (count>=ZT_ANNOUNCE_QUEUE_CAPACITY) { portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY; }
    }
    /* Keep one existing slot available for an explicit reset even when older
     * commands are waiting indefinitely for missing snapshot pages. */
    unsigned capacity=v->command.kind==ZT_CMD_RESET_GAME ? ZT_PENDING_COMMAND_CAPACITY : ZT_PENDING_COMMAND_CAPACITY-1;
    for (unsigned i=0;available<0 && i<capacity;++i) if (!commands[i].occupied) available=i;
    if (available<0) for (unsigned i=0;i<capacity;++i) {
        /* Role pages cannot reconstruct the frozen PREPARE rules or complete
         * terminal results. Keep those current-round commands for catch-up. */
        bool terminal=commands[i].value.round_id==current.round_id &&
            (commands[i].value.command.kind==ZT_CMD_PREPARE_ROUND || commands[i].value.command.kind==ZT_CMD_END_ROUND || commands[i].value.command.kind==ZT_CMD_FINAL_RESULT ||
                commands[i].value.command.kind==ZT_CMD_CANCEL_PREPARE);
        if (commands[i].occupied==COMMAND_CACHED && !terminal &&
            (available<0 || commands[i].deadline_us<commands[available].deadline_us)) available=i;
    }
    if (available>=0) {
        unsigned i=available;
        input_t in={.type=IN_COMMAND}; in.bytes[0]=i;
        zt_err_t r=enqueue_locked(&in);
        if (r==ZT_OK) commands[i]=(command_slot_t){.value=*v,.rx_us=rx,.age_ms=age,.hops=hops,.occupied=1,.server=server};
        portEXIT_CRITICAL(&ingress_guard); if (r==ZT_OK && owner) xTaskNotifyGive(owner); return r;
    }
    ++ingress_drops; portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY;
}
zt_err_t zt_game_post_command(const zt_server_command_t *v)
{
    return post_command(v,(uint64_t)esp_timer_get_time(),0,0,true);
}
zt_err_t zt_game_post_snapshot(const zt_server_snapshot_t *v)
{
    if (!v || v->page_count<1 || v->page_count>3 || v->page_index>=v->page_count || v->entry_count>8) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&ingress_guard);
    for (unsigned i=0;i<2;++i) if (!pages[i].occupied) {
        input_t in={.type=IN_PAGE}; in.bytes[0]=i;
        zt_err_t r=enqueue_locked(&in);
        if (r==ZT_OK) { pages[i].value.snapshot=*v; pages[i].server=1; pages[i].occupied=1; }
        portEXIT_CRITICAL(&ingress_guard); if (r==ZT_OK && owner) xTaskNotifyGive(owner); return r;
    }
    portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY;
}
static bool host_type(unsigned t)
{
    return t==ZT_PKT_COMMAND || t==ZT_PKT_ROSTER_PAGE || t==ZT_PKT_HOST_STATE || t==ZT_PKT_JOIN_RESULT ||
        t==ZT_PKT_WATERMARKS || t==ZT_PKT_CLOSE_RECEIPTS || t==ZT_PKT_EVENT_DECISIONS;
}
static bool direct_type(unsigned t)
{
    return t==ZT_PKT_BEACON || t==ZT_PKT_TAG_REQUEST || t==ZT_PKT_TAG_RESULT || t==ZT_PKT_TIME_QUERY ||
        t==ZT_PKT_TIME_REPLY || t==ZT_PKT_CACHE_PAGE || t==ZT_PKT_WANT_EVENTS || t==ZT_PKT_EVENT_COPY;
}
zt_err_t zt_game_post_domain(const zt_domain_message_t *m)
{
    if (!m) return ZT_ERR_INVALID_ARG;
    if (!initialized || !config_present) return ZT_ERR_INVALID_STATE;
    if (m->header.magic!=ZT_WIRE_MAGIC || m->header.protocol_version!=ZT_PROTOCOL_VERSION || m->header.game_id!=config.game_id ||
        m->header.reserved || m->header.hops>ZT_MAX_HOPS || m->header.payload_len>ZT_MAX_PAYLOAD) return ZT_ERR_PROTOCOL;
    if (host_type(m->header.type) && !mac_equal(&m->header.origin,&config.host_mac)) return ZT_ERR_AUTH;
    if (direct_type(m->header.type) && (m->header.hops || m->header.ttl_remaining || !mac_equal(&m->header.origin,&m->direct_source))) return ZT_ERR_AUTH;
    if (m->header.type==ZT_PKT_COMMAND) {
        zt_server_command_t v={.round_id=m->header.round_id,.command=m->payload.command};
        return post_command(&v,m->rx_us,m->header.age_ms,m->header.hops,false);
    }
    if (m->header.type==ZT_PKT_ROSTER_PAGE || m->header.type==ZT_PKT_HOST_STATE || m->header.type==ZT_PKT_WATERMARKS ||
        m->header.type==ZT_PKT_EVENT_DECISIONS || m->header.type==ZT_PKT_CACHE_PAGE || m->header.type==ZT_PKT_WANT_EVENTS) {
        portENTER_CRITICAL(&ingress_guard);
        for (unsigned i=0;i<2;++i) if (!pages[i].occupied) {
            input_t in={.type=IN_PAGE}; in.bytes[0]=i; zt_err_t r=enqueue_locked(&in);
            if (r==ZT_OK) { pages[i].value.radio=*m; pages[i].server=0; pages[i].occupied=1; }
            portEXIT_CRITICAL(&ingress_guard); if (r==ZT_OK && owner) xTaskNotifyGive(owner); return r;
        }
        portEXIT_CRITICAL(&ingress_guard); return ZT_ERR_BUSY;
    }
    input_t in={.rx_us=m->rx_us,.round=m->header.round_id,.boot=m->header.origin_boot_nonce,.origin=m->header.origin,
        .type=m->header.type,.hops=m->header.hops,.generation=m->driver_generation,.rssi=m->rssi,.channel=m->channel};
    size_t n=0; zt_err_t r;
    switch (m->header.type) {
    case ZT_PKT_BEACON:
        r=zt_wire_encode_beacon(&m->payload.beacon,in.bytes,50,&n);
        memcpy(in.bytes+50,&m->header.packet_seq,4);
        uint16_t beacon_age=m->header.age_ms>UINT16_MAX ? UINT16_MAX : (uint16_t)m->header.age_ms;
        memcpy(in.bytes+54,&beacon_age,2); break;
    case ZT_PKT_TAG_REQUEST: r=zt_wire_encode_tag_request(&m->payload.tag_request,in.bytes,56,&n); break;
    case ZT_PKT_TAG_RESULT: r=zt_wire_encode_tag_result(&m->payload.tag_result,in.bytes,56,&n); break;
    case ZT_PKT_JOIN_RESULT: r=zt_wire_encode_join_result(&m->payload.join_result,in.bytes,56,&n); break;
    case ZT_PKT_JOIN:
        /* Mesh forwards only accepted domain messages. Transit requests need
         * no local game queue, but must continue toward a multi-hop host. */
        if (!is_host()) return ZT_OK;
        r=zt_wire_encode_join(&m->payload.join,in.bytes,56,&n); break;
    case ZT_PKT_SNAPSHOT_REQUEST:
        if (!is_host()) return ZT_OK;
        r=zt_wire_encode_snapshot_request(&m->payload.snapshot_request,in.bytes,56,&n); break;
    case ZT_PKT_EVENT: r=zt_wire_encode_event(&m->payload.event,in.bytes,56,&n); break;
    case ZT_PKT_EVENT_COPY: r=zt_wire_encode_event_copy(&m->payload.event_copy,in.bytes,56,&n); break;
    case ZT_PKT_TIME_QUERY: r=zt_wire_encode_time_query(&m->payload.time_query,in.bytes,56,&n); break;
    case ZT_PKT_TIME_REPLY: r=zt_wire_encode_time_reply(&m->payload.time_reply,in.bytes,56,&n); break;
    case ZT_PKT_COMMAND_RECEIPT: r=zt_wire_encode_command_receipt(&m->payload.command_receipt,in.bytes,56,&n); break;
    case ZT_PKT_ROUND_CLOSED: r=zt_wire_encode_round_closed(&m->payload.round_closed,in.bytes,56,&n); break;
    case ZT_PKT_DECISION_RECEIPT: r=zt_wire_encode_decision_receipt(&m->payload.decision_receipt,in.bytes,56,&n); break;
    case ZT_PKT_CLOSE_RECEIPTS: r=zt_wire_encode_close_receipts(&m->payload.close_receipts,in.bytes,56,&n); break;
    default: return ZT_ERR_INVALID_ARG;
    }
    if (r!=ZT_OK || n!=m->header.payload_len) return ZT_ERR_PROTOCOL;
    in.len=n;
    /* Preserve eight slots for transaction and durable-completion traffic. */
    portENTER_CRITICAL(&ingress_guard);
    r=((in.type==ZT_PKT_BEACON || in.type==ZT_PKT_JOIN || in.type==ZT_PKT_SNAPSHOT_REQUEST) && qcount>=24) ? ZT_ERR_BUSY : enqueue_locked(&in);
    portEXIT_CRITICAL(&ingress_guard); if (r==ZT_OK && owner) xTaskNotifyGive(owner); return r;
}
static zt_err_t persist_sink_adapter(const zt_persist_completion_t *c,void *context)
{
    (void)context; return zt_game_post_persist(c);
}
/* Both boot and durable USB provisioning finish through the same path. No
 * radio startup is required here; mesh restoration retries from service(). */
static void configure_game(const zt_config_t *cfg)
{
    if (!mac_equal(&config.expected_mac,&view.self_mac)) { error_overlay(ZT_ERR_STORAGE); return; }
    /* The supplied configuration must be the persisted one. Never silently use
     * uncommitted provisioning to start a different game. */
    if (cfg && (cfg->game_id!=config.game_id || !mac_equal(&cfg->expected_mac,&config.expected_mac))) { error_overlay(ZT_ERR_STORAGE); return; }
    config_present=true; memcpy(view.name,config.name,sizeof(view.name));
    view.channel=config.last_channel;
    view.host_configured=mac_equal(&view.self_mac,&config.host_mac) && config.host_credentials_present;
    /* Configuration survives reboot; participation in a round does not. */
    memset(&current,0,sizeof(current));
    memset(&reset_record,0,sizeof(reset_record));
    view.admission=ZT_ADMISSION_LOBBY; view.phase=ZT_PHASE_LOBBY;
    view.round_id=0; view.self_slot=ZT_SLOT_INVALID; view.registered=0;
    rejoining=false; join_nonce=0;
    zt_game_peers_configure(config.game_id,NULL);
}
zt_err_t zt_game_init(const zt_config_t *cfg,zt_snapshot_sink_t ui,zt_game_feed_sink_t gateway,void *context)
{
    if (initialized) return ZT_ERR_INVALID_STATE;
    owner=xTaskGetCurrentTaskHandle();
    ui_sink=ui; feed_sink=gateway; sink_context=context;
    view=(zt_ui_snapshot_t){.admission=ZT_ADMISSION_BOOT,.role=ZT_ROLE_UNKNOWN,.self_slot=ZT_SLOT_INVALID,
        .selected_target=ZT_SLOT_INVALID,.brightness_cap=ZT_LED_COMPONENT_CAP};
    view.diagnostics.gateway_http_age_ms=UINT32_MAX;
    memcpy(view.build_id,ZT_BUILD_ID,ZT_BUILD_ID_LEN);
    if (esp_read_mac(view.self_mac.bytes,ESP_MAC_WIFI_STA)!=ESP_OK) return ZT_ERR_STORAGE;
    zt_store_status_t ss; zt_err_t r=zt_store_inspect(&view.self_mac,&ss);
    initialized=true; zt_game_clock_reset();
    if (r!=ZT_OK || ss.install_state==ZT_INSTALL_ERROR) { error_overlay(ZT_ERR_STORAGE); goto done; }
    if (ss.install_state==ZT_INSTALL_WAIT) { view.admission=ZT_ADMISSION_WAIT_INSTALL; goto done; }
    r=zt_store_open(&view.self_mac,persist_sink_adapter,NULL);
    if (r!=ZT_OK) { error_overlay(ZT_ERR_STORAGE); goto done; }
    r=zt_store_load_config(&config);
    if (r==ZT_ERR_NOT_FOUND) {
        view.admission=ZT_ADMISSION_NEEDS_CONFIG;
        config_poll_due=(uint64_t)esp_timer_get_time()+1000000ULL;
        goto done;
    }
    if (r!=ZT_OK || !mac_equal(&config.expected_mac,&view.self_mac)) { error_overlay(ZT_ERR_STORAGE); goto done; }
    configure_game(cfg);
 done:
    published=view;
    return view.error==ZT_ERROR_STORAGE ? ZT_ERR_STORAGE : ZT_OK;
}
static void configuration_service(uint64_t now)
{
    if (view.admission!=ZT_ADMISSION_NEEDS_CONFIG || config_present ||
        view.error==ZT_ERROR_STORAGE || now<config_poll_due) return;
    config_poll_due=now+1000000ULL;
    zt_err_t r=zt_store_load_config(&config);
    /* Persistence owns the I/O lock while committing. Only a durable value
     * can be adopted; lock contention and absence are the only retry cases. */
    if (r==ZT_ERR_NOT_FOUND || r==ZT_ERR_BUSY) return;
    if (r!=ZT_OK) error_overlay(ZT_ERR_STORAGE);
    else configure_game(NULL);
    publish_due=0;
}
static bool clock_running(uint64_t at,zt_clock_sample_t *sample)
{
    return !rejoining && current.phase==ZT_PHASE_RUNNING && view.error==ZT_ERROR_NONE &&
        zt_clock_read(at,sample)==ZT_OK && sample->quality==ZT_TIME_INITIALIZED && sample->elapsed_ms>=0 &&
        (uint64_t)sample->elapsed_ms+sample->uncertainty_ms<ZT_ROUND_DURATION_MS;
}
static zt_err_t submit(zt_persist_request_t *r,uint8_t action,uint64_t now)
{
    if (pending.id) return ZT_ERR_BUSY;
    if (next_request==UINT32_MAX) return ZT_ERR_OVERFLOW;
    r->request_id=++next_request;
    zt_err_t e=action==APPLY_CHECKPOINT && pending.final_present && r->value.checkpoint.phase==ZT_PHASE_FINAL ?
        zt_store_submit_final(r,&pending.final_result) : zt_store_submit(r);
    if (e==ZT_OK) { pending.id=r->request_id; pending.round=r->round_id; pending.kind=r->kind; pending.action=action; pending.submitted_us=now; pending.pending_sent=0; }
    else if (e==ZT_ERR_NO_SPACE) feedback(ZT_FEEDBACK_SYNC_REQUIRED,now);
    else if (e==ZT_ERR_STORAGE) error_overlay(e);
    return e;
}
static zt_err_t save_checkpoint(uint64_t now)
{
    zt_persist_request_t r={.kind=ZT_PERSIST_CHECKPOINT,.round_id=staged.round_id}; r.value.checkpoint=staged;
    return submit(&r,APPLY_CHECKPOINT,now);
}
static void send_result(zt_round_id_t round,const zt_wire_tag_result_t *r)
{
    zt_wire_payload_t p={.tag_result=*r}; send_payload(round,ZT_PKT_TAG_RESULT,&p,ZT_TX_PRIO_DIRECT_TAG);
}
static void cache_result(zt_round_id_t round,const zt_wire_tag_result_t *r,uint64_t now)
{
    unsigned i=outcome_next++%16;
    for (unsigned j=0;j<16;++j) if (outcomes[j].round==round && outcomes[j].result.actor_slot==r->actor_slot &&
        outcomes[j].result.request_boot==r->request_boot && outcomes[j].result.request_seq==r->request_seq) { i=j; break; }
    outcomes[i].round=round; outcomes[i].result=*r;
    outcomes[i].expires_us=now+ZT_TAG_OUTCOME_LIFETIME_MS*1000ULL; outcomes[i].repeat_us=now+ZT_TAG_RESULT_REPEAT_MS*1000ULL;
    send_result(round,r);
}
static void emit_event(zt_round_id_t round,const zt_wire_event_t *e)
{
    zt_wire_payload_t p={.event=*e}; send_payload(round,ZT_PKT_EVENT,&p,ZT_TX_PRIO_EVENT_CONTROL);
    zt_game_feed_item_t f={.kind=ZT_GAME_FEED_EVENT,.round_id=round,.body.event=*e}; feed(&f);
}
static void request_parent(const zt_wire_event_t *e)
{
    if (!e->actor_cause_seq) return;
    zt_wire_event_t parent;
    if (zt_store_read_event(current.round_id,e->actor_slot,e->actor_cause_seq,&parent)==ZT_OK) return;
    zt_wire_payload_t p={0}; p.want_events.target_slot=e->actor_slot; p.want_events.request_seq=e->event_seq;
    p.want_events.count=1; p.want_events.entries[0]=(zt_wire_cache_key_t){e->actor_slot,e->actor_cause_seq};
    send_payload(current.round_id,ZT_PKT_WANT_EVENTS,&p,ZT_TX_PRIO_CLOCK_REPAIR);
}
static void accept_request(const input_t *in,uint64_t now)
{
    zt_wire_tag_request_t req;
    if (zt_wire_decode_tag_request(in->bytes,in->len,&req)!=ZT_OK || in->round!=current.round_id ||
        req.victim_slot!=view.self_slot || in->generation!=driver_generation || in->channel!=current.round_channel) return;
    int actor=roster_index(req.actor_slot);
    if (actor<0 || !mac_equal(&in->origin,&current.roster[actor].mac) || req.actor_slot==view.self_slot) return;
    peers_refresh(now);
    zt_wire_tag_result_t result={.actor_slot=req.actor_slot,.request_boot=in->boot,.request_seq=req.attempt_seq};
    for (unsigned i=0;i<16;++i) if (outcomes[i].round==in->round && outcomes[i].expires_us>now &&
        outcomes[i].result.actor_slot==req.actor_slot && outcomes[i].result.request_boot==in->boot && outcomes[i].result.request_seq==req.attempt_seq) {
        send_result(in->round,&outcomes[i].result); return;
    }
    /* The durable attempt identity survives cache expiry and reboot. */
    zt_wire_event_t previous;
    if (zt_store_find_attempt(in->round,view.self_slot,req.actor_slot,in->boot,req.attempt_seq,&previous)==ZT_OK) {
        result.result=ZT_TAG_ACCEPTED; result.accepted_event=previous; cache_result(in->round,&result,now); return;
    }
    if (pending.id && pending.action==APPLY_EVENT) {
        result.result=pending.event.actor_slot==req.actor_slot && pending.event.request_boot==in->boot && pending.event.request_seq==req.attempt_seq ? ZT_TAG_PENDING : ZT_TAG_BUSY;
        send_result(in->round,&result); return;
    }
    zt_wire_role_entry_t *role=self_role();
    if (!role) return;
    zt_clock_sample_t occurrence;
    if (role->role==ZT_ROLE_ZOMBIE) result.result=ZT_TAG_ALREADY_ZOMBIE;
    else if (!clock_running(now,&occurrence)) result.result=ZT_TAG_ROUND_INACTIVE;
    else if (role->role!=ZT_ROLE_HUMAN || role->role_rev!=req.known_victim_role_rev) result.result=ZT_TAG_STALE_ACTOR;
    else {
        zt_peer_entry_t *peer=NULL;
        for (unsigned i=0;i<view.direct_contact_count;++i) if (view.contacts[i].slot==req.actor_slot) peer=&view.contacts[i];
        if (!peer || !peer->eligible || peer->latest_rssi<current.rules.tag_rssi ||
            in->rssi<current.rules.tag_rssi || req.tagger_observed_victim_rssi<current.rules.tag_rssi) result.result=ZT_TAG_OUT_OF_RANGE;
        else if (peer->role!=ZT_ROLE_ZOMBIE || peer->role_rev!=req.actor_role_rev || peer->cause.seq!=req.actor_cause_seq ||
            (!req.actor_cause_seq && req.actor_slot!=current.patient_zero_slot) ||
            current.roles[actor].role_rev>req.actor_role_rev || req.actor_elapsed_ms>=ZT_ROUND_DURATION_MS) result.result=ZT_TAG_STALE_ACTOR;
        else {
            zt_store_status_t ss;
            if (zt_store_status(&ss)!=ZT_OK || ss.install_state!=ZT_INSTALL_VALID || pending.id ||
                current.local_event_seq==UINT16_MAX || ss.event_count>=128) {
                result.result=ZT_TAG_BUSY; feedback(ZT_FEEDBACK_SYNC_REQUIRED,now);
            } else {
                zt_wire_event_t event={.victim_slot=view.self_slot,.event_seq=current.local_event_seq+1,.actor_slot=req.actor_slot,
                    .actor_cause_seq=req.actor_cause_seq,.actor_role_rev=req.actor_role_rev,.victim_prior_role_rev=role->role_rev,
                    .request_boot=in->boot,.request_seq=req.attempt_seq,.occurred_elapsed_ms=occurrence.elapsed_ms,
                    .uncertainty_ms=occurrence.uncertainty_ms,.tagger_observed_victim_rssi=req.tagger_observed_victim_rssi,
                    .victim_observed_tagger_rssi=in->rssi};
                zt_persist_request_t r={.kind=ZT_PERSIST_EVENT,.round_id=current.round_id,.value.event=event};
                if (submit(&r,APPLY_EVENT,now)==ZT_OK) { pending.event=event; return; }
                result.result=ZT_TAG_BUSY;
            }
        }
    }
    cache_result(in->round,&result,now);
}
static void command_receipt(const zt_wire_command_receipt_t *receipt)
{
    zt_wire_payload_t p={.command_receipt=*receipt}; send_payload(current.round_id,ZT_PKT_COMMAND_RECEIPT,&p,ZT_TX_PRIO_EVENT_CONTROL);
    zt_game_feed_item_t f={.kind=ZT_GAME_FEED_COMMAND_RECEIPT,.round_id=current.round_id,.body.command_receipt=*receipt}; feed(&f);
}
static void reset_to_lobby(void)
{
    zt_round_id_t round=reset_record.round_id;
    portENTER_CRITICAL(&view_guard); published_registration_id=0; portEXIT_CRITICAL(&view_guard);
    memset(&current,0,sizeof(current)); memset(&staged,0,sizeof(staged)); memset(&assembly,0,sizeof(assembly));
    memset(&outbound,0,sizeof(outbound)); memset(outcomes,0,sizeof(outcomes)); memset(&host_snapshot,0,sizeof(host_snapshot));
    memset(decision_received,0,sizeof(decision_received)); memset(decision_relay,0,sizeof(decision_relay));
    if (previous_round.round==round) memset(&previous_round,0,sizeof(previous_round));
    portENTER_CRITICAL(&ingress_guard);
    qhead=qcount=bhead=bcount=0; memset(commands,0,sizeof(commands)); memset(pages,0,sizeof(pages));
    portEXIT_CRITICAL(&ingress_guard);
    deferred_present=false;
    inventory_pages=0; inventory_round=0; event_retry_count=0; replay_cursor=previous_replay_cursor=0;
    rejoining=close_committed=checkpoint_dirty=false; join_nonce=0; join_due=time_sent_us=cooldown_until=0;
    round_end_elapsed_ms=ZT_ROUND_DURATION_MS;
    result_command_seq=0; result_authoritative=false;
    announcement_round=0; announcement_seq=0; announcement_next_us=0;
    beacon_due=host_metadata_due=snapshot_due=0;
    gateway_boot=gateway_seen_us=0; gateway_serial=0;
    zt_game_clock_reset(); zt_game_peers_configure(config.game_id,NULL);
    view.round_id=0; view.self_slot=ZT_SLOT_INVALID; view.registered=view.roster_count=view.ready_count=0;
    view.admission=ZT_ADMISSION_LOBBY; view.phase=ZT_PHASE_LOBBY; view.role=ZT_ROLE_UNKNOWN; view.role_rev=view.role_provisional=0;
    view.remaining_ms=ZT_ROUND_DURATION_MS; view.countdown_ms=0; view.selected_target=ZT_SLOT_INVALID;
    view.selected_name[0]=0; view.selected_tier=ZT_RANGE_UNKNOWN; view.direct_contact_count=0;
    view.feedback=ZT_FEEDBACK_NONE; view.feedback_expires_us=0; view.announcement[0]=0; view.announcement_expires_us=0;
    view.result_present=view.result_final=view.result_complete=0; view.winner=ZT_ROLE_UNKNOWN; view.missing_slots_bitmap=0;
    clear_host_control();
    reset_mesh_pending=reset_channel_pending=true; reset_receipt_due=0;
}
static void reset_service(uint64_t now)
{
    if (!reset_record.round_id) return;
    if (reset_mesh_pending) {
        if (zt_radio_mesh_reset(reset_record.round_id)!=ZT_OK) return;
        reset_mesh_pending=false;
    }
    if (reset_channel_pending) {
        zt_channel_status_t channel;
        if (zt_channel_get_status(&channel)!=ZT_OK) return;
        if (channel.state==ZT_CHANNEL_LOCKED) {
            zt_channel_unlock_after_expiry(reset_record.round_id);
            return;
        }
        if (channel.state==ZT_CHANNEL_RECOVERING) return;
        if (zt_channel_start_discovery(reset_record.channel)!=ZT_OK) return;
        reset_channel_pending=false;
    }
    if (view.registered && current.round_id && current.round_id!=reset_record.round_id) return;
    if (now<reset_receipt_due) return;
    zt_wire_payload_t p={.command_receipt={reset_record.slot,reset_record.command_seq,ZT_RECEIPT_APPLIED,ZT_DETAIL_NONE,0}};
    /* The receipt names the reset round even though ordinary beacons now name
     * lobby round zero. Receipt retries are retained only for this boot. */
    if (!is_host() && send_payload(reset_record.round_id,ZT_PKT_COMMAND_RECEIPT,&p,ZT_TX_PRIO_EVENT_CONTROL)!=ZT_OK) return;
    if (is_host()) {
        zt_game_feed_item_t item={.kind=ZT_GAME_FEED_COMMAND_RECEIPT,.round_id=reset_record.round_id,.body.command_receipt=p.command_receipt};
        feed(&item);
    }
    reset_receipt_due=now+ZT_JOIN_RETRY_MS*1000ULL;
}
static void refresh_role(void)
{
    zt_wire_role_entry_t *r=self_role();
    if (r) { view.role=r->role; view.role_rev=r->role_rev; view.role_provisional=!!(r->flags&ZT_ROLE_FLAG_PROVISIONAL); }
    view.phase=current.phase; view.round_id=current.round_id; view.channel=current.round_channel; view.roster_count=current.roster_count;
    if (!rejoining) view.admission=current.phase==ZT_PHASE_PREPARED ? ZT_ADMISSION_PREPARED : current.phase==ZT_PHASE_RUNNING ? ZT_ADMISSION_RUNNING :
        current.phase==ZT_PHASE_FINAL ? ZT_ADMISSION_FINAL : current.phase==ZT_PHASE_LOBBY ? ZT_ADMISSION_LOBBY : ZT_ADMISSION_EXPIRED_PENDING_SYNC;
}
static void display_end(zt_role_t winner,uint32_t elapsed_ms)
{
    round_end_elapsed_ms=elapsed_ms>ZT_ROUND_DURATION_MS ? ZT_ROUND_DURATION_MS : elapsed_ms;
    view.countdown_ms=0; view.remaining_ms=ZT_ROUND_DURATION_MS-round_end_elapsed_ms;
    view.winner=winner; view.result_present=1;
    if (current.phase!=ZT_PHASE_FINAL) { view.result_final=0; view.result_complete=0; view.missing_slots_bitmap=0; }
    outbound.active=false; view.feedback=ZT_FEEDBACK_NONE; view.feedback_expires_us=0;
    rejoining=false;
}
static void closed_feed(uint64_t now)
{
    if (!close_committed || view.self_slot>=20 || now<close_due) return;
    zt_wire_payload_t p={0}; p.round_closed=(zt_wire_round_closed_t){view.self_slot,current.produced[view.self_slot],view.role_rev,round_end_elapsed_ms};
    send_payload(current.round_id,ZT_PKT_ROUND_CLOSED,&p,ZT_TX_PRIO_EVENT_CONTROL);
    zt_game_feed_item_t f={.kind=ZT_GAME_FEED_ROUND_CLOSED,.round_id=current.round_id,.body.round_closed=p.round_closed}; feed(&f);
    close_due=now+ZT_ROUND_CLOSE_RETRY_MS*1000ULL;
}
static void completed(const zt_persist_completion_t *c,uint64_t now)
{
    if (!pending.id || c->request_id!=pending.id || c->kind!=pending.kind) return;
    uint8_t action=pending.action, host_command=pending.host_command, admission=pending.admission;
    pending.id=0; pending.host_command=0; pending.admission=0;
    if (c->result!=ZT_OK) {
        if (host_command) {
            portENTER_CRITICAL(&ingress_guard); commands[host_command-1].occupied=0; portEXIT_CRITICAL(&ingress_guard);
        }
        pending.final_present=0; pending.end_present=0;
        if (action==APPLY_EVENT) {
            zt_wire_tag_result_t result={.actor_slot=pending.event.actor_slot,.request_boot=pending.event.request_boot,
                .request_seq=pending.event.request_seq,.result=ZT_TAG_BUSY};
            cache_result(current.round_id,&result,now);
        }
        if (c->result==ZT_ERR_NO_SPACE) feedback(ZT_FEEDBACK_SYNC_REQUIRED,now); else error_overlay(ZT_ERR_STORAGE);
        if (action==APPLY_CHECKPOINT && pending.receipt.command_seq) {
            pending.receipt.state=ZT_RECEIPT_REJECTED; pending.receipt.detail=ZT_DETAIL_STORAGE_FAILURE; command_receipt(&pending.receipt);
        }
        return;
    }
    if (view.last_error==ZT_ERR_TIMEOUT) {
        zt_store_status_t ss;
        if (zt_store_status(&ss)==ZT_OK && ss.install_state==ZT_INSTALL_VALID) { view.error=ZT_ERROR_NONE; view.last_error=ZT_OK; }
    }
    if (action==APPLY_RESET) {
        reset_record=pending.reset;
        reset_to_lobby();
        reset_service(now);
    } else if (action==APPLY_EVENT) {
        const zt_wire_event_t *e=&pending.event;
        current.local_event_seq=e->event_seq; current.produced[view.self_slot]=e->event_seq;
        zt_wire_role_entry_t *r=self_role();
        r->role=ZT_ROLE_ZOMBIE; r->cause_slot=view.self_slot; r->cause_seq=e->event_seq; r->flags|=ZT_ROLE_FLAG_PROVISIONAL;
        refresh_role(); checkpoint_dirty=true; feedback(ZT_FEEDBACK_INFECTED,now);
        zt_wire_tag_result_t result={.actor_slot=e->actor_slot,.request_boot=e->request_boot,.request_seq=e->request_seq,
            .result=ZT_TAG_ACCEPTED,.accepted_event=*e};
        cache_result(current.round_id,&result,now); emit_event(current.round_id,e); request_parent(e);
        last_local_event=*e; event_retry_count=1; event_retry_us=now+700000ULL;
    } else if (action==APPLY_CHECKPOINT) {
        bool different=current.round_id!=staged.round_id;
        if (different && current.round_id) remember_previous(&current);
        current=staged; checkpoint_dirty=false;
        if (different) {
            zt_game_peers_configure(config.game_id,&current); close_committed=false; replay_cursor=0;
            memset(decision_received,0,sizeof(decision_received));
            round_end_elapsed_ms=ZT_ROUND_DURATION_MS;
            result_command_seq=0; result_authoritative=false;
            view.result_present=view.result_final=view.result_complete=0; view.winner=ZT_ROLE_UNKNOWN; view.missing_slots_bitmap=0;
        }
        for (unsigned i=0;i<current.roster_count;++i) if (mac_equal(&current.roster[i].mac,&view.self_mac)) view.self_slot=current.roster[i].slot;
        if (admission) {
            view.registered=1; rejoining=false;
            if (different) zt_game_clock_reset();
        }
        refresh_role();
        if (pending.end_present) {
            display_end(pending.end_winner,pending.end_elapsed_ms); pending.end_present=0;
            if (pending.result_authoritative) result_authoritative=true;
            if (pending.result_command_seq>result_command_seq) result_command_seq=pending.result_command_seq;
            refresh_role();
        }
        if (pending.final_present) {
            view.winner=pending.final_result.winner; view.result_present=1; view.result_final=1;
            view.result_complete=pending.final_result.complete; view.missing_slots_bitmap=pending.final_result.missing_slots_bitmap;
            pending.final_present=0;
        }
        if (pending.receipt.command_seq) command_receipt(&pending.receipt);
        pending.receipt.command_seq=0;
        if (is_host()) {
            host_schedule_snapshot(host_command && commands[host_command-1].value.command.kind==ZT_CMD_PREPARE_ROUND);
            if (host_command) host_arm_command(host_command-1,now);
        }
    } else if (action==APPLY_DECISION) {
        zt_wire_decision_entry_t *d=&pending.decision.decision;
        zt_slot_t own; uint16_t frontier;
        if (zt_store_decision_frontier(pending.decision.round_id,&own,&frontier)==ZT_OK) {
            if (pending.decision.round_id==current.round_id) current.decided[own]=frontier;
            zt_wire_payload_t p={0}; p.decision_receipt=(zt_wire_decision_receipt_t){own,frontier};
            send_payload(pending.decision.round_id,ZT_PKT_DECISION_RECEIPT,&p,ZT_TX_PRIO_EVENT_CONTROL);
            zt_game_feed_item_t f={.kind=ZT_GAME_FEED_DECISION_RECEIPT,.round_id=pending.decision.round_id,.body.decision_receipt=p.decision_receipt}; feed(&f);
        }
        int i=roster_index(d->victim_slot);
        if (i>=0 && !d->archived && pending.decision.round_id==current.round_id && d->status!=ZT_DECISION_PENDING_DEPENDENCY) {
            zt_wire_role_entry_t *r=&current.roles[i];
            if (r->cause_seq==d->event_seq && (r->flags&ZT_ROLE_FLAG_PROVISIONAL)) {
                if (d->status==ZT_DECISION_REJECTED) { r->role=ZT_ROLE_HUMAN; r->cause_slot=ZT_SLOT_INVALID; r->cause_seq=0; }
                r->flags&=~ZT_ROLE_FLAG_PROVISIONAL;
                if (r->covered_seq<d->event_seq) r->covered_seq=d->event_seq;
            }
            /* Only a contiguous decision can advance this compact frontier.
             * Gaps request a snapshot; recovery rebuilds from every decision. */
            if (d->event_seq==current.decided[d->victim_slot]+1u) current.decided[d->victim_slot]=d->event_seq;
            if (current.pending_roles[i].role_rev>r->role_rev && current.pending_roles[i].covered_seq>=current.produced[d->victim_slot]) *r=current.pending_roles[i];
            checkpoint_dirty=true; refresh_role();
            zt_wire_payload_t p={0}; p.decision_receipt=(zt_wire_decision_receipt_t){view.self_slot,current.decided[view.self_slot]};
            send_payload(current.round_id,ZT_PKT_DECISION_RECEIPT,&p,ZT_TX_PRIO_EVENT_CONTROL);
            zt_game_feed_item_t f={.kind=ZT_GAME_FEED_DECISION_RECEIPT,.round_id=current.round_id,.body.decision_receipt=p.decision_receipt}; feed(&f);
        }
    } else if (action==APPLY_CLOSE) {
        close_committed=true; current.closed_bitmap|=1u<<view.self_slot; closed_feed(now);
    } else if (action==APPLY_RECEIPT) {
        current=staged; checkpoint_dirty=false;
    } else { /* Remote event custody, never a local role transition. */
        emit_event(pending.round,&pending.event);
    }
}
static void request_snapshot(uint64_t now)
{
    if (now<snapshot_due || !config_present || !join_nonce || !view.registered) return;
    /* A registration slot is not yet a member of a frozen round. Round-zero
     * requests must use the unknown-slot form so mesh admission can deliver
     * them to the host, which validates the requester's authenticated MAC. */
    zt_wire_payload_t p={0}; p.snapshot_request=(zt_wire_snapshot_request_t){current.round_id ? view.self_slot : ZT_SLOT_INVALID,current.snapshot_rev,current.roster_hash,
        ZT_NEED_ROSTER|ZT_NEED_ROLES_PHASE_TIME|ZT_NEED_WATERMARKS|ZT_NEED_DECISIONS};
    uint32_t jitter=(uint32_t)(boot_nonce^(now/1000))%501;
    if (send_payload(current.round_id,ZT_PKT_SNAPSHOT_REQUEST,&p,ZT_TX_PRIO_CLOCK_REPAIR)!=ZT_OK) {
        snapshot_due=now+(200+jitter)*1000ULL;
        return;
    }
    /* Initial admission has no periodic role pages to fall back on. Retry a
     * lost roster request sooner, while established rounds retain the cap. */
    uint32_t interval=current.round_id ? ZT_SNAPSHOT_REQUEST_INTERVAL_MS : 1000;
    snapshot_due=now+(interval+jitter)*1000ULL;
}
static bool apply_clock(int32_t elapsed,uint32_t uncertainty,uint64_t rx,uint32_t age,uint8_t hops)
{
    uint64_t now=(uint64_t)esp_timer_get_time();
    if (elapsed==INT32_MIN || rx>now || age>ZT_CLOCK_AGE_MAX_MS || hops>=ZT_MAX_HOPS || now-rx>ZT_CLOCK_QUEUE_MAX_MS*1000ULL) return false;
    int64_t advanced=(int64_t)elapsed+age;
    uint64_t margin=(uint64_t)uncertainty+age+ZT_CLOCK_LINK_UNCERTAINTY_MS*(hops+1u);
    if (advanced>INT32_MAX || advanced<=INT32_MIN || margin>ZT_TIME_UNCERTAINTY_MAX_MS) return false;
    zt_clock_sample_t s={(int32_t)advanced,(uint32_t)margin,rx,ZT_TIME_INITIALIZED};
    return zt_clock_apply(current.round_id,&s)==ZT_OK;
}
static bool assembly_begin(zt_round_id_t round,uint32_t rev,uint64_t hash,uint8_t source)
{
    if (!round || !rev) return false;
    if (current.round_id==round && rev<current.snapshot_rev) return false;
    if (assembly.round==round && assembly.rev==rev && assembly.hash==hash)
        return !assembly.invalid && assembly.source==source;
    if (assembly.round==round && rev<=assembly.rev) return false;
    memset(&assembly,0,sizeof(assembly)); assembly.round=round; assembly.rev=rev; assembly.hash=hash;
    assembly.source=source;
    return true;
}
static bool roster_page(const zt_wire_roster_page_t *p,zt_round_id_t round,uint8_t source)
{
    if (!p->page_count || p->page_count>3 || p->page_index>=p->page_count || !p->entry_count || p->entry_count>8 ||
        (p->page_index+1<p->page_count && p->entry_count!=8) || !assembly_begin(round,p->snapshot_rev,p->roster_hash,source)) return false;
    unsigned base=p->page_index*8;
    if (base+p->entry_count>20 || (assembly.roster_pages && assembly.roster_pages!=p->page_count)) return false;
    if (assembly.roster_mask&(1u<<p->page_index)) return !memcmp(&assembly.roster[base],p->entries,p->entry_count*sizeof(p->entries[0]));
    for (unsigned i=0;i<p->entry_count;++i) {
        const zt_wire_roster_entry_t *r=&p->entries[i];
        uint8_t bytes[20]; size_t n;
        if (r->slot>=20 || (assembly.roster_slots&(1u<<r->slot)) || zt_wire_encode_roster_entry(r,bytes,sizeof(bytes),&n)!=ZT_OK) return false;
    }
    for (unsigned i=0;i<p->entry_count;++i) {
        assembly.roster[base+i]=p->entries[i]; assembly.roster_slots|=1u<<p->entries[i].slot;
    }
    assembly.roster_count+=p->entry_count; assembly.roster_pages=p->page_count; assembly.roster_mask|=1u<<p->page_index;
    if (assembly.roster_mask==(1u<<assembly.roster_pages)-1) {
        uint64_t hash;
        if (zt_wire_roster_hash(assembly.roster,assembly.roster_count,&hash)!=ZT_OK || hash!=assembly.hash) { assembly.complete=0; return false; }
        for (unsigned i=1;i<assembly.roster_count;++i) if (assembly.roster[i-1].slot>=assembly.roster[i].slot) return false;
        for (unsigned i=0;i<assembly.roster_count;++i) for (unsigned j=0;j<i;++j) if (mac_equal(&assembly.roster[i].mac,&assembly.roster[j].mac)) return false;
        assembly.complete=1;
    }
    return true;
}
static bool own_events_pending(void)
{
    return view.self_slot<20 && current.produced[view.self_slot]>current.decided[view.self_slot] && !(current.cleared_bitmap&(1u<<view.self_slot));
}
static void merge_role(zt_checkpoint_t *cp,const zt_wire_role_entry_t *role)
{
    for (unsigned i=0;i<cp->roster_count;++i) if (cp->roster[i].slot==role->slot) {
        zt_wire_role_entry_t *old=&cp->roles[i];
        if (role->role_rev<=old->role_rev) return;
        if (role->covered_seq<cp->produced[role->slot]) cp->pending_roles[i]=*role;
        else *old=*role;
        return;
    }
}
static void host_schedule_snapshot(bool roster)
{
    if (!is_host()) return;
    host_snapshot.requested=1;
    if (roster) host_snapshot.roster_requested=1;
}
static void host_arm_command(unsigned ci,uint64_t now)
{
    if (!is_host() || !commands[ci].server) return;
    command_slot_t *slot=&commands[ci];
    slot->targets=0;
    for (unsigned i=0;i<current.roster_count;++i) {
        unsigned target=current.roster[i].slot;
        if (target!=view.self_slot && (slot->value.command.target_slot==ZT_SLOT_ALL || slot->value.command.target_slot==target))
            slot->targets|=1u<<target;
    }
    slot->deadline_us=now+HOST_COMMAND_ATTEMPTS*ZT_COMMAND_RETRY_MS*1000ULL;
    slot->first_broadcast=slot->value.command.target_slot==ZT_SLOT_ALL;
    if (slot->value.command.kind==ZT_CMD_PREPARE_ROUND) {
        host_schedule_snapshot(true);
        int self=roster_index(view.self_slot);
        if (self>=0 && slot->value.command.target_slot==ZT_SLOT_ALL) current.roles[self].flags|=ZT_ROLE_FLAG_READY;
        view.ready_count=0;
        for (unsigned i=0;i<current.roster_count;++i) if (current.roles[i].flags&ZT_ROLE_FLAG_READY) ++view.ready_count;
    }
    portENTER_CRITICAL(&ingress_guard); slot->occupied=COMMAND_DELIVERING; portEXIT_CRITICAL(&ingress_guard);
}
static bool host_receipt(zt_round_id_t round,const zt_wire_command_receipt_t *receipt)
{
    for (unsigned i=0;i<8;++i) {
        command_slot_t *slot=&commands[i];
        if (slot->occupied!=COMMAND_DELIVERING || slot->value.round_id!=round ||
            slot->value.command.command_seq!=receipt->command_seq || !(slot->targets&(1u<<receipt->slot))) continue;
        bool ready=slot->value.command.kind==ZT_CMD_PREPARE_ROUND && receipt->state==ZT_RECEIPT_PREPARED_READY;
        if (ready) {
            zt_wire_prepare_round_args_t a;
            if (zt_wire_decode_prepare_round_args(slot->value.command.args,slot->value.command.args_len,&a)!=ZT_OK ||
                receipt->applied_snapshot_rev!=a.snapshot_rev) return false;
        }
        if (ready || (receipt->state==ZT_RECEIPT_APPLIED && slot->value.command.kind!=ZT_CMD_PREPARE_ROUND) ||
            receipt->state==ZT_RECEIPT_REJECTED) slot->targets&=~(1u<<receipt->slot);
        if (receipt->state==ZT_RECEIPT_REQUIRES_SNAPSHOT) host_schedule_snapshot(true);
        return ready;
    }
    return false;
}
static void host_replay_commands(zt_slot_t target,uint64_t now)
{
    /* Finished commands are a bounded, replaceable cache in the same eight
     * slots. Explicit rejoin can recover END/FINAL/CANCEL as well as role pages. */
    portENTER_CRITICAL(&ingress_guard);
    for (unsigned i=0;i<8;++i) {
        command_slot_t *slot=&commands[i];
        if ((slot->occupied!=COMMAND_CACHED && slot->occupied!=COMMAND_DELIVERING) || slot->value.round_id!=current.round_id ||
            slot->value.command.kind==ZT_CMD_ANNOUNCE ||
            (slot->value.command.target_slot!=ZT_SLOT_ALL && slot->value.command.target_slot!=target)) continue;
        if (slot->occupied==COMMAND_CACHED) {
            slot->targets=0; slot->deadline_us=now+HOST_COMMAND_ATTEMPTS*ZT_COMMAND_RETRY_MS*1000ULL;
            slot->first_broadcast=0;
        }
        if (!(slot->targets&(1u<<target))) {
            slot->targets|=1u<<target; slot->attempts[target]=0; slot->retry_us[target]=0;
        }
        slot->occupied=COMMAND_DELIVERING;
    }
    portEXIT_CRITICAL(&ingress_guard);
}
static void host_join(const input_t *in,uint64_t now)
{
    zt_wire_join_t j;
    if (!is_host() || zt_wire_decode_join(in->bytes,in->len,&j)!=ZT_OK ||
        !j.request_nonce || !mac_equal(&j.requested_badge_mac,&in->origin) ||
        mac_equal(&in->origin,&view.self_mac)) return;
    unsigned limit=ZT_MAX_PLAYERS;
    for (unsigned i=0;i<ZT_MAX_PLAYERS;++i) if (mac_equal(&host_join_limits[i].mac,&in->origin)) { limit=i; break; }
    if (limit<ZT_MAX_PLAYERS && now<host_join_limits[limit].due) return;
    if (limit==ZT_MAX_PLAYERS) limit=host_join_next++%ZT_MAX_PLAYERS;
    host_join_limits[limit].mac=in->origin; host_join_limits[limit].due=now+ZT_JOIN_RETRY_MS*1000ULL;
    host_join_limits[limit].request_nonce=j.request_nonce;
    /* Different badges may register in the same receive slice. Per-MAC bounds
     * and the bounded gateway queue limit load without discarding their JOINs. */
    int i=origin_index(&in->origin);
    zt_wire_payload_t p={0};
    p.join_result=(zt_wire_join_result_t){.target_mac=in->origin,.request_nonce=j.request_nonce,
        .status=i<0 ? ZT_JOIN_WAITING_FOR_SERVER : ZT_JOIN_REJOINED,
        .slot=i<0 ? ZT_SLOT_INVALID : current.roster[i].slot,.snapshot_rev=current.snapshot_rev};
    if (i<0) {
        /* Authentication and rate limits have already been applied. The backend
         * decides whether this badge can join; a missing roster is not closed. */
        zt_game_feed_item_t item={.kind=ZT_GAME_FEED_JOIN,.round_id=in->round,
            .body.join={.request=j,.boot_nonce=in->boot}};
        feed(&item); /* BUSY retains no custody; the badge retries this nonce. */
    }
    send_payload(current.round_id,ZT_PKT_JOIN_RESULT,&p,ZT_TX_PRIO_EVENT_CONTROL);
    if (i>=0) {
        host_schedule_snapshot(true); /* Roles follow; JOIN_RESULT has no role fields. */
        host_replay_commands(current.roster[i].slot,now);
    }
}
static void apply_join_result(const zt_wire_join_result_t *j,uint64_t now)
{
    if (!mac_equal(&j->target_mac,&view.self_mac) || !join_nonce || j->request_nonce!=join_nonce) return;
    if (j->status==ZT_JOIN_REGISTERED || j->status==ZT_JOIN_REJOINED) {
        if (view.self_slot!=ZT_SLOT_INVALID && current.round_id && j->slot!=view.self_slot) { error_overlay(ZT_ERR_AUTH); return; }
        view.self_slot=j->slot; view.registered=1;
        if (!current.round_id) view.admission=ZT_ADMISSION_WAITING_FOR_ROUND;
        request_snapshot(now);
    } else if (j->status==ZT_JOIN_REGISTRATION_CLOSED || j->status==ZT_JOIN_ROOM_FULL) {
        join_nonce=0;
        view.admission=ZT_ADMISSION_NEXT_ROUND;
    } else if (j->status==ZT_JOIN_BAD_CONFIGURATION) {
        join_nonce=0;
        error_overlay(ZT_ERR_AUTH);
    }
}
static bool registration_result(const input_t *in,uint64_t now)
{
    zt_wire_join_result_t j;
    if (!is_host() || zt_wire_decode_join_result(in->bytes,in->len,&j)!=ZT_OK) return true;
    if (mac_equal(&j.target_mac,&view.self_mac)) {
        apply_join_result(&j,now);
        return true;
    }
    bool requested=false;
    for (unsigned i=0;i<ZT_MAX_PLAYERS;++i)
        if (mac_equal(&host_join_limits[i].mac,&j.target_mac) && host_join_limits[i].request_nonce==j.request_nonce) requested=true;
    if (!requested) return true; /* A newer request supersedes a delayed result. */
    zt_wire_payload_t p={.join_result=j};
    if (send_payload(in->round,ZT_PKT_JOIN_RESULT,&p,ZT_TX_PRIO_EVENT_CONTROL)!=ZT_OK) return false;
    int i=origin_index(&j.target_mac);
    if (j.status<=ZT_JOIN_REJOINED && in->round==current.round_id && i>=0 && current.roster[i].slot==j.slot) {
        host_schedule_snapshot(true);
        host_replay_commands(j.slot,now);
    }
    return true;
}
static void host_snapshot_request(const input_t *in,uint64_t now)
{
    zt_wire_snapshot_request_t r;
    if (!is_host() || !current.round_id || zt_wire_decode_snapshot_request(in->bytes,in->len,&r)!=ZT_OK ||
        (in->round && in->round!=current.round_id)) return;
    int i=origin_index(&in->origin);
    if (i<0 || (r.slot!=ZT_SLOT_INVALID && r.slot!=current.roster[i].slot) || now<host_request_due[i]) return;
    host_request_due[i]=now+(in->round ? ZT_SNAPSHOT_REQUEST_INTERVAL_MS : 1000)*1000ULL;
    host_schedule_snapshot(!!(r.need_flags&ZT_NEED_ROSTER));
    host_replay_commands(current.roster[i].slot,now);
}
static bool process_command(unsigned ci,uint64_t now)
{
    command_slot_t *slot=&commands[ci];
    if (!slot->occupied) return true;
    const zt_server_command_t *msg=&slot->value;
    const zt_wire_command_t *cmd=&msg->command;
    if (pending.id) return false;
    if (cmd->kind==ZT_CMD_RESET_GAME && reset_record.round_id==msg->round_id &&
        reset_record.command_seq==cmd->command_seq && reset_record.slot==cmd->target_slot) {
        reset_receipt_due=0;
        return true;
    }
    if (msg->round_id==reset_record.round_id) return true;
    if (cmd->kind==ZT_CMD_RESET_GAME) {
        if (!msg->round_id || !cmd->command_seq || cmd->target_slot>=ZT_MAX_PLAYERS ||
            (cmd->args_len!=0 && cmd->args_len!=14)) return true;
        zt_mac_t target_mac={0}; uint64_t target_registration=0;
        if (cmd->args_len==14) {
            memcpy(target_mac.bytes,cmd->args,6);
            for (unsigned i=0;i<8;++i) target_registration|=(uint64_t)cmd->args[6+i]<<(8*i);
            if (!target_registration) return true;
        }
        bool self=cmd->args_len ? mac_equal(&target_mac,&view.self_mac) : cmd->target_slot==view.self_slot;
        if (is_host() && slot->server && !self) {
            if (!cmd->args_len) {
                int index=roster_index(cmd->target_slot);
                if (msg->round_id!=current.round_id || index<0) return true;
                target_mac=current.roster[index].mac;
            }
            /* The backend retries pending cleanup in bounded batches. Do not
             * occupy the eight gameplay command slots for an entire roster. */
            zt_wire_payload_t p={.command=*cmd};
            if (send_payload(msg->round_id,ZT_PKT_COMMAND,&p,ZT_TX_PRIO_EVENT_CONTROL)!=ZT_OK) return false;
            if (reset_delivery.round!=msg->round_id) {
                memset(&reset_delivery,0,sizeof(reset_delivery)); reset_delivery.round=msg->round_id;
            }
            reset_delivery.macs[cmd->target_slot]=target_mac;
            reset_delivery.seq[cmd->target_slot]=cmd->command_seq;
            reset_delivery.slots|=1u<<cmd->target_slot;
            return true;
        }
        if (!self || !join_nonce || !view.registered || cmd->target_slot!=view.self_slot ||
            (current.round_id && msg->round_id!=current.round_id) ||
            (cmd->args_len && target_registration!=registration_id())) return true;
        zt_persist_request_t request={.kind=ZT_PERSIST_RESET_ROUND,.round_id=msg->round_id};
        request.value.reset=(zt_reset_receipt_t){msg->round_id,cmd->command_seq,view.self_slot,view.channel};
        if (submit(&request,APPLY_RESET,now)!=ZT_OK) return false;
        pending.reset=request.value.reset;
        outbound.active=false;
        return true;
    }
    if (!join_nonce || !view.registered) return true;
    bool originate=is_host() && slot->server;
    if (originate) {
        uint8_t bytes[ZT_MAX_PAYLOAD]; size_t n;
        if (!cmd->command_seq || zt_wire_encode_command(cmd,bytes,sizeof(bytes),&n)!=ZT_OK) { view.last_error=ZT_ERR_PROTOCOL; return true; }
        if (cmd->target_slot!=ZT_SLOT_ALL && cmd->target_slot!=view.self_slot) {
            if (msg->round_id!=current.round_id || roster_index(cmd->target_slot)<0) { view.last_error=ZT_ERR_STALE; return true; }
            /* A remote target owns application. In particular, never apply its
             * ROLE_SET to the host or synthesize an APPLIED receipt for it. */
            if (cmd->kind==ZT_CMD_ROLE_SET) {
                zt_wire_role_set_args_t a;
                if (zt_wire_decode_role_set_args(cmd->args,cmd->args_len,&a)!=ZT_OK) return true;
                staged=current;
                zt_wire_role_entry_t role={.slot=cmd->target_slot,.role=a.role,.role_rev=a.role_rev,
                    .cause_slot=a.cause_slot,.cause_seq=a.cause_seq,.covered_seq=a.covered_seq};
                merge_role(&staged,&role);
                if (memcmp(&staged,&current,sizeof(current))) {
                    pending.receipt.command_seq=0; pending.final_present=0; pending.end_present=0;
                    if (save_checkpoint(now)!=ZT_OK) return false;
                    pending.host_command=ci+1;
                    portENTER_CRITICAL(&ingress_guard); slot->occupied=COMMAND_PERSISTING; portEXIT_CRITICAL(&ingress_guard);
                    return true;
                }
            }
            host_arm_command(ci,now); return true;
        }
    }
    if (cmd->target_slot!=ZT_SLOT_ALL && cmd->target_slot!=view.self_slot) return true;
    zt_wire_command_receipt_t receipt={view.self_slot,cmd->command_seq,ZT_RECEIPT_REJECTED,ZT_DETAIL_INVALID_ARGS,current.snapshot_rev};
    if (!cmd->command_seq) return true;
    if (current.round_id && msg->round_id!=current.round_id && !originate) {
        for (unsigned i=0;i<8;++i) if (current.applied_command_seq[i]==cmd->command_seq) {
            receipt.detail=ZT_DETAIL_WRONG_ROUND; command_receipt(&receipt); return true;
        }
    }
    if (cmd->kind==ZT_CMD_PREPARE_ROUND && msg->round_id==current.round_id && current.phase>=ZT_PHASE_RUNNING) {
        /* Reconnect seeds the host's bounded relay cache even after the old
         * applied-command sequence has aged out. Only the exact frozen rules
         * qualify; acknowledging them never restarts or downgrades the round. */
        uint8_t rules[ZT_PREPARE_ROUND_ARGS_BYTES]; size_t n;
        if (zt_wire_encode_prepare_round_args(&current.rules,rules,sizeof(rules),&n)==ZT_OK &&
            n==cmd->args_len && !memcmp(rules,cmd->args,n)) {
            receipt.state=ZT_RECEIPT_PREPARED_READY; receipt.detail=ZT_DETAIL_NONE;
            receipt.applied_snapshot_rev=current.rules.snapshot_rev;
            command_receipt(&receipt); if (originate) host_arm_command(ci,now);
        }
        return true;
    }
    for (unsigned i=0;i<8;++i) if (current.applied_command_seq[i]==cmd->command_seq && msg->round_id==current.round_id) {
        receipt.state=cmd->kind==ZT_CMD_PREPARE_ROUND ? ZT_RECEIPT_PREPARED_READY : ZT_RECEIPT_APPLIED;
        receipt.detail=ZT_DETAIL_NONE;
        if (cmd->kind==ZT_CMD_START_ROUND && rejoining && current.phase==ZT_PHASE_RUNNING) {
            zt_wire_start_round_args_t a;
            if (zt_wire_decode_start_round_args(cmd->args,cmd->args_len,&a)==ZT_OK && a.roster_hash==current.roster_hash &&
                apply_clock(a.sampled_elapsed_ms,a.uncertainty_ms,slot->rx_us,slot->age_ms,slot->hops)) { rejoining=false; refresh_role(); }
        }
        if (cmd->kind==ZT_CMD_FINAL_RESULT && current.phase==ZT_PHASE_FINAL) {
            zt_wire_final_result_args_t a;
            if (zt_wire_decode_final_result_args(cmd->args,cmd->args_len,&a)==ZT_OK && a.state_rev==current.state_rev) {
                view.winner=a.winner; view.result_present=1; view.result_final=1; view.result_complete=a.complete; view.missing_slots_bitmap=a.missing_slots_bitmap;
            }
        }
        command_receipt(&receipt); if (originate) host_arm_command(ci,now); return true;
    }
    if (cmd->kind==ZT_CMD_ANNOUNCE) {
        zt_wire_announce_args_t a; zt_clock_sample_t clock;
        if (msg->round_id!=current.round_id) return true;
        if (zt_wire_decode_announce_args(cmd->args,cmd->args_len,&a)!=ZT_OK ||
            cmd->valid_until_elapsed_ms==ZT_COMMAND_NO_EXPIRY) {
            command_receipt(&receipt); return true;
        }
        if (announcement_round!=current.round_id) {
            announcement_round=current.round_id; announcement_seq=0;
        }
        if (cmd->command_seq<=announcement_seq) {
            receipt.state=cmd->command_seq==announcement_seq ? ZT_RECEIPT_APPLIED : ZT_RECEIPT_REJECTED;
            receipt.detail=cmd->command_seq==announcement_seq ? ZT_DETAIL_NONE : ZT_DETAIL_STALE_REVISION;
            command_receipt(&receipt);
            if (originate && cmd->command_seq==announcement_seq) host_arm_command(ci,now);
            return true;
        }
        if (current.phase!=ZT_PHASE_RUNNING || zt_clock_read(now,&clock)!=ZT_OK ||
            clock.quality!=ZT_TIME_INITIALIZED || clock.elapsed_ms<0 ||
            (uint32_t)clock.elapsed_ms>=cmd->valid_until_elapsed_ms) {
            receipt.detail=ZT_DETAIL_STALE_REVISION; command_receipt(&receipt); return true;
        }
        /* Leave a bounded waiting slot; critical commands are selected first. */
        if (now<announcement_next_us) return false;
        uint32_t display_ms=cmd->valid_until_elapsed_ms-(uint32_t)clock.elapsed_ms;
        if (display_ms>ZT_ANNOUNCE_MAX_DISPLAY_MS) display_ms=ZT_ANNOUNCE_MAX_DISPLAY_MS;
        memcpy(view.announcement,a.text,a.text_len); view.announcement[a.text_len]=0;
        view.announcement_expires_us=now+display_ms*1000ULL;
        announcement_next_us=now+ZT_ANNOUNCE_MIN_INTERVAL_MS*1000ULL;
        announcement_seq=cmd->command_seq;
        receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE;
        command_receipt(&receipt);
        if (originate) host_arm_command(ci,now);
        return true;
    }
    if (cmd->valid_until_elapsed_ms!=ZT_COMMAND_NO_EXPIRY) { command_receipt(&receipt); return true; }
    if (cmd->kind!=ZT_CMD_PREPARE_ROUND && msg->round_id!=current.round_id) {
        receipt.detail=ZT_DETAIL_WRONG_ROUND; command_receipt(&receipt); return true;
    }
    staged=current; pending.final_present=0; pending.end_present=0;
    switch (cmd->kind) {
    case ZT_CMD_PREPARE_ROUND: {
        zt_wire_prepare_round_args_t a;
        if (zt_wire_decode_prepare_round_args(cmd->args,cmd->args_len,&a)!=ZT_OK || a.duration_ms!=ZT_ROUND_DURATION_MS ||
            a.tag_cooldown_ms!=ZT_TAG_COOLDOWN_MS || a.roster_count<2 || a.roster_count>20 || a.channel<1 || a.channel>11) break;
        if (current.round_id==msg->round_id && (a.roster_hash!=current.roster_hash || a.channel!=current.round_channel ||
            a.tag_rssi!=current.rules.tag_rssi || a.roster_count!=current.roster_count)) break;
        if (current.round_id==msg->round_id && current.phase>=ZT_PHASE_RUNNING) { receipt.detail=ZT_DETAIL_STALE_REVISION; break; }
        if (current.round_id!=msg->round_id && current.round_id && zt_store_round_retained(current.round_id) && (current.phase!=ZT_PHASE_FINAL || own_events_pending())) {
            receipt.detail=ZT_DETAIL_OLD_EVENTS_PENDING; break;
        }
        /* A prior registration cannot bypass the server snapshot's admission
         * checkpoint, even when its roster pages have already arrived. */
        if (!view.registered || (assembly.source==ASSEMBLY_SERVER && current.round_id!=msg->round_id) ||
            assembly.invalid || !assembly.complete || assembly.round!=msg->round_id || assembly.rev<a.snapshot_rev ||
            assembly.hash!=a.roster_hash || assembly.roster_count!=a.roster_count) {
            receipt.state=ZT_RECEIPT_REQUIRES_SNAPSHOT; receipt.detail=ZT_DETAIL_MISSING_PAGES; command_receipt(&receipt); request_snapshot(now); return false;
        }
        bool self=false;
        /* The roster is frozen by its hash/count, so a later authenticated
         * snapshot may provide the same membership to a badge that missed
         * PREPARE. Its roles and phase follow through normal snapshot repair. */
        for (unsigned i=0;i<assembly.roster_count;++i) if (mac_equal(&assembly.roster[i].mac,&view.self_mac) && assembly.roster[i].slot==view.self_slot) self=true;
        if (!self) break;
        zt_err_t locked=zt_channel_lock(msg->round_id,a.channel);
        if (locked!=ZT_OK) {
            if (locked!=ZT_ERR_BUSY) error_overlay(locked);
            return false;
        }
        zt_channel_status_t channel;
        if (zt_channel_get_status(&channel)!=ZT_OK) return false;
        if (channel.state!=ZT_CHANNEL_LOCKED || channel.channel!=a.channel) {
            /* Queue acceptance is not a completed hardware channel change.
             * Persist PREPARE and report READY only after the owner confirms. */
            return false;
        }
        memset(&staged,0,sizeof(staged));
        if (!originate || current.round_id==msg->round_id)
            memcpy(staged.applied_command_seq,current.applied_command_seq,sizeof(staged.applied_command_seq));
        staged.round_id=msg->round_id; staged.snapshot_rev=a.snapshot_rev; staged.roster_hash=a.roster_hash;
        staged.roster_count=a.roster_count; staged.rules=a; staged.phase=ZT_PHASE_PREPARED; staged.patient_zero_slot=ZT_SLOT_INVALID; staged.round_channel=a.channel;
        for (unsigned i=0;i<a.roster_count;++i) {
            staged.roster[i]=assembly.roster[i]; staged.roles[i]=(zt_wire_role_entry_t){.slot=staged.roster[i].slot,.role=ZT_ROLE_HUMAN,.cause_slot=ZT_SLOT_INVALID};
        }
        if (current.round_id!=msg->round_id) zt_game_clock_reset();
        rejoining=false; receipt.state=ZT_RECEIPT_PREPARED_READY; receipt.detail=ZT_DETAIL_NONE; break;
    }
    case ZT_CMD_START_ROUND: {
        zt_wire_start_round_args_t a;
        if (zt_wire_decode_start_round_args(cmd->args,cmd->args_len,&a)!=ZT_OK || a.duration_ms!=ZT_ROUND_DURATION_MS ||
            a.roster_hash!=current.roster_hash || roster_index(a.patient_zero_slot)<0 || !a.initial_role_rev) break;
        if (current.phase>=ZT_PHASE_RUNNING) {
            if (a.patient_zero_slot!=current.patient_zero_slot) break;
            /* A later canonical snapshot can include infections before an old
             * START receipt arrives. A matching START is idempotent and must
             * never overwrite those newer roles or move the snapshot backward. */
            if (rejoining && current.phase==ZT_PHASE_RUNNING &&
                apply_clock(a.sampled_elapsed_ms,a.uncertainty_ms,slot->rx_us,slot->age_ms,slot->hops)) {
                rejoining=false; refresh_role();
            }
            receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE;
            command_receipt(&receipt); if (originate) host_arm_command(ci,now); return true;
        }
        if (a.snapshot_rev<current.rules.snapshot_rev) break;
        if (current.phase!=ZT_PHASE_PREPARED || !apply_clock(a.sampled_elapsed_ms,a.uncertainty_ms,slot->rx_us,slot->age_ms,slot->hops)) {
            request_snapshot(now); return true;
        }
        staged.patient_zero_slot=a.patient_zero_slot;
        if (a.snapshot_rev>staged.snapshot_rev) staged.snapshot_rev=a.snapshot_rev;
        staged.phase=ZT_PHASE_RUNNING;
        for (unsigned i=0;i<staged.roster_count;++i) staged.roles[i]=(zt_wire_role_entry_t){.slot=staged.roster[i].slot,
            .role=staged.roster[i].slot==a.patient_zero_slot ? ZT_ROLE_ZOMBIE : ZT_ROLE_HUMAN,.role_rev=a.initial_role_rev,
            .cause_slot=staged.roster[i].slot==a.patient_zero_slot ? a.patient_zero_slot : ZT_SLOT_INVALID,.cause_seq=0};
        rejoining=false; receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE; break;
    }
    case ZT_CMD_ROLE_SET: {
        zt_wire_role_set_args_t a;
        if (zt_wire_decode_role_set_args(cmd->args,cmd->args_len,&a)!=ZT_OK || cmd->target_slot==ZT_SLOT_ALL) break;
        zt_wire_role_entry_t role={.slot=cmd->target_slot,.role=a.role,.role_rev=a.role_rev,.cause_slot=a.cause_slot,.cause_seq=a.cause_seq,.covered_seq=a.covered_seq};
        zt_wire_role_entry_t *old=self_role();
        if (!old || a.role_rev<old->role_rev) { receipt.detail=ZT_DETAIL_STALE_REVISION; break; }
        merge_role(&staged,&role);
        if (a.covered_seq<current.produced[view.self_slot]) {
            /* Persist the held update, but never claim its role was applied. */
            pending.receipt=(zt_wire_command_receipt_t){0};
            if (current.pending_roles[roster_index(view.self_slot)].role_rev==a.role_rev) { request_snapshot(now); return true; }
            return save_checkpoint(now)==ZT_OK;
        }
        receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE; break;
    }
    case ZT_CMD_END_ROUND: {
        zt_wire_end_round_args_t a;
        if (zt_wire_decode_end_round_args(cmd->args,cmd->args_len,&a)!=ZT_OK || current.phase<ZT_PHASE_RUNNING) break;
        if (current.phase==ZT_PHASE_FINAL || cmd->command_seq<result_command_seq) {
            receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE;
            command_receipt(&receipt); return true;
        }
        staged.phase=ZT_PHASE_EXPIRED_PENDING_SYNC; pending.end_present=1; pending.end_winner=a.winner;
        pending.end_elapsed_ms=a.effective_elapsed_ms;
        pending.result_command_seq=cmd->command_seq; pending.result_authoritative=1;
        receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE; break;
    }
    case ZT_CMD_FINAL_RESULT: {
        zt_wire_final_result_args_t a;
        if (zt_wire_decode_final_result_args(cmd->args,cmd->args_len,&a)!=ZT_OK || a.state_rev<current.state_rev || current.phase<ZT_PHASE_RUNNING) break;
        if (!close_committed) {
            /* Keep the command until local evidence closure is durable. A
             * reordered FINAL must neither be rejected nor strand RUNNING. */
            current.phase=ZT_PHASE_EXPIRED_PENDING_SYNC;
            display_end(a.winner,view.result_present ? round_end_elapsed_ms : ZT_ROUND_DURATION_MS-view.remaining_ms);
            result_authoritative=true; result_command_seq=cmd->command_seq;
            refresh_role(); checkpoint_dirty=true;
            return false;
        }
        staged.phase=ZT_PHASE_FINAL; staged.state_rev=a.state_rev;
        pending.final_present=1; pending.final_result=a;
        pending.end_present=1; pending.end_winner=a.winner; pending.end_elapsed_ms=round_end_elapsed_ms;
        pending.result_command_seq=cmd->command_seq; pending.result_authoritative=1;
        receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE; break;
    }
    case ZT_CMD_CANCEL_PREPARE: {
        zt_wire_cancel_prepare_args_t a;
        if (zt_wire_decode_cancel_prepare_args(cmd->args,cmd->args_len,&a)!=ZT_OK || current.phase!=ZT_PHASE_PREPARED) break;
        staged.phase=ZT_PHASE_FINAL; /* Retain the canceled round until clearance. */
        receipt.state=ZT_RECEIPT_APPLIED; receipt.detail=ZT_DETAIL_NONE; break;
    }
    default: break;
    }
    if (receipt.detail!=ZT_DETAIL_NONE) { command_receipt(&receipt); return true; }
    unsigned replace=0;
    for (unsigned i=0;i<8;++i) if (staged.applied_command_seq[i]<staged.applied_command_seq[replace]) replace=i;
    if (staged.applied_command_seq[replace]>cmd->command_seq) { receipt.detail=ZT_DETAIL_STALE_REVISION; receipt.state=ZT_RECEIPT_REJECTED; command_receipt(&receipt); return true; }
    staged.applied_command_seq[replace]=cmd->command_seq;
    /* Outbox IDs identify complete server messages, not individual entries.
     * Gateway owns advancing its cursor after all entries have completed. */
    receipt.applied_snapshot_rev=staged.snapshot_rev; pending.receipt=receipt;
    if (save_checkpoint(now)!=ZT_OK) return false;
    if (originate) {
        pending.host_command=ci+1;
        portENTER_CRITICAL(&ingress_guard); slot->occupied=COMMAND_PERSISTING; portEXIT_CRITICAL(&ingress_guard);
    }
    return true;
}
static bool decision(const zt_server_decision_t *d,uint64_t now)
{
    if (pending.id) return false;
    zt_persist_request_t r={.kind=ZT_PERSIST_DECISIONS,.round_id=d->round_id};
    r.value.decisions.count=1; r.value.decisions.snapshot_rev=current.snapshot_rev; r.value.decisions.entries[0]=d->decision;
    if (submit(&r,APPLY_DECISION,now)!=ZT_OK) return false;
    pending.decision=*d; return true;
}
static bool stage_assembled_result(void)
{
    pending.final_present=0; pending.end_present=0;
    pending.result_command_seq=0; pending.result_authoritative=assembly.source==ASSEMBLY_SERVER;
    if (assembly.phase<ZT_PHASE_EXPIRED_PENDING_SYNC) return false;
    bool waiting_for_close=assembly.phase==ZT_PHASE_FINAL &&
        (current.round_id!=assembly.round || !close_committed);
    bool already_final=current.round_id==assembly.round && current.phase==ZT_PHASE_FINAL;
    /* A snapshot can stop play immediately; final persistence follows this
     * badge's durable close so the close write cannot downgrade FINAL. */
    if (staged.phase==ZT_PHASE_FINAL && (waiting_for_close || !assembly.result_final) && !already_final)
        staged.phase=ZT_PHASE_EXPIRED_PENDING_SYNC;
    if (assembly.result_present && (!already_final || assembly.result_final)) {
        pending.end_present=1; pending.end_winner=assembly.winner; pending.end_elapsed_ms=assembly.effective_elapsed_ms;
        if (assembly.result_final && !waiting_for_close) {
            staged.phase=ZT_PHASE_FINAL;
            pending.final_present=1;
            pending.final_result=(zt_wire_final_result_args_t){assembly.winner,assembly.result_complete,
                assembly.missing_slots_bitmap,assembly.state_rev};
        }
    }
    return waiting_for_close && assembly.result_final;
}
static bool apply_assembled(uint64_t now)
{
    if (!join_nonce || !view.registered) return true;
    if (assembly.invalid || !assembly.complete || !assembly.role_pages || assembly.role_mask!=(1u<<assembly.role_pages)-1 ||
        assembly.role_slots!=assembly.roster_slots || assembly.role_count!=assembly.roster_count) return true;
    if (pending.id) return false;
    if (!current.round_id || assembly.round!=current.round_id || !view.registered) {
        /* Only server pages carry the complete rules needed for admission.
         * Mesh role pages repair an existing checkpoint; they cannot supply
         * missing rules or turn a relayed roster into a new registration. */
        if (assembly.source!=ASSEMBLY_SERVER || !assembly.roster_pages || assembly.roster_mask!=(1u<<assembly.roster_pages)-1 ||
            assembly.rules.roster_count!=assembly.roster_count) return true;
        int self=-1;
        for (unsigned i=0;i<assembly.roster_count;++i) if (mac_equal(&assembly.roster[i].mac,&view.self_mac)) self=i;
        if (self<0) { view.admission=ZT_ADMISSION_NEXT_ROUND; return true; }
        if (current.round_id && assembly.round!=current.round_id && zt_store_round_retained(current.round_id) &&
            (current.phase!=ZT_PHASE_FINAL || own_events_pending())) return true;
        /* A JOIN-assigned slot or an existing frozen roster is not movable. */
        if (view.registered && !current.round_id && view.self_slot!=assembly.roster[self].slot) { error_overlay(ZT_ERR_AUTH); return true; }
        if (current.round_id==assembly.round && (current.roster_hash!=assembly.hash || current.roster_count!=assembly.roster_count ||
            memcmp(current.roster,assembly.roster,current.roster_count*sizeof(current.roster[0])))) { error_overlay(ZT_ERR_AUTH); return true; }
        if (assembly.phase>=ZT_PHASE_RUNNING && (assembly.patient_zero>=20 || !(assembly.roster_slots&(1u<<assembly.patient_zero)))) return true;
        if (current.round_id==assembly.round) staged=current;
        else memset(&staged,0,sizeof(staged));
        staged.round_id=assembly.round; staged.snapshot_rev=assembly.rev; staged.state_rev=assembly.state_rev;
        staged.roster_hash=assembly.hash; staged.roster_count=assembly.roster_count;
        staged.phase=assembly.phase; staged.round_channel=assembly.channel; staged.patient_zero_slot=assembly.patient_zero;
        staged.rules=assembly.rules;
        for (unsigned i=0;i<assembly.roster_count;++i) {
            staged.roster[i]=assembly.roster[i];
            for (unsigned j=0;j<assembly.role_count;++j) if (assembly.roles[j].slot==staged.roster[i].slot) staged.roles[i]=assembly.roles[j];
        }
        pending.receipt.command_seq=0;
        bool waiting_for_close=stage_assembled_result();
        if (save_checkpoint(now)!=ZT_OK) return false;
        pending.admission=1;
        return !waiting_for_close;
    }
    if (current.roster_hash!=assembly.hash || current.roster_count!=assembly.roster_count ||
        memcmp(current.roster,assembly.roster,current.roster_count*sizeof(current.roster[0]))) { error_overlay(ZT_ERR_AUTH); return true; }
    staged=current;
    for (unsigned i=0;i<assembly.role_count;++i) merge_role(&staged,&assembly.roles[i]);
    if (assembly.rev>staged.snapshot_rev) staged.snapshot_rev=assembly.rev;
    if (assembly.state_rev>staged.state_rev) staged.state_rev=assembly.state_rev;
    if (assembly.phase>staged.phase && assembly.phase<=ZT_PHASE_FINAL) {
        if (assembly.phase==ZT_PHASE_RUNNING && roster_index(assembly.patient_zero)<0) return true;
        staged.phase=assembly.phase; staged.patient_zero_slot=assembly.patient_zero;
    }
    bool waiting_for_close=stage_assembled_result();
    bool result_changed=pending.end_present && (!view.result_present || view.winner!=pending.end_winner ||
        round_end_elapsed_ms!=pending.end_elapsed_ms || (pending.final_present &&
        (!view.result_final || view.result_complete!=pending.final_result.complete || view.missing_slots_bitmap!=pending.final_result.missing_slots_bitmap)));
    if (!memcmp(&staged,&current,sizeof(current)) && !result_changed) {
        pending.end_present=pending.final_present=0;
        return !waiting_for_close;
    }
    pending.receipt.command_seq=0;
    if (save_checkpoint(now)!=ZT_OK) return false;
    return !waiting_for_close;
}
static bool process_page(unsigned pi,uint64_t now)
{
    page_slot_t *slot=&pages[pi];
    if (!slot->occupied) return true;
    if ((slot->server ? slot->value.snapshot.round_id : slot->value.radio.header.round_id)==reset_record.round_id) return true;
    if ((!join_nonce || !view.registered) && (slot->server || slot->value.radio.header.type!=ZT_PKT_HOST_STATE)) return true;
    if (slot->server) {
        const zt_server_snapshot_t *s=&slot->value.snapshot;
        if (pending.id) return false;
        uint8_t rules[ZT_PREPARE_ROUND_ARGS_BYTES], prior_rules[ZT_PREPARE_ROUND_ARGS_BYTES]; size_t n;
        if (zt_wire_encode_prepare_round_args(&s->rules,rules,sizeof(rules),&n)!=ZT_OK ||
            s->phase>ZT_PHASE_FINAL || s->rules.roster_hash!=s->roster_hash || s->rules.channel!=s->round_channel ||
            s->rules.duration_ms!=ZT_ROUND_DURATION_MS || s->rules.tag_cooldown_ms!=ZT_TAG_COOLDOWN_MS ||
            (s->result_present && (s->phase<ZT_PHASE_EXPIRED_PENDING_SYNC ||
                s->winner>ZT_ROLE_ZOMBIE || s->effective_elapsed_ms>ZT_ROUND_DURATION_MS)) ||
            (s->result_final && (!s->result_present || s->phase!=ZT_PHASE_FINAL)) ||
            !assembly_begin(s->round_id,s->snapshot_id,s->roster_hash,ASSEMBLY_SERVER)) return true;
        if (assembly.role_pages) {
            if (zt_wire_encode_prepare_round_args(&assembly.rules,prior_rules,sizeof(prior_rules),&n)!=ZT_OK ||
                memcmp(rules,prior_rules,sizeof(rules)) || assembly.state_rev!=s->state_rev || assembly.phase!=s->phase ||
                assembly.patient_zero!=s->patient_zero_slot || assembly.channel!=s->round_channel ||
                assembly.start_time_ms!=s->start_time_ms || assembly.end_time_ms!=s->end_time_ms ||
                assembly.result_present!=s->result_present || assembly.result_final!=s->result_final ||
                assembly.result_complete!=s->result_complete || assembly.winner!=s->winner ||
                assembly.missing_slots_bitmap!=s->missing_slots_bitmap || assembly.effective_elapsed_ms!=s->effective_elapsed_ms) {
                assembly.invalid=1; return true;
            }
        }
        zt_wire_roster_page_t p={.snapshot_rev=s->snapshot_id,.roster_hash=s->roster_hash,.page_index=s->page_index,
            .page_count=s->page_count,.entry_count=s->entry_count};
        memcpy(p.entries,s->roster,sizeof(p.entries));
        if (!roster_page(&p,s->round_id,ASSEMBLY_SERVER)) { request_snapshot(now); return true; }
        if (assembly.role_mask&(1u<<s->page_index)) {
            for (unsigned i=0;i<s->entry_count;++i) {
                uint8_t role[ZT_ROLE_ENTRY_BYTES], prior_role[ZT_ROLE_ENTRY_BYTES];
                if (zt_wire_encode_role_entry(&s->roles[i],role,sizeof(role),&n)!=ZT_OK ||
                    zt_wire_encode_role_entry(&assembly.roles[s->page_index*8+i],prior_role,sizeof(prior_role),&n)!=ZT_OK ||
                    memcmp(role,prior_role,sizeof(role))) { assembly.invalid=1; return true; }
            }
            return apply_assembled(now);
        }
        if ((assembly.role_pages && assembly.role_pages!=s->page_count) || s->page_index*8+s->entry_count>20) return true;
        for (unsigned i=0;i<s->entry_count;++i) {
            if (s->roles[i].slot!=s->roster[i].slot || s->roles[i].slot>=20 || (assembly.role_slots&(1u<<s->roles[i].slot))) return true;
            uint8_t bytes[10]; size_t n;
            if (zt_wire_encode_role_entry(&s->roles[i],bytes,sizeof(bytes),&n)!=ZT_OK) return true;
        }
        for (unsigned i=0;i<s->entry_count;++i) { assembly.roles[s->page_index*8+i]=s->roles[i]; assembly.role_slots|=1u<<s->roles[i].slot; }
        assembly.role_mask|=1u<<s->page_index; assembly.role_count+=s->entry_count; assembly.role_pages=s->page_count;
        assembly.rules=s->rules; assembly.phase=s->phase; assembly.patient_zero=s->patient_zero_slot; assembly.channel=s->round_channel;
        assembly.state_rev=s->state_rev; assembly.server_id=s->server_id;
        assembly.start_time_ms=s->start_time_ms; assembly.end_time_ms=s->end_time_ms;
        assembly.result_present=s->result_present; assembly.result_final=s->result_final; assembly.result_complete=s->result_complete;
        assembly.winner=s->winner; assembly.missing_slots_bitmap=s->missing_slots_bitmap; assembly.effective_elapsed_ms=s->effective_elapsed_ms;
        return apply_assembled(now);
    }
    const zt_domain_message_t *m=&slot->value.radio;
    if (m->driver_generation!=driver_generation || m->channel!=view.channel) return true;
    switch (m->header.type) {
    case ZT_PKT_ROSTER_PAGE:
        if (!roster_page(&m->payload.roster_page,m->header.round_id,ASSEMBLY_MESH)) request_snapshot(now);
        return true;
    case ZT_PKT_HOST_STATE: {
        const zt_wire_host_state_t *h=&m->payload.host_state;
        server_status_observe(m->header.origin_boot_nonce,m->header.packet_seq,m->rx_us,m->header.age_ms,
            !!(h->gateway_flags&ZT_HOST_STATE_FLAG_SERVER_CONNECTED),now);
        /* Host reachability is independent of whether this round has a usable
         * clock yet. Keep the original receive/relay age, not processing time. */
        if (m->rx_us<=now && m->header.age_ms<=ZT_CLOCK_AGE_MAX_MS &&
            m->rx_us>=(uint64_t)m->header.age_ms*1000ULL) {
            uint64_t seen=m->rx_us-(uint64_t)m->header.age_ms*1000ULL;
            if (seen>host_seen_us) host_seen_us=seen;
        }
        if (!join_nonce || !view.registered) return true;
        if (m->header.round_id!=current.round_id || h->roster_hash!=current.roster_hash || h->duration_ms!=ZT_ROUND_DURATION_MS ||
            h->round_channel!=current.round_channel || h->snapshot_rev<current.snapshot_rev || h->entry_count>12 || h->page_count>2) return true;
        if (h->snapshot_rev>current.snapshot_rev || h->phase>current.phase) request_snapshot(now);
        if (h->phase>=ZT_PHASE_EXPIRED_PENDING_SYNC && h->winner<=ZT_ROLE_ZOMBIE &&
            !result_authoritative && h->remaining_ms<=ZT_ROUND_DURATION_MS && current.phase>=ZT_PHASE_RUNNING && current.phase<ZT_PHASE_FINAL &&
            (current.phase<ZT_PHASE_EXPIRED_PENDING_SYNC || !view.result_present || view.winner!=h->winner || view.remaining_ms!=h->remaining_ms)) {
            if (pending.id) return false;
            staged=current; staged.phase=ZT_PHASE_EXPIRED_PENDING_SYNC;
            pending.receipt.command_seq=0; pending.final_present=0; pending.end_present=1;
            pending.result_command_seq=0; pending.result_authoritative=0;
            pending.end_winner=h->winner; pending.end_elapsed_ms=ZT_ROUND_DURATION_MS-h->remaining_ms;
            if (save_checkpoint(now)!=ZT_OK) return false;
            /* Retain this page until its stop is durable, then process roles.
             * HOST_STATE does not claim FINAL completeness for this badge. */
            return false;
        }
        if (h->host_elapsed_ms!=INT32_MIN && (gateway_boot!=m->header.origin_boot_nonce || h->gateway_serial>gateway_serial)) {
            if (apply_clock(h->host_elapsed_ms,h->uncertainty_ms,m->rx_us,m->header.age_ms,m->header.hops)) {
                gateway_boot=m->header.origin_boot_nonce; gateway_serial=h->gateway_serial; gateway_seen_us=now; gateway_hops=m->header.hops;
                if (current.phase==ZT_PHASE_RUNNING && h->phase>=ZT_PHASE_RUNNING) { rejoining=false; refresh_role(); }
            }
        }
        if (!h->page_count) return true;
        if (h->page_index>=h->page_count || !h->entry_count || (h->page_index+1<h->page_count && h->entry_count!=12) ||
            h->page_index*12+h->entry_count>20) return true;
        if (pending.id) return false;
        if (!assembly_begin(m->header.round_id,h->snapshot_rev,h->roster_hash,ASSEMBLY_MESH)) return true;
        /* An unchanged frozen roster is already a verified complete assembly. */
        if (!assembly.complete) {
            memcpy(assembly.roster,current.roster,sizeof(current.roster)); assembly.roster_count=current.roster_count; assembly.complete=1;
            for (unsigned i=0;i<current.roster_count;++i) assembly.roster_slots|=1u<<current.roster[i].slot;
        }
        if (assembly.role_mask&(1u<<h->page_index)) return apply_assembled(now);
        if (assembly.role_pages && assembly.role_pages!=h->page_count) return true;
        for (unsigned i=0;i<h->entry_count;++i) {
            if (h->entries[i].slot>=20 || (assembly.role_slots&(1u<<h->entries[i].slot)) || roster_index(h->entries[i].slot)<0) return true;
        }
        for (unsigned i=0;i<h->entry_count;++i) { assembly.roles[h->page_index*12+i]=h->entries[i]; assembly.role_slots|=1u<<h->entries[i].slot; }
        assembly.role_count+=h->entry_count; assembly.role_pages=h->page_count; assembly.role_mask|=1u<<h->page_index;
        assembly.phase=h->phase; assembly.patient_zero=h->patient_zero_slot;
        return apply_assembled(now);
    }
    case ZT_PKT_EVENT_DECISIONS: {
        const zt_wire_event_decisions_t *d=&m->payload.event_decisions;
        if (d->count>20 || (m->header.round_id!=current.round_id && m->header.round_id!=zt_store_other_round(current.round_id))) return true;
        /* Each page remains owned until every entry has been copied to the
         * persistence request. Repeated entries are idempotent in storage. */
        if (slot->occupied>=2+d->count) return true;
        unsigned i=slot->occupied-2;
        if (i>=d->count) return true;
        zt_server_decision_t one={.round_id=m->header.round_id,.decision=d->entries[i]};
        if (decision(&one,now)) ++slot->occupied;
        return false;
    }
    case ZT_PKT_WATERMARKS: {
        const zt_wire_watermarks_t *w=&m->payload.watermarks;
        if (w->count>20 || m->header.round_id!=current.round_id || w->roster_hash!=current.roster_hash) return true;
        if (pending.id) return false;
        staged=current;
        for (unsigned i=0;i<w->count;++i) {
            unsigned s=w->entries[i].slot; if (s>=20 || roster_index(s)<0) return true;
            if (w->entries[i].server_received_contiguous>staged.received[s]) staged.received[s]=w->entries[i].server_received_contiguous;
            if (w->entries[i].server_final_contiguous>current.decided[s]) request_snapshot(now);
        }
        if (!memcmp(&staged,&current,sizeof(current))) return true;
        pending.receipt.command_seq=0; return save_checkpoint(now)==ZT_OK;
    }
    case ZT_PKT_WANT_EVENTS: {
        const zt_wire_want_events_t *w=&m->payload.want_events;
        if (w->target_slot!=view.self_slot || w->count>16 || origin_index(&m->direct_source)<0) return true;
        unsigned i=slot->occupied-2;
        if (i>=w->count) return true;
        zt_wire_event_t event;
        if (zt_store_read_event(m->header.round_id,w->entries[i].origin_slot,w->entries[i].event_seq,&event)==ZT_OK) {
            zt_wire_payload_t p={0}; p.event_copy.target_slot=current.roster[origin_index(&m->direct_source)].slot; p.event_copy.event=event;
            if (send_payload(m->header.round_id,ZT_PKT_EVENT_COPY,&p,ZT_TX_PRIO_REPLAY)!=ZT_OK) return false;
        }
        ++slot->occupied; return false;
    }
    case ZT_PKT_CACHE_PAGE: {
        const zt_wire_cache_page_t *c=&m->payload.cache_page;
        if (c->count>56 || c->page_count>3 || c->page_index>=c->page_count ||
            (c->target_slot!=ZT_SLOT_ALL && c->target_slot!=view.self_slot) || origin_index(&m->direct_source)<0) return true;
        zt_wire_payload_t p={0}; p.want_events.target_slot=current.roster[origin_index(&m->direct_source)].slot; p.want_events.request_seq=1;
        for (unsigned i=0;i<c->count && p.want_events.count<16;++i) {
            zt_wire_event_t event;
            if (zt_store_read_event(m->header.round_id,c->entries[i].origin_slot,c->entries[i].event_seq,&event)==ZT_ERR_NOT_FOUND)
                p.want_events.entries[p.want_events.count++]=c->entries[i];
        }
        if (p.want_events.count) send_payload(m->header.round_id,ZT_PKT_WANT_EVENTS,&p,ZT_TX_PRIO_CLOCK_REPAIR);
        return true;
    }
    default: return true;
    }
}
static void confirm(const zt_wire_event_t *e,uint64_t now)
{
    if (!outbound.confirmed && e->actor_slot==view.self_slot && outbound.request.attempt_seq && e->request_boot==outbound.boot &&
        e->request_seq==outbound.request.attempt_seq && e->victim_slot==outbound.request.victim_slot &&
        e->actor_cause_seq==outbound.request.actor_cause_seq && e->actor_role_rev==outbound.request.actor_role_rev) {
        /* A late first proof can confirm a timed-out attempt. Journal replay,
         * repeated direct results and repair copies cannot restart its effect. */
        outbound.active=false; outbound.confirmed=true; feedback(ZT_FEEDBACK_TAG_CONFIRMED,now);
    }
}
static bool incoming_event(zt_round_id_t round,const zt_wire_event_t *e,uint64_t now)
{
    if (round!=current.round_id && round!=zt_store_other_round(current.round_id)) return true;
    if (round==current.round_id && (roster_index(e->victim_slot)<0 || roster_index(e->actor_slot)<0)) return true;
    if (round==previous_round.round && (!(previous_round.slots&(1u<<e->victim_slot)) || !(previous_round.slots&(1u<<e->actor_slot)))) return true;
    if (round==current.round_id) confirm(e,now);
    zt_wire_event_t old;
    if (zt_store_read_event(round,e->victim_slot,e->event_seq,&old)==ZT_OK) return true;
    if ((round==current.round_id && e->victim_slot==view.self_slot) ||
        (round==previous_round.round && e->victim_slot==previous_round.self)) return true; /* Only local validation creates own events. */
    if (pending.id) return false;
    zt_persist_request_t r={.kind=ZT_PERSIST_EVENT,.round_id=round,.value.event=*e};
    if (submit(&r,6,now)!=ZT_OK) return false;
    pending.event=*e; return true;
}
static bool process_input(const input_t *in,uint64_t now)
{
    if (in->type==IN_CONTROL_RESULT) {
        control_result_t result; memcpy(&result,in->bytes,sizeof(result));
        if (!view.host_control_pending || result.action!=host_control_request.item.body.control.action ||
            result.request_seq!=host_control_request.item.body.control.request_seq) return true;
        if (result.result!=ZT_OK) {
            view.host_control_error=result.result; view.host_control_pending=0; view.last_error=result.result;
        }
        /* Acceptance is not a local phase transition. Keep the busy label
         * until authenticated snapshots/commands apply START or RESET. */
        return true;
    }
    if (in->type==IN_CLOCK) {
        zt_clock_sample_t c; memcpy(&c,in->bytes,sizeof(c));
        if (join_nonce && view.registered && current.round_id && in->round==current.round_id &&
            zt_clock_apply(in->round,&c)==ZT_OK && current.phase==ZT_PHASE_RUNNING) { rejoining=false; refresh_role(); }
        return true;
    }
    if (in->type==IN_INVALIDATE) {
        if (in->generation<driver_generation) return true;
        driver_generation=in->generation; zt_peers_invalidate(in->generation); host_seen_us=0; server_report.seen_us=0;
        return true;
    }
    if (in->type==IN_PERSIST) { zt_persist_completion_t c; memcpy(&c,in->bytes,sizeof(c)); completed(&c,now); return true; }
    if (in->type==IN_DECISION) { zt_server_decision_t d; memcpy(&d,in->bytes,sizeof(d)); return decision(&d,now); }
    if (in->type==IN_RECEIPT) {
        zt_server_receipt_t r; memcpy(&r,in->bytes,sizeof(r));
        if (r.round_id!=current.round_id || r.event_id.victim_slot>=20) return true;
        unsigned s=r.event_id.victim_slot;
        if (r.event_id.event_seq!=current.received[s]+1u) return true; /* Safe duplicate replay for holes. */
        if (pending.id) return false;
        staged=current; staged.received[s]=r.event_id.event_seq; pending.receipt.command_seq=0;
        return save_checkpoint(now)==ZT_OK;
    }
    if (in->type==IN_COMMAND) { commands[in->bytes[0]].occupied=2; return true; }
    if (in->type==IN_PAGE) { pages[in->bytes[0]].occupied=2; return true; }
    if (in->type==IN_REGISTRATION) return registration_result(in,now);
    if (in->generation!=driver_generation) return true;
    if (is_host() && in->type==ZT_PKT_COMMAND_RECEIPT && in->round==reset_delivery.round) {
        zt_wire_command_receipt_t receipt;
        if (zt_wire_decode_command_receipt(in->bytes,in->len,&receipt)==ZT_OK && receipt.slot<ZT_MAX_PLAYERS &&
            (reset_delivery.slots&(1u<<receipt.slot)) && reset_delivery.seq[receipt.slot]==receipt.command_seq &&
            mac_equal(&reset_delivery.macs[receipt.slot],&in->origin) && receipt.state==ZT_RECEIPT_APPLIED) {
            zt_game_feed_item_t item={.kind=ZT_GAME_FEED_COMMAND_RECEIPT,.round_id=in->round,.body.command_receipt=receipt};
            feed(&item);
        }
        return true;
    }
    if (in->round && in->round==reset_record.round_id && in->type!=ZT_PKT_BEACON) return true;
    if (in->type==ZT_PKT_JOIN) { host_join(in,now); return true; }
    if (in->type==ZT_PKT_SNAPSHOT_REQUEST) { host_snapshot_request(in,now); return true; }
    if (in->type==ZT_PKT_JOIN_RESULT) {
        zt_wire_join_result_t j;
        if (zt_wire_decode_join_result(in->bytes,in->len,&j)==ZT_OK && mac_equal(&in->origin,&config.host_mac)) apply_join_result(&j,now);
        return true;
    }
    if (in->type==ZT_PKT_BEACON) {
        /* Queued beacons from the previous lobby dwell cannot repopulate
         * contact history after a channel invalidation with the same driver. */
        if (in->channel!=view.channel) return true;
        zt_domain_message_t m={0};
        if (zt_wire_decode_beacon(in->bytes,in->len,&m.payload.beacon)!=ZT_OK) return true;
        m.header=(zt_wire_header_t){.type=ZT_PKT_BEACON,.game_id=config.game_id,.round_id=in->round,.origin=in->origin,.origin_boot_nonce=in->boot};
        memcpy(&m.header.packet_seq,in->bytes+50,4); m.direct_source=in->origin; m.rssi=in->rssi; m.channel=in->channel;
        m.rx_us=in->rx_us; m.driver_generation=in->generation;
        bool host_returned=mac_equal(&in->origin,&config.host_mac) &&
            (!host_seen_us || (now>=host_seen_us && now-host_seen_us>=ZT_PEER_STALE_MS*1000ULL));
        if (mac_equal(&in->origin,&config.host_mac)) {
            uint16_t age; memcpy(&age,in->bytes+54,2);
            server_status_observe(in->boot,m.header.packet_seq,in->rx_us,age,
                !!(m.payload.beacon.flags&ZT_BEACON_FLAG_SERVER_CONNECTED),now);
        }
        zt_err_t e=zt_peers_observe(&m); if (e!=ZT_OK && e!=ZT_ERR_STALE) view.last_error=e;
        if (e==ZT_OK && !is_host() && in->round==current.round_id && current.round_id &&
            (host_returned || m.payload.beacon.snapshot_rev>current.snapshot_rev || m.payload.beacon.phase>current.phase)) {
            request_snapshot(now);
            replay_due=0;
        }
        if (e==ZT_OK && mac_equal(&in->origin,&config.host_mac) && in->rx_us<=now && in->rx_us>host_seen_us)
            host_seen_us=in->rx_us;
        return true;
    }
    if (in->round!=current.round_id && in->round!=zt_store_other_round(current.round_id)) return true;
    if (in->type==ZT_PKT_COMMAND_RECEIPT || in->type==ZT_PKT_ROUND_CLOSED || in->type==ZT_PKT_DECISION_RECEIPT) {
        if (!view.host_selected || !view.host_configured) return true;
        zt_game_feed_item_t item={.round_id=in->round}; unsigned claimed;
        if (in->type==ZT_PKT_COMMAND_RECEIPT) {
            item.kind=ZT_GAME_FEED_COMMAND_RECEIPT;
            if (zt_wire_decode_command_receipt(in->bytes,in->len,&item.body.command_receipt)!=ZT_OK) return true;
            claimed=item.body.command_receipt.slot;
        } else if (in->type==ZT_PKT_ROUND_CLOSED) {
            item.kind=ZT_GAME_FEED_ROUND_CLOSED;
            if (zt_wire_decode_round_closed(in->bytes,in->len,&item.body.round_closed)!=ZT_OK) return true;
            claimed=item.body.round_closed.slot;
        } else {
            item.kind=ZT_GAME_FEED_DECISION_RECEIPT;
            if (zt_wire_decode_decision_receipt(in->bytes,in->len,&item.body.decision_receipt)!=ZT_OK) return true;
            claimed=item.body.decision_receipt.slot;
        }
        if (!event_origin(in->round,claimed,&in->origin)) return true;
        if (item.kind==ZT_GAME_FEED_DECISION_RECEIPT) {
            uint16_t *received=in->round==current.round_id ? decision_received : previous_decision_received;
            uint16_t through=item.body.decision_receipt.own_decided_contiguous;
            if (through>received[claimed]) received[claimed]=through;
        }
        bool prepared=false;
        if (item.kind==ZT_GAME_FEED_COMMAND_RECEIPT && is_host()) prepared=host_receipt(in->round,&item.body.command_receipt);
        if (in->round==current.round_id && item.kind==ZT_GAME_FEED_COMMAND_RECEIPT &&
            prepared && item.body.command_receipt.applied_snapshot_rev==current.snapshot_rev) {
            current.roles[roster_index(claimed)].flags|=ZT_ROLE_FLAG_READY;
            view.ready_count=0;
            for (unsigned i=0;i<current.roster_count;++i) if (current.roles[i].flags&ZT_ROLE_FLAG_READY) ++view.ready_count;
        }
        feed(&item); return true;
    }
    if (in->type==ZT_PKT_TAG_REQUEST) { accept_request(in,now); return true; }
    if (in->type==ZT_PKT_TAG_RESULT) {
        if (in->round!=current.round_id) return true;
        zt_wire_tag_result_t r;
        if (zt_wire_decode_tag_result(in->bytes,in->len,&r)!=ZT_OK || !outbound.request.attempt_seq || r.actor_slot!=view.self_slot ||
            r.request_boot!=outbound.boot || r.request_seq!=outbound.request.attempt_seq) return true;
        int i=roster_index(outbound.request.victim_slot);
        if (i<0 || !mac_equal(&in->origin,&current.roster[i].mac)) return true;
        outbound.responded=true;
        if (r.result==ZT_TAG_ACCEPTED) confirm(&r.accepted_event,now);
        else if (r.result!=ZT_TAG_PENDING && outbound.active) { outbound.active=false; feedback(ZT_FEEDBACK_UNCONFIRMED,now); }
        return true;
    }
    if (in->type==ZT_PKT_EVENT) {
        zt_wire_event_t e;
        if (zt_wire_decode_event(in->bytes,in->len,&e)!=ZT_OK) return true;
        if (!event_origin(in->round,e.victim_slot,&in->origin)) return true;
        return incoming_event(in->round,&e,now);
    }
    if (in->type==ZT_PKT_EVENT_COPY) {
        zt_wire_event_copy_t e;
        if (zt_wire_decode_event_copy(in->bytes,in->len,&e)!=ZT_OK || e.target_slot!=view.self_slot || origin_index(&in->origin)<0) return true;
        return incoming_event(in->round,&e.event,now);
    }
    if (in->type==ZT_PKT_TIME_QUERY) {
        if (in->round!=current.round_id) return true;
        zt_wire_time_query_t q; zt_clock_sample_t c;
        int i=origin_index(&in->origin);
        if (zt_wire_decode_time_query(in->bytes,in->len,&q)!=ZT_OK || q.target_slot!=view.self_slot || i<0 ||
            zt_clock_read(now,&c)!=ZT_OK || c.quality!=ZT_TIME_INITIALIZED) return true;
        zt_wire_payload_t p={0}; p.time_reply=(zt_wire_time_reply_t){current.roster[i].slot,q.request_nonce,c.elapsed_ms,current.snapshot_rev,0,c.uncertainty_ms,ZT_TIME_INITIALIZED};
        send_payload(current.round_id,ZT_PKT_TIME_REPLY,&p,ZT_TX_PRIO_CLOCK_REPAIR); return true;
    }
    if (in->type==ZT_PKT_TIME_REPLY) {
        if (in->round!=current.round_id) return true;
        zt_wire_time_reply_t r;
        if (zt_wire_decode_time_reply(in->bytes,in->len,&r)!=ZT_OK || !time_sent_us || r.target_slot!=view.self_slot || r.request_nonce!=time_nonce ||
            !mac_equal(&time_peer,&in->origin) || r.time_quality!=ZT_TIME_INITIALIZED || r.sampled_elapsed_ms==INT32_MIN || in->rx_us<time_sent_us ||
            in->rx_us-time_sent_us>ZT_PEER_TIME_RTT_MAX_MS*1000ULL || r.source_age_ms>ZT_CLOCK_AGE_MAX_MS || r.source_snapshot_rev<current.snapshot_rev) return true;
        /* Anchor at RX with half the measured RTT. Keep the stricter peer
         * RTT limit and carry its source uncertainty plus G04's 50 ms margin. */
        uint32_t half=(uint32_t)((in->rx_us-time_sent_us+1999)/2000);
        uint64_t uncertainty=(uint64_t)r.uncertainty_ms+half+ZT_SERVER_TIME_MARGIN_MS;
        int64_t elapsed=(int64_t)r.sampled_elapsed_ms+half;
        if (uncertainty>ZT_TIME_UNCERTAINTY_MAX_MS || elapsed>INT32_MAX || elapsed<=INT32_MIN) return true;
        zt_clock_sample_t c={(int32_t)elapsed,(uint32_t)uncertainty,in->rx_us,ZT_TIME_INITIALIZED};
        if (zt_clock_apply(current.round_id,&c)==ZT_OK) { rejoining=false; refresh_role(); time_sent_us=0; }
        return true;
    }
    if (in->type==ZT_PKT_CLOSE_RECEIPTS) {
        if (in->round!=current.round_id || view.self_slot>=20) return true;
        zt_wire_close_receipts_t c;
        if (zt_wire_decode_close_receipts(in->bytes,in->len,&c)==ZT_OK && (c.closed_bitmap&(1u<<view.self_slot))) close_due=UINT64_MAX;
        return true;
    }
    return true;
}
static void peers_refresh(uint64_t now)
{
    size_t count=0;
    zt_err_t r=zt_peers_list(now,view.contacts,ZT_MAX_PLAYERS,&count);
    if (r!=ZT_OK && r!=ZT_ERR_NO_SPACE) count=0;
    for (unsigned i=0;i<count;++i) {
        int ri=roster_index(view.contacts[i].slot);
        if (ri>=0 && current.roles[ri].role_rev>view.contacts[i].role_rev) view.contacts[i].eligible=0;
    }
    view.direct_contact_count=count; view.selected_target=ZT_SLOT_INVALID;
    view.selected_name[0]=0; view.selected_tier=ZT_RANGE_UNKNOWN;
    for (unsigned i=0;i<count;++i) if (view.contacts[i].eligible && view.contacts[i].role==ZT_ROLE_HUMAN && view.contacts[i].slot!=view.self_slot) {
        view.selected_target=view.contacts[i].slot; memcpy(view.selected_name,view.contacts[i].name,sizeof(view.selected_name));
        view.selected_tier=view.contacts[i].tier; break;
    }
}
static void attempt(uint64_t now)
{
    zt_clock_sample_t c; zt_wire_role_entry_t *r=self_role();
    if (!r || r->role!=ZT_ROLE_ZOMBIE) { feedback(ZT_FEEDBACK_STAY_CLEAR,now); return; }
    if (!clock_running(now,&c) || cooldown_until>now || outbound.active || pending.id || r->cause_slot!=view.self_slot ||
        (!r->cause_seq && view.self_slot!=current.patient_zero_slot)) return;
    if (view.selected_target==ZT_SLOT_INVALID) { feedback(ZT_FEEDBACK_GET_CLOSER,now); return; }
    if (attempt_seq==UINT32_MAX || zt_mesh_get_boot_nonce(&boot_nonce)!=ZT_OK) { feedback(ZT_FEEDBACK_SYNC_REQUIRED,now); return; }
    zt_peer_entry_t *target=NULL;
    for (unsigned i=0;i<view.direct_contact_count;++i) if (view.contacts[i].slot==view.selected_target) target=&view.contacts[i];
    if (!target) return;
    outbound.request=(zt_wire_tag_request_t){target->slot,view.self_slot,r->cause_seq,r->role_rev,target->role_rev,++attempt_seq,c.elapsed_ms,target->latest_rssi};
    outbound.boot=boot_nonce; outbound.active=true; outbound.responded=false; outbound.confirmed=false;
    outbound.sent=0; outbound.started_us=now; outbound.next_us=now;
}
static void outbound_service(uint64_t now)
{
    if (!outbound.active) return;
    if (now-outbound.started_us>=ZT_TAG_CONFIRM_WAIT_MS*1000ULL) { outbound.active=false; feedback(ZT_FEEDBACK_UNCONFIRMED,now); return; }
    if (outbound.responded || outbound.sent>=3 || now<outbound.next_us) return;
    zt_clock_sample_t c;
    if (!clock_running(now,&c)) { outbound.active=false; feedback(ZT_FEEDBACK_UNCONFIRMED,now); return; }
    zt_wire_payload_t p={.tag_request=outbound.request};
    if (send_payload(current.round_id,ZT_PKT_TAG_REQUEST,&p,ZT_TX_PRIO_DIRECT_TAG)==ZT_OK) {
        if (!outbound.sent) cooldown_until=now+ZT_TAG_COOLDOWN_MS*1000ULL;
        ++outbound.sent;
        static const unsigned offsets[3]=ZT_TAG_RETRY_OFFSETS_MS;
        if (outbound.sent<3) outbound.next_us=outbound.started_us+offsets[outbound.sent]*1000ULL;
    }
}
static void join_service(uint64_t now)
{
    if (!join_nonce || now<join_due || (view.registered && !rejoining)) return;
    zt_wire_payload_t p={0}; p.join.requested_badge_mac=view.self_mac; p.join.name_len=strnlen(config.name,ZT_NAME_MAX_LEN);
    memcpy(p.join.name,config.name,p.join.name_len); p.join.known_round_id=current.round_id; p.join.request_nonce=join_nonce;
    memcpy(p.join.build_id,ZT_BUILD_ID,ZT_BUILD_ID_LEN);
    zt_err_t sent;
    if (is_host()) {
        /* ESP-NOW does not loop our own JOIN back. The playing host registers
         * through the same gateway queue and nonce lifecycle as other badges. */
        zt_game_feed_item_t item={.kind=ZT_GAME_FEED_JOIN,.round_id=current.round_id,
            .body.join={.request=p.join,.boot_nonce=boot_nonce}};
        sent=feed(&item);
    } else sent=send_payload(current.round_id,ZT_PKT_JOIN,&p,ZT_TX_PRIO_EVENT_CONTROL);
    uint32_t jitter=(uint32_t)(boot_nonce^(now/1000)^join_nonce)%1001;
    join_due=now+(sent==ZT_OK ? ZT_JOIN_RETRY_MS+jitter : 200+jitter/4)*1000ULL;
}
static void host_control_service(uint64_t now)
{
    if (!view.host_control_pending) return;
    if (view.host_control==ZT_HOST_CONTROL_START && current.round_id && current.phase>=ZT_PHASE_PREPARED) {
        clear_host_control();
        return;
    }
    if (host_control_request.submitted || now<host_control_request.retry_us) return;
    zt_err_t r=feed(&host_control_request.item);
    if (r==ZT_OK) host_control_request.submitted=true;
    else if (r==ZT_ERR_BUSY || r==ZT_ERR_NO_SPACE || r==ZT_ERR_INVALID_STATE) {
        host_control_request.retry_us=now+250000;
    } else {
        view.host_control_error=r; view.host_control_pending=0; view.last_error=r;
    }
}
static void button(const zt_button_edge_t *b,uint64_t now)
{
    if (b->kind!=ZT_EDGE_PRESS) return;
    if (b->button==ZT_BUTTON_START) { view.status_overlay=!view.status_overlay; return; }
    if (b->button==ZT_BUTTON_HOME) { view.status_overlay=0; return; }
    if (b->button==ZT_BUTTON_B) {
        if (view.status_overlay || view.host_control_pending) return;
        zt_host_control_t action=host_start_allowed() ? ZT_HOST_CONTROL_START :
            host_reset_allowed() ? ZT_HOST_CONTROL_RESET : ZT_HOST_CONTROL_NONE;
        if (action==ZT_HOST_CONTROL_NONE) return;
        view.host_control=action; view.host_control_error=ZT_OK;
        if (host_control_seq==UINT32_MAX) { view.host_control_error=ZT_ERR_OVERFLOW; return; }
        memset(&host_control_request,0,sizeof(host_control_request));
        host_control_request.item=(zt_game_feed_item_t){.kind=ZT_GAME_FEED_CONTROL,.round_id=current.round_id,
            .body.control={.action=action,.request_seq=++host_control_seq,.boot_nonce=boot_nonce,.registration_id=registration_id()}};
        view.host_control_pending=1;
        host_control_service(now);
        return;
    }
    if (b->button!=ZT_BUTTON_A || view.error!=ZT_ERROR_NONE) return;
    if (view.admission==ZT_ADMISSION_LOBBY || view.admission==ZT_ADMISSION_REGISTERING) {
        if (!join_nonce && zt_mesh_get_boot_nonce(&boot_nonce)==ZT_OK) {
            uint32_t seed=(uint32_t)boot_nonce^(uint32_t)(boot_nonce>>32);
            if (join_generation==UINT32_MAX) { view.last_error=ZT_ERR_OVERFLOW; return; }
            join_nonce=seed^++join_generation;
            if (!join_nonce) {
                if (join_generation==UINT32_MAX) { view.last_error=ZT_ERR_OVERFLOW; return; }
                join_nonce=seed^++join_generation;
            }
            view.admission=ZT_ADMISSION_REGISTERING;
            join_due=now+((uint32_t)(boot_nonce^join_nonce)%251)*1000ULL;
        }
        join_service(now); return;
    }
    if (view.admission==ZT_ADMISSION_RUNNING) attempt(now);
}
static void beacon_service(uint64_t now)
{
    if (now<beacon_due || zt_mesh_get_boot_nonce(&boot_nonce)!=ZT_OK) return;
    zt_clock_sample_t c; zt_wire_payload_t p={0}; zt_wire_beacon_t *b=&p.beacon;
    b->slot=view.self_slot; b->role=view.role; b->phase=view.phase; b->role_rev=view.role_rev;
    b->flags=(view.host_selected && view.host_configured ? ZT_BEACON_FLAG_HOST : 0) |
        (view.role_provisional ? ZT_BEACON_FLAG_PROVISIONAL : 0) | (view.error!=ZT_ERROR_STORAGE ? ZT_BEACON_FLAG_STORAGE_HEALTHY : 0) |
        (view.registered ? ZT_BEACON_FLAG_REGISTERED : 0) |
        (is_host() && view.server_connected ? ZT_BEACON_FLAG_SERVER_CONNECTED : 0);
    zt_wire_role_entry_t *r=self_role(); if (r && r->role==ZT_ROLE_ZOMBIE) b->infection_cause_seq=r->cause_seq;
    if (view.self_slot<20) { b->own_produced_seq=current.produced[view.self_slot]; b->own_server_received=current.received[view.self_slot]; b->own_server_final=current.decided[view.self_slot]; }
    if (current.round_id) {
        zt_wire_cache_key_t keys[128]; size_t count;
        if (zt_store_inventory(current.round_id,keys,128,&count)==ZT_OK && zt_wire_cache_digest(keys,count,&b->cache_digest)==ZT_OK) b->cache_count=count;
    }
    b->elapsed_ms=INT32_MIN;
    if (zt_clock_read(now,&c)==ZT_OK) { b->elapsed_ms=c.elapsed_ms; b->uncertainty_ms=c.uncertainty_ms>UINT16_MAX ? UINT16_MAX : c.uncertainty_ms; b->time_quality=c.quality; }
    b->round_channel=view.channel; b->snapshot_rev=current.snapshot_rev;
    b->gateway_boot_nonce=gateway_boot; b->gateway_serial=gateway_serial; b->gateway_hops=gateway_hops;
    uint64_t age=gateway_seen_us ? (now-gateway_seen_us)/1000000ULL : 255;
    b->gateway_age_s=age>255 ? 255 : age;
    /* Stable per-boot jitter does not require entropy calls before radio start. */
    uint32_t jitter=(uint32_t)(boot_nonce^(now/1000))%201;
    send_payload(current.round_id,ZT_PKT_BEACON,&p,ZT_TX_PRIO_COSMETIC);
    beacon_due=now+(ZT_BEACON_PERIOD_MS-ZT_BEACON_JITTER_MS+jitter)*1000ULL;
}
static void time_service(uint64_t now)
{
    zt_clock_sample_t c;
    if (!current.round_id || current.phase>ZT_PHASE_RUNNING || now<time_due ||
        (!rejoining && zt_clock_read(now,&c)==ZT_OK && c.uncertainty_ms<1000)) return;
    if (!view.direct_contact_count) return;
    const zt_peer_entry_t *peer=NULL;
    for (unsigned i=0;i<view.direct_contact_count;++i) if (view.contacts[i].slot!=view.self_slot && view.contacts[i].age_ms<=1000) { peer=&view.contacts[i]; break; }
    if (!peer || time_nonce==UINT32_MAX) return;
    zt_wire_payload_t p={0}; p.time_query.target_slot=peer->slot; p.time_query.request_nonce=++time_nonce;
    uint64_t sent=(uint64_t)esp_timer_get_time();
    if (send_payload(current.round_id,ZT_PKT_TIME_QUERY,&p,ZT_TX_PRIO_CLOCK_REPAIR)==ZT_OK) { time_sent_us=sent; time_peer=peer->mac; }
    time_due=now+1000000ULL;
}
static void replay_service(uint64_t now)
{
    if (!current.round_id || now<replay_due) return;
    zt_round_id_t old=zt_store_other_round(current.round_id);
    zt_round_id_t round=old && replay_previous ? old : current.round_id;
    uint16_t *cursor=round==current.round_id ? &replay_cursor : &previous_replay_cursor;
    uint8_t bytes[64]; size_t written; uint16_t next;
    if (zt_store_export(round,*cursor,bytes,sizeof(bytes),&written,&next)==ZT_OK) {
        if (written) {
            zt_durable_event_t e;
            if (zt_store_decode_event(bytes,written,&e)==ZT_OK &&
                (round!=current.round_id || current.received[e.event.victim_slot]<e.event.event_seq)) emit_event(e.round_id,&e.event);
        }
        *cursor=next==128 ? 0 : next;
    }
    replay_previous=!replay_previous; replay_due=now+ZT_EVENT_REPLAY_SERVICE_MS*1000ULL;
}
static void inventory_service(uint64_t now)
{
    if (!current.round_id || now<inventory_next || !view.direct_contact_count) return;
    zt_wire_cache_key_t keys[128]; size_t count;
    if (!inventory_pages) {
        if (now<inventory_due) return;
        zt_round_id_t old=zt_store_other_round(current.round_id);
        inventory_round=old && now>=previous_inventory_due ? old : current.round_id;
        inventory_target=view.contacts[0].slot;
        if (zt_store_inventory(inventory_round,keys,128,&count)!=ZT_OK || zt_wire_cache_digest(keys,count,&inventory_digest)!=ZT_OK) return;
        inventory_pages=(count+55)/56; if (!inventory_pages) inventory_pages=1;
        inventory_page=0;
    } else {
        uint64_t digest;
        if (zt_store_inventory(inventory_round,keys,128,&count)!=ZT_OK || zt_wire_cache_digest(keys,count,&digest)!=ZT_OK) return;
        if (digest!=inventory_digest) { inventory_pages=0; return; }
    }
    zt_wire_payload_t p={0}; p.cache_page.target_slot=inventory_target; p.cache_page.cache_digest=inventory_digest;
    p.cache_page.page_index=inventory_page; p.cache_page.page_count=inventory_pages;
    size_t base=inventory_page*56;
    p.cache_page.count=count-base>56 ? 56 : count-base;
    memcpy(p.cache_page.entries,keys+base,p.cache_page.count*sizeof(keys[0]));
    if (send_payload(inventory_round,ZT_PKT_CACHE_PAGE,&p,ZT_TX_PRIO_CLOCK_REPAIR)!=ZT_OK) return;
    inventory_next=now+ZT_CACHE_PAGE_SPACING_MS*1000ULL;
    if (++inventory_page==inventory_pages) {
        if (inventory_round!=current.round_id) previous_inventory_due=now+ZT_OLD_ROUND_INVENTORY_MS*1000ULL;
        inventory_pages=0; inventory_due=now+ZT_CACHE_NEIGHBOR_INTERVAL_MS*1000ULL;
    }
}
static zt_wire_host_state_t host_state_fields(const zt_checkpoint_t *cp)
{
    return (zt_wire_host_state_t){.snapshot_rev=cp->snapshot_rev,.roster_hash=cp->roster_hash,.phase=cp->phase,
        .host_elapsed_ms=ZT_INT32_UNKNOWN,.round_channel=cp->round_channel,.winner=view.result_present ? view.winner : ZT_ROLE_UNKNOWN,
        .duration_ms=ZT_ROUND_DURATION_MS,.remaining_ms=ZT_ROUND_DURATION_MS,.patient_zero_slot=cp->patient_zero_slot};
}
static bool host_sample(zt_wire_host_state_t *h,uint64_t now)
{
    /* Leave ample serial space between game submissions for mesh's 2s ticks.
     * Like all transport counters, this counter must never wrap. */
    if (now/1000>=UINT32_MAX-1000u) { view.last_error=ZT_ERR_OVERFLOW; return false; }
    h->gateway_serial=(uint32_t)(now/1000)+1;
    h->gateway_flags=view.server_connected ? ZT_HOST_STATE_FLAG_SERVER_CONNECTED : 0;
    if (h->phase>=ZT_PHASE_EXPIRED_PENDING_SYNC) {
        h->host_elapsed_ms=round_end_elapsed_ms; h->remaining_ms=ZT_ROUND_DURATION_MS-round_end_elapsed_ms;
        return true;
    }
    zt_clock_sample_t c;
    if (zt_clock_read(now,&c)==ZT_OK && c.quality==ZT_TIME_INITIALIZED && c.uncertainty_ms<=ZT_TIME_UNCERTAINTY_MAX_MS) {
        h->host_elapsed_ms=c.elapsed_ms; h->uncertainty_ms=c.uncertainty_ms;
        h->remaining_ms=c.elapsed_ms<0 ? ZT_ROUND_DURATION_MS : (uint32_t)c.elapsed_ms>=ZT_ROUND_DURATION_MS ? 0 : ZT_ROUND_DURATION_MS-c.elapsed_ms;
    }
    return true;
}
static void host_snapshot_service(uint64_t now)
{
    if (!is_host() || !current.round_id || !current.roster_count) return;
    zt_wire_host_state_t metadata=host_state_fields(&current);
    if (now>=host_metadata_due) {
        zt_wire_payload_t p={.host_state=metadata};
        if (host_sample(&p.host_state,now) && send_payload(current.round_id,ZT_PKT_HOST_STATE,&p,ZT_TX_PRIO_CLOCK_REPAIR)==ZT_OK) {
            host_metadata_due=now+ZT_HOST_CLOCK_PERIOD_MS*1000ULL;
        } else {
            host_metadata_due=now+ZT_STATE_COALESCE_MS*1000ULL;
        }
    }
    if (host_snapshot.active && (host_snapshot.cp.round_id!=current.round_id || host_snapshot.cp.snapshot_rev!=current.snapshot_rev ||
        host_snapshot.cp.roster_hash!=current.roster_hash || host_snapshot.cp.phase!=current.phase)) {
        /* Restart all outstanding pages from one committed revision. */
        if (host_snapshot.roster_page<(host_snapshot.cp.roster_count+7)/8) host_snapshot.roster_requested=1;
        host_snapshot.active=0; host_snapshot.requested=1;
    }
    if (!host_snapshot.active && (host_snapshot.requested || now>=host_snapshot.periodic_due)) {
        host_snapshot.cp=current; host_snapshot.active=1;
        host_snapshot.roster_page=host_snapshot.roster_requested ? 0 : (current.roster_count+7)/8;
        host_snapshot.role_page=0;
        host_snapshot.requested=host_snapshot.roster_requested=0;
        host_snapshot.due=now+ZT_STATE_COALESCE_MS*1000ULL; host_snapshot.deadline=now+60000000ULL;
    }
    if (!host_snapshot.active || now<host_snapshot.due) return;
    if (now>=host_snapshot.deadline) {
        view.last_error=ZT_ERR_TIMEOUT; host_snapshot.active=0; host_snapshot.periodic_due=now+ZT_STATE_PERIOD_MS*1000ULL;
        ESP_LOGW("zt_game","Host snapshot transmission timed out"); return;
    }
    const zt_checkpoint_t *cp=&host_snapshot.cp;
    zt_wire_payload_t p={0}; zt_pkt_type_t type;
    unsigned roster_pages=(cp->roster_count+7)/8, role_pages=(cp->roster_count+11)/12;
    if (host_snapshot.roster_page<roster_pages) {
        type=ZT_PKT_ROSTER_PAGE;
        unsigned base=host_snapshot.roster_page*8, count=cp->roster_count-base; if (count>8) count=8;
        p.roster_page=(zt_wire_roster_page_t){.snapshot_rev=cp->snapshot_rev,.roster_hash=cp->roster_hash,
            .page_index=host_snapshot.roster_page,.page_count=roster_pages,.entry_count=count};
        memcpy(p.roster_page.entries,cp->roster+base,count*sizeof(cp->roster[0]));
    } else if (host_snapshot.role_page<role_pages) {
        type=ZT_PKT_HOST_STATE; p.host_state=host_state_fields(cp);
        if (!host_sample(&p.host_state,now)) return;
        unsigned base=host_snapshot.role_page*12, count=cp->roster_count-base; if (count>12) count=12;
        p.host_state.page_index=host_snapshot.role_page; p.host_state.page_count=role_pages; p.host_state.entry_count=count;
        memcpy(p.host_state.entries,cp->roles+base,count*sizeof(cp->roles[0]));
    } else {
        type=ZT_PKT_WATERMARKS; p.watermarks.snapshot_rev=cp->snapshot_rev; p.watermarks.roster_hash=cp->roster_hash;
        p.watermarks.count=cp->roster_count;
        for (unsigned i=0;i<cp->roster_count;++i) {
            unsigned s=cp->roster[i].slot;
            p.watermarks.entries[i]=(zt_wire_watermark_entry_t){s,cp->received[s],cp->decided[s]};
        }
    }
    host_snapshot.due=now+ZT_STATE_COALESCE_MS*1000ULL;
    if (send_payload(cp->round_id,type,&p,ZT_TX_PRIO_EVENT_CONTROL)!=ZT_OK) return;
    if (type==ZT_PKT_ROSTER_PAGE) ++host_snapshot.roster_page;
    else if (type==ZT_PKT_HOST_STATE) ++host_snapshot.role_page;
    else {
        host_snapshot.active=0;
        host_snapshot.periodic_due=now+ZT_STATE_PERIOD_MS*1000ULL;
    }
}
static void host_command_service(uint64_t now)
{
    if (!is_host()) return;
    static unsigned cursor;
    for (unsigned n=0;n<ZT_PENDING_COMMAND_CAPACITY*ZT_MAX_PLAYERS;++n) {
        unsigned index=cursor++%(ZT_PENDING_COMMAND_CAPACITY*ZT_MAX_PLAYERS), ci=index/ZT_MAX_PLAYERS, target=index%ZT_MAX_PLAYERS;
        command_slot_t *slot=&commands[ci];
        if (slot->occupied!=COMMAND_DELIVERING) continue;
        zt_clock_sample_t clock;
        bool cosmetic=slot->value.command.kind==ZT_CMD_ANNOUNCE;
        bool expired=cosmetic && (current.phase!=ZT_PHASE_RUNNING || zt_clock_read(now,&clock)!=ZT_OK ||
            clock.quality!=ZT_TIME_INITIALIZED || clock.elapsed_ms<0 ||
            (uint32_t)clock.elapsed_ms>=slot->value.command.valid_until_elapsed_ms);
        if (!slot->targets || now>=slot->deadline_us || expired || slot->value.round_id!=current.round_id) {
            if (slot->targets && !expired && !cosmetic) {
                view.last_error=ZT_ERR_TIMEOUT;
                ESP_LOGW("zt_game","Command %lu round %llu missing receipt bitmap 0x%05lx",
                    (unsigned long)slot->value.command.command_seq,(unsigned long long)slot->value.round_id,(unsigned long)slot->targets);
            }
            portENTER_CRITICAL(&ingress_guard); slot->occupied=COMMAND_CACHED; portEXIT_CRITICAL(&ingress_guard);
            continue;
        }
        if (!(slot->targets&(1u<<target)) || now<slot->retry_us[target] || slot->attempts[target]>=HOST_COMMAND_ATTEMPTS) continue;
        zt_wire_payload_t p={.command=slot->value.command}; p.command.target_slot=target;
        /* One initial round-wide envelope avoids twenty identical floods.
         * Subsequent attempts name only targets still missing their receipt. */
        if (slot->first_broadcast) p.command.target_slot=ZT_SLOT_ALL;
        if (p.command.kind==ZT_CMD_START_ROUND) {
            zt_wire_start_round_args_t a; size_t written;
            if (zt_wire_decode_start_round_args(p.command.args,p.command.args_len,&a)!=ZT_OK ||
                zt_clock_read(now,&clock)!=ZT_OK || clock.quality!=ZT_TIME_INITIALIZED || clock.uncertainty_ms>ZT_TIME_UNCERTAINTY_MAX_MS) return;
            a.sampled_elapsed_ms=clock.elapsed_ms; a.uncertainty_ms=clock.uncertainty_ms;
            if (zt_wire_encode_start_round_args(&a,p.command.args,sizeof(p.command.args),&written)!=ZT_OK) return;
            p.command.args_len=written;
        }
        /* Submission success is only transport custody. Only that target's
         * authenticated terminal receipt removes its bit. */
        if (send_payload(slot->value.round_id,ZT_PKT_COMMAND,&p,cosmetic ? ZT_TX_PRIO_COSMETIC : ZT_TX_PRIO_EVENT_CONTROL)==ZT_OK) {
            if (slot->first_broadcast) {
                slot->first_broadcast=0;
                for (unsigned s=0;s<ZT_MAX_PLAYERS;++s) if (slot->targets&(1u<<s)) {
                    slot->retry_us[s]=now+ZT_COMMAND_RETRY_MS*1000ULL; ++slot->attempts[s];
                }
            } else { slot->retry_us[target]=now+ZT_COMMAND_RETRY_MS*1000ULL; ++slot->attempts[target]; }
        }
        return; /* Bounded work, fair across commands and targets under load. */
    }
}
static void host_decision_service(uint64_t now)
{
    if (!is_host()) return;
    for (unsigned pass=0;pass<2;++pass) {
        unsigned index=(decision_relay_turn+pass)%2;
        zt_round_id_t round=index ? previous_round.round : current.round_id;
        if (!round || round==reset_record.round_id) continue;
        if (decision_relay[index].round!=round) {
            decision_relay[index].round=round; decision_relay[index].cursor=0; decision_relay[index].due=0;
        }
        if (now<decision_relay[index].due) continue;
        decision_relay_turn=(index+1)%2;
        zt_wire_event_decisions_t *page=&decision_payload.event_decisions;
        memset(page,0,sizeof(*page));
        size_t count=0; uint16_t next=decision_relay[index].cursor;
        zt_err_t result=zt_store_read_decisions(round,decision_relay[index].cursor,page->entries,
            ZT_DECISION_PAGE_ENTRIES,&count,&next);
        if (result==ZT_ERR_BUSY) return;
        if (result!=ZT_OK) { decision_relay[index].due=now+5000000ULL; return; }
        const uint16_t *received=index ? previous_decision_received : decision_received;
        zt_slot_t self=index ? previous_round.self : view.self_slot;
        for (unsigned i=0;i<count;++i) {
            const zt_wire_decision_entry_t *entry=&page->entries[i];
            if (entry->victim_slot==self || (entry->status!=ZT_DECISION_PENDING_DEPENDENCY && entry->event_seq<=received[entry->victim_slot])) continue;
            page->entries[page->count++]=*entry;
        }
        page->snapshot_rev=index ? 0 : current.snapshot_rev;
        /* Actual badge receipts suppress final decisions. A host's own durable
         * frontier never claims that a remote victim has applied its decision. */
        if (page->count && send_payload(round,ZT_PKT_EVENT_DECISIONS,&decision_payload,ZT_TX_PRIO_EVENT_CONTROL)!=ZT_OK) return;
        decision_relay[index].cursor=next>=ZT_JOURNAL_CAPACITY ? 0 : next;
        decision_relay[index].due=now+(next>=ZT_JOURNAL_CAPACITY ? 5000000ULL : ZT_STATE_COALESCE_MS*1000ULL);
        return;
    }
}
static void timers(uint64_t now)
{
    zt_clock_sample_t c;
    if (zt_clock_read(now,&c)==ZT_OK) {
        view.diagnostics.clock_uncertainty_ms=c.uncertainty_ms;
        if (current.phase<ZT_PHASE_EXPIRED_PENDING_SYNC) {
            view.countdown_ms=c.elapsed_ms<0 ? -c.elapsed_ms : 0;
            view.remaining_ms=c.elapsed_ms<0 ? ZT_ROUND_DURATION_MS : (uint32_t)c.elapsed_ms>=ZT_ROUND_DURATION_MS ? 0 : ZT_ROUND_DURATION_MS-c.elapsed_ms;
        }
        if (current.phase==ZT_PHASE_RUNNING && c.elapsed_ms>=ZT_ROUND_DURATION_MS) {
            current.phase=ZT_PHASE_EXPIRED_PENDING_SYNC;
            /* Peer roles can be partial or provisional. Only an authenticated
             * server result may declare the all-infected victory. */
            display_end(ZT_ROLE_HUMAN,ZT_ROUND_DURATION_MS); refresh_role(); checkpoint_dirty=true;
        }
    }
    if (pending.id && pending.action==APPLY_EVENT && !pending.pending_sent && now-pending.submitted_us>=ZT_PERSIST_PENDING_MS*1000ULL) {
        zt_wire_tag_result_t r={.actor_slot=pending.event.actor_slot,.request_boot=pending.event.request_boot,.request_seq=pending.event.request_seq,.result=ZT_TAG_PENDING};
        send_result(current.round_id,&r); pending.pending_sent=1;
    }
    if (pending.id && current.phase>=ZT_PHASE_EXPIRED_PENDING_SYNC && now-pending.submitted_us>=ZT_PERSIST_PENDING_MS*1000ULL) {
        /* Still unresolved: expose the failure, never claim a complete close. */
        view.last_error=ZT_ERR_TIMEOUT; view.error=ZT_ERROR_STORAGE;
    }
    for (unsigned i=0;i<16;++i) if (outcomes[i].round && outcomes[i].repeat_us && now>=outcomes[i].repeat_us) {
        if (now<outcomes[i].expires_us) send_result(outcomes[i].round,&outcomes[i].result);
        outcomes[i].repeat_us=0;
    }
    if (!pending.id && current.round_id && current.phase>=ZT_PHASE_EXPIRED_PENDING_SYNC && !close_committed && view.error!=ZT_ERROR_STORAGE) {
        zt_persist_request_t r={.kind=ZT_PERSIST_CLOSE,.round_id=current.round_id};
        r.value.close=(zt_wire_round_closed_t){view.self_slot,current.produced[view.self_slot],view.role_rev,round_end_elapsed_ms};
        submit(&r,APPLY_CLOSE,now);
    } else if (!pending.id && checkpoint_dirty && view.error!=ZT_ERROR_STORAGE) {
        staged=current; pending.receipt.command_seq=0; save_checkpoint(now);
    }
    if (view.feedback_expires_us<=now) view.feedback=ZT_FEEDBACK_NONE;
    if (view.announcement_expires_us<=now) view.announcement[0]=0;
    if (event_retry_count && now>=event_retry_us && view.self_slot<20) {
        if (current.received[view.self_slot]<last_local_event.event_seq) emit_event(current.round_id,&last_local_event);
        if (++event_retry_count>=3) event_retry_count=0; else event_retry_us=now+1000000ULL;
    }
    if (view.registered && !is_host() && (!current.round_id ||
        (current.phase>=ZT_PHASE_EXPIRED_PENDING_SYNC && !view.result_final))) request_snapshot(now);
    host_control_service(now); host_snapshot_service(now); host_command_service(now); host_decision_service(now);
    closed_feed(now); outbound_service(now); time_service(now); join_service(now); beacon_service(now); replay_service(now); inventory_service(now);
}
zt_err_t zt_game_service(uint64_t now_us)
{
    if (!initialized) return ZT_ERR_INVALID_STATE;
    TaskHandle_t caller=xTaskGetCurrentTaskHandle();
    if (owner!=caller) return ZT_ERR_INVALID_STATE;
    /* Caller timestamps are scheduling hints only; gameplay uses esp_timer. */
    (void)now_us; uint64_t now=(uint64_t)esp_timer_get_time();
    configuration_service(now);
    if (config_present) {
        reset_service(now);
        uint8_t selected,changed;
        if (zt_buttons_boot_host_switch(&selected,&changed)==ZT_OK) {
            if (!host_latched) {
                host_latched=true; view.host_selected=selected;
                if (selected && !view.host_configured) { error_overlay(ZT_ERR_AUTH); strcpy(view.announcement,"Host switch requires this badge's host configuration"); }
            }
            view.aux1_changed_live=changed;
        }
        view.diagnostics.gateway_initialized=0;
        view.diagnostics.gateway_activity=ZT_GATEWAY_ACTIVITY_IDLE;
        view.diagnostics.gateway_http_age_ms=UINT32_MAX;
        if (host_latched && view.host_selected && view.host_configured) {
            zt_gateway_status_t gateway;
            if (zt_gateway_status(&gateway)==ZT_OK) {
                view.diagnostics.gateway_initialized=1;
                view.diagnostics.gateway_error=(uint32_t)gateway.last_error;
                view.diagnostics.gateway_http_status=gateway.http_status>0 ? (uint32_t)gateway.http_status : 0;
                view.diagnostics.gateway_activity=(uint8_t)gateway.activity;
                if (gateway.last_http_success_us && now>=gateway.last_http_success_us) {
                    uint64_t http_age_ms=(now-gateway.last_http_success_us)/1000;
                    view.diagnostics.gateway_http_age_ms=http_age_ms>UINT32_MAX ? UINT32_MAX : (uint32_t)http_age_ms;
                }
                view.server_age_ms=gateway.last_inbound_us && now>=gateway.last_inbound_us ? (uint32_t)((now-gateway.last_inbound_us)/1000) : UINT32_MAX;
                view.server_connected=gateway.connected && gateway.welcomed && !gateway.auth_error && view.server_age_ms<ZT_GATEWAY_STALE_LINK_MS;
                view.diagnostics.gateway_reconnects=gateway.reconnect_count;
                if (gateway.auth_error) { error_overlay(ZT_ERR_AUTH); strcpy(view.announcement,"Host credentials rejected; configure over USB"); }
            }
        }
        zt_channel_status_t channel;
        view.diagnostics.wifi_has_ip=0;
        if (zt_channel_get_status(&channel)==ZT_OK) {
            view.diagnostics.wifi_has_ip=channel.has_ip;
            if (channel.driver_generation!=driver_generation || channel.channel!=view.channel) {
                driver_generation=channel.driver_generation; zt_peers_invalidate(driver_generation); host_seen_us=0; server_report.seen_us=0;
            }
            view.channel=channel.channel;
        }
        peers_refresh(now);
    }
    if (deferred_present && !pending.id && process_input(&deferred,now)) deferred_present=false;
    for (unsigned count=0;count<4;++count) {
        input_t in;
        portENTER_CRITICAL(&ingress_guard);
        bool have=qcount!=0;
        if (have) { in=queue[qhead]; qhead=(qhead+1)%32; --qcount; }
        portEXIT_CRITICAL(&ingress_guard);
        if (!have) break;
        if (!process_input(&in,now)) {
            if (!deferred_present) { deferred=in; deferred_present=true; }
            else {
                portENTER_CRITICAL(&ingress_guard); zt_err_t r=enqueue_locked(&in); portEXIT_CRITICAL(&ingress_guard);
                if (r!=ZT_OK) { request_snapshot(now); view.last_error=ZT_ERR_BUSY; }
            }
        }
    }
    /* Pages precede commands so PREPARE can finish without waiting for a new
     * command envelope. Work stays bounded even under a continuous RX stream. */
    for (unsigned i=0;i<2;++i) if (pages[i].occupied>=2 && process_page(i,now)) {
        portENTER_CRITICAL(&ingress_guard); pages[i].occupied=0; portEXIT_CRITICAL(&ingress_guard);
    }
    int chosen=-1;
    for (unsigned i=0;i<8;++i) if (commands[i].occupied==2) {
        bool reset=commands[i].value.command.kind==ZT_CMD_RESET_GAME;
        bool old_reset=chosen>=0 && commands[chosen].value.command.kind==ZT_CMD_RESET_GAME;
        bool cosmetic=commands[i].value.command.kind==ZT_CMD_ANNOUNCE;
        bool old_cosmetic=chosen>=0 && commands[chosen].value.command.kind==ZT_CMD_ANNOUNCE;
        if (chosen<0 || (reset && !old_reset) || (!cosmetic && old_cosmetic) ||
            (reset==old_reset && cosmetic==old_cosmetic && commands[i].value.command.command_seq<commands[chosen].value.command.command_seq)) chosen=i;
    }
    if (chosen>=0 && process_command(chosen,now)) {
        portENTER_CRITICAL(&ingress_guard);
        if (commands[chosen].occupied==2) commands[chosen].occupied=0;
        portEXIT_CRITICAL(&ingress_guard);
    }
    zt_button_edge_t edge;
    portENTER_CRITICAL(&ingress_guard); bool have_button=bcount!=0;
    if (have_button) { edge=buttons[bhead]; bhead=(bhead+1)%16; --bcount; }
    portEXIT_CRITICAL(&ingress_guard);
    if (config_present && host_latched) {
        peers_refresh(now);
        if (have_button && host_latched) button(&edge,now);
        timers(now);
        if (rejoining) { view.admission=ZT_ADMISSION_REJOINING; request_snapshot(now); }
    }
    view.host_age_ms=host_seen_us && now>=host_seen_us ? (uint32_t)((now-host_seen_us)/1000) : UINT32_MAX;
    view.host_connected=host_seen_us && view.host_age_ms<ZT_GATEWAY_DISCOVERY_MAX_AGE_S*1000u;
    if (!is_host()) {
        uint64_t age=server_report.seen_us && now>=server_report.seen_us ? (now-server_report.seen_us)/1000 : UINT32_MAX;
        view.server_age_ms=age>UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        view.server_connected=server_report.online && age<ZT_SERVER_STATUS_MAX_AGE_MS;
    }
    zt_store_status_t ss;
    if (zt_store_status(&ss)==ZT_OK) {
        view.pending_event_count=ss.event_count;
        if (ss.install_state==ZT_INSTALL_ERROR) error_overlay(ZT_ERR_STORAGE);
    }
    portENTER_CRITICAL(&ingress_guard); view.diagnostics.input_drops=ingress_drops; view.diagnostics.event_high_water=ingress_high_water; portEXIT_CRITICAL(&ingress_guard);
    view.host_can_start=host_start_allowed(); view.host_can_reset=host_reset_allowed();
    if (now>=publish_due) {
        zt_radio_diagnostics_t radio;
        if (zt_radio_get_diagnostics(&radio)==ZT_OK) {
            view.diagnostics.rx_drops=radio.rx_drops;
            view.diagnostics.tx_drops=radio.tx_drops;
            view.diagnostics.invalid_frames=radio.invalid_frames;
            view.diagnostics.auth_failures=radio.auth_failures;
            view.diagnostics.dedupe_hits=radio.dedupe_hits;
            view.diagnostics.rx_high_water=radio.rx_high_water;
            view.diagnostics.tx_high_water=radio.tx_high_water;
            view.diagnostics.tx_watchdogs=radio.tx_watchdogs;
            view.diagnostics.radio_restarts=radio.radio_restarts;
            view.diagnostics.replay_backlog=radio.replay_backlog;
            view.diagnostic_mode=radio.diagnostic_mode;
        }
        view.diagnostics.free_heap=heap_caps_get_free_size(MALLOC_CAP_8BIT);
        view.diagnostics.minimum_heap=heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
        view.diagnostics.largest_free_block=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        view.sampled_us=now; ++view.generation;
        uint64_t active_registration=registration_id();
        portENTER_CRITICAL(&view_guard); published=view; published_registration_id=active_registration; portEXIT_CRITICAL(&view_guard);
        if (ui_sink) {
            zt_err_t r=ui_sink(&published,sink_context);
            if (r!=ZT_OK) view.last_error=r;
        }
        publish_due=now+100000;
    }
    return ZT_OK;
}
zt_err_t zt_game_snapshot(zt_ui_snapshot_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&view_guard); *out=published; portEXIT_CRITICAL(&view_guard); return ZT_OK;
}
bool zt_game_admission_enabled(void)
{
    portENTER_CRITICAL(&view_guard);
    bool enabled=published.registered!=0;
    portEXIT_CRITICAL(&view_guard);
    return enabled;
}
uint64_t zt_game_registration_id(void)
{
    portENTER_CRITICAL(&view_guard); uint64_t id=published_registration_id; portEXIT_CRITICAL(&view_guard);
    return id;
}
zt_err_t zt_game_event_feed(zt_round_id_t round,uint16_t cursor,zt_gateway_event_t *out,size_t cap,size_t *count,uint16_t *next)
{
    if (!out || !count || !next || !cap || cursor>128) return ZT_ERR_INVALID_ARG;
    *count=0; *next=cursor;
    while (*count<cap && *next<128) {
        uint8_t bytes[64]; size_t n; uint16_t following;
        zt_err_t r=zt_store_export(round,*next,bytes,sizeof(bytes),&n,&following);
        if (r!=ZT_OK) return r;
        *next=following;
        if (n) {
            zt_durable_event_t e; r=zt_store_decode_event(bytes,n,&e); if (r!=ZT_OK) return r;
            out[*count]=(zt_gateway_event_t){.id={round,e.event.victim_slot,e.event.event_seq},.body=e.event}; ++*count;
        }
    }
    return ZT_OK;
}
