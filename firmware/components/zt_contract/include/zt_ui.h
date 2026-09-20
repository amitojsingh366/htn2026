#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_game.h"
#include "zt_hal.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ZT_UI_MAX_FPS 10
#define ZT_UI_FRAME_MIN_MS 100
#define ZT_LED_MAX_HZ 25
#define ZT_LED_FRAME_MIN_MS 40
typedef enum { ZT_SCREEN_SETUP, ZT_SCREEN_LOBBY, ZT_SCREEN_PREPARED, ZT_SCREEN_RUNNING_RADAR, ZT_SCREEN_PEER_LIST, ZT_SCREEN_STATUS, ZT_SCREEN_END } zt_screen_t;
typedef struct { zt_feedback_t kind; uint64_t expires_us; } zt_ui_overlay_t;
/* UI and LED are non-blocking state machines: no delay loops, no per-frame heap.
 * Coalesce snapshots; use two DMA stripes, wait for completion before reuse.
 * Infection/tag feedback outranks announcements. Radar angles are stable MAC
 * hashes for layout only, never direction. No button grants reset/erase/role. */
zt_err_t zt_ui_init(void);
zt_err_t zt_ui_submit_snapshot(const zt_ui_snapshot_t *snapshot);
zt_err_t zt_ui_select_screen(zt_screen_t screen);
/* Contract amendment 2, 2026-09-19, raised by packet H01 and integrated by the
 * orchestrator. Screen navigation is NOT gameplay state: which screen is shown, the
 * peer-list scroll offset, the diagnostics page index, whether an overlay is open, the
 * Home-hold timer and the brightness menu are all UI-local. The frozen header exposed
 * no way for the UI to observe button edges, so plan 6's "D-pad scrolls lists",
 * "left/right changes diagnostics page" and "hold Home for two seconds opens a
 * status/settings menu" were unimplementable as written.
 *
 * The integration layer fans each edge from the input task to BOTH the game task, which
 * owns gameplay meaning (A tags, A registers), and to the UI, which owns navigation.
 * This does not weaken the single-writer rule: the game task remains the only writer of
 * gameplay state, and the UI still never mutates it.
 *
 * No button reachable through this path may reset a role, reset a round, erase storage,
 * or change patient zero. Brightness may only be lowered, never raised above the
 * hardware cap of 24. */
zt_err_t zt_ui_post_button(const zt_button_edge_t *edge);
zt_err_t zt_ui_overlay(const zt_ui_overlay_t *overlay);
zt_err_t zt_ui_announce(const char *text, size_t len, uint64_t expires_us);
/* Contract amendment 5, 2026-09-19, raised by packet D01 and integrated by the
 * orchestrator.
 *
 * A banner is a persistent status line, not an announcement. plan 13 requires a visible
 * DIAGNOSTIC banner for as long as the link allowlist is engaged, and zt_demo.h requires
 * a DEMO MODE banner for as long as a local round is active. Neither can be built on
 * zt_ui_announce(): announcements are capped at ZT_ANNOUNCE_MAX_DISPLAY_MS (10 s) and
 * refused within ZT_ANNOUNCE_MIN_INTERVAL_MS (15 s) of the previous one, so a refreshed
 * announcement is guaranteed to leave a five-second hole. A banner that lapses while the
 * condition holds is worse than no banner, because it tells the operator the badge is in
 * a state it is not in.
 *
 * A banner therefore has no expiry and no rate limit. It stays until it is replaced or
 * cleared with a NULL pointer or a zero length. It is the LOWEST priority element on
 * screen: infection and tag feedback, error overlays and announcements all outrank it,
 * and it must never occupy their space or delay them. Bounded printable ASCII, at most
 * ZT_ANNOUNCE_MAX_LEN, validated exactly as an announcement is. It carries no secret,
 * no key, no token and no credential, and it is never persisted. */
zt_err_t zt_ui_set_banner(const char *text, size_t len);
/* Can only LOWER the hardware component cap of 24; never raise above it. */
zt_err_t zt_ui_set_brightness_cap(uint8_t cap);
zt_err_t zt_ui_service(uint64_t now_us);
zt_err_t zt_ui_render_stripe(const zt_ui_snapshot_t *snapshot, zt_screen_t screen, const zt_rect_t *rect, uint16_t *pixels, size_t capacity_pixels);
/* Small flash-resident ASCII bitmap font; caller-provided bounded glyph buffer. */
zt_err_t zt_ui_font_glyph(uint8_t ascii, uint8_t *out, size_t capacity, uint8_t *width, uint8_t *height, size_t *written);
#ifdef __cplusplus
}
#endif
