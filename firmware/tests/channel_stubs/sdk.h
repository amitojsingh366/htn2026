#pragma once
/* Minimal SDK surface for running the real channel owner on the host. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "zt_radio.h"

typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_WIFI_NOT_CONNECT,
    ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED };
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
extern const char test_wifi_event[], test_ip_event[];
#define WIFI_EVENT test_wifi_event
#define IP_EVENT test_ip_event
enum { ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_DISCONNECTED,
    WIFI_EVENT_SCAN_DONE, WIFI_EVENT_HOME_CHANNEL_CHANGE,
    IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP };
typedef struct { uint8_t new_chan; } wifi_event_home_channel_change_t;
typedef int esp_netif_t;
typedef int wifi_auth_mode_t;
typedef int wifi_second_chan_t;
enum { WIFI_STORAGE_RAM, WIFI_MODE_STA, WIFI_PS_NONE, WIFI_IF_STA,
    WIFI_BW_HT20, WIFI_SECOND_CHAN_NONE, WIFI_AUTH_WPA2_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_PSK, WIFI_FAST_SCAN,
    WIFI_SCAN_TYPE_ACTIVE };
typedef struct { int nvs_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct { uint8_t schan, nchan; } wifi_country_t;
typedef struct {
    struct {
        uint8_t ssid[32], password[64], bssid[6], channel;
        struct { wifi_auth_mode_t authmode; } threshold;
        int scan_method, failure_retry_cnt;
        bool rm_enabled, btm_enabled, mbo_enabled, ft_enabled, bssid_set;
        struct { bool capable; } pmf_cfg;
    } sta;
} wifi_config_t;
typedef struct {
    uint8_t ssid[33], bssid[6], primary;
    wifi_auth_mode_t authmode;
    int8_t rssi;
} wifi_ap_record_t;
typedef struct {
    uint8_t *ssid;
    uint8_t channel;
    int scan_type;
    bool show_hidden;
    struct { struct { unsigned min, max; } active; } scan_time;
} wifi_scan_config_t;

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(guard) ((void)(guard))
#define portEXIT_CRITICAL(guard) ((void)(guard))
#define pdTRUE 1
typedef struct {
    uint8_t *storage;
    unsigned length, item_size, head, count;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned length, unsigned size,
    uint8_t *storage, StaticQueue_t *queue)
{
    *queue = (StaticQueue_t){.storage = storage, .length = length, .item_size = size};
    return queue;
}
static inline int xQueueSend(QueueHandle_t queue, const void *item, unsigned wait)
{
    (void)wait;
    if (queue->count == queue->length) return 0;
    unsigned slot = (queue->head + queue->count++) % queue->length;
    memcpy(queue->storage + slot * queue->item_size, item, queue->item_size);
    return pdTRUE;
}
static inline int xQueueReceive(QueueHandle_t queue, void *item, unsigned wait)
{
    (void)wait;
    if (!queue->count) return 0;
    memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    --queue->count;
    return pdTRUE;
}
static inline void xQueueReset(QueueHandle_t queue) { queue->head = queue->count = 0; }

extern uint64_t test_now_us;
extern uint8_t test_wifi_channel;
extern unsigned test_set_channel_calls, test_disconnect_calls, test_scan_calls;
extern bool test_fail_set_channel;
static inline int64_t esp_timer_get_time(void) { return (int64_t)test_now_us; }
static inline esp_err_t esp_netif_init(void) { return ESP_OK; }
static inline esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void)
{ static esp_netif_t netif; return &netif; }
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t base,
    int id, void (*handler)(void *, esp_event_base_t, int32_t, void *), void *arg,
    esp_event_handler_instance_t *instance)
{ (void)base; (void)id; (void)handler; (void)arg; (void)instance; return ESP_OK; }
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{ (void)config; return ESP_OK; }
static inline esp_err_t esp_wifi_set_storage(int value) { (void)value; return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(int value) { (void)value; return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(int value) { (void)value; return ESP_OK; }
static inline esp_err_t esp_wifi_set_bandwidth(int iface, int width)
{ (void)iface; (void)width; return ESP_OK; }
static inline esp_err_t esp_wifi_set_country_code(const char *country, bool policy)
{ (void)country; (void)policy; return ESP_OK; }
static inline esp_err_t esp_wifi_get_country(wifi_country_t *country)
{ *country = (wifi_country_t){.schan = 1, .nchan = 11}; return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_channel(uint8_t channel, wifi_second_chan_t secondary)
{
    (void)secondary;
    ++test_set_channel_calls;
    if (test_fail_set_channel) return ESP_ERR_INVALID_STATE;
    test_wifi_channel = channel;
    return ESP_OK;
}
static inline esp_err_t esp_wifi_get_channel(uint8_t *channel, wifi_second_chan_t *secondary)
{ *channel = test_wifi_channel; *secondary = WIFI_SECOND_CHAN_NONE; return ESP_OK; }
static inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ (void)ap; return ESP_ERR_WIFI_NOT_CONNECT; }
static inline esp_err_t esp_wifi_disconnect(void) { ++test_disconnect_calls; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking)
{ (void)config; (void)blocking; ++test_scan_calls; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_stop(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_clear_ap_list(void) { return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) { *count = 0; return ESP_OK; }
static inline esp_err_t esp_wifi_scan_get_ap_record(wifi_ap_record_t *ap)
{ (void)ap; assert(!"clients must not read AP scans"); return ESP_ERR_INVALID_STATE; }
static inline esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif) { (void)netif; return ESP_OK; }
static inline esp_err_t esp_netif_dhcpc_start(esp_netif_t *netif) { (void)netif; return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config)
{ (void)iface; (void)config; return ESP_OK; }
static inline esp_err_t esp_wifi_connect(void)
{ assert(!"clients must not associate with an AP"); return ESP_ERR_INVALID_STATE; }
