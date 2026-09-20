#include "zt_store.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

/* Development badges only: no archive binding and not commissionable. */
#ifndef ZT_STORE_SELF_INSTALL
#define ZT_STORE_SELF_INSTALL 0
#endif
#if ZT_STORE_SELF_INSTALL
#define ZT_INSTALL_FLAG_SELF_INSTALLED UINT32_C(2)
#endif

/* Only the persistence service performs NVS mutations. Public readers use the
 * immutable in-memory journal; initialization/recovery runs before gameplay. */
typedef struct { zt_round_id_t round; zt_wire_event_t event; } cached_event_t;
static cached_event_t journal[ZT_JOURNAL_CAPACITY];
static zt_persist_request_t work;
static zt_checkpoint_t scratch;
static uint8_t blob[ZT_CHECKPOINT_MAX_BYTES];
static zt_store_status_t status;
static zt_mac_t device_mac;
static const esp_partition_t *partition;
static nvs_handle_t handle;
static bool opened, attempted_install;
static uint8_t work_state; /* 0 empty, 1 queued, 2 executing, 3 completion */
static zt_persist_completion_t completion;
static zt_persist_sink_t completion_sink;
static void *completion_context;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t io_lock_storage;
static SemaphoreHandle_t io_lock;
static TaskHandle_t persist_owner;
static uint16_t value_sizes[ZT_JOURNAL_CAPACITY + 6];
static uint8_t expected_keys[ZT_JOURNAL_CAPACITY + 6];
static zt_reset_receipt_t last_reset;
static uint64_t reset_epoch;
static bool reset_complete;
#define RESET_SIZE_INDEX (ZT_JOURNAL_CAPACITY + 5)
static const char *const round_keys[2] = { ZT_STORE_KEY_ROUND_A, ZT_STORE_KEY_ROUND_B };
static const char *const decision_keys[2] = { ZT_STORE_KEY_DECISION_A, ZT_STORE_KEY_DECISION_B };
static zt_round_id_t rounds[2];
static uint64_t round_epochs[2];
static zt_slot_t own_slots[2] = { ZT_SLOT_INVALID, ZT_SLOT_INVALID };
static uint16_t own_decided[2];
static uint8_t result_present[2], work_result_present;
static zt_wire_final_result_args_t results[2], work_result;

static uint64_t get_le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t crc(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static bool filled(const uint8_t *p, size_t n, uint8_t byte)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != byte) return false;
    return true;
}
static zt_err_t storage_error(void)
{
    portENTER_CRITICAL(&guard);
    status.last_error = ZT_ERR_STORAGE;
    status.install_state = ZT_INSTALL_ERROR;
    portEXIT_CRITICAL(&guard);
    return ZT_ERR_STORAGE;
}
static bool same_mac(const zt_mac_t *a, const zt_mac_t *b)
{
    return memcmp(a->bytes, b->bytes, ZT_MAC_BYTES) == 0;
}
static bool install_valid(const zt_install_header_t *h)
{
    return memcmp(h->magic, ZT_INSTALL_MAGIC, 4) == 0 &&
        h->schema == ZT_INSTALL_HEADER_SCHEMA && h->length == ZT_INSTALL_BYTES &&
        !h->reserved && h->nvs_offset == ZT_NVS_OFFSET && h->nvs_length == ZT_NVS_LENGTH &&
        h->flags == ZT_INSTALL_FLAG_INITIALIZED &&
        !filled(h->installation_uuid, 16, 0) && !filled(h->installation_uuid, 16, 255) &&
        !filled(h->baseline_sha256, 32, 0) && !filled(h->baseline_sha256, 32, 255);
}
#if ZT_STORE_SELF_INSTALL
static bool self_install_valid(const zt_install_header_t *h)
{
    return memcmp(h->magic, ZT_INSTALL_MAGIC, 4) == 0 &&
        h->schema == ZT_INSTALL_HEADER_SCHEMA && h->length == ZT_INSTALL_BYTES &&
        !h->reserved && h->nvs_offset == ZT_NVS_OFFSET && h->nvs_length == ZT_NVS_LENGTH &&
        h->flags == (ZT_INSTALL_FLAG_INITIALIZED | ZT_INSTALL_FLAG_SELF_INSTALLED) &&
        filled(h->installation_uuid, 16, 0) && filled(h->baseline_sha256, 32, 0);
}
#endif
zt_err_t zt_store_encode_install(const zt_install_header_t *h, uint8_t *out, size_t cap, size_t *written)
{
    if (!h || !out || !written) return ZT_ERR_INVALID_ARG;
    *written = 0;
    if (cap < ZT_INSTALL_BYTES) return ZT_ERR_INVALID_LENGTH;
    if (!install_valid(h)) return ZT_ERR_PROTOCOL;
    memset(out, 0, ZT_INSTALL_BYTES);
    memcpy(out, ZT_INSTALL_MAGIC, 4);
    put_le(out + 4, h->schema, 2); put_le(out + 6, ZT_INSTALL_BYTES, 2);
    memcpy(out + 8, h->mac.bytes, 6); memcpy(out + 16, h->installation_uuid, 16);
    memcpy(out + 32, h->baseline_sha256, 32);
    put_le(out + 64, ZT_NVS_OFFSET, 4); put_le(out + 68, ZT_NVS_LENGTH, 4);
    put_le(out + 72, ZT_INSTALL_FLAG_INITIALIZED, 4); put_le(out + 76, crc(out, 76), 4);
    *written = ZT_INSTALL_BYTES;
    return ZT_OK;
}
zt_err_t zt_store_decode_install(const uint8_t *buf, size_t len, zt_install_header_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    if (len != ZT_INSTALL_BYTES) return ZT_ERR_INVALID_LENGTH;
    if (get_le(buf + 76, 4) != crc(buf, 76)) return ZT_ERR_STORAGE;
    zt_install_header_t h = {0};
    memcpy(h.magic, buf, 4); h.schema = get_le(buf + 4, 2); h.length = get_le(buf + 6, 2);
    memcpy(h.mac.bytes, buf + 8, 6); h.reserved = get_le(buf + 14, 2);
    memcpy(h.installation_uuid, buf + 16, 16); memcpy(h.baseline_sha256, buf + 32, 32);
    h.nvs_offset = get_le(buf + 64, 4); h.nvs_length = get_le(buf + 68, 4);
    h.flags = get_le(buf + 72, 4); h.crc32 = get_le(buf + 76, 4);
    if (!install_valid(&h)
#if ZT_STORE_SELF_INSTALL
        && !self_install_valid(&h)
#endif
    ) return ZT_ERR_STORAGE;
    *out = h;
    return ZT_OK;
}
zt_err_t zt_store_encode_event(const zt_durable_event_t *e, uint8_t *out, size_t cap, size_t *written)
{
    if (!e || !out || !written) return ZT_ERR_INVALID_ARG;
    *written = 0;
    if (cap < ZT_DURABLE_EVENT_BYTES) return ZT_ERR_INVALID_LENGTH;
    if (memcmp(e->magic, "ZTEV", 4) || e->schema != ZT_STORE_SCHEMA_VERSION ||
        e->length != ZT_DURABLE_EVENT_BYTES || !e->round_id || !filled(e->reserved, 14, 0)) return ZT_ERR_PROTOCOL;
    memset(out, 0, ZT_DURABLE_EVENT_BYTES);
    memcpy(out, "ZTEV", 4); put_le(out + 4, e->schema, 2); put_le(out + 6, 64, 2);
    put_le(out + 8, e->round_id, 8);
    size_t n;
    zt_err_t r = zt_wire_encode_event(&e->event, out + 16, 30, &n);
    if (r != ZT_OK || n != 30) return r == ZT_OK ? ZT_ERR_PROTOCOL : r;
    put_le(out + 60, crc(out, 60), 4); *written = 64;
    return ZT_OK;
}
zt_err_t zt_store_decode_event(const uint8_t *buf, size_t len, zt_durable_event_t *out)
{
    if (!buf || !out) return ZT_ERR_INVALID_ARG;
    if (len != 64) return ZT_ERR_INVALID_LENGTH;
    if (memcmp(buf, "ZTEV", 4) || get_le(buf + 4, 2) != ZT_STORE_SCHEMA_VERSION ||
        get_le(buf + 6, 2) != 64 || !get_le(buf + 8, 8) || !filled(buf + 46, 14, 0) ||
        get_le(buf + 60, 4) != crc(buf, 60)) return ZT_ERR_STORAGE;
    zt_durable_event_t e = {0};
    memcpy(e.magic, buf, 4); e.schema = get_le(buf + 4, 2); e.length = 64;
    e.round_id = get_le(buf + 8, 8); e.crc32 = get_le(buf + 60, 4);
    zt_err_t r = zt_wire_decode_event(buf + 16, 30, &e.event);
    if (r != ZT_OK) return ZT_ERR_STORAGE;
    *out = e;
    return ZT_OK;
}

/* Inspect the actual flash table, not a generated CSV or runtime descriptor.
 * Each raw entry includes all flags and sixteen label bytes. The SDK has
 * already verified the table checksum while booting; no fifth entry is allowed. */
static zt_err_t layout(void)
{
    uint32_t size;
    if (esp_flash_get_physical_size(NULL, &size) != ESP_OK || size != ZT_FLASH_BYTES) return storage_error();
    static const struct { uint8_t type, subtype; uint32_t offset, length; char label[16]; } expected[] = {
        {1, 2, 0x9000, 0x4000, "nvs"}, {1, 1, 0xd000, 0x1000, "phy_init"},
        {0, 0, 0x10000, 0x2a0000, "factory"}, {1, 0x83, 0x2b0000, 0x140000, "storage"}
    };
    uint8_t entry[32];
    for (unsigned i = 0; i < 4; ++i) {
        if (esp_flash_read(NULL, entry, 0x8000 + 32 * i, 32) != ESP_OK ||
            get_le(entry, 2) != 0x50aa || entry[2] != expected[i].type || entry[3] != expected[i].subtype ||
            get_le(entry + 4, 4) != expected[i].offset || get_le(entry + 8, 4) != expected[i].length ||
            memcmp(entry + 12, expected[i].label, 16) || get_le(entry + 28, 4)) return storage_error();
    }
    for (unsigned i = 4; i < 0xc00 / 32; ++i) {
        if (esp_flash_read(NULL, entry, 0x8000 + 32 * i, 32) != ESP_OK) return storage_error();
        if (i == 4 && get_le(entry, 2) == 0xebeb && filled(entry + 2, 14, 255)) continue;
        if (!filled(entry, 32, 255)) return storage_error();
    }
    return ZT_OK;
}
static zt_err_t blank_range(uint32_t begin, uint32_t end, bool *blank)
{
    uint8_t bytes[256];
    *blank = true;
    for (uint32_t pos = begin; pos < end; pos += sizeof(bytes)) {
        uint32_t n = end - pos < sizeof(bytes) ? end - pos : sizeof(bytes);
        if (esp_flash_read(NULL, bytes, pos, n) != ESP_OK) return storage_error();
        if (!filled(bytes, n, 255)) *blank = false;
    }
    return ZT_OK;
}
static zt_err_t inspect(const zt_mac_t *mac, zt_store_status_t *out)
{
    if (!mac || !out) return ZT_ERR_INVALID_ARG;
    if (opened) { *out = status; return same_mac(mac, &device_mac) ? status.last_error : ZT_ERR_STORAGE; }
    zt_mac_t physical;
    if (esp_read_mac(physical.bytes, ESP_MAC_WIFI_STA) != ESP_OK || !same_mac(mac, &physical) || layout() != ZT_OK) {
        storage_error(); *out = status; return ZT_ERR_STORAGE;
    }
    device_mac = physical;
    uint8_t raw[ZT_INSTALL_BYTES];
    bool blank;
    if (esp_flash_read(NULL, raw, ZT_INSTALL_OFFSET, sizeof(raw)) != ESP_OK) goto bad;
    if (filled(raw, sizeof(raw), 255)) {
        if (blank_range(ZT_INSTALL_OFFSET, ZT_FLASH_BYTES, &blank) != ZT_OK || !blank) goto bad;
        status = (zt_store_status_t){ .install_state = ZT_INSTALL_WAIT };
    } else {
        zt_install_header_t h;
        if (zt_store_decode_install(raw, sizeof(raw), &h) != ZT_OK || !same_mac(&h.mac, mac) ||
            blank_range(ZT_INSTALL_OFFSET + ZT_INSTALL_BYTES, ZT_NVS_OFFSET, &blank) != ZT_OK || !blank) goto bad;
        status = (zt_store_status_t){ .install_state = ZT_INSTALL_VALID };
    }
    *out = status;
    return ZT_OK;
 bad:
    storage_error(); *out = status; return ZT_ERR_STORAGE;
}
static zt_err_t register_nvs(void)
{
    if (partition) return ZT_OK;
    if (esp_partition_register_external(NULL, 0x3F1000, 0xF000, "zt_nvs",
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &partition) != ESP_OK) return storage_error();
    return ZT_OK;
}
static zt_err_t commit_install(const uint8_t raw[ZT_INSTALL_BYTES])
{
    attempted_install = true;
    if (register_nvs() != ZT_OK || nvs_flash_init_partition("zt_nvs") != ESP_OK) return storage_error();
    /* Named init alone is lazy in IDF 5.5.3. Creating the namespace makes
     * initialization durable before the raw installation commit. */
    nvs_handle_t installation;
    if (nvs_open_from_partition("zt_nvs",ZT_NVS_NAMESPACE,NVS_READWRITE,&installation)!=ESP_OK) return storage_error();
    esp_err_t committed=nvs_commit(installation); nvs_close(installation);
    if (committed!=ESP_OK) return storage_error();
    /* The sole raw write: constant address, constant length, once, after NVS.
     * An interrupted attempt can only be recovered by the operator tool. */
    if (esp_flash_write(NULL, raw, ZT_INSTALL_OFFSET, ZT_INSTALL_BYTES) != ESP_OK) return storage_error();
    uint8_t verify[ZT_INSTALL_BYTES];
    if (esp_flash_read(NULL, verify, ZT_INSTALL_OFFSET, sizeof(verify)) != ESP_OK || memcmp(raw, verify, sizeof(verify))) return storage_error();
    status.install_state = ZT_INSTALL_VALID;
    return ZT_OK;
}
zt_err_t zt_store_inspect(const zt_mac_t *mac, zt_store_status_t *out)
{
    zt_err_t r = inspect(mac, out);
#if ZT_STORE_SELF_INSTALL
    if (r == ZT_OK && out->install_state == ZT_INSTALL_WAIT) {
        if (attempted_install) r = storage_error();
        else {
            /* inspect() proved ALL 64 KiB blank. Zero UUID/hash are reserved
             * sentinels, never a tool-issued identity or verified image hash.
             * The public encoder/install-init remain tool-header-only. */
            uint8_t raw[ZT_INSTALL_BYTES] = {0};
            memcpy(raw, ZT_INSTALL_MAGIC, 4);
            put_le(raw + 4, ZT_INSTALL_HEADER_SCHEMA, 2); put_le(raw + 6, ZT_INSTALL_BYTES, 2);
            memcpy(raw + 8, mac->bytes, ZT_MAC_BYTES);
            put_le(raw + 64, ZT_NVS_OFFSET, 4); put_le(raw + 68, ZT_NVS_LENGTH, 4);
            put_le(raw + 72, ZT_INSTALL_FLAG_INITIALIZED | ZT_INSTALL_FLAG_SELF_INSTALLED, 4);
            put_le(raw + 76, crc(raw, 76), 4);
            r = commit_install(raw);
        }
        *out = status;
    }
#endif
    return r;
}
zt_err_t zt_store_install_init(const zt_install_header_t *header)
{
    if (!header) return ZT_ERR_INVALID_ARG;
    if (opened || attempted_install || !install_valid(header)) return ZT_ERR_INVALID_STATE;
    zt_store_status_t s;
    /* Explicit tool installation must never trigger the development path. */
    zt_err_t r = inspect(&header->mac, &s);
    if (r != ZT_OK || s.install_state != ZT_INSTALL_WAIT) return ZT_ERR_STORAGE;
    uint8_t raw[ZT_INSTALL_BYTES]; size_t n;
    r = zt_store_encode_install(header, raw, sizeof(raw), &n);
    if (r != ZT_OK) return r;
    return commit_install(raw);
}

/* Internal blob framing: magic, schema, total length, explicit LE fields, CRC.
 * Cursor bounds apply in both directions; padding in decoded C values is never
 * serialized. These formats are private to schema 1, not radio envelopes. */
typedef struct { uint8_t *p; size_t at, end; bool reading, ok; } codec_t;
static void field(codec_t *c, void *p, size_t n)
{
    if (!c->ok || n > c->end - c->at) { c->ok = false; return; }
    if (c->reading) memcpy(p, c->p + c->at, n); else memcpy(c->p + c->at, p, n);
    c->at += n;
}
static uint64_t number(codec_t *c, uint64_t value, unsigned n)
{
    if (!c->ok || n > c->end - c->at) { c->ok = false; return 0; }
    if (c->reading) value = get_le(c->p + c->at, n); else put_le(c->p + c->at, value, n);
    c->at += n;
    return value;
}
#define NUM(c, obj, n) ((obj) = number((c), (obj), (n)))
static void role_codec(codec_t *c, zt_wire_role_entry_t *r)
{
    NUM(c,r->slot,1); NUM(c,r->role,1); NUM(c,r->role_rev,2); NUM(c,r->cause_slot,1);
    NUM(c,r->cause_seq,2); NUM(c,r->covered_seq,2); NUM(c,r->flags,1);
}
static void config_codec(codec_t *c, zt_config_t *v)
{
    field(c,v->expected_mac.bytes,6); field(c,v->name,sizeof(v->name)); NUM(c,v->game_id,8);
    field(c,v->group_key,sizeof(v->group_key)); field(c,v->host_mac.bytes,6);
    NUM(c,v->last_channel,1); NUM(c,v->host_credentials_present,1);
    field(c,v->https_base,sizeof(v->https_base)); field(c,v->ssid,sizeof(v->ssid));
    field(c,v->password,sizeof(v->password)); field(c,v->token,sizeof(v->token)); NUM(c,v->provisioned_time_ms,8);
}
static void checkpoint_codec(codec_t *c, zt_checkpoint_t *v)
{
    NUM(c,v->round_id,8); NUM(c,v->snapshot_rev,4); NUM(c,v->state_rev,4); NUM(c,v->roster_hash,8);
    NUM(c,v->roster_count,1); NUM(c,v->patient_zero_slot,1); NUM(c,v->round_channel,1); NUM(c,v->phase,1);
    for (unsigned i=0;i<ZT_MAX_PLAYERS;++i) {
        zt_wire_roster_entry_t *r=&v->roster[i];
        NUM(c,r->slot,1); field(c,r->mac.bytes,6); NUM(c,r->name_len,1); field(c,r->name,12);
        role_codec(c,&v->roles[i]);
    }
    NUM(c,v->rules.snapshot_rev,4); NUM(c,v->rules.roster_hash,8); NUM(c,v->rules.roster_count,1);
    NUM(c,v->rules.duration_ms,4); NUM(c,v->rules.channel,1); NUM(c,v->rules.tag_rssi,1); NUM(c,v->rules.tag_cooldown_ms,2);
    NUM(c,v->local_event_seq,2);
    for (unsigned i=0;i<ZT_MAX_PLAYERS;++i) { NUM(c,v->produced[i],2); NUM(c,v->received[i],2); NUM(c,v->decided[i],2); }
    for (unsigned i=0;i<ZT_PENDING_COMMAND_CAPACITY;++i) NUM(c,v->applied_command_seq[i],4);
    NUM(c,v->last_server_applied_id,4); field(c,v->journal_slots,ZT_JOURNAL_CAPACITY); NUM(c,v->journal_slot_count,2);
    for (unsigned i=0;i<ZT_MAX_PLAYERS;++i) role_codec(c,&v->pending_roles[i]);
    NUM(c,v->closed_bitmap,4); NUM(c,v->cleared_bitmap,4);
}
static bool text_valid(const char *s, size_t cap, size_t min, bool printable)
{
    size_t n = strnlen(s,cap);
    if (n < min || n == cap) return false;
    for (size_t i=0;i<n;++i) if (printable && ((uint8_t)s[i]<32 || (uint8_t)s[i]>126)) return false;
    return true;
}
static bool config_valid(const zt_config_t *v)
{
    if (!same_mac(&v->expected_mac,&device_mac) || !text_valid(v->name,sizeof(v->name),1,true) ||
        !v->game_id || (v->host_mac.bytes[0]&1) || filled(v->host_mac.bytes,6,0) ||
        filled(v->group_key,32,0) || v->last_channel < 1 || v->last_channel > 11 || v->host_credentials_present > 1 ||
        !text_valid(v->https_base,sizeof(v->https_base),0,true) || !text_valid(v->ssid,sizeof(v->ssid),0,false) ||
        !text_valid(v->password,sizeof(v->password),0,false) || !text_valid(v->token,sizeof(v->token),0,true)) return false;
    if (v->host_credentials_present && (!same_mac(&v->host_mac,&device_mac) || strncmp(v->https_base,"https://",8) ||
        !v->https_base[8] || !v->ssid[0] || !v->token[0])) return false;
    if (!v->host_credentials_present && (v->https_base[0] || v->ssid[0] || v->password[0] || v->token[0])) return false;
    return true;
}
static bool checkpoint_valid(const zt_checkpoint_t *v)
{
    if (!v->round_id || v->roster_count<2 || v->roster_count>20 || v->phase>ZT_PHASE_FINAL ||
        v->round_channel<1 || v->round_channel>11 || v->journal_slot_count>128 ||
        v->rules.duration_ms!=ZT_ROUND_DURATION_MS || v->rules.tag_cooldown_ms!=ZT_TAG_COOLDOWN_MS ||
        v->rules.roster_count!=v->roster_count || v->rules.roster_hash!=v->roster_hash ||
        v->rules.channel!=v->round_channel || (v->closed_bitmap & ~ZT_VALID_SLOT_BITMAP) ||
        (v->cleared_bitmap & ~ZT_VALID_SLOT_BITMAP)) return false;
    uint32_t slots=0; uint64_t hash;
    for (unsigned i=0;i<v->roster_count;++i) {
        const zt_wire_roster_entry_t *r=&v->roster[i];
        if (r->slot>=20 || (slots&(1u<<r->slot)) || r->name_len<1 || r->name_len>12 ||
            (i && v->roster[i-1].slot>=r->slot)) return false;
        slots|=1u<<r->slot;
        for (unsigned j=0;j<12;++j) if (j<r->name_len ? (r->name[j]<32 || r->name[j]>126) : r->name[j]!=0) return false;
        for (unsigned j=0;j<i;++j) if (same_mac(&r->mac,&v->roster[j].mac)) return false;
        uint8_t encoded[ZT_ROLE_ENTRY_BYTES]; size_t n;
        if (v->roles[i].slot!=r->slot || zt_wire_encode_role_entry(&v->roles[i],encoded,sizeof(encoded),&n)!=ZT_OK) return false;
    }
    if (v->phase>=ZT_PHASE_RUNNING && (v->patient_zero_slot>=20 || !(slots&(1u<<v->patient_zero_slot)))) return false;
    return zt_wire_roster_hash(v->roster,v->roster_count,&hash)==ZT_OK && hash==v->roster_hash;
}
static zt_err_t blob_get(const char *key, const char magic[4], size_t *len)
{
    *len=sizeof(blob);
    esp_err_t e=nvs_get_blob(handle,key,blob,len);
    if (e==ESP_ERR_NVS_NOT_FOUND) return ZT_ERR_NOT_FOUND;
    if (e!=ESP_OK || *len<12 || memcmp(blob,magic,4) || get_le(blob+4,2)!=ZT_STORE_SCHEMA_VERSION ||
        get_le(blob+6,2)!=*len || get_le(blob+*len-4,4)!=crc(blob,*len-4)) return storage_error();
    return ZT_OK;
}
static void blob_begin(const char magic[4]) { memset(blob,0,sizeof(blob)); memcpy(blob,magic,4); put_le(blob+4,ZT_STORE_SCHEMA_VERSION,2); }
static size_t blob_end(codec_t *c)
{
    if (!c->ok) return 0;
    size_t n=c->at+4; put_le(blob+6,n,2); put_le(blob+n-4,crc(blob,n-4),4); return n;
}
static zt_err_t stats_refresh(void)
{
    nvs_stats_t s;
    if (nvs_get_stats("zt_nvs",&s)!=ESP_OK) return storage_error();
    uint32_t live=0;
    for (unsigned i=0;i<ZT_JOURNAL_CAPACITY+6;++i) live+=value_sizes[i];
    /* Account page headers/bitmaps and the NVS-reserved GC page conservatively. */
    uint32_t allocated=(uint32_t)s.used_entries*32+(ZT_NVS_LENGTH/4096)*64;
    uint32_t available=(uint32_t)s.available_entries*32;
    portENTER_CRITICAL(&guard);
    status.live_bytes=live; status.allocated_bytes=allocated; status.free_bytes=available;
    portEXIT_CRITICAL(&guard);
    if (live>=ZT_STORE_LIVE_VALUES_LIMIT_BYTES || allocated>=ZT_STORE_ALLOCATED_LIMIT_BYTES || available<ZT_STORE_GC_RESERVE_BYTES) return storage_error();
    return ZT_OK;
}
static zt_err_t blob_set(const char *key, size_t len, unsigned size_index)
{
    if (!len || len>sizeof(blob) || size_index>=ZT_JOURNAL_CAPACITY+6) return ZT_ERR_INVALID_LENGTH;
    if (stats_refresh()!=ZT_OK) return ZT_ERR_STORAGE;
    size_t extra=((len+31)/32+3)*32;
    if (status.live_bytes-value_sizes[size_index]+len>=ZT_STORE_LIVE_VALUES_LIMIT_BYTES ||
        status.allocated_bytes+extra>=ZT_STORE_ALLOCATED_LIMIT_BYTES || status.free_bytes<ZT_STORE_GC_RESERVE_BYTES+extra) return ZT_ERR_NO_SPACE;
    if (nvs_set_blob(handle,key,blob,len)!=ESP_OK || nvs_commit(handle)!=ESP_OK) return storage_error();
    value_sizes[size_index]=len;
    return stats_refresh();
}
static void result_codec(codec_t *c,uint8_t *present,zt_wire_final_result_args_t *r)
{
    NUM(c,*present,1); NUM(c,r->winner,1); NUM(c,r->complete,1); NUM(c,r->missing_slots_bitmap,4); NUM(c,r->state_rev,4);
}
static zt_err_t checkpoint_get(unsigned slot, zt_checkpoint_t *v)
{
    size_t n; zt_err_t r=blob_get(round_keys[slot],"ZTRD",&n);
    if (r!=ZT_OK) return r;
    memset(v,0,sizeof(*v)); codec_t c={blob,8,n-4,true,true};
    uint64_t epoch=number(&c,0,8);
    uint8_t present=0; zt_wire_final_result_args_t result={0}; result_codec(&c,&present,&result); checkpoint_codec(&c,v);
    if (!epoch) return storage_error();
    round_epochs[slot]=epoch;
    if (!c.ok || c.at!=c.end || !checkpoint_valid(v) || present>1) return storage_error();
    if (present) {
        uint8_t encoded[ZT_FINAL_RESULT_ARGS_BYTES]; size_t written;
        if (v->phase!=ZT_PHASE_FINAL || zt_wire_encode_final_result_args(&result,encoded,sizeof(encoded),&written)!=ZT_OK) return storage_error();
    }
    portENTER_CRITICAL(&guard); result_present[slot]=present; results[slot]=result; portEXIT_CRITICAL(&guard);
    return ZT_OK;
}
static zt_err_t checkpoint_set(unsigned slot, zt_checkpoint_t *v)
{
    if (!checkpoint_valid(v)) return ZT_ERR_PROTOCOL;
    uint64_t epoch=round_epochs[slot];
    if (rounds[slot]!=v->round_id) {
        epoch=round_epochs[0]>round_epochs[1] ? round_epochs[0] : round_epochs[1];
        if (epoch<reset_epoch) epoch=reset_epoch;
        if (epoch==UINT64_MAX) return ZT_ERR_OVERFLOW;
        ++epoch;
    }
    blob_begin("ZTRD"); codec_t c={blob,8,sizeof(blob)-4,false,true};
    number(&c,epoch,8);
    uint8_t present=rounds[slot]==v->round_id ? result_present[slot] : 0;
    zt_wire_final_result_args_t result=present ? results[slot] : (zt_wire_final_result_args_t){0};
    if (work_result_present && work.kind==ZT_PERSIST_CHECKPOINT && work.round_id==v->round_id) { present=1; result=work_result; }
    result_codec(&c,&present,&result); checkpoint_codec(&c,v);
    size_t n=blob_end(&c); if (n>ZT_CHECKPOINT_MAX_BYTES) return ZT_ERR_INVALID_LENGTH;
    zt_err_t r=blob_set(round_keys[slot],n,1+slot);
    if (r==ZT_OK) {
        portENTER_CRITICAL(&guard);
        if (rounds[slot]!=v->round_id) { own_decided[slot]=0; own_slots[slot]=ZT_SLOT_INVALID; }
        rounds[slot]=v->round_id; round_epochs[slot]=epoch; result_present[slot]=present; results[slot]=result;
        for (unsigned i=0;i<v->roster_count;++i) if (same_mac(&v->roster[i].mac,&device_mac)) {
            own_slots[slot]=v->roster[i].slot;
            if (own_decided[slot]<v->decided[own_slots[slot]]) own_decided[slot]=v->decided[own_slots[slot]];
        }
        portEXIT_CRITICAL(&guard);
    }
    return r;
}
static int round_index(zt_round_id_t round)
{
    for (unsigned i=0;i<2;++i) if (rounds[i] && rounds[i]==round) return i;
    return -1;
}
static int role_index(const zt_checkpoint_t *cp, unsigned slot)
{
    for (unsigned i=0;i<cp->roster_count;++i) if (cp->roster[i].slot==slot) return i;
    return -1;
}
static void rebuild(zt_checkpoint_t *cp)
{
    cp->journal_slot_count=0;
    int self=-1;
    for (unsigned i=0;i<cp->roster_count;++i) if (same_mac(&cp->roster[i].mac,&device_mac)) self=cp->roster[i].slot;
    for (unsigned i=0;i<128;++i) if (journal[i].round==cp->round_id) {
        const zt_wire_event_t *e=&journal[i].event;
        cp->journal_slots[cp->journal_slot_count++]=i;
        if (cp->produced[e->victim_slot]<e->event_seq) cp->produced[e->victim_slot]=e->event_seq;
        if (e->victim_slot==self && cp->local_event_seq<e->event_seq) cp->local_event_seq=e->event_seq;
        int ri=role_index(cp,e->victim_slot);
        if (ri>=0 && cp->roles[ri].covered_seq<e->event_seq &&
            (cp->roles[ri].role!=ZT_ROLE_ZOMBIE || cp->roles[ri].cause_seq<e->event_seq)) {
            cp->roles[ri].role=ZT_ROLE_ZOMBIE; cp->roles[ri].cause_slot=e->victim_slot;
            cp->roles[ri].cause_seq=e->event_seq; cp->roles[ri].flags|=ZT_ROLE_FLAG_PROVISIONAL;
        }
    }
}
/* Decisions are keyed by immutable event identity, never by arrival order. */
static zt_err_t decisions_get(unsigned slot, size_t *n)
{
    zt_err_t r=blob_get(decision_keys[slot],"ZTDC",n);
    if (r!=ZT_OK) return r;
    if (*n<22 || get_le(blob+8,8)!=rounds[slot] || get_le(blob+16,2)>128 ||
        *n!=22+6*get_le(blob+16,2)) return storage_error();
    for (unsigned i=0;i<get_le(blob+16,2);++i) {
        zt_wire_decision_entry_t d;
        if (zt_wire_decode_decision_entry(blob+18+6*i,6,&d)!=ZT_OK) return storage_error();
        for (unsigned j=0;j<i;++j) if (blob[18+6*j]==d.victim_slot && get_le(blob+19+6*j,2)==d.event_seq) return storage_error();
    }
    return ZT_OK;
}
static zt_err_t apply_decisions(unsigned slot, zt_checkpoint_t *cp)
{
    size_t n; zt_err_t r=decisions_get(slot,&n);
    if (r==ZT_ERR_NOT_FOUND) return ZT_OK;
    if (r!=ZT_OK) return r;
    unsigned count=get_le(blob+16,2);
    for (unsigned i=0;i<count;++i) {
        zt_wire_decision_entry_t d;
        if (zt_wire_decode_decision_entry(blob+18+6*i,6,&d)!=ZT_OK) return storage_error();
        int ri=role_index(cp,d.victim_slot);
        if (ri>=0 && !d.archived && d.status!=ZT_DECISION_PENDING_DEPENDENCY && cp->roles[ri].cause_seq==d.event_seq &&
            (cp->roles[ri].flags&ZT_ROLE_FLAG_PROVISIONAL)) {
            if (d.status==ZT_DECISION_REJECTED) { cp->roles[ri].role=ZT_ROLE_HUMAN; cp->roles[ri].cause_slot=ZT_SLOT_INVALID; cp->roles[ri].cause_seq=0; }
            cp->roles[ri].flags&=~ZT_ROLE_FLAG_PROVISIONAL;
            if (cp->roles[ri].covered_seq<d.event_seq) cp->roles[ri].covered_seq=d.event_seq;
        }
    }
    for (unsigned s=0;s<20;++s) {
        for (unsigned pass=0;pass<128 && cp->decided[s]<UINT16_MAX;++pass) {
            bool found=false;
            for (unsigned i=0;i<count;++i) if (blob[18+6*i]==s && get_le(blob+19+6*i,2)==cp->decided[s]+1u &&
                blob[21+6*i]!=ZT_DECISION_PENDING_DEPENDENCY) { found=true; break; }
            if (!found) break;
            ++cp->decided[s];
        }
        int ri=role_index(cp,s);
        if (ri>=0 && cp->pending_roles[ri].role_rev>cp->roles[ri].role_rev &&
            cp->pending_roles[ri].covered_seq>=cp->produced[s]) cp->roles[ri]=cp->pending_roles[ri];
    }
    if (own_slots[slot]<20) {
        portENTER_CRITICAL(&guard); own_decided[slot]=cp->decided[own_slots[slot]]; portEXIT_CRITICAL(&guard);
    }
    return ZT_OK;
}
static bool reset_valid(const zt_reset_receipt_t *r)
{
    return r->round_id && r->command_seq && r->slot<ZT_MAX_PLAYERS && r->channel>=1 && r->channel<=11;
}
static void reset_codec(codec_t *c,zt_reset_receipt_t *r,uint64_t *epoch)
{
    NUM(c,r->round_id,8); NUM(c,r->command_seq,4); NUM(c,r->slot,1); NUM(c,r->channel,1);
    NUM(c,*epoch,8);
}
static zt_err_t erase_value(const char *key,unsigned index)
{
    esp_err_t e=nvs_erase_key(handle,key);
    if ((e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) || nvs_commit(handle)!=ESP_OK) return storage_error();
    value_sizes[index]=0; expected_keys[index]=0;
    return ZT_OK;
}
/* Idempotent cleanup is authorized by the separately committed reset record.
 * Configuration and unrelated retained rounds are never erased. */
static zt_err_t reset_cleanup(void)
{
    if (!reset_valid(&last_reset)) return ZT_ERR_INVALID_ARG;
    for (unsigned i=0;i<ZT_JOURNAL_CAPACITY;++i) {
        char key[8]; snprintf(key,sizeof(key),ZT_STORE_EVENT_KEY_FORMAT,i);
        size_t n=64; esp_err_t er=nvs_get_blob(handle,key,blob,&n);
        if (er==ESP_ERR_NVS_NOT_FOUND) continue;
        zt_durable_event_t event;
        if (er!=ESP_OK || zt_store_decode_event(blob,n,&event)!=ZT_OK) return storage_error();
        if (event.round_id!=last_reset.round_id) continue;
        zt_err_t r=erase_value(key,5+i); if (r!=ZT_OK) return r;
        portENTER_CRITICAL(&guard);
        if (journal[i].round) { memset(&journal[i],0,sizeof(journal[i])); --status.event_count; }
        portEXIT_CRITICAL(&guard);
    }
    for (unsigned s=0;s<2;++s) if (rounds[s]==last_reset.round_id) {
        zt_err_t r=erase_value(decision_keys[s],3+s); if (r!=ZT_OK) return r;
        r=erase_value(round_keys[s],1+s); if (r!=ZT_OK) return r;
        portENTER_CRITICAL(&guard);
        rounds[s]=0; own_slots[s]=ZT_SLOT_INVALID; own_decided[s]=0; result_present[s]=0;
        memset(&results[s],0,sizeof(results[s]));
        portEXIT_CRITICAL(&guard);
    }
    zt_err_t r=stats_refresh();
    if (r==ZT_OK) { portENTER_CRITICAL(&guard); reset_complete=true; portEXIT_CRITICAL(&guard); }
    return r;
}
static zt_err_t commit_reset(void)
{
    const zt_reset_receipt_t *r=&work.value.reset;
    if (!reset_valid(r) || r->round_id!=work.round_id) return ZT_ERR_INVALID_ARG;
    if (last_reset.command_seq>r->command_seq) return ZT_ERR_STALE;
    if (last_reset.command_seq==r->command_seq) {
        if (r->round_id!=last_reset.round_id || r->slot!=last_reset.slot) return ZT_ERR_CONFLICT;
        return reset_cleanup();
    }
    int s=round_index(r->round_id);
    if (s>=0 && own_slots[s]<ZT_MAX_PLAYERS && r->slot!=own_slots[s]) return ZT_ERR_AUTH;
    blob_begin("ZTRS"); codec_t c={blob,8,sizeof(blob)-4,false,true};
    uint64_t epoch=round_epochs[0]>round_epochs[1] ? round_epochs[0] : round_epochs[1];
    if (epoch<reset_epoch) epoch=reset_epoch;
    zt_reset_receipt_t copy=*r; reset_codec(&c,&copy,&epoch);
    zt_err_t result=blob_set("reset",blob_end(&c),RESET_SIZE_INDEX);
    if (result!=ZT_OK) return result;
    portENTER_CRITICAL(&guard); last_reset=copy; reset_epoch=epoch; reset_complete=false; portEXIT_CRITICAL(&guard);
    return reset_cleanup();
}
static zt_err_t recover(void)
{
    memset(journal,0,sizeof(journal)); memset(value_sizes,0,sizeof(value_sizes));
    memset(rounds,0,sizeof(rounds)); status.event_count=0;
    size_t n;
    zt_err_t r=blob_get(ZT_STORE_KEY_CONFIG,"ZTCF",&n);
    if (r==ZT_OK) {
        if (n>ZT_CONFIG_MAX_BYTES) return storage_error();
        zt_config_t cfg={0}; codec_t c={blob,8,n-4,true,true}; config_codec(&c,&cfg);
        if (!c.ok || c.at!=c.end || !config_valid(&cfg)) return storage_error();
        value_sizes[0]=n; memset(&cfg,0,sizeof(cfg));
    } else if (r!=ZT_ERR_NOT_FOUND) return r;
    for (unsigned s=0;s<2;++s) {
        r=checkpoint_get(s,&scratch);
        if (r==ZT_OK) {
            rounds[s]=scratch.round_id; value_sizes[1+s]=get_le(blob+6,2);
            for (unsigned j=0;j<scratch.roster_count;++j) if (same_mac(&scratch.roster[j].mac,&device_mac)) own_slots[s]=scratch.roster[j].slot;
        }
        else if (r!=ZT_ERR_NOT_FOUND) return r;
    }
    if (rounds[0] && rounds[0]==rounds[1]) return storage_error();
    r=blob_get("reset","ZTRS",&n);
    if (r==ZT_OK) {
        memset(&last_reset,0,sizeof(last_reset)); codec_t c={blob,8,n-4,true,true}; reset_codec(&c,&last_reset,&reset_epoch);
        if (!c.ok || c.at!=c.end || !reset_valid(&last_reset)) return storage_error();
        value_sizes[RESET_SIZE_INDEX]=n;
        r=reset_cleanup(); if (r!=ZT_OK) return r;
    } else if (r!=ZT_ERR_NOT_FOUND) return r;
    for (unsigned i=0;i<128;++i) {
        char key[8]; int k=snprintf(key,sizeof(key),ZT_STORE_EVENT_KEY_FORMAT,i);
        if (k<0 || k>=(int)sizeof(key)) return storage_error();
        n=64; esp_err_t er=nvs_get_blob(handle,key,blob,&n);
        if (er==ESP_ERR_NVS_NOT_FOUND) continue;
        zt_durable_event_t e;
        if (er!=ESP_OK || zt_store_decode_event(blob,n,&e)!=ZT_OK || round_index(e.round_id)<0) return storage_error();
        for (unsigned j=0;j<i;++j) if (journal[j].round==e.round_id && journal[j].event.victim_slot==e.event.victim_slot &&
            journal[j].event.event_seq==e.event.event_seq) return storage_error();
        journal[i]=(cached_event_t){e.round_id,e.event}; value_sizes[5+i]=64; ++status.event_count;
    }
    for (unsigned s=0;s<2;++s) {
        r=decisions_get(s,&n);
        if (r==ZT_OK) { if (!rounds[s]) return storage_error(); value_sizes[3+s]=n; }
        else if (r!=ZT_ERR_NOT_FOUND) return r;
    }
    for (unsigned s=0;s<2;++s) if (rounds[s]) {
        r=checkpoint_get(s,&scratch); if (r!=ZT_OK) return r;
        rebuild(&scratch); r=apply_decisions(s,&scratch); if (r!=ZT_OK) return r;
        if (own_slots[s]<20) own_decided[s]=scratch.decided[own_slots[s]];
    }
    /* Unknown keys/types are a schema error, never silently skipped. */
    nvs_iterator_t it=NULL;
    esp_err_t er=nvs_entry_find("zt_nvs",NULL,NVS_TYPE_ANY,&it);
    while (er==ESP_OK) {
        nvs_entry_info_t info;
        if (nvs_entry_info(it,&info)!=ESP_OK) { nvs_release_iterator(it); return storage_error(); }
        bool known=false;
        if (!strcmp(info.namespace_name,ZT_NVS_NAMESPACE) && info.type==NVS_TYPE_BLOB) {
            known=!strcmp(info.key,"cfg") || !strcmp(info.key,"round_a") || !strcmp(info.key,"round_b") ||
                !strcmp(info.key,"dec_a") || !strcmp(info.key,"dec_b") || !strcmp(info.key,"reset");
            if (strlen(info.key)==6 && !memcmp(info.key,"evt",3) && info.key[3]>='0' && info.key[3]<='9' &&
                info.key[4]>='0' && info.key[4]<='9' && info.key[5]>='0' && info.key[5]<='9') {
                unsigned id=(info.key[3]-'0')*100+(info.key[4]-'0')*10+info.key[5]-'0'; known=id<128;
            }
        }
        if (!known) { nvs_release_iterator(it); return storage_error(); }
        er=nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (er!=ESP_ERR_NVS_NOT_FOUND) return storage_error();
    for (unsigned i=0;i<ZT_JOURNAL_CAPACITY+6;++i) if (expected_keys[i] && !value_sizes[i]) return storage_error();
    return stats_refresh();
}
/* Read-only preflight of the PINNED IDF 5.5.3 NVS page framing. The SDK can
 * discard corrupt entries during initialization; detect corruption first so a
 * missing cfg/event is never mistaken for an unconfigured human. Incomplete
 * uncommitted blob chunks can still undergo normal SDK recovery. */
static int key_number(const uint8_t key[16])
{
    if (!memchr(key,0,16)) return -1;
    if (!strcmp((const char *)key,"cfg")) return 0;
    if (!strcmp((const char *)key,"reset")) return RESET_SIZE_INDEX;
    for (unsigned i=0;i<2;++i) {
        if (!strcmp((const char *)key,round_keys[i])) return 1+i;
        if (!strcmp((const char *)key,decision_keys[i])) return 3+i;
    }
    if (key[0]=='e' && key[1]=='v' && key[2]=='t' && key[3]>='0' && key[3]<='9' &&
        key[4]>='0' && key[4]<='9' && key[5]>='0' && key[5]<='9' && !key[6]) {
        unsigned n=(key[3]-'0')*100+(key[4]-'0')*10+key[5]-'0';
        if (n<128) return 5+n;
    }
    return -1;
}
static unsigned entry_state(const uint8_t bitmap[32],unsigned i)
{
    return (bitmap[i/4]>>((i%4)*2))&3;
}
static zt_err_t preflight_nvs(void)
{
    memset(expected_keys,0,sizeof(expected_keys));
    uint8_t head[64], item[32], data[32];
    for (uint32_t page=ZT_NVS_OFFSET;page<ZT_FLASH_BYTES;page+=4096) {
        if (esp_flash_read(NULL,head,page,sizeof(head))!=ESP_OK) return storage_error();
        uint32_t state=get_le(head,4);
        if (state==UINT32_MAX) {
            bool blank;
            if (blank_range(page,page+4096,&blank)!=ZT_OK || !blank) return storage_error();
            continue;
        }
        if ((state!=0xfffffffeu && state!=0xfffffffcu && state!=0xfffffff8u) || head[8]!=0xfe ||
            get_le(head+28,4)!=esp_rom_crc32_le(UINT32_MAX,head+4,24)) return storage_error();
        for (unsigned i=0;i<126;) {
            unsigned es=entry_state(head+32,i);
            if (es==3) { ++i; continue; }
            if (es==1 || esp_flash_read(NULL,item,page+64+32*i,32)!=ESP_OK) return storage_error();
            uint32_t hcrc=esp_rom_crc32_le(UINT32_MAX,item,4);
            hcrc=esp_rom_crc32_le(hcrc,item+8,24);
            bool header_ok=get_le(item+4,4)==hcrc && item[2] && item[2]<=126-i;
            if (es==0) {
                /* A partially completed deletion may leave some payload entries
                 * marked written; their erased header still defines the span. */
                i+=header_ok ? item[2] : 1; continue;
            }
            if (!header_ok) return storage_error();
            if (!item[0]) {
                if (item[1]!=NVS_TYPE_U8 || item[2]!=1 || !memchr(item+8,0,16) || strcmp((const char *)item+8,ZT_NVS_NAMESPACE) || item[24]!=1) return storage_error();
            } else {
                int key=key_number(item+8);
                if (item[0]!=1 || key<0 || (item[1]!=0x48 && item[1]!=NVS_TYPE_BLOB)) return storage_error();
                if (item[1]==0x48) {
                    if (item[2]!=1) return storage_error();
                    expected_keys[key]=1;
                } else {
                    unsigned length=get_le(item+24,2);
                    if (item[2]!=1+(length+31)/32) return storage_error();
                    bool complete=true;
                    for (unsigned j=1;j<item[2];++j) if (entry_state(head+32,i+j)!=2) complete=false;
                    if (complete) {
                        uint32_t dcrc=UINT32_MAX;
                        for (unsigned pos=0;pos<length;pos+=32) {
                            unsigned take=length-pos<32 ? length-pos : 32;
                            if (esp_flash_read(NULL,data,page+64+32*(i+1)+pos,take)!=ESP_OK) return storage_error();
                            dcrc=esp_rom_crc32_le(dcrc,data,take);
                        }
                        if (dcrc!=get_le(item+28,4)) return storage_error();
                    }
                }
            }
            i+=item[2];
        }
    }
    return ZT_OK;
}
/* Operator-selected development behavior: every MCU boot starts a new local
 * session. Preserve provisioning and installation metadata, but do not replay
 * an old game, pending tag, or reset receipt after a power cycle. Named-key
 * cleanup is idempotent if power is lost before the commit completes. */
static zt_err_t forget_game_on_boot(void)
{
    bool changed=false;
    const char *keys[]={round_keys[0],round_keys[1],decision_keys[0],decision_keys[1],"reset"};
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        esp_err_t r=nvs_erase_key(handle,keys[i]);
        if (r==ESP_OK) changed=true;
        else if (r!=ESP_ERR_NVS_NOT_FOUND) return storage_error();
    }
    for (unsigned i=0;i<ZT_JOURNAL_CAPACITY;++i) {
        char key[8];
        int n=snprintf(key,sizeof(key),ZT_STORE_EVENT_KEY_FORMAT,i);
        if (n<0 || n>=(int)sizeof(key)) return storage_error();
        esp_err_t r=nvs_erase_key(handle,key);
        if (r==ESP_OK) changed=true;
        else if (r!=ESP_ERR_NVS_NOT_FOUND) return storage_error();
    }
    if (changed && nvs_commit(handle)!=ESP_OK) return storage_error();
    /* Preflight saw the previous boot's keys; they are intentionally absent. */
    memset(expected_keys+1,0,sizeof(expected_keys)-sizeof(expected_keys[0]));
    return ZT_OK;
}
zt_err_t zt_store_open(const zt_mac_t *mac, zt_persist_sink_t sink, void *context)
{
    if (!mac || !sink) return ZT_ERR_INVALID_ARG;
    if (opened) return ZT_ERR_INVALID_STATE;
    zt_store_status_t s;
    zt_err_t r=zt_store_inspect(mac,&s);
    if (r!=ZT_OK) return r;
    if (s.install_state==ZT_INSTALL_WAIT) return ZT_ERR_INVALID_STATE;
    bool blank;
    if (blank_range(ZT_NVS_OFFSET,ZT_FLASH_BYTES,&blank)!=ZT_OK || blank) return storage_error();
    if (preflight_nvs()!=ZT_OK || register_nvs()!=ZT_OK || nvs_flash_init_partition("zt_nvs")!=ESP_OK ||
        nvs_open_from_partition("zt_nvs",ZT_NVS_NAMESPACE,NVS_READWRITE,&handle)!=ESP_OK) return storage_error();
    r=forget_game_on_boot();
    if (r==ZT_OK) r=recover();
    if (r!=ZT_OK) { nvs_close(handle); return r; }
    io_lock=xSemaphoreCreateMutexStatic(&io_lock_storage);
    if (!io_lock) { nvs_close(handle); return storage_error(); }
    completion_sink=sink; completion_context=context; opened=true;
    return ZT_OK;
}
zt_err_t zt_store_load_config(zt_config_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!opened) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(io_lock,0)!=pdTRUE) return ZT_ERR_BUSY;
    size_t n; zt_err_t r=blob_get("cfg","ZTCF",&n);
    if (r==ZT_OK) {
        memset(out,0,sizeof(*out)); codec_t c={blob,8,n-4,true,true}; config_codec(&c,out);
        if (!c.ok || c.at!=c.end || !config_valid(out)) { memset(out,0,sizeof(*out)); r=storage_error(); }
    }
    xSemaphoreGive(io_lock); return r;
}
zt_err_t zt_store_load_checkpoint(zt_round_id_t round, zt_checkpoint_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!opened) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(io_lock,0)!=pdTRUE) return ZT_ERR_BUSY;
    int s=round_index(round);
    /* Round zero is the boot recovery query: newest persisted snapshot wins.
     * Admission epochs are internal durable metadata; opaque IDs are never ordered. */
    if (!round) {
        s=rounds[0] ? 0 : (rounds[1] ? 1 : -1);
        if (rounds[0] && rounds[1]) s=round_epochs[1]>round_epochs[0] ? 1 : 0;
        if (s>=0 && last_reset.round_id && round_epochs[s]<=reset_epoch) s=-1;
    }
    zt_err_t r=s<0 ? ZT_ERR_NOT_FOUND : checkpoint_get(s,out);
    if (r==ZT_OK) { rebuild(out); r=apply_decisions(s,out); }
    xSemaphoreGive(io_lock); return r;
}
zt_err_t zt_store_load_reset(zt_round_id_t round,zt_reset_receipt_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!opened) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    bool match=last_reset.round_id && (!round || round==last_reset.round_id);
    zt_err_t r=!match ? ZT_ERR_NOT_FOUND : reset_complete ? ZT_OK : ZT_ERR_BUSY;
    if (r==ZT_OK) *out=last_reset;
    portEXIT_CRITICAL(&guard);
    return r;
}
zt_err_t zt_store_read_event(zt_round_id_t round, zt_slot_t victim, uint16_t seq, zt_wire_event_t *out)
{
    if (!out || !round || victim>=20 || !seq) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    for (unsigned i=0;i<128;++i) if (journal[i].round==round && journal[i].event.victim_slot==victim && journal[i].event.event_seq==seq) {
        *out=journal[i].event; portEXIT_CRITICAL(&guard); return ZT_OK;
    }
    portEXIT_CRITICAL(&guard); return ZT_ERR_NOT_FOUND;
}
zt_err_t zt_store_read_decisions(zt_round_id_t round, uint16_t cursor,
    zt_wire_decision_entry_t *out, size_t capacity, size_t *count, uint16_t *next)
{
    if (!round || !out || !count || !next || !capacity || capacity>ZT_DECISION_PAGE_ENTRIES ||
        cursor>ZT_JOURNAL_CAPACITY) return ZT_ERR_INVALID_ARG;
    *count=0; *next=cursor;
    if (!opened) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(io_lock,0)!=pdTRUE) return ZT_ERR_BUSY;
    int s=round_index(round);
    size_t n;
    zt_err_t r=s<0 ? ZT_ERR_NOT_FOUND : decisions_get((unsigned)s,&n);
    /* A retained round may legitimately have no decisions yet. */
    if (s>=0 && r==ZT_ERR_NOT_FOUND) { *next=ZT_JOURNAL_CAPACITY; r=ZT_OK; }
    else if (r==ZT_OK) {
        unsigned total=get_le(blob+16,2), at=cursor;
        while (at<total && *count<capacity) {
            r=zt_wire_decode_decision_entry(blob+18+6*at,6,&out[*count]);
            if (r!=ZT_OK) { *count=0; r=storage_error(); break; }
            ++*count; ++at;
        }
        if (r==ZT_OK) *next=at>=total ? ZT_JOURNAL_CAPACITY : (uint16_t)at;
    }
    xSemaphoreGive(io_lock); return r;
}
zt_err_t zt_store_export(zt_round_id_t round, uint16_t cursor, uint8_t *out, size_t cap, size_t *written, uint16_t *next)
{
    if (!out || !written || !next || cursor>128 || !round) return ZT_ERR_INVALID_ARG;
    *written=0; *next=cursor;
    if (!opened) return ZT_ERR_INVALID_STATE;
    if (cap<64) return ZT_ERR_INVALID_LENGTH;
    portENTER_CRITICAL(&guard); bool retained=round_index(round)>=0; portEXIT_CRITICAL(&guard);
    if (!retained) return ZT_ERR_NOT_FOUND;
    for (unsigned i=cursor;i<128;++i) {
        cached_event_t copy;
        portENTER_CRITICAL(&guard); copy=journal[i]; portEXIT_CRITICAL(&guard);
        if (copy.round==round) {
            if (cap-*written<64) { *next=i; return ZT_OK; }
            zt_durable_event_t e={.magic={'Z','T','E','V'},.schema=ZT_STORE_SCHEMA_VERSION,.length=64,.round_id=round,.event=copy.event};
            size_t n; zt_err_t r=zt_store_encode_event(&e,out+*written,cap-*written,&n);
            if (r!=ZT_OK) return r;
            *written+=n;
        }
        *next=i+1;
    }
    return ZT_OK;
}
zt_err_t zt_store_status(zt_store_status_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard); *out=status; portEXIT_CRITICAL(&guard); return ZT_OK;
}
static zt_err_t submit_inner(const zt_persist_request_t *request,const zt_wire_final_result_args_t *final)
{
    if (!request || !request->request_id || request->kind>ZT_PERSIST_RESET_ROUND) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    if (!opened || status.install_state!=ZT_INSTALL_VALID) { portEXIT_CRITICAL(&guard); return ZT_ERR_STORAGE; }
    if (work_state) { portEXIT_CRITICAL(&guard); return ZT_ERR_BUSY; }
    work=*request; work_result_present=final!=NULL; if (final) work_result=*final; work_state=1;
    portEXIT_CRITICAL(&guard);
    if (persist_owner) xTaskNotifyGive(persist_owner);
    return ZT_OK;
}
zt_err_t zt_store_submit(const zt_persist_request_t *request)
{
    return submit_inner(request,NULL);
}
/* Final-result metadata is included in the SAME checkpoint value as its
 * applied command. It never modifies immutable event evidence. */
zt_err_t zt_store_submit_final(const zt_persist_request_t *request,const zt_wire_final_result_args_t *final);
zt_err_t zt_store_get_final(zt_round_id_t round,zt_wire_final_result_args_t *out);
zt_err_t zt_store_submit_final(const zt_persist_request_t *request,const zt_wire_final_result_args_t *final)
{
    if (!request || !final || request->kind!=ZT_PERSIST_CHECKPOINT || request->value.checkpoint.phase!=ZT_PHASE_FINAL) return ZT_ERR_INVALID_ARG;
    uint8_t encoded[ZT_FINAL_RESULT_ARGS_BYTES]; size_t n;
    if (zt_wire_encode_final_result_args(final,encoded,sizeof(encoded),&n)!=ZT_OK) return ZT_ERR_PROTOCOL;
    return submit_inner(request,final);
}
zt_err_t zt_store_get_final(zt_round_id_t round,zt_wire_final_result_args_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard); int i=round_index(round);
    if (i<0 || !result_present[i]) { portEXIT_CRITICAL(&guard); return ZT_ERR_NOT_FOUND; }
    *out=results[i]; portEXIT_CRITICAL(&guard); return ZT_OK;
}
static zt_err_t commit_event(void)
{
    if (round_index(work.round_id)<0) return ZT_ERR_INVALID_STATE;
    zt_durable_event_t e={.magic={'Z','T','E','V'},.schema=ZT_STORE_SCHEMA_VERSION,.length=64,.round_id=work.round_id,.event=work.value.event};
    uint8_t encoded[64]; size_t n;
    zt_err_t r=zt_store_encode_event(&e,encoded,sizeof(encoded),&n);
    if (r!=ZT_OK) return r;
    int empty=-1;
    for (unsigned i=0;i<128;++i) {
        if (!journal[i].round) { if (empty<0) empty=i; continue; }
        const zt_wire_event_t *old=&journal[i].event;
        if (journal[i].round==e.round_id && old->victim_slot==e.event.victim_slot && old->event_seq==e.event.event_seq) {
            zt_durable_event_t prior=e; prior.event=*old; uint8_t bytes[64];
            if (zt_store_encode_event(&prior,bytes,sizeof(bytes),&n)!=ZT_OK) return storage_error();
            return memcmp(encoded,bytes,64) ? ZT_ERR_CONFLICT : ZT_OK;
        }
        if (journal[i].round==e.round_id && old->victim_slot==e.event.victim_slot &&
            old->actor_slot==e.event.actor_slot && old->request_boot==e.event.request_boot && old->request_seq==e.event.request_seq) return ZT_ERR_CONFLICT;
    }
    if (empty<0) return ZT_ERR_NO_SPACE;
    char key[8]; int k=snprintf(key,sizeof(key),ZT_STORE_EVENT_KEY_FORMAT,(unsigned)empty);
    if (k<0 || k>=(int)sizeof(key)) return ZT_ERR_OVERFLOW;
    /* The key must actually be absent. Neither retries nor recovery overwrite it. */
    n=0; esp_err_t er=nvs_get_blob(handle,key,NULL,&n);
    if (er!=ESP_ERR_NVS_NOT_FOUND) return storage_error();
    memcpy(blob,encoded,64); r=blob_set(key,64,5+empty);
    if (r==ZT_ERR_NO_SPACE) return r;
    /* NVS may have completed the value before returning an I/O error. Resolve
     * by exact readback before reporting failure to a locked victim. */
    n=64; er=nvs_get_blob(handle,key,blob,&n);
    if (er==ESP_OK && n==64 && !memcmp(blob,encoded,64)) {
        portENTER_CRITICAL(&guard);
        journal[empty]=(cached_event_t){e.round_id,e.event}; ++status.event_count;
        portEXIT_CRITICAL(&guard); value_sizes[5+empty]=64;
        return ZT_OK;
    }
    /* Ambiguous media outcome: no definitive completion. Keep the outstanding
     * request locked until USB recovery/reboot can inspect committed evidence. */
    storage_error(); return ZT_ERR_BUSY;
}
static zt_err_t commit_decisions(unsigned s)
{
    size_t n; zt_err_t r=decisions_get(s,&n);
    bool changed=r==ZT_ERR_NOT_FOUND;
    if (r==ZT_ERR_NOT_FOUND) { blob_begin("ZTDC"); put_le(blob+8,work.round_id,8); put_le(blob+16,0,2); }
    else if (r!=ZT_OK) return r;
    unsigned count=get_le(blob+16,2);
    if (work.value.decisions.count>20) return ZT_ERR_INVALID_LENGTH;
    for (unsigned j=0;j<work.value.decisions.count;++j) {
        zt_wire_decision_entry_t *d=&work.value.decisions.entries[j]; uint8_t enc[6]; size_t written;
        if (zt_wire_encode_decision_entry(d,enc,sizeof(enc),&written)!=ZT_OK) return ZT_ERR_PROTOCOL;
        unsigned i;
        for (i=0;i<count;++i) if (blob[18+6*i]==d->victim_slot && get_le(blob+19+6*i,2)==d->event_seq) break;
        if (i==count) { if (count==128) return ZT_ERR_NO_SPACE; ++count; changed=true; }
        else if (blob[21+6*i]!=ZT_DECISION_PENDING_DEPENDENCY &&
            (blob[21+6*i]!=d->status || blob[22+6*i]!=d->reason || blob[23+6*i]>d->archived)) return ZT_ERR_CONFLICT;
        else if (memcmp(blob+18+6*i,enc,6)) changed=true;
        memcpy(blob+18+6*i,enc,6);
    }
    /* Replays still reconstruct the committed frontier, but an identical
     * decision must not rewrite flash every time a badge receipt is retried. */
    if (changed) {
        put_le(blob+16,count,2); codec_t c={blob,18+6*count,sizeof(blob)-4,false,true};
        r=blob_set(decision_keys[s],blob_end(&c),3+s);
        if (r!=ZT_OK) return r;
    }
    r=checkpoint_get(s,&scratch); if (r!=ZT_OK) return r;
    rebuild(&scratch); return apply_decisions(s,&scratch);
}
static zt_err_t archive_clear(unsigned s)
{
    zt_err_t r=checkpoint_get(s,&scratch);
    if (r!=ZT_OK) return r;
    rebuild(&scratch); r=apply_decisions(s,&scratch); if (r!=ZT_OK) return r;
    const zt_archive_clearance_t *a=&work.value.clearance;
    if (a->round_id!=work.round_id || scratch.phase<ZT_PHASE_EXPIRED_PENDING_SYNC) return ZT_ERR_INVALID_STATE;
    for (unsigned i=0;i<20;++i) if (a->produced[i]<scratch.produced[i]) return ZT_ERR_CONFLICT;
    /* Verify the operator's receipt against this exact retained export. */
    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha);
    int er=mbedtls_sha256_starts(&sha,0);
    for (unsigned i=0;i<128 && er==0;++i) if (journal[i].round==work.round_id) {
        zt_durable_event_t e={.magic={'Z','T','E','V'},.schema=1,.length=64,.round_id=work.round_id,.event=journal[i].event};
        size_t n; r=zt_store_encode_event(&e,blob,sizeof(blob),&n);
        if (r!=ZT_OK) { mbedtls_sha256_free(&sha); return r; }
        er=mbedtls_sha256_update(&sha,blob,n);
    }
    uint8_t digest[32]; if (er==0) er=mbedtls_sha256_finish(&sha,digest); mbedtls_sha256_free(&sha);
    if (er || memcmp(digest,a->export_sha256,32)) return ZT_ERR_AUTH;
    /* Clearance is persisted BEFORE deletion; an interrupted cleanup retains
     * sufficient authority/evidence and never opens a new round over it. */
    scratch.cleared_bitmap=ZT_VALID_SLOT_BITMAP;
    r=checkpoint_set(s,&scratch); if (r!=ZT_OK) return r;
    for (unsigned i=0;i<128;++i) if (journal[i].round==work.round_id) {
        char key[8]; int k=snprintf(key,sizeof(key),ZT_STORE_EVENT_KEY_FORMAT,i);
        if (k<0 || k>=(int)sizeof(key) || nvs_erase_key(handle,key)!=ESP_OK || nvs_commit(handle)!=ESP_OK) return storage_error();
        portENTER_CRITICAL(&guard); memset(&journal[i],0,sizeof(journal[i])); --status.event_count; portEXIT_CRITICAL(&guard);
        value_sizes[5+i]=0;
    }
    esp_err_t e=nvs_erase_key(handle,decision_keys[s]);
    if ((e!=ESP_OK && e!=ESP_ERR_NVS_NOT_FOUND) || nvs_commit(handle)!=ESP_OK) return storage_error();
    value_sizes[3+s]=0;
    if (nvs_erase_key(handle,round_keys[s])!=ESP_OK || nvs_commit(handle)!=ESP_OK) return storage_error();
    portENTER_CRITICAL(&guard); rounds[s]=0; own_slots[s]=ZT_SLOT_INVALID; own_decided[s]=0; portEXIT_CRITICAL(&guard); value_sizes[1+s]=0;
    return stats_refresh();
}
static zt_err_t execute(void)
{
    if (work.kind==ZT_PERSIST_RESET_ROUND) return commit_reset();
    /* A delayed mesh/snapshot write cannot resurrect explicitly cleared data. */
    if (work.kind!=ZT_PERSIST_CONFIG && work.round_id && work.round_id==last_reset.round_id)
        return ZT_ERR_STALE;
    if (work.kind==ZT_PERSIST_EVENT) return commit_event();
    if (work.kind==ZT_PERSIST_CONFIG) {
        if (!config_valid(&work.value.config)) return ZT_ERR_INVALID_ARG;
        /* Configuration changes cannot invalidate retained causal evidence. */
        if (rounds[0] || rounds[1]) return ZT_ERR_INVALID_STATE;
        blob_begin("ZTCF"); codec_t c={blob,8,ZT_CONFIG_MAX_BYTES-4,false,true}; config_codec(&c,&work.value.config);
        return blob_set("cfg",blob_end(&c),0);
    }
    int s=round_index(work.round_id);
    if (work.kind==ZT_PERSIST_CHECKPOINT) {
        if (work.value.checkpoint.round_id!=work.round_id) return ZT_ERR_INVALID_ARG;
        if (s<0) { s=!rounds[0] ? 0 : (!rounds[1] ? 1 : -1); if (s<0) return ZT_ERR_NO_SPACE; }
        scratch=work.value.checkpoint; rebuild(&scratch);
        return checkpoint_set(s,&scratch);
    }
    if (s<0) return ZT_ERR_NOT_FOUND;
    if (work.kind==ZT_PERSIST_DECISIONS) return commit_decisions(s);
    if (work.kind==ZT_PERSIST_ARCHIVE_CLEAR) return archive_clear(s);
    zt_err_t r=checkpoint_get(s,&scratch); if (r!=ZT_OK) return r;
    rebuild(&scratch); r=apply_decisions(s,&scratch); if (r!=ZT_OK) return r;
    switch (work.kind) {
    case ZT_PERSIST_COMMAND_RECEIPT: {
        uint32_t seq=work.value.command_receipt.command_seq;
        unsigned pos=0;
        for (unsigned i=0;i<8;++i) { if (scratch.applied_command_seq[i]==seq) return ZT_OK; if (scratch.applied_command_seq[i]<scratch.applied_command_seq[pos]) pos=i; }
        scratch.applied_command_seq[pos]=seq; break;
    }
    case ZT_PERSIST_CLOSE: {
        const zt_wire_round_closed_t *c=&work.value.close;
        if (c->slot>=20 || c->produced_seq!=scratch.produced[c->slot] || c->close_elapsed_ms>ZT_ROUND_DURATION_MS) return ZT_ERR_CONFLICT;
        scratch.phase=ZT_PHASE_EXPIRED_PENDING_SYNC; scratch.closed_bitmap|=1u<<c->slot; break;
    }
    case ZT_PERSIST_SERVER_CURSOR:
        if (work.value.server_applied_id<scratch.last_server_applied_id) return ZT_ERR_STALE;
        scratch.last_server_applied_id=work.value.server_applied_id; break;
    default: return ZT_ERR_INVALID_ARG;
    }
    return checkpoint_set(s,&scratch);
}
zt_err_t zt_store_service(void)
{
    TaskHandle_t caller=xTaskGetCurrentTaskHandle();
    if (!persist_owner) persist_owner=caller;
    if (persist_owner!=caller) return ZT_ERR_INVALID_STATE;
    if (!opened) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(io_lock,0)!=pdTRUE) return ZT_ERR_BUSY;
    portENTER_CRITICAL(&guard); uint8_t state=work_state; if (state==1) work_state=2; portEXIT_CRITICAL(&guard);
    if (state==1) {
        zt_err_t r=execute();
        if (r==ZT_ERR_BUSY) { xSemaphoreGive(io_lock); return ZT_ERR_STORAGE; }
        completion=(zt_persist_completion_t){work.request_id,work.kind,r};
        portENTER_CRITICAL(&guard); work_state=3; portEXIT_CRITICAL(&guard); state=3;
    }
    if (state==3) {
        zt_err_t r=completion_sink(&completion,completion_context);
        if (r==ZT_OK) { portENTER_CRITICAL(&guard); memset(&work,0,sizeof(work)); work_state=0; portEXIT_CRITICAL(&guard); }
        xSemaphoreGive(io_lock); return r;
    }
    xSemaphoreGive(io_lock); return state==2 ? ZT_ERR_STORAGE : ZT_OK;
}

/* Private component collaboration: bounded RAM-only queries for the game task.
 * No shared public header or on-wire format is extended. */
zt_err_t zt_store_find_attempt(zt_round_id_t round, zt_slot_t victim, zt_slot_t actor,
                             zt_boot_nonce_t boot, uint32_t seq, zt_wire_event_t *out);
zt_err_t zt_store_inventory(zt_round_id_t round, zt_wire_cache_key_t *out, size_t cap, size_t *count);
zt_round_id_t zt_store_other_round(zt_round_id_t active);
zt_err_t zt_store_find_attempt(zt_round_id_t round, zt_slot_t victim, zt_slot_t actor,
                             zt_boot_nonce_t boot, uint32_t seq, zt_wire_event_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    for (unsigned i=0;i<128;++i) {
        const zt_wire_event_t *e=&journal[i].event;
        if (journal[i].round==round && e->victim_slot==victim && e->actor_slot==actor && e->request_boot==boot && e->request_seq==seq) {
            *out=*e; portEXIT_CRITICAL(&guard); return ZT_OK;
        }
    }
    portEXIT_CRITICAL(&guard); return ZT_ERR_NOT_FOUND;
}
zt_err_t zt_store_inventory(zt_round_id_t round, zt_wire_cache_key_t *out, size_t cap, size_t *count)
{
    if (!out || !count || cap<128) return ZT_ERR_INVALID_ARG;
    *count=0;
    portENTER_CRITICAL(&guard);
    for (unsigned i=0;i<128;++i) if (journal[i].round==round) out[(*count)++]=(zt_wire_cache_key_t){journal[i].event.victim_slot,journal[i].event.event_seq};
    portEXIT_CRITICAL(&guard);
    /* Sort bounded inventory outside the short publication critical section. */
    for (unsigned i=1;i<*count;++i) {
        zt_wire_cache_key_t key=out[i]; unsigned j=i;
        while (j && (out[j-1].origin_slot>key.origin_slot || (out[j-1].origin_slot==key.origin_slot && out[j-1].event_seq>key.event_seq))) {
            out[j]=out[j-1]; --j;
        }
        out[j]=key;
    }
    return ZT_OK;
}
zt_round_id_t zt_store_other_round(zt_round_id_t active)
{
    portENTER_CRITICAL(&guard); zt_round_id_t result=rounds[0] && rounds[0]!=active ? rounds[0] : rounds[1]!=active ? rounds[1] : 0;
    portEXIT_CRITICAL(&guard); return result;
}

zt_err_t zt_store_decision_frontier(zt_round_id_t round, zt_slot_t *slot, uint16_t *frontier);
zt_err_t zt_store_decision_frontier(zt_round_id_t round, zt_slot_t *slot, uint16_t *frontier)
{
    if (!slot || !frontier) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    int i=round_index(round);
    if (i<0 || own_slots[i]>=20) { portEXIT_CRITICAL(&guard); return ZT_ERR_NOT_FOUND; }
    *slot=own_slots[i]; *frontier=own_decided[i]; portEXIT_CRITICAL(&guard); return ZT_OK;
}

bool zt_store_round_retained(zt_round_id_t round);
bool zt_store_round_retained(zt_round_id_t round)
{
    portENTER_CRITICAL(&guard); bool retained=round_index(round)>=0; portEXIT_CRITICAL(&guard); return retained;
}
