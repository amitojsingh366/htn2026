#include "zt_ui.h"
#include <stdbool.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define STRIPE_ROWS (ZT_LCD_HEIGHT / ZT_LCD_STRIPE_HEIGHT)
#define PEER_VISIBLE_ROWS 8
#define DIAGNOSTIC_PAGES 4
_Static_assert(ZT_LCD_HEIGHT % ZT_LCD_STRIPE_HEIGHT == 0, "whole stripe rows");
_Static_assert(STRIPE_ROWS <= 64, "stripe cache bitmap capacity");

/* Component-private rendering entry point: navigation is local UI state, never
 * fields written back into the immutable gameplay snapshot. */
zt_err_t zt_ui_render_view_stripe(const zt_ui_snapshot_t *snapshot, zt_screen_t screen,
    const zt_rect_t *rect, uint16_t *pixels, size_t capacity_pixels,
    uint8_t peer_offset, uint8_t diagnostic_page, bool settings, const char *banner);

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    zt_ui_snapshot_t latest, frame;
    zt_screen_t screen, frame_screen;
    zt_admission_t admission;
    bool initialized, lcd_ready, led_ready, have_snapshot, rendering, servicing;
    bool frame_started, led_started, message_started, announce_started;
    uint8_t row, pending_dma, brightness;
    uint64_t next_frame_us, next_led_us, message_at_us, announce_at_us;
    zt_feedback_t feedback, last_snapshot_feedback;
    uint64_t feedback_at_us, feedback_until_us, last_snapshot_feedback_until;
    char announcement[ZT_ANNOUNCE_MAX_LEN + 1];
    char last_snapshot_announcement[ZT_ANNOUNCE_MAX_LEN + 1];
    uint64_t announcement_until_us, last_snapshot_announcement_until;
    uint32_t row_hash[STRIPE_ROWS];
    uint64_t valid_rows;
    uint32_t request_id;
    zt_err_t lcd_error;
    uint64_t home_pressed_us;
    uint16_t buttons_down;
    uint8_t peer_offset, diagnostic_page, frame_peer_offset, frame_diagnostic_page;
    bool status_open, settings_open, home_hold_opened, frame_settings, feedback_hidden;
    char banner[ZT_ANNOUNCE_MAX_LEN + 1], frame_banner[ZT_ANNOUNCE_MAX_LEN + 1];
} ui;

static bool printable(const char *value, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        if ((uint8_t)value[i] < ZT_ASCII_PRINTABLE_MIN || (uint8_t)value[i] > ZT_ASCII_PRINTABLE_MAX)
            return false;
    return true;
}

static unsigned priority(zt_feedback_t kind)
{
    switch (kind) {
    case ZT_FEEDBACK_INFECTED: return 4;
    case ZT_FEEDBACK_TAG_CONFIRMED: return 3;
    case ZT_FEEDBACK_UNCONFIRMED: case ZT_FEEDBACK_SYNC_REQUIRED: return 2;
    case ZT_FEEDBACK_GET_CLOSER: case ZT_FEEDBACK_STAY_CLEAR: return 1;
    default: return 0;
    }
}

/* Caller holds lock. Expiration is capped at arrival, never extended by a frame. */
static zt_err_t feedback_locked(zt_feedback_t kind, uint64_t expiry, uint64_t now)
{
    if (kind == ZT_FEEDBACK_NONE) {
        ui.feedback = ZT_FEEDBACK_NONE;
        ui.feedback_until_us = 0;
        return ZT_OK;
    }
    if (expiry <= now) return ZT_ERR_STALE;
    if (ui.feedback_until_us > now && priority(kind) < priority(ui.feedback)) return ZT_ERR_BUSY;
    if (priority(kind) == 1 && ui.message_started &&
        now - ui.message_at_us < ZT_GET_CLOSER_RATE_LIMIT_MS * 1000ULL) return ZT_ERR_BUSY;
    uint64_t duration = 1000000;
    if (kind == ZT_FEEDBACK_INFECTED) duration = ZT_LED_INFECTED_MS * 1000ULL;
    if (kind == ZT_FEEDBACK_TAG_CONFIRMED) duration = ZT_LED_TAG_CONFIRMED_MS * 1000ULL;
    if (kind == ZT_FEEDBACK_UNCONFIRMED) duration = ZT_LED_UNCONFIRMED_MS * 1000ULL;
    ui.feedback = kind;
    ui.feedback_hidden = false;
    ui.feedback_at_us = now;
    ui.feedback_until_us = expiry < now + duration ? expiry : now + duration;
    if (priority(kind) == 1) { ui.message_at_us = now; ui.message_started = true; }
    return ZT_OK;
}

static zt_err_t announce_locked(const char *text, size_t len, uint64_t expiry, uint64_t now)
{
    if (expiry <= now) return ZT_ERR_STALE;
    if (ui.announce_started && now - ui.announce_at_us < ZT_ANNOUNCE_MIN_INTERVAL_MS * 1000ULL)
        return ZT_ERR_BUSY;
    memcpy(ui.announcement,text,len);
    ui.announcement[len] = 0;
    uint64_t ceiling = now + ZT_ANNOUNCE_MAX_DISPLAY_MS * 1000ULL;
    ui.announcement_until_us = expiry < ceiling ? expiry : ceiling;
    ui.announce_at_us = now;
    ui.announce_started = true;
    return ZT_OK;
}

static zt_screen_t admission_screen(zt_admission_t state)
{
    switch (state) {
    case ZT_ADMISSION_LOBBY: case ZT_ADMISSION_REGISTERING:
    case ZT_ADMISSION_WAITING_FOR_ROUND: case ZT_ADMISSION_NEXT_ROUND:
        return ZT_SCREEN_LOBBY;
    case ZT_ADMISSION_PREPARED: return ZT_SCREEN_PREPARED;
    case ZT_ADMISSION_RUNNING: return ZT_SCREEN_RUNNING_RADAR;
    case ZT_ADMISSION_EXPIRED_PENDING_SYNC: case ZT_ADMISSION_FINAL: return ZT_SCREEN_END;
    default: return ZT_SCREEN_SETUP;
    }
}

static void home_hold_locked(uint64_t now)
{
    if ((ui.buttons_down & (1u << ZT_BUTTON_HOME)) && !ui.home_hold_opened &&
        now >= ui.home_pressed_us && now - ui.home_pressed_us >= ZT_HOME_HOLD_MS * 1000ULL) {
        ui.home_hold_opened = true;
        ui.status_open = true;
        ui.settings_open = true;
        ui.diagnostic_page = 0;
    }
}

static unsigned peer_scroll_limit_locked(uint64_t now)
{
    uint64_t elapsed = now > ui.latest.sampled_us ? (now - ui.latest.sampled_us) / 1000 : 0;
    unsigned count = 0;
    for (unsigned i = 0; i < ui.latest.direct_contact_count; ++i)
        if (ui.latest.contacts[i].recent_sample_count &&
            ui.latest.contacts[i].age_ms + elapsed < ZT_PEER_STALE_MS) ++count;
    return count > PEER_VISIBLE_ROWS ? count - PEER_VISIBLE_ROWS : 0;
}

zt_err_t zt_ui_post_button(const zt_button_edge_t *edge)
{
    if (!edge || edge->button < ZT_BUTTON_A || edge->button > ZT_BUTTON_START ||
        edge->kind < ZT_EDGE_RELEASE || edge->kind > ZT_EDGE_REPEAT) return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    /* A's gameplay meaning belongs solely to the game task. Aux1 is maintained
     * hardware state, not navigation. No combinations are interpreted here. */
    if (edge->button == ZT_BUTTON_A || edge->button == ZT_BUTTON_AUX1) return ZT_OK;
    bool direction = edge->button >= ZT_BUTTON_DOWN && edge->button <= ZT_BUTTON_UP;
    if (edge->kind == ZT_EDGE_REPEAT && !direction) return ZT_OK;
    uint16_t bit = 1u << edge->button;
    portENTER_CRITICAL(&lock);
    if (edge->kind == ZT_EDGE_RELEASE) {
        /* Account for a full hold even if both edges arrived between UI ticks. */
        if (edge->button == ZT_BUTTON_HOME) home_hold_locked(edge->at_us);
        ui.buttons_down &= ~bit;
        portEXIT_CRITICAL(&lock);
        return ZT_OK;
    }
    if ((edge->kind == ZT_EDGE_PRESS && (ui.buttons_down & bit)) ||
        (edge->kind == ZT_EDGE_REPEAT && !(ui.buttons_down & bit))) {
        portEXIT_CRITICAL(&lock);
        return ZT_OK;
    }
    ui.buttons_down |= bit;
    switch (edge->button) {
    case ZT_BUTTON_HOME:
        ui.status_open = ui.settings_open = false;
        ui.feedback_hidden = true;
        ui.announcement_until_us = 0;
        ui.home_pressed_us = edge->at_us;
        ui.home_hold_opened = false;
        break;
    case ZT_BUTTON_START:
        ui.status_open = !ui.status_open;
        ui.settings_open = false;
        break;
    case ZT_BUTTON_B:
        if (ui.admission == ZT_ADMISSION_RUNNING && ui.latest.countdown_ms <= 0) {
            ui.screen = ui.screen == ZT_SCREEN_PEER_LIST ? ZT_SCREEN_RUNNING_RADAR : ZT_SCREEN_PEER_LIST;
            ui.status_open = ui.settings_open = false;
        }
        break;
    case ZT_BUTTON_LEFT: case ZT_BUTTON_RIGHT:
        ui.status_open = true;
        ui.diagnostic_page = (ui.diagnostic_page +
            (edge->button == ZT_BUTTON_RIGHT ? 1 : DIAGNOSTIC_PAGES - 1)) % DIAGNOSTIC_PAGES;
        break;
    case ZT_BUTTON_UP: case ZT_BUTTON_DOWN:
        if (ui.settings_open) {
            /* DOWN dims one step; UP cannot undo a lowered cap. */
            if (edge->button == ZT_BUTTON_DOWN && ui.brightness) --ui.brightness;
        } else if (!ui.status_open && ui.screen == ZT_SCREEN_PEER_LIST) {
            unsigned limit = peer_scroll_limit_locked(edge->at_us);
            if (ui.peer_offset > limit) ui.peer_offset = limit;
            if (edge->button == ZT_BUTTON_UP && ui.peer_offset) --ui.peer_offset;
            else if (edge->button == ZT_BUTTON_DOWN && ui.peer_offset < limit) ++ui.peer_offset;
        }
        break;
    default: break;
    }
    /* HAL supplies the real 400/150 ms repeats; UI never synthesizes repeats. */
    portEXIT_CRITICAL(&lock);
    return ZT_OK;
}

static void lcd_done(uint32_t request_id, uint8_t index, zt_err_t result, void *context)
{
    (void)request_id; (void)index; (void)context;
    portENTER_CRITICAL(&lock);
    if (ui.pending_dma) --ui.pending_dma;
    if (result != ZT_OK) {
        ui.valid_rows = 0;
        ui.lcd_error = result;
    }
    portEXIT_CRITICAL(&lock);
}

zt_err_t zt_ui_init(void)
{
    if (ui.initialized) return ZT_ERR_INVALID_STATE;
    zt_err_t result;
    if (!ui.lcd_ready) {
        result = zt_lcd_init(lcd_done,NULL);
        if (result != ZT_OK) return result;
        ui.lcd_ready = true;
    }
    if (!ui.led_ready) {
        result = zt_leds_init();
        if (result != ZT_OK) return result;
        ui.led_ready = true;
    }
    ui.brightness = ZT_LED_COMPONENT_CAP;
    ui.screen = ZT_SCREEN_SETUP;
    ui.initialized = true;
    return ZT_OK;
}

zt_err_t zt_ui_submit_snapshot(const zt_ui_snapshot_t *snapshot)
{
    if (!snapshot || snapshot->direct_contact_count > ZT_MAX_PLAYERS ||
        snapshot->roster_count > ZT_MAX_PLAYERS || snapshot->ready_count > ZT_MAX_PLAYERS ||
        snapshot->admission > ZT_ADMISSION_FINAL ||
        snapshot->feedback > ZT_FEEDBACK_SYNC_REQUIRED) return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    uint64_t now = esp_timer_get_time();
    size_t announcement_len = strnlen(snapshot->announcement, sizeof(snapshot->announcement));
    if (announcement_len > ZT_ANNOUNCE_MAX_LEN ||
        !printable(snapshot->announcement,announcement_len)) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lock);
    if (!ui.have_snapshot || snapshot->admission != ui.admission) {
        ui.admission = snapshot->admission;
        ui.screen = admission_screen(snapshot->admission);
        ui.peer_offset = 0;
    }
    if (snapshot->feedback != ui.last_snapshot_feedback ||
        snapshot->feedback_expires_us != ui.last_snapshot_feedback_until) {
        if (snapshot->feedback != ZT_FEEDBACK_NONE)
            (void)feedback_locked(snapshot->feedback,snapshot->feedback_expires_us,now);
        ui.last_snapshot_feedback = snapshot->feedback;
        ui.last_snapshot_feedback_until = snapshot->feedback_expires_us;
    }
    if (snapshot->announcement_expires_us != ui.last_snapshot_announcement_until ||
        strncmp(snapshot->announcement,ui.last_snapshot_announcement,ZT_ANNOUNCE_MAX_LEN)) {
        zt_err_t result=announcement_len ? announce_locked(snapshot->announcement,announcement_len,snapshot->announcement_expires_us,now) : ZT_OK;
        /* The game and display run on separate task ticks. Retry the coalesced
         * snapshot if the display's own 15-second gate is still finishing. */
        if (result!=ZT_ERR_BUSY) {
            memcpy(ui.last_snapshot_announcement,snapshot->announcement,announcement_len);
            ui.last_snapshot_announcement[announcement_len] = 0;
            ui.last_snapshot_announcement_until = snapshot->announcement_expires_us;
        }
    }
    /* One coalescing mailbox and one immutable frame copy, no snapshot queue. */
    ui.latest = *snapshot;
    ui.latest.name[ZT_NAME_MAX_LEN] = 0;
    ui.latest.build_id[ZT_BUILD_ID_LEN] = 0;
    ui.latest.selected_name[ZT_NAME_MAX_LEN] = 0;
    for (unsigned i = 0; i < snapshot->direct_contact_count; ++i)
        ui.latest.contacts[i].name[ZT_NAME_MAX_LEN] = 0;
    if (snapshot->brightness_cap < ui.brightness) ui.brightness = snapshot->brightness_cap;
    ui.have_snapshot = true;
    portEXIT_CRITICAL(&lock);
    return ZT_OK;
}

zt_err_t zt_ui_select_screen(zt_screen_t screen)
{
    if (screen < ZT_SCREEN_SETUP || screen > ZT_SCREEN_END) return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);
    ui.status_open = screen == ZT_SCREEN_STATUS;
    ui.settings_open = false;
    if (!ui.status_open) ui.screen = screen;
    portEXIT_CRITICAL(&lock);
    return ZT_OK;
}

zt_err_t zt_ui_overlay(const zt_ui_overlay_t *overlay)
{
    if (!overlay || overlay->kind < ZT_FEEDBACK_NONE || overlay->kind > ZT_FEEDBACK_SYNC_REQUIRED)
        return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    zt_err_t result = feedback_locked(overlay->kind,overlay->expires_us,now);
    portEXIT_CRITICAL(&lock);
    return result;
}

zt_err_t zt_ui_announce(const char *text, size_t len, uint64_t expires_us)
{
    if (!text || len < ZT_ANNOUNCE_MIN_LEN || len > ZT_ANNOUNCE_MAX_LEN || !printable(text,len))
        return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    uint64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    zt_err_t result = announce_locked(text,len,expires_us,now);
    portEXIT_CRITICAL(&lock);
    return result;
}

zt_err_t zt_ui_set_banner(const char *text, size_t len)
{
    portENTER_CRITICAL(&lock);
    zt_err_t result = ZT_OK;
    if (!ui.initialized) result = ZT_ERR_INVALID_STATE;
    else if (!text || !len) ui.banner[0] = 0;
    else if (len > ZT_ANNOUNCE_MAX_LEN || !printable(text,len)) result = ZT_ERR_INVALID_ARG;
    else {
        memcpy(ui.banner,text,len);
        ui.banner[len] = 0;
    }
    portEXIT_CRITICAL(&lock);
    return result;
}

zt_err_t zt_ui_set_brightness_cap(uint8_t cap)
{
    if (cap > ZT_LED_COMPONENT_CAP) return ZT_ERR_INVALID_ARG;
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);
    zt_err_t result = cap <= ui.brightness ? ZT_OK : ZT_ERR_INVALID_ARG;
    if (result == ZT_OK) ui.brightness = cap;
    portEXIT_CRITICAL(&lock);
    return result;
}

static zt_range_tier_t peer_tier(const zt_peer_entry_t *p)
{
    if (p->filtered_rssi_q8 >= ZT_TIER_IN_RANGE_DBM * 256) return ZT_RANGE_IN_RANGE;
    if (p->filtered_rssi_q8 >= ZT_TIER_CLOSE_DBM * 256) return ZT_RANGE_CLOSE;
    if (p->filtered_rssi_q8 >= ZT_TIER_NEARBY_DBM * 256) return ZT_RANGE_NEARBY;
    return ZT_RANGE_FAR;
}

static zt_err_t leds(uint64_t now)
{
    zt_pixel_t pixels[ZT_LED_COUNT] = {0};
    static const uint8_t danger_order[] = {
        ZT_LED_MIDDLE_LEFT, ZT_LED_MIDDLE_RIGHT, ZT_LED_UPPER_LEFT, ZT_LED_UPPER_RIGHT,
    };
    static const uint16_t period_ms[] = {1600,1600,1200,800,400};
    portENTER_CRITICAL(&lock);
    uint8_t cap = ui.brightness;
    zt_feedback_t feedback = ui.feedback_until_us > now ? ui.feedback : ZT_FEEDBACK_NONE;
    uint64_t elapsed = now >= ui.feedback_at_us ? now - ui.feedback_at_us : 0;
    zt_role_t role = ui.latest.role;
    zt_range_tier_t closest = ZT_RANGE_UNKNOWN;
    if (ui.latest.admission == ZT_ADMISSION_RUNNING && ui.latest.countdown_ms <= 0) {
        for (unsigned i = 0; i < ui.latest.direct_contact_count; ++i) {
            const zt_peer_entry_t *p = &ui.latest.contacts[i];
            uint64_t age = p->age_ms + (now > ui.latest.sampled_us ? (now-ui.latest.sampled_us)/1000 : 0);
            bool opposite = (role == ZT_ROLE_HUMAN && p->role == ZT_ROLE_ZOMBIE) ||
                            (role == ZT_ROLE_ZOMBIE && p->role == ZT_ROLE_HUMAN);
            if (age < ZT_PEER_STALE_MS && p->recent_sample_count && opposite && peer_tier(p) > closest)
                closest = peer_tier(p);
        }
    }
    portEXIT_CRITICAL(&lock);
    if (feedback == ZT_FEEDBACK_TAG_CONFIRMED) {
        pixels[(elapsed / 100000) % ZT_LED_COUNT].g = 20;
    } else if (feedback == ZT_FEEDBACK_INFECTED) {
        uint8_t strength = (elapsed / 150000) % 2 ? 3 : 20;
        for (unsigned i = 0; i < ZT_LED_COUNT; ++i) pixels[i].r = strength;
    } else if (feedback == ZT_FEEDBACK_UNCONFIRMED) {
        for (unsigned i = 0; i < ZT_LED_COUNT; ++i) pixels[i].r = 4;
    } else {
        /* All six pixels show the role steadily; the bottom pair always keeps
         * that identity when the upper four warn about an approaching opponent.
         * Yellow shares the same 20-component total as one solid role pixel,
         * so proximity never increases the passive pattern's current budget. */
        zt_pixel_t idle = role == ZT_ROLE_HUMAN ? (zt_pixel_t){0,0,20} :
                          role == ZT_ROLE_ZOMBIE ? (zt_pixel_t){20,0,0} : (zt_pixel_t){0,0,0};
        for (unsigned i = 0; i < ZT_LED_COUNT; ++i) pixels[i] = idle;
        if (closest != ZT_RANGE_UNKNOWN) {
            unsigned period = period_ms[closest];
            unsigned phase = (now / 1000) % period;
            unsigned triangle = phase < period/2 ? phase : period-phase;
            uint8_t strength = 3 + triangle * 7 / (period/2);
            unsigned count = closest == ZT_RANGE_IN_RANGE ? 4 : closest == ZT_RANGE_CLOSE ? 3 :
                             closest == ZT_RANGE_NEARBY ? 2 : 1;
            for (unsigned i = 0; i < count; ++i)
                pixels[danger_order[i]] = (zt_pixel_t){strength,strength,0};
        }
    }
    return zt_leds_submit(pixels,ZT_LED_COUNT,cap);
}

static void prepare_frame(uint64_t now)
{
    /* Caller holds lock. Never alter the producer's snapshot or gameplay state. */
    ui.frame = ui.latest;
    /* Freeze banner updates with the frame; later setters cannot tear stripes.
     * Home, snapshots and announcement expiry never clear persistent status. */
    memcpy(ui.frame_banner,ui.banner,sizeof(ui.frame_banner));
    ui.frame_screen = ui.screen;
    ui.frame.status_overlay = ui.status_open;
    ui.frame_settings = ui.settings_open;
    ui.frame_diagnostic_page = ui.diagnostic_page;
    unsigned scroll_limit = peer_scroll_limit_locked(now);
    if (ui.peer_offset > scroll_limit) ui.peer_offset = scroll_limit;
    ui.frame_peer_offset = ui.peer_offset;
    uint64_t elapsed_ms = now > ui.frame.sampled_us ? (now-ui.frame.sampled_us)/1000 : 0;
    /* Scheduled RUNNING already carries assigned roles, but the round timer
     * begins only after its shared countdown has elapsed. */
    uint64_t countdown_ms = ui.frame.countdown_ms > 0 ? (uint32_t)ui.frame.countdown_ms : 0;
    uint64_t playing_ms = elapsed_ms > countdown_ms ? elapsed_ms - countdown_ms : 0;
    if (ui.frame.admission == ZT_ADMISSION_RUNNING)
        ui.frame.remaining_ms = playing_ms >= ui.frame.remaining_ms ? 0 : ui.frame.remaining_ms - playing_ms;
    if (ui.frame.remaining_ms > ZT_ROUND_DURATION_MS) ui.frame.remaining_ms = ZT_ROUND_DURATION_MS;
    if (ui.frame.countdown_ms > 0)
        ui.frame.countdown_ms = elapsed_ms >= (uint32_t)ui.frame.countdown_ms ? 0 : ui.frame.countdown_ms - elapsed_ms;
    for (unsigned i = 0; i < ui.frame.direct_contact_count; ++i) {
        uint64_t age = ui.frame.contacts[i].age_ms + elapsed_ms;
        ui.frame.contacts[i].age_ms = age > UINT32_MAX ? UINT32_MAX : age;
    }
    uint64_t age = ui.frame.host_age_ms + elapsed_ms;
    ui.frame.host_age_ms = age > UINT32_MAX ? UINT32_MAX : age;
    age = ui.frame.server_age_ms + elapsed_ms;
    ui.frame.server_age_ms = age > UINT32_MAX ? UINT32_MAX : age;
    ui.frame.feedback = ui.feedback;
    ui.frame.feedback_expires_us = ui.feedback_hidden ? 0 : ui.feedback_until_us;
    memcpy(ui.frame.announcement,ui.announcement,sizeof(ui.announcement));
    ui.frame.announcement_expires_us = ui.announcement_until_us;
    ui.frame.brightness_cap = ui.brightness;
    ui.frame.sampled_us = now;
    ui.row = 0;
    ui.rendering = true;
    ui.frame_started = true;
    ui.next_frame_us = now + ZT_UI_FRAME_MIN_MS * 1000ULL;
}

zt_err_t zt_ui_service(uint64_t now_us)
{
    if (!ui.initialized) return ZT_ERR_INVALID_STATE;
    portENTER_CRITICAL(&lock);
    if (ui.servicing) { portEXIT_CRITICAL(&lock); return ZT_ERR_BUSY; }
    if (!ui.have_snapshot) { portEXIT_CRITICAL(&lock); return ZT_OK; }
    ui.servicing = true;
    home_hold_locked(now_us);
    portEXIT_CRITICAL(&lock);
    /* Acquire also polls completions. Do it even between frames, so the final
     * stripe completes (or faults) without requiring another frame to start. */
    uint8_t index;
    uint16_t *pixels;
    size_t capacity;
    zt_err_t acquire_result = zt_lcd_acquire(&index,&pixels,&capacity);
    portENTER_CRITICAL(&lock);
    bool update_leds = !ui.led_started || now_us >= ui.next_led_us;
    if (update_leds) {
        ui.led_started = true;
        ui.next_led_us = now_us + ZT_LED_FRAME_MIN_MS * 1000ULL;
    }
    if (!ui.rendering && !ui.pending_dma && (!ui.frame_started || now_us >= ui.next_frame_us))
        prepare_frame(now_us);
    bool render = ui.rendering;
    zt_err_t result = ui.lcd_error;
    ui.lcd_error = ZT_OK;
    portEXIT_CRITICAL(&lock);
    if (update_leds) {
        zt_err_t err = leds(now_us);
        if (err != ZT_OK && err != ZT_ERR_BUSY) result = err;
        if (err != ZT_ERR_BUSY) {
            /* Match the HAL's post-refresh deadline, avoiding every second
             * 40 ms frame being rejected for arriving during the RMT tail. */
            uint64_t next_led_us = (uint64_t)esp_timer_get_time() + ZT_LED_FRAME_MIN_MS * 1000ULL;
            portENTER_CRITICAL(&lock);
            ui.next_led_us = next_led_us;
            portEXIT_CRITICAL(&lock);
        }
    }
    /* At most one stripe per service call. The integration task wakes on
     * work/ticks and yields between calls; no in-service wait for a free stripe. */
    if (render) {
        zt_err_t err = acquire_result;
        if (err == ZT_OK) {
            unsigned row = ui.row;
            zt_rect_t rect = {0,row*ZT_LCD_STRIPE_HEIGHT,ZT_LCD_WIDTH,(row+1)*ZT_LCD_STRIPE_HEIGHT};
            err = zt_ui_render_view_stripe(&ui.frame,ui.frame_screen,&rect,pixels,capacity,
                ui.frame_peer_offset,ui.frame_diagnostic_page,ui.frame_settings,ui.frame_banner);
            uint32_t hash = 2166136261u;
            if (err == ZT_OK)
                for (size_t i = 0; i < ZT_LCD_STRIPE_PIXELS; ++i) hash = (hash ^ pixels[i]) * 16777619u;
            portENTER_CRITICAL(&lock);
            bool changed = !(ui.valid_rows & (UINT64_C(1) << row)) || hash != ui.row_hash[row];
            portEXIT_CRITICAL(&lock);
            if (err == ZT_OK && changed) {
                zt_lcd_work_t work = {rect,pixels,ZT_LCD_STRIPE_PIXELS,index,++ui.request_id};
                portENTER_CRITICAL(&lock);
                ++ui.pending_dma;
                /* Publish cache before submit: completion may run immediately. */
                ui.row_hash[row] = hash;
                ui.valid_rows |= UINT64_C(1) << row;
                portEXIT_CRITICAL(&lock);
                err = zt_lcd_submit(&work);
                if (err != ZT_OK) {
                    portENTER_CRITICAL(&lock);
                    --ui.pending_dma;
                    ui.valid_rows &= ~(UINT64_C(1) << row);
                    portEXIT_CRITICAL(&lock);
                    (void)zt_lcd_release(index);
                }
            } else (void)zt_lcd_release(index);
            if (err == ZT_OK) {
                if (++ui.row == STRIPE_ROWS) ui.rendering = false;
            } else result = err;
        } else if (err != ZT_ERR_BUSY) result = err;
    } else if (acquire_result == ZT_OK) (void)zt_lcd_release(index);
    if (acquire_result != ZT_OK && acquire_result != ZT_ERR_BUSY) result = acquire_result;
    portENTER_CRITICAL(&lock);
    ui.servicing = false;
    portEXIT_CRITICAL(&lock);
    return result;
}
