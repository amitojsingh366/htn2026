# Packet D01 — DEMO MODE: an operator-started local round

## Goal

Implement `firmware/components/zt_demo/demo.c` against the frozen
`zt_contract/include/zt_demo.h`, so a proof-of-concept round can run on three badges
with no backend.

**Read `zt_demo.h` first and in full.** Its comment block is the specification: what this
is, why it exists, and the four constraints that keep it from being mistaken for server
authority. Do not weaken any of them.

## Base commit and dependencies

- Base commit: `d5e0f431f3cc596129ee71b74351b10ed9ed75b4` on `firmware/integration`.
- H01, M01 and G01 are merged. `zt_game`, `zt_radio`, `zt_hal` and `zt_ui` are real
  implementations, not scaffolds. Contract amendments 1–4 are in the frozen headers.
- Branch: `firmware/D01-demo-round`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/D01`

## Owned file allowlist

```
firmware/components/zt_demo/CMakeLists.txt
firmware/components/zt_demo/demo.c
```

Nothing else. Not `zt_game`, not `zt_radio`, not `zt_console`, not `main/app_main.c`, not
any header, not any document, not `tools/`. The orchestrator wires `app_main`; packet C01
owns the console parser that will deliver your request. A genuine contract problem is
`BLOCKED_CONTRACT`, not permission to edit someone else's file.

## Read before writing code, in your worktree

1. `components/zt_contract/include/zt_demo.h` — your contract.
2. `components/zt_contract/include/zt_game.h` — `zt_server_snapshot_t`,
   `zt_server_command_t`, `zt_ui_snapshot_t`, `zt_game_post_snapshot()`,
   `zt_game_post_command()`, `zt_game_snapshot()`.
3. `components/zt_contract/include/zt_wire.h` — `zt_wire_roster_entry_t`,
   `zt_wire_role_entry_t`, `zt_wire_command_t`, `zt_wire_prepare_round_args_t`, the
   command type enum, and `zt_wire_roster_hash()`.
4. **`components/zt_game/game.c`, read-only.** This matters more than anything else in
   this packet. G01's implementation is merged and authoritative, and it validates every
   posted snapshot and command. Find `zt_game_post_snapshot`, `zt_game_post_command` and
   the functions that apply them, and construct values that pass that real validation —
   page counts, entry counts, `snapshot_id`/`state_rev`, `roster_hash`, slot ranges,
   `command_seq`, role revisions, phase and timing fields. Do not guess and do not
   "improve" the game's rules to fit your output. If something the game requires cannot
   be synthesized honestly, that is `BLOCKED_CONTRACT`.
5. `plan.md` §4.1 steps 6–9 (PREPARE freezes the roster, START sets exactly the patient
   zero slot to zombie with synthetic cause sequence 0, start is scheduled ahead and
   start commands are idempotent), §4.2 (the deadline may be shortened, never extended).

## Required behavior

### `zt_demo_start(const zt_demo_round_t *round)`

Called from the console sink, in the game task's context. It must:

1. Validate: nonzero `round_id`; `player_count` between `ZT_DEMO_MIN_PLAYERS` and
   `ZT_MAX_PLAYERS`; no duplicate or all-zero MAC; `patient_zero_slot < player_count`;
   `channel` in 1..11. Clamp `start_delay_ms` to the header's MIN/MAX and substitute
   `ZT_DEMO_DEFAULT_DURATION_MS` for a zero duration. Malformed is `ZT_ERR_INVALID_ARG`.
2. Locate this badge's own factory MAC in `players[]`; absent is `ZT_ERR_NOT_FOUND`. The
   index is this badge's slot. Never assign yourself a slot that is not in the list.
3. Refuse, with `ZT_ERR_INVALID_STATE`, unless `zt_game_snapshot()` reports admission
   `ZT_ADMISSION_LOBBY` or `ZT_ADMISSION_NEXT_ROUND` **and** no retained round
   (`round_id == 0`) **and** no storage error **and** no demo already active. A real
   round, even an unfinalized previous one, always wins.
4. Synthesize the server's output and post it through the existing entry points:
   - roster/role snapshot pages via `zt_game_post_snapshot()`, at most 8 entries per
     page, zero-based `page_index`, consistent `page_count`, one `snapshot_id` for the
     assembly, and a `roster_hash` computed with `zt_wire_roster_hash()` over the
     canonical entries — never a hash you invented;
   - then `ZT_CMD_PREPARE_ROUND` with the rules (duration, tag RSSI, cooldown) taken from
     the contract defaults already in the headers;
   - then `ZT_CMD_START_ROUND` scheduled `start_delay_ms` ahead, naming
     `patient_zero_slot`, exactly once.
   Every badge given the same `zt_demo_round_t` must derive byte-identical roster
   entries, slots, hash, patient zero and deadline. Anything order-dependent, anything
   derived from discovery, anything randomized locally, and anything derived from the
   local clock's absolute value breaks that and is a defect.
5. Player names: derive deterministically from the slot (for example `P00`..`P19`) so
   every badge agrees. Do not read the local configured badge name into another badge's
   roster entry, and do not leave a name empty if the game requires one.

### `zt_demo_service(uint64_t now_us)`

- Delivers the staged snapshot pages and commands in order, spread across service calls
  rather than in one burst, retrying a post that returned `ZT_ERR_BUSY`. Bounded work per
  call, no blocking, no delay loop.
- Re-raises the banner: call `zt_ui_announce()` with `ZT_DEMO_BANNER` often enough that
  it never lapses while the demo is active (the announcement carries an expiry; refresh
  it before it expires). The operator must never see a demo round without the banner.
- Stops advancing once the game reports the round running; it does not re-post START, and
  it never re-issues a command the game already applied.

### `zt_demo_active(uint8_t *out)`

Nonzero while a demo round is active. `ZT_ERR_INVALID_ARG` on NULL. Active means: started
and not yet past its deadline or finalized. Cheap, no blocking, callable from any task.

### Channel

The round must run on `round.channel`. Check whether `game.c` already calls
`zt_channel_lock()` when it applies PREPARE. If it does, do nothing. If it does not, call
`zt_channel_lock(round_id, channel)` yourself at the right moment and say so in your
report. Do not start a scan, do not change channel during a running round, and do not
touch any other radio function.

## Excluded

No healing, no code stations, no sonar, no NFC, no positioning, no AI. No new wire packet,
no new console operation, no new NVS key, no persistence of anything (a demo round's
staging state is RAM-only). No relaxation of tag range, cooldown, causality, the
PERSISTING lock or the deadline. No way to start a demo from a button, a packet, a timer
or a boot path. No second host. No writing durable storage. No heap allocation after
init. No tests, no test harness, no mock, no CI.

## Allowed checks

Per-file compilation with the installed cross compiler, as other packets did. **Do not run
`idf.py build`**: the sandbox blocks it with a psutil `sysctl()` PermissionError, which is
expected, is not your problem, and is not a `BLOCKED_ENVIRONMENT` for this packet. The
orchestrator runs the authoritative build at review.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only the two allowlisted files exist or changed.
2. Two badges given the same `zt_demo_round_t` derive identical roster bytes, slots,
   roster hash, patient zero and deadline. No local randomness, no discovery input, no
   ordering dependence.
3. Every refusal in `zt_demo.h` is enforced, especially: a real or retained round always
   refuses, and nothing but the console can start a demo.
4. Snapshots and commands go through `zt_game_post_snapshot()` / `zt_game_post_command()`
   and pass `game.c`'s real validation. No gameplay rule is bypassed or relaxed.
5. The banner cannot lapse while a demo is active.
6. Bounded, non-blocking service; no persistence; no allocation in the hot path.

## Final report

Standard format. Under "Integration notes" state exactly: what `app_main` must call and
from which task; whether you call `zt_channel_lock()` or the game already does; and the
precise field values an operator must pass so that three badges agree.
