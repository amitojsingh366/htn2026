#include "gateway_internal.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static gateway_t state;
gateway_t *zt_gw = &state;
static unsigned polls, sends, heap_reads;
static int writable;
static zt_err_t send_result;
static const uint64_t interval = ZT_GATEWAY_DIAGNOSTICS_INTERVAL_MS * 1000ULL;

int esp_transport_poll_write(esp_transport_handle_t transport, int timeout_ms)
{
    assert(transport == state.ssl && timeout_ms == 0);
    ++polls;
    return writable;
}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{ assert(!task); return 3072; }
void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++heap_reads;
    *info = (multi_heap_info_t){32768, 24576, 16384};
}
zt_err_t zt_mesh_get_boot_nonce(zt_boot_nonce_t *out)
{ *out = UINT64_C(0x123456789abcdef0); return ZT_OK; }
zt_err_t gw_ws_send_with_timeout(const uint8_t *text, size_t len, uint32_t timeout_ms)
{
    ++sends;
    assert(text == (const uint8_t *)state.scratch.tx);
    assert(len == strlen((const char *)text) && len <= ZT_GATEWAY_DIAGNOSTICS_MAX_BYTES);
    assert(timeout_ms == 20);
    assert(strstr((const char *)text, "\"host_boot\":\"123456789abcdef0\""));
    assert(strstr((const char *)text, "\"gateway_stack_free_bytes\":3072"));
    assert(strstr((const char *)text, "\"heap_free_bytes\":32768"));
    assert(strstr((const char *)text, "\"reconnects\":2"));
    return send_result;
}
static void reset(void)
{
    memset(&state, 0, sizeof(state));
    state.ws = &state; state.ssl = &state;
    state.status.connected = state.status.welcomed = state.diagnostics_enabled = 1;
    state.diagnostics_due = interval;
    state.diagnostics_connections = 3;
    polls = sends = heap_reads = 0;
    writable = 1; send_result = ZT_OK;
}
static void skipped(void)
{
    gw_diagnostics_service(interval);
    assert(!polls && !sends && !heap_reads && !state.diagnostics_seq);
}
int main(void)
{
    reset(); state.diagnostics_enabled = 0; skipped();
    reset(); state.status.connected = 0; skipped();
    reset(); state.status.welcomed = 0; skipped();
    reset(); state.ws = NULL; skipped();
    reset(); state.ssl = NULL; skipped();
    reset(); state.response_pending = 1; state.response_due = interval + 1; skipped();
    reset(); state.send_due = interval + 1; skipped();
    reset(); state.command_count = 1; skipped();
    reset(); state.event_count = 1; skipped();
    reset(); state.requested_count = 1; skipped();
    reset(); state.resetting = 1; skipped();
    reset(); state.snapshot_pages = 2; state.snapshot_mask = 1; skipped();
    reset(); state.rx.ready = 1; skipped();
    reset(); state.rx.message_in_progress = 1; skipped();
    reset(); state.diagnostics_due = interval + 1; skipped();
    reset(); state.client_id = UINT32_MAX; skipped();

    reset();
    gw_diagnostics_service(interval);
    assert(sends == 1 && polls == 1 && heap_reads == 1);
    assert(state.diagnostics_seq == 1 && state.client_id == 1 && state.diagnostics_due == 2 * interval);
    assert(!state.response_pending && !state.send_due); /* No application reply/ACK budget. */
    gw_diagnostics_service(interval + 1);
    assert(sends == 1 && polls == 1 && heap_reads == 1);
    state.status.connected = 0;
    gw_diagnostics_service(20 * interval);
    assert(sends == 1);
    state.status.connected = 1;
    gw_diagnostics_service(20 * interval);
    assert(sends == 2 && state.diagnostics_due == 21 * interval); /* No catch-up burst. */

    reset(); state.response_pending = 1; state.response_due = interval - 1;
    gw_diagnostics_service(interval);
    assert(sends == 1 && state.response_pending == 1); /* Silent empty ACK cannot starve telemetry. */

    reset(); writable = 0;
    gw_diagnostics_service(interval);
    assert(polls == 1 && !sends && !heap_reads && state.diagnostics_due == 2 * interval);
    writable = 1;
    gw_diagnostics_service(interval + 1);
    assert(polls == 1 && !sends); /* Congestion is dropped, not retried. */

    reset(); send_result = ZT_ERR_NETWORK;
    gw_diagnostics_service(interval);
    gw_diagnostics_service(interval + 1);
    assert(sends == 1 && state.diagnostics_due == 2 * interval && !state.response_pending);
    gw_record_failure(ZT_ERR_NETWORK);
    assert(state.diagnostics_failures == 1 && state.diagnostics_last_error == ZT_ERR_NETWORK);
    gw_record_failure(ZT_OK);
    assert(state.diagnostics_failures == 1);
    state.diagnostics_failures = UINT32_MAX;
    gw_record_failure(ZT_ERR_TIMEOUT);
    assert(state.diagnostics_failures == UINT32_MAX && state.diagnostics_last_error == ZT_ERR_TIMEOUT);
    puts("Gateway diagnostic service: priority, pacing, no retries, no ACK budget, congestion and saturation passed");
    return 0;
}
