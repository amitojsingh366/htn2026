#include "gateway_internal.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <time.h>

void gw_record_failure(zt_err_t error)
{
    gateway_t *g = zt_gw;
    if (!g || error == ZT_OK) return;
    portENTER_CRITICAL(&g->guard);
    gw_record_failure_locked(g, error);
    portEXIT_CRITICAL(&g->guard);
}

void gw_diagnostics_service(uint64_t now)
{
    gateway_t *g = zt_gw;
    /* The caller services commands, joins, clocks, snapshots, receipts, and
     * infection evidence first. Never take a place in their request/ack budget.
     * A busy/offline host can skip any number of samples without queuing work. */
    if (!g || !g->diagnostics_enabled || !g->status.connected || !g->status.welcomed || !g->ws || !g->ssl ||
        now < g->diagnostics_due || now < g->send_due || (g->response_pending && now < g->response_due) || g->command_count ||
        g->event_count || g->requested_count || g->resetting ||
        (g->snapshot_pages && g->snapshot_mask != ((1u << g->snapshot_pages) - 1)) ||
        g->diagnostics_seq == UINT32_MAX || g->client_id == UINT32_MAX) return;
    portENTER_CRITICAL(&g->guard);
    bool receiving = g->rx.ready || g->rx.message_in_progress;
    portEXIT_CRITICAL(&g->guard);
    if (receiving) return;
    /* Pace attempts as well as successes; no catch-up bursts after reconnect.
     * A zero-time poll avoids adding a telemetry write to a congested socket. */
    g->diagnostics_due = now + ZT_GATEWAY_DIAGNOSTICS_INTERVAL_MS * 1000ULL;
    if (esp_transport_poll_write(g->ssl, 0) <= 0) return;

    /* Reuse existing scratch: no new allocation, task, persistent journal,
     * mesh message, connection, or Sentry credential lives on the badge. */
    zt_gateway_message_t *m = &g->message;
    memset(m, 0, sizeof(*m));
    m->envelope = (zt_gateway_envelope_t){ZT_GATEWAY_SCHEMA_VERSION, ZT_GATEWAY_DIAGNOSTICS,
        ++g->client_id, (uint64_t)time(NULL) * 1000};
    zt_gateway_diagnostics_t *d = &m->body.diagnostics;
    d->round_id = g->round;
    zt_mesh_get_boot_nonce(&d->host_boot);
    memcpy(d->fw, ZT_BUILD_ID, ZT_BUILD_ID_LEN);
    d->seq = ++g->diagnostics_seq;
    d->uptime_ms = now / 1000;
    portENTER_CRITICAL(&g->guard);
    d->reconnects = g->diagnostics_connections ? g->diagnostics_connections - 1 : 0;
    d->failures = g->diagnostics_failures;
    d->dropped_messages = g->rx.dropped_messages;
    d->send_failures = g->diagnostics_send_failures;
    d->last_error = g->diagnostics_last_error;
    d->websocket_stack_free_bytes = g->status.websocket_stack_free_min;
    portEXIT_CRITICAL(&g->guard);
    multi_heap_info_t heap;
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    d->heap_free_bytes = heap.total_free_bytes;
    d->heap_min_free_bytes = heap.minimum_free_bytes;
    d->heap_largest_free_bytes = heap.largest_free_block;
    d->gateway_stack_free_bytes = uxTaskGetStackHighWaterMark(NULL);
    size_t written = 0;
    zt_err_t result = zt_gateway_encode(m, g->scratch.tx, sizeof(g->scratch.tx), &written);
    if (result == ZT_OK)
        (void)gw_ws_send_with_timeout((const uint8_t *)g->scratch.tx, written, ZT_GATEWAY_DIAGNOSTICS_SEND_TIMEOUT_MS);
    else gw_record_failure(result);
    /* Backend accepts silently. No response_pending/send_due, receipt, or retry. */
}
