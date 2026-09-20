# Packet G03 — admission from an authoritative snapshot

## Goal

Let a badge be admitted into a round by an authoritative roster snapshot that contains
its own factory MAC, when it is not already registered.

## Why this is a product gap, not a demo convenience

Packet D01 hit it first, but it is not demo-specific. Today `zt_game_post_snapshot()`
assembles pages and `PREPARE` requires an already-registered badge with a matching self
slot, so there is no path from "this badge is in the server's roster" to "this badge is
registered". Trace either route and it dead-ends in the same place:

- **With a real backend**, the host registers over HTTPS, the server replies with the
  badge's slot, and the gateway hands the firmware a snapshot. The host badge is an
  ordinary player (decision D02) and must be registered like everyone else. If a snapshot
  naming it cannot register it, packet N01's gateway will hit this exact wall.
- **Over the mesh**, G02 now answers JOIN with the badge's *existing* roster slot, which
  is correct and deliberately does not invent admission. But the first badge to hold the
  roster — the host — has no one to JOIN to.

So the missing step is: an authoritative roster that names me makes me registered.

## Base commit and dependencies

- Base commit: `5561d7fa2addb8f0bbb916edfe15c6bc8d92d5ea` on `firmware/integration`.
- Branch: `firmware/G03-snapshot-admission`
- Worktree: `/Users/amitojsingh/Desktop/misc/hackerbadge/htn2026/.worktrees/G03`
- G02 is merged. This continues your own module.

## Owned file allowlist

```
firmware/components/zt_game/game.c
```

Only that. No header, no other component, no `app_main.c`, no document, no `tools/`.

## Required behavior

When a **complete, self-consistent** snapshot assembly finishes and this badge is not
registered for that round, and the assembled roster contains an entry whose MAC equals
this badge's factory MAC:

1. Adopt that entry's slot as `self_slot`, take the round ID, roster, roster hash,
   revision, phase, channel, patient-zero slot and rules from the assembly, and become
   registered for that round.
2. Persist the resulting checkpoint before acting on it, exactly as the existing
   admission paths do. Registration that does not survive a reboot is not registration.
3. Take the role the snapshot gives this badge, with its revision and cause. Do not
   invent a role and do not default to human if the snapshot says otherwise.
4. Then let the ordinary PREPARE and START handling proceed unchanged.

### The refusals matter more than the feature

- Only a **complete** assembly admits: every page present, one `snapshot_id`, one
  revision, a `roster_hash` that **recomputes and matches**. A partial, mixed-revision or
  hash-mismatched assembly admits nothing. Never merge pages across rounds or revisions.
- Only when **not already registered** for that round. A badge already holding a slot
  keeps it; a snapshot must never silently move a registered badge to a different slot,
  and must never renumber a frozen roster mid-round.
- Never admit from a snapshot that does not contain this badge's own MAC. Absence means
  not in this round — show `NEXT ROUND` — it does not mean "pick a free slot".
- A snapshot arriving over the **mesh** is only authoritative from the host, which your
  receive side already validates. Do not relax that check, and do not admit from a
  relayed or unauthenticated page.
- An undecided prior round still refuses a new PREPARE, per plan §4.5. This packet does
  not create an exception to that.
- Do not touch the server-authority model anywhere else: this is a badge accepting an
  authoritative statement about itself, not a badge deciding its own membership.

## Excluded

No new API, no header change, no new packet or command type, no NVS key. No change to
tag rules, proximity, causality, the PERSISTING lock, the deadline, event handling or any
existing validation. No demo logic. No backend or gateway work.

## Allowed checks

Per-file compilation with the installed cross compiler as before. **Do not run
`idf.py build`** — the sandbox blocks it with the psutil `sysctl()` PermissionError and
that is expected, not a `BLOCKED_ENVIRONMENT`. The orchestrator runs the authoritative
build.

**No hardware. No serial ports. No credentials. No Git commit, add, push, rebase or
reset — leave your work uncommitted in your worktree.**

## Review criteria

1. Only `game.c` changed.
2. A complete, hash-verified, single-revision snapshot naming this badge registers it and
   persists the checkpoint before acting.
3. Partial, mixed-revision, hash-mismatched, non-host and self-absent snapshots all admit
   nothing.
4. An already-registered badge is never moved, renumbered or re-admitted.
5. Roles come from the snapshot, never invented.

## Final report

Standard format. Under "Integration notes", state exactly what now registers a badge, in
order, and confirm what still refuses. Say plainly whether a host badge can now be
admitted into a round it holds the roster for.
