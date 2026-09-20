# Packet G02 — host-side origination: admission and command distribution over the mesh

## Goal

Implement the missing host half of `components/zt_game/game.c`: when this badge is the
designated host, it must actually transmit what every other badge needs in order to be
admitted to a round and to learn the round's state.

Packet D01 discovered this while implementing DEMO MODE, and it is not a demo problem. It
is a real gap in the product: G01 implemented the complete **receive** side — a player
correctly consumes `ZT_PKT_JOIN_RESULT`, `ZT_PKT_ROSTER_PAGE`, `ZT_PKT_HOST_STATE` and
`ZT_PKT_COMMAND` from the host, and validates that they came from the host — but nothing
in the firmware ever **sends** them. A player therefore broadcasts `ZT_PKT_JOIN` and
waits forever, no badge is ever registered, and no round can start with or without a
backend.

## Base commit and dependencies

- Base commit: `1282231ca770e6575e8d47c5ee564cb1c7f8e437` on `firmware/integration`.
- Branch: `firmware/G02-host-origination`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/G02`
- H01, M01, G01 and M01's follow-up are merged. You are extending your own merged work:
  this is the same session and the same module you wrote.

## Owned file allowlist

```
firmware/components/zt_game/game.c
```

Only that file. Not `clock.c`, not `peers.c`, not `zt_store`, not `zt_radio`, not any
header, not `app_main.c`, not a document. If you believe a header must change, that is
`BLOCKED_CONTRACT` — but read the whole of `zt_wire.h` first, because every packet type
and every payload struct you need already exists and is already encodable.

## Read first, in your worktree

`plan.md` §12, especially the host paragraph: "Commands retry every 2 seconds until
actual target receipt, with per-round idempotent command IDs; latest critical state
remains available by snapshot. Frozen roster pages are sent during preparation and on
explicit rejoin request, not perpetually each second." Also §4.1 steps 5–8 (registration,
PREPARE freezing the roster, the ready bitmap, START setting exactly the patient-zero
slot), and your own receive-side handlers for each packet type in `game.c`.

## Required behavior

Everything below happens **only when this badge is the host**. A non-host badge's
behavior must not change in any way. Decide host-ness the way the rest of the firmware
already does; do not invent a second notion of host, and never let two badges both act as
host on the same game.

### 1. Answer JOIN

A player broadcasts `ZT_PKT_JOIN`. The host must reply with `ZT_PKT_JOIN_RESULT` to that
badge, carrying the admission outcome your receive side already knows how to parse.

- Admission itself remains **server-owned** in the product: the host does not invent a
  roster slot for an unknown badge out of nothing. What the host owns is **distribution**
  — answering from the roster it holds. A badge in the frozen roster gets its existing
  slot and current role, which is the rejoin path of §4.1 and the one that matters for a
  badge that rebooted mid-round.
- A badge that is not in the roster gets an explicit negative result, not silence. It
  must end up showing `NEXT ROUND` rather than waiting forever.
- Idempotent: repeating a JOIN returns the same answer and never allocates anything twice.
- Rate-limit replies. A malformed or flooding JOIN must not create a response storm.

### 2. Distribute the frozen roster

Send `ZT_PKT_ROSTER_PAGE` covering the frozen roster during preparation, and on an
explicit rejoin or snapshot request — **not** perpetually every second. Page the roster
the way the receive side expects, binding pages by round and digest, and never mixing
pages from different rounds or revisions.

### 3. Distribute commands

When the host applies a server command that targets the round (PREPARE_ROUND,
START_ROUND, ROLE_SET, ANNOUNCE, END_ROUND, FINAL_RESULT, CANCEL_PREPARE), it must
transmit `ZT_PKT_COMMAND` so the other badges receive it.

- Retry every 2 seconds **until that specific target's receipt arrives**, using the
  per-round idempotent command ID. Your receive side already emits
  `ZT_PKT_COMMAND_RECEIPT`; consume it to stop retrying.
- Bounded: at most the eight pending commands the contract already allows, bounded
  retries, and no unbounded growth if a badge never answers. A badge that never
  acknowledges is a reported condition, not a memory leak.
- Host receipt must reflect **each badge's applied state**, never merely the host's own
  successful send. Do not mark a command delivered because the radio accepted it.

### 4. HOST_STATE

Keep emitting the clock/phase metadata the mesh already re-sends from its cached
template, and make sure a host actually submits it once so the mesh has a template. Per
§12 this carries `page_count=0`; it is clock and phase metadata only and must never clear
a roster or a role.

## Explicitly not in this packet

No backend, no HTTPS, no WebSocket, no gateway task. No new packet type, no new command
type, no new NVS key, no header change. No change to tag rules, proximity, causality, the
PERSISTING lock, the deadline, or any receive-side validation. No second host. No demo
logic — packet D01 owns that and will call into this through the ordinary command path.

## Allowed checks

Per-file compilation with the installed cross compiler, as you did before. **Do not run
`idf.py build`**: the sandbox blocks it with the psutil `sysctl()` PermissionError, which
is expected and is not a `BLOCKED_ENVIRONMENT` for this packet. The orchestrator runs the
authoritative build at review.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave the work uncommitted in your worktree.**

## Review criteria

1. Only `game.c` changed.
2. A non-host badge's behavior is unchanged.
3. JOIN is answered, idempotently and rate-limited, including an explicit negative for a
   badge outside the frozen roster.
4. Commands retry per target until that target's receipt, bounded, idempotent by command
   ID, and never marked delivered on send success alone.
5. Roster pages are sent at preparation and on request, not continuously.
6. No new authority: the host distributes what it holds and does not invent admission.

## Final report

Standard format. Under "Integration notes", state exactly what a host badge now sends,
when, and what stops each retry; and state plainly whether a badge that has never been in
a roster can now be admitted by a host alone, because packet D01's design depends on the
answer.
