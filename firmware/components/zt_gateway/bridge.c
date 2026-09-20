#include "gateway_internal.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

gateway_t *zt_gw;
static uint64_t next_empty_ack_us, next_clock_apply_us;

zt_err_t zt_gateway_init(const zt_config_t *config,const zt_gateway_sinks_t *sinks)
{
    if (zt_gw) return ZT_ERR_INVALID_STATE;
    if (!config || !sinks || !config->host_credentials_present ||
        memcmp(&config->expected_mac,&config->host_mac,sizeof(zt_mac_t)) ||
        strncmp(config->https_base,"https://",8) || !config->token[0]) return ZT_ERR_AUTH;
    size_t n=strnlen(config->https_base,sizeof(config->https_base));
    size_t token_len=strnlen(config->token,sizeof(config->token));
    if (n>=sizeof(config->https_base) || token_len>=sizeof(config->token) ||
        strchr(config->token,'\r') || strchr(config->token,'\n')) return ZT_ERR_INVALID_ARG;
    while (n>8 && config->https_base[n-1]=='/') --n;
    if (n<=8) return ZT_ERR_INVALID_ARG;
    for (size_t i=8;i<n;++i) if (config->https_base[i]=='/' || config->https_base[i]=='?' ||
        config->https_base[i]=='#' || config->https_base[i]=='@' || (unsigned char)config->https_base[i]<=32) return ZT_ERR_INVALID_ARG;
    gateway_t *g=calloc(1,sizeof(*g));
    if (!g) return ZT_ERR_NO_SPACE;
    g->guard=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    g->game=config->game_id; g->host=config->host_mac; g->sinks=*sinks;
    memcpy(g->origin,config->https_base,n); memcpy(g->token,config->token,token_len+1);
    zt_gw=g;
    return ZT_OK;
}
zt_err_t zt_gateway_status(zt_gateway_status_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    if (!zt_gw) { memset(out,0,sizeof(*out)); return ZT_ERR_INVALID_STATE; }
    portENTER_CRITICAL(&zt_gw->guard);
    *out=zt_gw->status; out->dropped_messages=zt_gw->rx.dropped_messages;
    portEXIT_CRITICAL(&zt_gw->guard);
    if (zt_gw->owner) out->gateway_stack_free_min=uxTaskGetStackHighWaterMark(zt_gw->owner);
    return ZT_OK;
}
void gw_backoff(uint64_t now)
{
    static const uint32_t delays[]=ZT_GATEWAY_BACKOFF_MS;
    gateway_t *g=zt_gw; uint32_t delay=delays[g->backoff];
    if (g->backoff+1<ZT_GATEWAY_BACKOFF_COUNT) ++g->backoff;
    uint64_t due=now+(delay-delay/5+esp_random()%(2*(delay/5)+1))*1000ULL;
    if (due>g->retry_us) g->retry_us=due;
    ++g->status.reconnect_count;
}
void gw_anchor(uint64_t server_ms,uint64_t sent_us,uint64_t received_us)
{
    if (!server_ms || received_us<sent_us || received_us-sent_us>2000000ULL) return;
    gateway_t *g=zt_gw; uint64_t half=(received_us-sent_us+1999)/2000;
    g->anchor_ms=server_ms+half; g->anchor_us=received_us;
    g->anchor_uncertainty_ms=(uint32_t)half+50;
}
static bool clock_sample(uint64_t now,uint64_t start,zt_clock_sample_t *out)
{
    gateway_t *g=zt_gw;
    if (!g->anchor_ms || !start || now<g->anchor_us) return false;
    uint64_t age=(now-g->anchor_us)/1000;
    uint64_t uncertainty=g->anchor_uncertainty_ms+(age+4999)/5000;
    int64_t elapsed=(int64_t)(g->anchor_ms+age)-(int64_t)start;
    if (uncertainty>ZT_TIME_UNCERTAINTY_MAX_MS || elapsed<=INT32_MIN || elapsed>INT32_MAX) return false;
    *out=(zt_clock_sample_t){(int32_t)elapsed,(uint32_t)uncertainty,now,ZT_TIME_INITIALIZED};
    return true;
}
zt_err_t gw_send(zt_gateway_message_t *message)
{
    gateway_t *g=zt_gw; size_t written;
    uint64_t now=esp_timer_get_time();
    /* One application request at a time keeps its reply within the single RX
     * mailbox. Bound recovery after a lost reply and pace backlog draining. */
    if (now<g->send_due || (g->response_pending && now<g->response_due)) return ZT_ERR_BUSY;
    message->envelope.v=ZT_GATEWAY_SCHEMA_VERSION;
    message->envelope.id=++g->client_id;
    message->envelope.ts=(uint64_t)time(NULL)*1000;
    zt_err_t r=zt_gateway_encode(message,g->scratch.tx,sizeof(g->scratch.tx),&written);
    if (r==ZT_OK) r=zt_gateway_ws_send((uint8_t *)g->scratch.tx,written);
    if (r==ZT_OK) {
        g->response_pending=1; g->response_due=now+1000000ULL;
        g->send_due=now+100000ULL;
    }
    return r;
}

static bool same_join(const zt_game_feed_item_t *a,const zt_game_feed_item_t *b)
{
    return a->body.join.boot_nonce==b->body.join.boot_nonce &&
        a->body.join.request.request_nonce==b->body.join.request.request_nonce &&
        !memcmp(&a->body.join.request.requested_badge_mac,
                &b->body.join.request.requested_badge_mac,sizeof(zt_mac_t));
}

static uint64_t control_key(const zt_game_feed_item_t *item)
{
    uint64_t h=UINT64_C(14695981039346656037);
    for (unsigned i=0;i<6;++i) { h^=zt_gw->host.bytes[i]; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<8;++i) { h^=(item->body.control.boot_nonce>>(8*i))&255; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<4;++i) { h^=(item->body.control.request_seq>>(8*i))&255; h*=UINT64_C(1099511628211); }
    h^=item->body.control.action; h*=UINT64_C(1099511628211);
    return h ? h : 1;
}

zt_err_t zt_gateway_submit_feed(const zt_game_feed_item_t *item)
{
    gateway_t *g=zt_gw;
    if (!g) return ZT_ERR_INVALID_STATE;
    if (!item) return ZT_ERR_INVALID_ARG;
    zt_err_t result=ZT_ERR_BUSY;
    portENTER_CRITICAL(&g->guard);
    if (item->kind==ZT_GAME_FEED_EVENT) {
        /* The game already committed this immutable event. Scan that journal
         * rather than allocating another volatile queue or taking custody. */
        g->event_due=0; result=ZT_OK;
    } else if (item->kind==ZT_GAME_FEED_DECISION_RECEIPT) {
        const zt_wire_decision_receipt_t *r=&item->body.decision_receipt;
        if (!item->round_id || r->slot>=ZT_MAX_PLAYERS) result=ZT_ERR_INVALID_ARG;
        else for (unsigned i=0;i<ZT_RETAINED_ROUND_CAPACITY;++i) {
            gateway_decision_ack_t *a=&g->decision_acks[i];
            if (a->round && a->round!=item->round_id) continue;
            a->round=item->round_id;
            if (a->through[r->slot]<r->own_decided_contiguous) a->through[r->slot]=r->own_decided_contiguous;
            a->dirty|=1u<<r->slot; result=ZT_OK; break;
        }
    } else if (item->kind==ZT_GAME_FEED_CONTROL) {
        const zt_host_control_t action=item->body.control.action;
        if (!item->body.control.boot_nonce || !item->body.control.request_seq || !item->body.control.registration_id ||
            (action!=ZT_HOST_CONTROL_START && action!=ZT_HOST_CONTROL_RESET) ||
            (action==ZT_HOST_CONTROL_START && item->round_id) || (action==ZT_HOST_CONTROL_RESET && !item->round_id))
            result=ZT_ERR_INVALID_ARG;
        else {
            uint64_t key=control_key(item);
            if (g->control.occupied && g->control.request.request_id==key &&
                g->control.request.registration_id==item->body.control.registration_id &&
                g->control.request.round_id==item->round_id && g->control.request.action==action) {
                g->control.delivered=0; result=ZT_OK;
            } else if (!g->control.occupied || g->control.delivered) {
                g->control=(gateway_control_t){.item=*item,.occupied=1,
                    .request={.action=action,.request_id=key,.registration_id=item->body.control.registration_id,.round_id=item->round_id}};
                result=ZT_OK;
            }
        }
    } else if (item->kind==ZT_GAME_FEED_JOIN) {
        int free_slot=-1, cached_slot=-1;
        for (unsigned i=0;i<GW_JOINS;++i) {
            gateway_join_t *j=&g->joins[i];
            if (!j->occupied) { if (free_slot<0) free_slot=i; continue; }
            if (same_join(&j->item,item)) {
                /* A lost radio reply needs a replay of this validated result,
                 * not another HTTPS exchange and WSS teardown. */
                j->delivered=0;
                result=ZT_OK; break;
            }
            if (j->delivered && (cached_slot<0 || !memcmp(&j->item.body.join.request.requested_badge_mac,
                    &item->body.join.request.requested_badge_mac,sizeof(zt_mac_t)))) cached_slot=i;
        }
        if (free_slot<0) free_slot=cached_slot;
        if (result!=ZT_OK && free_slot>=0) { g->joins[free_slot]=(gateway_join_t){.item=*item,.occupied=1}; result=ZT_OK; }
    } else if (item->kind==ZT_GAME_FEED_COMMAND_RECEIPT || item->kind==ZT_GAME_FEED_ROUND_CLOSED) {
        uint8_t slot=item->kind==ZT_GAME_FEED_COMMAND_RECEIPT ? item->body.command_receipt.slot : item->body.round_closed.slot;
        int index=-1;
        for (unsigned i=0;i<GW_ACKS;++i) {
            gateway_ack_t *a=&g->acks[i];
            if (!a->occupied) { if (index<0) index=i; continue; }
            uint8_t prior=a->item.kind==ZT_GAME_FEED_COMMAND_RECEIPT ? a->item.body.command_receipt.slot : a->item.body.round_closed.slot;
            if (a->item.kind==item->kind && a->item.round_id==item->round_id && prior==slot) { index=i; break; }
        }
        if (index>=0) {
            gateway_ack_t *a=&g->acks[index]; bool replace=true;
            if (a->occupied && item->kind==ZT_GAME_FEED_COMMAND_RECEIPT) {
                const zt_wire_command_receipt_t *old=&a->item.body.command_receipt, *next=&item->body.command_receipt;
                if (next->command_seq<old->command_seq || (next->command_seq==old->command_seq && next->state==ZT_RECEIPT_RECEIVED && old->state!=ZT_RECEIPT_RECEIVED)) replace=false;
            }
            if (replace) *a=(gateway_ack_t){.item=*item,.occupied=1};
            result=ZT_OK;
        }
    } else result=ZT_ERR_INVALID_ARG;
    portEXIT_CRITICAL(&g->guard);
    if (g->owner && result==ZT_OK) xTaskNotifyGive(g->owner);
    return result;
}

static zt_err_t queue_commands(const zt_gateway_commands_t *batch)
{
    gateway_t *g=zt_gw;
    for (unsigned i=0;i<batch->count;++i) if (batch->entries[i].type==ZT_CMD_RESET_GAME && !g->resetting) {
        g->resetting=1; g->command_count=0;
        g->event_count=0; g->event_sent_us=0; g->requested_count=0;
        break;
    }
    for (unsigned i=0;i<batch->count;++i) {
        const zt_gateway_command_t *c=&batch->entries[i]; bool exists=false;
        if (g->resetting && c->type!=ZT_CMD_RESET_GAME) continue;
        for (unsigned j=0;j<g->command_count;++j) if (g->commands[j].seq==c->seq && g->commands[j].round_id==c->round_id) { exists=true; break; }
        if (exists) continue;
        if (c->type==ZT_CMD_ANNOUNCE) {
            if (c->round_id!=g->round) continue;
            unsigned announcements=0;
            for (unsigned j=0;j<g->command_count;++j) if (g->commands[j].type==ZT_CMD_ANNOUNCE) ++announcements;
            if (announcements>=ZT_ANNOUNCE_QUEUE_CAPACITY) continue;
        } else if (g->command_count==GW_COMMANDS) {
            /* Dropped cosmetics remain eligible for bounded server replay, but
             * cannot block a state-changing command from entering the bridge. */
            for (unsigned j=0;j<g->command_count;++j) if (g->commands[j].type==ZT_CMD_ANNOUNCE) {
                memmove(g->commands+j,g->commands+j+1,(--g->command_count-j)*sizeof(g->commands[0]));
                break;
            }
        }
        if (g->command_count==GW_COMMANDS && (c->type==ZT_CMD_END_ROUND || c->type==ZT_CMD_FINAL_RESULT)) {
            /* The server retains unacknowledged commands. Make room for a
             * terminal state rather than let old admission work delay game over. */
            for (unsigned j=0;j<g->command_count;++j) {
                zt_command_kind_t type=g->commands[j].type;
                if (type==ZT_CMD_END_ROUND || type==ZT_CMD_FINAL_RESULT || type==ZT_CMD_RESET_GAME) continue;
                memmove(g->commands+j,g->commands+j+1,(--g->command_count-j)*sizeof(g->commands[0]));
                break;
            }
        }
        /* The server retains every command until actual badge receipts arrive.
         * A full relay queue must not pin the only inbound mailbox: doing so
         * stops clock/ACK/event traffic and eventually tears down live sync for
         * everyone while one unreachable target occupies the game queue.
         * Leave overflow for server replay without claiming badge application. */
        if (g->command_count==GW_COMMANDS) continue;
        g->commands[g->command_count++]=*c;
    }
    return ZT_OK;
}
static bool same_event(const zt_event_id_t *a,const zt_event_id_t *b)
{
    return a->round_id==b->round_id && a->victim_slot==b->victim_slot && a->event_seq==b->event_seq;
}
static void received_event(const zt_event_id_t *id)
{
    gateway_t *g=zt_gw;
    for (unsigned i=0;i<g->event_count;++i) if (same_event(&g->events[i].id,id)) {
        memmove(g->events+i,g->events+i+1,(--g->event_count-i)*sizeof(g->events[0]));
        break;
    }
    if (!g->event_count) g->event_sent_us=0;
}
zt_err_t zt_gateway_bridge_publish(const zt_gateway_message_t *verified)
{
    gateway_t *g=zt_gw;
    if (!g || !verified) return ZT_ERR_INVALID_ARG;
    uint64_t now=esp_timer_get_time();
    if (!zt_game_admission_enabled()) {
        /* A connection is only transport readiness. Keep welcome/time traffic
         * flowing, but do not restore a round before this boot's A registration. */
        switch (verified->envelope.t) {
        case ZT_GATEWAY_COMMANDS:
            /* Reset receipts and cleanup relay outlive the host's own admission. */
            for (unsigned i=0;i<verified->body.commands.count;++i)
                if (verified->body.commands.entries[i].type!=ZT_CMD_RESET_GAME) return ZT_OK;
            break;
        case ZT_GATEWAY_SNAPSHOT: case ZT_GATEWAY_RECEIPTS:
        case ZT_GATEWAY_DECISIONS: case ZT_GATEWAY_NEED_EVENTS:
            return ZT_OK;
        default: break;
        }
    }
    switch (verified->envelope.t) {
    case ZT_GATEWAY_WELCOME: {
        const zt_gateway_welcome_t *w=&verified->body.welcome;
        if (!g->hello_sent) return ZT_ERR_PROTOCOL;
        if (g->round && (!w->round_id || (g->resetting && !w->resetting))) {
            /* Server reset is immediate; archived cleanup has its own session.
             * Old transport work must not enter the new lobby. */
            portENTER_CRITICAL(&g->guard);
            memset(g->joins,0,sizeof(g->joins)); memset(g->acks,0,sizeof(g->acks));
            memset(g->decision_acks,0,sizeof(g->decision_acks));
            portEXIT_CRITICAL(&g->guard);
            g->command_count=0; g->event_count=0; g->requested_count=0;
            memset(g->event_rounds,0,sizeof(g->event_rounds));
            memset(g->event_cursor,0,sizeof(g->event_cursor));
            g->event_sent_us=0; g->start_ms=0;
        }
        if (w->resetting) {
            g->command_count=0; g->event_count=0; g->requested_count=0; g->event_sent_us=0;
        }
        /* Socket writes are not backend durability receipts. Replay cached
         * actual victim frontiers on every reconnect. */
        portENTER_CRITICAL(&g->guard);
        for (unsigned i=0;i<GW_ACKS;++i) g->acks[i].sent_us=0;
        for (unsigned i=0;i<ZT_RETAINED_ROUND_CAPACITY;++i) {
            if (g->decision_acks[i].round!=w->round_id) memset(&g->decision_acks[i],0,sizeof(g->decision_acks[i]));
            for (unsigned slot=0;slot<ZT_MAX_PLAYERS;++slot)
                if (g->decision_acks[i].through[slot]) g->decision_acks[i].dirty|=1u<<slot;
        }
        portEXIT_CRITICAL(&g->guard);
        g->status.welcomed=1; g->backoff=0; g->retry_us=0; g->status.last_error=ZT_OK;
        g->join_burst=0; g->join_resume_us=now+500000ULL;
        g->round=w->round_id; g->snapshot_id=w->snapshot_id; g->snapshot_pages=w->snapshot_pages;
        g->resetting=w->resetting;
        g->snapshot_mask=0; g->snapshot_request_pending=0; g->page_due=now;
        next_empty_ack_us=now;
        gw_anchor(w->server_time_ms,g->hello_us,now); g->sync_due=now;
        return ZT_OK;
    }
    case ZT_GATEWAY_SNAPSHOT: {
        zt_server_snapshot_t snapshot=verified->body.snapshot;
        if (!snapshot.round_id || !snapshot.entry_count) return ZT_OK; /* lobby metadata */
        if (snapshot.snapshot_id!=g->snapshot_id || snapshot.round_id!=g->round) {
            g->snapshot_mask=0; g->snapshot_id=snapshot.snapshot_id; g->round=snapshot.round_id;
        }
        snapshot.server_id=0; /* Admission durability is not a whole-outbox cursor. */
        zt_err_t r=g->sinks.snapshot ? g->sinks.snapshot(&snapshot,g->sinks.context) : ZT_ERR_INVALID_STATE;
        if (r==ZT_OK) {
            g->snapshot_pages=snapshot.page_count; g->snapshot_mask|=1u<<snapshot.page_index;
            g->snapshot_request_pending=0; g->page_due=now;
            if (snapshot.start_time_ms) g->start_ms=snapshot.start_time_ms;
        }
        return r;
    }
    case ZT_GATEWAY_COMMANDS: return queue_commands(&verified->body.commands);
    case ZT_GATEWAY_RECEIPTS:
        for (;g->inbound_index<verified->body.receipts.count;++g->inbound_index) {
            const zt_event_id_t *id=&verified->body.receipts.ids[g->inbound_index];
            zt_server_receipt_t receipt={.round_id=id->round_id,.server_id=0,.event_id=*id};
            zt_err_t r=g->sinks.receipt ? g->sinks.receipt(&receipt,g->sinks.context) : ZT_ERR_INVALID_STATE;
            if (r!=ZT_OK) return r;
            received_event(id);
        }
        return ZT_OK;
    case ZT_GATEWAY_DECISIONS:
        for (;g->inbound_index<verified->body.decisions.count;++g->inbound_index) {
            const zt_gateway_decision_t *d=&verified->body.decisions.entries[g->inbound_index];
            zt_server_decision_t decision={.round_id=d->id.round_id,.server_id=0,
                .decision={.victim_slot=d->id.victim_slot,.event_seq=d->id.event_seq,
                    .status=d->status,.reason=d->reason,.archived=d->archived}};
            zt_err_t r=g->sinks.decision ? g->sinks.decision(&decision,g->sinks.context) : ZT_ERR_INVALID_STATE;
            if (r!=ZT_OK) return r;
        }
        return ZT_OK;
    case ZT_GATEWAY_NEED_EVENTS:
        g->requested_count=verified->body.need_events.count;
        memcpy(g->requested_events,verified->body.need_events.events,g->requested_count*sizeof(g->requested_events[0]));
        g->event_due=0;
        return ZT_OK;
    case ZT_GATEWAY_TIME_SYNC_REPLY:
        if (!g->sync_us || verified->body.time_sync_reply.nonce!=g->sync_nonce) return ZT_ERR_STALE;
        gw_anchor(verified->body.time_sync_reply.server_time_ms,g->sync_us,now); g->sync_us=0; return ZT_OK;
    case ZT_GATEWAY_ERROR:
        if (verified->body.error.fatal) g->stopped=1;
        return ZT_ERR_PROTOCOL;
    default: return ZT_ERR_NOT_IMPLEMENTED;
    }
}

static bool command_service(unsigned index,uint64_t now)
{
    gateway_t *g=zt_gw;
    zt_gateway_command_t *c=&g->commands[index]; zt_checkpoint_t cp;
    if (c->type==ZT_CMD_ANNOUNCE) {
        zt_clock_sample_t clock;
        if (c->round_id!=g->round || zt_clock_read(now,&clock)!=ZT_OK || clock.quality!=ZT_TIME_INITIALIZED ||
            clock.elapsed_ms<0 || (uint32_t)clock.elapsed_ms>=c->valid_until_elapsed_ms) {
            memmove(g->commands+index,g->commands+index+1,(--g->command_count-index)*sizeof(g->commands[0]));
            return true;
        }
    }
    if (!zt_game_admission_enabled() && c->type!=ZT_CMD_RESET_GAME) return false;
    if (c->type!=ZT_CMD_RESET_GAME && (zt_store_load_checkpoint(c->round_id,&cp)!=ZT_OK || cp.round_id!=c->round_id)) return false;
    zt_server_command_t target={.round_id=c->round_id,.server_id=0,
        .command={.command_seq=c->seq,.kind=c->type,.target_slot=c->target,.valid_until_elapsed_ms=c->valid_until_elapsed_ms}};
    size_t size=0; zt_err_t r=ZT_ERR_NOT_IMPLEMENTED;
    if (c->type==ZT_CMD_PREPARE_ROUND) {
        if (cp.snapshot_rev<c->args.prepare.snapshot_rev || cp.roster_hash!=c->args.prepare.roster_hash) return false;
        r=zt_wire_encode_prepare_round_args(&c->args.prepare,target.command.args,sizeof(target.command.args),&size);
    } else if (c->type==ZT_CMD_START_ROUND) {
        if (cp.phase<ZT_PHASE_PREPARED) return false;
        zt_clock_sample_t clock;
        if (!clock_sample(now,c->args.start.start_time_ms,&clock)) { g->sync_due=0; return false; }
        zt_wire_start_round_args_t a={c->args.start.snapshot_id,c->args.start.roster_hash,c->args.start.patient_zero_slot,
            c->args.start.initial_role_rev,clock.elapsed_ms,c->args.start.duration_ms,(uint16_t)clock.uncertainty_ms};
        r=zt_wire_encode_start_round_args(&a,target.command.args,sizeof(target.command.args),&size);
        g->start_ms=c->args.start.start_time_ms;
        /* Game owner also applies the fresh sample carried in START. */
    } else if (c->type==ZT_CMD_ROLE_SET)
        r=zt_wire_encode_role_set_args(&c->args.role_set,target.command.args,sizeof(target.command.args),&size);
    else if (c->type==ZT_CMD_ANNOUNCE)
        r=zt_wire_encode_announce_args(&c->args.announce,target.command.args,sizeof(target.command.args),&size);
    else if (c->type==ZT_CMD_END_ROUND)
        r=zt_wire_encode_end_round_args(&c->args.end,target.command.args,sizeof(target.command.args),&size);
    else if (c->type==ZT_CMD_FINAL_RESULT)
        r=zt_wire_encode_final_result_args(&c->args.final_result,target.command.args,sizeof(target.command.args),&size);
    else if (c->type==ZT_CMD_RESET_GAME) {
        r=ZT_OK; g->resetting=1;
        if (c->args.reset.registration_id) {
            memcpy(target.command.args,c->args.reset.target_mac.bytes,6);
            for (unsigned i=0;i<8;++i) target.command.args[6+i]=(uint8_t)(c->args.reset.registration_id>>(8*i));
            size=14;
        }
    }
    if (r==ZT_OK) { target.command.args_len=size; r=g->sinks.command ? g->sinks.command(&target,g->sinks.context) : ZT_ERR_INVALID_STATE; }
    if (r==ZT_OK || r==ZT_ERR_NOT_IMPLEMENTED) {
        memmove(g->commands+index,g->commands+index+1,(--g->command_count-index)*sizeof(g->commands[0]));
        return true;
    } else if (r!=ZT_ERR_BUSY) g->status.last_error=r;
    return false;
}

static void commands_service(uint64_t now)
{
    gateway_t *g=zt_gw;
    /* Check each bounded entry, prioritizing end/results before other state
     * changes and then cosmetic announcements. A command awaiting
     * its clock, snapshot or game slot cannot hold unrelated targets or RESET
     * behind it. At most one successful transfer per service turn. */
    for (unsigned priority=0;priority<3;++priority) {
        for (unsigned i=0;i<g->command_count;++i) {
            bool terminal=g->commands[i].type==ZT_CMD_END_ROUND || g->commands[i].type==ZT_CMD_FINAL_RESULT;
            unsigned command_priority=terminal ? 0 : g->commands[i].type==ZT_CMD_ANNOUNCE ? 2 : 1;
            if (command_priority!=priority) continue;
            if (command_service(i,now)) return;
        }
    }
}

static void hello_build(zt_gateway_hello_t *hello)
{
    gateway_t *g=zt_gw; memset(hello,0,sizeof(*hello));
    hello->host_id=g->host; hello->game_id=g->game; hello->proto=ZT_PROTOCOL_VERSION;
    memcpy(hello->fw,ZT_BUILD_ID,ZT_BUILD_ID_LEN);
    zt_mesh_get_boot_nonce(&hello->host_boot);
    hello->registration_id=zt_game_registration_id();
    zt_channel_status_t channel; if (zt_channel_get_status(&channel)==ZT_OK) hello->channel=channel.channel;
    zt_checkpoint_t cp;
    if (zt_game_admission_enabled() && zt_store_load_checkpoint(0,&cp)==ZT_OK) {
        hello->round_id=cp.round_id; hello->state_rev=cp.state_rev;
        /* The initial adapter replays from zero. Never claim unproven cursor durability. */
        for (unsigned i=0;i<cp.roster_count;++i) {
            uint8_t slot=cp.roster[i].slot;
            hello->decided_through[hello->decided_count++]=(zt_gateway_frontier_t){slot,cp.decided[slot]};
            if (cp.produced[slot]>=cp.decided[slot]) hello->pending_events+=cp.produced[slot]-cp.decided[slot];
        }
    }
    zt_reset_receipt_t reset;
    if (!hello->round_id && g->resetting && g->round && zt_game_admission_enabled() &&
        zt_store_load_reset(g->round,&reset)!=ZT_OK) hello->round_id=g->round;
}
static uint64_t registration_key(const zt_game_feed_item_t *item)
{
    uint64_t h=UINT64_C(14695981039346656037);
    const zt_wire_join_t *j=&item->body.join.request;
    for (unsigned i=0;i<6;++i) { h^=j->requested_badge_mac.bytes[i]; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<8;++i) { h^=(item->body.join.boot_nonce>>(8*i))&255; h*=UINT64_C(1099511628211); }
    for (unsigned i=0;i<4;++i) { h^=(j->request_nonce>>(8*i))&255; h*=UINT64_C(1099511628211); }
    return h ? h : 1;
}

static bool start_waits_for_registration(void)
{
    gateway_t *g=zt_gw;
    bool waiting=false;
    portENTER_CRITICAL(&g->guard);
    if (g->control.occupied && !g->control.answered && !g->control.delivered &&
        g->control.request.action==ZT_HOST_CONTROL_START) {
        for (unsigned i=0;i<GW_JOINS;++i) {
            const gateway_join_t *join=&g->joins[i];
            /* Let already queued registrations reach the server before Start
             * freezes the roster. A completed response, radio delivery wait,
             * or failed HTTP attempt must not hold the control indefinitely. */
            if (join->occupied && !join->answered && !join->backoff) { waiting=true; break; }
        }
    }
    portEXIT_CRITICAL(&g->guard);
    return waiting;
}

static bool control_service(bool network,uint64_t now)
{
    gateway_t *g=zt_gw;
    gateway_control_t copy;
    portENTER_CRITICAL(&g->guard); copy=g->control; portEXIT_CRITICAL(&g->guard);
    if (!copy.occupied || copy.delivered) return false;
    bool used_http=false;
    if (!copy.answered && g->stopped) {
        copy.result=g->status.auth_error ? ZT_ERR_AUTH : g->status.last_error;
        if (copy.result==ZT_OK) copy.result=ZT_ERR_INVALID_STATE;
        copy.answered=1;
        portENTER_CRITICAL(&g->guard);
        if (g->control.request.request_id==copy.request.request_id) g->control=copy;
        portEXIT_CRITICAL(&g->guard);
    }
    if (!copy.answered) {
        if (!network || g->resetting || g->stopped || now<copy.retry_us ||
            (g->ws && !g->status.welcomed) || (g->response_pending && now<g->response_due)) return false;
        if (g->ws) gw_ws_destroy();
        used_http=true;
        zt_gateway_control_response_t response;
        copy.result=zt_gateway_control(&copy.request,&response);
        if (copy.result==ZT_ERR_NETWORK || copy.result==ZT_ERR_TIMEOUT || copy.result==ZT_ERR_BUSY ||
            copy.result==ZT_ERR_NO_SPACE || copy.result==ZT_ERR_INVALID_STATE) {
            static const uint32_t delays[]=ZT_GATEWAY_BACKOFF_MS;
            uint32_t delay=delays[copy.backoff];
            if (copy.backoff+1<ZT_GATEWAY_BACKOFF_COUNT) ++copy.backoff;
            copy.retry_us=esp_timer_get_time()+(delay-delay/5+esp_random()%(2*(delay/5)+1))*1000ULL;
            if (g->http_retry_us>copy.retry_us) copy.retry_us=g->http_retry_us;
        } else {
            copy.answered=1;
            if (copy.result==ZT_OK) {
                g->retry_us=0; g->backoff=0;
                if (copy.request.action==ZT_HOST_CONTROL_RESET) g->resetting=1;
            }
        }
        portENTER_CRITICAL(&g->guard);
        if (g->control.request.request_id==copy.request.request_id) g->control=copy;
        portEXIT_CRITICAL(&g->guard);
    }
    if (copy.answered && zt_game_post_control_result(copy.item.body.control.action,
            copy.item.body.control.request_seq,copy.result)==ZT_OK) {
        portENTER_CRITICAL(&g->guard);
        if (g->control.request.request_id==copy.request.request_id) g->control.delivered=1;
        portEXIT_CRITICAL(&g->guard);
    }
    return used_http;
}

static bool joins_service(bool network,uint64_t now)
{
    gateway_t *g=zt_gw;
    unsigned first=g->join_next;
    for (unsigned offset=0;offset<GW_JOINS;++offset) {
        unsigned i=(first+offset)%GW_JOINS;
        gateway_join_t copy;
        portENTER_CRITICAL(&g->guard); copy=g->joins[i]; portEXIT_CRITICAL(&g->guard);
        if (!copy.occupied || copy.delivered) continue;
        bool used_http=false;
        if (!copy.answered) {
            if (!network || now<copy.retry_us || g->stopped || now<g->join_resume_us ||
                g->join_burst>=4 || (g->ws && !g->status.welcomed) ||
                (g->response_pending && now<g->response_due)) continue;
            g->join_next=(i+1)%GW_JOINS;
            /* One TLS session at a time: close WSS before short HTTPS registration. */
            if (g->ws) gw_ws_destroy();
            used_http=true;
            ++g->join_burst;
            const zt_wire_join_t *j=&copy.item.body.join.request;
            zt_registration_request_t request={.badge_id=j->requested_badge_mac,.known_round_id=j->known_round_id,
                .stable_request_id=registration_key(&copy.item)};
            zt_err_t r=ZT_OK;
            if (j->name_len<ZT_NAME_MIN_LEN || j->name_len>ZT_NAME_MAX_LEN) {
                copy.response=(zt_registration_response_t){.status=ZT_JOIN_BAD_CONFIGURATION,.slot=ZT_SLOT_INVALID};
            } else {
                memcpy(request.name,j->name,j->name_len); memcpy(request.fw,j->build_id,ZT_BUILD_ID_LEN);
                r=zt_gateway_register(&request,&copy.response);
            }
            if (r!=ZT_OK) {
                static const uint32_t delays[]=ZT_GATEWAY_BACKOFF_MS;
                uint32_t delay=delays[copy.backoff];
                if (copy.backoff+1<ZT_GATEWAY_BACKOFF_COUNT) ++copy.backoff;
                copy.retry_us=esp_timer_get_time()+(delay-delay/5+esp_random()%(2*(delay/5)+1))*1000ULL;
                if (g->http_retry_us>copy.retry_us) copy.retry_us=g->http_retry_us;
                portENTER_CRITICAL(&g->guard);
                if (same_join(&g->joins[i].item,&copy.item)) {
                    g->joins[i].retry_us=copy.retry_us; g->joins[i].backoff=copy.backoff;
                }
                portEXIT_CRITICAL(&g->guard);
                return true;
            }
            copy.answered=1;
            portENTER_CRITICAL(&g->guard);
            if (same_join(&g->joins[i].item,&copy.item)) {
                g->joins[i].response=copy.response; g->joins[i].answered=1;
            }
            portEXIT_CRITICAL(&g->guard);
            /* A validated registration reply proves reachability. Reuse the
             * already-validated socket path instead of another TLS bootstrap. */
            g->retry_us=0; g->backoff=0;
        }
        zt_wire_join_result_t result={.target_mac=copy.item.body.join.request.requested_badge_mac,
            .request_nonce=copy.item.body.join.request.request_nonce,.status=copy.response.status,
            .slot=copy.response.slot,.snapshot_rev=copy.response.state_rev};
        g->join_next=(i+1)%GW_JOINS;
        zt_err_t posted=zt_game_post_registration(copy.response.round_id,&result);
        if (posted!=ZT_OK) {
            if (posted!=ZT_ERR_BUSY) g->status.last_error=posted;
            if (used_http) return true;
            continue;
        }
        portENTER_CRITICAL(&g->guard);
        if (same_join(&g->joins[i].item,&copy.item)) g->joins[i].delivered=1;
        portEXIT_CRITICAL(&g->guard);
        /* Return only when HTTPS consumed this turn. Queued retries and cached
         * radio replies must never suppress reconnect or live gateway work. */
        if (used_http) return true;
    }
    return false;
}

static bool acknowledgments_service(uint64_t now)
{
    gateway_t *g=zt_gw;
    zt_gateway_message_t m={.envelope.t=ZT_GATEWAY_ACK};
    m.body.ack.round_id=g->round;
    uint8_t applied_indices[ZT_GATEWAY_ACK_ARRAY_MAX], closed_indices[ZT_GATEWAY_ACK_ARRAY_MAX];
    unsigned next=g->ack_next;
    for (unsigned offset=0;offset<GW_ACKS;++offset) {
        unsigned i=(g->ack_next+offset)%GW_ACKS;
        gateway_ack_t copy;
        portENTER_CRITICAL(&g->guard); copy=g->acks[i]; portEXIT_CRITICAL(&g->guard);
        if (copy.occupied && copy.item.round_id!=g->round) {
            portENTER_CRITICAL(&g->guard);
            if (!memcmp(&g->acks[i].item,&copy.item,sizeof(copy.item))) memset(&g->acks[i],0,sizeof(g->acks[i]));
            portEXIT_CRITICAL(&g->guard);
            continue;
        }
        if (!copy.occupied || (copy.sent_us && now>=copy.sent_us && now-copy.sent_us<2000000ULL)) continue;
        if (copy.item.kind==ZT_GAME_FEED_COMMAND_RECEIPT) {
            if (m.body.ack.applied_count==ZT_GATEWAY_ACK_ARRAY_MAX) continue;
            zt_wire_command_receipt_t *r=&copy.item.body.command_receipt;
            unsigned j=m.body.ack.applied_count++;
            applied_indices[j]=i;
            m.body.ack.applied[j]=(zt_gateway_applied_t){r->command_seq,r->slot,r->state,r->applied_snapshot_rev};
            if (r->state==ZT_RECEIPT_PREPARED_READY)
                m.body.ack.ready[m.body.ack.ready_count++]=(zt_gateway_ready_t){r->slot,r->applied_snapshot_rev,copy.item.round_id};
        } else {
            if (m.body.ack.round_closed_count==ZT_GATEWAY_ACK_ARRAY_MAX) continue;
            unsigned j=m.body.ack.round_closed_count++;
            closed_indices[j]=i;
            m.body.ack.round_closed[j]=(zt_gateway_closed_t){copy.item.body.round_closed.slot,copy.item.body.round_closed.produced_seq};
        }
        next=(i+1)%GW_ACKS;
    }
    int decision_index=-1;
    if (!g->resetting && now>=g->decision_ack_due) for (unsigned i=0;i<ZT_RETAINED_ROUND_CAPACITY;++i) {
        gateway_decision_ack_t copy;
        portENTER_CRITICAL(&g->guard); copy=g->decision_acks[i]; portEXIT_CRITICAL(&g->guard);
        if (!copy.round || copy.round!=g->round || !copy.dirty) continue;
        decision_index=i;
        for (unsigned slot=0;slot<ZT_MAX_PLAYERS && m.body.ack.decision_applied_count<ZT_GATEWAY_ACK_ARRAY_MAX;++slot)
            if (copy.dirty&(1u<<slot)) m.body.ack.decision_applied[m.body.ack.decision_applied_count++]=
                (zt_gateway_decision_applied_t){slot,copy.through[slot]};
        break;
    }
    bool has_receipts=m.body.ack.applied_count || m.body.ack.round_closed_count || m.body.ack.decision_applied_count;
    if (!has_receipts) {
        if (now<next_empty_ack_us) return false;
        bool admitted=!g->round || g->resetting || !zt_game_admission_enabled();
        if (g->round && g->snapshot_pages && g->snapshot_mask==((1u<<g->snapshot_pages)-1)) {
            zt_checkpoint_t cp;
            admitted=admitted || (zt_store_load_checkpoint(g->round,&cp)==ZT_OK && cp.snapshot_rev>=g->snapshot_id);
        }
        if (!admitted) return false;
    }
    if (gw_send(&m)==ZT_OK) {
        portENTER_CRITICAL(&g->guard);
        for (unsigned j=0;j<m.body.ack.applied_count;++j) {
            gateway_ack_t *a=&g->acks[applied_indices[j]];
            const zt_gateway_applied_t *sent=&m.body.ack.applied[j];
            const zt_wire_command_receipt_t *current=&a->item.body.command_receipt;
            if (a->occupied && a->item.round_id==m.body.ack.round_id && a->item.kind==ZT_GAME_FEED_COMMAND_RECEIPT &&
                current->command_seq==sent->seq && current->slot==sent->slot && current->state==sent->result &&
                current->applied_snapshot_rev==sent->state_rev) a->sent_us=now;
        }
        for (unsigned j=0;j<m.body.ack.round_closed_count;++j) {
            gateway_ack_t *a=&g->acks[closed_indices[j]];
            const zt_gateway_closed_t *sent=&m.body.ack.round_closed[j];
            if (a->occupied && a->item.round_id==m.body.ack.round_id && a->item.kind==ZT_GAME_FEED_ROUND_CLOSED &&
                a->item.body.round_closed.slot==sent->slot && a->item.body.round_closed.produced_seq==sent->produced_seq)
                a->sent_us=now;
        }
        if (decision_index>=0) for (unsigned j=0;j<m.body.ack.decision_applied_count;++j) {
            const zt_gateway_decision_applied_t *sent=&m.body.ack.decision_applied[j];
            gateway_decision_ack_t *a=&g->decision_acks[decision_index];
            if (a->round==m.body.ack.round_id && a->through[sent->slot]==sent->through_seq) a->dirty&=~(1u<<sent->slot);
        }
        portEXIT_CRITICAL(&g->guard);
        g->ack_next=next;
        if (decision_index>=0) g->decision_ack_due=now+100000ULL;
        next_empty_ack_us=now+2000000ULL;
    }
    return true; /* Retain receipts across sends and replay them on reconnect. */
}

static bool events_service(uint64_t now)
{
    gateway_t *g=zt_gw;
    if (!zt_game_admission_enabled() || g->resetting || !g->round || now<g->event_due ||
        (g->event_sent_us && now>=g->event_sent_us && now-g->event_sent_us<2000000ULL)) return false;
    if (!g->event_count && g->requested_count) {
        /* Requested parents may have receipts already. Explicit replay must
         * bypass receipt filtering but still read the durable immutable body. */
        unsigned pending=0;
        for (unsigned i=0;i<g->requested_count;++i) {
            zt_event_id_t id=g->requested_events[i];
            if (id.round_id!=g->round) continue; /* Archived ingestion is not exposed yet. */
            if (g->event_count && id.round_id!=g->events[0].id.round_id) {
                g->requested_events[pending++]=id; continue;
            }
            zt_wire_event_t event;
            zt_err_t r=zt_store_read_event(id.round_id,id.victim_slot,id.event_seq,&event);
            if (r==ZT_OK)
                g->events[g->event_count++]=(zt_gateway_event_t){id,event};
            else if (r==ZT_ERR_BUSY) g->requested_events[pending++]=id;
        }
        g->requested_count=pending;
    }
    if (!g->event_count) {
        /* This backend accepts active-round evidence. Older retained journals
         * remain local for export; do not let one block the current batch. */
        zt_round_id_t rounds[ZT_RETAINED_ROUND_CAPACITY]={g->round,0};
        unsigned first=g->event_turn;
        for (unsigned pass=0;pass<ZT_RETAINED_ROUND_CAPACITY && !g->event_count;++pass) {
            unsigned index=(first+pass)%ZT_RETAINED_ROUND_CAPACITY;
            zt_round_id_t round=rounds[index];
            if (!round) continue;
            if (g->event_rounds[index]!=round) { g->event_rounds[index]=round; g->event_cursor[index]=0; }
            zt_checkpoint_t cp;
            if (zt_store_load_checkpoint(round,&cp)!=ZT_OK) continue;
            size_t count=0; uint16_t next=g->event_cursor[index];
            if (zt_game_event_feed(round,g->event_cursor[index],g->events,ZT_GATEWAY_EVENTS_MAX,&count,&next)!=ZT_OK) continue;
            g->event_cursor[index]=next>=ZT_JOURNAL_CAPACITY ? 0 : next;
            for (unsigned i=0;i<count;++i) if (g->events[i].id.victim_slot<ZT_MAX_PLAYERS &&
                cp.received[g->events[i].id.victim_slot]<g->events[i].id.event_seq)
                g->events[g->event_count++]=g->events[i];
            g->event_turn=(index+1)%ZT_RETAINED_ROUND_CAPACITY;
        }
    }
    if (!g->event_count) { g->event_due=now+250000ULL; return false; }
    zt_gateway_message_t m={.envelope.t=ZT_GATEWAY_EVENTS};
    m.body.events.round_id=g->events[0].id.round_id; m.body.events.count=g->event_count;
    memcpy(m.body.events.events,g->events,g->event_count*sizeof(g->events[0]));
    zt_err_t sent=gw_send(&m);
    if (sent==ZT_OK) g->event_sent_us=now;
    g->event_due=now+250000ULL;
    return sent==ZT_OK;
}
zt_err_t zt_gateway_service(uint64_t now)
{
    gateway_t *g=zt_gw;
    if (!g) return ZT_ERR_INVALID_STATE;
    if (!g->owner) g->owner=xTaskGetCurrentTaskHandle();
    if (g->owner!=xTaskGetCurrentTaskHandle()) return ZT_ERR_INVALID_STATE;
    zt_channel_status_t channel;
    bool online=zt_channel_get_status(&channel)==ZT_OK && channel.has_ip && time(NULL)>=1704067200;
    control_service(false,now); /* Deliver completed button results even after auth/network failure. */
    if (!online) { if (g->ws) gw_ws_destroy(); if (!g->resetting) joins_service(false,now); return ZT_ERR_BUSY; }
    if (g->stopped) { if (g->ws) gw_ws_destroy(); return g->status.last_error; }
    if (g->close_code==ZT_CLOSE_POLICY || g->close_code==ZT_CLOSE_UNKNOWN_GAME) {
        g->status.auth_error=g->close_code==ZT_CLOSE_POLICY;
        g->status.last_error=g->status.auth_error ? ZT_ERR_AUTH : ZT_ERR_NOT_FOUND;
        g->stopped=1; gw_ws_destroy(); return g->status.last_error;
    }
    zt_gateway_status_t link;
    portENTER_CRITICAL(&g->guard); link=g->status; portEXIT_CRITICAL(&g->guard);
    if (g->disconnected_event || g->overflow_event || (g->ws && link.connected && now>=link.last_inbound_us &&
        now-link.last_inbound_us>ZT_GATEWAY_STALE_LINK_MS*1000ULL)) {
        gw_ws_destroy(); g->close_code=0; gw_backoff(now);
    }
    if (g->rx.ready && g->ws) {
        zt_err_t r=zt_gateway_decode(g->rx.bytes,g->rx.message_len,&g->scratch.tokens,&g->message);
        if (r==ZT_OK) r=zt_gateway_bridge_publish(&g->message);
        if (r!=ZT_ERR_BUSY) {
            bool more=r==ZT_OK &&
                ((g->message.envelope.t==ZT_GATEWAY_COMMANDS && g->message.body.commands.count) ||
                 (g->message.envelope.t==ZT_GATEWAY_RECEIPTS && g->message.body.receipts.count) ||
                 (g->message.envelope.t==ZT_GATEWAY_DECISIONS && g->message.body.decisions.count) ||
                 g->message.envelope.t==ZT_GATEWAY_SNAPSHOT);
            gw_ws_release_rx(); g->response_pending=0;
            if (more) next_empty_ack_us=now+100000ULL;
        }
        if (r!=ZT_OK && r!=ZT_ERR_BUSY && r!=ZT_ERR_NOT_IMPLEMENTED && r!=ZT_ERR_STALE) {
            g->status.last_error=r; ++g->rx.dropped_messages;
        }
    }
    commands_service(now);
    /* Keep a backpressured reply immutable until the game can consume it. */
    if (g->rx.ready) return ZT_OK;
    /* Registration HTTPS closes the only WSS session. Let the bounded reset
     * cleanup finish before accepting registration work for the new lobby. */
    if (!g->resetting && !start_waits_for_registration() && control_service(true,now)) return ZT_OK;
    if (!g->resetting && joins_service(true,now)) return ZT_OK;
    if (!g->ws && now>=g->retry_us) {
        if (!g->bootstrapped) {
            zt_bootstrap_t bootstrap;
            zt_err_t r=zt_gateway_bootstrap(&bootstrap);
            if (r!=ZT_OK) {
                if (g->http_retry_us>g->retry_us) g->retry_us=g->http_retry_us;
                gw_backoff(esp_timer_get_time()); return r;
            }
            g->bootstrapped=1;
        }
        zt_gateway_hello_t hello; hello_build(&hello);
        zt_err_t r=zt_gateway_ws_open(&hello);
        if (r!=ZT_OK) { g->status.last_error=r; gw_backoff(esp_timer_get_time()); }
        return r;
    }
    if (!g->status.connected) return ZT_OK;
    if (!g->hello_sent) {
        zt_gateway_message_t m={.envelope.t=ZT_GATEWAY_HELLO}; hello_build(&m.body.hello);
        g->hello_us=esp_timer_get_time();
        zt_err_t r=gw_send(&m); if (r==ZT_OK) g->hello_sent=1;
        return r;
    }
    if (!g->status.welcomed) {
        if (now-g->hello_us>5000000ULL) { gw_ws_destroy(); gw_backoff(now); }
        return ZT_OK;
    }
    if (now<g->send_due || (g->response_pending && now<g->response_due)) return ZT_OK;
    if (now>=g->sync_due && (!g->sync_us || now-g->sync_us>2000000ULL)) {
        zt_gateway_message_t m={.envelope.t=ZT_GATEWAY_TIME_SYNC}; m.body.time_sync.nonce=++g->sync_nonce;
        g->sync_us=esp_timer_get_time();
        if (gw_send(&m)==ZT_OK) g->sync_due=now+ZT_GATEWAY_CLOCK_INTERVAL_MS*1000ULL;
        else g->sync_us=0;
        return ZT_OK;
    }
    if (g->sync_us && now-g->sync_us<2000000ULL) return ZT_OK;
    zt_reset_receipt_t reset;
    bool locally_reset=g->round && zt_store_load_reset(g->round,&reset)==ZT_OK;
    if (zt_game_admission_enabled() && !locally_reset && g->snapshot_pages && g->snapshot_mask!=((1u<<g->snapshot_pages)-1) && now>=g->page_due) {
        uint8_t page=0; while (g->snapshot_mask&(1u<<page)) ++page;
        zt_gateway_message_t m={.envelope.t=ZT_GATEWAY_NEED};
        m.body.need=(zt_gateway_need_t){.round_id=g->round,.wants_snapshot=1,.page_index=page,.snapshot_id=g->snapshot_id};
        if (gw_send(&m)==ZT_OK) g->page_due=now+1000000ULL;
        return ZT_OK;
    }
    if (zt_game_admission_enabled() && g->round && g->start_ms && now>=next_clock_apply_us) {
        zt_checkpoint_t cp; zt_clock_sample_t sample;
        if (clock_sample(now,g->start_ms,&sample) && zt_store_load_checkpoint(g->round,&cp)==ZT_OK && cp.round_id==g->round &&
            zt_clock_apply(g->round,&sample)==ZT_OK) next_clock_apply_us=now+1000000ULL;
    }
    /* Receipts and event uploads share the request budget fairly. A continuous
     * stream of retained receipt retries cannot starve new infection evidence. */
    if (g->events_first && events_service(now)) g->events_first=0;
    else if (acknowledgments_service(now)) g->events_first=1;
    else if (events_service(now)) g->events_first=0;
    return ZT_OK;
}
zt_err_t zt_gateway_mark_applied(zt_round_id_t round_id,uint32_t server_id)
{
    (void)round_id; (void)server_id;
    /* There is no durable whole-outbox admission ledger in this initial adapter.
     * Keeping the reconnect cursor at zero is safe; highest-seen is not. */
    return ZT_ERR_NOT_IMPLEMENTED;
}
