#include "zt_radio.h"
#include "zt_game.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern const zt_radio_config_t *zt_radio_configuration(void);
extern zt_boot_nonce_t zt_radio_boot_nonce(void);
extern uint32_t zt_radio_generation(void);
extern zt_err_t zt_radio_discard_round(zt_round_id_t round);
extern bool zt_radio_flood_type(uint8_t type);
extern zt_err_t zt_radio_wire_peek(const uint8_t *, size_t, zt_wire_header_t *);
extern zt_err_t zt_radio_payload_decode(uint8_t, const uint8_t *, size_t, zt_wire_payload_t *);
extern zt_err_t zt_radio_payload_encode(uint8_t, const zt_wire_payload_t *, uint8_t *, size_t, size_t *);
extern zt_err_t zt_radio_channel_discovered(uint8_t channel, uint64_t rx_us);

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static zt_domain_sink_t domain_sink;
static void *domain_context;
static uint32_t packet_seq;
static _Atomic uint32_t invalid_frames, auth_failures, dedupe_hits;
static _Atomic uint32_t diagnostic_replay_backlog, diagnostic_gateway_hops;
static _Atomic uint32_t diagnostic_gateway_age = UINT32_MAX;
static _Atomic int32_t diagnostic_last_rssi;

static void count_rejection(_Atomic uint32_t *counter)
{
    uint32_t n = atomic_load_explicit(counter, memory_order_relaxed);
    if (n != UINT32_MAX) atomic_store_explicit(counter, n + 1, memory_order_relaxed);
}

/* Only word-sized published fields are read here: no mesh mutex, cache scan,
 * timestamps, allocation, or pointers into game/driver state. */
void zt_radio_mesh_diagnostics(zt_radio_diagnostics_t *out)
{
    out->invalid_frames = atomic_load_explicit(&invalid_frames, memory_order_relaxed);
    out->auth_failures = atomic_load_explicit(&auth_failures, memory_order_relaxed);
    out->dedupe_hits = atomic_load_explicit(&dedupe_hits, memory_order_relaxed);
    out->replay_backlog = atomic_load_explicit(&diagnostic_replay_backlog, memory_order_relaxed);
    out->gateway_age_ms = atomic_load_explicit(&diagnostic_gateway_age, memory_order_relaxed);
    out->gateway_hops = (uint8_t)atomic_load_explicit(&diagnostic_gateway_hops, memory_order_relaxed);
    out->last_rssi = (int8_t)atomic_load_explicit(&diagnostic_last_rssi, memory_order_relaxed);
}
static portMUX_TYPE invalidation_guard = portMUX_INITIALIZER_UNLOCKED;
static uint64_t invalidated_us;
static bool invalidate_pending;
/* No unexpired dedupe entry is displaced to admit another flood. */
typedef struct {
    uint64_t boot;
    uint32_t seq, expires_ms;
    zt_mac_t origin;
    uint8_t used;
} dedupe_t;
_Static_assert(sizeof(dedupe_t) == 24, "dedupe budget");
static dedupe_t dedupe[ZT_DEDUPE_CAPACITY];
typedef struct {
    zt_round_id_t id;
    uint64_t hash;
    uint32_t revision, mask, archived;
    uint8_t complete, count, self_slot, restoring;
    uint16_t restore_cursor;
    zt_wire_roster_entry_t roster[ZT_MAX_PLAYERS];
    uint16_t received[ZT_MAX_PLAYERS];
} round_t;
static round_t rounds[ZT_RETAINED_ROUND_CAPACITY];
static struct {
    zt_round_id_t round;
    uint64_t hash;
    uint32_t rev;
    uint8_t pages, seen, counts[3];
    zt_wire_roster_entry_t entries[ZT_SNAPSHOT_MAX_PAGES * ZT_ROSTER_PAGE_ENTRIES];
} roster_assembly;
/* Canonical event bytes avoid structure padding; two retained rounds share the
 * one 128-entry cache. No digest mismatch can delete an entry. */
enum { CACHE_OWN = 1, CACHE_RETAINED = 2 };
typedef struct { uint64_t round; uint8_t body[30], flags, retry_step; } cache_t;
_Static_assert(sizeof(cache_t) == 40, "event cache budget");
static cache_t cache[ZT_CACHE_CAPACITY];
static uint32_t replay_due[ZT_CACHE_CAPACITY];
static uint16_t cache_count;
static uint8_t restore_turn;
static uint64_t replay_service_due, beacon_due, clock_due, old_inventory_due;
static zt_wire_beacon_t beacon_template;
static zt_wire_host_state_t host_template;
static zt_round_id_t beacon_round, host_round;
static bool have_beacon, have_host;
static uint64_t beacon_source_us, host_source_us;
static uint64_t gateway_boot, gateway_seen;
static uint32_t gateway_serial, gateway_initial_age;
static uint8_t gateway_hops;
typedef struct {
    uint64_t boot, digest, last_repair_us;
    uint32_t seq, last_ms, sample_ms[3];
    int16_t ewma_q8;
    int8_t last_rssi;
    uint8_t samples, seen, changed;
} peer_t;
static peer_t peers[ZT_MAX_PLAYERS];
static uint8_t repair_cursor;
static struct {
    bool active, responding;
    zt_round_id_t round;
    uint8_t target, inventory_target, page, pages, key_count, copy_count, copy_index;
    uint64_t digest, due, expires;
    zt_wire_cache_key_t keys[ZT_CACHE_CAPACITY];
    zt_wire_cache_key_t copies[ZT_WANT_EVENT_ENTRIES];
} exchange;
static uint16_t repair_request_seq;
static struct { bool active; zt_round_id_t round; uint8_t peer; uint32_t nonce; uint64_t sent; } time_query;
static struct { bool active; zt_round_id_t round; uint8_t victim; uint32_t attempt; uint64_t boot, sent; } tag_request;
static uint64_t last_join, last_snapshot;
static zt_round_id_t reset_round;
/* Cleanup traffic outlives the round's gameplay state. Keep only the identities
 * authorized by the latest host reset, never an event cache or playable roster. */
static struct {
    zt_round_id_t round;
    uint32_t mask, command_seq[ZT_MAX_PLAYERS];
    uint64_t request_due[ZT_MAX_PLAYERS];
    zt_mac_t macs[ZT_MAX_PLAYERS];
    bool complete_roster;
} reset_relay;

static bool mac_equal(const zt_mac_t *a, const zt_mac_t *b) { return memcmp(a->bytes, b->bytes, 6) == 0; }
static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }
static uint32_t jitter(uint32_t base, uint32_t percent)
{
    uint32_t span = base * percent / 100;
    return base - span + esp_random() % (2 * span + 1);
}
static round_t *round_find(zt_round_id_t id)
{
    for (unsigned i = 0; i < 2; ++i) if (rounds[i].id == id && id) return &rounds[i];
    return NULL;
}
/* Called by owners while holding mutex; readers see only the scalar mirrors. */
static void publish_diagnostics(uint64_t now)
{
    uint32_t backlog = 0;
    for (unsigned i = 0; i < cache_count; ++i) if (cache[i].flags & CACHE_OWN) {
        round_t *r = round_find(cache[i].round);
        uint16_t seq = (uint16_t)cache[i].body[1] | (uint16_t)cache[i].body[2] << 8;
        if (!r || r->received[cache[i].body[0]] < seq) ++backlog;
    }
    atomic_store_explicit(&diagnostic_replay_backlog, backlog, memory_order_relaxed);
    uint64_t age = gateway_seen ? (now > gateway_seen ? (now - gateway_seen) / 1000 : 0) + gateway_initial_age : UINT32_MAX;
    atomic_store_explicit(&diagnostic_gateway_age, age > UINT32_MAX ? UINT32_MAX : (uint32_t)age, memory_order_relaxed);
    atomic_store_explicit(&diagnostic_gateway_hops, gateway_hops, memory_order_relaxed);
}

static round_t *round_add(zt_round_id_t id)
{
    round_t *r = round_find(id);
    if (r) return r;
    for (unsigned i = 0; i < 2; ++i) if (!rounds[i].id) {
        rounds[i].id = id; rounds[i].self_slot = ZT_SLOT_INVALID;
        return &rounds[i];
    }
    /* Reuse only an explicitly archived retained round. A different digest
     * or arrival of a new round is never evidence of archive clearance. */
    for (unsigned i = 0; i < 2; ++i) if (rounds[i].id != beacon_round && rounds[i].mask &&
        (rounds[i].archived & rounds[i].mask) == rounds[i].mask) {
        zt_round_id_t old = rounds[i].id;
        for (unsigned j = 0; j < cache_count;) {
            if (cache[j].round != old) { ++j; continue; }
            --cache_count;
            cache[j] = cache[cache_count]; replay_due[j] = replay_due[cache_count];
        }
        memset(&rounds[i], 0, sizeof(rounds[i]));
        rounds[i].id = id; rounds[i].self_slot = ZT_SLOT_INVALID;
        return &rounds[i];
    }
    return NULL;
}
static int origin_slot(const round_t *r, const zt_mac_t *mac)
{
    if (r) for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i)
        if ((r->mask & (1u << i)) && mac_equal(&r->roster[i].mac, mac)) return i;
    return -1;
}
static bool mapped(const round_t *r, uint8_t s, const zt_mac_t *mac)
{
    return r && s < 20 && (r->mask & (1u << s)) && mac_equal(&r->roster[s].mac, mac);
}
static bool member(const round_t *r, uint8_t s) { return r && s < 20 && (r->mask & (1u << s)); }
static void remember_reset_roster(zt_round_id_t round)
{
    if (reset_relay.round != round) {
        memset(&reset_relay, 0, sizeof(reset_relay));
        reset_relay.round = round;
    }
    const round_t *r = round_find(round);
    if (!r || !r->complete) return;
    reset_relay.mask = r->mask;
    reset_relay.complete_roster = true;
    for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i)
        if (r->mask & (1u << i)) reset_relay.macs[i] = r->roster[i].mac;
}
static void remember_reset_command(zt_round_id_t round, const zt_wire_command_t *command)
{
    if (!round || command->kind != ZT_CMD_RESET_GAME || command->target_slot >= ZT_MAX_PLAYERS) return;
    remember_reset_roster(round);
    unsigned slot = command->target_slot;
    if (command->args_len == 14) {
        /* Lobby cleanup has no frozen roster. The authenticated host names the
         * registered target MAC; the game owner checks its registration token. */
        zt_mac_t target;
        memcpy(target.bytes, command->args, sizeof(target.bytes));
        if (reset_relay.complete_roster && (!(reset_relay.mask & (1u << slot)) ||
            !mac_equal(&reset_relay.macs[slot], &target))) return;
        reset_relay.macs[slot] = target;
        reset_relay.mask |= 1u << slot;
    }
    if (reset_relay.mask & (1u << slot)) reset_relay.command_seq[slot] = command->command_seq;
}
static bool reset_relay_member(const zt_mac_t *mac)
{
    for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i)
        if ((reset_relay.mask & (1u << i)) && mac_equal(&reset_relay.macs[i], mac)) return true;
    return false;
}
static bool reset_receipt_matches(const zt_domain_message_t *m)
{
    const zt_wire_command_receipt_t *receipt = &m->payload.command_receipt;
    unsigned slot = receipt->slot;
    return m->header.round_id == reset_relay.round && slot < ZT_MAX_PLAYERS &&
        (reset_relay.mask & (1u << slot)) && reset_relay.command_seq[slot] &&
        receipt->command_seq == reset_relay.command_seq[slot] &&
        mac_equal(&m->header.origin, &reset_relay.macs[slot]) &&
        (!m->header.hops || !reset_relay.complete_roster || reset_relay_member(&m->direct_source));
}
static bool reset_snapshot_matches(const zt_domain_message_t *m)
{
    if (m->header.type!=ZT_PKT_SNAPSHOT_REQUEST || !m->header.round_id ||
        m->header.round_id!=reset_relay.round) return false;
    unsigned slot=m->payload.snapshot_request.slot;
    return slot<ZT_MAX_PLAYERS && (reset_relay.mask&(1u<<slot)) &&
        mac_equal(&m->header.origin,&reset_relay.macs[slot]) &&
        (!m->header.hops || !reset_relay.complete_roster || reset_relay_member(&m->direct_source));
}
static bool authoritative(uint8_t t)
{
    return t == ZT_PKT_COMMAND || t == ZT_PKT_HOST_STATE || t == ZT_PKT_ROSTER_PAGE ||
        t == ZT_PKT_WATERMARKS || t == ZT_PKT_JOIN_RESULT || t == ZT_PKT_CLOSE_RECEIPTS || t == ZT_PKT_EVENT_DECISIONS;
}
static bool clock_frame(const zt_domain_message_t *m)
{
    return m->header.type == ZT_PKT_HOST_STATE || (m->header.type == ZT_PKT_COMMAND && m->payload.command.kind == ZT_CMD_START_ROUND);
}
static bool seen_envelope(const zt_wire_header_t *h, uint32_t now, bool insert, bool *duplicate)
{
    int free_index = -1;
    if (duplicate) *duplicate = false;
    for (unsigned i = 0; i < ZT_DEDUPE_CAPACITY; ++i) {
        dedupe_t *d = &dedupe[i];
        if (d->used && (int32_t)(now - d->expires_ms) >= 0) d->used = 0;
        if (!d->used) { if (free_index < 0) free_index = i; continue; }
        if (d->boot == h->origin_boot_nonce && d->seq == h->packet_seq && mac_equal(&d->origin, &h->origin)) {
            if (duplicate) *duplicate = true;
            return true;
        }
    }
    if (free_index < 0) return true;
    if (!insert) return false;
    dedupe[free_index] = (dedupe_t){.boot = h->origin_boot_nonce, .seq = h->packet_seq,
        .expires_ms = now + ZT_DEDUPE_LIFETIME_MS, .origin = h->origin, .used = 1};
    return false;
}
static int cache_find(zt_round_id_t round, uint8_t slot, uint16_t seq)
{
    for (unsigned i = 0; i < cache_count; ++i)
        if (cache[i].round == round && cache[i].body[0] == slot &&
            ((uint16_t)cache[i].body[1] | (uint16_t)cache[i].body[2] << 8) == seq) return i;
    return -1;
}
static zt_err_t cache_event(zt_round_id_t round, const zt_wire_event_t *event, uint8_t flags, uint64_t now)
{
    if (!round_find(round)) return ZT_ERR_INVALID_STATE;
    uint8_t body[30]; size_t n;
    zt_err_t z = zt_wire_encode_event(event, body, sizeof(body), &n);
    if (z != ZT_OK) return z;
    int old = cache_find(round, event->victim_slot, event->event_seq);
    if (old >= 0) {
        if (memcmp(body, cache[old].body, sizeof(body))) return ZT_ERR_CONFLICT;
        cache[old].flags |= flags;
        return ZT_OK;
    }
    unsigned index = cache_count;
    if (cache_count == ZT_CACHE_CAPACITY) {
        if (!flags) return ZT_ERR_NO_SPACE;
        for (unsigned i = 0; i < cache_count; ++i) if (!cache[i].flags) { index = i; break; }
        if (index == cache_count) return ZT_ERR_NO_SPACE;
    } else ++cache_count;
    cache_t *c = &cache[index];
    c->round = round; memcpy(c->body, body, sizeof(body)); c->flags = flags; c->retry_step = 0;
    replay_due[index] = (uint32_t)(now / 1000) + jitter(700, 20);
    if (round == beacon_round)
        for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i) if (peers[i].seen) peers[i].changed = 1;
    return ZT_OK;
}
static size_t cache_keys(zt_round_id_t round, zt_wire_cache_key_t *keys)
{
    size_t n = 0;
    for (unsigned i = 0; i < cache_count; ++i) if (cache[i].round == round) {
        zt_wire_cache_key_t k = {cache[i].body[0], (uint16_t)cache[i].body[1] | (uint16_t)cache[i].body[2] << 8};
        size_t j = n;
        while (j && (keys[j-1].origin_slot > k.origin_slot ||
            (keys[j-1].origin_slot == k.origin_slot && keys[j-1].event_seq > k.event_seq))) { keys[j] = keys[j-1]; --j; }
        keys[j] = k; ++n;
    }
    return n;
}
static zt_err_t assemble_roster(const zt_domain_message_t *m)
{
    const zt_wire_roster_page_t *p = &m->payload.roster_page;
    round_t *r = round_add(m->header.round_id);
    if (!r) return ZT_ERR_NO_SPACE;
    if (r->complete && r->hash != p->roster_hash) return ZT_ERR_CONFLICT;
    if (r->complete && p->snapshot_rev < r->revision) return ZT_ERR_STALE;
    if (roster_assembly.round != r->id || roster_assembly.rev != p->snapshot_rev || roster_assembly.hash != p->roster_hash) {
        memset(&roster_assembly, 0, sizeof(roster_assembly));
        roster_assembly.round = r->id; roster_assembly.rev = p->snapshot_rev;
        roster_assembly.hash = p->roster_hash; roster_assembly.pages = p->page_count;
    }
    if (roster_assembly.pages != p->page_count) return ZT_ERR_PROTOCOL;
    for (unsigned i = 0; i < p->entry_count; ++i) roster_assembly.entries[p->page_index * 8 + i] = p->entries[i];
    roster_assembly.counts[p->page_index] = p->entry_count;
    roster_assembly.seen |= 1u << p->page_index;
    if (roster_assembly.seen != (1u << p->page_count) - 1u) return ZT_OK;
    zt_wire_roster_entry_t assembled[ZT_MAX_PLAYERS];
    size_t count = 0;
    for (unsigned page = 0; page < p->page_count; ++page) {
        if (count + roster_assembly.counts[page] > ZT_MAX_PLAYERS) return ZT_ERR_PROTOCOL;
        for (unsigned i = 0; i < roster_assembly.counts[page]; ++i)
            assembled[count++] = roster_assembly.entries[page * 8 + i];
    }
    uint64_t hash;
    if (count < 2 || zt_wire_roster_hash(assembled, count, &hash) != ZT_OK || hash != p->roster_hash) return ZT_ERR_PROTOCOL;
    uint32_t mask = 0;
    for (unsigned i = 0; i < count; ++i) {
        const zt_wire_roster_entry_t *e = &assembled[i];
        for (unsigned j = 0; j < i; ++j) if (mac_equal(&e->mac, &assembled[j].mac)) return ZT_ERR_PROTOCOL;
        mask |= 1u << e->slot;
    }
    const zt_radio_config_t *cfg = zt_radio_configuration();
    for (unsigned i = 0; i < count; ++i) {
        zt_wire_roster_entry_t e = assembled[i];
        r->roster[e.slot] = e;
        if (cfg && mac_equal(&e.mac, &cfg->self_mac)) r->self_slot = e.slot;
    }
    r->mask = mask; r->count = count; r->hash = hash; r->revision = p->snapshot_rev; r->complete = 1;
    return ZT_OK;
}

static zt_err_t transmit(zt_round_id_t round, uint8_t type, const zt_wire_payload_t *payload,
                         zt_tx_priority_t priority, uint64_t now)
{
    const zt_radio_config_t *cfg = zt_radio_configuration();
    if (!cfg) return ZT_ERR_INVALID_STATE;
    if (packet_seq == UINT32_MAX) return ZT_ERR_OVERFLOW;
    if (authoritative(type) && (!cfg->is_host || !mac_equal(&cfg->host_mac, &cfg->self_mac))) return ZT_ERR_AUTH;
    uint8_t body[ZT_MAX_PAYLOAD]; size_t n, total;
    zt_err_t z = zt_radio_payload_encode(type, payload, body, sizeof(body), &n);
    if (z != ZT_OK) return z;
    zt_wire_header_t h = {.magic = ZT_WIRE_MAGIC, .protocol_version = 1, .type = type,
        .payload_len = n, .game_id = cfg->game_id, .round_id = round,
        .origin = cfg->self_mac, .origin_boot_nonce = zt_radio_boot_nonce(), .packet_seq = ++packet_seq};
    if (zt_radio_flood_type(type)) { h.flags = ZT_WIRE_FLAG_RELAY_CAPABLE; h.ttl_remaining = ZT_FLOOD_TTL; }
    if (type == ZT_PKT_TAG_REQUEST || type == ZT_PKT_TAG_RESULT) priority = ZT_TX_PRIO_DIRECT_TAG;
    else if (priority == ZT_TX_PRIO_DIRECT_TAG) priority = ZT_TX_PRIO_EVENT_CONTROL;
    zt_tx_frame_t frame = {.priority = priority, .request_id = h.packet_seq, .not_before_us = now};
    if (type == ZT_PKT_TAG_REQUEST || type == ZT_PKT_TAG_RESULT) frame.bucket = ZT_BUCKET_CRITICAL;
    else if (type == ZT_PKT_BEACON) frame.bucket = ZT_BUCKET_BEACON;
    else if (type >= ZT_PKT_CACHE_PAGE && type <= ZT_PKT_TIME_REPLY) frame.bucket = ZT_BUCKET_REPAIR;
    else frame.bucket = ZT_BUCKET_CONTROL;
    if (type == ZT_PKT_TAG_REQUEST) frame.expires_us = now + ZT_TAG_CONFIRM_WAIT_MS * 1000;
    if (type == ZT_PKT_TAG_RESULT) frame.expires_us = now + ZT_TAG_OUTCOME_LIFETIME_MS * 1000;
    if (type == ZT_PKT_BEACON) frame.expires_us = now + 500000;
    if (type == ZT_PKT_HOST_STATE || (type == ZT_PKT_COMMAND && payload->command.kind == ZT_CMD_START_ROUND)) frame.expires_us = now + 200000;
    if (type == ZT_PKT_TIME_QUERY || type == ZT_PKT_TIME_REPLY) frame.expires_us = now + 200000;
    z = zt_wire_encode_envelope(&h, body, n, cfg->group_key, frame.data, sizeof(frame.data), &total);
    if (z != ZT_OK) return z;
    frame.len = total;
    z = zt_radio_submit(&frame);
    if (z == ZT_OK && zt_radio_flood_type(type)) seen_envelope(&h, now / 1000, true, NULL);
    return z;
}

static zt_err_t forward(const zt_domain_message_t *m, uint64_t now)
{
    if (!zt_radio_flood_type(m->header.type) || !m->header.ttl_remaining) return ZT_OK;
    const zt_radio_config_t *cfg = zt_radio_configuration();
    zt_wire_header_t h = m->header;
    --h.ttl_remaining; ++h.hops;
    uint64_t age = (uint64_t)h.age_ms + (now - m->rx_us + 999) / 1000;
    if (age > UINT32_MAX || (clock_frame(m) && age > ZT_CLOCK_AGE_MAX_MS)) return ZT_ERR_STALE;
    h.age_ms = age;
    uint8_t body[ZT_MAX_PAYLOAD]; size_t n, total;
    zt_err_t z = zt_radio_payload_encode(h.type, &m->payload, body, sizeof(body), &n);
    if (z != ZT_OK) return z;
    zt_tx_frame_t frame = {.priority = ZT_TX_PRIO_EVENT_CONTROL, .bucket = ZT_BUCKET_CONTROL,
        .not_before_us = now + (30 + esp_random() % 91) * 1000};
    if (clock_frame(m)) { frame.priority = ZT_TX_PRIO_CLOCK_REPAIR; frame.expires_us = m->rx_us + 200000; }
    z = zt_wire_encode_envelope(&h, body, n, cfg->group_key, frame.data, sizeof(frame.data), &total);
    if (z != ZT_OK) return z;
    frame.len = total;
    return zt_radio_submit(&frame);
}

/* Checks cheap identity/round/roster constraints before computing an HMAC.
 * The group authentication is still mandatory before acting on any field. */
static zt_err_t admissible(const zt_domain_message_t *m, const round_t *r)
{
    const zt_radio_config_t *cfg = zt_radio_configuration();
    const zt_wire_header_t *h = &m->header;
    if (!cfg || h->game_id != cfg->game_id) return ZT_ERR_AUTH;
    if (authoritative(h->type) && !mac_equal(&h->origin, &cfg->host_mac)) return ZT_ERR_AUTH;
    if (h->hops == 0 && !mac_equal(&h->origin, &m->direct_source)) return ZT_ERR_AUTH;
    if (clock_frame(m) && h->age_ms > ZT_CLOCK_AGE_MAX_MS) return ZT_ERR_STALE;
    bool reset_command = h->type == ZT_PKT_COMMAND && m->payload.command.kind == ZT_CMD_RESET_GAME;
    /* A cleared intermediary still relays only receipts for a RESET it saw
     * authorized by the host, with the exact target identity and command seq. */
    if (h->type == ZT_PKT_COMMAND_RECEIPT && reset_receipt_matches(m)) return ZT_OK;
    /* Snapshot pulls from the latest retired roster are cleanup requests only.
     * They may cross a new round's channel lock, but never restore old play. */
    if (reset_snapshot_matches(m)) return ZT_OK;
    if (h->round_id && h->round_id == reset_round) {
        if (reset_command) {
            if (reset_relay.round == h->round_id && reset_relay.complete_roster && h->hops &&
                !reset_relay_member(&m->direct_source)) return ZT_ERR_AUTH;
            return ZT_OK;
        }
        /* The host may still be resetting other badges. Its direct beacon can
         * keep lobby discovery on the receipt channel without restoring play. */
        if (h->type == ZT_PKT_BEACON && mac_equal(&h->origin, &cfg->host_mac)) return ZT_OK;
        return ZT_ERR_STALE;
    }
    zt_channel_status_t channel_status;
    if (zt_channel_get_status(&channel_status) == ZT_OK && channel_status.state == ZT_CHANNEL_LOCKED &&
        h->round_id != beacon_round && h->type != ZT_PKT_JOIN && h->type != ZT_PKT_JOIN_RESULT && !reset_command) {
        /* A registered badge missing its first frozen roster still knows only
         * round zero. Let its authenticated discovery request reach the host;
         * game admission verifies its MAC against the frozen roster there. */
        bool admission_request = h->type == ZT_PKT_SNAPSHOT_REQUEST && !h->round_id &&
            m->payload.snapshot_request.slot == ZT_SLOT_INVALID;
        bool retained = r && (h->type == ZT_PKT_EVENT || h->type == ZT_PKT_EVENT_COPY ||
            h->type == ZT_PKT_CACHE_PAGE || h->type == ZT_PKT_WANT_EVENTS || h->type == ZT_PKT_WATERMARKS ||
            h->type == ZT_PKT_EVENT_DECISIONS || h->type == ZT_PKT_DECISION_RECEIPT ||
            h->type == ZT_PKT_ROUND_CLOSED || h->type == ZT_PKT_CLOSE_RECEIPTS || h->type == ZT_PKT_SNAPSHOT_REQUEST);
        if (!retained && !admission_request) return ZT_ERR_STALE;
    }
    if (h->type == ZT_PKT_JOIN) {
        return mac_equal(&m->payload.join.requested_badge_mac, &h->origin) ? ZT_OK : ZT_ERR_AUTH;
    }
    if (r && r->complete && h->hops && origin_slot(r, &m->direct_source) < 0) return ZT_ERR_AUTH;
    if (h->type == ZT_PKT_JOIN_RESULT || h->type == ZT_PKT_ROSTER_PAGE || h->type == ZT_PKT_HOST_STATE || h->type == ZT_PKT_COMMAND) return ZT_OK;
    if (h->type == ZT_PKT_BEACON && h->round_id == 0 && m->payload.beacon.phase == ZT_PHASE_LOBBY) return ZT_OK;
    if (h->type == ZT_PKT_SNAPSHOT_REQUEST && m->payload.snapshot_request.slot == ZT_SLOT_INVALID) return ZT_OK;
    if (!r || !r->complete) return ZT_ERR_STALE;
    int sender = origin_slot(r, &h->origin);
    if (sender < 0 && !authoritative(h->type)) return ZT_ERR_AUTH;
    switch (h->type) {
    case ZT_PKT_BEACON:
        return mapped(r, m->payload.beacon.slot, &m->direct_source) ? ZT_OK : ZT_ERR_AUTH;
    case ZT_PKT_TAG_REQUEST:
        return mapped(r, m->payload.tag_request.actor_slot, &m->direct_source) && member(r, m->payload.tag_request.victim_slot) ? ZT_OK : ZT_ERR_AUTH;
    case ZT_PKT_TAG_RESULT:
        if (!tag_request.active || tag_request.round != h->round_id ||
            !mapped(r, tag_request.victim, &m->direct_source) || m->payload.tag_result.actor_slot != r->self_slot ||
            m->payload.tag_result.request_boot != tag_request.boot || m->payload.tag_result.request_seq != tag_request.attempt) return ZT_ERR_AUTH;
        if (m->payload.tag_result.result == ZT_TAG_ACCEPTED &&
            (m->payload.tag_result.accepted_event.victim_slot != tag_request.victim ||
             m->payload.tag_result.accepted_event.actor_slot != r->self_slot ||
             m->payload.tag_result.accepted_event.request_boot != tag_request.boot ||
             m->payload.tag_result.accepted_event.request_seq != tag_request.attempt)) return ZT_ERR_AUTH;
        break;
    case ZT_PKT_EVENT:
        if (!mapped(r, m->payload.event.victim_slot, &h->origin) || !member(r, m->payload.event.actor_slot)) return ZT_ERR_AUTH;
        break;
    case ZT_PKT_EVENT_COPY:
        if (!member(r, m->payload.event_copy.event.victim_slot) || !member(r, m->payload.event_copy.event.actor_slot)) return ZT_ERR_AUTH;
        break;
    case ZT_PKT_COMMAND_RECEIPT: if (sender != m->payload.command_receipt.slot) return ZT_ERR_AUTH; break;
    case ZT_PKT_ROUND_CLOSED: if (sender != m->payload.round_closed.slot) return ZT_ERR_AUTH; break;
    case ZT_PKT_DECISION_RECEIPT: if (sender != m->payload.decision_receipt.slot) return ZT_ERR_AUTH; break;
    case ZT_PKT_SNAPSHOT_REQUEST: if (sender != m->payload.snapshot_request.slot) return ZT_ERR_AUTH; break;
    default: break;
    }
    return ZT_OK;
}

static void observe_peer(const zt_domain_message_t *m)
{
    uint8_t slot_id = m->payload.beacon.slot;
    peer_t *p = &peers[slot_id];
    uint32_t now = m->rx_us / 1000;
    if (!p->seen || now - p->last_ms >= ZT_PEER_EVICT_MS || p->boot != m->header.origin_boot_nonce) {
        memset(p, 0, sizeof(*p)); p->ewma_q8 = (int16_t)m->rssi * 256;
    } else p->ewma_q8 = (3 * (int32_t)p->ewma_q8 + 2 * (int32_t)m->rssi * 256) / 5;
    p->sample_ms[2] = p->sample_ms[1]; p->sample_ms[1] = p->sample_ms[0]; p->sample_ms[0] = now;
    p->samples = 0;
    for (unsigned i = 0; i < 3; ++i) if (p->sample_ms[i] && now - p->sample_ms[i] <= ZT_RSSI_SAMPLE_WINDOW_MS) ++p->samples;
    p->last_rssi = m->rssi; p->last_ms = now; p->seen = 1;
    p->seq = m->header.packet_seq; p->boot = m->header.origin_boot_nonce;
    if (p->digest != m->payload.beacon.cache_digest) p->changed = 1;
    p->digest = m->payload.beacon.cache_digest;
}

static void repair_receive(const zt_domain_message_t *m, round_t *r, uint64_t now)
{
    if (!r || r->self_slot == ZT_SLOT_INVALID) return;
    int sender = origin_slot(r, &m->direct_source);
    if (sender < 0) return;
    if (m->header.type == ZT_PKT_WANT_EVENTS) {
        const zt_wire_want_events_t *w = &m->payload.want_events;
        if (w->target_slot != r->self_slot) return;
        if (exchange.active && (exchange.round != r->id || (exchange.target != ZT_SLOT_ALL && exchange.target != sender))) return;
        if (exchange.copy_index < exchange.copy_count) return;
        if (!exchange.active) {
            memset(&exchange, 0, sizeof(exchange));
            exchange.active = exchange.responding = true;
            exchange.expires = now + 12000000;
            exchange.round = r->id; exchange.due = now;
        }
        if (exchange.page >= exchange.pages) exchange.responding = true;
        /* A WANT during an inventory is queued until all its pages leave. */
        exchange.target = sender;
        exchange.copy_count = w->count; exchange.copy_index = 0;
        for (unsigned i = 0; i < w->count; ++i) exchange.copies[i] = w->entries[i];
    } else if (m->header.type == ZT_PKT_CACHE_PAGE) {
        const zt_wire_cache_page_t *p = &m->payload.cache_page;
        if (p->target_slot != ZT_SLOT_ALL && p->target_slot != r->self_slot) return;
        /* These are the peer's inventory bytes, not our advertised digest.
         * Simultaneous repair must allow different digests for the two sides. */
        if (exchange.active && (exchange.round != r->id || exchange.target != sender)) return;
        if (!exchange.active) {
            if (peers[sender].last_repair_us && now - peers[sender].last_repair_us < 10000000) return;
            memset(&exchange, 0, sizeof(exchange));
            exchange.round = r->id; exchange.target = exchange.inventory_target = sender;
            exchange.key_count = cache_keys(r->id, exchange.keys);
            if (zt_wire_cache_digest(exchange.keys, exchange.key_count, &exchange.digest) != ZT_OK) return;
            exchange.pages = (exchange.key_count + 55) / 56;
            if (!exchange.pages) exchange.pages = 1;
            /* Return our inventory too, so a badge with an empty cache can
             * discover the other side's missing events in this same exchange. */
            exchange.active = true; exchange.due = now;
            exchange.expires = now + 10000000;
            peers[sender].last_repair_us = now;
            peers[sender].changed = 1;
        }
        zt_wire_payload_t want = {0};
        want.want_events.target_slot = sender;
        if (repair_request_seq == UINT16_MAX) return;
        want.want_events.request_seq = ++repair_request_seq;
        for (unsigned i = 0; i < p->count && want.want_events.count < 16; ++i) {
            zt_wire_cache_key_t k = p->entries[i];
            if (member(r, k.origin_slot) && cache_find(r->id, k.origin_slot, k.event_seq) < 0)
                want.want_events.entries[want.want_events.count++] = k;
        }
        if (want.want_events.count) transmit(r->id, ZT_PKT_WANT_EVENTS, &want, ZT_TX_PRIO_CLOCK_REPAIR, now);
    }
}

static zt_err_t deliver(zt_domain_message_t *m, uint64_t now)
{
    const zt_radio_config_t *cfg = zt_radio_configuration();
    round_t *r = round_find(m->header.round_id);
    zt_err_t z = ZT_OK;
    switch (m->header.type) {
    case ZT_PKT_ROSTER_PAGE:
        z = assemble_roster(m);
        if (z != ZT_OK) return z;
        break;
    case ZT_PKT_HOST_STATE:
        /* Zero pages is metadata only: roster storage is never touched here. */
        if ((!gateway_boot || gateway_boot == m->header.origin_boot_nonce) &&
            gateway_boot && m->payload.host_state.gateway_serial <= gateway_serial) break;
        gateway_boot = m->header.origin_boot_nonce;
        gateway_serial = m->payload.host_state.gateway_serial;
        gateway_seen = m->rx_us;
        gateway_initial_age = m->header.age_ms;
        gateway_hops = m->header.hops + 1;
        break;
    case ZT_PKT_BEACON:
        if (gateway_boot && m->payload.beacon.gateway_boot_nonce == gateway_boot &&
            m->payload.beacon.gateway_serial > gateway_serial) {
            gateway_serial = m->payload.beacon.gateway_serial;
            gateway_seen = m->rx_us;
            gateway_initial_age = (uint32_t)m->payload.beacon.gateway_age_s * 1000 + m->header.age_ms;
            gateway_hops = m->payload.beacon.gateway_hops < ZT_MAX_HOPS ? m->payload.beacon.gateway_hops + 1 : ZT_MAX_HOPS;
        }
        break;
    case ZT_PKT_EVENT:
        z = cache_event(m->header.round_id, &m->payload.event, 0, now);
        if (z == ZT_ERR_CONFLICT) return z;
        /* Saturated relay cache does not suppress the durable custodian sink. */
        break;
    case ZT_PKT_EVENT_COPY:
        z = cache_event(m->header.round_id, &m->payload.event_copy.event, 0, now);
        if (z == ZT_ERR_CONFLICT) return z;
        break;
    case ZT_PKT_CLOSE_RECEIPTS:
        if (r) r->archived |= m->payload.close_receipts.archived_bitmap;
        break;
    case ZT_PKT_WATERMARKS:
        if (r && r->hash == m->payload.watermarks.roster_hash) {
            for (unsigned i = 0; i < m->payload.watermarks.count; ++i) {
                const zt_wire_watermark_entry_t *e = &m->payload.watermarks.entries[i];
                if (member(r, e->slot) && e->server_received_contiguous > r->received[e->slot]) r->received[e->slot] = e->server_received_contiguous;
            }
        }
        break;
    case ZT_PKT_CACHE_PAGE: case ZT_PKT_WANT_EVENTS:
        repair_receive(m, r, now);
        return ZT_OK;
    case ZT_PKT_TIME_QUERY:
        if (r && m->payload.time_query.target_slot == r->self_slot) {
            zt_clock_sample_t sample;
            if (zt_clock_read(now, &sample) == ZT_OK && sample.quality == ZT_TIME_INITIALIZED &&
                sample.uncertainty_ms <= ZT_TIME_UNCERTAINTY_MAX_MS && r->id == beacon_round) {
                int sender = origin_slot(r, &m->direct_source);
                zt_wire_payload_t reply = {0};
                reply.time_reply = (zt_wire_time_reply_t){.target_slot = sender,
                    .request_nonce = m->payload.time_query.request_nonce, .sampled_elapsed_ms = sample.elapsed_ms,
                    .source_snapshot_rev = r->revision, .uncertainty_ms = sample.uncertainty_ms, .time_quality = 1};
                transmit(r->id, ZT_PKT_TIME_REPLY, &reply, ZT_TX_PRIO_CLOCK_REPAIR, now);
            }
        }
        return ZT_OK;
    case ZT_PKT_TIME_REPLY: {
        if (!r || !time_query.active || time_query.round != r->id ||
            m->payload.time_reply.target_slot != r->self_slot ||
            m->payload.time_reply.request_nonce != time_query.nonce ||
            !mapped(r, time_query.peer, &m->direct_source) || m->rx_us < time_query.sent ||
            m->rx_us - time_query.sent > ZT_PEER_TIME_RTT_MAX_MS * 1000 || m->payload.time_reply.time_quality != 1)
            return ZT_ERR_STALE;
        uint32_t rtt_ms = (uint32_t)((m->rx_us - time_query.sent + 999) / 1000);
        if (m->payload.time_reply.uncertainty_ms + (rtt_ms + 1) / 2 + 10 > ZT_TIME_UNCERTAINTY_MAX_MS) return ZT_ERR_STALE;
        /* The game is the clock's sole writer. Publish the matched raw reply;
         * its outstanding query record supplies RTT for clock_apply. */
        break;
    }
    default: break;
    }
    if (cfg && (mac_equal(&m->header.origin, &cfg->host_mac) && (uint64_t)m->header.age_ms + (now >= m->rx_us ? (now - m->rx_us) / 1000 : UINT32_MAX) <= ZT_CLOCK_AGE_MAX_MS))
        zt_radio_channel_discovered(m->channel, m->rx_us);
    else if (m->header.type == ZT_PKT_BEACON && m->header.round_id == 0 &&
        m->payload.beacon.phase == ZT_PHASE_LOBBY && m->payload.beacon.gateway_age_s < 10)
        zt_radio_channel_discovered(m->channel, m->rx_us);
    z = domain_sink ? domain_sink(m, domain_context) : ZT_ERR_INVALID_STATE;
    if (z == ZT_ERR_NO_SPACE) z = ZT_ERR_BUSY;
    if (z == ZT_OK && m->header.type == ZT_PKT_COMMAND)
        remember_reset_command(m->header.round_id, &m->payload.command);
    if (z == ZT_OK && m->header.type == ZT_PKT_TIME_REPLY) time_query.active = false;
    if (z == ZT_OK && m->header.type == ZT_PKT_BEACON && m->header.round_id && r &&
        r->complete && m->header.round_id == beacon_round) observe_peer(m);
    return z;
}

zt_err_t zt_mesh_init(zt_domain_sink_t sink, void *context)
{
    if (!sink) return ZT_ERR_INVALID_ARG;
    if (mutex) return ZT_ERR_INVALID_STATE;
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    if (!mutex) return ZT_ERR_NO_SPACE;
    domain_sink = sink; domain_context = context;
    return ZT_OK;
}

zt_err_t zt_mesh_receive(const zt_rx_frame_t *frame)
{
    if (!frame || !mutex) return ZT_ERR_INVALID_ARG;
    if (frame->driver_generation != zt_radio_generation()) return ZT_ERR_STALE;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    zt_domain_message_t m = {.direct_source = frame->src_mac, .rssi = frame->rssi,
        .channel = frame->channel, .rx_us = frame->rx_us, .driver_generation = frame->driver_generation};
    uint64_t now = now_us();
    portENTER_CRITICAL(&invalidation_guard);
    uint64_t invalidated = invalidated_us;
    bool clear = invalidate_pending;
    invalidate_pending = false;
    portEXIT_CRITICAL(&invalidation_guard);
    if (clear) { memset(peers, 0, sizeof(peers)); time_query.active = false; }
    if (frame->rx_us < invalidated || frame->rx_us > now) { xSemaphoreGive(mutex); return ZT_ERR_STALE; }
    zt_err_t z = zt_radio_wire_peek(frame->data, frame->len, &m.header);
    if (z == ZT_OK) z = zt_radio_payload_decode(m.header.type, frame->data + 48, m.header.payload_len, &m.payload);
    if (z == ZT_OK) z = admissible(&m, round_find(m.header.round_id));
    /* Envelope/identity admission errors precede group HMAC verification.
     * Neither HMAC failures nor duplicate/stale beacon suppressions enter here. */
    if (z != ZT_OK) count_rejection(&invalid_frames);
    if (z == ZT_OK) {
        uint8_t tag[16];
        const zt_radio_config_t *cfg = zt_radio_configuration();
        z = zt_wire_hmac(cfg->group_key, frame->data, frame->len - 16, tag);
        if (z == ZT_OK) {
            z = zt_wire_hmac_verify(tag, frame->data + frame->len - 16);
            if (z == ZT_ERR_AUTH) count_rejection(&auth_failures);
        }
    }
    bool duplicate = false;
    if (z == ZT_OK && zt_radio_flood_type(m.header.type) && seen_envelope(&m.header, now / 1000, false, &duplicate)) {
        if (duplicate) count_rejection(&dedupe_hits);
        z = duplicate ? ZT_ERR_STALE : ZT_ERR_NO_SPACE;
    }
    bool reset_request=z==ZT_OK && reset_snapshot_matches(&m);
    /* Update this bound only after HMAC verification and successful delivery,
     * so unauthenticated traffic cannot suppress a returning badge's repair. */
    if (reset_request && now<reset_relay.request_due[m.payload.snapshot_request.slot]) z=ZT_ERR_BUSY;
    if (z == ZT_OK && m.header.type == ZT_PKT_BEACON && m.header.round_id == beacon_round && m.payload.beacon.slot < 20) {
        peer_t *p = &peers[m.payload.beacon.slot];
        if (p->seen && p->boot == m.header.origin_boot_nonce && m.header.packet_seq <= p->seq) z = ZT_ERR_STALE;
    }
    if (z == ZT_OK) {
        z = deliver(&m, now);
        if (z == ZT_OK) {
            if (reset_request) reset_relay.request_due[m.payload.snapshot_request.slot]=
                now+ZT_SNAPSHOT_REQUEST_INTERVAL_MS*1000ULL;
            if (zt_radio_flood_type(m.header.type)) seen_envelope(&m.header, now / 1000, true, NULL);
            if (!m.header.hops && mac_equal(&m.header.origin, &m.direct_source))
                atomic_store_explicit(&diagnostic_last_rssi, m.rssi, memory_order_relaxed);
            forward(&m, now);
        }
    }
    publish_diagnostics(now);
    xSemaphoreGive(mutex);
    return z;
}

zt_err_t zt_mesh_publish(const zt_domain_message_t *message)
{
    if (!message || !mutex) return ZT_ERR_INVALID_ARG;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    /* This entry point takes an already-validated domain object, not RX bytes. */
    zt_domain_message_t copy = *message;
    zt_err_t z = deliver(&copy, now_us());
    publish_diagnostics(now_us());
    xSemaphoreGive(mutex);
    return z;
}

zt_err_t zt_mesh_submit(zt_round_id_t round_id, zt_pkt_type_t type, const zt_wire_payload_t *payload, zt_tx_priority_t priority)
{
    if (!payload || !mutex || priority < ZT_TX_PRIO_DIRECT_TAG || priority > ZT_TX_PRIO_COSMETIC) return ZT_ERR_INVALID_ARG;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    uint64_t now = now_us();
    zt_err_t z = ZT_OK;
    uint8_t validate[ZT_MAX_PAYLOAD]; size_t n;
    z = zt_radio_payload_encode(type, payload, validate, sizeof(validate), &n);
    if (z != ZT_OK) goto done;
    if (type == ZT_PKT_JOIN && last_join && now - last_join < 3000000) { z = ZT_ERR_BUSY; goto done; }
    if (type == ZT_PKT_SNAPSHOT_REQUEST && last_snapshot &&
        now - last_snapshot < (round_id ? ZT_SNAPSHOT_REQUEST_INTERVAL_MS : 1000) * 1000ULL) { z = ZT_ERR_BUSY; goto done; }
    if (type == ZT_PKT_BEACON) {
        beacon_template = payload->beacon; beacon_round = round_id; have_beacon = true;
        beacon_source_us = now;
        if (beacon_due && now < beacon_due) goto done;
    }
    if (type == ZT_PKT_EVENT) {
        z = cache_event(round_id, &payload->event, CACHE_OWN, now);
        if (z != ZT_OK) goto done;
    }
    z = transmit(round_id, type, payload, priority, now);
    if (z != ZT_OK) goto done;
    if (type == ZT_PKT_COMMAND) remember_reset_command(round_id, &payload->command);
    if (type == ZT_PKT_JOIN) last_join = now;
    if (type == ZT_PKT_SNAPSHOT_REQUEST) last_snapshot = now;
    if (type == ZT_PKT_BEACON) beacon_due = now + (400 + esp_random() % 201) * 1000;
    if (type == ZT_PKT_HOST_STATE) {
        host_template = payload->host_state; host_round = round_id; have_host = true;
        host_source_us = now;
        clock_due = now + ZT_HOST_CLOCK_PERIOD_MS * 1000;
        gateway_serial = payload->host_state.gateway_serial; gateway_boot = zt_radio_boot_nonce();
        gateway_seen = now; gateway_initial_age = 0; gateway_hops = 0;
    }
    if (type == ZT_PKT_ROSTER_PAGE) {
        zt_domain_message_t m = {.header.round_id = round_id}; m.payload = *payload;
        z = assemble_roster(&m);
    }
    if (type == ZT_PKT_TIME_QUERY) {
        time_query.active = true; time_query.round = round_id; time_query.peer = payload->time_query.target_slot;
        time_query.nonce = payload->time_query.request_nonce; time_query.sent = now;
    }
    if (type == ZT_PKT_TAG_REQUEST) {
        tag_request.active = true; tag_request.round = round_id; tag_request.victim = payload->tag_request.victim_slot;
        tag_request.attempt = payload->tag_request.attempt_seq; tag_request.boot = zt_radio_boot_nonce(); tag_request.sent = now;
    }
done:
    publish_diagnostics(now);
    xSemaphoreGive(mutex);
    return z;
}

void zt_radio_mesh_invalidate(uint32_t generation)
{
    portENTER_CRITICAL(&invalidation_guard);
    invalidated_us = now_us();
    invalidate_pending = true;
    portEXIT_CRITICAL(&invalidation_guard);
    zt_peers_invalidate(generation);
}

static void service_repair(uint64_t now)
{
    if (exchange.active && now >= exchange.expires) memset(&exchange, 0, sizeof(exchange));
    if (!exchange.active) {
        round_t *r = round_find(beacon_round);
        if (!r || !r->complete) return;
        for (unsigned i = 0; i < 20; ++i) {
            uint8_t s = repair_cursor++ % 20;
            peer_t *p = &peers[s];
            if (!p->seen || (uint32_t)(now / 1000) - p->last_ms >= ZT_PEER_STALE_MS ||
                !p->changed || (p->last_repair_us && now - p->last_repair_us < 10000000)) continue;
            exchange.key_count = cache_keys(r->id, exchange.keys);
            if (zt_wire_cache_digest(exchange.keys, exchange.key_count, &exchange.digest) != ZT_OK) return;
            if (exchange.digest == p->digest) { p->changed = 0; continue; }
            exchange.active = true; exchange.responding = false; exchange.round = r->id;
            exchange.target = exchange.inventory_target = s; exchange.page = 0; exchange.pages = (exchange.key_count + 55) / 56;
            if (!exchange.pages) exchange.pages = 1;
            exchange.due = now; exchange.expires = now + 10000000;
            /* Keep a mismatch eligible after timeout/loss. Only observing
             * matching inventories above clears the repair hint. */
            p->last_repair_us = now;
            break;
        }
    }
    if (!exchange.active || now < exchange.due) return;
    zt_wire_payload_t payload = {0};
    if (exchange.responding) {
        if (exchange.copy_index >= exchange.copy_count) return;
        zt_wire_cache_key_t key = exchange.copies[exchange.copy_index];
        int i = cache_find(exchange.round, key.origin_slot, key.event_seq);
        if (i < 0) { ++exchange.copy_index; return; }
        payload.event_copy.target_slot = exchange.target;
        if (zt_wire_decode_event(cache[i].body, 30, &payload.event_copy.event) != ZT_OK) return;
        if (transmit(exchange.round, ZT_PKT_EVENT_COPY, &payload, ZT_TX_PRIO_REPLAY, now) == ZT_OK) {
            ++exchange.copy_index; exchange.due = now + 500000;
        }
    } else if (exchange.page < exchange.pages) {
        payload.cache_page.target_slot = exchange.inventory_target; payload.cache_page.cache_digest = exchange.digest;
        payload.cache_page.page_index = exchange.page; payload.cache_page.page_count = exchange.pages;
        unsigned start = exchange.page * 56;
        unsigned n = exchange.key_count - start;
        if (n > 56) n = 56;
        payload.cache_page.count = n;
        for (unsigned i = 0; i < n; ++i) payload.cache_page.entries[i] = exchange.keys[start+i];
        if (transmit(exchange.round, ZT_PKT_CACHE_PAGE, &payload, ZT_TX_PRIO_CLOCK_REPAIR, now) == ZT_OK) {
            ++exchange.page; exchange.due = now + ZT_CACHE_PAGE_SPACING_MS * 1000;
            if (exchange.page == exchange.pages && exchange.copy_index < exchange.copy_count)
                exchange.responding = true;
        }
    }
}

zt_err_t zt_mesh_service(uint64_t now)
{
    if (!mutex) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    const zt_radio_config_t *cfg = zt_radio_configuration();
    if (!cfg) { xSemaphoreGive(mutex); return ZT_ERR_INVALID_STATE; }
    portENTER_CRITICAL(&invalidation_guard);
    bool clear = invalidate_pending;
    invalidate_pending = false;
    portEXIT_CRITICAL(&invalidation_guard);
    if (clear) { memset(peers, 0, sizeof(peers)); time_query.active = false; }
    /* At most one journal item per service call; alternate retained rounds so
     * a temporarily busy/full old-round feed cannot starve active restoration. */
    for (unsigned pass = 0; pass < ZT_RETAINED_ROUND_CAPACITY; ++pass) {
        unsigned i = (restore_turn + pass) % ZT_RETAINED_ROUND_CAPACITY;
        round_t *r = &rounds[i];
        if (!r->restoring) continue;
        restore_turn = (i + 1) % ZT_RETAINED_ROUND_CAPACITY;
        if (r->self_slot == ZT_SLOT_INVALID) {
            int self = origin_slot(r, &cfg->self_mac);
            if (self >= 0) r->self_slot = self;
        }
        zt_gateway_event_t event; size_t count = 0; uint16_t next = r->restore_cursor;
        zt_err_t z = zt_game_event_feed(r->id, r->restore_cursor, &event, 1, &count, &next);
        if (z == ZT_OK && count) {
            if (event.id.round_id != r->id || event.id.victim_slot != event.body.victim_slot ||
                event.id.event_seq != event.body.event_seq || next <= r->restore_cursor || next > ZT_JOURNAL_CAPACITY)
                z = ZT_ERR_PROTOCOL;
            else z = cache_event(r->id, &event.body,
                CACHE_RETAINED | (event.body.victim_slot == r->self_slot ? CACHE_OWN : 0), now);
        }
        if (z == ZT_OK) {
            r->restoring = count && next < ZT_JOURNAL_CAPACITY;
            r->restore_cursor = next;
        }
        break;
    }
    if (have_beacon && now >= beacon_due) {
        zt_wire_payload_t payload = {.beacon = beacon_template};
        if (now - beacon_source_us >= ZT_SERVER_STATUS_MAX_AGE_MS * 1000ULL)
            payload.beacon.flags &= ~ZT_BEACON_FLAG_SERVER_CONNECTED;
        zt_wire_cache_key_t keys[128];
        size_t n = cache_keys(beacon_round, keys);
        payload.beacon.cache_count = n;
        zt_wire_cache_digest(keys, n, &payload.beacon.cache_digest);
        zt_clock_sample_t sample;
        if (zt_clock_read(now, &sample) == ZT_OK && sample.quality == ZT_TIME_INITIALIZED && sample.uncertainty_ms <= 2000) {
            payload.beacon.elapsed_ms = sample.elapsed_ms; payload.beacon.uncertainty_ms = sample.uncertainty_ms;
            payload.beacon.time_quality = 1;
        } else { payload.beacon.elapsed_ms = ZT_INT32_UNKNOWN; payload.beacon.time_quality = 0; }
        payload.beacon.gateway_serial = gateway_serial; payload.beacon.gateway_boot_nonce = gateway_boot;
        uint64_t age = gateway_seen ? (now - gateway_seen) / 1000 + gateway_initial_age : UINT32_MAX;
        payload.beacon.gateway_age_s = age / 1000 > 255 ? 255 : age / 1000;
        payload.beacon.gateway_hops = gateway_hops;
        transmit(beacon_round, ZT_PKT_BEACON, &payload, ZT_TX_PRIO_COSMETIC, now);
        beacon_due = now + (400 + esp_random() % 201) * 1000;
    }
    if (have_host && cfg->is_host && now >= clock_due && gateway_serial != UINT32_MAX) {
        zt_clock_sample_t sample;
        if (zt_clock_read(now, &sample) == ZT_OK && sample.quality == ZT_TIME_INITIALIZED && sample.uncertainty_ms <= 2000) {
            zt_wire_payload_t payload = {.host_state = host_template};
            if (now - host_source_us >= ZT_SERVER_STATUS_MAX_AGE_MS * 1000ULL)
                payload.host_state.gateway_flags &= ~ZT_HOST_STATE_FLAG_SERVER_CONNECTED;
            payload.host_state.page_count = payload.host_state.page_index = payload.host_state.entry_count = 0;
            /* END_ROUND freezes the template at its effective elapsed time.
             * Keep that terminal timer while refreshing the gateway heartbeat. */
            if (payload.host_state.phase < ZT_PHASE_EXPIRED_PENDING_SYNC) {
                payload.host_state.host_elapsed_ms = sample.elapsed_ms;
                payload.host_state.uncertainty_ms = sample.uncertainty_ms;
                payload.host_state.remaining_ms = sample.elapsed_ms < 0 ? 600000 : (sample.elapsed_ms >= 600000 ? 0 : 600000 - sample.elapsed_ms);
            }
            payload.host_state.gateway_serial = gateway_serial + 1;
            if (transmit(host_round, ZT_PKT_HOST_STATE, &payload, ZT_TX_PRIO_CLOCK_REPAIR, now) == ZT_OK) {
                ++gateway_serial; gateway_seen = now;
            }
        }
        clock_due = now + 2000000;
    }
    if (now >= replay_service_due) {
        for (unsigned i = 0; i < cache_count; ++i) {
            cache_t *c = &cache[i];
            round_t *r = round_find(c->round);
            uint16_t seq = (uint16_t)c->body[1] | (uint16_t)c->body[2] << 8;
            if (!(c->flags & CACHE_OWN) || (r && r->received[c->body[0]] >= seq) || (int32_t)((uint32_t)(now / 1000) - replay_due[i]) < 0) continue;
            zt_wire_payload_t payload;
            if (zt_wire_decode_event(c->body, 30, &payload.event) == ZT_OK &&
                transmit(c->round, ZT_PKT_EVENT, &payload, ZT_TX_PRIO_REPLAY, now) == ZT_OK) {
                replay_due[i] = now / 1000 + jitter(c->retry_step == 0 ? 1000 : 30000, 20);
                if (c->retry_step < 2) ++c->retry_step;
            }
            replay_service_due = now + 1000000;
            break;
        }
    }
    for (unsigned i = 0; i < 20; ++i) if (peers[i].seen && (uint32_t)(now / 1000) - peers[i].last_ms >= 10000)
        memset(&peers[i], 0, sizeof(peers[i]));
    if (time_query.active && now - time_query.sent > 250000) time_query.active = false;
    if (tag_request.active && now - tag_request.sent > 10000000) tag_request.active = false;
    /* Give a due old-round inventory the next free exchange before starting
     * another current-round repair. Cache seeding never selects beacon_round. */
    if (exchange.active && now >= exchange.expires) memset(&exchange, 0, sizeof(exchange));
    if (!exchange.active && now >= old_inventory_due) {
        for (unsigned i = 0; i < 2; ++i) if (rounds[i].id && rounds[i].id != beacon_round && !rounds[i].restoring) {
            memset(&exchange, 0, sizeof(exchange));
            exchange.round = rounds[i].id; exchange.target = exchange.inventory_target = ZT_SLOT_ALL;
            exchange.key_count = cache_keys(exchange.round, exchange.keys);
            zt_wire_cache_digest(exchange.keys, exchange.key_count, &exchange.digest);
            exchange.pages = (exchange.key_count + 55) / 56;
            if (!exchange.pages) exchange.pages = 1;
            exchange.active = true; exchange.expires = now + 10000000; exchange.due = now;
            old_inventory_due = now + ZT_OLD_ROUND_INVENTORY_MS * 1000;
            break;
        }
    }
    service_repair(now);
    publish_diagnostics(now);
    xSemaphoreGive(mutex);
    return ZT_OK;
}

/* Game-task entry point: restore previous first and ACTIVE LAST. This reads
 * only the checkpoint; the service loop pulls the journal one item at a time. */
zt_err_t zt_radio_mesh_restore(zt_round_id_t round)
{
    if (!round) return ZT_ERR_INVALID_ARG;
    if (!mutex) return ZT_ERR_INVALID_STATE;
    zt_checkpoint_t checkpoint;
    zt_err_t z = zt_store_load_checkpoint(round, &checkpoint);
    if (z != ZT_OK) return z;
    if (checkpoint.round_id != round || checkpoint.roster_count < 2 || checkpoint.roster_count > 20) return ZT_ERR_PROTOCOL;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    round_t *r = round_find(round);
    /* Check both a completed roster and any held page assembly before changing
     * any round, cache, cursor, beacon selection, or diagnostic publication. */
    if ((r && r->complete && r->hash != checkpoint.roster_hash) ||
        (roster_assembly.round == round && roster_assembly.pages && roster_assembly.hash != checkpoint.roster_hash)) {
        xSemaphoreGive(mutex); return ZT_ERR_CONFLICT;
    }
    uint64_t hash;
    z = zt_wire_roster_hash(checkpoint.roster, checkpoint.roster_count, &hash);
    if (z != ZT_OK || hash != checkpoint.roster_hash) { xSemaphoreGive(mutex); return ZT_ERR_PROTOCOL; }
    for (unsigned i = 0; i < checkpoint.roster_count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (mac_equal(&checkpoint.roster[i].mac, &checkpoint.roster[j].mac)) {
                xSemaphoreGive(mutex); return ZT_ERR_PROTOCOL;
            }
    if (!r) r = round_add(round);
    if (!r) { xSemaphoreGive(mutex); return ZT_ERR_NO_SPACE; }
    const zt_radio_config_t *cfg = zt_radio_configuration();
    r->mask = 0; r->self_slot = ZT_SLOT_INVALID;
    for (unsigned i = 0; i < checkpoint.roster_count; ++i) {
        zt_wire_roster_entry_t e = checkpoint.roster[i];
        r->roster[e.slot] = e; r->mask |= 1u << e.slot;
        if (cfg && mac_equal(&e.mac, &cfg->self_mac)) r->self_slot = e.slot;
    }
    r->hash = hash; r->revision = checkpoint.snapshot_rev;
    r->count = checkpoint.roster_count; r->complete = 1; r->archived |= checkpoint.cleared_bitmap;
    r->restoring = 1; r->restore_cursor = 0;
    for (unsigned i = 0; i < 20; ++i)
        if (checkpoint.received[i] > r->received[i]) r->received[i] = checkpoint.received[i];
    if (beacon_round != round) {
        /* Never stamp an earlier round's role template with the selected ID. */
        have_beacon = false;
        memset(peers, 0, sizeof(peers));
    }
    beacon_round = round;
    old_inventory_due = 0;
    publish_diagnostics(now_us());
    xSemaphoreGive(mutex);
    return ZT_OK;
}

zt_err_t zt_radio_mesh_reset(zt_round_id_t round)
{
    if (!round) return ZT_ERR_INVALID_ARG;
    if (!mutex) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return ZT_ERR_BUSY;
    zt_err_t discarded = zt_radio_discard_round(round);
    if (discarded != ZT_OK) { xSemaphoreGive(mutex); return discarded; }
    reset_round = round;
    remember_reset_roster(round);
    for (unsigned i = 0; i < ZT_RETAINED_ROUND_CAPACITY; ++i)
        if (rounds[i].id == round) memset(&rounds[i], 0, sizeof(rounds[i]));
    for (unsigned i = 0; i < cache_count;) {
        if (cache[i].round != round) { ++i; continue; }
        --cache_count; cache[i] = cache[cache_count]; replay_due[i] = replay_due[cache_count];
    }
    if (roster_assembly.round == round) memset(&roster_assembly, 0, sizeof(roster_assembly));
    if (exchange.round == round) memset(&exchange, 0, sizeof(exchange));
    if (time_query.round == round) time_query.active = false;
    if (tag_request.round == round) tag_request.active = false;
    if (beacon_round == round) { beacon_round = 0; have_beacon = false; }
    if (host_round == round) { host_round = 0; have_host = false; }
    memset(peers, 0, sizeof(peers));
    gateway_boot = gateway_seen = 0; gateway_serial = gateway_initial_age = 0;
    last_join = last_snapshot = 0;
    publish_diagnostics(now_us());
    xSemaphoreGive(mutex);
    return ZT_OK;
}
