#include "zt_wire.h"
#include <stdbool.h>
#include <string.h>
#include "mbedtls/sha256.h"

/* Cursor operates on scalar fields, never on a structure's object image. */
typedef struct { const uint8_t *in; uint8_t *out; size_t size, pos; zt_err_t error; } cursor_t;
static uint64_t scalar(cursor_t *c, uint64_t value, unsigned bytes)
{
    if (c->error != ZT_OK) return 0;
    if (bytes > c->size - c->pos) { c->error = ZT_ERR_INVALID_LENGTH; return 0; }
    uint64_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        if (c->out) c->out[c->pos] = (uint8_t)(value >> (8 * i));
        else result |= (uint64_t)c->in[c->pos] << (8 * i);
        ++c->pos;
    }
    return c->out ? value : result;
}
static bool slot(uint8_t v) { return v < ZT_MAX_PLAYERS; }
static bool slot_or_all(uint8_t v) { return slot(v) || v == ZT_SLOT_ALL; }
static bool role(uint8_t v) { return v <= ZT_ROLE_ZOMBIE || v == ZT_ROLE_UNKNOWN; }
static bool channel(uint8_t v) { return v >= 1 && v <= 11; }
static bool pages(uint8_t i, uint8_t n, uint8_t max) { return n && n <= max && i < n; }
static bool printable(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i] < 0x20 || p[i] > 0x7e) return false;
    return true;
}
static bool name_valid(const uint8_t *p, uint8_t n)
{
    if (n < 1 || n > 12 || !printable(p, n)) return false;
    for (unsigned i = n; i < 12; ++i) if (p[i]) return false;
    return true;
}
static bool cause(uint8_t r, uint8_t s, uint16_t seq)
{
    if (r == ZT_ROLE_HUMAN) return s == ZT_SLOT_INVALID && seq == 0;
    return r == ZT_ROLE_UNKNOWN ? slot_or_all(s) : slot(s);
}
static bool command_valid(const zt_wire_command_t *v);

static void fields_event(cursor_t *c, zt_wire_event_t *v)
{
    v->victim_slot = (uint8_t)scalar(c, v->victim_slot, 1);
    v->event_seq = (uint16_t)scalar(c, v->event_seq, 2);
    v->actor_slot = (uint8_t)scalar(c, v->actor_slot, 1);
    v->actor_cause_seq = (uint16_t)scalar(c, v->actor_cause_seq, 2);
    v->actor_role_rev = (uint16_t)scalar(c, v->actor_role_rev, 2);
    v->victim_prior_role_rev = (uint16_t)scalar(c, v->victim_prior_role_rev, 2);
    v->request_boot = (zt_boot_nonce_t)scalar(c, v->request_boot, 8);
    v->request_seq = (uint32_t)scalar(c, v->request_seq, 4);
    v->occurred_elapsed_ms = (uint32_t)scalar(c, v->occurred_elapsed_ms, 4);
    v->uncertainty_ms = (uint16_t)scalar(c, v->uncertainty_ms, 2);
    v->tagger_observed_victim_rssi = (int8_t)scalar(c, v->tagger_observed_victim_rssi, 1);
    v->victim_observed_tagger_rssi = (int8_t)scalar(c, v->victim_observed_tagger_rssi, 1);
    if (c->error == ZT_OK && !(slot(v->victim_slot) && slot(v->actor_slot) && v->victim_slot != v->actor_slot && v->event_seq != 0 && v->occurred_elapsed_ms < ZT_ROUND_DURATION_MS && v->uncertainty_ms <= ZT_TIME_UNCERTAINTY_MAX_MS)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_event(const zt_wire_event_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_event_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_event(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_event(const uint8_t *buf, size_t len, zt_wire_event_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_event_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_event(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_roster_entry(cursor_t *c, zt_wire_roster_entry_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    for (unsigned i = 0; i < 6; ++i) v->mac.bytes[i] = (uint8_t)scalar(c, v->mac.bytes[i], 1);
    v->name_len = (uint8_t)scalar(c, v->name_len, 1);
    for (unsigned i = 0; i < 12; ++i) v->name[i] = (uint8_t)scalar(c, v->name[i], 1);
    if (c->error == ZT_OK && !(slot(v->slot) && name_valid(v->name, v->name_len))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_roster_entry(const zt_wire_roster_entry_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_roster_entry_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_roster_entry(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_roster_entry(const uint8_t *buf, size_t len, zt_wire_roster_entry_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_roster_entry_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_roster_entry(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_role_entry(cursor_t *c, zt_wire_role_entry_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->role = (uint8_t)scalar(c, v->role, 1);
    v->role_rev = (uint16_t)scalar(c, v->role_rev, 2);
    v->cause_slot = (uint8_t)scalar(c, v->cause_slot, 1);
    v->cause_seq = (uint16_t)scalar(c, v->cause_seq, 2);
    v->covered_seq = (uint16_t)scalar(c, v->covered_seq, 2);
    v->flags = (uint8_t)scalar(c, v->flags, 1);
    if (c->error == ZT_OK && !(slot(v->slot) && role(v->role) && cause(v->role, v->cause_slot, v->cause_seq) && !(v->flags & ~3u))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_role_entry(const zt_wire_role_entry_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_role_entry_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_role_entry(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_role_entry(const uint8_t *buf, size_t len, zt_wire_role_entry_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_role_entry_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_role_entry(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_watermark_entry(cursor_t *c, zt_wire_watermark_entry_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->server_received_contiguous = (uint16_t)scalar(c, v->server_received_contiguous, 2);
    v->server_final_contiguous = (uint16_t)scalar(c, v->server_final_contiguous, 2);
    if (c->error == ZT_OK && !(slot(v->slot) && v->server_final_contiguous <= v->server_received_contiguous)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_watermark_entry(const zt_wire_watermark_entry_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_watermark_entry_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_watermark_entry(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_watermark_entry(const uint8_t *buf, size_t len, zt_wire_watermark_entry_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_watermark_entry_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_watermark_entry(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_decision_entry(cursor_t *c, zt_wire_decision_entry_t *v)
{
    v->victim_slot = (uint8_t)scalar(c, v->victim_slot, 1);
    v->event_seq = (uint16_t)scalar(c, v->event_seq, 2);
    v->status = (uint8_t)scalar(c, v->status, 1);
    v->reason = (uint8_t)scalar(c, v->reason, 1);
    v->archived = (uint8_t)scalar(c, v->archived, 1);
    if (c->error == ZT_OK && !(slot(v->victim_slot) && v->event_seq && v->status <= ZT_DECISION_PENDING_DEPENDENCY && v->reason <= ZT_REASON_INVALID_PAYLOAD && v->archived <= 1 && !(v->archived && v->status == ZT_DECISION_PENDING_DEPENDENCY))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_decision_entry(const zt_wire_decision_entry_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_decision_entry_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_decision_entry(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_decision_entry(const uint8_t *buf, size_t len, zt_wire_decision_entry_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_decision_entry_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_decision_entry(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_cache_key(cursor_t *c, zt_wire_cache_key_t *v)
{
    v->origin_slot = (uint8_t)scalar(c, v->origin_slot, 1);
    v->event_seq = (uint16_t)scalar(c, v->event_seq, 2);
    if (c->error == ZT_OK && !(slot(v->origin_slot) && v->event_seq)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_cache_key(const zt_wire_cache_key_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_cache_key_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_cache_key(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_cache_key(const uint8_t *buf, size_t len, zt_wire_cache_key_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_cache_key_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_cache_key(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_beacon(cursor_t *c, zt_wire_beacon_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->role = (uint8_t)scalar(c, v->role, 1);
    v->phase = (uint8_t)scalar(c, v->phase, 1);
    v->flags = (uint8_t)scalar(c, v->flags, 1);
    v->role_rev = (uint16_t)scalar(c, v->role_rev, 2);
    v->infection_cause_seq = (uint16_t)scalar(c, v->infection_cause_seq, 2);
    v->own_produced_seq = (uint16_t)scalar(c, v->own_produced_seq, 2);
    v->own_server_received = (uint16_t)scalar(c, v->own_server_received, 2);
    v->own_server_final = (uint16_t)scalar(c, v->own_server_final, 2);
    v->elapsed_ms = (int32_t)scalar(c, v->elapsed_ms, 4);
    v->time_quality = (uint8_t)scalar(c, v->time_quality, 1);
    v->round_channel = (uint8_t)scalar(c, v->round_channel, 1);
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->gateway_serial = (uint32_t)scalar(c, v->gateway_serial, 4);
    v->gateway_hops = (uint8_t)scalar(c, v->gateway_hops, 1);
    v->gateway_age_s = (uint8_t)scalar(c, v->gateway_age_s, 1);
    v->cache_digest = (uint64_t)scalar(c, v->cache_digest, 8);
    v->cache_count = (uint16_t)scalar(c, v->cache_count, 2);
    v->uncertainty_ms = (uint16_t)scalar(c, v->uncertainty_ms, 2);
    v->gateway_boot_nonce = (zt_boot_nonce_t)scalar(c, v->gateway_boot_nonce, 8);
    if (c->error == ZT_OK && !(slot_or_all(v->slot) && role(v->role) && v->phase <= ZT_PHASE_FINAL && !(v->flags & ~31u) && v->time_quality <= 1 && channel(v->round_channel) && v->cache_count <= ZT_CACHE_CAPACITY && v->gateway_hops <= ZT_MAX_HOPS && v->own_server_final <= v->own_server_received && v->own_server_received <= v->own_produced_seq && (!v->time_quality || (v->elapsed_ms != ZT_INT32_UNKNOWN && v->uncertainty_ms <= ZT_TIME_UNCERTAINTY_MAX_MS)))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_beacon(const zt_wire_beacon_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_beacon_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_beacon(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_beacon(const uint8_t *buf, size_t len, zt_wire_beacon_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_beacon_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_beacon(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_tag_request(cursor_t *c, zt_wire_tag_request_t *v)
{
    v->victim_slot = (uint8_t)scalar(c, v->victim_slot, 1);
    v->actor_slot = (uint8_t)scalar(c, v->actor_slot, 1);
    v->actor_cause_seq = (uint16_t)scalar(c, v->actor_cause_seq, 2);
    v->actor_role_rev = (uint16_t)scalar(c, v->actor_role_rev, 2);
    v->known_victim_role_rev = (uint16_t)scalar(c, v->known_victim_role_rev, 2);
    v->attempt_seq = (uint32_t)scalar(c, v->attempt_seq, 4);
    v->actor_elapsed_ms = (uint32_t)scalar(c, v->actor_elapsed_ms, 4);
    v->tagger_observed_victim_rssi = (int8_t)scalar(c, v->tagger_observed_victim_rssi, 1);
    if (c->error == ZT_OK && !(slot(v->victim_slot) && slot(v->actor_slot) && v->victim_slot != v->actor_slot && v->actor_elapsed_ms < ZT_ROUND_DURATION_MS)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_tag_request(const zt_wire_tag_request_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_tag_request_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_tag_request(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_tag_request(const uint8_t *buf, size_t len, zt_wire_tag_request_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_tag_request_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_tag_request(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_tag_result(cursor_t *c, zt_wire_tag_result_t *v)
{
    v->actor_slot = (uint8_t)scalar(c, v->actor_slot, 1);
    v->request_boot = (zt_boot_nonce_t)scalar(c, v->request_boot, 8);
    v->request_seq = (uint32_t)scalar(c, v->request_seq, 4);
    v->result = (uint8_t)scalar(c, v->result, 1);
    if (v->result == ZT_TAG_ACCEPTED) fields_event(c, &v->accepted_event);
    else {
        for (unsigned i = 0; i < ZT_EVENT_BYTES; ++i)
            if (scalar(c, 0, 1) != 0) c->error = ZT_ERR_PROTOCOL;
    }
    if (c->error == ZT_OK && !(slot(v->actor_slot) && v->result <= ZT_TAG_PENDING)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_tag_result(const zt_wire_tag_result_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_tag_result_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_tag_result(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_tag_result(const uint8_t *buf, size_t len, zt_wire_tag_result_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_tag_result_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_tag_result(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_join(cursor_t *c, zt_wire_join_t *v)
{
    for (unsigned i = 0; i < 6; ++i) v->requested_badge_mac.bytes[i] = (uint8_t)scalar(c, v->requested_badge_mac.bytes[i], 1);
    v->name_len = (uint8_t)scalar(c, v->name_len, 1);
    for (unsigned i = 0; i < 12; ++i) v->name[i] = (uint8_t)scalar(c, v->name[i], 1);
    v->known_round_id = (uint64_t)scalar(c, v->known_round_id, 8);
    v->request_nonce = (uint32_t)scalar(c, v->request_nonce, 4);
    for (unsigned i = 0; i < 8; ++i) v->build_id[i] = (uint8_t)scalar(c, v->build_id[i], 1);
    if (c->error == ZT_OK && !(name_valid(v->name, v->name_len) && printable(v->build_id, 8))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_join(const zt_wire_join_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_join_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_join(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_join(const uint8_t *buf, size_t len, zt_wire_join_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_join_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_join(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_join_result(cursor_t *c, zt_wire_join_result_t *v)
{
    for (unsigned i = 0; i < 6; ++i) v->target_mac.bytes[i] = (uint8_t)scalar(c, v->target_mac.bytes[i], 1);
    v->request_nonce = (uint32_t)scalar(c, v->request_nonce, 4);
    v->status = (uint8_t)scalar(c, v->status, 1);
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    if (c->error == ZT_OK && !(v->status <= ZT_JOIN_WAITING_FOR_SERVER && slot_or_all(v->slot) && (v->status > ZT_JOIN_REJOINED || slot(v->slot)))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_join_result(const zt_wire_join_result_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_join_result_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_join_result(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_join_result(const uint8_t *buf, size_t len, zt_wire_join_result_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_join_result_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_join_result(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_roster_page(cursor_t *c, zt_wire_roster_page_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->page_index = (uint8_t)scalar(c, v->page_index, 1);
    v->page_count = (uint8_t)scalar(c, v->page_count, 1);
    v->entry_count = (uint8_t)scalar(c, v->entry_count, 1);
    if (v->entry_count > 8) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->entry_count; ++i) fields_roster_entry(c, &v->entries[i]);
    if (c->error == ZT_OK && !(pages(v->page_index, v->page_count, 3) && v->entry_count > 0)) c->error = ZT_ERR_PROTOCOL;
    for (unsigned i = 0; c->error == ZT_OK && i < v->entry_count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].slot == v->entries[j].slot) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_roster_page(const zt_wire_roster_page_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_roster_page_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_roster_page(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_roster_page(const uint8_t *buf, size_t len, zt_wire_roster_page_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_roster_page_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_roster_page(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_host_state(cursor_t *c, zt_wire_host_state_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->phase = (uint8_t)scalar(c, v->phase, 1);
    v->page_index = (uint8_t)scalar(c, v->page_index, 1);
    v->page_count = (uint8_t)scalar(c, v->page_count, 1);
    v->entry_count = (uint8_t)scalar(c, v->entry_count, 1);
    v->host_elapsed_ms = (int32_t)scalar(c, v->host_elapsed_ms, 4);
    v->rule_rev = (uint16_t)scalar(c, v->rule_rev, 2);
    v->round_channel = (uint8_t)scalar(c, v->round_channel, 1);
    v->winner = (uint8_t)scalar(c, v->winner, 1);
    v->duration_ms = (uint32_t)scalar(c, v->duration_ms, 4);
    v->remaining_ms = (uint32_t)scalar(c, v->remaining_ms, 4);
    v->patient_zero_slot = (uint8_t)scalar(c, v->patient_zero_slot, 1);
    v->gateway_serial = (uint32_t)scalar(c, v->gateway_serial, 4);
    if (v->entry_count > 12) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->entry_count; ++i) fields_role_entry(c, &v->entries[i]);
    v->uncertainty_ms = (uint16_t)scalar(c, v->uncertainty_ms, 2);
    /* Accept the original exact-length form as no connectivity evidence.
     * New encoders append one bounded flags byte; no other trailing data. */
    if (!c->in || c->pos < c->size) v->gateway_flags = (uint8_t)scalar(c, v->gateway_flags, 1);
    if (v->gateway_flags & ~ZT_HOST_STATE_FLAG_SERVER_CONNECTED) c->error = ZT_ERR_PROTOCOL;
    if (c->error == ZT_OK && !(v->phase <= ZT_PHASE_FINAL && role(v->winner) && channel(v->round_channel) && slot_or_all(v->patient_zero_slot) && v->duration_ms == ZT_ROUND_DURATION_MS && v->remaining_ms <= ZT_ROUND_DURATION_MS && ((v->page_count == 0 && v->page_index == 0 && v->entry_count == 0) || (pages(v->page_index, v->page_count, ZT_SNAPSHOT_MAX_PAGES) && v->entry_count > 0)))) c->error = ZT_ERR_PROTOCOL;
    for (unsigned i = 0; c->error == ZT_OK && i < v->entry_count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].slot == v->entries[j].slot) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_host_state(const zt_wire_host_state_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_host_state_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_host_state(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_host_state(const uint8_t *buf, size_t len, zt_wire_host_state_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_host_state_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_host_state(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_watermarks(cursor_t *c, zt_wire_watermarks_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->count = (uint8_t)scalar(c, v->count, 1);
    if (v->count > 20) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->count; ++i) fields_watermark_entry(c, &v->entries[i]);
    for (unsigned i = 0; c->error == ZT_OK && i < v->count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].slot == v->entries[j].slot) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_watermarks(const zt_wire_watermarks_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_watermarks_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_watermarks(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_watermarks(const uint8_t *buf, size_t len, zt_wire_watermarks_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_watermarks_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_watermarks(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_command(cursor_t *c, zt_wire_command_t *v)
{
    v->command_seq = (uint32_t)scalar(c, v->command_seq, 4);
    v->kind = (uint8_t)scalar(c, v->kind, 1);
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    v->valid_until_elapsed_ms = (uint32_t)scalar(c, v->valid_until_elapsed_ms, 4);
    v->args_len = (uint8_t)scalar(c, v->args_len, 1);
    if (v->args_len > 175) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->args_len; ++i) v->args[i] = (uint8_t)scalar(c, v->args[i], 1);
    if (c->error == ZT_OK && !(slot_or_all(v->target_slot) && command_valid(v))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_command(const zt_wire_command_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_command_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_command(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_command(const uint8_t *buf, size_t len, zt_wire_command_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_command_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_command(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_command_receipt(cursor_t *c, zt_wire_command_receipt_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->command_seq = (uint32_t)scalar(c, v->command_seq, 4);
    v->state = (uint8_t)scalar(c, v->state, 1);
    v->detail = (uint16_t)scalar(c, v->detail, 2);
    v->applied_snapshot_rev = (uint32_t)scalar(c, v->applied_snapshot_rev, 4);
    if (c->error == ZT_OK && !(slot(v->slot) && v->state <= ZT_RECEIPT_REQUIRES_SNAPSHOT && v->detail <= ZT_DETAIL_STALE_REVISION)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_command_receipt(const zt_wire_command_receipt_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_command_receipt_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_command_receipt(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_command_receipt(const uint8_t *buf, size_t len, zt_wire_command_receipt_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_command_receipt_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_command_receipt(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_snapshot_request(cursor_t *c, zt_wire_snapshot_request_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->wanted_snapshot_rev = (uint32_t)scalar(c, v->wanted_snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->need_flags = (uint8_t)scalar(c, v->need_flags, 1);
    if (c->error == ZT_OK && !(slot_or_all(v->slot) && !(v->need_flags & ~15u))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_snapshot_request(const zt_wire_snapshot_request_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_snapshot_request_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_snapshot_request(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_snapshot_request(const uint8_t *buf, size_t len, zt_wire_snapshot_request_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_snapshot_request_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_snapshot_request(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_round_closed(cursor_t *c, zt_wire_round_closed_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->produced_seq = (uint16_t)scalar(c, v->produced_seq, 2);
    v->last_role_rev = (uint16_t)scalar(c, v->last_role_rev, 2);
    v->close_elapsed_ms = (uint32_t)scalar(c, v->close_elapsed_ms, 4);
    if (c->error == ZT_OK && !(slot(v->slot))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_round_closed(const zt_wire_round_closed_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_round_closed_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_round_closed(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_round_closed(const uint8_t *buf, size_t len, zt_wire_round_closed_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_round_closed_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_round_closed(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_close_receipts(cursor_t *c, zt_wire_close_receipts_t *v)
{
    v->closed_bitmap = (uint32_t)scalar(c, v->closed_bitmap, 4);
    v->archived_bitmap = (uint32_t)scalar(c, v->archived_bitmap, 4);
    if (c->error == ZT_OK && !(!((v->closed_bitmap | v->archived_bitmap) & ~ZT_VALID_SLOT_BITMAP) && !(v->archived_bitmap & ~v->closed_bitmap))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_close_receipts(const zt_wire_close_receipts_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_close_receipts_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_close_receipts(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_close_receipts(const uint8_t *buf, size_t len, zt_wire_close_receipts_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_close_receipts_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_close_receipts(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_event_decisions(cursor_t *c, zt_wire_event_decisions_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->count = (uint8_t)scalar(c, v->count, 1);
    if (v->count > 20) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->count; ++i) fields_decision_entry(c, &v->entries[i]);
    for (unsigned i = 0; c->error == ZT_OK && i < v->count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].victim_slot == v->entries[j].victim_slot && v->entries[i].event_seq == v->entries[j].event_seq) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_event_decisions(const zt_wire_event_decisions_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_event_decisions_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_event_decisions(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_event_decisions(const uint8_t *buf, size_t len, zt_wire_event_decisions_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_event_decisions_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_event_decisions(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_decision_receipt(cursor_t *c, zt_wire_decision_receipt_t *v)
{
    v->slot = (uint8_t)scalar(c, v->slot, 1);
    v->own_decided_contiguous = (uint16_t)scalar(c, v->own_decided_contiguous, 2);
    if (c->error == ZT_OK && !(slot(v->slot))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_decision_receipt(const zt_wire_decision_receipt_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_decision_receipt_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_decision_receipt(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_decision_receipt(const uint8_t *buf, size_t len, zt_wire_decision_receipt_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_decision_receipt_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_decision_receipt(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_cache_page(cursor_t *c, zt_wire_cache_page_t *v)
{
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    v->cache_digest = (uint64_t)scalar(c, v->cache_digest, 8);
    v->page_index = (uint8_t)scalar(c, v->page_index, 1);
    v->page_count = (uint8_t)scalar(c, v->page_count, 1);
    v->count = (uint8_t)scalar(c, v->count, 1);
    if (v->count > 56) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->count; ++i) fields_cache_key(c, &v->entries[i]);
    if (c->error == ZT_OK && !(slot_or_all(v->target_slot) && pages(v->page_index, v->page_count, 3))) c->error = ZT_ERR_PROTOCOL;
    for (unsigned i = 0; c->error == ZT_OK && i < v->count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].origin_slot == v->entries[j].origin_slot && v->entries[i].event_seq == v->entries[j].event_seq) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_cache_page(const zt_wire_cache_page_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_cache_page_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_cache_page(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_cache_page(const uint8_t *buf, size_t len, zt_wire_cache_page_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_cache_page_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_cache_page(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_want_events(cursor_t *c, zt_wire_want_events_t *v)
{
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    v->request_seq = (uint16_t)scalar(c, v->request_seq, 2);
    v->count = (uint8_t)scalar(c, v->count, 1);
    if (v->count > 16) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->count; ++i) fields_cache_key(c, &v->entries[i]);
    if (c->error == ZT_OK && !(slot(v->target_slot) && v->count > 0)) c->error = ZT_ERR_PROTOCOL;
    for (unsigned i = 0; c->error == ZT_OK && i < v->count; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (v->entries[i].origin_slot == v->entries[j].origin_slot && v->entries[i].event_seq == v->entries[j].event_seq) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_want_events(const zt_wire_want_events_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_want_events_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_want_events(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_want_events(const uint8_t *buf, size_t len, zt_wire_want_events_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_want_events_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_want_events(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_event_copy(cursor_t *c, zt_wire_event_copy_t *v)
{
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    fields_event(c, &v->event);
    if (c->error == ZT_OK && !(slot_or_all(v->target_slot))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_event_copy(const zt_wire_event_copy_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_event_copy_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_event_copy(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_event_copy(const uint8_t *buf, size_t len, zt_wire_event_copy_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_event_copy_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_event_copy(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_time_query(cursor_t *c, zt_wire_time_query_t *v)
{
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    v->request_nonce = (uint32_t)scalar(c, v->request_nonce, 4);
    if (c->error == ZT_OK && !(slot(v->target_slot))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_time_query(const zt_wire_time_query_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_time_query_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_time_query(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_time_query(const uint8_t *buf, size_t len, zt_wire_time_query_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_time_query_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_time_query(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_time_reply(cursor_t *c, zt_wire_time_reply_t *v)
{
    v->target_slot = (uint8_t)scalar(c, v->target_slot, 1);
    v->request_nonce = (uint32_t)scalar(c, v->request_nonce, 4);
    v->sampled_elapsed_ms = (int32_t)scalar(c, v->sampled_elapsed_ms, 4);
    v->source_snapshot_rev = (uint32_t)scalar(c, v->source_snapshot_rev, 4);
    v->source_age_ms = (uint32_t)scalar(c, v->source_age_ms, 4);
    v->uncertainty_ms = (uint16_t)scalar(c, v->uncertainty_ms, 2);
    v->time_quality = (uint8_t)scalar(c, v->time_quality, 1);
    if (c->error == ZT_OK && !(slot(v->target_slot) && v->time_quality <= 1 && (!v->time_quality || (v->sampled_elapsed_ms != ZT_INT32_UNKNOWN && v->uncertainty_ms <= ZT_TIME_UNCERTAINTY_MAX_MS)))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_time_reply(const zt_wire_time_reply_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_time_reply_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_time_reply(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_time_reply(const uint8_t *buf, size_t len, zt_wire_time_reply_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_time_reply_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_time_reply(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_prepare_round_args(cursor_t *c, zt_wire_prepare_round_args_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->roster_count = (uint8_t)scalar(c, v->roster_count, 1);
    v->duration_ms = (uint32_t)scalar(c, v->duration_ms, 4);
    v->channel = (uint8_t)scalar(c, v->channel, 1);
    v->tag_rssi = (int8_t)scalar(c, v->tag_rssi, 1);
    v->tag_cooldown_ms = (uint16_t)scalar(c, v->tag_cooldown_ms, 2);
    if (c->error == ZT_OK && !(v->roster_count >= ZT_MIN_ROSTER_PLAYERS && v->roster_count <= ZT_MAX_PLAYERS && v->duration_ms == ZT_ROUND_DURATION_MS && channel(v->channel) && v->tag_rssi < 0 && v->tag_cooldown_ms == ZT_TAG_COOLDOWN_MS)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_prepare_round_args(const zt_wire_prepare_round_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_prepare_round_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_prepare_round_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_prepare_round_args(const uint8_t *buf, size_t len, zt_wire_prepare_round_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_prepare_round_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_prepare_round_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_start_round_args(cursor_t *c, zt_wire_start_round_args_t *v)
{
    v->snapshot_rev = (uint32_t)scalar(c, v->snapshot_rev, 4);
    v->roster_hash = (uint64_t)scalar(c, v->roster_hash, 8);
    v->patient_zero_slot = (uint8_t)scalar(c, v->patient_zero_slot, 1);
    v->initial_role_rev = (uint16_t)scalar(c, v->initial_role_rev, 2);
    v->sampled_elapsed_ms = (int32_t)scalar(c, v->sampled_elapsed_ms, 4);
    v->duration_ms = (uint32_t)scalar(c, v->duration_ms, 4);
    v->uncertainty_ms = (uint16_t)scalar(c, v->uncertainty_ms, 2);
    if (c->error == ZT_OK && !(slot(v->patient_zero_slot) && v->sampled_elapsed_ms != ZT_INT32_UNKNOWN && v->duration_ms == ZT_ROUND_DURATION_MS && v->uncertainty_ms <= ZT_TIME_UNCERTAINTY_MAX_MS)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_start_round_args(const zt_wire_start_round_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_start_round_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_start_round_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_start_round_args(const uint8_t *buf, size_t len, zt_wire_start_round_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_start_round_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_start_round_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_role_set_args(cursor_t *c, zt_wire_role_set_args_t *v)
{
    v->role = (uint8_t)scalar(c, v->role, 1);
    v->role_rev = (uint16_t)scalar(c, v->role_rev, 2);
    v->cause_slot = (uint8_t)scalar(c, v->cause_slot, 1);
    v->cause_seq = (uint16_t)scalar(c, v->cause_seq, 2);
    v->covered_seq = (uint16_t)scalar(c, v->covered_seq, 2);
    if (c->error == ZT_OK && !(v->role <= ZT_ROLE_ZOMBIE && cause(v->role, v->cause_slot, v->cause_seq))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_role_set_args(const zt_wire_role_set_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_role_set_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_role_set_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_role_set_args(const uint8_t *buf, size_t len, zt_wire_role_set_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_role_set_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_role_set_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_announce_args(cursor_t *c, zt_wire_announce_args_t *v)
{
    v->text_len = (uint8_t)scalar(c, v->text_len, 1);
    if (v->text_len > 96) { c->error = ZT_ERR_INVALID_LENGTH; return; }
    for (unsigned i = 0; i < v->text_len; ++i) v->text[i] = (uint8_t)scalar(c, v->text[i], 1);
    if (c->error == ZT_OK && !(v->text_len >= 1 && printable(v->text, v->text_len))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_announce_args(const zt_wire_announce_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_announce_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_announce_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_announce_args(const uint8_t *buf, size_t len, zt_wire_announce_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_announce_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_announce_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_end_round_args(cursor_t *c, zt_wire_end_round_args_t *v)
{
    v->effective_elapsed_ms = (uint32_t)scalar(c, v->effective_elapsed_ms, 4);
    v->reason = (uint8_t)scalar(c, v->reason, 1);
    v->winner = (uint8_t)scalar(c, v->winner, 1);
    v->provisional = (uint8_t)scalar(c, v->provisional, 1);
    if (c->error == ZT_OK && !(v->reason <= ZT_END_OPERATOR_STOP && role(v->winner) && v->provisional <= 1 && v->effective_elapsed_ms <= ZT_ROUND_DURATION_MS)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_end_round_args(const zt_wire_end_round_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_end_round_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_end_round_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_end_round_args(const uint8_t *buf, size_t len, zt_wire_end_round_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_end_round_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_end_round_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_final_result_args(cursor_t *c, zt_wire_final_result_args_t *v)
{
    v->winner = (uint8_t)scalar(c, v->winner, 1);
    v->complete = (uint8_t)scalar(c, v->complete, 1);
    v->missing_slots_bitmap = (uint32_t)scalar(c, v->missing_slots_bitmap, 4);
    v->state_rev = (uint32_t)scalar(c, v->state_rev, 4);
    if (c->error == ZT_OK && !(role(v->winner) && v->complete <= 1 && !(v->missing_slots_bitmap & ~ZT_VALID_SLOT_BITMAP))) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_final_result_args(const zt_wire_final_result_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_final_result_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_final_result_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_final_result_args(const uint8_t *buf, size_t len, zt_wire_final_result_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_final_result_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_final_result_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static void fields_cancel_prepare_args(cursor_t *c, zt_wire_cancel_prepare_args_t *v)
{
    v->reason = (uint8_t)scalar(c, v->reason, 1);
    if (c->error == ZT_OK && !(v->reason <= ZT_CANCEL_INVALID_CHANNEL)) c->error = ZT_ERR_PROTOCOL;
}

zt_err_t zt_wire_encode_cancel_prepare_args(const zt_wire_cancel_prepare_args_t *value, uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!value || !out || !written) return ZT_ERR_INVALID_ARG;
    zt_wire_cancel_prepare_args_t v = *value;
    cursor_t c = {.out = out, .size = capacity};
    fields_cancel_prepare_args(&c, &v);
    if (c.error == ZT_OK) *written = c.pos;
    return c.error;
}
zt_err_t zt_wire_decode_cancel_prepare_args(const uint8_t *buf, size_t len, zt_wire_cancel_prepare_args_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    zt_wire_cancel_prepare_args_t v = {0};
    cursor_t c = {.in = buf, .size = len};
    fields_cancel_prepare_args(&c, &v);
    if (c.error != ZT_OK) return c.error;
    if (c.pos != len) return ZT_ERR_INVALID_LENGTH;
    *out = v;
    return ZT_OK;
}

static bool command_valid(const zt_wire_command_t *v)
{
    if (v->kind != ZT_CMD_ANNOUNCE && v->valid_until_elapsed_ms != ZT_COMMAND_NO_EXPIRY) return false;
    if (v->kind == ZT_CMD_ANNOUNCE && v->valid_until_elapsed_ms == ZT_COMMAND_NO_EXPIRY) return false;
    switch (v->kind) {
    case ZT_CMD_PREPARE_ROUND: { zt_wire_prepare_round_args_t a; return zt_wire_decode_prepare_round_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_START_ROUND: { zt_wire_start_round_args_t a; return zt_wire_decode_start_round_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_ROLE_SET: { zt_wire_role_set_args_t a; return zt_wire_decode_role_set_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_ANNOUNCE: { zt_wire_announce_args_t a; return zt_wire_decode_announce_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_END_ROUND: { zt_wire_end_round_args_t a; return zt_wire_decode_end_round_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_FINAL_RESULT: { zt_wire_final_result_args_t a; return zt_wire_decode_final_result_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_CANCEL_PREPARE: { zt_wire_cancel_prepare_args_t a; return zt_wire_decode_cancel_prepare_args(v->args, v->args_len, &a) == ZT_OK; }
    case ZT_CMD_RESET_GAME: return slot(v->target_slot) && (v->args_len == 0 || v->args_len == 14);
    default: return false;
    }
}

zt_err_t zt_radio_payload_encode(uint8_t type, const zt_wire_payload_t *v, uint8_t *buf, size_t cap, size_t *len)
{
    if (!v) return ZT_ERR_INVALID_ARG;
    switch (type) {
    case ZT_PKT_BEACON: return zt_wire_encode_beacon(&v->beacon, buf, cap, len);
    case ZT_PKT_TAG_REQUEST: return zt_wire_encode_tag_request(&v->tag_request, buf, cap, len);
    case ZT_PKT_TAG_RESULT: return zt_wire_encode_tag_result(&v->tag_result, buf, cap, len);
    case ZT_PKT_JOIN: return zt_wire_encode_join(&v->join, buf, cap, len);
    case ZT_PKT_JOIN_RESULT: return zt_wire_encode_join_result(&v->join_result, buf, cap, len);
    case ZT_PKT_EVENT: return zt_wire_encode_event(&v->event, buf, cap, len);
    case ZT_PKT_ROSTER_PAGE: return zt_wire_encode_roster_page(&v->roster_page, buf, cap, len);
    case ZT_PKT_HOST_STATE: return zt_wire_encode_host_state(&v->host_state, buf, cap, len);
    case ZT_PKT_WATERMARKS: return zt_wire_encode_watermarks(&v->watermarks, buf, cap, len);
    case ZT_PKT_COMMAND: return zt_wire_encode_command(&v->command, buf, cap, len);
    case ZT_PKT_COMMAND_RECEIPT: return zt_wire_encode_command_receipt(&v->command_receipt, buf, cap, len);
    case ZT_PKT_SNAPSHOT_REQUEST: return zt_wire_encode_snapshot_request(&v->snapshot_request, buf, cap, len);
    case ZT_PKT_ROUND_CLOSED: return zt_wire_encode_round_closed(&v->round_closed, buf, cap, len);
    case ZT_PKT_CLOSE_RECEIPTS: return zt_wire_encode_close_receipts(&v->close_receipts, buf, cap, len);
    case ZT_PKT_EVENT_DECISIONS: return zt_wire_encode_event_decisions(&v->event_decisions, buf, cap, len);
    case ZT_PKT_DECISION_RECEIPT: return zt_wire_encode_decision_receipt(&v->decision_receipt, buf, cap, len);
    case ZT_PKT_CACHE_PAGE: return zt_wire_encode_cache_page(&v->cache_page, buf, cap, len);
    case ZT_PKT_WANT_EVENTS: return zt_wire_encode_want_events(&v->want_events, buf, cap, len);
    case ZT_PKT_EVENT_COPY: return zt_wire_encode_event_copy(&v->event_copy, buf, cap, len);
    case ZT_PKT_TIME_QUERY: return zt_wire_encode_time_query(&v->time_query, buf, cap, len);
    case ZT_PKT_TIME_REPLY: return zt_wire_encode_time_reply(&v->time_reply, buf, cap, len);
    default: return ZT_ERR_PROTOCOL;
    }
}

zt_err_t zt_radio_payload_decode(uint8_t type, const uint8_t *buf, size_t len, zt_wire_payload_t *v)
{
    if (!v) return ZT_ERR_INVALID_ARG;
    switch (type) {
    case ZT_PKT_BEACON: return zt_wire_decode_beacon(buf, len, &v->beacon);
    case ZT_PKT_TAG_REQUEST: return zt_wire_decode_tag_request(buf, len, &v->tag_request);
    case ZT_PKT_TAG_RESULT: return zt_wire_decode_tag_result(buf, len, &v->tag_result);
    case ZT_PKT_JOIN: return zt_wire_decode_join(buf, len, &v->join);
    case ZT_PKT_JOIN_RESULT: return zt_wire_decode_join_result(buf, len, &v->join_result);
    case ZT_PKT_EVENT: return zt_wire_decode_event(buf, len, &v->event);
    case ZT_PKT_ROSTER_PAGE: return zt_wire_decode_roster_page(buf, len, &v->roster_page);
    case ZT_PKT_HOST_STATE: return zt_wire_decode_host_state(buf, len, &v->host_state);
    case ZT_PKT_WATERMARKS: return zt_wire_decode_watermarks(buf, len, &v->watermarks);
    case ZT_PKT_COMMAND: return zt_wire_decode_command(buf, len, &v->command);
    case ZT_PKT_COMMAND_RECEIPT: return zt_wire_decode_command_receipt(buf, len, &v->command_receipt);
    case ZT_PKT_SNAPSHOT_REQUEST: return zt_wire_decode_snapshot_request(buf, len, &v->snapshot_request);
    case ZT_PKT_ROUND_CLOSED: return zt_wire_decode_round_closed(buf, len, &v->round_closed);
    case ZT_PKT_CLOSE_RECEIPTS: return zt_wire_decode_close_receipts(buf, len, &v->close_receipts);
    case ZT_PKT_EVENT_DECISIONS: return zt_wire_decode_event_decisions(buf, len, &v->event_decisions);
    case ZT_PKT_DECISION_RECEIPT: return zt_wire_decode_decision_receipt(buf, len, &v->decision_receipt);
    case ZT_PKT_CACHE_PAGE: return zt_wire_decode_cache_page(buf, len, &v->cache_page);
    case ZT_PKT_WANT_EVENTS: return zt_wire_decode_want_events(buf, len, &v->want_events);
    case ZT_PKT_EVENT_COPY: return zt_wire_decode_event_copy(buf, len, &v->event_copy);
    case ZT_PKT_TIME_QUERY: return zt_wire_decode_time_query(buf, len, &v->time_query);
    case ZT_PKT_TIME_REPLY: return zt_wire_decode_time_reply(buf, len, &v->time_reply);
    default: return ZT_ERR_PROTOCOL;
    }
}

/* RFC 2104, using SHA256's fixed context rather than an allocating md context. */
zt_err_t zt_wire_hmac(const uint8_t key[ZT_HMAC_KEY_BYTES], const uint8_t *data,
                      size_t len, uint8_t tag[ZT_HMAC_TAG_BYTES])
{
    if (!key || !data || !tag || len > ZT_WIRE_HEADER_BYTES + ZT_MAX_PAYLOAD)
        return ZT_ERR_INVALID_ARG;
    uint8_t pad[64], hash[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    for (unsigned i = 0; i < sizeof(pad); ++i) pad[i] = (i < 32 ? key[i] : 0) ^ 0x36;
    int r = mbedtls_sha256_starts(&c, 0);
    if (!r) r = mbedtls_sha256_update(&c, pad, sizeof(pad));
    if (!r) r = mbedtls_sha256_update(&c, data, len);
    if (!r) r = mbedtls_sha256_finish(&c, hash);
    for (unsigned i = 0; i < sizeof(pad); ++i) pad[i] = (i < 32 ? key[i] : 0) ^ 0x5c;
    if (!r) r = mbedtls_sha256_starts(&c, 0);
    if (!r) r = mbedtls_sha256_update(&c, pad, sizeof(pad));
    if (!r) r = mbedtls_sha256_update(&c, hash, sizeof(hash));
    if (!r) r = mbedtls_sha256_finish(&c, hash);
    if (!r) for (unsigned i = 0; i < ZT_HMAC_TAG_BYTES; ++i) tag[i] = hash[i];
    mbedtls_sha256_free(&c);
    volatile uint8_t *p = pad;
    for (unsigned i = 0; i < sizeof(pad); ++i) p[i] = 0;
    p = hash;
    for (unsigned i = 0; i < sizeof(hash); ++i) p[i] = 0;
    return r ? ZT_ERR_AUTH : ZT_OK;
}

zt_err_t zt_wire_hmac_verify(const uint8_t expected[16], const uint8_t actual[16])
{
    if (!expected || !actual) return ZT_ERR_INVALID_ARG;
    volatile uint32_t difference = 0;
    for (unsigned i = 0; i < ZT_HMAC_TAG_BYTES; ++i) difference |= expected[i] ^ actual[i];
    return difference ? ZT_ERR_AUTH : ZT_OK;
}

bool zt_radio_flood_type(uint8_t t)
{
    switch (t) {
    case ZT_PKT_JOIN: case ZT_PKT_JOIN_RESULT: case ZT_PKT_EVENT:
    case ZT_PKT_ROSTER_PAGE: case ZT_PKT_HOST_STATE: case ZT_PKT_WATERMARKS:
    case ZT_PKT_COMMAND: case ZT_PKT_COMMAND_RECEIPT: case ZT_PKT_SNAPSHOT_REQUEST:
    case ZT_PKT_ROUND_CLOSED: case ZT_PKT_CLOSE_RECEIPTS:
    case ZT_PKT_EVENT_DECISIONS: case ZT_PKT_DECISION_RECEIPT: return true;
    default: return false;
    }
}

static void header_fields(cursor_t *c, zt_wire_header_t *v)
{
    v->magic = scalar(c, v->magic, 2);
    v->protocol_version = scalar(c, v->protocol_version, 1);
    v->type = scalar(c, v->type, 1);
    v->payload_len = scalar(c, v->payload_len, 2);
    v->flags = scalar(c, v->flags, 1);
    v->ttl_remaining = scalar(c, v->ttl_remaining, 1);
    v->hops = scalar(c, v->hops, 1);
    v->reserved = scalar(c, v->reserved, 1);
    v->game_id = scalar(c, v->game_id, 8);
    v->round_id = scalar(c, v->round_id, 8);
    for (unsigned i = 0; i < 6; ++i) v->origin.bytes[i] = scalar(c, v->origin.bytes[i], 1);
    v->origin_boot_nonce = scalar(c, v->origin_boot_nonce, 8);
    v->packet_seq = scalar(c, v->packet_seq, 4);
    v->age_ms = scalar(c, v->age_ms, 4);
}

static zt_err_t header_valid(const zt_wire_header_t *h)
{
    if (h->magic != ZT_WIRE_MAGIC || h->reserved || (h->flags & ~ZT_WIRE_FLAG_RELAY_CAPABLE)) return ZT_ERR_PROTOCOL;
    if (h->protocol_version != ZT_PROTOCOL_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    if (h->payload_len > ZT_MAX_PAYLOAD) return ZT_ERR_INVALID_LENGTH;
    if (zt_radio_flood_type(h->type)) {
        if (h->flags != ZT_WIRE_FLAG_RELAY_CAPABLE || h->ttl_remaining + h->hops != ZT_FLOOD_TTL) return ZT_ERR_PROTOCOL;
    } else if (h->flags || h->hops || h->ttl_remaining) return ZT_ERR_PROTOCOL;
    return ZT_OK;
}

/* Cheap structural peek is private to the radio component. No authentication
 * claim is made until decode_envelope has succeeded. */
zt_err_t zt_radio_wire_peek(const uint8_t *buf, size_t len, zt_wire_header_t *h)
{
    if (!buf || !h) return ZT_ERR_INVALID_ARG;
    if (len < 64 || len > ZT_MAX_FRAME_BYTES) return ZT_ERR_INVALID_LENGTH;
    cursor_t c = {.in = buf, .size = ZT_WIRE_HEADER_BYTES};
    zt_wire_header_t v = {0};
    header_fields(&c, &v);
    zt_err_t r = header_valid(&v);
    if (r != ZT_OK) return r;
    if (len != 64u + v.payload_len) return ZT_ERR_INVALID_LENGTH;
    /* Validates the counted length and mandatory fields before hashing. */
    zt_wire_payload_t body;
    r = zt_radio_payload_decode(v.type, buf + 48, v.payload_len, &body);
    if (r == ZT_OK) *h = v;
    return r;
}

zt_err_t zt_wire_encode_envelope(const zt_wire_header_t *header, const uint8_t *payload,
    size_t payload_len, const uint8_t key[32], uint8_t *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!header || !payload || !key || !out || !written) return ZT_ERR_INVALID_ARG;
    if (payload_len > ZT_MAX_PAYLOAD || header->payload_len != payload_len || capacity < 64 + payload_len) return ZT_ERR_INVALID_LENGTH;
    zt_err_t r = header_valid(header);
    if (r != ZT_OK) return r;
    zt_wire_payload_t body;
    r = zt_radio_payload_decode(header->type, payload, payload_len, &body);
    if (r != ZT_OK) return r;
    zt_wire_header_t v = *header;
    cursor_t c = {.out = out, .size = 48};
    header_fields(&c, &v);
    memmove(out + 48, payload, payload_len);
    r = zt_wire_hmac(key, out, 48 + payload_len, out + 48 + payload_len);
    if (r == ZT_OK) *written = 64 + payload_len;
    return r;
}

zt_err_t zt_wire_decode_envelope(const uint8_t *buf, size_t len, const uint8_t key[32],
    zt_wire_header_t *header, uint8_t *payload, size_t capacity, size_t *payload_len)
{
    if (payload_len) *payload_len = 0;
    if (!buf || !key || !header || !payload || !payload_len) return ZT_ERR_INVALID_ARG;
    zt_wire_header_t h;
    zt_err_t r = zt_radio_wire_peek(buf, len, &h);
    if (r != ZT_OK) return r;
    if (capacity < h.payload_len) return ZT_ERR_NO_SPACE;
    uint8_t tag[16];
    r = zt_wire_hmac(key, buf, len - 16, tag);
    if (r == ZT_OK) r = zt_wire_hmac_verify(tag, buf + len - 16);
    if (r != ZT_OK) return r;
    memmove(payload, buf + 48, h.payload_len);
    *header = h;
    *payload_len = h.payload_len;
    return ZT_OK;
}

zt_err_t zt_wire_roster_hash(const zt_wire_roster_entry_t *entries, size_t count, uint64_t *hash)
{
    if ((!entries && count) || !hash || count > ZT_MAX_PLAYERS) return ZT_ERR_INVALID_ARG;
    uint8_t canonical[ZT_MAX_PLAYERS * ZT_ROSTER_ENTRY_BYTES];
    size_t used = 0;
    for (unsigned slot_id = 0; slot_id < ZT_MAX_PLAYERS; ++slot_id) {
        bool found = false;
        for (size_t j = 0; j < count; ++j) if (entries[j].slot == slot_id) {
            if (found) return ZT_ERR_PROTOCOL;
            found = true;
            size_t n;
            zt_err_t r = zt_wire_encode_roster_entry(&entries[j], canonical + used, sizeof(canonical) - used, &n);
            if (r != ZT_OK) return r;
            used += n;
        }
    }
    if (used != count * ZT_ROSTER_ENTRY_BYTES) return ZT_ERR_PROTOCOL;
    uint8_t digest[32];
    if (mbedtls_sha256(canonical, used, digest, 0)) return ZT_ERR_AUTH;
    *hash = 0;
    for (unsigned i = 0; i < 8; ++i) *hash |= (uint64_t)digest[i] << (i * 8);
    return ZT_OK;
}

zt_err_t zt_wire_cache_digest(const zt_wire_cache_key_t *keys, size_t count, uint64_t *digest)
{
    if ((!keys && count) || !digest || count > ZT_CACHE_CAPACITY) return ZT_ERR_INVALID_ARG;
    zt_wire_cache_key_t sorted[ZT_CACHE_CAPACITY];
    for (size_t i = 0; i < count; ++i) {
        if (!slot(keys[i].origin_slot) || !keys[i].event_seq) return ZT_ERR_PROTOCOL;
        size_t j = i;
        while (j && (sorted[j-1].origin_slot > keys[i].origin_slot ||
            (sorted[j-1].origin_slot == keys[i].origin_slot && sorted[j-1].event_seq > keys[i].event_seq))) {
            sorted[j] = sorted[j-1]; --j;
        }
        if (j && sorted[j-1].origin_slot == keys[i].origin_slot && sorted[j-1].event_seq == keys[i].event_seq) return ZT_ERR_PROTOCOL;
        sorted[j] = keys[i];
    }
    uint8_t canonical[ZT_CACHE_CAPACITY * ZT_CACHE_KEY_BYTES], hash[32];
    for (size_t i = 0; i < count; ++i) {
        canonical[i*3] = sorted[i].origin_slot;
        canonical[i*3+1] = (uint8_t)sorted[i].event_seq;
        canonical[i*3+2] = (uint8_t)(sorted[i].event_seq >> 8);
    }
    if (mbedtls_sha256(canonical, count * 3, hash, 0)) return ZT_ERR_AUTH;
    *digest = 0;
    for (unsigned i = 0; i < 8; ++i) *digest |= (uint64_t)hash[i] << (i*8);
    return ZT_OK;
}
