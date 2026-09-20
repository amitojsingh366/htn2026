#include "zt_console.h"
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "The operator console requires native USB Serial/JTAG"
#endif

/* Flat objects and scalar arrays only: no tree, recursion, or parser allocation. */
#define FIELD_MAX 40
#define ARRAY_MAX 20
#define LOG_MAX 240
#define INPUT_IDLE_US UINT64_C(5000000)
typedef struct { const char *p; size_t n; } span_t;
typedef struct { span_t key, value; bool used; } field_t;
typedef struct { field_t f[FIELD_MAX]; unsigned count; } object_t;
static zt_console_line_t line;
static zt_console_request_t request;
static object_t input;
static zt_console_sink_t request_sink;
static void *sink_context;
static StaticSemaphore_t rx_lock_memory, tx_lock_memory, service_lock_memory;
static SemaphoreHandle_t rx_lock, tx_lock, service_lock;
static char tx[ZT_CONSOLE_RESPONSE_MAX_BYTES + 2], log_line[LOG_MAX + 3];
static size_t tx_len, tx_at, log_len, log_at;
static uint64_t last_input_us, log_window;
static unsigned log_count;
static bool ready;
/* Exactly one outstanding request; the sink retains work until reply accepts it. */
static bool pending;
static uint32_t pending_id;
static zt_console_operation_t pending_op;
static portMUX_TYPE pending_guard = portMUX_INITIALIZER_UNLOCKED;

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static int unicode_hex(char c)
{
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return hex(c);
}
static bool u16(const char *p, uint32_t *v)
{
    *v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int h = unicode_hex(p[i]);
        if (h < 0) return false;
        *v = (*v << 4) | (unsigned)h;
    }
    return true;
}
/* Decode and validate Unicode even when dst is NULL. Reject embedded NUL. */
static bool string(span_t s, char *dst, size_t cap, size_t *length)
{
    if (s.n < 2 || s.p[0] != '"' || s.p[s.n - 1] != '"') return false;
    size_t n = 0;
    for (size_t i = 1; i < s.n - 1;) {
        uint32_t cp = (unsigned char)s.p[i++];
        if (cp < 32 || cp == '"') return false;
        if (cp == '\\') {
            if (i >= s.n - 1) return false;
            char e = s.p[i++];
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u':
                if (i + 4 > s.n - 1 || !u16(s.p + i, &cp)) return false;
                i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t low;
                    if (i + 6 > s.n - 1 || s.p[i] != '\\' || s.p[i + 1] != 'u' ||
                        !u16(s.p + i + 2, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    i += 6; cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                break;
            default: return false;
            }
        } else if (cp >= 128) {
            unsigned more; uint32_t min;
            if (cp >= 0xc2 && cp <= 0xdf) { more = 1; min = 0x80; cp &= 31; }
            else if (cp >= 0xe0 && cp <= 0xef) { more = 2; min = 0x800; cp &= 15; }
            else if (cp >= 0xf0 && cp <= 0xf4) { more = 3; min = 0x10000; cp &= 7; }
            else return false;
            if (i + more > s.n - 1) return false;
            while (more--) {
                unsigned c = (unsigned char)s.p[i++];
                if ((c & 0xc0) != 0x80) return false;
                cp = (cp << 6) | (c & 63);
            }
            if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        }
        if (!cp) return false;
        unsigned char bytes[4]; size_t count;
        if (cp < 0x80) { bytes[0] = cp; count = 1; }
        else if (cp < 0x800) { bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63); count = 2; }
        else if (cp < 0x10000) { bytes[0] = 0xe0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63); bytes[2] = 0x80 | (cp & 63); count = 3; }
        else { bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63); bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63); count = 4; }
        if (dst) {
            if (n + count >= cap) return false;
            memcpy(dst + n, bytes, count);
        }
        n += count;
    }
    if (dst) dst[n] = 0;
    if (length) *length = n;
    return true;
}
static void ws(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r')) ++*p;
}
static bool eq(span_t s, const char *value)
{
    return s.n == strlen(value) && !memcmp(s.p, value, s.n);
}
static bool integer(span_t s, uint64_t max, uint64_t *v)
{
    if (!s.n || (s.n > 1 && s.p[0] == '0')) return false;
    *v = 0;
    for (size_t i = 0; i < s.n; ++i) {
        unsigned d = (unsigned char)s.p[i] - '0';
        if (d > 9 || *v > max / 10 || (*v == max / 10 && d > max % 10)) return false;
        *v = *v * 10 + d;
    }
    return true;
}
static bool scalar(const char **p, const char *end, span_t *out)
{
    ws(p, end); const char *start = *p;
    if (*p == end) return false;
    if (**p == '"') {
        ++*p;
        bool escaped = false, closed = false;
        while (*p < end) {
            char c = *(*p)++;
            if (!escaped && c == '"') { closed = true; break; }
            if (!escaped && c == '\\') escaped = true; else escaped = false;
        }
        *out = (span_t){start, *p - start};
        return closed && string(*out, NULL, 0, NULL);
    }
    while (*p < end && **p != ',' && **p != '}' && **p != ']' && **p != ' ' && **p != '\t' && **p != '\r') ++*p;
    *out = (span_t){start, *p - start};
    uint64_t value;
    if (eq(*out, "true") || eq(*out, "false") || eq(*out, "null")) return true;
    span_t number = *out;
    if (number.n && number.p[0] == '-') { ++number.p; --number.n; }
    return integer(number, UINT64_MAX, &value);
}
static bool array(span_t s, span_t *items, unsigned *count)
{
    if (s.n < 2 || s.p[0] != '[' || s.p[s.n - 1] != ']') return false;
    const char *p = s.p + 1, *end = s.p + s.n - 1;
    *count = 0; ws(&p, end);
    if (p == end) return true;
    for (;;) {
        span_t item;
        if (*count == ARRAY_MAX || !scalar(&p, end, &item)) return false;
        if (items) items[*count] = item;
        ++*count; ws(&p, end);
        if (p == end) return true;
        if (*p++ != ',') return false;
    }
}
static bool parse(const char *bytes, size_t len, object_t *o)
{
    memset(o, 0, sizeof(*o));
    const char *p = bytes, *end = bytes + len;
    ws(&p, end);
    if (p == end || *p++ != '{') return false;
    ws(&p, end);
    if (p < end && *p == '}') { ++p; ws(&p, end); return p == end; }
    for (;;) {
        if (o->count == FIELD_MAX) return false;
        field_t *f = &o->f[o->count];
        if (!scalar(&p, end, &f->key) || f->key.n < 3 || f->key.p[0] != '"') return false;
        /* Keys have one spelling; escaped aliases and duplicate keys refuse. */
        for (size_t i = 1; i + 1 < f->key.n; ++i)
            if (!((f->key.p[i] >= 'a' && f->key.p[i] <= 'z') ||
                  (f->key.p[i] >= '0' && f->key.p[i] <= '9') || f->key.p[i] == '_')) return false;
        for (unsigned i = 0; i < o->count; ++i)
            if (f->key.n == o->f[i].key.n && !memcmp(f->key.p, o->f[i].key.p, f->key.n)) return false;
        ws(&p, end);
        if (p == end || *p++ != ':') return false;
        ws(&p, end);
        if (p < end && *p == '[') {
            const char *start = p++;
            ws(&p, end);
            if (p < end && *p != ']') {
                for (;;) {
                    span_t item;
                    if (!scalar(&p, end, &item)) return false;
                    ws(&p, end);
                    if (p == end) return false;
                    if (*p == ']') break;
                    if (*p++ != ',') return false;
                }
            }
            if (p == end || *p++ != ']') return false;
            f->value = (span_t){start, p - start};
            unsigned count;
            if (!array(f->value, NULL, &count)) return false;
        } else if (!scalar(&p, end, &f->value)) return false;
        ++o->count; ws(&p, end);
        if (p == end) return false;
        if (*p == '}') { ++p; ws(&p, end); return p == end; }
        if (*p++ != ',') return false;
    }
}
static span_t get(object_t *o, const char *name)
{
    size_t n = strlen(name);
    for (unsigned i = 0; i < o->count; ++i) {
        field_t *f = &o->f[i];
        if (f->key.n == n + 2 && !memcmp(f->key.p + 1, name, n)) { f->used = true; return f->value; }
    }
    return (span_t){0};
}
static bool number(object_t *o, const char *key, uint64_t max, uint64_t *v)
{
    return integer(get(o, key), max, v);
}
static bool text_field(object_t *o, const char *key, char *dst, size_t cap, size_t min, bool ascii)
{
    size_t n;
    if (!string(get(o, key), dst, cap, &n) || n < min) return false;
    for (size_t i = 0; ascii && i < n; ++i) if ((unsigned char)dst[i] < 32 || (unsigned char)dst[i] > 126) return false;
    return true;
}
static bool hex_bytes(span_t s, uint8_t *dst, size_t n)
{
    if (s.n != n * 2 + 2 || s.p[0] != '"' || s.p[s.n - 1] != '"') return false;
    for (size_t i = 0; i < n; ++i) {
        int a = hex(s.p[1 + 2 * i]), b = hex(s.p[2 + 2 * i]);
        if (a < 0 || b < 0) return false;
        dst[i] = (a << 4) | b;
    }
    return true;
}
static bool nonzero(const uint8_t *p, size_t n)
{
    unsigned value = 0;
    for (size_t i = 0; i < n; ++i) value |= p[i];
    return value != 0;
}
static bool mac(span_t s, zt_mac_t *out)
{
    return hex_bytes(s, out->bytes, 6) && !(out->bytes[0] & 1) && nonzero(out->bytes, 6);
}
static bool round_id(span_t s, uint64_t *out)
{
    uint8_t bytes[8];
    if (!hex_bytes(s, bytes, sizeof(bytes))) return false;
    *out = 0;
    for (unsigned i = 0; i < sizeof(bytes); ++i) *out = (*out << 8) | bytes[i];
    return true;
}
static bool uuid(span_t s, uint8_t out[16])
{
    if (s.n != 38 || s.p[0] != '"' || s.p[37] != '"') return false;
    unsigned at = 0;
    for (unsigned i = 1; i < 37;) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (s.p[i++] != '-') return false; continue; }
        int a = hex(s.p[i++]), b = hex(s.p[i++]);
        if (a < 0 || b < 0 || at == 16) return false;
        out[at++] = (a << 4) | b;
    }
    return at == 16 && nonzero(out, 16);
}
static bool https(const char *s)
{
    if (strncmp(s, "https://", 8) || !s[8] || s[8] == '/' || s[8] == ':' || s[8] == '?') return false;
    for (const char *p = s + 8; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p > 126 || *p == '@' || *p == '#' || *p == '\\') return false;
    return true;
}
static bool decode_request(void)
{
    object_t *o = &input; uint64_t value;
    if (!o->count || !eq(o->f[0].key, "\"id\"") || !number(o, "id", UINT32_MAX, &value)) return false;
    request.id = value;
    /* Receive is serialized; fixed name scratch keeps the decoder stack bounded. */
    static char op[sizeof("link_allowlist")];
    if (!text_field(o, "op", op, sizeof(op), 1, true)) return false;
    if (!strcmp(op, "info")) request.op = ZT_CONSOLE_INFO;
    else if (!strcmp(op, "status")) request.op = ZT_CONSOLE_STATUS;
    else if (!strcmp(op, "install_init")) {
        request.op = ZT_CONSOLE_INSTALL_INIT;
        zt_install_header_t *h = &request.args.install;
        if (!number(o, "schema", 1, &value) || value != 1 ||
            !mac(get(o, "expected_mac"), &h->mac) ||
            !uuid(get(o, "installation_uuid"), h->installation_uuid) ||
            !hex_bytes(get(o, "baseline_sha256"), h->baseline_sha256, 32) ||
            !nonzero(h->baseline_sha256, 32)) return false;
        bool uuid_ff = true, hash_ff = true;
        for (unsigned i = 0; i < 16; ++i) if (h->installation_uuid[i] != 255) uuid_ff = false;
        for (unsigned i = 0; i < 32; ++i) if (h->baseline_sha256[i] != 255) hash_ff = false;
        if (uuid_ff || hash_ff) return false;
        memcpy(h->magic, ZT_INSTALL_MAGIC, 4); h->schema = 1; h->length = ZT_INSTALL_BYTES;
        h->nvs_offset = ZT_NVS_OFFSET; h->nvs_length = ZT_NVS_LENGTH; h->flags = ZT_INSTALL_FLAG_INITIALIZED;
    } else if (!strcmp(op, "configure")) {
        request.op = ZT_CONSOLE_CONFIGURE;
        zt_config_t *c = &request.args.configure;
        if (!mac(get(o, "expected_mac"), &c->expected_mac) || !mac(get(o, "host_mac"), &c->host_mac) ||
            !text_field(o, "name", c->name, sizeof(c->name), 1, true) ||
            !round_id(get(o, "game_id"), &c->game_id) || !c->game_id ||
            !hex_bytes(get(o, "group_key"), c->group_key, sizeof(c->group_key)) || !nonzero(c->group_key, sizeof(c->group_key)) ||
            !number(o, "last_channel", 11, &value) || !value) return false;
        c->last_channel = value;
        span_t host = get(o, "host_credentials_present");
        if (!eq(host, "true") && !eq(host, "false")) return false;
        c->host_credentials_present = eq(host, "true");
        if (!number(o, "provisioned_time_ms", UINT64_C(9007199254740991), &c->provisioned_time_ms)) return false;
        if (c->host_credentials_present) {
            if (memcmp(c->expected_mac.bytes, c->host_mac.bytes, 6) ||
                !text_field(o, "https_base", c->https_base, sizeof(c->https_base), 9, true) || !https(c->https_base) ||
                !text_field(o, "ssid", c->ssid, sizeof(c->ssid), 1, false) ||
                !text_field(o, "password", c->password, sizeof(c->password), 0, false) ||
                !text_field(o, "token", c->token, sizeof(c->token), 1, true)) return false;
        } else if (!memcmp(c->expected_mac.bytes, c->host_mac.bytes, 6)) return false;
    } else if (!strcmp(op, "button")) {
        request.op = ZT_CONSOLE_BUTTON;
        char button[8], edge[8];
        static const char *names[] = {"A", "B", "HOME", "DOWN", "LEFT", "RIGHT", "UP", "AUX1", "START"};
        if (!text_field(o, "button", button, sizeof(button), 1, true) ||
            !text_field(o, "edge", edge, sizeof(edge), 1, true)) return false;
        unsigned i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (!strcmp(button, names[i])) break;
        if (i == sizeof(names) / sizeof(names[0]) || (strcmp(edge, "press") && strcmp(edge, "release"))) return false;
        request.args.button = (zt_button_edge_t){.at_us = esp_timer_get_time(), .button = i,
            .kind = !strcmp(edge, "press") ? ZT_EDGE_PRESS : ZT_EDGE_RELEASE};
    } else if (!strcmp(op, "link_allowlist")) {
        request.op = ZT_CONSOLE_LINK_ALLOWLIST;
        span_t items[ARRAY_MAX]; unsigned count;
        if (!array(get(o, "macs"), items, &count)) return false;
        request.args.allowlist.count = count;
        for (unsigned i = 0; i < count; ++i) {
            if (!mac(items[i], &request.args.allowlist.macs[i])) return false;
            for (unsigned j = 0; j < i; ++j)
                if (!memcmp(request.args.allowlist.macs[i].bytes, request.args.allowlist.macs[j].bytes, 6)) return false;
        }
    } else if (!strcmp(op, "demo_round")) {
        request.op = ZT_CONSOLE_DEMO_ROUND;
        zt_demo_round_t *d = &request.args.demo;
        span_t items[ARRAY_MAX]; unsigned count;
        if (!round_id(get(o, "round_id"), &d->round_id) || !d->round_id ||
            !number(o, "duration_ms", ZT_DEMO_DEFAULT_DURATION_MS, &value) ||
            (value != 0 && value != ZT_DEMO_DEFAULT_DURATION_MS)) return false;
        d->duration_ms = value;
        if (!array(get(o, "players"), items, &count) || count < ZT_DEMO_MIN_PLAYERS ||
            count > ZT_MAX_PLAYERS) return false;
        d->player_count = count;
        for (unsigned i = 0; i < count; ++i) {
            if (!mac(items[i], &d->players[i])) return false;
            for (unsigned j = 0; j < i; ++j)
                if (!memcmp(d->players[i].bytes, d->players[j].bytes, ZT_MAC_BYTES)) return false;
        }
        if (!number(o, "patient_zero_slot", count - 1, &value)) return false;
        d->patient_zero_slot = value;
        if (!number(o, "channel", ZT_CHANNEL_MAX, &value) || value < ZT_CHANNEL_MIN) return false;
        d->channel = value;
        if (!number(o, "start_delay_ms", ZT_DEMO_START_DELAY_MAX_MS, &value) ||
            value < ZT_DEMO_START_DELAY_MIN_MS) return false;
        d->start_delay_ms = value;
    } else if (!strcmp(op, "game_export")) {
        request.op = ZT_CONSOLE_GAME_EXPORT;
        span_t round = get(o, "round_id");
        if (round.n && (!round_id(round, &request.args.export_page.round_id) || !request.args.export_page.round_id)) return false;
        if (!number(o, "cursor", ZT_JOURNAL_CAPACITY, &value)) return false;
        request.args.export_page.cursor = value;
    } else if (!strcmp(op, "archive_clear")) {
        request.op = ZT_CONSOLE_ARCHIVE_CLEAR;
        zt_archive_clearance_t *a = &request.args.clearance;
        span_t items[ARRAY_MAX]; unsigned count;
        if (!round_id(get(o, "round_id"), &a->round_id) || !a->round_id ||
            !hex_bytes(get(o, "export_sha256"), a->export_sha256, 32) ||
            !array(get(o, "produced"), items, &count) || count != ZT_MAX_PLAYERS) return false;
        for (unsigned i = 0; i < count; ++i) {
            if (!integer(items[i], UINT16_MAX, &value)) return false;
            a->produced[i] = value;
        }
    } else return false;
    for (unsigned i = 0; i < o->count; ++i) if (!o->f[i].used) return false;
    return true;
}
/* The required first field survives even when the remainder overflows. */
static uint32_t prefix_id(void)
{
    const char *p = line.bytes, *end = p + line.used; span_t key, value; uint64_t id;
    ws(&p, end);
    if (p == end || *p++ != '{' || !scalar(&p, end, &key) || !eq(key, "\"id\"")) return 0;
    ws(&p, end);
    if (p == end || *p++ != ':' || !scalar(&p, end, &value) || !integer(value, UINT32_MAX, &id)) return 0;
    ws(&p, end);
    return p < end && (*p == ',' || *p == '}') ? (uint32_t)id : 0;
}
static zt_err_t refusal(uint32_t id, zt_err_t result)
{
    /* Fixed vocabulary; no input bytes, including parse failures, ever escape. */
    char bytes[96];
    int n = snprintf(bytes, sizeof(bytes), "{\"id\":%lu,\"ok\":false,\"error\":%u}", (unsigned long)id, (unsigned)result);
    if (n < 0 || n >= (int)sizeof(bytes)) return ZT_ERR_OVERFLOW;
    if (xSemaphoreTake(tx_lock, 0) != pdTRUE) return ZT_ERR_BUSY;
    if (tx_len) { xSemaphoreGive(tx_lock); return ZT_ERR_BUSY; }
    memcpy(tx, bytes, n); tx[n] = '\n'; tx_len = n + 1; tx_at = 0;
    xSemaphoreGive(tx_lock); return result;
}
/* Unreviewed SDK format arguments may contain credentials or line fragments.
 * Only explicitly sanitized zt_console_log() messages reach the USB stream. */
static int suppress_sdk_log(const char *format, va_list args)
{
    (void)format; (void)args; return 0;
}
zt_err_t zt_console_init(zt_console_sink_t sink, void *context)
{
    if (!sink) return ZT_ERR_INVALID_ARG;
    if (ready) return ZT_ERR_INVALID_STATE;
    rx_lock = xSemaphoreCreateMutexStatic(&rx_lock_memory);
    tx_lock = xSemaphoreCreateMutexStatic(&tx_lock_memory);
    service_lock = xSemaphoreCreateMutexStatic(&service_lock_memory);
    usb_serial_jtag_driver_config_t config = {.rx_buffer_size = 2048, .tx_buffer_size = 2048};
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) return ZT_ERR_INVALID_STATE;
    request_sink = sink; sink_context = context;
    esp_log_set_vprintf(suppress_sdk_log);
    ready = true;
    return ZT_OK;
}
zt_err_t zt_console_receive(const uint8_t *bytes, size_t len)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    if (!bytes || !len || len > ZT_CONSOLE_HOST_WRITE_MAX_BYTES) return ZT_ERR_INVALID_LENGTH;
    if (xSemaphoreTake(rx_lock, 0) != pdTRUE) return ZT_ERR_BUSY;
    zt_err_t result = ZT_OK;
    last_input_us = esp_timer_get_time();
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] == '\n') {
            uint32_t id = prefix_id();
            if (line.discarding_overflow == 1) result = refusal(id, ZT_ERR_OVERFLOW);
            else if (line.discarding_overflow == 2) result = ZT_ERR_TIMEOUT;
            else {
                wipe(&request, sizeof(request));
                if (!parse(line.bytes, line.used, &input) || !decode_request()) result = refusal(id, ZT_ERR_INVALID_ARG);
                else {
                    bool accepted = false;
                    if (xSemaphoreTake(tx_lock, 0) == pdTRUE) {
                        portENTER_CRITICAL(&pending_guard);
                        if (!pending && !tx_len) {
                            pending = true; pending_id = request.id; pending_op = request.op; accepted = true;
                        }
                        portEXIT_CRITICAL(&pending_guard);
                        xSemaphoreGive(tx_lock);
                    }
                    if (!accepted) result = refusal(request.id, ZT_ERR_BUSY);
                    else {
                        result = request_sink(&request, sink_context);
                        if (result != ZT_OK) {
                            /* The sink must not reply after returning a refusal. */
                            portENTER_CRITICAL(&pending_guard);
                            pending = false;
                            portEXIT_CRITICAL(&pending_guard);
                            result = refusal(request.id, result);
                        }
                    }
                }
            }
            wipe(&request, sizeof(request)); wipe(&input, sizeof(input)); wipe(&line, sizeof(line));
        } else if (!line.discarding_overflow) {
            if (line.used == ZT_CONSOLE_INPUT_MAX_BYTES) line.discarding_overflow = 1;
            else line.bytes[line.used++] = bytes[i];
        }
    }
    xSemaphoreGive(rx_lock); return result;
}
zt_err_t zt_console_reply(const zt_console_response_t *response)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    if (!response || !response->len || response->len > ZT_CONSOLE_RESPONSE_MAX_BYTES || response->json[0] != '{') return ZT_ERR_INVALID_LENGTH;
    /* Validate the envelope, not effect-specific payloads owned by integration. */
    object_t object; uint64_t id, error;
    if (!parse(response->json, response->len, &object) || !number(&object, "id", UINT32_MAX, &id) || id != response->id) return ZT_ERR_INVALID_ARG;
    span_t ok = get(&object, "ok");
    if (response->result == ZT_OK) { if (!eq(ok, "true")) return ZT_ERR_INVALID_ARG; }
    else if (!eq(ok, "false") || !number(&object, "error", ZT_ERR_MAX, &error) || error != response->result) return ZT_ERR_INVALID_ARG;
    if (xSemaphoreTake(tx_lock, 0) != pdTRUE) return ZT_ERR_BUSY;
    if (tx_len) { xSemaphoreGive(tx_lock); return ZT_ERR_BUSY; }
    portENTER_CRITICAL(&pending_guard);
    bool matches = pending && pending_id == response->id;
    zt_console_operation_t op = pending_op;
    portEXIT_CRITICAL(&pending_guard);
    if (!matches) { xSemaphoreGive(tx_lock); return ZT_ERR_INVALID_STATE; }
    /* Provisioning cannot return arbitrary values, even from a mistaken sink. */
    if (op == ZT_CONSOLE_CONFIGURE && response->result == ZT_OK) {
        uint8_t digest[32];
        if (object.count != 3 || !hex_bytes(get(&object, "config_sha256"), digest, sizeof(digest))) {
            xSemaphoreGive(tx_lock); return ZT_ERR_INVALID_ARG;
        }
    }
    if (response->result != ZT_OK && object.count != 3) { xSemaphoreGive(tx_lock); return ZT_ERR_INVALID_ARG; }
    memcpy(tx, response->json, response->len); tx[response->len] = '\n'; tx_len = response->len + 1; tx_at = 0;
    portENTER_CRITICAL(&pending_guard);
    pending = false;
    portEXIT_CRITICAL(&pending_guard);
    xSemaphoreGive(tx_lock); return ZT_OK;
}
zt_err_t zt_console_log(const char *sanitized_text, size_t len)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    if (!sanitized_text || !len || len > LOG_MAX) return ZT_ERR_INVALID_LENGTH;
    for (size_t i = 0; i < len; ++i) if ((unsigned char)sanitized_text[i] < 32 || (unsigned char)sanitized_text[i] > 126) return ZT_ERR_INVALID_ARG;
    if (xSemaphoreTake(tx_lock, 0) != pdTRUE) return ZT_ERR_BUSY;
    uint64_t window = (uint64_t)esp_timer_get_time() / 1000000;
    if (window != log_window) { log_window = window; log_count = 0; }
    if (log_len || log_count == ZT_CONSOLE_LOG_LINES_PER_SECOND) { xSemaphoreGive(tx_lock); return ZT_ERR_BUSY; }
    log_line[0] = '#'; log_line[1] = ' '; memcpy(log_line + 2, sanitized_text, len); log_line[len + 2] = '\n';
    log_len = len + 3; log_at = 0; ++log_count;
    xSemaphoreGive(tx_lock); return ZT_OK;
}
zt_err_t zt_console_service(uint64_t now_us)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    if (xSemaphoreTake(service_lock, 0) != pdTRUE) return ZT_ERR_BUSY;
    /* A partial line owns output until its LF is queued, across service calls. */
    if (xSemaphoreTake(tx_lock, 0) == pdTRUE) {
        bool logging = log_at || (!tx_len && log_len);
        char *data = logging ? log_line : tx;
        size_t *length = logging ? &log_len : &tx_len, *at = logging ? &log_at : &tx_at;
        if (*length) {
            size_t n = *length - *at;
            if (n > ZT_CONSOLE_HOST_WRITE_MAX_BYTES) n = ZT_CONSOLE_HOST_WRITE_MAX_BYTES;
            int sent = usb_serial_jtag_write_bytes(data + *at, n, 0);
            if (sent > 0 && (size_t)sent <= n) *at += sent;
            if (*at == *length) { wipe(data, *length); *at = 0; *length = 0; }
        }
        xSemaphoreGive(tx_lock);
    }
    if (xSemaphoreTake(rx_lock, 0) == pdTRUE) {
        if (line.used && now_us >= last_input_us && now_us - last_input_us >= INPUT_IDLE_US) {
            refusal(prefix_id(), ZT_ERR_TIMEOUT); wipe(&line, sizeof(line));
            line.discarding_overflow = 2; /* Drain the abandoned transaction to LF. */
        }
        xSemaphoreGive(rx_lock);
    }
    uint8_t bytes[ZT_CONSOLE_HOST_WRITE_MAX_BYTES];
    int n = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
    zt_err_t result = n > 0 ? zt_console_receive(bytes, n) : ZT_OK;
    wipe(bytes, sizeof(bytes));
    if (result == ZT_OK) {
        if (xSemaphoreTake(tx_lock, 0) != pdTRUE) result = ZT_ERR_BUSY;
        else {
            if (tx_len || log_len) result = ZT_ERR_BUSY;
            xSemaphoreGive(tx_lock);
        }
    }
    xSemaphoreGive(service_lock); return result;
}
