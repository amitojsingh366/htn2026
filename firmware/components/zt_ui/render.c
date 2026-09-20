#include "zt_ui.h"
#include "zt_gateway.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BG 0x0842
#define FG 0xef7d
#define MUTED 0x8c71
#define GRID 0x21a7
#define BLUE 0x453f
#define GREEN 0x67ed
#define RED 0xfacb
#define YELLOW 0xefe8

typedef struct { zt_rect_t rect; uint16_t *pixels; int stride; } canvas_t;

static void fill(canvas_t *c, int x, int y, int width, int height, uint16_t color)
{
    int x1 = x + width, y1 = y + height;
    if (x < c->rect.x0) x = c->rect.x0;
    if (y < c->rect.y0) y = c->rect.y0;
    if (x1 > c->rect.x1) x1 = c->rect.x1;
    if (y1 > c->rect.y1) y1 = c->rect.y1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > ZT_LCD_WIDTH) x1 = ZT_LCD_WIDTH;
    if (y1 > ZT_LCD_HEIGHT) y1 = ZT_LCD_HEIGHT;
    for (int row = y; row < y1; ++row)
        for (int col = x; col < x1; ++col)
            c->pixels[(row - c->rect.y0) * c->stride + col - c->rect.x0] = color;
}

static void text(canvas_t *c, int x, int y, const char *s, size_t limit, int scale, uint16_t color)
{
    if (y + 7 * scale <= c->rect.y0 || y >= c->rect.y1) return;
    for (size_t i = 0; i < limit && s[i] && x < ZT_LCD_WIDTH; ++i) {
        uint8_t bits[5], width, height;
        size_t written;
        uint8_t ch = (uint8_t)s[i];
        /* The one non-ASCII product label uses a drawn middle dot. */
        if (ch == 0xc2 && i + 1 < limit && (uint8_t)s[i + 1] == 0xb7) {
            fill(c, x + 2 * scale, y + 3 * scale, scale, scale, color);
            ++i;
        } else {
            if (ch < 32 || ch > 126) ch = '?';
            if (zt_ui_font_glyph(ch, bits, sizeof(bits), &width, &height, &written) == ZT_OK)
                for (unsigned col = 0; col < width; ++col)
                    for (unsigned row = 0; row < height; ++row)
                        if (bits[col] & (1u << row))
                            fill(c, x + col * scale, y + row * scale, scale, scale, color);
        }
        x += 6 * scale;
    }
}

static void line_value(canvas_t *c, int y, const char *label, uint32_t value)
{
    char number[16];
    snprintf(number, sizeof(number), "%lu", (unsigned long)value);
    text(c, 12, y, label, 32, 2, MUTED);
    int scale = strlen(number) > 7 ? 1 : 2;
    text(c, 228, y + (scale == 1 ? 4 : 0), number, sizeof(number), scale, FG);
}

static const char *role_name(zt_role_t role)
{
    return role == ZT_ROLE_ZOMBIE ? "ZOMBIE" : role == ZT_ROLE_HUMAN ? "HUMAN" : "UNKNOWN";
}

static uint16_t role_color(zt_role_t role)
{
    return role == ZT_ROLE_ZOMBIE ? RED : role == ZT_ROLE_HUMAN ? BLUE : MUTED;
}

static zt_range_tier_t tier(const zt_peer_entry_t *peer)
{
    int rssi = peer->filtered_rssi_q8;
    if (rssi >= ZT_TIER_IN_RANGE_DBM * 256) return ZT_RANGE_IN_RANGE;
    if (rssi >= ZT_TIER_CLOSE_DBM * 256) return ZT_RANGE_CLOSE;
    if (rssi >= ZT_TIER_NEARBY_DBM * 256) return ZT_RANGE_NEARBY;
    return ZT_RANGE_FAR;
}

static const char *tier_name(zt_range_tier_t value)
{
    switch (value) {
    case ZT_RANGE_IN_RANGE: return "IN RANGE";
    case ZT_RANGE_CLOSE: return "CLOSE";
    case ZT_RANGE_NEARBY: return "NEARBY";
    case ZT_RANGE_FAR: return "FAR";
    default: return "--";
    }
}

/* contacts[] is the frozen contract's direct-observation table. There is no
 * roster, relayed-event, host-presence or inferred contact rendering path. */
static bool fresh(const zt_peer_entry_t *p)
{
    return p->age_ms < ZT_PEER_STALE_MS && p->recent_sample_count > 0;
}

static unsigned sorted_contacts(const zt_ui_snapshot_t *s, uint8_t order[ZT_MAX_PLAYERS])
{
    unsigned count = 0;
    for (unsigned i = 0; i < s->direct_contact_count && i < ZT_MAX_PLAYERS; ++i) {
        if (!fresh(&s->contacts[i])) continue;
        unsigned j = count;
        while (j && (s->contacts[order[j - 1]].filtered_rssi_q8 < s->contacts[i].filtered_rssi_q8 ||
               (s->contacts[order[j - 1]].filtered_rssi_q8 == s->contacts[i].filtered_rssi_q8 &&
                s->contacts[order[j - 1]].slot > s->contacts[i].slot))) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
        ++count;
    }
    return count;
}

static void circle(canvas_t *c, int cx, int cy, int radius)
{
    if (cy + radius < c->rect.y0 || cy - radius >= c->rect.y1) return;
    int x = radius, y = 0, error = 1 - radius;
    while (x >= y) {
        fill(c,cx+x,cy+y,1,1,GRID); fill(c,cx+y,cy+x,1,1,GRID);
        fill(c,cx-y,cy+x,1,1,GRID); fill(c,cx-x,cy+y,1,1,GRID);
        fill(c,cx-x,cy-y,1,1,GRID); fill(c,cx-y,cy-x,1,1,GRID);
        fill(c,cx+y,cy-x,1,1,GRID); fill(c,cx+x,cy-y,1,1,GRID);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
}

static void filled_ring(canvas_t *c,int cx,int cy,int outer,int inner,uint16_t color)
{
    int top=cy-outer, bottom=cy+outer+1;
    if (top<c->rect.y0) top=c->rect.y0;
    if (bottom>c->rect.y1) bottom=c->rect.y1;
    int left=cx-outer, right=cx+outer+1;
    if (left<c->rect.x0) left=c->rect.x0;
    if (right>c->rect.x1) right=c->rect.x1;
    for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        int dx=x-cx, dy=y-cy, distance=dx*dx+dy*dy;
        if (distance<=outer*outer && (inner<0 || distance>inner*inner))
            fill(c,x,y,1,1,color);
    }
}

static void links(canvas_t *c, const zt_ui_snapshot_t *s, int x, int y, int scale)
{
    bool self_host = s->host_selected && s->host_configured;
    text(c,x,y,self_host ? "YOU ARE HOST" : s->host_connected ? "HOST ONLINE" : "HOST OFFLINE",16,scale,
         self_host || s->host_connected ? GREEN : MUTED);
    text(c,x,y+10*scale,s->server_connected ? "LIVE SYNC ONLINE" : "LIVE SYNC WAIT",16,scale,
         s->server_connected ? GREEN : YELLOW);
}

static const char *host_connection_detail(const zt_ui_snapshot_t *s)
{
    if (s->diagnostics.gateway_activity == ZT_GATEWAY_ACTIVITY_REGISTER) return "REGISTERING BADGE";
    if (s->diagnostics.gateway_activity == ZT_GATEWAY_ACTIVITY_BOOTSTRAP) return "SYNCING WITH SERVER";
    if (!s->diagnostics.wifi_has_ip) return "WAITING FOR HOTSPOT";
    uint32_t error = s->diagnostics.gateway_error;
    uint32_t http = s->diagnostics.gateway_http_status;
    if (s->server_connected && error == ZT_OK) return "SERVER READY";
    if (error == ZT_OK && s->diagnostics.gateway_http_age_ms < ZT_GATEWAY_STALE_LINK_MS)
        return "SERVER RECONNECTING";
    if (error == ZT_ERR_AUTH || http == 401 || http == 403) return "HOST AUTH ERROR";
    if (http == 429 || http >= 500 || error == ZT_ERR_BUSY) return "SERVER BUSY";
    if (http == 404) return "GAME NOT FOUND";
    if (http == 409 || error == ZT_ERR_CONFLICT) return "REGISTRATION CONFLICT";
    if (error == ZT_ERR_NETWORK) return "NETWORK ERROR";
    if (error == ZT_ERR_TIMEOUT) return "SERVER TIMEOUT";
    if (error == ZT_ERR_PROTOCOL || error == ZT_ERR_INVALID_LENGTH ||
        error == ZT_ERR_UNSUPPORTED_VERSION || error == ZT_ERR_INVALID_ARG ||
        (http >= 400 && http < 500)) return "RESPONSE ERROR";
    if (error == ZT_ERR_STORAGE) return "STORAGE ERROR";
    if (error == ZT_ERR_RADIO) return "RADIO ERROR";
    if (error == ZT_ERR_NO_SPACE) return "SERVER LINK FULL";
    if (error != ZT_OK) return "SERVER LINK ERROR";
    return "CONNECTING TO SERVER";
}
static bool host_control_needs_update(const zt_ui_snapshot_t *s)
{
    return s->host_control_error == ZT_ERR_NOT_IMPLEMENTED ||
        s->host_control_error == ZT_ERR_UNSUPPORTED_VERSION;
}
static const char *host_control_detail(const zt_ui_snapshot_t *s)
{
    switch (s->host_control_error) {
    case ZT_ERR_NOT_IMPLEMENTED: case ZT_ERR_UNSUPPORTED_VERSION: return "ASK OPERATOR TO UPDATE SERVER";
    case ZT_ERR_AUTH: return "HOST AUTH ERROR";
    case ZT_ERR_HOST_REGISTRATION: return "HOST REGISTRATION CHANGED";
    case ZT_ERR_ROSTER_SIZE: return "REGISTER AT LEAST TWO BADGES";
    case ZT_ERR_ROUND_ACTIVE: return "WAIT FOR THE ROUND TO FINISH";
    case ZT_ERR_INVALID_STATE: return "SERVER NOT READY";
    case ZT_ERR_CONFLICT: case ZT_ERR_STALE: return "ROUND CHANGED - SYNC AGAIN";
    case ZT_ERR_NOT_FOUND: return "SERVER CONTROL UNAVAILABLE";
    case ZT_ERR_INVALID_ARG: case ZT_ERR_PROTOCOL: return "SERVER REJECTED REQUEST";
    case ZT_ERR_TIMEOUT: return "SERVER TIMEOUT";
    case ZT_ERR_NETWORK: return "NETWORK ERROR";
    case ZT_ERR_BUSY: case ZT_ERR_NO_SPACE: return "SERVER BUSY";
    default: return "CONTROL REQUEST FAILED";
    }
}

static void countdown(canvas_t *c, const zt_ui_snapshot_t *s)
{
    const char *role=role_name(s->role);
    text(c,106,12,"YOUR ROLE",12,2,MUTED);
    text(c,(ZT_LCD_WIDTH-(int)strlen(role)*24)/2,40,role,8,4,role_color(s->role));
    text(c,76,86,"GAME STARTS IN",16,2,FG);
    char seconds[16];
    snprintf(seconds,sizeof(seconds),"%lu",(unsigned long)(((uint32_t)s->countdown_ms+999)/1000));
    text(c,(ZT_LCD_WIDTH-(int)strlen(seconds)*48)/2,113,seconds,sizeof(seconds),8,YELLOW);
    text(c,97,178,"TAGGING STARTS AT ZERO",24,1,MUTED);
    links(c,s,12,194,1);
    text(c,230,211,"START: STATUS",16,1,MUTED);
}

static void radar(canvas_t *c, const zt_ui_snapshot_t *s)
{
    /* Exactly the left 160x180 area, with no directional axis or bearing marks. */
    fill(c,0,8,ZT_RADAR_WIDTH,ZT_RADAR_HEIGHT,0x0863);
    uint8_t order[ZT_MAX_PLAYERS];
    unsigned count = sorted_contacts(s,order);
    const zt_peer_entry_t *nearest = NULL;
    /* RSSI supplies a proximity tier, never a bearing. Each closer tier adds
     * the next inner ring; all outer rings remain filled. */
    const zt_peer_entry_t *closest=count ? &s->contacts[order[0]] : NULL;
    zt_range_tier_t proximity=closest ? tier(closest) : ZT_RANGE_UNKNOWN;
    uint16_t color=closest && closest->role==ZT_ROLE_ZOMBIE ? RED : BLUE;
    for (unsigned ring=0;ring<4;++ring) {
        int outer=68-(int)ring*17, inner=outer-15;
        if ((unsigned)proximity>ring) filled_ring(c,80,98,outer,ring==3 ? -1 : inner,color);
        else circle(c,80,98,outer);
    }
    text(c,56,176,tier_name(proximity),12,1,closest ? YELLOW : MUTED);
    for (unsigned j = 0; j < count; ++j) {
        const zt_peer_entry_t *p = &s->contacts[order[j]];
        /* Display the game's selection, so the name cannot disagree with A's
         * target. Require the selection still has a fresh direct observation. */
        if (p->slot == s->selected_target && p->eligible &&
            p->age_ms <= ZT_TAG_SOURCE_MAX_AGE_MS) nearest = p;
    }
    text(c,8,192,ZT_RADAR_LABEL,sizeof(ZT_RADAR_LABEL),1,MUTED);
    text(c,168,12,role_name(s->role),8,3,role_color(s->role));
    char value[32];
    uint32_t remaining = s->remaining_ms > ZT_ROUND_DURATION_MS ? ZT_ROUND_DURATION_MS : s->remaining_ms;
    uint32_t seconds = (remaining + 999u) / 1000u;
    snprintf(value,sizeof(value),"%02lu:%02lu",(unsigned long)(seconds/60),(unsigned long)(seconds%60));
    text(c,168,44,value,sizeof(value),4,FG);
    text(c,168,82,"NEAREST ELIGIBLE",20,1,MUTED);
    text(c,168,98,nearest ? nearest->name : "NONE",ZT_NAME_MAX_LEN,2,FG);
    text(c,168,121,nearest ? tier_name(tier(nearest)) : "--",12,2,YELLOW);
    if (s->role == ZT_ROLE_ZOMBIE) {
        text(c,168,141,"A: INFECT",16,2,nearest ? GREEN : YELLOW);
        text(c,168,159,nearest ? "IN-RANGE HUMAN REQUIRED" : "GET CLOSE TO A HUMAN",24,1,MUTED);
    } else {
        snprintf(value,sizeof(value),"DIRECT CONTACTS %u",count);
        text(c,168,148,value,sizeof(value),1,MUTED);
    }
    links(c,s,168,175,1);
    text(c,168,195,"B: LIST  START: STATUS",24,1,MUTED);
}

static void status(canvas_t *c, const zt_ui_snapshot_t *s, uint8_t page, bool settings)
{
    uint8_t order[ZT_MAX_PLAYERS];
    unsigned direct_count = sorted_contacts(s,order);
    text(c,12,10,settings ? "SETTINGS" : "STATUS",12,3,FG);
    char heading[24];
    snprintf(heading,sizeof(heading),"PAGE %u/4",(unsigned)page + 1);
    text(c,224,10,heading,sizeof(heading),1,MUTED);
    snprintf(heading,sizeof(heading),"LED CAP %u",s->brightness_cap);
    text(c,224,25,heading,sizeof(heading),1,YELLOW);
    if (page == 0) {
        line_value(c,42,"CHANNEL",s->channel);
        line_value(c,64,"ROLE REV",s->role_rev);
        line_value(c,86,"PENDING EVENTS",s->pending_event_count);
        line_value(c,108,"DIRECT PEERS",direct_count);
        line_value(c,130,"HOST AGE S",s->host_age_ms/1000);
        line_value(c,152,s->host_selected && s->host_configured ? "SERVER RX AGE S" : "HOST REPORT AGE S",s->server_age_ms/1000);
        line_value(c,174,"LED CAP /24",s->brightness_cap);
    } else if (page == 1) {
        /* Only explicitly approved numerical diagnostics can reach this page. */
        line_value(c,42,"RX DROPS",s->diagnostics.rx_drops);
        line_value(c,64,"TX DROPS",s->diagnostics.tx_drops);
        line_value(c,86,"INPUT DROPS",s->diagnostics.input_drops);
        line_value(c,108,"RX HIGH WATER",s->diagnostics.rx_high_water);
        line_value(c,130,"TX HIGH WATER",s->diagnostics.tx_high_water);
        line_value(c,152,"REPLAY BACKLOG",s->diagnostics.replay_backlog);
        line_value(c,174,"CLOCK MS",s->diagnostics.clock_uncertainty_ms);
    } else if (page == 2) {
        if (s->host_selected && s->host_configured) {
            line_value(c,42,"HTTP STATUS",s->diagnostics.gateway_http_status);
            line_value(c,64,"GATEWAY ERROR",s->diagnostics.gateway_error);
            line_value(c,86,"WIFI HAS IP",s->diagnostics.wifi_has_ip);
            line_value(c,108,"GATEWAY INIT",s->diagnostics.gateway_initialized);
        } else {
            line_value(c,42,"INVALID FRAMES",s->diagnostics.invalid_frames);
            line_value(c,64,"AUTH FAILURES",s->diagnostics.auth_failures);
            line_value(c,86,"DEDUPE HITS",s->diagnostics.dedupe_hits);
            line_value(c,108,"TX WATCHDOGS",s->diagnostics.tx_watchdogs);
        }
        line_value(c,130,"RADIO RESTARTS",s->diagnostics.radio_restarts);
        line_value(c,152,"GATEWAY DROPS",s->diagnostics.gateway_drops);
        line_value(c,174,"GW RECONNECTS",s->diagnostics.gateway_reconnects);
    } else {
        line_value(c,42,"EVENT HIGH WATER",s->diagnostics.event_high_water);
        line_value(c,64,"REPLAY BACKLOG",s->diagnostics.replay_backlog);
        line_value(c,86,"FREE HEAP",s->diagnostics.free_heap);
        line_value(c,108,"MINIMUM HEAP",s->diagnostics.minimum_heap);
        line_value(c,130,"LARGEST BLOCK",s->diagnostics.largest_free_block);
        line_value(c,152,"CLOCK MS",s->diagnostics.clock_uncertainty_ms);
        line_value(c,174,"LED CAP /24",s->brightness_cap);
    }
    text(c,12,194,"AA OR USB POWER. USB DOES NOT RECHARGE AA.",48,1,YELLOW);
    text(c,12,211,settings ? "DOWN: DIM  LEFT/RIGHT: PAGE  HOME: CLOSE" :
         "LEFT/RIGHT: PAGE  HOLD HOME: SETTINGS",48,1,MUTED);
}

static void peers(canvas_t *c, const zt_ui_snapshot_t *s, uint8_t offset)
{
    text(c,12,10,"DIRECT PEERS",20,3,FG);
    uint8_t order[ZT_MAX_PLAYERS];
    unsigned count = sorted_contacts(s,order);
    if (!count) text(c,12,62,"NO FRESH CONTACTS",25,2,MUTED);
    unsigned limit = count > 8 ? count - 8 : 0;
    if (offset > limit) offset = limit;
    for (unsigned i = 0; offset + i < count && i < 8; ++i) {
        const zt_peer_entry_t *p = &s->contacts[order[offset + i]];
        int y = 42 + i * 20;
        text(c,12,y,p->name,ZT_NAME_MAX_LEN,2,FG);
        text(c,165,y,role_name(p->role),8,1,role_color(p->role));
        text(c,230,y,tier_name(tier(p)),12,1,YELLOW);
        fill(c,12,y+16,296,1,GRID);
    }
    char value[64];
    unsigned end = count < offset + 8u ? count : offset + 8u;
    if (s->role == ZT_ROLE_ZOMBIE)
        text(c,12,201,"A: INFECT NEAREST IN-RANGE HUMAN",40,1,YELLOW);
    snprintf(value,sizeof(value),"%u-%u/%u  UP/DOWN: SCROLL  B: RADAR",count ? (unsigned)offset + 1 : 0u,end,count);
    text(c,12,211,value,sizeof(value),1,MUTED);
}

static void mode_banner(canvas_t *c, const zt_ui_snapshot_t *s, const char *banner)
{
    /* All screens leave this footer free. Announcements/errors/feedback own
     * y=202..226, entirely above it. The Aux1 notice owns the footer if present. */
    if (!banner || !banner[0] || s->aux1_changed_live ||
        c->rect.y1 <= 228 || c->rect.y0 >= ZT_LCD_HEIGHT) return;
    size_t len = strnlen(banner,ZT_ANNOUNCE_MAX_LEN);
    fill(c,0,228,ZT_LCD_WIDTH,12,BG);
    fill(c,0,228,ZT_LCD_WIDTH,1,GRID);
    text(c,8,230,"MODE:",5,1,MUTED);
    /* Keep the complete value in UI storage, but visibly abbreviate long lines
     * instead of wrapping into the higher-priority announcement region. */
    /* Reserve the right edge for the firmware version on every page. */
    text(c,44,230,banner,len > 36 ? 33 : len,1,YELLOW);
    if (len > 36) text(c,44 + 33 * 6,230,"...",3,1,YELLOW);
}

static void overlays(canvas_t *c, const zt_ui_snapshot_t *s)
{
    const char *message = NULL;
    uint16_t color = YELLOW;
    if (s->error == ZT_ERROR_STORAGE) message = "STORAGE ERROR";
    else if (s->error == ZT_ERROR_RADIO) message = "RADIO ERROR";
    else if (s->error == ZT_ERROR_HOST_AUTH) message = "HOST AUTH ERROR";
    else if (s->feedback_expires_us > s->sampled_us) {
        switch (s->feedback) {
        case ZT_FEEDBACK_GET_CLOSER: message = "GET CLOSER"; break;
        case ZT_FEEDBACK_STAY_CLEAR: message = "STAY CLEAR"; break;
        case ZT_FEEDBACK_TAG_CONFIRMED: message = "TAG CONFIRMED"; color = GREEN; break;
        case ZT_FEEDBACK_INFECTED: message = "INFECTED"; color = RED; break;
        case ZT_FEEDBACK_UNCONFIRMED: message = "UNCONFIRMED"; color = RED; break;
        case ZT_FEEDBACK_SYNC_REQUIRED: message = "SYNC REQUIRED"; break;
        default: break;
        }
    }
    if (message) {
        fill(c,0,202,320,25,GRID);
        text(c,12,207,message,24,2,color);
    } else if (s->announcement[0] && s->announcement_expires_us > s->sampled_us) {
        fill(c,0,202,320,25,GRID);
        text(c,10,205,s->announcement,50,1,FG);
        size_t len = strnlen(s->announcement,ZT_ANNOUNCE_MAX_LEN);
        if (len > 50) text(c,10,216,s->announcement+50,len-50,1,FG);
    }
    if (s->aux1_changed_live) {
        fill(c,0,228,320,12,GRID);
        text(c,7,230,"HOST SWITCH CHANGED - APPLIES ON REBOOT",48,1,YELLOW);
    }
}

/* Private companion to ui.c, with navigation frozen for the entire frame. */
zt_err_t zt_ui_render_view_stripe(const zt_ui_snapshot_t *s, zt_screen_t screen,
    const zt_rect_t *rect, uint16_t *pixels, size_t capacity_pixels,
    uint8_t peer_offset, uint8_t diagnostic_page, bool settings, const char *banner)
{
    if (!s || !rect || !pixels || screen < ZT_SCREEN_SETUP || screen > ZT_SCREEN_END)
        return ZT_ERR_INVALID_ARG;
    int width = (int)rect->x1-rect->x0, height = (int)rect->y1-rect->y0;
    if (width <= 0 || height <= 0 || height > ZT_LCD_STRIPE_HEIGHT ||
        (size_t)width*height > ZT_LCD_STRIPE_PIXELS) return ZT_ERR_INVALID_LENGTH;
    size_t count = (size_t)width*height;
    if (capacity_pixels < count) return ZT_ERR_NO_SPACE;
    for (size_t i = 0; i < count; ++i) pixels[i] = BG;
    canvas_t c = {*rect,pixels,width};
    char value[64];
    if (s->status_overlay) screen = ZT_SCREEN_STATUS;
    switch (screen) {
    case ZT_SCREEN_SETUP: {
        /* Startup and saved-round recovery share this screen with provisioning. */
        const char *state = s->admission == ZT_ADMISSION_NEEDS_CONFIG ? "CONFIGURE OVER USB" :
            s->admission == ZT_ADMISSION_REJOINING ? "REJOINING ROUND" :
            s->admission == ZT_ADMISSION_RECOVERING ? "RECOVERING SAVED GAME" :
            s->admission == ZT_ADMISSION_WAIT_INSTALL ? "WAITING FOR INSTALL" : "STARTING";
        text(&c,12,16,"ZOMBIE TAG",20,3,FG);
        snprintf(value,sizeof(value),"BADGE %02X%02X%02X",s->self_mac.bytes[3],s->self_mac.bytes[4],s->self_mac.bytes[5]);
        text(&c,12,62,value,sizeof(value),2,MUTED);
        text(&c,12,92,"BUILD",12,2,MUTED);
        text(&c,90,92,s->build_id,ZT_BUILD_ID_LEN,2,FG);
        text(&c,12,127,state,24,2,FG);
        if (s->host_selected && !s->host_configured) {
            text(&c,12,165,"HOST NOT CONFIGURED",30,2,YELLOW);
            text(&c,12,188,"ASK THE OPERATOR TO CONFIGURE HOST",40,1,MUTED);
        } else if (s->admission == ZT_ADMISSION_REJOINING)
            text(&c,12,165,s->host_selected ? "WAITING FOR SERVER SYNC" : "WAITING FOR HOST SYNC",30,2,YELLOW);
        break;
    }
    case ZT_SCREEN_LOBBY: {
        text(&c,12,12,"LOBBY",16,3,FG);
        text(&c,12,48,s->name,ZT_NAME_MAX_LEN,3,BLUE);
        const char *state = s->host_control_pending && s->host_control == ZT_HOST_CONTROL_START ? "STARTING GAME" :
            s->host_control == ZT_HOST_CONTROL_START && host_control_needs_update(s) ? "SERVER UPDATE NEEDED" :
            s->host_control == ZT_HOST_CONTROL_START && s->host_control_error != ZT_OK ? "START FAILED - B: RETRY" :
            s->host_can_start ? "B: START GAME" :
            s->admission == ZT_ADMISSION_NEXT_ROUND ? "NEXT ROUND" :
            s->admission == ZT_ADMISSION_REGISTERING ? "REGISTERING" :
            s->registered ? "WAITING FOR ROUND" : "A: REGISTER";
        text(&c,12,84,state,26,2,YELLOW);
        if (s->roster_count)
            snprintf(value,sizeof(value),"ROUND ROSTER %u / %u",s->roster_count,ZT_MAX_PLAYERS);
        else
            snprintf(value,sizeof(value),"ROUND ROSTER PENDING");
        text(&c,12,116,value,sizeof(value),2,MUTED);
        if (s->registered) {
            snprintf(value,sizeof(value),"SLOT %u",s->self_slot);
            text(&c,12,142,value,sizeof(value),2,MUTED);
        }
        links(&c,s,12,170,1);
        if (s->host_selected && s->host_configured)
            text(&c,12,194,s->host_control_error != ZT_OK ? host_control_detail(s) : host_connection_detail(s),40,1,
                 s->host_control_error == ZT_OK && s->server_connected && s->diagnostics.gateway_error == ZT_OK ? GREEN : YELLOW);
        break;
    }
    case ZT_SCREEN_PREPARED:
        text(&c,12,12,"GET READY",20,3,FG);
        snprintf(value,sizeof(value),"READY %u / %u",s->ready_count,s->roster_count);
        text(&c,12,52,value,sizeof(value),2,GREEN);
        text(&c,12,90,"WAITING FOR BADGES",24,2,YELLOW);
        snprintf(value,sizeof(value),"CHANNEL %u",s->channel);
        text(&c,12,130,value,sizeof(value),2,MUTED);
        text(&c,12,158,"ROLES AFTER EVERYONE IS READY",36,1,MUTED);
        links(&c,s,12,184,1);
        break;
    case ZT_SCREEN_RUNNING_RADAR:
        if (s->countdown_ms>0) countdown(&c,s); else radar(&c,s);
        break;
    case ZT_SCREEN_PEER_LIST:
        if (s->countdown_ms>0) countdown(&c,s); else peers(&c,s,peer_offset);
        break;
    case ZT_SCREEN_STATUS: status(&c,s,diagnostic_page,settings); break;
    case ZT_SCREEN_END:
        if (s->result_present && s->winner != ZT_ROLE_UNKNOWN) {
            snprintf(value,sizeof(value),"%s WIN",s->winner == ZT_ROLE_ZOMBIE ? "ZOMBIES" : "HUMANS");
            text(&c,12,14,value,sizeof(value),4,role_color(s->winner));
            text(&c,12,91,s->winner == ZT_ROLE_ZOMBIE ? "ALL HUMANS INFECTED" : "SURVIVED THE ROUND",25,2,FG);
        } else {
            text(&c,12,14,"GAME OVER",20,4,FG);
            text(&c,12,91,"AWAITING RESULT",25,2,MUTED);
        }
        text(&c,12,59,s->result_final ? "FINAL RESULT" : "SYNCING RESULT",30,2,YELLOW);
        line_value(&c,131,"PENDING EVENTS",s->pending_event_count);
        if (s->host_control_error != ZT_OK && s->host_control == ZT_HOST_CONTROL_RESET)
            text(&c,12,157,host_control_detail(s),40,1,YELLOW);
        else if (s->result_final && !s->result_complete)
            text(&c,12,157,"SOME BADGES HAVE NOT SYNCED",40,1,YELLOW);
        if (s->host_control_pending && s->host_control == ZT_HOST_CONTROL_RESET)
            text(&c,12,177,"RESETTING GAME",26,2,YELLOW);
        else if (s->host_can_reset)
            text(&c,12,177,host_control_needs_update(s) ? "SERVER UPDATE NEEDED" :
                 s->host_control_error != ZT_OK ? "RESET FAILED - B: RETRY" : "B: RESET GAME",26,2,YELLOW);
        else if (!s->server_connected || s->pending_event_count || !s->result_final)
            text(&c,12,179,s->host_selected && s->host_configured ? "WAITING FOR SERVER TO SYNC" : "RECONNECT TO HOST TO SYNC",40,1,FG);
        else
            text(&c,12,179,"WAIT FOR HOST TO RESET",40,1,MUTED);
        text(&c,12,195,"START: STATUS",20,1,MUTED);
        break;
    }
    mode_banner(&c,s,banner);
    overlays(&c,s);
    text(&c,ZT_LCD_WIDTH-ZT_BUILD_ID_LEN*6-6,230,s->build_id,ZT_BUILD_ID_LEN,1,MUTED);
    return ZT_OK;
}

zt_err_t zt_ui_render_stripe(const zt_ui_snapshot_t *s, zt_screen_t screen,
                           const zt_rect_t *rect, uint16_t *pixels, size_t capacity_pixels)
{
    return zt_ui_render_view_stripe(s,screen,rect,pixels,capacity_pixels,
                                   0,s && s->diagnostic_mode ? 1 : 0,false,NULL);
}
