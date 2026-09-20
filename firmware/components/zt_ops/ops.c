#include "zt_ops.h"
#include "zt_ui.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

/* One producer, one game-task consumer. The producer publishes only after its
 * copy is complete; the consumer releases the slot only after reply acceptance.
 * No SDK callback, console callback, or persistence task executes effects here. */
enum { SLOT_EMPTY, SLOT_COPYING, SLOT_READY };
static atomic_uint slot;
static TaskHandle_t owner;
static zt_console_request_t request;
static zt_console_response_t response;
static bool json_full;
enum phase { BEGIN, LAYOUT, TAIL, HASH, HASH_FINISH, INFO_REPLY,
             INSTALL, INSTALL_READBACK, BUTTON_UI, ALLOWLIST_UI, REPLY };
static enum phase phase;

#define READ_BYTES 4096u
#define TABLE_OFFSET 0x8000u
#define TABLE_BYTES 0xc00u
#define EXPORT_BYTES (8u * ZT_DURABLE_EVENT_BYTES)
_Static_assert(ZT_FACTORY_BYTES == 2752512u, "hash the entire factory allocation");
_Static_assert(ZT_FLASH_BYTES - ZT_INSTALL_OFFSET == 65536u, "tail extent");
static _Alignas(4) uint8_t bytes[READ_BYTES];
static uint8_t header[ZT_INSTALL_BYTES], digest[ZT_SHA256_BYTES];
static uint32_t sha[8];
static bool tail_blank, header_valid, sector_blank, installed_here;
static uint32_t offset;
static zt_mac_t physical;
static esp_chip_info_t chip;
static zt_ui_snapshot_t snapshot;
static zt_checkpoint_t checkpoint;
static zt_config_t config_scratch;
static zt_gateway_event_t events[ZT_MAX_PLAYERS];
static const char *config_status;

/* Only these public checkpoint facts define an export session. Native padding
 * is never hashed or compared. A changing page fails instead of mixing epochs. */
static struct {
    bool valid;
    zt_round_id_t round;
    uint16_t next, produced[ZT_MAX_PLAYERS], decided[ZT_MAX_PLAYERS];
    uint16_t slot_count;
    uint8_t slots[ZT_JOURNAL_CAPACITY];
} exported;

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

static bool filled(const uint8_t *p, size_t n, uint8_t value)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != value) return false;
    return true;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* FIPS 180-4 SHA-256 compression, used only for the fixed-size factory digest.
 * The IDF C3 mbedTLS SHA backend acquires a shared blocking hardware lock and
 * can allocate DMA bounce buffers. Keep this read-only scan entirely in software.
 * The allocation and chunks are multiples of 64: no partial-block queue needed. */
_Static_assert(ZT_FACTORY_BYTES % 64 == 0 && READ_BYTES % 64 == 0, "SHA block alignment");
static uint32_t rotate(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void hash_start(void)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(sha, initial, sizeof(sha));
}

static void hash_block(const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[16];
    uint32_t a=sha[0], b=sha[1], c=sha[2], d=sha[3], e=sha[4], f=sha[5], g=sha[6], h=sha[7];
    for (unsigned i = 0; i < 64; ++i) {
        unsigned j = i & 15;
        if (i < 16) {
            w[j] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 |
                   (uint32_t)p[4*i+2] << 8 | p[4*i+3];
        } else {
            uint32_t x = w[(i-15)&15], y = w[(i-2)&15];
            w[j] += (rotate(x,7) ^ rotate(x,18) ^ (x>>3)) + w[(i-7)&15] +
                    (rotate(y,17) ^ rotate(y,19) ^ (y>>10));
        }
        uint32_t t1 = h + (rotate(e,6) ^ rotate(e,11) ^ rotate(e,25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[j];
        uint32_t t2 = (rotate(a,2) ^ rotate(a,13) ^ rotate(a,22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    sha[0]+=a; sha[1]+=b; sha[2]+=c; sha[3]+=d;
    sha[4]+=e; sha[5]+=f; sha[6]+=g; sha[7]+=h;
}

static void hash_finish(void)
{
    memset(bytes, 0, 64);
    bytes[0] = 0x80;
    uint64_t bits = (uint64_t)ZT_FACTORY_BYTES * 8;
    for (unsigned i = 0; i < 8; ++i) bytes[63-i] = (uint8_t)(bits >> (8*i));
    hash_block(bytes);
    for (unsigned i = 0; i < sizeof(digest); ++i)
        digest[i] = (uint8_t)(sha[i/4] >> (24 - 8*(i%4)));
    wipe(sha, sizeof(sha));
}

static void append(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void append(const char *format, ...)
{
    if (json_full) return;
    size_t available = sizeof(response.json) - response.len;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(response.json + response.len, available, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= available) { json_full = true; return; }
    response.len += (size_t)n;
}

static void hex(const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    if (json_full || n > (ZT_CONSOLE_RESPONSE_MAX_BYTES - response.len) / 2) {
        json_full = true;
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        response.json[response.len++] = digits[p[i] >> 4];
        response.json[response.len++] = digits[p[i] & 15];
    }
    response.json[response.len] = 0;
}

/* Only firmware-owned version strings use this helper. Never pass config. */
static void string(const char *s, size_t capacity)
{
    append("\"");
    size_t i;
    for (i = 0; i < capacity && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) { json_full = true; return; }
        if (c == '"' || c == '\\') append("\\");
        append("%c", c);
    }
    if (i == capacity) json_full = true;
    append("\"");
}

static void envelope(zt_err_t result)
{
    response.id = request.id;
    response.result = result;
    response.len = 0;
    json_full = false;
    append("{\"id\":%" PRIu32 ",\"ok\":%s", request.id, result == ZT_OK ? "true" : "false");
}

static void fail(zt_err_t result)
{
    wipe(sha, sizeof(sha));
    wipe(bytes, sizeof(bytes));
    wipe(&config_scratch, sizeof(config_scratch));
    wipe(&request.args, sizeof(request.args));
    envelope(result);
    append(",\"error\":%u}", (unsigned)result);
    phase = REPLY;
}

static void finish(void)
{
    append("}");
    if (json_full) fail(ZT_ERR_OVERFLOW);
    else phase = REPLY;
}

static zt_err_t identity(void)
{
    /* C3's factory STA address is its eFuse base MAC. A runtime base-MAC
     * override must not masquerade as the physical commissioning identity. */
    if (esp_efuse_mac_get_default(physical.bytes) != ESP_OK ||
        (physical.bytes[0] & 1) || filled(physical.bytes, ZT_MAC_BYTES, 0)) return ZT_ERR_STORAGE;
    esp_chip_info(&chip);
    uint32_t size;
    if (chip.model != CHIP_ESP32C3 ||
        esp_flash_get_physical_size(NULL, &size) != ESP_OK || size != ZT_FLASH_BYTES)
        return ZT_ERR_STORAGE;
    return ZT_OK;
}

static zt_err_t raw(uint32_t address, size_t length)
{
    if (length > sizeof(bytes)) return ZT_ERR_INVALID_LENGTH;
    return esp_flash_read(NULL, bytes, address, length) == ESP_OK ? ZT_OK : ZT_ERR_STORAGE;
}

/* Read the actual table, including the terminator, without touching stock NVS
 * or LittleFS. Boot has verified the optional SDK MD5 record. */
static zt_err_t layout(void)
{
    static const struct {
        uint8_t type, subtype;
        uint32_t offset, size;
        char label[16];
    } expected[] = {
        {1, 2, 0x9000, 0x4000, "nvs"},
        {1, 1, 0xd000, 0x1000, "phy_init"},
        {0, 0, ZT_FACTORY_OFFSET, ZT_FACTORY_BYTES, "factory"},
        {1, 0x83, 0x2b0000, 0x140000, "storage"}
    };
    zt_err_t r = raw(TABLE_OFFSET, TABLE_BYTES);
    if (r != ZT_OK) return r;
    for (unsigned i = 0; i < TABLE_BYTES / 32; ++i) {
        const uint8_t *p = bytes + 32 * i;
        if (i < 4) {
            if (p[0] != 0xaa || p[1] != 0x50 || p[2] != expected[i].type ||
                p[3] != expected[i].subtype || le32(p + 4) != expected[i].offset ||
                le32(p + 8) != expected[i].size || memcmp(p + 12, expected[i].label, 16) ||
                le32(p + 28)) return ZT_ERR_STORAGE;
        } else if (i == 4 && p[0] == 0xeb && p[1] == 0xeb && filled(p + 2, 14, 255)) {
            continue;
        } else if (!filled(p, 32, 255)) return ZT_ERR_STORAGE;
    }
    return ZT_OK;
}

static zt_err_t read_header(void)
{
    if (esp_flash_read(NULL, header, ZT_INSTALL_OFFSET, sizeof(header)) != ESP_OK)
        return ZT_ERR_STORAGE;
    zt_install_header_t decoded;
    header_valid = zt_store_decode_install(header, sizeof(header), &decoded) == ZT_OK &&
                   !memcmp(decoded.mac.bytes, physical.bytes, ZT_MAC_BYTES);
    return ZT_OK;
}

static zt_err_t read_config_status(void)
{
    zt_err_t r = zt_store_load_config(&config_scratch);
    wipe(&config_scratch, sizeof(config_scratch));
    if (r == ZT_ERR_BUSY) return r;
    if (r == ZT_OK) config_status = "present";
    else if (r == ZT_ERR_NOT_FOUND) config_status = "absent";
    else if (r == ZT_ERR_INVALID_STATE) {
        config_status = "unopened";
        /* install_init has already initialized this named partition. This read
         * does not initialize storage, create a namespace, or take its sink. */
        if (installed_here) {
            nvs_handle_t h;
            if (nvs_open_from_partition(ZT_NVS_PARTITION, ZT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
                return ZT_ERR_STORAGE;
            size_t n = 0;
            esp_err_t e = nvs_get_blob(h, ZT_STORE_KEY_CONFIG, NULL, &n);
            nvs_close(h);
            if (e == ESP_ERR_NVS_NOT_FOUND) config_status = "absent";
            else if (e == ESP_OK) config_status = "unopened"; /* not decoded/validated */
            else return ZT_ERR_STORAGE;
        }
    } else config_status = "error";
    return ZT_OK;
}

static const char *admission_name(zt_admission_t a)
{
    static const char *const names[] = {
        "BOOT", "WAIT_INSTALL", "NEEDS_CONFIG", "RECOVERING", "REJOINING", "LOBBY",
        "REGISTERING", "WAITING_FOR_ROUND", "NEXT_ROUND", "PREPARED", "RUNNING",
        "EXPIRED_PENDING_SYNC", "FINAL"
    };
    return (unsigned)a < sizeof(names) / sizeof(names[0]) ? names[a] : NULL;
}

static void info_reply(void)
{
    zt_err_t r = zt_game_snapshot(&snapshot);
    if (r != ZT_OK) { fail(r); return; }
    const char *admission = admission_name(snapshot.admission);
    if (!admission || memcmp(snapshot.self_mac.bytes, physical.bytes, ZT_MAC_BYTES) ||
        (snapshot.role != ZT_ROLE_HUMAN && snapshot.role != ZT_ROLE_ZOMBIE && snapshot.role != ZT_ROLE_UNKNOWN) ||
        (unsigned)snapshot.last_error > ZT_ERR_MAX) {
        fail(ZT_ERR_INVALID_STATE); return;
    }
    r = read_config_status();
    if (r == ZT_ERR_BUSY) return;
    if (r != ZT_OK) { fail(r); return; }
    /* The frozen snapshot cannot attest that a configured lobby has no staged
     * next-round snapshot/command. Do not label that unknown fact false. Setup
     * cannot accept gameplay transitions; a published round is already evidence
     * of retained/pending round state. */
    bool pending = snapshot.round_id != 0;
    if (!pending && snapshot.admission != ZT_ADMISSION_WAIT_INSTALL &&
        snapshot.admission != ZT_ADMISSION_NEEDS_CONFIG) {
        fail(ZT_ERR_NOT_IMPLEMENTED); return;
    }
    zt_store_status_t storage;
    r = zt_store_status(&storage);
    if (r != ZT_OK) { fail(r); return; }
    const char *install = tail_blank ? "WAIT_INSTALL" : header_valid && sector_blank ? "VALID" : "ERROR";
    if (storage.install_state == ZT_INSTALL_ERROR) install = "ERROR";
    envelope(ZT_OK);
    append(",\"firmware_version\":"); string(ZT_FIRMWARE_VERSION, sizeof(ZT_FIRMWARE_VERSION));
    append(",\"build_id\":"); string(ZT_BUILD_ID, sizeof(ZT_BUILD_ID));
    append(",\"protocol_version\":%u,\"mac\":\"", ZT_PROTOCOL_VERSION);
    hex(physical.bytes, ZT_MAC_BYTES);
    append("\",\"chip\":\"ESP32-C3\",\"chip_revision\":%u,\"flash_bytes\":%" PRIu32
           ",\"layout\":\"stock-v1\",\"factory_offset\":%" PRIu32 ",\"factory_bytes\":%" PRIu32
           ",\"install_offset\":%" PRIu32 ",\"nvs_offset\":%" PRIu32 ",\"nvs_bytes\":%" PRIu32
           ",\"factory_sha256\":\"", (unsigned)chip.revision, ZT_FLASH_BYTES, ZT_FACTORY_OFFSET,
           ZT_FACTORY_BYTES, ZT_INSTALL_OFFSET, ZT_NVS_OFFSET, ZT_NVS_LENGTH);
    hex(digest, sizeof(digest));
    append("\",\"install_state\":\"%s\",\"tail_blank\":%s,\"header_hex\":", install, tail_blank ? "true" : "false");
    if (header_valid && sector_blank) { append("\""); hex(header, sizeof(header)); append("\""); }
    else append("null");
    append(",\"config_status\":\"%s\",\"active_round\":\"%016" PRIx64
           "\",\"admission\":\"%s\",\"pending_round\":%s,\"role\":%u,\"last_error\":%u",
           config_status, snapshot.round_id, admission, pending ? "true" : "false", (unsigned)snapshot.role,
           (unsigned)(storage.last_error != ZT_OK ? storage.last_error :
                      !strcmp(install, "ERROR") ? ZT_ERR_STORAGE : snapshot.last_error));
    finish();
}

static void frontiers(const char *name, const uint16_t values[ZT_MAX_PLAYERS])
{
    append(",\"%s\":[", name);
    for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i) append("%s%u", i ? "," : "", (unsigned)values[i]);
    append("]");
}

static bool same_export(void)
{
    return checkpoint.round_id == exported.round &&
        !memcmp(checkpoint.produced, exported.produced, sizeof(exported.produced)) &&
        !memcmp(checkpoint.decided, exported.decided, sizeof(exported.decided)) &&
        checkpoint.journal_slot_count == exported.slot_count &&
        !memcmp(checkpoint.journal_slots, exported.slots, exported.slot_count);
}

static void export_page(void)
{
    uint16_t cursor = request.args.export_page.cursor, next;
    zt_round_id_t round = request.args.export_page.round_id;
    zt_err_t r;
    if (cursor > ZT_JOURNAL_CAPACITY) { fail(ZT_ERR_INVALID_ARG); return; }
    if (!round) {
        r = zt_game_snapshot(&snapshot);
        if (r != ZT_OK) { fail(r); return; }
        round = snapshot.round_id;
    }
    if (!round) { fail(ZT_ERR_NOT_FOUND); return; }
    r = zt_store_load_checkpoint(round, &checkpoint);
    if (r == ZT_ERR_BUSY) return;
    if (r != ZT_OK) { fail(r); return; }
    if (checkpoint.journal_slot_count > ZT_JOURNAL_CAPACITY) { fail(ZT_ERR_STORAGE); return; }
    if (exported.valid && exported.round == round && cursor) {
        if (cursor != exported.next || !same_export()) { fail(ZT_ERR_STALE); return; }
    } else {
        exported.round = round;
        memcpy(exported.produced, checkpoint.produced, sizeof(exported.produced));
        memcpy(exported.decided, checkpoint.decided, sizeof(exported.decided));
        exported.slot_count = checkpoint.journal_slot_count;
        memcpy(exported.slots, checkpoint.journal_slots, exported.slot_count);
        exported.next = cursor;
        exported.valid = true;
    }
    size_t written;
    r = zt_store_export(round, cursor, bytes, EXPORT_BYTES, &written, &next);
    if (r != ZT_OK) { fail(r); return; }
    if (written > EXPORT_BYTES || written % ZT_DURABLE_EVENT_BYTES || next > ZT_JOURNAL_CAPACITY ||
        (cursor < ZT_JOURNAL_CAPACITY && next <= cursor)) { fail(ZT_ERR_STORAGE); return; }
    r = zt_store_load_checkpoint(round, &checkpoint);
    if (r == ZT_ERR_BUSY) return; /* repeat the read-only page, never advance */
    if (r != ZT_OK || !same_export()) { fail(r == ZT_OK ? ZT_ERR_STALE : r); return; }
    envelope(ZT_OK);
    append(",\"round_id\":\"%016" PRIx64 "\",\"cursor\":%u,\"next_cursor\":%u,\"records_hex\":\"",
           round, (unsigned)cursor, (unsigned)next);
    hex(bytes, written);
    append("\"");
    frontiers("produced", exported.produced);
    frontiers("decided", exported.decided);
    append(",\"scope\":\"single_round\",\"retained_set_complete\":false,"
           "\"limitation\":\"PREVIOUS_ROUND_ENUMERATION_UNAVAILABLE\"");
    finish();
    if (response.result == ZT_OK) exported.next = next;
    wipe(bytes, sizeof(bytes));
}

static void status_reply(void)
{
    zt_radio_diagnostics_t radio;
    zt_channel_status_t channel;
    zt_store_status_t storage, after;
    zt_err_t r = zt_game_snapshot(&snapshot);
    if (r != ZT_OK) { fail(r); return; }
    r = zt_channel_get_status(&channel);
    if (r != ZT_OK) { fail(r); return; }
    r = zt_radio_get_diagnostics(&radio);
    if (r != ZT_OK) { fail(r); return; }
    r = zt_store_status(&storage);
    if (r != ZT_OK) { fail(r); return; }
    if (storage.install_state != ZT_INSTALL_VALID) { fail(ZT_ERR_INVALID_STATE); return; }
    if (snapshot.direct_contact_count > ZT_MAX_PLAYERS) { fail(ZT_ERR_INVALID_STATE); return; }
    size_t count = 0;
    uint16_t next = 0;
    if (snapshot.round_id) {
        r = zt_game_event_feed(snapshot.round_id, 0, events, ZT_MAX_PLAYERS, &count, &next);
        if (r == ZT_ERR_BUSY) return;
        if (r != ZT_OK) { fail(r); return; }
    }
    r = zt_store_status(&after);
    if (r != ZT_OK) { fail(r); return; }
    if (storage.event_count != after.event_count || count > storage.event_count || count > ZT_MAX_PLAYERS) {
        fail(ZT_ERR_STALE); return;
    }
    envelope(ZT_OK);
#define COUNTER(name, value) append(",\"" name "\":%" PRIu32, (uint32_t)(value))
    COUNTER("rx_drops", radio.rx_drops); COUNTER("tx_drops", radio.tx_drops);
    COUNTER("invalid_frames", radio.invalid_frames); COUNTER("auth_failures", radio.auth_failures);
    COUNTER("dedupe_hits", radio.dedupe_hits); COUNTER("rx_high_water", radio.rx_high_water);
    COUNTER("tx_high_water", radio.tx_high_water); COUNTER("event_high_water", snapshot.diagnostics.event_high_water);
    COUNTER("input_drops", snapshot.diagnostics.input_drops); COUNTER("tx_watchdogs", radio.tx_watchdogs);
    COUNTER("radio_restarts", radio.radio_restarts); COUNTER("gateway_drops", snapshot.diagnostics.gateway_drops);
    COUNTER("gateway_reconnects", snapshot.diagnostics.gateway_reconnects); COUNTER("replay_backlog", radio.replay_backlog);
    COUNTER("free_heap", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    COUNTER("minimum_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    COUNTER("largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    COUNTER("clock_uncertainty_ms", snapshot.diagnostics.clock_uncertainty_ms);
    COUNTER("gateway_age_ms", radio.gateway_age_ms); COUNTER("channel", radio.channel);
    COUNTER("pending_event_count", storage.event_count);
#undef COUNTER
    append(",\"diagnostic_mode\":%s,\"peer_ages_ms\":[", radio.diagnostic_mode ? "true" : "false");
    for (unsigned i = 0; i < snapshot.direct_contact_count; ++i)
        append("%s%" PRIu32, i ? "," : "", snapshot.contacts[i].age_ms);
    append("],\"pending_event_ids\":[");
    size_t emitted = 0;
    for (; emitted < count; ++emitted) {
        /* 27 bytes per quoted ID including comma, plus final completeness field. */
        if (ZT_CONSOLE_RESPONSE_MAX_BYTES - response.len < 27 + 40) break;
        const zt_event_id_t *id = &events[emitted].id;
        append("%s\"%016" PRIx64 "/%02x/%04x\"", emitted ? "," : "", id->round_id,
               (unsigned)id->victim_slot, (unsigned)id->event_seq);
    }
    append("],\"pending_ids_complete\":%s", emitted == storage.event_count ? "true" : "false");
    finish();
}

/* The frozen API gives the game the only store completion sink. It exposes
 * neither a completion query nor the pending transition/round admission guard.
 * Polling a value cannot distinguish an outstanding write from a definitive
 * refusal (or a pre-existing identical value), and console IDs collide with the
 * game's private persistence sequence. Until that ownership contract is amended,
 * refuse BEFORE submitting either durable operation. Do not steal the sink,
 * invent a reserved ID range, time out an accepted write, or claim a durable ack. */
static void configure(void)
{
    zt_err_t r = identity();
    if (r != ZT_OK) { fail(r); return; }
    if (memcmp(physical.bytes, request.args.configure.expected_mac.bytes, ZT_MAC_BYTES)) {
        fail(ZT_ERR_AUTH); return;
    }
    r = zt_game_snapshot(&snapshot);
    if (r != ZT_OK) { fail(r); return; }
    if (snapshot.round_id || (snapshot.admission != ZT_ADMISSION_NEEDS_CONFIG &&
        snapshot.admission != ZT_ADMISSION_LOBBY && snapshot.admission != ZT_ADMISSION_REGISTERING &&
        snapshot.admission != ZT_ADMISSION_WAITING_FOR_ROUND)) { fail(ZT_ERR_INVALID_STATE); return; }
    r = zt_store_load_checkpoint(0, &checkpoint);
    if (r == ZT_ERR_BUSY) return;
    if (r == ZT_OK) { fail(ZT_ERR_INVALID_STATE); return; }
    fail(r == ZT_ERR_NOT_FOUND ? ZT_ERR_NOT_IMPLEMENTED : r);
}

static void archive_clear(void)
{
    zt_round_id_t round = request.args.clearance.round_id;
    if (!round) { fail(ZT_ERR_INVALID_ARG); return; }
    zt_err_t r = zt_game_snapshot(&snapshot);
    if (r != ZT_OK) { fail(r); return; }
    if (round == snapshot.round_id) { fail(ZT_ERR_INVALID_STATE); return; }
    r = zt_store_load_checkpoint(round, &checkpoint);
    if (r == ZT_ERR_BUSY) return;
    if (r != ZT_OK) { fail(r); return; }
    if (checkpoint.phase < ZT_PHASE_EXPIRED_PENDING_SYNC) { fail(ZT_ERR_INVALID_STATE); return; }
    for (unsigned i = 0; i < ZT_MAX_PLAYERS; ++i) {
        if (checkpoint.produced[i] != request.args.clearance.produced[i] ||
            checkpoint.decided[i] < checkpoint.produced[i]) { fail(ZT_ERR_CONFLICT); return; }
    }
    fail(ZT_ERR_NOT_IMPLEMENTED);
}

static void begin(void)
{
    zt_err_t r;
    switch (request.op) {
    case ZT_CONSOLE_INFO:
    case ZT_CONSOLE_INSTALL_INIT:
        r = identity();
        if (r != ZT_OK) { fail(r); return; }
        if (request.op == ZT_CONSOLE_INSTALL_INIT &&
            memcmp(physical.bytes, request.args.install.mac.bytes, ZT_MAC_BYTES)) { fail(ZT_ERR_AUTH); return; }
        phase = LAYOUT;
        break;
    case ZT_CONSOLE_CONFIGURE: configure(); break;
    case ZT_CONSOLE_STATUS: status_reply(); break;
    case ZT_CONSOLE_GAME_EXPORT: export_page(); break;
    case ZT_CONSOLE_ARCHIVE_CLEAR: archive_clear(); break;
    case ZT_CONSOLE_BUTTON:
        if ((unsigned)request.args.button.button > ZT_BUTTON_START ||
            (unsigned)request.args.button.kind > ZT_EDGE_PRESS) { fail(ZT_ERR_INVALID_ARG); return; }
        /* Verify both owners exist before either consumes the edge. UI has no
         * shutdown path; its initialization remains valid across queue retries. */
        r = zt_game_snapshot(&snapshot);
        if (r == ZT_OK) r = zt_ui_submit_snapshot(&snapshot);
        if (r != ZT_OK) { fail(r); return; }
        r = zt_game_post_button(&request.args.button);
        if (r != ZT_OK) { fail(r); return; }
        phase = BUTTON_UI;
        break;
    case ZT_CONSOLE_LINK_ALLOWLIST:
        if (request.args.allowlist.count > ZT_LINK_ALLOWLIST_MAX) { fail(ZT_ERR_INVALID_ARG); return; }
        for (unsigned i = 0; i < request.args.allowlist.count; ++i) {
            const uint8_t *mac = request.args.allowlist.macs[i].bytes;
            if ((mac[0] & 1) || filled(mac, ZT_MAC_BYTES, 0)) { fail(ZT_ERR_INVALID_ARG); return; }
            for (unsigned j = 0; j < i; ++j)
                if (!memcmp(mac, request.args.allowlist.macs[j].bytes, ZT_MAC_BYTES)) { fail(ZT_ERR_INVALID_ARG); return; }
        }
        {
            zt_channel_status_t channel;
            r = zt_channel_get_status(&channel);
            if (r != ZT_OK) { fail(r); return; }
        }
        r = zt_game_snapshot(&snapshot);
        if (r == ZT_OK) r = zt_ui_submit_snapshot(&snapshot);
        if (r != ZT_OK) { fail(r); return; }
        r = zt_radio_link_allowlist(request.args.allowlist.macs, request.args.allowlist.count);
        if (r != ZT_OK) { fail(r); return; }
        phase = ALLOWLIST_UI;
        break;
    case ZT_CONSOLE_DEMO_ROUND:
        r = zt_demo_start(&request.args.demo);
        if (r != ZT_OK) { fail(r); return; }
        envelope(ZT_OK); finish();
        break;
    default: fail(ZT_ERR_INVALID_ARG); break;
    }
}

zt_err_t zt_ops_post(const zt_console_request_t *value)
{
    if (!value || (unsigned)value->op > ZT_CONSOLE_DEMO_ROUND) return ZT_ERR_INVALID_ARG;
    unsigned empty = SLOT_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(&slot, &empty, SLOT_COPYING,
                                                 memory_order_acquire, memory_order_relaxed)) return ZT_ERR_BUSY;
    request = *value;
    phase = BEGIN;
    atomic_store_explicit(&slot, SLOT_READY, memory_order_release);
    return ZT_OK;
}

zt_err_t zt_ops_service(uint64_t now_us)
{
    (void)now_us;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    if (!owner) owner = caller;
    if (owner != caller) return ZT_ERR_INVALID_STATE;
    if (atomic_load_explicit(&slot, memory_order_acquire) != SLOT_READY) return ZT_OK;
    zt_err_t r;
    switch (phase) {
    case BEGIN: begin(); break;
    case LAYOUT:
        r = layout();
        if (r != ZT_OK) { fail(r); break; }
        tail_blank = true; sector_blank = true; offset = ZT_INSTALL_OFFSET; phase = TAIL;
        break;
    case TAIL:
        r = raw(offset, READ_BYTES);
        if (r != ZT_OK) { fail(r); break; }
        if (!filled(bytes, READ_BYTES, 255)) tail_blank = false;
        if (offset == ZT_INSTALL_OFFSET) sector_blank = filled(bytes + ZT_INSTALL_BYTES, READ_BYTES - ZT_INSTALL_BYTES, 255);
        offset += READ_BYTES;
        if (offset == ZT_FLASH_BYTES) {
            if (request.op == ZT_CONSOLE_INSTALL_INIT) phase = INSTALL;
            else {
                hash_start();
                offset = 0; phase = HASH;
            }
        }
        break;
    case HASH: {
        size_t n = ZT_FACTORY_BYTES - offset;
        if (n > READ_BYTES) n = READ_BYTES;
        r = raw(ZT_FACTORY_OFFSET + offset, n);
        if (r != ZT_OK) { fail(r); break; }
        for (size_t i = 0; i < n; i += 64) hash_block(bytes + i);
        offset += n;
        if (offset == ZT_FACTORY_BYTES) phase = HASH_FINISH;
        break;
    }
    case HASH_FINISH:
        hash_finish(); wipe(bytes, sizeof(bytes));
        r = read_header();
        if (r != ZT_OK) { fail(r); break; }
        phase = INFO_REPLY;
        break;
    case INFO_REPLY: info_reply(); break;
    case INSTALL: {
        if (!tail_blank) { fail(ZT_ERR_INVALID_STATE); break; }
        r = zt_game_snapshot(&snapshot);
        if (r != ZT_OK) { fail(r); break; }
        if (snapshot.admission != ZT_ADMISSION_WAIT_INSTALL) { fail(ZT_ERR_INVALID_STATE); break; }
        size_t n;
        r = zt_store_encode_install(&request.args.install, bytes, sizeof(bytes), &n);
        if (r != ZT_OK) { fail(r); break; }
        /* This is the sole initializer. Its synchronous flash latency is owned
         * by zt_store; the frozen API does not expose incremental installation. */
        r = zt_store_install_init(&request.args.install);
        if (r != ZT_OK) { fail(r); break; }
        installed_here = true;
        phase = INSTALL_READBACK;
        break;
    }
    case INSTALL_READBACK:
        r = read_header();
        if (r != ZT_OK || !header_valid || memcmp(header, bytes, ZT_INSTALL_BYTES)) { fail(ZT_ERR_STORAGE); break; }
        r = read_config_status();
        if (r == ZT_ERR_BUSY) break;
        if (r != ZT_OK || strcmp(config_status, "absent")) { fail(ZT_ERR_STORAGE); break; }
        envelope(ZT_OK);
        append(",\"install_state\":\"VALID\",\"header_hex\":\""); hex(header, sizeof(header));
        append("\",\"config_status\":\"absent\""); finish();
        wipe(bytes, sizeof(bytes));
        break;
    case BUTTON_UI:
        r = zt_ui_post_button(&request.args.button);
        if (r == ZT_ERR_BUSY) break; /* never deliver to game twice */
        if (r != ZT_OK) { fail(r); break; }
        envelope(ZT_OK); finish();
        break;
    case ALLOWLIST_UI: {
        zt_radio_diagnostics_t radio;
        r = zt_radio_get_diagnostics(&radio);
        if (r != ZT_OK) { fail(r); break; }
        if (!!radio.diagnostic_mode != !!request.args.allowlist.count) { fail(ZT_ERR_RADIO); break; }
        r = zt_game_snapshot(&snapshot);
        if (r != ZT_OK) { fail(r); break; }
        snapshot.diagnostic_mode = radio.diagnostic_mode;
        r = zt_ui_submit_snapshot(&snapshot);
        if (r == ZT_ERR_BUSY) break;
        if (r != ZT_OK) { fail(r); break; }
        /* Subsequent game publications read the same radio-owned flag, keeping
         * the existing diagnostic renderer authoritative; no expiring banner. */
        envelope(ZT_OK);
        append(",\"diagnostic_mode\":%s,\"count\":%u", radio.diagnostic_mode ? "true" : "false",
               (unsigned)request.args.allowlist.count);
        finish();
        break;
    }
    case REPLY:
        r = zt_console_reply(&response);
        if (r == ZT_ERR_BUSY) return r;
        if (r != ZT_OK) return r; /* keep the exact completion; never repeat effects */
        wipe(&request, sizeof(request)); wipe(&response, sizeof(response));
        wipe(bytes, sizeof(bytes));
        atomic_store_explicit(&slot, SLOT_EMPTY, memory_order_release);
        return ZT_OK;
    }
    return ZT_ERR_BUSY;
}
