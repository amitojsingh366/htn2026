#include "zt_gateway.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The gateway owns the caller-supplied workspace. No heap-backed JSON tree and
 * no borrowed strings survive consumption of the corresponding RX buffer. */
enum { J_OBJECT = 1, J_ARRAY, J_STRING, J_NUMBER, J_NULL, J_TRUE, J_FALSE };
#define J_NONE UINT16_MAX
#define J_DEPTH_MAX 8

typedef struct {
    const char *json;
    size_t len, pos;
    zt_json_workspace_t *ws;
    zt_err_t error;
} parser_t;

typedef struct {
    const char *json;
    zt_json_workspace_t *ws;
    zt_err_t error;
} decoder_t;

static void whitespace(parser_t *p)
{
    while (p->pos < p->len && (p->json[p->pos] == ' ' || p->json[p->pos] == '\n' ||
           p->json[p->pos] == '\r' || p->json[p->pos] == '\t')) ++p->pos;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_value(parser_t *p, uint16_t parent, unsigned depth);

static bool parse_string(parser_t *p)
{
    ++p->pos;
    while (p->pos < p->len) {
        unsigned char c = p->json[p->pos++];
        if (c == '"') return true;
        if (c < 0x20 || c > 0x7e) return false;
        if (c != '\\') continue;
        if (p->pos == p->len) return false;
        c = p->json[p->pos++];
        if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
            c == 'n' || c == 'r' || c == 't') continue;
        if (c != 'u' || p->len - p->pos < 4) return false;
        unsigned value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            int h = hex_digit(p->json[p->pos++]);
            if (h < 0) return false;
            value = (value << 4) | (unsigned)h;
        }
        /* V1 text is ASCII. Reject unicode and surrogate pairs rather than
         * accepting an ambiguous normalized badge name or field name. */
        if (value > 0x7e) return false;
    }
    return false;
}

static bool parse_container(parser_t *p, uint16_t self, unsigned depth, bool object)
{
    char close = object ? '}' : ']';
    ++p->pos;
    whitespace(p);
    if (p->pos < p->len && p->json[p->pos] == close) { ++p->pos; return true; }
    for (;;) {
        if (object) {
            if (p->pos >= p->len || p->json[p->pos] != '"') return false;
            uint16_t key = p->ws->count;
            if (!parse_value(p, self, depth + 1)) return false;
            zt_json_token_t *k = &p->ws->tokens[key];
            /* Keys cannot use escapes; duplicate keys are always invalid. */
            for (uint16_t i = k->start; i < k->end; ++i)
                if (p->json[i] == '\\') return false;
            unsigned child = 0;
            for (uint16_t i = self + 1; i < key; ++i) {
                const zt_json_token_t *prior = &p->ws->tokens[i];
                if (prior->parent != self) continue;
                if ((child++ & 1u) == 0 && prior->end - prior->start == k->end - k->start &&
                    memcmp(p->json + prior->start, p->json + k->start, k->end - k->start) == 0)
                    return false;
            }
            whitespace(p);
            if (p->pos >= p->len || p->json[p->pos++] != ':') return false;
            whitespace(p);
        }
        if (!parse_value(p, self, depth + 1)) return false;
        whitespace(p);
        if (p->pos >= p->len) return false;
        char c = p->json[p->pos++];
        if (c == close) return true;
        if (c != ',') return false;
        whitespace(p);
    }
}

static bool parse_value(parser_t *p, uint16_t parent, unsigned depth)
{
    whitespace(p);
    if (depth > J_DEPTH_MAX || p->pos >= p->len) return false;
    if (p->ws->count == ZT_GATEWAY_JSON_TOKEN_CAPACITY) {
        p->error = ZT_ERR_NO_SPACE;
        return false;
    }
    uint16_t self = p->ws->count++;
    zt_json_token_t *t = &p->ws->tokens[self];
    *t = (zt_json_token_t){.start = (uint16_t)p->pos, .parent = parent};
    if (parent != J_NONE) ++p->ws->tokens[parent].child_count;
    char c = p->json[p->pos];
    if (c == '{' || c == '[') {
        t->type = c == '{' ? J_OBJECT : J_ARRAY;
        if (!parse_container(p, self, depth, c == '{')) return false;
    } else if (c == '"') {
        t->type = J_STRING;
        if (!parse_string(p)) return false;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        t->type = J_NUMBER;
        if (c == '-' && ++p->pos == p->len) return false;
        c = p->json[p->pos];
        if (c == '0') ++p->pos;
        else {
            if (c < '1' || c > '9') return false;
            do { ++p->pos; } while (p->pos < p->len && p->json[p->pos] >= '0' && p->json[p->pos] <= '9');
        }
    } else {
        const char *literal = NULL;
        if (c == 'n') { literal = "null"; t->type = J_NULL; }
        if (c == 't') { literal = "true"; t->type = J_TRUE; }
        if (c == 'f') { literal = "false"; t->type = J_FALSE; }
        if (!literal) return false;
        size_t n = strlen(literal);
        if (p->len - p->pos < n || memcmp(p->json + p->pos, literal, n)) return false;
        p->pos += n;
    }
    t->end = (uint16_t)p->pos;
    return true;
}

static zt_err_t begin_decode(const char *json, size_t len, zt_json_workspace_t *ws, decoder_t *d)
{
    if (!json || !ws || !d) return ZT_ERR_INVALID_ARG;
    if (!len || len > ZT_GATEWAY_MESSAGE_MAX_BYTES) return ZT_ERR_INVALID_LENGTH;
    ws->count = 0;
    parser_t p = {.json = json, .len = len, .ws = ws, .error = ZT_ERR_PROTOCOL};
    if (!parse_value(&p, J_NONE, 0)) return p.error;
    whitespace(&p);
    if (p.pos != len || ws->tokens[0].type != J_OBJECT) return ZT_ERR_PROTOCOL;
    *d = (decoder_t){.json = json, .ws = ws, .error = ZT_OK};
    return ZT_OK;
}

static bool kind(decoder_t *d, int index, unsigned type)
{
    if (index < 0 || index >= d->ws->count || d->ws->tokens[index].type != type) {
        d->error = ZT_ERR_PROTOCOL;
        return false;
    }
    return true;
}

static bool equal(decoder_t *d, int index, const char *s)
{
    if (index < 0 || d->ws->tokens[index].type != J_STRING) return false;
    const zt_json_token_t *t = &d->ws->tokens[index];
    size_t n = strlen(s);
    return t->end - t->start == n + 2 && !memcmp(d->json + t->start + 1, s, n);
}

static int field(decoder_t *d, int object, const char *name)
{
    if (!kind(d, object, J_OBJECT)) return -1;
    const zt_json_token_t *o = &d->ws->tokens[object];
    for (int i = object + 1; i < d->ws->count && d->ws->tokens[i].start < o->end; ++i) {
        if (d->ws->tokens[i].parent != object) continue;
        if (equal(d, i, name)) return i + 1;
        ++i;
        if (i < d->ws->count) {
            uint16_t end = d->ws->tokens[i].end;
            while (i + 1 < d->ws->count && d->ws->tokens[i + 1].start < end) ++i;
        }
    }
    return -1;
}

static uint64_t number(decoder_t *d, int index, uint64_t max)
{
    if (!kind(d, index, J_NUMBER)) return 0;
    const zt_json_token_t *t = &d->ws->tokens[index];
    uint64_t value = 0;
    for (unsigned i = t->start; i < t->end; ++i) {
        unsigned digit = (unsigned char)d->json[i] - '0';
        if (digit > 9 || digit > max || value > (max - digit) / 10) {
            d->error = ZT_ERR_PROTOCOL;
            return 0;
        }
        value = value * 10 + digit;
    }
    return value;
}

static uint64_t uint_field(decoder_t *d, int object, const char *name, uint64_t max)
{
    return number(d, field(d, object, name), max);
}

static bool bool_field(decoder_t *d, int object, const char *name)
{
    int index = field(d, object, name);
    if (index >= 0 && d->ws->tokens[index].type == J_TRUE) return true;
    if (index < 0 || d->ws->tokens[index].type != J_FALSE) d->error = ZT_ERR_PROTOCOL;
    return false;
}

static int8_t rssi_field(decoder_t *d, int object)
{
    int index = field(d, object, "tag_rssi");
    if (!kind(d, index, J_NUMBER)) return 0;
    const zt_json_token_t *t = &d->ws->tokens[index];
    if (d->json[t->start] != '-' || t->end - t->start > 4) { d->error = ZT_ERR_PROTOCOL; return 0; }
    unsigned n = 0;
    for (unsigned i = t->start + 1; i < t->end; ++i) n = n * 10 + d->json[i] - '0';
    if (!n || n > 127) d->error = ZT_ERR_PROTOCOL;
    return -(int)n;
}

static size_t string_value(decoder_t *d, int index, char *out, size_t capacity, size_t min)
{
    if (!kind(d, index, J_STRING) || !capacity) return 0;
    const zt_json_token_t *t = &d->ws->tokens[index];
    size_t n = 0;
    for (unsigned i = t->start + 1; i < t->end - 1u; ++i) {
        unsigned char c = d->json[i];
        if (c == '\\') {
            c = d->json[++i];
            if (c == 'u') {
                unsigned v = 0;
                for (unsigned j = 0; j < 4; ++j) v = (v << 4) | (unsigned)hex_digit(d->json[++i]);
                c = (unsigned char)v;
            } else if (c != '"' && c != '\\' && c != '/') {
                d->error = ZT_ERR_PROTOCOL;
                return 0;
            }
        }
        if (c < ZT_ASCII_PRINTABLE_MIN || c > ZT_ASCII_PRINTABLE_MAX || n + 1 >= capacity) {
            d->error = ZT_ERR_PROTOCOL;
            return 0;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    if (n < min) d->error = ZT_ERR_PROTOCOL;
    return n;
}

static uint64_t id_value(decoder_t *d, int index, bool nullable)
{
    if (index >= 0 && nullable && d->ws->tokens[index].type == J_NULL) return 0;
    if (!kind(d, index, J_STRING)) return 0;
    const zt_json_token_t *t = &d->ws->tokens[index];
    uint64_t value = 0;
    if (zt_id_parse(d->json + t->start + 1, t->end - t->start - 2, &value) != ZT_OK)
        d->error = ZT_ERR_PROTOCOL;
    return value;
}

static void mac_value(decoder_t *d, int index, zt_mac_t *out)
{
    if (!kind(d, index, J_STRING)) return;
    const zt_json_token_t *t = &d->ws->tokens[index];
    if (zt_mac_parse(d->json + t->start + 1, t->end - t->start - 2, out) != ZT_OK)
        d->error = ZT_ERR_PROTOCOL;
}

static void event_id_value(decoder_t *d, int index, zt_event_id_t *out)
{
    if (!kind(d, index, J_STRING)) return;
    const zt_json_token_t *t = &d->ws->tokens[index];
    if (zt_event_id_parse(d->json + t->start + 1, t->end - t->start - 2, out) != ZT_OK)
        d->error = ZT_ERR_PROTOCOL;
}

static int enum_value(decoder_t *d, int index, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (equal(d, index, names[i])) return (int)i;
    d->error = ZT_ERR_PROTOCOL;
    return 0;
}

static zt_phase_t phase_field(decoder_t *d, int object)
{
    static const char *const names[] = {"lobby", "prepared", "running", "expired_pending_sync", "final"};
    return (zt_phase_t)enum_value(d, field(d, object, "phase"), names, 5);
}

static zt_role_t role_value(decoder_t *d, int index)
{
    static const char *const names[] = {"H", "Z"};
    return (zt_role_t)enum_value(d, index, names, 2);
}

static zt_role_t winner_value(decoder_t *d, int index)
{
    if (index >= 0 && d->ws->tokens[index].type == J_NULL) return ZT_ROLE_UNKNOWN;
    return role_value(d, index);
}

static zt_cause_t cause_value(decoder_t *d, int index)
{
    zt_cause_t cause = {.slot = ZT_SLOT_INVALID};
    if (index >= 0 && d->ws->tokens[index].type == J_NULL) return cause;
    cause.slot = uint_field(d, index, "slot", ZT_MAX_PLAYERS - 1);
    cause.seq = uint_field(d, index, "seq", UINT16_MAX);
    return cause;
}

static int array_field(decoder_t *d, int object, const char *name, unsigned maximum, uint8_t *count)
{
    int index = field(d, object, name);
    if (!kind(d, index, J_ARRAY)) return -1;
    unsigned n = d->ws->tokens[index].child_count;
    if (n > maximum) { d->error = ZT_ERR_PROTOCOL; return -1; }
    *count = n;
    return index;
}

static int next_child(decoder_t *d, int array, int after)
{
    if (array < 0) return -1;
    for (int i = after + 1; i < d->ws->count && d->ws->tokens[i].start < d->ws->tokens[array].end; ++i)
        if (d->ws->tokens[i].parent == array) return i;
    return -1;
}

static void rules_fields(decoder_t *d, int object, zt_wire_prepare_round_args_t *rules)
{
    rules->duration_ms = uint_field(d, object, "duration_ms", UINT32_MAX);
    rules->tag_rssi = rssi_field(d, object);
    rules->tag_cooldown_ms = uint_field(d, object, "tag_cooldown_ms", UINT16_MAX);
    if (rules->duration_ms != ZT_ROUND_DURATION_MS || rules->tag_cooldown_ms != ZT_TAG_COOLDOWN_MS)
        d->error = ZT_ERR_PROTOCOL;
}

static void snapshot_fields(decoder_t *d, zt_server_snapshot_t *s, bool allow_lobby)
{
    s->round_id = id_value(d, field(d, 0, "round_id"), true);
    s->snapshot_id = uint_field(d, 0, "snapshot_id", UINT32_MAX);
    s->state_rev = uint_field(d, 0, "state_rev", UINT32_MAX);
    s->roster_hash = id_value(d, field(d, 0, "roster_hash"), false);
    s->page_index = uint_field(d, 0, "page_index", ZT_SNAPSHOT_MAX_PAGES - 1);
    s->page_count = uint_field(d, 0, "page_count", ZT_SNAPSHOT_MAX_PAGES);
    s->phase = phase_field(d, 0);
    s->start_time_ms = uint_field(d, 0, "start_time_ms", UINT64_MAX);
    s->end_time_ms = uint_field(d, 0, "end_time_ms", UINT64_MAX);
    s->patient_zero_slot = uint_field(d, 0, "patient_zero_slot", UINT8_MAX);
    s->round_channel = uint_field(d, 0, "round_channel", 11);
    int result = field(d, 0, "result_present");
    if (result >= 0) s->result_present = bool_field(d, 0, "result_present");
    if (s->result_present) {
        s->winner = winner_value(d, field(d, 0, "winner"));
        s->result_final = bool_field(d, 0, "result_final");
        s->result_complete = bool_field(d, 0, "result_complete");
        s->missing_slots_bitmap = uint_field(d, 0, "missing_slots_bitmap", (1u << ZT_MAX_PLAYERS) - 1);
        s->effective_elapsed_ms = uint_field(d, 0, "effective_elapsed_ms", ZT_ROUND_DURATION_MS);
        if (s->phase < ZT_PHASE_EXPIRED_PENDING_SYNC ||
            s->result_final != (s->phase == ZT_PHASE_FINAL) ||
            (s->result_complete && (!s->result_final || s->missing_slots_bitmap)))
            d->error = ZT_ERR_PROTOCOL;
    }
    s->rules.snapshot_rev = s->snapshot_id;
    s->rules.roster_hash = s->roster_hash;
    s->rules.roster_count = uint_field(d, 0, "roster_count", ZT_MAX_PLAYERS);
    s->rules.channel = s->round_channel;
    rules_fields(d, field(d, 0, "rules"), &s->rules);
    int players = array_field(d, 0, "players", ZT_GATEWAY_SNAPSHOT_ENTRIES, &s->entry_count);
    int item = players;
    uint32_t slots = 0;
    for (unsigned i = 0; i < s->entry_count; ++i) {
        item = next_child(d, players, item);
        zt_wire_roster_entry_t *roster = &s->roster[i];
        zt_wire_role_entry_t *role = &s->roles[i];
        roster->slot = uint_field(d, item, "slot", ZT_MAX_PLAYERS - 1);
        if (slots & (1u << roster->slot)) d->error = ZT_ERR_PROTOCOL;
        slots |= 1u << roster->slot;
        mac_value(d, field(d, item, "id"), &roster->mac);
        char name[ZT_NAME_BUFFER_BYTES];
        roster->name_len = string_value(d, field(d, item, "name"), name, sizeof(name), ZT_NAME_MIN_LEN);
        memcpy(roster->name, name, roster->name_len);
        role->slot = roster->slot;
        role->role = role_value(d, field(d, item, "role"));
        role->role_rev = uint_field(d, item, "role_rev", UINT16_MAX);
        zt_cause_t cause = cause_value(d, field(d, item, "cause"));
        role->cause_slot = cause.slot;
        role->cause_seq = cause.seq;
        role->covered_seq = uint_field(d, item, "covered_seq", UINT16_MAX);
        if (role->role == ZT_ROLE_HUMAN && (cause.slot != ZT_SLOT_INVALID || cause.seq)) d->error = ZT_ERR_PROTOCOL;
        if (role->role == ZT_ROLE_ZOMBIE && cause.slot == ZT_SLOT_INVALID) d->error = ZT_ERR_PROTOCOL;
    }
    if (!s->round_id) {
        if (!allow_lobby || s->phase != ZT_PHASE_LOBBY || s->entry_count || s->rules.roster_count ||
            s->snapshot_id || s->roster_hash || s->start_time_ms || s->end_time_ms ||
            s->page_index || s->page_count != 1 || s->round_channel || s->patient_zero_slot != ZT_SLOT_INVALID)
            d->error = ZT_ERR_PROTOCOL;
        return;
    }
    unsigned total = s->rules.roster_count;
    unsigned pages = (total + ZT_GATEWAY_SNAPSHOT_ENTRIES - 1) / ZT_GATEWAY_SNAPSHOT_ENTRIES;
    if (total < ZT_MIN_ROSTER_PLAYERS || !s->snapshot_id || !s->state_rev || !s->round_channel ||
        s->page_count != pages || s->page_index >= pages ||
        (s->patient_zero_slot >= ZT_MAX_PLAYERS && s->patient_zero_slot != ZT_SLOT_INVALID)) {
        d->error = ZT_ERR_PROTOCOL;
        return;
    }
    unsigned remaining = total - s->page_index * ZT_GATEWAY_SNAPSHOT_ENTRIES;
    unsigned expected = remaining < ZT_GATEWAY_SNAPSHOT_ENTRIES ? remaining : ZT_GATEWAY_SNAPSHOT_ENTRIES;
    if (s->entry_count != expected) d->error = ZT_ERR_PROTOCOL;
    if (s->phase >= ZT_PHASE_RUNNING && (!s->start_time_ms || s->patient_zero_slot == ZT_SLOT_INVALID ||
        s->start_time_ms > UINT64_MAX - s->rules.duration_ms ||
        s->end_time_ms != s->start_time_ms + s->rules.duration_ms)) d->error = ZT_ERR_PROTOCOL;
}

static void command_fields(decoder_t *d, int object, zt_gateway_command_t *c)
{
    c->seq = uint_field(d, object, "seq", UINT32_MAX);
    c->round_id = id_value(d, field(d, object, "round_id"), false);
    c->target = uint_field(d, object, "target", UINT8_MAX);
    c->valid_until_elapsed_ms = uint_field(d, object, "valid_until_elapsed_ms", UINT32_MAX);
    if (!c->seq || !c->round_id || (c->target >= ZT_MAX_PLAYERS && c->target != ZT_SLOT_ALL)) d->error = ZT_ERR_PROTOCOL;
    int type = field(d, object, "type");
    if (equal(d, type, "PREPARE_ROUND")) {
        c->type = ZT_CMD_PREPARE_ROUND;
        zt_wire_prepare_round_args_t *p = &c->args.prepare;
        p->snapshot_rev = uint_field(d, object, "snapshot_id", UINT32_MAX);
        p->roster_hash = id_value(d, field(d, object, "roster_hash"), false);
        p->roster_count = uint_field(d, object, "roster_count", ZT_MAX_PLAYERS);
        p->channel = uint_field(d, object, "channel", 11);
        rules_fields(d, object, p);
        if (!p->snapshot_rev || p->roster_count < ZT_MIN_ROSTER_PLAYERS || !p->channel) d->error = ZT_ERR_PROTOCOL;
    } else if (equal(d, type, "START_ROUND")) {
        c->type = ZT_CMD_START_ROUND;
        c->args.start.snapshot_id = uint_field(d, object, "snapshot_id", UINT32_MAX);
        c->args.start.roster_hash = id_value(d, field(d, object, "roster_hash"), false);
        c->args.start.patient_zero_slot = uint_field(d, object, "patient_zero_slot", ZT_MAX_PLAYERS - 1);
        c->args.start.initial_role_rev = uint_field(d, object, "initial_role_rev", UINT16_MAX);
        c->args.start.start_time_ms = uint_field(d, object, "start_time_ms", UINT64_MAX);
        c->args.start.duration_ms = uint_field(d, object, "duration_ms", UINT32_MAX);
        if (!c->args.start.snapshot_id || !c->args.start.initial_role_rev || !c->args.start.start_time_ms ||
            c->args.start.duration_ms != ZT_ROUND_DURATION_MS) d->error = ZT_ERR_PROTOCOL;
    } else if (equal(d, type, "ROLE_SET")) {
        c->type = ZT_CMD_ROLE_SET;
        zt_wire_role_set_args_t *r = &c->args.role_set;
        r->role = role_value(d, field(d, object, "role"));
        r->role_rev = uint_field(d, object, "role_rev", UINT16_MAX);
        zt_cause_t cause = cause_value(d, field(d, object, "cause"));
        r->cause_slot = cause.slot;
        r->cause_seq = cause.seq;
        r->covered_seq = uint_field(d, object, "covered_seq", UINT16_MAX);
        if (c->target >= ZT_MAX_PLAYERS || !r->role_rev ||
            c->valid_until_elapsed_ms != UINT32_MAX ||
            (r->role == ZT_ROLE_HUMAN && (cause.slot != ZT_SLOT_INVALID || cause.seq)) ||
            (r->role == ZT_ROLE_ZOMBIE && (cause.slot != c->target || cause.seq > r->covered_seq)))
            d->error = ZT_ERR_PROTOCOL;
    } else if (equal(d, type, "ANNOUNCE")) {
        c->type = ZT_CMD_ANNOUNCE;
        char text[ZT_ANNOUNCE_MAX_LEN + 1];
        size_t len = string_value(d, field(d, object, "text"), text, sizeof(text), ZT_ANNOUNCE_MIN_LEN);
        c->args.announce.text_len = len;
        memcpy(c->args.announce.text, text, len);
        if (c->valid_until_elapsed_ms == ZT_COMMAND_NO_EXPIRY) d->error = ZT_ERR_PROTOCOL;
    } else if (equal(d, type, "RESET_GAME")) {
        c->type = ZT_CMD_RESET_GAME;
        if (c->target >= ZT_MAX_PLAYERS || c->valid_until_elapsed_ms != UINT32_MAX)
            d->error = ZT_ERR_PROTOCOL;
        int target_mac = field(d, object, "target_mac");
        int registration_id = field(d, object, "registration_id");
        /* Lobby cleanup targets a registration instance, so a delayed reset
         * cannot clear a later registration that reused the same slot. */
        if (target_mac >= 0 || registration_id >= 0) {
            mac_value(d, target_mac, &c->args.reset.target_mac);
            c->args.reset.registration_id = id_value(d, registration_id, false);
            if (!c->args.reset.registration_id) d->error = ZT_ERR_PROTOCOL;
        }
    } else if (equal(d, type, "END_ROUND")) {
        static const char *const reasons[] = {"time_limit", "all_infected", "operator_stop"};
        c->type = ZT_CMD_END_ROUND;
        c->args.end.effective_elapsed_ms = uint_field(d, object, "effective_elapsed_ms", ZT_ROUND_DURATION_MS);
        c->args.end.reason = enum_value(d, field(d, object, "reason"), reasons, 3);
        c->args.end.winner = winner_value(d, field(d, object, "winner"));
        c->args.end.provisional = bool_field(d, object, "provisional");
        if (c->valid_until_elapsed_ms != UINT32_MAX) d->error = ZT_ERR_PROTOCOL;
    } else if (equal(d, type, "FINAL_RESULT")) {
        c->type = ZT_CMD_FINAL_RESULT;
        c->args.final_result.winner = winner_value(d, field(d, object, "winner"));
        c->args.final_result.complete = bool_field(d, object, "complete");
        c->args.final_result.missing_slots_bitmap = uint_field(d, object, "missing_slots_bitmap", (1u << ZT_MAX_PLAYERS) - 1);
        c->args.final_result.state_rev = uint_field(d, object, "state_rev", UINT32_MAX);
        if (c->valid_until_elapsed_ms != UINT32_MAX || !c->args.final_result.state_rev ||
            (c->args.final_result.complete && c->args.final_result.missing_slots_bitmap)) d->error = ZT_ERR_PROTOCOL;
    } else {
        /* Do not silently apply or acknowledge unsupported command kinds. */
        d->error = ZT_ERR_NOT_IMPLEMENTED;
    }
}

static const char *const message_types[] = {
    "hello", "events", "ack", "need", "time_sync", "welcome", "receipts",
    "decisions", "commands", "snapshot", "need_events", "time_sync_reply", "error", "diagnostics"
};

zt_err_t zt_gateway_decode(const char *json, size_t len, zt_json_workspace_t *workspace, zt_gateway_message_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    decoder_t d;
    zt_err_t err = begin_decode(json, len, workspace, &d);
    if (err != ZT_OK) return err;
    out->envelope.v = uint_field(&d, 0, "v", UINT8_MAX);
    if (d.error != ZT_OK) return d.error;
    if (out->envelope.v != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    out->envelope.t = enum_value(&d, field(&d, 0, "t"), message_types, sizeof(message_types) / sizeof(message_types[0]));
    out->envelope.id = uint_field(&d, 0, "id", UINT32_MAX);
    out->envelope.ts = uint_field(&d, 0, "ts", UINT64_MAX);
    if (d.error != ZT_OK) return d.error;
    switch (out->envelope.t) {
    case ZT_GATEWAY_WELCOME: {
        zt_gateway_welcome_t *w = &out->body.welcome;
        static const char *const resumes[] = {"ok", "reset"};
        w->server_time_ms = uint_field(&d, 0, "server_time_ms", UINT64_MAX);
        w->resume = enum_value(&d, field(&d, 0, "resume"), resumes, 2);
        w->phase = phase_field(&d, 0);
        w->round_id = id_value(&d, field(&d, 0, "round_id"), true);
        w->state_rev = uint_field(&d, 0, "state_rev", UINT32_MAX);
        w->resume_from = uint_field(&d, 0, "resume_from", UINT32_MAX);
        w->snapshot_id = uint_field(&d, 0, "snapshot_id", UINT32_MAX);
        w->snapshot_pages = uint_field(&d, 0, "snapshot_pages", ZT_SNAPSHOT_MAX_PAGES);
        int resetting = field(&d, 0, "resetting");
        if (resetting >= 0) {
            if (workspace->tokens[resetting].type != J_TRUE && workspace->tokens[resetting].type != J_FALSE)
                d.error = ZT_ERR_PROTOCOL;
            else w->resetting = workspace->tokens[resetting].type == J_TRUE;
        }
        int diagnostics = field(&d, 0, "diagnostics");
        if (diagnostics >= 0) {
            if (workspace->tokens[diagnostics].type != J_TRUE && workspace->tokens[diagnostics].type != J_FALSE)
                d.error = ZT_ERR_PROTOCOL;
            else w->diagnostics = workspace->tokens[diagnostics].type == J_TRUE;
        }
        if (!w->server_time_ms || (!w->round_id && w->phase != ZT_PHASE_LOBBY) ||
            (w->round_id && !w->resetting && (!w->snapshot_id || !w->snapshot_pages))) d.error = ZT_ERR_PROTOCOL;
        break;
    }
    case ZT_GATEWAY_SNAPSHOT:
        snapshot_fields(&d, &out->body.snapshot, false);
        out->body.snapshot.server_id = out->envelope.id;
        break;
    case ZT_GATEWAY_RECEIPTS: {
        zt_gateway_receipts_t *r = &out->body.receipts;
        int array = array_field(&d, 0, "received_events", ZT_GATEWAY_RECEIPTS_MAX, &r->count);
        int item = array;
        for (unsigned i = 0; i < r->count; ++i) {
            item = next_child(&d, array, item);
            event_id_value(&d, item, &r->ids[i]);
            if (d.error != ZT_OK) break;
        }
        break;
    }
    case ZT_GATEWAY_DECISIONS: {
        static const char *const statuses[] = {"accepted", "rejected", "pending_dependency"};
        static const char *const reasons[] = {"WRONG_ROUND", "NOT_ROSTERED", "INVALID_PARENT",
            "OUTSIDE_ROUND", "DUPLICATE_CONFLICT", "STALE_ROLE", "INVALID_PAYLOAD"};
        zt_gateway_decisions_t *decisions = &out->body.decisions;
        int array = array_field(&d, 0, "decisions", ZT_GATEWAY_DECISIONS_MAX, &decisions->count);
        int item = array;
        for (unsigned i = 0; i < decisions->count; ++i) {
            item = next_child(&d, array, item);
            zt_gateway_decision_t *decision = &decisions->entries[i];
            event_id_value(&d, field(&d, item, "id"), &decision->id);
            decision->status = enum_value(&d, field(&d, item, "status"), statuses, 3);
            int reason = field(&d, item, "reason");
            if (decision->status == ZT_DECISION_REJECTED)
                decision->reason = (zt_decision_reason_t)(1 + enum_value(&d, reason, reasons, 7));
            else if (kind(&d, reason, J_NULL)) decision->reason = ZT_REASON_NONE;
            int archived = field(&d, item, "archived");
            if (archived >= 0) {
                if (workspace->tokens[archived].type != J_TRUE && workspace->tokens[archived].type != J_FALSE)
                    d.error = ZT_ERR_PROTOCOL;
                else decision->archived = workspace->tokens[archived].type == J_TRUE;
            }
            if (d.error != ZT_OK) break;
        }
        break;
    }
    case ZT_GATEWAY_NEED_EVENTS: {
        zt_gateway_need_events_t *n = &out->body.need_events;
        int array = array_field(&d, 0, "need_events", ZT_GATEWAY_NEED_EVENTS_MAX, &n->count);
        int item = array;
        for (unsigned i = 0; i < n->count; ++i) {
            item = next_child(&d, array, item);
            event_id_value(&d, item, &n->events[i]);
            if (d.error != ZT_OK) break;
        }
        break;
    }
    case ZT_GATEWAY_COMMANDS: {
        zt_gateway_commands_t *c = &out->body.commands;
        c->state_rev = uint_field(&d, 0, "state_rev", UINT32_MAX);
        int array = array_field(&d, 0, "commands", ZT_GATEWAY_COMMANDS_MAX, &c->count);
        int item = array;
        for (unsigned i = 0; i < c->count; ++i) {
            item = next_child(&d, array, item);
            command_fields(&d, item, &c->entries[i]);
            if (d.error != ZT_OK) break;
        }
        break;
    }
    case ZT_GATEWAY_TIME_SYNC_REPLY:
        out->body.time_sync_reply.nonce = uint_field(&d, 0, "nonce", UINT32_MAX);
        out->body.time_sync_reply.server_time_ms = uint_field(&d, 0, "server_time_ms", UINT64_MAX);
        if (!out->body.time_sync_reply.server_time_ms) d.error = ZT_ERR_PROTOCOL;
        break;
    case ZT_GATEWAY_ERROR: {
        static const char *const codes[] = {"INVALID_PAYLOAD", "WRONG_ROUND", "REGISTRATION_CLOSED", "RATE_LIMITED", "TOO_LARGE", "CURSOR_EXPIRED", "INTERNAL"};
        out->body.error.code = enum_value(&d, field(&d, 0, "code"), codes, 7);
        int detail = field(&d, 0, "detail");
        if (kind(&d, detail, J_STRING)) {
            const zt_json_token_t *t = &workspace->tokens[detail];
            out->body.error.detail.offset = t->start + 1;
            out->body.error.detail.len = t->end - t->start - 2;
        }
        int fatal = field(&d, 0, "fatal");
        if (fatal < 0 || (workspace->tokens[fatal].type != J_TRUE && workspace->tokens[fatal].type != J_FALSE))
            d.error = ZT_ERR_PROTOCOL;
        else out->body.error.fatal = workspace->tokens[fatal].type == J_TRUE;
        break;
    }
    default:
        return ZT_ERR_NOT_IMPLEMENTED;
    }
    return d.error;
}

zt_err_t zt_gateway_decode_bootstrap(const char *json, size_t len, zt_json_workspace_t *workspace, zt_bootstrap_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    decoder_t d;
    zt_err_t err = begin_decode(json, len, workspace, &d);
    if (err != ZT_OK) return err;
    uint64_t v = uint_field(&d, 0, "v", UINT8_MAX);
    if (d.error != ZT_OK) return d.error;
    if (v != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    out->game_id = id_value(&d, field(&d, 0, "game_id"), false);
    mac_value(&d, field(&d, 0, "host_id"), &out->host_id);
    out->server_time_ms = uint_field(&d, 0, "server_time_ms", UINT64_MAX);
    out->max_players = uint_field(&d, 0, "max_players", ZT_MAX_PLAYERS);
    string_value(&d, field(&d, 0, "socket_path"), out->socket_path, sizeof(out->socket_path), 1);
    snapshot_fields(&d, &out->first_page, true);
    int next = field(&d, 0, "next_page");
    if (next < 0) d.error = ZT_ERR_PROTOCOL;
    else if (workspace->tokens[next].type != J_NULL) {
        out->has_next_page = 1;
        out->next_page = number(&d, next, ZT_SNAPSHOT_MAX_PAGES - 1);
        if (out->next_page != out->first_page.page_index + 1 || out->next_page >= out->first_page.page_count)
            d.error = ZT_ERR_PROTOCOL;
    } else if (out->first_page.page_index + 1 < out->first_page.page_count) d.error = ZT_ERR_PROTOCOL;
    if (!out->game_id || !out->server_time_ms || out->max_players < ZT_MIN_ROSTER_PLAYERS) d.error = ZT_ERR_PROTOCOL;
    return d.error;
}

zt_err_t zt_gateway_decode_registration(const char *json, size_t len, zt_json_workspace_t *workspace, zt_registration_response_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->slot = ZT_SLOT_INVALID;
    decoder_t d;
    zt_err_t err = begin_decode(json, len, workspace, &d);
    if (err != ZT_OK) return err;
    uint64_t v = uint_field(&d, 0, "v", UINT8_MAX);
    if (d.error != ZT_OK) return d.error;
    if (v != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    int status = field(&d, 0, "status");
    if (equal(&d, status, "registered") || equal(&d, status, "rejoined")) {
        out->status = equal(&d, status, "registered") ? ZT_JOIN_REGISTERED : ZT_JOIN_REJOINED;
        out->slot = uint_field(&d, 0, "slot", ZT_MAX_PLAYERS - 1);
        out->round_id = id_value(&d, field(&d, 0, "round_id"), true);
        out->state_rev = uint_field(&d, 0, "state_rev", UINT32_MAX);
        if (!out->state_rev) d.error = ZT_ERR_PROTOCOL;
    } else {
        int code = field(&d, 0, "code");
        if (equal(&d, code, "REGISTRATION_CLOSED")) out->status = ZT_JOIN_REGISTRATION_CLOSED;
        else if (equal(&d, code, "ROOM_FULL")) out->status = ZT_JOIN_ROOM_FULL;
        else if (equal(&d, code, "BAD_CONFIGURATION")) out->status = ZT_JOIN_BAD_CONFIGURATION;
        else d.error = ZT_ERR_PROTOCOL;
        int round = field(&d, 0, "round_id"), rev = field(&d, 0, "state_rev");
        if (round >= 0) out->round_id = id_value(&d, round, true);
        if (rev >= 0) out->state_rev = number(&d, rev, UINT32_MAX);
    }
    return d.error;
}

zt_err_t zt_gateway_decode_control(const char *json, size_t len, zt_json_workspace_t *workspace, zt_gateway_control_response_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    decoder_t d;
    zt_err_t err = begin_decode(json, len, workspace, &d);
    if (err != ZT_OK) return err;
    uint64_t v = uint_field(&d, 0, "v", UINT8_MAX);
    if (d.error != ZT_OK) return d.error;
    if (v != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    if (!bool_field(&d, 0, "accepted")) d.error = ZT_ERR_PROTOCOL;
    int action = field(&d, 0, "action");
    if (equal(&d, action, "start")) out->action = ZT_HOST_CONTROL_START;
    else if (equal(&d, action, "reset")) out->action = ZT_HOST_CONTROL_RESET;
    else d.error = ZT_ERR_PROTOCOL;
    out->request_id = id_value(&d, field(&d, 0, "request_id"), false);
    out->round_id = id_value(&d, field(&d, 0, "round_id"), true);
    out->state_rev = uint_field(&d, 0, "state_rev", UINT32_MAX);
    if (!out->request_id || !out->state_rev || (out->action == ZT_HOST_CONTROL_START && !out->round_id) ||
        (out->action == ZT_HOST_CONTROL_RESET && out->round_id)) d.error = ZT_ERR_PROTOCOL;
    return d.error;
}

zt_err_t zt_gateway_decode_control_rejection(const char *json, size_t len, zt_json_workspace_t *workspace,
    const zt_gateway_control_request_t *request, zt_err_t *reason)
{
    if (!request || !reason || !request->request_id ||
        (request->action != ZT_HOST_CONTROL_START && request->action != ZT_HOST_CONTROL_RESET)) return ZT_ERR_INVALID_ARG;
    *reason = ZT_ERR_CONFLICT;
    decoder_t d;
    zt_err_t err = begin_decode(json, len, workspace, &d);
    if (err != ZT_OK) return err;
    uint64_t version = uint_field(&d, 0, "v", UINT8_MAX);
    if (d.error != ZT_OK) return d.error;
    if (version != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    if (bool_field(&d, 0, "accepted")) d.error = ZT_ERR_PROTOCOL;
    const char *action = request->action == ZT_HOST_CONTROL_START ? "start" : "reset";
    if (!equal(&d, field(&d, 0, "action"), action) ||
        id_value(&d, field(&d, 0, "request_id"), false) != request->request_id) d.error = ZT_ERR_PROTOCOL;
    /* A rejection describes the server's current round, which may differ from
     * the requested round. Validate its shape without treating it as state. */
    (void)id_value(&d, field(&d, 0, "round_id"), true);
    if (!uint_field(&d, 0, "state_rev", UINT32_MAX)) d.error = ZT_ERR_PROTOCOL;
    int code = field(&d, 0, "code");
    if (!kind(&d, code, J_STRING) || d.error != ZT_OK) return d.error;
    if (equal(&d, code, "STALE_REGISTRATION") || equal(&d, code, "HOST_NOT_REGISTERED"))
        *reason = ZT_ERR_HOST_REGISTRATION;
    else if (equal(&d, code, "ROSTER_SIZE")) *reason = ZT_ERR_ROSTER_SIZE;
    else if (equal(&d, code, "ROUND_ACTIVE")) *reason = ZT_ERR_ROUND_ACTIVE;
    else if (equal(&d, code, "WRONG_ROUND")) *reason = ZT_ERR_STALE;
    return ZT_OK;
}

typedef struct { char *out; size_t capacity, len; zt_err_t error; } writer_t;

static void writef(writer_t *w, const char *format, ...)
{
    if (w->error != ZT_OK) return;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(w->out + w->len, w->capacity - w->len, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->capacity - w->len || (size_t)n > ZT_GATEWAY_MESSAGE_MAX_BYTES - w->len) {
        w->error = ZT_ERR_NO_SPACE;
        return;
    }
    w->len += n;
}

static void write_string(writer_t *w, const char *value, size_t capacity, size_t min)
{
    size_t n = strnlen(value, capacity);
    if (n == capacity || n < min) { w->error = ZT_ERR_INVALID_ARG; return; }
    writef(w, "\"");
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = value[i];
        if (c < ZT_ASCII_PRINTABLE_MIN || c > ZT_ASCII_PRINTABLE_MAX) { w->error = ZT_ERR_INVALID_ARG; return; }
        if (c == '"' || c == '\\') writef(w, "\\%c", c);
        else writef(w, "%c", c);
    }
    writef(w, "\"");
}

static void write_id(writer_t *w, uint64_t value, bool nullable)
{
    if (!value && nullable) writef(w, "null");
    else writef(w, "\"%016" PRIx64 "\"", value);
}

static void write_mac(writer_t *w, const zt_mac_t *mac)
{
    char text[ZT_MAC_TEXT_BYTES];
    if (zt_mac_format(mac, text, sizeof(text)) != ZT_OK) { w->error = ZT_ERR_INVALID_ARG; return; }
    writef(w, "\"%s\"", text);
}

static void write_cause(writer_t *w, zt_role_t role, const zt_cause_t *cause)
{
    if (role == ZT_ROLE_HUMAN && cause->slot == ZT_SLOT_INVALID && !cause->seq) writef(w, "null");
    else if (role == ZT_ROLE_ZOMBIE && cause->slot < ZT_MAX_PLAYERS)
        writef(w, "{\"slot\":%u,\"seq\":%u}", cause->slot, cause->seq);
    else w->error = ZT_ERR_INVALID_ARG;
}

static zt_err_t end_write(writer_t *w, size_t *written)
{
    if (w->error != ZT_OK) { w->out[0] = '\0'; return w->error; }
    *written = w->len;
    return ZT_OK;
}

zt_err_t zt_gateway_encode_registration(const zt_registration_request_t *request, char *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!request || !out || !capacity || !written) return ZT_ERR_INVALID_ARG;
    writer_t w = {.out = out, .capacity = capacity, .error = ZT_OK};
    out[0] = '\0';
    writef(&w, "{\"v\":1,\"badge_id\":");
    write_mac(&w, &request->badge_id);
    writef(&w, ",\"name\":");
    write_string(&w, request->name, sizeof(request->name), ZT_NAME_MIN_LEN);
    writef(&w, ",\"known_round_id\":");
    write_id(&w, request->known_round_id, true);
    writef(&w, ",\"fw\":");
    write_string(&w, request->fw, sizeof(request->fw), ZT_BUILD_ID_LEN);
    writef(&w, "}");
    return end_write(&w, written);
}

zt_err_t zt_gateway_encode_control(const zt_gateway_control_request_t *request, char *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!request || !out || !capacity || !written) return ZT_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!request->request_id || !request->registration_id ||
        (request->action != ZT_HOST_CONTROL_START && request->action != ZT_HOST_CONTROL_RESET) ||
        (request->action == ZT_HOST_CONTROL_START && request->round_id) ||
        (request->action == ZT_HOST_CONTROL_RESET && !request->round_id)) return ZT_ERR_INVALID_ARG;
    writer_t w = {.out = out, .capacity = capacity, .error = ZT_OK};
    writef(&w, "{\"v\":1,\"action\":\"%s\",\"request_id\":", request->action == ZT_HOST_CONTROL_START ? "start" : "reset");
    write_id(&w, request->request_id, false);
    writef(&w, ",\"registration_id\":"); write_id(&w, request->registration_id, false);
    writef(&w, ",\"round_id\":"); write_id(&w, request->round_id, true);
    writef(&w, "}");
    return end_write(&w, written);
}

static void write_events(writer_t *w, const zt_gateway_events_t *events)
{
    if (!events->round_id || !events->count || events->count > ZT_GATEWAY_EVENTS_MAX) {
        w->error = ZT_ERR_INVALID_ARG;
        return;
    }
    writef(w, ",\"round_id\":"); write_id(w, events->round_id, false);
    writef(w, ",\"events\":[");
    for (unsigned i = 0; i < events->count; ++i) {
        const zt_gateway_event_t *event = &events->events[i];
        const zt_wire_event_t *e = &event->body;
        char id[ZT_EVENT_ID_TEXT_BYTES];
        if (event->id.round_id != events->round_id || event->id.victim_slot != e->victim_slot ||
            event->id.event_seq != e->event_seq || e->actor_slot >= ZT_MAX_PLAYERS ||
            e->actor_slot == e->victim_slot || zt_event_id_format(&event->id, id, sizeof(id)) != ZT_OK) {
            w->error = ZT_ERR_INVALID_ARG;
            return;
        }
        /* Preserve the journal's immutable evidence exactly. Temporal and
         * causal adjudication belongs to the backend, including rejections. */
        writef(w, "%s{\"id\":\"%s\",\"victim\":%u,\"seq\":%u,\"actor\":%u,"
               "\"actor_cause\":{\"slot\":%u,\"seq\":%u},\"actor_role_rev\":%u,"
               "\"victim_role_rev\":%u,\"attempt\":{\"boot\":",
               i ? "," : "", id, e->victim_slot, e->event_seq, e->actor_slot,
               e->actor_slot, e->actor_cause_seq, e->actor_role_rev, e->victim_prior_role_rev);
        write_id(w, e->request_boot, false);
        writef(w, ",\"seq\":%" PRIu32 "},\"elapsed_ms\":%" PRIu32 ",\"uncertainty_ms\":%u,"
               "\"actor_rssi\":%d,\"victim_rssi\":%d}",
               e->request_seq, e->occurred_elapsed_ms, e->uncertainty_ms,
               e->tagger_observed_victim_rssi, e->victim_observed_tagger_rssi);
    }
    writef(w, "]");
}

static void write_ack(writer_t *w, const zt_gateway_ack_t *ack)
{
    static const char *const results[] = {"received", "applied", "prepared_ready", "rejected", "requires_snapshot"};
    if ((!ack->round_id && (ack->applied_count || ack->decision_applied_count ||
         ack->ready_count || ack->round_closed_count || ack->presence_count)) ||
        ack->applied_count > ZT_GATEWAY_ACK_ARRAY_MAX ||
        ack->decision_applied_count > ZT_GATEWAY_ACK_ARRAY_MAX || ack->ready_count > ZT_GATEWAY_ACK_ARRAY_MAX ||
        ack->round_closed_count > ZT_GATEWAY_ACK_ARRAY_MAX || ack->presence_count > ZT_GATEWAY_ACK_ARRAY_MAX) {
        w->error = ZT_ERR_INVALID_ARG;
        return;
    }
    writef(w, ",\"round_id\":"); write_id(w, ack->round_id, true);
    writef(w, ",\"applied\":[");
    for (unsigned i = 0; i < ack->applied_count; ++i) {
        const zt_gateway_applied_t *a = &ack->applied[i];
        if (a->slot >= ZT_MAX_PLAYERS || (unsigned)a->result > ZT_RECEIPT_REQUIRES_SNAPSHOT || !a->seq) { w->error = ZT_ERR_INVALID_ARG; return; }
        writef(w, "%s{\"seq\":%" PRIu32 ",\"slot\":%u,\"result\":\"%s\",\"state_rev\":%" PRIu32 "}",
               i ? "," : "", a->seq, a->slot, results[a->result], a->state_rev);
    }
    writef(w, "],\"decision_applied\":[");
    for (unsigned i = 0; i < ack->decision_applied_count; ++i) {
        const zt_gateway_decision_applied_t *a = &ack->decision_applied[i];
        if (a->slot >= ZT_MAX_PLAYERS) { w->error = ZT_ERR_INVALID_ARG; return; }
        writef(w, "%s{\"slot\":%u,\"through_seq\":%u}", i ? "," : "", a->slot, a->through_seq);
    }
    writef(w, "],\"ready\":[");
    for (unsigned i = 0; i < ack->ready_count; ++i) {
        const zt_gateway_ready_t *a = &ack->ready[i];
        if (a->slot >= ZT_MAX_PLAYERS || !a->snapshot_id || a->round_id != ack->round_id) { w->error = ZT_ERR_INVALID_ARG; return; }
        writef(w, "%s{\"slot\":%u,\"snapshot_id\":%" PRIu32 ",\"round_id\":", i ? "," : "", a->slot, a->snapshot_id);
        write_id(w, a->round_id, false); writef(w, "}");
    }
    writef(w, "],\"round_closed\":[");
    for (unsigned i = 0; i < ack->round_closed_count; ++i) {
        const zt_gateway_closed_t *a = &ack->round_closed[i];
        if (a->slot >= ZT_MAX_PLAYERS) { w->error = ZT_ERR_INVALID_ARG; return; }
        writef(w, "%s{\"slot\":%u,\"produced_seq\":%u}", i ? "," : "", a->slot, a->produced_seq);
    }
    writef(w, "],\"presence\":[");
    for (unsigned i = 0; i < ack->presence_count; ++i) {
        const zt_gateway_presence_t *a = &ack->presence[i];
        if (a->slot >= ZT_MAX_PLAYERS || a->role > ZT_ROLE_ZOMBIE || a->via_hops > ZT_MAX_HOPS) { w->error = ZT_ERR_INVALID_ARG; return; }
        writef(w, "%s{\"slot\":%u,\"role\":\"%s\",\"cause\":", i ? "," : "", a->slot, a->role == ZT_ROLE_HUMAN ? "H" : "Z");
        write_cause(w, a->role, &a->cause);
        writef(w, ",\"role_rev\":%u,\"produced_seq\":%u,\"decided_seq\":%u,\"age_ms\":%" PRIu32 ",\"via_hops\":%u}",
               a->role_rev, a->produced_seq, a->decided_seq, a->age_ms, a->via_hops);
    }
    writef(w, "]");
}

zt_err_t zt_gateway_encode(const zt_gateway_message_t *message, char *out, size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!message || !out || !capacity || !written) return ZT_ERR_INVALID_ARG;
    out[0] = '\0';
    if (message->envelope.v != ZT_GATEWAY_SCHEMA_VERSION) return ZT_ERR_UNSUPPORTED_VERSION;
    if (!message->envelope.id) return ZT_ERR_INVALID_ARG;
    if (message->envelope.t != ZT_GATEWAY_HELLO && message->envelope.t != ZT_GATEWAY_EVENTS &&
        message->envelope.t != ZT_GATEWAY_ACK &&
        message->envelope.t != ZT_GATEWAY_NEED && message->envelope.t != ZT_GATEWAY_TIME_SYNC &&
        message->envelope.t != ZT_GATEWAY_DIAGNOSTICS)
        return ZT_ERR_NOT_IMPLEMENTED;
    writer_t w = {.out = out, .capacity = capacity, .error = ZT_OK};
    writef(&w, "{\"v\":1,\"t\":\"%s\",\"id\":%" PRIu32 ",\"ts\":%" PRIu64,
           message_types[message->envelope.t], message->envelope.id, message->envelope.ts);
    switch (message->envelope.t) {
    case ZT_GATEWAY_HELLO: {
        const zt_gateway_hello_t *h = &message->body.hello;
        if (!h->game_id || !h->host_boot || h->proto != ZT_PROTOCOL_VERSION || h->channel < 1 || h->channel > 11 || h->decided_count > ZT_MAX_PLAYERS) {
            w.error = ZT_ERR_INVALID_ARG;
            break;
        }
        writef(&w, ",\"host_id\":"); write_mac(&w, &h->host_id);
        writef(&w, ",\"host_boot\":"); write_id(&w, h->host_boot, false);
        writef(&w, ",\"game_id\":"); write_id(&w, h->game_id, false);
        writef(&w, ",\"round_id\":"); write_id(&w, h->round_id, true);
        if (h->registration_id) {
            writef(&w, ",\"registration_id\":"); write_id(&w, h->registration_id, false);
        }
        writef(&w, ",\"proto\":%u,\"fw\":", h->proto); write_string(&w, h->fw, sizeof(h->fw), ZT_BUILD_ID_LEN);
        writef(&w, ",\"channel\":%u,\"last_server_id\":%" PRIu32 ",\"state_rev\":%" PRIu32 ",\"pending_events\":%u,\"decided_through\":[",
               h->channel, h->last_server_id, h->state_rev, h->pending_events);
        uint32_t slots = 0;
        for (unsigned i = 0; i < h->decided_count; ++i) {
            const zt_gateway_frontier_t *f = &h->decided_through[i];
            if (f->slot >= ZT_MAX_PLAYERS || (slots & (1u << f->slot))) { w.error = ZT_ERR_INVALID_ARG; break; }
            slots |= 1u << f->slot;
            writef(&w, "%s{\"slot\":%u,\"seq\":%u}", i ? "," : "", f->slot, f->seq);
        }
        writef(&w, "]");
        break;
    }
    case ZT_GATEWAY_EVENTS:
        write_events(&w, &message->body.events);
        break;
    case ZT_GATEWAY_ACK:
        write_ack(&w, &message->body.ack);
        break;
    case ZT_GATEWAY_NEED: {
        const zt_gateway_need_t *n = &message->body.need;
        if (!n->round_id || n->event_count > ZT_GATEWAY_NEED_EVENTS_MAX ||
            (n->wants_snapshot && (n->event_count || !n->snapshot_id || n->page_index >= ZT_SNAPSHOT_MAX_PAGES))) {
            w.error = ZT_ERR_INVALID_ARG;
            break;
        }
        writef(&w, ",\"round_id\":"); write_id(&w, n->round_id, false);
        if (n->wants_snapshot) writef(&w, ",\"snapshot_page\":{\"snapshot_id\":%" PRIu32 ",\"page_index\":%u}", n->snapshot_id, n->page_index);
        else {
            writef(&w, ",\"events\":[");
            for (unsigned i = 0; i < n->event_count; ++i) {
                char id[ZT_EVENT_ID_TEXT_BYTES];
                if (n->events[i].round_id != n->round_id || zt_event_id_format(&n->events[i], id, sizeof(id)) != ZT_OK) {
                    w.error = ZT_ERR_INVALID_ARG;
                    break;
                }
                writef(&w, "%s\"%s\"", i ? "," : "", id);
            }
            writef(&w, "]");
        }
        break;
    }
    case ZT_GATEWAY_TIME_SYNC:
        writef(&w, ",\"nonce\":%" PRIu32, message->body.time_sync.nonce);
        break;
    case ZT_GATEWAY_DIAGNOSTICS: {
        const zt_gateway_diagnostics_t *d = &message->body.diagnostics;
        if (!d->host_boot || !d->seq || d->uptime_ms > UINT64_C(9007199254740991) ||
            (unsigned)d->last_error > ZT_ERR_MAX || d->fw[ZT_BUILD_ID_LEN]) {
            w.error = ZT_ERR_INVALID_ARG;
            break;
        }
        for (unsigned i = 0; i < ZT_BUILD_ID_LEN; ++i) {
            char c = d->fw[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                w.error = ZT_ERR_INVALID_ARG;
        }
        writef(&w, ",\"round_id\":"); write_id(&w, d->round_id, true);
        writef(&w, ",\"host_boot\":"); write_id(&w, d->host_boot, false);
        writef(&w, ",\"fw\":"); write_string(&w, d->fw, sizeof(d->fw), ZT_BUILD_ID_LEN);
        writef(&w, ",\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu64
            ",\"reconnects\":%" PRIu32 ",\"failures\":%" PRIu32
            ",\"dropped_messages\":%" PRIu32 ",\"send_failures\":%" PRIu32
            ",\"heap_free_bytes\":%" PRIu32 ",\"heap_min_free_bytes\":%" PRIu32
            ",\"heap_largest_free_bytes\":%" PRIu32 ",\"gateway_stack_free_bytes\":%" PRIu32
            ",\"websocket_stack_free_bytes\":%" PRIu32 ",\"last_error\":%u",
            d->seq, d->uptime_ms, d->reconnects, d->failures, d->dropped_messages, d->send_failures,
            d->heap_free_bytes, d->heap_min_free_bytes, d->heap_largest_free_bytes,
            d->gateway_stack_free_bytes, d->websocket_stack_free_bytes, (unsigned)d->last_error);
        break;
    }
    default:
        return ZT_ERR_NOT_IMPLEMENTED;
    }
    writef(&w, "}");
    if (message->envelope.t == ZT_GATEWAY_DIAGNOSTICS && w.len > ZT_GATEWAY_DIAGNOSTICS_MAX_BYTES)
        w.error = ZT_ERR_NO_SPACE;
    return end_write(&w, written);
}
