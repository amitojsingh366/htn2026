# Packet M01 — ESP-NOW radio, wire codec, channel owner, and mesh

## Goal

Implement `components/zt_radio/`: the real serializers and parsers for every packet type,
the group HMAC, the single RX/TX owner, channel discovery and controlled recovery, and
bounded forwarding with dedupe, cache exchange, and anti-entropy.

This is the packet where a wrong byte offset silently breaks the whole game and a lost
send callback can permanently stall every transmission. Precision here is the deliverable.

## Branch and worktree

- Branch: `firmware/M01-radio-mesh`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/M01`

## Applicable specification

`plan.md` §12 **in full** — SDK facts, channel and gateway state machine, packet
contract, direct proximity and timing, forwarding/delivery/congestion, the additional
admission/close/command encodings, and the precise time and event rules. Also §4.3
(proximity), §4.4 (tag transaction, for the direct TAG paths), and §7 (budgets).
`docs/protocol.md` restates the same contract; `zt_wire.h` and `zt_radio.h` are frozen.

## Owned file allowlist

```
firmware/components/zt_radio/CMakeLists.txt
firmware/components/zt_radio/radio.c
firmware/components/zt_radio/channel.c
firmware/components/zt_radio/mesh.c
firmware/components/zt_radio/wire.c
```

Nothing else. `zt_hal`, `zt_ui`, `zt_game`, `zt_store`, `zt_gateway`, and `zt_console`
are owned by workers running right now.

## Required behavior

### `wire.c` — codec

Serialize and parse **field by field, explicit little endian**. Never memcpy a C struct
onto the wire. Total length is exactly `48 + payload_len + 16` and must be ≤250.

- Envelope: magic `0x5A54` at offset 0, protocol_version 1 at 2, type at 3, payload_len
  at 4, flags at 6, ttl_remaining at 7, hops at 8, reserved 0 at 9, game_id at 10,
  round_id at 18, origin STA MAC at 26, origin_boot_nonce at 32, packet_seq at 40,
  accumulated age_ms at 44.
- HMAC-SHA256 truncated to 16 bytes over the **complete header plus payload**, using the
  provisioned 32-byte group key. **Constant-time comparison.** Relays alter ttl, hops, and
  age_ms and therefore **recompute** the HMAC.
- Implement every type in `zt_wire.h`. **Type lengths are exact** except for explicitly
  counted arrays; reject trailing bytes. Reject unknown mandatory flag bits. Fixed
  `name[12]` fields are zero-padded after `name_len`; reject embedded control characters
  and non-zero padding.
- Check magic, type, length, round, and roster **before** any expensive processing.
- Invalid or malformed packets get **no response at all**. Never create a response storm.

### `radio.c` — single RX/TX owner

Wi-Fi STA started before ESP-NOW; ESP-NOW on `WIFI_IF_STA`; exactly **one** peer
`ff:ff:ff:ff:ff:ff`, channel 0, encrypt false. Never allocate one SDK peer per player.

- Callbacks are `esp_now_recv_cb_t` and `esp_now_send_cb_t`. The RX info is valid **only
  inside the callback**: copy source MAC, RSSI, channel, monotonic RX timestamp, and
  payload into the fixed 32-slot queue, then return. **Never retain an SDK pointer.** If
  the queue is full, increment a counter and drop — never allocate.
- **One TX in flight**, waiting for its callback, with a **500 ms watchdog**. A missing
  completion records a failed send and triggers controlled ESP-NOW/Wi-Fi recovery while
  keeping logical objects for retry. Increment a **driver-generation counter** on each
  restart, stop old callbacks before accepting a new send, and discard queued completions
  and RX work tagged with the old generation. Never free or reuse the in-flight buffer
  until the callback fires or teardown completes. This is what prevents one lost callback
  from stalling transmission forever.
- Token buckets per `plan.md` §12: critical local direct 8/s burst 8, flooded/control 6/s
  burst 8, repair 2/s burst 2, BEACON 2/s burst 2, and **total 20/s burst 20 including
  every category**. TX priority order: TAG_RESULT and own TAG_REQUEST; new EVENT and
  essential phase/role control; clocks and repair; cached event replay; cosmetic.
- Implement the diagnostic `link_allowlist` of §13: it drops **received frames before
  protocol handling**, never fabricates RSSI, shows a visible DIAGNOSTIC banner state, and
  is not persisted. Empty restores ordinary radio.

### `channel.c` — channel and gateway state machine

`esp_wifi_set_country_code("CA", false)`, then query the result; discovery iterates
**only its allowed channels intersected with 1..11**. Do not sweep 1..13. HT20,
`WIFI_PS_NONE`, no BLE, no connectionless sleep.

- LOBBY: ordinary badge starts on last known channel for 2 s, then passively listens on
  each allowed channel for 700 ms. Accept only HMAC-valid matching-game packets. Lock on
  a fresh packet from the **designated host** in any phase, or on a matching lobby
  participant with `gateway_age_s` < 10, and obtain the complete frozen roster before
  READY. Discovery repeats with 1 s backoff. **No fallback creates a second game and there
  is no automatic host election.**
- PREPARE/RUNNING: `round_channel` is persisted and immutable for the round. Participants
  **never channel-scan merely because the host disappeared**. Loss of gateway heartbeat
  means `server offline` — not a round reset and not all players missing. Isolated
  clusters keep tagging each other.
- Host reconnect, host-only: at backoff 10, 20, 30, then 30 s, run **one** asynchronous AP
  scan restricted to `scan_config.channel = round_channel`, configured SSID, preferring
  the last known BSSID, active min 40 / max 120 ms. Free scan results **exactly once**.
  Only if an AP with the exact configured SSID and required security is found on that
  channel, attempt one connection with the returned BSSID fixed, channel hint set,
  `failure_retry_cnt = 0`, roaming disabled, and a 3 s association watchdog. Save the
  successful BSSID so a hotspot restart stays recoverable on the same channel. A separate
  8 s IP timeout cancels DHCP.
- **The STA channel field is a hint, not a hard lock.** Monitor
  `WIFI_EVENT_HOME_CHANNEL_CHANGE` plus a 100 ms `esp_wifi_get_channel` check while
  reconnecting. On unexpected channel, association timeout, or cancelled scan: suspend
  sends, stop scan, disconnect; if state does not settle within 500 ms, deinit ESP-NOW,
  stop Wi-Fi, restart Wi-Fi **without calling connect**, restore `round_channel`, then
  recreate the ESP-NOW peer and callbacks. At most one recovery attempt in flight. Never
  issue `set_channel` concurrently with a scan or connect. **Never advertise a new channel
  to participants.**
- A socket-only failure with the STA still associated needs a socket reconnect, **never** a
  Wi-Fi scan or channel change. Channel scan or driver recovery **invalidates old
  proximity samples** so a returning radio cannot permit a stale-range tag.

### `mesh.c` — validated messages, proximity, forwarding, repair

- **Direct proximity only.** A BEACON updates proximity only with `hops == 0`, `ttl == 0`,
  header origin MAC equal to the SDK RX source MAC, valid slot/MAC mapping, and matching
  round. Direct TAG_REQUEST and TAG_RESULT likewise require SDK source MAC to equal the
  expected roster player, and are **never forwarded**. A loudly received relayed EVENT is
  evidence of a nearby relay, not a nearby victim.
- BEACON every 500 ms with independent ±100 ms jitter. EWMA `0.6·old + 0.4·new`,
  initialized from the first sample; fixed point is fine. Direct peers stale at 4 s, tag
  eligibility 1.0 s, eviction 10 s; the roster itself is never evicted.
- Dedupe key is `(origin, boot, packet_seq)` — **never packet_seq alone** — in a bounded
  256-entry, 30 s table. Per-peer boot/sequence state filters direct beacons so 40
  beacons/s do not churn the dedupe cache.
- Flood types originate `ttl=7, hops=0`, at most eight radio links. Forwarding requires
  `ttl > 0`, decrements ttl, increments hops, adds real local queue residence plus 3 ms
  per link to `age_ms`, and recomputes the HMAC. An envelope leaves each node **at most
  once**; forwarding jitter 30–120 ms. Never flood BEACON, TAG, TAG_RESULT, or cache
  exchange.
- EVENT origin sends immediately, retries at 0.7 s and 1.7 s, then every 30 s until the
  server_received watermark covers it, jittered ±20 %, servicing at most one pending
  replay per second.
- Neighbour repair: one exchange at a time, round-robin over changed-digest neighbours, at
  most once per neighbour per 10 s; up to 3 CACHE_PAGEs at 200 ms spacing; receiver
  requests ≤16 missing keys; responder sends EVENT_COPY at ≤2/s. Unintended recipients may
  cache an EVENT_COPY but **must not answer a request addressed to someone else**.
  **Never erase a missing event because another badge's digest differs.** Cache digest is
  the first eight SHA-256 bytes of sorted canonical `(slot u8, seq u16)` keys for that
  round; an empty cache hashes the empty sequence. Cache sets are **per round**, never
  aggregated across rounds.
- Host clock: a fresh HOST_STATE every 2 s with `page_count=0, page_index=0,
  entry_count=0` is clock and phase metadata only and **never clears a roster or roles**.
  Full role snapshots have `page_count >= 1`. Queue an individual clock frame at most
  200 ms locally and drop copies with accumulated age above 1,500 ms. A peer never
  rebrands an old sample as new. Gateway serial increases only for a fresh host clock,
  never for a relay copy.
- TIME_QUERY/TIME_REPLY: matched nonce, matching MAC and round, accept RTT ≤250 ms and
  source quality 1; receiver adds half RTT; uncertainty becomes peer uncertainty plus
  `ceil(RTT/2)+10 ms`. The peer replies with its **currently advanced** elapsed time, not
  a stale saved number. A peer without initialized time cannot initialize another badge.
- Authoritative types COMMAND, HOST_STATE, ROSTER_PAGE, WATERMARKS, JOIN_RESULT,
  CLOSE_RECEIPTS, EVENT_DECISIONS require **envelope origin MAC equal to the configured
  host**, with relays preserving that origin. Do **not** require the radio source MAC to
  equal the host for these multihop packets.
- Static allocations roughly per §12: RX ring 32×274, TX pool 24×274, dedupe 256×24,
  direct peers 20×80, event cache 128×40, reassembly 2 KiB. Copy RX into the fixed queue
  without allocation.

## Review criteria

1. Every byte offset, field order, length, and enum value matches `zt_wire.h` and
   `docs/protocol.md`, and parsing rejects trailing bytes and bad padding.
2. HMAC covers header plus payload, is compared in constant time, and is recomputed by
   relays after ttl/hops/age changes.
3. No SDK pointer outlives its callback and no allocation happens in a callback.
4. The one-TX-in-flight watchdog, driver-generation counter, and callback-loss recovery
   are all present and cannot deadlock transmission.
5. Dedupe uses the full `(origin, boot, packet_seq)` key; proximity updates accept only
   verified direct beacons.
6. Channel recovery never advertises a new channel, never scans on mere host silence, and
   never runs `set_channel` concurrently with scan or connect.
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
