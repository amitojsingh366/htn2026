# Packet G01 — game state machine, clock, peers, and durable storage

> **Attempt 2. Your BLOCKED_CONTRACT was correct on both counts and both are now fixed.**
>
> 1. The writable-region constraint did forbid the installation-header write that
>    `zt_store_install_init()` requires. It has been corrected below: firmware may write
>    the full 64 KiB tail `0x3F0000..0x3FFFFF`, comprising the 4 KiB header sector
>    (written exactly once, last, as the initialization commit, never erased) and the
>    60 KiB `zt_nvs`. Stopping rather than writing outside your stated permission was the
>    right call on this subsystem.
> 2. `zt_radio.h` now declares `zt_mesh_get_boot_nonce(zt_boot_nonce_t *out)`, owned by
>    the radio module, returning `ZT_ERR_INVALID_STATE` until radio entropy is available
>    and a value stable for the rest of the boot thereafter. Use it for the tag attempt
>    identity and for `request_boot` on committed events. `ZT_ERR_NOT_READY` does not
>    exist; the frozen enum was deliberately not widened.
>
> Your worktree is rebased onto the amended contract. Re-read `zt_radio.h` and the
> corrected constraint below, then implement the packet in full. Nothing else changed.

## Goal

Implement `components/zt_game/` (game state, clock, peer table) and `components/zt_store/`
(the `zt_nvs` persistence layer). The game task is the **sole writer of gameplay state**,
and the store is the only thing standing between a player's tag and losing it.

The storage rules here are not style preferences. This firmware runs on badges people
lent us, whose stock NVS, LittleFS, bootloader, and partition table must survive
untouched. A single stray erase call is unrecoverable damage.

## Branch and worktree

- Branch: `firmware/G01-game-store`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/G01`

## Applicable specification

`plan.md` §3.1 (flash layout), §3.2 (persistent data) **in full**, §4.1 (boot and
admission), §4.2 (clock), §4.3 (proximity and tag selection), §4.4 (tag transaction),
§4.5 (offline events and server authority), §7 (budgets), §11 "Stock-preserving firmware
layout" for the exact partition registration and prohibited calls.

## Owned file allowlist

```
firmware/components/zt_game/CMakeLists.txt
firmware/components/zt_game/game.c
firmware/components/zt_game/clock.c
firmware/components/zt_game/peers.c
firmware/components/zt_store/CMakeLists.txt
firmware/components/zt_store/store.c
```

Nothing else.

## Required behavior

### Storage (`store.c`) — read this section twice

Register the runtime partition and **nothing else**:

```c
esp_partition_register_external(NULL, 0x3F1000, 0xF000, "zt_nvs",
                                ESP_PARTITION_TYPE_DATA,
                                ESP_PARTITION_SUBTYPE_DATA_NVS, &part);
```

`NULL` explicitly selects the internal flash chip in ESP-IDF v5.5.3. This writes no
partition table. **Check every return value.** At boot verify the physical chip size and
the original table's exact four entries and ensure no on-flash partition covers the tail.
Overlap, a different flash size or layout, a non-blank unrecognized tail, or missing
enrollment authorization is a **hard storage error** — never permission to erase.

Initialize only `nvs_flash_init_partition("zt_nvs")` and open handles only with
`nvs_open_from_partition("zt_nvs", ...)`.

**Absolutely prohibited, with no exception and not even in a comment example:**
`nvs_flash_erase()` in any form, bare `nvs_flash_init()`, `nvs_open()`, mounting stock
LittleFS, Wi-Fi NVS restore, or any erase or write touching `0x000000..0x3EFFFF`. The
common ESP-IDF example pattern that calls `nvs_flash_erase()` after
`ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` is **forbidden**; those
errors, plus interrupted initialization, unknown schema, missing header, corrupt blob,
and wrong MAC, all lead to a visible `STORAGE ERROR` and USB recovery. **There is no
format-on-error path.**

Installation header: raw 80 bytes in its own sector at `0x3F0000`, read and validated
**before any NVS initialization** on every boot after the first. Layout exactly as
`zt_store.h` and §11 define (magic `ZTIN`, schema u16, length u16=80, MAC[6], reserved
u16, UUID[16], baseline SHA-256[32], NVS offset u32, NVS length u32, flags u32, CRC32).
On first install, `INSTALL_INIT` verifies the whole tail blank, initializes NVS in its
60 KiB region, then writes the valid header **last** as the initialization commit. A
blank tail boots into USB-only `WAIT_INSTALL` **without initializing NVS or Wi-Fi**. A
non-blank tail with no valid matching header never initializes automatically.

Records: keys within 15 characters (`cfg`, `round_a`, `round_b`, `dec_a`, `dec_b`,
`evt000`..`evt127`). Config ≤1,024 bytes, checkpoint ≤2,048 bytes. The durable event
value is exactly 64 bytes — magic[4], schema u16, length u16=64, round_id u64, wire EVENT
body[30], reserved-zero[14], CRC32 over the preceding 60 — and is **immutable**: no field
in a committed event is ever rewritten. Derived decisions live separately. Keep ≥16 KiB
free for NVS garbage collection; live serialized values below 20 KiB; key/page use below
40 KiB. Measure with NVS stats.

Persist config on explicit change, roster and round on admission, **an infection before
acknowledging it**, and server decisions before advertising them as applied. **Never**
persist beacons, RSSI updates, animation frames, or timer ticks.

Two separate NVS keys are **not** a transaction. Derive the next event sequence from the
retained checkpoint plus the maximum committed sequence during recovery. Persist the
event **before** the derived role checkpoint, and rebuild the journal index from
committed event values if a checkpoint update was interrupted.

### Game (`game.c`)

`BOOT -> NEEDS_CONFIG | RECOVERING -> LOBBY -> PREPARED -> RUNNING ->
EXPIRED_PENDING_SYNC -> FINAL`, with `STORAGE_ERROR`, `RADIO_ERROR`, and
`HOST_AUTH_ERROR` as explicit overlays. **An error never silently resets a player to
human.**

- Defer creating the 64-bit random boot nonce until Wi-Fi has started and the radio
  entropy source is available.
- Host mode requires **both** the Aux1 switch position **and** a config naming this badge
  the designated host with credentials. A misconfigured host switch shows an actionable
  error and **cannot create a second host**. Never expose a provisioning access point.
- Registration is admitted only by a server registration response.
- PREPARE freezes roster and channel; the badge assembles the full snapshot, persists it,
  and returns an application-level ready acknowledgement.
- At start, set exactly the patient-zero slot to zombie with synthetic cause sequence 0.
  START is **idempotent** and never restarts a running timer.
- **Tag transaction**, exactly as §4.4: tagger must be a running zombie with a known
  cause, unexpired deadline, clear cooldown, and an eligible direct human. Allocate
  `(boot nonce, attempt seq)`; send at 0, 250, 750 ms in fresh envelopes; stop on a
  matching response; wait at most 1,500 ms.
- Victim validates **everything before any state change**: packet validity, game and
  round, roster membership, direct source MAC, matching target, actor role and cause,
  fresh mutual proximity, running timer, own human role. A missing causal parent is
  accepted **provisionally** when the actor's fresh authenticated direct beacon advertises
  the matching zombie role and cause — request the parent and let the server adjudicate.
  **Never block an offline chain infection on an Internet lookup.**
- **`PERSISTING` lock:** the victim enters PERSISTING, allocates the next sequence, and
  commits the accepted request plus causal infection event in **one immutable value**
  before flipping role or emitting `TAG_RESULT(ACCEPTED)`. While a commit is outstanding
  the victim is locked: no second sequence, no second tag accepted. **At 500 ms without a
  definitive completion, return `PENDING` and keep the lock.** A late successful commit
  still applies the infection and sends its event and result. Only a **definitive
  failure** returns `BUSY` and releases the lock with the victim still human. Never send a
  positive acknowledgement before persistence reports success.
- Duplicate requests return the existing outcome and event reference — no second
  infection, score, write, or animation. Cache 16 recent outcomes for 10 s; durable
  records prevent duplication across reboot. Simultaneous requests are serialized by this
  task; the first durably committed wins and the loser gets `ALREADY_ZOMBIE`.
- Tagger shows success only on a matching accepted result or the victim's matching
  infection event; otherwise `UNCONFIRMED`. **A missing ACK must never undo an infection
  the victim already committed.**
- Role updates carry a server revision **and** the covered victim event-sequence frontier.
  A stale human snapshot must **not** erase a newer unsubmitted local infection: hold an
  update that does not cover it until the server decides. Refuse further event creation on
  sequence exhaustion; never wrap within a round. At 128 stored records, stop accepting
  new durable transitions and report `SYNC REQUIRED` rather than losing events.
- After 600,000 ms stop initiating and accepting tags, resolve every outstanding
  PERSISTING operation before committing and sending ROUND_CLOSED with the final produced
  frontier, and if storage never resolves, **report the failure rather than claiming a
  complete close**. Show a provisional result until the server sends a final one.

### Clock (`clock.c`)

Gameplay uses monotonic `esp_timer_get_time()` only — **never an adjustable wall clock**.
Derive the local deadline once; later valid samples may **shorten** it but never extend
it. A full reboot has no trusted elapsed-time continuity: retain role and events, show
`REJOINING`, and require a fresh same-round time exchange with a continuing peer or the
host before tagging. **A saved checkpoint alone cannot restart ten minutes.**

Reject a new tag unless `estimated_elapsed_ms + uncertainty_ms < 600000`. Events above
2,000 ms uncertainty require resynchronization. Grow uncertainty by 200 ppm of monotonic
elapsed time; a forwarded sample adds its accumulated age plus `100 ms × (hops+1)`.
Record uncertainty with every event at victim RX/validation time — storage latency never
alters occurrence time.

### Peers (`peers.c`)

Frozen roster plus bounded lobby discoveries (8, expiring after 10 s). Track ≥3 samples in
the last 1,500 ms; candidate heard within 1,000 ms; radar freshness drops after 4 s;
retained direct-sample history after 10 s. Threshold −58 dBm default, adjustable at
commissioning **before** a round; require **both** filtered and most-recent direct RSSI at
or above threshold, and the victim independently requires its own direct samples to meet
it. Choose the nearest eligible human by highest filtered RSSI, ties to the lower slot.
Three-second cooldown starts when a valid attempt is transmitted. Only fresh **direct**
observations ever affect proximity.

## Review criteria

1. There is exactly one writer of gameplay state, and no callback mutates it.
2. `nvs_flash_erase` appears nowhere; no code path can erase, format, or write any stock
   region; the only writable storage is `zt_nvs`.
3. The installation header is validated by raw read before NVS init on later boots, and a
   blank tail reaches `WAIT_INSTALL` without initializing NVS or Wi-Fi.
4. The event value is committed immutably before the role flip and before any positive
   acknowledgement; the PERSISTING lock, 500 ms `PENDING`, and definitive-failure-only
   `BUSY` behave exactly as specified.
5. The sequence counter is derived from committed evidence, not from two keys treated as
   a transaction.
6. A reboot cannot resume a round from a checkpoint alone without a fresh time exchange.
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
