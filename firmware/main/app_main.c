/* Zombie Tag integration entry point. Orchestrator-owned.
 *
 * This file wires modules together and creates plan.md 7's tasks with their exact
 * priorities and stacks. It owns no gameplay state, no wire format and no storage
 * policy: every decision belongs to the module that owns it. Substantive missing
 * behaviour goes back to a worker packet rather than growing here.
 *
 * Ownership notes that matter for anyone reading this later:
 *  - zt_game_init() performs the whole boot sequence of plan 4.1 (inspect storage,
 *    open it, forget the previous game, load configuration, choose the admission
 *    state). It owns the persistence sink. This file therefore does not open the
 *    store, and must not: a second zt_store_open() is an error by contract.
 *  - Button edges fan to BOTH the game, which owns their gameplay meaning, and the
 *    UI, which owns navigation. That is contract amendment 2 and it does not weaken
 *    the single-writer rule; the UI still never mutates gameplay state.
 *  - Radio RX copies go to the mesh, which authenticates and parses, and only
 *    validated domain messages reach the game. No callback mutates gameplay state.
 *  - Only the designated host starts the gateway task; game and radio callbacks
 *    enqueue work without doing network I/O.
 */
#include <string.h>
#include "zt_common.h"
#include "zt_hal.h"
#include "zt_ui.h"
#include "zt_radio.h"
#include "zt_game.h"
#include "zt_gateway.h"
#include "zt_store.h"
#include "zt_console.h"
#include "zt_demo.h"
#include "zt_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <time.h>
#include "mbedtls/sha256.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "zt_gateway_private.h"
#include "zt_device_private.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "zombie_tag";

/* plan.md 7. ESP-IDF stacks are byte units. Single core: no affinity. */
#define TASK_INPUT_PRIORITY 8
#define TASK_INPUT_STACK 3072
#define TASK_RADIO_PRIORITY 7
/* plan.md 7 budgets 6144 for radio_mesh and game, and states those are implementation
 * ceilings rather than measured results. Measured on hardware they are not enough: the
 * game task overflowed by 12 bytes transmitting a BEACON, through
 * transmit -> zt_wire_encode_beacon -> zt_wire_encode_envelope -> zt_wire_hmac ->
 * mbedtls SHA-256 -> esp_sha_acquire_hardware. Both tasks run that same HMAC path on
 * every transmission, so both get headroom. Record the actual high-water marks on the
 * first host badge, as plan.md 7 requires, and tighten these against real numbers. */
#define TASK_RADIO_STACK 8192
#define TASK_GAME_PRIORITY 6
#define TASK_GAME_STACK 8192
#define TASK_PERSIST_PRIORITY 5
#define TASK_PERSIST_STACK 4096
#define TASK_GATEWAY_PRIORITY 4
#define TASK_GATEWAY_STACK 12288
#define TASK_UI_PRIORITY 3
#define TASK_UI_STACK 4096
#define TASK_CONSOLE_PRIORITY 2
#define TASK_CONSOLE_STACK 4096

static uint8_t radio_started;
static uint8_t radio_host;

/* ---- DEMO SELF-PROVISIONING ------------------------------------------------
 *
 * Set ZT_SELF_PROVISION to 0 for the real operator flow, where the guarded tool
 * performs install-init and provision over USB and the installation header carries a
 * tool-verified baseline hash and a tool-issued installation UUID.
 *
 * With it set to 1 the badge provisions itself on first boot from the constants below,
 * so flashing is the only step. This is for the proof of concept, on badges whose owner
 * accepts it, and it is a real deviation worth stating plainly:
 *
 *  - The installation header's baseline hash is NOT a verified image hash. Firmware
 *    cannot know the archive's padded factory hash. It is a deterministic marker derived
 *    from a fixed string and this badge's MAC, chosen so it is obviously not a flash
 *    hash. A badge installed this way will not pass `commission`, by design.
 *  - The installation UUID is likewise derived locally, not issued by the tool.
 *  - The mesh key here is a build-time constant compiled into every badge. It is not a
 *    provisioned secret, and this image must not be given to participants.
 *
 * Everything else is unchanged: the tail is still verified blank first, NVS is still
 * initialised before the raw header is written LAST, and no stock region is touched. */
/* SPLIT 2026-09-19 after the guarded tool refused a flash with
 * INSTALLATION_BASELINE_MISMATCH.
 *
 * Self-INSTALL was a mistake and is now off. layout.installation_header() requires the
 * header's baseline to equal the archive's original 4 MiB SHA-256 and its UUID to be one
 * the tool actually issued. Firmware knows neither, and it should not: that header is the
 * tool's evidence that THIS badge was installed against THAT verified backup, which is
 * exactly what makes a later flash or restore trustworthy. A firmware-written header is
 * an unverifiable claim, so the tool rightly refuses to write over it.
 *
 * Self-CONFIG stays on. Configuration is not evidence about the badge's history: it is
 * game settings, it is validated by the game on load, and writing it locally removes the
 * `provision` step and the private session file without weakening any gate.
 *
 * So the flow is: flash, one `install` from the menu, then the badge configures itself
 * and reaches LOBBY. */
#define ZT_SELF_INSTALL 0
#define ZT_SELF_CONFIG 1
#define ZT_SELF_GAME_ID UINT64_C(0x5A544D454D4F01)
#define ZT_SELF_CHANNEL 6
/* The designated host: the badge whose MAC this is becomes host when its side switch is
 * on. Every other badge running the same image is an ordinary player. */
static const uint8_t ZT_SELF_HOST_MAC[6] = {0x28,0x84,0x85,0xd1,0xaf,0x74};
static const uint8_t ZT_SELF_KEY[ZT_HMAC_KEY_BYTES] = ZT_PRIVATE_MESH_KEY;

/* Deterministic, MAC-bound, and clearly not an image hash. */
static void self_marker(const zt_mac_t *mac, const char *purpose, uint8_t *out32)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)purpose, strlen(purpose));
    mbedtls_sha256_update(&ctx, mac->bytes, sizeof(mac->bytes));
    mbedtls_sha256_finish(&ctx, out32);
    mbedtls_sha256_free(&ctx);
}

static zt_err_t self_install(const zt_mac_t *mac)
{
    uint8_t uuid[32], baseline[32];
    self_marker(mac, "ZT-DEMO-SELF-PROVISION-UUID", uuid);
    self_marker(mac, "ZT-DEMO-SELF-PROVISION-NOT-A-FLASH-HASH", baseline);
    zt_install_header_t header = {
        .schema = 1, .length = ZT_INSTALL_BYTES, .mac = *mac, .reserved = 0,
        .nvs_offset = ZT_NVS_OFFSET, .nvs_length = ZT_NVS_LENGTH,
        .flags = ZT_INSTALL_FLAG_INITIALIZED,
    };
    memcpy(header.magic, ZT_INSTALL_MAGIC, ZT_INSTALL_MAGIC_BYTES);
    memcpy(header.installation_uuid, uuid, ZT_INSTALL_UUID_BYTES);
    memcpy(header.baseline_sha256, baseline, ZT_INSTALL_BASELINE_HASH_BYTES);
    zt_err_t result = zt_store_install_init(&header);
    ESP_LOGW(TAG, "self install-init %d", (int)result);
    return result;
}

static void self_config(const zt_mac_t *mac, zt_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->expected_mac = *mac;
    snprintf(out->name, sizeof(out->name), "B%02X%02X",
             mac->bytes[4], mac->bytes[5]);
    out->game_id = ZT_SELF_GAME_ID;
    memcpy(out->group_key, ZT_SELF_KEY, sizeof(out->group_key));
    memcpy(out->host_mac.bytes, ZT_SELF_HOST_MAC, sizeof(out->host_mac.bytes));
    out->last_channel = ZT_SELF_CHANNEL;
    /* Only the designated badge carries host fields. The build reads the backend
     * credential from an ignored private header, never a tracked source literal. */
    if (!memcmp(mac->bytes, ZT_SELF_HOST_MAC, 6)) {
        /* The designated host joins the configured iPhone hotspot. Radio startup
         * waits for the physical host switch to finish debouncing. */
        out->host_credentials_present = 1;
        snprintf(out->ssid, sizeof(out->ssid), "%s", ZT_PRIVATE_WIFI_SSID);
        snprintf(out->password, sizeof(out->password), "%s", ZT_PRIVATE_WIFI_PASSWORD);
        snprintf(out->https_base, sizeof(out->https_base), "https://htn2026-backend.amitoj.workers.dev");
        snprintf(out->token, sizeof(out->token), "%s", ZT_PRIVATE_GATEWAY_TOKEN);
    }
    out->provisioned_time_ms = 0;
}

/* Console-originated work that must run in the game task's context. The console
 * callback only stages it: a callback never performs a durable write, never makes
 * an admission decision and never mutates gameplay state. */
static portMUX_TYPE staging_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t pending;          /* 0 none, 1 demo, 2 configure */
    uint32_t console_id;
    zt_demo_round_t demo;
    zt_config_t config;
} staged;

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* ---- sinks ---------------------------------------------------------------- */

/* Input task context. Fanning here keeps both consumers' queues bounded and lets
 * each refuse independently; neither refusal may block the other. */
static zt_err_t on_button(const zt_button_edge_t *edge, void *context)
{
    (void)context;
    zt_err_t game = zt_game_post_button(edge);
    zt_err_t ui = zt_ui_post_button(edge);
    return game != ZT_OK ? game : ui;
}

/* Radio owner task. Copies already happened; hand the frame to the mesh. */
static zt_err_t on_rx_frame(const zt_rx_frame_t *frame, void *context)
{
    (void)context;
    return zt_mesh_receive(frame);
}

static void on_tx_complete(const zt_tx_completion_t *completion, void *context)
{
    (void)completion;
    (void)context;
}

/* Mesh task, validated domain only. The game copies what it keeps. */
static zt_err_t on_domain_message(const zt_domain_message_t *message, void *context)
{
    (void)context;
    return zt_game_post_domain(message);
}

/* Game task. The snapshot is an immutable published value; the UI owns its copy. */
static zt_err_t on_snapshot(const zt_ui_snapshot_t *snapshot, void *context)
{
    (void)context;
    return zt_ui_submit_snapshot(snapshot);
}

/* Host-only bounded queue. Refused event custody stays in the durable journal. */
static zt_err_t on_gateway_feed(const zt_game_feed_item_t *item, void *context)
{
    (void)context;
    return zt_gateway_submit_feed(item);
}

static zt_err_t on_server_decision(const zt_server_decision_t *value, void *context)
{ (void)context; return zt_game_post_decision(value); }
static zt_err_t on_server_command(const zt_server_command_t *value, void *context)
{ (void)context; return zt_game_post_command(value); }
static zt_err_t on_server_snapshot(const zt_server_snapshot_t *value, void *context)
{ (void)context; return zt_game_post_snapshot(value); }
static zt_err_t on_server_receipt(const zt_server_receipt_t *value, void *context)
{ (void)context; return zt_game_post_receipt(value); }

/* The console parses and posts; zt_ops performs the effect and builds the response,
 * running in the game task's context. app_main no longer answers console requests
 * itself: its stub replied with an empty body, which console.c rejects, so an
 * operation could succeed on the badge while the wire reported failure. */
static zt_err_t on_console_request(const zt_console_request_t *request, void *context)
{
    (void)context;
    return zt_ops_post(request);
}

static void reply(uint32_t id, zt_err_t result, const char *json)
{
    zt_console_response_t response = {.id = id, .result = result};
    if (json) {
        size_t len = strlen(json);
        if (len > ZT_CONSOLE_RESPONSE_MAX_BYTES) len = ZT_CONSOLE_RESPONSE_MAX_BYTES;
        memcpy(response.json, json, len);
        response.json[len] = '\0';
        response.len = len;
    }
    zt_console_reply(&response);
}

/* Configuration is acknowledged by HASH ONLY: never echo a credential, a token, an
 * SSID or the mesh key. The durable write is confirmed by reading the stored value
 * back and comparing it, which is a stronger claim than a queue accepting the
 * request, and needs no new contract. A definitive failure is reported as such. */
static void apply_configure(uint32_t id, const zt_config_t *wanted)
{
    zt_ui_snapshot_t snapshot;
    if (zt_game_snapshot(&snapshot) != ZT_OK) { reply(id, ZT_ERR_INVALID_STATE, NULL); return; }
    if ((snapshot.admission != ZT_ADMISSION_NEEDS_CONFIG &&
         snapshot.admission != ZT_ADMISSION_LOBBY) || snapshot.round_id) {
        /* plan 13: credentials, game and key change only in lobby with no pending
         * round. Reconfiguring under a live round would silently change identity. */
        reply(id, ZT_ERR_INVALID_STATE, NULL);
        return;
    }
    zt_persist_request_t request = {.request_id = 0, .kind = ZT_PERSIST_CONFIG};
    request.value.config = *wanted;
    zt_err_t result = zt_store_submit(&request);
    if (result != ZT_OK) { reply(id, result, NULL); return; }
    zt_config_t stored;
    result = ZT_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (zt_store_load_config(&stored) != ZT_OK) continue;
        if (!memcmp(&stored, wanted, sizeof(stored))) { result = ZT_OK; break; }
    }
    char json[96] = "";
    if (result == ZT_OK) {
        uint8_t digest[32];
        mbedtls_sha256((const unsigned char *)&stored, sizeof(stored), digest, 0);
        memcpy(json, "{\"config_sha256\":\"", 18);
        for (unsigned i = 0; i < 8; ++i)
            snprintf(json + 18 + i * 2, 3, "%02x", digest[i]);
        memcpy(json + 34, "\",\"reboot\":true}", 17);
        json[51] = '\0';
    }
    memset(&stored, 0, sizeof(stored));
    reply(id, result, result == ZT_OK ? json : NULL);
    if (result == ZT_OK) {
        vTaskDelay(pdMS_TO_TICKS(250));   /* let the reply reach the host */
        esp_restart();
    }
}

/* Every task must actually block between iterations. pdMS_TO_TICKS() rounds DOWN, so
 * at the default 100 Hz tick a 5 ms delay became vTaskDelay(0), which does not block:
 * it only yields to equal or higher priority. radio_mesh at priority 7 therefore spun
 * forever, starved the idle task — the task watchdog fired every 5 s naming radio_mesh —
 * and starved the UI at priority 3, which is why the display froze after two stripe
 * bands and looked like a rendering bug. Never let a service loop delay zero ticks. */
#define ZT_TASK_DELAY(ms) vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

/* ---- tasks ---------------------------------------------------------------- */

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        zt_buttons_poll(now_us());
        ZT_TASK_DELAY(ZT_BUTTON_POLL_MS);
    }
}


static void radio_task(void *arg)
{
    (void)arg;
    for (;;) {
        zt_radio_service(now_us());
        zt_mesh_service(now_us());
        ZT_TASK_DELAY(5);
    }
}

/* Boot-only snapshots/configuration must not occupy permanent BSS or the small
 * task stacks. Clear credential-bearing scratch before returning it to the heap. */
static void free_boot_scratch(void *scratch, size_t bytes)
{
    if (!scratch) return;
    volatile uint8_t *p = scratch;
    while (bytes--) *p++ = 0;
    heap_caps_free(scratch);
}

#if ZT_SELF_CONFIG
static void self_configure_boot(void)
{
    struct {
        zt_ui_snapshot_t snapshot;
        zt_config_t loaded;
        zt_persist_request_t request;
    } *scratch = heap_caps_calloc(1, sizeof(*scratch), MALLOC_CAP_8BIT);
    if (!scratch) {
        zt_console_log("self-provision: boot allocation failed",
                       sizeof("self-provision: boot allocation failed") - 1);
        return;
    }
    bool restart = false;
    zt_mac_t mac = {0};
    esp_read_mac(mac.bytes, ESP_MAC_WIFI_STA);
    if (zt_game_snapshot(&scratch->snapshot) != ZT_OK) goto done;
    if (scratch->snapshot.admission == ZT_ADMISSION_WAIT_INSTALL) {
#if ZT_SELF_INSTALL
        if (self_install(&mac) == ZT_OK) {
            ESP_LOGW(TAG, "self-provision: installed, restarting to open storage");
            restart = true;
        }
#else
        ESP_LOGW(TAG, "WAIT_INSTALL: run `install` from the operator menu");
#endif
        goto done;
    }
    if (scratch->snapshot.admission != ZT_ADMISSION_NEEDS_CONFIG &&
        (scratch->snapshot.admission != ZT_ADMISSION_LOBBY || scratch->snapshot.round_id))
        goto done;

    /* Rewrite changed baked settings only in the lobby without a round. The
     * request owns the desired value; one load buffer also verifies its commit. */
    scratch->request.request_id = 1;
    scratch->request.kind = ZT_PERSIST_CONFIG;
    self_config(&mac, &scratch->request.value.config);
    if (scratch->snapshot.admission == ZT_ADMISSION_LOBBY &&
        zt_store_load_config(&scratch->loaded) == ZT_OK &&
        !memcmp(&scratch->loaded, &scratch->request.value.config, sizeof(scratch->loaded)))
        goto done;
    /* submit copies the complete request before returning. No queued pointer
     * refers to this scratch allocation when it is wiped and freed. */
    if (zt_store_submit(&scratch->request) == ZT_OK) {
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(50));
            zt_store_service();
            if (zt_store_load_config(&scratch->loaded) == ZT_OK &&
                !memcmp(&scratch->loaded, &scratch->request.value.config, sizeof(scratch->loaded))) {
                ESP_LOGW(TAG, "self-provision: configured, restarting");
                restart = true;
                break;
            }
        }
        if (!restart) ESP_LOGE(TAG, "self-provision: configuration did not commit");
    }
done:
    free_boot_scratch(scratch, sizeof(*scratch));
    if (restart) {
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }
}
#endif

static void game_task(void *arg)
{
    TaskHandle_t boot_waiter = (TaskHandle_t)arg;
    /* zt_game_init records the calling task as the gameplay owner, so it runs
     * here and not in app_main. Everything else waits for it. */
    zt_err_t result = zt_game_init(NULL, on_snapshot, on_gateway_feed, NULL);
    ESP_LOGI(TAG, "game init %d", (int)result);
#if ZT_SELF_CONFIG
    self_configure_boot();
#endif
    /* Named game-key cleanup and configuration writes may take longer than a
     * fixed startup delay. Publish completion before radio bring-up reads them. */
    xTaskNotifyGive(boot_waiter);
    for (;;) {
        zt_game_service(now_us());
        zt_demo_service(now_us());
        zt_ops_service(now_us());
        /* Console-originated work runs HERE, in the gameplay owner's context, not
         * in the console callback. */
        uint8_t kind = 0;
        uint32_t id = 0;
        portENTER_CRITICAL(&staging_lock);
        kind = staged.pending;
        id = staged.console_id;
        portEXIT_CRITICAL(&staging_lock);
        if (kind == 1) {
            reply(id, zt_demo_start(&staged.demo), NULL);
            portENTER_CRITICAL(&staging_lock);
            staged.pending = 0;
            portEXIT_CRITICAL(&staging_lock);
        } else if (kind == 2) {
            apply_configure(id, &staged.config);
            portENTER_CRITICAL(&staging_lock);
            memset(&staged.config, 0, sizeof(staged.config));
            staged.pending = 0;
            portEXIT_CRITICAL(&staging_lock);
        }
        ZT_TASK_DELAY(10);
    }
}

static void persist_task(void *arg)
{
    (void)arg;
    for (;;) {
        zt_store_service();
        ZT_TASK_DELAY(10);
    }
}

static void gateway_task(void *arg)
{
    (void)arg;
    zt_config_t config;
    const zt_gateway_sinks_t sinks = {
        .decision = on_server_decision, .command = on_server_command,
        .snapshot = on_server_snapshot, .receipt = on_server_receipt,
    };
    zt_err_t result = zt_store_load_config(&config);
    if (result == ZT_OK) result = zt_gateway_init(&config, &sinks);
    memset(&config, 0, sizeof(config));
    if (result != ZT_OK) {
        zt_console_log("gateway initialization failed", sizeof("gateway initialization failed") - 1);
        vTaskDelete(NULL);
        return;
    }
    bool sntp_started = false;
    for (;;) {
        zt_channel_status_t channel = {0};
        if (!sntp_started && zt_channel_get_status(&channel) == ZT_OK && channel.has_ip) {
            esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_started = esp_netif_sntp_init(&clock) == ESP_OK;
        }
        zt_gateway_service(now_us());
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    /* zt_ui_service() reports render and LCD faults through its return value, and
     * discarding it made a stuck display completely silent: two stripe bands reached
     * the panel and the rest stayed as power-on GRAM noise with nothing logged.
     * Report every distinct outcome, and keep reporting a persistent one at a bounded
     * rate so a fault that never clears is visible without flooding the console. */
    zt_err_t previous = ZT_OK;
    unsigned repeats = 0;
    for (;;) {
        zt_err_t result = zt_ui_service(now_us());
        if (result != ZT_OK) {
            if (result != previous || ++repeats >= 50) {
                ESP_LOGE(TAG, "ui service %d (repeat %u)", (int)result, repeats);
                repeats = 0;
            }
            previous = result;
        } else if (previous != ZT_OK) {
            ESP_LOGW(TAG, "ui service recovered from %d", (int)previous);
            previous = ZT_OK;
            repeats = 0;
        }
        ZT_TASK_DELAY(5);
    }
}

/* USB Serial/JTAG discards anything written while no host is attached, so boot-time
 * logs are gone by the time a host enumerates, which is why every failure so far has
 * been invisible until it happened to coincide with an open connection. Emit a compact
 * heartbeat instead, so attaching at any moment shows the badge's actual state. It
 * carries no key, token, credential or MAC. */
static void heartbeat(void)
{
    static zt_ui_snapshot_t snap;
    char line[241];
    zt_radio_diagnostics_t diag = {0};
    if (zt_game_snapshot(&snap) != ZT_OK) {
        zt_console_log("hb: no snapshot yet", sizeof("hb: no snapshot yet") - 1);
        return;
    }
    zt_radio_get_diagnostics(&diag);
    zt_channel_status_t ch = {0};
    zt_channel_get_status(&ch);
    /* Console initialization suppresses ESP_LOG output to keep SDK messages and
     * credentials off the JSON transport. Use its sanitized log queue instead. */
    int len = snprintf(line, sizeof(line),
             "hb admission=%d ch=%u chstate=%d assoc=%u ip=%u peers=%u switch=%u configured=%u "
             "radio_host=%u host=%u srv=%u rx_drop=%lu auth_fail=%lu invalid=%lu dedupe=%lu radio_started=%u",
             (int)snap.admission, (unsigned)ch.channel, (int)ch.state,
             (unsigned)ch.associated, (unsigned)ch.has_ip,
             (unsigned)snap.direct_contact_count,
             (unsigned)snap.host_selected, (unsigned)snap.host_configured,
             (unsigned)radio_host, (unsigned)snap.host_connected, (unsigned)snap.server_connected,
             (unsigned long)diag.rx_drops, (unsigned long)diag.auth_failures,
             (unsigned long)diag.invalid_frames, (unsigned long)diag.dedupe_hits,
             (unsigned)radio_started);
    if (len > 0 && (size_t)len < sizeof(line)) zt_console_log(line, (size_t)len);
    len=snprintf(line,sizeof(line),"system build=%.*s uptime_s=%llu reset=%u min_free=%lu",
        ZT_BUILD_ID_LEN,ZT_BUILD_ID,(unsigned long long)(now_us()/1000000ULL),
        (unsigned)esp_reset_reason(),(unsigned long)esp_get_minimum_free_heap_size());
    if (len > 0 && (size_t)len < sizeof(line)) zt_console_log(line, (size_t)len);
    if (radio_host) {
        zt_gateway_status_t gateway = {0};
        if (zt_gateway_status(&gateway) == ZT_OK) {
            len = snprintf(line, sizeof(line),
                "gateway connected=%u welcomed=%u error=%d http=%d clock=%u free=%lu largest=%lu drops=%lu reconnects=%lu",
                (unsigned)gateway.connected, (unsigned)gateway.welcomed,
                (int)gateway.last_error, (int)gateway.http_status,
                (unsigned)(time(NULL) >= 1704067200),
                (unsigned long)esp_get_free_heap_size(),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned long)gateway.dropped_messages, (unsigned long)gateway.reconnect_count);
            if (len > 0 && (size_t)len < sizeof(line)) zt_console_log(line, (size_t)len);
            len=snprintf(line,sizeof(line),"gateway stack_free_min=%lu websocket_stack_free_min=%lu",
                (unsigned long)gateway.gateway_stack_free_min,(unsigned long)gateway.websocket_stack_free_min);
            if (len > 0 && (size_t)len < sizeof(line)) zt_console_log(line, (size_t)len);
        }
    }
}

static void console_task(void *arg)
{
    (void)arg;
    uint64_t next_hb = 0;
    for (;;) {
        zt_console_service(now_us());
        if (now_us() >= next_hb) {
            heartbeat();
            next_hb = now_us() + 2000000;
        }
        ZT_TASK_DELAY(20);
    }
}

/* ---- radio bring-up -------------------------------------------------------- */

/* Started only once the game reports a configured badge. plan 4.1: an unconfigured
 * or uninstalled badge stays USB-only and never starts Wi-Fi. */
static void start_radio_if_configured(void)
{
    /* Keep these large boot-only values off the main task's small stack, and
     * release their allocation before Wi-Fi and gateway TLS consume the heap. */
    struct {
        zt_ui_snapshot_t snapshot;
        zt_config_t config;
    } *scratch = heap_caps_calloc(1, sizeof(*scratch), MALLOC_CAP_8BIT);
    if (!scratch) {
        zt_console_log("radio not started: boot allocation failed",
                       sizeof("radio not started: boot allocation failed") - 1);
        return;
    }
    if (zt_game_snapshot(&scratch->snapshot) != ZT_OK) {
        free_boot_scratch(scratch, sizeof(*scratch));
        return;
    }
    if (scratch->snapshot.admission == ZT_ADMISSION_WAIT_INSTALL ||
        scratch->snapshot.admission == ZT_ADMISSION_NEEDS_CONFIG ||
        scratch->snapshot.error == ZT_ERROR_STORAGE) {
        ESP_LOGW(TAG, "radio not started: admission %d", (int)scratch->snapshot.admission);
        free_boot_scratch(scratch, sizeof(*scratch));
        return;
    }
    if (zt_store_load_config(&scratch->config) != ZT_OK) {
        ESP_LOGE(TAG, "radio not started: configuration unreadable");
        free_boot_scratch(scratch, sizeof(*scratch));
        return;
    }
    uint8_t host_selected = 0, changed_live = 0;
    /* The input task has only just started: its first sample is not a debounced
     * switch value. BUSY must never silently select player mode for this boot. */
    zt_err_t switch_result;
    uint64_t switch_deadline = now_us() + 1000000;
    do {
        switch_result = zt_buttons_boot_host_switch(&host_selected, &changed_live);
        if (switch_result != ZT_ERR_BUSY) break;
        ZT_TASK_DELAY(ZT_BUTTON_POLL_MS);
    } while (now_us() < switch_deadline);
    if (switch_result != ZT_OK) {
        zt_console_log("radio not started: host switch not ready",
                       sizeof("radio not started: host switch not ready") - 1);
        free_boot_scratch(scratch, sizeof(*scratch));
        return;
    }
    zt_radio_config_t radio = {
        .game_id = scratch->config.game_id,
        .self_mac = scratch->snapshot.self_mac,
        .host_mac = scratch->config.host_mac,
        .last_channel = scratch->config.last_channel ? scratch->config.last_channel : ZT_CHANNEL_MIN,
        /* Host mode needs BOTH the physical switch and a configuration naming this
         * badge as the designated host with credentials. The switch alone can never
         * create a second host. */
        .is_host = host_selected && scratch->snapshot.host_configured,
    };
    memcpy(radio.group_key, scratch->config.group_key, sizeof(radio.group_key));
    free_boot_scratch(scratch, sizeof(*scratch));
    zt_err_t result = zt_radio_init(&radio, on_rx_frame, on_tx_complete, NULL);
    memset(radio.group_key, 0, sizeof(radio.group_key));
    if (result != ZT_OK) {
        ESP_LOGE(TAG, "radio init %d", (int)result);
        return;
    }
    result = zt_mesh_init(on_domain_message, NULL);
    if (result != ZT_OK) {
        ESP_LOGE(TAG, "mesh init %d", (int)result);
        return;
    }
    result = zt_channel_start_discovery(radio.last_channel);
    ESP_LOGI(TAG, "radio started, host=%u, discovery %d",
             (unsigned)radio.is_host, (int)result);
    radio_host = radio.is_host;
    radio_started = 1;
}

/* ---- entry ----------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "firmware %s protocol %u build %.*s",
             ZT_FIRMWARE_VERSION, (unsigned)ZT_PROTOCOL_VERSION,
             ZT_BUILD_ID_LEN, ZT_BUILD_ID);

    /* Local I/O first: a badge that cannot start Wi-Fi must still show its state
     * and accept USB provisioning.
     *
     * zt_ui_init() OWNS the display and the LEDs: it calls zt_lcd_init() with its own
     * DMA-completion callback and zt_leds_init() itself. Do not call either here.
     * Calling zt_lcd_init() first registers a NULL completion callback and then makes
     * zt_ui_init() fail with ZT_ERR_INVALID_STATE, after which every
     * zt_ui_submit_snapshot() is rejected, no frame is ever rendered, and the ST7789
     * simply displays its uninitialised power-on GRAM as noise. */
    zt_err_t ui = zt_ui_init();
    ESP_LOGI(TAG, "ui %d buttons %d console %d", (int)ui,
             (int)zt_buttons_init(on_button, NULL),
             (int)zt_console_init(on_console_request, NULL));
    if (ui != ZT_OK) ESP_LOGE(TAG, "UI init failed: the screen will not render");

    if (xTaskCreate(game_task, "game", TASK_GAME_STACK, xTaskGetCurrentTaskHandle(),
                    TASK_GAME_PRIORITY, NULL) != pdPASS) {
        zt_console_log("game task allocation failed", sizeof("game task allocation failed") - 1);
        vTaskDelete(NULL);
        return;
    }
    /* The game owns storage bring-up; let it reach a decided admission state before
     * reading the snapshot that decides whether the radio may start. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xTaskCreate(persist_task, "persist", TASK_PERSIST_STACK, NULL, TASK_PERSIST_PRIORITY, NULL);
    xTaskCreate(ui_task, "ui", TASK_UI_STACK, NULL, TASK_UI_PRIORITY, NULL);
    xTaskCreate(console_task, "console", TASK_CONSOLE_STACK, NULL, TASK_CONSOLE_PRIORITY, NULL);
    xTaskCreate(input_task, "input", TASK_INPUT_STACK, NULL, TASK_INPUT_PRIORITY, NULL);

    ESP_LOGW(TAG, "starting radio now");
    start_radio_if_configured();
    ESP_LOGW(TAG, "radio start returned, started=%u", (unsigned)radio_started);
    if (radio_started)
        xTaskCreate(radio_task, "radio_mesh", TASK_RADIO_STACK, NULL, TASK_RADIO_PRIORITY, NULL);
    if (radio_started && radio_host &&
        xTaskCreate(gateway_task, "gateway", TASK_GATEWAY_STACK, NULL, TASK_GATEWAY_PRIORITY, NULL) != pdPASS)
        zt_console_log("gateway task allocation failed", sizeof("gateway task allocation failed") - 1);

    vTaskDelete(NULL);
}
