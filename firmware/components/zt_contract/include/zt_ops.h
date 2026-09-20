#pragma once
#include <stddef.h>
#include <stdint.h>
#include "zt_console.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Contract amendment 6, 2026-09-19, orchestrator-owned.
 *
 * zt_console.h deliberately makes the console a parser and a transport: its callback
 * "parses/copies/posts only; no direct gameplay mutations". Something else must therefore
 * perform each operation's effect and build its response, and docs/console.md calls that
 * something "the sink" or "integration". Until now that was a stub in app_main which
 * replied with an empty body, and console.c correctly rejected it with
 * ZT_ERR_INVALID_LENGTH, so install_init failed on the wire even when the storage
 * operation itself had succeeded on the badge.
 *
 * The effect layer is real work — a SHA-256 over the whole 2,752,512-byte factory
 * allocation, raw header reads, bounded JSON construction, admission checks — and it does
 * not belong inline in the integration entry point. It lives here as its own module.
 *
 * Ownership: zt_ops runs in the GAME task's context for anything that touches gameplay or
 * durable state, exactly as the stub did, because the game task is the only writer. It
 * never parses the wire format (that is zt_console), never mutates gameplay state
 * directly (it goes through the frozen zt_game entry points), and never invents a fact:
 * an operation whose evidence cannot be established fails rather than returning a
 * successful-looking default.
 *
 * It must never place a key, token, password, SSID, recovery reference or any flash
 * contents beyond the 80-byte installation header into a response or a log. */
/* Called from the console sink. Copies what it needs and returns immediately; a request
 * requiring durable work is completed later and answered through zt_ops_service(). */
zt_err_t zt_ops_post(const zt_console_request_t *request);
/* Serviced from the game task. Performs staged work and replies through
 * zt_console_reply(). Bounded work per call; never blocks. */
zt_err_t zt_ops_service(uint64_t now_us);
#ifdef __cplusplus
}
#endif
