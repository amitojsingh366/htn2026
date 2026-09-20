# Zombie Tag — firmware implementation plan

Version 1.0 · 2026-09-19 · Target: Hack the North 2026 ESP32-C3 badge

Revision 1.1 (2026-09-19) replaces the HTTP-polling gateway sync with a single host-owned WSS connection; see §5 and the amendment note in §1.

This is an implementation specification, not a claim that firmware, radio range, or recovery has been demonstrated. No badge was accessed or modified while preparing it. `orchestrator.md` describes how another agent turns this plan into work packets and integrates Codex CLI workers. The backend belongs to a different teammate; its required interface is specified here.


Reader paths: decisions and hardware (§§1–2); game/storage/runtime (§§3–4, 6–8); backend teammate (§5 plus the receipt/clock details in §12); recovery/operator tooling (§§11, 13); exact radio protocol (§12); implementation execution (`orchestrator.md`).

## 1. Decisions and scope

User decisions supersede the older handoff. `zombie_tag_handoff_v2.md` is background; its draft protocol and backend implementation are not binding.

| ID | Decision |
|---|---|
| D01 | Native C firmware, ESP-IDF **v5.5.3**, ESP32-C3, 160 MHz. No Lua, BLE, Arduino, or ESP-WIFI-MESH. ESP-NOW broadcast plus our bounded forwarding protocol provides the mesh. |
| D02 | All badges use the same binary. The designated gateway/host is an ordinary player with exactly the same tagging rules. No medic-host or nonplaying relay badge. |
| D03 | Mobile host connects to a **2.4 GHz phone hotspot** and the backend using HTTPS. USB is for commissioning, diagnostics, and recovery; a tethered laptop bridge does not satisfy the delivered demo. |
| D04 | Core: local radar, arm's-length-intended button tagging, multihop event delivery, disconnected play, reconnection, Wi-Fi gateway, server-selected patient zero, and short AI announcements. |
| D05 | No healing in the core. Two-human medic healing is stretch. Code stations, NFC healing, sonar, positioning, AI rule changes, voice, dashboard, and backend implementation are excluded. |
| D06 | A round lasts **600,000 ms**. Zombies win if all registered players become zombies within the round. Otherwise humans win at expiry. Server finalizes the result after reconciliation. |
| D07 | Server randomly selects exactly one patient zero from the frozen round roster. Registration closes at round preparation; those players may reconnect mid-round. New players wait for the next round. |
| D08 | Local tags take effect immediately after durable storage. The server owns canonical roles, causal event acceptance, scoring, and final results. Connectivity is not needed for already-started local play. |
| D09 | Only a zombie pressing **A** initiates a tag. Both sender and victim enforce direct-neighbor proximity. RSSI is a heuristic, not proof of physical distance or direction. |
| D10 | Initial equipment: three badges. Confirmed: **20 registered players**, one host, maximum eight radio hops. This is a capacity bound, not a measured 20-player performance claim. |
| D11 | Three concurrent workers maximum, each in its own Git worktree, using Codex CLI **`gpt-6-astra`**, reasoning `high`. Only orchestrator commits/merges and accesses badges. |
| D12 | No automated tests, CI, coverage targets, simulator, or test work packets. Build/link/size checks, required recovery verification, and observing the requested demo still apply. |
| D13 | Every physical badge needs its own verified **whole-flash** original backup before any custom write. Firmware backups, provisioning secrets, and participant recovery files stay outside Git and worker inputs. |
| D14 | Confirmed recovery workflow: organizer-operated local archive, a random recovery ID, an owner backup bundle, and two verified copies on separate storage. No online backup retrieval service. |
| D15 | Confirmed: rehearse restoration once on the first badge before its first custom flash. Every subsequent badge still gets two matching full reads and its own restore-capable archive. |
| D16 | Preserve the stock bootloader, partition table, stock NVS, PHY partition, and LittleFS. Flash only the application. Reserve the unused final 64 KiB as a 4 KiB raw installation header plus 60 KiB runtime NVS, after proving it is unused. |
| D17 | Raw RGB565 stripe rendering; no LVGL and no full-frame buffer. LED brightness is capped. Accelerometer and NFC remain uninitialized because core gameplay does not need them. |
| D18 | **Revised 2026-09-19.** The live gateway transport is one authenticated **WSS** connection owned by the host badge, using `espressif/esp_websocket_client` pinned to **1.8.0**. HTTPS is retained only for bootstrap and registration. The polling sync endpoint is removed and **no HTTP polling fallback is built**. Event identities, durable queues, dedupe, command cursors, causal dependencies, and the receipt/decision/application distinction are unchanged. |

D10, D14, and D15 incorporate the user’s final answers: 20 players, organizer-managed recovery, and a first-badge restore rehearsal. Operational inputs such as actual hotspot credentials, backend URL, and backup-drive location are supplied at execution time; they are never invented or committed.

### Definitions

- **Badge ID:** full factory Wi-Fi STA MAC, six bytes; JSON uses 12 lowercase hexadecimal characters without punctuation. Do not truncate to four bytes.
- **Game ID:** server-issued unsigned 64-bit lobby identifier; JSON uses exactly 16 lowercase hex characters.
- **Round ID:** server-issued nonzero random unsigned 64-bit identifier for one round; zero means lobby traffic. Never reuse a round ID.
- **Roster slot:** 0–19, assigned by server and immutable within a round. Slots are compact wire aliases for full badge IDs, not permanent identities.
- **Event ID:** `(round_id, victim_slot, event_seq)`. `event_seq` is a persisted unsigned 16-bit counter starting at 1. The synthetic patient-zero root is `(round_id, patient_zero_slot, 0)`.
- **Server revision:** unsigned monotonic revision for canonical state. A role's revision is separate from transport sequence numbers and event sequence numbers.
- **Offline:** no path to the server. Two badges outside the host's connected component can still hear each other, tag, and carry events back later. An isolated badge cannot tag anybody it cannot directly hear.
- **Code station:** the old handoff's laptop displaying a rotating code that cures a zombie. Excluded from this version.

## 2. Evidence and hardware boundary

`probe/REPORT.md` identifies ESP32-C3 revision v0.4, 4 MB flash, disabled secure boot/encryption, and functioning ROM download access on the investigated badge. Its existing `factory-firmware.bin` copies only `0x10000..0x2affff`; it is **not** a restoration backup. Later local records show launcher/save changes, so a fresh snapshot must preserve the state present when each participant hands over their badge.

| Hardware | Exact configuration |
|---|---|
| LCD | ST7789, 320×240, RGB565; SPI2, mode 0, 40 MHz; MOSI GPIO10, SCLK GPIO1, CS GPIO2, D/C GPIO0, reset GPIO4 |
| LCD orientation | `reset`, `init`, `invert_color(true)`, `swap_xy(true)`, `mirror(true,false)`, display on; verify RGB/BGR and byte order visually during commissioning |
| Buttons | 74HC165: DATA GPIO7, LOAD GPIO20, CLK GPIO21; sample before each clock pulse; active-low order A, B, Home, Down, Left, Right, Up, Aux1 |
| Start | GPIO9, active low; also the ROM-download boot strap |
| Aux1 | Maintained side switch, latched once at boot after debounce; not an edge-triggered momentary button |
| LEDs | Six WS2812B, GPIO3, RMT, GRB order; positions 0 upper-left, 1 upper-right, 2 middle-right, 3 bottom-right, 4 bottom-left, 5 middle-left |
| Shared I²C | SDA GPIO5, SCL GPIO6; not initialized in core |
| Accelerometer | Official HAL says address `0x19`, WHO_AM_I register value `0x11`; probe's address claim appears to conflate those. Do not copy its `0x11` address into a future driver without verification. |
| NFC | MFRC522 at `0x26`; not initialized in core |
| Console | Native USB Serial/JTAG; preserve GPIO18/19 and `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |
| Power | AA cells or USB; USB does not recharge AA cells. Full-brightness LEDs are prohibited by the design. |

Never repurpose flash GPIO12–17 or USB GPIO18–19. Do not drive the Start pin. GPIO2 remains LCD CS. Do not burn eFuses, enable secure boot, flash encryption, anti-rollback, download restrictions, or disable USB/JTAG. A failed replacement application must remain recoverable through ROM download.

## 3. Application and storage architecture

```text
     mobile hotspot -> HTTPS bootstrap/registration + one WSS connection
                           ^              |
                           | host only    | pushed commands / decisions
                           v              v
buttons -> game task <-> mesh task <-> gateway task
             |               ^               |
             v               |               v
          UI snapshot    ESP-NOW radio   bounded TX queue +
                                         4 KiB WS reassembly buffer
             |               |
          LCD + LEDs     other player badges
             |
       storage requests -> persistence task -> zt_nvs (last 60 KiB)
```

The game task is the only writer of gameplay state. Radio callbacks copy metadata and bytes into fixed queues, then return. The WebSocket event handler, HTTP, LCD DMA completion, and USB callbacks never mutate game state directly; the WebSocket handler copies bounded bytes into the reassembly buffer, signals the gateway task, and returns without parsing JSON or calling any client lifecycle function. A normal player does not allocate the gateway/TLS task, the socket, or the JSON buffers.

### 3.1 Stock-compatible flash layout

| Range | Size | Policy |
|---|---:|---|
| `0x000000..0x007fff` | 32 KiB | Preserve original bootloader and leading bytes |
| `0x008000..0x008fff` | 4 KiB | Preserve original partition-table sector |
| `0x009000..0x00cfff` | 16 KiB | Preserve stock `nvs`; never initialize it |
| `0x00d000..0x00dfff` | 4 KiB | Preserve stock `phy_init`; use PHY init data compiled into our app |
| `0x00e000..0x00ffff` | 8 KiB | Preserve gap |
| `0x010000..0x2affff` | 2,752,512 bytes | Only custom application flash target |
| `0x2b0000..0x3effff` | 1,310,720 bytes | Preserve stock `storage`/LittleFS; never mount it |
| `0x3f0000..0x3fffff` | 65,536 bytes | Custom installation header (first 4 KiB) and `zt_nvs` (remaining 60 KiB), only after the enrollment tool proves this tail is unallocated and entirely `0xff` in the original snapshot |

Build with a CSV matching the four stock partition entries. Do not add the runtime partition to the on-flash table and do not flash the generated bootloader/table. At boot call `esp_partition_register_external(NULL, 0x3f1000, 0xf000, "zt_nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &part)` and initialize/open that named partition only. Check every return value. Overlap, different flash size/layout, nonblank unrecognized tail, or missing enrollment authorization is a hard storage/commissioning error, not permission to erase.

Runtime registration on the main flash is supported by the pinned ESP-IDF API. It is a software partition descriptor; it does not write the partition table. The reusable enrollment tool owns the evidence that using the tail is safe for this device.

Use `CONFIG_ESP_WIFI_NVS_ENABLED=n`, `wifi_init_config_t.nvs_enable=0`, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n`, `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=n`, and `CONFIG_ESP_PHY_ENABLE_USB=y`. Never use the customary example fallback that calls `nvs_flash_erase()` after an initialization error.

### 3.2 Persistent data

Namespace `zt` contains a versioned configuration blob, active-round checkpoint, immutable infection records, server decisions, delivery metadata, and references to the separate raw installation marker. On a blank tail, boot in USB-only `WAIT_INSTALL`, without initializing NVS or Wi-Fi. The guarded tool sends `INSTALL_INIT` only after verified flash and archive checks; firmware validates full MAC, installation UUID, and schema, initializes the named NVS partition at 0x3f1000, then writes the separate raw installation header at 0x3f0000 LAST, before accepting provisioning. Raw-read and validate that header before any NVS initialization on subsequent boots. A nonblank tail without a valid matching header never initializes automatically. Store fixed binary records with schema version, length, and CRC32; NVS provides its own integrity mechanisms as well. No secrets in logs or UI diagnostics.

- Configuration: badge name (1–12 printable ASCII characters), game ID, 32-byte mesh group key, designated host MAC, last channel, and optional host-only HTTPS URL/token/hotspot credentials. Maximum serialized config 1,024 bytes.
- Round checkpoint: round ID, frozen roster, patient-zero slot, initial rules, current canonical revision, current role and cause, local event counter, produced/finalized vectors, and pending role decisions. Maximum 2,048 bytes.
- Event journal: maximum 128 records of at most 96 bytes each; keep accepted causal evidence through round reconciliation. Events are not discarded merely because a relay or host received them.
- Capacity reserve: keep at least 16 KiB free for NVS garbage collection and atomic replacement. Live serialized values must remain below 20 KiB and allocated NVS key/page use below 40 KiB; measure with NVS stats.
- Persist config on explicit change; roster/round on admission; an infection before acknowledging it; server decisions before advertising them as applied. Never persist beacons, RSSI updates, animation frames, or every timer tick.
- NVS key names stay within 15 characters: cfg, round_a, round_b, dec_a, dec_b, evt000..evt127. Event slots are immutable until round clearance. Canonical durable event value is 64 bytes: magic[4], schema u16, length u16=64, round_id u64, wire EVENT body[30], reserved-zero[14], CRC32 over the preceding60 bytes. Derived decisions live separately; no field in that immutable event is rewritten. Active/previous round checkpoints identify their journal slots and any import/export clearance. Rebuild that index from committed event values if a checkpoint update was interrupted.

On NVS corruption/full/schema mismatch: show `STORAGE ERROR`, retain the archive and existing contents, stop accepting tags requiring new durable state, and report through USB. Never format automatically. The operator tool may clear only `zt_nvs` after exporting pending game data and taking a new complete prewrite snapshot; it must never clear stock NVS or storage.

The counter advances from committed event records; do not assume two separate NVS keys form an atomic transaction. Derive the next sequence from the retained checkpoint plus maximum committed sequence during recovery. Persist the event before the derived role checkpoint. On reboot, reconstruct the role from the latest applicable event/decision if a power failure interrupted the checkpoint update. Do not send a positive tag acknowledgment until persistence reports success. While an infection commit is outstanding, lock that victim in `PERSISTING`; do not allocate another infection sequence or accept another tag. At 500 ms without a definitive completion, return `PENDING` for this attempt and retain the transition lock. A late successful commit still applies the infection and sends its event/result. Only a definitive failure can return `BUSY` and release the lock with the victim still human. This prevents a timed-out write from later contradicting live state.

## 4. Game behavior

### 4.1 Boot and admission state machine

`BOOT -> NEEDS_CONFIG | RECOVERING -> LOBBY -> PREPARED -> RUNNING -> EXPIRED_PENDING_SYNC -> FINAL`

Errors are explicit overlays/states (`STORAGE_ERROR`, `RADIO_ERROR`, `HOST_AUTH_ERROR`); they do not silently reset the player to human.

1. Initialize USB, minimal button input, dim LEDs, LCD, and the authorized game partition. Read factory MAC; defer creating the fresh 64-bit random transport boot nonce until Wi-Fi has started and the radio entropy source is available.
2. Read configuration and any unfinished round. If missing, show the badge ID and `Configure over USB`; do not expose a provisioning access point.
3. Latch Aux1. Host mode requires both the host switch position and a config identifying this badge as the designated host with credentials. An unconfigured host switch shows an actionable error; it cannot create a second host.
4. Start Wi-Fi STA and ESP-NOW. A player discovers the configured game on allowed channels; a host joins the configured 2.4 GHz hotspot in the lobby.
5. In lobby, A requests registration. The host submits it to the server; only a server registration response admits the player. Show `REGISTERING`, then name/slot and `WAITING FOR ROUND`.
6. PREPARE freezes the roster and channel. Every badge assembles the full snapshot, persists it, and returns an application-level ready acknowledgment. The host reports the ready bitmap.
7. Server starts only with the frozen roster ready, or sends CANCEL_PREPARE for the old round and explicitly creates a new round ID/roster excluding absent players. Never silently change a running roster. Server chooses patient zero once and schedules start at least ten seconds ahead.
8. At start, set exactly that slot to zombie with synthetic cause sequence 0; all other roster slots start human. Start commands are idempotent and do not restart an already-running timer.
9. After 600,000 ms, stop initiating/accepting new tags. Resolve every outstanding PERSISTING operation before committing/sending ROUND_CLOSED with the final produced-event frontier; if storage never resolves, report the failure and do not claim a complete close. Continue beacons and event synchronization. Show a provisional local result until the server sends a final result.

No server/Internet means no new authoritative round can be created. A running round continues without the host. A registered badge returning mid-round gets its existing slot/state; an unknown badge sees `NEXT ROUND` and cannot affect current players.

### 4.2 Clock behavior

Gameplay uses monotonic `esp_timer_get_time()`, never an adjustable wall clock. Host wall time is needed for HTTPS certificate validation and matching the server's scheduled start; seed it from operator provisioning and refresh by SNTP before round preparation. Do not disable TLS certificate-time checks to work around a bad clock.

Start/host status messages contain round elapsed/remaining time; forwarding includes bounded packet age. A live badge derives its local deadline once, then may shorten it using fresher valid timing but never extend it. Normal disconnected play keeps that deadline. A full reboot has no trusted elapsed-time continuity: retain role/events, show `REJOINING`, and require a fresh same-round time/state exchange with a continuing registered peer or the host before tagging. A saved checkpoint alone cannot restart ten minutes.

Time uncertainty is recorded with events. Reject new local tags when the badge cannot establish that the round is still running. A new tag requires `estimated_elapsed_ms + uncertainty_ms < 600000`. The server uses the same engineering uncertainty-margin rule; an event interval crossing expiry is rejected as OUTSIDE_ROUND. At most the final two seconds may be unavailable for tagging under poor clock quality; the displayed round still ends at its learned deadline. Events with uncertainty above 2,000 ms require resynchronization before local play.

### 4.3 Proximity and tag selection

Only fresh **direct** beacons and the direct TAG receive RSSI influence tag range. Relayed role/event reports never create a local radar contact or proximity measurement.

- Beacon interval: 500 ms ±100 ms jitter. Peer table is the frozen roster plus bounded lobby discoveries.
- EWMA: `filtered = 0.6 * previous + 0.4 * new`, initialized from the first measurement; fixed-point arithmetic is fine.
- Track at least three samples in the last 1,500 ms. Candidate must have been heard within 1,000 ms; drop radar freshness after 4 s and retained direct-sample history after 10 s.
- Initial tag threshold: **−58 dBm**, adjustable during commissioning before a round. Require both filtered and most-recent beacon RSSI at/above threshold. Victim independently requires direct TAG RSSI and its recent sender measurements to meet the threshold.
- Display tiers: at least −58 `IN RANGE`, at least −70 `CLOSE`, at least −82 `NEARBY`, otherwise `FAR`.
- Choose the nearest eligible human by highest filtered RSSI; ties break by lower roster slot. Show the selected name before the button press. A is press-edge only; holding it does not auto-tag.
- Three-second tag cooldown starts when a valid tag attempt is transmitted. An attempt with no eligible target just shows `GET CLOSER`, with 500 ms UI-message rate limiting.

Arm's length is the intended interaction, not a guaranteed radio boundary. Body blocking, badge orientation, and walls can change RSSI. No meters, compass bearings, or exact-distance claims appear on screen.

### 4.4 Tag transaction

1. Tagger must be a running zombie with a known infection cause, local deadline not expired, cooldown clear, and an eligible direct hokuman.
2. Allocate `(tagger boot nonce, attempt sequence)`; bind victim slot, actor cause, current round, and role revisions. Send the same logical request at 0, 250, and 750 ms, each in a fresh transport envelope. Stop retries on a matching response. Wait at most 1,500 ms for confirmation.
3. Victim checks complete packet validity, game/round, roster membership, direct source MAC, matching target, actor role/cause, fresh mutual proximity, running timer, and its own human role. All checks precede state change. The actor’s causal parent event may not yet be cached: accept provisionally when its fresh authenticated direct beacon advertises the matching zombie role/cause, request the missing parent, and let the server adjudicate that dependency. Do not block offline chain infections on an Internet lookup.
4. Victim enters PERSISTING, allocates the next infection sequence, records the accepted request plus causal infection event in one immutable committed value, then changes local role to zombie and emits `TAG_RESULT(ACCEPTED,event_ref)` and an infection EVENT.
5. Send result immediately and again after 150 ms. A duplicate request returns the existing outcome/event reference without a second infection, score, write, or animation. Cache 16 recent tag outcomes for 10 s; durable infection records also prevent duplication after reboot.
6. Simultaneous tag requests are serialized by the game task. The first valid request that is durably committed wins. The loser gets `ALREADY_ZOMBIE`; it does not receive infection credit.
7. Tagger shows success only for a matching accepted result or the victim's matching infection event. With no result, show `UNCONFIRMED`; lack of ACK must not undo an infection that the victim already committed.
8. A victim that cannot persist returns `BUSY` and remains human. Invalid/late/out-of-range requests return bounded reason codes when safe; do not create response storms to malformed traffic.

Result codes: `ACCEPTED=0`, `ALREADY_ZOMBIE=1`, `OUT_OF_RANGE=2`, `ROUND_INACTIVE=3`, `STALE_ACTOR=4`, `BUSY=5`, `NOT_ROSTERED=6`, `PENDING=7`. Never relay TAG or TAG_RESULT.

### 4.5 Offline events and server authority

Core gameplay has only human-to-zombie transitions, which makes causal reconciliation considerably smaller than the handoff's healing design. An infection event identifies its victim/origin, actor, actor's infection cause, observed role revisions, attempt ID, round-relative time, uncertainty, and direct RSSI evidence. Event occurrence time is the victim’s validation/RX time, captured before queuing persistence. A commit completing after the deadline can confirm that pre-deadline infection, but cannot enable new post-deadline tagging.

The server processes a causal graph rooted at patient zero:

1. Deduplicate by event ID. Same ID/different body is a protocol conflict, never an overwrite.
2. Validate frozen roster and round. If the actor's referenced infection has not arrived, hold the event as `PENDING_DEPENDENCY`; request that event. Receiving order is not gameplay order.
3. Accept at most one valid infection of a human at a time. The victim's locally committed event determines the winning tag in the ordinary simultaneous-tag case.
4. Accept a child only if its parent cause is accepted and the actor was zombie under the relevant causal state. Reject invalid parent chains and issue explicit role corrections when necessary.
5. Return durable per-event decisions and canonical role updates. Transport receipt alone is not acceptance.
6. Role updates carry a monotonically increasing server revision **and the victim event-sequence frontier covered by the decision**. A stale human snapshot must not erase a newer unsubmitted local infection. Hold an update that does not cover that infection until the server evaluates it; then apply the explicit decision. An explicit rejected event and its dependent local state are removed from gameplay, while their records remain available for diagnosis/reconciliation.

The server may correct a player back to human after rejecting a provisional infection; this is not a healing mechanic. A subsequent legitimate infection gets a new sequence. Never wrap counters in the same round: refuse further event creation on sequence exhaustion. Maximum 128 stored event records; if full, stop accepting new durable transitions and report `SYNC REQUIRED` rather than losing events.

Every badge periodically exchanges missing-event inventories with direct peers. This allows a tag witnessed in one disconnected group to travel through another player to the host later. Keep an event until a durable server decision has propagated; keep its compact causal evidence through round finalization. Host reboot must not lose the only copy.

At ten minutes the server may have incomplete information. It publishes `provisional`, then `final` after every frozen roster member has delivered a round-close statement with its produced-event frontier and those events are decided. If a badge never returns, show missing slots and leave the result provisional until an operator explicitly finalizes with incomplete evidence. No hidden timeout silently discards offline tags. A new round can start only after finalization or an explicit operator archival override. A badge with its own undecided prior-round events refuses new PREPARE until those events receive durable decisions, or an operator exports them and installs an ARCHIVE_CLEAR receipt over USB. Keep at most the active and immediately previous round metadata, with 128 event records total across them. Host sync requests carry exactly one round ID and alternate replay batches for the old round with current traffic. The backend must still ingest old-round replay after an override; it archives/adjudicates the evidence and returns decisions with `archived:true`, without silently changing an already forced-final score or commanding current-round roles. Older backlog beyond these bounds requires operator export, never silent deletion. Finalized compact old evidence may be removed only when a clearance receipt covers its produced frontier.

## 5. Host/backend contract: HTTPS bootstrap plus a single live WebSocket

This is a proposed contract for the backend teammate to implement exactly or amend jointly before coding. It does not describe an existing deployed API. Backend/database/dashboard/AI code is outside the firmware workers' ownership.

**Transport revision, 2026-09-19 (supersedes the earlier HTTP-polling sync).** The live gateway transport is one authenticated **WSS** connection owned by the host badge. HTTPS is retained only for bootstrap and player registration. The polling `POST /gateway/sync` endpoint is removed and **no HTTP polling fallback exists in this POC**. Everything else — event identities, durable queues, dedupe, causal dependencies, command cursors, and the separation of server receipt, server decision, and badge application — is unchanged, because those are application semantics and not transport.

### 5.1 Transport policy

- Only the host makes network requests, and it makes exactly one live connection for the whole game of up to 20 players. Ordinary players never allocate a TLS session, an HTTP client, or a WebSocket.
- **Bootstrap and registration use HTTPS** (`esp_http_client`): verified hostname and certificate bundle, no insecure fallback, no redirect with credentials attached. These are short request/response exchanges that establish identity and the server clock anchor before the socket opens.
- **Live traffic uses one WSS connection** (`espressif/esp_websocket_client`, pinned 1.8.0): event uploads, server receipts, event decisions, authoritative state, commands, and badge command receipts. Blocking socket work runs in the gateway task, never in the game or radio task.
- TLS verification is mandatory on both: attach the certificate bundle, keep the server common-name check enabled, and never set an insecure or skip-verification option. A clock too wrong to validate a certificate is a reported error, not a reason to disable time checks.
- `Authorization: Bearer <host-token>` is sent as an HTTP header, on the HTTPS requests and on the WebSocket upgrade request. **The token never appears in a URL, query string, subprotocol, or log.** Neither does the hotspot password, mesh key, or recovery ID.
- Sub-protocol `zt.v1` is offered on the upgrade and must be echoed by the server.
- JSON IDs for 64-bit values are strings. Numeric counters are unsigned JSON integers within their stated width. No floating-point IDs. Names and announcements are bounded printable ASCII in v1.
- HTTP path prefix and WebSocket schema version are `/api/v1` and `"v":1`. The mesh protocol version of §12 is separate and versioned independently.
- Server messages are pushed promptly. There is no polling loop and no minimum request rate. Periodic WebSocket ping/pong and a periodic application clock exchange are the only recurring traffic.

### 5.2 Endpoint table

| Method and path | Caller | Purpose / limits |
|---|---|---|
| `GET /api/v1/games/{game_id}/gateway/bootstrap?host_id={mac}` | Host, HTTPS | Authenticate designated host, obtain current lobby/round metadata, the server clock anchor, and the first snapshot page; no implicit registration or start |
| `POST /api/v1/games/{game_id}/registrations` | Host for a badge, HTTPS | Register in lobby or rejoin an existing frozen roster; idempotent by game/round + badge ID |
| `GET /api/v1/games/{game_id}/gateway/socket` → `101 Switching Protocols` | Host, WSS | The single live connection. Upgrade only; carries no request body |
| `POST /api/v1/games/{game_id}/rounds` | Dashboard/operator, **not firmware** | Freeze current registrations, prepare round, select random patient zero exactly once, schedule start after readiness |
| `POST /api/v1/games/{game_id}/rounds/{round_id}/finish` | Dashboard/operator, **not firmware** | End/force-finalize explicitly; report missing evidence if forced |

Game creation, operator login, and AI-to-backend integration belong to the teammate. The game ID, designated host, and secret provisioning file must exist before badge enrollment. Firmware runs no HTTP server, exposes no listening socket, and needs no PUT/PATCH/DELETE endpoint.

### 5.3 Bootstrap and registration

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

### 5.4 WebSocket message framing

Every logical message is **one WebSocket text frame containing exactly one JSON object**, at most **4,096 bytes** of UTF-8 payload, with the terminator allocated separately. Binary frames are rejected. A message that exceeds the bound is a protocol error in whichever direction produced it.

Fragmentation is a transport detail that both sides must handle, and the pinned client's metadata is **per frame, not per logical message**. This was corrected on 2026-09-19 after reading `esp_websocket_client.c` 1.8.0: `esp_websocket_client_recv()` resets `payload_offset` to zero at the start of every frame, sets `payload_len` from `esp_transport_ws_get_read_payload_len()` for that frame, and its inner loop only chunks a **single** frame that exceeds `buffer_size` across several `WEBSOCKET_EVENT_DATA` events. A message split across continuation frames therefore arrives as several independent recv cycles, each restarting at offset zero. Treating `payload_len` as the whole-message length is wrong and would never complete a multi-frame message.

The firmware reassembles into a single fixed 4,097-byte buffer using these rules:

- Each `WEBSOCKET_EVENT_DATA` carries a chunk of exactly one frame: `data_ptr`/`data_len` is the chunk, `payload_offset` is its offset **within that frame**, `payload_len` is **that frame's** total payload, and `fin` and `op_code` describe **that frame**.
- A frame is fully received when `payload_offset + data_len >= payload_len`.
- A logical message is an opening text frame (`op_code` `0x1`) followed by zero or more continuation frames (`op_code` `0x0`), and ends at the frame whose `fin` bit is set. The message is complete only once that final frame has been fully copied.
- The **4,096-byte bound is on the aggregate** of all frames in one logical message, not on any single frame.
- Control frames (`0x8` close, `0x9` ping, `0xA` pong) may be interleaved between the fragments of a message. They are handled by the client component, never reach the JSON parser, and **must not disturb or reset an assembly in progress**.
- Discard the whole partial assembly, count the drop, and **advance no cursor** if the aggregate would exceed 4,096 bytes, if a new opening text frame arrives while an assembly is in progress, if a binary frame arrives, or if the connection drops mid-assembly.
- Handles control frames correctly: ping/pong/close are transport concerns handled by the client component and never reach the JSON parser. Continuation frames (`op_code` 0) belong to the message in progress.
- Never parses JSON, never touches gameplay state, and never calls `esp_websocket_client_stop`, `_close`, or `_destroy` from the event handler — the component forbids it. The handler copies bounded bytes and signals the gateway task, which owns all parsing, all state changes, and all lifecycle calls.

Common envelope on every message in both directions:

```json
{"v":1,"t":"<type>","id":12,"ts":1789833612345}
```

`t` is the message type. `id` is an unsigned 32-bit sequence assigned by the **sender** and monotonically increasing within one connection for client messages, and monotonically increasing **across reconnects for the life of the round** for server messages that carry durable outbox content. `ts` is the sender's millisecond clock; the host treats only `server_time_ms` in a `welcome` or `time_sync_reply` as a clock anchor.

### 5.5 Connection, resume handshake, and acknowledgment

**Opening.** After a successful bootstrap, the gateway task opens the WSS connection with the bearer header. On `WEBSOCKET_EVENT_CONNECTED` the host sends exactly one `hello` and sends nothing else until `welcome` arrives:

```json
{"v":1,"t":"hello","id":1,"ts":1789833612000,
 "host_id":"aabbccddeeff","host_boot":"0123456789abcdef",
 "game_id":"0123456789abcdef","round_id":"fedcba9876543210",
 "proto":1,"fw":"<build-id>",
 "last_server_id":41,"state_rev":7,
 "pending_events":3,"decided_through":[{"slot":2,"seq":1}]}
```

`last_server_id` is the durably persisted ID of the last server outbox message the host **applied**, not merely received. The server replies:

```json
{"v":1,"t":"welcome","id":42,"ts":1789833612100,
 "server_time_ms":1789833612100,"resume":"ok",
 "phase":"running","round_id":"fedcba9876543210","state_rev":8,
 "resume_from":42,"snapshot_id":8,"snapshot_pages":3}
```

`resume` is `"ok"` when the server can replay from `last_server_id`, or `"reset"` when that cursor has expired. `"reset"` means: re-fetch a snapshot and rebuild canonical state, **while retaining every unsent and undecided local event**. A reset is never a reason to clear the journal, restart the round, or change a role.

**Acknowledgment is explicitly application-level.** A successful `esp_websocket_client_send_text` return value means bytes were handed to the transport. It is **not** an acknowledgment of anything. The firmware retains every event in its durable journal until the server names that event ID in a `receipts` message, and retains its compact causal evidence until `decisions` and round finalization clear it. These remain three distinct states, exactly as in §4.5: **server receipt** (durably ingested, possibly still `pending_dependency`), **server decision** (adjudicated), and **badge application** (the target badge committed the resulting state). No one of them implies another, and the socket implies none of them.

**Client to server:**

| `t` | Contents | Bounds |
|---|---|---|
| `hello` | resume handshake above | once per connection, first message |
| `events` | `events:[…]`, same event objects as before | ≤8 events, ≤4,096 bytes, at most one batch in flight |
| `ack` | `applied`, `decision_applied`, `ready`, `round_closed`, `presence` arrays | ≤8 entries per array, ≤4,096 bytes |
| `need` | `snapshot_page:{snapshot_id,page_index}` or `events:[event_id,…]` | ≤8 event IDs |
| `time_sync` | `nonce` u32 | at most one outstanding |

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
              "target":2,"role":"Z","role_rev":2,
              "cause":{"slot":2,"seq":1},"covered_seq":1}]}
```

Commands and decisions remain durable server outbox items. The host persists them before acknowledging, and acknowledges by `ack` naming the command sequence and the badge's own applied state. The server repeats unacknowledged commands. Supported decision statuses remain `accepted`, `rejected`, `pending_dependency`, with optional `archived:true` for a previous-round decision that does not alter an already finalized score; a pending result is not a final watermark. Rejection reasons remain `WRONG_ROUND`, `NOT_ROSTERED`, `INVALID_PARENT`, `OUTSIDE_ROUND`, `DUPLICATE_CONFLICT`, `STALE_ROLE`, `INVALID_PAYLOAD`. The backend must durably record a decision before sending it.

Snapshot assembly rules are unchanged: pages are zero-based, up to three pages cover 20 players, an assembly is bound to snapshot ID, hash, and round, pages from different revisions are never combined, and a newer snapshot cancels an incomplete older assembly and restarts at page zero.

Command ordering is unchanged: server command sequence is unsigned 32-bit, never reused within the game; each badge persists applied critical commands with its checkpoint and returns a per-command receipt; out-of-order `ANNOUNCE` can expire and be ignored; ordered state-changing commands buffer up to eight or trigger a snapshot request. A single global "largest seen" is still insufficient because targeted commands create gaps.

### 5.6 Heartbeats, timeouts, reconnection, and errors

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

**Reconnection never disturbs gameplay.** A socket drop means `server offline` and nothing else. It never resets the round, never clears a role, never discards a pending tag, never re-runs patient-zero selection, and never triggers a Wi-Fi channel scan on its own — only an actual STA disassociation does that, under the §12 channel rules, which are unchanged. Local ESP-NOW tagging, mesh forwarding, and offline play continue at full speed with the socket down. On reconnect the host repeats the `hello` handshake, replays every unacknowledged event from its durable journal, and re-sends outstanding receipts. Duplicate delivery is expected and harmless: events are deduplicated by event ID and commands by command sequence.

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

## 6. Screens, controls, and feedback

Raw stripe renderer, dark background, large role name, readable timer, and clear connectivity. Radar angles are stable hashes of badge ID for layout only. Put `PROXIMITY · NOT DIRECTION` under the radar. All radar contacts must be direct observations.

| Screen | Contents |
|---|---|
| Setup | Badge short ID, firmware build, `Configure over USB`, host configuration errors |
| Lobby | Player name, registration state, player count, host/server link status, A to register |
| Prepared | Roster ready, countdown, network/channel status; no tagging |
| Running radar | Left 160×180 radar; right role, remaining mm:ss, nearest eligible target, range tier, direct contacts, host/server status |
| Peer list | Fresh direct neighbors sorted by RSSI, name/role/tier; up/down scroll |
| Status | Channel, role revision, pending events, direct peers, host age, server age, battery-power guidance; never show secrets |
| End | `TIME UP`, provisional/final winner, pending event count, reconnect instruction if needed |

A tags only as zombie during a live round; as human shows `STAY CLEAR`; in lobby registers. B toggles radar/list. D-pad scrolls lists; left/right changes diagnostics page. Start toggles a status overlay. Home closes overlays; hold Home for two seconds opens a status/settings menu with brightness but **no role reset, round reset, or erase action**. AUX1 changes host selection on next reboot only, with a visible notice if toggled live. No button combination changes patient zero.

Poll buttons every 10 ms; require three stable samples (30 ms) for edges. D-pad repeat after 400 ms then every 150 ms; never repeat A. Sample maintained switch position at startup after stabilization.

LED output at 25 Hz maximum. Clamp every RGB component to 24/255 and total output conservatively; brightness menu can only lower the cap. Idle human bottom pair blue, zombie bottom pair green. Human red threat fill and zombie yellow-green prey fill follow `{4,3,5,2,0,1}`. Closer tier means faster pulse. Tag confirmed: green chase for 600 ms. Newly infected: red pulses for 1,200 ms. Unconfirmed/missed attempt: dim red for 300 ms. UI and LEDs are nonblocking state machines; no `delay()` loops in gameplay. No immunity/medic/cure animations in core.

Display updates at most 10 fps, dirty regions where practical. Use two 320×16 RGB565 internal-DMA buffers (20,480 bytes total), never one or two full screens. Wait for SPI transfer completion before reusing each stripe. LCD queue depth two. Compile a small ASCII bitmap font into flash; use integer drawing/clipping and no per-frame heap allocation.

No battery voltage reading is promised: no verified battery ADC pin exists in the supplied evidence. Show no invented battery percentage. USB/hotspot power guidance belongs in the operator instructions.

## 7. Runtime budgets and component contracts

These are implementation ceilings and acceptance targets, not measured results. Record actual static size, task high-water marks, free heap, largest block, and minimum heap on the first host badge.

| Task | Priority / stack bytes | Ownership |
|---|---|---|
| `input` | 8 / 3,072 | 100 Hz button sampling, enqueue edges |
| `radio_mesh` | 7 / 6,144 | Consume RX copies, authenticate/parse, dedupe, relay scheduling, one TX in flight |
| `game` | 6 / 6,144 | Sole gameplay state owner, timers, tag checks, immutable snapshots |
| `persist` | 5 / 4,096 | Serialized NVS requests; completion messages |
| `gateway` (host only) | 4 / 12,288 | STA supervisor, HTTPS bootstrap/registration, WSS connection lifecycle and resume handshake, bounded JSON parser, command delivery. Stack raised from 10,240 for the TLS session plus the websocket client's own task context. |
| `ui` | 3 / 4,096 | Render snapshot, LCD DMA, LED animations |
| `console` | 2 / 4,096 | USB diagnostics/provisioning, bounded input parser |

Task stacks are ESP-IDF byte units. Driver/Wi-Fi/lwIP/IDF tasks are additional. Avoid artificial core affinity on this single-core device. Every task blocks/yields between work and has bounded service per iteration. Use watchdogs normally; never disable watchdogs to mask blocking code.

| Resource | Bound |
|---|---:|
| Frozen roster / direct peers | 20 / 20 |
| Extra lobby discoveries | 8, expire after 10 s |
| Radio RX queue | 32 × 288-byte slots (includes copied frame/metadata) |
| TX scheduling slots | 24 × 288-byte slots; critical reserve 8 |
| Gameplay event queue | 32 × 96-byte records |
| Input edges | 16; coalesce directional repeat before losing press/release |
| Tag transaction | One outbound, 16 cached inbound outcomes |
| Durable event journal | 128 × <=96 bytes |
| Dedupe | 256 entries, 30 s lifetime, bounded replacement |
| Pending ordered commands | 8; overflow requests snapshot |
| LCD buffers | 20,480 bytes, DMA-capable |
| HTTP/WS JSON | Host-only 4,097-byte TX buffer and 4,097-byte WS reassembly buffer, plus fixed parser tokens. One `events` batch in flight; inbound logical messages over 4,096 bytes are dropped without advancing a cursor. |

Avoid double-copying queues into equally large staging tables. Static application pools excluding stacks/LCD must stay under 64 KiB. Target at least 64 KiB free heap and a 32 KiB largest free block before initial TLS; at least 24 KiB minimum free internal heap during steady connected host play. If measurement fails, first reduce noncritical telemetry/queue duplication, then reduce LCD stripe height; do not remove event persistence, backup gates, TLS validation, or Wi-Fi to make an apparent demo pass. Keep normal mbedTLS record compatibility; do not shrink TLS receive records below server requirements without negotiated support.

`hal_buttons` publishes timestamped edges; `hal_lcd` takes clipped rectangle/stripe work; `hal_leds` takes six capped pixels; `radio` publishes a copied `{src_mac,rssi,channel,rx_us,len,data}`; `mesh` publishes validated domain messages; `game` exposes an immutable UI snapshot and gateway event feed; `persist` reports durable success/failure with request ID; `host_bridge` publishes verified server decisions. Freeze these C headers before assigning implementations.

Heap allocation is allowed at startup, driver/TLS lifecycle, and bounded provisioning, not every beacon/frame. JSON parsing uses a fixed token array or an explicitly bounded parser allocator. No unbounded cJSON tree, string concatenation, recursive packet decoding, or linked-list growth in the hot path.

## 8. Repository and build deliverables

```text
firmware/
  CMakeLists.txt  sdkconfig.defaults  partitions.csv  dependencies.lock
  main/app_main.c
  components/zt_contract/include/zt_*.h
  components/zt_hal/{buttons,lcd,leds}.c
  components/zt_radio/{radio,channel,mesh,wire}.c
  components/zt_game/{game,clock,peers}.c
  components/zt_store/store.c
  components/zt_ui/{ui,font,render}.c
  components/zt_gateway/{http,ws,codec,bridge}.c
  components/zt_console/console.c
tools/
  badge_tool.py  requirements.txt
docs/
  protocol.md  backend-contract.md  operations.md
packets/                    # generated by the future orchestrator
plan.md  orchestrator.md
.orchestration/             # ignored worker ledger/logs
.worktrees/                 # ignored isolated worker checkouts
```

No backend project, dashboard project, CI directory, test directory, or custom cloud service. The backend contract extracted into `docs/backend-contract.md` must agree byte-for-byte in meaning with this plan. The orchestrator owns all shared headers, codecs' schema definitions, build entry points, sdkconfig, and `app_main.c` integration.

Pin ESP-IDF v5.5.3, **espressif/led_strip 3.0.3**, and **espressif/esp_websocket_client 1.8.0** exactly (not caret ranges) in the dependency manifest and `dependencies.lock`; IDF supplies `esp_lcd`, ESP-NOW, Wi-Fi, NVS, HTTP, and esp-tls. Version 1.8.0 was verified to configure, compile, and link against ESP-IDF v5.5.3 for `esp32c3` before being pinned. Pin recovery Python dependencies separately (esptool 5.4.0 verified during planning). Save tool versions in the release manifest. Compiler warnings matter; fix actual failures rather than suppressing them indiscriminately.

Required build settings: ESP32-C3 target, 160 MHz, 4 MB DIO 80 MHz flash, USB Serial/JTAG console, Bluetooth off, Wi-Fi enabled, no secure boot/encryption/eFuse-writing features, named custom storage only. Build-generated bootloader/table artifacts are not deployment targets. Release artifacts include app binary, ELF/map, build manifest, hashes, exact permitted write range, protocol/API versions, sanitized sdkconfig, and source commit.

Only the recovery-aware `badge_tool flash-game` path deploys an app. `idf.py flash`, `erase-flash`, merged full-flash binaries, generic web flashers, OTA, and ad hoc esptool write commands are not implementation shortcuts.

## 9. Delivery milestones and observations

1. Freeze schemas/interfaces and generate work packets. Implement the recovery tool before any badge write. Produce a compiled release candidate with no hardware access by workers.
2. On first badge: make verified complete backup(s), run selected restoration rehearsal, then gated app-only flash; confirm ordinary boot, display, button mapping, USB console, modest LEDs, and protected stock regions.
3. On second badge: independent backup/flash; observe beacons, direct RSSI, A-triggered tagging, accepted/missed feedback, and duplicate suppression by counters/logs.
4. On three badges: demonstrate A→B→C forwarding with a controlled **diagnostic link allowlist** that drops specified RX links before protocol handling. Keep direct proximity/tagging subject to real received frames; mark this clearly as emulated link loss, not a physical range measurement. Also observe an ordinary physical separation demo where feasible.
5. Demonstrate an already-registered pair tagging while the host path is unavailable, then reconnecting and showing the server's accepted causal events. Verify a newly joining identity is refused until the next round.
6. Demonstrate mobile hotspot hosting, server-chosen patient zero, ten-minute expiry, one announcement, and event replay after a host reboot/rejoin. Three physical badges demonstrate mechanisms, not proven capacity for 20.
7. Give each participant a recovery receipt/bundle and record which backup belongs to which physical device. Restore any badge through the same guarded tool when requested.

These are short operator observations of the requested product and recovery behavior, not an automated test suite. No fabricated result or unperformed hardware check may be marked complete. If the backend is unavailable, deliver compiled firmware and exact contract, explicitly mark end-to-end networking unobserved, and keep local gameplay progress independent.

## 10. Remaining operational inputs and known limits

- Backend teammate supplies the HTTPS base URL (which also determines the WSS origin), the WebSocket upgrade path, game ID, designated host token, operator start controls, and agreed contract. Operator supplies hotspot credentials, host MAC, names, two backup locations, and participant-device access.
- A 2.4 GHz hotspot is mandatory; 5 GHz-only is incompatible. Hotspot channel is pinned for the round; reconnect policy appears in the radio specification below.
- RSSI cannot promise arm's-length-only tags, nor direction. On-site tuning is part of commissioning, not a claimed solved positioning system.
- Offline outcomes are provisional until reconciled; permanent loss of all copies of an unsynced event cannot be repaired by the server.
- The single WSS connection is a deliberate single point of failure for *server* connectivity, with no HTTP polling fallback by decision. Local tagging, mesh forwarding, and offline play are unaffected by a socket outage and pending events survive it durably; but while the socket is down there is no server adjudication, no command delivery, and no fresh clock anchor.
- Worktree isolation does not restrict reads elsewhere on the user account. Real backups must be kept out of worker context and in a protected operator archive; stronger read isolation requires OS/filesystem controls.
- The full-flash archive restores flash state on the same functioning physical badge. It cannot undo eFuse burns, hardware damage, remote account changes, external NFC writes, or unsaved RAM. This is why no irreversible hardware/security modifications are allowed.
- Hardware layout/security state is verified separately for every badge, not assumed from the first probe. Unknown layout/encrypted device is not automatically enrolled.


## 11. Recovery, participant enrollment, and the only permitted flash path

This section is a specification for future implementation. No backup, flash, restore, eFuse operation, or serial connection was performed while writing it. It incorporates the decision to retain the stock bootloader and partition table and reserve the unused 64 KiB tail for a 4 KiB raw installation marker plus 60 KiB runtime-registered NVS. The user confirmed local organizer storage, recovery IDs plus owner backup files, and a first-badge restore rehearsal.

### Recovery promise and its boundary

The immutable original recovery target is the same physical badge’s complete persistent flash state captured at first enrollment, before the restore rehearsal or any custom write. A separate fresh snapshot immediately before each write preserves subsequent legitimate stock changes as well. That includes its then-current stock application, bootloader, partition table, NVS, PHY partition, LittleFS, provisioned identity, wallet material, installed applications, saves, and all otherwise unallocated addressable flash bytes. Existing edits on Amitoj's badge are part of that baseline. A snapshot cannot recover an earlier state that was never captured.

Capture exactly `0x400000` bytes from address zero on every supported 4 MiB badge. Do not substitute the existing `probe/factory-firmware.bin`, a downloaded firmware release, a filesystem export, or another participant's image. The existing file covers only `0x10000..0x2AFFFF` and omits personal data and boot infrastructure.

A flash image does not contain eFuses, flash-chip status/OTP configuration, volatile RAM, another NFC tag's data, or external account/website/blockchain state. Readable eFuse state is separately recorded for comparison; read-protected fields remain unavailable. eFuses are one-time-programmable and must never be written. Recovery is conditional on intact hardware, unchanged irreversible configuration, a working ROM download path, and retained verified archives. Do not promise recovery from physical damage or fuse burning. [Espressif eFuses](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/espefuse/index.html), [read protection](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/espefuse/summary-cmd.html).

### Supported participants and storage service

The recovery utility accepts any number of participants, one selected physical badge at a time. The first adapter supports only the HTN 2026 ESP32-C3, 4 MiB flash, unencrypted/unsecured ROM download, and the verified stock partition layout below. Arbitrary participant count does not mean arbitrary hardware. An unfamiliar chip, flash size, partition map, security state, or occupied tail is archived when readable but rejected for custom flashing until the orchestrator reviews a new adapter. There is no force flag.

Use an organizer-managed private archive outside every repository, Git worktree, shared project, cloud-sync folder, and worker sandbox. Example primary root: `/Users/amitojsingh/Library/Application Support/ZombieTag/BadgeArchive`. The operator must supply a real second storage destination on another durable device, such as an encrypted external drive. A second directory, symlink, APFS clone, or snapshot on the same physical disk does not satisfy the second-copy requirement. The utility checks resolved paths and filesystem/device identities; the organizer records the actual second medium because filesystem IDs alone cannot prove independent hardware.

Directories use mode `0700`; files use `0600`; the organizer uses encrypted storage volumes. Do not upload archives, put them in Git, send them to the backend, attach them to an agent, or print decoded wallet/identity data. Workers receive the recovery code's implementation specification and redacted result statuses only. The orchestrator's hardware process is the only process with archive and serial access.

Each badge receives a random human-readable recovery ID generated from 80 random bits, encoded as 16 Crockford Base32 characters in four groups, plus a checksum suffix and `ZT-` prefix. Check for collision against the local index before assigning it. The receipt contains that ID, enrollment time, masked badge identifier, snapshot hash, and instructions to return to the organizer with the same badge. It contains no secret key, flash image, or full eFuse data. This code is a lookup reference, not authorization to disclose a backup. For restoration, the physically connected badge's full factory MAC must match the archived manifest. An export to an owner's local drive additionally requires the organizer to establish ownership; possession of a code alone is insufficient. No online retrieval service is in the POC default.

Repeated enrollment of the same full MAC finds the existing recovery ID. It creates a new snapshot, never a replacement for `original`. A lost code can be recovered by connecting the same badge. Two badges never share a snapshot. The tool never restores one badge's identity or wallet onto another badge.

### Archive structure and manifest

Use a private `index.sqlite` for recovery-ID-to-device lookup, with directory scanning of signed-off manifests available to rebuild the index. The index is not the only record of ownership or snapshot identity. Each badge directory contains:

```text
<recovery-id>/
  receipt.txt
  original/                         # immutable first complete baseline
    flash.bin                      # exactly 4,194,304 bytes
    flash-second-read.bin           # independently acquired, equal bytes
    manifest.json
    efuses.json                    # complete readable summary, private
    efuses-readable.bin             # readable eFuse blocks; never a write input
    security-info.txt
    flash-info.txt
    partition-table.bin             # extracted offline from flash.bin
    partitions.json                # parsed offsets, sizes, types, hashes
    acquisition.log                # private; no trace/hex dump
    recovery-instructions.txt
  snapshots/<utc-time>-<uuid>/      # fresh state before later writes
    ...same capture files...
  operations/<uuid>/
    intent.json
    result.json
    post-write.bin                 # private complete readback
    commissioning.json
  tool/                            # versioned recovery script and dependency lock
```

The manifest records schema version; recovery ID; snapshot ID and kind; UTC capture times; full factory MAC; chip/package/revision; JEDEC flash ID and detected size; port/USB identifiers as observations, never identity; esptool version; recovery-tool revision; read mode; offsets and lengths; both complete-read SHA-256 values; parsed partition metadata and hashes; eFuse/security evidence hashes and read-protection masks; original snapshot linkage; acquisition return codes; operator label; and intended duplicate-copy location and medium label. Actual duplicate-copy verification is recorded in a separate operation receipt with the immutable manifest hash and copied file hashes; never insert a manifest's own hash into itself or modify the original after publishing it. No parsed wallet contents or passwords belong in the manifest.

Store paths relative to the snapshot wherever possible. Compute hashes on reopened files after flushing, not from the same in-memory buffer that was written. Stage a capture in a new `*.partial` directory, fsync files and directory metadata, then rename atomically to its final snapshot ID. Mirror it through a separate partial directory on the second medium, fsync it, reopen and hash the copied files, then atomically publish that copy. Mark `BACKUP_VERIFIED` only after both complete reads match and both durable copies verify. If the second medium is absent, full, or fails comparison, capture remains `AWAITING_SECOND_COPY`; flashing stays blocked.

Read-only file permissions and an immutable naming convention prevent routine replacement, but do not claim filesystem cryptographic immutability. Tool operations never open existing snapshot files for truncation and never implement deletion, retention pruning, or overwrite switches. Every failure creates a separate private operation record. The original manifest remains unchanged; subsequent operation status belongs in separate records/index transactions.

### Capture procedure

1. Organizer obtains the participant's agreement to backup and replace their badge software, assigns/looks up the recovery ID, closes browser IDEs and serial monitors, and selects exactly one port. Acquire a process lock for the archive and a hardware lock before any serial operation; never kill an unknown process holding the port.
2. Enter the ROM loader by holding START while connecting USB if normal connection does not work. Keep reliable USB power and leave the badge connected throughout capture and the following write. Never assume `/dev/cu.usbmodem101` identifies a particular badge.
3. Read chip type/revision, full factory MAC, flash ID/size, security information, complete readable eFuse summary, and readable eFuse binary. Require ESP32-C3, 4 MiB, secure boot off, flash encryption off, secure-download off, and an available ROM download path. This rechecks every participant; Amitoj's existing fuse log is not reusable evidence for another badge.
4. Use esptool **5.4.0**, the version installed and inspected in `.probe-venv`, with a pinned bundled RAM flasher stub. Loading this stub changes RAM and does not program flash. Read all 4 MiB twice into different new files while keeping the application stopped. Compare lengths, every byte, and SHA-256. No reset into application code is allowed between the reads. On mismatch, retain failed evidence and reacquire both reads in a fresh loader session; do not choose whichever read happens to parse.
5. Parse the saved flash offline. Check the boot image, partition-table checksum/entries, factory-app image, all partition bounds/nonoverlap, and the exact expected stock layout. Record per-region hashes without decoding personal data. Readable but unsupported layouts still get a recovery archive; they do not pass the custom-flash adapter.
6. Publish the primary snapshot and independently verify the second durable copy. Only now issue a `BACKUP_VERIFIED` receipt. A failed or interrupted capture is never eligible by filename alone.

The official CLI permits whole-flash reads and control over reset behavior. A stock application may mutate data on boot, so matching two reads made with an intervening normal boot is not a valid substitute for quiescent capture. [Read/write commands](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/basic-commands.html), [reset modes](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/advanced-options.html).

#### Exact esptool 5.4.0 command equivalents

These are reviewable equivalents, not a bypass around the guarded utility. `BADGE_PORT` and `SNAP_DIR` refer to values selected by that utility; archive logs are private. All tool stdout/stderr is captured privately and sanitized before displaying status. The shell commands below must never be run by a worker agent.

```sh
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset --no-stub get-security-info
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset --no-stub read-mac
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset --no-stub flash-id
espefuse --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset summary --format json --file "$SNAP_DIR/efuses.json" dump --format joint --file-name "$SNAP_DIR/efuses-readable.bin"
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset read-flash --flash-size 4MB 0x0 0x400000 "$SNAP_DIR/flash.bin"
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset read-flash --flash-size 4MB 0x0 0x400000 "$SNAP_DIR/flash-second-read.bin"
```

In the installed 5.4.0 source, `--after no-reset` exits a running stub back into the ROM loader; it does not start the application. Each next `--before no-reset` connects to that ROM and may upload a fresh stub. `--after no-reset-stub` is also available but deliberately unnecessary for the manual sequence. `--before no-reset-no-sync` is not the ordinary reconnect setting. For a ROM-only fallback, add `--no-stub` to both read commands and retain `--flash-size 4MB`; this is much slower. The previous 2.75 MB ROM-only read took about 15 minutes.

The implemented guarded utility uses the pinned public Python API and a single owned loader connection for final identity check, both reads, write, and readback. It must call `attach_flash()` after connecting/running the stub, retain the returned stub object, and avoid shell strings. This prevents a port-name/device-swap gap between a successful gate and the write. The standalone read-only eFuse step may use its CLI before the single-session capture begins. Any disconnect invalidates the operation and requires fresh identity and state capture; no automatically resumed write. [Official embedding API](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/scripting.html).

Local source verification performed: `.probe-venv/lib/python3.12/site-packages/esptool/cmds.py` contains `reset_chip`, `read_flash`, `write_flash`, `_update_image_flash_params`, and `verify_flash`; `loader.py` contains `soft_reset`; `espefuse/efuse/base_operations.py` contains the binary `dump --format joint` implementation. The `keep` write options preserve boot-image header bytes. Do not use `verify-flash --diff`, which may print actual mismatched flash bytes into logs or agent-visible output.

### Stock-preserving firmware layout

No bootloader or partition table is flashed for this POC. The build's factory partition must match the on-device table. The complete physical map is:

| Flash range (end exclusive) | Use | Permitted custom action |
|---|---|---|
| `0x000000..0x008000` | Stock bootloader and leading area | Read/verify only |
| `0x008000..0x009000` | Stock partition-table sector | Read/verify only |
| `0x009000..0x00D000` | Stock `nvs`, type 1/subtype 2 | Never initialize, erase, or write |
| `0x00D000..0x00E000` | Stock `phy_init`, type 1/subtype 1 | Read/verify only; custom PHY uses embedded data |
| `0x00E000..0x010000` | Existing gap | Read/verify only |
| `0x010000..0x2B0000` | Stock `factory`, type 0/subtype 0, length `0x2A0000` | Replace with custom app after gate |
| `0x2B0000..0x3F0000` | Stock `storage`, type 1/subtype `0x83`, length `0x140000` | Never mount, format, erase, or write |
| `0x3F0000..0x400000` | Previously unpartitioned 64 KiB tail | 4 KiB raw installation header plus 60 KiB `zt_nvs`, after baseline backup and FF check |

At boot, verify the physical chip size and original table's exact four entries (labels, types, subtypes, offsets, lengths, flags), ensure no on-flash partition covers the tail, and register a RAM-only partition using `esp_partition_register_external(NULL, 0x3F1000, 0xF000, "zt_nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, &partition)`. Despite the API's name, `NULL` explicitly selects the internal flash chip in ESP-IDF v5.5.3. The function rejects overlap and out-of-chip regions. This registration does not rewrite the on-flash table. Initialize only `nvs_flash_init_partition("zt_nvs")` and use `nvs_open_from_partition("zt_nvs", ...)` for every application handle. [Pinned partition API](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_partition/include/esp_partition.h), [implementation](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_partition/partition.c), [named NVS](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/storage/nvs_flash.html).

Before the first custom boot on each badge, require the entire tail to be `0xFF` in both full snapshots. If occupied, preserve it and stop; do not claim it is safe merely because no table entry references it. On later custom boots, require a valid application storage header instead. The archive retains the original tail bytes regardless.

The custom installation marker is a raw 80-byte header in its own sector at 0x3F0000; NVS begins at 0x3F1000 and spans 0xF000 bytes. Header offsets: 0 magic ASCII ZTIN[4]; 4 schema u16=1; 6 length u16=80; 8 factory MAC[6]; 14 reserved u16=0; 16 installation UUID[16]; 32 original snapshot SHA-256[32]; 64 NVS offset u32=0x3F1000; 68 NVS length u32=0xF000; 72 flags u32=1 (initialized); 76 IEEE CRC32 of bytes 0..75. All integers little endian; rest of the 4 KiB sector remains 0xFF. Register a separate bounded runtime zt_install data partition over that sector (custom subtype 0x40) or use an equivalently range-checked flash helper. The guarded tool generates the installation UUID. INSTALL_INIT first verifies the entire tail blank, initializes NVS only in its 60 KiB region, then writes this valid header LAST as the initialization commit. Subsequent boots raw-read/validate the header and MAC before NVS initialization. Interrupted initialization with any nonblank tail and no valid header enters STORAGE ERROR; it never retries by erasing. The header baseline hash links the immutable original archive; it is not evidence that firmware itself inspected the archive. Subsequent nonempty storage without a valid matching header must enter a visible storage-error/USB-recovery state. Interrupted initialization, unknown schema, missing header, corrupt blob, wrong MAC, `ESP_ERR_NVS_NO_FREE_PAGES`, and `ESP_ERR_NVS_NEW_VERSION_FOUND` must never trigger the common example-code erase/retry pattern. Do not call `nvs_flash_erase()` at all. Explicit maintenance may reset only `zt_nvs` after a new full current-state backup and identity gate; it is not part of automatic boot recovery.

NVS can perform its normal internal page recovery/garbage collection within its own 60 KiB; the prohibition is against automatic partition erasure, formatting unknown data, or touching stock regions. Store provisioning/configuration and the current game's committed roster/events in the 60 KiB NVS partition, bounded by the plan's 20-player limit. Keep aggregate live serialized data below 20 KiB, reserve the remaining partition space for NVS metadata and page turnover, and reject further changes visibly if a write/commit fails. Use compact bounded binary blobs, not unbounded JSON history. Checkpoints and events must be committed before acknowledging a persistent operation; an exhausted outbox must not silently discard unacknowledged gameplay evidence. Old completed rounds may be cleared only under the explicitly defined new-round retention rule, not to mask storage corruption.

The custom build must disable stock NVS users:

```text
CONFIG_ESP_WIFI_NVS_ENABLED=n
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n
CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=n
CONFIG_ESP_PHY_ENABLE_USB=y
```

Additionally set `wifi_init_config_t.nvs_enable=0` after `WIFI_INIT_CONFIG_DEFAULT()` and call `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before Wi-Fi credentials/configuration. Host credentials are read from `zt_nvs` by application code and passed to RAM configuration. Do not invoke the default `nvs_flash_init()`, `nvs_open()`, stock filesystem mounting, Wi-Fi restore, or default-NVS erase paths. Disabling PHY persistence makes radio startup calibrate without saving into stock NVS; embedding PHY initialization avoids depending on or rewriting `phy_init`. Keep native USB enabled while radio is running. These symbols were verified against [ESP-IDF v5.5.3 PHY Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_phy/Kconfig) and [Wi-Fi header](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_wifi.h).

Security/eFuse programming, secure boot activation, flash encryption activation, anti-rollback eFuse updates, JTAG/download disabling, flash voltage changes, and flash-status/OTP writes are prohibited in both firmware and tools. Build/review configuration must substantiate that prohibition before release. Default `idf.py flash`, merged all-flash release images, `erase-flash`, arbitrary `erase-region`, and direct worker flashing are prohibited. The guard is the only hardware mutation entry point.

Rejected alternatives: a new on-flash partition inside LittleFS would modify the table and destroy some stock storage; using stock `nvs` risks identity/calibration data; overwriting a guessed unused LittleFS sector is unsafe because allocation can move; adding the tail to the on-flash table is feasible but introduces an unnecessary persistent table write. Runtime registration provides real bounded installation/NVS partitions while changing only the app and the proven-empty tail. If this pinned API fails during bring-up, stop and revise the plan; do not silently switch layouts.

### Fresh capture before every firmware write

The original baseline is retained forever. Before every later app flash or full restore, create a fresh complete current-state capture using the same two-read, identity, hash, and second-copy procedure. This lets the owner recover either the original stock state or a later explicitly selected checkpoint. A capture made earlier in the same uninterrupted loader session may serve as this operation's current-state capture; do not repeat it merely to satisfy a label. Once the application has run, the connection has been lost, or an earlier write has occurred, a new capture is required.

No recursive backup is needed while creating a backup: captures are read-only. If a partially flashed or nonbooting badge remains readable, archive its current bytes too, label the snapshot as incomplete/nonbooting, and then restore from a previously verified image. READ_VERIFIED means equal full reads plus verified copies; STOCK_IMAGE_VALID and LAYOUT_SUPPORTED are separate results. An invalid current app or partition table must not prevent verified capture or full restoration: the restore gate checks live MAC/chip/size/security and the selected archived target, not current bootability/layout. The custom flash-game gate still requires the supported current layout and protected regions. If a fresh current read cannot be verified, the normal write gate blocks and reports the exact failure; the tool has no auto-force recovery bypass.

### Atomic write gate and minimal application flash

The organizer command `flash-game` is one operation containing capture, gate, write, and verification. It takes a recovery ID, explicit port, and a release artifact manifest. It never accepts an arbitrary offset list.

Gate conditions, checked under the same hardware/archive lock immediately before writing:

- Connected full factory MAC, chip revision, flash identity/size, and security/eFuse state match the selected archive; USB path is not used as identity.
- Original and fresh current snapshots have complete independent read matches and verified second copies. Rehash the actual selected files; do not trust only SQLite status.
- Stock partition table matches the supported layout; first installation has an all-FF tail; later installation has compatible custom storage. If custom firmware has run since the last accepted commissioning, protected stock regions must match that previous preflash/commissioning baseline. If stock firmware legitimately ran in between, including after the restore rehearsal, take a new preflash baseline for preservation and record that transition without replacing `original`. Unexplained changes while custom firmware was running stop rollout rather than being hidden by another flash.
- First-badge stock restore rehearsal has passed for this utility/toolchain/recovery workflow, as confirmed by the user. No bypass is implied by “no tests.”
- Application header is valid ESP32-C3, compatible with chip revision, DIO/80 MHz/4 MiB build configuration, and fits the `0x2A0000` factory allocation. The release manifest pins the source revision, SDK version, app hash, size, and reviewed configuration. No unsupported security features are present.
- The only permitted new-image write is at `0x10000`; its entire erased sector range fits inside the factory partition. Produce a release artifact padded with `0xFF` to exactly `0x2A0000`, so post-write verification can compare the whole application partition and no stale stock application bytes remain within it. Hash this padded image as well as the original built app.

Write an `intent.json` with operation UUID, identity, snapshot IDs/hashes, release hash, exact permitted range, and status `PREPARED`; fsync it. Change status to `WRITING` before the write. A crash leaves an incomplete operation, never a fabricated success. There is no reusable “approved badge” bit that permits future writes without a fresh capture.

Command equivalent after the gate:

```sh
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset write-flash --flash-mode keep --flash-freq keep --flash-size keep 0x10000 "$APP_PADDED_BIN"
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset read-flash --flash-size 4MB 0x0 0x400000 "$POST_WRITE_BIN"
```

The implementation performs these operations on one loader object without releasing the port. Build an expected 4 MiB image from the fresh current snapshot by replacing only `0x10000..0x2AFFFF` with the padded app. Compare complete readback against that expected image, by byte and SHA-256, before allowing any normal boot. Thus the readback simultaneously verifies the new app and proves every other flash byte was retained during the tool's write. Esptool's automatic write MD5 verification is useful but does not replace this complete comparison. [Verification behavior](https://docs.espressif.com/projects/esptool/en/latest/esp32c3/esptool/advanced-commands.html).

On mismatch, leave the device in the loader, record `WRITE_VERIFICATION_FAILED`, retain both archives and the private readback, and report differing ranges/hashes without exposing contents. Do not boot, retry indefinitely, widen writes, erase the device, or alter security settings. A deliberate restore operation can recover the original snapshot after its own gate.

On success, record `FLASH_VERIFIED`, fsync the result, then permit a normal reboot. Holding START at boot enters the ROM loader; a blank screen there is expected. Use a normal power cycle with START released when native-USB reset behavior does not start the application. Do not promise automatic reset on every badge.

### Commissioning: verify the firmware also preserves stock storage

The preboot comparison only proves the flashing tool's behavior. On the first custom installation, after custom firmware boots, provisions, initializes its radio, and reaches a working game screen, return to the loader and capture a complete post-commissioning snapshot. Compare protected ranges `0x000000..0x00FFFF` and `0x2B0000..0x3EFFFF` byte-for-byte against that operation's immediate preflash snapshot; compare the factory app against the padded release artifact. Changes are permitted only in `zt_nvs` at `0x3F0000..0x3FFFFF`. Re-read and compare readable eFuse/security state too. The immutable original remains the restoration target; a stock boot during rehearsal may legitimately change saved bytes before this newer preservation baseline is captured. The guard records `COMMISSIONED` only if this holds. Apply this stock-region commissioning check per participant, with the first badge also exercising both normal-player and host-radio initialization paths before participant rollout.

If stock bytes change, stop rollout and inspect the code path, typically default NVS, PHY persistence, or a filesystem/erase helper. Preserve the failed evidence and recover that badge through the verified original archive. This check is mandatory protection of user data, not a request for a POC test suite or CI.

### Restore procedure and rehearsal

`restore --recovery-id ID --snapshot original --port PORT` is the normal owner-return operation. The tool resolves the selected snapshot inside the private archive, verifies both stored copies, acquires the hardware lock, identifies the same full MAC, checks security/eFuse state, captures and duplicates current state, and keeps the device in the loader. It then writes the full original image at zero with all flash-header options set to `keep`. No separate chip erase is issued; write-flash handles the sectors covered by the complete image.

```sh
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset write-flash --flash-mode keep --flash-freq keep --flash-size keep 0x0 "$ORIGINAL_FLASH_BIN"
esptool --chip esp32c3 --port "$BADGE_PORT" --before no-reset --after no-reset read-flash --flash-size 4MB 0x0 0x400000 "$RESTORE_READBACK_BIN"
```

Compare all `4,194,304` restored bytes and SHA-256 to the selected image before any application boot. Record restored snapshot ID and hash, device identity, original-preserved status, eFuse comparison, timestamps, and tool revision. If comparison fails, remain in the loader and report failure; never claim restoration because the write command exited successfully.

Only after equality is established, boot normally with START released. The organizer and owner confirm normal stock display/launcher, responsive buttons, and recognizable saved state. Inspect existing identity/app availability without dumping wallet secrets or invoking the known crashing stock `radio` command. Flash may legitimately change once stock software resumes, so the exact restore claim is tied to the verified preboot instant. Record the subsequent functional observation separately as `STOCK_BOOT_CONFIRMED`; do not demand a postboot whole-flash hash remain unchanged.

Required first-device rehearsal, before any custom firmware is installed anywhere: capture the first badge, verify both copies, write that exact same original image back, complete the full preboot readback comparison, and confirm normal stock boot. This proves the actual recovery path while the known-good archive exists. The rehearsal itself is a flash write and follows the same gates; the already-fresh original capture can serve as its current-state snapshot while that loader session remains uninterrupted. After normal stock boot, capture fresh current state again before installing custom firmware. Future participant restores use the same verified workflow but remain identity-specific.

### Utility interface and ownership

Implement `tools/badge_tool.py` with `esptool==5.4.0` pinned. It is code to be written later, not a tool currently present. The CLI is sequential and supports:

```text
archive-init --archive ABS --mirror ABS --mirror-medium-label LABEL
ports
enroll --port PORT --owner-label LABEL
snapshot --recovery-id ID --port PORT --reason TEXT
verify --recovery-id ID --snapshot SNAPSHOT
receipt --recovery-id ID --output LOCAL_PATH
rehearse-restore --recovery-id ID --port PORT
flash-game --recovery-id ID --port PORT --release RELEASE_MANIFEST
commission --recovery-id ID --port PORT --release RELEASE_MANIFEST
restore --recovery-id ID --snapshot original --port PORT
export --recovery-id ID --snapshot original --destination OWNER_LOCAL_DIRECTORY
status --recovery-id ID
```

Archive roots are saved in organizer-local tool configuration, not the repository. `flash-game`, `restore`, and `rehearse-restore` automatically make/reuse an eligible fresh capture; the operator cannot omit it. `export` produces a verified local bundle with the selected flash image, manifest, receipt, tool revision/dependency lock, and restore instructions; it does not upload or send anything. Omit eFuse readable-key material from owner exports unless necessary and explicitly requested; the essential complete flash image and device match information remain included. An operator can restore using the organizer's full private archive without exporting secrets.

The orchestrator alone runs these hardware commands, owns locks, merges code, checks release configuration, and writes operation records. Worker agents implement modules in isolated code worktrees and have no task to inspect private archives or touch devices. Hardware failures stop that operation; unrelated code work can continue. No unit-test suite, integration-test framework, CI, or coverage work is required. Required checks are successful firmware compilation, release inspection, backup byte equality and durable-copy verification, guarded write/readback, the accepted restore rehearsal, and the focused physical commissioning observations above.

## 12. ESP-NOW radio, mesh, and exact wire contract

Application code implements forwarding, durable delivery, game identities and acknowledgements. Initial hardware proof is three badges; 20 active roster slots is a design cap, not a measured crowd capacity.

### SDK facts and consequences

Use ESP-IDF v5.5.3, Wi-Fi STA on every badge, ESP-NOW on `WIFI_IF_STA`, and exactly one peer `ff:ff:ff:ff:ff:ff`, channel=0, encrypt=false. This avoids allocating one SDK peer per player. Wi-Fi must be started before ESP-NOW; the default PHY rate is 1 Mbit/s. Keep every application packet <=250 bytes. Broadcast encryption is unsupported; use project HMAC below. MAC send success is not application delivery: game acknowledgements remain necessary. Send one frame at a time, waiting for its callback; callbacks only copy bounded metadata and bytes into a queue. [ESP-NOW v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/network/esp_now.html)

Use `esp_now_recv_cb_t(const esp_now_recv_info_t *, const uint8_t *, int)` and `esp_now_send_cb_t(const esp_now_send_info_t *, esp_now_send_status_t)`. The RX info is valid only inside its callback. Copy source MAC, RSSI, channel, receive monotonic timestamp and payload; never retain SDK pointers. [Pinned header](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_now.h)

Configure `esp_wifi_set_country_code("CA", false)` and query the resulting country; discovery iterates only its allowed channels intersected with 1..11. Do not inherit the handoff's blind 1..13 sweep. Use HT20, `WIFI_PS_NONE`, no BLE, and no connectionless sleeping. STA's home channel follows its associated AP; channel changes are disallowed during scanning/connecting. [Wi-Fi API](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/network/esp_wifi.html), [Wi-Fi driver guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi.html)

### Channel and gateway state machine

A single radio cannot provide continuous simultaneous mesh operation on channel A and hotspot traffic on channel B. The round has one immutable `round_channel`, persisted with its prepare record. Host and phone can move; hotspot must expose 2.4 GHz and retain that channel for an uninterrupted backend connection.

LOBBY: host connects to its configured hotspot, reads its actual channel, then advertises the lobby there. Ordinary badge starts on last known channel for 2 seconds, then passively listens for ESP-NOW on each allowed channel for 700 ms. It accepts only HMAC-valid matching game packets, locks on a fresh packet from the designated host in any phase, or on a matching lobby participant with gateway_age_s below10, and obtains the complete frozen roster before READY. No fallback creates a second game. Discovery repeats with 1 second backoff. Selected host is provisioned; no automatic host election.

PREPARE/RUNNING: persist round_channel; participants never channel-scan merely because the host disappears. Isolated clusters remain together and continue direct tags. Loss of gateway heartbeat means `server offline`, not round reset or all players missing. On reboot the badge restores this round channel, but must obtain a fresh same-round time sample before playing. It must not treat persisted elapsed time as a running real-time clock.

Host reconnect: a single radio-owner task owns all Wi-Fi state. On STA disconnect, tear down the WSS connection and clear IP/HTTP state, then restore round_channel after the disconnect/scan operation has completed. A socket-only failure with the STA still associated needs a socket reconnect, never a Wi-Fi scan or channel change. At backoff 10, 20, 30, then 30 seconds, run one asynchronous AP scan restricted to `scan_config.channel=round_channel`, configured SSID (prefer the last BSSID among results), active min=40/max=120 ms. Free scan results exactly once. Only if an AP with that exact configured SSID and required security is currently found on that channel, attempt one connection with its returned BSSID fixed, channel hint set, `failure_retry_cnt=0`, roaming disabled. Start a 3-second association watchdog. After successful credential authentication, save the returned BSSID as the last successful AP; a hotspot restart changing BSSID therefore remains recoverable on the same channel. Once associated on the correct channel, DHCP can proceed on-channel; a separate 8-second IP timeout cancels it. Failed attempts remain offline between retries.

**The STA channel field is a hint, not a hard lock.** An AP can move between scan and connection, and SDK connection scanning can visit other channels. Monitor `WIFI_EVENT_HOME_CHANNEL_CHANGE` plus a 100-ms `esp_wifi_get_channel` check while reconnecting. Unexpected channel, association timeout, or cancelled scan triggers controlled cancellation; never advertise the new channel to participants. Suspend sends, stop scan, disconnect; if state does not settle within 500 ms, deinit ESP-NOW, stop Wi-Fi, restart Wi-Fi without calling connect, restore round_channel, then recreate ESP-NOW peer/callbacks. This is a bounded host-only radio interruption; queue retries survive it. Do not issue `set_channel` concurrently with a scan/connect. At most one recovery attempt may be in flight. An AP now on a different channel simply will not be found by the next restricted scan; host remains an offline player until hotspot returns to round_channel or the round ends. Internet outage with intact AP association needs a socket reconnect with the §5.6 backoff only, never a Wi-Fi scan or a channel change. [Scan configuration, STA hint and channel-change event source](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_wifi_types_generic.h), [Connection and scan lifecycle source](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_wifi.h)

After local round expiry, allow lobby discovery/channel changes, but retain last round's durable unsent events for upload. Ordinary badges return to discovery only after expiry/end, never on host silence. Hardware acceptance must observe host AP loss/reconnect on same channel and changed-channel fallback; this is the main integration risk, not a proven guarantee from API names alone.

### Packet contract

All integer fields are explicit little endian; serialize field-by-field, never transmit a C struct. Exact total length =48+payload_len+16 and must be <=250, so payload <=186 bytes. HMAC-SHA256 truncated to 16 bytes covers the complete header+payload using a provisioned game key. Constant-time comparison. Relays alter ttl/hops/age and recompute HMAC. This identifies the configured group, not individual trustworthy participants: every member has the key. Host HTTPS token stays host-only. Packet magic/type/length/round/roster checks precede expensive application processing.

| Offset | Header field | Bytes |
|---|---|---:|
| 0 | magic=0x5A54 | 2 |
| 2 | protocol_version=1 | 1 |
| 3 | type | 1 |
| 4 | payload_len | 2 |
| 6 | flags (bit0 relayed-capable; other bits zero) | 1 |
| 7 | ttl_remaining | 1 |
| 8 | hops | 1 |
| 9 | reserved=0 | 1 |
| 10 | game_id | 8 |
| 18 | round_id (0 lobby) | 8 |
| 26 | transport origin STA MAC | 6 |
| 32 | origin_boot_nonce | 8 |
| 40 | packet_seq | 4 |
| 44 | accumulated age_ms | 4 |

Boot nonce is random after radio RNG is available and changes per reboot. Packet sequence increments per newly originated envelope; no wrap reuse (reboot first). Flood retries get new packet_seq, while stable object IDs remain unchanged. Relays preserve origin/boot/packet_seq. Dedupe key=(origin,boot,packet_seq), not packet_seq alone. Stable infection event key=(round_id,victim_slot,event_seq u16); commit the sequence within the immutable event and reconstruct the counter from committed evidence before role flip/ACK; separate NVS keys are not a transaction. Event seq starts at1; only patient zero's causal root uses seq0. Slot is valid only in the frozen roster. Full MAC is the durable badge identity; never truncate its low four bytes.

| Type | ID | Payload fields in order | Flood? |
|---|---:|---|---|
| BEACON | 0x01 | slot u8, role u8, phase u8, flags u8, role_rev u16, infection_cause_seq u16, own_produced_seq u16, own_server_received u16, own_server_final u16, elapsed_ms i32, time_quality u8, round_channel u8, snapshot_rev u32, gateway_serial u32, gateway_hops u8, gateway_age_s u8, cache_digest u64, cache_count u16, uncertainty_ms u16, gateway_boot_nonce u64 (50 bytes) | Never |
| TAG_REQUEST | 0x02 | victim_slot u8, actor_slot u8, actor_cause_seq u16, actor_role_rev u16, known_victim_role_rev u16, attempt_seq u32, actor_elapsed_ms u32, tagger_observed_victim_rssi i8 (17 bytes); boot ID in header | Never |
| TAG_RESULT | 0x03 | actor_slot u8, request_boot u64, request_seq u32, result u8, accepted EVENT body (30 bytes; zero when rejected) (44 bytes) | Never |
| EVENT | 0x10 | victim_slot u8, event_seq u16, actor_slot u8, actor_cause_seq u16, actor_role_rev u16, victim_prior_role_rev u16, request_boot u64, request_seq u32, occurred_elapsed_ms u32, uncertainty_ms u16, tagger_observed_victim_rssi i8, victim_observed_tagger_rssi i8 (30 bytes) | Yes |
| ROSTER_PAGE | 0x20 | snapshot_rev u32, roster_hash u64, page_index u8, page_count u8, entry_count u8, entries <=8 each {slot u8, MAC6, name_len u8, name[12]} (<=175 bytes) | Yes |
| HOST_STATE | 0x21 | snapshot_rev u32, roster_hash u64, phase u8, page_index u8, page_count u8, entry_count u8, host_elapsed_ms i32, rule_rev u16, round_channel u8, winner u8, duration_ms u32, remaining_ms u32, patient_zero_slot u8, gateway_serial u32; <=12 entries each {slot u8, role u8, role_rev u16, cause_slot u8, cause_seq u16, covered_seq u16, flags u8}, uncertainty_ms u16 (<=159 bytes) | Yes |
| WATERMARKS | 0x22 | snapshot_rev u32, roster_hash u64, count u8, then <=20 {slot u8, server_received_contiguous u16, server_final_contiguous u16} (<=113 bytes) | Yes |
| COMMAND | 0x23 | command_seq u32, cmd_kind u8, target_slot u8 (255=all), valid_until_elapsed_ms u32, args_len u8, args <=175 bytes | Yes |
| COMMAND_RECEIPT | 0x24 | slot u8, command_seq u32, state u8, detail u16, applied_snapshot_rev u32 (12 bytes) | Yes |
| CACHE_PAGE | 0x30 | target_slot u8 (255=broadcast), cache_digest u64, page_index u8, page_count u8, count u8, <=56 {origin_slot u8, event_seq u16} (<=180 bytes) | Never |
| WANT_EVENTS | 0x31 | target_slot u8, request_seq u16, count u8, <=16 {origin_slot u8,event_seq u16} (<=52 bytes) | Never |
| EVENT_COPY | 0x32 | target_slot u8, full EVENT body30 (31 bytes) | Never |
| TIME_QUERY | 0x33 | target_slot u8, request_nonce u32 (5 bytes) | Never |
| TIME_REPLY | 0x34 | target_slot u8, request_nonce u32, sampled_elapsed_ms i32, source_snapshot_rev u32, source_age_ms u32, uncertainty_ms u16, time_quality u8 (20 bytes) | Never |

TAG_RESULT codes: ACCEPTED=0, ALREADY_ZOMBIE=1, OUT_OF_RANGE=2, ROUND_INACTIVE=3, STALE_ACTOR=4, BUSY=5, NOT_ROSTERED=6, PENDING=7. Duplicate accepted requests return ACCEPTED with the same event reference. These match §4.4. Do not expose success before the victim's durable commit. Invalid/malformed packets get no response. Type lengths are exact except explicitly counted arrays; trailing bytes are rejected.

Command arguments are specified below and never exceed 175 bytes. Use roster/state pages, not generic fragmentation. ANNOUNCE96 ASCII easily fits. PREPARE and START refer to assembled roster_hash/snapshot_rev. No command embeds an unbounded log. Fields marked role_rev u16 must not wrap within a round.

### Direct proximity and timing

Only a BEACON with hops=0, ttl=0, header origin MAC equal to SDK RX source MAC, valid slot/MAC mapping and matching round can update that player's proximity. A relayed EVENT received loudly is evidence of a nearby relay, not of a nearby victim/actor. Direct TAG_REQUEST and TAG_RESULT also require actual SDK source MAC equals the expected roster player; they are never forwarded. Threshold checks use both tagger's recent direct samples and victim's own direct samples/request RSSI. The tighter §4.3 source-age/three-sample rules and two-sided checks are mandatory.

BEACON period500ms with independent +-100ms jitter; do not use packet floods for proximity. Direct peers stale after4seconds, tag eligibility1.0seconds, eviction10seconds (roster itself never evicted). EWMA0.6 old+0.4 new, initialize from first sample. Channel scan/driver recovery invalidates old proximity samples so returning radio does not permit stale range tags.

HOST_STATE/CLOCK sample is generated fresh by host, not replayed as if its time were new. `age_ms` accumulates local queue residence at each relay and a nominal3ms/link transmission allowance; receiver estimates round time = sample elapsed + age. Queue an individual clock frame at most200ms locally and drop copies with accumulated age above1500ms. Local `esp_timer` advances learned elapsed time and deadline. Never extend an already learned local round deadline based on a later sample. Repeated START with same command ID never resets it. A rebooted registered badge requests a TIME_REPLY from a same-round direct peer, measuring RTT with request_nonce; accept RTT<=250ms and good peer time quality, otherwise await host. The peer samples its current elapsed at reply construction; receiver adds halfRTT. This is approximate subsecond synchronization, not precision distributed time; the engineering expiry/uncertainty rule in §4.2 applies. Mark source freshness/quality; a peer without initialized time cannot initialize another badge.

### Forwarding, delivery and congestion

Flood types originate ttl=7,hops=0 (at most eight radio links including the original transmission); forwarding requires ttl>0, decrements ttl and increments hops. A receiver processes an authenticated object once, but may relay a fresh envelope for a stable object already stored. Envelopes leave each node at most once; randomized forwarding jitter30..120ms. No flooding of BEACON, TAG, TAG_RESULT, or cache exchange. Do not claim guaranteed reach beyond eight radio links in one flood; later neighbor cache exchange provides store-carry-forward.

Origin sends EVENT immediately and retries at0.7s and1.7s, then every30seconds until server_received watermark covers it. Retry schedule is jittered +-20%; a node services at most one pending replay persecond. Retain durable local events through the round even after received; only server final decisions settle acceptance/scoring. Host receiving a packet, host's HTTP200, server durable event receipt, server adjudication, and badge applying a command are distinct states. No one ACK substitutes for the rest. Server watermarks advance only over contiguous event IDs, including rejected final events; gaps must be repaired, never skipped.

Every participant caches up to128 distinct event bodies in RAM across at most active/previous round; own unfinalized events and host-upload obligations are durable. Relay-only caches may be RAM-only; the original victim remains a durable custodian, and two-hop delivery is not a promise to survive loss of every custodian. Cache summaries in direct BEACON allow peers to notice different sets. A badge may run one neighbor exchange at a time: choose a changed-digest neighbor round-robin, at most once perneighbor per10seconds; send up to3 CACHE_PAGEs for128 keys at200ms spacing, within rate limit. Receiver requests up to16 missing keys, responder sends EVENT_COPYs at maximum2/second. All are link-local broadcasts with explicit target_slot; unintended recipients may cache EVENT_COPY but must not answer a request not addressed to them. Loss heals on the next exchange, new digest or30-second retry. Never erase missing events just because another badge says its digest differs. Full cache means stop accepting new own tag transitions with `storage full`, retain existing unfinalized events, and drop nonessential relay copies first. The storage layout/capacity in §3 applies.

Host emits a fresh clock every2seconds using HOST_STATE with page_count=0,page_index=0,entry_count=0; this is clock/phase metadata only and never clears a roster or roles. Full role snapshots have page_count>=1 and are assembled separately by snapshot/hash/type.  latest role pages/watermarks every10seconds and promptly after changes (coalesce250ms). Frozen roster pages are sent during preparation and on explicit rejoin request, not perpetually eachsecond. Commands retry every2seconds until actual target receipt, with per-round idempotent command IDs; latest critical state remains available by snapshot. Announcements can expire/drop without blocking later commands. Host receipt must reflect each badge's applied state, not simply its own successful RF send.

Single TX owner priorities: TAG_RESULT / own TAG_REQUEST; new EVENT and essential phase/role control; clocks and repair; cached event replay; cosmetic messages/telemetry. Perbadge token buckets: critical local direct frames8/s burst8, flooded/control6/s burst8, repair2/s burst2, BEACON2/s burst2. Total transmissions20/s burst20 including every category. Expired low-priority packets drop; pending critical objects remain in durable queues for retry. No continuously flooded perplayerREPORT in core. Optional nearest4-neighbor telemetry no faster than10seconds, and disabled first under congestion.

Budget illustration, not throughput guarantee:20 players produce about40 direct beacons/s. Host clock one flood/2s permits up to10 transmissions/s network-wide; two role pages plus one watermark per10s add up to6/s. A simultaneous19-infection burst needs up to380 transmissions per flood attempt before retries, far larger than the handoff's beacon-only estimate. Token buckets smooth it; convergence may take seconds. Local tagging stays higher priority. At1Mbit/s a250-byte payload alone takes2ms, before framing, backoff, interference and hotspot traffic. Thus avoid claiming20-player venue capacity without observation. With3 badges, verify direct tag, broken host path, reconnect delivery and chain relay before increasing roster.

Static suggested radio allocations: RX ring32*(250+24)=8768B; TX pool24*(250+24)=6576B; envelope dedupe256*24=6144B; direct peers20*80=1600B; event cache128*40=5120B; reassembly2KiB; misc flags/counters<2KiB, subtotal<34KiB excluding Wi-Fi/RTOS and shared game/durable buffers. Dedupe covers flood envelopes only; perpeer boot/sequence state filters direct beacons, avoiding cache churn from40beacons/s. Copy RX to fixed queue without allocation; if full increment counter and drop. Pending clock/stat pages coalesce. Expose drops, queue highwater, replay backlog, gateway age, channel and RSSI in a diagnostics screen/console for manual demo checks.

### Additional admission, close, and command encodings

The following messages complete the wire contract; workers must not invent private variants. All use the same 48-byte envelope and HMAC. Roster and state pages use zero-based indices. Integers wider than a byte are little endian. Fixed name[12] fields are zero-padded after name_len; reject embedded controls and nonzero padding.

| Type | ID | Payload fields, in order | Forwarding |
|---|---:|---|---|
| JOIN | 0x04 | requested_badge_mac[6], name_len u8, name[12], known_round_id u64, request_nonce u32, build_id[8] (39 bytes) | Flood, rate limit one request per badge per 3 s; round header zero for lobby or known current round for rejoin |
| JOIN_RESULT | 0x05 | target_mac[6], request_nonce u32, status u8, slot u8, snapshot_rev u32 (16 bytes) | Host-origin flood; only named MAC applies |
| SNAPSHOT_REQUEST | 0x25 | slot u8 (255 if unassigned), wanted_snapshot_rev u32, roster_hash u64, need_flags u8 (14 bytes) | Flood at most once per badge per 5 s |
| ROUND_CLOSED | 0x26 | slot u8, produced_seq u16, last_role_rev u16, close_elapsed_ms u32 (9 bytes) | Flood every 5 s until server records close frontier, then stop |
| CLOSE_RECEIPTS | 0x27 | closed_bitmap u32, archived_bitmap u32 (8 bytes; only bottom 20 bits valid) | Host-origin flood; acknowledges server durable round-close records/clearance |
| EVENT_DECISIONS | 0x28 | snapshot_rev u32, count u8, <=20 entries {victim_slot u8, event_seq u16, status u8, reason u8, archived u8} (<=125 bytes) | Host-origin flood; durable per-event decisions |
| DECISION_RECEIPT | 0x29 | slot u8, own_decided_contiguous u16 (3 bytes) | Flood after new durable decisions, coalesce 500 ms |

JOIN status: 0 registered, 1 rejoined, 2 registration closed, 3 room full, 4 bad configuration, 5 waiting for server. Slot 255 means not assigned. Admission still requires complete authoritative roster/rules; a JOIN_RESULT alone is not permission to tag. Replay the same request_nonce until resolved. A direct JOIN must have matching requested MAC/header origin/SDK source; relays preserve its original identity.

Role encoding: human=0, zombie=1, unknown=255. On-wire phase: lobby=0, prepared=1, running=2, expired_pending_sync=3, final=4. Flags are explicitly defined per message; reject unknown mandatory bits. BEACON flags: bit0 designated host, bit1 role provisional, bit2 storage healthy, bit3 registration acknowledged; all others zero. Time quality: 0 unknown, 1 initialized with <=2,000 ms uncertainty. Role-state entry flags: bit0 provisional, bit1 player ready; others zero. Gateway serial increases for each fresh host clock, never for a relay copy. A peer may advertise only the freshest gateway serial it has actually received; gateway age increases with monotonic time and saturates at 255 s.

Common COMMAND payload starts with `command_seq u32, kind u8, target_slot u8, valid_until_elapsed_ms u32, args_len u8`. Critical state commands use expiry `0xffffffff` and rely on round/revision checks; ANNOUNCE uses a real expiry. Kind and exact args:

| Kind | Value | Args |
|---|---:|---|
| PREPARE_ROUND | 1 | snapshot_rev u32, roster_hash u64, roster_count u8 (2–20), duration_ms u32=600000, channel u8, tag_rssi i8, tag_cooldown_ms u16=3000 (21 bytes) |
| START_ROUND | 2 | snapshot_rev u32, roster_hash u64, patient_zero_slot u8, initial_role_rev u16, sampled_elapsed_ms i32, duration_ms u32, uncertainty_ms u16 (25 bytes) |
| ROLE_SET | 3 | role u8, role_rev u16, cause_slot u8, cause_seq u16, covered_seq u16 (8 bytes; human cause_slot=255/cause_seq=0) |
| ANNOUNCE | 4 | text_len u8, printable ASCII text[1..96] (2–97 bytes) |
| END_ROUND | 5 | effective_elapsed_ms u32, reason u8, winner u8 (255 unknown), provisional u8 (7 bytes) |
| FINAL_RESULT | 6 | winner u8, complete u8, missing_slots_bitmap u32, state_rev u32 (10 bytes) |
| CANCEL_PREPARE | 7 | reason u8 (1 byte) |

END reason: time_limit=0, all_infected=1, operator_stop=2. CANCEL reason: absent_players=0, operator_cancel=1, invalid_channel=2. A cancel affects PREPARED only, never RUNNING. A replacement preparation uses a new round ID. Final-result completeness is false when explicitly forced with missing evidence; UI says `FINAL · INCOMPLETE DATA`.

COMMAND_RECEIPT state: received=0, applied=1, prepared_ready=2, rejected=3, requires_snapshot=4. Detail: none=0, wrong_round=1, invalid_args=2, storage_failure=3, missing_pages=4, old_events_pending=5, stale_revision=6. Host accepts receipts only from their slot's mapped origin. RECEIVED means durable transport custody; APPLIED/PREPARED_READY means the target actually committed its state.

EVENT_DECISIONS status: accepted=0, rejected=1, pending_dependency=2. Reason: none=0, wrong_round=1, not_rostered=2, invalid_parent=3, outside_round=4, duplicate_conflict=5, stale_role=6, invalid_payload=7. Archive flag is 0/1. Host repeats latest undecided-delivery pages every 10 s and promptly on change until the origin's DECISION_RECEIPT covers them. Pending decisions never advance that receipt's contiguous final frontier. If a watermark says a missing decision exists, send SNAPSHOT_REQUEST need_flags bit3 rather than inventing an accepted result. Backend sync includes `decision_applied:[{slot,through_seq},...]` so individual receipt reaches the server. Relay caches may record decisions for their own retained copies; only an event origin's acknowledgment settles that origin's delivery.

Authoritative types COMMAND, HOST_STATE, ROSTER_PAGE, WATERMARKS, JOIN_RESULT, CLOSE_RECEIPTS, and EVENT_DECISIONS require envelope origin MAC equal to the configured host. Relays preserve that origin. Do not require radio source MAC to equal host for these multihop packets. Group HMAC protects against accidental/unauthorized outsiders without the key; it does not provide malicious-participant-proof authority because members share the key. This POC trusts enrolled participants and makes no competitive anti-cheat guarantee.

SNAPSHOT_REQUEST need_flags: bit0 roster, bit1 roles/phase/time, bit2 watermarks, bit3 individual event decisions; other bits zero. Cache sets/digests/counts are **per round**, not aggregated: every CACHE_PAGE/WANT_EVENTS/EVENT_COPY belongs to the envelope's round ID, and its compact keys are interpreted only in that round. BEACON summarizes its header round. A badge retaining the immediately previous round offers direct old-round CACHE_PAGE inventories once per 30 s, even while its ordinary beacon is current-round; neighbors may answer for either retained round. Limit one repair exchange overall per node. Round ID plus page digest binds assembly; never merge pages across rounds or digests. Cache digest is the first eight SHA-256 bytes of sorted canonical `(slot u8,seq u16)` keys for that round, encoded little endian. An empty cache hashes the empty sequence.

The one-in-flight TX rule has a 500 ms callback watchdog. Missing completion records a failed send and triggers the radio owner's controlled ESP-NOW/Wi-Fi recovery; keep logical objects for retry. Increment a driver-generation counter on each restart, stop old driver callbacks before accepting a new send, and discard queued completions/RX work tagged with the old generation. Never free/reuse the in-flight buffer until callback or completed driver teardown. This prevents a lost callback from permanently stalling every transmission.

The firmware's role_rev field is u16; no wrap is allowed within a round. Snapshot/command/state revisions are u32. The backend rejects would-be overflow and starts a fresh round/game identifier only after ordinary archival rules. Roster page hash is SHA-256 over entries sorted by slot (`slot,mac6,name_len,name12`); take the first eight digest bytes as the transmitted hash bytes. Names cannot be changed within a frozen roster.

### Precise time and event rules

Signed elapsed fields can be negative during a scheduled countdown. `INT32_MIN` means unknown. Event timestamps are unsigned and occur only at elapsed >=0. START's sample describes time when the host constructs that envelope; retry reconstructs the timing sample while retaining the same logical command ID. Relays add actual local queue residence to `age_ms` plus 3 ms per transmitted link and recompute the group HMAC. No peer rebrands an old sample as new.

Host clock anchor comes from the server's `server_time_ms` at response generation plus half measured request round-trip time. Initial uncertainty is `ceil(RTT/2)+50 ms`; accept a new time anchor only for RTT <=2,000 ms and a sane server/round match. Afterward grow uncertainty by 200 ppm of monotonic elapsed time. A forwarded sample adds its accumulated age to estimated elapsed and an additional `100 ms * (hops+1)` engineering margin to uncertainty. The exact queue delay is already measured, not added again as uncertainty. Drop samples whose resulting uncertainty exceeds 2,000 ms. The 100 ms/link allowance is a POC engineering margin, not a proven upper bound on RF contention; deadline accuracy remains approximate. Prefer matched-nonce direct time exchanges where available and report degraded timing instead of claiming precision synchronization.

TIME_QUERY/TIME_REPLY initializes a rebooted badge from a continuing peer: use request_nonce, matching MAC/round, RTT <=250 ms, and source quality 1. Receiver estimates peer sampled elapsed plus half RTT; uncertainty becomes peer uncertainty plus `ceil(RTT/2)+10 ms`. The peer replies using its current locally advanced elapsed/uncertainty, not a stale saved number. Once a badge has a local deadline, later valid samples can shorten it but never extend it. During initial start scheduling, a corrected countdown can shorten the time to start; do not flip back to PREPARED after play begins.

BEACON (50 bytes), HOST_STATE (at most 159 bytes), START args (25 bytes), and TIME_REPLY (20 bytes) include uncertainty in their canonical layouts above. Each event stores uncertainty at victim RX/validation; storage completion latency does not alter occurrence time. Gateway clock identity is (configured host MAC, host boot nonce, gateway_serial). HOST_STATE carries serial explicitly and its envelope carries host boot nonce; participant BEACONs forward both serial and gateway_boot_nonce. On a new host boot epoch, freshness starts from a newly received valid host frame, never by comparing its serial numerically with the previous epoch.

An event's actor cause sequence denotes the actor's own victim-slot infection, except patient-zero sequence 0. A pending parent does not prevent local infection when direct authenticated zombie evidence is current; server causality resolves it later. Corrected roles with missing/invalid parents never grant final score until the server decides.

Flood receipt is not durable storage or finalization. The backend must separately distinguish `received_events` (durably ingested, including events still pending a dependency) from `decisions` (adjudicated); §5.5 carries these as the distinct `receipts` and `decisions` message types, up to eight entries each. A successful WebSocket send acknowledges nothing. WATERMARKS' received frontier is built from durable ingestion, final frontier from accepted/rejected adjudication; pending dependencies do not advance the final frontier. Every badge retains own evidence until final clearance even after the received frontier stops its repeated flood.

On saturation: direct TAG_RESULT/TAG_REQUEST use reserved queue capacity; new critical objects stay in bounded durable storage; ordinary clock/state frames coalesce by snapshot/type; cosmetic traffic drops first. Host-event custody must be committed before advertising it as server-received (which only the actual backend can establish). Persisting an event in the host is not itself server receipt.

## 13. USB operator interface and commissioning tool

`tools/badge_tool.py` includes recovery subcommands above plus `provision`, `game-export`, `diagnose`, and `release-manifest`. The tool never embeds host secrets in the app binary or patches the firmware for individual participants. Same release binary on every badge; per-device config in zt_nvs.

The complete additional CLI surface is:

```text
release-manifest --build ABS_BUILD_DIR --output ABS_RELEASE_DIR
install-init --recovery-id ID --port PORT --operation VERIFIED_FLASH_OPERATION_UUID
provision --recovery-id ID --port PORT --config PRIVATE_FILE --name NAME [--host]
diagnose --recovery-id ID --port PORT
game-export --recovery-id ID --port PORT --output PRIVATE_LOCAL_FILE
reset-game-storage --recovery-id ID --port PORT --export-receipt RECEIPT
import-bundle --source OWNER_BUNDLE_DIRECTORY
```

`install-init` is a guarded continuation of a recorded FLASH_VERIFIED operation. Recheck archive hashes/copies, expected physical MAC, full padded factory-partition SHA-256 reported by running app (computed over the complete factory partition, not confused with the ELF hash), blank tail/WAIT_INSTALL, and the operation's unused installation UUID. It sends the console operation `install_init`, reads back the raw header/config status, and records INSTALL_INITIALIZED. A disconnect does not transfer approval to another MAC or firmware. It never improvises an install when no verified flash record exists.

`reset-game-storage` is explicit maintenance for interrupted initialization/corrupt custom storage, not an automatic boot fallback. It takes a fresh complete dual-read/mirrored snapshot, verifies same device and existing original, requires successful export or an operator record that no game journal was ever initialized, then erases **only** `0x3f0000..0x3fffff`. It compares full readback with an expected image that changes only that tail to 0xff. It returns to WAIT_INSTALL and records a new guarded installation continuation. There is no arbitrary offset or force flag. Original recovery images remain untouched.

`import-bundle` makes the owner's copy usable even if the organizer's original index/laptop is lost. Verify the export manifest, full image size/hash, device identity metadata, tool version, and per-file hashes; reject path traversal/symlinks/duplicate files before import. Atomically recreate the device/archive entry and a second verified copy on the newly configured independent medium. If its recovery ID collides with a different MAC, fail; if the MAC already exists, add the verified snapshot without replacing original. Only then allow ordinary `restore`. The bundle includes a self-contained export manifest with the original image hash, MAC/chip/layout/security facts, readable eFuse binary hash, and source manifest hash. Full readable eFuse key bytes need not be exported: compare a fresh same-version read hash during restoration. A missing optional private evidence file is not a corrupt bundle; required files are enumerated explicitly in the export manifest. Restoring still requires the same physical device and live identity/security checks.

Console transport is UTF-8 JSON Lines, maximum complete input line 2,048 bytes, maximum response 2,048 bytes, request ID u32 echoed in response. Each host USB write is at most 192 bytes; assemble a full line incrementally and reject overflow. Provisioning is a length-bounded input transaction, never logged or echoed. Asynchronous diagnostics use lines prefixed `# `; machine responses begin `{`. Serialize output so log fragments cannot interleave with response JSON. Rate-limit human logs to 10 lines/s, no packet hexdumps, no full configuration dump.

| Console operation | Preconditions / behavior |
|---|---|
| `info` | Read-only firmware/protocol versions, full connected MAC, install state, chip/flash layout, full padded factory-partition SHA-256, active round, role, and last error; no keys |
| `install_init` | Only WAIT_INSTALL and verified blank tail; payload MAC, schema=1, installation UUID, baseline SHA-256; tool has just passed full backup/flash checks; initialize named NVS marker, no other region |
| `configure` | USB operator supplies private config object; validate all bounds, expected MAC, game/host IDs, HTTPS-only URL <=192 bytes, SSID <=32 bytes, password <=63 bytes, token <=256 bytes, group key exactly32 bytes; persist atomically, acknowledge hash only, reboot normally |
| `status` | Read-only counters, channel, queue high-water, free/minimum heap, largest block, peer ages, pending event IDs/counts; no wallet/stock-storage access |
| `button` | Debug USB input only, accepts logical button press/release, goes through game rules; never directly changes role |
| `link_allowlist` | Explicit diagnostic mode for three-badge mesh demonstration; affects received frames before processing, never creates fake RSSI; visible DIAGNOSTIC banner; empty restores ordinary radio; setting not persisted |
| `game_export` | Stream bounded pages of current/previous round game evidence to a local operator file; never reads stock storage |
| `archive_clear` | Only after receipt covers exported/decided prior-round frontiers; clears that game's journal/checkpoint, not config, marker, or any stock partition |

Only `configure` in a lobby/no-pending-round state can change host credentials, game ID, or group key; active game configuration is frozen. Initial provisioning input is a private file read by the tool, not shell arguments echoed into history. The operator sets `provision --recovery-id ... --port ... --config /private/path/session.json --name NAME --host` on the selected host; ordinary badges receive only player fields and the shared mesh key. Validate that exactly one enrolled badge is the designated host. Host switch position alone cannot confer server credentials.

The backend teammate/operator creates the game and private session configuration. Registration is still a game action: flashing/configuring a badge does not automatically admit it to an already-running round. Recovery ID never becomes player ID, game ID, room code, token, or mesh key.

Before first guest rollout, the orchestrator records the first-badge restore rehearsal and the protected-region comparison after **both** player and host initialization. Per participant, repeat its own backup and protected-region commissioning. This implements the user's recovery requirement without imposing a general test suite.

## 14. Source notes and completion standard

Primary sources read during planning:

- Local `probe/REPORT.md`, boot/security logs, and later unlock/save records; these establish what was observed on the investigated badge, not universal facts about every guest badge.
- `/Users/amitojsingh/Downloads/zombie_tag_handoff_v2.md`; earlier handoff used only for the UI/LED reference, superseded by the user's current scope.
- [Official badge flashing page](https://badge.hackthenorth.com/custom-flash) and [HAL guide](https://badge.hackthenorth.com/custom-firmware-hal.md).
- [ESP-NOW v5.5.3](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/network/esp_now.html), [pinned esp_now.h](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.3/components/esp_wifi/include/esp_now.h), and Wi-Fi API/source links in §12.
- [espressif/led_strip 3.0.3 dependencies](https://components.espressif.com/components/espressif/led_strip/versions/3.0.3/dependencies) declare ESP-IDF >=5.0; use its RMT backend.
- [ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/protocols/esp_http_client.html), [SPI LCD](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/peripherals/lcd/spi_lcd.html), and NVS/partition/PHY references in §11.
- Espressif esptool documentation linked in §11, plus installed esptool **5.4.0** help/source for exact command/reset behavior.

The protocol, queue sizes, HTTP endpoints, retry policy, UI, and storage organization here are project design decisions. They are not copied claims that the original badge firmware already implements them. ESP-NOW range, battery life, 20-player venue performance, and successful restoration remain observations to record during execution.

The future delivery is complete when the firmware and reusable operator tool are implemented and built; the backend teammate has an exact agreed interface; the available badges have verified identity-specific archives; first-badge recovery is demonstrated; and the requested local tagging, relaying, mobile Wi-Fi, offline replay, and announcement behavior has been observed with the available equipment. Report missing backend/device inputs explicitly rather than claiming an unobserved end-to-end demo.
