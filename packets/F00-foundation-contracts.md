# Packet F00 — foundation, shared contracts, docs, and compiling scaffold

> **Attempt 2. Read this box first.**
>
> Attempt 1 of this packet ran against a superseded transport specification and was
> stopped part-way by the orchestrator. **Its files are still in your worktree** and most
> of them are still correct: the build system, `partitions.csv`, `sdkconfig.defaults`,
> the component `CMakeLists.txt` files, `app_main.c`, `docs/protocol.md`,
> `docs/hardware.md`, `zt_common.h`, `zt_hw.h`, and `zt_ids.h` were unaffected by the
> change. Keep and extend what is right; do not start over and do not delete work that
> still matches the specification.
>
> **What changed:** `plan.md` §5 has been rewritten. The live backend transport is now a
> single authenticated **WSS** connection owned by the host badge, not HTTP polling.
> `docs/backend-contract.md` as written in attempt 1 describes the removed polling design
> and **must be replaced**. `zt_gateway.h` had not been written yet and must now describe
> the WebSocket contract. `plan.md` and `orchestrator.md` in your worktree are the
> amended versions — **re-read `plan.md` §5 in full before touching anything gateway-related.**

## Goal

Create the ESP-IDF v5.5.3 project skeleton for the Zombie Tag badge firmware, the
complete set of shared `zt_contract` headers that every later module is forbidden to
change, the three extracted specification documents, and a scaffold that actually
compiles and links for `esp32c3`. You implement **no gameplay, radio, storage, UI,
gateway, or console behavior**. You produce the contracts those modules will be
implemented against, plus stubbed implementation files in their final locations.

After this packet is reviewed and merged, every file under
`firmware/components/zt_contract/include/`, `firmware/sdkconfig.defaults`,
`firmware/partitions.csv`, `firmware/CMakeLists.txt`, `firmware/main/app_main.c`, and
the three `docs/*.md` you create become **read-only to all later workers**. Getting
them right and complete is the entire point of this packet.

## Base commit

Your worktree is already checked out at the exact base commit; run
`git rev-parse HEAD` in it to record the SHA. The orchestrator ledger
(`.orchestration/state.json`, outside your worktree) holds the authoritative
record. Branch: `firmware/F00-foundation`.

## Dependencies already merged

None. This is the first packet.

## Branch and worktree

- Branch: `firmware/F00-foundation`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/F00`

Everything you need is inside that worktree. Work only there.

## Source of truth

Read, in the worktree root, **before writing any code**:

- `plan.md` — the product specification. This is authoritative.
- `orchestrator.md` — how this work is integrated. Read §§1, 4, 5 for the rules that bind you.

Sections of `plan.md` that apply directly to you: §1 (decisions), §2 (hardware), §3
(architecture, flash layout, persistent data), §4 (game behavior), §5 (HTTP contract),
§6 (screens/controls), §7 (runtime budgets and component contracts), §8 (repository and
build deliverables), §11 (flash layout and sdkconfig requirements), §12 (the complete
wire contract), §13 (USB console interface).

Do not consult any other handoff document. Do not reintroduce healing, code stations,
sonar, NFC, positioning, AI gameplay, WebSockets, a backend, or a dashboard. Those are
excluded by decision D05, D18, and plan §8.

## Owned file allowlist

You may create or modify **only** these paths. Anything else is out of bounds.

```
firmware/CMakeLists.txt
firmware/sdkconfig.defaults
firmware/partitions.csv
firmware/dependencies.lock              (only if idf.py generates it; see build notes)
firmware/main/CMakeLists.txt
firmware/main/app_main.c
firmware/main/idf_component.yml
firmware/components/zt_contract/CMakeLists.txt
firmware/components/zt_contract/include/zt_common.h
firmware/components/zt_contract/include/zt_hw.h
firmware/components/zt_contract/include/zt_ids.h
firmware/components/zt_contract/include/zt_wire.h
firmware/components/zt_contract/include/zt_radio.h
firmware/components/zt_contract/include/zt_game.h
firmware/components/zt_contract/include/zt_store.h
firmware/components/zt_contract/include/zt_hal.h
firmware/components/zt_contract/include/zt_ui.h
firmware/components/zt_contract/include/zt_gateway.h
firmware/components/zt_contract/include/zt_console.h
firmware/components/zt_hal/CMakeLists.txt
firmware/components/zt_hal/buttons.c
firmware/components/zt_hal/lcd.c
firmware/components/zt_hal/leds.c
firmware/components/zt_ui/CMakeLists.txt
firmware/components/zt_ui/ui.c
firmware/components/zt_ui/font.c
firmware/components/zt_ui/render.c
firmware/components/zt_radio/CMakeLists.txt
firmware/components/zt_radio/radio.c
firmware/components/zt_radio/channel.c
firmware/components/zt_radio/mesh.c
firmware/components/zt_radio/wire.c
firmware/components/zt_game/CMakeLists.txt
firmware/components/zt_game/game.c
firmware/components/zt_game/clock.c
firmware/components/zt_game/peers.c
firmware/components/zt_store/CMakeLists.txt
firmware/components/zt_store/store.c
firmware/components/zt_gateway/CMakeLists.txt
firmware/components/zt_gateway/http.c
firmware/components/zt_gateway/ws.c
firmware/components/zt_gateway/codec.c
firmware/components/zt_gateway/bridge.c
firmware/components/zt_console/CMakeLists.txt
firmware/components/zt_console/console.c
docs/protocol.md
docs/backend-contract.md
docs/operations.md
docs/hardware.md
```

**Explicitly forbidden paths:** `backend/`, `frontend/`, `tools/`, `docs/recovery.md`,
`plan.md`, `orchestrator.md`, `.gitignore`, `packets/`, anything under `.orchestration/`
or `.worktrees/`. `tools/badge_tool.py` and `docs/recovery.md` belong to packet R01,
which is running at the same time as you. Do not create them.

## Required deliverables

### 1. Build system

`firmware/CMakeLists.txt`: standard ESP-IDF project file, project name `zombie_tag`,
`cmake_minimum_required(VERSION 3.16)`, `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`.

`firmware/main/idf_component.yml`: dependency manifest. Pin **exactly**, not as a caret
or tilde range (plan §8):

- `idf: "5.5.3"`
- `espressif/led_strip: "3.0.3"`
- `espressif/esp_websocket_client: "1.8.0"`

The orchestrator has already verified that `esp_websocket_client` 1.8.0 configures,
compiles, and links against ESP-IDF v5.5.3 for `esp32c3`, so this pin is known-good. Do
not widen it to a range and do not substitute another version.

`firmware/partitions.csv`: **exactly the four stock entries** from plan §3.1 / §11 and
nothing else. The runtime `zt_nvs` and installation-header regions must NOT appear here
— they are registered at runtime with `esp_partition_register_external` and are never
written into the on-flash table. Required contents, offsets and sizes exactly:

| Name | Type | SubType | Offset | Size |
|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 0x4000 |
| `phy_init` | data | phy | 0xd000 | 0x1000 |
| `factory` | app | factory | 0x10000 | 0x2A0000 |
| `storage` | data | 0x83 | 0x2B0000 | 0x140000 |

`firmware/sdkconfig.defaults` must set at least, with these exact meanings (plan §3.1,
§8, §11):

```
CONFIG_IDF_TARGET="esp32c3"
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x8000
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n
CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=n
CONFIG_ESP_PHY_ENABLE_USB=y
CONFIG_BT_ENABLED=n
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_ESP_TLS_INSECURE=n
CONFIG_WS_TRANSPORT=y
```

plus whatever else is genuinely required to build, and explicitly **no** secure boot,
flash encryption, anti-rollback, eFuse-writing, or JTAG/download-disabling options. If a
symbol above does not exist in ESP-IDF v5.5.3 under that exact name, do not invent a
replacement silently: use the real symbol and say so in your final report under
"Contract amendments requested".

Every component gets its own `CMakeLists.txt` with `idf_component_register`, correct
`SRCS`, `INCLUDE_DIRS`, and `REQUIRES`/`PRIV_REQUIRES`. `zt_contract` is headers only:
register it with empty `SRCS` and `INCLUDE_DIRS "include"`. Component dependency
direction must be acyclic: every component may require `zt_contract`; no component may
require `zt_game`, `zt_radio`, `zt_gateway`, `zt_console`, or `zt_ui` except `main`.

### 2. Shared contract headers

Every header gets `#pragma once`, includes only what it needs, is C and C++ safe
(`extern "C"` guard), and uses fixed-width types from `<stdint.h>`. No header may
allocate, define non-`static inline` functions, or declare a mutable global.

Name every constant. A later worker must never need to write a bare number that also
appears in `plan.md`.

**`zt_common.h`** — version macros `ZT_PROTOCOL_VERSION` (1), `ZT_HTTP_API_VERSION` (1),
`ZT_STORE_SCHEMA_VERSION` (1), `ZT_INSTALL_HEADER_SCHEMA` (1); a `ZT_BUILD_ID_LEN` (8)
build identifier; the `zt_err_t` result enum used across module APIs, which must include
a distinct `ZT_ERR_NOT_IMPLEMENTED`; global capacity limits from plan §7 (max players 20,
max lobby discoveries 8, RX queue 32, TX slots 24, TX critical reserve 8, event queue 32,
input edge queue 16, cached tag outcomes 16, durable journal 128, dedupe 256 entries /
30 s, pending ordered commands 8, LCD buffer bytes 20480); round duration 600000 ms;
name length bounds 1..12; and the timing constants of plan §4.2/§4.3/§12 that more than
one module needs (beacon period and jitter, EWMA numerator/denominator, sample-age
windows, tag threshold default −58, display tier thresholds, tag cooldown 3000 ms, tag
retry offsets 0/250/750 ms, tag confirm wait 1500 ms, persist timeout 500 ms,
uncertainty ceiling 2000 ms, TTL 7, max hops 8, forwarding jitter 30..120 ms).

**`zt_hw.h`** — the sanitized hardware map from plan §2 only: LCD controller/resolution/
SPI pins/mode/clock, LCD orientation call sequence as documented constants, 74HC165
button pins and the active-low logical order A, B, Home, Down, Left, Right, Up, Aux1,
Start GPIO9 (documented as also the ROM-download strap, never driven), WS2812B count 6
on GPIO3 in GRB order with the physical position map and the position sequence
`{4,3,5,2,0,1}`, the LED per-component cap 24, shared I²C pins marked **reserved, not
initialized in core**, accelerometer and NFC addresses marked **not initialized**, and
the forbidden GPIO ranges (flash 12–17, USB 18–19). Include a comment that the
accelerometer address `0x19` vs. WHO_AM_I value `0x11` conflict in the probe report is
unresolved and must not be copied into a driver.

**`zt_ids.h`** — `zt_mac_t` (6 bytes, the durable badge identity, never truncated),
`zt_game_id_t`/`zt_round_id_t` as `uint64_t`, `zt_slot_t` with `ZT_SLOT_INVALID` 255 and
`ZT_SLOT_ALL` 255 documented distinctly for their message contexts, the
`(round_id, victim_slot, event_seq)` event identity type, boot nonce type, and prototypes
for the canonical text formats: 12 lowercase hex for MAC, 16 lowercase hex for 64-bit
IDs, and the `"<round16>/<slot02>/<seq04>"` event-ID string used by the HTTP contract.

**`zt_wire.h`** — the complete ESP-NOW contract of plan §12, exhaustively:

- Envelope: magic `0x5A54`, 48-byte header, per-field byte offsets and widths as named
  constants, HMAC-SHA256 truncated to 16 bytes appended, total length
  `48 + payload_len + 16 <= 250`, so `ZT_MAX_PAYLOAD` 186.
- `zt_pkt_type_t` with every ID in the plan: BEACON 0x01, TAG_REQUEST 0x02,
  TAG_RESULT 0x03, JOIN 0x04, JOIN_RESULT 0x05, EVENT 0x10, ROSTER_PAGE 0x20,
  HOST_STATE 0x21, WATERMARKS 0x22, COMMAND 0x23, COMMAND_RECEIPT 0x24,
  SNAPSHOT_REQUEST 0x25, ROUND_CLOSED 0x26, CLOSE_RECEIPTS 0x27,
  EVENT_DECISIONS 0x28, DECISION_RECEIPT 0x29, CACHE_PAGE 0x30, WANT_EVENTS 0x31,
  EVENT_COPY 0x32, TIME_QUERY 0x33, TIME_REPLY 0x34.
- A decoded C struct per payload type with fields **in the plan's order**, each carrying
  its exact wire size as a named constant, plus a per-type named constant for the exact
  or maximum encoded payload length (BEACON 50, TAG_REQUEST 17, TAG_RESULT 44, EVENT 30,
  JOIN 39, JOIN_RESULT 16, ROSTER_PAGE ≤175, HOST_STATE ≤159, WATERMARKS ≤113,
  COMMAND ≤175 args, COMMAND_RECEIPT 12, SNAPSHOT_REQUEST 14, ROUND_CLOSED 9,
  CLOSE_RECEIPTS 8, EVENT_DECISIONS ≤125, DECISION_RECEIPT 3, CACHE_PAGE ≤180,
  WANT_EVENTS ≤52, EVENT_COPY 31, TIME_QUERY 5, TIME_REPLY 20).
- Enums, with the plan's exact numeric values: role (human 0, zombie 1, unknown 255);
  wire phase (lobby 0, prepared 1, running 2, expired_pending_sync 3, final 4); tag
  result (ACCEPTED 0 … PENDING 7); JOIN status 0–5; COMMAND kinds PREPARE_ROUND 1 …
  CANCEL_PREPARE 7 with a per-kind args struct and exact arg length; COMMAND_RECEIPT
  state 0–4 and detail 0–6; EVENT_DECISIONS status 0–2 and reason 0–7; END reason 0–2;
  CANCEL reason 0–2; BEACON flag bits 0–3; role-state entry flag bits 0–1;
  SNAPSHOT_REQUEST need_flags bits 0–3; time quality 0–1.
- Prototypes for explicit field-by-field little-endian encode/decode of the envelope and
  of every payload type, returning `zt_err_t`, taking an explicit buffer and length, and
  rejecting trailing bytes. Document in the header that a C struct is **never**
  transmitted directly.
- Prototypes and constants for the group HMAC: 32-byte key, 16-byte truncated tag over
  header+payload, constant-time comparison, and recomputation by relays after they alter
  `ttl_remaining`, `hops`, and `age_ms`.
- The `ZT_INT32_UNKNOWN` sentinel (`INT32_MIN`) for unknown signed elapsed values.

**`zt_radio.h`** — `zt_rx_frame_t` holding a **copy** of `{src_mac, rssi, channel,
rx_us, len, data[250]}` and a driver-generation counter; the rule, stated in the header,
that SDK pointers are never retained past the callback; TX priority classes and the
per-class token buckets of plan §12 (critical 8/s burst 8, flood/control 6/s burst 8,
repair 2/s burst 2, beacon 2/s burst 2, total 20/s burst 20); the one-TX-in-flight rule
and its 500 ms callback watchdog; channel-owner API (lock, discovery scan over country
-allowed channels intersected with 1..11, controlled recovery); the mesh module's
validated-domain-message publication API; and the diagnostic link-allowlist API of
plan §13 (`link_allowlist`: drops received frames before protocol handling, never
fabricates RSSI, never persisted).

**`zt_game.h`** — `zt_role_t`, `zt_phase_t` mirroring the wire values; the admission
state machine states of plan §4.1 including `WAIT_INSTALL`, `NEEDS_CONFIG`, `RECOVERING`,
`REJOINING`, and the error overlays `STORAGE_ERROR`, `RADIO_ERROR`, `HOST_AUTH_ERROR`;
an immutable `zt_ui_snapshot_t` covering everything plan §6 puts on screen (role,
remaining ms, phase, selected target, range tier, direct-contact list with name/role/
tier/age, connectivity ages, pending event count, provisional/final result and its
completeness, announcement overlay and expiry, diagnostics counters); a radar/peer entry
type; the gateway event-feed API; and the game module API. State the ownership rule in
the header: the `game` task is the **only** writer of gameplay state, and radio, HTTP,
LCD-DMA and USB callbacks never mutate it.

**`zt_store.h`** — the NVS contract of plan §3.2 and §11: partition name `zt_nvs`,
runtime offset `0x3F1000` length `0xF000`, installation header sector `0x3F0000` with
its exact 80-byte layout (offsets 0 magic `ZTIN`, 4 schema u16, 6 length u16=80, 8 MAC[6],
14 reserved u16, 16 UUID[16], 32 baseline SHA-256[32], 64 NVS offset u32, 68 NVS length
u32, 72 flags u32, 76 CRC32); key names within 15 characters (`cfg`, `round_a`, `round_b`,
`dec_a`, `dec_b`, `evt000`..`evt127`); the immutable 64-byte durable event value layout
(magic[4], schema u16, length u16=64, round_id u64, wire EVENT body[30], reserved
zero[14], CRC32 over the preceding 60 bytes); config ≤1024 bytes and checkpoint ≤2048
bytes; the capacity reserve rule; and the asynchronous persist request/completion API
with request IDs. The header must state, as comments on the API, the §3.2 rules that
`nvs_flash_erase()` is never called, that no automatic format-on-error path exists, and
that a victim stays locked in `PERSISTING` with a 500 ms `PENDING` timeout that does not
release the transition lock.

**`zt_hal.h`** — buttons publishing timestamped edges with the logical button enum and
the 10 ms poll / three-stable-sample / 400 ms-then-150 ms repeat rules as constants;
LCD accepting clipped rectangle and stripe work with the two 320×16 RGB565 DMA stripe
buffers and queue depth two; LEDs accepting exactly six capped pixels. Aux1 is a
maintained switch latched once at boot, not an edge button — state that in the header.

**`zt_ui.h`** — the UI module API: submit snapshot, screen selection (Setup, Lobby,
Prepared, Running radar, Peer list, Status, End), overlay and announcement, brightness
cap adjustment that can only lower, and the ≤10 fps / 25 Hz LED bounds. State that UI
and LED are non-blocking state machines with no delay loops.

**`zt_gateway.h`** — the host-only gateway contract of the **rewritten** `plan.md` §5.
Read §5 in full first. This header must name, as constants and types:

- **Transport split.** HTTPS (`esp_http_client`) for bootstrap and registration only;
  one WSS connection (`esp_websocket_client`) for all live traffic. State in the header
  that **no HTTP polling fallback exists** and that ordinary players allocate neither.
- **Endpoints.** `/api/v1` prefix, the bootstrap and registrations paths, and the
  WebSocket upgrade path `/api/v1/games/{game_id}/gateway/socket`. Include the rule that
  a server-advertised `socket_path` is rejected if its scheme, host, or port differs from
  the configured HTTPS base.
- **Auth and TLS.** Bearer token supplied as an HTTP header on both the HTTPS requests
  and the WebSocket upgrade, subprotocol `zt.v1`, certificate-bundle verification and
  common-name checking mandatory. State in the header, as a rule and not a suggestion,
  that the token must never appear in a URL, query string, subprotocol, or log.
- **Message types.** An enum for every client→server type (`hello`, `events`, `ack`,
  `need`, `time_sync`) and every server→client type (`welcome`, `receipts`, `decisions`,
  `commands`, `snapshot`, `need_events`, `time_sync_reply`, `error`), plus the common
  envelope fields `v`, `t`, `id`, `ts`, and the `error` code vocabulary.
- **Framing bounds.** Logical message ≤4096 bytes; a fixed 4097-byte outbound buffer and
  a fixed 4097-byte inbound reassembly buffer; ≤8 events, ≤8 receipts, ≤8 decisions,
  ≤4 commands, one 8-entry snapshot page, ≤8 `need_events`; one `events` batch in flight.
- **Reassembly state.** A type holding the in-progress assembly, built on the **corrected**
  §5.4 semantics: `payload_len` and `payload_offset` in `esp_websocket_event_data_t` are
  **per frame**, not per logical message. A frame is complete when
  `payload_offset + data_len >= payload_len`; a logical message is an opening text frame
  (`op_code` `0x1`) plus continuation frames (`op_code` `0x0`) and ends at the frame whose
  `fin` bit is set; the 4,096-byte bound applies to the **aggregate** across all frames of
  that message. Control frames (`0x8`, `0x9`, `0xA`) may interleave and must not disturb
  the assembly. Document the discard rules: aggregate over bound, a new opening text frame
  while an assembly is in progress, a binary frame, or a disconnect mid-assembly all
  discard the assembly, count the drop, and **advance no cursor**.
- **Callback rule.** State explicitly that the WebSocket event handler copies bounded
  bytes and signals the gateway task and nothing else — it never parses JSON, never
  mutates gameplay state, and never calls `esp_websocket_client_stop`, `_close`, or
  `_destroy`, which the component forbids from within the handler.
- **Resume handshake.** The `hello`/`welcome` fields, including `last_server_id` defined
  as the last server outbox message durably **applied** (not merely received), and the
  `resume` values `"ok"` and `"reset"`. Document that `"reset"` means re-fetch a snapshot
  while **retaining every unsent and undecided local event**, and never clears the
  journal, restarts the round, or changes a role.
- **Acknowledgment model.** Name the three distinct states — server receipt, server
  decision, badge application — as separate enum values or flags, and state in the header
  that a successful `esp_websocket_client_send_text` return acknowledges **nothing** and
  that queued data is retained until the matching application acknowledgment arrives.
- **Timing constants**, exactly as §5.6 fixes them: ping interval 10 s, pong timeout
  20 s, stale-link timeout 25 s with no inbound frame, application clock exchange every
  30 s and once after `welcome`, network timeout 5 s, send timeout 200 ms, reconnect
  backoff 1/2/4/8/16/30 s capped with ±20 % jitter, `disable_auto_reconnect` set so the
  gateway task owns the ladder. Add the rule that **WebSocket ping/pong does not
  establish server time** — only `server_time_ms` from bootstrap, `welcome`, or
  `time_sync_reply` anchors the clock, under the §4.2 RTT and uncertainty rules.
- **Close and error handling.** The close-code table of §5.6 (1000, 1001, 1008, 1009,
  1011, 4001, 4003, 4010) and the upgrade-rejection cases, each mapped to its firmware
  reaction.
- **Decision and command feed types** the bridge publishes to the game task, unchanged in
  meaning from the polling design.

**`zt_console.h`** — USB JSON-Lines framing of plan §13: ≤2048-byte input line, ≤2048-byte
response, u32 request ID echoed, ≤192-byte host writes assembled incrementally, `# `
prefix for asynchronous human logs, `{` for machine responses, 10 lines/s log rate limit;
and the console operation enum `info`, `install_init`, `configure`, `status`, `button`,
`link_allowlist`, `game_export`, `archive_clear` with each operation's precondition
documented.

### 3. Scaffold implementation files

Create every `.c` file in the allowlist at its **final** location with the real
function signatures from the headers and stubbed bodies that return
`ZT_ERR_NOT_IMPLEMENTED` (or are empty for `void`). Mark **every** stub body with the
exact comment token:

```c
/* ZT_SCAFFOLD_STUB(F00): replaced in place by packet <OWNER>. */
```

where `<OWNER>` is `H01` for `zt_hal`/`zt_ui`, `M01` for `zt_radio`, `G01` for
`zt_game`/`zt_store`, `N01` for `zt_gateway`, `C01` for `zt_console`. Do not create a
separate stub library or a duplicate symbol elsewhere; the owning worker replaces these
bodies in place.

`firmware/main/app_main.c` stays minimal and **must not** initialize NVS, Wi-Fi,
ESP-NOW, the LCD, or any partition. It logs the firmware and protocol versions and the
build ID and then idles. The orchestrator owns all later wiring of tasks and modules.

### 4. Documents

**`docs/protocol.md`** — the complete ESP-NOW wire contract extracted from `plan.md`
§12 (and the result codes of §4.4) so that an implementer never needs to open `plan.md`:
envelope table with exact byte offsets and widths, every packet type with its ordered
payload fields and exact or maximum length, every enum with numeric values, HMAC
coverage and the relay recompute rule, dedupe key, forwarding/TTL/jitter rules, token
buckets, proximity and timing rules, and the additional admission/close/command
encodings of §12's later tables. Byte offsets and sizes must match `zt_wire.h` exactly.

**`docs/backend-contract.md`** — **replace the attempt-1 file entirely.** It currently
describes the removed HTTP-polling design. The new document is the backend teammate's
complete specification, extracted from the rewritten `plan.md` §5 plus the ingestion,
clock-anchor, and receipt details in §12, and must be implementable on its own without
opening `plan.md`:

- The retained HTTPS endpoints (bootstrap, registrations) with exact request and response
  JSON, headers, `Idempotency-Key`, and status codes.
- The WSS upgrade endpoint, the bearer-header authentication, the required `zt.v1`
  subprotocol echo, and the explicit rule that credentials never appear in a URL.
- The one-JSON-object-per-text-frame framing rule, the 4,096-byte logical bound, and
  correct handling of fragmentation and control frames on both sides.
- The common envelope, and **every** client→server and server→client message type with a
  worked JSON example and its exact bounds.
- The `hello`/`welcome` connection and resume handshake, `last_server_id` semantics, and
  what `resume:"ok"` versus `resume:"reset"` obliges each side to do.
- The acknowledgment model stated plainly for the backend: a socket send acknowledges
  nothing; the server must durably record before sending a receipt or a decision; and
  **server receipt, server decision, and badge application stay three distinct states**.
- Heartbeat, timeout, backoff, and stale-connection values from §5.6, so the server's
  own timeouts can be chosen to match.
- The close-code table, the `error` message shape and code vocabulary, and the HTTP
  status reactions.
- Pagination and snapshot-assembly rules, the decision status and rejection reason
  vocabulary, `decision_applied`, and round-specific replay with `archived:true`.
- The server's responsibilities the firmware depends on: exactly-once patient-zero
  selection, durable command outbox with never-reused sequences, repeat of unacknowledged
  commands, and `ANNOUNCE`-only AI output filtered to 96 printable ASCII characters at
  most once per 15 s.

Add a short, clearly marked note at the top that this is a **proposed contract to
implement or jointly amend before coding**, not a description of a deployed API, and that
game creation, operator login, the dashboard, and the AI announcement generator belong to
the backend teammate and are not firmware concerns. Note the 2026-09-19 transport
revision explicitly so the teammate can see what changed.

**`docs/operations.md`** — a skeleton, not a finished runbook. Sections with real
headings and honest `TODO(orchestrator)` placeholders where a value is supplied at
execution time: private inputs required (backend base URL, game ID, host token, hotspot
SSID/password, mesh group key, host MAC, archive roots) and the explicit statement that
none of them are ever committed; build and release steps; enrollment, install, and
restore ordering; and known limitations. Do not duplicate `docs/recovery.md`, which
packet R01 owns — reference it by name instead.

**`docs/hardware.md`** — a short sanitized hardware note matching `zt_hw.h`. Pin map,
peripheral configuration, forbidden pins, the uninitialized peripherals, and the
unresolved accelerometer address/WHO_AM_I conflict. **No probe binaries, no logs, no
flash dumps, no serial captures, no credentials, no recovery IDs.**

## Read-only inputs and expected exports

`plan.md` and `orchestrator.md` are read-only references. You export the headers,
documents, and build files listed above; later packets consume them without modification.

## Limits, errors, and forbidden behavior

- No heap allocation in any header-declared hot-path API; declare bounded buffers and
  explicit lengths at the API surface.
- No feature may be added that `plan.md` excludes. No WebSocket, OTA, BLE, LVGL,
  full-frame buffer, simulator, test suite, mock server, or CI configuration.
- No code may call `nvs_flash_erase()`, `nvs_flash_init()` without a partition name,
  `esp_partition_erase_range` on a stock region, eFuse APIs, or any secure-boot or
  flash-encryption API. Not even in a stub or a comment shown as an example.
- No serial port is opened. No device is contacted. You have no hardware.
- No credentials, tokens, keys, SSIDs, passwords, or recovery IDs in any file.
- You do not run `git add`, `git commit`, `git merge`, `git push`, `git rebase`, or
  `git reset`. Leave your work **uncommitted** in the worktree. The orchestrator reviews
  and commits it.

## Allowed build command

```sh
. /Users/amitojsingh/esp/esp-idf/export.sh >/dev/null 2>&1 && cd firmware && idf.py set-target esp32c3 && idf.py build
```

ESP-IDF v5.5.3 is installed at `/Users/amitojsingh/esp/esp-idf`. Run this and report the
real result. If the sandbox denies a write outside your worktree (the component-manager
cache under `~/.cache` or `~/.espressif` is the likely one), report
`BLOCKED_ENVIRONMENT` for that step with the exact error text, keep all your source
work, and continue — the orchestrator runs the authoritative build at review. Do **not**
disable the dependency manifest, vendor a copy of `led_strip`, or weaken the build to
make it pass.

`idf.py flash`, `idf.py monitor`, `erase-flash`, and any `esptool` invocation are
prohibited.

## Review criteria

There is no test suite and you must not create one. This packet is accepted when:

1. `partitions.csv` has exactly four entries with byte-exact offsets and sizes, and no
   entry covers `0x3F0000..0x3FFFFF`.
2. Every byte offset, field width, type ID, enum value, and length constant in
   `zt_wire.h` matches `plan.md` §12 and `docs/protocol.md` with no discrepancy.
3. `zt_gateway.h` and `docs/backend-contract.md` describe the **WebSocket** transport of
   the rewritten §5 — message types, resume handshake, 4 KiB framing and reassembly,
   acknowledgment model, heartbeat/timeout/backoff values, and close codes — with no
   surviving reference to a polling sync endpoint or an HTTP fallback, and the retained
   HTTPS bootstrap/registration endpoints unchanged.
4. `sdkconfig.defaults` contains every required stock-preservation and console symbol
   and no prohibited security symbol.
5. Every scaffold stub body carries the exact `ZT_SCAFFOLD_STUB(F00)` token naming its
   owning packet, and no duplicate-symbol stub library exists.
6. `app_main.c` initializes no NVS, Wi-Fi, radio, or display.
7. The project configures and links for `esp32c3`, or the build failure is reported
   honestly with its exact error.
8. No file outside the allowlist changed, and nothing was committed.

## Final report

Reply with exactly this structure:

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

If something in `plan.md` cannot be implemented correctly as written — a length that
does not add up, a field that cannot fit, a contradiction between two sections — return
`BLOCKED_CONTRACT` with the precise proposed change and your reasoning. Do not silently
reinterpret the wire format, adjust a threshold, add an endpoint, or resize a queue to
make something fit. A contract that cannot be met is a real issue for the orchestrator
to resolve, not permission to improvise.
