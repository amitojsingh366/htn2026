#include "sdk.h"
#include "zt_store.h"
#include <stdio.h>

extern zt_err_t zt_radio_channel_init(const zt_radio_config_t *config);
extern zt_err_t zt_radio_channel_service(uint64_t now_us);
extern zt_err_t zt_radio_channel_discovered(uint8_t channel, uint64_t rx_us);
extern uint32_t zt_radio_operating_channel(void);

const char test_wifi_event[] = "wifi", test_ip_event[] = "ip";
uint64_t test_now_us = UINT64_C(30000000);
uint8_t test_wifi_channel;
unsigned test_set_channel_calls, test_disconnect_calls, test_scan_calls;
bool test_fail_set_channel;
static unsigned invalidations;
uint32_t zt_radio_generation(void) { return 1; }
zt_err_t zt_radio_driver_restart(uint8_t channel)
{ (void)channel; assert(!"discovery must not restart the driver"); return ZT_ERR_RADIO; }
void zt_radio_mesh_invalidate(uint32_t generation)
{ assert(generation == 1); ++invalidations; }
zt_err_t zt_store_load_config(zt_config_t *config)
{ (void)config; assert(!"clients must not load host credentials"); return ZT_ERR_INVALID_STATE; }

static void expect(zt_channel_state_t state, uint8_t channel)
{
    zt_channel_status_t actual;
    assert(zt_channel_get_status(&actual) == ZT_OK);
    assert(actual.state == state);
    assert(actual.channel == channel);
    assert(zt_radio_operating_channel() == channel);
    assert(test_wifi_channel == channel);
    assert(!actual.associated && !actual.has_ip);
    assert(!test_scan_calls && !test_disconnect_calls);
}

static void tick(uint64_t now)
{
    test_now_us = now;
    assert(zt_radio_channel_service(now) == ZT_OK);
}

static void discovered(uint8_t channel, uint64_t received)
{ assert(zt_radio_channel_discovered(channel, received) == ZT_OK); }

static void home_event(uint8_t channel)
{
    zt_sta_event_t event = {.kind = ZT_STA_HOME_CHANNEL_CHANGED,
        .channel = channel, .driver_generation = zt_radio_generation()};
    assert(zt_channel_post_sta_event(&event) == ZT_OK);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const uint64_t start = test_now_us;
    const uint64_t lease = ZT_GATEWAY_DISCOVERY_MAX_AGE_S * UINT64_C(1000000);
    const uint64_t freshness = ZT_CLOCK_AGE_MAX_MS * UINT64_C(1000);
    zt_radio_config_t config = {.last_channel = 5};
    assert(zt_radio_channel_init(&config) == ZT_OK);
    expect(ZT_CHANNEL_DISCOVERY, 5);

    if (!strcmp(argv[1], "retune")) {
        discovered(6, start - 10);
        tick(start);
        expect(ZT_CHANNEL_LOBBY, 6);
        assert(test_set_channel_calls == 2 && invalidations == 1);
        tick(start + ZT_DISCOVERY_LAST_CHANNEL_MS * UINT64_C(1000));
        expect(ZT_CHANNEL_LOBBY, 6);
    } else if (!strcmp(argv[1], "queued-home-event")) {
        /* SDK callbacks from a previous sweep may still be in the queue. */
        home_event(5);
        discovered(6, start);
        tick(start);
        expect(ZT_CHANNEL_LOBBY, 6);
        home_event(5);
        tick(start + 1);
        expect(ZT_CHANNEL_LOBBY, 6);
    } else if (!strcmp(argv[1], "retune-failure")) {
        test_fail_set_channel = true;
        discovered(6, start);
        assert(zt_radio_channel_service(start) == ZT_ERR_RADIO);
        expect(ZT_CHANNEL_DISCOVERY, 5);
        assert(invalidations == 0);
        test_fail_set_channel = false;
        discovered(6, start + 1);
        tick(start + 1);
        expect(ZT_CHANNEL_LOBBY, 6);
    } else if (!strcmp(argv[1], "stale")) {
        discovered(6, start - freshness - 1);
        tick(start);
        expect(ZT_CHANNEL_DISCOVERY, 5);
        assert(test_set_channel_calls == 1);
    } else if (!strcmp(argv[1], "future")) {
        discovered(6, start + 1);
        tick(start);
        expect(ZT_CHANNEL_DISCOVERY, 5);
        assert(test_set_channel_calls == 1);
        /* An invalid future timestamp must not prevent later valid contact. */
        discovered(6, start);
        tick(start + 1);
        expect(ZT_CHANNEL_LOBBY, 6);
    } else if (!strcmp(argv[1], "locked")) {
        assert(zt_channel_lock(UINT64_C(42), 5) == ZT_OK);
        tick(start);
        expect(ZT_CHANNEL_LOCKED, 5);
        unsigned calls = test_set_channel_calls;
        discovered(6, start + 1);
        home_event(6);
        tick(start + 1);
        expect(ZT_CHANNEL_LOCKED, 5);
        assert(test_set_channel_calls == calls);
    } else if (!strcmp(argv[1], "lease-expiry")) {
        discovered(5, start);
        tick(start);
        expect(ZT_CHANNEL_LOBBY, 5);
        tick(start + lease - 1);
        expect(ZT_CHANNEL_LOBBY, 5);
        tick(start + lease);
        expect(ZT_CHANNEL_DISCOVERY, 1);
        /* A delayed copy of the old contact cannot restore its expired lease. */
        discovered(5, start);
        tick(start + lease + 1);
        expect(ZT_CHANNEL_DISCOVERY, 1);
        discovered(6, start + lease + 2);
        tick(start + lease + 2);
        expect(ZT_CHANNEL_LOBBY, 6);
    } else if (!strcmp(argv[1], "lease-refresh")) {
        discovered(5, start);
        tick(start);
        discovered(5, start + lease - 1);
        tick(start + lease - 1);
        tick(start + lease);
        expect(ZT_CHANNEL_LOBBY, 5);
        assert(test_set_channel_calls == 1 && invalidations == 0);
        tick(start + 2 * lease - 1);
        expect(ZT_CHANNEL_DISCOVERY, 1);
    } else if (!strcmp(argv[1], "newest-observation")) {
        discovered(6, start);
        discovered(5, start - 1);
        tick(start);
        expect(ZT_CHANNEL_LOBBY, 6);
    } else {
        assert(!"unknown test case");
    }
    printf("channel discovery: %s passed\n", argv[1]);
    return 0;
}
