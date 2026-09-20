#include "zt_radio.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Private interfaces stay within the four translation units of this component. */
extern zt_err_t zt_radio_channel_init(const zt_radio_config_t *config);
extern zt_err_t zt_radio_channel_service(uint64_t now);
extern bool zt_radio_channel_suspended(void);
extern zt_err_t zt_radio_wire_peek(const uint8_t *, size_t, zt_wire_header_t *);
extern void zt_radio_mesh_invalidate(uint32_t generation);
extern void zt_radio_mesh_diagnostics(zt_radio_diagnostics_t *out);
extern uint32_t zt_radio_operating_channel(void);

static const uint8_t broadcast[6] = {255,255,255,255,255,255};
static zt_radio_config_t config;
static zt_rx_sink_t rx_sink;
static zt_tx_sink_t tx_sink;
static void *sink_context;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static StaticQueue_t rx_control;
static uint8_t rx_storage[ZT_RX_QUEUE_CAPACITY * sizeof(zt_rx_frame_t)];
static QueueHandle_t rx_queue;
static TaskHandle_t owner;
static uint32_t generation;
static bool initialized, accepting_callbacks, now_started;
static zt_boot_nonce_t boot_nonce;
static _Atomic uint32_t boot_nonce_ready;
static _Atomic uint32_t diagnostic_mode;
static zt_mac_t allowlist[ZT_LINK_ALLOWLIST_MAX];
static size_t allow_count;
static struct {
    _Atomic uint32_t rx_drops, tx_drops, rx_high_water, tx_high_water, watchdogs, restarts, invalid_lengths;
} counters;

/* Writers are single-owner or already inside guard. Only aligned word loads
 * and stores are needed for publication; the getter never enters a lock. */
static void count_drop(_Atomic uint32_t *counter)
{
    uint32_t n = atomic_load_explicit(counter, memory_order_relaxed);
    if (n != UINT32_MAX) atomic_store_explicit(counter, n + 1, memory_order_relaxed);
}


/* 288 bytes per TX slot, including scheduling metadata. No second TX queue. */
typedef struct {
    uint64_t enqueued_us, not_before_us, expires_us;
    uint32_t request_id;
    uint16_t len;
    uint8_t data[ZT_MAX_FRAME_BYTES];
    uint8_t priority, bucket, state;
} tx_slot_t;
_Static_assert(sizeof(tx_slot_t) <= ZT_RADIO_SLOT_BYTES, "TX budget");
_Static_assert(sizeof(zt_rx_frame_t) <= ZT_RADIO_SLOT_BYTES, "RX budget");
static tx_slot_t slots[ZT_TX_SLOT_CAPACITY];
static int in_flight = -1;
static uint64_t sent_us;
static bool watchdog_reported;
static unsigned restart_stage;
static bool completion_ready;
static zt_tx_completion_t completion;
static uint32_t tokens[5] = {8000000,8000000,2000000,2000000,20000000};
static const uint8_t rates[5] = {8,6,2,2,20}, bursts[5] = {8,8,2,2,20};
static uint64_t replenished_us;
static zt_round_id_t discard_round_request;

zt_err_t zt_radio_discard_round(zt_round_id_t round)
{
    if (!round || !initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    if (discard_round_request && discard_round_request != round) {
        portEXIT_CRITICAL(&guard); return ZT_ERR_BUSY;
    }
    discard_round_request = round;
    portEXIT_CRITICAL(&guard);
    if (owner) xTaskNotifyGive(owner);
    return ZT_OK;
}

const zt_radio_config_t *zt_radio_configuration(void) { return initialized ? &config : NULL; }
zt_boot_nonce_t zt_radio_boot_nonce(void) { return boot_nonce; }

zt_err_t zt_mesh_get_boot_nonce(zt_boot_nonce_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!atomic_load_explicit(&boot_nonce_ready, memory_order_acquire)) return ZT_ERR_INVALID_STATE;
    /* Immutable after the release publication, even across driver restarts. */
    *out = boot_nonce;
    return ZT_OK;
}

zt_err_t zt_radio_get_diagnostics(zt_radio_diagnostics_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    zt_radio_mesh_diagnostics(out);
    out->rx_drops = atomic_load_explicit(&counters.rx_drops, memory_order_relaxed);
    out->tx_drops = atomic_load_explicit(&counters.tx_drops, memory_order_relaxed);
    out->rx_high_water = atomic_load_explicit(&counters.rx_high_water, memory_order_relaxed);
    out->tx_high_water = atomic_load_explicit(&counters.tx_high_water, memory_order_relaxed);
    out->tx_watchdogs = atomic_load_explicit(&counters.watchdogs, memory_order_relaxed);
    out->radio_restarts = atomic_load_explicit(&counters.restarts, memory_order_relaxed);
    uint32_t lengths = atomic_load_explicit(&counters.invalid_lengths, memory_order_relaxed);
    out->invalid_frames = lengths > UINT32_MAX - out->invalid_frames ? UINT32_MAX : out->invalid_frames + lengths;
    out->channel = (uint8_t)zt_radio_operating_channel();
    out->diagnostic_mode = (uint8_t)atomic_load_explicit(&diagnostic_mode, memory_order_relaxed);
    return ZT_OK;
}
uint32_t zt_radio_generation(void)
{
    portENTER_CRITICAL(&guard);
    uint32_t g = generation;
    portEXIT_CRITICAL(&guard);
    return g;
}

static void received(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !info->src_addr || !info->rx_ctrl || !data) return;
    zt_rx_frame_t f;
    portENTER_CRITICAL(&guard);
    bool allowed = accepting_callbacks && allow_count == 0;
    if (accepting_callbacks) for (size_t i = 0; i < allow_count; ++i)
        if (!memcmp(allowlist[i].bytes, info->src_addr, 6)) allowed = true;
    f.driver_generation = generation;
    portEXIT_CRITICAL(&guard);
    if (!allowed) return;
    if (len < 64 || len > ZT_MAX_FRAME_BYTES) { count_drop(&counters.invalid_lengths); return; }
    memcpy(f.src_mac.bytes, info->src_addr, 6);
    f.rssi = info->rx_ctrl->rssi;
    f.channel = info->rx_ctrl->channel;
    f.rx_us = (uint64_t)esp_timer_get_time();
    f.len = (uint16_t)len;
    memcpy(f.data, data, len);
    if (xQueueSend(rx_queue, &f, 0) != pdTRUE) count_drop(&counters.rx_drops);
    else {
        unsigned n = uxQueueMessagesWaiting(rx_queue);
        if (n > atomic_load_explicit(&counters.rx_high_water, memory_order_relaxed))
            atomic_store_explicit(&counters.rx_high_water, n, memory_order_relaxed);
    }
    if (owner) xTaskNotifyGive(owner);
}

static void sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    portENTER_CRITICAL(&guard);
    if (accepting_callbacks && in_flight >= 0 && !completion_ready) {
        completion = (zt_tx_completion_t){.request_id = slots[in_flight].request_id,
            .driver_generation = generation,
            .result = status == ESP_NOW_SEND_SUCCESS ? ZT_OK : ZT_ERR_RADIO};
        completion_ready = true;
    }
    portEXIT_CRITICAL(&guard);
    if (owner) xTaskNotifyGive(owner);
}

static zt_err_t start_now(void)
{
    if (esp_now_init() != ESP_OK) return ZT_ERR_RADIO;
    now_started = true;
    esp_now_peer_info_t peer = {.channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false};
    memcpy(peer.peer_addr, broadcast, 6);
    esp_now_recv_cb_t rx = received;
    esp_now_send_cb_t tx = sent;
    if (esp_now_add_peer(&peer) != ESP_OK || esp_now_register_recv_cb(rx) != ESP_OK ||
        esp_now_register_send_cb(tx) != ESP_OK) {
        esp_now_deinit(); now_started = false;
        return ZT_ERR_RADIO;
    }
    portENTER_CRITICAL(&guard);
    accepting_callbacks = true;
    portEXIT_CRITICAL(&guard);
    return ZT_OK;
}

/* Called exclusively by the channel state machine on the radio owner. The old
 * buffer is held through completed teardown, even if the completion was lost. */
zt_err_t zt_radio_driver_restart(uint8_t channel)
{
    portENTER_CRITICAL(&guard);
    accepting_callbacks = false;
    portEXIT_CRITICAL(&guard);
    if (now_started) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        if (esp_now_deinit() != ESP_OK) return ZT_ERR_RADIO;
        now_started = false;
    }
    if (!restart_stage) {
        esp_err_t stop = esp_wifi_stop();
        if (stop != ESP_OK && stop != ESP_ERR_WIFI_NOT_STARTED) return ZT_ERR_RADIO;
        restart_stage = 1;
    }
    zt_tx_completion_t failed = {0};
    bool report = false;
    portENTER_CRITICAL(&guard);
    if (restart_stage == 1 && in_flight >= 0) {
        failed = (zt_tx_completion_t){slots[in_flight].request_id, generation, ZT_ERR_TIMEOUT};
        slots[in_flight].state = 0;
        in_flight = -1;
        report = true;
    }
    completion_ready = false;
    if (restart_stage == 1) { ++generation; count_drop(&counters.restarts); }
    watchdog_reported = false;
    portEXIT_CRITICAL(&guard);
    xQueueReset(rx_queue);
    zt_radio_mesh_invalidate(generation);
    if (report && tx_sink) tx_sink(&failed, sink_context);
    if (restart_stage == 1) restart_stage = 2;
    if (restart_stage == 2) {
        if (esp_wifi_start() != ESP_OK) return ZT_ERR_RADIO;
        restart_stage = 3;
    }
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20) != ESP_OK ||
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) return ZT_ERR_RADIO;
    zt_err_t r = start_now();
    if (r == ZT_OK) restart_stage = 0;
    return r;
}

zt_err_t zt_radio_init(const zt_radio_config_t *cfg, zt_rx_sink_t rx, zt_tx_sink_t tx, void *context)
{
    if (!cfg || !rx || cfg->last_channel < 1 || cfg->last_channel > 11) return ZT_ERR_INVALID_ARG;
    if (initialized) return ZT_ERR_INVALID_STATE;
    if (cfg->is_host && memcmp(cfg->host_mac.bytes, cfg->self_mac.bytes, 6)) return ZT_ERR_AUTH;
    config = *cfg; rx_sink = rx; tx_sink = tx; sink_context = context;
    rx_queue = xQueueCreateStatic(ZT_RX_QUEUE_CAPACITY, sizeof(zt_rx_frame_t), rx_storage, &rx_control);
    if (!rx_queue) return ZT_ERR_NO_SPACE;
    generation = 1;
    zt_err_t r = zt_radio_channel_init(cfg);
    if (r != ZT_OK) return r;
    uint8_t actual_mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, actual_mac) != ESP_OK || memcmp(actual_mac, cfg->self_mac.bytes, 6)) return ZT_ERR_AUTH;
    r = start_now();
    if (r != ZT_OK) return r;
    if (!atomic_load_explicit(&boot_nonce_ready, memory_order_relaxed)) {
        esp_fill_random(&boot_nonce, sizeof(boot_nonce));
        atomic_store_explicit(&boot_nonce_ready, 1, memory_order_release);
    }
    initialized = true;
    replenished_us = (uint64_t)esp_timer_get_time();
    return ZT_OK;
}

zt_err_t zt_radio_submit(const zt_tx_frame_t *frame)
{
    if (!initialized) return ZT_ERR_INVALID_STATE;
    if (!frame || frame->len < 64 || frame->len > 250 ||
        frame->priority < ZT_TX_PRIO_DIRECT_TAG || frame->priority > ZT_TX_PRIO_COSMETIC ||
        frame->bucket < ZT_BUCKET_CRITICAL || frame->bucket > ZT_BUCKET_BEACON)
        return ZT_ERR_INVALID_ARG;
    zt_wire_header_t h;
    zt_err_t r = zt_radio_wire_peek(frame->data, frame->len, &h);
    if (r != ZT_OK) return r;
    int free_slot = -1, replace = -1, shed = -1;
    bool discarded = false;
    zt_tx_completion_t dropped = {0};
    unsigned used = 0;
    portENTER_CRITICAL(&guard);
    for (unsigned i = 0; i < ZT_TX_SLOT_CAPACITY; ++i) {
        if (!slots[i].state) free_slot = i;
        else ++used;
        if (slots[i].state != 1) continue;
        if (slots[i].priority > frame->priority && (shed < 0 || slots[i].priority > slots[shed].priority)) shed = i;
        if ((h.type == ZT_PKT_HOST_STATE || h.type == ZT_PKT_WATERMARKS || h.type == ZT_PKT_BEACON) &&
            slots[i].data[3] == h.type && !memcmp(slots[i].data + 18, frame->data + 18, 22)) {
            bool same_page = h.type != ZT_PKT_HOST_STATE ||
                (slots[i].data[61] == frame->data[61] && slots[i].data[62] == frame->data[62]);
            if (same_page) replace = i;
        }
    }
    bool constrained = free_slot < 0 || (frame->priority != ZT_TX_PRIO_DIRECT_TAG && used >= ZT_TX_SLOT_CAPACITY - ZT_TX_CRITICAL_RESERVE);
    if (replace < 0 && constrained) replace = shed;
    if (replace >= 0) {
        free_slot = replace;
        dropped = (zt_tx_completion_t){slots[replace].request_id, generation, ZT_ERR_STALE};
        discarded = true;
        --used;
    }
    if (free_slot < 0 || (frame->priority != ZT_TX_PRIO_DIRECT_TAG && used >= ZT_TX_SLOT_CAPACITY - ZT_TX_CRITICAL_RESERVE)) {
        count_drop(&counters.tx_drops);
        portEXIT_CRITICAL(&guard);
        return ZT_ERR_NO_SPACE;
    }
    tx_slot_t *s = &slots[free_slot];
    s->len = frame->len;
    memcpy(s->data, frame->data, frame->len);
    s->priority = frame->priority; s->bucket = frame->bucket;
    s->not_before_us = frame->not_before_us; s->expires_us = frame->expires_us;
    s->request_id = frame->request_id;
    s->enqueued_us = (uint64_t)esp_timer_get_time();
    if (h.type == ZT_PKT_HOST_STATE || (h.type == ZT_PKT_COMMAND && s->data[52] == ZT_CMD_START_ROUND)) {
        uint64_t deadline = s->enqueued_us + ZT_CLOCK_QUEUE_MAX_MS * 1000;
        if (!s->expires_us || s->expires_us > deadline) s->expires_us = deadline;
    }
    s->state = 1;
    if (++used > atomic_load_explicit(&counters.tx_high_water, memory_order_relaxed))
        atomic_store_explicit(&counters.tx_high_water, used, memory_order_relaxed);
    portEXIT_CRITICAL(&guard);
    if (discarded && tx_sink) tx_sink(&dropped, sink_context);
    if (owner) xTaskNotifyGive(owner);
    return ZT_OK;
}

static void finish_slot(int index, zt_err_t result)
{
    zt_tx_completion_t c;
    portENTER_CRITICAL(&guard);
    c = (zt_tx_completion_t){slots[index].request_id, generation, result};
    slots[index].state = 0;
    if (in_flight == index) { in_flight = -1; watchdog_reported = false; }
    portEXIT_CRITICAL(&guard);
    if (tx_sink) tx_sink(&c, sink_context);
}

zt_err_t zt_radio_service(uint64_t now)
{
    if (!initialized) return ZT_ERR_INVALID_STATE;
    if (!owner) owner = xTaskGetCurrentTaskHandle();
    if (owner != xTaskGetCurrentTaskHandle()) return ZT_ERR_BUSY;
    zt_err_t r = zt_radio_channel_service(now);
    if (r != ZT_OK) return r;
    portENTER_CRITICAL(&guard);
    zt_round_id_t discard = discard_round_request;
    discard_round_request = 0;
    portEXIT_CRITICAL(&guard);
    if (discard) for (unsigned i = 0; i < ZT_TX_SLOT_CAPACITY; ++i) {
        /* Own durable reset must not cancel the flood still carrying that
         * reset to the next badge, or a cleanup receipt travelling back. */
        if (slots[i].state != 1 || slots[i].data[3] == ZT_PKT_COMMAND_RECEIPT ||
            (slots[i].data[3] == ZT_PKT_COMMAND && slots[i].data[52] == ZT_CMD_RESET_GAME)) continue;
        zt_round_id_t round = 0;
        for (unsigned byte = 0; byte < 8; ++byte) round |= (uint64_t)slots[i].data[18 + byte] << (byte * 8);
        if (round == discard) finish_slot(i, ZT_ERR_STALE);
    }
    portENTER_CRITICAL(&guard);
    bool ready = completion_ready;
    zt_tx_completion_t c = completion;
    completion_ready = false;
    int active = in_flight;
    portEXIT_CRITICAL(&guard);
    if (ready && active >= 0 && c.driver_generation == generation) finish_slot(active, c.result);
    if (in_flight >= 0 && !watchdog_reported && now >= sent_us && now - sent_us >= ZT_TX_CALLBACK_TIMEOUT_MS * 1000) {
        zt_err_t recover = zt_channel_recover(now);
        if (recover == ZT_OK) { count_drop(&counters.watchdogs); watchdog_reported = true; }
        return recover;
    }
    /* Bounded receive slice, so a busy channel cannot starve TX/watchdogs. */
    zt_rx_frame_t f;
    for (unsigned i = 0; i < 8 && xQueuePeek(rx_queue, &f, 0) == pdTRUE; ++i) {
        portENTER_CRITICAL(&guard);
        bool allowed = allow_count == 0;
        for (size_t j = 0; j < allow_count; ++j) if (!memcmp(allowlist[j].bytes, f.src_mac.bytes, 6)) allowed = true;
        portEXIT_CRITICAL(&guard);
        zt_err_t accepted = ZT_OK;
        if (allowed && f.driver_generation == generation) accepted = rx_sink(&f, sink_context);
        if (accepted == ZT_ERR_BUSY) break;
        xQueueReceive(rx_queue, &f, 0);
    }
    if (in_flight >= 0 || zt_radio_channel_suspended()) return ZT_OK;
    uint64_t delta = now >= replenished_us ? now - replenished_us : 0;
    if (delta > 1000000) delta = 1000000;
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t n = tokens[i] + delta * rates[i];
        tokens[i] = n > bursts[i] * 1000000u ? bursts[i] * 1000000u : n;
    }
    replenished_us = now;
    int best = -1;
    for (unsigned i = 0; i < ZT_TX_SLOT_CAPACITY; ++i) {
        if (slots[i].state != 1) continue;
        if (slots[i].expires_us && now >= slots[i].expires_us) { finish_slot(i, ZT_ERR_STALE); continue; }
        if (slots[i].not_before_us > now || tokens[slots[i].bucket] < 1000000 || tokens[4] < 1000000) continue;
        if (best < 0 || slots[i].priority < slots[best].priority ||
            (slots[i].priority == slots[best].priority && slots[i].enqueued_us < slots[best].enqueued_us)) best = i;
    }
    if (best < 0) return ZT_OK;
    tx_slot_t *s = &slots[best];
    uint32_t age = (uint32_t)s->data[44] | (uint32_t)s->data[45] << 8 |
        (uint32_t)s->data[46] << 16 | (uint32_t)s->data[47] << 24;
    uint64_t new_age = age + (now > s->enqueued_us ? (now - s->enqueued_us + 999) / 1000 : 0) + ZT_LINK_AGE_ALLOWANCE_MS;
    if (new_age > UINT32_MAX || ((s->data[3] == ZT_PKT_HOST_STATE || (s->data[3] == ZT_PKT_COMMAND && s->data[52] == ZT_CMD_START_ROUND)) && new_age > ZT_CLOCK_AGE_MAX_MS)) {
        finish_slot(best, ZT_ERR_STALE); return ZT_OK;
    }
    for (unsigned i = 0; i < 4; ++i) s->data[44+i] = (uint8_t)(new_age >> (8*i));
    r = zt_wire_hmac(config.group_key, s->data, s->len - 16, s->data + s->len - 16);
    if (r != ZT_OK) { finish_slot(best, r); return r; }
    portENTER_CRITICAL(&guard);
    in_flight = best; s->state = 2; sent_us = now;
    portEXIT_CRITICAL(&guard);
    tokens[s->bucket] -= 1000000; tokens[4] -= 1000000;
    if (esp_now_send(broadcast, s->data, s->len) != ESP_OK) finish_slot(best, ZT_ERR_RADIO);
    return ZT_OK;
}

zt_err_t zt_radio_link_allowlist(const zt_mac_t *macs, size_t count)
{
    if ((!macs && count) || count > ZT_LINK_ALLOWLIST_MAX) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    for (size_t i = 0; i < count; ++i) allowlist[i] = macs[i];
    allow_count = count;
    atomic_store_explicit(&diagnostic_mode, count != 0, memory_order_relaxed);
    portEXIT_CRITICAL(&guard);
    return ZT_OK;
}
