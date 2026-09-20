#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "zt_game.h"

/* Run real mesh.c and wire.c with only platform/driver boundaries stubbed.
 * Frames use deterministic test identities and a public, synthetic group key. */
extern bool zt_radio_flood_type(uint8_t type);
extern zt_err_t zt_radio_payload_encode(uint8_t type, const zt_wire_payload_t *payload,
    uint8_t *out, size_t capacity, size_t *written);
extern void zt_radio_mesh_diagnostics(zt_radio_diagnostics_t *out);

static const zt_radio_config_t config = {
    .game_id = 123,
    .self_mac = {{2, 0, 0, 0, 0, 2}},
    .host_mac = {{2, 0, 0, 0, 0, 1}},
    .group_key = {1, 2, 3, 4},
    .last_channel = 5,
};
static const zt_mac_t other_mac = {{2, 0, 0, 0, 0, 3}};
static uint64_t test_now = 10000000;
static uint32_t next_seq;
static zt_channel_state_t channel_state = ZT_CHANNEL_DISCOVERY;
static unsigned discovery_calls, deliveries;
static uint8_t discovered_channel;
static uint64_t discovered_rx_us;
static zt_domain_message_t last_delivery;

int64_t esp_timer_get_time(void) { return (int64_t)test_now; }
uint32_t esp_random(void) { return 0; }
const zt_radio_config_t *zt_radio_configuration(void) { return &config; }
zt_boot_nonce_t zt_radio_boot_nonce(void) { return 42; }
uint32_t zt_radio_generation(void) { return 1; }
zt_err_t zt_radio_submit(const zt_tx_frame_t *frame) { (void)frame; return ZT_OK; }
zt_err_t zt_clock_read(uint64_t at_us, zt_clock_sample_t *out)
{
    (void)at_us; (void)out;
    return ZT_ERR_INVALID_STATE;
}
zt_err_t zt_channel_get_status(zt_channel_status_t *out)
{
    *out = (zt_channel_status_t){.state = channel_state, .channel = 5};
    return ZT_OK;
}
zt_err_t zt_radio_channel_discovered(uint8_t channel, uint64_t rx_us)
{
    ++discovery_calls;
    discovered_channel = channel;
    discovered_rx_us = rx_us;
    return ZT_OK;
}
static zt_err_t receive_domain(const zt_domain_message_t *message, void *context)
{
    (void)context;
    ++deliveries;
    last_delivery = *message;
    return ZT_OK;
}

static zt_wire_payload_t beacon(zt_wire_phase_t phase)
{
    zt_wire_payload_t payload = {0};
    payload.beacon = (zt_wire_beacon_t){.slot = 0, .role = ZT_ROLE_HUMAN,
        .phase = phase, .round_channel = 6, .gateway_age_s = 0};
    return payload;
}

static zt_wire_header_t header(uint8_t type, zt_round_id_t round)
{
    zt_wire_header_t h = {.magic = ZT_WIRE_MAGIC, .protocol_version = ZT_PROTOCOL_VERSION,
        .type = type, .game_id = config.game_id, .round_id = round,
        .origin = config.host_mac, .origin_boot_nonce = 99, .packet_seq = ++next_seq};
    if (zt_radio_flood_type(type)) {
        h.flags = ZT_WIRE_FLAG_RELAY_CAPABLE;
        h.ttl_remaining = ZT_FLOOD_TTL;
    }
    return h;
}

static zt_rx_frame_t frame(zt_wire_header_t h, zt_wire_payload_t payload)
{
    uint8_t body[ZT_MAX_PAYLOAD];
    size_t body_len, total;
    assert(zt_radio_payload_encode(h.type, &payload, body, sizeof(body), &body_len) == ZT_OK);
    h.payload_len = body_len;
    zt_rx_frame_t result = {.src_mac = h.origin, .rssi = -40, .channel = 5,
        .rx_us = test_now, .driver_generation = 1};
    assert(zt_wire_encode_envelope(&h, body, body_len, config.group_key,
        result.data, sizeof(result.data), &total) == ZT_OK);
    result.len = total;
    return result;
}

static void expect_discovery(zt_rx_frame_t *f)
{
    unsigned before = discovery_calls, delivered_before = deliveries;
    assert(zt_mesh_receive(f) == ZT_OK);
    assert(discovery_calls == before + 1);
    assert(discovered_channel == 6);
    assert(discovered_rx_us == f->rx_us);
    assert(deliveries == delivered_before + 1);
    /* Preserve RX metadata for domain diagnostics; only discovery uses the
     * advertised channel. */
    assert(last_delivery.channel == 5);
}

static void expect_no_discovery(zt_rx_frame_t *f, zt_err_t expected)
{
    unsigned before = discovery_calls, delivered_before = deliveries;
    assert(zt_mesh_receive(f) == expected);
    assert(discovery_calls == before);
    assert(deliveries == delivered_before + (expected == ZT_OK ? 1u : 0u));
}

static void advertised_channel_wins(void)
{
    zt_rx_frame_t f = frame(header(ZT_PKT_BEACON, 0), beacon(ZT_PHASE_LOBBY));
    expect_discovery(&f);

    zt_wire_payload_t payload = {0};
    payload.host_state = (zt_wire_host_state_t){.phase = ZT_PHASE_PREPARED,
        .round_channel = 6, .duration_ms = ZT_ROUND_DURATION_MS, .gateway_serial = 1};
    f = frame(header(ZT_PKT_HOST_STATE, 10), payload);
    expect_discovery(&f);
    puts("mesh: authenticated beacon and HOST_STATE use advertised channel 6 over RX channel 5");
}

static void unrelated_host_traffic_does_not_renew(void)
{
    zt_wire_payload_t payload = {0};
    payload.join_result = (zt_wire_join_result_t){.target_mac = config.self_mac,
        .request_nonce = 1, .status = ZT_JOIN_REGISTERED, .slot = 1};
    zt_rx_frame_t f = frame(header(ZT_PKT_JOIN_RESULT, 0), payload);
    expect_no_discovery(&f, ZT_OK);
    puts("mesh: JOIN_RESULT cannot renew discovery from RX metadata");
}

static void stale_and_future_advertisements(void)
{
    zt_wire_header_t h = header(ZT_PKT_BEACON, 0);
    h.age_ms = ZT_CLOCK_AGE_MAX_MS + 1;
    zt_rx_frame_t f = frame(h, beacon(ZT_PHASE_LOBBY));
    expect_no_discovery(&f, ZT_OK);

    f = frame(header(ZT_PKT_BEACON, 0), beacon(ZT_PHASE_LOBBY));
    f.rx_us -= (ZT_CLOCK_AGE_MAX_MS + 1) * 1000ULL;
    expect_no_discovery(&f, ZT_OK);

    /* Header and queue residence together exceed the lease freshness bound. */
    h = header(ZT_PKT_BEACON, 0);
    h.age_ms = ZT_CLOCK_AGE_MAX_MS;
    f = frame(h, beacon(ZT_PHASE_LOBBY));
    f.rx_us -= 1000;
    expect_no_discovery(&f, ZT_OK);

    f = frame(header(ZT_PKT_BEACON, 0), beacon(ZT_PHASE_LOBBY));
    ++f.rx_us;
    expect_no_discovery(&f, ZT_ERR_STALE);
    puts("mesh: stale, queued, and future advertisements do not establish a lease");
}

static void direct_host_recovers_missing_roster(void)
{
    zt_rx_frame_t f = frame(header(ZT_PKT_BEACON, 20), beacon(ZT_PHASE_PREPARED));
    expect_discovery(&f);

    /* One of two pages leaves round 20 present but its roster incomplete. */
    zt_wire_payload_t page = {0};
    page.roster_page = (zt_wire_roster_page_t){.snapshot_rev = 1, .roster_hash = 1,
        .page_count = 2, .entry_count = 1};
    page.roster_page.entries[0] = (zt_wire_roster_entry_t){.slot = 0,
        .mac = config.host_mac, .name_len = 4, .name = {'H', 'o', 's', 't'}};
    f = frame(header(ZT_PKT_ROSTER_PAGE, 20), page);
    expect_no_discovery(&f, ZT_OK);
    f = frame(header(ZT_PKT_BEACON, 20), beacon(ZT_PHASE_RUNNING));
    expect_discovery(&f);
    puts("mesh: direct host round beacon recovers discovery with absent or incomplete roster");
}

static void discovery_admission_remains_restricted(void)
{
    zt_wire_header_t h = header(ZT_PKT_BEACON, 20);
    h.origin = other_mac;
    zt_rx_frame_t f = frame(h, beacon(ZT_PHASE_PREPARED));
    expect_no_discovery(&f, ZT_ERR_STALE);

    f = frame(header(ZT_PKT_BEACON, 20), beacon(ZT_PHASE_PREPARED));
    f.src_mac = other_mac;
    expect_no_discovery(&f, ZT_ERR_AUTH);

    h = header(ZT_PKT_BEACON, 20);
    ++h.game_id;
    f = frame(h, beacon(ZT_PHASE_PREPARED));
    expect_no_discovery(&f, ZT_ERR_AUTH);

    channel_state = ZT_CHANNEL_LOCKED;
    f = frame(header(ZT_PKT_BEACON, 20), beacon(ZT_PHASE_PREPARED));
    expect_no_discovery(&f, ZT_ERR_STALE);
    channel_state = ZT_CHANNEL_DISCOVERY;
    puts("mesh: nonhost, source/game mismatches, and locked foreign rounds remain rejected");
}

static void invalid_hmac_cannot_discover(void)
{
    zt_radio_diagnostics_t before = {0}, after = {0};
    zt_radio_mesh_diagnostics(&before);
    zt_rx_frame_t f = frame(header(ZT_PKT_BEACON, 20), beacon(ZT_PHASE_PREPARED));
    f.data[f.len - 1] ^= 1;
    expect_no_discovery(&f, ZT_ERR_AUTH);
    zt_radio_mesh_diagnostics(&after);
    assert(after.auth_failures == before.auth_failures + 1);
    puts("mesh: failed HMAC cannot deliver a beacon or change discovery");
}

static void lobby_peer_advertisements(void)
{
    zt_wire_header_t h = header(ZT_PKT_BEACON, 0);
    h.origin = other_mac;
    zt_rx_frame_t f = frame(h, beacon(ZT_PHASE_LOBBY));
    expect_discovery(&f);

    zt_wire_payload_t payload = beacon(ZT_PHASE_LOBBY);
    payload.beacon.gateway_age_s = ZT_GATEWAY_DISCOVERY_MAX_AGE_S;
    h = header(ZT_PKT_BEACON, 0);
    h.origin = other_mac;
    f = frame(h, payload);
    expect_no_discovery(&f, ZT_OK);
    puts("mesh: lobby peers need fresh gateway evidence to advertise discovery");
}

int main(void)
{
    assert(zt_mesh_init(receive_domain, NULL) == ZT_OK);
    advertised_channel_wins();
    unrelated_host_traffic_does_not_renew();
    stale_and_future_advertisements();
    direct_host_recovers_missing_roster();
    discovery_admission_remains_restricted();
    invalid_hmac_cannot_discover();
    lobby_peer_advertisements();
    puts("mesh discovery tests passed");
    return 0;
}
