#include "zt_radio.h"
#include "zt_store.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

extern uint32_t zt_radio_generation(void);
extern zt_err_t zt_radio_driver_restart(uint8_t channel);
extern void zt_radio_mesh_invalidate(uint32_t generation);

static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static zt_channel_status_t status, published_status;
static zt_round_id_t published_round;
/* Mesh observations are copied to the radio owner; only that owner changes
 * discovery state. A lobby contact is a lease, never a permanent round lock. */
static struct { uint64_t rx_us; uint8_t channel; } discovery_observation;
static uint64_t lobby_seen_us;
static _Atomic uint32_t operating_channel;

uint32_t zt_radio_operating_channel(void)
{
    return atomic_load_explicit(&operating_channel, memory_order_relaxed);
}
static zt_round_id_t locked_round;
static bool host, standalone, ready, scanning, connecting, cancelling, force_restart;
static bool scan_list_owned, reading_results, have_candidate, disconnected;
static uint8_t allowed[11], allowed_count, discovery_index, backoff_index;
static uint8_t last_bssid[6];
static bool have_bssid;
static uint64_t dwell_due, retry_due, association_due, ip_due, check_due, cancel_due, scan_due;
static wifi_config_t station;
static wifi_ap_record_t candidate;
static uint16_t results_remaining;
static esp_netif_t *netif;
static StaticQueue_t events_control;
static uint8_t events_bytes[8 * sizeof(zt_sta_event_t)];
static QueueHandle_t events;
static _Atomic bool event_overflow;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static const uint32_t reconnect_backoff[] = ZT_STA_RECONNECT_BACKOFF_MS;
static struct { unsigned kind; zt_round_id_t round; uint8_t channel; } request;

static bool is_allowed(uint8_t channel)
{
    for (unsigned i = 0; i < allowed_count; ++i) if (allowed[i] == channel) return true;
    return false;
}

static void on_sdk_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    zt_sta_event_t e = {.driver_generation = zt_radio_generation()};
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: e.kind = ZT_STA_DISCONNECTED; break;
        case WIFI_EVENT_SCAN_DONE: e.kind = ZT_STA_SCAN_DONE; break;
        case WIFI_EVENT_HOME_CHANNEL_CHANGE:
            e.kind = ZT_STA_HOME_CHANNEL_CHANGED;
            e.channel = ((wifi_event_home_channel_change_t *)data)->new_chan;
            break;
        default: return;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) e.kind = ZT_STA_GOT_IP;
    else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) e.kind = ZT_STA_LOST_IP;
    else return;
    if (zt_channel_post_sta_event(&e) != ZT_OK) event_overflow = true;
}

zt_err_t zt_radio_channel_init(const zt_radio_config_t *cfg)
{
    if (ready) return ZT_ERR_INVALID_STATE;
    host = cfg->is_host;
    standalone = false;
    events = xQueueCreateStatic(8, sizeof(zt_sta_event_t), events_bytes, &events_control);
    esp_err_t r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return ZT_ERR_RADIO;
    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return ZT_ERR_RADIO;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) return ZT_ERR_RADIO;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;
    if (esp_wifi_init(&init) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_country_code(ZT_RADIO_COUNTRY, false) != ESP_OK)
        return ZT_ERR_RADIO;
    wifi_country_t country;
    if (esp_wifi_get_country(&country) != ESP_OK) return ZT_ERR_RADIO;
    for (unsigned c = country.schan; c < (unsigned)country.schan + country.nchan; ++c)
        if (c >= 1 && c <= 11) allowed[allowed_count++] = c;
    if (!allowed_count || !is_allowed(cfg->last_channel)) return ZT_ERR_PROTOCOL;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_sdk_event, NULL, &wifi_handler) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_sdk_event, NULL, &ip_handler) != ESP_OK)
        return ZT_ERR_RADIO;
    if (esp_wifi_start() != ESP_OK || esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20) != ESP_OK ||
        esp_wifi_set_channel(cfg->last_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) return ZT_ERR_RADIO;
    atomic_store_explicit(&operating_channel, cfg->last_channel, memory_order_relaxed);
    status = (zt_channel_status_t){.state = host ? ZT_CHANNEL_LOBBY : ZT_CHANNEL_DISCOVERY,
        .channel = cfg->last_channel, .driver_generation = zt_radio_generation()};
    published_status = status;
    if (host) {
        zt_config_t provisioned;
        zt_err_t z = zt_store_load_config(&provisioned);
        if (z != ZT_OK) return z;
        size_t n = strnlen(provisioned.ssid, sizeof(provisioned.ssid));
        size_t p = strnlen(provisioned.password, sizeof(provisioned.password));
        standalone = !provisioned.host_credentials_present || !n;
        if (!standalone) {
            if (n > 32 || p < 8 || p > 63) return ZT_ERR_INVALID_ARG;
            memcpy(station.sta.ssid, provisioned.ssid, n);
            memcpy(station.sta.password, provisioned.password, p);
            station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            station.sta.scan_method = WIFI_FAST_SCAN;
            station.sta.failure_retry_cnt = 0;
            station.sta.rm_enabled = 0; station.sta.btm_enabled = 0;
            station.sta.mbo_enabled = 0; station.sta.ft_enabled = 0;
            station.sta.pmf_cfg.capable = true;
        }
        memset(&provisioned, 0, sizeof(provisioned));
    }
    ready = true;
    disconnected = true;
    uint64_t now = (uint64_t)esp_timer_get_time();
    dwell_due = now + ZT_DISCOVERY_LAST_CHANNEL_MS * 1000;
    retry_due = now + reconnect_backoff[0] * 1000;
    backoff_index = 1;
    return ZT_OK;
}

static zt_err_t queue_request(unsigned kind, zt_round_id_t round, uint8_t channel)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    if (request.kind) { portEXIT_CRITICAL(&guard); return ZT_ERR_BUSY; }
    request.kind = kind; request.round = round; request.channel = channel;
    portEXIT_CRITICAL(&guard);
    return ZT_OK;
}
zt_err_t zt_channel_start_discovery(uint8_t last_channel)
{
    if (!is_allowed(last_channel)) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    bool locked = published_round != 0;
    portEXIT_CRITICAL(&guard);
    if (locked) return ZT_ERR_INVALID_STATE;
    return queue_request(1, 0, last_channel);
}
zt_err_t zt_channel_lock(zt_round_id_t round_id, uint8_t channel)
{
    if (!round_id || !is_allowed(channel)) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&guard);
    bool conflict = published_round && (round_id != published_round || channel != published_status.channel);
    bool complete = published_round == round_id && channel == published_status.channel && published_status.state == ZT_CHANNEL_LOCKED;
    portEXIT_CRITICAL(&guard);
    if (conflict) return ZT_ERR_CONFLICT;
    if (complete) return ZT_OK;
    return queue_request(2, round_id, channel);
}
zt_err_t zt_channel_unlock_after_expiry(zt_round_id_t round_id)
{
    portENTER_CRITICAL(&guard);
    bool match = round_id && round_id == published_round;
    uint8_t channel = published_status.channel;
    portEXIT_CRITICAL(&guard);
    if (!match) return ZT_ERR_INVALID_STATE;
    return queue_request(3, round_id, channel);
}
zt_err_t zt_channel_recover(uint64_t now_us)
{
    (void)now_us;
    return queue_request(4, 0, 0); /* Recovery reads the radio owner's state. */
}
zt_err_t zt_channel_get_status(zt_channel_status_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!ready) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    *out = published_status;
    portEXIT_CRITICAL(&guard);
    return ZT_OK;
}
zt_err_t zt_channel_post_sta_event(const zt_sta_event_t *event)
{
    if (!event || !events || (event->kind < ZT_STA_DISCONNECTED || event->kind > ZT_STA_SCAN_DONE)) return ZT_ERR_INVALID_ARG;
    return xQueueSend(events, event, 0) == pdTRUE ? ZT_OK : ZT_ERR_NO_SPACE;
}
bool zt_radio_channel_suspended(void)
{
    /* A configured lobby host advertises only after it has joined the hotspot;
     * otherwise players can adopt its remembered channel before it moves. */
    return cancelling || connecting || scanning || reading_results ||
        (host && !standalone && !locked_round && !status.has_ip);
}

/* Discovery is passive ESP-NOW listening, never an SDK AP scan. */
static zt_err_t select_channel(uint8_t channel)
{
    if (scanning || reading_results || connecting || cancelling || status.associated) return ZT_ERR_BUSY;
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) return ZT_ERR_RADIO;
    status.channel = channel;
    atomic_store_explicit(&operating_channel, channel, memory_order_relaxed);
    zt_radio_mesh_invalidate(zt_radio_generation());
    return ZT_OK;
}

static void schedule_retry(uint64_t now)
{
    retry_due = now + (uint64_t)reconnect_backoff[backoff_index] * 1000;
    if (backoff_index < 3) ++backoff_index;
}
static void release_scan(void)
{
    if (scan_list_owned) { esp_wifi_clear_ap_list(); scan_list_owned = false; }
    reading_results = false;
}
static void cancel_operation(uint64_t now, bool restart)
{
    if (cancelling) { force_restart |= restart; return; }
    cancelling = true; force_restart = restart;
    status.state = ZT_CHANNEL_RECOVERING;
    cancel_due = now + ZT_RADIO_CANCEL_TIMEOUT_MS * 1000;
    if (scanning) esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_netif_dhcpc_stop(netif);
    status.associated = status.has_ip = 0;
    zt_radio_mesh_invalidate(zt_radio_generation());
}

static bool security_matches(wifi_auth_mode_t mode)
{
    return mode == WIFI_AUTH_WPA2_PSK || mode == WIFI_AUTH_WPA2_WPA3_PSK || mode == WIFI_AUTH_WPA3_PSK;
}

static zt_err_t begin_scan(uint64_t now)
{
    /* Lobby may find the hotspot on any permitted 2.4-GHz channel. A locked
     * round always uses exactly its persisted channel, including retries. */
    wifi_scan_config_t scan = {.ssid = station.sta.ssid,
        .channel = locked_round ? status.channel : 0,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = ZT_STA_SCAN_MIN_MS, .max = ZT_STA_SCAN_MAX_MS}};
    uint64_t timeout_us = 3000000;
    if (!locked_round) {
        /* Phone hotspots may answer slowly: use a longer lobby-only dwell,
         * preserving the frozen round timing. Allow a full 2.4-GHz pass plus
         * channel transitions and event delivery before the scan watchdog. */
        scan.show_hidden = true;
        scan.scan_time.active.min = 250;
        scan.scan_time.active.max = 500;
        timeout_us = 10000000;
    }
    zt_radio_mesh_invalidate(zt_radio_generation());
    if (esp_wifi_scan_start(&scan, false) != ESP_OK) { schedule_retry(now); return ZT_ERR_RADIO; }
    scanning = true; scan_list_owned = true; have_candidate = false;
    scan_due = now + timeout_us;
    return ZT_OK;
}

/* Only authenticated discovery traffic reaches this private hook. */
zt_err_t zt_radio_channel_discovered(uint8_t channel, uint64_t rx_us)
{
    if (!ready || host || !is_allowed(channel)) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    if (rx_us > discovery_observation.rx_us) {
        discovery_observation.rx_us = rx_us;
        discovery_observation.channel = channel;
    }
    portEXIT_CRITICAL(&guard);
    return ZT_OK;
}

static zt_err_t service_channel(uint64_t now)
{
    if (!ready) return ZT_ERR_INVALID_STATE;
    unsigned command;
    uint8_t desired;
    zt_round_id_t round;
    portENTER_CRITICAL(&guard);
    command = request.kind; desired = request.channel; round = request.round;
    request.kind = 0;
    portEXIT_CRITICAL(&guard);
    if (command && command != 4 && (scanning || reading_results || connecting || cancelling)) {
        portENTER_CRITICAL(&guard);
        if (!request.kind) { request.kind = command; request.round = round; request.channel = desired; }
        portEXIT_CRITICAL(&guard);
        command = 0;
    }
    bool overflow = atomic_exchange_explicit(&event_overflow, false, memory_order_relaxed);
    if (command == 4 || overflow) {
        cancel_operation(now, true);
    } else if (command) {
        if (scanning || connecting || cancelling) return ZT_ERR_BUSY;
        if (command == 2) {
            if (locked_round && (round != locked_round || desired != status.channel)) return ZT_ERR_CONFLICT;
            if (status.associated && desired != status.channel) return ZT_ERR_CONFLICT;
            if (!status.associated && select_channel(desired) != ZT_OK) return ZT_ERR_RADIO;
            locked_round = round; status.state = ZT_CHANNEL_LOCKED;
        } else {
            if (command == 3) locked_round = 0;
            if (locked_round) return ZT_ERR_INVALID_STATE;
            status.state = host ? ZT_CHANNEL_LOBBY : ZT_CHANNEL_DISCOVERY;
            lobby_seen_us = 0;
            discovery_index = 0;
            if (!host && select_channel(desired) != ZT_OK) return ZT_ERR_RADIO;
            dwell_due = now + ZT_DISCOVERY_LAST_CHANNEL_MS * 1000;
        }
    }
    portENTER_CRITICAL(&guard);
    uint64_t observed_us = discovery_observation.rx_us;
    uint8_t observed_channel = discovery_observation.channel;
    discovery_observation.rx_us = 0;
    portEXIT_CRITICAL(&guard);
    if (!host && !locked_round && !cancelling && observed_channel == status.channel &&
        observed_us && observed_us <= now && now - observed_us <= ZT_CLOCK_AGE_MAX_MS * 1000ULL) {
        lobby_seen_us = observed_us;
        status.state = ZT_CHANNEL_LOBBY;
    }
    if (!host && !locked_round && status.state == ZT_CHANNEL_LOBBY && lobby_seen_us &&
        now - lobby_seen_us >= ZT_GATEWAY_DISCOVERY_MAX_AGE_S * 1000000ULL) {
        status.state = ZT_CHANNEL_DISCOVERY;
        discovery_index = 0;
        dwell_due = now;
        lobby_seen_us = 0;
        zt_radio_mesh_invalidate(zt_radio_generation());
    }
    zt_sta_event_t event;
    for (unsigned i = 0; i < 8 && xQueueReceive(events, &event, 0) == pdTRUE; ++i) {
        if (event.driver_generation != zt_radio_generation()) continue;
        /* A standalone host has no STA link or scan results to process. */
        if (standalone) continue;
        switch (event.kind) {
        case ZT_STA_DISCONNECTED:
            disconnected = true; status.associated = status.has_ip = 0;
            if (connecting && !cancelling) cancel_operation(now, false);
            if (!cancelling) schedule_retry(now);
            break;
        case ZT_STA_GOT_IP:
            if (!cancelling) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && (!locked_round || ap.primary == status.channel)) {
                    status.associated = status.has_ip = 1; connecting = false;
                    disconnected = false; backoff_index = 0;
                    memcpy(last_bssid, ap.bssid, 6); have_bssid = true;
                    if (!locked_round) status.channel = ap.primary;
                    atomic_store_explicit(&operating_channel, ap.primary, memory_order_relaxed);
                } else cancel_operation(now, false);
            }
            break;
        case ZT_STA_LOST_IP:
            status.has_ip = 0;
            ip_due = now + ZT_STA_IP_TIMEOUT_MS * 1000;
            break;
        case ZT_STA_HOME_CHANNEL_CHANGED:
            atomic_store_explicit(&operating_channel, event.channel, memory_order_relaxed);
            if (locked_round && event.channel != status.channel) cancel_operation(now, false);
            else if (!locked_round && is_allowed(event.channel)) status.channel = event.channel;
            break;
        case ZT_STA_SCAN_DONE:
            if (!scanning) break;
            scanning = false;
            if (cancelling) release_scan();
            else if (esp_wifi_scan_get_ap_num(&results_remaining) != ESP_OK) {
                release_scan(); cancel_operation(now, false);
            } else reading_results = true;
            break;
        }
    }
    if (cancelling) {
        wifi_ap_record_t ap;
        bool settled = !scanning && esp_wifi_sta_get_ap_info(&ap) == ESP_ERR_WIFI_NOT_CONNECT;
        if ((settled && !force_restart) || now >= cancel_due) {
            release_scan();
            if (force_restart || !settled) {
                zt_err_t r = zt_radio_driver_restart(status.channel);
                if (r != ZT_OK) { cancel_due = now + 500000; return r; }
                status.driver_generation = zt_radio_generation();
                xQueueReset(events);
            }
            scanning = connecting = cancelling = false;
            disconnected = true;
            status.state = locked_round ? ZT_CHANNEL_LOCKED : (host ? ZT_CHANNEL_LOBBY : ZT_CHANNEL_DISCOVERY);
            if (select_channel(status.channel) != ZT_OK) return ZT_ERR_RADIO;
            schedule_retry(now);
        }
        return ZT_OK;
    }
    /* Keep the radio available for ESP-NOW without entering the AP retry ladder.
     * Explicit round requests and driver recovery above still apply. */
    if (standalone) return ZT_OK;
    if (scanning && now >= scan_due) { cancel_operation(now, false); return ZT_OK; }
    if (reading_results) {
        /* Pop/free each SDK result exactly once; at most eight per iteration. */
        for (unsigned i = 0; i < 8 && results_remaining; ++i) {
            wifi_ap_record_t ap;
            if (esp_wifi_scan_get_ap_record(&ap) != ESP_OK) { release_scan(); cancel_operation(now, false); return ZT_ERR_RADIO; }
            --results_remaining;
            if (!results_remaining) scan_list_owned = false;
            size_t n = strnlen((const char *)station.sta.ssid, sizeof(station.sta.ssid));
            if (strnlen((const char *)ap.ssid, sizeof(ap.ssid)) != n || memcmp(ap.ssid, station.sta.ssid, n) ||
                !security_matches(ap.authmode) || !is_allowed(ap.primary) || (locked_round && ap.primary != status.channel)) continue;
            bool preferred = have_bssid && !memcmp(ap.bssid, last_bssid, 6);
            bool old_preferred = have_candidate && have_bssid && !memcmp(candidate.bssid, last_bssid, 6);
            if (!have_candidate || preferred || (!old_preferred && ap.rssi > candidate.rssi)) { candidate = ap; have_candidate = true; }
        }
        if (!results_remaining) {
            release_scan();
            if (have_candidate) {
                station.sta.bssid_set = true;
                memcpy(station.sta.bssid, candidate.bssid, 6);
                station.sta.channel = candidate.primary;
                if (!locked_round) status.channel = candidate.primary;
                /* Cancellation explicitly stops DHCP. ESP-NETIF treats STOPPED
                 * as static-IP mode on the next association, so re-arm it before
                 * connecting; starting while down restores the INIT state. */
                esp_err_t dhcp = esp_netif_dhcpc_start(netif);
                if ((dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) ||
                    esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK || esp_wifi_connect() != ESP_OK) {
                    cancel_operation(now, false); return ZT_ERR_RADIO;
                }
                connecting = true; disconnected = false;
                association_due = now + ZT_STA_ASSOC_TIMEOUT_MS * 1000;
                ip_due = 0; check_due = now;
            } else {
                if (!locked_round) discovery_index = (discovery_index + 1) % allowed_count;
                schedule_retry(now);
            }
        }
    }
    if (scanning || connecting || status.associated) {
        if (now >= check_due) {
            uint8_t primary; wifi_second_chan_t secondary;
            check_due = now + ZT_CHANNEL_CHECK_MS * 1000;
            esp_err_t channel_result = esp_wifi_get_channel(&primary, &secondary);
            if (channel_result == ESP_OK) atomic_store_explicit(&operating_channel, primary, memory_order_relaxed);
            if (channel_result != ESP_OK || (locked_round && primary != status.channel)) {
                cancel_operation(now, false); return ZT_OK;
            }
            wifi_ap_record_t ap;
            if (connecting && !status.associated && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                status.associated = 1; ip_due = now + ZT_STA_IP_TIMEOUT_MS * 1000;
                memcpy(last_bssid, ap.bssid, 6); have_bssid = true;
            }
        }
        if ((connecting && !status.associated && now >= association_due) || (status.associated && !status.has_ip && ip_due && now >= ip_due))
            cancel_operation(now, false);
    }
    if (!scanning && !reading_results && !connecting && !status.associated && host && now >= retry_due) return begin_scan(now);
    if (!host && status.state == ZT_CHANNEL_DISCOVERY && now >= dwell_due) {
        if (discovery_index == allowed_count) { discovery_index = 0; dwell_due = now + ZT_DISCOVERY_BACKOFF_MS * 1000; }
        else {
            zt_err_t r = select_channel(allowed[discovery_index++]);
            if (r != ZT_OK) return r;
            dwell_due = now + ZT_DISCOVERY_DWELL_MS * 1000;
        }
    }
    return ZT_OK;
}

zt_err_t zt_radio_channel_service(uint64_t now)
{
    zt_err_t result = service_channel(now);
    portENTER_CRITICAL(&guard);
    published_status = status;
    published_round = locked_round;
    portEXIT_CRITICAL(&guard);
    return result;
}
