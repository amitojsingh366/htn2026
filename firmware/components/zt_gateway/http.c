#include "gateway_internal.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static void activity_begin(zt_gateway_activity_t activity)
{
    portENTER_CRITICAL(&zt_gw->guard);
    zt_gw->status.activity=activity;
    portEXIT_CRITICAL(&zt_gw->guard);
}

static void activity_end(zt_err_t result)
{
    uint64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&zt_gw->guard);
    zt_gw->status.activity=ZT_GATEWAY_ACTIVITY_IDLE;
    zt_gw->status.last_error=result;
    if (result==ZT_OK) zt_gw->status.last_http_success_us=now;
    else gw_record_failure_locked(zt_gw,result);
    portEXIT_CRITICAL(&zt_gw->guard);
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    gateway_t *g=event->user_data;
    if (event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        if ((size_t)event->data_len>ZT_GATEWAY_MESSAGE_MAX_BYTES-g->rx.message_len) {
            g->overflow_event=1; return ESP_FAIL;
        }
        memcpy(g->rx.bytes+g->rx.message_len,event->data,event->data_len);
        g->rx.message_len+=event->data_len;
        g->rx.bytes[g->rx.message_len]=0;
    } else if (event->event_id==HTTP_EVENT_ON_HEADER && event->header_key &&
               !strcasecmp(event->header_key,"Retry-After")) {
        char *end; long seconds=strtol(event->header_value,&end,10);
        if (!*end && seconds>=1 && seconds<=60) g->http_retry_us=esp_timer_get_time()+seconds*1000000ULL;
    }
    return ESP_OK;
}

static zt_err_t request(const char *path, const char *body, const char *key, bool control)
{
    gateway_t *g=zt_gw;
    if (g) g->http_retry_us=0;
    if (!g || g->ws || time(NULL)<1704067200) return ZT_ERR_INVALID_STATE;
    char url[ZT_HTTPS_URL_MAX_LEN+160], authorization[ZT_HOST_TOKEN_MAX_LEN+8];
    int n=snprintf(url,sizeof(url),"%s%s",g->origin,path);
    if (n<0 || n>=sizeof(url)) return ZT_ERR_INVALID_LENGTH;
    snprintf(authorization,sizeof(authorization),"Bearer %s",g->token);
    esp_http_client_config_t config={.url=url,.crt_bundle_attach=esp_crt_bundle_attach,
        .timeout_ms=ZT_GATEWAY_NETWORK_TIMEOUT_MS,.disable_auto_redirect=true,
        .buffer_size=512,.buffer_size_tx=768,.event_handler=http_event,.user_data=g};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if (!client) return ZT_ERR_NO_SPACE;
    esp_http_client_set_header(client,"Authorization",authorization);
    esp_http_client_set_header(client,"Accept","application/json");
    if (body) {
        esp_http_client_set_method(client,HTTP_METHOD_POST);
        esp_http_client_set_header(client,"Content-Type","application/json");
        esp_http_client_set_header(client,"Idempotency-Key",key);
        esp_http_client_set_post_field(client,body,strlen(body));
    }
    g->rx.message_len=0; g->overflow_event=0;
    esp_err_t result=esp_http_client_perform(client);
    int status=esp_http_client_get_status_code(client);
    portENTER_CRITICAL(&g->guard);
    g->status.http_status=status;
    portEXIT_CRITICAL(&g->guard);
    bool complete=esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    memset(authorization,0,sizeof(authorization));
    if (g->overflow_event) return ZT_ERR_INVALID_LENGTH;
    if (result!=ESP_OK || !complete) return ZT_ERR_NETWORK;
    if (status==401 || status==403) {
        portENTER_CRITICAL(&g->guard);
        g->status.auth_error=1;
        portEXIT_CRITICAL(&g->guard);
        g->stopped=1; return ZT_ERR_AUTH;
    }
    if (control && status==409) return ZT_ERR_CONFLICT;
    if (control && status==404) return ZT_ERR_NOT_FOUND;
    /* Registration payload rejection belongs to that badge. It must not shut
     * down the shared gateway or prevent other queued badges registering. */
    if (body && (status==400 || status==422)) return ZT_ERR_INVALID_ARG;
    if (status==404 || status==400 || status==422 || (status>=300 && status<400)) {
        g->stopped=1; return ZT_ERR_PROTOCOL;
    }
    if (status==429) return ZT_ERR_BUSY;
    if (status!=200 && status!=201 && status!=409) return ZT_ERR_NETWORK;
    return ZT_OK;
}

zt_err_t zt_gateway_bootstrap(zt_bootstrap_t *out)
{
    if (!zt_gw || !out) return ZT_ERR_INVALID_ARG;
    char game[ZT_ID_TEXT_BYTES], host[ZT_MAC_TEXT_BYTES], path[144];
    zt_id_format(zt_gw->game,game,sizeof(game)); zt_mac_format(&zt_gw->host,host,sizeof(host));
    snprintf(path,sizeof(path),"/api/v1/games/%s/gateway/bootstrap?host_id=%s",game,host);
    activity_begin(ZT_GATEWAY_ACTIVITY_BOOTSTRAP);
    uint64_t sent=esp_timer_get_time();
    zt_err_t result=request(path,NULL,NULL,false);
    if (result==ZT_OK) result=zt_gateway_decode_bootstrap(zt_gw->rx.bytes,zt_gw->rx.message_len,&zt_gw->scratch.tokens,out);
    if (result==ZT_OK && (out->game_id!=zt_gw->game || memcmp(&out->host_id,&zt_gw->host,sizeof(out->host_id)))) result=ZT_ERR_AUTH;
    /* Only an origin-relative absolute path is accepted. No authority, scheme,
     * query credentials, fragments, backslashes, or redirect target can escape. */
    if (result==ZT_OK) {
        const char *p=out->socket_path;
        char expected[112];
        snprintf(expected,sizeof(expected),"/api/v1/games/%s/gateway/socket",game);
        if (strcmp(p,expected)) result=ZT_ERR_PROTOCOL;
        else {
            snprintf(zt_gw->socket_path,sizeof(zt_gw->socket_path),"%s",p);
            gw_anchor(out->server_time_ms,sent,esp_timer_get_time());
        }
    }
    activity_end(result);
    return result;
}

zt_err_t zt_gateway_register(const zt_registration_request_t *registration, zt_registration_response_t *out)
{
    if (!zt_gw || !registration || !out) return ZT_ERR_INVALID_ARG;
    char body[384], key[ZT_ID_TEXT_BYTES], game[ZT_ID_TEXT_BYTES], path[112]; size_t written;
    activity_begin(ZT_GATEWAY_ACTIVITY_REGISTER);
    zt_err_t result=zt_gateway_encode_registration(registration,body,sizeof(body),&written);
    if (result==ZT_OK) {
        zt_id_format(registration->stable_request_id,key,sizeof(key)); zt_id_format(zt_gw->game,game,sizeof(game));
        snprintf(path,sizeof(path),"/api/v1/games/%s/registrations",game);
        result=request(path,body,key,false);
        if (result==ZT_OK) result=zt_gateway_decode_registration(zt_gw->rx.bytes,zt_gw->rx.message_len,&zt_gw->scratch.tokens,out);
    }
    activity_end(result);
    if (result==ZT_ERR_INVALID_ARG) {
        *out=(zt_registration_response_t){.status=ZT_JOIN_BAD_CONFIGURATION,.slot=ZT_SLOT_INVALID};
        return ZT_OK;
    }
    return result;
}

zt_err_t zt_gateway_control(const zt_gateway_control_request_t *control, zt_gateway_control_response_t *out)
{
    if (!zt_gw || !control || !out) return ZT_ERR_INVALID_ARG;
    char body[192], key[ZT_ID_TEXT_BYTES], game[ZT_ID_TEXT_BYTES], path[112]; size_t written;
    activity_begin(control->action==ZT_HOST_CONTROL_RESET ? ZT_GATEWAY_ACTIVITY_RESET : ZT_GATEWAY_ACTIVITY_START);
    zt_err_t result=zt_gateway_encode_control(control,body,sizeof(body),&written);
    if (result==ZT_OK) {
        zt_id_format(control->request_id,key,sizeof(key)); zt_id_format(zt_gw->game,game,sizeof(game));
        snprintf(path,sizeof(path),"/api/v1/games/%s/gateway/control",game);
        result=request(path,body,key,true);
        if (result==ZT_OK) result=zt_gateway_decode_control(zt_gw->rx.bytes,zt_gw->rx.message_len,&zt_gw->scratch.tokens,out);
        if (result==ZT_OK && (out->action!=control->action || out->request_id!=control->request_id)) result=ZT_ERR_PROTOCOL;
    }
    activity_end(result);
    return result;
}
