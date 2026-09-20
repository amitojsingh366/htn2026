# Backend contract — v1

**Implementation status, 2026-09-20:** This document began as a proposed contract. Registration, preparation, infection synchronization and immediate server reset are implemented. LIVEG007 adds immediate provisional win results and normal round finalization after all roster evidence is decided. These additions remain experimental until manually verified on badges; source/build checks do not establish hardware behavior. Per-player scoring/ranking, operator force-finish APIs, archived-round late uploads, complete cursor-ledger recovery and other future behavior described below remain requirements rather than claims of implementation. Consult `HANDOFF.md` for the exact built/deployed release.

**LIVEG009 source update:** Gateway labels separate live WSS from HTTPS activity;
archived reset delivery gives peer badges a bounded opportunity before clearing
the host, and lobby cleanup is bound to each registration instance. Backend and
frontend changes remain local pending approved deployment. These source changes
do not establish an end-to-end badge result; `HANDOFF.md` records packaging and
deployment status.

**LIVEG010 source update:** Normal ACKs receive one bounded reply, including an
empty `commands` reply when no work is eligible. Command batches carry up to four
entries, prioritize end/results, and omit superseded role revisions. Replay has
bounded retry pacing, and each host connection restores the frozen PREPARE in its
relay cache. The LIVEG007 winner logic and these backend changes are still local;
production deployment remains pending approval. The production report of three
canonical zombies with `game_over:false` came from the older deployed Worker.

**Operator override in LIVEG005:** The user requested fresh local game state after
every reboot. Boot now clears saved round/checkpoint, event and decision data and
reset receipts, preserving provisioning and credentials. A current-boot A
registration is required before game admission. Cross-reboot retention/recovery
requirements below are superseded by this policy; an ordinary network reconnect
within the same boot still retains the current game and pending evidence.

Game creation, operator login, dashboard and AI announcement generation belong to the backend, not firmware. The **2026-09-19 transport revision** replaces live HTTP polling with one authenticated host-owned WSS connection. HTTPS handles bootstrap, registration, and explicit button-triggered host controls; no polling fallback exists.

**Framing correction accepted, 2026-09-19.** The pinned client's `payload_len` and `payload_offset` are per WebSocket frame, not per logical message. Section 5.4 defines separate frame progress and aggregate message length, including continuation frames and interleaved control frames. Evidence: [esp_websocket_client 1.8.0 receive implementation](https://github.com/espressif/esp-protocols/blob/websocket-v1.8.0/components/esp_websocket_client/esp_websocket_client.c#L995-L1029).

All sample identifiers and timestamps below are illustrative, not provisioning inputs. No usable credentials are included.

## 5.1 Transport policy

- Only the host makes network requests, and it makes exactly one live connection for the whole game of up to 20 players. Ordinary players never allocate a TLS session, an HTTP client, or a WebSocket.
- **Bootstrap, registration, and explicit host controls use HTTPS** (`esp_http_client`): verified hostname and certificate bundle, no insecure fallback, no redirect with credentials attached. Bootstrap/registration establish identity and the clock anchor; control requests explicitly prepare a lobby or reset a completed round.
- **Live traffic uses one WSS connection** (`espressif/esp_websocket_client`, pinned 1.8.0): event uploads, server receipts, event decisions, authoritative state, commands, and badge command receipts. Blocking socket work runs in the gateway task, never in the game or radio task.
- TLS verification is mandatory on both: attach the certificate bundle, keep the server common-name check enabled, and never set an insecure or skip-verification option. A clock too wrong to validate a certificate is a reported error, not a reason to disable time checks.
- `Authorization: Bearer <host-token>` is sent as an HTTP header, on the HTTPS requests and on the WebSocket upgrade request. **The token never appears in a URL, query string, subprotocol, or log.** Neither does the hotspot password, mesh key, or recovery ID.
- Sub-protocol `zt.v1` is offered on the upgrade and must be echoed by the server.
- JSON IDs for 64-bit values are strings. Numeric counters are unsigned JSON integers within their stated width. No floating-point IDs. Names and announcements are bounded printable ASCII in v1.
- HTTP path prefix and WebSocket schema version are `/api/v1` and `"v":1`. The mesh protocol version of §12 is separate and versioned independently.
- Live transport remains WSS. Alongside ping/pong and clock exchanges, the implementation sends bounded periodic application acknowledgments, including empty acknowledgments for snapshot/command/decision recovery. Each recovery response carries one bounded message so the single firmware RX mailbox can consume it before another request. There is no HTTP polling fallback.

The firmware keeps only one TLS session at a time, closing WSS while it performs
HTTPS registration or a host control request. `server_connected` and the mesh server-link flag still mean
a connected, welcomed WSS session with fresh inbound traffic. LIVEG009 displays
this as `LIVE SYNC ONLINE` or `LIVE SYNC WAIT`; successful HTTPS registration does
not set that flag. Host details separately show `REGISTERING BADGE` or
`SYNCING WITH SERVER` while HTTPS is active, `SERVER READY` for a live connection,
or `SERVER RECONNECTING` after a validated HTTP reply less than 25 seconds old
with no current gateway error. A reply counts only after complete decoding and,
for bootstrap, matching game/host identity and the allowed socket path. Existing
hotspot and specific error details remain available. `YOU ARE HOST`, winner
screens, and role LEDs retain their meanings.

## 5.2 Endpoint table

| Method and path | Caller | Purpose / limits |
|---|---|---|
| `GET /api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}` | Host, HTTPS | Authenticate designated host, obtain current lobby/round metadata, the server clock anchor, and the first snapshot page; no implicit registration or start |
| `POST /api/v1/games/{game_id}/registrations` | Host for a badge, HTTPS | Register in lobby or rejoin an existing frozen roster; idempotent by game/round + badge ID |
| `POST /api/v1/games/{game_id}/gateway/control` | Designated host firmware, authenticated HTTPS | Explicit button-triggered `start` or completed-round `reset`; bound to the current host registration and durable request identity |
| `GET /api/v1/games/{game_id}/gateway/socket` → `101 Switching Protocols` | Host, WSS | The single live connection. Upgrade only; carries no request body |
| `POST /api/v1/games/{game_id}/rounds` | Dashboard/operator, **not firmware** | Freeze current registrations, prepare round, select random patient zero exactly once, schedule start after readiness |
| `POST /api/v1/games/{game_id}/rounds/{round_id}/finish` | Dashboard/operator, **not firmware** | End/force-finalize explicitly; report missing evidence if forced |
| `POST /api/v1/games/{game_id}/reset-game` | Dashboard/operator, **not firmware** | Immediately archive the old round and clear server state to a fresh lobby; badge cleanup does not block completion |

Game creation, operator login, and AI-to-backend integration belong to the teammate. The game ID, designated host, and secret provisioning file must exist before badge enrollment. Firmware runs no HTTP server, exposes no listening socket, and needs no PUT/PATCH/DELETE endpoint.

## 5.3 Bootstrap and registration

These are unchanged from the HTTPS design and still run before the socket opens.

Bootstrap response:

```json
{
  "v": 1, "game_id": "0123456789abcdef", "host_id": "aabbccddeeff",
  "server_time_ms": 1789833600000, "phase": "lobby", "round_id": null,
  "max_players": 20, "state_rev": 1, "snapshot_id": 1, "next_page": null,
  "players": [], "rules": {"duration_ms":600000,"tag_rssi":-58,"tag_cooldown_ms":3000},
  "socket_path": "/api/v1/games/0123456789abcdef/gateway/socket"
}
```

The sample IDs and time are illustrative, not provisioned values. Active-round bootstrap additionally carries scheduled start/end UTC milliseconds, patient-zero slot, locked channel, snapshot page count, and current canonical role/cause/covered-event-frontier per player. Each player entry is `{slot,id,name,role,role_rev,cause,covered_seq}`. `cause` is null for human, otherwise `{slot,seq}`. A page contains at most eight entries and is tied to an immutable unsigned-32 snapshot ID and state revision. The wire uses `snapshot_rev` for this same snapshot ID. Pages also carry `roster_hash`, the first eight SHA-256 bytes of canonical serialized roster entries, interpreted as little-endian u64.

`socket_path` is advisory: the host may use it, but must reject any value whose scheme, host, or port differs from the configured HTTPS base. A server cannot redirect the badge to a different origin.

Registration request: `{"v":1,"badge_id":"aabbccddeeff","name":"Alex","known_round_id":null,"fw":"<build-id>"}`. Header `Idempotency-Key` is the stable request ID generated once for this request and reused on retry. Response 200/201: `{v,status:"registered"|"rejoined",slot,round_id,state_rev}`. Registration while running for an unknown badge returns 409 with `code:"REGISTRATION_CLOSED"`. Do not add it to the frozen roster. Lobby slots may change before preparation; round slots may not.

## 5.4 WebSocket message framing

Every logical message is **one WebSocket text message containing exactly one JSON object**. It consists of an opening text frame (`op_code` `0x1`) followed by zero or more continuation frames (`op_code` `0x0`), ending at the frame whose `fin` bit is set. The message completes only when that final frame has been fully copied. The **4,096-byte UTF-8 payload bound applies to the aggregate across all frames of one logical message**, in both directions; it is not a separate allowance for each frame. The terminator is allocated separately, so the firmware reassembly buffer remains exactly **4,097 bytes**. Binary frames are rejected. An over-bound message is a protocol error in whichever direction produced it.

Both sides must support fragmentation. In the pinned `esp_websocket_client` 1.8.0, a `WEBSOCKET_EVENT_DATA` event is a chunk of exactly one frame: `data_ptr`/`data_len` describes that chunk, `payload_offset` is its offset **within the frame**, `payload_len` is **that frame's total payload length**, and `fin` and `op_code` describe that frame. A frame larger than the client's receive buffer generates several events; the next frame starts a new receive cycle with offset zero. Consequently, firmware keeps per-frame expected length and copied progress separately from the aggregate message length.

- A frame is fully received when `payload_offset + data_len >= payload_len`. Accept the logical message only after the frame with `fin` set has been fully copied, never merely because an earlier chunk carries `fin`.
- Only the first chunk of an opening text frame starts a message. Further SDK chunks of that same frame are not new opening text frames. Continuation frames append to the message in progress; resetting per-frame progress must not reset the aggregate message length.
- Control frames (`0x8` close, `0x9` ping, `0xA` pong) may interleave between message fragments. The client component handles them; they never reach the JSON parser and **must not disturb or reset the data-frame progress or message assembly**. Receiving a close frame itself does not discard assembly; a subsequent disconnect does.
- Discard the whole assembly, count the drop, and **advance no cursor** if the aggregate would exceed 4,096 bytes, if a new opening text frame arrives while an assembly is in progress, if a binary frame arrives, or if the connection drops mid-assembly.
- The event handler copies bounded bytes and signals the gateway task only. It never parses JSON, mutates gameplay state, or calls `esp_websocket_client_stop`, `_close`, or `_destroy`; the component forbids those lifecycle calls in the handler. The gateway task owns parsing and lifecycle calls. A completed receive buffer remains immutable until that task releases it.

Common envelope on every message in both directions:

```json
{"v":1,"t":"<type>","id":12,"ts":1789833612345}
```

`t` is the message type. `id` is an unsigned 32-bit sequence assigned by the **sender** and monotonically increasing within one connection for client messages, and monotonically increasing **across reconnects for the life of the round** for server messages that carry durable outbox content. `ts` is the sender's millisecond clock; the host treats only `server_time_ms` in a `welcome` or `time_sync_reply` as a clock anchor.

## 5.5 Connection, resume handshake, and acknowledgment

**Opening.** After a successful bootstrap, the gateway task opens the WSS connection with the bearer header. On `WEBSOCKET_EVENT_CONNECTED` the host sends exactly one `hello` and sends nothing else until `welcome` arrives:

```json
{"v":1,"t":"hello","id":1,"ts":1789833612000,
 "host_id":"aabbccddeeff","host_boot":"0123456789abcdef",
 "game_id":"0123456789abcdef","round_id":"fedcba9876543210",
 "proto":1,"fw":"<build-id>",
 "last_server_id":41,"state_rev":7,
 "pending_events":3,"decided_through":[{"slot":2,"seq":1}]}
```

LIVEG009 may add `"registration_id":"0123456789abcdef"` to `hello`. This is the
host badge's current registration request key, encoded as a nonzero 16-character
hexadecimal string and omitted when unavailable. It identifies the registration
instance for archived lobby cleanup; a slot number alone is insufficient because
the server may reuse it after Reset Game. It does not replace `host_id`, bearer
authentication, or `host_boot`.

For a null-round `hello`, the backend finds unfinished lobby cleanup by the
host's archived `registration_id`. If the host was not itself registered in the
archived roster and has no registration ID, the backend may instead match the
`host_boot` saved from its last normal `hello`; this only recovers peer cleanup.
It does not invent a host registration or a host reset target. A finished archive
does not repeatedly resume that unregistered host's synthetic cleanup round.

`last_server_id` is the durably persisted ID of the last server outbox message the host **applied**, not merely received. The server replies:

```json
{"v":1,"t":"welcome","id":42,"ts":1789833612100,
 "server_time_ms":1789833612100,"resume":"ok",
 "phase":"running","round_id":"fedcba9876543210","state_rev":8,
 "resume_from":42,"snapshot_id":8,"snapshot_pages":3}
```

`resume` is `"ok"` when the server can replay from `last_server_id`, or `"reset"` when that cursor has expired. `"reset"` means: re-fetch a snapshot and rebuild canonical state, **while retaining every unsent and undecided local event**. A reset is never a reason to clear the journal, restart the round, or change a role.

`welcome` may also contain `"resetting":true` during an explicitly requested
operator reset. Omission means false; other value types are invalid. This flag
allows the gateway to recover reset commands and receipts while a badge is
clearing its old checkpoint. A nonzero `round_id` normally requires a nonzero
`snapshot_id` and `snapshot_pages`; zero snapshot metadata is allowed only when
`resetting` is true. The backend may use archived round/roster metadata for a
stale host's connection-scoped reset recovery while the dashboard and active game
already show a fresh lobby. `resume:"reset"` alone never authorizes the
destructive `RESET_GAME` operation.

**Acknowledgment is explicitly application-level.** A successful `esp_websocket_client_send_text` return value means bytes were handed to the transport. It is **not** an acknowledgment of anything. The firmware retains every event in its durable journal until the server names that event ID in a `receipts` message, and retains its compact causal evidence until `decisions` and round finalization clear it. These remain three distinct states, exactly as in §4.5: **server receipt** (durably ingested, possibly still `pending_dependency`), **server decision** (adjudicated), and **badge application** (the target badge committed the resulting state). No one of them implies another, and the socket implies none of them.

**Client to server:**

| `t` | Contents | Bounds |
|---|---|---|
| `hello` | resume handshake above | once per connection, first message |
| `events` | `events:[…]`, same event objects as before | ≤8 events, ≤4,096 bytes, at most one batch in flight |
| `ack` | `applied`, `decision_applied`, `ready`, `round_closed`, `presence` arrays | ≤8 entries per array, ≤4,096 bytes |
| `need` | `snapshot_page:{snapshot_id,page_index}` or `events:[event_id,…]` | ≤8 event IDs |
| `time_sync` | `nonce` u32 | at most one outstanding |
| `diagnostics` | Host-local cumulative counters, uptime and heap/stack health | Optional negotiated extension; firmware ≤1,024 bytes and ≥60 seconds between attempts; no reply |

The Sentry integration adds optional `diagnostics:true` to `welcome` when backend
telemetry is configured. Its absence or `false` disables diagnostic sends, so a
new host works with an older backend. Ordinary badges never originate or relay
diagnostics. The host uses its existing WSS connection only, after gameplay,
clock, registration, command, receipt and snapshot work.

A diagnostic carries the common envelope plus `round_id` (null in a lobby),
`host_boot`, eight-character `fw`, monotonic `seq`, `uptime_ms`, `reconnects`,
`failures`, `dropped_messages`, `send_failures`, `heap_free_bytes`,
`heap_min_free_bytes`, `heap_largest_free_bytes`, `gateway_stack_free_bytes`,
`websocket_stack_free_bytes`, and numeric `last_error`. Counters saturate at u32;
uptime is a safe JSON integer. Stack/heap values are bytes (ESP-IDF stack high-water
marks), and a zero WebSocket stack minimum means it is not yet available.
Reconnects count successful connections after the initial one within the same boot.

The backend validates the welcomed session, matching boot/round, field types,
message size, and sequence, and applies a persisted 30-second minimum interval.
Only allowlisted health fields and backend correlation tags become a Sentry log.
Rejected diagnostic samples within the generic 4,096-byte protocol bound are
silently dropped. They produce no receipts, game-state broadcasts, commands,
retry queue, or mesh traffic. See [Sentry operations](sentry.md) for budgets and
demonstration steps.

The `events` object is unchanged from the polling design:

```json
{"v":1,"t":"events","id":7,"ts":1789833612345,
 "round_id":"fedcba9876543210",
 "events":[{
   "id":"fedcba9876543210/02/0001","victim":2,"seq":1,
   "actor":0,"actor_cause":{"slot":0,"seq":0},
   "actor_role_rev":1,"victim_role_rev":1,
   "attempt":{"boot":"0102030405060708","seq":9},
   "elapsed_ms":12345,"uncertainty_ms":100,
   "actor_rssi":-49,"victim_rssi":-52}]}
```

Presence is best effort, reported for at most eight roster slots per message in rotation: `{slot,role,cause,role_rev,produced_seq,decided_seq,age_ms,via_hops}`. It is not an infection event and cannot award score or establish tag proximity. `ready` entries identify badge slot, snapshot ID, and round ID. `applied` entries identify command sequence, slot, result, and state revision. `round_closed` entries identify slot and final produced sequence. Host aggregate acknowledgment never substitutes for these per-badge acknowledgments.

**Server to client:**

| `t` | Contents | Bounds |
|---|---|---|
| `welcome` | handshake result above | once per connection |
| `receipts` | `received_events:[event_id,…]` — durably ingested | ≤8 |
| `decisions` | `decisions:[{id,status,reason,archived}]` — adjudicated | ≤8 |
| `commands` | `commands:[{seq,round_id,type,…}]` — durable outbox | ≤4 |
| `snapshot` | one roster/role page bound to `snapshot_id` and `roster_hash` | 8 entries/page |
| `need_events` | `need_events:[event_id,…]` — server requests replay | ≤8 |
| `time_sync_reply` | `nonce`, `server_time_ms` | one per request |
| `error` | `{code,detail,fatal}` | see §5.6 |

```json
{"v":1,"t":"commands","id":43,"ts":1789833612500,
 "state_rev":8,
 "commands":[{"seq":12,"round_id":"fedcba9876543210","type":"ROLE_SET",
              "target":2,"valid_until_elapsed_ms":4294967295,"role":"Z","role_rev":2,
              "cause":{"slot":2,"seq":1},"covered_seq":1}]}
```

Commands and decisions remain durable server outbox items. The host persists them before acknowledging, and acknowledges by `ack` naming the command sequence and the badge's own applied state. The server repeats unacknowledged commands. Supported decision statuses remain `accepted`, `rejected`, `pending_dependency`, with optional `archived:true` for a previous-round decision that does not alter an already finalized score; a pending result is not a final watermark. Rejection reasons remain `WRONG_ROUND`, `NOT_ROSTERED`, `INVALID_PARENT`, `OUTSIDE_ROUND`, `DUPLICATE_CONFLICT`, `STALE_ROLE`, `INVALID_PAYLOAD`. The backend must durably record a decision before sending it.

LIVEG010 replies to every valid ordinary `ack`, including ACKs carrying command
receipts. Each reply is one bounded `commands`, `decisions`, `need_events`, or
`snapshot` message. When there is no eligible work, the reply is `commands` with
the current `state_rev` and an empty `commands:[]` array. An empty reply means the
gateway should return to its idle polling interval; it is not an application
receipt for any badge. Completed archived reset cleanup may close its connection
instead. Event uploads still receive only their durable `receipts` response;
the next ACK retrieves resulting decisions, roles, or winner commands.

Normal replay rotates commands, decisions, and missing-event requests. Up to four
commands share one frame. New END_ROUND/FINAL_RESULT commands get the first replay
opportunity, and pending terminal commands lead subsequent command batches. Only
the newest ROLE_SET per target is replayed: it contains the complete canonical
role and covered-event frontier; superseded rows remain durable history. Within a
connection, repeated commands have a one-second minimum retry interval. Decision
and missing-event replay also pause for one second between completed cursor cycles,
so accelerated catch-up does not endlessly resend unchanged offline work.

After a host reconnects, the frozen PREPARE_ROUND is replayed until that connection
receives the host's `prepared_ready` or `applied` receipt, even if all original
readiness receipts were already stored. This restores the host's relay cache for
returning clients. A matching PREPARE is idempotent on a running or terminal badge:
it retains the phase, roles, timer, and result, and reports the original prepare
revision. It does not start the round again or replace Patient Zero.

Snapshot assembly rules are unchanged: pages are zero-based, up to three pages cover 20 players, an assembly is bound to snapshot ID, hash, and round, pages from different revisions are never combined, and a newer snapshot cancels an incomplete older assembly and restarts at page zero.

Command ordering is unchanged: server command sequence is unsigned 32-bit, never reused within the game; each badge persists applied critical commands with its checkpoint and returns a per-command receipt; out-of-order `ANNOUNCE` can expire and be ignored; ordered state-changing commands buffer up to eight or trigger a snapshot request. A single global "largest seen" is still insufficient because targeted commands create gaps.

## 5.6 Heartbeats, timeouts, reconnection, and errors

**Chosen and fixed for this POC**, with the reasoning recorded so the backend can match them:

| Parameter | Value | Why |
|---|---|---|
| WebSocket ping interval | 10 s | Detects a silently dead NAT path well inside one round |
| Pong timeout | 20 s | Two missed pings before the client declares the link dead |
| Application stale-link timeout | 25 s with no inbound frame of any kind | Backstop for a path that passes pongs but delivers nothing |
| Application clock exchange | every 30 s, and once immediately after `welcome` | Refreshes the server anchor and bounds uncertainty growth |
| Network operation timeout | 5 s | Matches the previous HTTPS budget |
| Send timeout | 200 ms | The gateway task never blocks longer than this on one send |
| Reconnect backoff | 1, 2, 4, 8, 16, 30 s cap, ±20 % jitter | Same ladder as the HTTPS design |

`disable_auto_reconnect` is set: the component's built-in fixed-interval reconnect has no jittered ladder and no place to run the resume handshake, so the gateway task owns reconnection explicitly.

**WebSocket ping/pong does not establish server time.** Pongs carry no server clock. The host clock anchor comes only from `server_time_ms` in a bootstrap response, a `welcome`, or a `time_sync_reply`, plus half the measured round-trip of that exchange. Initial uncertainty is `ceil(RTT/2)+50 ms`; accept an anchor only for RTT ≤2,000 ms and a sane server/round match; afterwards grow uncertainty by 200 ppm of monotonic elapsed time. All of §4.2 and §12's timing and uncertainty rules apply unchanged.

**Reconnection never disturbs gameplay.** A socket drop displays `LIVE SYNC WAIT`. It never resets the round, never clears a role, never discards a pending tag, never re-runs patient-zero selection, and never triggers a Wi-Fi channel scan on its own — only an actual STA disassociation does that, under the §12 channel rules, which are unchanged. Local ESP-NOW tagging, mesh forwarding, and offline play continue at full speed with the socket down. On reconnect the host repeats the `hello` handshake, replays every unacknowledged event from its durable journal, and re-sends outstanding receipts. Duplicate delivery is expected and harmless: events are deduplicated by event ID and commands by command sequence.

**Backpressure** is handled by refusing to grow, never by blocking gameplay. At most one `events` batch is in flight. If a send fails or times out, the batch stays queued and is retried by the gateway task. If the durable journal is full, the badge stops accepting new durable transitions and reports `SYNC REQUIRED` — it never drops an event to make room.

HTTPS errors on bootstrap and registration are unchanged: `400/422` record a schema error and stop replaying the identical malformed request; `401/403` show `HOST AUTH ERROR` with no retry loop and no token printing; `404` unknown game returns to operator setup; `409` reconciles a stale round or closed registration from the structured error; `413` splits a batch and never truncates an event; `429` respects a bounded `Retry-After` of 1–60 s; network and 5xx retry with backoff while the game continues locally. A 200 with a malformed or incomplete body is a failure and never deletes pending events.

WebSocket close and error handling:

| Condition | Firmware reaction |
|---|---|
| Upgrade rejected `401`/`403` | `HOST AUTH ERROR`; stop reconnecting; no token in any log |
| Upgrade rejected `404` | Unknown game; return to operator setup |
| Close `1000` / `1001` | Ordinary reconnect with backoff |
| Close `1008` policy violation | Treat as auth failure; stop reconnecting |
| Close `1009` message too big | Our fault: count it, shrink the next batch, never resend the oversize message |
| Close `1011` server error | Reconnect with backoff |
| Close `4001` unknown game | Return to operator setup |
| Close `4003` stale round | Reconnect and reconcile from snapshot |
| Close `4010` resume cursor expired | Reconnect, expect `resume:"reset"`, re-fetch snapshot, retain unsent events |
| TLS handshake or certificate failure | Report it; never disable verification or the certificate time check |
| Malformed or oversize inbound JSON | Drop that message, count it, advance no cursor; on repetition close with `1009` from the gateway task |
| `error` message with `fatal:true` | Stop reconnecting and show an actionable state |

An `error` message is `{"v":1,"t":"error","id":n,"ts":…,"code":"…","detail":"…","fatal":false}` with codes mirroring the HTTP vocabulary: `INVALID_PAYLOAD`, `WRONG_ROUND`, `REGISTRATION_CLOSED`, `RATE_LIMITED`, `TOO_LARGE`, `CURSOR_EXPIRED`, `INTERNAL`.

Server AI integration may still generate **`ANNOUNCE` only**. No AI credentials or model calls on the badge. The server filters and shortens output to 96 printable ASCII characters, no markup, at most one announcement per 15 s. Queue at most three; newer non-critical announcements may replace expired ones. Infection and tag feedback keep UI priority.

## Worked messages and JSON field reference

Every example below includes the common envelope. `v` is exactly 1, `id` is u32 and `ts` is unsigned millisecond time. Only `server_time_ms` in bootstrap/welcome/time_sync_reply establishes server time. All examples remain subject to the 4096-byte total limit, even when each array is within its individual limit. Extra whitespace counts toward that limit. Text is UTF-8; names and announcements are printable ASCII only. Unknown message types, malformed objects, wrong field types, trailing JSON or truncated bodies are failures and advance no cursor.

The plan gives complete examples for hello, welcome, events and ROLE_SET commands above. The following completes the worked examples. **Field spellings not explicitly fixed by §5 (notably full snapshot metadata and the remaining command bodies) are proposed below for joint review before the contract freeze.** The field widths and meanings come from the radio contract; none add gameplay behavior.

### HTTPS request and response details

Bootstrap, registration, and control require bearer authorization in the HTTP header. Use `Accept: application/json`; registration and control also use `Content-Type: application/json`, and registration uses `Idempotency-Key`. Never follow a redirect carrying credentials. Bootstrap is a GET with no body and returns 200 with the bootstrap object above. The designated host ID must match the authenticated game host. Bootstrap and registration never implicitly prepare or start a round.

Registration body:

```json
{"v":1,"badge_id":"aabbccddeeff","name":"Alex","known_round_id":null,"fw":"F00STUB0"}
```

Success (201 for a new registration, 200 for idempotent repeat or rejoin):

```json
{"v":1,"status":"registered","slot":2,"round_id":null,"state_rev":1}
```

```json
{"v":1,"status":"rejoined","slot":2,"round_id":"fedcba9876543210","state_rev":8}
```

Closed registration (409):

```json
{"v":1,"code":"REGISTRATION_CLOSED"}
```

The request ID for `Idempotency-Key` is generated once and kept on all retries. Registration is also idempotent by game/round and full badge identity. `known_round_id` is null in lobby or the existing round's 16-hex string; `fw` is eight ASCII bytes. `name` is 1–12 printable ASCII bytes. Slot is 0–19, not a permanent badge ID. The HTTP status/retry table in §5.6 applies; a valid-looking 200 without all required fields is still failure. The plan does not fix a general HTTPS error body's `detail` field; the structured `code` controls reconciliation.

### Explicit host controls

The host button uses the authenticated `/gateway/control` endpoint, separately
from the existing dashboard `/start-game` and `/reset-game` routes. It never calls
an unauthenticated dashboard route. Example lobby start:

```json
{"v":1,"action":"start","request_id":"0123456789abcdef","registration_id":"123456789abcdef0","round_id":null}
```

Both IDs are nonzero 16-character hexadecimal strings. `registration_id` is the
host's current successful registration key. `request_id` is generated once for
the button action; every retry keeps all fields unchanged. Start requires the
current lobby, at least two registered badges, and a registered host. It freezes
the roster and enters preparation; normal badge readiness still controls the
scheduled start. Because HTTPS closes WSS to maintain one TLS session, host
control Start accepts a valid channel from an authenticated host session seen
within the last 25 seconds. Expired readiness returns retryable HTTP 503
`HOST_OFFLINE`; reconnect and complete the WSS handshake before retrying.

For reset, `action` is `reset` and `round_id` names the current completed round.
An authoritative provisional winner is sufficient; FINAL_RESULT is not required.
The server clears immediately and archived badge cleanup proceeds separately.
An active round or a mismatched registration/round returns a semantic error.
The registration check also prevents a delayed first Start request from a prior
lobby from affecting a later lobby.

Success is HTTP 200:

```json
{"v":1,"accepted":true,"action":"start","request_id":"0123456789abcdef","round_id":"fedcba9876543210","state_rev":8}
```

A reset success has `action:"reset"` and `round_id:null`. The response and game
mutation commit in the same durable transaction. The newest 512 accepted or
semantic-error responses are retained across reset and replayed for identical
request IDs; changing the payload under an existing ID is rejected. A repeated
semantic error requires a fresh button request after the condition is corrected.
Transport/server errors remain retryable. Even after an old response leaves the
bounded history, registration and round guards prevent it from mutating a later
game. These controls remain local source until deployment is approved.

LIVEG009 records the successful registration key for each badge. After operator
reset, retrying an old pre-reset key returns `BAD_CONFIGURATION` instead of
repopulating the cleared lobby. A fresh A registration creates a new request
identity. Normal retries before reset continue to reuse their key.

The socket upgrade uses GET with no body and returns HTTP 101, echoing `Sec-WebSocket-Protocol: zt.v1`. It includes bearer authorization as a header, never as a query or subprotocol. A server-advertised path cannot change the configured HTTPS origin; firmware derives WSS only after this validation. HTTPS credentials are never redirected to another host or port.

### ack — client to server

Each of the five arrays has at most eight entries; all refer to the named round, and ready entries repeat that round explicitly. The entire object is at most 4096 bytes. Empty arrays are permitted. Presence rotates slots and has no scoring or proximity authority.

```json
{"v":1,"t":"ack","id":8,"ts":1789833612400,"round_id":"fedcba9876543210",
 "applied":[{"seq":12,"slot":2,"result":"applied","state_rev":8}],
 "decision_applied":[{"slot":2,"through_seq":1}],
 "ready":[{"slot":2,"snapshot_id":8,"round_id":"fedcba9876543210"}],
 "round_closed":[{"slot":2,"produced_seq":1}],
 "presence":[{"slot":2,"role":"Z","cause":{"slot":2,"seq":1},"role_rev":2,
              "produced_seq":1,"decided_seq":1,"age_ms":150,"via_hops":1}]}
```

`applied.seq` is a command u32; `slot` is 0–19. Proposed result strings map to COMMAND_RECEIPT: `received` (0), `applied` (1), `prepared_ready` (2), `rejected` (3), `requires_snapshot` (4). `state_rev`/`snapshot_id` are u32. `through_seq`/`produced_seq`/`decided_seq` and `role_rev` are u16, never wrapped. Only final accepted/rejected decisions advance `through_seq`. `role` is `H` or `Z`; human cause is null, zombie cause is `{slot,seq}`. `age_ms` is u32; `via_hops` is 0–8. A host cannot substitute its own RF-send success or aggregate bitmap for a target's durable applied receipt.

### need — client to server

Request one snapshot page, or up to eight event IDs, not both variants in the same object. Page indices are zero-based and below the advertised count (at most three).

```json
{"v":1,"t":"need","id":9,"ts":1789833612500,"round_id":"fedcba9876543210",
 "snapshot_page":{"snapshot_id":8,"page_index":1}}
```

```json
{"v":1,"t":"need","id":10,"ts":1789833612600,"round_id":"fedcba9876543210",
 "events":["fedcba9876543210/02/0001"]}
```

### time_sync — client to server

At most one outstanding request. Send immediately after welcome and every 30 seconds. `nonce` is u32; match the reply to measure the monotonic RTT.

```json
{"v":1,"t":"time_sync","id":11,"ts":1789833612700,"nonce":123}
```

### receipts — server to client

At most eight IDs in `received_events`. Emit only after durable ingestion. The event can still be pending its parent. The received watermark advances only through contiguous ingested sequences; never skip a gap.

```json
{"v":1,"t":"receipts","id":44,"ts":1789833612800,
 "received_events":["fedcba9876543210/02/0001"]}
```

### decisions — server to client

At most eight decisions. Persist before sending. Status is `accepted`, `rejected` or `pending_dependency`; reason is null/none when not rejected, otherwise one of the seven uppercase reasons in §5.5. `archived` is optional; omission means false.

```json
{"v":1,"t":"decisions","id":45,"ts":1789833612900,
 "decisions":[{"id":"fedcba9876543210/02/0001","status":"accepted","reason":null,"archived":false}]}
```

```json
{"v":1,"t":"decisions","id":46,"ts":1789833613000,
 "decisions":[{"id":"fedcba9876543210/03/0001","status":"rejected","reason":"INVALID_PARENT","archived":true}]}
```

A pending dependency is not final, does not advance a final watermark, and cannot be reported as applied final evidence. Rejected records remain immutable evidence even if gameplay state is corrected. `decision_applied` reports each origin's durably committed final frontier back to the server. Repeat decisions until that acknowledgment covers them.

### snapshot — server to client

Exactly one page per message, at most eight players. The example page has two entries; a full 20-player roster uses three pages. The page's immutable snapshot ID, round and roster hash bind assembly. `next_page` is null on the final page. Pages from distinct IDs/revisions/rounds/hashes cannot be combined. A newer snapshot cancels partial assembly and restarts at page zero. Apply only a complete consistent assembly; never use partial pages to clear newer local state.

```json
{"v":1,"t":"snapshot","id":47,"ts":1789833613100,
 "round_id":"fedcba9876543210","snapshot_id":8,"state_rev":8,
 "roster_hash":"0123456789abcdef","page_index":0,"page_count":1,"next_page":null,
 "phase":"running","start_time_ms":1789833600000,"end_time_ms":1789834200000,
 "patient_zero_slot":0,"round_channel":6,
 "rules":{"duration_ms":600000,"tag_rssi":-58,"tag_cooldown_ms":3000},
 "players":[{"slot":0,"id":"001122334455","name":"Sam","role":"Z","role_rev":1,
             "cause":{"slot":0,"seq":0},"covered_seq":0},
            {"slot":2,"id":"aabbccddeeff","name":"Alex","role":"Z","role_rev":2,
             "cause":{"slot":2,"seq":1},"covered_seq":1}]}
```

The illustrative hash is not a test vector. Compute the real hash from all frozen roster entries sorted by slot: `slot u8, MAC[6], name_len u8, name[12]` with zero padding. Take the first eight SHA-256 bytes, interpret them as little-endian u64, then use the canonical 16-hex JSON string. The radio name for `snapshot_id` is `snapshot_rev`; both are the same u32. `state_rev` is u32, `role_rev` and `covered_seq` are u16. `phase` is `lobby`, `prepared`, `running`, `expired_pending_sync`, or `final`. Round is null in lobby, otherwise nonzero u64 text. Scheduled start/end are UTC milliseconds; `end_time_ms` remains `start_time_ms + 600000` even when everyone is infected earlier.

LIVEG007 terminal snapshots also carry `result_present`, `result_final`, and
`result_complete` Booleans, `winner` (`H` or `Z` when a result is present),
`missing_slots_bitmap` (low 20 bits), and `effective_elapsed_ms` (u32). These
fields recover END_ROUND/FINAL_RESULT state during snapshot reconciliation.
`effective_elapsed_ms` identifies the actual end of play without rewriting the
scheduled deadline. A provisional winner is visible immediately and can be
corrected by subsequent authoritative evidence; `result_final` becomes true only
after all roster badges have closed and their produced event frontiers are
decided.

### need_events — server to client

Up to eight canonical event IDs. Firmware replays the requested immutable bodies, keeping active and previous round batches separate. A missing event is not permission to fabricate evidence.

```json
{"v":1,"t":"need_events","id":48,"ts":1789833613200,
 "need_events":["fedcba9876543210/02/0001"]}
```

### time_sync_reply — server to client

One reply per request, echo nonce, timestamp at response construction. Do not substitute WebSocket pong or generic envelope `ts` for this field.

```json
{"v":1,"t":"time_sync_reply","id":49,"ts":1789833613300,
 "nonce":123,"server_time_ms":1789833613300}
```

### error — server to client

`code` uses the seven values in §5.6; `fatal` is Boolean, `detail` is a string bounded by the remaining 4096-byte message budget. Detail must not contain credentials. Fatal means stop reconnecting and show actionable state.

```json
{"v":1,"t":"error","id":50,"ts":1789833613400,
 "code":"CURSOR_EXPIRED","detail":"Request a fresh snapshot; retain pending events.","fatal":false}
```

### Command field mapping

Each `commands` message contains at most four objects and is at most 4096 bytes. Every command has `seq` u32, `round_id` nonzero u64 text, `type` below, `target` (0–19 or 255 for all), and `valid_until_elapsed_ms`. The latter is 4294967295 for critical state commands and a real u32 expiry for ANNOUNCE. ROLE_SET and RESET_GAME require an individual target, never 255. Critical commands are never discarded just because their transport envelope expired.

| Type | Additional fields / exact limits | Radio kind / args bytes |
|---|---|---|
| PREPARE_ROUND | `snapshot_id` u32, `roster_hash` u64 text, `roster_count` 2–20, `duration_ms` 600000, `channel` 1–11 country-allowed, `tag_rssi` i8, `tag_cooldown_ms` 3000 | 1 / 21 |
| START_ROUND | `snapshot_id`, `roster_hash`, `patient_zero_slot`, `initial_role_rev` u16, `start_time_ms` UTC, `duration_ms` 600000; host constructs signed elapsed sample and uncertainty from its clock anchor | 2 / 25 |
| ROLE_SET | `role` H/Z, `role_rev` u16, `cause` null or `{slot,seq}`, `covered_seq` u16 | 3 / 8 |
| ANNOUNCE | `text` 1–96 printable ASCII, `valid_until_elapsed_ms` u32; server at most one/15s, badge queue three, display at most 10s | 4 / 2–97 |
| END_ROUND | `effective_elapsed_ms` u32, `reason` time_limit/all_infected/operator_stop, `winner` H/Z/null, `provisional` Boolean | 5 / 7 |
| FINAL_RESULT | `winner` H/Z/null, `complete` Boolean, `missing_slots_bitmap` low 20 bits, `state_rev` u32 | 6 / 10 |
| CANCEL_PREPARE | `reason` absent_players/operator_cancel/invalid_channel; only PREPARED | 7 / 1 |
| RESET_GAME | Explicit operator request, target 0–19; optional paired `target_mac` and `registration_id` for lobby cleanup; durable application before receipt | 8 / 0 or 14 |

For `ROLE_SET`, `role_rev` is nonzero. Human `cause` is null; a zombie cause
names the target's own slot and a sequence no greater than `covered_seq`.

An operator reset immediately archives cleanup information and exposes an empty
lobby with no saved registrations. Neither missing badges nor their receipts
block that server reset. For a frozen round, durable targeted `RESET_GAME`
commands retain its old round identity. Archived recovery delivers peer resets
before the host reset, keeping the host available to relay them through the mesh.
Each peer is offered its reset at least once, in batches of at most four. The
host reset becomes eligible when peer cleanup completes, or when all peers have
been offered cleanup and the bounded 15-second retry window has expired; an
offline peer cannot hold the host indefinitely. If the host was unregistered, the archive instead
finishes after this peer window without a host reset. This delivery window does
not delay the dashboard's fresh lobby. Bootstrap still returns the active game's
snapshot; archived cleanup is recovered through the WSS handshake.

A registered lobby has no round identity, so LIVEG009 assigns its archived
cleanup a synthetic nonzero round ID. Each lobby reset includes flat top-level
`target_mac` (12 hexadecimal MAC characters) and `registration_id` (nonzero
16-character hexadecimal registration request key). Both fields must be present
or both absent. The radio arguments are exactly 14 bytes: six MAC bytes followed
by the registration ID as little-endian u64. The badge applies this form only to
its matching current registration, preventing a delayed cleanup from clearing a
new registration that reused a slot. With both fields absent, legacy round
reset behavior and zero radio argument bytes remain unchanged.

Archived receipts affect only their cleanup commands, never the new active game.
Retries reuse the same command; the badge's persistent reset receipt makes
duplicate delivery safe after its checkpoint is cleared. Offline clients may
retain local state until they receive reset or reboot under the fresh-lobby boot
policy. This is separate from ordinary reconnect/snapshot reconciliation; it
must never be inferred from a socket failure or an expired resume cursor.

```json
{"v":1,"t":"commands","id":54,"ts":1789834201000,"state_rev":10,
 "commands":[{"seq":17,"round_id":"fedcba9876543210","type":"RESET_GAME",
              "target":2,"valid_until_elapsed_ms":4294967295}]}
```

Registration-bound lobby cleanup uses the same envelope with the extra pair:

```json
{"v":1,"t":"commands","id":55,"ts":1789834201100,"state_rev":11,
 "commands":[{"seq":18,"round_id":"1122334455667788","type":"RESET_GAME",
              "target":2,"valid_until_elapsed_ms":4294967295,
              "target_mac":"aabbccddeeff","registration_id":"0123456789abcdef"}]}
```

Examples of the remaining command kinds (split into two bounded messages):

```json
{"v":1,"t":"commands","id":51,"ts":1789833590000,"state_rev":7,
 "commands":[{"seq":10,"round_id":"fedcba9876543210","type":"PREPARE_ROUND","target":255,"valid_until_elapsed_ms":4294967295,
   "snapshot_id":8,"roster_hash":"0123456789abcdef","roster_count":2,
   "duration_ms":600000,"channel":6,"tag_rssi":-58,"tag_cooldown_ms":3000},
  {"seq":11,"round_id":"fedcba9876543210","type":"START_ROUND","target":255,"valid_until_elapsed_ms":4294967295,
   "snapshot_id":8,"roster_hash":"0123456789abcdef","patient_zero_slot":0,
   "initial_role_rev":1,"start_time_ms":1789833600000,"duration_ms":600000},
  {"seq":13,"round_id":"fedcba9876543210","type":"ANNOUNCE","target":255,
   "text":"Stay alert!","valid_until_elapsed_ms":30000}]}
```

```json
{"v":1,"t":"commands","id":52,"ts":1789834200000,"state_rev":9,
 "commands":[{"seq":14,"round_id":"fedcba9876543210","type":"END_ROUND","target":255,"valid_until_elapsed_ms":4294967295,
   "effective_elapsed_ms":600000,"reason":"time_limit","winner":"H","provisional":true}]}
```

An early canonical all-zombie roster instead produces `reason:"all_infected"`,
`winner:"Z"`, and the actual elapsed end time. END_ROUND is sent immediately;
FINAL_RESULT follows once every roster member has reported `round_closed` and
every event through those produced frontiers has a final decision:

```json
{"v":1,"t":"commands","id":53,"ts":1789834205000,"state_rev":10,
 "commands":[{"seq":15,"round_id":"fedcba9876543210","type":"FINAL_RESULT","target":255,"valid_until_elapsed_ms":4294967295,
   "winner":"H","complete":true,"missing_slots_bitmap":0,"state_rev":10}]}
```

Cancellation is a separate scenario, not something to send after the running-round examples:

```json
{"v":1,"t":"commands","id":53,"ts":1789833590000,"state_rev":7,
 "commands":[{"seq":16,"round_id":"fedcba9876543210","type":"CANCEL_PREPARE","target":255,"valid_until_elapsed_ms":4294967295,
   "reason":"absent_players"}]}
```

## Event ingestion, causal authority and durability

An event ID is exactly `<round16>/<slot02>/<seq04>` in lowercase hex; badge identity is full 12-hex MAC, never truncated. `round_id` is nonzero and never reused, slot belongs to the frozen roster, and event sequence starts at 1. Patient-zero synthetic cause alone uses sequence 0. Event JSON maps without information loss to the 30-byte EVENT payload:

| JSON field | Width / meaning |
|---|---|
| `victim`, `actor` | roster slot u8, 0–19 |
| `seq`, `actor_cause.seq` | u16; cause slot equals actor, including synthetic patient-zero root |
| `actor_role_rev`, `victim_role_rev` | u16; latter is victim's prior revision |
| `attempt.boot` | u64 text, transport boot identity |
| `attempt.seq` | u32 |
| `elapsed_ms` | u32, victim validation/RX occurrence time before storage queueing |
| `uncertainty_ms` | u16, maximum 2000 for local acceptance |
| `actor_rssi` | i8, tagger's observation of victim |
| `victim_rssi` | i8, victim's observation of tagger |

Deduplicate by event ID. Same ID with different immutable body is DUPLICATE_CONFLICT, never overwrite. Persist ingestion before receipt. Validate roster/round, hold a child PENDING_DEPENDENCY if its parent is missing and request it; transport arrival order is not gameplay order. Accept the child only if the cause is accepted and the actor was zombie in that causal state. Patient-zero selection happens exactly once for each frozen round. At most one valid human-to-zombie transition wins at a time; the victim's durably committed event identifies the winning simultaneous request.

Use the same expiry rule as the badge: `elapsed_ms + uncertainty_ms < 600000`. A crossing interval is OUTSIDE_ROUND. A storage commit may complete later without changing the captured occurrence time. Local fresh direct zombie evidence permits provisional offline chains; the server later decides causal validity. The game has no healing mechanic; an explicit rejected event may cause a canonical correction to human.

Every role correction carries a monotonically increasing revision **and covered event sequence**. Do not let a stale human snapshot erase a newer unsent local infection. Wait for the covering decision. Never wrap u16 role/event counters or u32 revisions/command sequences. Command sequence is never reused within the game, even across reconnects. Keep a durable outbox and repeat unacknowledged targeted commands. Badge critical-command receipts are checkpointed; up to eight out-of-order state commands are buffered before requesting a snapshot. ANNOUNCE may expire without blocking later state commands.

Server receipt, server decision and badge application are three independent durable states. Socket send success is none of them. Received frontiers cover contiguous durable ingestion; final frontiers cover contiguous accepted/rejected decisions, including rejections and excluding pending dependencies. The host's custody or RF transmission is never backend receipt. A badge retains its immutable local evidence through round clearance; ACK receipt can stop repeated floods without deleting causal evidence.

LIVEG007 sends a provisional END_ROUND immediately when the canonical roster is
all zombies (`all_infected`, winner `Z`) or the scheduled deadline passes with
survivors (`time_limit`, winner `H`). It enters `expired_pending_sync`; badges
stop play and show `ZOMBIES WIN` or `HUMANS WIN` with `SYNCING RESULT`. A badge
whose local deadline expires before it receives server result evidence can show
a provisional human win, subject to authoritative correction.

Each badge resolves outstanding PERSISTING operations before reporting
`round_closed` with its final produced frontier. The server remains provisional
until every roster member has closed and all events through those frontiers have
accepted or rejected decisions, including decisions for out-of-order uploads.
It then publishes FINAL_RESULT with `complete:true`, a zero missing-slot bitmap,
and phase `final`. A missing badge or pending dependency remains explicit missing
evidence, never a hidden timeout. Operator force-finalization with
`complete:false` remains a proposed API rather than part of normal LIVEG007
finalization.

Badge passive LEDs use all six pixels for steady zombie red or human blue.
Opposite-role proximity adds one to four yellow pulsing pixels while preserving
the bottom pair's role color. The electrical brightness cap is unchanged.

A new round requires finalization or explicit archival override. A badge with undecided old events refuses PREPARE unless decisions arrive or a USB export/ARCHIVE_CLEAR receipt covers them. Firmware retains at most active and immediately previous round, with 128 journal records total. Upload batches carry exactly one round and alternate previous replay with current traffic. The server continues ingesting/adjudicating old-round evidence after an override, returns `archived:true`, and never silently changes a forced-final score or current-round roles. Older backlog requires operator export, not deletion. Clear finalized compact evidence only when a receipt covers its produced frontier.

## Server clock and channel assumptions

For bootstrap, welcome and matched time_sync replies, use `server_time_ms` at response generation. Host estimates time by adding half the measured exchange RTT, initially assigning `ceil(RTT/2)+50ms` uncertainty; reject RTT over 2000ms or wrong server/round. Uncertainty grows by 200ppm of monotonic elapsed time. Ping/pong is liveness only.

Forwarded clock samples add measured queue age plus 3ms/link to elapsed; uncertainty adds 100ms per radio link. Clock frames queue locally at most 200ms and are dropped above 1500ms accumulated age or 2000ms resulting uncertainty. A rebooted badge requires a continuing same-round peer/host sample; its checkpoint never restarts ten minutes. Direct peer clock exchange requires matched nonce, RTT at most 250ms and initialized source, with added `ceil(RTT/2)+10ms` uncertainty. A learned local deadline may shorten, never extend.

Socket failure never triggers a Wi-Fi scan, clears a role, changes a channel, or restarts a round. Host remains an ordinary offline player. Only actual STA loss invokes restricted recovery on the immutable round channel. Backend service outage does not interrupt direct ESP-NOW tagging.

## Backend-only operator responsibilities

Create the game and designate one host before provisioning. Freeze a 2–20 player roster; prepare/persist/readiness acknowledgment must finish before starting. Select patient zero randomly exactly once and schedule start at least 10000ms ahead. If absent players are excluded, cancel PREPARED and issue a new round ID; never mutate a running roster. Store command outbox and event decisions durably, support resume replay, and retain old evidence after explicit forced finalization. Keep AI integration restricted to ANNOUNCE: filter to at most 96 printable ASCII characters without markup, at most one per 15 seconds. Firmware holds no AI credentials and makes no model calls.
