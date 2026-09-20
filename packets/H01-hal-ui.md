# Packet H01 — hardware abstraction layer and user interface

## Goal

Implement `components/zt_hal/` (buttons, LCD, LEDs) and `components/zt_ui/` (screens,
font, renderer) against the frozen contracts. These are the parts a person actually sees
and touches: if the button edges are wrong the game is unplayable, and if the renderer
blocks, everything else stutters.

## Branch and worktree

- Branch: `firmware/H01-hal-ui`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/H01`

## Applicable specification

`plan.md` §2 (exact hardware configuration), §6 (screens, controls, feedback), §7
(runtime budgets and component contracts), §4.1 and §4.3 for the states and range tiers
the UI displays. `docs/hardware.md` and `zt_hw.h` carry the sanitized pin map.

## Owned file allowlist

```
firmware/components/zt_hal/CMakeLists.txt
firmware/components/zt_hal/buttons.c
firmware/components/zt_hal/lcd.c
firmware/components/zt_hal/leds.c
firmware/components/zt_ui/CMakeLists.txt
firmware/components/zt_ui/ui.c
firmware/components/zt_ui/font.c
firmware/components/zt_ui/render.c
```

Nothing else. `zt_radio`, `zt_game`, `zt_store`, `zt_gateway`, and `zt_console` are owned
by workers running right now.

## Required behavior

### Buttons (`buttons.c`)

74HC165 shift register: DATA GPIO7, LOAD GPIO20, CLK GPIO21. **Sample before each clock
pulse**, not after — the first bit is present on DATA once LOAD is released. Active-low,
logical order A, B, Home, Down, Left, Right, Up, Aux1.

- Poll at 100 Hz (every 10 ms). Require **three stable samples** (30 ms) to accept an edge.
- Publish timestamped edges into the bounded edge queue (16 entries). Coalesce directional
  repeat before losing a press or release — never drop an A press to make room.
- D-pad auto-repeat after 400 ms, then every 150 ms. **Never repeat A.**
- Start is GPIO9, active low, and is **also the ROM-download boot strap**: read it, never
  drive it, never reconfigure it as an output.
- Aux1 is a **maintained side switch**, not a momentary button: latch its position once at
  boot after debounce stabilization. If it is toggled while running, surface that as a
  visible notice that takes effect on next reboot; do not change host mode live.

### LCD (`lcd.c`)

ST7789 320×240 RGB565 on SPI2, mode 0, 40 MHz; MOSI GPIO10, SCLK GPIO1, CS GPIO2,
D/C GPIO0, reset GPIO4. Init sequence: `reset`, `init`, `invert_color(true)`,
`swap_xy(true)`, `mirror(true,false)`, display on.

- **Two 320×16 RGB565 internal DMA-capable stripe buffers, 20,480 bytes total.** Never a
  full-frame buffer and never a single buffer.
- Wait for each SPI transfer to complete before reusing that stripe. LCD queue depth two.
- Accept clipped rectangle and stripe work. Integer drawing and clipping only. **No
  per-frame heap allocation.**
- GPIO2 stays LCD CS. Never touch GPIO12–17 (flash) or GPIO18–19 (USB).

### LEDs (`leds.c`)

Six WS2812B on GPIO3 via RMT, GRB order, using `espressif/led_strip` 3.0.3's RMT backend.

- **Clamp every RGB component to 24/255** and keep total output conservative. The
  brightness menu may only *lower* the cap, never raise it. Full brightness is prohibited.
- Update at **25 Hz maximum**.
- Physical positions: 0 upper-left, 1 upper-right, 2 middle-right, 3 bottom-right,
  4 bottom-left, 5 middle-left. Threat and prey fills follow the sequence `{4,3,5,2,0,1}`.

### UI (`ui.c`, `font.c`, `render.c`)

Raw stripe renderer, dark background, large role name, readable timer, clear
connectivity. **At most 10 fps**, dirty regions where practical. Compile a small ASCII
bitmap font into flash.

Screens exactly as `plan.md` §6 specifies: Setup, Lobby, Prepared, Running radar, Peer
list, Status, End. The Running radar is a 160×180 radar on the left; role, remaining
mm:ss, nearest eligible target, range tier, direct contacts, and host/server status on
the right.

- Radar angles are **stable hashes of badge ID for layout only**. Print
  `PROXIMITY · NOT DIRECTION` under the radar. **No meters, no compass bearings, no
  distance claims.** Every radar contact must be a direct observation.
- Range tiers: ≥ −58 `IN RANGE`, ≥ −70 `CLOSE`, ≥ −82 `NEARBY`, else `FAR`.
- Controls: A tags only as a zombie in a live round, shows `STAY CLEAR` as a human, and
  registers in lobby. B toggles radar/list. D-pad scrolls; left/right change diagnostics
  page. Start toggles a status overlay. Home closes overlays; holding Home two seconds
  opens a status/settings menu with brightness and **no role reset, no round reset, no
  erase action**. No button combination changes patient zero.
- LED feedback: idle human bottom pair blue, zombie bottom pair green; human red threat
  fill and zombie yellow-green prey fill; closer tier pulses faster; tag confirmed green
  chase 600 ms; newly infected red pulses 1,200 ms; unconfirmed or missed attempt dim red
  300 ms. No immunity, medic, or cure animation exists in core.
- `GET CLOSER` and similar messages are rate-limited to one per 500 ms.
- Announcement overlay expires after at most 10 s. Infection and tag feedback outrank it.
- The Status screen shows channel, role revision, pending events, direct peers, host age,
  server age, and battery-power guidance. **It never shows a secret** — no token, key,
  SSID, password, or recovery ID.
- **Show no battery percentage.** No verified battery ADC pin exists in the evidence, so
  inventing one is prohibited.
- UI and LED are non-blocking state machines driven by the immutable snapshot from the
  game task. **No delay loops.** You never write gameplay state.

## Review criteria

1. Button sampling order, debounce, repeat rules, and the Aux1 maintained-switch
   behavior match §2 and §6 exactly.
2. Exactly two 320×16 stripe buffers totalling 20,480 bytes, with transfer completion
   awaited before reuse, and no full-frame buffer anywhere.
3. Every LED component is clamped to 24 and the cap can only be lowered.
4. No per-frame heap allocation and no blocking delay in any render or animation path.
5. No secret and no invented battery reading can reach the display.
6. No file outside the allowlist changed, and nothing was committed.
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
