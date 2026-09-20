# Packet N01 — HTTPS bootstrap, the WSS gateway, JSON codec, and server bridge

## Goal

Implement `components/zt_gateway/`: the host-only path to the backend. HTTPS for
bootstrap and registration, **one** authenticated WSS connection for all live traffic, a
strictly bounded JSON codec, and the bridge that hands verified server decisions and
commands to the game task.

This is the only component that talks to the Internet, and it runs on the same badge that
is playing the game. Every rule below exists so that a bad network cannot slow down,
corrupt, or reset local play.

## Branch and worktree

- Branch: `firmware/N01-gateway-wss`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/N01`

## Applicable specification

`plan.md` §5 **in full** — this section was rewritten on 2026-09-19 and the framing rules
were corrected again after that. Read it now; do not work from memory of an HTTP-polling
design. Also §4.2 (clock), §4.5 (offline events and server authority), §7 (budgets),
§12's channel state machine for what you must **not** do to the radio.

`docs/backend-contract.md` is the same contract in the teammate's language. Where it and
`plan.md` §5 differ, report it — they are meant to be identical in meaning.

## Ownership boundary — read this before writing any Wi-Fi code

**You do not own Wi-Fi, the channel, or ESP-NOW.** Packet M01's `zt_radio/channel.c` is
the single radio-owner task: it owns association, the restricted same-channel scan, the
association watchdog, `WIFI_EVENT_HOME_CHANNEL_CHANGE` handling, and the controlled
ESP-NOW/Wi-Fi recovery. You consume connectivity through the frozen `zt_radio.h` API and
report your own state upward.

Concretely:

- Never call `esp_wifi_connect`, `esp_wifi_scan_start`, `esp_wifi_set_channel`,
  `esp_wifi_stop`, or `esp_now_*` from this component.
- **A socket failure is not a Wi-Fi failure.** If the STA is still associated, reconnect
  the socket with your backoff ladder and nothing else. Never trigger a scan, a channel
  change, or a radio restart because an HTTP request or a WebSocket dropped.
- When the radio owner reports the STA has gone down, tear down your TLS session and
  socket state, stop reconnecting, and wait to be told the link is back.

## Owned file allowlist

```
firmware/components/zt_gateway/CMakeLists.txt
firmware/components/zt_gateway/http.c
firmware/components/zt_gateway/ws.c
firmware/components/zt_gateway/codec.c
firmware/components/zt_gateway/bridge.c
```

Nothing else.

## Required behavior

### `http.c` — HTTPS bootstrap and registration

`esp_http_client`, HTTPS only. Certificate bundle attached, hostname verified. **Never**
set an insecure option, skip the common-name check, or follow a redirect carrying
credentials. A clock too wrong to validate a certificate is a reported error, never a
reason to disable the certificate time check.

- `GET /api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}` — authenticate, read
  lobby/round metadata, the server clock anchor, and the first snapshot page.
- `POST /api/v1/games/{game_id}/registrations` — register or rejoin, with
  `Idempotency-Key` generated **once** per logical request and reused on every retry.
- `Authorization: Bearer <host-token>` as an HTTP header. The token never appears in a
  URL, query string, log line, or error message.
- Cap body bytes while streaming, including chunked bodies. Abort an oversize or
  malformed response **without advancing any cursor**.
- Status handling exactly as §5.6: `400/422` record a schema error and stop replaying the
  identical malformed request; `401/403` → `HOST_AUTH_ERROR`, no retry loop, no token
  printed; `404` → operator setup; `409` reconcile from the structured error; `413` split
  the batch and **never truncate an event**; `429` respect `Retry-After` bounded 1–60 s;
  network and 5xx retry with backoff while the game continues locally. **A 200 with a
  malformed or incomplete body is a failure and never deletes pending events.**
- `socket_path` from bootstrap is advisory: reject any value whose scheme, host, or port
  differs from the configured HTTPS base. A server cannot redirect the badge to another
  origin.

### `ws.c` — the single WSS connection

`espressif/esp_websocket_client` 1.8.0, already pinned and resolvable. Bearer token via
header (`headers` or `esp_websocket_client_append_header`), subprotocol `zt.v1`,
`crt_bundle_attach` set, `skip_cert_common_name_check` **never** set.

**Event handler discipline — this is the rule most likely to be violated.** The handler
copies bounded bytes into the reassembly buffer, updates progress, signals the gateway
task, and returns. It **never** parses JSON, never mutates gameplay state, and never
calls `esp_websocket_client_stop`, `_close`, or `_destroy` — the component documents
those as illegal from handler context. The gateway task owns all parsing and every
lifecycle call.

**Reassembly — use the corrected §5.4 semantics.** `payload_len` and `payload_offset` in
`esp_websocket_event_data_t` are **per frame**, not per logical message:

- A frame is fully received when `payload_offset + data_len >= payload_len`.
- A logical message is an opening text frame (`op_code` `0x1`) plus zero or more
  continuation frames (`op_code` `0x0`), ending at the frame whose `fin` is set. Accept
  the message only after that final frame is fully copied — never because an earlier
  chunk carried `fin`.
- Only the first chunk of an opening text frame starts a message; later SDK chunks of the
  same frame are not new messages. Resetting per-frame progress must not reset the
  aggregate message length.
- The **4,096-byte bound is on the aggregate** across all frames of one message. The
  buffer is 4,097 bytes including the separate terminator.
- Control frames (`0x8`, `0x9`, `0xA`) may interleave and **must not disturb or reset an
  assembly in progress**. A close frame alone does not discard the assembly; a subsequent
  disconnect does.
- Discard the whole assembly, count the drop, and **advance no cursor** when the aggregate
  would exceed 4,096, a new opening text frame arrives mid-assembly, a binary frame
  arrives, or the connection drops mid-assembly.

**Connection and resume.** On `WEBSOCKET_EVENT_CONNECTED` send exactly one `hello` and
nothing else until `welcome` arrives. `last_server_id` is the last server outbox message
**durably applied**, not merely received — read it from persisted state, never from a RAM
counter. `resume:"reset"` means re-fetch a snapshot and rebuild canonical state **while
retaining every unsent and undecided local event**; it never clears the journal, restarts
the round, or changes a role.

**Timings, fixed by §5.6 and already named as constants in `zt_gateway.h`:** ping 10 s,
pong timeout 20 s, stale link at 25 s with no inbound frame of any kind, application
clock exchange every 30 s and once right after `welcome`, network timeout 5 s, send
timeout 200 ms, reconnect backoff 1/2/4/8/16/30 s capped with ±20 % jitter.
`disable_auto_reconnect` is set — **you** own the ladder, because the component's
built-in reconnect is a fixed interval with nowhere to run the resume handshake.

**Close codes** map exactly to the §5.6 table: `1000`/`1001` ordinary reconnect; `1008`
treat as auth failure and stop; `1009` count it, shrink the next batch, never resend the
oversize message; `1011` reconnect with backoff; `4001` operator setup; `4003` reconcile
from snapshot; `4010` expect `resume:"reset"`, re-fetch, retain unsent events. Upgrade
rejection `401`/`403` → `HOST_AUTH_ERROR` and stop reconnecting.

**WebSocket ping/pong does not establish server time.** Anchor the clock only from
`server_time_ms` in a bootstrap response, a `welcome`, or a `time_sync_reply`, plus half
the measured RTT of that exchange, accepted only for RTT ≤2,000 ms, with initial
uncertainty `ceil(RTT/2)+50 ms` growing at 200 ppm. Hand anchors to the game's clock
module through the frozen API; never write game state yourself.

### `codec.c` — bounded JSON

Fixed token array or an explicitly bounded parser allocator. **No unbounded cJSON tree,
no string concatenation, no recursive descent without a depth bound, no per-message heap
growth.** Encode and decode every message type in `zt_gateway.h`, with the exact field
spellings in `docs/backend-contract.md`.

64-bit IDs are **strings** in JSON, never numbers. Event IDs are exactly
`<round16>/<slot02>/<seq04>` lowercase hex. Badge IDs are 12 lowercase hex, never
truncated. Reject unknown message types, wrong field types, trailing JSON, and truncated
bodies — each advances no cursor. Respect every per-array bound: ≤8 events, ≤8 receipts,
≤8 decisions, ≤4 commands, one 8-entry snapshot page, ≤8 `need_events`. **Never assume
all maxima fit together** — the 4,096-byte total governs, so stop encoding on the byte
budget and send the remainder in the next message.

### `bridge.c` — server authority into the game

- **At most one `events` batch in flight.** On send failure or timeout the batch stays
  queued and is retried; never drop an event to make progress.
- A successful `esp_websocket_client_send_text` **acknowledges nothing**. Retain every
  event in the durable journal until the server names its ID in `receipts`, and retain
  causal evidence until `decisions` and round finalization clear it. Keep **server
  receipt**, **server decision**, and **badge application** as three distinct states.
- Persist commands and decisions durably **before** acknowledging them. Acknowledge with
  `ack`, naming the command sequence and the badge's own applied state. Expect the server
  to repeat unacknowledged commands; dedupe by command sequence.
- Snapshot assembly is bound to `snapshot_id`, `roster_hash`, and round. Never combine
  pages across revisions; a newer snapshot cancels an incomplete assembly and restarts at
  page zero. Apply only a complete, consistent assembly — **never let partial pages clear
  newer local state**.
- Ordered state-changing commands buffer up to eight, then request a snapshot. A single
  global "largest seen" is insufficient because targeted commands create gaps.
- **Backpressure never blocks gameplay.** Bound every queue, shed cosmetic traffic first,
  and let the game and radio tasks run at full speed with the socket down.

## Review criteria

1. No Wi-Fi, channel, or ESP-NOW call appears anywhere in this component, and no socket
   failure can trigger a radio action.
2. The event handler parses nothing, mutates nothing, and calls no client lifecycle
   function.
3. Reassembly treats `payload_len`/`payload_offset` as per-frame and bounds the aggregate
   at 4,096 bytes; interleaved control frames do not disturb an assembly.
4. TLS verification is never disabled and the token never reaches a URL or a log.
5. A socket send is never treated as an acknowledgement; the journal is cleared only by
   `receipts`, and causal evidence only by `decisions` and finalization.
6. JSON parsing is bounded with no heap growth per message, and every per-array and total
   byte bound is enforced.
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
