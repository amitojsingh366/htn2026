#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_common.h"
#include "zt_ids.h"
#include "zt_wire.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Contract amendment 4, 2026-09-19, orchestrator-owned, user-directed.
 *
 * DEMO MODE. plan.md D08 and 4.1 give the server sole authority to freeze a roster,
 * select patient zero and start a round, and that remains the real design. The backend
 * that would do so is not implemented, so with three badges and no server the firmware
 * reaches LOBBY and stops: no roles, no timer, no tagging, nothing to observe. The user
 * directed an explicitly marked local round so the proof of concept can run on hardware.
 *
 * This is a deviation and it is designed to be impossible to mistake for the real thing:
 *
 *  - It is reachable ONLY through the USB console, on a badge an operator physically
 *    holds. No button, no radio packet, no timer and no boot path may start it.
 *  - Every badge shows a persistent DEMO MODE banner for as long as it is active.
 *  - It is refused outright when a server-authoritative round exists: any admission
 *    state other than LOBBY or NEXT_ROUND, or any retained unfinalized round, is
 *    ZT_ERR_INVALID_STATE. It never overrides, cancels or replaces a real round.
 *  - It creates no new authority: it synthesizes exactly the snapshot and commands a
 *    server would have sent and posts them through the existing frozen
 *    zt_game_post_snapshot() and zt_game_post_command() entry points. The game applies
 *    them through its ordinary validation, and no gameplay rule is relaxed. Tag range,
 *    causality, persistence, the PERSISTING lock and the 600 s deadline all stand.
 *
 * REVISED 2026-09-19 after packet D01 read the merged game and found the original design
 * unimplementable. The first version had every badge synthesize the round locally from an
 * identical roster. That cannot work: a badge must be admitted to a round before PREPARE
 * applies, and admission is not something a badge grants itself.
 *
 * The revised design uses the ordinary protocol instead of working around it. The demo is
 * started on ONE badge, which must be the designated host. That badge posts the
 * synthesized snapshot and commands to its own game through the existing frozen entry
 * points; amendment 6 lets an authoritative roster naming a badge register it, and packet
 * G02's host origination then distributes JOIN_RESULT, ROSTER_PAGE and COMMAND over the
 * mesh exactly as it would for a real server. The other badges simply play: they JOIN,
 * they are admitted from the frozen roster, and they receive PREPARE and START. Nothing
 * about their path is demo-specific, which is the point.
 *
 * Slot is the index in players[]. The roster stays explicit rather than discovered so the
 * operator controls exactly who is in the round.
 *
 * Duration is fixed. The merged game validates PREPARE and START against exactly
 * 600000 ms, so duration_ms accepts only 0 or ZT_DEMO_DEFAULT_DURATION_MS and rejects
 * anything else rather than silently substituting a value the game would refuse.
 *
 * A demo round DOES persist its checkpoint, exactly as a real round does. The earlier
 * "no persistence" wording was wrong: persistence is what makes a rebooted badge rejoin
 * with its role and events intact, and suppressing it would break the very behaviour the
 * demo exists to show. What is RAM-only is D01's own staging state. A demo round is
 * never uploaded and never reconciled against a server, and its results are local only.
 * It must not be presented as a measured 20-player result. */
#define ZT_DEMO_DEFAULT_DURATION_MS 600000u
#define ZT_DEMO_MIN_PLAYERS 2
#define ZT_DEMO_START_DELAY_MIN_MS 3000u
#define ZT_DEMO_START_DELAY_MAX_MS 30000u
#define ZT_DEMO_BANNER "DEMO MODE - LOCAL ROUND, NO SERVER"
typedef struct {
    zt_round_id_t round_id;   /* nonzero */
    uint32_t duration_ms;     /* 0 or ZT_DEMO_DEFAULT_DURATION_MS; nothing else */
    uint8_t player_count;     /* 2..ZT_MAX_PLAYERS */
    zt_mac_t players[ZT_MAX_PLAYERS]; /* slot == index; identical on every badge */
    uint8_t patient_zero_slot;
    uint8_t channel;          /* 1..11; identical on every badge */
    uint16_t start_delay_ms;  /* clamped to the MIN/MAX above */
} zt_demo_round_t;
/* Called from the console sink in the game task's context. Validates the request,
 * refuses when a real round exists, and stages the synthetic snapshot and commands.
 * Returns ZT_ERR_INVALID_ARG on a malformed roster, ZT_ERR_NOT_FOUND when this badge's
 * own MAC is absent from players[], ZT_ERR_INVALID_STATE when refused. */
zt_err_t zt_demo_start(const zt_demo_round_t *round);
/* Started only on the designated host badge: ZT_ERR_AUTH when this badge is not the
 * configured host with the host switch latched. A player badge never starts a demo. */
/* Drives staged delivery and re-raises the banner. Serviced from the game task. */
zt_err_t zt_demo_service(uint64_t now_us);
/* Nonzero while a demo round is active, for the banner and for refusing a second one. */
zt_err_t zt_demo_active(uint8_t *out);
#ifdef __cplusplus
}
#endif
