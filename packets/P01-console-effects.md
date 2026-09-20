# Packet P01 — the console effect layer

## Goal

Implement `firmware/components/zt_ops/` against the frozen `zt_ops.h`: perform each USB
console operation and build its response, so the operator tool can actually install,
inspect and commission a badge.

## Why this exists, and what it already broke

`zt_console.h` makes the console a parser and transport: its callback "parses/copies/posts
only; no direct gameplay mutations". Something else performs the effect. Until now that
was a stub in `main/app_main.c` that replied with an empty body, so `console.c` rejected
the reply with `ZT_ERR_INVALID_LENGTH` and the operator saw
`CONSOLE_OPERATION_REFUSED` with `error: 3` — on an `install_init` whose storage work had
very likely already succeeded on the badge. That is the worst kind of failure: the device
changed state and the operator was told it had not.

`docs/console.md` is the specification for every response in this packet. Follow it
exactly; the operator tool verifies these fields and refuses on any mismatch.

## Base commit and dependencies

- Base commit: `c4d63910fb2b473b495731fa2630cd88cf42f97b` on `firmware/integration`.
- Branch: `firmware/P01-console-effects`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/P01`
- Merged and available: C01's `zt_console`, G01–G05's `zt_game`/`zt_store`, M01's
  `zt_radio` with `zt_radio_get_diagnostics()`, H01's `zt_hal`/`zt_ui`.

## Owned file allowlist

```
firmware/components/zt_ops/CMakeLists.txt
firmware/components/zt_ops/ops.c
```

Nothing else. Not `zt_console`, not `zt_store`, not `zt_game`, not any header, not
`app_main.c`, not `tools/`, not a document. The orchestrator wires `app_main` to call
`zt_ops_post()` and `zt_ops_service()`.

## Read before writing code

`docs/console.md` in full — it defines every field of every response and is what the tool
checks. Then `zt_ops.h`, `zt_console.h`, `zt_store.h` (especially `zt_store_install_init`,
`zt_store_inspect`, `zt_store_status`, `zt_store_export`), `zt_game.h`
(`zt_game_snapshot`, `zt_game_post_button`, `zt_game_event_feed`) and `zt_radio.h`
(`zt_radio_get_diagnostics`, `zt_radio_link_allowlist`).

## Required behavior

Implement every operation in `zt_console_operation_t`, each replying through
`zt_console_reply()` with a non-empty JSON object echoing the request id.

### `info`

The full flat field set in `docs/console.md`, including:

- `factory_sha256`: SHA-256 over **all 2,752,512 bytes** of the factory allocation, by
  bounded raw reads and incremental hashing. An app-image or ELF digest is **not** this
  value and the tool will reject it.
- `install_state`, `tail_blank` (true only after verifying the entire 64 KiB tail is
  `0xff`), `header_hex` (the raw read-back 80-byte header as 160 hex digits, null when
  there is no valid header), `config_status`, `admission`, `active_round`, `role`,
  `last_error`, and the fixed layout constants.
- Available in `WAIT_INSTALL` as well as configured and error states.
- **Never fill an unknown fact with a successful-looking default.** If the digest,
  identity or layout cannot be established, fail the operation.

### `install_init`

Compare the physical MAC, require `WAIT_INSTALL` and a verified blank tail, then call
`zt_store_install_init()`. Do not write a second initializer, a format, or a retry
fallback. On durable success, raw-read and verify the header, establish that configuration
is absent, and reply with `install_state` `VALID`, `header_hex` and `config_status`
`absent`. Perform **no reboot** before that response or the tool's follow-up `info`.

### `configure`

Validate admission — lobby or needs-config, with no pending round — persist through the
ordinary store path, confirm the durable result, and acknowledge with **hashes only**.
Never echo a key, token, password or SSID. A definitive failure is reported as a failure
and retains the existing configuration.

### `status` and `diagnose`

Sanitized counters, ages and pending identifiers only: take the radio-owned values from
`zt_radio_get_diagnostics()` rather than inventing a parallel counter set, plus heap and
storage figures. No packet dumps, no configuration dump, no recovery reference.

### `button`, `link_allowlist`, `game_export`, `archive_clear`

`button` posts through the ordinary game rules — it is an input, never an override.
`link_allowlist` applies the pre-protocol RX filter, is never persisted, and drives the
DIAGNOSTIC banner through the existing diagnostics path. `game_export` streams bounded
pages of active or named-round evidence and never reads stock storage; previous-round
enumeration is unavailable and must say so rather than silently exporting a partial set.
`archive_clear` requires a receipt covering the exported and decided frontiers.

## Ownership and threading

Anything touching gameplay or durable state runs in the **game task's** context, through
`zt_ops_service()`. `zt_ops_post()` copies and returns. Bounded work per service call:
the factory hash in particular must be chunked across calls rather than blocking, and
nothing may hold the CPU long enough to trip a watchdog or stall the display. No heap in
the hot path, no borrowed pointers, no blocking waits.

## Refusals

No key, token, password, SSID, recovery reference or flash content beyond the 80-byte
header may appear in a response or a log. No operation may erase, format, or clear stock
NVS or LittleFS. No operation may change a role, a round, or patient zero. An operation
whose preconditions fail is refused with the documented failure, never half-performed.

## Excluded

No new console operation, no new API, no header change, no backend, no tests or harness.

## Allowed checks

Per-file compilation with the installed cross compiler. **Do not run `idf.py build`** —
the sandbox blocks it with the psutil `sysctl()` PermissionError, which is expected. The
orchestrator runs the authoritative build.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the two allowlisted files exist or changed.
2. Every reply is a non-empty JSON object echoing the request id — the bug that started
   this packet.
3. `factory_sha256` covers the whole padded factory allocation and is chunked, not
   blocking.
4. `install_init` performs no second initializer and no reboot before its response.
5. No secret or flash content beyond the header is ever returned or logged.
6. Unknown facts fail the operation instead of defaulting.

## Final report

Standard format. List each operation with the exact response fields you produce, and state
how the factory hash is chunked and what its worst-case per-call cost is.
