#include "gateway_internal.h"
#include "esp_crt_bundle.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_transport_internal.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

void zt_gateway_ws_discard(zt_ws_reassembly_t *a)
{
    if (!a) return;
    uint32_t dropped=a->dropped_messages;
    memset(a,0,sizeof(*a)); a->dropped_messages=dropped;
}
static zt_err_t reject_chunk(zt_ws_reassembly_t *a, zt_err_t error)
{
    gw_counter_increment(&a->dropped_messages);
    if (!a->ready) zt_gateway_ws_discard(a);
    return error;
}
zt_err_t zt_gateway_ws_copy_chunk(zt_ws_reassembly_t *a, const zt_ws_chunk_t *c)
{
    if (!a || !c) return ZT_ERR_INVALID_ARG;
    if (c->op_code==ZT_WS_CLOSE || c->op_code==ZT_WS_PING || c->op_code==ZT_WS_PONG) return ZT_OK;
    if (a->ready) return reject_chunk(a,ZT_ERR_BUSY);
    if (c->payload_len<0 || c->payload_offset<0 || c->data_len<0 ||
        (c->data_len && !c->data_ptr) || c->payload_offset>c->payload_len ||
        c->data_len>c->payload_len-c->payload_offset ||
        (c->op_code!=ZT_WS_TEXT && c->op_code!=ZT_WS_CONTINUATION)) return reject_chunk(a,ZT_ERR_PROTOCOL);
    if (!a->frame_in_progress) {
        if (c->payload_offset || (c->op_code==ZT_WS_TEXT && a->message_in_progress) ||
            (c->op_code==ZT_WS_CONTINUATION && !a->message_in_progress)) return reject_chunk(a,ZT_ERR_PROTOCOL);
        if ((uint32_t)c->payload_len>ZT_GATEWAY_MESSAGE_MAX_BYTES-a->message_len) return reject_chunk(a,ZT_ERR_INVALID_LENGTH);
        a->frame_payload_len=c->payload_len; a->frame_copied_len=0;
        a->frame_in_progress=1; a->frame_fin=c->fin; a->frame_op_code=c->op_code; a->message_in_progress=1;
    } else if (a->frame_payload_len!=(uint32_t)c->payload_len || a->frame_copied_len!=(uint32_t)c->payload_offset ||
               a->frame_fin!=c->fin || a->frame_op_code!=c->op_code) return reject_chunk(a,ZT_ERR_PROTOCOL);
    if (c->data_len) memcpy(a->bytes+a->message_len,c->data_ptr,c->data_len);
    a->message_len+=c->data_len; a->frame_copied_len+=c->data_len;
    if (a->frame_copied_len==a->frame_payload_len) {
        a->frame_in_progress=0;
        if (a->frame_fin) { a->bytes[a->message_len]=0; a->ready=1; a->message_in_progress=0; }
    }
    return ZT_OK;
}

/* IDF 5.5's websocket transport neither verifies the selected subprotocol nor
 * exposes response headers, and client 1.8 follows redirects automatically.
 * This transparent TLS parent rejects the upgrade before websocket sees it
 * unless it is 101 + exactly zt.v1. It also prevents credentialed redirects. */
static int guarded_connect(esp_transport_handle_t t,const char *host,int port,int timeout)
{
    gateway_t *g=esp_transport_get_context_data(t);
    g->header_done=0; g->header_first=1; g->header_protocol=0; g->header_len=0; g->header_overflow=0;
    return esp_transport_connect(g->ssl,host,port,timeout);
}
static bool header_byte(gateway_t *g,char c)
{
    if (c!='\n') {
        if (g->header_len<sizeof(g->header_line)-1) g->header_line[g->header_len++]=c;
        else g->header_overflow=1;
        return true;
    }
    if (g->header_len && g->header_line[g->header_len-1]=='\r') --g->header_len;
    g->header_line[g->header_len]=0;
    if (g->header_first) {
        g->header_first=0;
        /* Avoid sscanf's ~1 KiB newlib parser frame on the TLS task stack. */
        int status=0;
        bool valid=!g->header_overflow && g->header_len>=12 &&
            !memcmp(g->header_line,"HTTP/1.1 ",9) &&
            (!g->header_line[12] || g->header_line[12]==' ');
        for (unsigned i=9;valid && i<12;++i) {
            if (g->header_line[i]<'0' || g->header_line[i]>'9') valid=false;
            else status=status*10+g->header_line[i]-'0';
        }
        if (!valid || status!=101) {
            g->status.http_status=status;
            if (status==401 || status==403) { g->status.auth_error=1; g->stopped=1; }
            return false;
        }
    } else if (!g->header_len && !g->header_overflow) {
        if (!g->header_protocol) { g->status.last_error=ZT_ERR_PROTOCOL; return false; }
        g->header_done=1;
    } else if (!strncasecmp(g->header_line,"Sec-WebSocket-Protocol:",23)) {
        const char *value=g->header_line+23;
        while (*value==' ' || *value=='\t') ++value;
        if (g->header_overflow || strcmp(value,ZT_GATEWAY_SUBPROTOCOL) || g->header_protocol) return false;
        g->header_protocol=1;
    }
    g->header_len=0; g->header_overflow=0;
    return true;
}
static int guarded_read(esp_transport_handle_t t,char *buffer,int len,int timeout)
{
    gateway_t *g=esp_transport_get_context_data(t);
    int n=esp_transport_read(g->ssl,buffer,len,timeout);
    for (int i=0;i<n && !g->header_done;++i) if (!header_byte(g,buffer[i])) return -1;
    return n;
}
static int guarded_write(esp_transport_handle_t t,const char *b,int n,int timeout)
{ return esp_transport_write(((gateway_t *)esp_transport_get_context_data(t))->ssl,b,n,timeout); }
static int guarded_close(esp_transport_handle_t t)
{ return esp_transport_close(((gateway_t *)esp_transport_get_context_data(t))->ssl); }
static int guarded_poll_read(esp_transport_handle_t t,int timeout)
{ return esp_transport_poll_read(((gateway_t *)esp_transport_get_context_data(t))->ssl,timeout); }
static int guarded_poll_write(esp_transport_handle_t t,int timeout)
{ return esp_transport_poll_write(((gateway_t *)esp_transport_get_context_data(t))->ssl,timeout); }
static int guarded_destroy(esp_transport_handle_t t) { (void)t; return 0; }
static int guarded_get_socket(esp_transport_handle_t t)
{ return esp_transport_get_socket(((gateway_t *)esp_transport_get_context_data(t))->ssl); }

static void websocket_event(void *context,esp_event_base_t base,int32_t event,void *data)
{
    (void)base; gateway_t *g=context; esp_websocket_event_data_t *e=data;
    uint32_t stack_free=uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&g->guard);
    if (!g->status.websocket_stack_free_min || stack_free<g->status.websocket_stack_free_min)
        g->status.websocket_stack_free_min=stack_free;
    if (event==WEBSOCKET_EVENT_CONNECTED) {
        g->status.connected=1; g->connected_event=1; g->status.last_inbound_us=esp_timer_get_time();
        gw_counter_increment(&g->diagnostics_connections);
    }
    else if (event==WEBSOCKET_EVENT_DISCONNECTED || event==WEBSOCKET_EVENT_CLOSED) {
        g->status.connected=0; g->status.welcomed=0; g->disconnected_event=1;
        if (event==WEBSOCKET_EVENT_CLOSED && e && e->close_status_code) g->close_code=e->close_status_code;
        if (!g->rx.ready) { if (g->rx.message_in_progress) gw_counter_increment(&g->rx.dropped_messages); zt_gateway_ws_discard(&g->rx); }
    } else if (event==WEBSOCKET_EVENT_DATA && e) {
        g->status.last_inbound_us=esp_timer_get_time();
        zt_ws_chunk_t chunk={e->payload_len,e->payload_offset,e->data_len,e->fin,e->op_code,(const uint8_t *)e->data_ptr};
        /* DATA is dispatched before the SDK fills close_status_code. Read
         * the two network-order bytes, allowing separate transport chunks. */
        if (e->op_code==ZT_WS_CLOSE && e->payload_len>=2 && e->data_ptr && e->data_len>0) {
            if (!e->payload_offset) g->close_code=(uint16_t)(uint8_t)e->data_ptr[0]<<8;
            if (e->payload_offset<=1 && e->payload_offset+e->data_len>1)
                g->close_code|=(uint8_t)e->data_ptr[1-e->payload_offset];
        }
        bool control=e->op_code==ZT_WS_CLOSE || e->op_code==ZT_WS_PING || e->op_code==ZT_WS_PONG;
        if (!control && (g->rx.ready || g->rx_dropping_message)) {
            /* A full mailbox is backpressure, not a broken TLS link. Drop the
             * entire extra message, including continuations, and let the
             * gateway's snapshot/command requests recover it. */
            if (!g->rx_dropping_message) gw_counter_increment(&g->rx.dropped_messages);
            g->rx_dropping_message=!(e->fin && e->payload_offset+e->data_len==e->payload_len);
        } else if (zt_gateway_ws_copy_chunk(&g->rx,&chunk)!=ZT_OK) g->overflow_event=1;
    } else if (event==WEBSOCKET_EVENT_ERROR && e) {
        int status=e->error_handle.esp_ws_handshake_status_code;
        if (status>0) g->status.http_status=status;
        if (status==401 || status==403) { g->status.auth_error=1; g->stopped=1; }
        g->status.last_error=g->status.auth_error ? ZT_ERR_AUTH : ZT_ERR_NETWORK;
        gw_record_failure_locked(g,g->status.last_error);
        g->disconnected_event=1;
    }
    portEXIT_CRITICAL(&g->guard);
    if (g->owner) xTaskNotifyGive(g->owner);
}
void gw_ws_release_rx(void)
{
    portENTER_CRITICAL(&zt_gw->guard); zt_gateway_ws_discard(&zt_gw->rx); portEXIT_CRITICAL(&zt_gw->guard);
    zt_gw->inbound_index=0;
}
void gw_ws_destroy(void)
{
    gateway_t *g=zt_gw;
    if (g->ws) { esp_websocket_client_stop(g->ws); esp_websocket_client_destroy(g->ws); g->ws=NULL; }
    if (g->ws_transport) { esp_transport_destroy(g->ws_transport); g->ws_transport=NULL; }
    if (g->guard_transport) { esp_transport_destroy(g->guard_transport); g->guard_transport=NULL; }
    if (g->ssl) { esp_transport_destroy(g->ssl); g->ssl=NULL; }
    portENTER_CRITICAL(&g->guard);
    g->status.connected=0; g->status.welcomed=0; g->diagnostics_enabled=0;
    portEXIT_CRITICAL(&g->guard);
    g->hello_sent=0; g->response_pending=0; g->response_due=0; g->send_due=0;
    g->connected_event=0; g->disconnected_event=0; g->overflow_event=0;
    g->rx_dropping_message=0;
    gw_ws_release_rx();
}
zt_err_t zt_gateway_ws_open(const zt_gateway_hello_t *hello)
{
    gateway_t *g=zt_gw;
    if (!g || !hello || g->ws || !g->socket_path[0]) return ZT_ERR_INVALID_STATE;
    gw_ws_release_rx();
    char uri[ZT_HTTPS_URL_MAX_LEN*2+8], headers[ZT_HOST_TOKEN_MAX_LEN+40];
    snprintf(uri,sizeof(uri),"wss://%s%s",g->origin+8,g->socket_path);
    snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\n",g->token);
    /* SDK transport logs the complete authorization header on failed writes. */
    esp_log_level_set("transport_ws",ESP_LOG_NONE);
    g->ssl=esp_transport_ssl_init(); g->guard_transport=esp_transport_init();
    if (!g->ssl || !g->guard_transport) goto failed;
    esp_transport_ssl_crt_bundle_attach(g->ssl,esp_crt_bundle_attach);
    esp_transport_set_context_data(g->guard_transport,g);
    esp_transport_set_func(g->guard_transport,guarded_connect,guarded_read,guarded_write,guarded_close,
        guarded_poll_read,guarded_poll_write,guarded_destroy);
    /* Borrowed from SSL; only SSL destroys its foundation. Without the socket
     * hook the SDK close path waits on an invalid descriptor instead of TLS. */
    g->guard_transport->_get_socket=guarded_get_socket;
    g->guard_transport->foundation=g->ssl->foundation;
    g->ws_transport=esp_transport_ws_init(g->guard_transport);
    if (!g->ws_transport) goto failed;
    esp_transport_set_default_port(g->ws_transport,443);
    const esp_transport_ws_config_t transport_config={.ws_path=g->socket_path,
        .sub_protocol=ZT_GATEWAY_SUBPROTOCOL,.headers=headers,.propagate_control_frames=true};
    /* ext_transport bypasses the client's transport configuration. Forward
     * PONG explicitly, or the client times out every healthy ping exchange. */
    if (esp_transport_ws_set_config(g->ws_transport,&transport_config)!=ESP_OK) goto failed;
    esp_websocket_client_config_t config={.uri=uri,.disable_auto_reconnect=true,.enable_close_reconnect=false,
        .ext_transport=g->ws_transport,.subprotocol=ZT_GATEWAY_SUBPROTOCOL,.crt_bundle_attach=esp_crt_bundle_attach,
        .task_prio=3,.task_stack=4096,.buffer_size=512,.network_timeout_ms=ZT_GATEWAY_NETWORK_TIMEOUT_MS,
        .ping_interval_sec=10,.pingpong_timeout_sec=20};
    g->ws=esp_websocket_client_init(&config);
    memset(headers,0,sizeof(headers));
    if (!g->ws || esp_websocket_register_events(g->ws,WEBSOCKET_EVENT_ANY,websocket_event,g)!=ESP_OK ||
        esp_websocket_client_start(g->ws)!=ESP_OK) goto failed;
    g->client_id=0; g->hello_sent=0; g->sync_us=0; g->sync_due=0; g->hello_us=esp_timer_get_time();
    return ZT_OK;
failed:
    memset(headers,0,sizeof(headers)); gw_ws_destroy(); return ZT_ERR_NO_SPACE;
}
zt_err_t zt_gateway_ws_send(const uint8_t *text,size_t len)
{
    return gw_ws_send_with_timeout(text,len,ZT_GATEWAY_SEND_TIMEOUT_MS);
}
zt_err_t gw_ws_send_with_timeout(const uint8_t *text,size_t len,uint32_t timeout_ms)
{
    if (!zt_gw || !zt_gw->ws || !zt_gw->status.connected) return ZT_ERR_INVALID_STATE;
    if (!text || !len || len>ZT_GATEWAY_MESSAGE_MAX_BYTES) return ZT_ERR_INVALID_LENGTH;
    if (esp_websocket_client_send_text(zt_gw->ws,(const char *)text,len,pdMS_TO_TICKS(timeout_ms))==(int)len) return ZT_OK;
    portENTER_CRITICAL(&zt_gw->guard);
    gw_counter_increment(&zt_gw->diagnostics_send_failures);
    gw_record_failure_locked(zt_gw,ZT_ERR_NETWORK);
    portEXIT_CRITICAL(&zt_gw->guard);
    return ZT_ERR_NETWORK;
}
zt_err_t zt_gateway_ws_close(uint16_t code)
{
    if (!zt_gw) return ZT_ERR_INVALID_STATE;
    (void)code; /* stop is bounded by the configured transport timeout */
    gw_ws_destroy(); return ZT_OK;
}
