# Packet C01 — USB console and the operator tool's device-facing subcommands

## Goal

Implement `components/zt_console/console.c` — the USB JSON-Lines operator interface — and
add the console-dependent subcommands to `tools/badge_tool.py` that packet R01
deliberately left to you.

This is the seam between a laptop and a participant's badge. Everything crossing it is
either a secret going in or evidence coming out, so the bounds and the refusals are the
feature.

## Base commit banner, added 2026-09-19 by the orchestrator

- Base commit: `1adb1ece7c955e7ca651ffe0bb098907a234bba3` on `firmware/integration`.
- **H01, M01 and G01 are all merged.** `zt_hal`, `zt_ui`, `zt_radio`, `zt_game` and
  `zt_store` are real implementations in your worktree, not scaffolds. Read
  `zt_store.c`'s `zt_store_install_init` and `zt_store_inspect` before writing the
  console's `install_init` path: G01 already implemented the firmware side of the
  guarded installation, including writing the raw header last. You are the transport
  and the operator-facing command, not a second implementation of it.
- Three contract amendments are in the frozen headers: `zt_mesh_get_boot_nonce`,
  `zt_ui_post_button`, and amendment 3's `zt_radio_get_diagnostics` /
  `zt_radio_mesh_restore`. `zt_radio_get_diagnostics` is what the `status` operation
  should report for radio counters — do not invent a parallel counter set.
- The badge-1 flash and restore loop has been demonstrated on real hardware at this
  base, so your work is on the critical path to a configurable badge: without
  `install_init` and `configure` over USB, a flashed badge cannot leave `WAIT_INSTALL`.
- Note on `tools/`: `zt_badge.revision()` hashes `badge_tool.py`, `requirements.txt` and
  `zt_badge/*.py`, and a restore rehearsal is only accepted under the current revision.
  Your two new modules under `tools/zt_badge/` therefore invalidate a rehearsal performed
  on hardware, which the orchestrator has accounted for and will repeat once. This is a
  reason to keep your tool-side footprint to exactly the two allowlisted files, and not a
  reason to put gate logic somewhere unhashed: guarded behavior belongs inside the hashed
  set. Packet O01 is concurrently writing `tools/badge_ops.py`, a convenience wrapper
  outside that set — do not edit, read-depend on, or coordinate with it.

## Branch and worktree

- Branch: `firmware/C01-console-provisioning`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/C01`

## Applicable specification

`plan.md` §13 **in full**, §3.2 (persistent data and the installation header), §11
("Utility interface and ownership", and `reset-game-storage` / `install-init` /
`import-bundle` semantics), §4.1 for `WAIT_INSTALL`. `docs/recovery.md` is R01's document
and describes the archive side — read it, do not edit it.

## Owned file allowlist

```
firmware/components/zt_console/CMakeLists.txt
firmware/components/zt_console/console.c
tools/zt_badge/console.py
tools/zt_badge/cli_console.py
docs/console.md
```

**Do not edit any other file under `tools/`.** R01 is merged and owns
`badge_tool.py`, `cli_recovery.py`, `archive.py`, `device.py`, `gates.py`, `release.py`,
`bundle.py`, and the rest. It left you an extension point: `badge_tool.py` imports
`zt_badge.cli_console` inside a `try/except ImportError` and calls its
`register(subparsers)`. Your module must expose exactly that function and set
`set_defaults(func=...)` on each parser it adds. If you believe you cannot work without
changing an R01 file, that is a `BLOCKED_CONTRACT`, not permission to edit it.

## Required behavior

### `console.c` — firmware side

Native USB Serial/JTAG. Preserve GPIO18/19 and `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`.

- **Framing:** UTF-8 JSON Lines. Maximum complete input line **2,048 bytes**, maximum
  response **2,048 bytes**, `u32` request ID echoed in every response. Each host write is
  at most **192 bytes**, so assemble a line incrementally and **reject overflow** rather
  than truncating silently.
- Asynchronous diagnostics are prefixed `# `; machine responses begin `{`. **Serialize
  output so a log fragment can never interleave with response JSON.** Rate-limit human
  logs to 10 lines/s. No packet hexdumps. No full configuration dump.
- Provisioning input is a length-bounded transaction that is **never logged or echoed**.

Operations, each with its stated precondition:

| Operation | Behavior |
|---|---|
| `info` | Read-only: firmware and protocol versions, full connected MAC, install state, chip and flash layout, the **full padded factory-partition SHA-256**, active round, role, last error. **No keys.** |
| `install_init` | Only in `WAIT_INSTALL` with a verified blank tail. Payload carries MAC, schema 1, installation UUID, baseline SHA-256. Initialize the named NVS partition, then write the installation header **last** as the commit. Touch no other region. |
| `configure` | Validate every bound: expected MAC, game and host IDs, HTTPS-only URL ≤192 bytes, SSID ≤32, password ≤63, token ≤256, group key **exactly 32 bytes**. Persist atomically, acknowledge **the hash only**, reboot normally. Permitted only in lobby with no pending round; active-game configuration is frozen. |
| `status` | Read-only counters, channel, queue high-water, free/minimum heap, largest block, peer ages, pending event IDs and counts. No wallet or stock-storage access. |
| `button` | Debug input only. Accepts a logical press/release and puts it **through the ordinary game rules**. It never directly changes a role. |
| `link_allowlist` | Diagnostic mode for the three-badge mesh demo. Drops received frames **before protocol handling**, never fabricates RSSI, shows a visible `DIAGNOSTIC` banner state, is **not persisted**, and an empty list restores ordinary radio. |
| `game_export` | Stream bounded pages of current and previous round evidence to the operator. Never reads stock storage. |
| `archive_clear` | Only after a receipt covers the exported and decided prior-round frontiers. Clears that game's journal and checkpoint — **never** config, the installation marker, or any stock partition. |

The `info` operation's factory-partition digest is computed over the **complete padded
factory partition**, not the ELF and not the app image alone. `badge_tool install-init`
compares it against the release manifest, so a wrong denominator silently breaks the
install gate.

### `console.py` and `cli_console.py` — tool side

`console.py` is the transport: open the selected port, write ≤192-byte chunks, assemble
response lines with the 2,048-byte bound, match request IDs, and time out cleanly. It
opens a serial port and therefore **must not be imported at `--help` time** — import it
lazily inside the handlers, exactly as R01's modules do.

`cli_console.py` registers these subcommands:

```text
install-init --recovery-id ID --port PORT --operation VERIFIED_FLASH_OPERATION_UUID
provision --recovery-id ID --port PORT --config PRIVATE_FILE --name NAME [--host]
diagnose --recovery-id ID --port PORT
game-export --recovery-id ID --port PORT --output PRIVATE_LOCAL_FILE
reset-game-storage --recovery-id ID --port PORT --export-receipt RECEIPT
```

- **`install-init` is a guarded continuation of a recorded `FLASH_VERIFIED` operation.**
  Recheck archive hashes and both copies, the expected physical MAC, the full padded
  factory-partition SHA-256 reported by the running app, blank tail and `WAIT_INSTALL`,
  and that the operation's installation UUID is unused. Then send the console
  `install_init`, read back the header and config status, and record
  `INSTALL_INITIALIZED`. A disconnect never transfers approval to another MAC or
  firmware. **Never improvise an install when no verified flash record exists.**
- **`provision`** reads a private config file — never shell arguments, which land in shell
  history. Validate that exactly one enrolled badge is the designated host. The host
  switch position alone never confers server credentials. Acknowledge configuration
  hashes without exposing values.
- **`reset-game-storage`** is explicit maintenance, never an automatic boot fallback. Take
  a fresh complete dual-read mirrored snapshot, verify the same device and the existing
  original, require a successful export or an operator record that no journal was ever
  initialized, then erase **only `0x3f0000..0x3fffff`**. Compare the full readback against
  an expected image that differs **only** in that tail being `0xff`. Return to
  `WAIT_INSTALL` and record a new guarded installation continuation. **No arbitrary offset
  and no force flag.**
- Reuse R01's `Archive`, `Operation`, locking, and gate helpers. Do not reimplement
  capture, verification, or write gating — and do not weaken them.

### `docs/console.md`

The console protocol and operator reference: framing, every operation with its
precondition and refusal, the JSON-Lines shapes, and the five subcommands. State plainly
that provisioning values are never echoed and that `link_allowlist` emulates link loss
and is **not** a range measurement.

## Review criteria

1. No file owned by R01 was modified; `register(subparsers)` matches the documented hook
   and `--help` still works with the module present.
2. Console input is bounded at 2,048 bytes with 192-byte assembly and rejects overflow
   rather than truncating; responses never interleave with logs.
3. No secret is echoed, logged, or returned — `configure` acknowledges hashes only.
4. `install_init` writes the header last, only in `WAIT_INSTALL` with a blank tail, and
   touches no other region.
5. `reset-game-storage` can erase only `0x3f0000..0x3fffff`, after a fresh verified
   snapshot and an export receipt, with a full readback comparison.
6. `link_allowlist` drops frames before protocol handling, fabricates no RSSI, and is not
   persisted.
7. No file outside the allowlist changed, and nothing was committed.
<!-- Reusable block composed into every post-F00 firmware packet. Orchestrator-owned. -->

## Source of truth

Read, in your worktree root, **before writing any code**:

- `plan.md` — the product specification, authoritative.
- `docs/protocol.md` — the frozen wire contract (for radio/game/gateway work).
- `docs/backend-contract.md` — the frozen HTTP contract (for gateway work).
- `firmware/components/zt_contract/include/*.h` — the frozen shared headers.

The headers are **read-only to you**. So are `firmware/sdkconfig.defaults`,
`firmware/partitions.csv`, `firmware/CMakeLists.txt`, `firmware/main/app_main.c`, and
every `docs/*.md`. If you cannot implement your packet correctly without changing one of
them, that is a `BLOCKED_CONTRACT`, not permission to edit it.

Your component's own `CMakeLists.txt` is yours to maintain after F00.

## Replacing scaffold stubs

Your implementation files already exist, created by F00, with real signatures and stubbed
bodies marked:

```c
/* ZT_SCAFFOLD_STUB(F00): replaced in place by packet <OWNER>. */
```

Replace those bodies **in place**. Do not create a parallel file, a second definition, or
a separate stub library. Remove the token from every body you implement; a remaining
token means unfinished work, and the orchestrator tracks them.

## Standing constraints

- **No hardware.** No serial port is opened. No device exists for you. No `idf.py flash`,
  `idf.py monitor`, `esptool`, or `erase-flash`.
- **No credentials.** No token, password, SSID, mesh key, or recovery ID appears in any
  file you write, in a log, in a comment, or in a default value.
- **No prohibited storage or security calls**, anywhere, including in comments shown as
  examples: `nvs_flash_erase()`, `nvs_flash_init()` without a partition name,
  `nvs_flash_init_partition` on a stock partition, erase or write of any stock region,
  any eFuse API, secure boot, flash encryption, anti-rollback, or download-restriction
  API.

  **Writable regions, corrected 2026-09-19 after packet G01 correctly reported that the
  earlier wording forbade a write the plan requires.** Firmware may write only the 64 KiB
  tail `0x3F0000..0x3FFFFF`, which is two distinct things:

  - `0x3F0000..0x3F0FFF` — the 4 KiB raw installation-header sector. Written **exactly
    once**, by `zt_store_install_init()`, **last**, as the initialization commit, and only
    after the whole tail was verified blank and the named NVS partition initialized
    successfully. It is never erased, never rewritten, and never touched on a later boot,
    where it is raw-read and validated **before** any NVS initialization.
  - `0x3F1000..0x3FFFFF` — the 60 KiB `zt_nvs` partition, registered at runtime.

  The application partition is written solely by `badge_tool`, never by firmware.
  Everything below `0x3F0000` is stock and is never written or erased by firmware under
  any circumstance.
- **No excluded features.** No healing, code station, sonar, NFC, accelerometer,
  positioning, AI gameplay logic, WebSocket, SSE, OTA, BLE, LVGL, full-frame buffer, or
  backend/dashboard code. Plan D05, D17, and D18 exclude them.
- **No test suite, mock server, fake device, simulator, CI config, or coverage tooling.**
  Plan D12 excludes them. Compile/link/size verification is the check.
- **No unbounded work in the hot path.** No per-frame heap allocation, no unbounded cJSON
  tree, no string concatenation, no recursive packet decoding, no linked-list growth. Use
  the fixed pools and bounds the headers declare; on overflow, count the drop and shed
  load in the order the plan specifies.
- **No blocking in gameplay.** No `vTaskDelay` spin-waits or delay loops in UI, LED, game,
  or radio service paths; every task blocks on its queue and yields between work items.
  Never disable a watchdog to mask blocking code.
- **No Git mutation.** You do not run `git add`, `git commit`, `git merge`, `git push`,
  `git rebase`, or `git reset`. Leave your work uncommitted in your worktree; the
  orchestrator reviews, stages, and commits it.
- **Stay inside your allowlist.** Another worker owns every other path and is very likely
  editing it right now. Do not read-modify-write files outside your list "just to make it
  build" — report the conflict instead.

## Allowed build command

```sh
. /Users/amitojsingh/esp/esp-idf/export.sh >/dev/null 2>&1 && cd firmware && idf.py build
```

ESP-IDF v5.5.3 lives at `/Users/amitojsingh/esp/esp-idf` and the `espressif/led_strip
3.0.3` dependency is already resolvable. Report the real result. If the sandbox denies a
write outside your worktree, report that step as `BLOCKED_ENVIRONMENT` with the exact
error, keep your source work, and continue — the orchestrator runs the authoritative
build at review. Never weaken the build, vendor a dependency, or suppress a warning class
to make it pass; fix the actual failure.

## Final report

```text
Status: READY_FOR_REVIEW | BLOCKED_CONTRACT | BLOCKED_ENVIRONMENT
Changed files:
Behavior implemented:
Build/syntax command and result:
Memory/size implications:
Known gaps and unfinished work:
Contract amendments requested:
Integration notes:
Hardware operations: NONE
```

Report honestly. A stub you did not reach, a rule you could not satisfy, and a bound you
had to exceed are all real information the orchestrator needs. Claiming completion you
did not achieve is the one failure that cannot be recovered downstream.

If the contract cannot be implemented correctly as written — a length that does not add
up, two sections that contradict, a field that cannot fit — return `BLOCKED_CONTRACT`
with the precise proposed change and your reasoning. Do not silently reinterpret the wire
format, adjust a threshold, add an endpoint, resize a queue without accounting for
memory, or reach into another worker's ownership.
