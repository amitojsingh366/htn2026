#include "zt_demo.h"
#include "zt_game.h"
#include "zt_ui.h"

#include <stdbool.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/* start/service belong exclusively to the game task. Only the small published
 * activity value is shared with other tasks. No staging state survives reboot. */
typedef enum { DEMO_IDLE, DEMO_PAGES, DEMO_ADMISSION, DEMO_READY,
               DEMO_MONITOR, DEMO_HALTED } demo_stage_t;
static struct {
    demo_stage_t stage;
    zt_server_snapshot_t page;
    zt_wire_roster_entry_t roster[ZT_MAX_PLAYERS];
    zt_server_command_t prepare, start;
    zt_slot_t self_slot;
    bool banner_set, start_posted;
    zt_err_t failure;
} demo;
static portMUX_TYPE activity_guard = portMUX_INITIALIZER_UNLOCKED;
static uint8_t activity;
static uint64_t deadline_us;

_Static_assert(ZT_DEMO_DEFAULT_DURATION_MS == ZT_ROUND_DURATION_MS,
               "demo duration must pass game validation");
_Static_assert(sizeof(ZT_DEMO_BANNER) - 1 <= ZT_ANNOUNCE_MAX_LEN,
               "demo banner must fit the UI");

static void publish_activity(uint8_t active, uint64_t deadline)
{
    portENTER_CRITICAL(&activity_guard);
    activity = active;
    deadline_us = deadline;
    portEXIT_CRITICAL(&activity_guard);
}

zt_err_t zt_demo_active(uint8_t *out)
{
    if (!out) return ZT_ERR_INVALID_ARG;
    portENTER_CRITICAL(&activity_guard);
    uint8_t active = activity;
    uint64_t deadline = deadline_us;
    portEXIT_CRITICAL(&activity_guard);
    *out = active && (uint64_t)esp_timer_get_time() < deadline;
    return ZT_OK;
}

static zt_err_t clear_banner(void)
{
    if (!demo.banner_set) return ZT_OK;
    zt_err_t err = zt_ui_set_banner(NULL, 0);
    if (err == ZT_OK) demo.banner_set = false;
    return err;
}

static zt_err_t finish(void)
{
    demo.stage = DEMO_IDLE;
    publish_activity(0, 0);
    return clear_banner();
}

zt_err_t zt_demo_start(const zt_demo_round_t *round)
{
    if (!round || !round->round_id ||
        round->player_count < ZT_DEMO_MIN_PLAYERS || round->player_count > ZT_MAX_PLAYERS ||
        round->patient_zero_slot >= round->player_count ||
        round->channel < 1 || round->channel > 11 ||
        (round->duration_ms && round->duration_ms != ZT_DEMO_DEFAULT_DURATION_MS))
        return ZT_ERR_INVALID_ARG;
    const zt_mac_t zero = {0};
    for (unsigned i = 0; i < round->player_count; ++i) {
        if (!memcmp(&round->players[i], &zero, sizeof(zero))) return ZT_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; ++j)
            if (!memcmp(&round->players[i], &round->players[j], sizeof(zero)))
                return ZT_ERR_INVALID_ARG;
    }

    zt_ui_snapshot_t view;
    zt_err_t err = zt_game_snapshot(&view);
    if (err != ZT_OK) return err;
    if (!view.host_configured || !view.host_selected) return ZT_ERR_AUTH;
    if ((view.admission != ZT_ADMISSION_LOBBY && view.admission != ZT_ADMISSION_NEXT_ROUND) ||
        view.round_id || view.error == ZT_ERROR_STORAGE || demo.stage != DEMO_IDLE)
        return ZT_ERR_INVALID_STATE;
    zt_slot_t self = ZT_SLOT_INVALID;
    /* game_init obtains self_mac from esp_read_mac(..., ESP_MAC_WIFI_STA). */
    for (unsigned i = 0; i < round->player_count; ++i)
        if (!memcmp(&round->players[i], &view.self_mac, sizeof(view.self_mac))) self = (zt_slot_t)i;
    if (self == ZT_SLOT_INVALID) return ZT_ERR_NOT_FOUND;
    if (view.registered && view.self_slot != self) return ZT_ERR_INVALID_STATE;

    err = clear_banner();
    if (err != ZT_OK) return err;
    memset(&demo, 0, sizeof(demo));
    demo.self_slot = self;
    for (unsigned i = 0; i < round->player_count; ++i) {
        zt_wire_roster_entry_t *entry = &demo.roster[i];
        entry->slot = (uint8_t)i;
        entry->mac = round->players[i];
        entry->name_len = 3;
        entry->name[0] = 'P';
        entry->name[1] = (uint8_t)('0' + i / 10);
        entry->name[2] = (uint8_t)('0' + i % 10);
    }
    uint64_t hash;
    err = zt_wire_roster_hash(demo.roster, round->player_count, &hash);
    if (err != ZT_OK) return err;
    zt_wire_prepare_round_args_t rules = {
        .snapshot_rev = 1, .roster_hash = hash, .roster_count = round->player_count,
        .duration_ms = ZT_DEMO_DEFAULT_DURATION_MS, .channel = round->channel,
        .tag_rssi = ZT_TAG_RSSI_DEFAULT_DBM, .tag_cooldown_ms = ZT_TAG_COOLDOWN_MS
    };
    /* LOBBY admission is deliberate: only PREPARE freezes the round, and only
     * START sets roles/timing. Zero start/end fields mean not yet scheduled. */
    demo.page = (zt_server_snapshot_t){
        .round_id = round->round_id, .server_id = 1, .snapshot_id = 1, .state_rev = 1,
        .roster_hash = hash,
        .page_count = (uint8_t)((round->player_count + ZT_ROSTER_PAGE_ENTRIES - 1) / ZT_ROSTER_PAGE_ENTRIES),
        .phase = ZT_PHASE_LOBBY, .patient_zero_slot = ZT_SLOT_INVALID,
        .round_channel = round->channel, .rules = rules
    };
    demo.prepare = (zt_server_command_t){
        .round_id = round->round_id, .server_id = 2,
        .command = {.command_seq = 1, .kind = ZT_CMD_PREPARE_ROUND,
                    .target_slot = ZT_SLOT_ALL, .valid_until_elapsed_ms = ZT_COMMAND_NO_EXPIRY}
    };
    size_t written;
    err = zt_wire_encode_prepare_round_args(&rules, demo.prepare.command.args,
                                           sizeof(demo.prepare.command.args), &written);
    if (err != ZT_OK) return err;
    demo.prepare.command.args_len = (uint8_t)written;
    uint32_t delay = round->start_delay_ms;
    if (delay < ZT_DEMO_START_DELAY_MIN_MS) delay = ZT_DEMO_START_DELAY_MIN_MS;
    if (delay > ZT_DEMO_START_DELAY_MAX_MS) delay = ZT_DEMO_START_DELAY_MAX_MS;
    zt_wire_start_round_args_t start = {
        .snapshot_rev = 1, .roster_hash = hash, .patient_zero_slot = round->patient_zero_slot,
        .initial_role_rev = 1, .sampled_elapsed_ms = -(int32_t)delay,
        .duration_ms = ZT_DEMO_DEFAULT_DURATION_MS, .uncertainty_ms = 0
    };
    demo.start = (zt_server_command_t){
        .round_id = round->round_id, .server_id = 3,
        .command = {.command_seq = 2, .kind = ZT_CMD_START_ROUND,
                    .target_slot = ZT_SLOT_ALL, .valid_until_elapsed_ms = ZT_COMMAND_NO_EXPIRY}
    };
    err = zt_wire_encode_start_round_args(&start, demo.start.command.args,
                                         sizeof(demo.start.command.args), &written);
    if (err != ZT_OK) return err;
    demo.start.command.args_len = (uint8_t)written;
    /* Establish the persistent banner before any page can reach the game. */
    err = zt_ui_set_banner(ZT_DEMO_BANNER, sizeof(ZT_DEMO_BANNER) - 1);
    if (err != ZT_OK) return err;
    demo.banner_set = true;
    demo.stage = DEMO_PAGES;
    publish_activity(1, UINT64_MAX); /* No deadline exists until START applies. */
    return ZT_OK;
}

zt_err_t zt_demo_service(uint64_t now_us)
{
    if (demo.stage == DEMO_IDLE) return clear_banner();
    zt_ui_snapshot_t view;
    zt_err_t err = zt_game_snapshot(&view);
    if (err != ZT_OK) return err;
    if (view.round_id && view.round_id != demo.page.round_id) {
        /* A competing authoritative round wins; never send another demo item. */
        err = finish();
        return err == ZT_OK ? ZT_ERR_INVALID_STATE : err;
    }
    if (view.round_id == demo.page.round_id) {
        if (view.phase >= ZT_PHASE_EXPIRED_PENDING_SYNC || view.result_final) return finish();
        if (view.phase == ZT_PHASE_RUNNING) demo.stage = DEMO_MONITOR;
        if (demo.start_posted || view.phase == ZT_PHASE_RUNNING) {
            zt_clock_sample_t clock;
            if (zt_clock_read(now_us, &clock) == ZT_OK) {
                if (clock.elapsed_ms >= (int32_t)ZT_DEMO_DEFAULT_DURATION_MS) return finish();
                /* Learn the game's deadline, including any shortening. Never
                 * initialize or adjust its clock, nor extend our activity. */
                uint64_t left = (uint64_t)((int64_t)ZT_DEMO_DEFAULT_DURATION_MS - clock.elapsed_ms) * 1000ULL;
                uint64_t deadline = now_us > UINT64_MAX - left ? UINT64_MAX : now_us + left;
                portENTER_CRITICAL(&activity_guard);
                if (deadline < deadline_us) deadline_us = deadline;
                portEXIT_CRITICAL(&activity_guard);
            }
        }
    }
    uint8_t active;
    zt_demo_active(&active);
    if (!active) return finish();
    if (view.error == ZT_ERROR_STORAGE) return ZT_ERR_STORAGE;
    if (!view.host_configured || !view.host_selected) return ZT_ERR_AUTH;
    if (demo.stage == DEMO_MONITOR) return ZT_OK;
    if (demo.stage == DEMO_HALTED) return demo.failure;

    /* At most one post per call. ZT_OK transfers custody to game/G02, which
     * owns persistence and delivery retries. Only BUSY retains our item. */
    if (demo.stage == DEMO_PAGES) {
        unsigned base = demo.page.page_index * ZT_ROSTER_PAGE_ENTRIES;
        unsigned count = demo.page.rules.roster_count - base;
        if (count > ZT_ROSTER_PAGE_ENTRIES) count = ZT_ROSTER_PAGE_ENTRIES;
        memset(demo.page.roster, 0, sizeof(demo.page.roster));
        memset(demo.page.roles, 0, sizeof(demo.page.roles));
        demo.page.entry_count = (uint8_t)count;
        for (unsigned i = 0; i < count; ++i) {
            demo.page.roster[i] = demo.roster[base + i];
            demo.page.roles[i] = (zt_wire_role_entry_t){
                .slot = (uint8_t)(base + i), .role = ZT_ROLE_HUMAN, .role_rev = 0,
                .cause_slot = ZT_SLOT_INVALID
            };
        }
        err = zt_game_post_snapshot(&demo.page);
        if (err == ZT_OK && ++demo.page.page_index == demo.page.page_count)
            demo.stage = DEMO_ADMISSION;
    } else {
        /* The published snapshot follows durable admission/PREPARE completion.
         * Do not queue a time sample behind an unresolved persistence request. */
        if (view.round_id != demo.page.round_id || !view.registered) return ZT_OK;
        if (view.self_slot != demo.self_slot || view.roster_count != demo.page.rules.roster_count)
            err = ZT_ERR_CONFLICT;
        else if (demo.stage == DEMO_ADMISSION) {
            if (view.phase == ZT_PHASE_PREPARED) {
                demo.stage = DEMO_READY; /* Already applied; never re-issue. */
                return ZT_OK;
            }
            err = zt_game_post_command(&demo.prepare);
            if (err == ZT_OK) demo.stage = DEMO_READY;
        } else {
            if (view.phase != ZT_PHASE_PREPARED || view.ready_count != view.roster_count)
                return ZT_OK;
            err = zt_game_post_command(&demo.start);
            if (err == ZT_OK) {
                demo.start_posted = true;
                demo.stage = DEMO_MONITOR;
            }
        }
    }
    if (err != ZT_OK && err != ZT_ERR_BUSY) {
        /* No cancellation or replacement: an accepted earlier item may still
         * be committing. Keep the banner and refuse another local start. */
        demo.stage = DEMO_HALTED;
        demo.failure = err;
    }
    return err;
}
