#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_game.h"
#include "zt_demo.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZT_CONSOLE_INPUT_MAX_BYTES 2048
#define ZT_CONSOLE_RESPONSE_MAX_BYTES 2048
#define ZT_CONSOLE_HOST_WRITE_MAX_BYTES 192
#define ZT_CONSOLE_LOG_LINES_PER_SECOND 10
#define ZT_CONSOLE_LOG_PREFIX "# "
#define ZT_CONSOLE_MACHINE_PREFIX '{'
typedef enum {
    ZT_CONSOLE_INFO, /* Read-only versions/full MAC/install/chip/layout/padded
                      * factory SHA256/round/role/error; never keys. */
    ZT_CONSOLE_INSTALL_INIT, /* WAIT_INSTALL + blank tail; expected MAC, schema,
                             * UUID, baseline SHA256; tool verified backup/flash. */
    ZT_CONSOLE_CONFIGURE, /* Private bounded config, expected MAC, HTTPS-only;
                          * credentials/game/key changes only lobby/no pending
                          * round. Persist atomically, hash-only ack, reboot. */
    ZT_CONSOLE_STATUS, /* Read-only sanitized counters/heap/ages/pending IDs. */
    ZT_CONSOLE_BUTTON, /* Debug press/release through normal game rules only. */
    ZT_CONSOLE_LINK_ALLOWLIST, /* Explicit diagnostic mode, visible banner;
                               * pre-protocol RX filter, empty disables,
                               * no fake RSSI and never persisted. */
    ZT_CONSOLE_GAME_EXPORT, /* Bounded active/previous game evidence pages only;
                            * never stock storage. */
    ZT_CONSOLE_ARCHIVE_CLEAR, /* Receipt covers prior exported/decided frontiers;
                             * no config/install-marker/stock deletion. */
    ZT_CONSOLE_DEMO_ROUND /* Contract amendment 4: operator-started local round for
                          * the proof-of-concept demo. See zt_demo.h. Explicitly a
                          * deviation from server authority, visibly banner-marked,
                          * and refused when a real round is already running. */
} zt_console_operation_t;
typedef struct {
    uint32_t id;
    zt_console_operation_t op;
    union {
        zt_install_header_t install;
        zt_config_t configure;
        zt_button_edge_t button;
        struct { uint8_t count; zt_mac_t macs[ZT_LINK_ALLOWLIST_MAX]; } allowlist;
        struct { zt_round_id_t round_id; uint16_t cursor; } export_page;
        zt_archive_clearance_t clearance;
        zt_demo_round_t demo;
    } args;
} zt_console_request_t;
typedef struct { uint32_t id; zt_err_t result; size_t len; char json[ZT_CONSOLE_RESPONSE_MAX_BYTES + 1]; } zt_console_response_t;
typedef struct { char bytes[ZT_CONSOLE_INPUT_MAX_BYTES + 1]; size_t used; uint8_t discarding_overflow; } zt_console_line_t;
typedef zt_err_t (*zt_console_sink_t)(const zt_console_request_t *request, void *context);
/* UTF-8 JSON Lines. Incrementally assemble <=192-byte host writes; reject
 * complete line >2048 bytes. Echo u32 request id, never provisioning secrets.
 * Machine response starts '{'; asynchronous log starts '# '. Serialize whole
 * lines so logs cannot interleave JSON. No hexdumps/full-config dumps.
 * Callback parses/copies/posts only; no direct gameplay mutations. */
zt_err_t zt_console_init(zt_console_sink_t sink, void *context);
zt_err_t zt_console_receive(const uint8_t *bytes, size_t len);
zt_err_t zt_console_reply(const zt_console_response_t *response);
zt_err_t zt_console_log(const char *sanitized_text, size_t len);
zt_err_t zt_console_service(uint64_t now_us);
#ifdef __cplusplus
}
#endif
